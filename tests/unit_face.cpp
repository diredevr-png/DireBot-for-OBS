// Unit tests for the face avatar's tracker (src/face/tracker.hpp): set-up,
// following made-up faces as they move, grow and tilt, holding on with a
// part hidden, never finding a face that isn't there, finding it again,
// and the smoothing and fade.
// Build and run: tools/test-link.sh (Linux, any C++17 compiler).

#include "../src/face/tracker.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

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

static const int W = 400, H = 225;

// Smooth made-up texture: value noise.
static float hash(int x, int y, int seed)
{
	uint32_t n = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)seed * 2246822519u;
	n = (n ^ (n >> 13)) * 1274126177u;
	return ((n ^ (n >> 16)) & 1023) / 1023.0f;
}

static float noise(float x, float y, float cell, int seed)
{
	x /= cell;
	y /= cell;
	const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
	const float fx = x - x0, fy = y - y0;
	const float a = hash(x0, y0, seed), b = hash(x0 + 1, y0, seed);
	const float c = hash(x0, y0 + 1, seed), d = hash(x0 + 1, y0 + 1, seed);
	return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

// Where the parts are on the made-up face, from its middle (face pixels).
static const float PX[3] = {-30, 30, 0}, PY[3] = {-12, -12, 33};

// The face's look at a point of the face (face pixels from its middle).
// `turn`: squashed across, as a head turned.
static float face_at(float fx, float fy)
{
	// The two eyes look much alike (as real ones do): a dark oval with a
	// light spot, on the same pattern - plus a little of their own.
	for (int i = 0; i < 2; i++) {
		const float ex = (fx - PX[i]) / 11, ey = (fy - PY[i]) / 6;
		if (ex * ex + ey * ey < 1) {
			const float spot = std::hypot(ex - 0.25f, ey + 0.2f) < 0.3f ? 120 : 0;
			return 50 + spot + 40 * noise(fx - PX[i] + 40, fy - PY[i] + 40, 3, 7) +
			       10 * noise(fx, fy, 2, 20 + i);
		}
	}
	const float mx = (fx - PX[2]) / 18, my = (fy - PY[2]) / 5;
	if (mx * mx + my * my < 1)
		return 70 + 60 * std::fabs(my) + 30 * noise(fx, fy, 3, 9);
	// Brows, nose: some shape for the skin.
	if (std::fabs(fy + 24) < 2.5f && std::fabs(std::fabs(fx) - 30) < 12)
		return 90;
	if (std::fabs(fx) < 4 && fy > -5 && fy < 18)
		return 150 + 30 * noise(fx, fy, 4, 11);
	return 170 + 40 * noise(fx, fy, 9, 3);
}

struct Pose {
	float cx = 200, cy = 110, s = 1, a = 0;
	float squash = 1; // across, for a turned head
	float shift = 0; // the mouth and nose moved across (face pixels)
	bool face = true;
	bool hide_mouth = false;
};

static std::vector<uint8_t> picture(const Pose &p, int bg_seed = 1)
{
	std::vector<uint8_t> img((size_t)W * H);
	const float c = std::cos(p.a), s = std::sin(p.a);
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			const float dx = x - p.cx, dy = y - p.cy;
			float fx = (c * dx + s * dy) / p.s, fy = (-s * dx + c * dy) / p.s;
			fx /= p.squash;
			float v;
			const float r = (fx / 62) * (fx / 62) + (fy / 75) * (fy / 75);
			if (p.face && r < 1) {
				float lx = fx;
				// The middle of the face (below the eyes) moves across.
				if (fy > 0)
					lx -= p.shift;
				v = face_at(lx, fy);
				if (p.hide_mouth && fy > 22)
					v = 30; // a hand or the mic arm
			} else {
				v = 60 + 120 * noise((float)x, (float)y, 14, bg_seed) + 40 * noise((float)x, (float)y, 4, bg_seed + 5);
			}
			img[(size_t)y * W + x] = (uint8_t)std::fmax(0, std::fmin(255, v));
		}
	return img;
}

static void truth(const Pose &p, int i, float &x, float &y)
{
	const float c = std::cos(p.a), s = std::sin(p.a);
	const float fx = PX[i] * p.squash, fy = PY[i];
	x = p.cx + p.s * (c * fx - s * fy);
	y = p.cy + p.s * (s * fx + c * fy);
}

static void boxes_for(const Pose &p, Box out[3])
{
	const float bw[3] = {26, 26, 40}, bh[3] = {16, 16, 14};
	for (int i = 0; i < 3; i++) {
		float x, y;
		truth(p, i, x, y);
		out[i] = {(x - bw[i] / 2) / W, (y - bh[i] / 2) / H, bw[i] / W, bh[i] / H};
	}
}

// Every part within `by` pixels of where it really is.
static bool on_target(const Face &f, const Pose &p, float by = 2.5f)
{
	if (!f.found)
		return false;
	for (int i = 0; i < 3; i++) {
		float x, y;
		truth(p, i, x, y);
		if (std::hypot(f.x[i] * W - x, f.y[i] * H - y) > by)
			return false;
	}
	return true;
}

static Tracker set_up_one(const Pose &p = Pose())
{
	Tracker t;
	Box b[3];
	boxes_for(p, b);
	const std::vector<uint8_t> img = picture(p);
	std::string problem;
	CHECK(t.set_up(img.data(), W, H, b, &problem));
	if (!problem.empty())
		std::fprintf(stderr, "  set_up: %s\n", problem.c_str());
	return t;
}

static void test_set_up()
{
	const Pose p;
	const std::vector<uint8_t> img = picture(p);
	Box b[3];
	boxes_for(p, b);
	Tracker t;
	std::string problem;
	CHECK(!t.ready());
	// Too small a box.
	Box tiny[3] = {b[0], b[1], b[2]};
	tiny[0].w = 2.0f / W;
	CHECK(!t.set_up(img.data(), W, H, tiny, &problem) && !problem.empty() && !t.ready());
	// A plain picture (the camera off): nothing to find again.
	std::vector<uint8_t> flat((size_t)W * H, 40);
	problem.clear();
	CHECK(!t.set_up(flat.data(), W, H, b, &problem) && !problem.empty());
	// Both eye boxes on the same eye.
	Box same[3] = {b[0], b[0], b[2]};
	CHECK(!t.set_up(img.data(), W, H, same, &problem));
	// The mouth box up with the eyes.
	Box up[3] = {b[0], b[1], b[2]};
	up[2].y = b[0].y;
	up[2].x = (b[0].x + b[1].x) / 2;
	CHECK(!t.set_up(img.data(), W, H, up, &problem));
	CHECK(t.set_up(img.data(), W, H, b, &problem) && t.ready());
	// Found straight away where it was set up.
	const Face &f = t.update(img.data(), W, H);
	CHECK(on_target(f, p, 1.5f));
	CHECK(f.confidence > 0.7f);
	CHECK(std::fabs(f.scale - 1) < 0.03f && std::fabs(f.angle) < 0.03f);
}

static void test_follows()
{
	Tracker t = set_up_one();
	Pose p;
	int on = 0, frames = 0;
	// Moves across, then comes closer, then tilts, then leans back.
	for (int i = 0; i < 20; i++, frames++) {
		p.cx += 3;
		p.cy += 1;
		on += on_target(t.update(picture(p).data(), W, H), p);
	}
	for (int i = 0; i < 20; i++, frames++) {
		p.s *= 1.015f;
		on += on_target(t.update(picture(p).data(), W, H), p);
	}
	for (int i = 0; i < 20; i++, frames++) {
		p.a += 0.018f;
		on += on_target(t.update(picture(p).data(), W, H), p);
	}
	for (int i = 0; i < 20; i++, frames++) {
		p.s /= 1.025f;
		p.a -= 0.025f;
		p.cx -= 4;
		on += on_target(t.update(picture(p).data(), W, H), p);
	}
	std::fprintf(stderr, "  followed %d of %d\n", on, frames);
	CHECK(on == frames);
	const Face &f = t.face();
	CHECK(std::fabs(f.scale - p.s) < 0.05f);
	CHECK(std::fabs(f.angle - p.a) < 0.05f);
	float mx = 0, my = 0;
	for (int i = 0; i < 3; i++) {
		float x, y;
		truth(p, i, x, y);
		mx += x / 3;
		my += y / 3;
	}
	CHECK(std::fabs(f.cx * W - mx) < 2 && std::fabs(f.cy * H - my) < 2);
}

static void test_part_hidden()
{
	Tracker t = set_up_one();
	Pose p;
	t.update(picture(p).data(), W, H);
	p.hide_mouth = true;
	int held = 0;
	float conf = 1;
	for (int i = 0; i < 10; i++) {
		p.cx += 1;
		const Face &f = t.update(picture(p).data(), W, H);
		// Still where the eyes say it is; the mouth worked out from them.
		held += on_target(f, p, 3.0f);
		conf = std::fmin(conf, f.found ? f.confidence : 0);
	}
	CHECK(held >= 8);
	CHECK(conf < 0.7f);
}

static void test_gone_and_back()
{
	Tracker t = set_up_one();
	Pose p;
	t.update(picture(p).data(), W, H);
	// Gone (walked off): never a face somewhere else.
	Pose gone = p;
	gone.face = false;
	int wrong = 0;
	for (int i = 0; i < 40; i++)
		wrong += t.update(picture(gone).data(), W, H).found;
	CHECK(wrong == 0);
	CHECK(t.misses() >= 40);
	// Back, somewhere else and a bit closer: found again.
	Pose back;
	back.cx = 110;
	back.cy = 120;
	back.s = 1.2f;
	int looks = 0;
	bool found = false;
	for (; looks < 30 && !found; looks++)
		found = t.update(picture(back).data(), W, H).found;
	std::fprintf(stderr, "  found again after %d looks\n", looks);
	CHECK(found);
	CHECK(on_target(t.face(), back, 3.0f));
}

static void test_nothing_there()
{
	Tracker t = set_up_one();
	// Other pictures with no face in: never found.
	Pose none;
	none.face = false;
	int wrong = 0;
	Smoother sm;
	int shown = 0;
	for (int seed = 2; seed < 60; seed++) {
		const Face &f = t.update(picture(none, seed).data(), W, H);
		wrong += f.found && f.confidence >= SURE;
		shown += sm.step(f, 0.033f, 1.0f).shown > 0;
	}
	std::fprintf(stderr, "  sure on nothing: %d of 58, shown %d\n", wrong, shown);
	CHECK(wrong <= 1);
	CHECK(shown == 0);
}

static void test_save_load()
{
	Tracker t = set_up_one();
	const std::string text = t.save();
	CHECK(!text.empty());
	Tracker again;
	CHECK(again.load(text) && again.ready());
	Pose p;
	p.cx = 150;
	bool found = false;
	for (int i = 0; i < 20 && !found; i++)
		found = on_target(again.update(picture(p).data(), W, H), p);
	CHECK(found);
	Tracker bad;
	CHECK(!bad.load("") && !bad.load("{}") && !bad.load("{\"v\":1,\"w\":400,\"h\":225,\"parts\":[1,2,3]}"));
	CHECK(!bad.ready());
	std::vector<uint8_t> bytes = {0, 1, 2, 250, 255, 7, 9};
	std::vector<uint8_t> back;
	CHECK(decode(encode(bytes), back) && back == bytes);
	CHECK(!decode("ab$c", back));
}

static void test_turned()
{
	Tracker t = set_up_one();
	Pose p;
	t.update(picture(p).data(), W, H);
	// Turned: eyes closer together, the middle of the face across.
	int turned = 0;
	for (int i = 1; i <= 12; i++) {
		p.squash = 1 - 0.025f * std::min(i, 8);
		p.shift = 1.0f * std::min(i, 8);
		turned = t.update(picture(p).data(), W, H).turned;
	}
	CHECK(t.face().found);
	CHECK(turned == 1);
	// Round to the other side, through facing.
	for (int i = 0; i < 16; i++) {
		p.shift = std::fmax(-8, p.shift - 1.0f);
		p.squash = 1 - 0.025f * std::fabs(p.shift);
		turned = t.update(picture(p).data(), W, H).turned;
	}
	CHECK(t.face().found);
	CHECK(turned == -1);
	p.squash = 1;
	p.shift = 0;
	for (int i = 0; i < 6; i++)
		turned = t.update(picture(p).data(), W, H).turned;
	CHECK(turned == 0);
}

static void test_smoother()
{
	Smoother sm;
	Face f;
	f.found = true;
	f.confidence = 1;
	f.cx = 0.6f;
	f.cy = 0.4f;
	for (int i = 0; i < 3; i++) {
		f.x[i] = 0.5f;
		f.y[i] = 0.5f;
	}
	Smoother::Out o = sm.step(f, 0.033f, 1.0f);
	CHECK(o.cx == 0.6f); // starts where it is
	CHECK(o.shown == 0); // not on the first sure look
	o = sm.step(f, 0.033f, 1.0f, false);
	CHECK(o.shown == 0); // the same look again doesn't count
	for (int i = 0; i < 10; i++)
		o = sm.step(f, 0.033f, 1.0f);
	CHECK(o.shown == 1);
	// Moves: the cut-outs keep up, the pose eases.
	f.cx = 0.7f;
	for (int i = 0; i < 3; i++)
		f.x[i] = 0.6f;
	o = sm.step(f, 0.033f, 1.0f);
	CHECK(o.cx > 0.6f && o.cx < 0.65f);
	for (int i = 0; i < 5; i++)
		o = sm.step(f, 0.033f, 1.0f);
	CHECK(std::fabs(o.x[0] - 0.6f) < 0.005f);
	// Lost: held for a moment, then faded out, and stays where it was.
	Face lost;
	o = sm.step(lost, 0.2f, 1.0f);
	CHECK(o.shown == 1);
	o = sm.step(lost, 0.15f, 1.0f);
	CHECK(o.shown == 1);
	for (int i = 0; i < 6; i++)
		o = sm.step(lost, 0.1f, 1.0f);
	CHECK(o.shown == 0);
	CHECK(std::fabs(o.x[0] - 0.6f) < 0.005f);
	// Unsure counts as lost.
	Smoother sm2;
	Face meh = f;
	meh.confidence = 0.1f;
	for (int i = 0; i < 30; i++)
		o = sm2.step(meh, 0.033f, 1.0f);
	CHECK(o.shown == 0);
}

int main()
{
	test_set_up();
	test_follows();
	test_part_hidden();
	test_gone_and_back();
	test_nothing_there();
	test_save_load();
	test_turned();
	test_smoother();
	std::printf("unit_face: %d passed, %d failed\n", passes, failures);
	return failures ? 1 : 0;
}
