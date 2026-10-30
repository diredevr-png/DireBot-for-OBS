// Unit tests for clean recordings (src/record/rules.hpp) and DireBot
// Move's maths (src/moves/plan.hpp).
// Build and run: tools/test-link.sh (Linux, any C++17 compiler).

#include "../src/moves/plan.hpp"
#include "../src/record/rules.hpp"

#include <cmath>
#include <cstdio>

using namespace direbot;

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

static void options_round_trip_and_clean()
{
	record::json j = {{"when", "stream"},
			  {"encoder", "jim_nvenc"},
			  {"picks",
			   {{{"source", "Camera"}, {"height", 720}, {"quality", "medium"}, {"audio_track", 2}},
			    {{"source", "Camera"}},
			    {{"source", ""}},
			    {{"source", "Game"}, {"height", 50}, {"quality", "silly"}, {"audio_track", 9}, {"enabled", false}},
			    {{"source", "C"}},
			    {{"source", "D"}},
			    {{"source", "E"}}}}};
	record::Options o = record::Options::from_json(j);
	CHECK(o.when == record::When::Stream);
	CHECK(o.encoder == "jim_nvenc");
	CHECK(o.picks.size() == 4); // twice and empty dropped, at most 4
	CHECK(o.picks[0].height == 720 && o.picks[0].quality == "medium" && o.picks[0].audio_track == 2);
	CHECK(o.picks[1].source == "Game" && o.picks[1].height == 144 && o.picks[1].quality == "high");
	CHECK(o.picks[1].audio_track == 6 && !o.picks[1].enabled);
	record::Options back = record::Options::from_json(o.to_json());
	CHECK(back.to_json() == o.to_json());
	CHECK(record::Options::from_json("nonsense").picks.empty());
	CHECK(record::when_from("hand") == record::When::Hand);
	CHECK(record::when_from("?") == record::When::Recording);
}

static void sizes_names_encoders()
{
	int w = 0, h = 0;
	record::fit(1920, 1080, 0, w, h);
	CHECK(w == 1920 && h == 1080);
	record::fit(1920, 1080, 720, w, h);
	CHECK(w == 1280 && h == 720);
	record::fit(1280, 720, 1080, w, h); // never bigger
	CHECK(w == 1280 && h == 720);
	record::fit(1081, 607, 0, w, h); // even
	CHECK(w == 1080 && h == 606);
	CHECK(record::kbps_for("high", 1920, 1080, 60) > record::kbps_for("medium", 1920, 1080, 60));
	CHECK(record::kbps_for("small", 2, 2, 30) == 1000);
	CHECK(record::kbps_for("high", 7680, 4320, 120) == 80000);
	CHECK(record::file_suffix("Cam: \"Front\"/1") == "Cam   Front  1");
	CHECK(record::file_suffix("  ..  ") == "Clean");
	CHECK(record::file_suffix("Game capture.") == "Game capture");
	CHECK(record::pick_encoder("auto", {"obs_x264", "jim_nvenc"}) == "jim_nvenc");
	CHECK(record::pick_encoder("obs_x264", {"obs_x264", "jim_nvenc"}) == "obs_x264");
	CHECK(record::pick_encoder("gone", {"obs_x264"}) == "obs_x264");
	CHECK(record::pick_encoder("auto", {}) == "obs_x264");
}

static void poses_round_trip()
{
	// 100x50 source, cropped 10 off the left, scaled 2x and turned 90°.
	moves::Pose p;
	p.cx = 500;
	p.cy = 300;
	p.rot = 1.5707963f;
	p.w = 180;
	p.h = 100;
	p.crop_l = 10;
	float ax, ay, bx, by, tx, ty;
	moves::matrix_of(p, 100, 50, ax, ay, bx, by, tx, ty);
	moves::Drawn d{ax, ay, bx, by, tx, ty, 100, 50, 10, 0, 0, 0};
	moves::Pose q = moves::pose_of(d);
	CHECK(near(q.cx, 500, 0.1f) && near(q.cy, 300, 0.1f));
	CHECK(near(q.rot, 1.5707963f) && near(q.w, 180, 0.1f) && near(q.h, 100, 0.1f));
	// A flipped one keeps its flip.
	moves::Drawn flipped{1, 0, 0, -1, 0, 50, 100, 50, 0, 0, 0, 0};
	CHECK(moves::pose_of(flipped).h < 0);
}

static void matching_and_between()
{
	moves::Pose a, b;
	a.cx = 100;
	a.w = a.h = 100;
	b.cx = 900;
	b.w = b.h = 200;
	std::vector<moves::Item> before = {{1, a, 100, 100}, {2, a, 100, 100}};
	std::vector<moves::Item> after = {{3, b, 100, 100}, {1, b, 100, 100}};
	auto steps = moves::plan(before, after, "fade", 1920, 1080);
	CHECK(steps.size() == 3);
	CHECK(steps[0].kind == moves::Kind::Go && steps[0].key == 2 && steps[0].to.alpha == 0);
	CHECK(steps[1].kind == moves::Kind::Come && steps[1].key == 3 && steps[1].from.alpha == 0);
	CHECK(steps[2].kind == moves::Kind::Move && steps[2].key == 1);
	moves::Pose mid = moves::between(steps[2].from, steps[2].to, 0.5f);
	CHECK(near(mid.cx, 500) && near(mid.w, 150));
	// Sliding in comes from the nearest edge, fully there.
	auto slide = moves::plan({}, {{7, b, 100, 100}}, "slide", 1920, 1080);
	CHECK(slide[0].from.alpha == 1 && slide[0].from.cy < 0);
	auto grow = moves::plan({}, {{7, b, 100, 100}}, "grow", 1920, 1080);
	CHECK(grow[0].from.w == 0);
	// Turning goes the short way round.
	moves::Pose r1, r2;
	r1.rot = 3.0f;
	r2.rot = -3.0f;
	CHECK(std::fabs(moves::between(r1, r2, 0.5f).rot) > 3.0f);
	for (const char *e : {"linear", "smooth", "ease_in", "ease_out", "bounce"}) {
		CHECK(near(moves::ease(e, 0), 0) && near(moves::ease(e, 1), 1));
	}
	CHECK(moves::ease("bounce", 0.8f) > 1.0f);
	CHECK(near(moves::ease("smooth", 0.5f), 0.5f));
}

int main()
{
	options_round_trip_and_clean();
	sizes_names_encoders();
	poses_round_trip();
	matching_and_between();
	std::printf("unit_record: %d passed, %d failed\n", passes, failures);
	return failures ? 1 : 0;
}
