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

#include "face-avatar.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/image-file.h>
#include <graphics/matrix4.h>
#include <graphics/vec4.h>
#include <util/platform.h>

#include <plugin-support.h>

#include "../obs/ui-call.hpp"
#include "tracker.hpp"

namespace direbot::face {

using json = nlohmann::json;

namespace {

// How often it looks for your face, in seconds.
constexpr float LOOK_EVERY = 1.0f / 30;
// Not looked for this long (not shown): what it saw last is too old.
constexpr float STALE = 0.5f;
constexpr float FIND_EVERY = 1.0f;
constexpr int MIN_SIZE = 64, MAX_SIZE = 4096;
// The set-up still DireBot shows, at most this wide.
constexpr int STILL_WIDTH = 640;
// Outfit layers at most.
constexpr size_t MOST_LAYERS = 12;

const char *text(const char *key)
{
	return obs_module_text(key);
}

float clampf(float v, float lo, float hi)
{
	return std::min(hi, std::max(lo, v));
}

uint32_t even_size(long long v)
{
	v = std::min<long long>(MAX_SIZE, std::max<long long>(MIN_SIZE, v));
	return (uint32_t)(v & ~1LL);
}

struct Speed {
	const char *id;
	const char *words;
	float seconds;
};
constexpr Speed SPEEDS[] = {{"slow", "FaceAvatar.Slow", 1.2f},
			    {"normal", "FaceAvatar.Normal", 0.6f},
			    {"quick", "FaceAvatar.Quick", 0.25f}};

float seconds_for(const char *id)
{
	for (const Speed &s : SPEEDS)
		if (id && std::strcmp(id, s.id) == 0)
			return s.seconds;
	return SPEEDS[1].seconds;
}

// A rectangle as shares of something (the character, the camera).
struct Rect {
	float x = 0, y = 0, w = 0, h = 0;
};

bool rect_from(const json &j, Rect &r)
{
	if (!j.is_array() || j.size() != 4)
		return false;
	for (const json &v : j)
		if (!v.is_number())
			return false;
	r = {clampf(j[0].get<float>(), -1, 2), clampf(j[1].get<float>(), -1, 2), clampf(j[2].get<float>(), 0, 2),
	     clampf(j[3].get<float>(), 0, 2)};
	return r.w > 0 && r.h > 0;
}

// A picture file, loaded once and redrawn every frame. Made and let go of
// on OBS's UI thread, never while the avatar's pictures are locked (that
// would wait on the graphics thread, which may be waiting on the lock).
struct Picture {
	std::string path;
	gs_image_file_t image = {};

	explicit Picture(const std::string &file) : path(file)
	{
		gs_image_file_init(&image, path.c_str());
		obs_enter_graphics();
		gs_image_file_init_texture(&image);
		obs_leave_graphics();
		if (!image.loaded)
			obs_log(LOG_WARNING, "DireBot Face Avatar: couldn't open a picture (is the file still there?)");
	}
	~Picture()
	{
		obs_enter_graphics();
		gs_image_file_free(&image);
		obs_leave_graphics();
	}
	Picture(const Picture &) = delete;
	Picture &operator=(const Picture &) = delete;

	gs_texture_t *texture() const { return image.loaded ? image.texture : nullptr; }
	float width() const { return image.loaded ? (float)image.cx : 0; }
	float height() const { return image.loaded ? (float)image.cy : 0; }
};
using PicturePtr = std::shared_ptr<Picture>;

// The same picture again if it's the same file, a new one if not.
PicturePtr load_picture(const PicturePtr &old, const char *want)
{
	const std::string file = want ? want : "";
	if (old && old->path == file)
		return old;
	return file.empty() ? nullptr : std::make_shared<Picture>(file);
}

bool loaded(const PicturePtr &p)
{
	return p && p->texture();
}

struct Layer {
	PicturePtr picture;
	Rect place; // on the character
	bool behind = false;
};

// The tracker, on a thread of its own: OBS hands it the newest small grey
// picture and carries on; what it found is picked up on a later frame.
class Worker {
public:
	Worker()
	{
		thread_ = std::thread([this] { run(); });
	}
	~Worker()
	{
		{
			std::lock_guard<std::mutex> l(lock_);
			stop_ = true;
		}
		wake_.notify_all();
		thread_.join();
	}

	// A new look (the graphics thread). One not yet worked on is
	// replaced: only the newest matters.
	void give(std::vector<uint8_t> &grey, int w, int h)
	{
		{
			std::lock_guard<std::mutex> l(lock_);
			in_.swap(grey);
			in_w_ = w;
			in_h_ = h;
			have_ = true;
		}
		wake_.notify_one();
	}

	// A new set-up (from the settings), or none.
	void set_up(const std::string &saved)
	{
		{
			std::lock_guard<std::mutex> l(lock_);
			saved_ = saved;
			load_ = true;
		}
		wake_.notify_one();
	}

	void forget()
	{
		std::lock_guard<std::mutex> l(lock_);
		forget_ = true;
	}

	// The newest result and how many looks there have been.
	Face face(uint64_t &looks, bool &ready, float &home_x, float &home_y) const
	{
		std::lock_guard<std::mutex> l(lock_);
		looks = looks_;
		ready = ready_;
		home_x = home_x_;
		home_y = home_y_;
		return face_;
	}

	float looks_per_second() const
	{
		std::lock_guard<std::mutex> l(lock_);
		return rate_;
	}

private:
	void run()
	{
		Tracker tracker;
		std::vector<uint8_t> grey;
		uint64_t window_start = os_gettime_ns();
		int window_looks = 0;
		for (;;) {
			int w = 0, h = 0;
			bool look = false, load = false, forget = false;
			std::string saved;
			{
				std::unique_lock<std::mutex> l(lock_);
				wake_.wait(l, [this] { return stop_ || have_ || load_; });
				if (stop_)
					return;
				if (load_) {
					load = true;
					saved.swap(saved_);
					load_ = false;
				}
				if (have_) {
					grey.swap(in_);
					w = in_w_;
					h = in_h_;
					have_ = false;
					look = true;
				}
				forget = forget_;
				forget_ = false;
			}
			if (load) {
				tracker = Tracker();
				if (!saved.empty() && !tracker.load(saved))
					obs_log(LOG_WARNING, "DireBot Face Avatar: its set-up couldn't be read; set it up again in DireBot.");
			}
			if (forget)
				tracker.forget();
			Face face;
			if (look)
				face = tracker.update(grey.data(), w, h);
			const uint64_t now = os_gettime_ns();
			window_looks += look;
			std::lock_guard<std::mutex> l(lock_);
			ready_ = tracker.ready();
			home_x_ = tracker.home_x();
			home_y_ = tracker.home_y();
			if (look) {
				face_ = face;
				looks_++;
			} else if (load) {
				face_ = Face();
			}
			if (now - window_start >= 2'000'000'000ULL) {
				rate_ = window_looks * 1e9f / (float)(now - window_start);
				window_start = now;
				window_looks = 0;
			}
		}
	}

	mutable std::mutex lock_;
	std::condition_variable wake_;
	std::thread thread_;
	bool stop_ = false, have_ = false, load_ = false, forget_ = false;
	std::vector<uint8_t> in_;
	int in_w_ = 0, in_h_ = 0;
	std::string saved_;
	Face face_;
	uint64_t looks_ = 0;
	bool ready_ = false;
	float home_x_ = 0.5f, home_y_ = 0.5f;
	float rate_ = 0;
};

struct FaceAvatar {
	obs_source_t *self = nullptr;

	// Set on OBS's UI thread (update), read while drawing.
	std::mutex lock, find_lock;
	std::string camera_name;
	obs_weak_source_t *camera = nullptr;
	std::atomic<uint32_t> width{1080}, height{1080};
	// The pictures: loaded on the UI thread, drawn on the graphics one.
	std::mutex pictures_lock;
	PicturePtr body, body_left, body_right, own_face;
	std::vector<Layer> layers;
	Rect spots[PARTS]; // on the character, as the viewer sees them: left eye, right eye, mouth
	Rect boxes[PARTS]; // in the camera, as it sees them: first eye, second eye, mouth
	bool have_spots = false;
	std::string saved; // the tracker's set-up, as last handed over
	float size = 0.85f, part_size = 1.0f, feather = 0.35f, tint_amount = 0;
	vec4 tint;
	bool mirror = true;
	float follow_move = 0.6f, follow_grow = 0.6f, follow_tilt = 1.0f, seconds = 0.6f;
	float most_move = 0.25f; // of the source's size
	bool turn_pictures = true;

	// Only on OBS's graphics thread (video_tick and video_render).
	Worker worker;
	Smoother smoother;
	std::atomic<bool> reset_smoother{false};
	uint64_t seen_looks = 0;
	gs_texrender_t *cam_tex = nullptr; // the camera, as it is
	gs_texrender_t *small = nullptr;   // the camera, small, for a look
	gs_texrender_t *out = nullptr;     // the avatar, drawn once a frame
	gs_stagesurf_t *stage = nullptr;
	uint32_t stage_w = 0, stage_h = 0;
	bool staged = false;
	float since_look = 0, since_find = 0;
	std::vector<uint8_t> grey;
	gs_effect_t *effect = nullptr;
	gs_eparam_t *p_image = nullptr, *p_opacity = nullptr, *p_origin = nullptr, *p_across = nullptr,
		    *p_down = nullptr, *p_feather = nullptr, *p_tint = nullptr, *p_tint_amount = nullptr;
	bool drawing = false, drawn = false;
	// What it showed last, for FaceGetState.
	std::mutex state_lock;
	Smoother::Out shown_now;
	Face face_now;
	bool ready_now = false, showing_now = false;
	uint64_t last_render = 0;

	FaceAvatar()
	{
		vec4_set(&tint, 1, 1, 1, 1);
		char *path = obs_module_file("effects/direbot-face.effect");
		obs_enter_graphics();
		if (path)
			effect = gs_effect_create_from_file(path, nullptr);
		if (effect) {
			p_image = gs_effect_get_param_by_name(effect, "image");
			p_opacity = gs_effect_get_param_by_name(effect, "opacity");
			p_origin = gs_effect_get_param_by_name(effect, "origin");
			p_across = gs_effect_get_param_by_name(effect, "across");
			p_down = gs_effect_get_param_by_name(effect, "down");
			p_feather = gs_effect_get_param_by_name(effect, "feather");
			p_tint = gs_effect_get_param_by_name(effect, "tint");
			p_tint_amount = gs_effect_get_param_by_name(effect, "tint_amount");
		}
		obs_leave_graphics();
		bfree(path);
		if (!effect)
			obs_log(LOG_WARNING, "DireBot Face Avatar: its effect file is missing, so it shows nothing.");
	}

	~FaceAvatar()
	{
		obs_enter_graphics();
		gs_texrender_destroy(cam_tex);
		gs_texrender_destroy(small);
		gs_texrender_destroy(out);
		gs_stagesurface_destroy(stage);
		gs_effect_destroy(effect);
		obs_leave_graphics();
		obs_weak_source_release(camera);
	}

	obs_source_t *get_camera()
	{
		std::lock_guard<std::mutex> l(lock);
		return camera ? obs_weak_source_get_source(camera) : nullptr;
	}

	// As Follow Cam does it: by name, never another face avatar, and OBS
	// told the camera shows while this does.
	void find_camera()
	{
		std::lock_guard<std::mutex> finding(find_lock);
		std::string name;
		{
			std::lock_guard<std::mutex> l(lock);
			name = camera_name;
		}
		obs_source_t *found = name.empty() ? nullptr : obs_get_source_by_name(name.c_str());
		if (found && std::strcmp(obs_source_get_id(found), SOURCE_ID) == 0) {
			obs_source_release(found);
			found = nullptr;
		}
		obs_source_t *old = get_camera();
		if (old != found) {
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

	// A small grey copy of the camera for the tracker: the one read back
	// last time goes to it, and a new one is asked for.
	void look(obs_source_t *cam, uint32_t cw, uint32_t ch)
	{
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
				worker.give(grey, (int)stage_w, (int)stage_h);
			}
			staged = false;
		}
		const uint32_t sw = Tracker::WIDTH;
		const uint32_t sh = std::clamp<uint32_t>((uint32_t)std::lround((double)sw * ch / cw) & ~1u, 32, sw * 4);
		if (!stage || stage_w != sw || stage_h != sh) {
			gs_stagesurface_destroy(stage);
			stage = gs_stagesurface_create(sw, sh, GS_RGBA);
			stage_w = sw;
			stage_h = sh;
			worker.forget();
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

	void draw_picture(gs_texture_t *tex, float x, float y, float w, float h, float opacity)
	{
		if (!tex || opacity <= 0.001f || w <= 0 || h <= 0)
			return;
		gs_effect_set_texture(p_image, tex);
		gs_effect_set_float(p_opacity, opacity);
		gs_matrix_push();
		gs_matrix_translate3f(x, y, 0);
		while (gs_effect_loop(effect, "Picture"))
			gs_draw_quadf(tex, 0, w, h);
		gs_matrix_pop();
	}

	// One eye or the mouth, from the camera onto the character's spot.
	void draw_cutout(gs_texture_t *cam, uint32_t cw, uint32_t ch, const Smoother::Out &o, int part, const Rect &spot,
			 float char_w, float char_h, float opacity)
	{
		if (!cam || opacity <= 0.001f)
			return;
		// In the camera: the part's box at set-up, as big as the face is
		// now and turned with it (pixel middles: + half a pixel).
		const float s = o.scale;
		const float bw = boxes[part].w * cw * s, bh = boxes[part].h * ch * s;
		const float c = std::cos(o.angle), sn = std::sin(o.angle);
		const float px = (o.x[part] + 0.5f / Tracker::WIDTH) * cw, py = (o.y[part] + 0.5f / Tracker::WIDTH) * ch;
		float ax = c * bw, ay = sn * bw, dx = -sn * bh, dy = c * bh;
		float ox = px - ax / 2 - dx / 2, oy = py - ay / 2 - dy / 2;
		if (mirror) {
			ox += ax;
			oy += ay;
			ax = -ax;
			ay = -ay;
		}
		vec2 origin, across, down;
		vec2_set(&origin, ox / cw, oy / ch);
		vec2_set(&across, ax / cw, ay / ch);
		vec2_set(&down, dx / cw, dy / ch);
		// On the character: its spot, made bigger or smaller round its
		// middle by "Eye and mouth size".
		const float sw = spot.w * char_w * part_size, sh = spot.h * char_h * part_size;
		const float sx = (spot.x + spot.w / 2) * char_w - sw / 2, sy = (spot.y + spot.h / 2) * char_h - sh / 2;
		gs_effect_set_texture(p_image, cam);
		gs_effect_set_float(p_opacity, opacity);
		gs_effect_set_vec2(p_origin, &origin);
		gs_effect_set_vec2(p_across, &across);
		gs_effect_set_vec2(p_down, &down);
		gs_effect_set_float(p_feather, feather);
		gs_effect_set_vec4(p_tint, &tint);
		gs_effect_set_float(p_tint_amount, tint_amount);
		gs_matrix_push();
		gs_matrix_translate3f(sx, sy, 0);
		while (gs_effect_loop(effect, "Cutout"))
			gs_draw_quadf(nullptr, 0, sw, sh);
		gs_matrix_pop();
	}

	void render()
	{
		const uint32_t w = width, h = height;
		if (drawing || !effect)
			return;
		{
			std::lock_guard<std::mutex> l(state_lock);
			showing_now = true;
			last_render = os_gettime_ns();
		}
		obs_source_t *cam = get_camera();
		uint32_t cw = cam ? obs_source_get_width(cam) : 0, ch = cam ? obs_source_get_height(cam) : 0;
		drawing = true;
		gs_blend_state_push();
		if (cam && cw && ch) {
			if (since_look >= LOOK_EVERY) {
				if (since_look > STALE) {
					staged = false;
					worker.forget();
				}
				since_look = 0;
				gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
				look(cam, cw, ch);
			}
			// The camera as it is, for the cut-outs.
			if (!cam_tex)
				cam_tex = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
			if (!drawn && cam_tex) {
				gs_texrender_reset(cam_tex);
				gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
				if (gs_texrender_begin(cam_tex, cw, ch)) {
					vec4 clear;
					vec4_zero(&clear);
					gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
					gs_ortho(0.0f, (float)cw, 0.0f, (float)ch, -100.0f, 100.0f);
					obs_source_video_render(cam);
					gs_texrender_end(cam_tex);
				}
			}
		}
		if (!out)
			out = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		if (out && !drawn && gs_texrender_begin(out, w, h)) {
			vec4 clear;
			vec4_zero(&clear);
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
			// Straight colour in, premultiplied out.
			gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE,
						   GS_BLEND_INVSRCALPHA);
			draw_avatar(cam && cw && ch ? gs_texrender_get_texture(cam_tex) : nullptr, cw, ch, w, h);
			gs_texrender_end(out);
			drawn = true;
		}
		gs_blend_state_pop();
		obs_source_release(cam);

		gs_texture_t *tex = drawn ? gs_texrender_get_texture(out) : nullptr;
		if (tex) {
			gs_blend_state_push();
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
			gs_effect_t *base = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_effect_set_texture(gs_effect_get_param_by_name(base, "image"), tex);
			while (gs_effect_loop(base, "Draw"))
				gs_draw_sprite(tex, 0, w, h);
			gs_blend_state_pop();
		}
		drawing = false;
	}

	void draw_avatar(gs_texture_t *cam, uint32_t cw, uint32_t ch, uint32_t w, uint32_t h)
	{
		std::lock_guard<std::mutex> l(pictures_lock);
		const Smoother::Out o = smoother.now();
		// Which way it faces: turned pictures if there are any.
		int turned = turn_pictures ? o.turned * (mirror ? -1 : 1) : 0;
		const Picture *pic = body.get();
		if (turned < 0 && loaded(body_left))
			pic = body_left.get();
		else if (turned > 0 && loaded(body_right))
			pic = body_right.get();
		if (!pic || !pic->texture())
			return;
		const float char_w = pic->width(), char_h = pic->height();
		// Sized to fit, then following you: moved, grown and tilted as
		// you are against the set-up still, within limits.
		float home_x = 0.5f, home_y = 0.5f;
		uint64_t looks;
		bool ready;
		worker.face(looks, ready, home_x, home_y);
		const float fit = std::min(w / char_w, h / char_h) * size;
		float dx = (o.cx - home_x) * follow_move, dy = (o.cy - home_y) * follow_move;
		if (mirror)
			dx = -dx;
		dx = clampf(dx, -most_move, most_move);
		dy = clampf(dy, -most_move, most_move);
		const float grow = clampf(std::pow(std::max(0.05f, o.scale), follow_grow), 0.6f, 1.6f);
		const float tilt = clampf(o.angle * follow_tilt * (mirror ? -1 : 1), -0.6f, 0.6f);
		gs_matrix_push();
		gs_matrix_translate3f(w * (0.5f + dx), h * (0.5f + dy), 0);
		gs_matrix_rotaa4f(0, 0, 1, tilt);
		gs_matrix_scale3f(fit * grow, fit * grow, 1);
		gs_matrix_translate3f(-char_w / 2, -char_h / 2, 0);

		for (const Layer &layer : layers)
			if (layer.behind && layer.picture)
				draw_picture(layer.picture->texture(), layer.place.x * char_w, layer.place.y * char_h,
					     layer.place.w * char_w, layer.place.h * char_h, 1);
		draw_picture(pic->texture(), 0, 0, char_w, char_h, 1);
		const float shown = ready && have_spots && cam ? o.shown : 0;
		if (shown > 0) {
			for (int part = 0; part < PARTS; part++) {
				// The camera's first eye is on the viewer's left - unless
				// mirrored, when it's on their right.
				int spot = part;
				if (part != MOUTH && mirror)
					spot = part == FIRST_EYE ? 1 : 0;
				draw_cutout(cam, cw, ch, o, part, spots[spot], char_w, char_h, shown);
			}
		}
		if (own_face)
			draw_picture(own_face->texture(), 0, 0, char_w, char_h, 1 - shown);
		for (const Layer &layer : layers)
			if (!layer.behind && layer.picture)
				draw_picture(layer.picture->texture(), layer.place.x * char_w, layer.place.y * char_h,
					     layer.place.w * char_w, layer.place.h * char_h, 1);
		gs_matrix_pop();
	}

	void tick(float dt)
	{
		drawn = false;
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
		uint64_t looks;
		bool ready;
		float hx, hy;
		const Face face = worker.face(looks, ready, hx, hy);
		const bool fresh = looks != seen_looks;
		seen_looks = looks;
		if (reset_smoother.exchange(false))
			smoother.reset();
		const Smoother::Out o = smoother.step(face, dt, seconds, fresh);
		std::lock_guard<std::mutex> l(state_lock);
		shown_now = o;
		face_now = face;
		ready_now = ready;
		if (os_gettime_ns() - last_render > 500'000'000ULL)
			showing_now = false;
	}

	void update(obs_data_t *settings)
	{
		width = even_size(obs_data_get_int(settings, "width"));
		height = even_size(obs_data_get_int(settings, "height"));
		const char *name = obs_data_get_string(settings, "source");
		bool changed;
		{
			std::lock_guard<std::mutex> l(lock);
			changed = camera_name != (name ? name : "");
			camera_name = name ? name : "";
		}
		if (changed)
			find_camera();

		// The set-up (from DireBot's FaceSetUp): how the parts look, and
		// each one's box in the camera at the time.
		const json face = json::parse(obs_data_get_string(settings, "face"), nullptr, false);
		std::string tracker_text;
		Rect got[PARTS];
		bool have_boxes = false;
		if (face.is_object() && face.contains("boxes") && face["boxes"].is_array() && face["boxes"].size() == PARTS) {
			have_boxes = true;
			for (int i = 0; i < PARTS; i++)
				have_boxes = have_boxes && rect_from(face["boxes"][i], got[i]);
			if (face.contains("tracker") && face["tracker"].is_string())
				tracker_text = face["tracker"].get<std::string>();
		}
		if (!have_boxes)
			tracker_text.clear();
		const json spot_json = json::parse(obs_data_get_string(settings, "spots"), nullptr, false);
		Rect got_spots[PARTS];
		bool have = spot_json.is_object();
		const char *names[PARTS] = {"left_eye", "right_eye", "mouth"};
		for (int i = 0; i < PARTS && have; i++)
			have = spot_json.contains(names[i]) && rect_from(spot_json[names[i]], got_spots[i]);
		const json outfit = json::parse(obs_data_get_string(settings, "outfit"), nullptr, false);

		// The pictures are loaded first, outside the lock (see Picture);
		// the ones they replace are let go after it.
		PicturePtr next_body, next_left, next_right, next_face;
		std::vector<Layer> next_layers;
		{
			std::lock_guard<std::mutex> l(pictures_lock);
			next_body = body;
			next_left = body_left;
			next_right = body_right;
			next_face = own_face;
			next_layers = layers;
		}
		next_body = load_picture(next_body, obs_data_get_string(settings, "character"));
		next_left = load_picture(next_left, obs_data_get_string(settings, "character_left"));
		next_right = load_picture(next_right, obs_data_get_string(settings, "character_right"));
		next_face = load_picture(next_face, obs_data_get_string(settings, "own_face"));
		// Outfit layers: a file still worn keeps its picture.
		std::vector<Layer> worn;
		if (outfit.is_array()) {
			for (const json &row : outfit) {
				if (worn.size() >= MOST_LAYERS || !row.is_object() || !row.contains("file") ||
				    !row["file"].is_string())
					continue;
				Layer layer;
				if (!rect_from(row.value("place", json()), layer.place))
					continue;
				const std::string file = row["file"].get<std::string>();
				for (const Layer &old : next_layers)
					if (old.picture && old.picture->path == file)
						layer.picture = old.picture;
				if (!layer.picture)
					layer.picture = load_picture(nullptr, file.c_str());
				layer.behind = row.value("behind", false);
				worn.push_back(layer);
			}
		}
		{
			std::lock_guard<std::mutex> l(pictures_lock);
			body.swap(next_body);
			body_left.swap(next_left);
			body_right.swap(next_right);
			own_face.swap(next_face);
			layers.swap(worn);
			if (have_boxes)
				std::copy(got, got + PARTS, boxes);
			have_spots = have;
			if (have)
				std::copy(got_spots, got_spots + PARTS, spots);
		}
		next_layers.clear();
		if (tracker_text != saved) {
			saved = tracker_text;
			worker.set_up(saved);
			reset_smoother = true;
		}

		size = clampf((float)obs_data_get_int(settings, "size") / 100.0f, 0.2f, 1.0f);
		part_size = clampf((float)obs_data_get_int(settings, "part_size") / 100.0f, 0.5f, 2.0f);
		feather = clampf((float)obs_data_get_int(settings, "feather") / 100.0f, 0, 1);
		tint_amount = clampf((float)obs_data_get_int(settings, "tint_amount") / 100.0f, 0, 1);
		const uint32_t colour = (uint32_t)obs_data_get_int(settings, "tint");
		vec4_from_rgba(&tint, colour | 0xFF000000u);
		mirror = obs_data_get_bool(settings, "mirror");
		turn_pictures = obs_data_get_bool(settings, "turn_pictures");
		follow_move = clampf((float)obs_data_get_int(settings, "follow_move") / 100.0f, 0, 1);
		follow_grow = clampf((float)obs_data_get_int(settings, "follow_grow") / 100.0f, 0, 1);
		follow_tilt = clampf((float)obs_data_get_int(settings, "follow_tilt") / 100.0f, 0, 1);
		most_move = clampf((float)obs_data_get_int(settings, "most_move") / 100.0f, 0, 0.5f);
		seconds = seconds_for(obs_data_get_string(settings, "speed"));
	}

	json state()
	{
		std::lock_guard<std::mutex> l(state_lock);
		json parts = json::array();
		for (int i = 0; i < PARTS; i++)
			parts.push_back({face_now.x[i], face_now.y[i]});
		json camera_name_now;
		{
			std::lock_guard<std::mutex> l2(lock);
			camera_name_now = camera_name;
		}
		return {{"ok", true},
			{"camera", camera_name_now},
			{"ready", ready_now},
			{"showing", showing_now},
			{"found", face_now.found},
			{"sure", face_now.found && face_now.confidence >= SURE},
			{"confidence", std::round(face_now.confidence * 100) / 100},
			{"parts", parts},
			{"scale", std::round(face_now.scale * 100) / 100},
			{"angle", std::round(face_now.angle * 100) / 100},
			{"turned", face_now.turned},
			{"shown", std::round(shown_now.shown * 100) / 100},
			{"looks_per_second", std::round(worker.looks_per_second() * 10) / 10}};
	}

	static void on_rename(void *data, calldata_t *cd)
	{
		auto *f = static_cast<FaceAvatar *>(data);
		const char *was = calldata_string(cd, "prev_name");
		const char *now = calldata_string(cd, "new_name");
		obs_source_t *renamed = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
		if (!was || !now || !renamed)
			return;
		{
			std::lock_guard<std::mutex> l(f->lock);
			if (f->camera_name != was)
				return;
			if (f->camera ? !obs_weak_source_references_source(f->camera, renamed)
				      : obs_source_get_type(renamed) != OBS_SOURCE_TYPE_INPUT)
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
	return text("FaceAvatar.Name");
}

void *create(obs_data_t *settings, obs_source_t *source)
{
	auto *f = new FaceAvatar();
	f->self = source;
	f->update(settings);
	signal_handler_connect(obs_get_signal_handler(), "source_rename", FaceAvatar::on_rename, f);
	return f;
}

void destroy(void *data)
{
	auto *f = static_cast<FaceAvatar *>(data);
	signal_handler_disconnect(obs_get_signal_handler(), "source_rename", FaceAvatar::on_rename, f);
	delete f;
}

void update(void *data, obs_data_t *settings)
{
	static_cast<FaceAvatar *>(data)->update(settings);
}

void get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "source", "");
	obs_data_set_default_int(settings, "width", 1080);
	obs_data_set_default_int(settings, "height", 1080);
	obs_data_set_default_int(settings, "size", 85);
	obs_data_set_default_int(settings, "part_size", 100);
	obs_data_set_default_int(settings, "feather", 35);
	obs_data_set_default_int(settings, "tint", 0xFFFFFF);
	obs_data_set_default_int(settings, "tint_amount", 0);
	obs_data_set_default_bool(settings, "mirror", true);
	obs_data_set_default_bool(settings, "turn_pictures", true);
	obs_data_set_default_int(settings, "follow_move", 60);
	obs_data_set_default_int(settings, "follow_grow", 60);
	obs_data_set_default_int(settings, "follow_tilt", 100);
	obs_data_set_default_int(settings, "most_move", 25);
	obs_data_set_default_string(settings, "speed", "normal");
	obs_data_set_default_string(settings, "face", "");
	obs_data_set_default_string(settings, "spots", "");
	obs_data_set_default_string(settings, "outfit", "[]");
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
	auto *f = static_cast<FaceAvatar *>(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "about", text("FaceAvatar.About"), OBS_TEXT_INFO);
	obs_property_t *list = obs_properties_add_list(props, "source", text("FaceAvatar.Camera"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(list, text("FaceAvatar.None"), "");
	obs_enum_sources(add_camera, list);
	if (f) {
		std::lock_guard<std::mutex> l(f->lock);
		if (!f->camera_name.empty() && (!f->camera || obs_weak_source_expired(f->camera)))
			obs_property_list_add_string(list, f->camera_name.c_str(), f->camera_name.c_str());
	}
	const char *pictures = "PNG (*.png);;All pictures (*.png *.jpg *.jpeg *.webp *.gif)";
	obs_properties_add_path(props, "character", text("FaceAvatar.Character"), OBS_PATH_FILE, pictures, nullptr);
	obs_properties_add_path(props, "character_left", text("FaceAvatar.CharacterLeft"), OBS_PATH_FILE, pictures,
				nullptr);
	obs_properties_add_path(props, "character_right", text("FaceAvatar.CharacterRight"), OBS_PATH_FILE, pictures,
				nullptr);
	obs_properties_add_path(props, "own_face", text("FaceAvatar.OwnFace"), OBS_PATH_FILE, pictures, nullptr);
	obs_property_t *p = obs_properties_add_int_slider(props, "size", text("FaceAvatar.Size"), 20, 100, 1);
	obs_property_int_set_suffix(p, "%");
	p = obs_properties_add_int_slider(props, "part_size", text("FaceAvatar.PartSize"), 50, 200, 5);
	obs_property_int_set_suffix(p, "%");
	p = obs_properties_add_int_slider(props, "feather", text("FaceAvatar.Feather"), 0, 100, 1);
	obs_property_int_set_suffix(p, "%");
	obs_properties_add_color(props, "tint", text("FaceAvatar.Tint"));
	p = obs_properties_add_int_slider(props, "tint_amount", text("FaceAvatar.TintAmount"), 0, 100, 1);
	obs_property_int_set_suffix(p, "%");
	obs_properties_add_bool(props, "mirror", text("FaceAvatar.Mirror"));
	obs_properties_add_bool(props, "turn_pictures", text("FaceAvatar.TurnPictures"));
	p = obs_properties_add_int_slider(props, "follow_move", text("FaceAvatar.FollowMove"), 0, 100, 1);
	obs_property_int_set_suffix(p, "%");
	p = obs_properties_add_int_slider(props, "follow_grow", text("FaceAvatar.FollowGrow"), 0, 100, 1);
	obs_property_int_set_suffix(p, "%");
	p = obs_properties_add_int_slider(props, "follow_tilt", text("FaceAvatar.FollowTilt"), 0, 100, 1);
	obs_property_int_set_suffix(p, "%");
	p = obs_properties_add_int_slider(props, "most_move", text("FaceAvatar.MostMove"), 0, 50, 1);
	obs_property_int_set_suffix(p, "%");
	obs_property_t *speed = obs_properties_add_list(props, "speed", text("FaceAvatar.Speed"), OBS_COMBO_TYPE_LIST,
							OBS_COMBO_FORMAT_STRING);
	for (const Speed &s : SPEEDS)
		obs_property_list_add_string(speed, text(s.words), s.id);
	obs_properties_add_int(props, "width", text("FaceAvatar.Width"), MIN_SIZE, MAX_SIZE, 2);
	obs_properties_add_int(props, "height", text("FaceAvatar.Height"), MIN_SIZE, MAX_SIZE, 2);
	return props;
}

uint32_t get_width(void *data)
{
	return static_cast<FaceAvatar *>(data)->width;
}

uint32_t get_height(void *data)
{
	return static_cast<FaceAvatar *>(data)->height;
}

void video_tick(void *data, float seconds)
{
	static_cast<FaceAvatar *>(data)->tick(seconds);
}

void video_render(void *data, gs_effect_t *)
{
	static_cast<FaceAvatar *>(data)->render();
}

void enum_active_sources(void *data, obs_source_enum_proc_t enum_callback, void *param)
{
	auto *f = static_cast<FaceAvatar *>(data);
	obs_source_t *cam = f->get_camera();
	if (cam) {
		enum_callback(f->self, cam, param);
		obs_source_release(cam);
	}
}

// ---- Vendor requests ------------------------------------------------------

json fail(const char *words)
{
	return {{"ok", false}, {"error", words}};
}

// The last set-up still: its small grey copy, kept for FaceSetUp.
struct Still {
	std::mutex lock;
	uint64_t id = 0;
	std::string camera;
	std::vector<uint8_t> grey;
	int w = 0, h = 0;
};
Still last_still;
std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);

// Draws the camera once at w x h and reads it back (RGBA, row after row).
bool grab(obs_source_t *cam, uint32_t cw, uint32_t ch, uint32_t w, uint32_t h, std::vector<uint8_t> &rgba)
{
	bool ok = false;
	obs_enter_graphics();
	gs_texrender_t *tex = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	gs_stagesurf_t *stage = gs_stagesurface_create(w, h, GS_RGBA);
	if (tex && stage && gs_texrender_begin(tex, w, h)) {
		vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)cw, 0.0f, (float)ch, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		obs_source_video_render(cam);
		gs_blend_state_pop();
		gs_texrender_end(tex);
		gs_stage_texture(stage, gs_texrender_get_texture(tex));
		uint8_t *data = nullptr;
		uint32_t line = 0;
		if (gs_stagesurface_map(stage, &data, &line)) {
			rgba.resize((size_t)w * h * 4);
			for (uint32_t y = 0; y < h; y++)
				std::memcpy(&rgba[(size_t)y * w * 4], data + (size_t)y * line, (size_t)w * 4);
			gs_stagesurface_unmap(stage);
			ok = true;
		}
	}
	gs_stagesurface_destroy(stage);
	gs_texrender_destroy(tex);
	obs_leave_graphics();
	return ok;
}

// A picture as a BMP file (24-bit, bottom row first): every browser shows
// one, and it needs no code but this.
std::string bmp(const std::vector<uint8_t> &rgba, uint32_t w, uint32_t h)
{
	const uint32_t row = (w * 3 + 3) & ~3u, size = 54 + row * h;
	std::vector<uint8_t> out(size, 0);
	auto put = [&](size_t at, uint32_t v, int bytes) {
		for (int i = 0; i < bytes; i++)
			out[at + i] = (uint8_t)(v >> (8 * i));
	};
	out[0] = 'B';
	out[1] = 'M';
	put(2, size, 4);
	put(10, 54, 4);
	put(14, 40, 4);
	put(18, w, 4);
	put(22, h, 4);
	put(26, 1, 2);
	put(28, 24, 2);
	put(34, row * h, 4);
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *p = &rgba[(size_t)(h - 1 - y) * w * 4];
		uint8_t *q = &out[54 + (size_t)y * row];
		for (uint32_t x = 0; x < w; x++, p += 4, q += 3) {
			q[0] = p[2];
			q[1] = p[1];
			q[2] = p[0];
		}
	}
	return "data:image/bmp;base64," + encode(out);
}

json take_still(const json &data)
{
	const std::string name = data.value("camera", std::string());
	if (name.empty())
		return fail("Pick your camera first.");
	obs_source_t *cam = obs_get_source_by_name(name.c_str());
	if (!cam)
		return fail("That camera isn't in OBS any more.");
	const uint32_t cw = obs_source_get_width(cam), ch = obs_source_get_height(cam);
	if (!cw || !ch) {
		obs_source_release(cam);
		return fail("The camera shows nothing right now - is it on, and in use by another app?");
	}
	// Shown for a moment, as OBS's own screenshots do, so a camera set to
	// switch off when hidden gives a picture.
	obs_source_inc_showing(cam);
	const uint32_t sw = std::min<uint32_t>(STILL_WIDTH, cw);
	const uint32_t sh = std::max<uint32_t>(2, (uint32_t)std::lround((double)sw * ch / cw));
	const uint32_t gw = Tracker::WIDTH;
	const uint32_t gh = std::clamp<uint32_t>((uint32_t)std::lround((double)gw * ch / cw) & ~1u, 32, gw * 4);
	std::vector<uint8_t> big, small;
	const bool ok = grab(cam, cw, ch, sw, sh, big) && grab(cam, cw, ch, gw, gh, small);
	obs_source_dec_showing(cam);
	obs_source_release(cam);
	if (!ok)
		return fail("Couldn't take a picture from the camera.");
	std::lock_guard<std::mutex> l(last_still.lock);
	last_still.id++;
	last_still.camera = name;
	last_still.w = (int)gw;
	last_still.h = (int)gh;
	last_still.grey.resize((size_t)gw * gh);
	for (size_t i = 0; i < last_still.grey.size(); i++) {
		const uint8_t *p = &small[i * 4];
		last_still.grey[i] = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
	}
	return {{"ok", true},
		{"still", last_still.id},
		{"camera", name},
		{"width", sw},
		{"height", sh},
		{"image", bmp(big, sw, sh)}};
}

// The face avatar source by name, with a reference to release.
obs_source_t *find_avatar(const json &data)
{
	const std::string name = data.value("source", std::string());
	obs_source_t *s = name.empty() ? nullptr : obs_get_source_by_name(name.c_str());
	if (s && std::strcmp(obs_source_get_unversioned_id(s), SOURCE_ID) != 0) {
		obs_source_release(s);
		s = nullptr;
	}
	return s;
}

json set_up(const json &data)
{
	obs_source_t *src = find_avatar(data);
	if (!src)
		return fail("That face avatar isn't in OBS any more.");
	Box boxes[PARTS];
	json rows = data.value("boxes", json());
	bool ok = rows.is_array() && rows.size() == PARTS;
	for (int i = 0; ok && i < PARTS; i++) {
		Rect r;
		ok = rect_from(rows[i], r);
		boxes[i] = {r.x, r.y, r.w, r.h};
	}
	if (!ok) {
		obs_source_release(src);
		return fail("Put a box over each eye and your mouth first.");
	}
	Tracker tracker;
	std::string problem, camera;
	{
		std::lock_guard<std::mutex> l(last_still.lock);
		if (!last_still.id || data.value("still", (uint64_t)0) != last_still.id) {
			obs_source_release(src);
			return fail("That picture is too old - take a new one.");
		}
		camera = last_still.camera;
		if (!tracker.set_up(last_still.grey.data(), last_still.w, last_still.h, boxes, &problem)) {
			obs_source_release(src);
			return fail(problem.c_str());
		}
	}
	json saved = {{"tracker", tracker.save()}, {"boxes", rows}};
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "face", saved.dump().c_str());
	obs_data_set_string(settings, "source", camera.c_str());
	obs_source_update(src, settings);
	obs_data_release(settings);
	obs_source_release(src);
	return {{"ok", true}};
}

json get_state(const json &data)
{
	obs_source_t *src = find_avatar(data);
	if (!src)
		return fail("That face avatar isn't in OBS any more.");
	auto *f = static_cast<FaceAvatar *>(obs_obj_get_data(src));
	json answer = f ? f->state() : fail("It isn't ready yet.");
	obs_source_release(src);
	return answer;
}

} // namespace

const std::vector<std::string> &requests()
{
	static const std::vector<std::string> list = {"FaceStill", "FaceSetUp", "FaceGetState"};
	return list;
}

bool handles(const std::string &type)
{
	const auto &list = requests();
	return std::find(list.begin(), list.end(), type) != list.end();
}

json request(const std::string &type, const json &data)
{
	return obs::ui_call(alive, [type, data]() -> json {
		if (type == "FaceStill")
			return take_still(data);
		if (type == "FaceSetUp")
			return set_up(data);
		if (type == "FaceGetState")
			return get_state(data);
		return fail("DireBot for OBS doesn't know that request.");
	});
}

void register_source()
{
	obs_source_info info = {};
	info.id = SOURCE_ID;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
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

} // namespace direbot::face
