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

// "Follow me": where you are in your camera's picture, so the tall picture
// can keep you in the middle - all of it worked out here, with no one
// else's code or model.
//
// It goes by movement: a small grey copy of the camera's picture, ten
// times a second, compared with the one before. Where things moved
// lately (a fading trace of it) is where you are; a still picture says
// nothing, and the crop stays where it was. Only movement near where it
// is now counts at first, so a pet or a screen behind you doesn't pull it
// away, unless that's the only thing moving for a while. The top of the
// movement is your head.
//
// Plain code over plain numbers, so it's tested without OBS
// (tests/unit_follow.cpp).

#pragma once

#include <cstdint>
#include <vector>

namespace direbot::follow {

// A spot in the picture, as shares of its width and height (0 to 1).
struct Spot {
	float x = 0.5f;
	float y = 0.3f;
	bool seen = false; // false: nothing moved, so where it was before
};

// The part of the picture to show: left, top, width, height in pixels.
struct Rect {
	float x = 0, y = 0, w = 0, h = 0;
};

// Where your head goes in the crop, from its top (a share of its height).
constexpr float HEAD_AT = 0.3f;

// The part of a src_w x src_h picture shown at out_w x out_h: as big as
// fits at zoom 1, `zoom` times closer above that, with `spot` (your head)
// in the middle across and HEAD_AT down - kept inside the picture.
Rect crop(int src_w, int src_h, int out_w, int out_h, float zoom, Spot spot);

class Tracker {
public:
	// Pictures this size are plenty, and cost next to nothing.
	static constexpr int WIDTH = 128;

	// A grey picture, w x h, row after row. `window`: how wide the crop
	// is, as a share of the picture's width. Returns where you are.
	Spot update(const uint8_t *grey, int w, int h, float window);
	Spot spot() const { return spot_; }
	void reset();

private:
	std::vector<uint8_t> prev_;
	std::vector<float> trace_; // where it moved lately, fading
	std::vector<float> weight_; // the trace, the newest counting most
	int w_ = 0, h_ = 0;
	Spot spot_;
	int away_ = 0; // updates in a row with the movement elsewhere
};

// The crop's centre, easing towards where you are instead of jumping:
// small moves (talking, leaning) don't move it at all, and it never
// swings faster than a comfortable pan.
class Smoother {
public:
	// `seconds`: about how long it takes to catch up. `dead`: moves
	// smaller than this (a share of the picture) are let be.
	Spot step(Spot target, float dt, float seconds, float dead = 0.06f);
	Spot now() const { return now_; }
	void reset() { started_ = false; }

private:
	Spot now_;
	bool started_ = false;
	bool moving_x_ = false, moving_y_ = false;
};

} // namespace direbot::follow
