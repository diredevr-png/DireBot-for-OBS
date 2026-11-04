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

#include "rig.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <set>

namespace direbot::face {

namespace {

// Arms brought down to this far from straight down (radians).
constexpr float ARM_FROM_SIDE = 0.3f;

constexpr float PI_F = 3.14159265f;
constexpr float RAD = PI_F / 180;
// Swinging parts move in steps of a 60th of a second, whatever OBS's
// frame rate, so they swing the same at 30 and 60 frames a second.
constexpr float SPRING_STEP = 1.0f / 60;
constexpr int MOST_STEPS = 4;
// A set-up chain slows only a little (Lens Studio's swing doesn't at all).
constexpr float PULLED_DRAG = 0.02f;
// How far your eyes turn at their most (degrees), for a full look.
constexpr float EYE_SIDEWAYS = 30, EYE_UPDOWN = 25;

float clamp01(float v)
{
	return std::min(1.0f, std::max(0.0f, v));
}
float ramp(float v, float from, float to)
{
	return clamp01((v - from) / (to - from));
}

// A name as it's compared: lower case, without its prefix ("blendShape1.",
// "vrc.") or spaces, dashes and underscores; "_L"/"_R" as "left"/"right".
std::string plain(const std::string &name)
{
	std::string s = name;
	const size_t dot = s.find_last_of('.');
	if (dot != std::string::npos && dot + 1 < s.size())
		s = s.substr(dot + 1);
	std::string low;
	for (char c : s)
		low.push_back((char)std::tolower((unsigned char)c));
	auto ends = [&low](const char *tail) {
		const size_t n = std::strlen(tail);
		return low.size() > n && low.compare(low.size() - n, n, tail) == 0;
	};
	if (ends("_l") || ends("-l") || ends(" l"))
		low = low.substr(0, low.size() - 2) + "left";
	else if (ends("_r") || ends("-r") || ends(" r"))
		low = low.substr(0, low.size() - 2) + "right";
	std::string out;
	for (char c : low)
		if (c != '_' && c != '-' && c != ' ')
			out.push_back(c);
	return out;
}

// Hair, a tail, a skirt...: dangly parts even on a person's skeleton.
bool dangly_name(std::string n)
{
	for (char &c : n)
		c = (char)std::tolower((unsigned char)c);
	for (const char *w : {"hair", "tail", "skirt", "ribbon", "cape", "chain", "string", "rope", "tassel"})
		if (n.find(w) != std::string::npos)
			return true;
	return false;
}

// The usual names other tools give the expressions, as VRM's.
std::string usual_expression(const std::string &name)
{
	static const std::pair<const char *, const char *> names[] = {
		{"blink", "blink"},
		{"blinkboth", "blink"},
		{"eyesclosed", "blink"},
		{"eyesclose", "blink"},
		{"closeeyes", "blink"},
		{"eyeclose", "blink"},
		{"\xe3\x81\xbe\xe3\x81\xb0\xe3\x81\x9f\xe3\x81\x8d", "blink"}, // まばたき
		{"blinkleft", "blinkLeft"},
		{"winkleft", "blinkLeft"},
		{"\xe3\x82\xa6\xe3\x82\xa3\xe3\x83\xb3\xe3\x82\xaf", "blinkLeft"}, // ウィンク
		{"blinkright", "blinkRight"},
		{"winkright", "blinkRight"},
		{"\xe3\x82\xa6\xe3\x82\xa3\xe3\x83\xb3\xe3\x82\xaf\xe5\x8f\xb3", "blinkRight"}, // ウィンク右
		{"a", "aa"},
		{"aa", "aa"},
		{"vaa", "aa"},
		{"mouthopen", "aa"},
		{"openmouth", "aa"},
		{"mouthopened", "aa"},
		{"\xe3\x81\x82", "aa"}, // あ
		{"i", "ih"},
		{"ih", "ih"},
		{"vih", "ih"},
		{"\xe3\x81\x84", "ih"}, // い
		{"u", "ou"},
		{"ou", "ou"},
		{"vou", "ou"},
		{"\xe3\x81\x86", "ou"}, // う
		{"e", "ee"},
		{"ee", "ee"},
		{"ve", "ee"},
		{"\xe3\x81\x88", "ee"}, // え
		{"o", "oh"},
		{"oh", "oh"},
		{"voh", "oh"},
		{"\xe3\x81\x8a", "oh"}, // お
		{"smile", "happy"},
		{"joy", "happy"},
		{"happy", "happy"},
		{"\xe7\xac\x91\xe3\x81\x84", "happy"}, // 笑い
		{"angry", "angry"},
		{"\xe6\x80\x92\xe3\x82\x8a", "angry"}, // 怒り
		{"sad", "sad"},
		{"sorrow", "sad"},
		{"surprised", "surprised"},
		{"surprise", "surprised"},
	};
	const std::string p = plain(name);
	for (const auto &n : names)
		if (p == n.first)
			return n.second;
	return {};
}

int move_id(const char *name)
{
	for (int i = 0; i < EXPRESSIONS; i++)
		if (std::strcmp(EXPRESSION_NAMES[i], name) == 0)
			return i;
	return 0;
}

// VRM's expressions from your 52 (0 to 1 each, already tuned: alive.hpp's
// tune_moves - each starts where your face's tuning says and is full where
// it says). `per_eye`: the character has a blink for each eye, so a wink is
// one and a blink both. A blink, the mouth's shapes and the eyes' looks
// follow the tuned moves as they are; the whole-face expressions (happy,
// angry, sad, surprised) only from half way or so, as a big smile, frown
// or raised brows - a VRM's "happy" often shuts the eyes too.
std::map<std::string, float> vrm_values(const float *mv, bool per_eye)
{
	static const int blink_l = move_id("eyeBlinkLeft"), blink_r = move_id("eyeBlinkRight"),
			 jaw = move_id("jawOpen"), funnel = move_id("mouthFunnel"), pucker = move_id("mouthPucker"),
			 stretch_l = move_id("mouthStretchLeft"), stretch_r = move_id("mouthStretchRight"),
			 smile_l = move_id("mouthSmileLeft"), smile_r = move_id("mouthSmileRight"),
			 frown_l = move_id("mouthFrownLeft"), frown_r = move_id("mouthFrownRight"),
			 brow_down_l = move_id("browDownLeft"), brow_down_r = move_id("browDownRight"),
			 brow_up = move_id("browInnerUp"), wide_l = move_id("eyeWideLeft"),
			 wide_r = move_id("eyeWideRight"), up_l = move_id("eyeLookUpLeft"),
			 up_r = move_id("eyeLookUpRight"), down_l = move_id("eyeLookDownLeft"),
			 down_r = move_id("eyeLookDownRight"), in_l = move_id("eyeLookInLeft"),
			 in_r = move_id("eyeLookInRight"), out_l = move_id("eyeLookOutLeft"),
			 out_r = move_id("eyeLookOutRight");
	std::map<std::string, float> v;
	const float l = clamp01(mv[blink_l]), r = clamp01(mv[blink_r]);
	if (per_eye) {
		const float both = std::min(l, r);
		v["blink"] = both;
		v["blinkLeft"] = l - both;
		v["blinkRight"] = r - both;
	} else {
		v["blink"] = (l + r) / 2;
		v["blinkLeft"] = l;
		v["blinkRight"] = r;
	}
	float aa = clamp01(mv[jaw]), oh = clamp01(mv[funnel]), ou = clamp01(mv[pucker]),
	      ee = ramp((mv[stretch_l] + mv[stretch_r]) / 2, 0.1f, 0.5f), ih = ee * 0.5f;
	const float mouth = aa + oh + ou + ee + ih;
	if (mouth > 1) {
		aa /= mouth;
		oh /= mouth;
		ou /= mouth;
		ee /= mouth;
		ih /= mouth;
	}
	v["aa"] = aa;
	v["ih"] = ih;
	v["ou"] = ou;
	v["ee"] = ee;
	v["oh"] = oh;
	v["happy"] = ramp((mv[smile_l] + mv[smile_r]) / 2, 0.45f, 1.0f);
	v["angry"] = ramp((mv[brow_down_l] + mv[brow_down_r]) / 2, 0.3f, 1.0f);
	v["sad"] = ramp((mv[frown_l] + mv[frown_r]) / 2, 0.3f, 0.8f);
	v["surprised"] = std::max(ramp(mv[brow_up], 0.4f, 1.0f), ramp((mv[wide_l] + mv[wide_r]) / 2, 0.35f, 0.85f));
	v["relaxed"] = 0;
	v["neutral"] = 0;
	v["lookUp"] = clamp01((mv[up_l] + mv[up_r]) / 2);
	v["lookDown"] = clamp01((mv[down_l] + mv[down_r]) / 2);
	// Its left is +x (it faces +z): its left eye looking out, its right in.
	v["lookLeft"] = clamp01((mv[out_l] + mv[in_r]) / 2);
	v["lookRight"] = clamp01((mv[in_l] + mv[out_r]) / 2);
	return v;
}

bool is_blink(const std::string &n)
{
	return n == "blink" || n == "blinkLeft" || n == "blinkRight";
}
bool is_look(const std::string &n)
{
	return n == "lookUp" || n == "lookDown" || n == "lookLeft" || n == "lookRight";
}
bool is_mouth(const std::string &n)
{
	return n == "aa" || n == "ih" || n == "ou" || n == "ee" || n == "oh";
}

Quat rot_of(const M4 &m)
{
	return quat_of(m);
}
float scale_of(const M4 &m)
{
	return length(V3{m.m[0], m.m[1], m.m[2]});
}
// A node by its name: a bone of a skin first, as a set-up names them.
int node_named(const Model &m, const std::string &name)
{
	int any = -1;
	for (const Skin &skin : m.skins)
		for (int j : skin.joints)
			if (j >= 0 && j < (int)m.nodes.size() && m.nodes[j].name == name)
				return j;
	for (size_t i = 0; i < m.nodes.size() && any < 0; i++)
		if (m.nodes[i].name == name)
			any = (int)i;
	return any;
}

int depth_of(const Model &m, int node)
{
	int d = 0;
	while (node >= 0 && d < 1000) {
		node = m.nodes[node].parent;
		d++;
	}
	return d;
}

// The whole-face expressions: a face on cue takes over from yours.
bool is_emotion(const std::string &n)
{
	return n == "happy" || n == "angry" || n == "sad" || n == "relaxed" || n == "surprised";
}
// VRM's own expression names (not a file's own).
bool is_vrm_preset(const std::string &n)
{
	return is_emotion(n) || is_blink(n) || is_look(n) || is_mouth(n) || n == "neutral";
}
std::string lower(const std::string &n)
{
	std::string out;
	for (char c : n)
		out.push_back((char)std::tolower((unsigned char)c));
	return out;
}

// The five faces from ARKit's shapes, for a character made for them.
struct Recipe {
	const char *face;
	std::vector<std::pair<const char *, float>> parts;
};
const std::vector<Recipe> &recipes()
{
	static const std::vector<Recipe> r = {
		{"happy",
		 {{"mouthSmileLeft", 0.85f},
		  {"mouthSmileRight", 0.85f},
		  {"cheekSquintLeft", 0.55f},
		  {"cheekSquintRight", 0.55f},
		  {"eyeSquintLeft", 0.45f},
		  {"eyeSquintRight", 0.45f},
		  {"mouthDimpleLeft", 0.2f},
		  {"mouthDimpleRight", 0.2f}}},
		{"angry",
		 {{"browDownLeft", 0.9f},
		  {"browDownRight", 0.9f},
		  {"eyeSquintLeft", 0.35f},
		  {"eyeSquintRight", 0.35f},
		  {"noseSneerLeft", 0.5f},
		  {"noseSneerRight", 0.5f},
		  {"mouthFrownLeft", 0.45f},
		  {"mouthFrownRight", 0.45f},
		  {"mouthPressLeft", 0.3f},
		  {"mouthPressRight", 0.3f}}},
		{"sad",
		 {{"browInnerUp", 0.85f},
		  {"mouthFrownLeft", 0.75f},
		  {"mouthFrownRight", 0.75f},
		  {"mouthShrugLower", 0.35f},
		  {"eyeSquintLeft", 0.15f},
		  {"eyeSquintRight", 0.15f}}},
		{"relaxed",
		 {{"eyeBlinkLeft", 0.3f},
		  {"eyeBlinkRight", 0.3f},
		  {"eyeSquintLeft", 0.35f},
		  {"eyeSquintRight", 0.35f},
		  {"mouthSmileLeft", 0.35f},
		  {"mouthSmileRight", 0.35f}}},
		{"surprised",
		 {{"eyeWideLeft", 0.9f},
		  {"eyeWideRight", 0.9f},
		  {"browInnerUp", 0.8f},
		  {"browOuterUpLeft", 0.9f},
		  {"browOuterUpRight", 0.9f},
		  {"jawOpen", 0.35f},
		  {"mouthFunnel", 0.15f}}},
	};
	return r;
}
// Your moves a whole-face expression on cue takes over from (your blinks,
// looks, jaw and mouth's shapes stay yours).
bool face_move(const char *n)
{
	for (const char *p : {"mouthSmile", "mouthFrown", "mouthPress", "mouthDimple", "mouthShrug", "browDown",
			      "browInnerUp", "browOuterUp", "cheekSquint", "cheekPuff", "noseSneer", "eyeWide",
			      "eyeSquint"})
		if (std::strncmp(n, p, std::strlen(p)) == 0)
			return true;
	return false;
}

// Where on a line from a to b is nearest p.
V3 nearest_on(V3 a, V3 b, V3 p)
{
	const V3 ab = b - a;
	const float l2 = dot(ab, ab);
	if (l2 < 1e-12f)
		return a;
	const float t = std::min(1.0f, std::max(0.0f, dot(p - a, ab) / l2));
	return a + ab * t;
}

} // namespace

int guess_turn(const Model &m)
{
	if (m.vrm == -1)
		return 180;
	if (m.is_vrm())
		return 0;
	const float wide = m.high.x - m.low.x, deep = m.high.z - m.low.z;
	return deep > wide * 1.25f ? 270 : 0;
}

int arkit_move(const std::string &name)
{
	const std::string p = plain(name);
	if (p.empty())
		return -1;
	for (int i = 1; i < EXPRESSIONS; i++)
		if (plain(EXPRESSION_NAMES[i]) == p)
			return i;
	return -1;
}

void swap_sides(float moves[EXPRESSIONS])
{
	static int other[EXPRESSIONS];
	static bool made = false;
	if (!made) {
		for (int i = 0; i < EXPRESSIONS; i++) {
			other[i] = i;
			std::string n = EXPRESSION_NAMES[i], m;
			const size_t l = n.rfind("Left"), r = n.rfind("Right");
			if (l != std::string::npos && l + 4 == n.size())
				m = n.substr(0, l) + "Right";
			else if (r != std::string::npos && r + 5 == n.size())
				m = n.substr(0, r) + "Left";
			if (!m.empty())
				for (int k = 0; k < EXPRESSIONS; k++)
					if (m == EXPRESSION_NAMES[k])
						other[i] = k;
		}
		made = true;
	}
	float copy[EXPRESSIONS];
	std::copy(moves, moves + EXPRESSIONS, copy);
	for (int i = 0; i < EXPRESSIONS; i++)
		moves[i] = copy[other[i]];
}

Rig::Rig(std::shared_ptr<const Model> model, const RigOptions &options) : model_(std::move(model)), options_(options)
{
	const Model &m = *model_;
	const size_t n = m.nodes.size();
	t_.resize(n);
	r_.resize(n);
	s_.resize(n);
	for (size_t i = 0; i < n; i++) {
		t_[i] = m.nodes[i].t;
		r_[i] = m.nodes[i].r;
		s_[i] = m.nodes[i].s;
	}
	rest_world_ = rest_world(m);
	world_ = rest_world_;
	held_.assign(n, 0);
	anim_turns_.assign(n, 0);
	if (options_.animation >= (int)m.animations.size())
		options_.animation = -1;
	head_ = m.bone("head");
	neck_ = m.bone("neck");
	left_eye_ = m.bone("leftEye");
	right_eye_ = m.bone("rightEye");
	// A character standing in a T (arms out straight, as most are made):
	// its arms brought down by its sides, as you'd stand.
	for (int side = 0; side < 2 && options_.arms_down; side++) {
		const int upper = m.bone(side ? "rightUpperArm" : "leftUpperArm");
		const int lower = m.bone(side ? "rightLowerArm" : "leftLowerArm");
		if (upper < 0 || lower < 0)
			continue;
		const V3 dir = move_of(rest_world_[lower]) - move_of(rest_world_[upper]);
		const V3 down{0, -1, 0};
		const V3 axis = cross(dir, down);
		if (length(dir) < 1e-6f || length(axis) < 1e-6f * length(dir))
			continue;
		const float from_down = std::acos(std::max(-1.0f, std::min(1.0f, dot(normalized(dir), down))));
		const float turn = from_down - ARM_FROM_SIDE;
		if (turn > 0.02f)
			arms_.push_back({upper, axis_angle(normalized(axis), turn)});
	}
	pivot_ = (m.low + m.high) * 0.5f;
	height_ = std::max(1e-4f, std::max(m.high.y - m.low.y, std::max(m.high.x - m.low.x, m.high.z - m.low.z)));
	// Its body: what breathes and follows your head.
	for (const char *name : {"spine", "chest", "upperChest"}) {
		const int b = m.bone(name);
		if (b >= 0 && b != head_ && b != neck_)
			torso_.push_back(b);
	}
	shoulder_[0] = m.bone("leftShoulder");
	shoulder_[1] = m.bone("rightShoulder");
	upper_arm_[0] = m.bone("leftUpperArm");
	upper_arm_[1] = m.bone("rightUpperArm");
	find_fingers();
	moved_.assign(n, 0);
	// Its bones by VRM 1.0's names: a 0.x's thumbs are named a joint on
	// (its proximal is 1.0's metacarpal, its intermediate 1.0's proximal).
	for (const auto &b : m.bones) {
		std::string name = b.first;
		if (m.vrm < 0)
			for (const std::string side : {"left", "right"}) {
				if (name == side + "ThumbProximal")
					name = side + "ThumbMetacarpal";
				else if (name == side + "ThumbIntermediate")
					name = side + "ThumbProximal";
			}
		if (b.second >= 0 && b.second < (int)n)
			bones1_[name] = b.second;
	}
	for (int side = 0; side < 2; side++) {
		const std::string sd = side ? "right" : "left";
		const int upper = m.bone(sd + "UpperArm"), lower = m.bone(sd + "LowerArm");
		if (upper >= 0 && lower >= 0) {
			arm_bone_[side][0] = upper;
			arm_bone_[side][1] = lower;
			arm_bone_[side][2] = m.bone(sd + "Hand");
		}
	}
	// Where its feet are, the facing model's way (for a VRM animation's
	// hips: its height over them).
	for (int c = 0; c < 8; c++) {
		const V3 corner{c & 1 ? m.high.x : m.low.x, c & 2 ? m.high.y : m.low.y, c & 4 ? m.high.z : m.low.z};
		const float y = rotate(options_.facing, corner).y;
		floor_ = c ? std::min(floor_, y) : y;
	}
	measure_head();
	find_arms();

	// Expressions: the morphs, each only where it moves something.
	sparse_.resize(m.meshes.size());
	weights_.resize(m.meshes.size());
	for (size_t mi = 0; mi < m.meshes.size(); mi++) {
		const Mesh &mesh = m.meshes[mi];
		weights_[mi] = mesh.weights;
		sparse_[mi].resize(mesh.parts.size());
		for (size_t pi = 0; pi < mesh.parts.size(); pi++) {
			const Primitive &p = mesh.parts[pi];
			sparse_[mi][pi].resize(p.morphs.size());
			for (size_t k = 0; k < p.morphs.size(); k++) {
				const MorphTarget &mt = p.morphs[k];
				Sparse &sp = sparse_[mi][pi][k];
				for (size_t v = 0; v < p.pos.size(); v++) {
					const V3 dp = mt.pos.empty() ? V3{} : mt.pos[v];
					const V3 dn = mt.normal.empty() ? V3{} : mt.normal[v];
					if (dot(dp, dp) > 1e-14f || dot(dn, dn) > 1e-10f) {
						sp.at.push_back((uint32_t)v);
						sp.pos.push_back(dp);
						sp.normal.push_back(dn);
					}
				}
			}
		}
	}
	// Every shape by its name (any case), for a face on cue by a shape's
	// own name.
	for (size_t mi = 0; mi < m.meshes.size(); mi++)
		for (size_t k = 0; k < m.meshes[mi].morph_names.size(); k++)
			if (!m.meshes[mi].morph_names[k].empty())
				morph_by_name_[lower(m.meshes[mi].morph_names[k])].push_back({(int)mi, (int)k});
	// What drives them: ARKit-named morphs straight from your moves; VRM's
	// expressions (or the usual names) worked out from them.
	std::map<std::string, bool> bound;
	for (const Expression &e : m.expressions)
		for (const Expression::Bind &b : e.binds) {
			Driver d;
			d.mesh = b.mesh;
			d.morph = b.morph;
			d.expression = e.name;
			d.weight = b.weight;
			drivers_.push_back(d);
			bound[std::to_string(b.mesh) + "/" + std::to_string(b.morph)] = true;
		}
	std::vector<Driver> arkit;
	for (size_t mi = 0; mi < m.meshes.size(); mi++)
		for (size_t k = 0; k < m.meshes[mi].morph_names.size(); k++) {
			const std::string &name = m.meshes[mi].morph_names[k];
			Driver d;
			d.mesh = (int)mi;
			d.morph = (int)k;
			d.move = arkit_move(name);
			if (d.move > 0) {
				arkit.push_back(d);
				continue;
			}
			if (m.is_vrm() || bound.count(std::to_string(mi) + "/" + std::to_string(k)))
				continue;
			d.move = -1;
			d.expression = usual_expression(name);
			if (!d.expression.empty())
				drivers_.push_back(d);
		}
	arkit_count_ = (int)arkit.size();
	// Made for ARKit ("perfect sync"): those alone, not VRM's on top.
	perfect_sync_ = arkit_count_ >= 10;
	if (perfect_sync_)
		drivers_.clear();
	drivers_.insert(drivers_.end(), arkit.begin(), arkit.end());

	// The faces your face doesn't move (their meshes).
	still_mesh_.assign(m.meshes.size(), 0);
	for (size_t i = 0; i < m.nodes.size(); i++) {
		const int mesh = m.nodes[i].mesh;
		if (mesh < 0 || mesh >= (int)m.meshes.size())
			continue;
		const std::string label = m.nodes[i].name.empty() ? "#" + std::to_string(i) : m.nodes[i].name;
		for (const std::string &s : options_.still_faces)
			if (s == label || s == "#" + std::to_string(i))
				still_mesh_[mesh] = 1;
	}

	// Whether its own blink and open mouth show: a shape that moves its
	// face at least FACE_SHOWS of its height (a real eyelid or jaw), not
	// a tiny marker (a shape only there to say where its eyes are).
	for (const Driver &d : drivers_) {
		const std::string name = d.move > 0 ? std::string(EXPRESSION_NAMES[d.move]) : d.expression;
		const bool blink = d.move > 0 ? name.rfind("eyeBlink", 0) == 0 : is_blink(name);
		const bool mouth = d.move > 0 ? name.rfind("jaw", 0) == 0 || name.rfind("mouth", 0) == 0 : is_mouth(name);
		if ((!blink || blinks_) && (!mouth || talks_))
			continue;
		float most = 0;
		if (d.mesh >= 0 && d.mesh < (int)sparse_.size())
			for (const std::vector<Sparse> &part : sparse_[d.mesh])
				if (d.morph >= 0 && d.morph < (int)part.size())
					for (const V3 &dp : part[d.morph].pos)
						most = std::max(most, length(dp));
		if (most * d.weight < FACE_SHOWS * height_)
			continue;
		blinks_ = blinks_ || blink;
		talks_ = talks_ || mouth;
	}

	// Parts that stay upright (a doll's body), as its set-up names them.
	const CharacterSetup *setup = options_.setup.get();
	if (setup)
		for (const UprightSetup &u : setup->upright) {
			Upright up;
			up.node = node_named(m, u.bone);
			up.follow = u.follow;
			if (up.node >= 0)
				upright_.push_back(up);
		}
	// Its own drawn face: the parts named, and all hanging off them.
	own_face_.assign(m.nodes.size(), 0);
	if (setup)
		for (const std::string &name : setup->own_face) {
			const int n = node_named(m, name);
			if (n >= 0)
				own_face_[n] = 1;
		}
	for (bool more = true; more;) {
		more = false;
		for (size_t i = 0; i < m.nodes.size(); i++) {
			const int p = m.nodes[i].parent;
			if (!own_face_[i] && p >= 0 && own_face_[p])
				own_face_[i] = more = true;
		}
	}

	// Swinging parts.
	if (!options_.springs)
		return;
	auto local_tail = [&](int node, int tail_node, V3 tail) {
		if (tail_node < 0)
			return tail;
		return point(inverse(rest_world_[node]), move_of(rest_world_[tail_node]));
	};
	for (const Spring &sp : m.springs) {
		Chain c;
		c.colliders = sp.colliders;
		for (const SpringJoint &sj : sp.joints) {
			Joint j;
			j.s = sj;
			j.rest = m.nodes[sj.node].r;
			j.tail = local_tail(sj.node, sj.tail_node, sj.tail);
			if (length(j.tail) < 1e-6f)
				continue;
			j.axis = normalized(j.tail);
			c.joints.push_back(j);
		}
		if (!c.joints.empty())
			springs_.push_back(c);
	}
	if (setup && setup->chains_given) {
		// The chains its set-up names (an imported Lens Studio project's):
		// each hangs from its first bone and swings, pulled as it says.
		std::vector<std::pair<int, Chain>> given;
		for (const ChainSetup &cs : setup->chains) {
			std::vector<int> nodes;
			for (const std::string &b : cs.bones)
				nodes.push_back(node_named(m, b));
			if (std::find(nodes.begin(), nodes.end(), -1) != nodes.end())
				continue;
			// Both ends held: the last link stays as it is.
			const size_t links = nodes.size() - 1 - (cs.both_ends && nodes.size() > 2 ? 1 : 0);
			Chain c;
			for (size_t i = 0; i < links; i++) {
				Joint j;
				j.s.node = nodes[i];
				j.s.tail_node = nodes[i + 1];
				j.tail = local_tail(nodes[i], nodes[i + 1], {});
				if (length(j.tail) < 1e-6f)
					continue;
				j.axis = normalized(j.tail);
				j.rest = m.nodes[nodes[i]].r;
				j.pulled = true;
				j.with_head = cs.with_head;
				j.pull = cs.pull;
				j.s.stiffness = 0;
				j.s.gravity = 0;
				j.s.drag = PULLED_DRAG;
				j.s.radius = 0;
				c.joints.push_back(j);
			}
			if (!c.joints.empty())
				given.push_back({depth_of(m, nodes[0]), c});
		}
		// A chain hanging off another swings after it.
		std::stable_sort(given.begin(), given.end(),
				 [](const auto &a, const auto &b) { return a.first < b.first; });
		for (auto &g : given)
			springs_.push_back(g.second);
	} else if (!m.is_vrm() && springs_.empty()) {
		// Its dangly parts (a chain of links, a doll's ears
		// and legs): each hangs from its first bone and swings, sized to the
		// character. A person's skeleton: only hair, tails and the like.
		for (const Skin &skin : m.skins) {
			const bool person = person_skeleton(m, skin);
			for (const std::vector<int> &chain : hanging_chains(m, skin)) {
				if (person && !dangly_name(m.nodes[chain[0]].name))
					continue;
				Chain c;
				for (size_t i = 1; i < chain.size(); i++) {
					Joint j;
					j.s.node = chain[i];
					if (i + 1 < chain.size()) {
						j.s.tail_node = chain[i + 1];
						j.tail = local_tail(chain[i], chain[i + 1], {});
					} else {
						// The last: as long again as the bone before it, on the
						// way it points.
						const Node &node = m.nodes[chain[i]];
						j.tail = rotate(conj(node.r), node.t);
					}
					if (length(j.tail) < 1e-6f)
						continue;
					j.axis = normalized(j.tail);
					j.rest = m.nodes[chain[i]].r;
					// In the character's own size (scaled by its world's
					// below); gravity only as far as it's tipped from upright
					// (step_springs), so it keeps its shape standing still.
					j.s.stiffness = 1.0f;
					j.s.gravity = 0.8f;
					j.s.drag = 0.06f;
					j.s.gravity_dir = {0, -1, 0};
					j.s.radius = 0;
					c.joints.push_back(j);
				}
				if (!c.joints.empty()) {
					springs_.push_back(c);
					auto_chain_ = true;
				}
			}
		}
	}
}

// `by` (a turn as the model at rest has it, round the node) on top of
// the node's own turn (at rest, or as its animation has it now).
void Rig::turn_in_rest(int node, Quat by)
{
	const Quat w = rot_of(rest_world_[node]);
	r_[node] = r_[node] * (conj(w) * by * w);
}

// A VRM's fingers, to relax: each joint bends towards its palm, about an
// axis across its finger, by how far a relaxed hand bends there - more at
// the middle joint than the knuckle and the tip, more on the little finger
// than the first (a resting hand's cascade) - less what its file already
// bends it, so a hand made relaxed isn't curled twice. The thumb's first
// joint drops a little towards the palm; past it, the thumb bends towards
// the first finger, across the palm, as a thumb's joints do (its nail
// faces out, not down like the fingers').
void Rig::find_fingers()
{
	const Model &m = *model_;
	auto at = [this](int n) {
		return move_of(rest_world_[n]);
	};
	// Each joint's bend at a full curl (degrees): knuckle, middle, tip.
	static const struct {
		const char *name;
		float bend[3];
	} DIGITS[] = {{"Index", {40, 60, 40}},
		      {"Middle", {45, 65, 42}},
		      {"Ring", {50, 70, 45}},
		      {"Little", {55, 75, 48}},
		      {"Thumb", {15, 25, 30}}};
	for (int side = 0; side < 2; side++) {
		const std::string s = side ? "right" : "left";
		const int hand = m.bone(s + "Hand");
		if (hand < 0)
			continue;
		auto bone = [&](const char *digit, const char *joint) {
			return m.bone(s + digit + joint);
		};
		const int index = bone("Index", "Proximal"), middle = bone("Middle", "Proximal");
		int little = bone("Little", "Proximal");
		if (little < 0)
			little = bone("Ring", "Proximal");
		const int ahead = middle >= 0 ? middle : index;
		if (ahead < 0 || index < 0 || little < 0 || index == little)
			continue;
		// Its palm's way: across the hand (first finger to little) and
		// along it (wrist to knuckles) - which way round for each hand.
		const V3 along = at(ahead) - at(hand), across = at(index) - at(little);
		const V3 n = cross(along, across);
		if (length(n) < 1e-9f * std::max(1e-6f, dot(along, along)))
			continue;
		const V3 palm = normalized(n) * (side ? -1.0f : 1.0f);
		for (const auto &digit : DIGITS) {
			const bool thumb = std::strcmp(digit.name, "Thumb") == 0;
			// VRM 1.0's thumb: metacarpal, proximal, distal; 0.x's:
			// proximal, intermediate, distal.
			int joint[3];
			if (thumb && bone("Thumb", "Metacarpal") >= 0) {
				joint[0] = bone("Thumb", "Metacarpal");
				joint[1] = bone("Thumb", "Proximal");
				joint[2] = bone("Thumb", "Distal");
			} else {
				joint[0] = bone(digit.name, "Proximal");
				joint[1] = bone(digit.name, "Intermediate");
				joint[2] = bone(digit.name, "Distal");
			}
			if (joint[0] < 0)
				continue;
			const V3 way = joint[1] >= 0 ? at(joint[1]) - at(joint[0]) : at(joint[0]) - at(hand);
			const V3 a = cross(way, palm);
			if (length(a) < 1e-9f * std::max(1e-6f, dot(way, way)))
				continue;
			const V3 axis = normalized(a);
			// How far its file already bends a joint, about its axis.
			auto bend = [](V3 from, V3 to, V3 axis) {
				const V3 f = from - axis * dot(from, axis), t = to - axis * dot(to, axis);
				if (length(f) < 1e-9f || length(t) < 1e-9f)
					return 0.0f;
				return std::atan2(dot(cross(f, t), axis), dot(f, t));
			};
			float before = 0;
			for (int k = 0; k < 3 && joint[k] >= 0; k++) {
				const int j = joint[k];
				const V3 in = k == 0 ? at(j) - at(hand) : at(j) - at(joint[k - 1]);
				int next = k + 1 < 3 ? joint[k + 1] : -1;
				if (next < 0)
					for (int c : m.nodes[j].children)
						if (length(at(c) - at(j)) > 1e-7f) {
							next = c;
							break;
						}
				// Past the thumb's first joint: about the axis that takes it
				// (its way at rest) towards the first finger's knuckle,
				// whichever way round its bones are rolled.
				V3 joint_axis = axis;
				if (thumb && k > 0) {
					const V3 w = next >= 0 ? at(next) - at(j) : at(j) - at(joint[k - 1]);
					const V3 wn = normalized(w, normalized(way));
					V3 to = at(index) - at(j);
					to = to - wn * dot(to, wn);
					const V3 across_to = cross(wn, to);
					if (length(to) > 1e-4f * length(way) && length(across_to) > 1e-4f * length(way))
						joint_axis = normalized(across_to);
				}
				float has;
				if (thumb && k == 0)
					has = 0; // a thumb's first joint is splayed out from the hand
				else if (next >= 0)
					has = bend(in, at(next) - at(j), joint_axis);
				else
					has = before * 0.67f; // a tip with nothing after it: as a tip bends with the joint before it
				has = std::max(-0.35f, has);
				before = has;
				Finger f;
				f.node = j;
				f.axis = joint_axis;
				f.most = digit.bend[k] * PI_F / 180;
				f.has = has;
				f.side = side;
				f.digit = thumb ? 0 : (int)(&digit - DIGITS) + 1;
				f.joint = k;
				fingers_.push_back(f);
			}
		}
	}
}

void Rig::curl_fingers(const Drive &d)
{
	const float c = clamp01(options_.fingers);
	for (const Finger &f : fingers_) {
		// One its own animation turns stays as the animation has it.
		if (anim_turns_[f.node])
			continue;
		// Relaxed (or as curled as asked)...
		float angle = c > 0 ? std::max(0.0f, f.most * c - f.has) : 0.0f;
		// ...or as yours, as much as your hand is followed.
		if (d.follow_arms && d.arms.fingers) {
			const ArmTarget &t = d.arms.side[f.side];
			const float k = clamp01(t.weight) * clamp01(t.hand);
			if (k > 0)
				angle += (t.bend[f.digit][f.joint] - f.has - angle) * k;
		}
		angle *= 1 - moved_[f.node];
		if (std::fabs(angle) > 1e-4f)
			turn_in_rest(f.node, axis_angle(f.axis, angle));
	}
}

// The turn taking x to `a` and y to `b` (made square to `a`).
static Quat frame_turn(V3 a, V3 b)
{
	a = normalized(a, {1, 0, 0});
	b = b - a * dot(b, a);
	if (length(b) < 1e-6f)
		b = std::fabs(a.y) < 0.9f ? V3{0, 1, 0} - a * a.y : V3{1, 0, 0} - a * a.x;
	b = normalized(b);
	const V3 c = cross(a, b);
	M4 m;
	m.m[0] = a.x;
	m.m[1] = a.y;
	m.m[2] = a.z;
	m.m[4] = b.x;
	m.m[5] = b.y;
	m.m[6] = b.z;
	m.m[8] = c.x;
	m.m[9] = c.y;
	m.m[10] = c.z;
	return quat_of(m);
}

// `v` turned towards `to` a share `k` of the way (unit vectors).
static V3 toward(V3 v, V3 to, float k)
{
	return normalized(rotate(slerp(Quat{}, turn_between(v, to), clamp01(k)), v), to);
}

// A VRM's arms at rest, the facing model's way (for following yours), and
// its head's middle and width.
void Rig::find_arms()
{
	const Model &m = *model_;
	const Quat F = options_.facing;
	auto at = [&](int n) { return rotate(F, move_of(rest_world_[n])); };
	const V3 front{0, 0, 1};
	for (int side = 0; side < 2; side++) {
		ArmRest &a = arm_rest_[side];
		a = ArmRest();
		const int up = arm_bone_[side][0], lo = arm_bone_[side][1], hd = arm_bone_[side][2];
		if (up < 0 || lo < 0 || hd < 0 || !m.is_vrm())
			continue;
		const V3 u = at(lo) - at(up), f = at(hd) - at(lo);
		a.upper_len = length(u);
		a.fore_len = length(f);
		if (a.upper_len < 1e-6f || a.fore_len < 1e-6f)
			continue;
		a.shoulder = at(up);
		a.upper = normalized(u);
		a.fore = normalized(f);
		// Its elbow bends its forearm forwards (towards its front); an
		// arm made pointing forwards bends it up.
		V3 h = cross(a.upper, front);
		if (length(h) < 0.3f)
			h = cross(a.upper, V3{0, 1, 0});
		a.hinge = normalized(h);
		V3 hf = a.hinge - a.fore * dot(a.hinge, a.fore);
		a.fore_hinge = normalized(hf, a.hinge);
		a.ok = std::isfinite(a.hinge.x + a.hinge.y + a.hinge.z + a.fore_hinge.x + a.fore_hinge.y + a.fore_hinge.z);
		// Its hand's ways, from its finger bones.
		const std::string sd = side ? "right" : "left";
		int knuckle = m.bone(sd + "MiddleProximal");
		const int first = m.bone(sd + "IndexProximal");
		int little = m.bone(sd + "LittleProximal");
		if (little < 0)
			little = m.bone(sd + "RingProximal");
		if (knuckle < 0)
			knuckle = first;
		if (knuckle >= 0 && first >= 0 && little >= 0 && first != little) {
			const V3 along = at(knuckle) - at(hd), across = at(first) - at(little);
			if (length(along) > 1e-6f && length(across) > 1e-6f &&
			    length(cross(normalized(along), normalized(across))) > 0.3f) {
				a.along = normalized(along);
				a.across = normalized(across);
				a.hand = true;
			}
		}
	}
	head_known_ = false;
	if (head_box_.found) {
		const float w = head_box_.high.x - head_box_.low.x;
		if (w > 1e-6f) {
			head_mid_ = (head_box_.low + head_box_.high) * 0.5f;
			head_width_ = w * 0.85f;
			head_known_ = true;
		}
	}
}

// Your arms (arms.hpp) as a VRM's: each part pointing where yours does
// (by its head, reaching where your hand is by yours), its elbow bending
// as yours, its hand turned as yours with its forearm taking half the
// twist (a wrist twisted alone pinches thin, the "candy wrapper"), within
// how far a wrist bends. In its chest's terms: they ride on its body.
// A move on cue takes over from them as much as it shows.
void Rig::drive_arms(const Drive &d)
{
	if (!d.follow_arms)
		return;
	for (int side = 0; side < 2; side++) {
		const ArmRest &ar = arm_rest_[side];
		const ArmTarget &t = d.arms.side[side];
		const int up = arm_bone_[side][0], hd = arm_bone_[side][2];
		if (!ar.ok || !(t.weight > 0) || anim_turns_[up])
			continue;
		const float amount = clamp01(t.weight) * (1 - moved_[up]);
		if (!(amount > 0))
			continue;
		V3 du = normalized(t.upper, ar.upper), df = normalized(t.fore, ar.fore);
		// By its head: its wrist where yours is by yours (its head's size),
		// its elbow out the way yours is.
		const float near = clamp01(t.near);
		if (near > 0 && head_known_) {
			const V3 target = head_mid_ + t.by_head * head_width_;
			const V3 to = target - ar.shoulder;
			const float lu = ar.upper_len, lf = ar.fore_len;
			float dist = length(to);
			if (dist > 1e-6f) {
				dist = std::min((lu + lf) * 0.999f, std::max(std::fabs(lu - lf) + 1e-3f * (lu + lf), dist));
				const V3 dir = normalized(to);
				const float along = (lu * lu - lf * lf + dist * dist) / (2 * dist);
				const float out = std::sqrt(std::max(0.0f, lu * lu - along * along));
				V3 pole = du - dir * dot(du, dir);
				if (length(pole) < 1e-3f)
					pole = V3{0, -1, 0} - dir * dir.y;
				const V3 elbow = ar.shoulder + dir * along + normalized(pole, {0, -1, 0}) * out;
				du = toward(du, normalized(elbow - ar.shoulder, du), near);
				df = toward(df, normalized(ar.shoulder + dir * dist - elbow, df), near);
			}
		}
		// The elbow's hinge: square to both parts when it's bent; held
		// straight, as yours was (or, not known, its own turned with it).
		const V3 natural = rotate(turn_between(ar.upper, du), ar.hinge);
		const float sure = clamp01(t.hinge_sure);
		V3 pref = natural * (1 - sure) + normalized(t.hinge, natural) * sure;
		pref = normalized(pref - du * dot(pref, du), normalized(natural - du * dot(natural, du), {0, 0, 1}));
		const float bent = std::acos(std::max(-1.0f, std::min(1.0f, dot(du, df))));
		const float bend_sure = ramp(bent, 0.15f, 0.45f);
		V3 plane = normalized(cross(du, df), pref);
		V3 h = normalized(plane * bend_sure + pref * (1 - bend_sure), pref);
		const Quat R1 = frame_turn(du, h) * conj(frame_turn(ar.upper, ar.hinge));
		Quat R2 = frame_turn(df, h) * conj(frame_turn(ar.fore, ar.fore_hinge));
		Quat R3 = R2;
		const float hand = clamp01(t.hand);
		if (hd >= 0 && ar.hand && hand > 0) {
			// Its hand as yours, against its forearm: a twist about the
			// forearm and a bend (the wrist's, no more than WRIST_MOST).
			const Quat G = frame_turn(t.along, t.across) * conj(frame_turn(ar.along, ar.across));
			Quat L = normalized(conj(R2) * G);
			const V3 axis = ar.fore;
			const float p = L.x * axis.x + L.y * axis.y + L.z * axis.z;
			Quat twist = normalized(Quat{axis.x * p, axis.y * p, axis.z * p, L.w});
			if (twist.w < 0)
				twist = {-twist.x, -twist.y, -twist.z, -twist.w};
			Quat swing = normalized(L * conj(twist));
			if (swing.w < 0)
				swing = {-swing.x, -swing.y, -swing.z, -swing.w};
			// Within a wrist's bend.
			const float swing_angle = 2 * std::acos(std::min(1.0f, swing.w));
			constexpr float WRIST_MOST = 80 * RAD;
			if (swing_angle > WRIST_MOST)
				swing = slerp(Quat{}, swing, WRIST_MOST / swing_angle);
			// Its hand's turn, and half the twist in its forearm (as much
			// as the hand is yours).
			const Quat hand_world = R2 * swing * twist;
			R2 = R2 * slerp(Quat{}, twist, 0.5f * hand);
			R3 = slerp(R2, hand_world, hand);
		}
		pose_bone(up, R1, amount);
		pose_bone(arm_bone_[side][1], conj(R1) * R2, amount);
		if (hd >= 0)
			pose_bone(hd, conj(R2) * R3, amount);
	}
}

void Rig::settle()
{
	settled_ = true;
}

void Rig::measure_head()
{
	const Model &m = *model_;
	head_box_ = HeadBox();
	if (head_ < 0)
		return;
	// The head and all that hangs off it.
	std::vector<char> under(m.nodes.size(), 0);
	under[head_] = 2;
	for (bool more = true; more;) {
		more = false;
		for (size_t i = 0; i < m.nodes.size(); i++) {
			const int p = m.nodes[i].parent;
			if (!under[i] && p >= 0 && under[p]) {
				under[i] = 1;
				more = true;
			}
		}
	}
	const Quat F = options_.facing;
	bool any_box = false, any_top = false;
	auto take = [&](V3 p, bool box) {
		const V3 f = rotate(F, p);
		if (!any_top || f.y > head_box_.top)
			head_box_.top = f.y;
		any_top = true;
		if (!box)
			return;
		if (!any_box) {
			head_box_.low = head_box_.high = f;
		} else {
			head_box_.low = {std::min(head_box_.low.x, f.x), std::min(head_box_.low.y, f.y),
					 std::min(head_box_.low.z, f.z)};
			head_box_.high = {std::max(head_box_.high.x, f.x), std::max(head_box_.high.y, f.y),
					  std::max(head_box_.high.z, f.z)};
		}
		any_box = true;
	};
	for (size_t ni = 0; ni < m.nodes.size(); ni++) {
		const Node &node = m.nodes[ni];
		if (node.mesh < 0)
			continue;
		const Skin *skin = node.skin >= 0 ? &m.skins[node.skin] : nullptr;
		for (const Primitive &p : m.meshes[node.mesh].parts) {
			const size_t n = p.pos.size();
			// A big one: every few points is plenty.
			const size_t every = std::max<size_t>(1, n / 100000);
			if (skin && !p.joints.empty() && p.weights.size() == n * 4) {
				for (size_t i = 0; i < n; i += every) {
					int best = 0;
					for (int k = 1; k < 4; k++)
						if (p.weights[i * 4 + k] > p.weights[i * 4 + best])
							best = k;
					const uint16_t j = p.joints[i * 4 + best];
					if (j >= skin->joints.size())
						continue;
					const int bone = skin->joints[j];
					if (bone < 0 || bone >= (int)m.nodes.size() || !under[bone])
						continue;
					take(point(rest_world_[bone] * skin->inverse_bind[j], p.pos[i]), under[bone] == 2);
				}
			} else if (under[ni]) {
				for (size_t i = 0; i < n; i += every)
					take(point(rest_world_[ni], p.pos[i]), under[ni] == 2);
			}
		}
	}
	head_box_.found = any_box && head_box_.high.y - head_box_.low.y > 1e-6f;
	if (head_box_.found)
		head_box_.top = std::max(head_box_.top, head_box_.high.y);
}

std::vector<std::string> Rig::faces() const
{
	std::vector<std::string> out;
	auto add = [&out](const std::string &n) {
		if (!n.empty() && std::find(out.begin(), out.end(), n) == out.end())
			out.push_back(n);
	};
	std::set<int> arkit;
	for (const Driver &d : drivers_)
		if (d.move > 0)
			arkit.insert(d.move);
	for (const char *e : {"happy", "angry", "sad", "relaxed", "surprised"}) {
		bool has = false;
		for (const Driver &d : drivers_)
			has = has || (d.move < 0 && d.expression == e);
		// Made for ARKit: from its shapes, if it has most of them.
		for (const Recipe &r : recipes())
			if (!has && r.face == std::string(e)) {
				size_t got = 0;
				for (const auto &part : r.parts)
					got += arkit.count(arkit_move(part.first));
				has = got * 2 > r.parts.size();
			}
		if (has)
			add(e);
	}
	for (const Driver &d : drivers_)
		if (d.move < 0 && !is_vrm_preset(d.expression))
			add(d.expression);
	if (morph_by_name_.count("tongueout"))
		add("tongueOut");
	return out;
}

std::vector<std::string> Rig::shapes() const
{
	std::vector<std::string> out;
	std::set<std::string> seen;
	for (const Mesh &mesh : model_->meshes)
		for (const std::string &n : mesh.morph_names)
			if (!n.empty() && out.size() < 400 && seen.insert(n).second)
				out.push_back(n);
	return out;
}

// A bone turned `world_turn` from how it rests (the facing model's way,
// in the world: VRM's "normalized" turn), `amount` of the way from how it
// is now.
void Rig::pose_bone(int node, Quat world_turn, float amount)
{
	if (node < 0 || node >= (int)r_.size() || !(amount > 0))
		return;
	const Quat F = options_.facing;
	const Quat N = conj(F) * normalized(world_turn) * F;
	const int p = model_->nodes[node].parent;
	const Quat P = p >= 0 ? rot_of(rest_world_[p]) : Quat{};
	const Quat W = rot_of(rest_world_[node]);
	r_[node] = normalized(slerp(r_[node], normalized(conj(P) * N * W), clamp01(amount)));
	moved_[node] = 1 - (1 - moved_[node]) * (1 - clamp01(amount));
}

// A VRM animation file's moves at `t` seconds: each bone's turn carried
// over (with those of the bones above it that this character hasn't, an
// upper chest say, so it still bends as far), its hips moved as far for
// its height, and its expressions.
void Rig::play_motion(const Motion &mo, float t, float amount)
{
	std::map<std::string, Quat> turn;
	bool moved_hips = false;
	V3 hips_at;
	float v[4];
	for (const MotionTrack &tr : mo.tracks) {
		sample(tr.channel, t, v);
		if (tr.channel.path == AnimChannel::Turn) {
			const Quat q{v[0], v[1], v[2], v[3]};
			if (std::isfinite(q.x + q.y + q.z + q.w))
				turn[tr.bone] = normalized(q);
		} else if (tr.bone == "hips" && tr.channel.path == AnimChannel::Move) {
			hips_at = {v[0], v[1], v[2]};
			moved_hips = std::isfinite(hips_at.x + hips_at.y + hips_at.z);
		}
	}
	for (const auto &b : bones1_) {
		Quat N;
		bool any = false;
		auto it = turn.find(b.first);
		if (it != turn.end()) {
			N = it->second;
			any = true;
		}
		std::string up = b.first;
		for (int steps = 0; steps < 16; steps++) {
			const char *parent = humanoid_parent(up);
			if (!parent || !*parent)
				break;
			up = parent;
			if (bones1_.count(up))
				break;
			auto jt = turn.find(up);
			if (jt != turn.end()) {
				N = jt->second * N;
				any = true;
			}
		}
		if (any)
			pose_bone(b.second, N, amount);
	}
	auto hips = bones1_.find("hips");
	if (moved_hips && hips != bones1_.end() && mo.hips_rest.y > 1e-3f) {
		const Quat F = options_.facing;
		const int h = hips->second;
		const float height = rotate(F, move_of(rest_world_[h])).y - floor_;
		if (height > 1e-4f) {
			const V3 by = rotate(conj(F), (hips_at - mo.hips_rest) * (height / mo.hips_rest.y));
			const int p = model_->nodes[h].parent;
			const V3 local = p >= 0 ? direction(inverse(rest_world_[p]), by) : by;
			t_[h] = t_[h] + local * clamp01(amount);
		}
	}
	for (const auto &e : mo.expressions) {
		sample(e.second, t, v);
		if (!std::isfinite(v[0]))
			continue;
		float &f = move_faces_[e.first];
		f = std::max(f, clamp01(v[0]) * clamp01(amount));
	}
}

// One of its own animations, once, at `t` seconds.
void Rig::play_own(int animation, float t, float amount)
{
	const Animation &a = model_->animations[animation];
	t = std::min(std::max(0.0f, t), a.length);
	const float k = clamp01(amount);
	float v[4];
	for (const AnimChannel &c : a.channels) {
		if (c.node < 0 || c.node >= (int)r_.size())
			continue;
		sample(c, t, v);
		if (c.path == AnimChannel::Move) {
			t_[c.node] = t_[c.node] + (V3{v[0], v[1], v[2]} - t_[c.node]) * k;
		} else if (c.path == AnimChannel::Size) {
			s_[c.node] = s_[c.node] + (V3{v[0], v[1], v[2]} - s_[c.node]) * k;
		} else {
			r_[c.node] = normalized(slerp(r_[c.node], normalized(Quat{v[0], v[1], v[2], v[3]}), k));
			moved_[c.node] = 1 - (1 - moved_[c.node]) * (1 - k);
		}
	}
}

// A VRM's own wave (its right hand) and cheer (both arms up, pumping), as
// VRM's turns from how it rests (the facing model's way: its left arm out
// along +x, its right along -x, facing +z).
void Rig::play_builtin(BuiltIn b, float t, float amount)
{
	const float D = PI_F / 180;
	auto Rz = [D](float deg) { return axis_angle({0, 0, 1}, deg * D); };
	auto Ry = [D](float deg) { return axis_angle({0, 1, 0}, deg * D); };
	if (b == BuiltIn::Wave) {
		const int *arm = arm_bone_[1];
		if (arm[0] < 0)
			return;
		const float w = std::sin(2 * PI_F * 1.6f * t);
		// The upper arm out and up a little, forward; the forearm up,
		// waving side to side; the hand flicking with it, open.
		pose_bone(arm[0], Ry(25) * Rz(-22), amount);
		pose_bone(arm[1], Rz(-(68 + 20 * w)), amount);
		if (arm[2] >= 0)
			pose_bone(arm[2], Rz(-8 * std::sin(2 * PI_F * 1.6f * t - 0.6f)), amount);
		for (const auto &bn : bones1_)
			if (bn.first.rfind("right", 0) == 0 &&
			    (bn.first.find("Proximal") != std::string::npos ||
			     bn.first.find("Intermediate") != std::string::npos ||
			     bn.first.find("Distal") != std::string::npos ||
			     bn.first.find("Metacarpal") != std::string::npos))
				pose_bone(bn.second, Quat{}, amount);
	} else if (b == BuiltIn::Cheer) {
		const float pump = std::pow(std::sin(2 * PI_F * 1.8f * t), 2.0f);
		for (int side = 0; side < 2; side++) {
			const int *arm = arm_bone_[side];
			if (arm[0] < 0)
				continue;
			const float s = side ? -1.0f : 1.0f; // its left out along +x
			pose_bone(arm[0], Ry(-s * 20) * Rz(s * (58 + 14 * pump)), amount);
			pose_bone(arm[1], Rz(s * (40 - 25 * pump)), amount);
		}
	}
}

void Rig::play_moves(const Drive &d)
{
	std::fill(moved_.begin(), moved_.end(), 0.0f);
	move_faces_.clear();
	for (const MoveNow &mv : d.body) {
		const float a = clamp01(mv.amount);
		if (!(a > 0) || !std::isfinite(mv.time))
			continue;
		if (mv.motion) {
			if (plays_motions())
				play_motion(*mv.motion, mv.time, a);
		} else if (mv.animation >= 0) {
			if (mv.animation < (int)model_->animations.size())
				play_own(mv.animation, mv.time, a);
		} else if (builtin_arms(mv.builtin)) {
			play_builtin(mv.builtin, mv.time, a);
		}
	}
}

void Rig::set_animation(int which)
{
	if (which < 0 || which >= (int)model_->animations.size())
		which = -1;
	if (which != options_.animation)
		anim_time_ = 0;
	options_.animation = which;
}

// Its own animation, on from where it was by `dt`, over and over: the
// nodes it moves are where it has them (the rest as they are).
void Rig::animate(float dt)
{
	const Model &m = *model_;
	anim_turns_.assign(m.nodes.size(), 0);
	if (options_.animation < 0 || options_.animation >= (int)m.animations.size())
		return;
	const Animation &a = m.animations[options_.animation];
	if (a.length > 0)
		anim_time_ = std::fmod(anim_time_ + std::max(0.0f, std::min(dt, 1.0f)), a.length);
	float v[4];
	for (const AnimChannel &c : a.channels) {
		sample(c, anim_time_, v);
		if (c.path == AnimChannel::Move)
			t_[c.node] = {v[0], v[1], v[2]};
		else if (c.path == AnimChannel::Size)
			s_[c.node] = {v[0], v[1], v[2]};
		else
			r_[c.node] = Quat{v[0], v[1], v[2], v[3]}, anim_turns_[c.node] = 1;
	}
}

// A node's place in the world from its parent's (or the root), and all
// under it.
void Rig::world_from(int start)
{
	const Model &m = *model_;
	std::vector<int> stack{start};
	while (!stack.empty()) {
		const int n = stack.back();
		stack.pop_back();
		const int p = m.nodes[n].parent;
		// A part standing upright keeps the place it was given.
		if (!held_[n])
			world_[n] = (p >= 0 ? world_[p] : root_) * matrix_of(t_[n], r_[n], s_[n]);
		for (int c : m.nodes[n].children)
			stack.push_back(c);
	}
}

void Rig::drive_head(const Drive &d)
{
	const Quat F = options_.facing;
	// A turn as the facing model has it (x its right... as the picture
	// shows it, y up, z out of its face) in the model's own terms.
	auto own = [&F](Quat q) {
		return conj(F) * q * F;
	};
	const Quat R = own(d.head);
	// A breath: its chest leans back a little (below) and its shoulders
	// rise (each turned up 3 degrees) - its arms still hanging as they
	// did, not swung out or forward: they're turned back by as much. Not
	// a side its animation moves.
	const float breath = clamp01(d.breath);
	const Quat lean = own(axis_angle({1, 0, 0}, -2.0f * RAD * breath));
	const int chest_top = head_ >= 0 && !torso_.empty() ? torso_.back() : -1;
	for (int side = 0; side < 2 && breath > 0; side++) {
		const int sh = shoulder_[side], arm = upper_arm_[side];
		if (arm < 0 || anim_turns_[arm])
			continue;
		// (Less of it as a move holds the arm.)
		const float k = 1 - moved_[arm];
		if (sh >= 0 && !anim_turns_[sh] && model_->nodes[arm].parent == sh) {
			const Quat shrug = own(axis_angle({0, 0, 1}, (side ? -3.0f : 3.0f) * RAD * breath * k));
			turn_in_rest(sh, shrug);
			turn_in_rest(arm, conj(shrug));
		}
		bool under_chest = false;
		for (int n = model_->nodes[arm].parent; n >= 0 && !under_chest; n = model_->nodes[n].parent)
			under_chest = n == chest_top;
		if (under_chest)
			turn_in_rest(arm, slerp(Quat{}, conj(lean), k));
	}
	// (Arms its animation turns are where it has them, down or not; a move
	// holding them, as much as it does.)
	for (const Arm &a : arms_)
		if (!anim_turns_[a.node])
			turn_in_rest(a.node, slerp(Quat{}, a.turn, 1 - moved_[a.node]));
	if (head_ < 0)
		return;
	// Its body: some of your head's turn, its own slow sway, and a breath
	// (its chest leaning back a little and rising). The neck and head
	// take the rest, so the head still turns just as yours.
	Quat torso;
	if (!torso_.empty()) {
		const Quat follow = slerp(Quat{}, R, clamp01(d.body_follow) * 0.4f);
		const Quat sway =
			own(axis_angle({0, 1, 0}, d.body_yaw * RAD) * axis_angle({0, 0, 1}, d.body_roll * RAD));
		torso = follow * sway;
		// The lowest bone takes 40% of the turn, the rest the others.
		const Quat low = slerp(Quat{}, torso, torso_.size() > 1 ? 0.4f : 1.0f);
		turn_in_rest(torso_[0], low);
		if (torso_.size() > 1) {
			const Quat upper = conj(low) * torso;
			const size_t n = torso_.size() - 1;
			Quat done;
			for (size_t i = 1; i <= n; i++) {
				const Quat part = i == n ? conj(done) * upper : slerp(Quat{}, upper, 1.0f / (float)n);
				turn_in_rest(torso_[i], part);
				done = done * part;
			}
		}
		// The breath on the top of the chest (not part of the turn the
		// head makes up for: its head rides a breath, as yours does).
		turn_in_rest(torso_.back(), lean);
		// ...and rising a little (up to 0.4% of its height: 7 mm on a
		// person), in its parent's own terms.
		const int top = torso_.back(), under = model_->nodes[top].parent;
		if (under >= 0)
			t_[top] = t_[top] + direction(inverse(rest_world_[under]), {0, 0.004f * height_ * breath, 0});
	}
	const Quat rest_of = conj(torso) * R;
	if (neck_ >= 0 && neck_ != head_) {
		const Quat rn = slerp(Quat{}, rest_of, options_.neck_share);
		turn_in_rest(neck_, rn);
		turn_in_rest(head_, conj(rn) * rest_of);
	} else {
		turn_in_rest(head_, rest_of);
	}
}

void Rig::drive_eyes(const Drive &d)
{
	const Model &m = *model_;
	if (!m.look_bones || (left_eye_ < 0 && right_eye_ < 0))
		return;
	static const int up_l = move_id("eyeLookUpLeft"), up_r = move_id("eyeLookUpRight"),
			 down_l = move_id("eyeLookDownLeft"), down_r = move_id("eyeLookDownRight"),
			 in_l = move_id("eyeLookInLeft"), in_r = move_id("eyeLookInRight"),
			 out_l = move_id("eyeLookOutLeft"), out_r = move_id("eyeLookOutRight");
	const float *mv = d.moves;
	// Your look (degrees): + towards its left (+x), + up.
	const float side = ((mv[out_l] - mv[in_l]) + (mv[in_r] - mv[out_r])) / 2 * EYE_SIDEWAYS;
	const float up = ((mv[up_l] - mv[down_l]) + (mv[up_r] - mv[down_r])) / 2 * EYE_UPDOWN;
	auto mapped = [](float deg, const LookRange &r) {
		const float a = std::min(std::fabs(deg), r.in) / r.in * r.out;
		return deg < 0 ? -a : a;
	};
	const Quat F = options_.facing;
	for (int which = 0; which < 2; which++) {
		const int eye = which == 0 ? left_eye_ : right_eye_;
		if (eye < 0 || !d.look)
			continue;
		// Its left eye looks out towards +x, its right eye in.
		const bool outwards = which == 0 ? side > 0 : side < 0;
		const float yaw = mapped(side, outwards ? m.look_out : m.look_in);
		const float pitch = mapped(up, up > 0 ? m.look_up : m.look_down);
		const Quat g = axis_angle({0, 1, 0}, yaw * RAD) * axis_angle({1, 0, 0}, -pitch * RAD);
		const Quat gm = conj(F) * g * F;
		const Quat w = rot_of(rest_world_[eye]);
		r_[eye] = r_[eye] * (conj(w) * gm * w);
	}
}

void Rig::drive_expressions(const Drive &d)
{
	const Model &m = *model_;
	for (size_t mi = 0; mi < m.meshes.size(); mi++)
		weights_[mi] = m.meshes[mi].weights;
	// A shape your face moves starts from nothing, whatever strength the
	// file was saved with (a file saved with its blink on would otherwise
	// have its eyes shut for good) - as VRM apps do.
	for (const Driver &dr : drivers_)
		if (dr.mesh >= 0 && dr.mesh < (int)weights_.size() && dr.morph >= 0 &&
		    dr.morph < (int)weights_[dr.mesh].size())
			weights_[dr.mesh][dr.morph] = 0;
	values_.clear();
	if (!d.expressions)
		return;
	bool per_eye = false;
	for (const Driver &dr : drivers_)
		if (dr.expression == "blinkLeft" || dr.expression == "blinkRight")
			per_eye = true;
	std::map<std::string, float> v = vrm_values(d.moves, per_eye);
	// The mic's vowels over the camera's mouth, as much as it has it;
	// ARKit-named shapes from your moves with the mic's mouth in them.
	float mic_moves[EXPRESSIONS];
	std::copy(d.moves, d.moves + EXPRESSIONS, mic_moves);
	const float mic_share = std::min(1.0f, std::max(0.0f, d.mic.share));
	if (mic_share > 0) {
		for (int k = 0; k < VOWELS; k++) {
			float &value = v[VOWEL_VRM[k]];
			value = value + (std::min(1.0f, std::max(0.0f, d.mic.vowel[k])) - value) * mic_share;
		}
		mic_into_moves(d.mic, mic_moves);
	}
	if (!d.look || (m.look_bones && (left_eye_ >= 0 || right_eye_ >= 0)))
		for (const char *k : {"lookUp", "lookDown", "lookLeft", "lookRight"})
			v[k] = 0;
	// On cue: faces (and a VRM animation's own), a whole-face one taking
	// over from yours as much as it's on - your blinks, looks and mouth
	// stay yours - and its eyes screwed shut.
	std::map<std::string, float> cue = move_faces_;
	for (const FaceNow &f : d.faces)
		if (!f.name.empty() && std::isfinite(f.amount)) {
			float &c = cue[f.name];
			c = std::max(c, clamp01(f.amount));
		}
	float taken = 0;
	for (const auto &c : cue)
		if (is_emotion(c.first))
			taken = std::max(taken, c.second);
	if (taken > 0)
		for (auto &kv : v)
			if (is_emotion(kv.first))
				kv.second *= 1 - taken;
	for (const auto &c : cue) {
		float &x = v[c.first];
		x = std::max(x, c.second);
	}
	const float squint = std::isfinite(d.squint) ? clamp01(d.squint) : 0.0f;
	if (squint > 0) {
		bool both = false;
		for (const Driver &dr : drivers_)
			both = both || (dr.move < 0 && dr.expression == "blink");
		for (const char *k : {"blink", "blinkLeft", "blinkRight"})
			if (both == (std::strcmp(k, "blink") == 0)) {
				float &x = v[k];
				x = std::max(x, squint);
			}
	}
	// Made for ARKit: the faces from its shapes, over yours.
	if (taken > 0 || squint > 0 || !cue.empty()) {
		if (taken > 0)
			for (int i = 1; i < EXPRESSIONS; i++)
				if (face_move(EXPRESSION_NAMES[i]))
					mic_moves[i] *= 1 - taken;
		for (const Recipe &r : recipes()) {
			auto it = cue.find(r.face);
			if (it == cue.end())
				continue;
			for (const auto &part : r.parts) {
				const int k = arkit_move(part.first);
				if (k > 0)
					mic_moves[k] = std::max(mic_moves[k], part.second * it->second);
			}
		}
		static const int blink_l = move_id("eyeBlinkLeft"), blink_r = move_id("eyeBlinkRight");
		mic_moves[blink_l] = std::max(mic_moves[blink_l], squint);
		mic_moves[blink_r] = std::max(mic_moves[blink_r], squint);
	}
	// The file's own expressions (not VRM's names) aren't driven.
	std::map<std::string, float> on;
	for (const Driver &dr : drivers_)
		if (dr.move < 0 && v.count(dr.expression))
			on[dr.expression] = v[dr.expression];
	for (const Expression &e : m.expressions)
		if (e.binary && on.count(e.name))
			on[e.name] = on[e.name] > 0.5f ? 1.0f : 0.0f;
	// An expression that holds back blinks, looks or the mouth.
	float blink_k = 1, look_k = 1, mouth_k = 1;
	for (const Expression &e : m.expressions) {
		auto it = on.find(e.name);
		if (it == on.end() || it->second <= 0.001f)
			continue;
		auto hold = [&](int how, float &k) {
			if (how == 1)
				k = 0;
			else if (how == 2)
				k *= 1 - it->second;
		};
		hold(e.override_blink, blink_k);
		hold(e.override_look, look_k);
		hold(e.override_mouth, mouth_k);
	}
	for (auto &kv : on) {
		if (is_blink(kv.first))
			kv.second *= blink_k;
		else if (is_look(kv.first))
			kv.second *= look_k;
		else if (is_mouth(kv.first))
			kv.second *= mouth_k;
	}
	values_ = on;
	for (const Driver &dr : drivers_) {
		if (dr.mesh < 0 || dr.mesh >= (int)weights_.size() || dr.morph < 0 ||
		    dr.morph >= (int)weights_[dr.mesh].size())
			continue;
		float amount;
		if (dr.move > 0) {
			amount = clamp01(mic_moves[dr.move]);
		} else {
			auto it = on.find(dr.expression);
			amount = it == on.end() ? 0 : it->second;
		}
		if (still_mesh_[dr.mesh])
			continue; // a face your face doesn't move: as it rests
		float &w = weights_[dr.mesh][dr.morph];
		w = clamp01(w + amount * dr.weight);
	}
	// A face on cue by a shape's own name.
	for (const auto &c : cue) {
		auto it = morph_by_name_.find(lower(c.first));
		if (it == morph_by_name_.end())
			continue;
		for (const auto &mk : it->second)
			if (!still_mesh_[mk.first] && mk.second < (int)weights_[mk.first].size()) {
				float &w = weights_[mk.first][mk.second];
				w = std::max(w, c.second);
			}
	}
}

void Rig::step_springs(float dt, float scale)
{
	const Model &m = *model_;
	const float floppy = clamp01(options_.floppy);
	// 0.5: as the file says; stiffer or floppier from there.
	const float stiff_k = floppy <= 0.5f ? 1 + (0.5f - floppy) * 6 : (1 - floppy) * 2;
	const float auto_k = auto_chain_ ? height_ : 1;
	// Dangly parts found by us: the character as made is how they hang
	// upright, so gravity pulls only as far as it's tipped from that.
	const V3 upright_down = auto_chain_ ? normalized(direction(root_, {0, -1, 0}), {0, -1, 0}) : V3{0, 0, 0};
	const Quat head_turn = head_turn_now();
	for (Chain &c : springs_) {
		for (Joint &j : c.joints) {
			const int n = j.s.node;
			const int p = m.nodes[n].parent;
			const M4 &pw = p >= 0 ? world_[p] : root_;
			const V3 head = point(pw, t_[n]);
			const Quat parent_rot = rot_of(pw);
			const float len = length(direction(world_[n], j.tail));
			if (len < 1e-7f)
				continue;
			const V3 rest_dir = rotate(parent_rot * j.rest, j.axis);
			const float k = scale * auto_k;
			V3 next;
			if (j.pulled) {
				// Swung and pulled, never back to how it was made: it hangs
				// the way it's pulled (with the head, or down).
				const V3 pull = j.with_head ? rotate(head_turn, j.pull) : j.pull;
				next = j.now + (j.now - j.before) * (1 - j.s.drag) + pull * (dt * dt * scale);
			} else {
				next = j.now + (j.now - j.before) * (1 - j.s.drag) +
				       rest_dir * (j.s.stiffness * stiff_k * dt * k) +
				       (j.s.gravity_dir - upright_down) * (j.s.gravity * dt * k);
			}
			next = head + normalized(next - head, rest_dir) * len;
			for (int ci : c.colliders) {
				if (ci < 0 || ci >= (int)m.colliders.size())
					continue;
				const Collider &col = m.colliders[ci];
				const M4 &cw = col.node >= 0 ? world_[col.node] : root_;
				const float cs = scale_of(cw);
				const V3 a = point(cw, col.offset);
				const V3 centre = col.capsule ? nearest_on(a, point(cw, col.tail), next) : a;
				const float r = col.radius * cs + j.s.radius * scale;
				const V3 off = next - centre;
				if (dot(off, off) < r * r) {
					next = centre + normalized(off, rest_dir) * r;
					next = head + normalized(next - head, rest_dir) * len;
				}
			}
			j.before = j.now;
			j.now = next;
			// Its turn: at rest, then round to where its tail swung.
			const V3 to = rotate(conj(parent_rot * j.rest), next - head);
			r_[n] = normalized(j.rest * turn_between(j.axis, to));
			world_from(n);
		}
	}
}

Quat Rig::head_turn_now() const
{
	// The facing model's directions (x right, y up, z out of its face)
	// as its head has them now.
	const Quat F = options_.facing;
	if (head_ >= 0)
		return rot_of(world_[head_]) * conj(rot_of(rest_world_[head_])) * conj(F);
	return rot_of(root_) * conj(F);
}

M4 Rig::head_frame() const
{
	if (head_ >= 0)
		return world_[head_] * inverse(rest_world_[head_]);
	return root_;
}

void Rig::stand_upright(const Drive &d)
{
	if (upright_.empty())
		return;
	const Model &m = *model_;
	const M4 &up = d.has_upright ? d.upright : d.place;
	for (Upright &u : upright_) {
		// It trails after where it would be, turned with your head...
		const V3 target = move_of(world_[u.node]);
		if (settled_ || u.follow <= 0)
			u.at = target;
		else
			u.at = u.at + (target - u.at) * (1 - std::exp(-u.follow * std::max(0.0f, d.dt)));
		// ...standing as it was made.
		M4 w = up * rest_world_[u.node];
		w.m[12] = u.at.x;
		w.m[13] = u.at.y;
		w.m[14] = u.at.z;
		world_[u.node] = w;
		held_[u.node] = 1;
		for (int c : m.nodes[u.node].children)
			world_from(c);
	}
}

void Rig::pose(const Drive &d)
{
	const Model &m = *model_;
	for (size_t i = 0; i < m.nodes.size(); i++) {
		t_[i] = m.nodes[i].t;
		r_[i] = m.nodes[i].r;
		s_[i] = m.nodes[i].s;
	}
	animate(d.dt);
	play_moves(d);
	// Swinging parts hang as its animation has them (it may swing a tail
	// itself), and swing from there.
	for (Chain &c : springs_)
		for (Joint &j : c.joints)
			j.rest = r_[j.s.node];
	std::fill(held_.begin(), held_.end(), 0);
	// No head bone: it all turns round its head.
	if (head_ < 0) {
		const Quat F = options_.facing;
		const Quat R = conj(F) * d.head * F;
		root_ = d.place * moved(pivot_) * matrix_of(R) * moved(pivot_ * -1);
		// A breath: it grows a little, up more than across, from its
		// bottom (up to 1.2% taller).
		const float b = d.breathe_whole ? clamp01(d.breath) : 0;
		if (b > 0) {
			const V3 base{pivot_.x, m.low.y, pivot_.z};
			root_ = root_ * moved(base) * matrix_of({}, Quat{}, {1 + 0.006f * b, 1 + 0.012f * b, 1 + 0.006f * b}) *
				moved(base * -1);
		}
	} else {
		root_ = d.place;
	}
	drive_head(d);
	drive_arms(d);
	curl_fingers(d);
	drive_eyes(d);
	drive_expressions(d);
	for (int r : m.roots)
		world_from(r);
	// Nodes outside the scene stay put, at rest.
	for (size_t i = 0; i < m.nodes.size(); i++)
		if (m.nodes[i].parent < 0 && std::find(m.roots.begin(), m.roots.end(), (int)i) == m.roots.end())
			world_from((int)i);

	const float scale = scale_of(d.place);
	const V3 root_at = move_of(root_);
	// A jump (a new place, or a long gap): swinging parts start at rest.
	if (!settled_ && (d.dt > 0.25f || length(root_at - last_root_) > height_ * scale * 0.75f))
		settled_ = true;
	last_root_ = root_at;
	stand_upright(d);
	hide_face_ = d.hide_own_face;
	if (settled_) {
		for (Chain &c : springs_)
			for (Joint &j : c.joints) {
				const int p = m.nodes[j.s.node].parent;
				const M4 &pw = p >= 0 ? world_[p] : root_;
				const M4 w = pw * matrix_of(t_[j.s.node], j.rest, s_[j.s.node]);
				j.now = j.before = point(w, j.tail);
			}
		spring_time_ = 0;
		settled_ = false;
	}
	// Thrown about by a hit: each swinging part given that speed.
	const V3 knock = d.knock;
	if (std::isfinite(knock.x + knock.y + knock.z) && dot(knock, knock) > 0)
		for (Chain &c : springs_)
			for (Joint &j : c.joints)
				j.before = j.before - knock * SPRING_STEP;
	if (!springs_.empty()) {
		spring_time_ += std::max(0.0f, d.dt);
		int steps = 0;
		// Between steps the joints keep last step's turn.
		while (spring_time_ >= SPRING_STEP && steps < MOST_STEPS) {
			step_springs(SPRING_STEP, scale);
			spring_time_ -= SPRING_STEP;
			steps++;
		}
		if (steps == MOST_STEPS)
			spring_time_ = 0;
		if (!steps)
			for (Chain &c : springs_)
				for (Joint &j : c.joints) {
					const int n = j.s.node;
					const int p = m.nodes[n].parent;
					const M4 &pw = p >= 0 ? world_[p] : root_;
					const V3 head = point(pw, t_[n]);
					const V3 to = rotate(conj(rot_of(pw) * j.rest), j.now - head);
					r_[n] = normalized(j.rest * turn_between(j.axis, to));
					world_from(n);
				}
	}
	skin();
}

void Rig::skin()
{
	const Model &m = *model_;
	size_t count = 0;
	for (size_t ni = 0; ni < m.nodes.size(); ni++)
		if (m.nodes[ni].mesh >= 0)
			count += m.meshes[m.nodes[ni].mesh].parts.size();
	parts_.resize(count);
	size_t at = 0;
	std::vector<V3> mp, mn;
	std::vector<M4> joint;
	for (size_t ni = 0; ni < m.nodes.size(); ni++) {
		const Node &node = m.nodes[ni];
		if (node.mesh < 0)
			continue;
		const Mesh &mesh = m.meshes[node.mesh];
		const Skin *skin = node.skin >= 0 ? &m.skins[node.skin] : nullptr;
		if (skin) {
			joint.resize(skin->joints.size());
			for (size_t j = 0; j < skin->joints.size(); j++)
				joint[j] = world_[skin->joints[j]] * skin->inverse_bind[j];
		}
		const std::vector<float> &w = weights_[node.mesh];
		for (size_t pi = 0; pi < mesh.parts.size(); pi++) {
			const Primitive &p = mesh.parts[pi];
			Part &out = parts_[at++];
			out.node = (int)ni;
			out.part = (int)pi;
			out.prim = &p;
			const size_t n = p.pos.size();
			const V3 *src_p = p.pos.data(), *src_n = p.normal.data();
			bool morphed = false;
			for (size_t k = 0; k < p.morphs.size() && k < w.size(); k++) {
				if (std::fabs(w[k]) < 1e-4f)
					continue;
				if (!morphed) {
					mp.assign(p.pos.begin(), p.pos.end());
					mn.assign(p.normal.begin(), p.normal.end());
					morphed = true;
				}
				const Sparse &sp = sparse_[node.mesh][pi][k];
				for (size_t i = 0; i < sp.at.size(); i++) {
					mp[sp.at[i]] = mp[sp.at[i]] + sp.pos[i] * w[k];
					mn[sp.at[i]] = mn[sp.at[i]] + sp.normal[i] * w[k];
				}
			}
			if (morphed) {
				src_p = mp.data();
				src_n = mn.data();
			}
			out.pos.resize(n);
			out.normal.resize(n);
			// Its tangents turned as its facings are (the drawing squares
			// them up with the facing).
			const bool tangents = p.tangent.size() == n * 4;
			out.tangent.resize(tangents ? n : 0);
			auto handed = [](const M4 &a) {
				const V3 x{a.m[0], a.m[1], a.m[2]}, y{a.m[4], a.m[5], a.m[6]}, z{a.m[8], a.m[9], a.m[10]};
				return dot(x, cross(y, z)) < 0 ? -1.0f : 1.0f;
			};
			out.handed = skin && !p.joints.empty() ? (p.joints[0] < joint.size() ? handed(joint[p.joints[0]]) : 1.0f)
							       : handed(world_[ni]);
			auto tangent_of = [&p](size_t i) {
				return V3{p.tangent[i * 4], p.tangent[i * 4 + 1], p.tangent[i * 4 + 2]};
			};
			if (skin && !p.joints.empty()) {
				for (size_t i = 0; i < n; i++) {
					V3 v, nn, tt;
					for (int k = 0; k < 4; k++) {
						const float wk = p.weights[i * 4 + k];
						const uint16_t j = p.joints[i * 4 + k];
						if (wk <= 0 || j >= joint.size())
							continue;
						v = v + point(joint[j], src_p[i]) * wk;
						nn = nn + direction(joint[j], src_n[i]) * wk;
						if (tangents)
							tt = tt + direction(joint[j], tangent_of(i)) * wk;
					}
					out.pos[i] = v;
					out.normal[i] = normalized(nn, {0, 0, 1});
					if (tangents)
						out.tangent[i] = normalized(tt, {1, 0, 0});
				}
			} else {
				const M4 &mw = world_[ni];
				for (size_t i = 0; i < n; i++) {
					out.pos[i] = point(mw, src_p[i]);
					out.normal[i] = normalized(direction(mw, src_n[i]), {0, 0, 1});
					if (tangents)
						out.tangent[i] = normalized(direction(mw, tangent_of(i)), {1, 0, 0});
				}
			}
			// Not drawn: every point in one, so its triangles cover nothing.
			if (hide_face_ && own_face_[ni] && n)
				std::fill(out.pos.begin() + 1, out.pos.end(), out.pos[0]);
		}
	}
}

namespace {

// A model's size turned to face +z: its middle and how big each way.
void facing_bounds(const Model &m, Quat facing, V3 &middle, V3 &size)
{
	V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
	for (int i = 0; i < 8; i++) {
		const V3 c{i & 1 ? m.high.x : m.low.x, i & 2 ? m.high.y : m.low.y, i & 4 ? m.high.z : m.low.z};
		const V3 r = rotate(facing, c);
		lo = {std::min(lo.x, r.x), std::min(lo.y, r.y), std::min(lo.z, r.z)};
		hi = {std::max(hi.x, r.x), std::max(hi.y, r.y), std::max(hi.z, r.z)};
	}
	middle = (lo + hi) * 0.5f;
	size = {std::max(1e-4f, hi.x - lo.x), std::max(1e-4f, hi.y - lo.y), std::max(1e-4f, hi.z - lo.z)};
}

// The head bone's place at rest, turned to face +z, or false.
bool head_bone(const Model &m, Quat facing, V3 &at)
{
	const int head = m.bone("head");
	if (head < 0)
		return false;
	at = rotate(facing, move_of(rest_world(m)[head]));
	return true;
}

// Halfway between its eyes (their bones) at rest, turned to face +z, or false.
bool eyes_middle(const Model &m, Quat facing, V3 &at)
{
	const int l = m.bone("leftEye"), r = m.bone("rightEye");
	if (l < 0 || r < 0)
		return false;
	const std::vector<M4> rest = rest_world(m);
	at = rotate(facing, (move_of(rest[l]) + move_of(rest[r])) * 0.5f);
	return true;
}

M4 worn_matrix(const Model &m, Quat facing, const WornPlace &place, float &units)
{
	V3 at = place.at;
	units = place.units;
	if (place.automatic) {
		V3 middle, size;
		facing_bounds(m, facing, middle, size);
		V3 hb, eyes;
		if (m.is_vrm() && eyes_middle(m, facing, eyes)) {
			// At its own size, its eyes where yours are (between their
			// middles, a little behind the surface).
			units = 100;
			at = V3{0, 2.7f, 2.0f} - eyes * units;
		} else if (m.is_vrm() && head_bone(m, facing, hb)) {
			// Its head bone (the top of its neck) where yours would be,
			// for a head made like most: its eyes 4.6 cm up, 1.2 forward.
			units = 100;
			at = V3{0, -1.9f, 0.8f} - hb * units;
		} else {
			// Its middle over your face, 24 cm across: a big one round your
			// head, a long thin one out in front of your face.
			units = 24 / std::max(size.x, std::max(size.y, size.z));
			const float z = std::max(1.0f, 8.5f - size.z * units / 2);
			at = V3{0, 1, z} - middle * units;
		}
	}
	units *= std::max(0.01f, place.size);
	return moved(at) * scaled(units) * matrix_of(facing);
}

} // namespace

M4 matrix_of_rows(const float rows[16])
{
	M4 out;
	for (int r = 0; r < 4; r++)
		for (int c = 0; c < 4; c++)
			out.at(r, c) = rows[r * 4 + c];
	return out;
}

Quat head_turn(float yaw, float pitch, float tilt, bool mirror)
{
	const float flip = mirror ? -1.0f : 1.0f;
	return axis_angle({0, 0, 1}, -tilt * flip) * axis_angle({0, 1, 0}, yaw * flip * RAD) *
	       axis_angle({1, 0, 0}, -pitch * RAD);
}

Framing frame_own_spot(const Model &m, Quat facing, uint32_t w, uint32_t h, float size, bool head, float dx, float dy,
		       float grow)
{
	Framing f;
	V3 middle, extent;
	facing_bounds(m, facing, middle, extent);
	V3 hb;
	if (head && m.is_vrm() && head_bone(m, facing, hb)) {
		// Head and shoulders: about half a metre round its head.
		middle = hb + V3{0, 0.04f, 0};
		extent = {0.5f, 0.5f, 0.3f};
	}
	const float s = 1 / std::max(extent.x, extent.y);
	const float aspect = (float)std::max(1u, w) / std::max(1u, h);
	size = std::min(1.0f, std::max(0.05f, size));
	f.fov = 20 * RAD;
	const float t = std::tan(f.fov / 2);
	// Far enough back that its front fits too (nearer, it looks bigger).
	const float d = std::max(extent.y * s, extent.x * s / aspect) / (size * 2 * t) + extent.z * s / 2;
	const float seen_h = 2 * d * t, seen_w = seen_h * aspect;
	f.view = moved({0, 0, -d});
	f.seen_w = seen_w;
	f.seen_h = seen_h;
	f.near = d * 0.05f;
	f.far = d * 20;
	grow = std::max(0.05f, grow);
	f.place = scaled(s * grow) * moved(middle * -1) * matrix_of(facing);
	f.world_scale = s * grow;
	f.height = extent.y * s * grow;
	f.pivot = rotate(conj(facing), middle);
	f.upright = f.place;
	move_own_spot(f, dx, dy);
	return f;
}

void move_own_spot(Framing &f, float dx, float dy)
{
	// Across the picture, whichever way the camera's turned.
	const M4 by = moved(f.right * (dx * f.seen_w) + f.up * (-dy * f.seen_h));
	f.place = by * f.place;
	f.upright = by * f.upright;
}

CameraView sensible_view(const CameraView &v)
{
	auto fit = [](float x, float lo, float hi, float def) {
		return std::isfinite(x) ? std::min(hi, std::max(lo, x)) : def;
	};
	CameraView c;
	c.yaw = fit(v.yaw, -120, 120, 0);
	c.pitch = fit(v.pitch, -45, 60, 0);
	c.zoom = fit(v.zoom, 0.3f, 4, 1);
	c.x = fit(v.x, -0.5f, 0.5f, 0);
	c.y = fit(v.y, -0.5f, 0.5f, 0);
	return c;
}

CameraView view_between(const CameraView &a, const CameraView &b, float t)
{
	t = std::min(1.0f, std::max(0.0f, t));
	CameraView c;
	c.yaw = a.yaw + (b.yaw - a.yaw) * t;
	c.pitch = a.pitch + (b.pitch - a.pitch) * t;
	// Growing evenly, as a camera moving in does.
	c.zoom = a.zoom * std::pow(b.zoom / std::max(1e-3f, a.zoom), t);
	c.x = a.x + (b.x - a.x) * t;
	c.y = a.y + (b.y - a.y) * t;
	return c;
}

void view_own_spot(Framing &f, const CameraView &cv)
{
	const CameraView v = sensible_view(cv);
	// Round it and up: the world turned the other way under the camera,
	// round the middle of what's framed (the world's middle).
	const Quat turn = axis_angle({1, 0, 0}, v.pitch * RAD) * axis_angle({0, 1, 0}, -v.yaw * RAD);
	const M4 r = matrix_of(turn);
	// Closer: a narrower view from where it is (its light and its depth
	// stay as they were).
	const float t = std::tan(f.fov / 2) / v.zoom;
	f.fov = 2 * std::atan(t);
	f.seen_w /= v.zoom;
	f.seen_h /= v.zoom;
	f.view = f.view * r * moved(rotate(conj(turn), V3{-v.x * f.seen_w, v.y * f.seen_h, 0}) * -1.0f);
	f.right = rotate(conj(turn), {1, 0, 0});
	f.up = rotate(conj(turn), {0, 1, 0});
}

Framing frame_worn(const Model &m, Quat facing, const WornPlace &place, const M4 &pose, float aspect)
{
	Framing f;
	float units = 1;
	const M4 worn = worn_matrix(m, facing, place, units);
	f.place = pose * worn;
	f.fov = 63 * RAD;
	f.near = 1;
	f.far = 5000;
	(void)aspect;
	const float pose_scale = length(V3{pose.m[0], pose.m[1], pose.m[2]});
	f.world_scale = units * pose_scale;
	// Your head held straight: where it is, facing the camera.
	f.upright = moved(move_of(pose)) * scaled(pose_scale) * worn;
	V3 middle, extent;
	facing_bounds(m, facing, middle, extent);
	f.height = extent.y * f.world_scale;
	return f;
}

V3 head_in_model(const Model &m, Quat facing, const WornPlace &place)
{
	float units = 1;
	return point(inverse(worn_matrix(m, facing, place, units)), V3{});
}

namespace {

// The surface nearest you at (x, y), the model facing +z: its z, or false.
bool front_z(const std::vector<std::vector<V3>> &points, const std::vector<Rig::Part> &parts, float x, float y,
	     float &z)
{
	bool found = false;
	for (size_t pi = 0; pi < parts.size(); pi++) {
		const std::vector<uint32_t> &index = parts[pi].prim->index;
		const std::vector<V3> &p = points[pi];
		for (size_t t = 0; t + 2 < index.size(); t += 3) {
			if (index[t] >= p.size() || index[t + 1] >= p.size() || index[t + 2] >= p.size())
				continue;
			const V3 a = p[index[t]], b = p[index[t + 1]], c = p[index[t + 2]];
			const float d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
			if (std::fabs(d) < 1e-12f)
				continue;
			const float u = ((b.y - c.y) * (x - c.x) + (c.x - b.x) * (y - c.y)) / d;
			const float v = ((c.y - a.y) * (x - c.x) + (a.x - c.x) * (y - c.y)) / d;
			const float w = 1 - u - v;
			if (u < 0 || v < 0 || w < 0)
				continue;
			const float hit = a.z * u + b.z * v + c.z * w;
			if (!found || hit > z) {
				z = hit;
				found = true;
			}
		}
	}
	return found;
}

// What an expression does, for finding the eyes and mouth: 1 its left eye
// shuts, 2 its right, 3 both; 10 + how surely it opens the mouth.
int face_part_of(const std::string &name)
{
	static const int blink_l = move_id("eyeBlinkLeft"), blink_r = move_id("eyeBlinkRight"),
			 funnel = move_id("mouthFunnel"), pucker = move_id("mouthPucker"), jaw = move_id("jawOpen");
	const int move = arkit_move(name);
	if (move > 0) {
		if (move == blink_l)
			return 1;
		if (move == blink_r)
			return 2;
		if (move == funnel || move == pucker)
			return 12;
		if (move == jaw)
			return 11;
		return 0;
	}
	const std::string e = name == "blink" || name == "blinkLeft" || name == "blinkRight" || name == "aa"
				      ? name
				      : usual_expression(name);
	if (e == "blink")
		return 3;
	if (e == "blinkLeft")
		return 1;
	if (e == "blinkRight")
		return 2;
	if (e == "aa")
		return 13;
	return 0;
}

} // namespace

FaceRegion face_region(const FaceSpots &fs, Quat facing)
{
	const FaceRegion all;
	const float bw = fs.box[2], bh = fs.box[3];
	if (!(bw > 0 && bh > 0))
		return all;
	// The eyes and mouth together, in the box (x right, y down).
	float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
	for (const FaceSpot &s : fs.spot) {
		const V3 p = rotate(facing, s.at);
		const float x = p.x - fs.box[0], y = fs.box[1] - p.y;
		x0 = std::min(x0, x - s.w / 2);
		x1 = std::max(x1, x + s.w / 2);
		y0 = std::min(y0, y - s.h / 2);
		y1 = std::max(y1, y + s.h / 2);
	}
	// A face round them: three times as big, a little higher (a forehead
	// is bigger than a chin), square.
	const float side = 3 * std::max(x1 - x0, y1 - y0);
	if (!(side > 0) || !std::isfinite(side))
		return all;
	const float w = std::min(side, bw), h = std::min(side, bh);
	if (w > 0.6f * bw && h > 0.6f * bh)
		return all;
	const float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2 - 0.08f * side;
	const float left = std::clamp(cx - w / 2, 0.0f, bw - w), top = std::clamp(cy - h / 2, 0.0f, bh - h);
	return {left / bw, top / bh, w / bw, h / bh};
}

FaceSpots face_spots(const std::shared_ptr<const Model> &mp, Quat facing, const CharacterSetup &setup)
{
	const Model &m = *mp;
	FaceSpots out;
	V3 middle, size;
	facing_bounds(m, facing, middle, size);
	out.box[0] = middle.x - size.x / 2;
	out.box[1] = middle.y + size.y / 2;
	out.box[2] = size.x;
	out.box[3] = size.y;
	out.front = middle.z + size.z / 2;
	out.back = middle.z - size.z / 2;
	const Quat back = conj(facing);

	// The model at rest (as it stands, its own expressions only), facing +z.
	RigOptions ro;
	ro.facing = facing;
	ro.springs = false;
	ro.arms_down = false;
	Rig rig(mp, ro);
	Drive d;
	d.expressions = false;
	d.look = false;
	rig.pose(d);
	const std::vector<Rig::Part> &parts = rig.parts();
	std::vector<std::vector<V3>> points(parts.size());
	for (size_t pi = 0; pi < parts.size(); pi++) {
		points[pi].resize(parts[pi].pos.size());
		for (size_t v = 0; v < parts[pi].pos.size(); v++)
			points[pi][v] = rotate(facing, parts[pi].pos[v]);
	}
	auto surface = [&](float x, float y, float fallback) {
		float z = fallback;
		front_z(points, parts, x, y, z);
		return z;
	};
	bool have[3] = {};
	V3 at[3];

	// Its blink and mouth expressions: the middle of what they move. A
	// shape counts by every name it goes by - its own and each VRM
	// expression it's in (one "jawOpen" shape can be all of aa, ih, ou,
	// ee and oh).
	std::map<std::pair<int, int>, std::vector<std::string>> vrm_names;
	for (const Expression &e : m.expressions)
		for (const Expression::Bind &b : e.binds)
			vrm_names[{b.mesh, b.morph}].push_back(e.name);
	auto kind_of = [&](int mi, size_t k, const Mesh &mesh) {
		int eye = 0, mouth = 0;
		auto take = [&](const std::string &name) {
			const int kind = face_part_of(name);
			if (kind > 10)
				mouth = std::max(mouth, kind);
			else if (kind == 1 || kind == 2)
				eye = kind;
			else if (kind == 3 && eye == 0)
				eye = 3;
		};
		if (k < mesh.morph_names.size())
			take(mesh.morph_names[k]);
		auto named = vrm_names.find({mi, (int)k});
		if (named != vrm_names.end())
			for (const std::string &name : named->second)
				take(name);
		return eye ? eye : mouth;
	};
	// What each moves (the facing model's points) and how far; a blink of
	// both eyes, split between them afterwards.
	std::vector<std::pair<V3, float>> moved_by[3], both;
	struct Hit {
		int node, kind;
		V3 p;
		float w;
	};
	std::vector<Hit> hits;
	for (size_t pi = 0; pi < parts.size(); pi++) {
		const int mi = m.nodes[parts[pi].node].mesh;
		const Primitive &prim = *parts[pi].prim;
		if (mi < 0)
			continue;
		const Mesh &mesh = m.meshes[mi];
		for (size_t k = 0; k < prim.morphs.size(); k++) {
			const int kind = kind_of(mi, k, mesh);
			if (kind == 0)
				continue;
			const std::vector<V3> &moved = prim.morphs[k].pos;
			for (size_t v = 0; v < moved.size() && v < points[pi].size(); v++) {
				const float w = length(moved[v]);
				if (w >= 1e-7f)
					hits.push_back({parts[pi].node, kind, points[pi][v], w});
			}
		}
	}
	// Two characters in one file (each its own part, as a VRM made of
	// two has them) can share a blink and a mouth: your eyes and mouth go
	// on one of them, not stretched across both. Parts whose moving bits
	// are near each other (a face, its teeth, its lashes) are one
	// character; the one used is the one round the head bone (a VRM's
	// own character), else the one with the biggest mouth, else the most.
	std::map<int, std::pair<V3, V3>> box_of; // each part's moving bits
	for (const Hit &h : hits) {
		auto it = box_of.find(h.node);
		if (it == box_of.end())
			box_of[h.node] = {h.p, h.p};
		else {
			V3 &lo = it->second.first, &hi = it->second.second;
			lo = {std::min(lo.x, h.p.x), std::min(lo.y, h.p.y), std::min(lo.z, h.p.z)};
			hi = {std::max(hi.x, h.p.x), std::max(hi.y, h.p.y), std::max(hi.z, h.p.z)};
		}
	}
	std::map<int, int> who; // part -> character
	if (box_of.size() > 1) {
		std::vector<int> nodes;
		for (const auto &kv : box_of)
			nodes.push_back(kv.first);
		std::vector<int> up(nodes.size());
		for (size_t i = 0; i < up.size(); i++)
			up[i] = (int)i;
		std::function<int(int)> root = [&](int i) { return up[i] == i ? i : (up[i] = root(up[i])); };
		auto gap = [](const std::pair<V3, V3> &a, const std::pair<V3, V3> &b) {
			const V3 d{std::max(0.0f, std::max(a.first.x - b.second.x, b.first.x - a.second.x)),
				   std::max(0.0f, std::max(a.first.y - b.second.y, b.first.y - a.second.y)),
				   std::max(0.0f, std::max(a.first.z - b.second.z, b.first.z - a.second.z))};
			return length(d);
		};
		for (size_t i = 0; i < nodes.size(); i++)
			for (size_t j = i + 1; j < nodes.size(); j++) {
				const auto &a = box_of[nodes[i]], &b = box_of[nodes[j]];
				const float across =
					std::max(length(a.second - a.first), length(b.second - b.first));
				if (gap(a, b) <= across)
					up[root((int)i)] = root((int)j);
			}
		for (size_t i = 0; i < nodes.size(); i++)
			who[nodes[i]] = root((int)i);
	}
	if (!who.empty()) {
		std::map<int, float> mouth, all, near;
		std::map<int, std::pair<V3, V3>> box;
		for (const auto &kv : box_of) {
			const int c = who[kv.first];
			auto it = box.find(c);
			if (it == box.end())
				box[c] = kv.second;
			else {
				V3 &lo = it->second.first, &hi = it->second.second;
				const V3 &a = kv.second.first, &b = kv.second.second;
				lo = {std::min(lo.x, a.x), std::min(lo.y, a.y), std::min(lo.z, a.z)};
				hi = {std::max(hi.x, b.x), std::max(hi.y, b.y), std::max(hi.z, b.z)};
			}
		}
		for (const Hit &h : hits) {
			all[who[h.node]] += h.w;
			if (h.kind > 10)
				mouth[who[h.node]] += h.w;
		}
		int pick = who.begin()->second;
		const int head_bone = m.bone("head");
		if (head_bone >= 0) {
			const V3 hb = rotate(facing, move_of(rest_world(m)[head_bone]));
			float best = 1e30f;
			for (const auto &kv : box) {
				const V3 &lo = kv.second.first, &hi = kv.second.second;
				const V3 d{std::max(0.0f, std::max(lo.x - hb.x, hb.x - hi.x)),
					   std::max(0.0f, std::max(lo.y - hb.y, hb.y - hi.y)),
					   std::max(0.0f, std::max(lo.z - hb.z, hb.z - hi.z))};
				if (length(d) < best) {
					best = length(d);
					pick = kv.first;
				}
			}
		} else {
			const std::map<int, float> &by = mouth.empty() ? all : mouth;
			float best = -1;
			for (const auto &kv : by)
				if (kv.second > best) {
					best = kv.second;
					pick = kv.first;
				}
		}
		std::vector<Hit> kept;
		for (const Hit &h : hits)
			if (who[h.node] == pick)
				kept.push_back(h);
		hits.swap(kept);
	}
	// Which mouth expression says most surely where it is.
	int mouth_kind = 0;
	for (const Hit &h : hits)
		if (h.kind > 10)
			mouth_kind = std::max(mouth_kind, h.kind);
	for (const Hit &h : hits) {
		if (h.kind > 10 && h.kind != mouth_kind)
			continue;
		if (h.kind == 3)
			both.push_back({h.p, h.w});
		else
			moved_by[h.kind > 10 ? 2 : h.kind - 1].push_back({h.p, h.w});
	}
	// Both eyes' blink: each side of the middle of what it moves (not the
	// model's middle - two characters in one file), its own left eye on
	// the right.
	if (!both.empty() && (moved_by[0].empty() || moved_by[1].empty())) {
		float lo = 1e30f, hi = -1e30f;
		for (const auto &pw : both) {
			lo = std::min(lo, pw.first.x);
			hi = std::max(hi, pw.first.x);
		}
		const float split = (lo + hi) / 2;
		const bool need[2] = {moved_by[0].empty(), moved_by[1].empty()};
		for (const auto &pw : both) {
			const int which = pw.first.x >= split ? 0 : 1;
			if (need[which])
				moved_by[which].push_back(pw);
		}
	}
	// Each one's middle and size: the box round what it moves most (a
	// blink's lids, "aa"'s lips).
	V3 box_size[3];
	for (int i = 0; i < 3; i++) {
		float most = 0;
		for (const auto &pw : moved_by[i])
			most = std::max(most, pw.second);
		if (most <= 0)
			continue;
		V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f}, sum;
		float weight = 0;
		for (const auto &pw : moved_by[i]) {
			if (pw.second < 0.25f * most)
				continue;
			const V3 p = pw.first;
			lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
			hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
			sum = sum + p * pw.second;
			weight += pw.second;
		}
		at[i] = {(lo.x + hi.x) / 2, (lo.y + hi.y) / 2, sum.z / weight};
		box_size[i] = hi - lo;
		have[i] = true;
		out.from[i] = "expressions";
	}

	// Its eye bones (a VRM's), on the surface in front of them.
	const int eye_bone[2] = {m.bone("leftEye"), m.bone("rightEye")};
	if ((!have[0] || !have[1]) && eye_bone[0] >= 0 && eye_bone[1] >= 0) {
		const std::vector<M4> rest = rest_world(m);
		for (int i = 0; i < 2; i++) {
			const V3 b = rotate(facing, move_of(rest[eye_bone[i]]));
			at[i] = {b.x, b.y, surface(b.x, b.y, b.z)};
			have[i] = true;
			out.from[i] = "eye bones";
		}
	}
	// A guess: the front of its top half - or, with a head bone, of its
	// head (what's above that bone, about as wide as it's tall).
	const float top = middle.y + size.y / 2;
	if (!have[0] || !have[1]) {
		float cx = middle.x, gap = 0.3f * size.x, y = top - 0.35f * size.y;
		const int head_bone = m.bone("head");
		if (head_bone >= 0) {
			const V3 h = rotate(facing, move_of(rest_world(m)[head_bone]));
			const float reach = std::max(0.0f, top - h.y);
			V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
			size_t found = 0;
			for (const auto &part : points)
				for (const V3 &p : part)
					if (p.y >= h.y - 1e-3f * size.y && std::fabs(p.x - h.x) <= 0.6f * reach) {
						lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), lo.z};
						hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), hi.z};
						found++;
					}
			if (found >= 3 && hi.x > lo.x && hi.y > lo.y) {
				cx = (lo.x + hi.x) / 2;
				gap = 0.36f * (hi.x - lo.x);
				y = hi.y - 0.55f * (hi.y - lo.y);
			}
		}
		for (int i = 0; i < 2; i++) {
			const float x = cx + (i == 0 ? gap : -gap) / 2;
			at[i] = {x, y, surface(x, y, middle.z + size.z / 2)};
			out.from[i] = "guessed";
		}
	}
	const float gap = std::max(1e-4f, std::fabs(at[0].x - at[1].x));
	if (!have[2]) {
		const float x = (at[0].x + at[1].x) / 2;
		const float y = std::max(middle.y - size.y / 2, (at[0].y + at[1].y) / 2 - 0.8f * gap);
		at[2] = {x, y, surface(x, y, middle.z + size.z / 2)};
		out.from[2] = "guessed";
	}
	for (int i = 0; i < 3; i++) {
		FaceSpot &s = out.spot[i];
		// As big as what its expression moves (a drawn character's big eyes
		// covered), within reason; else as a person's are, for the gap.
		const V3 b = box_size[i];
		const bool eye = i < 2;
		float w = eye ? 0.55f * gap : 0.9f * gap;
		if (b.x > 0)
			w = std::min(eye ? 0.85f : 1.3f, std::max(eye ? 0.35f : 0.5f, b.x * 1.1f / gap)) * gap;
		float h = w * (eye ? 0.65f : 0.55f);
		if (b.y > 0)
			h = std::min(eye ? 0.95f : 0.7f, std::max(eye ? 0.5f : 0.4f, b.y * 1.1f / w)) * w;
		s.w = w;
		s.h = h;
		// Just in front of the surface, in the model.
		s.at = rotate(back, at[i] + V3{0, 0, 0.02f * gap});
	}

	// As its set-up says.
	for (int i = 0; i < 3; i++) {
		const SpotSetup &g = setup.spots[i];
		if (!g.given)
			continue;
		FaceSpot &s = out.spot[i];
		if (g.has_at) {
			s.at = g.at;
			s.w = g.w;
			s.h = g.h;
		} else {
			const float x = out.box[0] + g.x * out.box[2], y = out.box[1] - g.y * out.box[3];
			s.at = rotate(back, V3{x, y, surface(x, y, middle.z + size.z / 2) + 0.01f * size.x});
			s.w = g.w * out.box[2];
			s.h = g.h * out.box[2];
		}
		out.from[i] = "set up";
	}
	return out;
}

} // namespace direbot::face
