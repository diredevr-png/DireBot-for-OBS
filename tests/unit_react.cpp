// Unit tests for what the face avatar does on cue (src/face/react.cpp):
// faces for a while, moves, things thrown at it and headpats - eased in
// and out, never jumping, settling back, and safe with odd numbers.
// Build and run: tools/test-link.sh (Linux, any C++17 compiler).

#include "../src/face/react.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>
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

static const float FRAME = 1.0f / 60;

static float face_amount(const ReactOut &o, const std::string &name)
{
	float a = 0;
	for (const FaceNow &f : o.faces)
		if (f.name == name)
			a = std::max(a, f.amount);
	return a;
}

static bool finite_out(const ReactOut &o)
{
	auto ok = [](float v) { return std::isfinite(v); };
	bool good = ok(o.squint) && ok(o.yaw) && ok(o.pitch) && ok(o.roll) && ok(o.dx) && ok(o.dy) &&
		    ok(o.knock.x) && ok(o.knock.y) && ok(o.knock.z) && ok(o.hand.x) && ok(o.hand.y) &&
		    ok(o.hand.size) && ok(o.hand.angle) && ok(o.hand.opacity);
	for (const FaceNow &f : o.faces)
		good = good && ok(f.amount) && f.amount >= 0 && f.amount <= 1;
	for (const BodyMove &m : o.body)
		good = good && ok(m.time) && ok(m.amount) && m.amount >= 0 && m.amount <= 1;
	for (const Flying &f : o.flying)
		good = good && ok(f.x) && ok(f.y) && ok(f.size) && ok(f.angle) && ok(f.opacity) && f.size > 0 &&
		       f.opacity >= 0 && f.opacity <= 1 && ok(f.squash_x) && ok(f.squash_y);
	for (const Burst &b : o.bursts)
		good = good && ok(b.x) && ok(b.y) && ok(b.size) && ok(b.opacity);
	return good;
}

// Runs `seconds` at 60 frames a second, calling `each` on every frame.
static void run(React &r, float seconds, const ReactView &view, const std::function<void(const ReactOut &)> &each)
{
	const int frames = (int)std::lround(seconds / FRAME);
	for (int i = 0; i < frames; i++)
		each(r.step(FRAME, view));
}

// The most any of the head's numbers changes from one frame to the next.
struct Jumps {
	bool first = true;
	ReactOut last;
	float turn = 0, place = 0, squint = 0, face = 0;
	void see(const ReactOut &o, const std::vector<std::string> &faces = {})
	{
		if (!first) {
			turn = std::max({turn, std::fabs(o.yaw - last.yaw), std::fabs(o.pitch - last.pitch),
					 std::fabs(o.roll - last.roll)});
			place = std::max({place, std::fabs(o.dx - last.dx), std::fabs(o.dy - last.dy)});
			squint = std::max(squint, std::fabs(o.squint - last.squint));
			for (const std::string &n : faces)
				face = std::max(face, std::fabs(face_amount(o, n) - face_amount(last, n)));
		}
		first = false;
		last = o;
	}
};

static void test_names()
{
	for (BuiltIn b : {BuiltIn::Nod, BuiltIn::Shake, BuiltIn::Tilt, BuiltIn::Bounce, BuiltIn::Wave, BuiltIn::Cheer}) {
		CHECK(builtin_named(builtin_name(b)) == b);
		CHECK(builtin_length(b) > 0.5f && builtin_length(b) < 4);
	}
	CHECK(builtin_named("NOD") == BuiltIn::Nod);
	CHECK(builtin_named("Wave") == BuiltIn::Wave);
	CHECK(builtin_named("dance") == BuiltIn::None);
	CHECK(builtin_named("") == BuiltIn::None);
	CHECK(std::string(builtin_name(BuiltIn::None)).empty());
	CHECK(builtin_arms(BuiltIn::Wave) && builtin_arms(BuiltIn::Cheer));
	CHECK(!builtin_arms(BuiltIn::Nod) && !builtin_arms(BuiltIn::Shake) && !builtin_arms(BuiltIn::Tilt) &&
	      !builtin_arms(BuiltIn::Bounce) && !builtin_arms(BuiltIn::None));
}

static void test_faces()
{
	ReactView view;
	{
		// Eased in, held, eased out, gone.
		React r;
		r.face("happy", 2);
		ReactOut o = r.step(FRAME, view);
		CHECK(face_amount(o, "happy") > 0 && face_amount(o, "happy") < 0.2f);
		CHECK(o.busy);
		Jumps j;
		float at_1 = 0, at_2_2 = 0;
		float t = FRAME;
		run(r, 3, view, [&](const ReactOut &o) {
			t += FRAME;
			j.see(o, {"happy"});
			if (std::fabs(t - 1) < FRAME / 2)
				at_1 = face_amount(o, "happy");
			if (std::fabs(t - 2.2f) < FRAME / 2)
				at_2_2 = face_amount(o, "happy");
		});
		CHECK(at_1 == 1);
		CHECK(at_2_2 > 0.05f && at_2_2 < 1);
		CHECK(j.face < 0.12f);
		o = r.step(FRAME, view);
		CHECK(o.faces.empty());
		CHECK(!o.busy);
	}
	{
		// A new face takes over: the old one out, the new one in, smoothly.
		React r;
		r.face("happy", 5, 0.8f);
		run(r, 1, view, [](const ReactOut &) {});
		r.face("angry", 2);
		Jumps j;
		ReactOut half;
		int frame = 0;
		run(r, 1, view, [&](const ReactOut &o) {
			j.see(o, {"happy", "angry"});
			if (++frame == 9)
				half = o;
		});
		CHECK(j.face < 0.12f);
		CHECK(face_amount(half, "happy") > 0 && face_amount(half, "happy") < 0.8f);
		CHECK(face_amount(half, "angry") > 0);
		ReactOut o = r.step(FRAME, view);
		CHECK(face_amount(o, "happy") == 0);
		CHECK(face_amount(o, "angry") == 1);
		// As much as asked.
		CHECK(o.faces.size() == 1);
	}
	{
		// The same face again keeps it on.
		React r;
		r.face("sad", 1);
		run(r, 0.8f, view, [](const ReactOut &) {});
		r.face("sad", 1);
		float least = 1;
		run(r, 0.9f, view, [&](const ReactOut &o) { least = std::min(least, face_amount(o, "sad")); });
		CHECK(least == 1);
		run(r, 0.6f, view, [](const ReactOut &) {});
		CHECK(r.step(FRAME, view).faces.empty());
	}
	{
		// The same face again while it's easing out (another took over).
		React r;
		r.face("happy", 3);
		run(r, 0.5f, view, [](const ReactOut &) {});
		r.face("angry", 3);
		run(r, 0.1f, view, [](const ReactOut &) {});
		r.face("happy", 3);
		Jumps j;
		bool once = true;
		run(r, 1, view, [&](const ReactOut &o) {
			j.see(o, {"happy", "angry"});
			int happy = 0;
			for (const FaceNow &f : o.faces)
				happy += f.name == "happy";
			once = once && happy <= 1;
		});
		CHECK(once);
		CHECK(j.face < 0.12f);
		CHECK(face_amount(r.step(FRAME, view), "happy") == 1);
	}
	{
		// No face: the one on eases off.
		React r;
		r.face("happy", 10);
		run(r, 1, view, [](const ReactOut &) {});
		r.face("", 1);
		Jumps j;
		run(r, 0.5f, view, [&](const ReactOut &o) { j.see(o, {"happy"}); });
		CHECK(j.face < 0.12f);
		CHECK(r.step(FRAME, view).faces.empty());
	}
	{
		// Odd numbers: nothing breaks, never on for more than a minute.
		React r;
		r.face("happy", std::numeric_limits<float>::quiet_NaN());
		r.face("angry", 5, std::numeric_limits<float>::infinity());
		CHECK(r.step(FRAME, view).faces.empty());
		r.face("happy", 1e9f, 7);
		run(r, 1, view, [](const ReactOut &) {});
		CHECK(face_amount(r.step(FRAME, view), "happy") == 1);
		run(r, MOST_FACE_TIME, view, [](const ReactOut &) {});
		CHECK(r.step(FRAME, view).faces.empty());
		r.face("sad", -3, -1);
		ReactOut o = r.step(FRAME, view);
		CHECK(finite_out(o));
		CHECK(face_amount(o, "sad") == 0);
		// Many faces, one after another: never a pile of them.
		for (int i = 0; i < 40; i++)
			r.face("f" + std::to_string(i), 5);
		o = r.step(FRAME, view);
		CHECK(o.faces.size() <= 6);
	}
}

static void test_moves()
{
	ReactView view;
	{
		// A nod: down twice and back, never a jump.
		React r;
		r.move(BuiltIn::Nod);
		float lowest = 0, highest = 0;
		Jumps j;
		run(r, builtin_length(BuiltIn::Nod) + 0.1f, view, [&](const ReactOut &o) {
			lowest = std::min(lowest, o.pitch);
			highest = std::max(highest, o.pitch);
			j.see(o);
			CHECK(o.body.empty());
		});
		CHECK(lowest < -8);
		CHECK(highest < 0.01f);
		CHECK(j.turn < 2);
		ReactOut o = r.step(FRAME, view);
		CHECK(std::fabs(o.pitch) < 1e-3f);
		CHECK(!o.busy);
	}
	{
		// A shake: both ways.
		React r;
		r.move(BuiltIn::Shake);
		float left = 0, right = 0;
		Jumps j;
		run(r, 1.3f, view, [&](const ReactOut &o) {
			left = std::min(left, o.yaw);
			right = std::max(right, o.yaw);
			j.see(o);
		});
		CHECK(left < -6 && right > 6);
		CHECK(j.turn < 3);
		CHECK(std::fabs(r.step(FRAME, view).yaw) < 1e-3f);
	}
	{
		// A tilt and a bounce.
		React r;
		r.move(BuiltIn::Tilt);
		float roll = 0;
		run(r, 1, view, [&](const ReactOut &o) { roll = std::max(roll, o.roll); });
		CHECK(roll > 12);
		React b;
		b.move(BuiltIn::Bounce);
		float up = 0;
		Jumps j;
		run(b, 1, view, [&](const ReactOut &o) {
			up = std::min(up, o.dy);
			j.see(o);
		});
		CHECK(up < -0.07f);
		CHECK(j.place < 0.02f);
	}
	{
		// A wave: the arms (for the rig), eased in and out, and a sway.
		React r;
		r.move(BuiltIn::Wave);
		float most = 0, last_time = -1;
		bool order = true, sway = false;
		run(r, builtin_length(BuiltIn::Wave) - 0.05f, view, [&](const ReactOut &o) {
			if (o.roll > 1)
				sway = true;
			for (const BodyMove &m : o.body) {
				order = order && m.builtin == BuiltIn::Wave && m.move == -1 && m.time > last_time;
				last_time = m.time;
				most = std::max(most, m.amount);
			}
		});
		CHECK(order);
		CHECK(most > 0.99f);
		CHECK(sway);
		run(r, 0.1f, view, [](const ReactOut &) {});
		CHECK(r.step(FRAME, view).body.empty());
	}
	{
		// The caller's own: played as long as it is, then gone.
		React r;
		r.play(3, 2);
		ReactOut o = r.step(FRAME, view);
		CHECK(o.body.size() == 1 && o.body[0].move == 3 && o.body[0].builtin == BuiltIn::None);
		CHECK(o.body[0].amount < 0.1f);
		float most = 0, last = 0, step = 0;
		run(r, 1.9f, view, [&](const ReactOut &o) {
			if (o.body.empty())
				return;
			most = std::max(most, o.body[0].amount);
			step = std::max(step, std::fabs(o.body[0].amount - last));
			last = o.body[0].amount;
		});
		CHECK(most > 0.99f);
		CHECK(step < 0.1f);
		run(r, 0.2f, view, [](const ReactOut &) {});
		CHECK(r.step(FRAME, view).body.empty());
		// Odd ones are skipped.
		r.play(-1, 2);
		r.play(2, std::numeric_limits<float>::quiet_NaN());
		r.move(BuiltIn::None);
		CHECK(r.step(FRAME, view).body.empty());
		// A very long one is cut at ten minutes.
		r.play(1, 1e9f);
		run(r, 601, view, [](const ReactOut &) {});
		CHECK(r.step(FRAME, view).body.empty());
	}
	{
		// Another move takes over: the old one eases out as the new one
		// comes in (oldest first), the head never jumping.
		React r;
		r.play(1, 5);
		run(r, 1, view, [](const ReactOut &) {});
		r.move(BuiltIn::Cheer);
		ReactOut o = r.step(FRAME, view);
		CHECK(o.body.size() == 2);
		if (o.body.size() == 2) {
			CHECK(o.body[0].move == 1 && o.body[1].builtin == BuiltIn::Cheer);
			CHECK(o.body[0].amount > 0.9f && o.body[1].amount < 0.1f);
		}
		run(r, 0.35f, view, [](const ReactOut &) {});
		o = r.step(FRAME, view);
		CHECK(o.body.size() == 1 && o.body[0].builtin == BuiltIn::Cheer);
		r.move(BuiltIn::Shake);
		Jumps j;
		run(r, 1.5f, view, [&](const ReactOut &o) { j.see(o); });
		CHECK(j.turn < 3);
		// Many at once: never a pile.
		for (int i = 0; i < 20; i++)
			r.play(i, 3);
		CHECK(r.step(FRAME, view).body.size() <= 3);
	}
}

// One throw from `side`, run to the end: what it did.
struct ThrowSeen {
	int hits = 0, hit_frame = -1, frames = 0;
	float start_x = 0, start_size = 0, hit_size = 0, hit_x = 0, hit_y = 0;
	float yaw_most = 0, pitch_most = 0, roll_most = 0, dx_most = 0;
	float squint_most = 0;
	int knock_frames = 0;
	V3 knock;
	int bursts_frames = 0;
	bool inside = true, finite = true, busy_end = true, shrinking = true;
	float face_most = 0;
	Jumps j;
};

static ThrowSeen throw_once(uint32_t seed, float side, float strength = 1, const std::string &face = "")
{
	ReactView view;
	React r(seed);
	Throw t;
	t.picture = 7;
	t.side = side;
	t.strength = strength;
	t.face = face;
	r.throw_at(t);
	ThrowSeen s;
	float last_size = 1e9f;
	run(r, 3, view, [&](const ReactOut &o) {
		s.frames++;
		s.finite = s.finite && finite_out(o);
		s.j.see(o);
		if (s.frames == 1 && !o.flying.empty()) {
			s.start_x = o.flying[0].x;
			s.start_size = o.flying[0].size;
		}
		for (const Flying &f : o.flying) {
			s.inside = s.inside && f.picture == 7;
			if (!f.hit) {
				s.shrinking = s.shrinking && f.size <= last_size + 1e-5f;
				last_size = f.size;
			}
		}
		if (o.hits) {
			s.hits += o.hits;
			s.hit_frame = s.frames;
			if (!o.flying.empty()) {
				s.hit_x = o.flying[0].x;
				s.hit_y = o.flying[0].y;
				s.hit_size = o.flying[0].size;
			}
		}
		if (o.knock.x != 0 || o.knock.y != 0 || o.knock.z != 0) {
			s.knock_frames++;
			s.knock = o.knock;
		}
		if (!o.bursts.empty())
			s.bursts_frames++;
		auto most = [](float &m, float v) {
			if (std::fabs(v) > std::fabs(m))
				m = v;
		};
		most(s.yaw_most, o.yaw);
		most(s.pitch_most, o.pitch);
		most(s.roll_most, o.roll);
		most(s.dx_most, o.dx);
		s.squint_most = std::max(s.squint_most, o.squint);
		if (!face.empty())
			s.face_most = std::max(s.face_most, face_amount(o, face));
		s.busy_end = o.busy;
	});
	return s;
}

static void test_throws()
{
	ReactView view;
	// From the right: it starts off the picture's right, big, shrinks as
	// it flies, hits its face once after FLY_TIME, the head knocked back
	// and away (nose to the picture's left, tipped back, rolled left),
	// hair thrown, a flash, an "ouch" - then it all settles.
	for (uint32_t seed : {1u, 2u, 3u, 99u, 12345u}) {
		ThrowSeen s = throw_once(seed, 1);
		CHECK(s.finite);
		CHECK(s.inside);
		CHECK(s.hits == 1);
		CHECK(std::abs(s.hit_frame - (int)std::lround(FLY_TIME / FRAME)) <= 1);
		CHECK(s.start_x > view.right);
		CHECK(s.shrinking);
		CHECK(s.start_size > s.hit_size * 2);
		CHECK(std::fabs(s.hit_x) < 0.25f && s.hit_y > -0.25f && s.hit_y < 0.15f);
		CHECK(s.yaw_most < -3);
		CHECK(s.pitch_most > 4);
		CHECK(s.roll_most < -2);
		CHECK(s.dx_most < 0);
		CHECK(s.knock_frames == 1);
		CHECK(s.knock.x < 0 && s.knock.z < 0);
		CHECK(s.bursts_frames > 10 && s.bursts_frames < 25);
		CHECK(s.squint_most == 1);
		CHECK(s.j.turn < 6);
		CHECK(s.j.place < 0.03f);
		CHECK(!s.busy_end);
	}
	// From the left: the other way.
	for (uint32_t seed : {4u, 5u, 6u}) {
		ThrowSeen s = throw_once(seed, -1);
		CHECK(s.start_x < view.left);
		CHECK(s.yaw_most > 3 && s.roll_most > 2 && s.dx_most > 0);
		CHECK(s.knock.x > 0);
	}
	// Either side, at random: both come up.
	{
		int lefts = 0, rights = 0;
		for (uint32_t seed = 1; seed <= 40; seed++) {
			ThrowSeen s = throw_once(seed * 7919u, 0);
			(s.start_x < 0 ? lefts : rights)++;
		}
		CHECK(lefts > 8 && rights > 8);
	}
	// Harder hits knock harder; a face if one is picked.
	{
		ThrowSeen soft = throw_once(8, 1, 0.1f), hard = throw_once(8, 1, 2), mid = throw_once(8, 1, 1, "angry");
		CHECK(std::fabs(hard.pitch_most) > std::fabs(soft.pitch_most) * 4);
		CHECK(mid.face_most == 1);
		CHECK(soft.face_most == 0);
		// Odd numbers are held to what's allowed.
		ThrowSeen odd = throw_once(8, 1, 1e9f);
		CHECK(std::fabs(odd.pitch_most - hard.pitch_most) < 1e-3f);
	}
	// The ouch: shut quickly after the hit, open again in under a second.
	{
		React r;
		r.throw_at(Throw());
		run(r, FLY_TIME + 0.1f, view, [](const ReactOut &) {});
		CHECK(r.step(FRAME, view).squint == 1);
		run(r, 0.8f, view, [](const ReactOut &) {});
		CHECK(r.step(FRAME, view).squint == 0);
	}
	// A volley: never more than MOST_FLYING on their way or MOST_WAITING
	// waiting, one a moment apart, every one that waited hits - each when
	// throw_at said it would - the head held to its most however many hit.
	{
		React r(77);
		Throw t;
		t.strength = 2;
		std::vector<float> when;
		for (int i = 0; i < 30; i++)
			when.push_back(r.throw_at(t));
		CHECK(r.waiting() == MOST_WAITING);
		CHECK(when[0] > FLY_TIME - 0.001f && when[0] < FLY_TIME + 0.001f);
		CHECK(std::fabs(when[15] - (FLY_TIME + 15 * THROW_GAP)) < 0.001f);
		CHECK(when[16] == -1 && when[29] == -1);
		int hits = 0, most_flying = 0, frame = 0;
		float worst = 0;
		bool held = true, finite = true;
		run(r, 6, view, [&](const ReactOut &o) {
			frame++;
			for (int k = 0; k < o.hits; k++) {
				// Within a frame or two of when it said.
				worst = std::max(worst, std::fabs(frame * FRAME - when[hits]));
				hits++;
			}
			int coming = 0;
			for (const Flying &f : o.flying)
				coming += !f.hit;
			most_flying = std::max(most_flying, coming);
			held = held && std::fabs(o.yaw) <= 35.01f && std::fabs(o.pitch) <= 30.01f &&
			       std::fabs(o.roll) <= 25.01f && std::fabs(o.dx) <= 0.351f && std::fabs(o.dy) <= 0.351f;
			finite = finite && finite_out(o);
		});
		CHECK(hits == MOST_WAITING);
		CHECK(worst < 2.5f * FRAME);
		CHECK(most_flying <= MOST_FLYING);
		CHECK(most_flying >= 4);
		CHECK(held);
		CHECK(finite);
		CHECK(!r.step(FRAME, view).busy);
	}
	// Stopped: nothing more hits, what's in the air falls away, nothing
	// waiting, and it's all quiet within a second.
	{
		React r;
		r.face("happy", 10);
		r.play(2, 10);
		for (int i = 0; i < 5; i++)
			r.throw_at(Throw());
		run(r, 0.25f, view, [](const ReactOut &) {});
		r.stop();
		CHECK(r.waiting() == 0);
		int hits = 0;
		Jumps j;
		bool fading = true;
		run(r, 1, view, [&](const ReactOut &o) {
			hits += o.hits;
			j.see(o, {"happy"});
			for (const Flying &f : o.flying)
				fading = fading && f.hit;
		});
		CHECK(hits == 0);
		CHECK(fading);
		CHECK(j.face < 0.12f);
		ReactOut o = r.step(FRAME, view);
		CHECK(!o.busy);
		CHECK(o.flying.empty() && o.faces.empty() && o.body.empty());
	}
	// A huge pause between frames, and a tiny picture: still fine.
	{
		React r;
		r.throw_at(Throw());
		ReactView tiny;
		tiny.left = -0.3f;
		tiny.right = 0.3f;
		tiny.top = -0.3f;
		tiny.bottom = 0.3f;
		ReactOut o = r.step(FRAME, tiny);
		CHECK(finite_out(o));
		o = r.step(30, tiny);
		CHECK(finite_out(o));
		o = r.step(-1, tiny);
		CHECK(finite_out(o));
		o = r.step(std::numeric_limits<float>::quiet_NaN(), tiny);
		CHECK(finite_out(o));
		run(r, 2, tiny, [](const ReactOut &) {});
		CHECK(!r.step(FRAME, tiny).busy);
		Throw odd;
		odd.side = std::numeric_limits<float>::quiet_NaN();
		odd.size = std::numeric_limits<float>::infinity();
		odd.face_time = -5;
		r.throw_at(odd);
		bool fine = true;
		run(r, 2, view, [&](const ReactOut &o) { fine = fine && finite_out(o); });
		CHECK(fine);
	}
}

static void test_pats()
{
	ReactView view;
	view.head_top = 0.7f;
	{
		// Down from above the picture, three pats (its head bobbing with
		// each, its hair bounced), squinting happily with a face, then up
		// and away.
		React r;
		Pat p;
		p.picture = 4;
		p.count = 3;
		r.pat(p);
		float first_y = 0, lowest_dy = 0, most_dy = 0, squint = 0, happy = 0;
		int knocks = 0, frames = 0, shown_frames = 0;
		bool picture = true, finite = true;
		float hand_jump = 0, last_y = 0;
		bool had_last = false;
		Jumps j;
		run(r, 3, view, [&](const ReactOut &o) {
			frames++;
			finite = finite && finite_out(o);
			j.see(o, {"happy"});
			if (o.hand.shown) {
				shown_frames++;
				picture = picture && o.hand.picture == 4;
				if (frames == 1)
					first_y = o.hand.y;
				if (had_last)
					hand_jump = std::max(hand_jump, std::fabs(o.hand.y - last_y));
				last_y = o.hand.y;
				had_last = true;
			}
			if (o.knock.y < 0)
				knocks++;
			lowest_dy = std::min(lowest_dy, o.dy);
			most_dy = std::max(most_dy, o.dy);
			squint = std::max(squint, o.squint);
			happy = std::max(happy, face_amount(o, "happy"));
		});
		CHECK(finite && picture);
		CHECK(first_y < view.top);
		CHECK(knocks == 3);
		CHECK(most_dy > 0.03f);
		CHECK(lowest_dy > -0.001f);
		CHECK(std::fabs(squint - 0.6f) < 0.01f);
		CHECK(happy == 1);
		const float length = 0.45f + 3 * 0.42f + 0.45f;
		CHECK(std::abs(shown_frames - (int)std::lround(length / FRAME)) <= 2);
		CHECK(hand_jump < 0.25f);
		CHECK(j.squint < 0.1f && j.face < 0.12f && j.place < 0.02f);
		ReactOut o = r.step(FRAME, view);
		CHECK(!o.hand.shown && !o.busy);
	}
	{
		// Patting: its hand comes down onto the top of its head (pushing
		// it down a little) with each pat.
		React r;
		Pat p;
		p.count = 4;
		p.face = "";
		r.pat(p);
		float lowest = -100;
		bool face = false;
		run(r, 1.2f, view, [&](const ReactOut &o) {
			lowest = std::max(lowest, o.hand.y);
			face = face || !o.faces.empty();
		});
		CHECK(std::fabs(lowest - (0.035f - view.head_top)) < 0.01f);
		CHECK(!face);
	}
	{
		// A character filling its picture, its head up to the top edge:
		// the hand comes down far enough to be seen (nearly half of it),
		// never below its eyebrows - on any picture of a hand.
		for (float tall : {1.25f, 0.5f, 3.0f}) {
			ReactView edge = view;
			edge.top = -0.64f;
			edge.hand_tall = tall;
			React r;
			Pat p;
			p.count = 4;
			r.pat(p);
			float lowest = -100, start = 100;
			bool first = true;
			run(r, 1.2f, edge, [&](const ReactOut &o) {
				if (first && o.hand.shown)
					start = o.hand.y;
				first = first && !o.hand.shown;
				lowest = std::max(lowest, o.hand.y);
			});
			const float hand = 0.85f * tall;
			const float shows = (lowest - edge.top) / hand;
			CHECK(shows > 0.44f || lowest > -0.25f - 0.001f);
			CHECK(lowest < -0.25f + 0.035f + 0.005f);
			CHECK(start <= edge.top + 0.01f);          // from out of sight
		}
		// Too odd to use: a hand as tall as usual.
		ReactView odd = view;
		odd.hand_tall = std::nanf("");
		React r;
		Pat p;
		r.pat(p);
		bool fine = true;
		run(r, 3, odd, [&](const ReactOut &o) { fine = fine && finite_out(o); });
		CHECK(fine);
	}
	{
		// More pats while patting: the same hand keeps going, never more
		// than MOST_PATS to come.
		React r;
		Pat p;
		p.count = 2;
		r.pat(p);
		run(r, 0.6f, view, [](const ReactOut &) {});
		p.count = 2;
		r.pat(p);
		int knocks = 0;
		run(r, 5, view, [&](const ReactOut &o) { knocks += o.knock.y < 0; });
		CHECK(knocks == 3); // 4 in all, one already made
		React big;
		p.count = 1000;
		big.pat(p);
		for (int i = 0; i < 10; i++)
			big.pat(p);
		knocks = 0;
		run(big, 30, view, [&](const ReactOut &o) { knocks += o.knock.y < 0; });
		CHECK(knocks == MOST_PATS);
	}
	{
		// Another headpat as the hand goes: it takes over from where the
		// hand is, no jump.
		React r;
		Pat p;
		p.count = 1;
		r.pat(p);
		run(r, 0.45f + 0.42f + 0.2f, view, [](const ReactOut &) {});
		ReactOut before = r.step(FRAME, view);
		CHECK(before.hand.shown);
		r.pat(p);
		float jump = 0, op_jump = 0, last_y = before.hand.y, last_op = before.hand.opacity;
		run(r, 2, view, [&](const ReactOut &o) {
			if (!o.hand.shown)
				return;
			jump = std::max(jump, std::fabs(o.hand.y - last_y));
			op_jump = std::max(op_jump, std::fabs(o.hand.opacity - last_op));
			last_y = o.hand.y;
			last_op = o.hand.opacity;
		});
		CHECK(jump < 0.25f);
		CHECK(op_jump < 0.15f);
	}
	{
		// Stopped mid-pat: the hand leaves from where it is.
		React r;
		Pat p;
		p.count = 10;
		r.pat(p);
		run(r, 0.45f + 0.42f * 2.5f, view, [](const ReactOut &) {});
		float last_y = r.step(FRAME, view).hand.y;
		r.stop();
		float jump = 0;
		run(r, 1, view, [&](const ReactOut &o) {
			if (!o.hand.shown)
				return;
			jump = std::max(jump, std::fabs(o.hand.y - last_y));
			last_y = o.hand.y;
		});
		CHECK(jump < 0.25f);
		CHECK(!r.step(FRAME, view).busy);
		// Stopped as it came in, too.
		r.pat(p);
		run(r, 0.2f, view, [](const ReactOut &) {});
		last_y = r.step(FRAME, view).hand.y;
		r.stop();
		jump = 0;
		run(r, 1, view, [&](const ReactOut &o) {
			if (!o.hand.shown)
				return;
			jump = std::max(jump, std::fabs(o.hand.y - last_y));
			last_y = o.hand.y;
		});
		CHECK(jump < 0.25f);
		CHECK(!r.step(FRAME, view).busy);
	}
	{
		// Odd numbers.
		React r;
		Pat p;
		p.count = -5;
		p.size = std::numeric_limits<float>::quiet_NaN();
		r.pat(p);
		bool fine = true;
		int knocks = 0;
		run(r, 2, view, [&](const ReactOut &o) {
			fine = fine && finite_out(o);
			knocks += o.knock.y < 0;
		});
		CHECK(fine);
		CHECK(knocks == 1);
	}
}

// Thrown and patted at once, with a face and a move: it all adds up and
// settles.
static void test_all_at_once()
{
	ReactView view;
	React r(5);
	r.face("surprised", 1.5f);
	r.move(BuiltIn::Nod);
	Pat p;
	r.pat(p);
	for (int i = 0; i < 3; i++)
		r.throw_at(Throw());
	int hits = 0;
	bool fine = true;
	run(r, 5, view, [&](const ReactOut &o) {
		hits += o.hits;
		fine = fine && finite_out(o);
	});
	CHECK(hits == 3);
	CHECK(fine);
	ReactOut o = r.step(FRAME, view);
	CHECK(!o.busy);
	CHECK(o.faces.empty() && o.flying.empty() && o.bursts.empty() && !o.hand.shown && o.body.empty());
	CHECK(o.yaw == 0 && o.pitch == 0 && o.roll == 0 && o.dx == 0 && o.dy == 0 && o.squint == 0);
	// The same from the same start.
	auto trace = [&](uint32_t seed) {
		React a(seed);
		for (int i = 0; i < 4; i++)
			a.throw_at(Throw());
		std::vector<float> v;
		run(a, 1.5f, view, [&](const ReactOut &o) {
			v.push_back(o.yaw);
			for (const Flying &f : o.flying)
				v.push_back(f.x);
		});
		return v;
	};
	CHECK(trace(9) == trace(9));
	CHECK(trace(9) != trace(10));
}

int main()
{
	test_names();
	test_faces();
	test_moves();
	test_throws();
	test_pats();
	test_all_at_once();
	std::printf("unit_react: %d passed, %d failed\n", passes, failures);
	return failures ? 1 : 0;
}
