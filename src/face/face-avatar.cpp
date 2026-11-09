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
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>
#include <util/platform.h>

#include <plugin-support.h>

#include "../link/crypto.hpp"
#include "../obs/picture-file.hpp"
#include "../obs/ui-call.hpp"
#include "alive.hpp"
#include "arms.hpp"
#include "avatar2d.hpp"
#include "avatar3d.hpp"
#include "body.hpp"
#include "companion.hpp"
#include "devices.hpp"
#include "effect-check.hpp"
#include "look-pace.hpp"
#include "model.hpp"
#include "phone.hpp"
#include "vmc.hpp"
#include "rig.hpp"
#include "signs.hpp"
#include "tracker.hpp"
#include "voice.hpp"

namespace direbot::face {

using json = nlohmann::json;

namespace {

// How often it looks for your face at most, in seconds ("Easy on my PC":
// half as often) - at each new picture the camera sends, up to that.
constexpr float LOOK_EVERY = 1.0f / 30, EASY_LOOK_EVERY = 1.0f / 15;
// ...and for your hands: up to as often as your face (each followed from
// where it was, so they keep up with quick moves); your body - where your
// shoulders and elbows are - less often (light mode: both less often, and
// a lighter hand model). Arms only (fingers not followed): the body's.
// The character eases between looks (ArmSolver::EASE).
constexpr float HANDS_EVERY = 1.0f / 30, LIGHT_HANDS_EVERY = 1.0f / 15;
constexpr float ARMS_EVERY = 1.0f / 15, LIGHT_ARMS_EVERY = 1.0f / 10;
// Not drawn for this long (not shown): what it saw last is too old.
constexpr float STALE = 0.5f;
// Hidden on every scene that's shown, how often it looks for a newly
// loaded 3D character (seconds) - 0.8.1.
constexpr float HIDDEN_PICK_EVERY = 0.25f;
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
// What it does on cue: at most this many things to throw and VRM
// animation files, and the numbers its VRM animation files' moves go by
// (React::play's; under it, its own animations' numbers).
constexpr size_t MOST_THROW_PICTURES = 24, MOST_MOTIONS = 32;
// The most pictures for what things do as they hit (0.9.0: splats,
// drips, juice, feathers, snow...), all of them together.
constexpr size_t MOST_EFFECT_PICTURES = 64;
// The throws filter (0.9.0): how much of a hit's knock-back jolts the
// camera (a person's head isn't a character's), and how long it stays
// awake after something's asked of it (seconds).
constexpr float CAMERA_JOLT = 0.45f;
constexpr uint64_t AWAKE_NS = 4'000'000'000ULL;
// A face avatar reading its camera (to find your face, or to show it
// behind the avatar) gets it as it is: a throws filter on that camera
// lets it through untouched - a tomato on your face would hide it from
// the tracker, and the avatar draws its own throws. (Drawing is on OBS's
// one graphics thread.)
thread_local int reading_camera = 0;
struct ReadingCamera {
	ReadingCamera() { reading_camera++; }
	~ReadingCamera() { reading_camera--; }
};
constexpr int MOTION_IDS = 10000;
// Chat's emotes thrown (by their pictures' files): the newest this many
// kept, numbered from EMOTE_IDS for React.
constexpr size_t MOST_EMOTES = 64;
constexpr int EMOTE_IDS = 1000;
// The 3D headpat hand: how tall a head it's made for (its file's units,
// metres), how far towards its side from the middle of the top of the
// head its palm rests (a share of the head's width), how far back (a
// share of its depth) and how far round there it looks for the highest
// point under its palm (a share of its height).
constexpr float HAND_HEAD = 0.22f, HAND_OVER = 0.0f, HAND_BACK = 0.05f, HAND_REACH = 0.18f;
// How tall the 3D hand looks for its width (a picture hand's is its
// picture's), where a picture's top edge cuts its head: as our cartoon
// hand's, so as much of it shows.
constexpr float HAND_3D_TALL = 0.8f;
// 3D things over the character: drawn nearly straight on (a narrow view).
constexpr float OVERLAY_FOV = 0.35f;
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
// 0.9.0: a move that stays (a ride...) that can't land exactly where its
// idle starts (none, or cut short): its idle cross-fades in over this
// long (seconds). And the camera views a companion and a move that stay
// glide to, as view_shown has them (the chosen one's number, or -1).
constexpr float STAY_FADE = 0.5f;
constexpr int VIEW_COMPANION = -2, VIEW_STAY = -3;

const char *text(const char *key)
{
	return obs_module_text(key);
}

float clampf(float v, float lo, float hi)
{
	return std::min(hi, std::max(lo, v));
}

float mix(float a, float b, float t)
{
	return a + (b - a) * t;
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
	PictureFile image;
	// A small copy of it (straight colour, 4 bytes a point), for its shape.
	std::vector<uint8_t> small;
	int small_w = 0, small_h = 0;
	bool small_bgra = false;

	explicit Picture(const std::string &file) : path(file)
	{
		image.open(path.c_str());
		keep_small();
		obs_enter_graphics();
		image.init_texture();
		obs_leave_graphics();
		if (!image.loaded())
			obs_log(LOG_WARNING, "DireBot Face Avatar: couldn't open a picture (is the file still there?)");
	}
	~Picture()
	{
		obs_enter_graphics();
		image.free();
		obs_leave_graphics();
	}

	// Before the picture goes to the graphics card (which lets go of its
	// points): every few points, at most this many across.
	void keep_small()
	{
		constexpr uint32_t MOST = 512;
		const uint8_t *points = image.texture_data();
		const uint32_t cx = image.cx(), cy = image.cy();
		if (!image.loaded() || image.is_animated_gif() || !points || !cx || !cy)
			return;
		const gs_color_format format = image.format();
		const bool bgra = format == GS_BGRA || format == GS_BGRX;
		if (!bgra && format != GS_RGBA)
			return;
		const bool opaque = format == GS_BGRX;
		const uint32_t every = std::max(1u, (std::max(cx, cy) + MOST - 1) / MOST);
		small_w = (int)((cx + every - 1) / every);
		small_h = (int)((cy + every - 1) / every);
		small.resize((size_t)small_w * small_h * 4);
		for (int y = 0; y < small_h; y++)
			for (int x = 0; x < small_w; x++) {
				const uint8_t *from = points + ((size_t)y * every * cx + (size_t)x * every) * 4;
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

	// An animated GIF on `ns` (to its next picture, if it's time), on OBS's
	// graphics thread.
	void animate(uint64_t ns)
	{
		if (image.loaded() && image.is_animated_gif() && image.tick(ns))
			image.update_texture();
	}
	gs_texture_t *texture() const { return image.loaded() ? image.texture() : nullptr; }
	float width() const { return image.loaded() ? (float)image.cx() : 0; }
	float height() const { return image.loaded() ? (float)image.cy() : 0; }
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

// The 3D headpat hand (assets/react-hand.glb: a right hand, its palm's
// middle at 0, its fingers along -x, its thumb along +y, its palm facing
// +z) as it pats a head, the head's facing way (x right as the picture
// shows it, y up, z towards you), from the picture's left (side < 0) or
// right: palm down, its fingers across the top of the head (tipped down
// over its far side) and a little towards you, its back tipped towards
// you, its forearm back and up over its side - a right hand from the
// left, a left one (mirrored) from the right, so its thumb is on the far
// side.
M4 hand_turn(float side)
{
	// From the right, flat: fingers to the picture's left, thumb away.
	M4 m;
	const float flat[16] = {1, 0, 0, 0, 0, 0, -1, 0, 0, -1, 0, 0, 0, 0, 0, 1};
	std::copy(flat, flat + 16, m.m);
	m = matrix_of(axis_angle({0, 0, 1}, 12 * RAD) * axis_angle({0, 1, 0}, 25 * RAD) *
		      axis_angle({1, 0, 0}, 30 * RAD)) *
	    m;
	if (side < 0) {
		M4 flip;
		flip.m[0] = -1;
		m = flip * m;
	}
	return m;
}

// A colour multiplier from DireBot ([r, g, b], linear; 1, 1, 1 as made).
V3 tint_of(const json &j)
{
	if (!j.is_array() || j.size() != 3)
		return {1, 1, 1};
	float c[3];
	for (int i = 0; i < 3; i++)
		c[i] = j[i].is_number() ? clampf(j[i].get<float>(), 0, 4) : 1.0f;
	return {c[0], c[1], c[2]};
}

// A 3D file (not a picture), by its name's end.
bool is_3d_file(const std::string &file)
{
	std::string end = file.size() >= 5 ? file.substr(file.size() - 5) : file;
	for (char &c : end)
		c = (char)std::tolower((unsigned char)c);
	return end.size() >= 4 && (end.compare(end.size() - 4, 4, ".glb") == 0 || end == ".gltf" ||
				   end.compare(end.size() - 4, 4, ".vrm") == 0);
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

	// Your face from a phone app (phone.hpp; the link's thread), and when
	// it came: worked on as a picture would be - the newest only.
	void give_phone(const PhoneLook &look, uint64_t ns)
	{
		{
			std::lock_guard<std::mutex> l(lock_);
			phone_in_ = look;
			phone_ns_ = ns;
			have_phone_ = true;
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
		// How lit the camera's picture is lately (alive.hpp), while it
		// sees your face.
		Light light;
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
		out.light = light_;
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
		PhoneLook phone;
		Pieces pieces;
		Light light;
		uint64_t window_start = os_gettime_ns();
		int window_looks = 0;
		double window_ms = 0;
		for (;;) {
			int w = 0, h = 0;
			uint64_t ns = 0;
			bool look = false, from_phone = false, forget = false, start = false, load = false;
			std::string text;
			float aspect[PARTS], soft;
			{
				std::unique_lock<std::mutex> l(lock_);
				wake_.wait(l, [this] { return stop_ || have_ || have_phone_ || load_rest_ || start_rest_; });
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
				} else if (have_phone_) {
					phone = phone_in_;
					ns = phone_ns_;
					have_phone_ = false;
					look = from_phone = true;
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
			if (forget) {
				tracker.forget();
				light = Light();
			}
			tracker.set_smoothing(smoothing_);
			const bool was_setting = tracker.setting();
			if (look && ok && from_phone) {
				// No picture: no eyes and mouth to cut out, nothing lit.
				tracker.take(phone, (double)ns / 1e9);
				pieces = Pieces();
				light = Light();
			} else if (look && ok) {
				const Frame frame{rgba.data(), w, h, w * 4, false};
				tracker.look(frame, (double)ns / 1e9);
				cut_pieces(tracker.face(), aspect, soft, pieces);
				skin_round(frame, pieces);
				// How lit it is, over about the last half second.
				const Light now = light_of(frame, tracker.face());
				if (now.seen) {
					const float k = light.seen ? 0.1f : 1.0f;
					light.face += (now.face - light.face) * k;
					light.left += (now.left - light.left) * k;
					light.right += (now.right - light.right) * k;
					light.room += (now.room - light.room) * k;
					light.seen = true;
				}
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
					light_ = light;
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
	bool stop_ = false, have_ = false, have_phone_ = false, forget_ = false, load_rest_ = false, start_rest_ = false;
	bool pending_ = false, setting_ = false;
	std::vector<uint8_t> in_;
	int in_w_ = 0, in_h_ = 0;
	uint64_t in_ns_ = 0;
	PhoneLook phone_in_;
	uint64_t phone_ns_ = 0;
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
	Light light_;
};

// Your body and hands (body.hpp), on a thread of their own as your face
// is. Its models load the first time "Follow my hands and arms" is on;
// while it's off nothing goes to it. OBS hands it a copy of the picture
// the face tracker gets, at most every HANDS_EVERY (light:
// LIGHT_HANDS_EVERY; arms only, ARMS_EVERY and LIGHT_ARMS_EVERY); the body
// model runs every ARMS_EVERY (light: LIGHT_ARMS_EVERY) of them.
class BodyWorker {
public:
	BodyWorker(std::string task, std::string lite, std::string palm)
		: task_(std::move(task)),
		  lite_(std::move(lite)),
		  palm_(std::move(palm))
	{
		thread_ = std::thread([this] { run(); });
	}
	~BodyWorker() { stop(); }

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

	// On (its models load, once) or off; your hands looked for too or
	// not; the light hand model; the smoothing (0 steadiest, 1 snappiest).
	void set(bool on, bool hands, bool light, float smoothing)
	{
		{
			std::lock_guard<std::mutex> l(lock_);
			if (on_ && !on)
				forget_ = true;
			on_ = on;
			hands_ = hands;
			light_ = light;
			smoothing_ = smoothing;
		}
		wake_.notify_one();
	}

	// A new picture (the graphics thread), copied: one not yet worked on
	// is replaced.
	void give(const std::vector<uint8_t> &rgba, int w, int h, uint64_t ns)
	{
		{
			std::lock_guard<std::mutex> l(lock_);
			if (!on_)
				return;
			in_.assign(rgba.begin(), rgba.end());
			in_w_ = w;
			in_h_ = h;
			in_ns_ = ns;
			have_ = true;
		}
		wake_.notify_one();
	}

	// A new camera: the next look starts afresh.
	void forget()
	{
		{
			std::lock_guard<std::mutex> l(lock_);
			forget_ = true;
		}
		wake_.notify_one();
	}

	// For "Check my camera": how often each of your hands (your own left
	// and right) was found over the last few seconds' looks (0 to 1), and
	// how lit it was when it was (its palm, and just round it: 0 black to
	// 1 white).
	struct HandsSeen {
		float found = 0;
		bool lit = false;
		float light = 0, around = 0;
	};
	static constexpr float FOUND_TIME = 1.5f, LIT_TIME = 0.5f;
	struct State {
		Body body;
		uint64_t ns = 0, looks = 0;
		bool on = false, loading = false, ready = false, palm = false;
		std::string problem;
		float rate = 0, ms = 0; // looks a second (at your hands), and how long each takes
		float body_rate = 0;    // ...of them, the body model's
		HandsSeen hands[2];
	};
	void state(State &out) const
	{
		std::lock_guard<std::mutex> l(lock_);
		out.body = body_;
		out.ns = body_ns_;
		out.looks = looks_;
		out.on = on_;
		out.loading = loading_;
		out.ready = ready_;
		out.palm = palm_ready_;
		out.problem = problem_;
		out.rate = rate_;
		out.ms = ms_;
		out.body_rate = body_rate_;
		out.hands[0] = hands_seen_[0];
		out.hands[1] = hands_seen_[1];
	}

private:
	void run()
	{
		BodyTracker tracker;
		bool loaded = false;
		std::vector<uint8_t> rgba;
		uint64_t window_start = os_gettime_ns();
		int window_looks = 0, window_body = 0;
		double window_ms = 0;
		uint64_t seen_ns = 0;
		for (;;) {
			bool load = false, look = false, forget = false, hands, light;
			float smoothing;
			int w = 0, h = 0;
			uint64_t ns = 0;
			{
				std::unique_lock<std::mutex> l(lock_);
				wake_.wait(l, [this] { return stop_ || have_ || forget_ || (on_ && !tried_); });
				if (stop_)
					return;
				if (on_ && !tried_) {
					load = true;
					tried_ = true;
					loading_ = true;
				}
				if (have_) {
					rgba.swap(in_);
					w = in_w_;
					h = in_h_;
					ns = in_ns_;
					have_ = false;
					look = on_;
				}
				forget = forget_;
				forget_ = false;
				hands = hands_;
				light = light_;
				smoothing = smoothing_;
			}
			if (load) {
				std::string problem;
				loaded = tracker.load_files(task_, lite_, &problem, palm_);
				if (!loaded)
					obs_log(LOG_WARNING, "DireBot Face Avatar: %s", problem.c_str());
				std::lock_guard<std::mutex> l(lock_);
				ready_ = loaded;
				palm_ready_ = tracker.has_palm();
				problem_ = problem;
				loading_ = false;
			}
			if (forget)
				tracker.forget();
			tracker.set_hands(hands);
			tracker.set_light(light);
			tracker.set_body_every(hands ? (light ? LIGHT_ARMS_EVERY : ARMS_EVERY) : 0);
			tracker.set_smoothing(smoothing);
			look = look && loaded;
			if (look) {
				const Frame frame{rgba.data(), w, h, w * 4, false};
				tracker.look(frame, (double)ns / 1e9);
				window_looks++;
				window_body += tracker.body().fresh;
				window_ms += tracker.body().ms;
			}
			const uint64_t now = os_gettime_ns();
			std::lock_guard<std::mutex> l(lock_);
			if (forget) {
				seen_ns = 0;
				hands_seen_[0] = hands_seen_[1] = HandsSeen();
			}
			if (look) {
				body_ = tracker.body();
				body_ns_ = ns;
				looks_++;
				// How often each hand's found, and how lit, eased over the
				// camera's time.
				const float dt = seen_ns && ns > seen_ns ? std::min(1.0f, (float)((ns - seen_ns) / 1e9))
									 : 0;
				const float k = 1 - std::exp(-dt / FOUND_TIME), kl = 1 - std::exp(-dt / LIT_TIME);
				for (int side = 0; side < 2; side++) {
					const HandSeen &hand = body_.hands[side];
					HandsSeen &hs = hands_seen_[side];
					hs.found = seen_ns ? hs.found + ((hand.found ? 1.0f : 0.0f) - hs.found) * k
							   : (hand.found ? 1.0f : 0.0f);
					if (hand.found) {
						const float a = hs.lit ? kl : 1;
						hs.light += (hand.light - hs.light) * a;
						hs.around += (hand.around - hs.around) * a;
						hs.lit = true;
					}
				}
				seen_ns = ns;
			} else if (forget) {
				body_ = Body();
				looks_++;
			}
			if (now - window_start >= 2'000'000'000ULL) {
				rate_ = window_looks * 1e9f / (float)(now - window_start);
				body_rate_ = window_body * 1e9f / (float)(now - window_start);
				ms_ = window_looks ? (float)(window_ms / window_looks) : 0;
				window_start = now;
				window_looks = window_body = 0;
				window_ms = 0;
			}
		}
	}

	const std::string task_, lite_, palm_;
	mutable std::mutex lock_;
	std::condition_variable wake_;
	std::thread thread_;
	bool stop_ = false, have_ = false, forget_ = false, tried_ = false;
	bool on_ = false, hands_ = true, light_ = false;
	float smoothing_ = 0.5f;
	std::vector<uint8_t> in_;
	int in_w_ = 0, in_h_ = 0;
	uint64_t in_ns_ = 0;
	Body body_;
	uint64_t body_ns_ = 0, looks_ = 0;
	bool loading_ = false, ready_ = false, palm_ready_ = false;
	std::string problem_;
	float rate_ = 0, ms_ = 0, body_rate_ = 0;
	HandsSeen hands_seen_[2];
};

// Where the face models are: data/face, next to the add-on.
std::string models_path()
{
	char *path = obs_module_file("face/face_landmarker.task");
	std::string out = path ? path : "";
	bfree(path);
	return out;
}

// ...and the body models (Google's holistic one; a lighter hand model for
// light mode - without it, light mode uses the full one; and the palm
// finder - without it, a hand is only looked for at your wrist).
std::string data_file(const char *name, bool needed)
{
	char *path = obs_module_file(name);
	std::string out = path ? path : needed ? std::string("data/") + name : std::string();
	bfree(path);
	return out;
}

void save_rest_task(void *param);
void save_profile_task(void *param);
void save_arms_task(void *param);
void send_signs_task(void *param);

// Where the vendor events go (set_emit).
std::mutex emit_lock;
Emit emit_now;
void emit_event(const std::string &type, const json &data)
{
	Emit e;
	{
		std::lock_guard<std::mutex> l(emit_lock);
		e = emit_now;
	}
	if (e)
		e(type, data);
}

// Where its mouth comes from: your camera, your mic, or both - the mic
// while you talk, the camera between (smiles, a mouth held open).
enum MouthFrom { MOUTH_FROM_CAMERA, MOUTH_FROM_MIC, MOUTH_FROM_BOTH };
// Saved camera views at most.
constexpr size_t MOST_VIEWS = 3;
// How quickly the mic takes the mouth over, and gives it back (seconds).
constexpr float MIC_TAKE = 0.08f, MIC_GIVE = 0.2f;
// The page asking how the mic sounds keeps it listening this long
// (otherwise it listens only while the avatar's shown).
constexpr uint64_t LISTEN_NS = 3'000'000'000ULL;

bool ends_with(const char *s, const char *end)
{
	const size_t n = std::strlen(s), m = std::strlen(end);
	return n >= m && std::strcmp(s + n - m, end) == 0;
}

bool is_audio(obs_source_t *s)
{
	return s && obs_source_get_type(s) == OBS_SOURCE_TYPE_INPUT &&
	       (obs_source_get_output_flags(s) & OBS_SOURCE_AUDIO) != 0 &&
	       std::strcmp(obs_source_get_unversioned_id(s), SOURCE_ID) != 0;
}

// The mic "" means: OBS's own Mic/Aux (Settings > Audio, the first one
// set), or else the first mic source in OBS - with a reference to release.
obs_source_t *default_mic()
{
	for (uint32_t channel = 3; channel <= 6; channel++) {
		obs_source_t *s = obs_get_output_source(channel);
		if (is_audio(s))
			return s;
		obs_source_release(s);
	}
	obs_source_t *first = nullptr;
	obs_enum_sources(
		[](void *param, obs_source_t *s) {
			const char *id = obs_source_get_unversioned_id(s);
			if (!id || !ends_with(id, "_input_capture") || !is_audio(s))
				return true;
			*static_cast<obs_source_t **>(param) = obs_source_get_ref(s);
			return false;
		},
		&first);
	return first;
}

struct FaceAvatar {
	obs_source_t *self = nullptr;
	obs_weak_source_t *weak_self = nullptr;
	// 0.9.0: "DireBot Throws", a filter on your camera - no character:
	// what's thrown lands on your real face in the camera's own picture
	// (its camera is the source it's on). Asleep (OBS draws the camera as
	// if it weren't there) until something is thrown.
	const bool as_filter = false;
	// ...thrown at lately (asleep after this), whether the camera jolts as
	// things hit, and your face as last seen in it (the camera's pixels).
	std::atomic<uint64_t> wake_until{0};
	std::atomic<bool> camera_shake{true};
	struct CameraFace {
		bool seen = false; // ever, since it woke
		bool now = false;  // in the newest look
		float x = 0, y = 0, head = 0, roll = 0, head_top = 0.6f;
	};
	CameraFace cam_face, cam_face_state; // (the copy under state_lock, for FaceGetState)

	// Set on OBS's UI thread (update), read while drawing.
	std::mutex lock, find_lock;
	std::string camera_name;
	obs_weak_source_t *camera = nullptr;
	// Its size from the settings - or, worn on your head, the camera's.
	std::atomic<uint32_t> width{1080}, height{1080};
	std::atomic<uint32_t> cam_w{0}, cam_h{0};
	std::atomic<float> cam_fps{0}; // the camera's new pictures a second (LookPace::rate)
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
	// 0.9.0: a 3D character's spot moved across the picture (a share of
	// its width, + right): a box made wider for a companion beside it.
	float spot_x = 0;
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
		std::string animation; // its own animation that plays, by name ("" none)
		// Its parts whose own face your face doesn't move, a name a line.
		std::string still_faces;
		int framing = 0; // 0 head and shoulders for a VRM, 1 all of it, 2 head
		bool hide = true, smooth = true;
		float fingers = 0.5f; // its fingers: 0 as made, 0.5 relaxed, 1 curled (RigOptions)
		WornPlace place;
		// Its lights (lights.hpp): a ready-made set or its own, and the
		// same in plain terms for the page; its normal and occlusion maps
		// used; soft skin on the parts that are skin (skin_materials).
		int light_preset = LIGHTS_STUDIO;
		LightTerms light_terms;
		Lights lights;
		bool detail = true;
		float soft_skin = 0;
		std::string skin_parts;
		// Its saved camera views (up to MOST_VIEWS), the one shown (-1 as
		// it's framed) and how long it glides from one to the next.
		std::vector<std::pair<std::string, CameraView>> views;
		int view = -1;
		float view_glide = 0.6f;
		// Its set-up (DireBot's "model_setup": an imported Lens Studio
		// project, spots dragged into place), and as text, to see changes.
		std::shared_ptr<const CharacterSetup> setup = std::make_shared<CharacterSetup>();
		std::string setup_text;
	};
	Look3D look3d; // under pictures_lock
	ModelLoader loader;
	// Your face's tuning, alive when you're still, and how much its body
	// follows your head (alive.hpp; under pictures_lock).
	FaceTune tune;
	AliveOptions alive_options;
	float body_follow = 0.3f;

	Worker worker;
	// Your face from a phone app instead of the camera (phone.hpp, 0.9.0):
	// "face_from" in the settings. The link asks the phone and hands each
	// look to the worker, on a thread of its own. The camera, if one is
	// picked, is then only looked at for your hands. Graphics thread: the
	// phone sent a look lately.
	std::atomic<bool> from_phone{false};
	PhoneLink phone;
	bool phone_live = false; // (or from a tracker app, below)
	// Moves from another tracker app over VMC (vmc.hpp, 0.9.0): your face
	// ("face_from" "vmc": through the tracker, as a phone's), your body and
	// legs ("vmc_body") and your arms, hands and fingers ("vmc_arms") on a
	// 3D VRM character in its own spot - each the app's while it sends it,
	// eased in and out, the rest as it was. "vmc_mirror": its left and
	// right swapped. The link is shared by every face avatar on the port,
	// held while any of these is on (under vmc_lock: the settings' thread
	// sets it, the graphics thread reads it).
	std::atomic<bool> from_vmc{false}, vmc_body{false}, vmc_arms{false}, vmc_mirror{false};
	std::atomic<bool> vmc_rehome{false};
	std::mutex vmc_lock;
	std::shared_ptr<VmcLink> vmc_link;
	// Graphics thread: what it sent and when, the face last handed over,
	// each part's share now (body, arms, fingers: 0 ours to 1 the app's)
	// and each bone as last sent (held while a part eases out), its hips'
	// home, and what goes to the rig.
	VmcSent vmc_sent;
	double vmc_now = 0;
	uint64_t vmc_changes = 0;
	bool vmc_face_live = false;
	float vmc_share[3] = {0, 0, 0};
	Quat vmc_held[VMC_BONES];
	bool vmc_held_has[VMC_BONES] = {};
	V3 vmc_home;
	bool vmc_home_set = false;
	TrackedBody tracked_now;
	std::vector<FaceNow> vmc_faces_now;
	// For FaceGetState (under state_lock): what it sends, what's used.
	VmcSends vmc_sends_state;
	bool vmc_using_state[4] = {false, false, false, false}; // face, body, arms, fingers
	const char *vmc_why_state = "";
	bool face_from_camera() const { return !from_phone && !from_vmc; }
	bool vmc_face_sending()
	{
		std::lock_guard<std::mutex> l(state_lock);
		return vmc_using_state[0];
	}
	// Gaming hands (gamehands.hpp, 0.9.0): a 3D VRM character's hands on
	// its own keyboard, mouse and game pad as you use yours ("game_hands",
	// off at first). Your keyboard, mouse and pads are read for every face
	// avatar with them on (devices.hpp: only THAT a key went down, never
	// which; nothing kept), held while they're on (under game_lock: the
	// settings' thread sets it, the graphics thread reads it); "Show me"
	// (DireBot's FaceGame) pretends for PRETEND_TIME seconds instead. Its
	// props' files, each DeskProp's (obs_module_file; under pictures_lock).
	std::atomic<bool> game_on{false}, game_show{false}, game_stop{false};
	std::mutex game_lock;
	std::shared_ptr<DeviceReader> game_reader;
	GameOptions game_options;
	std::string game_files[DESK_PROPS];
	// Graphics thread: the hands, on a character (else why not, for the
	// page), "Show me"'s time so far (-1 not showing), this frame's, and
	// round its desk (the facing model's way: framed with its head).
	GameHands game;
	bool game_wanted = false;
	const char *game_why = "off";
	double game_show_t = -1;
	// (With companions, 0.9.0) a move that stays (a ride) has its arms and
	// body while it's on: its desk is away (0 here to 1 gone), fading.
	float game_away = 0;
	DeskNow desk_now;
	Bounds desk_bounds, framed_desk;
	// For FaceGetState (under state_lock).
	DeskNow desk_state;
	DeviceState devices_state;
	bool game_wanted_state = false, game_reading_state = false;
	const char *game_why_state = "off";
	double game_show_state = -1;
	// Your hands and arms (body.hpp, arms.hpp), on a 3D VRM character in
	// its own spot: looked for on their own thread, followed on the
	// graphics one. On and light mode from the settings.
	BodyWorker body_worker;
	std::atomic<bool> arms_on{false}, arms_light{false}, arms_hands{true};
	std::atomic<bool> reset_arms{false};
	ArmOptions arm_options; // under pictures_lock
	// The solver: followed on the graphics thread, calibrated from
	// DireBot's FaceArms on the UI one.
	std::mutex arms_lock;
	ArmSolver arms;
	bool arms_pending = false; // a calibration made here, not in the settings yet
	std::string arms_cal_seen; // "arms_calibration" as last read or saved
	// Graphics thread: on, with a character to follow them on (else why
	// not, for the page), the newest look's, and what it follows now.
	bool arms_wanted = false;
	const char *arms_why = "off";
	uint64_t body_seen_looks = 0, body_given_ns = 0;
	BodyWorker::State body_seen;
	ArmsNow arms_now;
	// Hand signs and face moves that set things off (signs.hpp): on, with
	// a hand sign to watch for, with anything to; watching while it's
	// hidden too; and the rules, from the settings (UI thread), taken on
	// the graphics thread.
	std::atomic<bool> signs_on{false}, signs_hands{false}, signs_any{false}, signs_hidden{false};
	std::mutex signs_lock;
	std::vector<SignRule> signs_new;
	bool signs_changed = false;
	// Graphics thread: the watch, whether it wants your hands looked at
	// now, the body look it last saw, when a picture last went to the
	// trackers, and the camera held shown while it looks with the avatar
	// hidden.
	SignWatch signs;
	bool signs_hands_wanted = false;
	uint64_t signs_body_looks = 0;
	uint64_t last_look_ns = 0;
	obs_source_t *hidden_cam = nullptr;
	// Only on OBS's graphics thread (video_tick and video_render).
	Shown shown;
	// Your moves through the tuning, and what it does by itself on top
	// (alive.hpp), this frame.
	float tuned_now[EXPRESSIONS] = {};
	Alive alive;
	AliveOut alive_now;
	int held_turned = 0; // the way you last faced (Shown::Out::turned), while it's held
	// The tuning's readings, and what the character shows from them,
	// each held at its highest for PEAK_TIME seconds, for the page's live
	// bars: a blink is over before the page asks again.
	static constexpr double PEAK_TIME = 0.6;
	struct Peak {
		float value = 0;
		double at = -1e9;
		void take(float v, double now)
		{
			if (v >= value || now - at > PEAK_TIME) {
				value = v;
				at = now;
			}
		}
	};
	Peak reading_peak[TUNE_GROUPS], shows_peak[TUNE_GROUPS];
	double clock = 0;
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
	int newest_shot = -1; // the one look() drew this frame (0.9.0: the throws filter's camera)
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
		    *p_ring = nullptr, *p_ring_on = nullptr, *p_card_light = nullptr;
	EffectCheck check;
	bool drawing = false, drawn = false, have_out = false;
	std::vector<uint8_t> mask_bytes;
	// What it showed last, for FaceGetState.
	std::mutex state_lock;
	Shown::Out shown_now;
	Worker::State state_now;
	Drawn drawn_state;
	float tune_reading[TUNE_GROUPS] = {}, tune_reading_peak[TUNE_GROUPS] = {};
	float tune_shows[TUNE_GROUPS] = {}, tune_shows_peak[TUNE_GROUPS] = {};
	float idle_state = 1, breath_state = 0;
	// Where the character goes this frame.
	struct Placed {
		bool shown = false;
		int which = 0;              // its picture: 0 the usual, 1 turned left, 2 turned right
		float x = 0, y = 0;         // where its anchor goes (the source's pixels)
		float ax = 0, ay = 0;       // its anchor (pixels of the canvas, as it's turned)
		float scale = 1, angle = 0; // the source's pixels a character pixel; radians
		float yaw = 0, pitch = 0;   // degrees
		float opacity = 1;
		// Squished by a headpat (react.hpp: 0 not at all, 1 the most):
		// shorter and a little wider, from its bottom edge.
		float squish = 0;
	};
	// How much shorter and wider a picture character is, squished all the
	// way (all of it squishes, so less than a 3D character's head).
	static constexpr float SQUISH_TALL_2D = 0.05f, SQUISH_WIDE_2D = 0.03f;
	Placed placed_now;
	bool showing_now = false;
	uint64_t last_render = 0;
	// The 3D character (OBS's graphics thread only)...
	std::unique_ptr<Avatar3D> avatar3d;
	std::unique_ptr<Rig> rig;
	std::shared_ptr<const Model> model_now;
	std::string rig_key;
	// Its box as it stands (Rig::rest_bounds), which "All of it" frames in
	// its own spot, and the framing its shape for FaceGetState was worked
	// out for (-1: not yet) - 0.8.1.
	Bounds stands3d;
	int framed_for = -1;
	int stands_for = -1; // the animation its box was measured in (0.9.0)
	// 0.9.0: a companion beside it (companion.hpp): a pet of its own file,
	// loaded on its own thread, posed by a rig of its own and drawn with
	// the character in the same picture, patted by a hand of its own.
	// Asked for by DireBot's FaceReact ("companion") on OBS's UI thread,
	// under companion_lock; stepped and drawn on the graphics one.
	struct CompanionAsk {
		std::string file;                   // "" none
		std::string clips[COMPANION_CLIPS]; // its animations asked for by name ("" worked out)
		CompanionPlace place;
		// How much it moves with the character as the character follows
		// you about (0 it sits still, 1 with it all the way).
		float follow = 1;
		// The camera glides to `view` while it's here (else it stays as
		// it is).
		bool has_view = false;
		CameraView view;
		int come = 0;           // waiting for its file: 1 it comes, 2 straight there
		uint64_t clips_seq = 1; // a new one each time its clips are asked for anew
		// The file loaded and its clips sorted out for `ready_seq` (the
		// graphics thread says so).
		std::string ready;
		uint64_t ready_seq = 0;
	};
	std::mutex companion_lock;
	CompanionAsk companion_ask;
	CompanionPlayer companion_player;  // under companion_lock
	React companion_hand{0x51ED270Bu}; // its pats (under companion_lock)
	ModelLoader companion_loader;
	// ...graphics thread: loaded, rigged, measured as it sits; this frame's.
	std::unique_ptr<Avatar3D> companion3d;
	std::unique_ptr<Rig> companion_rig;
	std::shared_ptr<const Model> companion_model;
	std::string companion_path; // its file, as loaded
	std::string companion_problem;
	Bounds companion_box;
	int companion_pat = -1;
	uint64_t companion_clips_seen = 0;
	CompanionAsk companion_shown;
	CompanionNow companion_now;
	Hand companion_hand_now;
	bool companion_drawn = false;
	float companion_tall = 0; // its height in the world, sitting, as drawn
	M4 companion_rest;        // the character -> the world where its companion's spot is
	// ...for FaceGetState (under state_lock).
	struct CompanionState {
		std::string file, problem, pat;
		bool loaded = false, drawn = false;
		std::vector<std::string> animations;
		std::vector<float> lengths; // each animation's, seconds
		std::string clips[COMPANION_CLIPS];
		CompanionPhase phase = CompanionPhase::Away;
		bool busy = false, petting = false, patting = false, camera = false;
		int move = -1;  // one of its own moves playing (its animation)
		float size = 0; // its height in the world, sitting (0 not drawn)
	};
	CompanionState companion_state;
	// 0.9.0: a move that stays - the character's own animation played
	// once ("dragon_come"), then another looping as its idle in place of
	// its own ("dragon_idle", riding) until it's ended by another move
	// ("dragon_go") back to its own idle. Asked for on OBS's UI thread
	// (under react_lock: its move goes to React with it), stepped on the
	// graphics one.
	struct StayAsk {
		uint64_t seq = 0;
		bool start = false; // false: it ends
		std::string name;   // its idle's (for FaceGetState)
		int move = -1;      // the move to it (or back), one of its own animations
		float length = 0;
		int idle = -1; // starting: the idle it stays in
		bool has_view = false;
		CameraView view;
	};
	StayAsk stay_ask; // under react_lock
	// ...graphics thread: the newest asked for, whether its idle is in
	// yet (or its own back), the idle it has swapped in (-1 its own), and
	// one to swap in as soon as the rig's there (how long it's waited).
	StayAsk stay_job;
	uint64_t stay_seen = 0;
	bool stay_swapped = true, stay_on = false;
	int stay_idle_now = -1;
	bool stay_due = false, stay_aligned = false;
	int stay_target = -1;
	float stay_offset = 0, stay_fade = 0, stay_age = 0, stay_ease = 0, stay_amount = 0;
	// Hidden on every scene that's shown (OBS doesn't draw it): how long
	// since it last looked for a newly loaded 3D character (0.8.1).
	float since_hidden_pick = 0;
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
		// Its own face can blink / open its mouth where you'd see it
		// (known once it's rigged).
		bool rigged = false, blinks = false, talks = false;
		size_t points = 0, triangles = 0;
		int springs = 0, arkit = 0, upright = 0, cards = 0, layers = 0;
		// Finger joints it curls, and a body (spine, chest) that breathes
		// and follows your head.
		int fingers = 0;
		bool body = false;
		// Its bones that follow others (VRM's node constraints: twist
		// bones...), and how many its file has that can't be.
		int following = 0, following_left_out = 0;
		// Its own animations' names (and lengths, seconds), and the one
		// playing ("" none).
		std::vector<std::string> animations;
		std::vector<float> animation_lengths;
		std::string animation;
		// Its materials: each one's name, whether it has a normal map
		// ("bumps") and an occlusion map, and whether it counts as skin.
		struct Mat {
			std::string name;
			bool bumps = false, shadows = false, skin = false;
			// Bumps on tangents worked out (the file had none of its own):
			// close to what it was made with, but not always the same.
			bool guessed_tangents = false;
		};
		std::vector<Mat> materials;
		// Each spot (its own left eye, right eye, mouth) on its front
		// picture (shares of FaceSpots::box) and how it was found.
		float spot[3][4] = {};
		const char *spot_from[3] = {"", "", ""};
		// How wide and tall what's framed in its own spot is (the model's
		// units; 0 not known yet) - 0.8.1.
		float framed_w = 0, framed_h = 0;
		float yaw = 0, pitch = 0, x = 0, y = 0;
		std::map<std::string, float> values;
	};
	State3D state3d;
	// The mouth from your mic (voice.hpp): its sound comes in on OBS's
	// audio thread (heard), the mouth is worked out on the graphics one.
	Voice voice;
	std::mutex mic_lock, mic_find_lock;
	std::string mic_name;             // "" OBS's own Mic/Aux (default_mic)
	obs_weak_source_t *mic = nullptr; // the one it listens to (under mic_lock)
	std::string mic_found;            // its name (under mic_lock)
	std::atomic<int> mouth_from{MOUTH_FROM_BOTH};
	// Listening while it's shown, or till then (the page's level bar).
	std::atomic<uint64_t> listen_until{0};
	std::string profile_seen; // "mic_profile" as last read or saved (under lock)
	float since_mic_find = 0;
	MicMouth mic_now; // graphics thread
	// The camera view gliding from one to the next, and which materials
	// are skin (graphics thread).
	CameraView view_from, view_to, view_now;
	float view_t = 1;
	int view_shown = -1;
	bool view_started = false;
	int view_shown_state = -1; // ...for FaceGetState (under state_lock)
	bool view_gliding_state = false;
	const char *view_cue_state = ""; // 0.9.0: "companion" or "stay" while one has the camera
	std::vector<bool> skin_now;
	std::string skin_key;
	// What it does on cue (react.hpp): asked for by DireBot's FaceReact on
	// OBS's UI thread, stepped and drawn on the graphics one.
	std::mutex react_lock;
	React react;
	// Its pictures for it (under pictures_lock): the things thrown (by
	// number), the patting hand and a hit's flash; and the VRM animation
	// files it plays, by name.
	std::vector<PicturePtr> throw_pics;
	PicturePtr hand_pic, hit_pic;
	// ...each thing's file (a 3D one has no picture), the 3D headpat hand's
	// (used if it's loaded, else hand_pic) and its skin's and sleeve's
	// colours; the 3D files loaded (or why not); and chat's emotes thrown,
	// the newest kept (EMOTE_IDS + where).
	std::vector<std::string> throw_files;
	// What each thing does as it hits (0.9.0, react.hpp's HitLook, by the
	// thing's number): its pictures by number in effect_pics.
	std::vector<HitLook> throw_looks;
	std::vector<PicturePtr> effect_pics;
	std::string hand3d_file;
	// A picture of the hand: where it touches the head and where its wrist
	// is (shares of its width and height, as it comes from the left), and
	// how wide it is (heads, at size 1).
	struct HandPlace {
		float touch[2] = {0.5f, 1}, wrist[2] = {0.2f, 1};
		float width = 0.85f;
	};
	HandPlace hand_place;
	V3 hand_skin{1, 1, 1}, hand_sleeve{1, 1, 1};
	std::set<std::string> props_ready;
	std::map<std::string, std::string> prop_problems;
	struct Emote {
		PicturePtr pic;
		uint64_t used = 0;
	};
	std::vector<Emote> emotes;
	uint64_t emotes_used = 0;
	// The 3D files themselves (OBS's graphics thread): loaded on their own
	// thread, onto the graphics card; and drawn over the character (things
	// thrown, a 2D character's 3D hand), this frame's.
	PropLoader prop_loader;
	std::map<std::string, std::unique_ptr<Prop3D>> props3d;
	std::unique_ptr<Avatar3D> overlay3d;
	gs_texture_t *overlay_tex = nullptr;
	struct NamedMotion {
		std::string name, file, problem;
		std::shared_ptr<const Motion> motion;
		int id = -1; // React::play's number for it (kept while the file is)
	};
	std::vector<NamedMotion> motions;
	int next_motion_id = MOTION_IDS;
	// Graphics thread: this frame's, and where its face was drawn (the
	// picture's pixels, without the moves' own shift), how tall its head
	// is (pixels) and the picture round it (heads), for the next step.
	ReactOut react_now;
	ReactView react_view;
	float face_px[2] = {0, 0}, head_px = 0, react_shift[2] = {0, 0};
	float head_world = 0; // a 3D character's head, in the world's units
	bool face_known = false;
	int react_hits = 0;
	// ...for FaceGetState (under state_lock): the faces it makes, its own
	// shapes, whether it waves with arms and plays VRM animation files.
	struct ReactState {
		std::vector<std::string> faces, shapes;
		bool arms = false, motions = false, three_d = false, busy = false, placed = false;
		int hits = 0, flying = 0;
		int marks = 0, bits = 0; // 0.9.0: left on its face, bits in the air
		float head_px = 0;
		Hand hand; // the headpat's hand now
		float squint = 0, squish = 0;
		int stuck = 0;
		bool moving = false; // a body or head move playing (0.9.0)
	};
	ReactState react_state;
	// ...and a move that stays (0.9.0): on, its idle's name, still
	// swapping (its move playing).
	struct StayState {
		bool on = false, swapping = false;
		std::string name;
	};
	StayState stay_state;
	// ...and how it sounds, for FaceGetState (under state_lock).
	VoiceOut voice_state;
	Calibration cal_state;
	float mic_share_state = 0;
	// ...and your hands and arms (under state_lock).
	BodyWorker::State body_state;
	ArmsSeen arms_seen_state;
	bool arms_wanted_state = false;
	const char *arms_why_state = "off";
	// ...and the signs it watches for (under state_lock).
	SignsSeen signs_state;
	size_t signs_rules_state = 0;
	bool signs_hands_state = false, signs_looking_state = false, looking_hidden_state = false;

	// The camera is looked at for your face - or, with your face from a
	// phone, only for your hands (the arms, or a hand sign). Graphics
	// thread.
	bool camera_wanted() const { return face_from_camera() || arms_wanted || signs_hands_wanted; }

	// How often your hands and arms are looked at, at most (seconds).
	float arms_every() const
	{
		return arms_hands || signs_hands ? (arms_light ? LIGHT_HANDS_EVERY : HANDS_EVERY)
						 : (arms_light ? LIGHT_ARMS_EVERY : ARMS_EVERY);
	}

	explicit FaceAvatar(obs_source_t *source, bool filter = false)
		: self(source),
		  weak_self(obs_source_get_weak_source(source)),
		  as_filter(filter),
		  worker(models_path(),
			 [weak = weak_self](const std::string &text) {
				 // Saved in the settings on OBS's UI thread (save_rest_task).
				 obs_weak_source_addref(weak);
				 auto *job = new std::pair<obs_weak_source_t *, std::string>(weak, text);
				 obs_queue_task(OBS_TASK_UI, save_rest_task, job, false);
			 }),
		  phone([this](const PhoneLook &look, uint64_t) {
			  // On OBS's clock, as the camera's pictures are.
			  if (from_phone)
				  worker.give_phone(look, os_gettime_ns());
		  }),
		  body_worker(data_file("face/holistic_landmarker.task", true),
			      data_file("face/hand_landmark_lite.tflite", false),
			      data_file("face/palm_detection_full.tflite", false))
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
			p_card_light = param("card_light");
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
		// Not listening any more (OBS waits for a sound under way first).
		obs_source_t *m = get_mic();
		if (m) {
			obs_source_remove_audio_capture_callback(m, heard, this);
			obs_source_release(m);
		}
		obs_weak_source_release(mic);
		if (hidden_cam) {
			obs_source_dec_showing(hidden_cam);
			obs_source_release(hidden_cam);
			hidden_cam = nullptr;
		}
		from_phone = false;
		phone.stop();
		from_vmc = false;
		{
			std::lock_guard<std::mutex> l(vmc_lock);
			vmc_link.reset();
		}
		game_on = false;
		{
			std::shared_ptr<DeviceReader> old;
			std::lock_guard<std::mutex> l(game_lock);
			old.swap(game_reader);
		}
		worker.stop();
		body_worker.stop();
		obs_enter_graphics();
		avatar3d.reset();
		overlay3d.reset();
		companion_rig.reset();
		companion3d.reset();
		props3d.clear();
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
		if (as_filter) { // the source it's on
			obs_source_t *parent = obs_filter_get_parent(self);
			return parent ? obs_source_get_ref(parent) : nullptr;
		}
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
			reset_arms = true;
			body_worker.forget();
		}
		obs_source_release(old);
		obs_source_release(found);
	}

	// Listening at least this much longer (ns), shown or not.
	void listen_for(uint64_t ns)
	{
		const uint64_t want = os_gettime_ns() + ns;
		uint64_t now = listen_until.load();
		while (now < want && !listen_until.compare_exchange_weak(now, want)) {
		}
	}

	obs_source_t *get_mic()
	{
		std::lock_guard<std::mutex> l(mic_lock);
		return mic ? obs_weak_source_get_source(mic) : nullptr;
	}

	// The mic's sound, on OBS's audio thread: only while it's shown, or
	// the page is showing how it sounds. As loud as OBS's own meter has
	// it (after its volume slider).
	static void heard(void *param, obs_source_t *source, const audio_data *data, bool muted)
	{
		auto *f = static_cast<FaceAvatar *>(param);
		if (!data || !data->frames)
			return;
		if (!obs_source_showing(f->self) && os_gettime_ns() > f->listen_until.load())
			return;
		audio_t *audio = obs_get_audio();
		if (!audio)
			return;
		const size_t channels = std::min<size_t>(audio_output_get_channels(audio), MAX_AV_PLANES);
		const float *planes[MAX_AV_PLANES] = {};
		for (size_t c = 0; c < channels; c++)
			planes[c] = reinterpret_cast<const float *>(data->data[c]);
		f->voice.feed(planes, (int)channels, data->frames, audio_output_get_sample_rate(audio), muted,
			      obs_source_get_volume(source));
	}

	// The mic by its name ("" OBS's own Mic/Aux), listened to while its
	// mouth is from the mic (or both).
	void find_mic()
	{
		std::lock_guard<std::mutex> finding(mic_find_lock);
		std::string name;
		{
			std::lock_guard<std::mutex> l(mic_lock);
			name = mic_name;
		}
		obs_source_t *found = nullptr;
		if (mouth_from != MOUTH_FROM_CAMERA) {
			found = name.empty() ? default_mic() : obs_get_source_by_name(name.c_str());
			if (found && !is_audio(found)) {
				obs_source_release(found);
				found = nullptr;
			}
		}
		obs_source_t *old = get_mic();
		if (old != found) {
			if (old)
				obs_source_remove_audio_capture_callback(old, heard, this);
			voice.forget();
			if (found)
				obs_source_add_audio_capture_callback(found, heard, this);
			std::lock_guard<std::mutex> l(mic_lock);
			obs_weak_source_release(mic);
			mic = found ? obs_source_get_weak_source(found) : nullptr;
			mic_found = found ? obs_source_get_name(found) : "";
		}
		obs_source_release(old);
		obs_source_release(found);
	}

	// "Say each vowel" is done: its places saved in the settings
	// (save_profile_task, on the UI thread)...
	void profile_saved(const std::string &text)
	{
		std::lock_guard<std::mutex> l(lock);
		profile_seen = text;
	}

	// ...or forgotten: the usual places again.
	void forget_voice()
	{
		VoiceOptions o = voice.options();
		o.profile = VoiceProfile();
		voice.stop_calibrating();
		voice.set(o);
		obs_data_t *settings = obs_source_get_settings(self);
		obs_data_set_string(settings, "mic_profile", "");
		obs_data_release(settings);
		profile_saved("");
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
		last_look_ns = os_gettime_ns();
		if (staged && stage) {
			uint8_t *data = nullptr;
			uint32_t line = 0;
			bool gave = false;
			if (gs_stagesurface_map(stage, &data, &line)) {
				const bool go = pace.give(same_as_given(data, line));
				cam_fps = pace.rate();
				if (go) {
					const size_t row = (size_t)stage_w * 4;
					given.resize(row * stage_h);
					for (uint32_t y = 0; y < stage_h; y++)
						std::memcpy(&given[y * row], data + (size_t)y * line, row);
					// (Your face from a phone: the camera is only for your
					// hands.)
					if (face_from_camera()) {
						rgba.assign(given.begin(), given.end());
						worker.give(rgba, (int)stage_w, (int)stage_h, staged_ns);
					}
					gave = true;
					// Your hands and arms, maybe less often (a little
					// early is fine: the camera's pictures come unevenly).
					const float every = arms_every();
					if ((arms_wanted || signs_hands_wanted) &&
					    (staged_ns < body_given_ns || staged_ns - body_given_ns >= (uint64_t)(every * 0.9e9f))) {
						body_worker.give(given, (int)stage_w, (int)stage_h, staged_ns);
						body_given_ns = staged_ns;
					}
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
			if (face_from_camera())
				worker.forget();
			body_worker.forget();
			reset_arms = true;
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
		if (as_filter) {
			// The camera as the filters before this one leave it.
			if (obs_source_process_filter_begin(self, GS_RGBA, OBS_NO_DIRECT_RENDERING))
				obs_source_process_filter_end(self, obs_get_base_effect(OBS_EFFECT_DEFAULT), cw, ch);
		} else {
			ReadingCamera raw;
			obs_source_video_render(cam);
		}
		gs_texrender_end(shot.tex);
		newest_shot = slot;
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
		if (!face_from_camera()) { // no picture: nothing cut out
			used_shot = -1;
			return;
		}
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
			// within limits, gliding between looks. When it can't see you
			// it holds your last look a moment, then eases back to its own
			// spot and size (alive.hpp); it sways and breathes on top.
			const Shown::Glide &g = o.steady;
			const AliveOut &a = alive_now;
			const float fit = std::min(w / char_w, h / char_h) * size;
			const float grow =
				mix(clampf(std::pow(std::max(0.05f, g.scale), follow_grow), 0.6f, 1.6f), 1, a.idle);
			// Moved in proportion to its size: its eye gap, as it's shown.
			float its_gap = 0;
			if (have_spots) {
				const float lx = (spots[0].x + spots[0].w / 2) * char_w,
					    ly = (spots[0].y + spots[0].h / 2) * char_h;
				const float rx = (spots[1].x + spots[1].w / 2) * char_w,
					    ry = (spots[1].y + spots[1].h / 2) * char_h;
				its_gap = std::hypot(rx - lx, ry - ly) * fit * grow;
			}
			float dx = 0, dy = 0;
			follow_of(o, g, follow_move, its_gap / std::max(1u, w), its_gap / std::max(1u, h), dx, dy);
			dx = clampf(dx * flip * (1 - a.idle), -most_move, most_move);
			dy = clampf(dy * (1 - a.idle), -most_move, most_move);
			at.shown = true;
			at.x = w * (0.5f + dx);
			// A breath lifts it a little (up to 0.8% of its height).
			at.y = h * (0.5f + dy) - a.breath * 0.008f * char_h * fit * grow;
			at.ax = char_w / 2 - CX;
			at.ay = char_h / 2 - CY;
			at.scale = fit * grow;
			at.angle = clampf((a.roll * follow_tilt + a.sway_roll) * RAD * flip, -0.6f, 0.6f);
			if (shaped) {
				// A turned picture is already turned some of the way.
				float yaw = a.yaw * flip;
				if (which)
					yaw -= (which == 1 ? -1 : 1) * Shown::TURN_ON;
				at.yaw = clampf(yaw * bend + a.sway_yaw, -MOST_YAW, MOST_YAW);
				at.pitch = clampf((a.pitch * bend + a.sway_pitch) * 0.6f, -MOST_PITCH, MOST_PITCH);
			}
			// What it does on cue, as the picture shows it (react.hpp):
			// its head knocked back, nodding, shaking, bobbing.
			const ReactOut &rn = react_now;
			at.angle += rn.roll * RAD;
			if (shaped) {
				at.yaw += rn.yaw;
				at.pitch += rn.pitch * 0.6f;
			}
			react_shift[0] = rn.dx * head_px;
			react_shift[1] = rn.dy * head_px;
			at.x += react_shift[0];
			at.y += react_shift[1];
			at.squish = std::isfinite(rn.squish) ? clampf(rn.squish, -0.5f, 1.2f) : 0;
			return at;
		}
		react_shift[0] = react_shift[1] = 0;
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
		// (From a phone there's no picture to cut yours from: its own
		// drawn face moves with yours.)
		const bool real = !drawn_style && face_from_camera();
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
		if (at.squish != 0) {
			// Squished from its bottom edge (a headpat).
			const float bottom = CH - at.ay;
			gs_matrix_translate3f(0, bottom, 0);
			gs_matrix_scale3f(1 + SQUISH_WIDE_2D * at.squish, 1 - SQUISH_TALL_2D * at.squish, 1);
			gs_matrix_translate3f(0, -bottom, 0);
		}
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
		{
			ReadingCamera raw;
			obs_source_video_render(cam);
		}
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

	// Newly loaded 3D things (react.hpp's: the headpat hand, things to
	// throw) onto the graphics card, and those no longer wanted let go of
	// (graphics thread, pictures_lock held).
	void pick_props()
	{
		for (const std::shared_ptr<ModelPack> &pack : prop_loader.take()) {
			props3d.erase(pack->path);
			props_ready.erase(pack->path);
			prop_problems.erase(pack->path);
			if (pack->model) {
				props3d[pack->path] = std::make_unique<Prop3D>(pack->model, pack->pictures);
				props_ready.insert(pack->path);
			} else {
				prop_problems[pack->path] = pack->problem;
			}
		}
		for (auto it = props3d.begin(); it != props3d.end();) {
			const bool wanted =
				it->first == hand3d_file ||
				std::find(throw_files.begin(), throw_files.end(), it->first) != throw_files.end() ||
				std::find(std::begin(game_files), std::end(game_files), it->first) != std::end(game_files);
			if (wanted) {
				++it;
				continue;
			}
			props_ready.erase(it->first);
			it = props3d.erase(it);
		}
	}

	// The 3D headpat hand, if it's loaded (graphics thread).
	Prop3D *hand_3d()
	{
		if (hand3d_file.empty())
			return nullptr;
		auto it = props3d.find(hand3d_file);
		return it == props3d.end() ? nullptr : it->second.get();
	}

	// Gaming hands' prop `i` (DeskProp), if it's loaded (graphics thread).
	Prop3D *game_prop(int i)
	{
		if (i < 0 || i >= DESK_PROPS || game_files[i].empty())
			return nullptr;
		auto it = props3d.find(game_files[i]);
		return it == props3d.end() ? nullptr : it->second.get();
	}

	// Thing `n` thrown (react.hpp's Flying::picture), if it's a 3D one.
	Prop3D *thrown_3d(int n)
	{
		if (n < 0 || n >= (int)throw_files.size() || !is_3d_file(throw_files[n]))
			return nullptr;
		auto it = props3d.find(throw_files[n]);
		return it == props3d.end() ? nullptr : it->second.get();
	}

	// How tall thing `n` thrown is drawn for its size across (react.hpp's
	// Throw::tall): a 3D one's height over its longest side, as
	// draw_overlay sizes it; a picture's height over its width (pictures_lock
	// held).
	float drawn_tall(int n)
	{
		if (Prop3D *prop = thrown_3d(n)) {
			const V3 lo = prop->low(), hi = prop->high();
			return (hi.y - lo.y) / std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z, 1e-6f});
		}
		const PicturePtr &pic = thrown_picture(n);
		return loaded(pic) ? (float)pic->height() / std::max(1.0f, (float)pic->width()) : 1.0f;
	}

	// The 3D hand as react.hpp's Hand has it, round where its palm rests
	// (the hand's own units: as big as a hand for a head HAND_HEAD tall),
	// `side` the side it comes from as the world has it: its wrist bent
	// up, its skin and sleeve coloured.
	PropAt hand_at(Prop3D &hand, const M4 &rest_place, float side)
	{
		return hand_at(hand, rest_place, side, react_now.hand);
	}
	PropAt hand_at(Prop3D &hand, const M4 &rest_place, float side, const Hand &h)
	{
		PropAt p;
		p.prop = &hand;
		p.place = rest_place * scaled(h.size / HAND_HEAD) * hand_turn(side);
		p.opacity = h.opacity;
		const int lift = hand.shape("lift");
		if (lift >= 0) {
			p.shapes.assign((size_t)lift + 1, 0);
			p.shapes[lift] = h.lift;
		}
		const std::vector<Material> &mats = hand.model().materials;
		for (size_t i = 0; i < mats.size(); i++) {
			const std::string &n = mats[i].name;
			hand.tint((int)i, n.find("skin") != std::string::npos     ? hand_skin
					  : n.find("sleeve") != std::string::npos ? hand_sleeve
										  : V3{1, 1, 1});
		}
		return p;
	}

	// The 3D headpat hand on a 3D character's head: its palm on the top of
	// it (where it's highest under the palm), a little to its side and
	// back; lifted off it and out to its side as it comes and goes. It
	// moves with the head.
	PropAt hand_on_head(Prop3D &hand, Quat facing, bool wear)
	{
		const Hand &h = react_now.hand;
		const Model &m = *model_now;
		const Rig::HeadBox &hb = rig->head_box();
		float tall, wide, deep, crown;
		V3 mid;
		if (hb.found) {
			tall = hb.high.y - hb.low.y;
			wide = hb.high.x - hb.low.x;
			deep = hb.high.z - hb.low.z;
			mid = (hb.low + hb.high) * 0.5f;
			crown = hb.top;
		} else {
			// No head bone: from its eyes (its head 2.8 times as tall as
			// they're apart), up to the top of it all.
			const V3 le = rotate(facing, spots3d.spot[0].at), re = rotate(facing, spots3d.spot[1].at);
			const float gap = length(le - re);
			tall = gap > 1e-6f ? 2.8f * gap : 0.3f * std::max(1e-4f, m.high.y - m.low.y);
			wide = 0.7f * tall;
			deep = 0.85f * tall;
			mid = (le + re) * 0.5f - V3{0, 0.1f * tall, 0.4f * deep};
			crown = rotate(facing, m.high).y;
			for (int c = 0; c < 8; c++)
				crown = std::max(crown, rotate(facing, V3{c & 1 ? m.high.x : m.low.x,
									  c & 2 ? m.high.y : m.low.y,
									  c & 4 ? m.high.z : m.low.z})
								.y);
		}
		// Worn and mirrored, the picture is the world turned round.
		const float side = wear && mirror ? -h.side : h.side;
		const float cx = mid.x + side * HAND_OVER * wide, cz = mid.z - HAND_BACK * deep;
		const float cy = hb.found ? hb.top_at(cx, cz, HAND_REACH * tall) : crown;
		const V3 at{cx + side * h.away * tall, cy + h.rise * tall, cz};
		// On its head as a headpat squishes it - the hand itself not
		// squished.
		const M4 &sq = rig->squish();
		M4 rest = inverse(sq) * rig->head_frame() * matrix_of(conj(facing)) * moved(at) * scaled(tall);
		const V3 o = move_of(rest);
		rest = moved(point(sq, o) - o) * rest;
		return hand_at(hand, rest, side);
	}

	// 0.9.0: the 3D headpat hand on a companion (its hand's own pats,
	// `companion_hand_now`): its palm on its "pat" node (or the top of its
	// head bone) as the companion's posed, sized for the character's head -
	// the character's hand - and coming from the character's side.
	PropAt hand_on_companion(Prop3D &hand)
	{
		const Hand &h = companion_hand_now;
		const M4 &w = companion_rig->world()[companion_pat];
		// Its way up and the way its face points, square (and never
		// mirrored: a companion on its other side is, but a hand isn't).
		const V3 up = normalized(direction(w, {0, 1, 0}), {0, 1, 0});
		V3 front = direction(w, {0, 0, 1});
		front = normalized(front - up * dot(front, up), {0, 0, 1});
		const V3 right = cross(up, front);
		M4 frame;
		frame.m[0] = right.x, frame.m[1] = right.y, frame.m[2] = right.z;
		frame.m[4] = up.x, frame.m[5] = up.y, frame.m[6] = up.z;
		frame.m[8] = front.x, frame.m[9] = front.y, frame.m[10] = front.z;
		const V3 o = move_of(w);
		frame.m[12] = o.x, frame.m[13] = o.y, frame.m[14] = o.z;
		float tall = head_world;
		if (!(tall > 1e-6f))
			tall = 0.25f * std::max(1e-4f, companion_tall);
		const float side = h.side;
		const M4 rest = frame * moved({side * h.away * tall, h.rise * tall, 0}) * scaled(tall);
		return hand_at(hand, rest, side, h);
	}

	// 0.9.0: a newly loaded companion onto the graphics card, rigged and
	// measured as it sits (its idle's first moment), its animations
	// sorted out as asked; and a "come" that waited for it (graphics
	// thread, pictures_lock held).
	void pick_companion()
	{
		bool fresh = false;
		if (std::shared_ptr<ModelPack> pack = companion_loader.take()) {
			if (!companion3d)
				companion3d = std::make_unique<Avatar3D>();
			companion3d->set(pack->model, std::move(pack->pictures));
			companion_model = pack->model;
			companion_path = pack->path;
			companion_problem = pack->problem;
			companion_rig.reset();
			companion_clips_seen = 0;
			fresh = true;
			std::lock_guard<std::mutex> l(companion_lock);
			companion_ask.ready.clear();
		}
		if (!companion_model || !companion3d || !companion3d->ready()) {
			companion_rig.reset();
			if (fresh) {
				std::lock_guard<std::mutex> l(state_lock);
				companion_state = CompanionState();
				companion_state.file = companion_path;
				companion_state.problem = companion_problem;
			}
			return;
		}
		const Model &m = *companion_model;
		if (!companion_rig) {
			RigOptions ro;
			ro.facing = companion_facing(m);
			// A VRM's own springs swing; a .glb's ears and tail are in its
			// animations (no chains of its own guessed: they'd be its legs).
			ro.springs = m.is_vrm();
			ro.arms_down = false;
			ro.fingers = 0;
			ro.animation = -1;
			companion_rig = std::make_unique<Rig>(companion_model, ro);
			companion_pat = companion_pat_node(m);
			companion_clips_seen = 0;
		}
		CompanionClips clips;
		bool sorted = false;
		{
			std::lock_guard<std::mutex> l(companion_lock);
			if (companion_ask.file != companion_path)
				return;
			if (companion_clips_seen != companion_ask.clips_seq) {
				clips = companion_clips(m, companion_ask.clips);
				companion_player.set_clips(clips);
				companion_clips_seen = companion_ask.clips_seq;
				sorted = true;
			}
			companion_ask.ready = companion_path;
			companion_ask.ready_seq = companion_clips_seen;
			if (companion_ask.come) {
				companion_player.come(companion_ask.come == 2);
				companion_ask.come = 0;
			}
		}
		if (!sorted)
			return;
		companion_box = companion_rig->rest_bounds_in(clips[CLIP_IDLE].index);
		std::lock_guard<std::mutex> l(state_lock);
		CompanionState &st = companion_state;
		st.file = companion_path;
		st.problem = companion_problem;
		st.loaded = true;
		st.animations.clear();
		st.lengths.clear();
		for (const Animation &a : m.animations) {
			st.animations.push_back(a.name);
			st.lengths.push_back(std::isfinite(a.length) ? std::max(0.0f, a.length) : 0.0f);
		}
		for (int k = 0; k < COMPANION_CLIPS; k++)
			st.clips[k] = clips[k].index >= 0 ? m.animations[clips[k].index].name : "";
		st.pat = companion_pat >= 0 ? m.nodes[companion_pat].name : "";
	}

	// 0.9.0: a move that stays, its idle swapped in where its move begins
	// to ease out - timed so its loop starts as the move ends - or its own
	// back as the move back ends (graphics thread, pictures_lock held, the
	// rig there).
	void apply_stay()
	{
		if (!stay_due || !rig || !model_now)
			return;
		const Model &m = *model_now;
		const int own = animation_named(m, look3d.animation);
		const int to = stay_target >= 0 ? stay_target : own;
		const float length = to >= 0 && to < (int)m.animations.size() ? m.animations[to].length : 0;
		float start = stay_offset + stay_age;
		if (stay_aligned)
			start += length - stay_ease;
		if (length > 0) {
			start = std::fmod(start, length);
			if (start < 0)
				start += length;
		} else {
			start = 0;
		}
		rig->set_animation(to, stay_fade, start);
		stay_idle_now = stay_target;
		stay_due = false;
		std::lock_guard<std::mutex> l(state_lock);
		state3d.animation = to >= 0 ? m.animations[to].name : "";
	}

	// Things 3D over the character, round where its face is drawn (heads,
	// as react.hpp has them): things thrown, and a 2D character's 3D hand
	// (`hand_too`) - drawn now, put over it by draw_reactions.
	void draw_overlay(uint32_t w, uint32_t h, bool hand_too)
	{
		overlay_tex = nullptr;
		const ReactOut &r = react_now;
		if (!face_known || !w || !h || !(head_px >= 2))
			return;
		// The world: a head 1 across, the picture's middle at 0, y up.
		const float k = 1 / head_px;
		auto world_of = [&](float x, float y) {
			return V3{(face_px[0] + x * head_px - w * 0.5f) * k, -(face_px[1] + y * head_px - h * 0.5f) * k, 0};
		};
		std::vector<PropAt> with;
		for (const Flying &fl : r.flying) {
			Prop3D *prop = thrown_3d(fl.picture);
			if (!prop || !(fl.opacity > 0.001f))
				continue;
			const V3 lo = prop->low(), hi = prop->high();
			const float across = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z, 1e-6f});
			M4 squash;
			squash.m[0] = fl.squash_x;
			squash.m[5] = fl.squash_y;
			PropAt p;
			p.prop = prop;
			// Turned clockwise as the picture shows it: about its way.
			p.place = moved(world_of(fl.x, fl.y)) * squash * matrix_of(axis_angle(fl.axis, -fl.angle)) *
				  scaled(fl.size / across) * moved((lo + hi) * -0.5f);
			p.opacity = fl.opacity;
			with.push_back(p);
		}
		Prop3D *hand = hand_too && r.hand.shown ? hand_3d() : nullptr;
		if (hand)
			with.push_back(hand_at(*hand, moved(world_of(r.hand.x, r.hand.y)), r.hand.side));
		if (with.empty())
			return;
		if (!overlay3d)
			overlay3d = std::make_unique<Avatar3D>();
		View3D v;
		v.w = w;
		v.h = h;
		v.fov = OVERLAY_FOV;
		const float dist = (h * k * 0.5f) / std::tan(OVERLAY_FOV / 2);
		v.view = moved({0, 0, -dist});
		v.near = dist * 0.2f;
		v.far = dist * 5;
		v.lights = look3d.lights;
		v.look = model_now ? look3d.look : 0;
		v.smooth = !easy;
		v.soft_skin = 0.5f;
		overlay_tex = overlay3d->draw(nullptr, v, with);
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
			skin_key.clear();
			have_worn_pose = false;
			// A move that stays is its character's (a ride in its file): a
			// new character starts on its own idle. A companion stays.
			stay_on = false;
			stay_swapped = true;
			stay_due = false;
			stay_idle_now = -1;
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
				for (const Animation &a : m.animations) {
					st.animations.push_back(a.name);
					st.animation_lengths.push_back(a.length);
				}
				for (const Material &mat : m.materials) {
					State3D::Mat sm;
					sm.name = mat.name;
					sm.bumps = mat.normal_map.used();
					sm.shadows = mat.occlusion_map.used();
					st.materials.push_back(sm);
				}
				for (const Mesh &mesh : m.meshes)
					for (const Primitive &p : mesh.parts)
						if (p.material >= 0 && p.material < (int)st.materials.size() &&
						    !p.tangent.empty() && !p.own_tangents)
							st.materials[p.material].guessed_tangents = true;
			}
			std::lock_guard<std::mutex> l(state_lock);
			state3d = st;
			react_state.faces.clear();
			react_state.shapes.clear();
			react_state.arms = react_state.motions = false;
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
		ro.animation = animation_named(*model_now, look3d.animation);
		ro.fingers = look3d.fingers;
		{
			std::string line;
			for (char c : look3d.still_faces + "\n")
				if (c == '\n') {
					if (!line.empty())
						ro.still_faces.push_back(line);
					line.clear();
				} else if (c != '\r') {
					line += c;
				}
		}
		const std::string key = std::to_string(ro.facing.x) + "/" + std::to_string(ro.facing.y) + "/" +
					std::to_string(ro.facing.z) + "/" + std::to_string(ro.facing.w) + "/" +
					std::to_string(ro.floppy) + "/" + (ro.springs ? "1" : "0") + "/" +
					std::to_string(ro.fingers) + "/" + look3d.setup_text + "/" + look3d.still_faces;
		// 0.9.0: a move that stays has its own idle in its place (swapped
		// in and back by apply_stay) - its box still as it stands in its own.
		const bool staying = stay_idle_now >= 0 || stay_due;
		if (rig && key == rig_key && !staying && rig->animation() != ro.animation) {
			rig->set_animation(ro.animation);
			std::lock_guard<std::mutex> l(state_lock);
			state3d.animation = ro.animation >= 0 ? model_now->animations[ro.animation].name : "";
		}
		if (rig && key == rig_key && stands_for != ro.animation && !staying) {
			// As it stands in that one (its first moment).
			stands3d = rig->rest_bounds_in(ro.animation);
			stands_for = ro.animation;
			framed_for = -1;
		}
		if (!rig || key != rig_key) {
			rig = std::make_unique<Rig>(model_now, ro);
			rig_key = key;
			stands3d = rig->rest_bounds();
			stands_for = ro.animation;
			framed_for = -1;
			if (stay_idle_now >= 0)
				rig->set_animation(stay_idle_now);
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
			state3d.animation = rig->animation() >= 0 ? model_now->animations[rig->animation()].name : "";
			state3d.chain = rig->auto_chain();
			state3d.arkit = rig->arkit_parts();
			state3d.rigged = true;
			state3d.blinks = rig->blinks();
			state3d.talks = rig->talks();
			state3d.upright = rig->upright_parts();
			state3d.set_up = !look3d.setup->empty();
			state3d.fingers = rig->finger_joints();
			state3d.following = rig->following_bones();
			state3d.following_left_out = model_now->constraints_left_out;
			state3d.body = rig->has_body();
			react_state.faces = rig->faces();
			react_state.shapes = rig->shapes();
			react_state.arms = rig->has_arms();
			react_state.motions = rig->plays_motions();
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
		// The shape of what's framed in its own spot, for DireBot's "Fit
		// the OBS box to it" (0.8.1).
		const bool desk_changed = desk_bounds.found != framed_desk.found ||
					  (desk_bounds.found && (length(desk_bounds.low - framed_desk.low) > 1e-4f ||
								 length(desk_bounds.high - framed_desk.high) > 1e-4f));
		if (framed_for != look3d.framing || desk_changed) {
			framed_for = look3d.framing;
			framed_desk = desk_bounds;
			const Model &m = *model_now;
			const bool head = look3d.framing == 2 || (look3d.framing == 0 && m.is_vrm());
			V3 middle, extent;
			framed_box(m, ro.facing, head, &stands3d, middle, extent,
				   desk_bounds.found ? &desk_bounds : nullptr);
			std::lock_guard<std::mutex> l(state_lock);
			state3d.framed_w = extent.x;
			state3d.framed_h = extent.y;
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
		// Your face through its tuning, with its own blinks and its eyes'
		// small moves (alive.hpp).
		const AliveOut &a = alive_now;
		std::copy(a.moves, a.moves + EXPRESSIONS, d.moves);
		add_eye_turn(d.moves, a.eye_yaw, a.eye_pitch);
		d.breath = a.breath;
		Framing f;
		View3D v;
		bool show = true;
		float opacity = 1;
		if (!wear) {
			// Mirrored: it moves as your reflection does (not mirrored itself).
			if (mirror)
				swap_sides(d.moves);
			const Shown::Glide &g = o.steady;
			const float flip = mirror ? -1.0f : 1.0f;
			// When it can't see you it eases back to its own spot, its own size.
			const float grow =
				mix(clampf(std::pow(std::max(0.05f, g.scale), follow_grow), 0.6f, 1.6f), 1, a.idle);
			const bool head = look3d.framing == 2 || (look3d.framing == 0 && m.is_vrm());
			// Moved in proportion to its size: its eye gap (its spots), as
			// it's framed - seen from the camera view, gliding to a new one.
			f = frame_own_spot(m, facing, w, h, size, head, 0, 0, grow, &stands3d,
					   desk_bounds.found ? &desk_bounds : nullptr);
			int want = look3d.view >= 0 && look3d.view < (int)look3d.views.size() ? look3d.view : -1;
			CameraView target = want >= 0 ? look3d.views[want].second : CameraView();
			// 0.9.0: while a move that stays has its own view (a ride), or a
			// companion's here with one, the camera glides to that instead.
			if (stay_on && stay_job.has_view) {
				want = VIEW_STAY;
				target = sensible_view(stay_job.view);
			} else if (companion_now.camera && companion_shown.has_view && companion_rig) {
				want = VIEW_COMPANION;
				target = sensible_view(companion_shown.view);
			}
			if (!view_started) { // the first picture: straight there, no glide
				view_from = view_to = view_now = target;
				view_shown = want;
				view_t = 1;
				view_started = true;
			}
			const bool same = target.yaw == view_to.yaw && target.pitch == view_to.pitch &&
					  target.zoom == view_to.zoom && target.x == view_to.x && target.y == view_to.y;
			if (!same || want != view_shown) {
				view_from = view_now;
				view_to = target;
				view_shown = want;
				view_t = look3d.view_glide > 0 ? 0.0f : 1.0f;
			}
			view_t = std::min(1.0f, view_t + (look3d.view_glide > 0 ? frame_dt / look3d.view_glide : 1));
			// Eased in and out (smootherstep).
			const float e = view_t * view_t * view_t * (view_t * (view_t * 6 - 15) + 10);
			view_now = view_between(view_from, view_to, e);
			view_own_spot(f, view_now);
			// Its spot moved across the picture (0.9.0: a box made wider
			// for a companion, the character kept where it was).
			move_own_spot(f, spot_x, 0);
			{
				std::lock_guard<std::mutex> l(state_lock);
				view_shown_state = view_shown >= 0 ? view_shown : -1;
				view_cue_state = view_shown == VIEW_STAY        ? "stay"
						 : view_shown == VIEW_COMPANION ? "companion"
										: "";
				view_gliding_state = view_t < 1;
			}
			const float its_gap = length(spots3d.spot[0].at - spots3d.spot[1].at) * f.world_scale;
			float dx = 0, dy = 0;
			follow_of(o, g, follow_move, its_gap / f.seen_w, its_gap / f.seen_h, dx, dy);
			dx *= 1 - a.idle;
			dy *= 1 - a.idle;
			// What it does on cue moves it too (react.hpp: heads, as the
			// picture shows it).
			const ReactOut &rn = react_now;
			react_shift[0] = rn.dx * head_px;
			react_shift[1] = rn.dy * head_px;
			move_own_spot(f, clampf(dx * flip, -most_move, most_move) + react_shift[0] / std::max(1u, w),
				      clampf(dy, -most_move, most_move) + react_shift[1] / std::max(1u, h));
			// Where a companion's spot is: the character's as it's framed,
			// following you as much as it's asked to (not knocked about
			// with it).
			if (companion_rig && companion_now.shown) {
				const float k = clampf(companion_shown.follow, 0, 1);
				Framing fc = frame_own_spot(m, facing, w, h, size, head, 0, 0, mix(1, grow, k), &stands3d,
							    desk_bounds.found ? &desk_bounds : nullptr);
				view_own_spot(fc, view_now);
				move_own_spot(fc, spot_x + k * clampf(dx * flip, -most_move, most_move),
					      k * clampf(dy, -most_move, most_move));
				companion_rest = fc.place;
			}
			// Your head's turn (held, then easing ahead when it can't see
			// you), its own sway on top; its body takes some of it. And on
			// cue: knocked back, nodding, shaking (as the picture shows it:
			// head_turn mirrors yours, not those).
			d.head = head_turn(clampf(a.yaw * bend + a.sway_yaw, -MOST_YAW_3D, MOST_YAW_3D) + rn.yaw * flip,
					   clampf(a.pitch * bend + a.sway_pitch, -MOST_PITCH_3D, MOST_PITCH_3D) + rn.pitch,
					   clampf((a.roll * follow_tilt + a.sway_roll) * RAD, -0.6f, 0.6f) +
						   rn.roll * RAD * flip,
					   mirror);
			d.body_follow = body_follow;
			d.body_yaw = a.body_yaw;
			d.body_roll = a.body_roll;
			// Your hands and arms (step_arms), and what another tracker app
			// sends (step_vmc) - its over your camera's.
			d.follow_arms = arms_wanted;
			d.arms = arms_now;
			d.tracked = tracked_now;
			// Its hands on its keyboard, mouse or pad (step_game) - over
			// those.
			d.desk[0] = desk_now.hand[0];
			d.desk[1] = desk_now.hand[1];
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
			react_shift[0] = react_shift[1] = 0;
			// On your head it sits as your head is: it doesn't grow as a
			// whole with its breath.
			d.breathe_whole = false;
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
		} else {
			d.mic = mic_now; // its mouth from your mic, as much as it has it
		}
		// On cue (react.hpp): a face, its eyes shut, a move, hair thrown
		// about by a hit (heads a second as the picture shows it, into the
		// world's units: its picture's right and up, and towards you).
		d.faces = react_now.faces;
		// (A tracker app's faces too - its happy, its own shapes.)
		d.faces.insert(d.faces.end(), vmc_faces_now.begin(), vmc_faces_now.end());
		d.squint = react_now.squint;
		d.squish = react_now.squish;
		for (const BodyMove &bm : react_now.body) {
			MoveNow mv;
			mv.time = bm.time;
			mv.amount = bm.amount;
			if (bm.move >= MOTION_IDS) {
				for (const NamedMotion &nm : motions)
					if (nm.id == bm.move)
						mv.motion = nm.motion;
				if (!mv.motion)
					continue;
			} else if (bm.move >= 0) {
				mv.animation = bm.move;
			} else {
				mv.builtin = bm.builtin;
			}
			d.body.push_back(mv);
		}
		{
			const V3 &k = react_now.knock;
			const V3 right = wear && mirror ? f.right * -1.0f : f.right;
			const V3 toward = normalized(cross(f.right, f.up), {0, 0, 1});
			d.knock = (right * k.x + f.up * k.y + toward * k.z) * head_world;
		}
		rig->pose(d);
		// 0.9.0: its companion beside it, on its floor, posed as it comes,
		// sits, is patted and goes - drawn with it (only in its own spot).
		std::vector<Avatar3D::Other> others;
		companion_drawn = false;
		if (!wear && companion_rig && companion3d && companion_now.shown &&
		    companion_shown.file == companion_path) {
			const Model &cm = *companion_model;
			Drive cd;
			cd.dt = frame_dt;
			cd.look = false;
			cd.expressions = false;
			cd.breathe_whole = false;
			cd.place = companion_place(companion_box, companion_facing(cm), stands3d, facing,
						   companion_rest, companion_shown.place);
			cd.upright = cd.place;
			cd.has_upright = true;
			cd.body = companion_now.moves;
			companion_rig->pose(cd);
			others.push_back({companion3d.get(), companion_rig.get()});
			companion_drawn = true;
			companion_tall = (companion_box.high.y - companion_box.low.y) *
					 length(V3{cd.place.m[4], cd.place.m[5], cd.place.m[6]});
		}
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
		v.lights = look3d.lights;
		v.detail = look3d.detail;
		v.soft_skin = look3d.soft_skin;
		react_place_3d(v, f, facing, wear);
		const std::string skin_for = std::to_string((uintptr_t)model_now.get()) + "/" + look3d.skin_parts;
		if (skin_for != skin_key) {
			skin_now = skin_materials(m, look3d.skin_parts);
			skin_key = skin_for;
			std::lock_guard<std::mutex> l(state_lock);
			for (size_t i = 0; i < state3d.materials.size() && i < skin_now.size(); i++)
				state3d.materials[i].skin = skin_now[i];
		}
		v.skin = &skin_now;
		// The 3D headpat hand on its head, drawn with it (its hair hides
		// what's in it).
		std::vector<PropAt> with;
		Prop3D *hand = react_now.hand.shown ? hand_3d() : nullptr;
		if (hand && face_known)
			with.push_back(hand_on_head(*hand, facing, wear));
		// ...and on its companion's head, its own pats.
		Prop3D *pet_hand = companion_drawn && companion_hand_now.shown && companion_pat >= 0 ? hand_3d()
												     : nullptr;
		if (pet_hand)
			with.push_back(hand_on_companion(*pet_hand));
		// Its keyboard, mouse and pad in front of it (step_game).
		if (!wear)
			for (int i = 0; i < DESK_PROPS; i++) {
				const DeskPlace &dp = desk_now.prop[i];
				Prop3D *prop = dp.opacity > 0.001f ? game_prop(i) : nullptr;
				if (!prop)
					continue;
				PropAt p;
				p.prop = prop;
				p.place = f.place * matrix_of(conj(facing)) * dp.place;
				p.opacity = dp.opacity;
				with.push_back(p);
			}
		gs_texture_t *tex = show ? avatar3d->draw(rig.get(), v, with, others) : nullptr;
		{
			std::lock_guard<std::mutex> l(state_lock);
			companion_state.drawn = companion_drawn && tex != nullptr;
			companion_state.size = companion_drawn ? companion_tall : 0;
		}
		draw_overlay(w, h, false);
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
			draw_reactions();
		}
		gs_texrender_end(out);
		have_out = true;
		std::lock_guard<std::mutex> l(state_lock);
		state3d.cards = cards;
		state3d.layers = drawn_layers;
	}

	// Where its face is drawn and how tall its head is (a 3D character),
	// for what's thrown at it and a headpat: a VRM's head as its head bone
	// moves it (its face's middle a little below its eyes, the top of its
	// hair), else from its eyes (its head 2.8 times as tall as they're
	// apart, the top of it all).
	void react_place_3d(const View3D &v, const Framing &f, Quat facing, bool wear)
	{
		face_known = false;
		const Model &m = *model_now;
		const float W = (float)v.w, H = (float)v.h;
		if (!rig || !W || !H)
			return;
		const float k = 1 / std::tan(v.fov / 2), aspect = W / H;
		const M4 frame = rig->head_frame();
		const Quat back = conj(facing);
		bool ok = true;
		auto at = [&](V3 p) { // the facing model's way, at rest
			const V3 c = point(v.view, point(frame, rotate(back, p)));
			const float depth = -c.z;
			if (depth < v.near * 0.5f)
				ok = false;
			float sx = (c.x * k / aspect / std::max(depth, 1e-6f) + 1) * 0.5f * W;
			const float sy = (1 - c.y * k / std::max(depth, 1e-6f)) * 0.5f * H;
			if (wear && mirror)
				sx = W - sx;
			return std::array<float, 2>{sx, sy};
		};
		const V3 le = rotate(facing, spots3d.spot[0].at), re = rotate(facing, spots3d.spot[1].at);
		const bool eyes = std::strcmp(spots3d.from[0], "guessed") != 0 && std::strcmp(spots3d.from[1], "guessed") != 0;
		float top_y = rotate(facing, m.high).y;
		for (int c = 0; c < 8; c++)
			top_y = std::max(top_y, rotate(facing, V3{c & 1 ? m.high.x : m.low.x, c & 2 ? m.high.y : m.low.y,
								 c & 4 ? m.high.z : m.low.z})
							.y);
		const Rig::HeadBox &hb = rig->head_box();
		V3 mid;
		float tall, top;
		if (hb.found) {
			tall = hb.high.y - hb.low.y;
			mid = {(hb.low.x + hb.high.x) / 2, hb.low.y + 0.42f * tall, hb.high.z};
			const float eye_y = (le.y + re.y) / 2;
			if (eyes && eye_y > hb.low.y && eye_y < hb.high.y)
				mid.y = eye_y - 0.1f * tall;
			top = hb.top;
		} else {
			const float gap = length(le - re);
			tall = gap > 1e-6f ? 2.8f * gap : 0.3f * std::max(1e-4f, m.high.y - m.low.y);
			mid = (le + re) * 0.5f - V3{0, 0.1f * tall, 0};
			top = std::max(top_y, mid.y + 0.3f * tall);
		}
		const auto c = at(mid), up = at(mid + V3{0, tall, 0}), t = at({mid.x, top, mid.z});
		head_px = std::hypot(up[0] - c[0], up[1] - c[1]);
		head_world = tall * f.world_scale;
		if (!ok || !(head_px >= 2) || !std::isfinite(c[0] + c[1] + t[0] + t[1]))
			return;
		set_react_view(c[0] - react_shift[0], c[1] - react_shift[1],
			       std::hypot(t[0] - c[0], t[1] - c[1]) / head_px, v.w, v.h);
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
		ro.fingers = look3d.fingers;
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
		gs_texture_t *tex = avatar3d->draw(&still, v);
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
		// The character's light on them (the camera's picture is sRGB).
		const V3 front = front_light(v.lights);
		vec3 card_light;
		vec3_set(&card_light, std::pow(front.x, 1 / 2.2f), std::pow(front.y, 1 / 2.2f), std::pow(front.z, 1 / 2.2f));
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
			gs_effect_set_vec3(p_card_light, &card_light);
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
		if (cam && cw && ch && camera_wanted()) {
			if (since_render > STALE) {
				staged = false;
				if (face_from_camera())
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

	// 0.9.0, the throws filter: awake while something's thrown at you (and
	// a moment after), else the camera goes by untouched.
	bool awake()
	{
		if (!as_filter)
			return false;
		bool busy;
		{
			std::lock_guard<std::mutex> l(state_lock);
			busy = react_state.busy;
		}
		return busy || os_gettime_ns() < wake_until.load();
	}

	// The throws filter drawing: the camera (as the filters before it
	// leave it), what's thrown at you over your face in it, and the
	// camera jolted a little as things hit (zoomed in just enough that its
	// edges never show) - your face, and all that's on it, jolted with it.
	void render_filter()
	{
		obs_source_t *target = obs_filter_get_target(self);
		const uint32_t cw = target ? obs_source_get_base_width(target) : 0,
			       ch = target ? obs_source_get_base_height(target) : 0;
		if (reading_camera > 0 || drawing || !effect || !cw || !ch || !awake()) {
			obs_source_skip_video_filter(self);
			return;
		}
		{
			std::lock_guard<std::mutex> l(state_lock);
			showing_now = true;
			last_render = os_gettime_ns();
		}
		cam_w = cw;
		cam_h = ch;
		drawing = true;
		if (!drawn) {
			gs_blend_state_push();
			if (since_render > STALE) {
				staged = false;
				worker.forget();
				pace.reset();
				cam_face.now = false;
			}
			since_render = 0;
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
			newest_shot = -1;
			look(nullptr, cw, ch);
			draw_on_camera(cw, ch);
			gs_blend_state_pop();
			drawn = true;
		}
		gs_texture_t *tex = have_out ? gs_texrender_get_texture(out) : nullptr;
		if (tex) {
			gs_effect_t *base = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_effect_set_texture(gs_effect_get_param_by_name(base, "image"), tex);
			while (gs_effect_loop(base, "Draw"))
				gs_draw_sprite(tex, 0, cw, ch);
		}
		drawing = false;
		if (!tex)
			obs_source_skip_video_filter(self);
	}

	// Your face in the camera's picture, for what's thrown at it: where
	// (the middle of your eyes and mouth), how tall your head is (your
	// forehead's top to your chin, and a little for your hair) and its
	// tilt, from the newest look - held while it can't see you, a guess
	// (the middle, a little high) until it first does.
	void place_on_camera(uint32_t cw, uint32_t ch)
	{
		const Face &fc = seen.face;
		if (fc.found && fc.w > 0 && fc.h > 0) {
			const float kx = (float)cw / fc.w, ky = (float)ch / fc.h;
			auto at = [&](int i) {
				return std::array<float, 2>{fc.points[i].x * kx, fc.points[i].y * ky};
			};
			auto mid2 = [](std::array<float, 2> a, std::array<float, 2> b) {
				return std::array<float, 2>{(a[0] + b[0]) / 2, (a[1] + b[1]) / 2};
			};
			const auto left = mid2(at(RIGHT_EYE_OUTER), at(RIGHT_EYE_INNER));
			const auto right = mid2(at(LEFT_EYE_INNER), at(LEFT_EYE_OUTER));
			const auto mouth = mid2(at(MOUTH_RIGHT), at(MOUTH_LEFT));
			const auto top = at(10), chin = at(152);
			const float tall = std::hypot(chin[0] - top[0], chin[1] - top[1]) * 1.25f;
			if (tall >= 4 && std::isfinite(tall)) {
				CameraFace f;
				f.seen = f.now = true;
				f.x = (left[0] + right[0] + mouth[0]) / 3;
				f.y = (left[1] + right[1] + mouth[1]) / 3;
				f.head = tall;
				f.roll = std::atan2(right[1] - left[1], right[0] - left[0]);
				// (Picture's left eye to its right: a tilt clockwise is +.)
				f.head_top = clampf((f.y - top[1]) / tall + 0.12f, 0.2f, 1.5f);
				cam_face = f;
			}
		} else {
			cam_face.now = false;
		}
		if (!cam_face.seen) {
			cam_face.x = cw * 0.5f;
			cam_face.y = ch * 0.42f;
			cam_face.head = ch * 0.4f;
			cam_face.roll = 0;
			cam_face.head_top = 0.6f;
		}
		{
			std::lock_guard<std::mutex> l(state_lock);
			cam_face_state = cam_face;
		}
		head_px = cam_face.head;
		set_react_view(cam_face.x, cam_face.y, cam_face.head_top, cw, ch);
		react_view.roll = cam_face.roll;
		react_view.still_head = true;
	}

	// The camera and what's thrown at you into `out` (once a frame).
	void draw_on_camera(uint32_t cw, uint32_t ch)
	{
		gs_texture_t *cam_tex = newest_shot >= 0 && shots[newest_shot].tex
						? gs_texrender_get_texture(shots[newest_shot].tex)
						: nullptr;
		have_out = false;
		if (!cam_tex)
			return;
		std::lock_guard<std::mutex> l(pictures_lock);
		pick_props();
		place_on_camera(cw, ch);
		draw_overlay(cw, ch, true);
		if (!out)
			out = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		if (!out)
			return;
		gs_texrender_reset(out);
		if (!gs_texrender_begin(out, cw, ch))
			return;
		vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)cw, 0.0f, (float)ch, -100.0f, 100.0f);
		// The jolt: the knock-back's move and tilt, about your face, as
		// much as suits a camera, zoomed in just enough to fill it.
		const ReactOut &r = react_now;
		const bool shake = camera_shake.load();
		const float dx = shake ? r.dx * head_px * CAMERA_JOLT : 0,
			    dy = shake ? r.dy * head_px * CAMERA_JOLT : 0;
		const float turn = shake ? r.roll * RAD * CAMERA_JOLT : 0;
		const float zoom = cover_zoom((float)cw, (float)ch, cam_face.x, cam_face.y, dx, dy, turn);
		gs_matrix_push();
		gs_matrix_translate3f(cam_face.x + dx, cam_face.y + dy, 0);
		gs_matrix_rotaa4f(0, 0, 1, turn);
		gs_matrix_scale3f(zoom, zoom, 1);
		gs_matrix_translate3f(-cam_face.x, -cam_face.y, 0);
		gs_effect_t *base = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_effect_set_texture(gs_effect_get_param_by_name(base, "image"), cam_tex);
		while (gs_effect_loop(base, "Draw"))
			gs_draw_sprite(cam_tex, 0, cw, ch);
		draw_reactions();
		gs_matrix_pop();
		gs_texrender_end(out);
		have_out = true;
	}

	// The avatar into `out`, once a frame: the character drawn flat, then
	// turned and placed (over the camera, worn on your head).
	void draw_avatar(obs_source_t *cam, uint32_t cw, uint32_t ch, uint32_t w, uint32_t h)
	{
		std::lock_guard<std::mutex> l(pictures_lock);
		const Shown::Out o = shown.now();
		const bool wear = worn && cw && ch;
		pick_model();
		pick_companion();
		apply_stay();
		pick_props();
		upload_masks();
		if (rig && model_now) {
			draw_3d(cam, cw, ch, w, h, o, wear);
			return;
		}
		gs_texture_t *cut = cam && used_shot >= 0 ? gs_texrender_get_texture(shots[used_shot].tex) : nullptr;
		// Which way it faces: turned pictures if there are any (while it
		// holds your last look, the way you last faced).
		if (o.found)
			held_turned = o.turned;
		else if (std::fabs(alive_now.yaw) < Shown::TURN_OFF)
			held_turned = 0;
		const int turned = turn_pictures ? held_turned * (mirror ? -1 : 1) : 0;
		int which = 0;
		if (turned < 0 && loaded(body_left))
			which = 1;
		else if (turned > 0 && loaded(body_right))
			which = 2;
		const PicturePtr &pic = which == 1 ? body_left : which == 2 ? body_right : body;
		Placed at;
		gs_texture_t *flat_tex = nullptr;
		face_known = false;
		if (loaded(pic)) {
			at = place(o, *pic, which, shapes[which].get(), wear, w, h);
			if (at.shown)
				flat_tex = draw_flat(cut, cw, ch, o, *pic, shapes[which].get(), at.scale);
			react_place_2d(at, *pic, w, h);
			draw_overlay(w, h, true);
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
		if (flat_tex) {
			draw_turned(flat_tex, shapes[which], *pic, at);
			draw_reactions();
		}
		gs_texrender_end(out);
		have_out = true;
	}

	// Where its face is drawn and how tall its head is (a 2D character),
	// for what's thrown at it and a headpat: from its eyes' spots (its
	// head about 2.8 times as tall as they're apart, its face's middle a
	// little below them), else a guess at the top of its picture.
	void react_place_2d(const Placed &at, const Picture &pic, uint32_t w, uint32_t h)
	{
		face_known = false;
		if (!at.shown || !w || !h)
			return;
		const float char_w = pic.width(), char_h = pic.height();
		const float CX = canvas.x * char_w, CY = canvas.y * char_h;
		const float co = std::cos(at.angle), si = std::sin(at.angle);
		float ex = char_w / 2, ey = 0.3f * char_h, tall = 0.35f * char_h;
		if (have_spots) {
			const float lx = (spots[0].x + spots[0].w / 2) * char_w, ly = (spots[0].y + spots[0].h / 2) * char_h;
			const float rx = (spots[1].x + spots[1].w / 2) * char_w, ry = (spots[1].y + spots[1].h / 2) * char_h;
			ex = (lx + rx) / 2;
			ey = (ly + ry) / 2;
			tall = std::max(2.8f * std::hypot(rx - lx, ry - ly), 0.05f * char_h);
		}
		// (Squished by a headpat: from its bottom edge.)
		const float bottom = canvas.h * char_h - at.ay;
		const float u = (ex - CX - at.ax) * (1 + SQUISH_WIDE_2D * at.squish) * at.scale,
			    v = (bottom + (ey + 0.1f * tall - CY - at.ay - bottom) * (1 - SQUISH_TALL_2D * at.squish)) *
				at.scale;
		const float x = at.x + co * u - si * v, y = at.y + si * u + co * v;
		head_px = tall * at.scale;
		if (!(head_px >= 2) || !std::isfinite(x + y))
			return;
		set_react_view(x - react_shift[0], y - react_shift[1], 0.65f, w, h);
	}

	void set_react_view(float x, float y, float head_top, uint32_t w, uint32_t h)
	{
		face_px[0] = x;
		face_px[1] = y;
		react_view.left = -x / head_px;
		react_view.right = ((float)w - x) / head_px;
		react_view.top = -y / head_px;
		react_view.bottom = ((float)h - y) / head_px;
		react_view.head_top = clampf(head_top, 0.2f, 3);
		// (Called while drawing, its pictures' lock held.)
		// (How tall the hand is above where it touches, for its width.)
		react_view.hand_tall =
			hand_3d()	   ? HAND_3D_TALL
			: loaded(hand_pic) ? (float)hand_pic->height() / std::max(1.0f, (float)hand_pic->width()) *
						     hand_place.touch[1] * hand_place.width / 0.85f
					   : 1.25f;
		face_known = true;
	}

	// What's thrown at it, where things hit and the patting hand, over it
	// (the picture's pixels: react.hpp's heads from where its face is).
	void draw_reactions()
	{
		const ReactOut &r = react_now;
		if (!face_known || (r.flying.empty() && !r.hand.shown && r.bursts.empty() && !overlay_tex &&
				    r.marks.empty() && r.bits.empty()))
			return;
		// What's left on its face (0.9.0) first: only where the character
		// (or the camera) is drawn - a splat on its cheek never hangs in
		// the air past it. It's laid over what's there as much as that is
		// there (the picture's own see-through-ness kept).
		if (!r.marks.empty()) {
			gs_blend_function_separate(GS_BLEND_DSTALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ZERO,
						   GS_BLEND_ONE);
			for (const Mark &m : r.marks)
				draw_mark(m);
		}
		gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		// A picture `across` heads wide (as tall as its shape says), its
		// middle at (x, y) heads, turned clockwise `angle` and squashed
		// (sx, sy) in its own ways.
		auto sprite = [&](const PicturePtr &pic, float x, float y, float across, float angle, float opacity,
				  float sx, float sy) {
			if (!loaded(pic) || !(opacity > 0.001f) || !(across > 0))
				return;
			const float wpx = across * head_px * sx,
				    hpx = across * head_px * pic->height() / std::max(1.0f, pic->width()) * sy;
			const float cx = face_px[0] + x * head_px, cy = face_px[1] + y * head_px;
			const float co = std::cos(angle), si = std::sin(angle);
			const float ox = -wpx / 2, oy = -hpx / 2;
			if (!std::isfinite(cx + cy + wpx + hpx + co + si))
				return;
			matrix4 m;
			vec4_set(&m.x, co * wpx, si * wpx, 0, 0);
			vec4_set(&m.y, -si * hpx, co * hpx, 0, 0);
			vec4_set(&m.z, 0, 0, 1, 0);
			vec4_set(&m.t, cx + co * ox - si * oy, cy + si * ox + co * oy, 0, 1);
			vec4 clip;
			vec4_set(&clip, 0, 0, 1, 1);
			gs_effect_set_texture(p_image, pic->texture());
			gs_effect_set_float(p_opacity, std::min(1.0f, opacity));
			gs_effect_set_vec4(p_clip, &clip);
			gs_matrix_push();
			gs_matrix_mul(&m);
			check("Picture");
			while (gs_effect_loop(effect, "Picture"))
				gs_draw_quadf(pic->texture(), 0, 1, 1);
			gs_matrix_pop();
		};
		for (const Burst &b : r.bursts)
			sprite(hit_pic, b.x, b.y, b.size, 0, b.opacity, 1, 1);
		// Each moving picture on to its next look once a frame (an animated
		// GIF), however many are in the air.
		std::set<Picture *> animated;
		for (const Flying &f : r.flying) {
			const PicturePtr &pic = thrown_picture(f.picture);
			if (pic && animated.insert(pic.get()).second)
				pic->animate((uint64_t)(frame_dt * 1e9));
			sprite(pic, f.x, f.y, f.size, f.angle, f.opacity, f.squash_x, f.squash_y);
		}
		// Things 3D (draw_overlay), as drawn: see-through round them.
		if (overlay_tex) {
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
			gs_effect_t *base = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_effect_set_texture(gs_effect_get_param_by_name(base, "image"), overlay_tex);
			while (gs_effect_loop(base, "Draw"))
				gs_draw_sprite(overlay_tex, 0, gs_texture_get_width(overlay_tex),
					       gs_texture_get_height(overlay_tex));
			gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE,
						   GS_BLEND_INVSRCALPHA);
		}
		// The bits thrown off it as things hit (0.9.0): juice, feathers,
		// snow, puffs of powder - in front of all of it.
		for (const Bit &b : r.bits)
			sprite(effect_picture(b.picture), b.x, b.y, b.size, b.angle, b.opacity, 1, 1);
		// A picture of the hand (no 3D one).
		if (r.hand.shown && !hand_3d())
			draw_hand_picture(r.hand);
	}

	const PicturePtr &effect_picture(int n) const
	{
		static const PicturePtr none;
		return n >= 0 && n < (int)effect_pics.size() ? effect_pics[n] : none;
	}

	// Something left on its face (react.hpp's Mark): a splat round its
	// middle, or a drip hanging from the middle of its top edge, as long
	// as it has run - drawn with "Stain" (premultiplied: the blend keeps
	// it to where something is drawn already).
	void draw_mark(const Mark &mk)
	{
		const PicturePtr &pic = effect_picture(mk.picture);
		if (!loaded(pic) || !(mk.opacity > 0.001f) || !(mk.size > 0))
			return;
		const float wpx = mk.size * head_px;
		const float hpx = mk.from_top ? mk.length * head_px
					      : wpx * pic->height() / std::max(1.0f, (float)pic->width());
		const float cx = face_px[0] + mk.x * head_px, cy = face_px[1] + mk.y * head_px;
		const float co = std::cos(mk.angle), si = std::sin(mk.angle);
		// Its top left corner from (cx, cy), before it's turned.
		const float ox = -wpx / 2, oy = mk.from_top ? 0 : -hpx / 2;
		if (!std::isfinite(cx + cy + wpx + hpx + co + si) || !(hpx > 0))
			return;
		matrix4 m;
		vec4_set(&m.x, co * wpx, si * wpx, 0, 0);
		vec4_set(&m.y, -si * hpx, co * hpx, 0, 0);
		vec4_set(&m.z, 0, 0, 1, 0);
		vec4_set(&m.t, cx + co * ox - si * oy, cy + si * ox + co * oy, 0, 1);
		gs_effect_set_texture(p_image, pic->texture());
		gs_effect_set_float(p_opacity, std::min(1.0f, mk.opacity));
		gs_matrix_push();
		gs_matrix_mul(&m);
		check("Stain");
		while (gs_effect_loop(effect, "Stain"))
			gs_draw_quadf(pic->texture(), 0, 1, 1);
		gs_matrix_pop();
	}

	// A picture of the patting hand: where it touches at react.hpp's
	// (x, y), turned at its wrist - as it is from the left, flipped from
	// the right.
	void draw_hand_picture(const Hand &hand)
	{
		const PicturePtr &pic = hand_pic;
		if (!loaded(pic) || !(hand.opacity > 0.001f))
			return;
		const HandPlace &hp = hand_place;
		const bool flip = hand.side > 0;
		const float wpx = hand.size * hp.width * head_px,
			    hpx = wpx * pic->height() / std::max(1.0f, pic->width());
		const float tx = (flip ? 1 - hp.touch[0] : hp.touch[0]) * wpx, ty = hp.touch[1] * hpx;
		const float px = (flip ? 1 - hp.wrist[0] : hp.wrist[0]) * wpx, py = hp.wrist[1] * hpx;
		const float sx = face_px[0] + hand.x * head_px, sy = face_px[1] + hand.y * head_px;
		const float co = std::cos(hand.angle), si = std::sin(hand.angle);
		if (!std::isfinite(sx + sy + wpx + hpx + co + si) || !(wpx > 0))
			return;
		// Its touch on (sx, sy) as it rests, turned about its wrist.
		matrix4 m;
		vec4_set(&m.x, co * wpx, si * wpx, 0, 0);
		vec4_set(&m.y, -si * hpx, co * hpx, 0, 0);
		vec4_set(&m.z, 0, 0, 1, 0);
		vec4_set(&m.t, sx + px - tx - (co * px - si * py), sy + py - ty - (si * px + co * py), 0, 1);
		vec4 clip;
		vec4_set(&clip, 0, 0, 1, 1);
		gs_effect_set_texture(p_image, pic->texture());
		gs_effect_set_float(p_opacity, std::min(1.0f, hand.opacity));
		gs_effect_set_vec4(p_clip, &clip);
		gs_matrix_push();
		gs_matrix_mul(&m);
		check("Picture");
		while (gs_effect_loop(effect, "Picture"))
			gs_draw_quadf(pic->texture(), flip ? GS_FLIP_U : 0, 1, 1);
		gs_matrix_pop();
	}

	// Thing `n` thrown's picture (react.hpp's Flying::picture): one of the
	// things to throw, or one of chat's emotes (null: none, or a 3D one).
	const PicturePtr &thrown_picture(int n) const
	{
		static const PicturePtr none;
		if (n >= EMOTE_IDS && n < EMOTE_IDS + (int)emotes.size())
			return emotes[n - EMOTE_IDS].pic;
		return n >= 0 && n < (int)throw_pics.size() ? throw_pics[n] : none;
	}

	// Your hands and arms, followed on a 3D VRM character in its own spot
	// (not worn on your head): the newest look, then this frame's ease.
	void step_arms(float dt)
	{
		const bool wear = worn;
		const bool vrm = model_now && model_now->is_vrm();
		arms_wanted = arms_on && !wear && rig && rig->follows_arms();
		arms_why = arms_wanted		 ? ""
			   : !arms_on		 ? "off"
			   : wear		 ? "worn"
			   : !model_now || !rig ? "no_3d"
			   : !vrm		 ? "not_vrm"
						 : "no_arms";
		ArmOptions ao;
		{
			std::lock_guard<std::mutex> l(pictures_lock);
			ao = arm_options;
		}
		ao.mirror = mirror;
		body_worker.state(body_seen);
		const bool fresh = body_seen.looks != body_seen_looks;
		body_seen_looks = body_seen.looks;
		std::string finished;
		ArmsSeen seen_now;
		{
			std::lock_guard<std::mutex> l(arms_lock);
			arms.set_options(ao);
			const bool was_calibrating = arms.calibrating();
			if (reset_arms.exchange(false) || !arms_wanted) {
				arms.reset();
				if (!arms_wanted)
					arms.stop_calibration();
			} else if (fresh) {
				arms.look(body_seen.body);
			}
			arms.step(dt);
			if (was_calibrating && !arms.calibrating() && arms.calibration_result() == "ok") {
				finished = arms.save_calibration();
				arms_pending = true;
			}
			arms_now = arms.now();
			seen_now = arms.seen();
		}
		if (!finished.empty()) {
			// Saved in the settings on OBS's UI thread (save_arms_task).
			obs_weak_source_addref(weak_self);
			auto *job = new std::pair<obs_weak_source_t *, std::string>(weak_self, finished);
			obs_queue_task(OBS_TASK_UI, save_arms_task, job, false);
		}
		std::lock_guard<std::mutex> l(state_lock);
		body_state = body_seen;
		arms_seen_state = seen_now;
		arms_wanted_state = arms_wanted;
		arms_why_state = arms_why;
	}

	// Gaming hands: what's been done on your keyboard, mouse and pad
	// lately (or "Show me"'s pretending), its hands put there for the rig
	// and its keyboard, mouse and pad placed - on a 3D VRM character with
	// arms, in its own spot. Starting again whenever what it's from
	// changes. Graphics thread.
	void step_game(float dt)
	{
		const bool wear = worn;
		const bool vrm = model_now && model_now->is_vrm();
		std::shared_ptr<DeviceReader> reader;
		GameOptions go;
		{
			std::lock_guard<std::mutex> l(game_lock);
			reader = game_reader;
			go = game_options;
		}
		go.mirror = mirror;
		const DeskBody body = rig && !wear ? rig->desk_body() : DeskBody();
		game_wanted = game_on && !wear && rig && body.ok;
		game_why = game_wanted		 ? ""
			   : !game_on		 ? "off"
			   : wear		 ? "worn"
			   : !model_now || !rig ? "no_3d"
			   : !vrm		 ? "not_vrm"
						 : "no_arms";
		const bool stop = game_stop.exchange(false), show = game_show.exchange(false);
		if ((stop || !game_wanted) && game_show_t >= 0) {
			game_show_t = -1;
			game.reset();
		}
		if (show && game_wanted) {
			game_show_t = 0;
			game.reset();
		}
		const DeviceState read = reader ? reader->state() : DeviceState();
		DeviceState s;
		double now = 0;
		if (game_show_t >= 0) {
			game_show_t += dt;
			if (game_show_t > PRETEND_TIME) {
				game_show_t = -1;
				game.reset();
			} else {
				s = pretend_devices(game_show_t);
				now = game_show_t;
			}
		}
		if (game_show_t < 0 && reader) {
			s = read;
			now = DeviceReader::clock();
		}
		game.set_options(go);
		game.set_body(body);
		if (game_wanted) {
			game.step(s, now, dt);
			desk_now = game.now();
		} else if (desk_now.device != GameDevice::None) {
			game.reset();
			desk_now = DeskNow();
		}
		// A move that stays (riding up onto a dragon and flying on) has its
		// arms: its hands let go and its keyboard, mouse and pad fade away
		// till it ends (framed as before, so nothing jumps).
		game_away = clampf(game_away + clampf((stay_on ? 1.0f : 0.0f) - game_away, -dt / 0.4f, dt / 0.4f), 0, 1);
		if (game_away > 0) {
			for (DeskHand &hand : desk_now.hand)
				hand.weight *= 1 - game_away;
			for (DeskPlace &prop : desk_now.prop)
				prop.opacity *= 1 - game_away;
			desk_now.on *= 1 - game_away;
		}
		desk_bounds = Bounds();
		desk_bounds.found = game_wanted && desk_box(body, go, desk_bounds.low, desk_bounds.high);
		std::lock_guard<std::mutex> l(state_lock);
		desk_state = desk_now;
		// (Never which keys: there's nothing about them in it but how many.)
		devices_state = read;
		game_reading_state = (bool)reader;
		game_wanted_state = game_wanted;
		game_why_state = game_why;
		game_show_state = game_show_t;
	}

	// Another tracker app (VMC): what it sent lately - your face handed to
	// the tracker as a phone's is, your body, arms, fingers and eyes for
	// the rig, each part eased in while the app sends it and out when it
	// stops (held as it was last sent meanwhile). Graphics thread.
	static constexpr float VMC_EASE = 0.25f, VMC_RECENTRE = 4.0f;
	uint64_t vmc_given = 0;
	V3 vmc_hips_now;
	void step_vmc(float dt)
	{
		std::shared_ptr<VmcLink> link;
		{
			std::lock_guard<std::mutex> l(vmc_lock);
			link = vmc_link;
		}
		const bool parts = vmc_body || vmc_arms;
		const bool vrm = rig && model_now && model_now->is_vrm() && rig->plays_motions() && !worn;
		const char *why = !link || !parts        ? ""
				  : worn                 ? "worn"
				  : !model_now || !rig   ? "no_3d"
				  : !model_now->is_vrm() ? "not_vrm"
				  : !vrm                 ? "no_bones"
							 : "";
		bool fresh_face = false;
		if (link) {
			const uint64_t ch = link->changes();
			if (ch != vmc_changes) {
				link->sent(vmc_sent, vmc_now);
				vmc_changes = ch;
			} else {
				vmc_now = VmcLink::clock();
			}
			fresh_face = ch != vmc_given;
		} else if (vmc_changes) {
			vmc_sent = VmcSent();
			vmc_changes = 0;
		}
		const VmcSends sends = link ? vmc_sends(vmc_sent, vmc_now) : VmcSends();
		// Your face: through the tracker, its sides yours (swapped for a
		// mirror, ours or the app's); its faces by name for the rig.
		bool face_live = false;
		vmc_faces_now.clear();
		if (from_vmc && link) {
			PhoneLook look;
			std::vector<std::pair<std::string, float>> faces;
			face_live = vmc_face(vmc_sent, vmc_now, look, faces);
			if (face_live) {
				for (const auto &fc : faces) {
					face_into_moves(fc.first, fc.second, look.expressions);
					if (fc.second > 0.001f)
						vmc_faces_now.push_back(FaceNow{fc.first, fc.second});
				}
				if (vmc_mirror != mirror) {
					swap_sides(look.expressions);
					look.turn.yaw = -look.turn.yaw;
					look.turn.roll = -look.turn.roll;
				}
				if (fresh_face) {
					worker.give_phone(look, os_gettime_ns());
					vmc_given = vmc_changes;
				}
			}
		}
		if (vmc_face_live && !face_live)
			worker.give_phone(PhoneLook(), os_gettime_ns()); // lost
		vmc_face_live = face_live;
		// Your body, arms and fingers: each bone as last sent.
		const VmcBones b = link ? vmc_bones(vmc_sent, vmc_now, vmc_mirror) : VmcBones();
		bool sent[3] = {false, false, false};
		for (int i = 0; i < VMC_BONES; i++) {
			if (!b.has[i])
				continue;
			vmc_held[i] = b.turn[i];
			vmc_held_has[i] = true;
			const VmcPart part = vmc_part(i);
			if (part == VMC_PART_BODY || part == VMC_PART_LEGS)
				sent[0] = true;
			else if (part == VMC_PART_ARMS)
				sent[1] = true;
			else if (part == VMC_PART_FINGERS)
				sent[2] = true;
		}
		const bool want[3] = {vmc_body && vrm && sent[0], vmc_arms && vrm && sent[1],
				      vmc_arms && vrm && sent[2]};
		const float step = dt > 0 ? dt / VMC_EASE : 0;
		for (int k = 0; k < 3; k++)
			vmc_share[k] = clampf(
				vmc_share[k] + clampf((want[k] ? 1.0f : 0.0f) - vmc_share[k], -step, step), 0, 1);
		// Its hips' home: where they were as its body came in (and at
		// Set), across and back drifting after them, so a step aside
		// shows but it doesn't wander out of its spot.
		if (b.has[VMC_HIPS]) {
			const V3 at = b.at[VMC_HIPS];
			if (!vmc_home_set || vmc_rehome.exchange(false) || vmc_share[0] <= 0) {
				vmc_home = at;
				vmc_home_set = true;
			} else {
				const float k = dt > 0 ? std::min(1.0f, dt / VMC_RECENTRE) : 0;
				vmc_home.x += (at.x - vmc_home.x) * k;
				vmc_home.z += (at.z - vmc_home.z) * k;
			}
			vmc_hips_now = at;
		}
		TrackedBody t;
		if (vrm) {
			bool torso = false;
			for (int i = 0; i < VMC_BONES; i++) {
				if (!vmc_held_has[i])
					continue;
				const VmcPart part = vmc_part(i);
				float amount = 0;
				if (part == VMC_PART_BODY || part == VMC_PART_LEGS)
					amount = vmc_share[0];
				else if (part == VMC_PART_ARMS)
					amount = vmc_share[1];
				else if (part == VMC_PART_FINGERS)
					amount = vmc_share[2];
				// Its eyes, with its face (its neck and head come through
				// the tracker).
				else if ((i == VMC_LEFT_EYE || i == VMC_RIGHT_EYE) && face_live && b.has[i])
					amount = 1;
				if (!(amount > 0))
					continue;
				t.bones.push_back(TrackedBone{VMC_BONE_NAMES[i], vmc_held[i], amount});
				if (amount >= 1 && (i == VMC_LEFT_EYE || i == VMC_RIGHT_EYE))
					t.eyes = true;
				if (part == VMC_PART_BODY)
					torso = true;
			}
			if (vmc_share[0] > 0 && vmc_home_set) {
				const float height = vmc_home.y > 0.3f ? vmc_home.y : 0.9f;
				const V3 by = (vmc_hips_now - vmc_home) * (1 / height);
				t.hips = {clampf(by.x, -0.6f, 0.6f), clampf(by.y, -0.7f, 0.4f),
					  clampf(by.z, -0.6f, 0.6f)};
				t.hips_amount = vmc_share[0];
			}
			if (torso) {
				t.torso = vmc_share[0];
				// Your head from a camera or phone turns against the world:
				// the app's chest's turn is taken off it (the app's own
				// head's turn is already against its chest).
				if (!from_vmc)
					for (int i : {VMC_HIPS, VMC_SPINE, VMC_CHEST, VMC_UPPER_CHEST})
						if (vmc_held_has[i])
							t.chest = t.chest * vmc_held[i];
			}
		}
		if (!link)
			for (bool &h : vmc_held_has)
				h = false;
		tracked_now = std::move(t);
		std::lock_guard<std::mutex> l(state_lock);
		vmc_sends_state = sends;
		vmc_using_state[0] = face_live;
		for (int k = 0; k < 3; k++)
			vmc_using_state[1 + k] = vmc_share[k] > 0.5f;
		vmc_why_state = why;
	}

	// Hand signs and face moves (signs.hpp): watched while it's looking at
	// you - shown, or hidden with "while it's hidden" on - and each one
	// held its time sent to DireBot (send_signs_task). Your face's moves
	// as the camera reads them, through your tuning; your hands from the
	// body worker, on for them even with "Follow my hands and arms" off
	// (the character's arms don't move for them).
	void step_signs(float dt, const Shown::Out &o, const FaceTune &t)
	{
		{
			std::lock_guard<std::mutex> l(signs_lock);
			if (signs_changed) {
				signs.set_rules(std::move(signs_new));
				signs_new.clear();
				signs_changed = false;
			}
		}
		const bool on = signs_on && !signs.rules().empty();
		signs_hands_wanted = on && signs.wants_hands();
		// Looking: a picture went to the trackers lately.
		const uint64_t now = os_gettime_ns();
		bool looking = last_look_ns && now - last_look_ns < 500'000'000ULL;
		// ...or your face came from a phone lately, while it's shown (or
		// "while it's hidden" is on).
		if (!face_from_camera() && !looking)
			looking = phone_live && (signs_hidden || (last_render && now - last_render < 500'000'000ULL));
		const bool fresh = body_seen.looks != signs_body_looks;
		signs_body_looks = body_seen.looks;
		std::vector<SignFired> fired;
		if (!on || !looking) {
			signs.reset();
		} else {
			const Body *body = signs_hands_wanted && body_seen.ready ? &body_seen.body : nullptr;
			signs.step(dt, body, fresh, o.found && !o.holding ? o.moves : nullptr, t);
			fired = signs.take_fired();
		}
		if (!fired.empty()) {
			obs_weak_source_addref(weak_self);
			auto *job = new std::pair<obs_weak_source_t *, std::vector<SignFired>>(weak_self, std::move(fired));
			obs_queue_task(OBS_TASK_UI, send_signs_task, job, false);
		}
		std::lock_guard<std::mutex> l(state_lock);
		signs_state = signs.seen();
		signs_rules_state = signs.rules().size();
		signs_hands_state = signs_hands_wanted;
		signs_looking_state = on && looking;
	}

	// Watching for signs with the avatar hidden in OBS (a PNGtuber's, or a
	// 2D or 3D character in a scene not on now): the camera looked at
	// here, after tick, as drawing it would - and held "shown", so a
	// camera set to turn off when it isn't shown keeps going. OBS's
	// graphics thread.
	void look_while_hidden()
	{
		if (as_filter)
			return;
		bool shown;
		{
			std::lock_guard<std::mutex> l(state_lock);
			shown = showing_now;
		}
		obs_source_t *cam = !shown && signs_hidden && signs_any && camera_wanted() ? get_camera() : nullptr;
		if (cam != hidden_cam) {
			if (hidden_cam) {
				obs_source_dec_showing(hidden_cam);
				obs_source_release(hidden_cam);
			}
			hidden_cam = cam ? obs_source_get_ref(cam) : nullptr;
			if (hidden_cam)
				obs_source_inc_showing(hidden_cam);
		}
		const uint32_t cw = cam ? obs_source_get_width(cam) : 0, ch = cam ? obs_source_get_height(cam) : 0;
		const bool looking = cam && cw && ch;
		if (looking) {
			obs_enter_graphics();
			gs_blend_state_push();
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
			if (since_render > STALE) {
				staged = false;
				if (face_from_camera())
					worker.forget();
				pace.reset();
			}
			since_render = 0;
			look(cam, cw, ch);
			pick_shots();
			gs_blend_state_pop();
			obs_leave_graphics();
		}
		obs_source_release(cam);
		// Your face from a phone comes whether it's shown or not.
		const bool phone_looking = !face_from_camera() && phone_live && !shown && signs_hidden && signs_any;
		std::lock_guard<std::mutex> l(state_lock);
		looking_hidden_state = looking || phone_looking;
	}

	// A calibration is in the settings now (save_arms_task, on the UI
	// thread), or one was forgotten ("").
	void arms_saved(const std::string &text)
	{
		std::lock_guard<std::mutex> l(arms_lock);
		arms_cal_seen = text;
		arms_pending = false;
	}

	// 0.9.0: a move that stays, on from where it was: its move's time as
	// React plays it, and its idle (or its own back) due where the move
	// begins to ease out - at once if the move's gone (cut short by
	// another, stopped) or there's none (graphics thread).
	void step_stay(float dt)
	{
		if (stay_due)
			stay_age += dt;
		if (stay_swapped)
			return;
		bool found = false;
		float t = 0, amount = 0;
		for (const BodyMove &bm : react_now.body)
			if (stay_job.move >= 0 && bm.move == stay_job.move) {
				found = true;
				t = bm.time;
				amount = bm.amount;
			}
		const float ease = move_ease_out(stay_job.length);
		const float from = stay_job.length - ease;
		// Cut short by another move (easing out before its time): its idle
		// comes in under that one now.
		const bool cut = found && t > move_ease_in(stay_job.length) && amount < stay_amount - 1e-4f;
		stay_amount = found ? amount : 0;
		if (found && t < from && !cut)
			return;
		if (cut)
			found = false;
		stay_swapped = true;
		stay_due = true;
		stay_age = 0;
		stay_target = stay_job.start ? stay_job.idle : -1;
		// Landing on its idle's start as the move ends - unless the move
		// isn't there to cover it: a cross-fade then.
		stay_aligned = found;
		stay_ease = ease;
		stay_offset = found ? t - from : 0;
		stay_fade = found ? ease : STAY_FADE;
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
		if (since_find >= FIND_EVERY && !as_filter) {
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
		{
			const uint64_t now = os_gettime_ns();
			phone_live = !face_from_camera() && seen.ns && now >= seen.ns && now - seen.ns < 600'000'000ULL;
		}
		if (reset_shown.exchange(false))
			shown.reset();
		const Shown::Out o = shown.step(seen.face, seen.rest, dt, fresh);
		// Your face through its tuning, and what it does by itself on top:
		// breathing, swaying, its eyes' small moves, and when it can't see
		// you, your last look held a moment and then easing into idle.
		FaceTune t;
		AliveOptions ao;
		{
			std::lock_guard<std::mutex> l(pictures_lock);
			t = tune;
			ao = alive_options;
		}
		tune_moves(o.moves, t, tuned_now);
		alive.set(ao);
		const Shown::Glide &g = o.steady;
		alive_now = alive.step(dt, o.found, g.yaw, g.pitch, g.angle / RAD, tuned_now);
		// What it does on cue (react.hpp), round where its face was drawn.
		bool moving;
		{
			std::lock_guard<std::mutex> l(react_lock);
			react_now = react.step(dt, react_view);
			moving = react.moving();
			// A move that stays, newly asked for (its move went to React
			// with it, so it plays from this step).
			if (stay_ask.seq != stay_seen) {
				stay_seen = stay_ask.seq;
				stay_job = stay_ask;
				stay_swapped = false;
				stay_amount = 0;
				stay_on = stay_job.start;
			}
		}
		step_stay(dt);
		{
			std::lock_guard<std::mutex> l(companion_lock);
			companion_now = companion_player.step(dt);
			companion_hand_now = companion_hand.step(dt, ReactView()).hand;
			if (!companion_now.shown)
				companion_hand_now.shown = false;
			companion_shown = companion_ask;
		}
		react_hits += react_now.hits;
		step_vmc(dt);
		step_arms(dt);
		step_game(dt);
		step_signs(dt, o, t);
		// The page's live bars: what the camera reads, and what it shows.
		clock += dt;
		float reading[TUNE_GROUPS] = {}, shows[TUNE_GROUPS] = {};
		if (o.found) {
			tune_readings(o.moves, t, reading);
			tune_readings(tuned_now, t, shows);
		}
		for (int i = 0; i < TUNE_GROUPS; i++) {
			reading_peak[i].take(reading[i], clock);
			shows_peak[i].take(shows[i], clock);
		}
		// The mouth from your mic: all the time ("mic"; the camera's when
		// no sound comes in), or while you talk ("both").
		since_mic_find += dt;
		if (since_mic_find >= FIND_EVERY && !as_filter) {
			since_mic_find = 0;
			bool auto_mic;
			{
				std::lock_guard<std::mutex> l(mic_lock);
				auto_mic = mic_name.empty();
			}
			obs_source_t *m = get_mic();
			// Looked for again when it's gone, or when it's "" (OBS's own
			// Mic/Aux may have been set since).
			if (!m ? mouth_from != MOUTH_FROM_CAMERA : auto_mic)
				find_mic();
			obs_source_release(m);
		}
		const VoiceOut heard_now = voice.step(dt);
		VoiceProfile calibrated;
		if (voice.take_calibrated(calibrated)) {
			obs_weak_source_addref(weak_self);
			auto *job = new std::pair<obs_weak_source_t *, std::string>(weak_self, profile_text(calibrated));
			obs_queue_task(OBS_TASK_UI, save_profile_task, job, false);
		}
		const int from = mouth_from;
		const float share_to = from == MOUTH_FROM_CAMERA ? 0
				       : from == MOUTH_FROM_MIC  ? (heard_now.hearing ? 1.0f : 0.0f)
								 : heard_now.talking;
		const float share_tau = share_to > mic_now.share ? MIC_TAKE : MIC_GIVE;
		mic_now.share = share_to + (mic_now.share - share_to) * std::exp(-dt / share_tau);
		if (mic_now.share < 1e-3f)
			mic_now.share = 0;
		std::copy(heard_now.vowel, heard_now.vowel + VOWELS, mic_now.vowel);
		// Its drawn face, from yours (easing to rest when it can't see
		// you, blinking by itself) - its mouth from your mic as much as
		// the mic has it.
		float drawn_moves[EXPRESSIONS];
		std::copy(alive_now.moves, alive_now.moves + EXPRESSIONS, drawn_moves);
		mic_into_moves(mic_now, drawn_moves);
		// An "ouch" or a happy squint shuts its drawn eyes.
		for (int i = 1; i < EXPRESSIONS; i++)
			if (std::strncmp(EXPRESSION_NAMES[i], "eyeBlink", 8) == 0)
				drawn_moves[i] = std::max(drawn_moves[i], react_now.squint);
		drawn_now = drawn_face(drawn_moves, mirror, drawn_now);
		const Calibration cal = voice.calibration();
		std::lock_guard<std::mutex> l(state_lock);
		voice_state = heard_now;
		cal_state = cal;
		mic_share_state = mic_now.share;
		shown_now = o;
		state_now = seen;
		drawn_state = drawn_now;
		for (int i = 0; i < TUNE_GROUPS; i++) {
			tune_reading[i] = reading[i];
			tune_reading_peak[i] = reading_peak[i].value;
			tune_shows[i] = shows[i];
			tune_shows_peak[i] = shows_peak[i].value;
		}
		idle_state = alive_now.idle;
		breath_state = alive_now.breath;
		react_state.hits = react_hits;
		react_state.busy = react_now.busy;
		react_state.moving = moving;
		companion_state.phase = companion_now.phase;
		companion_state.busy = companion_now.busy;
		companion_state.petting = companion_now.petting;
		companion_state.move = companion_now.move;
		companion_state.patting = companion_hand_now.shown;
		companion_state.camera = companion_now.camera;
		stay_state.on = stay_on;
		stay_state.swapping = !stay_swapped || stay_due;
		stay_state.name = stay_on ? stay_job.name : "";
		react_state.flying = (int)react_now.flying.size();
		react_state.hand = react_now.hand;
		react_state.squint = react_now.squint;
		react_state.squish = react_now.squish;
		react_state.marks = (int)react_now.marks.size();
		react_state.bits = (int)react_now.bits.size();
		react_state.stuck = 0;
		for (const Flying &f : react_now.flying)
			react_state.stuck += f.stuck;
		if (os_gettime_ns() - last_render > 500'000'000ULL)
			showing_now = false;
	}

	// Hidden on every scene that's shown, OBS never draws it - and a 3D
	// character is only taken onto the graphics card as it's drawn, so it
	// said it was loading for ever. Here it's taken (rigged, measured) a
	// few times a second instead, so it's loaded - and says so - before
	// it's shown (0.8.1). OBS's graphics thread, after tick.
	void pick_while_hidden(float dt)
	{
		{
			std::lock_guard<std::mutex> l(state_lock);
			if (showing_now) {
				since_hidden_pick = 0;
				return;
			}
		}
		since_hidden_pick += dt;
		if (since_hidden_pick < HIDDEN_PICK_EVERY)
			return;
		since_hidden_pick = 0;
		obs_enter_graphics();
		{
			std::lock_guard<std::mutex> l(pictures_lock);
			if (as_filter) {
				// The throws filter's 3D things, ready before they're thrown.
				pick_props();
			} else {
				pick_model();
				pick_companion();
				apply_stay();
			}
		}
		obs_leave_graphics();
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

		// The mouth from your mic: which mic, the gate, how wide, and its
		// places from "say each vowel" when they're new (one just made and
		// not saved yet stays).
		const char *from = obs_data_get_string(settings, "mouth_from");
		const int from_now = from && std::strcmp(from, "camera") == 0 ? MOUTH_FROM_CAMERA
				     : from && std::strcmp(from, "mic") == 0  ? MOUTH_FROM_MIC
										: MOUTH_FROM_BOTH;
		const char *mic_text = obs_data_get_string(settings, "mic");
		bool mic_changed = mouth_from.exchange(from_now) != from_now;
		{
			std::lock_guard<std::mutex> l(mic_lock);
			mic_changed = mic_changed || mic_name != (mic_text ? mic_text : "");
			mic_name = mic_text ? mic_text : "";
		}
		if (mic_changed)
			find_mic();
		VoiceOptions vo;
		vo.start_db = (float)obs_data_get_int(settings, "mic_start");
		vo.full_db = (float)obs_data_get_int(settings, "mic_full");
		vo.amount = (float)obs_data_get_int(settings, "mic_amount") / 100.0f;
		const char *profile_text_now = obs_data_get_string(settings, "mic_profile");
		const std::string profile_now = profile_text_now ? profile_text_now : "";
		bool new_profile;
		{
			std::lock_guard<std::mutex> l(lock);
			new_profile = profile_now != profile_seen;
			profile_seen = profile_now;
		}
		if (new_profile) {
			if (!profile_from(profile_now, vo.profile))
				vo.profile = VoiceProfile();
			voice.set(vo);
		} else {
			voice.set_levels(vo);
		}

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
		// What it does on cue: the things thrown at it, the patting hand,
		// a hit's flash (a file still used keeps its picture) and the VRM
		// animation files it plays, each by its name.
		std::vector<PicturePtr> old_throws, next_throws, old_effects, next_effects;
		std::vector<HitLook> next_looks;
		PicturePtr next_hand, next_hit;
		std::vector<NamedMotion> old_motions, next_motions;
		{
			std::lock_guard<std::mutex> l(pictures_lock);
			old_throws = throw_pics;
			old_effects = effect_pics;
			next_hand = hand_pic;
			next_hit = hit_pic;
			old_motions = motions;
		}
		const json throws = json::parse(obs_data_get_string(settings, "react_items"), nullptr, false);
		std::vector<std::string> next_files, wanted_3d;
		if (throws.is_array())
			for (const json &row : throws) {
				if (next_throws.size() >= MOST_THROW_PICTURES)
					break;
				const std::string file = row.is_object() && row.contains("file") && row["file"].is_string()
								 ? row["file"].get<std::string>()
								 : "";
				next_files.push_back(file);
				// What it does as it hits (0.9.0): {"effect": "splat", "mark":
				// file, "drip": file, "bits": [files], "puff": file,
				// "seconds": 4, "squishy": false} - each picture by number.
				HitLook look;
				const json hit = row.is_object() && row.contains("hit") ? row["hit"] : json();
				if (hit.is_object()) {
					auto number = [&](const json &j) {
						const std::string f = j.is_string() ? j.get<std::string>() : "";
						if (f.empty())
							return -1;
						for (size_t i = 0; i < next_effects.size(); i++)
							if (next_effects[i] && next_effects[i]->path == f)
								return (int)i;
						if (next_effects.size() >= MOST_EFFECT_PICTURES)
							return -1;
						PicturePtr pic;
						for (const PicturePtr &old : old_effects)
							if (old && old->path == f)
								pic = old;
						next_effects.push_back(pic ? pic : load_picture(nullptr, f.c_str()));
						return (int)next_effects.size() - 1;
					};
					look.effect = effect_named(hit.value("effect", std::string()));
					look.mark = number(hit.value("mark", json()));
					look.drip = number(hit.value("drip", json()));
					look.puff = number(hit.value("puff", json()));
					const json bits = hit.value("bits", json());
					if (bits.is_array())
						for (const json &b : bits)
							if (look.bits.size() < 8) {
								const int n = number(b);
								if (n >= 0)
									look.bits.push_back(n);
							}
					const json secs = hit.value("seconds", json());
					look.seconds = secs.is_number() ? clampf(secs.get<float>(), 0.5f, MOST_MARK)
									: 4.0f;
					look.squishy = hit.value("squishy", false);
				}
				next_looks.push_back(look);
				// A 3D thing: loaded on its own thread (prop_loader).
				if (is_3d_file(file)) {
					next_throws.push_back(nullptr);
					wanted_3d.push_back(file);
					continue;
				}
				PicturePtr pic;
				for (const PicturePtr &old : old_throws)
					if (old && old->path == file)
						pic = old;
				next_throws.push_back(pic ? pic : load_picture(nullptr, file.c_str()));
			}
		next_hand = load_picture(next_hand, obs_data_get_string(settings, "react_hand"));
		next_hit = load_picture(next_hit, obs_data_get_string(settings, "react_hit"));
		// The 3D headpat hand, and its skin's and sleeve's colours.
		const char *hand3d_setting = obs_data_get_string(settings, "react_hand_3d");
		const std::string hand3d_now = hand3d_setting ? hand3d_setting : "";
		if (!hand3d_now.empty())
			wanted_3d.insert(wanted_3d.begin(), hand3d_now);
		const json tints = json::parse(obs_data_get_string(settings, "react_hand_tint"), nullptr, false);
		const V3 skin_tint = tints.is_object() ? tint_of(tints.value("skin", json())) : V3{1, 1, 1};
		const V3 sleeve_tint = tints.is_object() ? tint_of(tints.value("sleeve", json())) : V3{1, 1, 1};
		// Gaming hands' keyboard, mouse and pad (ours, with the add-on),
		// while they're on and shown.
		std::string game_files_now[DESK_PROPS];
		if (obs_data_get_bool(settings, "game_hands") && obs_data_get_bool(settings, "game_props"))
			for (int i = 0; i < DESK_PROPS; i++) {
				const std::string name = std::string("face/gaming/") + DESK_PROP_FILES[i] + ".glb";
				char *path = obs_module_file(name.c_str());
				game_files_now[i] = path ? path : "";
				bfree(path);
				if (!game_files_now[i].empty() &&
				    std::find(wanted_3d.begin(), wanted_3d.end(), game_files_now[i]) == wanted_3d.end())
					wanted_3d.push_back(game_files_now[i]);
			}
		prop_loader.want(wanted_3d);
		HandPlace place_now;
		const json hp = json::parse(obs_data_get_string(settings, "react_hand_place"), nullptr, false);
		if (hp.is_object()) {
			auto two = [&hp](const char *key, float *out) {
				const json &j = hp.contains(key) ? hp[key] : json();
				if (j.is_array() && j.size() == 2 && j[0].is_number() && j[1].is_number()) {
					out[0] = clampf(j[0].get<float>(), 0, 1);
					out[1] = clampf(j[1].get<float>(), 0, 1);
				}
			};
			two("touch", place_now.touch);
			two("wrist", place_now.wrist);
			if (hp.contains("width") && hp["width"].is_number())
				place_now.width = clampf(hp["width"].get<float>(), 0.2f, 4);
		}
		const json moves_json = json::parse(obs_data_get_string(settings, "react_moves"), nullptr, false);
		int motion_id = 0;
		{
			std::lock_guard<std::mutex> l(pictures_lock);
			motion_id = next_motion_id;
		}
		if (moves_json.is_array())
			for (const json &row : moves_json) {
				if (next_motions.size() >= MOST_MOTIONS || !row.is_object())
					continue;
				NamedMotion nm;
				nm.name = row.contains("name") && row["name"].is_string() ? row["name"].get<std::string>() : "";
				nm.file = row.contains("file") && row["file"].is_string() ? row["file"].get<std::string>() : "";
				if (nm.name.empty() || nm.file.empty())
					continue;
				for (const NamedMotion &old : old_motions)
					if (old.file == nm.file && (old.motion || !old.problem.empty())) {
						nm.motion = old.motion;
						nm.problem = old.problem;
						nm.id = old.id;
					}
				if (nm.id < 0) {
					Motion mo;
					std::string why;
					if (load_motion(nm.file, mo, &why))
						nm.motion = std::make_shared<const Motion>(std::move(mo));
					else
						nm.problem = why.empty() ? "it couldn't be read" : why;
					nm.id = motion_id++;
				}
				next_motions.push_back(nm);
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
			throw_pics.swap(next_throws);
			throw_files.swap(next_files);
			throw_looks.swap(next_looks);
			effect_pics.swap(next_effects);
			hand3d_file = hand3d_now;
			std::copy(std::begin(game_files_now), std::end(game_files_now), std::begin(game_files));
			hand_skin = skin_tint;
			hand_sleeve = sleeve_tint;
			hand_place = place_now;
			hand_pic.swap(next_hand);
			hit_pic.swap(next_hit);
			motions.swap(next_motions);
			next_motion_id = motion_id;
			have_spots = have;
			if (have)
				std::copy(got_spots, got_spots + PARTS, spots);
			canvas = canvas_now;
			if (new_shapes) {
				for (int i = 0; i < 3; i++)
					shapes[i].swap(next_shapes[i]);
				shape_key = shape_key_now;
			}
			size = clampf((float)obs_data_get_int(settings, "size") / 100.0f, 0.2f, 3.0f);
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
			spot_x = clampf((float)obs_data_get_double(settings, "spot_x") / 100.0f, -0.5f, 0.5f);
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
		const char *animation = obs_data_get_string(settings, "animation");
		l3.animation = animation ? animation : "";
		const char *still = obs_data_get_string(settings, "still_faces");
		l3.still_faces = still ? still : "";
		const char *framing = obs_data_get_string(settings, "framing");
		l3.framing = framing && std::strcmp(framing, "whole") == 0  ? 1
			     : framing && std::strcmp(framing, "head") == 0 ? 2
									    : 0;
		l3.hide = obs_data_get_bool(settings, "hide_behind_head");
		l3.smooth = obs_data_get_bool(settings, "smooth_edges");
		l3.fingers = clampf((float)obs_data_get_int(settings, "fingers") / 100.0f, 0, 1);
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
		// Its lights: a ready-made set, or its own from the page.
		const char *preset = obs_data_get_string(settings, "light_preset");
		l3.light_preset = light_preset_of(preset ? preset : "");
		if (l3.light_preset == LIGHTS_OWN) {
			// OBS's colours are 0xAABBGGRR.
			auto rgb = [settings](const char *key) {
				const uint32_t c = (uint32_t)obs_data_get_int(settings, key);
				return (c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF);
			};
			LightTerms own;
			own.side = (int)obs_data_get_int(settings, "light_side");
			own.height = (int)obs_data_get_int(settings, "light_height");
			own.colour = rgb("light_colour");
			own.brightness = (int)obs_data_get_int(settings, "light_brightness");
			own.room_colour = rgb("room_colour");
			own.room = (int)obs_data_get_int(settings, "room_brightness");
			own.rim_colour = rgb("rim_colour");
			own.rim = (int)obs_data_get_int(settings, "rim_brightness");
			l3.light_terms = own;
			l3.lights = lights_of(own);
		} else {
			l3.light_terms = preset_terms(l3.light_preset);
			l3.lights = preset_lights(l3.light_preset);
		}
		l3.detail = obs_data_get_bool(settings, "detail_maps");
		l3.soft_skin = clampf((float)obs_data_get_int(settings, "soft_skin") / 100.0f, 0, 1);
		const char *skin_parts = obs_data_get_string(settings, "skin_parts");
		l3.skin_parts = skin_parts ? skin_parts : "";
		// Its saved camera views, and the one shown ("main": as framed).
		const json views = json::parse(obs_data_get_string(settings, "views"), nullptr, false);
		if (views.is_array()) {
			for (const json &row : views) {
				if (l3.views.size() >= MOST_VIEWS)
					break;
				if (!row.is_object())
					continue;
				auto number = [&row](const char *key, float def) {
					auto it = row.find(key);
					return it != row.end() && it->is_number() ? (float)it->get<double>() : def;
				};
				CameraView cv;
				cv.yaw = number("yaw", 0);
				cv.pitch = number("pitch", 0);
				cv.zoom = number("zoom", 1);
				cv.x = number("x", 0);
				cv.y = number("y", 0);
				const auto name = row.find("name");
				l3.views.emplace_back(name != row.end() && name->is_string() ? name->get<std::string>() : "",
						      sensible_view(cv));
			}
		}
		const char *view = obs_data_get_string(settings, "view");
		const std::string view_name = view ? view : "";
		for (size_t i = 0; i < l3.views.size(); i++)
			if (view_name == std::to_string(i + 1))
				l3.view = (int)i;
		l3.view_glide = clampf((float)obs_data_get_int(settings, "view_glide") / 1000.0f, 0, 5);
		const char *setup_text = obs_data_get_string(settings, "model_setup");
		l3.setup_text = setup_text ? setup_text : "";
		{
			std::lock_guard<std::mutex> l(pictures_lock);
			if (look3d.setup_text == l3.setup_text)
				l3.setup = look3d.setup;
			else
				l3.setup = std::make_shared<CharacterSetup>(setup_from(l3.setup_text));
		}
		// Your face's tuning, and alive when you're still (alive.hpp).
		FaceTune t;
		for (int g = 0; g < TUNE_GROUPS; g++) {
			const std::string key = std::string("tune_") + TUNE_NAMES[g];
			t.range[g].from = clampf((float)obs_data_get_int(settings, (key + "_from").c_str()) / 100.0f, 0, 1);
			t.range[g].to = clampf((float)obs_data_get_int(settings, (key + "_to").c_str()) / 100.0f, 0, 1);
		}
		t.linked_blink = obs_data_get_bool(settings, "linked_blink");
		t.open_when_smiling = obs_data_get_bool(settings, "open_when_smiling");
		t.eye_move = clampf((float)obs_data_get_int(settings, "eye_move") / 100.0f, 0, 2);
		AliveOptions ao;
		ao.breath = clampf((float)obs_data_get_int(settings, "breathing") / 100.0f, 0, 1);
		ao.breath_time = clampf((float)obs_data_get_int(settings, "breath_time"), 2, 20);
		ao.sway = clampf((float)obs_data_get_int(settings, "sway") / 100.0f, 0, 1);
		ao.eye_moves = obs_data_get_bool(settings, "eye_moves");
		ao.auto_blink = obs_data_get_bool(settings, "auto_blink");
		{
			std::lock_guard<std::mutex> l(pictures_lock);
			look3d = l3;
			tune = t;
			alive_options = ao;
			body_follow = clampf((float)obs_data_get_int(settings, "body_follow") / 100.0f, 0, 1);
		}
		const char *model_file = obs_data_get_string(settings, "model");
		loader.want(model_file ? model_file : "");
		mirror = obs_data_get_bool(settings, "mirror");
		easy = obs_data_get_bool(settings, "easy");
		const char *style = obs_data_get_string(settings, "style");
		drawn_style = style && std::strcmp(style, "drawn") == 0;
		// Your face from a phone app (VTube Studio's, MeowFace, or
		// iFacialMocap), at the address its app shows - or the camera.
		// Switched, it starts again (nothing kept from the other); a face
		// from a phone can't be worn on your head in the camera's picture.
		const char *face_from = obs_data_get_string(settings, "face_from");
		const bool phone_now = face_from && std::strcmp(face_from, "phone") == 0;
		const bool vmc_now_face = face_from && std::strcmp(face_from, "vmc") == 0;
		const char *app = obs_data_get_string(settings, "phone_app");
		const PhoneApp phone_app = app && std::strcmp(app, "ifacialmocap") == 0 ? PHONE_IFACIALMOCAP
											 : PHONE_VTUBE_STUDIO;
		const char *address = obs_data_get_string(settings, "phone_address");
		const bool phone_changed = from_phone.exchange(phone_now) != phone_now;
		const bool vmc_changed = from_vmc.exchange(vmc_now_face) != vmc_now_face;
		if (phone_changed || vmc_changed) {
			worker.forget();
			new_camera = true;
			reset_shown = true;
		}
		phone.set(phone_app, phone_now && address ? address : "");
		// ...or from another tracker app (VMC), as your body and arms can
		// be: listening on its port while any of them is.
		vmc_body = obs_data_get_bool(settings, "vmc_body");
		vmc_arms = obs_data_get_bool(settings, "vmc_arms");
		vmc_mirror = obs_data_get_bool(settings, "vmc_mirror");
		{
			const int port = (int)obs_data_get_int(settings, "vmc_port");
			const bool want = vmc_now_face || vmc_body || vmc_arms;
			std::shared_ptr<VmcLink> old; // shut (if no one else holds it) once let go of
			std::lock_guard<std::mutex> l(vmc_lock);
			if (!want) {
				old.swap(vmc_link);
			} else if (!vmc_link || vmc_link->port() != port) {
				old.swap(vmc_link);
				vmc_link = VmcLink::open(port);
			}
		}
		worn = obs_data_get_bool(settings, "worn") && !phone_now && !vmc_now_face;
		// Gaming hands: on (your keyboard, mouse and pads read while they
		// are), which of them it uses, the hand your mouse is in, its own
		// keyboard, mouse and pad shown, resting, and its desk moved.
		{
			GameOptions go;
			const bool game_now = obs_data_get_bool(settings, "game_hands");
			const char *device = obs_data_get_string(settings, "game_device");
			go.device = device && std::strcmp(device, "keyboard") == 0 ? 1
				    : device && std::strcmp(device, "pad") == 0    ? 2
										   : 0;
			const char *mouse_hand = obs_data_get_string(settings, "game_mouse");
			go.mouse_right = !(mouse_hand && std::strcmp(mouse_hand, "left") == 0);
			go.props = obs_data_get_bool(settings, "game_props");
			go.rest_after = clampf((float)obs_data_get_int(settings, "game_rest"), 0, 600);
			go.height = clampf((float)obs_data_get_int(settings, "game_height") / 100.0f, -0.4f, 0.4f);
			go.distance = clampf((float)obs_data_get_int(settings, "game_distance") / 100.0f, -0.4f, 0.4f);
			std::shared_ptr<DeviceReader> old; // shut (if no one else holds it) once let go of
			std::lock_guard<std::mutex> l(game_lock);
			game_options = go;
			if (!game_now)
				old.swap(game_reader);
			else if (!game_reader)
				game_reader = DeviceReader::open();
			game_on = game_now;
		}
		// Smoothing: steadier one way; the other, snappier, and the
		// character aims further on past each look to keep up with you.
		const float smoothing = clampf((float)obs_data_get_int(settings, "smoothing") / 100.0f, 0, 1);
		worker.set_smoothing(smoothing);
		lead = 1 - smoothing;

		// Your hands and arms: on (its body models load the first time),
		// your hands too or not, light mode, how far it reaches and holds
		// its arms out, how long a lost hand stays, its own smoothing; and
		// its calibration, unless one just made isn't saved yet.
		ArmOptions arm_o;
		const bool arms_on_now = obs_data_get_bool(settings, "follow_arms");
		arm_o.fingers = obs_data_get_bool(settings, "arms_hands");
		arm_o.reach = clampf((float)obs_data_get_int(settings, "arms_reach") / 100.0f, 0.5f, 2);
		arm_o.spread = clampf((float)obs_data_get_int(settings, "arms_spread"), 0, 45);
		arm_o.hold = clampf((float)obs_data_get_int(settings, "arms_hold") / 1000.0f, 0, 3);
		{
			std::lock_guard<std::mutex> l(pictures_lock);
			arm_options = arm_o;
		}
		arms_on = arms_on_now;
		arms_hands = arm_o.fingers;
		arms_light = obs_data_get_bool(settings, "arms_light");
		// Hand signs and face moves: on, what to watch for (DireBot's
		// list), and while it's hidden too. A hand sign needs your hands
		// looked at, with or without the arms.
		const bool signs_on_now = obs_data_get_bool(settings, "hand_signs");
		const char *signs_text = obs_data_get_string(settings, "signs");
		int signs_skipped = 0;
		std::vector<SignRule> rules = rules_from(signs_text ? signs_text : "", &signs_skipped);
		if (signs_skipped)
			obs_log(LOG_WARNING, "DireBot Face Avatar: %d of its hand signs couldn't be read; set them again in DireBot.",
				signs_skipped);
		const bool hand_rule =
			std::any_of(rules.begin(), rules.end(), [](const SignRule &r) { return !r.face; });
		signs_hands = signs_on_now && hand_rule;
		signs_any = signs_on_now && !rules.empty();
		signs_on = signs_on_now;
		signs_hidden = obs_data_get_bool(settings, "signs_hidden");
		{
			std::lock_guard<std::mutex> l(signs_lock);
			signs_new = std::move(rules);
			signs_changed = true;
		}
		body_worker.set(arms_on_now || signs_hands, arm_o.fingers || signs_hands, arms_light,
				clampf((float)obs_data_get_int(settings, "arms_smoothing") / 100.0f, 0, 1));
		{
			const char *cal = obs_data_get_string(settings, "arms_calibration");
			const std::string cal_now = cal ? cal : "";
			std::lock_guard<std::mutex> l(arms_lock);
			if (!arms_pending && !arms.calibrating() && cal_now != arms_cal_seen) {
				arms_cal_seen = cal_now;
				if (!arms.load_calibration(cal_now)) {
					arms.clear_calibration();
					obs_log(LOG_WARNING,
						"DireBot Face Avatar: its hands' calibration couldn't be read; calibrate again in DireBot.");
				}
			}
		}

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

	// DireBot's FaceGame (OBS's UI thread): "Show me" - its hands on its
	// keyboard, mouse and pad as if you were typing, then playing, for
	// PRETEND_TIME seconds - or stop that.
	json game_to(const std::string &what)
	{
		auto no = [](const std::string &words) {
			return json{{"ok", false}, {"error", words}};
		};
		if (what == "stop") {
			game_stop = true;
			return json{{"ok", true}};
		}
		if (what != "show")
			return no("DireBot for OBS doesn't know that.");
		std::string why;
		bool showing;
		{
			std::lock_guard<std::mutex> l(state_lock);
			why = game_why_state;
			showing = showing_now;
		}
		if (!game_on)
			return no("Turn on gaming hands first.");
		if (why == "worn")
			return no("Gaming hands show on a 3D character in its own spot - not worn on your head.");
		if (why == "no_3d")
			return no("Gaming hands need a 3D VRM character - pick one first.");
		if (why == "not_vrm")
			return no("Gaming hands need a VRM character (.vrm) - other 3D files have no arms it knows.");
		if (why == "no_arms")
			return no("That character has no arms it can move.");
		if (!why.empty())
			return no("Gaming hands are just starting - try again in a moment.");
		if (!showing)
			return no("The face avatar isn't shown in OBS right now - show it, then try again.");
		game_show = true;
		return json{{"ok", true}, {"seconds", PRETEND_TIME}};
	}

	// DireBot's FaceArms (OBS's UI thread): calibrate (hold a hand up by
	// your ear, open, palm to the camera), stop, or forget it.
	json arms_to(const std::string &what)
	{
		auto no = [](const std::string &words) {
			return json{{"ok", false}, {"error", words}};
		};
		bool can, showing, ready, loading;
		{
			std::lock_guard<std::mutex> l(state_lock);
			can = arms_wanted_state;
			showing = showing_now;
			ready = body_state.ready;
			loading = body_state.loading;
		}
		if (what == "stop") {
			std::lock_guard<std::mutex> l(arms_lock);
			arms.stop_calibration();
		} else if (what == "forget") {
			{
				std::lock_guard<std::mutex> l(arms_lock);
				arms.stop_calibration();
				arms.clear_calibration();
			}
			obs_data_t *settings = obs_source_get_settings(self);
			obs_data_set_string(settings, "arms_calibration", "");
			obs_data_release(settings);
			arms_saved("");
		} else if (what == "calibrate") {
			if (!arms_on)
				return no("Turn on \"Follow my hands and arms\" first.");
			if (!arms_hands)
				return no("Turn on \"Follow my hands and fingers too\" first - it learns your open hand.");
			if (!can)
				return no("Hands and arms show on a 3D VRM character in its own spot - not worn on your head.");
			if (!ready)
				return no(loading ? "The body models are still loading - try again in a moment."
						  : "The body models couldn't be loaded - reinstall DireBot for OBS.");
			if (!showing)
				return no("Show the face avatar in OBS first - it only looks for you while it's shown.");
			std::lock_guard<std::mutex> l(arms_lock);
			arms.start_calibration();
		} else {
			return no("DireBot for OBS doesn't know that.");
		}
		return {{"ok", true}};
	}

	// DireBot's FaceReact (OBS's UI thread): a face for a while, a move,
	// something thrown at it, a headpat, or all of it stopped.
	json react_to(const json &data)
	{
		auto str = [&data](const char *key) {
			auto it = data.find(key);
			return it != data.end() && it->is_string() ? it->get<std::string>() : std::string();
		};
		auto num = [&data](const char *key, float def) {
			auto it = data.find(key);
			return it != data.end() && it->is_number() ? (float)it->get<double>() : def;
		};
		auto no = [](const std::string &words) {
			return json{{"ok", false}, {"error", words}};
		};
		auto same = [](const std::string &a, const std::string &b) {
			if (a.size() != b.size())
				return false;
			for (size_t i = 0; i < a.size(); i++)
				if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
					return false;
			return true;
		};
		// A face by its own name for it (any case: "Happy" is its "happy"),
		// whether it has it, and whether it's a 3D character.
		auto own_face = [this, &same](std::string &name, bool &three_d) {
			bool known = name.empty();
			std::lock_guard<std::mutex> l(state_lock);
			three_d = state3d.loaded;
			for (const auto *list : {&react_state.faces, &react_state.shapes})
				for (const std::string &n : *list)
					if (!known && same(n, name)) {
						name = n;
						known = true;
					}
			return known;
		};
		const std::string what = str("do");
		json answer = {{"ok", true}};
		// The throws filter wakes (it looks for your face from now).
		if (as_filter)
			wake_until = os_gettime_ns() + AWAKE_NS;
		if (what == "face") {
			std::string name = str("face");
			bool three_d;
			const bool known = own_face(name, three_d);
			{
				std::lock_guard<std::mutex> l(react_lock);
				react.face(name, num("seconds", 3), num("amount", 1));
			}
			answer["face"] = name;
			answer["shows"] = three_d && known;
			if (!three_d)
				answer["why"] = "Faces show on a 3D character.";
			else if (!known)
				answer["why"] = "This character has no face called that.";
		} else if (what == "move") {
			const std::string name = str("move");
			const BuiltIn b = builtin_named(name);
			if (b != BuiltIn::None) {
				// A wave or a cheer shows only on a VRM's arms.
				bool shows = true;
				if (builtin_arms(b)) {
					std::lock_guard<std::mutex> l(state_lock);
					shows = react_state.arms;
				}
				std::lock_guard<std::mutex> l(react_lock);
				react.move(b);
				answer["kind"] = builtin_arms(b) ? "arms" : "head";
				answer["shows"] = shows;
			} else {
				// A VRM animation file's, by its name (any case), then one of
				// its own animations.
				int id = -1;
				float length = 0;
				std::string problem;
				{
					std::lock_guard<std::mutex> l(pictures_lock);
					for (const NamedMotion &nm : motions)
						if (id < 0 && problem.empty() && same(nm.name, name)) {
							if (nm.motion) {
								id = nm.id;
								length = nm.motion->length;
							} else {
								problem = nm.problem;
							}
						}
				}
				if (id < 0 && problem.empty()) {
					std::lock_guard<std::mutex> l(state_lock);
					for (size_t i = 0; i < state3d.animations.size() && id < 0; i++)
						if (same(state3d.animations[i], name)) {
							id = (int)i;
							length = i < state3d.animation_lengths.size() ? state3d.animation_lengths[i] : 0;
						}
				}
				if (id < 0)
					return no(problem.empty() ? "It doesn't know a move called \"" + name + "\"."
								  : "That move's file can't be played: " + problem + ".");
				// A VRM animation file's plays only on a VRM character.
				bool shows = true;
				if (id >= MOTION_IDS) {
					std::lock_guard<std::mutex> l(state_lock);
					shows = react_state.motions;
				}
				std::lock_guard<std::mutex> l(react_lock);
				react.play(id, length);
				answer["kind"] = id >= MOTION_IDS ? "file" : "own";
				answer["shows"] = shows;
			}
		} else if (what == "throw") {
			Throw t;
			const std::string file = str("file");
			if (!file.empty()) {
				// One of chat's emotes, by its picture's file.
				t.picture = emote_number(file);
				if (t.picture < 0)
					return no("That emote's picture couldn't be opened.");
			} else {
				t.picture = (int)std::floor(num("item", 0));
				std::lock_guard<std::mutex> l(pictures_lock);
				if (t.picture < 0 || t.picture >= (int)throw_pics.size())
					return no("That thing to throw has no picture - pick one in DireBot.");
				const std::string &its = throw_files[t.picture];
				if (is_3d_file(its) && !props_ready.count(its)) {
					auto why = prop_problems.find(its);
					return no(why != prop_problems.end() ? "That 3D thing can't be thrown: " + why->second + "."
									     : "That 3D thing is still loading - try again in a moment.");
				}
				if (!is_3d_file(its) && !loaded(throw_pics[t.picture]))
					return no("That thing to throw has no picture - pick one in DireBot.");
			}
			{
				std::lock_guard<std::mutex> l(pictures_lock);
				t.tall = drawn_tall(t.picture);
				// What it does as it hits, as DireBot made it (not an emote).
				if (file.empty() && t.picture >= 0 && t.picture < (int)throw_looks.size())
					t.look = throw_looks[t.picture];
			}
			t.side = num("side", 0);
			t.strength = num("strength", 1);
			t.size = num("size", 1);
			t.face = str("face");
			bool three_d;
			own_face(t.face, three_d);
			t.face_time = num("face_seconds", 1.5f);
			// (0.8.1) Stuck to it a while, a bonk from above, and a
			// barrage: `count` of it, one after another.
			t.stick = num("stick", 0);
			t.above = str("from") == "above";
			const int count = (int)std::lround(clampf(num("count", 1), 1, (float)MOST_WAITING));
			float when = -1;
			int thrown = 0;
			{
				std::lock_guard<std::mutex> l(react_lock);
				for (int i = 0; i < count; i++) {
					const float w = react.throw_at(t);
					if (w < 0)
						break;
					if (!thrown)
						when = w;
					thrown++;
				}
			}
			if (!thrown)
				return no("Too much is being thrown at once - try again in a moment.");
			answer["hits_in"] = std::round(when * 1000) / 1000;
			answer["thrown"] = thrown;
			answer["every"] = THROW_GAP;
		} else if (what == "pat") {
			Pat p;
			p.count = (int)std::lround(clampf(num("count", 5), 1, (float)MOST_PATS));
			p.size = num("size", 1);
			p.side = num("side", 0);
			p.face = data.contains("face") ? str("face") : std::string("happy");
			bool three_d;
			own_face(p.face, three_d);
			std::lock_guard<std::mutex> l(react_lock);
			react.pat(p);
		} else if (what == "stop") {
			std::lock_guard<std::mutex> l(react_lock);
			react.stop();
		} else if (as_filter && (what == "companion" || what == "stay")) {
			return no("That's for a face avatar - this is a camera's throws filter.");
		} else if (what == "companion") {
			return companion_to(data);
		} else if (what == "stay") {
			return stay_to(data);
		} else {
			return no("DireBot for OBS doesn't know that.");
		}
		return answer;
	}

	// 0.9.0: its companion (companion.hpp), as DireBot's FaceReact asks:
	//   {"do": "companion", "action": "come", "file": ..., "clips": {"come":
	//    "", "idle": "", ...}, "place": {"size", "side", "made_for",
	//    "distance", "forward", "turn"}, "follow": 0-1, "view": {"yaw",
	//    "pitch", "zoom", "x", "y"} or none, "instant": false}
	//   {"action": "place", ...the same, nothing coming or going}
	//   {"action": "go", "scared": false, "instant": false}
	//   {"action": "pet", "count": 5, "size": 1}
	//   {"action": "move", "move": "howl"}: one of its own animations (any
	//    case) played once over its sitting, while it's out.
	//   {"action": "off"}: gone at once, its file let go of.
	// OBS's UI thread.
	json companion_to(const json &data)
	{
		auto str = [](const json &j, const char *key) {
			auto it = j.find(key);
			return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
		};
		auto num = [](const json &j, const char *key, float def) {
			auto it = j.find(key);
			return it != j.end() && it->is_number() ? (float)it->get<double>() : def;
		};
		auto flag = [](const json &j, const char *key) {
			auto it = j.find(key);
			return it != j.end() && it->is_boolean() && it->get<bool>();
		};
		auto no = [](const std::string &words) {
			return json{{"ok", false}, {"error", words}};
		};
		const std::string action = str(data, "action");
		json answer = {{"ok", true}};
		// Where it goes and how it's seen, as asked (each kept sensible).
		auto take_place = [&](CompanionAsk &a) {
			auto pl = data.find("place");
			if (pl != data.end() && pl->is_object()) {
				CompanionPlace p;
				p.size = num(*pl, "size", p.size);
				p.side = num(*pl, "side", p.side);
				p.made_for = num(*pl, "made_for", p.made_for);
				p.distance = num(*pl, "distance", p.distance);
				p.forward = num(*pl, "forward", p.forward);
				p.turn = num(*pl, "turn", p.turn);
				a.place = sensible_place(p);
			}
			if (data.contains("follow"))
				a.follow = clampf(num(data, "follow", 1), 0, 1);
			auto vw = data.find("view");
			if (vw != data.end()) {
				a.has_view = vw->is_object();
				if (a.has_view) {
					CameraView v;
					v.yaw = num(*vw, "yaw", 0);
					v.pitch = num(*vw, "pitch", 0);
					v.zoom = num(*vw, "zoom", 1);
					v.x = num(*vw, "x", 0);
					v.y = num(*vw, "y", 0);
					a.view = sensible_view(v);
				}
			}
			auto cl = data.find("clips");
			if (cl != data.end() && cl->is_object()) {
				bool changed = false;
				for (int k = 0; k < COMPANION_CLIPS; k++) {
					const std::string name = str(*cl, companion_clip_name(k));
					changed = changed || name != a.clips[k];
					a.clips[k] = name;
				}
				if (changed)
					a.clips_seq++;
			}
		};
		if (action == "come" || action == "place") {
			const std::string file = str(data, "file");
			if (file.empty())
				return no("Pick the companion's file (a .glb or .vrm) in DireBot.");
			if (!is_3d_file(file))
				return no("A companion is a 3D file: a .glb, .gltf or .vrm.");
			bool loaded_now;
			{
				std::lock_guard<std::mutex> l(state_lock);
				loaded_now = state3d.loaded;
			}
			std::lock_guard<std::mutex> l(companion_lock);
			CompanionAsk &a = companion_ask;
			if (file != a.file) {
				// Another one: the one there goes at once.
				companion_player.reset();
				companion_hand.stop();
				a.come = 0;
				a.file = file;
				a.clips_seq++;
			}
			take_place(a);
			companion_loader.want(file);
			const bool ready = a.ready == file && a.ready_seq == a.clips_seq;
			if (action == "come") {
				const bool instant = flag(data, "instant");
				bool came = true;
				if (ready && !a.come)
					came = companion_player.come(instant);
				else
					a.come = instant ? 2 : 1;
				answer["came"] = came;
				answer["loading"] = !ready;
			}
			answer["phase"] = companion_phase_name(companion_player.phase());
			if (!loaded_now)
				answer["why"] =
					"It shows beside a 3D character - this one isn't (or isn't loaded yet).";
		} else if (action == "go") {
			std::lock_guard<std::mutex> l(companion_lock);
			// Still loading: it never comes.
			bool went = companion_ask.come != 0;
			companion_ask.come = 0;
			went = companion_player.go(flag(data, "scared"), flag(data, "instant")) || went;
			companion_hand.stop();
			answer["went"] = went;
			answer["phase"] = companion_phase_name(companion_player.phase());
		} else if (action == "pet") {
			std::lock_guard<std::mutex> l(companion_lock);
			if (!companion_player.pet()) {
				answer["pets"] = false;
				answer["why"] = companion_player.phase() == CompanionPhase::Away
							? "The companion isn't here - bring it first."
							: "The companion is still coming or going.";
				return answer;
			}
			Pat p;
			p.count = (int)std::lround(clampf(num(data, "count", 5), 1, (float)MOST_PATS));
			p.size = num(data, "size", 1);
			// The character's hand: from its side of the companion.
			p.side = -companion_ask.place.side;
			p.face = "";
			companion_hand.pat(p);
			answer["pets"] = true;
		} else if (action == "move") {
			const std::string want = str(data, "move");
			if (want.empty())
				return no("Which of the companion's animations? (\"move\": its name)");
			// Its file's animations as the graphics thread loaded them
			// (state_lock, then companion_lock: never both at once).
			std::string loaded_file;
			bool loaded = false;
			std::vector<std::string> names;
			std::vector<float> lengths;
			{
				std::lock_guard<std::mutex> l(state_lock);
				loaded_file = companion_state.file;
				loaded = companion_state.loaded;
				names = companion_state.animations;
				lengths = companion_state.lengths;
			}
			std::lock_guard<std::mutex> l(companion_lock);
			const CompanionPhase phase = companion_player.phase();
			if (phase == CompanionPhase::Away || companion_ask.file.empty() || !loaded ||
			    loaded_file != companion_ask.file) {
				answer["moves"] = false;
				answer["why"] = phase == CompanionPhase::Away
							? "The companion isn't here - bring it first."
							: "The companion is still loading.";
				return answer;
			}
			auto lower = [](std::string s) {
				for (char &c : s)
					c = (char)std::tolower((unsigned char)c);
				return s;
			};
			const std::string low = lower(want);
			int index = -1;
			for (size_t i = 0; i < names.size() && i < lengths.size(); i++)
				if (lower(names[i]) == low) {
					index = (int)i;
					break;
				}
			if (index < 0)
				return no("The companion has no animation called \"" + want + "\".");
			if (!companion_player.play(index, lengths[index])) {
				answer["moves"] = false;
				answer["why"] = phase != CompanionPhase::Out ? "The companion is still coming or going."
						: companion_player.playing() >= 0
							? "The companion is busy with another move."
							: "That animation has no length to play.";
				return answer;
			}
			answer["moves"] = true;
			answer["length"] = lengths[index];
		} else if (action == "off") {
			std::lock_guard<std::mutex> l(companion_lock);
			companion_player.reset();
			companion_hand.stop();
			const uint64_t seq = companion_ask.clips_seq;
			companion_ask = CompanionAsk();
			companion_ask.clips_seq = seq + 1;
			companion_loader.want("");
		} else {
			return no("DireBot for OBS doesn't know that.");
		}
		return answer;
	}

	// 0.9.0: a move that stays, as DireBot's FaceReact asks:
	//   {"do": "stay", "action": "start", "move": "dragon_come", "idle":
	//    "dragon_idle", "view": {...} or none, "companion": "scared"}
	//   {"action": "end", "move": "dragon_go"}
	// Its moves are the character's own animations ("" none: straight to
	// its idle, cross-faded). "companion": its companion goes as it starts
	// ("go" or "scared"). OBS's UI thread.
	json stay_to(const json &data)
	{
		auto str = [&data](const char *key) {
			auto it = data.find(key);
			return it != data.end() && it->is_string() ? it->get<std::string>() : std::string();
		};
		auto num = [](const json &j, const char *key, float def) {
			auto it = j.find(key);
			return it != j.end() && it->is_number() ? (float)it->get<double>() : def;
		};
		auto no = [](const std::string &words) {
			return json{{"ok", false}, {"error", words}};
		};
		// One of its own animations by name (any case): its number and
		// length, or -1.
		auto own = [this](const std::string &name, float &length) {
			length = 0;
			if (name.empty())
				return -1;
			std::lock_guard<std::mutex> l(state_lock);
			for (size_t i = 0; i < state3d.animations.size(); i++) {
				const std::string &n = state3d.animations[i];
				bool same = n.size() == name.size();
				for (size_t c = 0; same && c < n.size(); c++)
					same = std::tolower((unsigned char)n[c]) ==
					       std::tolower((unsigned char)name[c]);
				if (same) {
					length = i < state3d.animation_lengths.size() ? state3d.animation_lengths[i]
										      : 0;
					return (int)i;
				}
			}
			return -1;
		};
		const std::string action = str("action");
		if (action != "start" && action != "end")
			return no("DireBot for OBS doesn't know that.");
		{
			std::lock_guard<std::mutex> l(state_lock);
			if (!state3d.loaded)
				return no(
					"Moves that stay are a 3D character's - this one isn't (or isn't loaded yet).");
		}
		StayAsk a;
		a.start = action == "start";
		const std::string move = str("move");
		a.move = own(move, a.length);
		if (!move.empty() && a.move < 0)
			return no("This character has no animation called \"" + move + "\".");
		if (a.start) {
			const std::string idle = str("idle");
			float idle_length;
			a.idle = own(idle, idle_length);
			if (a.idle < 0)
				return no(idle.empty() ? "Pick the animation it stays in."
						       : "This character has no animation called \"" + idle + "\".");
			a.name = idle;
			auto vw = data.find("view");
			a.has_view = vw != data.end() && vw->is_object();
			if (a.has_view) {
				CameraView v;
				v.yaw = num(*vw, "yaw", 0);
				v.pitch = num(*vw, "pitch", 0);
				v.zoom = num(*vw, "zoom", 1);
				v.x = num(*vw, "x", 0);
				v.y = num(*vw, "y", 0);
				a.view = sensible_view(v);
			}
		}
		{
			std::lock_guard<std::mutex> l(react_lock);
			a.seq = stay_ask.seq + 1;
			if (!a.start) {
				a.name = stay_ask.name;
				a.has_view = false;
			}
			stay_ask = a;
			if (a.move >= 0)
				react.play(a.move, a.length);
		}
		json answer = {{"ok", true}};
		const std::string scare = str("companion");
		if (a.start && (scare == "go" || scare == "scared")) {
			std::lock_guard<std::mutex> l(companion_lock);
			companion_ask.come = 0;
			companion_player.go(scare == "scared");
			companion_hand.stop();
			answer["companion"] = companion_phase_name(companion_player.phase());
		}
		return answer;
	}

	// One of chat's emotes thrown, by its picture's file (DireBot keeps
	// them, small): its number for React (EMOTE_IDS + where it's kept) -
	// opened the first time, the newest MOST_EMOTES kept (the one thrown
	// longest ago makes room); -1 if it can't be opened.
	int emote_number(const std::string &file)
	{
		{
			std::lock_guard<std::mutex> l(pictures_lock);
			for (size_t i = 0; i < emotes.size(); i++)
				if (emotes[i].pic && emotes[i].pic->path == file && loaded(emotes[i].pic)) {
					emotes[i].used = ++emotes_used;
					return EMOTE_IDS + (int)i;
				}
		}
		// Opened (onto the graphics card) with no lock held: the graphics
		// thread holds its own while it draws, then takes pictures_lock.
		PicturePtr pic = std::make_shared<Picture>(file);
		if (!loaded(pic))
			return -1;
		PicturePtr old;
		int n;
		{
			std::lock_guard<std::mutex> l(pictures_lock);
			size_t at = emotes.size();
			if (emotes.size() >= MOST_EMOTES) {
				at = 0;
				for (size_t i = 1; i < emotes.size(); i++)
					if (emotes[i].used < emotes[at].used)
						at = i;
				old = std::move(emotes[at].pic);
			} else {
				emotes.emplace_back();
			}
			emotes[at].pic = pic;
			emotes[at].used = ++emotes_used;
			n = EMOTE_IDS + (int)at;
		}
		// The one let go of closes here, no lock held.
		old.reset();
		return n;
	}

	json state(bool with_points)
	{
		const VoiceOptions mic_levels = voice.options();
		// The character's shape first, on its own: the graphics thread
		// takes the pictures' lock, then the state's.
		const char *spot_names[PARTS] = {"left_eye", "right_eye", "mouth"};
		json holes = json::object();
		bool shaped;
		json lights_json, views_json = json::array();
		json react_moves_json = json::array(), react_items_json = json::array();
		bool react_hand_ok = false, react_hit_ok = false, react_hand_3d = false;
		std::string react_hand_problem;
		int react_emotes = 0;
		int view_index;
		std::string companion_file_state;
		{
			std::lock_guard<std::mutex> l(companion_lock);
			companion_file_state = companion_ask.file;
		}
		const bool companion_loading_state = companion_loader.busy();
		int game_props_loaded = 0, game_props_wanted = 0;
		std::string game_props_problem;
		{
			std::lock_guard<std::mutex> l(pictures_lock);
			for (int i = 0; i < PARTS; i++)
				holes[spot_names[i]] = shapes[0] && shapes[0]->hole[i];
			shaped = shapes[0] && shapes[0]->ready();
			const LightTerms &t = look3d.light_terms;
			lights_json = {{"preset", LIGHT_PRESET_NAMES[look3d.light_preset]},
				       {"side", t.side},
				       {"height", t.height},
				       {"colour", colour_text(t.colour)},
				       {"brightness", t.brightness},
				       {"room_colour", colour_text(t.room_colour)},
				       {"room", t.room},
				       {"rim_colour", colour_text(t.rim_colour)},
				       {"rim", t.rim},
				       {"detail", look3d.detail},
				       {"soft_skin", (int)std::lround(look3d.soft_skin * 100)}};
			for (const NamedMotion &nm : motions) {
				json row = {{"name", nm.name}, {"length", nm.motion ? std::round(nm.motion->length * 100) / 100 : 0}};
				if (!nm.problem.empty())
					row["problem"] = nm.problem;
				react_moves_json.push_back(row);
			}
			for (size_t i = 0; i < throw_pics.size(); i++) {
				const std::string &file = i < throw_files.size() ? throw_files[i] : std::string();
				// What it does as it hits (0.9.0), and whether all its
				// pictures for that are loaded.
				json hit;
				if (i < throw_looks.size() && throw_looks[i].effect != Effect::None) {
					const HitLook &look = throw_looks[i];
					bool all = true;
					for (int n : {look.mark, look.drip, look.puff})
						all = all && (n < 0 ||
							      (n < (int)effect_pics.size() && loaded(effect_pics[n])));
					for (int n : look.bits)
						all = all && n < (int)effect_pics.size() && loaded(effect_pics[n]);
					hit = {{"effect", effect_name(look.effect)}, {"loaded", all}};
				}
				if (!is_3d_file(file)) {
					json row = {{"loaded", loaded(throw_pics[i])}};
					if (!hit.is_null())
						row["hit"] = hit;
					react_items_json.push_back(row);
					continue;
				}
				// A 3D one: loaded on its own (or why not).
				json row = {{"loaded", props_ready.count(file) > 0}, {"three_d", true}};
				if (!hit.is_null())
					row["hit"] = hit;
				auto why = prop_problems.find(file);
				if (why != prop_problems.end())
					row["problem"] = why->second;
				react_items_json.push_back(row);
			}
			react_hand_ok = loaded(hand_pic);
			react_hand_3d = !hand3d_file.empty() && props_ready.count(hand3d_file) > 0;
			if (!hand3d_file.empty()) {
				auto why = prop_problems.find(hand3d_file);
				if (why != prop_problems.end())
					react_hand_problem = why->second;
			}
			react_emotes = (int)emotes.size();
			react_hit_ok = loaded(hit_pic);
			// Gaming hands' keyboard, mouse and pad: how many are wanted
			// and loaded (or why not).
			for (int i = 0; i < DESK_PROPS; i++) {
				const std::string &file = game_files[i];
				if (file.empty() || (i > 0 && file == game_files[i - 1]))
					continue;
				game_props_wanted++;
				if (props_ready.count(file))
					game_props_loaded++;
				auto why = prop_problems.find(file);
				if (why != prop_problems.end() && game_props_problem.empty())
					game_props_problem = DESK_PROP_FILES[i] + std::string(": ") + why->second;
			}
			for (const auto &named : look3d.views)
				views_json.push_back({{"name", named.first},
						      {"yaw", named.second.yaw},
						      {"pitch", named.second.pitch},
						      {"zoom", named.second.zoom},
						      {"x", named.second.x},
						      {"y", named.second.y}});
			view_index = look3d.view;
		}
		bool arms_calibrating, arms_calibrated;
		std::string arms_result;
		GameOptions game_o;
		bool game_asked;
		{
			std::lock_guard<std::mutex> l(game_lock);
			game_o = game_options;
			game_asked = game_on;
		}
		{
			std::lock_guard<std::mutex> l(arms_lock);
			arms_calibrating = arms.calibrating();
			arms_calibrated = arms.calibrated();
			arms_result = arms.calibration_result();
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
		// Your face's tuning, live: what the camera reads for each group
		// and what the character shows from it (alive.hpp), now and at its
		// highest lately.
		{
			json reading = json::object(), reading_peak = json::object(), shows = json::object(),
			     shows_peak = json::object();
			for (int i = 0; i < TUNE_GROUPS; i++) {
				reading[TUNE_NAMES[i]] = r(tune_reading[i], 100);
				reading_peak[TUNE_NAMES[i]] = r(tune_reading_peak[i], 100);
				shows[TUNE_NAMES[i]] = r(tune_shows[i], 100);
				shows_peak[TUNE_NAMES[i]] = r(tune_shows_peak[i], 100);
			}
			answer["tune"] = {{"reading", reading},
					  {"reading_peak", reading_peak},
					  {"shows", shows},
					  {"shows_peak", shows_peak}};
		}
		answer["alive"] = {{"idle", r(idle_state, 100)}, {"breath", r(breath_state, 100)}};
		// The mouth from your mic: which mic it hears, how loud (dBFS,
		// as OBS's meter), what it hears, and "say each vowel".
		{
			const VoiceOut &vo = voice_state;
			const Calibration &c = cal_state;
			std::string asked, found;
			{
				std::lock_guard<std::mutex> l2(mic_lock);
				asked = mic_name;
				found = mic_found;
			}
			const int from = mouth_from;
			json vowels = json::object();
			for (int k = 0; k < VOWELS; k++)
				vowels[VOWEL_NAMES[k]] = r(vo.vowel[k], 100);
			json cal = {{"running", c.running},
				    {"vowel", VOWEL_NAMES[std::min(std::max(c.vowel, 0), VOWELS - 1)]},
				    {"number", c.vowel + 1},
				    {"listening", c.listening},
				    {"left", r(std::max(0.0f, c.left), 10)},
				    {"done", c.done}};
			if (c.done) {
				json got = json::object();
				for (int k = 0; k < VOWELS; k++)
					got[VOWEL_NAMES[k]] = c.heard[k];
				cal["heard"] = got;
				cal["own"] = c.result.own;
			}
			answer["mic"] = {{"from", from == MOUTH_FROM_CAMERA ? "camera"
						  : from == MOUTH_FROM_MIC  ? "mic"
									    : "both"},
					 {"mic", asked},
					 {"found", !found.empty()},
					 {"listening_to", found},
					 {"hearing", vo.hearing},
					 {"muted", vo.muted},
					 {"level", r(vo.level_db, 10)},
					 {"peak", r(vo.peak_db, 10)},
					 {"start", r(mic_levels.start_db, 10)},
					 {"full", r(mic_levels.full_db, 10)},
					 {"heard", vo.heard},
					 {"voiced", vo.voiced},
					 {"talking", r(vo.talking, 100)},
					 {"open", r(vo.open, 100)},
					 {"share", r(mic_share_state, 100)},
					 {"vowels", vowels},
					 {"f1", std::round(vo.f1)},
					 {"f2", std::round(vo.f2)},
					 {"own_voice", mic_levels.profile.own},
					 {"calibration", cal}};
		}
		// For "Check my camera": where your face is in the camera's
		// picture (shares of it), which way you face the camera itself
		// (degrees, as Turn - not against "Set"), and how lit it is.
		{
			json check = {{"seen", face.found}};
			if (face.found && face.w > 0 && face.h > 0) {
				float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
				for (const Point &p : face.points) {
					x0 = std::min(x0, p.x);
					y0 = std::min(y0, p.y);
					x1 = std::max(x1, p.x);
					y1 = std::max(y1, p.y);
				}
				check["box"] = {{"x", r(x0 / face.w, 1e3f)},
						{"y", r(y0 / face.h, 1e3f)},
						{"w", r((x1 - x0) / face.w, 1e3f)},
						{"h", r((y1 - y0) / face.h, 1e3f)}};
				const Turn facing = turn_of(face.pose);
				check["facing"] = {{"yaw", r(facing.yaw, 10)},
						   {"pitch", r(facing.pitch, 10)},
						   {"roll", r(facing.roll, 10)}};
			}
			const Light &lit = state_now.light;
			if (lit.seen)
				check["light"] = {{"face", r(lit.face, 100)},
						  {"left", r(lit.left, 100)},
						  {"right", r(lit.right, 100)},
						  {"room", r(lit.room, 100)}};
			check["camera_width"] = cam_w.load();
			check["camera_height"] = cam_h.load();
			check["camera_fps"] = r(cam_fps.load(), 10);
			answer["camera_check"] = check;
		}
		answer["style"] = drawn_style ? "drawn" : "real";
		answer["worn"] = worn.load();
		// Your face from a phone app: whether it's set to ask one, its
		// socket, where it asks and our port, why it can't, and what came
		// (and from where else - the phone may have another address).
		answer["face_from"] = from_phone ? "phone" : from_vmc ? "vmc" : "camera";
		{
			const PhoneLink::State ps = phone.state();
			answer["phone"] = {{"on", ps.on},
					   {"listening", ps.listening},
					   {"way", ps.app == PHONE_IFACIALMOCAP ? "ifacialmocap" : "vtube_studio"},
					   {"address", ps.address},
					   {"phone", ps.phone},
					   {"port", ps.port},
					   {"problem", ps.problem},
					   {"packets", ps.packets},
					   {"per_second", r(ps.per_second, 10)},
					   {"since", ps.since < 0 ? -1.0f : r(ps.since, 10)},
					   {"found", ps.found},
					   {"shapes", ps.shapes},
					   {"others", ps.others},
					   {"other", ps.other},
					   {"unread", ps.unread}};
		}
		// Another tracker app (VMC, 0.9.0): whether it listens and where,
		// why it can't, what came (and from where), what the app sends
		// lately part by part, and which parts the character takes from it
		// now (or why it can't).
		{
			std::shared_ptr<VmcLink> link;
			{
				std::lock_guard<std::mutex> l(vmc_lock);
				link = vmc_link;
			}
			const VmcLink::State vs = link ? link->state() : VmcLink::State();
			VmcSent sent;
			double at = 0;
			if (link)
				link->sent(sent, at);
			const VmcSends &sn = vmc_sends_state;
			answer["vmc"] = {{"on", (bool)link},
					 {"face", (bool)from_vmc},
					 {"body", (bool)vmc_body},
					 {"arms", (bool)vmc_arms},
					 {"mirror", (bool)vmc_mirror},
					 {"listening", vs.listening},
					 {"port", link ? link->port() : 0},
					 {"problem", vs.problem},
					 {"packets", vs.packets},
					 {"per_second", r(vs.per_second, 10)},
					 {"since", vs.since < 0 ? -1.0f : r(vs.since, 10)},
					 {"from", vs.from},
					 {"others", vs.others},
					 {"other", vs.other},
					 {"unread", vs.unread},
					 {"loaded", sent.loaded},
					 {"calibration", sent.calibration},
					 {"sends",
					  {{"bones", sn.bones},
					   {"body", sn.body},
					   {"legs", sn.legs},
					   {"head", sn.head},
					   {"eyes", sn.eyes},
					   {"arms", sn.arms},
					   {"fingers", sn.fingers},
					   {"shapes", sn.shapes},
					   {"perfect_sync", sn.perfect_sync}}},
					 {"using",
					  {{"face", vmc_using_state[0]},
					   {"body", vmc_using_state[1]},
					   {"arms", vmc_using_state[2]},
					   {"fingers", vmc_using_state[3]}}},
					 {"why_not", vmc_why_state}};
		}
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
		answer["lights"] = lights_json;
		{
			// What it does on cue: the faces it makes (its own shapes too),
			// whether it has arms to wave with and plays VRM animation
			// files, the moves it knows and the hits so far.
			const ReactState &rs = react_state;
			json faces = json::array(), shapes = json::array();
			for (const std::string &n : rs.faces)
				faces.push_back({{"name", n}});
			for (const std::string &n : rs.shapes)
				shapes.push_back({{"name", n}});
			answer["react"] = {{"faces", faces},
					   {"shapes", shapes},
					   {"arms", rs.arms},
					   {"vrm_moves", rs.motions},
					   {"moves", react_moves_json},
					   {"items", react_items_json},
					   {"hand", react_hand_ok},
					   {"hand_3d", react_hand_3d},
					   {"emotes", react_emotes},
					   {"hit", react_hit_ok},
					   {"hits", rs.hits},
					   {"flying", rs.flying},
					   {"stuck", rs.stuck},
					   {"marks", rs.marks},
					   {"bits", rs.bits},
					   {"patting", rs.hand.shown},
					   {"squint", std::round(rs.squint * 1000) / 1000},
					   {"squish", std::round(rs.squish * 1000) / 1000},
					   {"busy", rs.busy},
					   {"moving", rs.moving}};
			if (!react_hand_problem.empty())
				answer["react"]["hand_3d_problem"] = react_hand_problem;
			// The throws filter (0.9.0): awake, and your face in the camera
			// (pixels) as last seen - `face`: in the newest look.
			if (as_filter) {
				const CameraFace &cf = cam_face_state;
				auto r1 = [](float v) {
					return std::round(v * 10) / 10;
				};
				answer["camera"] = {{"awake", rs.busy || os_gettime_ns() < wake_until.load()},
						    {"face", cf.now},
						    {"seen", cf.seen},
						    {"x", r1(cf.x)},
						    {"y", r1(cf.y)},
						    {"head", r1(cf.head)},
						    {"roll", std::round(cf.roll * 1000) / 1000},
						    {"shake", camera_shake.load()}};
			}
			if (rs.hand.shown) {
				auto r3 = [](float v) { return std::round(v * 1000) / 1000; };
				answer["react"]["hand_now"] = {{"side", rs.hand.side},	       {"rise", r3(rs.hand.rise)},
							       {"away", r3(rs.hand.away)},     {"lift", r3(rs.hand.lift)},
							       {"opacity", r3(rs.hand.opacity)}};
			}
		}
		answer["camera_view"] = {{"view", view_index >= 0 ? std::to_string(view_index + 1) : "main"},
					 {"showing",
					  view_shown_state >= 0 ? std::to_string(view_shown_state + 1) : "main"},
					 {"gliding", view_gliding_state},
					 {"cue", view_cue_state},
					 {"views", views_json}};
		{
			// 0.9.0: its companion (its file, loaded or why not, its
			// animations and which it uses for what, where a headpat's hand
			// lands, and what it's doing), and a move that stays.
			const CompanionState &cs = companion_state;
			json anims = json::array(), clips = json::object();
			for (const std::string &n : cs.animations)
				anims.push_back({{"name", n}});
			for (int k = 0; k < COMPANION_CLIPS; k++)
				clips[companion_clip_name(k)] = cs.clips[k];
			answer["companion"] = {{"file", companion_file_state},
					       {"loaded", cs.loaded && cs.file == companion_file_state},
					       {"loading", companion_loading_state},
					       {"problem", cs.file == companion_file_state ? cs.problem : ""},
					       {"animations", anims},
					       {"clips", clips},
					       {"pat", cs.pat},
					       {"phase", companion_phase_name(cs.phase)},
					       {"shown", cs.phase != CompanionPhase::Away},
					       {"drawn", cs.drawn},
					       {"busy", cs.busy},
					       {"petting", cs.petting},
					       {"moving", cs.move >= 0 && cs.move < (int)cs.animations.size()
								  ? cs.animations[cs.move]
								  : ""},
					       {"patting", cs.patting},
					       {"camera", cs.camera},
					       {"height", r(cs.size, 1e4f)}};
			answer["stay"] = {{"on", stay_state.on},
					  {"swapping", stay_state.swapping},
					  {"idle", stay_state.name}};
		}
		{
			const State3D &st = state3d;
			json values = json::object();
			for (const auto &kv : st.values)
				values[kv.first] = r(kv.second, 100);
			// (Objects: OBS's own answers carry lists of objects only.)
			json animations = json::array();
			for (const std::string &name : st.animations)
				animations.push_back({{"name", name}});
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
				      {"animations", animations},
				      {"animation", st.animation},
				      {"swinging_chain", st.chain},
				      {"arkit_parts", st.arkit},
				      {"set_up", st.set_up},
				      {"finger_joints", st.fingers},
				      {"following_bones", st.following},
				      {"following_bones_left_out", st.following_left_out},
				      {"body", st.body},
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
			json materials = json::array();
			int bumpy = 0, shaded = 0, skin = 0, guessed = 0;
			for (const State3D::Mat &mat : st.materials) {
				materials.push_back({{"name", mat.name},
						     {"bumps", mat.bumps},
						     {"own_tangents", mat.bumps && !mat.guessed_tangents},
						     {"shadows", mat.shadows},
						     {"skin", mat.skin}});
				bumpy += mat.bumps;
				guessed += mat.guessed_tangents;
				shaded += mat.shadows;
				skin += mat.skin;
			}
			model["materials"] = materials;
			model["bump_parts"] = bumpy;
			model["guessed_tangent_parts"] = guessed;
			model["shadow_parts"] = shaded;
			model["skin_parts"] = skin;
			if (st.rigged) {
				model["blinks"] = st.blinks;
				model["talks"] = st.talks;
			}
			if (st.framed_w > 0 && st.framed_h > 0)
				model["framed"] = {{"w", r(st.framed_w, 1e3f)}, {"h", r(st.framed_h, 1e3f)}};
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
		answer["hands_and_arms"] =
			arms_state(with_points, arms_calibrating, arms_calibrated, arms_result);
		// Gaming hands (0.9.0): on, whether your keyboard, mouse and pads
		// are read here (or why not, in words), how many pads are in,
		// which it's using and whether each was used in the last second,
		// its hands typing and how much they're on them, "Show me", its
		// props - never anything about which keys.
		{
			const DeviceState &ds = devices_state;
			const DeskNow &dn = desk_state;
			const bool reading = game_reading_state;
			answer["gaming"] = {
				{"on", game_asked},
				{"device", game_o.device == 1 ? "keyboard" : game_o.device == 2 ? "pad" : "auto"},
				{"mouse_hand", game_o.mouse_right ? "right" : "left"},
				{"props", game_o.props},
				{"rest", game_o.rest_after},
				{"reading",
				 {{"keyboard", reading && ds.keyboard},
				  {"mouse", reading && ds.mouse},
				  {"pads", reading && ds.pads},
				  {"keyboard_problem", reading ? ds.keyboard_problem : ""},
				  {"mouse_problem", reading ? ds.mouse_problem : ""},
				  {"pads_problem", reading ? ds.pads_problem : ""},
				  {"pads_in", reading ? ds.pads_in : 0}}},
				{"shown", game_wanted_state},
				{"why_not", game_why_state},
				{"using", dn.device == GameDevice::Pad		 ? "pad"
					  : dn.device == GameDevice::KeyboardMouse ? "keyboard_mouse"
										   : "none"},
				{"typing", dn.typing},
				{"hands_on", r(dn.on, 100)},
				{"used", {{"keys", dn.keys}, {"mouse", dn.mouse}, {"pad", dn.pad}}},
				{"show_me", game_show_state >= 0},
				{"show_left", game_show_state >= 0 ? r((float)(PRETEND_TIME - game_show_state), 10) : 0.0f},
				{"props_loaded", game_props_loaded},
				{"props_wanted", game_props_wanted},
				{"props_problem", game_props_problem}};
		}
		answer["signs"] = signs_json();
		answer["looking_hidden"] = looking_hidden_state;
		return answer;
	}

	// For FaceGetState (under state_lock): the hand signs and face moves
	// it watches for - on, looking now, whether your hands are looked at,
	// watching while hidden - and what it sees: each hand's sign (your
	// own left and right), up by your face or not, waving; your face's
	// moves (0 to 1); how many it's sent, and the last one's id.
	json signs_json() const
	{
		auto r = [](float v) {
			return std::round(v * 100) / 100;
		};
		const SignsSeen &s = signs_state;
		json hands = json::object();
		const char *const sides[2] = {"left", "right"};
		for (int k = 0; k < 2; k++) {
			const HandNow &h = s.hands[k];
			hands[sides[k]] = {{"found", h.found},
					   {"sign", h.sign >= 0 ? json(HAND_SIGN_NAMES[h.sign]) : json()},
					   {"sure", r(h.score)},
					   {"raised", h.raised},
					   {"waving", h.waving}};
		}
		json face = json::object();
		for (int m = 0; m < FACE_MOVES; m++)
			face[FACE_MOVE_NAMES[m]] = r(s.face.value[m]);
		face["wink_left"] = r(s.face.wink[0]);
		face["wink_right"] = r(s.face.wink[1]);
		return {{"on", signs_on.load()},
			{"rules", signs_rules_state},
			{"watching", signs_looking_state},
			{"hands_looked_at", signs_hands_state},
			{"hidden", signs_hidden.load()},
			{"looking_hidden", looking_hidden_state},
			{"hands", hands},
			{"face_found", s.face_found},
			{"face", face},
			{"sent", s.fired},
			{"last", s.last}};
	}

	// For FaceGetState (under state_lock): your hands and arms - on, and
	// whether this character can follow them (else why not); the body
	// tracker; what it follows now (your own left and right); its
	// calibration; for "Check my camera", what of you the camera sees;
	// and, asked for, your body's and hands' points (shares of the picture).
	json arms_state(bool with_points, bool calibrating, bool calibrated, const std::string &result) const
	{
		auto r = [](float v, float steps) {
			return std::round(v * steps) / steps;
		};
		const BodyWorker::State &bs = body_state;
		const Body &b = bs.body;
		const bool mirrored = mirror;
		// Its sides -> yours: mirrored, its left arm is your right.
		auto yours = [&](const bool its[2], int you) {
			return its[mirrored ? 1 - you : you];
		};
		json a = {{"on", arms_on.load()},
			  {"hands", arms_hands.load()},
			  {"light", arms_light.load()},
			  {"can", arms_wanted_state},
			  {"why_not", arms_why_state},
			  {"loading", bs.loading},
			  {"ready", bs.ready},
			  {"looks_per_second", r(bs.rate, 10)},
			  {"most_per_second", r(1 / arms_every(), 10)},
			  {"body_per_second", r(bs.body_rate, 10)},
			  {"ms", r(bs.ms, 10)},
			  {"palm_finder", bs.palm},
			  {"found", b.found},
			  {"following",
			   {{"left_arm", yours(arms_seen_state.arm, 0)},
			    {"right_arm", yours(arms_seen_state.arm, 1)},
			    {"left_hand", yours(arms_seen_state.hand, 0)},
			    {"right_hand", yours(arms_seen_state.hand, 1)}}},
			  {"calibrating", calibrating},
			  {"calibrated", calibrated},
			  {"calibration", result}};
		if (!bs.problem.empty())
			a["problem"] = bs.problem;
		// How often each of your hands was found over the last few seconds,
		// and how lit it was when it was.
		json hands_seen = json::object();
		for (int you = 0; you < 2; you++) {
			const BodyWorker::HandsSeen &hs = bs.hands[you];
			json h = {{"found", r(hs.found, 100)}};
			if (hs.lit) {
				h["light"] = r(hs.light, 100);
				h["around"] = r(hs.around, 100);
			}
			hands_seen[you ? "right" : "left"] = h;
		}
		a["hands_seen"] = hands_seen;
		// What of you is in the picture: a point counts if the body model
		// thinks it's in sight and it's inside the picture.
		json framing = {{"found", b.found}};
		if (b.found && b.w > 0 && b.h > 0) {
			auto in_sight = [&b](int i) {
				const Point &p = b.points[i];
				return b.seen[i] >= 0.5f && p.x >= 0 && p.y >= 0 && p.x < b.w && p.y < b.h;
			};
			auto both = [&](int left, int right) {
				return (int)in_sight(left) + (int)in_sight(right);
			};
			framing["head"] = in_sight(BODY_NOSE);
			framing["shoulders"] = both(BODY_LEFT_SHOULDER, BODY_RIGHT_SHOULDER);
			framing["elbows"] = both(BODY_LEFT_ELBOW, BODY_RIGHT_ELBOW);
			framing["wrists"] = both(BODY_LEFT_WRIST, BODY_RIGHT_WRIST);
			framing["hips"] = both(BODY_LEFT_HIP, BODY_RIGHT_HIP);
			framing["hands"] = (int)b.hands[0].found + (int)b.hands[1].found;
			framing["turned"] = b.turned;
			// Your shoulders' width and their middle's height (shares of
			// the picture): too close, too far, too low.
			const Point &sl = b.points[BODY_LEFT_SHOULDER], &sr = b.points[BODY_RIGHT_SHOULDER];
			framing["shoulder_width"] = r(std::fabs(sl.x - sr.x) / b.w, 1e3f);
			framing["shoulder_height"] = r((sl.y + sr.y) * 0.5f / b.h, 1e3f);
		}
		a["framing"] = framing;
		if (with_points && b.found && b.w > 0 && b.h > 0) {
			// Your top half (MediaPipe's first 25 points), then each hand.
			auto at = [&](const Point &p) {
				return json{{"x", r((p.x + 0.5f) / b.w, 1e4f)}, {"y", r((p.y + 0.5f) / b.h, 1e4f)}};
			};
			json body_points = json::array();
			for (int i = 0; i <= BODY_RIGHT_HIP; i++) {
				json p = at(b.points[i]);
				p["seen"] = r(b.seen[i], 100);
				body_points.push_back(p);
			}
			a["points"] = body_points;
			const char *names[2] = {"left_hand_points", "right_hand_points"};
			for (int you = 0; you < 2; you++) {
				if (!b.hands[you].found)
					continue;
				json pts = json::array();
				for (const Point &p : b.hands[you].points)
					pts.push_back(at(p));
				a[names[you]] = pts;
			}
		}
		return a;
	}

	static void on_rename(void *data, calldata_t *cd)
	{
		auto *f = static_cast<FaceAvatar *>(data);
		const char *was = calldata_string(cd, "prev_name");
		const char *now = calldata_string(cd, "new_name");
		obs_source_t *renamed = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
		if (!was || !now || !renamed)
			return;
		bool camera_renamed;
		{
			std::lock_guard<std::mutex> l(f->lock);
			camera_renamed = f->camera_name == was &&
					 (f->camera ? obs_weak_source_references_source(f->camera, renamed)
						    : obs_source_get_type(renamed) == OBS_SOURCE_TYPE_INPUT);
			if (camera_renamed)
				f->camera_name = now;
		}
		bool mic_renamed;
		{
			std::lock_guard<std::mutex> l(f->mic_lock);
			// The one it listens to, by any name (its name for the page).
			if (f->mic && obs_weak_source_references_source(f->mic, renamed))
				f->mic_found = now;
			mic_renamed = !f->mic_name.empty() && f->mic_name == was &&
				      (f->mic ? obs_weak_source_references_source(f->mic, renamed) : is_audio(renamed));
			if (mic_renamed)
				f->mic_name = now;
		}
		if (!camera_renamed && !mic_renamed)
			return;
		obs_data_t *settings = obs_source_get_settings(f->self);
		if (camera_renamed)
			obs_data_set_string(settings, "source", now);
		if (mic_renamed)
			obs_data_set_string(settings, "mic", now);
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

void save_profile_task(void *param)
{
	std::unique_ptr<std::pair<obs_weak_source_t *, std::string>> job(
		static_cast<std::pair<obs_weak_source_t *, std::string> *>(param));
	obs_source_t *src = obs_weak_source_get_source(job->first);
	obs_weak_source_release(job->first);
	if (!src)
		return;
	obs_data_t *settings = obs_source_get_settings(src);
	obs_data_set_string(settings, "mic_profile", job->second.c_str());
	obs_data_release(settings);
	if (auto *f = static_cast<FaceAvatar *>(obs_obj_get_data(src)))
		f->profile_saved(job->second);
	obs_source_release(src);
}

void save_arms_task(void *param)
{
	std::unique_ptr<std::pair<obs_weak_source_t *, std::string>> job(
		static_cast<std::pair<obs_weak_source_t *, std::string> *>(param));
	obs_source_t *src = obs_weak_source_get_source(job->first);
	obs_weak_source_release(job->first);
	if (!src)
		return;
	obs_data_t *settings = obs_source_get_settings(src);
	obs_data_set_string(settings, "arms_calibration", job->second.c_str());
	obs_data_release(settings);
	if (auto *f = static_cast<FaceAvatar *>(obs_obj_get_data(src)))
		f->arms_saved(job->second);
	obs_source_release(src);
}

// Each sign held its time, to DireBot: a FaceSign event (OBS's UI thread).
void send_signs_task(void *param)
{
	std::unique_ptr<std::pair<obs_weak_source_t *, std::vector<SignFired>>> job(
		static_cast<std::pair<obs_weak_source_t *, std::vector<SignFired>> *>(param));
	obs_source_t *src = obs_weak_source_get_source(job->first);
	obs_weak_source_release(job->first);
	if (!src)
		return;
	const char *name = obs_source_get_name(src);
	for (const SignFired &s : job->second)
		emit_event("FaceSign",
			   {{"source", name ? name : ""}, {"id", s.id}, {"what", s.what}, {"side", s.side}});
	obs_source_release(src);
}

const char *get_name(void *)
{
	return text("FaceAvatar.Name");
}

obs_data_t *filter_settings(obs_data_t *settings);

void update_filter(FaceAvatar *f, obs_data_t *settings)
{
	obs_data_t *safe = filter_settings(settings);
	f->update(safe);
	obs_data_release(safe);
	f->camera_shake = obs_data_get_bool(settings, "camera_shake");
}

void *create(obs_data_t *settings, obs_source_t *source)
{
	auto *f = new FaceAvatar(source);
	f->update(settings);
	signal_handler_connect(obs_get_signal_handler(), "source_rename", FaceAvatar::on_rename, f);
	return f;
}

void destroy_filter(void *data)
{
	delete static_cast<FaceAvatar *>(data);
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
	// 0.9.0: its spot moved across the picture (room beside it for a companion).
	obs_data_set_default_double(settings, "spot_x", 0);
	obs_data_set_default_string(settings, "look", "auto");
	obs_data_set_default_int(settings, "outline", 0);
	obs_data_set_default_int(settings, "floppy", 50);
	obs_data_set_default_bool(settings, "swing", true);
	obs_data_set_default_string(settings, "animation", "");
	obs_data_set_default_string(settings, "still_faces", "");
	obs_data_set_default_string(settings, "framing", "auto");
	obs_data_set_default_bool(settings, "hide_behind_head", true);
	obs_data_set_default_bool(settings, "smooth_edges", true);
	obs_data_set_default_int(settings, "fingers", 50);
	// Your face's tuning (alive.hpp's starting values), and alive when
	// you're still.
	for (int g = 0; g < TUNE_GROUPS; g++) {
		const std::string key = std::string("tune_") + TUNE_NAMES[g];
		obs_data_set_default_int(settings, (key + "_from").c_str(), tune_default(g, false));
		obs_data_set_default_int(settings, (key + "_to").c_str(), tune_default(g, true));
	}
	obs_data_set_default_bool(settings, "linked_blink", true);
	obs_data_set_default_bool(settings, "open_when_smiling", true);
	obs_data_set_default_int(settings, "eye_move", 100);
	obs_data_set_default_int(settings, "body_follow", 30);
	obs_data_set_default_int(settings, "breathing", 50);
	obs_data_set_default_int(settings, "breath_time", 5);
	obs_data_set_default_int(settings, "sway", 50);
	obs_data_set_default_bool(settings, "eye_moves", true);
	obs_data_set_default_bool(settings, "auto_blink", false);
	obs_data_set_default_string(settings, "model_place", "");
	obs_data_set_default_string(settings, "model_setup", "");
	// The studio's lights in plain terms (lights.hpp), OBS's colours
	// 0xAABBGGRR.
	const LightTerms studio;
	auto obs_colour = [](uint32_t rgb) {
		return (long long)(0xFF000000u | (rgb & 0xFF) << 16 | (rgb & 0xFF00) | (rgb >> 16 & 0xFF));
	};
	obs_data_set_default_string(settings, "light_preset", "studio");
	obs_data_set_default_int(settings, "light_side", studio.side);
	obs_data_set_default_int(settings, "light_height", studio.height);
	obs_data_set_default_int(settings, "light_colour", obs_colour(studio.colour));
	obs_data_set_default_int(settings, "light_brightness", studio.brightness);
	obs_data_set_default_int(settings, "room_colour", obs_colour(studio.room_colour));
	obs_data_set_default_int(settings, "room_brightness", studio.room);
	obs_data_set_default_int(settings, "rim_colour", obs_colour(studio.rim_colour));
	obs_data_set_default_int(settings, "rim_brightness", studio.rim);
	obs_data_set_default_bool(settings, "detail_maps", true);
	obs_data_set_default_int(settings, "soft_skin", 0);
	obs_data_set_default_string(settings, "skin_parts", "");
	obs_data_set_default_string(settings, "views", "[]");
	// What it does on cue (DireBot sets these): pictures to throw ([{"file"}]),
	// the patting hand, a hit's flash, VRM animation files ([{"name", "file"}]).
	obs_data_set_default_string(settings, "react_items", "[]");
	obs_data_set_default_string(settings, "react_hand", "");
	obs_data_set_default_string(settings, "react_hand_3d", "");
	obs_data_set_default_string(settings, "react_hand_tint", "{}");
	obs_data_set_default_string(settings, "react_hand_place", "{}");
	obs_data_set_default_string(settings, "react_hit", "");
	obs_data_set_default_string(settings, "react_moves", "[]");
	obs_data_set_default_string(settings, "view", "main");
	obs_data_set_default_int(settings, "view_glide", 600);
	obs_data_set_default_string(settings, "mouth_from", "both");
	obs_data_set_default_string(settings, "mic", "");
	obs_data_set_default_int(settings, "mic_start", -50);
	obs_data_set_default_int(settings, "mic_full", -30);
	obs_data_set_default_int(settings, "mic_amount", 100);
	obs_data_set_default_string(settings, "mic_profile", "");
	// Your hands and arms: off until you turn them on (they use more of
	// your PC), and its calibration (ArmSolver::save_calibration).
	obs_data_set_default_bool(settings, "follow_arms", false);
	obs_data_set_default_bool(settings, "arms_hands", true);
	obs_data_set_default_bool(settings, "arms_light", false);
	obs_data_set_default_int(settings, "arms_reach", 100);
	obs_data_set_default_int(settings, "arms_spread", 0);
	obs_data_set_default_int(settings, "arms_hold", 500);
	obs_data_set_default_int(settings, "arms_smoothing", 50);
	obs_data_set_default_string(settings, "arms_calibration", "");
	// Hand signs and face moves that set things off: off, none, and only
	// while it's shown until DireBot sets them.
	obs_data_set_default_bool(settings, "hand_signs", false);
	obs_data_set_default_string(settings, "signs", "[]");
	obs_data_set_default_bool(settings, "signs_hidden", false);
	// Your face from the camera until you pick a phone app (0.9.0).
	obs_data_set_default_string(settings, "face_from", "camera");
	obs_data_set_default_string(settings, "phone_app", "vtube_studio");
	obs_data_set_default_string(settings, "phone_address", "");
	// Nothing from another tracker app until you pick it (0.9.0).
	obs_data_set_default_bool(settings, "vmc_body", false);
	obs_data_set_default_bool(settings, "vmc_arms", false);
	obs_data_set_default_bool(settings, "vmc_mirror", false);
	obs_data_set_default_int(settings, "vmc_port", VMC_PORT);
	// Gaming hands: off until you turn them on (0.9.0) - your keyboard,
	// mouse and pads aren't read till then.
	obs_data_set_default_bool(settings, "game_hands", false);
	obs_data_set_default_string(settings, "game_device", "auto");
	obs_data_set_default_string(settings, "game_mouse", "right");
	obs_data_set_default_bool(settings, "game_props", true);
	obs_data_set_default_int(settings, "game_rest", 0);
	obs_data_set_default_int(settings, "game_height", 0);
	obs_data_set_default_int(settings, "game_distance", 0);
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
	obs_property_t *face_from = obs_properties_add_list(props, "face_from", text("FaceAvatar.FaceFrom"),
							    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(face_from, text("FaceAvatar.FaceFromCamera"), "camera");
	obs_property_list_add_string(face_from, text("FaceAvatar.FaceFromPhone"), "phone");
	obs_property_list_add_string(face_from, text("FaceAvatar.FaceFromVmc"), "vmc");
	obs_property_t *apps = obs_properties_add_list(props, "phone_app", text("FaceAvatar.PhoneApp"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(apps, "VTube Studio", "vtube_studio");
	obs_property_list_add_string(apps, "MeowFace", "meowface");
	obs_property_list_add_string(apps, "iFacialMocap", "ifacialmocap");
	obs_properties_add_text(props, "phone_address", text("FaceAvatar.PhoneAddress"), OBS_TEXT_DEFAULT);
	obs_properties_add_bool(props, "vmc_body", text("FaceAvatar.VmcBody"));
	obs_properties_add_bool(props, "vmc_arms", text("FaceAvatar.VmcArms"));
	obs_properties_add_bool(props, "vmc_mirror", text("FaceAvatar.VmcMirror"));
	obs_properties_add_int(props, "vmc_port", text("FaceAvatar.VmcPort"), 1024, 65535, 1);
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
	obs_property_t *p = obs_properties_add_int_slider(props, "size", text("FaceAvatar.Size"), 20, 300, 1);
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
	// Its own animations, as the character loaded has them.
	obs_property_t *anim = obs_properties_add_list(m3, "animation", text("FaceAvatar.Animation"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(anim, text("FaceAvatar.AnimationNone"), "");
	if (f) {
		std::lock_guard<std::mutex> l(f->state_lock);
		for (const std::string &name : f->state3d.animations)
			obs_property_list_add_string(anim, name.c_str(), name.c_str());
	}
	obs_property_t *framing = obs_properties_add_list(m3, "framing", text("FaceAvatar.Framing"),
							  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(framing, text("FaceAvatar.FramingAuto"), "auto");
	obs_property_list_add_string(framing, text("FaceAvatar.FramingWhole"), "whole");
	obs_property_list_add_string(framing, text("FaceAvatar.FramingHead"), "head");
	obs_properties_add_bool(m3, "hide_behind_head", text("FaceAvatar.HideBehindHead"));
	obs_properties_add_bool(m3, "smooth_edges", text("FaceAvatar.SmoothEdges"));
	p = obs_properties_add_int_slider(m3, "fingers", text("FaceAvatar.Fingers"), 0, 100, 1);
	obs_property_int_set_suffix(p, "%");
	obs_properties_add_bool(m3, "detail_maps", text("FaceAvatar.DetailMaps"));
	p = obs_properties_add_int_slider(m3, "soft_skin", text("FaceAvatar.SoftSkin"), 0, 100, 1);
	obs_property_int_set_suffix(p, "%");
	obs_properties_add_text(m3, "skin_parts", text("FaceAvatar.SkinParts"), OBS_TEXT_MULTILINE);
	obs_property_t *shown_view = obs_properties_add_list(m3, "view", text("FaceAvatar.View"), OBS_COMBO_TYPE_LIST,
							     OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(shown_view, text("FaceAvatar.ViewMain"), "main");
	if (f) {
		std::lock_guard<std::mutex> l(f->pictures_lock);
		for (size_t i = 0; i < f->look3d.views.size(); i++) {
			const std::string key = std::to_string(i + 1);
			const std::string &name = f->look3d.views[i].first;
			obs_property_list_add_string(shown_view, (key + (name.empty() ? "" : ": " + name)).c_str(),
						     key.c_str());
		}
	}
	p = obs_properties_add_int_slider(m3, "view_glide", text("FaceAvatar.ViewGlide"), 0, 3000, 50);
	obs_property_int_set_suffix(p, " ms");
	p = obs_properties_add_float_slider(m3, "spot_x", text("FaceAvatar.SpotX"), -50, 50, 0.1);
	obs_property_float_set_suffix(p, "%");
	obs_properties_add_group(props, "model3d", text("FaceAvatar.Model3D"), OBS_GROUP_NORMAL, m3);

	// Its lights (a 3D character's).
	obs_properties_t *lit = obs_properties_create();
	obs_property_t *presets = obs_properties_add_list(lit, "light_preset", text("FaceAvatar.LightPreset"),
							  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	for (int i = 0; i < LIGHT_PRESETS; i++)
		obs_property_list_add_string(presets, text((std::string("FaceAvatar.Lights.") + LIGHT_PRESET_NAMES[i]).c_str()),
					     LIGHT_PRESET_NAMES[i]);
	obs_properties_add_text(lit, "lights_about", text("FaceAvatar.LightsAbout"), OBS_TEXT_INFO);
	p = obs_properties_add_int_slider(lit, "light_side", text("FaceAvatar.LightSide"), -180, 180, 1);
	obs_property_int_set_suffix(p, "\xC2\xB0");
	p = obs_properties_add_int_slider(lit, "light_height", text("FaceAvatar.LightHeight"), -90, 90, 1);
	obs_property_int_set_suffix(p, "\xC2\xB0");
	obs_properties_add_color(lit, "light_colour", text("FaceAvatar.LightColour"));
	p = obs_properties_add_int_slider(lit, "light_brightness", text("FaceAvatar.LightBrightness"), 0, 300, 5);
	obs_property_int_set_suffix(p, "%");
	obs_properties_add_color(lit, "room_colour", text("FaceAvatar.RoomColour"));
	p = obs_properties_add_int_slider(lit, "room_brightness", text("FaceAvatar.RoomBrightness"), 0, 300, 5);
	obs_property_int_set_suffix(p, "%");
	obs_properties_add_color(lit, "rim_colour", text("FaceAvatar.RimColour"));
	p = obs_properties_add_int_slider(lit, "rim_brightness", text("FaceAvatar.RimBrightness"), 0, 300, 5);
	obs_property_int_set_suffix(p, "%");
	obs_properties_add_group(props, "lights", text("FaceAvatar.Lights"), OBS_GROUP_NORMAL, lit);

	// Your face's tuning: where each of the camera's readings starts and
	// where it's full (DireBot's page shows them live).
	obs_properties_t *tune = obs_properties_create();
	obs_properties_add_text(tune, "tune_about", text("FaceAvatar.TuneAbout"), OBS_TEXT_INFO);
	for (int g = 0; g < TUNE_GROUPS; g++) {
		const std::string key = std::string("tune_") + TUNE_NAMES[g], name = std::string("FaceAvatar.Tune.") +
										     TUNE_NAMES[g];
		const std::string from = std::string(text(name.c_str())) + " - " + text("FaceAvatar.TuneFrom"),
				  to = std::string(text(name.c_str())) + " - " + text("FaceAvatar.TuneTo");
		p = obs_properties_add_int_slider(tune, (key + "_from").c_str(), from.c_str(), 0, 100, 1);
		obs_property_int_set_suffix(p, "%");
		p = obs_properties_add_int_slider(tune, (key + "_to").c_str(), to.c_str(), 0, 100, 1);
		obs_property_int_set_suffix(p, "%");
	}
	obs_properties_add_bool(tune, "linked_blink", text("FaceAvatar.LinkedBlink"));
	obs_properties_add_bool(tune, "open_when_smiling", text("FaceAvatar.OpenWhenSmiling"));
	p = obs_properties_add_int_slider(tune, "eye_move", text("FaceAvatar.EyeMove"), 0, 200, 5);
	obs_property_int_set_suffix(p, "%");
	obs_properties_add_group(props, "tune", text("FaceAvatar.Tune"), OBS_GROUP_NORMAL, tune);

	// Alive when you're still.
	obs_properties_t *alive = obs_properties_create();
	p = obs_properties_add_int_slider(alive, "breathing", text("FaceAvatar.Breathing"), 0, 100, 1);
	obs_property_int_set_suffix(p, "%");
	p = obs_properties_add_int_slider(alive, "breath_time", text("FaceAvatar.BreathTime"), 2, 20, 1);
	obs_property_int_set_suffix(p, " s");
	p = obs_properties_add_int_slider(alive, "sway", text("FaceAvatar.Sway"), 0, 100, 1);
	obs_property_int_set_suffix(p, "%");
	p = obs_properties_add_int_slider(alive, "body_follow", text("FaceAvatar.BodyFollow"), 0, 100, 1);
	obs_property_int_set_suffix(p, "%");
	obs_properties_add_bool(alive, "eye_moves", text("FaceAvatar.EyeMoves"));
	obs_properties_add_bool(alive, "auto_blink", text("FaceAvatar.AutoBlink"));
	obs_properties_add_group(props, "alive", text("FaceAvatar.Alive"), OBS_GROUP_NORMAL, alive);

	// Your hands and arms (3D VRM characters).
	obs_properties_t *arms = obs_properties_create();
	obs_properties_add_text(arms, "arms_about", text("FaceAvatar.ArmsAbout"), OBS_TEXT_INFO);
	obs_properties_add_bool(arms, "follow_arms", text("FaceAvatar.FollowArms"));
	obs_properties_add_bool(arms, "arms_hands", text("FaceAvatar.ArmsHands"));
	obs_properties_add_bool(arms, "arms_light", text("FaceAvatar.ArmsLight"));
	p = obs_properties_add_int_slider(arms, "arms_reach", text("FaceAvatar.ArmsReach"), 50, 200, 5);
	obs_property_int_set_suffix(p, "%");
	p = obs_properties_add_int_slider(arms, "arms_spread", text("FaceAvatar.ArmsSpread"), 0, 45, 1);
	obs_property_int_set_suffix(p, "\xC2\xB0");
	p = obs_properties_add_int_slider(arms, "arms_hold", text("FaceAvatar.ArmsHold"), 0, 3000, 100);
	obs_property_int_set_suffix(p, " ms");
	p = obs_properties_add_int_slider(arms, "arms_smoothing", text("FaceAvatar.ArmsSmoothing"), 0, 100, 1);
	obs_property_int_set_suffix(p, "%");
	obs_properties_add_group(props, "hands_and_arms", text("FaceAvatar.HandsAndArms"), OBS_GROUP_NORMAL, arms);

	// Gaming hands (3D VRM characters).
	obs_properties_t *game = obs_properties_create();
	obs_properties_add_text(game, "game_about", text("FaceAvatar.GameAbout"), OBS_TEXT_INFO);
	obs_properties_add_bool(game, "game_hands", text("FaceAvatar.GameHands"));
	obs_property_t *device = obs_properties_add_list(game, "game_device", text("FaceAvatar.GameDevice"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(device, text("FaceAvatar.GameDeviceAuto"), "auto");
	obs_property_list_add_string(device, text("FaceAvatar.GameDeviceKeyboard"), "keyboard");
	obs_property_list_add_string(device, text("FaceAvatar.GameDevicePad"), "pad");
	obs_property_t *mouse_hand = obs_properties_add_list(game, "game_mouse", text("FaceAvatar.GameMouse"),
							     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(mouse_hand, text("FaceAvatar.GameMouseRight"), "right");
	obs_property_list_add_string(mouse_hand, text("FaceAvatar.GameMouseLeft"), "left");
	obs_properties_add_bool(game, "game_props", text("FaceAvatar.GameProps"));
	p = obs_properties_add_int_slider(game, "game_rest", text("FaceAvatar.GameRest"), 0, 600, 5);
	obs_property_int_set_suffix(p, " s");
	p = obs_properties_add_int_slider(game, "game_height", text("FaceAvatar.GameHeight"), -40, 40, 1);
	obs_property_int_set_suffix(p, "%");
	p = obs_properties_add_int_slider(game, "game_distance", text("FaceAvatar.GameDistance"), -40, 40, 1);
	obs_property_int_set_suffix(p, "%");
	obs_properties_add_group(props, "gaming_hands", text("FaceAvatar.Gaming"), OBS_GROUP_NORMAL, game);

	// Hand signs and face moves (which ones, and what they do, are picked
	// in DireBot).
	obs_properties_t *signs = obs_properties_create();
	obs_properties_add_text(signs, "signs_about", text("FaceAvatar.SignsAbout"), OBS_TEXT_INFO);
	obs_properties_add_bool(signs, "hand_signs", text("FaceAvatar.HandSigns"));
	obs_properties_add_bool(signs, "signs_hidden", text("FaceAvatar.SignsHidden"));
	obs_properties_add_group(props, "signs_group", text("FaceAvatar.Signs"), OBS_GROUP_NORMAL, signs);

	// The mouth from your mic.
	obs_properties_t *heard = obs_properties_create();
	obs_properties_add_text(heard, "mic_about", text("FaceAvatar.MicAbout"), OBS_TEXT_INFO);
	obs_property_t *from = obs_properties_add_list(heard, "mouth_from", text("FaceAvatar.MouthFrom"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(from, text("FaceAvatar.MouthFromBoth"), "both");
	obs_property_list_add_string(from, text("FaceAvatar.MouthFromMic"), "mic");
	obs_property_list_add_string(from, text("FaceAvatar.MouthFromCamera"), "camera");
	obs_property_t *mics = obs_properties_add_list(heard, "mic", text("FaceAvatar.Mic"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(mics, text("FaceAvatar.MicDefault"), "");
	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			const char *name = obs_source_get_name(source);
			if (is_audio(source) && name && *name)
				obs_property_list_add_string(static_cast<obs_property_t *>(param), name, name);
			return true;
		},
		mics);
	if (f) {
		std::lock_guard<std::mutex> l(f->mic_lock);
		if (!f->mic_name.empty() && (!f->mic || obs_weak_source_expired(f->mic)))
			obs_property_list_add_string(mics, f->mic_name.c_str(), f->mic_name.c_str());
	}
	p = obs_properties_add_int_slider(heard, "mic_start", text("FaceAvatar.MicStart"), -90, -6, 1);
	obs_property_int_set_suffix(p, " dB");
	p = obs_properties_add_int_slider(heard, "mic_full", text("FaceAvatar.MicFull"), -60, 0, 1);
	obs_property_int_set_suffix(p, " dB");
	p = obs_properties_add_int_slider(heard, "mic_amount", text("FaceAvatar.MicAmount"), 0, 200, 5);
	obs_property_int_set_suffix(p, "%");
	obs_properties_add_group(props, "mic_mouth", text("FaceAvatar.MicMouth"), OBS_GROUP_NORMAL, heard);
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

// ---- The throws filter (0.9.0) ---------------------------------------------

const char *filter_name(void *)
{
	return text("ThrowsFilter.Name");
}

// Its settings as the face avatar it runs on reads them: only what's
// thrown (DireBot's react_* settings) and how it jolts the camera - never
// a character, a phone, a tracker app, your arms, hand signs, gaming
// hands or a mic, whatever's in them.
obs_data_t *filter_settings(obs_data_t *settings)
{
	obs_data_t *safe = obs_data_create();
	obs_data_apply(safe, settings);
	for (const char *key : {"source", "character", "character_left", "character_right", "model", "own_face",
				"outfit", "mic", "phone_address", "signs", "react_moves", "spots", "drawn_places"})
		obs_data_set_string(safe, key, "");
	obs_data_set_string(safe, "face_from", "camera");
	obs_data_set_string(safe, "mouth_from", "camera");
	for (const char *key : {"vmc_body", "vmc_arms", "follow_arms", "arms_hands", "game_hands", "game_props",
				"hand_signs", "signs_hidden", "worn", "mirror"})
		obs_data_set_bool(safe, key, false);
	return safe;
}

void *filter_create(obs_data_t *settings, obs_source_t *source)
{
	auto *f = new FaceAvatar(source, true);
	update_filter(f, settings);
	return f;
}

void filter_update(void *data, obs_data_t *settings)
{
	update_filter(static_cast<FaceAvatar *>(data), settings);
}

void filter_defaults(obs_data_t *settings)
{
	get_defaults(settings);
	obs_data_set_default_bool(settings, "camera_shake", true);
}

obs_properties_t *filter_properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "about", text("ThrowsFilter.About"), OBS_TEXT_INFO);
	obs_properties_add_bool(props, "camera_shake", text("ThrowsFilter.Shake"));
	return props;
}

void filter_tick(void *data, float seconds)
{
	auto *f = static_cast<FaceAvatar *>(data);
	f->tick(seconds);
	f->pick_while_hidden(seconds);
}

void filter_render(void *data, gs_effect_t *)
{
	static_cast<FaceAvatar *>(data)->render_filter();
}

void video_tick(void *data, float seconds)
{
	auto *f = static_cast<FaceAvatar *>(data);
	f->tick(seconds);
	f->pick_while_hidden(seconds);
	f->look_while_hidden();
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
	// 0.9.0: {"source": your camera, "filter": its throws filter's name}.
	const json filter = data.contains("filter") ? data["filter"] : json();
	if (filter.is_string()) {
		obs_source_t *f = s ? obs_source_get_filter_by_name(s, filter.get<std::string>().c_str()) : nullptr;
		obs_source_release(s);
		if (f && std::strcmp(obs_source_get_unversioned_id(f), THROWS_FILTER_ID) != 0) {
			obs_source_release(f);
			f = nullptr;
		}
		return f;
	}
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
			showing = f->showing_now || f->looking_hidden_state;
		}
		const PhoneLink::State ps = f->phone.state();
		if (!st.ready)
			answer = fail(st.problem.empty()
					      ? "The face models are still loading - try again in a moment."
					      : "The face models couldn't be loaded - reinstall DireBot for OBS.");
		else if (f->from_phone && (!ps.packets || ps.since < 0))
			answer = fail("Your phone isn't sending your face yet - open its app and check the address in DireBot.");
		else if (f->from_phone && ps.since > 1)
			answer = fail("Your phone stopped sending your face - is its app still open, with the screen on?");
		else if (f->from_phone)
			f->worker.start_rest();
		else if (f->from_vmc && !f->vmc_face_sending())
			answer = fail(
				"Your tracker app isn't sending your face - check its VMC sending is on, to this PC's port in DireBot.");
		else if (f->from_vmc)
			f->worker.start_rest();
		else if (!showing)
			answer = fail("Show the face avatar in OBS first - it only looks for you while it's shown.");
		else
			f->worker.start_rest();
		answer["setting"] = answer["ok"].get<bool>();
		// (A tracker app's body: its hips' home is where they are now.)
		if (answer["setting"].get<bool>())
			f->vmc_rehome = true;
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
	if (f) // the page shows how the mic sounds: it listens a while
		f->listen_for(LISTEN_NS);
	json answer = f ? f->state(data.value("points", false)) : fail("It isn't ready yet.");
	obs_source_release(src);
	return answer;
}

// "Say each vowel": started, stopped, or the places it made forgotten.
json mic_calibrate(const json &data)
{
	obs_source_t *src = find_avatar(data);
	if (!src)
		return fail("That face avatar isn't in OBS any more.");
	auto *f = static_cast<FaceAvatar *>(obs_obj_get_data(src));
	json answer = {{"ok", true}};
	const std::string what = data.contains("do") && data["do"].is_string() ? data["do"].get<std::string>() : "start";
	if (!f) {
		answer = fail("It isn't ready yet.");
	} else if (what == "stop") {
		f->voice.stop_calibrating();
	} else if (what == "forget") {
		f->forget_voice();
	} else if (what == "start") {
		obs_source_t *m = f->get_mic();
		if (!m) {
			answer = fail(f->mouth_from == MOUTH_FROM_CAMERA
					      ? "Its mouth is from your camera - pick your mic or both first."
					      : "It can't find your mic in OBS - pick it first.");
		} else {
			f->listen_for(LISTEN_NS * 10);
			f->voice.calibrate();
		}
		obs_source_release(m);
	} else {
		answer = fail("DireBot for OBS doesn't know that.");
	}
	obs_source_release(src);
	return answer;
}

// "Follow my hands and arms": calibrate, stop, or forget it.
json arms_request(const json &data)
{
	obs_source_t *src = find_avatar(data);
	if (!src)
		return fail("That face avatar isn't in OBS any more.");
	auto *f = static_cast<FaceAvatar *>(obs_obj_get_data(src));
	const std::string what = data.contains("do") && data["do"].is_string() ? data["do"].get<std::string>()
										: "calibrate";
	json answer = f ? f->arms_to(what) : fail("It isn't ready yet.");
	obs_source_release(src);
	return answer;
}

// "Show me" gaming hands, or stop.
json game_request(const json &data)
{
	obs_source_t *src = find_avatar(data);
	if (!src)
		return fail("That face avatar isn't in OBS any more.");
	auto *f = static_cast<FaceAvatar *>(obs_obj_get_data(src));
	const std::string what = data.contains("do") && data["do"].is_string() ? data["do"].get<std::string>()
										: "show";
	json answer = f ? f->game_to(what) : fail("It isn't ready yet.");
	obs_source_release(src);
	return answer;
}

json react_on_cue(const json &data)
{
	obs_source_t *src = find_avatar(data);
	if (!src)
		return fail("That face avatar isn't in OBS any more.");
	auto *f = static_cast<FaceAvatar *>(obs_obj_get_data(src));
	json answer = f ? f->react_to(data) : fail("It isn't ready yet.");
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
	static const std::vector<std::string> list = {"FaceSet",   "FaceGetState", "FaceModelFront", "FaceMicCalibrate",
						      "FaceReact", "FaceArms",	   "FaceGame"};
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
		if (type == "FaceMicCalibrate")
			return mic_calibrate(data);
		if (type == "FaceReact")
			return react_on_cue(data);
		if (type == "FaceArms")
			return arms_request(data);
		if (type == "FaceGame")
			return game_request(data);
		return fail("DireBot for OBS doesn't know that request.");
	});
}

void set_emit(Emit emit)
{
	std::lock_guard<std::mutex> l(emit_lock);
	emit_now = std::move(emit);
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

	// 0.9.0: "DireBot Throws", on your camera.
	obs_source_info filter = {};
	filter.id = THROWS_FILTER_ID;
	filter.type = OBS_SOURCE_TYPE_FILTER;
	filter.output_flags = OBS_SOURCE_VIDEO;
	filter.get_name = filter_name;
	filter.create = filter_create;
	filter.destroy = destroy_filter;
	filter.update = filter_update;
	filter.get_defaults = filter_defaults;
	filter.get_properties = filter_properties;
	filter.video_tick = filter_tick;
	filter.video_render = filter_render;
	obs_register_source(&filter);
}

} // namespace direbot::face
