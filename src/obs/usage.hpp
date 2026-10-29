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

// "What's slowing OBS down": how long each source and filter takes OBS
// every frame, from OBS's own source profiler, plus the overall numbers
// from OBS's Stats window.
//
// The profiler costs a little itself, so it's only on while someone is
// looking: the first report turns it on, and it goes off again a minute
// after the last one. If something else had it on already, it's left on.
//
// Nothing private goes in a report: no web addresses (an overlay's
// address often has a login key in it), no file paths, no device names.

#pragma once

#include <cstdint>
#include <mutex>

#include <util/platform.h>

#include "../link/protocol.hpp"

namespace direbot::obs {

using link::json;

class Usage {
public:
	~Usage();

	// The report. {"measuring": true} while the first few seconds are
	// still being measured.
	json report();
	// Once a second or so: turns the profiler off when nobody's asked
	// for a minute.
	void tick();
	// OBS is closing.
	void stop();

private:
	std::mutex lock_;
	bool on_ = false;
	bool ours_ = false;
	uint64_t since_ = 0;
	uint64_t asked_ = 0;
	os_cpu_usage_info_t *cpu_ = nullptr;
	// Frame counters when measuring started, to report since then.
	uint32_t base_total_ = 0, base_lagged_ = 0, base_video_total_ = 0, base_skipped_ = 0;

	void turn_on();
	void turn_off();
};

} // namespace direbot::obs
