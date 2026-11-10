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
#include <map>
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

// Something 3D drawn with a character or over one - a headpat's hand,
// something thrown - as its file has it (not posed: each part where its
// node puts it), its shapes (morph targets) set by name, put anywhere,
// any size, mirrored or not, its materials tinted. Made, placed and drawn
// on OBS's graphics thread.
class Prop3D {
public:
	Prop3D(std::shared_ptr<const Model> model, const std::vector<ModelPicture> &pictures);
	~Prop3D();
	Prop3D(const Prop3D &) = delete;
	Prop3D &operator=(const Prop3D &) = delete;

	const Model &model() const { return *model_; }
	// The box round it as made (its file's units), and the middle of it.
	V3 low() const { return low_; }
	V3 high() const { return high_; }
	// A shape's number by its name (-1: none), and how much it shows (0 to 1).
	int shape(const std::string &name) const;
	void set_shape(int shape, float amount);
	// Its material's colour times this (linear: 1, 1, 1 as made).
	void tint(int material, V3 times);
	// Where it is: its file's units -> the world (mirrored if it turns it
	// inside out), and how much it shows (0 to 1). Its points worked out
	// now, for the next draw (Avatar3D::draw does it, for each PropAt).
	void place(const M4 &to_world, float opacity = 1);
	float opacity() const { return opacity_; }
	bool mirrored() const { return handed_ < 0; }

private:
	friend class Avatar3D;
	struct Part {
		const Primitive *prim = nullptr;
		std::vector<V3> pos, normal, tangent; // as made, in its file's space
		std::vector<std::vector<V3>> morph_pos, morph_normal;
		gs_vertbuffer_t *vb = nullptr;
		gs_indexbuffer_t *ib = nullptr;
		size_t points = 0;
		bool bumped = false;
		float distance = 1;
		V3 middle; // its points' middle, as made (a glowing part's depth)
	};
	std::shared_ptr<const Model> model_;
	std::vector<gs_texture_t *> textures_;
	std::vector<Part> parts_;
	std::vector<std::string> shape_names_;
	std::vector<float> shapes_;
	std::vector<V3> tints_;
	V3 low_, high_;
	M4 place_;
	float opacity_ = 1, handed_ = 1;
};

// Something 3D to draw: where (its file's units -> the world), how much
// it shows, and its shapes (by Prop3D::shape's numbers; those left out
// as made).
struct PropAt {
	Prop3D *prop = nullptr;
	M4 place;
	float opacity = 1;
	std::vector<float> shapes;
};

// Things 3D (a headpat's hand, things to throw) loaded on a thread of
// their own, so OBS never waits: asked for as a list of files, each
// picked up once it's ready. One too detailed to draw quickly is said so.
constexpr size_t MOST_PROP_POINTS = 400000;
class PropLoader {
public:
	PropLoader();
	~PropLoader();
	PropLoader(const PropLoader &) = delete;
	PropLoader &operator=(const PropLoader &) = delete;
	// The files wanted now: each not loaded yet (or changed since) is
	// loaded, in turn.
	void want(const std::vector<std::string> &paths);
	// Those loaded since the last call.
	std::vector<std::shared_ptr<ModelPack>> take();

private:
	void run();
	std::mutex lock_;
	std::condition_variable wake_;
	std::vector<std::string> want_;
	std::map<std::string, std::string> stamps_; // loaded (or tried): its file's stamp then
	std::vector<std::shared_ptr<ModelPack>> done_;
	bool stop_ = false;
	std::thread thread_;
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
	// Another posed character drawn in the same picture (0.9.0: a companion
	// beside it, a wolf sitting by you): its own Avatar3D holds it on the
	// graphics card, posed by its own rig; seen with the same camera and
	// lights, each hiding the other where it's in front.
	struct Other {
		Avatar3D *avatar = nullptr;
		const Rig *rig = nullptr;
	};
	// The posed character drawn, premultiplied, see-through round it, with
	// anything 3D put with it (hidden where it's behind the character, as
	// the character is where they're in front of it); null if there's
	// nothing to draw. With no character (`rig` null): only those things.
	// Parts a rig hides (Rig::Part::hidden) aren't drawn.
	gs_texture_t *draw(const Rig *rig, const View3D &v, const std::vector<PropAt> &props = {},
			   const std::vector<Other> &others = {});

private:
	struct Part {
		gs_vertbuffer_t *vb = nullptr;
		gs_indexbuffer_t *ib = nullptr;
		size_t points = 0;
		bool bumped = false; // its points carry tangents (its normal map's)
		bool strands = false; // hair strands (model.hpp's Primitive::strands)
	};
	void clear();
	// The posed points onto the graphics card (0.9.0: parts it hides left
	// as they were); whether `rig` is this one's character.
	bool upload(const Rig *rig);
	void make_head();
	void set_camera();
	// 0.9.0: every point of the next part drawn at this point's depth (a
	// glowing part's middle, in the world), or (null) as they are.
	void set_flat(const V3 *at);
	void set_lights(const View3D &v);
	// front_only: a part drawn only from its front (its edges' facings are
	// laid side-on, not turned round: see the effect's towards_camera).
	// textures: the pictures it's drawn with (null: the character's).
	void set_material(const Material &m, const View3D &v, bool outline, bool skin, bool front_only,
			  const std::vector<gs_texture_t *> *textures = nullptr);
	void draw_part(size_t i, const Rig::Part &p, const char *technique);
	// Hair strands: the camera, and how big a pixel is (set_material first).
	void set_strand(const View3D &v, uint32_t drawn_w, uint32_t drawn_h);
	// A thing's parts: its solid ones, or its see-through ones (or all of
	// it, while it fades).
	void draw_prop(Prop3D &prop, const View3D &v, bool see_through);

	gs_effect_t *effect_ = nullptr;
	gs_eparam_t *p_cam = nullptr, *p_base_map = nullptr, *p_shade_map = nullptr, *p_emissive_map = nullptr,
		    *p_matcap_map = nullptr, *p_metal_map = nullptr, *p_image = nullptr, *p_base = nullptr,
		    *p_emissive = nullptr, *p_surface = nullptr, *p_toon = nullptr, *p_shade = nullptr,
		    *p_rim = nullptr, *p_matcap = nullptr, *p_look = nullptr, *p_uv = nullptr,
		    *p_outline_width = nullptr, *p_outline_colour = nullptr, *p_texel = nullptr,
		    *p_normal_map = nullptr, *p_occlusion_map = nullptr, *p_detail = nullptr, *p_key_dir = nullptr,
		    *p_key_colour = nullptr, *p_fill_dir = nullptr, *p_fill_colour = nullptr, *p_room_up = nullptr,
		    *p_room_down = nullptr, *p_rim_light = nullptr, *p_key_tint = nullptr, *p_room_tint = nullptr,
		    *p_glow = nullptr, *p_flat_at = nullptr, *p_eye = nullptr, *p_strand = nullptr;
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
