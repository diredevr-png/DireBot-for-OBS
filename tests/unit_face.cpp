// Unit tests for the face avatar's tracker (src/face): our engine's
// convolutions against plain sums, the whole tracker against Google
// MediaPipe's own answers on the same pictures (tests/data/face: a photo,
// mirrored, turned and half size - see tools/face-expected.py), following
// a face, the smoothing, losing and finding it, "Set", the head's turn,
// and what the avatar shows while the face is lost.
// Build and run: tools/test-link.sh (Linux, any C++17 compiler).

#include "../src/face/tracker.hpp"
#include "../src/face/pose.hpp"
#include "../src/face/nn/kernels.hpp"
#include "../src/face/nn/net.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#ifndef DIREBOT_SOURCE_DIR
#define DIREBOT_SOURCE_DIR "."
#endif

using namespace direbot::face;
namespace kernels = direbot::face::nn::kernels;
using json = nlohmann::json;

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
static const std::string MODELS = ROOT + "/data/face/face_landmarker.task";

// --- The convolutions ---------------------------------------------------

// The engine's packing (net.cpp): [out][kh][kw][in] -> [panel][kh][kw][in][8].
static std::vector<float> packed(const std::vector<float> &w, int out_c, int taps, int in_c)
{
	const int panels = (out_c + kernels::PANEL - 1) / kernels::PANEL;
	std::vector<float> p((size_t)panels * kernels::PANEL * taps * in_c, 0.0f);
	for (int o = 0; o < out_c; o++)
		for (int tap = 0; tap < taps; tap++)
			for (int c = 0; c < in_c; c++)
				p[(((size_t)(o / kernels::PANEL) * taps + tap) * in_c + c) * kernels::PANEL +
				  o % kernels::PANEL] = w[((size_t)o * taps + tap) * in_c + c];
	return p;
}

static size_t rounded(int n)
{
	return (size_t)(n + kernels::PANEL - 1) / kernels::PANEL * kernels::PANEL;
}

struct Shape {
	int in_h, in_w, c_in, c_out, kh, kw, sh, sw, pt, pl, dh, dw, out_h, out_w;
	bool residual, prelu, clamp;
};

static Shape random_shape(std::mt19937 &rng, bool depthwise)
{
	auto pick = [&](std::initializer_list<int> from) {
		std::uniform_int_distribution<size_t> d(0, from.size() - 1);
		return *(from.begin() + d(rng));
	};
	auto upto = [&](int lo, int hi) {
		return std::uniform_int_distribution<int>(lo, hi)(rng);
	};
	for (;;) {
		Shape s{};
		s.in_h = upto(1, 11);
		s.in_w = upto(1, 11);
		s.c_in = depthwise ? pick({1, 3, 7, 8, 9, 16, 17, 24, 33}) : pick({1, 2, 3, 5, 8, 13, 16, 17, 32, 40});
		s.c_out = depthwise ? s.c_in : pick({1, 3, 7, 8, 9, 16, 17, 24, 33, 40});
		s.kh = depthwise ? pick({1, 3, 5}) : pick({1, 2, 3, 5});
		s.kw = depthwise ? pick({1, 3, 5}) : pick({1, 2, 3, 5});
		s.sh = pick({1, 1, 2});
		s.sw = pick({1, 1, 2});
		s.dh = pick({1, 1, 1, 2});
		s.dw = pick({1, 1, 1, 2});
		const int eh = s.dh * (s.kh - 1) + 1, ew = s.dw * (s.kw - 1) + 1;
		s.pt = upto(0, eh - 1);
		s.pl = upto(0, ew - 1);
		const int pb = upto(0, eh - 1), pr = upto(0, ew - 1);
		s.out_h = (s.in_h + s.pt + pb - eh) / s.sh + 1;
		s.out_w = (s.in_w + s.pl + pr - ew) / s.sw + 1;
		if (s.in_h + s.pt + pb < eh || s.in_w + s.pl + pr < ew || s.out_h < 1 || s.out_w < 1)
			continue;
		s.residual = !depthwise && upto(0, 2) == 0;
		s.prelu = upto(0, 1) == 0;
		s.clamp = upto(0, 2) == 0;
		return s;
	}
}

static std::vector<float> random_values(std::mt19937 &rng, size_t n, float lo = -1, float hi = 1)
{
	std::uniform_real_distribution<float> d(lo, hi);
	std::vector<float> v(n);
	for (float &x : v)
		x = d(rng);
	return v;
}

// One shape on each version of the code: against the sums done plainly
// (in doubles), every output written and nothing past the end, and the
// rows split in two the same as all at once.
static void check_kernels(const Shape &s, bool depthwise, std::mt19937 &rng, double &worst)
{
	const int taps = s.kh * s.kw;
	const auto in = random_values(rng, (size_t)s.in_h * s.in_w * s.c_in);
	const auto w = random_values(rng, (size_t)s.c_out * taps * (depthwise ? 1 : s.c_in));
	auto bias = random_values(rng, rounded(s.c_out));
	auto slope = random_values(rng, rounded(s.c_out), 0, 0.5f);
	for (size_t i = s.c_out; i < bias.size(); i++) {
		bias[i] = 0;
		slope[i] = 1;
	}
	const size_t out_n = (size_t)s.out_h * s.out_w * s.c_out;
	const auto residual = random_values(rng, out_n);
	const float lo = s.clamp ? -0.5f : -3.4e38f, hi = s.clamp ? 0.8f : 3.4e38f;

	// [out][kh][kw][in] for a convolution; [kh][kw][c] for a depthwise one.
	std::vector<double> want(out_n), size(out_n);
	for (int y = 0; y < s.out_h; y++)
		for (int x = 0; x < s.out_w; x++)
			for (int o = 0; o < s.c_out; o++) {
				double sum = bias[o], mag = std::fabs(bias[o]);
				for (int ky = 0; ky < s.kh; ky++)
					for (int kx = 0; kx < s.kw; kx++) {
						const int iy = y * s.sh - s.pt + ky * s.dh,
							  ix = x * s.sw - s.pl + kx * s.dw;
						if (iy < 0 || iy >= s.in_h || ix < 0 || ix >= s.in_w)
							continue;
						const float *p = &in[((size_t)iy * s.in_w + ix) * s.c_in];
						const int tap = ky * s.kw + kx;
						if (depthwise) {
							const double t = (double)p[o] * w[(size_t)tap * s.c_out + o];
							sum += t;
							mag += std::fabs(t);
							continue;
						}
						for (int c = 0; c < s.c_in; c++) {
							const double t =
								(double)p[c] * w[((size_t)o * taps + tap) * s.c_in + c];
							sum += t;
							mag += std::fabs(t);
						}
					}
				const size_t at = ((size_t)y * s.out_w + x) * s.c_out + o;
				if (s.residual) {
					sum += residual[at];
					mag += std::fabs(residual[at]);
				}
				if (s.prelu && sum < 0)
					sum *= slope[o];
				want[at] = std::min<double>(hi, std::max<double>(lo, sum));
				size[at] = mag;
			}

	const auto pw = depthwise ? w : packed(w, s.c_out, taps, s.c_in);
	const std::vector<float> zeros((size_t)s.c_in, 0.0f);
	const float NOT_SET = std::numeric_limits<float>::quiet_NaN();
	const int GUARD = 64;
	auto run = [&](const kernels::Table &t, int from, int to, std::vector<float> &out) {
		if (depthwise) {
			kernels::Depthwise j;
			j.in = in.data();
			j.in_h = s.in_h;
			j.in_w = s.in_w;
			j.c = s.c_in;
			j.out = out.data();
			j.out_h = s.out_h;
			j.out_w = s.out_w;
			j.weights = pw.data();
			j.bias = bias.data();
			j.kh = s.kh;
			j.kw = s.kw;
			j.stride_h = s.sh;
			j.stride_w = s.sw;
			j.pad_top = s.pt;
			j.pad_left = s.pl;
			j.dil_h = s.dh;
			j.dil_w = s.dw;
			j.slope = s.prelu ? slope.data() : nullptr;
			j.lo = lo;
			j.hi = hi;
			j.row_from = from;
			j.row_to = to;
			t.depthwise(j);
			return;
		}
		kernels::Conv j;
		j.in = in.data();
		j.in_h = s.in_h;
		j.in_w = s.in_w;
		j.in_c = s.c_in;
		j.out = out.data();
		j.out_h = s.out_h;
		j.out_w = s.out_w;
		j.out_c = s.c_out;
		j.weights = pw.data();
		j.bias = bias.data();
		j.kh = s.kh;
		j.kw = s.kw;
		j.stride_h = s.sh;
		j.stride_w = s.sw;
		j.pad_top = s.pt;
		j.pad_left = s.pl;
		j.dil_h = s.dh;
		j.dil_w = s.dw;
		j.residual = s.residual ? residual.data() : nullptr;
		j.slope = s.prelu ? slope.data() : nullptr;
		j.lo = lo;
		j.hi = hi;
		j.zeros = zeros.data();
		j.row_from = from;
		j.row_to = to;
		t.conv(j);
	};
	std::vector<const kernels::Table *> tables = {&kernels::plain(), &kernels::best()};
	if (kernels::sse2() && kernels::sse2() != &kernels::best())
		tables.push_back(kernels::sse2());
	for (const kernels::Table *t : tables) {
		std::vector<float> all(out_n + GUARD, NOT_SET), split(out_n + GUARD, NOT_SET);
		run(*t, 0, s.out_h, all);
		const int half = s.out_h / 2;
		run(*t, 0, half, split);
		run(*t, half, s.out_h, split);
		bool right = true, same = true, guard = true;
		for (size_t i = 0; i < out_n; i++) {
			const double off = std::fabs((double)all[i] - want[i]);
			worst = std::max(worst, off / (size[i] + 1e-3));
			right = right && off <= 2e-6 * size[i] + 1e-6;
			same = same && all[i] == split[i];
		}
		for (size_t i = out_n; i < out_n + GUARD; i++)
			guard = guard && std::isnan(all[i]) && std::isnan(split[i]);
		if (!right || !same || !guard)
			std::fprintf(
				stderr,
				"  %s %s: %dx%dx%d -> %dx%dx%d, %dx%d filter, stride %d,%d, dilation %d,%d, pad %d,%d\n",
				t->name, depthwise ? "depthwise" : "conv", s.in_h, s.in_w, s.c_in, s.out_h, s.out_w,
				s.c_out, s.kh, s.kw, s.sh, s.sw, s.dh, s.dw, s.pt, s.pl);
		CHECK(right);
		CHECK(same);
		CHECK(guard);
	}
}

static void test_convolutions()
{
	std::mt19937 rng(20261030);
	double worst = 0;
	for (int i = 0; i < 300; i++)
		check_kernels(random_shape(rng, false), false, rng, worst);
	for (int i = 0; i < 200; i++)
		check_kernels(random_shape(rng, true), true, rng, worst);
	std::printf("unit_face: convolutions on %s, worst off by %.1e of their size\n", kernels::best().name, worst);
}

static void test_engine_says_no()
{
	nn::Net net;
	std::string problem;
	const uint8_t junk[64] = {1, 2, 3};
	CHECK(!net.load(junk, sizeof junk, &problem));
	CHECK(!problem.empty());
	CHECK(!net.loaded());
	CHECK(!net.load(nullptr, 0, &problem));
}

// --- Pictures -----------------------------------------------------------

struct Picture {
	int w = 0, h = 0;
	std::vector<uint8_t> rgb;
	const uint8_t *at(int x, int y) const { return &rgb[((size_t)y * w + x) * 3]; }
	uint8_t *at(int x, int y) { return &rgb[((size_t)y * w + x) * 3]; }
};

static Picture read_ppm(const std::string &path)
{
	std::ifstream in(path, std::ios::binary);
	std::string magic;
	int w = 0, h = 0, top = 0;
	in >> magic >> w >> h >> top;
	in.get();
	Picture p;
	if (magic != "P6" || top != 255 || w <= 0 || h <= 0)
		return p;
	p.w = w;
	p.h = h;
	p.rgb.resize((size_t)w * h * 3);
	in.read((char *)p.rgb.data(), (std::streamsize)p.rgb.size());
	if (!in)
		return Picture();
	return p;
}

// The same pictures as tools/face-expected.py makes, pixel for pixel.
static Picture mirrored(const Picture &p)
{
	Picture m = p;
	for (int y = 0; y < p.h; y++)
		for (int x = 0; x < p.w; x++)
			std::copy(p.at(p.w - 1 - x, y), p.at(p.w - 1 - x, y) + 3, m.at(x, y));
	return m;
}

static int floor_div(int a, int b)
{
	return a / b - ((a % b != 0) && ((a < 0) != (b < 0)));
}

static Picture turned(const Picture &p)
{
	Picture t = p;
	std::fill(t.rgb.begin(), t.rgb.end(), 0);
	for (int y = 0; y < p.h; y++)
		for (int x = 0; x < p.w; x++) {
			const int dx = 2 * x + 1 - p.w, dy = 2 * y + 1 - p.h;
			const int sx = floor_div(12 * dx + 5 * dy + 13 * (p.w - 1) + 13, 26);
			const int sy = floor_div(-5 * dx + 12 * dy + 13 * (p.h - 1) + 13, 26);
			if (sx >= 0 && sx < p.w && sy >= 0 && sy < p.h)
				std::copy(p.at(sx, sy), p.at(sx, sy) + 3, t.at(x, y));
		}
	return t;
}

static Picture halved(const Picture &p)
{
	Picture h;
	h.w = p.w / 2;
	h.h = p.h / 2;
	h.rgb.resize((size_t)h.w * h.h * 3);
	for (int y = 0; y < h.h; y++)
		for (int x = 0; x < h.w; x++)
			for (int k = 0; k < 3; k++)
				h.at(x, y)[k] =
					(uint8_t)((p.at(2 * x, 2 * y)[k] + p.at(2 * x + 1, 2 * y)[k] +
						   p.at(2 * x, 2 * y + 1)[k] + p.at(2 * x + 1, 2 * y + 1)[k] + 2) /
						  4);
	return h;
}

// Twice as big (each pixel 2 x 2).
static Picture doubled(const Picture &p)
{
	Picture d;
	d.w = p.w * 2;
	d.h = p.h * 2;
	d.rgb.resize((size_t)d.w * d.h * 3);
	for (int y = 0; y < d.h; y++)
		for (int x = 0; x < d.w; x++)
			std::copy(p.at(x / 2, y / 2), p.at(x / 2, y / 2) + 3, d.at(x, y));
	return d;
}

// The picture on a grey background, `x` pixels from its left.
static Picture placed(const Picture &p, int w, int h, int x, int y)
{
	Picture out;
	out.w = w;
	out.h = h;
	out.rgb.assign((size_t)w * h * 3, 96);
	for (int py = 0; py < p.h; py++)
		for (int px = 0; px < p.w; px++)
			if (px + x >= 0 && px + x < w && py + y >= 0 && py + y < h)
				std::copy(p.at(px, py), p.at(px, py) + 3, out.at(px + x, py + y));
	return out;
}

// As the add-on hands pictures over: 4 bytes a pixel, red first (or
// blue first), each row `pad` bytes longer than it needs.
struct Held {
	std::vector<uint8_t> bytes;
	Frame frame;
};

static Held frame_of(const Picture &p, bool bgra = false, int pad = 0)
{
	Held f;
	const int stride = p.w * 4 + pad;
	f.bytes.assign((size_t)stride * p.h, 7);
	for (int y = 0; y < p.h; y++)
		for (int x = 0; x < p.w; x++) {
			uint8_t *o = &f.bytes[(size_t)y * stride + (size_t)x * 4];
			const uint8_t *i = p.at(x, y);
			o[0] = bgra ? i[2] : i[0];
			o[1] = i[1];
			o[2] = bgra ? i[0] : i[2];
			o[3] = 255;
		}
	f.frame.px = f.bytes.data();
	f.frame.w = p.w;
	f.frame.h = p.h;
	f.frame.stride = stride;
	f.frame.bgra = bgra;
	return f;
}

static const Picture &portrait()
{
	static const Picture p = read_ppm(ROOT + "/tests/data/face/portrait.ppm");
	return p;
}

static bool load(Tracker &t, bool plain = false)
{
	t.set_plain(plain);
	std::string problem;
	const bool ok = t.load_file(MODELS, &problem);
	if (!ok)
		std::fprintf(stderr, "  can't load the face models: %s\n", problem.c_str());
	return ok;
}

// --- Against MediaPipe ----------------------------------------------------

struct Off {
	double points = 0, z = 0, expressions = 0, turn = 0, move = 0;
};

static Off compare(const Face &face, const json &want, const char *name)
{
	Off off;
	const auto &points = want["points"];
	for (int i = 0; i < POINTS; i++) {
		off.points = std::max(off.points, std::fabs(face.points[i].x - points[i][0].get<double>()));
		off.points = std::max(off.points, std::fabs(face.points[i].y - points[i][1].get<double>()));
		off.z = std::max(off.z, std::fabs(face.points[i].z - points[i][2].get<double>()));
	}
	for (int i = 0; i < EXPRESSIONS; i++)
		off.expressions = std::max(off.expressions,
					   std::fabs(face.expressions[i] - want["expressions"][i].get<double>()));
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 3; c++)
			off.turn = std::max(off.turn,
					    std::fabs(face.pose[r * 4 + c] - want["pose"][r * 4 + c].get<double>()));
		off.move = std::max(off.move, std::fabs(face.pose[r * 4 + 3] - want["pose"][r * 4 + 3].get<double>()));
	}
	std::printf("unit_face: %-21s against MediaPipe: points off by %.4f px (depth %.4f), expressions %.5f, "
		    "turn %.5f, place %.4f cm\n",
		    name, off.points, off.z, off.expressions, off.turn, off.move);
	return off;
}

static json expected()
{
	std::ifstream in(ROOT + "/tests/data/face/expected.json");
	return json::parse(in, nullptr, false);
}

static void test_same_as_mediapipe(bool plain)
{
	const json want = expected();
	CHECK(want.is_object());
	if (!want.is_object() || portrait().w == 0)
		return;
	Tracker t;
	CHECK(load(t, plain));
	t.set_smoothing(-1); // a still photo: MediaPipe doesn't smooth those
	for (int i = 0; i < EXPRESSIONS; i++)
		CHECK(want["plain"]["names"][i].get<std::string>() == EXPRESSION_NAMES[i]);
	const std::pair<const char *, Picture> pictures[] = {{"plain", portrait()},
							     {"mirrored", mirrored(portrait())},
							     {"turned", turned(portrait())},
							     {"halved", halved(portrait())}};
	for (const auto &[name, picture] : pictures) {
		const json &w = want[name];
		CHECK(w["w"] == picture.w && w["h"] == picture.h);
		t.forget(); // each one on its own, as MediaPipe's still-photo mode does
		const Held f = frame_of(picture);
		const Face &face = t.look(f.frame, 0);
		CHECK(face.searched);
		CHECK(face.found == w["found"].get<bool>());
		if (!face.found)
			continue;
		const Off off = compare(face, w, (std::string(name) + (plain ? " (plain code)" : "")).c_str());
		CHECK(off.points < 0.05);
		CHECK(off.z < 0.05);
		CHECK(off.expressions < 0.002);
		CHECK(off.turn < 0.001);
		CHECK(off.move < 0.02);
		// The face finder's own answers (its box in whole pixels, as
		// MediaPipe gives it).
		const auto &found = t.found();
		CHECK(found.size() == w["finds"].size());
		for (size_t i = 0; i < std::min(found.size(), w["finds"].size()); i++) {
			const json &box = w["finds"][i]["box"];
			CHECK(near(found[i].x, box[0].get<double>(), 1.01));
			CHECK(near(found[i].y, box[1].get<double>(), 1.01));
			CHECK(near(found[i].w, box[2].get<double>(), 1.01));
			CHECK(near(found[i].h, box[3].get<double>(), 1.01));
			CHECK(near(found[i].score, w["finds"][i]["score"].get<double>(), 0.001));
			for (int k = 0; k < 6; k++) {
				CHECK(near(found[i].keys[k * 2], w["finds"][i]["keys"][k][0].get<double>(), 0.05));
				CHECK(near(found[i].keys[k * 2 + 1], w["finds"][i]["keys"][k][1].get<double>(), 0.05));
			}
		}
	}
}

// Blue-first pixels and longer rows (as OBS can hand them over) are the
// same picture: the very same answers.
static void test_pixel_layouts()
{
	Tracker a, b;
	if (!load(a) || !load(b))
		return;
	a.set_smoothing(-1);
	b.set_smoothing(-1);
	const Held rgba = frame_of(portrait()), bgra = frame_of(portrait(), true, 36);
	const Face fa = a.look(rgba.frame, 0);
	const Face fb = b.look(bgra.frame, 0);
	CHECK(fa.found && fb.found);
	bool same = true;
	for (int i = 0; i < POINTS; i++)
		same = same && fa.points[i].x == fb.points[i].x && fa.points[i].y == fb.points[i].y &&
		       fa.points[i].z == fb.points[i].z;
	for (int i = 0; i < EXPRESSIONS; i++)
		same = same && fa.expressions[i] == fb.expressions[i];
	CHECK(same);
}

// A mirrored camera: the same face, its turn the other way, and the
// first eye still the one on the picture's left.
static void test_mirrored_camera()
{
	Tracker t;
	if (!load(t))
		return;
	t.set_smoothing(-1);
	const Face plain = t.look(frame_of(portrait()).frame, 0);
	t.forget();
	const Face mirror = t.look(frame_of(mirrored(portrait())).frame, 0);
	CHECK(plain.found && mirror.found);
	if (!plain.found || !mirror.found)
		return;
	CHECK(near(plain.turn.yaw, -mirror.turn.yaw, 2.5));
	CHECK(near(plain.turn.roll, -mirror.turn.roll, 2.5));
	CHECK(near(plain.turn.pitch, mirror.turn.pitch, 2.5));
	const Layout a = layout_of(plain), b = layout_of(mirror);
	CHECK(a.parts[FIRST_EYE].x < a.parts[SECOND_EYE].x);
	CHECK(b.parts[FIRST_EYE].x < b.parts[SECOND_EYE].x);
	// The model can't tell a mirrored camera: its "right eye" is the one
	// on the picture's left either way (why the parts go by the picture).
	CHECK(plain.points[RIGHT_EYE_OUTER].x < plain.points[LEFT_EYE_OUTER].x);
	CHECK(mirror.points[RIGHT_EYE_OUTER].x < mirror.points[LEFT_EYE_OUTER].x);
	for (int part = 0; part < PARTS; part++) {
		const int other = part == MOUTH ? MOUTH : 1 - part;
		CHECK(near(a.parts[part].x, 1 - b.parts[other].x, 0.01));
		CHECK(near(a.parts[part].y, b.parts[other].y, 0.01));
		CHECK(near(a.parts[part].w, b.parts[other].w, 0.01));
	}
	CHECK(near(a.angle, -b.angle, 0.03));
	CHECK(near(a.gap, b.gap, 0.005));
}

// The plain C++ code, step by step, gives the same answers as the fast
// code with its steps done together.
static void test_plain_and_fast_agree()
{
	Tracker plain, fast;
	if (!load(plain, true) || !load(fast))
		return;
	plain.set_smoothing(-1);
	fast.set_smoothing(-1);
	const Held f = frame_of(turned(portrait()));
	const Face a = plain.look(f.frame, 0), b = fast.look(f.frame, 0);
	CHECK(a.found && b.found);
	double points = 0, expressions = 0;
	for (int i = 0; i < POINTS; i++)
		points = std::max({points, (double)std::fabs(a.points[i].x - b.points[i].x),
				   (double)std::fabs(a.points[i].y - b.points[i].y)});
	for (int i = 0; i < EXPRESSIONS; i++)
		expressions = std::max(expressions, (double)std::fabs(a.expressions[i] - b.expressions[i]));
	CHECK(points < 0.01);
	CHECK(expressions < 0.0005);
}

// --- Following -------------------------------------------------------------

static void test_follows_and_loses()
{
	Tracker t;
	if (!load(t))
		return;
	t.set_smoothing(-1);
	const Held face = frame_of(portrait());
	Picture grey;
	grey.w = 256;
	grey.h = 256;
	grey.rgb.assign((size_t)256 * 256 * 3, 128);
	const Held nothing = frame_of(grey);

	const Face first = t.look(face.frame, 0);
	CHECK(first.found && first.searched);
	CHECK(first.w == 256 && first.h == 256);
	CHECK(first.presence > 0.9f);
	Tracker::Square sq;
	CHECK(t.following(&sq));
	CHECK(sq.w > 100 && sq.w < 256 && near(sq.w, sq.h, 1e-3));
	// The next looks follow on from the points: no finder, the same face.
	const Face second = t.look(face.frame, 0.033);
	CHECK(second.found && !second.searched);
	double moved = 0;
	for (int i = 0; i < POINTS; i++)
		moved = std::max(moved, (double)std::hypot(second.points[i].x - first.points[i].x,
							   second.points[i].y - first.points[i].y));
	CHECK(moved < 3);
	// Gone: lost straight away, and found again by the finder after.
	const Face gone = t.look(nothing.frame, 0.066);
	CHECK(!gone.found);
	CHECK(!t.following());
	const Face still_gone = t.look(nothing.frame, 0.1);
	CHECK(!still_gone.found && still_gone.searched);
	CHECK(t.found().empty());
	const Face back = t.look(face.frame, 0.133);
	CHECK(back.found && back.searched);
	// A new size of picture (another camera) starts again too.
	const Held small = frame_of(halved(portrait()));
	const Face other = t.look(small.frame, 0.166);
	CHECK(other.found && other.searched && other.w == 128);
	// Pictures it can't use.
	Frame bad = face.frame;
	bad.px = nullptr;
	CHECK(!t.look(bad, 0.2).found);
	bad = face.frame;
	bad.w = 1;
	CHECK(!t.look(bad, 0.233).found);
	bad = face.frame;
	bad.stride = 16;
	CHECK(!t.look(bad, 0.266).found);
	// forget(): the finder again.
	t.look(face.frame, 0.3);
	t.forget();
	CHECK(t.look(face.frame, 0.333).searched);
}

static void test_noise_has_no_face()
{
	Tracker t;
	if (!load(t))
		return;
	std::mt19937 rng(7);
	Picture p;
	p.w = 320;
	p.h = 240;
	p.rgb.resize((size_t)p.w * p.h * 3);
	for (auto &v : p.rgb)
		v = (uint8_t)(rng() & 255);
	const Face f = t.look(frame_of(p).frame, 0);
	CHECK(!f.found);
	CHECK(f.searched);
}

static void test_without_models()
{
	Tracker t;
	std::string problem;
	CHECK(!t.load_file(ROOT + "/no/such/face_landmarker.task", &problem));
	CHECK(problem.find("missing") != std::string::npos);
	const uint8_t junk[100] = {'P', 'K'};
	CHECK(!t.load(junk, sizeof junk, &problem));
	CHECK(problem.find("face_landmarker.task") != std::string::npos);
	CHECK(!t.ready());
	CHECK(!t.look(frame_of(portrait()).frame, 0).found);
}

// The nose's place across the picture on each look, the face on a grey
// background at `xs[look]`.
static std::vector<float> nose_track(float smoothing, const std::vector<int> &xs)
{
	Tracker t;
	std::vector<float> out;
	if (!load(t))
		return out;
	t.set_smoothing(smoothing);
	for (size_t i = 0; i < xs.size(); i++) {
		const Face &f = t.look(frame_of(placed(portrait(), 352, 256, xs[i], 0)).frame, i / 30.0);
		out.push_back(f.found ? f.points[NOSE_TIP].x : -1);
	}
	return out;
}

static double wobble(const std::vector<float> &xs, size_t from)
{
	double lo = 1e9, hi = -1e9;
	for (size_t i = from; i < xs.size(); i++) {
		lo = std::min<double>(lo, xs[i]);
		hi = std::max<double>(hi, xs[i]);
	}
	return hi - lo;
}

// Steady when you're still, straight there when you move - and the
// slider goes from steadier to snappier.
static void test_smoothing()
{
	// A jump of 8 pixels after a third of a second still.
	std::vector<int> jump(40, 40);
	std::fill(jump.begin() + 10, jump.end(), 48);
	const auto raw = nose_track(-1, jump), steady = nose_track(0, jump), normal = nose_track(0.5f, jump),
		   snappy = nose_track(1, jump);
	CHECK(raw.size() == 40 && steady.size() == 40 && normal.size() == 40 && snappy.size() == 40);
	if (raw.size() != 40 || steady.size() != 40 || normal.size() != 40 || snappy.size() != 40)
		return;
	CHECK(std::count(raw.begin(), raw.end(), -1.0f) == 0);
	CHECK(near(raw[10] - raw[9], 8, 1));
	const double lag_steady = raw[10] - steady[10], lag_normal = raw[10] - normal[10],
		     lag_snappy = raw[10] - snappy[10];
	std::printf("unit_face: after an 8 px move, behind by %.2f px (steady), %.2f (normal), %.2f (snappy)\n",
		    lag_steady, lag_normal, lag_snappy);
	CHECK(lag_steady > lag_normal && lag_normal > lag_snappy);
	CHECK(lag_snappy >= 0 && lag_snappy < 1);
	CHECK(lag_normal < 2.5);
	CHECK(lag_steady > 0.5);
	// ...and there soon after.
	for (const auto *s : {&steady, &normal, &snappy})
		CHECK(near((*s)[39], raw[39], 0.3));

	// One pixel back and forth, look after look (a camera's grain): the
	// steadier, the less it wobbles.
	std::vector<int> shake(40);
	for (size_t i = 0; i < shake.size(); i++)
		shake[i] = 40 + (int)(i % 2);
	const auto raw_w = wobble(nose_track(-1, shake), 20), steady_w = wobble(nose_track(0, shake), 20),
		   normal_w = wobble(nose_track(0.5f, shake), 20), snappy_w = wobble(nose_track(1, shake), 20);
	std::printf(
		"unit_face: one pixel of grain wobbles %.2f px (none), %.2f (steady), %.2f (normal), %.2f (snappy)\n",
		raw_w, steady_w, normal_w, snappy_w);
	CHECK(raw_w > 0.5);
	CHECK(steady_w < normal_w && normal_w < snappy_w && snappy_w <= raw_w + 1e-3);
	CHECK(steady_w < raw_w * 0.6);
}

// --- "Set" -------------------------------------------------------------------

static void test_set()
{
	Tracker t;
	if (!load(t))
		return;
	const Held face = frame_of(portrait());
	Picture grey;
	grey.w = 256;
	grey.h = 256;
	grey.rgb.assign((size_t)256 * 256 * 3, 128);
	const Held nothing = frame_of(grey);
	double now = 0;
	auto look = [&](const Held &f) -> const Face & {
		return t.look(f.frame, now += 1 / 30.0);
	};

	CHECK(!t.has_rest() && !t.setting());
	CHECK(t.save_rest().empty());
	// Before "Set": the turn is against looking straight at the camera.
	const Face before = look(face);
	CHECK(before.found);
	for (int i = 0; i < EXPRESSIONS; i++)
		CHECK(before.moves[i] == std::min(1.0f, std::max(0.0f, before.expressions[i])));
	t.set_rest();
	CHECK(t.setting());
	// Looks without a face don't count.
	look(nothing);
	look(nothing);
	CHECK(t.setting() && !t.has_rest());
	for (int i = 0; i < 9; i++)
		look(face);
	CHECK(t.setting());
	const Face last = look(face);
	CHECK(!t.setting() && t.has_rest());
	// Now the same face reads as at rest: no turn, nothing moved.
	const Face after = look(face);
	CHECK(near(after.turn.yaw, 0, 1) && near(after.turn.pitch, 0, 1) && near(after.turn.roll, 0, 1));
	float most = 0;
	for (int i = 0; i < EXPRESSIONS; i++)
		most = std::max(most, after.moves[i]);
	CHECK(most < 0.05f);
	const Rest &rest = t.rest();
	const Layout l = layout_of(last);
	CHECK(near(rest.x, l.cx, 0.01) && near(rest.y, l.cy, 0.01));
	CHECK(near(rest.gap, l.gap, 0.005) && near(rest.angle, l.angle, 0.02));

	// Kept as text in OBS's settings, and back.
	const std::string text = t.save_rest();
	CHECK(!text.empty());
	Tracker u;
	CHECK(u.load_rest(text));
	CHECK(u.has_rest());
	for (int i = 0; i < 16; i++)
		CHECK(near(u.rest().pose[i], rest.pose[i], 1e-4));
	for (int i = 0; i < EXPRESSIONS; i++)
		CHECK(near(u.rest().expressions[i], rest.expressions[i], 1e-4));
	CHECK(near(u.rest().x, rest.x, 1e-4) && near(u.rest().gap, rest.gap, 1e-4));
	CHECK(!u.load_rest("{}"));
	CHECK(!u.load_rest("not json"));
	CHECK(!u.load_rest("{\"pose\":[1,2,3],\"rest\":{}}"));
	CHECK(u.has_rest()); // a bad one leaves the old one
	// Turned head after "Set": the turn reads against the rest.
	t.forget();
	const Face tilted = t.look(frame_of(turned(portrait())).frame, now += 1);
	CHECK(tilted.searched && tilted.found);
	std::printf("unit_face: the photo turned by 22.6 degrees reads as yaw %.1f, pitch %.1f, roll %.1f\n",
		    tilted.turn.yaw, tilted.turn.pitch, tilted.turn.roll);
	CHECK(near(tilted.turn.roll, 22.6, 3));
	CHECK(near(tilted.turn.yaw, 0, 4) && near(tilted.turn.pitch, 0, 4));
	t.clear_rest();
	CHECK(!t.has_rest() && t.save_rest().empty());
}

// --- The head's turn ------------------------------------------------------------

static void turn_matrix(double yaw, double pitch, double roll, float pose[16])
{
	// Turned about y (yaw), then x (pitch), then z (roll), in degrees.
	const double d = 3.14159265358979323846 / 180;
	const double cy = std::cos(yaw * d), sy = std::sin(yaw * d), cp = std::cos(pitch * d), sp = std::sin(pitch * d),
		     cr = std::cos(roll * d), sr = std::sin(roll * d);
	const double Y[9] = {cy, 0, sy, 0, 1, 0, -sy, 0, cy};
	const double P[9] = {1, 0, 0, 0, cp, -sp, 0, sp, cp};
	const double R[9] = {cr, -sr, 0, sr, cr, 0, 0, 0, 1};
	double YP[9], M[9];
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++) {
			YP[r * 3 + c] = 0;
			for (int k = 0; k < 3; k++)
				YP[r * 3 + c] += Y[r * 3 + k] * P[k * 3 + c];
		}
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++) {
			M[r * 3 + c] = 0;
			for (int k = 0; k < 3; k++)
				M[r * 3 + c] += R[r * 3 + k] * YP[k * 3 + c];
		}
	for (int i = 0; i < 16; i++)
		pose[i] = i % 5 == 0 ? 1.0f : 0.0f;
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			pose[r * 4 + c] = (float)(M[r * 3 + c] * 1.3); // any size is taken out
	pose[3] = 2;
	pose[11] = -40;
}

static void test_turns()
{
	float pose[16], rest[16];
	turn_matrix(30, 0, 0, pose);
	Turn t = turn_of(pose);
	CHECK(near(t.yaw, 30, 0.01) && near(t.pitch, 0, 0.01) && near(t.roll, 0, 0.01));
	turn_matrix(0, 0, 0, pose);
	t = turn_of(pose);
	CHECK(near(t.yaw, 0, 0.01) && near(t.pitch, 0, 0.01) && near(t.roll, 0, 0.01));
	turn_matrix(0, 20, 0, pose);
	t = turn_of(pose);
	CHECK(near(std::fabs(t.pitch), 20, 0.01) && near(t.yaw, 0, 0.01) && near(t.roll, 0, 0.01));
	turn_matrix(0, 0, 15, pose);
	t = turn_of(pose);
	CHECK(near(std::fabs(t.roll), 15, 0.01) && near(t.yaw, 0, 0.01) && near(t.pitch, 0, 0.01));
	// Since a rest that's turned already (a camera off to one side).
	turn_matrix(25, 5, 0, rest);
	t = turn_since(rest, rest);
	CHECK(near(t.yaw, 0, 0.01) && near(t.pitch, 0, 0.01) && near(t.roll, 0, 0.01));

	// fit(): finds a known turn, size and move.
	std::mt19937 rng(3);
	std::uniform_real_distribution<double> d(-5, 5);
	const int n = 40;
	std::vector<double> from(n * 3), to(n * 3), weight(n, 1.0);
	float turn_f[16];
	turn_matrix(-12, 7, 33, turn_f);
	double turn[9];
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			turn[r * 3 + c] = turn_f[r * 4 + c] / 1.3;
	for (int i = 0; i < n; i++) {
		for (int k = 0; k < 3; k++)
			from[i * 3 + k] = d(rng);
		for (int r = 0; r < 3; r++) {
			to[i * 3 + r] = 1.7 * (turn[r * 3] * from[i * 3] + turn[r * 3 + 1] * from[i * 3 + 1] +
					       turn[r * 3 + 2] * from[i * 3 + 2]) +
					(r == 0   ? 3
					 : r == 1 ? -2
						  : 9);
		}
	}
	double got[9], scale = 0, move[3];
	CHECK(fit(from.data(), to.data(), weight.data(), n, got, scale, move));
	CHECK(near(scale, 1.7, 1e-5));
	for (int i = 0; i < 9; i++)
		CHECK(near(got[i], turn[i], 1e-5));
	CHECK(near(move[0], 3, 1e-4) && near(move[1], -2, 1e-4) && near(move[2], 9, 1e-4));

	Geometry g;
	const uint8_t junk[10] = {0x0a, 0x05};
	CHECK(!g.load(junk, sizeof junk));
	CHECK(!g.loaded());
}

// --- What the avatar shows --------------------------------------------------------

static void test_shown()
{
	Tracker t;
	if (!load(t))
		return;
	t.set_smoothing(-1);
	Face face = t.look(frame_of(portrait()).frame, 0);
	CHECK(face.found);
	const Face lost;
	Rest rest;
	Shown s;
	// Never seen: nothing shown.
	Shown::Out o = s.step(lost, rest, 0.1f);
	CHECK(o.shown == 0 && !o.found && !o.holding);
	// Found: back in BACK seconds.
	o = s.step(face, rest, 0.05f);
	CHECK(o.found && near(o.shown, 0.05 / Shown::BACK, 1e-4));
	o = s.step(face, rest, 0.2f);
	CHECK(o.shown == 1);
	CHECK(o.scale == 1 && o.home_x == 0.5f);
	const Layout l = layout_of(face);
	CHECK(near(o.layout.cx, l.cx, 1e-6));
	// Lost: held as it was for HOLD seconds, then faded over FADE.
	o = s.step(lost, rest, 0.2f);
	CHECK(!o.found && o.holding && o.shown == 1);
	CHECK(near(o.layout.cx, l.cx, 1e-6));
	o = s.step(lost, rest, 0.09f);
	CHECK(o.holding && o.shown == 1);
	o = s.step(lost, rest, 0.1f); // 0.39 s lost
	CHECK(o.holding && o.shown < 1 && o.shown > 0);
	o = s.step(lost, rest, 0.3f);
	CHECK(o.shown == 0 && !o.holding);
	CHECK(o.turned == 0);
	// Back quickly once found again.
	o = s.step(face, rest, 0.1f);
	CHECK(o.found && o.shown > 0.6f && o.shown < 1);

	// Turned pictures: past TURN_ON degrees, back under TURN_OFF.
	Shown turns;
	auto yaw = [&](float degrees) {
		Face f = face;
		f.turn.yaw = degrees;
		return turns.step(f, rest, 0.033f).turned;
	};
	CHECK(yaw(0) == 0);
	CHECK(yaw(18) == 0);
	CHECK(yaw(25) == 1);
	CHECK(yaw(15) == 1);
	CHECK(yaw(10) == 0);
	CHECK(yaw(-25) == -1);
	CHECK(yaw(-13) == -1);
	CHECK(yaw(30) == 1); // straight across
	CHECK(yaw(-15) == 0);

	// The same look again (no new picture) changes nothing but the fade.
	Shown again;
	again.step(face, rest, 0.033f);
	Face moved = face;
	for (auto &p : moved.points)
		p.x += 20;
	o = again.step(moved, rest, 0.033f, false);
	CHECK(near(o.layout.cx, l.cx, 1e-6));
	o = again.step(moved, rest, 0.033f, true);
	CHECK(o.layout.cx > l.cx + 0.05f);

	// Against "Set": the move, size and tilt since then.
	Rest set;
	set.set = true;
	set.x = 0.4f;
	set.y = 0.45f;
	set.gap = l.gap / 2;
	set.angle = l.angle - 0.1f;
	Shown since;
	o = since.step(face, set, 0.033f);
	CHECK(o.home_x == 0.4f && o.home_y == 0.45f);
	CHECK(near(o.scale, 2, 1e-4));
	CHECK(near(o.angle, 0.1, 1e-4));
	since.reset();
	CHECK(since.now().shown == 0 && !since.now().found);
}

// --- Speed --------------------------------------------------------------------------

static void test_speed()
{
	Tracker t;
	if (!load(t))
		return;
	// A face about 200 pixels tall in the biggest picture the add-on hands
	// over (960 x 540).
	const Held f = frame_of(placed(doubled(portrait()), 960, 540, 220, 20));
	std::vector<float> ms;
	for (int i = 0; i < 40; i++) {
		const Face &face = t.look(f.frame, i / 30.0);
		if (i == 0) {
			CHECK(face.found && face.searched);
			std::printf("unit_face: a 960 x 540 look with the face finder: %.1f ms\n", face.ms);
		} else {
			CHECK(face.found && !face.searched);
			ms.push_back(face.ms);
		}
	}
	std::sort(ms.begin(), ms.end());
	std::printf("unit_face: a look following the face: %.1f ms (median of %zu)\n", ms[ms.size() / 2], ms.size());
}

int main()
{
	test_convolutions();
	test_engine_says_no();
	CHECK(portrait().w == 256 && portrait().h == 256);
	test_same_as_mediapipe(false);
	test_same_as_mediapipe(true);
	test_pixel_layouts();
	test_mirrored_camera();
	test_plain_and_fast_agree();
	test_follows_and_loses();
	test_noise_has_no_face();
	test_without_models();
	test_smoothing();
	test_set();
	test_turns();
	test_shown();
	test_speed();
	std::printf("unit_face: %d passed, %d failed\n", passes, failures);
	return failures ? 1 : 0;
}
