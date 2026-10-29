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

// The few bits of hashing the link needs, with no library behind them:
// SHA-1 (only for the WebSocket handshake, which requires it), SHA-256
// (the obs-websocket login), base64, and random bytes from the system.

#pragma once

#include <cstdint>
#include <string>

namespace direbot::crypto {

std::string sha1(const std::string &data);   // 20 raw bytes
std::string sha256(const std::string &data); // 32 raw bytes

std::string base64(const std::string &data);
// The same, with - and _ and no padding: safe in a file and a URL.
std::string base64url(const std::string &data);

// `count` bytes from the system's secure random source. Empty if it
// can't give them (never happens in practice; callers refuse to start).
std::string random_bytes(size_t count);

// Equal, taking the same time whatever the strings hold.
bool same(const std::string &a, const std::string &b);

} // namespace direbot::crypto
