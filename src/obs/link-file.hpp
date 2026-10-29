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

// How DireBot finds the link: a small file in OBS's settings folder for
// this plugin, in the Windows user's own AppData, so only that user (and
// the PC's administrators) can read it - the same place OBS keeps its
// websocket password.
//
//   %APPDATA%\obs-studio\plugin_config\direbot-for-obs\link.json
//   {"format": 1, "port": 4466, "password": "...", "pid": 1234,
//    "obs": "32.0.2", "websocket": "5.6.3", "plugin": "0.1.0", "started": 1798000000}
//
// The password is new every time OBS starts, and the file goes when OBS
// closes. "pid" is OBS's process, so a file left behind by a crash can be
// told from a live one.

#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace direbot::obs {

class LinkFile {
public:
	bool write(uint16_t port, const std::string &password, const std::string &obs_version,
		   const std::string &websocket_version);
	// OBS is closing: the file goes, and isn't written again.
	void remove();

private:
	std::mutex lock_;
	std::string path_;
	bool closed_ = false;
};

} // namespace direbot::obs
