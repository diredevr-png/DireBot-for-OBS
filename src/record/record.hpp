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

// Clean recordings (rules.hpp says what they are). Each picked source gets
// a small private canvas of its own that shows only that source, an
// encoder (the graphics card's when there is one) and a file beside OBS's
// recordings: "2026-10-30 21-04-11 Camera.mp4".
//
// DireBot runs it from Studio → Clean recordings with the "Record..."
// vendor requests below, and hears "RecordChanged" vendor events back.
// Requests come on any thread and are done on OBS's UI thread.
//
// It works without DireBot too: set up once, it starts with OBS's own
// recording or stream button.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <obs.h>
#include <obs-frontend-api.h>
#include <nlohmann/json.hpp>

#include "rules.hpp"

namespace direbot::record {

class CleanRecordings {
public:
	using Emit = std::function<void(const std::string &, const json &)>;

	CleanRecordings() = default;
	~CleanRecordings();
	CleanRecordings(const CleanRecordings &) = delete;
	CleanRecordings &operator=(const CleanRecordings &) = delete;

	void start(Emit emit);
	void unload();
	void tick();

	static const std::vector<std::string> &requests();
	static bool handles(const std::string &type);
	json request(const std::string &type, const json &data);

private:
	struct Running {
		Pick pick;
		obs_canvas_t *canvas = nullptr;
		obs_encoder_t *video = nullptr;
		obs_encoder_t *audio = nullptr;
		obs_output_t *output = nullptr;
		std::string file;
		uint64_t started_ns = 0;
	};

	Options options_;
	std::vector<Running> running_;
	std::vector<std::string> problems_;
	std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
	std::atomic<bool> tick_queued_{false};
	Emit emit_;
	bool started_ = false;
	bool frontend_ = false;
	bool closing_ = false;
	bool loaded_ = false;

	json do_request(const std::string &type, const json &data);
	json state() const;
	void load_options();
	void save_options();
	// Starts every enabled pick that isn't running. Returns what didn't
	// start and why, in words.
	std::string start_all();
	std::string start_one(const Pick &pick);
	void stop_all();
	void release(Running &r);
	void tidy();
	void changed();
	static void on_frontend(enum obs_frontend_event event, void *self);
	static void on_stop(void *param, calldata_t *cd);
};

} // namespace direbot::record
