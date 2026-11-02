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

// Outfit pieces put on a 3D character (model.hpp: put_on,
// hide_under_pieces): plain code over plain numbers, on the loader's
// thread (tests/unit_model.cpp).

#include "model.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <unordered_map>

namespace direbot::face {

namespace {

// A character and all its pieces together.
constexpr size_t MOST_WORN_POINTS = 8'000'000;
constexpr size_t MOST_WORN_IMAGES = 1024;

// How far a piece may be from the character's bones it's fitted to (a
// share of how far apart they are) and still be the character's: a piece
// made for someone else is much further out.
constexpr float FIT_TYPICAL = 0.05f, FIT_WORST = 0.12f;

// Its skin hidden under a piece: covered within this far outwards (a share
// of its height: a vest over a shirt), or this far inwards (the piece
// sitting just under its skin, where it would poke through).
constexpr float COVER_OUT = 0.03f, COVER_IN = 0.012f;

// Lowercase letters and digits only: "J_Bip_C_Head" -> "jbipchead".
std::string squash(const std::string &s)
{
	std::string out;
	for (char c : s)
		if (std::isalnum((unsigned char)c))
			out.push_back((char)std::tolower((unsigned char)c));
	return out;
}

bool ends_with(const std::string &s, const std::string &end)
{
	return s.size() >= end.size() && s.compare(s.size() - end.size(), end.size(), end) == 0;
}

float extent_of(const Model &m)
{
	return std::max(1e-4f, std::max(m.high.y - m.low.y, std::max(m.high.x - m.low.x, m.high.z - m.low.z)));
}

M4 add_scaled(const M4 &a, const M4 &b, float k)
{
	M4 r;
	for (int i = 0; i < 16; i++)
		r.m[i] = a.m[i] + b.m[i] * k;
	return r;
}
M4 zero_matrix()
{
	M4 z;
	for (float &f : z.m)
		f = 0;
	return z;
}

// A facing (normal) through a matrix: by its inverse turned over, so a
// stretched part's still faces out from it.
V3 normal_by(const M4 &a, V3 n)
{
	const M4 inv = inverse(a);
	return normalized(V3{inv.m[0] * n.x + inv.m[1] * n.y + inv.m[2] * n.z,
			     inv.m[4] * n.x + inv.m[5] * n.y + inv.m[6] * n.z,
			     inv.m[8] * n.x + inv.m[9] * n.y + inv.m[10] * n.z},
			  {0, 0, 1});
}

// A move, turn and size (s, r, t: p -> r p s + t).
struct Fit {
	float s = 1;
	Quat r;
	V3 t;
	V3 apply(V3 p) const { return rotate(r, p) * s + t; }
	M4 matrix() const { return matrix_of(t, r, {s, s, s}); }
};

// The biggest of a symmetric 4 x 4's eigenvalues' vector (Jacobi's turns).
void top_eigenvector(double a[4][4], double out[4])
{
	double v[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
	for (int sweep = 0; sweep < 60; sweep++) {
		double off = 0;
		for (int p = 0; p < 4; p++)
			for (int q = p + 1; q < 4; q++)
				off += a[p][q] * a[p][q];
		if (off < 1e-22)
			break;
		for (int p = 0; p < 4; p++)
			for (int q = p + 1; q < 4; q++) {
				if (std::fabs(a[p][q]) < 1e-30)
					continue;
				const double theta = (a[q][q] - a[p][p]) / (2 * a[p][q]);
				const double t = (theta >= 0 ? 1 : -1) / (std::fabs(theta) + std::sqrt(theta * theta + 1));
				const double c = 1 / std::sqrt(t * t + 1), s = t * c;
				for (int k = 0; k < 4; k++) {
					const double akp = a[k][p], akq = a[k][q];
					a[k][p] = c * akp - s * akq;
					a[k][q] = s * akp + c * akq;
				}
				for (int k = 0; k < 4; k++) {
					const double apk = a[p][k], aqk = a[q][k];
					a[p][k] = c * apk - s * aqk;
					a[q][k] = s * apk + c * aqk;
				}
				for (int k = 0; k < 4; k++) {
					const double vkp = v[k][p], vkq = v[k][q];
					v[k][p] = c * vkp - s * vkq;
					v[k][q] = s * vkp + c * vkq;
				}
			}
	}
	int best = 0;
	for (int i = 1; i < 4; i++)
		if (a[i][i] > a[best][best])
			best = i;
	for (int k = 0; k < 4; k++)
		out[k] = v[k][best];
}

// The move, turn and size that best put `from` on `to` (Horn's way, with
// quaternions), and how far off it leaves them: typically (`rms`) and at
// worst. Too few or all in a line: moved only.
Fit fit_points(const std::vector<V3> &from, const std::vector<V3> &to, float &rms, float &worst)
{
	Fit f;
	const size_t n = from.size();
	rms = worst = 0;
	if (n == 0)
		return f;
	V3 ma, mb;
	for (size_t i = 0; i < n; i++) {
		ma = ma + from[i];
		mb = mb + to[i];
	}
	ma = ma * (1.0f / n);
	mb = mb * (1.0f / n);
	double s[3][3] = {}, aa = 0;
	for (size_t i = 0; i < n; i++) {
		const V3 a = from[i] - ma, b = to[i] - mb;
		const float av[3] = {a.x, a.y, a.z}, bv[3] = {b.x, b.y, b.z};
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++)
				s[r][c] += (double)av[r] * bv[c];
		aa += dot(a, a);
	}
	// In a line (or one place): no turn to be had from them.
	bool line = n < 3 || aa < 1e-12;
	if (!line) {
		float most = 0;
		const V3 a0 = from[0] - ma;
		V3 far = a0;
		for (size_t i = 0; i < n; i++)
			if (length(from[i] - from[0]) > most) {
				most = length(from[i] - from[0]);
				far = from[i];
			}
		float off = 0;
		const V3 axis = normalized(far - from[0]);
		for (size_t i = 0; i < n; i++) {
			const V3 d = from[i] - from[0];
			off = std::max(off, length(d - axis * dot(d, axis)));
		}
		line = off < most * 1e-3f;
	}
	if (line) {
		f.t = mb - ma;
	} else {
		const double sxx = s[0][0], sxy = s[0][1], sxz = s[0][2], syx = s[1][0], syy = s[1][1], syz = s[1][2],
			     szx = s[2][0], szy = s[2][1], szz = s[2][2];
		double nm[4][4] = {{sxx + syy + szz, syz - szy, szx - sxz, sxy - syx},
				   {syz - szy, sxx - syy - szz, sxy + syx, szx + sxz},
				   {szx - sxz, sxy + syx, -sxx + syy - szz, syz + szy},
				   {sxy - syx, szx + sxz, syz + szy, -sxx - syy + szz}};
		double q[4];
		top_eigenvector(nm, q);
		f.r = normalized(Quat{(float)q[1], (float)q[2], (float)q[3], (float)q[0]});
		double num = 0;
		for (size_t i = 0; i < n; i++)
			num += dot(to[i] - mb, rotate(f.r, from[i] - ma));
		f.s = (float)(num / aa);
		if (!(f.s > 1e-6f))
			f.s = 1;
		f.t = mb - rotate(f.r, ma) * f.s;
	}
	double sum = 0;
	for (size_t i = 0; i < n; i++) {
		const float d = length(f.apply(from[i]) - to[i]);
		sum += (double)d * d;
		worst = std::max(worst, d);
	}
	rms = (float)std::sqrt(sum / n);
	return f;
}

// The character's own bones: its skins' joints (all its own parts if it
// has none), parents first as its skins list them.
std::vector<int> bones_of(const Model &m, size_t own_nodes)
{
	std::vector<int> out;
	std::vector<char> in(m.nodes.size(), 0);
	for (size_t s = 0; s < m.own_skins(); s++)
		for (int j : m.skins[s].joints)
			if (j >= 0 && (size_t)j < own_nodes && !in[j]) {
				in[j] = 1;
				out.push_back(j);
			}
	if (out.empty())
		for (size_t i = 0; i < own_nodes; i++)
			if (m.nodes[i].mesh < 0)
				out.push_back((int)i);
	return out;
}

// A VRM bone's name ("leftHand") as other files have it: "lefthand",
// "handl", "handleft", "lhand".
std::vector<std::string> other_names(const std::string &vrm_name)
{
	std::vector<std::string> out = {squash(vrm_name)};
	for (const char *side : {"left", "right"}) {
		const std::string sd = side;
		if (vrm_name.rfind(sd, 0) == 0 && vrm_name.size() > sd.size()) {
			const std::string word = squash(vrm_name.substr(sd.size()));
			const std::string s1(1, sd[0]);
			out.insert(out.end(), {word + s1, word + sd, s1 + word});
		}
	}
	return out;
}

bool is_bone(const Model &m, int n)
{
	return n >= 0 && (size_t)n < m.nodes.size();
}

// The bones a loose piece may ride: not eyes or jaws (they turn as you
// look and talk), nor swinging hair.
std::vector<int> rides_of(const Model &m, const std::vector<int> &bones)
{
	std::set<int> not_these = {m.bone("leftEye"), m.bone("rightEye"), m.bone("jaw")};
	for (const Spring &sp : m.springs)
		for (const SpringJoint &j : sp.joints)
			not_these.insert(j.node);
	std::vector<int> out;
	for (int b : bones) {
		const std::string n = squash(m.nodes[b].name);
		if (not_these.count(b) || n.find("eye") != std::string::npos || n.find("jaw") != std::string::npos)
			continue;
		out.push_back(b);
	}
	return out.empty() ? bones : out;
}

// How far `p` is from a bone: from it to the middle of its children.
float bone_distance(const Model &m, const std::vector<M4> &world, int b, V3 p)
{
	const V3 a = move_of(world[b]);
	V3 sum;
	int kids = 0;
	for (int c : m.nodes[b].children)
		if (is_bone(m, c) && m.nodes[c].mesh < 0) {
			sum = sum + move_of(world[c]);
			kids++;
		}
	if (!kids)
		return length(p - a);
	const V3 e = sum * (1.0f / kids), d = e - a;
	const float dd = dot(d, d);
	const float t = dd > 1e-12f ? std::clamp(dot(p - a, d) / dd, 0.0f, 1.0f) : 0.0f;
	return length(p - (a + d * t));
}

int nearest_bone(const Model &m, const std::vector<M4> &world, const std::vector<int> &bones, V3 p)
{
	int best = -1;
	float bd = 1e30f;
	for (int b : bones) {
		const float d = bone_distance(m, world, b, p);
		if (d < bd) {
			bd = d;
			best = b;
		}
	}
	return best;
}

void shift_tex(TexRef &t, int by)
{
	if (t.image >= 0)
		t.image += by;
}

// Its points and facings at rest, in the model (as measure() skins them).
void rest_points(const Model &m, const std::vector<M4> &world, int node, const Primitive &p, std::vector<V3> &pos,
		 std::vector<V3> &normal)
{
	const Node &nd = m.nodes[node];
	const Skin *skin = nd.skin >= 0 && (size_t)nd.skin < m.skins.size() ? &m.skins[nd.skin] : nullptr;
	std::vector<M4> joint;
	if (skin)
		for (size_t j = 0; j < skin->joints.size(); j++)
			joint.push_back(world[skin->joints[j]] * skin->inverse_bind[j]);
	pos.resize(p.pos.size());
	normal.resize(p.pos.size());
	for (size_t i = 0; i < p.pos.size(); i++) {
		const V3 n = i < p.normal.size() ? p.normal[i] : V3{0, 0, 1};
		if (skin && !p.joints.empty()) {
			V3 v, nn;
			for (int k = 0; k < 4; k++) {
				const float w = p.weights[i * 4 + k];
				const uint16_t j = p.joints[i * 4 + k];
				if (w > 0 && j < joint.size()) {
					v = v + point(joint[j], p.pos[i]) * w;
					nn = nn + direction(joint[j], n) * w;
				}
			}
			pos[i] = v;
			normal[i] = normalized(nn, {0, 0, 1});
		} else {
			pos[i] = point(world[node], p.pos[i]);
			normal[i] = normalized(direction(world[node], n), {0, 0, 1});
		}
	}
}

// Its points (and expressions' moves) through `a`.
void bake(Primitive &p, const M4 &a)
{
	for (V3 &v : p.pos)
		v = point(a, v);
	for (V3 &n : p.normal)
		n = normal_by(a, n);
	for (MorphTarget &mt : p.morphs) {
		for (V3 &d : mt.pos)
			d = direction(a, d);
		for (V3 &n : mt.normal)
			n = direction(a, n);
	}
}

int add_node(Model &m, std::vector<M4> &world, Node node, const M4 &node_world)
{
	const int at = (int)m.nodes.size();
	if (node.parent >= 0)
		m.nodes[node.parent].children.push_back(at);
	else
		m.roots.push_back(at);
	m.nodes.push_back(std::move(node));
	world.push_back(node_world);
	return at;
}

// A node's own move, turn and size, from where it's to be and its parent.
void place_node(Node &n, const M4 &local)
{
	n.t = move_of(local);
	const V3 c0{local.m[0], local.m[1], local.m[2]}, c1{local.m[4], local.m[5], local.m[6]},
		c2{local.m[8], local.m[9], local.m[10]};
	n.s = {length(c0), length(c1), length(c2)};
	n.r = quat_of(local);
}

} // namespace

int bone_called(const Model &m, const std::string &name)
{
	if (name.empty())
		return -1;
	const size_t own = m.worn.empty() ? m.nodes.size() : (size_t)m.worn.front().first_node;
	const int vrm = m.bone(name);
	if (vrm >= 0 && (size_t)vrm < own)
		return vrm;
	const std::vector<int> bones = bones_of(m, own);
	for (int b : bones)
		if (m.nodes[b].name == name)
			return b;
	const std::vector<std::string> names = other_names(name);
	for (const std::string &want : names)
		for (int b : bones)
			if (squash(m.nodes[b].name) == want)
				return b;
	for (const std::string &want : names)
		for (int b : bones)
			if (want.size() >= 3 && ends_with(squash(m.nodes[b].name), want))
				return b;
	return -1;
}

PieceOn put_on(Model &m, const Model &piece, const PieceHow &how)
{
	PieceOn out;
	bool shown = false;
	for (const Node &n : piece.nodes)
		shown = shown || (n.mesh >= 0 && n.mesh < (int)piece.meshes.size());
	if (!shown || piece.points == 0) {
		out.problem = "there's nothing to see in it";
		return out;
	}
	if (m.points + piece.points > MOST_WORN_POINTS) {
		out.problem = "it's too big to put on with the rest (over 8 million points together)";
		return out;
	}
	if (m.images.size() + piece.images.size() > MOST_WORN_IMAGES) {
		out.problem = "there are too many pictures with the rest";
		return out;
	}
	const size_t own_nodes = m.worn.empty() ? m.nodes.size() : (size_t)m.worn.front().first_node;
	std::vector<M4> world = rest_world(m);
	const std::vector<M4> pw = rest_world(piece);
	const float height = extent_of(m);
	const std::vector<int> bones = bones_of(m, own_nodes);

	// Its bones: its skins' joints.
	std::vector<int> pbones;
	std::vector<char> is_pbone(piece.nodes.size(), 0);
	for (const Skin &s : piece.skins)
		for (int j : s.joints)
			if (is_bone(piece, j) && !is_pbone[j]) {
				is_pbone[j] = 1;
				pbones.push_back(j);
			}

	// Found among the character's: each way of naming them (the same, a
	// prefix, any case, a VRM bone's name) a group; the one finding most.
	std::vector<int> map(piece.nodes.size(), -1);
	Fit fit;
	bool fitted = false;
	if (!pbones.empty() && !bones.empty()) {
		struct Group {
			std::vector<int> from, to;
			std::set<int> have;
			int rank = 0; // the same 0, any case 1, VRM's name 2
		};
		std::map<std::string, Group> groups;
		auto add = [&](const std::string &key, int rank, int p, int b) {
			Group &g = groups[key];
			g.rank = rank;
			if (g.have.insert(p).second) {
				g.from.push_back(p);
				g.to.push_back(b);
			}
		};
		std::vector<std::string> squashed(bones.size());
		for (size_t i = 0; i < bones.size(); i++)
			squashed[i] = squash(m.nodes[bones[i]].name);
		for (int p : pbones) {
			const std::string &pn = piece.nodes[p].name;
			if (pn.empty())
				continue;
			const std::string ps = squash(pn);
			for (size_t i = 0; i < bones.size(); i++) {
				const std::string &bn = m.nodes[bones[i]].name;
				if (ends_with(bn, pn))
					add("=" + bn.substr(0, bn.size() - pn.size()), 0, p, bones[i]);
				if (!ps.empty() && ends_with(squashed[i], ps))
					add("~" + squashed[i].substr(0, squashed[i].size() - ps.size()), 1, p, bones[i]);
			}
			const int vrm = m.bone(pn);
			if (vrm >= 0 && (size_t)vrm < own_nodes)
				add("vrm", 2, p, vrm);
		}
		const Group *best = nullptr;
		Fit best_fit;
		float best_rms = 0, best_worst = 0;
		for (const auto &kv : groups) {
			const Group &g = kv.second;
			std::vector<V3> a, b;
			for (size_t i = 0; i < g.from.size(); i++) {
				a.push_back(move_of(pw[g.from[i]]));
				b.push_back(move_of(world[g.to[i]]));
			}
			float rms, worst;
			const Fit f = fit_points(a, b, rms, worst);
			const bool better = !best || g.from.size() > best->from.size() ||
					    (g.from.size() == best->from.size() &&
					     (rms < best_rms - 1e-6f || (rms <= best_rms + 1e-6f && g.rank < best->rank)));
			if (better) {
				best = &g;
				best_fit = f;
				best_rms = rms;
				best_worst = worst;
			}
		}
		if (best && !best->from.empty()) {
			// How far apart the bones it's fitted to are.
			V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
			for (int b : best->to) {
				const V3 p = move_of(world[b]);
				lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
				hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
			}
			const float span = std::max(length(hi - lo), height * 0.05f);
			if (best_rms > FIT_TYPICAL * span || best_worst > FIT_WORST * span || best_fit.s < 1e-3f ||
			    best_fit.s > 1e3f) {
				out.problem = "its bones aren't this character's - it was made for another";
				return out;
			}
			for (size_t i = 0; i < best->from.size(); i++)
				map[best->from[i]] = best->to[i];
			fit = best_fit;
			fitted = true;
			out.bones = (int)best->from.size();
		}
	}

	WornPiece rec;
	rec.first_node = (int)m.nodes.size();
	rec.first_mesh = (int)m.meshes.size();
	rec.first_skin = (int)m.skins.size();
	rec.hide = how.hide;

	// Its own bones (a cape's), hanging off the nearest of the character's
	// it was fitted to: parents first.
	if (fitted) {
		auto depth = [&](int n) {
			int d = 0;
			while (n >= 0 && d < 100000) {
				n = piece.nodes[n].parent;
				d++;
			}
			return d;
		};
		std::vector<int> order = pbones;
		std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return depth(a) < depth(b); });
		for (int p : order) {
			if (map[p] >= 0)
				continue;
			int q = piece.nodes[p].parent;
			while (q >= 0 && map[q] < 0)
				q = piece.nodes[q].parent;
			const M4 at = fit.matrix() * pw[p];
			Node n;
			n.name = piece.nodes[p].name;
			n.parent = q >= 0 ? map[q] : nearest_bone(m, world, bones, move_of(at));
			place_node(n, n.parent >= 0 ? inverse(world[n.parent]) * at : at);
			map[p] = add_node(m, world, n, at);
			out.own_bones++;
		}
	}

	// Its pictures and materials, after the character's.
	const int img0 = (int)m.images.size(), mat0 = (int)m.materials.size();
	m.images.insert(m.images.end(), piece.images.begin(), piece.images.end());
	for (Material mat : piece.materials) {
		for (TexRef *t : {&mat.base_map, &mat.metal_map, &mat.emissive_map, &mat.shade_map, &mat.rim_map,
				  &mat.matcap_map})
			shift_tex(*t, img0);
		m.materials.push_back(mat);
	}
	auto take_mesh = [&](int mesh) {
		Mesh copy = piece.meshes[mesh];
		for (Primitive &p : copy.parts)
			if (p.material >= 0)
				p.material += mat0;
		return copy;
	};

	// Its parts: on its bones (skinned), on one of its bones (stiff), or
	// loose.
	std::vector<int> loose;
	for (size_t i = 0; i < piece.nodes.size(); i++) {
		const Node &pn = piece.nodes[i];
		if (pn.mesh < 0 || pn.mesh >= (int)piece.meshes.size())
			continue;
		const Skin *ps = pn.skin >= 0 && pn.skin < (int)piece.skins.size() ? &piece.skins[pn.skin] : nullptr;
		bool skinned = ps && fitted;
		if (skinned)
			for (int j : ps->joints)
				skinned = skinned && map[j] >= 0;
		if (skinned) {
			Skin skin;
			std::vector<M4> was(ps->joints.size());
			std::vector<V3> shift(ps->joints.size());
			for (size_t k = 0; k < ps->joints.size(); k++) {
				const int b = map[ps->joints[k]];
				skin.joints.push_back(b);
				skin.inverse_bind.push_back(inverse(world[b]));
				was[k] = pw[ps->joints[k]] * ps->inverse_bind[k];
				// Each point follows its own bones' places on the
				// character, as near as they are to the fit.
				shift[k] = move_of(world[b]) - fit.apply(move_of(pw[ps->joints[k]]));
			}
			Mesh mesh = take_mesh(pn.mesh);
			for (Primitive &p : mesh.parts) {
				const size_t count = p.pos.size();
				if (p.joints.empty()) {
					// No bones for this part of it: on the first.
					p.joints.assign(count * 4, 0);
					p.weights.assign(count * 4, 0);
					for (size_t v = 0; v < count; v++)
						p.weights[v * 4] = 1;
					// (Its first bone's bind is its place at rest: the
					// points stay where the fit puts them.)
					bake(p, fit.matrix() * pw[i]);
					continue;
				}
				for (size_t v = 0; v < count; v++) {
					M4 acc = zero_matrix();
					V3 sh;
					float total = 0;
					for (int k = 0; k < 4; k++) {
						const float w = p.weights[v * 4 + k];
						const uint16_t j = p.joints[v * 4 + k];
						if (w <= 0 || j >= was.size())
							continue;
						acc = add_scaled(acc, was[j], w);
						sh = sh + shift[j] * w;
						total += w;
					}
					if (total <= 0)
						acc = pw[i];
					p.pos[v] = fit.apply(point(acc, p.pos[v])) + sh;
					if (v < p.normal.size())
						p.normal[v] = normalized(rotate(fit.r, normal_by(acc, p.normal[v])), {0, 0, 1});
					for (MorphTarget &mt : p.morphs) {
						if (v < mt.pos.size())
							mt.pos[v] = rotate(fit.r, direction(acc, mt.pos[v])) * fit.s;
						if (v < mt.normal.size())
							mt.normal[v] = rotate(fit.r, direction(acc, mt.normal[v]));
					}
				}
			}
			m.meshes.push_back(std::move(mesh));
			m.skins.push_back(std::move(skin));
			Node n;
			n.name = pn.name;
			n.mesh = (int)m.meshes.size() - 1;
			n.skin = (int)m.skins.size() - 1;
			add_node(m, world, n, M4());
			continue;
		}
		// Stiff on one of its bones that's the character's.
		int q = fitted ? pn.parent : -1;
		while (q >= 0 && !(is_pbone[q] && map[q] >= 0))
			q = piece.nodes[q].parent;
		if (q >= 0) {
			const int b = map[q];
			const V3 shift = move_of(world[b]) - fit.apply(move_of(pw[q]));
			Mesh mesh = take_mesh(pn.mesh);
			const M4 a = inverse(world[b]) * moved(shift) * fit.matrix() * pw[i];
			for (Primitive &p : mesh.parts) {
				p.joints.clear();
				p.weights.clear();
				bake(p, a);
			}
			m.meshes.push_back(std::move(mesh));
			Node n;
			n.name = pn.name;
			n.parent = b;
			n.mesh = (int)m.meshes.size() - 1;
			add_node(m, world, n, world[b]);
			continue;
		}
		loose.push_back((int)i);
	}

	// Loose: it rides a bone, nudged into place.
	if (!loose.empty()) {
		out.loose = out.bones == 0;
		const Fit use = fitted ? fit : Fit();
		std::vector<M4> at(loose.size());
		V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
		for (size_t k = 0; k < loose.size(); k++) {
			const int i = loose[k];
			at[k] = use.matrix() * pw[i];
			const Skin *ps = piece.nodes[i].skin >= 0 && piece.nodes[i].skin < (int)piece.skins.size()
						 ? &piece.skins[piece.nodes[i].skin]
						 : nullptr;
			for (const Primitive &p : piece.meshes[piece.nodes[i].mesh].parts) {
				std::vector<V3> pos, nor;
				if (ps && !p.joints.empty()) {
					// Bones that aren't the character's: as it stands.
					Model one;
					one.nodes = piece.nodes;
					one.skins = piece.skins;
					rest_points(one, pw, i, p, pos, nor);
					for (V3 &v : pos)
						v = use.apply(v);
				} else {
					for (const V3 &v : p.pos)
						pos.push_back(point(at[k], v));
				}
				for (const V3 &v : pos) {
					lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
					hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
				}
			}
		}
		if (lo.x > hi.x)
			lo = hi = {};
		const V3 mid = (lo + hi) * 0.5f, size3 = hi - lo;
		const float size = std::max(size3.x, std::max(size3.y, size3.z));
		const float grow = 0.25f * height;
		const bool near_it = mid.x > m.low.x - grow && mid.x < m.high.x + grow && mid.y > m.low.y - grow &&
				     mid.y < m.high.y + grow && mid.z > m.low.z - grow && mid.z < m.high.z + grow;
		const bool plausible = size >= 0.005f * height && size <= 1.5f * height && near_it;
		M4 first;
		V3 centre = mid;
		const int head = bone_called(m, "head");
		if (!plausible) {
			// On top of its head, about a hat's size, to nudge from.
			const float k = 0.18f * height / std::max(1e-6f, size);
			V3 top{(m.low.x + m.high.x) / 2, m.high.y, (m.low.z + m.high.z) / 2};
			if (head >= 0) {
				const V3 h = move_of(world[head]);
				top = {h.x, h.y + 0.75f * std::max(0.0f, m.high.y - h.y), h.z};
			}
			first = moved(top) * scaled(k) * moved(V3{mid.x, lo.y, mid.z} * -1);
			centre = point(first, mid);
			out.placed = true;
		}
		const Quat face = normalized(how.facing);
		const float rad = 3.14159265f / 180;
		const Quat t = axis_angle({0, 1, 0}, how.turn.y * rad) * axis_angle({1, 0, 0}, how.turn.x * rad) *
			       axis_angle({0, 0, 1}, how.turn.z * rad);
		const Quat turn_m = normalized(conj(face) * t * face);
		const V3 move_m = rotate(conj(face), how.move * height);
		const float grown = std::clamp(how.size, 0.01f, 100.0f);
		const M4 nudge = moved(centre + move_m) * matrix_of(turn_m) * scaled(grown) * moved(centre * -1);
		const V3 placed_mid = point(nudge, centre);
		int ride = -1;
		if (!how.bone.empty())
			ride = bone_called(m, how.bone);
		if (ride < 0 && out.placed && head >= 0)
			ride = head;
		if (ride < 0)
			ride = nearest_bone(m, world, rides_of(m, bones), placed_mid);
		for (size_t k = 0; k < loose.size(); k++) {
			const int i = loose[k];
			const Skin *ps = piece.nodes[i].skin >= 0 && piece.nodes[i].skin < (int)piece.skins.size()
						 ? &piece.skins[piece.nodes[i].skin]
						 : nullptr;
			Mesh mesh = take_mesh(piece.nodes[i].mesh);
			const M4 to_bone = ride >= 0 ? inverse(world[ride]) : M4();
			for (Primitive &p : mesh.parts) {
				if (ps && !p.joints.empty()) {
					Model one;
					one.nodes = piece.nodes;
					one.skins = piece.skins;
					std::vector<V3> pos, nor;
					rest_points(one, pw, i, p, pos, nor);
					p.pos = pos;
					p.normal = nor;
					p.morphs.clear();
					bake(p, to_bone * nudge * first * use.matrix());
				} else {
					bake(p, to_bone * nudge * first * at[k]);
				}
				p.joints.clear();
				p.weights.clear();
			}
			m.meshes.push_back(std::move(mesh));
			Node n;
			n.name = piece.nodes[i].name;
			n.parent = ride;
			n.mesh = (int)m.meshes.size() - 1;
			add_node(m, world, n, ride >= 0 ? world[ride] : M4());
		}
		if (ride >= 0) {
			out.rides = m.nodes[ride].name;
			for (const auto &kv : m.bones)
				if (kv.second == ride)
					out.rides = kv.first;
		}
	}

	rec.end_node = (int)m.nodes.size();
	rec.end_mesh = (int)m.meshes.size();
	rec.end_skin = (int)m.skins.size();
	for (int mi = rec.first_mesh; mi < rec.end_mesh; mi++)
		for (const Primitive &p : m.meshes[mi].parts) {
			m.points += p.pos.size();
			m.triangles += p.index.size() / 3;
		}
	m.worn.push_back(rec);
	out.on = true;
	return out;
}

size_t hide_under_pieces(Model &m)
{
	bool any = false;
	for (const WornPiece &w : m.worn)
		any = any || w.hide;
	if (!any)
		return 0;
	const std::vector<M4> world = rest_world(m);
	const float height = extent_of(m), cell = std::max(1e-5f, height * 0.02f);
	struct Tri {
		V3 a, b, c, n;
	};
	std::vector<Tri> tris;
	for (const WornPiece &w : m.worn) {
		if (!w.hide)
			continue;
		for (int ni = w.first_node; ni < w.end_node; ni++) {
			const Node &nd = m.nodes[ni];
			if (nd.mesh < 0)
				continue;
			for (const Primitive &p : m.meshes[nd.mesh].parts) {
				std::vector<V3> pos, nor;
				rest_points(m, world, ni, p, pos, nor);
				for (size_t i = 0; i + 2 < p.index.size(); i += 3) {
					const V3 a = pos[p.index[i]], b = pos[p.index[i + 1]], c = pos[p.index[i + 2]];
					const V3 n = cross(b - a, c - a);
					if (length(n) < 1e-12f)
						continue;
					tris.push_back({a, b, c, normalized(n)});
				}
			}
		}
	}
	if (tris.empty())
		return 0;
	// Which triangles are near each little box of space.
	auto key = [](int x, int y, int z) {
		return ((int64_t)(x + 1048576) << 42) | ((int64_t)(y + 1048576) << 21) | (int64_t)(z + 1048576);
	};
	auto cell_of = [&](float v) { return (int)std::floor(v / cell); };
	std::unordered_map<int64_t, std::vector<uint32_t>> grid;
	std::vector<uint32_t> big; // too big for the boxes: tried every time
	for (uint32_t t = 0; t < tris.size(); t++) {
		const Tri &tr = tris[t];
		const int x0 = cell_of(std::min({tr.a.x, tr.b.x, tr.c.x})), x1 = cell_of(std::max({tr.a.x, tr.b.x, tr.c.x}));
		const int y0 = cell_of(std::min({tr.a.y, tr.b.y, tr.c.y})), y1 = cell_of(std::max({tr.a.y, tr.b.y, tr.c.y}));
		const int z0 = cell_of(std::min({tr.a.z, tr.b.z, tr.c.z})), z1 = cell_of(std::max({tr.a.z, tr.b.z, tr.c.z}));
		if ((int64_t)(x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) > 512) {
			big.push_back(t);
			continue;
		}
		for (int x = x0; x <= x1; x++)
			for (int y = y0; y <= y1; y++)
				for (int z = z0; z <= z1; z++)
					grid[key(x, y, z)].push_back(t);
	}
	std::vector<uint32_t> stamp(tris.size(), 0);
	uint32_t ray = 0;
	// A piece within `reach` of `p` along `dir`, facing roughly along it.
	auto hits = [&](V3 p, V3 dir, float reach) {
		ray++;
		auto test = [&](uint32_t t) {
			if (stamp[t] == ray)
				return false;
			stamp[t] = ray;
			const Tri &tr = tris[t];
			if (std::fabs(dot(tr.n, dir)) < 0.2f)
				return false;
			const V3 e1 = tr.b - tr.a, e2 = tr.c - tr.a, h = cross(dir, e2);
			const float det = dot(e1, h);
			if (std::fabs(det) < 1e-12f)
				return false;
			const float inv = 1 / det;
			const V3 s = p - tr.a;
			const float u = dot(s, h) * inv;
			if (u < 0 || u > 1)
				return false;
			const V3 q = cross(s, e1);
			const float v = dot(dir, q) * inv;
			if (v < 0 || u + v > 1)
				return false;
			const float d = dot(e2, q) * inv;
			return d >= -cell * 0.01f && d <= reach;
		};
		for (uint32_t t : big)
			if (test(t))
				return true;
		const int steps = std::max(1, (int)std::ceil(reach / (cell * 0.5f)));
		int64_t last = -1;
		for (int s = 0; s <= steps; s++) {
			const V3 at = p + dir * (reach * s / steps);
			const int64_t k = key(cell_of(at.x), cell_of(at.y), cell_of(at.z));
			if (k == last)
				continue;
			last = k;
			const auto it = grid.find(k);
			if (it != grid.end())
				for (uint32_t t : it->second)
					if (test(t))
						return true;
		}
		return false;
	};
	size_t hidden = 0;
	const size_t own_nodes = (size_t)m.worn.front().first_node;
	const size_t own_meshes = m.own_meshes();
	std::set<int> done;
	for (size_t ni = 0; ni < own_nodes; ni++) {
		const Node &nd = m.nodes[ni];
		if (nd.mesh < 0 || (size_t)nd.mesh >= own_meshes || !done.insert(nd.mesh).second)
			continue;
		for (Primitive &p : m.meshes[nd.mesh].parts) {
			std::vector<V3> pos, nor;
			rest_points(m, world, (int)ni, p, pos, nor);
			std::vector<char> covered(pos.size(), 0);
			for (size_t v = 0; v < pos.size(); v++)
				covered[v] = hits(pos[v], nor[v], COVER_OUT * height) ||
					     hits(pos[v], nor[v] * -1, COVER_IN * height);
			std::vector<uint32_t> keep;
			keep.reserve(p.index.size());
			for (size_t i = 0; i + 2 < p.index.size(); i += 3) {
				if (covered[p.index[i]] && covered[p.index[i + 1]] && covered[p.index[i + 2]]) {
					hidden++;
					continue;
				}
				keep.insert(keep.end(), {p.index[i], p.index[i + 1], p.index[i + 2]});
			}
			p.index = std::move(keep);
		}
	}
	m.triangles = m.triangles >= hidden ? m.triangles - hidden : 0;
	return hidden;
}

} // namespace direbot::face
