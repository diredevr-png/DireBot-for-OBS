// Unit tests for the 2D face avatar's sums (src/face/avatar2d.cpp): the
// colour sums the shader repeats, the pieces of your real eyes and mouth
// cut from the test photo (where, how big, their soft outlines, the skin
// round them), a made-up character's shape (its depth for turning, its
// see-through spots and colours), turning and nodding, and the drawn
// face's pictures.
// Build and run: tools/test-link.sh (Linux, any C++17 compiler).

#include "../src/face/avatar2d.hpp"
#include "../src/face/tracker.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#ifndef DIREBOT_SOURCE_DIR
#define DIREBOT_SOURCE_DIR "."
#endif

using namespace direbot::face;

static int failures = 0;
static int passes = 0;

#define CHECK(cond)                                                              \
	do {                                                                     \
		if (cond) {                                                      \
			passes++;                                                \
		} else {                                                         \
			failures++;                                              \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                \
	} while (0)

static bool near(double a, double b, double by)
{
	return std::fabs(a - b) <= by;
}

static const std::string ROOT = DIREBOT_SOURCE_DIR;

// --- Pictures -------------------------------------------------------------

struct Picture {
	int w = 0, h = 0;
	std::vector<uint8_t> rgba; // 4 bytes a pixel, red first
	uint8_t *at(int x, int y) { return &rgba[((size_t)y * w + x) * 4]; }
	const uint8_t *at(int x, int y) const { return &rgba[((size_t)y * w + x) * 4]; }
	Frame frame() const { return Frame{rgba.data(), w, h, w * 4, false}; }
};

static Picture read_ppm(const std::string &path)
{
	Picture p;
	std::ifstream in(path, std::ios::binary);
	std::string magic;
	int top = 0;
	in >> magic >> p.w >> p.h >> top;
	in.get();
	if (magic != "P6" || top != 255)
		return Picture{};
	std::vector<uint8_t> rgb((size_t)p.w * p.h * 3);
	in.read((char *)rgb.data(), (std::streamsize)rgb.size());
	p.rgba.assign((size_t)p.w * p.h * 4, 255);
	for (size_t i = 0; i < (size_t)p.w * p.h; i++)
		std::copy(&rgb[i * 3], &rgb[i * 3] + 3, &p.rgba[i * 4]);
	return p;
}

static const Picture &portrait()
{
	static const Picture p = read_ppm(ROOT + "/tests/data/face/portrait.ppm");
	return p;
}

static Picture mirrored(const Picture &p)
{
	Picture m = p;
	for (int y = 0; y < p.h; y++)
		for (int x = 0; x < p.w; x++)
			std::copy(p.at(p.w - 1 - x, y), p.at(p.w - 1 - x, y) + 4, m.at(x, y));
	return m;
}

static Face face_of(const Picture &p)
{
	Tracker t;
	std::string problem;
	if (!t.load_file(ROOT + "/data/face/face_landmarker.task", &problem)) {
		std::fprintf(stderr, "  can't load the face models: %s\n", problem.c_str());
		return Face{};
	}
	t.set_smoothing(-1);
	return t.look(p.frame(), 0);
}

static const Face &portrait_face()
{
	static const Face f = face_of(portrait());
	return f;
}

// A point of the picture (shares) in a piece's own terms (u, v).
static void in_piece(const Piece &p, int w, int h, float x, float y, float &u, float &v)
{
	const float W = (float)w, H = (float)h;
	const float dx = x * W - p.x * W, dy = y * H - p.y * H;
	const float co = std::cos(p.angle), si = std::sin(p.angle);
	u = (co * dx + si * dy) / (p.w * W) + 0.5f;
	v = (-si * dx + co * dy) / (p.h * W) + 0.5f;
}

static int mask_at(const Piece &p, float u, float v)
{
	if (u < 0 || v < 0 || u >= 1 || v >= 1)
		return 0;
	return p.mask[(int)(v * MASK) * MASK + (int)(u * MASK)];
}

static int mask_at_point(const Piece &p, const Face &f, int point)
{
	float u, v;
	in_piece(p, f.w, f.h, (f.points[point].x + 0.5f) / f.w, (f.points[point].y + 0.5f) / f.h, u, v);
	return mask_at(p, u, v);
}

static long mask_sum(const Piece &p)
{
	long s = 0;
	for (uint8_t m : p.mask)
		s += m;
	return s;
}

static int inner_at(const Piece &p, float u, float v)
{
	if (u < 0 || v < 0 || u >= 1 || v >= 1)
		return 0;
	return p.inner[(int)(v * MASK) * MASK + (int)(u * MASK)];
}

static int inner_at_point(const Piece &p, const Face &f, int point)
{
	float u, v;
	in_piece(p, f.w, f.h, (f.points[point].x + 0.5f) / f.w, (f.points[point].y + 0.5f) / f.h, u, v);
	return inner_at(p, u, v);
}

static long inner_sum(const Piece &p)
{
	long s = 0;
	for (uint8_t m : p.inner)
		s += m;
	return s;
}

static double colour_gap(const Lab &a, const Lab &b)
{
	return std::sqrt((a.l - b.l) * (a.l - b.l) + (a.a - b.a) * (a.a - b.a) + (a.b - b.b) * (a.b - b.b));
}

// --- Colour ----------------------------------------------------------------

static void test_colour()
{
	const Lab white = lab_of(1, 1, 1), black = lab_of(0, 0, 0), red = lab_of(1, 0, 0);
	CHECK(near(white.l, 100, 0.01) && near(white.a, 0, 0.01) && near(white.b, 0, 0.01));
	CHECK(near(black.l, 0, 0.01));
	CHECK(near(red.l, 53.24, 0.05) && near(red.a, 80.09, 0.1) && near(red.b, 67.20, 0.1));
	// There and back.
	double worst = 0;
	for (float r = 0; r <= 1.0001f; r += 0.125f)
		for (float g = 0; g <= 1.0001f; g += 0.125f)
			for (float b = 0; b <= 1.0001f; b += 0.125f) {
				float back[3];
				rgb_of(lab_of(r, g, b), back);
				worst = std::max({worst, std::fabs(back[0] - (double)r), std::fabs(back[1] - (double)g),
						  std::fabs(back[2] - (double)b)});
			}
	CHECK(worst < 1e-3);

	// Round your eye or lips, it takes the character's colour, its light
	// and shade kept (skin in shadow, a beard, an eyebrow stay darker);
	// inside the outline it keeps its own, whatever colour your eyes are.
	const float skin_rgb[3] = {0.80f, 0.60f, 0.48f};
	const Lab skin = lab_of(skin_rgb[0], skin_rgb[1], skin_rgb[2]);
	const Lab fur = lab_of(0.45f, 0.45f, 0.50f);
	float out[3];
	skin_blend(skin_rgb, skin, fur, 1, 0, out);
	CHECK(colour_gap(lab_of(out[0], out[1], out[2]), fur) < 0.5);
	const float darker[3] = {0.60f, 0.42f, 0.32f}; // skin in shadow
	skin_blend(darker, skin, fur, 1, 0, out);
	const Lab shaded = lab_of(out[0], out[1], out[2]);
	CHECK(shaded.l < fur.l - 5 && near(shaded.a, fur.a, 3) && near(shaded.b, fur.b, 3));
	const float beard[3] = {0.20f, 0.14f, 0.10f};
	skin_blend(beard, skin, fur, 1, 0, out);
	const Lab dark = lab_of(out[0], out[1], out[2]);
	CHECK(dark.l < shaded.l - 10 && near(dark.a, fur.a, 3) && near(dark.b, fur.b, 3));
	const float sclera[3] = {0.95f, 0.95f, 0.93f}, green[3] = {0.25f, 0.35f, 0.15f},
		    brown[3] = {0.45f, 0.28f, 0.16f};
	for (const float *own : {sclera, green, brown, skin_rgb}) {
		skin_blend(own, skin, fur, 1, 1, out);
		CHECK(near(out[0], own[0], 1e-4) && near(out[1], own[1], 1e-4) && near(out[2], own[2], 1e-4));
	}
	// None at all: as it was. Half way (or half inside): half way.
	skin_blend(skin_rgb, skin, fur, 0, 0, out);
	CHECK(near(out[0], skin_rgb[0], 1e-6) && near(out[2], skin_rgb[2], 1e-6));
	float full[3], half[3], edge[3];
	skin_blend(skin_rgb, skin, fur, 1, 0, full);
	skin_blend(skin_rgb, skin, fur, 0.5f, 0, half);
	skin_blend(skin_rgb, skin, fur, 1, 0.5f, edge);
	CHECK(near(half[0], (full[0] + skin_rgb[0]) / 2, 1e-4));
	CHECK(near(edge[0], half[0], 1e-6) && near(edge[2], half[2], 1e-6));
}

// --- Your real eyes and mouth ----------------------------------------------

static void test_pieces()
{
	const Face &f = portrait_face();
	CHECK(f.found);
	if (!f.found)
		return;
	const float aspect[PARTS] = {0.7f, 0.7f, 0.5f};
	Pieces ps;
	cut_pieces(f, aspect, 0.35f, ps);
	CHECK(ps.found);
	const Piece &a = ps.part[FIRST_EYE], &b = ps.part[SECOND_EYE], &m = ps.part[MOUTH];
	// The first eye is the one on the picture's left (the photo's right eye).
	CHECK(a.x < b.x);
	CHECK(near(a.x * f.w, (f.points[RIGHT_EYE_OUTER].x + f.points[RIGHT_EYE_INNER].x) / 2 + 0.5, 0.06 * f.w));
	CHECK(m.y > a.y && m.y > b.y);
	// Never stretched: as tall as the spot says.
	CHECK(near(a.h / a.w, 0.7, 1e-4) && near(b.h / b.w, 0.7, 1e-4));
	CHECK(m.h / m.w >= 0.5f - 1e-4);
	// Level with the eyes.
	const float eye_angle = std::atan2(f.points[LEFT_IRIS].y - f.points[RIGHT_IRIS].y,
					   f.points[LEFT_IRIS].x - f.points[RIGHT_IRIS].x);
	CHECK(near(a.angle, eye_angle, 0.05) && a.angle == m.angle);
	// The eye shows, its corners just inside the soft edge; the piece's own
	// sides never do.
	CHECK(mask_at_point(a, f, RIGHT_IRIS) > 230);
	// Inside the outline: the eye itself (its own colour kept), not the
	// skin round it.
	CHECK(inner_at_point(a, f, RIGHT_IRIS) > 230 && inner_at_point(b, f, LEFT_IRIS) > 230);
	CHECK(inner_at_point(a, f, 105) < 10 && inner_at(a, 0.5f, 0.05f) == 0);
	CHECK(inner_sum(a) < mask_sum(a) / 2);
	CHECK(mask_at_point(b, f, LEFT_IRIS) > 230);
	CHECK(mask_at_point(a, f, RIGHT_EYE_OUTER) > 128 && mask_at_point(a, f, RIGHT_EYE_INNER) > 128);
	CHECK(mask_at(a, 0.01f, 0.01f) == 0 && mask_at(a, 0.99f, 0.5f) == 0 && mask_at(a, 0.5f, 0.99f) == 0);
	// The eyebrow is outside the eye's outline: hidden or nearly.
	CHECK(mask_at_point(a, f, 105) < 40);
	// The lips show; the chin and nose don't.
	CHECK(mask_at_point(m, f, 13) > 230 && mask_at_point(m, f, 14) > 230);
	CHECK(mask_at_point(m, f, MOUTH_RIGHT) > 128 && mask_at_point(m, f, MOUTH_LEFT) > 128);
	CHECK(mask_at_point(m, f, 152) == 0 && mask_at_point(m, f, NOSE_TIP) < 20);

	// A softer edge shows more round the outline.
	Pieces hard, softer;
	cut_pieces(f, aspect, 0, hard);
	cut_pieces(f, aspect, 1, softer);
	CHECK(mask_sum(hard.part[FIRST_EYE]) < mask_sum(a) && mask_sum(a) < mask_sum(softer.part[FIRST_EYE]));

	// A closed eye: the outline closes, so less of it shows - but its lids
	// still do.
	Face shut = f;
	const int upper[] = {173, 157, 158, 159, 160, 161, 246};
	const int lower[] = {155, 154, 153, 145, 144, 163, 7};
	for (int i = 0; i < 7; i++)
		shut.points[upper[i]] = shut.points[lower[i]];
	Pieces closed;
	cut_pieces(shut, aspect, 0.35f, closed);
	CHECK(mask_sum(closed.part[FIRST_EYE]) < mask_sum(a) * 0.8);
	CHECK(mask_sum(closed.part[FIRST_EYE]) > mask_sum(a) * 0.2);
	CHECK(near(closed.part[FIRST_EYE].w, a.w, 1e-6)); // the same size: it doesn't zoom as you blink

	// Turned 30 degrees: the pieces keep their size (from the eyes' distance
	// in 3D, not on the picture).
	Face turned = f;
	const float cx = f.points[NOSE_TIP].x, c30 = std::cos(0.52f), s30 = std::sin(0.52f);
	for (Point &p : turned.points) {
		const float x = p.x - cx;
		p.x = cx + x * c30 + p.z * s30;
		p.z = -x * s30 + p.z * c30;
	}
	Pieces t;
	cut_pieces(turned, aspect, 0.35f, t);
	CHECK(near(t.part[FIRST_EYE].w, a.w, a.w * 0.02));

	// No face, no pieces.
	Pieces none;
	cut_pieces(Face{}, aspect, 0.35f, none);
	CHECK(!none.found);

	// A mirrored camera: still the picture's left eye first, the same size.
	const Face mf = face_of(mirrored(portrait()));
	Pieces mp;
	cut_pieces(mf, aspect, 0.35f, mp);
	CHECK(mp.found && mp.part[FIRST_EYE].x < mp.part[SECOND_EYE].x);
	CHECK(near(mp.part[FIRST_EYE].x, 1 - b.x, 0.01) && near(mp.part[FIRST_EYE].w, b.w, b.w * 0.03));
	std::printf("unit_avatar: eye piece %.0f x %.0f px, mouth %.0f x %.0f px, open eye %ld, closed %ld\n",
		    a.w * f.w, a.h * f.w, m.w * f.w, m.h * f.w, mask_sum(a), mask_sum(closed.part[FIRST_EYE]));
}

static void test_skin()
{
	const Face &f = portrait_face();
	if (!f.found)
		return;
	const float aspect[PARTS] = {0.7f, 0.7f, 0.5f};
	Pieces ps;
	cut_pieces(f, aspect, 0.35f, ps);
	skin_round(portrait().frame(), ps);
	// The skin round each eye is about the cheek's colour below it.
	auto colour_at = [&](int point) {
		const uint8_t *px = portrait().at((int)f.points[point].x, (int)f.points[point].y);
		return lab_of(px[0] / 255.0f, px[1] / 255.0f, px[2] / 255.0f);
	};
	const Lab cheek = colour_at(118), other = colour_at(347);
	std::printf("unit_avatar: skin round the eyes %.0f/%.0f/%.0f and %.0f/%.0f/%.0f, cheeks %.0f/%.0f/%.0f\n",
		    ps.part[0].skin.l, ps.part[0].skin.a, ps.part[0].skin.b, ps.part[1].skin.l, ps.part[1].skin.a,
		    ps.part[1].skin.b, cheek.l, cheek.a, cheek.b);
	CHECK(colour_gap(ps.part[FIRST_EYE].skin, cheek) < 18);
	CHECK(colour_gap(ps.part[SECOND_EYE].skin, other) < 18);
	CHECK(ps.part[FIRST_EYE].skin.a > 3); // skin, not the grey of a room or the white of an eye

	// A picture all one colour: the skin is that colour.
	Picture flat = portrait();
	for (size_t i = 0; i < flat.rgba.size(); i += 4) {
		flat.rgba[i] = 200;
		flat.rgba[i + 1] = 150;
		flat.rgba[i + 2] = 120;
	}
	skin_round(flat.frame(), ps);
	CHECK(colour_gap(ps.part[MOUTH].skin, lab_of(200 / 255.0f, 150 / 255.0f, 120 / 255.0f)) < 0.5);
}

// --- The character ----------------------------------------------------------

// A round character, 300 x 360: an orange ball with darker ears, and its
// eyes and mouth see-through (or not).
static Picture character(bool holes)
{
	Picture p;
	p.w = 300;
	p.h = 360;
	p.rgba.assign((size_t)p.w * p.h * 4, 0);
	for (int y = 0; y < p.h; y++)
		for (int x = 0; x < p.w; x++) {
			uint8_t *px = p.at(x, y);
			const float dx = x + 0.5f - 150, dy = y + 0.5f - 200;
			if (dx * dx + dy * dy < 140 * 140) {
				px[0] = 230;
				px[1] = 130;
				px[2] = 40;
				px[3] = 255;
			}
			// Ears.
			for (int side = -1; side <= 1; side += 2) {
				const float ex = x + 0.5f - (150 + side * 90), ey = y + 0.5f - 50;
				if (ex * ex + ey * ey < 35 * 35) {
					px[0] = 120;
					px[1] = 60;
					px[2] = 20;
					px[3] = 255;
				}
			}
		}
	if (holes) {
		auto clear = [&](float cx, float cy, float rx, float ry) {
			for (int y = 0; y < p.h; y++)
				for (int x = 0; x < p.w; x++) {
					const float u = (x + 0.5f - cx) / rx, v = (y + 0.5f - cy) / ry;
					if (u * u + v * v < 1)
						p.at(x, y)[3] = 0;
				}
		};
		clear(105, 170, 28, 18);
		clear(195, 170, 28, 18);
		clear(150, 265, 40, 20);
	}
	return p;
}

static const Box SPOTS[PARTS] = {
	{(105 - 32) / 300.0f, (170 - 22) / 360.0f, 64 / 300.0f, 44 / 360.0f},
	{(195 - 32) / 300.0f, (170 - 22) / 360.0f, 64 / 300.0f, 44 / 360.0f},
	{(150 - 46) / 300.0f, (265 - 24) / 360.0f, 92 / 300.0f, 48 / 360.0f},
};

static void test_shape()
{
	const Picture c = character(false);
	const Shape s = shape_of(c.rgba.data(), c.w, c.h, c.w * 4, false, SPOTS, true);
	CHECK(s.ready() && s.w <= 256 && s.h == 256 && near((double)s.h / s.w, 1.2, 0.01));
	// Round: highest in the middle, nothing at the outline or outside it.
	const float middle = depth_at(s, 0.5f, 200 / 360.0f);
	CHECK(middle > 0.3f && middle < 0.55f);
	CHECK(depth_at(s, 0.5f, 340 / 360.0f) < middle * 0.4f);
	CHECK(depth_at(s, 0.02f, 0.98f) == 0);
	CHECK(depth_at(s, 2, 2) == 0);
	// The same both sides.
	CHECK(near(depth_at(s, 0.3f, 0.6f), depth_at(s, 0.7f, 0.6f), 0.005));
	// The ears are thinner than the head.
	CHECK(depth_at(s, 60 / 300.0f, 50 / 360.0f) < middle * 0.6f);
	// A nose or muzzle stands out just above the mouth.
	Shape plain = shape_of(c.rgba.data(), c.w, c.h, c.w * 4, false, SPOTS, false);
	CHECK(depth_at(s, 0.5f, 245 / 360.0f) > depth_at(plain, 0.5f, 245 / 360.0f) + 0.01f);
	// It turns round the middle of its eyes and mouth.
	CHECK(near(s.pivot_x, 0.5, 0.01) && near(s.pivot_y, (170 + 265) / 2.0 / 360, 0.01));
	CHECK(near(plain.pivot_x, 0.5, 1e-6) && near(plain.pivot_y, 0.5, 1e-6));
	// Its colour round each spot: the orange.
	const Lab orange = lab_of(230 / 255.0f, 130 / 255.0f, 40 / 255.0f);
	for (int i = 0; i < PARTS; i++) {
		CHECK(!s.hole[i]);
		CHECK(colour_gap(s.colour[i], orange) < 2);
	}
	// See-through eyes and mouth are found.
	const Picture h = character(true);
	const Shape hs = shape_of(h.rgba.data(), h.w, h.h, h.w * 4, false, SPOTS, true);
	for (int i = 0; i < PARTS; i++) {
		CHECK(hs.hole[i]);
		CHECK(colour_gap(hs.colour[i], orange) < 2);
	}
	// Blue first: the same colours.
	Picture bgra = c;
	for (size_t i = 0; i < bgra.rgba.size(); i += 4)
		std::swap(bgra.rgba[i], bgra.rgba[i + 2]);
	const Shape bs = shape_of(bgra.rgba.data(), c.w, c.h, c.w * 4, true, SPOTS, true);
	CHECK(colour_gap(bs.colour[0], orange) < 2);
	// A big picture is worked on small.
	Picture big;
	big.w = 1200;
	big.h = 800;
	big.rgba.assign((size_t)big.w * big.h * 4, 255);
	const Shape bigs = shape_of(big.rgba.data(), big.w, big.h, big.w * 4, false, SPOTS, false);
	CHECK(bigs.w == 256 && near(bigs.h, 171, 1));
	// Nothing at all: no shape.
	CHECK(!shape_of(nullptr, 0, 0, 0, false, SPOTS, false).ready());
	std::printf("unit_avatar: character %dx%d, depth in the middle %.2f of its width\n", s.w, s.h, middle);
}

static void test_turning()
{
	const Picture c = character(false);
	const Shape s = shape_of(c.rgba.data(), c.w, c.h, c.w * 4, false, SPOTS, true);
	const float W = 300, H = 360;
	float sx, sy;
	// Not turned: every point from where it is.
	turned_from(s, W, H, 0, 0, 120, 210, sx, sy);
	CHECK(near(sx, 120, 1e-3) && near(sy, 210, 1e-3));
	// Turned to the viewer's right: the front moves right, so what's in the
	// middle now came from the left; nodding up, from below.
	const float px = s.pivot_x * W, py = s.pivot_y * H;
	turned_from(s, W, H, 0.5f, 0, px, py, sx, sy);
	CHECK(sx < px - 20 && near(sy, py, 1));
	turned_from(s, W, H, -0.5f, 0, px, py, sx, sy);
	CHECK(sx > px + 20);
	turned_from(s, W, H, 0, 0.3f, px, py, sx, sy);
	CHECK(sy > py + 10 && near(sx, px, 1));
	// Outside the character it only narrows a little towards the middle.
	turned_from(s, W, H, 0.5f, 0, 5, 200, sx, sy);
	CHECK(sx < 5 && sx > 5 - (px - 5) * 0.06f && near(sy, 200, 1e-3));
	// Its outline stays about where it was, while its middle slides over.
	float tx, ty;
	turned_point(s, W, H, 0.5f, 0, 11, 200, tx, ty);
	CHECK(near(tx, 11, 8));
	turned_point(s, W, H, 0.5f, 0, px, py, tx, ty);
	CHECK(tx > px + 40);
	// The steps find the right point: turned forward again, it lands where
	// it was asked for.
	double worst = 0;
	const float yaw = 0.6f, pitch = 0.25f;
	for (float y = 80; y <= 330; y += 10)
		for (float x = 20; x <= 280; x += 10) {
			turned_from(s, W, H, yaw, pitch, x, y, sx, sy);
			turned_point(s, W, H, yaw, pitch, sx, sy, tx, ty);
			worst = std::max(worst, (double)std::hypot(tx - x, ty - y));
		}
	std::printf("unit_avatar: turned 34 degrees and nodded 14: off by at most %.2f px\n", worst);
	CHECK(worst < 1.5);
}

// --- The drawn face ----------------------------------------------------------

static int index_of(const char *name)
{
	for (int i = 0; i < EXPRESSIONS; i++)
		if (std::string(EXPRESSION_NAMES[i]) == name)
			return i;
	return 0;
}

static void test_drawn()
{
	float moves[EXPRESSIONS] = {};
	Drawn d = drawn_face(moves, false, Drawn{});
	CHECK(d.eye[0] == EYE_OPEN && d.eye[1] == EYE_OPEN && d.mouth == MOUTH_CLOSED);
	// Your right eye shut: the viewer's left eye - their right in a mirror.
	moves[index_of("eyeBlinkRight")] = 0.7f;
	d = drawn_face(moves, false, Drawn{});
	CHECK(d.eye[0] == EYE_CLOSED && d.eye[1] == EYE_OPEN);
	d = drawn_face(moves, true, Drawn{});
	CHECK(d.eye[0] == EYE_OPEN && d.eye[1] == EYE_CLOSED);
	// Half shut; and it doesn't flicker between two. (The moves are
	// through the tuning: 1 is all the way.)
	moves[index_of("eyeBlinkRight")] = 0.4f;
	CHECK(drawn_face(moves, false, Drawn{}).eye[0] == EYE_HALF);
	moves[index_of("eyeBlinkRight")] = 0.55f;
	Drawn shut;
	shut.eye[0] = EYE_CLOSED;
	CHECK(drawn_face(moves, false, shut).eye[0] == EYE_CLOSED);
	CHECK(drawn_face(moves, false, Drawn{}).eye[0] == EYE_HALF);
	moves[index_of("eyeBlinkRight")] = 0.25f;
	Drawn half;
	half.eye[0] = EYE_HALF;
	CHECK(drawn_face(moves, false, half).eye[0] == EYE_HALF);
	CHECK(drawn_face(moves, false, Drawn{}).eye[0] == EYE_OPEN);
	moves[index_of("eyeBlinkRight")] = 0;

	// The mouth.
	const int jaw = index_of("jawOpen"), funnel = index_of("mouthFunnel");
	const int smile_l = index_of("mouthSmileLeft"), smile_r = index_of("mouthSmileRight");
	moves[jaw] = 0.3f;
	CHECK(drawn_face(moves, false, Drawn{}).mouth == MOUTH_OPEN);
	moves[jaw] = 0.7f;
	CHECK(drawn_face(moves, false, Drawn{}).mouth == MOUTH_WIDE);
	moves[jaw] = 0.6f;
	Drawn wide;
	wide.mouth = MOUTH_WIDE;
	CHECK(drawn_face(moves, false, wide).mouth == MOUTH_WIDE);
	CHECK(drawn_face(moves, false, Drawn{}).mouth == MOUTH_OPEN);
	moves[jaw] = 0.1f;
	Drawn open;
	open.mouth = MOUTH_OPEN;
	CHECK(drawn_face(moves, false, open).mouth == MOUTH_OPEN);
	CHECK(drawn_face(moves, false, Drawn{}).mouth == MOUTH_CLOSED);
	moves[funnel] = 0.4f;
	CHECK(drawn_face(moves, false, Drawn{}).mouth == MOUTH_O);
	moves[funnel] = 0;
	moves[jaw] = 0;
	moves[smile_l] = moves[smile_r] = 0.4f;
	CHECK(drawn_face(moves, false, Drawn{}).mouth == MOUTH_SMILE);
	moves[jaw] = 0.3f; // laughing: open beats smiling
	CHECK(drawn_face(moves, false, Drawn{}).mouth == MOUTH_OPEN);

	// The nearest picture there is.
	const unsigned all_eyes = 0x7, open_only = 1u << EYE_OPEN, open_closed = (1u << EYE_OPEN) | (1u << EYE_CLOSED);
	CHECK(eye_picture(EYE_HALF, all_eyes) == EYE_HALF);
	CHECK(eye_picture(EYE_HALF, open_closed) == EYE_CLOSED);
	CHECK(eye_picture(EYE_CLOSED, open_only) == EYE_OPEN);
	CHECK(eye_picture(EYE_OPEN, 0) == -1);
	const unsigned closed_open = (1u << MOUTH_CLOSED) | (1u << MOUTH_OPEN);
	CHECK(mouth_picture(MOUTH_WIDE, closed_open) == MOUTH_OPEN);
	CHECK(mouth_picture(MOUTH_O, closed_open) == MOUTH_OPEN);
	CHECK(mouth_picture(MOUTH_SMILE, closed_open) == MOUTH_CLOSED);
	CHECK(mouth_picture(MOUTH_OPEN, 1u << MOUTH_WIDE) == MOUTH_WIDE);
	CHECK(mouth_picture(MOUTH_CLOSED, 1u << MOUTH_SMILE) == MOUTH_SMILE);
	CHECK(mouth_picture(MOUTH_CLOSED, 0) == -1);
}

int main()
{
	if (portrait().w == 0) {
		std::fprintf(stderr, "unit_avatar: can't read tests/data/face/portrait.ppm\n");
		return 1;
	}
	test_colour();
	test_pieces();
	test_skin();
	test_shape();
	test_turning();
	test_drawn();
	std::printf("unit_avatar: %d passed, %d failed\n", passes, failures);
	return failures ? 1 : 0;
}
