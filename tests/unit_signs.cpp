// Hand signs and face moves that set things off (src/face/signs.hpp): each
// hand shape from hands made of straight and curled fingers, turned about;
// loose and claw-shaped hands that are no sign; Google's own pictures of a
// thumb up, a peace sign, pointing up and a fist (tests/data/body) through
// our own body tracker; a wave and things that aren't one; and the watch's
// holds, its sides, a raised hand, face moves and winks, and the rules
// DireBot sends.

#include "../src/face/signs.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
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

static const std::string ROOT = DIREBOT_SOURCE_DIR;
constexpr float PI = 3.14159265f;

// --- Hands made of straight and curled fingers ------------------------------

struct P3 {
	float x, y, z;
};
static P3 add(P3 a, P3 b)
{
	return {a.x + b.x, a.y + b.y, a.z + b.z};
}
static P3 scale(P3 a, float s)
{
	return {a.x * s, a.y * s, a.z * s};
}

// A finger from its knuckle, up the picture (y is down), each joint bent
// towards the palm (it faces the camera: -z) by so many degrees.
static void finger(Point *p, int knuckle, P3 at, const float len[3], float a1, float a2, float a3)
{
	p[knuckle] = {at.x, at.y, at.z};
	float bend = 0;
	const float bends[3] = {a1, a2, a3};
	P3 here = at;
	for (int j = 0; j < 3; j++) {
		bend += bends[j] * PI / 180;
		here = add(here, scale({0, -std::cos(bend), -std::sin(bend)}, len[j]));
		p[knuckle + 1 + j] = {here.x, here.y, here.z};
	}
}

enum Thumb { THUMB_OUT, THUMB_IN, THUMB_UP, THUMB_OK };

// "E" a finger held out, "C" curled in, "H" half bent (a claw), "L" loose;
// first finger to little.
static std::vector<Point> hand_of(const char *fingers, Thumb thumb)
{
	std::vector<Point> p(HAND_POINTS);
	p[0] = {0, 0, 0};
	const P3 knuckles[4] = {{-0.03f, -0.085f, 0}, {-0.01f, -0.09f, 0}, {0.01f, -0.085f, 0}, {0.03f, -0.075f, 0}};
	const float lens[4][3] = {{0.04f, 0.025f, 0.02f},
				  {0.045f, 0.028f, 0.022f},
				  {0.042f, 0.026f, 0.021f},
				  {0.032f, 0.02f, 0.018f}};
	for (int f = 0; f < 4; f++) {
		const char c = fingers[f];
		const float a1 = c == 'E' ? 5 : c == 'C' ? 80 : c == 'H' ? 25 : 35;
		const float a2 = c == 'E' ? 5 : c == 'C' ? 100 : c == 'H' ? 55 : 40;
		const float a3 = c == 'E' ? 0 : c == 'C' ? 60 : c == 'H' ? 35 : 20;
		finger(p.data(), 5 + 4 * f, knuckles[f], lens[f], a1, a2, a3);
	}
	p[1] = {-0.02f, -0.02f, -0.01f};
	switch (thumb) {
	case THUMB_OUT:
		p[2] = {-0.04f, -0.035f, -0.015f};
		p[3] = {-0.065f, -0.05f, -0.015f};
		p[4] = {-0.085f, -0.062f, -0.015f};
		break;
	case THUMB_IN:
		p[2] = {-0.035f, -0.04f, -0.03f};
		p[3] = {-0.035f, -0.065f, -0.045f};
		p[4] = {-0.02f, -0.085f, -0.045f};
		break;
	case THUMB_UP:
		p[2] = {-0.045f, -0.03f, -0.01f};
		p[3] = {-0.055f, -0.06f, -0.005f};
		p[4] = {-0.06f, -0.09f, 0};
		break;
	case THUMB_OK: {
		p[2] = {-0.045f, -0.035f, -0.02f};
		const Point tip = p[8];
		p[4] = {tip.x - 0.003f, tip.y + 0.004f, tip.z};
		p[3] = {(p[2].x + p[4].x) / 2 - 0.01f, (p[2].y + p[4].y) / 2, (p[2].z + p[4].z) / 2};
		break;
	}
	}
	return p;
}

// Turned about the wrist: round the camera's line (roll), side to side
// (yaw), and top towards or away (pitch), degrees.
static std::vector<Point> turned(std::vector<Point> p, float roll, float yaw, float pitch)
{
	const float r = roll * PI / 180, y = yaw * PI / 180, t = pitch * PI / 180;
	for (Point &q : p) {
		float x = q.x * std::cos(r) - q.y * std::sin(r), yy = q.x * std::sin(r) + q.y * std::cos(r), z = q.z;
		const float x2 = x * std::cos(y) + z * std::sin(y), z2 = -x * std::sin(y) + z * std::cos(y);
		const float y3 = yy * std::cos(t) - z2 * std::sin(t), z3 = yy * std::sin(t) + z2 * std::cos(t);
		q = {x2, y3, z3};
	}
	return p;
}

static std::vector<Point> mirrored(std::vector<Point> p)
{
	for (Point &q : p)
		q.x = -q.x;
	return p;
}

// Degrees from straight up the picture, from one point to another.
static float from_up(const std::vector<Point> &p, int from, int to)
{
	const float x = p[to].x - p[from].x, y = p[to].y - p[from].y, z = p[to].z - p[from].z;
	return std::acos(std::max(-1.0f, std::min(1.0f, -y / std::sqrt(x * x + y * y + z * z)))) * 180 / PI;
}

static int sign_of(const std::vector<Point> &p)
{
	const HandShape s = read_hand_shape(p.data());
	return s.best_score >= SHAPE_ENTER ? s.best : -1;
}

static const char *name(int sign)
{
	return sign >= 0 ? HAND_SIGN_NAMES[sign] : "none";
}

static void test_made_hands()
{
	const struct {
		int sign;
		const char *fingers;
		Thumb thumb;
		bool points; // which way it points matters
	} made[] = {
		{SIGN_OPEN_PALM, "EEEE", THUMB_OUT, false},  {SIGN_FIST, "CCCC", THUMB_IN, false},
		{SIGN_THUMBS_UP, "CCCC", THUMB_UP, true},    {SIGN_POINT_UP, "ECCC", THUMB_IN, true},
		{SIGN_FINGER_GUN, "ECCC", THUMB_UP, false},  {SIGN_PEACE, "EECC", THUMB_IN, false},
		{SIGN_PEACE, "EECC", THUMB_OUT, false},      {SIGN_PEACE, "EEHH", THUMB_IN, false},
		{SIGN_MIDDLE_FINGER, "CECC", THUMB_IN, false}, {SIGN_HORNS, "ECCE", THUMB_IN, false},
		{SIGN_HORNS, "ECCE", THUMB_OUT, false},      {SIGN_OK, "CEEE", THUMB_OK, false},
	};
	int right = 0, tried = 0;
	for (const auto &m : made) {
		const std::vector<Point> hand = hand_of(m.fingers, m.thumb);
		const float rolls[] = {0, 20, -20, 35, -35};
		const float yaws[] = {0, 30, -30, 50};
		const float pitches[] = {0, 25, -25};
		for (float roll : rolls)
			for (float yaw : yaws)
				for (float pitch : pitches)
					for (int mirror = 0; mirror < 2; mirror++) {
						std::vector<Point> p = turned(hand, roll, yaw, pitch);
						if (mirror)
							p = mirrored(p);
						const int got = sign_of(p);
						// Pointing up: turned up to 40 degrees from
						// straight up it is; past 60 it isn't.
						bool want = true;
						if (m.points) {
							const float off = m.sign == SIGN_THUMBS_UP ? from_up(p, 2, 4)
												   : from_up(p, 5, 8);
							if (off > 40 && off <= 60)
								continue;
							want = off <= 40;
						}
						tried++;
						const bool ok = want ? got == m.sign : got != m.sign;
						right += ok;
						if (!ok)
							std::fprintf(stderr,
								     "unit_signs: %s (%s) turned %g %g %g%s read as %s\n",
								     name(m.sign), m.fingers, roll, yaw, pitch,
								     mirror ? " mirrored" : "", name(got));
					}
	}
	std::printf("unit_signs: made hands: %d of %d read right\n", right, tried);
	CHECK(right == tried);
	// Thumbs down: a thumbs up the other way up.
	CHECK(sign_of(turned(hand_of("CCCC", THUMB_UP), 180, 0, 0)) == SIGN_THUMBS_DOWN);
	CHECK(sign_of(turned(hand_of("CCCC", THUMB_UP), 160, 20, 10)) == SIGN_THUMBS_DOWN);
	// A thumb out sideways is neither up nor down; pointing down isn't
	// pointing up.
	CHECK(sign_of(turned(hand_of("CCCC", THUMB_UP), 90, 0, 0)) == -1);
	CHECK(sign_of(turned(hand_of("ECCC", THUMB_IN), 180, 0, 0)) == -1);
	// No sign: loose and claw-shaped hands, three fingers, a pinky, a ring
	// finger. (Four fingers up, the thumb folded, is an open palm.)
	const struct {
		const char *fingers;
		Thumb thumb;
	} none[] = {{"HHHH", THUMB_OUT}, {"HHHH", THUMB_IN}, {"LLLL", THUMB_OUT}, {"LLLL", THUMB_IN},
		    {"EEEC", THUMB_IN},  {"CCCE", THUMB_IN},  {"CCEC", THUMB_IN}};
	for (const auto &n : none)
		for (float roll : {0.0f, 30.0f, -30.0f}) {
			const int got = sign_of(turned(hand_of(n.fingers, n.thumb), roll, 15, 10));
			if (got != -1)
				std::fprintf(stderr, "unit_signs: %s read as %s\n", n.fingers, name(got));
			CHECK(got == -1);
		}
	// Nothing at all (every point at the wrist) is no sign.
	std::vector<Point> zero(HAND_POINTS);
	CHECK(read_hand_shape(zero.data()).best_score == 0);
}

// --- Google's pictures, through our own body tracker ------------------------

struct Picture {
	int w = 0, h = 0;
	std::vector<uint8_t> rgba;
};

static Picture read_ppm(const std::string &path, bool mirror)
{
	std::ifstream in(path, std::ios::binary);
	const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	Picture p;
	size_t at = 0;
	auto word = [&]() {
		while (at < data.size() && std::isspace(data[at]))
			at++;
		std::string w;
		while (at < data.size() && !std::isspace(data[at]))
			w += (char)data[at++];
		return w;
	};
	if (word() != "P6")
		return p;
	p.w = std::atoi(word().c_str());
	p.h = std::atoi(word().c_str());
	if (word() != "255" || p.w <= 0 || p.h <= 0 || data.size() < ++at + (size_t)p.w * p.h * 3)
		return Picture();
	p.rgba.resize((size_t)p.w * p.h * 4);
	for (int y = 0; y < p.h; y++)
		for (int x = 0; x < p.w; x++) {
			const uint8_t *i = &data[at + ((size_t)y * p.w + (mirror ? p.w - 1 - x : x)) * 3];
			uint8_t *o = &p.rgba[((size_t)y * p.w + x) * 4];
			o[0] = i[0];
			o[1] = i[1];
			o[2] = i[2];
			o[3] = 255;
		}
	return p;
}

static void test_googles_pictures()
{
	BodyTracker t;
	std::string problem;
	if (!t.load_files(ROOT + "/data/face/holistic_landmarker.task", ROOT + "/data/face/hand_landmark_lite.tflite",
			  &problem, ROOT + "/data/face/palm_detection_full.tflite")) {
		std::fprintf(stderr, "unit_signs: %s\n", problem.c_str());
		CHECK(false);
		return;
	}
	t.set_smoothing(-1);
	const struct {
		const char *file;
		int sign;
	} pictures[] = {{"thumb_up.ppm", SIGN_THUMBS_UP},
			{"victory.ppm", SIGN_PEACE},
			{"pointing_up.ppm", SIGN_POINT_UP},
			{"fist.ppm", SIGN_FIST}};
	for (const auto &pc : pictures)
		for (int mirror = 0; mirror < 2; mirror++) {
			const Picture pic = read_ppm(ROOT + "/tests/data/body/" + pc.file, mirror);
			CHECK(pic.w > 0);
			if (!pic.w)
				continue;
			Frame f;
			f.px = pic.rgba.data();
			f.w = pic.w;
			f.h = pic.h;
			f.stride = pic.w * 4;
			t.forget();
			// A few looks running, as a camera's would be.
			const Body *b = nullptr;
			for (int i = 0; i < 6; i++)
				b = &t.look(f, i / 30.0);
			const int s = b->hands[1].found ? 1 : 0;
			CHECK(b->hands[s].found);
			if (!b->hands[s].found)
				continue;
			const HandShape shape = read_hand_shape(b->hands[s].world);
			std::printf("unit_signs: %s%s: %s (%.2f sure)\n", pc.file, mirror ? " mirrored" : "",
				    name(shape.best_score >= SHAPE_ENTER ? shape.best : -1), shape.best_score);
			CHECK(shape.best == pc.sign && shape.best_score >= SHAPE_ENTER);
			// It's the hand's own side that's mirrored, not the sign.
			CHECK(b->hands[1 - s].found == false);
		}
}

// --- The watch ----------------------------------------------------------------

// A body with one hand (your own `side`) in the picture: its world points
// as made, its picture points a millimetre to a pixel round (x, y).
static Body body_with(const std::vector<Point> &hand, int side, float x, float y, bool both = false)
{
	Body b;
	b.found = true;
	b.w = 640;
	b.h = 480;
	// Mouth at 200, shoulders at 300 down the picture: raised is above 250.
	b.points[9] = {300, 200, 0};
	b.points[10] = {340, 200, 0};
	b.points[11] = {250, 300, 0};
	b.points[12] = {390, 300, 0};
	for (int i : {9, 10, 11, 12})
		b.seen[i] = 1;
	for (int s = 0; s < 2; s++) {
		if (s != side && !both)
			continue;
		HandSeen &h = b.hands[s];
		h.found = true;
		for (int i = 0; i < HAND_POINTS; i++) {
			h.world[i] = hand[i];
			h.points[i] = {x + hand[i].x * 1000 + (s != side ? 150 : 0), y + hand[i].y * 1000, hand[i].z * 1000};
		}
	}
	return b;
}

static const FaceTune TUNE;

// Steps `seconds` at 60 frames a second; how many fired.
static int run(SignWatch &w, float seconds, const Body *body, const float *moves = nullptr)
{
	int fired = 0;
	const int frames = (int)std::lround(seconds * 60);
	for (int i = 0; i < frames; i++) {
		w.step(1.0f / 60, body, true, moves, TUNE);
		fired += (int)w.take_fired().size();
	}
	return fired;
}

static SignRule rule(const char *id, int what, int side, float hold, bool face = false)
{
	SignRule r;
	r.id = id;
	r.what = what;
	r.side = side;
	r.hold = hold;
	r.face = face;
	return r;
}

static void test_holds()
{
	SignWatch w;
	w.set_rules({rule("up", SIGN_THUMBS_UP, SIDE_EITHER, 0.5f)});
	CHECK(w.wants_hands() && !w.wants_face());
	const Body up = body_with(hand_of("CCCC", THUMB_UP), 1, 320, 240);
	const Body fist = body_with(hand_of("CCCC", THUMB_IN), 1, 320, 240);
	const Body nobody;
	// Not before its hold; then once, however long it's held.
	CHECK(run(w, 0.45f, &up) == 0);
	CHECK(run(w, 0.1f, &up) == 1);
	CHECK(w.seen().hands[1].found && w.seen().hands[1].sign == SIGN_THUMBS_UP);
	CHECK(!w.seen().hands[0].found);
	CHECK(run(w, 3, &up) == 0);
	// Gone a moment (under SIGN_GRACE) isn't let go.
	CHECK(run(w, 0.1f, &fist) == 0);
	CHECK(run(w, 1, &up) == 0);
	// Let go, then held again: again.
	CHECK(run(w, 0.3f, &fist) == 0);
	CHECK(run(w, 0.4f, &up) == 0);
	CHECK(run(w, 0.15f, &up) == 1);
	CHECK(w.seen().fired == 2 && w.seen().last == "up");
	// A hand gone counts as let go; no hands looked at at all, too.
	CHECK(run(w, 0.3f, &nobody) == 0);
	CHECK(!w.seen().hands[1].found);
	CHECK(run(w, 0.6f, &up) == 1);
	CHECK(run(w, 0.3f, nullptr) == 0);
	CHECK(run(w, 0.6f, &up) == 1);
	// No hold: straight away.
	SignWatch now;
	now.set_rules({rule("fist", SIGN_FIST, SIDE_EITHER, 0)});
	CHECK(run(now, 1.0f / 60, &fist) == 1);
	// Rules changed: one kept keeps how long it's been held.
	w.set_rules({rule("up", SIGN_THUMBS_UP, SIDE_EITHER, 0.5f), rule("fist", SIGN_FIST, SIDE_EITHER, 0.5f)});
	CHECK(run(w, 1, &up) == 0);
	CHECK(run(w, 0.3f, &fist) == 0);
	CHECK(run(w, 0.3f, &fist) == 1);
}

static void test_sides()
{
	const std::vector<Point> peace = hand_of("EECC", THUMB_IN);
	const Body left = body_with(peace, 0, 320, 240), right = body_with(peace, 1, 320, 240),
		   both = body_with(peace, 0, 320, 240, true);
	auto fires = [](int side, const Body &b) {
		SignWatch w;
		w.set_rules({rule("p", SIGN_PEACE, side, 0.2f)});
		return run(w, 0.5f, &b);
	};
	CHECK(fires(SIDE_EITHER, left) == 1 && fires(SIDE_EITHER, right) == 1 && fires(SIDE_EITHER, both) == 1);
	CHECK(fires(SIDE_LEFT, left) == 1 && fires(SIDE_LEFT, right) == 0 && fires(SIDE_LEFT, both) == 1);
	CHECK(fires(SIDE_RIGHT, left) == 0 && fires(SIDE_RIGHT, right) == 1 && fires(SIDE_RIGHT, both) == 1);
	CHECK(fires(SIDE_BOTH, left) == 0 && fires(SIDE_BOTH, right) == 0 && fires(SIDE_BOTH, both) == 1);
	// Raised: only with the hand up by your face (the made hand's middle
	// is 90 px above where it's put).
	SignWatch w;
	SignRule r = rule("r", SIGN_PEACE, SIDE_EITHER, 0.2f);
	r.raised = true;
	w.set_rules({r});
	const Body low = body_with(peace, 1, 320, 420), high = body_with(peace, 1, 320, 300);
	CHECK(run(w, 0.5f, &low) == 0);
	CHECK(!w.seen().hands[1].raised && w.seen().hands[1].sign == SIGN_PEACE);
	CHECK(run(w, 0.5f, &high) == 1);
	CHECK(w.seen().hands[1].raised);
	// Without the shoulders in sight, a little below the mouth; without
	// the face, the picture's top half.
	Body no_shoulders = body_with(peace, 1, 320, 380);
	no_shoulders.seen[11] = no_shoulders.seen[12] = 0;
	no_shoulders.points[2] = {300, 150, 0};
	no_shoulders.points[5] = {340, 150, 0};
	no_shoulders.seen[2] = no_shoulders.seen[5] = 1;
	SignWatch w2;
	w2.set_rules({r});
	run(w2, 0.1f, &no_shoulders);
	CHECK(!w2.seen().hands[1].raised); // 290 is past 200 + 1.2 x 50
	no_shoulders = body_with(peace, 1, 320, 340);
	no_shoulders.seen[11] = no_shoulders.seen[12] = 0;
	no_shoulders.points[2] = {300, 150, 0};
	no_shoulders.points[5] = {340, 150, 0};
	no_shoulders.seen[2] = no_shoulders.seen[5] = 1;
	run(w2, 0.1f, &no_shoulders);
	CHECK(w2.seen().hands[1].raised); // 250 isn't
	Body faceless = body_with(peace, 1, 320, 300);
	faceless.found = false;
	run(w2, 0.1f, &faceless);
	CHECK(w2.seen().hands[1].raised);
	faceless = body_with(peace, 1, 320, 400);
	faceless.found = false;
	run(w2, 0.1f, &faceless);
	CHECK(!w2.seen().hands[1].raised);
}

// A hand going side to side (picture pixels: it's 90 across, wrist to
// knuckle), `hz` swings back and forth a second, for `seconds`, 30 looks
// a second; how many waves.
static int waves(SignWatch &w, const std::vector<Point> &hand, float px, float hz, float seconds, float *first = nullptr)
{
	int fired = 0;
	const int frames = (int)std::lround(seconds * 60);
	for (int i = 0; i < frames; i++) {
		const float t = i / 60.0f;
		const Body b = body_with(hand, 1, 320 + px * std::sin(2 * PI * hz * t), 240);
		w.step(1.0f / 60, &b, i % 2 == 0, nullptr, TUNE);
		const int n = (int)w.take_fired().size();
		if (n && !fired && first)
			*first = t;
		fired += n;
	}
	return fired;
}

static void test_wave()
{
	const std::vector<Point> open = hand_of("EEEE", THUMB_OUT), fist = hand_of("CCCC", THUMB_IN);
	auto watch = [] {
		SignWatch w;
		w.set_rules({rule("hi", SIGN_WAVE, SIDE_EITHER, 2.0f)});
		return w;
	};
	// Waving (60 px each way, twice a second): once, within a second -
	// the hold doesn't hold up a wave.
	SignWatch w = watch();
	float first = -1;
	CHECK(waves(w, open, 60, 2, 3, &first) == 1);
	std::printf("unit_signs: a wave seen after %.2f s\n", first);
	CHECK(first > 0 && first < 1.0f);
	CHECK(w.seen().hands[1].waving);
	// Stopped, then waving again: again.
	CHECK(waves(w, open, 0, 1, 2) == 0);
	CHECK(!w.seen().hands[1].waving);
	CHECK(waves(w, open, 60, 2, 2) == 1);
	// Not a wave: a fist going side to side; a small shake; one slow move.
	SignWatch a = watch(), b = watch(), c = watch();
	CHECK(waves(a, fist, 60, 2, 3) == 0);
	CHECK(waves(b, open, 15, 3, 3) == 0);
	CHECK(waves(c, open, 120, 0.15f, 3) == 0);
	// Near the camera or far: the same wave, in hand lengths.
	SignWatch d = watch();
	std::vector<Point> far = open;
	for (Point &p : far)
		p = {p.x * 0.4f, p.y * 0.4f, p.z * 0.4f};
	CHECK(waves(d, far, 60 * 0.4f, 2, 3) == 1);
}

static void test_face_moves()
{
	auto id = [](const char *n) {
		for (int i = 0; i < EXPRESSIONS; i++)
			if (std::string(EXPRESSION_NAMES[i]) == n)
				return i;
		return 0;
	};
	float rest[EXPRESSIONS] = {}, open[EXPRESSIONS] = {}, wink_l[EXPRESSIONS] = {}, wink_r[EXPRESSIONS] = {},
	      blink[EXPRESSIONS] = {}, talk[EXPRESSIONS] = {};
	open[id("jawOpen")] = 0.6f;
	talk[id("jawOpen")] = 0.3f;
	wink_l[id("eyeBlinkLeft")] = 0.7f;
	wink_l[id("eyeBlinkRight")] = 0.15f;
	wink_r[id("eyeBlinkRight")] = 0.7f;
	wink_r[id("eyeBlinkLeft")] = 0.15f;
	blink[id("eyeBlinkLeft")] = blink[id("eyeBlinkRight")] = 0.7f;
	const FaceMovesNow f = read_face_moves(open, TUNE);
	CHECK(f.value[MOVE_MOUTH_OPEN] == 1.0f);
	CHECK(read_face_moves(rest, TUNE).value[MOVE_MOUTH_OPEN] == 0);
	CHECK(read_face_moves(wink_l, TUNE).wink[0] > 0.9f && read_face_moves(wink_l, TUNE).wink[1] == 0);
	CHECK(read_face_moves(blink, TUNE).value[MOVE_WINK] == 0);
	// Through your tuning: a mouth tuned to open further needs more.
	FaceTune wide;
	wide.range[TUNE_MOUTH] = {0.1f, 0.9f};
	CHECK(read_face_moves(open, wide).value[MOVE_MOUTH_OPEN] < 0.7f);
	// Each its own move: brows, smile, a kiss.
	float brows[EXPRESSIONS] = {}, smile[EXPRESSIONS] = {}, kiss[EXPRESSIONS] = {};
	brows[id("browInnerUp")] = 0.8f;
	smile[id("mouthSmileLeft")] = smile[id("mouthSmileRight")] = 0.75f;
	kiss[id("mouthPucker")] = 0.75f;
	CHECK(read_face_moves(brows, TUNE).value[MOVE_BROWS_UP] == 1.0f);
	CHECK(read_face_moves(smile, TUNE).value[MOVE_SMILE] == 1.0f);
	CHECK(read_face_moves(kiss, TUNE).value[MOVE_KISS] == 1.0f);
	CHECK(read_face_moves(kiss, TUNE).value[MOVE_SMILE] == 0);

	SignWatch w;
	SignRule mouth = rule("aah", MOVE_MOUTH_OPEN, SIDE_EITHER, 0.6f, true);
	mouth.strength = 0.9f;
	SignRule left = rule("wl", MOVE_WINK, SIDE_LEFT, 0.2f, true), right = rule("wr", MOVE_WINK, SIDE_RIGHT, 0.2f, true),
		 any = rule("w", MOVE_WINK, SIDE_EITHER, 0.2f, true);
	left.strength = right.strength = any.strength = 0.5f;
	w.set_rules({mouth, left, right, any});
	CHECK(w.wants_face() && !w.wants_hands());
	auto fired = [&](const float *moves, float seconds) {
		std::vector<std::string> ids;
		const int frames = (int)std::lround(seconds * 60);
		for (int i = 0; i < frames; i++) {
			w.step(1.0f / 60, nullptr, false, moves, TUNE);
			for (const SignFired &s : w.take_fired())
				ids.push_back(s.id);
		}
		return ids;
	};
	CHECK(fired(talk, 2).empty()); // talking isn't wide open
	CHECK(fired(open, 0.5f).empty());
	CHECK(fired(open, 0.2f) == std::vector<std::string>{"aah"});
	CHECK(fired(rest, 0.5f).empty());
	CHECK((fired(wink_l, 0.5f) == std::vector<std::string>{"wl", "w"}));
	CHECK(fired(rest, 0.5f).empty());
	CHECK((fired(wink_r, 0.5f) == std::vector<std::string>{"wr", "w"}));
	CHECK(fired(blink, 1).empty());
	// The face not found is no face move.
	CHECK(fired(nullptr, 1).empty());
	CHECK(!w.seen().face_found);
	// What fired says what and which side.
	SignWatch v;
	v.set_rules({left});
	v.step(0.5f, nullptr, false, wink_l, TUNE);
	const std::vector<SignFired> got = v.take_fired();
	CHECK(got.size() == 1 && got[0].id == "wl" && got[0].what == "wink" && got[0].side == "left");
	SignWatch e;
	e.set_rules({any, mouth});
	e.step(0.5f, nullptr, false, wink_r, TUNE);
	std::vector<SignFired> eye = e.take_fired();
	CHECK(eye.size() == 1 && eye[0].side == "right");
	e.step(0.7f, nullptr, false, open, TUNE);
	eye = e.take_fired();
	CHECK(eye.size() == 1 && eye[0].id == "aah" && eye[0].side.empty());
}

// Which hand made it.
static void test_which_hand()
{
	const std::vector<Point> fist = hand_of("CCCC", THUMB_IN);
	const Body left = body_with(fist, 0, 320, 240), right = body_with(fist, 1, 320, 240),
		   both = body_with(fist, 0, 320, 240, true);
	auto side_of = [](int side, const Body &b) {
		SignWatch w;
		w.set_rules({rule("f", SIGN_FIST, side, 0)});
		w.step(0.1f, &b, true, nullptr, TUNE);
		const std::vector<SignFired> got = w.take_fired();
		return got.size() == 1 ? got[0].side : std::string("?");
	};
	CHECK(side_of(SIDE_EITHER, left) == "left" && side_of(SIDE_EITHER, right) == "right");
	CHECK(side_of(SIDE_EITHER, both) == "both" && side_of(SIDE_BOTH, both) == "both");
	CHECK(side_of(SIDE_LEFT, both) == "left" && side_of(SIDE_RIGHT, right) == "right");
}

static void test_rules()
{
	int skipped = -1;
	std::vector<SignRule> r = rules_from(
		R"([{"id": "a", "what": "thumbs_up", "side": "right", "hold_ms": 800, "raised": true},
		    {"id": "b", "what": "wink", "side": "both", "strength": 40},
		    {"id": "c", "what": "smile", "side": "left", "raised": true, "strength": 500, "hold_ms": -5},
		    {"id": "d", "what": "wave"},
		    {"id": "", "what": "fist"}, {"what": "fist"}, {"id": "e", "what": "dab"}, 7, "x",
		    {"id": "f", "what": "fist", "side": "sideways", "hold_ms": "lots"}])",
		&skipped);
	CHECK(skipped == 5);
	CHECK(r.size() == 5);
	if (r.size() == 5) {
		CHECK(r[0].id == "a" && !r[0].face && r[0].what == SIGN_THUMBS_UP && r[0].side == SIDE_RIGHT &&
		      std::fabs(r[0].hold - 0.8f) < 1e-6f && r[0].raised);
		// Both eyes isn't a wink: either.
		CHECK(r[1].face && r[1].what == MOVE_WINK && r[1].side == SIDE_EITHER &&
		      std::fabs(r[1].strength - 0.4f) < 1e-6f && std::fabs(r[1].hold - 0.5f) < 1e-6f);
		// A face move has no side and is never "raised"; numbers kept in reach.
		CHECK(r[2].face && r[2].side == SIDE_EITHER && !r[2].raised && r[2].strength == 1.0f && r[2].hold == 0);
		CHECK(!r[3].face && r[3].what == SIGN_WAVE && r[3].side == SIDE_EITHER);
		CHECK(r[4].id == "f" && r[4].side == SIDE_EITHER && std::fabs(r[4].hold - 0.5f) < 1e-6f);
	}
	CHECK(rules_from("").empty());
	CHECK(rules_from("not json", &skipped).empty() && skipped == 1);
	CHECK(rules_from("{}", &skipped).empty() && skipped == 1);
	std::string many = "[";
	for (int i = 0; i < 60; i++)
		many += std::string(i ? "," : "") + "{\"id\":\"r" + std::to_string(i) + "\",\"what\":\"fist\"}";
	many += "]";
	CHECK(rules_from(many, &skipped).size() == 50 && skipped == 10);
}

int main()
{
	test_made_hands();
	test_googles_pictures();
	test_holds();
	test_sides();
	test_wave();
	test_face_moves();
	test_which_hand();
	test_rules();
	std::printf("unit_signs: %d passed, %d failed\n", passes, failures);
	return failures ? 1 : 0;
}
