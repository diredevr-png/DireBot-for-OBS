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

#include "tracker.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace direbot::follow {

namespace {

// A grey level changing more than this between two looks is movement,
// not the camera's own grain.
constexpr int MOVED = 16;
// How fast the trace of movement fades, each look (ten a second): about
// a second and a half to fade away.
constexpr float FADE = 0.85f;
// More than this share of the picture moving at once is the lights or
// the camera itself, not you.
constexpr float EVERYTHING = 0.4f;
// A column has to have this many times what the middle column has to
// count: the rest is the camera's grain.
constexpr float GRAIN = 1.5f;
// Less movement than this (a share of the picture) says nothing.
constexpr float TOO_LITTLE = 0.002f;
// Movement near the crop has to be at least this share of all of it...
constexpr float NEAR_ENOUGH = 0.35f;
// ...or, this many looks in a row, the crop goes to where it is.
constexpr int AWAY_LOOKS = 5;
// Your head: this share of the movement is above it.
constexpr float HEAD_SHARE = 0.15f;
// The fastest the crop pans, in shares of the picture a second.
constexpr float MAX_PAN = 0.5f;

float clamp01(float v)
{
	return std::min(1.0f, std::max(0.0f, v));
}

// The weighted middle of [lo, hi) of `mass`; `got` says how much is there.
float middle(const std::vector<float> &mass, int lo, int hi, float &got)
{
	lo = std::max(0, lo);
	hi = std::min((int)mass.size(), hi);
	double sum = 0, total = 0;
	for (int x = lo; x < hi; x++) {
		sum += (x + 0.5) * mass[x];
		total += mass[x];
	}
	got = (float)total;
	return total > 0 ? (float)(sum / total) : (lo + hi) / 2.0f;
}

} // namespace

Rect crop(int src_w, int src_h, int out_w, int out_h, float zoom, Spot spot)
{
	Rect r;
	if (src_w <= 0 || src_h <= 0 || out_w <= 0 || out_h <= 0)
		return r;
	const float shape = (float)out_w / (float)out_h;
	if ((float)src_w / (float)src_h > shape) {
		r.h = (float)src_h;
		r.w = r.h * shape;
	} else {
		r.w = (float)src_w;
		r.h = r.w / shape;
	}
	zoom = std::min(4.0f, std::max(1.0f, std::isfinite(zoom) ? zoom : 1.0f));
	r.w /= zoom;
	r.h /= zoom;
	r.x = clamp01(spot.x) * (float)src_w - r.w / 2;
	r.y = clamp01(spot.y) * (float)src_h - r.h * HEAD_AT;
	r.x = std::min((float)src_w - r.w, std::max(0.0f, r.x));
	r.y = std::min((float)src_h - r.h, std::max(0.0f, r.y));
	return r;
}

void Tracker::forget()
{
	w_ = h_ = 0;
	away_ = 0;
	spot_.seen = false;
}

Spot Tracker::update(const uint8_t *grey, int w, int h, float window)
{
	if (!grey || w < 8 || h < 8)
		return spot_;
	const size_t n = (size_t)w * (size_t)h;
	if (w != w_ || h != h_) {
		// The first look: nothing to compare with yet.
		w_ = w;
		h_ = h;
		prev_.assign(grey, grey + n);
		trace_.assign(n, 0.0f);
		weight_.assign(n, 0.0f);
		away_ = 0;
		spot_.seen = false;
		return spot_;
	}

	size_t moved = 0;
	for (size_t i = 0; i < n; i++) {
		const bool m = std::abs((int)grey[i] - (int)prev_[i]) > MOVED;
		trace_[i] = m ? 1.0f : trace_[i] * FADE;
		moved += m ? 1 : 0;
		prev_[i] = grey[i];
	}
	if ((float)moved > EVERYTHING * (float)n) {
		std::fill(trace_.begin(), trace_.end(), 0.0f);
		spot_.seen = false;
		return spot_;
	}

	// How much moved in each column, less what every column has (the
	// camera's grain in a dark room). The newest movement counts most, so
	// the trail you leave as you walk doesn't hold the crop behind you.
	for (size_t i = 0; i < n; i++) {
		float t = trace_[i] * trace_[i];
		t *= t;
		weight_[i] = t * t;
	}
	std::vector<float> &cols = cols_;
	cols.assign(w, 0.0f);
	for (int y = 0; y < h; y++) {
		const float *line = &weight_[(size_t)y * w];
		for (int x = 0; x < w; x++)
			cols[x] += line[x];
	}
	sorted_.assign(cols.begin(), cols.end());
	std::nth_element(sorted_.begin(), sorted_.begin() + w / 2, sorted_.end());
	const float grain = sorted_[w / 2] * GRAIN + 1.0f;
	float total = 0;
	for (float &c : cols) {
		c = std::max(0.0f, c - grain);
		total += c;
	}
	if (total < TOO_LITTLE * (float)n) {
		spot_.seen = false;
		return spot_;
	}

	// Look near where it is: the middle of the movement within the crop's
	// width, a few times over, so it settles on you.
	const int half = std::max(2, (int)std::lround(clamp01(window) * w / 2));
	float x = clamp01(spot_.x) * w, near = 0;
	for (int i = 0; i < 4; i++)
		x = middle(cols, (int)std::lround(x) - half, (int)std::lround(x) + half, near);
	if (near < NEAR_ENOUGH * total) {
		if (++away_ >= AWAY_LOOKS) {
			// It's all somewhere else now: go there, to the crop-wide
			// stretch with the most movement in it.
			int best = 0;
			float best_mass = -1, mass = 0;
			const int span = std::min(w, 2 * half);
			for (int i = 0; i < span; i++)
				mass += cols[i];
			for (int lo = 0;; lo++) {
				if (mass > best_mass) {
					best_mass = mass;
					best = lo;
				}
				if (lo + span >= w)
					break;
				mass += cols[lo + span] - cols[lo];
			}
			x = middle(cols, best, best + span, near);
			away_ = 0;
		} else {
			spot_.seen = false;
			return spot_;
		}
	} else {
		away_ = 0;
	}

	// Your head: near the top of the movement under the crop.
	const int lo = std::max(0, (int)std::lround(x) - half), hi = std::min(w, (int)std::lround(x) + half);
	std::vector<float> &rows = rows_;
	rows.assign(h, 0.0f);
	float rows_total = 0;
	for (int y = 0; y < h; y++) {
		const float *line = &trace_[(size_t)y * w];
		for (int c = lo; c < hi; c++)
			rows[y] += line[c];
		rows_total += rows[y];
	}
	float y = spot_.y * h, run = 0;
	if (rows_total > 0) {
		for (int r = 0; r < h; r++) {
			run += rows[r];
			if (run >= HEAD_SHARE * rows_total) {
				y = r + 0.5f;
				break;
			}
		}
	}
	spot_.x = clamp01(x / (float)w);
	spot_.y = clamp01(y / (float)h);
	spot_.seen = true;
	return spot_;
}

namespace {

void ease(float &now, float target, bool &moving, float k, float dt, float dead)
{
	const float d = target - now;
	if (!moving && std::fabs(d) > dead)
		moving = true;
	if (!moving)
		return;
	const float most = MAX_PAN * dt;
	now += std::min(most, std::max(-most, d * k));
	if (std::fabs(target - now) < dead * 0.2f)
		moving = false;
}

} // namespace

Spot Smoother::step(Spot target, float dt, float seconds, float dead)
{
	target.x = clamp01(target.x);
	target.y = clamp01(target.y);
	if (!started_) {
		now_ = target;
		started_ = true;
		moving_x_ = moving_y_ = false;
		return now_;
	}
	if (!(dt > 0))
		return now_;
	dt = std::min(dt, 0.5f);
	const float k = 1.0f - std::exp(-dt / std::max(0.05f, seconds / 3.0f));
	ease(now_.x, target.x, moving_x_, k, dt, dead);
	ease(now_.y, target.y, moving_y_, k, dt, dead);
	now_.seen = target.seen;
	return now_;
}

} // namespace direbot::follow
