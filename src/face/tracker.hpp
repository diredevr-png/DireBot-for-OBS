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

// Your face in the camera's picture, for the face avatar: 478 points on
// it, 52 expressions (how open your mouth is, each eye's blink, a smile,
// a raised brow...) and which way your head is turned.
//
// Google's MediaPipe face models do the seeing (data/face, Apache 2.0),
// run by our own engine (nn/net.hpp), with the steps around them done
// as MediaPipe's Face Landmarker does them, so the answers are theirs:
//
//   1. Find a face: the whole picture, shrunk to 128 x 128, through the
//      face finder. It follows the biggest face it finds.
//   2. The 478 points: a square around the face, turned so the eyes are
//      level, cut out at 256 x 256, through the face mesh model - which
//      also says how sure it is there's a face there at all.
//   3. Next look: the square comes from where the points were, so the
//      finder only runs again once the face is lost.
//   4. Smoothing (a One Euro filter on each point): steady while you're
//      still, straight there when you move. How much is a slider.
//   5. The 52 expressions: 146 of the points through the expressions
//      model.
//   6. The head's turn and place in 3D (pose.hpp).
//
// "Set": look straight at the camera with a resting face, and that look
// is kept, so the head's turn and the expressions after it are against
// how you sit and how your face rests (a beard, a camera off to one side).
//
// Plain code over plain numbers, so it's tested without OBS
// (tests/unit_face.cpp).

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "pose.hpp"

namespace direbot::face {

constexpr int POINTS = 478;
constexpr int EXPRESSIONS = 52;
// MediaPipe's names for the expressions (Apple's ARKit ones, and
// "_neutral" first).
extern const char *const EXPRESSION_NAMES[EXPRESSIONS];

// Some points by number: the eyes' middles (the irises) and the lips.
// "Right" and "left" are the person's own: the right eye is on the
// picture's left in a camera that doesn't mirror.
enum PointId {
	RIGHT_IRIS = 468,
	LEFT_IRIS = 473,
	RIGHT_EYE_OUTER = 33,
	RIGHT_EYE_INNER = 133,
	LEFT_EYE_INNER = 362,
	LEFT_EYE_OUTER = 263,
	MOUTH_RIGHT = 61,
	MOUTH_LEFT = 291,
	UPPER_LIP_TOP = 0,
	LOWER_LIP_BOTTOM = 17,
	NOSE_TIP = 1,
};

// A picture from the camera: 4 bytes a pixel, row after row.
struct Frame {
	const uint8_t *px = nullptr;
	int w = 0, h = 0;
	int stride = 0;    // bytes from one row to the next
	bool bgra = false; // blue, green, red (else red first); the 4th is unused
};

struct Point {
	float x = 0, y = 0, z = 0;
};

// What one look saw.
struct Face {
	bool found = false;
	// The picture's size (the points are in its pixels).
	int w = 0, h = 0;
	// How sure the face model is (0 to 1); found means at least half.
	float presence = 0;
	// In the picture's pixels (0, 0 the top left pixel's middle); z: how
	// far from the camera, against the face's middle, on x's scale.
	Point points[POINTS];
	// 0 to 1, as the model says...
	float expressions[EXPRESSIONS] = {};
	// ...and against your resting face from "Set" (the same until then):
	// 0 at rest, 1 all the way.
	float moves[EXPRESSIONS] = {};
	// The head in 3D (pose.hpp), and its turn since "Set" (or since
	// looking straight at the camera, until then).
	float pose[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	Turn turn;
	// This look ran the face finder (no face was being followed).
	bool searched = false;
	// How long the look took, in milliseconds.
	float ms = 0;
};

// "Set": your face at rest, looking at the camera.
struct Rest {
	bool set = false;
	float pose[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	float expressions[EXPRESSIONS] = {};
	// Where the face was (the middle of the eyes and mouth, shares of the
	// picture), from eye to eye (a share of its width) and the eyes' tilt
	// (radians, clockwise as the picture shows it).
	float x = 0.5f, y = 0.5f, gap = 0, angle = 0;
};

// The eyes and mouth as boxes in the picture, for the 2D avatar's
// cut-outs: middles and sizes as shares of the picture (w of its width,
// h of its height), turned with the eyes' tilt.
enum PartId { FIRST_EYE = 0, SECOND_EYE = 1, MOUTH = 2, PARTS = 3 };
struct Part {
	float x = 0, y = 0, w = 0, h = 0;
};
struct Layout {
	// The first eye is the one on the picture's left.
	Part parts[PARTS];
	float cx = 0.5f, cy = 0.5f; // the three parts' middle
	float gap = 0;              // eye to eye, a share of the width
	float angle = 0;            // radians, clockwise as the picture shows it
};
Layout layout_of(const Face &face);

class Tracker {
public:
	Tracker();
	~Tracker();
	Tracker(const Tracker &) = delete;
	Tracker &operator=(const Tracker &) = delete;

	// Google's face_landmarker.task (a zip of the three models and the
	// face at rest), from memory or a file. False, with why, if it can't.
	bool load(const uint8_t *bytes, size_t size, std::string *problem = nullptr);
	bool load_file(const std::string &path, std::string *problem = nullptr);
	bool ready() const;

	// Looks at a new picture. `seconds`: when the camera took it (any
	// clock that only goes forward), for the smoothing.
	const Face &look(const Frame &picture, double seconds);
	const Face &face() const;

	// The next look starts with the face finder (a new camera, a while
	// without looking). "Set" is kept.
	void forget();

	// Smoothing: 0 the steadiest to 1 the snappiest; 0.5 is MediaPipe's
	// own. Below 0: none at all (for tests).
	void set_smoothing(float amount);

	// "Set": the next few looks that find the face are your resting face,
	// looking at the camera. setting() is true until it has them.
	void set_rest();
	bool setting() const;
	bool has_rest() const;
	void clear_rest();
	const Rest &rest() const;
	// "Set" as text, to keep in the source's settings, and back.
	std::string save_rest() const;
	bool load_rest(const std::string &text);

	// The face at rest in 3D and its triangles (for later).
	const Geometry &geometry() const;

	// For tests: the models step by step on the plain C++ code (net.hpp;
	// set before load), and the face finder's answers on the last look
	// that ran it (picture pixels).
	void set_plain(bool plain);
	struct Found {
		float x = 0, y = 0, w = 0, h = 0; // its box
		float score = 0;
		float keys[12] = {}; // 6 points: eyes, nose, mouth, ears (x, y)
	};
	const std::vector<Found> &found() const;
	// The square the next look will cut out (picture pixels, radians).
	struct Square {
		float cx = 0, cy = 0, w = 0, h = 0, angle = 0;
	};
	bool following(Square *square = nullptr) const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// What the 2D avatar shows, look by look: the eyes and mouth, the face's
// move, size and tilt against "Set", which way you're turned, and how much
// your real eyes and mouth show. When the face is lost they're held as
// they were for a moment (the avatar keeps the last picture of them, so
// it never shows your room), then faded to the avatar's own drawn face;
// once it's found again they're back quickly.
class Shown {
public:
	static constexpr float HOLD = 0.3f, FADE = 0.25f, BACK = 0.15f;
	// Turned pictures: past this many degrees, and back under the other.
	static constexpr float TURN_ON = 20, TURN_OFF = 12;

	struct Out {
		Layout layout;
		float scale = 1;                               // eye to eye, against "Set"
		float home_x = 0.5f, home_y = 0.5f, angle = 0; // tilt against "Set"
		float shown = 0;                               // the real eyes and mouth: 0 hidden to 1 shown
		bool holding = false;                          // shown, but from the last picture they were seen in
		bool found = false;
		int turned = 0; // -1 towards the picture's left, 1 its right
		Turn turn;
		float moves[EXPRESSIONS] = {};
	};
	// `face`: the newest look; `new_look`: it's a new one, not the last
	// one again; `dt`: seconds since the last step.
	Out step(const Face &face, const Rest &rest, float dt, bool new_look = true);
	Out now() const { return out_; }
	void reset();

private:
	Out out_;
	float lost_for_ = 0;
	bool seen_ = false;
};

} // namespace direbot::face
