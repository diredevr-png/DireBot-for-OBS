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

// Something 3D drawn with a character or over one (a headpat's hand,
// things thrown and the pieces they throw off, gaming hands' keyboard)
// made ready for the graphics card: avatar3d.hpp's Prop3D hands it over
// as it is, once, and never sends a point again.
//
//   - Its points are as its file has them - each part where its node (a
//     bound one: its strongest bone, as it was bound) puts it - not where
//     it's drawn: the card puts it there, by one matrix a draw (the
//     effect's Thing techniques). Moving, turning, squashing or mirroring
//     a thing costs no work a point, however many are on screen.
//   - Its shapes (glTF's morph targets - a thing's wobble, a splash
//     opening, a hand lifting) ride with its points, so the card adds
//     them too: the first PROP_SHAPES of them, on the parts that have any.
//   - Nothing else is kept. The file as read (its points again, its
//     triangles, its shapes point by point) is let go of as soon as this
//     is made, and a part of at most 65,536 points has its triangles'
//     corners in two bytes each.
//
// Plain code, on any thread: made where the file is read, off OBS's
// graphics thread (tests/unit_prop.cpp).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "math3d.hpp"
#include "model.hpp"

namespace direbot::face {

// How many of a thing's shapes the card adds (its file's first ones).
constexpr size_t PROP_SHAPES = 2;
// The numbers that ride with each point of a shaped part: for each shape,
// how far it moves the point (x y z) and turns its facing (x y z), packed
// into three fours (the effect's ShapedIn):
//   [0..3] move 0 (x y z), facing 0 (x)     [4..7] facing 0 (y z), move 1 (x y)
//   [8..11] move 1 (z), facing 1 (x y z)
constexpr size_t PROP_SHAPE_NUMBERS = 12;

struct PropMesh {
	struct Part {
		int material = -1; // -1: plain white
		size_t points = 0;
		bool bumped = false; // its tangents ride with its points (its normal map's)
		bool shaped = false; // its shapes do too (`shapes`)
		// Four numbers a point, as OBS has them: x y z and a spare one -
		// a tangent's is which way its picture's up is (1 or -1).
		std::vector<float> pos, normal, tangent;
		std::vector<float> uv;       // 2 a point
		std::vector<uint8_t> colour; // 4 a point, sRGB
		std::vector<float> shapes;   // PROP_SHAPE_NUMBERS a point
		// Its triangles' corners: one list or the other.
		std::vector<uint16_t> index16;
		std::vector<uint32_t> index32;
		size_t corners() const { return index16.empty() ? index32.size() : index16.size(); }
		V3 middle; // its points' middle (a glowing part's depth)
		// What it takes in memory (and again on the card).
		size_t bytes() const;
	};
	std::vector<Part> parts;
	std::vector<Material> materials;
	// Its shapes' names (may be empty), as many as are kept; and how many
	// its file has.
	std::vector<std::string> shape_names;
	size_t shapes = 0, shapes_in_file = 0;
	// The box round it, as made.
	V3 low, high;
	size_t points = 0, triangles = 0;
	size_t bytes() const;
};

// A file made ready (hair strands in a thing aren't drawn: left out).
PropMesh make_prop_mesh(const Model &m);

// A linear colour (4 numbers) as sRGB bytes, the way a point's colour is
// handed to the card.
void srgb_bytes(const float *linear, uint8_t *out);

// What the card is told to put a thing at `place` (its file's units -> the
// world), seen through `view` (the world -> the camera's view):
// - how its facings turn, straight to the view: as the inverse of its
//   place's rows-for-columns would (the same, for a turn and an even size;
//   right for one squashed or mirrored too), then as the view turns them -
//   the card makes each length 1 again;
// - and whether its place turns it inside out (-1: mirrored; else 1): its
//   other sides are then the ones left out, and its tangents' which-way-up
//   turns over.
inline M4 prop_turn(const M4 &view, const M4 &place)
{
	const M4 inv = inverse(place);
	M4 turn;
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++) {
			float sum = 0;
			for (int k = 0; k < 3; k++)
				sum += view.at(r, k) * inv.at(c, k);
			turn.at(r, c) = sum;
		}
	return turn;
}
inline float prop_handed(const M4 &place)
{
	const V3 ax = direction(place, {1, 0, 0}), ay = direction(place, {0, 1, 0}), az = direction(place, {0, 0, 1});
	return dot(cross(ax, ay), az) < 0 ? -1.0f : 1.0f;
}

} // namespace direbot::face
