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

// The lights on a 3D character (avatar3d.cpp): a key light, a second lamp
// from the other side, the room's light from above and below, and a rim
// light round its edge from behind - as you choose them (their way,
// colour and brightness), or a ready-made set: the studio (the look it
// always had), daylight, cozy and warm, night (horror blue), neon. They're
// where your camera is, so every camera view is lit the same way.
//
// Plain numbers, no OBS (tests/unit_model.cpp checks them).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "math3d.hpp"

namespace direbot::face {

// In the camera's view (x right, y up, towards you); colours linear,
// times brightness.
struct Lights {
	V3 key_dir{-0.45f, 0.6f, 0.66f}; // towards the key light (normalised when drawn)
	V3 key{1.05f, 1.029f, 0.9975f};
	V3 fill_dir{0.7f, 0.15f, 0.7f};
	V3 fill{0.2475f, 0.261f, 0.2925f};
	V3 room_up{0.42f, 0.43f, 0.46f}, room_down{0.16f, 0.15f, 0.15f};
	V3 rim{0, 0, 0};
	float rim_edge = 3; // how close to the edge it stays (higher: closer)
};

// The lights in plain terms, as the page has them: where the key light
// shines from (degrees: `side` round from straight in front, - the
// picture's left; `height` up from level), its colour and brightness
// (100: the studio's), the room's, and the rim light's (0 none).
// Colours are 0xRRGGBB (sRGB).
struct LightTerms {
	int side = -34, height = 37;
	uint32_t colour = 0xFFFDF9;
	int brightness = 100;
	uint32_t room_colour = 0xF5F7FF;
	int room = 105;
	uint32_t rim_colour = 0xFFFFFF;
	int rim = 0;
};

enum LightPreset { LIGHTS_STUDIO, LIGHTS_DAYLIGHT, LIGHTS_COZY, LIGHTS_NIGHT, LIGHTS_NEON, LIGHTS_OWN, LIGHT_PRESETS };
inline const char *const LIGHT_PRESET_NAMES[LIGHT_PRESETS] = {"studio", "daylight", "cozy", "night", "neon", "own"};

inline int light_preset_of(const std::string &name)
{
	for (int i = 0; i < LIGHT_PRESETS; i++)
		if (name == LIGHT_PRESET_NAMES[i])
			return i;
	return LIGHTS_STUDIO;
}

// Each ready-made set in plain terms (the studio's close to its own,
// exact lights: preset_lights).
inline LightTerms preset_terms(int preset)
{
	LightTerms t;
	switch (preset) {
	case LIGHTS_DAYLIGHT: // the sun high on the right, a blue sky
		t = {25, 50, 0xFFF4E0, 110, 0xCFE0FF, 110, 0xFFFFFF, 25};
		break;
	case LIGHTS_COZY: // a warm lamp low on the left, a warm dim room
		t = {-50, 20, 0xFFB066, 95, 0xFFC48A, 55, 0xFF9A40, 45};
		break;
	case LIGHTS_NIGHT: // moonlight from above, a dark blue room, a cold edge
		t = {40, 55, 0x7FA0FF, 70, 0x3A5AA0, 30, 0x5FB0FF, 70};
		break;
	case LIGHTS_NEON: // pink from the left, purple from the right, a cyan edge
		t = {-60, 15, 0xFF4FD8, 100, 0x7A3CFF, 45, 0x30E0FF, 90};
		break;
	default:
		break;
	}
	return t;
}

inline float light_linear(uint32_t rgb, int shift)
{
	const float c = (float)((rgb >> shift) & 0xFF) / 255.0f;
	return std::pow(c, 2.2f);
}
inline V3 light_colour(uint32_t rgb)
{
	return {light_linear(rgb, 16), light_linear(rgb, 8), light_linear(rgb, 0)};
}

// The lights from plain terms.
inline Lights lights_of(const LightTerms &t)
{
	constexpr float RAD = 3.14159265f / 180;
	const float side = (float)std::clamp(t.side, -180, 180) * RAD,
		    height = (float)std::clamp(t.height, -90, 90) * RAD;
	const float bright = std::clamp(t.brightness, 0, 300) / 100.0f, room = std::clamp(t.room, 0, 300) / 100.0f,
		    rim = std::clamp(t.rim, 0, 300) / 100.0f;
	Lights l;
	l.key_dir = {std::sin(side) * std::cos(height), std::sin(height), std::cos(side) * std::cos(height)};
	l.key = light_colour(t.colour) * (bright * 1.05f);
	// The second lamp: lower, from the other side, the room's colour a
	// little cooler (as the sky's); the room's light from below a little
	// warmer (off the floor) - as the studio has them.
	l.fill_dir = {t.side <= 0 ? 0.7f : -0.7f, 0.15f, 0.7f};
	const V3 rc = light_colour(t.room_colour);
	const V3 cooler{0.953f, 0.987f, 1.032f}, warmer{1.04f, 0.96f, 0.89f};
	auto times = [](V3 a, V3 b) { return V3{a.x * b.x, a.y * b.y, a.z * b.z}; };
	l.fill = times(rc, cooler) * (room * 0.27f);
	l.room_up = rc * (room * 0.44f);
	l.room_down = times(rc, warmer) * (room * 0.16f);
	l.rim = light_colour(t.rim_colour) * (rim * 0.8f);
	return l;
}

// A ready-made set's lights: the studio exactly as it always was.
inline Lights preset_lights(int preset)
{
	if (preset == LIGHTS_STUDIO || preset < 0 || preset >= LIGHT_PRESETS)
		return Lights();
	return lights_of(preset_terms(preset));
}

// How a face turned to the camera is lit by these lights, each colour as
// a share of how the studio lights it (1: as the studio) - what your
// camera's real eyes and mouth drawn on a character take, so they sit in
// its light rather than the camera's.
inline V3 front_light(const Lights &l)
{
	auto lit = [](const Lights &x) {
		auto facing = [](V3 d) {
			const float n = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
			return n > 0 ? std::max(0.0f, d.z / n) : 0.0f;
		};
		return x.key * facing(x.key_dir) + x.fill * facing(x.fill_dir) + (x.room_up + x.room_down) * 0.5f;
	};
	const V3 a = lit(l), s = lit(Lights());
	auto share = [](float v, float of) { return std::clamp(of > 0 ? v / of : 1.0f, 0.0f, 3.0f); };
	return {share(a.x, s.x), share(a.y, s.y), share(a.z, s.z)};
}

// "#rrggbb" and back (false: not a colour).
inline std::string colour_text(uint32_t rgb)
{
	static const char *hex = "0123456789abcdef";
	std::string s = "#";
	for (int shift = 20; shift >= 0; shift -= 4)
		s += hex[(rgb >> shift) & 0xF];
	return s;
}

} // namespace direbot::face
