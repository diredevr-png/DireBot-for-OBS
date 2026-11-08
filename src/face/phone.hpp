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

// Your face from a phone (0.9.0): an app on your phone follows your face
// (an iPhone's own face camera, or the app's tracker on Android) and sends
// it over your Wi-Fi, about 60 times a second - the 52 face moves (Apple's
// ARKit ones: the same names as ours) and which way your head is turned.
//
// Two ways of talking, as the apps have them:
//
//   VTube Studio (iPhone and Android, with "3rd Party PC Clients" on) and
//   MeowFace (Android): we send the phone a request on its port 21412
//   about once a second -
//     {"messageType":"iOSTrackingDataRequest","time":3,"sentBy":"DireBot",
//      "ports":[<ours>]}
//   - and it sends us JSON for that long: "FaceFound", "Rotation" and
//   "Position" ({x, y, z}), "BlendShapes" ([{"k": name, "v": 0 to 1}])
//   and more we don't need (fields may be added: unknown ones are fine).
//   (DenchiSoft's VTubeStudioBlendshapeUDPReceiverTest, MIT.)
//
//   iFacialMocap (iPhone): we send it "iFacialMocap_sahuasouryya9218sau
//   huiayeta91555dy3719|sendDataVersion=v2" on its port 49983 and it
//   sends lines back to ours, 49983: "name&value|...|=head#rx,ry,rz,px,
//   py,pz|rightEye#x,y,z|leftEye#x,y,z|" (values 0 to 100; the first
//   version "name-value"), its head turned in degrees, where it is in
//   metres.
//
// The names come three ways ("EyeBlinkLeft", "eyeBlinkLeft",
// "eyeBlink_L") and are read alike. Apple's "left" and "right" are the
// other way round to Google's (the face models the camera uses, which ours
// follow): Apple's eyeBlinkLeft is your right eye. They're swapped here,
// so a wink is the same eye either way. The head's turn is made ours too
// (Turn: yaw towards the picture's right, pitch up, roll clockwise, as a
// camera that doesn't mirror sees you).
//
// How the turns, places and sides compare is from programs that turn
// one into the other and are used every day: Inochi2D's facetrack-d
// (BSD), xuan25's VTube-IFacial-Link and DrBomb's ExpressionAppBridge
// (MIT); none of their code is in here.
//
// The face then goes through the tracker as a camera's would
// (Tracker::take): "Set", the smoothing, your face's tuning and the rest
// all work the same. What a phone can't give: your real eyes and mouth
// cut out of a camera's picture, the face worn on your head - and your
// hands, which still come from the camera if you pick one.
//
// The reading is plain code over plain text (tests/unit_phone.cpp); the
// link (PhoneLink) has a thread and one UDP socket of its own.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "pose.hpp"
#include "tracker.hpp"

namespace direbot::face {

// The phone apps, by how they talk.
enum PhoneApp {
	PHONE_VTUBE_STUDIO, // VTube Studio's phone app, and MeowFace (its way)
	PHONE_IFACIALMOCAP,
};
// Their ports on the phone (and iFacialMocap's here too).
constexpr int VTS_PORT = 21412, IFM_PORT = 49983;

// What one packet from the phone says.
struct PhoneLook {
	bool found = false;
	// Your face's moves, 0 to 1, in our names (tracker.hpp's - so the
	// sides are Google's); the ones it didn't send stay 0.
	float expressions[EXPRESSIONS] = {};
	int shapes = 0; // how many of the 52 it sent (its tongue too)
	// Which way your head is turned (degrees, as Turn).
	Turn turn;
	// Where your head is from the phone's camera, in centimetres: x
	// towards the picture's right, y up, z towards the camera (in front of
	// it is below 0) - `placed` when it says where across and up, `far`
	// when it says how far too.
	float x = 0, y = 0, z = 0;
	bool placed = false, far = false;
};

// The phone's name for one of the 52 -> ours (tracker.hpp's number), or
// -1 (its tongue, and anything else we don't have).
int phone_shape(const std::string &name);

// One packet read. False if it isn't one at all (its moves then aren't
// changed).
bool read_vtube_studio(const char *text, size_t size, PhoneLook &out);
bool read_ifacialmocap(const char *text, size_t size, PhoneLook &out);

// What we send the phone: VTube Studio's request for `seconds` of looks
// to our `port`, and iFacialMocap's hello (`v2`: its newer lines).
std::string vtube_studio_request(int port, float seconds);
std::string ifacialmocap_hello(bool v2);

// An address as you'd type it: "192.168.1.23", with ":port" or without,
// "[fe80::1]:21412", or a name. The host and the port (`fallback` when
// there isn't one). False if it can't be one.
bool split_address(const std::string &text, int fallback, std::string &host, int &port);

// The link: one UDP socket on a thread of its own, asking the phone for
// looks and taking them as they come. Only packets from the phone's own
// address are taken.
class PhoneLink {
public:
	// `look`: each packet read (on the link's thread), and when it came
	// (os clock, nanoseconds).
	using Taker = std::function<void(const PhoneLook &look, uint64_t ns)>;
	explicit PhoneLink(Taker take);
	~PhoneLink();
	PhoneLink(const PhoneLink &) = delete;
	PhoneLink &operator=(const PhoneLink &) = delete;

	// Starts (or starts again, when something changed) asking `address`
	// in the app's way; an empty address stops it.
	void set(PhoneApp app, const std::string &address);
	void stop();

	struct State {
		bool on = false;        // set to ask a phone
		bool listening = false; // its socket is open
		PhoneApp app = PHONE_VTUBE_STUDIO;
		std::string address;  // as set
		std::string phone;    // where it asks (host:port), once found
		int port = 0;         // ours, the phone sends to
		std::string problem;  // why it can't (in plain words), or ""
		uint64_t packets = 0; // read since it started
		float per_second = 0; // lately
		float since = -1;     // seconds since the last one (-1: none yet)
		bool found = false;   // the last one saw your face
		int shapes = 0;       // how many of the 52 the last one had
		// Packets that came from somewhere else (not the phone's
		// address), and the last such address - the phone may have
		// another one than the one typed in.
		uint64_t others = 0;
		std::string other;
		uint64_t unread = 0; // from the phone, but not readable
	};
	State state() const;

	// For tests: how often it asks again (seconds).
	void set_ask_every(float seconds);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace direbot::face
