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
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#define current_pid() ((long long)getpid())
#endif

namespace direbot::obs {

using link::json;

namespace {

#ifdef _WIN32
// The user's own AppData already keeps everyone else out.
bool write_private(const std::string &path, const std::string &body)
{
	return os_quick_write_utf8_file_safe(path.c_str(), body.c_str(), body.size(), false, "tmp", nullptr);
}
#else
// Elsewhere OBS's settings folder is readable by everyone (0755, and files
// 0644 under the usual umask), so the file is made 0600 from the start -
// never readable by anyone else, not even for a moment - then swapped in.
bool write_private(const std::string &path, const std::string &body)
{
	const std::string tmp = path + ".tmp";
	::unlink(tmp.c_str());
	int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0)
		return false;
	size_t done = 0;
	while (done < body.size()) {
		ssize_t n = ::write(fd, body.data() + done, body.size() - done);
		if (n <= 0) {
			::close(fd);
			::unlink(tmp.c_str());
			return false;
		}
		done += (size_t)n;
	}
	if (::fsync(fd) != 0 || ::close(fd) != 0 || std::rename(tmp.c_str(), path.c_str()) != 0) {
		::unlink(tmp.c_str());
		return false;
	}
	return true;
}
#endif

} // namespace

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
#ifndef _WIN32
			::chmod(folder, 0700); // the plugin's own folder: nobody else's business
#endif
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
	return write_private(path_, body);
}

void LinkFile::remove()
{
	std::lock_guard<std::mutex> l(lock_);
	closed_ = true;
	if (!path_.empty())
		os_unlink(path_.c_str());
}

} // namespace direbot::obs
