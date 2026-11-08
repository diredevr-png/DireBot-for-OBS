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
// ...and its Hand Landmarker's (its defaults): the palm finder this sure,
// at least; its finds overlapping this much are one palm; a palm's square
// moved half its size towards the fingers and grown out to the whole
// hand; a palm found over a hand already followed (their squares
// overlapping more than this) is that hand; two hands at most.
constexpr float PALM_SCORE = 0.5f;
constexpr float SAME_PALM = 0.3f;
constexpr float PALM_GROW = 2.6f, PALM_SHIFT = -0.5f;
constexpr float NEW_HAND = 0.5f;
constexpr int MOST_HANDS = 2;
// ...and how it tells one hand found twice: this many of its 21 points
// within this share of its palm's size of the other's points, and their
// boxes overlapping more than this.
constexpr int TWICE_POINTS = 10;
constexpr float TWICE_NEAR = 0.2f, TWICE_OVERLAP = 0.2f;
// The smoothing at 0.5 on the slider (MediaPipe's own for the body: its
// points, the two that place the next look's square, and the points in
// metres).
constexpr double MIN_CUTOFF = 0.05, BETA = 80;
constexpr double AUX_MIN_CUTOFF = 0.01, AUX_BETA = 10;
constexpr double WORLD_MIN_CUTOFF = 0.1, WORLD_BETA = 40;
// How likely each point is to be in sight, smoothed over about this long
// (seconds).
constexpr float SEEN_TIME = 0.15f;
// Our own checks on a hand (a "ghost" hand). A hand is looked for at a
// wrist the body model thinks is at least this likely in sight (MediaPipe
// looks from 0.1, but a hand at a wrist it thinks is hidden - your hands
// down at the desk, out of the picture - is more often a ghost than a
// hand); a hand is at one of the body's wrists within this many of its
// palm's lengths (wrist to middle knuckle) - a hand gripping your other
// arm, its wrist hidden, is a palm and a bit off.
constexpr float WRIST_SEEN = 0.3f;
constexpr float WRIST_FAR = 2.0f;
// The palm finder runs when a hand has just been lost, and this often
// (seconds) while one isn't followed - it costs as much as a hand model
// run, so not on every look as MediaPipe has it.
constexpr double PALM_EVERY = 0.5;
// Left or right: each look's share of a followed hand's (the hand model's
// own, eased over looks), how far the other way it must go before a
// followed hand changes sides (of 1), and how much the body counts when
// it's sure of you - a hand at one of its wrists, and which shoulder a
// hand is nearer.
constexpr float SIDE_EASE = 0.35f, SIDE_STICK = 0.25f;
constexpr float WRIST_SIDE = 0.6f, PLACE_SIDE = 0.3f;
// The body model sure of you: this sure a body is there, and of both your
// shoulders. Then a hand must be within this many of your shoulders'
// widths of a shoulder (your reach), and your left shoulder this far (a
// share of their width) on the picture's left of your right one is your
// back to the camera.
constexpr float BODY_SURE = 0.8f;
constexpr float REACH = 2.5f;
constexpr float TURNED = 0.3f;
// Your arm to your hand: the body model's wrist this many palms (wrist to
// middle knuckle) from the hand found, and its arm starts to be worked
// out from the hand; by the second, all of it. The arm's parts for
// working out its elbow (shares of your shoulders' width, at the least
// and the most), and how far forward or back of your shoulder its wrist
// can be then (metres).
constexpr float ARM_NEAR = 1.0f, ARM_FAR = 2.0f;
constexpr float UPPER_LEAST = 0.7f, UPPER_MOST = 1.1f, FORE_LEAST = 0.6f, FORE_MOST = 1.0f;
constexpr float WRIST_BACK = 0.15f;

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

namespace {

float ramp(float v, float from, float to)
{
	if (!(v > from))
		return 0;
	if (!(v < to))
		return 1;
	return (v - from) / (to - from);
}

// How lit the picture is round a point (0 black to 1 white, its own
// brightness): a few pixels spread over a circle `r` across, or - `ring`
// - on a ring that far out.
float light_at(const Frame &pic, float cx, float cy, float r, bool ring)
{
	const int ri = pic.bgra ? 2 : 0, bi = pic.bgra ? 0 : 2;
	double sum = 0;
	int count = 0;
	constexpr int STEPS = 7;
	for (int j = 0; j < STEPS; j++)
		for (int i = 0; i < STEPS; i++) {
			float dx, dy;
			if (ring) {
				const float a = (j * STEPS + i) * (2 * PI_F / (STEPS * STEPS));
				dx = std::cos(a) * r;
				dy = std::sin(a) * r;
			} else {
				dx = ((i + 0.5f) / STEPS * 2 - 1) * r;
				dy = ((j + 0.5f) / STEPS * 2 - 1) * r;
				if (dx * dx + dy * dy > r * r)
					continue;
			}
			const int x = (int)std::lround(cx + dx), y = (int)std::lround(cy + dy);
			if (x < 0 || y < 0 || x >= pic.w || y >= pic.h)
				continue;
			const uint8_t *p = pic.px + (size_t)y * pic.stride + (size_t)x * 4;
			sum += (0.2126f * p[ri] + 0.7152f * p[1] + 0.0722f * p[bi]) / 255.0f;
			count++;
		}
	return count ? (float)(sum / count) : 0;
}

// Two boxes' overlap as MediaPipe has it: shared area over both, square
// to the picture (a turned square's turn left out).
float overlap(float ax0, float ay0, float ax1, float ay1, float bx0, float by0, float bx1, float by1)
{
	const float ix = std::min(ax1, bx1) - std::max(ax0, bx0), iy = std::min(ay1, by1) - std::max(ay0, by0);
	if (!(ix > 0 && iy > 0))
		return 0;
	const float both = ix * iy, all = (ax1 - ax0) * (ay1 - ay0) + (bx1 - bx0) * (by1 - by0) - both;
	return all > 0 ? both / all : 0;
}

float square_overlap(const Box &a, const Box &b)
{
	return overlap(a.cx - a.w / 2, a.cy - a.h / 2, a.cx + a.w / 2, a.cy + a.h / 2, b.cx - b.w / 2, b.cy - b.h / 2,
		       b.cx + b.w / 2, b.cy + b.h / 2);
}

// MediaPipe's: one hand found twice - most of their points together, and
// their boxes overlapping.
bool found_twice(const Point *a, const Point *b)
{
	auto base = [](const Point *p) {
		return std::max({distance2d(p[HAND_WRIST], p[HAND_INDEX]), distance2d(p[HAND_INDEX], p[HAND_LITTLE]),
				 distance2d(p[HAND_LITTLE], p[HAND_WRIST])});
	};
	const float near = std::max(base(a), base(b)) * TWICE_NEAR;
	int together = 0;
	for (int i = 0; i < HAND_POINTS; i++)
		together += distance2d(a[i], b[i]) < near;
	float a0, a1, a2, a3, b0, b1, b2, b3;
	bounds_of(a, HAND_POINTS, a0, a1, a2, a3);
	bounds_of(b, HAND_POINTS, b0, b1, b2, b3);
	return together >= TWICE_POINTS && overlap(a0, a1, a2, a3, b0, b1, b2, b3) > TWICE_OVERLAP;
}

// The body's left and right points by number (MediaPipe's).
const int PAIRS[16][2] = {{1, 4},   {2, 5},   {3, 6},   {7, 8},   {9, 10},  {11, 12}, {13, 14}, {15, 16},
			  {17, 18}, {19, 20}, {21, 22}, {23, 24}, {25, 26}, {27, 28}, {29, 30}, {31, 32}};

// Your shoulders' width in the picture (pixels), and - turned side-on,
// when they're nearly one behind the other - the size your head and
// shoulders say they'd be.
float shoulders_px(const Point *p)
{
	const Point mid{(p[BODY_LEFT_SHOULDER].x + p[BODY_RIGHT_SHOULDER].x) / 2,
			(p[BODY_LEFT_SHOULDER].y + p[BODY_RIGHT_SHOULDER].y) / 2, 0};
	return std::max(distance2d(p[BODY_LEFT_SHOULDER], p[BODY_RIGHT_SHOULDER]),
			1.2f * distance2d(mid, p[BODY_NOSE]));
}

} // namespace

std::vector<float> ssd_anchors(int size, const std::vector<int> &strides)
{
	std::vector<float> anchors;
	const size_t layers = strides.size();
	size_t layer = 0;
	while (layer < layers) {
		size_t same = layer;
		int per_spot = 0;
		while (same < layers && strides[same] == strides[layer]) {
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

std::vector<BodyTracker::Palm> read_palms(const float *boxes, const float *scores, const std::vector<float> &anchors,
					  int size, int w, int h)
{
	struct Find {
		float x0, y0, x1, y1, score;
		float keys[14];
	};
	std::vector<Find> sure;
	const float s = (float)size;
	const size_t count = anchors.size() / 2;
	for (size_t i = 0; i < count; i++) {
		const float score = vision::sigmoid(std::min(100.0f, std::max(-100.0f, scores[i])));
		if (score < PALM_SCORE)
			continue;
		const float *r = boxes + i * 18;
		const float ax = anchors[i * 2], ay = anchors[i * 2 + 1];
		const float cx = r[0] / s + ax, cy = r[1] / s + ay, bw = r[2] / s, bh = r[3] / s;
		if (!(bw >= 0 && bh >= 0))
			continue;
		Find f{cx - bw / 2, cy - bh / 2, cx + bw / 2, cy + bh / 2, score, {}};
		for (int k = 0; k < 7; k++) {
			f.keys[k * 2] = r[4 + k * 2] / s + ax;
			f.keys[k * 2 + 1] = r[4 + k * 2 + 1] / s + ay;
		}
		sure.push_back(f);
	}
	std::stable_sort(sure.begin(), sure.end(), [](const Find &a, const Find &b) { return a.score > b.score; });
	// Weighted: each surest find with all those overlapping it merged in,
	// by how sure each is (it keeps its own score).
	std::vector<Find> merged;
	while (!sure.empty()) {
		const Find top = sure[0];
		std::vector<Find> rest;
		Find m = top;
		float total = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0, keys[14] = {};
		for (const Find &f : sure) {
			if (overlap(f.x0, f.y0, f.x1, f.y1, top.x0, top.y0, top.x1, top.y1) > SAME_PALM) {
				total += f.score;
				x0 += f.x0 * f.score;
				y0 += f.y0 * f.score;
				x1 += f.x1 * f.score;
				y1 += f.y1 * f.score;
				for (int k = 0; k < 14; k++)
					keys[k] += f.keys[k] * f.score;
			} else {
				rest.push_back(f);
			}
		}
		if (total > 0) {
			m.x0 = x0 / total;
			m.y0 = y0 / total;
			m.x1 = x1 / total;
			m.y1 = y1 / total;
			for (int k = 0; k < 14; k++)
				m.keys[k] = keys[k] / total;
		}
		merged.push_back(m);
		if (rest.size() == sure.size())
			break;
		sure.swap(rest);
	}
	// Back to the picture's pixels (the square fitted round it, middles
	// together).
	const float side = (float)std::max(w, h);
	const float left = w / 2.0f - side / 2, top = h / 2.0f - side / 2;
	std::vector<BodyTracker::Palm> out;
	for (const Find &f : merged) {
		BodyTracker::Palm p;
		p.x = left + f.x0 * side;
		p.y = top + f.y0 * side;
		p.w = (f.x1 - f.x0) * side;
		p.h = (f.y1 - f.y0) * side;
		p.score = f.score;
		for (int k = 0; k < 7; k++) {
			p.keys[k * 2] = left + f.keys[k * 2] * side;
			p.keys[k * 2 + 1] = top + f.keys[k * 2 + 1] * side;
		}
		out.push_back(p);
	}
	return out;
}

void palm_square(const BodyTracker::Palm &p, float out[5])
{
	// Turned by its wrist (point 0) to its middle knuckle (point 2).
	const float angle = wrap_angle(PI_F / 2 - std::atan2(-(p.keys[5] - p.keys[1]), p.keys[4] - p.keys[0]));
	const Box b = shift_square({p.x + p.w / 2, p.y + p.h / 2, p.w, p.h, angle}, PALM_GROW, PALM_SHIFT);
	out[0] = b.cx;
	out[1] = b.cy;
	out[2] = b.w;
	out[3] = b.h;
	out[4] = b.angle;
}

bool turn_back_round(Point points[BODY_POINTS], Point world[BODY_POINTS], float seen[BODY_POINTS])
{
	// (Against your shoulders' width as your head says it'd be facing the
	// camera: side-on, they're nearly one behind the other.)
	const Point &l = points[BODY_LEFT_SHOULDER], &r = points[BODY_RIGHT_SHOULDER];
	const float width = shoulders_px(points);
	if (!(width > 1) || !(seen[BODY_LEFT_SHOULDER] >= 0.5f) || !(seen[BODY_RIGHT_SHOULDER] >= 0.5f) ||
	    !(l.x - r.x < -TURNED * width))
		return false;
	for (const auto &pair : PAIRS) {
		std::swap(points[pair[0]], points[pair[1]]);
		std::swap(world[pair[0]], world[pair[1]]);
		std::swap(seen[pair[0]], seen[pair[1]]);
	}
	return true;
}

void arm_to_hand(Body &b, int side)
{
	const HandSeen &hand = b.hands[side];
	if (!b.found || !hand.found)
		return;
	const int S = side ? BODY_RIGHT_SHOULDER : BODY_LEFT_SHOULDER;
	const int E = side ? BODY_RIGHT_ELBOW : BODY_LEFT_ELBOW;
	const int W = side ? BODY_RIGHT_WRIST : BODY_LEFT_WRIST;
	const int L = BODY_LEFT_SHOULDER, R = BODY_RIGHT_SHOULDER;
	// Metres a pixel, about where you are: your shoulders' width in metres
	// (across the picture) over theirs in the picture.
	const float px = distance2d(b.points[L], b.points[R]);
	const float mx = b.world[L].x - b.world[R].x, my = b.world[L].y - b.world[R].y;
	const float metres = std::sqrt(mx * mx + my * my);
	if (!(px >= 4) || !(metres >= 0.05f) || !std::isfinite(metres / px))
		return;
	const float scale = metres / px;
	const Point hw = hand.points[HAND_WRIST];
	const Point was = b.points[W];
	if (!std::isfinite(hw.x) || !std::isfinite(hw.y) || !std::isfinite(was.x) || !std::isfinite(was.y))
		return;
	// A hand out of that arm's reach doesn't move it.
	if (!(distance2d(hw, b.points[S]) <= REACH * shoulders_px(b.points)))
		return;
	const float palm = std::max(1.0f, distance2d(hw, hand.points[HAND_MIDDLE]));
	const float k = ramp(distance2d(hw, was) / palm, ARM_NEAR, ARM_FAR);
	const float dx = hw.x - was.x, dy = hw.y - was.y;
	// The arm's parts as the body model had them (for working out its
	// elbow, below): as long as your arms are for your shoulders.
	auto sub = [](Point a, Point c) {
		return Point{a.x - c.x, a.y - c.y, a.z - c.z};
	};
	auto len = [](Point v) {
		return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
	};
	const float shoulders = len(sub(b.world[L], b.world[R]));
	const float upper =
		std::min(UPPER_MOST * shoulders, std::max(UPPER_LEAST * shoulders, len(sub(b.world[E], b.world[S]))));
	const float fore =
		std::min(FORE_MOST * shoulders, std::max(FORE_LEAST * shoulders, len(sub(b.world[W], b.world[E]))));
	// The wrist (and the body's hand points round it) to the hand.
	const int around[4] = {W, side ? BODY_RIGHT_PINKY : BODY_LEFT_PINKY, side ? BODY_RIGHT_INDEX : BODY_LEFT_INDEX,
			       side ? BODY_RIGHT_THUMB : BODY_LEFT_THUMB};
	for (int i : around) {
		b.points[i].x += dx;
		b.points[i].y += dy;
		b.world[i].x += dx * scale;
		b.world[i].y += dy * scale;
	}
	b.seen[W] = std::max(b.seen[W], hand.presence);
	if (!(k > 0))
		return;
	// The arm worked out from your shoulder: its parts as long as they
	// were, its wrist as far forward as the body model had it (it can't
	// tell from your hand alone), and its elbow out the way the body
	// model's was.
	const Point &s = b.world[S];
	Point &w = b.world[W];
	w.z = s.z + std::min(WRIST_BACK, std::max(-(upper + fore), w.z - s.z));
	const Point to = sub(w, s);
	const float far = len(to);
	if (!(far > 1e-4f) || !std::isfinite(far))
		return;
	const Point dir{to.x / far, to.y / far, to.z / far};
	Point elbow;
	if (far >= upper + fore) {
		// Further than the arm reaches (the body's size a little off): held
		// straight towards it.
		const float along = far * upper / (upper + fore);
		elbow = {s.x + dir.x * along, s.y + dir.y * along, s.z + dir.z * along};
	} else {
		const float d = std::max(far, std::fabs(upper - fore) + 1e-3f);
		const float along = (upper * upper - fore * fore + d * d) / (2 * d);
		const float out = std::sqrt(std::max(0.0f, upper * upper - along * along));
		// Out the way the body model's elbow was; else down.
		Point pole = sub(b.world[E], s);
		float dp = pole.x * dir.x + pole.y * dir.y + pole.z * dir.z;
		pole = {pole.x - dir.x * dp, pole.y - dir.y * dp, pole.z - dir.z * dp};
		if (len(pole) < 1e-3f) {
			dp = dir.y;
			pole = {-dir.x * dp, 1 - dir.y * dp, -dir.z * dp};
		}
		const float pl = len(pole);
		if (pl < 1e-6f)
			pole = {side ? -1.0f : 1.0f, 0, 0};
		else
			pole = {pole.x / pl, pole.y / pl, pole.z / pl};
		elbow = {s.x + dir.x * along + pole.x * out, s.y + dir.y * along + pole.y * out,
			 s.z + dir.z * along + pole.z * out};
	}
	if (!std::isfinite(elbow.x) || !std::isfinite(elbow.y) || !std::isfinite(elbow.z))
		return;
	Point &e = b.world[E];
	const Point ep{b.points[S].x + (elbow.x - s.x) / scale, b.points[S].y + (elbow.y - s.y) / scale, b.points[E].z};
	e = {e.x + (elbow.x - e.x) * k, e.y + (elbow.y - e.y) * k, e.z + (elbow.z - e.z) * k};
	b.points[E].x += (ep.x - b.points[E].x) * k;
	b.points[E].y += (ep.y - b.points[E].y) * k;
}

struct BodyTracker::Impl {
	nn::Net finder, model, recrop, hand_full, hand_lite, palm;
	bool plain = false;
	bool ready = false, has_lite = false, has_palm = false;
	bool hands_on = true, light = false;
	float body_every = 0;
	int find_size = 224, model_size = 256, recrop_size = 256, hand_size = 224, palm_size = 192;
	std::vector<float> anchors, palm_anchors;

	int pic_w = 0, pic_h = 0;
	bool following = false; // a body found: the next body look starts from it
	bool have_body = false; // ...and its points (kept_*) are from the last body look
	double body_seconds = 0, palm_seconds = 0;
	bool body_looked = false, palm_looked = false;
	bool lost_last = false; // a hand followed was dropped last look
	// Why each side's hand wasn't found on the last body look (a hand is
	// only looked for at a wrist then): kept for the looks between.
	HandGate wrist_gate[2] = {HandGate::NotLooked, HandGate::NotLooked};
	float wrist_tried[2] = {0, 0}, wrist_tried_hand[2] = {0, 0};
	Box square;
	Found found;
	std::vector<Palm> palm_finds;
	Body body;

	// The last body look's points, before smoothing...
	Point raw[BODY_POINTS], raw_world[BODY_POINTS];
	float raw_seen[BODY_POINTS] = {};
	Point aux[2];
	// ...and after (a hand found then moves its arm, on each look).
	Point kept_points[BODY_POINTS], kept_world[BODY_POINTS];
	float kept_seen[BODY_POINTS] = {};
	float kept_presence = 0;
	bool kept_turned = false;

	float amount = 0.5f;
	bool filtering = false;
	double last_seconds = 0;
	OneEuro filters[BODY_POINTS][3], world_filters[BODY_POINTS][3], aux_filters[2][2];

	// Up to two hands, each followed from where it was.
	struct Slot {
		bool on = false;         // found last look: followed from its points
		Point last[HAND_POINTS]; // ...as the hand model had them
		int side = 0;            // the person's left (0) or right (1), as shown
		float right = 0;         // -1 a left hand to 1 a right one: the hand model's own, eased
		bool filtering = false;
		OneEuro filters[HAND_POINTS][3], world_filters[HAND_POINTS][3];
	} slots[MOST_HANDS];

	// A hand the hand model is sure of, this look.
	struct Seen {
		int slot = -1; // the slot it's followed in (-1: new)
		HandFrom from = HandFrom::None;
		int wrist = -1; // looked for at this side's wrist
		Box box;
		float presence = 0, handedness = 0;
		Point points[HAND_POINTS], metres[HAND_POINTS];
		bool keep = true;
		int side = -1;
		float right = 0; // its own left or right, eased (-1 to 1)
		float score = 0; // ...with the body's say (above 0: a right hand)
	};

	bool load(const uint8_t *task, size_t task_size, const uint8_t *lite, size_t lite_size,
		  const uint8_t *palm_model, size_t palm_model_size, std::string *problem);
	bool find(const Frame &pic);
	bool points(const Frame &pic);
	void smooth(double seconds);
	void look_at_body(const Frame &pic, double seconds);
	bool look_at_hand(const Frame &pic, const Box &box, Seen &seen);
	void find_palms(const Frame &pic);
	void hands(const Frame &pic, double seconds);
	void sides(std::vector<Seen *> &kept);
	void smooth_hand(Slot &slot, HandSeen &hand, double seconds);
	void lose_body();
	void lose_hands();
	bool body_sure() const;
};

bool BodyTracker::Impl::load(const uint8_t *task, size_t task_size, const uint8_t *lite, size_t lite_size,
			     const uint8_t *palm_model, size_t palm_model_size, std::string *problem)
{
	ready = false;
	vision::Entry f, m, r, h;
	if (!vision::zip_entry(task, task_size, "pose_detector.tflite", f) ||
	    !vision::zip_entry(task, task_size, "pose_landmarks_detector.tflite", m) ||
	    !vision::zip_entry(task, task_size, "hand_roi_refinement.tflite", r) ||
	    !vision::zip_entry(task, task_size, "hand_landmarks_detector.tflite", h))
		return fail(problem, "the body models file isn't Google's holistic_landmarker.task");
	for (nn::Net *net : {&finder, &model, &recrop, &hand_full, &hand_lite, &palm})
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
	has_palm = false;
	if (palm_model && palm_model_size) {
		if (!palm.load(palm_model, palm_model_size, &why))
			return fail(problem, "the palm finder model: " + why);
		has_palm = true;
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
	anchors = ssd_anchors(find_size, {8, 16, 32, 32, 32});
	const size_t boxes = anchors.size() / 2;
	const size_t o0 = elements(finder.output_shape(0)), o1 = elements(finder.output_shape(1));
	if (!((o0 == boxes * 12 && o1 == boxes) || (o1 == boxes * 12 && o0 == boxes)))
		return fail(problem, "the body finder model isn't the one expected");
	if (has_palm) {
		palm_anchors.clear();
		if (square_in(palm, palm_size) && palm.outputs() == 2)
			palm_anchors = ssd_anchors(palm_size, {8, 16, 16, 16});
		const size_t n = palm_anchors.size() / 2;
		const size_t p0 = elements(palm.output_shape(0)), p1 = elements(palm.output_shape(1));
		if (!n || !((p0 == n * 18 && p1 == n) || (p1 == n * 18 && p0 == n)))
			return fail(problem, "the palm finder model isn't the one expected");
	}
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
	auto over = [](const Find &a, const Find &b) {
		return overlap(a.x0, a.y0, a.x1, a.y1, b.x0, b.y0, b.x1, b.y1);
	};
	// Only the surest is followed: the finds merged into it.
	const Find top = sure[0];
	Find best = top;
	float total = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0, keys[8] = {};
	for (const Find &b : sure)
		if (over(b, top) > SAME_BODY) {
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

// Step 3 for the body: each point through a One Euro filter (speed in
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
		std::copy(std::begin(raw), std::end(raw), kept_points);
		std::copy(std::begin(raw_world), std::end(raw_world), kept_world);
		std::copy(std::begin(raw_seen), std::end(raw_seen), kept_seen);
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
			kept_points[i].x = filters[i][0].apply(ns, scale, raw[i].x, min_cutoff, beta);
			kept_points[i].y = filters[i][1].apply(ns, scale, raw[i].y, min_cutoff, beta);
			kept_points[i].z = filters[i][2].apply(ns, scale, raw[i].z, min_cutoff, beta);
			kept_world[i].x = world_filters[i][0].apply(ns, 1, raw_world[i].x, world_min, world_beta);
			kept_world[i].y = world_filters[i][1].apply(ns, 1, raw_world[i].y, world_min, world_beta);
			kept_world[i].z = world_filters[i][2].apply(ns, 1, raw_world[i].z, world_min, world_beta);
			kept_seen[i] += (raw_seen[i] - kept_seen[i]) * k;
		}
		for (int i = 0; i < 2; i++) {
			next[i].x = aux_filters[i][0].apply(ns, scale, aux[i].x, aux_min, aux_beta);
			next[i].y = aux_filters[i][1].apply(ns, scale, aux[i].y, aux_min, aux_beta);
		}
		filtering = true;
	}
	last_seconds = seconds;
	// The next look's square.
	square = shift_square(box_of_two(next[0], next[1], PI_F / 2), BODY_GROW, 0);
}

void BodyTracker::Impl::lose_body()
{
	following = false;
	filtering = false;
	have_body = false;
	kept_presence = 0;
	kept_turned = false;
}

void BodyTracker::Impl::lose_hands()
{
	for (Slot &s : slots) {
		s.on = false;
		s.filtering = false;
	}
	palm_looked = false;
	lost_last = false;
	for (int side = 0; side < 2; side++) {
		wrist_gate[side] = HandGate::NotLooked;
		wrist_tried[side] = wrist_tried_hand[side] = 0;
	}
}

bool BodyTracker::Impl::body_sure() const
{
	return have_body && kept_presence >= BODY_SURE && kept_seen[BODY_LEFT_SHOULDER] >= BODY_SURE &&
	       kept_seen[BODY_RIGHT_SHOULDER] >= BODY_SURE && shoulders_px(kept_points) >= 4;
}

// Steps 1 to 3, when the body model's due.
void BodyTracker::Impl::look_at_body(const Frame &pic, double seconds)
{
	body.fresh = true;
	body_seconds = seconds;
	body_looked = true;
	if (!following) {
		body.searched = true;
		body.presence = 0;
		following = find(pic);
	}
	if (following && points(pic)) {
		// Your back to the camera? You face it to stream: swapped back,
		// before the smoothing (each point's filter follows the same side).
		kept_turned = turn_back_round(raw, raw_world, raw_seen);
		smooth(seconds);
		kept_presence = body.presence;
		have_body = true;
	} else {
		lose_body();
	}
}

// The hand model on a square: true if it's sure a hand is there, with its
// points (picture pixels) and in metres round the hand (the camera's way).
bool BodyTracker::Impl::look_at_hand(const Frame &pic, const Box &use, Seen &seen)
{
	if (!(use.w > 1) || !std::isfinite(use.cx) || !std::isfinite(use.cy) || !std::isfinite(use.angle))
		return false;
	seen.box = use;
	nn::Net &net = light && has_lite ? hand_lite : hand_full;
	vision::cut(pic, use, hand_size, hand_size, false, 1.0f / 255, 0, net.input(0));
	net.run();
	seen.presence = net.output(1)[0];
	seen.handedness = net.output(2)[0];
	if (!(seen.presence >= HAND_PRESENCE))
		return false;
	const float *out = net.output(0), *metres = net.output(3);
	const float size = (float)hand_size;
	const float c = std::cos(use.angle), s = std::sin(use.angle);
	for (int i = 0; i < HAND_POINTS; i++) {
		Point &p = seen.points[i];
		p = in_picture(use, out[i * 3] / size, out[i * 3 + 1] / size);
		p.z = out[i * 3 + 2] / size / HAND_Z * use.w;
		const float mx = metres[i * 3], my = metres[i * 3 + 1];
		seen.metres[i] = {c * mx - s * my, s * mx + c * my, metres[i * 3 + 2]};
	}
	return true;
}

// The palm finder on the whole picture, fitted into its square with black
// bars.
void BodyTracker::Impl::find_palms(const Frame &pic)
{
	palm_finds.clear();
	const float side = (float)std::max(pic.w, pic.h);
	const Box whole{pic.w / 2.0f, pic.h / 2.0f, side, side, 0};
	vision::cut(pic, whole, palm_size, palm_size, true, 1.0f / 255, 0, palm.input(0));
	palm.run();
	const size_t n = palm_anchors.size() / 2;
	const bool first_is_boxes = elements(palm.output_shape(0)) == n * 18;
	palm_finds = read_palms(palm.output(first_is_boxes ? 0 : 1), palm.output(first_is_boxes ? 1 : 0), palm_anchors,
				palm_size, pic.w, pic.h);
}

// Step 6: left or right. Each hand's own (the hand model's, eased over the
// looks it's followed), the body's wrist it's at and the shoulder it's
// nearer when the body model is sure of you; a followed hand keeps its
// side unless that turns well the other way; two hands, one each side.
void BodyTracker::Impl::sides(std::vector<Seen *> &kept)
{
	const bool sure = body_sure();
	const float shoulders = sure ? shoulders_px(kept_points) : 1;
	for (Seen *s : kept) {
		const float own = 2 * s->handedness - 1;
		s->right = s->from == HandFrom::Followed ? slots[s->slot].right * (1 - SIDE_EASE) + own * SIDE_EASE
							 : own;
		float t = s->right;
		if (s->from == HandFrom::Wrist) {
			t += (s->wrist ? 1 : -1) * WRIST_SIDE;
		} else if (sure) {
			const Point &w = s->points[HAND_WRIST];
			const float palm = std::max(1.0f, distance2d(w, s->points[HAND_MIDDLE]));
			bool at[2];
			for (int side = 0; side < 2; side++) {
				const int W = side ? BODY_RIGHT_WRIST : BODY_LEFT_WRIST;
				at[side] = kept_seen[W] > WRIST_SEEN &&
					   distance2d(w, kept_points[W]) <= WRIST_FAR * palm;
			}
			if (at[0] != at[1])
				t += (at[1] ? 1 : -1) * WRIST_SIDE;
			const float nearer = (distance2d(w, kept_points[BODY_LEFT_SHOULDER]) -
					      distance2d(w, kept_points[BODY_RIGHT_SHOULDER])) /
					     shoulders;
			t += PLACE_SIDE * std::max(-1.0f, std::min(1.0f, nearer));
		}
		s->score = t;
		if (s->from == HandFrom::Followed) {
			const int was = slots[s->slot].side;
			s->side = (was ? t > -SIDE_STICK : t > SIDE_STICK) ? 1 : 0;
		} else {
			s->side = t > 0 ? 1 : 0;
		}
	}
	if (kept.size() == 2) {
		// The one more "right" is the right hand; a pair both followed
		// swaps only when that's well past where they were.
		int right = kept[1]->score > kept[0]->score ? 1 : 0;
		if (kept[0]->from == HandFrom::Followed && kept[1]->from == HandFrom::Followed &&
		    slots[kept[0]->slot].side != slots[kept[1]->slot].side) {
			const int was = slots[kept[1]->slot].side ? 1 : 0;
			const float margin = was ? kept[0]->score - kept[1]->score : kept[1]->score - kept[0]->score;
			right = margin > 2 * SIDE_STICK ? 1 - was : was;
		}
		kept[right]->side = 1;
		kept[1 - right]->side = 0;
	}
}

void BodyTracker::Impl::smooth_hand(Slot &st, HandSeen &hand, double seconds)
{
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

// Steps 4 to 7, and our own checks, for both hands.
void BodyTracker::Impl::hands(const Frame &pic, double seconds)
{
	// Hands off: a hand is only looked for at a wrist, when the body model
	// has just looked - between, the hands as they were.
	if (!hands_on && !body.fresh)
		return;
	HandGate gate[2] = {HandGate::NotLooked, HandGate::NotLooked};
	float tried[2] = {0, 0}, tried_hand[2] = {0, 0};
	bool lost = lost_last;
	if (!body.fresh)
		for (int side = 0; side < 2; side++) {
			gate[side] = wrist_gate[side];
			tried[side] = wrist_tried[side];
			tried_hand[side] = wrist_tried_hand[side];
		}
	std::vector<Seen> seen;
	seen.reserve(MOST_HANDS + 2);
	// Step 4: the hands followed from where they were (hands off: none -
	// a hand is only looked for at a wrist, below).
	for (int k = 0; k < MOST_HANDS; k++) {
		Slot &slot = slots[k];
		if (!slot.on)
			continue;
		Seen s;
		s.slot = k;
		s.from = HandFrom::Followed;
		if (hands_on && look_at_hand(pic, hand_box_from_points(slot.last, pic.w, pic.h), s)) {
			seen.push_back(s);
			continue;
		}
		slot.on = false;
		slot.filtering = false;
		lost = true;
		if (hands_on) {
			gate[slot.side] = HandGate::Unsure;
			tried[slot.side] = s.presence;
			tried_hand[slot.side] = s.handedness;
		}
	}
	// Step 5: at your wrist, for a side no hand followed is shown on, when
	// the body model has just looked.
	if (body.fresh && have_body) {
		for (int side = 0; side < 2; side++) {
			bool shown = false;
			for (const Seen &s : seen)
				shown = shown || slots[s.slot].side == side;
			const int W = side ? BODY_RIGHT_WRIST : BODY_LEFT_WRIST;
			const int little = side ? BODY_RIGHT_PINKY : BODY_LEFT_PINKY;
			const int index = side ? BODY_RIGHT_INDEX : BODY_LEFT_INDEX;
			const Point w = raw[W];
			// Only where the wrist is likely in sight, and in the picture (a
			// hand can't be seen where the camera doesn't see).
			if (shown ||
			    !looks_for_hand(hands_on, raw[side ? BODY_RIGHT_ELBOW : BODY_LEFT_ELBOW], pic.w, pic.h) ||
			    !(raw_seen[W] > WRIST_SEEN) ||
			    !(w.x >= 0 && w.x <= pic.w - 1 && w.y >= 0 && w.y <= pic.h - 1))
				continue;
			// The square from the body, made better by the hand-square model.
			const Box from_body = hand_box_from_body(w, raw[little], raw[index]);
			if (!(from_body.w > 1))
				continue;
			vision::cut(pic, from_body, recrop_size, recrop_size, false, 1.0f / 255, 0, recrop.input(0));
			recrop.run();
			const float *rc = recrop.output(0);
			const float rs = (float)recrop_size;
			const Point a = in_picture(from_body, rc[0] / rs, rc[1] / rs),
				    b = in_picture(from_body, rc[2] / rs, rc[3] / rs);
			const Box better = shift_square(box_of_two(a, b, -PI_F / 2), 1, HAND_SHIFT);
			// A hand followed there already (the other side's, by its own
			// left or right) is that hand.
			bool taken = false;
			for (const Seen &s : seen)
				taken = taken || square_overlap(s.box, better) > NEW_HAND;
			if (taken)
				continue;
			Seen s;
			s.from = HandFrom::Wrist;
			s.wrist = side;
			if (look_at_hand(pic, better, s)) {
				seen.push_back(s);
			} else {
				gate[side] = HandGate::Unsure;
				tried[side] = s.presence;
				tried_hand[side] = s.handedness;
			}
		}
	}
	// ...and anywhere, by the palm finder: when a hand's just been lost,
	// and every so often while one isn't followed.
	body.palm = false;
	if (hands_on && has_palm && (int)seen.size() < MOST_HANDS &&
	    (lost || !palm_looked || seconds < palm_seconds || seconds - palm_seconds >= PALM_EVERY * 0.95)) {
		palm_looked = true;
		palm_seconds = seconds;
		body.palm = true;
		find_palms(pic);
		// MediaPipe's: its two surest palms, less any where a hand is
		// already followed.
		for (size_t i = 0; i < palm_finds.size() && i < (size_t)MOST_HANDS; i++) {
			if ((int)seen.size() >= MOST_HANDS)
				break;
			float sq[5];
			palm_square(palm_finds[i], sq);
			const Box box{sq[0], sq[1], sq[2], sq[3], sq[4]};
			bool taken = false;
			for (const Seen &s : seen)
				taken = taken || square_overlap(s.box, box) > NEW_HAND;
			if (taken)
				continue;
			Seen s;
			s.from = HandFrom::Palm;
			if (look_at_hand(pic, box, s))
				seen.push_back(s);
		}
	}
	// One hand found twice: the first kept (followed, then at a wrist,
	// then by the palm finder).
	std::vector<Seen *> kept;
	for (Seen &s : seen) {
		for (Seen *k : kept)
			if (found_twice(s.points, k->points)) {
				s.keep = false;
				if (s.from == HandFrom::Wrist && gate[s.wrist] == HandGate::NotLooked)
					gate[s.wrist] = HandGate::Twice;
				break;
			}
		if (s.keep)
			kept.push_back(&s);
	}
	// A new hand within your reach, when the body model is sure of you (a
	// hand followed was, when it was found: it's yours, whatever the body
	// model has now - in a dim room it's often unsure of you).
	if (body_sure()) {
		const float reach = REACH * shoulders_px(kept_points);
		std::vector<Seen *> near;
		for (Seen *s : kept) {
			const Point &w = s->points[HAND_WRIST];
			const float d = std::min(distance2d(w, kept_points[BODY_LEFT_SHOULDER]),
						 distance2d(w, kept_points[BODY_RIGHT_SHOULDER]));
			if (s->from == HandFrom::Followed || d <= reach) {
				near.push_back(s);
			} else {
				s->keep = false;
				const int side = s->from == HandFrom::Wrist ? s->wrist : s->handedness > 0.5f ? 1 : 0;
				gate[side] = HandGate::Elsewhere;
				tried[side] = s->presence;
				tried_hand[side] = s->handedness;
			}
		}
		kept.swap(near);
	}
	while ((int)kept.size() > MOST_HANDS)
		kept.pop_back();
	// Hands off: each at the wrist it was looked for at; on, step 6.
	if (hands_on) {
		sides(kept);
	} else {
		for (Seen *s : kept)
			s->side = s->wrist;
	}
	// Into slots: a followed hand keeps its own, a new one takes a free
	// one; the slots of hands not kept are free again (a followed hand not
	// kept - found twice - is lost: the palm finder looks next time).
	bool used[MOST_HANDS] = {};
	for (Seen *s : kept)
		if (s->from == HandFrom::Followed)
			used[s->slot] = true;
	lost_last = false;
	for (int k = 0; k < MOST_HANDS; k++)
		if (!used[k]) {
			lost_last = lost_last || slots[k].on;
			slots[k].on = false;
			slots[k].filtering = false;
		}
	for (Seen *s : kept) {
		if (s->slot < 0)
			for (int k = 0; k < MOST_HANDS; k++)
				if (!used[k]) {
					s->slot = k;
					used[k] = true;
					slots[k].filtering = false;
					break;
				}
		Slot &slot = slots[s->slot];
		slot.on = hands_on;
		std::copy(s->points, s->points + HAND_POINTS, slot.last);
		slot.side = s->side;
		slot.right = s->right;
	}
	// What each side shows.
	for (int side = 0; side < 2; side++) {
		HandSeen &hand = body.hands[side];
		Seen *s = nullptr;
		for (Seen *k : kept)
			if (k->side == side)
				s = k;
		if (body.fresh) {
			wrist_gate[side] = s ? HandGate::NotLooked : gate[side];
			wrist_tried[side] = s ? 0 : tried[side];
			wrist_tried_hand[side] = s ? 0 : tried_hand[side];
		}
		hand.found = s != nullptr;
		hand.tracked = s && s->from == HandFrom::Followed;
		hand.from = s ? s->from : HandFrom::None;
		if (!s) {
			hand.gate = gate[side];
			hand.presence = tried[side];
			hand.handedness = tried_hand[side];
			hand.light = hand.around = 0;
			continue;
		}
		hand.gate = HandGate::Ok;
		hand.presence = s->presence;
		hand.handedness = s->handedness;
		std::copy(s->points, s->points + HAND_POINTS, hand.points);
		// In metres round its wrist for now (put at the body's wrist, below).
		for (int i = 0; i < HAND_POINTS; i++)
			hand.world[i] = {s->metres[i].x - s->metres[0].x, s->metres[i].y - s->metres[0].y,
					 s->metres[i].z - s->metres[0].z};
		hand.box[0] = s->box.cx;
		hand.box[1] = s->box.cy;
		hand.box[2] = s->box.w;
		hand.box[3] = s->box.h;
		hand.box[4] = s->box.angle;
		// How lit: its palm (round the middle of the wrist and knuckles),
		// and just round the hand.
		const Point *p = s->points;
		const float cx = (p[0].x + p[5].x + p[9].x + p[13].x + p[17].x) / 5,
			    cy = (p[0].y + p[5].y + p[9].y + p[13].y + p[17].y) / 5;
		const float palm = std::max(2.0f, distance2d(p[HAND_WRIST], p[HAND_MIDDLE]));
		hand.light = light_at(pic, cx, cy, palm * 0.35f, false);
		hand.around = light_at(pic, cx, cy, palm * 1.8f, true);
		smooth_hand(slots[s->slot], hand, seconds);
	}
}

BodyTracker::BodyTracker() : impl_(std::make_unique<Impl>()) {}
BodyTracker::~BodyTracker() = default;

bool looks_for_hand(bool hands_on, const Point &elbow, int w, int h)
{
	return hands_on || !(elbow.x >= 0 && elbow.x <= w - 1 && elbow.y >= 0 && elbow.y <= h - 1);
}

bool BodyTracker::load(const uint8_t *task, size_t task_size, const uint8_t *lite, size_t lite_size,
		       std::string *problem, const uint8_t *palm, size_t palm_size)
{
	forget();
	return impl_->load(task, task_size, lite, lite_size, palm, palm_size, problem);
}

bool BodyTracker::load_files(const std::string &task_path, const std::string &lite_path, std::string *problem,
			     const std::string &palm_path)
{
	auto read = [](const std::string &path, std::vector<uint8_t> &out) {
		std::ifstream in(utf8_path(path), std::ios::binary);
		if (!in)
			return false;
		out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		return true;
	};
	std::vector<uint8_t> task, lite, palm;
	if (!read(task_path, task))
		return fail(problem, "the body models file is missing (" + task_path + ")");
	if (!lite_path.empty() && !read(lite_path, lite))
		return fail(problem, "the light hand model file is missing (" + lite_path + ")");
	if (!palm_path.empty() && !read(palm_path, palm))
		return fail(problem, "the palm finder model file is missing (" + palm_path + ")");
	return load(task.data(), task.size(), lite.empty() ? nullptr : lite.data(), lite.size(), problem,
		    palm.empty() ? nullptr : palm.data(), palm.size());
}

bool BodyTracker::ready() const
{
	return impl_->ready;
}

bool BodyTracker::has_palm() const
{
	return impl_->ready && impl_->has_palm;
}

const Body &BodyTracker::look(const Frame &pic, double seconds)
{
	Impl &m = *impl_;
	const auto start = std::chrono::steady_clock::now();
	Body &body = m.body;
	body.searched = false;
	body.fresh = false;
	body.palm = false;
	if (!m.ready || !pic.px || pic.w < 2 || pic.h < 2 || pic.stride < pic.w * 4) {
		forget();
		return body;
	}
	if (pic.w != m.pic_w || pic.h != m.pic_h) {
		m.pic_w = pic.w;
		m.pic_h = pic.h;
		forget();
	}
	body.w = pic.w;
	body.h = pic.h;
	// The body model when it's due (a little early is fine: the camera's
	// pictures come unevenly).
	if (!m.body_looked || m.body_every <= 0 || seconds < m.body_seconds ||
	    seconds - m.body_seconds >= m.body_every * 0.75)
		m.look_at_body(pic, seconds);
	// The body as its last look had it...
	body.found = m.have_body;
	body.presence = m.have_body ? m.kept_presence : 0;
	body.turned = m.have_body && m.kept_turned;
	if (m.have_body) {
		std::copy(std::begin(m.kept_points), std::end(m.kept_points), body.points);
		std::copy(std::begin(m.kept_world), std::end(m.kept_world), body.world);
		std::copy(std::begin(m.kept_seen), std::end(m.kept_seen), body.seen);
	}
	// ...your hands on every look...
	m.hands(pic, seconds);
	// ...and your arms to your hands, each hand in metres at its wrist.
	for (int side = 0; side < 2; side++) {
		HandSeen &hand = body.hands[side];
		if (!hand.found)
			continue;
		arm_to_hand(body, side);
		const Point at = body.found ? body.world[side ? BODY_RIGHT_WRIST : BODY_LEFT_WRIST] : Point{};
		const Point w0 = hand.world[HAND_WRIST];
		for (int i = 0; i < HAND_POINTS; i++)
			hand.world[i] = {hand.world[i].x - w0.x + at.x, hand.world[i].y - w0.y + at.y,
					 hand.world[i].z - w0.z + at.z};
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
	Impl &m = *impl_;
	m.lose_body();
	m.lose_hands();
	m.body_looked = false;
	m.body.found = false;
	m.body.presence = 0;
	m.body.turned = false;
	for (HandSeen &hand : m.body.hands) {
		hand.found = false;
		hand.gate = HandGate::NotLooked;
		hand.presence = 0;
		hand.from = HandFrom::None;
		hand.tracked = false;
	}
}

void BodyTracker::set_hands(bool on)
{
	impl_->hands_on = on;
}

void BodyTracker::set_body_every(float seconds)
{
	impl_->body_every = std::max(0.0f, seconds);
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

const std::vector<BodyTracker::Palm> &BodyTracker::palms() const
{
	return impl_->palm_finds;
}

} // namespace direbot::face
