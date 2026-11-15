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

#include <cstring>

#include <obs.h>

#include "address.hpp"

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

	obs_enum_sources(each, &want);
	int clicked = 0;
	for (obs_source_t *source : want.found) {
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
	return {{"ok", true}, {"clicked", clicked}};
}

} // namespace direbot::obs
