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

// Secrets typed into OBS - stream keys - kept in a file locked to this
// Windows account, with Windows' own data protection (DPAPI, the same
// lock Windows keeps saved Wi-Fi passwords with). A copy of the file is
// no use on another PC or to another Windows user: the key has to be
// typed in again there.
//
//   lock("live_123")  -> "dpapi:AQAAANCMnd8BFdER..."   (Windows)
//                     -> "plain:live_123"               (elsewhere: tests)
//
// Nothing here logs, and nothing keeps a copy.

#pragma once

#include <string>

namespace direbot::obs::keystore {

// What goes in the file. Empty in, empty out.
std::string lock(const std::string &secret);

// The secret back from lock()'s text. False when it can't be had: locked
// on another PC or Windows account, damaged, or not one of ours. `secret`
// is empty then. Empty text is an empty secret (true).
bool unlock(const std::string &stored, std::string &secret);

// Writes over a string's memory before it's let go.
void wipe(std::string &secret);

} // namespace direbot::obs::keystore
