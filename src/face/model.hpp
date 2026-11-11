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
// (spring bones) with what they bump into; its bones that follow others
// (VRM's node constraints: twist bones, sleeves). And its own animations
// (moves, turns and sizes; its shapes' strengths only where the file
// asks for them - Node::plays_shapes), and VRM animation files (.vrma)
// for any VRM.
//
// Plain code over plain numbers (tests/unit_model.cpp). Every number in a
// file is checked before it's used, so a broken or hostile file only
// fails to load.

#pragma once

#include <algorithm>
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
	// Which sides of its triangles show: glTF's front only (the side its
	// corners go round anticlockwise), both (doubleSided), or - VRM 0.x's
	// MToon culling its front - only the back.
	bool double_sided = false;
	bool inside_only = false;
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
	// Detail pictures (glTF's normal and occlusion textures, on the
	// part's first UVs): small bumps and dents the light picks out (and
	// how strongly), and how shut-in each spot is from the room's light
	// (and how much that counts).
	TexRef normal_map;
	float normal_scale = 1;
	TexRef occlusion_map;
	float occlusion_strength = 1;
	// 0.9.0: a glowing see-through part - fire, sparks, smoke (glTF's
	// extras.direbot.glow): a soft gas that adds its light (its emissive
	// colour x its picture x its points' colour) to what's behind it, hides
	// what's behind it only as much as its base colour's alpha says, and
	// thins out where it's seen edge on (`glow_soft`, a power) - with
	// `glow_ragged`, eating into its picture's thin places first, so its
	// outline breaks up into wisps. Each one is drawn at its middle's depth
	// (data/effects/direbot-face-3d.effect).
	bool glow = false;
	float glow_soft = 1.5f, glow_ragged = 0;
	// Stand-in hair (glTF's extras.direbot.hair_standin: true): strips,
	// shells or pictures a character wears where its file also has real
	// hair strands. Drawn only while its strands aren't (the "today's
	// style" hair setting, or a file loaded with no strands).
	bool hair_standin = false;
};

// Which of a part's sides are drawn (glTF's front and back): both, its
// front, or only its back - each the other way round when the part is
// mirrored (`handed` -1: its corners go round the other way).
enum class Sides { Both, Front, Back };
inline Sides sides_shown(const Material &m, float handed)
{
	if (m.double_sided)
		return Sides::Both;
	return m.inside_only == (handed < 0) ? Sides::Front : Sides::Back;
}

// An expression's change to a part, kept only for the points it moves:
// point at[i] moved by pos[i] (and its facing changed by normal[i]) at
// full strength. A file's expression usually moves a small part of a
// big character (a blink, its eyelids), and a part with nothing moved
// keeps nothing. normal is empty if the file has none.
struct MorphTarget {
	std::vector<uint32_t> at; // in order, each once
	std::vector<V3> pos, normal;
	// Its move of point v (none if it doesn't move it).
	V3 pos_of(uint32_t v) const;
};

// A part's hair strands (a glTF LINES or LINE_STRIP part), made ready to
// draw. The part's points (Primitive::pos, each once), its strands back to
// back from root to tip, in the order they're drawn in: strand 0 first,
// and however few are drawn they're spread evenly over the coat (so hair
// thins by drawing only the first so many, each a little wider). A big
// groom is kept only as far as the points allowed (LoadOptions): the
// file's first strands in that order, each with the points that shape it
// (points on a straight stretch left out).
//
// Not every hair is worth as much. Where the coat is thin - short fur
// over the skin, a muzzle, the edge of a beard - each hair shows, and a
// few wide ones in their place look like string. Where it's deep, the
// hairs under the top ones only fill. So each strand has a `rate`: 1
// where the coat is thinnest, less the deeper it is there (never under
// 1 / LoadOptions::strand_uneven), and a `key`: its place among the
// file's strands (0 to 1, evenly spread) / its rate. The draw order is
// by key, and the strands drawn are those with keys under a threshold
// t: min(1, t x rate) of the hairs everywhere, each drawn as much wider.
// So thinning takes from the deep coat first, down to its rate, and then
// from all of it alike. A coat the same all over: every rate 1, its keys
// its strands' places, t the share drawn.
struct Strands {
	uint32_t in_file = 0;        // how many strands the file has
	std::vector<uint32_t> first; // strand s: points first[s] up to first[s + 1] (one more entry than strands)
	std::vector<float> width;    // a point: its width (the file's _WIDTH, its units; 0.0001 if none)
	std::vector<float> along;    // a point: how far along its strand (0 root, 1 tip)
	std::vector<uint8_t> colour; // 4 a point, sRGB (empty: none)
	std::vector<float> key;      // a strand: where it comes in the draw order (they rise; under 4)
	std::vector<float> rate;     // a strand: 0.25 to 1, one of RATES steps (rate_of)
	// The threshold all its kept strands are drawn at: the key of the
	// file's first strand not kept (all kept: 1 / its lowest rate).
	float full = 1;
	// Its hairs' width along their lengths, as drawn at `full` (a strand
	// of whose neighbours only a share are kept: that much wider).
	float mean_width = 0;
	// Sent to the card as lines (hair.hpp's hair_form: a line is a pixel
	// wide, and can't stand for a hair wider than about LINE_COVERS of
	// one), its wider hairs lose some of what they cover. How wide a pixel
	// has to be - in its file's units, its hairs as drawn at `full` - for
	// all of them together to lose a hundredth of it (`line_pixel`), and a
	// twentieth (`line_pixel_most`). Hairs all one width: 1.24 and 1.19
	// times that width.
	static constexpr float LINE_COVERS = 0.8f;
	float line_pixel = 0, line_pixel_most = 0;
	// And for them to lose more of it, each of LINE_LOSSES (hair.hpp's
	// hair_line_pixel: how thin a budget may leave a coat drawn as lines).
	static constexpr int LINE_LOSSES = 5;
	static constexpr float LINE_LOSS[LINE_LOSSES] = {0.1f, 0.15f, 0.2f, 0.3f, 0.4f};
	float line_pixel_at[LINE_LOSSES] = {0, 0, 0, 0, 0};
	size_t strands() const { return first.empty() ? 0 : first.size() - 1; }
	// How many of its strands (the first of them) lie wholly in its first
	// `points` points.
	size_t strands_in(size_t points) const;
	// How many of its strands (the first of them) have keys under
	// `threshold`, and the threshold its first `count` strands are drawn
	// at.
	size_t strands_under(float threshold) const;
	float threshold_of(size_t count) const;
	// A rate as kept (and as the graphics card is told it): the nearest
	// of RATES steps from 0.25 to 1.
	static constexpr int RATES = 64;
	static int rate_step(float rate);
	static float rate_of(int step) { return 0.25f + 0.75f * (float)step / (float)(RATES - 1); }

	// The order its pieces are drawn in (a piece: two triangles from one
	// point to the next of its strand). Hairs are see-through, so they're
	// blended from the back: by `groups` - the strands that hang from one
	// bone, far groups first (worked out each frame) - and inside a group
	// by `layers`, the pieces nearest their roots first (a coat's tips lie
	// over its roots), and inside a layer by strand, so the first so many
	// strands' pieces are the start of each run.
	struct Group {
		int joint = 0;       // its skin's joint (no bones: 0)
		V3 middle;           // its strands' middle, as made
		uint32_t sample = 0; // a point near it
		// How far from their roots its inner layers (the first `core` of
		// them) reach, in its file's units: where its outer layers'
		// pieces start, or where the inner ones' end on average (hairs
		// of a piece or two), whichever is further.
		float reach = 0;
	};
	std::vector<Group> groups;
	int layers = 0;
	// The inner layers: the hull, which hides hair well behind it (the
	// drawing's; none with a single layer).
	int core = 0;
	std::vector<uint32_t> pieces; // each: the point it starts at
	// groups x layers runs of `pieces`, a group's layers back to back:
	// run g * layers + l is pieces runs[g * layers + l] up to the next.
	std::vector<uint32_t> runs;
	// A coat whose hairs aren't all one width - a few wide ones among many
	// thin - in two sorts, so its thin pieces can go to the card as lines
	// while its wide ones stay ribbons (hair.hpp's hair_form): a piece is
	// thin when it's no wider (as drawn at `full`) than `thin_width`,
	// THIN_WIDTH times the width half its hairs' length is under. Then
	// each run is its wide pieces first, runs[r] up to thin[r], and its
	// thin ones after, each sort by strand. Only when at least THIN_MOST
	// of its pieces are thin and some aren't: otherwise one sort
	// (`thin` empty, thin_width 0), a run all by strand.
	static constexpr float THIN_WIDTH = 1.5f, THIN_MOST = 0.5f;
	float thin_width = 0;
	std::vector<uint32_t> thin; // a run: where its thin pieces start
	float thin_share = 0;       // how many of its pieces are thin (0 to 1)
	// As line_pixel, line_pixel_most and line_pixel_at, of its thin pieces
	// alone.
	float thin_pixel = 0, thin_pixel_most = 0;
	float thin_pixel_at[LINE_LOSSES] = {0, 0, 0, 0, 0};
	// A piece's width as drawn at `full`: the piece from point `i` of
	// strand `s`.
	double piece_width(size_t s, size_t i) const
	{
		return 0.5 * ((double)width[i] + (double)width[i + 1]) / std::min(1.0, (double)full * (double)rate[s]);
	}
	// A run's pieces that belong to the first `count` strands: a stretch
	// of `pieces` - two in two sorts, its wide ones' and its thin ones'.
	struct Stretch {
		uint32_t from = 0, count = 0;
	};
	void pieces_of(size_t run, size_t count, Stretch out[2]) const;
	// How many of them there are.
	uint32_t pieces_of(size_t run, size_t count) const;

	// Expressions (the mesh's morph targets) move hair with the skin it
	// grows from: each point follows one of up to MOST_ANCHORS spots
	// (anchor[point], 0: it never moves), and expression k moves spot a by
	// moves[k][a] at full strength. Points close together that an
	// expression moves alike share a spot.
	static constexpr size_t MOST_ANCHORS = 2047;
	std::vector<uint16_t> anchor;       // a point (empty: none move)
	std::vector<std::vector<V3>> moves; // an expression: each spot's move (spot 0: none)
	size_t anchors() const { return moves.empty() ? 0 : moves[0].size(); }
	// How far the worst of its points is left from where the file would
	// move it, following its spot instead (the file's units; 0: every
	// moved point just where the file puts it).
	float spots_off = 0;
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
	// 4 a point, only on a part with a normal map (empty: none): the way
	// its picture's right is (x y z), and which way its up is (w, 1 or
	// -1: up = w * cross(normal, tangent)) - from the file, or worked
	// out from its UVs (tangents_of).
	std::vector<float> tangent;
	bool own_tangents = false; // its tangents are the file's (not worked out)
	// Hair strands (a glTF LINES or LINE_STRIP part): `pos` its strands'
	// points (see Strands), `normal` the way its strand runs at each
	// (bones turn it as they turn a facing), `joints` and `weights` each
	// point's; no uv, colour (hair.colour), index (hair.pieces) or morphs
	// (hair.moves). Drawn by the effect's Strand technique, each piece a
	// ribbon facing the camera.
	bool strands = false;
	Strands hair;
};
// A part's tangents worked out from its points, facings and UVs with
// MikkTSpace, as glTF asks (its UVs' v going down the picture, a normal
// map's green up) and as Blender has them.
std::vector<float> tangents_of(const Primitive &p);

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
	// Its size is a switch (the file says so: "extras": {"direbot":
	// {"switch": true}}): a part shown or put away by a size track that
	// jumps (STEP) is never played part-way - not while a move eases in
	// or out over how it stood (rig.cpp's play_own).
	bool size_switch = false;
	// Its mesh's shapes are played by the file's own animations (the file
	// says so: "extras": {"direbot": {"shapes": true}}): their "weights"
	// tracks - one that jumps (STEP) all of it as soon as a move plays at
	// all, the others eased with the move as its bones are. Without it
	// such tracks aren't read (as before 0.9.0's hair work: a file's
	// shapes are its face's, moved by yours).
	bool plays_shapes = false;
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

// A bone that follows another (VRM's node constraints,
// VRMC_node_constraint 1.0): `node` turns `weight` (0 to 1) of the way
// from how it rests to where `source` takes it -
//   - Roll (twist bones): only round its own `axis` (x, y or z), as much
//     as `source` has turned round it since it rested;
//   - Aim (a sleeve): turned the least it takes for its `axis` (+ or -
//     x, y or z) to point at `source`;
//   - Rotation (a second arm): turned as `source` has turned since it
//     rested, in its own space.
struct Constraint {
	enum Kind { Roll, Aim, Rotation };
	Kind kind = Rotation;
	int node = -1, source = -1;
	V3 axis{1, 0, 0};
	float weight = 1;
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
// shortest way round), or curved by each key's slopes. Or (Shapes, glTF's
// "weights") the strengths of its mesh's shapes - only of a node whose
// file asks for them to be played (Node::plays_shapes).
struct AnimChannel {
	enum Path { Move, Turn, Size, Shapes };
	enum Ease { Step, Straight, Curved };
	int node = -1;
	Path path = Turn;
	Ease ease = Straight;
	int shapes = 0;            // Shapes: how many its mesh has (numbers a key)
	std::vector<float> times;  // seconds, rising
	std::vector<float> values; // 3 a key (4 for a turn); Curved: its slope in, value and slope out
	int size() const { return path == Turn ? 4 : path == Shapes ? shapes : 3; }
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
	// Its bones that follow others, in the order they're worked out (each
	// after those it waits on); and how many the file has that can't be
	// (one following itself, or caught in a loop of them, or waiting on
	// one that is: left out, as the spec says no file should have them).
	std::vector<Constraint> constraints;
	int constraints_left_out = 0;
	std::vector<Animation> animations;
	// Which way it faces as it is: VRM 1.0 and glTF along +z, VRM 0.x -z.
	bool faces_back = false;

	// Its size standing as it is (no expressions, bones at rest).
	V3 low, high;
	size_t points = 0, triangles = 0;
	// Its hair strands: how many its file has, those kept (LoadOptions)
	// and their points; and whether it wears stand-in hair too
	// (Material::hair_standin).
	size_t strands_in_file = 0, strands = 0, strand_points = 0;
	bool hair_standins = false;
};

// How a file is loaded.
struct LoadOptions {
	// Hair strands: the most points kept, all its strand parts together
	// (the first strands of each in their draw order, a part's share by
	// its size). 0: none - but a character with no stand-in hair
	// (Material::hair_standin) to wear instead keeps `strand_points_bare`.
	size_t strand_points = 2'000'000;
	size_t strand_points_bare = 800'000;
	// A strand's points that bend it less than this share of the
	// character's size are left out (0: all kept).
	float strand_bend = 0.0003f;
	// ...unless the strand turns more than this many degrees between the
	// points kept either side (a curl stays round; 0: no such limit).
	float strand_turn = 20.0f;
	// ...unless its bones could bend the strand more than this share of
	// the character's size there (a straight hair on a chain of bones
	// keeps enough points to swing in a curve).
	float strand_swing = 0.004f;
	// How many layers each group of strands is drawn in (Strands::layers).
	int strand_layers = 5;
	// When not every strand can be kept, which are (Strands::rate): hair
	// that covers its skin up to `strand_cover` times over (its hairs'
	// own widths x lengths, over the skin they grow from) is kept first,
	// deeper hair less - as much less as it's deeper, down to
	// 1 / `strand_uneven` of the thin hair's share (1: all alike; at
	// most 4).
	float strand_cover = 1.5f;
	float strand_uneven = 3.0f;
	bool operator==(const LoadOptions &o) const
	{
		return strand_points == o.strand_points && strand_points_bare == o.strand_points_bare &&
		       strand_bend == o.strand_bend && strand_turn == o.strand_turn && strand_swing == o.strand_swing &&
		       strand_layers == o.strand_layers && strand_cover == o.strand_cover &&
		       strand_uneven == o.strand_uneven;
	}
	bool operator!=(const LoadOptions &o) const { return !(*this == o); }
};

// A channel at `t` seconds (before its first key: its first; after its
// last: its last), into `out` (3 numbers, or a turn's 4).
void sample(const AnimChannel &c, float t, float *out);
// An animation's number by its name, or -1 ("" or none so named).
int animation_named(const Model &m, const std::string &name);

// Loads a .glb, .vrm or .gltf (with its files beside it). False, with why
// in plain words, if it can't.
bool load_model(const std::string &path, Model &out, std::string *problem = nullptr,
		const LoadOptions &options = LoadOptions());
// From memory: `folder` is where a .gltf's other files are ("" for none).
bool load_model(const uint8_t *bytes, size_t size, const std::string &folder, Model &out,
		std::string *problem = nullptr, const LoadOptions &options = LoadOptions());

// A skin's dangly parts: every line of bones, parent to child, that
// hangs off the rest of it and ends free - a chain of links (all of
// its bones), a doll's ears and legs. A line starts
// where the skeleton starts or branches; one bone alone isn't one.
std::vector<std::vector<int>> hanging_chains(const Model &m, const Skin &skin);
// Whether a skin's bones are named as a person's are (hips, spine,
// arms, legs...): its arms and fingers mustn't dangle.
bool person_skeleton(const Model &m, const Skin &skin);

// Which of its materials are skin, for soft skin: the ones named in
// `names` (a name a line, any case), or with none named, the ones whose
// names say so ("skin", "face", "body" or "head" in them) - never eyes,
// hair, brows, lashes, teeth, a tongue or clothes.
std::vector<bool> skin_materials(const Model &m, const std::string &names);

// Every node's place in the model at rest (world matrices).
std::vector<M4> rest_world(const Model &m);

// A VRM animation file (.vrma: VRM's own, VRMC_vrm_animation 1.0) - a
// wave, a dance, a bow - for any VRM to play (rig.hpp). Its moves go by
// bone name, so one file fits every VRM. (Carried onto a character's
// bones as the VRM Animation spec says, the way pixiv's three-vrm - MIT
// licence - does it: its steps as a recipe, no code copied.)
struct MotionTrack {
	std::string bone; // VRM 1.0's name ("hips", "leftUpperArm"...)
	// Turn: each key the bone's turn from how it rests, in the file's
	// world (its rest's turn taken out: VRM's "normalized" turn, the same
	// for any character). Move (its hips only): where its hips are, in
	// the file's world. (The channel's node isn't used: -1.)
	AnimChannel channel;
};
struct Motion {
	std::string name;   // the file's name for it (may be empty)
	float length = 0;   // seconds: its last key
	std::vector<MotionTrack> tracks;
	V3 hips_rest;       // where its hips are at rest (its height's scale)
	// Its expressions ("happy", "aa", "blink"... or the file's own): each
	// one's strength over time (the channel's first number).
	std::vector<std::pair<std::string, AnimChannel>> expressions;
};
// Loads a .vrma. False, with why in plain words, if it can't.
bool load_motion(const std::string &path, Motion &out, std::string *problem = nullptr);
bool load_motion(const uint8_t *bytes, size_t size, const std::string &folder, Motion &out,
		 std::string *problem = nullptr);
// The VRM humanoid bone a bone hangs from (VRM 1.0's names): "" for the
// hips, nullptr if it isn't one.
const char *humanoid_parent(const std::string &bone);

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
