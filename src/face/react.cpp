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

#include "react.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace direbot::face {

namespace {

constexpr float PI_F = 3.14159265f;

// A face eases in this fast, out this slowly, and out this fast when
// another takes over.
constexpr float FACE_IN = 0.25f, FACE_OUT = 0.4f, TAKE_OVER = 0.3f;
// A move hands over to the next in this long.
constexpr float MOVE_TAKE_OVER = 0.3f;
// How long a throw takes to fall away after it hits.
constexpr float FALL_TIME = 0.75f;
// A thing stuck to it slides down this far (heads) over its last moment
// stuck (seconds), then falls.
constexpr float STICK_SLIDE = 0.6f, STICK_DROP = 0.18f;
// As it sticks it turns to stand nearly upright (within this, radians)
// this fast (seconds).
constexpr float STICK_TILT = 0.3f, STICK_TURN = 0.15f;
// A bonk: its wind-up (seconds; the rest of FLY_TIME it comes down), how
// far it rises meanwhile (heads) and how far it tips back (radians).
constexpr float BONK_WIND = 0.36f, BONK_RISE = 0.12f, BONK_TIP = 0.55f;
// How far above its head a bonk comes down from (heads), with room; and
// how much of it (of its height) comes down into the top of its head -
// hair and all - as it lands on it.
constexpr float BONK_HIGH = 0.9f, BONK_SINK = 0.2f;
// The flash where one hits.
constexpr float FLASH_TIME = 0.32f;
// The "ouch": shut fast, held, then opened slowly.
constexpr float OUCH_SHUT = 0.05f, OUCH_HOLD = 0.3f, OUCH_OPEN = 0.45f;
// A headpat: its hand comes in from a side and lands, pats (a pat every
// PAT_TIME: lands, rests a moment pressing, is lifted from the wrist and
// falls again), and goes - from part way into its last pat's lift.
constexpr float PAT_ENTER = 0.5f, PAT_TIME = 0.42f, PAT_LEAVE = 0.5f, PAT_LEAVE_AT = 0.42f;
// Where in a pat (a share of it, from landing) it starts to lift, is up
// highest and lands again; how long its head takes to give under it, and
// to come back up.
constexpr float PAT_LIFT_FROM = 0.3f, PAT_TOP = 0.72f, PAT_GIVE = 0.12f, PAT_BACK = 0.35f;
// Where it comes in from and goes to (heads above the top of its head and
// out to its side), how high a pat lifts it (heads) and how far a
// picture of it turns at its wrist then (radians).
constexpr float PAT_FROM_RISE = 0.9f, PAT_FROM_AWAY = 0.55f, PAT_RISE = 0.1f, PAT_TURN = 0.38f;
// How much it squints meanwhile, how far each pat pushes its head (and the
// hand) down (heads) and nods it (degrees), and how far it leans into the
// hand (degrees).
constexpr float PAT_SQUINT = 0.7f, PAT_PRESS = 0.035f, PAT_NOD = 4.0f, PAT_LEAN = 6.0f;
// With its head up to the picture's top edge: at least this much of the
// hand shows (a share of its height), its bottom edge never lower than
// this (heads from the middle of its face: about its eyebrows).
constexpr float PAT_SHOWS = 0.45f, PAT_LOWEST = -0.25f;
// Once the hand has gone its eyes stay shut happily this long, then open
// over this long (seconds).
constexpr float PAT_HAPPY_HOLD = 0.6f, PAT_HAPPY_OPEN = 0.45f;
// Its head's squish follows each pat's give on a spring: how fast it
// swings (about four times a second) and how soon it settles - so it
// springs back a little past its own shape, like a squeezed toy.
constexpr float SQUISH_W = 24.0f, SQUISH_DAMP = 0.35f;
// The knock-back springs: how fast they swing and how soon they settle
// (swung out in a tenth of a second, settled in two thirds), and the most
// each can be pushed.
constexpr float KNOCK_W = 14.0f, KNOCK_DAMP = 0.32f;
constexpr float MOST_YAW = 35, MOST_PITCH = 30, MOST_ROLL = 25, MOST_MOVE = 0.35f;
// 0.9.0, what it does as it hits. Broken (a splat, pieces): squashed flat
// against it this long (seconds), then gone.
constexpr float CRUSH = 0.07f;
// A splat: this many times as wide as the thing was as it hit; it spreads
// out over SPLAT_SPREAD (seconds), a touch past its size, and settles;
// at the end of its stay it slides down this far (heads) as it fades.
constexpr float SPLAT_ACROSS = 1.6f, SPLAT_SPREAD = 0.09f, MARK_FADE = 0.8f, MARK_SLIDE = 0.1f;
// The snow a snowball leaves: as wide as the snowball was, this many times.
constexpr float SNOW_ACROSS = 1.3f;
// A drip: as wide as this share of its splat.
constexpr float DRIP_ACROSS = 0.11f;
// Bits fall this fast (heads a second, each second); a feather drifts
// down at about FEATHER_FALL (heads a second), slowed by the air.
constexpr float BIT_GRAVITY = 8.5f, FEATHER_FALL = 0.55f, FEATHER_DRAG = 3.2f;
// A squishy thing (a rubber duck): how flat it squashes, and its wobble
// back (how fast it settles, seconds; how fast it wobbles).
constexpr float SQUISH_FLAT = 0.6f, SQUISH_SETTLE = 0.12f, SQUISH_WOBBLE = 30.0f;

float clamp01(float v)
{
	return v < 0 ? 0 : v > 1 ? 1 : v;
}
float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}
float smooth(float t)
{
	t = clamp01(t);
	return t * t * (3 - 2 * t);
}
float sq(float v)
{
	return v * v;
}
bool finite(float v)
{
	return std::isfinite(v);
}

// How much a move shows `u` seconds into it, `length` long: eased in and
// out (quicker for a short one).
float envelope(float u, float length)
{
	return smooth(u / move_ease_in(length)) * smooth((length - u) / move_ease_out(length));
}

} // namespace

const char *effect_name(Effect e)
{
	switch (e) {
	case Effect::Splat:
		return "splat";
	case Effect::Feathers:
		return "feathers";
	case Effect::Pieces:
		return "pieces";
	default:
		return "";
	}
}

Effect effect_named(const std::string &name)
{
	std::string n;
	for (char c : name)
		n += (char)std::tolower((unsigned char)c);
	for (Effect e : {Effect::Splat, Effect::Feathers, Effect::Pieces})
		if (n == effect_name(e))
			return e;
	return Effect::None;
}

float cover_zoom(float w, float h, float cx, float cy, float dx, float dy, float turn)
{
	if (!(w > 2 && h > 2) || !finite(cx + cy + dx + dy + turn))
		return 1;
	cx = clampf(cx, 1, w - 1);
	cy = clampf(cy, 1, h - 1);
	const float co = std::cos(turn), si = std::sin(turn);
	float k = 1;
	// Each corner of the picture shown comes from inside the picture.
	for (float px : {0.0f, w})
		for (float py : {0.0f, h}) {
			const float ux = px - cx - dx, uy = py - cy - dy;
			const float vx = co * ux + si * uy, vy = -si * ux + co * uy;
			k = std::max(k, vx > 0 ? vx / (w - cx) : -vx / cx);
			k = std::max(k, vy > 0 ? vy / (h - cy) : -vy / cy);
		}
	return std::min(k, 1.6f);
}

float move_ease_in(float length)
{
	return std::max(1e-4f, std::min(0.3f, length * 0.35f));
}

float move_ease_out(float length)
{
	return std::max(1e-4f, std::min(0.45f, length * 0.45f));
}

const char *builtin_name(BuiltIn b)
{
	switch (b) {
	case BuiltIn::Nod:
		return "nod";
	case BuiltIn::Shake:
		return "shake";
	case BuiltIn::Tilt:
		return "tilt";
	case BuiltIn::Bounce:
		return "bounce";
	case BuiltIn::Wave:
		return "wave";
	case BuiltIn::Cheer:
		return "cheer";
	default:
		return "";
	}
}

BuiltIn builtin_named(const std::string &name)
{
	std::string n;
	for (char c : name)
		n += (char)std::tolower((unsigned char)c);
	for (BuiltIn b : {BuiltIn::Nod, BuiltIn::Shake, BuiltIn::Tilt, BuiltIn::Bounce, BuiltIn::Wave, BuiltIn::Cheer})
		if (n == builtin_name(b))
			return b;
	return BuiltIn::None;
}

bool builtin_arms(BuiltIn b)
{
	return b == BuiltIn::Wave || b == BuiltIn::Cheer;
}

float builtin_length(BuiltIn b)
{
	switch (b) {
	case BuiltIn::Nod:
		return 1.0f;
	case BuiltIn::Shake:
		return 1.2f;
	case BuiltIn::Tilt:
		return 1.8f;
	case BuiltIn::Bounce:
		return 0.9f;
	case BuiltIn::Wave:
		return 2.6f;
	case BuiltIn::Cheer:
		return 2.2f;
	default:
		return 0;
	}
}

React::React(uint32_t seed)
{
	// Stirred, so starts close together still throw differently.
	uint32_t x = seed + 0x9E3779B9u;
	x = (x ^ (x >> 16)) * 0x85EBCA6Bu;
	x = (x ^ (x >> 13)) * 0xC2B2AE35u;
	x ^= x >> 16;
	state_ = x ? x : 1;
}

float React::random01()
{
	// xorshift32: the same throws from the same start (for tests).
	uint32_t x = state_;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	state_ = x;
	return (float)(x >> 8) / 16777216.0f;
}

void React::Spring::step(float dt, float stiffness, float damping)
{
	speed += (-stiffness * value - damping * speed) * dt;
	value += speed * dt;
}

void React::face(const std::string &name, float seconds, float amount)
{
	if (!finite(seconds) || !finite(amount))
		return;
	seconds = clampf(seconds, 0.1f, MOST_FACE_TIME);
	amount = clamp01(amount);
	// No face: the one on eases off.
	for (Cue &c : faces_) {
		if (c.name == name && c.out_at < 0 && !name.empty()) {
			// The same face again: on for `seconds` more from now.
			c.length = std::max(c.length, time_ - c.at + seconds);
			c.amount = amount;
			continue;
		}
		if (c.out_at < 0)
			c.out_at = time_;
	}
	if (name.empty())
		return;
	for (const Cue &c : faces_)
		if (c.name == name && c.out_at < 0)
			return;
	Cue c;
	c.name = name;
	c.amount = amount;
	c.at = time_;
	c.length = seconds;
	faces_.push_back(c);
	// Never many: the oldest (already easing out) go first.
	while (faces_.size() > 6)
		faces_.erase(faces_.begin());
}

void React::move(BuiltIn b)
{
	if (b == BuiltIn::None)
		return;
	for (Act &a : acts_)
		if (a.out_at < 0)
			a.out_at = time_;
	Act a;
	a.builtin = b;
	a.at = time_;
	a.length = builtin_length(b);
	acts_.push_back(a);
	while (acts_.size() > 3)
		acts_.erase(acts_.begin());
}

void React::play(int id, float length)
{
	if (id < 0 || !finite(length))
		return;
	for (Act &a : acts_)
		if (a.out_at < 0)
			a.out_at = time_;
	Act a;
	a.id = id;
	a.at = time_;
	a.length = clampf(length, 0.2f, 600.0f);
	acts_.push_back(a);
	while (acts_.size() > 3)
		acts_.erase(acts_.begin());
}

float React::throw_at(const Throw &t)
{
	if ((int)waiting_.size() >= MOST_WAITING)
		return -1;
	// It leaves once those before it have, one a THROW_GAP apart (never
	// more than MOST_FLYING on their way: FLY_TIME / THROW_GAP are at most).
	const float leaves = std::max(0.0f, THROW_GAP - since_launch_) + THROW_GAP * (float)waiting_.size();
	Item it;
	it.t = t;
	it.t.strength = finite(t.strength) ? clampf(t.strength, 0.1f, 2.0f) : 1.0f;
	it.t.size = finite(t.size) ? clampf(t.size, 0.2f, 3.0f) : 1.0f;
	it.t.face_time = finite(t.face_time) ? clampf(t.face_time, 0.1f, 10.0f) : 1.5f;
	it.t.side = finite(t.side) ? t.side : 0.0f;
	it.t.stick = finite(t.stick) ? clampf(t.stick, 0, MOST_STICK) : 0.0f;
	HitLook &look = it.t.look;
	look.seconds = finite(look.seconds) ? clampf(look.seconds, 0.5f, MOST_MARK) : 4.0f;
	// Broken as it hits: it can't stick.
	if (look.effect == Effect::Splat || look.effect == Effect::Pieces)
		it.t.stick = 0;
	waiting_.push_back(it);
	return leaves + FLY_TIME;
}

void React::pat(const Pat &p)
{
	const int count = std::max(1, std::min(MOST_PATS, p.count));
	if (patting_ && !leaving_) {
		// Patting already: more pats (the same hand), never more than
		// MOST_PATS to come.
		const int landed = pat_t_ < PAT_ENTER ? 0 : 1 + (int)((pat_t_ - PAT_ENTER) / PAT_TIME);
		const int left = std::max(0, pat_count_ - landed);
		pat_count_ += std::max(0, std::min(count, MOST_PATS - left));
		pat_leave_ = PAT_ENTER + (pat_count_ - 1 + PAT_LEAVE_AT) * PAT_TIME;
		if (!pat_.face.empty())
			face(pat_.face, pat_leave_ - pat_t_ + PAT_LEAVE * 0.5f);
		return;
	}
	// Its hand still going from the last one: the new one takes over from
	// where it is (from the same side).
	const bool taking_over = patting_ && leaving_ && shown_;
	const float side = taking_over ? pat_.side : p.side < 0 ? -1.0f : p.side > 0 ? 1.0f : (random01() < 0.5f ? -1.0f : 1.0f);
	if (taking_over) {
		from_ = last_pose_;
		blend_ = 0;
	} else {
		blend_ = 1;
	}
	patting_ = true;
	leaving_ = false;
	pat_ = p;
	pat_.side = side;
	pat_.count = count;
	pat_.size = finite(p.size) ? clampf(p.size, 0.3f, 3.0f) : 1.0f;
	pat_t_ = 0;
	pat_count_ = count;
	pat_leave_ = PAT_ENTER + (count - 1 + PAT_LEAVE_AT) * PAT_TIME;
	pats_done_ = 0;
	after_ = -1;
	hurry_after_ = false;
	if (!pat_.face.empty())
		face(pat_.face, pat_leave_ + PAT_LEAVE + PAT_HAPPY_HOLD + PAT_HAPPY_OPEN * 0.5f);
}

// The hand `t` seconds into a headpat, before it leaves: coming in over
// its head (by two thirds of the way in it's up over the top of it,
// lifted as between pats), falling onto it; then each pat.
React::PatPose React::pat_pose(float t) const
{
	PatPose p;
	p.lean = smooth(t / PAT_ENTER);
	if (t < PAT_ENTER) {
		const float u = t / PAT_ENTER, over = 0.65f;
		p.opacity = smooth(u / 0.35f);
		if (u < over) {
			const float e = smooth(u / over);
			p.away = PAT_FROM_AWAY * (1 - e);
			p.rise = PAT_FROM_RISE + (PAT_RISE - PAT_FROM_RISE) * e;
			p.lift = 0.6f + 0.4f * e;
		} else {
			// Falling, faster as it goes, like a pat.
			const float f = 1 - sq((u - over) / (1 - over));
			p.rise = PAT_RISE * f;
			p.lift = f;
		}
		p.squint = PAT_SQUINT * smooth((u - 0.5f) / 0.5f);
		return p;
	}
	const float into = t - PAT_ENTER;
	const float phase = (into - PAT_TIME * std::floor(into / PAT_TIME)) / PAT_TIME;
	p.opacity = 1;
	p.squint = PAT_SQUINT;
	// Its head gives under it as it lands, and comes back up.
	p.press = phase < PAT_GIVE ? smooth(phase / PAT_GIVE) : 1 - smooth((phase - PAT_GIVE) / PAT_BACK);
	// Resting a moment, lifted (eased), and let fall (faster as it goes).
	if (phase < PAT_LIFT_FROM)
		p.lift = 0;
	else if (phase < PAT_TOP)
		p.lift = smooth((phase - PAT_LIFT_FROM) / (PAT_TOP - PAT_LIFT_FROM));
	else
		p.lift = 1 - sq((phase - PAT_TOP) / (1 - PAT_TOP));
	p.rise = PAT_RISE * p.lift;
	return p;
}

void React::stop()
{
	for (Cue &c : faces_)
		if (c.out_at < 0)
			c.out_at = time_;
	for (Act &a : acts_)
		if (a.out_at < 0)
			a.out_at = time_;
	waiting_.clear();
	// Its eyes open now, after a headpat (not kept shut a moment).
	hurry_after_ = true;
	if (after_ >= 0)
		after_ = std::max(after_, PAT_HAPPY_HOLD);
	// What's in the air falls away (no hit).
	for (Item &it : flying_) {
		// Stuck ones fall off now.
		unstick(it);
		if (it.hit)
			continue;
		const float u = clamp01(it.at / FLY_TIME);
		it.hit = true;
		it.dropped = true;
		it.hit_at = it.at;
		it.vx = 0.3f * (it.x1 - it.x0) / FLY_TIME;
		it.vy = std::min(0.3f * ((it.y1 - it.y0) - it.arc * 4 * (1 - 2 * u)) / FLY_TIME, 0.0f);
	}
	if (patting_ && !leaving_ && pat_t_ < pat_leave_)
		pat_leave_ = pat_t_;
	// What's left on its face fades now; bits in the air too.
	for (MarkItem &m : marks_)
		if (m.fade_at < 0 || m.fade_time > 0.35f) {
			m.fade_at = m.at;
			m.fade_time = 0.35f;
		}
	for (BitItem &b : bits_)
		if (b.fade_at < 0)
			b.fade_at = b.at;
}

void React::launch(Item &it, const ReactView &view)
{
	const Throw &t = it.t;
	it.side = t.side < 0 ? -1.0f : t.side > 0 ? 1.0f : (random01() < 0.5f ? -1.0f : 1.0f);
	it.size0 = 0.45f * t.size;
	if (t.above) {
		// A bonk: it shows up over its head (as big as it is: it isn't
		// thrown from where you watch) and comes down on the top of it -
		// from as high as the picture has room for. It bounces off to
		// `side`.
		// Its bottom on the top of its head (a little into it): not its
		// middle, which hid its forehead and eyes.
		const float half = 0.5f * it.size0 * clampf(finite(t.tall) ? t.tall : 1, 0.05f, 4);
		const float top = -clampf(finite(view.head_top) ? view.head_top : 0.6f, 0.2f, 3);
		// (A head up to the picture's top edge: no higher than its
		// wind-up allows, so at least half of it always shows.)
		it.x1 = random(-0.12f, 0.12f);
		it.y1 = std::max(top - half * (1 - 2 * BONK_SINK), view.top + BONK_RISE);
		it.touch_y = it.y1 + half * (1 - 2 * BONK_SINK);
		// From up to BONK_HIGH over its head - all of it in the picture,
		// wind-up and all. A head up to the picture's top edge (a
		// character filling it) leaves less room: then it comes down
		// slantwise from above its other side as well, as far again.
		const float highest = view.top + half * 1.2f + BONK_RISE;
		const float room = clampf(it.y1 - highest, 0, BONK_HIGH);
		it.y0 = it.y1 - room;
		it.x0 = it.x1 - it.side * (BONK_HIGH - room);
		// (In the picture across, too, when there's room for it.)
		const float lo = view.left + it.size0 * 0.6f, hi = view.right - it.size0 * 0.6f;
		if (lo < hi)
			it.x0 = clampf(it.x0, lo, hi);
		it.arc = 0;
		it.spin = 0;
		it.angle = 0;
		it.axis = {0, 0, 1};
		it.x = it.x0;
		it.y = it.y0;
		it.size = it.size0;
		return;
	}
	// From just past the picture's side, somewhere about level with its
	// face (thrown from where you watch: big at first)...
	const float big = it.size0 * 2.5f;
	it.x0 = it.side > 0 ? view.right + big * 0.6f : view.left - big * 0.6f;
	const float lo = std::max(view.top + big * 0.5f, -0.6f), hi = std::min(view.bottom - big * 0.5f, 1.2f);
	it.y0 = lo < hi ? random(lo, hi) : 0.3f;
	// ...to somewhere on its face, over an arc (flatter the harder).
	it.x1 = random(-0.22f, 0.22f);
	it.y1 = random(-0.22f, 0.12f);
	it.touch_y = it.y1;
	it.arc = random(0.35f, 0.75f) / (0.6f + 0.4f * t.strength);
	// Spinning the way it flies.
	it.spin = -it.side * random(5.0f, 10.0f);
	it.angle = random(-PI_F, PI_F);
	// A 3D one tumbles about a way of its own, mostly end over end.
	it.axis = normalized(V3{random(-0.4f, 0.4f), random(-1, 1), 1}, {0, 0, 1});
	it.x = it.x0;
	it.y = it.y0;
	it.size = big;
}

void React::hit(Item &it, ReactOut &out)
{
	const Throw &t = it.t;
	out.hits++;
	// Which way it was going as it hit (heads a second), and that way
	// on the picture (unit).
	const float vx = (it.x1 - it.x0) / FLY_TIME, vy = ((it.y1 - it.y0) + 4 * it.arc) / FLY_TIME;
	const float len = std::sqrt(vx * vx + vy * vy);
	const float ux = len > 1e-4f ? vx / len : -it.side, uy = len > 1e-4f ? vy / len : 0.0f;
	const float k = t.strength;
	// Its head knocked back where it hit (springs: each pushed so it
	// swings out about this far). Thrown sideways it turns away; on the
	// side of its face it turns that way too; the hit tips its head back
	// (more if high, less if it was coming down), and sideways.
	auto push = [&](Spring &s, float peak) {
		s.speed += peak * KNOCK_W * 1.5f;
	};
	if (t.above) {
		// Bonked on top: its head knocked down (a nod), tipped a little,
		// and squished; its hair thrown down.
		push(pitch_, -k * 11.0f);
		push(roll_, k * random(-4.0f, 4.0f));
		push(dy_, k * 0.05f);
		squish_speed_ += k * 30.0f;
		out.knock = out.knock + V3{0, -2.4f * k, -0.6f * k};
	} else {
		push(yaw_, k * (9.0f * ux + 10.0f * it.x1));
		push(pitch_, k * (8.0f - 10.0f * it.y1 - 3.0f * uy));
		push(roll_, k * (6.0f * ux));
		push(dx_, k * 0.06f * ux);
		push(dy_, k * 0.025f * uy);
		// Its swinging parts thrown the way it went, and back.
		out.knock = out.knock + V3{ux * 2.2f * k, -uy * 1.2f * k, -2.6f * k};
	}
	// The flash, the "ouch" and its face.
	Flash f;
	f.x = it.x1;
	f.y = it.touch_y;
	f.at = time_;
	f.size = it.size0 * 1.4f;
	flashes_.push_back(f);
	if (flashes_.size() > 8)
		flashes_.erase(flashes_.begin());
	ouch_ = 0;
	if (!t.face.empty())
		face(t.face, t.face_time);
	// Bouncing off: back the way it came a little, up, then falling (a
	// bonk: up and off to its side, spinning).
	if (t.above) {
		it.vx = it.side * random(0.6f, 1.2f);
		it.vy = -random(2.0f, 2.8f);
		it.spin = it.side * random(4.0f, 8.0f);
	} else {
		it.vx = -ux * random(0.8f, 1.6f) * (0.6f + 0.4f * k);
		it.vy = -random(1.6f, 2.6f);
		it.spin = -it.spin * 0.6f;
	}
	it.x = it.x1;
	it.y = it.y1;
	effect(it, it.x1, it.touch_y);
	if (t.look.effect == Effect::Splat || t.look.effect == Effect::Pieces) {
		// Broken against it: no bouncing off.
		it.broke = true;
		it.vx = it.vy = 0;
		it.spin = 0;
		return;
	}
	// Or stuck where it hit a while - turning to stand nearly upright
	// (the nearest way round), as if it had landed so.
	if (t.stick > 0) {
		it.stuck = true;
		it.stuck_roll = roll_now_;
		it.stuck_at = time_;
		it.stuck_till = it.at + t.stick;
		it.stuck_from = it.angle;
		const float tilt = random(-STICK_TILT, STICK_TILT);
		it.stuck_angle = it.angle - std::remainder(it.angle - tilt, 2 * PI_F);
	}
}

void React::unstick(Item &it)
{
	if (!it.stuck)
		return;
	it.stuck = false;
	// Falling from where it is now (its fade starts again).
	it.hit_at = it.at;
	it.vx = random(-0.3f, 0.3f);
	it.vy = 0.4f;
	it.spin = random(-3.0f, 3.0f);
}

int React::stuck() const
{
	int n = 0;
	for (const Item &it : flying_)
		n += it.stuck;
	return n;
}

void React::add_mark(const MarkItem &m)
{
	marks_.push_back(m);
	// Never too many: the oldest still showing fades now (its drips with
	// it - they're left with it).
	int showing = 0;
	for (const MarkItem &k : marks_)
		showing += !k.drip && k.fade_at < 0;
	for (MarkItem &k : marks_) {
		if (showing <= MOST_MARKS)
			break;
		if (!k.drip && k.fade_at < 0) {
			k.fade_at = k.at;
			k.fade_time = 0.4f;
			showing--;
			for (MarkItem &d : marks_)
				if (d.drip && d.id == k.id && d.fade_at < 0) {
					d.fade_at = d.at;
					d.fade_time = 0.4f;
				}
		}
	}
}

void React::effect(const Item &it, float x, float y)
{
	const HitLook &look = it.t.look;
	if (look.effect == Effect::None)
		return;
	// As big as it was made (0.45 heads across): bigger or smaller things
	// splash bigger or smaller.
	const float k = it.size0 / 0.45f, rk = std::sqrt(k);
	auto bit_picture = [&]() {
		return look.bits.empty() ? -1 : look.bits[(size_t)(random01() * look.bits.size()) % look.bits.size()];
	};
	auto add_bit = [&](const BitItem &b) {
		if ((int)bits_.size() < MOST_BITS && b.picture >= 0)
			bits_.push_back(b);
	};
	if (look.effect == Effect::Splat) {
		MarkItem m;
		m.picture = look.mark;
		m.x = x + random(-0.03f, 0.03f);
		m.y = y + random(-0.02f, 0.03f);
		m.size = SPLAT_ACROSS * it.size0;
		m.angle = random(-PI_F, PI_F);
		m.stay = look.seconds;
		m.grow = 0;
		m.roll = roll_now_;
		m.id = ++mark_ids_;
		if (m.picture >= 0)
			add_mark(m);
		// Drips running down from its lower part.
		const int drips = look.drip >= 0 && m.picture >= 0 ? 2 + (random01() < 0.5f) : 0;
		for (int i = 0; i < drips; i++) {
			MarkItem d;
			d.picture = look.drip;
			d.drip = true;
			// Spread across its lower part, never two on top of each other.
			const float across = (i + 0.5f) / drips - 0.5f;
			d.x = m.x + m.size * (0.62f * across + random(-0.05f, 0.05f));
			d.y = m.y + m.size * random(0.08f, 0.2f);
			d.size = m.size * DRIP_ACROSS * random(0.8f, 1.15f);
			d.stay = look.seconds;
			d.wait = random(0.12f, 0.55f);
			d.run = random(0.14f, 0.32f) * k;
			d.run_time = random(1.3f, 2.6f);
			d.roll = roll_now_;
			d.id = m.id;
			marks_.push_back(d);
		}
		// Juice thrown off it, out and up from where it burst.
		const int drops = look.bits.empty() ? 0 : 8 + (int)(random01() * 4);
		for (int i = 0; i < drops; i++) {
			BitItem b;
			b.kind = BitKind::Drop;
			b.picture = bit_picture();
			const float a = random(-PI_F, PI_F), v = random(1.4f, 3.2f) * rk;
			b.x = x;
			b.y = y;
			b.vx = v * std::cos(a);
			b.vy = v * std::sin(a) - random(0.4f, 1.2f);
			b.size = random(0.05f, 0.1f) * k;
			b.life = random(0.45f, 0.75f);
			add_bit(b);
		}
	} else if (look.effect == Effect::Feathers) {
		const int n = 12 + (int)(random01() * 5);
		for (int i = 0; i < n; i++) {
			BitItem b;
			b.kind = BitKind::Feather;
			b.picture = bit_picture();
			b.x = x + random(-0.25f, 0.25f) * k;
			b.y = y + random(-0.12f, 0.08f) * k;
			const float out = b.x - x;
			b.vx = (out >= 0 ? 1 : -1) * random(0.3f, 1.6f) * rk + out * 2;
			// A short puff up (not off the top of the picture), then down
			// past its face.
			b.vy = -random(0.3f, 1.2f) * rk;
			b.size = random(0.09f, 0.13f) * k;
			b.angle = random(-1.2f, 1.2f);
			b.spin = random(-4.0f, 4.0f);
			b.sway = random(0.07f, 0.15f) * k;
			b.sway_w = random(2.8f, 4.6f);
			b.sway_at = random(0, 2 * PI_F);
			b.life = random(3.2f, 4.4f);
			add_bit(b);
		}
	} else if (look.effect == Effect::Pieces) {
		// The snow left on its face a moment.
		if (look.mark >= 0) {
			MarkItem m;
			m.picture = look.mark;
			m.x = x + random(-0.02f, 0.02f);
			m.y = y;
			m.size = SNOW_ACROSS * it.size0;
			m.angle = random(-PI_F, PI_F);
			m.stay = look.seconds;
			m.grow = 0;
			m.roll = roll_now_;
			m.id = ++mark_ids_;
			add_mark(m);
		}
		const int n = look.bits.empty() ? 0 : 9 + (int)(random01() * 4);
		for (int i = 0; i < n; i++) {
			BitItem b;
			b.kind = BitKind::Chunk;
			b.picture = bit_picture();
			const float a = random(-PI_F, PI_F), v = random(1.2f, 3.0f) * rk;
			b.x = x + 0.12f * k * std::cos(a);
			b.y = y + 0.12f * k * std::sin(a);
			b.vx = v * std::cos(a);
			b.vy = v * std::sin(a) - random(0.5f, 1.3f);
			b.size = random(0.08f, 0.15f) * k;
			b.angle = random(-PI_F, PI_F);
			b.spin = random(-10.0f, 10.0f);
			b.life = random(0.7f, 1.1f);
			add_bit(b);
		}
		for (int i = 0; i < 2 && look.puff >= 0; i++) {
			BitItem b;
			b.kind = BitKind::Puff;
			b.picture = look.puff;
			b.x = x + random(-0.05f, 0.05f) * k;
			b.y = y + random(-0.05f, 0.03f) * k;
			b.vx = random(-0.2f, 0.2f);
			b.vy = -random(0.05f, 0.25f);
			b.size = it.size0 * random(1.0f, 1.25f);
			b.angle = random(-PI_F, PI_F);
			b.spin = random(-0.8f, 0.8f);
			b.life = random(0.5f, 0.65f);
			add_bit(b);
		}
	}
}

void React::step_effects(float dt, const ReactView &view, ReactOut &out)
{
	// The bits in the air.
	for (BitItem &b : bits_) {
		b.at += dt;
		switch (b.kind) {
		case BitKind::Feather: {
			// Slowed by the air, drifting down, swaying side to side - more
			// as it slows - and tipping the way it swings.
			const float drag = std::exp(-FEATHER_DRAG * dt);
			b.vx *= drag;
			b.vy = FEATHER_FALL + (b.vy - FEATHER_FALL) * drag;
			b.x += b.vx * dt;
			b.y += b.vy * dt;
			b.spin *= drag;
			b.angle += b.spin * dt;
			const float ph = b.sway_w * b.at + b.sway_at, grows = smooth(b.at / 0.5f);
			b.sway_x = b.sway * grows * std::sin(ph);
			break;
		}
		case BitKind::Puff: {
			const float drag = std::exp(-2.0f * dt);
			b.vx *= drag;
			b.vy *= drag;
			b.x += b.vx * dt;
			b.y += b.vy * dt;
			b.angle += b.spin * dt;
			break;
		}
		default:
			b.vy += BIT_GRAVITY * dt;
			b.x += b.vx * dt;
			b.y += b.vy * dt;
			b.angle += b.spin * dt;
			break;
		}
	}
	bits_.erase(std::remove_if(bits_.begin(), bits_.end(),
				   [&](const BitItem &b) {
					   return b.at >= b.life || (b.fade_at >= 0 && b.at - b.fade_at >= 0.3f) ||
						  b.y - b.size > view.bottom + 0.5f;
				   }),
		    bits_.end());
	for (const BitItem &b : bits_) {
		Bit o;
		o.picture = b.picture;
		o.x = b.x;
		o.y = b.y;
		o.angle = b.angle;
		o.size = b.size;
		float fade = 1;
		switch (b.kind) {
		case BitKind::Drop: {
			// Its point trailing behind it, the way it flies.
			o.angle = std::atan2(-b.vx, b.vy);
			// Stretched a little by its speed.
			fade = 1 - smooth((b.at - b.life * 0.6f) / (b.life * 0.4f));
			break;
		}
		case BitKind::Feather: {
			const float ph = b.sway_w * b.at + b.sway_at;
			o.x = b.x + b.sway_x;
			o.angle = b.angle + 0.55f * smooth(b.at / 0.5f) * std::cos(ph);
			fade = smooth(b.at / 0.06f) * (1 - smooth((b.at - (b.life - 0.7f)) / 0.7f));
			break;
		}
		case BitKind::Chunk:
			fade = 1 - smooth((b.at - (b.life - 0.3f)) / 0.3f);
			break;
		case BitKind::Puff: {
			const float u = b.at / b.life;
			o.size = b.size * (1 + 1.1f * (1 - sq(1 - clamp01(u))));
			fade = 0.85f * (1 - smooth(u));
			break;
		}
		}
		if (b.fade_at >= 0)
			fade *= 1 - smooth((b.at - b.fade_at) / 0.3f);
		o.opacity = clamp01(fade);
		if (o.opacity > 0.001f)
			out.bits.push_back(o);
	}

	// What's left on its face.
	for (MarkItem &m : marks_)
		m.at += dt;
	marks_.erase(std::remove_if(marks_.begin(), marks_.end(),
				    [&](const MarkItem &m) {
					    return m.at >= m.stay + MARK_FADE ||
						   (m.fade_at >= 0 && m.at - m.fade_at >= m.fade_time);
				    }),
		     marks_.end());
}

ReactOut React::step(float dt, const ReactView &view)
{
	ReactOut out;
	if (!finite(dt) || dt < 0)
		dt = 0;
	dt = std::min(dt, 0.25f);
	time_ += dt;
	since_launch_ += dt;
	roll_now_ = finite(view.roll) ? view.roll : 0.0f;

	// Throws: launched one a moment apart (and never too many on their way).
	int coming = 0;
	for (const Item &it : flying_)
		coming += !it.hit;
	while (!waiting_.empty() && coming < MOST_FLYING && since_launch_ >= THROW_GAP - 1e-4f) {
		Item it = waiting_.front();
		waiting_.erase(waiting_.begin());
		launch(it, view);
		flying_.push_back(it);
		since_launch_ = 0;
		coming++;
	}
	for (Item &it : flying_) {
		it.at += dt;
		if (!it.hit) {
			const float u = clamp01(it.at / FLY_TIME);
			if (it.t.above) {
				// A bonk: rising a little and tipping back (away from
				// the side it'll bounce off to), then coming down,
				// faster as it falls, swinging forward.
				const float top = it.y0 - BONK_RISE;
				if (it.at < BONK_WIND) {
					const float w = smooth(it.at / BONK_WIND);
					it.x = it.x0;
					it.y = it.y0 - BONK_RISE * w;
					it.angle = -it.side * BONK_TIP * w;
				} else {
					const float d = clamp01((it.at - BONK_WIND) / (FLY_TIME - BONK_WIND));
					it.x = it.x0 + (it.x1 - it.x0) * d * d;
					it.y = top + (it.y1 - top) * d * d;
					it.angle = -it.side * BONK_TIP * (1 - 1.4f * d * d);
				}
				it.size = it.size0;
			} else {
				it.x = it.x0 + (it.x1 - it.x0) * u;
				it.y = it.y0 + (it.y1 - it.y0) * u - it.arc * 4 * u * (1 - u);
				// Smaller as it goes away from you, towards its face.
				it.size = it.size0 / (0.4f + 0.6f * u);
				it.angle += it.spin * dt;
			}
			if (it.at >= FLY_TIME) {
				it.hit = true;
				it.hit_at = FLY_TIME;
				hit(it, out);
			}
		} else if (it.broke) {
			// Squashed flat against it where it broke.
			it.x = it.x1;
			it.y = it.y1;
		} else if (it.stuck) {
			// Stuck where it hit; sliding down a little at the end, then
			// falling.
			const float slide = 1 - smooth((it.stuck_till - it.at) / STICK_SLIDE);
			it.x = it.x1;
			it.y = it.y1 + STICK_DROP * slide * slide;
			it.angle = it.stuck_from +
				   (it.stuck_angle - it.stuck_from) * smooth((it.at - it.hit_at) / STICK_TURN);
			if (it.at >= it.stuck_till)
				unstick(it);
		} else {
			it.vy += 9.0f * dt;
			it.x += it.vx * dt;
			it.y += it.vy * dt;
			it.angle += it.spin * dt;
			// Coming back towards you a little as it falls.
			it.size += it.size0 * 0.35f * dt;
		}
	}
	// Never too many stuck at once: the one stuck longest falls first.
	for (int n = stuck(); n > MOST_STUCK; n--) {
		Item *oldest = nullptr;
		for (Item &it : flying_)
			if (it.stuck && (!oldest || it.stuck_at < oldest->stuck_at))
				oldest = &it;
		unstick(*oldest);
	}
	flying_.erase(std::remove_if(flying_.begin(), flying_.end(),
				     [&](const Item &it) {
					     return it.hit && !it.stuck &&
						    ((it.broke && it.at - it.hit_at >= CRUSH) ||
						     it.at - it.hit_at >= FALL_TIME ||
						     it.y - it.size > view.bottom + 0.5f);
				     }),
		      flying_.end());
	for (const Item &it : flying_) {
		Flying f;
		f.picture = it.t.picture;
		f.x = it.x;
		f.y = it.y;
		f.size = it.size;
		f.angle = it.angle;
		f.axis = it.axis;
		f.hit = it.hit;
		f.stuck = it.stuck;
		if (it.stuck) {
			// Squashed flat against it a moment, then a little flat while
			// it's stuck there.
			const float e = std::max(0.0f, 1 - (it.at - it.hit_at) / 0.14f);
			f.opacity = 1;
			f.squash_x = 1.06f + 0.29f * e;
			f.squash_y = 0.95f - 0.25f * e;
		} else if (it.broke) {
			// Crushed flat against it, then gone (what it left takes over).
			const float u = clamp01((it.at - it.hit_at) / CRUSH);
			f.opacity = 1 - smooth(u);
			f.squash_x = 1.3f + 0.35f * u;
			f.squash_y = 0.6f - 0.25f * u;
		} else if (it.hit) {
			const float tt = it.at - it.hit_at;
			f.opacity = 1 - smooth((tt - (it.dropped ? 0.0f : 0.25f)) / (it.dropped ? 0.3f : FALL_TIME - 0.25f));
			if (!it.dropped && it.t.look.squishy) {
				// Squashed flat, wobbling back as it bounces off.
				const float e = std::exp(-tt / SQUISH_SETTLE) * std::cos(tt * SQUISH_WOBBLE);
				f.squash_x = 1 + SQUISH_FLAT * e;
				f.squash_y = 1 - SQUISH_FLAT * 0.8f * e;
			} else if (!it.dropped) {
				// Squashed flat against its face a moment, then springing back.
				const float e = std::max(0.0f, 1 - tt / 0.14f);
				f.squash_x = 1 + 0.35f * e;
				f.squash_y = 1 - 0.3f * e;
			}
		} else {
			// Fading in as it comes in from the side (a bonk: over its
			// head).
			f.opacity = smooth(it.at / (it.t.above ? 0.15f : 0.08f));
		}
		out.flying.push_back(f);
	}

	// What it did as things hit: bits in the air, what's left on its face.
	step_effects(dt, view, out);

	// The flashes.
	flashes_.erase(std::remove_if(flashes_.begin(), flashes_.end(),
				      [&](const Flash &f) { return time_ - f.at >= FLASH_TIME; }),
		       flashes_.end());
	for (const Flash &f : flashes_) {
		const float u = (time_ - f.at) / FLASH_TIME;
		Burst b;
		b.x = f.x;
		b.y = f.y;
		b.size = f.size * (0.6f + 0.8f * smooth(u * 1.4f));
		b.opacity = 1 - smooth(u);
		out.bursts.push_back(b);
	}

	// The "ouch".
	float squint = 0;
	if (ouch_ >= 0) {
		ouch_ += dt;
		if (ouch_ < OUCH_SHUT)
			squint = smooth(ouch_ / OUCH_SHUT);
		else if (ouch_ < OUCH_SHUT + OUCH_HOLD)
			squint = 1;
		else if (ouch_ < OUCH_SHUT + OUCH_HOLD + OUCH_OPEN)
			squint = 1 - smooth((ouch_ - OUCH_SHUT - OUCH_HOLD) / OUCH_OPEN);
		else
			ouch_ = -1;
	}

	// The knock-back springs, in small steps so they never run away.
	{
		const float stiff = KNOCK_W * KNOCK_W, damp = 2 * KNOCK_DAMP * KNOCK_W;
		float left = dt;
		while (left > 0) {
			const float h = std::min(left, 1.0f / 240);
			for (Spring *s : {&yaw_, &pitch_, &roll_, &dx_, &dy_})
				s->step(h, stiff, damp);
			left -= h;
		}
		auto hold = [](Spring &s, float most) {
			if (std::fabs(s.value) > most) {
				s.value = s.value > 0 ? most : -most;
				s.speed = 0;
			}
		};
		hold(yaw_, MOST_YAW);
		hold(pitch_, MOST_PITCH);
		hold(roll_, MOST_ROLL);
		hold(dx_, MOST_MOVE);
		hold(dy_, MOST_MOVE);
		out.yaw = yaw_.value;
		out.pitch = pitch_.value;
		out.roll = roll_.value;
		out.dx = dx_.value;
		out.dy = dy_.value;
	}

	// Faces.
	faces_.erase(std::remove_if(faces_.begin(), faces_.end(),
				    [&](const Cue &c) {
					    return time_ >= c.at + c.length + FACE_OUT ||
						   (c.out_at >= 0 && time_ >= c.out_at + TAKE_OVER);
				    }),
		     faces_.end());
	for (const Cue &c : faces_) {
		float w = smooth((time_ - c.at) / FACE_IN) * (1 - smooth((time_ - c.at - c.length) / FACE_OUT));
		if (c.out_at >= 0)
			w *= 1 - smooth((time_ - c.out_at) / TAKE_OVER);
		if (w * c.amount <= 0.001f)
			continue;
		// The same face twice (one easing out, one in): the more of the two.
		bool merged = false;
		for (FaceNow &f : out.faces)
			if (f.name == c.name) {
				f.amount = std::max(f.amount, w * c.amount);
				merged = true;
			}
		if (!merged)
			out.faces.push_back({c.name, w * c.amount});
	}

	// Moves: each one's head part (any character), and its body part for
	// the rig.
	acts_.erase(std::remove_if(acts_.begin(), acts_.end(),
				   [&](const Act &a) {
					   return time_ >= a.at + a.length ||
						  (a.out_at >= 0 && time_ >= a.out_at + MOVE_TAKE_OVER);
				   }),
		    acts_.end());
	for (const Act &a : acts_) {
		const float u = time_ - a.at, L = a.length, p = clamp01(u / L);
		const float take = a.out_at >= 0 ? 1 - smooth((time_ - a.out_at) / MOVE_TAKE_OVER) : 1.0f;
		const float env = std::sin(PI_F * p); // 0 at both ends
		float yaw = 0, pitch = 0, roll = 0, dx = 0, dy = 0;
		switch (a.builtin) {
		case BuiltIn::Nod:
			// Down and up twice, the second smaller.
			pitch = -13.0f * sq(std::sin(2 * PI_F * p)) * (1 - 0.35f * p);
			break;
		case BuiltIn::Shake:
			yaw = 15.0f * std::sin(4 * PI_F * p) * env;
			break;
		case BuiltIn::Tilt: {
			const float hold = smooth(u / 0.35f) * smooth((L - u) / 0.5f);
			roll = 15.0f * hold;
			yaw = 4.0f * hold;
			pitch = 3.0f * hold;
			break;
		}
		case BuiltIn::Bounce:
			dy = -0.09f * sq(std::sin(2 * PI_F * p));
			pitch = 3.0f * sq(std::sin(2 * PI_F * p));
			break;
		case BuiltIn::Wave:
			// Swaying a little as it waves (all a picture character does).
			roll = (4.0f + 3.0f * std::sin(3 * PI_F * p)) * env;
			break;
		case BuiltIn::Cheer:
			dy = -0.08f * sq(std::sin(3 * PI_F * p)) * env;
			pitch = 9.0f * env;
			break;
		default:
			break;
		}
		out.yaw += yaw * take;
		out.pitch += pitch * take;
		out.roll += roll * take;
		out.dx += dx * take;
		out.dy += dy * take;
		if (a.id >= 0 || builtin_arms(a.builtin)) {
			BodyMove m;
			m.builtin = a.id >= 0 ? BuiltIn::None : a.builtin;
			m.move = a.id;
			m.time = u;
			m.amount = envelope(u, L) * take;
			if (m.amount > 0.0005f)
				out.body.push_back(m);
		}
	}

	// The headpat.
	if (patting_) {
		pat_t_ += dt;
		const float t = pat_t_;
		if (!leaving_ && t >= pat_leave_) {
			// Leaving from just as it is now (mid-pat, if stopped).
			leaving_ = true;
			leave_from_ = shown_ ? last_pose_ : pat_pose(0);
		}
		PatPose p;
		if (!leaving_) {
			p = pat_pose(t);
			// Each pat bounces its hair as it lands.
			const int landed = t < PAT_ENTER ? 0 : 1 + (int)((t - PAT_ENTER) / PAT_TIME);
			if (landed > pats_done_) {
				pats_done_ = landed;
				out.knock = out.knock + V3{0, -0.8f * pat_.size, 0};
			}
		} else {
			// Up and away to its side, fading as it goes.
			const float u = clamp01((t - pat_leave_) / PAT_LEAVE), e = smooth(u);
			const PatPose &f = leave_from_;
			p.away = f.away + (PAT_FROM_AWAY - f.away) * e;
			p.rise = f.rise + (PAT_FROM_RISE - f.rise) * e;
			p.lift = f.lift + (0.6f - f.lift) * e;
			p.opacity = f.opacity * (1 - smooth((u - 0.4f) / 0.6f));
			p.squint = f.squint;
			p.press = f.press * (1 - smooth((t - pat_leave_) / 0.15f));
			p.lean = f.lean * (1 - e);
			if (u >= 1) {
				patting_ = false;
				// Its eyes stay shut happily a moment.
				after_ = f.squint > 0.001f ? hurry_after_ ? PAT_HAPPY_HOLD : 0 : -1;
				p = PatPose();
			}
		}
		if (blend_ < 1 && patting_) {
			// Taking over from the last hand.
			blend_ = std::min(1.0f, blend_ + dt / 0.2f);
			const float w = smooth(blend_);
			auto mix = [w](float a, float b) { return a + (b - a) * w; };
			p.away = mix(from_.away, p.away);
			p.rise = mix(from_.rise, p.rise);
			p.lift = mix(from_.lift, p.lift);
			p.opacity = mix(from_.opacity, p.opacity);
			p.press = mix(from_.press, p.press);
			p.squint = mix(from_.squint, p.squint);
			p.lean = mix(from_.lean, p.lean);
		}
		if (patting_) {
			Hand &h = out.hand;
			h.shown = true;
			h.picture = pat_.picture;
			h.side = pat_.side;
			h.size = pat_.size;
			h.opacity = p.opacity;
			h.rise = p.rise;
			h.away = p.away;
			h.lift = p.lift;
			// A picture of it: on top of its head - but a head up to the
			// picture's top edge (a character filling it, as most do) would
			// leave it outside it: then it comes down far enough that some
			// of it shows (never lower than its eyebrows). It goes down
			// with its head.
			h.across = 0.85f * pat_.size;
			const float hand_tall =
				h.across * clampf(finite(view.hand_tall) ? view.hand_tall : 1.25f, 0.2f, 5);
			const float rest =
				std::max(-view.head_top, std::min(view.top + PAT_SHOWS * hand_tall, PAT_LOWEST));
			h.x = pat_.side * p.away;
			h.y = rest - p.rise + PAT_PRESS * p.press;
			h.angle = pat_.side * PAT_TURN * p.lift;
		}
		out.dy += PAT_PRESS * p.press;
		out.pitch -= PAT_NOD * p.press;
		// Leaning into the hand: the top of its head towards its side.
		out.roll += pat_.side * PAT_LEAN * p.lean;
		squint = std::max(squint, p.squint);
		last_pose_ = p;
	}
	// The squish: each pat's give, on a spring (in small steps, steady at
	// any frame rate).
	{
		const float target = patting_ ? last_pose_.press : 0.0f;
		for (float left = dt; left > 1e-6f;) {
			const float h = std::min(left, 1.0f / 240);
			squish_speed_ += (-SQUISH_W * SQUISH_W * (squish_ - target) - 2 * SQUISH_DAMP * SQUISH_W * squish_speed_) * h;
			squish_ += squish_speed_ * h;
			left -= h;
		}
		if (!patting_ && std::fabs(squish_) < 2e-3f && std::fabs(squish_speed_) < 0.05f)
			squish_ = squish_speed_ = 0;
		out.squish = clampf(squish_, -0.5f, 1.2f);
	}
	// After a headpat: its eyes kept shut a moment, then opening.
	if (after_ >= 0) {
		after_ += dt;
		squint = std::max(squint, PAT_SQUINT * (1 - smooth((after_ - PAT_HAPPY_HOLD) / PAT_HAPPY_OPEN)));
		if (after_ >= PAT_HAPPY_HOLD + PAT_HAPPY_OPEN)
			after_ = -1;
	}
	// What's stuck to it moves with its head as these moves move it (and
	// tips with it) - and with its own tilt since (ReactView::roll).
	{
		const bool moved = !view.still_head;
		const float knock = moved ? out.roll * PI_F / 180 : 0;
		auto follow = [&](float &x, float &y, float &angle, float since) {
			const float a = knock + since, co = std::cos(a), si = std::sin(a);
			const float x0 = x, y0 = y;
			x = x0 * co - y0 * si + (moved ? out.dx : 0);
			y = x0 * si + y0 * co + (moved ? out.dy : 0);
			angle += a;
		};
		size_t n = 0;
		for (Flying &f : out.flying) {
			// (out.flying is in flying_'s order.)
			const Item &it = flying_[n++];
			if (f.stuck)
				follow(f.x, f.y, f.angle, roll_now_ - it.stuck_roll);
		}
		// What's left on its face: a splat spreading out as it lands, a
		// touch past its size, settling; its drips running down; at the
		// end of its stay sliding down a little as it fades.
		for (const MarkItem &m : marks_) {
			Mark o;
			o.picture = m.picture;
			o.x = m.x;
			o.y = m.y;
			o.size = m.size;
			o.angle = m.angle;
			float fade = 1;
			const float end = m.at - m.stay;
			if (end > 0) {
				fade = 1 - smooth(end / MARK_FADE);
				o.y += MARK_SLIDE * smooth(end / MARK_FADE);
			}
			if (m.fade_at >= 0)
				fade = std::min(fade, 1 - smooth((m.at - m.fade_at) / m.fade_time));
			if (m.drip) {
				const float q = clamp01((m.at - m.wait) / m.run_time);
				if (q <= 0)
					continue;
				o.from_top = true;
				o.angle = 0;
				// Fast at first, slowing as it runs out.
				o.length = std::max(o.size * 0.6f, m.run * (1 - sq(1 - q)) + o.size);
				fade *= smooth((m.at - m.wait) / 0.08f);
			} else if (m.grow < 1) {
				const float u = m.at / SPLAT_SPREAD;
				o.size *= u < 1 ? 0.55f + 0.53f * smooth(u)
						: 1.08f - 0.08f * smooth((m.at - SPLAT_SPREAD) / 0.12f);
			}
			o.opacity = clamp01(fade);
			if (o.opacity <= 0.001f)
				continue;
			follow(o.x, o.y, o.angle, roll_now_ - m.roll);
			out.marks.push_back(o);
		}
	}
	shown_ = out.hand.shown;
	out.squint = squint;

	const bool settling = std::fabs(yaw_.value) + std::fabs(pitch_.value) + std::fabs(roll_.value) > 0.01f ||
			      std::fabs(yaw_.speed) + std::fabs(pitch_.speed) + std::fabs(roll_.speed) > 0.05f ||
			      std::fabs(dx_.value) + std::fabs(dy_.value) > 1e-4f ||
			      std::fabs(dx_.speed) + std::fabs(dy_.speed) > 1e-3f;
	if (!settling)
		yaw_ = pitch_ = roll_ = dx_ = dy_ = Spring();
	out.busy = settling || !faces_.empty() || !acts_.empty() || !flying_.empty() || !waiting_.empty() || patting_ ||
		   !flashes_.empty() || ouch_ >= 0 || out.hits > 0 || after_ >= 0 || squish_ != 0 ||
		   squish_speed_ != 0 || !marks_.empty() || !bits_.empty();
	return out;
}

} // namespace direbot::face
