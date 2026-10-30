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

#include "outputs.hpp"

#include <algorithm>
#include <cstring>
#include <set>

#include <obs-frontend-api.h>
#include <obs.hpp>
#include <util/config-file.h>
#include <util/platform.h>

#include <plugin-support.h>

#include "../obs/keystore.hpp"
#include "../obs/recording.hpp"
#include "rules.hpp"

namespace direbot::vertical {

namespace {

constexpr const char *VIDEO_ENCODER = "direbot_vertical_video";
constexpr const char *AUDIO_ENCODER = "direbot_vertical_audio";

using obs::audio_encoder_id;
using obs::audio_kbps;
using obs::text;
using obs::file_name;
using obs::main_encoder;
using obs::main_encoder_settings;
using obs::record_place;
using obs::RecordPlace;
using obs::set_quality;
using obs::simple_mode;

std::set<std::string> video_encoders()
{
	std::set<std::string> ids;
	for (const EncoderRow &row : h264_encoders())
		ids.insert(row.id);
	// Old ones still work: only as a last resort for their maker.
	for (const char *old : {"jim_nvenc", "ffmpeg_nvenc", "obs_qsv11"})
		if (obs_get_encoder_codec(old))
			ids.insert(old);
	return ids;
}


} // namespace

std::string stop_words(int code, bool recording)
{
	switch (code) {
	case OBS_OUTPUT_SUCCESS:
		return "";
	case OBS_OUTPUT_BAD_PATH:
		return recording ? "OBS couldn't write to the recording folder. Check it in OBS → Settings → Output."
				 : "The server address isn't right. Check it in OBS → Tools → DireBot Vertical.";
	case OBS_OUTPUT_CONNECT_FAILED:
		return "Couldn't reach the server. Check the server address and your internet.";
	case OBS_OUTPUT_INVALID_STREAM:
		return "The server turned the stream down. The stream key may be wrong or out of date "
		       "(TikTok and Instagram give you a new one each time you go live).";
	case OBS_OUTPUT_DISCONNECTED:
		return "The connection dropped and didn't come back.";
	case OBS_OUTPUT_UNSUPPORTED:
		return "The encoder doesn't work for this. Pick another one in OBS → Tools → DireBot Vertical.";
	case OBS_OUTPUT_NO_SPACE:
		return "The drive is full.";
	case OBS_OUTPUT_ENCODE_ERROR:
		return "The encoder stopped working. Pick another one in OBS → Tools → DireBot Vertical, or update "
		       "your graphics driver.";
	default:
		return recording ? "The recording stopped because of an error in OBS."
				 : "The stream stopped because of an error in OBS.";
	}
}

std::vector<EncoderRow> h264_encoders()
{
	std::vector<EncoderRow> rows;
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); i++) {
		const char *codec = obs_get_encoder_codec(id);
		if (obs_get_encoder_type(id) != OBS_ENCODER_VIDEO || !codec || strcmp(codec, "h264") != 0 ||
		    (obs_get_encoder_caps(id) & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL)))
			continue;
		const char *name = obs_encoder_get_display_name(id);
		rows.push_back({id, name ? name : id});
	}
	return rows;
}

std::string auto_encoder()
{
	bool simple = true;
	const std::string main = main_encoder(obs_frontend_get_profile_config(), simple);
	return rules::pick_encoder("auto", simple, main, video_encoders());
}

Outputs::Outputs()
{
	for (int i = 0; i <= Options::SLOTS; i++)
		who_[i] = {this, i};
}

Outputs::~Outputs()
{
	release(0);
}

bool Outputs::make_encoders(video_t *video, const Options &options, std::string &problem)
{
	if (video_ && video && obs_encoder_video(video_) != video) {
		// Made for the picture before a resize: never fed the new one.
		if (obs_encoder_active(video_)) {
			problem = "The vertical picture changed size while it's in use. Stop it, then start again.";
			return false;
		}
		release_encoders();
	}
	if (video_) {
		// Already feeding something: only the bitrate can change now.
		OBSDataAutoRelease s = obs_encoder_get_settings(video_);
		if (obs_data_get_int(s, "bitrate") != options.kbps &&
		    (obs_encoder_get_caps(video_) & OBS_ENCODER_CAP_DYN_BITRATE)) {
			obs_data_set_int(s, "bitrate", options.kbps);
			obs_encoder_update(video_, s);
		}
		return true;
	}
	release_encoders();
	if (!video) {
		problem = "The vertical picture isn't ready yet.";
		return false;
	}

	config_t *config = obs_frontend_get_profile_config();
	bool simple = true;
	const std::string main = main_encoder(config, simple);
	encoder_id_ = rules::pick_encoder(options.encoder, simple, main, video_encoders());

	obs_data_t *settings = main_encoder_settings(config, encoder_id_);
	if (!settings)
		settings = obs_data_create();
	set_quality(settings, encoder_id_, options.kbps, config);
	video_ = obs_video_encoder_create(encoder_id_.c_str(), VIDEO_ENCODER, settings, nullptr);
	obs_data_release(settings);

	if (!video_) {
		release_encoders();
		problem = "OBS couldn't make an encoder for the vertical picture.";
		return false;
	}
	obs_encoder_set_video(video_, video);
	obs_log(LOG_INFO, "Vertical: encoding with %s at %d kbps.", encoder_id_.c_str(), options.kbps);
	return true;
}

obs_encoder_t *Outputs::audio_for(int wanted, int &track, std::string &problem)
{
	config_t *config = obs_frontend_get_profile_config();
	const int stream_track = config ? (int)config_get_int(config, "AdvOut", "TrackIndex") : 1;
	track = rules::sound_track(wanted, simple_mode(config), stream_track);
	obs_encoder_t *&enc = audio_[track - 1];
	if (enc)
		return enc;
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_int(settings, "bitrate", audio_kbps(config, track));
	const std::string name = std::string(AUDIO_ENCODER) + "_" + std::to_string(track);
	enc = obs_audio_encoder_create(audio_encoder_id(), name.c_str(), settings, (size_t)(track - 1), nullptr);
	if (!enc) {
		problem = "OBS couldn't make a sound encoder for audio track " + std::to_string(track) + ".";
		return nullptr;
	}
	obs_encoder_set_audio(enc, obs_get_audio());
	return enc;
}

bool Outputs::audio_active() const
{
	for (obs_encoder_t *enc : audio_)
		if (enc && obs_encoder_active(enc))
			return true;
	return false;
}

void Outputs::release_encoders()
{
	obs_encoder_release(video_);
	video_ = nullptr;
	for (obs_encoder_t *&enc : audio_) {
		obs_encoder_release(enc);
		enc = nullptr;
	}
}

void Outputs::watch(obs_output_t *output, int slot)
{
	Who &who = who_[slot];
	signal_handler_t *sh = obs_output_get_signal_handler(output);
	signal_handler_connect(sh, "start", on_start, &who);
	signal_handler_connect(sh, "stop", on_stop, &who);
	signal_handler_connect(sh, "reconnect", on_reconnect, &who);
	signal_handler_connect(sh, "reconnect_success", on_reconnected, &who);
}

void Outputs::unwatch(obs_output_t *output, int slot)
{
	Who &who = who_[slot];
	signal_handler_t *sh = obs_output_get_signal_handler(output);
	signal_handler_disconnect(sh, "start", on_start, &who);
	signal_handler_disconnect(sh, "stop", on_stop, &who);
	signal_handler_disconnect(sh, "reconnect", on_reconnect, &who);
	signal_handler_disconnect(sh, "reconnect_success", on_reconnected, &who);
}

std::string Outputs::start_streams(video_t *video, const Options &options, int slot)
{
	std::string problems;
	auto note = [&](const std::string &label, const std::string &why) {
		if (!problems.empty())
			problems += " ";
		problems += label + ": " + why;
	};

	config_t *config = obs_frontend_get_profile_config();
	for (int i = 1; i <= Options::SLOTS; i++) {
		if (slot && i != slot)
			continue;
		const Destination &d = options.places[i - 1];
		const std::string label = rules::platform_label(d.platform);
		if (stream_active(i))
			continue;
		if (!options.ready(i)) {
			if (slot)
				note(label, d.key_locked.empty() ? "there's no stream key yet. Type it in OBS → Tools → "
								   "DireBot Vertical."
								 : "it's off, or its server address isn't right.");
			continue;
		}

		std::string key;
		if (!obs::keystore::unlock(d.key_locked, key) || key.empty()) {
			obs::keystore::wipe(key);
			note(label, "the stream key can't be read on this PC. Type it again in OBS → Tools → DireBot "
				    "Vertical.");
			continue;
		}

		std::string problem;
		if (!make_encoders(video, options, problem)) {
			obs::keystore::wipe(key);
			note(label, problem);
			break;
		}
		int track = 0;
		obs_encoder_t *audio = audio_for(d.audio_track, track, problem);
		if (!audio) {
			obs::keystore::wipe(key);
			note(label, problem);
			continue;
		}

		Stream &s = streams_[i - 1];
		release_stream(s, i);
		const std::string name = rules::stream_output_name(i);

		OBSDataAutoRelease service_settings = obs_data_create();
		obs_data_set_string(service_settings, "server", d.server.c_str());
		obs_data_set_string(service_settings, "key", key.c_str());
		obs_data_set_bool(service_settings, "use_auth", false);
		obs::keystore::wipe(key);
		s.service = obs_service_create_private("rtmp_custom", name.c_str(), service_settings);

		OBSDataAutoRelease output_settings = obs_data_create();
		obs_data_set_string(output_settings, "bind_ip", text(config, "Output", "BindIP", "default").c_str());
		obs_data_set_string(output_settings, "ip_family", text(config, "Output", "IPFamily", "IPv4+IPv6").c_str());
		s.output = obs_output_create("rtmp_output", name.c_str(), output_settings, nullptr);
		if (!s.service || !s.output) {
			release_stream(s, i);
			note(label, "OBS couldn't set up the stream.");
			continue;
		}

		const bool reconnect = !config || config_get_bool(config, "Output", "Reconnect");
		const int retries = config ? (int)config_get_uint(config, "Output", "MaxRetries") : 25;
		const int delay = config ? (int)config_get_uint(config, "Output", "RetryDelay") : 2;
		obs_output_set_reconnect_settings(s.output, reconnect ? (retries > 0 ? retries : 25) : 0,
						  delay > 0 ? delay : 2);
		obs_output_set_service(s.output, s.service);
		obs_output_set_video_encoder(s.output, video_);
		obs_output_set_audio_encoder(s.output, audio, 0);
		watch(s.output, i);

		{
			std::lock_guard<std::mutex> hold(lock_);
			s.problem.clear();
			s.reconnecting = false;
			s.started = true;
		}
		s.track = track;
		s.bytes = 0;
		s.at_ns = os_gettime_ns();
		s.kbps = 0;
		if (!obs_output_start(s.output)) {
			{
				std::lock_guard<std::mutex> hold(lock_);
				s.started = false;
			}
			const char *error = obs_output_get_last_error(s.output);
			obs_log(LOG_WARNING, "Vertical: %s wouldn't start.", name.c_str());
			note(label, error && *error && !strstr(error, "rtmp") ? error : "OBS couldn't start the stream.");
			release_stream(s, i);
			continue;
		}
		obs_log(LOG_INFO, "Vertical: streaming to %s (%s).", label.c_str(), rules::host_of(d.server).c_str());
	}
	tidy();
	return problems;
}

void Outputs::stop_streams(int slot)
{
	// A stream still connecting stops too.
	for (int i = 1; i <= Options::SLOTS; i++)
		if ((!slot || i == slot) && stream_active(i))
			obs_output_stop(streams_[i - 1].output);
}

std::string Outputs::start_record(video_t *video, const Options &options)
{
	if (recording())
		return "";
	std::string problem;
	if (!make_encoders(video, options, problem))
		return problem;
	obs_encoder_t *audio = audio_for(options.audio_track, record_track_, problem);
	if (!audio) {
		tidy();
		return problem;
	}

	release_record();
	config_t *config = obs_frontend_get_profile_config();
	const RecordPlace place = record_place(config);
	std::string id = rules::recording_output(place.format);
	std::string ext = rules::recording_extension(place.format);
	if (id != "ffmpeg_muxer" && !obs_get_output_flags(id.c_str())) {
		// An OBS without hybrid MP4/MOV: a fragmented one plays just as well.
		id = "ffmpeg_muxer";
	}
	const std::string path = obs::record_path(place, ext, "Vertical", problem);
	if (path.empty()) {
		tidy();
		return problem;
	}

	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "path", path.c_str());
	std::string muxer = place.muxer;
	const std::string frag = rules::recording_muxer_settings(place.format);
	if (!frag.empty() && muxer.find("movflags") == std::string::npos)
		muxer = muxer.empty() ? frag : frag + " " + muxer;
	else if (id == "ffmpeg_muxer" && ext != "mkv" && place.format.rfind("hybrid", 0) == 0)
		muxer = "movflags=frag_keyframe+empty_moov+delay_moov";
	obs_data_set_string(settings, "muxer_settings", muxer.c_str());

	record_ = obs_output_create(id.c_str(), rules::RECORD_OUTPUT, settings, nullptr);
	if (!record_) {
		tidy();
		return "OBS couldn't set up the recording.";
	}
	obs_output_set_video_encoder(record_, video_);
	obs_output_set_audio_encoder(record_, audio, 0);
	watch(record_, 0);
	{
		std::lock_guard<std::mutex> hold(lock_);
		record_problem_.clear();
		record_started_ = true;
	}
	record_file_ = path;
	record_started_ns_ = os_gettime_ns();
	record_paused_ns_ = 0;
	pause_at_ns_ = 0;
	if (!obs_output_start(record_)) {
		{
			std::lock_guard<std::mutex> hold(lock_);
			record_started_ = false;
		}
		const char *error = obs_output_get_last_error(record_);
		std::string why = error && *error ? error : "OBS couldn't start the recording.";
		release_record();
		tidy();
		return why;
	}
	obs_log(LOG_INFO, "Vertical: recording to %s.", file_name(path).c_str());
	return "";
}

void Outputs::stop_record()
{
	if (recording())
		obs_output_stop(record_);
}

void Outputs::pause_record(bool pause)
{
	if (!record_ || !obs_output_active(record_) || obs_output_paused(record_) == pause)
		return;
	if (!obs_output_pause(record_, pause))
		return;
	const uint64_t now = os_gettime_ns();
	if (pause) {
		pause_at_ns_ = now;
	} else if (pause_at_ns_) {
		record_paused_ns_ += now - pause_at_ns_;
		pause_at_ns_ = 0;
	}
}

bool Outputs::streaming() const
{
	for (int i = 1; i <= Options::SLOTS; i++)
		if (stream_active(i))
			return true;
	return false;
}

bool Outputs::stream_active(int slot) const
{
	if (slot < 1 || slot > Options::SLOTS)
		return false;
	const Stream &s = streams_[slot - 1];
	if (!s.output)
		return false;
	std::lock_guard<std::mutex> hold(lock_);
	return s.started || obs_output_active(s.output);
}

bool Outputs::recording() const
{
	if (!record_)
		return false;
	std::lock_guard<std::mutex> hold(lock_);
	return record_started_ || obs_output_active(record_);
}

bool Outputs::record_paused() const
{
	return record_ && obs_output_active(record_) && obs_output_paused(record_);
}

void Outputs::release_stream(Stream &s, int slot)
{
	if (s.output) {
		unwatch(s.output, slot);
		obs_output_release(s.output);
	}
	obs_service_release(s.service);
	s.output = nullptr;
	s.service = nullptr;
	s.bytes = 0;
	s.kbps = 0;
	std::lock_guard<std::mutex> hold(lock_);
	s.started = false;
	s.reconnecting = false;
}

void Outputs::release_record()
{
	if (record_) {
		unwatch(record_, 0);
		obs_output_release(record_);
	}
	record_ = nullptr;
	std::lock_guard<std::mutex> hold(lock_);
	record_started_ = false;
}

void Outputs::release(uint32_t wait_ms)
{
	stop_streams();
	stop_record();
	// A recording needs a moment to finish its file properly.
	const uint64_t until = os_gettime_ns() + (uint64_t)wait_ms * 1000000ULL;
	while (wait_ms && (recording() || streaming()) && os_gettime_ns() < until)
		os_sleep_ms(20);
	for (int i = 1; i <= Options::SLOTS; i++)
		release_stream(streams_[i - 1], i);
	release_record();
	release_encoders();
}

void Outputs::tidy()
{
	for (int i = 1; i <= Options::SLOTS; i++) {
		Stream &s = streams_[i - 1];
		if (s.output && !stream_active(i))
			release_stream(s, i);
	}
	if (record_ && !recording())
		release_record();
	// Only once they've finished: an encoder let go while it still
	// encodes takes the output's frames with it.
	if (!streaming() && !recording() && !(video_ && obs_encoder_active(video_)) && !audio_active())
		release_encoders();
}

std::string Outputs::encoder_name() const
{
	if (encoder_id_.empty())
		return "";
	const char *name = obs_encoder_get_display_name(encoder_id_.c_str());
	return name ? name : encoder_id_;
}

json Outputs::status(const Options &options)
{
	const uint64_t now = os_gettime_ns();
	json places = json::array();
	for (int i = 1; i <= Options::SLOTS; i++) {
		Stream &s = streams_[i - 1];
		const Destination &d = options.places[i - 1];
		const bool active = s.output && obs_output_active(s.output);
		const bool connecting = !active && stream_active(i);
		int dropped = 0, frames = 0;
		if (active) {
			const uint64_t bytes = obs_output_get_total_bytes(s.output);
			if (s.at_ns && now > s.at_ns && bytes >= s.bytes) {
				const double seconds = (now - s.at_ns) / 1e9;
				if (seconds >= 0.5) {
					s.kbps = (bytes - s.bytes) * 8.0 / 1000.0 / seconds;
					s.bytes = bytes;
					s.at_ns = now;
				}
			}
			dropped = obs_output_get_frames_dropped(s.output);
			frames = obs_output_get_total_frames(s.output);
		}
		std::string problem;
		bool reconnecting;
		{
			std::lock_guard<std::mutex> hold(lock_);
			problem = s.problem;
			reconnecting = s.reconnecting;
		}
		places.push_back({{"slot", i},
				  {"platform", d.platform},
				  {"label", rules::platform_label(d.platform)},
				  {"live", active && !reconnecting},
				  {"active", active},
				  {"connecting", connecting},
				  {"reconnecting", active && reconnecting},
				  {"kbps", active ? (int)(s.kbps + 0.5) : 0},
				  {"track", active ? s.track : 0},
				  {"dropped", dropped},
				  {"frames", frames},
				  {"problem", problem}});
	}

	const bool rec = recording();
	int seconds = 0;
	if (rec && record_started_ns_) {
		uint64_t paused = record_paused_ns_;
		if (pause_at_ns_)
			paused += now - pause_at_ns_;
		const uint64_t ran = now - record_started_ns_;
		seconds = ran > paused ? (int)((ran - paused) / 1000000000ULL) : 0;
	}
	std::string record_problem;
	{
		std::lock_guard<std::mutex> hold(lock_);
		record_problem = record_problem_;
	}
	return {{"streams", places},
		{"streaming", streaming()},
		{"recording",
		 {{"active", rec},
		  {"paused", rec && obs_output_paused(record_)},
		  {"seconds", seconds},
		  {"file", record_file_.empty() ? "" : file_name(record_file_)},
		  {"track", rec ? record_track_ : 0},
		  {"problem", record_problem}}},
		{"encoder", encoder_name()},
		{"encoder_id", encoder_id_}};
}

void Outputs::on_start(void *data, calldata_t *)
{
	auto *who = static_cast<Who *>(data);
	if (who->self->on_change)
		who->self->on_change();
}

void Outputs::on_stop(void *data, calldata_t *cd)
{
	auto *who = static_cast<Who *>(data);
	Outputs *self = who->self;
	const int code = (int)calldata_int(cd, "code");
	obs_output_t *output = (obs_output_t *)calldata_ptr(cd, "output");
	{
		std::lock_guard<std::mutex> hold(self->lock_);
		std::string words = stop_words(code, who->slot == 0);
		// An encoder's own words say what's wrong; a server's might hold
		// its address, so they're not passed on.
		if (code == OBS_OUTPUT_ENCODE_ERROR || (who->slot == 0 && code != OBS_OUTPUT_SUCCESS)) {
			const char *error = output ? obs_output_get_last_error(output) : nullptr;
			if (error && *error)
				words += std::string(" (") + error + ")";
		}
		if (who->slot == 0) {
			self->record_problem_ = words;
			self->record_started_ = false;
		} else {
			Stream &s = self->streams_[who->slot - 1];
			s.problem = words;
			s.reconnecting = false;
			s.started = false;
		}
	}
	if (code != OBS_OUTPUT_SUCCESS)
		obs_log(LOG_WARNING, "Vertical: %s stopped (code %d).",
			who->slot ? rules::stream_output_name(who->slot).c_str() : rules::RECORD_OUTPUT, code);
	if (self->on_change)
		self->on_change();
}

void Outputs::on_reconnect(void *data, calldata_t *)
{
	auto *who = static_cast<Who *>(data);
	if (who->slot > 0) {
		std::lock_guard<std::mutex> hold(who->self->lock_);
		who->self->streams_[who->slot - 1].reconnecting = true;
	}
	if (who->self->on_change)
		who->self->on_change();
}

void Outputs::on_reconnected(void *data, calldata_t *)
{
	auto *who = static_cast<Who *>(data);
	if (who->slot > 0) {
		std::lock_guard<std::mutex> hold(who->self->lock_);
		who->self->streams_[who->slot - 1].reconnecting = false;
	}
	if (who->self->on_change)
		who->self->on_change();
}

} // namespace direbot::vertical
