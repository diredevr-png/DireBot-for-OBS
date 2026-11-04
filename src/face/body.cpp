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

#include "body.hpp"
#include "files.hpp"
#include "vision.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <vector>

#include "nn/net.hpp"

namespace direbot::face {

namespace {

using vision::Box;
using vision::elements;
using vision::OneEuro;
using vision::sigmoid;
using vision::wrap_angle;
constexpr float PI_F = (float)vision::PI;

// MediaPipe's Holistic Landmarker's settings (its defaults).
constexpr float FIND_SCORE = 0.5f;   // the body finder: this sure, at least
constexpr float SAME_BODY = 0.3f;    // finds overlapping this much are one body
constexpr float PRESENCE = 0.5f;     // the body model: a body is there
constexpr float BODY_GROW = 1.25f;   // the body's square: this much bigger
constexpr int HEAT_KERNEL = 7;       // the heat map's sharper guess: this many cells across...
constexpr float HEAT_SURE = 0.5f;    // ...when it's this sure
constexpr float HAND_GROW = 2.7f;    // the hand's square from the body's points
constexpr float HAND_AGAIN = 2.0f;   // the hand's square from where it was
constexpr float HAND_SHIFT = -0.1f;  // both moved this much of their size towards the fingers
constexpr float HAND_PRESENCE = 0.5f; // the hand model: a hand is there
constexpr float HAND_Z = 0.4f;        // the hand model's depth, as MediaPipe scales it
// Following a hand from where it was, while it's within this of where
// the body says it is (turn in degrees, move and size as shares).
constexpr float TRACK_TURN = 40, TRACK_MOVE = 0.2f, TRACK_SIZE = 0.4f, TRACK_MARGIN = -0.1f;
// The smoothing at 0.5 on the slider (MediaPipe's own for the body: its
// points, the two that place the next look's square, and the points in
// metres).
constexpr double MIN_CUTOFF = 0.05, BETA = 80;
constexpr double AUX_MIN_CUTOFF = 0.01, AUX_BETA = 10;
constexpr double WORLD_MIN_CUTOFF = 0.1, WORLD_BETA = 40;
// How likely each point is to be in sight, smoothed over about this long
// (seconds).
constexpr float SEEN_TIME = 0.15f;
// Our own checks on a hand (a "ghost" hand). A hand is looked for when
// the body model thinks its wrist is at least this likely in sight
// (MediaPipe looks from 0.1, but a hand at a wrist it thinks is hidden -
// your hands down at the desk, out of the picture - is more often a
// ghost than a hand); its wrist must be within this many of its palm's
// lengths (wrist to middle knuckle) of the body's wrist (a hand gripping
// your other arm, its wrist hidden, is a palm and a bit off); two hands
// overlapping this much are one hand found twice.
constexpr float WRIST_SEEN = 0.3f;
constexpr float WRIST_FAR = 2.0f;
constexpr float SAME_HAND = 0.5f;

// The body finder's anchors (MediaPipe's SSD anchors for it): 2 per spot
// on a 28 x 28 grid, 2 on a 14 x 14, then 6 on a 7 x 7.
std::vector<float> body_anchors(int size)
{
	std::vector<float> anchors;
	const int strides[5] = {8, 16, 32, 32, 32};
	int layer = 0;
	while (layer < 5) {
		int same = layer, per_spot = 0;
		while (same < 5 && strides[same] == strides[layer]) {
			per_spot += 2; // aspect ratio 1, and the one between sizes
			same++;
		}
		const int grid = (int)std::ceil((double)size / strides[layer]);
		for (int y = 0; y < grid; y++)
			for (int x = 0; x < grid; x++)
				for (int k = 0; k < per_spot; k++) {
					anchors.push_back((x + 0.5f) / grid);
					anchors.push_back((y + 0.5f) / grid);
				}
		layer = same;
	}
	return anchors;
}

// A point given as shares of a box (0 to 1 across it and down it), in
// the picture.
Point in_picture(const Box &b, float u, float v)
{
	const float c = std::cos(b.angle), s = std::sin(b.angle);
	const float x = (u - 0.5f) * b.w, y = (v - 0.5f) * b.h;
	return {c * x - s * y + b.cx, s * x + c * y + b.cy, 0};
}

// MediaPipe's square from two points: round the first, twice as big as
// it is to the second, turned so the first-to-second way points `target`
// (radians: pi/2 up, -pi/2 down).
Box box_of_two(Point a, Point b, float target)
{
	const float size = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y)) * 2;
	return {a.x, a.y, size, size, wrap_angle(target - std::atan2(-(b.y - a.y), b.x - a.x))};
}

// MediaPipe's rect transformation: moved `shift` of its height along its
// own down, made square on its longer side, then grown `grow` times.
Box shift_square(Box b, float grow, float shift)
{
	b.cx += -b.h * shift * std::sin(b.angle);
	b.cy += b.h * shift * std::cos(b.angle);
	const float side = std::max(b.w, b.h);
	b.w = b.h = side * grow;
	return b;
}

// MediaPipe's hand square from the body: the wrist, little finger and
// first finger points; the middle finger guessed between them.
Box hand_box_from_body(Point wrist, Point little, Point index)
{
	const Point middle{(2 * index.x + little.x) / 3, (2 * index.y + little.y) / 3, 0};
	const float size = std::sqrt((middle.x - wrist.x) * (middle.x - wrist.x) +
				     (middle.y - wrist.y) * (middle.y - wrist.y)) *
			   2;
	const Box b{middle.x, middle.y, size, size,
		    wrap_angle(PI_F / 2 - std::atan2(-(middle.y - wrist.y), middle.x - wrist.x))};
	return shift_square(b, HAND_GROW, HAND_SHIFT);
}

// MediaPipe's hand square from a hand's points (its palm's twelve),
// turned with the hand, round them all.
Box hand_box_from_points(const Point *p, int w, int h)
{
	static const int PALM[12] = {0, 1, 2, 3, 5, 6, 9, 10, 13, 14, 17, 18};
	const float W = (float)w, H = (float)h;
	const float x0 = p[0].x, y0 = p[0].y;
	const float x1 = ((p[5].x + p[13].x) / 2 + p[9].x) / 2, y1 = ((p[5].y + p[13].y) / 2 + p[9].y) / 2;
	const float rotation = wrap_angle(PI_F / 2 - std::atan2(-(y1 - y0), x1 - x0));
	const float reverse = wrap_angle(-rotation);
	// As MediaPipe has it, in shares of the picture (its largest start at
	// the smallest positive number, as theirs do).
	float max_x = std::numeric_limits<float>::min(), max_y = std::numeric_limits<float>::min();
	float min_x = std::numeric_limits<float>::max(), min_y = std::numeric_limits<float>::max();
	for (int i : PALM) {
		max_x = std::max(max_x, p[i].x / W);
		max_y = std::max(max_y, p[i].y / H);
		min_x = std::min(min_x, p[i].x / W);
		min_y = std::min(min_y, p[i].y / H);
	}
	const float acx = (max_x + min_x) / 2, acy = (max_y + min_y) / 2;
	max_x = max_y = std::numeric_limits<float>::min();
	min_x = min_y = std::numeric_limits<float>::max();
	const float cr = std::cos(reverse), sr = std::sin(reverse);
	for (int i : PALM) {
		const float ox = (p[i].x / W - acx) * W, oy = (p[i].y / H - acy) * H;
		const float px = ox * cr - oy * sr, py = ox * sr + oy * cr;
		max_x = std::max(max_x, px);
		max_y = std::max(max_y, py);
		min_x = std::min(min_x, px);
		min_y = std::min(min_y, py);
	}
	const float pcx = (max_x + min_x) / 2, pcy = (max_y + min_y) / 2;
	const float c = std::cos(rotation), s = std::sin(rotation);
	const Box b{pcx * c - pcy * s + W * acx, pcx * s + pcy * c + H * acy, max_x - min_x, max_y - min_y, rotation};
	return shift_square(b, HAND_AGAIN, HAND_SHIFT);
}

// MediaPipe's check that a hand can be followed from where it was: the
// square from its last points (`was`) about where the body's square for
// it (`now`) is - its turn, middle and size - and those points inside
// `now`, a little smaller.
bool still_there(const Box &was, const Box &now, const Point *points)
{
	float turn = (was.angle - now.angle) * 180 / PI_F;
	if (turn > 180)
		turn -= 360;
	if (turn < -180)
		turn += 360;
	if (std::fabs(turn) > TRACK_TURN)
		return false;
	const float c = std::cos(-now.angle), s = std::sin(-now.angle);
	auto along = [&](float x, float y, float &u, float &v) {
		u = x * c - y * s;
		v = x * s + y * c;
	};
	float wu, wv, nu, nv;
	along(was.cx, was.cy, wu, wv);
	along(now.cx, now.cy, nu, nv);
	if (std::fabs(wu - nu) > now.w * TRACK_MOVE || std::fabs(wv - nv) > now.h * TRACK_MOVE)
		return false;
	if (std::fabs(was.w - now.w) > now.w * TRACK_SIZE || std::fabs(was.h - now.h) > now.h * TRACK_SIZE)
		return false;
	const float half_w = now.w * (1 + TRACK_MARGIN) / 2, half_h = now.h * (1 + TRACK_MARGIN) / 2;
	for (int i = 0; i < HAND_POINTS; i++) {
		float u, v;
		along(points[i].x, points[i].y, u, v);
		if (!(nu - half_w < u && u < nu + half_w && nv - half_h < v && v < nv + half_h))
			return false;
	}
	return true;
}

float distance2d(Point a, Point b)
{
	return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
}

// A hand's points' box (picture pixels): left, top, right, bottom.
void bounds_of(const Point *p, int n, float &x0, float &y0, float &x1, float &y1)
{
	x0 = y0 = 1e30f;
	x1 = y1 = -1e30f;
	for (int i = 0; i < n; i++) {
		x0 = std::min(x0, p[i].x);
		y0 = std::min(y0, p[i].y);
		x1 = std::max(x1, p[i].x);
		y1 = std::max(y1, p[i].y);
	}
}

bool fail(std::string *problem, const std::string &why)
{
	if (problem)
		*problem = why;
	return false;
}

} // namespace

struct BodyTracker::Impl {
	nn::Net finder, model, recrop, hand_full, hand_lite;
	bool plain = false;
	bool ready = false, has_lite = false;
	bool hands_on = true, light = false;
	int find_size = 224, model_size = 256, recrop_size = 256, hand_size = 224;
	std::vector<float> anchors;

	int pic_w = 0, pic_h = 0;
	bool following = false;
	Box square;
	Found found;
	Body body;

	// This look's points, before smoothing.
	Point raw[BODY_POINTS], raw_world[BODY_POINTS];
	float raw_seen[BODY_POINTS] = {};
	Point aux[2];

	float amount = 0.5f;
	bool filtering = false;
	double last_seconds = 0;
	OneEuro filters[BODY_POINTS][3], world_filters[BODY_POINTS][3], aux_filters[2][2];

	struct HandState {
		bool have_last = false; // the hand model found it last look (its points)
		Point last[HAND_POINTS];
		bool filtering = false;
		OneEuro filters[HAND_POINTS][3], world_filters[HAND_POINTS][3];
	} hand_state[2];

	bool load(const uint8_t *task, size_t task_size, const uint8_t *lite, size_t lite_size, std::string *problem);
	bool find(const Frame &pic);
	bool points(const Frame &pic);
	void smooth(double seconds);
	void hands(const Frame &pic, double seconds);
	bool look_at_hand(const Frame &pic, int side, HandSeen &hand);
	void smooth_hand(int side, double seconds);
	void lost();
	void lose_hand(int side);
};

bool BodyTracker::Impl::load(const uint8_t *task, size_t task_size, const uint8_t *lite, size_t lite_size,
			     std::string *problem)
{
	ready = false;
	vision::Entry f, m, r, h;
	if (!vision::zip_entry(task, task_size, "pose_detector.tflite", f) ||
	    !vision::zip_entry(task, task_size, "pose_landmarks_detector.tflite", m) ||
	    !vision::zip_entry(task, task_size, "hand_roi_refinement.tflite", r) ||
	    !vision::zip_entry(task, task_size, "hand_landmarks_detector.tflite", h))
		return fail(problem, "the body models file isn't Google's holistic_landmarker.task");
	for (nn::Net *net : {&finder, &model, &recrop, &hand_full, &hand_lite})
		net->set_plain(plain);
	// The body model's outline of you (its 3rd answer) isn't used.
	model.set_wanted({0, 1, 3, 4});
	std::string why;
	if (!finder.load(f.data, f.size, &why))
		return fail(problem, "the body finder model: " + why);
	if (!model.load(m.data, m.size, &why))
		return fail(problem, "the body model: " + why);
	if (!recrop.load(r.data, r.size, &why))
		return fail(problem, "the hand-square model: " + why);
	if (!hand_full.load(h.data, h.size, &why))
		return fail(problem, "the hand model: " + why);
	has_lite = false;
	if (lite && lite_size) {
		if (!hand_lite.load(lite, lite_size, &why))
			return fail(problem, "the light hand model: " + why);
		has_lite = true;
	}
	// The shapes this code expects.
	auto square_in = [](const nn::Net &n, int &size) {
		if (n.inputs() != 1)
			return false;
		const auto &s = n.input_shape(0);
		if (s.size() != 4 || s[0] != 1 || s[1] != s[2] || s[3] != 3 || s[1] < 16)
			return false;
		size = s[1];
		return true;
	};
	auto hand_ok = [&](const nn::Net &n, int &size) {
		return square_in(n, size) && n.outputs() == 4 && elements(n.output_shape(0)) == HAND_POINTS * 3 &&
		       elements(n.output_shape(1)) == 1 && elements(n.output_shape(2)) == 1 &&
		       elements(n.output_shape(3)) == HAND_POINTS * 3;
	};
	int lite_size_px = 0;
	if (!square_in(finder, find_size) || finder.outputs() != 2 || !square_in(model, model_size) ||
	    model.outputs() != 5 || elements(model.output_shape(0)) != 39 * 5 || elements(model.output_shape(1)) != 1 ||
	    elements(model.output_shape(4)) != 39 * 3 || model.output_shape(3).size() != 4 ||
	    model.output_shape(3)[3] != 39 || !square_in(recrop, recrop_size) || recrop.outputs() != 1 ||
	    elements(recrop.output_shape(0)) != 4 || !hand_ok(hand_full, hand_size) ||
	    (has_lite && (!hand_ok(hand_lite, lite_size_px) || lite_size_px != hand_size)))
		return fail(problem, "the body and hand models aren't the ones expected");
	anchors = body_anchors(find_size);
	const size_t boxes = anchors.size() / 2;
	const size_t o0 = elements(finder.output_shape(0)), o1 = elements(finder.output_shape(1));
	if (!((o0 == boxes * 12 && o1 == boxes) || (o1 == boxes * 12 && o0 == boxes)))
		return fail(problem, "the body finder model isn't the one expected");
	ready = true;
	return true;
}

// Step 1: the whole picture, fitted into the finder's square with black
// bars, then its finds: the sure ones, overlapping ones merged (weighted
// by how sure), and the surest one followed: a square round its hips'
// middle, out to past its head, turned upright.
bool BodyTracker::Impl::find(const Frame &pic)
{
	found = Found();
	const float side = (float)std::max(pic.w, pic.h);
	const Box whole{pic.w / 2.0f, pic.h / 2.0f, side, side, 0};
	vision::cut(pic, whole, find_size, find_size, true, 2.0f / 255, -1, finder.input(0));
	finder.run();
	const size_t boxes = anchors.size() / 2;
	const bool first_is_boxes = elements(finder.output_shape(0)) == boxes * 12;
	const float *raw_boxes = finder.output(first_is_boxes ? 0 : 1),
		    *scores = finder.output(first_is_boxes ? 1 : 0);
	struct Find {
		float x0, y0, x1, y1, score;
		float keys[8];
	};
	std::vector<Find> sure;
	const float size = (float)find_size;
	for (size_t i = 0; i < boxes; i++) {
		const float score = sigmoid(std::min(100.0f, std::max(-100.0f, scores[i])));
		if (score < FIND_SCORE)
			continue;
		const float *r = raw_boxes + i * 12;
		const float ax = anchors[i * 2], ay = anchors[i * 2 + 1];
		const float cx = r[0] / size + ax, cy = r[1] / size + ay, w = r[2] / size, h = r[3] / size;
		if (w < 0 || h < 0)
			continue;
		Find b{cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2, score, {}};
		for (int k = 0; k < 4; k++) {
			b.keys[k * 2] = r[4 + k * 2] / size + ax;
			b.keys[k * 2 + 1] = r[4 + k * 2 + 1] / size + ay;
		}
		sure.push_back(b);
	}
	if (sure.empty())
		return false;
	std::stable_sort(sure.begin(), sure.end(), [](const Find &a, const Find &b) { return a.score > b.score; });
	auto overlap = [](const Find &a, const Find &b) {
		const float ix = std::min(a.x1, b.x1) - std::max(a.x0, b.x0),
			    iy = std::min(a.y1, b.y1) - std::max(a.y0, b.y0);
		if (ix <= 0 || iy <= 0)
			return 0.0f;
		const float both = ix * iy, all = (a.x1 - a.x0) * (a.y1 - a.y0) + (b.x1 - b.x0) * (b.y1 - b.y0) - both;
		return all > 0 ? both / all : 0.0f;
	};
	// Only the surest is followed: the finds merged into it.
	const Find top = sure[0];
	Find best = top;
	float total = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0, keys[8] = {};
	for (const Find &b : sure)
		if (overlap(b, top) > SAME_BODY) {
			total += b.score;
			x0 += b.x0 * b.score;
			y0 += b.y0 * b.score;
			x1 += b.x1 * b.score;
			y1 += b.y1 * b.score;
			for (int k = 0; k < 8; k++)
				keys[k] += b.keys[k] * b.score;
		}
	if (total > 0) {
		best.x0 = x0 / total;
		best.y0 = y0 / total;
		best.x1 = x1 / total;
		best.y1 = y1 / total;
		for (int k = 0; k < 8; k++)
			best.keys[k] = keys[k] / total;
	}
	// Back to the picture's pixels.
	const float left = whole.cx - side / 2, top_edge = whole.cy - side / 2;
	found.any = true;
	found.x = left + best.x0 * side;
	found.y = top_edge + best.y0 * side;
	found.w = (best.x1 - best.x0) * side;
	found.h = (best.y1 - best.y0) * side;
	found.score = best.score;
	for (int k = 0; k < 4; k++) {
		found.keys[k * 2] = left + best.keys[k * 2] * side;
		found.keys[k * 2 + 1] = top_edge + best.keys[k * 2 + 1] * side;
	}
	const Point hips{found.keys[0], found.keys[1], 0}, above{found.keys[2], found.keys[3], 0};
	square = shift_square(box_of_two(hips, above, PI_F / 2), BODY_GROW, 0);
	return square.w > 1;
}

// Step 2: the 33 points in the square (and its two extra ones), whether
// a body is there, how likely each point is to be in sight, and the
// points in metres.
bool BodyTracker::Impl::points(const Frame &pic)
{
	vision::cut(pic, square, model_size, model_size, false, 1.0f / 255, 0, model.input(0));
	model.run();
	body.presence = model.output(1)[0];
	if (!(body.presence >= PRESENCE))
		return false;
	const float *out = model.output(0), *heat = model.output(3), *metres = model.output(4);
	const float size = (float)model_size;
	float u[39], v[39], z[39];
	for (int i = 0; i < 39; i++) {
		u[i] = out[i * 5] / size;
		v[i] = out[i * 5 + 1] / size;
		z[i] = out[i * 5 + 2] / size;
	}
	// The sharper guess from the heat map: round each point, the average
	// of where it is weighted by how sure it is there, if it's sure enough.
	const auto &hs = model.output_shape(3);
	const int hh = hs[1], hw = hs[2], hc = hs[3];
	const int reach = (HEAT_KERNEL - 1) / 2;
	for (int i = 0; i < 39; i++) {
		const int col = (int)(u[i] * hw), row = (int)(v[i] * hh);
		if (col < 0 || col >= hw || row < 0 || row >= hh)
			continue;
		float sum = 0, wc = 0, wr = 0, most = 0;
		for (int r = std::max(0, row - reach); r < std::min(hh, row + reach + 1); r++)
			for (int c = std::max(0, col - reach); c < std::min(hw, col + reach + 1); c++) {
				const float sure = sigmoid(heat[((size_t)r * hw + c) * hc + i]);
				sum += sure;
				most = std::max(most, sure);
				wc += c * sure;
				wr += r * sure;
			}
		if (most >= HEAT_SURE && sum > 0) {
			u[i] = wc / hw / sum;
			v[i] = wr / hh / sum;
		}
	}
	const float c = std::cos(square.angle), s = std::sin(square.angle);
	for (int i = 0; i < 39; i++) {
		Point p = in_picture(square, u[i], v[i]);
		p.z = z[i] * square.w;
		if (i < BODY_POINTS) {
			raw[i] = p;
			raw_seen[i] = sigmoid(out[i * 5 + 3]);
			const float mx = metres[i * 3], my = metres[i * 3 + 1];
			raw_world[i] = {c * mx - s * my, s * mx + c * my, metres[i * 3 + 2]};
		} else if (i < BODY_POINTS + 2) {
			aux[i - BODY_POINTS] = p;
		}
	}
	return true;
}

// Step 5 for the body: each point through a One Euro filter (speed in
// body sizes a second), how likely each is in sight eased, the points in
// metres too; and the next look's square from the two extra points,
// smoothed more.
void BodyTracker::Impl::smooth(double seconds)
{
	const Box scale_box = box_of_two(aux[0], aux[1], PI_F / 2);
	const float object = (scale_box.w + scale_box.h) / 2;
	const int64_t ns = (int64_t)std::llround(seconds * 1e9);
	const bool smoothing = amount >= 0 && object >= 1e-6f;
	if (!smoothing || !filtering) {
		for (auto &f : filters)
			for (auto &k : f)
				k = OneEuro();
		for (auto &f : world_filters)
			for (auto &k : f)
				k = OneEuro();
		for (auto &f : aux_filters)
			for (auto &k : f)
				k = OneEuro();
	}
	Point next[2] = {aux[0], aux[1]};
	if (!smoothing) {
		std::copy(std::begin(raw), std::end(raw), body.points);
		std::copy(std::begin(raw_world), std::end(raw_world), body.world);
		std::copy(std::begin(raw_seen), std::end(raw_seen), body.seen);
		filtering = false;
	} else {
		double min_cutoff, beta, aux_min, aux_beta, world_min, world_beta;
		vision::smoothing_of(amount, MIN_CUTOFF, BETA, min_cutoff, beta);
		vision::smoothing_of(amount, AUX_MIN_CUTOFF, AUX_BETA, aux_min, aux_beta);
		vision::smoothing_of(amount, WORLD_MIN_CUTOFF, WORLD_BETA, world_min, world_beta);
		const double scale = 1 / object;
		const float dt = filtering ? (float)std::max(0.0, seconds - last_seconds) : 0;
		const float k = filtering ? 1 - std::exp(-dt / SEEN_TIME) : 1;
		for (int i = 0; i < BODY_POINTS; i++) {
			body.points[i].x = filters[i][0].apply(ns, scale, raw[i].x, min_cutoff, beta);
			body.points[i].y = filters[i][1].apply(ns, scale, raw[i].y, min_cutoff, beta);
			body.points[i].z = filters[i][2].apply(ns, scale, raw[i].z, min_cutoff, beta);
			body.world[i].x = world_filters[i][0].apply(ns, 1, raw_world[i].x, world_min, world_beta);
			body.world[i].y = world_filters[i][1].apply(ns, 1, raw_world[i].y, world_min, world_beta);
			body.world[i].z = world_filters[i][2].apply(ns, 1, raw_world[i].z, world_min, world_beta);
			body.seen[i] += (raw_seen[i] - body.seen[i]) * k;
		}
		for (int i = 0; i < 2; i++) {
			next[i].x = aux_filters[i][0].apply(ns, scale, aux[i].x, aux_min, aux_beta);
			next[i].y = aux_filters[i][1].apply(ns, scale, aux[i].y, aux_min, aux_beta);
		}
		filtering = true;
	}
	last_seconds = seconds;
	// Step 3: the next look's square.
	square = shift_square(box_of_two(next[0], next[1], PI_F / 2), BODY_GROW, 0);
}

void BodyTracker::Impl::lose_hand(int side)
{
	hand_state[side].have_last = false;
	hand_state[side].filtering = false;
}

void BodyTracker::Impl::lost()
{
	following = false;
	filtering = false;
	body.found = false;
	body.presence = 0;
	for (int side = 0; side < 2; side++) {
		lose_hand(side);
		body.hands[side].found = false;
		body.hands[side].gate = HandGate::NotLooked;
		body.hands[side].presence = 0;
	}
}

// Step 4 for one hand: its square, from where it was or from the body,
// and the hand model on it. True if the model is sure it's a hand.
bool BodyTracker::Impl::look_at_hand(const Frame &pic, int side, HandSeen &hand)
{
	HandState &st = hand_state[side];
	const int wrist = side ? BODY_RIGHT_WRIST : BODY_LEFT_WRIST;
	const int little = side ? BODY_RIGHT_PINKY : BODY_LEFT_PINKY;
	const int index = side ? BODY_RIGHT_INDEX : BODY_LEFT_INDEX;
	const Point w = raw[wrist];
	// Only where the wrist is likely in sight, and in the picture (a hand
	// can't be seen where the camera doesn't see).
	if (!(raw_seen[wrist] > WRIST_SEEN) || !(w.x >= 0 && w.x <= pic.w - 1 && w.y >= 0 && w.y <= pic.h - 1))
		return false;
	// The square from the body, made better by the hand-square model.
	const Box from_body = hand_box_from_body(w, raw[little], raw[index]);
	if (!(from_body.w > 1))
		return false;
	vision::cut(pic, from_body, recrop_size, recrop_size, false, 1.0f / 255, 0, recrop.input(0));
	recrop.run();
	const float *rc = recrop.output(0);
	const float rs = (float)recrop_size;
	const Point a = in_picture(from_body, rc[0] / rs, rc[1] / rs), b = in_picture(from_body, rc[2] / rs, rc[3] / rs);
	const Box better = shift_square(box_of_two(a, b, -PI_F / 2), 1, HAND_SHIFT);
	Box use = better;
	hand.tracked = false;
	if (st.have_last) {
		const Box again = hand_box_from_points(st.last, pic.w, pic.h);
		if (still_there(again, better, st.last)) {
			use = again;
			hand.tracked = true;
		}
	}
	if (!(use.w > 1))
		return false;
	hand.box[0] = use.cx;
	hand.box[1] = use.cy;
	hand.box[2] = use.w;
	hand.box[3] = use.h;
	hand.box[4] = use.angle;
	nn::Net &net = light && has_lite ? hand_lite : hand_full;
	vision::cut(pic, use, hand_size, hand_size, false, 1.0f / 255, 0, net.input(0));
	net.run();
	hand.presence = net.output(1)[0];
	hand.handedness = net.output(2)[0];
	if (!(hand.presence >= HAND_PRESENCE)) {
		hand.gate = HandGate::Unsure;
		return false;
	}
	const float *out = net.output(0), *metres = net.output(3);
	const float size = (float)hand_size;
	const float c = std::cos(use.angle), s = std::sin(use.angle);
	const Point body_wrist = raw_world[wrist];
	Point world[HAND_POINTS];
	for (int i = 0; i < HAND_POINTS; i++) {
		Point &p = hand.points[i];
		p = in_picture(use, out[i * 3] / size, out[i * 3 + 1] / size);
		p.z = out[i * 3 + 2] / size / HAND_Z * use.w;
		const float mx = metres[i * 3], my = metres[i * 3 + 1];
		world[i] = {c * mx - s * my, s * mx + c * my, metres[i * 3 + 2]};
	}
	// In metres, its wrist where the body's wrist is.
	for (int i = 0; i < HAND_POINTS; i++)
		hand.world[i] = {world[i].x - world[0].x + body_wrist.x, world[i].y - world[0].y + body_wrist.y,
				 world[i].z - world[0].z + body_wrist.z};
	std::copy(hand.points, hand.points + HAND_POINTS, st.last);
	st.have_last = true;
	return true;
}

void BodyTracker::Impl::smooth_hand(int side, double seconds)
{
	HandState &st = hand_state[side];
	HandSeen &hand = body.hands[side];
	float x0, y0, x1, y1;
	bounds_of(hand.points, HAND_POINTS, x0, y0, x1, y1);
	const float object = ((x1 - x0) + (y1 - y0)) / 2;
	Point lo, hi;
	bounds_of(hand.world, HAND_POINTS, lo.x, lo.y, hi.x, hi.y);
	const float world_object = ((hi.x - lo.x) + (hi.y - lo.y)) / 2;
	if (amount < 0 || !(object >= 1e-6f) || !(world_object >= 1e-6f)) {
		st.filtering = false;
		return;
	}
	if (!st.filtering) {
		for (auto &f : st.filters)
			for (auto &k : f)
				k = OneEuro();
		for (auto &f : st.world_filters)
			for (auto &k : f)
				k = OneEuro();
		st.filtering = true;
	}
	double min_cutoff, beta;
	vision::smoothing_of(amount, MIN_CUTOFF, BETA, min_cutoff, beta);
	const int64_t ns = (int64_t)std::llround(seconds * 1e9);
	for (int i = 0; i < HAND_POINTS; i++) {
		Point &p = hand.points[i], &q = hand.world[i];
		p.x = st.filters[i][0].apply(ns, 1 / object, p.x, min_cutoff, beta);
		p.y = st.filters[i][1].apply(ns, 1 / object, p.y, min_cutoff, beta);
		p.z = st.filters[i][2].apply(ns, 1 / object, p.z, min_cutoff, beta);
		q.x = st.world_filters[i][0].apply(ns, 1 / world_object, q.x, min_cutoff, beta);
		q.y = st.world_filters[i][1].apply(ns, 1 / world_object, q.y, min_cutoff, beta);
		q.z = st.world_filters[i][2].apply(ns, 1 / world_object, q.z, min_cutoff, beta);
	}
}

// Step 4 and our own checks, for both hands.
void BodyTracker::Impl::hands(const Frame &pic, double seconds)
{
	for (int side = 0; side < 2; side++) {
		HandSeen &hand = body.hands[side];
		hand.found = false;
		hand.gate = HandGate::NotLooked;
		hand.presence = 0;
		hand.handedness = 0;
		hand.tracked = false;
		if (!hands_on || !look_at_hand(pic, side, hand)) {
			lose_hand(side);
			continue;
		}
		// Its wrist where the body's is?
		const Point body_wrist = raw[side ? BODY_RIGHT_WRIST : BODY_LEFT_WRIST];
		const float palm = distance2d(hand.points[HAND_WRIST], hand.points[HAND_MIDDLE]);
		if (!(distance2d(hand.points[HAND_WRIST], body_wrist) <= WRIST_FAR * std::max(palm, 1.0f))) {
			hand.gate = HandGate::Elsewhere;
			hand_state[side].filtering = false;
			continue;
		}
		hand.gate = HandGate::Ok;
		hand.found = true;
	}
	// One hand found twice: the one whose wrist is further from its
	// body's wrist (for its palm's size) isn't it.
	HandSeen &l = body.hands[0], &r = body.hands[1];
	if (l.found && r.found) {
		float a0, b0, a1, b1, c0, d0, c1, d1;
		bounds_of(l.points, HAND_POINTS, a0, b0, a1, b1);
		bounds_of(r.points, HAND_POINTS, c0, d0, c1, d1);
		const float ix = std::min(a1, c1) - std::max(a0, c0), iy = std::min(b1, d1) - std::max(b0, d0);
		const float both = ix > 0 && iy > 0 ? ix * iy : 0;
		const float all = (a1 - a0) * (b1 - b0) + (c1 - c0) * (d1 - d0) - both;
		if (all > 0 && both / all > SAME_HAND) {
			auto off = [&](int side) {
				const HandSeen &hd = body.hands[side];
				const float palm = std::max(1.0f, distance2d(hd.points[HAND_WRIST], hd.points[HAND_MIDDLE]));
				return distance2d(hd.points[HAND_WRIST], raw[side ? BODY_RIGHT_WRIST : BODY_LEFT_WRIST]) /
				       palm;
			};
			const int drop = off(0) <= off(1) ? 1 : 0;
			body.hands[drop].found = false;
			body.hands[drop].gate = HandGate::Twice;
			lose_hand(drop);
		}
	}
	for (int side = 0; side < 2; side++)
		if (body.hands[side].found)
			smooth_hand(side, seconds);
}

BodyTracker::BodyTracker() : impl_(std::make_unique<Impl>()) {}
BodyTracker::~BodyTracker() = default;

bool BodyTracker::load(const uint8_t *task, size_t task_size, const uint8_t *lite, size_t lite_size,
		       std::string *problem)
{
	forget();
	return impl_->load(task, task_size, lite, lite_size, problem);
}

bool BodyTracker::load_files(const std::string &task_path, const std::string &lite_path, std::string *problem)
{
	std::ifstream in(utf8_path(task_path), std::ios::binary);
	if (!in)
		return fail(problem, "the body models file is missing (" + task_path + ")");
	std::vector<uint8_t> task((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	std::vector<uint8_t> lite;
	if (!lite_path.empty()) {
		std::ifstream l(utf8_path(lite_path), std::ios::binary);
		if (!l)
			return fail(problem, "the light hand model file is missing (" + lite_path + ")");
		lite.assign((std::istreambuf_iterator<char>(l)), std::istreambuf_iterator<char>());
	}
	return load(task.data(), task.size(), lite.empty() ? nullptr : lite.data(), lite.size(), problem);
}

bool BodyTracker::ready() const
{
	return impl_->ready;
}

const Body &BodyTracker::look(const Frame &pic, double seconds)
{
	Impl &m = *impl_;
	const auto start = std::chrono::steady_clock::now();
	Body &body = m.body;
	body.searched = false;
	if (!m.ready || !pic.px || pic.w < 2 || pic.h < 2 || pic.stride < pic.w * 4) {
		m.lost();
		return body;
	}
	if (pic.w != m.pic_w || pic.h != m.pic_h) {
		m.pic_w = pic.w;
		m.pic_h = pic.h;
		m.lost();
	}
	if (!m.following) {
		body.searched = true;
		body.presence = 0;
		m.following = m.find(pic);
	}
	if (m.following && m.points(pic)) {
		body.found = true;
		body.w = pic.w;
		body.h = pic.h;
		m.smooth(seconds);
		m.hands(pic, seconds);
	} else {
		m.lost();
	}
	body.ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
	return body;
}

const Body &BodyTracker::body() const
{
	return impl_->body;
}

void BodyTracker::forget()
{
	impl_->lost();
}

void BodyTracker::set_hands(bool on)
{
	impl_->hands_on = on;
}

void BodyTracker::set_light(bool on)
{
	impl_->light = on;
}

bool BodyTracker::light() const
{
	return impl_->light && impl_->has_lite;
}

void BodyTracker::set_smoothing(float amount)
{
	impl_->amount = amount;
}

void BodyTracker::set_plain(bool plain)
{
	impl_->plain = plain;
}

const BodyTracker::Found &BodyTracker::found() const
{
	return impl_->found;
}

} // namespace direbot::face
