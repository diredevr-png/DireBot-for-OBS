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

#include "avatar3d.hpp"
#include "card-picture.hpp"
#include "files.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include <obs-module.h>
#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>

#include <plugin-support.h>

namespace direbot::face {

namespace {

// Drawn twice the size for smooth edges, unless that's past this.
constexpr uint32_t MOST_DRAWN = 8192;

std::string extension_of(const ModelImage &img)
{
	const auto &b = img.bytes;
	if (b.size() >= 4 && b[0] == 0x89 && b[1] == 'P')
		return ".png";
	if (b.size() >= 3 && b[0] == 0xFF && b[1] == 0xD8)
		return ".jpg";
	if (b.size() >= 12 && std::memcmp(b.data(), "RIFF", 4) == 0 && std::memcmp(b.data() + 8, "WEBP", 4) == 0)
		return ".webp";
	if (img.mime == "image/jpeg")
		return ".jpg";
	return ".png";
}

void srgb_bytes(const float *c, uint8_t *out)
{
	for (int k = 0; k < 3; k++) {
		const float v = std::min(1.0f, std::max(0.0f, c[k]));
		out[k] = (uint8_t)std::lround(std::pow(v, 1 / 2.2f) * 255);
	}
	out[3] = (uint8_t)std::lround(std::min(1.0f, std::max(0.0f, c[3])) * 255);
}

matrix4 obs_matrix(const M4 &m)
{
	// Column after column with points as columns is, in memory, OBS's
	// rows with points as rows: the same numbers.
	matrix4 out;
	std::memcpy(&out, m.m, sizeof(float) * 16);
	return out;
}

// The side of a part OBS leaves out, for the sides that show: OBS's names
// for glTF's back and front as they're drawn here (found by drawing:
// tests/test_real_face.py's test_one_sided_parts_show_only_their_fronts).
gs_cull_mode cull_of(const Material &m, float handed)
{
	switch (sides_shown(m, handed)) {
	case Sides::Front:
		return GS_BACK;
	case Sides::Back:
		return GS_FRONT;
	default:
		return GS_NEITHER;
	}
}

void set_vec4(gs_eparam_t *p, float x, float y, float z, float w)
{
	vec4 v;
	vec4_set(&v, x, y, z, w);
	gs_effect_set_vec4(p, &v);
}
void set_vec3(gs_eparam_t *p, const float *c)
{
	vec3 v;
	vec3_set(&v, c[0], c[1], c[2]);
	gs_effect_set_vec3(p, &v);
}

} // namespace

std::vector<ModelPicture> read_pictures(const Model &m)
{
	static std::atomic<unsigned> counter{0};
	std::vector<ModelPicture> out(m.images.size());
	const std::vector<PictureUse> uses = picture_uses(m);
	std::error_code ec;
	const std::filesystem::path folder = std::filesystem::temp_directory_path(ec);
	if (ec)
		return out;
	std::random_device rd;
	const unsigned tag = rd();
	for (size_t i = 0; i < m.images.size(); i++) {
		const ModelImage &img = m.images[i];
		if (img.bytes.empty())
			continue;
		// OBS reads pictures from files, so each goes through one for a moment.
		const std::filesystem::path path = folder / ("direbot-face-" + std::to_string(tag) + "-" +
							     std::to_string(counter++) + extension_of(img));
		{
			std::ofstream f(path, std::ios::binary);
			if (!f)
				continue;
			f.write(reinterpret_cast<const char *>(img.bytes.data()), (std::streamsize)img.bytes.size());
		}
		gs_color_format format = GS_UNKNOWN;
		uint32_t w = 0, h = 0;
		// OBS reads its path as UTF-8 (path.string() is the PC's old code
		// page on Windows: another file in a folder named in other letters).
		uint8_t *data = gs_create_texture_file_data(path.u8string().c_str(), &format, &w, &h);
		std::filesystem::remove(path, ec);
		if (!data)
			continue;
		if ((format == GS_RGBA || format == GS_BGRA || format == GS_BGRX) && w && h && w <= 16384 &&
		    h <= 16384) {
			const bool bgr = format != GS_RGBA;
			std::vector<uint8_t> rgba((size_t)w * h * 4);
			for (size_t p = 0; p < (size_t)w * h; p++) {
				const uint8_t *s = data + p * 4;
				uint8_t *d = &rgba[p * 4];
				d[0] = bgr ? s[2] : s[0];
				d[1] = s[1];
				d[2] = bgr ? s[0] : s[2];
				d[3] = format == GS_BGRX ? 255 : s[3];
			}
			out[i] = picture_from(rgba.data(), w, h, uses[i]);
		}
		bfree(data);
	}
	return out;
}

ModelLoader::ModelLoader() : thread_([this] { run(); }) {}

ModelLoader::~ModelLoader()
{
	{
		std::lock_guard<std::mutex> l(lock_);
		stop_ = true;
	}
	wake_.notify_all();
	thread_.join();
}

void ModelLoader::want(const std::string &path, bool again, const LoadOptions &options)
{
	// The same file changed since (made again in its place): loaded anew.
	const std::string stamp = file_stamp(path);
	{
		std::lock_guard<std::mutex> l(lock_);
		if (path == asked_ && stamp == stamp_ && options == options_ && !again)
			return;
		asked_ = path;
		stamp_ = stamp;
		options_ = options;
		want_ = path;
		pending_ = true;
	}
	wake_.notify_all();
}

std::shared_ptr<ModelPack> ModelLoader::take()
{
	std::lock_guard<std::mutex> l(lock_);
	return std::move(done_);
}

bool ModelLoader::busy() const
{
	std::lock_guard<std::mutex> l(lock_);
	return pending_ || working_;
}

void ModelLoader::run()
{
	for (;;) {
		std::string path;
		LoadOptions options;
		{
			std::unique_lock<std::mutex> l(lock_);
			wake_.wait(l, [this] { return stop_ || pending_; });
			if (stop_)
				return;
			path = want_;
			options = options_;
			pending_ = false;
			working_ = true;
		}
		auto pack = std::make_shared<ModelPack>();
		pack->path = path;
		if (!path.empty()) {
			auto model = std::make_shared<Model>();
			std::string problem;
			if (load_model(path, *model, &problem, options)) {
				pack->pictures = read_pictures(*model);
				pack->model = model;
			} else {
				pack->problem = problem;
				obs_log(LOG_WARNING, "DireBot Face Avatar: the 3D character didn't load: %s",
					problem.c_str());
			}
		}
		std::lock_guard<std::mutex> l(lock_);
		working_ = false;
		// A newer ask while this one loaded: this one's dropped.
		if (!pending_)
			done_ = pack;
	}
}

PropLoader::PropLoader() : thread_([this] { run(); }) {}

PropLoader::~PropLoader()
{
	{
		std::lock_guard<std::mutex> l(lock_);
		stop_ = true;
	}
	wake_.notify_all();
	thread_.join();
}

void PropLoader::want(const std::vector<std::string> &paths)
{
	{
		std::lock_guard<std::mutex> l(lock_);
		want_ = paths;
		// One let go of is loaded anew if it's wanted again.
		for (auto it = stamps_.begin(); it != stamps_.end();)
			if (std::find(paths.begin(), paths.end(), it->first) == paths.end())
				it = stamps_.erase(it);
			else
				++it;
	}
	wake_.notify_all();
}

std::vector<std::shared_ptr<ModelPack>> PropLoader::take()
{
	std::lock_guard<std::mutex> l(lock_);
	std::vector<std::shared_ptr<ModelPack>> out;
	out.swap(done_);
	return out;
}

void PropLoader::run()
{
	for (;;) {
		std::string path, stamp;
		{
			std::unique_lock<std::mutex> l(lock_);
			// The next one wanted that isn't loaded as it is now.
			auto next = [&] {
				for (const std::string &p : want_) {
					if (p.empty())
						continue;
					const std::string s = file_stamp(p);
					auto it = stamps_.find(p);
					if (it == stamps_.end() || it->second != s) {
						path = p;
						stamp = s;
						return true;
					}
				}
				return false;
			};
			wake_.wait(l, [&] { return stop_ || next(); });
			if (stop_)
				return;
			stamps_[path] = stamp;
		}
		auto pack = std::make_shared<ModelPack>();
		pack->path = path;
		auto model = std::make_shared<Model>();
		std::string problem;
		// (Hair strands in a thing aren't drawn: none kept.)
		LoadOptions options;
		options.strand_points = options.strand_points_bare = 0;
		if (load_model(path, *model, &problem, options)) {
			if (model->points > MOST_PROP_POINTS) {
				pack->problem = "it's too detailed to throw (" + std::to_string(model->points) +
						" points; at most " + std::to_string(MOST_PROP_POINTS) + ")";
			} else {
				pack->pictures = read_pictures(*model);
				pack->model = model;
			}
		} else {
			pack->problem = problem.empty() ? "it couldn't be read" : problem;
		}
		if (!pack->problem.empty())
			obs_log(LOG_WARNING, "DireBot Face Avatar: a 3D thing didn't load: %s", pack->problem.c_str());
		std::lock_guard<std::mutex> l(lock_);
		done_.push_back(pack);
	}
}

Avatar3D::Avatar3D()
{
	// On Windows (Direct3D 11) the effect that also poses characters on
	// the graphics card; OpenGL can't be sent its bones (only the first of
	// a list of numbers goes), so there - or if it won't load - the plain
	// one, the CPU posing them.
	const bool d3d = gs_get_device_type() == GS_DEVICE_DIRECT3D_11;
	for (const char *file : {"effects/direbot-face-3d-skin.effect", "effects/direbot-face-3d.effect"}) {
		if (effect_ || (!d3d && std::strstr(file, "-skin")))
			continue;
		char *path = obs_module_file(file);
		if (!path)
			continue;
		char *errors = nullptr;
		effect_ = gs_effect_create_from_file(path, &errors);
		if (!effect_)
			obs_log(LOG_WARNING, "DireBot Face Avatar: the 3D effect (%s) didn't load: %s", file,
				errors ? errors : "(no reason given)");
		bfree(errors);
		bfree(path);
	}
	if (effect_) {
		auto param = [this](const char *name) {
			return gs_effect_get_param_by_name(effect_, name);
		};
		p_cam = param("cam");
		p_base_map = param("base_map");
		p_shade_map = param("shade_map");
		p_emissive_map = param("emissive_map");
		p_matcap_map = param("matcap_map");
		p_metal_map = param("metal_map");
		p_image = param("image");
		p_base = param("base_colour");
		p_emissive = param("emissive");
		p_surface = param("surface");
		p_toon = param("toon");
		p_shade = param("shade_colour");
		p_rim = param("rim_colour");
		p_matcap = param("matcap_colour");
		p_look = param("look");
		p_uv = param("uv_place");
		p_outline_width = param("outline_width");
		p_outline_colour = param("outline_colour");
		p_texel = param("texel");
		p_normal_map = param("normal_map");
		p_occlusion_map = param("occlusion_map");
		p_detail = param("detail");
		p_key_dir = param("key_dir");
		p_key_colour = param("key_colour");
		p_fill_dir = param("fill_dir");
		p_fill_colour = param("fill_colour");
		p_room_up = param("room_up");
		p_room_down = param("room_down");
		p_rim_light = param("rim_light");
		p_key_tint = param("key_tint");
		p_room_tint = param("room_tint");
		p_glow = param("gas");
		p_flat_at = param("flat_at");
		p_eye = param("eye_pos");
		p_strand = param("strand");
		p_strand_lod = param("strand_lod");
		p_strand_view = param("strand_view");
		p_depth_map = param("depth_map");
		p_spots = param("spot_moves");
		p_bones = param("bone_cols");
		p_skin = param("skin_info");
		// Posing on the graphics card, where the effect has it (an older
		// effect file beside a newer add-on: as before).
		// (DIREBOT_FACE_CPU_POSE=1: posed by the CPU anyway - the tests
		// compare the two.)
		const char *cpu = std::getenv("DIREBOT_FACE_CPU_POSE");
		can_gpu_skin_ = gs_effect_get_technique(effect_, "DrawSk") != nullptr && p_bones && p_skin &&
				!(cpu && *cpu && *cpu != '0');
		gpu_skin_ = can_gpu_skin_;
		check_.load(effect_, "3D", can_gpu_skin_ ? face_3d_skin_needs() : face_3d_needs());
	}
	// Plain pictures for a part without its own: white, black, and a
	// normal map with no bumps (straight out: 128, 128, 255).
	const uint8_t white[4] = {255, 255, 255, 255}, black[4] = {0, 0, 0, 255}, flat[4] = {128, 128, 255, 255};
	const uint8_t *w = white, *b = black, *f = flat;
	white_ = gs_texture_create(1, 1, GS_RGBA, 1, &w, 0);
	black_ = gs_texture_create(1, 1, GS_RGBA, 1, &b, 0);
	flat_ = gs_texture_create(1, 1, GS_RGBA, 1, &f, 0);
	make_head();
#ifdef _WIN32
	// (The graphics card lost: see clocks_lost.)
	gs_device_loss lost = {};
	lost.device_loss_release = [](void *data) {
		static_cast<Avatar3D *>(data)->clocks_lost();
	};
	lost.device_loss_rebuild = [](void *, void *) {
	};
	lost.data = this;
	gs_register_loss_callbacks(&lost);
#endif
}

Avatar3D::~Avatar3D()
{
#ifdef _WIN32
	gs_unregister_loss_callbacks(this);
#endif
	clear();
	gs_vertexbuffer_destroy(head_.vb);
	gs_indexbuffer_destroy(head_.ib);
	gs_texture_destroy(white_);
	gs_texture_destroy(black_);
	gs_texture_destroy(flat_);
	gs_texrender_destroy(scene_);
	gs_texrender_destroy(out_);
	gs_texrender_destroy(depth_);
	for (HairClock &c : clocks_) {
		if (c.depth)
			gs_timer_destroy(c.depth);
		if (c.hair)
			gs_timer_destroy(c.hair);
		if (c.range)
			gs_timer_range_destroy(c.range);
	}
	gs_effect_destroy(effect_);
}

void Avatar3D::set_hair(const HairPlan &plan)
{
	hair_plan_ = plan;
	budget_.set_rule(plan.budget_rule);
	budget_.set_limit(plan.budget_ms);
}

void Avatar3D::clear()
{
	for (gs_texture_t *t : textures_)
		gs_texture_destroy(t);
	textures_.clear();
	for (Part &p : parts_)
		free_part(p);
	parts_.clear();
	bone_cols_.clear();
	model_.reset();
}

void Avatar3D::free_part(Part &part)
{
	gs_vertexbuffer_destroy(part.vb);
	gs_indexbuffer_destroy(part.ib);
	gs_indexbuffer_destroy(part.ib_lines);
	part.vb = nullptr;
	part.ib = part.ib_lines = nullptr;
	for (auto &sort : part.laid)
		for (Part::Laid &l : sort) {
			gs_vertexbuffer_destroy(l.vb);
			gs_indexbuffer_destroy(l.ib);
			l = Part::Laid();
		}
	for (auto &from : part.laid_from)
		from.clear();
	gs_vertexbuffer_destroy(part.hull.vb);
	gs_indexbuffer_destroy(part.hull.ib);
	part.hull = Part::Laid();
	part.hull_lay = StrandHull();
}

namespace {

// A buffer of `n` points (with tangents: `tangents`); a still one (not
// `dynamic`) is filled from `points` now, as OBS keeps no copy of it to
// change later.
gs_vertbuffer_t *make_vb(size_t n, bool dynamic, const V3 *points = nullptr, bool tangents = false, bool skin = false)
{
	gs_vb_data *vb = gs_vbdata_create();
	vb->num = n;
	vb->points = static_cast<vec3 *>(bzalloc(sizeof(vec3) * n));
	vb->normals = static_cast<vec3 *>(bzalloc(sizeof(vec3) * n));
	if (tangents)
		vb->tangents = static_cast<vec3 *>(bzalloc(sizeof(vec3) * n));
	for (size_t i = 0; points && i < n; i++) {
		vec3_set(&vb->points[i], points[i].x, points[i].y, points[i].z);
		vec3_set(&vb->normals[i], 0, 0, 1);
	}
	vb->colors = static_cast<uint32_t *>(bzalloc(sizeof(uint32_t) * n));
	// Posed on the graphics card: each point's 4 bones and how much each
	// moves it (after its UVs).
	vb->num_tex = 1 + (skin ? 2 : 0);
	vb->tvarray = static_cast<gs_tvertarray *>(bzalloc(sizeof(gs_tvertarray) * vb->num_tex));
	for (size_t t = 0; t < vb->num_tex; t++) {
		vb->tvarray[t].width = t < 1u ? 2 : 4;
		vb->tvarray[t].array = bzalloc(sizeof(float) * vb->tvarray[t].width * n);
	}
	return gs_vertexbuffer_create(vb, dynamic ? GS_DYNAMIC : 0);
}

gs_indexbuffer_t *make_ib(const std::vector<uint32_t> &index)
{
	uint32_t *ix = static_cast<uint32_t *>(bmalloc(sizeof(uint32_t) * index.size()));
	std::memcpy(ix, index.data(), sizeof(uint32_t) * index.size());
	return gs_indexbuffer_create(GS_UNSIGNED_LONG, ix, index.size(), 0);
}

// A direction folded onto a square (two numbers; the effect's oct_dir
// unfolds it): the way a strand runs, small enough to ride with a hair
// point's other numbers.
void folded(V3 n, float *e)
{
	const float l = std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z);
	if (!(l > 0)) {
		e[0] = e[1] = 0;
		return;
	}
	float x = n.x / l, y = n.y / l;
	if (n.z < 0) {
		const float ox = (1 - std::fabs(y)) * (x >= 0 ? 1.0f : -1.0f);
		const float oy = (1 - std::fabs(x)) * (y >= 0 ? 1.0f : -1.0f);
		x = ox;
		y = oy;
	}
	e[0] = x;
	e[1] = y;
}

} // namespace

// A hair-strand part (model.hpp's Strands) onto the graphics card, as the
// effect's Strand techniques read it. Each of its points twice, its two
// sides:
//   points   x y z, and w: + or - (which side) 1 and its strand's key
//            and rate as one whole number (strand_order)
//   normals  x y: the way its strand runs, folded; z: its width; w: its
//            expression spot's number plus how far along its strand it
//            is (0 root to 0.9995 tip)
//   colours  its own
//   (posed on the card) one set of 4: each of its 4 bones' number plus
//            how much it moves the point (0 to 0.999)
// 52 bytes a side on the card, 36 posed here; then its pieces, 6 corners
// each, in the order they're drawn in (Strands::pieces). Posed on the
// card it never changes; posed here its places and runs are sent each
// frame (upload).
// A strand's key and rate (Strands::key, rate) as the one number the
// graphics card is told (the effect's strand_out takes it apart): its
// key in steps of 1 / 32768 (under 4: 131072 of them), and its rate's
// step x 131072 - a whole number under 2^23, which a float holds exactly.
static float strand_order(const Strands &h, size_t s)
{
	const float key = s < h.key.size() ? h.key[s] : (float)((double)s / (double)std::max<uint32_t>(1, h.in_file));
	const float rate = s < h.rate.size() ? h.rate[s] : 1.0f;
	const float steps = std::min(131071.0f, std::max(0.0f, std::floor(key * 32768.0f)));
	return steps + 131072.0f * (float)Strands::rate_step(rate);
}

namespace {

// A strand part's points as the card is given them (make_strand_part's
// list): corner `k` of a buffer filled with point `i`'s numbers, its
// first side or its second.
struct StrandFill {
	const Primitive &p;
	const Strands &h;
	gs_vb_data *vb;
	float *jw; // (posed on the card: its bones; else null)
	bool boned, spots, coloured;
	std::vector<float> order; // a point: its strand's number (1 + strand_order)

	StrandFill(const Primitive &prim, bool bones)
		: p(prim), h(prim.hair), vb(nullptr), jw(nullptr),
		  boned(bones && prim.joints.size() == prim.pos.size() * 4 &&
			prim.weights.size() == prim.pos.size() * 4),
		  spots(prim.hair.anchor.size() == prim.pos.size()),
		  coloured(prim.hair.colour.size() == prim.pos.size() * 4), order(prim.pos.size(), 1.0f)
	{
		for (size_t s = 0; s < h.strands(); s++) {
			const float o = 1.0f + strand_order(h, s);
			for (uint32_t i = h.first[s]; i < h.first[s + 1] && i < order.size(); i++)
				order[i] = o;
		}
	}
	// A buffer of `count` corners to fill (with their bones: `skin`).
	void make(size_t count, bool skin)
	{
		vb = gs_vbdata_create();
		vb->num = count;
		vb->points = static_cast<vec3 *>(bzalloc(sizeof(vec3) * count));
		vb->normals = static_cast<vec3 *>(bzalloc(sizeof(vec3) * count));
		vb->colors = static_cast<uint32_t *>(bzalloc(sizeof(uint32_t) * count));
		jw = nullptr;
		if (skin) {
			vb->num_tex = 1;
			vb->tvarray = static_cast<gs_tvertarray *>(bzalloc(sizeof(gs_tvertarray)));
			vb->tvarray[0].width = 4;
			vb->tvarray[0].array = bzalloc(sizeof(float) * 4 * count);
			jw = static_cast<float *>(vb->tvarray[0].array);
		}
	}
	void corner(size_t k, uint32_t i, bool second) const
	{
		static const uint8_t white[4] = {255, 255, 255, 255};
		float e[2];
		folded(p.normal[i], e);
		const float along =
			std::min(1.0f, std::max(0.0f, h.along[i])) * 0.9995f + (spots ? (float)h.anchor[i] : 0.0f);
		vec3_set(&vb->points[k], p.pos[i].x, p.pos[i].y, p.pos[i].z);
		vb->points[k].w = second ? order[i] : -order[i];
		vec3_set(&vb->normals[k], e[0], e[1], h.width[i]);
		vb->normals[k].w = along;
		std::memcpy(&vb->colors[k], coloured ? &h.colour[(size_t)i * 4] : white, 4);
		// Its bones (none: its node's world as bone 0, all of it).
		for (int j = 0; jw && j < 4; j++)
			jw[k * 4 + j] = boned ? (float)std::min<uint16_t>(254, p.joints[(size_t)i * 4 + j]) +
							std::min(1.0f, p.weights[(size_t)i * 4 + j]) * 0.999f
					      : (j == 0 ? 0.999f : 0.0f);
	}
};

} // namespace

void Avatar3D::make_strand_part(Part &part, const Primitive &p, bool boned)
{
	const Strands &h = p.hair;
	const size_t n = p.pos.size(), count = n * 2;
	if (!n || h.pieces.empty() || h.width.size() != n || h.along.size() != n || p.normal.size() != n)
		return;
	StrandFill fill(p, boned);
	part.boned = fill.boned;
	fill.make(count, part.skin);
	for (uint32_t i = 0; i < n; i++) {
		fill.corner((size_t)i * 2, i, false);
		fill.corner((size_t)i * 2 + 1, i, true);
	}
	part.vb = gs_vertexbuffer_create(fill.vb, part.skin ? 0 : GS_DYNAMIC);
	part.points = count;
	uint32_t *ix = static_cast<uint32_t *>(bmalloc(sizeof(uint32_t) * h.pieces.size() * 6));
	strand_corners(h.pieces.data(), nullptr, h.pieces.size(), false, ix);
	part.ib = gs_indexbuffer_create(GS_UNSIGNED_LONG, ix, h.pieces.size() * 6, 0);
}

// A strand part's pieces of one sort, laid out as they're drawn (posed on
// the card; HairPlan::layout 1). The card reads a piece's corners where
// they lie in its memory: by point (make_strand_part) a run's pieces -
// one layer of a group, a few points of each strand - are far apart, and
// each is fetched on its own (measured on a Radeon RX 9070 XT, 11-11: a
// coat's time went with how much memory its corners were spread over, not
// with how many pieces showed). Here a run's corners follow each other in
// the order its pieces are drawn in (Strands::pieces), each sort's apart:
// a point two pieces running on from each other share is there once, and
// a point of two runs once in each. As ribbons both its sides; as lines
// only its first (a line's corners are all first sides). The first so
// many strands' pieces are still the start of each run's stretch.
bool Avatar3D::lay_strands(Part &part, const Primitive &p, int sort, bool lines)
{
	Part::Laid &l = part.laid[sort ? 1 : 0][lines ? 1 : 0];
	if (l.tried)
		return l.none || (l.vb && l.ib);
	l.tried = true;
	const Strands &h = p.hair;
	const size_t n = p.pos.size();
	if (!part.skin || !n || h.pieces.empty() || h.runs.size() < 2 || h.width.size() != n || h.along.size() != n ||
	    p.normal.size() != n)
		return false;
	const auto began = std::chrono::steady_clock::now();
	StrandLayout lay = strand_layout(h, sort ? 1 : 0, lines);
	part.laid_from[sort ? 1 : 0] = std::move(lay.from);
	if (lay.corners.empty()) {
		l.none = true;
		return true;
	}
	const size_t sides = lines ? 1 : 2, points = lay.points.size();
	if (points * sides > 0xFFFFFFF0u)
		return false;
	StrandFill fill(p, part.boned);
	fill.make(points * sides, true);
	for (size_t k = 0; k < points; k++)
		for (size_t side = 0; side < sides; side++)
			fill.corner(k * sides + side, lay.points[k], side == 1);
	l.vb = gs_vertexbuffer_create(fill.vb, 0);
	l.ib = make_ib(lay.corners);
	if (!l.vb || !l.ib) {
		gs_vertexbuffer_destroy(l.vb);
		gs_indexbuffer_destroy(l.ib);
		l.vb = nullptr;
		l.ib = nullptr;
		return false;
	}
	// (52 bytes a corner, 4 a corner of a piece.)
	l.bytes = points * sides * 52 + lay.corners.size() * 4;
	l.ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - began).count();
	return true;
}

// A strand part's hull laid out as it's drawn (posed on the card;
// HairPlan::layout 1; hair.hpp's strand_hull): by point its pieces - a few
// points of every fourth strand, a layer at a time - are as far apart in
// the card's memory as a run's are, and each group's each layer is a draw
// of its own. Here a group's pieces follow each other by strand, a draw a
// group. The same corners as by point, so the same picture.
bool Avatar3D::lay_hull(Part &part, const Primitive &p)
{
	Part::Laid &l = part.hull;
	if (l.tried)
		return l.none || (l.vb && l.ib);
	l.tried = true;
	const Strands &h = p.hair;
	const size_t n = p.pos.size();
	if (!part.skin || !n || h.pieces.empty() || h.runs.size() < 2 || h.width.size() != n || h.along.size() != n ||
	    p.normal.size() != n)
		return false;
	const auto began = std::chrono::steady_clock::now();
	StrandHull lay = strand_hull(h);
	if (lay.corners.empty()) {
		l.none = true;
		return true;
	}
	const size_t points = lay.points.size();
	if (points * 2 > 0xFFFFFFF0u)
		return false;
	StrandFill fill(p, part.boned);
	fill.make(points * 2, true);
	for (size_t k = 0; k < points; k++)
		for (size_t side = 0; side < 2; side++)
			fill.corner(k * 2 + side, lay.points[k], side == 1);
	l.vb = gs_vertexbuffer_create(fill.vb, 0);
	l.ib = make_ib(lay.corners);
	if (!l.vb || !l.ib) {
		gs_vertexbuffer_destroy(l.vb);
		gs_indexbuffer_destroy(l.ib);
		l.vb = nullptr;
		l.ib = nullptr;
		return false;
	}
	l.bytes = points * 2 * 52 + lay.corners.size() * 4;
	std::vector<uint32_t>().swap(lay.points);
	std::vector<uint32_t>().swap(lay.corners);
	part.hull_lay = std::move(lay);
	l.ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - began).count();
	return true;
}

// The hidden head shape: a ball the shape of a head, in the face's own
// centimetres (MediaPipe's face at rest: its middle at 0, the eyes 3 in
// front), well behind the face, so neither your face nor a character's
// face worn over it is ever hidden.
void Avatar3D::make_head()
{
	constexpr int RINGS = 12, ROUND = 24;
	const V3 middle{0, -0.5f, -5}, radius{7.4f, 10.2f, 7.6f};
	std::vector<V3> pos;
	std::vector<uint32_t> index;
	for (int r = 0; r <= RINGS; r++) {
		const float a = 3.14159265f * r / RINGS;
		for (int s = 0; s <= ROUND; s++) {
			const float b = 2 * 3.14159265f * s / ROUND;
			pos.push_back(middle + V3{radius.x * std::sin(a) * std::cos(b), radius.y * std::cos(a),
						  radius.z * std::sin(a) * std::sin(b)});
		}
	}
	for (int r = 0; r < RINGS; r++)
		for (int s = 0; s < ROUND; s++) {
			const uint32_t i = (uint32_t)(r * (ROUND + 1) + s), j = i + ROUND + 1;
			index.insert(index.end(), {i, j, i + 1, i + 1, j, j + 1});
		}
	head_.vb = make_vb(pos.size(), false, pos.data());
	head_.ib = make_ib(index);
	head_.points = pos.size();
}

void Avatar3D::set(std::shared_ptr<const Model> model, std::vector<ModelPicture> pictures)
{
	clear();
	model_ = std::move(model);
	if (!model_)
		return;
	for (const ModelPicture &pic : pictures) {
		gs_texture_t *tex = nullptr;
		if (pic.size && !pic.levels.empty()) {
			std::vector<const uint8_t *> data;
			for (const auto &level : pic.levels)
				data.push_back(level.data());
			tex = gs_texture_create(pic.size, pic.size, GS_RGBA, (uint32_t)data.size(), data.data(), 0);
		}
		textures_.push_back(tex);
	}
	make_parts();
}

void Avatar3D::set_gpu_skin(bool on)
{
	const bool now = can_gpu_skin_ && on;
	if (now == gpu_skin_)
		return;
	gpu_skin_ = now;
	// Its parts made again for the new way (its pictures kept).
	for (Part &p : parts_)
		free_part(p);
	parts_.clear();
	bone_cols_.clear();
	if (model_)
		make_parts();
}

void Avatar3D::make_parts()
{
	const Model &m = *model_;
	for (const Node &node : m.nodes) {
		if (node.mesh < 0)
			continue;
		for (const Primitive &p : m.meshes[node.mesh].parts) {
			Part part;
			part.points = p.pos.size();
			part.bumped = p.tangent.size() == part.points * 4;
			part.strands = p.strands;
			const bool card = gpu_skin_ &&
					  (node.skin < 0 || m.skins[node.skin].joints.size() <= Rig::GPU_MOST_BONES);
			if (p.strands) {
				part.skin = card;
				make_strand_part(part, p, node.skin >= 0);
				parts_.push_back(part);
				continue;
			}
			if (part.points && !p.index.empty()) {
				part.skin = card;
				part.vb = make_vb(part.points, true, nullptr, part.bumped, part.skin);
				part.ib = make_ib(p.index);
			}
			if (part.vb) {
				gs_vb_data *d = gs_vertexbuffer_get_data(part.vb);
				float *uv = static_cast<float *>(d->tvarray[0].array);
				for (size_t i = 0; i < part.points; i++) {
					if (!p.uv.empty()) {
						uv[i * 2] = p.uv[i * 2];
						uv[i * 2 + 1] = p.uv[i * 2 + 1];
					}
					uint8_t c[4] = {255, 255, 255, 255};
					if (!p.colour.empty())
						srgb_bytes(&p.colour[i * 4], c);
					std::memcpy(&d->colors[i], c, 4);
				}
				if (part.skin) {
					// Its points as made, and its bones (none: its node's
					// world as bone 0, all of it).
					float *jt = static_cast<float *>(d->tvarray[1].array);
					float *wt = static_cast<float *>(d->tvarray[2].array);
					const bool boned = node.skin >= 0 && p.joints.size() == part.points * 4 &&
							   p.weights.size() == part.points * 4;
					for (size_t i = 0; i < part.points; i++) {
						vec3_set(&d->points[i], p.pos[i].x, p.pos[i].y, p.pos[i].z);
						const V3 n = i < p.normal.size() ? p.normal[i] : V3{0, 0, 1};
						vec3_set(&d->normals[i], n.x, n.y, n.z);
						if (part.bumped && d->tangents) {
							vec3_set(&d->tangents[i], p.tangent[i * 4], p.tangent[i * 4 + 1],
								 p.tangent[i * 4 + 2]);
							d->tangents[i].w = p.tangent[i * 4 + 3];
						}
						for (int k = 0; k < 4; k++) {
							jt[i * 4 + k] = boned ? (float)p.joints[i * 4 + k] : 0.0f;
							wt[i * 4 + k] = boned ? p.weights[i * 4 + k] : (k == 0 ? 1.0f : 0.0f);
						}
					}
				}
				// Its colours and UVs sent now, once (drawing sends only
				// what moves).
				gs_vertexbuffer_flush(part.vb);
			}
			parts_.push_back(part);
		}
	}
}

void Avatar3D::set_camera()
{
	// Handed over turned rows-for-columns, as OBS hands over ViewProj.
	matrix4 cam = obs_matrix(cam_);
	matrix4_transpose(&cam, &cam);
	gs_effect_set_matrix4(p_cam, &cam);
	set_flat(nullptr);
}

void Avatar3D::set_flat(const V3 *at)
{
	set_vec4(p_flat_at, at ? at->x : 0.0f, at ? at->y : 0.0f, at ? at->z : 0.0f, at ? 1.0f : 0.0f);
}

// The lights (lights.hpp), as the effect has them.
void Avatar3D::set_lights(const View3D &v)
{
	const Lights &l = v.lights;
	const Lights studio;
	auto put = [](gs_eparam_t *p, V3 c) {
		vec3 x;
		vec3_set(&x, c.x, c.y, c.z);
		gs_effect_set_vec3(p, &x);
	};
	auto share = [](V3 a, V3 b) {
		return V3{b.x > 0 ? a.x / b.x : 1, b.y > 0 ? a.y / b.y : 1, b.z > 0 ? a.z / b.z : 1};
	};
	put(p_key_dir, normalized(l.key_dir, {0, 0, 1}));
	put(p_key_colour, l.key);
	put(p_fill_dir, normalized(l.fill_dir, {0, 0, 1}));
	put(p_fill_colour, l.fill);
	put(p_room_up, l.room_up);
	put(p_room_down, l.room_down);
	set_vec4(p_rim_light, l.rim.x, l.rim.y, l.rim.z, std::max(0.1f, l.rim_edge));
	put(p_key_tint, share(l.key, studio.key));
	put(p_room_tint, share(l.room_up + l.room_down, studio.room_up + studio.room_down));
}

void Avatar3D::set_material(const Material &m, const View3D &v, bool outline, bool skin, bool front_only,
			    const std::vector<gs_texture_t *> *textures)
{
	// Everything a technique reads, every time: OBS empties them all
	// when the technique before finishes (see effect-needs.hpp).
	set_camera();
	set_lights(v);
	const std::vector<gs_texture_t *> &pics = textures ? *textures : textures_;
	auto tex = [&pics](const TexRef &r, gs_texture_t *none) {
		if (r.image >= 0 && r.image < (int)pics.size() && pics[r.image])
			return pics[r.image];
		return none;
	};
	int mode = m.shading == Shading::Toon ? 1 : m.shading == Shading::Unlit ? 2 : 0;
	if (v.look == 1)
		mode = 0;
	else if (v.look == 2)
		mode = 1;
	else if (v.look == 3)
		mode = 2;
	const bool own_toon = m.shading == Shading::Toon;
	gs_texture_t *base = tex(m.base_map, white_);
	gs_effect_set_texture(p_base_map, base);
	gs_effect_set_texture(p_shade_map, own_toon ? tex(m.shade_map, white_) : base);
	gs_effect_set_texture(p_emissive_map, tex(m.emissive_map, white_));
	gs_texture_t *cap = own_toon ? tex(m.matcap_map, nullptr) : nullptr;
	gs_effect_set_texture(p_matcap_map, cap ? cap : black_);
	gs_effect_set_texture(p_metal_map, tex(m.metal_map, white_));
	// Its detail pictures (VRM's MToon has no occlusion).
	const bool detail = v.detail && mode != 2;
	gs_effect_set_texture(p_normal_map, detail ? tex(m.normal_map, flat_) : flat_);
	gs_effect_set_texture(p_occlusion_map, detail && !own_toon ? tex(m.occlusion_map, white_) : white_);
	set_vec4(p_detail, detail ? m.normal_scale : 0.0f, detail && !own_toon ? m.occlusion_strength : 0.0f,
		 skin ? std::min(1.0f, std::max(0.0f, v.soft_skin)) : 0.0f, front_only ? 1.0f : 0.0f);
	set_vec4(p_base, m.base[0], m.base[1], m.base[2], m.base[3]);
	set_vec3(p_emissive, m.emissive);
	set_vec4(p_surface, m.metallic, m.roughness, m.alpha == AlphaMode::Mask ? std::max(0.001f, m.cutoff) : 0.0f,
		 m.alpha == AlphaMode::Blend ? 1.0f : 0.0f);
	set_vec4(p_glow, m.glow ? 1.0f : 0.0f, m.glow_soft, m.glow_ragged, 0.0f);
	if (own_toon) {
		set_vec4(p_toon, m.shade_shift, m.shade_toony, m.rim_power, m.rim_lift);
		set_vec3(p_shade, m.shade);
		set_vec3(p_rim, m.rim);
		set_vec3(p_matcap, m.matcap);
	} else {
		// A cartoon of a plain part: its own colour a shade darker in shadow.
		const float shade[3] = {0.55f, 0.5f, 0.55f}, rim[3] = {0.12f, 0.12f, 0.12f}, none[3] = {0, 0, 0};
		set_vec4(p_toon, 0.05f, 0.85f, 3.0f, 0.0f);
		set_vec3(p_shade, shade);
		set_vec3(p_rim, rim);
		set_vec3(p_matcap, none);
	}
	set_vec4(p_look, (float)mode, own_toon ? m.rim_mix : 1.0f, cap ? 1.0f : 0.0f, opacity_);
	set_vec4(p_uv, m.base_map.offset[0], m.base_map.offset[1], m.base_map.scale[0], m.base_map.scale[1]);
	if (outline) {
		float width = 0;
		if (m.outline == Outline::World)
			width = m.outline_width * world_scale_;
		else if (m.outline == Outline::Screen)
			width = m.outline_width * part_distance_ * 2 * std::tan(fov_ / 2);
		width = std::max(width, outline_share_ * height_);
		gs_effect_set_float(p_outline_width, width);
		const bool own = m.outline != Outline::None;
		set_vec4(p_outline_colour, own ? m.outline_colour[0] : 0.08f, own ? m.outline_colour[1] : 0.06f,
			 own ? m.outline_colour[2] : 0.07f, own ? m.outline_mix : 0.5f);
	}
}

void Avatar3D::draw_part(size_t i, const Rig::Part &p, const char *technique)
{
	const Part &part = parts_[i];
	if (!part.vb || !part.ib)
		return;
	if (p.gpu) {
		if (!part.skin || p.collapsed)
			return;
		auto b = bone_cols_.find(p.bones);
		if (b == bone_cols_.end() || b->second.cols.empty())
			return;
		gs_effect_set_val(p_bones, b->second.cols.data(), b->second.cols.size() * sizeof(float));
		set_vec4(p_skin, p.handed, 0, 0, 0);
	}
	gs_load_vertexbuffer(part.vb);
	gs_load_indexbuffer(part.ib);
	check_(technique);
	while (gs_effect_loop(effect_, technique))
		gs_draw(GS_TRIS, 0, 0);
}

void Avatar3D::set_strand(const View3D &v, uint32_t drawn_h, float upto, float scale, gs_texture_t *depth, bool lines,
			  int skip)
{
	const M4 to_world = inverse(cam_);
	const V3 eye = move_of(to_world);
	set_vec3(p_eye, &eye.x);
	// One pixel's width a unit from the camera; hairs at least 1 pixel of
	// the picture as drawn (twice the size: half a pixel of the one
	// shown). A hair's root sits on the skin: it shows up to 2 mm (of a
	// character in metres) behind a solid part - the hair further along
	// less, its tip not at all.
	const float pixel = 2 * std::tan(v.fov / 2) / std::max(1u, drawn_h);
	set_vec4(p_strand, pixel, 1.0f, scale, 0.002f * scale);
	set_vec4(p_strand_lod, upto, 0.0f, lines ? 1.0f : 0.0f, 0.0f);
	// The picture's size (as many times wider than the one shown as it's
	// higher). Hidden hair left out before it's drawn: only a point
	// further off than everything round it by 5 mm (of a character in
	// metres) more than its root is allowed.
	if (p_strand_view)
		set_vec4(p_strand_view, (float)v.w * ((float)drawn_h / (float)std::max(1u, v.h)), (float)drawn_h,
			 0.005f * scale, (float)skip);
	gs_effect_set_texture(p_depth_map, depth ? depth : white_);
}

void Avatar3D::set_depth_material(const Material &m, const std::vector<gs_texture_t *> *textures)
{
	set_camera();
	const std::vector<gs_texture_t *> &pics = textures ? *textures : textures_;
	const int image = m.base_map.image;
	gs_effect_set_texture(p_base_map, image >= 0 && image < (int)pics.size() && pics[image] ? pics[image] : white_);
	set_vec4(p_base, m.base[0], m.base[1], m.base[2], m.base[3]);
	set_vec4(p_surface, m.metallic, m.roughness, m.alpha == AlphaMode::Mask ? std::max(0.001f, m.cutoff) : 0.0f,
		 0.0f);
	set_vec4(p_uv, m.base_map.offset[0], m.base_map.offset[1], m.base_map.scale[0], m.base_map.scale[1]);
}

bool Avatar3D::load_strands(size_t i, const Rig::Part &p, bool lines, bool laid)
{
	Part &part = parts_[i];
	if (!part.vb || !part.ib || !p.prim)
		return false;
	if (lines && !laid && !part.ib_lines) {
		// (Its pieces as lines: made the first time it's drawn that way.)
		const Strands &h = p.prim->hair;
		uint32_t *ix = static_cast<uint32_t *>(bmalloc(sizeof(uint32_t) * h.pieces.size() * 2));
		strand_corners(h.pieces.data(), nullptr, h.pieces.size(), true, ix);
		part.ib_lines = gs_indexbuffer_create(GS_UNSIGNED_LONG, ix, h.pieces.size() * 2, 0);
		if (!part.ib_lines)
			return false;
	}
	if (p.gpu) {
		auto b = bone_cols_.find(p.bones);
		if (b == bone_cols_.end() || b->second.cols.empty())
			return false;
		gs_effect_set_val(p_bones, b->second.cols.data(), b->second.cols.size() * sizeof(float));
		// Its spots' moves: always the whole list (Direct3D wants every
		// number of it).
		spots_.assign((Strands::MOST_ANCHORS + 1) * 4, 0.0f);
		for (size_t a = 0; a < p.spot_moves.size() && a <= Strands::MOST_ANCHORS; a++) {
			spots_[a * 4] = p.spot_moves[a].x;
			spots_[a * 4 + 1] = p.spot_moves[a].y;
			spots_[a * 4 + 2] = p.spot_moves[a].z;
		}
		gs_effect_set_val(p_spots, spots_.data(), spots_.size() * sizeof(float));
	}
	if (laid)
		return true;
	gs_load_vertexbuffer(part.vb);
	gs_load_indexbuffer(lines ? part.ib_lines : part.ib);
	return true;
}

void Avatar3D::draw_strands(size_t i, const Rig::Part &p, const std::vector<int> &groups, size_t layer, size_t count,
			    const char *technique, bool lines, int sort, HairDrawn &drawn)
{
	// Laid out as drawn (posed on the card): each sort's own points and
	// pieces, made before the first time they're drawn; one that can't be
	// made (no memory for it): by point, as before.
	Part &part = parts_[i];
	bool laid = hair_plan_.layout == 1 && p.gpu && part.skin && p.prim;
	for (int k = 0; laid && k < 2; k++)
		if (sort != 1 - k)
			laid = lay_strands(part, *p.prim, k, lines);
	if (!load_strands(i, p, lines, laid))
		return;
	// (A piece: two triangles' six corners, or a line's two.)
	const uint32_t corners = lines ? 2 : 6;
	const gs_draw_mode shape = lines ? GS_LINES : GS_TRIS;
	const Strands &h = p.prim->hair;
	check_(technique);
	const size_t layers = (size_t)std::max(1, h.layers);
	if (layer >= layers)
		return;
	// (A measure - HairPlan::measure 5: a stretch in two draws, its first
	// half and its second.)
	const bool halves = hair_plan_.measure == 5;
	auto send = [&](uint32_t from, uint32_t pieces) {
		const uint32_t first = halves && pieces > 1 ? pieces / 2 : pieces;
		gs_draw(shape, from * corners, first * corners);
		drawn.draws++;
		if (first < pieces) {
			gs_draw(shape, (from + first) * corners, (pieces - first) * corners);
			drawn.draws++;
		}
	};
	if (laid) {
		const Part::Laid *loaded = nullptr;
		while (gs_effect_loop(effect_, technique)) {
			loaded = nullptr;
			for (int g : groups) {
				const size_t run = (size_t)g * layers + layer;
				Strands::Stretch some[2];
				h.pieces_of(run, count, some);
				for (int k = 0; k < 2; k++) {
					if (!some[k].count || sort == 1 - k || run >= part.laid_from[k].size())
						continue;
					const Part::Laid &l = part.laid[k][lines ? 1 : 0];
					if (loaded != &l) {
						gs_load_vertexbuffer(l.vb);
						gs_load_indexbuffer(l.ib);
						loaded = &l;
					}
					send(part.laid_from[k][run], some[k].count);
					drawn.pieces += some[k].count;
					drawn.laid += some[k].count;
					if (lines)
						drawn.lines += some[k].count;
				}
			}
		}
		return;
	}
	while (gs_effect_loop(effect_, technique))
		for (int g : groups) {
			Strands::Stretch some[2];
			h.pieces_of((size_t)g * layers + layer, count, some);
			if (sort == 0)
				some[1].count = 0;
			else if (sort == 1)
				some[0].count = 0;
			else if (some[0].from + some[0].count == some[1].from) {
				// (All of both: one stretch.)
				some[0].count += some[1].count;
				some[1].count = 0;
			}
			for (const Strands::Stretch &one : some) {
				if (!one.count)
					continue;
				send(one.from, one.count);
				drawn.pieces += one.count;
				if (lines)
					drawn.lines += one.count;
			}
		}
}

void Avatar3D::draw_hull(size_t i, const Rig::Part &p, const View3D &v, uint32_t drawn_h, size_t count,
			 HairDrawn &drawn)
{
	const Strands &h = p.prim->hair;
	const size_t layers = (size_t)std::max(1, h.layers), core = (size_t)std::max(0, std::min(h.core, h.layers));
	// (A measure - HairPlan::measure 4: no hull.)
	if (!core || !count || hair_plan_.measure == 4)
		return;
	// A quarter of the strands drawn (the first of them), each as much
	// wider.
	const size_t some = hull_strands(count);
	const float upto = h.threshold_of(some);
	// Laid out as drawn (posed on the card), made before the first time
	// it's drawn; one that can't be made: by point, as before.
	Part &part = parts_[i];
	const bool laid = hair_plan_.layout == 1 && p.gpu && part.skin && p.prim && lay_hull(part, *p.prim);
	set_camera();
	set_strand(v, drawn_h, upto, p.scale, nullptr);
	if (!load_strands(i, p, false, laid))
		return;
	const char *technique = p.gpu ? "HullSk" : "Hull";
	check_(technique);
	if (laid) {
		if (part.hull.none || part.hull_lay.from.size() != h.groups.size() + 1)
			return;
		gs_load_vertexbuffer(part.hull.vb);
		gs_load_indexbuffer(part.hull.ib);
		while (gs_effect_loop(effect_, technique))
			for (size_t g = 0; g < h.groups.size(); g++) {
				// (A group's first so many strands' pieces: the start of
				// its stretch.)
				const uint32_t pieces = part.hull_lay.pieces_of(h, g, some);
				if (!pieces)
					continue;
				set_vec4(p_strand_lod, upto, 1.5f * h.groups[g].reach * p.scale, 0.0f, 0.0f);
				gs_draw(GS_TRIS, part.hull_lay.from[g] * 6, pieces * 6);
				drawn.hull_pieces += pieces;
				drawn.hull_draws++;
			}
		return;
	}
	while (gs_effect_loop(effect_, technique))
		for (size_t g = 0; g < h.groups.size(); g++) {
			// (Half as far again as its inner layers reach: a hair's
			// root, straight behind its own inner end, always shows.)
			set_vec4(p_strand_lod, upto, 1.5f * h.groups[g].reach * p.scale, 0.0f, 0.0f);
			for (size_t l = 0; l < core; l++) {
				Strands::Stretch both[2];
				h.pieces_of(g * layers + l, some, both);
				for (const Strands::Stretch &one : both)
					if (one.count) {
						gs_draw(GS_TRIS, one.from * 6, one.count * 6);
						drawn.hull_pieces += one.count;
						drawn.hull_draws++;
					}
			}
		}
}

void Avatar3D::draw_prop(Prop3D &prop, const View3D &v, bool see_through, bool depth_only)
{
	if (!(prop.opacity_ > 0.001f))
		return;
	// (A thing fading in or out hides no hair: it's drawn over it, as
	// see-through as it is.)
	if (depth_only && prop.opacity_ < 0.999f)
		return;
	static const Material plain;
	const Model &m = *prop.model_;
	const float was = opacity_;
	opacity_ = v.opacity * std::min(1.0f, prop.opacity_);
	const bool fading = opacity_ < 0.999f;
	for (size_t i = 0; i < prop.parts_.size(); i++) {
		Prop3D::Part &part = prop.parts_[i];
		if (!part.vb || !part.ib)
			continue;
		const int mi = part.prim->material;
		Material mat = mi >= 0 && mi < (int)m.materials.size() ? m.materials[mi] : plain;
		if (depth_only) {
			// Its solid parts' distances, for the hairs behind them
			// (strands in a thing aren't drawn at all).
			if (mat.alpha == AlphaMode::Blend || mat.glow || part.prim->strands)
				continue;
			set_depth_material(mat, &prop.textures_);
			gs_set_cull_mode(cull_of(mat, prop.handed_));
			gs_load_vertexbuffer(part.vb);
			gs_load_indexbuffer(part.ib);
			check_("Depth");
			while (gs_effect_loop(effect_, "Depth"))
				gs_draw(GS_TRIS, 0, 0);
			gs_set_cull_mode(GS_NEITHER);
			continue;
		}
		if ((mat.alpha == AlphaMode::Blend || fading) != see_through)
			continue;
		if (mi >= 0 && mi < (int)prop.tints_.size()) {
			const V3 t = prop.tints_[mi];
			mat.base[0] *= t.x;
			mat.base[1] *= t.y;
			mat.base[2] *= t.z;
		}
		const bool skin = mat.name.find("skin") != std::string::npos;
		const bool flat_look = v.look == 3 || (v.look == 0 && mat.shading == Shading::Unlit);
		const char *technique =
			part.bumped && v.detail && !flat_look && mat.normal_map.used() ? "DrawBumped" : "Draw";
		part_distance_ = part.distance;
		set_material(mat, v, false, skin, sides_shown(mat, prop.handed_) == Sides::Front, &prop.textures_);
		gs_set_cull_mode(cull_of(mat, prop.handed_));
		// A glowing part (0.9.0): all of it at its middle's depth.
		const V3 middle = point(prop.place_, part.middle);
		if (mat.glow) {
			set_flat(&middle);
			gs_depth_function(GS_LEQUAL);
		}
		gs_load_vertexbuffer(part.vb);
		gs_load_indexbuffer(part.ib);
		check_(technique);
		while (gs_effect_loop(effect_, technique))
			gs_draw(GS_TRIS, 0, 0);
		if (mat.glow) {
			set_flat(nullptr);
			gs_depth_function(GS_LESS);
		}
		gs_set_cull_mode(GS_NEITHER);
	}
	opacity_ = was;
}

Prop3D::Prop3D(std::shared_ptr<const Model> model, const std::vector<ModelPicture> &pictures)
	: model_(std::move(model))
{
	for (const ModelPicture &pic : pictures) {
		gs_texture_t *tex = nullptr;
		if (pic.size && !pic.levels.empty()) {
			std::vector<const uint8_t *> data;
			for (const auto &level : pic.levels)
				data.push_back(level.data());
			tex = gs_texture_create(pic.size, pic.size, GS_RGBA, (uint32_t)data.size(), data.data(), 0);
		}
		textures_.push_back(tex);
	}
	const Model &m = *model_;
	tints_.assign(m.materials.size(), V3{1, 1, 1});
	const std::vector<M4> world = rest_world(m);
	bool any = false;
	for (size_t ni = 0; ni < m.nodes.size(); ni++) {
		const Node &node = m.nodes[ni];
		if (node.mesh < 0 || node.mesh >= (int)m.meshes.size())
			continue;
		const Mesh &mesh = m.meshes[node.mesh];
		const Skin *skin = node.skin >= 0 && node.skin < (int)m.skins.size() ? &m.skins[node.skin] : nullptr;
		for (const Primitive &p : mesh.parts) {
			Part part;
			part.prim = &p;
			part.points = p.pos.size();
			if (!part.points || p.index.empty())
				continue;
			const bool weighted = skin && p.joints.size() == part.points * 4 && p.weights.size() == part.points * 4;
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
			part.bumped = p.tangent.size() == part.points * 4;
			part.pos.resize(part.points);
			part.normal.resize(part.points);
			if (part.bumped)
				part.tangent.resize(part.points);
			for (size_t k = 0; k < part.points; k++) {
				const M4 w = at(k);
				part.pos[k] = point(w, p.pos[k]);
				part.normal[k] =
					normalized(direction(w, k < p.normal.size() ? p.normal[k] : V3{0, 0, 1}), {0, 0, 1});
				if (part.bumped)
					part.tangent[k] = normalized(
						direction(w, {p.tangent[k * 4], p.tangent[k * 4 + 1], p.tangent[k * 4 + 2]}),
						{1, 0, 0});
				const V3 q = part.pos[k];
				if (!any) {
					low_ = high_ = q;
					any = true;
				}
				low_ = {std::min(low_.x, q.x), std::min(low_.y, q.y), std::min(low_.z, q.z)};
				high_ = {std::max(high_.x, q.x), std::max(high_.y, q.y), std::max(high_.z, q.z)};
				part.middle = part.middle + q * (1.0f / (float)part.points);
			}
			for (const MorphTarget &t : p.morphs) {
				std::vector<V3> dp(part.points), dn;
				for (size_t i = 0; i < t.at.size(); i++)
					if (t.at[i] < part.points)
						dp[t.at[i]] = direction(at(t.at[i]), t.pos[i]);
				if (!t.normal.empty()) {
					dn.resize(part.points);
					for (size_t i = 0; i < t.normal.size(); i++)
						if (t.at[i] < part.points)
							dn[t.at[i]] = direction(at(t.at[i]), t.normal[i]);
				}
				part.morph_pos.push_back(std::move(dp));
				part.morph_normal.push_back(std::move(dn));
			}
			part.vb = make_vb(part.points, true, nullptr, part.bumped);
			part.ib = make_ib(p.index);
			if (part.vb) {
				gs_vb_data *d = gs_vertexbuffer_get_data(part.vb);
				float *uv = static_cast<float *>(d->tvarray[0].array);
				for (size_t k = 0; k < part.points; k++) {
					if (p.uv.size() >= (k + 1) * 2) {
						uv[k * 2] = p.uv[k * 2];
						uv[k * 2 + 1] = p.uv[k * 2 + 1];
					}
					uint8_t c[4] = {255, 255, 255, 255};
					if (p.colour.size() >= (k + 1) * 4)
						srgb_bytes(&p.colour[k * 4], c);
					std::memcpy(&d->colors[k], c, 4);
				}
				gs_vertexbuffer_flush(part.vb);
			}
			if (shape_names_.size() < mesh.morph_names.size())
				shape_names_ = mesh.morph_names;
			parts_.push_back(std::move(part));
		}
	}
	size_t shapes = shape_names_.size();
	for (const Part &part : parts_)
		shapes = std::max(shapes, part.morph_pos.size());
	shapes_.assign(shapes, 0);
	place(M4());
}

Prop3D::~Prop3D()
{
	for (gs_texture_t *t : textures_)
		gs_texture_destroy(t);
	for (Part &p : parts_) {
		gs_vertexbuffer_destroy(p.vb);
		gs_indexbuffer_destroy(p.ib);
	}
}

int Prop3D::shape(const std::string &name) const
{
	for (size_t i = 0; i < shape_names_.size(); i++)
		if (shape_names_[i] == name)
			return (int)i;
	return -1;
}

void Prop3D::set_shape(int shape, float amount)
{
	if (shape >= 0 && shape < (int)shapes_.size())
		shapes_[shape] = std::isfinite(amount) ? std::min(1.0f, std::max(0.0f, amount)) : 0.0f;
}

void Prop3D::tint(int material, V3 times)
{
	if (material >= 0 && material < (int)tints_.size())
		tints_[material] = times;
}

void Prop3D::place(const M4 &to_world, float opacity)
{
	place_ = to_world;
	opacity_ = std::isfinite(opacity) ? std::min(1.0f, std::max(0.0f, opacity)) : 0.0f;
	const V3 ax = direction(to_world, {1, 0, 0}), ay = direction(to_world, {0, 1, 0}),
		 az = direction(to_world, {0, 0, 1});
	handed_ = dot(cross(ax, ay), az) < 0 ? -1.0f : 1.0f;
	// Normals turn as the inverse's rows-for-columns would (the same, for
	// a turn and an even size), sized back to length 1 as they're used.
	const M4 normals = inverse(to_world);
	auto normal_of = [&normals](V3 n) {
		return normalized(V3{normals.m[0] * n.x + normals.m[1] * n.y + normals.m[2] * n.z,
				     normals.m[4] * n.x + normals.m[5] * n.y + normals.m[6] * n.z,
				     normals.m[8] * n.x + normals.m[9] * n.y + normals.m[10] * n.z},
				  {0, 0, 1});
	};
	for (Part &part : parts_) {
		if (!part.vb)
			continue;
		gs_vb_data *d = gs_vertexbuffer_get_data(part.vb);
		const bool tangents = part.bumped && d->tangents;
		for (size_t k = 0; k < part.points; k++) {
			V3 p = part.pos[k], n = part.normal[k];
			for (size_t s = 0; s < part.morph_pos.size() && s < shapes_.size(); s++) {
				const float w = shapes_[s];
				if (w <= 0)
					continue;
				p = p + part.morph_pos[s][k] * w;
				if (!part.morph_normal[s].empty())
					n = n + part.morph_normal[s][k] * w;
			}
			const V3 wp = point(to_world, p), wn = normal_of(n);
			vec3_set(&d->points[k], wp.x, wp.y, wp.z);
			vec3_set(&d->normals[k], wn.x, wn.y, wn.z);
			if (tangents) {
				const V3 t = normalized(direction(to_world, part.tangent[k]), {1, 0, 0});
				vec3_set(&d->tangents[k], t.x, t.y, t.z);
				d->tangents[k].w = part.prim->tangent[k * 4 + 3] * handed_;
			}
		}
		gs_vb_data moved = {};
		moved.num = d->num;
		moved.points = d->points;
		moved.normals = d->normals;
		moved.tangents = tangents ? d->tangents : nullptr;
		gs_vertexbuffer_flush_direct(part.vb, &moved);
	}
}

bool Avatar3D::upload(const Rig *rig)
{
	const bool character =
		rig && model_ && &rig->model() == model_.get() && rig->parts().size() == parts_.size();
	if (!character)
		return false;
	const std::vector<Rig::Part> &posed = rig->parts();
	bone_stamp_++;
	for (size_t i = 0; i < posed.size(); i++) {
		Part &part = parts_[i];
		const Rig::Part &p = posed[i];
		if (p.gpu) {
			if (!part.vb || !part.skin || p.hidden)
				continue;
			// Its points as made, with its expressions while any shows
			// (and once more as made when they stop). (Hair strands'
			// stay as made: the card moves them by their spots.)
			if (!part.strands && (p.morphed || part.was_morphed || part.posed_here)) {
				const Primitive &prim = *p.prim;
				const bool now = p.morphed && p.rest_pos.size() == part.points &&
						 p.rest_normal.size() == part.points;
				const V3 *rp = now ? p.rest_pos.data() : prim.pos.data();
				const V3 *rn = now ? p.rest_normal.data() : prim.normal.data();
				gs_vb_data *d = gs_vertexbuffer_get_data(part.vb);
				for (size_t k = 0; k < part.points; k++) {
					vec3_set(&d->points[k], rp[k].x, rp[k].y, rp[k].z);
					vec3_set(&d->normals[k], rn[k].x, rn[k].y, rn[k].z);
				}
				gs_vb_data moved = {};
				moved.num = d->num;
				moved.points = d->points;
				moved.normals = d->normals;
				gs_vertexbuffer_flush_direct(part.vb, &moved);
				part.was_morphed = now;
				part.posed_here = false;
			}
			// Its bones, once a frame for each set.
			if (p.bones >= 0) {
				const std::vector<M4> &set = rig->bones(p.bones);
				BoneCols &b = bone_cols_[p.bones];
				if (b.stamp != bone_stamp_ && !set.empty() && set.size() <= Rig::GPU_MOST_BONES) {
					// Always the whole list (Direct3D wants every number
					// of it), the bones not there left as nothing.
					b.cols.assign(Rig::GPU_MOST_BONES * 16, 0.0f);
					for (size_t j = 0; j < set.size(); j++)
						std::memcpy(&b.cols[j * 16], set[j].m, sizeof(float) * 16);
					b.stamp = bone_stamp_;
				}
			}
			continue;
		}
		if (part.strands) {
			// Hair strands posed here: each point's place and the way
			// its strand runs, for both its sides (the rest of what
			// rides with them stays as made).
			// (Only those the rig posed: the first of them.)
			if (!part.vb || part.skin || p.hidden || p.collapsed || p.pos.empty() ||
			    p.pos.size() * 2 > part.points || p.normal.size() != p.pos.size())
				continue;
			gs_vb_data *d = gs_vertexbuffer_get_data(part.vb);
			for (size_t k = 0; k < p.pos.size(); k++) {
				float e[2];
				folded(p.normal[k], e);
				for (size_t side = 0; side < 2; side++) {
					vec3 &at = d->points[k * 2 + side], &run = d->normals[k * 2 + side];
					at.x = p.pos[k].x, at.y = p.pos[k].y, at.z = p.pos[k].z;
					run.x = e[0], run.y = e[1];
				}
			}
			gs_vb_data moved = {};
			moved.num = d->num;
			moved.points = d->points;
			moved.normals = d->normals;
			gs_vertexbuffer_flush_direct(part.vb, &moved);
			continue;
		}
		if (!part.vb || p.hidden || p.pos.size() != part.points)
			continue;
		// (A part made for the card, posed here this once - measuring:
		// its points as made go back before the card poses it again.)
		part.posed_here = part.skin;
		gs_vb_data *d = gs_vertexbuffer_get_data(part.vb);
		for (size_t k = 0; k < part.points; k++) {
			vec3_set(&d->points[k], p.pos[k].x, p.pos[k].y, p.pos[k].z);
			vec3_set(&d->normals[k], p.normal[k].x, p.normal[k].y, p.normal[k].z);
		}
		const bool tangents = part.bumped && d->tangents && p.tangent.size() == part.points;
		if (tangents)
			for (size_t k = 0; k < part.points; k++) {
				vec3_set(&d->tangents[k], p.tangent[k].x, p.tangent[k].y, p.tangent[k].z);
				// Which way its up is, in the fourth number.
				d->tangents[k].w = p.prim->tangent[k * 4 + 3] * p.handed;
			}
		// Only what moves: its colours and UVs stay as they were sent.
		gs_vb_data moved = {};
		moved.num = d->num;
		moved.points = d->points;
		moved.normals = d->normals;
		moved.tangents = tangents ? d->tangents : nullptr;
		gs_vertexbuffer_flush_direct(part.vb, &moved);
	}
	return true;
}

// The graphics card's clocks turned on one: what the frame CLOCKS back
// took, read now (asked sooner, OBS waits for the card) and told to the
// hair budget.
void Avatar3D::turn_clock()
{
	clock_at_ = (clock_at_ + 1) % CLOCKS;
	HairClock &c = clocks_[clock_at_];
	if (!c.used)
		return;
	c.used = false;
	bool disjoint = false;
	uint64_t frequency = 0, depth = 0, hair = 0;
	// (Disjoint: the card's clock jumped part way - a reading to leave out.)
	bool read = gs_timer_range_get_data(c.range, &disjoint, &frequency) && frequency;
	read = read && gs_timer_get_data(c.depth, &depth) && gs_timer_get_data(c.hair, &hair) && !disjoint;
	const double ms = read ? ((double)depth + (double)hair) * 1000.0 / (double)frequency : -1;
	if (!(ms >= 0) || ms > 10000) {
		// No answer: enough of them in a row and the budget has no
		// clock to go by (and what hair took isn't known any more).
		budget_.unread();
		if (budget_.silent()) {
			took_.clear();
			took_depth_.clear();
		}
		return;
	}
	budget_.read((float)ms, c.share);
	took_.push_back((float)ms);
	took_depth_.push_back((float)((double)depth * 1000.0 / (double)frequency));
	if (took_.size() > 31)
		took_.erase(took_.begin());
	if (took_depth_.size() > 31)
		took_depth_.erase(took_depth_.begin());
}

// This frame's clocks started (null: there are none - hair's time isn't
// known, and the hair budget leaves hair as it is).
Avatar3D::HairClock *Avatar3D::start_clock(float share)
{
	if (!clocks_work_)
		return nullptr;
	HairClock &c = clocks_[clock_at_];
	if (!c.depth || !c.hair) {
		if (!c.depth)
			c.depth = gs_timer_create();
		if (!c.hair)
			c.hair = gs_timer_create();
		if (!c.range)
			c.range = gs_timer_range_create();
		// (OpenGL has no range: its clocks are always in nanoseconds.)
		if (!c.depth || !c.hair || (!c.range && gs_get_device_type() != GS_DEVICE_OPENGL)) {
			clocks_work_ = false;
			return nullptr;
		}
	}
	c.used = true;
	c.share = share;
	gs_timer_range_begin(c.range);
	return &c;
}

// The graphics card was lost (Windows: its driver restarted): the clocks
// asked before it never answer.
void Avatar3D::clocks_lost()
{
	for (HairClock &c : clocks_)
		c.used = false;
}

float Avatar3D::hair_took(const std::vector<float> &readings)
{
	if (readings.empty())
		return -1;
	std::vector<float> sorted = readings;
	std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
	return sorted[sorted.size() / 2];
}

gs_texture_t *Avatar3D::draw(const Rig *rig, const View3D &v, const std::vector<PropAt> &props,
			     const std::vector<Other> &others)
{
	const bool character = upload(rig);
	turn_clock();
	hair_drawn_ = HairDrawn();
	hair_drawn_.ms = hair_took(took_);
	hair_drawn_.depth_ms = hair_took(took_depth_);
	// 0.9.0: the others beside it, each onto the graphics card too.
	struct Drawn {
		Avatar3D *a;
		const std::vector<Rig::Part> *posed;
		std::vector<float> distance;
		std::vector<V3> middle; // each part's middle, in the world (a glowing part's depth)
		bool own_outlines = false;
		bool main = false;
		// Hair strands: how many of each strand part's are drawn (none:
		// it isn't), and whether any are - its stand-in hair is then
		// left out.
		std::vector<HairShare> hair;
		bool strands = false;
	};
	std::vector<Drawn> all;
	static const std::vector<Rig::Part> nothing;
	all.push_back({this, character ? &rig->parts() : &nothing, {}, {}, false, true, {}, false});
	for (const Other &o : others)
		if (o.avatar && o.avatar != this && o.avatar->effect_ && o.avatar->upload(o.rig))
			all.push_back({o.avatar, &o.rig->parts(), {}, {}, false, false, {}, false});
	if (!effect_ || !v.w || !v.h || (!character && props.empty() && all.size() < 2))
		return nullptr;

	const uint32_t k = v.smooth && v.w * 2 <= MOST_DRAWN && v.h * 2 <= MOST_DRAWN ? 2 : 1;
	if (!scene_)
		scene_ = gs_texrender_create(GS_RGBA, GS_Z24_S8);
	if (!scene_)
		return nullptr;
	for (Drawn &dr : all) {
		dr.a->opacity_ = v.opacity;
		dr.a->outline_share_ = v.outline;
		dr.a->height_ = v.height;
		dr.a->world_scale_ = v.world_scale;
		dr.a->fov_ = v.fov;
		dr.a->cam_ = v.view;
		// Each part's distance from the camera (its middle), for the
		// see-through parts' order and screen-sized outlines.
		const std::vector<Rig::Part> &posed = *dr.posed;
		dr.distance.assign(posed.size(), 1);
		dr.middle.assign(posed.size(), V3{});
		for (size_t i = 0; i < posed.size(); i++) {
			if (posed[i].gpu) {
				dr.middle[i] = posed[i].middle;
				dr.distance[i] = std::max(1e-4f, -point(v.view, dr.middle[i]).z);
				continue;
			}
			const std::vector<V3> &pos = posed[i].pos;
			V3 sum;
			size_t n = 0;
			for (size_t j = 0; j < pos.size(); j += 17, n++)
				sum = sum + pos[j];
			if (n) {
				dr.middle[i] = sum * (1.0f / n);
				dr.distance[i] = std::max(1e-4f, -point(v.view, dr.middle[i]).z);
			}
		}
		// A character with outlines of its own keeps the outline setting
		// off the parts it has none on (a face, its eyes).
		if (dr.a->model_)
			for (const Material &mat : dr.a->model_->materials)
				dr.own_outlines = dr.own_outlines || mat.outline != Outline::None;
	}

	// Hair strands: how many of each part's are drawn - what its size on
	// screen asks for (hair.hpp's hair_share), the hair budget's share of
	// that, and no more pieces in all than the plan allows.
	const uint32_t drawn_h = v.h * k;
	// (Or the share the plan holds it at, measuring.)
	const float asked = clocks_work_ ? budget_.share() : 1.0f;
	const float budget = hair_plan_.share > 0 ? std::min(1.0f, hair_plan_.share) : asked;
	// (Hair to draw; and any of it posed on the graphics card.)
	bool any_hair = false, any_card_hair = false;
	{
		const float pixel = 2 * std::tan(v.fov / 2) / std::max(1u, drawn_h);
		// (The pieces of a part's first `count` strands: a strand's are
		// one fewer than its points.)
		auto pieces_of = [](const Strands &h, size_t count) {
			return count < h.first.size() ? (size_t)h.first[count] - count : 0;
		};
		size_t pieces = 0;
		for (Drawn &dr : all) {
			const std::vector<Rig::Part> &posed = *dr.posed;
			dr.hair.assign(posed.size(), HairShare());
			for (size_t i = 0; i < posed.size(); i++) {
				Part &part = dr.a->parts_[i];
				const Rig::Part &p = posed[i];
				if (!part.strands || !part.vb || !part.ib || part.skin != p.gpu || p.hidden ||
				    p.collapsed || !p.prim) {
					part.as_lines = part.thin_lines = part.formed = false;
					continue;
				}
				const Strands &h = p.prim->hair;
				// (Posed here: those the rig posed, the first of them.)
				const size_t loaded = p.gpu ? h.strands() : h.strands_in(p.pos.size());
				// (As lines where its hairs are thin enough: hair.hpp.)
				const HairForm form = hair_form(h, loaded, p.scale, dr.distance[i], pixel, 1.0f,
								hair_plan_.solid, budget,
								dr.a->p_strand_view ? hair_plan_.lines : 0,
								part.as_lines, part.thin_lines, hair_plan_.line_loss,
								hair_plan_.line_cost, hair_plan_.budget_rule == 1);
				dr.hair[i] = form.share;
				// (How often a part drawn the frame before changes between
				// lines and ribbons: counted.)
				if (part.formed && (part.as_lines != form.lines || part.thin_lines != form.thin))
					form_changes_++;
				part.formed = true;
				part.as_lines = form.lines;
				part.thin_lines = form.thin;
				pieces += pieces_of(h, dr.hair[i].count);
			}
		}
		// Too many pieces in all: every part's threshold as much lower as
		// brings them under (found by halving).
		float lower = 1;
		if (hair_plan_.pieces && pieces > hair_plan_.pieces) {
			auto count_at = [](const Strands &h, const HairShare &s, float f) {
				return std::max(std::min<size_t>(s.count, 16),
						std::min(s.count, h.strands_under(s.upto * f)));
			};
			float most = 1;
			lower = 0;
			for (int step = 0; step < 20; step++) {
				const float f = 0.5f * (lower + most);
				size_t sum = 0;
				for (const Drawn &dr : all)
					for (size_t i = 0; i < dr.hair.size(); i++)
						if (dr.hair[i].count)
							sum += pieces_of((*dr.posed)[i].prim->hair,
									 count_at((*dr.posed)[i].prim->hair, dr.hair[i],
										  f));
				(sum > hair_plan_.pieces ? most : lower) = f;
			}
			for (Drawn &dr : all)
				for (size_t i = 0; i < dr.hair.size(); i++) {
					HairShare &s = dr.hair[i];
					if (!s.count)
						continue;
					const Strands &h = (*dr.posed)[i].prim->hair;
					s.count = count_at(h, s, lower);
					s.upto = h.threshold_of(s.count);
				}
		}
		for (Drawn &dr : all)
			for (size_t i = 0; i < dr.hair.size(); i++) {
				const HairShare &s = dr.hair[i];
				if (!s.count)
					continue;
				const Strands &h = (*dr.posed)[i].prim->hair;
				dr.strands = any_hair = true;
				any_card_hair = any_card_hair || (*dr.posed)[i].gpu;
				hair_drawn_.in_file += h.in_file;
				hair_drawn_.loaded += (*dr.posed)[i].gpu ? h.strands()
									 : h.strands_in((*dr.posed)[i].pos.size());
				hair_drawn_.drawn += s.count;
				for (const auto &sort : dr.a->parts_[i].laid)
					for (const Part::Laid &l : sort) {
						hair_drawn_.laid_bytes += l.bytes;
						hair_drawn_.laid_ms += l.ms;
					}
				hair_drawn_.laid_bytes += dr.a->parts_[i].hull.bytes;
				hair_drawn_.laid_ms += dr.a->parts_[i].hull.ms;
			}
	}
	hair_drawn_.share = budget;
	hair_drawn_.share_changes = budget_.changes();
	hair_drawn_.form_changes = form_changes_;
	hair_drawn_.over = budget_.over();

	static const Material plain;
	auto material_of = [&](const Drawn &dr, size_t i) -> const Material & {
		const int mi = (*dr.posed)[i].prim->material;
		const Model *m = dr.a->model_.get();
		return m && mi >= 0 && mi < (int)m->materials.size() ? m->materials[mi] : plain;
	};
	// Not hidden by its rig - nor a character's stand-in hair, while its
	// strands are drawn.
	auto shows = [&](const Drawn &dr, size_t i) {
		const Rig::Part &p = (*dr.posed)[i];
		if (p.hidden || p.collapsed)
			return false;
		return !(dr.strands && !dr.a->parts_[i].strands && material_of(dr, i).hair_standin);
	};
	// Anything with it put where it goes (the same one may be in several
	// places: its points are worked out again for each).
	std::map<Prop3D *, const PropAt *> put_at;
	auto put = [&](const PropAt &at) {
		const PropAt *&last = put_at[at.prop];
		if (last == &at)
			return;
		last = &at;
		for (size_t n = 0; n < at.shapes.size(); n++)
			at.prop->set_shape((int)n, at.shapes[n]);
		at.prop->place(at.place, at.opacity);
	};
	// The camera's view as OBS's 3D wants it: y down, looking along -z.
	// The same on Direct3D and OpenGL: OBS's Direct3D turns z round
	// itself, so turning it here too would put the model behind the
	// camera.
	const matrix4 view = obs_matrix(obs_view(v.view));
	const bool hides_head = character && v.hide_behind_head && head_.vb && head_.ib;

	// Hairs are drawn with no depth test of their own (so that hairs over
	// one another all show through), and write no depth: first a picture
	// of every solid part's distance from the camera, which hides the
	// hairs behind them. Timed on the graphics card, with the hairs: the
	// hair budget's reading.
	gs_texture_t *depth_map = nullptr;
	HairClock *clock = any_hair ? start_clock(budget) : nullptr;
	int hair_timer = 0; // 0 not started, 1 running, 2 stopped
	auto stop_clock = [&] {
		if (!clock)
			return;
		if (hair_timer == 0)
			gs_timer_begin(clock->hair);
		if (hair_timer < 2)
			gs_timer_end(clock->hair);
		gs_timer_range_end(clock->range);
		clock = nullptr;
	};
	if (any_hair) {
		if (clock)
			gs_timer_begin(clock->depth);
		if (!depth_)
			depth_ = gs_texrender_create(GS_R32F, GS_Z24_S8);
		if (depth_)
			gs_texrender_reset(depth_);
		if (depth_ && gs_texrender_begin(depth_, v.w * k, v.h * k)) {
			vec4 far_away;
			vec4_set(&far_away, 1e9f, 0.0f, 0.0f, 1.0f);
			gs_clear(GS_CLEAR_COLOR | GS_CLEAR_DEPTH, &far_away, 1.0f, 0);
			gs_perspective(v.fov * 180 / 3.14159265f, (float)v.w / v.h, v.near, v.far);
			gs_matrix_set(&view);
			const gs_cull_mode cull = gs_get_cull_mode();
			gs_set_cull_mode(GS_NEITHER);
			gs_enable_depth_test(true);
			gs_depth_function(GS_LESS);
			// (Written as it is: blended, a distance would be mixed with
			// the one before.)
			gs_blend_state_push();
			gs_enable_blending(false);
			if (hides_head) {
				// The hidden head shape: the face's centimetres -> the view.
				gs_matrix_push();
				const matrix4 head = obs_matrix(v.head);
				gs_matrix_mul(&head);
				const M4 was = cam_;
				cam_ = v.view * v.head;
				set_depth_material(plain);
				cam_ = was;
				gs_load_vertexbuffer(head_.vb);
				gs_load_indexbuffer(head_.ib);
				check_("Depth");
				while (gs_effect_loop(effect_, "Depth"))
					gs_draw(GS_TRIS, 0, 0);
				gs_matrix_pop();
			}
			for (Drawn &dr : all)
				for (size_t i = 0; i < dr.posed->size(); i++) {
					const Material &mat = material_of(dr, i);
					if (!shows(dr, i) || dr.a->parts_[i].strands || mat.alpha == AlphaMode::Blend ||
					    mat.glow)
						continue;
					const Rig::Part &p = (*dr.posed)[i];
					dr.a->set_depth_material(mat);
					gs_set_cull_mode(cull_of(mat, p.handed));
					dr.a->draw_part(i, p, p.gpu ? "DepthSk" : "Depth");
					gs_set_cull_mode(GS_NEITHER);
				}
			for (const PropAt &at : props) {
				if (!at.prop)
					continue;
				put(at);
				draw_prop(*at.prop, v, false, true);
			}
			// The hairs' own inner layers too (the hull), each pixel
			// keeping the nearest: hair well behind another part's hair
			// is hidden by it, as it is by the part itself.
			gs_enable_blending(true);
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE);
			gs_blend_op(GS_BLEND_OP_MIN);
			for (Drawn &dr : all)
				for (size_t i = 0; i < dr.hair.size(); i++)
					if (dr.hair[i].count)
						dr.a->draw_hull(i, (*dr.posed)[i], v, drawn_h, dr.hair[i].count,
								hair_drawn_);
			gs_blend_op(GS_BLEND_OP_ADD);
			gs_blend_state_pop();
			gs_enable_depth_test(false);
			gs_set_cull_mode(cull);
			gs_texrender_end(depth_);
			depth_map = gs_texrender_get_texture(depth_);
		}
		if (clock)
			gs_timer_end(clock->depth);
		if (!depth_map) {
			// (No picture: no hair - its stand-in hair, where it has any.)
			any_hair = false;
			for (Drawn &dr : all) {
				dr.strands = false;
				dr.hair.assign(dr.hair.size(), HairShare());
			}
			hair_drawn_.drawn = 0;
		}
	}

	gs_texrender_reset(scene_);
	if (!gs_texrender_begin(scene_, v.w * k, v.h * k)) {
		stop_clock();
		return nullptr;
	}
	vec4 clear_colour;
	vec4_zero(&clear_colour);
	gs_clear(GS_CLEAR_COLOR | GS_CLEAR_DEPTH, &clear_colour, 1.0f, 0);
	gs_perspective(v.fov * 180 / 3.14159265f, (float)v.w / v.h, v.near, v.far);
	gs_matrix_set(&view);

	const gs_cull_mode cull = gs_get_cull_mode();
	gs_set_cull_mode(GS_NEITHER);
	gs_enable_depth_test(true);
	gs_depth_function(GS_LESS);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	if (hides_head) {
		gs_matrix_push();
		const matrix4 head = obs_matrix(v.head);
		gs_matrix_mul(&head);
		gs_enable_color(false, false, false, false);
		gs_load_vertexbuffer(head_.vb);
		gs_load_indexbuffer(head_.ib);
		set_camera();
		set_vec4(p_uv, 0, 0, 1, 1);
		check_("Hidden");
		while (gs_effect_loop(effect_, "Hidden"))
			gs_draw(GS_TRIS, 0, 0);
		gs_enable_color(true, true, true, true);
		gs_matrix_pop();
	}

	// Skin is the main character's (the others' materials aren't in its
	// list).
	auto is_skin = [&](const Drawn &dr, size_t i) {
		const int mi = (*dr.posed)[i].prim->material;
		return dr.main && v.skin && mi >= 0 && mi < (int)v.skin->size() && (*v.skin)[mi];
	};
	// With its normal map's bumps where it has one (not flat colours).
	auto technique_of = [&](const Drawn &dr, size_t i) {
		const Material &mat = material_of(dr, i);
		const bool flat_look = v.look == 3 || (v.look == 0 && mat.shading == Shading::Unlit);
		const bool sk = (*dr.posed)[i].gpu;
		if (dr.a->parts_[i].bumped && v.detail && !flat_look && mat.normal_map.used())
			return sk ? "DrawBumpedSk" : "DrawBumped";
		return sk ? "DrawSk" : "Draw";
	};
	// Outlines (only their far side, so they show round the edges), then
	// the solid parts over them, then the see-through ones far to near.
	// The outline setting draws one round every part - but a character
	// with outlines of its own keeps them off the parts it has none on (a
	// face, its eyes).
	// What's see-through, drawn last, far to near: a part, all a
	// character's hair strands, or (while there's hair) the see-through
	// parts of a thing.
	struct See {
		int queue = 0;
		float distance = 0;
		size_t w = 0, i = 0; // which of `all`, its part
		bool hair = false;   // all its hair strands
		const PropAt *prop = nullptr;
	};
	std::vector<See> blended;
	// A character's hair strands: its strand parts far to near, and each
	// one's groups of strands (those a bone holds: Strands::groups) far
	// to near.
	struct HairPart {
		size_t i = 0;
		float distance = 0;
		std::vector<std::pair<float, int>> groups; // (how far, which)
		std::vector<int> order;
	};
	std::vector<std::vector<HairPart>> hair(all.size());
	// Outlines with both sides (their near side is left out as they're
	// drawn), the rest with only the sides their materials show.
	for (Drawn &dr : all)
		for (size_t i = 0; i < dr.posed->size(); i++) {
			const Material &mat = material_of(dr, i);
			if (!shows(dr, i) || mat.glow || dr.a->parts_[i].strands ||
			    (mat.outline == Outline::None && (v.outline <= 0 || dr.own_outlines)))
				continue;
			dr.a->part_distance_ = dr.distance[i];
			dr.a->set_material(mat, v, true, false, false);
			dr.a->draw_part(i, (*dr.posed)[i], (*dr.posed)[i].gpu ? "OutlineSk" : "Outline");
		}
	for (size_t w = 0; w < all.size(); w++) {
		Drawn &dr = all[w];
		for (size_t i = 0; i < dr.posed->size(); i++) {
			if (!shows(dr, i))
				continue;
			const Material &mat = material_of(dr, i);
			const Rig::Part &p = (*dr.posed)[i];
			if (dr.a->parts_[i].strands) {
				// Hair strands: each group where its bone has it now (posed
				// on the card: its middle as made, moved by its bone; posed
				// here: a point near its middle).
				if (!dr.hair[i].count)
					continue;
				const Strands &h = p.prim->hair;
				const BoneCols *bones = nullptr;
				if (p.gpu) {
					auto b = dr.a->bone_cols_.find(p.bones);
					if (b != dr.a->bone_cols_.end())
						bones = &b->second;
				}
				HairPart part;
				part.i = i;
				part.distance = dr.distance[i];
				for (size_t g = 0; g < h.groups.size(); g++) {
					const Strands::Group &group = h.groups[g];
					V3 at = dr.middle[i];
					if (bones && group.joint >= 0 &&
					    ((size_t)group.joint + 1) * 16 <= bones->cols.size()) {
						M4 bone;
						std::memcpy(bone.m, &bones->cols[(size_t)group.joint * 16],
							    sizeof(float) * 16);
						at = point(bone, group.middle);
					} else if (!p.gpu && group.sample < p.pos.size()) {
						at = p.pos[group.sample];
					}
					part.groups.push_back({-point(v.view, at).z, (int)g});
				}
				std::stable_sort(part.groups.begin(), part.groups.end(),
						 [](const std::pair<float, int> &a, const std::pair<float, int> &b) {
							 return a.first > b.first;
						 });
				for (const auto &g : part.groups)
					part.order.push_back(g.second);
				if (hair[w].empty()) {
					// (Its place among what's see-through: its first strand
					// part's.)
					See s;
					s.queue = mat.queue;
					s.distance = dr.distance[i];
					s.w = w;
					s.hair = true;
					blended.push_back(s);
				}
				hair[w].push_back(std::move(part));
				continue;
			}
			if (mat.alpha == AlphaMode::Blend || v.opacity < 0.999f) {
				See s;
				s.queue = mat.queue;
				s.distance = dr.distance[i];
				s.w = w;
				s.i = i;
				blended.push_back(s);
				continue;
			}
			dr.a->part_distance_ = dr.distance[i];
			dr.a->set_material(mat, v, false, is_skin(dr, i), sides_shown(mat, p.handed) == Sides::Front);
			gs_set_cull_mode(cull_of(mat, p.handed));
			dr.a->draw_part(i, p, technique_of(dr, i));
			gs_set_cull_mode(GS_NEITHER);
		}
	}
	// Anything with it: its solid parts, then its see-through ones - with
	// hair in the picture, those in their turn with the rest that's
	// see-through (hairs aren't hidden by what's see-through: a thing
	// fading away in front of a beard is drawn after it).
	for (const PropAt &at : props) {
		if (!at.prop)
			continue;
		put(at);
		draw_prop(*at.prop, v, false);
		if (!any_hair) {
			draw_prop(*at.prop, v, true);
			continue;
		}
		See s;
		s.distance = -point(v.view, point(at.place, (at.prop->low() + at.prop->high()) * 0.5f)).z;
		s.prop = &at;
		blended.push_back(s);
	}
	std::stable_sort(blended.begin(), blended.end(), [](const See &a, const See &b) {
		if (a.queue != b.queue)
			return a.queue < b.queue;
		return a.distance > b.distance;
	});
	size_t last_hair = blended.size();
	for (size_t at = 0; at < blended.size(); at++)
		if (blended[at].hair)
			last_hair = at;
	// Hidden hair left out before it's drawn (the -skin effect's
	// strand_shown; Direct3D): the picture of the solid parts' distances
	// given to the card's vertex stage too, while the hair's drawn.
	const bool skips = any_hair && any_card_hair && depth_map && hair_plan_.skip_hidden > 0 && p_strand_view &&
			   card_picture_put(depth_map);
	hair_drawn_.skips = skips;
	for (size_t at = 0; at < blended.size(); at++) {
		const See &s = blended[at];
		if (s.prop) {
			put(*s.prop);
			draw_prop(*s.prop->prop, v, true);
			continue;
		}
		Drawn &dr = all[s.w];
		if (s.hair) {
			// All its hair strands, a layer at a time from the roots out
			// (the tips lie over the roots), and in each layer the far
			// parts and groups first. Both sides, no depth test (the
			// picture of the solid parts' distances hides them).
			std::vector<HairPart> &parts = hair[s.w];
			std::stable_sort(parts.begin(), parts.end(),
					 [](const HairPart &a, const HairPart &b) { return a.distance > b.distance; });
			size_t layers = 0;
			for (const HairPart &part : parts)
				layers = std::max<size_t>(layers, (size_t)(*dr.posed)[part.i].prim->hair.layers);
			if (clock && hair_timer == 0) {
				gs_timer_begin(clock->hair);
				hair_timer = 1;
			}
			gs_set_cull_mode(GS_NEITHER);
			gs_enable_depth_test(false);
			// (A measure - HairPlan::measure 1: all of it into one pixel
			// of the picture's corner.)
			const bool one_pixel = hair_plan_.measure == 1;
			if (one_pixel) {
				const gs_rect corner = {0, 0, 1, 1};
				gs_set_scissor_rect(&corner);
			}
			for (size_t layer = 0; layer < layers; layer++)
				for (const HairPart &part : parts) {
					const Rig::Part &p = (*dr.posed)[part.i];
					const Part &form = dr.a->parts_[part.i];
					dr.a->part_distance_ = part.distance;
					// (Its thin pieces alone as lines: its wide ones
					// first, as ribbons - under them, as the deep coat
					// they stand for is. Everything the effect reads is
					// given again for the second: a technique's end
					// empties it.)
					for (int sort = form.thin_lines ? 0 : -1; sort < (form.thin_lines ? 2 : 0); sort++) {
						const bool lines = form.as_lines || sort == 1;
						dr.a->set_material(material_of(dr, part.i), v, false, false, false);
						dr.a->set_strand(v, drawn_h, dr.hair[part.i].upto, p.scale, depth_map,
								 lines, skips && p.gpu ? hair_plan_.skip_hidden : 0);
						// (Measures 2 and 3: less of each corner read.)
						const char *technique = !p.gpu                    ? "Strand"
									: hair_plan_.measure == 2 ? "StrandSkLean"
									: hair_plan_.measure == 3 ? "StrandSkBare"
												  : "StrandSk";
						dr.a->draw_strands(part.i, p, part.order, layer, dr.hair[part.i].count,
								   technique, lines, sort, hair_drawn_);
					}
				}
			if (one_pixel)
				gs_set_scissor_rect(nullptr);
			gs_enable_depth_test(true);
			if (clock && hair_timer == 1 && at >= last_hair) {
				gs_timer_end(clock->hair);
				hair_timer = 2;
			}
			continue;
		}
		const size_t i = s.i;
		const Material &mat = material_of(dr, i);
		const Rig::Part &p = (*dr.posed)[i];
		dr.a->part_distance_ = dr.distance[i];
		dr.a->set_material(mat, v, false, is_skin(dr, i), sides_shown(mat, p.handed) == Sides::Front);
		gs_set_cull_mode(cull_of(mat, p.handed));
		// A glowing part (0.9.0: a puff of fire or smoke): all of it at its
		// middle's depth, so it never cuts into itself or the puffs before
		// it (drawn far to near, each nearer one passes).
		if (mat.glow) {
			dr.a->set_flat(&dr.middle[i]);
			gs_depth_function(GS_LEQUAL);
		}
		dr.a->draw_part(i, p, technique_of(dr, i));
		if (mat.glow) {
			dr.a->set_flat(nullptr);
			gs_depth_function(GS_LESS);
		}
		gs_set_cull_mode(GS_NEITHER);
	}
	if (skips)
		card_picture_take();
	stop_clock();

	gs_blend_state_pop();
	gs_enable_depth_test(false);
	gs_set_cull_mode(cull);
	gs_texrender_end(scene_);
	gs_texture_t *big = gs_texrender_get_texture(scene_);
	if (k == 1 || !big)
		return big;

	if (!out_)
		out_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (!out_)
		return big;
	gs_texrender_reset(out_);
	if (!gs_texrender_begin(out_, v.w, v.h))
		return big;
	gs_clear(GS_CLEAR_COLOR, &clear_colour, 0.0f, 0);
	gs_ortho(0.0f, (float)v.w, 0.0f, (float)v.h, -100.0f, 100.0f);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	vec2 texel;
	vec2_set(&texel, 1.0f / (v.w * k), 1.0f / (v.h * k));
	gs_effect_set_texture(p_image, big);
	gs_effect_set_vec2(p_texel, &texel);
	check_("Down");
	while (gs_effect_loop(effect_, "Down"))
		gs_draw_sprite(big, 0, v.w, v.h);
	gs_blend_state_pop();
	gs_texrender_end(out_);
	return gs_texrender_get_texture(out_);
}

} // namespace direbot::face
