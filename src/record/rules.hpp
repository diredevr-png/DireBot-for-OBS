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

// Clean recordings: the sources you pick (your camera, your game capture)
// each recorded to a file of their own, with no overlay, alerts or other
// layers on top - for sharper clips later.
//
// What's kept, with each OBS profile in direbot-record.json in the
// profile's folder (like the vertical picture's settings): which sources,
// each one's size and quality and sound, and when they record - with
// OBS's recording, with the stream, or only when started by hand.
//
// Plain data and JSON, no OBS: the unit tests check it.

#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace direbot::record {

using json = nlohmann::json;

// When the clean recordings start and stop.
enum class When { Recording, Stream, Hand };

const char *when_id(When w);
When when_from(const std::string &id);

struct Pick {
	// The source's name in OBS (and its uuid, when known, so a renamed
	// source is still found).
	std::string source;
	std::string uuid;
	// Tallest side in pixels, 0 for the source's own size.
	int height = 0;
	// "high", "medium" or "small" - turned into a bitrate by its size.
	std::string quality = "high";
	// One of OBS's audio tracks (1-6), or 0 for no sound.
	int audio_track = 0;
	bool enabled = true;
};

struct Options {
	static constexpr int MOST = 4;
	std::vector<Pick> picks;
	When when = When::Recording;
	// An encoder id, or "auto": the graphics card's when there is one.
	std::string encoder = "auto";

	json to_json() const;
	static Options from_json(const json &j);
};

// The size a source records at: its own, or scaled down so its height is
// `height` - keeping its shape, never bigger than it is, and even (the
// encoders want even sizes).
void fit(int source_w, int source_h, int height, int &out_w, int &out_h);

// Bitrate in kbps for a quality at a size and frame rate.
int kbps_for(const std::string &quality, int width, int height, double fps);

// The last part of the file name: the source's name, with anything a
// file name can't have taken out. "Clean" when nothing is left.
std::string file_suffix(const std::string &source);

// The encoder to use: the one asked for if OBS has it, else the graphics
// card's (NVIDIA, AMD, Intel, Apple), else x264.
std::string pick_encoder(const std::string &wanted, const std::vector<std::string> &available);

} // namespace direbot::record
