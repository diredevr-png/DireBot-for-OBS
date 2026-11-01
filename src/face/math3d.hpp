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

// Small 3D sums for the 3D face avatar (model.hpp, rig.hpp): points,
// turns (quaternions, x y z w as glTF has them) and 4 x 4 matrices
// (column after column, as glTF has them: m[12..14] is the move).
// Right-handed, y up, as glTF.

#pragma once

#include <cmath>

namespace direbot::face {

struct V3 {
	float x = 0, y = 0, z = 0;
};
inline V3 operator+(V3 a, V3 b)
{
	return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline V3 operator-(V3 a, V3 b)
{
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline V3 operator*(V3 a, float k)
{
	return {a.x * k, a.y * k, a.z * k};
}
inline V3 operator-(V3 a)
{
	return {-a.x, -a.y, -a.z};
}
inline float dot(V3 a, V3 b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline V3 cross(V3 a, V3 b)
{
	return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(V3 a)
{
	return std::sqrt(dot(a, a));
}
inline V3 normalized(V3 a, V3 fallback = {0, 1, 0})
{
	const float l = length(a);
	return l > 1e-12f ? a * (1 / l) : fallback;
}

struct Quat {
	float x = 0, y = 0, z = 0, w = 1;
};
inline Quat operator*(Quat a, Quat b)
{
	return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Quat conj(Quat q)
{
	return {-q.x, -q.y, -q.z, q.w};
}
inline Quat normalized(Quat q)
{
	const float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
	return l > 1e-12f ? Quat{q.x / l, q.y / l, q.z / l, q.w / l} : Quat{};
}
inline V3 rotate(Quat q, V3 v)
{
	const V3 u{q.x, q.y, q.z};
	const V3 t = cross(u, v) * 2;
	return v + t * q.w + cross(u, t);
}
// `angle` radians about `axis` (any length).
inline Quat axis_angle(V3 axis, float angle)
{
	const V3 a = normalized(axis);
	const float s = std::sin(angle / 2);
	return {a.x * s, a.y * s, a.z * s, std::cos(angle / 2)};
}
// The shortest turn taking direction `from` to direction `to`.
inline Quat turn_between(V3 from, V3 to)
{
	from = normalized(from);
	to = normalized(to);
	const float c = dot(from, to);
	if (c < -0.99999f) {
		V3 axis = cross({1, 0, 0}, from);
		if (length(axis) < 1e-4f)
			axis = cross({0, 1, 0}, from);
		return axis_angle(axis, 3.14159265f);
	}
	const V3 a = cross(from, to);
	return normalized(Quat{a.x, a.y, a.z, 1 + c});
}
inline Quat slerp(Quat a, Quat b, float t)
{
	float c = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
	if (c < 0) {
		b = {-b.x, -b.y, -b.z, -b.w};
		c = -c;
	}
	if (c > 0.9995f)
		return normalized(Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
				       a.w + (b.w - a.w) * t});
	const float th = std::acos(c), s = std::sin(th);
	const float ka = std::sin((1 - t) * th) / s, kb = std::sin(t * th) / s;
	return {a.x * ka + b.x * kb, a.y * ka + b.y * kb, a.z * ka + b.z * kb, a.w * ka + b.w * kb};
}

struct M4 {
	float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	float &at(int row, int col) { return m[col * 4 + row]; }
	float at(int row, int col) const { return m[col * 4 + row]; }
};
inline M4 operator*(const M4 &a, const M4 &b)
{
	M4 r;
	for (int c = 0; c < 4; c++)
		for (int row = 0; row < 4; row++) {
			float s = 0;
			for (int k = 0; k < 4; k++)
				s += a.at(row, k) * b.at(k, c);
			r.at(row, c) = s;
		}
	return r;
}
// The camera's view (x right, y up, looking along -z) as OBS's 3D wants
// it, for gs_matrix_set with gs_perspective: y turned round (OBS's y runs
// down), still looking along -z. The same on Direct3D and OpenGL: OBS's
// Direct3D turns z round itself, so turning it here as well would put the
// character behind the camera.
inline M4 obs_view(const M4 &view)
{
	M4 flip;
	flip.m[5] = -1;
	return flip * view;
}
inline V3 point(const M4 &a, V3 p)
{
	return {a.m[0] * p.x + a.m[4] * p.y + a.m[8] * p.z + a.m[12],
		a.m[1] * p.x + a.m[5] * p.y + a.m[9] * p.z + a.m[13],
		a.m[2] * p.x + a.m[6] * p.y + a.m[10] * p.z + a.m[14]};
}
inline V3 direction(const M4 &a, V3 d)
{
	return {a.m[0] * d.x + a.m[4] * d.y + a.m[8] * d.z, a.m[1] * d.x + a.m[5] * d.y + a.m[9] * d.z,
		a.m[2] * d.x + a.m[6] * d.y + a.m[10] * d.z};
}
inline V3 move_of(const M4 &a)
{
	return {a.m[12], a.m[13], a.m[14]};
}
inline M4 moved(V3 t)
{
	M4 r;
	r.m[12] = t.x;
	r.m[13] = t.y;
	r.m[14] = t.z;
	return r;
}
inline M4 scaled(float k)
{
	M4 r;
	r.m[0] = r.m[5] = r.m[10] = k;
	return r;
}
inline M4 matrix_of(V3 t, Quat q, V3 s)
{
	M4 r;
	const float x = q.x, y = q.y, z = q.z, w = q.w;
	r.m[0] = (1 - 2 * (y * y + z * z)) * s.x;
	r.m[1] = (2 * (x * y + z * w)) * s.x;
	r.m[2] = (2 * (x * z - y * w)) * s.x;
	r.m[4] = (2 * (x * y - z * w)) * s.y;
	r.m[5] = (1 - 2 * (x * x + z * z)) * s.y;
	r.m[6] = (2 * (y * z + x * w)) * s.y;
	r.m[8] = (2 * (x * z + y * w)) * s.z;
	r.m[9] = (2 * (y * z - x * w)) * s.z;
	r.m[10] = (1 - 2 * (x * x + y * y)) * s.z;
	r.m[12] = t.x;
	r.m[13] = t.y;
	r.m[14] = t.z;
	return r;
}
inline M4 matrix_of(Quat q)
{
	return matrix_of({}, q, {1, 1, 1});
}
// The turn in a matrix (any size taken out), as a quaternion.
inline Quat quat_of(const M4 &a)
{
	V3 c0{a.m[0], a.m[1], a.m[2]}, c1{a.m[4], a.m[5], a.m[6]}, c2{a.m[8], a.m[9], a.m[10]};
	c0 = normalized(c0, {1, 0, 0});
	c1 = normalized(c1, {0, 1, 0});
	c2 = normalized(c2, {0, 0, 1});
	const float tr = c0.x + c1.y + c2.z;
	Quat q;
	if (tr > 0) {
		const float s = std::sqrt(tr + 1) * 2;
		q = {(c1.z - c2.y) / s, (c2.x - c0.z) / s, (c0.y - c1.x) / s, s / 4};
	} else if (c0.x > c1.y && c0.x > c2.z) {
		const float s = std::sqrt(1 + c0.x - c1.y - c2.z) * 2;
		q = {s / 4, (c1.x + c0.y) / s, (c2.x + c0.z) / s, (c1.z - c2.y) / s};
	} else if (c1.y > c2.z) {
		const float s = std::sqrt(1 + c1.y - c0.x - c2.z) * 2;
		q = {(c1.x + c0.y) / s, s / 4, (c2.y + c1.z) / s, (c2.x - c0.z) / s};
	} else {
		const float s = std::sqrt(1 + c2.z - c0.x - c1.y) * 2;
		q = {(c2.x + c0.z) / s, (c2.y + c1.z) / s, s / 4, (c0.y - c1.x) / s};
	}
	return normalized(q);
}
// The inverse of any 4 x 4 (identity if it has none).
inline M4 inverse(const M4 &a)
{
	const float *m = a.m;
	float inv[16];
	inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
		 m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
	inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
		 m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
	inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
		 m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
	inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
		  m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
	inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
		 m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
	inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
		 m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
	inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
		 m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
	inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
		  m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
	inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
		 m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
	inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
		 m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
	inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
		  m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
	inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
		  m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
	inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
		 m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
	inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
		 m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
	inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
		  m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
	inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
		  m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
	const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
	M4 r;
	if (std::fabs(det) < 1e-30f)
		return r;
	for (int i = 0; i < 16; i++)
		r.m[i] = inv[i] / det;
	return r;
}

} // namespace direbot::face
