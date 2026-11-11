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

// A picture for the graphics card's vertex stage. OBS gives an effect's
// pictures to the pixel stage alone, so a vertex shader that reads one -
// the -skin effect's strand_shown, which looks up how far off the solid
// parts are - reads nothing. On Windows (Direct3D 11) the card itself can
// be told: the picture goes in the vertex stage's first place, until it's
// taken out again. Anywhere else it can't be done, and says so.

#pragma once

#include <obs.h>

namespace direbot::face {

// On OBS's graphics thread. `picture` in the vertex stage's first place
// (the one picture a vertex shader of ours reads): whether it is.
bool card_picture_put(gs_texture_t *picture);
// Taken out again (always, once the drawing that read it is done: a
// picture can't be drawn into while a stage still holds it).
void card_picture_take();

} // namespace direbot::face
