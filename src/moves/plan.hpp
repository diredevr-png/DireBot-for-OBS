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

// The maths of "DireBot Move" (move-transition.hpp): where each source is
// in the old and the new scene, which ones are in both, and where each is
// part way through. Plain maths, no OBS: the unit tests check it.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace direbot::moves {

// Where a source sits in a scene: its middle, how it's turned (radians),
// how big it shows (a negative size is flipped), how much is cropped off
// each side (in the source's own pixels), and how see-through.
struct Pose {
	float cx = 0, cy = 0;
	float rot = 0;
	float w = 0, h = 0;
	float crop_l = 0, crop_t = 0, crop_r = 0, crop_b = 0;
	float alpha = 1;
};

// A scene item as OBS draws it: its draw transform's two axes and corner
// (x = (ax, ay), y = (bx, by), corner (tx, ty)), for a picture the size
// of the source after cropping.
struct Drawn {
	float ax, ay, bx, by, tx, ty;
	float src_w, src_h;
	float crop_l, crop_t, crop_r, crop_b;
};

Pose pose_of(const Drawn &d);

// The other way: the axes and corner to draw a pose with.
void matrix_of(const Pose &p, float src_w, float src_h, float &ax, float &ay, float &bx, float &by, float &tx,
	       float &ty);

// One item of a scene, for matching: `key` is the source (the same source
// in both scenes moves; anything else comes or goes).
struct Item {
	uintptr_t key;
	Pose pose;
	float src_w, src_h;
};

enum class Kind { Move, Come, Go };

struct Step {
	Kind kind;
	uintptr_t key;
	float src_w, src_h;
	Pose from, to;
};

// In the order to draw them: what's going (in the old scene's order),
// then the new scene's items in its order, each moving from where it was
// or coming in. `canvas_w/h` and `come` ("fade", "slide", "grow") say how
// new ones come in and old ones go.
std::vector<Step> plan(const std::vector<Item> &before, const std::vector<Item> &after, const std::string &come,
		       float canvas_w, float canvas_h);

// 0..1 through, eased: "linear", "smooth", "ease_out", "ease_in", "bounce".
float ease(const std::string &how, float t);

Pose between(const Pose &a, const Pose &b, float t);

} // namespace direbot::moves
