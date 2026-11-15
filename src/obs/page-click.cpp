/*
DireBot for OBS
Copyright (C) 2026 DireBot

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/


#include "page-click.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>

#include <obs.h>
#include <graphics/matrix4.h>

#include "address.hpp"
#include "ui-call.hpp"

namespace direbot::obs {

namespace {

// More than this many copies of one page is nobody's set-up.
constexpr size_t MOST_SOURCES = 8;

struct Wanted {
	int port = 0;
	std::string path;
	std::vector<obs_source_t *> found; // each with a reference held
};

bool same_page(obs_source_t *source, const Wanted &want)
{
	const char *id = obs_source_get_unversioned_id(source);
	if (!id || std::strcmp(id, "browser_source") != 0)
		return false;
	obs_data_t *s = obs_source_get_settings(source);
	if (!s)
		return false;
	bool same = false;
	if (!obs_data_get_bool(s, "is_local_file")) {
		const char *raw = obs_data_get_string(s, "url");
		const std::string url = raw ? raw : "";
		int port = 0;
		same = local_address(url, port) && port == want.port && address_path(url) == want.path;
	}
	obs_data_release(s);
	return same;
}

bool each(void *param, obs_source_t *source)
{
	auto *want = static_cast<Wanted *>(param);
	if (want->found.size() < MOST_SOURCES && same_page(source, *want)) {
		obs_source_t *held = obs_source_get_ref(source);
		if (held)
			want->found.push_back(held);
	}
	return true;
}

json refused(const char *why)
{
	return {{"ok", false}, {"error", why}};
}

// How big a source is drawn on the stream: the program's scene, and the
// scenes and groups inside it, looked through for it. The biggest it's
// drawn at, as a scale of its own size (0 when it isn't in there).
struct Look {
	obs_source_t *target = nullptr;
	double sx = 0, sy = 0;
};

constexpr int DEEPEST = 6;

void look_in(obs_scene_t *scene, double sx, double sy, Look &look, int depth);

struct LookHere {
	Look *look;
	double sx, sy;
	int depth;
};

bool look_item(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *here = static_cast<LookHere *>(param);
	if (!obs_sceneitem_visible(item))
		return true;
	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source)
		return true;
	// Each pixel of the item's own picture, as big as it's drawn (bounds,
	// crop and rotation included), in the scene around it.
	struct matrix4 m;
	obs_sceneitem_get_draw_transform(item, &m);
	const double sx = std::hypot((double)m.x.x, (double)m.x.y) * here->sx;
	const double sy = std::hypot((double)m.y.x, (double)m.y.y) * here->sy;
	if (source == here->look->target) {
		if (sx * sy > here->look->sx * here->look->sy) {
			here->look->sx = sx;
			here->look->sy = sy;
		}
		return true;
	}
	if (here->depth >= DEEPEST)
		return true;
	obs_scene_t *inner = obs_sceneitem_is_group(item) ? obs_sceneitem_group_get_scene(item)
							  : obs_scene_from_source(source);
	if (inner)
		look_in(inner, sx, sy, *here->look, here->depth + 1);
	return true;
}

void look_in(obs_scene_t *scene, double sx, double sy, Look &look, int depth)
{
	LookHere here{&look, sx, sy, depth};
	obs_scene_enum_items(scene, look_item, &here);
}

// The scene on the stream now (the one a transition is going to).
obs_source_t *program_scene()
{
	obs_source_t *out = obs_get_output_source(0);
	if (!out)
		return nullptr;
	if (obs_source_get_type(out) == OBS_SOURCE_TYPE_TRANSITION) {
		obs_source_t *active = obs_transition_get_active_source(out);
		obs_source_release(out);
		return active;
	}
	return out;
}

// How big the source is drawn on the stream: (width, height) in pixels of
// OBS's canvas. Shown some other way than in the program's scenes (a
// downstream keyer): taken as its own size.
void drawn_size(obs_source_t *source, double &w, double &h)
{
	Look look;
	look.target = source;
	obs_source_t *program = program_scene();
	if (program) {
		obs_scene_t *scene = obs_scene_from_source(program);
		if (scene)
			look_in(scene, 1.0, 1.0, look, 0);
		obs_source_release(program);
	}
	if (look.sx <= 0 || look.sy <= 0)
		look.sx = look.sy = 1.0;
	w = look.sx * obs_source_get_width(source);
	h = look.sy * obs_source_get_height(source);
}

bool part(const json &data, const char *key, double &out)
{
	if (!data.contains(key))
		return true;
	if (!data[key].is_number())
		return false;
	out = data[key].get<double>();
	return std::isfinite(out);
}

// The clicks themselves, on OBS's UI thread.
json click_pages(Wanted &want, double x, double y, double w, double h, double min_w, double min_h, bool sized)
{
	obs_enum_sources(each, &want);
	int clicked = 0, hidden = 0, small = 0;
	for (obs_source_t *source : want.found) {
		// Only a page on the stream now: a player nobody can see is never
		// started (nor one drawn smaller than its site allows).
		if (!obs_source_active(source)) {
			hidden++;
			obs_source_release(source);
			continue;
		}
		if (sized) {
			double dw = 0, dh = 0;
			drawn_size(source, dw, dh);
			if (!player_big_enough(dw, dh, w, h, min_w, min_h)) {
				small++;
				obs_source_release(source);
				continue;
			}
		}
		int px = 0, py = 0;
		if (page_spot(x, y, obs_source_get_width(source), obs_source_get_height(source), px, py)) {
			// As OBS's Interact window does it: the page gets the focus,
			// the mouse moves there, and the button goes down and up.
			struct obs_mouse_event e = {};
			e.modifiers = 0;
			e.x = px;
			e.y = py;
			obs_source_send_focus(source, true);
			obs_source_send_mouse_move(source, &e, false);
			obs_source_send_mouse_click(source, &e, MOUSE_LEFT, false, 1);
			obs_source_send_mouse_click(source, &e, MOUSE_LEFT, true, 1);
			clicked++;
		}
		obs_source_release(source);
	}
	return {{"ok", true}, {"clicked", clicked}, {"hidden", hidden}, {"small", small}};
}

} // namespace

const std::vector<std::string> &page_click_requests()
{
	static const std::vector<std::string> names = {"ClickPage"};
	return names;
}

bool page_click_handles(const std::string &type)
{
	return type == "ClickPage";
}

json page_click(const std::string &type, const json &data)
{
	if (type != "ClickPage")
		return refused("Not a request this knows.");
	if (!data.is_object() || !data.contains("port") || !data["port"].is_number_integer() ||
	    !data.contains("path") || !data["path"].is_string() || !data.contains("x") || !data["x"].is_number() ||
	    !data.contains("y") || !data["y"].is_number())
		return refused("ClickPage needs port, path, x and y.");
	Wanted want;
	want.port = data["port"].get<int>();
	want.path = data["path"].get<std::string>();
	const double x = data["x"].get<double>(), y = data["y"].get<double>();
	if (want.port <= 0 || want.port > 65535)
		return refused("That isn't a port.");
	if (want.path.empty() || want.path[0] != '/' || want.path.size() > 200 ||
	    want.path.find_first_of("?#") != std::string::npos)
		return refused("That isn't a page's path.");
	if (!(x >= 0 && x <= 1 && y >= 0 && y <= 1))
		return refused("The spot has to be on the page (0 to 1 across and down).");
	// The player's size (parts of the page) and the smallest it may be
	// started at, in pixels on the stream: optional, both or neither.
	double w = 0, h = 0, min_w = 0, min_h = 0;
	if (!part(data, "w", w) || !part(data, "h", h) || !part(data, "min_w", min_w) || !part(data, "min_h", min_h))
		return refused("w, h, min_w and min_h are numbers.");
	if (w < 0 || w > 1 || h < 0 || h > 1 || min_w < 0 || min_h < 0 || min_w > 10000 || min_h > 10000)
		return refused("The player's size has to be part of the page (0 to 1), its smallest 0 to 10000.");
	const bool sized = min_w > 0 || min_h > 0;
	if (sized && (w <= 0 || h <= 0))
		return refused("A smallest size needs the player's size (w and h).");

	// On OBS's UI thread, as its Interact window does. (Sent straight
	// from obs-websocket's thread, in the real-OBS test on Linux, the
	// click never reached the page and OBS went away.)
	static const std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
	return ui_call(alive, [want, x, y, w, h, min_w, min_h, sized]() mutable -> json {
		return click_pages(want, x, y, w, h, min_w, min_h, sized);
	});
}

} // namespace direbot::obs
