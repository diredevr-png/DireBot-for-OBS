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

#include "alive.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace direbot::face {

const char *const TUNE_NAMES[TUNE_GROUPS] = {"blink", "mouth", "smile", "round", "brows_up", "brows_down", "look"};

namespace {

constexpr float PI_F = 3.14159265f;

float clamp01(float v)
{
	return std::min(1.0f, std::max(0.0f, v));
}
float ramp(float v, const TuneRange &r)
{
	const float from = clamp01(r.from);
	const float to = std::max(clamp01(r.to), from + 0.02f);
	return clamp01((v - from) / (to - from));
}
float smooth(float t)
{
	t = clamp01(t);
	return t * t * (3 - 2 * t);
}
float mix(float a, float b, float t)
{
	return a + (b - a) * t;
}

int id(const char *name)
{
	for (int i = 0; i < EXPRESSIONS; i++)
		if (std::strcmp(EXPRESSION_NAMES[i], name) == 0)
			return i;
	return 0;
}

struct Ids {
	int blink_l = id("eyeBlinkLeft"), blink_r = id("eyeBlinkRight"), jaw = id("jawOpen"),
	    smile_l = id("mouthSmileLeft"), smile_r = id("mouthSmileRight"), funnel = id("mouthFunnel"),
	    pucker = id("mouthPucker"), inner_up = id("browInnerUp"), outer_up_l = id("browOuterUpLeft"),
	    outer_up_r = id("browOuterUpRight"), down_l = id("browDownLeft"), down_r = id("browDownRight");
	int looks[8] = {id("eyeLookUpLeft"),  id("eyeLookUpRight"), id("eyeLookDownLeft"), id("eyeLookDownRight"),
			id("eyeLookInLeft"),  id("eyeLookInRight"), id("eyeLookOutLeft"),  id("eyeLookOutRight")};
};
const Ids &ids()
{
	static const Ids i;
	return i;
}

} // namespace

int tune_default(int group, bool to)
{
	if (group < 0 || group >= TUNE_GROUPS)
		return to ? 100 : 0;
	const FaceTune t;
	return (int)std::lround((to ? t.range[group].to : t.range[group].from) * 100);
}

void tune_readings(const float mv[EXPRESSIONS], const FaceTune &t, float out[TUNE_GROUPS])
{
	const Ids &i = ids();
	out[TUNE_BLINK] = t.linked_blink ? (mv[i.blink_l] + mv[i.blink_r]) / 2 : std::max(mv[i.blink_l], mv[i.blink_r]);
	out[TUNE_MOUTH] = mv[i.jaw];
	out[TUNE_SMILE] = (mv[i.smile_l] + mv[i.smile_r]) / 2;
	out[TUNE_ROUND] = std::max(mv[i.funnel], mv[i.pucker]);
	out[TUNE_BROWS_UP] = std::max(mv[i.inner_up], (mv[i.outer_up_l] + mv[i.outer_up_r]) / 2);
	out[TUNE_BROWS_DOWN] = (mv[i.down_l] + mv[i.down_r]) / 2;
	float look = 0;
	for (int k : i.looks)
		look = std::max(look, mv[k]);
	out[TUNE_LOOK] = look;
	for (int g = 0; g < TUNE_GROUPS; g++)
		out[g] = clamp01(out[g]);
}

void tune_moves(const float in[EXPRESSIONS], const FaceTune &t, float out[EXPRESSIONS])
{
	const Ids &i = ids();
	if (out != in)
		std::copy(in, in + EXPRESSIONS, out);
	auto through = [&](int move, int group) {
		out[move] = ramp(in[move], t.range[group]);
	};
	// The eyes: in a smile they start shutting later (by up to most of
	// the way to full), so a big smile doesn't close them - a real blink
	// still does.
	TuneRange blink = t.range[TUNE_BLINK];
	if (t.open_when_smiling) {
		const float smile = ramp((in[i.smile_l] + in[i.smile_r]) / 2, t.range[TUNE_SMILE]);
		const float to = std::max(clamp01(blink.to), clamp01(blink.from) + 0.02f);
		blink.from = mix(clamp01(blink.from), to - 0.02f, 0.6f * smile);
	}
	if (t.linked_blink) {
		const float both = ramp((in[i.blink_l] + in[i.blink_r]) / 2, blink);
		out[i.blink_l] = out[i.blink_r] = both;
	} else {
		out[i.blink_l] = ramp(in[i.blink_l], blink);
		out[i.blink_r] = ramp(in[i.blink_r], blink);
	}
	through(i.jaw, TUNE_MOUTH);
	through(i.smile_l, TUNE_SMILE);
	through(i.smile_r, TUNE_SMILE);
	through(i.funnel, TUNE_ROUND);
	through(i.pucker, TUNE_ROUND);
	through(i.inner_up, TUNE_BROWS_UP);
	through(i.outer_up_l, TUNE_BROWS_UP);
	through(i.outer_up_r, TUNE_BROWS_UP);
	through(i.down_l, TUNE_BROWS_DOWN);
	through(i.down_r, TUNE_BROWS_DOWN);
	const float eyes = std::max(0.0f, std::isfinite(t.eye_move) ? t.eye_move : 1.0f);
	for (int k : i.looks)
		out[k] = clamp01(ramp(in[k], t.range[TUNE_LOOK]) * eyes);
}

void add_eye_turn(float moves[EXPRESSIONS], float yaw, float pitch)
{
	const Ids &i = ids();
	// looks: up L, up R, down L, down R, in L, in R, out L, out R.
	auto add = [moves](int k, float by) {
		if (k > 0)
			moves[k] = clamp01(moves[k] + by);
	};
	if (!std::isfinite(yaw) || !std::isfinite(pitch))
		return;
	const float side = std::fabs(yaw) / 30, up = std::fabs(pitch) / 25;
	// Towards its left: its left eye looks out, its right eye in.
	add(yaw > 0 ? i.looks[6] : i.looks[4], side);
	add(yaw > 0 ? i.looks[5] : i.looks[7], side);
	add(pitch > 0 ? i.looks[0] : i.looks[2], up);
	add(pitch > 0 ? i.looks[1] : i.looks[3], up);
}

Light light_of(const Frame &pic, const Face &face)
{
	Light l;
	if (!pic.px || pic.w <= 0 || pic.h <= 0 || pic.stride < pic.w * 4 || !face.found || face.w <= 0 ||
	    face.h <= 0)
		return l;
	// The face's box in the picture (its points may be on another scale).
	float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
	for (const Point &p : face.points) {
		x0 = std::min(x0, p.x);
		y0 = std::min(y0, p.y);
		x1 = std::max(x1, p.x);
		y1 = std::max(y1, p.y);
	}
	const float sx = (float)pic.w / face.w, sy = (float)pic.h / face.h;
	x0 *= sx, x1 *= sx, y0 *= sy, y1 *= sy;
	if (!(x1 > x0 + 4 && y1 > y0 + 4))
		return l;
	const int ri = pic.bgra ? 2 : 0, bi = pic.bgra ? 0 : 2;
	auto luma = [&](int x, int y) {
		const uint8_t *p = pic.px + (size_t)y * pic.stride + (size_t)x * 4;
		return (0.2126f * p[ri] + 0.7152f * p[1] + 0.0722f * p[bi]) / 255.0f;
	};
	// Its middle: in from the edges (hair, the background), brow to chin.
	const float fw = x1 - x0, fh = y1 - y0;
	const float fx0 = x0 + fw * 0.2f, fx1 = x1 - fw * 0.2f, fy0 = y0 + fh * 0.25f, fy1 = y1 - fh * 0.1f;
	constexpr int ACROSS = 24, DOWN = 24;
	double side[2] = {0, 0};
	int count[2] = {0, 0};
	for (int j = 0; j < DOWN; j++)
		for (int i = 0; i < ACROSS; i++) {
			const int x = (int)(fx0 + (fx1 - fx0) * (i + 0.5f) / ACROSS),
				  y = (int)(fy0 + (fy1 - fy0) * (j + 0.5f) / DOWN);
			if (x < 0 || y < 0 || x >= pic.w || y >= pic.h)
				continue;
			const int s = i < ACROSS / 2 ? 0 : 1;
			side[s] += luma(x, y);
			count[s]++;
		}
	if (!count[0] || !count[1])
		return l;
	// The room: the picture on a grid, less the face's whole box.
	constexpr int RX = 48, RY = 27;
	double room = 0;
	int rooms = 0;
	for (int j = 0; j < RY; j++)
		for (int i = 0; i < RX; i++) {
			const int x = (int)((i + 0.5f) * pic.w / RX), y = (int)((j + 0.5f) * pic.h / RY);
			if (x >= x0 && x <= x1 && y >= y0 && y <= y1)
				continue;
			room += luma(x, y);
			rooms++;
		}
	l.seen = true;
	l.left = (float)(side[0] / count[0]);
	l.right = (float)(side[1] / count[1]);
	l.face = (float)((side[0] + side[1]) / (count[0] + count[1]));
	l.room = rooms ? (float)(room / rooms) : l.face;
	return l;
}

Alive::Alive(uint32_t seed) : seed_(seed ? seed : 1), state_(seed ? seed : 1)
{
	reset();
}

void Alive::reset()
{
	state_ = seed_;
	time_ = 0;
	breath_at_ = 0;
	lost_for_ = 0;
	// Nothing seen yet: idle until it sees you.
	idle_ = 1;
	seen_ = false;
	held_yaw_ = held_pitch_ = held_roll_ = 0;
	std::fill(std::begin(held_), std::end(held_), 0.0f);
	eye_to_[0] = eye_to_[1] = eye_[0] = eye_[1] = 0;
	eye_next_ = 0;
	blink_ = 0;
	blink_t_ = -1;
	blink_wait_ = 1.5f;
	blinks_left_ = 0;
}

float Alive::random01()
{
	// xorshift32: the same moves from the same start (for tests).
	uint32_t x = state_;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	state_ = x;
	return (float)(x >> 8) / 16777216.0f;
}

void Alive::blink_step(float dt, bool on)
{
	// VMagicMirror's blink: shut in 0.06 s, held 0.04 s, open in 0.12 s.
	constexpr float SHUT = 0.06f, HELD = 0.04f, OPEN = 0.12f;
	if (blink_t_ >= 0) {
		blink_t_ += dt;
		if (blink_t_ < SHUT)
			blink_ = blink_t_ / SHUT;
		else if (blink_t_ < SHUT + HELD)
			blink_ = 1;
		else if (blink_t_ < SHUT + HELD + OPEN)
			blink_ = 1 - (blink_t_ - SHUT - HELD) / OPEN;
		else if (--blinks_left_ > 0)
			blink_t_ = 0, blink_ = 0;
		else
			blink_t_ = -1, blink_ = 0;
		return;
	}
	blink_ = 0;
	if (!on)
		return;
	blink_wait_ -= dt;
	if (blink_wait_ > 0)
		return;
	blink_t_ = 0;
	blinks_left_ = random01() < 0.2f ? 2 : 1;
	blink_wait_ = random(2.5f, 7.0f);
}

AliveOut Alive::step(float dt, bool found, float yaw, float pitch, float roll, const float moves[EXPRESSIONS])
{
	dt = std::isfinite(dt) ? std::min(0.25f, std::max(0.0f, dt)) : 0;
	AliveOut out;
	time_ += dt;
	const AliveOptions &o = options_;

	// A breath: in for half of it, out for the other half (sin squared).
	const float breath_time = std::min(20.0f, std::max(1.5f, std::isfinite(o.breath_time) ? o.breath_time : 5));
	breath_at_ = std::fmod(breath_at_ + dt / breath_time, 1.0);
	const float in = std::sin(PI_F * (float)breath_at_);
	out.breath = in * in * clamp01(o.breath);

	// Following you, or holding your last look, then idle.
	if (found) {
		seen_ = true;
		lost_for_ = 0;
		held_yaw_ = yaw;
		held_pitch_ = pitch;
		held_roll_ = roll;
		std::copy(moves, moves + EXPRESSIONS, held_);
		idle_ *= std::exp(-dt / BACK);
		if (idle_ < 1e-3f)
			idle_ = 0;
	} else if (seen_) {
		lost_for_ += dt;
		idle_ = std::max(idle_, smooth((lost_for_ - HOLD) / EASE));
	} else {
		idle_ = 1;
	}
	const float w = idle_;
	out.idle = w;

	// Its sway: slow waves that never quite repeat, timed from its breath
	// (as Live2D's own idle times its head and body from a breath of about
	// 3.2 seconds). A little while it follows you; a gentle look-about
	// when idle.
	const float k = breath_time / 3.2345f, T = (float)time_;
	auto wave = [T, k](float period, float phase) {
		return std::sin(2 * PI_F * T / (period * k) + phase);
	};
	const float sway = clamp01(o.sway) * 2;
	const float sy = 0.65f * wave(6.53f, 0) + 0.35f * wave(2.31f, 1.1f);
	const float sp = 0.6f * wave(3.53f, 0.7f) + 0.4f * wave(8.9f, 2.3f);
	const float sr = 0.6f * wave(5.53f, 1.9f) + 0.4f * wave(12.7f, 0.4f);
	out.yaw = mix(held_yaw_, 0, w);
	out.pitch = mix(held_pitch_, 0, w);
	out.roll = mix(held_roll_, 0, w);
	out.sway_yaw = sy * mix(0.8f, 7.0f, w) * sway;
	out.sway_pitch = sp * mix(0.6f, 3.5f, w) * sway;
	out.sway_roll = sr * mix(0.6f, 4.0f, w) * sway;
	out.body_yaw = wave(15.53f, 0.5f) * 1.2f * sway * (1 + w);
	out.body_roll = wave(11.3f, 2.0f) * 0.8f * sway * (1 + w);

	// Its eyes' small moves (VMagicMirror's: a new spot every 0.4 to 2
	// seconds, reached quickly), further when it's looking about - and
	// less far up (more of the eye's white shows).
	if (o.eye_moves) {
		const float rx = mix(1.5f, 8.0f, w), ry = mix(1.0f, 4.0f, w);
		eye_next_ -= dt;
		if (eye_next_ <= 0) {
			eye_next_ = random(0.4f, 2.0f);
			eye_to_[0] = random(-rx, rx);
			eye_to_[1] = random(-ry, ry * 0.5f);
		}
		// Back with you: a spot it picked looking about comes in.
		eye_to_[0] = std::min(rx, std::max(-rx, eye_to_[0]));
		eye_to_[1] = std::min(ry * 0.5f, std::max(-ry, eye_to_[1]));
	} else {
		eye_to_[0] = eye_to_[1] = 0;
	}
	const float reach = std::min(1.0f, 11 * dt);
	eye_[0] += (eye_to_[0] - eye_[0]) * reach;
	eye_[1] += (eye_to_[1] - eye_[1]) * reach;
	out.eye_yaw = eye_[0];
	out.eye_pitch = eye_[1];

	// Your face, easing to rest when idle; its own blinks when idle (or
	// always, if asked).
	for (int i = 0; i < EXPRESSIONS; i++)
		out.moves[i] = held_[i] * (1 - w);
	blink_step(dt, w > 0.5f || (found && o.auto_blink));
	const Ids &ids_ = ids();
	out.moves[ids_.blink_l] = std::max(out.moves[ids_.blink_l], blink_);
	out.moves[ids_.blink_r] = std::max(out.moves[ids_.blink_r], blink_);
	return out;
}

} // namespace direbot::face
