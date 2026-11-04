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

// Your arms, hands and fingers (body.hpp's points) as the 3D avatar's:
// which way each part of its arms points, its elbows' bend, its hands'
// turn and each finger joint's bend - against its chest, so they ride
// on its body as yours do on you.
//
// The maths follows Kalidokit's (github.com/yeemachine/kalidokit, MIT
// licence, Copyright (c) 2021 yeemachine): an arm's parts point where
// yours do (the body's points in metres, as you face), a hand turns to
// your palm's way, and each finger joint bends as far as the angle
// between your finger's bones there. On top of that, our own:
//
//   - Your chest is the frame: your arms against your shoulders (and
//     your hips, when the camera sees them), so leaning or turning in
//     your chair doesn't swing its arms.
//   - The elbow bends the way yours does (its hinge from your arm), kept
//     as it was while your arm is nearly straight (no flips).
//   - By your head (a hand on your face, scratching your head, a peace
//     sign by your eye): its hand goes to the same place by its own head,
//     whatever its size, so a hand on your cheek is on its cheek.
//   - Natural limits on each finger joint, and on how far a wrist bends.
//   - A part that's lost (out of the picture, or a "ghost" hand) is held
//     as it was for a moment, then rests (arms down, fingers relaxed);
//     found again, it's back quickly. A hand must be seen two looks
//     running before it moves anything.
//   - Calibrate: hold a hand up by your ear, open, palm to the camera.
//     It learns how your open hand looks (so its fingers open all the
//     way when yours do) and where your hand is when it's by your ear
//     (so its hand goes by its ear).
//   - Reach (how far it reaches towards and away from the camera, as the
//     camera judges depth less well) and arms out from its body.
//
// Plain code over plain numbers (tests/unit_body.cpp).

#pragma once

#include <string>

#include "body.hpp"
#include "math3d.hpp"

namespace direbot::face {

// One of its arms (its own left [0] and right [1]) in its chest's terms
// at rest, the facing model's way: x its left (the picture's right as it
// faces you), y up, z out of its front.
struct ArmTarget {
	// 0 resting (its arms as they'd be without you) to 1 following yours.
	float weight = 0;
	V3 upper{1, 0, 0}, fore{1, 0, 0}; // shoulder to elbow, elbow to wrist (unit)
	// The elbow's bend axis (upper x fore) and how sure it is: 0 for an
	// arm held straight (any axis would do) to 1.
	V3 hinge{0, -1, 0};
	float hinge_sure = 0;
	// The wrist against the middle of your head (in head widths: ear to
	// ear) and how near your head it is (0 far to 1 by it).
	V3 by_head;
	float near = 0;
	// Its hand: 0 as its arm leaves it to 1 as yours is (its turn, and its
	// fingers' bends if fingers are followed).
	float hand = 0;
	V3 along{1, 0, 0}, across{0, 0, 1}; // wrist to middle knuckle, little knuckle to first (unit)
	// Thumb, first, middle, ring, little: each joint's bend from
	// straight, root first (radians, + towards the palm).
	float bend[5][3] = {};
};

struct ArmsNow {
	bool fingers = true; // its fingers follow yours (else they rest relaxed)
	ArmTarget side[2];
};

struct ArmOptions {
	bool mirror = true;  // it moves as your reflection (your right arm, its left)
	bool fingers = true; // follow your fingers
	float reach = 1;     // towards and away from the camera: 1 as you, 2 twice as far
	float spread = 0;    // degrees: its arms held out from its body when they're down
	float hold = 0.5f;   // seconds a lost arm or hand stays as it was before it rests
};

// What's being followed now, for the page (its own sides).
struct ArmsSeen {
	bool arm[2] = {false, false};
	bool hand[2] = {false, false};
};

class ArmSolver {
public:
	// A found arm or hand comes back over BACK seconds; a lost one, after
	// its hold, rests over FADE. Each frame eases towards the newest look
	// over about EASE seconds (looks come 10 to 20 a second; OBS draws 60).
	static constexpr float BACK = 0.25f, FADE = 0.6f, EASE = 0.06f;
	// A hand (or arm) must be found this many looks running.
	static constexpr int STREAK = 2;
	// Your wrist's place by your ear, held up open (head widths: out from
	// your head, down, forward): where "Calibrate" puts it.
	static constexpr float BY_EAR_OUT = 0.65f, BY_EAR_DOWN = 0.55f;

	void set_options(const ArmOptions &o) { options_ = o; }
	const ArmOptions &options() const { return options_; }

	// A new look from the body tracker.
	void look(const Body &body);
	// A frame `dt` seconds on.
	void step(float dt);
	const ArmsNow &now() const { return now_; }
	ArmsSeen seen() const;
	// Back to resting at once (a new camera, tracking turned off).
	void reset();

	// Calibrate: the next CALIBRATE_LOOKS looks that see a hand by your
	// head are kept. calibrating() is true until then (or until it gives
	// up after CALIBRATE_TIME seconds: no hand seen).
	static constexpr int CALIBRATE_LOOKS = 12;
	static constexpr float CALIBRATE_TIME = 6;
	void start_calibration();
	void stop_calibration();
	bool calibrating() const { return calibrating_; }
	bool calibrated() const { return calibrated_; }
	// The last calibration's result: "" while none ran, "ok", or why it
	// didn't take ("no hand").
	const std::string &calibration_result() const { return calibration_result_; }
	// As text, to keep in the source's settings, and back ("": none).
	std::string save_calibration() const;
	bool load_calibration(const std::string &text);
	void clear_calibration();

private:
	struct Side {
		bool arm_ok = false, hand_ok = false;
		int arm_streak = 0, hand_streak = 0;
		float arm_lost = 0, hand_lost = 0; // seconds since last seen
		bool fresh = false;                // a new look since the last step
		ArmTarget target;                  // the newest look's
	};
	void finish_calibration();

	ArmOptions options_;
	Side side_[2];
	ArmsNow now_;
	// Calibration: your open hand's bends (subtracted, so open is
	// straight) and the offset that takes your hand by your ear to
	// BY_EAR (out, down, forward; head widths).
	bool calibrated_ = false, calibrating_ = false;
	float open_[5][3] = {};
	V3 ear_offset_;
	std::string calibration_result_;
	int kept_ = 0;
	float calibrate_time_ = 0;
	double sum_bend_[5][3] = {};
	int bend_count_ = 0;
	double sum_place_[3] = {};
	int place_count_ = 0;
};

// For tests: each joint's bend in a hand's points (metres, the camera's
// way), as the solver measures them; `left`: a left hand.
void finger_bends(const Point *hand, bool left, float bend[5][3]);

} // namespace direbot::face
