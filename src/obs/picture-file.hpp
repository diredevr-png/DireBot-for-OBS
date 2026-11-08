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

// OBS's own picture loader (PNG, JPG, animated GIF...), for any version of
// OBS the add-on runs in.
//
// OBS 33 took the old loader out of OBS (gs_image_file_*) and put a new one
// in (gs_image_file_ex_*, with a different layout). Windows won't load an
// add-on at all when one name it links to is missing, so the add-on links
// to neither: it looks up whichever this OBS has when a picture is first
// opened. With neither, pictures just don't open (and the log says why);
// everything else in the add-on keeps working.

#pragma once

#include <cstdint>
#include <string>

#include <graphics/graphics.h>

namespace direbot {

class PictureFile {
public:
	PictureFile();
	~PictureFile();
	PictureFile(const PictureFile &) = delete;
	PictureFile &operator=(const PictureFile &) = delete;

	// Reads the file (any thread but the graphics one; straight alpha).
	void open(const char *path);
	// Graphics thread (inside obs_enter_graphics): makes its texture, and
	// lets go of the points for a still picture.
	void init_texture();
	// Graphics thread: lets go of everything.
	void free();
	// Graphics thread: an animated GIF on `ns`; true when its picture moved
	// on (then update_texture()).
	bool tick(uint64_t ns);
	void update_texture();

	bool loaded() const;
	bool is_animated_gif() const;
	gs_texture_t *texture() const;
	gs_color_format format() const;
	uint32_t cx() const;
	uint32_t cy() const;
	// The points (4 bytes each) until init_texture(); null after that for a
	// still picture.
	const uint8_t *texture_data() const;

	// Which loader this OBS has: "new" (OBS 33 and newer), "old" (OBS 32
	// and older) or "none".
	static const char *which();

private:
	// Either loader's picture (the old one is about 40 KB, so not inside
	// this object).
	void *storage;
};

} // namespace direbot
