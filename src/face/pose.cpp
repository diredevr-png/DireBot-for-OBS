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

#include "pose.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace direbot::face {

namespace {

constexpr double PI = 3.14159265358979323846;
// MediaPipe's camera: 63 degrees from top to bottom, the near plane 1 cm
// away (the far one doesn't change the pose).
constexpr double FOV = 63.0, NEAR = 1.0;
constexpr int REST_POINTS = 468;

// Reads Google's file: a protocol buffer.
//   GeometryPipelineMetadata { canonical_mesh = 1 (Mesh3d);
//     procrustes_landmark_basis = 2 (repeated {landmark_id = 1; weight = 2});
//     input_source = 3 }
//   Mesh3d { vertex_type = 1; primitive_type = 2;
//     vertex_buffer = 3 (floats: x, y, z, u, v each); index_buffer = 4 }
struct Proto {
	const uint8_t *p, *end;
	bool bad = false;

	bool more() const { return !bad && p < end; }
	uint64_t varint()
	{
		uint64_t v = 0;
		for (int shift = 0; shift < 64; shift += 7) {
			if (p >= end) {
				bad = true;
				return 0;
			}
			const uint8_t c = *p++;
			v |= (uint64_t)(c & 127) << shift;
			if (c < 128)
				return v;
		}
		bad = true;
		return 0;
	}
	float fixed32()
	{
		if (end - p < 4) {
			bad = true;
			return 0;
		}
		float f;
		std::memcpy(&f, p, 4);
		p += 4;
		return f;
	}
	Proto bytes()
	{
		const uint64_t n = varint();
		if (bad || n > (uint64_t)(end - p)) {
			bad = true;
			return {end, end, true};
		}
		Proto sub{p, p + n};
		p += n;
		return sub;
	}
	void skip(int type)
	{
		if (type == 0)
			varint();
		else if (type == 1 && end - p >= 8)
			p += 8;
		else if (type == 2)
			bytes();
		else if (type == 5 && end - p >= 4)
			p += 4;
		else
			bad = true;
	}
};

void mat3_mul(const double a[9], const double b[9], double out[9])
{
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			out[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
}

// The biggest eigenvalue's vector of a 4 x 4 symmetric matrix (Jacobi).
void biggest_eigenvector(double a[16], double v[4])
{
	double vec[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	for (int sweep = 0; sweep < 60; sweep++) {
		double off = 0, all = 0;
		for (int i = 0; i < 4; i++)
			for (int j = 0; j < 4; j++) {
				all += a[i * 4 + j] * a[i * 4 + j];
				if (i != j)
					off += a[i * 4 + j] * a[i * 4 + j];
			}
		if (off <= 1e-30 * all)
			break;
		for (int p = 0; p < 3; p++) {
			for (int q = p + 1; q < 4; q++) {
				const double apq = a[p * 4 + q];
				if (std::fabs(apq) < 1e-300)
					continue;
				const double theta = (a[q * 4 + q] - a[p * 4 + p]) / (2 * apq);
				const double t =
					(theta >= 0 ? 1 : -1) / (std::fabs(theta) + std::sqrt(theta * theta + 1));
				const double c = 1 / std::sqrt(t * t + 1), s = t * c;
				for (int k = 0; k < 4; k++) {
					const double akp = a[k * 4 + p], akq = a[k * 4 + q];
					a[k * 4 + p] = c * akp - s * akq;
					a[k * 4 + q] = s * akp + c * akq;
				}
				for (int k = 0; k < 4; k++) {
					const double apk = a[p * 4 + k], aqk = a[q * 4 + k];
					a[p * 4 + k] = c * apk - s * aqk;
					a[q * 4 + k] = s * apk + c * aqk;
				}
				for (int k = 0; k < 4; k++) {
					const double vkp = vec[k * 4 + p], vkq = vec[k * 4 + q];
					vec[k * 4 + p] = c * vkp - s * vkq;
					vec[k * 4 + q] = s * vkp + c * vkq;
				}
			}
		}
	}
	int best = 0;
	for (int i = 1; i < 4; i++)
		if (a[i * 4 + i] > a[best * 4 + best])
			best = i;
	for (int k = 0; k < 4; k++)
		v[k] = vec[k * 4 + best];
}

Turn turn_from(const double r[9])
{
	// r = Ry(yaw) Rx(-pitch) Rz(-roll), y up: the nose (0, 0, 1) ends up
	// at (r02, r12, r22).
	Turn t;
	t.yaw = (float)(std::atan2(r[2], r[8]) * 180 / PI);
	t.pitch = (float)(std::asin(std::max(-1.0, std::min(1.0, r[5]))) * 180 / PI);
	t.roll = (float)(-std::atan2(r[3], r[4]) * 180 / PI);
	return t;
}

// A pose's turn, with its size taken out of each column.
void turn_part(const float pose[16], double r[9])
{
	for (int c = 0; c < 3; c++) {
		const double x = pose[c], y = pose[4 + c], z = pose[8 + c];
		const double n = std::sqrt(x * x + y * y + z * z);
		const double k = n > 0 ? 1 / n : 0;
		r[c] = x * k;
		r[3 + c] = y * k;
		r[6 + c] = z * k;
	}
}

} // namespace

bool fit(const double *from, const double *to, const double *weight, size_t n, double turn[9], double &scale,
	 double move[3])
{
	double total = 0, centre[3] = {0, 0, 0};
	for (size_t i = 0; i < n; i++) {
		total += weight[i];
		for (int k = 0; k < 3; k++)
			centre[k] += weight[i] * from[i * 3 + k];
	}
	if (!(total > 1e-9))
		return false;
	for (int k = 0; k < 3; k++)
		centre[k] /= total;
	// S = sum of weight * (from - centre) to^T, and how spread `from` is.
	double s[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0}, spread = 0;
	for (size_t i = 0; i < n; i++) {
		const double w = weight[i];
		if (w == 0)
			continue;
		const double a[3] = {from[i * 3] - centre[0], from[i * 3 + 1] - centre[1], from[i * 3 + 2] - centre[2]};
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++)
				s[r * 3 + c] += w * a[r] * to[i * 3 + c];
		spread += w * (a[0] * from[i * 3] + a[1] * from[i * 3 + 1] + a[2] * from[i * 3 + 2]);
	}
	double norm = 0;
	for (double x : s)
		norm += x * x;
	if (!(norm > 1e-18) || !(spread > 1e-9))
		return false;
	// The best turn (never a mirror) as a quaternion: the biggest
	// eigenvalue's vector of Horn's 4 x 4 matrix (B. Horn, 1987).
	const double xx = s[0], xy = s[1], xz = s[2], yx = s[3], yy = s[4], yz = s[5], zx = s[6], zy = s[7], zz = s[8];
	double m[16] = {xx + yy + zz, yz - zy,      zx - xz,       xy - yx, //
			yz - zy,      xx - yy - zz, xy + yx,       zx + xz, //
			zx - xz,      xy + yx,      -xx + yy - zz, yz + zy, //
			xy - yx,      zx + xz,      yz + zy,       -xx - yy + zz};
	double q[4];
	biggest_eigenvector(m, q);
	const double qn = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	if (!(qn > 0))
		return false;
	const double w = q[0] / qn, x = q[1] / qn, y = q[2] / qn, z = q[3] / qn;
	turn[0] = w * w + x * x - y * y - z * z;
	turn[1] = 2 * (x * y - w * z);
	turn[2] = 2 * (x * z + w * y);
	turn[3] = 2 * (y * x + w * z);
	turn[4] = w * w - x * x + y * y - z * z;
	turn[5] = 2 * (y * z - w * x);
	turn[6] = 2 * (z * x - w * y);
	turn[7] = 2 * (z * y + w * x);
	turn[8] = w * w - x * x - y * y + z * z;
	// Size: how far the turned `from` goes along `to`, against its spread.
	double along = 0;
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			along += turn[r * 3 + c] * s[c * 3 + r];
	scale = along / spread;
	if (!(scale > 1e-9))
		return false;
	for (int k = 0; k < 3; k++)
		move[k] = 0;
	for (size_t i = 0; i < n; i++) {
		const double wt = weight[i];
		if (wt == 0)
			continue;
		for (int r = 0; r < 3; r++) {
			const double turned = turn[r * 3] * from[i * 3] + turn[r * 3 + 1] * from[i * 3 + 1] +
					      turn[r * 3 + 2] * from[i * 3 + 2];
			move[r] += wt * (to[i * 3 + r] - scale * turned);
		}
	}
	for (int k = 0; k < 3; k++)
		move[k] /= total;
	return true;
}

bool Geometry::load(const uint8_t *bytes, size_t size, std::string *problem)
{
	rest_.clear();
	used_.clear();
	weight_.clear();
	rest_out_.clear();
	triangles_.clear();
	std::vector<float> vertices;
	std::vector<std::pair<uint32_t, float>> basis;
	int vertex_type = 0, source = 0;
	Proto top{bytes, bytes + size};
	while (top.more()) {
		const uint64_t key = top.varint();
		const int field = (int)(key >> 3), type = (int)(key & 7);
		if (field == 1 && type == 2) {
			Proto mesh = top.bytes();
			while (mesh.more()) {
				const uint64_t k = mesh.varint();
				const int f = (int)(k >> 3), t = (int)(k & 7);
				if (f == 1 && t == 0) {
					vertex_type = (int)mesh.varint();
				} else if (f == 3 && t == 5) {
					vertices.push_back(mesh.fixed32());
				} else if (f == 3 && t == 2) {
					Proto packed = mesh.bytes();
					while (packed.more())
						vertices.push_back(packed.fixed32());
					mesh.bad |= packed.bad;
				} else if (f == 4 && t == 0) {
					triangles_.push_back((uint32_t)mesh.varint());
				} else if (f == 4 && t == 2) {
					Proto packed = mesh.bytes();
					while (packed.more())
						triangles_.push_back((uint32_t)packed.varint());
					mesh.bad |= packed.bad;
				} else {
					mesh.skip(t);
				}
			}
			top.bad |= mesh.bad;
		} else if (field == 2 && type == 2) {
			Proto ref = top.bytes();
			uint32_t id = 0;
			float weight = 0;
			while (ref.more()) {
				const uint64_t k = ref.varint();
				const int f = (int)(k >> 3), t = (int)(k & 7);
				if (f == 1 && t == 0)
					id = (uint32_t)ref.varint();
				else if (f == 2 && t == 5)
					weight = ref.fixed32();
				else
					ref.skip(t);
			}
			top.bad |= ref.bad;
			basis.emplace_back(id, weight);
		} else if (field == 3 && type == 0) {
			source = (int)top.varint();
		} else {
			top.skip(type);
		}
	}
	auto fail = [&](const char *why) {
		if (problem)
			*problem = why;
		rest_.clear();
		used_.clear();
		weight_.clear();
		triangles_.clear();
		return false;
	};
	// Only the kind made for the face mesh's points (input_source 0 or 1,
	// not 2: a face detector's), with x, y, z, u, v per point.
	if (top.bad || vertex_type != 0 || source > 1)
		return fail("the face's 3D file is damaged");
	if (vertices.size() != (size_t)REST_POINTS * 5 || basis.empty() || triangles_.size() % 3)
		return fail("the face's 3D file isn't the one expected");
	for (uint32_t i : triangles_)
		if (i >= (uint32_t)REST_POINTS)
			return fail("the face's 3D file is damaged");
	rest_.resize((size_t)REST_POINTS * 3);
	rest_out_.resize((size_t)REST_POINTS * 3);
	for (int i = 0; i < REST_POINTS; i++)
		for (int k = 0; k < 3; k++) {
			rest_[(size_t)i * 3 + k] = vertices[(size_t)i * 5 + k];
			rest_out_[(size_t)i * 3 + k] = vertices[(size_t)i * 5 + k];
		}
	for (const auto &b : basis) {
		if (b.first >= (uint32_t)REST_POINTS || !(b.second >= 0))
			return fail("the face's 3D file is damaged");
		if (b.second > 0) {
			used_.push_back((int)b.first);
			weight_.push_back(b.second);
		}
	}
	if (used_.empty())
		return fail("the face's 3D file is damaged");
	return true;
}

bool Geometry::pose(const float *xyz, int w, int h, float out[16]) const
{
	if (!loaded() || w <= 0 || h <= 0)
		return false;
	// Too bunched up to work out (MediaPipe's own check).
	double mx = 0, my = 0;
	for (int i = 0; i < REST_POINTS; i++) {
		mx += xyz[i * 3] / w;
		my += xyz[i * 3 + 1] / h;
	}
	mx /= REST_POINTS;
	my /= REST_POINTS;
	double most = 0;
	for (int i = 0; i < REST_POINTS; i++) {
		const double dx = xyz[i * 3] / w - mx, dy = xyz[i * 3 + 1] / h - my;
		most = std::max(most, dx * dx + dy * dy);
	}
	if (std::sqrt(most) <= 1e-3)
		return false;

	// The picture on the camera's near plane (y up), z on x's scale.
	const double near_h = 2 * NEAR * std::tan(0.5 * FOV * PI / 180), near_w = near_h * w / h;
	const size_t n = used_.size();
	std::vector<double> from(n * 3), screen(n * 3), to(n * 3);
	double depth = 0;
	for (int i = 0; i < REST_POINTS; i++)
		depth += xyz[i * 3 + 2] / w * near_w;
	depth /= REST_POINTS;
	for (size_t j = 0; j < n; j++) {
		const int i = used_[j];
		for (int k = 0; k < 3; k++)
			from[j * 3 + k] = rest_[(size_t)i * 3 + k];
		screen[j * 3] = xyz[i * 3] / w * near_w - near_w / 2;
		screen[j * 3 + 1] = (1 - xyz[i * 3 + 1] / h) * near_h - near_h / 2;
		screen[j * 3 + 2] = xyz[i * 3 + 2] / w * near_w;
	}
	double turn[9], scale, move[3];
	// 1: how big against the face at rest, from the flat picture.
	for (size_t j = 0; j < n; j++) {
		to[j * 3] = screen[j * 3];
		to[j * 3 + 1] = screen[j * 3 + 1];
		to[j * 3 + 2] = -screen[j * 3 + 2];
	}
	if (!fit(from.data(), to.data(), weight_.data(), n, turn, scale, move))
		return false;
	const double first = scale;
	// Back out into 3D at that size.
	auto unproject = [&](double size) {
		for (size_t j = 0; j < n; j++) {
			const double z = (screen[j * 3 + 2] - depth + NEAR) / size;
			to[j * 3] = screen[j * 3] * z / NEAR;
			to[j * 3 + 1] = screen[j * 3 + 1] * z / NEAR;
			to[j * 3 + 2] = -z;
		}
	};
	// 2: its size again, now in 3D.
	unproject(first);
	if (!fit(from.data(), to.data(), weight_.data(), n, turn, scale, move))
		return false;
	// 3: the answer, at both sizes together.
	unproject(first * scale);
	if (!fit(from.data(), to.data(), weight_.data(), n, turn, scale, move))
		return false;
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 3; c++)
			out[r * 4 + c] = (float)(scale * turn[r * 3 + c]);
		out[r * 4 + 3] = (float)move[r];
	}
	out[12] = out[13] = out[14] = 0;
	out[15] = 1;
	return true;
}

Turn turn_of(const float pose[16])
{
	double r[9];
	turn_part(pose, r);
	return turn_from(r);
}

Turn turn_since(const float pose[16], const float rest[16])
{
	double r[9], r0[9], r0t[9], d[9];
	turn_part(pose, r);
	turn_part(rest, r0);
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			r0t[i * 3 + j] = r0[j * 3 + i];
	mat3_mul(r0t, r, d);
	return turn_from(d);
}

} // namespace direbot::face
