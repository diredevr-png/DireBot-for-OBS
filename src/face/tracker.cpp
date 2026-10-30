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
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

#include <nlohmann/json.hpp>

#include "nn/net.hpp"

namespace direbot::face {

using json = nlohmann::json;

const char *const EXPRESSION_NAMES[EXPRESSIONS] = {
	"_neutral",         "browDownLeft",      "browDownRight",      "browInnerUp",
	"browOuterUpLeft",  "browOuterUpRight",  "cheekPuff",          "cheekSquintLeft",
	"cheekSquintRight", "eyeBlinkLeft",      "eyeBlinkRight",      "eyeLookDownLeft",
	"eyeLookDownRight", "eyeLookInLeft",     "eyeLookInRight",     "eyeLookOutLeft",
	"eyeLookOutRight",  "eyeLookUpLeft",     "eyeLookUpRight",     "eyeSquintLeft",
	"eyeSquintRight",   "eyeWideLeft",       "eyeWideRight",       "jawForward",
	"jawLeft",          "jawOpen",           "jawRight",           "mouthClose",
	"mouthDimpleLeft",  "mouthDimpleRight",  "mouthFrownLeft",     "mouthFrownRight",
	"mouthFunnel",      "mouthLeft",         "mouthLowerDownLeft", "mouthLowerDownRight",
	"mouthPressLeft",   "mouthPressRight",   "mouthPucker",        "mouthRight",
	"mouthRollLower",   "mouthRollUpper",    "mouthShrugLower",    "mouthShrugUpper",
	"mouthSmileLeft",   "mouthSmileRight",   "mouthStretchLeft",   "mouthStretchRight",
	"mouthUpperUpLeft", "mouthUpperUpRight", "noseSneerLeft",      "noseSneerRight"};

namespace {

constexpr double PI = 3.14159265358979323846;

// MediaPipe's Face Landmarker's settings (its defaults).
constexpr float FIND_SCORE = 0.5f; // the face finder: this sure, at least
constexpr float SAME_FACE = 0.3f;  // finds overlapping this much are one face
constexpr float PRESENCE = 0.5f;   // the face model: there's a face there
constexpr float GROW = 1.5f;       // the square: this much bigger than the face
// The smoothing at 0.5 on the slider (MediaPipe's own): steady at rest,
// quick to follow a move.
constexpr double MIN_CUTOFF = 0.05, BETA = 80, D_CUTOFF = 1.0;
// "Set" keeps this many looks.
constexpr int REST_LOOKS = 10;

// The 146 points the expressions model reads (MediaPipe's list).
constexpr int SHAPE_POINTS[146] = {0,   1,   4,   5,   6,   7,   8,   10,  13,  14,  17,  21,  33,  37,  39,  40,  46,
				   52,  53,  54,  55,  58,  61,  63,  65,  66,  67,  70,  78,  80,  81,  82,  84,  87,
				   88,  91,  93,  95,  103, 105, 107, 109, 127, 132, 133, 136, 144, 145, 146, 148, 149,
				   150, 152, 153, 154, 155, 157, 158, 159, 160, 161, 162, 163, 168, 172, 173, 176, 178,
				   181, 185, 191, 195, 197, 234, 246, 249, 251, 263, 267, 269, 270, 276, 282, 283, 284,
				   285, 288, 291, 293, 295, 296, 297, 300, 308, 310, 311, 312, 314, 317, 318, 321, 323,
				   324, 332, 334, 336, 338, 356, 361, 362, 365, 373, 374, 375, 377, 378, 379, 380, 381,
				   382, 384, 385, 386, 387, 388, 389, 390, 397, 398, 400, 402, 405, 409, 415, 454, 466,
				   468, 469, 470, 471, 472, 473, 474, 475, 476, 477};

uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// The files in a zip that are stored as they are (Google's .task file
// keeps them that way, so nothing needs unpacking).
struct Entry {
	const uint8_t *data = nullptr;
	size_t size = 0;
};

bool zip_entry(const uint8_t *zip, size_t size, const std::string &name, Entry &out)
{
	if (size < 22)
		return false;
	// The end record, within the last 64 KB (it can carry a comment).
	size_t end = size - 22;
	const size_t stop = size > 22 + 65535 ? size - 22 - 65535 : 0;
	while (le32(zip + end) != 0x06054b50) {
		if (end == stop)
			return false;
		end--;
	}
	const uint16_t count = le16(zip + end + 10);
	size_t at = le32(zip + end + 16);
	for (uint16_t i = 0; i < count; i++) {
		if (at + 46 > size || le32(zip + at) != 0x02014b50)
			return false;
		const uint16_t method = le16(zip + at + 10);
		const uint32_t packed = le32(zip + at + 20), unpacked = le32(zip + at + 24);
		const uint16_t name_len = le16(zip + at + 28), extra_len = le16(zip + at + 30),
			       note_len = le16(zip + at + 32);
		const uint32_t local = le32(zip + at + 42);
		if (at + 46 + name_len > size)
			return false;
		if (name_len == name.size() && std::memcmp(zip + at + 46, name.data(), name_len) == 0) {
			if (method != 0 || packed != unpacked)
				return false;
			if ((size_t)local + 30 > size || le32(zip + local) != 0x04034b50)
				return false;
			const size_t data = (size_t)local + 30 + le16(zip + local + 26) + le16(zip + local + 28);
			if (data > size || packed > size - data)
				return false;
			out.data = zip + data;
			out.size = packed;
			return true;
		}
		at += 46 + (size_t)name_len + extra_len + note_len;
	}
	return false;
}

size_t elements(const std::vector<int> &shape)
{
	size_t n = 1;
	for (int d : shape)
		n *= (size_t)std::max(0, d);
	return n;
}

float sigmoid(float x)
{
	return 1 / (1 + std::exp(-x));
}

float wrap_angle(float a)
{
	return (float)(a - 2 * PI * std::floor((a + PI) / (2 * PI)));
}

// A turned box in the picture: middle, size (pixels) and turn (radians,
// clockwise as the picture shows it).
using Square = Tracker::Square;

// Cuts `sq` out of the picture at out_w x out_h, the way MediaPipe does
// it (OpenCV's warp with bilinear sampling: 1/32 pixel steps, weights in
// 15 bits, rounded to whole 0-255 values), then each value * scale +
// offset as floats, red, green, blue. Outside the picture: black, or
// the nearest edge pixel.
void cut(const Frame &pic, const Square &sq, int out_w, int out_h, bool black_edges, float scale, float offset,
	 float *out)
{
	const double c = std::cos((double)sq.angle), s = std::sin((double)sq.angle);
	const double sx = sq.w / out_w, sy = sq.h / out_h;
	// out (u, v) -> picture: middle + turn * (u * sx - w/2, v * sy - h/2).
	const double m0 = c * sx, m1 = -s * sy, m2 = sq.cx - c * sq.w / 2 + s * sq.h / 2;
	const double m3 = s * sx, m4 = c * sy, m5 = sq.cy - s * sq.w / 2 - c * sq.h / 2;
	const int r = pic.bgra ? 2 : 0, b = pic.bgra ? 0 : 2;
	const int w = pic.w, h = pic.h;
	for (int v = 0; v < out_h; v++) {
		const double x0 = m1 * v + m2, y0 = m4 * v + m5;
		float *o = out + (size_t)v * out_w * 3;
		for (int u = 0; u < out_w; u++, o += 3) {
			const long X = std::lrint((x0 + m0 * u) * 32), Y = std::lrint((y0 + m3 * u) * 32);
			const long ix = X >> 5, iy = Y >> 5;
			const int fx = (int)(X & 31), fy = (int)(Y & 31);
			const int w00 = (32 - fx) * (32 - fy) * 32, w01 = fx * (32 - fy) * 32;
			const int w10 = (32 - fx) * fy * 32, w11 = fx * fy * 32;
			const uint8_t *p00, *p01, *p10, *p11;
			static const uint8_t zero[4] = {0, 0, 0, 0};
			if (ix >= 0 && ix < w - 1 && iy >= 0 && iy < h - 1) {
				p00 = pic.px + (size_t)iy * pic.stride + (size_t)ix * 4;
				p01 = p00 + 4;
				p10 = p00 + pic.stride;
				p11 = p10 + 4;
			} else if (black_edges) {
				if (ix >= w || ix + 1 < 0 || iy >= h || iy + 1 < 0) {
					o[0] = o[1] = o[2] = offset;
					continue;
				}
				auto at = [&](long x, long y) {
					return x >= 0 && x < w && y >= 0 && y < h
						       ? pic.px + (size_t)y * pic.stride + (size_t)x * 4
						       : zero;
				};
				p00 = at(ix, iy);
				p01 = at(ix + 1, iy);
				p10 = at(ix, iy + 1);
				p11 = at(ix + 1, iy + 1);
			} else {
				auto at = [&](long x, long y) {
					x = std::min<long>(std::max<long>(x, 0), w - 1);
					y = std::min<long>(std::max<long>(y, 0), h - 1);
					return pic.px + (size_t)y * pic.stride + (size_t)x * 4;
				};
				p00 = at(ix, iy);
				p01 = at(ix + 1, iy);
				p10 = at(ix, iy + 1);
				p11 = at(ix + 1, iy + 1);
			}
			const int ch[3] = {r, 1, b};
			for (int k = 0; k < 3; k++) {
				const int i = ch[k];
				const int sum = p00[i] * w00 + p01[i] * w01 + p10[i] * w10 + p11[i] * w11;
				const int value = std::min(255, std::max(0, (sum + (1 << 14)) >> 15));
				o[k] = (float)value * scale + offset;
			}
		}
	}
}

// MediaPipe's One Euro filter (G. Casiez et al., 2012): a low-pass filter
// whose cut-off rises with how fast the value moves.
struct LowPass {
	bool started = false;
	float raw = 0, kept = 0;
	float apply(float value, double alpha)
	{
		const float result = started ? (float)(alpha * value + (1 - alpha) * kept) : value;
		started = true;
		raw = value;
		kept = result;
		return result;
	}
};

struct OneEuro {
	double hz = 30;
	int64_t last = -1;
	LowPass x, dx;

	static double alpha(double hz, double cutoff)
	{
		const double te = 1 / hz, tau = 1 / (2 * PI * cutoff);
		return 1 / (1 + tau / te);
	}
	float apply(int64_t ns, double scale, float value, double min_cutoff, double beta)
	{
		if (last >= ns)
			return value;
		if (last != 0 && ns != 0)
			hz = 1e9 / (double)(ns - last);
		last = ns;
		const double speed = x.started ? (value - x.raw) * scale * hz : 0;
		const double eased = dx.apply((float)speed, alpha(hz, D_CUTOFF));
		const double cutoff = min_cutoff + beta * std::fabs(eased);
		return x.apply(value, alpha(hz, cutoff));
	}
};

} // namespace

struct Tracker::Impl {
	nn::Net finder, mesh, shapes;
	Geometry geometry;
	bool ready = false;
	int find_size = 128, mesh_size = 256;
	std::vector<float> anchors; // x, y each (shares of the finder's square)

	int pic_w = 0, pic_h = 0;
	bool following = false;
	Square square;
	std::vector<Found> found;
	Face face;
	Point raw[POINTS];

	float amount = 0.5f;
	OneEuro filters[POINTS][3];
	bool filtering = false;

	// "Set"
	Rest rest;
	int setting = 0; // looks still to keep (0: not setting)
	int kept = 0;
	double sum_pose[16] = {};
	double sum_rest[EXPRESSIONS] = {};
	double sum_x = 0, sum_y = 0, sum_gap = 0, sum_angle = 0;

	bool load(const uint8_t *bytes, size_t size, std::string *problem);
	void make_anchors();
	bool find(const Frame &pic);
	bool points(const Frame &pic);
	void smooth(double seconds);
	void lost();
	void keep_rest();
};

namespace {

bool fail(std::string *problem, const std::string &why)
{
	if (problem)
		*problem = why;
	return false;
}

} // namespace

bool Tracker::Impl::load(const uint8_t *bytes, size_t size, std::string *problem)
{
	ready = false;
	Entry f, m, s, g;
	if (!zip_entry(bytes, size, "face_detector.tflite", f) ||
	    !zip_entry(bytes, size, "face_landmarks_detector.tflite", m) ||
	    !zip_entry(bytes, size, "face_blendshapes.tflite", s) ||
	    !zip_entry(bytes, size, "geometry_pipeline_metadata_landmarks.binarypb", g))
		return fail(problem, "the face models file isn't Google's face_landmarker.task");
	std::string why;
	if (!finder.load(f.data, f.size, &why))
		return fail(problem, "the face finder model: " + why);
	if (!mesh.load(m.data, m.size, &why))
		return fail(problem, "the face mesh model: " + why);
	if (!shapes.load(s.data, s.size, &why))
		return fail(problem, "the expressions model: " + why);
	if (!geometry.load(g.data, g.size, &why))
		return fail(problem, why);
	// The shapes this code expects.
	const auto &fi = finder.input_shape(0), &mi = mesh.input_shape(0);
	if (finder.inputs() != 1 || fi.size() != 4 || fi[1] != fi[2] || fi[3] != 3 || finder.outputs() != 2 ||
	    mesh.inputs() != 1 || mi.size() != 4 || mi[1] != mi[2] || mi[3] != 3 || mesh.outputs() < 2 ||
	    elements(mesh.output_shape(0)) != (size_t)POINTS * 3 || elements(mesh.output_shape(1)) != 1 ||
	    shapes.inputs() != 1 || elements(shapes.input_shape(0)) != 146 * 2 || shapes.outputs() != 1 ||
	    elements(shapes.output_shape(0)) != EXPRESSIONS)
		return fail(problem, "the face models aren't the ones expected");
	find_size = fi[1];
	mesh_size = mi[1];
	make_anchors();
	const size_t boxes = anchors.size() / 2;
	const auto &o0 = finder.output_shape(0), &o1 = finder.output_shape(1);
	if (!((elements(o0) == boxes * 16 && elements(o1) == boxes) ||
	      (elements(o1) == boxes * 16 && elements(o0) == boxes)))
		return fail(problem, "the face finder model isn't the one expected");
	ready = true;
	return true;
}

// The face finder's boxes start from these (MediaPipe's SSD anchors for
// the short-range finder): 2 per spot on a 16 x 16 grid, then 6 per spot
// on an 8 x 8 one.
void Tracker::Impl::make_anchors()
{
	anchors.clear();
	const int strides[4] = {8, 16, 16, 16};
	int layer = 0;
	while (layer < 4) {
		int same = layer, per_spot = 0;
		while (same < 4 && strides[same] == strides[layer]) {
			per_spot += 2; // aspect ratio 1, and the one between sizes
			same++;
		}
		const int grid = (int)std::ceil((double)find_size / strides[layer]);
		for (int y = 0; y < grid; y++)
			for (int x = 0; x < grid; x++)
				for (int k = 0; k < per_spot; k++) {
					anchors.push_back((x + 0.5f) / grid);
					anchors.push_back((y + 0.5f) / grid);
				}
		layer = same;
	}
}

// Step 1: the whole picture, fitted into the finder's square with black
// bars, then its boxes: the sure ones, overlapping ones merged (weighted
// by how sure), and the biggest one followed.
bool Tracker::Impl::find(const Frame &pic)
{
	found.clear();
	const float side = (float)std::max(pic.w, pic.h);
	const Square whole{pic.w / 2.0f, pic.h / 2.0f, side, side, 0};
	cut(pic, whole, find_size, find_size, true, 2.0f / 255, -1, finder.input(0));
	finder.run();
	const size_t boxes = anchors.size() / 2;
	const bool first_is_boxes = elements(finder.output_shape(0)) == boxes * 16;
	const float *raw = finder.output(first_is_boxes ? 0 : 1), *scores = finder.output(first_is_boxes ? 1 : 0);

	struct Box {
		float x0, y0, x1, y1, score;
		float keys[12];
	};
	std::vector<Box> sure;
	const float size = (float)find_size;
	for (size_t i = 0; i < boxes; i++) {
		const float score = sigmoid(std::min(100.0f, std::max(-100.0f, scores[i])));
		if (score < FIND_SCORE)
			continue;
		const float *r = raw + i * 16;
		const float ax = anchors[i * 2], ay = anchors[i * 2 + 1];
		const float cx = r[0] / size + ax, cy = r[1] / size + ay, w = r[2] / size, h = r[3] / size;
		if (w < 0 || h < 0)
			continue;
		Box b{cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2, score, {}};
		for (int k = 0; k < 6; k++) {
			b.keys[k * 2] = r[4 + k * 2] / size + ax;
			b.keys[k * 2 + 1] = r[4 + k * 2 + 1] / size + ay;
		}
		sure.push_back(b);
	}
	std::stable_sort(sure.begin(), sure.end(), [](const Box &a, const Box &b) { return a.score > b.score; });
	auto overlap = [](const Box &a, const Box &b) {
		const float ix = std::min(a.x1, b.x1) - std::max(a.x0, b.x0),
			    iy = std::min(a.y1, b.y1) - std::max(a.y0, b.y0);
		if (ix <= 0 || iy <= 0)
			return 0.0f;
		const float both = ix * iy, all = (a.x1 - a.x0) * (a.y1 - a.y0) + (b.x1 - b.x0) * (b.y1 - b.y0) - both;
		return all > 0 ? both / all : 0.0f;
	};
	std::vector<Box> faces;
	while (!sure.empty()) {
		const Box top = sure[0];
		std::vector<Box> rest;
		Box merged = top;
		float total = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0, keys[12] = {};
		for (const Box &b : sure) {
			if (overlap(b, top) > SAME_FACE) {
				total += b.score;
				x0 += b.x0 * b.score;
				y0 += b.y0 * b.score;
				x1 += b.x1 * b.score;
				y1 += b.y1 * b.score;
				for (int k = 0; k < 12; k++)
					keys[k] += b.keys[k] * b.score;
			} else {
				rest.push_back(b);
			}
		}
		if (total > 0) {
			merged.x0 = x0 / total;
			merged.y0 = y0 / total;
			merged.x1 = x1 / total;
			merged.y1 = y1 / total;
			for (int k = 0; k < 12; k++)
				merged.keys[k] = keys[k] / total;
		}
		faces.push_back(merged);
		if (rest.size() == sure.size())
			break;
		sure.swap(rest);
	}
	// Back to the picture's pixels.
	const float left = whole.cx - side / 2, top = whole.cy - side / 2;
	for (const Box &b : faces) {
		Found f;
		f.x = left + b.x0 * side;
		f.y = top + b.y0 * side;
		f.w = (b.x1 - b.x0) * side;
		f.h = (b.y1 - b.y0) * side;
		f.score = b.score;
		for (int k = 0; k < 6; k++) {
			f.keys[k * 2] = left + b.keys[k * 2] * side;
			f.keys[k * 2 + 1] = top + b.keys[k * 2 + 1] * side;
		}
		found.push_back(f);
	}
	if (found.empty())
		return false;
	const Found *best = &found[0];
	for (const Found &f : found)
		if (f.w * f.h > best->w * best->h)
			best = &f;
	// The square: the box, turned so the eyes (its first two points) are
	// level, half as big again.
	square.cx = best->x + best->w / 2;
	square.cy = best->y + best->h / 2;
	square.angle = wrap_angle(-std::atan2(-(best->keys[3] - best->keys[1]), best->keys[2] - best->keys[0]));
	square.w = best->w * GROW;
	square.h = best->h * GROW;
	return true;
}

// Step 2: the 478 points in the square, and whether there's a face there.
// Then the square for the next look, from the points: around them all,
// turned by the eyes' outer corners, made square, half as big again.
bool Tracker::Impl::points(const Frame &pic)
{
	cut(pic, square, mesh_size, mesh_size, false, 1.0f / 255, 0, mesh.input(0));
	mesh.run();
	face.presence = sigmoid(mesh.output(1)[0]);
	if (!(face.presence >= PRESENCE))
		return false;
	const float *out = mesh.output(0);
	const float c = std::cos(square.angle), s = std::sin(square.angle);
	// z on x's scale, as MediaPipe has it.
	const float zs = pic.w * std::sqrt(std::pow(square.w * c / pic.w, 2.0f) + std::pow(square.w * s / pic.h, 2.0f));
	const float size = (float)mesh_size;
	float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
	for (int i = 0; i < POINTS; i++) {
		const float x = (out[i * 3] / size - 0.5f) * square.w, y = (out[i * 3 + 1] / size - 0.5f) * square.h;
		Point &p = raw[i];
		p.x = c * x - s * y + square.cx;
		p.y = s * x + c * y + square.cy;
		p.z = out[i * 3 + 2] / size * zs;
		x0 = std::min(x0, p.x);
		y0 = std::min(y0, p.y);
		x1 = std::max(x1, p.x);
		y1 = std::max(y1, p.y);
	}
	const Point &a = raw[RIGHT_EYE_OUTER], &b = raw[LEFT_EYE_OUTER];
	square.cx = (x0 + x1) / 2;
	square.cy = (y0 + y1) / 2;
	square.angle = wrap_angle(-std::atan2(-(b.y - a.y), b.x - a.x));
	square.w = square.h = std::max(x1 - x0, y1 - y0) * GROW;
	return true;
}

// Step 4: each point's x, y and z through a One Euro filter, its speed
// measured in face sizes a second.
void Tracker::Impl::smooth(double seconds)
{
	if (amount < 0) {
		std::copy(std::begin(raw), std::end(raw), face.points);
		return;
	}
	if (!filtering) {
		for (auto &f : filters)
			for (auto &k : f)
				k = OneEuro();
		filtering = true;
	}
	float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
	for (const Point &p : raw) {
		x0 = std::min(x0, p.x);
		y0 = std::min(y0, p.y);
		x1 = std::max(x1, p.x);
		y1 = std::max(y1, p.y);
	}
	const float size = ((x1 - x0) + (y1 - y0)) / 2;
	if (!(size >= 1e-6f)) {
		std::copy(std::begin(raw), std::end(raw), face.points);
		return;
	}
	// The slider: steadier at rest and slower to follow one way, the
	// other way snappier; 0.5 is MediaPipe's.
	const double t = std::min(1.0f, std::max(0.0f, amount)) * 2 - 1;
	const double min_cutoff = MIN_CUTOFF * std::pow(5.0, t), beta = BETA * std::pow(3.0, t);
	const int64_t ns = (int64_t)std::llround(seconds * 1e9);
	const double scale = 1 / size;
	for (int i = 0; i < POINTS; i++) {
		face.points[i].x = filters[i][0].apply(ns, scale, raw[i].x, min_cutoff, beta);
		face.points[i].y = filters[i][1].apply(ns, scale, raw[i].y, min_cutoff, beta);
		face.points[i].z = filters[i][2].apply(ns, scale, raw[i].z, min_cutoff, beta);
	}
}

void Tracker::Impl::lost()
{
	following = false;
	filtering = false;
	face.found = false;
}

void Tracker::Impl::keep_rest()
{
	for (int i = 0; i < 16; i++)
		sum_pose[i] += face.pose[i];
	for (int i = 0; i < EXPRESSIONS; i++)
		sum_rest[i] += face.expressions[i];
	const Layout l = layout_of(face);
	sum_x += l.cx;
	sum_y += l.cy;
	sum_gap += l.gap;
	sum_angle += l.angle;
	kept++;
	if (--setting > 0)
		return;
	float *rest_pose = rest.pose;
	// The average turn, made a turn again (Gram-Schmidt on its columns).
	double col[3][3];
	for (int c = 0; c < 3; c++)
		for (int r = 0; r < 3; r++)
			col[c][r] = sum_pose[r * 4 + c];
	auto dot = [](const double *a, const double *b) {
		return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
	};
	auto unit = [&](double *a) {
		const double n = std::sqrt(dot(a, a));
		for (int k = 0; k < 3; k++)
			a[k] = n > 0 ? a[k] / n : 0;
	};
	unit(col[0]);
	const double d01 = dot(col[0], col[1]);
	for (int k = 0; k < 3; k++)
		col[1][k] -= d01 * col[0][k];
	unit(col[1]);
	col[2][0] = col[0][1] * col[1][2] - col[0][2] * col[1][1];
	col[2][1] = col[0][2] * col[1][0] - col[0][0] * col[1][2];
	col[2][2] = col[0][0] * col[1][1] - col[0][1] * col[1][0];
	for (int c = 0; c < 3; c++)
		for (int r = 0; r < 3; r++)
			rest_pose[r * 4 + c] = (float)col[c][r];
	for (int r = 0; r < 3; r++)
		rest_pose[r * 4 + 3] = (float)(sum_pose[r * 4 + 3] / kept);
	rest_pose[12] = rest_pose[13] = rest_pose[14] = 0;
	rest_pose[15] = 1;
	for (int i = 0; i < EXPRESSIONS; i++)
		rest.expressions[i] = (float)(sum_rest[i] / kept);
	rest.x = (float)(sum_x / kept);
	rest.y = (float)(sum_y / kept);
	rest.gap = (float)(sum_gap / kept);
	rest.angle = (float)(sum_angle / kept);
	rest.set = true;
}

Tracker::Tracker() : impl_(std::make_unique<Impl>()) {}
Tracker::~Tracker() = default;

bool Tracker::load(const uint8_t *bytes, size_t size, std::string *problem)
{
	forget();
	return impl_->load(bytes, size, problem);
}

bool Tracker::load_file(const std::string &path, std::string *problem)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return fail(problem, "the face models file is missing (" + path + ")");
	std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	return load(bytes.data(), bytes.size(), problem);
}

bool Tracker::ready() const
{
	return impl_->ready;
}

const Face &Tracker::look(const Frame &pic, double seconds)
{
	Impl &m = *impl_;
	const auto start = std::chrono::steady_clock::now();
	Face &face = m.face;
	face.searched = false;
	if (!m.ready || !pic.px || pic.w < 2 || pic.h < 2 || pic.stride < pic.w * 4) {
		m.lost();
		face.presence = 0;
		return face;
	}
	if (pic.w != m.pic_w || pic.h != m.pic_h) {
		m.pic_w = pic.w;
		m.pic_h = pic.h;
		m.lost();
	}
	if (!m.following) {
		face.searched = true;
		face.presence = 0;
		m.following = m.find(pic);
	}
	if (m.following && m.points(pic)) {
		m.smooth(seconds);
		// Step 5: the expressions, from the smoothed points.
		float *in = m.shapes.input(0);
		for (int i = 0; i < 146; i++) {
			in[i * 2] = face.points[SHAPE_POINTS[i]].x;
			in[i * 2 + 1] = face.points[SHAPE_POINTS[i]].y;
		}
		m.shapes.run();
		std::copy(m.shapes.output(0), m.shapes.output(0) + EXPRESSIONS, face.expressions);
		// Step 6: the head in 3D.
		m.geometry.pose(&face.points[0].x, pic.w, pic.h, face.pose);
		face.found = true;
		face.w = pic.w;
		face.h = pic.h;
		if (m.setting > 0)
			m.keep_rest();
		face.turn = m.rest.set ? turn_since(face.pose, m.rest.pose) : turn_of(face.pose);
		for (int i = 0; i < EXPRESSIONS; i++) {
			const float r = m.rest.set ? m.rest.expressions[i] : 0;
			face.moves[i] = r < 0.98f ? std::min(1.0f, std::max(0.0f, (face.expressions[i] - r) / (1 - r)))
						  : 0;
		}
	} else {
		m.lost();
	}
	face.ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
	return face;
}

const Face &Tracker::face() const
{
	return impl_->face;
}

void Tracker::forget()
{
	impl_->lost();
}

void Tracker::set_smoothing(float amount)
{
	impl_->amount = amount;
}

void Tracker::set_rest()
{
	Impl &m = *impl_;
	m.setting = REST_LOOKS;
	m.kept = 0;
	std::fill(std::begin(m.sum_pose), std::end(m.sum_pose), 0.0);
	std::fill(std::begin(m.sum_rest), std::end(m.sum_rest), 0.0);
	m.sum_x = m.sum_y = m.sum_gap = m.sum_angle = 0;
}

bool Tracker::setting() const
{
	return impl_->setting > 0;
}

bool Tracker::has_rest() const
{
	return impl_->rest.set;
}

void Tracker::clear_rest()
{
	impl_->rest = Rest();
	impl_->setting = 0;
}

const Rest &Tracker::rest() const
{
	return impl_->rest;
}

namespace {

float rounded(float v, float steps)
{
	return std::round(v * steps) / steps;
}

} // namespace

std::string Tracker::save_rest() const
{
	const Rest &r = impl_->rest;
	if (!r.set)
		return std::string();
	json pose = json::array(), rest = json::object();
	for (int i = 0; i < 12; i++)
		pose.push_back(rounded(r.pose[i], 1e5f));
	for (int i = 0; i < EXPRESSIONS; i++)
		rest[EXPRESSION_NAMES[i]] = rounded(r.expressions[i], 1e4f);
	return json{{"v", 1},
		    {"pose", pose},
		    {"rest", rest},
		    {"x", rounded(r.x, 1e5f)},
		    {"y", rounded(r.y, 1e5f)},
		    {"gap", rounded(r.gap, 1e5f)},
		    {"angle", rounded(r.angle, 1e5f)}}
		.dump();
}

bool Tracker::load_rest(const std::string &text)
{
	const json j = json::parse(text, nullptr, false);
	if (!j.is_object() || !j.contains("pose") || !j["pose"].is_array() || j["pose"].size() != 12 ||
	    !j.contains("rest") || !j["rest"].is_object())
		return false;
	Rest r;
	for (int i = 0; i < 12; i++) {
		if (!j["pose"][i].is_number())
			return false;
		r.pose[i] = j["pose"][i].get<float>();
	}
	r.pose[12] = r.pose[13] = r.pose[14] = 0;
	r.pose[15] = 1;
	for (int i = 0; i < EXPRESSIONS; i++) {
		const auto it = j["rest"].find(EXPRESSION_NAMES[i]);
		if (it != j["rest"].end() && it->is_number())
			r.expressions[i] = std::min(1.0f, std::max(0.0f, it->get<float>()));
	}
	auto number = [&](const char *key, float fallback, float lo, float hi) {
		const auto it = j.find(key);
		return it != j.end() && it->is_number() ? std::min(hi, std::max(lo, it->get<float>())) : fallback;
	};
	r.x = number("x", 0.5f, -1, 2);
	r.y = number("y", 0.5f, -1, 2);
	r.gap = number("gap", 0, 0, 2);
	r.angle = number("angle", 0, -4, 4);
	r.set = true;
	impl_->rest = r;
	impl_->setting = 0;
	return true;
}

const Geometry &Tracker::geometry() const
{
	return impl_->geometry;
}

void Tracker::set_plain(bool plain)
{
	impl_->finder.set_plain(plain);
	impl_->mesh.set_plain(plain);
	impl_->shapes.set_plain(plain);
}

const std::vector<Tracker::Found> &Tracker::found() const
{
	return impl_->found;
}

bool Tracker::following(Square *square) const
{
	if (square)
		*square = impl_->square;
	return impl_->following;
}

Layout layout_of(const Face &face)
{
	Layout l;
	if (!face.found || face.w <= 0 || face.h <= 0)
		return l;
	const float w = (float)face.w, h = (float)face.h;
	auto at = [&](int i) {
		return face.points[i];
	};
	auto mid = [](const Point &a, const Point &b) {
		return Point{(a.x + b.x) / 2, (a.y + b.y) / 2, 0};
	};
	auto dist = [](const Point &a, const Point &b) {
		return std::hypot(a.x - b.x, a.y - b.y);
	};
	// Each eye from its corners (they hold still while the eye moves and
	// blinks), with room for its lids; the mouth from its corners and
	// lips, taller as it opens.
	const Point right = mid(at(RIGHT_EYE_OUTER), at(RIGHT_EYE_INNER)),
		    left = mid(at(LEFT_EYE_OUTER), at(LEFT_EYE_INNER));
	const float right_w = dist(at(RIGHT_EYE_OUTER), at(RIGHT_EYE_INNER)),
		    left_w = dist(at(LEFT_EYE_OUTER), at(LEFT_EYE_INNER));
	const bool right_first = right.x <= left.x;
	const Point &first = right_first ? right : left, &second = right_first ? left : right;
	const float first_w = right_first ? right_w : left_w, second_w = right_first ? left_w : right_w;
	const Point corners = mid(at(MOUTH_RIGHT), at(MOUTH_LEFT)), lips = mid(at(UPPER_LIP_TOP), at(LOWER_LIP_BOTTOM));
	const float mouth_w = dist(at(MOUTH_RIGHT), at(MOUTH_LEFT)),
		    mouth_h = dist(at(UPPER_LIP_TOP), at(LOWER_LIP_BOTTOM));
	auto part = [&](float x, float y, float pw, float ph) {
		return Part{(x + 0.5f) / w, (y + 0.5f) / h, pw / w, ph / h};
	};
	l.parts[FIRST_EYE] = part(first.x, first.y, first_w * 1.9f, first_w * 1.25f);
	l.parts[SECOND_EYE] = part(second.x, second.y, second_w * 1.9f, second_w * 1.25f);
	l.parts[MOUTH] = part(corners.x, lips.y, mouth_w * 1.5f, std::max(mouth_w * 0.7f, mouth_h * 1.5f));
	l.cx = (l.parts[0].x + l.parts[1].x + l.parts[2].x) / 3;
	l.cy = (l.parts[0].y + l.parts[1].y + l.parts[2].y) / 3;
	l.gap = dist(first, second) / w;
	l.angle = std::atan2(second.y - first.y, second.x - first.x);
	return l;
}

Shown::Out Shown::step(const Face &face, const Rest &rest, float dt, bool new_look)
{
	dt = std::max(0.0f, dt);
	Out &o = out_;
	o.home_x = rest.set ? rest.x : 0.5f;
	o.home_y = rest.set ? rest.y : 0.5f;
	if (face.found) {
		if (new_look || !seen_) {
			o.layout = layout_of(face);
			o.scale = rest.set && rest.gap > 0 ? o.layout.gap / rest.gap : 1;
			o.angle = wrap_angle(o.layout.angle - (rest.set ? rest.angle : 0));
			o.turn = face.turn;
			std::copy(std::begin(face.moves), std::end(face.moves), o.moves);
			const float yaw = face.turn.yaw;
			if (o.turned == 0 && std::fabs(yaw) > TURN_ON)
				o.turned = yaw < 0 ? -1 : 1;
			else if (o.turned != 0 && (std::fabs(yaw) < TURN_OFF || (yaw < 0) != (o.turned < 0)))
				o.turned = std::fabs(yaw) > TURN_ON ? (yaw < 0 ? -1 : 1) : 0;
		}
		seen_ = true;
		lost_for_ = 0;
		o.found = true;
		o.holding = false;
		o.shown = std::min(1.0f, o.shown + dt / BACK);
		return o;
	}
	o.found = false;
	if (!seen_) {
		o.shown = 0;
		return o;
	}
	// Lost: held as it was, then faded.
	lost_for_ += dt;
	o.holding = o.shown > 0;
	if (lost_for_ > HOLD)
		o.shown = std::max(0.0f, o.shown - dt / FADE);
	if (o.shown <= 0) {
		o.holding = false;
		o.turned = 0;
		o.turn = Turn();
		std::fill(std::begin(o.moves), std::end(o.moves), 0.0f);
	}
	return o;
}

void Shown::reset()
{
	out_ = Out();
	lost_for_ = 0;
	seen_ = false;
}

} // namespace direbot::face
