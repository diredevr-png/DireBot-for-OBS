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

#include "gamehands.hpp"

#include <algorithm>
#include <cmath>

namespace direbot::face {

const char *const DESK_PROP_FILES[DESK_PROPS] = {"keyboard", "mouse", "mat", "pad", "stick", "stick"};

namespace {

constexpr float PI = 3.14159265358979f, DEG = PI / 180;

float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}
// A share `k` of the way there over `dt` seconds, settling in about `tau`.
float ease(float dt, float tau)
{
	return tau > 0 ? 1 - std::exp(-dt / tau) : 1.0f;
}
V3 mirror_x(V3 v)
{
	return {-v.x, v.y, v.z};
}
// A real hand, as REAL_ARM's arm has it: wrist to middle knuckle, first
// knuckle to little one, its middle finger.
constexpr float REAL_PALM = 0.098f, REAL_KNUCKLES = 0.063f, REAL_FINGER = 0.094f;
// How big the pad is (and the hands' hold on it), to fit its own hands:
// as big as its hands are for its arms - small hands hold a smaller one,
// big hands a bigger one.
float hand_scale(const DeskBody &b)
{
	if (!(b.arm > 0) || !std::isfinite(b.arm))
		return 1;
	float sum = 0;
	int n = 0;
	const float measure[3][2] = {{b.hand, REAL_PALM}, {b.knuckles, REAL_KNUCKLES}, {b.finger, REAL_FINGER}};
	for (const auto &m : measure)
		if (m[0] > 0 && std::isfinite(m[0])) {
			sum += m[0] / b.arm * REAL_ARM / m[1];
			n++;
		}
	return n ? clampf(sum / (float)n, 0.7f, 1.4f) : 1.0f;
}
// Its palm's length (wrist to middle knuckle) on the pad, in the pad's
// own metres.
float palm_on_pad(const DeskBody &b)
{
	const float k = b.arm > 0 ? b.arm / REAL_ARM : 0;
	if (!(b.hand > 0) || !(k > 0) || !std::isfinite(b.hand / k))
		return REAL_PALM;
	return clampf(b.hand / k / hand_scale(b), 0.6f * REAL_PALM, 1.5f * REAL_PALM);
}

// ---- Where everything is (metres, round the middle of its shoulders: x its
// left, y up, z out of its front) for a right-handed you it doesn't mirror:
// the keyboard under its left hand, the mouse under its right. Mirrored
// (or the mouse in your left hand) it's all the other way round.

// The keyboard's middle, on the desk (its own file's: x its left, y up,
// z away), and each hand's wrist as it types on it: the keyboard hand
// over its WASD, the mouse hand over its J and K when it types too - each
// wrist low, in front of the keyboard (a hand's as long as its fingers
// reach to the middle of the keys).
const V3 KEYBOARD_AT{0.06f, -0.33f, 0.36f};
const V3 KEYS_HOME{0.125f, 0.045f, -0.170f};
const V3 KEYS_OTHER{-0.07f, 0.045f, -0.170f};
// The mouse at the middle of its mat, and the mouse hand's wrist from the
// mouse's middle (as the mouse is turned).
const V3 MOUSE_AT{-0.27f, -0.33f, 0.31f};
const V3 MOUSE_WRIST{0.004f, 0.040f, -0.106f};
// How far the mouse goes on its mat (metres a count of yours: about as far
// as yours at 800 DPI, twice as far at 1600), how far it can go each way,
// and how slowly it drifts back to the middle.
constexpr double MOUSE_PER_COUNT = 0.00003;
constexpr float MOUSE_X = 0.06f, MOUSE_Z = 0.04f, MOUSE_HOME = 2.5f;
// The pad as it's held (its middle), its face tilted up towards it, and on
// it (its own: x its left, y its face's up, z away - in a real hand's
// metres, grown or shrunk with its hands): the left hand's middle knuckle
// (the right one's the other way round), each stick's root.
const V3 PAD_AT{0.0f, -0.23f, 0.27f};
constexpr float PAD_TILT = -35 * DEG;
const V3 PAD_KNUCKLE{0.073f, -0.027f, 0.018f}; // its middle knuckle (its wrist a palm back from it)
const V3 STICK_AT[2] = {{0.040f, 0.010f, 0.012f}, {-0.025f, 0.010f, -0.018f}};
// Each thumb's tip just over its stick's top (up the stick, as it leans)
// or its buttons (the d-pad's, the four's middles), pushed down a little
// when one's pressed; each first finger's on its trigger's front (in a
// little as it's squeezed) or along its bumper's top.
constexpr float STICK_TOP = 0.022f, PUSH_DOWN = 0.005f;
const V3 DPAD_TOP{0.025f, 0.026f, -0.018f}, FACE_TOP{-0.040f, 0.026f, 0.012f};
const V3 TRIGGER_FRONT{0.040f, -0.004f, 0.050f}, TRIGGER_IN{0.0f, -0.004f, -0.006f};
const V3 BUMPER_TOP{0.044f, 0.019f, 0.040f};
// Its middle, ring and little fingers' tips round under its grip, on the
// grip's inside (each a finger's thickness off it).
const V3 GRIP_TIPS[3] = {{0.020f, -0.036f, -0.015f}, {0.029f, -0.028f, -0.029f}, {0.034f, -0.032f, -0.043f}};
constexpr float STICK_LEAN = 20 * DEG;

// Each hand's ways, the left one's (the right's are these mirrored): on
// the keyboard its fingers forward, a little in and up from its wrist,
// its first finger's side a little up; on the mouse over its back; on the pad
// its palm against its grip from outside it (the grip across its palm, as
// a hand holds a handle), its fingers forward and a little down, its first
// finger's side up. And where its elbow points: down and out a little,
// back.
const V3 KEYS_ALONG{-0.17f, 0.18f, 1.0f}, KEYS_ACROSS{-1.0f, 0.35f, 0.06f};
const V3 MOUSE_ALONG{-0.06f, 0.13f, 1.0f}, MOUSE_ACROSS{-1.0f, 0.22f, 0.0f};
const V3 PAD_ALONG{-0.34f, -0.46f, 0.82f}, PAD_ACROSS{-0.10f, 0.89f, 0.45f};
const V3 KEYS_POLE{0.45f, -1.0f, -0.30f}, PAD_POLE{0.75f, -1.0f, -0.15f};

// Fingers (thumb, first, middle, ring, little; knuckle, middle, tip -
// radians): over the keys curved down onto them; on the mouse its first
// two flat on the buttons, the others tucked; round the pad's grip, the
// first on a trigger (straighter on a bumper), the thumb on a stick or the
// buttons - where each starts from, reaching its spot on the pad.
const float KEYS_BEND[5][3] = {
	{0.15f, 0.25f, 0.15f}, {0.20f, 0.45f, 0.30f}, {0.22f, 0.48f, 0.30f}, {0.25f, 0.50f, 0.30f}, {0.30f, 0.55f, 0.30f}};
const float KEY_PRESS[3] = {0.30f, 0.15f, 0.05f};
const float THUMB_PRESS[3] = {0.12f, 0.20f, 0.12f};
const float MOUSE_BEND[5][3] = {
	{0.25f, 0.30f, 0.20f}, {0.12f, 0.30f, 0.15f}, {0.15f, 0.32f, 0.18f}, {0.55f, 0.85f, 0.50f}, {0.60f, 0.90f, 0.50f}};
const float CLICK[3] = {0.16f, 0.08f, 0.04f};
const float PAD_GRIP[3][3] = {{1.50f, 1.20f, 1.05f}, {1.60f, 1.10f, 1.00f}, {1.70f, 0.75f, 0.90f}};
const float INDEX_TRIGGER[3] = {0.64f, 0.63f, 0.63f}, INDEX_SQUEEZE[3] = {0.25f, 0.35f, 0.25f};
const float INDEX_BUMPER[3] = {0.15f, 0.25f, 0.15f};
const float THUMB_STICK[3] = {0.97f, 0.25f, 0.15f}, THUMB_BUTTONS[3] = {0.40f, 0.45f, 0.35f};
const float THUMB_PUSH[3] = {0.05f, 0.15f, 0.20f};

// A key tapped shows its finger down at least this long (seconds).
constexpr double TAP = 0.09;

} // namespace

float GameHands::random()
{
	// xorshift32
	uint32_t x = seed_;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	seed_ = x;
	return (float)(x >> 8) / 16777216.0f;
}

void GameHands::reset()
{
	const GameOptions o = options_;
	const DeskBody b = body_;
	*this = GameHands();
	options_ = o;
	body_ = b;
}

// ---- What's been done since the last frame ----------------------------------

void GameHands::press_key(double now)
{
	const bool kflip = options_.mirror == options_.mouse_right;
	// Typing with both hands: either hand, as it happens (never by the key).
	const int canon = typing_ && random() < 0.5f ? 1 : 0;
	Hand &h = hand_[canon ^ (kflip ? 1 : 0)];
	// A finger not held already: the first and second most likely, now and
	// then the thumb (a space bar's).
	static const float WEIGHT[5] = {0.08f, 0.31f, 0.31f, 0.18f, 0.12f};
	float total = 0;
	for (int i = 0; i < 5; i++)
		if (!h.finger[i].down)
			total += WEIGHT[i];
	int pick = 1;
	if (total > 0) {
		float r = random() * total;
		for (int i = 0; i < 5; i++) {
			if (h.finger[i].down)
				continue;
			pick = i;
			r -= WEIGHT[i];
			if (r <= 0)
				break;
		}
	}
	Finger &f = h.finger[pick];
	f.down = true;
	f.since = now;
	f.until = now + TAP;
	h.presses++;
	// Its wrist dips with it, and now and then it wanders a little.
	h.speed.y -= 0.10f;
	if (h.presses % 4 == 0 || random() < 0.2f)
		h.drift_to = {(random() * 2 - 1) * 0.014f, 0, (random() * 2 - 1) * 0.010f};
	presses_at_[press_ring_ % 8] = now;
	press_ring_++;
}

void GameHands::take(const DeviceState &s, double now, float dt)
{
	const DeviceState &p = have_last_ ? last_ : s;
	const uint64_t presses = s.key_presses > p.key_presses ? s.key_presses - p.key_presses : 0;
	const uint64_t clicks = s.clicks > p.clicks ? s.clicks - p.clicks : 0;
	const double mx = s.mouse_x - p.mouse_x, my = s.mouse_y - p.mouse_y;
	const double wheel = s.wheel - p.wheel;
	const float moved = (float)std::sqrt(mx * mx + my * my);
	const bool mouse_used = moved > 2 || clicks > 0 || wheel != 0 || s.mouse_buttons != p.mouse_buttons;
	const bool keys_used = presses > 0 || s.keys_held != p.keys_held;
	const bool pad_used = have_last_ && s.pad.connected && s.pad_at > p.pad_at;
	if (!have_last_)
		any_at_ = now;
	// Which you're using: each claimed by what's done on it, fading.
	const float fade = std::exp(-dt / 0.7f);
	kbm_claim_ *= fade;
	pad_claim_ *= fade;
	kbm_claim_ += (float)std::min<uint64_t>(presses + clicks, 4) + (moved > 2 ? 0.35f : 0.0f) +
		      (wheel != 0 ? 1.0f : 0.0f);
	if (pad_used) {
		const uint32_t newly = s.pad.buttons & ~p.pad.buttons;
		int pressed = 0;
		for (uint32_t b = newly; b; b &= b - 1)
			pressed++;
		pad_claim_ += 0.35f + (float)pressed;
	}
	if (keys_used || mouse_used)
		kbm_at_ = any_at_ = now;
	if (keys_used)
		keys_at_ = now;
	if (pad_used)
		pad_at_ = any_at_ = now;
	if (mouse_used)
		mouse_used_at_ = now;
	if (options_.device == 1) {
		chosen_ = GameDevice::KeyboardMouse;
	} else if (options_.device == 2) {
		chosen_ = GameDevice::Pad;
	} else if (chosen_ != GameDevice::Pad && s.pad.connected && pad_claim_ >= CLAIM) {
		chosen_ = GameDevice::Pad;
		kbm_claim_ = 0;
	} else if (chosen_ == GameDevice::Pad && (kbm_claim_ >= CLAIM || !s.pad.connected)) {
		chosen_ = GameDevice::KeyboardMouse;
		pad_claim_ = 0;
	}
	const float to_pad = chosen_ == GameDevice::Pad ? 1.0f : 0.0f;
	if (!have_last_)
		share_pad_ = to_pad; // (starting: straight on the one it's using)
	share_pad_ = clampf(share_pad_ + clampf(to_pad - share_pad_, -dt / SWITCH, dt / SWITCH), 0, 1);
	share_kbm_ = 1 - share_pad_;
	// Resting after a while with none used, if asked.
	const bool on = !(options_.rest_after > 0 && now - any_at_ > options_.rest_after);
	on_ = clampf(on_ + clampf((on ? 1.0f : 0.0f) - on_, -dt / 0.6f, dt / 0.6f), 0, 1);

	// Keys: a finger down for each, and held while keys are (the oldest let
	// go of first as they come up).
	for (uint64_t i = 0; i < std::min<uint64_t>(presses, 6); i++)
		press_key(now);
	const int want_held = std::max(0, std::min(4, s.keys_held));
	for (;;) {
		int count = 0, oldest_side = -1, oldest = -1;
		for (int side = 0; side < 2; side++)
			for (int i = 0; i < 5; i++) {
				const Finger &f = hand_[side].finger[i];
				if (!f.down)
					continue;
				count++;
				if (oldest < 0 || f.since < hand_[oldest_side].finger[oldest].since) {
					oldest_side = side;
					oldest = i;
				}
			}
		if (count <= want_held || oldest < 0)
			break;
		hand_[oldest_side].finger[oldest].down = false;
	}
	// Typing away with the mouse still: both hands type, till the mouse
	// is used again (or a while with no typing).
	int recent = 0;
	double last_press = -1;
	for (double at : presses_at_) {
		if (at >= 0 && now - at < 1.2)
			recent++;
		last_press = std::max(last_press, at);
	}
	if (chosen_ != GameDevice::KeyboardMouse || mouse_used || (last_press >= 0 && now - last_press > 4))
		typing_ = false;
	else if (!typing_ && recent >= 4 && (mouse_used_at_ < 0 || now - mouse_used_at_ > 1.2))
		typing_ = true;

	// The mouse: as far as yours went, its way back to the middle slowly.
	const double home = std::exp(-dt / MOUSE_HOME);
	mouse_acc_x_ = mouse_acc_x_ * home + mx * MOUSE_PER_COUNT;
	mouse_acc_z_ = mouse_acc_z_ * home + my * MOUSE_PER_COUNT;
	// (Your right is its right: its -x. Towards you is towards it: -z.)
	const float was_x = mouse_x_;
	mouse_x_ = -MOUSE_X * (float)std::tanh(mouse_acc_x_ / MOUSE_X);
	mouse_z_ = -MOUSE_Z * (float)std::tanh(mouse_acc_z_ / MOUSE_Z);
	const float speed = dt > 0 ? (mouse_x_ - was_x) / dt : 0;
	const float turn_to = clampf(mouse_x_ * 2.5f - speed * 0.6f, -0.25f, 0.25f);
	mouse_turn_ += (turn_to - mouse_turn_) * ease(dt, 0.12f);
	if (wheel != 0)
		wheel_flick_ = 0.2f;
	wheel_flick_ = std::max(0.0f, wheel_flick_ - dt);

	// The pad: each thumb where it's being used, the pad bumped about by
	// what's pressed and moved, and a little lower while something's held.
	const PadState &q = s.pad;
	if (q.buttons & PAD_DPAD) {
		thumb_want_[0] = 1;
		thumb_at_[0] = now;
	} else if (std::hypot(q.left_x, q.left_y) > 0.15f || now - thumb_at_[0] > 0.6) {
		thumb_want_[0] = 0;
	}
	if (std::hypot(q.right_x, q.right_y) > 0.15f) {
		thumb_want_[1] = 0;
		thumb_at_[1] = now;
	} else if ((q.buttons & PAD_FACE) || now - thumb_at_[1] > 0.8) {
		thumb_want_[1] = 1;
	}
	for (int c = 0; c < 2; c++)
		thumb_on_[c] += (thumb_want_[c] - thumb_on_[c]) * ease(dt, 0.08f);
	if (pad_used) {
		const uint32_t newly = q.buttons & ~pad_seen_.buttons;
		int pressed = 0;
		for (uint32_t b = newly; b; b &= b - 1)
			pressed++;
		const float stick = std::fabs(q.left_x - pad_seen_.left_x) + std::fabs(q.left_y - pad_seen_.left_y) +
				    std::fabs(q.right_x - pad_seen_.right_x) + std::fabs(q.right_y - pad_seen_.right_y);
		pad_busy_ += 0.5f * (float)pressed + 0.6f * stick;
		if (pad_busy_ > 1.5f) {
			pad_busy_ = 0;
			pad_jitter_to_ = {(random() * 2 - 1) * 0.008f, (random() * 2 - 1) * 0.010f,
					  (random() * 2 - 1) * 0.008f};
			pad_turn_to_ = {(random() * 2 - 1) * 4 * DEG, (random() * 2 - 1) * 2 * DEG,
					(random() * 2 - 1) * 4 * DEG};
		}
	}
	pad_busy_ = std::max(0.0f, pad_busy_ - 0.4f * dt);
	if (pad_at_ < 0 || now - pad_at_ > 4) {
		pad_jitter_to_ = {};
		pad_turn_to_ = {};
	}
	pad_seen_ = q;

	const double used_lately = 1.0;
	now_.keys = keys_at_ >= 0 && now - keys_at_ < used_lately;
	now_.mouse = mouse_used_at_ >= 0 && now - mouse_used_at_ < used_lately;
	now_.pad = pad_at_ >= 0 && now - pad_at_ < used_lately;
	last_ = s;
	have_last_ = true;
}

// ---- Where each hand goes ----------------------------------------------------

GameHands::Target GameHands::keyboard_hand(int canon, double now) const
{
	const bool kflip = options_.mirror == options_.mouse_right;
	const Hand &h = hand_[canon ^ (kflip ? 1 : 0)];
	const V3 desk{0, options_.height * REAL_ARM, options_.distance * REAL_ARM};
	Target t;
	t.wrist = KEYBOARD_AT + desk + (canon == 0 ? KEYS_HOME : KEYS_OTHER) + h.drift;
	t.along = canon == 0 ? KEYS_ALONG : mirror_x(KEYS_ALONG);
	t.across = canon == 0 ? KEYS_ACROSS : mirror_x(KEYS_ACROSS);
	t.pole = canon == 0 ? KEYS_POLE : mirror_x(KEYS_POLE);
	bool any = false;
	for (int i = 0; i < 5; i++) {
		const Finger &f = h.finger[i];
		const bool pressed = f.down || now < f.until;
		any = any || pressed;
		for (int j = 0; j < 3; j++)
			t.bend[i][j] = KEYS_BEND[i][j] + (pressed ? (i == 0 ? THUMB_PRESS[j] : KEY_PRESS[j]) : 0.0f);
	}
	// (A little lower while a key's held.)
	if (any)
		t.wrist.y -= 0.004f;
	return t;
}

GameHands::Target GameHands::mouse_hand(double) const
{
	const V3 desk{0, options_.height * REAL_ARM, options_.distance * REAL_ARM};
	const Quat turn = axis_angle({0, 1, 0}, mouse_turn_);
	const V3 mouse = MOUSE_AT + desk + V3{mouse_x_, 0, mouse_z_};
	Target t;
	t.wrist = mouse + rotate(turn, mirror_x(MOUSE_WRIST));
	t.along = rotate(turn, mirror_x(MOUSE_ALONG));
	t.across = rotate(turn, mirror_x(MOUSE_ACROSS));
	t.pole = mirror_x(KEYS_POLE);
	const uint32_t b = last_.mouse_buttons;
	for (int i = 0; i < 5; i++)
		for (int j = 0; j < 3; j++)
			t.bend[i][j] = MOUSE_BEND[i][j];
	for (int j = 0; j < 3; j++) {
		if (b & 1)
			t.bend[1][j] += CLICK[j];
		if (b & 2)
			t.bend[2][j] += CLICK[j];
		if (b & 4) {
			t.bend[1][j] += CLICK[j] * 0.5f;
			t.bend[2][j] += CLICK[j] * 0.5f;
		}
		if (b & 24)
			t.bend[0][j] += THUMB_PRESS[j];
	}
	// The wheel: its first finger flicks back and over it.
	if (wheel_flick_ > 0)
		t.bend[1][0] -= 0.25f * std::sin(PI * (1 - wheel_flick_ / 0.2f));
	return t;
}

void GameHands::pad_targets(Target out[2], M4 &pad, M4 sticks[2], double, float dt)
{
	const PadState &p = pad_seen_;
	pad_clock_ += dt;
	const float tc = pad_clock_;
	// Swaying by itself, slowly; bumped about by what's pressed; lower while
	// something's held; leaning with the left stick.
	const V3 sway{0.004f * std::sin(tc * 2 * PI / 4.2f), 0.003f * std::sin(tc * 2 * PI / 3.1f + 1),
		      0.003f * std::sin(tc * 2 * PI / 5.3f + 2)};
	const float sway_pitch = 1.5f * DEG * std::sin(tc * 2 * PI / 4.7f),
		    sway_roll = 1.5f * DEG * std::sin(tc * 2 * PI / 3.9f + 0.5f);
	const float k = ease(dt, 0.25f);
	pad_jitter_ = pad_jitter_ + (pad_jitter_to_ - pad_jitter_) * k;
	pad_turn_ = pad_turn_ + (pad_turn_to_ - pad_turn_) * k;
	const bool held = p.buttons || p.left_trigger > 0.1f || p.right_trigger > 0.1f;
	pad_dip_ += ((held ? 1.0f : 0.0f) - pad_dip_) * ease(dt, 0.06f);
	const V3 desk{0, options_.height * REAL_ARM, options_.distance * REAL_ARM};
	const V3 at = PAD_AT + desk + pad_jitter_ + sway + V3{0, -0.007f * pad_dip_, 0};
	const float pitch = PAD_TILT + p.left_y * 4 * DEG + pad_turn_.x + sway_pitch;
	const float roll = p.left_x * 6 * DEG + pad_turn_.z + sway_roll;
	const Quat turn = axis_angle({0, 1, 0}, pad_turn_.y) * axis_angle({1, 0, 0}, pitch) * axis_angle({0, 0, 1}, roll);
	pad = moved(at) * matrix_of(turn) * scaled(hand_scale(body_));
	sticks[0] = pad * moved(STICK_AT[0]) *
		    matrix_of(axis_angle({1, 0, 0}, p.left_y * STICK_LEAN) * axis_angle({0, 0, 1}, p.left_x * STICK_LEAN));
	sticks[1] = pad * moved(STICK_AT[1]) * matrix_of(axis_angle({1, 0, 0}, p.right_y * STICK_LEAN) *
							  axis_angle({0, 0, 1}, p.right_x * STICK_LEAN));
	for (int c = 0; c < 2; c++) {
		const bool left = c == 0;
		Target &t = out[c];
		// Its thumb from the stick to the buttons moves its hand back a
		// little (the left's d-pad and the right's stick are lower down).
		const float back = left ? thumb_on_[0] : 1 - thumb_on_[1];
		const V3 knuckle = PAD_KNUCKLE - normalized(PAD_ALONG) * palm_on_pad(body_);
		V3 w = left ? knuckle : mirror_x(knuckle);
		w.z -= 0.012f * back;
		t.wrist = point(pad, w);
		t.along = rotate(turn, left ? PAD_ALONG : mirror_x(PAD_ALONG));
		t.across = rotate(turn, left ? PAD_ACROSS : mirror_x(PAD_ACROSS));
		t.pole = left ? PAD_POLE : mirror_x(PAD_POLE);
		const float on = left ? thumb_on_[0] : thumb_on_[1];
		const bool push = left ? (p.buttons & (PAD_DPAD | PAD_LEFT_STICK)) != 0
				       : (p.buttons & (PAD_FACE | PAD_RIGHT_STICK)) != 0;
		const bool bumper = (p.buttons & (left ? PAD_LEFT_BUMPER : PAD_RIGHT_BUMPER)) != 0;
		const float trig = left ? p.left_trigger : p.right_trigger;
		for (int j = 0; j < 3; j++) {
			t.bend[0][j] = THUMB_STICK[j] + (THUMB_BUTTONS[j] - THUMB_STICK[j]) * on + (push ? THUMB_PUSH[j] : 0);
			t.bend[1][j] = bumper ? INDEX_BUMPER[j] : INDEX_TRIGGER[j] + INDEX_SQUEEZE[j] * trig;
			for (int i = 2; i < 5; i++)
				t.bend[i][j] = PAD_GRIP[i - 2][j];
		}
		// Its thumb on its stick (up it, as it leans) or its buttons; its
		// first finger on its trigger or bumper; the others round its grip.
		const V3 stick = point(sticks[c], {0, STICK_TOP, 0});
		const V3 buttons = point(pad, left ? DPAD_TOP : FACE_TOP);
		t.reach[0] = stick + (buttons - stick) * on;
		if (push)
			t.reach[0].y -= PUSH_DOWN * hand_scale(body_);
		const V3 trigger = TRIGGER_FRONT + TRIGGER_IN * trig;
		t.reach[1] = point(pad, bumper ? (left ? BUMPER_TOP : mirror_x(BUMPER_TOP)) : (left ? trigger : mirror_x(trigger)));
		for (int i = 2; i < 5; i++)
			t.reach[i] = point(pad, left ? GRIP_TIPS[i - 2] : mirror_x(GRIP_TIPS[i - 2]));
		for (float &w : t.reach_weight)
			w = 1;
	}
}

void GameHands::glide(int side, const Target &t, float dt)
{
	Hand &h = hand_[side];
	if (!h.started) {
		h.at = t.wrist;
		h.speed = {};
		h.along = t.along;
		h.across = t.across;
		for (int i = 0; i < 5; i++)
			for (int j = 0; j < 3; j++)
				h.bend[i][j] = t.bend[i][j];
		for (int i = 0; i < 5; i++)
			h.reach_weight[i] = t.reach_weight[i];
		h.started = true;
	}
	// Its fingers reaching only once it's there.
	const float there = clampf(1 - (length(t.wrist - h.at) - 0.01f) / 0.04f, 0, 1);
	for (int i = 0; i < 5; i++) {
		h.reach[i] = t.reach[i];
		h.reach_weight[i] += (t.reach_weight[i] * there - h.reach_weight[i]) * ease(dt, 0.08f);
	}
	// Lifted over on its way somewhere further off.
	V3 goal = t.wrist;
	const float far = length(goal - h.at);
	if (far > 0.03f)
		goal.y += std::min(LIFT, 0.45f * (far - 0.03f));
	// A spring that settles without overshooting (in small steps).
	const float w = 4.74f / GLIDE;
	const int steps = std::max(1, std::min(8, (int)std::ceil(dt / 0.0125f)));
	const float hstep = dt / (float)steps;
	for (int i = 0; i < steps; i++) {
		const V3 a = (goal - h.at) * (w * w) - h.speed * (2 * w);
		h.speed = h.speed + a * hstep;
		h.at = h.at + h.speed * hstep;
	}
	// Its typing wander, easing.
	h.drift = h.drift + (h.drift_to - h.drift) * ease(dt, 0.4f);
	const float k = ease(dt, 0.07f);
	h.along = normalized(h.along + (normalized(t.along) - h.along) * k, {0, 0, 1});
	V3 across = h.across + (normalized(t.across) - h.across) * k;
	h.across = normalized(across - h.along * dot(across, h.along), {1, 0, 0});
	for (int i = 0; i < 5; i++)
		for (int j = 0; j < 3; j++) {
			const float to = t.bend[i][j];
			h.bend[i][j] += (to - h.bend[i][j]) * ease(dt, to > h.bend[i][j] ? 0.03f : 0.06f);
		}
}

void GameHands::step(const DeviceState &s, double now, float dt)
{
	if (!std::isfinite(dt) || dt < 0)
		dt = 0;
	dt = std::min(dt, 0.1f);
	take(s, now, dt);
	// A typing hand's wander fades when it stops.
	for (Hand &h : hand_) {
		double last = -1;
		for (const Finger &f : h.finger)
			last = std::max(last, f.since);
		if (last < 0 || now - last > 2)
			h.drift_to = {};
	}
	const int kflip = options_.mirror == options_.mouse_right ? 1 : 0, pflip = options_.mirror ? 1 : 0;
	Target canon_kbm[2] = {keyboard_hand(0, now), typing_ ? keyboard_hand(1, now) : mouse_hand(now)};
	Target canon_pad[2];
	M4 pad, sticks[2];
	pad_targets(canon_pad, pad, sticks, now, dt);
	auto flipped = [](Target t, bool flip) {
		if (flip) {
			t.wrist = mirror_x(t.wrist);
			t.along = mirror_x(t.along);
			t.across = mirror_x(t.across);
			t.pole = mirror_x(t.pole);
			for (V3 &r : t.reach)
				r = mirror_x(r);
		}
		return t;
	};
	Target poles[2];
	for (int c = 0; c < 2; c++) {
		const bool on_pad = chosen_ == GameDevice::Pad;
		const int side = c ^ (on_pad ? pflip : kflip);
		const Target t = flipped(on_pad ? canon_pad[c] : canon_kbm[c], (on_pad ? pflip : kflip) != 0);
		glide(side, t, dt);
		poles[side] = t;
	}
	// Into its own size and place.
	const bool ok = body_.ok && body_.arm > 0;
	const float k = ok ? body_.arm / REAL_ARM : 0;
	const V3 mid = (body_.shoulder[0] + body_.shoulder[1]) * 0.5f;
	for (int side = 0; side < 2; side++) {
		DeskHand &d = now_.hand[side];
		const Hand &h = hand_[side];
		d.weight = ok ? on_ : 0;
		d.wrist = mid + h.at * k;
		d.along = h.along;
		d.across = h.across;
		d.pole = normalized(poles[side].pole, {0, -1, 0});
		for (int i = 0; i < 5; i++)
			for (int j = 0; j < 3; j++)
				d.bend[i][j] = h.bend[i][j];
		for (int i = 0; i < 5; i++) {
			d.reach[i] = mid + h.reach[i] * k;
			d.reach_weight[i] = ok ? h.reach_weight[i] : 0;
		}
	}
	M4 flip_k, flip_p;
	if (kflip)
		flip_k.m[0] = -1;
	if (pflip)
		flip_p.m[0] = -1;
	const M4 to_body = moved(mid) * scaled(k);
	const V3 desk{0, options_.height * REAL_ARM, options_.distance * REAL_ARM};
	const M4 mouse = moved(MOUSE_AT + desk + V3{mouse_x_, 0, mouse_z_}) * matrix_of(axis_angle({0, 1, 0}, mouse_turn_));
	const float shown = ok && options_.props ? 1.0f : 0.0f;
	now_.prop[DESK_KEYBOARD] = {shown * share_kbm_, to_body * flip_k * moved(KEYBOARD_AT + desk)};
	now_.prop[DESK_MOUSE] = {shown * share_kbm_, to_body * flip_k * mouse};
	now_.prop[DESK_MAT] = {shown * share_kbm_, to_body * flip_k * moved(MOUSE_AT + desk)};
	// (The pad only while its hands hold it.)
	const float held = shown * share_pad_ * on_;
	now_.prop[DESK_PAD] = {held, to_body * flip_p * pad};
	now_.prop[DESK_LEFT_STICK] = {held, to_body * flip_p * sticks[0]};
	now_.prop[DESK_RIGHT_STICK] = {held, to_body * flip_p * sticks[1]};
	now_.device = chosen_;
	now_.on = on_;
	now_.typing = typing_;
}

// ---- Framing it -----------------------------------------------------------------

bool desk_box(const DeskBody &b, const GameOptions &o, V3 &low, V3 &high)
{
	if (!b.ok || !(b.arm > 0))
		return false;
	// The keyboard and the mouse's mat (and the hands over them, a little
	// above), and the pad held up between them - as it's laid out, then
	// mirrored as it's used.
	const V3 desk{0, o.height * REAL_ARM, o.distance * REAL_ARM};
	V3 lo{KEYBOARD_AT.x - 0.19f, KEYBOARD_AT.y - 0.02f, KEYBOARD_AT.z + KEYS_HOME.z - 0.03f};
	V3 hi{KEYBOARD_AT.x + 0.19f, KEYBOARD_AT.y + 0.12f, KEYBOARD_AT.z + 0.08f};
	lo.x = std::min(lo.x, MOUSE_AT.x - 0.16f);
	lo.z = std::min(lo.z, MOUSE_AT.z - 0.13f);
	hi.z = std::max(hi.z, MOUSE_AT.z + 0.13f);
	if (o.mirror == o.mouse_right) {
		const float x = lo.x;
		lo.x = -hi.x;
		hi.x = -x;
	}
	const float s = hand_scale(b);
	lo.x = std::min(lo.x, PAD_AT.x - 0.13f * s);
	hi.x = std::max(hi.x, PAD_AT.x + 0.13f * s);
	hi.y = std::max(hi.y, PAD_AT.y + 0.08f * s);
	const float k = b.arm / REAL_ARM;
	const V3 mid = (b.shoulder[0] + b.shoulder[1]) * 0.5f;
	low = mid + (lo + desk) * k;
	high = mid + (hi + desk) * k;
	return true;
}

// ---- "Show me" -------------------------------------------------------------------

DeviceState pretend_devices(double t)
{
	DeviceState s;
	s.keyboard = s.mouse = s.pads = true;
	t = std::max(0.0, std::min(t, PRETEND_TIME));
	// Typing first (the mouse still: both hands type), then a game on the
	// keyboard and mouse, then one on a pad.
	constexpr double GAME = 4.5, PAD = 9.0;
	// Typing: bursts of 8 keys a second for 1.1 s, then a breath.
	auto typed = [](double u) {
		const int whole = (int)(u / 1.5);
		const double part = u - whole * 1.5;
		return (uint64_t)(whole * 9 + (int)(std::min(part, 1.1) * 8.0));
	};
	uint64_t presses = typed(std::min(t, GAME));
	int held = 0;
	if (t < GAME) {
		const double part = std::fmod(t, 1.5);
		if (part < 1.1 && std::fmod(part * 8.0, 1.0) < 0.5)
			held = 1;
		s.key_at = t;
	} else {
		// The game: W held all along, A or D tapped every 0.8 s (held a
		// quarter of a second), the mouse swept about and clicked.
		const double u = std::min(t, PAD) - GAME;
		presses += 1 + (uint64_t)(u / 0.8);
		held = t < PAD ? 1 + (std::fmod(u, 0.8) < 0.25 ? 1 : 0) : 0;
		s.key_at = std::min(t, PAD);
		s.mouse_x = 1800 * std::sin(2 * PI * u / 2.2);
		s.mouse_y = 600 * std::sin(2 * PI * u / 1.7);
		s.clicks = (uint64_t)(u / 0.7);
		s.mouse_buttons = t < PAD && std::fmod(u, 0.7) > 0.58 ? 1u : 0u;
		s.mouse_at = std::min(t, PAD);
	}
	s.key_presses = presses;
	s.keys_held = held;
	s.pads_in = 1;
	s.pad.connected = true;
	if (t >= PAD) {
		const double u = t - PAD;
		s.pad.left_x = 0.8f * (float)std::sin(2 * PI * u / 1.6);
		s.pad.left_y = 0.8f * (float)std::cos(2 * PI * u / 1.6);
		s.pad.right_x = 0.7f * (float)std::sin(2 * PI * u / 2.3);
		s.pad.right_trigger = std::fmod(u, 1.2) < 0.4 ? 1.0f : 0.0f;
		s.pad.buttons = std::fmod(u + 0.6, 0.9) < 0.15 ? PAD_A : 0u;
		if (u > 2.0 && u < 2.7)
			s.pad.buttons |= PAD_LEFT_BUMPER;
		s.pad_at = t;
	}
	return s;
}

} // namespace direbot::face
