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

#include "tracker.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <nlohmann/json.hpp>

namespace direbot::face {

using json = nlohmann::json;

namespace {

// A part this alike (-1 to 1) might be it; the three have to agree too.
constexpr float PART_Z = 0.5f;
// Two parts alone (the third hidden by a hand or the mic) only count
// this alike, and only close to where the face was.
constexpr float PAIR_Z = 0.62f;
// Found from nothing: all three this alike.
constexpr float FIND_Z = 0.72f;
// ...and fitting the set-up's shape this closely.
constexpr float FIND_SHAPE = 0.15f;
// How far a part may sit from where the other two put it, against the
// eye gap.
constexpr float SHAPE = 0.22f;
// Bigger or smaller than at set-up, at most.
constexpr float MIN_SCALE = 0.3f, MAX_SCALE = 3.5f;
constexpr float MAX_ANGLE = 0.8f;
// Lost: how many looks between looks over the whole picture.
constexpr int FIND_EVERY = 8;
// Templates bigger than this many pixels use every other one.
constexpr int MOST_PIXELS = 500;
// How much bigger than its box each part's look is kept (across, down).
constexpr float GROW_ACROSS = 1.75f, GROW_DOWN = 2.1f;

float clampf(float v, float lo, float hi)
{
	return std::min(hi, std::max(lo, v));
}

float sample(const uint8_t *img, int w, int h, float x, float y)
{
	x = clampf(x, 0, (float)w - 1.001f);
	y = clampf(y, 0, (float)h - 1.001f);
	const int x0 = (int)x, y0 = (int)y;
	const float fx = x - x0, fy = y - y0;
	const uint8_t *p = img + (size_t)y0 * w + x0;
	const float top = p[0] + (p[1] - p[0]) * fx;
	const float bottom = p[w] + (p[w + 1] - p[w]) * fx;
	return top + (bottom - top) * fy;
}

float sample(const Tracker::Patch &p, float x, float y)
{
	x = clampf(x, 0, (float)p.w - 1.001f);
	y = clampf(y, 0, (float)p.h - 1.001f);
	const int x0 = (int)x, y0 = (int)y;
	const float fx = x - x0, fy = y - y0;
	const float *q = &p.px[(size_t)y0 * p.w + x0];
	const float top = q[0] + (q[1] - q[0]) * fx;
	const float bottom = q[p.w] + (q[p.w + 1] - q[p.w]) * fx;
	return top + (bottom - top) * fy;
}

// A template turned and sized for one look: offsets from its centre and
// its zero-mean, unit-length values.
struct Warped {
	std::vector<int> dx, dy;
	std::vector<float> t;
	int left = 0, right = 0, top = 0, bottom = 0;
	bool ok = false;
};

Warped warp(const Tracker::Patch &orig, const Tracker::Patch &learned, float scale, float angle)
{
	Warped out;
	const float c = std::cos(angle), s = std::sin(angle);
	const float hw = orig.w * 0.5f, hh = orig.h * 0.5f;
	const int ex = (int)std::ceil((std::fabs(c) * hw + std::fabs(s) * hh) * scale);
	const int ey = (int)std::ceil((std::fabs(s) * hw + std::fabs(c) * hh) * scale);
	const int area = (2 * ex + 1) * (2 * ey + 1);
	const int stride = area > MOST_PIXELS * 4 ? 3 : area > MOST_PIXELS ? 2 : 1;
	double sum = 0;
	for (int y = -ey; y <= ey; y += stride) {
		for (int x = -ex; x <= ex; x += stride) {
			// Back into the set-up patch.
			const float u = (c * x + s * y) / scale + hw - 0.5f;
			const float v = (-s * x + c * y) / scale + hh - 0.5f;
			if (u < -0.5f || v < -0.5f || u > orig.w - 0.5f || v > orig.h - 0.5f)
				continue;
			const float value = 0.5f * (sample(orig, u, v) + sample(learned, u, v));
			out.dx.push_back(x);
			out.dy.push_back(y);
			out.t.push_back(value);
			sum += value;
			out.left = std::min(out.left, x);
			out.right = std::max(out.right, x);
			out.top = std::min(out.top, y);
			out.bottom = std::max(out.bottom, y);
		}
	}
	if (out.t.size() < 12)
		return out;
	const float mean = (float)(sum / out.t.size());
	double norm = 0;
	for (float &v : out.t) {
		v -= mean;
		norm += (double)v * v;
	}
	if (norm < 1e-3)
		return out;
	const float k = (float)(1.0 / std::sqrt(norm));
	for (float &v : out.t)
		v *= k;
	out.ok = true;
	return out;
}

// How alike (-1 to 1) the template is to the picture centred at (x, y).
// Flat picture there: 0, it isn't a face part.
float zncc(const Warped &t, const uint8_t *img, int w, int h, int x, int y)
{
	if (x + t.left < 0 || y + t.top < 0 || x + t.right >= w || y + t.bottom >= h)
		return -1;
	double si = 0, sii = 0, sit = 0;
	const size_t n = t.t.size();
	const uint8_t *base = img + (size_t)y * w + x;
	for (size_t k = 0; k < n; k++) {
		const float v = base[(ptrdiff_t)t.dy[k] * w + t.dx[k]];
		si += v;
		sii += (double)v * v;
		sit += (double)v * t.t[k];
	}
	const double var = sii - si * si / (double)n;
	if (var < n * 4.0) // under ~2 grey levels of change: flat
		return 0;
	return (float)(sit / std::sqrt(var));
}

// The peak's fractional position from its neighbours.
float peak(float before, float at, float after)
{
	const float d = before - 2 * at + after;
	if (d >= -1e-6f)
		return 0;
	return clampf(0.5f * (before - after) / d, -0.5f, 0.5f);
}

const char *B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

} // namespace

std::string encode(const std::vector<uint8_t> &bytes)
{
	std::string out;
	out.reserve((bytes.size() + 2) / 3 * 4);
	for (size_t i = 0; i < bytes.size(); i += 3) {
		const uint32_t n = (uint32_t)bytes[i] << 16 | (i + 1 < bytes.size() ? (uint32_t)bytes[i + 1] << 8 : 0) |
				   (i + 2 < bytes.size() ? bytes[i + 2] : 0);
		out += B64[n >> 18 & 63];
		out += B64[n >> 12 & 63];
		out += i + 1 < bytes.size() ? B64[n >> 6 & 63] : '=';
		out += i + 2 < bytes.size() ? B64[n & 63] : '=';
	}
	return out;
}

bool decode(const std::string &text, std::vector<uint8_t> &bytes)
{
	bytes.clear();
	uint32_t n = 0;
	int bits = 0;
	for (char ch : text) {
		if (ch == '=')
			break;
		const char *at = std::strchr(B64, ch);
		if (!at || !ch)
			return false;
		n = n << 6 | (uint32_t)(at - B64);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			bytes.push_back((uint8_t)(n >> bits & 255));
		}
	}
	return true;
}

bool Tracker::set_up(const uint8_t *grey, int w, int h, const Box parts[PARTS], std::string *problem)
{
	auto say = [&](const char *words) {
		if (problem)
			*problem = words;
		return false;
	};
	if (!grey || w < 32 || h < 32)
		return say("The still is too small.");
	Box grown[PARTS];
	float cx[PARTS], cy[PARTS], bw[PARTS];
	for (int i = 0; i < PARTS; i++) {
		const Box &b = parts[i];
		const float x0 = clampf(b.x, 0, 1) * w, y0 = clampf(b.y, 0, 1) * h;
		const float x1 = clampf(b.x + b.w, 0, 1) * w, y1 = clampf(b.y + b.h, 0, 1) * h;
		if (x1 - x0 < 5 || y1 - y0 < 4)
			return say(i == MOUTH ? "The mouth box is too small - make it bigger, or come closer to the camera."
					      : "An eye box is too small - make it bigger, or come closer to the camera.");
		cx[i] = (x0 + x1) * 0.5f;
		cy[i] = (y0 + y1) * 0.5f;
		bw[i] = x1 - x0;
		// What's found again is more than the box: the brow and the skin
		// around an eye, the beard around a mouth. An eye alone changes
		// too much as you look about; with what's around it, it's yours.
		// Kept centred on the box, so less where it meets the picture's
		// edge.
		const float hw = std::min({(x1 - x0) * GROW_ACROSS * 0.5f, cx[i], w - cx[i]});
		const float hh = std::min({(y1 - y0) * GROW_DOWN * 0.5f, cy[i], h - cy[i]});
		grown[i] = {(cx[i] - hw) / w, (cy[i] - hh) / h, hw * 2 / w, hh * 2 / h};
	}
	const float gap = std::hypot(cx[1] - cx[0], cy[1] - cy[0]);
	const float drop = std::hypot(cx[MOUTH] - (cx[0] + cx[1]) * 0.5f, cy[MOUTH] - (cy[0] + cy[1]) * 0.5f);
	if (gap < std::max(bw[0], bw[1]) * 0.6f)
		return say("The two eye boxes are on top of each other.");
	if (drop < gap * 0.3f)
		return say("The mouth box should be below the eyes.");
	return take(grey, w, h, grown, problem);
}

bool Tracker::take(const uint8_t *grey, int w, int h, const Box parts[PARTS], std::string *problem)
{
	auto say = [&](const char *words) {
		if (problem)
			*problem = words;
		return false;
	};
	Ref refs[PARTS];
	for (int i = 0; i < PARTS; i++) {
		const Box &b = parts[i];
		const int x0 = (int)std::lround(clampf(b.x, 0, 1) * w), y0 = (int)std::lround(clampf(b.y, 0, 1) * h);
		const int x1 = (int)std::lround(clampf(b.x + b.w, 0, 1) * w),
			  y1 = (int)std::lround(clampf(b.y + b.h, 0, 1) * h);
		if (x1 - x0 < 5 || y1 - y0 < 4)
			return say("A box is too small.");
		Patch p;
		p.w = x1 - x0;
		p.h = y1 - y0;
		p.px.resize((size_t)p.w * p.h);
		double sum = 0, sq = 0;
		for (int y = 0; y < p.h; y++)
			for (int x = 0; x < p.w; x++) {
				const float v = grey[(size_t)(y0 + y) * w + x0 + x];
				p.px[(size_t)y * p.w + x] = v;
				sum += v;
				sq += (double)v * v;
			}
		const double n = (double)p.px.size();
		const double sd = std::sqrt(std::max(0.0, sq / n - (sum / n) * (sum / n)));
		if (sd < 5)
			return say(i == MOUTH ? "The mouth box is too plain to find again - is the camera on, and the room lit?"
					      : "An eye box is too plain to find again - is the camera on, and the room lit?");
		refs[i].original = p;
		refs[i].learned = p;
		// Pixel coordinates: pixel x's middle is at x.
		refs[i].cx = x0 + (p.w - 1) * 0.5f;
		refs[i].cy = y0 + (p.h - 1) * 0.5f;
	}
	const float gap = std::hypot(refs[1].cx - refs[0].cx, refs[1].cy - refs[0].cy);
	const float mx = (refs[0].cx + refs[1].cx) * 0.5f, my = (refs[0].cy + refs[1].cy) * 0.5f;
	const float drop = std::hypot(refs[MOUTH].cx - mx, refs[MOUTH].cy - my);
	if (gap < 6)
		return say("The two eye boxes are on top of each other.");
	if (drop < gap * 0.3f)
		return say("The mouth box should be below the eyes.");
	for (int i = 0; i < PARTS; i++)
		ref_[i] = refs[i];
	setup_w_ = w;
	setup_h_ = h;
	eye_gap_ = gap;
	ref_ratio_ = gap / drop;
	ready_ = true;
	forget();
	// The next look starts where the still had it.
	have_last_ = true;
	for (int i = 0; i < PARTS; i++) {
		last_x_[i] = ref_[i].cx;
		last_y_[i] = ref_[i].cy;
	}
	last_scale_ = 1;
	last_angle_ = 0;
	last_w_ = w;
	last_h_ = h;
	return true;
}

std::string Tracker::save() const
{
	if (!ready_)
		return "";
	json parts = json::array();
	for (const Ref &r : ref_) {
		std::vector<uint8_t> bytes(r.original.px.size());
		for (size_t i = 0; i < bytes.size(); i++)
			bytes[i] = (uint8_t)clampf(std::round(r.original.px[i]), 0, 255);
		parts.push_back({{"x", r.cx - (r.original.w - 1) * 0.5f},
				 {"y", r.cy - (r.original.h - 1) * 0.5f},
				 {"w", r.original.w},
				 {"h", r.original.h},
				 {"px", encode(bytes)}});
	}
	return json{{"v", 1}, {"w", setup_w_}, {"h", setup_h_}, {"parts", parts}}.dump();
}

bool Tracker::load(const std::string &text)
{
	const json j = json::parse(text, nullptr, false);
	if (j.is_discarded() || !j.is_object() || j.value("v", 0) != 1 || !j.contains("parts") ||
	    !j["parts"].is_array() || j["parts"].size() != PARTS)
		return false;
	const int w = j.value("w", 0), h = j.value("h", 0);
	if (w < 32 || h < 32 || w > 4096 || h > 4096)
		return false;
	// Rebuilt as a picture with just the three patches in it, and set up
	// from that: the same checks as a new set-up.
	std::vector<uint8_t> canvas((size_t)w * h, 0);
	Box boxes[PARTS];
	for (int i = 0; i < PARTS; i++) {
		const json &p = j["parts"][i];
		if (!p.is_object())
			return false;
		const int pw = p.value("w", 0), ph = p.value("h", 0);
		const int x0 = (int)std::lround(p.value("x", -1.0)), y0 = (int)std::lround(p.value("y", -1.0));
		std::vector<uint8_t> bytes;
		if (pw < 1 || ph < 1 || x0 < 0 || y0 < 0 || x0 + pw > w || y0 + ph > h ||
		    !decode(p.value("px", std::string()), bytes) || bytes.size() != (size_t)pw * ph)
			return false;
		for (int y = 0; y < ph; y++)
			std::copy_n(&bytes[(size_t)y * pw], pw, &canvas[(size_t)(y0 + y) * w + x0]);
		boxes[i] = {x0 / (float)w, y0 / (float)h, pw / (float)w, ph / (float)h};
	}
	return take(canvas.data(), w, h, boxes);
}

void Tracker::forget()
{
	have_last_ = false;
	misses_ = 0;
	looks_lost_ = 0;
	turned_for_ = 0;
	turned_seen_ = 0;
	face_ = Face();
}

Tracker::Hit Tracker::search(int part, const uint8_t *grey, int w, int h, float px, float py, float radius,
			     float scale, float angle, int step) const
{
	Hit best;
	const Warped t = warp(ref_[part].original, ref_[part].learned, scale, angle);
	if (!t.ok)
		return best;
	const int r = std::max(1, (int)std::ceil(radius));
	const int cx = (int)std::lround(px), cy = (int)std::lround(py);
	int bx = cx, by = cy;
	for (int y = cy - r; y <= cy + r; y += step)
		for (int x = cx - r; x <= cx + r; x += step) {
			const float z = zncc(t, grey, w, h, x, y);
			if (z > best.z) {
				best.z = z;
				bx = x;
				by = y;
			}
		}
	if (best.z <= -1)
		return best;
	if (step > 1) {
		const int ox = bx, oy = by;
		for (int y = oy - step + 1; y < oy + step; y++)
			for (int x = ox - step + 1; x < ox + step; x++) {
				if (x == ox && y == oy)
					continue;
				const float z = zncc(t, grey, w, h, x, y);
				if (z > best.z) {
					best.z = z;
					bx = x;
					by = y;
				}
			}
	}
	best.x = bx + peak(zncc(t, grey, w, h, bx - 1, by), best.z, zncc(t, grey, w, h, bx + 1, by));
	best.y = by + peak(zncc(t, grey, w, h, bx, by - 1), best.z, zncc(t, grey, w, h, bx, by + 1));
	return best;
}

bool Tracker::fit(const Hit hits[PARTS], const bool use[PARTS], float &s, float &a, float &tx, float &ty) const
{
	double sw = 0, qx = 0, qy = 0, px = 0, py = 0;
	for (int i = 0; i < PARTS; i++) {
		if (!use[i])
			continue;
		const double wt = std::max(0.1f, hits[i].z);
		sw += wt;
		qx += wt * ref_[i].cx;
		qy += wt * ref_[i].cy;
		px += wt * hits[i].x;
		py += wt * hits[i].y;
	}
	if (sw <= 0)
		return false;
	qx /= sw;
	qy /= sw;
	px /= sw;
	py /= sw;
	double A = 0, B = 0, D = 0;
	for (int i = 0; i < PARTS; i++) {
		if (!use[i])
			continue;
		const double wt = std::max(0.1f, hits[i].z);
		const double Qx = ref_[i].cx - qx, Qy = ref_[i].cy - qy;
		const double Px = hits[i].x - px, Py = hits[i].y - py;
		A += wt * (Qx * Px + Qy * Py);
		B += wt * (Qx * Py - Qy * Px);
		D += wt * (Qx * Qx + Qy * Qy);
	}
	if (D < 1e-6)
		return false;
	s = (float)(std::sqrt(A * A + B * B) / D);
	a = (float)std::atan2(B, A);
	const float c = std::cos(a), sn = std::sin(a);
	tx = (float)(px - s * (c * qx - sn * qy));
	ty = (float)(py - s * (sn * qx + c * qy));
	return std::isfinite(s) && s > 0;
}

bool Tracker::stretch(const Hit hits[PARTS], Shape &out) const
{
	// The set-up's own two ways: eye to eye, and at right angles down.
	const float ux = (ref_[1].cx - ref_[0].cx) / eye_gap_, uy = (ref_[1].cy - ref_[0].cy) / eye_gap_;
	const float vx = -uy, vy = ux;
	float qcx = 0, qcy = 0, pcx = 0, pcy = 0;
	for (int i = 0; i < PARTS; i++) {
		qcx += ref_[i].cx / PARTS;
		qcy += ref_[i].cy / PARTS;
		pcx += hits[i].x / PARTS;
		pcy += hits[i].y / PARTS;
	}
	// The straight-line map M from the set-up's (across, down) to the
	// picture: M = (sum P Q^T)(sum Q Q^T)^-1.
	double qq[4] = {0, 0, 0, 0}, pq[4] = {0, 0, 0, 0};
	for (int i = 0; i < PARTS; i++) {
		const double dx = ref_[i].cx - qcx, dy = ref_[i].cy - qcy;
		const double qa = dx * ux + dy * uy, qd = dx * vx + dy * vy;
		const double px = hits[i].x - pcx, py = hits[i].y - pcy;
		qq[0] += qa * qa;
		qq[1] += qa * qd;
		qq[3] += qd * qd;
		pq[0] += px * qa;
		pq[1] += px * qd;
		pq[2] += py * qa;
		pq[3] += py * qd;
	}
	qq[2] = qq[1];
	const double det = qq[0] * qq[3] - qq[1] * qq[2];
	if (std::fabs(det) < 1e-6)
		return false;
	const double i0 = qq[3] / det, i1 = -qq[1] / det, i3 = qq[0] / det;
	const double m11 = pq[0] * i0 + pq[1] * i1, m12 = pq[0] * i1 + pq[1] * i3;
	const double m21 = pq[2] * i0 + pq[3] * i1, m22 = pq[2] * i1 + pq[3] * i3;
	// Split into a turn, a size each way and a lean.
	const double across = std::hypot(m11, m21);
	if (across < 1e-6)
		return false;
	const double c = m11 / across, sn = m21 / across;
	const double lean = c * m12 + sn * m22, down = -sn * m12 + c * m22;
	if (down <= 1e-6)
		return false; // flipped over: not a face
	out.across = (float)across;
	out.down = (float)down;
	out.lean = (float)(lean / across);
	out.angle = (float)(std::atan2(sn, c) - std::atan2(uy, ux));
	return std::isfinite(out.angle);
}

bool Tracker::accept(const Hit hits[PARTS], const uint8_t *grey, int w, int h, float strict, float shape)
{
	const float k = (float)w / setup_w_;
	struct Try {
		bool use[PARTS];
		bool pair;
	};
	const Try tries[] = {{{true, true, true}, false},
			     {{true, true, false}, true},
			     {{true, false, true}, true},
			     {{false, true, true}, true}};
	// Each way of reading it that holds up; the surest one wins (a wrong
	// mouth that still just fits loses to the two eyes alone).
	bool any = false;
	float best_conf = -1, best_s = 1, best_a = 0, best_x[PARTS], best_y[PARTS];
	int best_turned = 0;
	for (const Try &t : tries) {
		// Pairs only near where it was: two parts can fit anywhere.
		if (t.pair && !have_last_)
			continue;
		float zmin = 1;
		bool ok = true;
		for (int i = 0; i < PARTS; i++)
			if (t.use[i]) {
				zmin = std::min(zmin, hits[i].z);
				if (hits[i].z < (t.pair ? PAIR_Z : strict))
					ok = false;
			}
		if (!ok)
			continue;
		float s, a, tx, ty;
		if (!fit(hits, t.use, s, a, tx, ty))
			continue;
		float fx[PARTS], fy[PARTS], worst = 0;
		int turned = 0;
		if (t.pair) {
			// Two parts: the set-up's shape, moved, sized and tilted.
			const float c = std::cos(a), sn = std::sin(a);
			const float gap = eye_gap_ * s;
			for (int i = 0; i < PARTS; i++) {
				fx[i] = s * (c * ref_[i].cx - sn * ref_[i].cy) + tx;
				fy[i] = s * (sn * ref_[i].cx + c * ref_[i].cy) + ty;
				if (t.use[i])
					worst = std::max(worst, std::hypot(fx[i] - hits[i].x, fy[i] - hits[i].y) / gap / shape);
			}
		} else {
			// All three: a real head also nods and turns, which squashes
			// the face up and down or across. So the shape may stretch
			// along the face's own two ways (eye to eye, and down to the
			// mouth) within limits, and lean a little - more than that
			// isn't a face.
			Shape f;
			if (!stretch(hits, f))
				continue;
			s = std::sqrt(f.across * f.down);
			a = f.angle;
			const float limit = shape / SHAPE;
			worst = std::max(std::fabs(std::log(f.down / f.across)) / (std::log(1.45f) * limit),
					 std::fabs(f.lean) / (0.3f * limit));
			for (int i = 0; i < PARTS; i++) {
				fx[i] = hits[i].x;
				fy[i] = hits[i].y;
			}
			// Turned: narrower across than down, and the mouth leaning
			// towards the side it turned to (along the line from the first
			// eye to the second: with the eyes the camera's way round,
			// that's the picture's right).
			if (f.across / f.down < 0.88f && std::fabs(f.lean) > 0.06f)
				turned = f.lean > 0 ? 1 : -1;
		}
		const float scale = s / k;
		if (scale < MIN_SCALE || scale > MAX_SCALE || std::fabs(a) > MAX_ANGLE)
			continue;
		if (have_last_ && (s / last_scale_ < 0.8f || s / last_scale_ > 1.25f ||
				   std::fabs(a - last_angle_) > (t.pair ? 0.2f : 0.35f)))
			continue;
		bool inside = true;
		for (int i = 0; i < PARTS; i++)
			if (fx[i] < 0 || fy[i] < 0 || fx[i] >= w || fy[i] >= h)
				inside = false;
		if (worst > 1 || !inside)
			continue;
		const float conf = clampf((zmin - PART_Z) / 0.35f, 0, 1) * (1 - 0.5f * worst) * (t.pair ? 0.6f : 1);
		if (conf <= best_conf)
			continue;
		any = true;
		best_conf = conf;
		best_s = s;
		best_a = a;
		best_turned = turned;
		for (int i = 0; i < PARTS; i++) {
			best_x[i] = fx[i];
			best_y[i] = fy[i];
		}
	}
	if (!any)
		return false;

	face_.found = true;
	face_.confidence = best_conf;
	float sx = 0, sy = 0;
	for (int i = 0; i < PARTS; i++) {
		face_.x[i] = best_x[i] / w;
		face_.y[i] = best_y[i] / h;
		sx += best_x[i];
		sy += best_y[i];
		last_x_[i] = best_x[i];
		last_y_[i] = best_y[i];
	}
	face_.cx = sx / PARTS / w;
	face_.cy = sy / PARTS / h;
	face_.scale = best_s / k;
	face_.angle = best_a;

	const int turned = best_turned;
	if (turned == turned_seen_)
		turned_for_++;
	else {
		turned_seen_ = turned;
		turned_for_ = 1;
	}
	if (turned_for_ >= 3)
		face_.turned = turned_seen_;

	have_last_ = true;
	last_scale_ = best_s;
	last_angle_ = best_a;
	last_w_ = w;
	last_h_ = h;
	misses_ = 0;
	looks_lost_ = 0;
	if (face_.confidence > 0.75f)
		learn(grey, w, h);
	return true;
}

void Tracker::learn(const uint8_t *grey, int w, int h)
{
	const float s = last_scale_, c = std::cos(last_angle_), sn = std::sin(last_angle_);
	for (int i = 0; i < PARTS; i++) {
		Patch &l = ref_[i].learned;
		const float hw = l.w * 0.5f, hh = l.h * 0.5f;
		for (int y = 0; y < l.h; y++)
			for (int x = 0; x < l.w; x++) {
				const float qx = x + 0.5f - hw, qy = y + 0.5f - hh;
				const float px = last_x_[i] + s * (c * qx - sn * qy);
				const float py = last_y_[i] + s * (sn * qx + c * qy);
				float &v = l.px[(size_t)y * l.w + x];
				v = v * 0.95f + sample(grey, w, h, px, py) * 0.05f;
			}
	}
}

void Tracker::find_anywhere(const uint8_t *grey, int w, int h)
{
	const float k = (float)w / setup_w_;
	struct Seed {
		float x, y, z, s;
		int part;
	};
	// Looked over at half size (a quarter of the work), then each likely
	// spot checked properly.
	const int hw = w / 2, hh = h / 2;
	half_.resize((size_t)hw * hh);
	for (int y = 0; y < hh; y++)
		for (int x = 0; x < hw; x++) {
			const uint8_t *p = grey + (size_t)(y * 2) * w + x * 2;
			half_[(size_t)y * hw + x] = (uint8_t)((p[0] + p[1] + p[w] + p[w + 1] + 2) >> 2);
		}
	std::vector<Seed> seeds;
	for (int part = FIRST_EYE; part <= SECOND_EYE; part++) {
		for (float scale : {0.55f, 0.72f, 0.92f, 1.18f, 1.5f}) {
			const float s = scale * k;
			const Warped t = warp(ref_[part].original, ref_[part].learned, s * 0.5f, 0);
			if (!t.ok)
				continue;
			// The best spot in each cell, so one strong patch can't fill
			// the list.
			const int cell = std::max(4, (int)(eye_gap_ * s * 0.25f));
			for (int cy = 0; cy < hh; cy += cell)
				for (int cx = 0; cx < hw; cx += cell) {
					Seed best{0, 0, FIND_Z - 0.1f, s, part};
					bool any = false;
					for (int y = cy; y < std::min(hh, cy + cell); y++)
						for (int x = cx; x < std::min(hw, cx + cell); x++) {
							const float z = zncc(t, half_.data(), hw, hh, x, y);
							if (z > best.z) {
								best.x = x * 2.0f + 0.5f;
								best.y = y * 2.0f + 0.5f;
								best.z = z;
								any = true;
							}
						}
					if (any)
						seeds.push_back(best);
				}
		}
	}
	std::sort(seeds.begin(), seeds.end(), [](const Seed &a, const Seed &b) { return a.z > b.z; });
	if (seeds.size() > 24)
		seeds.resize(24);
	const bool all[PARTS] = {true, true, true};
	for (const Seed &seed : seeds) {
		// The other two where the set-up says they'd be from this one...
		Hit hits[PARTS];
		const float gap = eye_gap_ * seed.s;
		bool likely = true;
		for (int i = 0; i < PARTS && likely; i++) {
			const float px = seed.x + seed.s * (ref_[i].cx - ref_[seed.part].cx);
			const float py = seed.y + seed.s * (ref_[i].cy - ref_[seed.part].cy);
			hits[i] = search(i, grey, w, h, px, py, i == seed.part ? 3 : 0.35f * gap, seed.s, 0, 2);
			likely = hits[i].z >= PART_Z;
		}
		float s, a, tx, ty;
		if (!likely || !fit(hits, all, s, a, tx, ty))
			continue;
		// ...then each looked for again at the size and tilt all three
		// say, so a seed from a rough size still lands.
		const float c = std::cos(a), sn = std::sin(a);
		for (int i = 0; i < PARTS; i++) {
			const float px = s * (c * ref_[i].cx - sn * ref_[i].cy) + tx;
			const float py = s * (sn * ref_[i].cx + c * ref_[i].cy) + ty;
			Hit best;
			for (float ds : {0.93f, 1.0f, 1.07f}) {
				const Hit hit = search(i, grey, w, h, px, py, std::max(2.0f, 0.12f * eye_gap_ * s), s * ds, a, 1);
				if (hit.z > best.z)
					best = hit;
			}
			hits[i] = best;
		}
		if (accept(hits, grey, w, h, FIND_Z, FIND_SHAPE))
			return;
	}
}

const Face &Tracker::update(const uint8_t *grey, int w, int h)
{
	face_.found = false;
	face_.confidence = 0;
	if (!ready_ || !grey || w < 32 || h < 32)
		return face_;
	if (have_last_ && (w != last_w_ || h != last_h_))
		forget();
	bool found = false;
	if (have_last_) {
		const float gap = eye_gap_ * last_scale_;
		const float radius = std::max(4.0f, 0.45f * gap);
		Hit hits[PARTS];
		for (int i = 0; i < PARTS; i++) {
			const Hit coarse = search(i, grey, w, h, last_x_[i], last_y_[i], radius, last_scale_, last_angle_, 2);
			Hit best = coarse;
			if (coarse.z > -1) {
				for (float ds : {0.93f, 1.0f, 1.07f})
					for (float da : {-0.1f, 0.0f, 0.1f}) {
						if (ds == 1.0f && da == 0.0f)
							continue;
						const Hit hit = search(i, grey, w, h, coarse.x, coarse.y, 1.5f, last_scale_ * ds,
								       last_angle_ + da, 1);
						if (hit.z > best.z)
							best = hit;
					}
			}
			hits[i] = best;
		}
		found = accept(hits, grey, w, h, PART_Z, SHAPE);
		if (!found) {
			misses_++;
			// Not found for a moment: start from nothing.
			if (misses_ > 6)
				have_last_ = false;
		} else if (face_.confidence < SURE) {
			// Following something only a bit like the face for a while:
			// it may not be the face at all. Start again, strictly.
			if (++unsure_ > 10) {
				have_last_ = false;
				unsure_ = 0;
			}
		} else {
			unsure_ = 0;
		}
	} else {
		misses_++;
	}
	if (!found && !have_last_) {
		if (looks_lost_++ % FIND_EVERY == 0)
			find_anywhere(grey, w, h);
	}
	return face_;
}

void Smoother::reset()
{
	started_ = false;
	unsure_for_ = 0;
	sure_looks_ = 0;
	out_ = Out();
}

Smoother::Out Smoother::step(const Face &face, float dt, float seconds, bool new_look)
{
	dt = clampf(dt, 0, 0.5f);
	const bool sure = face.found && face.confidence >= SURE;
	if (new_look)
		sure_looks_ = sure ? sure_looks_ + 1 : 0;
	if (sure && !started_) {
		started_ = true;
		out_.cx = face.cx;
		out_.cy = face.cy;
		out_.scale = face.scale;
		out_.angle = face.angle;
		for (int i = 0; i < PARTS; i++) {
			out_.x[i] = face.x[i];
			out_.y[i] = face.y[i];
		}
	}
	// Only a sure look moves anything: an unsure one may be a piece of
	// the room, so everything holds where it was.
	if (sure) {
		// The pose follows gently; the cut-outs stay on the face.
		const float slow = 1 - std::exp(-dt / std::max(0.05f, seconds / 3));
		const float quick = 1 - std::exp(-dt / 0.04f);
		out_.cx += (face.cx - out_.cx) * slow;
		out_.cy += (face.cy - out_.cy) * slow;
		out_.scale += (face.scale - out_.scale) * slow;
		out_.angle += (face.angle - out_.angle) * slow;
		for (int i = 0; i < PARTS; i++) {
			out_.x[i] += (face.x[i] - out_.x[i]) * quick;
			out_.y[i] += (face.y[i] - out_.y[i]) * quick;
		}
		out_.turned = face.turned;
	}
	if (sure && (out_.shown > 0 || sure_looks_ >= SURE_LOOKS)) {
		unsure_for_ = 0;
		out_.shown = std::min(1.0f, out_.shown + dt / 0.15f);
	} else {
		unsure_for_ += dt;
		// Held still for a moment, then the avatar's own face.
		if (unsure_for_ > 0.4f)
			out_.shown = std::max(0.0f, out_.shown - dt / 0.3f);
		if (unsure_for_ > 2.0f) {
			const float back = 1 - std::exp(-dt / 1.5f);
			out_.angle -= out_.angle * back;
			out_.turned = 0;
		}
	}
	return out_;
}

} // namespace direbot::face
