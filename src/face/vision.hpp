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

// What the face tracker (tracker.cpp) and the body tracker (body.cpp)
// both use around Google's models: finding a model in a .task file,
// cutting a turned square out of the camera's picture the way MediaPipe
// does, and MediaPipe's One Euro smoothing.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "tracker.hpp"

namespace direbot::face::vision {

constexpr double PI = 3.14159265358979323846;

inline uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

inline uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// A file in a zip that is stored as it is (Google's .task files keep
// their models that way, so nothing needs unpacking).
struct Entry {
	const uint8_t *data = nullptr;
	size_t size = 0;
};

inline bool zip_entry(const uint8_t *zip, size_t size, const std::string &name, Entry &out)
{
	if (size < 22)
		return false;
	// The end record, within the last 64 KB (it can carry a comment).
	size_t end = size - 22;
	const size_t stop = size > 22 + 65535 ? size - 22 - 65535 : 0;
	while (le32(zip + end) != 0x06054b50) {
		if (end == stop)
			return false;
		end--;
	}
	const uint16_t count = le16(zip + end + 10);
	size_t at = le32(zip + end + 16);
	for (uint16_t i = 0; i < count; i++) {
		if (at + 46 > size || le32(zip + at) != 0x02014b50)
			return false;
		const uint16_t method = le16(zip + at + 10);
		const uint32_t packed = le32(zip + at + 20), unpacked = le32(zip + at + 24);
		const uint16_t name_len = le16(zip + at + 28), extra_len = le16(zip + at + 30),
			       note_len = le16(zip + at + 32);
		const uint32_t local = le32(zip + at + 42);
		if (at + 46 + name_len > size)
			return false;
		if (name_len == name.size() && std::memcmp(zip + at + 46, name.data(), name_len) == 0) {
			if (method != 0 || packed != unpacked)
				return false;
			if ((size_t)local + 30 > size || le32(zip + local) != 0x04034b50)
				return false;
			const size_t data = (size_t)local + 30 + le16(zip + local + 26) + le16(zip + local + 28);
			if (data > size || packed > size - data)
				return false;
			out.data = zip + data;
			out.size = packed;
			return true;
		}
		at += 46 + (size_t)name_len + extra_len + note_len;
	}
	return false;
}

inline size_t elements(const std::vector<int> &shape)
{
	size_t n = 1;
	for (int d : shape)
		n *= (size_t)std::max(0, d);
	return n;
}

inline float sigmoid(float x)
{
	return 1 / (1 + std::exp(-x));
}

// An angle brought into -pi..pi (MediaPipe's NormalizeRadians).
inline float wrap_angle(float a)
{
	return (float)(a - 2 * PI * std::floor((a + PI) / (2 * PI)));
}

// A turned box in the picture: its middle and size (pixels) and its turn
// (radians, clockwise as the picture shows it).
struct Box {
	float cx = 0, cy = 0, w = 0, h = 0, angle = 0;
};

// Cuts `box` out of the picture at out_w x out_h, the way MediaPipe does
// it (OpenCV's warp with bilinear sampling: 1/32 pixel steps, weights in
// 15 bits, rounded to whole 0-255 values), then each value * scale +
// offset as floats, red, green, blue. Outside the picture: black, or
// the nearest edge pixel.
inline void cut(const Frame &pic, const Box &box, int out_w, int out_h, bool black_edges, float scale, float offset,
		float *out)
{
	const double c = std::cos((double)box.angle), s = std::sin((double)box.angle);
	const double sx = box.w / out_w, sy = box.h / out_h;
	// out (u, v) -> picture: middle + turn * (u * sx - w/2, v * sy - h/2).
	const double m0 = c * sx, m1 = -s * sy, m2 = box.cx - c * box.w / 2 + s * box.h / 2;
	const double m3 = s * sx, m4 = c * sy, m5 = box.cy - s * box.w / 2 - c * box.h / 2;
	const int r = pic.bgra ? 2 : 0, b = pic.bgra ? 0 : 2;
	const int w = pic.w, h = pic.h;
	for (int v = 0; v < out_h; v++) {
		const double x0 = m1 * v + m2, y0 = m4 * v + m5;
		float *o = out + (size_t)v * out_w * 3;
		for (int u = 0; u < out_w; u++, o += 3) {
			const long X = std::lrint((x0 + m0 * u) * 32), Y = std::lrint((y0 + m3 * u) * 32);
			const long ix = X >> 5, iy = Y >> 5;
			const int fx = (int)(X & 31), fy = (int)(Y & 31);
			const int w00 = (32 - fx) * (32 - fy) * 32, w01 = fx * (32 - fy) * 32;
			const int w10 = (32 - fx) * fy * 32, w11 = fx * fy * 32;
			const uint8_t *p00, *p01, *p10, *p11;
			static const uint8_t zero[4] = {0, 0, 0, 0};
			if (ix >= 0 && ix < w - 1 && iy >= 0 && iy < h - 1) {
				p00 = pic.px + (size_t)iy * pic.stride + (size_t)ix * 4;
				p01 = p00 + 4;
				p10 = p00 + pic.stride;
				p11 = p10 + 4;
			} else if (black_edges) {
				if (ix >= w || ix + 1 < 0 || iy >= h || iy + 1 < 0) {
					o[0] = o[1] = o[2] = offset;
					continue;
				}
				auto at = [&](long x, long y) {
					return x >= 0 && x < w && y >= 0 && y < h
						       ? pic.px + (size_t)y * pic.stride + (size_t)x * 4
						       : zero;
				};
				p00 = at(ix, iy);
				p01 = at(ix + 1, iy);
				p10 = at(ix, iy + 1);
				p11 = at(ix + 1, iy + 1);
			} else {
				auto at = [&](long x, long y) {
					x = std::min<long>(std::max<long>(x, 0), w - 1);
					y = std::min<long>(std::max<long>(y, 0), h - 1);
					return pic.px + (size_t)y * pic.stride + (size_t)x * 4;
				};
				p00 = at(ix, iy);
				p01 = at(ix + 1, iy);
				p10 = at(ix, iy + 1);
				p11 = at(ix + 1, iy + 1);
			}
			const int ch[3] = {r, 1, b};
			for (int k = 0; k < 3; k++) {
				const int i = ch[k];
				const int sum = p00[i] * w00 + p01[i] * w01 + p10[i] * w10 + p11[i] * w11;
				const int value = std::min(255, std::max(0, (sum + (1 << 14)) >> 15));
				o[k] = (float)value * scale + offset;
			}
		}
	}
}

// MediaPipe's One Euro filter (G. Casiez et al., 2012): a low-pass filter
// whose cut-off rises with how fast the value moves.
struct LowPass {
	bool started = false;
	float raw = 0, kept = 0;
	float apply(float value, double alpha)
	{
		const float result = started ? (float)(alpha * value + (1 - alpha) * kept) : value;
		started = true;
		raw = value;
		kept = result;
		return result;
	}
};

struct OneEuro {
	double hz = 30;
	int64_t last = -1;
	LowPass x, dx;

	static double alpha(double hz, double cutoff)
	{
		const double te = 1 / hz, tau = 1 / (2 * PI * cutoff);
		return 1 / (1 + tau / te);
	}
	// `ns`: when (nanoseconds, only ever forward); `scale`: the value's
	// units -> the units the speed is judged in (1 / the size of what's
	// followed, so a far-off face or hand is smoothed as a near one).
	float apply(int64_t ns, double scale, float value, double min_cutoff, double beta, double d_cutoff = 1.0)
	{
		if (last >= ns)
			return value;
		if (last != 0 && ns != 0)
			hz = 1e9 / (double)(ns - last);
		last = ns;
		const double speed = x.started ? (value - x.raw) * scale * hz : 0;
		const double eased = dx.apply((float)speed, alpha(hz, d_cutoff));
		const double cutoff = min_cutoff + beta * std::fabs(eased);
		return x.apply(value, alpha(hz, cutoff));
	}
};

// The smoothing slider (0 steadiest to 1 snappiest, 0.5 MediaPipe's own)
// as a One Euro filter's minimum cut-off and speed factor: steadier at
// rest and slower to follow one way, the other way snappier.
inline void smoothing_of(float amount, double min_cutoff, double beta, double &out_min_cutoff, double &out_beta)
{
	const double t = std::min(1.0f, std::max(0.0f, amount)) * 2 - 1;
	out_min_cutoff = min_cutoff * std::pow(5.0, t);
	out_beta = beta * std::pow(3.0, t);
}

} // namespace direbot::face::vision
