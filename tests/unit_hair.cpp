// Unit tests for hair strands (src/face/hair.hpp, and the strands'
// part of src/face/model.cpp and src/face/rig.cpp), on coats made here:
// the hair detail setting and what each choice allows, how many of a
// part's strands are drawn for its size on screen, and the hair budget; a
// file's lines and line strips read into strands, the order they're drawn
// in, the points allowed, how thickly hair covers its skin and which
// strands are kept and drawn first for it (their rates and keys), the
// points that shape a strand (and let its bones swing it, and keep a curl
// round), its groups, layers and pieces, its expressions (point by point
// and strand by strand) and its stand-in hair; and strands posed by the
// rig, here and for the graphics card.
// Build and run: tools/test-link.sh (Linux, any C++17 compiler).

#include "../src/face/hair.hpp"
#include "../src/face/model.hpp"
#include "../src/face/rig.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

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

// --- Making glTF files ------------------------------------------------------

// (As tests/unit_model.cpp's.)
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
	int indices32(const std::vector<uint32_t> &ix)
	{
		return accessor(view(ix.data(), ix.size() * 4), 5125, ix.size(), "SCALAR");
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

// A coat of hair in the making: its strands' points root to tip, back to
// back, with what a file may say of each point. Written as glTF LINES,
// each piece a pair of point numbers.
struct Groom {
	std::vector<V3> pos;
	std::vector<uint32_t> first = {0};   // strand s: points first[s] up to first[s + 1]
	std::vector<float> width;            // one a point (empty: the file has none)
	std::vector<float> colour;           // 4 a point, linear (empty: none)
	std::vector<uint16_t> joints;        // 4 a point (empty: no bones)
	std::vector<float> weights;          // 4 a point
	std::vector<std::vector<V3>> shapes; // each expression: how far it moves every point

	size_t strands() const { return first.size() - 1; }
	void strand(const std::vector<V3> &points)
	{
		pos.insert(pos.end(), points.begin(), points.end());
		first.push_back((uint32_t)pos.size());
	}
	// A straight strand of `points` points, evenly apart (as loaded: its
	// root and tip).
	void straight(V3 root, V3 tip, int points = 3)
	{
		for (int i = 0; i < points; i++)
			pos.push_back(root + (tip - root) * ((float)i / (float)(points - 1)));
		first.push_back((uint32_t)pos.size());
	}
	// The bones of the next point (after those said so far).
	void held(uint16_t j0, float w0, uint16_t j1 = 0, float w1 = 0)
	{
		joints.insert(joints.end(), {j0, j1, 0, 0});
		weights.insert(weights.end(), {w0, w1, 0, 0});
	}
	json part(Builder &b) const
	{
		json attr = {{"POSITION", b.vec3s(pos)}};
		if (!width.empty())
			attr["_WIDTH"] = b.floats(width, 1, "SCALAR");
		if (!colour.empty())
			attr["COLOR_0"] = b.floats(colour, 4, "VEC4");
		if (!joints.empty()) {
			attr["JOINTS_0"] =
				b.accessor(b.view(joints.data(), joints.size() * 2), 5123, pos.size(), "VEC4");
			attr["WEIGHTS_0"] = b.floats(weights, 4, "VEC4");
		}
		std::vector<uint32_t> ix;
		for (size_t s = 0; s < strands(); s++)
			for (uint32_t i = first[s]; i + 1 < first[s + 1]; i++)
				ix.insert(ix.end(), {i, i + 1});
		json p = {{"attributes", attr}, {"mode", 1}, {"indices", b.indices32(ix)}};
		if (!shapes.empty()) {
			json targets = json::array();
			for (const std::vector<V3> &s : shapes)
				targets.push_back({{"POSITION", b.vec3s(s)}});
			p["targets"] = targets;
		}
		return p;
	}
	// Which of its points is at `v` (-1: none).
	int at(V3 v) const
	{
		// (Looked up in a list made once - and made again if a point
		// has moved since.)
		for (int again = 0; again < 2; again++) {
			const auto it = where.find(std::array<float, 3>{v.x, v.y, v.z});
			if (it != where.end() && (size_t)it->second < pos.size() &&
			    near3(pos[(size_t)it->second], v, 0))
				return it->second;
			if (again)
				break;
			where.clear();
			for (size_t i = 0; i < pos.size(); i++)
				where.emplace(std::array<float, 3>{pos[i].x, pos[i].y, pos[i].z}, (int)i);
		}
		return -1;
	}
	// Which strand point i is on.
	size_t strand_of(size_t i) const
	{
		return (size_t)(std::upper_bound(first.begin(), first.end(), (uint32_t)i) - first.begin()) - 1;
	}

private:
	mutable std::map<std::array<float, 3>, int> where;
};

// A file of these coats: one mesh, a part each, on the node "Hair" - held
// by bones if `bones` says where each is (every bone a node of its own
// under one armature, the mesh's skin made with them as they stand).
static Builder file_of(const std::vector<Groom> &grooms, const std::vector<V3> &bones = {})
{
	Builder b;
	json parts = json::array();
	for (const Groom &g : grooms)
		parts.push_back(g.part(b));
	b.doc["meshes"].push_back({{"primitives", parts}});
	json node = {{"name", "Hair"}, {"mesh", 0}};
	if (!bones.empty()) {
		const int armature = b.node({{"name", "Armature"}});
		json joints = json::array();
		std::vector<float> ibm;
		for (size_t j = 0; j < bones.size(); j++) {
			joints.push_back(b.node({{"name", "j" + std::to_string(j)},
						 {"translation", {bones[j].x, bones[j].y, bones[j].z}}}));
			const M4 inv = inverse(moved(bones[j]));
			ibm.insert(ibm.end(), inv.m, inv.m + 16);
		}
		b.doc["nodes"][armature]["children"] = joints;
		b.doc["skins"].push_back({{"joints", joints}, {"inverseBindMatrices", b.floats(ibm, 16, "MAT4")}});
		node["skin"] = 0;
	}
	b.node(node);
	return b;
}

// A body beside the hair (a triangle, its mesh's last part): something
// to see, so a file whose strands all go still loads.
static void add_body(Builder &b)
{
	const std::vector<V3> tri = {{0, 0, 0}, {1, 0, 0}, {0, 0, 1}};
	b.doc["meshes"][0]["primitives"].push_back(
		{{"attributes", {{"POSITION", b.vec3s(tri)}}}, {"indices", b.indices({0, 1, 2})}});
}

static bool load(Builder &b, Model &m, const LoadOptions &options = LoadOptions(), std::string *why = nullptr)
{
	const std::vector<uint8_t> bytes = b.glb();
	std::string w;
	const bool ok = load_model(bytes.data(), bytes.size(), "", m, &w, options);
	if (why)
		*why = w;
	return ok;
}

// The file read (a failed check, and why, if it's refused).
static Model loaded(Builder &b, const LoadOptions &options = LoadOptions())
{
	Model m;
	std::string why;
	const bool ok = load(b, m, options, &why);
	CHECK(ok);
	if (!ok)
		std::printf("  refused: %s\n", why.c_str());
	return m;
}
static Model loaded(const Groom &g, const LoadOptions &options = LoadOptions())
{
	Builder b = file_of({g});
	return loaded(b, options);
}

// A model's part (an empty one if it has no such part).
static const Primitive &part_of(const Model &m, size_t part = 0, size_t mesh = 0)
{
	static const Primitive nothing;
	return mesh < m.meshes.size() && part < m.meshes[mesh].parts.size() ? m.meshes[mesh].parts[part] : nothing;
}

// Every point kept: which of the coat's points it is (-1: none of them).
static std::vector<int> kept_of(const Groom &g, const Primitive &p)
{
	std::vector<int> out;
	for (const V3 &v : p.pos)
		out.push_back(g.at(v));
	return out;
}

// How a file is loaded to try its strands' points: left out where they
// bend it less than `bend`, kept where its bones could swing it more than
// `swing` - and, only if `turn` says so, where it turns more than that
// many degrees (none: the rule is off, as it was before there was one).
static LoadOptions options_with(float bend, float swing = LoadOptions().strand_swing, float turn = 0)
{
	LoadOptions o;
	o.strand_bend = bend;
	o.strand_swing = swing;
	o.strand_turn = turn;
	return o;
}

static bool same_points(const std::vector<V3> &a, const std::vector<V3> &b)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); i++)
		if (!near3(a[i], b[i], 0))
			return false;
	return true;
}

// How far `p` is from the straight run a to b.
static float off_run(V3 p, V3 a, V3 b)
{
	const V3 ab = b - a, ap = p - a;
	const float ab2 = dot(ab, ab);
	const float t = ab2 > 0 ? std::min(1.0f, std::max(0.0f, dot(ap, ab) / ab2)) : 0.0f;
	return length(ap - ab * t);
}

// How unlike two points' bones are (their 4 numbers and how much each
// holds the point): 0 the same, 1 none shared.
static float unlike(const uint16_t *ja, const float *wa, const uint16_t *jb, const float *wb)
{
	std::map<int, float> d;
	for (int k = 0; k < 4; k++) {
		d[ja[k]] += wa[k];
		d[jb[k]] -= wb[k];
	}
	float sum = 0;
	for (const auto &e : d)
		sum += std::fabs(e.second);
	return sum * 0.5f;
}

// How unlike a coat's point i's bones are to an even change from point
// a's to point c's, `u` of the way from a to c.
static float uneven(const Groom &g, int i, int a, int c, float u)
{
	std::map<int, float> d;
	for (int k = 0; k < 4; k++) {
		d[g.joints[(size_t)i * 4 + k]] += g.weights[(size_t)i * 4 + k];
		d[g.joints[(size_t)a * 4 + k]] -= g.weights[(size_t)a * 4 + k] * (1 - u);
		d[g.joints[(size_t)c * 4 + k]] -= g.weights[(size_t)c * 4 + k] * u;
	}
	float sum = 0;
	for (const auto &e : d)
		sum += std::fabs(e.second);
	return sum * 0.5f;
}

// A number from 0 up to 1, the same every run (and on every machine).
static float chance(uint32_t &seed)
{
	seed = seed * 1664525u + 1013904223u;
	return (float)(seed >> 8) / 16777216.0f;
}

// --- The hair detail setting --------------------------------------------------

static void test_detail_names()
{
	CHECK(hair_detail_of("full") == HairDetail::Full && hair_detail_of("light") == HairDetail::Light &&
	      hair_detail_of("today") == HairDetail::Today);
	// Anything else: full.
	CHECK(hair_detail_of("") == HairDetail::Full && hair_detail_of("Light") == HairDetail::Full &&
	      hair_detail_of("heavy") == HairDetail::Full && hair_detail_of("today ") == HairDetail::Full);
	for (HairDetail d : {HairDetail::Full, HairDetail::Light, HairDetail::Today})
		CHECK(hair_detail_of(hair_detail_name(d)) == d);
	CHECK(std::string(hair_detail_name(HairDetail::Full)) == "full" &&
	      std::string(hair_detail_name(HairDetail::Light)) == "light" &&
	      std::string(hair_detail_name(HairDetail::Today)) == "today");
}

static void test_plans()
{
	const HairPlan full = hair_plan(HairDetail::Full), light = hair_plan(HairDetail::Light),
		       today = hair_plan(HairDetail::Today);
	CHECK(full.points_card == 2'000'000 && full.points_cpu == 150'000 && full.solid == 0.4f &&
	      full.budget_ms == 1.0f && full.pieces == 1'500'000);
	CHECK(light.points_card == 800'000 && light.points_cpu == 60'000 && light.solid == 0.65f &&
	      light.budget_ms == 0.5f && light.pieces == 750'000);
	CHECK(today.points_card == 800'000 && today.points_cpu == 60'000 && today.solid == 0.65f &&
	      today.budget_ms == 0.5f && today.pieces == 750'000);
	// As made: nothing limited.
	const HairPlan made;
	CHECK(made.budget_ms == 0 && made.pieces == 0);

	// How a file is loaded for each: the points kept (on the card, here),
	// and how straight a run is left out.
	for (bool on_card : {false, true}) {
		const LoadOptions f = hair_load_options(HairDetail::Full, on_card),
				  l = hair_load_options(HairDetail::Light, on_card),
				  t = hair_load_options(HairDetail::Today, on_card);
		CHECK(f.strand_points == (on_card ? 2'000'000u : 150'000u) && f.strand_points_bare == f.strand_points);
		CHECK(f.strand_bend == 0.0003f && f.strand_turn == 20.0f && f.strand_swing == 0.004f &&
		      f.strand_layers == 5);
		CHECK(l.strand_points == (on_card ? 800'000u : 60'000u) && l.strand_points_bare == l.strand_points);
		CHECK(l.strand_bend == 0.0005f && l.strand_turn == 30.0f && l.strand_swing == 0.008f &&
		      l.strand_layers == 5);
		// Today: no strands - but a character with no stand-in hair
		// keeps as many as Light.
		CHECK(t.strand_points == 0 && t.strand_points_bare == l.strand_points);
		CHECK(t.strand_bend == l.strand_bend && t.strand_turn == l.strand_turn &&
		      t.strand_swing == l.strand_swing && t.strand_layers == 5);
		// Which strands are kept first when not all can be: the same
		// whatever the choice - thin hair (up to one and a half times
		// over its skin) first, deep hair down to a third as much.
		for (const LoadOptions &o : {f, l, t})
			CHECK(o.strand_cover == 1.5f && o.strand_uneven == 3.0f);
		// (A change of choice is a change of how it's loaded.)
		CHECK(f != l && l != t && f != t && f == hair_load_options(HairDetail::Full, on_card));
	}
	CHECK(hair_load_options(HairDetail::Full, true) != hair_load_options(HairDetail::Full, false));
	// (A file loaded with nothing said: as Full's on the card - but for a
	// character with no stand-in hair, which keeps as many as Light's.)
	const LoadOptions unsaid;
	LoadOptions as_full = hair_load_options(HairDetail::Full, true);
	CHECK(unsaid != as_full && unsaid.strand_points == 2'000'000 && unsaid.strand_points_bare == 800'000);
	as_full.strand_points_bare = hair_load_options(HairDetail::Light, true).strand_points_bare;
	CHECK(unsaid == as_full);
	// (Every one of its numbers counts when two are told apart.)
	const std::function<void(LoadOptions &)> changes[] = {
		[](LoadOptions &o) { o.strand_points++; },   [](LoadOptions &o) { o.strand_points_bare++; },
		[](LoadOptions &o) { o.strand_bend *= 2; },  [](LoadOptions &o) { o.strand_turn *= 2; },
		[](LoadOptions &o) { o.strand_swing *= 2; }, [](LoadOptions &o) { o.strand_layers++; },
		[](LoadOptions &o) { o.strand_cover *= 2; }, [](LoadOptions &o) { o.strand_uneven *= 2; },
	};
	for (const std::function<void(LoadOptions &)> &change : changes) {
		LoadOptions o;
		change(o);
		CHECK(o != unsaid && !(o == unsaid));
	}
}

// --- How many strands for its size on screen ----------------------------------

// A part's strands as the loader leaves a coat the same all over (every
// strand's rate 1; test_rates_and_keys holds the loader to it): the first
// `kept` of the `in_file` its file has, in the order they're drawn in, its
// hairs `width` wide - which reads as much wider as fewer are kept.
static Strands alike(size_t in_file, size_t kept, float width)
{
	Strands h;
	h.in_file = (uint32_t)in_file;
	h.first.push_back(0);
	for (size_t k = 0; k < kept; k++) {
		h.first.push_back((uint32_t)(2 * k + 2));
		h.key.push_back((float)((double)k / (double)in_file));
		h.rate.push_back(1.0f);
	}
	h.full = kept < in_file ? (float)((double)kept / (double)in_file) : 1.0f;
	h.mean_width = h.full > 0 ? width / std::min(1.0f, h.full) : width;
	return h;
}

static void test_share_on_screen()
{
	// A coat of 10000 strands, its hairs 0.001 wide, where a pixel is 0.001
	// wide a unit from the camera: a pixel wide from 1 away. A hair is
	// drawn as long as it's 0.4 of a pixel wide (the plan's `solid`).
	const float pixel = 0.001f, width = 0.001f;
	const Strands coat = alike(10000, 10000, width);
	HairShare s = hair_share(coat, 10000, 1, 1, pixel, 1, 0.4f, 1);
	CHECK(s.count == 10000 && s.upto == 1);
	// 0.4 of a pixel wide (from 2.5 away): still all of them.
	s = hair_share(coat, 10000, 1, 2.5f, pixel, 1, 0.4f, 1);
	CHECK(s.count == 10000 && s.upto == 1);
	// Further: fewer, as many as leaves each drawn (1 / upto as wide)
	// 0.38 of a pixel - half as many from twice as far. The strands drawn
	// are the ones whose keys are under `upto`: of a coat the same all
	// over, that share of its file's.
	const float away[] = {5, 10, 20, 40};
	const double want[] = {5263.2, 2631.6, 1315.8, 657.9};
	for (int i = 0; i < 4; i++) {
		s = hair_share(coat, 10000, 1, away[i], pixel, 1, 0.4f, 1);
		CHECK(near((double)s.count, want[i], 1.5));
		CHECK(near(s.upto, (double)s.count / 10000, 1e-7));
		CHECK(near(width / (away[i] * pixel) / s.upto, 0.38, 0.002));
		CHECK(coat.strands_under(s.upto) == s.count);
		// (Bones that make it twice the size: as from half as far.)
		CHECK(hair_share(coat, 10000, 2, away[i] * 2, pixel, 1, 0.4f, 1).count == s.count);
	}
	CHECK(hair_share(coat, 10000, 3, 7.5f, pixel, 1, 0.4f, 1).count == 10000);
	// Light's hairs are drawn more solid (0.65 of a pixel): fewer sooner.
	s = hair_share(coat, 10000, 1, 2.5f, pixel, 1, 0.65f, 1);
	CHECK(near((double)s.count, 0.4 / (0.95 * 0.65) * 10000, 1.5));
	// No hair drawn thinner than 2 pixels: half as many again.
	s = hair_share(coat, 10000, 1, 5, pixel, 2, 0.4f, 1);
	CHECK(near((double)s.count, 2631.6, 1.5));

	// The hair budget's say: its share of those.
	s = hair_share(coat, 10000, 1, 1, pixel, 1, 0.4f, 0.5f);
	CHECK(s.count == 5000 && s.upto == 0.5f);
	s = hair_share(coat, 10000, 1, 5, pixel, 1, 0.4f, 0.25f);
	CHECK(near((double)s.count, 5263.2 / 4, 1.5));
	// (More than all is all; less than none is the few below.)
	s = hair_share(coat, 10000, 1, 1, pixel, 1, 0.4f, 3);
	CHECK(s.count == 10000 && s.upto == 1);
	s = hair_share(coat, 10000, 1, 1, pixel, 1, 0.4f, -1);
	CHECK(s.count == 16);

	// A coat of which only the first 3000 strands were kept: each of its
	// hairs stands for more of the file's, and reads that much wider
	// (alike's mean_width). All of them from near - drawn at the share of
	// the file's they are - and from 10 away as many as the whole coat
	// would draw.
	const Strands some = alike(10000, 3000, width);
	s = hair_share(some, 3000, 1, 1, pixel, 1, 0.4f, 1);
	CHECK(s.count == 3000 && near(s.upto, 0.3, 1e-7));
	s = hair_share(some, 3000, 1, 5, pixel, 1, 0.4f, 1);
	CHECK(s.count == 3000 && near(s.upto, 0.3, 1e-7));
	s = hair_share(some, 3000, 1, 10, pixel, 1, 0.4f, 1);
	CHECK(near((double)s.count, 2631.6, 1.5) && near(s.upto, (double)s.count / 10000, 1e-7));
	// Never more than are ready to draw (only the first 3000 of a whole
	// coat posed): those, drawn at the threshold that takes just them.
	s = hair_share(coat, 3000, 1, 1, pixel, 1, 0.4f, 1);
	CHECK(s.count == 3000 && near(s.upto, 0.3, 1e-7));
	s = hair_share(coat, 3000, 1, 3, pixel, 1, 0.4f, 1);
	CHECK(s.count == 3000 && near(s.upto, 0.3, 1e-7));
	s = hair_share(coat, 3000, 1, 10, pixel, 1, 0.4f, 1);
	CHECK(near((double)s.count, 2631.6, 1.5));
	// (More said to be ready than it has: all it has.)
	s = hair_share(some, 5000, 1, 1, pixel, 1, 0.4f, 1);
	CHECK(s.count == 3000 && near(s.upto, 0.3, 1e-7));
	// Never fewer than 16 (or all it has).
	s = hair_share(coat, 10000, 1, 1e6f, pixel, 1, 0.4f, 1);
	CHECK(s.count == 16 && near(s.upto, 0.0016, 1e-8));
	s = hair_share(alike(10000, 5, width), 5, 1, 1e6f, pixel, 1, 0.4f, 1);
	CHECK(s.count == 5 && near(s.upto, 0.0005, 1e-8));
	s = hair_share(alike(5, 5, width), 5, 1, 1e6f, pixel, 1, 0.4f, 0);
	CHECK(s.count == 5 && s.upto == 1);
	s = hair_share(coat, 7, 1, 1e6f, pixel, 1, 0.4f, 0);
	CHECK(s.count == 7 && near(s.upto, 0.0007, 1e-8));
	// No strands: none.
	CHECK(hair_share(Strands(), 0, 1, 1, pixel, 1, 0.4f, 1).count == 0);
	CHECK(hair_share(coat, 0, 1, 1, pixel, 1, 0.4f, 1).count == 0);
	CHECK(hair_share(Strands(), 100, 1, 1, pixel, 1, 0.4f, 1).count == 0);
	CHECK(hair_share(alike(10000, 0, width), 100, 1, 1, pixel, 1, 0.4f, 1).count == 0);

	// Numbers that aren't numbers: still some of its strands, never more
	// than it has, drawn at a threshold that is one.
	const float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
	const float bad[] = {nan, inf, -inf, 0, -1, 1e30f, 1e-30f};
	const Strands part = alike(10000, 4000, width);
	for (int which = 0; which < 6; which++)
		for (float v : bad) {
			float a[6] = {1, 3, pixel, 1, 0.4f, 1};
			a[which] = v;
			s = hair_share(part, 4000, a[0], a[1], a[2], a[3], a[4], a[5]);
			CHECK(s.count >= 16 && s.count <= 4000);
			CHECK(std::isfinite(s.upto) && near(s.upto, (double)s.count / 10000, 1e-7));
		}
	// (And a coat whose own numbers aren't: none the worse.)
	Strands odd = part;
	for (float v : bad) {
		odd.mean_width = v;
		s = hair_share(odd, 4000, 1, 3, pixel, 1, 0.4f, 1);
		CHECK(s.count >= 16 && s.count <= 4000 && std::isfinite(s.upto));
		odd.mean_width = part.mean_width;
		odd.full = v;
		s = hair_share(odd, 4000, 1, 3, pixel, 1, 0.4f, 1);
		CHECK(s.count >= 16 && s.count <= 4000);
		odd.full = part.full;
	}
}

// --- The hair budget --------------------------------------------------------

// As many readings as it waits for, all `ms`, at the share it asks for.
static void read_all(HairBudget &b, float ms)
{
	const float share = b.share();
	for (size_t i = 0; i < HairBudget::READINGS; i++)
		b.read(ms, share);
}

static void test_budget()
{
	CHECK(HairBudget::START == 0.25f && HairBudget::LEAST == 0.1f && HairBudget::READINGS == 6);
	const float least = HairBudget::LEAST;
	// No limit: all of them, whatever it reads.
	HairBudget b;
	CHECK(b.limit() == 0 && b.share() == 1 && b.measured() == -1);
	for (int i = 0; i < 20; i++)
		b.read(50, 1);
	CHECK(b.share() == 1 && b.measured() == -1);

	// A limit: it starts at a quarter, and changes only on its sixth
	// reading at the share it asked for.
	b.set_limit(1.0f);
	CHECK(b.limit() == 1 && b.share() == 0.25f && b.measured() == -1);
	for (int i = 0; i < 5; i++)
		b.read(2.0f, 0.25f);
	CHECK(b.share() == 0.25f && b.measured() == -1);
	// Twice over: fewer by 0.9 x limit / took = 0.45 - but never less than
	// half at a step.
	b.read(2.0f, 0.25f);
	CHECK(b.share() == 0.125f && b.measured() == 2.0f);
	// Between three quarters of the limit and the limit: left as it is.
	read_all(b, 0.8f);
	read_all(b, 1.0f);
	read_all(b, 0.75f);
	CHECK(b.share() == 0.125f && b.measured() == 0.75f);
	// Well under: more, by 0.9 x limit / took - a quarter more at most.
	read_all(b, 0.74f);
	CHECK(near(b.share(), 0.125 * 0.9 / 0.74, 1e-7));
	const float more = b.share();
	read_all(b, 0.5f);
	CHECK(near(b.share(), more * 1.25, 1e-7));
	read_all(b, 0);
	CHECK(near(b.share(), more * 1.25 * 1.25, 1e-7) && b.measured() == 0);
	// A fifth over: 0.9 / 1.2 = three quarters of them.
	const float before = b.share();
	read_all(b, 1.2f);
	CHECK(near(b.share(), before * 0.75, 1e-7) && near(b.measured(), 1.2, 1e-7));
	read_all(b, 0);
	read_all(b, 0);

	// It goes by the second lowest of its six: one reading far off (a
	// game drawn in between, or a clock's slip) changes nothing, nor do
	// two spikes.
	const float steady = b.share();
	CHECK(steady > 2 * least);
	const float spike[6] = {0.8f, 0.8f, 9.0f, 0.8f, 0.8f, 0.8f};
	for (float ms : spike)
		b.read(ms, steady);
	CHECK(b.share() == steady && b.measured() == 0.8f);
	const float spikes[6] = {9.0f, 0.8f, 30.0f, 0.8f, 0.8f, 0.8f};
	for (float ms : spikes)
		b.read(ms, steady);
	CHECK(b.share() == steady && b.measured() == 0.8f);
	const float low[6] = {0.01f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f};
	for (float ms : low)
		b.read(ms, steady);
	CHECK(b.share() == steady && b.measured() == 0.9f);
	// (Really over, all but one: cut.)
	const float over[6] = {0.01f, 3, 3, 3, 3, 3};
	for (float ms : over)
		b.read(ms, steady);
	CHECK(b.share() == steady * 0.5f && b.measured() == 3);

	// Readings at another share (drawn before its last change reached the
	// card) are left out, however many.
	const float now = b.share();
	for (int i = 0; i < 40; i++)
		b.read(0, now * 2);
	CHECK(b.share() == now);
	for (int i = 0; i < 3; i++)
		b.read(0, now);
	for (int i = 0; i < 10; i++)
		b.read(100, now * 0.5f);
	CHECK(b.share() == now);
	for (int i = 0; i < 3; i++)
		b.read(0, now);
	CHECK(near(b.share(), now * 1.25, 1e-7));
	// So are readings that aren't times.
	const float grown = b.share();
	for (int i = 0; i < 5; i++)
		b.read(0, grown);
	b.read(std::numeric_limits<float>::quiet_NaN(), grown);
	b.read(-1, grown);
	b.read(std::numeric_limits<float>::infinity(), grown);
	CHECK(b.share() == grown);
	b.read(0, grown);
	CHECK(near(b.share(), grown * 1.25, 1e-7));

	// Never fewer than the least (a tenth: fewer, each as much wider, and
	// a coat is shards) - however far over; its reading still says how
	// far.
	for (int i = 0; i < 20; i++)
		read_all(b, 100);
	CHECK(b.share() == least && b.measured() == 100);
	// Never more than all: from the least, a quarter more each step.
	int steps = 0;
	while (b.share() < 1 && steps < 100) {
		const float was = b.share();
		read_all(b, 0.01f);
		CHECK(b.share() > was && b.share() <= was * 1.25f + 1e-7f);
		steps++;
	}
	CHECK(b.share() == 1 && steps == (int)std::ceil(std::log(1 / least) / std::log(1.25) - 1e-6));
	read_all(b, 0.01f);
	CHECK(b.share() == 1 && b.measured() == 0.01f);
	// (And from all, one step over the limit at a time, down to the
	// least and no further.)
	float last = 1;
	for (int i = 0; i < 20; i++) {
		read_all(b, 100);
		CHECK(b.share() == std::max(least, last * 0.5f));
		last = b.share();
	}
	CHECK(last == least);

	// The same limit set again: nothing forgotten.
	HairBudget c;
	c.set_limit(1.0f);
	for (int i = 0; i < 3; i++)
		c.read(2, 0.25f);
	c.set_limit(1.0f);
	for (int i = 0; i < 3; i++)
		c.read(2, 0.25f);
	CHECK(c.share() == 0.125f && c.measured() == 2);
	// Another limit: its share kept, its readings begun again - and what
	// it measured under the old limit forgotten (-1) until it has a full
	// set of readings under the new one.
	for (int i = 0; i < 5; i++)
		c.read(0.1f, 0.125f);
	CHECK(c.measured() == 2);
	c.set_limit(2.0f);
	CHECK(c.limit() == 2 && c.share() == 0.125f && c.measured() == -1);
	for (int i = 0; i < 5; i++)
		c.read(0.1f, 0.125f);
	CHECK(c.share() == 0.125f && c.measured() == -1);
	c.read(0.1f, 0.125f);
	CHECK(c.share() == 0.15625f && c.measured() == 0.1f);
	// (The same limit said again forgets nothing.)
	c.set_limit(2.0f);
	CHECK(c.measured() == 0.1f);
	// No limit again: all of them, nothing measured; and a limit after
	// that starts at a quarter.
	c.set_limit(0);
	CHECK(c.limit() == 0 && c.share() == 1 && c.measured() == -1);
	read_all(c, 100);
	CHECK(c.share() == 1 && c.measured() == -1);
	c.set_limit(0.5f);
	CHECK(c.share() == 0.25f && c.measured() == -1);
	read_all(c, 0.45f);
	CHECK(c.measured() == 0.45f);
	c.set_limit(0.25f);
	CHECK(c.measured() == -1);
	// A limit that isn't one: none.
	for (float bad : {-1.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
		c.set_limit(1.0f);
		c.set_limit(bad);
		CHECK(c.limit() == 0 && c.share() == 1 && c.measured() == -1);
	}

	// A clock that stops answering. A frame or two with no answer (the
	// card's clock jumped) changes nothing - and neither do 119 in a row.
	CHECK(HairBudget::SILENT == 120);
	HairBudget d;
	d.set_limit(1.0f);
	read_all(d, 2.0f);
	CHECK(d.share() == 0.125f && !d.silent());
	for (int i = 0; i < 119; i++)
		d.unread();
	CHECK(d.share() == 0.125f && !d.silent());
	// One answer, and the count begins again - an answer for another share
	// than the one asked for now too (drawn before the last change), and
	// one that isn't a time at all doesn't count as one.
	d.read(0.9f, 0.125f);
	for (int i = 0; i < 119; i++)
		d.unread();
	d.read(0.9f, 0.25f);
	for (int i = 0; i < 119; i++)
		d.unread();
	d.read(-1, 0.125f);
	d.read(std::numeric_limits<float>::quiet_NaN(), 0.125f);
	CHECK(d.share() == 0.125f && !d.silent());
	// The 120th frame in a row with none: there's no clock to go by, so
	// all the hairs are drawn, as with no limit - and for as long as it
	// stays silent.
	d.unread();
	CHECK(d.silent() && d.share() == 1 && d.limit() == 1);
	for (int i = 0; i < 1000; i++)
		d.unread();
	CHECK(d.silent() && d.share() == 1);
	// It answers again (for all the hairs, as they were drawn): back to
	// the share it had, its readings from before the silence still counted
	// (one of the six it waits for was in) - five more and it changes.
	d.read(9.0f, 1.0f);
	CHECK(!d.silent() && d.share() == 0.125f && d.measured() == 2.0f);
	for (int i = 0; i < 4; i++)
		d.read(0.9f, 0.125f);
	CHECK(d.share() == 0.125f && d.measured() == 2.0f);
	d.read(0.9f, 0.125f);
	CHECK(d.share() == 0.125f && d.measured() == 0.9f);
	// With no limit there's no clock to miss: never silent, and a limit
	// set after starts as any does.
	HairBudget e;
	for (int i = 0; i < 500; i++)
		e.unread();
	CHECK(!e.silent() && e.share() == 1);
	e.set_limit(1.0f);
	CHECK(!e.silent() && e.share() == 0.25f);
	// Another limit set while it's silent: the count begins again.
	for (int i = 0; i < 120; i++)
		e.unread();
	CHECK(e.silent() && e.share() == 1);
	e.set_limit(2.0f);
	CHECK(!e.silent() && e.share() == 0.25f);
	// (The same limit set again changes nothing.)
	for (int i = 0; i < 120; i++)
		e.unread();
	e.set_limit(2.0f);
	CHECK(e.silent() && e.share() == 1);
}

// A pretend card: hair takes `cost` milliseconds with every hair drawn,
// and as much less as fewer are - and every third reading is far over (a
// game's frame drawn in between). The budget ends, and stays, where the
// time is between three quarters of the limit and the limit - or at all
// the hairs if that's under it, or the fewest if even those are over.
static void test_budget_settles()
{
	for (float limit : {1.0f, 0.5f})
		for (float cost : {0.1f, 0.5f, 0.9f, 1.3f, 2.0f, 7.0f, 40.0f, 200.0f}) {
			HairBudget b;
			b.set_limit(limit);
			int last_change = 0;
			float most = 0;
			for (int step = 1; step <= 80; step++) {
				const float share = b.share();
				for (size_t r = 0; r < HairBudget::READINGS; r++)
					b.read(cost * share * (r % 3 == 1 ? 6.0f : 1.0f), share);
				if (b.share() != share)
					last_change = step;
				// (Once it's under the limit it never goes back over.)
				if (cost * share <= limit)
					most = std::max(most, cost * b.share());
			}
			const float took = cost * b.share();
			CHECK(last_change <= 20);
			CHECK(most <= limit * 1.0001f);
			if (b.share() == 1)
				CHECK(took <= limit);
			else if (b.share() == HairBudget::LEAST)
				CHECK(took >= 0.75f * limit);
			else
				CHECK(took >= 0.75f * limit * 0.9999f && took <= limit * 1.0001f);
			if (cost * HairBudget::LEAST <= limit && cost >= limit)
				CHECK(b.share() > HairBudget::LEAST && b.share() < 1 && near(b.measured(), took, 1e-5));
		}
}

// --- A file's lines and line strips -----------------------------------------

// A strand part as a file has it: its points, drawn as `mode` (1 lines, 3
// a line strip), by point numbers if `ix` has any.
static json lines_part(Builder &b, const std::vector<V3> &pos, int mode, const std::vector<uint16_t> &ix = {})
{
	json p = {{"attributes", {{"POSITION", b.vec3s(pos)}}}, {"mode", mode}};
	if (!ix.empty())
		p["indices"] = b.indices(ix);
	return p;
}

static Builder lines_file(const std::vector<V3> &pos, int mode, const std::vector<uint16_t> &ix = {})
{
	Builder b;
	b.doc["meshes"].push_back({{"primitives", json::array({lines_part(b, pos, mode, ix)})}});
	b.node({{"mesh", 0}});
	return b;
}

static void test_lines_and_strips()
{
	const std::vector<V3> zig = {{0, 0, 0}, {0.1f, 0.1f, 0}, {0, 0.2f, 0}, {0.1f, 0.3f, 0}, {0, 0.4f, 0}};
	// A line strip: one strand, its points in the file's order.
	{
		Builder b = lines_file(zig, 3);
		const Model m = loaded(b);
		const Primitive &h = part_of(m);
		CHECK(h.strands && h.hair.in_file == 1 && h.hair.strands() == 1);
		CHECK((h.hair.first == std::vector<uint32_t>{0, 5}) && same_points(h.pos, zig));
		// One of each point, the way its strand runs there; no triangles
		// or anything else of a part made of them.
		CHECK(h.normal.size() == 5 && h.hair.width.size() == 5 && h.hair.along.size() == 5);
		CHECK(h.index.empty() && h.uv.empty() && h.colour.empty() && h.morphs.empty() && h.joints.empty() &&
		      h.weights.empty() && h.tangent.empty() && h.hair.colour.empty());
		CHECK(m.strands_in_file == 1 && m.strands == 1 && m.strand_points == 5 && !m.hair_standins);
	}
	// A line strip by point numbers: in their order (here, backwards).
	{
		Builder b = lines_file(zig, 3, {4, 3, 2, 1, 0});
		const Model m = loaded(b);
		const Primitive &h = part_of(m);
		CHECK(h.strands && h.hair.strands() == 1 &&
		      same_points(h.pos, std::vector<V3>(zig.rbegin(), zig.rend())));
	}
	// Lines by point numbers: pieces that share a point make one strand,
	// in whatever order the file lists them.
	{
		const std::vector<V3> two = {{0, 0, 0}, {0.1f, 0.1f, 0}, {0, 0.2f, 0},
					     {1, 0, 0}, {1.1f, 0.1f, 0}, {1, 0.2f, 0}};
		for (int order = 0; order < 2; order++) {
			Builder b = lines_file(two, 1,
					       order ? std::vector<uint16_t>{4, 5, 3, 4, 1, 2, 0, 1}
						     : std::vector<uint16_t>{0, 1, 1, 2, 3, 4, 4, 5});
			const Model m = loaded(b);
			const Primitive &h = part_of(m);
			CHECK(h.strands && h.hair.in_file == 2 && h.hair.strands() == 2 && h.pos.size() == 6);
			if (h.pos.size() != 6)
				continue;
			const size_t a = h.pos[0].x < 0.5f ? 0 : 3;
			CHECK(same_points(std::vector<V3>(h.pos.begin() + a, h.pos.begin() + a + 3),
					  std::vector<V3>(two.begin(), two.begin() + 3)));
			CHECK(same_points(std::vector<V3>(h.pos.begin() + (3 - a), h.pos.begin() + (3 - a) + 3),
					  std::vector<V3>(two.begin() + 3, two.end())));
		}
	}
	// A point keeps the first piece out of it and the first into it: a
	// fork's second arm, a piece from a point to itself and a second way
	// into a point are left out.
	{
		const std::vector<V3> fork = {{0, 0, 0}, {0.1f, 0.1f, 0}, {0, 0.2f, 0}, {0.3f, 0.2f, 0}, {0.3f, 0, 0}};
		Builder b = lines_file(fork, 1, {0, 1, 1, 2, 1, 3, 2, 2, 4, 1});
		const Model m = loaded(b);
		const Primitive &h = part_of(m);
		CHECK(h.strands && h.hair.in_file == 1 && h.hair.strands() == 1);
		CHECK(same_points(h.pos, std::vector<V3>(fork.begin(), fork.begin() + 3)));
	}
	// A ring (no point that no piece comes into) and a lone point: no
	// strands, and no harm - the part is there, empty, beside its body.
	{
		Builder ring = lines_file(zig, 1, {0, 1, 1, 2, 2, 0});
		add_body(ring);
		const Model m = loaded(ring);
		CHECK(part_of(m).strands && part_of(m).hair.in_file == 0 && part_of(m).pos.empty() && m.strands == 0);
		CHECK(m.meshes.size() == 1 && m.meshes[0].parts.size() == 2 && part_of(m, 1).index.size() == 3);
		Builder lone = lines_file({{1, 2, 3}}, 3);
		add_body(lone);
		const Model n = loaded(lone);
		CHECK(part_of(n).strands && part_of(n).hair.strands() == 0 && part_of(n).pos.empty() &&
		      part_of(n).hair.pieces.empty());
		CHECK(n.strands_in_file == 0 && n.strand_points == 0);
	}
	// Points (mode 0) and line loops (mode 2) aren't hair: left out.
	for (int mode : {0, 2}) {
		Builder b = lines_file(zig, mode);
		add_body(b);
		const Model m = loaded(b);
		CHECK(m.meshes.size() == 1 && m.meshes[0].parts.size() == 1 && !part_of(m).strands &&
		      m.strands_in_file == 0);
	}
	// (A file of nothing but strands that come to none has nothing to
	// see in it: refused, as any such file is.)
	{
		Model m;
		std::string why;
		Builder ring = lines_file(zig, 1, {0, 1, 1, 2, 2, 0});
		CHECK(!load(ring, m, LoadOptions(), &why) && !why.empty());
	}
	// A broken file is refused: a piece past its points, point numbers
	// that aren't whole, bones that don't fit its points.
	{
		Model m;
		std::string why;
		Builder past = lines_file(zig, 1, {0, 1, 1, 5});
		CHECK(!load(past, m, LoadOptions(), &why) && !why.empty());
		Builder b;
		json part = lines_part(b, zig, 1);
		part["indices"] = b.floats({0, 1, 1, 2}, 1, "SCALAR");
		b.doc["meshes"].push_back({{"primitives", json::array({part})}});
		b.node({{"mesh", 0}});
		CHECK(!load(b, m, LoadOptions(), &why) && !why.empty());
		Builder c;
		json boned = lines_part(c, zig, 3);
		const std::vector<uint16_t> j(16, 0);
		boned["attributes"]["JOINTS_0"] = c.accessor(c.view(j.data(), 32), 5123, 4, "VEC4");
		boned["attributes"]["WEIGHTS_0"] = c.floats(std::vector<float>(16, 0.25f), 4, "VEC4");
		c.doc["meshes"].push_back({{"primitives", json::array({boned})}});
		c.node({{"mesh", 0}});
		CHECK(!load(c, m, LoadOptions(), &why) && !why.empty());
	}
}

// Whether a file loads, and `right` holds of it, found out in a process
// of its own: a crash there is a failed check here, not the end of the
// tests.
static bool loads_right(Builder &b, const LoadOptions &options, const std::function<bool(const Model &)> &right)
{
	std::fflush(nullptr);
	const pid_t child = fork();
	if (child < 0)
		return false;
	if (child == 0) {
		Model m;
		const bool ok = load(b, m, options) && right(m);
		std::fflush(nullptr);
		_exit(ok ? 0 : 1);
	}
	int status = 0;
	return waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// A hair of just two points - every pair of points of a file's lines
// with no point numbers, a groom of straight bristles - is a strand of
// its root and tip.
static void test_hairs_of_two_points()
{
	const std::vector<V3> pairs = {{0, 0, 0}, {0, 0.1f, 0}, {1, 0, 0}, {1, 0.2f, 0}, {2, 0, 0}, {2, 0.3f, 0}};
	auto three_hairs = [](const Model &m) {
		const Primitive &h = part_of(m);
		bool ok = h.strands && h.hair.in_file == 3 && h.hair.strands() == 3 && h.pos.size() == 6 &&
			  h.normal.size() == 6 && h.hair.pieces.size() == 3;
		ok = ok && m.strands_in_file == 3 && m.strands == 3 && m.strand_points == 6;
		std::set<int> roots;
		for (size_t s = 0; ok && s < 3; s++) {
			const V3 root = h.pos[h.hair.first[s]], tip = h.pos[h.hair.first[s] + 1];
			ok = h.hair.first[s + 1] == h.hair.first[s] + 2 && root.y == 0 && tip.x == root.x &&
			     near(tip.y, 0.1 * (root.x + 1), 1e-6) &&
			     near3(h.normal[h.hair.first[s]], {0, 1, 0}, 1e-6f);
			roots.insert((int)root.x);
		}
		return ok && roots.size() == 3;
	};
	// Lines with no point numbers: every pair of points a strand of its
	// own - as loaded, and with every point kept (strand_bend 0).
	Builder lines = lines_file(pairs, 1);
	CHECK(loads_right(lines, options_with(0), three_hairs));
	CHECK(loads_right(lines, LoadOptions(), three_hairs));
	// The same by point numbers, and held by a bone.
	Groom g;
	for (int s = 0; s < 3; s++) {
		g.strand({pairs[(size_t)s * 2], pairs[(size_t)s * 2 + 1]});
		g.held(0, 1);
		g.held(0, 1);
	}
	Builder plain = lines_file(pairs, 1, {0, 1, 2, 3, 4, 5});
	CHECK(loads_right(plain, LoadOptions(), three_hairs));
	Builder boned = file_of({g}, {{0, 0, 0}});
	CHECK(loads_right(boned, LoadOptions(), [&](const Model &m) {
		return three_hairs(m) && part_of(m).joints.size() == 24 && part_of(m).hair.groups.size() == 1;
	}));
	// Among longer hairs: kept like the rest.
	Groom mixed;
	mixed.strand({{0, 0, 0}, {0.05f, 0.1f, 0}, {0, 0.2f, 0}});
	mixed.strand({{1, 0, 0}, {1, 0.2f, 0}});
	mixed.strand({{2, 0, 0}, {2.05f, 0.1f, 0}, {2, 0.2f, 0}});
	Builder some = file_of({mixed});
	CHECK(loads_right(some, LoadOptions(), [](const Model &m) {
		const Primitive &h = part_of(m);
		bool ok = h.hair.strands() == 3 && h.pos.size() == 8 && h.hair.pieces.size() == 5;
		for (size_t s = 0; ok && s < 3; s++)
			ok = h.hair.first[s + 1] - h.hair.first[s] == (h.pos[h.hair.first[s]].x == 1 ? 2u : 3u);
		return ok;
	}));
}

// Numbers no hair has - places too big for a float's sums, widths and
// bones below nothing - load, or are refused: never worse.
static void test_wild_numbers()
{
	const float big = 3e38f;
	Groom g;
	g.strand({{0, 0, 0}, {big, 0, 0}, {-big, big, 0}, {0, 1, 0}});
	g.strand({{1, 0, 0}, {1, 1e-30f, 0}, {1, 2e-30f, 0}});
	g.strand({{-big, -big, -big}, {big, big, big}, {0, 0, 0}});
	for (size_t i = 0; i < g.pos.size(); i++) {
		g.width.push_back(i % 2 ? -1.0f : big);
		g.colour.insert(g.colour.end(), {big, -big, 2, -3});
		g.joints.insert(g.joints.end(), {65535, 0, 7, 3});
		g.weights.insert(g.weights.end(), {-big, big, big, 0});
	}
	g.shapes.assign(1, std::vector<V3>(g.pos.size(), V3{big, -big, big}));
	// As loaded, with every point kept, and short of points (so its
	// strands are rated by how thickly they cover their skin).
	for (int how = 0; how < 3; how++) {
		Builder b = file_of({g}, {{0, 0, 0}, {big, 0, 0}});
		const std::vector<uint8_t> bytes = b.glb();
		LoadOptions options;
		if (how == 1)
			options.strand_bend = 0;
		if (how == 2)
			options.strand_points = 7;
		std::fflush(nullptr);
		const pid_t child = fork();
		if (child == 0) {
			Model m;
			std::string why;
			bool ok = true;
			if (load_model(bytes.data(), bytes.size(), "", m, &why, options)) {
				// (Loaded: its lists the lengths they say, and posed.)
				const Primitive &h = part_of(m);
				ok = h.normal.size() == h.pos.size() && h.hair.width.size() == h.pos.size() &&
				     h.hair.first.back() == h.pos.size() && h.weights.size() == h.pos.size() * 4 &&
				     (h.hair.anchor.empty() || h.hair.anchor.size() == h.pos.size()) &&
				     h.hair.key.size() == h.hair.strands() && h.hair.rate.size() == h.hair.strands();
				// (Its rates are rates, its keys rise, and however many
				// strands are asked for it answers with some it has.)
				for (size_t k = 0; k < h.hair.strands(); k++)
					ok = ok && h.hair.rate[k] >= 0.25f && h.hair.rate[k] <= 1 &&
					     (k == 0 || h.hair.key[k] >= h.hair.key[k - 1]);
				ok = ok && hair_share(h.hair, h.hair.strands(), 1, 3, 0.001f, 1, 0.4f, 0.5f).count <=
						   h.hair.strands();
				for (uint16_t a : h.hair.anchor)
					ok = ok && a < h.hair.anchors();
				for (const Strands::Group &group : h.hair.groups)
					ok = ok && group.sample < h.pos.size();
				auto shared = std::make_shared<Model>(std::move(m));
				shared->meshes[0].weights.assign(shared->meshes[0].weights.size(), 1.0f);
				Rig rig(shared, RigOptions());
				Drive d;
				d.dt = 1.0f / 60;
				rig.pose(d);
				rig.rest_bounds();
			}
			std::fflush(nullptr);
			_exit(ok ? 0 : 1);
		}
		int status = -1;
		CHECK(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) &&
		      WEXITSTATUS(status) == 0);
	}
}

// --- What's kept of each point ------------------------------------------------

static void test_what_a_point_keeps()
{
	// One strand of four points, three runs 0.3, 0.4 and 0.5 long, each
	// turning a corner; every point kept (strand_bend 0).
	Groom g;
	g.strand({{0, 0, 0}, {0, 0.3f, 0}, {0.4f, 0.3f, 0}, {0.4f, 0.3f, 0.5f}});
	g.width = {0.004f, 0.003f, 0.002f, 0.001f};
	g.colour = {1, 0.5f, 0.2f, 1, 0, 0, 0, 0.5f, 0.5f, 0.5f, 0.5f, 1, 1, 1, 1, 0};
	// Bones as files have them: not adding up to 1, none at all, below 0.
	g.joints = {3, 1, 0, 0, 1, 0, 0, 0, 2, 3, 1, 0, 0, 2, 0, 0};
	g.weights = {2, 2, 0, 0, 0, 0, 0, 0, 0.2f, 0.2f, 0.4f, 0.2f, -1, 3, 0, 0};
	Builder b = file_of({g}, {{0, 0, 0}, {0, 0.1f, 0}, {0, 0.2f, 0}, {0, 0.3f, 0}});
	const Model m = loaded(b, options_with(0));
	const Primitive &h = part_of(m);
	CHECK(h.strands && same_points(h.pos, g.pos) && h.normal.size() == 4);
	if (h.pos.size() != 4 || h.normal.size() != 4)
		return;
	// The way it runs at each: from the point before to the one after.
	CHECK(near3(h.normal[0], {0, 1, 0}, 1e-6f) && near3(h.normal[1], {0.8f, 0.6f, 0}, 1e-6f));
	CHECK(near3(h.normal[2], normalized(V3{0.4f, 0, 0.5f}, {}), 1e-6f) && near3(h.normal[3], {0, 0, 1}, 1e-6f));
	// How far along its strand each is.
	CHECK((h.hair.along.size() == 4) && h.hair.along[0] == 0 && near(h.hair.along[1], 0.25, 1e-6) &&
	      near(h.hair.along[2], 0.7 / 1.2, 1e-6) && h.hair.along[3] == 1);
	// Its width - and the mean of them along the strand's length: 0.0035
	// for 0.3 of it, 0.0025 for 0.4, 0.0015 for 0.5.
	CHECK(h.hair.width == g.width && near(h.hair.mean_width, 0.0028 / 1.2, 1e-8));
	// Its colour, as the screen has it (sRGB bytes); how see-through as
	// the file says.
	CHECK((h.hair.colour ==
	       std::vector<uint8_t>{255, 186, 123, 255, 0, 0, 0, 128, 186, 186, 186, 255, 255, 255, 255, 0}));
	// Its bones the file's, and how much each holds it adding up to 1 (no
	// bone at all: its first, wholly).
	CHECK((h.joints == std::vector<uint16_t>{3, 1, 0, 0, 1, 0, 0, 0, 2, 3, 1, 0, 0, 2, 0, 0}));
	const float want[16] = {0.5f, 0.5f, 0, 0, 1, 0, 0, 0, 0.2f, 0.2f, 0.4f, 0.2f, 0, 1, 0, 0};
	CHECK(h.weights.size() == 16);
	for (size_t i = 0; i < 16 && h.weights.size() == 16; i++)
		CHECK(near(h.weights[i], want[i], 1e-6));

	// A file that says none of it: hairs a tenth of a millimetre wide, no
	// colours, no bones.
	Groom plain;
	plain.strand(g.pos);
	const Model n = loaded(plain, options_with(0));
	const Primitive &q = part_of(n);
	CHECK((q.hair.width == std::vector<float>(4, 0.0001f)) && near(q.hair.mean_width, 0.0001, 1e-10));
	CHECK(q.hair.colour.empty() && q.joints.empty() && q.weights.empty());

	// Colours as bytes, three a point: solid.
	Builder c;
	const std::vector<uint8_t> bytes = {255, 128, 0, 0, 0, 255, 128, 128, 128, 255, 255, 255};
	json part = lines_part(c, g.pos, 3);
	part["attributes"]["COLOR_0"] = c.accessor(c.view(bytes.data(), bytes.size()), 5121, 4, "VEC3", 0, true);
	c.doc["meshes"].push_back({{"primitives", json::array({part})}});
	c.node({{"mesh", 0}});
	const Model o = loaded(c, options_with(0));
	CHECK((part_of(o).hair.colour ==
	       std::vector<uint8_t>{255, 186, 0, 255, 0, 0, 255, 255, 186, 186, 186, 255, 255, 255, 255, 255}));

	// Points left out don't change how far along the rest are: a straight
	// strand 1 long, its points bunched at the root, widest at 0.3 along
	// (so that point is kept, and the two on the even runs either side
	// aren't).
	Groom taper;
	taper.strand({{0, 0, 0}, {0, 0.1f, 0}, {0, 0.3f, 0}, {0, 0.6f, 0}, {0, 1, 0}});
	taper.width = {0.001f, 0.001f + 0.004f / 3, 0.005f, 0.005f - 0.004f * 3 / 7, 0.001f};
	const Model t = loaded(taper);
	const Primitive &tp = part_of(t);
	CHECK((kept_of(taper, tp) == std::vector<int>{0, 2, 4}));
	CHECK(tp.hair.along.size() == 3 && near(tp.hair.along[1], 0.3, 1e-6) && tp.hair.along[2] == 1);
	CHECK((tp.hair.width == std::vector<float>{0.001f, 0.005f, 0.001f}) && near(tp.hair.mean_width, 0.003, 1e-8));
	CHECK(tp.hair.in_file == 1 && t.strand_points == 3);

	// The mean width is along the hairs' lengths, whichever of their
	// points are kept: a straight hair that narrows evenly from 0.006 to
	// 0.002 has it 0.004 - given as 3 points, as 9, as 9 bunched at its
	// root, and with them left out or all kept.
	for (int points : {3, 9, -9}) {
		Groom even;
		const int n = std::abs(points);
		std::vector<V3> p;
		for (int i = 0; i < n; i++) {
			const float along = (float)i / (float)(n - 1), at = points < 0 ? along * along : along;
			p.push_back({0.5f, at, 0});
			even.width.push_back(0.006f - 0.004f * at);
		}
		even.strand(p);
		for (float bend : {0.0003f, 0.0f}) {
			const Model e = loaded(even, options_with(bend));
			CHECK(part_of(e).pos.size() == (bend > 0 ? 2u : (size_t)n));
			CHECK(near(part_of(e).hair.mean_width, 0.004, 2e-8));
		}
	}
	// (And of two hairs, the longer counts for more: one 1 long and 0.001
	// wide, one 3 long and 0.005 wide - 0.004, not 0.003.)
	{
		Groom two;
		two.straight({0, 0, 0}, {0, 1, 0});
		two.straight({1, 0, 0}, {1, 3, 0});
		two.width = {0.001f, 0.001f, 0.001f, 0.005f, 0.005f, 0.005f};
		CHECK(near(part_of(loaded(two)).hair.mean_width, 0.004, 1e-8));
	}
}

// --- The points that shape a strand -------------------------------------------

// Every point of the coat that was left out lies within `bend` of the
// straight run between the kept points either side of it (strand 0 only).
static bool within_bend(const Groom &g, const std::vector<int> &kept, float bend)
{
	for (size_t k = 0; k + 1 < kept.size(); k++) {
		if (kept[k] < 0 || kept[k + 1] <= kept[k])
			return false;
		for (int i = kept[k] + 1; i < kept[k + 1]; i++)
			if (off_run(g.pos[i], g.pos[kept[k]], g.pos[kept[k + 1]]) > bend)
				return false;
	}
	return !kept.empty() && kept.front() == 0 && kept.back() + 1 == (int)g.pos.size();
}

static void test_points_that_shape()
{
	// A straight strand of 11 points, 1 long (so the character is 1 big,
	// and a point bends its strand if it's 0.0003 off): its root and tip.
	Groom straight;
	straight.straight({0, 0, 0}, {0, 1, 0}, 11);
	{
		const Model m = loaded(straight);
		const Primitive &h = part_of(m);
		CHECK((kept_of(straight, h) == std::vector<int>{0, 10}) &&
		      (h.hair.first == std::vector<uint32_t>{0, 2}));
		CHECK(h.hair.in_file == 1 && m.strands_in_file == 1 && m.strand_points == 2);
		CHECK(h.hair.along.size() == 2 && h.hair.along[0] == 0 && h.hair.along[1] == 1);
		CHECK(h.hair.pieces.size() == 1);
	}
	// strand_bend 0: every point.
	{
		const Model m = loaded(straight, options_with(0));
		CHECK(part_of(m).pos.size() == 11 && same_points(part_of(m).pos, straight.pos));
	}
	// One point a millimetre aside: kept, with the points either side of
	// it (the run to it leaves the straight they're on).
	Groom bump = straight;
	bump.pos[5].x = 0.001f;
	{
		const Model m = loaded(bump);
		CHECK((kept_of(bump, part_of(m)) == std::vector<int>{0, 4, 5, 6, 10}));
		// Left out when a bend that big doesn't count...
		const Model coarse = loaded(bump, options_with(0.002f));
		CHECK((kept_of(bump, part_of(coarse)) == std::vector<int>{0, 10}));
	}
	// ...as one too small to see is as the file is: 0.00002 of its size.
	{
		Groom slight = straight;
		slight.pos[5].x = 0.00002f;
		const Model m = loaded(slight);
		CHECK((kept_of(slight, part_of(m)) == std::vector<int>{0, 10}));
	}
	// The size is the whole character's: the same strand, a point 0.0009
	// aside, kept on its own (0.0009 of its size) and left out beside a
	// body 10 big (0.00009 of it).
	{
		Groom small = straight;
		small.pos[5].x = 0.0009f;
		const Model alone = loaded(small);
		CHECK(part_of(alone).pos.size() == 5);
		Builder b = file_of({small});
		const std::vector<V3> body = {{0, 0, 0}, {10, 0, 0}, {0, 0, 1}};
		b.doc["meshes"][0]["primitives"].push_back(
			{{"attributes", {{"POSITION", b.vec3s(body)}}}, {"indices", b.indices({0, 1, 2})}});
		const Model m = loaded(b);
		CHECK(m.meshes[0].parts.size() == 2 && part_of(m, 0).strands && !part_of(m, 1).strands);
		CHECK((kept_of(small, part_of(m, 0)) == std::vector<int>{0, 10}) && part_of(m, 1).index.size() == 3);
		CHECK(m.strands_in_file == 1 && m.strands == 1 && m.strand_points == 2);
	}

	// A curl: a quarter turn of a circle of radius 1, 201 points. However
	// fine it's kept (`bend` of the model's size; by how far its points
	// are off alone, not by how far it turns), no point left out is
	// further than that from the kept run it was on - and the finer, the
	// more points.
	Groom curl;
	{
		std::vector<V3> arc;
		for (int i = 0; i <= 200; i++) {
			const float a = 1.5707963f * (float)i / 200;
			arc.push_back({std::cos(a), std::sin(a), 0});
		}
		curl.strand(arc);
	}
	const float size = std::sqrt(2.0f); // its box: 1 by 1
	size_t before = 202;
	for (float bend : {0.0003f, 0.001f, 0.005f, 0.05f}) {
		const Model m = loaded(curl, options_with(bend));
		const std::vector<int> kept = kept_of(curl, part_of(m));
		CHECK(within_bend(curl, kept, bend * size * 1.001f + 1e-7f));
		// (A run that turns `a` is 1 - cos(a / 2) off its arc at most:
		// at least this many runs.)
		const double turn = 2 * std::acos(1 - (double)bend * size);
		CHECK(kept.size() >= (size_t)std::ceil(1.5707963 / turn) + 1 && kept.size() < before);
		// (And no more than twice the fewest that would do.)
		CHECK(kept.size() <= 2 * (size_t)std::ceil(1.5707963 / turn) + 2);
		before = kept.size();
	}
	// (At 0.05: its ends, its middle and its quarters.)
	CHECK(before == 5);

	// A hair that thickens in the middle keeps the point, by more than a
	// tenth of its widest (0.0012 of 0.012)...
	Groom wide;
	wide.straight({0, 0, 0}, {0, 1, 0}, 5);
	wide.width = {0.010f, 0.010f, 0.012f, 0.010f, 0.010f};
	CHECK((kept_of(wide, part_of(loaded(wide))) == std::vector<int>{0, 2, 4}));
	// ...not by less (0.0009 of 0.0109)...
	wide.width[2] = 0.0109f;
	CHECK((kept_of(wide, part_of(loaded(wide))) == std::vector<int>{0, 4}));
	// ...and one that narrows evenly root to tip needs none (its ends
	// say it all).
	wide.width = {0.008f, 0.006f, 0.004f, 0.002f, 0};
	{
		const Model m = loaded(wide);
		CHECK((kept_of(wide, part_of(m)) == std::vector<int>{0, 4}));
		CHECK((part_of(m).hair.width == std::vector<float>{0.008f, 0}) &&
		      near(part_of(m).hair.mean_width, 0.004, 1e-9));
	}

	// A colour that changes by a shade you'd see keeps the point (its
	// green 186 to 196 of 255: more than 6)...
	Groom dyed;
	dyed.straight({0, 0, 0}, {0, 1, 0}, 5);
	for (int i = 0; i < 5; i++)
		dyed.colour.insert(dyed.colour.end(), {0.5f, 0.5f, 0.5f, 1});
	dyed.colour[2 * 4 + 1] = 0.56f;
	{
		const Model m = loaded(dyed);
		CHECK((kept_of(dyed, part_of(m)) == std::vector<int>{0, 2, 4}));
		CHECK((part_of(m).hair.colour ==
		       std::vector<uint8_t>{186, 186, 186, 255, 186, 196, 186, 255, 186, 186, 186, 255}));
	}
	// ...one you wouldn't doesn't (186 to 188)...
	dyed.colour[2 * 4 + 1] = 0.51f;
	CHECK((kept_of(dyed, part_of(loaded(dyed))) == std::vector<int>{0, 4}));
	// ...nor a hair that fades evenly to see-through at its tip.
	for (int i = 0; i < 5; i++)
		dyed.colour[i * 4 + 1] = 0.5f, dyed.colour[i * 4 + 3] = 1 - i * 0.25f;
	CHECK((kept_of(dyed, part_of(loaded(dyed))) == std::vector<int>{0, 4}));
	// (A point where it turns see-through is kept.)
	dyed.colour[2 * 4 + 3] = 0.2f;
	CHECK((kept_of(dyed, part_of(loaded(dyed))) == std::vector<int>{0, 1, 2, 3, 4}));
}

// A straight strand 1 long, 801 points, hung on a chain of 5 bones: held
// wholly by bone 0 at its root, by bone 1 a quarter along, and so on,
// each bone handing over to the next evenly in between.
static Groom chain_strand(int points = 801)
{
	Groom g;
	g.straight({0, 1, 0}, {0, 0, 0}, points);
	for (int i = 0; i < points; i++) {
		const float along = 4.0f * (float)i / (float)(points - 1);
		const int bone = std::min(3, (int)along);
		const float share = std::min(1.0f, along - (float)bone);
		g.held((uint16_t)bone, 1 - share, (uint16_t)(bone + 1), share);
	}
	return g;
}
static const std::vector<V3> chain_bones = {{0, 1, 0}, {0, 0.75f, 0}, {0, 0.5f, 0}, {0, 0.25f, 0}, {0, 0, 0}};

static void test_bones_that_swing()
{
	const Groom g = chain_strand();
	// Each kept run's ends hang from bones no more unlike than its length
	// allows (unlike x length within `swing`), and the bones of every
	// point left out are within 0.05 of an even change between the kept
	// points either side of it.
	auto checked = [&g](const Primitive &h, float swing) {
		const std::vector<int> kept = kept_of(g, h);
		bool ok = !kept.empty() && kept.front() == 0 && kept.back() == 800 &&
			  h.joints.size() == kept.size() * 4;
		for (size_t k = 0; ok && k + 1 < kept.size(); k++) {
			const int a = kept[k], c = kept[k + 1];
			ok = a >= 0 && c > a;
			if (!ok)
				break;
			// (The kept points' bones are the file's.)
			ok = unlike(&h.joints[k * 4], &h.weights[k * 4], &g.joints[a * 4], &g.weights[a * 4]) < 1e-5f;
			const float run = length(g.pos[c] - g.pos[a]);
			const float apart =
				unlike(&g.joints[a * 4], &g.weights[a * 4], &g.joints[c * 4], &g.weights[c * 4]);
			if (swing > 0 && c > a + 1 && apart * run > swing * 1.001f)
				ok = false;
			for (int i = a + 1; ok && i < c; i++)
				ok = uneven(g, i, a, c, length(g.pos[i] - g.pos[a]) / run) <= 0.05f + 1e-4f;
		}
		return ok;
	};
	Builder b = file_of({g}, chain_bones);
	// As loaded: 0.004 of its size. A run inside one bone's hand-over (a
	// quarter long, its ends 1 unlike) is halved three times: 0.03125
	// long, its ends 0.125 unlike - 0.0039. 8 runs each, 33 points.
	{
		const Model m = loaded(b);
		const Primitive &h = part_of(m);
		CHECK(checked(h, 0.004f));
		CHECK(h.pos.size() == 33 && h.hair.strands() == 1 && h.hair.in_file == 1);
		const std::vector<int> kept = kept_of(g, h);
		bool even = kept.size() == 33;
		for (size_t k = 0; even && k < kept.size(); k++)
			even = kept[k] == (int)k * 25;
		CHECK(even);
		// Straight still: its points left out change nothing of its shape.
		CHECK(within_bend(g, kept, 1e-6f));
	}
	// Twice the swing allowed: runs twice as long (0.0625: 0.0156), 17
	// points. Light's 0.008: between the two - the same 33.
	{
		const Model m = loaded(b, options_with(0.0003f, 0.016f));
		CHECK(checked(part_of(m), 0.016f) && part_of(m).pos.size() == 17);
		const Model l = loaded(b, hair_load_options(HairDetail::Light, true));
		CHECK(checked(part_of(l), 0.008f) && part_of(l).pos.size() == 33);
	}
	// No swing asked for: only the points where its bones change hands.
	{
		const Model m = loaded(b, options_with(0.0003f, 0));
		CHECK(checked(part_of(m), 0));
		CHECK((kept_of(g, part_of(m)) == std::vector<int>{0, 200, 400, 600, 800}));
	}
	// strand_bend 0 keeps every point, bones or no.
	CHECK(part_of(loaded(b, options_with(0, 0.004f))).pos.size() == 801);

	// The same strand held by one bone all the way: its root and tip.
	Groom one;
	one.straight({0, 1, 0}, {0, 0, 0}, 801);
	for (int i = 0; i < 801; i++)
		one.held(2, 1);
	Builder ob = file_of({one}, chain_bones);
	{
		const Model m = loaded(ob);
		const Primitive &h = part_of(m);
		CHECK((kept_of(one, h) == std::vector<int>{0, 800}));
		CHECK((h.joints == std::vector<uint16_t>{2, 0, 0, 0, 2, 0, 0, 0}) &&
		      (h.weights == std::vector<float>{1, 0, 0, 0, 1, 0, 0, 0}));
	}
	// And by two bones evenly shared all the way (nothing to bend it
	// between): its root and tip.
	Groom shared;
	shared.straight({0, 1, 0}, {0, 0, 0}, 801);
	for (int i = 0; i < 801; i++)
		shared.held(1, 0.5f, 3, 0.5f);
	Builder sb = file_of({shared}, chain_bones);
	CHECK((kept_of(shared, part_of(loaded(sb))) == std::vector<int>{0, 800}));

	// A short hair on the same chain (a hundredth as long, in a
	// character still 1 big): its bones can't bend it enough to see.
	Groom shorter = chain_strand(41);
	for (V3 &v : shorter.pos)
		v.y = 1 - (1 - v.y) * 0.01f;
	Builder hb = file_of({shorter, g}, chain_bones);
	{
		const Model m = loaded(hb, options_with(0.0003f, 0.004f));
		// (Kept: its ends and the 3 points where one bone hands it to the
		// next.)
		CHECK((kept_of(shorter, part_of(m, 0)) == std::vector<int>{0, 10, 20, 30, 40}));
		CHECK(part_of(m, 1).pos.size() == 33 && m.strand_points == 38 && m.strands == 2);
	}
}

// --- The points that keep a curl round ------------------------------------------

// A curl: `pieces` pieces round a circle of `radius`, the first setting
// out from `from` straight up, each turned `step` degrees on from the one
// before (towards +x, then down), `rise` further along z with each. Its
// points, the one at `from` first.
static std::vector<V3> curl(V3 from, double radius, int pieces, double step, double rise = 0)
{
	std::vector<V3> out;
	for (int k = 0; k <= pieces; k++) {
		const double turned = step * (double)k * 3.14159265358979 / 180;
		out.push_back(from + V3{(float)(radius * (1 - std::cos(turned))), (float)(radius * std::sin(turned)),
					(float)(rise * (double)k)});
	}
	return out;
}

// How far a part's strands turn between two kept points next to each other
// on a hair: the angle, in degrees, between the file's piece out of the
// one and its piece into the other - the most of any two. (-1: a point
// kept that isn't the file's, or out of order.)
static double most_turned(const Groom &g, const Primitive &p)
{
	const std::vector<int> kept = kept_of(g, p);
	double most = 0;
	for (size_t s = 0; s < p.hair.strands(); s++)
		for (uint32_t i = p.hair.first[s]; i + 1 < p.hair.first[s + 1]; i++) {
			const int a = kept[i], b = kept[i + 1];
			if (a < 0 || b <= a || g.strand_of((size_t)a) != g.strand_of((size_t)b))
				return -1;
			const V3 out = g.pos[(size_t)a + 1] - g.pos[(size_t)a],
				 in = g.pos[(size_t)b] - g.pos[(size_t)b - 1];
			const double ox = out.x, oy = out.y, oz = out.z, ix = in.x, iy = in.y, iz = in.z;
			const double lengths = std::sqrt((ox * ox + oy * oy + oz * oz) * (ix * ix + iy * iy + iz * iz));
			if (!(lengths > 0))
				continue;
			const double c = (ox * ix + oy * iy + oz * iz) / lengths;
			most = std::max(most, std::acos(std::min(1.0, std::max(-1.0, c))) * 180 / 3.14159265358979);
		}
	return most;
}

// How many points are kept of the coat's strand `s`.
static size_t kept_on(const Groom &g, const Primitive &p, size_t s)
{
	size_t n = 0;
	for (int at : kept_of(g, p))
		n += at >= 0 && g.strand_of((size_t)at) == s ? 1 : 0;
	return n;
}

static void test_turns()
{
	// A ruler - one straight hair a metre long, so the coat is a metre
	// across and a point is left out where it bends its strand under 0.3 mm
	// (strand_bend 0.0003 of the coat's size) - beside each hair tried.
	auto beside_ruler = [](const std::vector<V3> &hair) {
		Groom g;
		g.strand(hair);
		g.straight({0, 0, 0.01f}, {0, 1, 0.01f}, 2);
		return g;
	};
	const LoadOptions full = hair_load_options(HairDetail::Full, true),
			  light = hair_load_options(HairDetail::Light, true);
	const float swing = LoadOptions().strand_swing;

	// A curl a fifth of a millimetre across: three quarters of a circle of
	// 0.1 mm radius, in 60 pieces each turned 4.5 degrees on. No point of
	// it is 0.3 mm off the straight run from its root to its tip (the
	// furthest, 0.17 mm), so going by that alone only those two are kept:
	// a curl drawn as one straight piece.
	{
		const Groom g = beside_ruler(curl({}, 0.0001, 60, 4.5));
		CHECK(g.strands() == 2 && g.pos.size() == 63);
		const Model flat = loaded(g, options_with(0.0003f));
		CHECK(part_of(flat).pos.size() == 4 && kept_on(g, part_of(flat), 0) == 2);
		// (Its first and last pieces are 59 turns of 4.5 degrees apart:
		// 265.5 degrees round, so 94.5 from each other.)
		CHECK(near(most_turned(g, part_of(flat)), 360 - 59 * 4.5, 0.1));
		// As the hair detail setting loads it, a run that turns more than
		// 20 degrees from its first piece to its last is split at its
		// furthest point (its middle) until none does. 60 pieces (59 turns
		// of 4.5 degrees) halve to 30, to 15, to 7 and 8 (6 and 7 turns: 27
		// and 31.5 degrees), to 3 and 4, and 4 and 4 (13.5 degrees at
		// most): 16 runs, 17 points.
		const Model round = loaded(g, full);
		CHECK(kept_on(g, part_of(round), 0) == 17 && part_of(round).pos.size() == 19);
		const double turned = most_turned(g, part_of(round));
		CHECK(turned > 13.4 && turned < 13.6);
		// (The same as loaded with nothing said, and with the turn said
		// alone.)
		CHECK(same_points(part_of(loaded(g)).pos, part_of(round).pos));
		CHECK(same_points(part_of(loaded(g, options_with(0.0003f, swing, 20))).pos, part_of(round).pos));
		// Light lets a run turn 30 degrees: the runs of 7 pieces (27
		// degrees) are left whole - 12 runs, 13 points.
		const Model lighter = loaded(g, light);
		CHECK(kept_on(g, part_of(lighter), 0) == 13);
		const double light_turned = most_turned(g, part_of(lighter));
		CHECK(light_turned > 26.9 && light_turned < 27.1);
		// Whatever the limit: no kept run turns more than it, with no more
		// points than twice the fewest that could do it (a run that turns
		// just too far is halved).
		for (float limit : {5.0f, 10.0f, 20.0f, 30.0f, 50.0f, 60.0f, 80.0f}) {
			const Model m = loaded(g, options_with(0.0003f, swing, limit));
			const size_t kept = kept_on(g, part_of(m), 0);
			// (A run of q pieces turns 4.5 x (q - 1) degrees.)
			const size_t longest = (size_t)(limit / 4.5f) + 1, fewest = (60 + longest - 1) / longest + 1;
			const double most = most_turned(g, part_of(m));
			CHECK(most >= 0 && most <= limit && kept >= fewest && kept <= 2 * fewest);
		}
		// No limit (strand_turn 0 or less, or no number), or half a turn or
		// more: the rule is off.
		for (float off : {0.0f, -20.0f, 180.0f, 360.0f, std::numeric_limits<float>::quiet_NaN()})
			CHECK(same_points(part_of(loaded(g, options_with(0.0003f, swing, off))).pos,
					  part_of(flat).pos));
	}
	// A ringlet 2 mm across (1 mm radius, the same 60 pieces): going by how
	// far off a straight run its points are alone, it's halved twice - its
	// middle 1.7 mm off the whole run, each half's 0.62 mm off its own, each
	// quarter's 0.17 mm - and left 4 runs of 15 pieces, a row of corners of
	// 63 degrees. With the rule each quarter is split as above: 17 points.
	{
		const Groom g = beside_ruler(curl({}, 0.001, 60, 4.5));
		const Model corners = loaded(g, options_with(0.0003f));
		CHECK(kept_on(g, part_of(corners), 0) == 5 && near(most_turned(g, part_of(corners)), 63, 0.1));
		const Model round = loaded(g, full);
		CHECK(kept_on(g, part_of(round), 0) == 17 && near(most_turned(g, part_of(round)), 13.5, 0.1));
		// (Every point the looser way keeps, the rule keeps too.)
		std::vector<int> few = kept_of(g, part_of(corners)), many = kept_of(g, part_of(round));
		std::sort(few.begin(), few.end());
		std::sort(many.begin(), many.end());
		CHECK(std::includes(many.begin(), many.end(), few.begin(), few.end()));
	}
	// A hair 5 cm long with the first curl at its tip (a hook): a shaft of
	// 5 straight pieces, then the curl's 60. Root to tip it turns too far,
	// and is split at its furthest point from that run: the top of the
	// hook, 20 pieces round it (0.2 mm from its tip). The part before, at
	// where the shaft ends (0.1 mm off): the shaft is one run. The hook's
	// first 20 pieces and its last 40 are halved down to runs of 5 (18
	// degrees) - 12 of them. 13 runs, 14 points of its 66: the fewest that
	// could do (a run out of the shaft could take 4 of the hook's pieces,
	// 15.75 degrees; the other 56 are 12 runs of 5 at most).
	{
		std::vector<V3> hook;
		for (int i = 0; i < 5; i++)
			hook.push_back({0.02f, 0.01f * (float)i, 0});
		const std::vector<V3> tip = curl({0.02f, 0.05f, 0}, 0.0001, 60, 4.5);
		hook.insert(hook.end(), tip.begin(), tip.end());
		const Groom g = beside_ruler(hook);
		CHECK(g.first[1] == 66 && kept_on(g, part_of(loaded(g, options_with(0.0003f))), 0) == 2);
		const Model m = loaded(g, full);
		const double turned = most_turned(g, part_of(m));
		CHECK(kept_on(g, part_of(m), 0) == 14 && turned > 17.9 && turned < 18.1);
		// (Its shaft's end and the hook's top among them.)
		const std::vector<int> kept = kept_of(g, part_of(m));
		CHECK(std::count(kept.begin(), kept.end(), 5) == 1 && std::count(kept.begin(), kept.end(), 25) == 1 &&
		      std::count(kept.begin(), kept.end(), 1) + std::count(kept.begin(), kept.end(), 2) +
				      std::count(kept.begin(), kept.end(), 3) +
				      std::count(kept.begin(), kept.end(), 4) ==
			      0);
	}
	// Arcs of 3 to 20 pieces, each turned 2 degrees on, a hair each (and a
	// straight hair of 50 points, on a slant): a run of q pieces turns
	// 2 x (q - 1) degrees, so up to 10 pieces (18 degrees) are left one
	// run - root and tip - and 12 to 20 pieces (22 to 38 degrees) are split
	// once, in the middle: 3 points. Light, at 30 degrees, leaves up to 15
	// pieces (28) whole. (11 and 16 pieces turn just 20 and 30: not tried.)
	{
		Groom g;
		std::vector<int> pieces;
		for (int q = 3; q <= 20; q++) {
			if (q == 11 || q == 16)
				continue;
			g.strand(curl({0.001f * (float)q, 0, 0}, 0.0001, q, 2));
			pieces.push_back(q);
		}
		g.straight({0.05f, 0, 0}, {0.15f, 0.25f, 0.1f}, 50);
		g.straight({0, 0, 0.01f}, {0, 1, 0.01f}, 2);
		const Model as_full = loaded(g, full), as_light = loaded(g, light),
			    flat = loaded(g, options_with(0.0003f));
		bool right = true, right_light = true, all_flat = true;
		for (size_t s = 0; s < pieces.size(); s++) {
			right = right && kept_on(g, part_of(as_full), s) == (pieces[s] <= 10 ? 2u : 3u);
			right_light = right_light && kept_on(g, part_of(as_light), s) == (pieces[s] <= 15 ? 2u : 3u);
			all_flat = all_flat && kept_on(g, part_of(flat), s) == 2;
		}
		CHECK(right);
		CHECK(right_light);
		CHECK(all_flat);
		// (Split in the middle: the two runs turn 18 degrees at most.)
		CHECK(most_turned(g, part_of(as_full)) < 18.1 && most_turned(g, part_of(as_light)) < 28.1);
		// The straight hair: its root and tip, rule or no rule.
		for (const Model *m : {&as_full, &as_light, &flat})
			CHECK(kept_on(g, part_of(*m), pieces.size()) == 2 &&
			      part_of(*m).hair.strands() == pieces.size() + 2);
	}
	// A ringlet 10 cm across (two turns of a 5 cm radius, in pieces turned
	// 3 degrees on, rising as it goes): its points are kept for how far off
	// a straight run they are (0.3 mm off is a run of 12 degrees or so) long
	// before any run turns 20 - the same points, rule or no rule.
	{
		const Groom g = beside_ruler(curl({0.2f, 0, 0}, 0.05, 240, 3, 0.0005));
		const Model with = loaded(g, full), without = loaded(g, options_with(0.0003f));
		CHECK(same_points(part_of(with).pos, part_of(without).pos));
		const size_t kept = kept_on(g, part_of(with), 0);
		CHECK(kept >= 61 && kept <= 121 && most_turned(g, part_of(with)) < 9.1);
	}
	// A point kept for something else comes first: an arc of 4 pieces, each
	// turned 8 degrees on (24 from its first to its last), that narrows
	// fast over its first piece and evenly after. Its second point is the
	// one a straight taper from root to tip gets most wrong, so that's
	// where it's split - and the run from there to the tip turns 16
	// degrees, which is let be: 3 points. (Split for the turn first, at
	// its middle, it would keep the second point as well: 4.)
	{
		Groom g = beside_ruler(curl({0.02f, 0, 0}, 0.0001, 4, 8));
		g.width = {0.00010f, 0.00004f, 0.00003f, 0.00002f, 0.00001f, 0.0001f, 0.0001f};
		// (The file's points kept, in the file's order: the ruler's two
		// are 5 and 6.)
		auto kept_in_order = [](const Groom &of, const Model &m) {
			std::vector<int> kept = kept_of(of, part_of(m));
			std::sort(kept.begin(), kept.end());
			return kept;
		};
		const Model m = loaded(g, full);
		CHECK((kept_in_order(g, m) == std::vector<int>{0, 1, 4, 5, 6}));
		CHECK(near(most_turned(g, part_of(m)), 16, 0.1));
		// (With no widths said, it's split in the middle for the turn.)
		g.width.clear();
		CHECK((kept_in_order(g, loaded(g, full)) == std::vector<int>{0, 2, 4, 5, 6}));
	}
}

// --- The order strands are drawn in ---------------------------------------------

// How many of the part's first `count` strands have their root in each
// cell of a grid `cells` a side over the box lo to hi (cells^3 numbers).
static std::vector<int> roots_in_cells(const Primitive &h, size_t count, int cells, V3 lo, V3 hi)
{
	std::vector<int> n((size_t)cells * cells * cells, 0);
	auto cell = [cells](float v, float a, float b) {
		return b > a ? std::min(cells - 1, std::max(0, (int)((v - a) / (b - a) * (float)cells))) : 0;
	};
	for (size_t s = 0; s < count && s < h.hair.strands(); s++) {
		const V3 r = h.pos[h.hair.first[s]];
		n[(size_t)cell(r.x, lo.x, hi.x) +
		  (size_t)cells * ((size_t)cell(r.y, lo.y, hi.y) + (size_t)cells * (size_t)cell(r.z, lo.z, hi.z))]++;
	}
	return n;
}

// The same for roots on the ground: a flat grid, cells x cells, over x
// and z from `lo` to `hi`.
static std::vector<int> roots_in_squares(const Primitive &h, size_t count, int cells, float lo, float hi)
{
	std::vector<int> n((size_t)cells * cells, 0);
	auto cell = [cells, lo, hi](float v) {
		return std::min(cells - 1, std::max(0, (int)((v - lo) / (hi - lo) * (float)cells)));
	};
	for (size_t s = 0; s < count && s < h.hair.strands(); s++) {
		const V3 r = h.pos[h.hair.first[s]];
		n[(size_t)cell(r.x) + (size_t)cells * (size_t)cell(r.z)]++;
	}
	return n;
}

static int spread_of(const std::vector<int> &n)
{
	return *std::max_element(n.begin(), n.end()) - *std::min_element(n.begin(), n.end());
}

static void test_spread()
{
	// A lawn: 32 x 32 roots a step apart on the ground, a hair up from
	// each. The first 4 drawn are one from each quarter of it, the first
	// 16 one from each sixteenth, and so on: however few are drawn,
	// they're spread evenly.
	{
		Groom g;
		for (int z = 0; z < 32; z++)
			for (int x = 0; x < 32; x++)
				g.straight({(float)x, 0, (float)z}, {(float)x, 0.5f, (float)z});
		const Model m = loaded(g);
		const Primitive &h = part_of(m);
		CHECK(h.hair.in_file == 1024 && h.hair.strands() == 1024 && h.pos.size() == 2048);
		CHECK((roots_in_squares(h, 4, 2, -0.5f, 31.5f) == std::vector<int>(4, 1)));
		CHECK((roots_in_squares(h, 16, 4, -0.5f, 31.5f) == std::vector<int>(16, 1)));
		CHECK((roots_in_squares(h, 64, 8, -0.5f, 31.5f) == std::vector<int>(64, 1)));
		CHECK((roots_in_squares(h, 128, 8, -0.5f, 31.5f) == std::vector<int>(64, 2)));
		CHECK((roots_in_squares(h, 256, 16, -0.5f, 31.5f) == std::vector<int>(256, 1)));
		CHECK((roots_in_squares(h, 512, 16, -0.5f, 31.5f) == std::vector<int>(256, 2)));
		CHECK((roots_in_squares(h, 1024, 32, -0.5f, 31.5f) == std::vector<int>(1024, 1)));
		// Any other number of them: its quarters and sixteenths within
		// one of each other.
		for (size_t count : {3u, 10u, 37u, 100u, 333u, 900u}) {
			CHECK(spread_of(roots_in_squares(h, count, 2, -0.5f, 31.5f)) <= 1);
			CHECK(spread_of(roots_in_squares(h, count, 4, -0.5f, 31.5f)) <= 1);
		}
		// Every strand once, each a hair straight up from its root.
		std::set<std::pair<int, int>> roots;
		bool whole = true;
		for (size_t s = 0; s < h.hair.strands(); s++) {
			const V3 r = h.pos[h.hair.first[s]], t = h.pos[h.hair.first[s] + 1];
			roots.insert({(int)r.x, (int)r.z});
			whole = whole && r.y == 0 && t.y == 0.5f && t.x == r.x && t.z == r.z;
		}
		CHECK(whole && roots.size() == 1024);
	}
	// A ball of fur: 16 x 16 x 16 roots. The first 8 are one from each
	// eighth of its box, the first 64 and 512 one from each cell 4 and 8 a
	// side.
	{
		Groom g;
		for (int z = 0; z < 16; z++)
			for (int y = 0; y < 16; y++)
				for (int x = 0; x < 16; x++)
					g.straight({(float)x, (float)y, (float)z}, {x + 0.2f, y + 0.2f, z + 0.2f});
		const Model m = loaded(g);
		const Primitive &h = part_of(m);
		CHECK(h.hair.strands() == 4096);
		const V3 lo{-0.5f, -0.5f, -0.5f}, hi{15.5f, 15.5f, 15.5f};
		CHECK((roots_in_cells(h, 8, 2, lo, hi) == std::vector<int>(8, 1)));
		CHECK((roots_in_cells(h, 64, 4, lo, hi) == std::vector<int>(64, 1)));
		CHECK((roots_in_cells(h, 512, 8, lo, hi) == std::vector<int>(512, 1)));
		CHECK((roots_in_cells(h, 1024, 8, lo, hi) == std::vector<int>(512, 2)));
		CHECK((roots_in_cells(h, 4096, 16, lo, hi) == std::vector<int>(4096, 1)));
	}
	// Roots scattered anyhow (3000 of them over a square, as a groom's
	// are): the first eighth of them touch every cell of a grid 8 a side,
	// and any first so many are shared between its quarters as the whole
	// coat is, give or take a few.
	{
		Groom g;
		uint32_t seed = 7;
		for (int i = 0; i < 3000; i++) {
			const float x = chance(seed), z = chance(seed);
			g.straight({x, 0, z}, {x, 0.05f, z});
		}
		const Model m = loaded(g);
		const Primitive &h = part_of(m);
		CHECK(h.hair.strands() == 3000);
		const std::vector<int> eighth = roots_in_squares(h, 375, 8, 0, 1);
		CHECK(*std::min_element(eighth.begin(), eighth.end()) >= 1);
		const std::vector<int> all = roots_in_squares(h, 3000, 2, 0, 1);
		for (size_t count : {40u, 100u, 375u, 750u, 1500u, 2400u}) {
			const std::vector<int> n = roots_in_squares(h, count, 2, 0, 1);
			for (int q = 0; q < 4; q++)
				CHECK(near(n[q], (double)all[q] * (double)count / 3000, 4));
		}
	}
}

// --- The points allowed -------------------------------------------------------

// A lawn of `across` x `deep` hairs a step apart, each of `points` points
// 0.1 up from the last: straight (so 2 are kept, its root and tip), or
// with a kink at each point between, a quarter of a step aside (all
// kept).
constexpr float KINK = 0.25f;
static Groom lawn(int across, int deep, int points = 3, bool kinked = false, V3 from = {})
{
	Groom g;
	for (int z = 0; z < deep; z++)
		for (int x = 0; x < across; x++) {
			std::vector<V3> s;
			for (int i = 0; i < points; i++)
				s.push_back(from +
					    V3{(float)x + (kinked && i % 2 ? KINK : 0.0f), 0.1f * (float)i, (float)z});
			g.strand(s);
		}
	return g;
}

static LoadOptions allowed(size_t points, size_t bare = LoadOptions().strand_points_bare)
{
	LoadOptions o;
	o.strand_points = points;
	o.strand_points_bare = bare;
	return o;
}

// The strands of `some` are the first of `all`'s, point for point.
static bool first_of(const Primitive &some, const Primitive &all)
{
	if (some.pos.size() > all.pos.size() || some.hair.first.size() > all.hair.first.size())
		return false;
	for (size_t i = 0; i < some.pos.size(); i++)
		if (!near3(some.pos[i], all.pos[i], 0) || !near3(some.normal[i], all.normal[i], 0) ||
		    some.hair.width[i] != all.hair.width[i] || some.hair.along[i] != all.hair.along[i])
			return false;
	return std::equal(some.hair.first.begin(), some.hair.first.end(), all.hair.first.begin());
}

static void test_points_allowed()
{
	// 1000 hairs of 2 points. All of them fit as loaded.
	const Groom g = lawn(40, 25);
	const Model whole = loaded(g);
	const Primitive &all = part_of(whole);
	CHECK(all.hair.strands() == 1000 && whole.strand_points == 2000);
	// 500 points allowed: the first 250 strands of its draw order - the
	// file's count still all of them.
	for (size_t points : {500u, 501u}) {
		const Model m = loaded(g, allowed(points));
		const Primitive &h = part_of(m);
		CHECK(h.hair.in_file == 1000 && h.hair.strands() == 250 && h.pos.size() == 500);
		CHECK(m.strands_in_file == 1000 && m.strands == 250 && m.strand_points == 500);
		CHECK(first_of(h, all));
		CHECK(h.hair.width.size() == 500 && h.hair.along.size() == 500 && h.normal.size() == 500 &&
		      h.hair.pieces.size() == 250);
	}
	// Exactly enough: all; one too few: all but one; fewer than one
	// strand's: none.
	CHECK(part_of(loaded(g, allowed(2000))).hair.strands() == 1000);
	CHECK(part_of(loaded(g, allowed(1999))).hair.strands() == 999);
	{
		Builder b = file_of({g});
		add_body(b);
		const Model m = loaded(b, allowed(1));
		const Primitive &h = part_of(m);
		CHECK(h.strands && h.hair.in_file == 1000 && h.hair.strands() == 0 && h.pos.empty());
		CHECK(h.hair.pieces.empty() && h.hair.groups.empty() && m.strands == 0 && m.strand_points == 0);
		CHECK(m.strands_in_file == 1000 && part_of(m, 1).index.size() == 3);
	}

	// Hairs of different lengths (3 to 7 kept points): whole strands only,
	// the first of the order, never past the points allowed - and the next
	// one wouldn't have fitted.
	Groom mixed;
	for (int i = 0; i < 600; i++) {
		std::vector<V3> s;
		for (int k = 0; k < 3 + i % 5; k++)
			s.push_back({(float)(i % 30) + (k % 2 ? 0.05f : 0.0f), 0.1f * (float)k, (float)(i / 30)});
		mixed.strand(s);
	}
	const Model mixed_whole = loaded(mixed);
	const Primitive &mixed_all = part_of(mixed_whole);
	CHECK(mixed_all.hair.strands() == 600 && mixed_all.pos.size() == 3000);
	for (size_t points : {7u, 100u, 999u, 1000u, 1700u, 2999u}) {
		const Model m = loaded(mixed, allowed(points));
		const Primitive &h = part_of(m);
		const size_t kept = h.hair.strands();
		CHECK(h.pos.size() <= points && first_of(h, mixed_all) && kept < 600);
		CHECK(kept < 600 && mixed_all.hair.first[kept + 1] > points);
		CHECK(m.strand_points == h.pos.size() && m.strands == kept && m.strands_in_file == 600);
	}

	// Two coats, 3000 hairs and 1000 (6000 points and 2000), and 2000
	// points for both: each keeps the same share of its own, a quarter.
	{
		const Groom big = lawn(60, 50), small = lawn(40, 25, 3, false, {100, 0, 0});
		Builder b = file_of({big, small});
		const Model m = loaded(b, allowed(2000));
		CHECK(part_of(m, 0).hair.strands() == 750 && part_of(m, 1).hair.strands() == 250);
		CHECK(m.strands_in_file == 4000 && m.strands == 1000 && m.strand_points == 2000);
		const Model every = loaded(b, allowed(8000));
		CHECK(part_of(every, 0).hair.strands() == 3000 && part_of(every, 1).hair.strands() == 1000);
		CHECK(first_of(part_of(m, 0), part_of(every, 0)) && first_of(part_of(m, 1), part_of(every, 1)));
		// (One point short of all: each a little short.)
		const Model nearly = loaded(b, allowed(7999));
		CHECK(nearly.strand_points <= 7999 && nearly.strand_points >= 7994);
	}
	// By the points each would keep, not its strands: 1000 kinked hairs of
	// 3 points beside 500 straight ones of 2 (3000 points and 1000), 1000
	// points for both: a quarter of each.
	{
		const Groom kinked = lawn(40, 25, 3, true), plain = lawn(25, 20, 5, false, {100, 0, 0});
		Builder b = file_of({kinked, plain});
		const Model m = loaded(b, allowed(1000));
		CHECK(part_of(m, 0).hair.strands() == 250 && part_of(m, 0).pos.size() == 750);
		CHECK(part_of(m, 1).hair.strands() == 125 && part_of(m, 1).pos.size() == 250);
		CHECK(m.strands_in_file == 1500 && m.strands == 375 && m.strand_points == 1000);
	}
}

// --- How thickly hair covers its skin -----------------------------------------

// A flat patch of fur: `across` x `deep` roots on the ground from `from`,
// `dx` and `dz` apart, each a straight hair `length` long (straight up) and
// `width` wide, given as `points` points.
static Groom patch(int across, int deep, float dx, float dz, float length, float width, V3 from = {}, int points = 3)
{
	Groom g;
	for (int z = 0; z < deep; z++)
		for (int x = 0; x < across; x++) {
			const V3 root = from + V3{dx * (float)x, 0, dz * (float)z};
			g.straight(root, root + V3{0, length, 0}, points);
			g.width.insert(g.width.end(), (size_t)points, width);
		}
	return g;
}

// One coat after another, as one part (their points and widths).
static Groom joined(Groom a, const Groom &b)
{
	const uint32_t at = (uint32_t)a.pos.size();
	a.pos.insert(a.pos.end(), b.pos.begin(), b.pos.end());
	for (size_t s = 1; s < b.first.size(); s++)
		a.first.push_back(at + b.first[s]);
	a.width.insert(a.width.end(), b.width.begin(), b.width.end());
	return a;
}

// How a file is loaded short of points: `points` of them, hair that covers
// its skin up to `cover` times over kept first and the deepest at
// 1 / `uneven` of that.
static LoadOptions rated(size_t points, float cover = LoadOptions().strand_cover,
			 float uneven = LoadOptions().strand_uneven)
{
	LoadOptions o;
	o.strand_points = points;
	o.strand_cover = cover;
	o.strand_uneven = uneven;
	return o;
}

// How thickly the loader finds hair to cover the skin at each strand of a
// coat of straight hairs, read off the rates it gives them: loaded a point
// short of all (so they're rated, and all but one kept) with `cover` the
// most that counts as thin and the deepest hair rated a quarter, a strand
// where the hair is deeper is rated cover / how deep - so how deep is
// cover / its rate (as near as a rate's 64 steps tell: a hundredth or so).
// A strand rated 1 reads as `cover`: that, or thinner. -1: the strand that
// wasn't kept.
static std::vector<double> covers_of(const Groom &g, float cover)
{
	std::vector<double> out(g.strands(), -1.0);
	const Model m = loaded(g, rated(2 * g.strands() - 1, cover, 4));
	const Primitive &h = part_of(m);
	CHECK(h.hair.strands() + 1 == g.strands() && h.hair.rate.size() == h.hair.strands());
	for (size_t s = 0; s < h.hair.strands() && s < h.hair.rate.size(); s++) {
		const int at = g.at(h.pos[h.hair.first[s]]);
		if (at >= 0)
			out[g.strand_of((size_t)at)] = (double)cover / (double)h.hair.rate[s];
	}
	return out;
}

// The least, the most and the mean of them over the strands `which` says
// (those kept).
struct Spread {
	double low = 1e30, high = -1e30, mean = 0;
	size_t count = 0;
};
static Spread spread_over(const std::vector<double> &v, const std::function<bool(size_t)> &which)
{
	Spread s;
	for (size_t i = 0; i < v.size(); i++) {
		if (v[i] < 0 || !which(i))
			continue;
		s.low = std::min(s.low, v[i]);
		s.high = std::max(s.high, v[i]);
		s.mean += v[i];
		s.count++;
	}
	if (s.count)
		s.mean /= (double)s.count;
	return s;
}

static void test_cover()
{
	// 64 x 64 hairs a centimetre apart, each 20 cm long and a millimetre
	// wide: 0.2 x 0.001 of hair on every 0.01 x 0.01 of skin - it covers
	// its skin twice over. So says the loader of every hair well inside
	// the patch (16 hairs or more from its edges): each within a
	// twenty-fifth, all together within a hundredth.
	const float d = 0.01f;
	auto inside = [](int side_x, int side_z, int rim) {
		return [=](size_t i) {
			const int x = (int)i % side_x, z = (int)i / side_x;
			return x >= rim && x < side_x - rim && z >= rim && z < side_z - rim;
		};
	};
	auto holds = [](const Spread &s, double want, double each, double all) {
		return s.count > 0 && s.low > want * (1 - each) && s.high < want * (1 + each) &&
		       near(s.mean, want, want * all);
	};
	const Groom fur = patch(64, 64, d, d, 0.2f, 0.001f);
	{
		const std::vector<double> cover = covers_of(fur, 1.0f);
		const Spread in = spread_over(cover, inside(64, 64, 16));
		CHECK(in.count >= 32 * 32 - 1 && holds(in, 2.0, 0.04, 0.01));
		// At the patch's edge half the hair round a root is missing, at a
		// corner three quarters: about half as deep at the middle of an
		// edge, and thin (under the 1 that counts as thin) at a corner.
		for (size_t corner : {(size_t)0, (size_t)63, (size_t)64 * 63, (size_t)64 * 64 - 1})
			CHECK(cover[corner] == 1.0);
		for (size_t edge : {(size_t)32, (size_t)64 * 32, (size_t)64 * 32 + 63, (size_t)64 * 63 + 32})
			CHECK(cover[edge] >= 1.0 && cover[edge] < 2.0 * 0.62);
		std::printf("unit_hair: fur that covers its skin 2 times over reads %.3f to %.3f inside the patch "
			    "(mean %.4f), %.3f at the middle of an edge\n",
			    in.low, in.high, in.mean, cover[32]);
	}
	// Hairs twice as long, twice as wide, or twice as many (half as far
	// apart one way): four times over, each.
	CHECK(holds(spread_over(covers_of(patch(64, 64, d, d, 0.4f, 0.001f), 2.0f), inside(64, 64, 16)), 4.0, 0.04,
		    0.01));
	CHECK(holds(spread_over(covers_of(patch(64, 64, d, d, 0.2f, 0.002f), 2.0f), inside(64, 64, 16)), 4.0, 0.04,
		    0.01));
	CHECK(holds(spread_over(covers_of(patch(64, 128, d, d / 2, 0.2f, 0.001f), 2.0f), inside(64, 128, 16)), 4.0,
		    0.06, 0.01));
	// Half as long: once over. A third as wide and three times as far
	// apart both ways: 0.2 x 0.001 / 3 over 0.03 x 0.03, a thirteenth
	// and a bit.
	CHECK(holds(spread_over(covers_of(patch(64, 64, d, d, 0.1f, 0.001f), 0.5f), inside(64, 64, 16)), 1.0, 0.04,
		    0.01));
	CHECK(holds(spread_over(covers_of(patch(64, 64, 3 * d, 3 * d, 0.2f, 0.001f / 3), 0.2f / 5.4f),
				inside(64, 64, 16)),
		    0.2 / 2.7, 0.04, 0.01));
	// The same however big the coat is, and however its points are given
	// (a hair's length is all of its file's, whichever points are kept).
	CHECK(holds(spread_over(covers_of(patch(40, 40, d, d, 0.2f, 0.001f), 1.0f), inside(40, 40, 12)), 2.0, 0.04,
		    0.01));
	CHECK(holds(spread_over(covers_of(patch(150, 150, d, d, 0.2f, 0.001f), 1.0f), inside(150, 150, 30)), 2.0, 0.04,
		    0.01));
	CHECK(holds(spread_over(covers_of(patch(64, 64, d, d, 0.2f, 0.001f, {}, 9), 1.0f), inside(64, 64, 16)), 2.0,
		    0.04, 0.01));
	// A hair that narrows evenly from 2 mm at its root to nothing at its
	// tip is as much hair as one a millimetre wide all the way.
	{
		Groom taper = fur;
		for (size_t i = 0; i < taper.width.size(); i++)
			taper.width[i] = 0.002f - 0.001f * (float)(i % 3);
		CHECK(holds(spread_over(covers_of(taper, 1.0f), inside(64, 64, 16)), 2.0, 0.04, 0.01));
	}
	// A file that gives no widths: hairs a tenth of a millimetre wide -
	// 8 cm long and 2 mm apart, twice over.
	{
		Groom fine = patch(64, 64, 0.002f, 0.002f, 0.08f, 0);
		fine.width.clear();
		CHECK(holds(spread_over(covers_of(fine, 1.0f), inside(64, 64, 16)), 2.0, 0.04, 0.01));
	}
	// The skin is measured as it lies, not as seen from above: the same
	// fur on a slope of 45 degrees (its roots as far apart along x, so
	// 1.414 times as far apart on the skin) covers it 2 / 1.414 times.
	{
		Groom slope;
		for (int z = 0; z < 64; z++)
			for (int x = 0; x < 64; x++) {
				const V3 root{d * (float)x, d * (float)x, d * (float)z};
				slope.straight(root, root + V3{-0.2f, 0.2f, 0} * 0.70710678f);
				slope.width.insert(slope.width.end(), 3, 0.001f);
			}
		CHECK(holds(spread_over(covers_of(slope, 0.7f), inside(64, 64, 16)), 2.0 / 1.41421356, 0.1, 0.015));
	}
	// Roots scattered anyhow, as a groom's are (4096 of them over 64 cm
	// square, as many to the square centimetre): the same on the whole -
	// though any one root's reading goes by the few dozen hairs that
	// happen to be round it.
	{
		Groom scattered;
		uint32_t seed = 5;
		for (int i = 0; i < 4096; i++) {
			const float x = 0.64f * chance(seed), z = 0.64f * chance(seed);
			scattered.straight({x, 0, z}, {x, 0.2f, z});
			scattered.width.insert(scattered.width.end(), 3, 0.001f);
		}
		const std::vector<double> cover = covers_of(scattered, 1.0f);
		const Spread in = spread_over(cover, [&scattered](size_t i) {
			const V3 root = scattered.pos[scattered.first[i]];
			return root.x > 0.16f && root.x < 0.48f && root.z > 0.16f && root.z < 0.48f;
		});
		CHECK(in.count > 900 && near(in.mean, 2.0, 0.16) && in.low > 1.0 && in.high < 4.0);
		std::printf("unit_hair: the same fur with its roots scattered reads %.2f to %.2f (mean %.3f)\n", in.low,
			    in.high, in.mean);
	}
}

// --- Which strands are kept and drawn first -----------------------------------

// Thin fur and a deep mane, a metre apart: as many hairs each, a
// centimetre apart. The fur's are 5 cm long and a millimetre wide (half
// its skin covered); the mane's a metre long and 6 mm wide (its skin sixty
// times over - so deep that even its corners, a quarter as deep, are deep).
constexpr double FUR_LENGTH = 0.05, FUR_WIDTH = 0.001, MANE_LENGTH = 1.0, MANE_WIDTH = 0.006;
static Groom fur_patch(int across = 32, int deep = 32, int points = 3)
{
	return patch(across, deep, 0.01f, 0.01f, (float)FUR_LENGTH, (float)FUR_WIDTH, {}, points);
}
static Groom mane_patch(int across = 32, int deep = 32, int points = 3)
{
	return patch(across, deep, 0.01f, 0.01f, (float)MANE_LENGTH, (float)MANE_WIDTH, {1, 0, 0}, points);
}
static bool of_mane(V3 root)
{
	return root.x > 0.75f;
}

// How many of a part's first `count` strands are the fur's and the mane's.
struct Halves {
	size_t fur = 0, mane = 0;
};
static Halves halves_of(const Primitive &h, size_t count = (size_t)-1)
{
	Halves n;
	for (size_t s = 0; s < h.hair.strands() && s < count; s++)
		(of_mane(h.pos[h.hair.first[s]]) ? n.mane : n.fur)++;
	return n;
}

// What must hold of any part's keys and rates: one of each a strand; a
// rate one of its 64 steps from a quarter to 1; a key its strand's place
// among the file's strands (a whole number of them, no two strands the
// same) over the file's count and its rate; the keys never falling, under
// 4, and none past `full`. (Coats of these sizes: a float tells a place
// of a few thousand to a hundredth.)
static bool keys_right(const Primitive &p)
{
	const Strands &h = p.hair;
	const size_t n = h.strands();
	if (h.key.size() != n || h.rate.size() != n || !(h.full >= 0) || !(h.full <= 4))
		return false;
	std::set<long> places;
	for (size_t k = 0; k < n; k++) {
		const float rate = h.rate[k];
		if (!(rate >= 0.25f && rate <= 1) || rate != Strands::rate_of(Strands::rate_step(rate)))
			return false;
		if ((k && h.key[k] < h.key[k - 1]) || !(h.key[k] >= 0) || !(h.key[k] < 4) || h.key[k] > h.full)
			return false;
		const double place = (double)h.key[k] * (double)rate * (double)h.in_file;
		if (std::fabs(place - std::round(place)) > 0.01 || place > (double)h.in_file - 0.5 ||
		    !places.insert(std::lround(place)).second)
			return false;
	}
	return true;
}

// Two parts with the same strands, kept and rated the same.
static bool same_strands(const Primitive &a, const Primitive &b)
{
	return same_points(a.pos, b.pos) && a.hair.in_file == b.hair.in_file && a.hair.first == b.hair.first &&
	       a.hair.width == b.hair.width && a.hair.key == b.hair.key && a.hair.rate == b.hair.rate &&
	       a.hair.full == b.hair.full && a.hair.mean_width == b.hair.mean_width;
}

// The mean width, along their lengths, of `fur` of the fur's hairs and
// `mane` of the mane's as drawn at the threshold `full`: each widened by 1
// over the share of the hairs round it that are drawn there -
// min(1, full x its rate) - the fur's rate 1, the mane's `mane_rate`.
static double width_at_full(const Halves &n, double full, double mane_rate)
{
	const double fur = (double)n.fur * FUR_LENGTH, mane = (double)n.mane * MANE_LENGTH;
	return (fur * FUR_WIDTH / std::min(1.0, full) + mane * MANE_WIDTH / std::min(1.0, full * mane_rate)) /
	       (fur + mane);
}

static void test_rates_and_keys()
{
	// 1024 hairs of fur and 1024 of mane, one part: 2048 strands, 2 points
	// kept of each.
	const Groom coat = joined(fur_patch(), mane_patch());
	Builder b = file_of({coat});
	// All of it allowed: nothing to choose. Every strand's rate is 1 and
	// its key its place among the file's strands (spread evenly: any first
	// so many are as much fur as mane), all of them drawn at 1.
	{
		const Model m = loaded(b);
		const Primitive &h = part_of(m);
		CHECK(h.hair.strands() == 2048 && h.pos.size() == 4096 && h.hair.full == 1 && keys_right(h));
		bool plain = h.hair.key.size() == 2048 && h.hair.rate.size() == 2048;
		for (size_t k = 0; plain && k < 2048; k++)
			plain = h.hair.rate[k] == 1 && h.hair.key[k] == (float)((double)k / 2048);
		CHECK(plain);
		for (size_t count : {2u, 64u, 500u, 1001u, 2048u}) {
			const Halves n = halves_of(h, count);
			CHECK(n.fur + 2 >= n.mane && n.mane + 2 >= n.fur);
		}
		// (Its hairs' mean width: 1 mm for 5 cm of each two hairs' length,
		// 6 mm for the other metre.)
		CHECK(near(h.hair.mean_width, (0.05 * 0.001 + 1.0 * 0.006) / 1.05, 1e-8));
	}

	// Short of points, room for `kept` strands: thin hair first. The fur's
	// hairs are kept at three times the share of the mane's (strand_uneven
	// as loaded), each rated 1 and the mane's a third - until the fur is
	// all kept, and the rest of the room is the mane's.
	for (size_t kept : {256u, 512u, 1024u, 1300u, 1536u, 1800u, 2047u}) {
		const Model m = loaded(b, allowed(2 * kept));
		const Primitive &h = part_of(m);
		const Halves n = halves_of(h);
		CHECK(h.hair.strands() == kept && h.hair.in_file == 2048 && m.strands_in_file == 2048 && keys_right(h));
		if (h.hair.strands() != kept || !keys_right(h))
			continue;
		if (kept <= 1300) {
			CHECK(near((double)n.fur, 0.75 * (double)kept, 2) &&
			      near((double)n.mane, 0.25 * (double)kept, 2));
			// (The threshold: the share of the fur that's kept.)
			CHECK(h.hair.full < 1 && near(h.hair.full, (double)n.fur / 1024, 0.003));
		} else {
			CHECK(n.fur == 1024 && n.mane == kept - 1024);
			CHECK(h.hair.full > 1 && near(h.hair.full, 3.0 * (double)n.mane / 1024, 0.01));
		}
		bool rates = true;
		for (size_t k = 0; k < kept; k++)
			rates = rates && (of_mane(h.pos[h.hair.first[k]]) ? near(h.hair.rate[k], 1.0 / 3, 0.006)
									  : h.hair.rate[k] == 1);
		CHECK(rates);
		// Its threshold: the key of the first strand it couldn't keep -
		// the next one's, of the same file loaded with room for one more.
		// (With room for all of them nothing is rated: the last one's key
		// is worked out from its place, the one place no kept strand has,
		// and the mane's rate.)
		if (kept + 1 < 2048) {
			const Model more = loaded(b, allowed(2 * kept + 2));
			CHECK(part_of(more).hair.strands() == kept + 1 && first_of(h, part_of(more)));
			if (part_of(more).hair.strands() == kept + 1)
				CHECK(h.hair.full == part_of(more).hair.key[kept] && h.hair.key.back() <= h.hair.full);
		} else {
			std::vector<bool> has(2048, false);
			for (size_t k = 0; k < kept; k++)
				has[(size_t)std::lround((double)h.hair.key[k] * (double)h.hair.rate[k] * 2048)] = true;
			const size_t left = (size_t)(std::find(has.begin(), has.end(), false) - has.begin());
			CHECK(left < 2048 && std::count(has.begin(), has.end(), false) == 1);
			CHECK(near(h.hair.full, (double)left / 2048 / (double)h.hair.rate.back(), 1e-5) &&
			      h.hair.key.back() <= h.hair.full && of_mane(h.pos[h.hair.first[kept - 1]]));
		}
		// Its hairs' mean width as drawn at that threshold, where each
		// stands for the hairs round it that aren't.
		const double want = width_at_full(n, h.hair.full, 1.0 / 3);
		CHECK(near(h.hair.mean_width, want, want * 1e-5));
		// (The widths themselves are the file's.)
		bool own = h.hair.width.size() == h.pos.size();
		for (size_t i = 0; own && i < h.pos.size(); i++)
			own = h.hair.width[i] == (of_mane(h.pos[i]) ? (float)MANE_WIDTH : (float)FUR_WIDTH);
		CHECK(own);
	}
	// Deep hair at half the thin hair's share, or two thirds, or a quarter
	// (strand_uneven 2, 1.5, 4 - and never under a quarter): room for 600
	// strands goes round as that says.
	const float uneven[] = {2, 1.5f, 4, 10};
	const double share[] = {2, 1.5, 4, 4};
	for (int i = 0; i < 4; i++) {
		const Model m = loaded(b, rated(1200, 1.5f, uneven[i]));
		const Primitive &h = part_of(m);
		const Halves n = halves_of(h);
		CHECK(h.hair.strands() == 600 && keys_right(h));
		CHECK(near((double)n.fur, 600 * share[i] / (share[i] + 1), 2) &&
		      near((double)n.mane, 600 / (share[i] + 1), 2));
		const double want = width_at_full(n, h.hair.full, 1 / share[i]);
		CHECK(near(h.hair.mean_width, want, want * 0.002));
	}
	// What counts as thin is strand_cover: with fur that covers half its
	// skin counted deep too (thin: up to a tenth), it's five times deep -
	// rated a third as the mane is, and the two kept alike.
	{
		const Model m = loaded(b, rated(1200, 0.1f, 3));
		const Halves n = halves_of(part_of(m));
		// (Its corners and edges, a quarter and a half as deep, are still
		// rated above its middle: a little more fur than mane.)
		CHECK(part_of(m).hair.strands() == 600 && n.fur >= 300 && n.fur < 345 && keys_right(part_of(m)));
	}
	// Every strand worth the same (strand_uneven 1 or less), or no cover
	// counted as thin (strand_cover 0 or less): no rates. The first strands
	// of the evenly spread order, as much fur as mane; their keys their
	// places, the threshold the share kept, and each hair read as wide as
	// the whole coat's would be over that share.
	for (int how = 0; how < 4; how++) {
		const LoadOptions o = how == 0   ? rated(1200, 1.5f, 1)
				      : how == 1 ? rated(1200, 1.5f, 0.5f)
				      : how == 2 ? rated(1200, 0, 3)
						 : rated(1200, -1, 3);
		const Model m = loaded(b, o);
		const Primitive &h = part_of(m);
		const Halves n = halves_of(h);
		CHECK(h.hair.strands() == 600 && n.fur + 2 >= n.mane && n.mane + 2 >= n.fur && keys_right(h));
		const Strands same = alike(2048, 600, 1);
		CHECK(h.hair.key == same.key && h.hair.rate == same.rate && h.hair.full == same.full);
		const double want = width_at_full(n, 600.0 / 2048, 1);
		CHECK(near(h.hair.mean_width, want, want * 1e-5));
		CHECK(first_of(h, part_of(loaded(b))));
	}

	// A coat the same all over is kept the same whether its strands are
	// rated or not: thin all over (the fur alone), or deep all over (the
	// mane alone - the deepest hair there is has the thinnest's rate when
	// there is none thinner).
	for (int which = 0; which < 2; which++) {
		const Groom one = which ? mane_patch() : fur_patch();
		const Model with = loaded(one, allowed(600)), without = loaded(one, rated(600, 1.5f, 1));
		const Primitive &h = part_of(with);
		CHECK(h.hair.strands() == 300 && same_strands(h, part_of(without)) && keys_right(h));
		bool ones = true;
		for (float rate : h.hair.rate)
			ones = ones && rate == 1;
		CHECK(ones && h.hair.full == (float)(300.0 / 1024));
		const double width = which ? MANE_WIDTH : FUR_WIDTH;
		CHECK(near(h.hair.mean_width, width * 1024 / 300, width * 1e-4));
	}

	// Every strand kept though the file was short of points by the
	// loader's reckoning - which goes by one strand of each run of a few
	// (here 4001 strands, two to a run, the last a run of its own), so a
	// last strand of 3000 points among others of 3 makes the coat look
	// 3000 points longer than it is. Then they're rated, and all of them
	// are drawn at 1 / the lowest rate: 3.
	{
		Groom most = joined(fur_patch(50, 40), mane_patch(50, 40));
		// (One more hair of fur, off the patch's corner.)
		most.straight({0.51f, 0, 0}, {0.51f, (float)FUR_LENGTH, 0});
		most.width.insert(most.width.end(), 3, (float)FUR_WIDTH);
		const Model whole = loaded(most, options_with(0));
		const Primitive &all = part_of(whole);
		CHECK(all.hair.strands() == 4001 && all.pos.size() == 12003);
		if (all.hair.strands() != 4001)
			return;
		// (Which strand comes last in the order.)
		const int last = most.at(all.pos[all.hair.first[4000]]);
		CHECK(last >= 0);
		if (last < 0)
			return;
		const size_t long_one = most.strand_of((size_t)last);
		Groom coat_of_one_long;
		for (size_t s = 0; s < most.strands(); s++) {
			const V3 root = most.pos[most.first[s]], tip = most.pos[most.first[s + 1] - 1];
			const int points = s == long_one ? 3000 : 3;
			coat_of_one_long.straight(root, tip, points);
			coat_of_one_long.width.insert(coat_of_one_long.width.end(), (size_t)points,
						      most.width[most.first[s]]);
		}
		LoadOptions o = options_with(0);
		o.strand_points = 15000;
		const Model m = loaded(coat_of_one_long, o);
		const Primitive &h = part_of(m);
		CHECK(h.hair.strands() == 4001 && h.pos.size() == 15000 && keys_right(h));
		float lowest = 1;
		for (float rate : h.hair.rate)
			lowest = std::min(lowest, rate);
		CHECK(near(lowest, 1.0 / 3, 0.006) && h.hair.full == 1 / lowest);
		// (None stands for hairs that aren't there: its hairs' mean width
		// is the file's.)
		const double fur = 2001 * FUR_LENGTH, mane = 2000 * MANE_LENGTH;
		const double want = (fur * FUR_WIDTH + mane * MANE_WIDTH) / (fur + mane);
		CHECK(near(h.hair.mean_width, want, want * 1e-4));
		// Not rated (every strand worth the same): all drawn at 1.
		o.strand_uneven = 1;
		const Model plain = loaded(coat_of_one_long, o);
		CHECK(part_of(plain).hair.strands() == 4001 && part_of(plain).hair.full == 1);
	}
}

// --- Thresholds ---------------------------------------------------------------

static void test_thresholds()
{
	// A rate is one of 64 steps from a quarter to 1, evenly apart.
	CHECK(Strands::RATES == 64 && Strands::rate_of(0) == 0.25f && Strands::rate_of(63) == 1.0f);
	CHECK(Strands::rate_of(21) == 0.5f && Strands::rate_of(42) == 0.75f &&
	      near(Strands::rate_of(7), 1.0 / 3, 1e-7));
	bool steps = true;
	for (int s = 0; s < 64; s++) {
		steps = steps && Strands::rate_step(Strands::rate_of(s)) == s;
		if (s)
			steps = steps && near(Strands::rate_of(s) - Strands::rate_of(s - 1), 0.75 / 63, 1e-6);
	}
	CHECK(steps);
	// Any rate: the step nearest it (half a step off at most).
	bool nearest = true;
	for (int i = 0; i <= 3000; i++) {
		const float rate = 0.25f + 0.75f * (float)i / 3000;
		const int s = Strands::rate_step(rate);
		nearest = nearest && s >= 0 && s <= 63 && std::fabs(Strands::rate_of(s) - rate) <= 0.75f / 126 + 1e-6f;
	}
	CHECK(nearest);
	CHECK(Strands::rate_step(0.25f + 0.4f * 0.75f / 63) == 0 && Strands::rate_step(0.25f + 0.6f * 0.75f / 63) == 1);
	CHECK(Strands::rate_step(1 - 0.4f * 0.75f / 63) == 63 && Strands::rate_step(1 - 0.6f * 0.75f / 63) == 62);
	// Under a quarter: the first; over 1: the last; not a number: one of
	// them.
	const float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
	CHECK(Strands::rate_step(0.2f) == 0 && Strands::rate_step(0) == 0 && Strands::rate_step(-3) == 0 &&
	      Strands::rate_step(-inf) == 0);
	CHECK(Strands::rate_step(1.01f) == 63 && Strands::rate_step(7) == 63 && Strands::rate_step(inf) == 63);
	CHECK(Strands::rate_step(nan) >= 0 && Strands::rate_step(nan) <= 63);

	// Six strands kept of a file's eight, their keys as given; all of them
	// drawn at 1.2.
	Strands h;
	h.in_file = 8;
	h.first = {0, 2, 4, 6, 8, 10, 12};
	h.key = {0, 0.1f, 0.2f, 0.2f, 0.5f, 0.9f};
	h.rate.assign(6, 1.0f);
	h.full = 1.2f;
	// How many (the first of them) have keys under a threshold.
	const float under[] = {-1, 0, 0.05f, 0.1f, 0.11f, 0.2f, 0.21f, 0.5f, 0.9f, 0.91f, 1.2f, 9, inf, -inf};
	const size_t count[] = {0, 0, 1, 1, 2, 2, 4, 4, 5, 6, 6, 6, 6, 0};
	for (int i = 0; i < 14; i++)
		CHECK(h.strands_under(under[i]) == count[i]);
	CHECK(h.strands_under(nan) <= 6);
	// The threshold its first so many are drawn at: the key of the next
	// (all of them, or more: `full`).
	const float at[] = {0, 0.1f, 0.2f, 0.2f, 0.5f, 0.9f, 1.2f, 1.2f};
	for (size_t c = 0; c < 8; c++)
		CHECK(h.threshold_of(c) == at[c]);
	CHECK(h.threshold_of(1000000) == 1.2f);
	// (Back again: the strands under that threshold are those - but for
	// one whose key is the same as the next's.)
	for (size_t c = 0; c <= 6; c++)
		CHECK(h.strands_under(h.threshold_of(c)) == (c == 3 ? 2u : c));
	// Never over `full`, whatever a key says.
	h.full = 0.45f;
	CHECK(h.threshold_of(3) == 0.2f && h.threshold_of(4) == 0.45f && h.threshold_of(5) == 0.45f &&
	      h.threshold_of(6) == 0.45f);
	// Lists that don't add up (fewer keys than strands): the strands that
	// have keys, and no harm.
	h.key.resize(3);
	CHECK(h.strands_under(9) == 3 && h.strands_under(0.15f) == 2 && h.threshold_of(2) == 0.2f &&
	      h.threshold_of(3) == 0.45f && h.threshold_of(5) == 0.45f);
	h.key = {0, 0.1f, 0.2f, 0.2f, 0.5f, 0.9f, 1, 1.1f, 1.15f};
	CHECK(h.strands_under(9) == 6 && h.threshold_of(6) == 0.45f);
	// No strands: none, drawn at 1.
	CHECK(Strands().strands_under(1) == 0 && Strands().threshold_of(0) == 1 && Strands().threshold_of(5) == 1);
}

// --- How many strands of a rated coat ------------------------------------------

// A part's strands as the loader leaves a coat of thin fur and a deep mane
// of as many hairs (`each`), every one kept: the fur's rated 1 and the
// mane's `rate`, their places among the file's strands taking turns (the
// fur's the even ones), in the order of their keys - a place over the
// file's count and the strand's rate. `mane` says which are the mane's.
// Every hair `width` wide and as long as every other.
static Strands fur_and_mane(size_t each, float rate, float width, std::vector<char> &mane)
{
	struct One {
		double key;
		float rate;
		char mane;
	};
	std::vector<One> all;
	for (size_t i = 0; i < each; i++) {
		all.push_back({(double)(2 * i) / (double)(2 * each), 1.0f, 0});
		all.push_back({(double)(2 * i + 1) / (double)(2 * each) / (double)rate, rate, 1});
	}
	std::stable_sort(all.begin(), all.end(), [](const One &a, const One &b) { return a.key < b.key; });
	Strands h;
	h.in_file = (uint32_t)(2 * each);
	h.first.push_back(0);
	mane.clear();
	for (const One &one : all) {
		h.first.push_back(h.first.back() + 2);
		h.key.push_back((float)one.key);
		h.rate.push_back(one.rate);
		mane.push_back(one.mane);
	}
	// (All kept: drawn at 1 / the lowest rate, where no hair is widened.)
	h.full = 1 / rate;
	h.mean_width = width;
	return h;
}

static void test_share_of_a_rated_coat()
{
	// 1500 hairs of fur and 1500 of mane (rated a third), a millimetre
	// wide; a pixel is a millimetre wide a unit from the camera.
	const float pixel = 0.001f, width = 0.001f, third = Strands::rate_of(7);
	std::vector<char> mane;
	const Strands coat = fur_and_mane(1500, third, width, mane);
	auto drawn = [&mane](const HairShare &s) {
		Halves n;
		for (size_t k = 0; k < s.count && k < mane.size(); k++)
			(mane[k] ? n.mane : n.fur)++;
		return n;
	};
	// From near, all of it: drawn at the threshold that takes the mane's
	// last hair, 3.
	HairShare s = hair_share(coat, 3000, 1, 1, pixel, 1, 0.4f, 1);
	CHECK(s.count == 3000 && near(s.upto, 3, 1e-6) && s.upto == coat.full);
	// The hair budget's say, a share of that threshold: down to a third it
	// takes from the mane alone (the mane's hairs that share, the fur's all
	// drawn); under a third from both alike - the fur's hairs three times
	// the share, the mane's the share.
	const float budget[] = {0.9f, 0.75f, 0.5f, 0.4f, 0.3f, 0.2f, 0.1f, 0.05f};
	for (float share : budget) {
		s = hair_share(coat, 3000, 1, 1, pixel, 1, 0.4f, share);
		const Halves n = drawn(s);
		CHECK(n.fur + n.mane == s.count && s.count < 3000);
		CHECK(near((double)n.fur, 1500 * std::min(1.0, 3.0 * share), 1.5) &&
		      near((double)n.mane, 1500.0 * share, 1.5));
		// (Drawn at the threshold that takes just those.)
		CHECK(near(s.upto, 3.0 * share, 0.002) && s.upto == coat.threshold_of(s.count) &&
		      coat.strands_under(s.upto) == s.count);
	}
	// Its size on screen does the same: from 5 away its hairs are 0.2 of a
	// pixel wide, 0.526 of the 0.38 they're drawn at - the mane's hairs
	// that share, the fur's all; from 20 away 0.132 - the mane's that
	// share, the fur's three times it.
	s = hair_share(coat, 3000, 1, 5, pixel, 1, 0.4f, 1);
	CHECK(drawn(s).fur == 1500 && near((double)drawn(s).mane, 1500 / (5 * 0.38), 1.5));
	s = hair_share(coat, 3000, 1, 20, pixel, 1, 0.4f, 1);
	CHECK(near((double)drawn(s).fur, 4500 / (20 * 0.38), 1.5) &&
	      near((double)drawn(s).mane, 1500 / (20 * 0.38), 1.5));
	// (And both: a share of a share.)
	s = hair_share(coat, 3000, 1, 5, pixel, 1, 0.4f, 0.5f);
	CHECK(near((double)drawn(s).fur, 4500 / (5 * 0.38) / 2, 1.5) &&
	      near((double)drawn(s).mane, 1500 / (5 * 0.38) / 2, 1.5));
	// Never fewer than 16: three of the fur's to each of the mane's.
	s = hair_share(coat, 3000, 1, 1e6f, pixel, 1, 0.4f, 1);
	CHECK(s.count == 16 && drawn(s).fur == 12 && drawn(s).mane == 4 && s.upto == coat.key[16]);
	s = hair_share(coat, 3000, 1, 1, pixel, 1, 0.4f, 0);
	CHECK(s.count == 16);
	// Fewer ready to draw than it has (only its first 1000 posed): never
	// more than those, drawn at the threshold that takes just them.
	s = hair_share(coat, 1000, 1, 1, pixel, 1, 0.4f, 1);
	CHECK(s.count == 1000 && s.upto == coat.key[1000] && near(s.upto, 0.5, 0.002));
	CHECK(drawn(s).fur == 750 && drawn(s).mane == 250);
	s = hair_share(coat, 1000, 1, 1, pixel, 1, 0.4f, 0.5f);
	CHECK(s.count == 1000);
	s = hair_share(coat, 1000, 1, 1, pixel, 1, 0.4f, 0.1f);
	CHECK(s.count == 600 && drawn(s).fur == 450);
	s = hair_share(coat, 5, 1, 1e6f, pixel, 1, 0.4f, 1);
	CHECK(s.count == 5 && s.upto == coat.key[5]);

	// The same of a coat as the loader makes it: the fur and the mane of
	// test_rates_and_keys with room for 1536 strands - all 1024 of the
	// fur and half the mane, so drawn at 1.5 with every strand.
	const Groom both = joined(fur_patch(), mane_patch());
	const Model m = loaded(both, allowed(2 * 1536));
	const Primitive &p = part_of(m);
	const Strands &h = p.hair;
	CHECK(h.strands() == 1536 && near(h.full, 1.5, 0.01));
	if (h.strands() != 1536)
		return;
	// Seen from so near that its hairs are over 0.4 of a pixel wide.
	s = hair_share(h, 1536, 1, 0.01f, pixel, 1, 0.4f, 1);
	CHECK(s.count == 1536 && s.upto == h.full);
	for (float share : {0.9f, 0.7f, 0.5f, 0.25f, 0.1f}) {
		s = hair_share(h, 1536, 1, 0.01f, pixel, 1, 0.4f, share);
		const Halves n = halves_of(p, s.count);
		const double upto = (double)share * (double)h.full;
		CHECK(near((double)n.fur, 1024 * std::min(1.0, upto), 2) && near((double)n.mane, 1024 * upto / 3, 2));
		CHECK(near(s.upto, upto, 0.003));
	}
	// Its hairs' width for its size on screen is as drawn with all of
	// them, where each of the mane's stands for two (0.012 wide for a
	// metre, to the fur's 0.001 for 5 cm): from 12 away that's a pixel of
	// 0.012 units, the hair 0.914 of one wide - all drawn; from 60 away a
	// fifth of that, 0.481 of the 0.38 they're drawn at.
	CHECK(near(h.mean_width, (512 * 2 * MANE_WIDTH + 1024 * FUR_LENGTH * FUR_WIDTH) / (512 + 1024 * FUR_LENGTH),
		   2e-5));
	s = hair_share(h, 1536, 1, 12, pixel, 1, 0.4f, 1);
	CHECK(s.count == 1536);
	s = hair_share(h, 1536, 1, 60, pixel, 1, 0.4f, 1);
	CHECK(near(s.upto, (double)h.mean_width / 0.06 / 0.38 * (double)h.full, 0.003) && s.count < 1536 &&
	      halves_of(p, s.count).fur > 700);
}

// --- One threshold for all a character's parts ----------------------------------

static void test_parts_share_alike()
{
	// The fur and the mane as two parts. Each is the same all over (so its
	// own strands' rates are all 1), but the mane's hairs count a third of
	// the fur's: with room for 512 strands the fur keeps three times the
	// share of its hairs that the mane does - 384 and 128.
	const Groom fur = fur_patch(), mane = mane_patch();
	Builder b = file_of({fur, mane});
	{
		const Model m = loaded(b, allowed(1024));
		const Primitive &f = part_of(m, 0), &d = part_of(m, 1);
		CHECK(near((double)f.hair.strands(), 384, 1) && near((double)d.hair.strands(), 128, 1));
		CHECK(m.strand_points <= 1024 && m.strand_points >= 1020 && m.strands_in_file == 2048);
		for (const Primitive *p : {&f, &d}) {
			CHECK(keys_right(*p) && p->hair.in_file == 1024);
			const Strands same = alike(1024, p->hair.strands(), 1);
			CHECK(p->hair.key == same.key && p->hair.rate == same.rate && p->hair.full == same.full);
		}
		// (Each part's hairs read as wide as its own share kept makes
		// them: the mane's, an eighth kept, eight times.)
		CHECK(near(d.hair.mean_width, MANE_WIDTH * 1024 / (double)d.hair.strands(), 1e-6));
		CHECK(near(f.hair.mean_width, FUR_WIDTH * 1024 / (double)f.hair.strands(), 1e-7));
		// (The first of each part's own order.)
		const Model whole = loaded(b);
		CHECK(first_of(f, part_of(whole, 0)) && first_of(d, part_of(whole, 1)));
	}
	// By other numbers: the mane at half, two thirds or a quarter of the
	// fur's share.
	const float uneven[] = {2, 1.5f, 4, 10};
	const double share[] = {2, 1.5, 4, 4};
	for (int i = 0; i < 4; i++) {
		const Model m = loaded(b, rated(1024, 1.5f, uneven[i]));
		const double f = (double)part_of(m, 0).hair.strands(), d = (double)part_of(m, 1).hair.strands();
		CHECK(near(f, 512 * share[i] / (share[i] + 1), 1.5) && near(d, 512 / (share[i] + 1), 1.5));
		CHECK(d > 0 && near(f / d, share[i], share[i] * 0.02));
	}
	// Room for more than all the fur: all of it, and the rest the mane's.
	{
		const Model m = loaded(b, allowed(3000));
		CHECK(part_of(m, 0).hair.strands() >= 1023 && near((double)part_of(m, 1).hair.strands(), 476, 2));
		CHECK(m.strand_points <= 3000 && m.strand_points >= 2994);
	}
	// Every strand worth the same, or no cover counted as thin: each part
	// the same share of its own, as it was before strands had rates.
	for (const LoadOptions &o : {rated(1024, 1.5f, 1), rated(1024, 0, 3)}) {
		const Model m = loaded(b, o);
		CHECK(part_of(m, 0).hair.strands() == 256 && part_of(m, 1).hair.strands() == 256);
	}
	// Parts that are each thin all over share alike however they're
	// loaded: a big lawn and a small one (3000 hairs and 1000, 2 points
	// each), room for a quarter of them.
	{
		const Groom big = lawn(60, 50), small = lawn(40, 25, 3, false, {100, 0, 0});
		Builder lawns = file_of({big, small});
		for (const LoadOptions &o : {allowed(2000), rated(2000, 1.5f, 1)}) {
			const Model m = loaded(lawns, o);
			CHECK(part_of(m, 0).hair.strands() == 750 && part_of(m, 1).hair.strands() == 250);
		}
	}
	// A part with a patch of each (the coat of test_rates_and_keys)
	// beside a part of fur alone: one threshold for both - the lone fur
	// kept at the share the other part's fur is.
	{
		const Groom coat = joined(fur_patch(), mane_patch());
		Groom lone = patch(32, 32, 0.01f, 0.01f, (float)FUR_LENGTH, (float)FUR_WIDTH, {0, 0, 2});
		Builder two = file_of({coat, lone});
		const Model m = loaded(two, allowed(2048));
		const Halves n = halves_of(part_of(m, 0));
		const double share = (double)n.fur / 1024;
		// (Room for 1024 strands: the fur's 2048 hairs at 3 / 7, the
		// mane's 1024 at 1 / 7.)
		CHECK(near(share, 3.0 / 7, 0.01) && near((double)n.mane / 1024, 1.0 / 7, 0.01));
		CHECK(near((double)part_of(m, 1).hair.strands() / 1024, share, 0.01));
		CHECK(m.strand_points <= 2048 && m.strand_points >= 2040);
	}
}

// --- A part's share when its strands differ ------------------------------------

// A lawn of `side` x `side` hairs a step apart from `from`: the hair at
// x, z of as many points as `points_at` says - 2 a straight hair, more a
// hair kinked at each point (all kept).
static Groom lawn_of(int side, V3 from, const std::function<int(int, int)> &points_at)
{
	Groom g;
	for (int z = 0; z < side; z++)
		for (int x = 0; x < side; x++) {
			std::vector<V3> s;
			for (int i = 0; i < points_at(x, z); i++)
				s.push_back(from + V3{(float)x + (i % 2 ? 0.4f : 0.0f), 0.1f * (float)i, (float)z});
			g.strand(s);
		}
	return g;
}

// The points each part's first strands come to under the one threshold
// that takes as many strands as `allowed` points have room for - every
// part the same share of its strands, counted strand by strand from the
// parts loaded whole (coats whose strands are all rated alike).
static std::vector<size_t> shares_by_count(const std::vector<const Primitive *> &whole, size_t allowed)
{
	std::vector<double> tried;
	for (const Primitive *p : whole)
		for (size_t k = 0; k <= p->hair.strands(); k++)
			tried.push_back((double)k / (double)p->hair.strands());
	std::sort(tried.begin(), tried.end());
	std::vector<size_t> best(whole.size(), 0);
	for (double share : tried) {
		std::vector<size_t> points;
		size_t sum = 0;
		for (const Primitive *p : whole) {
			const size_t strands = (size_t)std::floor(share * (double)p->hair.strands() + 1e-9);
			points.push_back(p->hair.first[std::min(strands, p->hair.strands())]);
			sum += points.back();
		}
		if (sum > allowed)
			break;
		best = points;
	}
	return best;
}

static void test_share_by_sample()
{
	// Two lawns of 4096 hairs each whose hairs' points go by where they
	// are: one's far half of 9 points and its near half of 2, the other's
	// left half of 9 and its right of 3. Short of points, the loader can't
	// shape every strand to share them out - it goes by one strand of each
	// run of two - yet each part keeps what's its due (the same share of
	// its strands as the other) to within a twentieth.
	const Groom far_long = lawn_of(64, {}, [](int, int z) { return z < 32 ? 2 : 9; });
	const Groom left_long = lawn_of(64, {100, 0, 0}, [](int x, int) { return x < 32 ? 9 : 3; });
	Builder b = file_of({far_long, left_long});
	const Model whole = loaded(b);
	const Primitive &a = part_of(whole, 0), &c = part_of(whole, 1);
	CHECK(a.hair.strands() == 4096 && a.pos.size() == 2048 * 2 + 2048 * 9);
	CHECK(c.hair.strands() == 4096 && c.pos.size() == 2048 * 9 + 2048 * 3);
	if (a.hair.strands() != 4096 || c.hair.strands() != 4096)
		return;
	double worst = 0;
	for (size_t points : {3000u, 6000u, 15000u, 30000u, 45000u})
		for (int how = 0; how < 2; how++) {
			const Model m = loaded(b, how ? rated(points, 1.5f, 1) : allowed(points));
			const std::vector<size_t> due = shares_by_count({&a, &c}, points);
			CHECK(due[0] + due[1] <= points && due[0] + due[1] + 18 > points);
			for (size_t k = 0; k < 2; k++) {
				const double kept = (double)part_of(m, k).pos.size();
				worst = std::max(worst, std::fabs(kept - (double)due[k]) / (double)due[k]);
				CHECK(near(kept, (double)due[k], 0.05 * (double)due[k]));
				CHECK(first_of(part_of(m, k), part_of(whole, k)) && keys_right(part_of(m, k)));
			}
			// (Never more than there's room for, and little of it left.)
			CHECK(m.strand_points <= points && (double)m.strand_points >= 0.95 * (double)points);
		}
	std::printf("unit_hair: two parts whose strands' points go by where they are: each kept within %.1f%% of "
		    "its due\n",
		    100 * worst);
}

// --- Groups ---------------------------------------------------------------------

// Which group each of a part's strands is drawn in (-1: its pieces
// aren't all in one group's runs, or it has none).
static std::vector<int> groups_of_strands(const Primitive &p)
{
	const Strands &h = p.hair;
	std::vector<int> out(h.strands(), -1);
	std::vector<char> mixed(h.strands(), 0);
	const size_t layers = (size_t)std::max(1, h.layers);
	for (size_t r = 0; r + 1 < h.runs.size(); r++)
		for (uint32_t k = h.runs[r]; k < h.runs[r + 1] && k < h.pieces.size(); k++) {
			const size_t s = (size_t)(std::upper_bound(h.first.begin(), h.first.end(), h.pieces[k]) -
						  h.first.begin()) -
					 1;
			if (s >= out.size())
				continue;
			if (out[s] >= 0 && out[s] != (int)(r / layers))
				mixed[s] = 1;
			out[s] = (int)(r / layers);
		}
	for (size_t s = 0; s < out.size(); s++)
		if (mixed[s])
			out[s] = -1;
	return out;
}

static void test_groups()
{
	// 12 hairs in a row, each held most by one of 4 bones (hair s by
	// bone s % 4 - its second number, the stronger) and the rest by the
	// next: 4 groups, by the bone that holds its root most.
	{
		Groom g;
		for (int s = 0; s < 12; s++) {
			g.straight({(float)s, 0, 0}, {(float)s, 0.1f, 0});
			for (int i = 0; i < 3; i++)
				g.held((uint16_t)((s + 1) % 4), 0.4f, (uint16_t)(s % 4), 0.6f);
		}
		Builder b = file_of({g}, {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {3, 0, 0}});
		const Model m = loaded(b);
		const Primitive &h = part_of(m);
		CHECK(h.hair.strands() == 12 && h.hair.groups.size() == 4 && h.hair.layers == 5);
		CHECK(h.hair.runs.size() == 4 * 5 + 1);
		const std::vector<int> of = groups_of_strands(h);
		std::set<int> joints;
		for (const Strands::Group &group : h.hair.groups)
			joints.insert(group.joint);
		CHECK((joints == std::set<int>{0, 1, 2, 3}));
		for (size_t s = 0; s < h.hair.strands() && of.size() == 12; s++) {
			const int root = (int)h.pos[h.hair.first[s]].x;
			CHECK(of[s] >= 0 && of[s] < 4 && h.hair.groups[(size_t)of[s] % 4].joint == root % 4);
		}
		// Each group's middle: its three hairs' (a hair of 2 points:
		// its root) - bone 0's hairs are at 0, 4 and 8. And a point
		// near it: the hair at 4's.
		for (const Strands::Group &group : h.hair.groups) {
			CHECK(near3(group.middle, {4.0f + (float)group.joint, 0, 0}, 1e-5f));
			CHECK(group.sample < h.pos.size() &&
			      near3(h.pos[group.sample % h.pos.size()], group.middle, 1e-5f));
		}
	}
	// No bones: one group, bone 0, its middle the mean of its hairs'
	// middle points.
	{
		Groom g;
		for (int s = 0; s < 5; s++)
			g.strand({{(float)s, 0, 0}, {(float)s + 0.05f, 1, 0}, {(float)s, 2, (float)s}});
		const Model m = loaded(g);
		const Primitive &h = part_of(m);
		CHECK(h.hair.groups.size() == 1 && h.pos.size() == 15);
		if (h.hair.groups.size() == 1) {
			CHECK(h.hair.groups[0].joint == 0 && near3(h.hair.groups[0].middle, {2.05f, 1, 0}, 1e-5f));
			CHECK(near3(h.pos[h.hair.groups[0].sample % 15], {2.05f, 1, 0}, 1e-5f));
		}
		CHECK((groups_of_strands(h) == std::vector<int>(5, 0)));
	}
	// 40 bones, more than the 32 groups there may be: 32 with 4 hairs
	// each, ten apart, and 8 with one hair, each a step from one of the
	// first 8. The small ones join the group nearest them, which keeps its
	// own bone.
	{
		Groom g;
		std::vector<V3> bones;
		for (int j = 0; j < 32; j++) {
			bones.push_back({10.0f * j, 0, 0});
			for (int q = 0; q < 4; q++) {
				g.straight({10.0f * j, 0, 0.1f * q}, {10.0f * j, 0.5f, 0.1f * q});
				for (int i = 0; i < 3; i++)
					g.held((uint16_t)j, 1);
			}
		}
		for (int k = 0; k < 8; k++) {
			bones.push_back({10.0f * k + 1, 0, 0});
			g.straight({10.0f * k + 1, 0, 0.15f}, {10.0f * k + 1, 0.5f, 0.15f});
			for (int i = 0; i < 3; i++)
				g.held((uint16_t)(32 + k), 1);
		}
		Builder b = file_of({g}, bones);
		const Model m = loaded(b);
		const Primitive &h = part_of(m);
		CHECK(h.hair.strands() == 136 && h.hair.groups.size() == 32 && h.hair.runs.size() == 32 * 5 + 1);
		std::set<int> joints;
		for (const Strands::Group &group : h.hair.groups)
			joints.insert(group.joint);
		CHECK(joints.size() == 32 && *joints.begin() == 0 && *joints.rbegin() == 31);
		const std::vector<int> of = groups_of_strands(h);
		std::vector<int> count(32, 0);
		bool right = of.size() == 136 && h.hair.groups.size() == 32;
		for (size_t s = 0; right && s < of.size(); s++) {
			const float x = h.pos[h.hair.first[s]].x;
			// (A small one's root is at 10 k + 1: bone k's group.)
			const int bone = (int)(x / 10);
			right = of[s] >= 0 && of[s] < 32 && h.hair.groups[of[s]].joint == bone;
			if (right)
				count[bone]++;
		}
		CHECK(right);
		for (int j = 0; j < 32; j++)
			CHECK(count[j] == (j < 8 ? 5 : 4));
		for (const Strands::Group &group : h.hair.groups) {
			const float x = 10.0f * (float)group.joint + (group.joint < 8 ? 0.2f : 0.0f);
			CHECK(near3(group.middle, {x, 0, 0.15f}, 1e-4f));
			// (A point of one of its own hairs.)
			CHECK(group.sample < h.pos.size() &&
			      (int)(h.pos[group.sample % h.pos.size()].x / 10) == group.joint);
		}
	}
	// The point near a group's middle is one of the part's first strands
	// (only those may be posed) where the group has any there: 200 hairs
	// in a row, all but one on bone 0. Bone 0's point is the nearest its
	// middle of the first 64 drawn; the one hair on bone 1 is drawn late
	// (the 101st), and its group's point is its own.
	{
		Groom g;
		for (int s = 0; s < 200; s++) {
			g.straight({(float)s, 0, 0}, {(float)s, 0.5f, 0});
			for (int i = 0; i < 3; i++)
				g.held(s == 1 ? 1 : 0, 1);
		}
		Builder b = file_of({g}, {{0, 0, 0}, {1, 0, 0}});
		const Model m = loaded(b);
		const Primitive &h = part_of(m);
		CHECK(h.hair.strands() == 200 && h.hair.groups.size() == 2);
		if (h.hair.strands() != 200 || h.hair.groups.size() != 2)
			return;
		const Strands::Group &most = h.hair.groups[h.hair.groups[0].joint == 0 ? 0 : 1],
				     &one = h.hair.groups[h.hair.groups[0].joint == 0 ? 1 : 0];
		CHECK(most.joint == 0 && one.joint == 1);
		CHECK(near3(most.middle, {(19900.0f - 1) / 199, 0, 0}, 1e-3f) && near3(one.middle, {1, 0, 0}, 0));
		size_t late = 0;
		float nearest = 1e30f;
		for (size_t s = 0; s < 200; s++) {
			const V3 root = h.pos[h.hair.first[s]];
			if (root.x == 1)
				late = s;
			else if (s < 64)
				nearest = std::min(nearest, length(root - most.middle));
		}
		CHECK(late == 100);
		CHECK(most.sample < h.hair.first[64] && near(length(h.pos[most.sample] - most.middle), nearest, 1e-6));
		CHECK(one.sample == h.hair.first[late]);
	}
}

// --- Layers and pieces ----------------------------------------------------------

// How far along its strand each piece starts (a piece: the point it
// starts at), added up as the loader does.
static std::vector<float> piece_starts(const Primitive &p)
{
	std::vector<float> from(p.pos.size(), 0);
	for (size_t s = 0; s < p.hair.strands(); s++) {
		float run = 0;
		for (uint32_t i = p.hair.first[s]; i + 1 < p.hair.first[s + 1]; i++) {
			from[i] = run;
			run += length(p.pos[i + 1] - p.pos[i]);
		}
	}
	return from;
}

// What must hold of any part's pieces: every piece once, in the runs of
// its strand's group; a run's pieces in strand order; a group's layers
// from the root out, every hair's first piece in the first; and its reach
// no nearer than any piece of its inner layers starts.
static bool pieces_right(const Primitive &p)
{
	const Strands &h = p.hair;
	const size_t groups = h.groups.size(), layers = (size_t)h.layers;
	if (h.layers < 1 || h.runs.size() != groups * layers + 1 || h.runs.front() != 0 ||
	    h.runs.back() != h.pieces.size())
		return false;
	for (size_t r = 0; r + 1 < h.runs.size(); r++)
		if (h.runs[r] > h.runs[r + 1])
			return false;
	// Every piece once.
	std::vector<uint32_t> want, got = h.pieces;
	for (size_t s = 0; s < h.strands(); s++)
		for (uint32_t i = h.first[s]; i + 1 < h.first[s + 1]; i++)
			want.push_back(i);
	std::sort(got.begin(), got.end());
	if (got != want)
		return false;
	const std::vector<int> group_of = groups_of_strands(p);
	for (int g : group_of)
		if (g < 0)
			return false;
	const std::vector<float> from = piece_starts(p);
	for (size_t g = 0; g < groups; g++) {
		float inner = -1; // the furthest start of the layers so far
		for (size_t l = 0; l < layers; l++) {
			const uint32_t a = h.runs[g * layers + l], b = h.runs[g * layers + l + 1];
			float nearest = 1e30f, furthest = -1;
			for (uint32_t k = a; k < b; k++) {
				// (In strand order, so in point order.)
				if (k > a && h.pieces[k] <= h.pieces[k - 1])
					return false;
				nearest = std::min(nearest, from[h.pieces[k]]);
				furthest = std::max(furthest, from[h.pieces[k]]);
			}
			if (a == b)
				continue;
			// Further out than every piece of the layers inside it (so
			// pieces that start as far out are in one layer) - and past
			// the first, no piece that starts at its root.
			if (!(nearest > inner) || (l > 0 && !(nearest > 0)))
				return false;
			// The hull's layers start within its reach.
			if ((int)l < h.core && !(furthest <= h.groups[g].reach))
				return false;
			inner = furthest;
		}
	}
	return true;
}

// A coat of `strands` hairs on two bones (the left half's roots on bone
// 0, the right's on bone 1), each of 9 to 16 points unevenly apart, no
// two pieces of a group starting the same distance from their roots but
// the first of each hair.
static Groom long_coat(int strands)
{
	Groom g;
	uint32_t seed = 99;
	for (int s = 0; s < strands; s++) {
		const int points = 9 + s % 8;
		std::vector<V3> p;
		float y = 0;
		for (int i = 0; i < points; i++) {
			p.push_back({(float)(s % 20) + (i % 2 ? 0.05f : 0.0f), y, (float)(s / 20)});
			y += 0.02f + 0.03f * chance(seed);
			g.held(s % 20 < 10 ? 0 : 1, 1);
		}
		g.strand(p);
	}
	return g;
}

static void test_layers_and_pieces()
{
	const Groom g = long_coat(400);
	Builder b = file_of({g}, {{5, 0, 0}, {15, 0, 0}});
	const Model m = loaded(b, options_with(0));
	const Primitive &h = part_of(m);
	CHECK(h.hair.strands() == 400 && h.pos.size() == g.pos.size() && h.hair.groups.size() == 2);
	// 5 layers, the inner 3 the hull.
	CHECK(h.hair.layers == 5 && h.hair.core == 3 && h.hair.runs.size() == 11);
	CHECK(h.hair.pieces.size() == g.pos.size() - 400);
	CHECK(pieces_right(h));
	if (!pieces_right(h) || h.hair.groups.size() != 2)
		return;
	// As many pieces in each of a group's layers (give or take one), by
	// how far from their roots they start: layer l's all start past the
	// group's l-th fifth, and no further than the next - the piece that
	// starts just where a layer ends is that layer's last (so the first
	// layer has one more than a fifth, the last one fewer).
	const std::vector<float> from = piece_starts(h);
	for (size_t gi = 0; gi < 2; gi++) {
		std::vector<float> starts;
		for (uint32_t k = h.hair.runs[gi * 5]; k < h.hair.runs[gi * 5 + 5]; k++)
			starts.push_back(from[h.hair.pieces[k]]);
		std::sort(starts.begin(), starts.end());
		const size_t n = starts.size();
		CHECK(n > 2000);
		double ends = 0;
		size_t inner = 0;
		for (size_t l = 0; l < 5; l++) {
			const uint32_t a = h.hair.runs[gi * 5 + l], e = h.hair.runs[gi * 5 + l + 1];
			CHECK(e - a == n * (l + 1) / 5 - n * l / 5 + (l == 0 ? 1 : 0) - (l == 4 ? 1 : 0));
			bool inside = true;
			for (uint32_t k = a; k < e; k++) {
				const uint32_t piece = h.hair.pieces[k];
				const float at = from[piece];
				inside = inside && (l == 0 || at > starts[n * l / 5]) &&
					 (l == 4 || at <= starts[n * (l + 1) / 5]);
				if (l < 3) {
					ends += (double)at + (double)length(h.pos[piece + 1] - h.pos[piece]);
					inner++;
				}
			}
			CHECK(inside);
		}
		// Its reach: where its hull's layers end, three fifths of its
		// pieces starting inside it (hairs this long: well past where
		// those pieces end on average).
		CHECK(h.hair.groups[gi].reach == starts[n * 3 / 5] && h.hair.groups[gi].reach > 0.1f);
		CHECK(inner == n * 3 / 5 + 1 && ends / (double)inner < starts[n * 3 / 5]);
	}

	// How many of a run's pieces belong to the first so many strands: as
	// counted one by one - for none, a few, most, all and more than all.
	std::vector<uint32_t> strand_of(h.pos.size(), 0);
	for (size_t s = 0; s < h.hair.strands(); s++)
		for (uint32_t i = h.hair.first[s]; i < h.hair.first[s + 1]; i++)
			strand_of[i] = (uint32_t)s;
	bool counted = true;
	size_t tried = 0;
	for (size_t count : {0u, 1u, 2u, 3u, 7u, 16u, 50u, 199u, 200u, 201u, 399u, 400u, 401u, 100000u}) {
		size_t total = 0;
		for (size_t r = 0; r + 1 < h.hair.runs.size(); r++, tried++) {
			uint32_t n = 0;
			for (uint32_t k = h.hair.runs[r]; k < h.hair.runs[r + 1]; k++)
				n += strand_of[h.hair.pieces[k]] < count ? 1 : 0;
			counted = counted && h.hair.pieces_of(r, count) == n;
			total += n;
			// (Those are the run's first: what's drawn is a run's start.)
			for (uint32_t k = h.hair.runs[r]; k < h.hair.runs[r] + n; k++)
				counted = counted && strand_of[h.hair.pieces[k]] < count;
		}
		// (All the runs together: every piece of those strands.)
		const size_t strands = std::min<size_t>(count, 400);
		counted = counted && total == h.hair.first[strands] - strands;
	}
	CHECK(counted && tried == 14 * 10);
	// (A run it hasn't: none.)
	CHECK(h.hair.pieces_of(10, 400) == 0 && h.hair.pieces_of(1000000, 5) == 0);

	// How many strands lie wholly in the first so many points: as counted.
	bool whole = true;
	for (size_t points = 0; points <= h.pos.size() + 3; points++) {
		size_t n = 0;
		while (n < h.hair.strands() && h.hair.first[n + 1] <= points)
			n++;
		whole = whole && h.hair.strands_in(points) == n;
	}
	CHECK(whole);
	CHECK(h.hair.strands_in(0) == 0 && h.hair.strands_in(8) == 0 && h.hair.strands_in(h.hair.first[1]) == 1 &&
	      h.hair.strands_in(h.pos.size()) == 400 && h.hair.strands_in((size_t)1 << 40) == 400);
	CHECK(Strands().strands_in(100) == 0 && Strands().strands() == 0 && Strands().pieces_of(0, 1) == 0 &&
	      Strands().anchors() == 0);

	// Other numbers of layers: 1 to 16, the hull three fifths of them
	// (none of a single layer).
	const int asked[] = {1, 2, 3, 4, 5, 8, 16, 100, 0, -3};
	const int layers[] = {1, 2, 3, 4, 5, 8, 16, 16, 1, 1};
	const int core[] = {0, 1, 2, 2, 3, 5, 10, 10, 0, 0};
	for (int i = 0; i < 10; i++) {
		LoadOptions o = options_with(0);
		o.strand_layers = asked[i];
		const Model n = loaded(b, o);
		const Primitive &q = part_of(n);
		CHECK(q.hair.layers == layers[i] && q.hair.core == core[i]);
		CHECK(q.hair.runs.size() == 2 * (size_t)layers[i] + 1 && pieces_right(q));
		if (layers[i] == 1 && q.hair.groups.size() == 2)
			CHECK(q.hair.groups[0].reach == 0 && q.hair.groups[1].reach == 0);
	}
	// Short hairs, of fewer pieces than there are layers (2, 3 and 6
	// each): the same holds - pieces that start as far from their roots
	// are in one layer, and every hair's first piece in the first.
	auto run_sizes = [](const Primitive &q) {
		std::vector<uint32_t> sizes;
		for (size_t r = 0; r + 1 < q.hair.runs.size(); r++)
			sizes.push_back(q.hair.runs[r + 1] - q.hair.runs[r]);
		return sizes;
	};
	// (Every hair's pieces start the same distances out, so each layer
	// ends at one of those distances and takes the pieces that start there
	// with it: of 2 pieces a hair the second are all in the third layer, of
	// 3 the second in the second layer and the third in the fourth, of 6
	// the first two in the first layer and one in each after.)
	const std::vector<uint32_t> short_sizes[3] = {{1000, 0, 1000, 0, 0},
						      {1000, 1000, 0, 1000, 0},
						      {2000, 1000, 1000, 1000, 1000}};
	int short_at = 0;
	for (int points : {3, 4, 7}) {
		const Model s = loaded(lawn(40, 25, points, true));
		const Primitive &q = part_of(s);
		CHECK(q.hair.pieces.size() == 1000u * (size_t)(points - 1) && pieces_right(q));
		CHECK(q.hair.runs.size() == 6 && run_sizes(q) == short_sizes[short_at++]);
		std::vector<bool> root(q.pos.size(), false);
		for (size_t k = 0; k < q.hair.strands(); k++)
			root[q.hair.first[k]] = true;
		size_t roots_first = 0, roots_after = 0;
		for (size_t k = 0; k < q.hair.pieces.size() && q.hair.runs.size() == 6; k++)
			(k < q.hair.runs[1] ? roots_first : roots_after) += root[q.hair.pieces[k]] ? 1 : 0;
		CHECK(roots_first == 1000 && roots_after == 0);
	}
	// Hairs of one piece (fur: a root and a tip): all in the first layer -
	// the hull's - which reaches as far as they do: 0.2, their length.
	{
		const Model s = loaded(lawn(40, 25));
		const Primitive &q = part_of(s);
		CHECK(q.hair.pieces.size() == 1000 && pieces_right(q) && q.hair.core == 3);
		CHECK((run_sizes(q) == std::vector<uint32_t>{1000, 0, 0, 0, 0}));
		CHECK(q.hair.groups.size() == 1 && near(q.hair.groups[0].reach, 0.2, 1e-6));
	}
	// Of different lengths (0.1, 0.2, 0.3 and 0.6 in turn): their mean,
	// 0.3.
	{
		Groom fur;
		const float lengths[4] = {0.1f, 0.2f, 0.3f, 0.6f};
		for (int i = 0; i < 1000; i++)
			fur.straight({(float)(i % 40), 0, (float)(i / 40)},
				     {(float)(i % 40), lengths[i % 4], (float)(i / 40)});
		const Model s = loaded(fur);
		const Primitive &q = part_of(s);
		CHECK(pieces_right(q) && (run_sizes(q) == std::vector<uint32_t>{1000, 0, 0, 0, 0}));
		CHECK(q.hair.groups.size() == 1 && near(q.hair.groups[0].reach, 0.3, 1e-5));
	}
	// Hairs of two pieces, each `piece` long: the first pieces in the
	// first layer, the second - all starting `piece` out, just where the
	// third layer ends - in the third; all of it hull, which reaches
	// where those pieces end on average: one and a half pieces out.
	{
		const Model s = loaded(lawn(40, 25, 3, true));
		const Primitive &q = part_of(s);
		const float piece = std::sqrt(KINK * KINK + 0.01f);
		CHECK((run_sizes(q) == std::vector<uint32_t>{1000, 0, 1000, 0, 0}));
		CHECK(q.hair.groups.size() == 1 && near(q.hair.groups[0].reach, 1.5 * piece, 1e-5));
	}
	// Fur of two pieces (0.1 each) with a long hair every fifth (6 such
	// pieces; 2800 pieces in all, 560 a layer): the hull is every hair's
	// first two pieces - the first in the first layer, the second (the
	// second and third layers both end where they start) in the second -
	// and reaches where those end on average, 0.15; the long hairs' other
	// pieces are the outer layers, the fourth ending where their fourth
	// pieces start.
	{
		Groom both;
		for (int i = 0; i < 1000; i++) {
			std::vector<V3> hair;
			for (int k = 0; k < (i % 5 ? 3 : 7); k++)
				hair.push_back({(float)(i % 40), 0.1f * (float)k, (float)(i / 40)});
			both.strand(hair);
		}
		const Model s = loaded(both, options_with(0));
		const Primitive &q = part_of(s);
		CHECK(q.hair.pieces.size() == 800 * 2 + 200 * 6 && pieces_right(q));
		CHECK((run_sizes(q) == std::vector<uint32_t>{1000, 1000, 0, 400, 400}));
		CHECK(q.hair.groups.size() == 1 && near(q.hair.groups[0].reach, 0.15, 1e-5));
	}
	// The same pieces however it's simplified (as loaded: its kinks kept).
	const Model d = loaded(b);
	CHECK(part_of(d).pos.size() == g.pos.size() && pieces_right(part_of(d)));
	// And of a coat cut short by its points allowed.
	const Model few = loaded(b, allowed(1000));
	CHECK(part_of(few).hair.strands() > 60 && part_of(few).hair.strands() < 120 && pieces_right(part_of(few)));
}

// --- Expressions --------------------------------------------------------------

// How far from what the file says the part's points are moved by
// expression `k` (the worst of them), going by their spots.
static float worst_move(const Groom &g, const Primitive &h, size_t k)
{
	float worst = 0;
	for (size_t i = 0; i < h.pos.size(); i++) {
		const int at = g.at(h.pos[i]);
		if (at < 0 || k >= h.hair.moves.size())
			return 1e30f;
		const size_t spot = h.hair.anchor.empty() ? 0 : h.hair.anchor[i];
		const V3 moved_by = spot && spot < h.hair.moves[k].size() ? h.hair.moves[k][spot] : V3{};
		worst = std::max(worst, length(moved_by - g.shapes[k][(size_t)at]));
	}
	return worst;
}

static void test_expressions()
{
	// 120 hairs of 3 points. The first expression moves each of the first
	// 100 whole (as hair rides the skin it grows from), each its own way;
	// the second moves the first 10 of them; the third none. Each hair
	// moved follows a spot of its own, moved as the file says; the last 20
	// follow none.
	{
		Groom g = lawn(12, 10, 3, true);
		g.shapes.assign(3, std::vector<V3>(g.pos.size()));
		for (size_t i = 0; i < g.pos.size(); i++) {
			const size_t s = i / 3;
			if (s < 100)
				g.shapes[0][i] = {0.01f + 0.0001f * (float)s, 0.002f * (float)s, -0.003f};
			if (s < 10)
				g.shapes[1][i] = {0, 0.05f, 0.01f * (float)s};
		}
		const Model m = loaded(g);
		const Primitive &h = part_of(m);
		CHECK(h.pos.size() == 360 && h.morphs.empty() && m.meshes[0].weights.size() == 3);
		CHECK(h.hair.anchor.size() == 360 && h.hair.moves.size() == 3 && h.hair.anchors() == 101);
		if (h.hair.anchor.size() != 360 || h.hair.moves.size() != 3)
			return;
		std::map<int, int> strand_spot;
		bool right = true;
		for (size_t i = 0; i < 360; i++) {
			const int s = g.at(h.pos[i]) / 3;
			const int spot = h.hair.anchor[i];
			right = right && (spot != 0) == (s < 100) && spot <= 100;
			if (strand_spot.count(s))
				right = right && strand_spot[s] == spot;
			strand_spot[s] = spot;
		}
		std::set<int> spots;
		for (const auto &e : strand_spot)
			spots.insert(e.second);
		CHECK(right && spots.size() == 101);
		CHECK(worst_move(g, h, 0) < 1e-7f && worst_move(g, h, 1) < 1e-7f && worst_move(g, h, 2) == 0);
		for (int k = 0; k < 3; k++)
			CHECK(h.hair.moves[k].size() == 101 && near3(h.hair.moves[k][0], {}, 0));
	}
	// A hair an expression bends (its root still, its tip furthest): each
	// point it moves follows its own spot.
	{
		Groom g;
		g.straight({0, 0, 0}, {0, 0.5f, 0}, 6);
		g.strand({{1, 0, 0}, {1, 0.25f, 0.01f}, {1, 0.5f, 0}});
		g.shapes.assign(1, std::vector<V3>(9));
		for (int i = 0; i < 6; i++)
			g.shapes[0][i] = {0.002f * i * i, 0, 0};
		const Model m = loaded(g, options_with(0));
		const Primitive &h = part_of(m);
		CHECK(h.pos.size() == 9 && h.hair.anchors() == 6 && h.hair.anchor.size() == 9);
		std::set<int> spots;
		for (size_t i = 0; i < h.pos.size() && h.hair.anchor.size() == 9; i++) {
			const int at = g.at(h.pos[i]);
			CHECK((h.hair.anchor[i] != 0) == (at >= 1 && at <= 5));
			spots.insert(h.hair.anchor[i]);
		}
		CHECK(spots.size() == 6 && worst_move(g, h, 0) < 1e-9f);
	}
	// An expression as a short list of the points it moves (a sparse
	// one): the same.
	{
		Groom g = lawn(10, 1, 3, true);
		Builder b = file_of({g});
		const std::vector<uint32_t> at = {3, 4, 5, 28};
		const std::vector<float> by = {0, 0.01f, 0, 0, 0.01f, 0, 0, 0.01f, 0, 0.02f, 0, 0};
		b.doc["accessors"].push_back(
			{{"componentType", 5126},
			 {"count", g.pos.size()},
			 {"type", "VEC3"},
			 {"sparse",
			  {{"count", 4},
			   {"indices", {{"bufferView", b.view(at.data(), 16)}, {"componentType", 5125}}},
			   {"values", {{"bufferView", b.view(by.data(), 48)}}}}}});
		b.doc["meshes"][0]["primitives"][0]["targets"] = {{{"POSITION", (int)b.doc["accessors"].size() - 1}}};
		const Model m = loaded(b);
		const Primitive &h = part_of(m);
		CHECK(h.pos.size() == 30 && h.morphs.empty() && h.hair.anchors() == 3 && h.hair.anchor.size() == 30);
		g.shapes.assign(1, std::vector<V3>(30));
		g.shapes[0][3] = g.shapes[0][4] = g.shapes[0][5] = {0, 0.01f, 0};
		g.shapes[0][28] = {0.02f, 0, 0};
		CHECK(worst_move(g, h, 0) < 1e-9f);
		size_t moved = 0;
		for (uint16_t a : h.hair.anchor)
			moved += a ? 1 : 0;
		CHECK(moved == 4);
	}
	// Expressions that move no hair (a face's, on a mesh the hair shares):
	// no spots, nothing kept.
	{
		Groom g = lawn(10, 1, 3, true);
		g.shapes.assign(2, std::vector<V3>(g.pos.size()));
		const Model m = loaded(g);
		const Primitive &h = part_of(m);
		CHECK(h.pos.size() == 30 && h.morphs.empty() && h.hair.anchor.empty() && h.hair.anchors() == 0);
		// And a strand part with none of the mesh's expressions on it.
		Groom bare = lawn(10, 1, 3, true);
		Builder b = file_of({bare});
		const std::vector<V3> tri = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, up = {{0, 0, 1}, {}, {}};
		b.doc["meshes"][0]["primitives"].push_back({{"attributes", {{"POSITION", b.vec3s(tri)}}},
							    {"indices", b.indices({0, 1, 2})},
							    {"targets", {{{"POSITION", b.vec3s(up)}}}}});
		const Model n = loaded(b);
		CHECK(n.meshes[0].weights.size() == 1 && part_of(n, 1).morphs.size() == 1);
		CHECK(part_of(n, 0).strands && part_of(n, 0).morphs.empty() && part_of(n, 0).hair.anchor.empty() &&
		      part_of(n, 0).hair.anchors() == 0);
	}
	// An expression that doesn't fit its part: refused.
	{
		Groom g = lawn(10, 1, 3, true);
		g.shapes.assign(1, std::vector<V3>(g.pos.size() - 1, V3{0, 0.01f, 0}));
		Builder b = file_of({g});
		Model m;
		std::string why;
		CHECK(!load(b, m, LoadOptions(), &why) && !why.empty());
	}

	// A big coat: 5000 hairs of 4 points, far more points than the 2047
	// spots there may be. Points near each other share a spot, and are
	// moved by the mean of what the file says for them all.
	{
		Groom g;
		for (int z = 0; z < 50; z++)
			for (int x = 0; x < 100; x++) {
				std::vector<V3> s;
				for (int i = 0; i < 4; i++)
					s.push_back({0.01f * x + (i % 2 ? 0.002f : 0.0f), 0.01f * i, 0.01f * z});
				g.strand(s);
			}
		g.shapes.assign(2, std::vector<V3>(g.pos.size()));
		float biggest[2] = {0, 0};
		for (size_t i = 0; i < g.pos.size(); i++) {
			const V3 p = g.pos[i], over = {std::sin(3 * p.x), 0.5f, std::cos(4 * p.z)};
			// A swell that changes smoothly over the coat, a tenth
			// more at the tips than the roots (so no hair moves whole)...
			g.shapes[0][i] = over * (0.02f * (1 + 3.5f * p.y));
			// ...and a ruffle that leaves the roots still and grows up
			// each hair.
			g.shapes[1][i] = over * (0.5f * p.y);
			for (int k = 0; k < 2; k++)
				biggest[k] = std::max(biggest[k], length(g.shapes[(size_t)k][i]));
		}
		for (int k = 0; k < 2; k++) {
			Groom one = g;
			one.shapes = {g.shapes[(size_t)k]};
			const Model m = loaded(one, options_with(0));
			const Primitive &h = part_of(m);
			CHECK(h.pos.size() == 20000 && h.hair.anchor.size() == 20000 && h.morphs.empty());
			CHECK(h.hair.anchors() == Strands::MOST_ANCHORS + 1 && Strands::MOST_ANCHORS == 2047);
			uint16_t most = 0;
			size_t still_points = 0, still_roots = 0;
			for (size_t i = 0; i < h.hair.anchor.size(); i++) {
				most = std::max(most, h.hair.anchor[i]);
				still_points += h.hair.anchor[i] ? 0 : 1;
				still_roots += !h.hair.anchor[i] && h.hair.along[i] == 0 ? 1 : 0;
			}
			// (The ruffle's roots follow no spot; the swell moves all.)
			CHECK(most == 2047 && still_points == (k ? 5000u : 0u) && still_roots == still_points);
			const float worst = worst_move(one, h, 0);
			// The swell: every point within a tenth of the biggest move
			// of where the file would move it. The ruffle: within a
			// third (a spot's points are near each other on the coat,
			// but may be at any height up their hairs - and are all
			// moved as the ones halfway up are).
			CHECK(worst < biggest[k] * (k ? 0.34f : 0.1f));
			std::printf(
				"unit_hair: 20000 points on 2047 spots, %s: the worst %.5f off (the biggest move %.5f)\n",
				k ? "a ruffle that grows up each hair" : "a swell over the coat", worst, biggest[k]);
		}
	}
	// The same coat riding its skin (each hair moved whole, by what moves
	// its root): 5000 hairs on 2047 spots, each within a twentieth.
	{
		Groom g = lawn(100, 50, 3, true);
		for (V3 &v : g.pos)
			v = {v.x * 0.01f, v.y * 0.1f, v.z * 0.01f};
		g.shapes.assign(1, std::vector<V3>(g.pos.size()));
		float biggest = 0;
		for (size_t i = 0; i < g.pos.size(); i++) {
			const V3 root = g.pos[i - i % 3];
			g.shapes[0][i] = V3{std::sin(3 * root.x), 0.5f, std::cos(4 * root.z)} * 0.01f;
			biggest = std::max(biggest, length(g.shapes[0][i]));
		}
		const Model m = loaded(g);
		const Primitive &h = part_of(m);
		CHECK(h.pos.size() == 15000 && h.hair.anchors() == 2048);
		bool whole = h.hair.anchor.size() == 15000;
		for (size_t i = 0; whole && i < 15000; i++)
			whole = h.hair.anchor[i] != 0 && h.hair.anchor[i] == h.hair.anchor[i - i % 3];
		CHECK(whole);
		const float worst = worst_move(g, h, 0);
		CHECK(worst < 0.05f * biggest);
		std::printf(
			"unit_hair: 5000 hairs moved whole on 2047 spots, the worst %.6f off (the biggest move %.5f)\n",
			worst, biggest);
	}
}

// --- Expressions, strand by strand ----------------------------------------------

// An expression's moves said strand by strand: the strands it moves (by
// their numbers, rising) and how far each, all of a strand's points alike.
// (None: an expression that moves none of the part's.)
struct ByStrand {
	std::vector<uint32_t> strands;
	std::vector<V3> by;
};

// A part's expressions strand by strand, as a file says them (this
// add-on's own extra on the part, extras.direbot.strand_morphs): the point
// each strand starts at (and where the last ends), then each expression.
static json strand_morphs(Builder &b, const std::vector<uint32_t> &starts, const std::vector<ByStrand> &each)
{
	json targets = json::array();
	for (const ByStrand &e : each) {
		if (e.strands.empty())
			targets.push_back(nullptr);
		else
			targets.push_back({{"strands", b.indices32(e.strands)}, {"POSITION", b.vec3s(e.by)}});
	}
	return {{"direbot", {{"strand_morphs", {{"strand_start", b.indices32(starts)}, {"targets", targets}}}}}};
}

// A coat's expressions (each of which moves every strand it moves whole)
// strand by strand: each moved strand's first point's move.
static std::vector<ByStrand> by_strand_of(const Groom &g)
{
	std::vector<ByStrand> each(g.shapes.size());
	for (size_t k = 0; k < g.shapes.size(); k++)
		for (size_t s = 0; s < g.strands(); s++) {
			const V3 by = g.shapes[k][g.first[s]];
			if (dot(by, by) > 0) {
				each[k].strands.push_back((uint32_t)s);
				each[k].by.push_back(by);
			}
		}
	return each;
}

// A file of one coat whose expressions are said strand by strand (`each`;
// its strands starting at `starts`), with what it says the usual way too:
//   Still  an expression each, that moves no point (numbers with nothing
//          behind them: all zeros) - as a groom is written out
//   Body   none of its own: the mesh's expressions are a body's beside it
//          (a triangle, moved by each)
//   Own    the coat's own (Groom::shapes), to be added to
enum class Usual { Still, Body, Own };
static Builder by_strand_file(Groom g, const std::vector<uint32_t> &starts, const std::vector<ByStrand> &each,
			      Usual usual, size_t expressions)
{
	if (usual != Usual::Own)
		g.shapes.clear();
	Builder b = file_of({g});
	json &part = b.doc["meshes"][0]["primitives"][0];
	if (usual == Usual::Still) {
		b.doc["accessors"].push_back({{"componentType", 5126}, {"count", g.pos.size()}, {"type", "VEC3"}});
		const int zeros = (int)b.doc["accessors"].size() - 1;
		for (size_t k = 0; k < expressions; k++)
			part["targets"].push_back({{"POSITION", zeros}});
	}
	if (usual == Usual::Body) {
		const std::vector<V3> tri = {{0, -1, 0}, {1, -1, 0}, {0, -1, 1}}, up = {{0, 0.1f, 0}, {}, {}};
		json targets = json::array();
		for (size_t k = 0; k < expressions; k++)
			targets.push_back({{"POSITION", b.vec3s(up)}});
		b.doc["meshes"][0]["primitives"].push_back({{"attributes", {{"POSITION", b.vec3s(tri)}}},
							    {"indices", b.indices({0, 1, 2})},
							    {"targets", targets}});
	}
	// (`part` may have moved when the body was added.)
	b.doc["meshes"][0]["primitives"][0]["extras"] = strand_morphs(b, starts, each);
	return b;
}

// Two parts' strands the same, following the same spots, moved the same.
static bool same_moves(const Primitive &a, const Primitive &b)
{
	if (!same_points(a.pos, b.pos) || a.hair.first != b.hair.first || a.hair.anchor != b.hair.anchor ||
	    a.hair.moves.size() != b.hair.moves.size())
		return false;
	for (size_t k = 0; k < a.hair.moves.size(); k++)
		if (!same_points(a.hair.moves[k], b.hair.moves[k]))
			return false;
	return true;
}

// Whether a file is refused, and how the loader says why - found out in a
// process of its own, so a crash there is an answer here: "" if it loads,
// "crashed" if reading it brings the program down.
static std::string refused(Builder &b, const LoadOptions &options = LoadOptions())
{
	int ends[2];
	if (pipe(ends) != 0)
		return "no pipe";
	std::fflush(nullptr);
	const pid_t child = fork();
	if (child == 0) {
		close(ends[0]);
		Model m;
		std::string why;
		const bool ok = load(b, m, options, &why);
		const std::string say = ok ? std::string() : why.empty() ? std::string("refused, nothing said") : why;
		const ssize_t wrote = write(ends[1], say.data(), say.size());
		std::fflush(nullptr);
		_exit(wrote == (ssize_t)say.size() ? 0 : 1);
	}
	close(ends[1]);
	std::string said;
	char some[256];
	for (ssize_t n; (n = read(ends[0], some, sizeof some)) > 0;)
		said.append(some, (size_t)n);
	close(ends[0]);
	int status = -1;
	const bool ended = child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) &&
			   WEXITSTATUS(status) == 0;
	return ended ? said : std::string("crashed");
}

static void test_expressions_by_strand()
{
	// 120 hairs of 3 points, as test_expressions' first: the first
	// expression moves each of the first 100 whole, each its own way, the
	// second the first 10, the third none. Said strand by strand - with
	// the usual lists left still, as a groom is written out, or with none
	// of its own beside a body's - it's read just as said point by point:
	// the same spots, the same moves.
	Groom g = lawn(12, 10, 3, true);
	g.shapes.assign(3, std::vector<V3>(g.pos.size()));
	for (size_t i = 0; i < g.pos.size(); i++) {
		const size_t s = i / 3;
		if (s < 100)
			g.shapes[0][i] = {0.01f + 0.0001f * (float)s, 0.002f * (float)s, -0.003f};
		if (s < 10)
			g.shapes[1][i] = {0, 0.05f, 0.01f * (float)s};
	}
	const std::vector<ByStrand> each = by_strand_of(g);
	CHECK(each.size() == 3 && each[0].strands.size() == 100 && each[1].strands.size() == 10 &&
	      each[2].strands.empty());
	const Model usual = loaded(g);
	CHECK(part_of(usual).hair.anchors() == 101 && part_of(usual).hair.moves.size() == 3);
	for (Usual how : {Usual::Still, Usual::Body}) {
		Builder b = by_strand_file(g, g.first, each, how, 3);
		const Model m = loaded(b);
		const Primitive &h = part_of(m);
		CHECK(h.strands && h.morphs.empty() && m.meshes[0].weights.size() == 3);
		CHECK(h.hair.anchors() == 101 && h.hair.anchor.size() == 360 && same_moves(h, part_of(usual)));
		// (And as the file says: every point of a moved strand by its
		// strand's move.)
		CHECK(worst_move(g, h, 0) < 1e-7f && worst_move(g, h, 1) < 1e-7f && worst_move(g, h, 2) == 0);
	}
	// The same of strands simplified, put in the order they're drawn in,
	// and cut short: 1000 straight hairs of 3 points, 2 of each kept and
	// room for 300 of them. One expression moves every third hair, each its
	// own way; another the last 500, all one way.
	{
		Groom fur = lawn(40, 25);
		fur.shapes.assign(2, std::vector<V3>(fur.pos.size()));
		for (size_t i = 0; i < fur.pos.size(); i++) {
			const size_t s = i / 3;
			if (s % 3 == 0)
				fur.shapes[0][i] = {0.02f * (float)(s % 7), 0.01f, 0.0005f * (float)s};
			if (s >= 500)
				fur.shapes[1][i] = {0, 0.03f, 0};
		}
		const Model point_by_point = loaded(fur, allowed(600));
		Builder b = by_strand_file(fur, fur.first, by_strand_of(fur), Usual::Still, 2);
		const Model strand_by_strand = loaded(b, allowed(600));
		const Primitive &h = part_of(strand_by_strand);
		CHECK(h.hair.strands() == 300 && h.pos.size() == 600 && h.hair.anchors() > 100);
		CHECK(same_moves(h, part_of(point_by_point)));
		CHECK(worst_move(fur, h, 0) < 1e-7f && worst_move(fur, h, 1) < 1e-7f);
	}
	// And of a coat whose strands are rated (thin fur and a deep mane, room
	// for 600 of its 2048 strands): the fur's hairs moved, each its own way.
	{
		Groom coat = joined(fur_patch(), mane_patch());
		coat.shapes.assign(1, std::vector<V3>(coat.pos.size()));
		for (size_t i = 0; i < coat.pos.size(); i++)
			if (!of_mane(coat.pos[i - i % 3]))
				coat.shapes[0][i] = {0.001f * (float)(i / 3 % 32), 0.002f, 0.0001f * (float)(i / 3)};
		const Model point_by_point = loaded(coat, allowed(1200));
		Builder b = by_strand_file(coat, coat.first, by_strand_of(coat), Usual::Still, 1);
		const Model strand_by_strand = loaded(b, allowed(1200));
		const Primitive &h = part_of(strand_by_strand);
		CHECK(h.hair.strands() == 600 && halves_of(h).fur > 400 && h.hair.anchors() > 100);
		CHECK(same_moves(h, part_of(point_by_point)) && worst_move(coat, h, 0) < 1e-7f);
	}

	// The strands a file's moves go by are its own to say: they needn't be
	// all the part's hairs, nor one hair each.
	{
		// From the 11th hair on (its numbers start there): strands 0 to 9
		// are hairs 10 to 19.
		std::vector<uint32_t> later(g.first.begin() + 10, g.first.end());
		ByStrand ten;
		for (uint32_t s = 0; s < 10; s++) {
			ten.strands.push_back(s);
			ten.by.push_back({0.01f * (float)(s + 1), 0.02f, 0});
		}
		Groom want = g;
		want.shapes.assign(1, std::vector<V3>(g.pos.size()));
		for (size_t i = 30; i < 60; i++)
			want.shapes[0][i] = ten.by[i / 3 - 10];
		Builder b = by_strand_file(g, later, {ten}, Usual::Still, 1);
		CHECK(same_moves(part_of(loaded(b)), part_of(loaded(want))));
		// Up to the 50th hair only: the rest are no strand of theirs, and
		// never moved.
		std::vector<uint32_t> early(g.first.begin(), g.first.begin() + 51);
		Groom first_ten = g;
		first_ten.shapes.assign(1, std::vector<V3>(g.pos.size()));
		for (size_t i = 0; i < 30; i++)
			first_ten.shapes[0][i] = ten.by[i / 3];
		Builder c = by_strand_file(g, early, {ten}, Usual::Still, 1);
		CHECK(same_moves(part_of(loaded(c)), part_of(loaded(first_ten))));
		// Two hairs a strand, and strands of no points between them (their
		// starts never fall, but may stay): strand 2k + 1 is hairs 2k and
		// 2k + 1, strand 2k none.
		std::vector<uint32_t> pairs;
		for (size_t s = 0; s <= 120; s += 2) {
			pairs.push_back(g.first[s]);
			pairs.push_back(g.first[s]);
		}
		ByStrand some;
		for (uint32_t k : {1u, 4u, 5u, 41u, 119u}) {
			some.strands.push_back(k);
			some.by.push_back({0, 0.01f * (float)(k + 1), 0.03f});
		}
		Groom paired = g;
		paired.shapes.assign(1, std::vector<V3>(g.pos.size()));
		for (size_t i = 0; i < g.pos.size(); i++) {
			const uint32_t strand = 2 * (uint32_t)(i / 6) + 1;
			for (size_t k = 0; k < some.strands.size(); k++)
				if (some.strands[k] == strand)
					paired.shapes[0][i] = some.by[k];
		}
		Builder d = by_strand_file(g, pairs, {some}, Usual::Still, 1);
		const Model m = loaded(d);
		// (Strands 1, 5, 41 and 119 are hairs 0 and 1, 4 and 5, 40 and 41,
		// 118 and 119; strand 4 has no points.)
		CHECK(same_moves(part_of(m), part_of(loaded(paired))) && part_of(m).hair.anchors() == 9);
	}

	// Both ways together add. The usual way moves hairs 0 to 59, each its
	// own way; strand by strand hairs 40 to 99 are moved - 50 to 54 just
	// back again, which leaves them still. Read as one coat whose every
	// point is moved by the two together.
	{
		Groom own = g;
		own.shapes.assign(1, std::vector<V3>(g.pos.size()));
		ByStrand more;
		Groom sum = own;
		for (uint32_t s = 0; s < 100; s++) {
			const V3 usual_way = s < 60 ? V3{0.01f + 0.0005f * (float)s, 0.002f * (float)s, -0.003f} : V3{};
			V3 by_strand = s >= 40 ? V3{-0.004f, 0.001f * (float)s, 0.02f} : V3{};
			if (s >= 50 && s < 55)
				by_strand = usual_way * -1.0f;
			for (size_t i = g.first[s]; i < g.first[s + 1]; i++) {
				own.shapes[0][i] = usual_way;
				sum.shapes[0][i] = usual_way + by_strand;
			}
			if (s >= 40) {
				more.strands.push_back(s);
				more.by.push_back(by_strand);
			}
		}
		Builder b = by_strand_file(own, g.first, {more}, Usual::Own, 1);
		const Model m = loaded(b), want = loaded(sum);
		// (95 hairs moved, and the spot for none.)
		CHECK(part_of(want).hair.anchors() == 96 && same_moves(part_of(m), part_of(want)));
		CHECK(worst_move(sum, part_of(m), 0) < 1e-7f);
		bool still = part_of(m).hair.anchor.size() == 360;
		for (size_t i = 0; still && i < 360; i++) {
			const int s = g.at(part_of(m).pos[i]) / 3;
			still = (part_of(m).hair.anchor[i] == 0) == ((s >= 50 && s < 55) || s >= 100);
		}
		CHECK(still);
		// A hair an expression bends the usual way (its root still, its tip
		// furthest), and moves whole strand by strand: each point by both.
		Groom bent;
		bent.straight({0, 0, 0}, {0, 0.5f, 0}, 6);
		bent.strand({{1, 0, 0}, {1, 0.25f, 0.01f}, {1, 0.5f, 0}});
		bent.shapes.assign(1, std::vector<V3>(9));
		for (int i = 0; i < 6; i++)
			bent.shapes[0][(size_t)i] = {0.002f * (float)(i * i), 0, 0};
		ByStrand whole;
		whole.strands = {0, 1};
		whole.by = {{0, 0, 0.05f}, {0, 0.01f, 0}};
		Groom bent_sum = bent;
		for (size_t i = 0; i < 9; i++)
			bent_sum.shapes[0][i] = bent.shapes[0][i] + whole.by[i < 6 ? 0 : 1];
		Builder c = by_strand_file(bent, bent.first, {whole}, Usual::Own, 1);
		const Model both = loaded(c, options_with(0)), both_want = loaded(bent_sum, options_with(0));
		CHECK(part_of(both).pos.size() == 9 && part_of(both).hair.anchors() == 8);
		CHECK(same_moves(part_of(both), part_of(both_want)) && worst_move(bent_sum, part_of(both), 0) < 1e-8f);
	}

	// Expressions the part's list doesn't reach, or reaches past: a mesh of
	// 3 expressions whose part lists 1, or 5 - the first 3 at most are
	// read, the rest (of the mesh's) move nothing.
	{
		std::vector<ByStrand> one(each.begin(), each.begin() + 1), five = each;
		five.push_back(each[1]);
		five.push_back(each[0]);
		Groom first_only = g;
		first_only.shapes[1].assign(g.pos.size(), V3{});
		Builder b = by_strand_file(g, g.first, one, Usual::Still, 3);
		CHECK(same_moves(part_of(loaded(b)), part_of(loaded(first_only))));
		Builder c = by_strand_file(g, g.first, five, Usual::Still, 3);
		CHECK(same_moves(part_of(loaded(c)), part_of(usual)));
		// Nothing in the list that's a list of strands (a number, a word,
		// an empty thing): no moves of that expression's.
		Builder d = by_strand_file(g, g.first, each, Usual::Still, 3);
		json &targets = d.doc["meshes"][0]["primitives"][0]["extras"]["direbot"]["strand_morphs"]["targets"];
		targets[1] = 7;
		targets[2] = json::object();
		CHECK(same_moves(part_of(loaded(d)), part_of(loaded(first_only))));
		targets[1] = "none";
		CHECK(same_moves(part_of(loaded(d)), part_of(loaded(first_only))));
		// No list of where its strands start: nothing said strand by strand
		// is read at all.
		Builder e = by_strand_file(g, g.first, each, Usual::Still, 3);
		e.doc["meshes"][0]["primitives"][0]["extras"]["direbot"]["strand_morphs"].erase("strand_start");
		const Model none = loaded(e);
		CHECK(part_of(none).hair.anchors() == 0 && part_of(none).hair.anchor.empty() &&
		      part_of(none).pos.size() == 360);
		// (Nor of an extra that's no such thing.)
		Builder f = by_strand_file(g, g.first, each, Usual::Still, 3);
		f.doc["meshes"][0]["primitives"][0]["extras"]["direbot"]["strand_morphs"] = 5;
		CHECK(part_of(loaded(f)).hair.anchors() == 0);
	}

	// Lists that don't fit their part: refused, in plain words - never a
	// crash, never half read.
	const std::string unfit = "the file is damaged (its hair's moves don't fit its strands)",
			  unread = "the file has numbers DireBot can't read";
	auto with_starts = [&](const std::vector<uint32_t> &starts) {
		// (One expression, that moves its first strand.)
		ByStrand e;
		e.strands = {0};
		e.by = {{0, 0.01f, 0}};
		Builder b = by_strand_file(g, starts, {e}, Usual::Still, 3);
		return refused(b);
	};
	auto with_list = [&](const std::vector<uint32_t> &strands, size_t moves) {
		ByStrand e;
		e.strands = strands;
		e.by.assign(moves, V3{0, 0.01f, 0});
		Builder b = by_strand_file(g, g.first, {e}, Usual::Still, 3);
		return refused(b);
	};
	// (As they should be: read.)
	CHECK(with_starts(g.first).empty() && with_list({0, 5, 119}, 3).empty());
	{
		// Where its strands start: falling; past its points; one number
		// only, or none.
		std::vector<uint32_t> falling = g.first;
		std::swap(falling[40], falling[41]);
		CHECK(with_starts(falling) == unfit);
		std::vector<uint32_t> past = g.first;
		past.back() = (uint32_t)g.pos.size() + 1;
		CHECK(with_starts(past) == unfit);
		past.back() = 0xFFFFFFFFu;
		CHECK(with_starts(past) == unfit);
		CHECK(with_starts({0}) == unfit);
		CHECK(with_starts({360}) == unfit);
		// (All its points one strand, or strands of no points: fits.)
		CHECK(with_starts({0, 360}).empty() && with_starts({360, 360}).empty() &&
		      with_starts({0, 0, 0}).empty());
		// The strands an expression moves: one past the last (strand 120
		// of 120, numbered from 0), or far past; not rising (one twice,
		// two the wrong way round).
		CHECK(with_list({0, 5, 120}, 3) == unfit);
		CHECK(with_list({0xFFFFFFFFu}, 1) == unfit);
		CHECK(with_list({5, 5}, 2) == unfit);
		CHECK(with_list({7, 3}, 2) == unfit);
		// Its moves: more of them than strands, or fewer.
		CHECK(with_list({0, 5, 119}, 4) == unfit);
		CHECK(with_list({0, 5, 119}, 2) == unfit);
		CHECK(with_list({3}, 120) == unfit);
	}
	{
		// Numbers of the wrong kind: where strands start, or which strands,
		// as fractions; a list of pairs; moves of two numbers each.
		Builder b = by_strand_file(g, g.first, each, Usual::Still, 3);
		json &extra = b.doc["meshes"][0]["primitives"][0]["extras"]["direbot"]["strand_morphs"];
		std::vector<float> as_floats(g.first.begin(), g.first.end());
		const json starts = extra["strand_start"];
		extra["strand_start"] = b.floats(as_floats, 1, "SCALAR");
		CHECK(refused(b) == unread);
		extra["strand_start"] = starts;
		CHECK(refused(b).empty());
		const json strands = extra["targets"][1]["strands"];
		extra["targets"][1]["strands"] = b.floats({0, 1, 2, 3, 4, 5, 6, 7, 8, 9}, 1, "SCALAR");
		CHECK(refused(b) == unread);
		extra["targets"][1]["strands"] = b.accessor(b.view(g.first.data(), 80), 5125, 10, "VEC2");
		CHECK(refused(b) == unread);
		extra["targets"][1]["strands"] = strands;
		extra["targets"][1]["POSITION"] = b.floats(std::vector<float>(20, 0.01f), 2, "VEC2");
		CHECK(refused(b) == unread);
		// Moves it names but the file hasn't (no such list, or none said).
		extra["targets"][1]["POSITION"] = 100000;
		CHECK(!refused(b).empty() && refused(b) != "crashed");
		extra["targets"][1].erase("POSITION");
		CHECK(!refused(b).empty() && refused(b) != "crashed");
		// Where its strands start, named but not there.
		extra["targets"][1] = nullptr;
		extra["strand_start"] = 100000;
		CHECK(!refused(b).empty() && refused(b) != "crashed");
		// A list given as a few changed numbers over nothing (a sparse
		// one): not read.
		const std::vector<uint32_t> at = {1, 2}, to = {3, 6};
		b.doc["accessors"].push_back(
			{{"componentType", 5125},
			 {"count", 121},
			 {"type", "SCALAR"},
			 {"sparse",
			  {{"count", 2},
			   {"indices", {{"bufferView", b.view(at.data(), 8)}, {"componentType", 5125}}},
			   {"values", {{"bufferView", b.view(to.data(), 8)}}}}}});
		extra["strand_start"] = (int)b.doc["accessors"].size() - 1;
		CHECK(refused(b) == unread);
	}
	// Refused the same when its strands are cut short, or none are kept at
	// all (stand-in hair worn instead): what's wrong with the file is said
	// before any strand is chosen.
	{
		ByStrand e;
		e.strands = {0, 5, 120};
		e.by.assign(3, V3{0, 0.01f, 0});
		Builder b = by_strand_file(g, g.first, {e}, Usual::Still, 3);
		CHECK(refused(b, allowed(30)) == unfit && refused(b, allowed(0, 0)) == unfit);
	}
}

// --- Stand-in hair --------------------------------------------------------------

static void test_stand_in_hair()
{
	// A character with strands, and strips to wear instead: a material
	// marked extras.direbot.hair_standin.
	const Groom g = lawn(20, 10);
	auto file = [&g](json extras) {
		Builder b = file_of({g});
		const std::vector<V3> strip = {{0, 0, 0}, {20, 0, 0}, {0, 0, 10}};
		b.doc["meshes"][0]["primitives"].push_back({{"attributes", {{"POSITION", b.vec3s(strip)}}},
							    {"indices", b.indices({0, 1, 2})},
							    {"material", 0}});
		json material = {{"name", "strips"}};
		if (!extras.is_null())
			material["extras"] = extras;
		b.doc["materials"].push_back(material);
		return b;
	};
	Builder with = file({{"direbot", {{"hair_standin", true}}}});
	Builder without = file(json());
	Builder off = file({{"direbot", {{"hair_standin", false}}}});
	// Strands wanted: kept, stand-in hair or no.
	{
		const Model m = loaded(with);
		CHECK(m.hair_standins && m.materials.size() == 1 && m.materials[0].hair_standin);
		CHECK(part_of(m, 0).hair.strands() == 200 && m.strands == 200 && m.strand_points == 400);
		const Model n = loaded(without);
		CHECK(!n.hair_standins && !n.materials[0].hair_standin && n.strands == 200);
		CHECK(!loaded(off).hair_standins);
	}
	// None wanted (strand_points 0): one with stand-in hair keeps none -
	// its file's count still known, the part still there, empty.
	{
		const Model m = loaded(with, allowed(0, 100));
		CHECK(m.hair_standins && m.strands_in_file == 200 && m.strands == 0 && m.strand_points == 0);
		CHECK(m.meshes[0].parts.size() == 2 && part_of(m, 0).strands && part_of(m, 0).pos.empty() &&
		      part_of(m, 0).hair.in_file == 200 && part_of(m, 0).hair.strands() == 0);
		CHECK(part_of(m, 1).index.size() == 3);
		// (It stands, measured and posed, without them.)
		auto shared = std::make_shared<Model>(m);
		Rig rig(shared, RigOptions());
		const Bounds box = rig.rest_bounds();
		Drive d;
		d.dt = 1.0f / 60;
		rig.pose(d);
		CHECK(box.found && near3(box.low, {0, 0, 0}, 1e-5f) && near3(box.high, {20, 0, 10}, 1e-5f));
		CHECK(rig.parts().size() == 2 && rig.parts()[0].pos.empty() && rig.parts()[1].pos.size() == 3);
	}
	// One with none to wear keeps a few all the same: as many as
	// strand_points_bare allows.
	for (Builder *b : {&without, &off}) {
		const Model m = loaded(*b, allowed(0, 100));
		CHECK(!m.hair_standins && m.strands_in_file == 200 && m.strands == 50 && m.strand_points == 100);
		const Model all = loaded(*b, allowed(0, 100000));
		CHECK(all.strands == 200);
		CHECK(loaded(*b, allowed(0, 0)).strands == 0);
	}
	// Stand-in hair that nothing wears (a material marked so, on no part)
	// is none to wear: the character keeps its strands as one with none.
	{
		Builder unworn = file(json());
		unworn.doc["materials"].push_back(
			{{"name", "spare"}, {"extras", {{"direbot", {{"hair_standin", true}}}}}});
		const Model m = loaded(unworn);
		CHECK(!m.hair_standins && m.materials.size() == 2 && !m.materials[0].hair_standin &&
		      m.materials[1].hair_standin && m.strands == 200);
		const Model few = loaded(unworn, allowed(0, 100));
		CHECK(!few.hair_standins && few.strands == 50 && few.strand_points == 100);
		for (bool on_card : {false, true})
			CHECK(loaded(unworn, hair_load_options(HairDetail::Today, on_card)).strands == 200);
		// (A part that names a material the file hasn't wears none.)
		unworn.doc["meshes"][0]["primitives"][1]["material"] = 2;
		CHECK(!loaded(unworn, allowed(0, 100)).hair_standins);
		// Worn by the strips: stand-in hair after all.
		unworn.doc["meshes"][0]["primitives"][1]["material"] = 1;
		const Model worn = loaded(unworn, allowed(0, 100));
		CHECK(worn.hair_standins && worn.strands == 0 && worn.strands_in_file == 200);
	}
	// As the hair detail setting loads them: Today wears the stand-in
	// hair where there is some, Full and Light the strands.
	for (bool on_card : {false, true}) {
		CHECK(loaded(with, hair_load_options(HairDetail::Today, on_card)).strands == 0);
		CHECK(loaded(without, hair_load_options(HairDetail::Today, on_card)).strands == 200);
		CHECK(loaded(with, hair_load_options(HairDetail::Light, on_card)).strands == 200);
		CHECK(loaded(with, hair_load_options(HairDetail::Full, on_card)).strands == 200);
	}
}

// --- Posed by the rig -----------------------------------------------------------

static Drive still()
{
	Drive d;
	d.dt = 1.0f / 60;
	return d;
}

// A matrix's biggest stretch any way.
static float stretch_of(const M4 &m)
{
	return std::max({length(V3{m.m[0], m.m[1], m.m[2]}), length(V3{m.m[4], m.m[5], m.m[6]}),
			 length(V3{m.m[8], m.m[9], m.m[10]})});
}

// A posed part's place in its model: its node's mesh's part.
static const Primitive &prim_of(const Model &m, const Rig::Part &p)
{
	return m.meshes[(size_t)m.nodes[(size_t)p.node].mesh].parts[(size_t)p.part];
}

// Point i of a strand part where its bones (or, with none, its node) put
// it, worked out by hand: moved by its spot's move if `spots` has any,
// then by each bone that holds it; and the way its strand runs there.
static void by_hand(const Rig &rig, const Rig::Part &part, size_t i, const std::vector<V3> &spots, V3 &pos, V3 &normal)
{
	const Model &m = rig.model();
	const Primitive &p = prim_of(m, part);
	const Node &node = m.nodes[(size_t)part.node];
	V3 at = p.pos[i];
	if (!spots.empty() && !p.hair.anchor.empty())
		at = at + spots[p.hair.anchor[i]];
	if (node.skin < 0 || p.joints.empty()) {
		pos = point(rig.world()[(size_t)part.node], at);
		normal = normalized(direction(rig.world()[(size_t)part.node], p.normal[i]), {});
		return;
	}
	const Skin &skin = m.skins[(size_t)node.skin];
	pos = normal = V3{};
	for (int k = 0; k < 4; k++) {
		const float w = p.weights[i * 4 + k];
		const size_t j = p.joints[i * 4 + k];
		if (!(w > 0) || j >= skin.joints.size())
			continue;
		const M4 bone = rig.world()[(size_t)skin.joints[j]] * skin.inverse_bind[j];
		pos = pos + point(bone, at) * w;
		normal = normal + direction(bone, p.normal[i]) * w;
	}
	normal = normalized(normal, {});
}

// How far the worst of a posed part's points (and the ways its strands
// run) is from where they go by hand.
static float worst_posed(const Rig &rig, const Rig::Part &part, const std::vector<V3> &spots)
{
	float worst = part.pos.size() == part.normal.size() ? 0.0f : 1e30f;
	for (size_t i = 0; i < part.pos.size() && i < part.normal.size(); i++) {
		V3 pos, normal;
		by_hand(rig, part, i, spots, pos, normal);
		worst = std::max({worst, length(part.pos[i] - pos), length(part.normal[i] - normal)});
	}
	return worst;
}

// A character for posing: three bones in a chain, bent where they join
// (not as its skin was made: straight), 30 hairs of 10 points hanging
// down them, each point shared between two bones; and, on a node of its
// own with no bones (moved, turned and twice the size), a tuft of 8
// hairs. Two expressions: the first shifts the chain's first 10 hairs
// whole and bends the tuft, the second bends the chain's next 10.
struct Posable {
	Groom hanging, tuft;
	Builder b;
	int hair_node = -1, tuft_node = -1;
	Posable()
	{
		for (int s = 0; s < 30; s++) {
			std::vector<V3> p;
			for (int i = 0; i < 10; i++) {
				const float y = 1 - 0.1f * i;
				p.push_back({0.01f * s, y, 0.02f});
				const float along = (1 - y) / 0.3f;
				const int bone = std::min(1, (int)along);
				const float share = std::min(1.0f, along - (float)bone);
				hanging.held((uint16_t)bone, 1 - share, (uint16_t)(bone + 1), share);
			}
			hanging.strand(p);
		}
		hanging.shapes.assign(2, std::vector<V3>(hanging.pos.size()));
		for (size_t i = 0; i < hanging.pos.size(); i++) {
			const size_t s = i / 10, k = i % 10;
			if (s < 10)
				hanging.shapes[0][i] = {0.01f, 0.002f * (float)s, 0.03f};
			else if (s < 20)
				hanging.shapes[1][i] = {0, 0, 0.004f * (float)(k * k)};
		}
		for (int s = 0; s < 8; s++)
			tuft.strand({{0.02f * s, 0, 0}, {0.02f * s + 0.01f, 0.05f, 0}, {0.02f * s, 0.1f, 0.01f * s}});
		tuft.shapes.assign(2, std::vector<V3>(tuft.pos.size()));
		for (size_t i = 0; i < tuft.pos.size(); i++)
			tuft.shapes[0][i] = {0, 0, 0.01f * (float)(i % 3)};

		b.doc["meshes"].push_back({{"primitives", json::array({hanging.part(b)})}});
		b.doc["meshes"].push_back({{"primitives", json::array({tuft.part(b)})}});
		const int armature = b.node({{"name", "Armature"}});
		const Quat lean = axis_angle({0, 0, 1}, 0.5f), twist = axis_angle({1, 0, 0}, -0.4f);
		const int j0 = b.node({{"name", "j0"}, {"translation", {0, 1, 0}}});
		const int j1 = b.node({{"name", "j1"},
				       {"translation", {0, -0.3f, 0}},
				       {"rotation", {lean.x, lean.y, lean.z, lean.w}}});
		const int j2 = b.node({{"name", "j2"},
				       {"translation", {0, -0.3f, 0}},
				       {"rotation", {twist.x, twist.y, twist.z, twist.w}}});
		b.doc["nodes"][armature]["children"] = {j0};
		b.doc["nodes"][j0]["children"] = {j1};
		b.doc["nodes"][j1]["children"] = {j2};
		std::vector<float> ibm;
		for (int j = 0; j < 3; j++) {
			const M4 inv = inverse(moved({0, 1 - 0.3f * j, 0}));
			ibm.insert(ibm.end(), inv.m, inv.m + 16);
		}
		b.doc["skins"].push_back(
			{{"joints", {j0, j1, j2}}, {"inverseBindMatrices", b.floats(ibm, 16, "MAT4")}});
		hair_node = b.node({{"name", "Hair"}, {"mesh", 0}, {"skin", 0}});
		const Quat turn = axis_angle({0, 1, 0}, 0.8f);
		tuft_node = b.node({{"name", "Tuft"},
				    {"mesh", 1},
				    {"translation", {0.5f, 0.2f, -0.1f}},
				    {"rotation", {turn.x, turn.y, turn.z, turn.w}},
				    {"scale", {2, 2, 2}}});
	}
};

// The part of a rig that poses `node`'s mesh.
static const Rig::Part *posed_part(const Rig &rig, int node)
{
	for (const Rig::Part &p : rig.parts())
		if (p.node == node)
			return &p;
	return nullptr;
}

static void test_posed_here()
{
	Posable c;
	for (int simplified = 0; simplified < 2; simplified++) {
		auto m = std::make_shared<Model>(loaded(c.b, simplified ? LoadOptions() : options_with(0)));
		CHECK(m->meshes.size() == 2 && m->strands == 38);
		if (m->meshes.size() != 2)
			return;
		// (Every point kept; or as loaded: 8 of each hair's 10, the two on
		// the straight run its last bone holds alone left out.)
		CHECK(m->meshes[0].parts[0].pos.size() == (simplified ? 240u : 300u));
		CHECK(m->meshes[0].parts[0].hair.moves.size() == 2 && m->meshes[1].parts[0].hair.moves.size() == 2);
		// Nothing showing: every point where its bones put it.
		Drive d = still();
		d.place = moved({0.3f, 0, 0.1f}) * matrix_of(axis_angle({0, 1, 0}, 0.7f));
		{
			Rig rig(m, RigOptions());
			for (int f = 0; f < 5; f++)
				rig.pose(d);
			const Rig::Part *hair = posed_part(rig, c.hair_node), *tuft = posed_part(rig, c.tuft_node);
			CHECK(hair && tuft && !hair->gpu && !tuft->gpu && !hair->hidden && !hair->collapsed);
			if (!hair || !tuft)
				return;
			CHECK(hair->pos.size() == m->meshes[0].parts[0].pos.size() &&
			      hair->normal.size() == hair->pos.size());
			CHECK(tuft->pos.size() == 24 && hair->spot_moves.empty() && tuft->spot_moves.empty());
			CHECK(hair->tangent.empty() && !hair->morphed && hair->rest_pos.empty());
			CHECK(worst_posed(rig, *hair, {}) < 1e-5f && worst_posed(rig, *tuft, {}) < 1e-5f);
			// (Its bones do bend it: a point low on a hair is well off
			// where the model's place alone would put it.)
			const size_t tip = m->meshes[0].parts[0].hair.first[1] - 1;
			CHECK(length(hair->pos[tip] - point(d.place, m->meshes[0].parts[0].pos[tip])) > 0.05f);
			// How much bigger its bones make it: not at all - and the
			// tuft's node twice.
			CHECK(near(hair->scale, 1, 1e-5) && near(tuft->scale, 2, 1e-5));
			CHECK(near(tuft->scale, stretch_of(rig.world()[(size_t)c.tuft_node]), 1e-6));
		}
		// Expressions showing (the first half on, the second full): each
		// spot moved by both, each point by its spot's move, then its
		// bones.
		auto showing = std::make_shared<Model>(*m);
		showing->meshes[0].weights = {0.5f, 1.0f};
		showing->meshes[1].weights = {0.5f, 1.0f};
		{
			Rig rig(showing, RigOptions());
			for (int f = 0; f < 5; f++)
				rig.pose(d);
			const Rig::Part *hair = posed_part(rig, c.hair_node), *tuft = posed_part(rig, c.tuft_node);
			if (!hair || !tuft)
				return;
			for (const Rig::Part *part : {hair, tuft}) {
				const Strands &h = prim_of(*showing, *part).hair;
				CHECK(part->spot_moves.size() == h.anchors() && h.anchors() > 1);
				bool summed = part->spot_moves.size() == h.anchors();
				for (size_t a = 0; summed && a < h.anchors(); a++)
					summed =
						near3(part->spot_moves[a], h.moves[0][a] * 0.5f + h.moves[1][a], 1e-8f);
				CHECK(summed);
				CHECK(worst_posed(rig, *part, part->spot_moves) < 1e-5f);
				// (They do move it: by hand without them it's off.)
				CHECK(worst_posed(rig, *part, {}) > 0.004f);
				CHECK(!part->morphed && part->rest_pos.empty());
			}
			// A hair shifted whole lands half its file's shift from where
			// it hung (turned as its bone turns it): its root, held by
			// the first bone, which only the model's place turns.
			const Primitive &p = showing->meshes[0].parts[0];
			size_t root = 0;
			while (root < p.pos.size() && c.hanging.at(p.pos[root]) != 30)
				root++;
			CHECK(root < p.pos.size());
			if (root < p.pos.size())
				CHECK(near3(hair->pos[root],
					    point(d.place, c.hanging.pos[30] + c.hanging.shapes[0][30] * 0.5f), 1e-5f));
		}
		// Too faint to show (under a ten-thousandth): no spots moved.
		auto faint = std::make_shared<Model>(*m);
		faint->meshes[0].weights = {0.00005f, 0};
		{
			Rig rig(faint, RigOptions());
			rig.pose(d);
			const Rig::Part *hair = posed_part(rig, c.hair_node);
			CHECK(hair && hair->spot_moves.empty() && worst_posed(rig, *hair, {}) < 1e-5f);
		}
	}

	// A coat on a bone three times the size: its hairs' widths are three
	// times as big in the world, and the part says so.
	{
		Groom g = lawn(5, 2);
		for (size_t i = 0; i < g.pos.size(); i++)
			g.held(0, 1);
		Builder b = file_of({g}, {{0, 0, 0}});
		b.doc["nodes"][1]["scale"] = {3, 3, 3};
		auto m = std::make_shared<Model>(loaded(b));
		Rig rig(m, RigOptions());
		rig.pose(still());
		CHECK(rig.parts().size() == 1 && near(rig.parts()[0].scale, 3, 1e-5));
		CHECK(rig.parts().size() == 1 && worst_posed(rig, rig.parts()[0], {}) < 1e-5f);
		if (rig.parts().size() == 1 && rig.parts()[0].pos.size() == 20) {
			// (Three times as far from its bone as made.)
			const Primitive &p = m->meshes[0].parts[0];
			CHECK(near3(rig.parts()[0].pos[1], p.pos[1] * 3, 1e-5f));
		}
		// And in a world where the whole character is drawn half size.
		Drive d = still();
		d.place = scaled(0.5f);
		rig.pose(d);
		CHECK(near(rig.parts()[0].scale, 1.5, 1e-5));
	}
}

// --- The bone a part goes by ----------------------------------------------------

static void test_first_bone()
{
	// A coat held by its second bone alone - though its file lists the
	// first bone first, as holding it by nothing. It's as big as the bone
	// that does hold it makes it: three times, with that bone three times
	// the size; no bigger for the other being so.
	auto coat = [](float w0, float w1) {
		Groom g = lawn(5, 2);
		for (size_t i = 0; i < g.pos.size(); i++)
			g.held(0, w0, 1, w1);
		return g;
	};
	// (Its nodes: the armature, bone 0, bone 1, the hair.)
	auto posed = [](const Groom &g, int bone, json scale, bool card) {
		Builder b = file_of({g}, {{0, 0, 0}, {0, 0, 0}});
		b.doc["nodes"][(size_t)bone + 1]["scale"] = scale;
		auto m = std::make_shared<Model>(loaded(b));
		Rig rig(m, RigOptions());
		rig.set_gpu_skin(card);
		rig.pose(still());
		Rig::Part out;
		if (rig.parts().size() == 1 && rig.parts()[0].gpu == card)
			out = rig.parts()[0];
		else
			out.scale = out.handed = -100;
		return out;
	};
	const Groom second = coat(0, 1);
	{
		const Model m = loaded(second);
		const Primitive &p = part_of(m);
		CHECK(p.joints.size() == 80 && p.joints[0] == 0 && p.weights[0] == 0 && p.joints[1] == 1 &&
		      p.weights[1] == 1);
	}
	for (bool card : {false, true}) {
		CHECK(near(posed(second, 1, {3, 3, 3}, card).scale, 3, 1e-5));
		CHECK(near(posed(second, 0, {3, 3, 3}, card).scale, 1, 1e-5));
		// And faces the way that bone turns it: mirrored with it, not with
		// the other.
		CHECK(posed(second, 1, {-1, 1, 1}, card).handed == -1 && posed(second, 1, {1, 1, 1}, card).handed == 1);
		CHECK(posed(second, 0, {-1, 1, 1}, card).handed == 1);
		// Held by both: by the one that holds it most - the first listed,
		// when they hold it alike.
		CHECK(near(posed(coat(0.4f, 0.6f), 1, {3, 3, 3}, card).scale, 3, 1e-5));
		CHECK(near(posed(coat(0.6f, 0.4f), 1, {3, 3, 3}, card).scale, 1, 1e-5));
		CHECK(near(posed(coat(0.5f, 0.5f), 0, {3, 3, 3}, card).scale, 3, 1e-5));
		CHECK(near(posed(coat(0.5f, 0.5f), 1, {3, 3, 3}, card).scale, 1, 1e-5));
		// Listed last of its four, the three before it holding nothing.
		Groom last = lawn(5, 2);
		for (size_t i = 0; i < last.pos.size(); i++) {
			last.joints.insert(last.joints.end(), {0, 0, 0, 1});
			last.weights.insert(last.weights.end(), {0, 0, 0, 1});
		}
		CHECK(near(posed(last, 1, {3, 3, 3}, card).scale, 3, 1e-5));
		CHECK(posed(last, 1, {1, -1, 1}, card).handed == -1);
	}
	// (Posed here, its points go where that bone puts them: three times as
	// far out.)
	{
		Builder b = file_of({second}, {{0, 0, 0}, {0, 0, 0}});
		b.doc["nodes"][2]["scale"] = {3, 3, 3};
		auto m = std::make_shared<Model>(loaded(b));
		Rig rig(m, RigOptions());
		rig.pose(still());
		CHECK(rig.parts().size() == 1 && rig.parts()[0].pos.size() == 20 &&
		      worst_posed(rig, rig.parts()[0], {}) < 1e-5f);
		if (rig.parts().size() == 1 && rig.parts()[0].pos.size() == 20)
			CHECK(near3(rig.parts()[0].pos[1], part_of(*m).pos[1] * 3, 1e-5f));
	}
}

// --- Fewer strands posed here ---------------------------------------------------

static void test_fewer_posed()
{
	// Two coats on one node, no bones: 3000 hairs and 1000, each kept as
	// 2 points (its root and tip).
	const Groom big = lawn(60, 50), small = lawn(40, 25, 3, false, {100, 0, 0});
	Builder b = file_of({big, small});
	auto m = std::make_shared<Model>(loaded(b));
	CHECK(m->strand_points == 8000);
	Rig rig(m, RigOptions());
	auto sizes = [&rig](size_t most) {
		rig.set_strand_points(most);
		rig.pose(still());
		return rig.parts().size() == 2 ? std::make_pair(rig.parts()[0].pos.size(), rig.parts()[1].pos.size())
					       : std::make_pair((size_t)0, (size_t)0);
	};
	// As made (150000 points), and with room for all: all of them.
	rig.pose(still());
	CHECK(rig.parts().size() == 2 && rig.parts()[0].pos.size() == 6000 && rig.parts()[1].pos.size() == 2000);
	CHECK(sizes(8000) == std::make_pair((size_t)6000, (size_t)2000));
	CHECK(sizes(1000000) == std::make_pair((size_t)6000, (size_t)2000));
	// Fewer: each part its share, whole strands - the first of them, each
	// where all of them posed would have it.
	CHECK(sizes(2000) == std::make_pair((size_t)1500, (size_t)500));
	CHECK(worst_posed(rig, rig.parts()[0], {}) < 1e-6f && worst_posed(rig, rig.parts()[1], {}) < 1e-6f);
	CHECK(rig.parts()[0].normal.size() == 1500 && rig.parts()[1].normal.size() == 500);
	// (2001 for 8000: 1500.75 and 500.25 - whole strands, 1500 and 500.)
	CHECK(sizes(2001) == std::make_pair((size_t)1500, (size_t)500));
	CHECK(sizes(7999) == std::make_pair((size_t)5998, (size_t)1998));
	CHECK(sizes(4) == std::make_pair((size_t)32, (size_t)32));
	// Never fewer than 16 strands of each.
	CHECK(sizes(100) == std::make_pair((size_t)74, (size_t)32));
	CHECK(sizes(0) == std::make_pair((size_t)32, (size_t)32));
	CHECK(worst_posed(rig, rig.parts()[0], {}) < 1e-6f);
	// Measuring it goes by those too, and leaves them posed.
	const Bounds box = rig.rest_bounds();
	CHECK(box.found && box.low.x >= 0 && box.high.x <= 139 && box.high.x > 100 && box.high.z <= 49);
	rig.pose(still());
	CHECK(rig.parts()[0].pos.size() == 32);
	// More again: all of them again.
	CHECK(sizes(150000) == std::make_pair((size_t)6000, (size_t)2000));

	// Hairs of different lengths (3 to 7 points), with an expression
	// showing: what's posed ends where a strand does, within its share.
	Groom mixed;
	for (int i = 0; i < 600; i++) {
		std::vector<V3> s;
		for (int k = 0; k < 3 + i % 5; k++)
			s.push_back({(float)(i % 30) + (k % 2 ? 0.05f : 0.0f), 0.1f * (float)k, (float)(i / 30)});
		mixed.strand(s);
	}
	mixed.shapes.assign(1, std::vector<V3>(mixed.pos.size()));
	for (size_t i = 0; i < mixed.pos.size(); i++)
		mixed.shapes[0][i] = {0, 0.3f * mixed.pos[i].y, 0.01f * mixed.pos[i].x};
	Builder mb = file_of({mixed, lawn(3, 3, 3, false, {100, 0, 0})});
	auto mm = std::make_shared<Model>(loaded(mb));
	mm->meshes[0].weights = {1.0f};
	CHECK(mm->strand_points == 3000 + 18 && part_of(*mm, 0).hair.anchors() > 1000);
	Rig mixed_rig(mm, RigOptions());
	for (size_t most : {3018u, 3017u, 1800u, 1000u, 333u, 100u, 17u}) {
		mixed_rig.set_strand_points(most);
		mixed_rig.pose(still());
		CHECK(mixed_rig.parts().size() == 2);
		if (mixed_rig.parts().size() != 2)
			break;
		const Rig::Part &part = mixed_rig.parts()[0], &few = mixed_rig.parts()[1];
		const Strands &h = part_of(*mm, 0).hair;
		const size_t n = part.pos.size(), strands = h.strands_in(n);
		// A strand's end, its share of the points (or its first 16
		// strands, if that's more).
		CHECK(n == h.first[strands] && part.normal.size() == n);
		const size_t share = most >= 3018 ? 3000 : 3000 * most / 3018;
		CHECK(n <= std::max<size_t>(share, h.first[16]) && (strands == 600 || h.first[strands + 1] > share));
		CHECK(strands >= 16);
		// (A part of 9 hairs: all of them, always.)
		CHECK(few.pos.size() == 18);
		CHECK(part.spot_moves.size() == h.anchors() && worst_posed(mixed_rig, part, part.spot_moves) < 1e-5f);
	}
}

// --- Posed for the graphics card ------------------------------------------------

// A coat of 1000 hairs (2000 points kept) held by `bones` bones in a row,
// hair s by bone s % bones.
static Builder many_boned(int bones, Groom *made = nullptr)
{
	Groom g = lawn(40, 25);
	for (size_t i = 0; i < g.pos.size(); i++)
		g.held((uint16_t)(g.strand_of(i) % (size_t)bones), 1);
	std::vector<V3> at;
	for (int j = 0; j < bones; j++)
		at.push_back({0.01f * j, 0, 0});
	if (made)
		*made = g;
	return file_of({g}, at);
}

static void test_for_the_card()
{
	Posable c;
	auto m = std::make_shared<Model>(loaded(c.b));
	m->meshes[0].weights = {0.5f, 1.0f};
	m->meshes[1].weights = {0.5f, 1.0f};
	Rig cpu(m, RigOptions()), card(m, RigOptions());
	card.set_gpu_skin(true);
	CHECK(card.gpu_skin() && !cpu.gpu_skin());
	Drive d = still();
	d.place = moved({0.3f, 0, 0.1f}) * matrix_of(axis_angle({0, 1, 0}, 0.7f));
	// Measured before it's ever posed, and after: every point worked out
	// here, the same box either way.
	const Bounds before = card.rest_bounds(), cpu_box = cpu.rest_bounds();
	CHECK(before.found && cpu_box.found && near3(before.low, cpu_box.low, 1e-6f) &&
	      near3(before.high, cpu_box.high, 1e-6f));
	// (The box holds the hair: down to the chain's bent end, out to the tuft.)
	CHECK(before.high.y > 0.99f && before.low.y < 0.45f && before.high.x > 0.5f);
	for (int f = 0; f < 5; f++) {
		cpu.pose(d);
		card.pose(d);
	}
	for (int node : {c.hair_node, c.tuft_node}) {
		const Rig::Part *g = posed_part(card, node), *r = posed_part(cpu, node);
		CHECK(g && r);
		if (!g || !r)
			return;
		const Primitive &p = prim_of(*m, *g);
		// Posed on the card: no points here; its bones (a part with none:
		// its node's place, as bone 0), its spots' moves, how much bigger
		// its bones make it, and its middle.
		CHECK(g->gpu && g->pos.empty() && g->normal.empty() && g->tangent.empty() && g->bones == node);
		CHECK(!g->morphed && g->rest_pos.empty() && g->rest_normal.empty() && !g->hidden && !g->collapsed);
		CHECK(card.bones(g->bones).size() == (node == c.hair_node ? 3u : 1u));
		CHECK(g->spot_moves.size() == p.hair.anchors() && r->spot_moves.size() == g->spot_moves.size());
		bool same = g->spot_moves.size() == r->spot_moves.size();
		for (size_t a = 0; same && a < g->spot_moves.size(); a++)
			same = near3(g->spot_moves[a], r->spot_moves[a], 0);
		CHECK(same && near(g->scale, r->scale, 1e-6) && near(g->scale, node == c.hair_node ? 1 : 2, 1e-5));
		// The card's sums (each point as made, moved by its spot's move,
		// then by its bones) land every point where the CPU puts it.
		const std::vector<M4> &set = card.bones(g->bones);
		float worst = r->pos.size() == p.pos.size() ? 0.0f : 1e30f;
		V3 low{1e30f, 1e30f, 1e30f}, high{-1e30f, -1e30f, -1e30f};
		for (size_t i = 0; i < r->pos.size() && i < p.pos.size(); i++) {
			const V3 at = p.pos[i] + g->spot_moves[p.hair.anchor[i]];
			V3 sum;
			for (int k = 0; k < 4; k++) {
				const float w = p.joints.empty() ? (k == 0 ? 1.0f : 0.0f) : p.weights[i * 4 + k];
				const size_t j = p.joints.empty() ? 0 : p.joints[i * 4 + k];
				if (w > 0 && j < set.size())
					sum = sum + point(set[j], at) * w;
			}
			worst = std::max(worst, length(sum - r->pos[i]));
			low = {std::min(low.x, sum.x), std::min(low.y, sum.y), std::min(low.z, sum.z)};
			high = {std::max(high.x, sum.x), std::max(high.y, sum.y), std::max(high.z, sum.z)};
		}
		CHECK(worst < 1e-5f);
		// (Its middle: among its points.)
		CHECK(g->middle.x >= low.x && g->middle.x <= high.x && g->middle.y >= low.y && g->middle.y <= high.y &&
		      g->middle.z >= low.z && g->middle.z <= high.z);
		CHECK(near3(g->middle, r->middle, 1e-5f) || r->middle.x == 0);
	}
	// Measured between frames: every point worked out here again (the
	// same box as the rig that poses here finds), and its parts left as
	// posed for the card.
	const Bounds after = card.rest_bounds(), cpu_after = cpu.rest_bounds();
	CHECK(after.found && cpu_after.found && near3(after.low, cpu_after.low, 1e-5f) &&
	      near3(after.high, cpu_after.high, 1e-5f));
	CHECK(posed_part(card, c.hair_node)->gpu && posed_part(card, c.hair_node)->pos.empty() &&
	      !posed_part(card, c.hair_node)->spot_moves.empty());
	// The most points posed here is nothing to a part posed on the card.
	card.set_strand_points(10);
	card.pose(d);
	CHECK(posed_part(card, c.hair_node)->gpu && posed_part(card, c.hair_node)->pos.empty());
	CHECK(card.rest_bounds().found);
	// Off again: posed here again, every point.
	card.set_strand_points(150000);
	card.set_gpu_skin(false);
	card.pose(d);
	CHECK(!posed_part(card, c.hair_node)->gpu && posed_part(card, c.hair_node)->pos.size() == 240);

	// More bones than the card's list holds (255): a part with 256 or
	// more is posed here even so - capped as any posed here is - and one
	// with 255 on the card.
	CHECK(Rig::GPU_MOST_BONES == 255);
	const int bones[] = {255, 256, 300};
	for (int count : bones) {
		Groom g;
		Builder b = many_boned(count, &g);
		auto big = std::make_shared<Model>(loaded(b));
		CHECK(big->skins.size() == 1 && big->skins[0].joints.size() == (size_t)count &&
		      big->strand_points == 2000);
		Rig rig(big, RigOptions());
		rig.set_gpu_skin(true);
		rig.pose(still());
		CHECK(rig.parts().size() == 1);
		if (rig.parts().size() != 1)
			continue;
		const Rig::Part &part = rig.parts()[0];
		if (count <= 255) {
			CHECK(part.gpu && part.pos.empty() && rig.bones(part.bones).size() == (size_t)count);
			continue;
		}
		CHECK(!part.gpu && part.pos.size() == 2000 && part.normal.size() == 2000);
		CHECK(worst_posed(rig, part, {}) < 1e-5f);
		// (Its highest bones do hold points.)
		const Primitive &p = part_of(*big);
		CHECK(*std::max_element(p.joints.begin(), p.joints.end()) == count - 1);
		rig.set_strand_points(500);
		rig.pose(still());
		CHECK(!rig.parts()[0].gpu && rig.parts()[0].pos.size() == 500 &&
		      worst_posed(rig, rig.parts()[0], {}) < 1e-5f);
		rig.set_strand_points(1);
		rig.pose(still());
		CHECK(rig.parts()[0].pos.size() == 32);
		CHECK(rig.rest_bounds().found);
	}

	// Two coats, one the card can pose (3 bones, 6000 points) and one it
	// can't (300 bones, 2000 points), and 1000 points to pose here. All
	// posed here, they share them by size; with the card posing the
	// first, the second has them all.
	{
		Groom wide = lawn(60, 50, 3, false, {0, 0, 100});
		for (size_t i = 0; i < wide.pos.size(); i++)
			wide.held((uint16_t)(wide.strand_of(i) % 3), 1);
		Groom g;
		Builder b = many_boned(300, &g);
		b.doc["meshes"].push_back({{"primitives", json::array({wide.part(b)})}});
		const int a0 = b.node({{"name", "a0"}}), a1 = b.node({{"name", "a1"}}), a2 = b.node({{"name", "a2"}});
		b.doc["skins"].push_back({{"joints", {a0, a1, a2}}});
		const int wide_node = b.node({{"name", "Wide"}, {"mesh", 1}, {"skin", 1}});
		auto both = std::make_shared<Model>(loaded(b));
		CHECK(both->strand_points == 8000 && both->skins.size() == 2);
		Rig rig(both, RigOptions());
		rig.set_strand_points(1000);
		rig.pose(still());
		CHECK(rig.parts().size() == 2);
		if (rig.parts().size() != 2)
			return;
		const size_t few = rig.parts()[0].node == wide_node ? 0 : 1, many = 1 - few;
		CHECK(!rig.parts()[many].gpu && !rig.parts()[few].gpu);
		CHECK(rig.parts()[many].pos.size() == 250 && rig.parts()[few].pos.size() == 750);
		rig.set_gpu_skin(true);
		rig.pose(still());
		CHECK(!rig.parts()[many].gpu && rig.parts()[few].gpu);
		CHECK(rig.parts()[many].pos.size() == 1000 && rig.parts()[few].pos.empty());
		// (Measuring poses both here again, sharing them again - then
		// both are as they were.)
		const Bounds box = rig.rest_bounds();
		CHECK(box.found && box.high.z >= 100 && box.high.x > 30);
		CHECK(!rig.parts()[many].gpu && rig.parts()[few].gpu);
		CHECK(rig.parts()[many].pos.size() == 1000 && rig.parts()[few].pos.empty());
	}
}

// A part posed on the graphics card has only its middle worked out here,
// from some of its points: every 17th - but of a big coat never more than
// a few thousand (two million hair points read every frame took the CPU
// milliseconds), and never by a step that keeps landing on the same point
// of every hair.
static void test_a_big_coats_middle()
{
	// Up to about 70,000 points: every 17th, as it always was.
	for (size_t n : {(size_t)0, (size_t)1, (size_t)16, (size_t)17, (size_t)2000, (size_t)69632})
		CHECK(middle_step(n) == 17);
	// More: no more than 4,096 of them read (and not far fewer), by a step
	// that 2, 3, 5, 7, 11 and 13 don't divide.
	size_t last = 17;
	for (size_t n : {(size_t)69633, (size_t)73728, (size_t)98304, (size_t)250000, (size_t)715342, (size_t)1999771,
			 (size_t)2000000, (size_t)16000000}) {
		const size_t step = middle_step(n);
		CHECK(step > 17 && step >= last && n / step <= 4096 && n / step >= 1800);
		for (size_t d : {2, 3, 5, 7, 11, 13})
			CHECK(step % d != 0);
		last = step;
	}
	CHECK(middle_step(69633) == 19 && middle_step(98304) == 29 && middle_step(2000000) == 491);

	// A coat of 12,288 hairs of 8 points each (kinked: every point shapes
	// its hair, so every one is kept): 98,304 points, 24 times 4,096 -
	// read every 24th (3 hairs on), each would be a root; by its step of
	// 29 they're from all along the hairs, and the middle is the coat's.
	const Groom g = lawn(128, 96, 8, true);
	auto m = std::make_shared<Model>(loaded(g));
	CHECK(m->meshes.size() == 1 && m->meshes[0].parts.size() == 1);
	if (m->meshes.size() != 1 || m->meshes[0].parts.size() != 1)
		return;
	const Primitive &p = m->meshes[0].parts[0];
	CHECK(p.strands && p.hair.strands() == 12288 && p.pos.size() == 98304);
	bool eights = true;
	for (size_t k = 0; k < p.hair.strands(); k++)
		eights = eights && p.hair.first[k + 1] - p.hair.first[k] == 8;
	CHECK(eights);
	Rig cpu(m, RigOptions()), card(m, RigOptions());
	card.set_gpu_skin(true);
	cpu.set_strand_points(1000000);
	Drive d = still();
	d.place = moved({3.0f, 0.5f, -2.0f}) * matrix_of(axis_angle({0, 1, 0}, 0.7f));
	cpu.pose(d);
	card.pose(d);
	CHECK(cpu.parts().size() == 1 && card.parts().size() == 1);
	if (cpu.parts().size() != 1 || card.parts().size() != 1)
		return;
	const Rig::Part &r = cpu.parts()[0], &c = card.parts()[0];
	CHECK(c.gpu && c.pos.empty() && !r.gpu && r.pos.size() == 98304);
	double sum[3] = {0, 0, 0};
	V3 roots{};
	for (size_t i = 0; i < r.pos.size(); i++) {
		sum[0] += r.pos[i].x, sum[1] += r.pos[i].y, sum[2] += r.pos[i].z;
		if (i % 8 == 0)
			roots = roots + r.pos[i] * (8.0f / (float)r.pos.size());
	}
	const V3 all{(float)(sum[0] / r.pos.size()), (float)(sum[1] / r.pos.size()), (float)(sum[2] / r.pos.size())};
	// (The hairs stand 0.7 tall: all of them 0.35 above their roots.)
	CHECK(near(all.y - roots.y, 0.35, 1e-3));
	// Its middle: the coat's own, to a hundredth of its width (128) - and
	// as far above its roots.
	CHECK(near3(c.middle, all, 1.2f));
	CHECK(near(c.middle.y, all.y, 0.02));
	// (Worked out from just those points: every 29th, 3,390 of them.)
	V3 some{};
	size_t read = 0;
	for (size_t i = 0; i < r.pos.size(); i += middle_step(r.pos.size()), read++)
		some = some + r.pos[i];
	CHECK(read == 3390 && near3(c.middle, some * (1.0f / (float)read), 2e-3f));
	std::printf("unit_hair: a coat of %zu points, every %zuth read: its middle %.3f from the coat's own (%.3f up)\n",
		    p.pos.size(), middle_step(p.pos.size()), length(c.middle - all), c.middle.y - all.y);
}

int main()
{
	test_detail_names();
	test_plans();
	test_share_on_screen();
	test_budget();
	test_budget_settles();
	test_lines_and_strips();
	test_hairs_of_two_points();
	test_wild_numbers();
	test_what_a_point_keeps();
	test_points_that_shape();
	test_bones_that_swing();
	test_turns();
	test_spread();
	test_points_allowed();
	test_cover();
	test_rates_and_keys();
	test_thresholds();
	test_share_of_a_rated_coat();
	test_parts_share_alike();
	test_share_by_sample();
	test_groups();
	test_layers_and_pieces();
	test_expressions();
	test_expressions_by_strand();
	test_stand_in_hair();
	test_posed_here();
	test_first_bone();
	test_fewer_posed();
	test_for_the_card();
	test_a_big_coats_middle();
	std::printf("unit_hair: %d passed, %d failed\n", passes, failures);
	return failures ? 1 : 0;
}
