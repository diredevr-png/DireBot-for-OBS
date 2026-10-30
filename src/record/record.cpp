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

#include "record.hpp"

#include <algorithm>
#include <cstring>

#include <obs-frontend-api.h>
#include <obs.hpp>
#include <util/config-file.h>
#include <util/platform.h>

#include <plugin-support.h>

#include "../obs/recording.hpp"
#include "../obs/ui-call.hpp"
#include "../vertical/rules.hpp"

namespace direbot::record {

namespace {

constexpr const char *OPTIONS_FILE = "direbot-record.json";
constexpr uint64_t MS = 1'000'000;

const std::vector<std::string> REQUESTS = {"RecordGetState", "RecordSetOptions", "RecordStart", "RecordStop"};

json fail(const std::string &words)
{
	return {{"ok", false}, {"error", words}};
}

std::string profile_file()
{
	char *dir = obs_frontend_get_current_profile_path();
	if (!dir)
		return "";
	std::string path = std::string(dir) + "/" + OPTIONS_FILE;
	bfree(dir);
	return path;
}

std::vector<std::string> video_encoder_ids()
{
	std::vector<std::string> ids;
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_encoder_types(i, &id); i++) {
		const char *codec = obs_get_encoder_codec(id);
		if (obs_get_encoder_type(id) == OBS_ENCODER_VIDEO && codec && strcmp(codec, "h264") == 0 &&
		    !(obs_get_encoder_caps(id) & OBS_ENCODER_CAP_INTERNAL))
			ids.push_back(id);
	}
	return ids;
}

// The source a pick means: by uuid first (it survives a rename), then by
// name. A new reference, or null.
obs_source_t *find_source(const Pick &pick)
{
	obs_source_t *s = pick.uuid.empty() ? nullptr : obs_get_source_by_uuid(pick.uuid.c_str());
	if (!s)
		s = obs_get_source_by_name(pick.source.c_str());
	return s;
}

// Sources with a picture of their own, for the list DireBot shows: inputs,
// not scenes or filters.
json video_sources()
{
	json rows = json::array();
	obs_enum_sources(
		[](void *param, obs_source_t *s) {
			auto *list = static_cast<json *>(param);
			if (!(obs_source_get_output_flags(s) & OBS_SOURCE_VIDEO))
				return true;
			const char *name = obs_source_get_name(s);
			list->push_back({{"name", name ? name : ""},
					 {"uuid", obs_source_get_uuid(s)},
					 {"width", obs_source_get_width(s)},
					 {"height", obs_source_get_height(s)}});
			return true;
		},
		&rows);
	return rows;
}

std::string stop_words(int code)
{
	switch (code) {
	case OBS_OUTPUT_SUCCESS:
		return "";
	case OBS_OUTPUT_BAD_PATH:
		return "OBS couldn't write to the recording folder. Check it in OBS → Settings → Output.";
	case OBS_OUTPUT_NO_SPACE:
		return "The drive is full.";
	case OBS_OUTPUT_UNSUPPORTED:
	case OBS_OUTPUT_ENCODE_ERROR:
		return "The encoder stopped working. Pick another one, or update your graphics driver.";
	default:
		return "A clean recording stopped because of an error in OBS.";
	}
}

} // namespace

CleanRecordings::~CleanRecordings()
{
	unload();
}

const std::vector<std::string> &CleanRecordings::requests()
{
	return REQUESTS;
}

bool CleanRecordings::handles(const std::string &type)
{
	return std::find(REQUESTS.begin(), REQUESTS.end(), type) != REQUESTS.end();
}

void CleanRecordings::start(Emit emit)
{
	if (started_)
		return;
	emit_ = std::move(emit);
	obs_frontend_add_event_callback(&CleanRecordings::on_frontend, this);
	frontend_ = true;
	started_ = true;
}

void CleanRecordings::unload()
{
	*alive_ = false;
	if (!started_)
		return;
	started_ = false;
	closing_ = true;
	for (Running &r : running_) {
		if (r.output && obs_output_active(r.output))
			obs_output_force_stop(r.output);
		release(r);
	}
	running_.clear();
	// Usually already done at OBS_FRONTEND_EVENT_EXIT: by unload OBS's
	// window has let go of its list.
	if (frontend_)
		obs_frontend_remove_event_callback(&CleanRecordings::on_frontend, this);
	frontend_ = false;
}

json CleanRecordings::request(const std::string &type, const json &data)
{
	if (!started_ || closing_)
		return fail("OBS is closing.");
	return obs::ui_call(alive_, [this, type, data] { return do_request(type, data); });
}

void CleanRecordings::tick()
{
	if (!started_ || closing_ || running_.empty() || tick_queued_.exchange(true))
		return;
	obs::later(alive_, [this] {
		tick_queued_ = false;
		if (closing_)
			return;
		const size_t before = running_.size();
		tidy();
		if (running_.size() != before || !running_.empty())
			changed();
	});
}

json CleanRecordings::do_request(const std::string &type, const json &data)
{
	if (closing_)
		return fail("OBS is closing.");
	if (type == "RecordGetState")
		return state();
	if (type == "RecordSetOptions") {
		if (!data.is_object() || !data.contains("options"))
			return fail("Nothing to save.");
		options_ = Options::from_json(data["options"]);
		save_options();
		changed();
		return state();
	}
	if (type == "RecordStart") {
		const std::string problem = start_all();
		json answer = state();
		if (!problem.empty() && running_.empty())
			return fail(problem);
		if (!problem.empty())
			answer["note"] = problem;
		return answer;
	}
	if (type == "RecordStop") {
		stop_all();
		return state();
	}
	return fail("DireBot for OBS doesn't know that request.");
}

json CleanRecordings::state() const
{
	json rows = json::array();
	const uint64_t now = os_gettime_ns();
	for (const Running &r : running_)
		rows.push_back({{"source", r.pick.source},
				{"file", obs::file_name(r.file)},
				{"recording", r.output && obs_output_active(r.output)},
				{"seconds", r.started_ns ? (now - r.started_ns) / 1'000'000'000ULL : 0}});
	json encoders = json::array();
	for (const std::string &id : video_encoder_ids()) {
		const char *name = obs_encoder_get_display_name(id.c_str());
		encoders.push_back({{"id", id}, {"name", name ? name : id}});
	}
	return {{"ok", true},
		{"options", options_.to_json()},
		{"running", rows},
		{"problems", problems_},
		{"sources", video_sources()},
		{"encoders", encoders},
		{"auto_encoder", pick_encoder("auto", video_encoder_ids())}};
}

void CleanRecordings::load_options()
{
	options_ = Options();
	const std::string path = profile_file();
	char *text = path.empty() ? nullptr : os_quick_read_utf8_file(path.c_str());
	if (text) {
		json j = json::parse(text, nullptr, false);
		if (!j.is_discarded())
			options_ = Options::from_json(j);
		bfree(text);
	}
}

void CleanRecordings::save_options()
{
	const std::string path = profile_file();
	if (path.empty())
		return;
	const std::string body = options_.to_json().dump(1, '\t', false, json::error_handler_t::replace);
	if (!os_quick_write_utf8_file_safe(path.c_str(), body.c_str(), body.size(), false, "tmp", "bak"))
		obs_log(LOG_WARNING, "Clean recordings: couldn't save %s.", OPTIONS_FILE);
}

std::string CleanRecordings::start_all()
{
	tidy();
	problems_.clear();
	std::string problems;
	int started = 0;
	for (const Pick &pick : options_.picks) {
		if (!pick.enabled)
			continue;
		const bool already = std::any_of(running_.begin(), running_.end(),
						 [&](const Running &r) { return r.pick.source == pick.source; });
		if (already)
			continue;
		const std::string why = start_one(pick);
		if (why.empty()) {
			started++;
		} else {
			problems_.push_back(pick.source + ": " + why);
			problems += (problems.empty() ? "" : " ") + pick.source + ": " + why;
		}
	}
	if (!started && problems.empty() && running_.empty())
		problems = "No sources are picked for clean recordings yet.";
	changed();
	return problems;
}

std::string CleanRecordings::start_one(const Pick &pick)
{
	OBSSourceAutoRelease source = find_source(pick);
	if (!source)
		return "it isn't in OBS any more.";
	const int sw = (int)obs_source_get_width(source), sh = (int)obs_source_get_height(source);
	if (sw < 2 || sh < 2)
		return "it isn't showing a picture right now.";

	Running r;
	r.pick = pick;
	obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi))
		return "OBS's video isn't ready.";
	int ow = 0, oh = 0;
	fit(sw, sh, pick.height, ow, oh);
	ovi.base_width = (uint32_t)(sw & ~1);
	ovi.base_height = (uint32_t)(sh & ~1);
	ovi.output_width = (uint32_t)ow;
	ovi.output_height = (uint32_t)oh;
	const std::string name = "DireBot clean " + pick.source;
	r.canvas = obs_canvas_create_private(name.c_str(), &ovi, DEVICE);
	if (!r.canvas || !obs_canvas_has_video(r.canvas)) {
		release(r);
		return "OBS couldn't make a picture for it.";
	}
	obs_canvas_set_channel(r.canvas, 0, source);

	config_t *config = obs_frontend_get_profile_config();
	const std::string encoder = pick_encoder(options_.encoder, video_encoder_ids());
	OBSDataAutoRelease vs = obs_data_create();
	const double fps = ovi.fps_den ? (double)ovi.fps_num / ovi.fps_den : 30.0;
	obs::set_quality(vs, encoder, kbps_for(pick.quality, ow, oh, fps), config);
	r.video = obs_video_encoder_create(encoder.c_str(), (name + " video").c_str(), vs, nullptr);
	if (!r.video) {
		release(r);
		return "OBS couldn't make an encoder for it.";
	}
	obs_encoder_set_video(r.video, obs_canvas_get_video(r.canvas));

	if (pick.audio_track > 0) {
		OBSDataAutoRelease as = obs_data_create();
		obs_data_set_int(as, "bitrate", obs::audio_kbps(config, pick.audio_track));
		r.audio = obs_audio_encoder_create(obs::audio_encoder_id(), (name + " sound").c_str(), as,
						   (size_t)(pick.audio_track - 1), nullptr);
		if (!r.audio) {
			release(r);
			return "OBS couldn't make a sound encoder for it.";
		}
		obs_encoder_set_audio(r.audio, obs_get_audio());
	}

	const obs::RecordPlace place = obs::record_place(config);
	std::string id = vertical::rules::recording_output(place.format);
	const std::string ext = vertical::rules::recording_extension(place.format);
	if (id != "ffmpeg_muxer" && !obs_get_output_flags(id.c_str()))
		id = "ffmpeg_muxer";
	std::string problem;
	r.file = obs::record_path(place, ext, file_suffix(pick.source), problem);
	if (r.file.empty()) {
		release(r);
		return problem;
	}
	OBSDataAutoRelease os = obs_data_create();
	obs_data_set_string(os, "path", r.file.c_str());
	std::string muxer = vertical::rules::recording_muxer_settings(place.format);
	if (muxer.empty() && id == "ffmpeg_muxer" && ext != "mkv" && place.format.rfind("hybrid", 0) == 0)
		muxer = "movflags=frag_keyframe+empty_moov+delay_moov";
	obs_data_set_string(os, "muxer_settings", muxer.c_str());
	r.output = obs_output_create(id.c_str(), (name + " file").c_str(), os, nullptr);
	if (!r.output) {
		release(r);
		return "OBS couldn't set up the recording.";
	}
	obs_output_set_video_encoder(r.output, r.video);
	if (r.audio)
		obs_output_set_audio_encoder(r.output, r.audio, 0);
	signal_handler_connect(obs_output_get_signal_handler(r.output), "stop", &CleanRecordings::on_stop, this);
	r.started_ns = os_gettime_ns();
	if (!obs_output_start(r.output)) {
		const char *error = obs_output_get_last_error(r.output);
		std::string why = error && *error ? error : "OBS couldn't start the recording.";
		release(r);
		return why;
	}
	obs_log(LOG_INFO, "Clean recordings: recording %s with %s.", obs::file_name(r.file).c_str(),
		encoder.c_str());
	running_.push_back(r);
	return "";
}

void CleanRecordings::stop_all()
{
	for (Running &r : running_)
		if (r.output && obs_output_active(r.output))
			obs_output_stop(r.output);
	changed();
}

void CleanRecordings::release(Running &r)
{
	if (r.output) {
		signal_handler_disconnect(obs_output_get_signal_handler(r.output), "stop", &CleanRecordings::on_stop, this);
		obs_output_release(r.output);
		r.output = nullptr;
	}
	if (r.video) {
		obs_encoder_release(r.video);
		r.video = nullptr;
	}
	if (r.audio) {
		obs_encoder_release(r.audio);
		r.audio = nullptr;
	}
	if (r.canvas) {
		obs_canvas_set_channel(r.canvas, 0, nullptr);
		obs_canvas_remove(r.canvas);
		obs_canvas_release(r.canvas);
		r.canvas = nullptr;
	}
}

// Lets go of the ones that have finished.
void CleanRecordings::tidy()
{
	for (auto it = running_.begin(); it != running_.end();) {
		if (it->output && obs_output_active(it->output)) {
			++it;
			continue;
		}
		release(*it);
		it = running_.erase(it);
	}
}

void CleanRecordings::changed()
{
	if (emit_ && !closing_)
		emit_("RecordChanged", state());
}

// From the output's own thread: noted on the UI thread.
void CleanRecordings::on_stop(void *param, calldata_t *cd)
{
	auto *self = static_cast<CleanRecordings *>(param);
	const int code = (int)calldata_int(cd, "code");
	obs::later(self->alive_, [self, code] {
		if (code != OBS_OUTPUT_SUCCESS)
			self->problems_.push_back(stop_words(code));
		self->tidy();
		self->changed();
	});
}

void CleanRecordings::on_frontend(enum obs_frontend_event event, void *self)
{
	auto *c = static_cast<CleanRecordings *>(self);
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		c->loaded_ = true;
		c->load_options();
		break;
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
		c->stop_all();
		c->load_options();
		c->changed();
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		if (c->options_.when == When::Recording)
			c->start_all();
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPING:
		if (c->options_.when == When::Recording)
			c->stop_all();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		if (c->options_.when == When::Stream)
			c->start_all();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
		if (c->options_.when == When::Stream)
			c->stop_all();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
		c->stop_all();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		c->stop_all();
		// Now, while OBS's window still has its list.
		obs_frontend_remove_event_callback(&CleanRecordings::on_frontend, c);
		c->frontend_ = false;
		break;
	default:
		break;
	}
}

} // namespace direbot::record
