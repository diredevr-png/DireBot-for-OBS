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

#include "follow-cam.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>

#include <plugin-support.h>

#include "tracker.hpp"

namespace direbot::follow {

namespace {

// How often it looks for you, in seconds.
constexpr float LOOK_EVERY = 0.1f;
// Not looked for this long (not shown, or not following): what it saw
// last is too old to compare with.
constexpr float STALE = 0.5f;
// How long a lost camera (renamed away, removed) waits between looks for
// it again, in seconds.
constexpr float FIND_EVERY = 1.0f;
constexpr int MIN_SIZE = 64, MAX_SIZE = 4096;
// Moves smaller than this share of the crop's width are let be: closer
// in, smaller moves show, so it follows them.
constexpr float STILL_SHARE = 0.12f;

const char *text(const char *key)
{
	return obs_module_text(key);
}

struct Speed {
	const char *id;
	const char *words;
	float seconds;
};
constexpr Speed SPEEDS[] = {{"slow", "FollowCam.Slow", 2.5f},
			    {"normal", "FollowCam.Normal", 1.2f},
			    {"quick", "FollowCam.Quick", 0.6f}};

float seconds_for(const char *id)
{
	for (const Speed &s : SPEEDS)
		if (id && std::strcmp(id, s.id) == 0)
			return s.seconds;
	return SPEEDS[1].seconds;
}

uint32_t even_size(long long v)
{
	v = std::min<long long>(MAX_SIZE, std::max<long long>(MIN_SIZE, v));
	return (uint32_t)(v & ~1LL);
}

struct FollowCam {
	obs_source_t *self = nullptr;

	// Set on OBS's UI thread (update), read while drawing.
	std::mutex lock, find_lock;
	std::string camera_name;
	obs_weak_source_t *camera = nullptr;
	std::atomic<uint32_t> width{1080}, height{1920};
	std::atomic<float> zoom{1.0f}, seconds{1.2f};
	std::atomic<bool> following{true};

	// Only on OBS's graphics thread (video_tick and video_render).
	Tracker tracker;
	Smoother smoother;
	gs_texrender_t *out = nullptr;
	gs_texrender_t *small = nullptr;
	gs_stagesurf_t *stage = nullptr;
	uint32_t stage_w = 0, stage_h = 0;
	bool staged = false;
	float since_look = 0, since_find = 0;
	float window = 0.3f; // the crop's width, a share of the camera's
	std::vector<uint8_t> grey;
	bool drawing = false;
	// The crop was drawn this frame: shown in more than one place (the
	// tall picture and a main scene, both scenes of a fade), it's drawn
	// once and the same picture shown again.
	bool drawn = false;

	~FollowCam()
	{
		obs_enter_graphics();
		gs_texrender_destroy(out);
		gs_texrender_destroy(small);
		gs_stagesurface_destroy(stage);
		obs_leave_graphics();
		obs_weak_source_release(camera);
	}

	// The camera, with a reference to release; null when there's none.
	obs_source_t *get_camera()
	{
		std::lock_guard<std::mutex> l(lock);
		return camera ? obs_weak_source_get_source(camera) : nullptr;
	}

	// Looks the camera up by its name (from OBS's UI thread, or the
	// graphics thread when a lost one comes back).
	void find_camera()
	{
		std::lock_guard<std::mutex> finding(find_lock);
		std::string name;
		{
			std::lock_guard<std::mutex> l(lock);
			name = camera_name;
		}
		obs_source_t *found = name.empty() ? nullptr : obs_get_source_by_name(name.c_str());
		// Never another follow cam (each would draw the other forever).
		if (found && std::strcmp(obs_source_get_id(found), SOURCE_ID) == 0) {
			obs_source_release(found);
			found = nullptr;
		}
		obs_source_t *old = get_camera();
		if (old != found) {
			// While this shows, so does the camera (and it runs): OBS is
			// told which camera that is now. A camera that has this in it
			// (a scene with this follow cam) can't be.
			if (old)
				obs_source_remove_active_child(self, old);
			if (found && !obs_source_add_active_child(self, found)) {
				obs_source_release(found);
				found = nullptr;
			}
			std::lock_guard<std::mutex> l(lock);
			obs_weak_source_release(camera);
			camera = found ? obs_source_get_weak_source(found) : nullptr;
		}
		obs_source_release(old);
		obs_source_release(found);
	}

	void look(obs_source_t *cam, uint32_t cw, uint32_t ch)
	{
		// What was read back last time: OBS had a whole look to copy it.
		if (staged && stage) {
			uint8_t *data = nullptr;
			uint32_t line = 0;
			if (gs_stagesurface_map(stage, &data, &line)) {
				grey.resize((size_t)stage_w * stage_h);
				for (uint32_t y = 0; y < stage_h; y++) {
					const uint8_t *p = data + (size_t)y * line;
					uint8_t *g = &grey[(size_t)y * stage_w];
					for (uint32_t x = 0; x < stage_w; x++, p += 4)
						g[x] = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
				}
				gs_stagesurface_unmap(stage);
				tracker.update(grey.data(), (int)stage_w, (int)stage_h, window);
			}
			staged = false;
		}

		const uint32_t sw = Tracker::WIDTH;
		// At most 8 times as tall as it's wide: a very tall source would
		// want a look bigger than a graphics card makes (and fail every
		// frame). Where it moves is in fractions of the picture, so a
		// squashed look still follows.
		const uint32_t sh = std::clamp<uint32_t>((uint32_t)std::lround((double)sw * ch / cw) & ~1u, 8, sw * 8);
		if (!stage || stage_w != sw || stage_h != sh) {
			gs_stagesurface_destroy(stage);
			stage = gs_stagesurface_create(sw, sh, GS_RGBA);
			stage_w = sw;
			stage_h = sh;
			tracker.forget();
		}
		if (!small)
			small = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		if (!stage || !small)
			return;
		gs_texrender_reset(small);
		if (gs_texrender_begin(small, sw, sh)) {
			vec4 clear;
			vec4_zero(&clear);
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho(0.0f, (float)cw, 0.0f, (float)ch, -100.0f, 100.0f);
			obs_source_video_render(cam);
			gs_texrender_end(small);
			gs_stage_texture(stage, gs_texrender_get_texture(small));
			staged = true;
		}
	}

	void render()
	{
		obs_source_t *cam = get_camera();
		if (!cam)
			return;
		const uint32_t cw = obs_source_get_width(cam), ch = obs_source_get_height(cam);
		const uint32_t w = width, h = height;
		if (!cw || !ch || drawing) {
			obs_source_release(cam);
			return;
		}
		drawing = true;
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

		const Rect r = crop((int)cw, (int)ch, (int)w, (int)h, zoom, smoother.now());
		window = r.w / (float)cw;
		if (following && since_look >= LOOK_EVERY) {
			if (since_look > STALE) {
				staged = false;
				tracker.forget();
			}
			since_look = 0;
			look(cam, cw, ch);
		}
		if (!out)
			out = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		// A texrender draws once until it's reset.
		if (out && !drawn)
			gs_texrender_reset(out);
		if (out && !drawn && gs_texrender_begin(out, w, h)) {
			vec4 clear;
			vec4_zero(&clear);
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho(r.x, r.x + r.w, r.y, r.y + r.h, -100.0f, 100.0f);
			obs_source_video_render(cam);
			gs_texrender_end(out);
			drawn = true;
		}
		gs_blend_state_pop();
		obs_source_release(cam);

		gs_texture_t *tex = drawn ? gs_texrender_get_texture(out) : nullptr;
		if (tex) {
			const bool linear = gs_get_linear_srgb();
			const bool before = gs_framebuffer_srgb_enabled();
			gs_enable_framebuffer_srgb(linear);
			gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
			if (linear)
				gs_effect_set_texture_srgb(image, tex);
			else
				gs_effect_set_texture(image, tex);
			while (gs_effect_loop(effect, "Draw"))
				gs_draw_sprite(tex, 0, w, h);
			gs_enable_framebuffer_srgb(before);
		}
		drawing = false;
	}

	void tick(float dt)
	{
		// A new frame: the crop is drawn afresh the first time it shows.
		drawn = false;
		gs_texrender_reset(out);
		since_look += dt;
		since_find += dt;
		if (since_find >= FIND_EVERY) {
			since_find = 0;
			obs_source_t *cam = get_camera();
			if (cam)
				obs_source_release(cam);
			else
				find_camera();
		}
		Spot target;
		if (following)
			target = tracker.spot();
		smoother.step(target, dt, seconds, std::max(0.01f, STILL_SHARE * window));
	}

	void update(obs_data_t *settings)
	{
		width = even_size(obs_data_get_int(settings, "width"));
		height = even_size(obs_data_get_int(settings, "height"));
		zoom = (float)std::min<long long>(400, std::max<long long>(100, obs_data_get_int(settings, "zoom"))) /
		       100.0f;
		seconds = seconds_for(obs_data_get_string(settings, "speed"));
		following = obs_data_get_bool(settings, "follow");
		const char *name = obs_data_get_string(settings, "source");
		bool changed;
		{
			std::lock_guard<std::mutex> l(lock);
			changed = camera_name != (name ? name : "");
			camera_name = name ? name : "";
		}
		if (changed)
			find_camera();
	}

	// The camera renamed: keep following it.
	static void on_rename(void *data, calldata_t *cd)
	{
		auto *f = static_cast<FollowCam *>(data);
		const char *was = calldata_string(cd, "prev_name");
		const char *now = calldata_string(cd, "new_name");
		obs_source_t *renamed = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
		if (!was || !now || !renamed)
			return;
		{
			std::lock_guard<std::mutex> l(f->lock);
			if (f->camera_name != was)
				return;
			// Only the camera itself: a filter (or anything else) that
			// happened to have its name isn't it. With the camera gone
			// for now, a source by that name that's renamed still is.
			if (f->camera ? !obs_weak_source_references_source(f->camera, renamed)
				      : obs_source_get_type(renamed) != OBS_SOURCE_TYPE_INPUT &&
						obs_source_get_type(renamed) != OBS_SOURCE_TYPE_SCENE)
				return;
			f->camera_name = now;
		}
		obs_data_t *settings = obs_source_get_settings(f->self);
		obs_data_set_string(settings, "source", now);
		obs_data_release(settings);
	}
};

const char *get_name(void *)
{
	return text("FollowCam.Name");
}

void *create(obs_data_t *settings, obs_source_t *source)
{
	auto *f = new FollowCam();
	f->self = source;
	f->update(settings);
	signal_handler_connect(obs_get_signal_handler(), "source_rename", FollowCam::on_rename, f);
	return f;
}

void destroy(void *data)
{
	auto *f = static_cast<FollowCam *>(data);
	signal_handler_disconnect(obs_get_signal_handler(), "source_rename", FollowCam::on_rename, f);
	delete f;
}

void update(void *data, obs_data_t *settings)
{
	static_cast<FollowCam *>(data)->update(settings);
}

void get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "source", "");
	obs_data_set_default_int(settings, "width", 1080);
	obs_data_set_default_int(settings, "height", 1920);
	obs_data_set_default_int(settings, "zoom", 100);
	obs_data_set_default_string(settings, "speed", "normal");
	obs_data_set_default_bool(settings, "follow", true);
}

bool add_camera(void *param, obs_source_t *source)
{
	auto *list = static_cast<obs_property_t *>(param);
	const uint32_t flags = obs_source_get_output_flags(source);
	if (!(flags & OBS_SOURCE_VIDEO) || std::strcmp(obs_source_get_id(source), SOURCE_ID) == 0)
		return true;
	const char *name = obs_source_get_name(source);
	if (name && *name)
		obs_property_list_add_string(list, name, name);
	return true;
}

obs_properties_t *get_properties(void *data)
{
	auto *f = static_cast<FollowCam *>(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *list = obs_properties_add_list(props, "source", text("FollowCam.Camera"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(list, text("FollowCam.None"), "");
	obs_enum_sources(add_camera, list);
	if (f) {
		// One that's gone still shows, so it isn't lost by opening this.
		std::lock_guard<std::mutex> l(f->lock);
		if (!f->camera_name.empty() && (!f->camera || obs_weak_source_expired(f->camera)))
			obs_property_list_add_string(list, f->camera_name.c_str(), f->camera_name.c_str());
	}
	obs_property_t *follow = obs_properties_add_bool(props, "follow", text("FollowCam.Follow"));
	obs_property_set_long_description(follow, text("FollowCam.FollowHelp"));
	obs_property_t *zoom = obs_properties_add_int_slider(props, "zoom", text("FollowCam.Zoom"), 100, 300, 5);
	obs_property_int_set_suffix(zoom, "%");
	obs_property_t *speed = obs_properties_add_list(props, "speed", text("FollowCam.Speed"), OBS_COMBO_TYPE_LIST,
							OBS_COMBO_FORMAT_STRING);
	for (const Speed &s : SPEEDS)
		obs_property_list_add_string(speed, text(s.words), s.id);
	obs_properties_add_int(props, "width", text("FollowCam.Width"), MIN_SIZE, MAX_SIZE, 2);
	obs_properties_add_int(props, "height", text("FollowCam.Height"), MIN_SIZE, MAX_SIZE, 2);
	obs_properties_add_text(props, "about", text("FollowCam.About"), OBS_TEXT_INFO);
	return props;
}

uint32_t get_width(void *data)
{
	return static_cast<FollowCam *>(data)->width;
}

uint32_t get_height(void *data)
{
	return static_cast<FollowCam *>(data)->height;
}

void video_tick(void *data, float seconds)
{
	static_cast<FollowCam *>(data)->tick(seconds);
}

void video_render(void *data, gs_effect_t *)
{
	static_cast<FollowCam *>(data)->render();
}

// The camera shows (and runs) while this does.
void enum_active_sources(void *data, obs_source_enum_proc_t enum_callback, void *param)
{
	auto *f = static_cast<FollowCam *>(data);
	obs_source_t *cam = f->get_camera();
	if (cam) {
		enum_callback(f->self, cam, param);
		obs_source_release(cam);
	}
}

} // namespace

void register_source()
{
	obs_source_info info = {};
	info.id = SOURCE_ID;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
	info.icon_type = OBS_ICON_TYPE_CAMERA;
	info.get_name = get_name;
	info.create = create;
	info.destroy = destroy;
	info.update = update;
	info.get_defaults = get_defaults;
	info.get_properties = get_properties;
	info.get_width = get_width;
	info.get_height = get_height;
	info.video_tick = video_tick;
	info.video_render = video_render;
	info.enum_active_sources = enum_active_sources;
	obs_register_source(&info);
}

} // namespace direbot::follow
