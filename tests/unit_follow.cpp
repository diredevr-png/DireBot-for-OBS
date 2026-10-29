// Unit tests for "Follow me" (src/follow/tracker.hpp): the crop, finding
// you by movement in made-up camera pictures, and the smooth pan.
// Build and run: tools/test-link.sh (Linux, any C++17 compiler).

#include "../src/follow/tracker.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace direbot::follow;

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

static bool near(float a, float b, float by = 0.01f)
{
	return std::fabs(a - b) <= by;
}

static void test_crop()
{
	Spot mid;
	mid.x = 0.5f;
	mid.y = 0.3f;
	// A wide camera into the tall picture: full height, a 9:16 slice.
	Rect r = crop(1920, 1080, 1080, 1920, 1.0f, mid);
	CHECK(near(r.h, 1080) && near(r.w, 607.5f) && near(r.x, 656.25f) && near(r.y, 0));
	Spot left = mid, right = mid;
	left.x = 0.0f;
	right.x = 1.0f;
	CHECK(near(crop(1920, 1080, 1080, 1920, 1.0f, left).x, 0));
	CHECK(near(crop(1920, 1080, 1080, 1920, 1.0f, right).x, 1920 - 607.5f));
	// Twice as close: half the size, your head 30% down.
	r = crop(1920, 1080, 1080, 1920, 2.0f, mid);
	CHECK(near(r.w, 303.75f) && near(r.h, 540) && near(r.y, 0.3f * 1080 - 0.3f * 540));
	// The camera's box in "camera on top, game below" (1080 x 768).
	r = crop(1920, 1080, 1080, 768, 1.0f, mid);
	CHECK(near(r.h, 1080) && near(r.w, 1080 * 1080 / 768.0f, 0.1f));
	// A box wider than the camera: full width.
	r = crop(1280, 720, 1920, 720, 1.0f, mid);
	CHECK(near(r.w, 1280) && near(r.h, 480) && near(r.x, 0));
	// Nonsense in, nothing out; zoom kept between 1 and 4.
	r = crop(0, 1080, 1080, 1920, 1.0f, mid);
	CHECK(r.w == 0 && r.h == 0);
	CHECK(near(crop(1920, 1080, 1080, 1920, NAN, mid).w, 607.5f));
	CHECK(near(crop(1920, 1080, 1080, 1920, 0.2f, mid).w, 607.5f));
	CHECK(near(crop(1920, 1080, 1080, 1920, 99.0f, mid).w, 607.5f / 4));
}

// A made-up camera: a dim room with a little grain, and people (bright
// boxes) that move a couple of pixels each look, like someone talking.
struct Camera {
	int w = 128, h = 72;
	unsigned seed = 12345;
	int grain = 4;
	struct Person {
		int x, y, w, h, wiggle;
	};
	std::vector<Person> people;
	int look = 0;
	int base = 40;

	int noise()
	{
		seed = seed * 1103515245u + 12345u;
		return grain ? (int)((seed >> 16) % (unsigned)(2 * grain + 1)) - grain : 0;
	}

	std::vector<uint8_t> next()
	{
		std::vector<uint8_t> f((size_t)w * h);
		for (auto &p : f)
			p = (uint8_t)(base + noise());
		for (const Person &p : people) {
			const int dx = (look % 2) ? p.wiggle : 0;
			for (int y = p.y; y < p.y + p.h && y < h; y++)
				for (int x = p.x + dx; x < p.x + dx + p.w && x < w; x++)
					if (x >= 0 && y >= 0)
						f[(size_t)y * w + x] = 200;
		}
		look++;
		return f;
	}
};

static Spot feed(Tracker &t, Camera &c, int looks, float window = 0.32f)
{
	Spot s;
	for (int i = 0; i < looks; i++) {
		auto f = c.next();
		s = t.update(f.data(), c.w, c.h, window);
	}
	return s;
}

static void test_finds_you()
{
	Tracker t;
	Camera c;
	c.people.push_back({88, 20, 16, 40, 3}); // right of the middle
	Spot s = feed(t, c, 1);
	CHECK(!s.seen); // the first look has nothing to compare with
	// Outside the crop at first: after half a second it goes there.
	s = feed(t, c, 12);
	CHECK(s.seen);
	CHECK(near(s.x, (88 + 8 + 1.5f) / 128.0f, 0.04f));
	// Your head: near the top of the movement.
	CHECK(s.y > 20 / 72.0f && s.y < 34 / 72.0f);
}

static void test_holds_when_still()
{
	Tracker t;
	Camera c;
	c.people.push_back({30, 20, 16, 40, 3});
	Spot s = feed(t, c, 15);
	CHECK(s.seen && near(s.x, 0.3f, 0.05f));
	// Sitting still: nothing moves, and it stays where it was.
	c.people[0].wiggle = 0;
	s = feed(t, c, 30);
	CHECK(!s.seen && near(s.x, 0.3f, 0.05f));
}

static void test_a_pet_behind_you_doesnt_pull_it_away()
{
	Tracker t;
	Camera c;
	c.people.push_back({56, 18, 16, 44, 3});
	Spot s = feed(t, c, 15);
	CHECK(s.seen && near(s.x, 0.5f, 0.05f));
	// Something smaller moving at the edge, while you talk.
	c.people.push_back({116, 50, 8, 8, 2});
	s = feed(t, c, 40);
	CHECK(near(s.x, 0.5f, 0.06f));
}

static void test_you_walk_across()
{
	Tracker t;
	Camera c;
	c.people.push_back({20, 18, 16, 44, 3});
	feed(t, c, 12);
	// Walking right, a few pixels a look: it follows.
	for (int x = 20; x <= 100; x += 4) {
		c.people[0].x = x;
		feed(t, c, 1);
	}
	Spot s = feed(t, c, 10);
	CHECK(s.seen && near(s.x, (100 + 8) / 128.0f, 0.06f));
}

static void test_you_walk_over_and_stop()
{
	Tracker t;
	Camera c;
	c.people.push_back({20, 18, 12, 30, 0});
	feed(t, c, 3);
	// Walking right and then standing dead still: it ends on you, not on
	// the trail you left.
	for (int x = 20; x <= 100; x += 4) {
		c.people[0].x = x;
		feed(t, c, 1);
	}
	Spot s = feed(t, c, 20);
	CHECK(near(s.x, (100 + 6) / 128.0f, 0.03f));
}

static void test_the_lights_change()
{
	Tracker t;
	Camera c;
	c.people.push_back({30, 20, 16, 40, 3});
	Spot before = feed(t, c, 15);
	// Everything brighter at once: not you moving.
	c.base = 150;
	c.people.clear();
	Spot s = feed(t, c, 1);
	CHECK(!s.seen && near(s.x, before.x));
}

static void test_grain_alone_says_nothing()
{
	Tracker t;
	Camera c;
	c.grain = 9; // a dark room, a cheap camera
	Spot s = feed(t, c, 60);
	CHECK(!s.seen && near(s.x, 0.5f));
	// Nonsense pictures change nothing.
	CHECK(!t.update(nullptr, 128, 72, 0.3f).seen);
	uint8_t tiny[4] = {0, 0, 0, 0};
	CHECK(near(t.update(tiny, 2, 2, 0.3f).x, 0.5f));
}

static void test_smoother()
{
	Smoother m;
	Spot a;
	a.x = 0.5f;
	a.y = 0.3f;
	Spot s = m.step(a, 0.1f, 1.0f);
	CHECK(near(s.x, 0.5f)); // the first time: straight there
	// Leaning a little: it doesn't move.
	Spot lean = a;
	lean.x = 0.53f;
	for (int i = 0; i < 20; i++)
		s = m.step(lean, 0.1f, 1.0f);
	CHECK(near(s.x, 0.5f));
	// Across the room: it pans there, never faster than half the
	// picture a second.
	Spot far = a;
	far.x = 0.9f;
	float was = s.x;
	s = m.step(far, 0.1f, 1.0f);
	CHECK(s.x > was && s.x - was <= 0.05f + 1e-6f);
	for (int i = 0; i < 40; i++)
		s = m.step(far, 0.1f, 1.0f);
	CHECK(near(s.x, 0.9f, 0.02f));
	// A long gap (OBS was busy) is taken as half a second at most.
	m.reset();
	m.step(a, 0.1f, 1.0f);
	s = m.step(far, 30.0f, 1.0f);
	CHECK(s.x - 0.5f <= 0.25f + 1e-6f);
	// No time passing: no move.
	Spot same = m.now();
	CHECK(near(m.step(far, 0.0f, 1.0f).x, same.x));
}

int main()
{
	test_crop();
	test_finds_you();
	test_holds_when_still();
	test_a_pet_behind_you_doesnt_pull_it_away();
	test_you_walk_across();
	test_you_walk_over_and_stop();
	test_the_lights_change();
	test_grain_alone_says_nothing();
	test_smoother();
	std::printf("follow: %d passed, %d failed\n", passes, failures);
	return failures ? 1 : 0;
}
