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

// A 3D character from a file, for the face avatar: our own reader for
// glTF 2.0 (.glb and .gltf) and VRM (.vrm, 1.0 and 0.x), the files
// Blender, VRoid Studio and most 3D tools save.
//
// What it reads: the parts (points, which way they face, where the
// pictures go on them, their bones and how much each bone pulls them,
// and their expressions - "morph targets"), the bones, the materials
// (colour, pictures, see-through, shiny or matte, and VRM's cartoon
// "MToon") and the pictures as the file has them (PNG or JPEG, turned
// into textures by OBS). From VRM too: which bone is the head, neck and
// eyes, the expressions (blink, "aa", happy...) and the swinging parts
// (spring bones) with what they bump into. And its own animations
// (moves, turns and sizes; not its expressions' strengths).
//
// Plain code over plain numbers (tests/unit_model.cpp). Every number in a
// file is checked before it's used, so a broken or hostile file only
// fails to load.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "math3d.hpp"

namespace direbot::face {

// A picture in the file, as it's stored (the graphics side decodes it).
struct ModelImage {
	std::vector<uint8_t> bytes;
	std::string mime; // "image/png", "image/jpeg" (or empty: look at the bytes)
	std::string name;
};

// A picture on a material: which, and KHR_texture_transform's move and
// size of where it lands.
struct TexRef {
	int image = -1;
	float offset[2] = {0, 0};
	float scale[2] = {1, 1};
	bool used() const { return image >= 0; }
};

enum class AlphaMode { Opaque, Mask, Blend };
enum class Shading { Lit, Toon, Unlit };
enum class Outline { None, World, Screen };

struct Material {
	std::string name;
	// Colours are linear (as glTF has them), 0 to 1.
	float base[4] = {1, 1, 1, 1};
	TexRef base_map;
	AlphaMode alpha = AlphaMode::Opaque;
	float cutoff = 0.5f;
	bool double_sided = false;
	float metallic = 1, roughness = 1;
	TexRef metal_map; // glTF: green is roughness, blue is metal
	float emissive[3] = {0, 0, 0};
	TexRef emissive_map;
	Shading shading = Shading::Lit;
	// Cartoon (VRM's MToon, as VRM 1.0 has it): the shadow side's colour,
	// where light turns to shadow (-1 to 1) and how sharply (0 soft, 1
	// sharp), a rim of light round the edge, a "matcap" picture of
	// shine, and an outline.
	float shade[3] = {0, 0, 0};
	TexRef shade_map;
	float shade_shift = 0, shade_toony = 0.9f;
	float rim[3] = {0, 0, 0};
	float rim_power = 5, rim_lift = 0, rim_mix = 1;
	TexRef rim_map;
	float matcap[3] = {1, 1, 1};
	TexRef matcap_map;
	Outline outline = Outline::None;
	float outline_width = 0; // World: model units; Screen: a share of the picture's height
	float outline_colour[3] = {0, 0, 0};
	float outline_mix = 1; // how much the outline takes the light
	int queue = 0;         // VRM's draw order among see-through parts
};

// An expression's change to a part: each point moved by this much (and
// its facing changed) at full strength.
struct MorphTarget {
	std::vector<V3> pos, normal; // normal is empty if the file has none
};

struct Primitive {
	std::vector<V3> pos, normal;  // normal: made from the triangles if the file has none
	std::vector<float> uv;        // 2 a point (empty: none)
	std::vector<float> colour;    // 4 a point, linear (empty: none)
	std::vector<uint16_t> joints; // 4 a point (empty: no bones)
	std::vector<float> weights;   // 4 a point, adding up to 1
	std::vector<uint32_t> index;  // 3 a triangle
	std::vector<MorphTarget> morphs;
	int material = -1; // -1: plain white
};

struct Mesh {
	std::string name;
	std::vector<Primitive> parts;
	std::vector<std::string> morph_names; // as many as each part has morphs (names may be empty)
	std::vector<float> weights;           // each morph's own strength
};

struct Node {
	std::string name;
	int parent = -1;
	std::vector<int> children;
	V3 t;
	Quat r;
	V3 s{1, 1, 1};
	int mesh = -1, skin = -1;
	M4 local() const { return matrix_of(t, r, s); }
};

struct Skin {
	std::vector<int> joints;
	std::vector<M4> inverse_bind;
};

// A VRM expression ("blink", "aa", "happy"... VRM 1.0's names, 0.x's
// turned into them) or the file's own.
struct Expression {
	std::string name;
	bool preset = false;
	struct Bind {
		int mesh = -1, morph = -1;
		float weight = 1; // 0 to 1
	};
	std::vector<Bind> binds;
	bool binary = false;
	// What it does to the blinks, eye looks and mouth shapes while it's
	// on: 0 nothing, 1 stops them, 2 weakens them by as much.
	int override_blink = 0, override_look = 0, override_mouth = 0;
};

// A swinging bone: `node` turns so its tail (`tail_node`'s place, or
// `tail` in its own space when that's -1) swings as it's thrown about,
// pulled back to how it rests (`stiffness`), down (`gravity` along
// `gravity_dir`) and slowed (`drag`, 0 to 1). `radius`: how far from the
// tail it keeps out of what it bumps into. VRM's numbers (metres).
struct SpringJoint {
	int node = -1, tail_node = -1;
	V3 tail;
	float radius = 0, stiffness = 1, gravity = 0, drag = 0.5f;
	V3 gravity_dir{0, -1, 0};
};
// A chain or tree of them, parents first, and what they bump into.
struct Spring {
	std::string name;
	std::vector<SpringJoint> joints;
	std::vector<int> colliders;
};
// A ball (or a capsule from `offset` to `tail`) on a bone.
struct Collider {
	int node = -1;
	V3 offset, tail;
	float radius = 0;
	bool capsule = false;
};

// VRM's eye looks: up to how many degrees the eyes turn (bone looks) or
// how strong the look expressions get, for a look `in` degrees.
struct LookRange {
	float in = 90, out = 10;
};

// The file's own animation (glTF's): its parts' moves over time, an
// idle, a tail's wag... (played over and over by the rig, if chosen).
// Each channel moves one node: where it is, its turn or its size, key
// by key - jumping to each key, straight between them (turns the
// shortest way round), or curved by each key's slopes.
struct AnimChannel {
	enum Path { Move, Turn, Size };
	enum Ease { Step, Straight, Curved };
	int node = -1;
	Path path = Turn;
	Ease ease = Straight;
	std::vector<float> times;  // seconds, rising
	std::vector<float> values; // 3 a key (4 for a turn); Curved: its slope in, value and slope out
	int size() const { return path == Turn ? 4 : 3; }
};
struct Animation {
	std::string name; // as the file has it, or "Animation 1", "Animation 2"...
	std::vector<AnimChannel> channels;
	float length = 0; // seconds: its last key
};
struct ModelMeta {
	std::string title, author, licence;
	std::string allowed;    // who may be it ("onlyAuthor", "everyone"...)
	std::string commercial; // may it earn money ("personalNonProfit", "corporation"...)
};

// An outfit's 3D piece put on the character (put_on): what was added for
// it. Its nodes and meshes come after the character's own, in order.
struct WornPiece {
	int first_node = 0, end_node = 0; // [first, end): its own bones and parts
	int first_mesh = 0, end_mesh = 0;
	int first_skin = 0, end_skin = 0;
	bool hide = true; // the character's own skin under it isn't drawn (hide_under_pieces)
};

struct Model {
	std::vector<Node> nodes;
	std::vector<Mesh> meshes;
	std::vector<Skin> skins;
	std::vector<Material> materials;
	std::vector<ModelImage> images;
	std::vector<int> roots; // the scene's top nodes

	int vrm = 0; // 0 plain glTF, 1 VRM 1.0, -1 VRM 0.x
	bool is_vrm() const { return vrm != 0; }
	ModelMeta meta;
	// The humanoid bones VRM names ("head", "neck", "leftEye"...).
	std::map<std::string, int> bones;
	int bone(const std::string &name) const
	{
		auto it = bones.find(name);
		return it == bones.end() ? -1 : it->second;
	}
	std::vector<Expression> expressions;
	bool look_bones = true; // eyes look by turning bones (else by expressions)
	LookRange look_in, look_out, look_up, look_down;
	std::vector<Spring> springs;
	std::vector<Collider> colliders;
	std::vector<Animation> animations;
	// Which way it faces as it is: VRM 1.0 and glTF along +z, VRM 0.x -z.
	bool faces_back = false;

	// Its size standing as it is (no expressions, bones at rest) - the
	// character's own, outfit pieces left out, so a rifle on its back
	// doesn't shrink it in its spot.
	V3 low, high;
	size_t points = 0, triangles = 0;
	// Outfit pieces put on it (put_on), in order.
	std::vector<WornPiece> worn;
	// How many of its meshes, skins and nodes are its own (the rest are
	// its pieces').
	size_t own_meshes() const { return worn.empty() ? meshes.size() : (size_t)worn.front().first_mesh; }
	size_t own_skins() const { return worn.empty() ? skins.size() : (size_t)worn.front().first_skin; }
};

// A channel at `t` seconds (before its first key: its first; after its
// last: its last), into `out` (3 numbers, or a turn's 4).
void sample(const AnimChannel &c, float t, float *out);
// An animation's number by its name, or -1 ("" or none so named).
int animation_named(const Model &m, const std::string &name);

// Loads a .glb, .vrm or .gltf (with its files beside it). False, with why
// in plain words, if it can't.
bool load_model(const std::string &path, Model &out, std::string *problem = nullptr);
// From memory: `folder` is where a .gltf's other files are ("" for none).
bool load_model(const uint8_t *bytes, size_t size, const std::string &folder, Model &out,
		std::string *problem = nullptr);

// A skin's dangly parts: every line of bones, parent to child, that
// hangs off the rest of it and ends free - a chain of links (all of
// its bones), a doll's ears and legs. A line starts
// where the skeleton starts or branches; one bone alone isn't one.
std::vector<std::vector<int>> hanging_chains(const Model &m, const Skin &skin);
// Whether a skin's bones are named as a person's are (hips, spine,
// arms, legs...): its arms and fingers mustn't dangle.
bool person_skeleton(const Model &m, const Skin &skin);

// Every node's place in the model at rest (world matrices).
std::vector<M4> rest_world(const Model &m);

// --- Outfit pieces (0.5.0) ------------------------------------------------
//
// A 3D piece of an outfit in a file of its own - a shirt, boots, a vest,
// a hat - put on a character as it's loaded, so it moves with it:
//
//   - A piece with bones (made on the character's own skeleton, as
//     Blender exports a piece of clothing): its bones are found among the
//     character's by name - the same, any case, or the same with a prefix
//     the character's bones all have ("wolf_hips" for "hips", a file of
//     two characters) - and it's fitted onto them (moved, turned and
//     sized as its bones are to theirs; each point then follows its own
//     bones' places). Bones of its own (a cape's) hang off the nearest of
//     the character's, and swing.
//   - A loose piece (no bones: a hat from anywhere): it rides one of the
//     character's bones (the nearest, or the one named), nudged into
//     place. One far away or of another size is put on top of its head
//     first, to nudge from.
//
// The character's own skin under a piece can be hidden (hide_under_pieces),
// so it doesn't poke through as it moves.
struct PieceHow {
	bool hide = true;
	// A loose piece's bone: a VRM bone's name ("head", "leftHand"...) or
	// the file's own; "" the nearest.
	std::string bone;
	// A loose piece's nudge, as the character faces you: moved right, up
	// and towards you (shares of its height), turned round those (degrees)
	// and grown round its middle.
	V3 move, turn;
	float size = 1;
	// The turn from the character as it is to facing you (RigOptions::facing).
	Quat facing;
};
struct PieceOn {
	bool on = false;
	std::string problem; // why not, in plain words
	int bones = 0;       // its bones found among the character's
	int own_bones = 0;   // bones of its own, added
	bool loose = false;
	std::string rides; // a loose piece's bone
	bool placed = false; // a loose piece put on its head first (it was far away, or another size)
};
PieceOn put_on(Model &m, const Model &piece, const PieceHow &how);
// The character's own triangles under its pieces (those with `hide`)
// taken out: every corner of one covered. How many were.
size_t hide_under_pieces(Model &m);
// One of a character's bones by a VRM bone's name or its own, or -1.
int bone_called(const Model &m, const std::string &name);

// A plain .glb from parts (for tests and for characters built in DireBot):
// one node a part, its picture (a PNG's bytes, or none) on it.
struct GlbPart {
	std::string name;
	std::vector<V3> pos, normal;
	std::vector<float> uv;
	std::vector<uint32_t> index;
	float colour[4] = {1, 1, 1, 1};
	float metallic = 0, roughness = 0.5f;
	std::vector<uint8_t> png;
	V3 t;
	Quat r;
	// Expressions on it, by name.
	std::vector<std::string> morph_names;
	std::vector<std::vector<V3>> morphs;
};
std::vector<uint8_t> write_glb(const std::vector<GlbPart> &parts);

} // namespace direbot::face
