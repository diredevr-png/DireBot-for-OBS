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

#pragma once

#include <algorithm>

namespace direbot::face {

// When the camera's picture goes to the tracker. On each frame OBS draws,
// a copy of the camera's picture is asked for, and the one asked for the
// frame before is read back (the graphics card has it ready by then); it
// goes to the tracker if it's a new picture. A camera's picture stays up
// for a frame or more (a camera sends 30 a second, OBS often draws 60),
// and a look at the same picture again would stop the character for a
// look, then jump it two. A camera sending no more than `every` allows
// has each of its pictures looked at straight away; a faster one (a 60 a
// second camera, a scene with a game in it) every so often, evenly. A
// picture that doesn't change at all goes now and then all the same (a
// photo, a still camera), so "Set" and the like still see it.
struct LookPace {
	// Looks at most about this often (seconds); a picture the same as the
	// last look's, this many times less often.
	float every = 1.0f / 30;
	static constexpr float SAME_RARER = 3;
	// How long the count of new pictures a second is taken over (seconds).
	static constexpr float RATE_TIME = 0.5f;

	// A frame `dt` seconds long has begun.
	void frame(float dt)
	{
		since_ += dt;
		dt_ = dt;
	}
	// The picture read back this frame: does it go? `same`: the very
	// picture the last one that went had.
	bool give(bool same)
	{
		if (dt_ > 0) {
			const float k = std::min(1.0f, dt_ / RATE_TIME);
			rate_ += ((same ? 0.0f : 1 / dt_) - rate_) * k;
		}
		fast_ = fast_ ? rate_ > 1.1f / every : rate_ > 1.25f / every;
		// Within half a frame: a frame's length wobbles.
		const float soon = since_ + dt_ / 2;
		const bool go = same ? soon >= every * SAME_RARER : !fast_ || soon >= every;
		if (go)
			since_ = 0;
		return go;
	}
	// Start again: the next picture goes straight away.
	void reset()
	{
		since_ = 1;
		rate_ = 0;
		fast_ = false;
	}
	// New pictures a second, about; are they coming faster than `every`?
	float rate() const { return rate_; }
	bool fast() const { return fast_; }

private:
	float since_ = 1, dt_ = 0, rate_ = 0;
	bool fast_ = false;
};

} // namespace direbot::face
