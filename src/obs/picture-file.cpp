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

#include "picture-file.hpp"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <obs-module.h>
#include <plugin-support.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <graphics/image-file.h>

// The old loader's layout comes from OBS's own header, while the add-on is
// built against an OBS that still has it (release builds: OBS 31.1).
#if LIBOBS_API_MAJOR_VER < 33
#define DIREBOT_OLD_PICTURES 1
#else
#define DIREBOT_OLD_PICTURES 0
#endif

namespace direbot {
namespace {

// OBS 33's struct gs_image_file_ex, field for field (libobs/graphics/
// image-file.h). Our own name, so it never clashes with OBS's header.
struct ImageEx {
	gs_texture_t *texture;
	enum gs_color_format format;
	uint32_t cx;
	uint32_t cy;
	bool is_animated_gif;
	bool frame_updated;
	bool loaded;
	void *internal;
	uint64_t cur_time;
	int cur_frame;
	int cur_loop;
	uint8_t *texture_data;
	uint64_t mem_usage;
	enum gs_image_alpha_mode alpha_mode;
	enum gs_color_space space;
};

#if LIBOBS_API_MAJOR_VER >= 33
// Built against OBS 33 or newer: check our copy against the real one.
static_assert(sizeof(ImageEx) == sizeof(gs_image_file_ex_t), "OBS changed gs_image_file_ex");
static_assert(offsetof(ImageEx, texture_data) == offsetof(gs_image_file_ex_t, texture_data),
	      "OBS changed gs_image_file_ex");
static_assert(offsetof(ImageEx, loaded) == offsetof(gs_image_file_ex_t, loaded), "OBS changed gs_image_file_ex");
#endif

using ExInit = void (*)(ImageEx *, const char *, enum gs_image_alpha_mode);
using ExFree = void (*)(ImageEx *);
using ExTick = bool (*)(ImageEx *, uint64_t);

#if DIREBOT_OLD_PICTURES
using Old = gs_image_file_t;
// The fields read from either layout without knowing which: the same place
// in both.
static_assert(offsetof(Old, texture) == offsetof(ImageEx, texture), "layouts differ");
static_assert(offsetof(Old, format) == offsetof(ImageEx, format), "layouts differ");
static_assert(offsetof(Old, cx) == offsetof(ImageEx, cx), "layouts differ");
static_assert(offsetof(Old, cy) == offsetof(ImageEx, cy), "layouts differ");
static_assert(offsetof(Old, is_animated_gif) == offsetof(ImageEx, is_animated_gif), "layouts differ");
static_assert(offsetof(Old, loaded) == offsetof(ImageEx, loaded), "layouts differ");
using OldInit = void (*)(Old *, const char *);
using OldFree = void (*)(Old *);
using OldTick = bool (*)(Old *, uint64_t);
#endif

enum class Kind { none, ex, old };

struct Loader {
	Kind kind = Kind::none;
	ExInit ex_init = nullptr;
	ExFree ex_free = nullptr, ex_init_texture = nullptr, ex_update_texture = nullptr;
	ExTick ex_tick = nullptr;
#if DIREBOT_OLD_PICTURES
	OldInit old_init = nullptr;
	OldFree old_free = nullptr, old_init_texture = nullptr, old_update_texture = nullptr;
	OldTick old_tick = nullptr;
#endif
};

void *find_in_obs(const char *name)
{
#ifdef _WIN32
	HMODULE obs = GetModuleHandleW(L"obs.dll");
	return obs ? reinterpret_cast<void *>(GetProcAddress(obs, name)) : nullptr;
#else
	return dlsym(RTLD_DEFAULT, name);
#endif
}

template<typename F> bool find(F &into, const char *name)
{
	into = reinterpret_cast<F>(find_in_obs(name));
	return into != nullptr;
}

const Loader &loader()
{
	static Loader l;
	static std::once_flag once;
	std::call_once(once, [] {
		Loader n;
		if (find(n.ex_init, "gs_image_file_ex_init") && find(n.ex_free, "gs_image_file_ex_free") &&
		    find(n.ex_init_texture, "gs_image_file_ex_init_texture") &&
		    find(n.ex_tick, "gs_image_file_ex_tick") &&
		    find(n.ex_update_texture, "gs_image_file_ex_update_texture")) {
			n.kind = Kind::ex;
		}
#if DIREBOT_OLD_PICTURES
		else if (find(n.old_init, "gs_image_file_init") && find(n.old_free, "gs_image_file_free") &&
			 find(n.old_init_texture, "gs_image_file_init_texture") &&
			 find(n.old_tick, "gs_image_file_tick") &&
			 find(n.old_update_texture, "gs_image_file_update_texture")) {
			n.kind = Kind::old;
		}
#endif
		if (n.kind == Kind::none)
			obs_log(LOG_WARNING, "This OBS has no picture loader the add-on knows, so pictures won't open "
					     "(the add-on may need an update for this OBS)");
		l = n;
	});
	return l;
}

// Room for whichever layout this OBS uses.
#if DIREBOT_OLD_PICTURES
constexpr size_t STORAGE = sizeof(Old) > sizeof(ImageEx) ? sizeof(Old) : sizeof(ImageEx);
#else
constexpr size_t STORAGE = sizeof(ImageEx);
#endif

} // namespace

#define EX reinterpret_cast<ImageEx *>(storage)
#define CEX reinterpret_cast<const ImageEx *>(storage)
#define OLD reinterpret_cast<Old *>(storage)
#define COLD reinterpret_cast<const Old *>(storage)

PictureFile::PictureFile() : storage(std::calloc(1, STORAGE)) {}

// Doesn't let go of the texture (that needs the graphics thread): free()
// first.
PictureFile::~PictureFile()
{
	std::free(storage);
}

void PictureFile::open(const char *path)
{
	if (!storage)
		return;
	const Loader &l = loader();
	if (l.kind == Kind::ex)
		l.ex_init(EX, path, GS_IMAGE_ALPHA_STRAIGHT);
#if DIREBOT_OLD_PICTURES
	else if (l.kind == Kind::old)
		l.old_init(OLD, path);
#endif
}

void PictureFile::init_texture()
{
	if (!storage)
		return;
	const Loader &l = loader();
	if (l.kind == Kind::ex)
		l.ex_init_texture(EX);
#if DIREBOT_OLD_PICTURES
	else if (l.kind == Kind::old)
		l.old_init_texture(OLD);
#endif
}

void PictureFile::free()
{
	if (!storage)
		return;
	const Loader &l = loader();
	if (l.kind == Kind::ex)
		l.ex_free(EX);
#if DIREBOT_OLD_PICTURES
	else if (l.kind == Kind::old)
		l.old_free(OLD);
#endif
	std::memset(storage, 0, STORAGE);
}

bool PictureFile::tick(uint64_t ns)
{
	if (!storage)
		return false;
	const Loader &l = loader();
	if (l.kind == Kind::ex)
		return l.ex_tick(EX, ns);
#if DIREBOT_OLD_PICTURES
	if (l.kind == Kind::old)
		return l.old_tick(OLD, ns);
#endif
	return false;
}

void PictureFile::update_texture()
{
	if (!storage)
		return;
	const Loader &l = loader();
	if (l.kind == Kind::ex)
		l.ex_update_texture(EX);
#if DIREBOT_OLD_PICTURES
	else if (l.kind == Kind::old)
		l.old_update_texture(OLD);
#endif
}

// The first fields are the same in both layouts; texture_data isn't.
// Nothing else is read before this says yes.
bool PictureFile::loaded() const
{
	return storage && CEX->loaded;
}
bool PictureFile::is_animated_gif() const
{
	return CEX->is_animated_gif;
}
gs_texture_t *PictureFile::texture() const
{
	return CEX->texture;
}
gs_color_format PictureFile::format() const
{
	return CEX->format;
}
uint32_t PictureFile::cx() const
{
	return CEX->cx;
}
uint32_t PictureFile::cy() const
{
	return CEX->cy;
}
const uint8_t *PictureFile::texture_data() const
{
	if (!storage)
		return nullptr;
#if DIREBOT_OLD_PICTURES
	if (loader().kind == Kind::old)
		return COLD->texture_data;
#endif
	return CEX->texture_data;
}

const char *PictureFile::which()
{
	switch (loader().kind) {
	case Kind::ex:
		return "new";
	case Kind::old:
		return "old";
	default:
		return "none";
	}
}

} // namespace direbot
