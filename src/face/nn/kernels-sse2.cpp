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

// The convolutions with SSE2, which every 64-bit PC has: the fallback for
// a CPU without AVX2. Eight floats are two SSE registers.

#include "kernels.hpp"

#if defined(__x86_64__) || defined(_M_X64)

#include <emmintrin.h>

namespace direbot::face::nn::kernels {

namespace {

struct V {
	__m128 a, b;
};

static inline V vload(const float *p)
{
	return {_mm_loadu_ps(p), _mm_loadu_ps(p + 4)};
}
static inline void vstore(float *p, V v)
{
	_mm_storeu_ps(p, v.a);
	_mm_storeu_ps(p + 4, v.b);
}
static inline V vset(float x)
{
	const __m128 s = _mm_set1_ps(x);
	return {s, s};
}
static inline V vfma(V a, V b, V c)
{
	return {_mm_add_ps(_mm_mul_ps(a.a, b.a), c.a), _mm_add_ps(_mm_mul_ps(a.b, b.b), c.b)};
}
static inline V vadd(V a, V b)
{
	return {_mm_add_ps(a.a, b.a), _mm_add_ps(a.b, b.b)};
}
static inline V vmax(V a, V b)
{
	return {_mm_max_ps(a.a, b.a), _mm_max_ps(a.b, b.b)};
}
static inline V vmin(V a, V b)
{
	return {_mm_min_ps(a.a, b.a), _mm_min_ps(a.b, b.b)};
}
static inline __m128 prelu4(__m128 x, __m128 s)
{
	const __m128 up = _mm_cmpge_ps(x, _mm_setzero_ps());
	return _mm_or_ps(_mm_and_ps(up, x), _mm_andnot_ps(up, _mm_mul_ps(x, s)));
}
static inline V vprelu(V x, V s)
{
	return {prelu4(x.a, s.a), prelu4(x.b, s.b)};
}

// 16 registers: 2 pixels x 2 panels (8 registers of sums) or 6 x 1 (12).
constexpr int MR_TWO = 2;
constexpr int MR_ONE = 6;

#include "kernels-impl.inc"

const Table table = {"SSE2", conv, depthwise};

} // namespace

const Table *sse2()
{
	return &table;
}

} // namespace direbot::face::nn::kernels

#else

namespace direbot::face::nn::kernels {
const Table *sse2()
{
	return nullptr;
}
} // namespace direbot::face::nn::kernels

#endif
