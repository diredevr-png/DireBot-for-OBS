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

// Your face from a phone (src/face/phone.hpp): the apps' packets read,
// their names and sides made ours, the face through the tracker as a
// camera's (Tracker::take), and the link talking to a pretend phone on
// this machine - VTube Studio's way and iFacialMocap's.

#include "../src/face/phone.hpp"
#include "../src/face/tracker.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <thread>
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

static const std::string ROOT = DIREBOT_SOURCE_DIR;
static const std::string MODELS = ROOT + "/data/face/face_landmarker.task";

static int ours(const char *name)
{
	for (int i = 0; i < EXPRESSIONS; i++)
		if (std::strcmp(EXPRESSION_NAMES[i], name) == 0)
			return i;
	std::fprintf(stderr, "no expression %s\n", name);
	std::abort();
}

// Apple's 52, as VTube Studio's iPhone app names them.
static const char *const APPLE[] = {
	"EyeBlinkLeft",       "EyeLookDownLeft",     "EyeLookInLeft",     "EyeLookOutLeft",    "EyeLookUpLeft",
	"EyeSquintLeft",      "EyeWideLeft",         "EyeBlinkRight",     "EyeLookDownRight",  "EyeLookInRight",
	"EyeLookOutRight",    "EyeLookUpRight",      "EyeSquintRight",    "EyeWideRight",      "JawForward",
	"JawRight",           "JawLeft",             "JawOpen",           "MouthClose",        "MouthFunnel",
	"MouthPucker",        "MouthLeft",           "MouthRight",        "MouthSmileLeft",    "MouthSmileRight",
	"MouthFrownLeft",     "MouthFrownRight",     "MouthDimpleLeft",   "MouthDimpleRight",  "MouthStretchLeft",
	"MouthStretchRight",  "MouthRollLower",      "MouthRollUpper",    "MouthShrugLower",   "MouthShrugUpper",
	"MouthPressLeft",     "MouthPressRight",     "MouthLowerDownLeft", "MouthLowerDownRight", "MouthUpperUpLeft",
	"MouthUpperUpRight",  "BrowDownLeft",        "BrowDownRight",     "BrowInnerUp",       "BrowOuterUpLeft",
	"BrowOuterUpRight",   "CheekPuff",           "CheekSquintLeft",   "CheekSquintRight",  "NoseSneerLeft",
	"NoseSneerRight"};

static void test_names()
{
	// Every one of Apple's 51 (its tongue aside) is one of ours, each a
	// different one.
	std::set<int> seen;
	for (const char *name : APPLE) {
		const int i = phone_shape(name);
		CHECK(i > 0);
		seen.insert(i);
	}
	CHECK(seen.size() == 51);
	CHECK(phone_shape("TongueOut") == -1);
	CHECK(phone_shape("tongueOut") == -1);
	CHECK(phone_shape("_neutral") == -1);
	CHECK(phone_shape("") == -1);
	CHECK(phone_shape("hapihapi") == -1);
	// Apple's left is Google's right (your right eye is Apple's
	// eyeBlinkLeft), whichever way the name's written.
	for (const char *name : {"EyeBlinkLeft", "eyeBlinkLeft", "eyeBlink_L", "EYEBLINK_L"})
		CHECK(phone_shape(name) == ours("eyeBlinkRight"));
	for (const char *name : {"EyeBlinkRight", "eyeBlinkRight", "eyeBlink_R"})
		CHECK(phone_shape(name) == ours("eyeBlinkLeft"));
	CHECK(phone_shape("mouthSmile_L") == ours("mouthSmileRight"));
	CHECK(phone_shape("eyeLookOut_L") == ours("eyeLookOutRight"));
	CHECK(phone_shape("eyeLookIn_R") == ours("eyeLookInLeft"));
	// ...the mouth and jaw going to one side too.
	CHECK(phone_shape("mouthLeft") == ours("mouthRight"));
	CHECK(phone_shape("MouthRight") == ours("mouthLeft"));
	CHECK(phone_shape("jawLeft") == ours("jawRight"));
	// The ones with no side are the same.
	CHECK(phone_shape("JawOpen") == ours("jawOpen"));
	CHECK(phone_shape("jawOpen") == ours("jawOpen"));
	CHECK(phone_shape("browInnerUp") == ours("browInnerUp"));
	CHECK(phone_shape("CheekPuff") == ours("cheekPuff"));
	CHECK(phone_shape("mouthPucker") == ours("mouthPucker"));
}

static void test_vtube_studio()
{
	// An iPhone's packet (VTube Studio's own names), with fields we don't
	// know about.
	const std::string packet = R"({"Timestamp":1762590000123,"Hotkey":-1,"FaceFound":true,
		"Rotation":{"x":12.5,"y":-6.0,"z":350.0},"Position":{"x":1.5,"y":-2.0,"z":3.0},
		"EyeLeft":{"x":2,"y":3,"z":0},"EyeRight":{"x":2,"y":3,"z":0},
		"BlendShapes":[{"k":"EyeBlinkLeft","v":0.8},{"k":"JawOpen","v":0.4},{"k":"MouthSmileRight","v":1.7},
			{"k":"TongueOut","v":0.3},{"k":"SomethingNew","v":0.5},{"k":"BrowInnerUp","v":-0.2}],
		"NewThing":{"a":[1,2,3]}})";
	PhoneLook look;
	CHECK(read_vtube_studio(packet.data(), packet.size(), look));
	CHECK(look.found);
	CHECK(look.shapes == 5); // its tongue too (nothing of ours moves with it)
	CHECK(near(look.expressions[ours("eyeBlinkRight")], 0.8, 1e-6));
	CHECK(look.expressions[ours("eyeBlinkLeft")] == 0);
	CHECK(near(look.expressions[ours("jawOpen")], 0.4, 1e-6));
	CHECK(look.expressions[ours("mouthSmileLeft")] == 1); // held to 0..1
	CHECK(look.expressions[ours("browInnerUp")] == 0);
	// Its x is round to the side (+: your own right, which is yaw - in
	// ours), y the nod (+ up), z the lean; 350 is -10.
	CHECK(near(look.turn.yaw, -12.5, 1e-4));
	CHECK(near(look.turn.pitch, -6.0, 1e-4));
	CHECK(near(look.turn.roll, 10.0, 1e-4));
	CHECK(look.placed);
	CHECK(near(look.x, -1.5, 1e-5));
	CHECK(near(look.y, -2.0, 1e-5));
	CHECK(!look.far);

	// MeowFace's names (Apple's own way), and VTube Studio on Android's
	// (some with _L and _R).
	const std::string meow = R"({"FaceFound":true,"Rotation":{"x":0,"y":0,"z":0},"Position":{"x":0,"y":0,"z":0},
		"BlendShapes":[{"k":"eyeBlinkLeft","v":0.25},{"k":"jawOpen","v":0.5}]})";
	CHECK(read_vtube_studio(meow.data(), meow.size(), look));
	CHECK(near(look.expressions[ours("eyeBlinkRight")], 0.25, 1e-6));
	CHECK(near(look.expressions[ours("jawOpen")], 0.5, 1e-6));
	const std::string android = R"({"FaceFound":true,"Rotation":{"x":0,"y":0,"z":0},
		"BlendShapes":[{"k":"EyeBlinkLeft","v":0.1},{"k":"eyeLookOut_L","v":0.6},{"k":"mouthSmile_R","v":0.3}]})";
	CHECK(read_vtube_studio(android.data(), android.size(), look));
	CHECK(near(look.expressions[ours("eyeBlinkRight")], 0.1, 1e-6));
	CHECK(near(look.expressions[ours("eyeLookOutRight")], 0.6, 1e-6));
	CHECK(near(look.expressions[ours("mouthSmileLeft")], 0.3, 1e-6));
	CHECK(!look.placed); // no Position

	// Lost: nothing it says counts.
	const std::string lost = R"({"FaceFound":false,"Rotation":{"x":30,"y":0,"z":0},
		"BlendShapes":[{"k":"JawOpen","v":0.9}]})";
	CHECK(read_vtube_studio(lost.data(), lost.size(), look));
	CHECK(!look.found);
	CHECK(look.expressions[ours("jawOpen")] == 0);
	CHECK(look.turn.yaw == 0);

	// Not a packet at all: false, and what it had is kept.
	look.expressions[ours("jawOpen")] = 0.7f;
	for (const char *bad : {"", "hello", "[1,2,3]", "{\"Timestamp\":5}", "{\"BlendShapes\":", "null"}) {
		CHECK(!read_vtube_studio(bad, std::strlen(bad), look));
		CHECK(near(look.expressions[ours("jawOpen")], 0.7, 1e-6));
	}
}

static void test_ifacialmocap()
{
	// Its first way: name-value (0 to 100), then its head (degrees, then
	// metres) and eyes.
	const std::string v1 = "mouthSmile_R-30|eyeBlink_L-100|jawOpen-25|tongueOut-0|hapihapi-5|"
			       "=head#-10.5,20,5,0.01,-0.02,-0.45|rightEye#5.2,2.4,0.25|leftEye#5.3,-1.6,-0.17|";
	PhoneLook look;
	CHECK(read_ifacialmocap(v1.data(), v1.size(), look));
	CHECK(look.found);
	CHECK(look.shapes == 4);
	CHECK(near(look.expressions[ours("eyeBlinkRight")], 1.0, 1e-6));
	CHECK(near(look.expressions[ours("mouthSmileLeft")], 0.3, 1e-6));
	CHECK(near(look.expressions[ours("jawOpen")], 0.25, 1e-6));
	// x the nod (+ down), y round to the side (+ your own right), z the
	// lean.
	CHECK(near(look.turn.pitch, 10.5, 1e-4));
	CHECK(near(look.turn.yaw, -20, 1e-4));
	CHECK(near(look.turn.roll, 5, 1e-4));
	CHECK(look.placed && look.far);
	CHECK(near(look.x, -1, 1e-4));
	CHECK(near(look.y, -2, 1e-4));
	CHECK(near(look.z, -45, 1e-4));

	// Its second way: name&value, with decimals; and a head too near to be
	// where it says (how far is then left out).
	const std::string v2 = "eyeBlink_R&55.5| mouthLeft&12 |=head#1.5,-2.25,0,0.052,0.236,-0.026|";
	CHECK(read_ifacialmocap(v2.data(), v2.size(), look));
	CHECK(near(look.expressions[ours("eyeBlinkLeft")], 0.555, 1e-5));
	CHECK(near(look.expressions[ours("mouthRight")], 0.12, 1e-6));
	CHECK(near(look.turn.yaw, 2.25, 1e-4));
	CHECK(look.placed);
	CHECK(!look.far);

	// Only its head (3 numbers): turned, not placed.
	const std::string head = "=head#0,0,-7|";
	CHECK(read_ifacialmocap(head.data(), head.size(), look));
	CHECK(near(look.turn.roll, -7, 1e-4));
	CHECK(!look.placed);

	// Not one of its lines.
	for (const char *bad : {"", "|||", "hello", "=head#a,b,c|", "x&y|", "{\"FaceFound\":true}"})
		CHECK(!read_ifacialmocap(bad, std::strlen(bad), look));
}

static void test_asking()
{
	const json ask = json::parse(vtube_studio_request(54321, 3));
	CHECK(ask["messageType"] == "iOSTrackingDataRequest");
	CHECK(ask["time"] == 3);
	CHECK(ask["sentBy"] == "DireBot");
	CHECK(ask["ports"] == json::array({54321}));
	CHECK(json::parse(vtube_studio_request(1, 99))["time"] == 10); // the app's most
	CHECK(ifacialmocap_hello(false) == "iFacialMocap_sahuasouryya9218sauhuiayeta91555dy3719");
	CHECK(ifacialmocap_hello(true) == "iFacialMocap_sahuasouryya9218sauhuiayeta91555dy3719|sendDataVersion=v2");

	std::string host;
	int port = 0;
	CHECK(split_address("192.168.1.23", VTS_PORT, host, port) && host == "192.168.1.23" && port == VTS_PORT);
	CHECK(split_address(" 192.168.1.23 : 21500 ", VTS_PORT, host, port) && host == "192.168.1.23" &&
	      port == 21500);
	CHECK(split_address("http://10.0.0.7/", IFM_PORT, host, port) && host == "10.0.0.7" && port == IFM_PORT);
	CHECK(split_address("[fe80::1%wlan0]:500", VTS_PORT, host, port) && host == "fe80::1%wlan0" && port == 500);
	CHECK(split_address("fe80::1", VTS_PORT, host, port) && host == "fe80::1" && port == VTS_PORT);
	CHECK(split_address("my-iphone.local", VTS_PORT, host, port) && host == "my-iphone.local");
	for (const char *bad : {"", " ", "1.2.3.4:99999", "1.2.3.4:0", "1.2.3.4:abc", "a/b", "[::1", "[::1]x", "1.2.3.4:"})
		CHECK(!split_address(bad, VTS_PORT, host, port));
}

// The face through the tracker as a camera's would be.
static void test_take()
{
	Tracker tracker;
	std::string problem;
	CHECK(tracker.load_file(MODELS, &problem));
	tracker.set_smoothing(-1);
	PhoneLook look;
	look.found = true;
	look.turn = {20, -10, 5};
	look.x = 3;
	look.y = -2;
	look.placed = true;
	look.expressions[ours("eyeBlinkRight")] = 0.8f;
	look.expressions[ours("jawOpen")] = 0.6f;
	const Face &face = tracker.take(look, 1.0);
	CHECK(face.found);
	CHECK(face.w == PHONE_W && face.h == PHONE_H);
	CHECK(face.presence == 1);
	// The head's pose worked out from its points is the turn and place it
	// came from.
	const Turn t = turn_of(face.pose);
	CHECK(near(t.yaw, 20, 0.5));
	CHECK(near(t.pitch, -10, 0.5));
	CHECK(near(t.roll, 5, 0.5));
	CHECK(near(face.turn.yaw, 20, 0.5)); // no "Set" yet
	CHECK(near(face.pose[3], 3, 1.0));
	CHECK(near(face.pose[7], -2, 1.0));
	CHECK(near(face.pose[11], -PHONE_FAR, 3.0));
	CHECK(near(face.expressions[ours("eyeBlinkRight")], 0.8, 1e-6));
	CHECK(near(face.moves[ours("jawOpen")], 0.6, 1e-6));
	CHECK(face.expressions[0] == 0);
	// Yaw + is towards the picture's right: the nose is right of the eyes'
	// middle; your right eye (Google's 33) on the picture's left.
	const float eyes_x = (face.points[RIGHT_EYE_OUTER].x + face.points[LEFT_EYE_OUTER].x) / 2;
	CHECK(face.points[NOSE_TIP].x > eyes_x);
	CHECK(face.points[RIGHT_EYE_OUTER].x < face.points[LEFT_EYE_OUTER].x);
	// Nodding down (pitch -): the nose is under where it'd be straight on.
	{
		Tracker straight;
		CHECK(straight.load_file(MODELS));
		straight.set_smoothing(-1);
		PhoneLook s;
		s.found = true;
		const float level = straight.take(s, 1.0).points[NOSE_TIP].y;
		s.turn.pitch = -15;
		CHECK(straight.take(s, 1.1).points[NOSE_TIP].y > level + 5);
		// Nearer: bigger.
		const Layout far_away = layout_of(straight.take(s, 1.2));
		s.far = true;
		s.z = -25;
		const Layout close_by = layout_of(straight.take(s, 1.3));
		CHECK(close_by.gap > far_away.gap * 1.5);
		CHECK(near(straight.face().pose[11], -25, 2.0));
	}

	// The 2D avatar's view of it: found, eyes apart, the turn.
	{
		Shown shown;
		const Shown::Out o = shown.step(face, tracker.rest(), 1 / 60.0f);
		CHECK(o.found);
		CHECK(o.layout.gap > 0.02f && o.layout.gap < 0.2f);
		CHECK(near(o.turn.yaw, 20, 0.5));
		// Turned well past the edge (20): the side shown.
		Tracker turning;
		CHECK(turning.load_file(MODELS));
		turning.set_smoothing(-1);
		PhoneLook far_round = look;
		far_round.turn.yaw = 30;
		Shown shown2;
		const Shown::Out o2 = shown2.step(turning.take(far_round, 1.0), turning.rest(), 1 / 60.0f);
		CHECK(o2.turned == 1);
		PhoneLook other_way = far_round;
		other_way.turn.yaw = -30;
		Shown shown3;
		CHECK(shown3.step(turning.take(other_way, 1.1), turning.rest(), 1 / 60.0f).turned == -1);
	}

	// "Set": ten looks at rest, then the turn and moves are against it.
	PhoneLook rest = look;
	rest.turn = {10, -20, 0};
	rest.expressions[ours("jawOpen")] = 0.2f;
	tracker.set_rest();
	CHECK(tracker.setting());
	for (int i = 0; i < 10; i++)
		tracker.take(rest, 2.0 + i / 60.0);
	CHECK(!tracker.setting());
	CHECK(tracker.has_rest());
	PhoneLook moved = rest;
	moved.turn.yaw = 25;
	moved.expressions[ours("jawOpen")] = 0.6f;
	const Face &after = tracker.take(moved, 3.0);
	CHECK(near(after.turn.yaw, 15, 1.0));
	CHECK(near(after.turn.pitch, 0, 1.0));
	CHECK(near(after.moves[ours("jawOpen")], 0.5, 1e-3));
	// "Set" as text and back works for a phone's too.
	const std::string saved = tracker.save_rest();
	CHECK(!saved.empty());
	Tracker again;
	CHECK(again.load_file(MODELS));
	CHECK(again.load_rest(saved));

	// Lost.
	PhoneLook gone;
	CHECK(!tracker.take(gone, 4.0).found);

	// Smoothing on: a still head stays where it is; a turn gets there.
	{
		Tracker smooth;
		CHECK(smooth.load_file(MODELS));
		smooth.set_smoothing(0.5f);
		PhoneLook s;
		s.found = true;
		for (int i = 0; i < 30; i++)
			smooth.take(s, i / 60.0);
		CHECK(near(turn_of(smooth.face().pose).yaw, 0, 0.5));
		s.turn.yaw = 30;
		for (int i = 30; i < 90; i++)
			smooth.take(s, i / 60.0);
		CHECK(near(turn_of(smooth.face().pose).yaw, 30, 1.5));
	}

	// Back to a camera after a phone: the picture's looked at as before.
	{
		std::ifstream in(ROOT + "/tests/data/face/portrait.ppm", std::ios::binary);
		std::string magic;
		int w = 0, h = 0, most = 0;
		in >> magic >> w >> h >> most;
		in.get();
		std::vector<uint8_t> rgb((size_t)w * h * 3);
		in.read(reinterpret_cast<char *>(rgb.data()), (std::streamsize)rgb.size());
		std::vector<uint8_t> rgba((size_t)w * h * 4, 255);
		for (size_t i = 0; i < (size_t)w * h; i++)
			for (int k = 0; k < 3; k++)
				rgba[i * 4 + k] = rgb[i * 3 + k];
		const Frame frame{rgba.data(), w, h, w * 4, false};
		const Face &seen = tracker.look(frame, 5.0);
		CHECK(seen.found);
		CHECK(seen.w == w && seen.h == h);
	}
}

// ---- the link, with a pretend phone on this machine ----------------------------

struct Pretend {
	int sock = -1;
	int port = 0;
	explicit Pretend(const char *ip = "127.0.0.1", int at = 0)
	{
		sock = ::socket(AF_INET, SOCK_DGRAM, 0);
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_port = htons((uint16_t)at);
		inet_pton(AF_INET, ip, &a.sin_addr);
		if (::bind(sock, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0) {
			::close(sock);
			sock = -1;
			return;
		}
		socklen_t len = sizeof(a);
		getsockname(sock, reinterpret_cast<sockaddr *>(&a), &len);
		port = ntohs(a.sin_port);
		timeval tv{0, 200000};
		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	}
	~Pretend()
	{
		if (sock >= 0)
			::close(sock);
	}
	// What the PC sent, and where from.
	bool hear(std::string &text, sockaddr_in &from, double seconds = 3)
	{
		const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
		char buf[4096];
		while (std::chrono::steady_clock::now() < until) {
			socklen_t len = sizeof(from);
			const ssize_t n = ::recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&from), &len);
			if (n > 0) {
				text.assign(buf, (size_t)n);
				return true;
			}
		}
		return false;
	}
	void send_to(const std::string &text, int to_port)
	{
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_port = htons((uint16_t)to_port);
		inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
		::sendto(sock, text.data(), text.size(), 0, reinterpret_cast<sockaddr *>(&a), sizeof(a));
	}
};

struct Taken {
	std::mutex lock;
	std::vector<PhoneLook> looks;
	size_t count()
	{
		std::lock_guard<std::mutex> l(lock);
		return looks.size();
	}
	PhoneLook last()
	{
		std::lock_guard<std::mutex> l(lock);
		return looks.empty() ? PhoneLook() : looks.back();
	}
};

static bool wait_for(const std::function<bool()> &done, double seconds = 3)
{
	const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
	while (std::chrono::steady_clock::now() < until) {
		if (done())
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return done();
}

static void test_link_vtube_studio()
{
	Pretend phone;
	CHECK(phone.sock >= 0);
	Taken taken;
	PhoneLink link([&](const PhoneLook &look, uint64_t) {
		std::lock_guard<std::mutex> l(taken.lock);
		taken.looks.push_back(look);
	});
	link.set_ask_every(0.2f);
	CHECK(!link.state().on);
	link.set(PHONE_VTUBE_STUDIO, "127.0.0.1:" + std::to_string(phone.port));
	// It asks the phone, on the phone's own port, for looks sent to its.
	std::string asked;
	sockaddr_in from{};
	CHECK(phone.hear(asked, from));
	const json ask = json::parse(asked, nullptr, false);
	CHECK(ask.is_object() && ask.value("messageType", "") == "iOSTrackingDataRequest");
	const int back = ask.is_object() && ask["ports"].is_array() && !ask["ports"].empty() ? ask["ports"][0].get<int>() : 0;
	CHECK(back > 0);
	CHECK(back == ntohs(from.sin_port)); // its own socket
	PhoneLink::State st = link.state();
	CHECK(st.on && st.listening && st.port == back);
	CHECK(st.problem.empty());
	// ...and asks again, about once a second (here, more often).
	std::string again;
	CHECK(phone.hear(again, from, 2));
	// 60 looks a second for half a second.
	for (int i = 0; i < 30; i++) {
		phone.send_to(R"({"FaceFound":true,"Rotation":{"x":-15,"y":5,"z":0},"Position":{"x":0,"y":0,"z":0},)"
			      R"("BlendShapes":[{"k":"EyeBlinkRight","v":0.9}]})",
			      back);
		std::this_thread::sleep_for(std::chrono::milliseconds(16));
	}
	CHECK(wait_for([&] { return taken.count() >= 25; }));
	const PhoneLook got = taken.last();
	CHECK(got.found);
	CHECK(near(got.turn.yaw, 15, 1e-4));
	CHECK(near(got.expressions[ours("eyeBlinkLeft")], 0.9, 1e-6));
	st = link.state();
	CHECK(st.packets >= 25);
	CHECK(st.found);
	CHECK(st.shapes == 1);
	CHECK(st.since >= 0 && st.since < 1);
	CHECK(wait_for([&] { return link.state().per_second > 10; }, 2));
	// Not readable: counted, nothing taken.
	const size_t before = taken.count();
	phone.send_to("not a packet", back);
	CHECK(wait_for([&] { return link.state().unread == 1; }));
	CHECK(taken.count() == before);
	// From somewhere else (another address on this machine): left out.
	{
		Pretend stranger("127.0.0.2");
		if (stranger.sock >= 0) {
			stranger.send_to(R"({"FaceFound":true,"BlendShapes":[{"k":"JawOpen","v":1}]})", back);
			CHECK(wait_for([&] { return link.state().others == 1; }));
			CHECK(link.state().other == "127.0.0.2");
			CHECK(taken.count() == before);
		}
	}
	// It stops sending: your face is lost, once.
	CHECK(wait_for([&] { return !taken.last().found; }, 2));
	const size_t lost_at = taken.count();
	std::this_thread::sleep_for(std::chrono::milliseconds(700));
	CHECK(taken.count() == lost_at);
	// Off.
	link.stop();
	CHECK(wait_for([&] { return !link.state().on && !link.state().listening; }));
}

static void test_link_ifacialmocap()
{
	Pretend phone;
	Taken taken;
	PhoneLink link([&](const PhoneLook &look, uint64_t) {
		std::lock_guard<std::mutex> l(taken.lock);
		taken.looks.push_back(look);
	});
	link.set(PHONE_IFACIALMOCAP, "127.0.0.1:" + std::to_string(phone.port));
	std::string hello;
	sockaddr_in from{};
	CHECK(phone.hear(hello, from));
	CHECK(hello.rfind("iFacialMocap_sahuasouryya9218sauhuiayeta91555dy3719", 0) == 0);
	// Its answers go to iFacialMocap's own port on the PC.
	CHECK(ntohs(from.sin_port) == IFM_PORT);
	CHECK(link.state().port == IFM_PORT);
	for (int i = 0; i < 10; i++) {
		phone.send_to("eyeBlink_L&100|jawOpen&40|=head#0,30,0,0,0,-0.5|", IFM_PORT);
		std::this_thread::sleep_for(std::chrono::milliseconds(16));
	}
	CHECK(wait_for([&] { return taken.count() >= 8; }));
	const PhoneLook got = taken.last();
	CHECK(got.found);
	CHECK(near(got.turn.yaw, -30, 1e-4));
	CHECK(near(got.expressions[ours("eyeBlinkRight")], 1, 1e-6));
	CHECK(got.far && near(got.z, -50, 1e-3));
	link.stop();
	CHECK(wait_for([&] { return !link.state().listening; }));

	// Its port taken by another app: said so, in plain words.
	Pretend other("0.0.0.0", IFM_PORT);
	if (other.sock >= 0) {
		PhoneLink busy([](const PhoneLook &, uint64_t) {});
		busy.set(PHONE_IFACIALMOCAP, "127.0.0.1");
		CHECK(wait_for([&] { return busy.state().problem.find("49983") != std::string::npos; }));
		CHECK(!busy.state().listening);
	}
}

static void test_link_problems()
{
	PhoneLink link([](const PhoneLook &, uint64_t) {});
	link.set(PHONE_VTUBE_STUDIO, "1.2.3.4:99999");
	CHECK(wait_for([&] { return link.state().problem.find("isn't an address") != std::string::npos; }));
	CHECK(link.state().on && !link.state().listening);
	// A new address starts over.
	Pretend phone;
	link.set(PHONE_VTUBE_STUDIO, "127.0.0.1:" + std::to_string(phone.port));
	CHECK(wait_for([&] { return link.state().listening && link.state().problem.empty(); }));
	CHECK(link.state().address == "127.0.0.1:" + std::to_string(phone.port));
	CHECK(link.state().packets == 0);
	CHECK(link.state().since < 0);
}

int main()
{
	test_names();
	test_vtube_studio();
	test_ifacialmocap();
	test_asking();
	test_take();
	test_link_vtube_studio();
	test_link_ifacialmocap();
	test_link_problems();
	std::printf("unit_phone: %d passed, %d failed\n", passes, failures);
	return failures ? 1 : 0;
}
