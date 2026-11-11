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

#pragma once

#include <string>
#include <vector>

namespace direbot::face {

// The values each technique of the face avatar's effects reads (OBS's
// own ViewProj aside): data/effects/direbot-face-3d.effect (the 3D
// characters; -skin: posing them on the card too) and direbot-face.effect (pictures, cutouts, cards). OBS
// empties every value an effect holds each time one of its techniques
// finishes, and on Windows (Direct3D) it won't draw at all if a value a
// shader reads is empty, where OpenGL quietly reads zeros. So the drawing
// sets every one of these just before each technique and checks it did
// (effect-check.hpp); tests/unit_model.cpp checks these lists against the
// effects themselves.
struct EffectNeeds {
	const char *technique;
	std::vector<std::string> values;
};

inline const std::vector<EffectNeeds> &face_3d_needs()
{
	static const std::vector<EffectNeeds> needs = {
		{"Draw",
		 {"cam", "uv_place", "base_map", "shade_map", "emissive_map", "metal_map", "matcap_map", "base_colour",
		  "look", "surface", "emissive", "toon", "shade_colour", "rim_colour", "matcap_colour", "occlusion_map",
		  "detail", "key_dir", "key_colour", "fill_dir", "fill_colour", "room_up", "room_down", "rim_light",
		  "key_tint", "room_tint", "gas", "flat_at"}},
		{"DrawBumped",
		 {"cam", "uv_place", "base_map", "shade_map", "emissive_map", "metal_map", "matcap_map", "base_colour",
		  "look", "surface", "emissive", "toon", "shade_colour", "rim_colour", "matcap_colour", "occlusion_map",
		  "detail", "key_dir", "key_colour", "fill_dir", "fill_colour", "room_up", "room_down", "rim_light",
		  "key_tint", "room_tint", "normal_map", "gas", "flat_at"}},
		{"Outline",
		 {"cam", "uv_place", "outline_width", "base_map", "surface", "base_colour", "outline_colour", "look",
		  "key_tint"}},
		{"Hidden", {"cam", "uv_place", "flat_at"}},
		{"Depth", {"cam", "uv_place", "flat_at", "base_map", "surface", "base_colour"}},
		{"Strand",
		 {"cam", "base_map", "emissive_map", "base_colour", "look", "surface", "emissive", "key_dir",
		  "key_colour", "fill_dir", "fill_colour", "room_up", "room_down", "rim_light", "key_tint", "room_tint",
		  "eye_pos", "strand", "strand_lod", "strand_view", "depth_map"}},
		{"Hull", {"cam", "eye_pos", "strand", "strand_lod"}},
		{"Down", {"image", "texel"}},
	};
	return needs;
}

// direbot-face-3d-skin.effect (Windows: posing on the graphics card): all
// of the above, and the same drawn from the points as made, with bones.
inline const std::vector<EffectNeeds> &face_3d_skin_needs()
{
	static const std::vector<EffectNeeds> needs = [] {
		std::vector<EffectNeeds> all = face_3d_needs();
		const std::vector<EffectNeeds> skin = {
			{"DrawSk",
			 {"cam", "uv_place", "base_map", "shade_map", "emissive_map", "metal_map", "matcap_map", "base_colour",
			  "look", "surface", "emissive", "toon", "shade_colour", "rim_colour", "matcap_colour", "occlusion_map",
			  "detail", "key_dir", "key_colour", "fill_dir", "fill_colour", "room_up", "room_down", "rim_light",
			  "key_tint", "room_tint", "gas", "flat_at", "bone_cols"}},
			{"DrawBumpedSk",
			 {"cam", "uv_place", "base_map", "shade_map", "emissive_map", "metal_map", "matcap_map", "base_colour",
			  "look", "surface", "emissive", "toon", "shade_colour", "rim_colour", "matcap_colour", "occlusion_map",
			  "detail", "key_dir", "key_colour", "fill_dir", "fill_colour", "room_up", "room_down", "rim_light",
			  "key_tint", "room_tint", "normal_map", "gas", "flat_at", "bone_cols", "skin_info"}},
			{"OutlineSk",
			 {"cam", "uv_place", "outline_width", "base_map", "surface", "base_colour", "outline_colour", "look",
			  "key_tint", "bone_cols"}},
			{"DepthSk", {"cam", "uv_place", "flat_at", "base_map", "surface", "base_colour", "bone_cols"}},
			{"StrandSk",
			 {"cam", "base_map", "emissive_map", "base_colour", "look", "surface", "emissive", "key_dir",
			  "key_colour", "fill_dir", "fill_colour", "room_up", "room_down", "rim_light", "key_tint", "room_tint",
			  "eye_pos", "strand", "strand_lod", "strand_view", "depth_map", "bone_cols", "spot_moves"}},
			{"HullSk", {"cam", "eye_pos", "strand", "strand_lod", "bone_cols", "spot_moves"}},
		};
		all.insert(all.end(), skin.begin(), skin.end());
		return all;
	}();
	return needs;
}

inline const std::vector<EffectNeeds> &face_2d_needs()
{
	static const std::vector<EffectNeeds> needs = {
		{"Picture", {"image", "opacity", "clip"}},
		{"Stain", {"image", "opacity"}},
		{"Cutout",
		 {"image", "mask_map", "body_map", "opacity", "origin", "across", "down", "mask_place", "body_place",
		  "inside", "skin_lab", "target_lab", "mask_rows", "skin_amount"}},
		{"Turn", {"image", "depth_map", "opacity", "char_px", "pivot", "cell", "turn_k", "pad"}},
		{"Card",
		 {"image", "mask_map", "body_map", "opacity", "origin", "across", "down", "mask_place", "mask_rows",
		  "body_origin", "body_across", "body_down", "ring", "ring_on", "skin_lab", "target_lab",
		  "skin_amount", "card_light"}},
		{"Camera", {"image", "flip"}},
	};
	return needs;
}

} // namespace direbot::face
