// Unit tests for the 3D face avatar's model reader and posing
// (src/face/model.cpp, src/face/rig.cpp), on models made here: plain
// glTF the ways files store their numbers (packed, interleaved, sparse,
// squeezed into bytes, strips and fans, matrices), broken and hostile
// files, VRM 1.0 and 0.x (bones, expressions, swinging parts, what they
// bump into), and a chain of links swinging on one line of bones.
// Build and run: tools/test-link.sh (Linux, any C++17 compiler).

#include "../src/face/model.hpp"
#include "../src/face/rig.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

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
static bool near3(V3 a, V3 b, float by)
{
	return length(a - b) <= by;
}

static int move_of_name(const char *name)
{
	for (int i = 0; i < EXPRESSIONS; i++)
		if (std::strcmp(EXPRESSION_NAMES[i], name) == 0)
			return i;
	return -1;
}

// --- Making glTF files ------------------------------------------------------

struct Builder {
	json doc = {{"asset", {{"version", "2.0"}}}};
	std::vector<uint8_t> bin;

	int view(const void *data, size_t bytes, int stride = 0)
	{
		while (bin.size() % 4)
			bin.push_back(0);
		const size_t at = bin.size();
		bin.resize(at + bytes);
		std::memcpy(bin.data() + at, data, bytes);
		json v = {{"buffer", 0}, {"byteOffset", at}, {"byteLength", bytes}};
		if (stride)
			v["byteStride"] = stride;
		doc["bufferViews"].push_back(v);
		return (int)doc["bufferViews"].size() - 1;
	}
	int accessor(int view_i, int type, size_t count, const char *kind, size_t offset = 0, bool normalized = false)
	{
		json a = {{"bufferView", view_i}, {"componentType", type}, {"count", count}, {"type", kind}};
		if (offset)
			a["byteOffset"] = offset;
		if (normalized)
			a["normalized"] = true;
		doc["accessors"].push_back(a);
		return (int)doc["accessors"].size() - 1;
	}
	int floats(const std::vector<float> &f, int comps, const char *kind)
	{
		return accessor(view(f.data(), f.size() * 4), 5126, f.size() / comps, kind);
	}
	int vec3s(const std::vector<V3> &v)
	{
		return floats(std::vector<float>(&v[0].x, &v[0].x + v.size() * 3), 3, "VEC3");
	}
	int indices(const std::vector<uint16_t> &ix)
	{
		return accessor(view(ix.data(), ix.size() * 2), 5123, ix.size(), "SCALAR");
	}
	int node(json n)
	{
		doc["nodes"].push_back(n);
		return (int)doc["nodes"].size() - 1;
	}
	std::vector<uint8_t> glb()
	{
		json d = doc;
		d["buffers"] = json::array({{{"byteLength", bin.size()}}});
		if (!d.contains("scenes")) {
			json roots = json::array();
			std::vector<bool> child(d["nodes"].size(), false);
			for (const json &n : d["nodes"])
				if (n.contains("children"))
					for (const json &c : n["children"])
						child[c.get<int>()] = true;
			for (size_t i = 0; i < child.size(); i++)
				if (!child[i])
					roots.push_back(i);
			d["scenes"] = json::array({{{"nodes", roots}}});
		}
		std::string text = d.dump();
		while (text.size() % 4)
			text.push_back(' ');
		std::vector<uint8_t> b = bin;
		while (b.size() % 4)
			b.push_back(0);
		std::vector<uint8_t> out;
		auto put = [&out](uint32_t v) {
			for (int i = 0; i < 4; i++)
				out.push_back((uint8_t)(v >> (8 * i)));
		};
		out.insert(out.end(), {'g', 'l', 'T', 'F'});
		put(2);
		put((uint32_t)(28 + text.size() + b.size()));
		put((uint32_t)text.size());
		put(0x4E4F534A);
		out.insert(out.end(), text.begin(), text.end());
		put((uint32_t)b.size());
		put(0x004E4942);
		out.insert(out.end(), b.begin(), b.end());
		return out;
	}
};

static bool load(const std::vector<uint8_t> &bytes, Model &m, std::string *why = nullptr)
{
	std::string w;
	const bool ok = load_model(bytes.data(), bytes.size(), "", m, &w);
	if (why)
		*why = w;
	return ok;
}

// A unit cube's 8 corners and 12 triangles, middle at `c`.
static void cube(std::vector<V3> &pos, std::vector<uint16_t> &ix, V3 c = {}, float half = 0.5f)
{
	pos.clear();
	for (int i = 0; i < 8; i++)
		pos.push_back(c + V3{(i & 1 ? half : -half), (i & 2 ? half : -half), (i & 4 ? half : -half)});
	ix = {0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4,
	      2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5};
}

// --- Plain glTF ---------------------------------------------------------------

static void test_write_and_read()
{
	GlbPart part;
	part.name = "box";
	std::vector<uint16_t> ix16;
	cube(part.pos, ix16, {0, 1, 0});
	part.index.assign(ix16.begin(), ix16.end());
	for (size_t i = 0; i < part.pos.size(); i++)
		part.uv.insert(part.uv.end(), {part.pos[i].x + 0.5f, part.pos[i].y});
	part.colour[0] = 0.5f;
	part.metallic = 0.25f;
	part.roughness = 0.75f;
	part.png = {0x89, 'P', 'N', 'G', 1, 2, 3};
	part.morph_names = {"jawOpen", "Blink"};
	part.morphs.push_back(std::vector<V3>(8, V3{0, -0.1f, 0}));
	part.morphs.push_back(std::vector<V3>(8, V3{0.2f, 0, 0}));
	part.t = {1, 0, 0};
	Model m;
	std::string why;
	CHECK(load(write_glb({part}), m, &why));
	CHECK(why.empty());
	CHECK(m.nodes.size() == 1 && m.meshes.size() == 1 && m.materials.size() == 1 && m.images.size() == 1);
	CHECK(m.points == 8 && m.triangles == 12);
	// Its size: moved 1 along x.
	CHECK(near3(m.low, {0.5f, 0.5f, -0.5f}, 1e-5f) && near3(m.high, {1.5f, 1.5f, 0.5f}, 1e-5f));
	const Primitive &p = m.meshes[0].parts[0];
	CHECK(p.uv.size() == 16 && p.normal.size() == 8);
	// Normals made from the triangles point out of the box.
	bool out = true;
	for (size_t i = 0; i < 8; i++)
		out = out && dot(p.normal[i], p.pos[i] - V3{0, 1, 0}) > 0;
	CHECK(out);
	CHECK(m.meshes[0].morph_names.size() == 2 && m.meshes[0].morph_names[0] == "jawOpen");
	CHECK(p.morphs.size() == 2 && near3(p.morphs[1].pos[3], {0.2f, 0, 0}, 1e-6f));
	CHECK(near(m.materials[0].base[0], 0.5, 1e-6) && near(m.materials[0].metallic, 0.25, 1e-6) &&
	      near(m.materials[0].roughness, 0.75, 1e-6));
	CHECK(m.materials[0].base_map.image == 0 && m.images[0].bytes.size() == 7 && m.images[0].mime == "image/png");
	CHECK(!m.is_vrm() && m.bones.empty() && m.springs.empty());
}

// Numbers stored the other ways: interleaved, bytes and shorts squeezed
// to 0..1, sparse, a strip and a fan, a node's matrix.
static void test_storage()
{
	Builder b;
	// Interleaved: x y z then u v, 20 bytes a point, 4 points.
	std::vector<float> inter = {0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 1, 0, 1, 1};
	const int v = b.view(inter.data(), inter.size() * 4, 20);
	const int pos = b.accessor(v, 5126, 4, "VEC3");
	const int uv = b.accessor(v, 5126, 4, "VEC2", 12);
	// Colours as normalized bytes.
	std::vector<uint8_t> col = {255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0, 255, 255, 255, 255};
	const int colour = b.accessor(b.view(col.data(), col.size()), 5121, 4, "VEC4", 0, true);
	// A strip: 0 1 2 3 -> (0 1 2) (2 1 3).
	std::vector<uint16_t> strip = {0, 1, 2, 3};
	const int ix = b.indices(strip);
	// Sparse: the points moved by an expression, only point 3.
	std::vector<uint16_t> sp_at = {3};
	std::vector<float> sp_val = {0, 0, 0.5f};
	json target = {{"bufferView", nullptr}};
	b.doc["accessors"].push_back({{"componentType", 5126},
				      {"count", 4},
				      {"type", "VEC3"},
				      {"sparse",
				       {{"count", 1},
					{"indices", {{"bufferView", b.view(sp_at.data(), 2)}, {"componentType", 5123}}},
					{"values", {{"bufferView", b.view(sp_val.data(), 12)}}}}}});
	const int morph = (int)b.doc["accessors"].size() - 1;
	b.doc["meshes"].push_back({{"primitives",
				    {{{"attributes", {{"POSITION", pos}, {"TEXCOORD_0", uv}, {"COLOR_0", colour}}},
				      {"indices", ix},
				      {"mode", 5},
				      {"targets", {{{"POSITION", morph}}}}}}}});
	// A fan of the same 4 points: (0 1 2) (0 2 3).
	std::vector<uint16_t> fan = {0, 1, 2, 3};
	b.doc["meshes"].push_back(
		{{"primitives", {{{"attributes", {{"POSITION", pos}}}, {"indices", b.indices(fan)}, {"mode", 6}}}}});
	// Moved 2 along y and turned a quarter about z, as a matrix.
	b.node({{"mesh", 0}, {"matrix", {0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 0, 2, 0, 1}}});
	b.node({{"mesh", 1}});
	Model m;
	std::string why;
	CHECK(load(b.glb(), m, &why));
	if (m.meshes.size() < 2)
		return;
	const Primitive &p = m.meshes[0].parts[0];
	CHECK(p.pos.size() == 4 && near3(p.pos[2], {0, 1, 0}, 1e-6f));
	CHECK(p.uv.size() == 8 && near(p.uv[2], 1, 1e-6) && near(p.uv[5], 1, 1e-6) && near(p.uv[6], 1, 1e-6));
	CHECK(p.colour.size() == 16 && near(p.colour[0], 1, 1e-6) && near(p.colour[7], 128 / 255.0, 1e-6));
	CHECK((p.index == std::vector<uint32_t>{0, 1, 2, 2, 1, 3}));
	CHECK(p.morphs.size() == 1 && near3(p.morphs[0].pos[3], {0, 0, 0.5f}, 1e-6f) &&
	      near3(p.morphs[0].pos[0], {}, 1e-9f));
	CHECK((m.meshes[1].parts[0].index == std::vector<uint32_t>{0, 1, 2, 0, 2, 3}));
	const Node &n = m.nodes[0];
	CHECK(near3(n.t, {0, 2, 0}, 1e-6f) && near3(n.s, {1, 1, 1}, 1e-5f));
	CHECK(near3(rotate(n.r, {1, 0, 0}), {0, 1, 0}, 1e-5f));
	// A quarter turn about z then up 2: point (1, 0, 0) lands at (0, 3, 0).
	CHECK(near(m.high.y, 3, 1e-5));
}

// Broken and hostile files fail with words, never a crash.
static void test_broken()
{
	Model m;
	std::string why;
	const std::vector<uint8_t> nothing;
	CHECK(!load(nothing, m, &why) && !why.empty());
	const std::string words = "hello there";
	CHECK(!load(std::vector<uint8_t>(words.begin(), words.end()), m, &why) && !why.empty());
	// A glTF 1.0 file.
	const std::string old = R"({"asset":{"version":"1.0"}})";
	CHECK(!load(std::vector<uint8_t>(old.begin(), old.end()), m, &why) && why.find("2.0") != std::string::npos);
	// Squeezed shapes.
	const std::string draco = R"({"asset":{"version":"2.0"},"extensionsRequired":["KHR_draco_mesh_compression"]})";
	CHECK(!load(std::vector<uint8_t>(draco.begin(), draco.end()), m, &why) &&
	      why.find("compression") != std::string::npos);
	// A file that needs the internet.
	const std::string web =
		R"({"asset":{"version":"2.0"},"buffers":[{"uri":"https://example.com/x.bin","byteLength":4}]})";
	CHECK(!load(std::vector<uint8_t>(web.begin(), web.end()), m, &why) &&
	      why.find("internet") != std::string::npos);

	auto base = [] {
		Builder b;
		std::vector<V3> pos;
		std::vector<uint16_t> ix;
		cube(pos, ix);
		b.doc["meshes"].push_back(
			{{"primitives", {{{"attributes", {{"POSITION", b.vec3s(pos)}}}, {"indices", b.indices(ix)}}}}});
		b.node({{"mesh", 0}});
		return b;
	};
	{
		Builder b = base();
		CHECK(load(b.glb(), m));
		b.doc["accessors"][0]["count"] = 9; // past its end
		CHECK(!load(b.glb(), m, &why) && !why.empty());
	}
	{
		Builder b = base();
		std::vector<uint16_t> bad = {0, 1, 99};
		b.doc["meshes"][0]["primitives"][0]["indices"] = b.indices(bad);
		CHECK(!load(b.glb(), m, &why) && why.find("triangle") != std::string::npos);
	}
	{
		Builder b = base();
		b.node({{"children", {2}}});
		b.node({{"children", {1}}});
		CHECK(!load(b.glb(), m, &why) && why.find("loop") != std::string::npos);
	}
	{
		Builder b = base();
		b.doc["bufferViews"][0]["byteOffset"] = 1u << 30;
		CHECK(!load(b.glb(), m, &why));
	}
	{
		Builder b = base();
		b.doc["accessors"][0]["componentType"] = "lots";
		CHECK(!load(b.glb(), m, &why));
	}
	// A whole .glb cut short anywhere, and with bytes changed at random:
	// a model or a reason, never a crash.
	GlbPart part;
	std::vector<uint16_t> ix16;
	cube(part.pos, ix16);
	part.index.assign(ix16.begin(), ix16.end());
	part.morph_names = {"a"};
	part.morphs.push_back(std::vector<V3>(8, V3{0, 0.1f, 0}));
	const std::vector<uint8_t> good = write_glb({part});
	int answered = 0;
	for (size_t cut = 0; cut < good.size(); cut += 7) {
		std::vector<uint8_t> shorter(good.begin(), good.begin() + (long)cut);
		Model mm;
		std::string w;
		const bool ok = load(shorter, mm, &w);
		answered += ok || !w.empty();
	}
	CHECK(answered == (int)((good.size() + 6) / 7));
	std::mt19937 rng(7);
	answered = 0;
	for (int round = 0; round < 3000; round++) {
		std::vector<uint8_t> bad = good;
		const int changes = 1 + (int)(rng() % 6);
		for (int c = 0; c < changes; c++)
			bad[rng() % bad.size()] = (uint8_t)rng();
		Model mm;
		std::string w;
		const bool ok = load(bad, mm, &w);
		answered += ok || !w.empty();
		if (ok) {
			// What loads poses without trouble.
			Rig rig(std::make_shared<Model>(mm), RigOptions());
			Drive d;
			d.dt = 1.0f / 30;
			rig.pose(d);
		}
	}
	CHECK(answered == 3000);
}

// --- VRM -----------------------------------------------------------------------

// A little VRM 1.0 person: hips, spine, neck, head and two eyes in a
// column, a box for a head skinned to the head bone (with expressions),
// and a ponytail of three joints swinging behind the head, a ball on the
// head for it to bump into.
struct Person {
	Builder b;
	int hips, neck, head, leye, reye, tail0, tail1, tail2, face_node;
};

static Person make_person(bool vrm0)
{
	Person p;
	Builder &b = p.b;
	p.hips = b.node({{"name", "hips"}, {"translation", {0, 1, 0}}});
	p.neck = b.node({{"name", "neck"}, {"translation", {0, 0.5f, 0}}});
	p.head = b.node({{"name", "head"}, {"translation", {0, 0.1f, 0}}});
	p.leye = b.node({{"name", "leftEye"}, {"translation", {0.03f, 0.1f, 0.08f}}});
	p.reye = b.node({{"name", "rightEye"}, {"translation", {-0.03f, 0.1f, 0.08f}}});
	// The ponytail sticks straight out behind (-z), level.
	p.tail0 = b.node({{"name", "tail0"}, {"translation", {0, 0.1f, -0.1f}}});
	p.tail1 = b.node({{"name", "tail1"}, {"translation", {0, 0, -0.1f}}});
	p.tail2 = b.node({{"name", "tail2"}, {"translation", {0, 0, -0.1f}}});
	b.doc["nodes"][p.hips]["children"] = {p.neck};
	b.doc["nodes"][p.neck]["children"] = {p.head};
	b.doc["nodes"][p.head]["children"] = {p.leye, p.reye, p.tail0};
	b.doc["nodes"][p.tail0]["children"] = {p.tail1};
	b.doc["nodes"][p.tail1]["children"] = {p.tail2};
	// The head box (world y 1.5 to 1.7), all on the head bone.
	std::vector<V3> pos;
	std::vector<uint16_t> ix;
	cube(pos, ix, {0, 1.7f, 0}, 0.1f);
	std::vector<uint8_t> joints(8 * 4, 0);
	std::vector<float> weights;
	for (int i = 0; i < 8; i++)
		weights.insert(weights.end(), {1, 0, 0, 0});
	std::vector<V3> open(8, V3{0, -0.05f, 0}), shut(8, V3{0.01f, 0, 0}), smile(8, V3{0, 0, 0.02f}),
		jaw(8, V3{0, 0, -0.03f});
	const int a_pos = b.vec3s(pos), a_ix = b.indices(ix);
	const int a_j = b.accessor(b.view(joints.data(), joints.size()), 5121, 8, "VEC4");
	const int a_w = b.floats(weights, 4, "VEC4");
	json targets = json::array();
	for (const auto *t : {&open, &shut, &smile, &jaw})
		targets.push_back({{"POSITION", b.vec3s(*t)}});
	b.doc["meshes"].push_back(
		{{"extras", {{"targetNames", {"Fcl_MTH_A", "Fcl_EYE_Close", "Fcl_ALL_Joy", "jawOpen"}}}},
		 {"primitives",
		  {{{"attributes", {{"POSITION", a_pos}, {"JOINTS_0", a_j}, {"WEIGHTS_0", a_w}}},
		    {"indices", a_ix},
		    {"targets", targets}}}}});
	// Its inverse bind: the head bone at rest is at (0, 1.6, 0).
	std::vector<float> ibm = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1.6f, 0, 1};
	b.doc["skins"].push_back({{"joints", {p.head}}, {"inverseBindMatrices", b.floats(ibm, 16, "MAT4")}});
	p.face_node = b.node({{"name", "face"}, {"mesh", 0}, {"skin", 0}});
	b.doc["scenes"] = json::array({{{"nodes", {p.hips, p.face_node}}}});
	if (!vrm0) {
		b.doc["extensions"]["VRMC_vrm"] = {
			{"specVersion", "1.0"},
			{"meta", {{"name", "Tester"}, {"authors", {"DireBot"}}, {"avatarPermission", "everyone"}}},
			{"humanoid",
			 {{"humanBones",
			   {{"hips", {{"node", p.hips}}},
			    {"neck", {{"node", p.neck}}},
			    {"head", {{"node", p.head}}},
			    {"leftEye", {{"node", p.leye}}},
			    {"rightEye", {{"node", p.reye}}}}}}},
			{"lookAt",
			 {{"type", "bone"},
			  {"rangeMapHorizontalOuter", {{"inputMaxValue", 90}, {"outputScale", 12}}},
			  {"rangeMapHorizontalInner", {{"inputMaxValue", 90}, {"outputScale", 8}}},
			  {"rangeMapVerticalUp", {{"inputMaxValue", 90}, {"outputScale", 10}}},
			  {"rangeMapVerticalDown", {{"inputMaxValue", 90}, {"outputScale", 10}}}}},
			{"expressions",
			 {{"preset",
			   {{"aa", {{"morphTargetBinds", {{{"node", p.face_node}, {"index", 0}, {"weight", 1.0}}}}}},
			    {"blink", {{"morphTargetBinds", {{{"node", p.face_node}, {"index", 1}, {"weight", 1.0}}}}}},
			    {"happy",
			     {{"morphTargetBinds", {{{"node", p.face_node}, {"index", 2}, {"weight", 0.5}}}},
			      {"overrideBlink", "block"}}}}},
			  {"custom", {{"wave", {{"morphTargetBinds", json::array()}}}}}}}};
		b.doc["extensions"]["VRMC_springBone"] = {
			{"specVersion", "1.0"},
			{"colliders",
			 {{{"node", p.head}, {"shape", {{"sphere", {{"offset", {0, 0, 0}}, {"radius", 0.1}}}}}}}},
			{"colliderGroups", {{{"colliders", {0}}}}},
			{"springs",
			 {{{"name", "ponytail"},
			   {"joints",
			    {{{"node", p.tail0},
			      {"stiffness", 0.0},
			      {"gravityPower", 1.0},
			      {"dragForce", 0.4},
			      {"hitRadius", 0.02}},
			     {{"node", p.tail1},
			      {"stiffness", 0.0},
			      {"gravityPower", 1.0},
			      {"dragForce", 0.4},
			      {"hitRadius", 0.02}},
			     {{"node", p.tail2}}}},
			   {"colliderGroups", {0}}}}}};
	} else {
		b.doc["extensions"]["VRM"] = {
			{"specVersion", "0.0"},
			{"meta", {{"title", "Tester 0"}, {"author", "DireBot"}, {"allowedUserName", "Everyone"}}},
			{"humanoid",
			 {{"humanBones",
			   {{{"bone", "hips"}, {"node", p.hips}},
			    {{"bone", "neck"}, {"node", p.neck}},
			    {{"bone", "head"}, {"node", p.head}},
			    {{"bone", "leftEye"}, {"node", p.leye}},
			    {{"bone", "rightEye"}, {"node", p.reye}}}}}},
			{"firstPerson",
			 {{"lookAtTypeName", "Bone"}, {"lookAtHorizontalOuter", {{"xRange", 90}, {"yRange", 12}}}}},
			{"blendShapeMaster",
			 {{"blendShapeGroups",
			   {{{"name", "A"},
			     {"presetName", "a"},
			     {"binds", {{{"mesh", 0}, {"index", 0}, {"weight", 100}}}}},
			    {{"name", "Blink_L"},
			     {"presetName", "blink_l"},
			     {"binds", {{{"mesh", 0}, {"index", 1}, {"weight", 50}}}}},
			    {{"name", "Blink_R"},
			     {"presetName", "blink_r"},
			     {"binds", {{{"mesh", 0}, {"index", 1}, {"weight", 50}}}}},
			    {{"name", "Joy"},
			     {"presetName", "joy"},
			     {"binds", {{{"mesh", 0}, {"index", 2}, {"weight", 100}}}}},
			    {{"name", "Mine"}, {"presetName", "unknown"}, {"binds", json::array()}}}}}},
			{"secondaryAnimation",
			 {{"boneGroups",
			   {{{"comment", "ponytail"},
			     {"stiffiness", 0.0},
			     {"gravityPower", 1.0},
			     {"gravityDir", {{"x", 0}, {"y", -1}, {"z", 0}}},
			     {"dragForce", 0.4},
			     {"hitRadius", 0.02},
			     {"bones", {p.tail0}},
			     {"colliderGroups", {0}}}}},
			  {"colliderGroups",
			   {{{"node", p.head},
			     {"colliders", {{{"offset", {{"x", 0}, {"y", 0}, {"z", 0.05}}}, {"radius", 0.1}}}}}}}}},
			{"materialProperties", json::array()}};
	}
	return p;
}

static void test_vrm1_reading()
{
	Person p = make_person(false);
	Model m;
	std::string why;
	CHECK(load(p.b.glb(), m, &why));
	CHECK(m.vrm == 1 && !m.faces_back && m.meta.title == "Tester" && m.meta.author == "DireBot");
	CHECK(m.bone("head") == p.head && m.bone("neck") == p.neck && m.bone("leftEye") == p.leye);
	CHECK(m.expressions.size() == 4);
	const Expression *happy = nullptr, *aa = nullptr;
	for (const Expression &e : m.expressions) {
		if (e.name == "happy")
			happy = &e;
		if (e.name == "aa")
			aa = &e;
	}
	CHECK(happy && happy->preset && happy->override_blink == 1 && happy->binds.size() == 1 &&
	      near(happy->binds[0].weight, 0.5, 1e-6));
	CHECK(aa && aa->binds.size() == 1 && aa->binds[0].mesh == 0 && aa->binds[0].morph == 0);
	CHECK(m.look_bones && near(m.look_out.out, 12, 1e-6) && near(m.look_in.out, 8, 1e-6));
	// The ponytail: two swinging joints (the last is only its tip).
	CHECK(m.springs.size() == 1 && m.springs[0].joints.size() == 2);
	CHECK(m.springs[0].joints[0].node == p.tail0 && m.springs[0].joints[0].tail_node == p.tail1);
	CHECK(m.springs[0].colliders.size() == 1 && m.colliders.size() == 1 && near(m.colliders[0].radius, 0.1, 1e-6));
}

static void test_vrm0_reading()
{
	Person p = make_person(true);
	Model m;
	std::string why;
	CHECK(load(p.b.glb(), m, &why));
	CHECK(m.vrm == -1 && m.faces_back && m.meta.title == "Tester 0");
	CHECK(m.bone("head") == p.head);
	std::vector<std::string> names;
	for (const Expression &e : m.expressions)
		names.push_back(e.name);
	CHECK((names == std::vector<std::string>{"aa", "blinkLeft", "blinkRight", "happy", "Mine"}));
	CHECK(near(m.expressions[1].binds[0].weight, 0.5, 1e-6));
	// Every bone from the root down swings: tail0 -> tail1 -> tail2, and
	// tail2 towards a made-up tip 7 cm on.
	CHECK(m.springs.size() == 1 && m.springs[0].joints.size() == 3);
	if (m.springs.size() == 1 && m.springs[0].joints.size() == 3) {
		const SpringJoint &last = m.springs[0].joints[2];
		CHECK(last.node == p.tail2 && last.tail_node < 0 && near3(last.tail, {0, 0, -0.07f}, 1e-5f));
	}
	// VRM 0.x's z is the other way round.
	CHECK(m.colliders.size() == 1 && near3(m.colliders[0].offset, {0, 0, -0.05f}, 1e-6f));
}

static Drive still()
{
	Drive d;
	d.dt = 1.0f / 60;
	return d;
}

static void test_head_and_eyes()
{
	Person p = make_person(false);
	auto m = std::make_shared<Model>();
	CHECK(load(p.b.glb(), *m));
	RigOptions o;
	o.springs = false;
	Rig rig(m, o);
	CHECK(!rig.turns_whole());
	Drive d = still();
	rig.pose(d);
	const V3 box_rest = rig.parts()[0].pos[7]; // (0.1, 1.8, 0.1)
	CHECK(near3(box_rest, {0.1f, 1.8f, 0.1f}, 1e-5f));
	// A quarter turn to its left: the head box turns about the head bone
	// (at 0, 1.6, 0), the neck taking 40% of it.
	d.head = axis_angle({0, 1, 0}, 3.14159265f / 2);
	rig.pose(d);
	const V3 turned = rig.parts()[0].pos[7];
	CHECK(near3(turned, {0.1f, 1.8f, -0.1f}, 1e-4f));
	const Quat neck = quat_of(rig.world()[p.neck]);
	CHECK(near3(rotate(neck, {0, 0, 1}), {std::sin(0.4f * 1.5708f), 0, std::cos(0.4f * 1.5708f)}, 1e-4f));
	// Eyes: looking out of its left (a full look) turns the left eye 12
	// degrees towards +x and the right 8 (it looks in).
	d.head = Quat();
	d.moves[move_of_name("eyeLookOutLeft")] = 1;
	d.moves[move_of_name("eyeLookInRight")] = 1;
	rig.pose(d);
	const V3 lf = rotate(quat_of(rig.world()[p.leye]), {0, 0, 1});
	const V3 rf = rotate(quat_of(rig.world()[p.reye]), {0, 0, 1});
	CHECK(near(std::atan2(lf.x, lf.z) * 180 / 3.14159265, 30.0 / 90 * 12, 0.05));
	CHECK(near(std::atan2(rf.x, rf.z) * 180 / 3.14159265, 30.0 / 90 * 8, 0.05));
	// Facing -z (VRM 0.x): the same turn of yours turns it the same way
	// as it's seen.
	RigOptions back = o;
	back.facing = axis_angle({0, 1, 0}, 3.14159265f);
	Rig rig0(m, back);
	Drive d0 = still();
	d0.head = axis_angle({0, 1, 0}, 0.5f);
	rig0.pose(d0);
	const V3 f0 = rotate(quat_of(rig0.world()[p.head]), {0, 0, -1}); // its front
	const V3 seen = rotate(back.facing, f0);
	CHECK(near3(seen, {std::sin(0.5f), 0, std::cos(0.5f)}, 1e-4f));
}

static void test_expressions()
{
	Person p = make_person(false);
	auto m = std::make_shared<Model>();
	CHECK(load(p.b.glb(), *m));
	RigOptions o;
	o.springs = false;
	Rig rig(m, o);
	// The mesh has one ARKit-named morph (jawOpen): driven as it is, and
	// not enough of them to be "perfect sync".
	CHECK(rig.arkit_parts() == 1);
	Drive d = still();
	d.moves[move_of_name("jawOpen")] = 0.8f;
	rig.pose(d);
	const std::vector<float> &w = rig.weights()[0];
	CHECK(near(w[3], 0.8, 1e-5));                            // jawOpen itself
	CHECK(near(w[0], (0.8 - 0.05) / 0.5 > 1 ? 1 : 0, 1e-5)); // aa, all the way
	CHECK(near(rig.expression_values().at("aa"), 1, 1e-5));
	// The box's points moved by them.
	CHECK(near(rig.parts()[0].pos[0].y, 1.6f - 0.05f, 1e-4));
	// Blinks: both eyes shut -> blink.
	d = still();
	d.moves[move_of_name("eyeBlinkLeft")] = 1;
	d.moves[move_of_name("eyeBlinkRight")] = 1;
	rig.pose(d);
	CHECK(near(rig.weights()[0][1], 1, 1e-5));
	// Happy blocks blinking.
	d.moves[move_of_name("mouthSmileLeft")] = 1;
	d.moves[move_of_name("mouthSmileRight")] = 1;
	rig.pose(d);
	CHECK(near(rig.expression_values().at("happy"), 1, 1e-5));
	CHECK(near(rig.weights()[0][2], 0.5, 1e-5)); // happy at its bind's 0.5
	CHECK(near(rig.weights()[0][1], 0, 1e-5));   // no blink
	// Expressions off: nothing moves.
	d.expressions = false;
	rig.pose(d);
	CHECK(near(rig.weights()[0][2], 0, 1e-9) && near(rig.weights()[0][3], 0, 1e-9));

	// VRM 0.x: a blink for each eye, so a wink is one of them and a blink
	// both (each bound at half to the same morph).
	Person p0 = make_person(true);
	auto m0 = std::make_shared<Model>();
	CHECK(load(p0.b.glb(), *m0));
	Rig rig0(m0, o);
	Drive d0 = still();
	d0.moves[move_of_name("eyeBlinkLeft")] = 1;
	rig0.pose(d0);
	CHECK(near(rig0.expression_values().at("blinkLeft"), 1, 1e-5) &&
	      near(rig0.expression_values().at("blinkRight"), 0, 1e-5));
	CHECK(near(rig0.weights()[0][1], 0.5, 1e-5));
}

static void test_names_and_sides()
{
	CHECK(arkit_move("jawOpen") == move_of_name("jawOpen"));
	CHECK(arkit_move("blendShape1.JawOpen") == move_of_name("jawOpen"));
	CHECK(arkit_move("eyeBlink_L") == move_of_name("eyeBlinkLeft"));
	CHECK(arkit_move("EyeBlinkRight") == move_of_name("eyeBlinkRight"));
	CHECK(arkit_move("mouth_smile_r") == move_of_name("mouthSmileRight"));
	CHECK(arkit_move("Fcl_MTH_A") < 0 && arkit_move("") < 0);
	float mv[EXPRESSIONS] = {};
	mv[move_of_name("eyeBlinkLeft")] = 1;
	mv[move_of_name("mouthLeft")] = 0.5f;
	mv[move_of_name("jawOpen")] = 0.25f;
	swap_sides(mv);
	CHECK(mv[move_of_name("eyeBlinkRight")] == 1 && mv[move_of_name("eyeBlinkLeft")] == 0);
	CHECK(mv[move_of_name("mouthRight")] == 0.5f && mv[move_of_name("mouthLeft")] == 0);
	CHECK(mv[move_of_name("jawOpen")] == 0.25f);

	// A plain model with the usual names (no VRM): "Blink" and "A" follow
	// your blink and open mouth; "Smile" your smile.
	GlbPart part;
	std::vector<uint16_t> ix16;
	cube(part.pos, ix16);
	part.index.assign(ix16.begin(), ix16.end());
	part.morph_names = {"Blink", "A", "Smile", "Other"};
	for (int i = 0; i < 4; i++)
		part.morphs.push_back(std::vector<V3>(8, V3{0, 0.1f, 0}));
	auto m = std::make_shared<Model>();
	CHECK(load(write_glb({part}), *m));
	Rig rig(m, RigOptions());
	Drive d = still();
	d.moves[move_of_name("eyeBlinkLeft")] = 1;
	d.moves[move_of_name("eyeBlinkRight")] = 1;
	d.moves[move_of_name("jawOpen")] = 0.3f;
	rig.pose(d);
	CHECK(near(rig.weights()[0][0], 1, 1e-5));
	CHECK(near(rig.weights()[0][1], 0.5, 1e-5));
	CHECK(near(rig.weights()[0][2], 0, 1e-5) && near(rig.weights()[0][3], 0, 1e-5));

	// Made for ARKit: 52 morphs named as ARKit's, driven straight.
	GlbPart sync = part;
	sync.morph_names.clear();
	sync.morphs.clear();
	for (int i = 1; i < EXPRESSIONS; i++) {
		sync.morph_names.push_back(EXPRESSION_NAMES[i]);
		sync.morphs.push_back(std::vector<V3>(8, V3{0, 0.01f, 0}));
	}
	auto ms = std::make_shared<Model>();
	CHECK(load(write_glb({sync}), *ms));
	Rig rs(ms, RigOptions());
	CHECK(rs.arkit_parts() == EXPRESSIONS - 1);
	Drive ds = still();
	for (int i = 1; i < EXPRESSIONS; i++)
		ds.moves[i] = (float)i / EXPRESSIONS;
	rs.pose(ds);
	bool same = true;
	for (int i = 1; i < EXPRESSIONS; i++)
		same = same && near(rs.weights()[0][i - 1], (float)i / EXPRESSIONS, 1e-5);
	CHECK(same);
}

static void test_springs()
{
	Person p = make_person(false);
	auto m = std::make_shared<Model>();
	CHECK(load(p.b.glb(), *m));
	Rig rig(m, RigOptions());
	CHECK(rig.springs() == 1 && !rig.auto_chain());
	Drive d = still();
	d.dt = 0;
	rig.pose(d);
	d.dt = 1.0f / 60;
	const V3 tip0 = move_of(rig.world()[p.tail2]);
	CHECK(near3(tip0, {0, 1.7f, -0.3f}, 1e-4f)); // straight out behind, at first
	// No stiffness, just gravity: after two seconds the ponytail hangs
	// down, each part still as long as it was, kept out of the head's ball.
	for (int i = 0; i < 120; i++)
		rig.pose(d);
	const V3 root = move_of(rig.world()[p.tail0]), mid = move_of(rig.world()[p.tail1]),
		 tip = move_of(rig.world()[p.tail2]);
	CHECK(tip.y < 1.6f);
	CHECK(near(length(mid - root), 0.1, 1e-3) && near(length(tip - mid), 0.1, 1e-3));
	const V3 ball = move_of(rig.world()[p.head]);
	CHECK(length(mid - ball) >= 0.1f + 0.02f - 1e-3f && length(tip - ball) >= 0.1f + 0.02f - 1e-3f);
	// Swinging: the whole person thrown sideways, the ponytail lags behind.
	Drive moved_d = d;
	moved_d.place = moved({0.05f, 0, 0});
	rig.pose(moved_d);
	const V3 after = move_of(rig.world()[p.tail2]);
	CHECK(after.x - 0.05f < tip.x - 1e-4f);
	// Twice the size: lengths twice as long.
	Drive big = d;
	big.place = scaled(2);
	rig.settle();
	for (int i = 0; i < 60; i++)
		rig.pose(big);
	CHECK(near(length(move_of(rig.world()[p.tail1]) - move_of(rig.world()[p.tail0])), 0.2, 2e-3));
	// Any frame rate: 30 frames a second does two steps a frame.
	Rig r30(m, RigOptions()), r60(m, RigOptions());
	Drive d30 = still(), d60 = still();
	d30.dt = 1.0f / 30;
	for (int i = 0; i < 60; i++) {
		r30.pose(d30);
		r60.pose(d60);
		r60.pose(d60);
	}
	CHECK(near3(move_of(r30.world()[p.tail2]), move_of(r60.world()[p.tail2]), 1e-4f));
	// Off: it stays as it is.
	RigOptions off;
	off.springs = false;
	Rig still_rig(m, off);
	for (int i = 0; i < 30; i++)
		still_rig.pose(d);
	CHECK(near3(move_of(still_rig.world()[p.tail2]), {0, 1.7f, -0.3f}, 1e-4f));
}

// A chain of links: one skinned line of 4 bones hanging down from
// the first, no VRM - it swings on its own.
static void test_link_chain()
{
	Builder b;
	const int armature = b.node({{"name", "Armature"}});
	const int b0 = b.node({{"name", "Bone"}, {"translation", {0, 1, 0}}, {"rotation", {1, 0, 0, 0}}});
	const int b1 = b.node({{"name", "Bone.001"}, {"translation", {0, 0.3f, 0}}});
	const int b2 = b.node({{"name", "Bone.002"}, {"translation", {0, 0.3f, 0}}});
	const int b3 = b.node({{"name", "Bone.003"}, {"translation", {0, 0.3f, 0}}});
	b.doc["nodes"][armature]["children"] = {b0};
	b.doc["nodes"][b0]["children"] = {b1};
	b.doc["nodes"][b1]["children"] = {b2};
	b.doc["nodes"][b2]["children"] = {b3};
	// A thin column from y 1 down to 0, each point on the bone it's beside.
	std::vector<V3> pos;
	std::vector<uint8_t> joints;
	std::vector<float> weights;
	std::vector<uint16_t> ix;
	for (int i = 0; i <= 12; i++) {
		const float y = 1 - i / 12.0f;
		const int bone = std::min(3, (int)((1 - y) / 0.3f));
		for (float x : {-0.05f, 0.05f}) {
			pos.push_back({x, y, 0});
			joints.insert(joints.end(), {(uint8_t)bone, 0, 0, 0});
			weights.insert(weights.end(), {1, 0, 0, 0});
		}
		if (i)
			ix.insert(ix.end(), {(uint16_t)(2 * i - 2), (uint16_t)(2 * i - 1), (uint16_t)(2 * i),
					     (uint16_t)(2 * i - 1), (uint16_t)(2 * i + 1), (uint16_t)(2 * i)});
	}
	b.doc["meshes"].push_back(
		{{"primitives",
		  {{{"attributes",
		     {{"POSITION", b.vec3s(pos)},
		      {"JOINTS_0", b.accessor(b.view(joints.data(), joints.size()), 5121, pos.size(), "VEC4")},
		      {"WEIGHTS_0", b.floats(weights, 4, "VEC4")}}},
		    {"indices", b.indices(ix)}}}}});
	// Each bone's inverse bind: it points down (turned half about x) from
	// y 1, 0.7, 0.4, 0.1.
	std::vector<float> ibm;
	for (int i = 0; i < 4; i++) {
		const float y = 1 - 0.3f * i;
		const M4 inv = inverse(moved({0, y, 0}) * matrix_of(Quat{1, 0, 0, 0}));
		ibm.insert(ibm.end(), inv.m, inv.m + 16);
	}
	b.doc["skins"].push_back({{"joints", {b0, b1, b2, b3}}, {"inverseBindMatrices", b.floats(ibm, 16, "MAT4")}});
	const int body = b.node({{"name", "Links"}, {"mesh", 0}, {"skin", 0}});
	b.doc["scenes"] = json::array({{{"nodes", {armature, body}}}});
	auto m = std::make_shared<Model>();
	std::string why;
	CHECK(load(b.glb(), *m, &why));
	CHECK((hanging_chains(*m, m->skins[0]) == std::vector<std::vector<int>>{{b0, b1, b2, b3}}));
	CHECK(!person_skeleton(*m, m->skins[0]));
	Rig rig(m, RigOptions());
	CHECK(rig.auto_chain() && rig.springs() == 1 && rig.turns_whole());
	Drive d = still();
	rig.pose(d);
	CHECK(near(rig.parts()[0].pos[24].y, 0, 1e-4)); // the bottom, at rest
	// Hanging still, it stays put (it hangs straight down already).
	for (int i = 0; i < 60; i++)
		rig.pose(d);
	CHECK(near(rig.parts()[0].pos[24].x, -0.05f, 2e-3) && near(rig.parts()[0].pos[24].y, 0, 2e-3));
	// Thrown to the side, the bottom lags behind the top, then swings
	// back and settles.
	d.place = moved({0.2f, 0, 0});
	rig.pose(d);
	rig.pose(d);
	const V3 top = rig.parts()[0].pos[0], bottom = rig.parts()[0].pos[24];
	CHECK(near(top.x, 0.15f, 1e-4)); // the top: where it was put
	CHECK(bottom.x < 0.15f - 0.01f);
	float most = 0;
	for (int i = 0; i < 600; i++) {
		rig.pose(d);
		most = std::max(most, std::fabs(rig.parts()[0].pos[24].x - 0.15f));
	}
	CHECK(most > 0.02f);
	CHECK(near(rig.parts()[0].pos[24].x, 0.15f, 0.01));
	// Its first bone never swings.
	CHECK(near3(rotate(quat_of(rig.world()[b0]), {0, 1, 0}), {0, -1, 0}, 1e-5f));
	// Turning your head turns it all round its middle.
	Drive t = still();
	t.head = axis_angle({0, 0, 1}, 0.3f);
	Rig turning(m, RigOptions());
	turning.pose(t);
	const V3 mid = turning.pivot();
	const V3 top_now = turning.parts()[0].pos[0] + V3{0.05f, 0, 0};
	const V3 top_want = mid + rotate(t.head, V3{0, 1, 0} - mid);
	CHECK(near3(top_now, top_want, 0.02f));
}

// A skeleton from bones named and placed (parent first), one point at the
// end of each bone (on it), all in one skin.
struct Bones {
	Builder b;
	std::vector<int> nodes;
	std::vector<V3> at; // rest, in the model
	int add(const char *name, int parent, V3 t)
	{
		const int n = b.node({{"name", name}, {"translation", {t.x, t.y, t.z}}});
		if (parent >= 0)
			b.doc["nodes"][nodes[parent]]["children"].push_back(n);
		at.push_back(parent >= 0 ? at[parent] + t : t);
		nodes.push_back(n);
		return (int)nodes.size() - 1;
	}
	std::shared_ptr<Model> load_it()
	{
		std::vector<V3> pos;
		std::vector<uint8_t> joints;
		std::vector<float> weights, ibm;
		std::vector<uint16_t> ix;
		for (size_t i = 0; i < nodes.size(); i++) {
			pos.push_back(at[i]);
			joints.insert(joints.end(), {(uint8_t)i, 0, 0, 0});
			weights.insert(weights.end(), {1, 0, 0, 0});
			const M4 inv = inverse(moved(at[i]));
			ibm.insert(ibm.end(), inv.m, inv.m + 16);
		}
		for (size_t i = 0; i + 2 < nodes.size(); i++)
			ix.insert(ix.end(), {(uint16_t)i, (uint16_t)(i + 1), (uint16_t)(i + 2)});
		b.doc["meshes"].push_back(
			{{"primitives",
			  {{{"attributes",
			     {{"POSITION", b.vec3s(pos)},
			      {"JOINTS_0", b.accessor(b.view(joints.data(), joints.size()), 5121, pos.size(), "VEC4")},
			      {"WEIGHTS_0", b.floats(weights, 4, "VEC4")}}},
			    {"indices", b.indices(ix)}}}}});
		json js = json::array();
		for (int n : nodes)
			js.push_back(n);
		b.doc["skins"].push_back({{"joints", js}, {"inverseBindMatrices", b.floats(ibm, 16, "MAT4")}});
		const int body = b.node({{"name", "Body"}, {"mesh", 0}, {"skin", 0}});
		b.doc["scenes"] = json::array({{{"nodes", {nodes[0], body}}}});
		auto m = std::make_shared<Model>();
		std::string why;
		CHECK(load(b.glb(), *m, &why));
		return m;
	}
};

static void test_dangly_parts()
{
	// A doll: its root, a body bone, an ear sticking up and a leg hanging
	// down, each three bones.
	Bones d;
	const int root = d.add("Bone", -1, {0, 0, 0});
	d.add("Bone.001", root, {0, -0.1f, 0});
	const int e0 = d.add("Bone.002", root, {0.1f, 0.1f, 0});
	const int e1 = d.add("Bone.003", e0, {0, 0.15f, 0});
	const int e2 = d.add("Bone.004", e1, {0.03f, 0.15f, 0});
	const int l0 = d.add("Bone.005", root, {0.05f, -0.2f, 0});
	const int l1 = d.add("Bone.006", l0, {0, -0.12f, 0});
	const int l2 = d.add("Bone.007", l1, {0, -0.12f, 0});
	auto m = d.load_it();
	const std::vector<std::vector<int>> chains = hanging_chains(*m, m->skins[0]);
	CHECK(chains.size() == 2);
	CHECK((chains[0] == std::vector<int>{d.nodes[e0], d.nodes[e1], d.nodes[e2]}));
	CHECK((chains[1] == std::vector<int>{d.nodes[l0], d.nodes[l1], d.nodes[l2]}));
	CHECK(!person_skeleton(*m, m->skins[0]));
	Rig rig(m, RigOptions());
	CHECK(rig.auto_chain() && rig.springs() == 2);
	// Points: in the order the bones were added.
	auto pt = [&](const Rig &r, int i) {
		return r.parts()[0].pos[i];
	};
	// Standing still, both keep their shape: the ear stays up.
	Drive s = still();
	for (int i = 0; i < 120; i++)
		rig.pose(s);
	CHECK(near3(pt(rig, e2), d.at[e2], 1e-3f));
	CHECK(near3(pt(rig, l2), d.at[l2], 1e-3f));
	// Tipped over to the side (as your head tilts), the leg swings back
	// towards hanging down; the ear tips over with the head but stays up.
	Drive t = still();
	const Quat tip = axis_angle({0, 0, 1}, 0.7f);
	t.place = matrix_of(tip);
	for (int i = 0; i < 600; i++)
		rig.pose(t);
	auto angle = [](V3 a, V3 b) {
		return std::acos(std::max(-1.0f, std::min(1.0f, dot(normalized(a), normalized(b)))));
	};
	const V3 leg_now = pt(rig, l2) - pt(rig, l0), leg_tipped = rotate(tip, d.at[l2] - d.at[l0]);
	CHECK(angle(leg_now, {0, -1, 0}) < angle(leg_tipped, {0, -1, 0}) - 0.2f);
	const V3 ear_now = pt(rig, e2) - pt(rig, e0), ear_tipped = rotate(tip, d.at[e2] - d.at[e0]);
	CHECK(angle(ear_now, ear_tipped) < 0.35f);
	CHECK(dot(ear_now, {0, 1, 0}) > 0);
	// Its body and the first bone of each never swing.
	CHECK(near3(pt(rig, e0), rotate(tip, d.at[e0]), 1e-4f));
	CHECK(near3(pt(rig, l0), rotate(tip, d.at[l0]), 1e-4f));

	// A person's skeleton: arms, legs and fingers don't dangle; hair does.
	Bones p;
	const int hips = p.add("Hips", -1, {0, 1, 0});
	const int spine = p.add("Spine", hips, {0, 0.2f, 0});
	const int head = p.add("Head", spine, {0, 0.4f, 0});
	const int h0 = p.add("Hair1", head, {0, 0.1f, -0.1f});
	p.add("Hair2", h0, {0, -0.1f, -0.05f});
	p.add("HeadTop_End", head, {0, 0.2f, 0});
	const int arm = p.add("LeftArm", spine, {0.2f, 0.3f, 0});
	const int fore = p.add("LeftForeArm", arm, {0.25f, 0, 0});
	const int hand = p.add("LeftHand", fore, {0.25f, 0, 0});
	const int f1 = p.add("LeftHandIndex1", hand, {0.05f, 0, 0});
	p.add("LeftHandIndex2", f1, {0.03f, 0, 0});
	const int t1 = p.add("LeftHandThumb1", hand, {0.03f, 0, 0.03f});
	p.add("LeftHandThumb2", t1, {0.03f, 0, 0});
	const int up = p.add("LeftUpLeg", hips, {0.1f, 0, 0});
	const int leg = p.add("LeftLeg", up, {0, -0.45f, 0});
	p.add("LeftFoot", leg, {0, -0.45f, 0});
	auto person = p.load_it();
	CHECK(person_skeleton(*person, person->skins[0]));
	Rig pr(person, RigOptions());
	CHECK(pr.springs() == 1);
}

static void test_facing_arms_and_wearing()
{
	// Which way a character faces: glTF's way (+z), unless it's much
	// deeper than it's wide (made facing +x, as the AR VTuber ones are).
	Model flat, deep, round;
	flat.low = {-0.3f, 0, -0.1f};
	flat.high = {0.3f, 1, 0.1f};
	deep.low = {-0.1f, 0, -0.3f};
	deep.high = {0.1f, 1, 0.3f};
	round.low = {-0.1f, 0, -0.11f};
	round.high = {0.1f, 1, 0.11f};
	CHECK(guess_turn(flat) == 0 && guess_turn(deep) == 270 && guess_turn(round) == 0);
	// That turn brings its +x round to face the camera (+z).
	CHECK(near3(rotate(axis_angle({0, 1, 0}, 270 * 3.14159265f / 180), {1, 0, 0}), {0, 0, 1}, 1e-5f));
	deep.vrm = 1;
	CHECK(guess_turn(deep) == 0);
	deep.vrm = -1;
	CHECK(guess_turn(deep) == 180);

	// A person made standing in a T: its arms come down by its sides.
	Person p = make_person(false);
	Builder &b = p.b;
	const int lu = b.node({{"name", "upper_arm.L"}, {"translation", {0.1f, 0.4f, 0}}});
	const int ll = b.node({{"name", "lower_arm.L"}, {"translation", {0.25f, 0, 0}}});
	const int ru = b.node({{"name", "upper_arm.R"}, {"translation", {-0.1f, 0.4f, 0}}});
	const int rl = b.node({{"name", "lower_arm.R"}, {"translation", {-0.25f, 0, 0}}});
	b.doc["nodes"][p.hips]["children"].push_back(lu);
	b.doc["nodes"][p.hips]["children"].push_back(ru);
	b.doc["nodes"][lu]["children"] = {ll};
	b.doc["nodes"][ru]["children"] = {rl};
	json &bones = b.doc["extensions"]["VRMC_vrm"]["humanoid"]["humanBones"];
	bones["leftUpperArm"] = {{"node", lu}};
	bones["leftLowerArm"] = {{"node", ll}};
	bones["rightUpperArm"] = {{"node", ru}};
	bones["rightLowerArm"] = {{"node", rl}};
	auto m = std::make_shared<Model>();
	CHECK(load(b.glb(), *m));
	RigOptions o;
	o.springs = false;
	Rig rig(m, o);
	rig.pose(still());
	for (int side = 0; side < 2; side++) {
		const V3 shoulder = move_of(rig.world()[side ? ru : lu]), elbow = move_of(rig.world()[side ? rl : ll]);
		const V3 arm = elbow - shoulder;
		CHECK(near(std::acos(dot(normalized(arm), {0, -1, 0})), 0.3, 1e-3));
		CHECK(side ? arm.x < 0 : arm.x > 0); // still out to its own side
		CHECK(near(length(arm), 0.25, 1e-4));
	}
	o.arms_down = false;
	Rig t_pose(m, o);
	t_pose.pose(still());
	CHECK(near3(move_of(t_pose.world()[ll]) - move_of(t_pose.world()[lu]), {0.25f, 0, 0}, 1e-5f));

	// Worn: a VRM at its own size, its eyes where yours are...
	const Framing f = frame_worn(*m, Quat{}, WornPlace(), M4(), 16 / 9.0f);
	CHECK(near3(point(f.place, {0, 1.7f, 0.08f}), {0, 2.7f, 2.0f}, 1e-3f));
	CHECK(near(f.world_scale, 100, 1e-3));
	// ...a big one round your head; a thin one out in front of your face.
	Model big, thin;
	big.low = {-0.5f, -0.5f, -0.5f};
	big.high = {0.5f, 0.5f, 0.5f};
	thin.low = {-0.02f, 0, -0.02f};
	thin.high = {0.02f, 1, 0.02f};
	const Framing fb = frame_worn(big, Quat{}, WornPlace(), M4(), 1);
	const Framing ft = frame_worn(thin, Quat{}, WornPlace(), M4(), 1);
	CHECK(near3(point(fb.place, {0, 0, 0}), {0, 1, 1}, 1e-4f));
	CHECK(near3(point(ft.place, {0, 0.5f, 0}), {0, 1, 8.02f}, 1e-3f));
	CHECK(near(fb.height, 24, 1e-3) && near(ft.height, 24, 1e-3));
}

static void test_set_up()
{
	// Reading it: what can't be used is left out.
	CHECK(setup_from("").empty() && setup_from("not json").empty() && setup_from("[1, 2]").empty());
	const CharacterSetup su = setup_from(R"({
		"facing": [0, 2, 0, 2], "pivot": [0, -0.1, 0],
		"spots": {"left_eye": {"at": [0.1, 0.2, 0.3], "w": 0.05, "h": 0.03},
			  "right_eye": {"x": 0.4, "y": 0.3, "w": 0.1, "h": 0.06},
			  "mouth": {"at": [0, 0, 0], "w": -1, "h": 1}},
		"chains": [{"bones": ["A", "B", "C"], "pull": [1, -1, 0], "with_head": false},
			   {"bones": ["Only one"]}, "junk"],
		"upright": [{"bone": "Body", "follow": 4.5}, {"follow": 2}]})");
	CHECK(!su.empty() && su.has_facing && near(su.facing.y, 0.70710678, 1e-5) &&
	      near(su.facing.w, 0.70710678, 1e-5));
	CHECK(su.has_pivot && near3(su.pivot, {0, -0.1f, 0}, 1e-6f));
	CHECK(su.spots[0].given && su.spots[0].has_at && near3(su.spots[0].at, {0.1f, 0.2f, 0.3f}, 1e-6f));
	CHECK(su.spots[1].given && !su.spots[1].has_at && near(su.spots[1].x, 0.4, 1e-6) &&
	      near(su.spots[1].w, 0.1, 1e-6));
	CHECK(!su.spots[2].given);
	CHECK(su.chains_given && su.chains.size() == 1 && su.chains[0].bones.size() == 3 && !su.chains[0].with_head &&
	      near3(su.chains[0].pull, {1, -1, 0}, 1e-6f));
	CHECK(su.upright.size() == 1 && su.upright[0].bone == "Body" && near(su.upright[0].follow, 4.5, 1e-6));
	CHECK(setup_from(R"({"chains": []})").chains_given);

	// A doll as an imported Lens has it: its ear a chain pulled out to
	// its side (turning with its head), its leg pulled straight down the
	// picture, its body standing upright and trailing after its head.
	Bones d;
	const int root = d.add("Bone", -1, {0, 0, 0});
	const int body = d.add("Bone.001", root, {0, -0.1f, 0});
	const int e0 = d.add("Bone.002", root, {0.1f, 0.1f, 0});
	const int e1 = d.add("Bone.003", e0, {0, 0.15f, 0});
	const int e2 = d.add("Bone.004", e1, {0.03f, 0.15f, 0});
	const int l0 = d.add("Bone.005", body, {0.05f, -0.2f, 0});
	const int l1 = d.add("Bone.006", l0, {0, -0.12f, 0});
	const int l2 = d.add("Bone.007", l1, {0, -0.12f, 0});
	auto m = d.load_it();
	auto setup = std::make_shared<CharacterSetup>(setup_from(R"({
		"chains": [{"bones": ["Bone.005", "Bone.006", "Bone.007"], "pull": [0, -20, 0], "with_head": false},
			   {"bones": ["Bone.002", "Bone.003", "Bone.004"], "pull": [20, 0, 0]},
			   {"bones": ["Bone.002", "No such bone"]}],
		"upright": [{"bone": "Bone.001", "follow": 5}]})"));
	RigOptions o;
	o.setup = setup;
	Rig rig(m, o);
	// Only the chains it names swing, the one hanging off another after it.
	CHECK(!rig.auto_chain() && rig.springs() == 2 && rig.upright_parts() == 1);
	auto pt = [&](const Rig &r, int i) {
		return r.parts()[0].pos[i];
	};
	auto way = [](V3 a, V3 b) {
		return normalized(b - a);
	};
	Drive s = still();
	for (int i = 0; i < 240; i++)
		rig.pose(s);
	// Pulled as it says - not the shape it was made: the ear lies out to
	// its side, the leg hangs straight down.
	CHECK(dot(way(pt(rig, e0), pt(rig, e2)), {1, 0, 0}) > 0.95f);
	CHECK(dot(way(pt(rig, l0), pt(rig, l2)), {0, -1, 0}) > 0.99f);
	CHECK(near(length(pt(rig, e2) - pt(rig, e0)), length(d.at[e2] - d.at[e0]), 0.03));
	// The head turned a quarter (your head tipped right over): the ear's
	// pull turns with it; the leg still hangs straight down the picture,
	// and the body stays standing as it was made.
	Drive t = still();
	const Quat tip = axis_angle({0, 0, 1}, 1.2f);
	t.head = tip;
	rig.set_pivot({0, 0, 0});
	for (int i = 0; i < 600; i++)
		rig.pose(t);
	CHECK(dot(way(pt(rig, e0), pt(rig, e2)), rotate(tip, {1, 0, 0})) > 0.95f);
	CHECK(dot(way(pt(rig, l0), pt(rig, l2)), {0, -1, 0}) > 0.99f);
	CHECK(near3(pt(rig, body), d.at[body] + (rotate(tip, d.at[body]) - d.at[body]), 1e-3f));
	CHECK(near3(pt(rig, l0) - pt(rig, body), d.at[l0] - d.at[body], 1e-3f));
	// Moved suddenly (but not far enough to start again): the head jumps,
	// the body trails after it and catches up.
	for (int i = 0; i < 240; i++)
		rig.pose(s);
	Drive mv = still();
	mv.place = moved({0.05f, 0, 0});
	rig.pose(mv);
	const float behind = pt(rig, root).x - pt(rig, body).x;
	CHECK(behind > 0.03f);
	for (int i = 0; i < 120; i++)
		rig.pose(mv);
	CHECK(std::fabs(pt(rig, root).x - pt(rig, body).x) < 0.002f);
	// Upright: as `upright` puts it, not as the head is turned.
	Drive u = mv;
	u.head = tip;
	u.upright = moved({0.05f, 0, 0});
	u.has_upright = true;
	for (int i = 0; i < 120; i++)
		rig.pose(u);
	CHECK(near3(pt(rig, l0) - pt(rig, body), d.at[l0] - d.at[body], 1e-3f));

	// Where your eyes and mouth go: a face with ARKit's expressions on it.
	GlbPart face;
	face.name = "face";
	const int N = 8;
	for (int y = 0; y <= N; y++)
		for (int x = 0; x <= N; x++)
			face.pos.push_back({-0.5f + (float)x / N, -0.5f + (float)y / N, 0.5f});
	for (int y = 0; y < N; y++)
		for (int x = 0; x < N; x++) {
			const uint32_t a = y * (N + 1) + x;
			face.index.insert(face.index.end(), {a, a + 1, a + N + 1, a + 1, a + N + 2, a + N + 1});
		}
	auto at_point = [&](float x, float y) {
		return (uint32_t)(std::lround((y + 0.5f) * N) * (N + 1) + std::lround((x + 0.5f) * N));
	};
	auto moving = [&](std::initializer_list<std::pair<float, float>> where, V3 by) {
		std::vector<V3> mv(face.pos.size());
		for (auto w : where)
			mv[at_point(w.first, w.second)] = by;
		return mv;
	};
	face.morph_names = {"eyeBlinkLeft", "eyeBlinkRight", "jawOpen", "mouthFunnel", "browInnerUp"};
	face.morphs.push_back(moving({{0.25f, 0.25f}, {0.375f, 0.25f}}, {0, -0.05f, 0}));
	face.morphs.push_back(moving({{-0.25f, 0.25f}, {-0.375f, 0.25f}}, {0, -0.05f, 0}));
	face.morphs.push_back(moving({{0, -0.5f}}, {0, -0.1f, 0}));
	face.morphs.push_back(moving({{-0.125f, -0.25f}, {0.125f, -0.25f}}, {0, 0, 0.05f}));
	face.morphs.push_back(moving({{0, 0.5f}}, {0, 0.05f, 0}));
	auto fm = std::make_shared<Model>();
	CHECK(load(write_glb({face}), *fm));
	FaceSpots fs = face_spots(fm, Quat(), CharacterSetup());
	// Its own left eye is on the viewer's right (+x); the mouth where
	// "mouthFunnel" moves it (surer than the jaw).
	CHECK(std::strcmp(fs.from[0], "expressions") == 0 && std::strcmp(fs.from[2], "expressions") == 0);
	CHECK(near(fs.spot[0].at.x, 0.3125, 1e-3) && near(fs.spot[0].at.y, 0.25, 1e-3) && fs.spot[0].at.z >= 0.5f);
	CHECK(near(fs.spot[1].at.x, -0.3125, 1e-3) && near(fs.spot[1].at.y, 0.25, 1e-3));
	CHECK(near(fs.spot[2].at.x, 0, 1e-3) && near(fs.spot[2].at.y, -0.25, 1e-3));
	CHECK(fs.spot[0].w > 0.2f && fs.spot[0].w < 0.6f && fs.spot[0].h > 0 && fs.spot[0].h < fs.spot[0].w);
	CHECK(fs.spot[2].w > fs.spot[0].w);
	CHECK(near(fs.box[0], -0.5, 1e-4) && near(fs.box[1], 0.5, 1e-4) && near(fs.box[2], 1, 1e-4));
	// Facing the other way, its eyes are still the ones its expressions
	// name, each pushed out of its face the way it now faces.
	FaceSpots turned = face_spots(fm, axis_angle({0, 1, 0}, 3.14159265f), CharacterSetup());
	CHECK(near(turned.spot[0].at.x, 0.3125, 1e-3) && turned.spot[0].at.z < 0.5f);
	// As its set-up says: in the model, or dragged on its front picture
	// (onto whatever's in front there).
	CharacterSetup given = setup_from(R"({"spots": {"left_eye": {"at": [0.1, 0.1, 0.6], "w": 0.2, "h": 0.1},
		"mouth": {"x": 0.5, "y": 0.75, "w": 0.4, "h": 0.2}}})");
	FaceSpots gs = face_spots(fm, Quat(), given);
	CHECK(std::strcmp(gs.from[0], "set up") == 0 && near3(gs.spot[0].at, {0.1f, 0.1f, 0.6f}, 1e-6f) &&
	      near(gs.spot[0].w, 0.2, 1e-6));
	CHECK(std::strcmp(gs.from[1], "expressions") == 0);
	CHECK(near(gs.spot[2].at.x, 0, 1e-4) && near(gs.spot[2].at.y, -0.25, 1e-4) && gs.spot[2].at.z > 0.5f &&
	      gs.spot[2].at.z < 0.52f && near(gs.spot[2].w, 0.4, 1e-4));
	// Nothing to go on: a guess at the front of its top half.
	GlbPart plain = face;
	plain.morph_names.clear();
	plain.morphs.clear();
	auto pm = std::make_shared<Model>();
	CHECK(load(write_glb({plain}), *pm));
	FaceSpots ps = face_spots(pm, Quat(), CharacterSetup());
	CHECK(std::strcmp(ps.from[0], "guessed") == 0 && ps.spot[0].at.x > 0 && ps.spot[1].at.x < 0 &&
	      ps.spot[0].at.y > 0 && ps.spot[2].at.y < ps.spot[0].at.y && near(ps.spot[0].at.z, 0.5, 0.02));

	// Its own drawn eyes ("own_face"): drawn as usual, but not while your
	// real ones are on it (every point in one, covering nothing).
	CHECK(setup_from(R"({"own_face": ["eyes", 3, "mouth"]})").own_face ==
	      std::vector<std::string>({"eyes", "mouth"}));
	GlbPart eyes = plain;
	eyes.name = "eyes";
	eyes.t = {0, 0, 0.1f};
	auto em = std::make_shared<Model>();
	CHECK(load(write_glb({plain, eyes}), *em));
	RigOptions eo;
	eo.setup = std::make_shared<CharacterSetup>(setup_from(R"({"own_face": ["eyes", "no such part"]})"));
	Rig er(em, eo);
	Drive shown = still();
	er.pose(shown);
	auto spread = [](const Rig::Part &p) {
		V3 lo = p.pos[0], hi = p.pos[0];
		for (const V3 &v : p.pos) {
			lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
			hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
		}
		return length(hi - lo);
	};
	CHECK(er.parts().size() == 2 && spread(er.parts()[0]) > 1 && spread(er.parts()[1]) > 1);
	shown.hide_own_face = true;
	er.pose(shown);
	CHECK(spread(er.parts()[0]) > 1 && spread(er.parts()[1]) == 0);
	shown.hide_own_face = false;
	er.pose(shown);
	CHECK(spread(er.parts()[1]) > 1);
}

int main()
{
	test_write_and_read();
	test_storage();
	test_broken();
	test_vrm1_reading();
	test_vrm0_reading();
	test_head_and_eyes();
	test_expressions();
	test_names_and_sides();
	test_springs();
	test_link_chain();
	test_dangly_parts();
	test_facing_arms_and_wearing();
	test_set_up();
	std::printf("unit_model: %d passed, %d failed\n", passes, failures);
	return failures ? 1 : 0;
}
