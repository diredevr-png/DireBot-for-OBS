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

// OBS's own output settings, shared by everything here that records or
// encodes beside OBS (the vertical picture, clean recordings): which
// encoder the wide stream uses and its settings, the sound tracks' bitrates,
// and where OBS puts recordings and how it names them.
//
// On OBS's UI thread (they read the profile's settings).

#pragma once

#include <string>

#include <obs.h>
#include <util/config-file.h>

namespace direbot::obs {

std::string text(config_t *config, const char *section, const char *name, const char *fallback = "");
bool simple_mode(config_t *config);
// The wide stream's encoder id; `simple` says which output mode OBS is in.
std::string main_encoder(config_t *config, bool &simple);
const char *audio_encoder_id();
int audio_kbps(config_t *config, int track);
// The wide stream's own encoder settings (OBS keeps them beside the
// profile in Advanced mode). Null when they're not there or are another
// encoder's.
obs_data_t *main_encoder_settings(config_t *config, const std::string &id);
void set_quality(obs_data_t *s, const std::string &id, int kbps, config_t *config);

// Where OBS's recordings go, from its own recording settings.
struct RecordPlace {
	std::string folder, format, muxer, name_format;
	bool no_space = false, overwrite = false;
};

RecordPlace record_place(config_t *config);
bool exists(const std::string &path);
// "2026-10-29 21-04-11 <suffix>.mp4" beside OBS's "2026-10-29 21-04-11.mp4",
// never over a file that's there unless OBS is set to. "" (and `problem`)
// when the folder isn't there.
std::string record_path(const RecordPlace &p, const std::string &ext, const std::string &suffix,
			std::string &problem);
std::string file_name(const std::string &path);

} // namespace direbot::obs
