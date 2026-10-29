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

// How the vertical picture goes out, kept with each OBS profile (like
// OBS's own stream settings) in direbot-vertical.json in the profile's
// folder: up to three places it streams to, its quality, and whether it
// starts with OBS's stream and records with OBS's recording.
//
// Stream keys are only ever in that file locked (keystore.hpp), and only
// ever typed in OBS's own window. What DireBot is told (to_public) says
// whether a key is there, never what it is, and a server only by its
// host name.
//
// Plain data and JSON, no OBS: the unit tests check it.

#pragma once

#include <array>
#include <string>

#include <nlohmann/json.hpp>

namespace direbot::vertical {

using json = nlohmann::json;

struct Destination {
	std::string platform = "tiktok";
	std::string server;
	// keystore::lock()'s text, never the key itself.
	std::string key_locked;
	bool enabled = false;
};

struct Options {
	static constexpr int SLOTS = 3;
	std::array<Destination, SLOTS> places;
	int kbps = 6000;
	// An encoder id, or "auto": the same kind as the wide stream's.
	std::string encoder = "auto";
	// Which of OBS's audio tracks the tall picture takes its sound from.
	int audio_track = 1;
	// Start and stop with OBS's own stream.
	bool with_stream = false;
	// Record the tall picture whenever OBS records.
	bool record_with_main = false;

	json to_file() const;
	static Options from_file(const json &j);
	// For DireBot: no keys, a server's host only.
	json to_public() const;
	// Ready to stream: on, with a server OBS can send to and a key.
	bool ready(int slot) const;
	bool any_ready() const;
};

} // namespace direbot::vertical
