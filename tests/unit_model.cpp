// Unit tests for the 3D face avatar's model reader and posing
// (src/face/model.cpp, src/face/rig.cpp), on models made here: plain
// glTF the ways files store their numbers (packed, interleaved, sparse,
// squeezed into bytes, strips and fans, matrices), broken and hostile
// files, VRM 1.0 and 0.x (bones, expressions, swinging parts, what they
// bump into, a body that breathes, fingers that rest relaxed), and a
// chain of links swinging on one line of bones. And your face's tuning,
// alive when you're still and the camera check (src/face/alive.cpp).
// Build and run: tools/test-link.sh (Linux, any C++17 compiler).

#include "../src/face/alive.hpp"
#include "../src/face/effect-needs.hpp"
#include "../src/face/files.hpp"
#include "../src/face/lights.hpp"
#include "../src/face/model.hpp"
#include "../src/face/rig.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>
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

// A character in a folder (and a file) named in other letters loads (on
// Windows, C++'s own reading takes the PC's old code page unless told
// it's UTF-8), and a file made again in its place is seen to have changed.
static void test_files_named_in_other_letters()
{
	std::error_code ec;
	const std::filesystem::path folder =
		std::filesystem::temp_directory_path(ec) / utf8_path("direbot Jos\xc3\xa9 \xd0\x9b\xd0\xb8\xd1\x81\xd0\xb0");
	std::filesystem::create_directories(folder, ec);
	CHECK(!ec);
	const std::string path = folder.u8string() + "/Bunny \xc3\x91.glb";
	auto write = [&](int nodes) {
		Builder b;
		std::vector<V3> pos;
		std::vector<uint16_t> ix;
		cube(pos, ix);
		b.doc["meshes"].push_back(
			{{"primitives", {{{"attributes", {{"POSITION", b.vec3s(pos)}}}, {"indices", b.indices(ix)}}}}});
		for (int i = 0; i < nodes; i++)
			b.node({{"mesh", 0}, {"name", "part" + std::to_string(i)}});
		const std::vector<uint8_t> bytes = b.glb();
		std::ofstream f(utf8_path(path), std::ios::binary | std::ios::trunc);
		f.write(reinterpret_cast<const char *>(bytes.data()), (std::streamsize)bytes.size());
	};
	CHECK(file_stamp(path).empty());
	write(1);
	Model m;
	std::string why;
	CHECK(load_model(path, m, &why));
	CHECK(m.nodes.size() == 1);
	const std::string first = file_stamp(path);
	CHECK(!first.empty() && file_stamp(path) == first);
	write(2);
	CHECK(file_stamp(path) != first);
	Model again;
	CHECK(load_model(path, again, &why) && again.nodes.size() == 2);
	CHECK(file_stamp("").empty());
	std::filesystem::remove_all(folder, ec);
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

// `shapes`: how far its face shapes move (1 as made; tiny: markers).
static Person make_person(bool vrm0, float shapes = 1)
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
	std::vector<V3> open(8, V3{0, -0.05f * shapes, 0}), shut(8, V3{0.01f * shapes, 0, 0}),
		smile(8, V3{0, 0, 0.02f * shapes}), jaw(8, V3{0, 0, -0.03f * shapes});
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

// Where your eyes and mouth go on a VRM: a shape counts by every name it
// goes by, and with nothing to go on the guess is at its head.
static void test_spots_on_a_vrm()
{
	// Every mouth expression on the one "jawOpen" shape, as a simple VRM
	// made in Blender has it: the mouth is where that shape moves.
	{
		Person p = make_person(false);
		json &mesh = p.b.doc["meshes"][0];
		mesh["extras"]["targetNames"] = {"One", "Fcl_EYE_Close", "Three", "jawOpen"};
		json &preset = p.b.doc["extensions"]["VRMC_vrm"]["expressions"]["preset"];
		for (const char *viseme : {"aa", "ih", "ou", "ee", "oh"})
			preset[viseme] = {
				{"morphTargetBinds", {{{"node", p.face_node}, {"index", 3}, {"weight", 1.0}}}}};
		auto m = std::make_shared<Model>();
		CHECK(load(p.b.glb(), *m));
		FaceSpots fs = face_spots(m, Quat(), CharacterSetup());
		CHECK(std::strcmp(fs.from[2], "expressions") == 0);
		CHECK(std::strcmp(fs.from[0], "expressions") == 0 && std::strcmp(fs.from[1], "expressions") == 0);
	}
	// No eye bones, no expressions, a body far below its head: the guess
	// is on its head, not its chest.
	{
		Person p = make_person(false);
		Builder &b = p.b;
		json &bones = b.doc["extensions"]["VRMC_vrm"]["humanoid"]["humanBones"];
		bones.erase("leftEye");
		bones.erase("rightEye");
		b.doc["extensions"]["VRMC_vrm"].erase("expressions");
		b.doc["meshes"][0]["extras"]["targetNames"] = {"One", "Two", "Three", "Four"};
		std::vector<V3> pos;
		std::vector<uint16_t> ix;
		cube(pos, ix, {0, 0.6f, 0}, 0.4f);
		b.doc["meshes"].push_back(
			{{"primitives", {{{"attributes", {{"POSITION", b.vec3s(pos)}}}, {"indices", b.indices(ix)}}}}});
		const int body = b.node({{"name", "body"}, {"mesh", 1}});
		b.doc["scenes"][0]["nodes"].push_back(body);
		auto m = std::make_shared<Model>();
		CHECK(load(b.glb(), *m));
		FaceSpots fs = face_spots(m, Quat(), CharacterSetup());
		CHECK(std::strcmp(fs.from[0], "guessed") == 0 && std::strcmp(fs.from[2], "guessed") == 0);
		CHECK(fs.spot[0].at.y > 1.6f && fs.spot[0].at.y < 1.8f && near(fs.spot[0].at.x, 0.036, 2e-3) &&
		      near(fs.spot[1].at.x, -0.036, 2e-3));
		CHECK(fs.spot[2].at.y < fs.spot[0].at.y && fs.spot[2].at.y > 1.5f);
	}
}

// Two characters in one file sharing a blink and a mouth (a VRM's blink
// and aa bound to both): your eyes and mouth go on the one with the head
// bone, not stretched across both - even with the other one bigger.
static Drive still();

static void test_spots_on_two_characters()
{
	Person alone = make_person(false);
	auto one = std::make_shared<Model>();
	CHECK(load(alone.b.glb(), *one));
	const FaceSpots solo = face_spots(one, Quat(), CharacterSetup());

	Person p = make_person(false);
	Builder &b = p.b;
	// A big wolf's head to its left, moved further by the same shapes.
	std::vector<V3> pos;
	std::vector<uint16_t> ix;
	cube(pos, ix, {1.5f, 1.0f, 0}, 0.2f);
	std::vector<V3> open(8, V3{0, -0.2f, 0}), shut(8, V3{0.05f, 0, 0});
	json targets = json::array({{{"POSITION", b.vec3s(open)}}, {{"POSITION", b.vec3s(shut)}}});
	json prim = {{"attributes", {{"POSITION", b.vec3s(pos)}}}, {"indices", b.indices(ix)}, {"targets", targets}};
	b.doc["meshes"].push_back({{"primitives", json::array({prim})}, {"extras", {{"targetNames", {"wolf_A", "wolf_Close"}}}}});
	const int wolf = b.node({{"name", "Wolf"}, {"mesh", 1}});
	b.doc["scenes"][0]["nodes"].push_back(wolf);
	json &preset = b.doc["extensions"]["VRMC_vrm"]["expressions"]["preset"];
	preset["aa"]["morphTargetBinds"].push_back({{"node", wolf}, {"index", 0}, {"weight", 1.0}});
	preset["blink"]["morphTargetBinds"].push_back({{"node", wolf}, {"index", 1}, {"weight", 1.0}});
	auto two = std::make_shared<Model>();
	CHECK(load(b.glb(), *two));
	const FaceSpots fs = face_spots(two, Quat(), CharacterSetup());
	for (int i = 0; i < 3; i++) {
		CHECK(std::strcmp(fs.from[i], "expressions") == 0);
		CHECK(near3(fs.spot[i].at, solo.spot[i].at, 1e-3f));
		CHECK(near(fs.spot[i].w, solo.spot[i].w, 1e-3) && near(fs.spot[i].h, solo.spot[i].h, 1e-3));
	}
	CHECK(fs.spot[0].at.x > 0 && fs.spot[1].at.x < 0); // its own left eye on its left

	// Whose face moves: both, or one of them as picked (by name).
	RigOptions o;
	o.springs = false;
	Drive d = still();
	d.moves[move_of_name("jawOpen")] = 1;
	d.moves[move_of_name("eyeBlinkLeft")] = 1;
	d.moves[move_of_name("eyeBlinkRight")] = 1;
	Rig both(two, o);
	both.pose(d);
	CHECK(both.weights()[0][0] > 0.5f && both.weights()[1][0] > 0.5f && both.weights()[1][1] > 0.5f);
	o.still_faces = {"Wolf"};
	Rig only_person(two, o);
	only_person.pose(d);
	CHECK(only_person.weights()[0][0] > 0.5f && near(only_person.weights()[1][0], 0, 1e-6) &&
	      near(only_person.weights()[1][1], 0, 1e-6));
	o.still_faces = {"face"};
	Rig only_wolf(two, o);
	only_wolf.pose(d);
	CHECK(near(only_wolf.weights()[0][0], 0, 1e-6) && near(only_wolf.weights()[0][3], 0, 1e-6) &&
	      only_wolf.weights()[1][0] > 0.5f);
	// A part with no name goes by its place ("#n"); a name it hasn't, nothing.
	o.still_faces = {"#" + std::to_string(wolf), "Nobody"};
	Rig by_place(two, o);
	by_place.pose(d);
	CHECK(by_place.weights()[0][0] > 0.5f && near(by_place.weights()[1][0], 0, 1e-6));
}

static void test_face_region()
{
	// Someone with a big wolf beside them: a close look is round their
	// face, square, inside the box, every eye and the mouth in it.
	FaceSpots fs;
	fs.box[0] = -0.8f;
	fs.box[1] = 1.8f;
	fs.box[2] = 1.6f;
	fs.box[3] = 1.8f;
	fs.spot[0] = {{-0.364f, 1.68f, 0.1f}, 0.03f, 0.02f};
	fs.spot[1] = {{-0.436f, 1.68f, 0.1f}, 0.03f, 0.02f};
	fs.spot[2] = {{-0.4f, 1.6f, 0.1f}, 0.04f, 0.02f};
	const FaceRegion r = face_region(fs, Quat{});
	CHECK(r.w < 0.3f && r.h < 0.3f);
	CHECK(near(r.w * fs.box[2], r.h * fs.box[3], 1e-4));
	CHECK(r.x >= 0 && r.y >= 0 && r.x + r.w <= 1 + 1e-6f && r.y + r.h <= 1 + 1e-6f);
	for (const FaceSpot &s : fs.spot) {
		const float x = (s.at.x - fs.box[0]) / fs.box[2], y = (fs.box[1] - s.at.y) / fs.box[3];
		CHECK(x > r.x && x < r.x + r.w && y > r.y && y < r.y + r.h);
	}
	// Its middle a little above the eyes and mouth's (it's near the top
	// of the box here, so it can't go all the way).
	CHECK(r.y == 0);
	CHECK(near(r.x + r.w / 2, (-0.4f - fs.box[0]) / fs.box[2], 1e-3));
	// Turned round, the face is on the other side.
	const FaceRegion turned = face_region(fs, axis_angle({0, 1, 0}, 3.14159265f));
	CHECK(near(turned.x + turned.w / 2, (0.4f - fs.box[0]) / fs.box[2], 1e-3));
	// A head that's mostly face: all of it.
	FaceSpots head = fs;
	head.box[0] = -0.55f;
	head.box[1] = 1.75f;
	head.box[2] = 0.3f;
	head.box[3] = 0.3f;
	const FaceRegion whole = face_region(head, Quat{});
	CHECK(whole.x == 0 && whole.y == 0 && whole.w == 1 && whole.h == 1);
	// A tall thin one: its width, the top of it.
	FaceSpots thin = fs;
	thin.box[0] = -0.55f;
	thin.box[2] = 0.3f;
	const FaceRegion top = face_region(thin, Quat{});
	CHECK(top.w == 1 && top.h < 0.25f && top.y == 0);
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

	// Which sides show: MToon's culling (Unity's: 0 none, 1 its front, 2
	// its back) over glTF's doubleSided; without it, glTF's.
	{
		Person q = make_person(true);
		json &doc = q.b.doc;
		doc["materials"] = json::array();
		for (int i = 0; i < 5; i++)
			doc["materials"].push_back({{"name", "m" + std::to_string(i)}, {"doubleSided", i % 2 == 0}});
		auto mtoon = [](float cull) {
			return json{{"shader", "VRM/MToon"}, {"floatProperties", {{"_CullMode", cull}}}};
		};
		doc["extensions"]["VRM"]["materialProperties"] = {mtoon(0), mtoon(1), mtoon(2),
								  {{"shader", "VRM/MToon"}}, mtoon(7)};
		Model sides;
		CHECK(load(q.b.glb(), sides));
		CHECK(sides.materials.size() == 5);
		if (sides.materials.size() == 5) {
			// Drawn: both, the back, the front; mirrored, the other one.
			CHECK(sides_shown(sides.materials[0], 1) == Sides::Both &&
			      sides_shown(sides.materials[0], -1) == Sides::Both);
			CHECK(sides_shown(sides.materials[1], 1) == Sides::Back &&
			      sides_shown(sides.materials[1], -1) == Sides::Front);
			CHECK(sides_shown(sides.materials[2], 1) == Sides::Front &&
			      sides_shown(sides.materials[2], -1) == Sides::Back);
			CHECK(sides.materials[0].double_sided && !sides.materials[0].inside_only);
			CHECK(!sides.materials[1].double_sided && sides.materials[1].inside_only);
			CHECK(!sides.materials[2].double_sided && !sides.materials[2].inside_only);
			CHECK(!sides.materials[3].double_sided && !sides.materials[3].inside_only);
			CHECK(sides.materials[4].double_sided && !sides.materials[4].inside_only);
		}
	}
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
	CHECK(near(w[3], 0.8, 1e-5)); // jawOpen itself
	// aa as far: your moves come through your face's tuning (alive.hpp),
	// so an open mouth is already as open as it's tuned to be.
	CHECK(near(w[0], 0.8, 1e-5));
	CHECK(near(rig.expression_values().at("aa"), 0.8, 1e-5));
	// The box's points moved by them.
	CHECK(near(rig.parts()[0].pos[0].y, 1.6f - 0.05f * 0.8f, 1e-4));
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
	// The mouth from your mic (voice.hpp): all the mic's, an A heard -
	// aa as loud as it's heard whatever your camera's mouth does, and
	// the ARKit-named jawOpen as mic_into_moves opens it for an A.
	d = still();
	d.moves[move_of_name("jawOpen")] = 0.8f;
	d.mic.vowel[VOWEL_A] = 0.6f;
	d.mic.share = 1;
	rig.pose(d);
	CHECK(near(rig.expression_values().at("aa"), 0.6, 1e-5));
	CHECK(near(rig.weights()[0][0], 0.6, 1e-5));
	CHECK(near(rig.weights()[0][3], 0.75 * 0.6, 1e-5));
	// Half the mic's, half the camera's.
	d.mic.share = 0.5f;
	rig.pose(d);
	CHECK(near(rig.expression_values().at("aa"), 0.7, 1e-5));
	CHECK(near(rig.weights()[0][3], 0.8 + (0.45 - 0.8) * 0.5, 1e-5));
	// An O heard: aa shuts (oh isn't bound on this one), its jaw half open.
	d.mic = MicMouth();
	d.mic.vowel[VOWEL_O] = 1;
	d.mic.share = 1;
	rig.pose(d);
	CHECK(near(rig.expression_values().at("aa"), 0, 1e-5));
	CHECK(near(rig.weights()[0][3], 0.5, 1e-5));
	// None of it the mic's: the camera's mouth, as before.
	d.mic.share = 0;
	rig.pose(d);
	CHECK(near(rig.weights()[0][0], 0.8, 1e-5) && near(rig.weights()[0][3], 0.8, 1e-5));
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

// Whether its own face shows a blink and an open mouth: real shapes do;
// tiny marker shapes (only there to say where its eyes and mouth are)
// don't, on a VRM 1.0, a VRM 0.x or a plain one.
static void test_own_face_shows()
{
	RigOptions o;
	o.springs = false;
	for (bool vrm0 : {false, true}) {
		auto real = std::make_shared<Model>();
		CHECK(load(make_person(vrm0).b.glb(), *real));
		Rig r(real, o);
		CHECK(r.blinks() && r.talks());
		auto markers = std::make_shared<Model>();
		CHECK(load(make_person(vrm0, 0.001f).b.glb(), *markers));
		Rig k(markers, o);
		CHECK(!k.blinks() && !k.talks());
		// Its marker shapes are still driven as before (only too small to see).
		Drive d = still();
		d.moves[move_of_name("jawOpen")] = 1;
		k.pose(d);
		CHECK(near(k.weights()[0][3], 1, 1e-5));
	}
	// A file saved with its blink and mouth on (the mesh's own strengths
	// 1): they start shut and open as your face does, not stuck on.
	{
		Person p = make_person(false);
		p.b.doc["meshes"][0]["weights"] = {1.0, 1.0, 0.0, 1.0};
		auto saved = std::make_shared<Model>();
		CHECK(load(p.b.glb(), *saved));
		Rig r(saved, o);
		r.pose(still());
		CHECK(near(r.weights()[0][0], 0, 1e-6) && near(r.weights()[0][1], 0, 1e-6) &&
		      near(r.weights()[0][3], 0, 1e-6));
		Drive d = still();
		d.moves[move_of_name("eyeBlinkLeft")] = 1;
		d.moves[move_of_name("eyeBlinkRight")] = 1;
		r.pose(d);
		CHECK(near(r.weights()[0][1], 1, 1e-5));
		// Its eyes and mouth are found where they are open, too.
		const FaceSpots a = face_spots(saved, Quat(), CharacterSetup());
		auto plain = std::make_shared<Model>();
		CHECK(load(make_person(false).b.glb(), *plain));
		const FaceSpots b = face_spots(plain, Quat(), CharacterSetup());
		for (int i = 0; i < 3; i++)
			CHECK(near3(a.spot[i].at, b.spot[i].at, 1e-4f));
	}
	// A real mouth but a marker blink: talks, doesn't blink.
	Person p = make_person(false);
	{
		// Its blink shape (target 1) made tiny.
		const int acc = p.b.doc["meshes"][0]["primitives"][0]["targets"][1]["POSITION"].get<int>();
		const json &view = p.b.doc["bufferViews"][p.b.doc["accessors"][acc]["bufferView"].get<int>()];
		float *f = reinterpret_cast<float *>(p.b.bin.data() + view["byteOffset"].get<size_t>());
		for (int i = 0; i < 24; i++)
			f[i] *= 0.001f;
	}
	auto half = std::make_shared<Model>();
	CHECK(load(p.b.glb(), *half));
	Rig h(half, o);
	CHECK(!h.blinks() && h.talks());
	// Nothing at all: a box with no shapes neither blinks nor talks.
	Builder b;
	std::vector<V3> pos;
	std::vector<uint16_t> ix;
	cube(pos, ix);
	json prim = {{"attributes", {{"POSITION", b.vec3s(pos)}}}, {"indices", b.indices(ix)}};
	b.doc["meshes"].push_back({{"primitives", json::array({prim})}});
	b.node({{"mesh", 0}});
	auto plain = std::make_shared<Model>();
	CHECK(load(b.glb(), *plain));
	Rig n(plain, o);
	CHECK(!n.blinks() && !n.talks());
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
	CHECK(near(rig.weights()[0][1], 0.3, 1e-5));
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

// Its own animations (an idle, a tail's wag...): read as the file has
// them, played over and over when chosen - nothing when not. The bot
// itself wags no tail: a spring named "Tail" just hangs, standing still.
static void test_own_animation()
{
	Person p = make_person(false);
	Builder &b = p.b;
	const float PI = 3.14159265f;
	const Quat q40 = axis_angle({0, 1, 0}, 40 * PI / 180);
	const Quat nod = axis_angle({1, 0, 0}, 10 * PI / 180);
	// The ponytail as a tail: stiff enough to hold out behind.
	auto &sp = b.doc["extensions"]["VRMC_springBone"]["springs"][0];
	sp["name"] = "Tail";
	for (auto &j : sp["joints"])
		if (j.contains("stiffness"))
			j["stiffness"] = 1.0, j["gravityPower"] = 0.1;
	// "Wag": the tail's root swung 40 degrees round and back over a
	// second (straight between keys), its hips up 5 cm from 0.4 seconds
	// (jumping to it), its left eye grown to twice its size (curved).
	const int t3 = b.floats({0, 0.5f, 1}, 1, "SCALAR"), t2 = b.floats({0, 0.4f}, 1, "SCALAR");
	const int t01 = b.floats({0, 1}, 1, "SCALAR");
	const int swing = b.floats({0, 0, 0, 1, q40.x, q40.y, q40.z, q40.w, 0, 0, 0, 1}, 4, "VEC4");
	const int hop = b.floats({0, 1, 0, 0, 1.05f, 0}, 3, "VEC3");
	const int grow = b.floats({0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 2, 2, 2, 0, 0, 0}, 3, "VEC3");
	const int bad = b.floats({0, 0, 0, 0, 0, 0}, 3, "VEC3");
	b.doc["animations"] =
		json::array({{{"name", "Wag"},
			      {"samplers",
			       {{{"input", t3}, {"output", swing}},
				{{"input", t2}, {"output", hop}, {"interpolation", "STEP"}},
				{{"input", t01}, {"output", grow}, {"interpolation", "CUBICSPLINE"}},
				{{"input", t2}, {"output", bad}}}},
			      {"channels",
			       {{{"sampler", 0}, {"target", {{"node", p.tail0}, {"path", "rotation"}}}},
				{{"sampler", 1}, {"target", {{"node", p.hips}, {"path", "translation"}}}},
				{{"sampler", 2}, {"target", {{"node", p.leye}, {"path", "scale"}}}},
				// Left out: an expression's strengths, a missing part, a turn
				// of three numbers, a missing sampler.
				{{"sampler", 1}, {"target", {{"node", p.face_node}, {"path", "weights"}}}},
				{{"sampler", 0}, {"target", {{"node", 999}, {"path", "rotation"}}}},
				{{"sampler", 3}, {"target", {{"node", p.tail1}, {"path", "rotation"}}}},
				{{"sampler", 9}, {"target", {{"node", p.tail1}, {"path", "rotation"}}}}}}},
			     {{"samplers", json::array()}, {"channels", json::array()}},
			     {{"name", "Nod"},
			      {"samplers",
			       {{{"input", b.floats({0}, 1, "SCALAR")},
				 {"output", b.floats({nod.x, nod.y, nod.z, nod.w}, 4, "VEC4")}}}},
			      {"channels", {{{"sampler", 0}, {"target", {{"node", p.head}, {"path", "rotation"}}}}}}}});
	auto m = std::make_shared<Model>();
	std::string why;
	CHECK(load(b.glb(), *m, &why));
	CHECK(m->animations.size() == 3);
	if (m->animations.size() != 3)
		return;
	const Animation &wag = m->animations[0];
	CHECK(wag.name == "Wag" && m->animations[1].name == "Animation 2" && m->animations[2].name == "Nod");
	CHECK(wag.channels.size() == 3 && near(wag.length, 1, 1e-6) && m->animations[1].channels.empty());
	CHECK(animation_named(*m, "Wag") == 0 && animation_named(*m, "Nod") == 2 && animation_named(*m, "") == -1 &&
	      animation_named(*m, "wag") == -1);
	if (wag.channels.size() != 3)
		return;
	// Sampled: straight (the shortest way round), jumping, curved; held
	// before its first key and after its last.
	float v[4];
	auto turn_is = [&](float deg) {
		const Quat q = axis_angle({0, 1, 0}, deg * PI / 180);
		return near(std::fabs(v[0] * q.x + v[1] * q.y + v[2] * q.z + v[3] * q.w), 1, 1e-5);
	};
	sample(wag.channels[0], 0.25f, v);
	CHECK(turn_is(20));
	sample(wag.channels[0], 0.5f, v);
	CHECK(turn_is(40));
	sample(wag.channels[0], 0.875f, v);
	CHECK(turn_is(10));
	sample(wag.channels[0], -1, v);
	CHECK(turn_is(0));
	sample(wag.channels[0], 5, v);
	CHECK(turn_is(0));
	sample(wag.channels[1], 0.3f, v);
	CHECK(near3({v[0], v[1], v[2]}, {0, 1, 0}, 1e-6f));
	sample(wag.channels[1], 0.6f, v);
	CHECK(near3({v[0], v[1], v[2]}, {0, 1.05f, 0}, 1e-6f));
	sample(wag.channels[2], 0.5f, v);
	CHECK(near(v[0], 1.5, 1e-5));
	sample(wag.channels[2], 0.25f, v);
	CHECK(near(v[0], 1.15625, 1e-5)); // 1 + 3u^2 - 2u^3
	// Curved with slopes: a slope out of the first key of 4 a second.
	AnimChannel sloped = wag.channels[2];
	sloped.values[6] = 4, sloped.values[7] = 4, sloped.values[8] = 4;
	sample(sloped, 0.5f, v);
	CHECK(near(v[0], 1.5 + 4 * 0.125, 1e-5)); // (u^3 - 2u^2 + u) at a half: 0.125

	// Not chosen: it all stands still, the tail too.
	auto tip_x = [&](Rig &rig, int frames, float *lo, float *hi) {
		Drive d = still();
		*lo = 1e9f, *hi = -1e9f;
		for (int i = 0; i < frames; i++) {
			rig.pose(d);
			const float x = move_of(rig.world()[p.tail2]).x;
			*lo = std::min(*lo, x), *hi = std::max(*hi, x);
		}
	};
	{
		Rig rig(m, RigOptions());
		CHECK(rig.animation() == -1);
		float lo, hi;
		tip_x(rig, 240, &lo, &hi);
		CHECK(hi - lo < 1e-3f);
	}
	// Chosen, nothing swinging: exactly as it has them, over and over.
	{
		RigOptions o;
		o.springs = false;
		o.animation = 0;
		Rig rig(m, o);
		Drive d = still();
		for (int i = 0; i < 30; i++)
			rig.pose(d); // half a second in
		const float x_at_half = move_of(rig.world()[p.tail2]).x;
		CHECK(near(x_at_half, -0.2 * std::sin(40 * PI / 180), 1e-3));
		CHECK(near(move_of(rig.world()[p.head]).y, 1.05 + 0.5 + 0.1, 1e-4));
		const M4 &eye = rig.world()[p.leye];
		CHECK(near(length(V3{eye.m[0], eye.m[1], eye.m[2]}), 1.5, 1e-3));
		for (int i = 0; i < 45; i++)
			rig.pose(d); // 1.25 seconds: a quarter into it again
		CHECK(near(move_of(rig.world()[p.tail2]).x, -0.2 * std::sin(20 * PI / 180), 1e-3));
		CHECK(near(move_of(rig.world()[p.head]).y, 1.6, 1e-4));
		// Another of them (with nothing in it): all at rest at once;
		// one it hasn't got: none.
		rig.set_animation(1);
		rig.pose(d);
		CHECK(near(move_of(rig.world()[p.tail2]).x, 0, 1e-5) && rig.animation() == 1);
		rig.set_animation(7);
		CHECK(rig.animation() == -1);
	}
	// Chosen, swinging: the tail swings from where its animation has it,
	// side to side, a little after it.
	{
		RigOptions o;
		o.animation = 0;
		Rig rig(m, o);
		float lo, hi;
		tip_x(rig, 120, &lo, &hi); // settled in
		tip_x(rig, 120, &lo, &hi);
		CHECK(hi - lo > 0.06f && lo < -0.05f);
	}
	// Your head turns from where its animation has it: nodded 10 degrees,
	// and turned with you.
	{
		RigOptions o;
		o.springs = false;
		o.animation = 2;
		Rig rig(m, o);
		Drive d = still();
		rig.pose(d);
		V3 ahead = rotate(quat_of(rig.world()[p.head]), {0, 0, 1});
		CHECK(near3(ahead, {0, -std::sin(10 * PI / 180), std::cos(10 * PI / 180)}, 1e-4f));
		d.head = axis_angle({0, 1, 0}, 20 * PI / 180);
		rig.pose(d);
		ahead = rotate(quat_of(rig.world()[p.head]), {0, 0, 1});
		CHECK(near(ahead.y, -std::sin(10 * PI / 180), 0.02) && near(ahead.x, std::sin(20 * PI / 180), 0.03));
	}
	// Arms its animation turns stay where it has them (not brought down
	// again); arms it doesn't come down by its sides as ever.
	{
		Person a = make_person(false);
		Builder &ab = a.b;
		const int lu = ab.node({{"name", "upper_arm.L"}, {"translation", {0.1f, 0.4f, 0}}});
		const int ll = ab.node({{"name", "lower_arm.L"}, {"translation", {0.25f, 0, 0}}});
		const int ru = ab.node({{"name", "upper_arm.R"}, {"translation", {-0.1f, 0.4f, 0}}});
		const int rl = ab.node({{"name", "lower_arm.R"}, {"translation", {-0.25f, 0, 0}}});
		ab.doc["nodes"][a.hips]["children"].push_back(lu);
		ab.doc["nodes"][a.hips]["children"].push_back(ru);
		ab.doc["nodes"][lu]["children"] = {ll};
		ab.doc["nodes"][ru]["children"] = {rl};
		json &bones = ab.doc["extensions"]["VRMC_vrm"]["humanoid"]["humanBones"];
		bones["leftUpperArm"] = {{"node", lu}};
		bones["leftLowerArm"] = {{"node", ll}};
		bones["rightUpperArm"] = {{"node", ru}};
		bones["rightLowerArm"] = {{"node", rl}};
		const Quat down = axis_angle({0, 0, 1}, -70 * PI / 180);
		ab.doc["animations"] = json::array(
			{{{"name", "Arm down"},
			  {"samplers",
			   {{{"input", ab.floats({0}, 1, "SCALAR")},
			     {"output", ab.floats({down.x, down.y, down.z, down.w}, 4, "VEC4")}}}},
			  {"channels", {{{"sampler", 0}, {"target", {{"node", lu}, {"path", "rotation"}}}}}}}});
		auto am = std::make_shared<Model>();
		CHECK(load(ab.glb(), *am));
		RigOptions o;
		o.springs = false;
		o.animation = 0;
		Rig rig(am, o);
		rig.pose(still());
		const V3 left = move_of(rig.world()[ll]) - move_of(rig.world()[lu]);
		const V3 right = move_of(rig.world()[rl]) - move_of(rig.world()[ru]);
		CHECK(near(std::acos(dot(normalized(left), {0, -1, 0})), 20 * PI / 180, 1e-3));
		CHECK(near(std::acos(dot(normalized(right), {0, -1, 0})), 0.3, 1e-3));
	}
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

// OBS's own sums for its 3D, as libobs 30-32 does them: gs_perspective's
// frustum on each graphics card's way (Direct3D 11, OpenGL), and what each
// does to the view before drawing: Direct3D turns its z round, OpenGL
// turns y round drawing into a picture. `d3d_turns_z_too`: the old mistake,
// turning z round in the add-on as well.
struct Seen {
	float w, across, down, depth; // down: a share of the picture from its top
};
static Seen obs_sees(bool d3d, const Framing &f, float aspect, V3 world, bool d3d_turns_z_too = false)
{
	M4 view = obs_view(f.view);
	if (d3d_turns_z_too) {
		M4 turn;
		turn.m[10] = -1;
		view = turn * view;
	}
	if (d3d)
		for (int c = 0; c < 4; c++)
			view.at(2, c) = -view.at(2, c);
	const float n = f.near, fa = f.far;
	const float ymax = n * std::tan(f.fov * 0.5f), ymin = -ymax;
	const float left = ymin * aspect, right = ymax * aspect, top = ymin, bottom = ymax;
	M4 p;
	p.m[0] = p.m[5] = p.m[10] = p.m[15] = 0;
	const float rml = right - left;
	p.at(0, 0) = 2 * n / rml;
	if (d3d) {
		const float bmt = bottom - top, fmn = fa - n;
		p.at(0, 2) = (left + right) / -rml;
		p.at(1, 1) = 2 * n / -bmt;
		p.at(1, 2) = (bottom + top) / bmt;
		p.at(2, 2) = fa / fmn;
		p.at(2, 3) = n * fa / -fmn;
		p.at(3, 2) = 1;
	} else {
		const float tmb = top - bottom, nmf = n - fa;
		p.at(0, 2) = (left + right) / rml;
		p.at(1, 1) = 2 * n / tmb;
		p.at(1, 2) = (bottom + top) / tmb;
		p.at(2, 2) = (fa + n) / nmf;
		p.at(2, 3) = 2 * n * fa / nmf;
		p.at(3, 2) = -1;
		for (int c = 0; c < 4; c++)
			p.at(1, c) = -p.at(1, c);
	}
	const M4 all = p * view;
	float clip[4];
	const float in[4] = {world.x, world.y, world.z, 1};
	for (int r = 0; r < 4; r++) {
		clip[r] = 0;
		for (int k = 0; k < 4; k++)
			clip[r] += all.at(r, k) * in[k];
	}
	Seen s;
	s.w = clip[3];
	const float x = clip[0] / clip[3], y = clip[1] / clip[3], z = clip[2] / clip[3];
	s.across = (x + 1) / 2;
	// Direct3D's picture starts at its top (y +1); OpenGL's at y -1, which
	// OBS shows as the top too.
	s.down = d3d ? (1 - y) / 2 : (1 + y) / 2;
	s.depth = d3d ? z : (z + 1) / 2;
	return s;
}

static void test_obs_camera()
{
	// A character 1.6 tall, in its own spot and worn on a head: on both
	// graphics cards it's in front of the camera, in the picture, the
	// right way up, near before far, and in the same place on each.
	Model m;
	m.low = {-0.3f, 0, -0.1f};
	m.high = {0.3f, 1.6f, 0.1f};
	const float aspect = 16 / 9.0f;
	M4 pose = moved({0, 0, -60}); // your face 60 cm in front of the camera
	const Framing frames[2] = {frame_own_spot(m, Quat{}, 1280, 720, 0.8f, false, 0, 0, 1),
				   frame_worn(m, Quat{}, WornPlace(), pose, aspect)};
	for (const Framing &f : frames) {
		const V3 top = point(f.place, {0, 1.6f, 0}), feet = point(f.place, {0, 0, 0});
		const V3 front = point(f.place, {0, 0.8f, 0.1f}), back = point(f.place, {0, 0.8f, -0.1f});
		const V3 left_side = point(f.place, {-0.3f, 0.8f, 0});
		for (int d3d = 0; d3d < 2; d3d++) {
			for (V3 at : {top, feet, front, back, left_side}) {
				const Seen s = obs_sees(d3d, f, aspect, at);
				CHECK(s.w > 0);
				CHECK(s.across > -0.01f && s.across < 1.01f && s.down > -0.01f && s.down < 1.01f);
				CHECK(s.depth > 0 && s.depth < 1);
				// The same spot in the picture whichever card draws it.
				const Seen o = obs_sees(!d3d, f, aspect, at);
				CHECK(near(s.across, o.across, 1e-4) && near(s.down, o.down, 1e-4));
			}
			CHECK(obs_sees(d3d, f, aspect, top).down < obs_sees(d3d, f, aspect, feet).down);
			CHECK(obs_sees(d3d, f, aspect, front).depth < obs_sees(d3d, f, aspect, back).depth);
			// Its -x side on the picture's left: x runs right on both.
			CHECK(obs_sees(d3d, f, aspect, left_side).across < 0.5f);
		}
		// Turning z round in the add-on as well as in OBS's Direct3D put
		// it all behind the camera: nothing drawn.
		CHECK(obs_sees(true, f, aspect, top, true).w < 0);
	}
	// Moved on in its spot (shares of the picture): the same as framed
	// there, and that far across and down the picture.
	{
		const Framing there = frame_own_spot(m, Quat{}, 1280, 720, 0.8f, false, 0.1f, 0.05f, 1.2f);
		Framing moved_on = frame_own_spot(m, Quat{}, 1280, 720, 0.8f, false, 0, 0, 1.2f);
		const Seen before = obs_sees(false, moved_on, aspect, point(moved_on.place, {0, 0.8f, 0}));
		move_own_spot(moved_on, 0.1f, 0.05f);
		CHECK(near(moved_on.seen_w / moved_on.seen_h, aspect, 1e-4));
		for (int i = 0; i < 16; i++)
			CHECK(near(moved_on.place.m[i], there.place.m[i], 1e-5) &&
			      near(moved_on.upright.m[i], there.upright.m[i], 1e-5));
		const Seen after = obs_sees(false, moved_on, aspect, point(moved_on.place, {0, 0.8f, 0}));
		CHECK(near(after.across - before.across, 0.1, 1e-3) && near(after.down - before.down, 0.05, 1e-3));
	}
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

#ifndef DIREBOT_SOURCE_DIR
#define DIREBOT_SOURCE_DIR "."
#endif

// The values effect-needs.hpp says each technique reads are exactly the
// ones its shaders read, as OBS works it out: by every word in each
// function (and the functions it calls) that's a value's name. The
// drawing sets those before every technique, and Windows won't draw one
// with any value missing. And no function has its own variable named
// like a value, which OBS would take as reading the value.
static void check_effect_needs(const std::string &file, const std::vector<direbot::face::EffectNeeds> &needs)
{
	std::ifstream in(std::string(DIREBOT_SOURCE_DIR) + "/data/effects/" + file);
	CHECK(in.good());
	std::stringstream ss;
	ss << in.rdbuf();
	const std::string text = std::regex_replace(ss.str(), std::regex("//[^\n]*"), "");

	std::set<std::string> uniforms;
	const std::regex uniform_re("uniform\\s+\\w+\\s+(\\w+)\\s*[;=]");
	for (std::sregex_iterator it(text.begin(), text.end(), uniform_re), end; it != end; ++it)
		uniforms.insert((*it)[1]);
	CHECK(uniforms.count("image"));

	// Each function's body, by name.
	std::map<std::string, std::string> bodies;
	const std::regex func_re("(^|\\n)\\w+\\s+(\\w+)\\s*\\([^)]*\\)\\s*(:\\s*\\w+\\s*)?\\{");
	for (std::sregex_iterator it(text.begin(), text.end(), func_re), end; it != end; ++it) {
		size_t at = (size_t)(it->position() + it->length());
		int depth = 1;
		size_t i = at;
		for (; i < text.size() && depth; i++)
			depth += text[i] == '{' ? 1 : text[i] == '}' ? -1 : 0;
		bodies[(*it)[2]] = text.substr(at, i - at);
	}
	CHECK(bodies.size() > 3);

	const std::regex local_re("\\b(float[234]?(x[234])?|int[234]?|uint|bool|half[234]?)\\s+(\\w+)\\s*[=;,\\[]");
	for (const auto &b : bodies)
		for (std::sregex_iterator it(b.second.begin(), b.second.end(), local_re), end; it != end; ++it)
			if (uniforms.count((*it)[3])) {
				std::printf("  %s: %s has its own %s\n", file.c_str(), b.first.c_str(),
					    (*it)[3].str().c_str());
				CHECK(false);
			}

	std::function<void(const std::string &, std::set<std::string> &, std::set<std::string> &)> reads;
	reads = [&](const std::string &fn, std::set<std::string> &got, std::set<std::string> &seen) {
		if (!seen.insert(fn).second || !bodies.count(fn))
			return;
		const std::string &body = bodies[fn];
		const std::regex word("[A-Za-z_]\\w*");
		for (std::sregex_iterator it(body.begin(), body.end(), word), end; it != end; ++it) {
			const std::string w = it->str();
			if (uniforms.count(w) && w != "ViewProj")
				got.insert(w);
			if (bodies.count(w))
				reads(w, got, seen);
		}
	};

	const std::regex tech_re(
		"technique\\s+(\\w+)\\s*\\{\\s*pass\\s*\\{\\s*vertex_shader\\s*=\\s*(\\w+)\\s*\\([^)]*\\)\\s*;"
		"\\s*pixel_shader\\s*=\\s*(\\w+)");
	size_t techniques = 0;
	for (std::sregex_iterator it(text.begin(), text.end(), tech_re), end; it != end; ++it, techniques++) {
		std::set<std::string> got, seen;
		reads((*it)[2], got, seen);
		reads((*it)[3], got, seen);
		const direbot::face::EffectNeeds *listed = nullptr;
		for (const auto &n : needs)
			if ((*it)[1] == n.technique)
				listed = &n;
		CHECK(listed != nullptr);
		if (!listed)
			continue;
		const std::set<std::string> want(listed->values.begin(), listed->values.end());
		if (want != got) {
			std::string have;
			for (const auto &g : got)
				have += g + " ";
			std::printf("  %s %s reads: %s\n", file.c_str(), listed->technique, have.c_str());
		}
		CHECK(want == got);
		CHECK(want.size() == listed->values.size());
	}
	CHECK(techniques == needs.size());
}

static void test_effect_needs()
{
	check_effect_needs("direbot-face-3d.effect", direbot::face::face_3d_needs());
	check_effect_needs("direbot-face.effect", direbot::face::face_2d_needs());
}


// --- Detail maps, lights, soft skin, camera views ---------------------------

// A square facing +z (x 0 to 1, y 0 to 1), its picture the right way up
// (glTF: v down the picture), with a normal map (and an occlusion map),
// as `materials` says; `tangents` in the file if given.
static std::vector<uint8_t> bumpy_square(const json &material, const std::vector<float> &tangents = {},
					 bool mirrored_uv = false, json extra_material = json())
{
	Builder b;
	const std::vector<V3> pos = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}};
	const std::vector<V3> normal(4, V3{0, 0, 1});
	std::vector<float> uv = {0, 1, 1, 1, 0, 0, 1, 0};
	if (mirrored_uv)
		for (size_t i = 0; i < uv.size(); i += 2)
			uv[i] = 1 - uv[i];
	const std::vector<uint16_t> ix = {0, 1, 2, 2, 1, 3};
	json attr = {{"POSITION", b.vec3s(pos)}, {"NORMAL", b.vec3s(normal)}, {"TEXCOORD_0", b.floats(uv, 2, "VEC2")}};
	if (!tangents.empty())
		attr["TANGENT"] = b.floats(tangents, 4, "VEC4");
	json prims = json::array({{{"attributes", attr}, {"indices", b.indices(ix)}, {"material", 0}}});
	if (!extra_material.is_null())
		prims.push_back({{"attributes", attr}, {"indices", b.indices(ix)}, {"material", 1}});
	b.doc["meshes"].push_back({{"primitives", prims}});
	b.doc["materials"].push_back(material);
	if (!extra_material.is_null())
		b.doc["materials"].push_back(extra_material);
	b.doc["images"].push_back({{"uri", "data:image/png;base64,"}});
	b.doc["textures"].push_back({{"source", 0}});
	b.node({{"name", "square"}, {"mesh", 0}});
	return b.glb();
}

static void test_detail_maps()
{
	const json bumpy = {{"name", "Skin"},
			    {"normalTexture", {{"index", 0}, {"scale", 0.5}}},
			    {"occlusionTexture", {{"index", 0}, {"strength", 0.7}}}};
	// Read from the material, its tangents worked out from its UVs: its
	// picture's right along +x, its up along +y (w 1).
	{
		Model m;
		CHECK(load(bumpy_square(bumpy), m));
		const Material &mat = m.materials[0];
		CHECK(mat.normal_map.used() && near(mat.normal_scale, 0.5, 1e-6));
		CHECK(mat.occlusion_map.used() && near(mat.occlusion_strength, 0.7, 1e-6));
		const Primitive &p = m.meshes[0].parts[0];
		CHECK(p.tangent.size() == 16 && !p.own_tangents);
		for (size_t i = 0; i < 4 && p.tangent.size() == 16; i++)
			CHECK(near(p.tangent[i * 4], 1, 1e-5) && near(p.tangent[i * 4 + 1], 0, 1e-5) &&
			      near(p.tangent[i * 4 + 2], 0, 1e-5) && p.tangent[i * 4 + 3] == 1);
	}
	// The picture flipped left to right: its right along -x, its up still
	// +y - so w is -1.
	{
		Model m;
		CHECK(load(bumpy_square(bumpy, {}, true), m));
		const Primitive &p = m.meshes[0].parts[0];
		CHECK(p.tangent.size() == 16 && near(p.tangent[0], -1, 1e-5) && p.tangent[3] == -1);
	}
	// The file's own tangents, squared up (1 long, w 1 or -1).
	{
		Model m;
		CHECK(load(bumpy_square(bumpy, {2, 0, 0, -3, 2, 0, 0, -3, 0, 3, 0, 0.5f, 0, 3, 0, 0.5f}), m));
		const Primitive &p = m.meshes[0].parts[0];
		CHECK(p.tangent.size() == 16 && near(p.tangent[0], 1, 1e-6) && p.tangent[3] == -1);
		CHECK(near(p.tangent[9], 1, 1e-6) && p.tangent[11] == 1);
		CHECK(p.own_tangents);
	}
	// Broken ones in the file (no length): worked out instead.
	{
		Model m;
		CHECK(load(bumpy_square(bumpy, std::vector<float>(16, 0.0f)), m));
		const Primitive &p = m.meshes[0].parts[0];
		CHECK(p.tangent.size() == 16 && near(p.tangent[0], 1, 1e-5) && p.tangent[3] == 1);
		CHECK(!p.own_tangents);
	}
	// No normal map: no tangents kept, even the file's.
	{
		Model m;
		CHECK(load(bumpy_square({{"name", "plain"}}, std::vector<float>(16, 1.0f)), m));
		CHECK(!m.materials[0].normal_map.used() && m.meshes[0].parts[0].tangent.empty());
		CHECK(!m.meshes[0].parts[0].own_tangents);
	}
	// A map on the second UVs (not read): left out.
	{
		Model m;
		CHECK(load(bumpy_square({{"normalTexture", {{"index", 0}, {"texCoord", 1}}},
					 {"occlusionTexture", {{"index", 0}, {"texCoord", 1}}}}),
			   m));
		CHECK(!m.materials[0].normal_map.used() && !m.materials[0].occlusion_map.used());
		CHECK(m.meshes[0].parts[0].tangent.empty());
	}
	// A silly strength: kept sensible.
	{
		Model m;
		CHECK(load(bumpy_square({{"normalTexture", {{"index", 0}, {"scale", 1e9}}},
					 {"occlusionTexture", {{"index", 0}, {"strength", -5}}}}),
			   m));
		CHECK(near(m.materials[0].normal_scale, 4, 1e-6) && near(m.materials[0].occlusion_strength, 0, 1e-6));
	}
	// Posed: turned with its node, and a mirrored node's up the other way.
	{
		Model m;
		CHECK(load(bumpy_square(bumpy), m));
		m.nodes[0].r = axis_angle({0, 0, 1}, 3.14159265f / 2);
		RigOptions o;
		o.springs = false;
		auto model = std::make_shared<Model>(m);
		Rig rig(model, o);
		rig.pose(still());
		const Rig::Part &part = rig.parts()[0];
		CHECK(part.tangent.size() == 4 && near3(part.tangent[0], {0, 1, 0}, 1e-4f) && part.handed == 1);
		Model mm;
		CHECK(load(bumpy_square(bumpy), mm));
		mm.nodes[0].s = {-1, 1, 1};
		Rig mirrored(std::make_shared<Model>(mm), o);
		mirrored.pose(still());
		CHECK(mirrored.parts()[0].handed == -1 && near3(mirrored.parts()[0].tangent[0], {-1, 0, 0}, 1e-4f));
		// A part without a normal map: no tangents posed.
		Model plain;
		CHECK(load(bumpy_square({{"name", "plain"}}), plain));
		Rig r2(std::make_shared<Model>(plain), o);
		r2.pose(still());
		CHECK(r2.parts()[0].tangent.empty());
	}
	// VRM 1.0's MToon keeps glTF's normal map.
	{
		json mtoon = bumpy;
		mtoon["extensions"] = {{"VRMC_materials_mtoon", {{"specVersion", "1.0"}}}};
		Model m;
		CHECK(load(bumpy_square(mtoon), m));
		CHECK(m.materials[0].shading == Shading::Toon && m.materials[0].normal_map.used());
		CHECK(m.meshes[0].parts[0].tangent.size() == 16);
	}
	// Two materials on one part's points: tangents only where it's bumpy.
	{
		Model m;
		CHECK(load(bumpy_square(bumpy, {}, false, {{"name", "Shirt"}}), m));
		CHECK(m.meshes[0].parts.size() == 2 && m.meshes[0].parts[0].tangent.size() == 16 &&
		      m.meshes[0].parts[1].tangent.empty());
	}
	// Worked out on a curved part (a box): square to each facing.
	{
		Primitive p;
		std::vector<uint16_t> ix16;
		cube(p.pos, ix16);
		p.index.assign(ix16.begin(), ix16.end());
		for (const V3 &v : p.pos)
			p.uv.insert(p.uv.end(), {v.x + 0.5f, 0.5f - v.y});
		p.normal.assign(p.pos.size(), V3{});
		for (size_t i = 0; i < p.pos.size(); i++)
			p.normal[i] = normalized(p.pos[i], {0, 0, 1});
		const std::vector<float> t = tangents_of(p);
		CHECK(t.size() == p.pos.size() * 4);
		bool square = true, finite = true;
		for (size_t i = 0; i < p.pos.size() && t.size() == p.pos.size() * 4; i++) {
			const V3 tv{t[i * 4], t[i * 4 + 1], t[i * 4 + 2]};
			square = square && std::fabs(dot(tv, p.normal[i])) < 1e-4f && near(length(tv), 1, 1e-4);
			finite = finite && std::isfinite(tv.x + tv.y + tv.z) && (t[i * 4 + 3] == 1 || t[i * 4 + 3] == -1);
		}
		CHECK(square && finite);
		// No UVs: none.
		p.uv.clear();
		CHECK(tangents_of(p).empty());
	}
	// MikkTSpace's, as Blender works them out: round a tube (its picture
	// wrapped round it, the picture's top at the tube's top), each point's
	// tangent along the picture's right - round the tube - and its up up
	// the tube; the picture's second half mirrored (its right the other
	// way round, its up still up: w the other way). Points at its seam,
	// the mirror's middle and its ends are shared as a file has them.
	{
		Primitive p;
		const int K = 16;
		for (int row = 0; row < 2; row++)
			for (int j = 0; j <= K; j++) {
				const float a = 2 * 3.14159265f * j / K;
				p.pos.push_back({std::cos(a), (float)row, std::sin(a)});
				p.normal.push_back({std::cos(a), 0, std::sin(a)});
				// u to 1 half way round, then back (mirrored).
				const float u = j <= K / 2 ? 2.0f * j / K : 2.0f - 2.0f * j / K;
				p.uv.insert(p.uv.end(), {u, row ? 0.0f : 1.0f});
			}
		for (int j = 0; j < K; j++) {
			const uint32_t a = j, b = j + 1, c = K + 1 + j, d = K + 2 + j;
			p.index.insert(p.index.end(), {a, c, b, b, c, d});
		}
		const std::vector<float> t = tangents_of(p);
		CHECK(t.size() == p.pos.size() * 4);
		int right = 0, flipped = 0;
		for (int row = 0; row < 2 && t.size() == p.pos.size() * 4; row++)
			for (int j = 1; j < K; j++) {
				if (j == K / 2)
					continue; // the mirror's middle: either way
				const size_t i = (size_t)row * (K + 1) + j;
				const float a = 2 * 3.14159265f * j / K;
				const V3 round{-std::sin(a), 0, std::cos(a)}; // u growing, first half
				const V3 tv{t[i * 4], t[i * 4 + 1], t[i * 4 + 2]};
				const V3 up = cross(p.normal[i], tv) * t[i * 4 + 3];
				const bool first = j < K / 2;
				right += near3(tv, first ? round : round * -1.0f, 1e-3f) && near3(up, {0, 1, 0}, 1e-3f);
				flipped += t[i * 4 + 3] == (first ? -1.0f : 1.0f);
			}
		CHECK(right == 2 * (K - 2) && flipped == 2 * (K - 2));
		// The same again: the same tangents.
		CHECK(tangents_of(p) == t);
		// A triangle naming a point that isn't there is left out, and a
		// point no triangle has gets one anyway.
		p.index.insert(p.index.end(), {0, 1, 999});
		p.pos.push_back({0, 5, 0});
		p.normal.push_back({0, 1, 0});
		p.uv.insert(p.uv.end(), {0.5f, 0.5f});
		const std::vector<float> t2 = tangents_of(p);
		CHECK(t2.size() == p.pos.size() * 4 && std::equal(t.begin(), t.end(), t2.begin()));
		const size_t last = (p.pos.size() - 1) * 4;
		CHECK(t2.size() == p.pos.size() * 4 && near(length(V3{t2[last], t2[last + 1], t2[last + 2]}), 1, 1e-5) &&
		      near(t2[last + 1], 0, 1e-5));
	}
}

static void test_lights()
{
	// The studio: exactly the lights it always had.
	const Lights studio = preset_lights(LIGHTS_STUDIO), plain;
	CHECK(near3(studio.key_dir, {-0.45f, 0.6f, 0.66f}, 0) && near3(studio.key, {1.05f, 1.029f, 0.9975f}, 0));
	CHECK(near3(studio.fill, {0.2475f, 0.261f, 0.2925f}, 0) && near3(studio.room_up, {0.42f, 0.43f, 0.46f}, 0));
	CHECK(near3(studio.room_down, {0.16f, 0.15f, 0.15f}, 0) && near3(studio.rim, {0, 0, 0}, 0));
	CHECK(near3(plain.key, studio.key, 0));
	// Your camera's eyes and mouth on it: as they always were in the
	// studio; darker and bluer at night; pinker in neon; brighter with
	// the light doubled; never past three times.
	const V3 front = front_light(studio), night_front = front_light(preset_lights(LIGHTS_NIGHT));
	CHECK(front.x == 1 && front.y == 1 && front.z == 1);
	CHECK(night_front.x < 0.6f && night_front.z > night_front.x * 1.5f);
	const V3 neon_front = front_light(preset_lights(LIGHTS_NEON));
	CHECK(neon_front.x > neon_front.y * 1.3f);
	LightTerms doubled = preset_terms(LIGHTS_STUDIO);
	doubled.brightness = 200;
	doubled.room = 210;
	const V3 bright_front = front_light(lights_of(doubled));
	CHECK(bright_front.x > 1.9f && bright_front.x < 2.1f);
	doubled.brightness = doubled.room = 300;
	Lights blinding = lights_of(doubled);
	blinding.key = blinding.key * 10;
	CHECK(front_light(blinding).x == 3);
	// Its plain terms make lights very close to it ("make my own" from
	// the studio changes nothing you'd see).
	const Lights own_studio = lights_of(preset_terms(LIGHTS_STUDIO));
	CHECK(dot(normalized(own_studio.key_dir, {}), normalized(studio.key_dir, {})) > std::cos(1.0f * 3.14159265f / 180));
	CHECK(near3(own_studio.key, studio.key, 0.005f) && near3(own_studio.room_up, studio.room_up, 0.005f));
	CHECK(near3(own_studio.fill, studio.fill, 0.005f) && near3(own_studio.room_down, studio.room_down, 0.005f));
	CHECK(length(own_studio.rim) == 0);
	// Each set by its name; an unknown name is the studio.
	for (int i = 0; i < LIGHT_PRESETS; i++)
		CHECK(light_preset_of(LIGHT_PRESET_NAMES[i]) == i);
	CHECK(light_preset_of("") == LIGHTS_STUDIO && light_preset_of("disco") == LIGHTS_STUDIO);
	// The ready-made sets: sensible numbers, each its own look.
	auto brightness = [](V3 c) { return c.x + c.y + c.z; };
	for (int i = LIGHTS_DAYLIGHT; i <= LIGHTS_NEON; i++) {
		const Lights l = preset_lights(i);
		bool fine = true;
		for (const V3 &c : {l.key, l.fill, l.room_up, l.room_down, l.rim, l.key_dir})
			fine = fine && std::isfinite(c.x + c.y + c.z);
		for (const V3 &c : {l.key, l.fill, l.room_up, l.room_down, l.rim})
			fine = fine && c.x >= 0 && c.y >= 0 && c.z >= 0;
		CHECK(fine && length(l.key_dir) > 0.99f && l.key_dir.z > 0); // from in front
		CHECK(!near3(l.key, studio.key, 0.05f));
	}
	const Lights night = preset_lights(LIGHTS_NIGHT), cozy = preset_lights(LIGHTS_COZY),
		     day = preset_lights(LIGHTS_DAYLIGHT), neon = preset_lights(LIGHTS_NEON);
	CHECK(brightness(night.key) < brightness(studio.key) * 0.6f && night.key.z > night.key.x); // dim, blue
	CHECK(brightness(night.room_up) < brightness(studio.room_up) * 0.4f);
	CHECK(cozy.key.x > cozy.key.z * 2); // warm
	CHECK(day.key_dir.x > 0 && cozy.key_dir.x < 0);
	CHECK(neon.rim.z > neon.rim.x && neon.key.x > neon.key.y); // pink light, a cyan edge
	CHECK(length(day.rim) > 0 && length(neon.rim) > length(day.rim));
	// Own terms: the way, colour and brightness as given.
	LightTerms t;
	t.side = 90;
	t.height = 0;
	t.colour = 0xFF0000;
	t.brightness = 200;
	t.rim = 0;
	const Lights red = lights_of(t);
	CHECK(near3(red.key_dir, {1, 0, 0}, 1e-5f) && near3(red.key, {2.1f, 0, 0}, 1e-5f));
	CHECK(red.fill_dir.x < 0); // the second lamp from the other side
	t.side = 9999;
	t.brightness = -50;
	const Lights silly = lights_of(t);
	CHECK(std::isfinite(silly.key_dir.x) && silly.key.x == 0);
	CHECK(colour_text(0xFFB066) == "#ffb066" && colour_text(0) == "#000000");
}

static void test_skin_parts()
{
	Model m;
	for (const char *name : {"Face_00_SKIN", "Body_00_SKIN", "EyeWhite_00_EYE", "HairBack_00_HAIR", "Wolf3D_Body",
				 "Shirt", "Head", "Headphones", "Bodysuit", "FaceBrow", "Material.001"}) {
		Material mat;
		mat.name = name;
		m.materials.push_back(mat);
	}
	const std::vector<bool> guess = skin_materials(m, "");
	const std::vector<bool> want = {true, true, false, false, true, false, true, false, false, false, false};
	CHECK(guess == want);
	// Named ones only (any case, a name a line).
	const std::vector<bool> named = skin_materials(m, " shirt \r\nMATERIAL.001\n\n");
	CHECK(named == std::vector<bool>({false, false, false, false, false, true, false, false, false, false, true}));
	CHECK(skin_materials(Model(), "").empty());
	// Ears, a neck, hands... by the whole word (a realistic VRM's "ear"),
	// not inside another (an earring, armour, a handle).
	Model w;
	for (const char *name : {"ear", "Ears_L", "earRight", "Neck", "HANDS", "Earring", "ArmorPlate", "Handle",
				 "HandGlove", "gear", "Nose01", "lipstick"}) {
		Material mat;
		mat.name = name;
		w.materials.push_back(mat);
	}
	CHECK(skin_materials(w, "") ==
	      std::vector<bool>({true, true, true, true, true, false, false, false, false, false, true, false}));
}

static void test_camera_views()
{
	Person p = make_person(false);
	auto m = std::make_shared<Model>();
	CHECK(load(p.b.glb(), *m));
	const Quat facing;
	const Framing base = frame_own_spot(*m, facing, 1000, 1000, 0.8f, false, 0, 0, 1);
	// As it's framed: nothing changes.
	{
		Framing f = base;
		view_own_spot(f, CameraView());
		CHECK(near(f.fov, base.fov, 1e-6) && near(f.seen_w, base.seen_w, 1e-6));
		CHECK(near3(point(f.view, {0.3f, 0.2f, 0.1f}), point(base.view, {0.3f, 0.2f, 0.1f}), 1e-5f));
	}
	// Twice as close: half as much of the world seen, a narrower view.
	{
		Framing f = base;
		CameraView v;
		v.zoom = 2;
		view_own_spot(f, v);
		CHECK(near(f.seen_w, base.seen_w / 2, 1e-5) && near(f.seen_h, base.seen_h / 2, 1e-5));
		CHECK(near(std::tan(f.fov / 2), std::tan(base.fov / 2) / 2, 1e-5));
	}
	// Gone round to the picture's right a quarter: its left side (the
	// world's +x) faces the camera.
	{
		Framing f = base;
		CameraView v;
		v.yaw = 90;
		view_own_spot(f, v);
		const V3 side = point(f.view, {0.5f, 0, 0}), middle = point(f.view, {0, 0, 0});
		CHECK(near(side.z - middle.z, 0.5, 1e-4) && near(side.x - middle.x, 0, 1e-4));
		// Moving with you now moves it across the picture still.
		const V3 before = point(f.view, point(f.place, {0, 0, 0}));
		move_own_spot(f, 0.1f, 0);
		const V3 after = point(f.view, point(f.place, {0, 0, 0}));
		CHECK(near(after.x - before.x, 0.1 * f.seen_w, 1e-4) && near(after.z - before.z, 0, 1e-4));
	}
	// From above: the top of it nearer the camera.
	{
		Framing f = base;
		CameraView v;
		v.pitch = 30;
		view_own_spot(f, v);
		CHECK(point(f.view, {0, 0.5f, 0}).z > point(f.view, {0, -0.5f, 0}).z);
	}
	// Sat further right and down in the picture.
	{
		Framing f = base;
		CameraView v;
		v.x = 0.25f;
		v.y = 0.1f;
		v.zoom = 1.5f;
		view_own_spot(f, v);
		const V3 c = point(f.view, {0, 0, 0});
		CHECK(near(c.x, 0.25 * f.seen_w, 1e-4) && near(c.y, -0.1 * f.seen_h, 1e-4));
	}
	// Kept sensible, and gliding between two.
	{
		CameraView silly;
		silly.yaw = 500;
		silly.pitch = -90;
		silly.zoom = 0;
		silly.x = std::nanf("");
		const CameraView s = sensible_view(silly);
		CHECK(s.yaw == 120 && s.pitch == -45 && near(s.zoom, 0.3, 1e-6) && s.x == 0);
		CameraView a, b;
		b.yaw = 40;
		b.zoom = 4;
		const CameraView half = view_between(a, b, 0.5f);
		CHECK(near(half.yaw, 20, 1e-5) && near(half.zoom, 2, 1e-4)); // growing evenly: 1, 2, 4
		CHECK(near(view_between(a, b, 1).zoom, 4, 1e-5) && near(view_between(a, b, -1).yaw, 0, 1e-6));
	}
}

// --- Tune your face, alive when you're still, the camera check ---------------

static void test_tune()
{
	CHECK(tune_default(TUNE_BLINK, false) == 20 && tune_default(TUNE_BLINK, true) == 65);
	CHECK(tune_default(TUNE_MOUTH, true) == 50 && tune_default(-1, true) == 100 && tune_default(99, false) == 0);
	CHECK(std::string(TUNE_NAMES[TUNE_BROWS_DOWN]) == "brows_down" && std::string(TUNE_NAMES[TUNE_LOOK]) == "look");
	const int bl = move_of_name("eyeBlinkLeft"), br = move_of_name("eyeBlinkRight"), jaw = move_of_name("jawOpen"),
		  sl = move_of_name("mouthSmileLeft"), sr = move_of_name("mouthSmileRight"),
		  funnel = move_of_name("mouthFunnel"), pucker = move_of_name("mouthPucker"),
		  inner = move_of_name("browInnerUp"), outer_l = move_of_name("browOuterUpLeft"),
		  outer_r = move_of_name("browOuterUpRight"), down_l = move_of_name("browDownLeft"),
		  down_r = move_of_name("browDownRight"), look_in = move_of_name("eyeLookInLeft"),
		  look_down = move_of_name("eyeLookDownRight"), cheek = move_of_name("cheekPuff");

	// What the camera reads for each group.
	FaceTune t;
	float mv[EXPRESSIONS] = {}, read[TUNE_GROUPS], out[EXPRESSIONS];
	mv[bl] = 0.6f, mv[br] = 0.2f, mv[jaw] = 0.3f, mv[sl] = 0.4f, mv[sr] = 0.2f, mv[funnel] = 0.1f;
	mv[pucker] = 0.5f, mv[inner] = 0.2f, mv[outer_l] = 0.6f, mv[outer_r] = 0.4f, mv[down_l] = 0.3f;
	mv[down_r] = 0.1f, mv[look_in] = 0.45f, mv[look_down] = 0.3f, mv[cheek] = 0.4f;
	tune_readings(mv, t, read);
	CHECK(near(read[TUNE_BLINK], 0.4, 1e-6)); // both eyes together
	CHECK(near(read[TUNE_MOUTH], 0.3, 1e-6) && near(read[TUNE_SMILE], 0.3, 1e-6));
	CHECK(near(read[TUNE_ROUND], 0.5, 1e-6) && near(read[TUNE_BROWS_UP], 0.5, 1e-6));
	CHECK(near(read[TUNE_BROWS_DOWN], 0.2, 1e-6) && near(read[TUNE_LOOK], 0.45, 1e-6));
	t.linked_blink = false;
	tune_readings(mv, t, read);
	CHECK(near(read[TUNE_BLINK], 0.6, 1e-6)); // the more shut eye

	// Each from where it starts (0) to where it's full (1); moves
	// outside the groups as they are.
	t = FaceTune();
	t.open_when_smiling = false;
	float m2[EXPRESSIONS] = {};
	m2[jaw] = 0.05f + 0.45f * 0.5f;
	m2[cheek] = 0.4f;
	m2[funnel] = 0.1f; // under where it starts
	m2[pucker] = 0.9f; // past full
	tune_moves(m2, t, out);
	CHECK(near(out[jaw], 0.5, 1e-5) && near(out[cheek], 0.4, 1e-6) && out[funnel] == 0 && out[pucker] == 1);
	tune_moves(m2, t, m2); // in place, the same
	CHECK(near(m2[jaw], 0.5, 1e-5) && m2[pucker] == 1 && near(m2[cheek], 0.4, 1e-6));

	// One eye reads shut and the other doesn't: no half-wink, linked.
	float m3[EXPRESSIONS] = {};
	m3[bl] = 0.65f, m3[br] = 0.2f;
	tune_moves(m3, t, out);
	CHECK(near(out[bl], out[br], 1e-6) && near(out[bl], (0.425 - 0.2) / 0.45, 1e-5));
	t.linked_blink = false;
	tune_moves(m3, t, out);
	CHECK(near(out[bl], 1, 1e-6) && out[br] == 0);

	// A big smile pushes the cheeks up and reads as the eyes shutting:
	// they stay open - and a real blink still shuts them.
	t = FaceTune();
	float m4[EXPRESSIONS] = {};
	m4[bl] = m4[br] = 0.5f;
	tune_moves(m4, t, out);
	const float plain = out[bl];
	m4[sl] = m4[sr] = 0.7f;
	tune_moves(m4, t, out);
	CHECK(plain > 0.6f && out[bl] < 0.3f && out[br] < 0.3f);
	m4[bl] = m4[br] = 0.7f;
	tune_moves(m4, t, out);
	CHECK(out[bl] == 1 && out[br] == 1);
	t.open_when_smiling = false;
	m4[bl] = m4[br] = 0.5f;
	tune_moves(m4, t, out);
	CHECK(near(out[bl], plain, 1e-6));

	// The eyes as far as they're set to move.
	t = FaceTune();
	float m5[EXPRESSIONS] = {};
	m5[look_in] = 0.4f; // (0.4 - 0.1) / 0.6: halfway
	tune_moves(m5, t, out);
	CHECK(near(out[look_in], 0.5, 1e-5));
	t.eye_move = 2;
	tune_moves(m5, t, out);
	CHECK(near(out[look_in], 1, 1e-5));
	t.eye_move = 0;
	tune_moves(m5, t, out);
	CHECK(out[look_in] == 0);
	t.eye_move = NAN;
	tune_moves(m5, t, out);
	CHECK(near(out[look_in], 0.5, 1e-5));

	// Ranges the wrong way round, or not numbers: full just past where it
	// starts, never a NaN.
	t = FaceTune();
	t.range[TUNE_MOUTH] = {0.8f, 0.3f};
	float m6[EXPRESSIONS] = {};
	m6[jaw] = 0.81f;
	tune_moves(m6, t, out);
	CHECK(near(out[jaw], 0.5, 1e-3));
	t.range[TUNE_MOUTH] = {NAN, 2};
	t.range[TUNE_BLINK] = {INFINITY, -INFINITY};
	m6[bl] = m6[br] = 0.99f;
	tune_moves(m6, t, out);
	bool fine = true;
	for (int i = 0; i < EXPRESSIONS; i++)
		fine = fine && std::isfinite(out[i]) && out[i] >= 0 && out[i] <= 1;
	CHECK(fine && near(out[jaw], 0.81, 1e-5));
}

static void test_alive()
{
	const int bl = move_of_name("eyeBlinkLeft"), br = move_of_name("eyeBlinkRight"), jaw = move_of_name("jawOpen");
	float none[EXPRESSIONS] = {}, face[EXPRESSIONS] = {};
	face[jaw] = 0.6f;
	const float dt = 1.0f / 60;

	// A breath: in for half of it, out for the other half.
	{
		Alive a;
		AliveOptions o;
		o.breath = 1;
		o.breath_time = 4;
		o.sway = 0;
		o.eye_moves = false;
		a.set(o);
		float most = 0, last = 1, most_at = 0;
		for (int i = 1; i <= 240; i++) {
			const AliveOut out = a.step(dt, true, 0, 0, 0, none);
			if (out.breath > most)
				most = out.breath, most_at = i * dt;
			last = out.breath;
			CHECK(i < 240 || (out.sway_yaw == 0 && out.body_yaw == 0 && out.body_roll == 0));
		}
		CHECK(most > 0.999f && near(most_at, 2, 0.05) && last < 0.01f);
		o.breath = 0.5f;
		a.set(o);
		for (int i = 0; i < 120; i++)
			most = std::max(most, a.step(dt, true, 0, 0, 0, none).breath * 2);
		CHECK(most <= 1.0001f);
		o.breath = 0;
		a.set(o);
		CHECK(a.step(dt, true, 0, 0, 0, none).breath == 0);
	}

	// Following you; your last look held a moment when it can't see you,
	// then easing into idle; back with you in a moment.
	{
		Alive a;
		AliveOptions o;
		o.sway = 0;
		o.breath = 0;
		o.eye_moves = false;
		a.set(o);
		AliveOut out = a.step(dt, false, 0, 0, 0, none);
		CHECK(out.idle == 1); // nothing seen yet: idle
		for (int i = 0; i < 60; i++)
			out = a.step(dt, true, 20, -10, 5, face);
		CHECK(out.idle == 0 && near(out.yaw, 20, 1e-4) && near(out.pitch, -10, 1e-4) && near(out.roll, 5, 1e-4));
		CHECK(near(out.moves[jaw], 0.6, 1e-6) && out.moves[bl] == 0);
		for (int i = 0; i < 54; i++) // 0.9 s
			out = a.step(dt, false, 0, 0, 0, none);
		CHECK(out.idle == 0 && near(out.yaw, 20, 1e-4) && near(out.moves[jaw], 0.6, 1e-6));
		for (int i = 0; i < 51; i++) // 1.75 s: halfway through easing
			out = a.step(dt, false, 0, 0, 0, none);
		CHECK(near(out.idle, 0.5, 0.03) && near(out.yaw, 10, 0.6) && near(out.moves[jaw], 0.3, 0.02));
		for (int i = 0; i < 46; i++) // 2.52 s
			out = a.step(dt, false, 0, 0, 0, none);
		CHECK(out.idle == 1 && out.yaw == 0 && out.pitch == 0 && out.moves[jaw] == 0);
		for (int i = 0; i < 18; i++) // 0.3 s
			out = a.step(dt, true, -15, 0, 0, face);
		CHECK(out.idle < 0.1f && near(out.yaw, -15 * (1 - out.idle), 1e-3));
		// Lost again while still coming back: no jump.
		AliveOut was = out;
		out = a.step(dt, false, 0, 0, 0, none);
		CHECK(near(out.idle, was.idle, 1e-6) && near(out.yaw, was.yaw, 0.2));
	}

	// Idle: its own blinks (3 to 7 seconds apart, now and then twice),
	// looking about and its eyes' small moves - the same each time from
	// the same start, and within their ranges.
	{
		Alive a, b;
		int blinks = 0;
		bool shut = false, same = true, ranged = true;
		for (int i = 0; i < 60 * 60; i++) {
			const AliveOut x = a.step(dt, false, 0, 0, 0, none), y = b.step(dt, false, 0, 0, 0, none);
			same = same && x.moves[bl] == y.moves[bl] && x.eye_yaw == y.eye_yaw && x.sway_yaw == y.sway_yaw &&
			       x.breath == y.breath;
			ranged = ranged && x.moves[bl] >= 0 && x.moves[bl] <= 1 && x.moves[bl] == x.moves[br] &&
				 std::fabs(x.eye_yaw) <= 8 && x.eye_pitch >= -4 && x.eye_pitch <= 2 &&
				 std::fabs(x.sway_yaw) <= 7.01f && std::fabs(x.sway_pitch) <= 3.51f &&
				 std::fabs(x.sway_roll) <= 4.01f && std::fabs(x.body_yaw) <= 2.41f &&
				 std::fabs(x.body_roll) <= 1.61f && x.breath >= 0 && x.breath <= 0.5f;
			if (!shut && x.moves[bl] > 0.9f)
				blinks++, shut = true;
			if (x.moves[bl] < 0.1f)
				shut = false;
		}
		CHECK(same && ranged);
		std::printf("unit_model: %d blinks in a minute, idle\n", blinks);
		CHECK(blinks >= 8 && blinks <= 30);
	}

	// While it sees you: its eyes' small moves are small, and it doesn't
	// blink by itself - unless asked (a camera that misses blinks).
	{
		Alive a;
		AliveOptions o;
		a.set(o);
		int shut = 0;
		bool small = true;
		for (int i = 0; i < 30 * 60; i++) {
			const AliveOut x = a.step(dt, true, 0, 0, 0, none);
			shut += x.moves[bl] > 0.5f;
			if (i > 90) // (once it's back with you)
				small = small && std::fabs(x.eye_yaw) <= 1.501f && x.eye_pitch >= -1.001f &&
					x.eye_pitch <= 0.501f &&
					std::fabs(x.sway_yaw) <= 0.81f;
		}
		CHECK(shut == 0);
		CHECK(small);
		o.auto_blink = true;
		a.set(o);
		for (int i = 0; i < 30 * 60; i++)
			shut += a.step(dt, true, 0, 0, 0, none).moves[bl] > 0.5f;
		CHECK(shut > 0);
		// Its eyes' small moves off: back to straight ahead.
		o.eye_moves = false;
		a.set(o);
		AliveOut x;
		for (int i = 0; i < 60; i++)
			x = a.step(dt, true, 0, 0, 0, none);
		CHECK(std::fabs(x.eye_yaw) < 0.01f && std::fabs(x.eye_pitch) < 0.01f);
		// A frame time that's no number moves nothing on.
		x = a.step(NAN, true, 0, 0, 0, none);
		CHECK(std::isfinite(x.breath) && std::isfinite(x.sway_yaw) && std::isfinite(x.eye_yaw));
	}

	// Its eyes' small moves as the eyes' looks: 15 degrees towards its
	// left is its left eye half out and its right eye half in.
	float looks[EXPRESSIONS] = {};
	add_eye_turn(looks, 15, -12.5f);
	CHECK(near(looks[move_of_name("eyeLookOutLeft")], 0.5, 1e-6) &&
	      near(looks[move_of_name("eyeLookInRight")], 0.5, 1e-6) && looks[move_of_name("eyeLookInLeft")] == 0);
	CHECK(near(looks[move_of_name("eyeLookDownLeft")], 0.5, 1e-6) &&
	      near(looks[move_of_name("eyeLookDownRight")], 0.5, 1e-6) && looks[move_of_name("eyeLookUpLeft")] == 0);
	add_eye_turn(looks, -60, 0);
	CHECK(looks[move_of_name("eyeLookInLeft")] == 1 && looks[move_of_name("eyeLookOutRight")] == 1);
}

static void test_light()
{
	// A picture 64 x 48: the face's box (16-48 across, 8-40 down) lit
	// brighter on the picture's left than its right, the room dim.
	constexpr int W = 64, H = 48;
	std::vector<uint8_t> px(W * H * 4, 0);
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			const bool in = x >= 16 && x < 48 && y >= 8 && y < 40;
			const uint8_t v = !in ? 30 : x < 32 ? 200 : 100;
			uint8_t *p = &px[(y * W + x) * 4];
			p[0] = p[1] = p[2] = v;
			p[3] = 255;
		}
	Face face;
	face.found = true;
	// Its points at twice the picture's size (as the tracker's can be).
	face.w = W * 2;
	face.h = H * 2;
	for (Point &p : face.points)
		p = {64, 48, 0};
	face.points[0] = {32, 16, 0};
	face.points[1] = {96, 80, 0};
	const Frame frame{px.data(), W, H, W * 4, false};
	const Light l = light_of(frame, face);
	CHECK(l.seen && near(l.left, 200 / 255.0, 1e-3) && near(l.right, 100 / 255.0, 1e-3));
	CHECK(near(l.face, 150 / 255.0, 1e-3) && near(l.room, 30 / 255.0, 1e-3));
	// Blue first, the same (grey is grey).
	const Frame bgra{px.data(), W, H, W * 4, true};
	CHECK(near(light_of(bgra, face).face, l.face, 1e-6));
	face.found = false;
	CHECK(!light_of(frame, face).seen);
	face.found = true;
	CHECK(!light_of(Frame{}, face).seen);
}

// A person with a body: its spine and chest under its neck, shoulders,
// arms out in a T and hands with fingers held straight out, palms down
// (VRM 1.0's bones). `bent`: its left first finger's knuckle already
// bent this far towards its palm (degrees), as some files have them.
struct Figure {
	Person p;
	int spine, chest, hand[2], index[2][3], middle[2][3], thumb[2][3];
};
static Figure make_body(float bent = 0)
{
	Figure o;
	o.p = make_person(false);
	Person &p = o.p;
	Builder &b = p.b;
	json &bones = b.doc["extensions"]["VRMC_vrm"]["humanoid"]["humanBones"];
	o.spine = b.node({{"name", "spine"}, {"translation", {0, 0.15f, 0}}});
	o.chest = b.node({{"name", "chest"}, {"translation", {0, 0.2f, 0}}});
	b.doc["nodes"][p.neck]["translation"] = {0, 0.15f, 0};
	b.doc["nodes"][p.hips]["children"] = {o.spine};
	b.doc["nodes"][o.spine]["children"] = {o.chest};
	b.doc["nodes"][o.chest]["children"] = {p.neck};
	bones["spine"] = {{"node", o.spine}};
	bones["chest"] = {{"node", o.chest}};
	const float PI = 3.14159265f;
	for (int side = 0; side < 2; side++) {
		const float sx = side ? -1.0f : 1.0f;
		const std::string s = side ? "right" : "left", tag = side ? ".R" : ".L";
		auto add = [&](int parent, const std::string &name, V3 at) {
			const int n = b.node({{"name", name + tag}, {"translation", {at.x, at.y, at.z}}});
			b.doc["nodes"][parent]["children"].push_back(n);
			return n;
		};
		const int shoulder = add(o.chest, "shoulder", {sx * 0.05f, 0.13f, 0});
		const int upper = add(shoulder, "upper_arm", {sx * 0.05f, 0, 0});
		const int lower = add(upper, "lower_arm", {sx * 0.25f, 0, 0});
		const int hand = o.hand[side] = add(lower, "hand", {sx * 0.25f, 0, 0});
		bones[s + "Shoulder"] = {{"node", shoulder}};
		bones[s + "UpperArm"] = {{"node", upper}};
		bones[s + "LowerArm"] = {{"node", lower}};
		bones[s + "Hand"] = {{"node", hand}};
		const struct {
			const char *name;
			float z;
		} digits[] = {{"Index", 0.02f}, {"Middle", 0}, {"Ring", -0.02f}, {"Little", -0.04f}};
		for (const auto &d : digits) {
			int j[3];
			j[0] = add(hand, std::string(d.name) + "1", {sx * 0.03f, 0, d.z});
			j[1] = add(j[0], std::string(d.name) + "2", {sx * 0.02f, 0, 0});
			j[2] = add(j[1], std::string(d.name) + "3", {sx * 0.02f, 0, 0});
			const char *joint[3] = {"Proximal", "Intermediate", "Distal"};
			for (int k = 0; k < 3; k++)
				bones[s + d.name + joint[k]] = {{"node", j[k]}};
			if (std::strcmp(d.name, "Index") == 0)
				std::copy(j, j + 3, o.index[side]);
			if (std::strcmp(d.name, "Middle") == 0)
				std::copy(j, j + 3, o.middle[side]);
		}
		int *t = o.thumb[side];
		t[0] = add(hand, "thumb1", {sx * 0.01f, 0, 0.03f});
		t[1] = add(t[0], "thumb2", {sx * 0.015f, 0, 0.015f});
		t[2] = add(t[1], "thumb3", {sx * 0.015f, 0, 0.015f});
		bones[s + "ThumbMetacarpal"] = {{"node", t[0]}};
		bones[s + "ThumbProximal"] = {{"node", t[1]}};
		bones[s + "ThumbDistal"] = {{"node", t[2]}};
	}
	if (bent != 0) {
		// Towards its left palm (down) about -z.
		const Quat q = axis_angle({0, 0, -1}, bent * PI / 180);
		b.doc["nodes"][o.index[0][0]]["rotation"] = {q.x, q.y, q.z, q.w};
	}
	return o;
}

// How far a joint is bent from the bone before it (degrees), and
// whether towards its palm.
static float bend_of(const Rig &rig, int before, int joint, int side, bool *to_palm = nullptr)
{
	const float sx = side ? -1.0f : 1.0f;
	const V3 a = normalized(direction(rig.world()[before], {sx, 0, 0})),
		 b = normalized(direction(rig.world()[joint], {sx, 0, 0}));
	if (to_palm)
		*to_palm = dot(b, normalized(direction(rig.world()[before], {0, -1, 0}))) > 1e-4f;
	return std::acos(std::min(1.0f, std::max(-1.0f, dot(a, b)))) * 180 / 3.14159265f;
}

// A VRM animation file (.vrma): a T-posed skeleton whose bones are each
// turned their own way at rest (as files from other tools are), keys
// given as the turns from rest they mean (`turns`, VRM's way: in the
// world), its hips moved to `hips_at` over a second, and its "happy" on.
struct MotionBones {
	std::vector<std::string> names;
	std::map<std::string, int> node;
	std::map<std::string, Quat> world_turn; // each bone's own turn at rest, in the world
};
static std::vector<uint8_t> make_motion(const std::map<std::string, Quat> &turns, V3 hips_at = {0, 1, 0},
					float happy = -1, bool with_ext = true, bool with_hips = true,
					MotionBones *out = nullptr)
{
	Builder b;
	struct B {
		const char *name, *parent;
		V3 at;
	};
	// Where each is in the world, standing in a T (1 m hips), and the
	// file's own odd turn for each at rest.
	const B bones[] = {{"hips", "", {0, 1, 0}},
			   {"spine", "hips", {0, 1.1f, 0}},
			   {"chest", "spine", {0, 1.25f, 0}},
			   {"upperChest", "chest", {0, 1.35f, 0}},
			   {"neck", "upperChest", {0, 1.45f, 0}},
			   {"head", "neck", {0, 1.55f, 0}},
			   {"leftShoulder", "upperChest", {0.05f, 1.42f, 0}},
			   {"leftUpperArm", "leftShoulder", {0.12f, 1.42f, 0}},
			   {"leftLowerArm", "leftUpperArm", {0.4f, 1.42f, 0}},
			   {"rightShoulder", "upperChest", {-0.05f, 1.42f, 0}},
			   {"rightUpperArm", "rightShoulder", {-0.12f, 1.42f, 0}},
			   {"rightLowerArm", "rightUpperArm", {-0.4f, 1.42f, 0}}};
	const float PI = 3.14159265f;
	std::map<std::string, Quat> W;
	std::map<std::string, V3> at;
	MotionBones mb;
	int k = 0;
	for (const B &bn : bones) {
		// An odd turn of its own.
		const Quat w = with_hips || std::string(bn.name) != "hips"
				       ? normalized(axis_angle(normalized(V3{0.3f + k * 0.1f, 1, -0.2f * k}),
							       (20 + 13 * k) * PI / 180))
				       : Quat{};
		k++;
		W[bn.name] = w;
		at[bn.name] = bn.at;
		const bool root = !*bn.parent;
		const Quat pw = root ? Quat{} : W[bn.parent];
		const V3 pa = root ? V3{} : at[bn.parent];
		const Quat local = normalized(conj(pw) * w);
		const V3 t = rotate(conj(pw), bn.at - pa);
		const int n = b.node({{"name", std::string("J_") + bn.name},
				      {"translation", {t.x, t.y, t.z}},
				      {"rotation", {local.x, local.y, local.z, local.w}}});
		mb.node[bn.name] = n;
		mb.names.push_back(bn.name);
		if (!root)
			b.doc["nodes"][mb.node[bn.parent]]["children"].push_back(n);
	}
	mb.world_turn = W;
	const int face = b.node({{"name", "happy"}});
	json human = json::object();
	for (const auto &nn : mb.node)
		if (with_hips || nn.first != "hips")
			human[nn.first] = {{"node", nn.second}};
	json samplers = json::array(), channels = json::array();
	const int times = b.floats({0, 1}, 1, "SCALAR");
	for (const auto &t : turns) {
		std::string parent = "";
		for (const B &bn : bones)
			if (t.first == bn.name)
				parent = bn.parent;
		const Quat P = parent.empty() ? Quat{} : W[parent];
		// From rest to the turn over the second.
		const Quat q0 = normalized(conj(P) * W[t.first]);
		const Quat q1 = normalized(conj(P) * t.second * W[t.first]);
		samplers.push_back({{"input", times},
				    {"output", b.floats({q0.x, q0.y, q0.z, q0.w, q1.x, q1.y, q1.z, q1.w}, 4, "VEC4")}});
		channels.push_back({{"sampler", samplers.size() - 1},
				    {"target", {{"node", mb.node[t.first]}, {"path", "rotation"}}}});
	}
	samplers.push_back({{"input", times}, {"output", b.floats({0, 1, 0, hips_at.x, hips_at.y, hips_at.z}, 3, "VEC3")}});
	channels.push_back({{"sampler", samplers.size() - 1}, {"target", {{"node", mb.node["hips"]}, {"path", "translation"}}}});
	if (happy >= 0) {
		samplers.push_back({{"input", times}, {"output", b.floats({0, 0, 0, happy, 0, 0}, 3, "VEC3")}});
		channels.push_back({{"sampler", samplers.size() - 1}, {"target", {{"node", face}, {"path", "translation"}}}});
	}
	b.doc["animations"] = json::array({{{"name", "Test move"}, {"samplers", samplers}, {"channels", channels}}});
	if (with_ext)
		b.doc["extensions"]["VRMC_vrm_animation"] = {
			{"specVersion", "1.0"},
			{"humanoid", {{"humanBones", human}}},
			{"expressions", {{"preset", {{"happy", {{"node", face}}}}}}}};
	if (out)
		*out = mb;
	return b.glb();
}

// 0.8.1: "All of it" framed as it stands - its arms down by its sides,
// not as wide as its arms reach in the T it was made in - with a little
// room round it; a VRM's head and shoulders as before; Size up to 3.
static void test_framed_as_it_stands()
{
	Figure body = make_body();
	Builder &b = body.p.b;
	// A box on each hand, out at the ends of its arms in the T, on its
	// hand bone.
	const float hand_x = 0.6f, hand_y = 1.48f;
	std::vector<V3> pos, other;
	std::vector<uint16_t> ix, ix_other;
	cube(pos, ix, {hand_x, hand_y, 0}, 0.05f);
	cube(other, ix_other, {-hand_x, hand_y, 0}, 0.05f);
	for (uint16_t i : ix_other)
		ix.push_back((uint16_t)(i + 8));
	pos.insert(pos.end(), other.begin(), other.end());
	std::vector<uint8_t> joints;
	std::vector<float> weights;
	for (int i = 0; i < 16; i++) {
		joints.insert(joints.end(), {(uint8_t)(i < 8 ? 0 : 1), 0, 0, 0});
		weights.insert(weights.end(), {1, 0, 0, 0});
	}
	const int a_pos = b.vec3s(pos), a_ix = b.indices(ix);
	const int a_j = b.accessor(b.view(joints.data(), joints.size()), 5121, 16, "VEC4");
	const int a_w = b.floats(weights, 4, "VEC4");
	json mesh;
	mesh["primitives"] = json::array(
		{{{"attributes", {{"POSITION", a_pos}, {"JOINTS_0", a_j}, {"WEIGHTS_0", a_w}}}, {"indices", a_ix}}});
	b.doc["meshes"].push_back(mesh);
	const std::vector<float> ibm = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -hand_x, -hand_y, 0, 1,
					1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, hand_x,  -hand_y, 0, 1};
	b.doc["skins"].push_back(
		{{"joints", {body.hand[0], body.hand[1]}}, {"inverseBindMatrices", b.floats(ibm, 16, "MAT4")}});
	const int hands = b.node({{"name", "hands"},
				  {"mesh", (int)b.doc["meshes"].size() - 1},
				  {"skin", (int)b.doc["skins"].size() - 1}});
	b.doc["scenes"][0]["nodes"].push_back(hands);
	auto m = std::make_shared<Model>();
	CHECK(load(b.glb(), *m));
	// Its file's box: as wide as its arms reach (1.3), from its hands to
	// the top of its head (0.37 tall).
	CHECK(near(m->high.x - m->low.x, 1.3, 1e-4) && near(m->high.y - m->low.y, 0.37, 1e-4));

	// As it stands: its hands hang by its sides, below its hips' height.
	RigOptions o;
	o.springs = false;
	Rig rig(m, o);
	const Bounds s = rig.rest_bounds();
	CHECK(s.found);
	CHECK(s.high.x < 0.35f && s.low.x > -0.35f);
	CHECK(s.low.y < 1.0f && s.low.y > 0.9f);
	CHECK(near(s.high.y, 1.8, 1e-3));
	// ...and the same again after it's been posed.
	rig.pose(still());
	CHECK(rig.rest_bounds().low.y == s.low.y);
	// One left in its T is as wide as ever.
	RigOptions in_t = o;
	in_t.arms_down = false;
	Rig t_rig(m, in_t);
	CHECK(near(t_rig.rest_bounds().high.x, 0.65, 1e-3));

	// Framed: its box as it stands, with room round it.
	V3 middle, extent;
	framed_box(*m, Quat{}, false, &s, middle, extent);
	const float room = 0.05f * (s.high.y - s.low.y);
	CHECK(near(extent.x, s.high.x - s.low.x + room, 1e-4) && near(extent.y, s.high.y - s.low.y + room, 1e-4));
	CHECK(near(middle.y, (s.high.y + s.low.y) / 2, 1e-4));
	// Without it (or not found): its file's box, as before 0.8.1.
	framed_box(*m, Quat{}, false, nullptr, middle, extent);
	CHECK(near(extent.x, 1.3, 1e-4) && near(extent.y, 0.37, 1e-4));
	const Bounds none;
	framed_box(*m, Quat{}, false, &none, middle, extent);
	CHECK(near(extent.x, 1.3, 1e-4));
	// Head and shoulders: half a metre round its head, either way.
	framed_box(*m, Quat{}, true, &s, middle, extent);
	CHECK(near(extent.x, 0.5, 1e-6) && near(extent.y, 0.5, 1e-6));

	// In a square spot at 100%: as tall as the spot (with its room),
	// where its T made it a quarter of that.
	const Framing stood = frame_own_spot(*m, Quat{}, 1000, 1000, 1, false, 0, 0, 1, &s);
	const Framing in_its_t = frame_own_spot(*m, Quat{}, 1000, 1000, 1, false, 0, 0, 1);
	CHECK(near(stood.height, 1, 1e-5));
	CHECK(in_its_t.height < 0.3f);
	// Twice the size: the camera nearer, about half as much seen; up to 3.
	const Framing twice = frame_own_spot(*m, Quat{}, 1000, 1000, 2, false, 0, 0, 1, &s);
	CHECK(twice.seen_h < stood.seen_h * 0.6f);
	const Framing most = frame_own_spot(*m, Quat{}, 1000, 1000, 3, false, 0, 0, 1, &s);
	CHECK(near(frame_own_spot(*m, Quat{}, 1000, 1000, 9, false, 0, 0, 1, &s).seen_h, most.seen_h, 1e-6));
	CHECK(most.seen_h < twice.seen_h);
	// Bigger than its spot, its top's kept in the picture (just under its
	// top edge) and it's cut off at the bottom: head and chest, not its
	// middle. (Up the picture: +1 at its top edge, -1 at its bottom.)
	auto up_the_picture = [&](const Framing &f, float y) {
		const V3 p = point(f.view * f.place, {0, y, 0});
		return p.y / (-p.z * std::tan(f.fov / 2));
	};
	for (const Framing *f : {&stood, &twice, &most}) {
		const float top = up_the_picture(*f, s.high.y);
		CHECK(top < 1 && top > 0.85f);
	}
	CHECK(up_the_picture(stood, s.low.y) > -1);
	CHECK(up_the_picture(twice, s.low.y) < -1);
	// Not taller than its spot (a wide one, lying down): in its middle.
	const Framing wide = frame_own_spot(*m, Quat{}, 1000, 1000, 1, false, 0, 0, 1);
	CHECK(near(up_the_picture(wide, (m->high.y + m->low.y) / 2), 0, 1e-4));
}

static void test_motion_reading()
{
	const float PI = 3.14159265f;
	const Quat up60 = axis_angle({0, 0, 1}, 60 * PI / 180);
	Motion mo;
	std::string why;
	const std::vector<uint8_t> file = make_motion({{"leftUpperArm", up60}}, {0, 1.1f, 0.05f}, 0.8f);
	CHECK(load_motion(file.data(), file.size(), "", mo, &why));
	CHECK(mo.name == "Test move");
	CHECK(near(mo.length, 1, 1e-6));
	CHECK(near3(mo.hips_rest, {0, 1, 0}, 1e-5f));
	CHECK(mo.tracks.size() == 2);
	CHECK(mo.expressions.size() == 1 && mo.expressions[0].first == "happy");
	for (const MotionTrack &t : mo.tracks) {
		float v[4];
		sample(t.channel, 1, v);
		if (t.bone == "leftUpperArm") {
			// Its odd turn at rest taken out: the turn as meant.
			const Quat q{v[0], v[1], v[2], v[3]};
			CHECK(std::fabs(std::fabs(q.x * up60.x + q.y * up60.y + q.z * up60.z + q.w * up60.w) - 1) < 1e-4f);
			sample(t.channel, 0, v);
			CHECK(std::fabs(std::fabs(v[3]) - 1) < 1e-4f); // at rest: no turn
		} else {
			CHECK(t.bone == "hips" && t.channel.path == AnimChannel::Move);
			CHECK(near3({v[0], v[1], v[2]}, {0, 1.1f, 0.05f}, 1e-5f));
		}
	}
	// Not one: plain glTF, no hips, nothing in it, a broken file.
	Motion bad;
	const std::vector<uint8_t> plain = make_motion({{"head", up60}}, {0, 1, 0}, -1, false);
	CHECK(!load_motion(plain.data(), plain.size(), "", bad, &why) && why.find("isn't a VRM animation") != std::string::npos);
	const std::vector<uint8_t> no_hips = make_motion({{"head", up60}}, {0, 1, 0}, -1, true, false);
	CHECK(!load_motion(no_hips.data(), no_hips.size(), "", bad, &why) && why.find("hips") != std::string::npos);
	const std::string junk = "not a file at all";
	CHECK(!load_motion((const uint8_t *)junk.data(), junk.size(), "", bad, &why) && !why.empty());
	std::vector<uint8_t> cut(file.begin(), file.begin() + file.size() / 2);
	CHECK(!load_motion(cut.data(), cut.size(), "", bad, &why) && !why.empty());
	CHECK(bad.tracks.empty());
	// A 3D model isn't a move, and a move isn't a 3D model.
	Model m;
	CHECK(!load(file, m, &why));
	CHECK(!load_motion(make_person(false).b.glb().data(), make_person(false).b.glb().size(), "", bad, &why));
	CHECK(std::string(humanoid_parent("leftThumbMetacarpal")) == "leftHand");
	CHECK(std::string(humanoid_parent("hips")).empty() && humanoid_parent("tail") == nullptr);
}

// The way a bone points in the world: from it to its child.
static V3 way_of(const Rig &rig, int from, int to)
{
	return normalized(move_of(rig.world()[to]) - move_of(rig.world()[from]));
}

static void test_moves_on_a_vrm()
{
	const float PI = 3.14159265f;
	Figure body = make_body();
	Builder &b = body.p.b;
	// Feet on the ground (y 0), so its hips are 1 m up as the file's are.
	{
		std::vector<V3> pos;
		std::vector<uint16_t> ix;
		cube(pos, ix, {0, 0.05f, 0}, 0.05f);
		const int a_pos = b.vec3s(pos), a_ix = b.indices(ix);
		const json prim = {{"attributes", {{"POSITION", a_pos}}}, {"indices", a_ix}};
		b.doc["meshes"].push_back({{"primitives", json::array({prim})}});
		const int feet = b.node({{"name", "feet"}, {"mesh", (int)b.doc["meshes"].size() - 1}});
		b.doc["scenes"][0]["nodes"].push_back(feet);
	}
	auto m = std::make_shared<Model>();
	CHECK(load(b.glb(), *m));
	RigOptions o;
	o.springs = false;
	Rig rig(m, o);
	CHECK(rig.plays_motions() && rig.has_arms());
	const json &bones = b.doc["extensions"]["VRMC_vrm"]["humanoid"]["humanBones"];
	auto node = [&](const char *n) { return bones[n]["node"].get<int>(); };
	// The file raises its left arm 60 degrees and twists its upper chest
	// (which this one hasn't: its neck and shoulders take the twist).
	const Quat up60 = axis_angle({0, 0, 1}, 60 * PI / 180), twist = axis_angle({0, 1, 0}, 30 * PI / 180);
	Motion mo;
	const std::vector<uint8_t> file =
		make_motion({{"leftUpperArm", up60}, {"upperChest", twist}}, {0, 1.1f, 0}, 0.8f);
	CHECK(load_motion(file.data(), file.size(), "", mo));
	auto motion = std::make_shared<const Motion>(mo);
	rig.pose(still());
	const V3 hips0 = move_of(rig.world()[node("hips")]);
	const V3 neck_way0 = direction(rig.world()[node("neck")], {0, 0, 1});
	const V3 idle = way_of(rig, node("leftUpperArm"), node("leftLowerArm")); // down by its side
	Drive d = still();
	MoveNow mv;
	mv.motion = motion;
	mv.time = 1;
	mv.amount = 1;
	d.body = {mv};
	rig.pose(d);
	// Its arm up where the file has it (the twist above it too).
	const V3 arm = way_of(rig, node("leftUpperArm"), node("leftLowerArm"));
	const V3 want = rotate(twist * up60, {1, 0, 0});
	CHECK(near3(arm, want, 2e-3f));
	CHECK(near3(direction(rig.world()[node("neck")], {0, 0, 1}), rotate(twist, neck_way0), 2e-3f));
	// Its hips up 10 cm (its hips as high as the file's), its happy on.
	CHECK(near3(move_of(rig.world()[node("hips")]) - hips0, {0, 0.1f, 0}, 2e-3f));
	CHECK(near(rig.expression_values().at("happy"), 0.8, 1e-4));
	// Half of it: half way from how it stands (its arm down by its side)
	// to the file's pose.
	d.body[0].amount = 0.5f;
	rig.pose(d);
	CHECK(near3(move_of(rig.world()[node("hips")]) - hips0, {0, 0.05f, 0}, 2e-3f));
	const V3 half = way_of(rig, node("leftUpperArm"), node("leftLowerArm"));
	const float up_idle = std::asin(idle.y), up_full = std::asin(arm.y), up_half = std::asin(half.y);
	CHECK(std::fabs(up_half - (up_idle + up_full) / 2) < 5 * PI / 180);
	// Its start: in a T, as the file starts.
	d.body[0].time = 0;
	d.body[0].amount = 1;
	rig.pose(d);
	CHECK(near3(way_of(rig, node("leftUpperArm"), node("leftLowerArm")), {1, 0, 0}, 2e-3f));
	// Nothing playing: its arms back down by its sides.
	rig.pose(still());
	CHECK(way_of(rig, node("leftUpperArm"), node("leftLowerArm")).y < -0.8f);

	// VRM 0.x (it faces -z): the same move, the same way as you see it.
	Person p0 = make_person(true);
	auto m0 = std::make_shared<Model>();
	CHECK(load(p0.b.glb(), *m0));
	RigOptions o0;
	o0.springs = false;
	o0.facing = axis_angle({0, 1, 0}, PI);
	Rig rig0(m0, o0);
	CHECK(rig0.plays_motions() && !rig0.has_arms());
	const Quat look = axis_angle({0, 1, 0}, 40 * PI / 180); // its nose towards +x (its left)
	const std::vector<uint8_t> turn_file = make_motion({{"head", look}});
	Motion head_mo;
	CHECK(load_motion(turn_file.data(), turn_file.size(), "", head_mo));
	Drive d0 = still();
	MoveNow hm;
	hm.motion = std::make_shared<const Motion>(head_mo);
	hm.time = 1;
	hm.amount = 1;
	d0.body = {hm};
	rig0.pose(d0);
	const V3 nose = rotate(o0.facing, direction(rig0.world()[p0.head], {0, 0, -1}));
	CHECK(near3(nose, {std::sin(40 * PI / 180), 0, std::cos(40 * PI / 180)}, 2e-3f));
	// ...and on a VRM 1.0, the same.
	Person p1 = make_person(false);
	auto m1 = std::make_shared<Model>();
	CHECK(load(p1.b.glb(), *m1));
	Rig rig1(m1, o);
	rig1.pose(d0);
	CHECK(near3(direction(rig1.world()[p1.head], {0, 0, 1}), {std::sin(40 * PI / 180), 0, std::cos(40 * PI / 180)},
		    2e-3f));
}

// 0.9.0: another tracker app's bones (VMC): each bone as much as it's the
// app's, carried over bones this one hasn't, a move on cue over them, its
// hips moved for its height, your head's turn against its chest, its eyes
// as the app has them.
static void test_tracked_bones()
{
	const float PI = 3.14159265f;
	Figure body = make_body();
	Builder &b = body.p.b;
	{
		std::vector<V3> pos;
		std::vector<uint16_t> ix;
		cube(pos, ix, {0, 0.05f, 0}, 0.05f);
		const int a_pos = b.vec3s(pos), a_ix = b.indices(ix);
		const json prim = {{"attributes", {{"POSITION", a_pos}}}, {"indices", a_ix}};
		b.doc["meshes"].push_back({{"primitives", json::array({prim})}});
		const int feet = b.node({{"name", "feet"}, {"mesh", (int)b.doc["meshes"].size() - 1}});
		b.doc["scenes"][0]["nodes"].push_back(feet);
	}
	auto m = std::make_shared<Model>();
	CHECK(load(b.glb(), *m));
	RigOptions o;
	o.springs = false;
	Rig rig(m, o);
	CHECK(rig.plays_motions());
	const json &bones = b.doc["extensions"]["VRMC_vrm"]["humanoid"]["humanBones"];
	auto node = [&](const char *n) {
		return bones[n]["node"].get<int>();
	};
	rig.pose(still());
	const V3 hips0 = move_of(rig.world()[node("hips")]);
	const V3 idle = way_of(rig, node("leftUpperArm"), node("leftLowerArm"));
	CHECK(idle.y < -0.8f); // down by its side
	// Its left arm up 60 and its upper chest twisted (which this one
	// hasn't: its neck and shoulders take it), as the app has them.
	const Quat up60 = axis_angle({0, 0, 1}, 60 * PI / 180), twist = axis_angle({0, 1, 0}, 30 * PI / 180);
	Drive d = still();
	d.tracked.bones = {{"leftUpperArm", up60, 1}, {"upperChest", twist, 1}};
	rig.pose(d);
	const V3 arm = way_of(rig, node("leftUpperArm"), node("leftLowerArm"));
	CHECK(near3(arm, rotate(twist * up60, {1, 0, 0}), 2e-3f));
	// (Its right arm, not sent: down by its side, turned with the chest.)
	CHECK(way_of(rig, node("rightUpperArm"), node("rightLowerArm")).y < -0.8f);
	// Half of it: half way from how it stands.
	d.tracked.bones[0].amount = 0.5f;
	d.tracked.bones[1].amount = 0;
	rig.pose(d);
	const V3 half = way_of(rig, node("leftUpperArm"), node("leftLowerArm"));
	const V3 full_flat = rotate(up60, {1, 0, 0});
	CHECK(std::fabs(std::asin(half.y) - (std::asin(idle.y) + std::asin(full_flat.y)) / 2) < 5 * PI / 180);
	// Bones with no turn that's a turn: left as they are.
	d.tracked.bones = {{"leftUpperArm", Quat{NAN, 0, 0, 1}, 1}};
	rig.pose(d);
	CHECK(way_of(rig, node("leftUpperArm"), node("leftLowerArm")).y < -0.8f);

	// A move on cue over it: the arm where the move has it.
	const Quat down30 = axis_angle({0, 0, 1}, -30 * PI / 180);
	Motion mo;
	const std::vector<uint8_t> file = make_motion({{"leftUpperArm", down30}}, {0, 1, 0});
	CHECK(load_motion(file.data(), file.size(), "", mo));
	MoveNow mv;
	mv.motion = std::make_shared<const Motion>(mo);
	mv.time = 1;
	mv.amount = 1;
	d.tracked.bones = {{"leftUpperArm", up60, 1}};
	d.body = {mv};
	rig.pose(d);
	CHECK(near3(way_of(rig, node("leftUpperArm"), node("leftLowerArm")), rotate(down30, {1, 0, 0}), 2e-3f));
	d.body.clear();

	// Its hips: moved as far for its height (1 m: as far as said), across
	// to its left (+x) and down - turned as well, as an app sends them.
	d.tracked.bones = {{"hips", axis_angle({0, 1, 0}, 10 * PI / 180), 1}};
	d.tracked.hips = {0.1f, -0.2f, 0};
	d.tracked.hips_amount = 1;
	rig.pose(d);
	CHECK(near3(move_of(rig.world()[node("hips")]) - hips0, {0.1f, -0.2f, 0}, 2e-3f));
	d.tracked.hips_amount = 0.5f;
	rig.pose(d);
	CHECK(near3(move_of(rig.world()[node("hips")]) - hips0, {0.05f, -0.1f, 0}, 2e-3f));
	// A move on cue holding its hips: where the move has them.
	d.tracked.hips_amount = 1;
	{
		Motion still_hips;
		const std::vector<uint8_t> f2 = make_motion({{"hips", Quat{}}}, {0, 1, 0});
		CHECK(load_motion(f2.data(), f2.size(), "", still_hips));
		MoveNow hold;
		hold.motion = std::make_shared<const Motion>(still_hips);
		hold.time = 1;
		hold.amount = 1;
		d.body = {hold};
		rig.pose(d);
		CHECK(near3(move_of(rig.world()[node("hips")]), hips0, 2e-3f));
		d.body.clear();
	}
	d.tracked.hips_amount = 0;
	d.tracked.bones.clear();
	rig.pose(d);
	CHECK(near3(move_of(rig.world()[node("hips")]), hips0, 1e-5f));

	// Your head from a camera turns against the world: with the app's
	// body twisted 30 degrees, your head straight on is still straight on
	// (the neck turns back by as much)...
	d.tracked.bones = {{"upperChest", twist, 1}};
	rig.pose(d);
	const V3 nose_twisted = direction(rig.world()[body.p.head], {0, 0, 1});
	CHECK(near3(nose_twisted, rotate(twist, {0, 0, 1}), 2e-3f));
	d.tracked.torso = 1;
	d.tracked.chest = twist;
	rig.pose(d);
	CHECK(near3(direction(rig.world()[body.p.head], {0, 0, 1}), {0, 0, 1}, 2e-3f));
	// ...turned 20 to its left, 20 to its left of the world.
	d.head = axis_angle({0, 1, 0}, 20 * PI / 180);
	rig.pose(d);
	CHECK(near3(direction(rig.world()[body.p.head], {0, 0, 1}),
		    {std::sin(20 * PI / 180), 0, std::cos(20 * PI / 180)}, 2e-3f));
	// Its own body doesn't follow your head as well (the app's has it).
	d.body_follow = 1;
	d.body_yaw = 10;
	rig.pose(d);
	CHECK(near3(direction(rig.world()[body.spine], {0, 0, 1}), {0, 0, 1}, 1e-4f));
	d = still();

	// Its eyes as the app has them: our look from your face not as well.
	Person p = make_person(false);
	auto pm = std::make_shared<Model>();
	CHECK(load(p.b.glb(), *pm));
	Rig eyes(pm, o);
	Drive e = still();
	e.moves[move_of_name("eyeLookOutLeft")] = 1;
	e.moves[move_of_name("eyeLookInRight")] = 1;
	eyes.pose(e);
	const V3 looked = rotate(quat_of(eyes.world()[p.leye]), {0, 0, 1});
	CHECK(looked.x > 0.03f);                                    // (4 degrees: test_head_and_eyes)
	const Quat app_look = axis_angle({0, 1, 0}, -5 * PI / 180); // towards its right
	e.tracked.bones = {{"leftEye", app_look, 1}, {"rightEye", app_look, 1}};
	e.tracked.eyes = true;
	eyes.pose(e);
	CHECK(near3(rotate(quat_of(eyes.world()[p.leye]), {0, 0, 1}), rotate(app_look, {0, 0, 1}), 2e-3f));
	CHECK(near3(rotate(quat_of(eyes.world()[p.reye]), {0, 0, 1}), rotate(app_look, {0, 0, 1}), 2e-3f));
	// No legs under its hips (a bust: its lowest is its head): its hips
	// still step across, as far as a whole figure its size would.
	{
		Drive h = still();
		eyes.pose(h);
		const V3 at0 = move_of(eyes.world()[p.hips]);
		h.tracked.hips = {0.2f, 0, 0};
		h.tracked.hips_amount = 1;
		eyes.pose(h);
		const V3 moved = move_of(eyes.world()[p.hips]) - at0;
		CHECK(moved.x > 0.005f && moved.x < 0.2f && near(moved.y, 0, 1e-5) && near(moved.z, 0, 1e-5));
	}

	// VRM 0.x (it faces -z): the same turn, the same way as you see it.
	Person p0 = make_person(true);
	auto m0 = std::make_shared<Model>();
	CHECK(load(p0.b.glb(), *m0));
	RigOptions o0 = o;
	o0.facing = axis_angle({0, 1, 0}, PI);
	Rig rig0(m0, o0);
	Drive d0 = still();
	const Quat look = axis_angle({0, 1, 0}, 40 * PI / 180);
	d0.tracked.bones = {{"neck", look, 1}};
	rig0.pose(d0);
	const V3 nose = rotate(o0.facing, direction(rig0.world()[p0.head], {0, 0, -1}));
	CHECK(near3(nose, {std::sin(40 * PI / 180), 0, std::cos(40 * PI / 180)}, 2e-3f));
	// Not a VRM's bones: nothing.
	Builder plain;
	std::vector<V3> pos;
	std::vector<uint16_t> ix;
	cube(pos, ix);
	const json prim = {{"attributes", {{"POSITION", plain.vec3s(pos)}}}, {"indices", plain.indices(ix)}};
	plain.doc["meshes"].push_back({{"primitives", json::array({prim})}});
	plain.doc["scenes"][0]["nodes"].push_back(plain.node({{"name", "hips"}, {"mesh", 0}}));
	auto pm2 = std::make_shared<Model>();
	CHECK(load(plain.glb(), *pm2));
	Rig plain_rig(pm2, o);
	Drive dp = still();
	plain_rig.pose(dp);
	const M4 before = plain_rig.world()[0];
	dp.tracked.bones = {{"hips", up60, 1}};
	dp.tracked.hips = {0.3f, 0, 0};
	dp.tracked.hips_amount = 1;
	plain_rig.pose(dp);
	CHECK(near3(move_of(plain_rig.world()[0]), move_of(before), 1e-6f));
	CHECK(near3(direction(plain_rig.world()[0], {1, 0, 0}), direction(before, {1, 0, 0}), 1e-6f));
}

static void test_faces_from_names()
{
	// A face by VRM's name made from ARKit's shapes (for a character made
	// for them, a 2D one, the hand signs' face moves).
	float moves[EXPRESSIONS] = {};
	face_into_moves("happy", 1, moves);
	CHECK(moves[move_of_name("mouthSmileLeft")] > 0.3f && moves[move_of_name("mouthSmileRight")] > 0.3f);
	float half[EXPRESSIONS] = {};
	face_into_moves("happy", 0.5f, half);
	CHECK(near(half[move_of_name("mouthSmileLeft")], moves[move_of_name("mouthSmileLeft")] * 0.5f, 1e-5));
	// Each as much as it was or more.
	float more[EXPRESSIONS] = {};
	more[move_of_name("mouthSmileLeft")] = 1;
	face_into_moves("happy", 0.5f, more);
	CHECK(more[move_of_name("mouthSmileLeft")] == 1);
	float sad[EXPRESSIONS] = {};
	face_into_moves("sad", 1, sad);
	CHECK(sad[move_of_name("mouthFrownLeft")] > 0.2f || sad[move_of_name("browInnerUp")] > 0.2f);
	// Not one of VRM's, or none of it: nothing.
	float none[EXPRESSIONS] = {};
	face_into_moves("Heart_Eyes", 1, none);
	face_into_moves("happy", 0, none);
	face_into_moves("happy", NAN, none);
	bool all_zero = true;
	for (float v : none)
		all_zero = all_zero && v == 0;
	CHECK(all_zero);
}

static void test_wave_and_cheer()
{
	Figure body = make_body();
	auto m = std::make_shared<Model>();
	CHECK(load(body.p.b.glb(), *m));
	RigOptions o;
	o.springs = false;
	Rig rig(m, o);
	const json &bones = body.p.b.doc["extensions"]["VRMC_vrm"]["humanoid"]["humanBones"];
	auto node = [&](const std::string &n) { return bones[n]["node"].get<int>(); };
	Drive d = still();
	MoveNow w;
	w.builtin = BuiltIn::Wave;
	w.amount = 1;
	// Its right forearm up, waving side to side; its left arm still down;
	// its right fingers open (the rest stay relaxed).
	float most_x = -1, least_x = 1;
	for (float t = 0; t < 1.2f; t += 0.05f) {
		w.time = t;
		d.body = {w};
		rig.pose(d);
		const V3 fore = way_of(rig, node("rightLowerArm"), node("rightHand"));
		CHECK(fore.y > 0.75f);
		most_x = std::max(most_x, fore.x);
		least_x = std::min(least_x, fore.x);
		CHECK(way_of(rig, node("leftUpperArm"), node("leftLowerArm")).y < -0.8f);
	}
	CHECK(most_x - least_x > 0.4f);
	CHECK(bend_of(rig, body.hand[1], body.index[1][0], 1) < 1);
	CHECK(bend_of(rig, body.index[0][0], body.index[0][1], 0) > 10);
	// A cheer: both arms up.
	MoveNow c;
	c.builtin = BuiltIn::Cheer;
	c.amount = 1;
	c.time = 0.3f;
	d.body = {c};
	rig.pose(d);
	CHECK(way_of(rig, node("leftUpperArm"), node("leftLowerArm")).y > 0.5f);
	CHECK(way_of(rig, node("rightUpperArm"), node("rightLowerArm")).y > 0.5f);
	CHECK(way_of(rig, node("leftLowerArm"), node("leftHand")).y > 0.7f);
	// One over another: the cheer taking over from the wave, half way.
	w.time = 0.5f;
	c.amount = 0.5f;
	d.body = {w, c};
	rig.pose(d);
	const V3 left = way_of(rig, node("leftUpperArm"), node("leftLowerArm"));
	CHECK(left.y > -0.7f && left.y < 0.6f);
	// A character with no arms: nothing happens, nothing breaks.
	Person p = make_person(false);
	auto mp = std::make_shared<Model>();
	CHECK(load(p.b.glb(), *mp));
	Rig armless(mp, o);
	d.body = {w};
	armless.pose(d);
	CHECK(!armless.has_arms());
}

static void test_own_move_once()
{
	// Its own animation (the tail's "Wag" of test_own_animation, simpler):
	// played once at a time, as much as asked.
	Person p = make_person(false);
	Builder &b = p.b;
	const float PI = 3.14159265f;
	const Quat q40 = axis_angle({0, 1, 0}, 40 * PI / 180);
	const int t2 = b.floats({0, 1}, 1, "SCALAR");
	const int swing = b.floats({0, 0, 0, 1, q40.x, q40.y, q40.z, q40.w}, 4, "VEC4");
	b.doc["animations"] = json::array({{{"name", "Look"},
					     {"samplers", {{{"input", t2}, {"output", swing}}}},
					     {"channels", {{{"sampler", 0}, {"target", {{"node", p.head}, {"path", "rotation"}}}}}}}});
	auto m = std::make_shared<Model>();
	CHECK(load(b.glb(), *m));
	RigOptions o;
	o.springs = false;
	Rig rig(m, o);
	Drive d = still();
	MoveNow mv;
	mv.animation = 0;
	mv.time = 1;
	mv.amount = 1;
	d.body = {mv};
	rig.pose(d);
	CHECK(near3(direction(rig.world()[p.head], {0, 0, 1}), {std::sin(40 * PI / 180), 0, std::cos(40 * PI / 180)}, 1e-3f));
	d.body[0].amount = 0.5f;
	rig.pose(d);
	CHECK(near3(direction(rig.world()[p.head], {0, 0, 1}), {std::sin(20 * PI / 180), 0, std::cos(20 * PI / 180)}, 1e-3f));
	// Past its end: as it ends. Before its start: as it starts.
	d.body[0].amount = 1;
	d.body[0].time = 30;
	rig.pose(d);
	CHECK(near3(direction(rig.world()[p.head], {0, 0, 1}), {std::sin(40 * PI / 180), 0, std::cos(40 * PI / 180)}, 1e-3f));
	// One it hasn't, and odd numbers: nothing happens.
	d.body[0].animation = 5;
	rig.pose(d);
	CHECK(near3(direction(rig.world()[p.head], {0, 0, 1}), {0, 0, 1}, 1e-4f));
	d.body[0].animation = 0;
	d.body[0].time = std::numeric_limits<float>::quiet_NaN();
	rig.pose(d);
	CHECK(near3(direction(rig.world()[p.head], {0, 0, 1}), {0, 0, 1}, 1e-4f));
}

static void test_faces_on_cue()
{
	Person p = make_person(false);
	auto m = std::make_shared<Model>();
	CHECK(load(p.b.glb(), *m));
	RigOptions o;
	o.springs = false;
	Rig rig(m, o);
	const std::vector<std::string> faces = rig.faces();
	CHECK(std::find(faces.begin(), faces.end(), "happy") != faces.end());
	CHECK(std::find(faces.begin(), faces.end(), "angry") == faces.end());
	const std::vector<std::string> shapes = rig.shapes();
	CHECK(shapes.size() == 4 && shapes[0] == "Fcl_MTH_A");
	// Happy on: its "happy" (bound at half), blinks held back (it blocks
	// them) - even an "ouch".
	Drive d = still();
	d.faces = {{"happy", 1}};
	d.squint = 1;
	rig.pose(d);
	CHECK(near(rig.expression_values().at("happy"), 1, 1e-6));
	CHECK(near(rig.weights()[0][2], 0.5, 1e-5));
	CHECK(near(rig.weights()[0][1], 0, 1e-6));
	// An "ouch" alone shuts its eyes; half a face half.
	d.faces.clear();
	rig.pose(d);
	CHECK(near(rig.weights()[0][1], 1, 1e-5));
	d.squint = 0;
	d.faces = {{"happy", 0.5f}};
	rig.pose(d);
	CHECK(near(rig.weights()[0][2], 0.25, 1e-5));
	// Your own smile under a face on cue is taken over: a big smile
	// (happy 1 from you) with "happy" half on stays happy...
	const int smile_l = move_of_name("mouthSmileLeft"), smile_r = move_of_name("mouthSmileRight");
	d.moves[smile_l] = d.moves[smile_r] = 1;
	rig.pose(d);
	CHECK(near(rig.expression_values().at("happy"), 0.5, 1e-5));
	// ...a face's own shape by name, and an odd one.
	d.moves[smile_l] = d.moves[smile_r] = 0;
	d.faces = {{"Fcl_MTH_A", 0.7f}, {"nothing like it", 1}, {"", 1}, {"happy", std::numeric_limits<float>::quiet_NaN()}};
	rig.pose(d);
	CHECK(near(rig.weights()[0][0], 0.7, 1e-5));
	CHECK(near(rig.weights()[0][2], 0, 1e-6));
	// Your mouth stays yours under a face (it can still talk).
	d.faces = {{"happy", 1}};
	d.moves[move_of_name("jawOpen")] = 1;
	rig.pose(d);
	CHECK(rig.expression_values().at("aa") > 0.9f);

	// Made for ARKit: faces made from its shapes.
	GlbPart part;
	std::vector<uint16_t> ix;
	cube(part.pos, ix, {}, 0.5f);
	part.index.assign(ix.begin(), ix.end());
	const char *names[] = {"mouthSmileLeft", "mouthSmileRight", "cheekSquintLeft", "cheekSquintRight",
			       "eyeSquintLeft",  "eyeSquintRight",  "browDownLeft",    "browDownRight",
			       "eyeBlinkLeft",   "eyeBlinkRight",   "jawOpen",         "tongueOut"};
	for (const char *n : names) {
		part.morph_names.push_back(n);
		part.morphs.push_back(std::vector<V3>(part.pos.size(), V3{0, 0.1f, 0}));
	}
	auto ma = std::make_shared<Model>();
	CHECK(load(write_glb({part}), *ma));
	Rig ar(ma, o);
	CHECK(ar.arkit_parts() == 11);
	const std::vector<std::string> af = ar.faces();
	CHECK(std::find(af.begin(), af.end(), "happy") != af.end());
	CHECK(std::find(af.begin(), af.end(), "angry") == af.end()); // too few of its shapes
	CHECK(std::find(af.begin(), af.end(), "tongueOut") != af.end());
	Drive da = still();
	da.faces = {{"happy", 1}, {"tongueOut", 1}};
	ar.pose(da);
	CHECK(near(ar.weights()[0][0], 0.85, 1e-5) && near(ar.weights()[0][2], 0.55, 1e-5));
	CHECK(near(ar.weights()[0][11], 1, 1e-6));
	da.faces.clear();
	da.squint = 1;
	ar.pose(da);
	CHECK(near(ar.weights()[0][8], 1, 1e-6) && near(ar.weights()[0][9], 1, 1e-6));
	CHECK(near(ar.weights()[0][0], 0, 1e-6));
}

static void test_knocked_and_head()
{
	// A hit throws its ponytail the way it went, and it settles back.
	Person p = make_person(false);
	auto &sp = p.b.doc["extensions"]["VRMC_springBone"]["springs"][0];
	for (auto &j : sp["joints"])
		if (j.contains("stiffness"))
			j["stiffness"] = 2.0, j["gravityPower"] = 0.0;
	auto m = std::make_shared<Model>();
	CHECK(load(p.b.glb(), *m));
	Rig rig(m, RigOptions());
	Drive d = still();
	for (int i = 0; i < 240; i++)
		rig.pose(d);
	const V3 rest = move_of(rig.world()[p.tail2]);
	Drive k = d;
	k.knock = {0, 3, 0};
	rig.pose(k);
	float highest = rest.y;
	for (int i = 0; i < 20; i++) {
		rig.pose(d);
		highest = std::max(highest, move_of(rig.world()[p.tail2]).y);
	}
	CHECK(highest > rest.y + 0.02f);
	for (int i = 0; i < 400; i++)
		rig.pose(d);
	CHECK(near3(move_of(rig.world()[p.tail2]), rest, 5e-3f));
	// An odd knock: nothing breaks.
	k.knock = {std::numeric_limits<float>::quiet_NaN(), 0, 0};
	rig.pose(k);
	CHECK(std::isfinite(move_of(rig.world()[p.tail2]).x));
	// Its head: the box round what its head bone moves.
	const Rig::HeadBox &hb = rig.head_box();
	CHECK(hb.found);
	CHECK(near3(hb.low, {-0.1f, 1.6f, -0.1f}, 1e-4f) && near3(hb.high, {0.1f, 1.8f, 0.1f}, 1e-4f));
	CHECK(near(hb.top, 1.8, 1e-4));
	// VRM 0.x, facing -z: the same box, the facing way.
	Person p0 = make_person(true);
	auto m0 = std::make_shared<Model>();
	CHECK(load(p0.b.glb(), *m0));
	RigOptions o0;
	o0.facing = axis_angle({0, 1, 0}, 3.14159265f);
	Rig rig0(m0, o0);
	CHECK(rig0.head_box().found && near(rig0.head_box().high.y - rig0.head_box().low.y, 0.2, 1e-4));
	// No head bone: none.
	GlbPart part;
	std::vector<uint16_t> ix;
	cube(part.pos, ix);
	part.index.assign(ix.begin(), ix.end());
	auto mc = std::make_shared<Model>();
	CHECK(load(write_glb({part}), *mc));
	Rig rc(mc, RigOptions());
	CHECK(!rc.head_box().found && !rc.plays_motions() && !rc.has_arms());
	CHECK(rc.faces().empty());
}

// Your arms (arms.hpp) on a VRM: each part pointing where yours does, the
// elbow bending your way, the hand turned as yours (its forearm taking
// half the twist), each finger joint bent as yours, by its head where your
// hand is by yours - and as much as it follows you, the rest its own.
static void test_arms_follow()
{
	const float PI = 3.14159265f;
	Figure body = make_body();
	auto m = std::make_shared<Model>();
	CHECK(load(body.p.b.glb(), *m));
	RigOptions o;
	o.springs = false;
	Rig rig(m, o);
	CHECK(rig.follows_arms());
	auto at = [&](const Rig &r, int node) { return move_of(r.world()[node]); };
	auto way = [&](const Rig &r, int from, int to) { return normalized(at(r, to) - at(r, from)); };
	const int lower[2] = {m->bone("leftLowerArm"), m->bone("rightLowerArm")};
	const int upper[2] = {m->bone("leftUpperArm"), m->bone("rightUpperArm")};
	// Not following: its arms down by its sides as ever.
	rig.pose(still());
	const V3 own_upper = way(rig, upper[0], lower[0]);
	Drive d = still();
	d.follow_arms = true;
	rig.pose(d);
	CHECK(near3(way(rig, upper[0], lower[0]), own_upper, 1e-5f));

	// Its left arm straight down, its right out in front, bent up at the
	// elbow (its hand at its shoulder's height).
	ArmTarget &L = d.arms.side[0], &R = d.arms.side[1];
	L.weight = R.weight = 1;
	L.upper = L.fore = {0, -1, 0};
	R.upper = {0, 0, 1};
	R.fore = {0, 1, 0};
	R.hinge = normalized(cross(R.upper, R.fore));
	R.hinge_sure = 1;
	rig.pose(d);
	CHECK(near3(way(rig, upper[0], lower[0]), {0, -1, 0}, 1e-4f));
	CHECK(near3(way(rig, lower[0], body.hand[0]), {0, -1, 0}, 1e-4f));
	CHECK(near3(way(rig, upper[1], lower[1]), {0, 0, 1}, 1e-4f));
	CHECK(near3(way(rig, lower[1], body.hand[1]), {0, 1, 0}, 1e-4f));
	// Its bones keep their lengths.
	CHECK(near(length(at(rig, lower[1]) - at(rig, upper[1])), 0.25, 1e-4));
	CHECK(near(length(at(rig, body.hand[1]) - at(rig, lower[1])), 0.25, 1e-4));

	// Raised past level, its shoulder (collarbone) lifts a third of how far
	// the arm is above level, at most 25 degrees, as yours does - its arm
	// still pointing as yours. Level or lower: as it was.
	{
		const int sh[2] = {m->bone("leftShoulder"), m->bone("rightShoulder")};
		auto lifted = [&](int side) { return std::asin(way(rig, sh[side], upper[side]).y) * 180 / PI; };
		CHECK(near(lifted(1), 0, 1e-3) && near(lifted(0), 0, 1e-3));
		R.upper = normalized(V3{0, 1, 1});
		R.fore = R.upper;
		rig.pose(d);
		CHECK(near3(way(rig, upper[1], lower[1]), R.upper, 1e-4f));
		CHECK(near(lifted(1), 15, 0.1) && near(lifted(0), 0, 1e-3));
		R.upper = R.fore = {0, 1, 0};
		rig.pose(d);
		CHECK(near3(way(rig, upper[1], lower[1]), {0, 1, 0}, 1e-4f));
		CHECK(near(lifted(1), 25, 0.1));
		CHECK(near(length(at(rig, lower[1]) - at(rig, upper[1])), 0.25, 1e-4));
		// Half followed: half as far.
		R.weight = 0.5f;
		rig.pose(d);
		CHECK(near(lifted(1), 12.5, 0.1));
		R.weight = 1;
		R.upper = {0, 0, 1};
		R.fore = {0, 1, 0};
		rig.pose(d);
		CHECK(near(lifted(1), 0, 1e-3));
	}

	// Half followed: half way between its own and yours.
	L.weight = 0.5f;
	rig.pose(d);
	const V3 half = way(rig, upper[0], lower[0]);
	const float own_angle = std::acos(std::min(1.0f, dot(own_upper, V3{0, -1, 0})));
	CHECK(near(std::acos(std::min(1.0f, dot(half, V3{0, -1, 0}))), own_angle / 2, 0.02));
	L.weight = 1;

	// Its hand as yours: hanging down, fingers down, its first finger to
	// its front - and its palm turned to its body.
	L.hand = 1;
	L.along = {0, -1, 0};
	L.across = {0, 0, 1};
	rig.pose(d);
	const int knuckle = body.middle[0][0], first = body.index[0][0];
	const int little = m->bone("leftLittleProximal");
	CHECK(near3(way(rig, body.hand[0], knuckle), {0, -1, 0}, 1e-3f));
	CHECK(near3(way(rig, little, first), {0, 0, 1}, 1e-3f));
	// Turned palm forward (a quarter twist about the forearm): its hand
	// takes it all, its forearm half.
	L.across = {-1, 0, 0};
	rig.pose(d);
	CHECK(near3(way(rig, body.hand[0], knuckle), {0, -1, 0}, 1e-3f));
	CHECK(near3(way(rig, little, first), {-1, 0, 0}, 1e-3f));
	{
		// The forearm's own turn about its length, against the upper arm's.
		const V3 fa = normalized(direction(rig.world()[lower[0]], {0, 0, 1}));
		const V3 ua = normalized(direction(rig.world()[upper[0]], {0, 0, 1}));
		const float twist = std::acos(std::min(1.0f, std::max(-1.0f, dot(fa, ua)))) * 180 / PI;
		CHECK(near(twist, 45, 1));
	}
	// A wrist bent further than a wrist bends: no more than 80 degrees.
	L.across = {0, 0, 1};
	L.along = {0, 1, 0};
	rig.pose(d);
	{
		const float bent = std::acos(std::min(1.0f, std::max(-1.0f, dot(way(rig, body.hand[0], knuckle),
										      way(rig, lower[0], body.hand[0]))))) *
				   180 / PI;
		CHECK(near(bent, 80, 1));
	}
	L.along = {0, -1, 0};

	// Each finger joint bent as yours (with your hand followed).
	L.bend[1][0] = 40 * PI / 180;
	L.bend[1][1] = 70 * PI / 180;
	rig.pose(d);
	bool palm = false;
	CHECK(near(bend_of(rig, body.hand[0], body.index[0][0], 0, &palm), 40, 0.1) && palm);
	CHECK(near(bend_of(rig, body.index[0][0], body.index[0][1], 0, &palm), 70, 0.1) && palm);
	// ...not while its fingers don't follow yours: relaxed, its own way.
	d.arms.fingers = false;
	rig.pose(d);
	CHECK(near(bend_of(rig, body.hand[0], body.index[0][0], 0), 20, 0.05));
	d.arms.fingers = true;
	// ...and half as your hand fades: half way to its own.
	L.hand = 0.5f;
	rig.pose(d);
	CHECK(near(bend_of(rig, body.hand[0], body.index[0][0], 0), 30, 0.1));
	L.hand = 1;

	// By its head: its wrist where yours is by yours, in head widths from
	// the middle of its head.
	const Rig::HeadBox &hb = rig.head_box();
	CHECK(hb.found);
	const V3 mid = (hb.low + hb.high) * 0.5f;
	const float width = (hb.high.x - hb.low.x) * 0.85f;
	L.near = 1;
	L.by_head = {0.9f, -0.4f, 0.4f};
	rig.pose(d);
	const V3 wanted = mid + L.by_head * width;
	const V3 shoulder = at(rig, upper[0]);
	if (length(wanted - shoulder) < 0.49f)
		CHECK(near3(at(rig, body.hand[0]), wanted, 2e-3f));
	CHECK(near(length(at(rig, lower[0]) - shoulder), 0.25, 1e-4));
	// ...with its arm raised and its shoulder lifted too: from where its
	// shoulder is then.
	L.upper = L.fore = {0, 1, 0};
	L.by_head = {0.7f, 0.9f, 0.2f};
	rig.pose(d);
	{
		const V3 up_wanted = mid + L.by_head * width;
		const V3 lifted_at = at(rig, upper[0]);
		CHECK(lifted_at.y > shoulder.y + 0.005f);
		if (length(up_wanted - lifted_at) < 0.49f)
			CHECK(near3(at(rig, body.hand[0]), up_wanted, 2e-3f));
	}
	L.upper = L.fore = {0, -1, 0};
	L.near = 0;

	// A move on cue takes over its arms.
	d.body.clear();
	MoveNow wave;
	wave.builtin = BuiltIn::Wave;
	wave.time = 0.5f;
	wave.amount = 1;
	d.body.push_back(wave);
	rig.pose(d);
	CHECK(!near3(way(rig, upper[0], lower[0]), {0, -1, 0}, 0.05f) ||
	      !near3(way(rig, upper[1], lower[1]), {0, 0, 1}, 0.05f));
	d.body.clear();

	// Not a VRM: its arms aren't followed (they aren't known for sure).
	Figure plain = make_body();
	plain.p.b.doc["extensions"].erase("VRMC_vrm");
	plain.p.b.doc["extensionsUsed"] = json::array();
	auto mg = std::make_shared<Model>();
	if (load(plain.p.b.glb(), *mg)) {
		Rig gr(mg, o);
		CHECK(!gr.follows_arms());
	}
}

static void test_fingers_and_body()
{
	Figure body = make_body();
	auto m = std::make_shared<Model>();
	CHECK(load(body.p.b.glb(), *m));
	RigOptions o;
	o.springs = false;
	Rig rig(m, o);
	CHECK(rig.finger_joints() == 30 && rig.has_body());
	rig.pose(still());
	// Relaxed (the starting 0.5): each joint half its full curl, towards
	// the palm, on both hands; the thumb a little.
	for (int side = 0; side < 2; side++) {
		bool palm = false;
		CHECK(near(bend_of(rig, body.hand[side], body.index[side][0], side, &palm), 20, 0.05) && palm);
		CHECK(near(bend_of(rig, body.index[side][0], body.index[side][1], side, &palm), 30, 0.05) && palm);
		CHECK(near(bend_of(rig, body.middle[side][1], body.middle[side][2], side, &palm), 21, 0.05) && palm);
		// (The thumb points out along its own way, between the fingers'
		// way and the front.)
		const V3 way{side ? -1.0f : 1.0f, 0, 1};
		const V3 thumb0 = normalized(direction(rig.world()[body.thumb[side][0]], way)),
			 hand0 = normalized(direction(rig.world()[body.hand[side]], way));
		CHECK(near(std::acos(std::min(1.0f, dot(thumb0, hand0))) * 180 / 3.14159265, 7.5, 0.05) &&
		      dot(thumb0, normalized(direction(rig.world()[body.hand[side]], {0, -1, 0}))) > 0);
	}
	// As made, and curled.
	o.fingers = 0;
	Rig flat(m, o);
	flat.pose(still());
	CHECK(near(bend_of(flat, body.hand[0], body.index[0][0], 0), 0, 0.05) && flat.finger_joints() == 30);
	o.fingers = 1;
	Rig curled(m, o);
	curled.pose(still());
	CHECK(near(bend_of(curled, body.index[1][0], body.index[1][1], 1), 60, 0.05));
	// Past its first joint, the thumb bends towards the first finger,
	// across the palm (about the hand's up, as its first bone carries it),
	// not down under the hand: its middle joint 12.5 degrees relaxed, 25
	// curled, its tip nearer the first finger's knuckle.
	for (int side = 0; side < 2; side++) {
		auto thumb_bend = [&](const Rig &r, float *across) {
			const int *t = body.thumb[side];
			const V3 t0 = move_of(r.world()[t[0]]), t1 = move_of(r.world()[t[1]]),
				 t2 = move_of(r.world()[t[2]]);
			const V3 s01 = normalized(t1 - t0), s12 = normalized(t2 - t1);
			const V3 up = normalized(direction(r.world()[t[0]], {0, 1, 0}));
			if (across)
				*across = std::fabs(dot(normalized(cross(s01, s12)), up));
			return std::acos(std::min(1.0f, dot(s01, s12))) * 180 / 3.14159265f;
		};
		auto tip_to_index = [&](const Rig &r) {
			return length(move_of(r.world()[body.thumb[side][2]]) - move_of(r.world()[body.index[side][0]]));
		};
		float across = 0;
		CHECK(near(thumb_bend(rig, &across), 12.5, 0.05) && across > 0.98f);
		CHECK(near(thumb_bend(curled, &across), 25, 0.05) && across > 0.98f);
		CHECK(near(thumb_bend(flat, nullptr), 0, 0.05));
		CHECK(tip_to_index(rig) < tip_to_index(flat) - 0.002f && tip_to_index(curled) < tip_to_index(rig));
		// Its bones rolled a quarter turn about their own way (as a VRM's
		// thumb is, its nail out): the same bend, the same way.
		Figure rolled = make_body();
		const V3 along = normalized(V3{side ? -1.0f : 1.0f, 0, 1});
		const Quat roll = axis_angle(along, 3.14159265f / 2);
		rolled.p.b.doc["nodes"][rolled.thumb[side][0]]["rotation"] = {roll.x, roll.y, roll.z, roll.w};
		auto mr = std::make_shared<Model>();
		CHECK(load(rolled.p.b.glb(), *mr));
		RigOptions ro = o;
		ro.fingers = 0.5f;
		Rig rr(mr, ro);
		rr.pose(still());
		const int *t = rolled.thumb[side];
		const V3 r1 = move_of(rr.world()[t[1]]), r2 = move_of(rr.world()[t[2]]);
		const V3 b1 = move_of(rig.world()[body.thumb[side][1]]), b2 = move_of(rig.world()[body.thumb[side][2]]);
		CHECK(near3(r1, b1, 1e-5f) && near3(r2, b2, 1e-5f));
	}

	// A finger its file already bends 30 degrees: relaxed leaves it be;
	// curled, it's bent only the rest of the way.
	Figure pre = make_body(30);
	auto mp = std::make_shared<Model>();
	CHECK(load(pre.p.b.glb(), *mp));
	o.fingers = 0.5f;
	Rig relaxed_pre(mp, o);
	relaxed_pre.pose(still());
	CHECK(near(bend_of(relaxed_pre, pre.hand[0], pre.index[0][0], 0), 30, 0.05));
	o.fingers = 1;
	Rig curled_pre(mp, o);
	curled_pre.pose(still());
	CHECK(near(bend_of(curled_pre, pre.hand[0], pre.index[0][0], 0), 40, 0.05));

	// A finger its own animation moves is left to it.
	{
		Figure an = make_body();
		Builder &b = an.p.b;
		b.doc["animations"] = json::array(
			{{{"name", "Point"},
			  {"samplers",
			   {{{"input", b.floats({0}, 1, "SCALAR")}, {"output", b.floats({0, 0, 0, 1}, 4, "VEC4")}}}},
			  {"channels", {{{"sampler", 0}, {"target", {{"node", an.index[0][0]}, {"path", "rotation"}}}}}}}});
		auto ma = std::make_shared<Model>();
		CHECK(load(b.glb(), *ma));
		RigOptions oa;
		oa.springs = false;
		oa.animation = 0;
		Rig ra(ma, oa);
		ra.pose(still());
		CHECK(near(bend_of(ra, an.hand[0], an.index[0][0], 0), 0, 0.05));
		CHECK(near(bend_of(ra, an.hand[1], an.index[1][0], 1), 20, 0.05));
	}

	// Its body takes some of your head's turn (40% at most), its head
	// still turned just as yours.
	const float PI = 3.14159265f;
	const Quat turn = axis_angle({0, 1, 0}, 30 * PI / 180);
	Drive d = still();
	d.head = turn;
	d.body_follow = 1;
	rig.pose(d);
	const V3 head_front = direction(rig.world()[body.p.head], {0, 0, 1});
	const V3 chest_front = direction(rig.world()[body.chest], {0, 0, 1});
	const V3 spine_front = direction(rig.world()[body.spine], {0, 0, 1});
	CHECK(near3(normalized(head_front), rotate(turn, {0, 0, 1}), 1e-4f));
	CHECK(near(std::atan2(chest_front.x, chest_front.z) * 180 / PI, 12, 0.05));
	CHECK(near(std::atan2(spine_front.x, spine_front.z) * 180 / PI, 4.8, 0.05));
	d.body_follow = 0;
	rig.pose(d);
	CHECK(near3(normalized(direction(rig.world()[body.chest], {0, 0, 1})), {0, 0, 1}, 1e-5f));
	CHECK(near3(normalized(direction(rig.world()[body.p.head], {0, 0, 1})), rotate(turn, {0, 0, 1}), 1e-4f));
	// Its own sway turns its body, not its head.
	d = still();
	d.body_yaw = 2;
	rig.pose(d);
	const V3 swayed = direction(rig.world()[body.chest], {0, 0, 1});
	CHECK(near(std::atan2(swayed.x, swayed.z) * 180 / PI, 2, 0.05));
	CHECK(near3(normalized(direction(rig.world()[body.p.head], {0, 0, 1})), {0, 0, 1}, 1e-4f));

	// A breath: its chest rises (its head with it) and leans back a
	// little, its shoulders rise.
	d = still();
	rig.pose(d);
	const V3 head_out = move_of(rig.world()[body.p.head]);
	const V3 arm_out = move_of(rig.world()[m->bone("leftUpperArm")]),
		 elbow_out = move_of(rig.world()[m->bone("leftLowerArm")]);
	d.breath = 1;
	rig.pose(d);
	const V3 head_in = move_of(rig.world()[body.p.head]);
	const V3 arm_in = move_of(rig.world()[m->bone("leftUpperArm")]),
		 elbow_in = move_of(rig.world()[m->bone("leftLowerArm")]);
	// (Its height as the rig takes it: what's drawn, here its head.)
	const float height = std::max(m->high.y - m->low.y, std::max(m->high.x - m->low.x, m->high.z - m->low.z));
	CHECK(head_in.y - head_out.y > 0.002f * height && head_in.y - head_out.y < 0.006f * height);
	CHECK(head_in.z < head_out.z); // leaning back
	// Its shoulder up, its arm still hanging the same way.
	CHECK(arm_in.y > arm_out.y + 0.0015f);
	CHECK(near3(normalized(elbow_in - arm_in), normalized(elbow_out - arm_out), 1e-4f));

	// One with no bones grows a little with each breath, from its bottom
	// - unless it's worn on your head.
	GlbPart part;
	std::vector<uint16_t> ix16;
	cube(part.pos, ix16, {0, 0.5f, 0});
	part.index.assign(ix16.begin(), ix16.end());
	auto mc = std::make_shared<Model>();
	CHECK(load(write_glb({part}), *mc));
	Rig box(mc, RigOptions());
	CHECK(box.finger_joints() == 0 && !box.has_body());
	Drive db = still();
	db.breath = 1;
	box.pose(db);
	float low = 1e9f, high = -1e9f;
	for (const V3 &v : box.parts()[0].pos)
		low = std::min(low, v.y), high = std::max(high, v.y);
	CHECK(near(low, 0, 1e-5) && near(high, 1.012, 1e-5));
	db.breathe_whole = false;
	box.pose(db);
	high = -1e9f;
	for (const V3 &v : box.parts()[0].pos)
		high = std::max(high, v.y);
	CHECK(near(high, 1, 1e-5));
}

// --- Bones that follow others (VRMC_node_constraint) ---------------------

// Whether two turns are the same (to `by` radians).
static bool same_turn(Quat a, Quat b, float by = 1e-3f)
{
	const float c = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
	return 2 * std::acos(std::min(1.0f, c)) <= by;
}
// A node's own turn as the rig has it (from its parent's world).
static Quat own_turn(const Rig &rig, int node)
{
	const int p = rig.model().nodes[node].parent;
	const M4 local = p >= 0 ? inverse(rig.world()[p]) * rig.world()[node] : rig.world()[node];
	return quat_of(local);
}
static json follows(const char *kind, int source, json more = json::object(), const char *version = "1.0")
{
	more["source"] = source;
	return {{"VRMC_node_constraint", {{"specVersion", version}, {"constraint", {{kind, more}}}}}};
}

// A node under `parent`, at `at`.
static int add_to(Builder &b, int parent, const std::string &name, V3 at = {})
{
	const int i = b.node({{"name", name}, {"translation", {at.x, at.y, at.z}}});
	b.doc["nodes"][parent]["children"].push_back(i);
	return i;
}

static void test_bones_that_follow()
{
	const float PI = 3.14159265f;
	const float D = PI / 180;
	Figure body = make_body();
	Builder &b = body.p.b;
	json &bones = b.doc["extensions"]["VRMC_vrm"]["humanoid"]["humanBones"];
	const int hand = body.hand[0];
	const int lower = bones["leftLowerArm"]["node"].get<int>();
	const int upper = bones["leftUpperArm"]["node"].get<int>();
	const int shoulder = bones["leftShoulder"]["node"].get<int>();
	auto quat = [](Quat q) { return json::array({q.x, q.y, q.z, q.w}); };
	auto add = [&](int parent, const char *name, V3 at, json more = json::object()) {
		json n = {{"name", name}, {"translation", {at.x, at.y, at.z}}};
		n.update(more);
		const int i = b.node(n);
		b.doc["nodes"][parent]["children"].push_back(i);
		return i;
	};
	// Its hand rests turned (its own z along its forearm), and its forearm
	// twist bone rests turned too (its own -y along its forearm): half its
	// hand's twist, worked out through both.
	const Quat hand_rest = axis_angle({0, 1, 0}, 90 * D), twist_rest = axis_angle({0, 0, 1}, 90 * D);
	b.doc["nodes"][hand]["rotation"] = quat(hand_rest);
	const int twist = add(lower, "forearm_twist.L", {0.125f, 0, 0},
			      {{"rotation", quat(twist_rest)},
			       {"extensions", follows("roll", hand, {{"rollAxis", "Y"}, {"weight", 0.5}})}});
	// A roll with no axis (X, as three-vrm has it), of the beta's version.
	const int plain_roll = add(lower, "plain_roll", {0.1f, 0, 0},
				   {{"extensions", follows("roll", hand, json::object(), "1.0-beta")}});
	// A second arm copying its upper arm's turn, and one copying half of
	// that - first in the file, so it must be worked out after.
	const int sub2 = add(body.chest, "sub2", {-0.1f, 0.1f, 0});
	const Quat sub_rest = axis_angle({0, 0, 1}, 45 * D);
	const int sub = add(body.chest, "sub_arm", {0.1f, 0.1f, 0},
			    {{"rotation", quat(sub_rest)}, {"extensions", follows("rotation", upper)}});
	b.doc["nodes"][sub2]["extensions"] = follows("rotation", sub, {{"weight", 0.5}});
	const int sub_tip = add(sub, "sub_tip", {0.2f, 0, 0});
	// Weights kept from 0 to 1.
	const int heavy = add(body.chest, "heavy", {0, 0.05f, 0}, {{"extensions", follows("rotation", upper, {{"weight", 3}})}});
	const int light = add(body.chest, "light", {0, 0.05f, 0}, {{"extensions", follows("rotation", upper, {{"weight", -2}})}});
	// Something aiming at its second arm's tip, first in the file: it waits
	// on its source's parents. A sleeve aiming at its elbow, under a part
	// grown twice as tall as it's wide.
	const int pointer = add(body.p.hips, "pointer", {0, 0.3f, 0.2f});
	b.doc["nodes"][pointer]["extensions"] = follows("aim", sub_tip, {{"aimAxis", "PositiveX"}});
	const int sleeve_root = add(shoulder, "sleeve_root", {0.05f, 0, 0}, {{"scale", {1, 2, 1}}});
	const int sleeve = add(sleeve_root, "sleeve", {0, 0, 0},
			       {{"extensions", follows("aim", lower, {{"aimAxis", "PositiveX"}})}});
	// Left out: one following itself, two following each other and one
	// waiting on them, an unknown version, roll axis or aim axis, no such
	// source, no constraint, and an aim at its own child.
	const int self = add(body.chest, "self", {}), loop_a = add(body.chest, "loop_a", {}),
		  loop_b = add(body.chest, "loop_b", {}), waiter = add(body.chest, "waiter", {});
	b.doc["nodes"][self]["extensions"] = follows("rotation", self);
	b.doc["nodes"][loop_a]["extensions"] = follows("rotation", loop_b);
	b.doc["nodes"][loop_b]["extensions"] = follows("rotation", loop_a);
	b.doc["nodes"][waiter]["extensions"] = follows("rotation", loop_a);
	add(body.chest, "v2", {}, {{"extensions", follows("rotation", upper, json::object(), "2.0")}});
	add(body.chest, "axis_w", {}, {{"extensions", follows("roll", hand, {{"rollAxis", "W"}})}});
	add(body.chest, "aim_up", {}, {{"extensions", follows("aim", hand, {{"aimAxis", "Up"}})}});
	add(body.chest, "nowhere", {}, {{"extensions", follows("rotation", 99999)}});
	add(body.chest, "empty", {},
	    {{"extensions", {{"VRMC_node_constraint", {{"specVersion", "1.0"}, {"constraint", json::object()}}}}}});
	const int aimer = add(body.chest, "aimer", {});
	const int aimer_child = add(aimer, "aimer_child", {0.1f, 0.1f, 0});
	b.doc["nodes"][aimer]["extensions"] = follows("aim", aimer_child);
	// Its upper arm and hand posed by its own animation: the arm swung
	// out and forwards, the hand twisted 60 degrees round its forearm and
	// bent 30 degrees up at the wrist.
	const Quat arm = normalized(axis_angle({0, 0, 1}, -50 * D) * axis_angle({0, 1, 0}, 40 * D));
	const Quat hand_turn = normalized(axis_angle({0, 0, 1}, 30 * D) * axis_angle({1, 0, 0}, 60 * D) * hand_rest);
	b.doc["animations"] = json::array(
		{{{"name", "Pose"},
		  {"samplers",
		   {{{"input", b.floats({0}, 1, "SCALAR")}, {"output", b.floats({arm.x, arm.y, arm.z, arm.w}, 4, "VEC4")}},
		    {{"input", b.floats({0}, 1, "SCALAR")},
		     {"output", b.floats({hand_turn.x, hand_turn.y, hand_turn.z, hand_turn.w}, 4, "VEC4")}}}},
		  {"channels",
		   {{{"sampler", 0}, {"target", {{"node", upper}, {"path", "rotation"}}}},
		    {{"sampler", 1}, {"target", {{"node", hand}, {"path", "rotation"}}}}}}}});

	auto m = std::make_shared<Model>();
	std::string why;
	CHECK(load(b.glb(), *m, &why));
	CHECK(m->constraints.size() == 8);
	CHECK(m->constraints_left_out == 10);
	auto place = [&](int node) {
		for (size_t i = 0; i < m->constraints.size(); i++)
			if (m->constraints[i].node == node)
				return (int)i;
		return -1;
	};
	for (int n : {twist, plain_roll, sub, sub2, heavy, light, pointer, sleeve})
		CHECK(place(n) >= 0);
	for (int n : {self, loop_a, loop_b, waiter, aimer})
		CHECK(place(n) < 0);
	CHECK(place(sub) < place(sub2) && place(sub) < place(pointer));
	if (place(twist) < 0 || place(heavy) < 0 || place(light) < 0 || place(plain_roll) < 0)
		return;
	const Constraint &tc = m->constraints[place(twist)];
	CHECK(tc.kind == Constraint::Roll && tc.source == hand && near3(tc.axis, {0, 1, 0}, 0) && near(tc.weight, 0.5, 1e-6));
	CHECK(near3(m->constraints[place(plain_roll)].axis, {1, 0, 0}, 0));
	CHECK(m->constraints[place(heavy)].weight == 1 && m->constraints[place(light)].weight == 0);
	CHECK(m->constraints[place(pointer)].kind == Constraint::Aim);

	RigOptions o;
	o.arms_down = false;
	o.fingers = 0;
	o.animation = animation_named(*m, "Pose");
	Rig rig(m, o);
	CHECK(rig.following_bones() == 8);
	Drive d = still();
	d.dt = 0;
	rig.pose(d);
	// Twist: half the hand's 60 degrees round its forearm (x), none of its
	// bend - however each rests.
	CHECK(same_turn(own_turn(rig, twist), axis_angle({1, 0, 0}, 30 * D) * twist_rest));
	CHECK(same_turn(own_turn(rig, plain_roll), axis_angle({1, 0, 0}, 60 * D)));
	// Copied turns, in their own spaces; a copy of a copy, half of it.
	CHECK(same_turn(own_turn(rig, sub), sub_rest * arm));
	CHECK(same_turn(own_turn(rig, sub2), slerp(Quat{}, arm, 0.5f)));
	CHECK(same_turn(own_turn(rig, heavy), arm));
	CHECK(same_turn(own_turn(rig, light), Quat{}));
	// Aims: its x straight at its source, even under a part grown unevenly.
	auto at = [&](int n) { return move_of(rig.world()[n]); };
	auto x_of = [&](int n) { return normalized(direction(rig.world()[n], {1, 0, 0})); };
	CHECK(near3(x_of(sleeve), normalized(at(lower) - at(sleeve)), 1e-4f));
	CHECK(near3(x_of(pointer), normalized(at(sub_tip) - at(pointer)), 1e-4f));
	// The arm really moved, so that means something.
	CHECK(dot(normalized(at(lower) - at(upper)), {1, 0, 0}) < 0.7f);
	// Worked out from where they rest every frame, not on top of last time.
	const Quat once = own_turn(rig, twist);
	d.dt = 1.0f / 60;
	for (int i = 0; i < 5; i++)
		rig.pose(d);
	CHECK(same_turn(own_turn(rig, twist), once));
	CHECK(near3(x_of(sleeve), normalized(at(lower) - at(sleeve)), 1e-4f));

	// A long line of them, each following the next: worked out from the
	// last back (no stack used up). Past 1024 the rest are left out, so the
	// 1025th is free - turned by its own animation, and all before it with
	// it.
	Person lp = make_person(false);
	Builder &line = lp.b;
	const int LINE = 1030, first = (int)line.doc["nodes"].size();
	for (int i = 0; i < LINE; i++)
		add_to(line, lp.hips, "n" + std::to_string(i));
	for (int i = 0; i < LINE - 1; i++)
		line.doc["nodes"][first + i]["extensions"] = follows("rotation", first + i + 1);
	const Quat spin = axis_angle({0, 1, 0}, 20 * D);
	line.doc["animations"] = json::array(
		{{{"samplers",
		   {{{"input", line.floats({0}, 1, "SCALAR")},
		     {"output", line.floats({spin.x, spin.y, spin.z, spin.w}, 4, "VEC4")}}}},
		  {"channels", {{{"sampler", 0}, {"target", {{"node", first + 1024}, {"path", "rotation"}}}}}}}});
	auto lm = std::make_shared<Model>();
	CHECK(load(line.glb(), *lm, &why));
	CHECK(lm->constraints.size() == 1024 && lm->constraints_left_out == LINE - 1 - 1024);
	if (lm->constraints.size() != 1024)
		return;
	CHECK(lm->constraints.front().node == first + 1023 && lm->constraints.back().node == first);
	RigOptions lo;
	lo.animation = 0;
	Rig lrig(lm, lo);
	lrig.pose(d);
	CHECK(same_turn(own_turn(lrig, first), spin) && same_turn(own_turn(lrig, first + 500), spin));

	// Before anything swings: a bone aiming at a point that moves (round
	// it, over a second) swings its swinging tip about - the tip, left
	// where it was, lags behind it.
	Person sp = make_person(false);
	Builder &sb = sp.b;
	const int goal = add_to(sb, sp.hips, "goal", {0.3f, 0, 0});
	const int aimed = add_to(sb, sp.hips, "aimed", {0, 0, 0});
	sb.doc["nodes"][aimed]["extensions"] = follows("aim", goal, {{"aimAxis", "PositiveX"}});
	const int tip = add_to(sb, aimed, "tip", {0.1f, 0, 0});
	add_to(sb, tip, "tip_end", {0.1f, 0, 0});
	sb.doc["extensions"]["VRMC_springBone"]["springs"].push_back(
		{{"name", "tip"},
		 {"joints",
		  {{{"node", tip}, {"stiffness", 0.0}, {"gravityPower", 0.0}, {"dragForce", 0.0}}, {{"node", tip + 1}}}}});
	sb.doc["animations"] = json::array(
		{{{"samplers",
		   {{{"input", sb.floats({0, 1}, 1, "SCALAR")},
		     {"output", sb.floats({0.3f, 0, 0, 0, 0, 0.3f}, 3, "VEC3")}}}},
		  {"channels", {{{"sampler", 0}, {"target", {{"node", goal}, {"path", "translation"}}}}}}}});
	auto smod = std::make_shared<Model>();
	CHECK(load(sb.glb(), *smod, &why));
	RigOptions so;
	so.animation = 0;
	Rig srig(smod, so);
	Drive sd = still();
	sd.dt = 0;
	srig.pose(sd);
	auto x_in = [](const Rig &r, int n) { return normalized(direction(r.world()[n], {1, 0, 0})); };
	CHECK(near3(x_in(srig, tip), x_in(srig, aimed), 1e-4f));
	sd.dt = 1.0f / 60;
	for (int i = 0; i < 30; i++)
		srig.pose(sd);
	const V3 aim_now = x_in(srig, aimed);
	CHECK(near3(aim_now, normalized(move_of(srig.world()[goal]) - move_of(srig.world()[aimed])), 1e-4f));
	CHECK(dot(aim_now, {1, 0, 0}) < 0.9f);         // it has swung
	CHECK(dot(x_in(srig, tip), aim_now) < 0.996f); // its tip more than 5 degrees behind
}

int main()
{
	test_write_and_read();
	test_storage();
	test_files_named_in_other_letters();
	test_broken();
	test_vrm1_reading();
	test_vrm0_reading();
	test_head_and_eyes();
	test_expressions();
	test_own_face_shows();
	test_names_and_sides();
	test_springs();
	test_own_animation();
	test_link_chain();
	test_spots_on_a_vrm();
	test_spots_on_two_characters();
	test_face_region();
	test_dangly_parts();
	test_facing_arms_and_wearing();
	test_set_up();
	test_obs_camera();
	test_effect_needs();
	test_detail_maps();
	test_lights();
	test_skin_parts();
	test_camera_views();
	test_framed_as_it_stands();
	test_tune();
	test_alive();
	test_light();
	test_fingers_and_body();
	test_arms_follow();
	test_motion_reading();
	test_moves_on_a_vrm();
	test_tracked_bones();
	test_faces_from_names();
	test_wave_and_cheer();
	test_own_move_once();
	test_faces_on_cue();
	test_knocked_and_head();
	test_bones_that_follow();
	std::printf("unit_model: %d passed, %d failed\n", passes, failures);
	return failures ? 1 : 0;
}
