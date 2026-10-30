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

// Doing something on OBS's UI thread from any other, the way the vertical
// picture does (vertical.cpp has the long story):
//
// - ui_call: runs it there and waits a few seconds for the answer. Not
//   begun in time: it never will be, and the answer says so.
// - later: runs it there later, always by way of the graphics thread, so
//   it's never inside the caller (an output let go inside its own "stop"
//   signal would make OBS wait on itself).
//
// `alive` goes false when the part using it unloads; work queued before
// then is skipped.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

#include <obs.h>
#include <nlohmann/json.hpp>

namespace direbot::obs {

namespace ui_detail {

struct Job {
	std::mutex lock;
	std::condition_variable done_cv;
	bool done = false, started = false, cancelled = false;
	nlohmann::json result;
	std::function<nlohmann::json()> work;
};

inline void run(void *param)
{
	std::unique_ptr<std::shared_ptr<Job>> holder(static_cast<std::shared_ptr<Job> *>(param));
	std::shared_ptr<Job> job = *holder;
	{
		std::lock_guard<std::mutex> l(job->lock);
		if (job->cancelled)
			return;
		job->started = true;
	}
	nlohmann::json result;
	try {
		result = job->work();
	} catch (const std::exception &) {
		result = {{"ok", false}, {"error", "DireBot for OBS couldn't do that."}};
	}
	{
		std::lock_guard<std::mutex> l(job->lock);
		job->result = std::move(result);
		job->done = true;
	}
	job->done_cv.notify_all();
}

} // namespace ui_detail

inline nlohmann::json ui_call(const std::shared_ptr<std::atomic<bool>> &alive, std::function<nlohmann::json()> work,
			      int wait_ms = 5000)
{
	auto fail = [](const char *words) { return nlohmann::json{{"ok", false}, {"error", words}}; };
	if (!*alive)
		return fail("OBS is closing.");
	if (obs_in_task_thread(OBS_TASK_UI))
		return work();
	auto job = std::make_shared<ui_detail::Job>();
	job->work = [alive, work = std::move(work)]() -> nlohmann::json {
		if (!*alive)
			return {{"ok", false}, {"error", "OBS is closing."}};
		return work();
	};
	obs_queue_task(OBS_TASK_UI, ui_detail::run, new std::shared_ptr<ui_detail::Job>(job), false);
	std::unique_lock<std::mutex> l(job->lock);
	if (job->done_cv.wait_for(l, std::chrono::milliseconds(wait_ms), [&] { return job->done; }))
		return job->result;
	if (!job->started) {
		job->cancelled = true;
		return fail("OBS is busy right now. Try again in a moment.");
	}
	if (job->done_cv.wait_for(l, std::chrono::milliseconds(wait_ms), [&] { return job->done; }))
		return job->result;
	return fail("OBS is still doing that. Look again in a moment.");
}

inline void later(const std::shared_ptr<std::atomic<bool>> &alive, std::function<void()> work)
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

} // namespace direbot::obs
