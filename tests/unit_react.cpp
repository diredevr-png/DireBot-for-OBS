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
		    ok(o.hand.size) && ok(o.hand.across) && ok(o.hand.angle) && ok(o.hand.opacity);
	for (const FaceNow &f : o.faces)
		good = good && ok(f.amount) && f.amount >= 0 && f.amount <= 1;
	for (const BodyMove &m : o.body)
		good = good && ok(m.time) && ok(m.amount) && m.amount >= 0 && m.amount <= 1;
	for (const Flying &f : o.flying)
		good = good && ok(f.x) && ok(f.y) && ok(f.size) && ok(f.angle) && ok(f.opacity) && f.size > 0 &&
		       f.opacity >= 0 && f.opacity <= 1 && ok(f.squash_x) && ok(f.squash_y);
	for (const Burst &b : o.bursts)
		good = good && ok(b.x) && ok(b.y) && ok(b.size) && ok(b.opacity);
	for (const Mark &m : o.marks)
		good = good && ok(m.x) && ok(m.y) && ok(m.size) && ok(m.angle) && ok(m.opacity) && ok(m.length) &&
		       m.size > 0 && m.opacity > 0 && m.opacity <= 1 && (!m.from_top || m.length > 0);
	for (const Bit &b : o.bits)
		good = good && ok(b.x) && ok(b.y) && ok(b.size) && ok(b.angle) && ok(b.opacity) && b.size > 0 &&
		       b.opacity > 0 && b.opacity <= 1;
	for (const Flying &f : o.flying)
		good = good && ok(f.squash_z) && f.squash_z > 0 && ok(f.wobble) && f.wobble >= -1 && f.wobble <= 1;
	for (const Piece3D &p : o.pieces)
		good = good && ok(p.x) && ok(p.y) && ok(p.z) && ok(p.size) && ok(p.open) && ok(p.opacity) &&
		       ok(p.turn.x) && ok(p.turn.y) && ok(p.turn.z) && ok(p.turn.w) && ok(p.shape.x) &&
		       ok(p.shape.y) && ok(p.shape.z) && p.size > 0 && p.opacity > 0 && p.opacity <= 1 &&
		       p.open >= 0 && p.open <= 1 && p.shape.x > 0 && p.shape.y > 0 && p.shape.z > 0;
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

// How long a headpat's hand shows: coming in, its pats (it leaves part way
// into its last one's lift) and going.
static float pat_length(int count)
{
	return 0.5f + (count - 1 + 0.42f) * 0.42f + 0.5f;
}

static void test_pats()
{
	ReactView view;
	view.head_top = 0.7f;
	{
		// In from the picture's left, over its head, three pats (its head
		// dipping with each and leaning into the hand, its hair bounced),
		// squinting happily with a face, then up and away.
		React r;
		Pat p;
		p.picture = 4;
		p.count = 3;
		p.side = -1;
		r.pat(p);
		float first_x = 0, first_y = 0, first_away = 0, first_rise = 0, lowest_dy = 0, most_dy = 0;
		float squint = 0, happy = 0, lean = 0, most_lift = 0, most_rise = 0, turned = 0;
		int knocks = 0, frames = 0, shown_frames = 0;
		bool picture = true, finite = true, side = true;
		float hand_jump = 0, last_x = 0, last_y = 0, lift_jump = 0, last_lift = 0;
		bool had_last = false;
		Jumps j;
		run(r, 3.5f, view, [&](const ReactOut &o) {
			frames++;
			finite = finite && finite_out(o);
			j.see(o, {"happy"});
			if (o.hand.shown) {
				shown_frames++;
				picture = picture && o.hand.picture == 4;
				side = side && o.hand.side == -1 && o.hand.x <= 1e-6f && o.hand.angle <= 1e-6f;
				if (frames == 1) {
					first_x = o.hand.x;
					first_y = o.hand.y;
					first_away = o.hand.away;
					first_rise = o.hand.rise;
				}
				if (had_last) {
					hand_jump = std::max({hand_jump, std::fabs(o.hand.y - last_y),
							      std::fabs(o.hand.x - last_x)});
					lift_jump = std::max(lift_jump, std::fabs(o.hand.lift - last_lift));
				}
				last_x = o.hand.x;
				last_y = o.hand.y;
				last_lift = o.hand.lift;
				had_last = true;
				if (frames * FRAME > 0.6f && frames * FRAME < 1.3f) {
					most_lift = std::max(most_lift, o.hand.lift);
					most_rise = std::max(most_rise, o.hand.rise);
					turned = std::min(turned, o.hand.angle);
				}
			}
			if (o.knock.y < 0)
				knocks++;
			lowest_dy = std::min(lowest_dy, o.dy);
			most_dy = std::max(most_dy, o.dy);
			lean = std::min(lean, o.roll);
			squint = std::max(squint, o.squint);
			happy = std::max(happy, face_amount(o, "happy"));
		});
		CHECK(finite && picture && side);
		// From above and out to its side.
		CHECK(first_x < -0.5f && first_away > 0.5f && first_rise > 0.85f);
		CHECK(first_y < -view.head_top - 0.8f);
		CHECK(knocks == 3);
		CHECK(most_dy > 0.03f && most_dy < 0.036f);
		CHECK(lowest_dy > -0.001f);
		CHECK(lean < -5.5f && lean > -6.5f); // the top of its head to the left
		CHECK(std::fabs(squint - 0.7f) < 0.01f);
		CHECK(happy == 1);
		// Lifted from the wrist between pats, a little off its head.
		CHECK(most_lift > 0.99f && std::fabs(most_rise - 0.1f) < 0.002f);
		CHECK(std::fabs(turned + 0.38f) < 0.01f);
		CHECK(std::abs(shown_frames - (int)std::lround(pat_length(3) / FRAME)) <= 2);
		CHECK(hand_jump < 0.12f && lift_jump < 0.3f); // quick as it lands
		CHECK(j.squint < 0.1f && j.face < 0.12f && j.place < 0.02f && j.turn < 2.5f);
		ReactOut o = r.step(FRAME, view);
		CHECK(!o.hand.shown && !o.busy);
		CHECK(o.squish == 0 && o.squint == 0);
	}
	{
		// Each pat squishes its head (shorter, a little wider) and it
		// springs back a little past its own shape; once the hand has gone
		// its eyes stay shut happily a moment, then open - its face on
		// till then.
		React r;
		Pat p;
		p.count = 3;
		r.pat(p);
		float most = 0, least = 0, gone_at = -1, t = 0, squish_jump = 0, last_squish = 0;
		float squint_after_03 = -1, squint_after_13 = -1, happy_after_03 = -1;
		bool finite = true, was_shown = false;
		run(r, 3.5f, view, [&](const ReactOut &o) {
			t += FRAME;
			finite = finite && std::isfinite(o.squish);
			most = std::max(most, o.squish);
			least = std::min(least, o.squish);
			squish_jump = std::max(squish_jump, std::fabs(o.squish - last_squish));
			last_squish = o.squish;
			if (o.hand.shown)
				was_shown = true;
			else if (was_shown && gone_at < 0)
				gone_at = t;
			if (gone_at >= 0 && squint_after_03 < 0 && t >= gone_at + 0.3f) {
				squint_after_03 = o.squint;
				happy_after_03 = face_amount(o, "happy");
			}
			if (gone_at >= 0 && squint_after_13 < 0 && t >= gone_at + 1.3f)
				squint_after_13 = o.squint;
		});
		CHECK(finite);
		CHECK(most > 0.85f && most < 1.3f);
		CHECK(least < -0.03f && least > -0.5f);
		CHECK(squish_jump < 0.35f);
		CHECK(gone_at > 0);
		CHECK(std::fabs(squint_after_03 - 0.7f) < 0.01f && happy_after_03 > 0.5f);
		CHECK(squint_after_13 == 0);
		CHECK(!r.step(FRAME, view).busy);
		// Stopped: its eyes open straight away (no moment shut after).
		React s;
		s.pat(p);
		run(s, 1.2f, view, [](const ReactOut &) {});
		s.stop();
		float shut_at_08 = -1;
		t = 0;
		run(s, 1.2f, view, [&](const ReactOut &o) {
			t += FRAME;
			if (shut_at_08 < 0 && t >= 0.95f)
				shut_at_08 = o.squint;
		});
		CHECK(shut_at_08 >= 0 && shut_at_08 < 0.01f);
		CHECK(!s.step(FRAME, view).busy);
	}
	{
		// A pat: it lands, rests a moment pressing (its head giving under
		// it, then coming back up), is lifted from the wrist (eased) and
		// falls again - faster as it lands than as it was lifted.
		React r;
		Pat p;
		p.count = 6;
		p.face = "";
		p.side = 1;
		r.pat(p);
		run(r, 0.5f + 0.42f, view, [](const ReactOut &) {}); // to the second landing
		std::vector<float> lift, dy, roll;
		float x = 0, angle = 0;
		bool face = false;
		run(r, 0.42f, view, [&](const ReactOut &o) {
			lift.push_back(o.hand.lift);
			dy.push_back(o.dy);
			roll.push_back(o.roll);
			x = std::max(x, o.hand.x);
			angle = std::max(angle, o.hand.angle);
			face = face || !o.faces.empty();
		});
		const size_t n = lift.size();
		CHECK(n == 25);
		CHECK(lift[0] < 0.01f && lift[n * 25 / 100] < 0.001f); // resting
		CHECK(*std::max_element(lift.begin(), lift.end()) > 0.97f);
		const size_t top = std::max_element(lift.begin(), lift.end()) - lift.begin();
		CHECK(top > n / 2 && top < n * 85 / 100);
		float up = 0, down = 0;
		for (size_t i = 1; i < n; i++) {
			up = std::max(up, lift[i] - lift[i - 1]);
			down = std::max(down, lift[i - 1] - lift[i]);
		}
		CHECK(down > 1.3f * up);
		const size_t deepest = std::max_element(dy.begin(), dy.end()) - dy.begin();
		CHECK(deepest <= 4 && dy[deepest] > 0.03f);
		CHECK(dy[n / 2] < 0.01f);
		CHECK(*std::min_element(roll.begin(), roll.end()) > 5.9f); // leaning right, towards it
		CHECK(x < 1e-6f && angle > 0.3f);
		CHECK(!face);
	}
	{
		// Patting: its hand rests on the top of its head, going down with
		// it as it gives.
		React r;
		Pat p;
		p.count = 4;
		r.pat(p);
		float lowest = -100;
		run(r, 1.2f, view, [&](const ReactOut &o) { lowest = std::max(lowest, o.hand.y); });
		CHECK(std::fabs(lowest - (0.035f - view.head_top)) < 0.004f);
	}
	{
		// Either side, at random.
		int left = 0, right = 0;
		for (uint32_t seed = 1; seed <= 20; seed++) {
			React r(seed);
			r.pat(Pat());
			const ReactOut o = r.step(FRAME, view);
			left += o.hand.side == -1;
			right += o.hand.side == 1;
		}
		CHECK(left + right == 20 && left > 2 && right > 2);
	}
	{
		// A character filling its picture, its head up to the top edge:
		// a picture of the hand comes down far enough to be seen (nearly
		// half of it), never below its eyebrows - any picture of a hand.
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
			CHECK(start <= edge.top + 0.01f); // from out of sight
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
		int knocks = 0;
		run(r, 0.6f, view, [&](const ReactOut &o) { knocks += o.knock.y < 0; });
		CHECK(knocks == 1);
		p.count = 2;
		r.pat(p);
		run(r, 5, view, [&](const ReactOut &o) { knocks += o.knock.y < 0; });
		CHECK(knocks == 4);
		React big;
		p.count = 1000;
		big.pat(p);
		for (int i = 0; i < 10; i++)
			big.pat(p);
		knocks = 0;
		run(big, 30, view, [&](const ReactOut &o) { knocks += o.knock.y < 0; });
		CHECK(knocks == MOST_PATS);
		// More once its last pat has landed (before it goes): one more
		// lift and fall.
		React late;
		p.count = 1;
		late.pat(p);
		knocks = 0;
		run(late, 0.55f, view, [&](const ReactOut &o) { knocks += o.knock.y < 0; });
		late.pat(p);
		run(late, 3, view, [&](const ReactOut &o) { knocks += o.knock.y < 0; });
		CHECK(knocks == 2);
	}
	{
		// Another headpat as the hand goes: it takes over from where the
		// hand is (from the same side), no jump.
		React r;
		Pat p;
		p.count = 1;
		p.side = 1;
		r.pat(p);
		run(r, pat_length(1) - 0.3f, view, [](const ReactOut &) {});
		ReactOut before = r.step(FRAME, view);
		CHECK(before.hand.shown && before.hand.opacity < 1);
		p.side = -1;
		r.pat(p);
		float jump = 0, op_jump = 0, last_x = before.hand.x, last_y = before.hand.y,
		      last_op = before.hand.opacity;
		bool same_side = true;
		run(r, 2, view, [&](const ReactOut &o) {
			if (!o.hand.shown)
				return;
			same_side = same_side && o.hand.side == 1;
			jump = std::max({jump, std::fabs(o.hand.y - last_y), std::fabs(o.hand.x - last_x)});
			op_jump = std::max(op_jump, std::fabs(o.hand.opacity - last_op));
			last_x = o.hand.x;
			last_y = o.hand.y;
			last_op = o.hand.opacity;
		});
		CHECK(jump < 0.12f);
		CHECK(op_jump < 0.15f);
		CHECK(same_side);
	}
	{
		// Stopped mid-pat: the hand leaves from where it is.
		React r;
		Pat p;
		p.count = 10;
		r.pat(p);
		run(r, 0.5f + 0.42f * 2.5f, view, [](const ReactOut &) {});
		ReactOut o = r.step(FRAME, view);
		float last_x = o.hand.x, last_y = o.hand.y;
		r.stop();
		float jump = 0;
		run(r, 1, view, [&](const ReactOut &o) {
			if (!o.hand.shown)
				return;
			jump = std::max({jump, std::fabs(o.hand.y - last_y), std::fabs(o.hand.x - last_x)});
			last_x = o.hand.x;
			last_y = o.hand.y;
		});
		CHECK(jump < 0.12f);
		CHECK(!r.step(FRAME, view).busy);
		// Stopped as it came in, too.
		r.pat(p);
		run(r, 0.2f, view, [](const ReactOut &) {});
		o = r.step(FRAME, view);
		last_x = o.hand.x;
		last_y = o.hand.y;
		r.stop();
		jump = 0;
		run(r, 1, view, [&](const ReactOut &o) {
			if (!o.hand.shown)
				return;
			jump = std::max({jump, std::fabs(o.hand.y - last_y), std::fabs(o.hand.x - last_x)});
			last_x = o.hand.x;
			last_y = o.hand.y;
		});
		CHECK(jump < 0.12f);
		CHECK(!r.step(FRAME, view).busy);
	}
	{
		// Odd numbers.
		React r;
		Pat p;
		p.count = -5;
		p.size = std::numeric_limits<float>::quiet_NaN();
		p.side = std::numeric_limits<float>::quiet_NaN();
		r.pat(p);
		bool fine = true;
		int knocks = 0;
		run(r, 2, view, [&](const ReactOut &o) {
			fine = fine && finite_out(o) && std::isfinite(o.hand.rise + o.hand.away + o.hand.lift) &&
			       (!o.hand.shown || o.hand.side == -1 || o.hand.side == 1);
			knocks += o.knock.y < 0;
		});
		CHECK(fine);
		CHECK(knocks == 1);
	}
}

// Things that stick (they stay where they hit, moving with its head, then
// slide off), and bonks from above.
static void test_sticks_and_bonks()
{
	ReactView view;
	view.head_top = 0.7f;
	{
		// Stuck for 2 seconds: where it hit, whole, moving with its head;
		// then sliding down a little, falling and gone.
		React r;
		Throw t;
		t.picture = 1;
		t.stick = 2;
		t.side = 1;
		CHECK(std::fabs(r.throw_at(t) - 0.5f) < 0.11f);
		float t_now = 0, hit_x = 0, hit_y = 0, follow_err = 0, slid = 0, faded = 1;
		int stuck_frames = 0;
		bool hit = false, fine = true, upright = true;
		run(r, 3.6f, view, [&](const ReactOut &o) {
			t_now += FRAME;
			fine = fine && finite_out(o);
			for (const Flying &f : o.flying) {
				if (f.stuck) {
					stuck_frames++;
					fine = fine && f.opacity == 1;
					// Where it hit, as its head is moved and tipped.
					const float a = o.roll * 3.14159265f / 180;
					if (!hit) {
						hit = true;
						hit_x = (f.x - o.dx) * std::cos(a) + (f.y - o.dy) * std::sin(a);
						hit_y = -(f.x - o.dx) * std::sin(a) + (f.y - o.dy) * std::cos(a);
					} else if (t_now < 2.0f) {
						// Standing nearly upright once it's settled.
						if (t_now > 0.8f) {
							const float turn = std::remainder(f.angle - a, 2 * 3.14159265f);
							upright = upright && std::fabs(turn) <= 0.3f + 1e-3f;
						}
						const float x = (f.x - o.dx) * std::cos(a) + (f.y - o.dy) * std::sin(a);
						const float y = -(f.x - o.dx) * std::sin(a) + (f.y - o.dy) * std::cos(a);
						follow_err = std::max({follow_err, std::fabs(x - hit_x), std::fabs(y - hit_y)});
					} else {
						const float y = -(f.x - o.dx) * std::sin(a) + (f.y - o.dy) * std::cos(a);
						slid = std::max(slid, y - hit_y);
					}
				} else if (hit && f.hit) {
					faded = std::min(faded, f.opacity);
				}
			}
			if (t_now > 0.9f && t_now < 2.3f)
				fine = fine && r.stuck() == 1;
		});
		CHECK(fine && hit);
		CHECK(std::abs(stuck_frames - (int)std::lround(2.0f / FRAME)) <= 2);
		CHECK(upright);
		CHECK(follow_err < 0.002f);
		CHECK(slid > 0.15f && slid < 0.2f);
		CHECK(faded < 0.05f);
		CHECK(r.flying() == 0 && r.stuck() == 0);
		CHECK(!r.step(FRAME, view).busy);
	}
	{
		// Never more than MOST_STUCK at once (the oldest falls first), and
		// stop() makes them all fall.
		React r;
		Throw t;
		t.stick = 10;
		for (int i = 0; i < MOST_WAITING; i++)
			CHECK(r.throw_at(t) > 0);
		int most = 0;
		run(r, 4, view, [&](const ReactOut &) { most = std::max(most, r.stuck()); });
		CHECK(most == MOST_STUCK);
		r.stop();
		ReactOut o = r.step(FRAME, view);
		CHECK(r.stuck() == 0);
		bool falling = !o.flying.empty();
		for (const Flying &f : o.flying)
			falling = falling && !f.stuck;
		CHECK(falling);
		run(r, 1.5f, view, [](const ReactOut &) {});
		CHECK(r.flying() == 0 && !r.step(FRAME, view).busy);
		// Odd numbers: never stuck, or no longer than MOST_STICK.
		React q;
		t.stick = std::numeric_limits<float>::quiet_NaN();
		q.throw_at(t);
		t.stick = -3;
		q.throw_at(t);
		int stuck = 0;
		run(q, 1, view, [&](const ReactOut &) { stuck = std::max(stuck, q.stuck()); });
		CHECK(stuck == 0);
		t.stick = 1000;
		q.throw_at(t);
		run(q, 1, view, [](const ReactOut &) {});
		CHECK(q.stuck() == 1);
		run(q, MOST_STICK - 0.4f, view, [](const ReactOut &) {});
		CHECK(q.stuck() == 0);
	}
	{
		// A bonk: over its head (rising and tipping back), then down on
		// top of it - its head nodded down and squished - and off to its
		// side.
		// It lands with its bottom on the top of its head (a fifth of it
		// into the hair), not its middle - that hid its forehead and eyes.
		ReactView roomy = view;
		roomy.top = -2.5f;
		React r;
		Throw t;
		t.picture = 2;
		t.above = true;
		t.side = 1;
		CHECK(std::fabs(r.throw_at(t) - 0.5f) < 0.11f);
		float t_now = 0, first_y = 0, highest = 10, tipped = 0, hit_y = 0, nod = 0, squish = 0, after_x = 0;
		float x_drift = 0, start_x = 0, flash_y = 0;
		int hits = 0;
		bool first = true, fine = true, same_size = true;
		run(r, 1.6f, roomy, [&](const ReactOut &o) {
			t_now += FRAME;
			fine = fine && finite_out(o);
			hits += o.hits;
			if (o.hits) {
				hit_y = o.flying.empty() ? 0 : o.flying[0].y;
				flash_y = o.bursts.empty() ? 0 : o.bursts[0].y;
			}
			nod = std::min(nod, o.pitch);
			squish = std::max(squish, o.squish);
			if (o.flying.empty())
				return;
			const Flying &f = o.flying[0];
			if (!f.hit) {
				if (first) {
					first_y = f.y;
					start_x = f.x;
					first = false;
				}
				highest = std::min(highest, f.y);
				tipped = std::min(tipped, f.angle);
				x_drift = std::max(x_drift, std::fabs(f.x - start_x));
				same_size = same_size && std::fabs(f.size - 0.45f) < 1e-4f;
			} else if (t_now > 1.0f) {
				after_x = f.x;
			}
		});
		CHECK(fine && hits == 1);
		CHECK(first_y < -0.7f - 0.2f);
		CHECK(highest < first_y - 0.1f);
		CHECK(tipped < -0.5f); // tipped back, away from its right
		CHECK(x_drift < 1e-4f && same_size); // straight down, with room
		const float half = 0.45f / 2;
		CHECK(std::fabs(hit_y - (-0.7f - half * 0.6f)) < 1e-3f);
		CHECK(std::fabs(flash_y - -0.7f) < 1e-3f); // the flash on the top of its head
		CHECK(nod < -8);
		CHECK(squish > 0.5f);
		CHECK(after_x > 0.4f); // off to its right
		run(r, 1.5f, roomy, [](const ReactOut &) {});
		CHECK(!r.step(FRAME, roomy).busy);
		// Twice as tall as it's wide (a bottle): higher, its bottom on
		// its head all the same.
		React b;
		Throw bottle = t;
		bottle.tall = 2;
		b.throw_at(bottle);
		float bottle_y = 0;
		run(b, 0.6f, roomy, [&](const ReactOut &o) {
			if (o.hits)
				bottle_y = o.flying[0].y;
		});
		CHECK(std::fabs(bottle_y - (-0.7f - 2 * half * 0.6f)) < 1e-3f);
		// Not as much room over its head: it comes down slantwise.
		React s;
		s.throw_at(t);
		const float s_x = s.step(FRAME, view).flying[0].x;
		float s_hit_x = s_x;
		run(s, 0.6f, view, [&](const ReactOut &o) {
			if (o.hits)
				s_hit_x = o.flying[0].x;
		});
		CHECK(s_hit_x - s_x > 0.1f);
		// No room above its head in the picture (its head up to the top
		// edge): it comes in slantwise from its other side, never more
		// than half of it out of the picture.
		ReactView tight = view;
		tight.top = -0.75f;
		React q;
		q.throw_at(t);
		ReactOut o = q.step(FRAME, tight);
		CHECK(o.flying.size() == 1);
		bool inside = true;
		float least_y = 10;
		run(q, 0.45f, tight, [&](const ReactOut &o) {
			for (const Flying &f : o.flying)
				if (!f.hit) {
					inside = inside && f.y >= tight.top - 1e-3f;
					least_y = std::min(least_y, f.y);
				}
		});
		CHECK(inside);
		CHECK(o.flying[0].x < -0.6f); // from its left: it bounces off to its right
		CHECK(least_y < -0.7f + 0.08f - 0.05f);
		// A narrow picture: it starts in it across too.
		ReactView narrow = tight;
		narrow.left = -0.7f;
		narrow.right = 0.7f;
		React n;
		n.throw_at(t);
		o = n.step(FRAME, narrow);
		CHECK(o.flying.size() == 1 && o.flying[0].x >= -0.7f + 0.45f * 0.6f - 1e-4f);
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

// 0.9.0: what a thing does as it hits - a splat left on its face (juice
// thrown off, drips running down, sliding off and fading), feathers
// floating down, pieces bursting off (some snow left a moment), a squishy
// thing squashed flat and wobbling back.
static void test_effects()
{
	CHECK(effect_named("splat") == Effect::Splat && effect_named("FEATHERS") == Effect::Feathers);
	CHECK(effect_named("pieces") == Effect::Pieces && effect_named("nope") == Effect::None);
	CHECK(effect_named("") == Effect::None);
	for (Effect e : {Effect::None, Effect::Splat, Effect::Feathers, Effect::Pieces})
		CHECK(effect_named(effect_name(e)) == e);
	ReactView view;
	auto splat = []() {
		Throw t;
		t.picture = 0;
		t.side = 1;
		t.stick = 3; // broken as it hits: never stuck
		t.look.effect = Effect::Splat;
		t.look.mark = 5;
		t.look.drip = 6;
		t.look.bits = {7};
		t.look.seconds = 2;
		return t;
	};
	auto step_to_hit = [&](React &r) {
		ReactOut o;
		for (int i = 0; i < 200 && !o.hits; i++)
			o = r.step(FRAME, view);
		return o;
	};
	{
		// The splat: left where it hit, juice thrown off, the tomato gone.
		React r(3);
		CHECK(r.throw_at(splat()) > 0);
		ReactOut o = step_to_hit(r);
		CHECK(o.hits == 1);
		CHECK(o.marks.size() == 1 && o.marks[0].picture == 5 && !o.marks[0].from_top);
		const float mx = o.marks[0].x, my = o.marks[0].y;
		CHECK(std::fabs(mx) < 0.4f && std::fabs(my) < 0.4f);
		CHECK(o.bits.size() >= 8 && o.bits.size() <= 11);
		for (const Bit &b : o.bits)
			CHECK(b.picture == 7);
		CHECK(r.stuck() == 0);
		// It spreads out from a little under its size, a touch past it,
		// and settles.
		float first = o.marks[0].size, most = 0, settled = 0;
		bool tomato_gone = false, drips_grow = true;
		float drip_seen = 0;
		std::vector<float> last_len(8, 0);
		// The juice's middle, a tenth and three tenths of a second on.
		std::vector<float> juice_y;
		bool fine = finite_out(o);
		run(r, 1.0f, view, [&](const ReactOut &q) {
			fine = fine && finite_out(q);
			for (const Mark &m : q.marks)
				if (!m.from_top) {
					most = std::max(most, m.size);
					settled = m.size;
				}
			if (q.flying.empty())
				tomato_gone = true;
			int d = 0;
			for (const Mark &m : q.marks)
				if (m.from_top) {
					CHECK(m.picture == 6);
					drips_grow = drips_grow && m.length + 1e-5f >= last_len[d];
					last_len[d++] = m.length;
					drip_seen = std::max(drip_seen, m.length);
				}
			if (!q.bits.empty()) {
				float y = 0;
				for (const Bit &b : q.bits)
					y += b.y / q.bits.size();
				juice_y.push_back(y);
			}
		});
		CHECK(fine);
		CHECK(first < most && most > settled && settled > first);
		CHECK(std::fabs(most / settled - 1.08f) < 0.03f);
		CHECK(tomato_gone && r.flying() == 0);
		CHECK(drips_grow && drip_seen > 0.1f);
		// Thrown out and up, then falling.
		CHECK(juice_y.size() > 20 && juice_y[20] > juice_y[6]);
		// The juice falls off and is gone well before the splat.
		ReactOut now = r.step(FRAME, view);
		CHECK(now.bits.empty());
		CHECK(!now.marks.empty());
		// It stays its seconds (2 here), slides down a little as it fades,
		// then it's gone and all is still.
		float y_before = 0;
		for (const Mark &m : now.marks)
			if (!m.from_top)
				y_before = m.y;
		run(r, 1.2f, view, [](const ReactOut &) {});
		now = r.step(FRAME, view);
		float y_after = y_before;
		for (const Mark &m : now.marks)
			if (!m.from_top)
				y_after = m.y;
		CHECK(!now.marks.empty() && y_after > y_before + 0.02f);
		run(r, 1.0f, view, [](const ReactOut &) {});
		now = r.step(FRAME, view);
		CHECK(now.marks.empty() && !now.busy);
	}
	{
		// What's left on its face tilts with its face (a camera's), and
		// moves with its head.
		React r(4);
		r.throw_at(splat());
		ReactOut o = step_to_hit(r);
		run(r, 0.8f, view, [](const ReactOut &) {});
		o = r.step(FRAME, view);
		Mark before;
		for (const Mark &m : o.marks)
			if (!m.from_top)
				before = m;
		ReactView tipped = view;
		tipped.roll = 0.4f;
		o = r.step(FRAME, tipped);
		Mark after;
		for (const Mark &m : o.marks)
			if (!m.from_top)
				after = m;
		const float c = std::cos(0.4f), s = std::sin(0.4f);
		CHECK(std::fabs(after.x - (before.x * c - before.y * s)) < 0.02f);
		CHECK(std::fabs(after.y - (before.x * s + before.y * c)) < 0.02f);
		CHECK(std::fabs(after.angle - before.angle - 0.4f) < 0.02f);
	}
	{
		// Feathers: the pillow bounces off as before, feathers float down
		// slowly, swaying, and are gone in a few seconds.
		React r(5);
		Throw t;
		t.picture = 0;
		t.above = true;
		t.look.effect = Effect::Feathers;
		t.look.bits = {8, 9};
		r.throw_at(t);
		ReactOut o = step_to_hit(r);
		CHECK(o.bits.size() >= 12 && o.bits.size() <= 16);
		CHECK(o.marks.empty());
		bool both = false;
		for (const Bit &b : o.bits)
			both = both || b.picture == 9;
		CHECK(both);
		run(r, 0.3f, view, [](const ReactOut &) {});
		o = r.step(FRAME, view);
		CHECK(!o.flying.empty() && o.flying[0].opacity > 0.3f); // the pillow, bouncing off
		// Drifting down slowly once the air has them, swaying side to side.
		run(r, 0.9f, view, [](const ReactOut &) {});
		std::vector<Bit> a = r.step(FRAME, view).bits;
		run(r, 0.5f, view, [](const ReactOut &) {});
		std::vector<Bit> b = r.step(FRAME, view).bits;
		CHECK(a.size() == b.size() && !a.empty());
		float fall = 0;
		for (size_t i = 0; i < a.size() && i < b.size(); i++)
			fall += (b[i].y - a[i].y) / a.size();
		CHECK(fall > 0.05f && fall < 0.3f);
		run(r, 3.0f, view, [](const ReactOut &) {});
		CHECK(r.step(FRAME, view).bits.empty() && r.bits() == 0);
	}
	{
		// Pieces: crushed, then chunks and a puff burst off it, some snow
		// left a moment.
		React r(6);
		Throw t;
		t.picture = 0;
		t.look.effect = Effect::Pieces;
		t.look.mark = 3;
		t.look.bits = {10, 11, 12};
		t.look.puff = 13;
		t.look.seconds = 1;
		r.throw_at(t);
		ReactOut o = step_to_hit(r);
		int chunks = 0, puffs = 0;
		float puff_size = 0;
		for (const Bit &b : o.bits) {
			chunks += b.picture >= 10 && b.picture <= 12;
			if (b.picture == 13) {
				puffs++;
				puff_size = b.size;
			}
		}
		CHECK(chunks >= 9 && chunks <= 12 && puffs == 2);
		CHECK(o.marks.size() == 1 && o.marks[0].picture == 3);
		run(r, 0.25f, view, [](const ReactOut &) {});
		o = r.step(FRAME, view);
		CHECK(o.flying.empty());
		for (const Bit &b : o.bits)
			if (b.picture == 13)
				CHECK(b.size > puff_size * 1.3f);
		run(r, 1.0f, view, [](const ReactOut &) {});
		CHECK(r.step(FRAME, view).bits.empty());
		run(r, 1.0f, view, [](const ReactOut &) {});
		CHECK(!r.step(FRAME, view).busy);
	}
	{
		// A glitter bomb: its cloud of glitter dust hangs (puff_seconds),
		// and the glitter left on its face glints (its height map and
		// shine); a snowball's puff is still gone in well under a second.
		React r(16);
		Throw t;
		t.picture = 0;
		t.look.effect = Effect::Pieces;
		t.look.mark = 3;
		t.look.mark_bump = 4;
		t.look.shine = 1.4f;
		t.look.puff = 13;
		t.look.puff_seconds = 1.8f;
		t.look.seconds = 4;
		r.throw_at(t);
		ReactOut o = step_to_hit(r);
		CHECK(o.marks.size() == 1 && o.marks[0].bump == 4 && std::fabs(o.marks[0].shine - 1.4f) < 1e-4f);
		int puffs = 0;
		for (const Bit &b : o.bits)
			puffs += b.picture == 13;
		CHECK(puffs == 2);
		run(r, 1.2f, view, [](const ReactOut &) {});
		puffs = 0;
		for (const Bit &b : r.step(FRAME, view).bits)
			puffs += b.picture == 13 && b.opacity > 0.05f;
		CHECK(puffs == 2);
		run(r, 0.9f, view, [](const ReactOut &) {});
		puffs = 0;
		for (const Bit &b : r.step(FRAME, view).bits)
			puffs += b.picture == 13;
		CHECK(puffs == 0);
		// Never longer than MOST_PUFF, whatever it's set to.
		React r2(17);
		t.look.puff_seconds = 1000;
		r2.throw_at(t);
		step_to_hit(r2);
		run(r2, MOST_PUFF * 1.1f + 0.05f, view, [](const ReactOut &) {});
		puffs = 0;
		for (const Bit &b : r2.step(FRAME, view).bits)
			puffs += b.picture == 13;
		CHECK(puffs == 0);
		// A snowball (no puff_seconds): gone by 0.7 s; its snow flat.
		React r3(18);
		Throw s;
		s.picture = 0;
		s.look.effect = Effect::Pieces;
		s.look.mark = 3;
		s.look.puff = 13;
		r3.throw_at(s);
		o = step_to_hit(r3);
		CHECK(o.marks.size() == 1 && o.marks[0].bump == -1);
		run(r3, 0.7f, view, [](const ReactOut &) {});
		puffs = 0;
		for (const Bit &b : r3.step(FRAME, view).bits)
			puffs += b.picture == 13;
		CHECK(puffs == 0);
	}
	{
		// A squishy one: squashed flat as it hits, wobbling back (taller
		// than it is a moment), settling.
		React r(7);
		Throw t;
		t.picture = 0;
		t.look.squishy = true;
		r.throw_at(t);
		step_to_hit(r);
		float widest = 0, tallest = 0, last = 0;
		run(r, 0.6f, view, [&](const ReactOut &q) {
			if (q.flying.empty())
				return;
			widest = std::max(widest, q.flying[0].squash_x);
			tallest = std::max(tallest, q.flying[0].squash_y);
			last = q.flying[0].squash_x;
		});
		CHECK(widest > 1.45f && tallest > 1.05f && std::fabs(last - 1) < 0.02f);
		// Not squishy: as before.
		React plain(7);
		plain.throw_at(Throw());
		step_to_hit(plain);
		float plain_widest = 0;
		run(plain, 0.3f, view, [&](const ReactOut &q) {
			for (const Flying &f : q.flying)
				plain_widest = std::max(plain_widest, f.squash_x);
		});
		CHECK(plain_widest < 1.36f);
	}
	{
		// Never too much: the oldest splats fade as more come; bits capped;
		// stop clears it all quickly; odd numbers are safe.
		React r(8);
		int showing_most = 0;
		size_t bits_most = 0;
		bool fine = true;
		for (int round = 0; round < 4; round++) {
			for (int i = 0; i < 12; i++) {
				Throw t = splat();
				t.look.seconds = std::numeric_limits<float>::quiet_NaN();
				r.throw_at(t);
			}
			run(r, 1.6f, view, [&](const ReactOut &q) {
				fine = fine && finite_out(q);
				bits_most = std::max(bits_most, q.bits.size());
			});
			showing_most = std::max(showing_most, r.marks());
		}
		CHECK(fine);
		CHECK(bits_most <= (size_t)MOST_BITS);
		int splats = 0;
		for (const Mark &m : r.step(FRAME, view).marks)
			splats += !m.from_top && m.opacity > 0.99f;
		CHECK(splats <= MOST_MARKS && splats > 0);
		r.stop();
		run(r, 0.45f, view, [](const ReactOut &) {});
		ReactOut o = r.step(FRAME, view);
		CHECK(o.marks.empty() && o.bits.empty());
	}
}

// 0.12.0: 3D pieces thrown off as it hits - a tomato's pulp tumbling away
// the way it was going, its juice in drops thrown out all round and
// stretched by their speed, the sheet of juice spreading open in a blink
// and settling flat - each shrinking away (stopped: at once), never too
// many; a quick mist; what's left on its face wet; a broken 3D thing
// squashed flat against its face.
static void test_pieces()
{
	PieceWay w = PieceWay::Chunk;
	CHECK(piece_way_named("DROP", w) && w == PieceWay::Drop);
	CHECK(piece_way_named("crown", w) && w == PieceWay::Crown);
	CHECK(!piece_way_named("blob", w) && w == PieceWay::Crown && !piece_way_named("", w));
	for (PieceWay v : {PieceWay::Chunk, PieceWay::Drop, PieceWay::Crown, PieceWay::Flutter, PieceWay::Rise})
		CHECK(piece_way_named(piece_way_name(v), w) && w == v);
	CHECK(piece_way_named("Rise", w) && w == PieceWay::Rise);
	// (Room below for all of it to fall and shrink away in sight.)
	ReactView view;
	view.bottom = 6;
	auto tomato = []() {
		Throw t;
		t.picture = 0;
		t.side = 1; // from the picture's right: going left
		t.three_d = true;
		t.look.effect = Effect::Splat;
		t.look.mark = 5;
		t.look.drip = 6;
		t.look.puff = 8;
		t.look.mark_bump = 9;
		t.look.drip_bump = 10;
		t.look.shine = 0.7f;
		t.look.seconds = 1;
		PieceLook crown, chunk, drop;
		crown.thing = 0;
		crown.way = PieceWay::Crown;
		crown.size = 0.5f;
		chunk.thing = 1;
		chunk.least = 3;
		chunk.most = 5;
		chunk.size = 0.13f;
		drop.thing = 2;
		drop.way = PieceWay::Drop;
		drop.least = 18;
		drop.most = 26;
		drop.size = 0.05f;
		t.look.pieces = {crown, chunk, drop};
		return t;
	};
	auto step_to_hit = [&](React &r) {
		ReactOut o;
		for (int i = 0; i < 200 && !o.hits; i++)
			o = r.step(FRAME, view);
		return o;
	};
	auto of = [](const ReactOut &o, int thing) {
		std::vector<Piece3D> got;
		for (const Piece3D &p : o.pieces)
			if (p.thing == thing)
				got.push_back(p);
		return got;
	};
	for (uint32_t seed : {1u, 2u, 3u, 4u, 5u}) {
		React r(seed);
		CHECK(r.throw_at(tomato()) > 0);
		ReactOut o = step_to_hit(r);
		CHECK(o.hits == 1 && finite_out(o));
		const std::vector<Piece3D> crowns = of(o, 0), chunks = of(o, 1), drops = of(o, 2);
		CHECK(crowns.size() == 1);
		CHECK(chunks.size() >= 3 && chunks.size() <= 5);
		CHECK(drops.size() >= 18 && drops.size() <= 26);
		CHECK((int)o.pieces.size() == r.pieces());
		// The crown where it burst (a little on the way it was going),
		// lying flat with its open side towards you, just burst; a little
		// see-through; longer the way it was going.
		CHECK(o.marks.size() >= 1);
		const Mark splat = o.marks[0];
		const Piece3D &c0 = crowns[0];
		CHECK(std::fabs(c0.x - splat.x) < 0.1f && std::fabs(c0.y - splat.y) < 0.1f && c0.z > 0);
		CHECK(c0.open < 0.65f && c0.opacity <= 0.9f + 1e-6f && c0.shape.x > 1.1f);
		const V3 up = rotate(c0.turn, {0, 1, 0});
		CHECK(up.z > 0.99f);
		// Its long way (its own x) along the way it was going (leftwards,
		// and down as it came down on its arc).
		CHECK(rotate(c0.turn, {1, 0, 0}).x < -0.2f);
		// What's left on its face is wet, its drips too; the mist.
		CHECK(splat.bump == 9 && std::fabs(splat.shine - 0.7f) < 1e-6f);
		int mists = 0;
		for (const Bit &b : o.bits)
			mists += b.picture == 8;
		CHECK(mists == 1);
		// The tomato itself squashed flat against its face: wider every
		// way, thin towards you.
		CHECK(o.flying.size() == 1 && o.flying[0].squash_z < 0.5f &&
		      std::fabs(o.flying[0].squash_x - o.flying[0].squash_y) < 1e-6f && o.flying[0].squash_x > 1.2f);
		// On: the crown opens in a blink and grows, then settles flat and
		// is gone in about a third of a second.
		float open_at_70ms = 0, biggest = c0.size, flattest = 1;
		float crown_gone = -1, mist_gone = -1, t = 0;
		// The chunks: the way it was going (left) at first, towards you,
		// and falling; shrinking away at the end. The drops: stretched,
		// their fronts the way they go.
		float chunk_vx = 0, first_x = 0;
		for (const Piece3D &p : chunks)
			first_x += p.x / chunks.size();
		bool drops_point = true, drops_stretched = true, towards_you = true, fine = true;
		std::vector<Piece3D> last = o.pieces;
		std::vector<float> chunk_y;
		bool drips_wet = false;
		float chunk_small = 1;
		while (t < 1.4f) {
			ReactOut q = r.step(FRAME, view);
			t += FRAME;
			fine = fine && finite_out(q);
			const std::vector<Piece3D> cr = of(q, 0), ch = of(q, 1), dr = of(q, 2);
			if (!cr.empty()) {
				if (t <= 0.07f + 1e-4f)
					open_at_70ms = cr[0].open;
				biggest = std::max(biggest, cr[0].size);
				flattest = std::min(flattest, cr[0].shape.y);
			} else if (crown_gone < 0) {
				crown_gone = t;
			}
			bool mist = false;
			for (const Bit &b : q.bits)
				mist = mist || b.picture == 8;
			if (!mist && mist_gone < 0)
				mist_gone = t;
			for (const Mark &m : q.marks)
				drips_wet = drips_wet || (m.from_top && m.bump == 10);
			if (t < 0.1f && !ch.empty()) {
				float x = 0;
				for (const Piece3D &p : ch)
					x += p.x / ch.size();
				chunk_vx = (x - first_x) / t;
				for (const Piece3D &p : ch)
					towards_you = towards_you && p.z > 0;
			}
			if (!ch.empty()) {
				float y = 0;
				for (const Piece3D &p : ch)
					y += p.y / ch.size();
				chunk_y.push_back(y);
				for (const Piece3D &p : ch)
					chunk_small = std::min(chunk_small, p.size / 0.13f);
			}
			// Each drop's front the way it went since the frame before.
			std::vector<Piece3D> lastd;
			for (const Piece3D &p : last)
				if (p.thing == 2)
					lastd.push_back(p);
			if (t < 0.3f && lastd.size() == dr.size())
				for (size_t i = 0; i < dr.size(); i++) {
					const V3 went{dr[i].x - lastd[i].x, -(dr[i].y - lastd[i].y), dr[i].z - lastd[i].z};
					if (length(went) > 1e-4f)
						drops_point = drops_point &&
							      dot(rotate(dr[i].turn, {1, 0, 0}), normalized(went)) > 0.95f;
					drops_stretched = drops_stretched && dr[i].shape.x > 1.05f && dr[i].shape.y < 1 &&
							  std::fabs(dr[i].shape.y - dr[i].shape.z) < 1e-5f;
				}
			last = q.pieces;
		}
		CHECK(fine);
		CHECK(open_at_70ms > 0.99f && biggest > c0.size * 1.3f && flattest < 0.3f);
		CHECK(crown_gone > 0.3f && crown_gone < 0.4f);
		CHECK(mist_gone > 0.15f && mist_gone < 0.32f);
		CHECK(drips_wet);
		CHECK(chunk_vx < -0.3f && towards_you);
		CHECK(chunk_y.size() > 30 && chunk_y[30] > chunk_y[5]);
		CHECK(chunk_small < 0.5f);
		CHECK(drops_point && drops_stretched);
		// All gone, and all still.
		run(r, 3.0f, view, [](const ReactOut &) {});
		ReactOut now = r.step(FRAME, view);
		CHECK(now.pieces.empty() && r.pieces() == 0 && !now.busy);
	}
	{
		// Fluttering (a pillow's feathers): out all round, slowed at once,
		// then drifting down slowly for a few seconds, swaying, turning.
		React r(13);
		Throw t;
		t.picture = 0;
		t.side = -1;
		PieceLook f;
		f.thing = 7;
		f.way = PieceWay::Flutter;
		f.least = f.most = 14;
		f.size = 0.12f;
		t.look.pieces = {f};
		r.throw_at(t);
		ReactOut o = step_to_hit(r);
		CHECK(of(o, 7).size() == 14);
		float y0 = 0;
		for (const Piece3D &p : o.pieces)
			y0 += p.y / o.pieces.size();
		std::vector<float> ys;
		std::vector<V3> ups;
		float last = 0;
		int frames = 0;
		bool fine = true, slow = true;
		std::vector<float> xs_first;
		run(r, 2.5f, view, [&](const ReactOut &q) {
			fine = fine && finite_out(q);
			frames++;
			if (q.pieces.empty())
				return;
			float y = 0;
			for (const Piece3D &p : q.pieces)
				y += p.y / q.pieces.size();
			if (frames > 60 && !ys.empty())
				slow = slow && (y - ys.back()) * 60 < 0.75f;
			ys.push_back(y);
			if (frames % 10 == 0)
				ups.push_back(rotate(q.pieces[0].turn, {0, 1, 0}));
			last = y;
		});
		CHECK(fine && slow);
		// Still in the air after 2.5 s, well below where it burst.
		CHECK(r.pieces() == 14 && last > y0 + 0.4f && last < y0 + 2.0f);
		// Turning over as it goes (the way its top faces changes).
		float least = 1;
		for (size_t i = 1; i < ups.size(); i++)
			least = std::min(least, dot(ups[i], ups[0]));
		CHECK(least < 0.3f);
		run(r, 2.0f, view, [](const ReactOut &) {});
		CHECK(r.pieces() == 0);
	}
	{
		// Stopped: its pieces shrink away at once.
		React r(9);
		r.throw_at(tomato());
		ReactOut o = step_to_hit(r);
		CHECK(!o.pieces.empty());
		r.stop();
		run(r, 0.25f, view, [](const ReactOut &) {});
		CHECK(r.step(FRAME, view).pieces.empty());
	}
	{
		// Never too many: a barrage keeps under MOST_PIECES.
		React r(10);
		for (int i = 0; i < MOST_WAITING; i++)
			r.throw_at(tomato());
		int most = 0;
		run(r, 3.0f, view, [&](const ReactOut &q) { most = std::max(most, (int)q.pieces.size()); });
		CHECK(most > 60 && most <= MOST_PIECES);
	}
	{
		// Pieces of a thing that only hits (no effect): thrown all the
		// same, and it bounces off as before; odd numbers are safe (none,
		// no thing, least past most).
		React r(11);
		Throw t;
		t.picture = 0;
		t.three_d = true;
		PieceLook a, b, c;
		a.thing = 3;
		a.least = a.most = 2;
		b.thing = -1;
		b.least = b.most = 5;
		c.thing = 4;
		c.least = 9;
		c.most = 2;
		c.size = std::numeric_limits<float>::quiet_NaN();
		t.look.pieces = {a, b, c};
		r.throw_at(t);
		ReactOut o = step_to_hit(r);
		CHECK(of(o, 3).size() == 2 && of(o, -1).empty());
		CHECK(of(o, 4).empty() && finite_out(o));
		CHECK(o.flying.size() == 1 && o.flying[0].squash_z == 1);
		run(r, 0.3f, view, [](const ReactOut &) {});
		CHECK(r.flying() == 1);
		// A picture broken against it: as before (squashed as a picture).
		React r2(12);
		Throw p = tomato();
		p.three_d = false;
		p.look.pieces.clear();
		r2.throw_at(p);
		o = step_to_hit(r2);
		CHECK(o.flying.size() == 1 && o.flying[0].squash_z == 1 && o.flying[0].squash_y < 0.7f);
		CHECK(o.pieces.empty());
	}
}

// 0.12.0: a thing that stays whole with a splat (a pie's tin), one that
// flies point first (a dart), wobbling 3D things, rising puffs (a stink).
static void test_whole_point_wobble_rise()
{
	ReactView view;
	view.bottom = 6;
	auto step_to_hit = [&](React &r) {
		ReactOut o;
		for (int i = 0; i < 200 && !o.hits; i++)
			o = r.step(FRAME, view);
		return o;
	};
	{
		// A pie: a splat with pieces, its tin whole and stuck a moment, then
		// sliding off and falling; a splat broken as before without `whole`.
		for (uint32_t seed : {1u, 2u, 3u}) {
			React r(seed);
			Throw t;
			t.picture = 0;
			t.three_d = true;
			t.stick = 0.4f;
			t.look.effect = Effect::Splat;
			t.look.mark = 1;
			t.look.whole = true;
			PieceLook lump;
			lump.thing = 2;
			lump.least = lump.most = 3;
			t.look.pieces = {lump};
			CHECK(r.throw_at(t) > 0);
			ReactOut o = step_to_hit(r);
			CHECK(o.hits == 1 && finite_out(o));
			CHECK(o.flying.size() == 1 && o.flying[0].stuck && o.flying[0].squash_z == 1);
			CHECK(o.marks.size() == 1 && o.pieces.size() == 3);
			// (Moving with its head as the hit knocks it: never far.)
			const float x = o.flying[0].x, y = o.flying[0].y;
			run(r, 0.3f, view, [&](const ReactOut &q) {
				CHECK(q.flying.size() == 1 && q.flying[0].stuck && q.flying[0].opacity > 0.99f);
				CHECK(std::hypot(q.flying[0].x - x, q.flying[0].y - y) < 0.35f);
			});
			bool fell = false;
			run(r, 1.5f, view, [&](const ReactOut &q) {
				fell = fell || (q.flying.size() == 1 && !q.flying[0].stuck);
				CHECK(finite_out(q));
			});
			CHECK(fell && r.flying() == 0 && r.step(FRAME, view).marks.size() == 1);
		}
		// Not whole: broken as before, never stuck.
		React r(4);
		Throw t;
		t.picture = 0;
		t.three_d = true;
		t.stick = 2;
		t.look.effect = Effect::Splat;
		t.look.mark = 1;
		r.throw_at(t);
		ReactOut o = step_to_hit(r);
		CHECK(o.flying.size() == 1 && !o.flying[0].stuck && o.flying[0].squash_z < 0.5f);
		// Whole and not sticking: it bounces off, its splat left all the same.
		React r2(5);
		t.stick = 0;
		t.look.whole = true;
		r2.throw_at(t);
		o = step_to_hit(r2);
		CHECK(o.flying.size() == 1 && !o.flying[0].stuck && o.flying[0].squash_z == 1 && o.marks.size() == 1);
		run(r2, 0.2f, view, [](const ReactOut &) {});
		o = r2.step(FRAME, view);
		CHECK(o.flying.size() == 1 && o.flying[0].y < o.marks[0].y);
	}
	{
		// A dart: point first, never turning on its way, at its forehead;
		// stuck, its tail swings side to side (about its up), dying away;
		// it pops off up and out, tumbling.
		for (uint32_t seed : {6u, 7u, 8u, 9u}) {
			React r(seed);
			Throw t;
			t.picture = 0;
			t.three_d = true;
			t.stick = 1.5f;
			t.look.point = true;
			r.throw_at(t);
			ReactOut o;
			for (int i = 0; i < 200 && !o.hits; i++) {
				o = r.step(FRAME, view);
				if (!o.hits && o.flying.size() == 1) {
					CHECK(o.flying[0].angle == 0);
					CHECK(o.flying[0].axis.z == 1);
				}
			}
			CHECK(o.flying.size() == 1 && o.flying[0].stuck);
			CHECK(o.flying[0].y <= -0.25f && o.flying[0].y >= -0.37f && std::fabs(o.flying[0].x) <= 0.17f);
			float early = 0, late = 0;
			int sign_changes = 0;
			float last = 0;
			run(r, 1.2f, view, [&](const ReactOut &q) {
				if (q.flying.size() != 1 || !q.flying[0].stuck)
					return;
				// (About its up, give or take its head's tilt.)
				const Flying &f = q.flying[0];
				CHECK(std::fabs(f.axis.y) > 0.8f || std::fabs(f.angle) < 0.12f);
				CHECK(std::fabs(f.angle) <= 0.42f);
				const float swing = f.angle * f.axis.y;
				if (swing * last < 0)
					sign_changes++;
				last = swing;
				if (sign_changes < 2)
					early = std::max(early, std::fabs(swing));
				else if (sign_changes > 6)
					late = std::max(late, std::fabs(swing));
			});
			CHECK(early > 0.1f && late < early * 0.5f && sign_changes >= 6);
			bool up = false, tumbles = false;
			float y0 = 0;
			run(r, 1.0f, view, [&](const ReactOut &q) {
				if (q.flying.size() != 1 || q.flying[0].stuck)
					return;
				if (y0 == 0)
					y0 = q.flying[0].y;
				up = up || q.flying[0].y < y0 - 0.02f;
				tumbles = tumbles || std::fabs(q.flying[0].axis.y) < 0.999f;
			});
			CHECK(up && tumbles);
		}
	}
	{
		// Steady (a pie): tipped back a little as it leaves (about its own
		// x), tipping forward all the way to as made as it lands, stuck so;
		// it lands where a throw does (not at its forehead); falling off,
		// it tips on over.
		for (uint32_t seed : {13u, 14u, 15u}) {
			React r(seed);
			Throw t;
			t.picture = 0;
			t.three_d = true;
			t.stick = 0.4f;
			t.look.effect = Effect::Splat;
			t.look.mark = 1;
			t.look.whole = true;
			t.look.steady = true;
			r.throw_at(t);
			ReactOut o;
			float last = 1e9f, first = 1;
			bool tips_forward = true;
			for (int i = 0; i < 200 && !o.hits; i++) {
				o = r.step(FRAME, view);
				for (const Flying &f : o.flying)
					if (!f.hit) {
						CHECK(f.axis.x == 1 && f.axis.y == 0 && f.axis.z == 0);
						if (first > 0)
							first = f.angle;
						tips_forward = tips_forward && std::fabs(f.angle) <= last + 1e-6f;
						last = std::fabs(f.angle);
					}
			}
			// (Less than none: the renderer's way round, showing its top.)
			CHECK(first < -0.9f && first > -1.3f && tips_forward && last < 0.05f);
			// (Stuck, it tips with its head as the hit knocks it.)
			CHECK(o.flying.size() == 1 && o.flying[0].stuck);
			CHECK(o.flying[0].y >= -0.3f && o.flying[0].y <= 0.2f);
			run(r, 0.3f, view, [&](const ReactOut &q) {
				if (q.flying.size() == 1 && q.flying[0].stuck)
					CHECK(std::fabs(q.flying[0].angle) < 0.1f);
			});
			// Falling off, tipping on over about its own x, mostly.
			float most = 0;
			run(r, 0.6f, view, [&](const ReactOut &q) {
				for (const Flying &f : q.flying)
					if (!f.stuck) {
						CHECK(std::fabs(f.axis.x) > 0.9f);
						most = std::max(most, -f.angle);
					}
			});
			CHECK(most > 0.5f);
		}
	}
	{
		// Steady from above (an ice cream, made upside down): it comes down
		// as a bonk does (turning only in the picture), sits on its head
		// as made, then slides off to its side, toppling that way.
		for (float side : {-1.0f, 1.0f}) {
			React r(16);
			Throw t;
			t.picture = 0;
			t.three_d = true;
			t.above = true;
			t.side = side;
			t.stick = 1.2f;
			t.look.effect = Effect::Splat;
			t.look.mark = 1;
			t.look.whole = true;
			t.look.steady = true;
			r.throw_at(t);
			ReactOut o;
			for (int i = 0; i < 200 && !o.hits; i++) {
				o = r.step(FRAME, view);
				for (const Flying &f : o.flying)
					CHECK(std::fabs(f.axis.x) < 1e-5f && std::fabs(f.axis.y) < 1e-5f &&
					      std::fabs(f.axis.z) > 0.9999f);
			}
			CHECK(o.flying.size() == 1 && o.flying[0].stuck);
			const float sat_x = o.flying[0].x;
			// (Squashed down onto its head: its bottom stays where it sits
			// as it springs back.)
			// (Its head kept still here, so only the squash would move it.)
			ReactView still = view;
			still.still_head = true;
			float bottom = 1e9f;
			run(r, 0.2f, still, [&](const ReactOut &q) {
				if (q.flying.size() == 1 && q.flying[0].stuck) {
					const float b = q.flying[0].y + 0.5f * q.flying[0].size * q.flying[0].squash_y;
					if (bottom > 1e8f)
						bottom = b;
					CHECK(std::fabs(b - bottom) < 0.004f);
				}
			});
			run(r, 0.3f, view, [&](const ReactOut &q) {
				if (q.flying.size() == 1 && q.flying[0].stuck)
					CHECK(std::fabs(q.flying[0].angle) < 0.1f);
			});
			float went = 0, toppled = 0;
			bool round_z = true;
			run(r, 1.4f, view, [&](const ReactOut &q) {
				for (const Flying &f : q.flying)
					if (!f.stuck) {
						round_z = round_z && std::fabs(f.axis.z) > 0.999f;
						went = (f.x - sat_x) * side;
						toppled = std::max(toppled, f.angle * side);
					}
			});
			CHECK(round_z && went > 0.15f && toppled > 0.5f);
		}
	}
	{
		// A hard thing (a pinecone) doesn't squash as it hits and bounces
		// off, nor stuck; one that isn't squashes a moment.
		for (bool hard : {true, false})
			for (float stick : {0.0f, 0.6f}) {
				React r(18);
				Throw t;
				t.picture = 0;
				t.three_d = true;
				t.stick = stick;
				t.look.hard = hard;
				r.throw_at(t);
				ReactOut o = step_to_hit(r);
				float most = 0;
				auto widest = [&](const ReactOut &q) {
					for (const Flying &f : q.flying)
						most = std::max({most, std::fabs(f.squash_x - 1), std::fabs(f.squash_y - 1),
								 std::fabs(f.squash_z - 1)});
				};
				widest(o);
				run(r, 1.0f, view, widest);
				if (hard)
					CHECK(most < 1e-6f);
				else
					CHECK(most > 0.2f);
			}
	}
	{
		// What a thing that stays whole leaves lies nearly level (a fish's
		// smear along it, slime sagging down); a broken one's, any way.
		float whole_most = 0, broken_most = 0;
		for (uint32_t seed = 30; seed < 50; seed++)
			for (bool whole : {true, false}) {
				React r(seed);
				Throw t;
				t.picture = 0;
				t.three_d = true;
				t.stick = 0.5f;
				t.look.effect = Effect::Splat;
				t.look.mark = 1;
				t.look.whole = whole;
				r.throw_at(t);
				const ReactOut o = step_to_hit(r);
				CHECK(o.marks.size() == 1);
				if (o.marks.size() == 1)
					(whole ? whole_most : broken_most) =
						std::max(whole ? whole_most : broken_most, std::fabs(o.marks[0].angle));
			}
		CHECK(whole_most < 0.35f && broken_most > 1.5f);
	}
	{
		// Stuck, a squishy one (slime) stays a flattened mound, thin towards
		// you; one that isn't, only a little flat. Coming off, neither is
		// squashed again as if it had just hit: each springs back round as
		// it falls, from as it was.
		for (bool squishy : {true, false}) {
			React r(17);
			Throw t;
			t.picture = 0;
			t.three_d = true;
			t.stick = 0.6f;
			t.look.squishy = squishy;
			r.throw_at(t);
			step_to_hit(r);
			Flying last;
			run(r, 0.5f, view, [&](const ReactOut &q) {
				if (q.flying.size() == 1 && q.flying[0].stuck)
					last = q.flying[0];
			});
			CHECK(last.stuck);
			if (squishy)
				CHECK(last.squash_x > 1.1f && last.squash_y < 0.9f && last.squash_z < 0.7f);
			else
				CHECK(last.squash_x < 1.1f && last.squash_y > 0.9f && last.squash_z == 1);
			float widest = 0, prev_x = last.squash_x, jump = 0;
			Flying end;
			bool fell = false;
			run(r, 0.5f, view, [&](const ReactOut &q) {
				if (q.flying.size() == 1 && !q.flying[0].stuck) {
					const Flying &f = q.flying[0];
					if (!fell)
						jump = std::fabs(f.squash_x - prev_x);
					fell = true;
					widest = std::max(widest, f.squash_x);
					end = f;
				} else if (q.flying.size() == 1) {
					prev_x = q.flying[0].squash_x;
				}
			});
			CHECK(fell && jump < 0.05f && widest <= prev_x + 1e-4f);
			CHECK(std::fabs(end.squash_x - 1) < 0.02f && std::fabs(end.squash_y - 1) < 0.02f &&
			      std::fabs(end.squash_z - 1) < 0.02f);
		}
	}
	{
		// Wobble: as it flies (both ways), flopping as it hits and dying
		// away while stuck, a little as it falls.
		React r(10);
		Throw t;
		t.picture = 0;
		t.three_d = true;
		t.stick = 2;
		r.throw_at(t);
		float lo = 0, hi = 0;
		ReactOut o;
		for (int i = 0; i < 200 && !o.hits; i++) {
			o = r.step(FRAME, view);
			if (!o.hits)
				for (const Flying &f : o.flying) {
					lo = std::min(lo, f.wobble);
					hi = std::max(hi, f.wobble);
				}
		}
		CHECK(lo < -0.5f && hi > 0.5f && finite_out(o));
		CHECK(o.flying.size() == 1 && o.flying[0].wobble > 0.9f);
		float later = 0;
		run(r, 1.5f, view, [&](const ReactOut &q) {
			CHECK(finite_out(q));
			if (q.flying.size() == 1 && q.flying[0].stuck)
				later = std::fabs(q.flying[0].wobble);
		});
		CHECK(later < 0.2f);
	}
	{
		// A stink: puffs rising one after another over a second or so -
		// none yet as it hits, growing as they rise, thinning away; stopped,
		// those still to come never come.
		React r(11);
		Throw t;
		t.picture = 0;
		t.three_d = true;
		t.stick = 2;
		PieceLook puff;
		puff.thing = 7;
		puff.way = PieceWay::Rise;
		puff.least = puff.most = 8;
		puff.size = 0.2f;
		t.look.pieces = {puff};
		r.throw_at(t);
		ReactOut o = step_to_hit(r);
		const float hit_y = o.flying[0].y;
		CHECK(o.pieces.size() <= 2 && finite_out(o));
		int most = 0;
		float highest = 1e9f, biggest = 0;
		bool thin = false;
		run(r, 1.0f, view, [&](const ReactOut &q) {
			CHECK(finite_out(q));
			most = std::max(most, (int)q.pieces.size());
			for (const Piece3D &p : q.pieces) {
				highest = std::min(highest, p.y);
				biggest = std::max(biggest, p.size);
				thin = thin || p.opacity < 0.5f;
			}
		});
		CHECK(most >= 5 && highest < hit_y - 0.15f && biggest > 0.2f && thin);
		r.stop();
		run(r, 0.25f, view, [](const ReactOut &) {});
		run(r, 2.0f, view, [&](const ReactOut &q) { CHECK(q.pieces.empty()); });
		// Left alone, all gone by RISE_OVER + its life.
		React r2(12);
		r2.throw_at(t);
		step_to_hit(r2);
		run(r2, 3.6f, view, [](const ReactOut &) {});
		CHECK(r2.step(FRAME, view).pieces.empty());
	}
}

// After 0.10.0: a move that stays (a ride) has its idle cross-faded in
// under it while the move shows whole - done as the move begins to ease
// out, so the idle before it never shows through the move easing out over
// the new one. A short move: as long as it shows whole; none: at once.
static void test_stay_swap()
{
	const ReactView view;
	for (float L : {4.0f, 2.0f, 1.0f, 0.5f}) {
		const StaySwap s = stay_swap(L, 0.5f);
		CHECK(s.fade > 0 && s.fade <= 0.5f + 1e-6f);
		CHECK(std::fabs(s.at + s.fade - (L - move_ease_out(L))) < 1e-5f);
		CHECK(s.at >= move_ease_in(L) - 1e-5f);
		// The move as React plays it shows whole all the while.
		React r;
		r.play(7, L);
		float least = 1;
		bool seen = false;
		for (int f = 0; f * FRAME < L + 0.2f; f++)
			for (const BodyMove &m : r.step(FRAME, view).body)
				if (m.move == 7 && m.time >= s.at && m.time <= s.at + s.fade) {
					least = std::min(least, m.amount);
					seen = true;
				}
		CHECK(seen && least > 0.999f);
	}
	// A 4 s ride: in from 3.05 s over half a second (0.9.0: from 3.55 s
	// over 0.45 s, as the move eased out).
	const StaySwap ride = stay_swap(4, 0.5f);
	CHECK(std::fabs(ride.at - 3.05f) < 1e-5f && std::fabs(ride.fade - 0.5f) < 1e-6f);
	// Asked for less: less.
	CHECK(std::fabs(stay_swap(4, 0.2f).fade - 0.2f) < 1e-6f && std::fabs(stay_swap(4, 0.2f).at - 3.35f) < 1e-5f);
	// No time to: swapped at once as it begins to ease out. Odd numbers:
	// at its start, at once.
	CHECK(stay_swap(4, 0).fade == 0 && std::fabs(stay_swap(4, 0).at - 3.55f) < 1e-5f);
	for (float odd :
	     {0.0f, -3.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
		const StaySwap s = stay_swap(odd, 0.5f);
		CHECK(std::isfinite(s.at) && std::isfinite(s.fade) && s.fade >= 0 && s.at >= 0);
	}
	CHECK(stay_swap(4, std::numeric_limits<float>::quiet_NaN()).fade == 0);
}

int main()
{
	test_names();
	test_faces();
	test_moves();
	test_throws();
	test_pats();
	test_sticks_and_bonks();
	test_all_at_once();
	test_effects();
	test_pieces();
	test_whole_point_wobble_rise();
	test_stay_swap();
	std::printf("unit_react: %d passed, %d failed\n", passes, failures);
	return failures ? 1 : 0;
}
