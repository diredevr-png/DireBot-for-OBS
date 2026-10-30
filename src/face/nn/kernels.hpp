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

// The engine's convolutions (net.hpp), where nearly all of its time goes.
// Three versions of the same code: plain C++, SSE2 (every 64-bit PC) and
// AVX2 with FMA (most PCs since 2013), picked once by what the CPU has.
//
// Pictures are row after row, pixel after pixel, channel after channel
// (NHWC, as TensorFlow Lite keeps them). A convolution's weights are
// packed once into panels of 8 output channels: for each panel, each
// tap of the filter and each input channel, its 8 weights side by side
// (zeros past the last channel).
//
// The SIMD versions live in files built with their own compiler flags.
// They use no standard library and no inline function from a header, so
// an AVX2 copy of one can never be picked for a CPU without AVX2.

#pragma once

#include <cstddef>

namespace direbot::face::nn::kernels {

constexpr int PANEL = 8;
// Taps a convolution's filter can have at most (5 x 5).
constexpr int MOST_TAPS = 25;

struct Conv {
	const float *in = nullptr;
	int in_h = 0, in_w = 0, in_c = 0;
	float *out = nullptr;
	int out_h = 0, out_w = 0, out_c = 0;
	const float *weights = nullptr; // packed (see above)
	const float *bias = nullptr;    // out_c, rounded up to whole panels
	int kh = 1, kw = 1, stride_h = 1, stride_w = 1, pad_top = 0, pad_left = 0, dil_h = 1, dil_w = 1;
	// Afterwards, in this order: + residual (the same shape as out), then
	// PReLU (slope per channel, rounded up to whole panels), then a clamp.
	const float *residual = nullptr;
	const float *slope = nullptr;
	float lo = -3.4e38f, hi = 3.4e38f;
	// Zeros, in_c long, for taps that land outside the picture.
	const float *zeros = nullptr;
	// Only these output rows (for splitting the work).
	int row_from = 0, row_to = 0;
};

struct Depthwise {
	const float *in = nullptr;
	int in_h = 0, in_w = 0, c = 0;
	float *out = nullptr;
	int out_h = 0, out_w = 0;
	const float *weights = nullptr; // [kh][kw][c], as the file has them
	const float *bias = nullptr;    // c
	int kh = 3, kw = 3, stride_h = 1, stride_w = 1, pad_top = 0, pad_left = 0, dil_h = 1, dil_w = 1;
	const float *slope = nullptr;
	float lo = -3.4e38f, hi = 3.4e38f;
	int row_from = 0, row_to = 0;
};

struct Table {
	const char *name;
	void (*conv)(const Conv &);
	void (*depthwise)(const Depthwise &);
};

const Table &plain();
// The fastest this CPU can run.
const Table &best();

// Built in their own files (null where the compiler can't make them).
const Table *sse2();
const Table *avx2();

} // namespace direbot::face::nn::kernels
