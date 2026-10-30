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

// "DireBot Face Avatar": your character, drawn with your real eyes and
// mouth cut out of your camera and stuck onto it. Only the character is
// drawn - your room and the rest of your face never are - with
// see-through everywhere else.
//
// The character is the streamer's own PNGs: the character (and, if they
// like, pictures of it turned left and right), its own drawn eyes and
// mouth for when the camera can't see you, and outfit layers in front of
// or behind it. Where your eyes and mouth go on it is set by dragging
// boxes in DireBot (Avatar -> Face avatar), which also takes the set-up
// still: a box over each eye and your mouth (tracker.hpp).
//
// Like Follow Cam it only works while it's shown. Thirty times a second
// it reads a small grey copy of the camera's picture (a look later, so
// OBS never waits), and the tracker works on its own thread, so a slow
// look never holds up OBS's picture.
//
// Vendor requests (DireBot):
//   FaceStill {camera}            a picture from the camera to set up on:
//                                 `image` (a BMP as a data: address),
//                                 `width`, `height`, `still` (its id)
//   FaceSetUp {source, still, boxes: [[x, y, w, h] x 3]}
//                                 keeps how your eyes and mouth look (in
//                                 the source's settings), or `error`
//   FaceGetState {source}         what it sees now
// Everything else (pictures, spots, outfit, looks) is the source's own
// settings, set with obs-websocket's SetInputSettings.

#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace direbot::face {

constexpr const char *SOURCE_ID = "direbot_face_avatar";

// From obs_module_load, so scene collections find it when they load.
void register_source();

const std::vector<std::string> &requests();
bool handles(const std::string &type);
// On any thread; the work is done on OBS's UI thread.
nlohmann::json request(const std::string &type, const nlohmann::json &data);

} // namespace direbot::face
