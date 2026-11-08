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

// Moves from another tracker app (0.9.0), over the VMC protocol: apps
// that follow you - XR Animator, Webcam Motion Capture, VSeeFace,
// VirtualMotionCapture, mocopi and SlimeVR's bridges, Waidayo and other
// phone apps - send their character's bones and face over VMC, and the
// face avatar's 3D character takes them: its body and legs, its arms,
// hands and fingers, and its face (each as you pick), the rest still from
// your camera or not at all.
//
// VMC is OSC (small binary messages) over UDP, to this PC's port 39539
// unless you pick another. What's read here (each app sends what it
// has; anything else is skipped):
//
//   /VMC/Ext/Bone/Pos  name, x y z, x y z w: one bone - Unity's name for
//                      it ("Hips", "LeftUpperArm", "RightIndexProximal"...),
//                      its place and turn against its parent bone
//   /VMC/Ext/Root/Pos  "root", x y z, x y z w (and more): where the whole
//                      character is - not used: the face avatar stays in
//                      its own spot
//   /VMC/Ext/Blend/Val name, value: a face shape ("A", "Blink_L", "Joy",
//                      or Apple's ARKit names - "perfect sync" - like
//                      "JawOpen"), 0 to 1...
//   /VMC/Ext/Blend/Apply ...all of them shown together from here
//   /VMC/Ext/OK        whether its character is loaded (and calibrated)
//   /VMC/Ext/T         its clock
//
// The bones' turns are as the app's own character has them, in Unity's
// terms (left-handed: x its right, y up, z its front) and from a T pose
// with every bone at rest turned nowhere (a VRM as VRM apps hold it:
// "normalized") - so each is VRM's normalized turn, and any VRM takes it
// as it takes a VRM animation file's (rig.hpp). Here they're turned into
// ours (right-handed, facing +z: its left along +x), the mirror of
// Unity's: a place (-x, y, z), a turn (x, -y, -z, w).
//
// Its face goes through the tracker as a phone's does (phone.hpp's
// PhoneLook, Tracker::take): its head's turn (its neck and head bones),
// its eyes' looks (its eye bones, or shapes), its face's moves - so Set,
// the tuning, alive when you're still, the hand signs' face moves and a
// 2D character all work with it. Its faces (happy, angry...) and any
// shape of its own a 3D character has too are shown by their names.
//
// How a VMC receiver takes all this is from EVMC4U, gpsnmeajp's VMC
// receiver for Unity (MIT licence; its steps as a recipe, no code
// copied).
//
// The reading is plain code over plain bytes (tests/unit_vmc.cpp); the
// link (VmcLink) has a thread and one UDP socket of its own, shared by
// every face avatar listening on the same port.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "math3d.hpp"
#include "phone.hpp"

namespace direbot::face {

constexpr int VMC_PORT = 39539;

// ---- OSC -----------------------------------------------------------------------

// One value in a message: its OSC type ('i' int, 'f' float, 's' text,
// 'd', 'h', 'T', 'F'...) and what it is.
struct OscArg {
	char type = 'N';
	double number = 0; // i, f, h, d, c (and T 1, F 0)
	std::string text;  // s, S
	bool is_number() const { return type == 'i' || type == 'f' || type == 'd' || type == 'h'; }
};
struct OscMessage {
	std::string address;
	std::vector<OscArg> args;
};
// One packet: a message, or a bundle of them (bundles in bundles too);
// each message read goes to `each`, in order. How many were read (0: it
// isn't OSC at all - or a bundle with nothing in it).
size_t read_osc(const uint8_t *data, size_t size, const std::function<void(const OscMessage &)> &each);
// A message as OSC, and messages in a bundle (for tests and pretend apps).
std::string osc_message(const OscMessage &m);
std::string osc_bundle(const std::vector<std::string> &messages);

// ---- The humanoid's bones ------------------------------------------------------

// VRM 1.0's names for a humanoid's bones (VRM 0.x's and Unity's are the
// same but for the thumb: their ThumbProximal, ThumbIntermediate and
// ThumbDistal are 1.0's ThumbMetacarpal, ThumbProximal and ThumbDistal).
enum VmcBone {
	VMC_HIPS,
	VMC_SPINE,
	VMC_CHEST,
	VMC_UPPER_CHEST,
	VMC_NECK,
	VMC_HEAD,
	VMC_LEFT_EYE,
	VMC_RIGHT_EYE,
	VMC_JAW,
	VMC_LEFT_UPPER_LEG,
	VMC_LEFT_LOWER_LEG,
	VMC_LEFT_FOOT,
	VMC_LEFT_TOES,
	VMC_RIGHT_UPPER_LEG,
	VMC_RIGHT_LOWER_LEG,
	VMC_RIGHT_FOOT,
	VMC_RIGHT_TOES,
	VMC_LEFT_SHOULDER,
	VMC_LEFT_UPPER_ARM,
	VMC_LEFT_LOWER_ARM,
	VMC_LEFT_HAND,
	VMC_RIGHT_SHOULDER,
	VMC_RIGHT_UPPER_ARM,
	VMC_RIGHT_LOWER_ARM,
	VMC_RIGHT_HAND,
	// Then each hand's fingers, the left's first: the thumb, first,
	// middle, ring and little finger, three bones each from the hand out.
	VMC_LEFT_THUMB_METACARPAL,
	VMC_RIGHT_THUMB_METACARPAL = VMC_LEFT_THUMB_METACARPAL + 15,
	VMC_BONES = VMC_RIGHT_THUMB_METACARPAL + 15,
};
extern const char *const VMC_BONE_NAMES[VMC_BONES];
// A bone by Unity's name (any case: VMC's are Unity's), or VRM 1.0's
// "...ThumbMetacarpal" - else -1.
int vmc_bone(const std::string &name);
// The same bone on the other side (leftHand <-> rightHand), or itself.
int vmc_other_side(int bone);
// Which part of you it is.
enum VmcPart { VMC_PART_BODY, VMC_PART_LEGS, VMC_PART_HEAD, VMC_PART_ARMS, VMC_PART_FINGERS };
VmcPart vmc_part(int bone);

// ---- What an app sent ----------------------------------------------------------

// A bone, a share of a second, a face shape counts as sent this long after
// it last came: then that part of you is yours again.
constexpr double VMC_FRESH = 0.5;

struct VmcSent {
	// Each bone as the app last sent it, in Unity's terms: its turn (x y z
	// w) and place (x y z) against its parent, and when (seconds on the
	// reader's clock; below 0: never).
	float turn[VMC_BONES][4] = {};
	float at[VMC_BONES][3] = {};
	double when[VMC_BONES];
	// Its face's shapes as last shown (Blend/Apply), and when.
	std::vector<std::pair<std::string, float>> shapes;
	double shapes_when = -1;
	// /VMC/Ext/OK: its character loaded (1) or not (0), its calibration
	// (3: done), or -1 when it hasn't said.
	int loaded = -1, calibration = -1;
	// Messages read, and those that aren't VMC's or not used.
	uint64_t messages = 0, skipped = 0;
	// Goes up each time something new came.
	uint64_t changes = 0;
	VmcSent();
};

// Reads an app's messages into what it has sent (plain code: no clock or
// socket of its own; `now` is the caller's seconds).
class VmcReader {
public:
	void take(const OscMessage &m, double now);
	// A packet's messages all read: shapes set but never shown with
	// Blend/Apply (an app that doesn't send it) are shown now.
	void packet_done(double now);
	const VmcSent &sent() const { return sent_; }
	void clear();

private:
	VmcSent sent_;
	std::vector<std::pair<std::string, float>> pending_;
	bool applies_ = false; // it sends Blend/Apply
};

// What the app is sending lately, part by part (for the page and its
// check): bones of its body (hips, spine, chest, legs), its head (neck,
// head, eyes), its arms (shoulders to hands) and its fingers; its face's
// shapes - how many, and whether they're Apple's ARKit ones ("perfect
// sync").
struct VmcSends {
	int bones = 0;
	bool body = false, head = false, arms = false, fingers = false, legs = false, eyes = false;
	int shapes = 0;
	bool perfect_sync = false;
};
VmcSends vmc_sends(const VmcSent &s, double now);

// Its bones as ours (the facing model's way: right-handed, its left along
// +x, facing +z), each that's fresh: VRM's normalized turn and its place
// against its parent. `mirror`: left and right swapped (its left arm
// moves yours as your reflection's).
struct VmcBones {
	Quat turn[VMC_BONES];
	V3 at[VMC_BONES];
	bool has[VMC_BONES] = {};
};
VmcBones vmc_bones(const VmcSent &s, double now, bool mirror);

// A turn from Unity's terms to ours (and a place).
Quat vmc_turn(const float q[4]);
V3 vmc_place(const float p[3]);

// Its face as the tracker takes a phone's (phone.hpp), its character's own
// left and right (a mirrored one is swapped by the caller): its face's
// moves from its shapes (ARKit's as they are; VRM's blinks, vowels and
// looks made into ARKit's), its eyes' looks from its eye bones when its
// shapes don't say, its head's turn from its neck and head bones (against
// its chest). Its faces (VRM's happy, angry, sad, relaxed, surprised: by
// VRM 1.0's names) and its other shapes go in `faces`, by name, for a
// character that has them. Not found when nothing of its face came lately.
// An eye bone turned this far (degrees) is looking all the way.
constexpr float VMC_EYE_RANGE = 12;
bool vmc_face(const VmcSent &s, double now, PhoneLook &look, std::vector<std::pair<std::string, float>> &faces);
// A face shape's name plain: lower case, letters and digits only, "_L"
// and "_R" at its end as "left" and "right" ("Blink_L" -> "blinkleft").
std::string vmc_plain(const std::string &name);

// ---- The link ------------------------------------------------------------------

// One UDP socket on a thread of its own, listening on a port of this PC
// for an app's VMC: on every network this PC is on (an app on your phone
// sends over your Wi-Fi), from one app at a time - the first that sends;
// another that sends while it does is only counted (and named on the
// page), and takes over once the first has been quiet for a second.
class VmcLink {
public:
	// The link listening on `port` - opened the first time it's asked
	// for, shut once no face avatar holds it.
	static std::shared_ptr<VmcLink> open(int port);
	explicit VmcLink(int port);
	~VmcLink();
	VmcLink(const VmcLink &) = delete;
	VmcLink &operator=(const VmcLink &) = delete;

	int port() const { return port_; }
	struct State {
		bool listening = false; // its socket is open
		int port = 0;
		std::string problem;  // why it can't listen (in plain words), or ""
		uint64_t packets = 0; // read since it started
		float per_second = 0; // lately
		float since = -1;     // seconds since the last one (-1: none yet)
		// Where the app sending is ("this PC" or its address).
		std::string from;
		// Packets from another app (and where it is), and those that
		// weren't VMC (OSC) at all.
		uint64_t others = 0;
		std::string other;
		uint64_t unread = 0;
	};
	State state() const;
	// What it's been sent (a copy) and the reader's clock now (seconds).
	void sent(VmcSent &out, double &now) const;
	uint64_t changes() const;
	// The reader's clock (seconds).
	static double clock();

private:
	struct Impl;
	int port_;
	std::unique_ptr<Impl> impl_;
};

} // namespace direbot::face
