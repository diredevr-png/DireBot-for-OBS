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

// The plain C++ convolutions (what the SIMD ones are checked against),
// and which ones this CPU gets.

#include "kernels.hpp"

#include <algorithm>
#include <cstdint>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

namespace direbot::face::nn::kernels {

namespace {

float finish(float x, const float *slope, int c, float lo, float hi)
{
	if (slope && !(x >= 0))
		x *= slope[c];
	return std::min(hi, std::max(lo, x));
}

void conv_plain(const Conv &j)
{
	const int panels = (j.out_c + PANEL - 1) / PANEL;
	const size_t panel_size = (size_t)j.kh * j.kw * j.in_c * PANEL;
	for (int oy = j.row_from; oy < j.row_to; oy++) {
		for (int ox = 0; ox < j.out_w; ox++) {
			float *out = j.out + ((size_t)oy * j.out_w + ox) * j.out_c;
			for (int p = 0; p < panels; p++) {
				float acc[PANEL];
				for (int i = 0; i < PANEL; i++)
					acc[i] = j.bias[p * PANEL + i];
				const float *w = j.weights + (size_t)p * panel_size;
				for (int ky = 0; ky < j.kh; ky++) {
					const int iy = oy * j.stride_h - j.pad_top + ky * j.dil_h;
					for (int kx = 0; kx < j.kw; kx++, w += (size_t)j.in_c * PANEL) {
						const int ix = ox * j.stride_w - j.pad_left + kx * j.dil_w;
						if (iy < 0 || iy >= j.in_h || ix < 0 || ix >= j.in_w)
							continue;
						const float *in = j.in + ((size_t)iy * j.in_w + ix) * j.in_c;
						for (int ci = 0; ci < j.in_c; ci++)
							for (int i = 0; i < PANEL; i++)
								acc[i] += in[ci] * w[ci * PANEL + i];
					}
				}
				for (int i = 0; i < PANEL && p * PANEL + i < j.out_c; i++) {
					const int c = p * PANEL + i;
					float x = acc[i];
					if (j.residual)
						x += j.residual[((size_t)oy * j.out_w + ox) * j.out_c + c];
					out[c] = finish(x, j.slope, c, j.lo, j.hi);
				}
			}
		}
	}
}

void depthwise_plain(const Depthwise &j)
{
	for (int oy = j.row_from; oy < j.row_to; oy++) {
		for (int ox = 0; ox < j.out_w; ox++) {
			float *out = j.out + ((size_t)oy * j.out_w + ox) * j.c;
			for (int c = 0; c < j.c; c++) {
				float acc = j.bias[c];
				for (int ky = 0; ky < j.kh; ky++) {
					const int iy = oy * j.stride_h - j.pad_top + ky * j.dil_h;
					if (iy < 0 || iy >= j.in_h)
						continue;
					for (int kx = 0; kx < j.kw; kx++) {
						const int ix = ox * j.stride_w - j.pad_left + kx * j.dil_w;
						if (ix < 0 || ix >= j.in_w)
							continue;
						acc += j.in[((size_t)iy * j.in_w + ix) * j.c + c] *
						       j.weights[((size_t)ky * j.kw + kx) * j.c + c];
					}
				}
				out[c] = finish(acc, j.slope, c, j.lo, j.hi);
			}
		}
	}
}

const Table plain_table = {"plain", conv_plain, depthwise_plain};

// AVX2 and FMA, and Windows saving the wide registers on a switch.
bool has_avx2()
{
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
	int r[4];
	__cpuid(r, 0);
	if (r[0] < 7)
		return false;
	__cpuid(r, 1);
	const bool fma = (r[2] >> 12) & 1, osxsave = (r[2] >> 27) & 1, avx = (r[2] >> 28) & 1;
	if (!fma || !osxsave || !avx)
		return false;
	if ((_xgetbv(0) & 6) != 6)
		return false;
	__cpuidex(r, 7, 0);
	return (r[1] >> 5) & 1;
#elif defined(__x86_64__) || defined(__i386__)
	unsigned a, b, c, d;
	if (!__get_cpuid(0, &a, &b, &c, &d) || a < 7)
		return false;
	__get_cpuid(1, &a, &b, &c, &d);
	const bool fma = (c >> 12) & 1, osxsave = (c >> 27) & 1, avx = (c >> 28) & 1;
	if (!fma || !osxsave || !avx)
		return false;
	unsigned lo, hi;
	__asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
	if ((lo & 6) != 6)
		return false;
	__cpuid_count(7, 0, a, b, c, d);
	return (b >> 5) & 1;
#else
	return false;
#endif
}

} // namespace

const Table &plain()
{
	return plain_table;
}

const Table &best()
{
	static const Table *pick = [] {
		const Table *t = has_avx2() ? avx2() : nullptr;
		if (!t)
			t = sse2();
		return t ? t : &plain_table;
	}();
	return *pick;
}

} // namespace direbot::face::nn::kernels
