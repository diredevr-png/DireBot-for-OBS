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

// A 3D character's pictures (its textures) as the graphics card gets
// them: square, a power of two across (at most 2048), with their smaller
// copies ("mipmaps") for drawing small without shimmer.
//
//   - A colour picture (a base colour, a glow, a cartoon's shade) is
//     shrunk as light - its colours turned back into light first (glTF's
//     sRGB undone, the way the effect reads them), averaged, then turned
//     back - so fine dark detail (stubble, pores, brow hairs) doesn't come
//     out darker and greyer small. A data picture (a normal, metal and
//     roughness, or occlusion map) is shrunk as the numbers it is.
//   - Colours are weighted by how solid they are, so see-through edges
//     never darken.
//   - A cut-out's picture (glTF's alpha MASK: lashes, stubble, hair cards)
//     keeps as much of it solid at every size as it has full size: each
//     smaller copy's see-through values are scaled until the same share
//     of it is over the cut-off (Castano's "alpha to coverage" mipmaps,
//     2010) - otherwise thin strands average below the cut-off and thin
//     out, or vanish, at stream size.
//
// Plain code over pixels, on any thread (tests/unit_pictures.cpp).

#pragma once

#include <cstdint>
#include <vector>

#include "model.hpp"

namespace direbot::face {

// The most pixels across a picture has on the graphics card.
constexpr uint32_t MOST_PICTURE = 2048;

// A model's picture as pixels (red, green, blue, see-through), square and
// a power of two across, with its smaller copies, each half the last.
struct ModelPicture {
	uint32_t size = 0; // 0: unreadable (drawn plain white)
	std::vector<std::vector<uint8_t>> levels;
};

// How a picture is used, which says how it's shrunk.
struct PictureUse {
	// Its colours are colours (sRGB), not numbers (a normal map's).
	bool colour = true;
	// A cut-out's: the see-through value (0 to 1, of the picture's own)
	// under which its material hides it; -1 for none.
	float cutoff = -1;
};

// Each of a model's pictures' use, from its materials: a colour picture
// unless some material reads it as data; a cut-out's cut-off when a MASK
// material takes its colour from it (the material's own see-through
// factor counted: a half see-through material's 0.25 is the picture's
// 0.5).
std::vector<PictureUse> picture_uses(const Model &m);

// A picture from pixels (w x h, 4 bytes a pixel, top row first).
ModelPicture picture_from(const uint8_t *rgba, uint32_t w, uint32_t h, const PictureUse &use = PictureUse());

// The share of a level's pixels (4 bytes each) that are solid: their
// see-through value, times `scale`, at or over `cutoff` (0 to 1).
float solid_share(const std::vector<uint8_t> &level, float cutoff, float scale = 1);

} // namespace direbot::face
