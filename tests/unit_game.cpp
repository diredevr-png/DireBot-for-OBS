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

// Gaming hands (src/face/devices.hpp, gamehands.hpp): your keyboard, mouse
// and pads counted - never which key - and a character's hands on its own
// keyboard, mouse and pad as you use yours: which it's on, typing, the
// mouse, the pad, mirrored or not, resting, gliding, its desk's box and
// "Show me".

#include "../src/face/devices.hpp"
#include "../src/face/gamehands.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace direbot::face;

static int failures = 0;
static int passes = 0;

#define CHECK(cond)                                                                      \
	do {                                                                             \
		if (cond) {                                                              \
			passes++;                                                        \
		} else {                                                                 \
			failures++;                                                      \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                        \
	} while (0)

static bool near(double a, double b, double by)
{
	return std::fabs(a - b) <= by;
}

static bool finite3(V3 v)
{
	return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// ---- Counting them ----------------------------------------------------------------

// What's counted is the same whichever keys they were: nothing in it says
// which (a password typed and anything else as long give the same).
static bool same_state(const DeviceState &a, const DeviceState &b)
{
	return a.key_presses == b.key_presses && a.keys_held == b.keys_held && a.key_at == b.key_at &&
	       a.mouse_x == b.mouse_x && a.mouse_y == b.mouse_y && a.clicks == b.clicks &&
	       a.mouse_buttons == b.mouse_buttons && a.wheel == b.wheel && a.mouse_at == b.mouse_at;
}

static void test_tally_keys()
{
	DeviceTally t;
	CHECK(t.state().key_presses == 0 && t.state().keys_held == 0 && t.state().key_at < 0);
	// A key down, held (repeating), up: one press.
	t.key(17, true, 1.0);
	t.key(17, true, 1.03);
	t.key(17, true, 1.06);
	CHECK(t.state().key_presses == 1 && t.state().keys_held == 1 && t.held(17));
	CHECK(t.state().key_at == 1.0); // (a repeat isn't a use)
	t.key(17, false, 1.2);
	CHECK(t.state().key_presses == 1 && t.state().keys_held == 0 && !t.held(17));
	CHECK(t.state().key_at == 1.2);
	// Two held at once, up one at a time; a key up it never saw go down
	// changes nothing.
	t.key(30, true, 2.0);
	t.key(32, true, 2.1);
	CHECK(t.state().key_presses == 3 && t.state().keys_held == 2);
	t.key(99, false, 2.2);
	CHECK(t.state().keys_held == 2 && t.state().key_at == 2.1);
	t.key(30, false, 2.3);
	CHECK(t.state().keys_held == 1 && t.held(32) && !t.held(30));
	// Everything up at once (the PC locked): none held, the presses kept.
	t.keys_up();
	CHECK(t.state().keys_held == 0 && !t.held(32) && t.state().key_presses == 3);
	t.key(32, false, 2.4);
	CHECK(t.state().keys_held == 0);
	// Out of range: nothing.
	t.key(-1, true, 3);
	t.key(DeviceTally::SLOTS, true, 3);
	CHECK(t.state().key_presses == 3 && !t.held(-1) && !t.held(DeviceTally::SLOTS));
	// An E0 key (256 on) is its own.
	t.key(256 + 29, true, 4);
	t.key(29, true, 4.1);
	CHECK(t.state().keys_held == 2 && t.state().key_presses == 5);

	// Privacy: "hunter2" and seven other keys at the same times leave
	// exactly the same behind.
	const int word[] = {35, 22, 49, 20, 18, 19, 3};
	const int other[] = {44, 45, 46, 47, 48, 50, 51};
	DeviceTally a, b;
	for (int i = 0; i < 7; i++) {
		a.key(word[i], true, 10 + i * 0.1);
		b.key(other[i], true, 10 + i * 0.1);
		a.key(word[i], false, 10.05 + i * 0.1);
		b.key(other[i], false, 10.05 + i * 0.1);
	}
	CHECK(same_state(a.state(), b.state()));
	CHECK(a.state().key_presses == 7 && a.state().keys_held == 0);
	// ...and nothing of them is held once they're up.
	bool any = false;
	for (int s = 0; s < DeviceTally::SLOTS; s++)
		any = any || a.held(s);
	CHECK(!any);
}

static void test_tally_mouse()
{
	DeviceTally t;
	t.mouse_move(3, -2, 1);
	t.mouse_move(4, 1, 1.1);
	CHECK(t.state().mouse_x == 7 && t.state().mouse_y == -1 && t.state().mouse_at == 1.1);
	// Not moved, or nonsense: nothing.
	t.mouse_move(0, 0, 2);
	t.mouse_move(NAN, 1, 2);
	t.mouse_move(1, INFINITY, 2);
	CHECK(t.state().mouse_x == 7 && t.state().mouse_y == -1 && t.state().mouse_at == 1.1);
	// A click counts as it goes down; held, its button's bit.
	t.mouse_button(0, true, 3);
	t.mouse_button(0, true, 3.1); // (down again without an up: not another)
	CHECK(t.state().clicks == 1 && t.state().mouse_buttons == 1);
	t.mouse_button(1, true, 3.2);
	CHECK(t.state().clicks == 2 && t.state().mouse_buttons == 3);
	t.mouse_button(0, false, 3.3);
	CHECK(t.state().mouse_buttons == 2 && t.state().mouse_at == 3.3);
	t.mouse_button(4, true, 3.4);
	CHECK(t.state().mouse_buttons == (2u | 16u) && t.state().clicks == 3);
	t.mouse_button(5, true, 3.5);
	t.mouse_button(-1, true, 3.5);
	CHECK(t.state().clicks == 3 && t.state().mouse_at == 3.4);
	// The wheel, summed.
	t.wheel(1, 4);
	t.wheel(-3, 4.1);
	t.wheel(0, 4.2);
	CHECK(t.state().wheel == -2 && t.state().mouse_at == 4.1);
}

static void test_tally_pads()
{
	// XInput's numbers: inside a stick's dead zone it's still; all the way
	// it's 1; a trigger's first touch is nothing.
	PadState p = pad_from_xinput(0, 0, 0, 7000, 0, 0, 8000);
	CHECK(p.connected && p.left_x == 0 && p.left_y == 0 && p.right_y == 0);
	p = pad_from_xinput(0, 0, 0, 32767, 0, 0, -32768);
	CHECK(near(p.left_x, 1, 1e-4) && near(p.left_y, 0, 1e-6) && near(p.right_y, -1, 1e-4));
	// Halfway past the dead zone, along a diagonal: its way kept.
	const float d = 7849 + (32767 - 7849) * 0.5f;
	p = pad_from_xinput(0, 0, 0, (int16_t)(d / std::sqrt(2.0f)), (int16_t)(d / std::sqrt(2.0f)), 0, 0);
	CHECK(near(std::hypot(p.left_x, p.left_y), 0.5, 0.01) && near(p.left_x, p.left_y, 1e-3));
	p = pad_from_xinput(0, 30, 255, 0, 0, 0, 0);
	CHECK(p.left_trigger == 0 && near(p.right_trigger, 1, 1e-6));
	p = pad_from_xinput(0, 0, 142, 0, 0, 0, 0);
	CHECK(near(p.right_trigger, (142 - 30) / 225.0, 1e-5));
	// Its buttons as XInput has them - anything else (its guide button,
	// others' extras) left out.
	p = pad_from_xinput(PAD_A | PAD_LEFT_BUMPER | 0x0400, 0, 0, 0, 0, 0, 0);
	CHECK(p.buttons == (PAD_A | PAD_LEFT_BUMPER));
	// The same as far as the hands go.
	PadState q = p;
	q.left_x = 0.015f;
	CHECK(same_pad(p, q));
	q.left_x = 0.03f;
	CHECK(!same_pad(p, q));
	q = p;
	q.buttons |= PAD_B;
	CHECK(!same_pad(p, q));

	// Used: when it changes as far as the hands go - not while it sits
	// still, nor a stick creeping by less than that each time.
	DeviceTally t;
	PadState a;
	a.connected = true;
	t.pad(a, 1, 1);
	CHECK(t.state().pads_in == 1 && t.state().pad.connected && t.state().pad_at == 1);
	t.pad(a, 1, 2);
	CHECK(t.state().pad_at == 1);
	a.left_x = 0.01f;
	t.pad(a, 1, 3);
	a.left_x = 0.019f;
	t.pad(a, 1, 4);
	CHECK(t.state().pad_at == 1 && near(t.state().pad.left_x, 0.019, 1e-6));
	a.left_x = 0.025f;
	t.pad(a, 1, 5);
	CHECK(t.state().pad_at == 5);
	a.buttons = PAD_X;
	t.pad(a, 2, 6);
	CHECK(t.state().pad_at == 6 && t.state().pads_in == 2);
	// Unplugged: not connected, nothing used; plugged in again it's used.
	t.pad(PadState(), 0, 7);
	CHECK(!t.state().pad.connected && t.state().pads_in == 0 && t.state().pad_at == 6);
	t.pad(a, 1, 8);
	CHECK(t.state().pad_at == 8);
}

static void test_reader_here()
{
	// Off Windows nothing can be read, and it says so; it's shared while
	// it's held.
	std::shared_ptr<DeviceReader> r = DeviceReader::open(), r2 = DeviceReader::open();
	CHECK(r && r == r2);
	const DeviceState s = r->state();
#ifndef _WIN32
	CHECK(!s.keyboard && !s.mouse && !s.pads);
	CHECK(!s.keyboard_problem.empty() && !s.mouse_problem.empty() && !s.pads_problem.empty());
#endif
	CHECK(s.key_presses == 0 && s.clicks == 0);
	const double c0 = DeviceReader::clock(), c1 = DeviceReader::clock();
	CHECK(c1 >= c0);
	const DeviceReader *was = r.get();
	r.reset();
	r2.reset();
	std::shared_ptr<DeviceReader> r3 = DeviceReader::open();
	CHECK(r3 != nullptr);
	(void)was;
}

// ---- Its hands --------------------------------------------------------------------

// A character as tall as a person: shoulders 36 cm apart at 1.4 m, its arms
// 0.55 m - or half that size (its own units).
static DeskBody body_of(float size = 1)
{
	DeskBody b;
	b.ok = true;
	b.shoulder[0] = V3{0.18f, 1.40f, 0} * size;
	b.shoulder[1] = V3{-0.18f, 1.40f, 0} * size;
	b.arm = 0.55f * size;
	return b;
}

// Runs it `seconds` at 60 a second from `t`, `s` changed by `change` each
// frame (given the time).
struct Run {
	GameHands g;
	DeviceState s;
	double t = 0;
	template <class F> void go(double seconds, F change)
	{
		const int frames = (int)std::lround(seconds * 60);
		for (int i = 0; i < frames; i++) {
			t += 1.0 / 60;
			change(s, t);
			g.step(s, t, 1.0f / 60);
		}
	}
	void wait(double seconds)
	{
		go(seconds, [](DeviceState &, double) {});
	}
	void press(int keys = 1)
	{
		go(1.0 / 60, [&](DeviceState &st, double now) {
			st.key_presses += (uint64_t)keys;
			st.key_at = now;
		});
	}
};

static V3 place_of(const M4 &m)
{
	return {m.m[12], m.m[13], m.m[14]};
}

static int fingers_down(const DeskHand &h, const float rest[5][3])
{
	int n = 0;
	for (int i = 0; i < 5; i++)
		if (h.bend[i][0] > rest[i][0] + 0.12f)
			n++;
	return n;
}

static void test_no_body()
{
	GameHands g;
	DeviceState s;
	g.step(s, 1, 1.0f / 60);
	CHECK(g.now().hand[0].weight == 0 && g.now().hand[1].weight == 0);
	for (int i = 0; i < DESK_PROPS; i++)
		CHECK(g.now().prop[i].opacity == 0);
	V3 lo, hi;
	CHECK(!desk_box(DeskBody(), GameOptions(), lo, hi));
}

static void test_keyboard_mouse()
{
	const DeskBody b = body_of();
	const V3 mid = (b.shoulder[0] + b.shoulder[1]) * 0.5f;
	// Mirrored, your mouse in your right hand: your reflection's - its
	// left hand on the mouse (on its left), its right on the keyboard.
	Run r;
	r.g.set_body(b);
	r.wait(1);
	const DeskNow &n = r.g.now();
	CHECK(n.device == GameDevice::KeyboardMouse && !n.typing);
	CHECK(near(n.hand[0].weight, 1, 1e-6) && near(n.hand[1].weight, 1, 1e-6) && near(n.on, 1, 1e-6));
	const V3 mouse = place_of(n.prop[DESK_MOUSE].place), keyboard = place_of(n.prop[DESK_KEYBOARD].place);
	CHECK(mouse.x > mid.x + 0.15f && keyboard.x < mid.x);
	CHECK(n.prop[DESK_MOUSE].place.m[0] < 0); // (its keyboard and mouse mirrored too)
	CHECK(n.hand[0].wrist.x > mid.x + 0.15f && n.hand[1].wrist.x < mid.x + 0.05f);
	// The desk in front of it, below its shoulders; its hands on it, in reach.
	CHECK(keyboard.y < mid.y - 0.2f && keyboard.z > 0.2f);
	for (int side = 0; side < 2; side++) {
		const DeskHand &h = n.hand[side];
		CHECK(finite3(h.wrist) && near(length(h.along), 1, 1e-4) && near(length(h.across), 1, 1e-4));
		CHECK(near(dot(h.along, h.across), 0, 1e-3));
		const float reach = length(h.wrist - b.shoulder[side]);
		CHECK(reach > 0.45f * b.arm && reach < 0.95f * b.arm);
		CHECK(h.wrist.y > keyboard.y && h.wrist.y < keyboard.y + 0.12f);
	}
	// Its props shown.
	CHECK(n.prop[DESK_KEYBOARD].opacity == 1 && n.prop[DESK_MOUSE].opacity == 1 && n.prop[DESK_MAT].opacity == 1);
	CHECK(n.prop[DESK_PAD].opacity == 0);

	// Not mirrored: as you are - its right hand on the mouse, on its right.
	Run u;
	GameOptions o;
	o.mirror = false;
	u.g.set_options(o);
	u.g.set_body(b);
	u.wait(1);
	CHECK(place_of(u.g.now().prop[DESK_MOUSE].place).x < mid.x - 0.15f);
	CHECK(u.g.now().prop[DESK_MOUSE].place.m[0] > 0);
	CHECK(u.g.now().hand[1].wrist.x < mid.x - 0.15f);
	// Your mouse in your left hand, mirrored: on its right.
	Run lft;
	o.mirror = true;
	o.mouse_right = false;
	lft.g.set_options(o);
	lft.g.set_body(b);
	lft.wait(1);
	CHECK(place_of(lft.g.now().prop[DESK_MOUSE].place).x < mid.x - 0.15f);

	// A character half the size: everything half as far from its shoulders.
	Run h;
	h.g.set_body(body_of(0.5f));
	h.wait(1);
	const V3 hmid = mid * 0.5f;
	CHECK(near(length(place_of(h.g.now().prop[DESK_MOUSE].place) - hmid), 0.5f * length(mouse - mid), 1e-4));
	CHECK(near(length(h.g.now().hand[1].wrist - hmid), 0.5f * length(n.hand[1].wrist - mid), 1e-3));
	CHECK(near(h.g.now().prop[DESK_KEYBOARD].place.m[5], 0.5, 1e-5));

	// Its desk raised and moved away (shares of its arm).
	Run up;
	GameOptions oh;
	oh.height = 0.2f;
	oh.distance = 0.1f;
	up.g.set_options(oh);
	up.g.set_body(b);
	up.wait(1);
	const V3 raised = place_of(up.g.now().prop[DESK_KEYBOARD].place);
	CHECK(near(raised.y - keyboard.y, 0.2 * 0.55, 1e-4) && near(raised.z - keyboard.z, 0.1 * 0.55, 1e-4));
	CHECK(near(up.g.now().hand[1].wrist.y - n.hand[1].wrist.y, 0.2 * 0.55, 0.01));
	// No props: hands there, nothing drawn.
	Run np;
	GameOptions on;
	on.props = false;
	np.g.set_options(on);
	np.g.set_body(b);
	np.wait(1);
	CHECK(np.g.now().hand[1].weight == 1);
	for (int i = 0; i < DESK_PROPS; i++)
		CHECK(np.g.now().prop[i].opacity == 0);
}

static void test_keys()
{
	static const float NONE[5][3] = {};
	const DeskBody b = body_of();
	Run r;
	r.g.set_body(b);
	r.wait(1);
	// Mirrored and right-handed: the keyboard's under its right hand.
	const DeskHand &kb = r.g.now().hand[1];
	float rest[5][3];
	for (int i = 0; i < 5; i++)
		for (int j = 0; j < 3; j++)
			rest[i][j] = kb.bend[i][j];
	CHECK(fingers_down(kb, rest) == 0);
	// A key tapped: a finger of that hand goes down and comes back up.
	r.press();
	r.wait(0.05);
	CHECK(fingers_down(r.g.now().hand[1], rest) == 1);
	CHECK(fingers_down(r.g.now().hand[0], NONE) <= 5); // (the mouse hand's as it was)
	r.wait(0.5);
	CHECK(fingers_down(r.g.now().hand[1], rest) == 0);
	CHECK(r.g.now().keys); // used in the last second
	r.wait(1.2);
	CHECK(!r.g.now().keys);
	// W held (and D with it): two fingers held down while they're held,
	// up as they come up - the oldest first.
	r.go(1.0 / 60, [](DeviceState &s, double now) {
		s.key_presses++;
		s.keys_held = 1;
		s.key_at = now;
	});
	r.wait(0.3);
	CHECK(fingers_down(r.g.now().hand[1], rest) == 1);
	r.go(1.0 / 60, [](DeviceState &s, double now) {
		s.key_presses++;
		s.keys_held = 2;
		s.key_at = now;
	});
	r.wait(0.5);
	CHECK(fingers_down(r.g.now().hand[1], rest) == 2);
	r.go(1.0 / 60, [](DeviceState &s, double) { s.keys_held = 1; });
	r.wait(0.3);
	CHECK(fingers_down(r.g.now().hand[1], rest) == 1);
	r.go(1.0 / 60, [](DeviceState &s, double) { s.keys_held = 0; });
	r.wait(0.3);
	CHECK(fingers_down(r.g.now().hand[1], rest) == 0);
	// The same presses give the same hands (its own randomness is the
	// same every run).
	Run a, c;
	for (Run *x : {&a, &c}) {
		x->g.set_body(b);
		x->wait(0.2);
		for (int i = 0; i < 12; i++) {
			x->press();
			x->wait(0.1);
		}
	}
	bool same = true;
	for (int side = 0; side < 2; side++)
		for (int i = 0; i < 5; i++)
			for (int j = 0; j < 3; j++)
				same = same && a.g.now().hand[side].bend[i][j] == c.g.now().hand[side].bend[i][j];
	CHECK(same && length(a.g.now().hand[1].wrist - c.g.now().hand[1].wrist) == 0);
}

static void test_typing()
{
	const DeskBody b = body_of();
	Run r;
	r.g.set_body(b);
	r.wait(1);
	const V3 mouse_hand = r.g.now().hand[0].wrist, key_hand = r.g.now().hand[1].wrist;
	// Typing away, the mouse still: both hands type.
	for (int i = 0; i < 10; i++) {
		r.press();
		r.wait(0.12);
	}
	CHECK(r.g.now().typing);
	r.wait(0.5);
	const V3 typing_hand = r.g.now().hand[0].wrist;
	CHECK(length(typing_hand - mouse_hand) > 0.12f);
	CHECK(std::fabs(typing_hand.x - key_hand.x) < 0.26f && std::fabs(typing_hand.y - key_hand.y) < 0.03f);
	// Its keys pressed with either hand now.
	int left_downs = 0;
	for (int i = 0; i < 20; i++) {
		r.press();
		r.wait(0.03);
		if (r.g.now().hand[0].bend[1][0] > 0.4f || r.g.now().hand[0].bend[2][0] > 0.4f)
			left_downs++;
		r.wait(0.1);
	}
	CHECK(left_downs > 2);
	// The mouse moved: that hand back on it.
	r.go(0.3, [](DeviceState &s, double now) {
		s.mouse_x += 30;
		s.mouse_at = now;
	});
	CHECK(!r.g.now().typing);
	r.wait(1);
	CHECK(length(r.g.now().hand[0].wrist - mouse_hand) < 0.06f);
	// Typing stops for a while: it stops typing.
	for (int i = 0; i < 10; i++) {
		r.press();
		r.wait(0.12);
	}
	CHECK(r.g.now().typing);
	r.wait(5);
	CHECK(!r.g.now().typing);
}

static void test_mouse()
{
	const DeskBody b = body_of();
	for (bool mirror : {true, false}) {
		Run r;
		GameOptions o;
		o.mirror = mirror;
		r.g.set_options(o);
		r.g.set_body(b);
		r.wait(1);
		const V3 at = place_of(r.g.now().prop[DESK_MOUSE].place);
		const int hand = mirror ? 0 : 1;
		const V3 wrist = r.g.now().hand[hand].wrist;
		// Yours moved right: its own moves right as the picture shows it
		// (mirrored: to its left, +x; not: to its right).
		r.go(0.25, [](DeviceState &s, double now) {
			s.mouse_x += 40;
			s.mouse_at = now;
		});
		r.wait(0.4);
		const V3 moved = place_of(r.g.now().prop[DESK_MOUSE].place);
		CHECK(mirror ? moved.x > at.x + 0.012f : moved.x < at.x - 0.012f);
		const V3 w2 = r.g.now().hand[hand].wrist;
		CHECK(mirror ? w2.x > wrist.x + 0.01f : w2.x < wrist.x - 0.01f);
		// Not past its mat, however far.
		r.go(1, [](DeviceState &s, double now) {
			s.mouse_x += 4000;
			s.mouse_at = now;
		});
		CHECK(std::fabs(place_of(r.g.now().prop[DESK_MOUSE].place).x - at.x) <= 0.061f);
		// Towards you: towards it.
		r.wait(20);
		const V3 back = place_of(r.g.now().prop[DESK_MOUSE].place);
		CHECK(std::fabs(back.x - at.x) < 0.005f); // (it drifts back to the middle)
		r.go(0.25, [](DeviceState &s, double now) {
			s.mouse_y += 40;
			s.mouse_at = now;
		});
		CHECK(place_of(r.g.now().prop[DESK_MOUSE].place).z < back.z - 0.01f);
		// A click: its first finger down while it's held; the right
		// button its second.
		r.wait(1);
		const float first = r.g.now().hand[hand].bend[1][0], second = r.g.now().hand[hand].bend[2][0];
		r.go(0.2, [](DeviceState &s, double now) {
			if (!(s.mouse_buttons & 1))
				s.clicks++;
			s.mouse_buttons = 1;
			s.mouse_at = now;
		});
		CHECK(r.g.now().hand[hand].bend[1][0] > first + 0.1f && near(r.g.now().hand[hand].bend[2][0], second, 0.01));
		r.go(0.2, [](DeviceState &s, double now) {
			s.mouse_buttons = 2;
			s.clicks++;
			s.mouse_at = now;
		});
		CHECK(near(r.g.now().hand[hand].bend[1][0], first, 0.02) && r.g.now().hand[hand].bend[2][0] > second + 0.1f);
		CHECK(r.g.now().mouse);
	}
}

static void pad_used(DeviceState &s, double now, float lx = 0.5f)
{
	s.pads_in = 1;
	s.pad.connected = true;
	s.pad.left_x = lx * (float)std::sin(now * 7);
	s.pad.left_y = lx * (float)std::cos(now * 7);
	s.pad_at = now;
}

static void test_pad()
{
	const DeskBody b = body_of();
	const V3 mid = (b.shoulder[0] + b.shoulder[1]) * 0.5f;
	Run r;
	r.g.set_body(b);
	r.wait(1);
	const V3 kb_wrist = r.g.now().hand[1].wrist;
	// A pad used a moment: not yet (a nudge doesn't take it).
	r.go(0.05, [](DeviceState &s, double now) { pad_used(s, now); });
	CHECK(r.g.now().device == GameDevice::KeyboardMouse);
	// Played: its hands go to it, gliding (not there in one go).
	r.go(1, [](DeviceState &s, double now) { pad_used(s, now); });
	CHECK(r.g.now().device == GameDevice::Pad);
	r.go(1, [](DeviceState &s, double now) { pad_used(s, now); });
	const DeskNow &n = r.g.now();
	CHECK(n.prop[DESK_PAD].opacity == 1 && n.prop[DESK_KEYBOARD].opacity == 0 && n.prop[DESK_MOUSE].opacity == 0);
	CHECK(n.prop[DESK_LEFT_STICK].opacity == 1 && n.prop[DESK_RIGHT_STICK].opacity == 1);
	CHECK(n.pad);
	const V3 pad = place_of(n.prop[DESK_PAD].place);
	CHECK(near(pad.x, mid.x, 0.03) && pad.y < mid.y - 0.1f && pad.z > 0.15f);
	// Both hands on it, either side of it.
	CHECK(n.hand[0].wrist.x > pad.x + 0.03f && n.hand[1].wrist.x < pad.x - 0.03f);
	for (int side = 0; side < 2; side++) {
		CHECK(length(n.hand[side].wrist - pad) < 0.16f);
		const float reach = length(n.hand[side].wrist - b.shoulder[side]);
		CHECK(reach > 0.4f * b.arm && reach < 0.95f * b.arm);
	}
	CHECK(length(n.hand[1].wrist - kb_wrist) > 0.06f);
	// Its left stick leans as yours does.
	r.go(0.3, [](DeviceState &s, double now) {
		pad_used(s, now, 0);
		s.pad.left_x = 1;
		s.pad.left_y = 0;
	});
	const M4 &stick = r.g.now().prop[DESK_LEFT_STICK].place, &body_pad = r.g.now().prop[DESK_PAD].place;
	// (Its up against the pad's up.)
	const V3 su{stick.m[4], stick.m[5], stick.m[6]}, pu{body_pad.m[4], body_pad.m[5], body_pad.m[6]};
	const float lean = std::acos(std::min(1.0f, dot(normalized(su), normalized(pu))));
	CHECK(near(lean, 20 * 3.14159265 / 180, 0.02));
	// A trigger held: its first finger squeezes; a bumper: straighter
	// (your left hand's: its right, mirrored).
	const float idx = r.g.now().hand[1].bend[1][0];
	r.go(0.3, [](DeviceState &s, double now) {
		pad_used(s, now, 0);
		s.pad.left_trigger = 1;
	});
	const float squeezed = r.g.now().hand[1].bend[1][0];
	r.go(0.3, [](DeviceState &s, double now) {
		pad_used(s, now, 0);
		s.pad.left_trigger = 0;
		s.pad.buttons = PAD_LEFT_BUMPER;
	});
	const float bumper = r.g.now().hand[1].bend[1][0];
	CHECK(squeezed > idx + 0.1f && bumper < idx - 0.1f);
	// Back on the keyboard and mouse once you use them.
	r.go(0.1, [](DeviceState &s, double) { s.pad.buttons = 0; });
	for (int i = 0; i < 4; i++) {
		r.press();
		r.wait(0.1);
	}
	CHECK(r.g.now().device == GameDevice::KeyboardMouse);
	r.wait(1);
	CHECK(r.g.now().prop[DESK_PAD].opacity == 0 && r.g.now().prop[DESK_KEYBOARD].opacity == 1);
	// ...or once the pad's unplugged.
	r.go(2, [](DeviceState &s, double now) { pad_used(s, now); });
	CHECK(r.g.now().device == GameDevice::Pad);
	r.go(0.1, [](DeviceState &s, double) {
		s.pad = PadState();
		s.pads_in = 0;
	});
	CHECK(r.g.now().device == GameDevice::KeyboardMouse);
	// Mirrored or not, its left thumb's stick is on its left (yours on
	// yours, as your reflection: the other way round).
	const V3 mirrored_stick = place_of(n.prop[DESK_LEFT_STICK].place);
	Run u;
	GameOptions o;
	o.mirror = false;
	o.device = 2;
	u.g.set_options(o);
	u.g.set_body(b);
	u.wait(1);
	CHECK(u.g.now().device == GameDevice::Pad);
	const V3 own_stick = place_of(u.g.now().prop[DESK_LEFT_STICK].place);
	CHECK(own_stick.x > mid.x && mirrored_stick.x < mid.x);
}

// The pad's size against its arms (a real hand's is 1).
static float pad_size(const DeskBody &b)
{
	Run r;
	GameOptions o;
	o.device = 2;
	r.g.set_options(o);
	r.g.set_body(b);
	r.wait(0.2);
	const M4 &m = r.g.now().prop[DESK_PAD].place;
	return length(V3{m.m[0], m.m[1], m.m[2]}) / (b.arm / 0.55f);
}

static void test_pad_hold()
{
	// The pad as big as its hands are for its arms: its palm, its
	// knuckles' width and its fingers' length, against a real hand's.
	DeskBody b = body_of();
	CHECK(near(pad_size(b), 1, 1e-4)); // (not known: a real one's)
	b.hand = 0.098f;
	b.knuckles = 0.063f;
	b.finger = 0.094f;
	CHECK(near(pad_size(b), 1, 1e-3));
	DeskBody big = b;
	big.hand *= 1.3f;
	big.knuckles *= 1.3f;
	big.finger *= 1.3f;
	CHECK(near(pad_size(big), 1.3, 1e-3));
	DeskBody half = body_of(0.5f);
	half.hand = 0.049f;
	half.knuckles = 0.0315f;
	half.finger = 0.047f;
	CHECK(near(pad_size(half), 1, 1e-3)); // (half the size all over: the same)
	DeskBody huge = b, tiny = b, odd = b;
	huge.hand = huge.knuckles = huge.finger = 1;
	tiny.hand = tiny.knuckles = tiny.finger = 0.001f;
	odd.hand = NAN;
	odd.knuckles = INFINITY;
	CHECK(near(pad_size(huge), 1.4, 1e-3) && near(pad_size(tiny), 0.7, 1e-3));
	CHECK(near(pad_size(odd), 0.094f / 0.094f, 1e-3)); // (just its fingers)

	// Its fingers reaching where they go on the pad (its own: not
	// mirrored): its thumb on its stick's top, its first finger on its
	// trigger's front, the others round under its grip on that side.
	Run r;
	GameOptions o;
	o.mirror = false;
	r.g.set_options(o);
	r.g.set_body(b);
	r.wait(1);
	for (int side = 0; side < 2; side++)
		for (int i = 0; i < 5; i++)
			CHECK(r.g.now().hand[side].reach_weight[i] == 0); // (on the keyboard and mouse: as they're bent)
	r.go(0.3, [](DeviceState &s, double now) { pad_used(s, now); });
	CHECK(r.g.now().device == GameDevice::Pad && r.g.now().hand[0].reach_weight[0] < 0.5f); // (on its way)
	r.go(1.5, [](DeviceState &s, double now) { pad_used(s, now, 0); });
	const DeskNow n = r.g.now();
	const M4 &pm = n.prop[DESK_PAD].place;
	const V3 pc = place_of(pm), px = normalized(V3{pm.m[0], pm.m[1], pm.m[2]}),
		 py = normalized(V3{pm.m[4], pm.m[5], pm.m[6]}), pz = normalized(V3{pm.m[8], pm.m[9], pm.m[10]});
	const V3 stick = place_of(n.prop[DESK_LEFT_STICK].place);
	for (int side = 0; side < 2; side++) {
		const DeskHand &h = n.hand[side];
		for (int i = 0; i < 5; i++)
			CHECK(h.reach_weight[i] > 0.99f && finite3(h.reach[i]));
		const float own = side == 0 ? 1.0f : -1.0f;
		CHECK(dot(h.reach[0] - pc, py) > 0.02f);   // thumb over its face
		CHECK(dot(h.reach[1] - pc, pz) > 0.04f);   // first finger at its far edge
		CHECK(own * dot(h.reach[1] - pc, px) > 0.03f);
		for (int i = 2; i < 5; i++) {
			CHECK(dot(h.reach[i] - pc, py) < -0.02f); // under it
			CHECK(own * dot(h.reach[i] - pc, px) > 0.01f); // its own side
			CHECK(dot(h.reach[i] - pc, pz) < 0); // along its grip
		}
	}
	CHECK(near(dot(n.hand[0].reach[0] - stick, py), 0.022, 0.002) && length(n.hand[0].reach[0] - stick) < 0.025f);
	// ...following its stick as it leans; on a bumper, its first finger up
	// on it; squeezing a trigger, in a little.
	r.go(0.2, [](DeviceState &s, double now) {
		pad_used(s, now, 0);
		s.pad.left_x = 1;
		s.pad.left_y = 0;
	});
	const V3 leant = r.g.now().hand[0].reach[0];
	const V3 top = place_of(r.g.now().prop[DESK_LEFT_STICK].place);
	CHECK(length(leant - n.hand[0].reach[0]) > 0.005f && length(leant - top) < 0.025f);
	r.go(0.2, [](DeviceState &s, double now) {
		pad_used(s, now, 0);
		s.pad.buttons = PAD_LEFT_BUMPER;
	});
	CHECK(dot(r.g.now().hand[0].reach[1] - n.hand[0].reach[1], py) > 0.015f);
	r.go(0.2, [](DeviceState &s, double now) {
		pad_used(s, now, 0);
		s.pad.buttons = 0;
		s.pad.left_trigger = 1;
	});
	CHECK(dot(r.g.now().hand[0].reach[1] - n.hand[0].reach[1], pz) < -0.004f);
	// Its thumb onto its d-pad when that's pressed (lower, nearer it),
	// down a little more while it's held.
	r.go(0.5, [](DeviceState &s, double now) {
		pad_used(s, now, 0);
		s.pad.buttons = PAD_UP;
	});
	const V3 dpad = r.g.now().hand[0].reach[0];
	CHECK(dot(dpad - n.hand[0].reach[0], pz) < -0.02f && dot(dpad - n.hand[0].reach[0], py) < 0);

	// A longer palm: its wrist further back from the same knuckles.
	DeskBody palm = b;
	palm.hand *= 1.3f;
	palm.knuckles *= 0.7f; // (the pad the same size)
	Run a, c;
	o.device = 2;
	a.g.set_options(o);
	c.g.set_options(o);
	a.g.set_body(b);
	c.g.set_body(palm);
	a.wait(2);
	c.wait(2);
	const float back = dot(a.g.now().hand[0].wrist - c.g.now().hand[0].wrist, pz);
	CHECK(near(pad_size(b), pad_size(palm), 1e-3) && back > 0.02f && back < 0.04f);
}

static void test_choice_and_rest()
{
	const DeskBody b = body_of();
	// Always the keyboard and mouse: a pad changes nothing.
	Run k;
	GameOptions o;
	o.device = 1;
	k.g.set_options(o);
	k.g.set_body(b);
	k.go(3, [](DeviceState &s, double now) { pad_used(s, now); });
	CHECK(k.g.now().device == GameDevice::KeyboardMouse && k.g.now().prop[DESK_PAD].opacity == 0);
	// Always a pad: on it from the first, keys or not.
	Run p;
	o.device = 2;
	p.g.set_options(o);
	p.g.set_body(b);
	p.g.step(DeviceState(), 0, 1.0f / 60);
	// (No keyboard for a moment first.)
	CHECK(p.g.now().prop[DESK_KEYBOARD].opacity == 0 && p.g.now().prop[DESK_PAD].opacity >= 0);
	p.wait(0.1);
	CHECK(p.g.now().device == GameDevice::Pad);
	for (int i = 0; i < 6; i++) {
		p.press();
		p.wait(0.1);
	}
	CHECK(p.g.now().device == GameDevice::Pad);
	// Resting after a while with nothing used, if asked: its hands its own
	// again (gliding off), the pad put down; back as soon as it's used.
	Run r;
	o = GameOptions();
	o.rest_after = 3;
	r.g.set_options(o);
	r.g.set_body(b);
	r.wait(1);
	CHECK(near(r.g.now().on, 1, 1e-6));
	r.wait(1.5);
	CHECK(near(r.g.now().on, 1, 1e-6));
	r.wait(1.2);
	CHECK(near(r.g.now().on, 0, 1e-6) && r.g.now().hand[0].weight == 0 && r.g.now().hand[1].weight == 0);
	CHECK(r.g.now().prop[DESK_KEYBOARD].opacity == 1); // (the desk's still there)
	r.press();
	r.wait(0.15);
	CHECK(r.g.now().on > 0.2f && r.g.now().on < 1);
	r.wait(0.6);
	CHECK(near(r.g.now().on, 1, 1e-6));
	// Never, by default.
	Run n;
	n.g.set_body(b);
	n.wait(60);
	CHECK(near(n.g.now().on, 1, 1e-6));
}

static void test_glide()
{
	// Switching to the pad, a hand glides there over about GLIDE seconds,
	// lifted on its way - no jumps from one frame to the next.
	const DeskBody b = body_of();
	Run r;
	r.g.set_body(b);
	r.wait(1);
	float most = 0, highest = -10;
	V3 was = r.g.now().hand[1].wrist;
	const float start_y = was.y;
	for (int i = 0; i < 120; i++) {
		r.go(1.0 / 60, [](DeviceState &s, double now) { pad_used(s, now); });
		const V3 w = r.g.now().hand[1].wrist;
		most = std::max(most, length(w - was));
		highest = std::max(highest, w.y);
		was = w;
	}
	CHECK(r.g.now().device == GameDevice::Pad);
	CHECK(most < 0.03f); // (under 1.8 m/s)
	CHECK(highest > std::max(start_y, was.y) + 0.002f);
	// Big gaps between frames (OBS stalls): still no jumps past where it goes.
	GameHands g;
	g.set_body(b);
	DeviceState s;
	g.step(s, 0, 0);
	const V3 a = g.now().hand[1].wrist;
	g.step(s, 5, 5);
	g.step(s, 5, -1);
	g.step(s, 5, NAN);
	CHECK(finite3(g.now().hand[1].wrist) && length(g.now().hand[1].wrist - a) < 0.01f);
}

static void test_reset()
{
	const DeskBody b = body_of();
	Run r;
	GameOptions o;
	o.rest_after = 2;
	r.g.set_options(o);
	r.g.set_body(b);
	r.go(2, [](DeviceState &s, double now) { pad_used(s, now); });
	CHECK(r.g.now().device == GameDevice::Pad);
	r.g.reset();
	CHECK(r.g.options().rest_after == 2);
	DeviceState fresh;
	r.g.step(fresh, 100, 1.0f / 60);
	CHECK(r.g.now().device == GameDevice::KeyboardMouse && r.g.now().hand[1].weight > 0);
}

static void test_desk_box()
{
	const DeskBody b = body_of();
	for (int k = 0; k < 4; k++) {
		GameOptions o;
		o.mirror = k & 1;
		o.mouse_right = (k & 2) == 0;
		o.height = k == 3 ? 0.3f : 0;
		V3 lo, hi;
		CHECK(desk_box(b, o, lo, hi));
		CHECK(lo.x < hi.x && lo.y < hi.y && lo.z < hi.z);
		auto inside = [&](V3 p, float room) {
			return p.x >= lo.x - room && p.x <= hi.x + room && p.y >= lo.y - room && p.y <= hi.y + room &&
			       p.z >= lo.z - room && p.z <= hi.z + room;
		};
		// Everything it shows is in it, whichever's used.
		for (int device = 1; device <= 2; device++) {
			Run r;
			o.device = device;
			r.g.set_options(o);
			r.g.set_body(b);
			r.wait(1);
			for (int i = 0; i < DESK_PROPS; i++)
				if (r.g.now().prop[i].opacity > 0)
					CHECK(inside(place_of(r.g.now().prop[i].place), 0.01f));
			for (int side = 0; side < 2; side++)
				CHECK(inside(r.g.now().hand[side].wrist, 0.01f));
		}
		// Below its shoulders (unless it's raised a lot), in front of it.
		CHECK((k == 3 || hi.y < b.shoulder[0].y) && lo.z > 0);
	}
}

static void test_pretend()
{
	// "Show me": typing (the mouse still), then a game on the keyboard and
	// mouse, then on a pad - each as GameHands takes it.
	const DeviceState a = pretend_devices(0), z = pretend_devices(PRETEND_TIME + 5);
	CHECK(a.keyboard && a.mouse && a.pads && a.key_presses == 0);
	CHECK(z.key_presses == pretend_devices(PRETEND_TIME).key_presses);
	const DeskBody b = body_of();
	GameHands g;
	g.set_body(b);
	bool typed = false, kbm_game = false, pad = false, mouse_moved = false;
	uint64_t last_presses = 0;
	bool presses_only_go_up = true;
	for (double t = 0; t <= PRETEND_TIME; t += 1.0 / 60) {
		const DeviceState s = pretend_devices(t);
		presses_only_go_up = presses_only_go_up && s.key_presses >= last_presses;
		last_presses = s.key_presses;
		g.step(s, t, 1.0f / 60);
		if (t > 2.5 && t < 4.5)
			typed = typed || g.now().typing;
		if (t > 6 && t < 9)
			kbm_game = kbm_game || (!g.now().typing && g.now().device == GameDevice::KeyboardMouse);
		if (t > 6 && t < 9 && g.now().mouse)
			mouse_moved = true;
		if (t > 11)
			pad = pad || g.now().device == GameDevice::Pad;
	}
	CHECK(presses_only_go_up);
	CHECK(typed && kbm_game && mouse_moved && pad);
	CHECK(pretend_devices(1).mouse_x == 0 && pretend_devices(6).mouse_x != 0);
	CHECK(pretend_devices(10).pad_at > 9);
}

int main()
{
	test_tally_keys();
	test_tally_mouse();
	test_tally_pads();
	test_reader_here();
	test_no_body();
	test_keyboard_mouse();
	test_keys();
	test_typing();
	test_mouse();
	test_pad();
	test_pad_hold();
	test_choice_and_rest();
	test_glide();
	test_reset();
	test_desk_box();
	test_pretend();
	std::printf("unit_game: %d passed, %d failed\n", passes, failures);
	return failures ? 1 : 0;
}
