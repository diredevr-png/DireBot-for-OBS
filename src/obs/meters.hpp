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

// Audio levels for the link's InputVolumeMeters event, the one DireBot's
// talking avatar listens to. obs-websocket only measures them for clients
// of its own websocket, so the link measures them itself: with OBS's own
// volume meter (the one in the Audio Mixer), on every audio input that's
// in use, and only while someone is listening.
//
// The event says the same as obs-websocket's: for each input, for each
// channel, [level, peak, peak before the volume slider], as multipliers
// (0 to 1). A muted input reads 0, apart from that last one.

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <obs.h>

namespace direbot::obs {

class Meters {
public:
	Meters();
	~Meters();
	Meters(const Meters &) = delete;
	Meters &operator=(const Meters &) = delete;

	// Start and stop measuring. From one thread at a time (the plugin's
	// ticker); never with a lock held that an OBS signal might want.
	void attach();
	void detach();
	bool attached() const { return attached_; }

	// The event's data, as JSON text: {"inputs":[...]}.
	std::string read();

private:
	struct Meter;
	bool attached_ = false;
	std::mutex lock_;
	std::vector<std::unique_ptr<Meter>> meters_;

	void add(obs_source_t *input);
	static bool wanted(obs_source_t *source);
	static void on_activate(void *self, calldata_t *cd);
	static void on_deactivate(void *self, calldata_t *cd);
};

} // namespace direbot::obs
