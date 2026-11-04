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

// A 3D character (model.hpp, posed by rig.hpp) drawn with OBS's own
// graphics, for the face avatar: its parts with depth (near hides far),
// lit glossy, as a cartoon or flat (data/effects/direbot-face-3d.effect)
// by the lights chosen (lights.hpp), with its normal and occlusion maps
// and soft skin,
// cartoon outlines, see-through parts after the solid ones (far to near),
// a hidden head shape that tucks what's behind your head out of sight
// (worn on your head), and smooth edges (drawn twice the size, halved).
// See-through everywhere else.

#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <graphics/graphics.h>

#include "effect-check.hpp"
#include "lights.hpp"
#include "math3d.hpp"
#include "model.hpp"
#include "pictures.hpp"
#include "rig.hpp"

namespace direbot::face {

// A model's pictures (pictures.hpp), read from the file's PNGs and JPEGs
// by OBS's own reader, each shrunk the way its materials use it. On any
// thread.
std::vector<ModelPicture> read_pictures(const Model &m);

// A character file loaded on a thread of its own (big VRMs take a moment),
// so OBS never waits: asked for by path, picked up once it's ready.
struct ModelPack {
	std::string path;
	std::shared_ptr<const Model> model; // null: none, or it didn't load
	std::vector<ModelPicture> pictures;
	std::string problem;
};
class ModelLoader {
public:
	ModelLoader();
	~ModelLoader();
	ModelLoader(const ModelLoader &) = delete;
	ModelLoader &operator=(const ModelLoader &) = delete;
	// "" for none. The same path again does nothing, unless the file's
	// changed since (`again`: load it anew anyway).
	void want(const std::string &path, bool again = false);
	// The newest one loaded since the last call, or null.
	std::shared_ptr<ModelPack> take();
	bool busy() const;

private:
	void run();
	mutable std::mutex lock_;
	std::condition_variable wake_;
	std::string want_, asked_, stamp_;
	bool pending_ = false, stop_ = false, working_ = false;
	std::shared_ptr<ModelPack> done_;
	std::thread thread_;
};

struct View3D {
	uint32_t w = 0, h = 0; // the picture made
	float fov = 0.35f;     // radians, top to bottom
	float near = 0.1f, far = 1000;
	// The world -> the camera's view (x right, y up, looking along -z).
	M4 view;
	// Model units -> world units (outlines are in model units).
	float world_scale = 1;
	// 0 as its materials say, 1 glossy, 2 cartoon, 3 flat.
	int look = 0;
	// A cartoon outline round every part (a share of its height, 0 none),
	// as well as any its materials have.
	float outline = 0;
	float height = 1; // the character's height, in world units
	bool smooth = true;
	// The hidden head shape: on your head, as the face's pose (the
	// face's centimetres -> the world) puts it.
	bool hide_behind_head = false;
	M4 head;
	float opacity = 1;
	Lights lights;
	// Its normal and occlusion maps used; soft skin (0 to 1) on the
	// materials `skin` says are skin (skin_materials; null: none).
	bool detail = true;
	float soft_skin = 0;
	const std::vector<bool> *skin = nullptr;
};

class Avatar3D {
public:
	// On OBS's graphics thread, as is everything here.
	Avatar3D();
	~Avatar3D();
	Avatar3D(const Avatar3D &) = delete;
	Avatar3D &operator=(const Avatar3D &) = delete;

	bool ready() const { return effect_ != nullptr; }
	// A new character (or none) onto the graphics card.
	void set(std::shared_ptr<const Model> model, std::vector<ModelPicture> pictures);
	const Model *model() const { return model_.get(); }
	// The posed character drawn, premultiplied, see-through round it; null
	// if there's nothing to draw.
	gs_texture_t *draw(const Rig &rig, const View3D &v);

private:
	struct Part {
		gs_vertbuffer_t *vb = nullptr;
		gs_indexbuffer_t *ib = nullptr;
		size_t points = 0;
		bool bumped = false; // its points carry tangents (its normal map's)
	};
	void clear();
	void make_head();
	void set_camera();
	void set_lights(const View3D &v);
	// front_only: a part drawn only from its front (its edges' facings are
	// laid side-on, not turned round: see the effect's towards_camera).
	void set_material(const Material &m, const View3D &v, bool outline, bool skin, bool front_only);
	void draw_part(size_t i, const Rig::Part &p, const char *technique);

	gs_effect_t *effect_ = nullptr;
	gs_eparam_t *p_cam = nullptr, *p_base_map = nullptr, *p_shade_map = nullptr, *p_emissive_map = nullptr,
		    *p_matcap_map = nullptr, *p_metal_map = nullptr, *p_image = nullptr, *p_base = nullptr,
		    *p_emissive = nullptr, *p_surface = nullptr, *p_toon = nullptr, *p_shade = nullptr,
		    *p_rim = nullptr, *p_matcap = nullptr, *p_look = nullptr, *p_uv = nullptr,
		    *p_outline_width = nullptr, *p_outline_colour = nullptr, *p_texel = nullptr,
		    *p_normal_map = nullptr, *p_occlusion_map = nullptr, *p_detail = nullptr, *p_key_dir = nullptr,
		    *p_key_colour = nullptr, *p_fill_dir = nullptr, *p_fill_colour = nullptr, *p_room_up = nullptr,
		    *p_room_down = nullptr, *p_rim_light = nullptr, *p_key_tint = nullptr, *p_room_tint = nullptr;
	EffectCheck check_;
	M4 cam_;
	gs_texture_t *white_ = nullptr, *black_ = nullptr, *flat_ = nullptr;
	std::shared_ptr<const Model> model_;
	std::vector<gs_texture_t *> textures_;
	std::vector<Part> parts_;
	Part head_;
	gs_texrender_t *scene_ = nullptr, *out_ = nullptr;
	float opacity_ = 1;
	float outline_share_ = 0, height_ = 1, world_scale_ = 1, fov_ = 0.35f;
	float part_distance_ = 1;
};

} // namespace direbot::face
