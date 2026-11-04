// Unit tests for following your hands and arms (src/face/body.cpp,
// src/face/arms.cpp): each body and hand model on our engine against
// Google's own runtime (LiteRT), the body tracker against MediaPipe's
// Holistic Landmarker on the same pictures (tests/data/body - see
// tools/body-expected.py), following and losing a body, the hand gates,
// then the arm and finger solver: which way each part points, mirrored
// or not, the elbow's hinge, reach and arms held out, by your head, each
// finger joint's bend and its limits, a hand seen twice before it moves
// anything, hold-and-fade, and calibration.
// Build and run: tools/test-link.sh (Linux, any C++17 compiler).

#include "../src/face/arms.hpp"
#include "../src/face/body.hpp"
#include "../src/face/vision.hpp"
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

static bool near3(V3 a, V3 b, float by)
{
	return length(a - b) <= by;
}

static const std::string ROOT = DIREBOT_SOURCE_DIR;
static const std::string TASK = ROOT + "/data/face/holistic_landmarker.task";
static const std::string LITE = ROOT + "/data/face/hand_landmark_lite.tflite";
static const std::string DATA = ROOT + "/tests/data/body/";
constexpr float PI = 3.14159265f;

static std::vector<uint8_t> read_file(const std::string &path)
{
	std::ifstream in(path, std::ios::binary);
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static const json &expected()
{
	static const json j = json::parse(read_file(DATA + "expected.json"), nullptr, false);
	return j;
}

// A picture, red, green, blue (a grey one's grey three times).
struct Picture {
	int w = 0, h = 0;
	std::vector<uint8_t> rgb;
	const uint8_t *at(int x, int y) const { return &rgb[((size_t)y * w + x) * 3]; }
};

static Picture read_pnm(const std::string &path)
{
	Picture p;
	const std::vector<uint8_t> data = read_file(path);
	size_t at = 0;
	auto word = [&]() {
		while (at < data.size() && std::isspace(data[at]))
			at++;
		std::string w;
		while (at < data.size() && !std::isspace(data[at]))
			w += (char)data[at++];
		return w;
	};
	const std::string magic = word();
	p.w = std::atoi(word().c_str());
	p.h = std::atoi(word().c_str());
	const int top = std::atoi(word().c_str());
	at++;
	const int channels = magic == "P5" ? 1 : 3;
	if ((magic != "P5" && magic != "P6") || top != 255 || p.w <= 0 || p.h <= 0 ||
	    data.size() < at + (size_t)p.w * p.h * channels)
		return Picture();
	p.rgb.resize((size_t)p.w * p.h * 3);
	for (size_t i = 0; i < (size_t)p.w * p.h; i++)
		for (int c = 0; c < 3; c++)
			p.rgb[i * 3 + c] = data[at + i * channels + (channels == 3 ? c : 0)];
	return p;
}

static const Picture &astronaut()
{
	static const Picture p = read_pnm(DATA + "astronaut.ppm");
	return p;
}

static const Picture &camera()
{
	static const Picture p = read_pnm(DATA + "camera.pgm");
	return p;
}

static Picture mirrored(const Picture &p)
{
	Picture m = p;
	for (int y = 0; y < p.h; y++)
		for (int x = 0; x < p.w; x++)
			for (int c = 0; c < 3; c++)
				m.rgb[((size_t)y * p.w + x) * 3 + c] = p.at(p.w - 1 - x, y)[c];
	return m;
}

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

// --- Each model on our engine, against LiteRT -----------------------------

// How far apart two answers are: the most, over 1 + the size of LiteRT's.
struct Gap {
	double most = 0;
	void take(double ours, double theirs)
	{
		const double g = std::fabs(ours - theirs) / (1 + std::fabs(theirs));
		if (!(g <= most)) // NaN counts as far
			most = std::isfinite(g) ? g : 1e9;
	}
};

static bool load_net(nn::Net &net, const std::vector<uint8_t> &file, const std::string &name, bool plain)
{
	vision::Entry e;
	if (name.empty()) {
		e.data = file.data();
		e.size = file.size();
	} else if (!vision::zip_entry(file.data(), file.size(), name, e)) {
		return false;
	}
	net.set_plain(plain);
	std::string why;
	const bool ok = net.load(e.data, e.size, &why);
	if (!ok)
		std::fprintf(stderr, "unit_body: %s: %s\n", name.c_str(), why.c_str());
	return ok;
}

// The piece of the astronaut LiteRT had, value * scale + offset.
static void fill(nn::Net &net, const json &spec, float scale, float offset)
{
	const int fx = spec["from"][0], fy = spec["from"][1], size = spec["size"];
	float *in = net.input(0);
	for (int y = 0; y < size; y++)
		for (int x = 0; x < size; x++)
			for (int c = 0; c < 3; c++)
				in[((size_t)y * size + x) * 3 + c] = astronaut().at(fx + x, fy + y)[c] * scale + offset;
}

static void test_models(bool plain)
{
	const json &m = expected()["models"];
	const std::vector<uint8_t> task = read_file(TASK), lite = read_file(LITE);
	CHECK(!task.empty() && !lite.empty() && m.is_object());
	if (task.empty() || lite.empty() || !m.is_object())
		return;
	const char *how = plain ? " (plain code)" : "";

	// The body finder: its surest anchors.
	nn::Net finder;
	CHECK(load_net(finder, task, "pose_detector.tflite", plain));
	if (finder.loaded()) {
		fill(finder, m["finder"], 2.0f / 255, -1);
		finder.run();
		const bool boxes_first = vision::elements(finder.output_shape(0)) == 2254 * 12;
		const float *boxes = finder.output(boxes_first ? 0 : 1), *scores = finder.output(boxes_first ? 1 : 0);
		Gap g;
		for (const json &b : m["finder"]["best"]) {
			const int i = b["anchor"];
			g.take(scores[i], b["score"].get<double>());
			for (int k = 0; k < 12; k++)
				g.take(boxes[i * 12 + k], b["box"][k].get<double>());
		}
		std::printf("unit_body: body finder%s within %.2g of LiteRT\n", how, g.most);
		CHECK(g.most < 2e-3);
	}

	// The body model, all of it and only what's wanted (as body.cpp has
	// it): the same answers.
	for (int wanted = 0; wanted < 2; wanted++) {
		nn::Net body;
		if (wanted)
			body.set_wanted({0, 1, 3, 4});
		CHECK(load_net(body, task, "pose_landmarks_detector.tflite", plain));
		if (!body.loaded())
			continue;
		fill(body, m["body"], 1.0f / 255, 0);
		body.run();
		CHECK(!wanted || body.output(2) == nullptr);
		const json &b = m["body"];
		Gap points, world, heat;
		for (size_t i = 0; i < b["points"].size(); i++)
			points.take(body.output(0)[i], b["points"][i].get<double>());
		for (size_t i = 0; i < b["world"].size(); i++)
			world.take(body.output(4)[i] * 100, b["world"][i].get<double>() * 100); // in cm
		const auto &hs = body.output_shape(3);
		const int cells = hs[1] * hs[2], chans = hs[3];
		int same_place = 0;
		for (int c = 0; c < chans; c++) {
			float top = -1e9f;
			int at = 0;
			for (int k = 0; k < cells; k++)
				if (body.output(3)[(size_t)k * chans + c] > top) {
					top = body.output(3)[(size_t)k * chans + c];
					at = k;
				}
			heat.take(top, b["heat_top"][c].get<double>());
			same_place += at == b["heat_at"][c].get<int>();
		}
		std::printf("unit_body: body model%s%s: points within %.2g, metres %.2g, heat map %.2g (%d of %d peaks in the same place)\n",
			    how, wanted ? " (wanted outputs)" : "", points.most, world.most, heat.most, same_place, chans);
		CHECK(points.most < 2e-3);
		CHECK(near(body.output(1)[0], b["presence"].get<double>(), 1e-3));
		CHECK(world.most < 2e-3);
		CHECK(heat.most < 2e-3);
		CHECK(same_place >= chans - 2);
	}

	// The hand-square model.
	nn::Net recrop;
	CHECK(load_net(recrop, task, "hand_roi_refinement.tflite", plain));
	if (recrop.loaded()) {
		fill(recrop, m["recrop"], 1.0f / 255, 0);
		recrop.run();
		Gap g;
		for (int i = 0; i < 4; i++)
			g.take(recrop.output(0)[i], m["recrop"]["points"][i].get<double>());
		std::printf("unit_body: hand-square model%s within %.2g of LiteRT\n", how, g.most);
		CHECK(g.most < 2e-3);
	}

	// The hand models: all they say.
	for (const char *key : {"hand", "lite"}) {
		nn::Net hand;
		const bool full = std::string(key) == "hand";
		CHECK(full ? load_net(hand, task, "hand_landmarks_detector.tflite", plain) : load_net(hand, lite, "", plain));
		if (!hand.loaded())
			continue;
		const json &h = m[key];
		fill(hand, h, 1.0f / 255, 0);
		hand.run();
		CHECK(hand.outputs() == h["outputs"].size());
		Gap g;
		for (size_t o = 0; o < std::min(hand.outputs(), h["outputs"].size()); o++) {
			CHECK(vision::elements(hand.output_shape(o)) == h["outputs"][o].size());
			for (size_t i = 0; i < h["outputs"][o].size() && i < vision::elements(hand.output_shape(o)); i++)
				g.take(hand.output(o)[i], h["outputs"][o][i].get<double>());
		}
		std::printf("unit_body: %s model%s within %.2g of LiteRT\n", full ? "hand" : "light hand", how, g.most);
		CHECK(g.most < 2e-3);
	}
}

// --- The body tracker, against MediaPipe -----------------------------------

static bool load(BodyTracker &t, bool plain = false)
{
	t.set_plain(plain);
	std::string problem;
	const bool ok = t.load_files(TASK, LITE, &problem);
	if (!ok)
		std::fprintf(stderr, "unit_body: %s\n", problem.c_str());
	CHECK(ok && t.ready());
	t.set_smoothing(-1);
	return ok;
}

struct Off {
	int within_px = 0; // points within a pixel
	double most_px = 0, most_world = 0, most_seen = 0;
	int world_close = 0; // within a centimetre
};

static Off compare(const Body &b, const json &w)
{
	Off off;
	for (int i = 0; i < BODY_POINTS; i++) {
		const json &p = w["points"][i], &q = w["world"][i];
		const double d = std::hypot(b.points[i].x - p[0].get<double>(), b.points[i].y - p[1].get<double>());
		off.most_px = std::max(off.most_px, d);
		off.within_px += d <= 1;
		off.most_seen = std::max(off.most_seen, std::fabs(b.seen[i] - p[3].get<double>()));
		const double dw = std::sqrt(std::pow(b.world[i].x - q[0].get<double>(), 2) +
					    std::pow(b.world[i].y - q[1].get<double>(), 2) +
					    std::pow(b.world[i].z - q[2].get<double>(), 2));
		off.most_world = std::max(off.most_world, dw);
		off.world_close += dw <= 0.01;
	}
	return off;
}

static void test_same_as_mediapipe(bool plain)
{
	BodyTracker t;
	if (!load(t, plain))
		return;
	const Picture flipped = mirrored(astronaut());
	const struct {
		const char *name;
		const Picture &picture;
	} pictures[] = {{"astronaut", astronaut()}, {"mirrored", flipped}, {"camera", camera()}};
	for (const auto &pc : pictures) {
		const json &w = expected()["pictures"][pc.name];
		CHECK(pc.picture.w == w["w"].get<int>() && pc.picture.h == w["h"].get<int>());
		t.forget();
		const Held f = frame_of(pc.picture);
		const Body &b = t.look(f.frame, 0);
		CHECK(b.searched);
		CHECK(b.found == w["found"].get<bool>());
		if (!b.found || !w["found"].get<bool>())
			continue;
		const Off off = compare(b, w);
		std::printf("unit_body: %s%s: %d of 33 points within a pixel of MediaPipe's (at most %.2f), in metres "
			    "at most %.4f off, how likely each is in sight at most %.4f off\n",
			    pc.name, plain ? " (plain code)" : "", off.within_px, off.most_px, off.most_world,
			    off.most_seen);
		// (A point the model is only just sure of can land elsewhere on
		// its heat map: a few pixels on one of camera.pgm's.)
		CHECK(off.within_px >= 31);
		CHECK(off.most_px < 10);
		CHECK(off.world_close >= 31);
		CHECK(off.most_world < 0.05);
		CHECK(off.most_seen < 0.02);
		CHECK(b.w == pc.picture.w && b.h == pc.picture.h);
		// No hand where MediaPipe finds none.
		CHECK(!b.hands[0].found && w["left_hand"].empty());
		CHECK(!b.hands[1].found && w["right_hand"].empty());
	}
}

// Blue-first pixels and longer rows (as OBS can hand them over) are the
// same picture.
static void test_pixel_layouts()
{
	BodyTracker a, b;
	if (!load(a) || !load(b))
		return;
	const Held rgba = frame_of(astronaut()), bgra = frame_of(astronaut(), true, 36);
	const Body ba = a.look(rgba.frame, 0);
	const Body &bb = b.look(bgra.frame, 0);
	CHECK(ba.found && bb.found);
	bool same = true;
	for (int i = 0; i < BODY_POINTS; i++)
		same = same && ba.points[i].x == bb.points[i].x && ba.points[i].y == bb.points[i].y &&
		       ba.world[i].z == bb.world[i].z;
	CHECK(same);
}

// Following: the next look starts from where the body was (no finder),
// smoothed - as MediaPipe's VIDEO mode (its points out of the picture,
// your hands here, are guesses that move from look to look); noise has
// no body; forgotten, it searches again.
static void test_follows_and_loses()
{
	BodyTracker t;
	if (!load(t))
		return;
	t.set_smoothing(0.5f); // MediaPipe's own
	const Held f = frame_of(astronaut());
	const json &want = expected()["following"];
	const double step = want["ms"].get<double>() / 1000;
	for (size_t i = 0; i < want["looks"].size(); i++) {
		const Body &b = t.look(f.frame, (double)i * step);
		CHECK(b.found && b.searched == (i == 0));
		int within = 0;
		double most = 0;
		for (int k = 0; k < BODY_POINTS; k++) {
			const json &p = want["looks"][i][k];
			const double d = std::hypot(b.points[k].x - p[0].get<double>(), b.points[k].y - p[1].get<double>());
			within += d <= 1;
			most = std::max(most, d);
		}
		std::printf("unit_body: following, look %zu: %d of 33 points within a pixel of MediaPipe's (at most %.2f)\n",
			    i + 1, within, most);
		CHECK(within >= 27);
		CHECK(most < 4);
	}
	// Noise: no body.
	Picture noise = astronaut();
	std::mt19937 rng(7);
	for (uint8_t &v : noise.rgb)
		v = (uint8_t)(rng() & 255);
	const Held n = frame_of(noise);
	CHECK(!t.look(n.frame, 1).found);
	CHECK(!t.look(n.frame, 1.1).found);
	// Back, it's found again.
	CHECK(t.look(f.frame, 1.2).found);
	t.forget();
	CHECK(t.look(f.frame, 1.3).searched);
}

// The hand gates: hands not looked for when they're off; looked for
// only at a wrist in sight, in the picture; the hand model unsure here.
static void test_hand_gates()
{
	BodyTracker t;
	if (!load(t))
		return;
	const Held f = frame_of(camera());
	const Body &b = t.look(f.frame, 0);
	CHECK(b.found);
	for (int s = 0; s < 2; s++) {
		const int wrist = s ? BODY_RIGHT_WRIST : BODY_LEFT_WRIST;
		const bool in_sight = b.seen[wrist] > 0.3f && b.points[wrist].x >= 0 && b.points[wrist].y >= 0 &&
				      b.points[wrist].x < b.w && b.points[wrist].y < b.h;
		CHECK(!b.hands[s].found);
		CHECK(b.hands[s].gate == (in_sight ? HandGate::Unsure : HandGate::NotLooked));
		if (b.hands[s].gate == HandGate::Unsure)
			CHECK(b.hands[s].presence < 0.5f);
	}
	t.set_hands(false);
	const Body &off = t.look(f.frame, 1.0 / 15);
	CHECK(off.found && off.hands[0].gate == HandGate::NotLooked && off.hands[1].gate == HandGate::NotLooked);
	// Light mode runs the lighter hand model (or the full one without it).
	t.set_hands(true);
	t.set_light(true);
	CHECK(t.light());
	const Body &light = t.look(f.frame, 2.0 / 15);
	CHECK(light.found && !light.hands[0].found && !light.hands[1].found);
	BodyTracker no_lite;
	std::string problem;
	CHECK(no_lite.load_files(TASK, "", &problem));
	no_lite.set_light(true);
	CHECK(no_lite.look(f.frame, 0).found);
}

static void test_without_models()
{
	BodyTracker t;
	std::string problem;
	CHECK(!t.load_files(ROOT + "/no/such/holistic_landmarker.task", "", &problem));
	CHECK(problem.find("missing") != std::string::npos);
	CHECK(!t.load_files(TASK, ROOT + "/no/such/hand_landmark_lite.tflite", &problem));
	CHECK(problem.find("missing") != std::string::npos);
	// The face models' file isn't the body's.
	CHECK(!t.load_files(ROOT + "/data/face/face_landmarker.task", "", &problem));
	CHECK(problem.find("holistic_landmarker.task") != std::string::npos);
	const uint8_t junk[100] = {'P', 'K'};
	CHECK(!t.load(junk, sizeof junk, nullptr, 0, &problem));
	CHECK(!t.ready());
	const Held f = frame_of(astronaut());
	CHECK(!t.look(f.frame, 0).found);
}

static void test_speed()
{
	BodyTracker t;
	if (!load(t))
		return;
	const Held f = frame_of(camera());
	std::vector<double> ms;
	for (int i = 0; i < 9; i++) {
		const Body &b = t.look(f.frame, i / 15.0);
		if (i)
			ms.push_back(b.ms);
	}
	std::sort(ms.begin(), ms.end());
	std::printf("unit_body: a look following a body (its hands looked for): %.1f ms (median of 8)\n",
		    ms[ms.size() / 2]);
}

// --- Fingers --------------------------------------------------------------

// A right hand held up, palm to the camera, fingers straight up (metres,
// the camera's way: x right, y down, z away), its thumb out to the side;
// `bend`: each finger's middle joint bent this far (degrees) towards the
// palm; `left`: the left hand (mirrored).
static void make_hand(Point *p, float bend, bool left, V3 at = {})
{
	auto put = [&](int i, V3 v) {
		p[i] = {(left ? -v.x : v.x) + at.x, v.y + at.y, v.z + at.z};
	};
	put(HAND_WRIST, {0, 0, 0});
	// Thumb: out towards its first finger's side, then up.
	put(1, {0.025f, -0.02f, 0});
	put(2, {0.045f, -0.04f, 0});
	put(3, {0.055f, -0.065f, 0});
	put(4, {0.06f, -0.09f, 0});
	const float xs[4] = {0.02f, 0, -0.02f, -0.038f};
	const float b = bend * PI / 180;
	for (int f = 0; f < 4; f++) {
		const int root = 5 + f * 4;
		const V3 knuckle{xs[f], -0.085f, 0};
		put(root, knuckle);
		const V3 middle = knuckle + V3{0, -0.04f, 0};
		put(root + 1, middle);
		// Bent towards the palm: the palm faces the camera (-z).
		const V3 way{0, -std::cos(b), -std::sin(b)};
		put(root + 2, middle + way * 0.025f);
		put(root + 3, middle + way * 0.045f);
	}
}

static void test_finger_bends()
{
	Point hand[HAND_POINTS];
	float bend[5][3];
	for (int left = 0; left < 2; left++) {
		make_hand(hand, 0, left);
		finger_bends(hand, left, bend);
		bool straight = true;
		for (int f = 1; f < 5; f++)
			for (int j = 0; j < 3; j++)
				straight = straight && near(bend[f][j], 0, 1e-4);
		CHECK(straight);
		// Each finger's middle joint bent 60 degrees towards the palm.
		make_hand(hand, 60, left);
		finger_bends(hand, left, bend);
		for (int f = 1; f < 5; f++) {
			CHECK(near(bend[f][0], 0, 1e-4));
			CHECK(near(bend[f][1], 60 * PI / 180, 1e-3));
			CHECK(near(bend[f][2], 0, 1e-4));
		}
		// ...and back (the wrong way): less than nothing.
		make_hand(hand, -20, left);
		finger_bends(hand, left, bend);
		CHECK(near(bend[2][1], -20 * PI / 180, 1e-3));
		// A finger spread sideways isn't bent.
		make_hand(hand, 0, left);
		hand[HAND_INDEX + 1].x += left ? -0.02f : 0.02f;
		hand[HAND_INDEX + 2].x += left ? -0.03f : 0.03f;
		hand[HAND_INDEX + 3].x += left ? -0.04f : 0.04f;
		finger_bends(hand, left, bend);
		CHECK(near(bend[1][0], 0, 1e-4));
	}
	// The thumb's root bends towards the palm (in at the camera).
	make_hand(hand, 0, false);
	hand[2].z -= 0.02f;
	hand[3].z -= 0.035f;
	hand[4].z -= 0.05f;
	finger_bends(hand, false, bend);
	CHECK(bend[0][0] > 0.2f);
}

// --- The arm solver -------------------------------------------------------

// A body facing the camera (metres round its hips, the camera's way),
// shoulders, ears and hips in sight, its arms hanging down - its left
// arm on the picture's right.
static Body make_body()
{
	Body b;
	b.found = true;
	b.w = 640;
	b.h = 480;
	b.presence = 1;
	for (float &s : b.seen)
		s = 1;
	for (Point &p : b.points)
		p = {320, 240, 0};
	auto both = [&](int left, int right, V3 at) {
		b.world[left] = {at.x, at.y, at.z};
		b.world[right] = {-at.x, at.y, at.z};
	};
	b.world[BODY_NOSE] = {0, -0.6f, -0.08f};
	both(BODY_LEFT_EAR, BODY_RIGHT_EAR, {0.075f, -0.62f, 0});
	both(BODY_LEFT_SHOULDER, BODY_RIGHT_SHOULDER, {0.18f, -0.45f, 0});
	both(BODY_LEFT_ELBOW, BODY_RIGHT_ELBOW, {0.19f, -0.17f, 0});
	both(BODY_LEFT_WRIST, BODY_RIGHT_WRIST, {0.2f, 0.08f, 0});
	both(BODY_LEFT_HIP, BODY_RIGHT_HIP, {0.1f, 0, 0});
	return b;
}

// Your left arm: its elbow and wrist (from your left shoulder, metres,
// x your left, y up, z towards the camera).
static void put_left_arm(Body &b, V3 elbow, V3 wrist)
{
	const Point &s = b.world[BODY_LEFT_SHOULDER];
	b.world[BODY_LEFT_ELBOW] = {s.x + elbow.x, s.y - elbow.y, s.z - elbow.z};
	b.world[BODY_LEFT_WRIST] = {s.x + wrist.x, s.y - wrist.y, s.z - wrist.z};
}

static void look_twice(ArmSolver &s, const Body &b, float dt = 1.0f / 15)
{
	for (int i = 0; i < 2; i++) {
		s.look(b);
		s.step(dt);
	}
}

static void settle(ArmSolver &s, const Body &b, float seconds = 1)
{
	for (float t = 0; t < seconds; t += 1.0f / 15) {
		s.look(b);
		for (int k = 0; k < 4; k++)
			s.step(1.0f / 60);
	}
}

static void test_arm_directions()
{
	ArmSolver s;
	Body b = make_body();
	// Seen once: nothing moves yet (a "ghost" for one look moves nothing).
	s.look(b);
	s.step(1.0f / 60);
	CHECK(s.now().side[0].weight == 0 && s.now().side[1].weight == 0);
	CHECK(!s.seen().arm[0] && !s.seen().arm[1]);
	// Twice: it comes in over BACK seconds.
	s.look(b);
	s.step(0.1f);
	CHECK(near(s.now().side[0].weight, 0.1f / ArmSolver::BACK, 1e-4) && s.seen().arm[0] && s.seen().arm[1]);
	settle(s, b);
	CHECK(s.now().side[0].weight == 1 && s.now().side[1].weight == 1);
	// Hanging down (a little out from its sides, as yours are).
	for (int side = 0; side < 2; side++) {
		CHECK(s.now().side[side].upper.y < -0.99f);
		CHECK(s.now().side[side].fore.y < -0.99f);
	}

	// Your left arm out to your left: mirrored, its right arm out to its
	// right (-x: x is its left).
	put_left_arm(b, {0.27f, 0, 0}, {0.54f, 0, 0});
	settle(s, b);
	CHECK(near3(s.now().side[1].upper, {-1, 0, 0}, 1e-3f));
	CHECK(near3(s.now().side[1].fore, {-1, 0, 0}, 1e-3f));
	CHECK(s.now().side[0].upper.y < -0.99f);
	// Not mirrored: its left arm, out to its left.
	ArmOptions o;
	o.mirror = false;
	ArmSolver plain;
	plain.set_options(o);
	settle(plain, b);
	CHECK(near3(plain.now().side[0].upper, {1, 0, 0}, 1e-3f));
	CHECK(plain.now().side[1].upper.y < -0.99f);

	// Your forearm up from your elbow: bent 90 degrees, its hinge square
	// to both and sure.
	put_left_arm(b, {0.27f, 0, 0}, {0.27f, 0.25f, 0});
	settle(s, b);
	const ArmTarget &t = s.now().side[1];
	CHECK(near3(t.upper, {-1, 0, 0}, 1e-3f) && near3(t.fore, {0, 1, 0}, 1e-3f));
	CHECK(near3(t.hinge, normalized(cross(t.upper, t.fore)), 1e-3f) && t.hinge_sure > 0.99f);
	// Straightened: the hinge kept as it was, less sure each look.
	put_left_arm(b, {0.27f, 0, 0}, {0.54f, 0, 0});
	s.look(b);
	s.step(1.0f / 60);
	const ArmTarget &st = s.now().side[1];
	CHECK(dot(st.hinge, V3{0, 0, -1}) > 0.9f);
	settle(s, b, 3);
	CHECK(s.now().side[1].hinge_sure < 0.2f);

	// Leaning or turning in your chair doesn't swing its arms: your whole
	// body turned 30 degrees, your arms as before against your chest.
	{
		Body turned = b;
		const Quat q = axis_angle({0, 1, 0}, 30 * PI / 180);
		for (Point &p : turned.world) {
			const V3 v = rotate(q, V3{p.x, p.y, p.z});
			p = {v.x, v.y, v.z};
		}
		ArmSolver a;
		settle(a, turned);
		CHECK(near3(a.now().side[1].upper, {-1, 0, 0}, 1e-3f));
	}

	// Reaching towards the camera: twice as far with reach 2.
	put_left_arm(b, {0.2f, 0, 0.2f}, {0.4f, 0, 0.4f});
	settle(s, b);
	CHECK(near3(s.now().side[1].upper, normalized(V3{-1, 0, 1}), 1e-3f));
	o.mirror = true;
	o.reach = 2;
	s.set_options(o);
	settle(s, b);
	CHECK(near3(s.now().side[1].upper, normalized(V3{-1, 0, 2}), 1e-3f));
	o.reach = 1;

	// Arms held out from its body by 30 degrees when they hang down.
	b = make_body();
	o.spread = 30;
	ArmSolver out;
	out.set_options(o);
	settle(out, b);
	const V3 down_left = out.now().side[0].upper, down_right = out.now().side[1].upper;
	CHECK(near(std::atan2(down_left.x, -down_left.y) * 180 / PI, 30 + std::atan2(0.01, 0.28) * 180 / PI, 0.5));
	CHECK(near(std::atan2(-down_right.x, -down_right.y) * 180 / PI, 30 + std::atan2(0.01, 0.28) * 180 / PI, 0.5));
}

static void test_lost_and_found()
{
	ArmOptions o;
	o.hold = 0.5f;
	ArmSolver s;
	s.set_options(o);
	Body b = make_body();
	settle(s, b);
	CHECK(s.now().side[0].weight == 1);
	// Your left wrist out of sight: its right arm held, then resting.
	Body lost = b;
	lost.seen[BODY_LEFT_WRIST] = 0.1f;
	float t = 0;
	while (t < 0.45f) {
		s.look(lost);
		s.step(0.05f);
		t += 0.05f;
	}
	CHECK(s.now().side[1].weight == 1 && s.now().side[0].weight == 1);
	CHECK(!s.seen().arm[1] && s.seen().arm[0]);
	while (t < 0.5f + ArmSolver::FADE / 2) {
		s.look(lost);
		s.step(0.05f);
		t += 0.05f;
	}
	CHECK(s.now().side[1].weight > 0.3f && s.now().side[1].weight < 0.7f);
	while (t < 0.5f + ArmSolver::FADE + 0.1f) {
		s.look(lost);
		s.step(0.05f);
		t += 0.05f;
	}
	CHECK(s.now().side[1].weight == 0);
	// Found again: back in BACK seconds (after two looks).
	look_twice(s, b, 0.05f);
	CHECK(s.now().side[1].weight > 0);
	settle(s, b, 0.3f);
	CHECK(s.now().side[1].weight == 1);
	// No shoulders: neither arm.
	Body no_shoulder = b;
	no_shoulder.seen[BODY_RIGHT_SHOULDER] = 0;
	s.look(no_shoulder);
	CHECK(!s.seen().arm[0] && !s.seen().arm[1]);
	// No body; a broken one (not a number): nothing followed, nothing
	// broken.
	Body nan = b;
	nan.world[BODY_LEFT_ELBOW].x = std::numeric_limits<float>::quiet_NaN();
	look_twice(s, nan);
	CHECK(!s.seen().arm[1]);
	CHECK(std::isfinite(s.now().side[1].upper.x));
	s.look(Body());
	CHECK(!s.seen().arm[0]);
	// Reset: resting at once.
	s.reset();
	CHECK(s.now().side[0].weight == 0 && s.now().side[1].weight == 0);
}

static Body with_hands(float bend)
{
	Body b = make_body();
	for (int you = 0; you < 2; you++) {
		HandSeen &h = b.hands[you];
		h.found = true;
		h.gate = HandGate::Ok;
		h.presence = 1;
		const Point &w = b.world[you ? BODY_RIGHT_WRIST : BODY_LEFT_WRIST];
		// (The camera's view of your hand: your left hand is a mirrored
		// right hand.)
		make_hand(h.world, bend, you == 0, {w.x, w.y, w.z});
	}
	return b;
}

static void test_hands()
{
	ArmSolver s;
	Body b = with_hands(60);
	look_twice(s, b);
	settle(s, b);
	CHECK(s.seen().hand[0] && s.seen().hand[1]);
	for (int side = 0; side < 2; side++) {
		const ArmTarget &t = s.now().side[side];
		CHECK(t.hand == 1);
		CHECK(near(t.bend[2][1], 60 * PI / 180, 2e-3));
		CHECK(near(t.bend[2][0], 0, 2e-3));
		// Fingers up, its palm to the camera: along up; mirrored.
		CHECK(near3(t.along, {0, 1, 0}, 1e-3f));
	}
	// Mirrored: your left hand (index finger towards the picture's left,
	// your body's middle) is its right, first finger towards its left.
	CHECK(s.now().side[1].across.x > 0.99f);
	CHECK(s.now().side[0].across.x < -0.99f);
	// Beyond a joint's limit: no further.
	Body far = with_hands(150);
	settle(s, far);
	CHECK(near(s.now().side[0].bend[1][1], 110 * PI / 180, 1e-3));
	// Your hands not followed: none.
	ArmOptions o;
	o.fingers = false;
	ArmSolver arms_only;
	arms_only.set_options(o);
	settle(arms_only, b);
	CHECK(!arms_only.seen().hand[0] && arms_only.now().side[0].hand == 0 && !arms_only.now().fingers);
	CHECK(arms_only.seen().arm[0]);
	// A hand seen once moves nothing; lost, held then let go.
	ArmSolver once;
	settle(once, make_body());
	once.look(b);
	once.step(1.0f / 60);
	CHECK(once.now().side[0].hand == 0);
	once.look(b);
	once.step(0.1f);
	CHECK(once.now().side[0].hand > 0);
	settle(once, b);
	Body gone = b;
	gone.hands[0].found = false;
	gone.hands[1].found = false;
	for (int i = 0; i < 6; i++) {
		once.look(gone);
		once.step(0.05f);
	}
	CHECK(once.now().side[0].hand == 1);
	for (int i = 0; i < 20; i++) {
		once.look(gone);
		once.step(0.05f);
	}
	CHECK(once.now().side[0].hand == 0 && once.now().side[0].weight == 1);
}

// Your left hand by your left ear, open, palm to the camera.
static Body by_ear(V3 off = {})
{
	Body b = with_hands(8);
	const Point &ear = b.world[BODY_LEFT_EAR];
	const V3 wrist{ear.x + 0.06f + off.x, ear.y + 0.05f + off.y, ear.z - 0.02f + off.z};
	b.world[BODY_LEFT_ELBOW] = {0.3f, -0.3f, -0.05f};
	b.world[BODY_LEFT_WRIST] = {wrist.x, wrist.y, wrist.z};
	make_hand(b.hands[0].world, 8, true, wrist);
	b.hands[1].found = false;
	return b;
}

static void test_by_head_and_calibration()
{
	ArmSolver s;
	const Body b = by_ear();
	settle(s, b);
	const ArmTarget &t = s.now().side[1];
	CHECK(t.near > 0.99f);
	// Its place by its head: from the middle of your ears, in ear-to-ear
	// widths (0.15 m here), mirrored.
	const V3 want{-(0.075f + 0.06f) / 0.15f, -0.05f / 0.15f, 0.02f / 0.15f};
	CHECK(near3(t.by_head, want, 1e-3f));
	// Far from your head: not near at all.
	settle(s, make_body());
	CHECK(s.now().side[1].near < 0.01f);

	// Calibrate: twelve looks by your ear, then it knows your open hand
	// (a little bent here: open is straight now) and where your hand goes.
	CHECK(!s.calibrating() && !s.calibrated() && s.calibration_result().empty());
	CHECK(s.save_calibration().empty());
	s.start_calibration();
	CHECK(s.calibrating());
	for (int i = 0; i < ArmSolver::CALIBRATE_LOOKS + 2 && s.calibrating(); i++) {
		s.look(b);
		s.step(1.0f / 15);
	}
	CHECK(!s.calibrating() && s.calibrated() && s.calibration_result() == "ok");
	settle(s, b);
	CHECK(near(s.now().side[1].bend[2][1], 0, 2e-3));
	CHECK(near3(s.now().side[1].by_head, {-ArmSolver::BY_EAR_OUT, -ArmSolver::BY_EAR_DOWN, 0}, 1e-3f));
	// Kept as text, and read back the same.
	const std::string text = s.save_calibration();
	CHECK(!text.empty());
	ArmSolver again;
	CHECK(again.load_calibration(text) && again.calibrated() && again.save_calibration() == text);
	settle(again, b);
	CHECK(near3(again.now().side[1].by_head, s.now().side[1].by_head, 1e-3f));
	// Its other hand is moved the same way (out from its head).
	Body right = b;
	for (int i = 0; i < BODY_POINTS; i++)
		right.world[i].x = -b.world[i].x;
	std::swap(right.world[BODY_LEFT_WRIST], right.world[BODY_RIGHT_WRIST]);
	std::swap(right.world[BODY_LEFT_ELBOW], right.world[BODY_RIGHT_ELBOW]);
	std::swap(right.world[BODY_LEFT_SHOULDER], right.world[BODY_RIGHT_SHOULDER]);
	std::swap(right.world[BODY_LEFT_EAR], right.world[BODY_RIGHT_EAR]);
	std::swap(right.world[BODY_LEFT_HIP], right.world[BODY_RIGHT_HIP]);
	settle(again, right);
	CHECK(near3(again.now().side[0].by_head, {ArmSolver::BY_EAR_OUT, -ArmSolver::BY_EAR_DOWN, 0}, 1e-3f));
	// Bad text: not taken, the calibration it had kept.
	for (const char *bad : {"x", "{}", "{\"v\":2,\"by_ear\":[0,0,0],\"open\":[]}",
				"{\"v\":1,\"by_ear\":[9,0,0],\"open\":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]}",
				"{\"v\":1,\"by_ear\":[0,0,0],\"open\":[0,0,0,0,0,0,0,0,0,0,0,0,0,0]}"})
		CHECK(!again.load_calibration(bad) && again.calibrated());
	// "" forgets it.
	CHECK(again.load_calibration("") && !again.calibrated());
	// No hand seen: it gives up after CALIBRATE_TIME seconds.
	ArmSolver none;
	none.start_calibration();
	for (float time = 0; time < ArmSolver::CALIBRATE_TIME + 0.2f; time += 0.1f) {
		none.look(make_body());
		none.step(0.1f);
	}
	CHECK(!none.calibrating() && !none.calibrated() && none.calibration_result() == "no hand");
	// Stopped: nothing kept.
	ArmSolver stopped;
	stopped.start_calibration();
	stopped.look(b);
	stopped.stop_calibration();
	CHECK(!stopped.calibrating() && !stopped.calibrated());
}

int main()
{
	test_models(false);
	test_models(true);
	test_same_as_mediapipe(false);
	test_same_as_mediapipe(true);
	test_pixel_layouts();
	test_follows_and_loses();
	test_hand_gates();
	test_without_models();
	test_finger_bends();
	test_arm_directions();
	test_lost_and_found();
	test_hands();
	test_by_head_and_calibration();
	test_speed();
	std::printf("unit_body: %d passed, %d failed\n", passes, failures);
	return failures ? 1 : 0;
}
