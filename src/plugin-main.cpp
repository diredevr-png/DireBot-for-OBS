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

// DireBot for OBS.
//
// DireBot Link: once OBS has loaded, a connection only DireBot can use
// opens on this PC, and a file tells DireBot where it is and its password
// (link-file.hpp), so nobody sets up a websocket password or port. It
// speaks obs-websocket's language, answered by obs-websocket itself.
//
// "What's slowing OBS": DireBot asks for the report (usage.hpp) through
// the link, with CallVendorRequest, vendor "direbot".
//
// Closing OBS: nothing here ever makes OBS's window wait. When OBS starts
// closing, new requests are turned down and the link file goes; the
// threads are only waited for when the plugin is unloaded, after OBS's
// window has finished its last jobs - a request already running can need
// the window (switching scene collection does), so waiting for it any
// earlier could freeze OBS.
//
// Later parts plug in beside these: multistream, vertical, auto-duck and
// captions each get their own folder under src/, their own vendor
// requests, and a line in features().

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>

#include <plugin-support.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "link/crypto.hpp"
#include "link/server.hpp"
#include "obs/link-file.hpp"
#include "obs/meters.hpp"
#include "obs/usage.hpp"
#include "obs/websocket.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")
OBS_MODULE_AUTHOR("DireBot")

MODULE_EXPORT const char *obs_module_name(void)
{
	return "DireBot for OBS";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Description");
}

namespace {

using namespace direbot;
using link::json;

// What DireBot can count on from this version of the plugin. Bumped when
// a request changes in a way DireBot has to know about.
constexpr int API = 1;
// Not obs-websocket's 4455, so the two never fight over a port.
constexpr uint16_t PREFERRED_PORT = 4466;
constexpr uint64_t MS = 1'000'000;
constexpr uint64_t METER_EVERY_NS = 50 * MS; // as obs-websocket does
constexpr int MAX_LISTEN_TRIES = 5;

class Plugin {
public:
	Plugin()
		: server_(ws_, [](link::Server::LogLevel level, const std::string &line) {
			  obs_log(level == link::Server::Warning ? LOG_WARNING : LOG_INFO, "%s", line.c_str());
		  })
	{
	}

	void post_load()
	{
		if (!ws_.find()) {
			obs_log(LOG_WARNING,
				"OBS's websocket plugin (obs-websocket) is missing or too old, so DireBot Link is off. "
				"DireBot can still connect with the websocket password.");
			return;
		}
		ws_.connect([this](const std::string &type, const json &data,
				   bool &found) { return vendor_request(type, data, found); },
			    [this](uint64_t intent, const char *type, const char *data) { on_event(intent, type, data); },
			    {"GetInfo", "GetUsage"});
		server_.on_wanted_changed = [this](uint64_t) { poke(); };
		obs_frontend_add_event_callback(&Plugin::on_frontend, this);
		frontend_ = true;
		ticker_ = std::thread([this] { run(); });
	}

	// obs-websocket may be unloaded already, so nothing here calls it:
	// that was done at OBS_FRONTEND_EVENT_EXIT.
	void unload()
	{
		closing();
		stop_ticker();
		server_.stop();
		if (frontend_)
			obs_frontend_remove_event_callback(&Plugin::on_frontend, this);
		frontend_ = false;
		usage_.stop();
	}

private:
	obs::WebSocket ws_;
	link::Server server_;
	obs::LinkFile link_file_;
	obs::Meters meters_;
	obs::Usage usage_;
	bool frontend_ = false;

	std::thread ticker_;
	std::mutex lock_;
	std::condition_variable wake_;
	bool poked_ = false;
	bool quit_ = false;
	std::atomic<bool> loaded_{false};
	std::atomic<bool> closing_{false};
	std::atomic<bool> disconnected_{false};

	static void on_frontend(enum obs_frontend_event event, void *param)
	{
		auto *p = static_cast<Plugin *>(param);
		switch (event) {
		case OBS_FRONTEND_EVENT_FINISHED_LOADING:
			p->loaded_ = true;
			p->poke();
			break;
		case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
			p->closing();
			break;
		case OBS_FRONTEND_EVENT_EXIT:
			p->closing();
			// The ticker never waits for OBS's window, so this can't freeze.
			p->stop_ticker();
			p->usage_.stop();
			p->disconnect();
			// Now, while OBS's window still has its list: by unload it's
			// gone. (OBS copes with a callback taking itself off.)
			obs_frontend_remove_event_callback(&Plugin::on_frontend, p);
			p->frontend_ = false;
			break;
		default:
			break;
		}
	}

	// From OBS's window: never waits.
	void closing()
	{
		if (closing_.exchange(true))
			return;
		server_.begin_stop();
		link_file_.remove();
		poke();
	}

	// While obs-websocket is surely still loaded.
	void disconnect()
	{
		if (!disconnected_.exchange(true))
			ws_.disconnect();
	}

	void poke()
	{
		std::lock_guard<std::mutex> l(lock_);
		poked_ = true;
		wake_.notify_one();
	}

	void stop_ticker()
	{
		{
			std::lock_guard<std::mutex> l(lock_);
			quit_ = true;
			wake_.notify_one();
		}
		if (ticker_.joinable())
			ticker_.join();
	}

	void on_event(uint64_t intent, const char *type, const char *data)
	{
		// Most events go to nobody: skip building the message.
		if (!server_.running() || !(server_.wanted() & intent))
			return;
		server_.broadcast(intent, link::event_message(type, intent, data));
	}

	json vendor_request(const std::string &type, const json &, bool &found)
	{
		found = true;
		if (type == "GetInfo")
			return info();
		if (type == "GetUsage")
			return usage_.report();
		found = false;
		return nullptr;
	}

	json info()
	{
		return {{"plugin", PLUGIN_VERSION},
			{"api", API},
			{"obs", obs_get_version_string()},
			{"link", server_.running()},
			{"link_port", server_.port()},
			{"features", {{"link", true}, {"meters", true}, {"usage", true}}}};
	}

	// One small thread for everything that runs on a clock: opening the
	// link once OBS is ready, the audio levels, and turning the usage
	// profiler off when nobody's looking.
	void run()
	{
		uint64_t next_try = 0, next_meters = 0, next_second = 0;
		int listen_failures = 0;
		std::unique_lock<std::mutex> l(lock_);
		while (!quit_) {
			// Sleeps until the next thing that's due, or a poke.
			uint64_t until = next_second;
			if (meters_.attached())
				until = std::min(until, next_meters);
			if (loaded_ && !closing_ && !server_.running() && listen_failures < MAX_LISTEN_TRIES)
				until = std::min(until, next_try);
			const uint64_t before = os_gettime_ns();
			if (until > before)
				wake_.wait_for(l, std::chrono::nanoseconds(until - before),
					       [this] { return quit_ || poked_; });
			poked_ = false;
			if (quit_)
				break;
			l.unlock();

			const uint64_t now = os_gettime_ns();
			const bool closing = closing_;
			if (loaded_ && !closing && !server_.running() && listen_failures < MAX_LISTEN_TRIES &&
			    now >= next_try) {
				switch (open_link()) {
				case Opened:
					break;
				case NotReady:
					next_try = now + 1000 * MS;
					break;
				case Failed:
					listen_failures++;
					next_try = now + 15000 * MS;
					break;
				}
			}

			const bool want_meters = !closing && server_.running() &&
						 (server_.wanted() & link::subs::InputVolumeMeters);
			if (want_meters && !meters_.attached()) {
				meters_.attach();
				next_meters = now;
			} else if (!want_meters && meters_.attached()) {
				meters_.detach();
			}
			if (meters_.attached() && now >= next_meters) {
				server_.broadcast(link::subs::InputVolumeMeters,
						  link::event_message("InputVolumeMeters", link::subs::InputVolumeMeters,
								      meters_.read()),
						  true);
				// Keeps to the beat even when a wake-up comes late.
				next_meters += METER_EVERY_NS;
				if (next_meters <= now)
					next_meters = now + METER_EVERY_NS;
			}
			if (now >= next_second) {
				usage_.tick();
				next_second = now + 1000 * MS;
			}

			l.lock();
		}
		l.unlock();
		meters_.detach();
	}

	enum OpenResult { Opened, NotReady, Failed };

	OpenResult open_link()
	{
		// obs-websocket answers once OBS has finished loading; until then
		// (or while a scene collection loads) there's nothing to link to.
		link::RequestResult version = ws_.call("GetVersion", json::object());
		if (version.code != link::status::Success || !version.data.is_object())
			return NotReady;
		const std::string obs_version = version.data.value("obsVersion", "");
		const std::string ws_version = version.data.value("obsWebSocketVersion", "");

		const std::string password = crypto::base64url(crypto::random_bytes(32));
		if (password.size() < 40) {
			obs_log(LOG_WARNING, "Windows didn't give a random password, so DireBot Link is off.");
			return Failed;
		}
		link::Server::Options options;
		options.password = password;
		options.websocket_version = ws_version;
		options.preferred_port = PREFERRED_PORT;
		// A request can wait on OBS's window (adding a scene does), so a
		// few at once: one busy client never holds up the others.
		options.workers = std::clamp<size_t>(std::thread::hardware_concurrency(), 2, 4);
		const uint16_t port = server_.start(options);
		if (!port)
			return Failed;
		if (!link_file_.write(port, password, obs_version, ws_version)) {
			server_.begin_stop();
			// OBS closing: unload() finishes the stop, see the top.
			if (closing_)
				return Opened;
			obs_log(LOG_WARNING, "Couldn't save the link file for DireBot, so DireBot Link is off.");
			server_.stop();
			return Failed;
		}
		obs_log(LOG_INFO, "DireBot Link is on (port %u, this PC only).", (unsigned)port);
		return Opened;
	}
};

Plugin *plugin = nullptr;

} // namespace

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "version %s loaded", PLUGIN_VERSION);
	return true;
}

// obs-websocket can load after this plugin, so it's looked for here.
void obs_module_post_load(void)
{
	plugin = new Plugin();
	plugin->post_load();
}

void obs_module_unload(void)
{
	if (plugin) {
		plugin->unload();
		delete plugin;
		plugin = nullptr;
	}
	obs_log(LOG_INFO, "unloaded");
}
