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

#include "multistream.hpp"

#include <algorithm>
#include <cstring>

#include <obs-module.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <plugin-support.h>

#include "../link/crypto.hpp"
#include "../obs/keystore.hpp"

namespace direbot::multistream {

namespace {

// What DireBot can count on from these requests.
constexpr int API = 1;
constexpr const char *FILE_NAME = "direbot-multistream.json";
// Outputs and encoders this part makes are called this, then the
// place's id (or "lighter", "track2"...): DireBot tells them apart by it.
constexpr const char *PREFIX = "direbot_multistream_";
constexpr const char *WINDOW_ID = "direbot_multistream_settings";
constexpr const char *WINDOW_TITLE = "DireBot Multistream";
constexpr size_t MAX_PLACES = 12;

enum class State { Starting, Sending, Reconnecting, Stopping, Stopped };

const char *state_name(State s)
{
	switch (s) {
	case State::Starting:
		return "starting";
	case State::Sending:
		return "sending";
	case State::Reconnecting:
		return "reconnecting";
	case State::Stopping:
		return "stopping";
	default:
		return "off";
	}
}

std::string key_of(const std::string &id, const char *what)
{
	return "p_" + id + "_" + what;
}

std::vector<std::string> split_ids(const char *text)
{
	std::vector<std::string> out;
	std::string cur;
	for (const char *c = text ? text : ""; *c; c++) {
		if (*c == ',') {
			if (!cur.empty() && std::find(out.begin(), out.end(), cur) == out.end())
				out.push_back(cur);
			cur.clear();
		} else if (std::isalnum((unsigned char)*c)) {
			cur.push_back(*c);
		}
	}
	if (!cur.empty() && std::find(out.begin(), out.end(), cur) == out.end())
		out.push_back(cur);
	return out;
}

std::string join_ids(const std::vector<Place> &list)
{
	std::string out;
	for (const Place &p : list)
		out += (out.empty() ? "" : ",") + p.id;
	return out;
}

std::string str(const char *s)
{
	return s ? std::string(s) : std::string();
}

// OBS's stream, as far as the places need it.
struct Main {
	obs_output_t *output = nullptr; // ours to release
	obs_encoder_t *video = nullptr;
	obs_encoder_t *audio = nullptr;
	bool multitrack = false;
	bool live = false;

	Main()
	{
		live = obs_frontend_streaming_active();
		output = obs_frontend_get_streaming_output();
		if (output) {
			video = obs_output_get_video_encoder(output);
			audio = obs_output_get_audio_encoder(output, 0);
			// Enhanced Broadcasting: several pictures at once, made
			// for Twitch alone - not shared.
			multitrack = obs_output_get_video_encoder2(output, 1) != nullptr;
		}
	}
	~Main() { obs_output_release(output); }
	Main(const Main &) = delete;
	Main &operator=(const Main &) = delete;

	// The audio track your stream sends (Settings → Output; always track 1
	// in Simple mode).
	static int track()
	{
		config_t *profile = obs_frontend_get_profile_config();
		const char *mode = profile ? config_get_string(profile, "Output", "Mode") : nullptr;
		if (!mode || std::strcmp(mode, "Advanced") != 0)
			return 1;
		const int t = (int)config_get_int(profile, "AdvOut", "TrackIndex");
		return t >= 1 && t <= 6 ? t : 1;
	}
};

int bitrate_of(obs_encoder_t *enc)
{
	if (!enc)
		return 0;
	obs_data_t *s = obs_encoder_get_settings(enc);
	int kbps = s ? (int)obs_data_get_int(s, "bitrate") : 0;
	obs_data_release(s);
	return kbps;
}

std::string codec_label(const char *codec)
{
	const std::string c = str(codec);
	if (c == "h264")
		return "H.264";
	if (c == "hevc")
		return "HEVC";
	if (c == "av1")
		return "AV1";
	return c.empty() ? "another format" : c;
}

// An H.264 encoder: `wanted` if it is one, else the same maker's as your
// stream's (NVIDIA's, Intel's, AMD's, Apple's), else x264 (the processor).
std::string pick_h264(const std::string &wanted, obs_encoder_t *main_video)
{
	auto usable = [](const char *id) {
		if (!id || obs_get_encoder_type(id) != OBS_ENCODER_VIDEO)
			return false;
		if (str(obs_get_encoder_codec(id)) != "h264")
			return false;
		return (obs_get_encoder_caps(id) & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL)) == 0;
	};
	if (!wanted.empty() && usable(wanted.c_str()))
		return wanted;
	const std::string main_id = main_video ? str(obs_encoder_get_id(main_video)) : std::string();
	if (!main_id.empty() && usable(main_id.c_str()))
		return main_id;
	std::string family;
	for (const char *f : {"nvenc", "qsv", "amf", "videotoolbox", "vaapi"})
		if (main_id.find(f) != std::string::npos)
			family = f;
	std::string any;
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); i++) {
		if (!usable(id))
			continue;
		if (!family.empty() && str(id).find(family) != std::string::npos)
			return id;
		if (any.empty())
			any = id;
	}
	if (usable("obs_x264"))
		return "obs_x264";
	return any;
}

// The H.264 encoders this OBS has, for the window and DireBot.
json h264_encoders()
{
	json out = json::array();
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); i++) {
		if (obs_get_encoder_type(id) != OBS_ENCODER_VIDEO || str(obs_get_encoder_codec(id)) != "h264")
			continue;
		if (obs_get_encoder_caps(id) & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL))
			continue;
		out.push_back({{"id", id}, {"name", str(obs_encoder_get_display_name(id))}});
	}
	return out;
}

std::string profile_dir()
{
	char *path = obs_frontend_get_current_profile_path();
	std::string out = str(path);
	bfree(path);
	return out;
}

std::string profile_name()
{
	char *name = obs_frontend_get_current_profile();
	std::string out = str(name);
	bfree(name);
	return out;
}

json read_json(const std::string &path, bool &found)
{
	found = false;
	char *text = os_quick_read_utf8_file(path.c_str());
	if (!text)
		return nullptr;
	found = true;
	json j = json::parse(text, nullptr, false);
	bfree(text);
	return j.is_discarded() ? json(nullptr) : j;
}

// What OBS's audio tracks are called (Settings → Output, Advanced).
std::string track_name(int track)
{
	config_t *profile = obs_frontend_get_profile_config();
	std::string key = "Track" + std::to_string(track) + "Name";
	const char *name = profile ? config_get_string(profile, "AdvOut", key.c_str()) : nullptr;
	return str(name);
}

bool profile_bool(const char *section, const char *name, bool fallback)
{
	config_t *profile = obs_frontend_get_profile_config();
	if (!profile || !config_has_user_value(profile, section, name)) {
		if (profile && config_has_default_value(profile, section, name))
			return config_get_bool(profile, section, name);
		return fallback;
	}
	return config_get_bool(profile, section, name);
}

int profile_int(const char *section, const char *name, int fallback)
{
	config_t *profile = obs_frontend_get_profile_config();
	if (!profile || (!config_has_user_value(profile, section, name) &&
			 !config_has_default_value(profile, section, name)))
		return fallback;
	return (int)config_get_int(profile, section, name);
}

// A message from OBS, unless it has the server or the key in it.
std::string safe_words(const char *raw, const Place &p)
{
	std::string text = str(raw);
	if (text.empty() || text.size() > 300)
		return std::string();
	if ((!p.key.empty() && text.find(p.key) != std::string::npos) ||
	    (!p.server.empty() && text.find(p.server) != std::string::npos))
		return std::string();
	return text;
}

} // namespace

struct Multistream::Runner {
	std::string id;
	std::string name;
	std::string host;
	std::string video; // the picture it's sending: same / lighter / own
	int kbps = 0;      // its video bitrate
	obs_output_t *output = nullptr;
	obs_service_t *service = nullptr;
	std::string video_slot; // the encoders it uses that this part made
	std::string audio_slot;
	// Changed by the output's own threads.
	std::mutex lock;
	State state = State::Starting;
	uint64_t since_ns = 0;
	int code = 0;
	bool by_you = false;
	bool connected = false; // signals hooked up

	static void on_start(void *param, calldata_t *)
	{
		auto *r = static_cast<Runner *>(param);
		{
			std::lock_guard<std::mutex> l(r->lock);
			r->state = State::Sending;
			r->since_ns = os_gettime_ns();
			r->code = 0;
		}
		obs_log(LOG_INFO, "Multistream: sending to %s.", r->name.c_str());
	}

	static void on_stop(void *param, calldata_t *cd)
	{
		auto *r = static_cast<Runner *>(param);
		const int code = (int)calldata_int(cd, "code");
		bool by_you;
		{
			std::lock_guard<std::mutex> l(r->lock);
			r->state = State::Stopped;
			r->code = code;
			by_you = r->by_you;
		}
		if (code && !by_you)
			obs_log(LOG_WARNING, "Multistream: %s stopped (%d): %s", r->name.c_str(), code,
				stop_words(code).c_str());
		else
			obs_log(LOG_INFO, "Multistream: %s stopped.", r->name.c_str());
	}

	static void on_stopping(void *param, calldata_t *)
	{
		auto *r = static_cast<Runner *>(param);
		std::lock_guard<std::mutex> l(r->lock);
		if (r->state != State::Stopped)
			r->state = State::Stopping;
	}

	static void on_reconnect(void *param, calldata_t *)
	{
		auto *r = static_cast<Runner *>(param);
		{
			std::lock_guard<std::mutex> l(r->lock);
			r->state = State::Reconnecting;
		}
		obs_log(LOG_WARNING, "Multistream: %s lost its connection, reconnecting.", r->name.c_str());
	}

	static void on_reconnected(void *param, calldata_t *)
	{
		auto *r = static_cast<Runner *>(param);
		{
			std::lock_guard<std::mutex> l(r->lock);
			r->state = State::Sending;
		}
		obs_log(LOG_INFO, "Multistream: %s reconnected.", r->name.c_str());
	}

	void hook(bool on)
	{
		if (!output || on == connected)
			return;
		signal_handler_t *sh = obs_output_get_signal_handler(output);
		auto both = [&](const char *signal, signal_callback_t cb) {
			if (on)
				signal_handler_connect(sh, signal, cb, this);
			else
				signal_handler_disconnect(sh, signal, cb, this);
		};
		both("start", on_start);
		both("stop", on_stop);
		both("stopping", on_stopping);
		both("reconnect", on_reconnect);
		both("reconnect_success", on_reconnected);
		connected = on;
	}

	State now()
	{
		std::lock_guard<std::mutex> l(lock);
		return state;
	}

	// Let go of the output, then the service it points at.
	void release()
	{
		hook(false);
		if (output && obs_output_active(output))
			obs_output_force_stop(output);
		obs_output_release(output);
		output = nullptr;
		obs_service_release(service);
		service = nullptr;
	}

	~Runner() { release(); }
};

Multistream::Multistream() : window_(WINDOW_ID, WINDOW_TITLE, this) {}

Multistream::~Multistream()
{
	unload(true);
	window_.stop();
}

const std::vector<std::string> &Multistream::requests()
{
	static const std::vector<std::string> names = {"GetMultistream",    "MultistreamStart", "MultistreamStop",
						       "MultistreamStopAll", "MultistreamAdd",   "MultistreamSet",
						       "MultistreamOpen",    "MultistreamImport"};
	return names;
}

std::string Multistream::new_place_id()
{
	std::lock_guard<std::mutex> l(lock_);
	return places_.new_id([](size_t n) { return crypto::random_bytes(n); });
}

void Multistream::start()
{
	window_.start();
	std::lock_guard<std::mutex> l(lock_);
	load();
}

// ---- the profile's file ----------------------------------------------------------

void Multistream::load()
{
	const std::string dir = profile_dir();
	file_ = dir.empty() ? std::string() : dir + "/" + FILE_NAME;
	places_ = Places();
	file_problem_.clear();
	generation_++;
	pending_.clear();
	loaded_ = true;
	dirty_ = false;
	last_error_.clear();
	if (!file_.empty()) {
		bool found = false;
		json j = read_json(file_, found);
		if (found && !j.is_object()) {
			// Kept beside, never written over.
			std::string aside = file_ + ".damaged";
			os_unlink(aside.c_str());
			os_rename(file_.c_str(), aside.c_str());
			file_problem_ = "damaged";
			obs_log(LOG_WARNING, "Multistream: this profile's %s was damaged; it's kept as %s.damaged.",
				FILE_NAME, FILE_NAME);
		} else if (found) {
			places_ = Places::from_file(j, [](const std::string &locked, std::string &key) {
				return obs::keystore::unlock(locked, key);
			});
		}
	}
	size_t lost = 0;
	for (const Place &p : places_.list)
		lost += p.key_lost() ? 1 : 0;
	if (!places_.list.empty())
		obs_log(LOG_INFO, "Multistream: %zu place(s) in this profile.", places_.list.size());
	if (lost)
		obs_log(LOG_WARNING,
			"Multistream: %zu stream key(s) were typed in on another PC or Windows account and need typing "
			"in again.",
			lost);
	push_all();
}

void Multistream::save()
{
	if (!dirty_ || file_.empty())
		return;
	dirty_ = false;
	const std::string body =
		places_.to_file([](const std::string &key) { return obs::keystore::lock(key); })
			.dump(1, ' ', false, json::error_handler_t::replace);
	if (!os_quick_write_utf8_file_safe(file_.c_str(), body.c_str(), body.size(), false, "tmp", nullptr))
		obs_log(LOG_WARNING, "Multistream: couldn't save this profile's places.");
}

// Every place stops (OBS closing, or another profile), and the encoders
// go.
void Multistream::unload(bool force)
{
	for (auto &it : runners_) {
		Runner &r = *it.second;
		if (r.now() != State::Stopped) {
			{
				std::lock_guard<std::mutex> l(r.lock);
				r.by_you = true;
			}
			if (force)
				obs_output_force_stop(r.output);
			else
				obs_output_stop(r.output);
		}
	}
	if (force) {
		runners_.clear();
		for (auto &e : encoders_)
			obs_encoder_release(e.second);
		encoders_.clear();
	}
}

// ---- OBS's events -----------------------------------------------------------------

void Multistream::on_frontend(enum obs_frontend_event event)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING: {
		std::lock_guard<std::mutex> l(lock_);
		if (!loaded_ || file_.empty())
			load();
		break;
	}
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		start_with_stream();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
		stop_with_stream();
		break;
	case OBS_FRONTEND_EVENT_PROFILE_CHANGING: {
		std::lock_guard<std::mutex> l(lock_);
		save();
		unload(true);
		break;
	}
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED: {
		std::lock_guard<std::mutex> l(lock_);
		load();
		break;
	}
	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
	case OBS_FRONTEND_EVENT_EXIT: {
		{
			std::lock_guard<std::mutex> l(lock_);
			if (closing_)
				break;
			closing_ = true;
			save();
			unload(true);
		}
		if (event == OBS_FRONTEND_EVENT_EXIT)
			window_.stop();
		break;
	}
	default:
		break;
	}
}

void Multistream::tick()
{
	std::lock_guard<std::mutex> l(lock_);
	if (closing_)
		return;
	tidy_runners();
	save();
}

void Multistream::start_with_stream()
{
	std::lock_guard<std::mutex> l(lock_);
	for (Place &p : places_.list) {
		if (!p.on || !p.with_stream)
			continue;
		std::string why = start_place(p, false);
		if (!why.empty()) {
			last_error_[p.id] = why;
			obs_log(LOG_WARNING, "Multistream: %s didn't start with your stream: %s", p.name.c_str(),
				why.c_str());
		}
	}
}

void Multistream::stop_with_stream()
{
	std::lock_guard<std::mutex> l(lock_);
	for (const Place &p : places_.list)
		if (p.stop_with_stream)
			stop_place(p.id, false);
}

// ---- starting and stopping ---------------------------------------------------------

Multistream::Runner *Multistream::runner(const std::string &id)
{
	auto it = runners_.find(id);
	return it == runners_.end() ? nullptr : it->second.get();
}

void Multistream::tidy_runners()
{
	for (auto it = runners_.begin(); it != runners_.end();) {
		Runner &r = *it->second;
		State s;
		int code;
		bool by_you;
		{
			std::lock_guard<std::mutex> l(r.lock);
			s = r.state;
			code = r.code;
			by_you = r.by_you;
		}
		if (s != State::Stopped || (r.output && obs_output_active(r.output))) {
			++it;
			continue;
		}
		if (code && !by_you)
			last_error_[r.id] = stop_words(code);
		it = runners_.erase(it);
	}
	// The encoders nothing uses any more.
	for (auto it = encoders_.begin(); it != encoders_.end();) {
		bool used = false;
		for (auto &r : runners_)
			used = used || r.second->video_slot == it->first || r.second->audio_slot == it->first;
		if (used) {
			++it;
			continue;
		}
		obs_encoder_release(it->second);
		it = encoders_.erase(it);
	}
}

obs_encoder_t *Multistream::video_encoder(const std::string &slot, int kbps, int height, const std::string &kind,
					  obs_encoder_t *main_video, std::string &why)
{
	auto have = encoders_.find(slot);
	if (have != encoders_.end())
		return have->second;
	const std::string id = pick_h264(kind, main_video);
	if (id.empty()) {
		why = "This OBS has no H.264 encoder to make a lighter picture with.";
		return nullptr;
	}
	obs_data_t *settings = obs_data_create();
	if (main_video && id == str(obs_encoder_get_id(main_video))) {
		// The same kind as your stream's: its settings, smaller.
		obs_data_t *main = obs_encoder_get_settings(main_video);
		obs_data_apply(settings, main);
		obs_data_release(main);
	}
	obs_data_set_int(settings, "bitrate", kbps);
	obs_data_set_string(settings, "rate_control", "CBR");
	if (obs_data_get_int(settings, "keyint_sec") <= 0)
		obs_data_set_int(settings, "keyint_sec", 2);
	std::string name = PREFIX + slot;
	std::replace(name.begin(), name.end(), ':', '_');
	obs_encoder_t *enc = obs_video_encoder_create(id.c_str(), name.c_str(), settings, nullptr);
	obs_data_release(settings);
	if (!enc) {
		why = "OBS couldn't make an encoder for the lighter picture.";
		return nullptr;
	}
	obs_encoder_set_video(enc, obs_get_video());
	obs_video_info ovi = {};
	if (height > 0 && obs_get_video_info(&ovi) && ovi.output_height > (uint32_t)height) {
		uint32_t width = (uint32_t)((uint64_t)ovi.output_width * (uint32_t)height / ovi.output_height);
		width -= width % 2;
		obs_encoder_set_gpu_scale_type(enc, OBS_SCALE_BICUBIC);
		obs_encoder_set_scaled_size(enc, width, (uint32_t)height);
	}
	encoders_[slot] = enc;
	return enc;
}

obs_encoder_t *Multistream::audio_encoder(int track, obs_encoder_t *main_audio, std::string &why)
{
	const std::string slot = "track:" + std::to_string(track);
	auto have = encoders_.find(slot);
	if (have != encoders_.end())
		return have->second;
	std::string id = "ffmpeg_aac";
	if (main_audio && str(obs_encoder_get_codec(main_audio)) == "aac")
		id = str(obs_encoder_get_id(main_audio));
	obs_data_t *settings = obs_data_create();
	const int kbps = bitrate_of(main_audio);
	obs_data_set_int(settings, "bitrate", kbps > 0 ? kbps : 160);
	std::string name = std::string(PREFIX) + "track" + std::to_string(track);
	obs_encoder_t *enc =
		obs_audio_encoder_create(id.c_str(), name.c_str(), settings, (size_t)(track - 1), nullptr);
	obs_data_release(settings);
	if (!enc) {
		why = "OBS couldn't make a sound encoder for audio track " + std::to_string(track) + ".";
		return nullptr;
	}
	obs_encoder_set_audio(enc, obs_get_audio());
	encoders_[slot] = enc;
	return enc;
}

std::string Multistream::start_place(Place &p, bool by_hand)
{
	if (closing_)
		return "OBS is closing.";
	tidy_runners();
	if (Runner *r = runner(p.id))
		if (r->now() != State::Stopped)
			return std::string(); // already going
	if (!p.on)
		return p.name + " is switched off in DireBot Multistream.";
	std::string why = not_ready(p);
	if (!why.empty())
		return why;

	Main main;
	const Site *site = find_site(p.site);
	std::string video = p.video;
	// Your stream's picture can't be shared when it's several (Enhanced
	// Broadcasting), or when the site can't play its format.
	if (video == SAME && (main.multitrack || !main.video))
		video = LIGHTER;
	if (video == SAME && site && site->needs_h264 && str(obs_encoder_get_codec(main.video)) != "h264")
		video = LIGHTER;
	obs_encoder_t *venc = nullptr;
	std::string vslot;
	int kbps = 0;
	if (video == SAME) {
		if (!main.live)
			return "Go live first: " + p.name +
			       " sends your stream's own picture, so it starts once your stream has. (With a Lighter "
			       "picture and a sound track of its own, a place can start by itself.)";
		venc = main.video;
		kbps = bitrate_of(venc);
	} else {
		const bool own = video == OWN;
		vslot = own ? "own:" + p.id : std::string(LIGHTER);
		kbps = own && p.kbps ? p.kbps : places_.lighter.kbps;
		venc = video_encoder(vslot, kbps, own ? p.height : places_.lighter.height,
				     own ? p.encoder : places_.lighter.encoder, main.video, why);
		if (!venc)
			return why;
	}
	if (site && site->needs_h264 && str(obs_encoder_get_codec(venc)) != "h264")
		return std::string(site->label) + " only takes H.264 video, and that encoder makes " +
		       codec_label(obs_encoder_get_codec(venc)) + ". Pick another encoder for " + p.name + ".";

	obs_encoder_t *aenc = nullptr;
	std::string aslot;
	if (p.track == 0 || (main.audio && p.track == main.track())) {
		if (!main.live || !main.audio)
			return "Go live first: " + p.name +
			       " sends your stream's own sound, so it starts once your stream has.";
		aenc = main.audio;
	} else {
		aslot = "track:" + std::to_string(p.track);
		aenc = audio_encoder(p.track, main.audio, why);
		if (!aenc)
			return why;
	}

	// The server and key go to OBS's own connection, in memory, and no
	// further.
	const std::string name = PREFIX + p.id;
	obs_data_t *ss = obs_data_create();
	obs_data_set_string(ss, "server", p.server.c_str());
	obs_data_set_string(ss, "key", p.key.c_str());
	obs_data_set_bool(ss, "use_auth", false);
	obs_data_set_bool(ss, "bwtest", false);
	obs_service_t *service = obs_service_create_private("rtmp_custom", name.c_str(), ss);
	obs_data_release(ss);
	if (!service)
		return "OBS couldn't set up the connection for " + p.name + ".";
	const char *type = obs_service_get_preferred_output_type(service);
	if (!type || !*type)
		type = "rtmp_output";

	obs_data_t *os = obs_data_create();
	// Dynamic bitrate would lower a shared encoder's bitrate - Twitch's
	// too - for one site's bad connection.
	obs_data_set_bool(os, "dyn_bitrate", false);
	config_t *profile = obs_frontend_get_profile_config();
	if (profile) {
		const char *bind = config_get_string(profile, "Output", "BindIP");
		if (bind && *bind)
			obs_data_set_string(os, "bind_ip", bind);
		const char *family = config_get_string(profile, "Output", "IPFamily");
		if (family && *family)
			obs_data_set_string(os, "ip_family", family);
	}
	obs_output_t *out = obs_output_create(type, name.c_str(), os, nullptr);
	obs_data_release(os);
	if (!out) {
		obs_service_release(service);
		return "OBS couldn't make an output for " + p.name + ".";
	}
	obs_output_set_service(out, service);
	obs_output_set_video_encoder(out, venc);
	obs_output_set_audio_encoder(out, aenc, 0);
	// Reconnecting and the stream delay as OBS's own stream has them.
	if (profile_bool("Output", "Reconnect", true))
		obs_output_set_reconnect_settings(out, profile_int("Output", "MaxRetries", 25),
						  profile_int("Output", "RetryDelay", 2));
	else
		obs_output_set_reconnect_settings(out, 0, 0);
	if (profile_bool("Output", "DelayEnable", false))
		obs_output_set_delay(out, (uint32_t)std::max(0, profile_int("Output", "DelaySec", 20)),
				     profile_bool("Output", "DelayPreserve", true) ? OBS_OUTPUT_DELAY_PRESERVE : 0);

	auto r = std::make_unique<Runner>();
	r->id = p.id;
	r->name = p.name;
	r->host = host_of(p.server);
	r->video = video;
	r->kbps = kbps;
	r->output = out;
	r->service = service;
	r->video_slot = vslot;
	r->audio_slot = aslot;
	r->hook(true);
	Runner &run = *r;
	runners_[p.id] = std::move(r);
	last_error_.erase(p.id);
	obs_log(LOG_INFO, "Multistream: starting %s (%s, %s picture, %s)%s.", p.name.c_str(),
		run.host.empty() ? "?" : run.host.c_str(), video.c_str(),
		p.track ? ("audio track " + std::to_string(p.track)).c_str() : "your stream's sound",
		by_hand ? "" : " with your stream");
	if (!obs_output_start(out)) {
		std::string said = safe_words(obs_output_get_last_error(out), p);
		{
			std::lock_guard<std::mutex> l(run.lock);
			run.state = State::Stopped;
		}
		why = "OBS couldn't start " + p.name + (said.empty() ? "." : ": " + said);
		last_error_[p.id] = why;
		obs_log(LOG_WARNING, "Multistream: couldn't start %s.", p.name.c_str());
		tidy_runners();
		return why;
	}
	return std::string();
}

bool Multistream::stop_place(const std::string &id, bool now)
{
	Runner *r = runner(id);
	if (!r || !r->output)
		return false;
	{
		std::lock_guard<std::mutex> l(r->lock);
		if (r->state == State::Stopped)
			return false;
		r->state = State::Stopping;
		r->by_you = true;
	}
	if (now)
		obs_output_force_stop(r->output);
	else
		obs_output_stop(r->output);
	return true;
}

void Multistream::apply_lighter_bitrate()
{
	auto it = encoders_.find(LIGHTER);
	if (it == encoders_.end())
		return;
	obs_data_t *s = obs_data_create();
	obs_data_set_int(s, "bitrate", places_.lighter.kbps);
	obs_encoder_update(it->second, s);
	obs_data_release(s);
}

// ---- what DireBot asks ---------------------------------------------------------------

json Multistream::place_report(const Place &p)
{
	const Site *site = find_site(p.site);
	const std::string server = p.server;
	const size_t scheme = server.find("://");
	std::string protocol = scheme == std::string::npos ? "RTMP" : server.substr(0, scheme);
	for (char &c : protocol)
		c = (char)std::toupper((unsigned char)c);
	json out = {{"id", p.id},
		    {"name", p.name},
		    {"site", p.site},
		    {"label", site ? site->label : "Another site"},
		    {"host", host_of(p.server)},
		    {"protocol", protocol.substr(0, 10)},
		    {"on", p.on},
		    {"with_stream", p.with_stream},
		    {"stop_with_stream", p.stop_with_stream},
		    {"key_set", p.key_set()},
		    {"key_lost", p.key_lost()},
		    {"server_set", !p.server.empty()},
		    {"video", p.video},
		    {"kbps", p.kbps},
		    {"height", p.height},
		    {"encoder", p.encoder},
		    {"track", p.track},
		    {"came_from", p.came_from},
		    {"twin", twin_of(places_, p)},
		    {"output", PREFIX + p.id},
		    {"state", "off"},
		    {"error", ""},
		    {"ready", not_ready(p)}};
	auto e = last_error_.find(p.id);
	if (e != last_error_.end())
		out["error"] = e->second;
	Runner *r = runner(p.id);
	if (r && r->output) {
		State s;
		uint64_t since;
		bool by_you;
		{
			std::lock_guard<std::mutex> l(r->lock);
			s = r->state;
			since = r->since_ns;
			by_you = r->by_you;
		}
		if (s == State::Stopped && obs_output_active(r->output))
			s = State::Stopping;
		if (s == State::Sending && obs_output_reconnecting(r->output))
			s = State::Reconnecting;
		out["state"] = state_name(s);
		out["by_you"] = by_you;
		out["sending_video"] = r->video;
		out["sending_kbps"] = r->kbps;
		out["bytes"] = obs_output_get_total_bytes(r->output);
		out["frames"] = obs_output_get_total_frames(r->output);
		out["dropped"] = obs_output_get_frames_dropped(r->output);
		out["congestion"] = obs_output_get_congestion(r->output);
		out["seconds"] = since ? (double)(os_gettime_ns() - since) / 1e9 : 0.0;
	}
	return out;
}

json Multistream::report()
{
	Main main;
	obs_video_info ovi = {};
	const bool video = obs_get_video_info(&ovi);
	json m = {{"live", main.live},
		  {"multitrack", main.multitrack},
		  {"video_kbps", bitrate_of(main.video)},
		  {"audio_kbps", bitrate_of(main.audio)},
		  {"audio_track", main.track()},
		  {"encoder", main.video ? str(obs_encoder_get_id(main.video)) : std::string()},
		  {"encoder_name", main.video ? str(obs_encoder_get_display_name(obs_encoder_get_id(main.video)))
					      : std::string()},
		  {"codec", main.video ? str(obs_encoder_get_codec(main.video)) : std::string()},
		  {"width", video ? ovi.output_width : 0},
		  {"height", video ? ovi.output_height : 0},
		  {"fps", video && ovi.fps_den ? (double)ovi.fps_num / ovi.fps_den : 0.0},
		  {"delay", profile_bool("Output", "DelayEnable", false) ? profile_int("Output", "DelaySec", 0) : 0}};
	json tracks = json::array();
	for (int t = 1; t <= 6; t++)
		tracks.push_back({{"track", t}, {"name", track_name(t)}});
	json places = json::array();
	for (const Place &p : places_.list)
		places.push_back(place_report(p));
	json sites_out = json::array();
	for (const Site &s : sites())
		sites_out.push_back({{"key", s.key}, {"label", s.label}, {"where", s.where}});
	return {{"ok", true},
		{"api", API},
		{"profile", profile_name()},
		{"file_problem", file_problem_},
		{"window_open", window_.is_open()},
		{"main", m},
		{"lighter",
		 {{"kbps", places_.lighter.kbps},
		  {"height", places_.lighter.height},
		  {"encoder", places_.lighter.encoder}}},
		{"tracks", tracks},
		{"encoders", h264_encoders()},
		{"sites", sites_out},
		{"places", places}};
}

namespace {

json fail(const std::string &words)
{
	return {{"ok", false}, {"error", words}};
}

std::string id_in(const json &data)
{
	auto it = data.find("id");
	return it != data.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool truthy(const json &data, const char *key)
{
	auto it = data.find(key);
	return it != data.end() && it->is_boolean() && it->get<bool>();
}

} // namespace

bool Multistream::request(const std::string &type, const json &data, json &answer)
{
	const auto &names = requests();
	if (std::find(names.begin(), names.end(), type) == names.end())
		return false;
	std::lock_guard<std::mutex> l(lock_);
	if (!loaded_)
		load();
	tidy_runners();
	if (type == "GetMultistream") {
		answer = report();
		return true;
	}
	if (closing_) {
		answer = fail("OBS is closing.");
		return true;
	}
	if (type == "MultistreamStart") {
		Place *p = places_.find(id_in(data));
		if (!p) {
			answer = fail("That place isn't in DireBot Multistream any more.");
			return true;
		}
		std::string why = start_place(*p, true);
		answer = why.empty() ? json{{"ok", true}, {"note", "Starting " + p->name}} : fail(why);
		return true;
	}
	if (type == "MultistreamStop") {
		Place *p = places_.find(id_in(data));
		if (!p) {
			answer = fail("That place isn't in DireBot Multistream any more.");
			return true;
		}
		const bool stopped = stop_place(p->id, truthy(data, "now"));
		answer = {{"ok", true}, {"stopped", stopped}};
		return true;
	}
	if (type == "MultistreamStopAll") {
		int n = 0;
		for (const Place &p : places_.list)
			n += stop_place(p.id, truthy(data, "now")) ? 1 : 0;
		answer = {{"ok", true}, {"stopped", n}};
		return true;
	}
	if (type == "MultistreamOpen") {
		const std::string id = id_in(data);
		const bool known = !id.empty() && places_.find(id);
		window_.open([id, known](obs_data_t *settings) {
			obs_data_set_string(settings, "focus", known ? id.c_str() : "");
		});
		answer = {{"ok", true}};
		return true;
	}
	if (type == "MultistreamAdd") {
		const std::string site_key = data.value("site", std::string("other"));
		const Site *site = find_site(site_key);
		if (!site) {
			answer = fail("DireBot Multistream doesn't know that site.");
			return true;
		}
		if (places_.list.size() >= MAX_PLACES) {
			answer = fail("That's as many places as DireBot Multistream takes.");
			return true;
		}
		Place p;
		p.id = places_.new_id([](size_t n) { return crypto::random_bytes(n); });
		p.site = site->key;
		std::string name = site->label;
		for (int n = 2; std::any_of(places_.list.begin(), places_.list.end(),
					    [&](const Place &q) { return q.name == name; });
		     n++)
			name = std::string(site->label) + " " + std::to_string(n);
		p.name = name;
		p.server = site->server;
		tidy(p);
		places_.list.push_back(p);
		pending_.insert(p.id);
		dirty_ = true;
		push_place(p);
		if (truthy(data, "open")) {
			const std::string id = p.id;
			window_.open([id](obs_data_t *settings) { obs_data_set_string(settings, "focus", id.c_str()); });
		}
		obs_log(LOG_INFO, "Multistream: DireBot added %s.", p.name.c_str());
		answer = {{"ok", true}, {"id", p.id}, {"name", p.name}};
		return true;
	}
	if (type == "MultistreamSet") {
		auto changes = data.find("changes");
		if (changes == data.end() || !changes->is_object()) {
			answer = fail("Nothing to change.");
			return true;
		}
		if (changes->contains("server") || changes->contains("key")) {
			answer = fail("A place's server and stream key are only changed in OBS: Tools → DireBot "
				      "Multistream.");
			return true;
		}
		const std::string id = id_in(data);
		if (id.empty() || id == "lighter") {
			Lighter &lt = places_.lighter;
			lt.kbps = changes->value("kbps", lt.kbps);
			lt.height = changes->value("height", lt.height);
			if (changes->contains("encoder") && (*changes)["encoder"].is_string())
				lt.encoder = (*changes)["encoder"].get<std::string>();
			tidy(lt);
			apply_lighter_bitrate();
			dirty_ = true;
			push_lighter();
			answer = {{"ok", true}};
			return true;
		}
		Place *p = places_.find(id);
		if (!p) {
			answer = fail("That place isn't in DireBot Multistream any more.");
			return true;
		}
		const bool was_on = p->on;
		try {
			if (changes->contains("name"))
				p->name = changes->at("name").get<std::string>();
			if (changes->contains("on"))
				p->on = changes->at("on").get<bool>();
			if (changes->contains("with_stream"))
				p->with_stream = changes->at("with_stream").get<bool>();
			if (changes->contains("stop_with_stream"))
				p->stop_with_stream = changes->at("stop_with_stream").get<bool>();
			if (changes->contains("video"))
				p->video = changes->at("video").get<std::string>();
			if (changes->contains("kbps"))
				p->kbps = changes->at("kbps").get<int>();
			if (changes->contains("height"))
				p->height = changes->at("height").get<int>();
			if (changes->contains("encoder"))
				p->encoder = changes->at("encoder").get<std::string>();
			if (changes->contains("track"))
				p->track = changes->at("track").get<int>();
		} catch (const std::exception &) {
			answer = fail("One of those changes isn't the right kind of value.");
			return true;
		}
		tidy(*p);
		if (was_on && !p->on)
			stop_place(p->id, false);
		dirty_ = true;
		push_place(*p);
		answer = {{"ok", true}, {"place", place_report(*p)}};
		return true;
	}
	if (type == "MultistreamImport") {
		// A list of objects, not of names: over OBS's own websocket an
		// answer goes through obs_data, which only keeps lists of objects.
		json added = json::array();
		std::string why = import_from(data.value("from", std::string()), added);
		answer = why.empty() ? json{{"ok", true}, {"added", added}} : fail(why);
		return true;
	}
	return false;
}

std::string Multistream::import_from(const std::string &from, json &added)
{
	std::vector<Place> found;
	bool there = false;
	if (from == "multi_rtmp") {
		const std::string dir = profile_dir();
		if (!dir.empty())
			found = from_multi_rtmp(read_json(dir + "/obs-multi-rtmp.json", there));
	} else if (from == "aitum") {
		char *ours = obs_module_config_path("");
		std::string folder = str(ours);
		bfree(ours);
		// .../plugin_config/direbot-for-obs/ -> .../plugin_config/
		while (!folder.empty() && (folder.back() == '/' || folder.back() == '\\'))
			folder.pop_back();
		const size_t cut = folder.find_last_of("/\\");
		if (cut != std::string::npos) {
			folder = folder.substr(0, cut);
			found = from_aitum(read_json(folder + "/aitum-multistream/config.json", there), profile_name());
		}
	} else {
		return "DireBot Multistream doesn't know that add-on.";
	}
	if (!there)
		return "That add-on has nothing saved for this OBS profile.";
	for (Place &p : found) {
		const bool twin = std::any_of(places_.list.begin(), places_.list.end(), [&](const Place &q) {
			return (!q.key.empty() && q.key == p.key && q.server == p.server) ||
			       (q.came_from == p.came_from && q.name == p.name);
		});
		if (twin || places_.list.size() >= MAX_PLACES) {
			obs::keystore::wipe(p.key);
			continue;
		}
		p.id = places_.new_id([](size_t n) { return crypto::random_bytes(n); });
		places_.list.push_back(p);
		pending_.insert(p.id);
		added.push_back({{"id", p.id}, {"name", p.name}});
		push_place(p);
		obs::keystore::wipe(p.key);
	}
	if (!added.empty()) {
		dirty_ = true;
		obs_log(LOG_INFO, "Multistream: brought %zu place(s) over from %s.", added.size(),
			from == "aitum" ? "Aitum Multistream" : "Multiple RTMP outputs");
	}
	return std::string();
}

// ---- the window in OBS -----------------------------------------------------------------

namespace {

// Writes a place's settings, without its key.
void write_place(obs_data_t *s, const Place &p)
{
	obs_data_set_string(s, key_of(p.id, "name").c_str(), p.name.c_str());
	obs_data_set_string(s, key_of(p.id, "site").c_str(), p.site.c_str());
	obs_data_set_string(s, key_of(p.id, "server").c_str(), p.server.c_str());
	obs_data_set_bool(s, key_of(p.id, "on").c_str(), p.on);
	obs_data_set_bool(s, key_of(p.id, "start").c_str(), p.with_stream);
	obs_data_set_bool(s, key_of(p.id, "stop").c_str(), p.stop_with_stream);
	obs_data_set_string(s, key_of(p.id, "video").c_str(), p.video.c_str());
	obs_data_set_int(s, key_of(p.id, "kbps").c_str(), p.kbps ? p.kbps : 4500);
	obs_data_set_int(s, key_of(p.id, "height").c_str(), p.height);
	obs_data_set_string(s, key_of(p.id, "encoder").c_str(), p.encoder.c_str());
	obs_data_set_int(s, key_of(p.id, "track").c_str(), p.track);
}

void erase_place(obs_data_t *s, const std::string &id)
{
	for (const char *what : {"name", "site", "server", "key", "nokey", "on", "start", "stop", "video", "kbps",
				 "height", "encoder", "track"})
		obs_data_erase(s, key_of(id, what).c_str());
}

void add_id(obs_data_t *s, const std::string &id)
{
	std::vector<std::string> ids = split_ids(obs_data_get_string(s, "ids"));
	if (std::find(ids.begin(), ids.end(), id) == ids.end())
		ids.push_back(id);
	std::string joined;
	for (const std::string &i : ids)
		joined += (joined.empty() ? "" : ",") + i;
	obs_data_set_string(s, "ids", joined.c_str());
}

// What a button in the window needs: this part, and which place.
struct Button {
	Multistream *self;
	std::string id;
	obs::SettingsWindow *window;
};

struct Buttons {
	std::vector<std::unique_ptr<Button>> list;
	Button *add(Multistream *self, const std::string &id, obs::SettingsWindow *window)
	{
		list.push_back(std::make_unique<Button>(Button{self, id, window}));
		return list.back().get();
	}
};

void free_buttons(void *param)
{
	delete static_cast<Buttons *>(param);
}

bool redraw(obs_properties_t *, obs_property_t *, obs_data_t *)
{
	return true;
}

// Picking a site fills in its server, when the box is empty or still has
// another site's.
bool site_picked(void *priv, obs_properties_t *, obs_property_t *, obs_data_t *settings)
{
	auto *b = static_cast<Button *>(priv);
	const std::string server_key = key_of(b->id, "server");
	const std::string server = obs_data_get_string(settings, server_key.c_str());
	const Site *site = find_site(obs_data_get_string(settings, key_of(b->id, "site").c_str()));
	if (!site)
		return false;
	bool stock = server.empty();
	for (const Site &s : sites())
		stock = stock || (*s.server && server == s.server);
	if (stock && server != site->server) {
		obs_data_set_string(settings, server_key.c_str(), site->server);
		return true;
	}
	return false;
}

} // namespace

void Multistream::defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "lighter_kbps", 3500);
	obs_data_set_default_int(settings, "lighter_height", 720);
}

obs_properties_t *Multistream::properties(obs_data_t *settings)
{
	obs_properties_t *props = obs_properties_create();
	auto *buttons = new Buttons();
	obs_properties_set_param(props, buttons, free_buttons);
	const std::string focus = obs_data_get_string(settings, "focus");
	const std::vector<std::string> ids = split_ids(obs_data_get_string(settings, "ids"));
	const bool focused = !focus.empty() && std::find(ids.begin(), ids.end(), focus) != ids.end();

	obs_property_t *intro = obs_properties_add_text(
		props, "intro",
		focused ? "Type this place's stream key below, then press OK. It stays here in OBS: DireBot never "
			  "sees it."
			: "Your stream also goes to each place ticked here, as well as where OBS streams (Settings → "
			  "Stream). Stream keys stay here in OBS: DireBot never sees them.",
		OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(intro, true);

	obs_data_t *main_settings = nullptr;
	{
		Main main;
		if (main.video)
			main_settings = obs_encoder_get_settings(main.video);
	}
	const int stream_kbps = main_settings ? (int)obs_data_get_int(main_settings, "bitrate") : 0;
	obs_data_release(main_settings);

	for (const std::string &id : ids) {
		if (focused && id != focus)
			continue;
		Button *b = buttons->add(this, id, &window_);
		obs_properties_t *g = obs_properties_create();
		const std::string name = obs_data_get_string(settings, key_of(id, "name").c_str());
		const std::string site_key = obs_data_get_string(settings, key_of(id, "site").c_str());
		const Site *site = find_site(site_key);

		obs_properties_add_text(g, key_of(id, "name").c_str(), "Name", OBS_TEXT_DEFAULT);
		obs_property_t *list = obs_properties_add_list(g, key_of(id, "site").c_str(), "Site", OBS_COMBO_TYPE_LIST,
							       OBS_COMBO_FORMAT_STRING);
		for (const Site &s : sites())
			obs_property_list_add_string(list, s.label, s.key);
		obs_property_set_modified_callback2(list, site_picked, b);
		obs_properties_add_text(g, key_of(id, "server").c_str(), "Server", OBS_TEXT_DEFAULT);
		// A saved key is never shown, or put back in the box: the box is
		// for a new one.
		obs_properties_add_text(g, key_of(id, "key").c_str(), "Stream key", OBS_TEXT_PASSWORD);
		{
			bool saved = false, lost = false;
			{
				std::lock_guard<std::mutex> l(lock_);
				auto snap = snapshot_.find(id);
				const Place *p = places_.find(id);
				if (snap != snapshot_.end()) {
					saved = !snap->second.first.empty();
					lost = !saved && !snap->second.second.empty();
				} else if (p) {
					saved = p->key_set();
					lost = p->key_lost();
				}
			}
			obs_property_t *note = obs_properties_add_text(
				g, key_of(id, "keynote").c_str(),
				saved  ? "A stream key is saved. Type a new one only to change it."
				: lost ? "Its key was typed in on another PC or Windows account: type it in again."
				       : "No stream key yet: paste it in above.",
				OBS_TEXT_INFO);
			obs_property_text_set_info_word_wrap(note, true);
			if (!saved)
				obs_property_text_set_info_type(note, OBS_TEXT_INFO_WARNING);
			if (saved)
				obs_properties_add_bool(g, key_of(id, "nokey").c_str(), "Remove the saved key");
		}
		if (site && *site->where) {
			obs_property_t *where =
				obs_properties_add_text(g, key_of(id, "where").c_str(), site->where, OBS_TEXT_INFO);
			obs_property_text_set_info_word_wrap(where, true);
		}
		obs_properties_add_bool(g, key_of(id, "start").c_str(), "Start when I go live");
		obs_properties_add_bool(g, key_of(id, "stop").c_str(), "End when I end my stream");

		obs_property_t *video = obs_properties_add_list(g, key_of(id, "video").c_str(), "Picture",
								OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		std::string same = "Same as my stream";
		if (stream_kbps > 0)
			same += " (" + std::to_string(stream_kbps) + " Kbps)";
		obs_property_list_add_string(video, same.c_str(), SAME);
		obs_property_list_add_string(video, "Lighter (shared by every Lighter place)", LIGHTER);
		obs_property_list_add_string(video, "Its own", OWN);
		obs_property_set_modified_callback(video, redraw);
		const bool own = std::string(obs_data_get_string(settings, key_of(id, "video").c_str())) == OWN;
		if (own) {
			obs_property_t *kbps = obs_properties_add_int(g, key_of(id, "kbps").c_str(), "Its bitrate",
								      300, 50000, 100);
			obs_property_int_set_suffix(kbps, " Kbps");
			obs_property_t *size = obs_properties_add_list(g, key_of(id, "height").c_str(), "Its size",
								       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
			for (int h : heights())
				obs_property_list_add_int(size, h ? (std::to_string(h) + "p").c_str() : "Same as my stream",
							  h);
			obs_property_t *enc = obs_properties_add_list(g, key_of(id, "encoder").c_str(), "Its encoder",
								      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
			obs_property_list_add_string(enc, "The same kind as my stream's", "");
			for (const json &e : h264_encoders())
				obs_property_list_add_string(enc, e["name"].get<std::string>().c_str(),
							     e["id"].get<std::string>().c_str());
		}
		obs_property_t *sound = obs_properties_add_list(g, key_of(id, "track").c_str(), "Sound",
								OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(sound, "Same as my stream", 0);
		for (int t = 1; t <= 6; t++) {
			std::string label = "Audio track " + std::to_string(t);
			const std::string named = track_name(t);
			if (!named.empty())
				label += " (" + named + ")";
			obs_property_list_add_int(sound, label.c_str(), t);
		}
		obs_properties_add_button2(
			g, key_of(id, "remove").c_str(), "Remove this place",
			[](obs_properties_t *, obs_property_t *, void *priv) {
				auto *b = static_cast<Button *>(priv);
				const std::string gone = b->id;
				b->window->change([gone](obs_data_t *s) {
					std::vector<std::string> ids = split_ids(obs_data_get_string(s, "ids"));
					ids.erase(std::remove(ids.begin(), ids.end(), gone), ids.end());
					std::string joined;
					for (const std::string &i : ids)
						joined += (joined.empty() ? "" : ",") + i;
					obs_data_set_string(s, "ids", joined.c_str());
					erase_place(s, gone);
					if (std::string(obs_data_get_string(s, "focus")) == gone)
						obs_data_set_string(s, "focus", "");
				});
				return false;
			},
			b);
		const std::string title = (name.empty() ? std::string("A place") : name) +
					  (site && name != site->label ? " - " + std::string(site->label) : "");
		obs_properties_add_group(props, key_of(id, "on").c_str(), title.c_str(), OBS_GROUP_CHECKABLE, g);
	}

	if (focused) {
		Button *b = buttons->add(this, "", &window_);
		obs_properties_add_button2(
			props, "show_all", "Show every place",
			[](obs_properties_t *, obs_property_t *, void *priv) {
				static_cast<Button *>(priv)->window->change(
					[](obs_data_t *s) { obs_data_set_string(s, "focus", ""); });
				return false;
			},
			b);
		return props;
	}

	if (ids.size() < MAX_PLACES) {
		Button *b = buttons->add(this, "", &window_);
		obs_properties_add_button2(
			props, "add", "Add a place",
			[](obs_properties_t *, obs_property_t *, void *priv) {
				auto *b = static_cast<Button *>(priv);
				Place p;
				p.id = b->self->new_place_id();
				p.name = "YouTube";
				p.site = "youtube";
				p.server = find_site("youtube")->server;
				b->window->change([p](obs_data_t *s) {
					write_place(s, p);
					add_id(s, p.id);
				});
				return false;
			},
			b);
	}

	obs_properties_t *lighter = obs_properties_create();
	obs_property_t *lk = obs_properties_add_int(lighter, "lighter_kbps", "Bitrate", 300, 50000, 100);
	obs_property_int_set_suffix(lk, " Kbps");
	obs_property_t *lh = obs_properties_add_list(lighter, "lighter_height", "Size", OBS_COMBO_TYPE_LIST,
						     OBS_COMBO_FORMAT_INT);
	for (int h : heights())
		obs_property_list_add_int(lh, h ? (std::to_string(h) + "p").c_str() : "Same as my stream", h);
	obs_property_t *le = obs_properties_add_list(lighter, "lighter_encoder", "Encoder", OBS_COMBO_TYPE_LIST,
						     OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(le, "The same kind as my stream's", "");
	for (const json &e : h264_encoders())
		obs_property_list_add_string(le, e["name"].get<std::string>().c_str(),
					     e["id"].get<std::string>().c_str());
	obs_property_t *li = obs_properties_add_text(
		lighter, "lighter_info",
		"One smaller picture every place set to Lighter shares: less upload for each, and one extra encode "
		"however many use it. Its bitrate changes straight away; its size and encoder the next time it "
		"starts.",
		OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(li, true);
	obs_properties_add_group(props, "lighter", "Lighter picture", OBS_GROUP_NORMAL, lighter);
	return props;
}

void Multistream::changed(obs_data_t *settings)
{
	std::lock_guard<std::mutex> l(lock_);
	if (closing_)
		return;
	// OBS's Edit → Undo / Redo of the window's OK puts back all of its
	// settings from then, keys and all, with the window shut: the places
	// stay as they are, and the window's settings go back to matching
	// them, without keys.
	if (obs_data_has_user_value(settings, "undo_uuid")) {
		obs_data_erase(settings, "undo_uuid");
		if (!window_open_) {
			obs_log(LOG_INFO, "Multistream: OBS's Undo / Redo doesn't change places; the window's settings "
					  "were put back.");
			for (const std::string &id : split_ids(obs_data_get_string(settings, "ids"))) {
				obs_data_erase(settings, key_of(id, "key").c_str());
				obs_data_erase(settings, key_of(id, "nokey").c_str());
			}
			push_all();
			return;
		}
	}
	if (obs_data_get_int(settings, "generation") != generation_)
		return;
	const std::vector<std::string> ids = split_ids(obs_data_get_string(settings, "ids"));
	std::vector<Place> list;
	for (const std::string &id : ids) {
		pending_.erase(id);
		const Place *was = places_.find(id);
		Place p = was ? *was : Place();
		p.id = id;
		p.name = obs_data_get_string(settings, key_of(id, "name").c_str());
		p.site = obs_data_get_string(settings, key_of(id, "site").c_str());
		p.server = obs_data_get_string(settings, key_of(id, "server").c_str());
		p.on = obs_data_get_bool(settings, key_of(id, "on").c_str());
		p.with_stream = obs_data_get_bool(settings, key_of(id, "start").c_str());
		p.stop_with_stream = obs_data_get_bool(settings, key_of(id, "stop").c_str());
		p.video = obs_data_get_string(settings, key_of(id, "video").c_str());
		p.kbps = (int)obs_data_get_int(settings, key_of(id, "kbps").c_str());
		p.height = (int)obs_data_get_int(settings, key_of(id, "height").c_str());
		p.encoder = obs_data_get_string(settings, key_of(id, "encoder").c_str());
		p.track = (int)obs_data_get_int(settings, key_of(id, "track").c_str());
		// The key: one typed in the window; none, with "Remove the
		// saved key" ticked; otherwise the one from when the window
		// opened (Cancel, or a box emptied again) - or, with the window
		// shut, the one it has.
		std::string typed = obs_data_get_string(settings, key_of(id, "key").c_str());
		auto snap = snapshot_.find(id);
		if (typed.find_first_not_of(" \t\r\n") != std::string::npos) {
			if (typed != p.key) {
				obs::keystore::wipe(p.key);
				p.key = typed;
				p.locked_key.clear();
			}
		} else if (obs_data_get_bool(settings, key_of(id, "nokey").c_str())) {
			obs::keystore::wipe(p.key);
			p.locked_key.clear();
		} else if (snap != snapshot_.end()) {
			if (p.key != snap->second.first) {
				obs::keystore::wipe(p.key);
				p.key = snap->second.first;
			}
			p.locked_key = snap->second.second;
		}
		obs::keystore::wipe(typed);
		tidy(p);
		list.push_back(std::move(p));
	}
	// Places DireBot added that the window hasn't heard of yet stay.
	for (const Place &p : places_.list)
		if (pending_.count(p.id) && std::none_of(list.begin(), list.end(), [&](const Place &q) {
			    return q.id == p.id;
		    }))
			list.push_back(p);
	// Gone, or switched off: stopped.
	for (const Place &old : places_.list) {
		auto now = std::find_if(list.begin(), list.end(), [&](const Place &q) { return q.id == old.id; });
		if (now == list.end() || (old.on && !now->on))
			stop_place(old.id, false);
		if (now == list.end())
			last_error_.erase(old.id);
	}
	Lighter lt;
	lt.kbps = (int)obs_data_get_int(settings, "lighter_kbps");
	lt.height = (int)obs_data_get_int(settings, "lighter_height");
	lt.encoder = obs_data_get_string(settings, "lighter_encoder");
	tidy(lt);
	// Saved only when something did change (this also hears the window
	// being told what was loaded).
	if (list != places_.list || lt != places_.lighter)
		dirty_ = true;
	for (Place &old : places_.list)
		obs::keystore::wipe(old.key);
	places_.list = std::move(list);
	const bool rate = lt.kbps != places_.lighter.kbps;
	places_.lighter = lt;
	if (rate)
		apply_lighter_bitrate();
}

bool Multistream::opened(obs_data_t *)
{
	std::lock_guard<std::mutex> l(lock_);
	window_open_ = true;
	snapshot_.clear();
	for (const Place &p : places_.list)
		snapshot_[p.id] = {p.key, p.locked_key};
	return false;
}

void Multistream::closed(obs_data_t *settings)
{
	std::lock_guard<std::mutex> l(lock_);
	window_open_ = false;
	for (auto &s : snapshot_) {
		obs::keystore::wipe(s.second.first);
		obs::keystore::wipe(s.second.second);
	}
	snapshot_.clear();
	// What was typed goes from the window's settings: the next time it
	// opens, the boxes are empty again.
	for (const std::string &id : split_ids(obs_data_get_string(settings, "ids"))) {
		std::string typed = obs_data_get_string(settings, key_of(id, "key").c_str());
		obs::keystore::wipe(typed);
		obs_data_erase(settings, key_of(id, "key").c_str());
		obs_data_erase(settings, key_of(id, "nokey").c_str());
	}
	obs_data_set_string(settings, "focus", "");
	save();
}

// The model into the window's settings (never a key).
void Multistream::push_place(const Place &p)
{
	Place copy = p;
	obs::keystore::wipe(copy.key);
	window_.change([copy](obs_data_t *s) {
		write_place(s, copy);
		add_id(s, copy.id);
	});
}

void Multistream::push_lighter()
{
	const Lighter lt = places_.lighter;
	window_.change([lt](obs_data_t *s) {
		obs_data_set_int(s, "lighter_kbps", lt.kbps);
		obs_data_set_int(s, "lighter_height", lt.height);
		obs_data_set_string(s, "lighter_encoder", lt.encoder.c_str());
	});
}

void Multistream::push_all()
{
	std::vector<Place> copies = places_.list;
	for (Place &p : copies)
		obs::keystore::wipe(p.key);
	const Lighter lt = places_.lighter;
	const std::string ids = join_ids(copies);
	const long long generation = generation_;
	for (const Place &p : copies)
		pending_.insert(p.id);
	window_.change([copies, lt, ids, generation](obs_data_t *s) {
		obs_data_set_int(s, "generation", generation);
		for (const std::string &old : split_ids(obs_data_get_string(s, "ids")))
			erase_place(s, old);
		for (const Place &p : copies)
			write_place(s, p);
		obs_data_set_string(s, "ids", ids.c_str());
		obs_data_set_int(s, "lighter_kbps", lt.kbps);
		obs_data_set_int(s, "lighter_height", lt.height);
		obs_data_set_string(s, "lighter_encoder", lt.encoder.c_str());
	});
}

} // namespace direbot::multistream
