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

#include "vertical.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>

#include <obs-module.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <plugin-support.h>

#include "rules.hpp"

namespace direbot::vertical {

namespace {

constexpr const char *SAVE_KEY = "direbot_vertical";
constexpr const char *OPTIONS_FILE = "direbot-vertical.json";
constexpr uint64_t MS = 1000000ULL;
// A request waits this long for OBS's window to get to it.
constexpr int REQUEST_WAIT_MS = 5000;
// OBS closing or changing scene collection: time for a recording to
// finish its file.
constexpr uint32_t CLOSE_WAIT_MS = 5000;
constexpr uint64_t STATUS_EVERY_NS = 2000 * MS;
constexpr int MAX_LOOK_SECONDS = 120;

const std::vector<std::string> REQUESTS = {
	"VerticalGetState",    "VerticalSetUp",       "VerticalSetSize",      "VerticalSetScene",
	"VerticalCreateScene", "VerticalRemoveScene", "VerticalRenameScene",  "VerticalSetFollow",
	"VerticalSetOptions",  "VerticalStartStream", "VerticalStopStream",   "VerticalStartRecord",
	"VerticalStopRecord",  "VerticalPauseRecord", "VerticalOpenSettings", "VerticalLook",
	"VerticalGetEncoders",
};

json fail(const std::string &words)
{
	return {{"ok", false}, {"error", words}};
}

std::string text(const json &j, const char *key)
{
	auto it = j.find(key);
	return it != j.end() && it->is_string() ? it->get<std::string>() : "";
}

bool has(const json &j, const char *key)
{
	return j.find(key) != j.end();
}

int number(const json &j, const char *key, int fallback)
{
	auto it = j.find(key);
	if (it == j.end() || !it->is_number())
		return fallback;
	const double v = it->get<double>();
	return v == v && v > -1e9 && v < 1e9 ? (int)v : fallback;
}

bool flag(const json &j, const char *key, bool fallback)
{
	auto it = j.find(key);
	return it != j.end() && it->is_boolean() ? it->get<bool>() : fallback;
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

std::string main_scene_name()
{
	obs_source_t *scene = obs_frontend_get_current_scene();
	std::string name = scene ? obs_source_get_name(scene) : "";
	obs_source_release(scene);
	return name;
}

// A request done on OBS's UI thread, for a thread that waits on it.
struct Job {
	std::mutex lock;
	std::condition_variable done_cv;
	bool done = false;
	json result;
	std::function<json()> work;
};

void run_job(void *param)
{
	std::unique_ptr<std::shared_ptr<Job>> holder(static_cast<std::shared_ptr<Job> *>(param));
	std::shared_ptr<Job> job = *holder;
	json result;
	try {
		result = job->work();
	} catch (const std::exception &) {
		result = fail("DireBot for OBS couldn't do that.");
	}
	{
		std::lock_guard<std::mutex> l(job->lock);
		job->result = std::move(result);
		job->done = true;
	}
	job->done_cv.notify_all();
}

// Later, on the UI thread, if it's still there. Asked from the UI thread
// itself, OBS runs a UI task straight away, inside the caller: an output
// stopped from the UI thread would then be let go inside its own "stop"
// signal, before OBS has finished stopping it, and OBS would wait on
// itself for good. So every task goes by way of the graphics thread
// first: always later, and always in order.
void later(const std::shared_ptr<std::atomic<bool>> &alive, std::function<void()> work)
{
	struct Task {
		std::shared_ptr<std::atomic<bool>> alive;
		std::function<void()> work;
	};
	obs_queue_task(
		OBS_TASK_GRAPHICS,
		[](void *param) {
			obs_queue_task(
				OBS_TASK_UI,
				[](void *param) {
					std::unique_ptr<Task> task(static_cast<Task *>(param));
					if (*task->alive)
						task->work();
				},
				param, false);
		},
		new Task{alive, std::move(work)}, false);
}

} // namespace

Vertical::Vertical()
	: page_([this]() -> const Options & { return options_; },
		[this](const Options &o) { set_options(o, false); }),
	  window_("direbot_vertical_settings", obs_module_text("Vertical.Title"), &page_)
{
}

Vertical::~Vertical()
{
	unload();
}

const std::vector<std::string> &Vertical::requests()
{
	return REQUESTS;
}

bool Vertical::handles(const std::string &type)
{
	return std::find(REQUESTS.begin(), REQUESTS.end(), type) != REQUESTS.end();
}

void Vertical::start(Emit emit)
{
	if (started_)
		return;
	emit_ = std::move(emit);
	canvas_.on_removed = [this] {
		// The scene collection is going: what uses the picture goes
		// first. (Normally already stopped when it started changing.)
		outputs_.release(0);
		streams_with_main_ = record_with_main_ = false;
	};
	auto alive = alive_;
	outputs_.on_change = [this, alive] {
		// From an output's own thread.
		later(alive, [this] {
			if (closing_)
				return;
			outputs_.tidy();
			if (!outputs_.streaming())
				streams_with_main_ = false;
			if (!outputs_.recording())
				record_with_main_ = false;
			update_awake();
			changed();
		});
	};
	window_.start();
	obs_frontend_add_event_callback(&Vertical::on_frontend, this);
	obs_frontend_add_save_callback(&Vertical::on_save, this);
	frontend_ = true;
	signal_handler_connect(obs_get_signal_handler(), "video_reset", &Vertical::on_video_reset, this);
	started_ = true;
}

void Vertical::unload()
{
	*alive_ = false;
	if (!started_)
		return;
	started_ = false;
	closing_ = true;
	signal_handler_disconnect(obs_get_signal_handler(), "video_reset", &Vertical::on_video_reset, this);
	outputs_.release(0);
	canvas_.release();
	window_.stop();
	if (frontend_) {
		obs_frontend_remove_event_callback(&Vertical::on_frontend, this);
		obs_frontend_remove_save_callback(&Vertical::on_save, this);
		frontend_ = false;
	}
}

json Vertical::request(const std::string &type, const json &data)
{
	if (!started_ || closing_)
		return fail("OBS is closing.");
	if (obs_in_task_thread(OBS_TASK_UI))
		return do_request(type, data);

	auto job = std::make_shared<Job>();
	auto alive = alive_;
	job->work = [this, alive, type, data]() -> json {
		if (!*alive)
			return fail("OBS is closing.");
		return do_request(type, data);
	};
	obs_queue_task(OBS_TASK_UI, run_job, new std::shared_ptr<Job>(job), false);
	std::unique_lock<std::mutex> l(job->lock);
	if (!job->done_cv.wait_for(l, std::chrono::milliseconds(REQUEST_WAIT_MS), [&] { return job->done; }))
		return fail("OBS is busy right now. Try again in a moment.");
	return job->result;
}

void Vertical::tick()
{
	if (!started_ || closing_ || tick_queued_.exchange(true))
		return;
	later(alive_, [this] {
		tick_queued_ = false;
		if (!closing_)
			on_tick();
	});
}

void Vertical::on_tick()
{
	const uint64_t now = os_gettime_ns();
	if (looking_until_ns_ && now >= looking_until_ns_)
		looking_until_ns_ = 0;
	// An output's "stop" can come a moment before it's no longer active:
	// what it left is let go here then.
	if (!outputs_.active())
		outputs_.tidy();
	if (canvas_.awake())
		update_awake();
	// It couldn't have its video while OBS was sending or recording.
	if (canvas_.exists() && !canvas_.has_video() && !obs_video_active() && canvas_.ensure_video())
		changed();
	if (outputs_.active() && emit_ && now >= next_status_ns_) {
		next_status_ns_ = now + STATUS_EVERY_NS;
		emit_("VerticalStatus", {{"outputs", outputs_.status(options_)}});
	}
}

json Vertical::do_request(const std::string &type, const json &data)
{
	if (closing_)
		return fail("OBS is closing.");

	if (type == "VerticalGetState")
		return state();

	if (type == "VerticalGetEncoders") {
		json rows = json::array();
		for (const EncoderRow &row : h264_encoders())
			rows.push_back({{"id", row.id}, {"name", row.name}});
		const std::string picked = auto_encoder();
		const char *name = obs_encoder_get_display_name(picked.c_str());
		return {{"ok", true}, {"encoders", rows}, {"auto", {{"id", picked}, {"name", name ? name : picked}}}};
	}

	if (type == "VerticalSetUp") {
		const uint32_t w = (uint32_t)number(data, "width", width_ ? (int)width_ : (int)rules::DEFAULT_WIDTH);
		const uint32_t h = (uint32_t)number(data, "height", height_ ? (int)height_ : (int)rules::DEFAULT_HEIGHT);
		if (!rules::size_ok(w, h))
			return fail("That isn't a size the vertical picture can be: even numbers, 240 to 4096, and taller "
				    "than wide.");
		if (!canvas_.exists())
			attach();
		if (!canvas_.exists()) {
			std::string problem;
			if (!canvas_.create(w, h, problem))
				return fail(problem);
			saved_uuid_ = canvas_.uuid();
			width_ = w;
			height_ = h;
			update_awake();
			// Kept in the scene collection now, not just when OBS closes.
			obs_frontend_save();
		}
		changed();
		return state();
	}

	if (type == "VerticalSetSize") {
		const uint32_t w = (uint32_t)number(data, "width", 0);
		const uint32_t h = (uint32_t)number(data, "height", 0);
		std::string problem;
		if (!rules::size_ok(w, h))
			return fail("That isn't a size the vertical picture can be: even numbers, 240 to 4096, and taller "
				    "than wide.");
		if (canvas_.exists() && !canvas_.resize(w, h, problem))
			return fail(problem);
		width_ = w;
		height_ = h;
		obs_frontend_save();
		changed();
		return state();
	}

	if (type == "VerticalSetScene") {
		const std::string name = text(data, "scene");
		if (!canvas_.switch_to(name, flag(data, "animate", true)))
			return fail("There's no vertical scene called " + name + ".");
		changed();
		return state();
	}

	if (type == "VerticalCreateScene") {
		std::string problem;
		SceneRow row = canvas_.create_scene(text(data, "name"), problem);
		if (row.name.empty())
			return fail(problem);
		changed();
		json out = state();
		out["scene"] = {{"name", row.name}, {"uuid", row.uuid}};
		return out;
	}

	if (type == "VerticalRemoveScene") {
		const std::string name = text(data, "scene");
		std::string problem;
		if (!canvas_.remove_scene(name, problem))
			return fail(problem);
		for (auto it = links_.begin(); it != links_.end();)
			it = it->second == name ? links_.erase(it) : std::next(it);
		changed();
		return state();
	}

	if (type == "VerticalRenameScene") {
		const std::string name = text(data, "scene"), to = text(data, "to");
		std::string problem;
		if (!canvas_.rename_scene(name, to, problem))
			return fail(problem);
		for (auto &link : links_)
			if (link.second == name)
				link.second = to;
		changed();
		return state();
	}

	if (type == "VerticalSetFollow") {
		follow_ = flag(data, "on", follow_);
		auto list = data.find("links");
		if (list != data.end() && list->is_array()) {
			links_.clear();
			for (const json &row : *list) {
				if (!row.is_object())
					continue;
				// An empty tall scene: leave the tall picture as it is.
				const std::string main = text(row, "main"), tall = text(row, "tall");
				if (!main.empty())
					links_[main] = tall;
			}
		}
		follow_main(true);
		changed();
		return state();
	}

	if (type == "VerticalSetOptions") {
		Options next = options_;
		next.kbps = std::clamp(number(data, "kbps", next.kbps), rules::MIN_KBPS, rules::MAX_KBPS);
		if (has(data, "encoder")) {
			const std::string encoder = text(data, "encoder");
			next.encoder = encoder.empty() ? "auto" : encoder;
		}
		next.audio_track = std::clamp(number(data, "audio_track", next.audio_track), 0, 6);
		next.with_stream = flag(data, "with_stream", next.with_stream);
		next.record_with_main = flag(data, "record_with_main", next.record_with_main);
		// Which site, on or off, and its sound. Servers and keys only
		// from OBS's window.
		auto places = data.find("places");
		if (places != data.end() && places->is_array()) {
			for (const json &row : *places) {
				const int slot = row.is_object() ? number(row, "slot", 0) : 0;
				if (slot < 1 || slot > Options::SLOTS)
					continue;
				Destination &d = next.places[slot - 1];
				const std::string site = text(row, "platform");
				if (!site.empty() && site != d.platform) {
					const std::string old_usual = rules::usual_server(d.platform);
					Options probe;
					probe.places[0].platform = site;
					d.platform = Options::from_file(probe.to_file()).places[0].platform;
					if (d.server.empty() || d.server == old_usual)
						d.server = rules::usual_server(d.platform);
				}
				d.enabled = flag(row, "enabled", d.enabled);
				// Its sound: an audio track, 0 for the one OBS's stream
				// sends. Used from its next start.
				d.audio_track = std::clamp(number(row, "audio_track", d.audio_track), 0, 6);
			}
		}
		set_options(next, true);
		return state();
	}

	if (type == "VerticalStartStream") {
		const int slot = number(data, "slot", 0);
		const std::string problem = start_streams(slot);
		streams_with_main_ = false;
		json out = state();
		if (!problem.empty() && !outputs_.streaming()) {
			out["ok"] = false;
			out["error"] = problem;
		} else if (!problem.empty()) {
			out["warning"] = problem;
		}
		return out;
	}

	if (type == "VerticalStopStream") {
		outputs_.stop_streams(number(data, "slot", 0));
		return state();
	}

	if (type == "VerticalStartRecord") {
		const std::string problem = start_record();
		record_with_main_ = false;
		if (!problem.empty())
			return fail(problem);
		return state();
	}

	if (type == "VerticalStopRecord") {
		outputs_.stop_record();
		return state();
	}

	if (type == "VerticalPauseRecord") {
		outputs_.pause_record(flag(data, "pause", true));
		changed();
		return state();
	}

	if (type == "VerticalOpenSettings") {
		window_.open();
		return {{"ok", true}};
	}

	if (type == "VerticalLook") {
		const int seconds = std::clamp(number(data, "seconds", 30), 0, MAX_LOOK_SECONDS);
		looking_until_ns_ = seconds ? os_gettime_ns() + (uint64_t)seconds * 1000 * MS : 0;
		update_awake();
		return {{"ok", true}, {"awake", canvas_.awake()}};
	}

	return fail("No request was found by that name.");
}

json Vertical::state()
{
	json scenes = json::array();
	for (const SceneRow &row : canvas_.scenes())
		scenes.push_back({{"name", row.name}, {"uuid", row.uuid}});
	json links = json::array();
	for (const auto &link : links_)
		links.push_back({{"main", link.first}, {"tall", link.second}});
	const bool exists = canvas_.exists();
	return {{"ok", true},
		{"name", rules::CANVAS_NAME},
		{"exists", exists},
		{"uuid", exists ? canvas_.uuid() : std::string()},
		{"video", canvas_.has_video()},
		{"width", exists ? canvas_.width() : (width_ ? width_ : rules::DEFAULT_WIDTH)},
		{"height", exists ? canvas_.height() : (height_ ? height_ : rules::DEFAULT_HEIGHT)},
		{"fps", canvas_.fps()},
		{"awake", canvas_.awake()},
		{"scenes", scenes},
		{"current", canvas_.current()},
		{"follow", {{"on", follow_}, {"links", links}}},
		{"options", options_.to_public()},
		{"outputs", outputs_.status(options_)},
		{"streaming", outputs_.streaming()},
		{"recording", outputs_.recording()},
		{"with_main", {{"streams", streams_with_main_}, {"record", record_with_main_}}},
		{"twitch", twitch_uses_it()},
		{"settings_open", window_.is_open()},
		{"problem", last_problem_}};
}

void Vertical::attach()
{
	if (canvas_.exists())
		return;
	if (!canvas_.attach(saved_uuid_, width_, height_, saved_scene_))
		return;
	saved_uuid_ = canvas_.uuid();
	width_ = canvas_.width();
	height_ = canvas_.height();
	update_awake();
	follow_main(false);
}

void Vertical::let_go(uint32_t wait_ms)
{
	const bool had = outputs_.active() || canvas_.exists();
	outputs_.release(wait_ms);
	streams_with_main_ = record_with_main_ = false;
	if (canvas_.exists())
		saved_scene_ = canvas_.current();
	canvas_.release();
	if (had && !closing_)
		changed();
}

void Vertical::leave_profile()
{
	// Where it goes and the keys are the profile's: its streams and
	// recording end with it. The tall picture stays (it's the scene
	// collection's).
	if (!outputs_.active())
		return;
	obs_log(LOG_INFO, "Another profile: the vertical streams and recording stop");
	outputs_.release(CLOSE_WAIT_MS);
	streams_with_main_ = record_with_main_ = false;
	update_awake();
	changed();
}

bool Vertical::twitch_uses_it() const
{
	if (!canvas_.exists())
		return false;
	config_t *config = obs_frontend_get_profile_config();
	if (!config || !config_get_bool(config, "Stream1", "EnableMultitrackVideo"))
		return false;
	const char *uuid = config_get_string(config, "Stream1", "MultitrackExtraCanvas");
	return uuid && canvas_.uuid() == uuid;
}

void Vertical::update_awake()
{
	// Drawn only while something uses it.
	const bool twitch = twitch_uses_it() && (main_starting_ || obs_frontend_streaming_active());
	const bool looking = looking_until_ns_ && os_gettime_ns() < looking_until_ns_;
	canvas_.set_awake(outputs_.active() || looking || twitch);
}

void Vertical::follow_main(bool animate)
{
	if (!follow_ || !canvas_.exists())
		return;
	const std::string pick = rules::follow_pick(main_scene_name(), canvas_.scene_names(), links_);
	if (!pick.empty() && pick != canvas_.current() && canvas_.switch_to(pick, animate))
		changed();
}

void Vertical::load_options()
{
	options_ = Options();
	const std::string path = profile_file();
	char *text = path.empty() ? nullptr : os_quick_read_utf8_file(path.c_str());
	if (text) {
		json j = json::parse(text, nullptr, false);
		if (!j.is_discarded())
			options_ = Options::from_file(j);
		bfree(text);
	}
	refresh_window();
}

void Vertical::save_options()
{
	const std::string path = profile_file();
	if (path.empty())
		return;
	const std::string body = options_.to_file().dump(1, '\t', false, json::error_handler_t::replace);
	if (!os_quick_write_utf8_file_safe(path.c_str(), body.c_str(), body.size(), false, "tmp", "bak"))
		obs_log(LOG_WARNING, "Vertical: couldn't save %s.", OPTIONS_FILE);
}

void Vertical::set_options(const Options &options, bool from_direbot)
{
	options_ = options;
	save_options();
	// The window is drawn again only for a change from DireBot: not while
	// someone types in it.
	if (from_direbot)
		refresh_window();
	changed();
}

void Vertical::refresh_window()
{
	const Options copy = options_;
	window_.change([copy](obs_data_t *settings) { SettingsPage::fill(settings, copy); });
}

void Vertical::changed()
{
	if (emit_ && loaded_ && !closing_)
		emit_("VerticalChanged", state());
}

std::string Vertical::start_streams(int slot)
{
	if (!canvas_.exists())
		return "Set up the vertical picture first.";
	if (!canvas_.ensure_video())
		return "The vertical picture isn't ready yet.";
	// Awake first, so the first frames aren't black.
	canvas_.set_awake(true);
	const std::string problem = outputs_.start_streams(canvas_.video(), options_, slot);
	last_problem_ = problem;
	if (!problem.empty())
		obs_log(LOG_WARNING, "Vertical: not every stream started.");
	update_awake();
	changed();
	return problem;
}

std::string Vertical::start_record()
{
	if (!canvas_.exists())
		return "Set up the vertical picture first.";
	if (!canvas_.ensure_video())
		return "The vertical picture isn't ready yet.";
	canvas_.set_awake(true);
	const std::string problem = outputs_.start_record(canvas_.video(), options_);
	last_problem_ = problem;
	update_awake();
	changed();
	return problem;
}

void Vertical::start_with_main_stream()
{
	if (!options_.with_stream || !canvas_.exists() || !options_.any_ready() || outputs_.streaming())
		return;
	start_streams(0);
	streams_with_main_ = outputs_.streaming();
}

void Vertical::on_video_reset(void *self, calldata_t *)
{
	auto *v = static_cast<Vertical *>(self);
	// OBS's own frame rate changed: the tall picture follows it.
	later(v->alive_, [v] {
		if (!v->closing_ && !v->outputs_.active()) {
			v->outputs_.tidy();
			v->canvas_.main_video_changed();
		}
	});
}

void Vertical::on_frontend(enum obs_frontend_event event, void *self)
{
	auto *v = static_cast<Vertical *>(self);
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		v->loaded_ = true;
		v->load_options();
		v->attach();
		v->changed();
		break;
	case OBS_FRONTEND_EVENT_PROFILE_CHANGING:
		v->leave_profile();
		break;
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
		// OBS 31 and up makes a new profile and switches to it without
		// saying it's changing first.
		v->leave_profile();
		v->load_options();
		v->changed();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
		v->let_go(CLOSE_WAIT_MS);
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		v->let_go(CLOSE_WAIT_MS);
		// The next collection brings its own (or none).
		v->saved_uuid_.clear();
		v->saved_scene_.clear();
		v->width_ = v->height_ = 0;
		v->follow_ = false;
		v->links_.clear();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		v->attach();
		v->changed();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		v->follow_main(true);
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTING:
		v->main_starting_ = true;
		v->update_awake();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		v->main_starting_ = false;
		v->start_with_main_stream();
		v->update_awake();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
		if (v->streams_with_main_)
			v->outputs_.stop_streams();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		v->main_starting_ = false;
		v->update_awake();
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		if (v->options_.record_with_main && v->canvas_.exists() && !v->outputs_.recording())
			v->record_with_main_ = v->start_record().empty();
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPING:
		if (v->record_with_main_)
			v->outputs_.stop_record();
		break;
	case OBS_FRONTEND_EVENT_RECORDING_PAUSED:
	case OBS_FRONTEND_EVENT_RECORDING_UNPAUSED:
		if (v->record_with_main_) {
			v->outputs_.pause_record(event == OBS_FRONTEND_EVENT_RECORDING_PAUSED);
			v->changed();
		}
		break;
	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
		// OBS is closing: recordings finish their files while OBS's
		// window still waits for them.
		v->closing_ = true;
		v->let_go(CLOSE_WAIT_MS);
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		v->closing_ = true;
		v->let_go(0);
		v->window_.stop();
		// Now, while OBS's window still has its lists.
		obs_frontend_remove_event_callback(&Vertical::on_frontend, v);
		obs_frontend_remove_save_callback(&Vertical::on_save, v);
		v->frontend_ = false;
		break;
	default:
		break;
	}
}

void Vertical::on_save(obs_data_t *data, bool saving, void *self)
{
	auto *v = static_cast<Vertical *>(self);
	if (saving) {
		// Only in a collection it was set up in.
		if (!v->canvas_.exists() && v->saved_uuid_.empty())
			return;
		obs_data_t *mine = obs_data_create();
		obs_data_set_string(mine, "uuid", v->canvas_.exists() ? v->canvas_.uuid().c_str() : v->saved_uuid_.c_str());
		obs_data_set_int(mine, "width", v->canvas_.exists() ? v->canvas_.width() : v->width_);
		obs_data_set_int(mine, "height", v->canvas_.exists() ? v->canvas_.height() : v->height_);
		obs_data_set_string(mine, "scene", v->canvas_.exists() ? v->canvas_.current().c_str() : v->saved_scene_.c_str());
		obs_data_set_bool(mine, "follow", v->follow_);
		obs_data_array_t *links = obs_data_array_create();
		for (const auto &link : v->links_) {
			obs_data_t *row = obs_data_create();
			obs_data_set_string(row, "main", link.first.c_str());
			obs_data_set_string(row, "tall", link.second.c_str());
			obs_data_array_push_back(links, row);
			obs_data_release(row);
		}
		obs_data_set_array(mine, "links", links);
		obs_data_array_release(links);
		obs_data_set_obj(data, SAVE_KEY, mine);
		obs_data_release(mine);
		return;
	}

	// A scene collection loading: OBS has made its canvases by now.
	obs_data_t *mine = obs_data_get_obj(data, SAVE_KEY);
	if (mine) {
		v->saved_uuid_ = obs_data_get_string(mine, "uuid");
		v->saved_scene_ = obs_data_get_string(mine, "scene");
		v->width_ = (uint32_t)obs_data_get_int(mine, "width");
		v->height_ = (uint32_t)obs_data_get_int(mine, "height");
		v->follow_ = obs_data_get_bool(mine, "follow");
		v->links_.clear();
		obs_data_array_t *links = obs_data_get_array(mine, "links");
		for (size_t i = 0; links && i < obs_data_array_count(links); i++) {
			obs_data_t *row = obs_data_array_item(links, i);
			const std::string main = obs_data_get_string(row, "main"), tall = obs_data_get_string(row, "tall");
			if (!main.empty())
				v->links_[main] = tall;
			obs_data_release(row);
		}
		obs_data_array_release(links);
		obs_data_release(mine);
	}
	v->attach();
}

} // namespace direbot::vertical
