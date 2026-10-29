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

// DireBot's vertical picture, in place of the Aitum Vertical plugin: a
// tall picture in OBS with scenes of its own (canvas.hpp), streamed to up
// to three places and recorded beside the wide recording (outputs.hpp),
// with the stream keys typed in OBS's Tools menu (settings.hpp). DireBot
// runs it from Studio → Vertical with the "Vertical..." vendor requests
// below, and hears "VerticalChanged" vendor events back.
//
// It works without DireBot too: once set up, OBS keeps the picture and
// its scenes in the scene collection, the Tools menu window sets where it
// goes, and "go live with OBS's stream" starts it with OBS's own button.
//
// Requests come on any thread and are done on OBS's UI thread; they wait
// at most a few seconds for it.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <obs-frontend-api.h>
#include <nlohmann/json.hpp>

#include "../obs/settings-window.hpp"
#include "canvas.hpp"
#include "options.hpp"
#include "outputs.hpp"
#include "settings.hpp"

namespace direbot::vertical {

class Vertical {
public:
	// A vendor event for DireBot: (type, data). Any thread.
	using Emit = std::function<void(const std::string &, const json &)>;

	Vertical();
	~Vertical();
	Vertical(const Vertical &) = delete;
	Vertical &operator=(const Vertical &) = delete;

	// obs_module_post_load, on OBS's UI thread.
	void start(Emit emit);
	// obs_module_unload: whatever OBS closing didn't already do.
	void unload();

	// The vendor requests it answers.
	static const std::vector<std::string> &requests();
	static bool handles(const std::string &type);
	// Any thread except the UI thread waiting on something else.
	json request(const std::string &type, const json &data);

	// From the plugin's clock, about once a second, on its own thread.
	void tick();

private:
	// False once it's going: work queued for later checks it first.
	std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);

	Canvas canvas_;
	Outputs outputs_;
	Options options_;
	SettingsPage page_;
	obs::SettingsWindow window_;
	Emit emit_;

	std::atomic<bool> started_{false};
	std::atomic<bool> closing_{false}; // OBS is closing: nothing new starts
	bool frontend_ = false;
	bool loaded_ = false; // OBS finished loading
	bool main_starting_ = false; // OBS's own stream is connecting
	// From the scene collection (saved with it).
	std::string saved_uuid_, saved_scene_;
	uint32_t width_ = 0, height_ = 0;
	bool follow_ = false;
	std::map<std::string, std::string> links_; // your scene -> tall scene
	// DireBot looking at the picture (Studio → Vertical open) until then.
	uint64_t looking_until_ns_ = 0;
	// The tall streams/recording were started by OBS's own.
	bool streams_with_main_ = false, record_with_main_ = false;
	std::string last_problem_;
	uint64_t next_status_ns_ = 0;
	std::atomic<bool> tick_queued_{false};

	// On the UI thread.
	json do_request(const std::string &type, const json &data);
	json state();
	void attach();
	void let_go(uint32_t wait_ms);
	void leave_profile();
	void update_awake();
	bool twitch_uses_it() const;
	void follow_main(bool animate);
	void load_options();
	void save_options();
	void set_options(const Options &options, bool from_direbot);
	void refresh_window();
	// Tells DireBot (VerticalChanged).
	void changed();
	void on_tick();
	void start_with_main_stream();
	std::string start_streams(int slot);
	std::string start_record();

	static void on_video_reset(void *self, calldata_t *cd);
	static void on_frontend(enum obs_frontend_event event, void *self);
	static void on_save(obs_data_t *data, bool saving, void *self);
};

} // namespace direbot::vertical
