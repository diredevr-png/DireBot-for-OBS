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

// The tall picture itself: a second picture in OBS (an OBS "canvas", OBS
// 31.1 and newer) with scenes of its own. OBS keeps it, and its scenes,
// in the scene collection, and lists it as an "Additional canvas" for
// Twitch's phone viewers (Settings → Stream → Enhanced Broadcasting).
// OBS doesn't keep its size, so that's kept here (vertical.cpp saves it
// with the scene collection) and given back each time it loads.
//
// It's drawn only while something uses it ("awake"): while OBS sends or
// records anything, or DireBot is looking at it. The rest of the time it
// costs OBS nothing - its scenes aren't drawn and what's only in them
// isn't running.
//
// Scenes change through a fade, as long as OBS's own transitions.
//
// Everything here is on OBS's UI thread.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <obs.h>

namespace direbot::vertical {

struct SceneRow {
	std::string name;
	std::string uuid;
};

class Canvas {
public:
	Canvas() = default;
	~Canvas();
	Canvas(const Canvas &) = delete;
	Canvas &operator=(const Canvas &) = delete;

	// Finds the tall picture in this scene collection - by its id, else
	// by its name - and gives it its video at this size. False when
	// there's none.
	bool attach(const std::string &uuid, uint32_t width, uint32_t height, const std::string &scene);
	// Makes one, with its first scene.
	bool create(uint32_t width, uint32_t height, std::string &problem);
	// Lets go of it: the scene collection or OBS is closing. (OBS keeps
	// it in the collection.)
	void release();

	bool exists() const { return canvas_ != nullptr; }
	obs_canvas_t *get() const { return canvas_; }
	// Its video, for encoders. Null without one.
	video_t *video() const;
	bool has_video() const;
	std::string uuid() const;
	uint32_t width() const { return width_; }
	uint32_t height() const { return height_; }
	double fps() const;

	// OBS can only give it video when nothing is sending or recording;
	// tried again from here when it couldn't.
	bool ensure_video();
	// A new size: only while OBS isn't sending or recording.
	bool resize(uint32_t width, uint32_t height, std::string &problem);
	// OBS's own video changed (resolution, frame rate): same size, OBS's
	// new frame rate.
	void main_video_changed();

	void set_awake(bool awake);
	bool awake() const { return awake_; }

	std::vector<SceneRow> scenes() const;
	std::vector<std::string> scene_names() const;
	std::string current() const { return current_; }
	// Puts a scene on; a fade when it's awake and `animate`.
	bool switch_to(const std::string &name, bool animate);
	// Makes a scene (a new name: " 2" added when it's taken).
	SceneRow create_scene(const std::string &name, std::string &problem);
	bool remove_scene(const std::string &name, std::string &problem);
	bool rename_scene(const std::string &name, const std::string &to, std::string &problem);

	// OBS removed it (the scene collection is closing): called on the UI
	// thread, after which it's let go.
	std::function<void()> on_removed;

private:
	obs_canvas_t *canvas_ = nullptr;
	obs_source_t *transition_ = nullptr;
	bool awake_ = false;
	uint32_t width_ = 0, height_ = 0;
	std::string current_;

	bool video_info(uint32_t width, uint32_t height, obs_video_info &ovi) const;
	void make_transition();
	void connect(obs_canvas_t *canvas);
	static void removed(void *self, calldata_t *cd);
	static void scene_renamed(void *self, calldata_t *cd);
	obs_source_t *scene_source(const std::string &name) const;
};

} // namespace direbot::vertical
