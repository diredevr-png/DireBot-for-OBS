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

// Where your eyes and mouth are in the camera's picture, for the face
// avatar - all of it worked out here, with no one else's code or model.
//
// Set up once: on a still from the camera you put a box over each eye
// and your mouth, and how those three look is kept. After that, on a
// small grey copy of each picture, it looks for each one again near
// where it was (how alike two patches are, whatever the light: zero-mean
// normalised cross-correlation), then fits one move, size and tilt to
// all three so they stay in shape as one face. A part that doesn't agree
// with the other two isn't believed. It learns slowly while it's sure
// (the light changes over a stream), and when it's lost it looks over
// the whole picture every few looks until the three agree again.
//
// It would rather say "not sure" than be wrong: the avatar then shows
// its own drawn eyes and mouth, never a piece of your room.
//
// Plain code over plain numbers, so it's tested without OBS
// (tests/unit_face.cpp).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace direbot::face {

// The parts, in the picture as the camera sees it (not mirrored).
enum PartId { FIRST_EYE = 0, SECOND_EYE = 1, MOUTH = 2, PARTS = 3 };

// A box: left, top, width, height as shares of the picture (0 to 1).
struct Box {
	float x = 0, y = 0, w = 0, h = 0;
};

// Sure enough to show your real eyes and mouth.
constexpr float SURE = 0.45f;

// What it sees now.
struct Face {
	bool found = false;
	// 0 (a guess) to 1 (sure). Only meaningful while found.
	float confidence = 0;
	// Each part's centre, shares of the picture.
	float x[PARTS] = {0, 0, 0}, y[PARTS] = {0, 0, 0};
	// The face as a whole, against the set-up still: the three parts'
	// middle (shares of the picture), how much bigger (1: the same), and
	// its tilt in radians (positive: clockwise on screen).
	float cx = 0.5f, cy = 0.5f, scale = 1, angle = 0;
	// Head turned: -1 towards the picture's left, 1 its right, 0 facing.
	int turned = 0;
};

class Tracker {
public:
	// Wide enough for an eye at a normal distance to be ~20 pixels.
	static constexpr int WIDTH = 400;

	// Keeps how the three parts look, from a grey picture (w x h, row
	// after row). False (and nothing changes) for boxes that are too
	// small, too plain to find again, or off the picture.
	bool set_up(const uint8_t *grey, int w, int h, const Box parts[PARTS], std::string *problem = nullptr);
	bool ready() const { return ready_; }
	// Where the face was in the set-up still (the middle of the three
	// parts, shares of the picture): the avatar's "home".
	float home_x() const { return ready_ ? (ref_[0].cx + ref_[1].cx + ref_[2].cx) / 3 / setup_w_ : 0.5f; }
	float home_y() const { return ready_ ? (ref_[0].cy + ref_[1].cy + ref_[2].cy) / 3 / setup_h_ : 0.5f; }

	// The set-up as text, to keep in the source's settings, and back.
	std::string save() const;
	bool load(const std::string &text);

	// Looks for the face in a new picture.
	const Face &update(const uint8_t *grey, int w, int h);
	const Face &face() const { return face_; }

	// Lost: the next look starts from nothing (a new camera size, or a
	// while without looking). The set-up is kept.
	void forget();

	// For tests and the state: looks since it was last found.
	int misses() const { return misses_; }

	struct Patch {
		int w = 0, h = 0;
		std::vector<float> px;
	};

private:
	struct Ref {
		Patch original, learned;
		float cx = 0, cy = 0; // centre, set-up pixels
	};
	struct Hit {
		float x = 0, y = 0, z = -1;
	};

	// set_up's boxes, already grown (and load's).
	bool take(const uint8_t *grey, int w, int h, const Box parts[PARTS], std::string *problem = nullptr);
	Hit search(int part, const uint8_t *grey, int w, int h, float px, float py, float radius, float scale,
		   float angle, int step) const;
	bool fit(const Hit hits[PARTS], const bool use[PARTS], float &s, float &a, float &tx, float &ty) const;
	// The three parts against the set-up's shape: how much bigger across
	// (eye to eye) and down (to the mouth), the tilt, and the lean (the
	// mouth off to one side, against the size across).
	struct Shape {
		float across = 1, down = 1, angle = 0, lean = 0;
	};
	bool stretch(const Hit hits[PARTS], Shape &out) const;
	bool accept(const Hit hits[PARTS], const uint8_t *grey, int w, int h, float strict, float shape);
	void learn(const uint8_t *grey, int w, int h);
	void find_anywhere(const uint8_t *grey, int w, int h);

	bool ready_ = false;
	int setup_w_ = 0, setup_h_ = 0;
	Ref ref_[PARTS];
	float eye_gap_ = 0; // set-up pixels
	float ref_ratio_ = 0; // eye gap against eyes-to-mouth, set up
	Face face_;
	int misses_ = 0;
	int looks_lost_ = 0;
	int unsure_ = 0; // looks in a row found, but not surely
	int turned_for_ = 0; // looks in a row the turn reading has held
	int turned_seen_ = 0;
	// Where it was last (picture pixels of the current size), for the
	// next look to start from.
	bool have_last_ = false;
	float last_x_[PARTS] = {0, 0, 0}, last_y_[PARTS] = {0, 0, 0};
	float last_scale_ = 1, last_angle_ = 0;
	int last_w_ = 0, last_h_ = 0;
	std::vector<uint8_t> half_; // kept, so a look over everything doesn't allocate
};

// The pose eased over time, so the avatar doesn't shake, and how much the
// cut-outs show: held still a moment when tracking goes unsure, then
// faded to the avatar's own drawn face.
class Smoother {
public:
	struct Out {
		float cx = 0.5f, cy = 0.5f, scale = 1, angle = 0;
		float x[PARTS] = {0, 0, 0}, y[PARTS] = {0, 0, 0};
		float shown = 0; // the cut-outs: 0 hidden to 1 shown
		int turned = 0;
	};
	// `seconds`: about how long it takes to catch up. Once faded out, the
	// cut-outs only come back after a few sure looks in a row: one lucky
	// match on the wallpaper never shows.
	static constexpr int SURE_LOOKS = 3;
	// `new_look`: the face is from a new look, not the last one again.
	Out step(const Face &face, float dt, float seconds, bool new_look = true);
	Out now() const { return out_; }
	void reset();

private:
	Out out_;
	bool started_ = false;
	float unsure_for_ = 0;
	int sure_looks_ = 0; // in a row
};

// The patch's pixels as text and back (for save/load).
std::string encode(const std::vector<uint8_t> &bytes);
bool decode(const std::string &text, std::vector<uint8_t> &bytes);

} // namespace direbot::face
