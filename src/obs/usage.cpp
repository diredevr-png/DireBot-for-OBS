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

#include "usage.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

#include <obs.h>
#include <obs-frontend-api.h>

#if LIBOBS_API_MAJOR_VER >= 31
#include <util/source-profiler.h>
#define HAVE_PROFILER true
#else
// OBS 30 has no source profiler (the Windows build is for 31.1.1 and up;
// this is for building elsewhere): the report has the overall numbers.
#define HAVE_PROFILER false
typedef struct profiler_result {
	uint64_t tick_avg, tick_max, render_avg, render_max, render_gpu_avg, render_gpu_max, render_sum, render_gpu_sum;
	double async_input, async_rendered;
	uint64_t async_input_best, async_input_worst, async_rendered_best, async_rendered_worst;
} profiler_result_t;
static inline void source_profiler_enable(bool) {}
static inline void source_profiler_gpu_enable(bool) {}
static inline bool source_profiler_fill_result(obs_source_t *, profiler_result_t *)
{
	return false;
}
#endif

#include "../link/crypto.hpp"
#include "address.hpp"

namespace direbot::obs {

namespace {

constexpr uint64_t SECOND_NS = 1'000'000'000;
// Nobody's asked for this long: the profiler goes off.
constexpr uint64_t IDLE_NS = 60 * SECOND_NS;
// The profiler needs a few frames before its numbers mean anything.
constexpr uint64_t WARM_UP_NS = 3 * SECOND_NS;
// A big scene collection still makes a small answer.
constexpr size_t MAX_SOURCES = 400;

double ms(uint64_t ns)
{
	return std::round((double)ns / 1000.0) / 1000.0;
}

double rounded(double v, double places = 1000.0)
{
	return std::isfinite(v) ? std::round(v * places) / places : 0.0;
}

std::string text(const char *s)
{
	return s ? s : "";
}

// The first 12 hex digits of a SHA-256: enough to see the same camera
// added twice, without saying which camera it is.
std::string short_hash(const std::string &value)
{
	static const char *hex = "0123456789abcdef";
	const std::string digest = crypto::sha256(value);
	std::string out;
	for (size_t i = 0; i < 6 && i < digest.size(); i++) {
		out += hex[(unsigned char)digest[i] >> 4];
		out += hex[(unsigned char)digest[i] & 15];
	}
	return out;
}

// A few settings that say why a source is slow, by kind of source.
json hints(obs_source_t *source, const std::string &kind)
{
	json h = json::object();
	obs_data_t *s = obs_source_get_settings(source);
	if (!s)
		return h;
	if (kind == "browser_source") {
		h["width"] = obs_data_get_int(s, "width");
		h["height"] = obs_data_get_int(s, "height");
		h["fps_custom"] = obs_data_get_bool(s, "fps_custom");
		h["fps"] = obs_data_get_int(s, "fps");
		h["shutdown"] = obs_data_get_bool(s, "shutdown");
		h["local_file"] = obs_data_get_bool(s, "is_local_file");
		int port = 0;
		const bool here = !obs_data_get_bool(s, "is_local_file") && local_address(text(obs_data_get_string(s, "url")), port);
		h["this_pc"] = here;
		if (here)
			h["port"] = port;
	} else if (kind == "dshow_input") {
		const std::string device = text(obs_data_get_string(s, "video_device_id"));
		if (!device.empty())
			h["device"] = short_hash(device);
		h["resolution"] = text(obs_data_get_string(s, "resolution"));
	} else if (kind == "av_capture_input" || kind == "macos-avcapture" || kind == "macos-avcapture-fast") {
		const std::string device = text(obs_data_get_string(s, "device"));
		if (!device.empty())
			h["device"] = short_hash(device);
	} else if (kind == "v4l2_input") {
		const std::string device = text(obs_data_get_string(s, "device_id"));
		if (!device.empty())
			h["device"] = short_hash(device);
	} else if (kind == "ffmpeg_source") {
		h["hw_decode"] = obs_data_get_bool(s, "hw_decode");
		h["local_file"] = obs_data_get_bool(s, "is_local_file");
	} else if (kind == "noise_suppress_filter") {
		h["method"] = text(obs_data_get_string(s, "method"));
	}
	obs_data_release(s);
	return h;
}

// What's drawn under a filter: a filter's time takes in everything it
// draws on (the filters before it, and the source), so its own share is
// its time less that.
struct Below {
	uint64_t cpu = 0, gpu = 0;
};

json entry(obs_source_t *source, const char *type, obs_source_t *parent, Below *below = nullptr)
{
	const std::string kind = text(obs_source_get_unversioned_id(source));
	json e = {{"name", text(obs_source_get_name(source))},
		  {"uuid", text(obs_source_get_uuid(source))},
		  {"kind", kind},
		  {"type", type},
		  {"active", obs_source_active(source)},
		  {"showing", obs_source_showing(source)}};
	if (parent) {
		e["parent"] = text(obs_source_get_name(parent));
		e["parent_uuid"] = text(obs_source_get_uuid(parent));
		e["enabled"] = obs_source_enabled(source);
	}
	profiler_result_t r = {};
	const bool measured = source_profiler_fill_result(source, &r);
	e["measured"] = measured;
	if (measured) {
		e["tick_ms"] = ms(r.tick_avg);
		e["tick_max_ms"] = ms(r.tick_max);
		// Every time it's drawn in a frame, added up: a source in two
		// scenes on screen at once costs twice.
		e["render_ms"] = ms(r.render_sum);
		e["render_max_ms"] = ms(r.render_max);
		e["gpu_ms"] = ms(r.render_gpu_sum);
		e["gpu_max_ms"] = ms(r.render_gpu_max);
		if (below) {
			if (parent && r.render_sum > 0 && obs_source_enabled(source)) {
				e["own_ms"] = ms(r.render_sum > below->cpu ? r.render_sum - below->cpu : 0);
				e["own_gpu_ms"] = ms(r.render_gpu_sum > below->gpu ? r.render_gpu_sum - below->gpu : 0);
			}
			// A filter that's off draws nothing of its own, and one that
			// drew nothing this time (audio filters) leaves it as it was.
			if (r.render_sum > 0 && (!parent || obs_source_enabled(source)))
				*below = {r.render_sum, r.render_gpu_sum};
		}
		if (obs_source_get_output_flags(source) & OBS_SOURCE_ASYNC_VIDEO) {
			e["fps_in"] = rounded(r.async_input, 10.0);
			e["fps_shown"] = rounded(r.async_rendered, 10.0);
		}
	}
	json h = hints(source, kind);
	if (!h.empty())
		e["hints"] = std::move(h);
	return e;
}

std::vector<obs_source_t *> filters_of(obs_source_t *parent)
{
	std::vector<obs_source_t *> out;
	obs_source_enum_filters(
		parent,
		[](obs_source_t *, obs_source_t *filter, void *param) {
			obs_source_t *ref = obs_source_get_ref(filter);
			if (ref)
				static_cast<std::vector<obs_source_t *> *>(param)->push_back(ref);
		},
		&out);
	return out;
}

bool collect(void *param, obs_source_t *source)
{
	obs_source_t *ref = obs_source_get_ref(source);
	if (ref)
		static_cast<std::vector<obs_source_t *> *>(param)->push_back(ref);
	return true;
}

void release_all(std::vector<obs_source_t *> &list)
{
	for (obs_source_t *s : list)
		obs_source_release(s);
	list.clear();
}

} // namespace

Usage::~Usage()
{
	stop();
}

void Usage::turn_on()
{
	// Something else (another plugin) has the profiler on already when a
	// source has numbers before we've asked: then it's left as it is.
	std::vector<obs_source_t *> inputs;
	obs_enum_sources(collect, &inputs);
	bool already = false;
	for (obs_source_t *s : inputs) {
		profiler_result_t r;
		if (source_profiler_fill_result(s, &r)) {
			already = true;
			break;
		}
	}
	release_all(inputs);
	ours_ = !already;
	if (ours_) {
		source_profiler_enable(true);
		source_profiler_gpu_enable(true);
	}
	on_ = true;
	since_ = os_gettime_ns();
	if (!cpu_)
		cpu_ = os_cpu_usage_info_start();
	base_total_ = obs_get_total_frames();
	base_lagged_ = obs_get_lagged_frames();
	video_t *video = obs_get_video();
	base_video_total_ = video ? video_output_get_total_frames(video) : 0;
	base_skipped_ = video ? video_output_get_skipped_frames(video) : 0;
}

void Usage::turn_off()
{
	if (ours_) {
		source_profiler_gpu_enable(false);
		source_profiler_enable(false);
	}
	if (cpu_)
		os_cpu_usage_info_destroy(cpu_);
	cpu_ = nullptr;
	on_ = ours_ = false;
}

void Usage::tick()
{
	std::lock_guard<std::mutex> l(lock_);
	if (on_ && os_gettime_ns() - asked_ > IDLE_NS)
		turn_off();
}

void Usage::stop()
{
	std::lock_guard<std::mutex> l(lock_);
	if (on_)
		turn_off();
}

json Usage::report()
{
	std::lock_guard<std::mutex> l(lock_);
	const uint64_t now = os_gettime_ns();
	if (!on_)
		turn_on();
	else if (ours_)
		source_profiler_enable(true); // in case something else turned it off
	asked_ = now;

	json out = json::object();
	out["profiler"] = HAVE_PROFILER;
	out["measuring"] = HAVE_PROFILER && ours_ && now - since_ < WARM_UP_NS;
	out["measured_ms"] = (uint64_t)((now - since_) / 1'000'000);

	obs_video_info ovi = {};
	double fps = 0;
	if (obs_get_video_info(&ovi) && ovi.fps_den) {
		fps = (double)ovi.fps_num / (double)ovi.fps_den;
		out["base"] = {{"width", ovi.base_width}, {"height", ovi.base_height}};
		out["output"] = {{"width", ovi.output_width}, {"height", ovi.output_height}};
	}
	out["fps"] = rounded(fps, 100.0);
	out["fps_now"] = rounded(obs_get_active_fps(), 100.0);
	out["frame_ms"] = ms(obs_get_average_frame_time_ns());
	out["frame_budget_ms"] = fps > 0 ? rounded(1000.0 / fps) : 0.0;
	// Counted since measuring started, the way OBS's Stats window counts.
	out["frames"] = (uint32_t)(obs_get_total_frames() - base_total_);
	out["frames_lagged"] = (uint32_t)(obs_get_lagged_frames() - base_lagged_);
	video_t *video = obs_get_video();
	if (video) {
		out["encoded"] = (uint32_t)(video_output_get_total_frames(video) - base_video_total_);
		out["encoded_skipped"] = (uint32_t)(video_output_get_skipped_frames(video) - base_skipped_);
	}
	out["cpu"] = cpu_ ? rounded(os_cpu_usage_info_query(cpu_), 10.0) : 0.0;
	out["memory_mb"] = (uint64_t)(os_get_proc_resident_size() / (1024 * 1024));
	obs_output_t *stream = obs_frontend_get_streaming_output();
	if (stream) {
		if (obs_output_active(stream)) {
			out["stream_frames"] = obs_output_get_total_frames(stream);
			out["stream_dropped"] = obs_output_get_frames_dropped(stream);
		}
		obs_output_release(stream);
	}

	// Inputs and their filters, then the filters on scenes. Scenes and
	// transitions themselves are left out: a scene's time is just its
	// sources' added up.
	json sources = json::array();
	std::vector<obs_source_t *> list;
	obs_enum_sources(collect, &list);
	// Filters come first-applied first, each drawing on the one before.
	for (obs_source_t *input : list) {
		if (sources.size() >= MAX_SOURCES)
			break;
		Below below;
		sources.push_back(entry(input, "input", nullptr, &below));
		std::vector<obs_source_t *> filters = filters_of(input);
		for (obs_source_t *f : filters)
			if (sources.size() < MAX_SOURCES)
				sources.push_back(entry(f, "filter", input, &below));
		release_all(filters);
	}
	release_all(list);
	obs_enum_scenes(collect, &list);
	for (obs_source_t *scene : list) {
		std::vector<obs_source_t *> filters = filters_of(scene);
		if (filters.empty())
			continue;
		Below below;
		profiler_result_t r = {};
		if (source_profiler_fill_result(scene, &r))
			below = {r.render_sum, r.render_gpu_sum};
		for (obs_source_t *f : filters)
			if (sources.size() < MAX_SOURCES)
				sources.push_back(entry(f, "filter", scene, &below));
		release_all(filters);
	}
	release_all(list);
	out["sources"] = std::move(sources);
	return out;
}

} // namespace direbot::obs
