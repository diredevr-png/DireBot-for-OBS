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

#include "plan.hpp"

#include <algorithm>
#include <cmath>

namespace direbot::moves {

namespace {

constexpr float PI = 3.14159265358979f;

float lerp(float a, float b, float t)
{
	return a + (b - a) * t;
}

// Off the nearest edge of the canvas, the same size and turn.
Pose off_edge(const Pose &p, float cw, float ch)
{
	Pose out = p;
	const float half_w = std::fabs(p.w) / 2, half_h = std::fabs(p.h) / 2;
	const float left = p.cx, right = cw - p.cx, top = p.cy, bottom = ch - p.cy;
	const float nearest = std::min(std::min(left, right), std::min(top, bottom));
	if (nearest == left)
		out.cx = -half_w;
	else if (nearest == right)
		out.cx = cw + half_w;
	else if (nearest == top)
		out.cy = -half_h;
	else
		out.cy = ch + half_h;
	return out;
}

Pose hidden(const Pose &p, const std::string &how, float cw, float ch)
{
	Pose out = how == "slide" ? off_edge(p, cw, ch) : p;
	if (how == "grow") {
		out.w = 0;
		out.h = 0;
	}
	out.alpha = how == "slide" ? 1.0f : 0.0f;
	return out;
}

} // namespace

Pose pose_of(const Drawn &d)
{
	Pose p;
	const float cw = std::max(1.0f, d.src_w - d.crop_l - d.crop_r);
	const float ch = std::max(1.0f, d.src_h - d.crop_t - d.crop_b);
	const float sx = std::hypot(d.ax, d.ay);
	const float det = d.ax * d.by - d.ay * d.bx;
	const float sy = sx > 0 ? det / sx : 0;
	p.rot = std::atan2(d.ay, d.ax);
	p.w = cw * sx;
	p.h = ch * sy;
	p.cx = d.tx + d.ax * cw / 2 + d.bx * ch / 2;
	p.cy = d.ty + d.ay * cw / 2 + d.by * ch / 2;
	p.crop_l = d.crop_l;
	p.crop_t = d.crop_t;
	p.crop_r = d.crop_r;
	p.crop_b = d.crop_b;
	return p;
}

void matrix_of(const Pose &p, float src_w, float src_h, float &ax, float &ay, float &bx, float &by, float &tx,
	       float &ty)
{
	const float cw = std::max(1.0f, src_w - p.crop_l - p.crop_r);
	const float ch = std::max(1.0f, src_h - p.crop_t - p.crop_b);
	const float sx = p.w / cw, sy = p.h / ch;
	const float c = std::cos(p.rot), s = std::sin(p.rot);
	ax = c * sx;
	ay = s * sx;
	bx = -s * sy;
	by = c * sy;
	tx = p.cx - ax * cw / 2 - bx * ch / 2;
	ty = p.cy - ay * cw / 2 - by * ch / 2;
}

std::vector<Step> plan(const std::vector<Item> &before, const std::vector<Item> &after, const std::string &come,
		       float canvas_w, float canvas_h)
{
	std::vector<bool> used(before.size(), false);
	std::vector<Step> moves;
	for (const Item &b : after) {
		size_t found = before.size();
		for (size_t i = 0; i < before.size(); i++) {
			if (!used[i] && before[i].key == b.key) {
				found = i;
				break;
			}
		}
		if (found < before.size()) {
			used[found] = true;
			moves.push_back({Kind::Move, b.key, b.src_w, b.src_h, before[found].pose, b.pose});
		} else {
			moves.push_back({Kind::Come, b.key, b.src_w, b.src_h, hidden(b.pose, come, canvas_w, canvas_h),
					 b.pose});
		}
	}
	std::vector<Step> out;
	for (size_t i = 0; i < before.size(); i++)
		if (!used[i])
			out.push_back({Kind::Go, before[i].key, before[i].src_w, before[i].src_h, before[i].pose,
				       hidden(before[i].pose, come, canvas_w, canvas_h)});
	out.insert(out.end(), moves.begin(), moves.end());
	return out;
}

float ease(const std::string &how, float t)
{
	t = std::clamp(t, 0.0f, 1.0f);
	if (how == "linear")
		return t;
	if (how == "ease_in")
		return t * t * t;
	if (how == "ease_out") {
		const float u = 1 - t;
		return 1 - u * u * u;
	}
	if (how == "bounce") {
		// Goes a little past and settles back.
		const float c1 = 1.70158f, c3 = c1 + 1;
		const float u = t - 1;
		return 1 + c3 * u * u * u + c1 * u * u;
	}
	// "smooth": slow, quick, slow.
	return t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.0f) / 2;
}

Pose between(const Pose &a, const Pose &b, float t)
{
	Pose p;
	p.cx = lerp(a.cx, b.cx, t);
	p.cy = lerp(a.cy, b.cy, t);
	// The short way round.
	float turn = std::fmod(b.rot - a.rot, 2 * PI);
	if (turn > PI)
		turn -= 2 * PI;
	if (turn < -PI)
		turn += 2 * PI;
	p.rot = a.rot + turn * t;
	p.w = lerp(a.w, b.w, t);
	p.h = lerp(a.h, b.h, t);
	p.crop_l = lerp(a.crop_l, b.crop_l, t);
	p.crop_t = lerp(a.crop_t, b.crop_t, t);
	p.crop_r = lerp(a.crop_r, b.crop_r, t);
	p.crop_b = lerp(a.crop_b, b.crop_b, t);
	p.alpha = std::clamp(lerp(a.alpha, b.alpha, t), 0.0f, 1.0f);
	return p;
}

} // namespace direbot::moves
