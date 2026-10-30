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

// The convolutions with AVX2 and FMA: built with -mavx2 -mfma (GCC,
// Clang) or /arch:AVX2 (Visual Studio), and only called when the CPU has
// both (kernels.cpp checks).

#include "kernels.hpp"

#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__AVX2__) && defined(__FMA__) || defined(_MSC_VER))

#include <immintrin.h>

namespace direbot::face::nn::kernels {

namespace {

typedef __m256 V;

static inline V vload(const float *p)
{
	return _mm256_loadu_ps(p);
}
static inline void vstore(float *p, V a)
{
	_mm256_storeu_ps(p, a);
}
static inline V vset(float x)
{
	return _mm256_set1_ps(x);
}
static inline V vfma(V a, V b, V c)
{
	return _mm256_fmadd_ps(a, b, c);
}
static inline V vadd(V a, V b)
{
	return _mm256_add_ps(a, b);
}
static inline V vmax(V a, V b)
{
	return _mm256_max_ps(a, b);
}
static inline V vmin(V a, V b)
{
	return _mm256_min_ps(a, b);
}
static inline V vprelu(V x, V slope)
{
	const V down = _mm256_mul_ps(x, slope);
	return _mm256_blendv_ps(down, x, _mm256_cmp_ps(x, _mm256_setzero_ps(), _CMP_GE_OQ));
}

// 16 registers: 6 pixels x 2 panels of sums + 2 of weights + 1, or 12 x 1.
constexpr int MR_TWO = 6;
constexpr int MR_ONE = 12;

#include "kernels-impl.inc"

const Table table = {"AVX2", conv, depthwise};

} // namespace

const Table *avx2()
{
	return &table;
}

} // namespace direbot::face::nn::kernels

#else

namespace direbot::face::nn::kernels {
const Table *avx2()
{
	return nullptr;
}
} // namespace direbot::face::nn::kernels

#endif
