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

// Moves from another tracker app over VMC (src/face/vmc.hpp): OSC read and
// written, the humanoid's bones by Unity's names, what an app sent read,
// its turns made ours (worked out by hand from Unity's own terms), its
// face as the tracker takes a phone's, and the link with pretend apps on
// this machine.

#include "../src/face/vmc.hpp"
#include "../src/face/tracker.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <thread>
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

static bool near(double a, double b, double by)
{
	return std::fabs(a - b) <= by;
}

static bool near3(V3 a, V3 b, float by)
{
	return near(a.x, b.x, by) && near(a.y, b.y, by) && near(a.z, b.z, by);
}

static const std::string ROOT = DIREBOT_SOURCE_DIR;
static const std::string MODELS = ROOT + "/data/face/face_landmarker.task";
static const float PI = 3.14159265f;

static int ours(const char *name)
{
	for (int i = 0; i < EXPRESSIONS; i++)
		if (std::strcmp(EXPRESSION_NAMES[i], name) == 0)
			return i;
	std::fprintf(stderr, "no expression %s\n", name);
	failures++;
	return 0;
}

// ---- Making messages -------------------------------------------------------------

static OscArg text(const std::string &s)
{
	OscArg a;
	a.type = 's';
	a.text = s;
	return a;
}
static OscArg number(double v, char type = 'f')
{
	OscArg a;
	a.type = type;
	a.number = v;
	return a;
}
static OscMessage message(const std::string &address, std::vector<OscArg> args = {})
{
	OscMessage m;
	m.address = address;
	m.args = std::move(args);
	return m;
}
// A bone as an app sends it: Unity's name, its place, its turn (x y z w).
static OscMessage bone(const std::string &name, V3 at, Quat q)
{
	return message("/VMC/Ext/Bone/Pos", {text(name), number(at.x), number(at.y), number(at.z), number(q.x),
					     number(q.y), number(q.z), number(q.w)});
}
static OscMessage shape(const std::string &name, float v)
{
	return message("/VMC/Ext/Blend/Val", {text(name), number(v)});
}
static OscMessage apply()
{
	return message("/VMC/Ext/Blend/Apply");
}
// Unity's turn of `degrees` about `axis` (Quaternion.AngleAxis: the same sum
// as ours, in its own terms).
static Quat unity_turn(V3 axis, float degrees)
{
	return axis_angle(normalized(axis), degrees * PI / 180);
}

static std::vector<OscMessage> read_all(const std::string &packet, size_t *count = nullptr)
{
	std::vector<OscMessage> out;
	const size_t n = read_osc(reinterpret_cast<const uint8_t *>(packet.data()), packet.size(),
				  [&out](const OscMessage &m) { out.push_back(m); });
	if (count)
		*count = n;
	return out;
}

// ---- OSC -------------------------------------------------------------------------

static void test_osc()
{
	// Each type an app may send, written and read back.
	OscMessage m =
		message("/VMC/Ext/Bone/Pos", {text("Hips"), number(0.25), number(-1.5), number(3, 'i'), number(-7, 'i'),
					      number(0.125, 'd'), number(1234567890123.0, 'h')});
	OscArg t, f;
	t.type = 'T';
	f.type = 'F';
	m.args.push_back(t);
	m.args.push_back(f);
	m.args.push_back(text("")); // an empty text: its four zero bytes
	const std::string bytes = osc_message(m);
	CHECK(bytes.size() % 4 == 0);
	size_t n = 0;
	std::vector<OscMessage> got = read_all(bytes, &n);
	CHECK(n == 1 && got.size() == 1);
	if (got.size() == 1) {
		const OscMessage &g = got[0];
		CHECK(g.address == "/VMC/Ext/Bone/Pos");
		CHECK(g.args.size() == 10);
		if (g.args.size() == 10) {
			CHECK(g.args[0].type == 's' && g.args[0].text == "Hips");
			CHECK(g.args[1].type == 'f' && g.args[1].number == 0.25);
			CHECK(g.args[2].number == -1.5);
			CHECK(g.args[3].type == 'i' && g.args[3].number == 3);
			CHECK(g.args[4].number == -7);
			CHECK(g.args[5].type == 'd' && g.args[5].number == 0.125);
			CHECK(g.args[6].type == 'h' && g.args[6].number == 1234567890123.0);
			CHECK(g.args[7].type == 'T' && g.args[7].number == 1);
			CHECK(g.args[8].type == 'F' && g.args[8].number == 0);
			CHECK(g.args[9].type == 's' && g.args[9].text.empty());
			CHECK(g.args[1].is_number() && g.args[3].is_number() && g.args[5].is_number() &&
			      g.args[6].is_number() && !g.args[0].is_number() && !g.args[7].is_number());
		}
	}
	// Texts four bytes long and not: padded right ("Head" needs 4 more).
	for (const char *name : {"Head", "Hip", "Hips1", "LeftThumbIntermediate"}) {
		const std::vector<OscMessage> r = read_all(osc_message(message("/a", {text(name), number(1)})));
		CHECK(r.size() == 1 && r[0].args.size() == 2 && r[0].args[0].text == name && r[0].args[1].number == 1);
	}

	// A bundle of messages, and bundles in a bundle - read in order.
	const std::string one = osc_message(message("/one", {number(1, 'i')}));
	const std::string two = osc_message(message("/two", {number(2, 'i')}));
	const std::string three = osc_message(message("/three"));
	const std::string inner = osc_bundle({two, three});
	got = read_all(osc_bundle({one, inner}), &n);
	CHECK(n == 3 && got.size() == 3);
	if (got.size() == 3) {
		CHECK(got[0].address == "/one" && got[0].args[0].number == 1);
		CHECK(got[1].address == "/two" && got[2].address == "/three" && got[2].args.empty());
	}
	// Bundles eight deep are read; deeper aren't (a packet can't make it
	// dig for ever).
	std::string deep = one;
	for (int k = 0; k < 8; k++)
		deep = osc_bundle({deep});
	CHECK(read_all(deep).size() == 1);
	CHECK(read_all(osc_bundle({deep})).empty());
	// An empty bundle: nothing read.
	CHECK(read_all(osc_bundle({}), &n).empty() && n == 0);

	// The oldest OSC: an address and no types - read, no values.
	{
		std::string old = "/VMC/Ext/Blend/Apply";
		old.push_back('\0');
		while (old.size() % 4)
			old.push_back('\0');
		got = read_all(old);
		CHECK(got.size() == 1 && got[0].args.empty());
	}
	// A blob (skipped, what's after it read), an array's ends (its values
	// read as they come), a colour, a MIDI note, nil and impulse.
	{
		std::string b = "/x";
		b.append(2, '\0');
		b += ",b[if]rmNI";
		b.push_back('\0');
		while (b.size() % 4)
			b.push_back('\0');
		const uint8_t blob[] = {0, 0, 0, 5, 'h', 'e', 'l', 'l', 'o', 0, 0, 0};
		b.append(reinterpret_cast<const char *>(blob), sizeof(blob));
		const uint8_t vals[] = {0, 0, 0, 9, 0x3f, 0x80, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8};
		b.append(reinterpret_cast<const char *>(vals), sizeof(vals));
		got = read_all(b);
		CHECK(got.size() == 1);
		if (got.size() == 1) {
			const auto &a = got[0].args;
			CHECK(a.size() == 7);
			if (a.size() == 7) {
				CHECK(a[0].type == 'b');
				CHECK(a[1].type == 'i' && a[1].number == 9);
				CHECK(a[2].type == 'f' && a[2].number == 1);
				CHECK(a[3].type == 'r' && a[4].type == 'm' && a[5].type == 'N' && a[6].type == 'I');
			}
		}
	}

	// Not OSC, or cut short: nothing read (and nothing read past the end).
	const std::string good = osc_message(message("/VMC/Ext/Bone/Pos", {text("Hips"), number(1), number(2)}));
	// (Cut right after its address: that's OSC's oldest way, an address
	// with no types - read, with nothing.)
	for (size_t cut = 0; cut < good.size(); cut++) {
		const std::vector<OscMessage> r = read_all(good.substr(0, cut));
		CHECK(r.empty() || (cut == 20 && r.size() == 1 && r[0].args.empty()));
	}
	CHECK(read_all("").empty());
	CHECK(read_all(std::string("abc\0", 4)).empty()); // no '/'
	CHECK(read_all(std::string("/abc", 4)).empty());  // no end to its text
	{
		std::string bad = "/x";
		bad.append(2, '\0');
		bad += ",q";
		bad.append(2, '\0'); // a type no one knows the size of
		bad.append(4, '\0');
		CHECK(read_all(bad).empty());
	}
	{
		// A blob longer than the packet.
		std::string bad = "/x";
		bad.append(2, '\0');
		bad += ",b";
		bad.append(2, '\0');
		const uint8_t big[] = {0x7f, 0xff, 0xff, 0xff};
		bad.append(reinterpret_cast<const char *>(big), 4);
		CHECK(read_all(bad).empty());
	}
	// A bundle whose part says it's longer than what's left: the parts
	// before it read, it not.
	{
		std::string b = osc_bundle({one});
		const uint8_t huge[] = {0, 0, 0x10, 0};
		b.append(reinterpret_cast<const char *>(huge), 4);
		b += two;
		got = read_all(b);
		CHECK(got.size() == 1 && got[0].address == "/one");
	}
	// Random bytes, and good packets with random bytes changed: never a
	// crash, never read past the end (run under a sanitizer to see).
	std::mt19937 rng(7);
	const std::string full = osc_bundle({good, osc_message(shape("Joy", 0.5f)), osc_message(apply())});
	for (int round = 0; round < 3000; round++) {
		std::string p = round % 2 ? full : std::string(rng() % 64, '\0');
		for (size_t k = 0; k < p.size(); k++)
			if (round % 2 == 0 || rng() % 8 == 0)
				p[k] = (char)(rng() & 0xff);
		read_all(p);
	}
	CHECK(true);
}

// ---- The humanoid's bones --------------------------------------------------------

static void test_bones()
{
	CHECK(vmc_bone("Hips") == VMC_HIPS);
	CHECK(vmc_bone("hips") == VMC_HIPS);
	CHECK(vmc_bone("HIPS") == VMC_HIPS);
	CHECK(vmc_bone("UpperChest") == VMC_UPPER_CHEST);
	CHECK(vmc_bone("LeftUpperArm") == VMC_LEFT_UPPER_ARM);
	CHECK(vmc_bone("RightHand") == VMC_RIGHT_HAND);
	CHECK(vmc_bone("LeftToes") == VMC_LEFT_TOES);
	CHECK(vmc_bone("Jaw") == VMC_JAW);
	CHECK(vmc_bone("RightLittleDistal") == VMC_BONES - 1);
	CHECK(vmc_bone("LeftIndexProximal") == VMC_LEFT_THUMB_METACARPAL + 3);
	CHECK(vmc_bone("RightMiddleIntermediate") == VMC_RIGHT_THUMB_METACARPAL + 7);
	// Unity's thumb is VRM 1.0's a bone further in: its first bone
	// (Proximal) is 1.0's metacarpal.
	CHECK(vmc_bone("LeftThumbProximal") == VMC_LEFT_THUMB_METACARPAL);
	CHECK(vmc_bone("LeftThumbIntermediate") == VMC_LEFT_THUMB_METACARPAL + 1);
	CHECK(vmc_bone("LeftThumbDistal") == VMC_LEFT_THUMB_METACARPAL + 2);
	CHECK(vmc_bone("RightThumbProximal") == VMC_RIGHT_THUMB_METACARPAL);
	CHECK(vmc_bone("RightThumbIntermediate") == VMC_RIGHT_THUMB_METACARPAL + 1);
	CHECK(vmc_bone("RightThumbDistal") == VMC_RIGHT_THUMB_METACARPAL + 2);
	CHECK(vmc_bone("leftThumbMetacarpal") == VMC_LEFT_THUMB_METACARPAL);
	CHECK(std::string(VMC_BONE_NAMES[VMC_LEFT_THUMB_METACARPAL + 1]) == "leftThumbProximal");
	CHECK(vmc_bone("LastBone") == -1); // Unity's end marker
	CHECK(vmc_bone("") == -1);
	CHECK(vmc_bone("Tail") == -1);
	// Each name is one bone's (but 1.0's thumb proximal: Unity's wins).
	std::set<std::string> names;
	for (int i = 0; i < VMC_BONES; i++) {
		names.insert(VMC_BONE_NAMES[i]);
		const std::string n = VMC_BONE_NAMES[i];
		if (n.find("ThumbProximal") == std::string::npos)
			CHECK(vmc_bone(n) == i);
	}
	CHECK((int)names.size() == VMC_BONES);
	// The other side.
	CHECK(vmc_other_side(VMC_LEFT_HAND) == VMC_RIGHT_HAND);
	CHECK(vmc_other_side(VMC_RIGHT_UPPER_LEG) == VMC_LEFT_UPPER_LEG);
	CHECK(vmc_other_side(VMC_LEFT_EYE) == VMC_RIGHT_EYE);
	CHECK(vmc_other_side(VMC_HIPS) == VMC_HIPS);
	CHECK(vmc_other_side(VMC_HEAD) == VMC_HEAD);
	CHECK(vmc_other_side(VMC_LEFT_THUMB_METACARPAL + 5) == VMC_RIGHT_THUMB_METACARPAL + 5);
	CHECK(vmc_other_side(-1) == -1);
	for (int i = 0; i < VMC_BONES; i++)
		CHECK(vmc_other_side(vmc_other_side(i)) == i);
	// Its parts.
	CHECK(vmc_part(VMC_HIPS) == VMC_PART_BODY && vmc_part(VMC_UPPER_CHEST) == VMC_PART_BODY);
	CHECK(vmc_part(VMC_NECK) == VMC_PART_HEAD && vmc_part(VMC_RIGHT_EYE) == VMC_PART_HEAD &&
	      vmc_part(VMC_JAW) == VMC_PART_HEAD);
	CHECK(vmc_part(VMC_LEFT_UPPER_LEG) == VMC_PART_LEGS && vmc_part(VMC_RIGHT_TOES) == VMC_PART_LEGS);
	CHECK(vmc_part(VMC_LEFT_SHOULDER) == VMC_PART_ARMS && vmc_part(VMC_RIGHT_HAND) == VMC_PART_ARMS);
	CHECK(vmc_part(VMC_LEFT_THUMB_METACARPAL) == VMC_PART_FINGERS && vmc_part(VMC_BONES - 1) == VMC_PART_FINGERS);
}

// ---- What an app sent ------------------------------------------------------------

static void test_reader()
{
	VmcReader r;
	CHECK(r.sent().when[VMC_HIPS] < 0 && r.sent().shapes.empty() && r.sent().loaded == -1);
	const Quat q = normalized(Quat{0.1f, 0.2f, 0.3f, 0.9f});
	r.take(bone("Hips", {0.1f, 0.95f, -0.2f}, q), 10.0);
	r.packet_done(10.0);
	const VmcSent &s = r.sent();
	CHECK(s.when[VMC_HIPS] == 10.0);
	CHECK(near(s.at[VMC_HIPS][0], 0.1, 1e-6) && near(s.at[VMC_HIPS][1], 0.95, 1e-6) &&
	      near(s.at[VMC_HIPS][2], -0.2, 1e-6));
	CHECK(near(s.turn[VMC_HIPS][0], q.x, 1e-6) && near(s.turn[VMC_HIPS][3], q.w, 1e-6));
	CHECK(s.changes == 1 && s.messages == 1 && s.skipped == 0);
	// Whole numbers ('i') as well as floats; not a bone of ours, too few
	// values, or not a number: skipped.
	r.take(message("/VMC/Ext/Bone/Pos", {text("Head"), number(0, 'i'), number(1, 'i'), number(0, 'i'),
					     number(0, 'i'), number(0, 'i'), number(0, 'i'), number(1, 'i')}),
	       10.1);
	CHECK(s.when[VMC_HEAD] == 10.1 && s.turn[VMC_HEAD][3] == 1);
	r.take(bone("Tail", {}, {}), 10.1);
	r.take(message("/VMC/Ext/Bone/Pos", {text("Neck"), number(0), number(1)}), 10.1);
	r.take(message("/VMC/Ext/Bone/Pos",
		       {text("Neck"), number(0), number(1), number(0), text("x"), number(0), number(0), number(1)}),
	       10.1);
	r.take(message("/VMC/Ext/Bone/Pos",
		       {text("Neck"), number(0), number(1), number(0), number(NAN), number(0), number(0), number(1)}),
	       10.1);
	r.take(message("/VMC/Ext/Bone/Pos"), 10.1);
	CHECK(s.when[VMC_NECK] < 0 && s.skipped == 5);
	// Its place and clock: read, not used - not skipped.
	r.take(message("/VMC/Ext/Root/Pos",
		       {text("root"), number(0), number(0), number(0), number(0), number(0), number(0), number(1)}),
	       10.1);
	r.take(message("/VMC/Ext/T", {number(12.5)}), 10.1);
	r.take(message("/VMC/Ext/Cam", {text("Camera")}), 10.1);
	CHECK(s.skipped == 6);
	// Loaded and calibrated.
	r.take(message("/VMC/Ext/OK", {number(1, 'i'), number(3, 'i'), number(0, 'i')}), 10.1);
	CHECK(s.loaded == 1 && s.calibration == 3);
	r.take(message("/VMC/Ext/OK", {number(0, 'i')}), 10.1);
	CHECK(s.loaded == 0 && s.calibration == 3);

	// Face shapes shown together at Blend/Apply.
	r.take(shape("Joy", 0.5f), 10.2);
	r.take(shape("Blink_L", 1), 10.2);
	r.take(shape("Joy", 0.75f), 10.2); // the same again: its last value
	CHECK(s.shapes.empty() && s.shapes_when < 0);
	const uint64_t before = s.changes;
	r.take(apply(), 10.2);
	CHECK(s.shapes.size() == 2 && s.shapes_when == 10.2 && s.changes == before + 1);
	CHECK(s.shapes[0].first == "Joy" && s.shapes[0].second == 0.75f);
	CHECK(s.shapes[1].first == "Blink_L" && s.shapes[1].second == 1);
	// An app that sends Apply: values set wait for it (not shown at the
	// packet's end).
	r.take(shape("Joy", 0.1f), 10.3);
	r.packet_done(10.3);
	CHECK(s.shapes[0].second == 0.75f);
	r.take(apply(), 10.35);
	CHECK(s.shapes[0].second == 0.1f && s.shapes.size() == 2); // the others kept
	// A shape without a name, or a value that's not a number: skipped.
	const uint64_t skipped = s.skipped;
	r.take(message("/VMC/Ext/Blend/Val", {text(""), number(1)}), 10.4);
	r.take(message("/VMC/Ext/Blend/Val", {text("A"), text("1")}), 10.4);
	r.take(message("/VMC/Ext/Blend/Val", {text("A")}), 10.4);
	CHECK(s.skipped == skipped + 3);

	// An app that never sends Apply: shown as each packet ends.
	VmcReader no_apply;
	no_apply.take(shape("A", 0.6f), 5.0);
	CHECK(no_apply.sent().shapes.empty());
	no_apply.packet_done(5.0);
	CHECK(no_apply.sent().shapes.size() == 1 && no_apply.sent().shapes_when == 5.0);
	no_apply.packet_done(5.1); // nothing new: unchanged
	CHECK(no_apply.sent().shapes_when == 5.0);
	// Not more than so many shapes (a packet that isn't one).
	for (int k = 0; k < 2000; k++)
		no_apply.take(shape("s" + std::to_string(k), 0.5f), 6.0);
	no_apply.packet_done(6.0);
	CHECK(no_apply.sent().shapes.size() <= 512);

	r.clear();
	CHECK(r.sent().when[VMC_HIPS] < 0 && r.sent().shapes.empty() && r.sent().changes == 0 && r.sent().loaded == -1);
	// Cleared, it waits for Apply no more.
	r.take(shape("A", 1), 11.0);
	r.packet_done(11.0);
	CHECK(r.sent().shapes.size() == 1);
}

static void test_sends()
{
	VmcReader r;
	for (const char *b : {"Hips", "Spine", "Neck", "Head", "LeftEye", "LeftUpperArm", "RightHand",
			      "LeftIndexProximal", "LeftUpperLeg"})
		r.take(bone(b, {}, {}), 1.0);
	VmcSends s = vmc_sends(r.sent(), 1.2);
	CHECK(s.bones == 9 && s.body && s.head && s.eyes && s.arms && s.fingers && s.legs);
	CHECK(s.shapes == 0 && !s.perfect_sync);
	// Lately only: half a second after, nothing.
	s = vmc_sends(r.sent(), 1.0 + VMC_FRESH + 0.01);
	CHECK(s.bones == 0 && !s.body && !s.arms);
	// Arms only.
	VmcReader arms;
	arms.take(bone("RightUpperArm", {}, {}), 1.0);
	s = vmc_sends(arms.sent(), 1.0);
	CHECK(s.bones == 1 && s.arms && !s.body && !s.head && !s.fingers && !s.legs && !s.eyes);
	// A clock gone back a long way (the reader started again): not fresh.
	CHECK(vmc_sends(arms.sent(), -5.0).bones == 0);
	// VRM's shapes; then ARKit's - "perfect sync".
	for (const char *n : {"A", "I", "Blink", "Joy"})
		r.take(shape(n, 0.5f), 2.0);
	r.take(apply(), 2.0);
	s = vmc_sends(r.sent(), 2.1);
	CHECK(s.shapes == 4 && !s.perfect_sync && s.bones == 0);
	for (int i = 1; i < EXPRESSIONS; i++)
		r.take(shape(EXPRESSION_NAMES[i], 0), 3.0);
	r.take(apply(), 3.0);
	s = vmc_sends(r.sent(), 3.0);
	CHECK(s.shapes == 4 + EXPRESSIONS - 1 && s.perfect_sync);
}

// ---- Turns ----------------------------------------------------------------------

static void test_turns()
{
	// Unity's left arm (at rest along its -x: its left) up 60 degrees: a
	// turn of -60 about Unity's z (its front) - worked out from Unity's
	// sums: AngleAxis(-60, forward) * left = (-0.5, 0.866, 0).
	const Quat unity_up = unity_turn({0, 0, 1}, -60);
	CHECK(near3(rotate(unity_up, {-1, 0, 0}), {-0.5f, 0.8660254f, 0}, 1e-5f));
	float uq[4] = {unity_up.x, unity_up.y, unity_up.z, unity_up.w};
	const Quat ours_up = vmc_turn(uq);
	// Ours: its left along +x, up 60 - +60 about z.
	CHECK(near3(rotate(ours_up, {1, 0, 0}), {0.5f, 0.8660254f, 0}, 1e-5f));
	CHECK(near(ours_up.z, std::sin(30 * PI / 180), 1e-5) && near(ours_up.w, std::cos(30 * PI / 180), 1e-5));
	// Unity's head turned 40 degrees towards its left (-x): about y by -40.
	// Ours: its nose towards +x - its left (the picture's right).
	const Quat unity_left = unity_turn({0, 1, 0}, -40);
	CHECK(near3(rotate(unity_left, {0, 0, 1}), {-std::sin(40 * PI / 180), 0, std::cos(40 * PI / 180)}, 1e-5f));
	float lq[4] = {unity_left.x, unity_left.y, unity_left.z, unity_left.w};
	CHECK(near3(rotate(vmc_turn(lq), {0, 0, 1}), {std::sin(40 * PI / 180), 0, std::cos(40 * PI / 180)}, 1e-5f));
	// A place: its left (Unity's -x) is ours +x.
	const float left_hand[3] = {-0.4f, 1.4f, 0.1f};
	CHECK(near3(vmc_place(left_hand), {0.4f, 1.4f, 0.1f}, 1e-6f));
	// Any turn of any place, Unity's way then made ours, is ours of ours:
	// the two are the same mirror.
	std::mt19937 rng(3);
	std::uniform_real_distribution<float> u(-1, 1);
	bool all = true;
	for (int k = 0; k < 200; k++) {
		const Quat q = normalized(Quat{u(rng), u(rng), u(rng), u(rng)});
		const float p[3] = {u(rng), u(rng), u(rng)};
		const float qq[4] = {q.x, q.y, q.z, q.w};
		const V3 turned = rotate(q, {p[0], p[1], p[2]});
		const float t[3] = {turned.x, turned.y, turned.z};
		all = all && near3(rotate(vmc_turn(qq), vmc_place(p)), vmc_place(t), 1e-4f);
	}
	CHECK(all);

	// Its bones as ours: fresh ones only, normalized.
	VmcReader r;
	r.take(bone("LeftUpperArm", {-0.1f, 0, 0}, unity_up), 1.0);
	r.take(bone("Head", {0, 0.1f, 0}, unity_left), 1.0);
	r.take(bone("Hips", {0.05f, 0.9f, 0}, {0, 0, 0, 2}), 1.0); // not unit length
	r.take(bone("Neck", {}, {0, 0, 0, 0}), 1.0);               // no turn at all
	VmcBones b = vmc_bones(r.sent(), 1.1, false);
	CHECK(b.has[VMC_LEFT_UPPER_ARM] && b.has[VMC_HEAD] && b.has[VMC_HIPS]);
	CHECK(!b.has[VMC_NECK] && !b.has[VMC_RIGHT_UPPER_ARM] && !b.has[VMC_SPINE]);
	CHECK(near3(rotate(b.turn[VMC_LEFT_UPPER_ARM], {1, 0, 0}), {0.5f, 0.8660254f, 0}, 1e-5f));
	CHECK(near3(b.at[VMC_LEFT_UPPER_ARM], {0.1f, 0, 0}, 1e-6f));
	CHECK(near(b.turn[VMC_HIPS].w, 1, 1e-6));
	CHECK(near3(b.at[VMC_HIPS], {-0.05f, 0.9f, 0}, 1e-6f));
	// Mirrored: its left arm moves our right as a reflection's - up too.
	VmcBones m = vmc_bones(r.sent(), 1.1, true);
	CHECK(m.has[VMC_RIGHT_UPPER_ARM] && !m.has[VMC_LEFT_UPPER_ARM]);
	CHECK(near3(rotate(m.turn[VMC_RIGHT_UPPER_ARM], {-1, 0, 0}), {-0.5f, 0.8660254f, 0}, 1e-5f));
	CHECK(near3(m.at[VMC_RIGHT_UPPER_ARM], {-0.1f, 0, 0}, 1e-6f));
	// ...and its head turned to its left turns ours to its right.
	CHECK(near3(rotate(m.turn[VMC_HEAD], {0, 0, 1}), {-std::sin(40 * PI / 180), 0, std::cos(40 * PI / 180)},
		    1e-5f));
	CHECK(near3(m.at[VMC_HIPS], {0.05f, 0.9f, 0}, 1e-6f));
	// Stale: none.
	b = vmc_bones(r.sent(), 1.0 + VMC_FRESH + 0.1, false);
	CHECK(!b.has[VMC_LEFT_UPPER_ARM] && !b.has[VMC_HEAD]);
}

// ---- Its face ---------------------------------------------------------------------

static bool has_face(const std::vector<std::pair<std::string, float>> &faces, const std::string &name, float v)
{
	for (const auto &f : faces)
		if (f.first == name)
			return near(f.second, v, 1e-5);
	return false;
}

static void test_plain()
{
	CHECK(vmc_plain("Blink_L") == "blinkleft");
	CHECK(vmc_plain("Blink_R") == "blinkright");
	CHECK(vmc_plain("eyeBlinkLeft") == "eyeblinkleft");
	CHECK(vmc_plain("EyeBlinkLeft") == "eyeblinkleft");
	CHECK(vmc_plain("blendShape1.eyeBlinkLeft") == "eyeblinkleft");
	CHECK(vmc_plain("Fcl_EYE_Close_R") == "fcleyecloseright");
	CHECK(vmc_plain("A") == "a");
	CHECK(vmc_plain("mouth-Smile Left") == "mouthsmileleft");
	CHECK(vmc_plain("") == "");
}

static void test_face()
{
	PhoneLook look;
	std::vector<std::pair<std::string, float>> faces;
	// Nothing of its face: not found.
	{
		VmcReader r;
		r.take(bone("LeftUpperArm", {}, {}), 1.0);
		CHECK(!vmc_face(r.sent(), 1.0, look, faces) && !look.found);
	}
	// Apple's 52 ("perfect sync"): as they are - its own sides (the
	// caller swaps them for a mirror) - VRM's vowels and blinks beside
	// them not taken as well.
	{
		VmcReader r;
		for (int i = 1; i < EXPRESSIONS; i++)
			r.take(shape(EXPRESSION_NAMES[i], 0), 1.0);
		r.take(shape("eyeBlinkLeft", 0.8f), 1.0);
		r.take(shape("JawOpen", 0.6f), 1.0);         // its first letter big, as some send
		r.take(shape("mouthSmileRight", 1.5f), 1.0); // past 1: 1
		r.take(shape("tongueOut", 0.4f), 1.0);
		r.take(shape("A", 1), 1.0);
		r.take(shape("Blink_R", 1), 1.0);
		r.take(shape("Joy", 0.3f), 1.0);
		r.take(apply(), 1.0);
		CHECK(vmc_face(r.sent(), 1.1, look, faces));
		CHECK(look.found);
		CHECK(near(look.expressions[ours("eyeBlinkLeft")], 0.8, 1e-6));
		CHECK(look.expressions[ours("eyeBlinkRight")] == 0);
		CHECK(near(look.expressions[ours("jawOpen")], 0.6, 1e-6));
		CHECK(look.expressions[ours("mouthSmileRight")] == 1);
		CHECK(look.shapes == EXPRESSIONS); // the 51 and its tongue
		CHECK(has_face(faces, "tongueOut", 0.4f));
		CHECK(has_face(faces, "happy", 0.3f)); // a face: by VRM 1.0's name
		CHECK(!has_face(faces, "A", 1) && !has_face(faces, "Blink_R", 1));
		CHECK(look.expressions[0] == 0);
	}
	// VRM's own (0.x): vowels into the mouth, blinks each side, looks,
	// its faces by 1.0's names, its own shapes by theirs.
	{
		VmcReader r;
		for (auto sv : std::vector<std::pair<const char *, float>>{{"A", 1},
									   {"Blink", 0.5f},
									   {"Blink_R", 1},
									   {"LookLeft", 0.7f},
									   {"Joy", 0.2f},
									   {"Angry", 0.3f},
									   {"Sorrow", 0.4f},
									   {"Fun", 0.5f},
									   {"Surprised", 0.6f},
									   {"Neutral", 1},
									   {"Heart_Eyes", 0.9f}})
			r.take(shape(sv.first, sv.second), 2.0);
		// Its eye bones too: its looks come from the shapes it sent.
		r.take(bone("LeftEye", {}, unity_turn({0, 1, 0}, 12)), 2.0);
		r.packet_done(2.0); // (no Apply: shown at the packet's end)
		CHECK(vmc_face(r.sent(), 2.0, look, faces));
		CHECK(look.expressions[ours("jawOpen")] > 0.3f);
		CHECK(near(look.expressions[ours("eyeBlinkLeft")], 0.5, 1e-6));
		CHECK(near(look.expressions[ours("eyeBlinkRight")], 1, 1e-6));
		// Towards its left: its left eye out, its right in.
		CHECK(near(look.expressions[ours("eyeLookOutLeft")], 0.7, 1e-6));
		CHECK(near(look.expressions[ours("eyeLookInRight")], 0.7, 1e-6));
		CHECK(look.expressions[ours("eyeLookInLeft")] == 0 && look.expressions[ours("eyeLookOutRight")] == 0);
		CHECK(has_face(faces, "happy", 0.2f) && has_face(faces, "angry", 0.3f) && has_face(faces, "sad", 0.4f));
		CHECK(has_face(faces, "relaxed", 0.5f) && has_face(faces, "surprised", 0.6f));
		CHECK(has_face(faces, "Heart_Eyes", 0.9f));
		CHECK(!has_face(faces, "Neutral", 1) && !has_face(faces, "neutral", 1));
		CHECK(faces.size() == 6);
		CHECK(look.shapes == 1); // one vowel (blinks and looks aren't of the 52 it says it sent)
	}
	// VRM 1.0's names.
	{
		VmcReader r;
		r.take(shape("aa", 0), 3.0);
		r.take(shape("oh", 1), 3.0);
		r.take(shape("blinkLeft", 1), 3.0);
		r.take(shape("happy", 0.8f), 3.0);
		r.take(shape("relaxed", 0.1f), 3.0);
		r.take(apply(), 3.0);
		CHECK(vmc_face(r.sent(), 3.0, look, faces));
		CHECK(look.expressions[ours("mouthFunnel")] > 0.2f || look.expressions[ours("mouthPucker")] > 0.2f);
		CHECK(look.expressions[ours("eyeBlinkLeft")] == 1 && look.expressions[ours("eyeBlinkRight")] == 0);
		CHECK(has_face(faces, "happy", 0.8f) && has_face(faces, "relaxed", 0.1f));
	}
	// Its looks from its eye bones, when its shapes don't say: each 12
	// degrees towards its left (Unity: about y by -12) and 6 up (about x
	// by -6) - looking all the way left, half way up.
	{
		VmcReader r;
		const Quat q = unity_turn({0, 1, 0}, -12) * unity_turn({1, 0, 0}, -6);
		r.take(bone("LeftEye", {}, q), 4.0);
		r.take(bone("RightEye", {}, q), 4.0);
		r.take(bone("Head", {}, {}), 4.0);
		CHECK(vmc_face(r.sent(), 4.0, look, faces));
		CHECK(near(look.expressions[ours("eyeLookOutLeft")], 1, 0.02));
		CHECK(near(look.expressions[ours("eyeLookInRight")], 1, 0.02));
		CHECK(look.expressions[ours("eyeLookInLeft")] == 0 && look.expressions[ours("eyeLookOutRight")] == 0);
		CHECK(near(look.expressions[ours("eyeLookUpLeft")], 0.5, 0.03));
		CHECK(near(look.expressions[ours("eyeLookUpRight")], 0.5, 0.03));
		CHECK(look.expressions[ours("eyeLookDownLeft")] == 0);
		CHECK(faces.empty() && look.shapes == 0);
		// To its right and down, past how far a look goes: all the way.
		VmcReader r2;
		const Quat q2 = unity_turn({0, 1, 0}, 30) * unity_turn({1, 0, 0}, 3);
		r2.take(bone("LeftEye", {}, q2), 4.0);
		r2.take(bone("Neck", {}, {}), 4.0);
		CHECK(vmc_face(r2.sent(), 4.0, look, faces));
		CHECK(near(look.expressions[ours("eyeLookInLeft")], 1, 1e-5));
		CHECK(near(look.expressions[ours("eyeLookOutRight")], 1, 1e-5));
		CHECK(look.expressions[ours("eyeLookOutLeft")] == 0);
		CHECK(near(look.expressions[ours("eyeLookDownLeft")], 0.25, 0.03));
	}
	// Its head: its neck's turn, then its head's, in the tracker's terms
	// (yaw + towards the picture's right: its left; pitch + up; roll +
	// clockwise as the picture shows it).
	{
		VmcReader r;
		r.take(bone("Neck", {}, unity_turn({0, 1, 0}, -20)), 5.0);
		r.take(bone("Head", {}, unity_turn({0, 1, 0}, -20)), 5.0);
		CHECK(vmc_face(r.sent(), 5.0, look, faces));
		CHECK(near(look.turn.yaw, 40, 0.1) && near(look.turn.pitch, 0, 0.1) && near(look.turn.roll, 0, 0.1));
		VmcReader up;
		up.take(bone("Head", {}, unity_turn({1, 0, 0}, -20)), 5.0); // its face up
		CHECK(vmc_face(up.sent(), 5.0, look, faces));
		CHECK(near(look.turn.pitch, 20, 0.1) && near(look.turn.yaw, 0, 0.1));
		VmcReader tilt;
		// Its head towards its right shoulder (Unity's +x): the picture's
		// left - anticlockwise.
		tilt.take(bone("Head", {}, unity_turn({0, 0, 1}, -15)), 5.0);
		CHECK(near3(rotate(unity_turn({0, 0, 1}, -15), {0, 1, 0}),
			    {std::sin(15 * PI / 180), std::cos(15 * PI / 180), 0}, 1e-5f));
		CHECK(vmc_face(tilt.sent(), 5.0, look, faces));
		CHECK(near(look.turn.roll, -15, 0.1) && near(look.turn.yaw, 0, 0.1));
		// Stale: not found.
		CHECK(!vmc_face(r.sent(), 5.0 + VMC_FRESH + 0.1, look, faces));
	}
	// Through the tracker as a phone's: found, its turn and moves.
	{
		Tracker tracker;
		std::string problem;
		CHECK(tracker.load_file(MODELS, &problem));
		tracker.set_smoothing(-1);
		VmcReader r;
		r.take(bone("Neck", {}, unity_turn({0, 1, 0}, -10)), 6.0);
		r.take(bone("Head", {}, unity_turn({0, 1, 0}, -15) * unity_turn({1, 0, 0}, 8)), 6.0);
		for (int i = 1; i < EXPRESSIONS; i++)
			r.take(shape(EXPRESSION_NAMES[i], 0), 6.0);
		r.take(shape("jawOpen", 0.7f), 6.0);
		r.take(apply(), 6.0);
		CHECK(vmc_face(r.sent(), 6.0, look, faces));
		const Face &face = tracker.take(look, 6.0);
		CHECK(face.found);
		CHECK(near(face.turn.yaw, 25, 0.6));
		CHECK(near(face.turn.pitch, -8, 0.6));
		CHECK(near(face.moves[ours("jawOpen")], 0.7, 1e-5));
	}
}

// ---- The link ----------------------------------------------------------------------

// An app on this machine sending VMC.
struct App {
	int sock = -1;
	App()
	{
		sock = ::socket(AF_INET, SOCK_DGRAM, 0);
		sockaddr_in a{};
		a.sin_family = AF_INET;
		inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
		if (::bind(sock, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0) {
			::close(sock);
			sock = -1;
		}
	}
	~App()
	{
		if (sock >= 0)
			::close(sock);
	}
	void send_to(const std::string &bytes, int port)
	{
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_port = htons((uint16_t)port);
		inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
		::sendto(sock, bytes.data(), bytes.size(), 0, reinterpret_cast<sockaddr *>(&a), sizeof(a));
	}
};

// A UDP port free on this machine just now.
static int free_port()
{
	const int s = ::socket(AF_INET, SOCK_DGRAM, 0);
	sockaddr_in a{};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	::bind(s, reinterpret_cast<sockaddr *>(&a), sizeof(a));
	socklen_t len = sizeof(a);
	getsockname(s, reinterpret_cast<sockaddr *>(&a), &len);
	::close(s);
	return ntohs(a.sin_port);
}

template<class F> static bool wait_for(F done, double seconds = 3)
{
	const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
	while (std::chrono::steady_clock::now() < until) {
		if (done())
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return done();
}

// A frame as an app sends it: its whole body's bones, its face's shapes,
// Apply, OK - in one bundle.
static std::string frame(float arm_up, float joy)
{
	std::vector<std::string> parts;
	for (int i = 0; i < VMC_BONES; i++) {
		Quat q;
		if (i == VMC_LEFT_UPPER_ARM)
			q = unity_turn({0, 0, 1}, -arm_up);
		// Unity's names: big first letters, its thumbs a bone further out.
		std::string name = VMC_BONE_NAMES[i];
		name[0] = (char)std::toupper((unsigned char)name[0]);
		const size_t at = name.find("Thumb");
		if (at != std::string::npos) {
			const std::string joint = name.substr(at + 5);
			name = name.substr(0, at + 5) + (joint == "Metacarpal" ? "Proximal"
							 : joint == "Proximal" ? "Intermediate"
									       : joint);
		}
		parts.push_back(osc_message(bone(name, {0, i == VMC_HIPS ? 0.9f : 0.1f, 0}, q)));
	}
	parts.push_back(osc_message(shape("Joy", joy)));
	parts.push_back(osc_message(shape("A", 0.5f)));
	parts.push_back(osc_message(apply()));
	parts.push_back(osc_message(message("/VMC/Ext/OK", {number(1, 'i'), number(3, 'i'), number(0, 'i')})));
	parts.push_back(osc_message(message("/VMC/Ext/T", {number(1.5)})));
	return osc_bundle(parts);
}

static void test_link()
{
	const int port = free_port();
	std::shared_ptr<VmcLink> link = VmcLink::open(port);
	// One link a port, shared by every face avatar listening on it.
	CHECK(VmcLink::open(port) == link);
	CHECK(link->port() == port);
	CHECK(wait_for([&] { return link->state().listening; }));
	VmcLink::State st = link->state();
	CHECK(st.problem.empty() && st.port == port && st.packets == 0 && st.since < 0 && st.from.empty());

	// An app sends: its bones, face and OK read, from "this PC".
	App app;
	CHECK(app.sock >= 0);
	const std::string f = frame(60, 0.4f);
	CHECK(f.size() < 9000);
	app.send_to(f, port);
	CHECK(wait_for([&] { return link->state().packets >= 1; }));
	VmcSent sent;
	double now = 0;
	link->sent(sent, now);
	CHECK(sent.when[VMC_HIPS] >= 0 && now - sent.when[VMC_HIPS] < 1);
	CHECK(sent.loaded == 1 && sent.calibration == 3);
	CHECK(sent.shapes.size() == 2);
	CHECK(sent.messages == (uint64_t)VMC_BONES + 5 && sent.skipped == 0);
	const VmcSends s = vmc_sends(sent, now);
	CHECK(s.bones == VMC_BONES && s.body && s.arms && s.fingers && s.legs && s.head && s.eyes && s.shapes == 2);
	const VmcBones b = vmc_bones(sent, now, false);
	CHECK(near3(rotate(b.turn[VMC_LEFT_UPPER_ARM], {1, 0, 0}), {0.5f, 0.8660254f, 0}, 1e-4f));
	st = link->state();
	CHECK(st.from == "this PC" && st.since >= 0 && st.since < 1 && st.others == 0 && st.unread == 0);
	const uint64_t changes = link->changes();
	CHECK(changes == sent.changes && changes > 0);

	// Not VMC at all: counted, nothing changed.
	app.send_to("hello there", port);
	CHECK(wait_for([&] { return link->state().unread == 1; }));
	CHECK(link->changes() == changes);

	// A second app while the first sends: only counted (and where).
	App other;
	std::vector<std::string> just_hand = {osc_message(bone("RightHand", {}, {}))};
	other.send_to(osc_bundle(just_hand), port);
	CHECK(wait_for([&] { return link->state().others == 1; }));
	CHECK(link->state().other == "this PC");
	link->sent(sent, now);
	CHECK(sent.when[VMC_HIPS] >= 0 && sent.when[VMC_RIGHT_HAND] >= 0 && sent.shapes.size() == 2);
	CHECK(link->changes() == changes);

	// The first keeps on (30 a second for a second): how many a second.
	for (int k = 0; k < 36; k++) {
		app.send_to(frame(60, 0.4f), port);
		std::this_thread::sleep_for(std::chrono::milliseconds(33));
	}
	CHECK(wait_for([&] { return link->state().per_second > 15; }, 2));
	st = link->state();
	CHECK(st.per_second < 45);
	CHECK(st.packets >= 30);

	// The first goes quiet: after a second the other takes over - nothing
	// of the first's kept.
	std::this_thread::sleep_for(std::chrono::milliseconds(1100));
	other.send_to(osc_bundle({osc_message(bone("LeftHand", {}, {})), osc_message(shape("Blink", 1))}), port);
	CHECK(wait_for([&] {
		VmcSent s2;
		double n2;
		link->sent(s2, n2);
		return s2.when[VMC_HIPS] < 0;
	}));
	link->sent(sent, now);
	CHECK(sent.when[VMC_LEFT_HAND] >= 0 && sent.when[VMC_RIGHT_HAND] < 0 && sent.loaded == -1);
	CHECK(sent.shapes.size() == 1 && sent.shapes[0].first == "Blink");
	// Since: going up while nothing comes.
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	CHECK(link->state().since > 0.2f);

	// Let go: a new one next time.
	VmcLink *was = link.get();
	link.reset();
	std::shared_ptr<VmcLink> again = VmcLink::open(port);
	CHECK(again && again->state().packets == 0);
	(void)was;
	again.reset();
}

static void test_link_problems()
{
	// Another app has the port: said in plain words.
	const int s = ::socket(AF_INET, SOCK_DGRAM, 0);
	sockaddr_in a{};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	::bind(s, reinterpret_cast<sockaddr *>(&a), sizeof(a));
	socklen_t len = sizeof(a);
	getsockname(s, reinterpret_cast<sockaddr *>(&a), &len);
	const int taken = ntohs(a.sin_port);
	{
		VmcLink link(taken);
		CHECK(wait_for([&] { return !link.state().problem.empty(); }));
		const VmcLink::State st = link.state();
		CHECK(!st.listening);
		CHECK(st.problem.find("Another app on this PC is listening on port " + std::to_string(taken)) == 0);
		// Freed: it listens (tries again every 2 seconds).
		::close(s);
		CHECK(wait_for([&] { return link.state().listening; }, 4));
		CHECK(link.state().problem.empty());
	}
	// A port that can't be one.
	for (int bad : {0, 70000, -3}) {
		VmcLink link(bad);
		CHECK(wait_for([&] { return !link.state().problem.empty(); }));
		CHECK(link.state().problem.find("That port can't be one") == 0);
		CHECK(!link.state().listening);
	}
	// Shut at once, listening or not.
	const auto t0 = std::chrono::steady_clock::now();
	{
		VmcLink link(free_port());
	}
	CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500));
}

int main()
{
	test_osc();
	test_bones();
	test_reader();
	test_sends();
	test_turns();
	test_plain();
	test_face();
	test_link();
	test_link_problems();
	std::printf("unit_vmc: %d passed, %d failed\n", passes, failures);
	return failures ? 1 : 0;
}
