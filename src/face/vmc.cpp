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

#include "vmc.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include "pose.hpp"
#include "voice.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
// (Windows' old "near" and "far" words, which PhoneLook uses.)
#undef near
#undef far
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace direbot::face {

// ---- OSC -----------------------------------------------------------------------

namespace {

// Bundles in bundles this deep at most (a packet can't make it dig for
// ever).
constexpr int MOST_DEPTH = 8;

uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

uint64_t be64(const uint8_t *p)
{
	return (uint64_t)be32(p) << 32 | be32(p + 4);
}

// A text and its ending (0) padded to 4 bytes, from `at`: the text, and
// `at` moved past it. False if it runs past the end.
bool osc_text(const uint8_t *d, size_t size, size_t &at, std::string &out)
{
	size_t end = at;
	while (end < size && d[end])
		end++;
	if (end >= size)
		return false;
	out.assign(reinterpret_cast<const char *>(d) + at, end - at);
	const size_t next = (end + 4) & ~(size_t)3;
	if (next > size)
		return false;
	at = next;
	return true;
}

bool read_message(const uint8_t *d, size_t size, OscMessage &m)
{
	size_t at = 0;
	if (!osc_text(d, size, at, m.address) || m.address.empty() || m.address[0] != '/')
		return false;
	m.args.clear();
	// No types (OSC's oldest way): nothing more we can read.
	if (at >= size || d[at] != ',')
		return true;
	std::string tags;
	if (!osc_text(d, size, at, tags))
		return false;
	for (size_t k = 1; k < tags.size(); k++) {
		OscArg a;
		a.type = tags[k];
		auto need = [&](size_t n) {
			return at + n <= size;
		};
		switch (a.type) {
		case 'i':
		case 'c':
		case 'r':
		case 'm':
			if (!need(4))
				return false;
			a.number = (double)(int32_t)be32(d + at);
			if (a.type == 'r' || a.type == 'm')
				a.number = 0;
			at += 4;
			break;
		case 'f': {
			if (!need(4))
				return false;
			const uint32_t bits = be32(d + at);
			float f;
			std::memcpy(&f, &bits, 4);
			a.number = f;
			at += 4;
			break;
		}
		case 'h':
		case 't':
			if (!need(8))
				return false;
			a.number = a.type == 'h' ? (double)(int64_t)be64(d + at) : 0.0;
			at += 8;
			break;
		case 'd': {
			if (!need(8))
				return false;
			const uint64_t bits = be64(d + at);
			double v;
			std::memcpy(&v, &bits, 8);
			a.number = v;
			at += 8;
			break;
		}
		case 's':
		case 'S':
			if (!osc_text(d, size, at, a.text))
				return false;
			break;
		case 'b': {
			if (!need(4))
				return false;
			const size_t n = be32(d + at);
			at += 4;
			const size_t padded = (n + 3) & ~(size_t)3;
			if (n > size || !need(padded))
				return false;
			at += padded;
			break;
		}
		case 'T':
			a.number = 1;
			break;
		case 'F':
		case 'N':
		case 'I':
			break;
		case '[':
		case ']':
			continue; // an array's ends: its values are read as they come
		default:
			return false; // a type we don't know the size of: can't read on
		}
		m.args.push_back(std::move(a));
	}
	return true;
}

size_t read_packet(const uint8_t *d, size_t size, const std::function<void(const OscMessage &)> &each, int depth)
{
	if (!d || size < 4)
		return 0;
	if (size >= 16 && std::memcmp(d, "#bundle", 8) == 0) {
		if (depth >= MOST_DEPTH)
			return 0;
		size_t at = 16, read = 0;
		while (at + 4 <= size) {
			const size_t n = be32(d + at);
			at += 4;
			if (n > size - at)
				break;
			read += read_packet(d + at, n, each, depth + 1);
			at += n;
		}
		return read;
	}
	OscMessage m;
	if (!read_message(d, size, m))
		return 0;
	each(m);
	return 1;
}

void put32(std::string &out, uint32_t v)
{
	out.push_back((char)(v >> 24));
	out.push_back((char)(v >> 16));
	out.push_back((char)(v >> 8));
	out.push_back((char)v);
}

void put_text(std::string &out, const std::string &s)
{
	out += s;
	out.push_back('\0');
	while (out.size() % 4)
		out.push_back('\0');
}

} // namespace

size_t read_osc(const uint8_t *data, size_t size, const std::function<void(const OscMessage &)> &each)
{
	return read_packet(data, size, each, 0);
}

std::string osc_message(const OscMessage &m)
{
	std::string out;
	put_text(out, m.address);
	std::string tags = ",";
	for (const OscArg &a : m.args)
		tags += a.type;
	put_text(out, tags);
	for (const OscArg &a : m.args) {
		switch (a.type) {
		case 'i':
			put32(out, (uint32_t)(int32_t)a.number);
			break;
		case 'f': {
			const float f = (float)a.number;
			uint32_t bits;
			std::memcpy(&bits, &f, 4);
			put32(out, bits);
			break;
		}
		case 'd': {
			uint64_t bits;
			std::memcpy(&bits, &a.number, 8);
			put32(out, (uint32_t)(bits >> 32));
			put32(out, (uint32_t)bits);
			break;
		}
		case 'h': {
			const uint64_t v = (uint64_t)(int64_t)a.number;
			put32(out, (uint32_t)(v >> 32));
			put32(out, (uint32_t)v);
			break;
		}
		case 's':
		case 'S':
			put_text(out, a.text);
			break;
		default:
			break; // T, F, N, I: no data
		}
	}
	return out;
}

std::string osc_bundle(const std::vector<std::string> &messages)
{
	std::string out("#bundle", 8);
	put32(out, 0);
	put32(out, 1); // "at once"
	for (const std::string &m : messages) {
		put32(out, (uint32_t)m.size());
		out += m;
	}
	return out;
}

// ---- The humanoid's bones ------------------------------------------------------

const char *const VMC_BONE_NAMES[VMC_BONES] = {
	"hips",
	"spine",
	"chest",
	"upperChest",
	"neck",
	"head",
	"leftEye",
	"rightEye",
	"jaw",
	"leftUpperLeg",
	"leftLowerLeg",
	"leftFoot",
	"leftToes",
	"rightUpperLeg",
	"rightLowerLeg",
	"rightFoot",
	"rightToes",
	"leftShoulder",
	"leftUpperArm",
	"leftLowerArm",
	"leftHand",
	"rightShoulder",
	"rightUpperArm",
	"rightLowerArm",
	"rightHand",
	"leftThumbMetacarpal",
	"leftThumbProximal",
	"leftThumbDistal",
	"leftIndexProximal",
	"leftIndexIntermediate",
	"leftIndexDistal",
	"leftMiddleProximal",
	"leftMiddleIntermediate",
	"leftMiddleDistal",
	"leftRingProximal",
	"leftRingIntermediate",
	"leftRingDistal",
	"leftLittleProximal",
	"leftLittleIntermediate",
	"leftLittleDistal",
	"rightThumbMetacarpal",
	"rightThumbProximal",
	"rightThumbDistal",
	"rightIndexProximal",
	"rightIndexIntermediate",
	"rightIndexDistal",
	"rightMiddleProximal",
	"rightMiddleIntermediate",
	"rightMiddleDistal",
	"rightRingProximal",
	"rightRingIntermediate",
	"rightRingDistal",
	"rightLittleProximal",
	"rightLittleIntermediate",
	"rightLittleDistal",
};

namespace {

std::string lower(const std::string &s)
{
	std::string out = s;
	for (char &c : out)
		c = (char)std::tolower((unsigned char)c);
	return out;
}

bool ends_with(const std::string &s, const char *end)
{
	const size_t n = std::strlen(end);
	return s.size() >= n && s.compare(s.size() - n, n, end) == 0;
}

// Unity's names (lower case) -> ours. VMC's names are Unity's, so its
// "LeftThumbProximal" is the thumb's first bone (VRM 1.0's
// leftThumbMetacarpal); 1.0's own "...ThumbMetacarpal" is read too.
const std::map<std::string, int> &bone_names()
{
	static const std::map<std::string, int> names = [] {
		std::map<std::string, int> m;
		for (int i = 0; i < VMC_BONES; i++)
			m[lower(VMC_BONE_NAMES[i])] = i;
		// Then Unity's thumbs over 1.0's ("...ThumbProximal" is Unity's
		// first bone, 1.0's second).
		for (int i = 0; i < VMC_BONES; i++) {
			const std::string n = lower(VMC_BONE_NAMES[i]);
			if (ends_with(n, "thumbmetacarpal"))
				m[n.substr(0, n.size() - 10) + "proximal"] = i;
			else if (ends_with(n, "thumbproximal"))
				m[n.substr(0, n.size() - 8) + "intermediate"] = i;
		}
		return m;
	}();
	return names;
}

float clamp01(float v)
{
	return v > 0 ? (v < 1 ? v : 1) : 0;
}

bool fresh(double when, double now)
{
	return when >= 0 && now - when <= VMC_FRESH && now >= when - 1;
}

} // namespace

int vmc_bone(const std::string &name)
{
	const auto &names = bone_names();
	const auto it = names.find(lower(name));
	return it == names.end() ? -1 : it->second;
}

int vmc_other_side(int bone)
{
	if (bone < 0 || bone >= VMC_BONES)
		return bone;
	const std::string n = VMC_BONE_NAMES[bone];
	std::string other;
	if (n.rfind("left", 0) == 0)
		other = "right" + n.substr(4);
	else if (n.rfind("right", 0) == 0)
		other = "left" + n.substr(5);
	else
		return bone;
	for (int i = 0; i < VMC_BONES; i++)
		if (other == VMC_BONE_NAMES[i])
			return i;
	return bone;
}

VmcPart vmc_part(int bone)
{
	if (bone >= VMC_LEFT_THUMB_METACARPAL)
		return VMC_PART_FINGERS;
	if (bone >= VMC_LEFT_SHOULDER)
		return VMC_PART_ARMS;
	if (bone >= VMC_LEFT_UPPER_LEG)
		return VMC_PART_LEGS;
	if (bone >= VMC_NECK)
		return VMC_PART_HEAD;
	return VMC_PART_BODY;
}

// ---- What an app sent ----------------------------------------------------------

namespace {

// A face's shapes this many at most (an app's own and ARKit's 52 are far
// fewer; past this a packet isn't one).
constexpr size_t MOST_SHAPES = 512;

void set_shape(std::vector<std::pair<std::string, float>> &shapes, const std::string &name, float value)
{
	for (auto &s : shapes)
		if (s.first == name) {
			s.second = value;
			return;
		}
	if (shapes.size() < MOST_SHAPES)
		shapes.emplace_back(name, value);
}

} // namespace

VmcSent::VmcSent()
{
	for (int i = 0; i < VMC_BONES; i++) {
		when[i] = -1;
		turn[i][3] = 1;
	}
}

void VmcReader::clear()
{
	sent_ = VmcSent();
	pending_.clear();
	applies_ = false;
}

void VmcReader::take(const OscMessage &m, double now)
{
	sent_.messages++;
	const std::vector<OscArg> &a = m.args;
	auto numbers = [&a](size_t from, size_t count) {
		if (a.size() < from + count)
			return false;
		for (size_t k = from; k < from + count; k++)
			if (!a[k].is_number() || !std::isfinite(a[k].number))
				return false;
		return true;
	};
	if (m.address == "/VMC/Ext/Bone/Pos") {
		if (a.empty() || a[0].type != 's' || !numbers(1, 7)) {
			sent_.skipped++;
			return;
		}
		const int b = vmc_bone(a[0].text);
		if (b < 0) {
			sent_.skipped++;
			return;
		}
		for (int k = 0; k < 3; k++)
			sent_.at[b][k] = (float)a[1 + k].number;
		for (int k = 0; k < 4; k++)
			sent_.turn[b][k] = (float)a[4 + k].number;
		sent_.when[b] = now;
		sent_.changes++;
	} else if (m.address == "/VMC/Ext/Blend/Val") {
		if (a.size() < 2 || a[0].type != 's' || a[0].text.empty() || !numbers(1, 1)) {
			sent_.skipped++;
			return;
		}
		set_shape(pending_, a[0].text, (float)a[1].number);
	} else if (m.address == "/VMC/Ext/Blend/Apply") {
		applies_ = true;
		for (const auto &p : pending_)
			set_shape(sent_.shapes, p.first, p.second);
		pending_.clear();
		sent_.shapes_when = now;
		sent_.changes++;
	} else if (m.address == "/VMC/Ext/OK") {
		if (numbers(0, 1))
			sent_.loaded = (int)a[0].number;
		if (numbers(1, 1))
			sent_.calibration = (int)a[1].number;
	} else if (m.address != "/VMC/Ext/Root/Pos" && m.address != "/VMC/Ext/T") {
		sent_.skipped++;
	}
}

void VmcReader::packet_done(double now)
{
	if (applies_ || pending_.empty())
		return;
	for (const auto &p : pending_)
		set_shape(sent_.shapes, p.first, p.second);
	pending_.clear();
	sent_.shapes_when = now;
	sent_.changes++;
}

std::string vmc_plain(const std::string &name)
{
	std::string n = lower(name);
	// "blendShape1.eyeBlinkLeft" (a mesh's own prefix) -> its last part.
	const size_t dot = n.rfind('.');
	if (dot != std::string::npos && dot + 1 < n.size())
		n = n.substr(dot + 1);
	if (ends_with(n, "_l"))
		n = n.substr(0, n.size() - 2) + "left";
	else if (ends_with(n, "_r"))
		n = n.substr(0, n.size() - 2) + "right";
	std::string out;
	for (char c : n)
		if (std::isalnum((unsigned char)c))
			out += c;
	return out;
}

namespace {

// ARKit's names (plain) -> ours: VMC's are its character's own sides, and
// so are ours on a character (the tracker's are yours: the caller swaps
// them for a mirror) - so as they are.
const std::map<std::string, int> &arkit_names()
{
	static const std::map<std::string, int> names = [] {
		std::map<std::string, int> m;
		for (int i = 1; i < EXPRESSIONS; i++)
			m[vmc_plain(EXPRESSION_NAMES[i])] = i;
		return m;
	}();
	return names;
}

int expression(const char *name)
{
	const auto &n = arkit_names();
	const auto it = n.find(vmc_plain(name));
	return it == n.end() ? -1 : it->second;
}

} // namespace

VmcSends vmc_sends(const VmcSent &s, double now)
{
	VmcSends out;
	for (int b = 0; b < VMC_BONES; b++) {
		if (!fresh(s.when[b], now))
			continue;
		out.bones++;
		switch (vmc_part(b)) {
		case VMC_PART_BODY:
			out.body = true;
			break;
		case VMC_PART_LEGS:
			out.legs = true;
			break;
		case VMC_PART_HEAD:
			out.head = true;
			if (b == VMC_LEFT_EYE || b == VMC_RIGHT_EYE)
				out.eyes = true;
			break;
		case VMC_PART_ARMS:
			out.arms = true;
			break;
		case VMC_PART_FINGERS:
			out.fingers = true;
			break;
		}
	}
	if (fresh(s.shapes_when, now)) {
		int arkit = 0;
		for (const auto &sh : s.shapes) {
			out.shapes++;
			if (arkit_names().count(vmc_plain(sh.first)))
				arkit++;
		}
		out.perfect_sync = arkit >= 10;
	}
	return out;
}

Quat vmc_turn(const float q[4])
{
	return {q[0], -q[1], -q[2], q[3]};
}

V3 vmc_place(const float p[3])
{
	return {-p[0], p[1], p[2]};
}

VmcBones vmc_bones(const VmcSent &s, double now, bool mirror)
{
	VmcBones out;
	for (int b = 0; b < VMC_BONES; b++) {
		const int from = mirror ? vmc_other_side(b) : b;
		if (!fresh(s.when[from], now))
			continue;
		Quat q = vmc_turn(s.turn[from]);
		V3 p = vmc_place(s.at[from]);
		if (mirror) {
			q = {q.x, -q.y, -q.z, q.w};
			p.x = -p.x;
		}
		const float n = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
		if (!(n > 1e-6f) || !std::isfinite(n) || !std::isfinite(p.x + p.y + p.z))
			continue;
		out.turn[b] = normalized(q);
		out.at[b] = p;
		out.has[b] = true;
	}
	return out;
}

namespace {

// VRM's faces, by its 0.x and 1.0 names (plain) -> 1.0's.
const char *vrm_face(const std::string &plain)
{
	static const std::map<std::string, const char *> faces = {
		{"joy", "happy"}, {"happy", "happy"}, {"angry", "angry"},     {"sorrow", "sad"},
		{"sad", "sad"},   {"fun", "relaxed"}, {"relaxed", "relaxed"}, {"surprised", "surprised"},
	};
	const auto it = faces.find(plain);
	return it == faces.end() ? nullptr : it->second;
}

// The row-after-row 4 x 4 of a turn (the tracker's pose's way).
void rows_of(Quat q, float m[16])
{
	q = normalized(q);
	const float x = q.x, y = q.y, z = q.z, w = q.w;
	const float r[9] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),
			    2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
			    2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y)};
	for (int i = 0; i < 16; i++)
		m[i] = 0;
	for (int row = 0; row < 3; row++)
		for (int col = 0; col < 3; col++)
			m[row * 4 + col] = r[row * 3 + col];
	m[15] = 1;
}

} // namespace

bool vmc_face(const VmcSent &s, double now, PhoneLook &look, std::vector<std::pair<std::string, float>> &faces)
{
	look = PhoneLook();
	faces.clear();
	const bool shapes = fresh(s.shapes_when, now) && !s.shapes.empty();
	const bool neck = fresh(s.when[VMC_NECK], now), head = fresh(s.when[VMC_HEAD], now);
	if (!shapes && !neck && !head)
		return false;
	look.found = true;
	float *ex = look.expressions;
	bool looks_said = false;
	if (shapes) {
		// Apple's ARKit shapes ("perfect sync"): its mouth, eyes and looks
		// are those, and VRM's own blinks, vowels and looks beside them
		// (an app sends both for a character that has both) aren't taken
		// as well.
		int arkit = 0;
		for (const auto &sh : s.shapes)
			if (arkit_names().count(vmc_plain(sh.first)))
				arkit++;
		const bool perfect = arkit >= 10;
		MicMouth vowels;
		bool any_vowel = false;
		float blink = 0, blink_side[2] = {0, 0}, up = 0, down = 0, toward_left = 0, toward_right = 0;
		bool any_blink = false, any_look = false;
		std::vector<int> seen;
		auto count = [&seen, &look](int id) {
			if (std::find(seen.begin(), seen.end(), id) == seen.end()) {
				seen.push_back(id);
				look.shapes++;
			}
		};
		for (const auto &sh : s.shapes) {
			const std::string p = vmc_plain(sh.first);
			const float v = clamp01(std::isfinite(sh.second) ? sh.second : 0.0f);
			const auto ark = arkit_names().find(p);
			if (ark != arkit_names().end()) {
				ex[ark->second] = std::max(ex[ark->second], v);
				count(ark->second);
				if (p.rfind("eyelook", 0) == 0)
					looks_said = true;
				continue;
			}
			if (p == "tongueout") {
				count(EXPRESSIONS);
				faces.emplace_back(sh.first, v);
				continue;
			}
			static const char *const vowel_names[VOWELS][2] = {{"a", "aa"},
									   {"i", "ih"},
									   {"u", "ou"},
									   {"e", "ee"},
									   {"o", "oh"}};
			int vowel = -1;
			for (int k = 0; k < VOWELS; k++)
				if (p == vowel_names[k][0] || p == vowel_names[k][1])
					vowel = k;
			if (vowel >= 0) {
				if (!perfect) {
					vowels.vowel[vowel] = std::max(vowels.vowel[vowel], v);
					any_vowel = true;
					count(EXPRESSIONS + 1 + vowel);
				}
				continue;
			}
			if (p == "blink" || p == "blinkleft" || p == "blinkright") {
				if (!perfect) {
					if (p == "blink")
						blink = std::max(blink, v);
					else
						blink_side[p == "blinkright"] =
							std::max(blink_side[p == "blinkright"], v);
					any_blink = true;
				}
				continue;
			}
			if (p == "lookup" || p == "lookdown" || p == "lookleft" || p == "lookright") {
				if (!perfect) {
					(p == "lookup"     ? up
					 : p == "lookdown" ? down
					 : p == "lookleft" ? toward_left
							   : toward_right) = v;
					any_look = true;
					looks_said = true;
				}
				continue;
			}
			if (const char *f = vrm_face(p)) {
				faces.emplace_back(f, v);
				continue;
			}
			if (p == "neutral" || p.empty())
				continue;
			// Its own (a heart-eyes face, a blush...): by its name, for a
			// character that has one too.
			faces.emplace_back(sh.first, v);
		}
		if (any_vowel) {
			vowels.share = 1;
			mic_into_moves(vowels, ex);
		}
		static const int blink_l = expression("eyeBlinkLeft"), blink_r = expression("eyeBlinkRight");
		if (any_blink) {
			ex[blink_l] = std::max(blink, blink_side[0]);
			ex[blink_r] = std::max(blink, blink_side[1]);
		}
		if (any_look) {
			// Its left is +x: towards its left its left eye looks out and
			// its right eye in (rig.cpp's vrm_values, backwards).
			ex[expression("eyeLookUpLeft")] = ex[expression("eyeLookUpRight")] = up;
			ex[expression("eyeLookDownLeft")] = ex[expression("eyeLookDownRight")] = down;
			ex[expression("eyeLookOutLeft")] = ex[expression("eyeLookInRight")] = toward_left;
			ex[expression("eyeLookInLeft")] = ex[expression("eyeLookOutRight")] = toward_right;
		}
	}
	// Its eyes' looks from its eye bones, when its shapes don't say.
	if (!looks_said) {
		float side = 0, upward = 0;
		int eyes = 0;
		for (int b : {VMC_LEFT_EYE, VMC_RIGHT_EYE}) {
			if (!fresh(s.when[b], now))
				continue;
			const V3 f = rotate(normalized(vmc_turn(s.turn[b])), {0, 0, 1});
			if (!std::isfinite(f.x + f.y + f.z))
				continue;
			side += std::atan2(f.x, f.z) * 180 / 3.14159265f;
			upward += std::atan2(f.y, std::sqrt(f.x * f.x + f.z * f.z)) * 180 / 3.14159265f;
			eyes++;
		}
		if (eyes) {
			side = std::max(-1.0f, std::min(1.0f, side / eyes / VMC_EYE_RANGE));
			upward = std::max(-1.0f, std::min(1.0f, upward / eyes / VMC_EYE_RANGE));
			const float l = std::max(0.0f, side), r = std::max(0.0f, -side);
			ex[expression("eyeLookOutLeft")] = ex[expression("eyeLookInRight")] = l;
			ex[expression("eyeLookInLeft")] = ex[expression("eyeLookOutRight")] = r;
			ex[expression("eyeLookUpLeft")] = ex[expression("eyeLookUpRight")] = std::max(0.0f, upward);
			ex[expression("eyeLookDownLeft")] = ex[expression("eyeLookDownRight")] =
				std::max(0.0f, -upward);
		}
	}
	// Its head's turn against its chest: its neck's, then its head's.
	Quat q;
	if (neck)
		q = normalized(vmc_turn(s.turn[VMC_NECK]));
	if (head)
		q = q * normalized(vmc_turn(s.turn[VMC_HEAD]));
	if (std::isfinite(q.x + q.y + q.z + q.w)) {
		float rows[16];
		rows_of(q, rows);
		look.turn = turn_of(rows);
	}
	return true;
}

// ---- The link ------------------------------------------------------------------

namespace {

#ifdef _WIN32
typedef SOCKET sock_t;
const sock_t NO_SOCKET = INVALID_SOCKET;
void close_socket(sock_t s)
{
	closesocket(s);
}
int poll_now(pollfd *fds, size_t count, int ms)
{
	return WSAPoll(fds, (ULONG)count, ms);
}
bool nonblocking(sock_t s)
{
	u_long on = 1;
	return ioctlsocket(s, FIONBIO, &on) == 0;
}
#else
typedef int sock_t;
const sock_t NO_SOCKET = -1;
void close_socket(sock_t s)
{
	::close(s);
}
int poll_now(pollfd *fds, size_t count, int ms)
{
	return ::poll(fds, (nfds_t)count, ms);
}
bool nonblocking(sock_t s)
{
	const int flags = fcntl(s, F_GETFL, 0);
	return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}
#endif

// A port it can't listen on is tried again this often (seconds); another
// app sending is taken over once the first is quiet this long.
constexpr double RETRY = 2.0, TAKE_OVER = 1.0;

// Where a packet came from, as the page says it: "this PC", or its
// address (an IPv4 one as itself, not mapped into IPv6).
std::string where_text(const sockaddr_storage &a)
{
	char text[INET6_ADDRSTRLEN] = {};
	if (a.ss_family == AF_INET) {
		const auto &four = reinterpret_cast<const sockaddr_in &>(a);
		if ((ntohl(four.sin_addr.s_addr) >> 24) == 127)
			return "this PC";
		inet_ntop(AF_INET, &four.sin_addr, text, sizeof(text));
	} else if (a.ss_family == AF_INET6) {
		const auto &six = reinterpret_cast<const sockaddr_in6 &>(a).sin6_addr;
		static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
		static const uint8_t loop[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
		const uint8_t *b = reinterpret_cast<const uint8_t *>(&six);
		if (std::memcmp(b, loop, 16) == 0)
			return "this PC";
		if (std::memcmp(b, mapped, 12) == 0) {
			if (b[12] == 127)
				return "this PC";
			in_addr four;
			std::memcpy(&four, b + 12, 4);
			inet_ntop(AF_INET, &four, text, sizeof(text));
		} else {
			inet_ntop(AF_INET6, &six, text, sizeof(text));
		}
	}
	return text;
}

// The same app: the same address and port.
bool same_sender(const sockaddr_storage &a, const sockaddr_storage &b)
{
	if (a.ss_family != b.ss_family)
		return false;
	if (a.ss_family == AF_INET) {
		const auto &x = reinterpret_cast<const sockaddr_in &>(a), &y = reinterpret_cast<const sockaddr_in &>(b);
		return x.sin_port == y.sin_port && x.sin_addr.s_addr == y.sin_addr.s_addr;
	}
	if (a.ss_family == AF_INET6) {
		const auto &x = reinterpret_cast<const sockaddr_in6 &>(a),
			   &y = reinterpret_cast<const sockaddr_in6 &>(b);
		return x.sin6_port == y.sin6_port && std::memcmp(&x.sin6_addr, &y.sin6_addr, sizeof(in6_addr)) == 0;
	}
	return false;
}

std::mutex links_lock;
std::map<int, std::weak_ptr<VmcLink>> links;

} // namespace

double VmcLink::clock()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct VmcLink::Impl {
	std::thread thread;
	mutable std::mutex lock;
	std::atomic<bool> stop{false};
	State state;
	VmcReader reader;
	std::vector<OscMessage> messages; // one packet's (the thread's own)
#ifdef _WIN32
	bool wsa = false;
#endif
	void run(int port);
	void set_problem(const std::string &words)
	{
		std::lock_guard<std::mutex> l(lock);
		state.problem = words;
		state.listening = false;
	}
};

std::shared_ptr<VmcLink> VmcLink::open(int port)
{
	std::lock_guard<std::mutex> l(links_lock);
	auto it = links.find(port);
	if (it != links.end())
		if (auto link = it->second.lock())
			return link;
	auto link = std::make_shared<VmcLink>(port);
	links[port] = link;
	return link;
}

VmcLink::VmcLink(int port) : port_(port), impl_(std::make_unique<Impl>())
{
	impl_->state.port = port;
#ifdef _WIN32
	WSADATA data;
	impl_->wsa = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif
	Impl *m = impl_.get();
	impl_->thread = std::thread([m, port] { m->run(port); });
}

VmcLink::~VmcLink()
{
	impl_->stop = true;
	if (impl_->thread.joinable())
		impl_->thread.join();
#ifdef _WIN32
	if (impl_->wsa)
		WSACleanup();
#endif
}

VmcLink::State VmcLink::state() const
{
	std::lock_guard<std::mutex> l(impl_->lock);
	return impl_->state;
}

void VmcLink::sent(VmcSent &out, double &now) const
{
	std::lock_guard<std::mutex> l(impl_->lock);
	out = impl_->reader.sent();
	now = clock();
}

uint64_t VmcLink::changes() const
{
	std::lock_guard<std::mutex> l(impl_->lock);
	return impl_->reader.sent().changes;
}

void VmcLink::Impl::run(int port)
{
	sock_t sock = NO_SOCKET;
	double last_try = -1e9, last_packet = -1e9, window = clock();
	uint64_t window_packets = 0;
	sockaddr_storage sender{};
	bool have_sender = false, any = false;
	// A VMC packet is a UDP packet: 64 KB at most.
	std::vector<uint8_t> buffer(65536);

	// Listening on every network this PC is on, IPv6 and IPv4 at once
	// where it can (else IPv4).
	auto open_now = [&]() -> bool {
		last_try = clock();
		if (port < 1 || port > 65535) {
			set_problem("That port can't be one - pick one from 1024 to 65535 (39539 is VMC's own).");
			return false;
		}
		bool in_use = false;
		for (int family : {AF_INET6, AF_INET}) {
			sock_t s = ::socket(family, SOCK_DGRAM, IPPROTO_UDP);
			if (s == NO_SOCKET)
				continue;
			sockaddr_storage here{};
			socklen_t here_len = 0;
			if (family == AF_INET6) {
				int off = 0;
				if (setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char *>(&off),
					       sizeof(off)) != 0) {
					close_socket(s);
					continue;
				}
				auto &six = reinterpret_cast<sockaddr_in6 &>(here);
				six.sin6_family = AF_INET6; // any address: all 0 already
				six.sin6_port = htons((uint16_t)port);
				here_len = sizeof(sockaddr_in6);
			} else {
				auto &four = reinterpret_cast<sockaddr_in &>(here);
				four.sin_family = AF_INET;
				four.sin_addr.s_addr = htonl(INADDR_ANY);
				four.sin_port = htons((uint16_t)port);
				here_len = sizeof(sockaddr_in);
			}
			if (::bind(s, reinterpret_cast<sockaddr *>(&here), here_len) != 0) {
#ifdef _WIN32
				in_use = in_use || WSAGetLastError() == WSAEADDRINUSE || WSAGetLastError() == WSAEACCES;
#else
				in_use = in_use || errno == EADDRINUSE;
#endif
				close_socket(s);
				continue;
			}
			if (!nonblocking(s)) {
				close_socket(s);
				continue;
			}
			// Room for a burst (a whole body's bones every frame).
			int room = 1 << 20;
			setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&room), sizeof(room));
			sock = s;
			std::lock_guard<std::mutex> l(lock);
			state.listening = true;
			state.problem.clear();
			return true;
		}
		set_problem(in_use ? "Another app on this PC is listening on port " + std::to_string(port) +
					     " (VSeeFace, VMagicMirror or Warudo taking VMC in, say) - close it, or "
					     "pick another port here and in your tracker app."
				   : "This PC wouldn't let OBS listen on port " + std::to_string(port) + ".");
		return false;
	};
	while (!stop) {
		if (sock == NO_SOCKET) {
			if (clock() - last_try >= RETRY)
				open_now();
			if (sock == NO_SOCKET) {
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
				continue;
			}
		}
		pollfd fds{};
		fds.fd = sock;
		fds.events = POLLIN;
		const int ready = poll_now(&fds, 1, 50);
		if (ready > 0 && (fds.revents & POLLIN)) {
			// Everything waiting (the socket doesn't wait for more).
			for (int k = 0; k < 256; k++) {
				sockaddr_storage from{};
				socklen_t from_len = sizeof(from);
				const auto n = ::recvfrom(sock, reinterpret_cast<char *>(buffer.data()),
							  (int)buffer.size(), 0, reinterpret_cast<sockaddr *>(&from),
							  &from_len);
				if (n <= 0)
					break;
				const double now = clock();
				const bool new_app = !have_sender || !same_sender(from, sender);
				std::lock_guard<std::mutex> l(lock);
				// Another app while the first is still sending: counted.
				if (have_sender && new_app && now - last_packet < TAKE_OVER) {
					state.others++;
					state.other = where_text(from);
					continue;
				}
				messages.clear();
				if (!read_osc(buffer.data(), (size_t)n,
					      [this](const OscMessage &m) { messages.push_back(m); })) {
					state.unread++;
					continue;
				}
				if (new_app) {
					// A new app (or the same one again from a new port):
					// nothing of the last one's kept.
					reader.clear();
					sender = from;
					have_sender = true;
					state.from = where_text(from);
				}
				for (const OscMessage &m : messages)
					reader.take(m, now);
				reader.packet_done(now);
				last_packet = now;
				any = true;
				window_packets++;
				state.packets++;
			}
		} else if (ready < 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		const double now = clock(), span = now - window;
		std::lock_guard<std::mutex> l(lock);
		if (span >= 1.0) {
			state.per_second = (float)((double)window_packets / span);
			window_packets = 0;
			window = now;
		}
		state.since = any ? (float)(now - last_packet) : -1.0f;
	}
	if (sock != NO_SOCKET)
		close_socket(sock);
	std::lock_guard<std::mutex> l(lock);
	state.listening = false;
}

} // namespace direbot::face
