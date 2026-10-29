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

// Multistream's places: where your stream goes as well as where OBS
// streams (Settings → Stream). Plain data and plain functions - no OBS -
// so all of it is tested on its own (tests/unit.cpp).
//
// A place's stream key lives in memory here and, locked to the Windows
// account (obs/keystore.hpp), in the OBS profile's
// direbot-multistream.json. It never goes into a report, a log line or
// an error: report() only says whether there is one.

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "../link/protocol.hpp"

namespace direbot::multistream {

using link::json;

// A site the window knows: its name, the server most people use, and
// where its key is.
struct Site {
	const char *key;
	const char *label;
	const char *server; // "" when each streamer gets their own
	const char *where;  // where to find the server and key, in words
	bool needs_h264;    // takes no other video format
};

const std::vector<Site> &sites();
const Site *find_site(const std::string &key);
// The host name only: never anything after it (a key can be in the path).
std::string host_of(const std::string &server);
// "youtube", "kick"... from a place's name, then its server; "other".
std::string guess_site(const std::string &name, const std::string &server);

// Picture choices.
constexpr const char *SAME = "same";       // your stream's encoder: the same picture, no extra work
constexpr const char *LIGHTER = "lighter"; // one smaller encoder every "lighter" place shares
constexpr const char *OWN = "own";         // an encoder of its own

// Sizes offered for a smaller picture (0: your stream's size).
const std::vector<int> &heights();

struct Place {
	std::string id;
	std::string name;
	std::string site = "other";
	std::string server;
	std::string key;        // in memory only
	std::string locked_key; // as it was in the file, when it couldn't be unlocked here
	bool on = true;
	bool with_stream = true;      // starts when you go live
	bool stop_with_stream = true; // ends when you end your stream
	std::string video = SAME;
	int kbps = 0;            // OWN: its video bitrate (0: the lighter one's)
	int height = 0;          // OWN: its height (0: your stream's)
	std::string encoder;     // OWN: an encoder type id ("": the same kind as your stream's)
	int track = 0;           // 0: the same sound as your stream; 1-6: that OBS audio track
	std::string came_from;   // "multi_rtmp" / "aitum": brought over from that add-on

	bool key_set() const { return !key.empty(); }
	bool key_lost() const { return key.empty() && !locked_key.empty(); }
	bool operator==(const Place &o) const;
	bool operator!=(const Place &o) const { return !(*this == o); }
};

struct Lighter {
	int kbps = 3500;
	int height = 720;
	std::string encoder;
	bool operator==(const Lighter &o) const { return kbps == o.kbps && height == o.height && encoder == o.encoder; }
	bool operator!=(const Lighter &o) const { return !(*this == o); }
};

struct Places {
	std::vector<Place> list;
	Lighter lighter;

	Place *find(const std::string &id);
	const Place *find(const std::string &id) const;
	// A new id no place has: 8 letters and digits from `random`.
	std::string new_id(const std::function<std::string(size_t)> &random) const;

	// The file. `lock` turns a key into what's written.
	json to_file(const std::function<std::string(const std::string &)> &lock) const;
	// From the file. `unlock` gives a key back (false: can't here).
	// Anything odd in it is left out or set right, never thrown.
	static Places from_file(const json &file,
				const std::function<bool(const std::string &, std::string &)> &unlock);
};

// Tidies one place: names and sizes in range, a site that's known.
void tidy(Place &p);
void tidy(Lighter &l);

// What a place needs before it can start, in words ("" when ready).
std::string not_ready(const Place &p);

// The id of a place before `p` (switched on) with the same server and the
// same stream key - two streams to one channel - or "". The keys are only
// compared here, inside OBS: DireBot is told the other place's id.
std::string twin_of(const Places &all, const Place &p);

// Multiple RTMP outputs' settings (obs-multi-rtmp.json in the profile)
// and Aitum Multistream's (its config.json, every profile): their places,
// keys and all - read inside OBS, never by DireBot.
std::vector<Place> from_multi_rtmp(const json &file);
std::vector<Place> from_aitum(const json &file, const std::string &profile);

// OBS's output stop codes in words.
std::string stop_words(int code);

} // namespace direbot::multistream
