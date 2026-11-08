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

#include "signs.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace direbot::face {

const char *const HAND_SIGN_NAMES[HAND_SIGNS] = {"open_palm",  "fist",	      "thumbs_up",     "thumbs_down",
						 "point_up",   "finger_gun",  "peace",	       "middle_finger",
						 "horns",      "ok",	      "wave"};
const char *const FACE_MOVE_NAMES[FACE_MOVES] = {"mouth_open", "brows_up", "smile", "kiss", "wink"};
const char *const SIGN_SIDE_NAMES[SIGN_SIDES] = {"either", "left", "right", "both"};

namespace {

constexpr float PI = 3.14159265358979f;
constexpr size_t MOST_RULES = 50;

float clamp01(float v)
{
	return v < 0 ? 0 : v > 1 ? 1 : v;
}

// 0 at a, 1 at b (either way round), straight between.
float ramp(float v, float a, float b)
{
	if (a == b)
		return v >= b ? 1.0f : 0.0f;
	return clamp01((v - a) / (b - a));
}

float through(float v, const TuneRange &r)
{
	return r.to > r.from + 1e-4f ? clamp01((v - r.from) / (r.to - r.from)) : (v >= r.to ? 1.0f : 0.0f);
}

struct V {
	float x, y, z;
};
V sub(const Point &a, const Point &b)
{
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}
float length(const V &v)
{
	return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}
float dist(const Point &a, const Point &b)
{
	return length(sub(a, b));
}
// Degrees between two directions (0 when either is nothing).
float angle(const V &a, const V &b)
{
	const float na = length(a), nb = length(b);
	if (na < 1e-9f || nb < 1e-9f)
		return 0;
	const float c = (a.x * b.x + a.y * b.y + a.z * b.z) / (na * nb);
	return std::acos(std::max(-1.0f, std::min(1.0f, c))) * 180.0f / PI;
}

// Up and down as the camera has them (y is down).
constexpr V UP = {0, -1, 0}, DOWN = {0, 1, 0};

int move_id(const char *name)
{
	for (int i = 0; i < EXPRESSIONS; i++)
		if (std::strcmp(EXPRESSION_NAMES[i], name) == 0)
			return i;
	return 0;
}

int name_of(const std::string &name, const char *const *names, int count)
{
	for (int i = 0; i < count; i++)
		if (name == names[i])
			return i;
	return -1;
}

// The hand up by your face or higher: its middle above halfway from your
// mouth down to your shoulders (or, without your shoulders in sight, a
// little below your mouth; without your face, the picture's top half).
bool hand_raised(const Body &b, const HandSeen &h)
{
	const float y = h.points[9].y;
	auto sure = [&](int i) {
		return b.found && b.seen[i] > 0.5f;
	};
	if (sure(9) && sure(10)) {
		const float mouth = (b.points[9].y + b.points[10].y) / 2;
		if (sure(11) && sure(12))
			return y < (mouth + (b.points[11].y + b.points[12].y) / 2) / 2;
		if (sure(2) && sure(5)) {
			const float eyes = (b.points[2].y + b.points[5].y) / 2;
			return y < mouth + 1.2f * std::max(0.0f, mouth - eyes);
		}
		return y < mouth;
	}
	return b.h > 0 && y < 0.5f * (float)b.h;
}

} // namespace

HandShape read_hand_shape(const Point p[HAND_POINTS])
{
	HandShape s;
	const Point &wrist = p[0];
	const float palm = dist(p[9], wrist);
	if (!(palm > 1e-5f))
		return s;
	// Each finger: how far its tip is from the wrist against the length
	// along it (wrist, knuckle and its three joints), and how much its
	// two upper joints bend. Straight: it reaches most of its length, or
	// a little less with its joints straight; curled: it reaches little,
	// or a little more bent right round. Between, neither.
	for (int f = 0; f < 4; f++) {
		const int m = 5 + 4 * f;
		const Point *chain[5] = {&wrist, &p[m], &p[m + 1], &p[m + 2], &p[m + 3]};
		float along = 0;
		for (int i = 0; i < 4; i++)
			along += dist(*chain[i + 1], *chain[i]);
		const float reach = along > 1e-6f ? dist(*chain[4], wrist) / along : 0;
		const float bend = angle(sub(*chain[2], *chain[1]), sub(*chain[3], *chain[2])) +
				   angle(sub(*chain[3], *chain[2]), sub(*chain[4], *chain[3]));
		s.out[f] = std::max(ramp(reach, 0.80f, 0.90f), std::min(ramp(reach, 0.74f, 0.84f), ramp(bend, 70, 35)));
		s.curled[f] = std::max(ramp(reach, 0.74f, 0.64f), std::min(ramp(reach, 0.80f, 0.70f), ramp(bend, 110, 140)));
	}
	// The thumb: out from the fingers (its tip away from the first two
	// fingers' middle joints), which way it points, and touching the
	// first finger's tip (OK).
	float near = 1e9f;
	for (int k : {6, 7, 10, 11})
		near = std::min(near, dist(p[4], p[k]));
	const float thumb_out = ramp(near / palm, 0.28f, 0.45f), thumb_in = 1 - thumb_out;
	const float thumb_up = ramp(angle(sub(p[4], p[2]), UP), 65, 40);
	const float thumb_down = ramp(angle(sub(p[4], p[2]), DOWN), 65, 40);
	const float index_up = ramp(angle(sub(p[8], p[5]), UP), 65, 40);
	const float touching = ramp(dist(p[4], p[8]) / palm, 0.40f, 0.25f);
	const float i = s.out[0], m = s.out[1], g = s.out[2], l = s.out[3];
	const float I = s.curled[0], M = s.curled[1], G = s.curled[2], L = s.curled[3];
	// Not held out (looser than curled): for the signs whose fingers held
	// out are the sign (a peace sign's other two fingers are often loose).
	const float di = 1 - i, dm = 1 - m, dg = 1 - g, dl = 1 - l;
	auto all = [](std::initializer_list<float> v) {
		return std::min(v);
	};
	s.score[SIGN_OPEN_PALM] = all({i, m, g, l, thumb_out});
	s.score[SIGN_FIST] = all({I, M, G, L, thumb_in});
	s.score[SIGN_THUMBS_UP] = all({I, M, G, L, thumb_out, thumb_up});
	s.score[SIGN_THUMBS_DOWN] = all({I, M, G, L, thumb_out, thumb_down});
	s.score[SIGN_POINT_UP] = all({i, M, G, L, thumb_in, index_up});
	s.score[SIGN_FINGER_GUN] = all({i, M, G, L, thumb_out});
	s.score[SIGN_PEACE] = all({i, m, dg, dl});
	s.score[SIGN_MIDDLE_FINGER] = all({di, m, dg, dl});
	s.score[SIGN_HORNS] = all({i, dm, dg, l});
	s.score[SIGN_OK] = all({touching, m, g, l});
	for (int k = 0; k < HAND_SHAPES; k++)
		if (s.best < 0 || s.score[k] > s.best_score) {
			s.best = k;
			s.best_score = s.score[k];
		}
	return s;
}

FaceMovesNow read_face_moves(const float mv[EXPRESSIONS], const FaceTune &t)
{
	static const int jaw = move_id("jawOpen"), inner_up = move_id("browInnerUp"),
			 outer_l = move_id("browOuterUpLeft"), outer_r = move_id("browOuterUpRight"),
			 smile_l = move_id("mouthSmileLeft"), smile_r = move_id("mouthSmileRight"),
			 pucker = move_id("mouthPucker"), blink_l = move_id("eyeBlinkLeft"),
			 blink_r = move_id("eyeBlinkRight");
	FaceMovesNow f;
	f.value[MOVE_MOUTH_OPEN] = through(mv[jaw], t.range[TUNE_MOUTH]);
	const TuneRange &brows = t.range[TUNE_BROWS_UP];
	f.value[MOVE_BROWS_UP] =
		std::max(through(mv[inner_up], brows), (through(mv[outer_l], brows) + through(mv[outer_r], brows)) / 2);
	f.value[MOVE_SMILE] =
		(through(mv[smile_l], t.range[TUNE_SMILE]) + through(mv[smile_r], t.range[TUNE_SMILE])) / 2;
	f.value[MOVE_KISS] = through(mv[pucker], t.range[TUNE_ROUND]);
	// Each eye on its own (whether or not the character's are linked).
	const float shut_l = through(mv[blink_l], t.range[TUNE_BLINK]), shut_r = through(mv[blink_r], t.range[TUNE_BLINK]);
	f.wink[0] = clamp01(shut_l - shut_r);
	f.wink[1] = clamp01(shut_r - shut_l);
	f.value[MOVE_WINK] = std::max(f.wink[0], f.wink[1]);
	return f;
}

std::vector<SignRule> rules_from(const std::string &text, int *skipped)
{
	std::vector<SignRule> out;
	int bad = 0;
	json list = json::parse(text.empty() ? std::string("[]") : text, nullptr, false);
	if (!list.is_array()) {
		list = json::array();
		bad++;
	}
	for (const json &item : list) {
		if (out.size() >= MOST_RULES || !item.is_object()) {
			bad++;
			continue;
		}
		SignRule r;
		const json id = item.value("id", json());
		const json what = item.value("what", json());
		if (!id.is_string() || id.get<std::string>().empty() || id.get<std::string>().size() > 64 ||
		    !what.is_string()) {
			bad++;
			continue;
		}
		r.id = id.get<std::string>();
		const std::string w = what.get<std::string>();
		const int sign = name_of(w, HAND_SIGN_NAMES, HAND_SIGNS), move = name_of(w, FACE_MOVE_NAMES, FACE_MOVES);
		if (sign < 0 && move < 0) {
			bad++;
			continue;
		}
		r.face = sign < 0;
		r.what = r.face ? move : sign;
		const json side = item.value("side", json("either"));
		r.side = side.is_string() ? name_of(side.get<std::string>(), SIGN_SIDE_NAMES, SIGN_SIDES) : -1;
		if (r.side < 0)
			r.side = SIDE_EITHER;
		// Both eyes shut is a blink, not a wink.
		if (r.face && (r.what != MOVE_WINK || r.side == SIDE_BOTH))
			r.side = SIDE_EITHER;
		const json hold = item.value("hold_ms", json(500));
		r.hold = hold.is_number() ? std::max(0.0f, std::min(10.0f, hold.get<float>() / 1000.0f)) : 0.5f;
		const json raised = item.value("raised", json(false));
		r.raised = !r.face && raised.is_boolean() && raised.get<bool>();
		const json strength = item.value("strength", json(70));
		r.strength = strength.is_number() ? std::max(0.1f, std::min(1.0f, strength.get<float>() / 100.0f))
						  : 0.7f;
		out.push_back(r);
	}
	if (skipped)
		*skipped = bad;
	return out;
}

void SignWatch::set_rules(std::vector<SignRule> rules)
{
	// A rule kept (the same id and what) keeps how long it's been held.
	std::vector<Track> tracks(rules.size());
	for (size_t i = 0; i < rules.size(); i++)
		for (size_t j = 0; j < rules_.size(); j++)
			if (rules_[j].id == rules[i].id && rules_[j].face == rules[i].face &&
			    rules_[j].what == rules[i].what && j < tracks_.size()) {
				tracks[i] = tracks_[j];
				break;
			}
	rules_ = std::move(rules);
	tracks_ = std::move(tracks);
}

bool SignWatch::wants_hands() const
{
	return std::any_of(rules_.begin(), rules_.end(), [](const SignRule &r) { return !r.face; });
}

bool SignWatch::wants_face() const
{
	return std::any_of(rules_.begin(), rules_.end(), [](const SignRule &r) { return r.face; });
}

void SignWatch::reset()
{
	for (Hand &h : hands_)
		h = Hand();
	for (Track &t : tracks_)
		t = Track();
	const int fired = seen_.fired;
	const std::string last = seen_.last;
	seen_ = SignsSeen();
	seen_.fired = fired;
	seen_.last = last;
}

std::vector<SignFired> SignWatch::take_fired()
{
	std::vector<SignFired> out;
	out.swap(fired_);
	return out;
}

void SignWatch::look_at_hands(const Body &body)
{
	for (int s = 0; s < 2; s++) {
		const HandSeen &seen = body.hands[s];
		Hand &h = hands_[s];
		HandNow &now = seen_.hands[s];
		if (!seen.found) {
			h = Hand();
			now = HandNow();
			continue;
		}
		const HandShape shape = read_hand_shape(seen.world);
		// The shape it holds: one counts once it's sure, and stays while
		// it's fairly sure - unless another is surer.
		int keep = h.sign;
		if (keep >= 0 && shape.score[keep] < SHAPE_STAY)
			keep = -1;
		if (shape.best >= 0 && shape.best != keep && shape.best_score >= SHAPE_ENTER &&
		    (keep < 0 || shape.best_score > shape.score[keep]))
			keep = shape.best;
		h.sign = keep;
		now.found = true;
		now.sign = keep;
		now.score = keep >= 0 ? shape.score[keep] : shape.best_score;
		now.raised = hand_raised(body, seen);
		// A wave: open (three fingers out, at least), going side to
		// side - in hand lengths, so near the camera or far is the same.
		// Closed a moment (a blurred look) it waits; longer, it's over.
		Wave &w = h.wave;
		int out = 0;
		for (float o : shape.out)
			out += o >= 0.5f ? 1 : 0;
		const float size = std::hypot(seen.points[9].x - seen.points[0].x, seen.points[9].y - seen.points[0].y);
		const float px = (seen.points[9].x + seen.points[12].x) / 2;
		if (out < 3 || size < 1) {
			if (w.started && clock_ - w.open_at > 0.3f)
				w = Wave();
			now.waving = (int)w.swings.size() >= WAVE_SWINGS;
			continue;
		}
		w.open_at = clock_;
		if (!w.started) {
			w = Wave();
			w.started = true;
			w.open_at = clock_;
			w.size = size;
			w.last_px = px;
			now.waving = false;
			continue;
		}
		w.size = w.size * 0.8f + size * 0.2f;
		w.x += (px - w.last_px) / std::max(1.0f, w.size);
		w.last_px = px;
		bool swung = false;
		if (w.dir == 0) {
			if (std::fabs(w.x - w.extreme) >= WAVE_SWING) {
				w.dir = w.x > w.extreme ? 1 : -1;
				swung = true;
			}
		} else if (w.dir > 0 ? w.x > w.extreme : w.x < w.extreme) {
			w.extreme = w.x;
		} else if (std::fabs(w.x - w.extreme) >= WAVE_SWING) {
			w.dir = -w.dir;
			swung = true;
		}
		if (swung) {
			w.extreme = w.x;
			w.swings.push_back(clock_);
		}
		w.swings.erase(std::remove_if(w.swings.begin(), w.swings.end(),
					      [&](float t) { return t < clock_ - WAVE_TIME; }),
			       w.swings.end());
		now.waving = (int)w.swings.size() >= WAVE_SWINGS;
	}
}

void SignWatch::step(float dt, const Body *body, bool body_fresh, const float *moves, const FaceTune &tune)
{
	clock_ += dt;
	if (!body) {
		for (int s = 0; s < 2; s++) {
			hands_[s] = Hand();
			seen_.hands[s] = HandNow();
		}
	} else if (body_fresh) {
		look_at_hands(*body);
	}
	seen_.face_found = moves != nullptr;
	seen_.face = moves ? read_face_moves(moves, tune) : FaceMovesNow();
	auto hand_on = [&](const SignRule &r, int s) {
		const HandNow &h = seen_.hands[s];
		if (!h.found || (r.raised && !h.raised))
			return false;
		return r.what == SIGN_WAVE ? h.waving : h.sign == r.what;
	};
	for (size_t k = 0; k < rules_.size() && k < tracks_.size(); k++) {
		const SignRule &r = rules_[k];
		Track &t = tracks_[k];
		bool on;
		const char *side = "";
		if (r.face) {
			const FaceMovesNow &f = seen_.face;
			const float v = r.what != MOVE_WINK  ? f.value[r.what]
					: r.side == SIDE_LEFT  ? f.wink[0]
					: r.side == SIDE_RIGHT ? f.wink[1]
							       : f.value[MOVE_WINK];
			// Past the strength to start, and a little under it to stay.
			on = seen_.face_found && v >= (t.on ? r.strength - 0.1f : r.strength);
			// Which eye winked.
			if (r.what == MOVE_WINK)
				side = r.side == SIDE_EITHER ? (f.wink[0] >= f.wink[1] ? "left" : "right")
							     : SIGN_SIDE_NAMES[r.side];
		} else {
			const bool l = hand_on(r, 0), rr = hand_on(r, 1);
			on = r.side == SIDE_LEFT    ? l
			     : r.side == SIDE_RIGHT ? rr
			     : r.side == SIDE_BOTH  ? l && rr
						    : l || rr;
			// Which hand made it (both, if both did).
			side = r.side != SIDE_EITHER ? SIGN_SIDE_NAMES[r.side] : l && rr ? "both" : l ? "left" : "right";
		}
		t.on = on;
		if (on) {
			t.off_for = 0;
			t.held += dt;
			// A wave counts as it's seen (it took its swings).
			const float need = !r.face && r.what == SIGN_WAVE ? 0 : r.hold;
			if (t.armed && t.held >= need) {
				t.armed = false;
				const char *what = r.face ? FACE_MOVE_NAMES[r.what] : HAND_SIGN_NAMES[r.what];
				fired_.push_back({r.id, what, side});
				if (fired_.size() > 20)
					fired_.erase(fired_.begin());
				seen_.fired++;
				seen_.last = r.id;
			}
		} else {
			t.off_for += dt;
			// Let go for a moment: it can count again.
			if (t.off_for >= SIGN_GRACE) {
				t.held = 0;
				t.armed = true;
			}
		}
	}
}

} // namespace direbot::face
