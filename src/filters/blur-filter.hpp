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

// "DireBot Blur": a filter that blurs a source or turns it into big
// squares. DireBot's stream tricks (Blur, Pixelate) add it for a few
// seconds and take it off again; anyone can add it in OBS too.
//
//   settings: "mode" ("blur" or "pixelate"), "amount" (0-100: how strong,
//   as pixels at 1080p - bigger pictures get more, smaller ones less)

#pragma once

namespace direbot::filters {

constexpr const char *BLUR_ID = "direbot_blur_filter";

void register_blur();

} // namespace direbot::filters
