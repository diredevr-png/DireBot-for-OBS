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

// "DireBot Move": an OBS transition. When the same source is in both
// scenes, it slides, turns and resizes into its new place instead of
// jumping; sources only in the new scene fade, slide or grow in, and ones
// only in the old scene go the same way. How long is OBS's own duration
// box; the easing and how new ones come in are its settings.
//
// Anything that isn't a scene to a scene (a source picked as the
// "scene"), it simply fades.

#pragma once

namespace direbot::moves {

constexpr const char *TRANSITION_ID = "direbot_move_transition";

void register_transition();

} // namespace direbot::moves
