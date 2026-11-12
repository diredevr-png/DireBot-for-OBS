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
//     if one is picked) - then it bounces off and falls away; or it sticks
//     there a while, moving with its head, then slides off. Or a bonk: up
//     over its head, a moment's wind-up, and down on top of it - its head
//     knocked down and squished.
//   - What it does as it hits (0.9.0), as made for each thing: a splat
//     (a tomato bursts on its face - juice thrown off it, the splat left
//     there moving with its head, drips running down, then sliding off
//     and fading), feathers (a pillow's, floating down, swaying, as it
//     bounces off), pieces (a snowball crushed against it, bursting into
//     chunks and a puff of powder, some snow left on its face a moment),
//     or a squish (a rubber duck squashed flat and wobbling back).
//   - A headpat: a hand coming in from a side, over the top of its head,
//     palm down, and patting it - lifted from the wrist and let fall, a
//     couple of times a second - its head dipping and squishing with each
//     pat (springing back) and leaning into the hand, its hair bouncing,
//     while it squints happily (and makes a face, if one is picked), its
//     eyes kept shut a moment after the hand has gone. A 3D hand on its
//     head (or a picture of one, turned at its wrist).
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
// How long a move `length` seconds long eases in, and out at its end
// (0.9.0: a move that ends in a new idle has it swapped in under it, so
// it lands on it: stay_swap).
float move_ease_in(float length);
float move_ease_out(float length);
// After 0.10.0: a move that stays (a ride), `length` seconds long: its
// idle is cross-faded in under it while the move shows whole - from `at`
// seconds into it over `fade` (at most `most`), done as the move begins to
// ease out - so the idle before it never shows through as the move eases
// out over the new one. (0.9.0 cross-faded it in as the move eased out:
// halfway, a quarter of the old idle showed.)
struct StaySwap {
	float at = 0, fade = 0;
};
StaySwap stay_swap(float length, float most);

// What happens where a thing hits, besides the flash (0.9.0).
enum class Effect { None, Splat, Feathers, Pieces };
// As DireBot names them ("", "splat", "feathers", "pieces"), and back
// (Effect::None if it isn't one).
const char *effect_name(Effect e);
Effect effect_named(const std::string &name);

// An effect and its pictures (the caller's numbers, -1 none): what's left
// on its face (a splat; the snow a snowball leaves), the drips running
// down from it, what's thrown off it (juice, feathers, snow - one of them
// at random each time) and a snowball's puff of powder.
struct HitLook {
	Effect effect = Effect::None;
	int mark = -1;
	int drip = -1;
	std::vector<int> bits;
	int puff = -1;
	// How long what's left on its face stays (seconds, up to MOST_MARK).
	float seconds = 4;
	// Squashed much flatter as it hits, and wobbling back (a rubber duck).
	bool squishy = false;
};
constexpr float MOST_MARK = 15;
// At most this many left on its face at once (one more and the oldest
// fades now), and this many bits in the air.
constexpr int MOST_MARKS = 24;
constexpr int MOST_BITS = 240;

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
	// Stuck to it where it hits this long (seconds, up to MOST_STICK; 0:
	// it bounces off), moving with its head; then it slides down a little
	// and falls away.
	float stick = 0;
	// A bonk: up over its head (a moment's wind-up, tipping back) and
	// down on top of it, rather than thrown from a side. Its head is
	// knocked down and squished, and it bounces off to a side.
	bool above = false;
	// How tall it's drawn for `size` across: a picture's height over its
	// width, a 3D thing's height over its longest side (a bonk lands
	// with its bottom on the top of its head).
	float tall = 1;
	// What it does as it hits (0.9.0). A splat or pieces never stick:
	// it's broken.
	HitLook look;
};
// The longest a thing stays stuck to it, and at most this many stuck at
// once (one more and the one stuck longest falls away).
constexpr float MOST_STICK = 10;
constexpr int MOST_STUCK = 12;

// A headpat: the hand's picture (the caller's number), how many pats,
// how big the hand is (1: as big as a hand is for a head), its face
// meanwhile ("" none: it only squints), and the side it comes from (-1
// the picture's left, 1 its right, 0 either, at random).
struct Pat {
	int picture = -1;
	int count = 5;
	float size = 1;
	std::string face = "happy";
	float side = 0;
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
	// A 3D one turns `angle` about this way (as the picture shows it: x
	// right, y up, z out of the picture towards you).
	V3 axis{0, 0, 1};
	// Stuck to it now (Throw::stick).
	bool stuck = false;
};

// The patting hand: shown, its picture, the side it comes from (-1 the
// picture's left, 1 its right), how big (`size`: 1 as made), how much it
// shows, and its pose:
//   - a picture of a hand: the middle of its bottom edge (where it touches
//     its head) at (x, y) heads, `across` heads wide, turned `angle`
//     (radians, clockwise) about its wrist (its bottom corner on its side);
//   - a 3D hand on the top of its head: lifted off it (`rise`) and out to
//     its side (`away`; both in heads), its wrist bent up (`lift`: 0 flat
//     on its head, 1 lifted between pats).
struct Hand {
	bool shown = false;
	int picture = -1;
	float side = -1, size = 1, opacity = 0;
	float x = 0, y = 0, across = 0, angle = 0;
	float rise = 0, away = 0, lift = 0;
};

// The flash where something hit: where, how big, how much it shows.
struct Burst {
	float x = 0, y = 0, size = 0, opacity = 0;
};

// Left on its face (0.9.0): a splat, or a drip running down from one. Its
// picture (HitLook's number), `size` heads across, turned `angle`
// (radians, clockwise), how much it shows; (x, y) its middle - or, a
// drip (`from_top`), the middle of its top edge, drawn `length` heads
// tall (it runs down, its top staying put). Drawn on the character only
// (never on the air round it).
struct Mark {
	int picture = -1;
	float x = 0, y = 0, size = 0, angle = 0, opacity = 1;
	bool from_top = false;
	float length = 0;
};

// A bit thrown off it as it hits (0.9.0): a drop of juice, a feather, a
// chunk of snow, a puff of powder - its picture, its middle at (x, y),
// `size` heads across, turned `angle` (clockwise), how much it shows.
struct Bit {
	int picture = -1;
	float x = 0, y = 0, size = 0, angle = 0, opacity = 1;
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
	// Its head squished by a headpat: 0 not at all, 1 a pat's most -
	// shorter and a little wider, from where it sits on its neck (a
	// picture: from its bottom edge) - and a little under 0 as it springs
	// back up past its own shape.
	float squish = 0;
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
	// What's left on its face, oldest first, and the bits in the air
	// (0.9.0).
	std::vector<Mark> marks;
	std::vector<Bit> bits;
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
	// Its face's own tilt (radians, clockwise as the picture shows it):
	// what's stuck to it and left on it tilts with it (0.9.0: a real
	// face in a camera; a character's own tilt is in its moves).
	float roll = 0;
	// A real head (0.9.0: a camera's): the knock-back doesn't move it, so
	// what's stuck to it and left on it doesn't move with the knock-back
	// either (the whole picture is jolted instead).
	bool still_head = false;
};

// 0.9.0, the camera jolted as things hit: a picture w x h drawn turned
// `turn` (radians, clockwise) about (cx, cy), moved (dx, dy) and zoomed
// about (cx, cy) - the least zoom (at least 1, at most 1.6) that leaves
// no gap at its edges.
float cover_zoom(float w, float h, float cx, float cy, float dx, float dy, float turn);

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
	// How many are stuck to it now, for tests and FaceGetState.
	int stuck() const;
	// How many are left on its face, and bits in the air (0.9.0).
	int marks() const { return (int)marks_.size(); }
	int bits() const { return (int)bits_.size(); }
	void pat(const Pat &p);
	// Everything eased off (quickly) and nothing waiting.
	void stop();

	// One frame, `dt` seconds on.
	ReactOut step(float dt, const ReactView &view);

	// A body or head move playing now (0.9.0, for FaceGetState).
	bool moving() const { return !acts_.empty(); }
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
		V3 axis{0, 0, 1};
		bool hit = false;
		bool dropped = false; // stopped before it hit: it only falls away
		float hit_at = 0;     // `at` when it hit
		// Stuck to it (Throw::stick) until `at` reaches `stuck_till`;
		// `stuck_at`, when it stuck (for the oldest to go first).
		bool stuck = false;
		float stuck_till = 0, stuck_at = 0;
		// Its turn as it stuck, and the one it settles to.
		float stuck_from = 0, stuck_angle = 0;
		float vx = 0, vy = 0, x = 0, y = 0, size0 = 1, size = 1;
		// Where it touches as it hits, for the flash (a bonk: the top of
		// its head, under its bottom; else its middle).
		float touch_y = 0;
		// Broken as it hit (a splat, pieces): squashed flat a moment and
		// gone.
		bool broke = false;
		// Its face's tilt as it stuck (ReactView::roll).
		float stuck_roll = 0;
	};
	// Left on its face: a splat, or a drip from one (`drip`: how far it
	// runs, heads, and how long it takes to; `wait`: how long after the
	// splat it starts).
	struct MarkItem {
		int picture = -1;
		bool drip = false;
		float x = 0, y = 0, size = 0, angle = 0;
		float at = 0, stay = 4;
		float wait = 0, run = 0, run_time = 1;
		float grow = 1; // a splat: how big it lands, as it spreads (0 to 1: none)
		// Fading early (too many, or stopped): when, and how long it takes.
		float fade_at = -1, fade_time = 0.8f;
		float roll = 0; // its face's tilt as it was left
		int id = 0;     // a splat's number; a drip's, its splat's
	};
	// A bit thrown off it: how it moves.
	enum class BitKind { Drop, Feather, Chunk, Puff };
	struct BitItem {
		BitKind kind = BitKind::Drop;
		int picture = -1;
		float x = 0, y = 0, vx = 0, vy = 0, size = 0, angle = 0, spin = 0;
		float at = 0, life = 1;
		// A feather's sway: how far, how fast, where in it.
		float sway = 0, sway_w = 0, sway_at = 0, sway_x = 0;
		float fade_at = -1;
	};
	struct Spring {
		float value = 0, speed = 0;
		void step(float dt, float stiffness, float damping);
	};
	float random01();
	float random(float lo, float hi) { return lo + (hi - lo) * random01(); }
	void launch(Item &it, const ReactView &view);
	void hit(Item &it, ReactOut &out);
	// Off it now (stuck), falling away.
	void unstick(Item &it);
	// What it does as it hits (its HitLook), from where it touches.
	void effect(const Item &it, float x, float y);
	void step_effects(float dt, const ReactView &view, ReactOut &out);
	void add_mark(const MarkItem &m);

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
	// A headpat's hand and what it does to its head, at a moment: out to
	// its side and lifted off it (heads), its wrist bent, how much it
	// shows, its head pushed down (0 to 1), its squint and its lean into
	// the hand (0 to 1).
	struct PatPose {
		float away = 0, rise = 0, lift = 0, opacity = 0, press = 0, squint = 0, lean = 0;
	};
	PatPose pat_pose(float t) const;
	// The headpat on now: how long since it started, how many pats, when
	// its hand starts to leave, and the pats it's made.
	bool patting_ = false;
	Pat pat_;
	float pat_t_ = 0, pat_leave_ = 0;
	int pat_count_ = 0;
	bool leaving_ = false;
	int pats_done_ = 0;
	// The pose last drawn, a new headpat's hand taking over from it, and
	// the pose as it started to leave.
	PatPose last_pose_, from_, leave_from_;
	bool shown_ = false;
	float blend_ = 1;
	// After a headpat: how long since its hand went (-1: not now) - its
	// eyes stay shut happily a moment, then open.
	float after_ = -1;
	bool hurry_after_ = false; // stopped: its eyes open straight away
	// The squish (a spring after the head's give under the hand).
	float squish_ = 0, squish_speed_ = 0;
	struct Flash {
		float x = 0, y = 0, at = 0, size = 0.6f;
	};
	std::vector<Flash> flashes_;
	std::vector<MarkItem> marks_;
	std::vector<BitItem> bits_;
	float roll_now_ = 0; // ReactView::roll this step
	int mark_ids_ = 0;
};

} // namespace direbot::face
