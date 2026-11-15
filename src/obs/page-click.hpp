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


// 0.11.0: a click on one of DireBot's own pages in OBS.
//
// Twitch's clip player waits on its play button inside OBS: autoplay,
// muted or not, never starts it, and one click does (OBS's Interact
// window). DireBot's clip queue asks for that click with ClickPage: the
// add-on finds the browser sources showing that page of DireBot's (this
// PC, DireBot's port, the same path) and clicks each one where it's told,
// the way the Interact window does.
//
// Only DireBot's own pages on this PC are ever clicked, never anyone
// else's; nothing about a source's address is ever reported (it can hold
// a login key). And only a page on the stream now (in the program's
// scene, shown): Twitch lets a player start only where it can be seen -
// and, given the player's size, only one drawn at least as big as its
// site allows (Twitch's: 400 x 300).

#pragma once

#include <string>
#include <vector>

#include "../link/protocol.hpp"

namespace direbot::obs {

using link::json;

// The request names, for registering.
const std::vector<std::string> &page_click_requests();
bool page_click_handles(const std::string &type);

// {"port": DireBot's port, "path": "/overlay", "x": 0-1, "y": 0-1,
//  and optionally "w", "h": the player's size as parts of the page, with
//  "min_w", "min_h": the smallest it may be drawn on the stream, pixels}
//   -> {"ok": true, "clicked": how many sources, "hidden": how many weren't
//       on the stream, "small": how many showed the player too small}
//   or {"ok": false, "error"}.
json page_click(const std::string &type, const json &data);

} // namespace direbot::obs
