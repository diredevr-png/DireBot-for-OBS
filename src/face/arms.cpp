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

#include "arms.hpp"

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

namespace direbot::face {

using json = nlohmann::json;

namespace {

constexpr float PI_F = 3.14159265358979f;
constexpr float DEG = PI_F / 180;
// A shoulder, elbow or wrist the body model thinks is at least this likely
// in sight is followed.
constexpr float SEEN = 0.5f;
// An elbow bent less than this is straight (its hinge unknown); more than
// this, its hinge is sure (radians).
constexpr float STRAIGHT = 0.15f, BENT = 0.45f;
// By your head: within NEAR head widths of its middle it's all by its
// head, past FAR not at all.
constexpr float NEAR = 1.5f, FAR = 2.5f;
// The narrowest a head can be (metres, ear to ear), for a bad guess.
constexpr float HEAD_LEAST = 0.08f;

// Each joint's limits (degrees from straight; + towards the palm): the
// thumb's root, middle and tip, then a finger's knuckle, middle and tip.
constexpr float THUMB_LIMITS[3][2] = {{-20, 45}, {-15, 60}, {-15, 80}};
constexpr float FINGER_LIMITS[3][2] = {{-15, 90}, {0, 110}, {0, 80}};
// A hand whose fingers bend back this far all told (degrees, each joint of
// the four fingers) is turned round (finger_bends).
constexpr float BENT_BACK = 40;
// A calibration's open hand bends no more than this (degrees), and moves
// a hand by its head no more than this (head widths).
constexpr float OPEN_MOST = 30, OFFSET_MOST = 0.6f;

float clamp01(float v)
{
	return v < 0 ? 0 : v > 1 ? 1 : v;
}

// 0 at `from`, 1 at `to` (either way round), straight between.
float ramp(float v, float from, float to)
{
	return clamp01((v - from) / (to - from));
}

// The camera's way (x right, y down, z away) -> as you face it (x right,
// y up, z towards the camera).
V3 upright(const Point &p)
{
	return {p.x, -p.y, -p.z};
}

// The bend from `a` to `b` towards `palm` (radians): only the part of the
// turn that's towards the palm (a sideways spread isn't a bend).
float flex(V3 a, V3 b, V3 palm)
{
	const V3 ah = normalized(a, {0, 0, 0});
	const V3 side = palm - ah * dot(palm, ah);
	if (length(ah) < 0.5f || length(side) < 1e-9f || length(b) < 1e-9f)
		return 0;
	const V3 n = normalized(side);
	return std::atan2(dot(b, n), dot(b, ah));
}

// The bend from `a` to `b` about `axis` (unit; radians, + turning by the
// right hand's rule): both as seen down the axis, so a turn about any other
// axis (a finger spread sideways) isn't a bend.
float bend_about(V3 a, V3 b, V3 axis)
{
	const V3 f = a - axis * dot(a, axis), t = b - axis * dot(b, axis);
	if (length(axis) < 0.5f || length(f) < 1e-9f || length(t) < 1e-9f)
		return 0;
	return std::atan2(dot(cross(f, t), axis), dot(f, t));
}

// A point in the picture (pixels).
bool in_picture(const Body &body, int i)
{
	const Point &p = body.points[i];
	return p.x >= 0 && p.x <= body.w - 1 && p.y >= 0 && p.y <= body.h - 1;
}

// A bend `v` with a calibration's open hand bent `open` there: open is
// straight, and the most a joint bends (`most`) stays the most - so a
// calibration opens your hand all the way and still closes your fist.
// (Less than open: back from straight, as much.)
float opened(float v, float open, float most)
{
	if (v <= open || !(most - open > 1e-3f))
		return v - open;
	return (v - open) * most / (most - open);
}

float limit(float v, const float lim[2])
{
	return std::min(lim[1] * DEG, std::max(lim[0] * DEG, v));
}

// `v` turned towards `to` a share `k` of the way (unit vectors).
V3 ease_dir(V3 v, V3 to, float k)
{
	if (k >= 1)
		return to;
	return normalized(rotate(slerp(Quat{}, turn_between(v, to), k), v), to);
}

bool finite(V3 v)
{
	return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

} // namespace

bool finger_bends(const Point *hand, bool left, float bend[5][3])
{
	V3 p[HAND_POINTS];
	for (int i = 0; i < HAND_POINTS; i++)
		p[i] = {hand[i].x, hand[i].y, hand[i].z};
	// The way the palm faces (the side fingers curl to): first knuckle and
	// little knuckle about the wrist, which way round for each hand.
	V3 palm = normalized(cross(p[5] - p[0], p[17] - p[0]), {0, 0, 0});
	if (left)
		palm = -palm;
	// Each finger bends about one axis, across it (square to its palm and
	// the way from the wrist to its knuckle), the same for each joint
	// however far it's curled: in a fist the middle joint and tip bend on
	// round, back under the knuckle.
	float back = 0;
	for (int f = 1; f < 5; f++) {
		const int root = 1 + f * 4; // 5, 9, 13, 17
		const V3 bone[4] = {p[root] - p[0], p[root + 1] - p[root], p[root + 2] - p[root + 1],
				    p[root + 3] - p[root + 2]};
		const V3 axis = normalized(cross(bone[0], palm), {0, 0, 0});
		for (int j = 0; j < 3; j++) {
			bend[f][j] = bend_about(bone[j], bone[j + 1], axis);
			back += bend[f][j];
		}
	}
	// Fingers bend towards the palm, hardly ever back. Bent back on the
	// whole, the hand is the other way round from what it should be: the
	// hand model's guess of its depth is a mirror of the hand it sees (a
	// fist from behind, a small hand far off), or it's the other hand.
	const bool round = back < -BENT_BACK * DEG;
	if (round) {
		palm = -palm;
		for (int f = 1; f < 5; f++)
			for (int j = 0; j < 3; j++)
				bend[f][j] = -bend[f][j];
	}
	// The thumb: its root bends towards the palm, past it towards the
	// first finger's knuckle.
	bend[0][0] = flex(p[1] - p[0], p[2] - p[1], palm);
	bend[0][1] = flex(p[2] - p[1], p[3] - p[2], p[5] - p[2]);
	bend[0][2] = flex(p[3] - p[2], p[4] - p[3], p[5] - p[3]);
	return round;
}

void ArmSolver::look(const Body &body)
{
	auto lose = [](Side &sd) {
		sd.arm_ok = sd.hand_ok = false;
		sd.arm_streak = sd.hand_streak = 0;
	};
	if (!body.found || !(body.seen[BODY_LEFT_SHOULDER] >= SEEN) || !(body.seen[BODY_RIGHT_SHOULDER] >= SEEN)) {
		lose(side_[0]);
		lose(side_[1]);
		return;
	}
	const Point *w = body.world;
	// A point that isn't a number (a broken look) is never followed.
	auto ok = [w](int i) { return finite(upright(w[i])); };
	// Your chest: across your shoulders (your left), up (from your hips
	// when the camera sees them, else the camera's up) and forward.
	const V3 sl = upright(w[BODY_LEFT_SHOULDER]), sr = upright(w[BODY_RIGHT_SHOULDER]);
	if (!ok(BODY_LEFT_SHOULDER) || !ok(BODY_RIGHT_SHOULDER) || !(length(sl - sr) >= 0.05f)) {
		lose(side_[0]);
		lose(side_[1]);
		return;
	}
	const V3 across = normalized(sl - sr);
	const V3 mid = (sl + sr) * 0.5f;
	const V3 hips = (upright(w[BODY_LEFT_HIP]) + upright(w[BODY_RIGHT_HIP])) * 0.5f;
	const float hips_seen = ok(BODY_LEFT_HIP) && ok(BODY_RIGHT_HIP)
					? std::min(body.seen[BODY_LEFT_HIP], body.seen[BODY_RIGHT_HIP])
					: 0;
	const float hk = ramp(hips_seen, 0.5f, 0.8f);
	const V3 up_ref = hk > 0 ? normalized(V3{0, 1, 0} * (1 - hk) + normalized(mid - hips) * hk) : V3{0, 1, 0};
	V3 up = up_ref - across * dot(up_ref, across);
	if (length(up) < 0.2f)
		up = V3{0, 1, 0} - across * across.y;
	up = normalized(up);
	const V3 fwd = cross(across, up);
	auto local = [&](V3 v) { return V3{dot(v, across), dot(v, up), dot(v, fwd)}; };
	const float reach = std::min(3.0f, std::max(0.25f, options_.reach));
	// Your head: the middle of your ears, and ear to ear.
	// (Ears not known: a head the usual size above your shoulders.)
	const bool ears = ok(BODY_LEFT_EAR) && ok(BODY_RIGHT_EAR);
	const V3 el = upright(w[BODY_LEFT_EAR]), er = upright(w[BODY_RIGHT_EAR]);
	const V3 head = ears ? (el + er) * 0.5f : mid + up * (length(sl - sr) * 0.45f);
	const float head_w = std::max(HEAD_LEAST, ears ? length(el - er) : length(sl - sr) * 0.42f);
	const bool mirror = options_.mirror;
	auto flip = [mirror](V3 v) { return mirror ? V3{-v.x, v.y, v.z} : v; };

	bool calibration_seen = false;
	for (int you = 0; you < 2; you++) {
		Side &sd = side_[mirror ? 1 - you : you];
		const int its = mirror ? 1 - you : you;
		const int S = you ? BODY_RIGHT_SHOULDER : BODY_LEFT_SHOULDER;
		const int E = you ? BODY_RIGHT_ELBOW : BODY_LEFT_ELBOW;
		const int W = you ? BODY_RIGHT_WRIST : BODY_LEFT_WRIST;
		// Only an arm you can see. The body model guesses where an arm it
		// can't see is - down by your desk, in your lap, even a wrist in
		// front of your chest - and is often sure of it:
		//   - a hand the hand model finds at its wrist is there;
		//   - else a wrist below the picture is an arm hanging down out of
		//     it (one raised out of the top or the sides is followed);
		//   - else its elbow and wrist must be in sight, and - where the
		//     hand model looked at the wrist and found no hand of its own -
		//     its elbow in the picture too (sitting close, your elbows are
		//     below the picture with your hand by your face: then only
		//     your hand found shows the arm is there).
		const HandSeen &h = body.hands[you];
		bool there;
		if (h.found)
			there = true;
		else if (!(body.points[W].y <= body.h - 1) || !std::isfinite(body.points[W].x))
			there = false;
		else
			there = body.seen[E] >= SEEN && body.seen[W] >= SEEN &&
				(h.gate == HandGate::NotLooked || in_picture(body, E));
		if (!there || !ok(E) || !ok(W)) {
			lose(sd);
			continue;
		}
		V3 e = local(upright(w[E]) - upright(w[S])), wr = local(upright(w[W]) - upright(w[S]));
		e.z *= reach;
		wr.z *= reach;
		if (length(e) < 1e-4f || length(wr - e) < 1e-4f) {
			lose(sd);
			continue;
		}
		ArmTarget t;
		t.upper = flip(normalized(e));
		t.fore = flip(normalized(wr - e));
		// The elbow's hinge: from the bend, or - an arm nearly straight -
		// as it was, a little less sure each look.
		const float bent = std::acos(std::max(-1.0f, std::min(1.0f, dot(t.upper, t.fore))));
		const float sure = ramp(bent, STRAIGHT, BENT);
		const V3 now = normalized(cross(t.upper, t.fore), {0, 0, 0});
		V3 was = sd.target.hinge - t.upper * dot(sd.target.hinge, t.upper);
		const float was_sure = sd.arm_ok && length(was) > 0.3f ? sd.target.hinge_sure * 0.9f : 0;
		was = normalized(was, {0, 0, 0});
		t.hinge = normalized(now * sure + was * (1 - sure), now);
		t.hinge_sure = std::max(sure, was_sure);
		if (length(t.hinge) < 0.5f)
			t.hinge_sure = 0;
		// By your head.
		V3 rel = local(upright(w[W]) - head) * (1 / head_w);
		rel.z *= reach;
		t.near = ramp(length(rel), FAR, NEAR);
		// Your hand, out from your head (+x out, whichever side).
		const V3 out_rel{you ? -rel.x : rel.x, rel.y, rel.z};
		if (calibrated_)
			rel = rel + V3{you ? -ear_offset_.x : ear_offset_.x, ear_offset_.y, ear_offset_.z};
		t.by_head = flip(rel);
		// Its hand.
		bool hand = false;
		if (h.found && options_.fingers) {
			float b[5][3];
			// The hand turned round (its depth a mirror of yours): its
			// depth the other way.
			const bool round = finger_bends(h.world, you == 0, b);
			auto hand_way = [&](int from, int to) {
				V3 v = upright(h.world[to]) - upright(h.world[from]);
				if (round)
					v.z = -v.z;
				return local(v);
			};
			const V3 along = hand_way(HAND_WRIST, HAND_MIDDLE);
			const V3 side = hand_way(HAND_LITTLE, HAND_INDEX);
			if (length(along) > 1e-4f && length(side) > 1e-4f) {
				t.along = flip(normalized(along));
				t.across = flip(normalized(side));
				if (calibrating_ && t.near > 0.3f) {
					for (int f = 0; f < 5; f++)
						for (int j = 0; j < 3; j++)
							sum_bend_[f][j] += b[f][j];
					bend_count_++;
					sum_place_[0] += out_rel.x;
					sum_place_[1] += out_rel.y;
					sum_place_[2] += out_rel.z;
					place_count_++;
					calibration_seen = true;
				}
				for (int f = 0; f < 5; f++)
					for (int j = 0; j < 3; j++) {
						const float *lim = f ? FINGER_LIMITS[j] : THUMB_LIMITS[j];
						t.bend[f][j] = limit(calibrated_ ? opened(b[f][j], open_[f][j], lim[1] * DEG)
										 : b[f][j],
								     lim);
					}
				hand = true;
			}
		}
		// Arms out from its body: turned out about its front, as much as
		// the arm hangs down.
		if (options_.spread > 0) {
			const float a = std::min(45.0f, options_.spread) * DEG * clamp01(-t.upper.y) * (its ? -1.0f : 1.0f);
			const Quat q = axis_angle({0, 0, 1}, a);
			t.upper = rotate(q, t.upper);
			t.fore = rotate(q, t.fore);
			t.hinge = rotate(q, t.hinge);
			t.along = rotate(q, t.along);
			t.across = rotate(q, t.across);
		}
		if (!finite(t.upper) || !finite(t.fore) || !finite(t.hinge) || !finite(t.by_head) || !finite(t.along) ||
		    !finite(t.across)) {
			lose(sd);
			continue;
		}
		sd.target = t;
		sd.arm_ok = true;
		sd.arm_streak++;
		sd.hand_ok = hand;
		sd.hand_streak = hand ? sd.hand_streak + 1 : 0;
	}
	if (calibration_seen && ++kept_ >= CALIBRATE_LOOKS)
		finish_calibration();
}

void ArmSolver::step(float dt)
{
	if (!(dt >= 0) || !std::isfinite(dt))
		dt = 0;
	const float k = 1 - std::exp(-dt / EASE);
	const float hold = std::max(0.0f, options_.hold);
	for (int s = 0; s < 2; s++) {
		Side &sd = side_[s];
		ArmTarget &cur = now_.side[s];
		const ArmTarget &t = sd.target;
		const bool arm_on = sd.arm_ok && sd.arm_streak >= STREAK;
		const bool hand_on = arm_on && sd.hand_ok && sd.hand_streak >= STREAK;
		if (arm_on) {
			sd.arm_lost = 0;
			const float e = cur.weight <= 0 ? 1 : k;
			cur.upper = ease_dir(cur.upper, t.upper, e);
			cur.fore = ease_dir(cur.fore, t.fore, e);
			cur.hinge = ease_dir(cur.hinge, t.hinge, e);
			cur.hinge_sure += (t.hinge_sure - cur.hinge_sure) * e;
			cur.by_head = cur.by_head + (t.by_head - cur.by_head) * e;
			cur.near += (t.near - cur.near) * e;
			cur.weight = std::min(1.0f, cur.weight + (BACK > 0 ? dt / BACK : 1));
		} else {
			sd.arm_lost += dt;
			if (sd.arm_lost > hold)
				cur.weight = std::max(0.0f, cur.weight - dt / FADE);
		}
		if (hand_on) {
			sd.hand_lost = 0;
			const float e = cur.hand <= 0 ? 1 : k;
			cur.along = ease_dir(cur.along, t.along, e);
			cur.across = ease_dir(cur.across, t.across, e);
			for (int f = 0; f < 5; f++)
				for (int j = 0; j < 3; j++)
					cur.bend[f][j] += (t.bend[f][j] - cur.bend[f][j]) * e;
			cur.hand = std::min(1.0f, cur.hand + dt / BACK);
		} else {
			sd.hand_lost += dt;
			if (sd.hand_lost > hold)
				cur.hand = std::max(0.0f, cur.hand - dt / FADE);
		}
	}
	now_.fingers = options_.fingers;
	if (calibrating_) {
		calibrate_time_ += dt;
		if (calibrate_time_ > CALIBRATE_TIME)
			finish_calibration();
	}
}

ArmsSeen ArmSolver::seen() const
{
	ArmsSeen out;
	for (int s = 0; s < 2; s++) {
		out.arm[s] = side_[s].arm_ok && side_[s].arm_streak >= STREAK;
		out.hand[s] = out.arm[s] && side_[s].hand_ok && side_[s].hand_streak >= STREAK;
	}
	return out;
}

void ArmSolver::reset()
{
	for (Side &sd : side_)
		sd = Side();
	now_ = ArmsNow();
	now_.fingers = options_.fingers;
}

void ArmSolver::start_calibration()
{
	calibrating_ = true;
	calibrate_time_ = 0;
	kept_ = 0;
	bend_count_ = place_count_ = 0;
	for (auto &f : sum_bend_)
		for (double &j : f)
			j = 0;
	for (double &p : sum_place_)
		p = 0;
	calibration_result_.clear();
}

void ArmSolver::stop_calibration()
{
	calibrating_ = false;
}

void ArmSolver::finish_calibration()
{
	calibrating_ = false;
	if (!bend_count_ || !place_count_) {
		calibration_result_ = "no hand";
		return;
	}
	for (int f = 0; f < 5; f++)
		for (int j = 0; j < 3; j++)
			open_[f][j] = std::min(OPEN_MOST * DEG,
					       std::max(-OPEN_MOST * DEG, (float)(sum_bend_[f][j] / bend_count_)));
	auto clamp_off = [](float v) { return std::min(OFFSET_MOST, std::max(-OFFSET_MOST, v)); };
	ear_offset_ = {clamp_off(BY_EAR_OUT - (float)(sum_place_[0] / place_count_)),
		       clamp_off(-BY_EAR_DOWN - (float)(sum_place_[1] / place_count_)),
		       clamp_off(0 - (float)(sum_place_[2] / place_count_))};
	calibrated_ = true;
	calibration_result_ = "ok";
}

std::string ArmSolver::save_calibration() const
{
	if (!calibrated_)
		return "";
	auto rounded = [](float v) { return std::round(v * 1e4f) / 1e4f; };
	json open = json::array();
	for (const auto &f : open_)
		for (float j : f)
			open.push_back(rounded(j));
	return json{{"v", 2},
		    {"by_ear", {rounded(ear_offset_.x), rounded(ear_offset_.y), rounded(ear_offset_.z)}},
		    {"open", open}}
		.dump();
}

bool ArmSolver::load_calibration(const std::string &text)
{
	if (text.empty()) {
		clear_calibration();
		return true;
	}
	// v 2: today's. v 1: from a test build of 0.8.0 before its fingers were
	// measured as now - its hand by your ear is kept, its open hand isn't
	// (bends measured another way would keep a fist from closing).
	const json j = json::parse(text, nullptr, false);
	const int version = j.is_object() ? j.value("v", 0) : 0;
	if (version != 1 && version != 2)
		return false;
	const auto by_ear = j.find("by_ear"), open = j.find("open");
	if (by_ear == j.end() || !by_ear->is_array() || by_ear->size() != 3 || open == j.end() || !open->is_array() ||
	    open->size() != 15)
		return false;
	float v[18];
	for (int i = 0; i < 18; i++) {
		const json &x = i < 3 ? (*by_ear)[i] : (*open)[i - 3];
		if (!x.is_number())
			return false;
		v[i] = x.get<float>();
		const float most = i < 3 ? OFFSET_MOST : OPEN_MOST * DEG;
		if (!std::isfinite(v[i]) || std::fabs(v[i]) > most + 1e-3f)
			return false;
	}
	ear_offset_ = {v[0], v[1], v[2]};
	for (int f = 0; f < 5; f++)
		for (int k = 0; k < 3; k++)
			open_[f][k] = version == 2 ? v[3 + f * 3 + k] : 0;
	calibrated_ = true;
	return true;
}

void ArmSolver::clear_calibration()
{
	calibrated_ = false;
	ear_offset_ = V3();
	for (auto &f : open_)
		for (float &j : f)
			j = 0;
}

} // namespace direbot::face
