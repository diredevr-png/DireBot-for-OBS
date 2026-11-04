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

#include "avatar2d.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace direbot::face {

namespace {

float clamp01(float v)
{
	return v < 0 ? 0 : v > 1 ? 1 : v;
}

float smooth(float t)
{
	t = clamp01(t);
	return t * t * (3 - 2 * t);
}

// ---- colour ----

float linear_of(float c)
{
	return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float srgb_of(float c)
{
	c = std::max(0.0f, c);
	return clamp01(c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1 / 2.4f) - 0.055f);
}

constexpr float WHITE_X = 0.95047f, WHITE_Y = 1.0f, WHITE_Z = 1.08883f;
constexpr float EDGE = 6.0f / 29;

float f_of(float t)
{
	return t > EDGE * EDGE * EDGE ? std::cbrt(t) : t / (3 * EDGE * EDGE) + 4.0f / 29;
}

float f_back(float t)
{
	return t > EDGE ? t * t * t : 3 * EDGE * EDGE * (t - 4.0f / 29);
}

// ---- the outlines (MediaPipe's point numbers, round each one) ----

// Your right eye (on the picture's left in a camera that doesn't mirror)
// and left eye: along the lower lid from the outer corner, then back
// along the upper one.
const int RIGHT_EYE_LINE[] = {33, 7, 163, 144, 145, 153, 154, 155, 133, 173, 157, 158, 159, 160, 161, 246};
const int LEFT_EYE_LINE[] = {263, 249, 390, 373, 374, 380, 381, 382, 362, 398, 384, 385, 386, 387, 388, 466};
// The lips' outer edge, from your right corner along the lower lip.
const int LIPS_LINE[] = {61, 146, 91, 181, 84, 17, 314, 405, 321, 375, 291, 409, 270, 269, 267, 0, 37, 39, 40, 185};
constexpr int EYE_LINE = 16, LIPS = 20;

// How big a piece is, against how far apart the eyes are.
constexpr float EYE_PIECE = 0.78f, MOUTH_PIECE = 1.3f;
// How far above the line between its corners an eye's middle is (a share
// of how far apart they are).
constexpr float EYE_LIFT = 0.06f;

struct P2 {
	float x = 0, y = 0;
};

float to_segment(P2 p, P2 a, P2 b)
{
	const float dx = b.x - a.x, dy = b.y - a.y;
	const float len = dx * dx + dy * dy;
	float t = len > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len : 0;
	t = clamp01(t);
	return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
}

// How far p is from the outline: below 0 inside it.
float from_outline(const P2 *line, int n, P2 p)
{
	float best = std::numeric_limits<float>::max();
	bool inside = false;
	for (int i = 0, j = n - 1; i < n; j = i++) {
		best = std::min(best, to_segment(p, line[j], line[i]));
		if ((line[i].y > p.y) != (line[j].y > p.y) &&
		    p.x < (line[j].x - line[i].x) * (p.y - line[i].y) / (line[j].y - line[i].y) + line[i].x)
			inside = !inside;
	}
	return inside ? -best : best;
}

P2 middle_of(const Face &face, const int *line, int n)
{
	P2 m;
	for (int i = 0; i < n; i++) {
		m.x += face.points[line[i]].x;
		m.y += face.points[line[i]].y;
	}
	m.x /= n;
	m.y /= n;
	return m;
}

Point middle3(const Face &face, const int *line, int n)
{
	Point m;
	for (int i = 0; i < n; i++) {
		m.x += face.points[line[i]].x;
		m.y += face.points[line[i]].y;
		m.z += face.points[line[i]].z;
	}
	m.x /= n;
	m.y /= n;
	m.z /= n;
	return m;
}

float apart(const Point &a, const Point &b)
{
	return std::hypot(a.x - b.x, a.y - b.y);
}

// One piece: middle `c` and size (pixels), turned by `angle`; its mask
// from `line` (n points), `margin` out from it, then fading over `fade`.
void make_piece(const Face &face, P2 c, float pw, float ph, float angle, const int *line, int n, float margin,
		float fade, Piece &out)
{
	const float W = (float)face.w, H = (float)face.h;
	out.x = (c.x + 0.5f) / W;
	out.y = (c.y + 0.5f) / H;
	out.w = pw / W;
	out.h = ph / W;
	out.angle = angle;
	// The outline in the piece's own terms (its middle at 0, straight).
	P2 local[LIPS];
	const float co = std::cos(angle), si = std::sin(angle);
	for (int i = 0; i < n; i++) {
		const float dx = face.points[line[i]].x - c.x, dy = face.points[line[i]].y - c.y;
		local[i] = {co * dx + si * dy, -si * dx + co * dy};
	}
	fade = std::max(fade, 0.5f);
	// Inside the outline, softly over about a point of the mask either side.
	const float edge = std::max(0.5f, pw / MASK);
	for (int j = 0; j < MASK; j++) {
		const float v = (j + 0.5f) / MASK;
		for (int i = 0; i < MASK; i++) {
			const float u = (i + 0.5f) / MASK;
			const float d = from_outline(local, n, P2{(u - 0.5f) * pw, (v - 0.5f) * ph});
			float a = smooth((margin + fade - d) / fade);
			// Never a hard edge at the piece's own sides.
			const float r = std::hypot(2 * u - 1, 2 * v - 1);
			a *= smooth((1.02f - r) / 0.22f);
			out.mask[j * MASK + i] = (uint8_t)std::lround(a * 255);
			out.inner[j * MASK + i] = (uint8_t)std::lround(smooth((edge - d) / (2 * edge)) * 255);
		}
	}
}

// A point of the picture, smoothly between its pixels (0 to 1 each).
void sample(const Frame &f, float x, float y, float rgb[3])
{
	x = std::min(std::max(x, 0.0f), (float)f.w - 1);
	y = std::min(std::max(y, 0.0f), (float)f.h - 1);
	const int x0 = (int)x, y0 = (int)y;
	const int x1 = std::min(x0 + 1, f.w - 1), y1 = std::min(y0 + 1, f.h - 1);
	const float tx = x - x0, ty = y - y0;
	const int red = f.bgra ? 2 : 0, blue = f.bgra ? 0 : 2;
	auto at = [&](int px, int py, int ch) {
		return (float)f.px[(size_t)py * f.stride + (size_t)px * 4 + ch];
	};
	const int order[3] = {red, 1, blue};
	for (int k = 0; k < 3; k++) {
		const int ch = order[k];
		const float top = at(x0, y0, ch) * (1 - tx) + at(x1, y0, ch) * tx;
		const float bottom = at(x0, y1, ch) * (1 - tx) + at(x1, y1, ch) * tx;
		rgb[k] = (top * (1 - ty) + bottom * ty) / 255;
	}
}

float median(std::vector<float> &v)
{
	if (v.empty())
		return 0;
	auto mid = v.begin() + v.size() / 2;
	std::nth_element(v.begin(), mid, v.end());
	return *mid;
}

// Squared distances to the nearest 0 along one line (Felzenszwalb and
// Huttenlocher's transform): `f` in (0 or very big), `d` out, n long.
void distance_line(const float *f, float *d, int n, std::vector<int> &v, std::vector<double> &z)
{
	const double BIG = 1e30;
	v.assign(n, 0);
	z.assign(n + 1, 0);
	auto meet = [&](int q, int p) {
		return (((double)f[q] + (double)q * q) - ((double)f[p] + (double)p * p)) / (2.0 * (q - p));
	};
	int k = 0;
	z[0] = -BIG;
	z[1] = BIG;
	for (int q = 1; q < n; q++) {
		double s = meet(q, v[k]);
		while (s <= z[k]) {
			k--;
			s = meet(q, v[k]);
		}
		k++;
		v[k] = q;
		z[k] = s;
		z[k + 1] = BIG;
	}
	k = 0;
	for (int q = 0; q < n; q++) {
		while (z[k + 1] < q)
			k++;
		const double dq = q - v[k];
		d[q] = (float)std::min(1e20, dq * dq + f[v[k]]);
	}
}

void box_blur(std::vector<float> &px, int w, int h, int r)
{
	if (r <= 0)
		return;
	std::vector<float> tmp(px.size());
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			float sum = 0;
			int n = 0;
			for (int i = std::max(0, x - r); i <= std::min(w - 1, x + r); i++, n++)
				sum += px[(size_t)y * w + i];
			tmp[(size_t)y * w + x] = sum / n;
		}
	}
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			float sum = 0;
			int n = 0;
			for (int j = std::max(0, y - r); j <= std::min(h - 1, y + r); j++, n++)
				sum += tmp[(size_t)j * w + x];
			px[(size_t)y * w + x] = sum / n;
		}
	}
}

int expression(const char *name)
{
	for (int i = 0; i < EXPRESSIONS; i++)
		if (std::strcmp(EXPRESSION_NAMES[i], name) == 0)
			return i;
	return 0;
}

} // namespace

// ---- colour ----

Lab lab_of(float r, float g, float b)
{
	r = linear_of(r);
	g = linear_of(g);
	b = linear_of(b);
	const float X = (0.4124564f * r + 0.3575761f * g + 0.1804375f * b) / WHITE_X;
	const float Y = (0.2126729f * r + 0.7151522f * g + 0.0721750f * b) / WHITE_Y;
	const float Z = (0.0193339f * r + 0.1191920f * g + 0.9503041f * b) / WHITE_Z;
	const float fx = f_of(X), fy = f_of(Y), fz = f_of(Z);
	return Lab{116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz)};
}

void rgb_of(const Lab &lab, float rgb[3])
{
	const float fy = (lab.l + 16) / 116, fx = fy + lab.a / 500, fz = fy - lab.b / 200;
	const float X = WHITE_X * f_back(fx), Y = WHITE_Y * f_back(fy), Z = WHITE_Z * f_back(fz);
	rgb[0] = srgb_of(3.2404542f * X - 1.5371385f * Y - 0.4985314f * Z);
	rgb[1] = srgb_of(-0.9692660f * X + 1.8760108f * Y + 0.0415560f * Z);
	rgb[2] = srgb_of(0.0556434f * X - 0.2040259f * Y + 1.0572252f * Z);
}

void skin_blend(const float rgb[3], const Lab &skin, const Lab &target, float amount, float inner, float out[3])
{
	const Lab c = lab_of(rgb[0], rgb[1], rgb[2]);
	const float w = clamp01(amount) * (1 - clamp01(inner));
	float turned[3];
	rgb_of(Lab{target.l + (c.l - skin.l) * 0.9f, target.a, target.b}, turned);
	for (int k = 0; k < 3; k++)
		out[k] = rgb[k] + (turned[k] - rgb[k]) * w;
}

// ---- your real eyes and mouth ----

void cut_pieces(const Face &face, const float aspect[PARTS], float soft, Pieces &out)
{
	out.found = face.found && face.w > 0 && face.h > 0;
	if (!out.found)
		return;
	soft = clamp01(soft);
	// Each eye's middle from its corners (they hold still as it blinks).
	const int RIGHT_CORNERS[] = {RIGHT_EYE_OUTER, RIGHT_EYE_INNER},
		  LEFT_CORNERS[] = {LEFT_EYE_OUTER, LEFT_EYE_INNER};
	const P2 right = middle_of(face, RIGHT_CORNERS, 2), left = middle_of(face, LEFT_CORNERS, 2);
	const P2 lips = middle_of(face, LIPS_LINE, LIPS);
	// How far apart the eyes are in 3D, so a piece keeps its size as you
	// turn (z is on x's scale).
	const Point r3 = middle3(face, RIGHT_CORNERS, 2), l3 = middle3(face, LEFT_CORNERS, 2);
	const float gap = std::max(1.0f, std::sqrt((r3.x - l3.x) * (r3.x - l3.x) + (r3.y - l3.y) * (r3.y - l3.y) +
						   (r3.z - l3.z) * (r3.z - l3.z)));
	out.gap = gap / face.w;
	const bool right_first = right.x <= left.x;
	const P2 first = right_first ? right : left, second = right_first ? left : right;
	const float angle = std::atan2(second.y - first.y, second.x - first.x);
	const float right_w = apart(face.points[RIGHT_EYE_OUTER], face.points[RIGHT_EYE_INNER]);
	const float left_w = apart(face.points[LEFT_EYE_OUTER], face.points[LEFT_EYE_INNER]);
	const float mouth_w = apart(face.points[MOUTH_RIGHT], face.points[MOUTH_LEFT]);
	const float lips_h = apart(face.points[UPPER_LIP_TOP], face.points[LOWER_LIP_BOTTOM]);

	auto eye = [&](int part, P2 c, const int *line, float eye_w) {
		// The eye's opening sits a little above the line between its
		// corners (its upper lid curves higher than its lower one).
		c.x += std::sin(angle) * EYE_LIFT * eye_w;
		c.y -= std::cos(angle) * EYE_LIFT * eye_w;
		float pw = EYE_PIECE * gap, ph = pw * std::max(0.1f, aspect[part]);
		const float need = std::max(1.0f, eye_w * 1.45f / pw);
		pw *= need;
		ph *= need;
		make_piece(face, c, pw, ph, angle, line, EYE_LINE, 0.10f * eye_w, (0.08f + 0.30f * soft) * eye_w,
			   out.part[part]);
		out.part[part].grown = need;
	};
	eye(FIRST_EYE, first, right_first ? RIGHT_EYE_LINE : LEFT_EYE_LINE, right_first ? right_w : left_w);
	eye(SECOND_EYE, second, right_first ? LEFT_EYE_LINE : RIGHT_EYE_LINE, right_first ? left_w : right_w);
	float pw = MOUTH_PIECE * gap, ph = pw * std::max(0.1f, aspect[MOUTH]);
	// A wide-open or wide mouth stays whole: the piece takes in more.
	const float need = std::max(1.0f, std::max(lips_h * 1.7f / ph, mouth_w * 1.35f / pw));
	pw *= need;
	ph *= need;
	make_piece(face, lips, pw, ph, angle, LIPS_LINE, LIPS, 0.05f * mouth_w, (0.06f + 0.22f * soft) * mouth_w,
		   out.part[MOUTH]);
	out.part[MOUTH].grown = need;
}

void piece_point(const Piece &p, int picture_w, int picture_h, float u, float v, float &x, float &y)
{
	const float W = (float)picture_w, H = (float)picture_h;
	const float lx = (u - 0.5f) * p.w * W, ly = (v - 0.5f) * p.h * W;
	const float co = std::cos(p.angle), si = std::sin(p.angle);
	x = (p.x * W + co * lx - si * ly) / W;
	y = (p.y * H + si * lx + co * ly) / H;
}

void skin_round(const Frame &picture, Pieces &pieces)
{
	if (!pieces.found || !picture.px || picture.w <= 0 || picture.h <= 0)
		return;
	constexpr int AROUND = 64;
	std::vector<float> l, a, b;
	for (Piece &p : pieces.part) {
		l.clear();
		a.clear();
		b.clear();
		for (int i = 0; i < AROUND; i++) {
			const float t = 6.2831853f * i / AROUND;
			float x, y, rgb[3];
			piece_point(p, picture.w, picture.h, 0.5f + 0.45f * std::cos(t), 0.5f + 0.45f * std::sin(t), x,
				    y);
			sample(picture, x * picture.w - 0.5f, y * picture.h - 0.5f, rgb);
			const Lab c = lab_of(rgb[0], rgb[1], rgb[2]);
			l.push_back(c.l);
			a.push_back(c.a);
			b.push_back(c.b);
		}
		p.skin = Lab{median(l), median(a), median(b)};
	}
}

// ---- the character ----

Shape shape_of(const uint8_t *rgba, int w, int h, int stride, bool bgra, const Box spots[PARTS], bool have_spots)
{
	Shape s;
	if (!rgba || w <= 0 || h <= 0)
		return s;
	// Worked on at most this many points across.
	constexpr int MOST = 256;
	const float k = std::min(1.0f, (float)MOST / std::max(w, h));
	const int gw = std::max(1, (int)std::lround(w * k)), gh = std::max(1, (int)std::lround(h * k));
	std::vector<float> alpha((size_t)gw * gh), colour((size_t)gw * gh * 3);
	const int red = bgra ? 2 : 0, blue = bgra ? 0 : 2;
	for (int gy = 0; gy < gh; gy++) {
		const int y0 = gy * h / gh, y1 = std::max(y0 + 1, (gy + 1) * h / gh);
		for (int gx = 0; gx < gw; gx++) {
			const int x0 = gx * w / gw, x1 = std::max(x0 + 1, (gx + 1) * w / gw);
			float sa = 0, sr = 0, sg = 0, sb = 0;
			int n = 0;
			for (int y = y0; y < y1; y++) {
				const uint8_t *row = rgba + (size_t)y * stride;
				for (int x = x0; x < x1; x++, n++) {
					const uint8_t *p = row + (size_t)x * 4;
					const float a = p[3] / 255.0f;
					sa += a;
					sr += p[red] / 255.0f * a;
					sg += p[1] / 255.0f * a;
					sb += p[blue] / 255.0f * a;
				}
			}
			const size_t i = (size_t)gy * gw + gx;
			alpha[i] = sa / n;
			if (sa > 0) {
				colour[i * 3] = sr / sa;
				colour[i * 3 + 1] = sg / sa;
				colour[i * 3 + 2] = sb / sa;
			}
		}
	}
	s.w = gw;
	s.h = gh;
	s.depth.assign((size_t)gw * gh, 0);

	// How far each inside point is from the outline (the picture's own
	// edge counts as outline too).
	const int pw = gw + 2, ph = gh + 2;
	std::vector<float> f((size_t)pw * ph), d((size_t)pw * ph), line(std::max(pw, ph)), out(std::max(pw, ph));
	std::vector<int> v;
	std::vector<double> z;
	for (int y = 0; y < ph; y++)
		for (int x = 0; x < pw; x++) {
			const bool in = x > 0 && y > 0 && x <= gw && y <= gh &&
					alpha[(size_t)(y - 1) * gw + (x - 1)] > 0.5f;
			f[(size_t)y * pw + x] = in ? 1e20f : 0;
		}
	for (int x = 0; x < pw; x++) {
		for (int y = 0; y < ph; y++)
			line[y] = f[(size_t)y * pw + x];
		distance_line(line.data(), out.data(), ph, v, z);
		for (int y = 0; y < ph; y++)
			d[(size_t)y * pw + x] = out[y];
	}
	for (int y = 0; y < ph; y++) {
		distance_line(d.data() + (size_t)y * pw, out.data(), pw, v, z);
		std::copy(out.begin(), out.begin() + pw, d.begin() + (size_t)y * pw);
	}
	float most = 0;
	for (int y = 0; y < gh; y++)
		for (int x = 0; x < gw; x++)
			most = std::max(most, d[(size_t)(y + 1) * pw + x + 1]);
	most = std::sqrt(most);
	if (most >= 1) {
		// Rounded off, flat on top - not so steep at the outline that
		// turning would fold it over.
		for (int y = 0; y < gh; y++)
			for (int x = 0; x < gw; x++) {
				const float t = std::sqrt(d[(size_t)(y + 1) * pw + x + 1]) / most;
				s.depth[(size_t)y * gw + x] = t > 0 ? most * 0.85f * (1 - (1 - t) * (1 - t)) : 0;
			}
	}
	auto middle = [&](const Box &b, float &x, float &y) {
		x = (b.x + b.w / 2) * gw;
		y = (b.y + b.h / 2) * gh;
	};
	if (have_spots) {
		float lx, ly, rx, ry, mx, my;
		middle(spots[0], lx, ly);
		middle(spots[1], rx, ry);
		middle(spots[2], mx, my);
		const float ex = (lx + rx) / 2, ey = (ly + ry) / 2;
		// A nose or muzzle, just above the mouth.
		const float nx = ex + 0.75f * (mx - ex), ny = ey + 0.75f * (my - ey);
		const float r = std::max(0.55f * std::hypot(mx - ex, my - ey), 0.6f * spots[2].w * gw);
		if (r > 1)
			for (int y = 0; y < gh; y++)
				for (int x = 0; x < gw; x++) {
					float &dep = s.depth[(size_t)y * gw + x];
					const float q = 1 - ((x + 0.5f - nx) * (x + 0.5f - nx) +
							     (y + 0.5f - ny) * (y + 0.5f - ny)) /
								    (r * r);
					if (dep > 0 && q > 0)
						dep += r * 0.3f * std::sqrt(q);
				}
		s.pivot_x = ex / gw;
		s.pivot_y = (ey + my) / 2 / gh;
	}
	box_blur(s.depth, gw, gh, std::max(1, (int)std::lround(0.02f * gw)));
	for (float &dep : s.depth)
		dep /= gw;

	// Each spot: see-through or not, and the picture's colour round it.
	std::vector<float> r, g, b, all_r, all_g, all_b;
	for (size_t i = 0; i < alpha.size(); i++)
		if (alpha[i] > 0.85f) {
			all_r.push_back(colour[i * 3]);
			all_g.push_back(colour[i * 3 + 1]);
			all_b.push_back(colour[i * 3 + 2]);
		}
	const Lab whole = all_r.empty() ? lab_of(0.5f, 0.5f, 0.5f)
					: lab_of(median(all_r), median(all_g), median(all_b));
	for (int part = 0; part < PARTS; part++) {
		s.colour[part] = whole;
		if (!have_spots)
			continue;
		const Box &bx = spots[part];
		const int x0 = (int)std::floor(bx.x * gw), x1 = (int)std::ceil((bx.x + bx.w) * gw);
		const int y0 = (int)std::floor(bx.y * gh), y1 = (int)std::ceil((bx.y + bx.h) * gh);
		int clear = 0, n = 0;
		for (int y = std::max(0, y0); y < std::min(gh, y1); y++)
			for (int x = std::max(0, x0); x < std::min(gw, x1); x++, n++)
				clear += alpha[(size_t)y * gw + x] < 0.15f;
		s.hole[part] = n > 0 && clear > 0.35f * n;
		const int mx = std::max(1, (int)std::lround(bx.w * gw * 0.35f)),
			  my = std::max(1, (int)std::lround(bx.h * gh * 0.35f));
		r.clear();
		g.clear();
		b.clear();
		for (int y = std::max(0, y0 - my); y < std::min(gh, y1 + my); y++)
			for (int x = std::max(0, x0 - mx); x < std::min(gw, x1 + mx); x++) {
				if (x >= x0 && x < x1 && y >= y0 && y < y1)
					continue;
				const size_t i = (size_t)y * gw + x;
				if (alpha[i] > 0.85f) {
					r.push_back(colour[i * 3]);
					g.push_back(colour[i * 3 + 1]);
					b.push_back(colour[i * 3 + 2]);
				}
			}
		if (r.size() >= 4)
			s.colour[part] = lab_of(median(r), median(g), median(b));
	}
	return s;
}

float depth_at(const Shape &s, float x, float y)
{
	if (!s.ready())
		return 0;
	const float fx = x * s.w - 0.5f, fy = y * s.h - 0.5f;
	if (fx < -0.5f || fy < -0.5f || fx > s.w - 0.5f || fy > s.h - 0.5f)
		return 0;
	const int x0 = std::max(0, std::min(s.w - 1, (int)std::floor(fx)));
	const int y0 = std::max(0, std::min(s.h - 1, (int)std::floor(fy)));
	const int x1 = std::min(s.w - 1, x0 + 1), y1 = std::min(s.h - 1, y0 + 1);
	const float tx = clamp01(fx - x0), ty = clamp01(fy - y0);
	auto at = [&](int px, int py) {
		return s.depth[(size_t)py * s.w + px];
	};
	return (at(x0, y0) * (1 - tx) + at(x1, y0) * tx) * (1 - ty) + (at(x0, y1) * (1 - tx) + at(x1, y1) * tx) * ty;
}

void turned_point(const Shape &s, float w, float h, float yaw, float pitch, float x, float y, float &tx, float &ty)
{
	const float px = s.pivot_x * w, py = s.pivot_y * h;
	const float Z = depth_at(s, x / w, y / h) * w;
	tx = px + (x - px) * (1 - FORESHORTEN * (1 - std::cos(yaw))) + Z * std::sin(yaw);
	ty = py + (y - py) * (1 - FORESHORTEN * (1 - std::cos(pitch))) - Z * std::sin(pitch);
}

void turned_from(const Shape &s, float w, float h, float yaw, float pitch, float x, float y, float &sx, float &sy)
{
	const float px = s.pivot_x * w, py = s.pivot_y * h;
	const float kx = 1 - FORESHORTEN * (1 - std::cos(yaw)), ky = 1 - FORESHORTEN * (1 - std::cos(pitch));
	const float syaw = std::sin(yaw), spitch = std::sin(pitch);
	// One depth map point, in character pixels: how far to look along to
	// see which way the surface slopes.
	const float ex = s.ready() ? w / s.w : 1, ey = s.ready() ? h / s.h : 1;
	const float X = x - px, Y = y - py;
	// Newton's steps: from a guess, how far its landing is off and how the
	// landing moves with it (the surface's slope) say where to try next.
	// Each step is kept short, and the best guess wins (where the surface
	// folds over, no point lands exactly).
	const float most = 0.08f * w;
	float ax = X, ay = Y, bx = X, by = Y, best = 1e30f;
	for (int i = 0; i <= TURN_STEPS; i++) {
		const float gx = (ax + px) / w, gy = (ay + py) / h;
		const float Z = depth_at(s, gx, gy) * w;
		const float fx = ax * kx + Z * syaw - X, fy = ay * ky - Z * spitch - Y;
		const float off = fx * fx + fy * fy;
		if (off < best) {
			best = off;
			bx = ax;
			by = ay;
		}
		// There (within a hundredth of a pixel): the rest is skipped.
		if (i == TURN_STEPS || off < 1e-4f)
			break;
		const float zx = (depth_at(s, gx + ex / w, gy) * w - Z) / ex;
		const float zy = (depth_at(s, gx, gy + ey / h) * w - Z) / ey;
		const float a = kx + syaw * zx, b = syaw * zy, c = -spitch * zx, d = ky - spitch * zy;
		float det = a * d - b * c;
		if (std::fabs(det) < 0.15f)
			det = det < 0 ? -0.15f : 0.15f;
		float dx = (d * fx - b * fy) / det, dy = (a * fy - c * fx) / det;
		const float len = std::sqrt(dx * dx + dy * dy);
		if (len > most) {
			dx *= most / len;
			dy *= most / len;
		}
		ax -= dx;
		ay -= dy;
	}
	sx = bx + px;
	sy = by + py;
}

// ---- the drawn face ----

Drawn drawn_face(const float moves[EXPRESSIONS], bool mirror, const Drawn &before)
{
	static const int BLINK_LEFT = expression("eyeBlinkLeft"), BLINK_RIGHT = expression("eyeBlinkRight"),
			 JAW = expression("jawOpen"), FUNNEL = expression("mouthFunnel"),
			 SMILE_LEFT = expression("mouthSmileLeft"), SMILE_RIGHT = expression("mouthSmileRight");
	Drawn d;
	// The viewer's left eye is your right one - your left in a mirror.
	const float blink[2] = {moves[mirror ? BLINK_LEFT : BLINK_RIGHT], moves[mirror ? BLINK_RIGHT : BLINK_LEFT]};
	for (int e = 0; e < 2; e++) {
		const int was = before.eye[e];
		const float v = blink[e];
		d.eye[e] = v > (was == EYE_CLOSED ? 0.5f : 0.6f)  ? EYE_CLOSED
			   : v > (was >= EYE_HALF ? 0.2f : 0.3f) ? EYE_HALF
								 : EYE_OPEN;
	}
	const float jaw = moves[JAW], funnel = moves[FUNNEL];
	const float smile = (moves[SMILE_LEFT] + moves[SMILE_RIGHT]) / 2;
	const int was = before.mouth;
	const bool was_open = was == MOUTH_OPEN || was == MOUTH_WIDE || was == MOUTH_O;
	if (funnel > (was == MOUTH_O ? 0.25f : 0.35f) && jaw > 0.05f)
		d.mouth = MOUTH_O;
	else if (jaw > (was == MOUTH_WIDE ? 0.55f : 0.65f))
		d.mouth = MOUTH_WIDE;
	else if (jaw > (was_open ? 0.06f : 0.12f))
		d.mouth = MOUTH_OPEN;
	else if (smile > (was == MOUTH_SMILE ? 0.28f : 0.35f))
		d.mouth = MOUTH_SMILE;
	else
		d.mouth = MOUTH_CLOSED;
	return d;
}

namespace {

int first_of(const int *order, int n, unsigned have)
{
	for (int i = 0; i < n; i++)
		if (have & (1u << order[i]))
			return order[i];
	return -1;
}

} // namespace

int eye_picture(int look, unsigned have)
{
	static const int OPEN[] = {EYE_OPEN, EYE_HALF};
	static const int HALF[] = {EYE_HALF, EYE_CLOSED, EYE_OPEN};
	static const int CLOSED[] = {EYE_CLOSED, EYE_HALF, EYE_OPEN};
	switch (look) {
	case EYE_HALF:
		return first_of(HALF, 3, have);
	case EYE_CLOSED:
		return first_of(CLOSED, 3, have);
	default:
		return first_of(OPEN, 2, have);
	}
}

int mouth_picture(int look, unsigned have)
{
	static const int CLOSED[] = {MOUTH_CLOSED, MOUTH_SMILE};
	static const int OPEN[] = {MOUTH_OPEN, MOUTH_WIDE, MOUTH_O, MOUTH_CLOSED};
	static const int WIDE[] = {MOUTH_WIDE, MOUTH_OPEN, MOUTH_O, MOUTH_CLOSED};
	static const int SMILE[] = {MOUTH_SMILE, MOUTH_CLOSED};
	static const int O[] = {MOUTH_O, MOUTH_OPEN, MOUTH_WIDE, MOUTH_CLOSED};
	switch (look) {
	case MOUTH_OPEN:
		return first_of(OPEN, 4, have);
	case MOUTH_WIDE:
		return first_of(WIDE, 4, have);
	case MOUTH_SMILE:
		return first_of(SMILE, 2, have);
	case MOUTH_O:
		return first_of(O, 4, have);
	default:
		return first_of(CLOSED, 2, have);
	}
}

} // namespace direbot::face
