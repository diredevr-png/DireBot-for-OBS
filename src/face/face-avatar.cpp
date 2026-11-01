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
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/image-file.h>
#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>
#include <util/platform.h>

#include <plugin-support.h>

#include "../link/crypto.hpp"
#include "../obs/ui-call.hpp"
#include "avatar2d.hpp"
#include "avatar3d.hpp"
#include "effect-check.hpp"
#include "look-pace.hpp"
#include "model.hpp"
#include "rig.hpp"
#include "tracker.hpp"

namespace direbot::face {

using json = nlohmann::json;

namespace {

// How often it looks for your face at most, in seconds ("Easy on my PC":
// half as often) - at each new picture the camera sends, up to that.
constexpr float LOOK_EVERY = 1.0f / 30, EASY_LOOK_EVERY = 1.0f / 15;
// Not drawn for this long (not shown): what it saw last is too old.
constexpr float STALE = 0.5f;
constexpr float FIND_EVERY = 1.0f;
constexpr int MIN_SIZE = 64, MAX_SIZE = 4096;
// The picture the tracker gets: the camera, at most this big on its
// longest side (the face models work on 256 x 256 of it at most).
constexpr uint32_t LOOK_SIZE = 960;
// Camera pictures kept for the cut-outs: the one in use, the one the
// tracker is looking at, the newest, and one to spare (see look; one
// that doesn't go to the tracker is free again straight away).
constexpr int SHOTS = 4;
// Outfit layers at most.
constexpr size_t MOST_LAYERS = 12;
// How much bigger than its spot an eye or the mouth is drawn: room round
// it for its soft edge.
constexpr float PIECE_ROOM = 1.25f;
// Turning and nodding at most (degrees): in its own spot; worn, nodding.
constexpr float MOST_YAW = 40, MOST_PITCH = 20, WORN_PITCH = 25;
// A 3D character turns and nods more (it's really turned).
constexpr float MOST_YAW_3D = 60, MOST_PITCH_3D = 40;
constexpr float RAD = 3.14159265f / 180;
// Room round the character as it turns (a share each side), and the
// most pixels across it's drawn at before it turns.
constexpr float TURN_PAD = 0.08f, FLAT_MOST = 2048;
// Worn on your head, when it can't see you: held this long, then faded
// over this long (seconds).
constexpr float WORN_HOLD = 1.0f, WORN_FADE = 0.5f;
// A tail's usual wag: this many a second, there and back (its "wag_speed"
// setting, a share of it).
constexpr float WAG_SPEED = 1.5f;

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

// A rectangle as shares of something (the character, the camera).
using Rect = Box;

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
	// A small copy of it (straight colour, 4 bytes a point), for its shape.
	std::vector<uint8_t> small;
	int small_w = 0, small_h = 0;
	bool small_bgra = false;

	explicit Picture(const std::string &file) : path(file)
	{
		gs_image_file_init(&image, path.c_str());
		keep_small();
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

	// Before the picture goes to the graphics card (which lets go of its
	// points): every few points, at most this many across.
	void keep_small()
	{
		constexpr uint32_t MOST = 512;
		if (!image.loaded || image.is_animated_gif || !image.texture_data || !image.cx || !image.cy)
			return;
		const bool bgra = image.format == GS_BGRA || image.format == GS_BGRX;
		if (!bgra && image.format != GS_RGBA)
			return;
		const bool opaque = image.format == GS_BGRX;
		const uint32_t every = std::max(1u, (std::max(image.cx, image.cy) + MOST - 1) / MOST);
		small_w = (int)((image.cx + every - 1) / every);
		small_h = (int)((image.cy + every - 1) / every);
		small.resize((size_t)small_w * small_h * 4);
		for (int y = 0; y < small_h; y++)
			for (int x = 0; x < small_w; x++) {
				const uint8_t *from =
					image.texture_data + ((size_t)y * every * image.cx + (size_t)x * every) * 4;
				uint8_t *to = &small[((size_t)y * small_w + x) * 4];
				std::memcpy(to, from, 4);
				if (opaque)
					to[3] = 255;
			}
		small_bgra = bgra;
	}
	Picture(const Picture &) = delete;
	Picture &operator=(const Picture &) = delete;

	// How much of it shows at (u, v) (0 to 1 across and down it), from the
	// small copy: 0 outside it.
	float alpha_at(float u, float v) const
	{
		if (small.empty() || u < 0 || v < 0 || u >= 1 || v >= 1)
			return 0;
		const int x = std::min(small_w - 1, (int)(u * small_w)), y = std::min(small_h - 1, (int)(v * small_h));
		return small[((size_t)y * small_w + x) * 4 + 3] / 255.0f;
	}

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

// Its drawn face's pictures in the settings, and the looks' names.
const char *const EYE_KEYS[EYE_LOOKS] = {"eyes_open", "eyes_half", "eyes_closed"};
const char *const MOUTH_KEYS[MOUTH_LOOKS] = {"mouth_closed", "mouth_open", "mouth_wide", "mouth_smile", "mouth_o"};
const char *const EYE_NAMES[EYE_LOOKS] = {"open", "half", "closed"};
const char *const MOUTH_NAMES[MOUTH_LOOKS] = {"closed", "open", "wide", "smile", "o"};

// What's drawn before the character turns (shares of it): itself, its
// outfit, and your eyes and mouth round its spots (a wide-open mouth
// comes out further).
Rect canvas_of(const std::vector<Layer> &layers, const Rect spots[PARTS], bool have_spots, float part_size)
{
	float x0 = 0, y0 = 0, x1 = 1, y1 = 1;
	auto take = [&](float l, float t, float r, float b) {
		x0 = std::min(x0, l);
		y0 = std::min(y0, t);
		x1 = std::max(x1, r);
		y1 = std::max(y1, b);
	};
	for (const Layer &layer : layers)
		take(layer.place.x, layer.place.y, layer.place.x + layer.place.w, layer.place.y + layer.place.h);
	for (int i = 0; i < PARTS && have_spots; i++) {
		const Rect &r = spots[i];
		const float grow = part_size * PIECE_ROOM * 1.4f;
		const float cx = r.x + r.w / 2, cy = r.y + r.h / 2;
		take(cx - r.w * grow / 2, cy - r.h * grow / 2, cx + r.w * grow / 2, cy + r.h * grow / 2);
	}
	x0 = std::max(x0, -1.0f);
	y0 = std::max(y0, -1.0f);
	x1 = std::min(x1, 3.0f);
	y1 = std::min(y1, 3.0f);
	return {x0, y0, x1 - x0, y1 - y0};
}

// A character picture's shape (avatar2d.hpp): how far it stands out,
// over the canvas, outfit and all (so a hat turns with the head); and
// from the picture alone, where it's see-through and its colour round
// each spot. None for a picture with no small copy (an animated GIF).
std::shared_ptr<const Shape> shape_for(const PicturePtr &pic, const std::vector<Layer> &layers, const Rect &canvas,
				       const Rect spots[PARTS], bool have_spots)
{
	if (!pic || pic->small.empty())
		return nullptr;
	const Shape own = shape_of(pic->small.data(), pic->small_w, pic->small_h, pic->small_w * 4, pic->small_bgra,
				   spots, have_spots);
	constexpr float MOST = 320;
	const float across = canvas.w * pic->small_w, down = canvas.h * pic->small_h;
	const float k = std::min(1.0f, MOST / std::max(1.0f, std::max(across, down)));
	const int gw = std::max(1, (int)std::lround(across * k)), gh = std::max(1, (int)std::lround(down * k));
	std::vector<uint8_t> all((size_t)gw * gh * 4, 0);
	for (int y = 0; y < gh; y++)
		for (int x = 0; x < gw; x++) {
			const float u = canvas.x + (x + 0.5f) / gw * canvas.w,
				    v = canvas.y + (y + 0.5f) / gh * canvas.h;
			float clear = 1 - pic->alpha_at(u, v);
			for (const Layer &layer : layers)
				if (layer.picture && layer.place.w > 0 && layer.place.h > 0)
					clear *= 1 - layer.picture->alpha_at((u - layer.place.x) / layer.place.w,
									     (v - layer.place.y) / layer.place.h);
			all[((size_t)y * gw + x) * 4 + 3] = (uint8_t)std::lround((1 - clear) * 255);
		}
	Rect on[PARTS];
	for (int i = 0; i < PARTS; i++)
		on[i] = {(spots[i].x - canvas.x) / canvas.w, (spots[i].y - canvas.y) / canvas.h, spots[i].w / canvas.w,
			 spots[i].h / canvas.h};
	auto s = std::make_shared<Shape>(shape_of(all.data(), gw, gh, gw * 4, false, on, have_spots));
	std::copy(own.hole, own.hole + PARTS, s->hole);
	std::copy(own.colour, own.colour + PARTS, s->colour);
	return s;
}

// The tracker, on a thread of its own: OBS hands it the newest picture
// and carries on; what it found is picked up on a later frame. It loads
// the face models itself, so OBS never waits for that either.
class Worker {
public:
	// `done`: "Set" has finished, with it as text (on this thread).
	Worker(std::string models, std::function<void(const std::string &)> done)
		: models_(std::move(models)),
		  done_(std::move(done))
	{
		thread_ = std::thread([this] { run(); });
	}
	~Worker() { stop(); }

	// Waits for the thread to finish (its "Set" callback included).
	void stop()
	{
		{
			std::lock_guard<std::mutex> l(lock_);
			stop_ = true;
		}
		wake_.notify_all();
		if (thread_.joinable())
			thread_.join();
	}

	// A new look (the graphics thread): RGBA, row after row, and when it
	// was taken. One not yet worked on is replaced: only the newest
	// matters.
	void give(std::vector<uint8_t> &rgba, int w, int h, uint64_t ns)
	{
		{
			std::lock_guard<std::mutex> l(lock_);
			in_.swap(rgba);
			in_w_ = w;
			in_h_ = h;
			in_ns_ = ns;
			have_ = true;
		}
		wake_.notify_one();
	}

	void forget()
	{
		std::lock_guard<std::mutex> l(lock_);
		forget_ = true;
	}

	void set_smoothing(float amount) { smoothing_ = amount; }

	// How the pieces of your eyes and mouth are cut (avatar2d.hpp's
	// cut_pieces): each one's height over its width, and how soft.
	void set_cut(const float aspect[PARTS], float soft)
	{
		std::lock_guard<std::mutex> l(lock_);
		std::copy(aspect, aspect + PARTS, aspect_);
		soft_ = soft;
	}

	// "Set": the next looks that find the face are its rest.
	void start_rest()
	{
		{
			std::lock_guard<std::mutex> l(lock_);
			start_rest_ = true;
		}
		wake_.notify_one();
	}
	// A "Set" from the settings ("": none).
	void load_rest(const std::string &text)
	{
		{
			std::lock_guard<std::mutex> l(lock_);
			rest_text_ = text;
			load_rest_ = true;
			start_rest_ = false;
		}
		wake_.notify_one();
	}
	// A "Set" finished here but not in the settings yet.
	bool rest_pending() const
	{
		std::lock_guard<std::mutex> l(lock_);
		return pending_ || start_rest_ || setting_;
	}
	void rest_saved()
	{
		std::lock_guard<std::mutex> l(lock_);
		pending_ = false;
	}

	struct State {
		Face face;
		Pieces pieces;   // your eyes and mouth in that picture
		uint64_t ns = 0; // when the picture `face` is from was taken (give's `ns`)
		Rest rest;
		bool ready = false, setting = false;
		std::string problem;
		uint64_t looks = 0;
		float rate = 0, ms = 0;
	};
	void state(State &out) const
	{
		std::lock_guard<std::mutex> l(lock_);
		out.face = face_;
		out.pieces = pieces_;
		out.ns = face_ns_;
		out.rest = rest_;
		out.ready = ready_;
		out.setting = setting_ || start_rest_;
		out.problem = problem_;
		out.looks = looks_;
		out.rate = rate_;
		out.ms = ms_;
	}

private:
	void run()
	{
		Tracker tracker;
		std::string problem;
		const bool ok = tracker.load_file(models_, &problem);
		if (!ok)
			obs_log(LOG_WARNING, "DireBot Face Avatar: %s", problem.c_str());
		{
			std::lock_guard<std::mutex> l(lock_);
			ready_ = ok;
			problem_ = problem;
		}
		std::vector<uint8_t> rgba;
		Pieces pieces;
		uint64_t window_start = os_gettime_ns();
		int window_looks = 0;
		double window_ms = 0;
		for (;;) {
			int w = 0, h = 0;
			uint64_t ns = 0;
			bool look = false, forget = false, start = false, load = false;
			std::string text;
			float aspect[PARTS], soft;
			{
				std::unique_lock<std::mutex> l(lock_);
				wake_.wait(l, [this] { return stop_ || have_ || load_rest_ || start_rest_; });
				if (stop_)
					return;
				if (load_rest_) {
					load = true;
					text.swap(rest_text_);
					load_rest_ = false;
				}
				if (start_rest_) {
					start = true;
					start_rest_ = false;
					setting_ = true;
				}
				if (have_) {
					rgba.swap(in_);
					w = in_w_;
					h = in_h_;
					ns = in_ns_;
					have_ = false;
					look = true;
				}
				forget = forget_;
				forget_ = false;
				std::copy(aspect_, aspect_ + PARTS, aspect);
				soft = soft_;
			}
			if (load) {
				if (text.empty())
					tracker.clear_rest();
				else if (!tracker.load_rest(text))
					obs_log(LOG_WARNING,
						"DireBot Face Avatar: its \"Set\" couldn't be read; press Set again in DireBot.");
			}
			if (start)
				tracker.set_rest();
			if (forget)
				tracker.forget();
			tracker.set_smoothing(smoothing_);
			const bool was_setting = tracker.setting();
			if (look && ok) {
				const Frame frame{rgba.data(), w, h, w * 4, false};
				tracker.look(frame, (double)ns / 1e9);
				cut_pieces(tracker.face(), aspect, soft, pieces);
				skin_round(frame, pieces);
			}
			std::string finished;
			if (was_setting && !tracker.setting() && tracker.has_rest())
				finished = tracker.save_rest();
			const uint64_t now = os_gettime_ns();
			if (look && ok) {
				window_looks++;
				window_ms += tracker.face().ms;
			}
			{
				std::lock_guard<std::mutex> l(lock_);
				if (look || load || forget)
					face_ = tracker.face();
				if (look) {
					face_ns_ = ns;
					pieces_ = pieces;
				}
				rest_ = tracker.rest();
				setting_ = tracker.setting();
				if (look)
					looks_++;
				if (!finished.empty())
					pending_ = true;
				if (now - window_start >= 2'000'000'000ULL) {
					rate_ = window_looks * 1e9f / (float)(now - window_start);
					ms_ = window_looks ? (float)(window_ms / window_looks) : 0;
					window_start = now;
					window_looks = 0;
					window_ms = 0;
				}
			}
			if (!finished.empty() && done_)
				done_(finished);
		}
	}

	const std::string models_;
	const std::function<void(const std::string &)> done_;
	std::atomic<float> smoothing_{0.5f};
	mutable std::mutex lock_;
	std::condition_variable wake_;
	std::thread thread_;
	bool stop_ = false, have_ = false, forget_ = false, load_rest_ = false, start_rest_ = false;
	bool pending_ = false, setting_ = false;
	std::vector<uint8_t> in_;
	int in_w_ = 0, in_h_ = 0;
	uint64_t in_ns_ = 0;
	std::string rest_text_;
	Face face_;
	Pieces pieces_;
	float aspect_[PARTS] = {0.7f, 0.7f, 0.5f}, soft_ = 0.35f;
	uint64_t face_ns_ = 0;
	Rest rest_;
	uint64_t looks_ = 0;
	bool ready_ = false;
	std::string problem_;
	float rate_ = 0, ms_ = 0;
};

// Where the face models are: data/face, next to the add-on.
std::string models_path()
{
	char *path = obs_module_file("face/face_landmarker.task");
	std::string out = path ? path : "";
	bfree(path);
	return out;
}

void save_rest_task(void *param);

struct FaceAvatar {
	obs_source_t *self = nullptr;
	obs_weak_source_t *weak_self = nullptr;

	// Set on OBS's UI thread (update), read while drawing.
	std::mutex lock, find_lock;
	std::string camera_name;
	obs_weak_source_t *camera = nullptr;
	// Its size from the settings - or, worn on your head, the camera's.
	std::atomic<uint32_t> width{1080}, height{1080};
	std::atomic<uint32_t> cam_w{0}, cam_h{0};
	// The pictures: loaded on the UI thread, drawn on the graphics one.
	std::mutex pictures_lock;
	PicturePtr body, body_left, body_right, own_face;
	// Its own drawn face (each the character's size, or where
	// `drawn_places` puts it): both eyes open, half shut, shut; the mouth.
	PicturePtr eyes[EYE_LOOKS], mouths[MOUTH_LOOKS];
	Rect eye_places[EYE_LOOKS], mouth_places[MOUTH_LOOKS];
	std::vector<Layer> layers;
	Rect spots[PARTS]; // on the character, as the viewer sees them: left eye, right eye, mouth
	bool have_spots = false;
	// What's drawn before it turns (shares of the character): the
	// character, its outfit and its eyes and mouth. Each picture's shape
	// (body, turned left, turned right) is over it, outfit and all.
	Rect canvas{0, 0, 1, 1};
	std::shared_ptr<const Shape> shapes[3];
	std::string shape_key; // what they were worked out for
	std::string rest_seen; // "Set" as last read from the settings or saved to them (under `lock`)
	// The rest are set under `pictures_lock` too.
	float size = 0.85f, part_size = 1.0f, feather = 0.35f;
	// Your skin: how much it takes the character's colour, and the colour
	// (the character's own round each spot, or one picked).
	float skin = 0.8f;
	bool skin_auto = true;
	Lab skin_colour;
	std::atomic<bool> mirror{true};
	float follow_move = 0.6f, follow_grow = 0.6f, follow_tilt = 1.0f;
	float most_move = 0.25f; // of the source's size
	float bend = 0.85f;      // turning and nodding with you
	bool turn_pictures = true;
	std::atomic<bool> easy{false};
	// Your real eyes and mouth, or its own drawn face, moved by yours.
	std::atomic<bool> drawn_style{false};
	// Worn on your head: the source is the camera, with the character on
	// your head (and the camera itself, or not).
	std::atomic<bool> worn{false};
	bool show_camera = true;
	float worn_size = 1.3f;
	// A 3D character (.glb, .gltf, .vrm) instead of the pictures, when
	// there is one: loaded on its own thread, drawn by Avatar3D.
	struct Look3D {
		int turn = 0; // degrees round from the way it most likely faces (guess_turn)
		int look = 0; // 0 its own, 1 glossy, 2 cartoon, 3 flat
		float outline = 0, floppy = 0.5f;
		bool swing = true;
		float wag = 0.5f, wag_speed = 1.5f; // as RigOptions has them
		int framing = 0; // 0 head and shoulders for a VRM, 1 all of it, 2 head
		bool hide = true, smooth = true;
		WornPlace place;
		// Its set-up (DireBot's "model_setup": an imported Lens Studio
		// project, spots dragged into place), and as text, to see changes.
		std::shared_ptr<const CharacterSetup> setup = std::make_shared<CharacterSetup>();
		std::string setup_text;
	};
	Look3D look3d; // under pictures_lock
	ModelLoader loader;

	Worker worker;
	// Only on OBS's graphics thread (video_tick and video_render).
	Shown shown;
	std::atomic<bool> reset_shown{false};
	uint64_t seen_looks = 0;
	Worker::State seen;
	// The camera as it was at each look. The cut-outs come from the very
	// picture the newest look found your face in, so your eyes and mouth
	// sit still in them however fast you move - a newer picture's would
	// already have moved on from where the tracker saw them. The one in
	// use is kept while the face is lost, so they never show anything else.
	struct Shot {
		gs_texrender_t *tex = nullptr;
		uint64_t ns = 0; // when it was taken; 0: nothing in it
	};
	Shot shots[SHOTS];
	int used_shot = -1;   // the cut-outs'
	int looked_shot = -1; // the newest look's, found or not (worn: the camera shown)
	std::atomic<bool> new_camera{false};
	gs_texrender_t *small = nullptr; // the camera, small, for a look
	gs_texrender_t *flat = nullptr;  // the character, flat, before it turns
	gs_texrender_t *out = nullptr;   // the avatar, drawn once a frame
	// The pieces of your eyes and mouth in use (from the look whose shot
	// they're cut from; kept while the face is lost) and their outlines
	// on the graphics card, side by side.
	Pieces held;
	Turn held_turn; // your head's turn then, as the camera sees it
	uint64_t held_ns = 0;
	bool masks_fresh = false;
	gs_texture_t *masks = nullptr;
	// The shapes' depth maps (body, turned left, turned right).
	gs_texture_t *depths[3] = {};
	std::shared_ptr<const Shape> depths_for[3];
	gs_texture_t *no_depth = nullptr; // for a picture with no shape
	Drawn drawn_now;                  // its drawn face's pictures
	gs_stagesurf_t *stage = nullptr;
	uint32_t stage_w = 0, stage_h = 0;
	bool staged = false;
	uint64_t staged_ns = 0;
	int staged_shot = -1;
	std::vector<uint8_t> given; // the last picture that went to the tracker
	LookPace pace;
	std::atomic<float> lead{0.5f}; // the glide's (Shown::set_lead), from the smoothing
	float since_render = 0, since_find = 0;
	float frame_dt = 0; // the last frame's length
	std::vector<uint8_t> rgba;
	gs_effect_t *effect = nullptr;
	gs_eparam_t *p_image = nullptr, *p_opacity = nullptr, *p_clip = nullptr, *p_origin = nullptr,
		    *p_across = nullptr, *p_down = nullptr, *p_mask_map = nullptr, *p_mask_place = nullptr,
		    *p_skin_lab = nullptr, *p_target_lab = nullptr, *p_skin_amount = nullptr, *p_mask_rows = nullptr,
		    *p_body_map = nullptr, *p_body_place = nullptr, *p_inside = nullptr, *p_depth_map = nullptr,
		    *p_char_px = nullptr, *p_pivot = nullptr, *p_turn_k = nullptr, *p_cell = nullptr, *p_pad = nullptr,
		    *p_flip = nullptr, *p_body_origin = nullptr, *p_body_across = nullptr, *p_body_down = nullptr,
		    *p_ring = nullptr, *p_ring_on = nullptr;
	EffectCheck check;
	bool drawing = false, drawn = false, have_out = false;
	std::vector<uint8_t> mask_bytes;
	// What it showed last, for FaceGetState.
	std::mutex state_lock;
	Shown::Out shown_now;
	Worker::State state_now;
	Drawn drawn_state;
	// Where the character goes this frame.
	struct Placed {
		bool shown = false;
		int which = 0;              // its picture: 0 the usual, 1 turned left, 2 turned right
		float x = 0, y = 0;         // where its anchor goes (the source's pixels)
		float ax = 0, ay = 0;       // its anchor (pixels of the canvas, as it's turned)
		float scale = 1, angle = 0; // the source's pixels a character pixel; radians
		float yaw = 0, pitch = 0;   // degrees
		float opacity = 1;
	};
	Placed placed_now;
	bool showing_now = false;
	uint64_t last_render = 0;
	// The 3D character (OBS's graphics thread only)...
	std::unique_ptr<Avatar3D> avatar3d;
	std::unique_ptr<Rig> rig;
	std::shared_ptr<const Model> model_now;
	std::string rig_key;
	M4 worn_pose;
	bool have_worn_pose = false;
	// Where your eyes and mouth go on it (worked out with its rig), and
	// the shape they're cut (under pictures_lock).
	FaceSpots spots3d;
	float aspect3d[PARTS] = {0.65f, 0.65f, 0.55f};
	// ...and what it showed, for FaceGetState (under state_lock).
	struct State3D {
		std::string file, problem, kind, title, author;
		bool loaded = false, shown = false, chain = false, set_up = false;
		size_t points = 0, triangles = 0;
		int springs = 0, tails = 0, arkit = 0, upright = 0, cards = 0, layers = 0;
		// Each spot (its own left eye, right eye, mouth) on its front
		// picture (shares of FaceSpots::box) and how it was found.
		float spot[3][4] = {};
		const char *spot_from[3] = {"", "", ""};
		float yaw = 0, pitch = 0, x = 0, y = 0;
		std::map<std::string, float> values;
	};
	State3D state3d;

	explicit FaceAvatar(obs_source_t *source)
		: self(source),
		  weak_self(obs_source_get_weak_source(source)),
		  worker(models_path(), [weak = weak_self](const std::string &text) {
			  // Saved in the settings on OBS's UI thread (save_rest_task).
			  obs_weak_source_addref(weak);
			  auto *job = new std::pair<obs_weak_source_t *, std::string>(weak, text);
			  obs_queue_task(OBS_TASK_UI, save_rest_task, job, false);
		  })
	{
		skin_colour = lab_of(0.8f, 0.62f, 0.5f);
		char *path = obs_module_file("effects/direbot-face.effect");
		obs_enter_graphics();
		if (path)
			effect = gs_effect_create_from_file(path, nullptr);
		if (effect) {
			auto param = [this](const char *name) {
				return gs_effect_get_param_by_name(effect, name);
			};
			p_image = param("image");
			p_opacity = param("opacity");
			p_clip = param("clip");
			p_origin = param("origin");
			p_across = param("across");
			p_down = param("down");
			p_mask_map = param("mask_map");
			p_mask_place = param("mask_place");
			p_skin_lab = param("skin_lab");
			p_target_lab = param("target_lab");
			p_skin_amount = param("skin_amount");
			p_mask_rows = param("mask_rows");
			p_body_map = param("body_map");
			p_body_place = param("body_place");
			p_inside = param("inside");
			p_depth_map = param("depth_map");
			p_char_px = param("char_px");
			p_pivot = param("pivot");
			p_turn_k = param("turn_k");
			p_cell = param("cell");
			p_pad = param("pad");
			p_flip = param("flip");
			p_body_origin = param("body_origin");
			p_body_across = param("body_across");
			p_body_down = param("body_down");
			p_ring = param("ring");
			p_ring_on = param("ring_on");
			check.load(effect, "2D", face_2d_needs());
			const float zero = 0;
			const uint8_t *zero_data = reinterpret_cast<const uint8_t *>(&zero);
			no_depth = gs_texture_create(1, 1, GS_R32F, 1, &zero_data, 0);
			masks = gs_texture_create(MASK * PARTS, MASK * 2, GS_R8, 1, nullptr, GS_DYNAMIC);
		}
		obs_leave_graphics();
		bfree(path);
		if (!effect)
			obs_log(LOG_WARNING, "DireBot Face Avatar: its effect file is missing, so it shows nothing.");
	}

	~FaceAvatar()
	{
		worker.stop();
		obs_enter_graphics();
		avatar3d.reset();
		for (Shot &shot : shots)
			gs_texrender_destroy(shot.tex);
		gs_texrender_destroy(small);
		gs_texrender_destroy(flat);
		gs_texrender_destroy(out);
		gs_texture_destroy(masks);
		for (gs_texture_t *depth : depths)
			gs_texture_destroy(depth);
		gs_texture_destroy(no_depth);
		gs_stagesurface_destroy(stage);
		gs_effect_destroy(effect);
		obs_leave_graphics();
		obs_weak_source_release(camera);
		obs_weak_source_release(weak_self);
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
			new_camera = true;
		}
		obs_source_release(old);
		obs_source_release(found);
	}

	// Is the picture read back (`line` bytes a row) the one that went to
	// the tracker last? A camera's next picture differs all over (if only
	// by its grain), so this stops at the first row unless it's the same.
	bool same_as_given(const uint8_t *data, uint32_t line) const
	{
		const size_t row = (size_t)stage_w * 4;
		if (given.size() != row * stage_h)
			return false;
		for (uint32_t y = 0; y < stage_h; y++)
			if (std::memcmp(&given[y * row], data + (size_t)y * line, row) != 0)
				return false;
		return true;
	}

	// A copy of the camera for the tracker, once a frame (see LookPace):
	// the one asked for last frame is read back and goes to it if it's
	// new and it's time, and a new one is asked for - the camera at full
	// size into a shot (kept for the cut-outs), then shrunk from that.
	void look(obs_source_t *cam, uint32_t cw, uint32_t ch)
	{
		if (staged && stage) {
			uint8_t *data = nullptr;
			uint32_t line = 0;
			bool gave = false;
			if (gs_stagesurface_map(stage, &data, &line)) {
				if (pace.give(same_as_given(data, line))) {
					const size_t row = (size_t)stage_w * 4;
					given.resize(row * stage_h);
					for (uint32_t y = 0; y < stage_h; y++)
						std::memcpy(&given[y * row], data + (size_t)y * line, row);
					rgba.assign(given.begin(), given.end());
					worker.give(rgba, (int)stage_w, (int)stage_h, staged_ns);
					gave = true;
				}
				gs_stagesurface_unmap(stage);
			}
			// Its shot is free again.
			if (!gave && staged_shot >= 0 && staged_shot != used_shot)
				shots[staged_shot].ns = 0;
			staged = false;
		}
		const double fit = std::min(1.0, (double)LOOK_SIZE / std::max(cw, ch));
		const uint32_t sw = std::max<uint32_t>(32, (uint32_t)std::lround(cw * fit) & ~1u);
		const uint32_t sh = std::max<uint32_t>(32, (uint32_t)std::lround(ch * fit) & ~1u);
		if (!stage || stage_w != sw || stage_h != sh) {
			gs_stagesurface_destroy(stage);
			stage = gs_stagesurface_create(sw, sh, GS_RGBA);
			stage_w = sw;
			stage_h = sh;
			worker.forget();
		}
		// The oldest shot that isn't in use.
		int slot = -1;
		for (int i = 0; i < SHOTS; i++)
			if (i != used_shot && (slot < 0 || shots[i].ns < shots[slot].ns))
				slot = i;
		Shot &shot = shots[slot];
		if (!shot.tex)
			shot.tex = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		if (!small)
			small = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		if (!stage || !small || !shot.tex)
			return;
		vec4 clear;
		vec4_zero(&clear);
		gs_texrender_reset(shot.tex);
		if (!gs_texrender_begin(shot.tex, cw, ch))
			return;
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)cw, 0.0f, (float)ch, -100.0f, 100.0f);
		obs_source_video_render(cam);
		gs_texrender_end(shot.tex);
		const uint64_t ns = os_gettime_ns();
		shot.ns = ns;
		gs_texture_t *full = gs_texrender_get_texture(shot.tex);
		gs_texrender_reset(small);
		if (full && gs_texrender_begin(small, sw, sh)) {
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho(0.0f, (float)sw, 0.0f, (float)sh, -100.0f, 100.0f);
			gs_effect_t *base = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_effect_set_texture(gs_effect_get_param_by_name(base, "image"), full);
			while (gs_effect_loop(base, "Draw"))
				gs_draw_sprite(full, 0, sw, sh);
			gs_texrender_end(small);
			gs_stage_texture(stage, gs_texrender_get_texture(small));
			staged = true;
			staged_ns = ns;
			staged_shot = slot;
		}
	}

	// The shots in use: the newest look's (the camera, worn on your head)
	// and, if it found your face, the one the cut-outs come from, with the
	// pieces of your eyes and mouth in it (else the last ones are kept).
	void pick_shots()
	{
		if (new_camera.exchange(false)) {
			used_shot = -1;
			for (Shot &shot : shots)
				shot.ns = 0;
			held = Pieces();
			held_ns = 0;
		}
		looked_shot = -1;
		if (seen.ns)
			for (int i = 0; i < SHOTS; i++)
				if (shots[i].tex && shots[i].ns == seen.ns)
					looked_shot = i;
		if (looked_shot >= 0 && seen.face.found && seen.pieces.found) {
			used_shot = looked_shot;
			if (held_ns != seen.ns) {
				held = seen.pieces;
				held_turn = turn_of(seen.face.pose);
				held_ns = seen.ns;
				masks_fresh = true;
			}
		}
	}

	// The pieces' soft outlines to the graphics card, side by side.
	void upload_masks()
	{
		if (!masks_fresh || !masks)
			return;
		masks_fresh = false;
		mask_bytes.resize((size_t)MASK * PARTS * MASK * 2);
		for (int part = 0; part < PARTS; part++)
			for (int y = 0; y < MASK; y++) {
				std::memcpy(&mask_bytes[(size_t)y * MASK * PARTS + (size_t)part * MASK],
					    &held.part[part].mask[y * MASK], MASK);
				std::memcpy(&mask_bytes[(size_t)(y + MASK) * MASK * PARTS + (size_t)part * MASK],
					    &held.part[part].inner[y * MASK], MASK);
			}
		gs_texture_set_image(masks, mask_bytes.data(), MASK * PARTS, false);
	}

	// A picture at (x, y), w x h - only from `from` to `to` across it.
	void draw_picture(gs_texture_t *tex, float x, float y, float w, float h, float opacity, float from = 0,
			  float to = 1)
	{
		from = clampf(from, 0, 1);
		to = clampf(to, 0, 1);
		if (!tex || opacity <= 0.001f || w <= 0 || h <= 0 || to <= from)
			return;
		vec4 clip;
		vec4_set(&clip, from, 0, to, 1);
		gs_effect_set_texture(p_image, tex);
		gs_effect_set_float(p_opacity, opacity);
		gs_effect_set_vec4(p_clip, &clip);
		gs_matrix_push();
		gs_matrix_translate3f(x, y, 0);
		check("Picture");
		while (gs_effect_loop(effect, "Picture"))
			gs_draw_quadf(tex, 0, w, h);
		gs_matrix_pop();
	}
	void draw_picture(const PicturePtr &pic, const Rect &place, float char_w, float char_h, float opacity,
			  float from = 0, float to = 1)
	{
		if (loaded(pic))
			draw_picture(pic->texture(), place.x * char_w, place.y * char_h, place.w * char_w,
				     place.h * char_h, opacity, from, to);
	}

	// One of your eyes or your mouth (`part`, as the camera has them),
	// from the camera's picture onto the character's spot `spot`: on top
	// of it, kept inside its outline (`inside`), or behind it.
	void draw_piece(gs_texture_t *cam, uint32_t cw, uint32_t ch, int part, int spot, const Picture &pic,
			const Shape *shape, bool inside, float opacity)
	{
		const Piece &p = held.part[part];
		if (!cam || opacity <= 0.001f || p.w <= 0 || p.h <= 0)
			return;
		// In the camera's picture: the piece, turned with your eyes.
		const float W = (float)cw, H = (float)ch;
		const float pw = p.w * W, ph = p.h * W;
		const float co = std::cos(p.angle), si = std::sin(p.angle);
		float ax = co * pw, ay = si * pw, dx = -si * ph, dy = co * ph;
		float ox = p.x * W - ax / 2 - dx / 2, oy = p.y * H - ay / 2 - dy / 2;
		if (mirror) {
			ox += ax;
			oy += ay;
			ax = -ax;
			ay = -ay;
		}
		vec2 origin, across, down;
		vec2_set(&origin, ox / W, oy / H);
		vec2_set(&across, ax / W, ay / H);
		vec2_set(&down, dx / W, dy / H);
		// On the character: round its spot's middle, with room for the
		// soft edge, made bigger or smaller by "Eye and mouth size" (and
		// bigger with a wide-open mouth, which is cut bigger).
		const float char_w = pic.width(), char_h = pic.height();
		const Rect &r = spots[spot];
		const float grow = part_size * PIECE_ROOM * std::max(1.0f, p.grown);
		const float sw = r.w * char_w * grow, sh = r.h * char_h * grow;
		const float sx = (r.x + r.w / 2) * char_w - sw / 2, sy = (r.y + r.h / 2) * char_h - sh / 2;
		// Its masks, from the middle of their first points to their last.
		vec4 mask_place, body_place;
		vec2 mask_rows;
		vec4_set(&mask_place, (part * MASK + 0.5f) / (MASK * PARTS), (MASK - 1.0f) / (MASK * PARTS),
			 mirror ? 1.0f : 0.0f, 0);
		vec2_set(&mask_rows, 0.5f / (2 * MASK), (MASK - 1.0f) / (2 * MASK));
		vec4_set(&body_place, sx / char_w, sy / char_h, sw / char_w, sh / char_h);
		// Your skin takes the character's colour round the spot (or the
		// one picked); your eyes, lips and teeth keep theirs.
		const Lab target = skin_auto && shape ? shape->colour[spot] : skin_colour;
		const float amount = skin_auto && !shape ? 0 : skin;
		vec3 skin_lab, target_lab;
		vec3_set(&skin_lab, p.skin.l, p.skin.a, p.skin.b);
		vec3_set(&target_lab, target.l, target.a, target.b);
		gs_effect_set_texture(p_image, cam);
		gs_effect_set_texture(p_mask_map, masks);
		gs_effect_set_texture(p_body_map, pic.texture());
		gs_effect_set_float(p_opacity, opacity);
		gs_effect_set_vec2(p_origin, &origin);
		gs_effect_set_vec2(p_across, &across);
		gs_effect_set_vec2(p_down, &down);
		gs_effect_set_vec4(p_mask_place, &mask_place);
		gs_effect_set_vec4(p_body_place, &body_place);
		gs_effect_set_float(p_inside, inside ? 1.0f : 0.0f);
		gs_effect_set_vec3(p_skin_lab, &skin_lab);
		gs_effect_set_vec3(p_target_lab, &target_lab);
		gs_effect_set_vec2(p_mask_rows, &mask_rows);
		gs_effect_set_float(p_skin_amount, amount);
		gs_matrix_push();
		gs_matrix_translate3f(sx, sy, 0);
		check("Cutout");
		while (gs_effect_loop(effect, "Cutout"))
			gs_draw_quadf(nullptr, 0, sw, sh);
		gs_matrix_pop();
	}

	// Its own drawn face as `d` says: each eye's picture (split down the
	// middle between the eyes when they differ, for a wink) and the
	// mouth's. False if it has none of them.
	bool draw_drawn(const Drawn &d, float char_w, float char_h, float opacity)
	{
		unsigned have_eyes = 0, have_mouths = 0;
		for (int i = 0; i < EYE_LOOKS; i++)
			if (loaded(eyes[i]))
				have_eyes |= 1u << i;
		for (int i = 0; i < MOUTH_LOOKS; i++)
			if (loaded(mouths[i]))
				have_mouths |= 1u << i;
		if (!have_eyes && !have_mouths)
			return false;
		if (opacity <= 0.001f)
			return true;
		const int left = eye_picture(d.eye[0], have_eyes), right = eye_picture(d.eye[1], have_eyes);
		const float split = have_spots ? (spots[0].x + spots[0].w / 2 + spots[1].x + spots[1].w / 2) / 2 : 0.5f;
		if (left >= 0 && left == right) {
			draw_picture(eyes[left], eye_places[left], char_w, char_h, opacity);
		} else {
			if (left >= 0) {
				const Rect &r = eye_places[left];
				draw_picture(eyes[left], r, char_w, char_h, opacity, 0, (split - r.x) / r.w);
			}
			if (right >= 0) {
				const Rect &r = eye_places[right];
				draw_picture(eyes[right], r, char_w, char_h, opacity, (split - r.x) / r.w, 1);
			}
		}
		const int mouth = mouth_picture(d.mouth, have_mouths);
		if (mouth >= 0)
			draw_picture(mouths[mouth], mouth_places[mouth], char_w, char_h, opacity);
		return true;
	}

	Placed place(const Shown::Out &o, const Picture &pic, int which, const Shape *shape, bool wear, uint32_t w,
		     uint32_t h)
	{
		Placed at;
		at.which = which;
		const float char_w = pic.width(), char_h = pic.height();
		const float CX = canvas.x * char_w, CY = canvas.y * char_h, CW = canvas.w * char_w,
			    CH = canvas.h * char_h;
		const float flip = mirror ? -1.0f : 1.0f;
		const bool shaped = shape && shape->ready();
		if (!wear) {
			// In its own spot: sized to fit, then following you - moved,
			// grown, tilted, turned and nodded as you are against "Set",
			// within limits, gliding between looks.
			const Shown::Glide &g = o.glide;
			const float fit = std::min(w / char_w, h / char_h) * size;
			const float dx = clampf((g.cx - o.home_x) * follow_move * flip, -most_move, most_move);
			const float dy = clampf((g.cy - o.home_y) * follow_move, -most_move, most_move);
			const float grow = clampf(std::pow(std::max(0.05f, g.scale), follow_grow), 0.6f, 1.6f);
			at.shown = true;
			at.x = w * (0.5f + dx);
			at.y = h * (0.5f + dy);
			at.ax = char_w / 2 - CX;
			at.ay = char_h / 2 - CY;
			at.scale = fit * grow;
			at.angle = clampf(g.angle * follow_tilt * flip, -0.6f, 0.6f);
			if (shaped) {
				// A turned picture is already turned some of the way.
				float yaw = g.yaw * flip;
				if (which)
					yaw -= (which == 1 ? -1 : 1) * Shown::TURN_ON;
				at.yaw = clampf(yaw * bend, -MOST_YAW, MOST_YAW);
				at.pitch = clampf(g.pitch * bend * 0.6f, -MOST_PITCH, MOST_PITCH);
			}
			return at;
		}
		// Worn on your head: its eyes on yours, sized from how far apart
		// yours are, tilted, turned and nodded as your head is.
		if (!held.found)
			return at;
		const Piece &a = held.part[FIRST_EYE], &b = held.part[SECOND_EYE];
		at.x = (a.x + b.x) / 2 * w;
		at.y = (a.y + b.y) / 2 * h;
		if (mirror)
			at.x = w - at.x;
		float ex = char_w / 2, ey = char_h * 0.45f, gap = 0.3f * char_w;
		if (have_spots) {
			const float lx = (spots[0].x + spots[0].w / 2) * char_w,
				    ly = (spots[0].y + spots[0].h / 2) * char_h;
			const float rx = (spots[1].x + spots[1].w / 2) * char_w,
				    ry = (spots[1].y + spots[1].h / 2) * char_h;
			ex = (lx + rx) / 2;
			ey = (ly + ry) / 2;
			gap = std::max(1.0f, std::hypot(rx - lx, ry - ly));
		}
		if (shaped) {
			at.yaw = clampf(held_turn.yaw * flip, -MOST_YAW, MOST_YAW);
			at.pitch = clampf(held_turn.pitch, -WORN_PITCH, WORN_PITCH);
		}
		// Its eyes' middle, as the turn moves it.
		at.ax = ex - CX;
		at.ay = ey - CY;
		if (shaped)
			turned_point(*shape, CW, CH, at.yaw * RAD, at.pitch * RAD, ex - CX, ey - CY, at.ax, at.ay);
		at.scale = worn_size * held.gap * w / gap;
		at.angle = a.angle * flip;
		const float t = clampf((o.lost_for - WORN_HOLD) / WORN_FADE, 0, 1);
		at.opacity = 1 - t * t * (3 - 2 * t);
		at.shown = at.opacity > 0.001f;
		return at;
	}

	// The character before it turns, into `flat`: its outfit, itself and
	// its face - your real eyes and mouth, or its drawn ones - at about
	// `scale` pixels a character pixel. Its texture, or none.
	gs_texture_t *draw_flat(gs_texture_t *cam, uint32_t cw, uint32_t ch, const Shown::Out &o, const Picture &pic,
				const Shape *shape, float scale)
	{
		const float char_w = pic.width(), char_h = pic.height();
		const float CX = canvas.x * char_w, CY = canvas.y * char_h, CW = canvas.w * char_w,
			    CH = canvas.h * char_h;
		// Its size goes in steps, so it isn't made anew every frame as
		// you lean in.
		float k = std::exp2(std::ceil(std::log2(std::max(scale, 1e-3f)) * 4) / 4);
		k = std::min(k, FLAT_MOST / std::max(CW, CH));
		const uint32_t fw = std::max(1u, (uint32_t)std::lround(CW * k)),
			       fh = std::max(1u, (uint32_t)std::lround(CH * k));
		if (!flat)
			flat = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		if (!flat)
			return nullptr;
		gs_texrender_reset(flat);
		if (!gs_texrender_begin(flat, fw, fh))
			return nullptr;
		vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(CX, CX + CW, CY, CY + CH, -100.0f, 100.0f);
		// Straight colour in, premultiplied out.
		gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		const Rect whole{0, 0, 1, 1};
		for (const Layer &layer : layers)
			if (layer.behind)
				draw_picture(layer.picture, layer.place, char_w, char_h, 1);
		const bool real = !drawn_style;
		const float showing = real && seen.ready && have_spots && cam && held.found ? o.shown : 0;
		// The character's spot for each of yours: the camera's first eye
		// is on the viewer's left - unless mirrored.
		int spot_of[PARTS];
		bool behind[PARTS];
		for (int part = 0; part < PARTS; part++) {
			spot_of[part] = part == MOUTH ? MOUTH : mirror ? (part == FIRST_EYE ? 1 : 0) : part;
			behind[part] = shape && shape->hole[spot_of[part]];
		}
		// Where it's see-through, yours show from behind it.
		if (showing > 0)
			for (int part = 0; part < PARTS; part++)
				if (behind[part])
					draw_piece(cam, cw, ch, part, spot_of[part], pic, shape, false, showing);
		draw_picture(pic.texture(), 0, 0, char_w, char_h, 1);
		if (showing > 0)
			for (int part = 0; part < PARTS; part++)
				if (!behind[part])
					draw_piece(cam, cw, ch, part, spot_of[part], pic, shape, true, showing);
		// Its own face: drawn, moved by yours - or while yours isn't showing.
		const float face = real ? 1 - showing : 1;
		if (!draw_drawn(real ? Drawn() : drawn_now, char_w, char_h, face))
			draw_picture(own_face, whole, char_w, char_h, face);
		for (const Layer &layer : layers)
			if (!layer.behind)
				draw_picture(layer.picture, layer.place, char_w, char_h, 1);
		gs_texrender_end(flat);
		return gs_texrender_get_texture(flat);
	}

	// A shape's depth map on the graphics card.
	gs_texture_t *depth_texture(int which, const std::shared_ptr<const Shape> &shape)
	{
		if (!shape || !shape->ready())
			return no_depth;
		if (depths_for[which] != shape) {
			gs_texture_destroy(depths[which]);
			const uint8_t *data = reinterpret_cast<const uint8_t *>(shape->depth.data());
			depths[which] = gs_texture_create((uint32_t)shape->w, (uint32_t)shape->h, GS_R32F, 1, &data, 0);
			depths_for[which] = shape;
		}
		return depths[which] ? depths[which] : no_depth;
	}

	// The flat character, turned and nodded (each point slid by how far
	// it stands out) and placed.
	void draw_turned(gs_texture_t *flat_tex, const std::shared_ptr<const Shape> &shape, const Picture &pic,
			 const Placed &at)
	{
		const float CW = canvas.w * pic.width(), CH = canvas.h * pic.height();
		const bool shaped = shape && shape->ready();
		const float yaw = at.yaw * RAD, pitch = at.pitch * RAD;
		vec2 char_px, pivot, cell;
		vec4 turn_k;
		vec2_set(&char_px, CW, CH);
		vec2_set(&pivot, (shaped ? shape->pivot_x : 0.5f) * CW, (shaped ? shape->pivot_y : 0.5f) * CH);
		vec2_set(&cell, shaped ? CW / shape->w : CW, shaped ? CH / shape->h : CH);
		vec4_set(&turn_k, 1 - FORESHORTEN * (1 - std::cos(yaw)), 1 - FORESHORTEN * (1 - std::cos(pitch)),
			 std::sin(yaw), std::sin(pitch));
		gs_effect_set_texture(p_image, flat_tex);
		gs_effect_set_texture(p_depth_map, depth_texture(at.which, shape));
		gs_effect_set_float(p_opacity, at.opacity);
		gs_effect_set_vec2(p_char_px, &char_px);
		gs_effect_set_vec2(p_pivot, &pivot);
		gs_effect_set_vec2(p_cell, &cell);
		gs_effect_set_vec4(p_turn_k, &turn_k);
		gs_effect_set_float(p_pad, TURN_PAD);
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		gs_matrix_push();
		gs_matrix_translate3f(at.x, at.y, 0);
		gs_matrix_rotaa4f(0, 0, 1, at.angle);
		gs_matrix_scale3f(at.scale, at.scale, 1);
		gs_matrix_translate3f(-at.ax - TURN_PAD * CW, -at.ay - TURN_PAD * CH, 0);
		check("Turn");
		while (gs_effect_loop(effect, "Turn"))
			gs_draw_quadf(nullptr, 0, CW * (1 + 2 * TURN_PAD), CH * (1 + 2 * TURN_PAD));
		gs_matrix_pop();
	}

	// The camera, worn on your head: the very picture your eyes were
	// found in, so the character sits on them exactly (with "Easy on my
	// PC" that's only 15 a second, so then the camera as it is now).
	void draw_camera(obs_source_t *cam, uint32_t cw, uint32_t ch)
	{
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		gs_texture_t *tex = !easy && looked_shot >= 0 ? gs_texrender_get_texture(shots[looked_shot].tex)
							      : nullptr;
		if (tex) {
			gs_effect_set_texture(p_image, tex);
			gs_effect_set_float(p_flip, mirror ? 1.0f : 0.0f);
			check("Camera");
			while (gs_effect_loop(effect, "Camera"))
				gs_draw_quadf(tex, 0, (float)cw, (float)ch);
			return;
		}
		gs_matrix_push();
		if (mirror) {
			gs_matrix_translate3f((float)cw, 0, 0);
			gs_matrix_scale3f(-1, 1, 1);
		}
		obs_source_video_render(cam);
		gs_matrix_pop();
	}

	// Which way the 3D character faces: as set, or as it is (a VRM 0.x
	// faces away, so it's turned round).
	Quat facing_of(const Model &m) const
	{
		// As its set-up says, or turned from the way it most likely faces
		// - and turned further, if asked.
		if (look3d.setup->has_facing)
			return axis_angle({0, 1, 0}, std::max(0, look3d.turn) * RAD) * look3d.setup->facing;
		const int turn = guess_turn(m) + std::max(0, look3d.turn);
		return axis_angle({0, 1, 0}, turn * RAD);
	}

	// A newly loaded 3D character onto the graphics card, and its rig
	// (made anew when how it swings or faces changes).
	void pick_model()
	{
		if (std::shared_ptr<ModelPack> pack = loader.take()) {
			if (!avatar3d)
				avatar3d = std::make_unique<Avatar3D>();
			avatar3d->set(pack->model, std::move(pack->pictures));
			model_now = pack->model;
			rig.reset();
			rig_key.clear();
			have_worn_pose = false;
			State3D st;
			st.file = pack->path;
			st.problem = pack->problem;
			if (model_now) {
				const Model &m = *model_now;
				st.loaded = true;
				st.kind = m.vrm == 1 ? "vrm1" : m.vrm == -1 ? "vrm0" : "gltf";
				st.title = m.meta.title;
				st.author = m.meta.author;
				st.points = m.points;
				st.triangles = m.triangles;
			}
			std::lock_guard<std::mutex> l(state_lock);
			state3d = st;
		}
		if (!model_now || !avatar3d || !avatar3d->ready()) {
			rig.reset();
			return;
		}
		RigOptions ro;
		ro.facing = facing_of(*model_now);
		ro.floppy = look3d.floppy;
		ro.springs = look3d.swing;
		ro.setup = look3d.setup;
		ro.wag = look3d.wag;
		ro.wag_speed = look3d.wag_speed;
		const std::string key = std::to_string(ro.facing.x) + "/" + std::to_string(ro.facing.y) + "/" +
					std::to_string(ro.facing.z) + "/" + std::to_string(ro.facing.w) + "/" +
					std::to_string(ro.floppy) + "/" + (ro.springs ? "1" : "0") + "/" +
					look3d.setup_text;
		if (rig)
			rig->set_wag(ro.wag, ro.wag_speed);
		if (!rig || key != rig_key) {
			rig = std::make_unique<Rig>(model_now, ro);
			rig_key = key;
			spots3d = face_spots(model_now, ro.facing, *look3d.setup);
			// Your eyes and mouth are cut the shape of its spots.
			const float eye = (spots3d.spot[0].h / std::max(1e-6f, spots3d.spot[0].w) +
					   spots3d.spot[1].h / std::max(1e-6f, spots3d.spot[1].w)) /
					  2;
			const float aspect[PARTS] = {clampf(eye, 0.1f, 4.0f), clampf(eye, 0.1f, 4.0f),
						     clampf(spots3d.spot[2].h / std::max(1e-6f, spots3d.spot[2].w),
							    0.1f, 4.0f)};
			std::copy(aspect, aspect + PARTS, aspect3d);
			worker.set_cut(aspect, feather);
			std::lock_guard<std::mutex> l(state_lock);
			state3d.springs = rig->springs();
			state3d.tails = rig->tails();
			state3d.chain = rig->auto_chain();
			state3d.arkit = rig->arkit_parts();
			state3d.upright = rig->upright_parts();
			state3d.set_up = !look3d.setup->empty();
			const float *box = spots3d.box;
			for (int i = 0; i < 3; i++) {
				const FaceSpot &sp = spots3d.spot[i];
				const V3 f = rotate(ro.facing, sp.at);
				state3d.spot[i][0] = (f.x - box[0]) / std::max(1e-6f, box[2]);
				state3d.spot[i][1] = (box[1] - f.y) / std::max(1e-6f, box[3]);
				state3d.spot[i][2] = sp.w / std::max(1e-6f, box[2]);
				state3d.spot[i][3] = sp.h / std::max(1e-6f, box[2]);
				state3d.spot_from[i] = spots3d.from[i];
			}
		}
	}

	// The 3D character into `out`: posed by your face and drawn - in its
	// own spot, following you as the pictures do; or worn on your head,
	// sat on it as your face's pose puts it, over the camera.
	void draw_3d(obs_source_t *cam, uint32_t cw, uint32_t ch, uint32_t w, uint32_t h, const Shown::Out &o,
		     bool wear)
	{
		const Model &m = *model_now;
		const Quat facing = facing_of(m);
		Drive d;
		d.dt = frame_dt;
		std::copy(o.moves, o.moves + EXPRESSIONS, d.moves);
		Framing f;
		View3D v;
		bool show = true;
		float opacity = 1;
		if (!wear) {
			// Mirrored: it moves as your reflection does (not mirrored itself).
			if (mirror)
				swap_sides(d.moves);
			const Shown::Glide &g = o.glide;
			const float flip = mirror ? -1.0f : 1.0f;
			const float dx = clampf((g.cx - o.home_x) * follow_move * flip, -most_move, most_move);
			const float dy = clampf((g.cy - o.home_y) * follow_move, -most_move, most_move);
			const float grow = clampf(std::pow(std::max(0.05f, g.scale), follow_grow), 0.6f, 1.6f);
			const bool head = look3d.framing == 2 || (look3d.framing == 0 && m.is_vrm());
			f = frame_own_spot(m, facing, w, h, size, head, dx, dy, grow);
			d.head = head_turn(clampf(g.yaw * bend, -MOST_YAW_3D, MOST_YAW_3D),
					   clampf(g.pitch * bend, -MOST_PITCH_3D, MOST_PITCH_3D),
					   clampf(g.angle * follow_tilt, -0.6f, 0.6f), mirror);
			v.w = w;
			v.h = h;
		} else {
			// Your face's pose in the very picture shown (held a moment
			// when it's lost, then faded).
			if (looked_shot >= 0 && seen.face.found) {
				worn_pose = matrix_of_rows(seen.face.pose);
				have_worn_pose = true;
			}
			const float t = clampf((o.lost_for - WORN_HOLD) / WORN_FADE, 0, 1);
			opacity = 1 - t * t * (3 - 2 * t);
			show = have_worn_pose && opacity > 0.001f;
			f = frame_worn(m, facing, look3d.place, worn_pose, (float)cw / std::max(1u, ch));
			v.w = cw;
			v.h = ch;
			v.hide_behind_head = look3d.hide;
			v.head = worn_pose;
		}
		d.place = f.place;
		d.upright = f.upright;
		d.has_upright = true;
		if (rig->turns_whole())
			rig->set_pivot(turn_pivot(m, facing, f));
		// Your real eyes and mouth on it: its own stay open and shut under them.
		gs_texture_t *cut = cam && used_shot >= 0 ? gs_texrender_get_texture(shots[used_shot].tex) : nullptr;
		const float showing = !drawn_style && seen.ready && cut && held.found ? o.shown : 0;
		if (showing > 0.5f) {
			quiet_face(d.moves);
			d.hide_own_face = true;
		}
		rig->pose(d);
		v.fov = f.fov;
		v.near = f.near;
		v.far = f.far;
		v.view = f.view;
		v.world_scale = f.world_scale;
		v.height = f.height;
		v.look = look3d.look;
		v.outline = look3d.outline;
		v.smooth = look3d.smooth && !easy;
		v.opacity = opacity;
		gs_texture_t *tex = show ? avatar3d->draw(*rig, v) : nullptr;
		{
			std::lock_guard<std::mutex> l(state_lock);
			state3d.shown = tex != nullptr;
			state3d.values = rig->expression_values();
			const V3 front = rotate(d.head, {0, 0, 1});
			state3d.yaw = std::atan2(front.x, front.z) / RAD;
			state3d.pitch = std::asin(clampf(front.y, -1, 1)) / RAD;
			// Where its middle is in the picture (pixels).
			const V3 mid = point(f.place, (m.low + m.high) * 0.5f);
			const V3 c = point(f.view, mid);
			const float k = 1 / std::tan(f.fov / 2);
			const float aspect = (float)v.w / std::max(1u, v.h);
			if (c.z < -1e-4f) {
				const float sx = (c.x * k / aspect) / -c.z, sy = (c.y * k) / -c.z;
				state3d.x = (wear && mirror ? -sx : sx) * 0.5f * v.w + 0.5f * v.w;
				state3d.y = -sy * 0.5f * v.h + 0.5f * v.h;
			}
		}
		{
			std::lock_guard<std::mutex> l(state_lock);
			placed_now = Placed();
			placed_now.shown = tex != nullptr;
			placed_now.opacity = opacity;
		}
		if (!out)
			out = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		if (!out)
			return;
		gs_texrender_reset(out);
		if (!gs_texrender_begin(out, w, h))
			return;
		vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		if (wear && show_camera)
			draw_camera(cam, cw, ch);
		int cards = 0, drawn_layers = 0;
		if (tex) {
			const M4 frame = rig->head_frame();
			drawn_layers += draw_layers_3d(true, v, frame, facing, wear, opacity);
			// Worn, it's drawn as the camera sees you: mirrored with it.
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
			gs_effect_t *base = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_effect_set_texture(gs_effect_get_param_by_name(base, "image"), tex);
			while (gs_effect_loop(base, "Draw"))
				gs_draw_sprite(tex, wear && mirror ? GS_FLIP_U : 0, w, h);
			if (showing * opacity > 0.001f)
				cards = draw_cards(cut, cw, ch, tex, v, frame, f.upright, facing, wear,
						   showing * opacity);
			drawn_layers += draw_layers_3d(false, v, frame, facing, wear, opacity);
		}
		gs_texrender_end(out);
		have_out = true;
		std::lock_guard<std::mutex> l(state_lock);
		state3d.cards = cards;
		state3d.layers = drawn_layers;
	}

	// What it turns round, whole, in its own spot: where it's put on your
	// head, if that's set; else its set-up's (an imported Lens's head,
	// if that's inside it) or round its own head, behind its eyes; else
	// its middle.
	V3 turn_pivot(const Model &m, Quat facing, const Framing &f) const
	{
		if (!look3d.place.automatic)
			return head_in_model(m, facing, look3d.place);
		const CharacterSetup &su = *look3d.setup;
		if (su.empty())
			return f.pivot;
		const V3 room = (m.high - m.low) * 0.1f;
		const V3 lo = m.low - room, hi = m.high + room;
		const V3 p = su.pivot;
		if (su.has_pivot && p.x >= lo.x && p.y >= lo.y && p.z >= lo.z && p.x <= hi.x && p.y <= hi.y &&
		    p.z <= hi.z)
			return p;
		V3 eyes = rotate(facing, (spots3d.spot[0].at + spots3d.spot[1].at) * 0.5f);
		eyes.z = rotate(facing, (m.low + m.high) * 0.5f).z;
		return rotate(conj(facing), eyes);
	}

	// Its own eyes and mouth stay as they are (your real ones are on them).
	static void quiet_face(float moves[EXPRESSIONS])
	{
		for (int i = 1; i < EXPRESSIONS; i++) {
			const char *n = EXPRESSION_NAMES[i];
			if (std::strncmp(n, "eyeBlink", 8) == 0 || std::strncmp(n, "eyeSquint", 9) == 0 ||
			    std::strncmp(n, "jaw", 3) == 0 || std::strncmp(n, "mouth", 5) == 0)
				moves[i] = 0;
		}
	}

	// Your real eyes and mouth on the 3D character (`body`, as drawn into
	// `out`): each piece of the camera on a card in its spot, riding on
	// its head (`frame`, the model -> the world as its head is now) but
	// turned only half as far as your head (the camera already shows the
	// turn; `unturned`: the model -> the world, your head straight), kept
	// inside its outline, its skin taking the colour round the card. Your
	// far eye fades as you turn from the camera (from 30 degrees, gone by
	// 50), your mouth from 40, and a card turned from the camera goes.
	// How many it drew.
	// Outfit pictures on a 3D character, riding on its head: each where
	// it was put on its front picture (shares of it), just in front of it
	// - or just behind it, for those behind. -> how many were drawn.
	int draw_layers_3d(bool behind, const View3D &v, const M4 &frame, Quat facing, bool wear, float opacity)
	{
		if (layers.empty() || !v.w || !v.h || opacity <= 0.001f)
			return 0;
		const float W = (float)v.w, H = (float)v.h;
		const float k = 1 / std::tan(v.fov / 2), aspect = W / H;
		const Quat back = conj(facing);
		const float *box = spots3d.box;
		const float z = behind ? spots3d.back : spots3d.front;
		int drawn = 0;
		for (const Layer &layer : layers) {
			if (layer.behind != behind || !loaded(layer.picture))
				continue;
			bool ok = true;
			auto at = [&](float sx, float sy) {
				const V3 p{box[0] + sx * box[2], box[1] - sy * box[3], z};
				const V3 c = point(v.view, point(frame, rotate(back, p)));
				const float depth = -c.z;
				if (depth < v.near * 0.5f)
					ok = false;
				return std::array<float, 2>{(c.x * k / aspect / std::max(depth, 1e-6f) + 1) * 0.5f * W,
							    (1 - c.y * k / std::max(depth, 1e-6f)) * 0.5f * H};
			};
			const Rect &r = layer.place;
			const auto o = at(r.x, r.y), right = at(r.x + r.w, r.y), down = at(r.x, r.y + r.h);
			if (!ok)
				continue;
			float ox = o[0], ax = right[0] - o[0], dx = down[0] - o[0];
			if (wear && mirror) {
				ox = W - ox;
				ax = -ax;
				dx = -dx;
			}
			matrix4 m;
			vec4_set(&m.x, ax, right[1] - o[1], 0, 0);
			vec4_set(&m.y, dx, down[1] - o[1], 0, 0);
			vec4_set(&m.z, 0, 0, 1, 0);
			vec4_set(&m.t, ox, o[1], 0, 1);
			gs_texture_t *tex = layer.picture->texture();
			vec4 clip;
			vec4_set(&clip, 0, 0, 1, 1);
			gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE,
						   GS_BLEND_INVSRCALPHA);
			gs_effect_set_texture(p_image, tex);
			gs_effect_set_float(p_opacity, opacity);
			gs_effect_set_vec4(p_clip, &clip);
			gs_matrix_push();
			gs_matrix_mul(&m);
			check("Picture");
			while (gs_effect_loop(effect, "Picture"))
				gs_draw_quadf(tex, 0, 1, 1);
			gs_matrix_pop();
			drawn++;
		}
		return drawn;
	}

	// Its 3D character straight from the front, as it is made (no turn,
	// no expressions, nothing swinging), its front box filling the picture
	// - for DireBot's page to put its eyes, mouth and outfits on. `close`:
	// just the part round its face, for its eyes and mouth up close.
	json front_picture(int want_w, bool close)
	{
		obs_enter_graphics();
		json out;
		{
			std::lock_guard<std::mutex> l(pictures_lock);
			out = draw_front(want_w, close);
		}
		obs_leave_graphics();
		return out;
	}
	json draw_front(int want_w, bool close)
	{
		auto no = [](const char *words) {
			return json{{"ok", false}, {"error", words}};
		};
		if (!model_now || !avatar3d || !avatar3d->ready())
			return no("Its 3D character isn't loaded yet - is the file still there?");
		const Model &m = *model_now;
		RigOptions ro;
		ro.facing = facing_of(m);
		ro.springs = false;
		ro.floppy = look3d.floppy;
		ro.setup = look3d.setup;
		Rig still(model_now, ro);
		const FaceSpots fs = face_spots(model_now, ro.facing, *look3d.setup);
		const float bw = std::max(1e-6f, fs.box[2]), bh = std::max(1e-6f, fs.box[3]);
		// What's drawn: the whole box, or (`close`) the part round its face.
		const FaceRegion region = close ? face_region(fs, ro.facing) : FaceRegion();
		const float rx = fs.box[0] + region.x * bw, ry = fs.box[1] - region.y * bh;
		const float rw = std::max(1e-6f, region.w * bw), rh = std::max(1e-6f, region.h * bh);
		uint32_t W = (uint32_t)std::clamp(want_w, 64, 1024);
		uint32_t H = (uint32_t)std::clamp((int)std::lround(W * rh / rw), 16, 2048);
		if (H > 1024) {
			W = std::max(16u, (uint32_t)std::lround(W * 1024.0f / H));
			H = 1024;
		}
		// Nearly straight on (a narrow view from far away), so its front
		// box is the picture's edges wherever a point is in depth.
		const float fov = 2 * RAD, s = 1 / rh, dist = 0.5f / std::tan(fov / 2);
		const V3 mid{rx + rw / 2, ry - rh / 2, (fs.front + fs.back) / 2};
		Drive d;
		d.place = scaled(s) * moved(mid * -1) * matrix_of(ro.facing);
		still.pose(d);
		View3D v;
		v.w = W;
		v.h = H;
		v.fov = fov;
		v.view = moved({0, 0, -dist});
		const float deep = std::max(1.0f, (fs.front - fs.back) * s);
		v.near = std::max(0.01f, dist - deep * 2);
		v.far = dist + deep * 2;
		v.world_scale = s;
		v.height = 1;
		v.look = look3d.look;
		v.outline = look3d.outline;
		v.smooth = true;
		gs_texture_t *tex = avatar3d->draw(still, v);
		if (!tex)
			return no("It couldn't be drawn.");
		gs_texrender_t *flat_pic = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		gs_stagesurf_t *stage = gs_stagesurface_create(W, H, GS_RGBA);
		std::string rgba;
		if (flat_pic && stage && gs_texrender_begin(flat_pic, W, H)) {
			vec4 clear;
			vec4_zero(&clear);
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho(0.0f, (float)W, 0.0f, (float)H, -100.0f, 100.0f);
			gs_blend_state_push();
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
			gs_effect_t *base = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_effect_set_texture(gs_effect_get_param_by_name(base, "image"), tex);
			while (gs_effect_loop(base, "Draw"))
				gs_draw_sprite(tex, 0, W, H);
			gs_blend_state_pop();
			gs_texrender_end(flat_pic);
			gs_stage_texture(stage, gs_texrender_get_texture(flat_pic));
			uint8_t *data = nullptr;
			uint32_t line = 0;
			if (gs_stagesurface_map(stage, &data, &line)) {
				rgba.resize((size_t)W * H * 4);
				for (uint32_t y = 0; y < H; y++)
					std::memcpy(&rgba[(size_t)y * W * 4], data + (size_t)y * line, (size_t)W * 4);
				gs_stagesurface_unmap(stage);
			}
		}
		gs_stagesurface_destroy(stage);
		gs_texrender_destroy(flat_pic);
		if (rgba.empty())
			return no("It couldn't be drawn.");
		// Drawn with its colour already times how solid it is: back to
		// plain colour, as a picture file has it.
		for (size_t i = 0; i < rgba.size(); i += 4) {
			const int a = (uint8_t)rgba[i + 3];
			for (int c = 0; c < 3 && a > 0 && a < 255; c++)
				rgba[i + c] = (char)std::min(255, ((uint8_t)rgba[i + c] * 255 + a / 2) / a);
		}
		json spots = json::object();
		const char *names[3] = {"left_eye", "right_eye", "mouth"};
		for (int i = 0; i < 3; i++) {
			const V3 p = rotate(ro.facing, fs.spot[i].at);
			spots[names[i]] = {{"x", (p.x - fs.box[0]) / bw},
					   {"y", (fs.box[1] - p.y) / bh},
					   {"w", fs.spot[i].w / bw},
					   {"h", fs.spot[i].h / bw},
					   {"from", fs.from[i]}};
		}
		return {{"ok", true},
			{"width", W},
			{"height", H},
			{"rgba", crypto::base64(rgba)},
			// (OBS's replies drop lists of plain numbers: an object.)
			{"box", {{"left", fs.box[0]}, {"top", fs.box[1]}, {"width", bw}, {"height", bh}}},
			// The part of the box drawn (shares of it): all, or round its face.
			{"region", {{"x", region.x}, {"y", region.y}, {"w", region.w}, {"h", region.h}}},
			{"spots", spots}};
	}

	int draw_cards(gs_texture_t *cam, uint32_t cw, uint32_t ch, gs_texture_t *body, const View3D &v,
		       const M4 &frame, const M4 &unturned, Quat facing, bool wear, float opacity)
	{
		if (!cam || !body || !v.w || !v.h)
			return 0;
		const float W = (float)v.w, H = (float)v.h;
		const float k = 1 / std::tan(v.fov / 2), aspect = W / H;
		bool behind = false;
		auto project = [&](V3 p, float &depth) {
			const V3 c = point(v.view, p);
			depth = -c.z;
			if (depth < v.near * 0.5f) {
				behind = true;
				depth = v.near * 0.5f;
			}
			return std::array<float, 2>{(c.x * k / aspect / depth + 1) * 0.5f * W,
						    (1 - c.y * k / depth) * 0.5f * H};
		};
		const Quat back = conj(facing);
		const Quat full = quat_of(frame), half = slerp(quat_of(unturned), full, 0.5f);
		const float scale = length(V3{frame.m[0], frame.m[1], frame.m[2]});
		const V3 eye_at = point(inverse(v.view), V3{});
		struct Card {
			std::array<float, 2> c, across, down;
			float depth = 0, facing = 1;
		} card[3];
		for (int i = 0; i < 3; i++) {
			const FaceSpot &sp = spots3d.spot[i];
			const V3 centre = point(frame, sp.at);
			const V3 a = rotate(half, rotate(back, V3{1, 0, 0})),
				 dn = rotate(half, rotate(back, V3{0, -1, 0}));
			const V3 normal = rotate(full, rotate(back, V3{0, 0, 1}));
			Card &cd = card[i];
			cd.c = project(centre, cd.depth);
			cd.facing = dot(normal, normalized(eye_at - centre, {0, 0, 1}));
			float d0, d1;
			const float hw = sp.w * scale * 0.5f, hh = sp.h * scale * 0.5f;
			const auto r = project(centre + a * hw, d0), l = project(centre - a * hw, d1);
			const auto b = project(centre + dn * hh, d0), t = project(centre - dn * hh, d1);
			cd.across = {r[0] - l[0], r[1] - l[1]};
			cd.down = {b[0] - t[0], b[1] - t[1]};
		}
		if (behind)
			return 0;
		// Which piece goes on which card: the eye on the left of the picture
		// on the card on the left. Worn, the picture is the camera's own
		// (mirrored later, card and all); in its own spot it moves as your
		// reflection does.
		const bool flip_piece = !wear && mirror;
		const int left_card = card[0].c[0] <= card[1].c[0] ? 0 : 1;
		const int far_card = card[0].depth >= card[1].depth ? 0 : 1;
		const float yaw = std::fabs(held_turn.yaw);
		gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		int drawn_cards = 0;
		for (int i = 0; i < 3; i++) {
			const int part = i == 2 ? MOUTH : (i == left_card) == !flip_piece ? FIRST_EYE : SECOND_EYE;
			const Piece &p = held.part[part];
			if (p.w <= 0 || p.h <= 0)
				continue;
			const Card &cd = card[i];
			float fade = clampf((cd.facing - 0.15f) / 0.25f, 0, 1);
			if (i == 2)
				fade *= clampf(1 - (yaw - 40) / 20, 0, 1);
			else if (i == far_card)
				fade *= clampf(1 - (yaw - 30) / 20, 0, 1);
			const float alpha = opacity * fade;
			if (alpha <= 0.001f)
				continue;
			// The card: round its spot's middle, with room for the soft edge,
			// grown by "Eye and mouth size" (and a wide-open mouth's more).
			const float grow = part_size * PIECE_ROOM * std::max(1.0f, p.grown);
			const std::array<float, 2> ax{cd.across[0] * grow, cd.across[1] * grow},
				dy{cd.down[0] * grow, cd.down[1] * grow};
			const float ox = cd.c[0] - ax[0] / 2 - dy[0] / 2, oy = cd.c[1] - ax[1] / 2 - dy[1] / 2;
			// In the camera's picture: the piece, turned with your eyes.
			const float pw = p.w * cw, ph = p.h * cw;
			const float co = std::cos(p.angle), si = std::sin(p.angle);
			float cax = co * pw, cay = si * pw, cdx = -si * ph, cdy = co * ph;
			float cox = p.x * cw - cax / 2 - cdx / 2, coy = p.y * ch - cay / 2 - cdy / 2;
			if (flip_piece) {
				cox += cax;
				coy += cay;
				cax = -cax;
				cay = -cay;
			}
			vec2 origin, across, down, b_origin, b_across, b_down, mask_rows;
			vec4 mask_place, ring;
			vec2_set(&origin, cox / cw, coy / ch);
			vec2_set(&across, cax / cw, cay / ch);
			vec2_set(&down, cdx / cw, cdy / ch);
			vec2_set(&b_origin, ox / W, oy / H);
			vec2_set(&b_across, ax[0] / W, ax[1] / H);
			vec2_set(&b_down, dy[0] / W, dy[1] / H);
			vec4_set(&mask_place, (part * MASK + 0.5f) / (MASK * PARTS), (MASK - 1.0f) / (MASK * PARTS),
				 flip_piece ? 1.0f : 0.0f, 0);
			vec2_set(&mask_rows, 0.5f / (2 * MASK), (MASK - 1.0f) / (2 * MASK));
			const float ring_k = 0.62f * grow;
			vec4_set(&ring, cd.c[0] / W, cd.c[1] / H, ring_k * std::hypot(cd.across[0], cd.across[1]) / W,
				 ring_k * std::hypot(cd.down[0], cd.down[1]) / H);
			vec3 skin_lab, target_lab;
			vec3_set(&skin_lab, p.skin.l, p.skin.a, p.skin.b);
			vec3_set(&target_lab, skin_colour.l, skin_colour.a, skin_colour.b);
			gs_effect_set_texture(p_image, cam);
			gs_effect_set_texture(p_mask_map, masks);
			gs_effect_set_texture(p_body_map, body);
			gs_effect_set_float(p_opacity, alpha);
			gs_effect_set_vec2(p_origin, &origin);
			gs_effect_set_vec2(p_across, &across);
			gs_effect_set_vec2(p_down, &down);
			gs_effect_set_vec4(p_mask_place, &mask_place);
			gs_effect_set_vec2(p_mask_rows, &mask_rows);
			gs_effect_set_vec2(p_body_origin, &b_origin);
			gs_effect_set_vec2(p_body_across, &b_across);
			gs_effect_set_vec2(p_body_down, &b_down);
			gs_effect_set_vec4(p_ring, &ring);
			gs_effect_set_float(p_ring_on, skin_auto ? 1.0f : 0.0f);
			gs_effect_set_vec3(p_skin_lab, &skin_lab);
			gs_effect_set_vec3(p_target_lab, &target_lab);
			gs_effect_set_float(p_skin_amount, skin);
			// The card as a slanted box: corner, then across and down
			// (mirrored with the picture, worn).
			matrix4 m;
			if (wear && mirror) {
				vec4_set(&m.x, -ax[0], ax[1], 0, 0);
				vec4_set(&m.y, -dy[0], dy[1], 0, 0);
				vec4_set(&m.t, W - ox, oy, 0, 1);
			} else {
				vec4_set(&m.x, ax[0], ax[1], 0, 0);
				vec4_set(&m.y, dy[0], dy[1], 0, 0);
				vec4_set(&m.t, ox, oy, 0, 1);
			}
			vec4_set(&m.z, 0, 0, 1, 0);
			gs_matrix_push();
			gs_matrix_mul(&m);
			check("Card");
			while (gs_effect_loop(effect, "Card"))
				gs_draw_quadf(nullptr, 0, 1, 1);
			gs_matrix_pop();
			drawn_cards++;
		}
		return drawn_cards;
	}

	void render()
	{
		if (drawing || !effect)
			return;
		{
			std::lock_guard<std::mutex> l(state_lock);
			showing_now = true;
			last_render = os_gettime_ns();
		}
		obs_source_t *cam = get_camera();
		const uint32_t cw = cam ? obs_source_get_width(cam) : 0, ch = cam ? obs_source_get_height(cam) : 0;
		cam_w = cw;
		cam_h = ch;
		const bool wear = worn && cw && ch;
		const uint32_t w = wear ? cw : width.load(), h = wear ? ch : height.load();
		drawing = true;
		gs_blend_state_push();
		if (cam && cw && ch) {
			if (since_render > STALE) {
				staged = false;
				worker.forget();
				pace.reset();
			}
			since_render = 0;
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
			look(cam, cw, ch);
			pick_shots();
		}
		if (!drawn) {
			draw_avatar(cam, cw, ch, w, h);
			drawn = true;
		}
		gs_blend_state_pop();
		obs_source_release(cam);

		gs_texture_t *tex = have_out ? gs_texrender_get_texture(out) : nullptr;
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

	// The avatar into `out`, once a frame: the character drawn flat, then
	// turned and placed (over the camera, worn on your head).
	void draw_avatar(obs_source_t *cam, uint32_t cw, uint32_t ch, uint32_t w, uint32_t h)
	{
		std::lock_guard<std::mutex> l(pictures_lock);
		const Shown::Out o = shown.now();
		const bool wear = worn && cw && ch;
		pick_model();
		upload_masks();
		if (rig && model_now) {
			draw_3d(cam, cw, ch, w, h, o, wear);
			return;
		}
		gs_texture_t *cut = cam && used_shot >= 0 ? gs_texrender_get_texture(shots[used_shot].tex) : nullptr;
		// Which way it faces: turned pictures if there are any.
		const int turned = turn_pictures ? o.turned * (mirror ? -1 : 1) : 0;
		int which = 0;
		if (turned < 0 && loaded(body_left))
			which = 1;
		else if (turned > 0 && loaded(body_right))
			which = 2;
		const PicturePtr &pic = which == 1 ? body_left : which == 2 ? body_right : body;
		Placed at;
		gs_texture_t *flat_tex = nullptr;
		if (loaded(pic)) {
			at = place(o, *pic, which, shapes[which].get(), wear, w, h);
			if (at.shown)
				flat_tex = draw_flat(cut, cw, ch, o, *pic, shapes[which].get(), at.scale);
		}
		{
			std::lock_guard<std::mutex> l2(state_lock);
			placed_now = at;
		}
		if (!out)
			out = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		if (!out)
			return;
		// A texrender draws once until it's reset.
		gs_texrender_reset(out);
		if (!gs_texrender_begin(out, w, h))
			return;
		vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		if (wear && show_camera)
			draw_camera(cam, cw, ch);
		if (flat_tex)
			draw_turned(flat_tex, shapes[which], *pic, at);
		gs_texrender_end(out);
		have_out = true;
	}

	void tick(float dt)
	{
		drawn = false;
		frame_dt = dt;
		since_render += dt;
		since_find += dt;
		pace.every = easy ? EASY_LOOK_EVERY : LOOK_EVERY;
		pace.frame(dt);
		shown.set_lead(lead);
		if (since_find >= FIND_EVERY) {
			since_find = 0;
			obs_source_t *cam = get_camera();
			if (cam)
				obs_source_release(cam);
			else
				find_camera();
		}
		worker.state(seen);
		const bool fresh = seen.looks != seen_looks;
		seen_looks = seen.looks;
		if (reset_shown.exchange(false))
			shown.reset();
		const Shown::Out o = shown.step(seen.face, seen.rest, dt, fresh);
		// Its drawn face, from yours (at rest while it can't see you).
		drawn_now = o.found || o.holding ? drawn_face(o.moves, mirror, drawn_now) : Drawn();
		std::lock_guard<std::mutex> l(state_lock);
		shown_now = o;
		state_now = seen;
		drawn_state = drawn_now;
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

		// "Set" (from DireBot's FaceSet, saved here by save_rest_task),
		// unless one has just finished and isn't saved yet.
		const char *rest_text = obs_data_get_string(settings, "rest");
		const std::string rest_now = rest_text ? rest_text : "";
		bool new_rest = false;
		{
			std::lock_guard<std::mutex> l(lock);
			if (rest_seen != rest_now && !worker.rest_pending()) {
				rest_seen = rest_now;
				new_rest = true;
			}
		}
		if (new_rest) {
			worker.load_rest(rest_now);
			reset_shown = true;
		}
		const json spot_json = json::parse(obs_data_get_string(settings, "spots"), nullptr, false);
		Rect got_spots[PARTS];
		bool have = spot_json.is_object();
		const char *names[PARTS] = {"left_eye", "right_eye", "mouth"};
		for (int i = 0; i < PARTS && have; i++)
			have = spot_json.contains(names[i]) && rect_from(spot_json[names[i]], got_spots[i]);
		const json outfit = json::parse(obs_data_get_string(settings, "outfit"), nullptr, false);
		const json places = json::parse(obs_data_get_string(settings, "drawn_places"), nullptr, false);
		auto place_of = [&places](const char *key) {
			Rect r{0, 0, 1, 1};
			if (places.is_object() && places.contains(key))
				rect_from(places[key], r);
			return r;
		};

		// The pictures are loaded first, outside the lock (see Picture);
		// the ones they replace are let go after it.
		PicturePtr next_body, next_left, next_right, next_face, next_eyes[EYE_LOOKS], next_mouths[MOUTH_LOOKS];
		std::vector<Layer> next_layers;
		{
			std::lock_guard<std::mutex> l(pictures_lock);
			next_body = body;
			next_left = body_left;
			next_right = body_right;
			next_face = own_face;
			std::copy(eyes, eyes + EYE_LOOKS, next_eyes);
			std::copy(mouths, mouths + MOUTH_LOOKS, next_mouths);
			next_layers = layers;
		}
		next_body = load_picture(next_body, obs_data_get_string(settings, "character"));
		next_left = load_picture(next_left, obs_data_get_string(settings, "character_left"));
		next_right = load_picture(next_right, obs_data_get_string(settings, "character_right"));
		next_face = load_picture(next_face, obs_data_get_string(settings, "own_face"));
		Rect eye_at[EYE_LOOKS], mouth_at[MOUTH_LOOKS];
		for (int i = 0; i < EYE_LOOKS; i++) {
			next_eyes[i] = load_picture(next_eyes[i], obs_data_get_string(settings, EYE_KEYS[i]));
			eye_at[i] = place_of(EYE_KEYS[i]);
		}
		for (int i = 0; i < MOUTH_LOOKS; i++) {
			next_mouths[i] = load_picture(next_mouths[i], obs_data_get_string(settings, MOUTH_KEYS[i]));
			mouth_at[i] = place_of(MOUTH_KEYS[i]);
		}
		// Outfit layers: a file still worn keeps its picture.
		std::vector<Layer> wearing;
		if (outfit.is_array()) {
			for (const json &row : outfit) {
				if (wearing.size() >= MOST_LAYERS || !row.is_object() || !row.contains("file") ||
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
				wearing.push_back(layer);
			}
		}

		const float part_size_now = clampf((float)obs_data_get_int(settings, "part_size") / 100.0f, 0.5f, 2.0f);
		// The shapes, worked out again only when what they're of changes.
		const Rect canvas_now = canvas_of(wearing, got_spots, have, part_size_now);
		json key = json::array();
		for (const PicturePtr *p : {&next_body, &next_left, &next_right})
			key.push_back(*p ? (*p)->path : "");
		for (const Layer &layer : wearing)
			key.push_back({layer.picture ? layer.picture->path : "", layer.place.x, layer.place.y,
				       layer.place.w, layer.place.h});
		for (int i = 0; i < PARTS && have; i++)
			key.push_back({got_spots[i].x, got_spots[i].y, got_spots[i].w, got_spots[i].h});
		key.push_back({canvas_now.x, canvas_now.y, canvas_now.w, canvas_now.h});
		const std::string shape_key_now = key.dump();
		bool new_shapes;
		{
			std::lock_guard<std::mutex> l(pictures_lock);
			new_shapes = shape_key_now != shape_key;
		}
		std::shared_ptr<const Shape> next_shapes[3];
		if (new_shapes) {
			next_shapes[0] = shape_for(next_body, wearing, canvas_now, got_spots, have);
			next_shapes[1] = shape_for(next_left, wearing, canvas_now, got_spots, have);
			next_shapes[2] = shape_for(next_right, wearing, canvas_now, got_spots, have);
		}
		vec4 colour;
		vec4_from_rgba(&colour, (uint32_t)obs_data_get_int(settings, "skin_colour") | 0xFF000000u);
		{
			std::lock_guard<std::mutex> l(pictures_lock);
			body.swap(next_body);
			body_left.swap(next_left);
			body_right.swap(next_right);
			own_face.swap(next_face);
			for (int i = 0; i < EYE_LOOKS; i++) {
				eyes[i].swap(next_eyes[i]);
				eye_places[i] = eye_at[i];
			}
			for (int i = 0; i < MOUTH_LOOKS; i++) {
				mouths[i].swap(next_mouths[i]);
				mouth_places[i] = mouth_at[i];
			}
			layers.swap(wearing);
			have_spots = have;
			if (have)
				std::copy(got_spots, got_spots + PARTS, spots);
			canvas = canvas_now;
			if (new_shapes) {
				for (int i = 0; i < 3; i++)
					shapes[i].swap(next_shapes[i]);
				shape_key = shape_key_now;
			}
			size = clampf((float)obs_data_get_int(settings, "size") / 100.0f, 0.2f, 1.0f);
			part_size = part_size_now;
			feather = clampf((float)obs_data_get_int(settings, "feather") / 100.0f, 0, 1);
			skin = clampf((float)obs_data_get_int(settings, "skin") / 100.0f, 0, 1);
			skin_auto = obs_data_get_bool(settings, "skin_auto");
			skin_colour = lab_of(colour.x, colour.y, colour.z);
			bend = clampf((float)obs_data_get_int(settings, "bend") / 100.0f, 0, 1);
			turn_pictures = obs_data_get_bool(settings, "turn_pictures");
			follow_move = clampf((float)obs_data_get_int(settings, "follow_move") / 100.0f, 0, 1);
			follow_grow = clampf((float)obs_data_get_int(settings, "follow_grow") / 100.0f, 0, 1);
			follow_tilt = clampf((float)obs_data_get_int(settings, "follow_tilt") / 100.0f, 0, 1);
			most_move = clampf((float)obs_data_get_int(settings, "most_move") / 100.0f, 0, 0.5f);
			show_camera = obs_data_get_bool(settings, "show_camera");
			worn_size = clampf((float)obs_data_get_int(settings, "worn_size") / 100.0f, 0.5f, 3.0f);
		}
		next_layers.clear();
		// The 3D character.
		Look3D l3;
		l3.turn = (int)obs_data_get_int(settings, "model_turn");
		if (l3.turn != -1)
			l3.turn = ((l3.turn % 360) + 360) % 360;
		const char *look_name = obs_data_get_string(settings, "look");
		l3.look = !look_name                               ? 0
			  : std::strcmp(look_name, "glossy") == 0  ? 1
			  : std::strcmp(look_name, "cartoon") == 0 ? 2
			  : std::strcmp(look_name, "flat") == 0    ? 3
								   : 0;
		l3.outline = clampf((float)obs_data_get_int(settings, "outline") / 100.0f, 0, 1) * 0.02f;
		l3.floppy = clampf((float)obs_data_get_int(settings, "floppy") / 100.0f, 0, 1);
		l3.swing = obs_data_get_bool(settings, "swing");
		// Tails: how far they wag (0 not at all), and how fast against
		// a usual wag (WAG_SPEED wags a second).
		l3.wag = clampf((float)obs_data_get_int(settings, "wag") / 100.0f, 0, 1);
		l3.wag_speed = WAG_SPEED * clampf((float)obs_data_get_int(settings, "wag_speed") / 100.0f, 0.1f, 4);
		const char *framing = obs_data_get_string(settings, "framing");
		l3.framing = framing && std::strcmp(framing, "whole") == 0  ? 1
			     : framing && std::strcmp(framing, "head") == 0 ? 2
									    : 0;
		l3.hide = obs_data_get_bool(settings, "hide_behind_head");
		l3.smooth = obs_data_get_bool(settings, "smooth_edges");
		const json model_place = json::parse(obs_data_get_string(settings, "model_place"), nullptr, false);
		if (model_place.is_object()) {
			auto number = [&model_place](const char *key, float def) {
				auto it = model_place.find(key);
				return it != model_place.end() && it->is_number() && std::isfinite(it->get<double>())
					       ? (float)it->get<double>()
					       : def;
			};
			const float units = number("units", 0);
			if (units > 0) {
				l3.place.automatic = false;
				l3.place.units = clampf(units, 1e-4f, 1e6f);
				l3.place.at = {clampf(number("x", 0), -200, 200), clampf(number("y", 0), -200, 200),
					       clampf(number("z", 0), -200, 200)};
			}
			l3.place.size = clampf(number("size", 1), 0.1f, 10);
		}
		const char *setup_text = obs_data_get_string(settings, "model_setup");
		l3.setup_text = setup_text ? setup_text : "";
		{
			std::lock_guard<std::mutex> l(pictures_lock);
			if (look3d.setup_text == l3.setup_text)
				l3.setup = look3d.setup;
			else
				l3.setup = std::make_shared<CharacterSetup>(setup_from(l3.setup_text));
		}
		{
			std::lock_guard<std::mutex> l(pictures_lock);
			look3d = l3;
		}
		const char *model_file = obs_data_get_string(settings, "model");
		loader.want(model_file ? model_file : "");
		mirror = obs_data_get_bool(settings, "mirror");
		easy = obs_data_get_bool(settings, "easy");
		const char *style = obs_data_get_string(settings, "style");
		drawn_style = style && std::strcmp(style, "drawn") == 0;
		worn = obs_data_get_bool(settings, "worn");
		// Smoothing: steadier one way; the other, snappier, and the
		// character aims further on past each look to keep up with you.
		const float smoothing = clampf((float)obs_data_get_int(settings, "smoothing") / 100.0f, 0, 1);
		worker.set_smoothing(smoothing);
		lead = 1 - smoothing;

		// Each of your eyes and your mouth is cut the shape of the
		// character's spot it goes into, so it's never stretched (a 3D
		// character's, once it's loaded: pick_model).
		float aspect[PARTS] = {0.7f, 0.7f, 0.5f};
		if (model_file && *model_file) {
			std::lock_guard<std::mutex> l(pictures_lock);
			worker.set_cut(aspect3d, feather);
		} else {
			std::lock_guard<std::mutex> l(pictures_lock);
			const float char_w = loaded(body) ? body->width() : 1,
				    char_h = loaded(body) ? body->height() : 1;
			for (int part = 0; part < PARTS && have_spots; part++) {
				const int spot = part == MOUTH ? MOUTH : mirror ? (part == FIRST_EYE ? 1 : 0) : part;
				aspect[part] = clampf(spots[spot].h * char_h / std::max(1e-6f, spots[spot].w * char_w),
						      0.1f, 4.0f);
			}
			worker.set_cut(aspect, feather);
		}
	}

	// "Set" is in the settings now (save_rest_task, on the UI thread).
	void rest_saved(const std::string &text)
	{
		{
			std::lock_guard<std::mutex> l(lock);
			rest_seen = text;
		}
		worker.rest_saved();
	}

	json state(bool with_points)
	{
		// The character's shape first, on its own: the graphics thread
		// takes the pictures' lock, then the state's.
		const char *spot_names[PARTS] = {"left_eye", "right_eye", "mouth"};
		json holes = json::object();
		bool shaped;
		{
			std::lock_guard<std::mutex> l(pictures_lock);
			for (int i = 0; i < PARTS; i++)
				holes[spot_names[i]] = shapes[0] && shapes[0]->hole[i];
			shaped = shapes[0] && shapes[0]->ready();
		}
		std::lock_guard<std::mutex> l(state_lock);
		const Face &face = state_now.face;
		const Shown::Out &o = shown_now;
		auto r = [](float v, float steps) {
			return std::round(v * steps) / steps;
		};
		json parts = json::array();
		for (const Part &p : o.layout.parts)
			parts.push_back(
				{{"x", r(p.x, 1e4f)}, {"y", r(p.y, 1e4f)}, {"w", r(p.w, 1e4f)}, {"h", r(p.h, 1e4f)}});
		json moves = json::object();
		for (int i = 1; i < EXPRESSIONS; i++)
			moves[EXPRESSION_NAMES[i]] = r(o.moves[i], 100);
		json camera_name_now;
		{
			std::lock_guard<std::mutex> l2(lock);
			camera_name_now = camera_name;
		}
		json answer = {
			{"ok", true},
			{"camera", camera_name_now},
			{"ready", state_now.ready},
			{"showing", showing_now},
			{"found", face.found},
			{"presence", r(face.presence, 100)},
			{"set", state_now.rest.set},
			{"setting", state_now.setting},
			{"turn",
			 {{"yaw", r(o.turn.yaw, 10)}, {"pitch", r(o.turn.pitch, 10)}, {"roll", r(o.turn.roll, 10)}}},
			{"turned", o.turned},
			{"moves", moves},
			{"parts", parts},
			{"shown", r(o.shown, 100)},
			{"holding", o.holding},
			{"looks_per_second", r(state_now.rate, 10)},
			{"ms", r(state_now.ms, 10)}};
		if (!state_now.problem.empty())
			answer["problem"] = state_now.problem;
		answer["style"] = drawn_style ? "drawn" : "real";
		answer["worn"] = worn.load();
		// (No lists of plain values: OBS only passes on lists of objects.)
		answer["drawn"] = {{"left_eye", EYE_NAMES[drawn_state.eye[0]]},
				   {"right_eye", EYE_NAMES[drawn_state.eye[1]]},
				   {"mouth", MOUTH_NAMES[drawn_state.mouth]}};
		const Placed &at = placed_now;
		answer["placed"] = {{"shown", at.shown},
				    {"picture", at.which == 1   ? "left"
						: at.which == 2 ? "right"
								: "main"},
				    {"x", r(at.x, 10)},
				    {"y", r(at.y, 10)},
				    {"scale", r(at.scale, 1e4f)},
				    {"angle", r(at.angle / RAD, 10)},
				    {"yaw", r(at.yaw, 10)},
				    {"pitch", r(at.pitch, 10)},
				    {"opacity", r(at.opacity, 100)}};
		answer["see_through"] = holes;
		answer["shaped"] = shaped;
		{
			const State3D &st = state3d;
			json values = json::object();
			for (const auto &kv : st.values)
				values[kv.first] = r(kv.second, 100);
			json model = {{"file", st.file},
				      {"loaded", st.loaded},
				      {"loading", loader.busy()},
				      {"shown", st.shown},
				      {"kind", st.kind},
				      {"title", st.title},
				      {"author", st.author},
				      {"points", st.points},
				      {"triangles", st.triangles},
				      {"springs", st.springs},
				      {"tails", st.tails},
				      {"swinging_chain", st.chain},
				      {"arkit_parts", st.arkit},
				      {"set_up", st.set_up},
				      {"upright_parts", st.upright},
				      {"cards", st.cards},
				      {"outfit_layers", st.layers},
				      {"expressions", values},
				      {"yaw", r(st.yaw, 10)},
				      {"pitch", r(st.pitch, 10)},
				      {"x", r(st.x, 10)},
				      {"y", r(st.y, 10)}};
			if (!st.problem.empty())
				model["problem"] = st.problem;
			if (st.loaded) {
				json spots = json::object();
				for (int i = 0; i < PARTS; i++)
					spots[spot_names[i]] = {{"x", r(st.spot[i][0], 1e4f)},
								{"y", r(st.spot[i][1], 1e4f)},
								{"w", r(st.spot[i][2], 1e4f)},
								{"h", r(st.spot[i][3], 1e4f)},
								{"from", st.spot_from[i]}};
				model["spots"] = spots;
			}
			answer["model"] = model;
		}
		if (with_points && face.found && face.w > 0 && face.h > 0) {
			json points = json::array();
			for (const Point &p : face.points)
				points.push_back(
					{{"x", r((p.x + 0.5f) / face.w, 1e4f)}, {"y", r((p.y + 0.5f) / face.h, 1e4f)}});
			answer["points"] = points;
		}
		return answer;
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

void save_rest_task(void *param)
{
	std::unique_ptr<std::pair<obs_weak_source_t *, std::string>> job(
		static_cast<std::pair<obs_weak_source_t *, std::string> *>(param));
	obs_source_t *src = obs_weak_source_get_source(job->first);
	obs_weak_source_release(job->first);
	if (!src)
		return;
	obs_data_t *settings = obs_source_get_settings(src);
	obs_data_set_string(settings, "rest", job->second.c_str());
	obs_data_release(settings);
	if (auto *f = static_cast<FaceAvatar *>(obs_obj_get_data(src)))
		f->rest_saved(job->second);
	obs_source_release(src);
}

const char *get_name(void *)
{
	return text("FaceAvatar.Name");
}

void *create(obs_data_t *settings, obs_source_t *source)
{
	auto *f = new FaceAvatar(source);
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
	obs_data_set_default_string(settings, "style", "real");
	obs_data_set_default_int(settings, "skin", 80);
	obs_data_set_default_bool(settings, "skin_auto", true);
	obs_data_set_default_int(settings, "skin_colour", 0xFF809ECC);
	obs_data_set_default_bool(settings, "mirror", true);
	obs_data_set_default_int(settings, "bend", 85);
	obs_data_set_default_bool(settings, "turn_pictures", true);
	obs_data_set_default_bool(settings, "worn", false);
	obs_data_set_default_bool(settings, "show_camera", true);
	obs_data_set_default_int(settings, "worn_size", 130);
	for (const char *key : EYE_KEYS)
		obs_data_set_default_string(settings, key, "");
	for (const char *key : MOUTH_KEYS)
		obs_data_set_default_string(settings, key, "");
	obs_data_set_default_string(settings, "drawn_places", "");
	obs_data_set_default_int(settings, "follow_move", 60);
	obs_data_set_default_int(settings, "follow_grow", 60);
	obs_data_set_default_int(settings, "follow_tilt", 100);
	obs_data_set_default_int(settings, "most_move", 25);
	obs_data_set_default_int(settings, "smoothing", 50);
	obs_data_set_default_bool(settings, "easy", false);
	obs_data_set_default_string(settings, "rest", "");
	obs_data_set_default_string(settings, "spots", "");
	obs_data_set_default_string(settings, "outfit", "[]");
	obs_data_set_default_string(settings, "model", "");
	obs_data_set_default_int(settings, "model_turn", 0);
	obs_data_set_default_string(settings, "look", "auto");
	obs_data_set_default_int(settings, "outline", 0);
	obs_data_set_default_int(settings, "floppy", 50);
	obs_data_set_default_bool(settings, "swing", true);
	obs_data_set_default_int(settings, "wag", 50);
	obs_data_set_default_int(settings, "wag_speed", 100);
	obs_data_set_default_string(settings, "framing", "auto");
	obs_data_set_default_bool(settings, "hide_behind_head", true);
	obs_data_set_default_bool(settings, "smooth_edges", true);
	obs_data_set_default_string(settings, "model_place", "");
	obs_data_set_default_string(settings, "model_setup", "");
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
	obs_property_t *style = obs_properties_add_list(props, "style", text("FaceAvatar.Style"), OBS_COMBO_TYPE_LIST,
							OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(style, text("FaceAvatar.StyleReal"), "real");
	obs_property_list_add_string(style, text("FaceAvatar.StyleDrawn"), "drawn");
	obs_property_t *p = obs_properties_add_int_slider(props, "size", text("FaceAvatar.Size"), 20, 100, 1);
	obs_property_int_set_suffix(p, "%");
	p = obs_properties_add_int_slider(props, "part_size", text("FaceAvatar.PartSize"), 50, 200, 5);
	obs_property_int_set_suffix(p, "%");
	p = obs_properties_add_int_slider(props, "feather", text("FaceAvatar.Feather"), 0, 100, 1);
	obs_property_int_set_suffix(p, "%");
	p = obs_properties_add_int_slider(props, "skin", text("FaceAvatar.Skin"), 0, 100, 1);
	obs_property_int_set_suffix(p, "%");
	obs_properties_add_bool(props, "skin_auto", text("FaceAvatar.SkinAuto"));
	obs_properties_add_color(props, "skin_colour", text("FaceAvatar.SkinColour"));
	obs_properties_add_bool(props, "mirror", text("FaceAvatar.Mirror"));
	p = obs_properties_add_int_slider(props, "bend", text("FaceAvatar.Bend"), 0, 100, 1);
	obs_property_int_set_suffix(p, "%");
	obs_properties_add_bool(props, "turn_pictures", text("FaceAvatar.TurnPictures"));
	p = obs_properties_add_int_slider(props, "follow_move", text("FaceAvatar.FollowMove"), 0, 100, 1);
	obs_property_int_set_suffix(p, "%");
	p = obs_properties_add_int_slider(props, "follow_grow", text("FaceAvatar.FollowGrow"), 0, 100, 1);
	obs_property_int_set_suffix(p, "%");
	p = obs_properties_add_int_slider(props, "follow_tilt", text("FaceAvatar.FollowTilt"), 0, 100, 1);
	obs_property_int_set_suffix(p, "%");
	p = obs_properties_add_int_slider(props, "most_move", text("FaceAvatar.MostMove"), 0, 50, 1);
	obs_property_int_set_suffix(p, "%");
	p = obs_properties_add_int_slider(props, "smoothing", text("FaceAvatar.Smoothing"), 0, 100, 1);
	obs_property_int_set_suffix(p, "%");
	obs_properties_add_bool(props, "easy", text("FaceAvatar.Easy"));
	obs_properties_add_bool(props, "worn", text("FaceAvatar.Worn"));
	obs_properties_add_bool(props, "show_camera", text("FaceAvatar.ShowCamera"));
	p = obs_properties_add_int_slider(props, "worn_size", text("FaceAvatar.WornSize"), 50, 300, 5);
	obs_property_int_set_suffix(p, "%");
	obs_properties_add_int(props, "width", text("FaceAvatar.Width"), MIN_SIZE, MAX_SIZE, 2);
	obs_properties_add_int(props, "height", text("FaceAvatar.Height"), MIN_SIZE, MAX_SIZE, 2);
	obs_properties_t *drawn = obs_properties_create();
	const char *eye_text[EYE_LOOKS] = {"FaceAvatar.EyesOpen", "FaceAvatar.EyesHalf", "FaceAvatar.EyesClosed"};
	const char *mouth_text[MOUTH_LOOKS] = {"FaceAvatar.MouthClosed", "FaceAvatar.MouthOpen", "FaceAvatar.MouthWide",
					       "FaceAvatar.MouthSmile", "FaceAvatar.MouthO"};
	obs_properties_add_text(drawn, "drawn_about", text("FaceAvatar.DrawnAbout"), OBS_TEXT_INFO);
	for (int i = 0; i < EYE_LOOKS; i++)
		obs_properties_add_path(drawn, EYE_KEYS[i], text(eye_text[i]), OBS_PATH_FILE, pictures, nullptr);
	for (int i = 0; i < MOUTH_LOOKS; i++)
		obs_properties_add_path(drawn, MOUTH_KEYS[i], text(mouth_text[i]), OBS_PATH_FILE, pictures, nullptr);
	obs_properties_add_group(props, "drawn_face", text("FaceAvatar.DrawnFace"), OBS_GROUP_NORMAL, drawn);

	obs_properties_t *m3 = obs_properties_create();
	obs_properties_add_text(m3, "model_about", text("FaceAvatar.ModelAbout"), OBS_TEXT_INFO);
	obs_properties_add_path(m3, "model", text("FaceAvatar.Model"), OBS_PATH_FILE,
				"3D characters (*.glb *.vrm *.gltf);;All files (*.*)", nullptr);
	obs_property_t *turn = obs_properties_add_list(m3, "model_turn", text("FaceAvatar.ModelTurn"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(turn, text("FaceAvatar.TurnAuto"), 0);
	obs_property_list_add_int(turn, text("FaceAvatar.TurnLeft"), 90);
	obs_property_list_add_int(turn, text("FaceAvatar.TurnBack"), 180);
	obs_property_list_add_int(turn, text("FaceAvatar.TurnRight"), 270);
	obs_property_t *look = obs_properties_add_list(m3, "look", text("FaceAvatar.Look"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(look, text("FaceAvatar.LookAuto"), "auto");
	obs_property_list_add_string(look, text("FaceAvatar.LookGlossy"), "glossy");
	obs_property_list_add_string(look, text("FaceAvatar.LookCartoon"), "cartoon");
	obs_property_list_add_string(look, text("FaceAvatar.LookFlat"), "flat");
	p = obs_properties_add_int_slider(m3, "outline", text("FaceAvatar.Outline"), 0, 100, 1);
	obs_property_int_set_suffix(p, "%");
	obs_properties_add_bool(m3, "swing", text("FaceAvatar.Swing"));
	p = obs_properties_add_int_slider(m3, "floppy", text("FaceAvatar.Floppy"), 0, 100, 1);
	obs_property_int_set_suffix(p, "%");
	p = obs_properties_add_int_slider(m3, "wag", text("FaceAvatar.Wag"), 0, 100, 1);
	obs_property_int_set_suffix(p, "%");
	p = obs_properties_add_int_slider(m3, "wag_speed", text("FaceAvatar.WagSpeed"), 25, 300, 5);
	obs_property_int_set_suffix(p, "%");
	obs_property_t *framing = obs_properties_add_list(m3, "framing", text("FaceAvatar.Framing"),
							  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(framing, text("FaceAvatar.FramingAuto"), "auto");
	obs_property_list_add_string(framing, text("FaceAvatar.FramingWhole"), "whole");
	obs_property_list_add_string(framing, text("FaceAvatar.FramingHead"), "head");
	obs_properties_add_bool(m3, "hide_behind_head", text("FaceAvatar.HideBehindHead"));
	obs_properties_add_bool(m3, "smooth_edges", text("FaceAvatar.SmoothEdges"));
	obs_properties_add_group(props, "model3d", text("FaceAvatar.Model3D"), OBS_GROUP_NORMAL, m3);
	return props;
}

// Worn on your head, it's the camera's size.
uint32_t get_width(void *data)
{
	auto *f = static_cast<FaceAvatar *>(data);
	return f->worn && f->cam_w && f->cam_h ? f->cam_w.load() : f->width.load();
}

uint32_t get_height(void *data)
{
	auto *f = static_cast<FaceAvatar *>(data);
	return f->worn && f->cam_w && f->cam_h ? f->cam_h.load() : f->height.load();
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

std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);

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

// "Look straight at the camera and press Set" (or `clear`).
json set_rest(const json &data)
{
	obs_source_t *src = find_avatar(data);
	if (!src)
		return fail("That face avatar isn't in OBS any more.");
	auto *f = static_cast<FaceAvatar *>(obs_obj_get_data(src));
	json answer = {{"ok", true}};
	if (!f) {
		answer = fail("It isn't ready yet.");
	} else if (data.value("clear", false)) {
		obs_data_t *settings = obs_source_get_settings(src);
		obs_data_set_string(settings, "rest", "");
		obs_data_release(settings);
		f->rest_saved("");
		f->worker.load_rest("");
		f->reset_shown = true;
	} else {
		Worker::State st;
		f->worker.state(st);
		bool showing;
		{
			std::lock_guard<std::mutex> l(f->state_lock);
			showing = f->showing_now;
		}
		if (!st.ready)
			answer = fail(st.problem.empty()
					      ? "The face models are still loading - try again in a moment."
					      : "The face models couldn't be loaded - reinstall DireBot for OBS.");
		else if (!showing)
			answer = fail("Show the face avatar in OBS first - it only looks for you while it's shown.");
		else
			f->worker.start_rest();
		answer["setting"] = answer["ok"].get<bool>();
	}
	obs_source_release(src);
	return answer;
}

json get_state(const json &data)
{
	obs_source_t *src = find_avatar(data);
	if (!src)
		return fail("That face avatar isn't in OBS any more.");
	auto *f = static_cast<FaceAvatar *>(obs_obj_get_data(src));
	json answer = f ? f->state(data.value("points", false)) : fail("It isn't ready yet.");
	obs_source_release(src);
	return answer;
}

json model_front(const json &data)
{
	obs_source_t *src = find_avatar(data);
	if (!src)
		return fail("That face avatar isn't in OBS any more.");
	auto *f = static_cast<FaceAvatar *>(obs_obj_get_data(src));
	const int width = data.contains("width") && data["width"].is_number() ? data["width"].get<int>() : 400;
	const bool close = data.contains("close") && data["close"].is_boolean() && data["close"].get<bool>();
	json answer = f ? f->front_picture(width, close) : fail("It isn't ready yet.");
	obs_source_release(src);
	return answer;
}

} // namespace

const std::vector<std::string> &requests()
{
	static const std::vector<std::string> list = {"FaceSet", "FaceGetState", "FaceModelFront"};
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
		if (type == "FaceSet")
			return set_rest(data);
		if (type == "FaceGetState")
			return get_state(data);
		if (type == "FaceModelFront")
			return model_front(data);
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
