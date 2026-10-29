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

// Multistream: your stream to YouTube, Kick and the rest as well as
// where OBS streams, sent by OBS itself from this PC.
//
//   OBS's stream ------------------------------------> Twitch
//   each place: its own output, sharing an encoder ---> YouTube, Kick...
//
// Each place's picture is one of:
//   same    - your stream's own encoder: the same picture, no extra work
//             for the PC, and the most upload;
//   lighter - one smaller encoder (720p at 3,500 Kbps unless changed),
//             shared by every place set to it: one extra encode however
//             many places use it, and less upload for each;
//   own     - an encoder of its own (bitrate, size, which encoder).
// and its sound is your stream's, or one of OBS's six audio tracks: put
// the music on Twitch's track only, and the places on another, and they
// get everything but the music.
//
// Keys are typed into the "DireBot Multistream" window in OBS (Tools
// menu) and kept, locked to the Windows account, in the OBS profile's
// direbot-multistream.json. DireBot asks for the state and presses the
// buttons through vendor requests; no request reads or sets a key or a
// server.

#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <obs.h>
#include <obs-frontend-api.h>

#include "places.hpp"
#include "../obs/settings-window.hpp"

namespace direbot::multistream {

class Multistream : public obs::SettingsPage {
public:
	Multistream();
	~Multistream() override;

	// From obs_module_post_load: the window in OBS, and this profile's
	// places.
	void start();
	// OBS's window's events (its UI thread).
	void on_frontend(enum obs_frontend_event event);
	// About once a second, from the plugin's clock: tidies up after
	// stopped places and saves changes.
	void tick();

	// The vendor requests this part answers (GetMultistream,
	// MultistreamStart...).
	static const std::vector<std::string> &requests();
	// One of them. False when `type` isn't one of this part's.
	bool request(const std::string &type, const json &data, json &answer);

	// The window (obs::SettingsPage).
	obs_properties_t *properties(obs_data_t *settings) override;
	void changed(obs_data_t *settings) override;
	void defaults(obs_data_t *settings) override;
	bool opened(obs_data_t *settings) override;
	void closed(obs_data_t *settings) override;

	// A new place id (the window's Add button).
	std::string new_place_id();

	struct Runner;

private:
	std::mutex lock_;
	Places places_;
	std::string file_;         // this profile's settings file
	std::string file_problem_; // "" or "damaged"
	bool loaded_ = false;
	bool dirty_ = false;
	bool closing_ = false;
	// Places DireBot just added, not in the window's settings yet.
	std::set<std::string> pending_;
	// Keys as they were when the window opened, for its Cancel.
	std::map<std::string, std::pair<std::string, std::string>> snapshot_;
	bool window_open_ = false;
	// Which load() the window's settings are from: the window hears of a
	// profile's places a moment after it loads, and anything it says
	// before that is about the last profile's.
	long long generation_ = 0;
	// Why a place last didn't start or stopped by itself, in words.
	std::map<std::string, std::string> last_error_;

	std::map<std::string, std::unique_ptr<Runner>> runners_;
	// Encoders this part made: "lighter", "own:<id>", "track:<n>".
	std::map<std::string, obs_encoder_t *> encoders_;

	obs::SettingsWindow window_;

	// Under lock_ unless said.
	void load();
	void save();
	void unload(bool force);
	std::string start_place(Place &p, bool by_hand);
	bool stop_place(const std::string &id, bool now);
	void start_with_stream();
	void stop_with_stream();
	Runner *runner(const std::string &id);
	void tidy_runners();
	obs_encoder_t *video_encoder(const std::string &slot, int kbps, int height, const std::string &kind,
				     obs_encoder_t *main_video, std::string &why);
	obs_encoder_t *audio_encoder(int track, obs_encoder_t *main_audio, std::string &why);
	json report();
	json place_report(const Place &p);
	void push_place(const Place &p);
	void push_lighter();
	void push_all();
	std::string import_from(const std::string &from, json &added);
	void apply_lighter_bitrate();
};

} // namespace direbot::multistream
