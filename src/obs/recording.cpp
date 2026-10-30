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

#include "recording.hpp"

#include <algorithm>

#include <obs-frontend-api.h>
#include <util/platform.h>

namespace direbot::obs {

std::string text(config_t *config, const char *section, const char *name, const char *fallback)
{
	const char *v = config ? config_get_string(config, section, name) : nullptr;
	return v && *v ? v : fallback;
}

bool simple_mode(config_t *config)
{
	return text(config, "Output", "Mode", "Simple") != "Advanced";
}

std::string main_encoder(config_t *config, bool &simple)
{
	simple = simple_mode(config);
	return simple ? text(config, "SimpleOutput", "StreamEncoder", "x264") : text(config, "AdvOut", "Encoder", "obs_x264");
}

const char *audio_encoder_id()
{
	if (obs_get_encoder_codec("ffmpeg_aac"))
		return "ffmpeg_aac";
	if (obs_get_encoder_codec("CoreAudio_AAC"))
		return "CoreAudio_AAC";
	return "ffmpeg_aac";
}

int audio_kbps(config_t *config, int track)
{
	int kbps = 160;
	if (config) {
		if (simple_mode(config)) {
			kbps = (int)config_get_uint(config, "SimpleOutput", "ABitrate");
		} else {
			const std::string key = "Track" + std::to_string(track) + "Bitrate";
			kbps = (int)config_get_uint(config, "AdvOut", key.c_str());
		}
	}
	return kbps > 0 ? std::clamp(kbps, 64, 320) : 160;
}

obs_data_t *main_encoder_settings(config_t *config, const std::string &id)
{
	if (!config || simple_mode(config) || text(config, "AdvOut", "Encoder") != id)
		return nullptr;
	char *dir = obs_frontend_get_current_profile_path();
	if (!dir)
		return nullptr;
	const std::string path = std::string(dir) + "/streamEncoder.json";
	bfree(dir);
	return obs_data_create_from_json_file_safe(path.c_str(), "bak");
}

void set_quality(obs_data_t *s, const std::string &id, int kbps, config_t *config)
{
	obs_data_set_string(s, "rate_control", "CBR");
	obs_data_set_int(s, "bitrate", kbps);
	obs_data_set_int(s, "keyint_sec", 2);
	if (id == "obs_x264" && !obs_data_has_user_value(s, "preset"))
		obs_data_set_string(s, "preset", text(config, "SimpleOutput", "Preset", "veryfast").c_str());
}

RecordPlace record_place(config_t *config)
{
	RecordPlace p;
	p.name_format = text(config, "Output", "FilenameFormatting", "%CCYY-%MM-%DD %hh-%mm-%ss");
	p.overwrite = config && config_get_bool(config, "Output", "OverwriteIfExists");
	if (simple_mode(config)) {
		p.folder = text(config, "SimpleOutput", "FilePath");
		p.format = text(config, "SimpleOutput", "RecFormat2", "hybrid_mp4");
		p.muxer = text(config, "SimpleOutput", "MuxerCustom");
		p.no_space = config && config_get_bool(config, "SimpleOutput", "FileNameWithoutSpace");
	} else if (text(config, "AdvOut", "RecType", "Standard") == "FFmpeg") {
		// A custom FFmpeg recording: its folder, in a file type the tall
		// picture's encoder can go in.
		p.folder = text(config, "AdvOut", "FFFilePath");
		p.format = "mkv";
		p.no_space = config && config_get_bool(config, "AdvOut", "FFFileNameWithoutSpace");
	} else {
		p.folder = text(config, "AdvOut", "RecFilePath");
		p.format = text(config, "AdvOut", "RecFormat2", "hybrid_mp4");
		p.muxer = text(config, "AdvOut", "RecMuxerCustom");
		p.no_space = config && config_get_bool(config, "AdvOut", "RecFileNameWithoutSpace");
	}
	return p;
}

bool exists(const std::string &path)
{
	return os_file_exists(path.c_str());
}

std::string record_path(const RecordPlace &p, const std::string &ext, const std::string &suffix,
			std::string &problem)
{
	os_dir_t *dir = p.folder.empty() ? nullptr : os_opendir(p.folder.c_str());
	if (!dir) {
		problem = "OBS's recording folder isn't there. Pick one in OBS → Settings → Output.";
		return "";
	}
	os_closedir(dir);

	std::string folder = p.folder;
	if (folder.back() != '/' && folder.back() != '\\')
		folder += "/";
	const std::string format = p.name_format + " " + suffix;
	char *name = os_generate_formatted_filename(ext.c_str(), !p.no_space, format.c_str());
	std::string path = folder + (name ? name : suffix + "." + ext);
	bfree(name);

	// A name format can have folders in it ("%CCYY/%MM/...").
	const size_t slash = path.find_last_of("/\\");
	if (slash != std::string::npos)
		os_mkdirs(path.substr(0, slash).c_str());

	if (!p.overwrite && exists(path)) {
		const std::string stem = path.substr(0, path.size() - ext.size() - 1);
		for (int n = 2; n < 1000; n++) {
			const std::string next =
				stem + (p.no_space ? "_" + std::to_string(n) : " (" + std::to_string(n) + ")") + "." + ext;
			if (!exists(next)) {
				path = next;
				break;
			}
		}
	}
	return path;
}

std::string file_name(const std::string &path)
{
	const size_t slash = path.find_last_of("/\\");
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace direbot::obs
