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

// A file's path as OBS and DireBot give it (UTF-8) for C++'s own file
// reading. On Windows that takes a plain string in the PC's old code
// page, so a folder or file named in other letters ("José", "Лиса") would
// be another file, or none.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace direbot::face {

inline std::filesystem::path utf8_path(const std::string &path)
{
	return std::filesystem::u8path(path);
}

// A file's size and when it last changed, to see when it's changed (""
// for no file there).
inline std::string file_stamp(const std::string &path)
{
	if (path.empty())
		return "";
	std::error_code ec;
	const std::filesystem::path p = utf8_path(path);
	const std::uintmax_t size = std::filesystem::file_size(p, ec);
	if (ec)
		return "";
	const auto when = std::filesystem::last_write_time(p, ec);
	if (ec)
		return "";
	return std::to_string(size) + "@" + std::to_string((long long)when.time_since_epoch().count());
}

} // namespace direbot::face
