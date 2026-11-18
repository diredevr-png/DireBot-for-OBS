// Unit tests for things 3D made ready for the graphics card
// (src/face/prop-mesh.hpp: a headpat's hand, things thrown, their pieces),
// on models made here: what's kept of a file (its points where its nodes
// and bones put them, its first two shapes with their points, its
// triangles' corners in two bytes where they fit) and how little it takes;
// and that the card's sums - a thing put by one matrix a draw, its shapes
// added there (the effect's Thing techniques, written out again here) -
// come to what 0.12.0's first build worked out point by point on the CPU,
// for a thing turned, sized, squashed, mirrored and shaped.
// Build and run: tools/test-link.sh (Linux, any C++17 compiler).

#include "../src/face/prop-mesh.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace direbot::face;

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

static bool near3(V3 a, V3 b, float by)
{
	return length(a - b) <= by;
}

// The same numbers every run.
struct Dice {
	uint32_t at = 12345;
	float roll()
	{
		at = at * 1664525u + 1013904223u;
		return (float)(at >> 8) / (float)(1u << 24);
	}
	float between(float a, float b) { return a + (b - a) * roll(); }
	V3 point(float size) { return {between(-size, size), between(-size, size), between(-size, size)}; }
};

// A part of `n` points in a ring of triangles, with facings, UVs, colours
// and (`tangents`) tangents; `shapes`: how many shapes, each moving every
// third point or so (and turning its facing, but for the last).
static Primitive made_part(Dice &d, size_t n, bool tangents, size_t shapes, int material)
{
	Primitive p;
	p.material = material;
	for (size_t k = 0; k < n; k++) {
		p.pos.push_back(d.point(0.5f));
		p.normal.push_back(normalized(d.point(1), {0, 0, 1}));
		p.uv.push_back(d.roll());
		p.uv.push_back(d.roll());
		for (int c = 0; c < 4; c++)
			p.colour.push_back(d.roll());
		if (tangents) {
			const V3 t = normalized(d.point(1), {1, 0, 0});
			p.tangent.insert(p.tangent.end(), {t.x, t.y, t.z, k % 2 ? 1.0f : -1.0f});
		}
	}
	for (size_t k = 0; k + 2 < n; k++)
		p.index.insert(p.index.end(), {(uint32_t)k, (uint32_t)k + 1, (uint32_t)k + 2});
	for (size_t s = 0; s < shapes; s++) {
		MorphTarget t;
		for (size_t k = s % 3; k < n; k += 3) {
			t.at.push_back((uint32_t)k);
			t.pos.push_back(d.point(0.2f));
			if (s + 1 < shapes || shapes == 1)
				t.normal.push_back(d.point(0.5f));
		}
		p.morphs.push_back(t);
	}
	return p;
}

// A thing of three parts: one on a node moved, turned and sized unevenly
// (three shapes, tangents), one bound to two bones (two shapes), one plain.
static Model made_thing()
{
	Dice d;
	Model m;
	Material red, blue;
	red.name = "red";
	blue.name = "blue skin";
	m.materials = {red, blue};

	Node root;
	root.name = "root";
	root.children = {1, 2, 3, 4};
	Node a;
	a.name = "a";
	a.parent = 0;
	a.t = {0.3f, -0.2f, 0.1f};
	a.r = axis_angle({0.3f, 1, 0.2f}, 0.9f);
	a.s = {1.5f, 0.7f, 1.1f};
	a.mesh = 0;
	Node b;
	b.name = "b";
	b.parent = 0;
	b.mesh = 1;
	b.skin = 0;
	Node bone1;
	bone1.name = "bone1";
	bone1.parent = 0;
	bone1.t = {0, 0.4f, 0};
	bone1.r = axis_angle({1, 0, 0}, 0.5f);
	Node bone2;
	bone2.name = "bone2";
	bone2.parent = 0;
	bone2.t = {0.2f, 0, -0.3f};
	bone2.r = axis_angle({0, 0, 1}, -0.7f);
	Node c;
	c.name = "c";
	c.parent = 0;
	c.t = {-0.4f, 0, 0};
	c.mesh = 2;
	root.children.push_back(5);
	m.nodes = {root, a, b, bone1, bone2, c};
	m.roots = {0};

	Mesh ma;
	ma.parts.push_back(made_part(d, 40, true, 3, 0));
	ma.morph_names = {"wobble", "wobble back", "third"};
	Mesh mb;
	Primitive bound = made_part(d, 31, false, 2, 1);
	for (size_t k = 0; k < bound.pos.size(); k++) {
		// (Its strongest bone: the second, for every other point.)
		const float w = k % 2 ? 0.3f : 0.8f;
		bound.joints.insert(bound.joints.end(), {0, 1, 0, 0});
		bound.weights.insert(bound.weights.end(), {w, 1 - w, 0, 0});
	}
	mb.parts.push_back(bound);
	mb.morph_names = {"wobble", "wobble back"};
	Mesh mc;
	mc.parts.push_back(made_part(d, 12, false, 0, -1));
	m.meshes = {ma, mb, mc};

	Skin skin;
	skin.joints = {3, 4};
	skin.inverse_bind = {matrix_of({0.1f, 0, 0}, axis_angle({0, 1, 0}, 0.2f), {1, 1, 1}),
			     matrix_of({0, -0.1f, 0.05f}, axis_angle({1, 0, 0}, -0.3f), {1, 1, 1})};
	m.skins = {skin};
	return m;
}

// --- 0.12.0's first build: every point worked out on the CPU ----------------
// (Prop3D's constructor and Prop3D::place as they were, on the file as
// read: what the card's sums must come to.)
struct Placed {
	std::vector<V3> pos, normal, tangent; // in the world
	std::vector<float> tangent_up;
};
static std::vector<Placed> placed_as_before(const Model &m, const M4 &to_world, const std::vector<float> &shapes)
{
	std::vector<Placed> out;
	const std::vector<M4> world = rest_world(m);
	const V3 ax = direction(to_world, {1, 0, 0}), ay = direction(to_world, {0, 1, 0}),
		 az = direction(to_world, {0, 0, 1});
	const float handed = dot(cross(ax, ay), az) < 0 ? -1.0f : 1.0f;
	const M4 normals = inverse(to_world);
	auto normal_of = [&normals](V3 n) {
		return normalized(V3{normals.m[0] * n.x + normals.m[1] * n.y + normals.m[2] * n.z,
				     normals.m[4] * n.x + normals.m[5] * n.y + normals.m[6] * n.z,
				     normals.m[8] * n.x + normals.m[9] * n.y + normals.m[10] * n.z},
				  {0, 0, 1});
	};
	for (size_t ni = 0; ni < m.nodes.size(); ni++) {
		const Node &node = m.nodes[ni];
		if (node.mesh < 0 || node.mesh >= (int)m.meshes.size())
			continue;
		const Skin *skin = node.skin >= 0 && node.skin < (int)m.skins.size() ? &m.skins[node.skin] : nullptr;
		for (const Primitive &p : m.meshes[node.mesh].parts) {
			const size_t n = p.pos.size();
			if (!n || p.index.empty())
				continue;
			const bool weighted = skin && p.joints.size() == n * 4 && p.weights.size() == n * 4;
			auto at = [&](size_t k) -> M4 {
				if (weighted) {
					int best = 0;
					for (int j = 1; j < 4; j++)
						if (p.weights[k * 4 + j] > p.weights[k * 4 + best])
							best = j;
					const uint16_t joint = p.joints[k * 4 + best];
					if (joint < skin->joints.size() && joint < skin->inverse_bind.size()) {
						const int bone = skin->joints[joint];
						if (bone >= 0 && bone < (int)world.size())
							return world[bone] * skin->inverse_bind[joint];
					}
				}
				return world[ni];
			};
			const bool bumped = p.tangent.size() == n * 4;
			std::vector<V3> pos(n), normal(n), tangent(bumped ? n : 0);
			for (size_t k = 0; k < n; k++) {
				const M4 w = at(k);
				pos[k] = point(w, p.pos[k]);
				normal[k] = normalized(direction(w, p.normal[k]), {0, 0, 1});
				if (bumped)
					tangent[k] = normalized(direction(w, {p.tangent[k * 4], p.tangent[k * 4 + 1],
									      p.tangent[k * 4 + 2]}),
								{1, 0, 0});
			}
			std::vector<std::vector<V3>> morph_pos, morph_normal;
			for (const MorphTarget &t : p.morphs) {
				std::vector<V3> dp(n), dn;
				for (size_t i = 0; i < t.at.size(); i++)
					if (t.at[i] < n)
						dp[t.at[i]] = direction(at(t.at[i]), t.pos[i]);
				if (!t.normal.empty()) {
					dn.resize(n);
					for (size_t i = 0; i < t.normal.size(); i++)
						if (t.at[i] < n)
							dn[t.at[i]] = direction(at(t.at[i]), t.normal[i]);
				}
				morph_pos.push_back(dp);
				morph_normal.push_back(dn);
			}
			Placed part;
			for (size_t k = 0; k < n; k++) {
				V3 q = pos[k], f = normal[k];
				for (size_t s = 0; s < morph_pos.size() && s < shapes.size(); s++) {
					const float w = shapes[s];
					if (w <= 0)
						continue;
					q = q + morph_pos[s][k] * w;
					if (!morph_normal[s].empty())
						f = f + morph_normal[s][k] * w;
				}
				part.pos.push_back(point(to_world, q));
				part.normal.push_back(normal_of(f));
				if (bumped) {
					part.tangent.push_back(normalized(direction(to_world, tangent[k]), {1, 0, 0}));
					part.tangent_up.push_back(p.tangent[k * 4 + 3] * handed);
				}
			}
			out.push_back(part);
		}
	}
	return out;
}

// --- The card's sums (the effect's thing_out, thing_bumped_out, shaped_pos
// and shaped_norm, word for word), from a made part's numbers ---------------
static V3 unit(V3 v, V3 otherwise)
{
	const float l = length(v);
	return l > 0.000000000001f ? v * (1 / l) : otherwise;
}
struct OnCard {
	V3 vpos, vnrm, vtan; // in the camera's view
	float tangent_up = 1;
};
static OnCard on_card(const PropMesh::Part &part, size_t k, const M4 &cam, const M4 &turn, float shape0, float shape1,
		      float handed)
{
	V3 pos{part.pos[k * 4], part.pos[k * 4 + 1], part.pos[k * 4 + 2]};
	V3 norm{part.normal[k * 4], part.normal[k * 4 + 1], part.normal[k * 4 + 2]};
	if (part.shaped && (shape0 > 0 || shape1 > 0)) {
		const float *s = &part.shapes[k * PROP_SHAPE_NUMBERS];
		const float *s0 = s, *s1 = s + 4, *s2 = s + 8;
		pos = pos + V3{s0[0], s0[1], s0[2]} * shape0 + V3{s1[2], s1[3], s2[0]} * shape1;
		norm = norm + V3{s0[3], s1[0], s1[1]} * shape0 + V3{s2[1], s2[2], s2[3]} * shape1;
	}
	OnCard o;
	o.vpos = point(cam, pos);
	o.vnrm = unit(direction(turn, norm), {0, 0, 1});
	if (part.bumped) {
		o.vtan = unit(direction(cam, {part.tangent[k * 4], part.tangent[k * 4 + 1], part.tangent[k * 4 + 2]}),
			      {1, 0, 0});
		o.tangent_up = part.tangent[k * 4 + 3] * handed;
	}
	return o;
}

static void test_what_is_kept_of_a_file()
{
	const Model m = made_thing();
	const PropMesh mesh = make_prop_mesh(m);
	CHECK(mesh.parts.size() == 3);
	CHECK(mesh.points == 40 + 31 + 12);
	CHECK(mesh.triangles == 38 + 29 + 10);
	CHECK(mesh.materials.size() == 2 && mesh.materials[1].name == "blue skin");
	// Its first two shapes, by name; a third isn't kept (nor found by name).
	CHECK(mesh.shapes == PROP_SHAPES && mesh.shapes_in_file == 3);
	CHECK(mesh.shape_names.size() == 2 && mesh.shape_names[0] == "wobble" && mesh.shape_names[1] == "wobble back");
	if (mesh.parts.size() != 3)
		return;
	const PropMesh::Part &a = mesh.parts[0], &b = mesh.parts[1], &c = mesh.parts[2];
	CHECK(a.material == 0 && b.material == 1 && c.material == -1);
	CHECK(a.bumped && !b.bumped && !c.bumped);
	CHECK(a.shaped && b.shaped && !c.shaped);
	// Four numbers a point (two for its UVs, twelve for its shapes), four
	// bytes of colour, and its corners in two bytes each.
	CHECK(a.pos.size() == 160 && a.normal.size() == 160 && a.tangent.size() == 160 && a.uv.size() == 80 &&
	      a.colour.size() == 160 && a.shapes.size() == 40 * PROP_SHAPE_NUMBERS);
	CHECK(c.tangent.empty() && c.shapes.empty());
	CHECK(a.index16.size() == 38 * 3 && a.index32.empty() && a.corners() == 38 * 3);
	CHECK(a.index16[3] == 1 && a.index16[4] == 2 && a.index16[5] == 3);
	CHECK(a.bytes() == 40 * (16 + 16 + 16 + 8 + 4 + 48) + 38 * 3 * 2);
	CHECK(c.bytes() == 12 * (16 + 16 + 8 + 4) + 10 * 3 * 2);
	CHECK(mesh.bytes() == a.bytes() + b.bytes() + c.bytes());
	// A point's UV and colour ride as the file has them (its colour as
	// sRGB bytes).
	const Primitive &pa = m.meshes[0].parts[0];
	CHECK(a.uv[7 * 2] == pa.uv[7 * 2] && a.uv[7 * 2 + 1] == pa.uv[7 * 2 + 1]);
	uint8_t want[4];
	srgb_bytes(&pa.colour[7 * 4], want);
	CHECK(a.colour[7 * 4] == want[0] && a.colour[7 * 4 + 1] == want[1] && a.colour[7 * 4 + 2] == want[2] &&
	      a.colour[7 * 4 + 3] == want[3]);
	// A tangent's which-way-up rides with it.
	CHECK(a.tangent[3] == -1 && a.tangent[7] == 1);
	// The box round it and each part's middle: its points' own.
	V3 low = {1e9f, 1e9f, 1e9f}, high = {-1e9f, -1e9f, -1e9f};
	for (const PropMesh::Part &part : mesh.parts) {
		V3 sum;
		for (size_t k = 0; k < part.points; k++) {
			const V3 q{part.pos[k * 4], part.pos[k * 4 + 1], part.pos[k * 4 + 2]};
			sum = sum + q;
			low = {std::min(low.x, q.x), std::min(low.y, q.y), std::min(low.z, q.z)};
			high = {std::max(high.x, q.x), std::max(high.y, q.y), std::max(high.z, q.z)};
		}
		CHECK(near3(part.middle, sum * (1.0f / (float)part.points), 1e-5f));
	}
	CHECK(near3(mesh.low, low, 0) && near3(mesh.high, high, 0));

	// A part of more than 65,536 points: its corners in four bytes each.
	Dice d;
	Model big;
	Node n;
	n.mesh = 0;
	big.nodes = {n};
	big.roots = {0};
	Mesh bm;
	bm.parts.push_back(made_part(d, 65537, false, 0, -1));
	bm.parts.push_back(made_part(d, 65536, false, 0, -1));
	big.meshes = {bm};
	const PropMesh big_mesh = make_prop_mesh(big);
	CHECK(big_mesh.parts.size() == 2);
	if (big_mesh.parts.size() == 2) {
		CHECK(big_mesh.parts[0].index16.empty() && big_mesh.parts[0].index32.size() == 65535 * 3);
		CHECK(big_mesh.parts[0].index32.back() == 65536);
		CHECK(big_mesh.parts[1].index32.empty() && big_mesh.parts[1].index16.size() == 65534 * 3);
		CHECK(big_mesh.parts[1].index16.back() == 65535);
	}
	CHECK(big_mesh.shapes == 0 && big_mesh.shape_names.empty());

	// A part with no points or no triangles, and hair strands: left out.
	Model odd = made_thing();
	odd.meshes[2].parts[0].index.clear();
	odd.meshes[1].parts[0].strands = true;
	const PropMesh odd_mesh = make_prop_mesh(odd);
	CHECK(odd_mesh.parts.size() == 1 && odd_mesh.points == 40);

	// A shape that moves nothing: no numbers carried for it.
	Model still = made_thing();
	for (Mesh &mesh_ : still.meshes)
		for (Primitive &p : mesh_.parts)
			for (MorphTarget &t : p.morphs) {
				t.pos.assign(t.pos.size(), V3{});
				t.normal.assign(t.normal.size(), V3{});
			}
	const PropMesh still_mesh = make_prop_mesh(still);
	CHECK(still_mesh.parts.size() == 3 && !still_mesh.parts[0].shaped && still_mesh.parts[0].shapes.empty());
	// (Its shapes are still there to ask for by name.)
	CHECK(still_mesh.shapes == 2);
}

static void test_the_cards_sums_are_the_cpus()
{
	const Model m = made_thing();
	const PropMesh mesh = make_prop_mesh(m);
	// A camera looking at it from a little up and to the side.
	const M4 view = moved({0.1f, -0.2f, -3}) * matrix_of(axis_angle({1, 0.3f, 0}, 0.25f));
	struct Case {
		const char *name;
		M4 place;
		std::vector<float> shapes;
	};
	M4 squash;
	squash.m[0] = 1.4f, squash.m[5] = 0.5f, squash.m[10] = 1.2f;
	M4 mirror;
	mirror.m[0] = -1;
	const M4 turned = moved({0.5f, 0.2f, -0.4f}) * matrix_of(axis_angle({0.2f, 1, 0.4f}, 1.1f)) * scaled(0.37f);
	const std::vector<Case> cases = {
		{"as made", M4(), {}},
		{"turned and sized", turned, {}},
		{"squashed", turned * squash, {}},
		{"mirrored", turned * mirror, {}},
		{"mirrored and squashed",
		 moved({-0.3f, 0, 0}) * mirror * squash * matrix_of(axis_angle({1, 0, 0}, 2)),
		 {}},
		{"its first shape", turned, {0.6f}},
		{"its second shape", turned * squash, {0, 0.8f}},
		{"both shapes", turned * mirror, {0.35f, 1}},
		// (A third was never kept: as if it weren't asked for.)
	};
	for (const Case &c : cases) {
		const std::vector<Placed> before = placed_as_before(m, c.place, c.shapes);
		CHECK(before.size() == mesh.parts.size());
		if (before.size() != mesh.parts.size())
			continue;
		// What the card is given: `cam` with its place in it, its
		// facings' turn, its shapes, which way round it is.
		const M4 cam = view * c.place;
		const M4 turn = prop_turn(view, c.place);
		const float handed = prop_handed(c.place);
		const float s0 = c.shapes.size() > 0 ? c.shapes[0] : 0, s1 = c.shapes.size() > 1 ? c.shapes[1] : 0;
		float worst_pos = 0, worst_normal = 0, worst_tangent = 0;
		bool ups = true;
		for (size_t i = 0; i < mesh.parts.size(); i++) {
			const PropMesh::Part &part = mesh.parts[i];
			CHECK(before[i].pos.size() == part.points);
			for (size_t k = 0; k < part.points && k < before[i].pos.size(); k++) {
				const OnCard o = on_card(part, k, cam, turn, s0, s1, handed);
				worst_pos = std::max(worst_pos, length(o.vpos - point(view, before[i].pos[k])));
				worst_normal =
					std::max(worst_normal, length(o.vnrm - direction(view, before[i].normal[k])));
				if (part.bumped) {
					worst_tangent = std::max(
						worst_tangent, length(o.vtan - direction(view, before[i].tangent[k])));
					ups = ups && o.tangent_up == before[i].tangent_up[k];
				}
			}
		}
		if (worst_pos > 2e-5f || worst_normal > 2e-5f || worst_tangent > 2e-5f || !ups)
			std::printf("  %s: off by %g (points), %g (facings), %g (tangents)\n", c.name, worst_pos,
				    worst_normal, worst_tangent);
		CHECK(worst_pos <= 2e-5f);
		CHECK(worst_normal <= 2e-5f);
		CHECK(worst_tangent <= 2e-5f);
		CHECK(ups);
	}
	// Which way round: mirrored once is, twice isn't.
	CHECK(prop_handed(M4()) == 1 && prop_handed(turned) == 1);
	CHECK(prop_handed(mirror) == -1 && prop_handed(turned * mirror) == -1);
	CHECK(prop_handed(mirror * turned * mirror) == 1);
	// A plain turn's facings turn as it does.
	const M4 spin = matrix_of(axis_angle({0, 1, 0}, 0.8f));
	CHECK(near3(direction(prop_turn(M4(), spin), {1, 0, 0}), direction(spin, {1, 0, 0}), 1e-6f));
	// Squashed flat along y, a facing leaning up leans up more.
	const V3 lean = unit(direction(prop_turn(M4(), squash), normalized(V3{1, 1, 0})), {0, 0, 1});
	CHECK(lean.y > 0.9f && lean.x > 0 && lean.x < 0.4f);
}

int main()
{
	test_what_is_kept_of_a_file();
	test_the_cards_sums_are_the_cpus();
	std::printf("unit_prop: %d passed, %d failed\n", passes, failures);
	return failures ? 1 : 0;
}
