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

#include "rules.hpp"

#include <algorithm>
#include <cctype>

namespace direbot::vertical::rules {

namespace {

std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

bool contains(const std::string &haystack, const char *needle)
{
	return haystack.find(needle) != std::string::npos;
}

bool starts(const std::string &s, const char *prefix)
{
	return s.rfind(prefix, 0) == 0;
}

} // namespace

std::string stream_output_name(int slot)
{
	return std::string(OUTPUT_PREFIX) + "stream_" + std::to_string(slot);
}

bool size_ok(uint32_t width, uint32_t height)
{
	return width >= 240 && height >= 240 && width <= 4096 && height <= 4096 && width % 2 == 0 &&
	       height % 2 == 0 && width <= height;
}

std::string host_of(const std::string &server)
{
	std::string s = server;
	const size_t scheme = s.find("://");
	if (scheme != std::string::npos)
		s = s.substr(scheme + 3);
	const size_t end = s.find_first_of("/?#");
	if (end != std::string::npos)
		s = s.substr(0, end);
	// No "user:password@" in front.
	const size_t at = s.rfind('@');
	if (at != std::string::npos)
		s = s.substr(at + 1);
	// No port.
	if (!s.empty() && s[0] == '[') {
		const size_t close = s.find(']');
		s = close == std::string::npos ? std::string() : s.substr(0, close + 1);
	} else {
		const size_t colon = s.find(':');
		if (colon != std::string::npos)
			s = s.substr(0, colon);
	}
	return lower(s);
}

std::string platform_label(const std::string &platform)
{
	static const std::map<std::string, std::string> names = {
		{"tiktok", "TikTok"},     {"youtube", "YouTube"}, {"instagram", "Instagram"}, {"kick", "Kick"},
		{"facebook", "Facebook"}, {"twitch", "Twitch"},   {"x", "X"},
	};
	auto it = names.find(platform);
	return it == names.end() ? "Another site" : it->second;
}

std::string usual_server(const std::string &platform)
{
	if (platform == "youtube")
		return "rtmp://a.rtmp.youtube.com/live2";
	if (platform == "facebook")
		return "rtmps://live-api-s.facebook.com:443/rtmp/";
	if (platform == "twitch")
		return "rtmp://live.twitch.tv/app";
	return "";
}

bool server_ok(const std::string &server)
{
	const std::string s = lower(server);
	if (!starts(s, "rtmp://") && !starts(s, "rtmps://"))
		return false;
	return !host_of(server).empty();
}

std::string follow_pick(const std::string &main_scene, const std::vector<std::string> &tall_scenes,
			const std::map<std::string, std::string> &links)
{
	auto have = [&](const std::string &name) {
		return std::find(tall_scenes.begin(), tall_scenes.end(), name) != tall_scenes.end();
	};
	if (main_scene.empty())
		return "";
	auto it = links.find(main_scene);
	if (it != links.end()) {
		// Picked as "stay as it is" ("") or a scene that's gone: nothing.
		return have(it->second) ? it->second : "";
	}
	for (const std::string &name :
	     {main_scene, std::string(BUILT_PREFIX) + main_scene, main_scene + " (Vertical)"})
		if (have(name))
			return name;
	return "";
}

std::string pick_encoder(const std::string &wanted, bool simple, const std::string &main,
			 const std::set<std::string> &available)
{
	auto ok = [&](const std::string &id) {
		return !id.empty() && available.count(id) > 0;
	};
	if (wanted != "auto" && !wanted.empty() && ok(wanted))
		return wanted;

	// The wide stream's encoder, by its maker.
	std::string kind = lower(main);
	if (!simple) {
		if (contains(kind, "nvenc"))
			kind = "nvenc";
		else if (contains(kind, "amf"))
			kind = "amd";
		else if (contains(kind, "qsv"))
			kind = "qsv";
		else if (contains(kind, "videotoolbox"))
			kind = "apple";
		else
			kind = "x264";
	} else if (starts(kind, "nvenc")) {
		kind = "nvenc";
	} else if (starts(kind, "amd")) {
		kind = "amd";
	} else if (starts(kind, "qsv")) {
		kind = "qsv";
	} else if (starts(kind, "apple")) {
		kind = "apple";
	} else {
		kind = "x264";
	}

	std::vector<std::string> choices;
	if (kind == "nvenc")
		choices = {"obs_nvenc_h264_tex", "jim_nvenc", "ffmpeg_nvenc"};
	else if (kind == "amd")
		choices = {"h264_texture_amf", "h264_fallback_amf"};
	else if (kind == "qsv")
		choices = {"obs_qsv11_v2", "obs_qsv11"};
	else if (kind == "apple")
		choices = {"com.apple.videotoolbox.videoencoder.ave.avc",
			   "com.apple.videotoolbox.videoencoder.h264.gva"};
	for (const std::string &id : choices)
		if (ok(id))
			return id;
	return "obs_x264";
}

std::string recording_extension(const std::string &format)
{
	if (format == "fragmented_mp4" || format == "hybrid_mp4")
		return "mp4";
	if (format == "fragmented_mov" || format == "hybrid_mov")
		return "mov";
	if (format == "mpegts")
		return "ts";
	if (format == "mkv" || format == "mp4" || format == "mov" || format == "flv")
		return format;
	return "mkv";
}

std::string recording_output(const std::string &format)
{
	if (format == "hybrid_mp4")
		return "mp4_output";
	if (format == "hybrid_mov")
		return "mov_output";
	return "ffmpeg_muxer";
}

std::string recording_muxer_settings(const std::string &format)
{
	if (starts(format, "fragmented"))
		return "movflags=frag_keyframe+empty_moov+delay_moov";
	return "";
}

} // namespace direbot::vertical::rules
