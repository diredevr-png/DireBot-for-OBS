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

#include "options.hpp"

#include <algorithm>

#include "rules.hpp"

namespace direbot::vertical {

namespace {

constexpr int FORMAT = 1;

const char *const PLATFORMS[] = {"tiktok", "youtube", "instagram", "kick", "facebook", "x", "twitch", "other"};

std::string text(const json &j, const char *key, const std::string &fallback = "")
{
	auto it = j.find(key);
	return it != j.end() && it->is_string() ? it->get<std::string>() : fallback;
}

bool flag(const json &j, const char *key, bool fallback)
{
	auto it = j.find(key);
	return it != j.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

int number(const json &j, const char *key, int fallback, int low, int high)
{
	auto it = j.find(key);
	if (it == j.end() || !it->is_number())
		return fallback;
	const double v = it->get<double>();
	if (!(v == v))
		return fallback;
	return std::clamp((int)v, low, high);
}

std::string known_platform(const std::string &p)
{
	for (const char *name : PLATFORMS)
		if (p == name)
			return p;
	return "other";
}

} // namespace

json Options::to_file() const
{
	json list = json::array();
	for (const Destination &d : places)
		list.push_back({{"platform", d.platform},
				{"server", d.server},
				{"key", d.key_locked},
				{"enabled", d.enabled}});
	return {{"format", FORMAT},
		{"places", list},
		{"kbps", kbps},
		{"encoder", encoder},
		{"audio_track", audio_track},
		{"with_stream", with_stream},
		{"record_with_main", record_with_main}};
}

Options Options::from_file(const json &j)
{
	Options o;
	if (!j.is_object())
		return o;
	auto list = j.find("places");
	if (list != j.end() && list->is_array()) {
		for (size_t i = 0; i < list->size() && i < (size_t)SLOTS; i++) {
			const json &row = (*list)[i];
			if (!row.is_object())
				continue;
			Destination &d = o.places[i];
			d.platform = known_platform(text(row, "platform", "other"));
			d.server = text(row, "server");
			d.key_locked = text(row, "key");
			d.enabled = flag(row, "enabled", false);
		}
	}
	o.kbps = number(j, "kbps", o.kbps, rules::MIN_KBPS, rules::MAX_KBPS);
	o.encoder = text(j, "encoder", "auto");
	if (o.encoder.empty())
		o.encoder = "auto";
	o.audio_track = number(j, "audio_track", 1, 1, 6);
	o.with_stream = flag(j, "with_stream", false);
	o.record_with_main = flag(j, "record_with_main", false);
	return o;
}

json Options::to_public() const
{
	json list = json::array();
	for (int i = 0; i < SLOTS; i++) {
		const Destination &d = places[i];
		list.push_back({{"slot", i + 1},
				{"platform", d.platform},
				{"label", rules::platform_label(d.platform)},
				{"host", rules::host_of(d.server)},
				{"enabled", d.enabled},
				{"server_set", !d.server.empty()},
				{"server_ok", rules::server_ok(d.server)},
				{"key_set", !d.key_locked.empty()},
				{"output", rules::stream_output_name(i + 1)}});
	}
	return {{"places", list},
		{"kbps", kbps},
		{"encoder", encoder},
		{"audio_track", audio_track},
		{"with_stream", with_stream},
		{"record_with_main", record_with_main}};
}

bool Options::ready(int slot) const
{
	if (slot < 1 || slot > SLOTS)
		return false;
	const Destination &d = places[slot - 1];
	return d.enabled && rules::server_ok(d.server) && !d.key_locked.empty();
}

bool Options::any_ready() const
{
	for (int i = 1; i <= SLOTS; i++)
		if (ready(i))
			return true;
	return false;
}

} // namespace direbot::vertical
