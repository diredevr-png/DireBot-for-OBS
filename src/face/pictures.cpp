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

#include "pictures.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace direbot::face {

namespace {

uint32_t next_power(uint32_t v)
{
	uint32_t p = 1;
	while (p < v && p < MOST_PICTURE)
		p <<= 1;
	return p;
}

// A colour's byte as light, the way the effect reads it (its lin(): a
// gamma of 2.2), and back: the byte whose light is nearest - the same
// as rounding the effect's srgb() - from the light half way between
// each byte and the next (looked up near, then made exact).
struct Light {
	static constexpr int STEPS = 65536;
	std::array<float, 256> of{};
	std::array<float, 255> between{};
	std::vector<uint8_t> near;
	Light() : near(STEPS)
	{
		for (int i = 0; i < 256; i++)
			of[i] = std::pow(i / 255.0f, 2.2f);
		for (int i = 0; i < 255; i++)
			between[i] = std::pow((i + 0.5f) / 255.0f, 2.2f);
		for (int i = 0; i < STEPS; i++)
			near[i] = exact((float)i / (STEPS - 1), i ? near[i - 1] : 0);
	}
	uint8_t exact(float light, int from) const
	{
		int b = from;
		while (b < 255 && light >= between[b])
			b++;
		while (b > 0 && light < between[b - 1])
			b--;
		return (uint8_t)b;
	}
	uint8_t byte(float light) const
	{
		if (!(light > 0))
			return 0;
		if (light >= 1)
			return 255;
		return exact(light, near[(int)(light * (STEPS - 1))]);
	}
};
const Light &light()
{
	static const Light l;
	return l;
}

uint8_t rounded(float v)
{
	return (uint8_t)std::lround(std::min(255.0f, std::max(0.0f, v)));
}

// Pixels mixed, each with its share. A colour picture's colours as light
// and weighted by how solid they are, so a see-through pixel's colour
// counts for nothing; a data picture's as the numbers they are (its
// fourth number isn't how solid it is).
struct Mix {
	bool colour;
	float c[3] = {0, 0, 0}, plain[3] = {0, 0, 0}, a = 0, n = 0;
	explicit Mix(bool colour_) : colour(colour_) {}
	void add(const uint8_t *p, float share)
	{
		const float pa = p[3] / 255.0f * share, weight = colour ? pa : share;
		for (int k = 0; k < 3; k++) {
			const float v = colour ? light().of[p[k]] : (float)p[k];
			c[k] += v * weight;
			plain[k] += v * share;
		}
		a += pa;
		n += share;
	}
	void put(uint8_t *o) const
	{
		const float weights = colour ? a : n;
		for (int k = 0; k < 3; k++) {
			// Wholly see-through: its colours plainly averaged.
			const float v = weights > 1e-6f ? c[k] / weights : n > 0 ? plain[k] / n : 0;
			o[k] = colour ? light().byte(v) : rounded(v);
		}
		o[3] = rounded(n > 0 ? a / n * 255 : 0);
	}
};

// Half the size: each pixel the mix of 2 x 2.
std::vector<uint8_t> halved(const std::vector<uint8_t> &px, uint32_t w, uint32_t h, bool colour, uint32_t &hw,
			    uint32_t &hh)
{
	hw = std::max(1u, w / 2);
	hh = std::max(1u, h / 2);
	std::vector<uint8_t> out((size_t)hw * hh * 4);
	for (uint32_t y = 0; y < hh; y++)
		for (uint32_t x = 0; x < hw; x++) {
			Mix m(colour);
			for (uint32_t dy = 0; dy < 2; dy++)
				for (uint32_t dx = 0; dx < 2; dx++) {
					const uint32_t sx = std::min(w - 1, x * 2 + dx),
						       sy = std::min(h - 1, y * 2 + dy);
					m.add(&px[((size_t)sy * w + sx) * 4], 1);
				}
			m.put(&out[((size_t)y * hw + x) * 4]);
		}
	return out;
}

// Square, `to` across: halved first while it's over twice the size, then
// smoothly read (a mix of the 4 nearest).
std::vector<uint8_t> resized(const uint8_t *px, uint32_t w, uint32_t h, uint32_t to, bool colour)
{
	std::vector<uint8_t> cur(px, px + (size_t)w * h * 4);
	while (w >= to * 2 && h >= to * 2) {
		uint32_t nw, nh;
		cur = halved(cur, w, h, colour, nw, nh);
		w = nw;
		h = nh;
	}
	if (w == to && h == to)
		return cur;
	std::vector<uint8_t> out((size_t)to * to * 4);
	for (uint32_t y = 0; y < to; y++) {
		const float fy = std::max(0.0f, (y + 0.5f) * h / to - 0.5f);
		const uint32_t y0 = std::min(h - 1, (uint32_t)fy), y1 = std::min(h - 1, y0 + 1);
		const float ty = fy - y0;
		for (uint32_t x = 0; x < to; x++) {
			const float fx = std::max(0.0f, (x + 0.5f) * w / to - 0.5f);
			const uint32_t x0 = std::min(w - 1, (uint32_t)fx), x1 = std::min(w - 1, x0 + 1);
			const float tx = fx - x0;
			auto at = [&](uint32_t xx, uint32_t yy) { return &cur[((size_t)yy * w + xx) * 4]; };
			Mix m(colour);
			m.add(at(x0, y0), (1 - tx) * (1 - ty));
			m.add(at(x1, y0), tx * (1 - ty));
			m.add(at(x0, y1), (1 - tx) * ty);
			m.add(at(x1, y1), tx * ty);
			m.put(&out[((size_t)y * to + x) * 4]);
		}
	}
	return out;
}

// A picture of only a few colours: a low-poly model's palette (often a
// strip of one pixel for each colour, and not a power of two across) or
// flat patches. A painted picture has far more.
constexpr size_t FEW_COLOURS = 64;
// Made bigger, such a picture is copied pixel for pixel, at least this
// many times across (up to SHARP_MOST): the graphics card's smooth read
// then lands wholly inside each colour, so a swatch's middle is exactly
// its colour, never part of the one beside it.
constexpr uint32_t SHARP_TIMES = 8;
constexpr uint32_t SHARP_MOST = 512;

bool few_colours(const uint8_t *px, uint32_t w, uint32_t h)
{
	std::vector<uint32_t> seen; // kept in order
	uint32_t last = 0;
	for (size_t i = 0; i < (size_t)w * h; i++) {
		uint32_t c;
		std::memcpy(&c, px + i * 4, 4);
		if (i && c == last)
			continue;
		last = c;
		const auto at = std::lower_bound(seen.begin(), seen.end(), c);
		if (at != seen.end() && *at == c)
			continue;
		if (seen.size() == FEW_COLOURS)
			return false;
		seen.insert(at, c);
	}
	return true;
}

// Square, `to` across (no smaller than the picture): each pixel the
// picture's pixel it lands on.
std::vector<uint8_t> copied_up(const uint8_t *px, uint32_t w, uint32_t h, uint32_t to)
{
	std::vector<uint8_t> out((size_t)to * to * 4);
	for (uint32_t y = 0; y < to; y++) {
		const uint32_t sy = std::min<uint64_t>(h - 1, ((uint64_t)y * 2 + 1) * h / (2 * (uint64_t)to));
		for (uint32_t x = 0; x < to; x++) {
			const uint32_t sx = std::min<uint64_t>(w - 1, ((uint64_t)x * 2 + 1) * w / (2 * (uint64_t)to));
			std::memcpy(&out[((size_t)y * to + x) * 4], px + ((size_t)sy * w + sx) * 4, 4);
		}
	}
	return out;
}

// How many of a level's pixels have each see-through value.
std::array<size_t, 256> count_of(const std::vector<uint8_t> &level)
{
	std::array<size_t, 256> n{};
	for (size_t i = 3; i < level.size(); i += 4)
		n[level[i]]++;
	return n;
}

uint8_t scaled(uint8_t a, float scale)
{
	return rounded(a * scale);
}

// The share solid, from the counts: as stored (each value scaled, then
// rounded to a byte) and read (over 255) against the cut-off.
float share_of(const std::array<size_t, 256> &n, float cutoff, float scale)
{
	size_t solid = 0, all = 0;
	for (int v = 0; v < 256; v++) {
		all += n[v];
		if (scaled((uint8_t)v, scale) / 255.0f >= cutoff)
			solid += n[v];
	}
	return all ? (float)solid / all : 0;
}

// A level's see-through values scaled so that `want` of it is solid.
void keep_solid(std::vector<uint8_t> &level, float cutoff, float want)
{
	const std::array<size_t, 256> n = count_of(level);
	if (std::fabs(share_of(n, cutoff, 1) - want) < 1e-6f)
		return;
	// More scale, more solid: the least scale that's solid enough, or
	// the most that isn't, whichever's nearer.
	float lo = 0, hi = 256;
	for (int i = 0; i < 40; i++) {
		const float mid = (lo + hi) / 2;
		(share_of(n, cutoff, mid) >= want ? hi : lo) = mid;
	}
	const float scale = std::fabs(share_of(n, cutoff, lo) - want) < std::fabs(share_of(n, cutoff, hi) - want) ? lo
															 : hi;
	for (size_t i = 3; i < level.size(); i += 4)
		level[i] = scaled(level[i], scale);
}

} // namespace

std::vector<PictureUse> picture_uses(const Model &m)
{
	std::vector<PictureUse> uses(m.images.size());
	std::vector<bool> data(m.images.size(), false), cut(m.images.size(), false);
	auto is = [&](const TexRef &r) { return r.image >= 0 && r.image < (int)m.images.size(); };
	for (const Material &mat : m.materials) {
		for (const TexRef *r : {&mat.metal_map, &mat.normal_map, &mat.occlusion_map})
			if (is(*r))
				data[r->image] = true;
		// The material's own see-through factor counted: the picture's
		// value it hides under. Over 1 nothing shows whatever the
		// picture; 0 or under, all of it does.
		if (mat.alpha != AlphaMode::Mask || !is(mat.base_map) || cut[mat.base_map.image])
			continue;
		const float base = mat.base[3];
		if (!(base > 0) || !std::isfinite(base) || !std::isfinite(mat.cutoff))
			continue;
		const float t = mat.cutoff / base;
		if (t > 0 && t <= 1) {
			// The first MASK material's, when more than one shares it.
			uses[mat.base_map.image].cutoff = t;
			cut[mat.base_map.image] = true;
		}
	}
	for (size_t i = 0; i < uses.size(); i++)
		uses[i].colour = !data[i];
	return uses;
}

ModelPicture picture_from(const uint8_t *rgba, uint32_t w, uint32_t h, const PictureUse &use)
{
	ModelPicture pic;
	if (!rgba || !w || !h)
		return pic;
	uint32_t size = next_power(std::max(w, h));
	// Made bigger with only a few colours (a palette): copied pixel for
	// pixel, and bigger still, so no colour runs into the next.
	const bool bigger = w <= size && h <= size && (w < size || h < size);
	const bool sharp = bigger && few_colours(rgba, w, h);
	if (sharp)
		size = std::max(size, std::min(next_power(std::max(w, h) * SHARP_TIMES), SHARP_MOST));
	pic.size = size;
	pic.levels.push_back(sharp ? copied_up(rgba, w, h, size) : resized(rgba, w, h, size, use.colour));
	uint32_t s = size;
	while (s > 1) {
		uint32_t nw, nh;
		pic.levels.push_back(halved(pic.levels.back(), s, s, use.colour, nw, nh));
		s = nw;
	}
	// A cut-out's: each level made from the one before as it was, then
	// scaled to be as solid as the picture it came from.
	const float t = use.cutoff;
	if (t > 0 && t <= 1) {
		const std::vector<uint8_t> source(rgba, rgba + (size_t)w * h * 4);
		const float want = solid_share(source, t);
		if (want > 0 && want < 1)
			for (std::vector<uint8_t> &level : pic.levels)
				keep_solid(level, t, want);
	}
	return pic;
}

float solid_share(const std::vector<uint8_t> &level, float cutoff, float scale)
{
	const std::array<size_t, 256> n = count_of(level);
	size_t solid = 0, all = 0;
	for (int v = 0; v < 256; v++) {
		all += n[v];
		if (v / 255.0f * scale >= cutoff)
			solid += n[v];
	}
	return all ? (float)solid / all : 0;
}

} // namespace direbot::face
