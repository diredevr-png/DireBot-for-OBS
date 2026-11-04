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

// The 2D face avatar's sums (face-avatar.cpp draws with them):
//
//   * Your real eyes and mouth: where each piece is cut from the camera
//     (straightened, sized from how far apart your eyes are so it doesn't
//     shrink as you turn), its soft outline along your real eye or lips,
//     and the colour of the skin round it, so the skin round them can
//     take the character's colour while your eyes, lips and teeth (inside
//     the outline) keep theirs.
//   * The character's shape: how far each point of the flat picture
//     stands out towards you (rounded off from its outline, with a bump
//     for a nose or muzzle), so it can turn and nod with you; which of
//     its eye and mouth spots are see-through (your eyes and mouth then
//     go behind it); and its colour round each spot.
//   * The drawn face: which of the character's own eye and mouth pictures
//     show (open, half, closed; closed, open, wide, smile, "O"), from your
//     expressions.
//
// Plain code over plain numbers, tested without OBS (tests/unit_avatar.cpp).
// The shader (data/effects/direbot-face.effect) does the same colour and
// turning sums per pixel.

#pragma once

#include <cstdint>
#include <vector>

#include "tracker.hpp"

namespace direbot::face {

// A box as shares of a picture: its top left corner and size.
struct Box {
	float x = 0, y = 0, w = 0, h = 0;
};

// ---- colour ----------------------------------------------------------------

// CIE L*a*b* (D65), from and to sRGB (each 0 to 1).
struct Lab {
	float l = 0, a = 0, b = 0;
};
Lab lab_of(float r, float g, float b);
void rgb_of(const Lab &lab, float rgb[3]);

// ---- your real eyes and mouth ----------------------------------------------

// Each piece's soft outline is a MASK x MASK picture.
constexpr int MASK = 48;

// One piece of the camera's picture: an eye or the mouth.
struct Piece {
	// Its middle (shares of the picture's width and height), its size
	// (both as shares of the picture's WIDTH, so it's square in pixels)
	// and its turn (radians, clockwise as the picture shows it).
	float x = 0, y = 0, w = 0, h = 0, angle = 0;
	// How much bigger than usual it was cut (a wide-open mouth stays
	// whole), so it's drawn that much bigger too: 1 usually.
	float grown = 1;
	// How much of each point shows, 0 to 255: row after row from the
	// piece's top, its left (as the camera sees it) first.
	uint8_t mask[MASK * MASK] = {};
	// The same way, how much of each point is inside the outline - your
	// eye itself, or your lips and teeth - so it keeps its own colour.
	uint8_t inner[MASK * MASK] = {};
	// The skin just round it.
	Lab skin;
};
struct Pieces {
	bool found = false;
	// In Layout's order: the eye on the picture's left, the other, the mouth.
	Piece part[PARTS];
	// How far apart the eyes are in 3D (a share of the picture's width):
	// it doesn't shrink as you turn.
	float gap = 0;
};

// The pieces from a look. `aspect`: each piece's height over its width
// (that of the character's spot it goes into, so it's never stretched),
// in the same order; `soft`: 0 a hard edge to 1 the softest.
void cut_pieces(const Face &face, const float aspect[PARTS], float soft, Pieces &out);
// The skin colour round each piece, from the picture the look was of.
void skin_round(const Frame &picture, Pieces &pieces);

// A piece's point (u, v from 0 to 1 across and down it) in the picture
// it was cut from (shares).
void piece_point(const Piece &p, int picture_w, int picture_h, float u, float v, float &x, float &y);

// How a point of a piece is shown (the shader's sums, for the tests):
// its colour `rgb` (sRGB), the skin round the piece, the colour the skin
// takes, how much (0 to 1) and how much of the point is inside the
// outline (`inner`, 0 to 1). Outside, it takes the colour with its own
// light and shade (a shadow, a beard, an eyebrow stay darker); inside, it
// keeps its own - whatever colour your eyes are.
void skin_blend(const float rgb[3], const Lab &skin, const Lab &target, float amount, float inner, float out[3]);

// ---- the character ---------------------------------------------------------

struct Shape {
	// The depth map: `w` x `h` points over the picture, each how far it
	// stands out towards you as a share of the picture's width (0 at its
	// outline and outside it).
	int w = 0, h = 0;
	std::vector<float> depth;
	// What it turns round (shares of the picture).
	float pivot_x = 0.5f, pivot_y = 0.5f;
	// Per spot (as the spots were given): see-through there, so your eye
	// or mouth goes behind the picture; and the picture's colour round it.
	bool hole[PARTS] = {};
	Lab colour[PARTS];
	bool ready() const { return w > 0 && h > 0; }
};

// `rgba`: the picture, straight colour, 4 bytes a point (red first, or
// blue first if `bgra`); a few hundred points across is plenty.
// `spots`: its left eye, right eye and mouth as the viewer sees them, or
// none (`have_spots` false).
Shape shape_of(const uint8_t *rgba, int w, int h, int stride, bool bgra, const Box spots[PARTS], bool have_spots);

// How far the picture stands out at (x, y) (shares), smoothly between
// the depth map's points.
float depth_at(const Shape &s, float x, float y);

// Turning and nodding, the way a drawn head turns: each point slides
// sideways (and up or down) by how far it stands out, so its nose and eyes
// move over while its outline mostly stays, and the whole narrows a
// little (FORESHORTEN: 0 not at all, 1 as a flat card would).
// `yaw`: radians, + turns its front to the viewer's right; `pitch`: + up.
// turned_point: where the flat picture's (x, y) lands (character pixels,
// `w` x `h` of them); turned_from: the flat picture's point that lands
// on (x, y). The shader's PSTurn does turned_from's very steps.
constexpr float FORESHORTEN = 0.35f;
constexpr int TURN_STEPS = 5;
void turned_point(const Shape &s, float w, float h, float yaw, float pitch, float x, float y, float &tx, float &ty);
void turned_from(const Shape &s, float w, float h, float yaw, float pitch, float x, float y, float &sx, float &sy);

// ---- the drawn face --------------------------------------------------------

enum EyeLook { EYE_OPEN, EYE_HALF, EYE_CLOSED, EYE_LOOKS };
enum MouthLook { MOUTH_CLOSED, MOUTH_OPEN, MOUTH_WIDE, MOUTH_SMILE, MOUTH_O, MOUTH_LOOKS };
struct Drawn {
	int eye[2] = {EYE_OPEN, EYE_OPEN}; // the viewer's left eye, right eye
	int mouth = MOUTH_CLOSED;
};
// What your face is doing, from its expressions against "Set" through
// the tuning (`moves`: alive.hpp's tune_moves - 0 at rest, 1 all the
// way). `mirror`: the camera's picture is shown mirrored. `before`: what
// it was, so it doesn't flicker between two.
Drawn drawn_face(const float moves[EXPRESSIONS], bool mirror, const Drawn &before);
// The picture to show for a look, given which there are (`have`: a bit
// per look): the nearest there is, or -1 for none.
int eye_picture(int look, unsigned have);
int mouth_picture(int look, unsigned have);

} // namespace direbot::face
