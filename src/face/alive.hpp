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

// Two things between what the camera reads and what the character does:
//
//   - Tune: for each of the face's main moves (a blink, the mouth open, a
//     smile, round lips, brows up, brows down, the eyes' looks), the
//     camera's reading where the character's starts and where it's full.
//     Google's face models read some faces and cameras muted - a blink
//     that never quite closes, a mouth that barely opens - and every face
//     is different, so each range can be set while watching a live bar
//     (as Warudo's "blendshape mapping" and VTube Studio's parameters do).
//     Blinks can go both eyes together (no half-winks when one eye reads
//     shut and the other doesn't), and the eyes stay open in a smile (a
//     big smile pushes the cheeks up and reads as the eyes closing).
//
//   - Alive: a character that holds perfectly still looks frozen on
//     stream. It breathes (VMagicMirror's "wait motion": a sin-squared
//     curve, a breath every few seconds), sways a little, and its eyes
//     make the small moves eyes make (a new spot every 0.4 to 2 seconds,
//     reached quickly). When it can't see you it holds your last look for
//     a moment, then eases into a gentle idle - looking about, blinking by
//     itself (3 to 7 seconds apart, now and then twice) - instead of
//     freezing; once it sees you again it's back with you in a moment.
//
//   - The camera check: how lit your face is (each side of it) and the
//     room behind you, for DireBot's "Check my camera".
//
// Plain code over plain numbers (tests/unit_model.cpp).

#pragma once

#include <cstdint>

#include "tracker.hpp"

namespace direbot::face {

enum TuneGroup { TUNE_BLINK, TUNE_MOUTH, TUNE_SMILE, TUNE_ROUND, TUNE_BROWS_UP, TUNE_BROWS_DOWN, TUNE_LOOK, TUNE_GROUPS };
// Their names in the settings ("tune_blink_from"...) and in FaceGetState.
extern const char *const TUNE_NAMES[TUNE_GROUPS];

struct TuneRange {
	float from = 0, to = 1; // the camera's reading (0 to 1): where it starts, where it's full
};

struct FaceTune {
	// Where each starts and where it's full. The starting values: a blink
	// shut by 0.65 (Google's face models seldom read a blink much past
	// 0.7), the mouth open by 0.5 (Warudo's advice: 0.4 to 0.6), round
	// lips at 0.7, the rest a little past the face's own small twitches.
	TuneRange range[TUNE_GROUPS] = {{0.2f, 0.65f}, {0.05f, 0.5f}, {0.05f, 0.7f}, {0.15f, 0.7f},
					{0.1f, 0.8f},  {0.1f, 0.8f}, {0.1f, 0.7f}};
	bool linked_blink = true;
	bool open_when_smiling = true;
	// How far the eyes move (1 as tuned; 0 not at all, 2 twice as far).
	float eye_move = 1;
};
// The starting values as numbers 0 to 100, for the settings' defaults.
int tune_default(int group, bool to);

// What the camera reads for each group now (0 to 1): a blink, both eyes
// together (or the more shut one, when they aren't linked); the mouth
// open; the smile and brows down, both sides together; round lips, the
// rounder of an "O" and a "U"; brows up, the higher of the inner and outer
// brows; the eyes, the furthest they look any way.
void tune_readings(const float moves[EXPRESSIONS], const FaceTune &t, float out[TUNE_GROUPS]);
// Your moves through the tune: each of a group's moves from its range
// (0 below where it starts, 1 from where it's full), and the eyes as far
// as they're set to move. Moves outside the groups are as they are.
void tune_moves(const float in[EXPRESSIONS], const FaceTune &t, float out[EXPRESSIONS]);

// Eyes turned a little more (degrees: + towards the character's left, +
// up), through the eyes' looks: as the rig turns eyes, a look of 1 is 30
// degrees across, 25 up or down.
void add_eye_turn(float moves[EXPRESSIONS], float yaw, float pitch);

struct AliveOptions {
	float breath = 0.5f;      // how much it breathes: 0 not at all to 1 the most
	float breath_time = 5;    // seconds a breath
	float sway = 0.5f;        // how much it sways and looks about: 0 to 1
	bool eye_moves = true;    // its eyes' small moves
	bool auto_blink = false;  // it blinks by itself while it sees you too (a camera that misses blinks)
};

// What it does by itself this frame, on top of what it follows.
struct AliveOut {
	float breath = 0; // 0 breathed out to 1 breathed in, already times how much
	float idle = 0;   // 0 following you to 1 idle (it can't see you)
	// Its head's turn (degrees, as `step` was given them): yours while it
	// sees you, then the last it had, easing to straight ahead when idle...
	float yaw = 0, pitch = 0, roll = 0;
	// ...and its own slow sway on top: a little while it follows you, a
	// gentle look-about when idle.
	float sway_yaw = 0, sway_pitch = 0, sway_roll = 0;
	// Its body's slow sway (degrees: a turn about up, and a lean).
	float body_yaw = 0, body_roll = 0;
	// Its eyes' small moves (degrees: + to its left, + up).
	float eye_yaw = 0, eye_pitch = 0;
	// Your moves, held a moment when it can't see you, then easing to
	// rest - with its own blinks.
	float moves[EXPRESSIONS] = {};
};

// How lit the camera's picture is (0 black to 1 white, as the picture's
// own brightness): your face (its middle, from brow to chin), each side
// of it as the picture shows it, and the rest of the picture round it.
// `seen`: a face was found to measure.
struct Light {
	bool seen = false;
	float face = 0, left = 0, right = 0, room = 0;
};
Light light_of(const Frame &picture, const Face &face);

class Alive {
public:
	// How long it holds your last look before it eases into idle, and how
	// long the easing takes (seconds); back with you takes about BACK.
	static constexpr float HOLD = 1.0f, EASE = 1.5f, BACK = 0.12f;

	explicit Alive(uint32_t seed = 0x2545F491u);
	void set(const AliveOptions &o) { options_ = o; }
	const AliveOptions &options() const { return options_; }
	// One frame, `dt` seconds on: `found`, it sees your face; `yaw`,
	// `pitch`, `roll` your head's turn (degrees) and `moves` your face
	// (tuned) when it does.
	AliveOut step(float dt, bool found, float yaw, float pitch, float roll, const float moves[EXPRESSIONS]);
	// Back to the start: idle until it sees you.
	void reset();
	// An automatic blink now (0 to 1), for tests.
	float blink_now() const { return blink_; }

private:
	float random01();
	float random(float lo, float hi) { return lo + (hi - lo) * random01(); }
	void blink_step(float dt, bool on);

	AliveOptions options_;
	uint32_t seed_, state_;
	double time_ = 0;      // seconds it's been running
	double breath_at_ = 0; // a breath's progress (in breaths)
	float lost_for_ = 0;
	float idle_ = 0;
	bool seen_ = false;
	float held_yaw_ = 0, held_pitch_ = 0, held_roll_ = 0;
	float held_[EXPRESSIONS] = {};
	// Its eyes' small moves: where they're going, where they are, and
	// how long until the next.
	float eye_to_[2] = {}, eye_[2] = {};
	float eye_next_ = 0;
	// A blink: how far through (seconds), how many to go, and the wait
	// before the next.
	float blink_ = 0, blink_t_ = -1, blink_wait_ = 2;
	int blinks_left_ = 0;
};

} // namespace direbot::face
