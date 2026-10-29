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

// "DireBot Follow Cam": a source showing part of your camera - the shape
// of the box it's in (9:16 for the whole tall picture, wider for the
// camera's box above a game) - that keeps you in the middle as you move
// (tracker.hpp works out where you are). Your camera itself is left as it
// is, so your usual scenes don't change.
//
// It only does anything while it's shown: the tall picture asleep, or a
// scene with it not on, costs nothing. Ten times a second it looks at a
// small copy of the camera's picture (read back a look later, so OBS
// never waits for it); every frame it draws the crop.
//
// DireBot makes them for "Make my vertical scenes" (vendor GetInfo says
// features.follow_cam), through obs-websocket's CreateInput with kind
// "direbot_follow_cam".

#pragma once

namespace direbot::follow {

constexpr const char *SOURCE_ID = "direbot_follow_cam";

// From obs_module_load, so scene collections find it when they load.
void register_source();

} // namespace direbot::follow
