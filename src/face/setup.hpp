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

// A 3D character's set-up beyond its file: the face avatar's
// "model_setup" setting, as DireBot sends it - read from a Lens Studio
// project it was imported from (DireBot's lensimport.py), or spots
// dragged into place. Lengths are in the model's own units.
//
//   {"facing": [x, y, z, w],     the turn from the model to facing you
//    "pivot": [x, y, z],         what it turns round, whole
//    "spots": {"left_eye": {"at": [x, y, z], "w": .., "h": ..},
//              "right_eye": {"x": .., "y": .., "w": .., "h": ..}, "mouth": ...},
//    "chains": [{"bones": [...], "pull": [x, y, z], "with_head": true,
//                "both_ends": false}],
//    "upright": [{"bone": "...", "follow": 5}],
//    "own_face": ["...", ...]}
//
// A spot is the character's own left eye, right eye or mouth: its middle
// ("at", in the model) or, dragged on its picture from the front, its
// middle as shares of that picture ("x" from the left, "y" from the top,
// on whatever's in front there); "w" and "h" are its size (with "at", in
// the model's units; else shares of the picture's width). A chain: its
// bones in order, the first hanging from where it is and the rest
// swinging, pulled by "pull" (the model's units a second, each second -
// turning with your head, or straight down the picture). "upright": parts
// that stay as they are made, however your head turns, trailing after
// your head ("follow": how quickly, a second). "own_face": its own drawn
// eyes and mouth (parts, with all hanging off them), hidden while your
// real ones are on it.

#pragma once

#include <string>
#include <vector>

#include "math3d.hpp"

namespace direbot::face {

struct SpotSetup {
	bool given = false;
	bool has_at = false;
	V3 at;              // with has_at: in the model
	float x = 0, y = 0; // else: shares of its front picture
	float w = 0, h = 0;
};
struct ChainSetup {
	std::vector<std::string> bones;
	V3 pull{0, -1, 0};
	bool with_head = true;
	bool both_ends = false;
};
struct UprightSetup {
	std::string bone;
	float follow = 5;
};
struct CharacterSetup {
	bool has_facing = false;
	Quat facing;
	bool has_pivot = false;
	V3 pivot;
	// The character's own left eye, right eye, mouth.
	SpotSetup spots[3];
	// Chains given: only these swing (none of the dangly parts it would
	// otherwise find).
	bool chains_given = false;
	std::vector<ChainSetup> chains;
	std::vector<UprightSetup> upright;
	std::vector<std::string> own_face;
	bool empty() const;
};

// Reads it; anything it can't use is left out ("" or bad JSON: none).
CharacterSetup setup_from(const std::string &text);

} // namespace direbot::face
