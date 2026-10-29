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

// Where the vertical picture goes: up to three streams (TikTok, YouTube,
// Instagram...) and its own recording, a file of its own beside OBS's
// wide one, in OBS's recording folder and file type.
//
// One video encoder feeds all of them, and one audio encoder for each of
// OBS's audio tracks they take their sound from (each place picks its
// own), made when the first one starts and let go when the last one
// stops: the tall picture is encoded once however many places it goes,
// and costs nothing while it goes nowhere. The encoder is the same kind as the wide stream's
// (NVENC, AMD, Quick Sync or x264) unless another is picked.
//
// Stream keys come out of their lock (keystore.hpp) only to start a
// stream, straight into OBS's stream settings for it, and are wiped from
// here at once.
//
// On OBS's UI thread, apart from the outputs' own signals, which only
// note what happened and ask for a look (on_change) on the UI thread.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <obs.h>

#include "options.hpp"

namespace direbot::vertical {

class Outputs {
public:
	Outputs();
	~Outputs();
	Outputs(const Outputs &) = delete;
	Outputs &operator=(const Outputs &) = delete;

	// Starts the streams that are ready (or just `slot`, 1-3). Returns
	// what didn't start and why, in words; "" when all did.
	std::string start_streams(video_t *video, const Options &options, int slot = 0);
	void stop_streams(int slot = 0);
	std::string start_record(video_t *video, const Options &options);
	void stop_record();
	void pause_record(bool pause);

	bool streaming() const;
	// Connecting, live or reconnecting.
	bool stream_active(int slot) const;
	bool recording() const;
	bool record_paused() const;
	bool active() const { return streaming() || recording(); }

	// Lets go of everything; stops what's running first (`wait_ms` for a
	// recording to finish its file). For OBS closing or the picture going.
	void release(uint32_t wait_ms = 0);
	// Lets go of the encoders when nothing uses them any more.
	void tidy();

	// How each place and the recording are doing, for DireBot.
	json status(const Options &options);
	std::string encoder_name() const;

	// Something started or stopped: called from any thread.
	std::function<void()> on_change;

private:
	struct Stream {
		obs_output_t *output = nullptr;
		obs_service_t *service = nullptr;
		std::string problem;       // why it last stopped, in words
		bool started = false;      // from its start until its "stop"
		bool reconnecting = false;
		int track = 0;             // the audio track it sends (1-6)
		uint64_t bytes = 0, at_ns = 0;
		double kbps = 0;
	};
	std::array<Stream, Options::SLOTS> streams_;
	obs_output_t *record_ = nullptr;
	bool record_started_ = false;
	std::string record_problem_, record_file_;
	int record_track_ = 0;
	uint64_t record_started_ns_ = 0, record_paused_ns_ = 0, pause_at_ns_ = 0;
	obs_encoder_t *video_ = nullptr;
	// By audio track: [0] is track 1.
	std::array<obs_encoder_t *, 6> audio_{};
	std::string encoder_id_;
	// The problems, started and reconnecting, which signals set.
	mutable std::mutex lock_;

	// Who a signal is about: these outputs, and which place (0 for the
	// recording). Fixed for the Outputs' life, so signals never read the
	// output pointers the UI thread changes.
	struct Who {
		Outputs *self;
		int slot;
	};
	std::array<Who, Options::SLOTS + 1> who_;

	bool make_encoders(video_t *video, const Options &options, std::string &problem);
	// The audio encoder for a place's or the recording's sound (`wanted`
	// 0: the track OBS's own stream sends); `track` says which it is.
	obs_encoder_t *audio_for(int wanted, int &track, std::string &problem);
	bool audio_active() const;
	void release_encoders();
	void release_stream(Stream &s, int slot);
	void release_record();
	// Signals from a place's output (slot 1-3) or the recording's (0).
	void watch(obs_output_t *output, int slot);
	void unwatch(obs_output_t *output, int slot);
	static void on_start(void *data, calldata_t *cd);
	static void on_stop(void *data, calldata_t *cd);
	static void on_reconnect(void *data, calldata_t *cd);
	static void on_reconnected(void *data, calldata_t *cd);
};

// What a stop code means, in words.
std::string stop_words(int code, bool recording);

// The H.264 encoders this OBS has (every site takes H.264), and the one
// "auto" picks now: the same kind as the wide stream's.
struct EncoderRow {
	std::string id, name;
};
std::vector<EncoderRow> h264_encoders();
std::string auto_encoder();

} // namespace direbot::vertical
