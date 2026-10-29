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

#include "link-file.hpp"

#include <ctime>

#include <obs-module.h>
#include <util/platform.h>

#include <plugin-support.h>

#include "../link/protocol.hpp"

#ifdef _WIN32
#include <process.h>
#define current_pid() ((long long)_getpid())
#else
#include <unistd.h>
#define current_pid() ((long long)getpid())
#endif

namespace direbot::obs {

using link::json;

bool LinkFile::write(uint16_t port, const std::string &password, const std::string &obs_version,
		     const std::string &websocket_version)
{
	std::lock_guard<std::mutex> l(lock_);
	if (closed_)
		return false;
	if (path_.empty()) {
		char *folder = obs_module_config_path("");
		char *file = obs_module_config_path("link.json");
		if (folder && file) {
			os_mkdirs(folder);
			path_ = file;
		}
		bfree(folder);
		bfree(file);
		if (path_.empty())
			return false;
	}
	const std::string body = json({{"format", 1},
				       {"port", port},
				       {"password", password},
				       {"pid", current_pid()},
				       {"obs", obs_version},
				       {"websocket", websocket_version},
				       {"plugin", PLUGIN_VERSION},
				       {"started", (long long)std::time(nullptr)}})
					 .dump(1);
	// Written to link.json.tmp, then swapped in: DireBot never reads half
	// a file. No backup copy is kept.
	return os_quick_write_utf8_file_safe(path_.c_str(), body.c_str(), body.size(), false, "tmp", nullptr);
}

void LinkFile::remove()
{
	std::lock_guard<std::mutex> l(lock_);
	closed_ = true;
	if (!path_.empty())
		os_unlink(path_.c_str());
}

} // namespace direbot::obs
