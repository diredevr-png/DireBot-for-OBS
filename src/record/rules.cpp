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
#include <cmath>

namespace direbot::record {

namespace {

constexpr const char *QUALITIES[] = {"high", "medium", "small"};

int clamp_int(const json &j, const char *key, int fallback, int low, int high)
{
	if (!j.is_object() || !j.contains(key) || !j[key].is_number())
		return fallback;
	const double v = j[key].get<double>();
	if (!std::isfinite(v))
		return fallback;
	return (int)std::clamp<double>(v, low, high);
}

std::string str(const json &j, const char *key, const std::string &fallback = "")
{
	if (!j.is_object() || !j.contains(key) || !j[key].is_string())
		return fallback;
	return j[key].get<std::string>().substr(0, 256);
}

} // namespace

const char *when_id(When w)
{
	switch (w) {
	case When::Stream:
		return "stream";
	case When::Hand:
		return "hand";
	default:
		return "recording";
	}
}

When when_from(const std::string &id)
{
	if (id == "stream")
		return When::Stream;
	if (id == "hand")
		return When::Hand;
	return When::Recording;
}

json Options::to_json() const
{
	json rows = json::array();
	for (const Pick &p : picks)
		rows.push_back({{"source", p.source},
				{"uuid", p.uuid},
				{"height", p.height},
				{"quality", p.quality},
				{"audio_track", p.audio_track},
				{"enabled", p.enabled}});
	return {{"picks", rows}, {"when", when_id(when)}, {"encoder", encoder}};
}

Options Options::from_json(const json &j)
{
	Options o;
	if (!j.is_object())
		return o;
	o.when = when_from(str(j, "when", "recording"));
	o.encoder = str(j, "encoder", "auto");
	if (o.encoder.empty())
		o.encoder = "auto";
	if (j.contains("picks") && j["picks"].is_array()) {
		for (const json &row : j["picks"]) {
			if ((int)o.picks.size() >= MOST)
				break;
			Pick p;
			p.source = str(row, "source");
			if (p.source.empty())
				continue;
			bool twice = false;
			for (const Pick &other : o.picks)
				twice = twice || other.source == p.source;
			if (twice)
				continue;
			p.uuid = str(row, "uuid");
			p.height = clamp_int(row, "height", 0, 0, 4320);
			if (p.height && p.height < 144)
				p.height = 144;
			p.quality = str(row, "quality", "high");
			if (std::find(std::begin(QUALITIES), std::end(QUALITIES), p.quality) == std::end(QUALITIES))
				p.quality = "high";
			p.audio_track = clamp_int(row, "audio_track", 0, 0, 6);
			p.enabled = !(row.is_object() && row.contains("enabled") && row["enabled"].is_boolean() &&
				      !row["enabled"].get<bool>());
			o.picks.push_back(p);
		}
	}
	return o;
}

void fit(int source_w, int source_h, int height, int &out_w, int &out_h)
{
	source_w = std::max(2, source_w);
	source_h = std::max(2, source_h);
	if (height <= 0 || height >= source_h) {
		out_w = source_w;
		out_h = source_h;
	} else {
		out_h = height;
		out_w = (int)std::lround((double)source_w * height / source_h);
	}
	out_w = std::max(2, out_w & ~1);
	out_h = std::max(2, out_h & ~1);
}

int kbps_for(const std::string &quality, int width, int height, double fps)
{
	// Bits per pixel each frame: a clean camera or game picture for
	// editing later wants more than a stream.
	double bpp = 0.11;
	if (quality == "medium")
		bpp = 0.07;
	else if (quality == "small")
		bpp = 0.04;
	if (!std::isfinite(fps) || fps <= 0)
		fps = 30;
	const double kbps = (double)width * height * std::min(fps, 120.0) * bpp / 1000.0;
	return (int)std::clamp(kbps, 1000.0, 80000.0);
}

std::string file_suffix(const std::string &source)
{
	std::string out;
	for (unsigned char c : source) {
		if (c < 32 || std::string("<>:\"/\\|?*").find((char)c) != std::string::npos)
			out += ' ';
		else
			out += (char)c;
	}
	// No dots or spaces at the ends (Windows trims them), no long names.
	while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
		out.pop_back();
	size_t start = 0;
	while (start < out.size() && (out[start] == ' ' || out[start] == '.'))
		start++;
	out = out.substr(start, 60);
	while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
		out.pop_back();
	return out.empty() ? "Clean" : out;
}

std::string pick_encoder(const std::string &wanted, const std::vector<std::string> &available)
{
	auto has = [&](const std::string &id) {
		return std::find(available.begin(), available.end(), id) != available.end();
	};
	if (wanted != "auto" && has(wanted))
		return wanted;
	for (const char *id : {"obs_nvenc_h264_tex", "jim_nvenc", "h264_texture_amf", "obs_qsv11_v2", "obs_qsv11",
			       "com.apple.videotoolbox.videoencoder.ave.avc", "ffmpeg_nvenc"})
		if (has(id))
			return id;
	return "obs_x264";
}

} // namespace direbot::record
