// Unit tests for a companion beside the 3D character and for moves that
// stay (0.9.0: src/face/companion.cpp, and the rig's part of it in
// src/face/rig.cpp): which of its animations are which, its comings and
// goings (never jumping, safe with odd numbers), where it goes beside the
// character, where a headpat lands on it, an idle cross-faded in and
// landing where a move ends, and a mount kept hidden until a move brings
// it out - over made-up files (tests/data/companion).
// Build and run: tools/test-link.sh (Linux, any C++17 compiler).

#include "../src/face/companion.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
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

static const float FRAME = 1.0f / 60;

static bool near(float a, float b, float within)
{
	return std::fabs(a - b) <= within;
}

static Model named(const std::vector<std::pair<std::string, float>> &anims)
{
	Model m;
	for (const auto &a : anims) {
		Animation an;
		an.name = a.first;
		an.length = a.second;
		m.animations.push_back(an);
	}
	return m;
}

static const std::string NONE[COMPANION_CLIPS] = {"", "", "", "", ""};

static bool load(const char *file, Model &m)
{
	std::string why;
	const bool ok = load_model(std::string(DIREBOT_SOURCE_DIR) + "/tests/data/companion/" + file, m, &why);
	if (!ok)
		std::fprintf(stderr, "couldn't load %s: %s\n", file, why.c_str());
	return ok;
}

// How much a clip shows this frame (0 if it doesn't), and at what time.
static float amount_of(const CompanionNow &n, int index, float *time = nullptr)
{
	float a = 0;
	for (const MoveNow &m : n.moves)
		if (m.animation == index) {
			a = m.amount;
			if (time)
				*time = m.time;
		}
	return a;
}

// How much a clip shows in the pose (each move over the ones before it as
// much as it shows: what's under one at 1 doesn't show at all).
static float shows(const CompanionNow &n, int index)
{
	float a = 0;
	for (size_t i = 0; i < n.moves.size(); i++)
		if (n.moves[i].animation == index) {
			a = n.moves[i].amount;
			for (size_t j = i + 1; j < n.moves.size(); j++)
				a *= 1 - n.moves[j].amount;
		}
	return a;
}

static bool sane(const CompanionNow &n)
{
	bool good = true;
	for (const MoveNow &m : n.moves)
		good = good && std::isfinite(m.time) && std::isfinite(m.amount) && m.amount > 0 && m.amount <= 1 &&
		       m.time >= 0 && m.animation >= 0;
	return good;
}

static void test_names()
{
	CHECK(std::string(companion_clip_name(CLIP_COME)) == "come");
	CHECK(std::string(companion_clip_name(CLIP_SCARED)) == "scared");
	CHECK(std::string(companion_clip_name(-1)).empty());
	CHECK(std::string(companion_clip_name(COMPANION_CLIPS)).empty());
	CHECK(std::string(companion_phase_name(CompanionPhase::Away)) == "away");
	CHECK(std::string(companion_phase_name(CompanionPhase::Coming)) == "coming");
	CHECK(std::string(companion_phase_name(CompanionPhase::Out)) == "out");
	CHECK(std::string(companion_phase_name(CompanionPhase::Going)) == "going");
}

static void test_clips()
{
	// Its own names, any case.
	{
		const Model m = named({{"Idle", 6}, {"COME", 3.5f}, {"pet", 3}, {"Go", 3}, {"scared", 2.5f}});
		const CompanionClips c = companion_clips(m, NONE);
		CHECK(c[CLIP_IDLE].index == 0 && near(c[CLIP_IDLE].length, 6, 1e-6f));
		CHECK(c[CLIP_COME].index == 1);
		CHECK(c[CLIP_PET].index == 2);
		CHECK(c[CLIP_GO].index == 3);
		CHECK(c[CLIP_SCARED].index == 4 && near(c[CLIP_SCARED].length, 2.5f, 1e-6f));
	}
	// The usual words for them, among others in their names.
	{
		const Model m = named({{"Armature|Wolf_Arrive", 3},
				       {"SitLoop", 5},
				       {"happy_wiggle", 2},
				       {"wolf.leave", 3},
				       {"Bolt", 2}});
		const CompanionClips c = companion_clips(m, NONE);
		CHECK(c[CLIP_COME].index == 0);
		CHECK(c[CLIP_IDLE].index == 1);
		CHECK(c[CLIP_PET].index == 2);
		CHECK(c[CLIP_GO].index == 3);
		CHECK(c[CLIP_SCARED].index == 4);
	}
	// A name that's just the word wins over one with it among others, and
	// each animation is only one of them ("run_in" is its scared here, as
	// "come" is its come).
	{
		const Model m = named({{"run_in", 2}, {"come", 3}, {"idle", 4}});
		const CompanionClips c = companion_clips(m, NONE);
		CHECK(c[CLIP_COME].index == 1);
		CHECK(c[CLIP_SCARED].index == 0);
		CHECK(c[CLIP_IDLE].index == 2);
		CHECK(c[CLIP_GO].index == -1 && c[CLIP_PET].index == -1);
	}
	// Words inside other words don't count ("spin" isn't "in", "outfit"
	// isn't "out").
	{
		const Model m = named({{"spin", 2}, {"outfit", 2}});
		const CompanionClips c = companion_clips(m, NONE);
		CHECK(c[CLIP_COME].index == -1 && c[CLIP_GO].index == -1);
	}
	// As asked (any case), over the usual names; asked for one it hasn't:
	// worked out.
	{
		const Model m = named({{"come", 3}, {"idle", 4}, {"Trot_In", 2}, {"Lie_Down", 6}});
		const std::string asked[COMPANION_CLIPS] = {"trot_in", "LIE_DOWN", "", "", "nope"};
		const CompanionClips c = companion_clips(m, asked);
		CHECK(c[CLIP_COME].index == 2);
		CHECK(c[CLIP_IDLE].index == 3);
		CHECK(c[CLIP_SCARED].index == -1);
	}
	// Its only animation is its idle, whatever it's called.
	{
		const Model m = named({{"Take 001", 4}});
		const CompanionClips c = companion_clips(m, NONE);
		CHECK(c[CLIP_IDLE].index == 0);
		CHECK(c[CLIP_COME].index == -1);
	}
	// None: nothing, and a null list of names is safe.
	{
		const Model m;
		const CompanionClips c = companion_clips(m, nullptr);
		for (int k = 0; k < COMPANION_CLIPS; k++)
			CHECK(c[k].index == -1);
	}
	// A broken length is 0, not NaN.
	{
		const Model m = named({{"idle", std::numeric_limits<float>::quiet_NaN()}});
		CHECK(companion_clips(m, NONE)[CLIP_IDLE].length == 0);
	}
}

static CompanionClips wolf_clips()
{
	return companion_clips(named({{"come", 3.5f}, {"idle", 6}, {"pet", 3}, {"go", 3}, {"scared", 2.5f}}), NONE);
}

// While it's shown, how much each clip shows changes smoothly frame to
// frame, and a clip's time only goes on by a frame (or loops round).
struct Smooth {
	CompanionNow last;
	bool first = true;
	float most_amount = 0, most_time = 0;
	void see(const CompanionNow &n)
	{
		if (!first && last.shown && n.shown)
			for (int i = 0; i < COMPANION_CLIPS; i++) {
				float t0 = 0, t1 = 0;
				const float a0 = amount_of(last, i, &t0), a1 = amount_of(n, i, &t1);
				most_amount = std::max(most_amount, std::fabs(shows(n, i) - shows(last, i)));
				if (a0 > 0 && a1 > 0 && t1 >= t0)
					most_time = std::max(most_time, t1 - t0);
			}
		last = n;
		first = false;
	}
};

static void test_coming()
{
	CompanionPlayer p;
	p.set_clips(wolf_clips());
	CHECK(p.phase() == CompanionPhase::Away);
	CompanionNow n = p.step(FRAME);
	CHECK(!n.shown && !n.camera && n.moves.empty());
	CHECK(p.come());
	CHECK(!p.come()); // coming already
	CHECK(p.phase() == CompanionPhase::Coming);
	Smooth sm;
	n = p.step(0);
	sm.see(n);
	CHECK(n.shown && n.busy && n.camera);
	CHECK(near(amount_of(n, CLIP_COME), 1, 1e-6f));
	CHECK(amount_of(n, CLIP_IDLE) == 0);
	// Its idle comes in over its come where the come begins to ease out,
	// from where its loop ends - so it lands on the idle's first moment
	// just as the come ends.
	const float from = 3.5f - move_ease_out(3.5f);
	bool idle_seen = false;
	float idle_at_end = -1;
	for (int f = 0; f < 400 && p.phase() == CompanionPhase::Coming; f++) {
		n = p.step(FRAME);
		sm.see(n);
		CHECK(sane(n));
		if (p.phase() != CompanionPhase::Coming)
			break;
		float it = 0;
		const float a = amount_of(n, CLIP_IDLE, &it);
		if (p.time() < from - 2 * FRAME)
			CHECK(a == 0);
		if (a > 0 && !idle_seen) {
			idle_seen = true;
			CHECK(it > 6 - move_ease_out(3.5f) - 2 * FRAME);
		}
		if (a > 0)
			idle_at_end = it;
	}
	CHECK(idle_seen);
	CHECK(p.phase() == CompanionPhase::Out);
	// The idle's last moment (or, as near as makes no odds, its first again),
	// then its first.
	CHECK(idle_at_end >= 0 && std::min(idle_at_end, 6 - idle_at_end) < 2 * FRAME);
	n = p.step(FRAME);
	sm.see(n);
	float it = -1;
	CHECK(near(amount_of(n, CLIP_IDLE, &it), 1, 1e-6f));
	CHECK(it < 3 * FRAME);
	CHECK(amount_of(n, CLIP_COME) == 0);
	CHECK(n.shown && !n.busy && n.camera);
	CHECK(!p.come()); // here already
	CHECK(sm.most_amount < 0.06f);
	CHECK(sm.most_time < 1.5f * FRAME);
	// Its idle over and over.
	for (int f = 0; f < 60 * 13; f++) {
		n = p.step(FRAME);
		sm.see(n);
	}
	CHECK(p.phase() == CompanionPhase::Out);
	CHECK(near(amount_of(n, CLIP_IDLE, &it), 1, 1e-6f) && it >= 0 && it < 6);
}

static void test_petting()
{
	CompanionPlayer p;
	p.set_clips(wolf_clips());
	CHECK(!p.pet()); // not here
	p.come();
	p.step(FRAME);
	CHECK(!p.pet()); // still coming
	p.come(true);
	CHECK(p.phase() == CompanionPhase::Out);
	CHECK(p.pet());
	Smooth sm;
	CompanionNow n = p.step(0);
	sm.see(n);
	CHECK(n.petting);
	CHECK(amount_of(n, CLIP_PET) == 0); // eased in from nothing
	float most = 0;
	for (int f = 0; f < 60; f++) {
		n = p.step(FRAME);
		sm.see(n);
		most = std::max(most, amount_of(n, CLIP_PET));
	}
	CHECK(near(most, 1, 1e-4f));
	CHECK(near(amount_of(n, CLIP_IDLE), 1, 1e-6f)); // under it all along
	CHECK(p.pet());                                 // again while it plays: once more after
	for (int f = 0; f < 60 * 3; f++) {
		n = p.step(FRAME);
		sm.see(n);
	}
	CHECK(n.petting);
	for (int f = 0; f < 60 * 3; f++) {
		n = p.step(FRAME);
		sm.see(n);
	}
	CHECK(!n.petting);
	CHECK(amount_of(n, CLIP_PET) == 0);
	CHECK(sm.most_amount < 0.12f);
	// No "pet" of its own: patted all the same (the hand pats it), nothing
	// plays.
	CompanionPlayer q;
	q.set_clips(companion_clips(named({{"idle", 4}}), NONE));
	q.come();
	CHECK(q.phase() == CompanionPhase::Out); // no come: straight there
	CHECK(q.pet());
	CHECK(!q.step(FRAME).petting);
}

static void test_going()
{
	CompanionPlayer p;
	p.set_clips(wolf_clips());
	CHECK(!p.go()); // not here
	p.come(true);
	CHECK(p.go());
	CHECK(!p.go()); // going already
	CHECK(p.phase() == CompanionPhase::Going);
	CHECK(!p.pet());
	Smooth sm;
	CompanionNow n = p.step(0);
	sm.see(n);
	CHECK(n.busy && n.camera);
	CHECK(amount_of(n, CLIP_GO) == 0);
	CHECK(near(amount_of(n, CLIP_IDLE), 1, 1e-6f));
	bool camera_off = false;
	for (int f = 0; f < 400 && p.phase() == CompanionPhase::Going; f++) {
		n = p.step(FRAME);
		sm.see(n);
		CHECK(sane(n));
		if (p.phase() == CompanionPhase::Going && p.time() < 1.5f - FRAME)
			CHECK(n.camera);
		if (!n.camera)
			camera_off = true;
	}
	CHECK(camera_off); // the camera's back half way through its go
	CHECK(p.phase() == CompanionPhase::Away);
	n = p.step(FRAME);
	CHECK(!n.shown && n.moves.empty() && !n.camera);
	CHECK(sm.most_amount < 0.12f);
	// Scared: its scared clip.
	p.come(true);
	CHECK(p.go(true));
	p.step(1.0f);
	n = p.step(0);
	CHECK(amount_of(n, CLIP_SCARED) > 0 && amount_of(n, CLIP_GO) == 0);
	// Scared with none of its own: its go.
	CompanionPlayer q;
	q.set_clips(companion_clips(named({{"idle", 4}, {"go", 2}}), NONE));
	q.come(true);
	CHECK(q.go(true));
	q.step(1.0f);
	CHECK(amount_of(q.step(0), 1) > 0);
	// Neither: straight away.
	CompanionPlayer r;
	r.set_clips(companion_clips(named({{"idle", 4}}), NONE));
	r.come(true);
	CHECK(r.go());
	CHECK(r.phase() == CompanionPhase::Away);
	// At once.
	p.reset();
	p.come(true);
	CHECK(p.go(false, true));
	CHECK(p.phase() == CompanionPhase::Away);
	CHECK(!p.go(false, true));
}

static void test_changing_its_mind()
{
	// Asked to go as it comes: it goes once it's here (no jump from half
	// way in) - or stays after all, asked to come again.
	CompanionPlayer p;
	p.set_clips(wolf_clips());
	p.come();
	p.step(1.0f);
	CHECK(p.go(true));
	CHECK(!p.go()); // asked already
	CHECK(p.phase() == CompanionPhase::Coming);
	for (int f = 0; f < 60 * 3 && p.phase() == CompanionPhase::Coming; f++)
		p.step(FRAME);
	CHECK(p.phase() == CompanionPhase::Going);
	CHECK(amount_of(p.step(0.5f), CLIP_SCARED) > 0);
	p.reset();
	p.come();
	p.step(1.0f);
	CHECK(p.go());
	CHECK(p.come()); // stays after all
	for (int f = 0; f < 60 * 3; f++)
		p.step(FRAME);
	CHECK(p.phase() == CompanionPhase::Out);
	// Asked to come as it goes: it comes back once it's gone, the camera
	// staying out for it.
	CHECK(p.go());
	p.step(0.5f);
	CHECK(p.come());
	CHECK(!p.come());
	bool camera_off = false, came_back = false;
	for (int f = 0; f < 60 * 8; f++) {
		const CompanionNow n = p.step(FRAME);
		camera_off = camera_off || !n.camera;
		came_back = came_back || p.phase() == CompanionPhase::Coming;
	}
	CHECK(!camera_off);
	CHECK(came_back);
	CHECK(p.phase() == CompanionPhase::Out);
	// ...or stays gone after all.
	CHECK(p.go());
	p.step(0.5f);
	CHECK(p.come());
	CHECK(p.go());
	for (int f = 0; f < 60 * 4; f++)
		p.step(FRAME);
	CHECK(p.phase() == CompanionPhase::Away);
	// One long step finishes more than one thing.
	p.come();
	p.go();
	p.step(1.0f);
	p.step(1.0f);
	p.step(1.0f);
	p.step(1.0f);
	CHECK(p.phase() == CompanionPhase::Going);
	p.step(1.0f);
	p.step(1.0f);
	CHECK(p.phase() == CompanionPhase::Going); // 3.5 s coming, then 3 going
	p.step(1.0f);
	CHECK(p.phase() == CompanionPhase::Away);
}

static void test_odd_numbers()
{
	CompanionPlayer p;
	p.set_clips(wolf_clips());
	p.come();
	const float odd[] = {std::numeric_limits<float>::quiet_NaN(),
			     std::numeric_limits<float>::infinity(),
			     -std::numeric_limits<float>::infinity(),
			     -5,
			     1e30f,
			     0};
	for (float dt : odd) {
		const CompanionNow n = p.step(dt);
		CHECK(sane(n));
		CHECK(std::isfinite(p.time()));
	}
	// Clips with no length: never stuck coming or going.
	CompanionPlayer q;
	q.set_clips(companion_clips(named({{"come", 0}, {"idle", 0}, {"go", 0}}), NONE));
	q.come();
	CHECK(q.phase() == CompanionPhase::Out);
	CHECK(sane(q.step(FRAME)));
	q.go();
	CHECK(q.phase() == CompanionPhase::Away);
	// No clips at all: there, sitting as made; then gone.
	CompanionPlayer r;
	CHECK(r.come());
	CompanionNow n = r.step(FRAME);
	CHECK(n.shown && n.moves.empty());
	CHECK(r.go());
	CHECK(!r.step(FRAME).shown);
}

// Where its box ends up in the world.
static void placed_box(const M4 &m, const Bounds &b, V3 &lo, V3 &hi)
{
	lo = {1e30f, 1e30f, 1e30f};
	hi = {-1e30f, -1e30f, -1e30f};
	for (int i = 0; i < 8; i++) {
		const V3 c = point(m, {(i & 1) ? b.high.x : b.low.x, (i & 2) ? b.high.y : b.low.y,
				       (i & 4) ? b.high.z : b.low.z});
		lo = {std::min(lo.x, c.x), std::min(lo.y, c.y), std::min(lo.z, c.z)};
		hi = {std::max(hi.x, c.x), std::max(hi.y, c.y), std::max(hi.z, c.z)};
	}
}
static float handed(const M4 &a)
{
	const V3 x{a.m[0], a.m[1], a.m[2]}, y{a.m[4], a.m[5], a.m[6]}, z{a.m[8], a.m[9], a.m[10]};
	return dot(x, cross(y, z)) < 0 ? -1.0f : 1.0f;
}

static void test_place()
{
	// A character 1.8 tall standing at the middle, facing +z; a pet 1.2
	// tall sitting, its middle off its origin.
	Bounds person;
	person.low = {-0.3f, 0, -0.15f};
	person.high = {0.3f, 1.8f, 0.15f};
	person.found = true;
	Bounds pet;
	pet.low = {-0.2f, 0, -0.6f};
	pet.high = {0.2f, 1.2f, 0.2f};
	pet.found = true;
	CompanionPlace p; // 70% of its height, the picture's left, 0.32 heights out
	M4 m = companion_place(pet, Quat{}, person, Quat{}, M4(), p);
	V3 lo, hi;
	placed_box(m, pet, lo, hi);
	CHECK(near(lo.y, 0, 1e-4f));                          // on the character's floor
	CHECK(near(hi.y - lo.y, 0.7f * 1.8f, 1e-4f));         // sized to it
	CHECK(near((lo.x + hi.x) / 2, -0.32f * 1.8f, 1e-4f)); // at its side, the picture's left
	CHECK(near((lo.z + hi.z) / 2, 0, 1e-4f));
	CHECK(handed(m) > 0);
	// Its other side: mirrored, there.
	p.side = 1;
	m = companion_place(pet, Quat{}, person, Quat{}, M4(), p);
	placed_box(m, pet, lo, hi);
	CHECK(near((lo.x + hi.x) / 2, 0.32f * 1.8f, 1e-4f));
	CHECK(near(lo.y, 0, 1e-4f));
	CHECK(handed(m) < 0);
	// Towards the camera, and bigger.
	p.side = -1;
	p.forward = 0.1f;
	p.size = 1;
	m = companion_place(pet, Quat{}, person, Quat{}, M4(), p);
	placed_box(m, pet, lo, hi);
	CHECK(near((lo.z + hi.z) / 2, 0.18f, 1e-4f));
	CHECK(near(hi.y - lo.y, 1.8f, 1e-4f));
	// Turned towards the character: its front (+z) turns towards +x on the
	// picture's left, -x on its right.
	p = CompanionPlace();
	p.turn = 30;
	m = companion_place(pet, Quat{}, person, Quat{}, M4(), p);
	CHECK(direction(m, {0, 0, 1}).x > 0.1f);
	p.side = 1;
	m = companion_place(pet, Quat{}, person, Quat{}, M4(), p);
	CHECK(direction(m, {0, 0, 1}).x < -0.1f);
	// A character facing the other way in its file (VRM 0.x), put in the
	// world bigger and moved: its side is still the picture's left.
	const Quat back = axis_angle({0, 1, 0}, 3.14159265f);
	Bounds turned = person;
	turned.low = {-0.3f, 0, -0.15f};
	const M4 world = moved({1, 2, 3}) * scaled(0.5f) * matrix_of(back);
	p = CompanionPlace();
	m = companion_place(pet, Quat{}, turned, back, world, p);
	placed_box(m, pet, lo, hi);
	CHECK(near(lo.y, 2, 1e-4f));
	CHECK(near(hi.y - lo.y, 0.7f * 0.9f, 1e-4f));
	CHECK(near((lo.x + hi.x) / 2, 1 - 0.32f * 0.9f, 1e-4f));
	// A pet facing the other way in its file: turned to face +z first.
	m = companion_place(pet, back, person, Quat{}, M4(), CompanionPlace());
	CHECK(direction(m, {0, 0, -1}).z > 0.5f);
	// Kept sensible.
	CompanionPlace odd;
	odd.size = std::numeric_limits<float>::quiet_NaN();
	odd.side = 0;
	odd.made_for = 7;
	odd.distance = 100;
	odd.forward = -std::numeric_limits<float>::infinity();
	odd.turn = 1000;
	const CompanionPlace s = sensible_place(odd);
	CHECK(s.size == 0.7f && s.side == -1 && s.made_for == 1 && s.distance == 3 && s.forward == 0 && s.turn == 180);
	CHECK(sensible_place(CompanionPlace()).size == 0.7f);
	odd.size = 99;
	CHECK(sensible_place(odd).size == 3);
	m = companion_place(Bounds(), Quat{}, Bounds(), Quat{}, M4(), odd);
	for (float v : m.m)
		CHECK(std::isfinite(v));
}

static void test_wolf_file()
{
	Model m;
	if (!load("wolf-test.glb", m)) {
		failures++;
		return;
	}
	CHECK(!m.is_vrm());
	const CompanionClips c = companion_clips(m, NONE);
	for (int k = 0; k < COMPANION_CLIPS; k++)
		CHECK(c[k].index >= 0 && m.animations[c[k].index].name == companion_clip_name(k));
	CHECK(near(c[CLIP_COME].length, 3.5f, 1e-3f) && near(c[CLIP_IDLE].length, 6, 1e-3f));
	const int pat = companion_pat_node(m);
	CHECK(pat >= 0 && m.nodes[pat].name == "pat");
	CHECK(companion_facing(m).w == 1);
	// Its rig, no springs: its box as it sits.
	RigOptions ro;
	ro.facing = companion_facing(m);
	ro.springs = false;
	ro.arms_down = false;
	Rig rig(std::make_shared<Model>(m), ro);
	CHECK(rig.springs() == 0 && !rig.auto_chain());
	const Bounds sits = rig.rest_bounds_in(c[CLIP_IDLE].index);
	CHECK(sits.found);
	CHECK(near(sits.low.y, 0, 0.01f) && near(sits.high.y, 1.27f, 0.02f));
	CHECK(rig.animation() == -1);
	// Coming: its come moves it in from the picture's left, ending where
	// it sits.
	CompanionPlayer p;
	p.set_clips(c);
	p.come();
	Drive d;
	d.look = false;
	d.expressions = false;
	auto middle_x = [&rig]() {
		float lo = 1e30f, hi = -1e30f;
		for (const Rig::Part &part : rig.parts())
			for (const V3 &v : part.pos) {
				lo = std::min(lo, v.x);
				hi = std::max(hi, v.x);
			}
		return (lo + hi) / 2;
	};
	d.body = p.step(0).moves;
	rig.pose(d);
	CHECK(middle_x() < -2);
	float last = middle_x(), most_step = 0;
	for (int f = 0; f < 60 * 4; f++) {
		d.body = p.step(FRAME).moves;
		d.dt = FRAME;
		rig.pose(d);
		const float x = middle_x();
		most_step = std::max(most_step, std::fabs(x - last));
		last = x;
	}
	CHECK(p.phase() == CompanionPhase::Out);
	CHECK(near(last, (sits.low.x + sits.high.x) / 2, 0.01f));
	CHECK(most_step < 0.05f); // never a jump
	// Placed beside a character: on its floor, its height's share.
	Bounds person;
	person.low = {-0.3f, 0, -0.15f};
	person.high = {0.3f, 1.8f, 0.15f};
	person.found = true;
	d.place = companion_place(sits, ro.facing, person, Quat{}, M4(), CompanionPlace());
	d.body = p.step(0).moves;
	rig.pose(d);
	float lo_y = 1e30f, hi_y = -1e30f;
	for (const Rig::Part &part : rig.parts())
		for (const V3 &v : part.pos) {
			lo_y = std::min(lo_y, v.y);
			hi_y = std::max(hi_y, v.y);
		}
	CHECK(near(lo_y, 0, 0.02f));
	CHECK(near(hi_y - lo_y, 0.7f * 1.8f, 0.05f));
	// Mirrored for the other side, its parts show their fronts still.
	CompanionPlace other;
	other.side = 1;
	d.place = companion_place(sits, ro.facing, person, Quat{}, M4(), other);
	rig.pose(d);
	bool all_mirrored = !rig.parts().empty();
	for (const Rig::Part &part : rig.parts())
		all_mirrored = all_mirrored && part.handed < 0;
	CHECK(all_mirrored);
	// Its pat node is on top of its head, as posed.
	const V3 at = move_of(rig.world()[pat]);
	CHECK(at.y > hi_y - 0.15f && at.y < hi_y + 0.01f);
}

static void test_pat_node()
{
	Model m;
	Node a, b, c;
	a.name = "root";
	b.name = "mixamorig:Head";
	c.name = "HeadTop_End";
	m.nodes = {a, b, c};
	CHECK(companion_pat_node(m) == 1);
	m.nodes[2].name = "head";
	CHECK(companion_pat_node(m) == 2); // just "head" first
	m.nodes[0].name = "PAT";
	CHECK(companion_pat_node(m) == 0); // its own "pat" before all
	Model none;
	Node n;
	n.name = "body";
	none.nodes = {n};
	CHECK(companion_pat_node(none) == -1);
	// A VRM's head bone.
	Model v;
	if (load("dragon-test.vrm", v)) {
		const int h = companion_pat_node(v);
		CHECK(h >= 0 && h == v.bone("head"));
	} else {
		failures++;
	}
	Model back;
	back.faces_back = true;
	CHECK(std::fabs(companion_facing(back).y) > 0.99f);
}

// The character's own idle cross-faded to another, landing where asked.
static void test_cross_fade()
{
	Model m;
	if (!load("dragon-test.vrm", m)) {
		failures++;
		return;
	}
	const int idle = animation_named(m, "idle"), ride = animation_named(m, "dragon_idle"),
		  come = animation_named(m, "dragon_come");
	CHECK(idle >= 0 && ride >= 0 && come >= 0);
	RigOptions ro;
	ro.animation = idle;
	Rig rig(std::make_shared<Model>(m), ro);
	Drive d;
	d.look = false;
	d.expressions = false;
	d.dt = FRAME;
	for (int f = 0; f < 30; f++)
		rig.pose(d);
	// Swapped straight away (as it always was): no fade.
	rig.set_animation(ride);
	CHECK(rig.animation() == ride && !rig.animation_fading() && rig.animation_time() == 0);
	rig.set_animation(idle, 0.5f, 1.25f);
	CHECK(rig.animation() == idle && rig.animation_fading());
	CHECK(near(rig.animation_time(), 1.25f, 1e-5f));
	for (int f = 0; f < 40; f++)
		rig.pose(d);
	CHECK(!rig.animation_fading());
	// Started past its end: looped round. The same one again: nothing.
	rig.set_animation(ride, 0, 9);
	CHECK(near(rig.animation_time(), 1, 1e-4f));
	rig.set_animation(ride, 0.5f, 3);
	CHECK(near(rig.animation_time(), 1, 1e-4f) && !rig.animation_fading());
	// A fade changes the dragon's place smoothly (no jump), frame to frame.
	rig.set_animation(idle);
	rig.pose(d);
	const int root = [&m]() {
		for (size_t i = 0; i < m.nodes.size(); i++)
			if (m.nodes[i].name == "dragon_root")
				return (int)i;
		return -1;
	}();
	CHECK(root >= 0);
	rig.set_animation(ride, 0.45f, 0);
	float last = move_of(rig.world()[root]).y, most = 0;
	for (int f = 0; f < 40; f++) {
		rig.pose(d);
		const float y = move_of(rig.world()[root]).y;
		most = std::max(most, std::fabs(y - last));
		last = y;
	}
	CHECK(most < 0.06f);
	CHECK(near(last, 0.95f - 0.55f, 0.06f)); // under it, riding
	(void)come;
}

// A mount kept hidden in its rider's file: not worked out or drawn at
// rest, and not in its box - until a move brings it out.
static void test_hidden_parts()
{
	Model m;
	if (!load("dragon-test.vrm", m)) {
		failures++;
		return;
	}
	const int idle = animation_named(m, "idle"), ride = animation_named(m, "dragon_idle");
	RigOptions ro;
	ro.animation = idle;
	Rig rig(std::make_shared<Model>(m), ro);
	Drive d;
	d.look = false;
	d.expressions = false;
	rig.pose(d);
	int hidden = 0, shown = 0;
	for (const Rig::Part &p : rig.parts()) {
		const std::string &name = m.nodes[p.node].name;
		if (name == "dragon") {
			CHECK(p.hidden && p.pos.empty());
			hidden++;
		} else {
			CHECK(!p.hidden && !p.pos.empty());
			shown++;
		}
	}
	CHECK(hidden == 1 && shown >= 1);
	const Bounds stands = rig.rest_bounds();
	CHECK(stands.found);
	CHECK(stands.high.x - stands.low.x < 1.6f); // its arms, not the dragon's wings
	// Riding: it's there, and in the box as it rides.
	const Bounds rides = rig.rest_bounds_in(ride);
	CHECK(rides.high.x - rides.low.x > 4);
	CHECK(rig.animation() == idle); // measuring changed nothing
	Drive with;
	with.look = false;
	with.expressions = false;
	MoveNow mv;
	mv.animation = ride;
	mv.amount = 1;
	mv.time = 1;
	with.body.push_back(mv);
	rig.pose(with);
	for (const Rig::Part &p : rig.parts())
		if (m.nodes[p.node].name == "dragon")
			CHECK(!p.hidden && !p.pos.empty());
	// A rig made in its riding idle shows it from the start.
	RigOptions riding = ro;
	riding.animation = ride;
	Rig r2(std::make_shared<Model>(m), riding);
	r2.pose(d);
	for (const Rig::Part &p : r2.parts())
		if (m.nodes[p.node].name == "dragon")
			CHECK(!p.hidden);
}

int main()
{
	test_names();
	test_clips();
	test_coming();
	test_petting();
	test_going();
	test_changing_its_mind();
	test_odd_numbers();
	test_place();
	test_wolf_file();
	test_pat_node();
	test_cross_fade();
	test_hidden_parts();
	std::printf("unit_companion: %d passed, %d failed\n", passes, failures);
	return failures ? 1 : 0;
}
