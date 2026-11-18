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

#include "prop-mesh.hpp"

#include <algorithm>
#include <cmath>

namespace direbot::face {

void srgb_bytes(const float *c, uint8_t *out)
{
	for (int k = 0; k < 3; k++) {
		const float v = std::min(1.0f, std::max(0.0f, c[k]));
		out[k] = (uint8_t)std::lround(std::pow(v, 1 / 2.2f) * 255);
	}
	out[3] = (uint8_t)std::lround(std::min(1.0f, std::max(0.0f, c[3])) * 255);
}

size_t PropMesh::Part::bytes() const
{
	return (pos.size() + normal.size() + tangent.size() + uv.size() + shapes.size()) * sizeof(float) +
	       colour.size() + index16.size() * sizeof(uint16_t) + index32.size() * sizeof(uint32_t);
}

size_t PropMesh::bytes() const
{
	size_t sum = 0;
	for (const Part &part : parts)
		sum += part.bytes();
	return sum;
}

PropMesh make_prop_mesh(const Model &m)
{
	PropMesh out;
	out.materials = m.materials;
	const std::vector<M4> world = rest_world(m);
	bool any = false;
	for (size_t ni = 0; ni < m.nodes.size(); ni++) {
		const Node &node = m.nodes[ni];
		if (node.mesh < 0 || node.mesh >= (int)m.meshes.size())
			continue;
		const Mesh &mesh = m.meshes[node.mesh];
		const Skin *skin = node.skin >= 0 && node.skin < (int)m.skins.size() ? &m.skins[node.skin] : nullptr;
		for (const Primitive &p : mesh.parts) {
			PropMesh::Part part;
			part.material = p.material;
			part.points = p.pos.size();
			if (!part.points || p.index.empty() || p.strands)
				continue;
			const size_t n = part.points;
			const bool weighted = skin && p.joints.size() == n * 4 && p.weights.size() == n * 4;
			// Where each point's node (a bound one: its strongest bone, as
			// it was bound) puts it.
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
			part.bumped = p.tangent.size() == n * 4;
			part.pos.assign(n * 4, 0.0f);
			part.normal.assign(n * 4, 0.0f);
			if (part.bumped)
				part.tangent.assign(n * 4, 0.0f);
			part.uv.assign(n * 2, 0.0f);
			part.colour.assign(n * 4, 255);
			for (size_t k = 0; k < n; k++) {
				const M4 w = at(k);
				const V3 q = point(w, p.pos[k]);
				const V3 facing = normalized(
					direction(w, k < p.normal.size() ? p.normal[k] : V3{0, 0, 1}), {0, 0, 1});
				part.pos[k * 4] = q.x, part.pos[k * 4 + 1] = q.y, part.pos[k * 4 + 2] = q.z;
				part.normal[k * 4] = facing.x, part.normal[k * 4 + 1] = facing.y,
						part.normal[k * 4 + 2] = facing.z;
				if (part.bumped) {
					const V3 t = normalized(direction(w, {p.tangent[k * 4], p.tangent[k * 4 + 1],
									      p.tangent[k * 4 + 2]}),
								{1, 0, 0});
					part.tangent[k * 4] = t.x, part.tangent[k * 4 + 1] = t.y,
							 part.tangent[k * 4 + 2] = t.z;
					// Which way its picture's up is.
					part.tangent[k * 4 + 3] = p.tangent[k * 4 + 3];
				}
				if (p.uv.size() >= (k + 1) * 2) {
					part.uv[k * 2] = p.uv[k * 2];
					part.uv[k * 2 + 1] = p.uv[k * 2 + 1];
				}
				if (p.colour.size() >= (k + 1) * 4)
					srgb_bytes(&p.colour[k * 4], &part.colour[k * 4]);
				if (!any) {
					out.low = out.high = q;
					any = true;
				}
				out.low = {std::min(out.low.x, q.x), std::min(out.low.y, q.y),
					   std::min(out.low.z, q.z)};
				out.high = {std::max(out.high.x, q.x), std::max(out.high.y, q.y),
					    std::max(out.high.z, q.z)};
				part.middle = part.middle + q * (1.0f / (float)n);
			}
			// Its shapes, the first of them: how far each moves a point and
			// turns its facing, with the point (a shape that moves none of
			// this part: nothing to carry).
			out.shapes_in_file = std::max(out.shapes_in_file, p.morphs.size());
			const size_t kept = std::min(p.morphs.size(), PROP_SHAPES);
			for (size_t s = 0; s < kept; s++) {
				const MorphTarget &t = p.morphs[s];
				// (Where a shape's numbers are among a point's twelve.)
				const size_t move_at[PROP_SHAPES][3] = {{0, 1, 2}, {6, 7, 8}};
				const size_t turn_at[PROP_SHAPES][3] = {{3, 4, 5}, {9, 10, 11}};
				for (size_t i = 0; i < t.at.size(); i++) {
					const size_t k = t.at[i];
					if (k >= n)
						continue;
					const M4 w = at(k);
					const V3 dp = i < t.pos.size() ? direction(w, t.pos[i]) : V3{};
					const V3 dn = i < t.normal.size() ? direction(w, t.normal[i]) : V3{};
					if (dp.x == 0 && dp.y == 0 && dp.z == 0 && dn.x == 0 && dn.y == 0 && dn.z == 0)
						continue;
					if (!part.shaped) {
						part.shaped = true;
						part.shapes.assign(n * PROP_SHAPE_NUMBERS, 0.0f);
					}
					float *row = &part.shapes[k * PROP_SHAPE_NUMBERS];
					row[move_at[s][0]] = dp.x, row[move_at[s][1]] = dp.y, row[move_at[s][2]] = dp.z;
					row[turn_at[s][0]] = dn.x, row[turn_at[s][1]] = dn.y, row[turn_at[s][2]] = dn.z;
				}
			}
			out.shapes = std::max(out.shapes, kept);
			if (n <= 65536) {
				part.index16.resize(p.index.size());
				for (size_t i = 0; i < p.index.size(); i++)
					part.index16[i] = (uint16_t)p.index[i];
			} else {
				part.index32 = p.index;
			}
			out.points += n;
			out.triangles += p.index.size() / 3;
			if (out.shape_names.size() < mesh.morph_names.size())
				out.shape_names = mesh.morph_names;
			out.parts.push_back(std::move(part));
		}
	}
	// (As many names as shapes kept; a file with more names than shapes,
	// or the other way: each shape its name, or none.)
	out.shape_names.resize(out.shapes);
	return out;
}

} // namespace direbot::face
