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

#pragma once

#include <cstdint>
#include <string>

namespace direbot::obs {

// Whether a web address points at this PC (DireBot's own overlays do),
// and on which port (0: none given). Only that is ever reported, never
// the address itself - an overlay's address often has a login key in it.
bool local_address(const std::string &url, int &port);

// 0.11.0: the path of a web address ("/overlay" of
// "http://localhost:8080/overlay?x=1"; "/" when there's none), for telling
// which of DireBot's pages a browser source shows. "" if it isn't one.
std::string address_path(const std::string &url);

// 0.11.0: a spot given as parts of a page (0-1 across, 0-1 down) in a
// page `width` x `height` pixels: the pixel. False for a spot that isn't
// on the page (or a page with no size).
bool page_spot(double x, double y, uint32_t width, uint32_t height, int &px, int &py);

} // namespace direbot::obs
