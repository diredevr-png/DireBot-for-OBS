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

// What the character does on cue (a deck pad, chat, a redeem, an alert),
// on top of following you:
//
//   - A face for a while: happy, angry, surprised, tongue out... - the
//     faces a camera can't read - eased in and out. A new one takes over
//     from the one before.
//   - A move, once: a nod, a shake of the head, a tilt or a bounce (any
//     character: its head, as the picture shows it); a wave or a cheer (a
//     VRM's arms: rig.hpp moves them); one of its own animations, or a
//     VRM animation file's (rig.hpp plays those, as long as they are).
//     Each eases in and out, so it never jumps.
//   - Something thrown at it: a picture flying in from a side, growing
//     smaller as it goes (it's thrown from where you watch), spinning. It
//     hits its face - its head knocked back where it hits (a spring: it
//     snaps back and settles), its hair and ears thrown about, a flash
//     where it hit, an "ouch" (its eyes screwed shut a moment, and a face
//     if one is picked) - then it bounces off and falls away.
//   - A headpat: a hand coming down onto the top of its head and patting
//     it, its head bobbing with each pat, while it squints happily (and
//     makes a face, if one is picked).
//
// Where things are is in "heads" (the height of its head on the picture)
// from the middle of its face (where it would be without these moves), x
// right and y down as the picture shows it, so the same numbers fit a 3D
// character and a picture one, big or small.
// Plain code over plain numbers (tests/unit_react.cpp).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "math3d.hpp"

namespace direbot::face {

// Seconds from a throw leaving to its hit, and at least this long between
// two leaving (a volley: one after another).
constexpr float FLY_TIME = 0.5f;
constexpr float THROW_GAP = 0.1f;
// At most this many on their way at once, and waiting their turn.
constexpr int MOST_FLYING = 8;
constexpr int MOST_WAITING = 16;
// The longest a face stays on, and a headpat's most pats.
constexpr float MOST_FACE_TIME = 60;
constexpr int MOST_PATS = 20;

// The moves every character has, and the two only a VRM's arms make.
enum class BuiltIn { None, Nod, Shake, Tilt, Bounce, Wave, Cheer };
// As DireBot names them ("nod", "shake", "tilt", "bounce", "wave",
// "cheer"), and back (any case; BuiltIn::None if it isn't one).
const char *builtin_name(BuiltIn b);
BuiltIn builtin_named(const std::string &name);
// Whether it moves a VRM's arms (rig.hpp), rather than its head.
bool builtin_arms(BuiltIn b);
// How long it lasts (seconds).
float builtin_length(BuiltIn b);

// Something to throw: its picture (the caller's number for it), from
// which side (-1 the picture's left, 1 its right, 0 either, at random),
// how hard (1 as made, 0.1 to 2), how big (1: a little under half its
// head), and the face it makes when it's hit ("" none) and for how long.
struct Throw {
	int picture = -1;
	float side = 0;
	float strength = 1;
	float size = 1;
	std::string face;
	float face_time = 1.5f;
};

// A headpat: the hand's picture (the caller's number), how many pats,
// how big the hand is (1: about its head's width), and its face meanwhile
// ("" none: it only squints).
struct Pat {
	int picture = -1;
	int count = 5;
	float size = 1;
	std::string face = "happy";
};

// A body move for rig.hpp: a VRM's wave or cheer (`builtin`), or the
// caller's own (`move`: the number it gave play()), how far into it
// (seconds) and how much it shows (0 to 1).
struct BodyMove {
	BuiltIn builtin = BuiltIn::None;
	int move = -1;
	float time = 0, amount = 0;
};

// A face on now, and how much (0 to 1).
struct FaceNow {
	std::string name;
	float amount = 0;
};

// A picture in the air: where (heads from the middle of its face), how
// big (heads across), its turn (radians, clockwise), how much it shows,
// and whether it's hit yet.
struct Flying {
	int picture = -1;
	float x = 0, y = 0, size = 0, angle = 0, opacity = 1;
	// Squashed a moment as it hits: its width and height times these.
	float squash_x = 1, squash_y = 1;
	bool hit = false;
};

// The patting hand: shown, its picture, where the middle of its bottom
// edge is (heads), how big (heads across), its turn and how much it shows.
struct Hand {
	bool shown = false;
	int picture = -1;
	float x = 0, y = 0, size = 0, angle = 0, opacity = 0;
};

// The flash where something hit: where, how big, how much it shows.
struct Burst {
	float x = 0, y = 0, size = 0, opacity = 0;
};

struct ReactOut {
	std::vector<FaceNow> faces;
	// Its eyes shut this much (0 to 1): an "ouch", a happy squint.
	float squint = 0;
	// Its head turned more (degrees, as the picture shows it: + its nose
	// to the picture's right, + looking up, + tipped clockwise)...
	float yaw = 0, pitch = 0, roll = 0;
	// ...and moved (heads; + right, + down).
	float dx = 0, dy = 0;
	// The body moves playing (rig.hpp plays them), oldest first: one
	// easing out as the next takes over.
	std::vector<BodyMove> body;
	// Its swinging parts thrown about, once, this frame (zero if not): the
	// way, as the picture shows it (x right, y up, z out of the picture
	// towards you), in heads a second.
	V3 knock;
	std::vector<Flying> flying;
	Hand hand;
	std::vector<Burst> bursts;
	// Hits this frame (for FaceGetState's count, and the hit's sound).
	int hits = 0;
	// Anything on at all (when nothing is, nothing needs drawing).
	bool busy = false;
};

// Where the picture's edges are, from the middle of its face (heads), how
// far above it the top of its head is, and how tall the patting hand's
// picture is for its width.
struct ReactView {
	float left = -2, right = 2, top = -2, bottom = 2;
	float head_top = 0.6f;
	float hand_tall = 1.25f;
};

class React {
public:
	explicit React(uint32_t seed = 0x9E3779B9u);

	// A face, for `seconds` (as much as `amount`, 0 to 1). The face before
	// it eases out as this one eases in; the same face again keeps it on;
	// no face ("") eases off the one on.
	void face(const std::string &name, float seconds, float amount = 1);
	// A move every character has, or a VRM's arms' (wave, cheer).
	void move(BuiltIn b);
	// One of the caller's own moves (rig.hpp plays it): `id`, and how
	// long it is (seconds). Another move takes over from it.
	void play(int id, float length);
	// Thrown: how many seconds until it hits (DireBot plays the hit's
	// sound then), or -1 if too many are waiting already.
	float throw_at(const Throw &t);
	void pat(const Pat &p);
	// Everything eased off (quickly) and nothing waiting.
	void stop();

	// One frame, `dt` seconds on.
	ReactOut step(float dt, const ReactView &view);

	// How many are in the air or waiting, for tests.
	int flying() const { return (int)flying_.size(); }
	int waiting() const { return (int)waiting_.size(); }

private:
	struct Cue {
		std::string name;
		float amount = 1, at = 0, length = 1;
		float out_at = -1; // when it started easing out early (another took over)
	};
	// A move: built in, or the caller's (`id`).
	struct Act {
		BuiltIn builtin = BuiltIn::None;
		int id = -1;
		float at = 0, length = 1;
		float out_at = -1;
	};
	struct Item {
		Throw t;
		float side = 1; // -1 from the left, 1 from the right
		float at = 0;   // seconds since it was thrown
		float x0 = 0, y0 = 0, x1 = 0, y1 = 0, arc = 0;
		float spin = 0, angle = 0;
		bool hit = false;
		bool dropped = false; // stopped before it hit: it only falls away
		float hit_at = 0;     // `at` when it hit
		float vx = 0, vy = 0, x = 0, y = 0, size0 = 1, size = 1;
	};
	struct Spring {
		float value = 0, speed = 0;
		void step(float dt, float stiffness, float damping);
	};
	float random01();
	float random(float lo, float hi) { return lo + (hi - lo) * random01(); }
	void launch(Item &it, const ReactView &view);
	void hit(Item &it, ReactOut &out);

	uint32_t state_;
	float time_ = 0;
	std::vector<Cue> faces_;
	// The moves: the one on now last, the ones before it easing out.
	std::vector<Act> acts_;
	std::vector<Item> flying_, waiting_;
	float since_launch_ = 1;
	// The knock-back: springs on its head's turn and place.
	Spring yaw_, pitch_, roll_, dx_, dy_;
	// The "ouch": how long since it was hit.
	float ouch_ = -1;
	// The headpat on now: how long since it started, when its hand
	// starts to leave (and from how high), and the pats it's made.
	bool patting_ = false;
	Pat pat_;
	float pat_t_ = 0, pat_leave_ = 0;
	bool leaving_ = false;
	int pats_done_ = 0;
	// The hand last drawn, a new headpat's hand taking over from it, and
	// the hand as it started to leave.
	Hand last_hand_, from_, leave_from_;
	float blend_ = 1, from_squint_ = 0, from_press_ = 0;
	// The pat's squint and push on its head last frame, and as it left.
	float pat_squint_ = 0, press_ = 0, leave_squint_ = 0, leave_press_ = 0;
	struct Flash {
		float x = 0, y = 0, at = 0, size = 0.6f;
	};
	std::vector<Flash> flashes_;
};

} // namespace direbot::face
