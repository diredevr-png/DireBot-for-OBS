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

#include "setup.hpp"

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

namespace direbot::face {

using json = nlohmann::json;

namespace {

constexpr size_t MOST_CHAINS = 64, MOST_BONES = 64, MOST_UPRIGHT = 16;
constexpr float FAR = 1e6f;

bool number(const json &j, float &out)
{
	if (!j.is_number())
		return false;
	const double v = j.get<double>();
	if (!std::isfinite(v) || std::fabs(v) > FAR)
		return false;
	out = (float)v;
	return true;
}

bool vec_of(const json &j, size_t n, float *out)
{
	if (!j.is_array() || j.size() != n)
		return false;
	for (size_t i = 0; i < n; i++)
		if (!number(j[i], out[i]))
			return false;
	return true;
}

bool v3_of(const json &j, V3 &out)
{
	float v[3];
	if (!vec_of(j, 3, v))
		return false;
	out = {v[0], v[1], v[2]};
	return true;
}

float number_or(const json &j, const char *key, float def)
{
	float v = def;
	if (j.is_object() && j.contains(key))
		number(j[key], v);
	return v;
}

} // namespace

bool CharacterSetup::empty() const
{
	if (has_facing || has_pivot || chains_given || !upright.empty())
		return false;
	for (const SpotSetup &s : spots)
		if (s.given)
			return false;
	return true;
}

CharacterSetup setup_from(const std::string &text)
{
	CharacterSetup out;
	if (text.empty())
		return out;
	const json j = json::parse(text, nullptr, false);
	if (!j.is_object())
		return out;
	if (j.contains("facing")) {
		float q[4];
		if (vec_of(j["facing"], 4, q)) {
			const float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
			if (len > 1e-6f) {
				out.facing = {q[0] / len, q[1] / len, q[2] / len, q[3] / len};
				out.has_facing = true;
			}
		}
	}
	if (j.contains("pivot"))
		out.has_pivot = v3_of(j["pivot"], out.pivot);
	const char *names[3] = {"left_eye", "right_eye", "mouth"};
	const json spots = j.value("spots", json::object());
	for (int i = 0; i < 3 && spots.is_object(); i++) {
		if (!spots.contains(names[i]) || !spots[names[i]].is_object())
			continue;
		const json &s = spots[names[i]];
		SpotSetup &spot = out.spots[i];
		spot.w = number_or(s, "w", 0);
		spot.h = number_or(s, "h", 0);
		if (!(spot.w > 0) || !(spot.h > 0))
			continue;
		if (s.contains("at") && v3_of(s["at"], spot.at)) {
			spot.has_at = true;
		} else {
			spot.x = number_or(s, "x", -1);
			spot.y = number_or(s, "y", -1);
			if (spot.x < -0.5f || spot.x > 1.5f || spot.y < -0.5f || spot.y > 1.5f)
				continue;
			spot.w = std::min(spot.w, 2.0f);
			spot.h = std::min(spot.h, 2.0f);
		}
		spot.given = true;
	}
	if (j.contains("chains") && j["chains"].is_array()) {
		out.chains_given = true;
		for (const json &c : j["chains"]) {
			if (out.chains.size() >= MOST_CHAINS)
				break;
			if (!c.is_object() || !c.contains("bones") || !c["bones"].is_array())
				continue;
			ChainSetup chain;
			for (const json &b : c["bones"])
				if (b.is_string() && chain.bones.size() < MOST_BONES)
					chain.bones.push_back(b.get<std::string>());
			if (chain.bones.size() < 2)
				continue;
			if (c.contains("pull"))
				v3_of(c["pull"], chain.pull);
			chain.with_head = c.value("with_head", true);
			chain.both_ends = c.value("both_ends", false);
			out.chains.push_back(chain);
		}
	}
	if (j.contains("upright") && j["upright"].is_array())
		for (const json &u : j["upright"]) {
			if (out.upright.size() >= MOST_UPRIGHT)
				break;
			if (!u.is_object() || !u.contains("bone") || !u["bone"].is_string())
				continue;
			UprightSetup up;
			up.bone = u["bone"].get<std::string>();
			up.follow = std::min(1000.0f, std::max(0.0f, number_or(u, "follow", 5)));
			out.upright.push_back(up);
		}
	return out;
}

} // namespace direbot::face
