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

// Gaming hands (0.9.0): a 3D VRM character's hands on its own keyboard,
// mouse and game pad as you use yours (devices.hpp) - its own 3D ones in
// front of it, or none shown.
//
//   - Keyboard and mouse: one hand on the keyboard, a finger going down as
//     each key does (a finger of its own choosing, never the key's: the
//     hand can't spell anything out), held while keys are held - WASD held
//     in a game holds fingers down; the other hand on the mouse, the mouse
//     moving on its mat as yours does (a game that takes the mouse for
//     itself too), a click pressing its first or second finger, the wheel
//     flicking its first. Typing away with the mouse still, both hands
//     type; move the mouse and that hand goes back to it.
//   - A pad: both hands on it, held up a little and tilted towards it,
//     each thumb on its stick or its buttons as you use them, the sticks
//     leaning as yours do, its first fingers on the triggers and
//     bumpers; the pad itself leaning and bobbing a little as you play.
//   - What you use: whichever you used last (or always one), the other
//     fading away; your hands resting after a while without either, if
//     asked. The mouse in your right hand or your left; mirrored as the
//     character is (your reflection or not).
//
// The way it all moves follows VMagicMirror's (github.com/malaybaku/
// VMagicMirror, MIT licence, Copyright (c) baku_dreameater): a hand
// reaching for a key lifted on its way, a finger held down, a pad
// jittering with what you press and swaying by itself - its recipes, not
// its code. Here the hands glide on springs (no snapping), a hand going
// from one thing to another lifts over it, and nothing about which key
// is shown.
//
// All lengths here are a real person's (metres, an arm 55 cm from
// shoulder to wrist) round the middle of its shoulders - x its left, y up,
// z out of its front, as rig.hpp's facing model has them - and grown to
// its own size by its arms' length (DeskBody). Plain code over plain
// numbers (tests/unit_game.cpp).

#pragma once

#include <cstdint>

#include "devices.hpp"
#include "math3d.hpp"

namespace direbot::face {

// An adult's arm, shoulder to wrist (metres): the props and the desk are
// grown by its arm over this.
constexpr float REAL_ARM = 0.55f;

// The character as the hands need it (Rig::desk_body): the facing model's
// way (x its left, y up, z out of its front), in its own units.
struct DeskBody {
	bool ok = false;
	V3 shoulder[2]; // where its left [0] and right [1] upper arms turn from
	float arm = 0;  // shoulder to wrist (its two arms' middle)
	// Its hands (the two's middle; 0 not known): wrist to middle knuckle,
	// first knuckle to little one, and its middle finger's length.
	float hand = 0, knuckles = 0, finger = 0;
};

// One of its hands on a keyboard, mouse or pad (as arms.hpp's ArmTarget
// says a hand's ways), the facing model's way, in its own units: 0 its
// own (resting, or as the camera or a tracker app has it) to 1 there.
struct DeskHand {
	float weight = 0;
	V3 wrist;
	V3 along{0, 0, 1}, across{1, 0, 0}; // wrist to middle knuckle, little knuckle to first (unit)
	V3 pole{0, -1, 0};                  // its elbow points this way
	// Thumb, first, middle, ring, little: each joint's bend from straight,
	// root first (radians, + towards the palm).
	float bend[5][3] = {};
	// Where each one's tip reaches to (a pad's stick, its trigger, round
	// its grip), as far as its joints bend that way from `bend` - and how
	// much (0: just as `bend` has it).
	V3 reach[5];
	float reach_weight[5] = {};
};

// The 3D things in front of it (each its own file, made in metres: x its
// left as it faces the character, y up, z away from it - as the facing
// model has them).
enum DeskProp {
	DESK_KEYBOARD,
	DESK_MOUSE,
	DESK_MAT, // the mouse's mat
	DESK_PAD,
	DESK_LEFT_STICK, // the pad's sticks, each the same file
	DESK_RIGHT_STICK,
	DESK_PROPS
};
// Each one's file (data/face/gaming/<name>.glb).
extern const char *const DESK_PROP_FILES[DESK_PROPS];

struct DeskPlace {
	float opacity = 0; // 0 not drawn
	M4 place;          // its file (metres) -> the facing model's way, in its own units
};

enum class GameDevice { None = 0, KeyboardMouse = 1, Pad = 2 };

struct GameOptions {
	// 0 whichever you used last, 1 always the keyboard and mouse, 2 always a pad.
	int device = 0;
	bool mouse_right = true; // the mouse in your right hand
	bool mirror = true;      // it moves as your reflection (your right hand, its left)
	bool props = true;       // its own keyboard, mouse and pad shown
	// Its hands rest after this many seconds with none of them used (0:
	// they never do).
	float rest_after = 0;
	// The desk (all the things on it) raised and moved away from it: shares
	// of its arm's length (-0.4 to 0.4).
	float height = 0, distance = 0;
};

struct DeskNow {
	DeskHand hand[2]; // its left [0] and right [1]
	DeskPlace prop[DESK_PROPS];
	GameDevice device = GameDevice::None;
	float on = 0;        // how much its hands are on them (0 resting)
	bool typing = false; // both hands on the keyboard
	// Used in the last second (for the page): a key, the mouse, a pad.
	bool keys = false, mouse = false, pad = false;
};

class GameHands {
public:
	// A hand glides on a spring settling in about this long (seconds); a
	// hand going from one thing to another lifts over it, up to LIFT
	// (metres).
	static constexpr float GLIDE = 0.3f, LIFT = 0.07f;
	// Which you use, as soon as you've used it this much (a key or click
	// 1, the mouse moving or the pad changing about 1 a twentieth of a
	// second), fading over SWITCH seconds.
	static constexpr float CLAIM = 2.5f, SWITCH = 0.35f;

	void set_options(const GameOptions &o) { options_ = o; }
	const GameOptions &options() const { return options_; }
	void set_body(const DeskBody &b) { body_ = b; }
	// A frame on: what's been done on them (`now`: the reader's clock), and
	// `dt` seconds since the last frame.
	void step(const DeviceState &s, double now, float dt);
	const DeskNow &now() const { return now_; }
	// As if just turned on: nothing pressed, its hands where they'd go.
	void reset();

private:
	struct Finger {
		bool down = false;  // held
		double until = -1;  // shown pressed till then at least (a quick tap)
		double since = 0;   // when it went down
	};
	struct Hand {
		bool started = false;
		V3 at, speed;    // its wrist (metres, as the desk has it), and how fast it's going
		V3 along{0, 0, 1}, across{1, 0, 0};
		float bend[5][3] = {};
		V3 reach[5];
		float reach_weight[5] = {};
		Finger finger[5];
		V3 drift, drift_to; // a typing hand's wander round where it rests
		int presses = 0;
	};
	struct Target {
		V3 wrist, along{0, 0, 1}, across{1, 0, 0}, pole{0, -1, 0};
		float bend[5][3] = {};
		V3 reach[5];
		float reach_weight[5] = {};
	};

	void take(const DeviceState &s, double now, float dt);
	void press_key(double now);
	Target keyboard_hand(int canon, double now) const;
	Target mouse_hand(double now) const;
	void pad_targets(Target out[2], M4 &pad, M4 sticks[2], double now, float dt);
	void glide(int side, const Target &t, float dt);
	float random(); // 0 to 1, its own (the same each run: tests)

	GameOptions options_;
	DeskBody body_;
	DeskNow now_;
	Hand hand_[2]; // by its own side, as it's drawn
	bool have_last_ = false;
	DeviceState last_;
	double kbm_at_ = -1, pad_at_ = -1, keys_at_ = -1, mouse_used_at_ = -1, any_at_ = -1;
	float kbm_claim_ = 0, pad_claim_ = 0;
	GameDevice chosen_ = GameDevice::KeyboardMouse;
	float share_kbm_ = 1, share_pad_ = 0, on_ = 0;
	bool typing_ = false;
	double presses_at_[8] = {-1, -1, -1, -1, -1, -1, -1, -1}; // the last few key presses
	int press_ring_ = 0;
	// The mouse on its mat (metres from its middle: x its left, z away) and
	// its turn; how far its buttons and wheel have gone.
	double mouse_acc_x_ = 0, mouse_acc_z_ = 0;
	float mouse_x_ = 0, mouse_z_ = 0, mouse_turn_ = 0;
	float wheel_flick_ = 0; // seconds of a wheel's flick left
	// The pad: its jitter (where it's heading and where it is), its dip
	// while something's pressed, its time, each thumb's place (0 the
	// stick, 1 the buttons) and when it last changed.
	V3 pad_jitter_, pad_jitter_to_, pad_turn_, pad_turn_to_;
	float pad_dip_ = 0, pad_clock_ = 0, pad_busy_ = 0;
	float thumb_on_[2] = {0, 1}, thumb_want_[2] = {0, 1};
	double thumb_at_[2] = {-1, -1};
	PadState pad_seen_;
	uint32_t seed_ = 0x9e3779b9u;
};

// Round its desk and its hands on it, as `o` has them (the facing model's
// way, in its own units: everything on it whichever is used), for framing
// it with its head and shoulders. False without its arms.
bool desk_box(const DeskBody &b, const GameOptions &o, V3 &low, V3 &high);

// "Show me": GameHands' devices pretending to be used for PRETEND_TIME
// seconds (`t` from the start) - typing, playing with the keyboard and
// mouse, then a pad - so its hands can be seen without pressing anything.
constexpr double PRETEND_TIME = 14;
DeviceState pretend_devices(double t);

} // namespace direbot::face
