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

// The vertical picture's rules that don't need OBS: which sizes it can
// be, where a stream goes, which tall scene goes with which of your
// scenes, which encoder and file type. Plain functions, so the unit tests
// check them without an OBS (tests/unit.cpp).

#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace direbot::vertical::rules {

// The tall picture's name in OBS (OBS's canvas list, and the "Additional
// canvas" box under Twitch's Enhanced Broadcasting).
constexpr const char *CANVAS_NAME = "DireBot Vertical";
// Its first scene, made with it.
constexpr const char *FIRST_SCENE = "Vertical";
// What DireBot's "Make my vertical scenes" puts in front of a scene's name.
constexpr const char *BUILT_PREFIX = "Vertical - ";
// Every output the vertical picture makes starts with this, so DireBot
// (and its multistream) can tell them from the others in OBS's list.
constexpr const char *OUTPUT_PREFIX = "direbot_vertical_";
constexpr const char *RECORD_OUTPUT = "direbot_vertical_record";

constexpr uint32_t DEFAULT_WIDTH = 1080;
constexpr uint32_t DEFAULT_HEIGHT = 1920;
constexpr int DEFAULT_KBPS = 6000;
constexpr int MIN_KBPS = 500;
constexpr int MAX_KBPS = 20000;

// "direbot_vertical_stream_2" for the second place it goes.
std::string stream_output_name(int slot);

// Sizes it can be: even, 240 to 4096 pixels, and not wider than tall.
bool size_ok(uint32_t width, uint32_t height);

// The server's host name alone ("live.tiktok.com"): what DireBot is told
// about a server, since a key can be written into one's path.
std::string host_of(const std::string &server);

// "TikTok" for "tiktok".
std::string platform_label(const std::string &platform);
// A site's usual server, when it has one for everybody ("" when it gives
// a new one each time, like TikTok and Instagram).
std::string usual_server(const std::string &platform);
// Whether OBS can send there: rtmp:// or rtmps://, with a host.
bool server_ok(const std::string &server);

// The tall scene to put on with one of your usual scenes: the one picked
// for it, else one with the same name, else "Vertical - <name>" (what
// DireBot's scene maker calls it), else "<name> (Vertical)". "" for none.
std::string follow_pick(const std::string &main_scene, const std::vector<std::string> &tall_scenes,
			const std::map<std::string, std::string> &links);

// The H.264 encoder for the tall picture. `wanted` is an encoder id the
// person picked, or "auto": the same kind of encoder as the wide stream
// (`simple`: OBS's Simple output mode, where `main` is its short name
// like "nvenc"; else `main` is the encoder's id). Every site the tall
// picture goes to takes H.264, so an HEVC or AV1 choice becomes the same
// maker's H.264. Only ids in `available` are given; x264 otherwise.
std::string pick_encoder(const std::string &wanted, bool simple, const std::string &main,
			 const std::set<std::string> &available);

// The audio track (1-6) a place or the recording takes its sound from.
// `wanted` 0 is the one OBS's own stream sends: track 1 in Simple output
// mode (`simple`), else `stream_track` (Advanced mode's choice).
int sound_track(int wanted, bool simple, int stream_track);

// A recording's file type, from OBS's own recording setting ("mkv",
// "hybrid_mp4", "fragmented_mov"...): the extension, and the OBS output
// that writes it.
std::string recording_extension(const std::string &format);
std::string recording_output(const std::string &format);
// Muxer settings for a fragmented MP4/MOV, as OBS writes them.
std::string recording_muxer_settings(const std::string &format);

} // namespace direbot::vertical::rules
