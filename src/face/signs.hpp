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

// Hand signs and face moves that set things off (0.9.0): a thumbs up
// held a moment, a wave, a wink... - each one DireBot is told to watch
// for, held as long as it's set to, sends DireBot a "FaceSign" event,
// and DireBot runs the routine picked for it. The character's arms don't
// have to follow yours for this ("Follow my hands and arms" can stay
// off): your hands are looked at all the same, and nothing moves.
//
// The hand shapes are read from a hand's 21 points in metres (body.hpp's
// HandSeen::world), so they're the same near the camera or far, turned
// any way: for each finger, how far its tip reaches from the wrist
// against the finger's own length, and how much it bends - straight, or
// curled into the palm, with a gap between where it's neither (a loose
// or claw-shaped hand is no sign at all) - and where the thumb is
// against the fingers, and which way the thumb or the first finger
// points (thumbs up and down, pointing up). Rules of our own, tried on
// Google's own pictures for its gesture model and on DireBot users' own
// camera pictures of each sign (fists, open hands, a peace sign, the
// middle finger, finger guns, claws and hands holding food).
//
// A wave is a hand open and going side to side: three swings, each at
// least half the hand's length, within a second and a half.
//
// The face moves are your face's own moves through your tuning (the
// Face avatar page's "Tune your face"), so "mouth wide open" is as wide
// as you've tuned the mouth to open, whoever's face it is; a wink is one
// eye shut while the other stays open.

#pragma once

#include <string>
#include <vector>

#include "alive.hpp"
#include "body.hpp"

namespace direbot::face {

// The hand signs: the shapes, then a wave (a moving one).
enum HandSign {
	SIGN_OPEN_PALM,
	SIGN_FIST,
	SIGN_THUMBS_UP,
	SIGN_THUMBS_DOWN,
	SIGN_POINT_UP,
	SIGN_FINGER_GUN,
	SIGN_PEACE,
	SIGN_MIDDLE_FINGER,
	SIGN_HORNS,
	SIGN_OK,
	HAND_SHAPES,
	SIGN_WAVE = HAND_SHAPES,
	HAND_SIGNS
};
extern const char *const HAND_SIGN_NAMES[HAND_SIGNS];

enum FaceMove { MOVE_MOUTH_OPEN, MOVE_BROWS_UP, MOVE_SMILE, MOVE_KISS, MOVE_WINK, FACE_MOVES };
extern const char *const FACE_MOVE_NAMES[FACE_MOVES];

// Which hand (your own left or right, wherever the camera has it) or
// eye; "both" is both hands at once.
enum SignSide { SIDE_EITHER, SIDE_LEFT, SIDE_RIGHT, SIDE_BOTH, SIGN_SIDES };
extern const char *const SIGN_SIDE_NAMES[SIGN_SIDES];

// A shape counts once it's this sure, and stays while it's this sure.
constexpr float SHAPE_ENTER = 0.55f, SHAPE_STAY = 0.35f;
// A sign or face move gone for less than this doesn't start its hold
// again (a look that missed your hand, a twitch).
constexpr float SIGN_GRACE = 0.2f;
// A wave: this many swings, each this many hand lengths (wrist to the
// middle finger's knuckle), within this many seconds.
constexpr int WAVE_SWINGS = 3;
constexpr float WAVE_SWING = 0.5f, WAVE_TIME = 1.5f;

// How sure it is of each hand shape (0 to 1) in a hand's world points
// (HandSeen::world), and the surest.
struct HandShape {
	float score[HAND_SHAPES] = {};
	int best = -1;
	float best_score = 0;
	// Each finger, first to little (0 to 1): held out straight; curled in.
	float out[4] = {}, curled[4] = {};
};
HandShape read_hand_shape(const Point world[HAND_POINTS]);

// Your face's moves now (0 to 1) from the camera's moves (tracker.hpp's
// EXPRESSIONS, as read - not as the character shows them) through your
// tuning. A wink, each eye: how much more shut it is than the other
// ([0] your own left eye, [1] your right).
struct FaceMovesNow {
	float value[FACE_MOVES] = {};
	float wink[2] = {};
};
FaceMovesNow read_face_moves(const float moves[EXPRESSIONS], const FaceTune &tune);

// One sign or face move DireBot watches for (the source's "signs"
// setting, a JSON list of {id, what, side, hold_ms, raised, strength}).
struct SignRule {
	std::string id;
	bool face = false; // a face move (else a hand sign)
	int what = 0;      // HandSign, or FaceMove
	int side = SIDE_EITHER;
	float hold = 0.5f;     // seconds it's held for before it counts (not a wave's)
	bool raised = false;   // a hand sign only with the hand up by your face or higher
	float strength = 0.7f; // a face move: how much of it (0.1 to 1)
};
// The rules in the setting's text; ones it can't read are left out
// (`skipped` counts them). An empty text is no rules.
std::vector<SignRule> rules_from(const std::string &text, int *skipped = nullptr);

// One that's been held its time: its rule's id, what it is, and which
// hand made it ("left", "right" or "both") or eye winked ("left" or
// "right"; "" for the other face moves).
struct SignFired {
	std::string id, what, side;
};

// What it sees now, for DireBot's page.
struct HandNow {
	bool found = false;
	int sign = -1; // the shape it holds (HandSign), or -1
	float score = 0;
	bool raised = false;
	bool waving = false;
};
struct SignsSeen {
	HandNow hands[2]; // your own left [0] and right [1]
	bool face_found = false;
	FaceMovesNow face;
	int fired = 0; // how many have been sent
	std::string last; // the last one's id
};

// Watches for the rules: OBS's graphics thread, every frame.
class SignWatch {
public:
	void set_rules(std::vector<SignRule> rules);
	const std::vector<SignRule> &rules() const { return rules_; }
	bool wants_hands() const;
	bool wants_face() const;
	// A frame: the newest look at your body and hands (null while they
	// aren't looked at) and whether it's a new one; your face's moves as
	// read (null while it isn't found) and your tuning; seconds since the
	// last frame.
	void step(float dt, const Body *body, bool body_fresh, const float *moves, const FaceTune &tune);
	// The ones held their time since it was last asked.
	std::vector<SignFired> take_fired();
	SignsSeen seen() const { return seen_; }
	void reset();

private:
	struct Wave {
		bool started = false;
		float x = 0, extreme = 0; // where the hand is, in hand lengths, and its furthest this swing
		float last_px = 0, size = 0;
		int dir = 0;
		std::vector<float> swings; // when each was (seconds)
		float open_at = 0;         // when it was last open
	};
	struct Hand {
		int sign = -1;
		Wave wave;
	};
	struct Track {
		bool on = false; // held now (face moves: past the strength, and staying)
		float held = 0, off_for = 0;
		bool armed = true;
	};
	void look_at_hands(const Body &body);

	std::vector<SignRule> rules_;
	std::vector<Track> tracks_;
	Hand hands_[2];
	SignsSeen seen_;
	float clock_ = 0;
	std::vector<SignFired> fired_;
};

} // namespace direbot::face
