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

#include "canvas.hpp"

#include <algorithm>
#include <cstring>

#include <obs-frontend-api.h>

#include <plugin-support.h>

#include "rules.hpp"

namespace direbot::vertical {

namespace {

constexpr uint32_t DEFAULT_FADE_MS = 300;

obs_canvas_t *find_canvas(const std::string &uuid)
{
	// The frontend's own list: the canvases it keeps in the collection.
	obs_frontend_canvas_list list = {};
	obs_frontend_get_canvases(&list);
	obs_canvas_t *found = nullptr, *by_name = nullptr;
	for (size_t i = 0; i < list.canvases.num; i++) {
		obs_canvas_t *c = list.canvases.array[i];
		if (obs_canvas_removed(c))
			continue;
		if (!uuid.empty() && uuid == obs_canvas_get_uuid(c))
			found = c;
		else if (!by_name && strcmp(obs_canvas_get_name(c), rules::CANVAS_NAME) == 0)
			by_name = c;
	}
	obs_canvas_t *pick = found ? found : by_name;
	pick = pick ? obs_canvas_get_ref(pick) : nullptr;
	obs_frontend_canvas_list_free(&list);
	return pick;
}

struct Collect {
	std::vector<SceneRow> rows;
};

bool collect_scene(void *param, obs_source_t *source)
{
	auto *c = static_cast<Collect *>(param);
	if (obs_source_is_scene(source) && !obs_source_removed(source))
		c->rows.push_back({obs_source_get_name(source), obs_source_get_uuid(source)});
	return true;
}

} // namespace

Canvas::~Canvas()
{
	release();
}

bool Canvas::video_info(uint32_t width, uint32_t height, obs_video_info &ovi) const
{
	// OBS's own frame rate, colours and scaling; the tall size.
	if (!obs_get_video_info(&ovi))
		return false;
	ovi.base_width = width;
	ovi.base_height = height;
	ovi.output_width = width;
	ovi.output_height = height;
	return true;
}

void Canvas::connect(obs_canvas_t *canvas)
{
	signal_handler_t *sh = obs_canvas_get_signal_handler(canvas);
	signal_handler_connect(sh, "remove", &Canvas::removed, this);
	signal_handler_connect(sh, "source_rename", &Canvas::scene_renamed, this);
}

bool Canvas::attach(const std::string &uuid, uint32_t width, uint32_t height, const std::string &scene)
{
	release();
	obs_canvas_t *canvas = find_canvas(uuid);
	if (!canvas)
		return false;
	canvas_ = canvas;
	connect(canvas_);
	width_ = rules::size_ok(width, height) ? width : rules::DEFAULT_WIDTH;
	height_ = rules::size_ok(width, height) ? height : rules::DEFAULT_HEIGHT;
	make_transition();
	ensure_video();
	// Back on the scene it was on, else its first.
	std::vector<std::string> names = scene_names();
	std::string pick = scene;
	if (std::find(names.begin(), names.end(), pick) == names.end())
		pick = names.empty() ? std::string() : names.front();
	if (!pick.empty())
		switch_to(pick, false);
	obs_log(LOG_INFO, "Vertical: found the tall picture (%ux%u, %zu scene%s).", width_, height_, names.size(),
		names.size() == 1 ? "" : "s");
	return true;
}

bool Canvas::create(uint32_t width, uint32_t height, std::string &problem)
{
	if (canvas_)
		return true;
	if (!rules::size_ok(width, height)) {
		problem = "That isn't a size the vertical picture can be.";
		return false;
	}
	obs_video_info ovi = {};
	if (!video_info(width, height, ovi)) {
		problem = "OBS's video isn't running.";
		return false;
	}
	// Video now only when OBS isn't sending or recording; else the first
	// chance after (ensure_video).
	const bool video_now = !obs_video_active();
	obs_canvas_t *canvas = obs_frontend_add_canvas(rules::CANVAS_NAME, video_now ? &ovi : nullptr, PROGRAM);
	if (!canvas) {
		problem = "OBS didn't make the vertical picture.";
		return false;
	}
	canvas_ = canvas;
	connect(canvas_);
	width_ = width;
	height_ = height;
	make_transition();
	if (!video_now)
		ensure_video();
	// The canvas keeps its own hold on its scenes (SCENE_REF).
	obs_scene_release(obs_canvas_scene_create(canvas_, rules::FIRST_SCENE));
	switch_to(rules::FIRST_SCENE, false);
	obs_log(LOG_INFO, "Vertical: made the tall picture (%ux%u).", width_, height_);
	return true;
}

void Canvas::make_transition()
{
	if (transition_)
		return;
	transition_ = obs_source_create_private("fade_transition", "DireBot Vertical fade", nullptr);
	if (!transition_)
		transition_ = obs_source_create_private("cut_transition", "DireBot Vertical cut", nullptr);
	if (transition_) {
		obs_transition_set_size(transition_, width_, height_);
		obs_transition_set_alignment(transition_, OBS_ALIGN_CENTER);
		obs_transition_set_scale_type(transition_, OBS_TRANSITION_SCALE_ASPECT);
	}
}

void Canvas::release()
{
	if (canvas_) {
		signal_handler_t *sh = obs_canvas_get_signal_handler(canvas_);
		signal_handler_disconnect(sh, "remove", &Canvas::removed, this);
		signal_handler_disconnect(sh, "source_rename", &Canvas::scene_renamed, this);
		if (awake_ && !obs_canvas_removed(canvas_))
			obs_canvas_set_channel(canvas_, 0, nullptr);
	}
	awake_ = false;
	if (transition_) {
		obs_transition_clear(transition_);
		obs_source_release(transition_);
		transition_ = nullptr;
	}
	obs_canvas_release(canvas_);
	canvas_ = nullptr;
	set_current("");
}

video_t *Canvas::video() const
{
	return canvas_ ? obs_canvas_get_video(canvas_) : nullptr;
}

bool Canvas::has_video() const
{
	return canvas_ && obs_canvas_has_video(canvas_);
}

std::string Canvas::uuid() const
{
	return canvas_ ? obs_canvas_get_uuid(canvas_) : "";
}

double Canvas::fps() const
{
	obs_video_info ovi = {};
	if (!canvas_ || !obs_canvas_get_video_info(canvas_, &ovi) || !ovi.fps_den)
		return 0.0;
	return (double)ovi.fps_num / (double)ovi.fps_den;
}

bool Canvas::ensure_video()
{
	if (!canvas_ || obs_canvas_removed(canvas_))
		return false;
	if (obs_canvas_has_video(canvas_)) {
		obs_video_info ovi = {};
		if (obs_canvas_get_video_info(canvas_, &ovi) && ovi.base_width == width_ && ovi.base_height == height_)
			return true;
	}
	obs_video_info ovi = {};
	if (!video_info(width_, height_, ovi))
		return false;
	// OBS refuses while anything sends or records.
	if (obs_video_active())
		return obs_canvas_has_video(canvas_);
	if (!obs_canvas_reset_video(canvas_, &ovi)) {
		obs_log(LOG_WARNING, "Vertical: OBS couldn't give the tall picture its video (%ux%u).", width_,
			height_);
		return false;
	}
	return true;
}

bool Canvas::resize(uint32_t width, uint32_t height, std::string &problem)
{
	if (!canvas_) {
		problem = "There's no vertical picture yet.";
		return false;
	}
	if (!rules::size_ok(width, height)) {
		problem = "That isn't a size the vertical picture can be.";
		return false;
	}
	if (width == width_ && height == height_ && obs_canvas_has_video(canvas_))
		return true;
	if (obs_video_active()) {
		problem = "OBS is sending or recording - the size can change once it stops.";
		return false;
	}
	const uint32_t old_w = width_, old_h = height_;
	width_ = width;
	height_ = height;
	if (!ensure_video()) {
		width_ = old_w;
		height_ = old_h;
		ensure_video();
		problem = "OBS couldn't change the vertical picture's size.";
		return false;
	}
	if (transition_)
		obs_transition_set_size(transition_, width_, height_);
	obs_log(LOG_INFO, "Vertical: the tall picture is now %ux%u.", width_, height_);
	return true;
}

void Canvas::main_video_changed()
{
	if (!canvas_ || obs_canvas_removed(canvas_) || obs_video_active())
		return;
	obs_video_info ovi = {};
	if (!video_info(width_, height_, ovi))
		return;
	obs_canvas_reset_video(canvas_, &ovi);
}

void Canvas::set_awake(bool awake)
{
	if (!canvas_ || obs_canvas_removed(canvas_))
		awake = false;
	if (awake == awake_)
		return;
	awake_ = awake;
	if (canvas_ && !obs_canvas_removed(canvas_))
		obs_canvas_set_channel(canvas_, 0, awake ? transition_ : nullptr);
}

std::vector<SceneRow> Canvas::scenes() const
{
	Collect c;
	if (canvas_)
		obs_canvas_enum_scenes(canvas_, collect_scene, &c);
	return c.rows;
}

std::vector<std::string> Canvas::scene_names() const
{
	std::vector<std::string> out;
	for (const SceneRow &row : scenes())
		out.push_back(row.name);
	return out;
}

obs_source_t *Canvas::scene_source(const std::string &name) const
{
	if (!canvas_ || name.empty())
		return nullptr;
	obs_source_t *s = obs_canvas_get_source_by_name(canvas_, name.c_str());
	if (s && !obs_source_is_scene(s)) {
		obs_source_release(s);
		return nullptr;
	}
	return s;
}

bool Canvas::switch_to(const std::string &name, bool animate)
{
	obs_source_t *scene = scene_source(name);
	if (!scene)
		return false;
	if (transition_) {
		if (animate && awake_ && name != current()) {
			uint32_t ms = (uint32_t)obs_frontend_get_transition_duration();
			obs_transition_start(transition_, OBS_TRANSITION_MODE_AUTO, ms ? ms : DEFAULT_FADE_MS, scene);
		} else {
			obs_transition_set(transition_, scene);
		}
	}
	set_current(name);
	obs_source_release(scene);
	return true;
}

SceneRow Canvas::create_scene(const std::string &name, std::string &problem)
{
	if (!canvas_) {
		problem = "There's no vertical picture yet.";
		return {};
	}
	std::string base = name.empty() ? std::string(rules::FIRST_SCENE) : name;
	std::string pick = base;
	for (int i = 2; i < 1000; i++) {
		obs_source_t *have = obs_canvas_get_source_by_name(canvas_, pick.c_str());
		if (!have)
			break;
		obs_source_release(have);
		pick = base + " " + std::to_string(i);
	}
	obs_scene_t *scene = obs_canvas_scene_create(canvas_, pick.c_str());
	if (!scene) {
		problem = "OBS didn't make the scene.";
		return {};
	}
	obs_source_t *source = obs_scene_get_source(scene);
	SceneRow row{obs_source_get_name(source), obs_source_get_uuid(source)};
	obs_scene_release(scene); // the canvas holds it
	// The first scene in an empty picture goes on.
	if (current().empty())
		switch_to(row.name, false);
	return row;
}

bool Canvas::remove_scene(const std::string &name, std::string &problem)
{
	obs_source_t *scene = scene_source(name);
	if (!scene) {
		problem = "There's no vertical scene called " + name + ".";
		return false;
	}
	std::vector<std::string> names = scene_names();
	if (names.size() <= 1) {
		obs_source_release(scene);
		problem = "The vertical picture keeps at least one scene.";
		return false;
	}
	if (name == current()) {
		for (const std::string &other : names)
			if (other != name) {
				switch_to(other, false);
				break;
			}
	}
	obs_source_remove(scene);
	obs_source_release(scene);
	return true;
}

bool Canvas::rename_scene(const std::string &name, const std::string &to, std::string &problem)
{
	obs_source_t *scene = scene_source(name);
	if (!scene) {
		problem = "There's no vertical scene called " + name + ".";
		return false;
	}
	if (to.empty()) {
		obs_source_release(scene);
		problem = "A scene needs a name.";
		return false;
	}
	obs_source_t *taken = obs_canvas_get_source_by_name(canvas_, to.c_str());
	if (taken && taken != scene) {
		obs_source_release(taken);
		obs_source_release(scene);
		problem = "There's a vertical scene called " + to + " already.";
		return false;
	}
	obs_source_release(taken);
	obs_source_set_name(scene, to.c_str());
	obs_source_release(scene);
	return true;
}

void Canvas::removed(void *self, calldata_t *)
{
	auto *c = static_cast<Canvas *>(self);
	// Whoever listens lets go of what uses the picture's video first.
	if (c->on_removed)
		c->on_removed();
	c->release();
}

void Canvas::scene_renamed(void *self, calldata_t *cd)
{
	auto *c = static_cast<Canvas *>(self);
	const char *from = calldata_string(cd, "prev_name");
	const char *to = calldata_string(cd, "new_name");
	if (!from || !to)
		return;
	std::lock_guard<std::mutex> l(c->current_lock_);
	if (c->current_ == from)
		c->current_ = to;
}

} // namespace direbot::vertical
