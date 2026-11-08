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

// A companion beside the 3D character (0.9.0): a pet of its own file (a
// .glb or .vrm with its own animations) that DireBot brings next to
// whichever 3D character is on - a wolf that trots in, sits at your side
// and looks up at you, gets a headpat, and trots off again. It stands on
// the floor the character stands on, sized to it, in the same camera and
// light; the character itself goes on as it was.
//
// Here, as plain code over plain numbers (tests/unit_companion.cpp):
//   - which of its file's animations are which (coming, sitting there over
//     and over, being patted, going, going scared);
//   - its comings and goings: what plays how much, frame by frame, its
//     "come" ending exactly where its idle starts so it never jumps;
//   - where it goes beside the character, and the node a headpat's hand
//     lands on.
// face-avatar.cpp loads it, poses it with a rig of its own and draws it
// with the character (avatar3d.hpp's Avatar3D::Other).

#pragma once

#include <string>
#include <vector>

#include "math3d.hpp"
#include "model.hpp"
#include "react.hpp"
#include "rig.hpp"

namespace direbot::face {

// What each of its animations is for, in this order.
enum CompanionClipKind { CLIP_COME, CLIP_IDLE, CLIP_PET, CLIP_GO, CLIP_SCARED, COMPANION_CLIPS };
// As DireBot names them ("come", "idle", "pet", "go", "scared").
const char *companion_clip_name(int kind);

// One of its animations: its number in its file (-1 none) and how long.
struct CompanionClip {
	int index = -1;
	float length = 0;
};
struct CompanionClips {
	CompanionClip clip[COMPANION_CLIPS];
	const CompanionClip &operator[](int kind) const { return clip[kind]; }
};
// Which of its file's animations are which: the one named as `asked`
// says (any case; "" for each: worked out), else by the usual names -
// come (arrive, enter, in), idle (sit, loop, breathe), pet (pat,
// happy), go (leave, exit, out), scared (flee, run, bolt). Its only
// animation, if it has just one, is its idle.
CompanionClips companion_clips(const Model &m, const std::string asked[COMPANION_CLIPS]);

enum class CompanionPhase { Away, Coming, Out, Going };
const char *companion_phase_name(CompanionPhase p);

// What it plays this frame (for its rig: oldest first, each over the
// last as much as it shows), and how it's doing.
struct CompanionNow {
	CompanionPhase phase = CompanionPhase::Away;
	bool shown = false;   // drawn at all
	bool busy = false;    // coming or going
	bool petting = false; // its "pet" playing
	// The camera's: on while it's here (from when it starts to come), off
	// once it's leaving (half way through its "go"; on still if it's to
	// come back).
	bool camera = false;
	std::vector<MoveNow> moves;
};

// Its comings and goings: pure logic, stepped frame by frame.
class CompanionPlayer {
public:
	void set_clips(const CompanionClips &c) { clips_ = c; }
	const CompanionClips &clips() const { return clips_; }
	// It comes: its "come" (ending sitting, as its idle starts), or -
	// `instant`, or it has none - straight there, sitting. Going, it comes
	// back once it's gone (asked to go while coming: it stays after all).
	// False if there's nothing new to do (out, or coming, already).
	bool come(bool instant = false);
	// It goes: its "go" (or "scared", if `scared` and it has one) - or,
	// `instant` or it has neither, straight away. Coming, it goes once it's
	// here (no jump from half way in). False if there's nothing new to do
	// (away, or going, already).
	bool go(bool scared = false, bool instant = false);
	// Patted: its "pet" over its idle (patted again while it plays: once
	// more after). False if it isn't out, or it's coming or going; true
	// with no "pet" (it sits on, the hand pats it).
	bool pet();
	// Gone at once, nothing playing (a new file, say).
	void reset();
	CompanionNow step(float dt);
	CompanionPhase phase() const { return phase_; }
	// Seconds into what it's doing now (for tests).
	float time() const { return t_; }

private:
	void moves(CompanionNow &out) const;
	void start_coming();
	void start_going(bool scared);
	CompanionClips clips_;
	CompanionPhase phase_ = CompanionPhase::Away;
	float t_ = 0;         // seconds into its come or go
	float idle_t_ = 0;    // seconds into its idle
	int going_ = CLIP_GO; // its go or scared
	bool petting_ = false, pet_again_ = false;
	float pet_t_ = 0;
	// Asked while busy: comes back once gone, goes once here (scared?).
	bool come_after_ = false, go_after_ = false, go_scared_ = false;
};

// Where it goes beside the character, as DireBot sets it.
struct CompanionPlace {
	// Its height (as it is in its idle's first moment) a share of the
	// character's height as it stands (0.05 to 3).
	float size = 0.7f;
	// The side of the character it's on, as the picture shows it (-1 the
	// picture's left, 1 its right), and the side its file is made for -
	// on the other, it's mirrored.
	float side = -1, made_for = -1;
	// From the character's middle to its middle, across the picture, and
	// towards the camera: shares of the character's height.
	float distance = 0.32f, forward = 0;
	// Turned (degrees) about its up: + towards the character.
	float turn = 0;
};
// Each kept sensible (size 0.05 to 3, sides -1 or 1, distance -3 to 3,
// forward -3 to 3, turn -180 to 180).
CompanionPlace sensible_place(const CompanionPlace &p);

// Its model -> the world: beside the character whose box as it stands is
// `stands` (in the character's model, which `facing` turns to face +z),
// put in the world by `rest_place` (the character's model -> the world as
// it's framed, not moved by your head); its own box `its_box` (as it is
// in its idle's first moment, its model, which `its_facing` turns to
// face +z) - its bottom on the character's floor.
M4 companion_place(const Bounds &its_box, Quat its_facing, const Bounds &stands, Quat facing, const M4 &rest_place,
		   const CompanionPlace &p);

// The way it faces in its file (as glTF says: +z; a VRM 0.x the other way).
Quat companion_facing(const Model &m);

// The node a headpat's hand lands on: one named "pat" (any case), else its
// head bone (a VRM's, or a node named "head"); -1 if none.
int companion_pat_node(const Model &m);

} // namespace direbot::face
