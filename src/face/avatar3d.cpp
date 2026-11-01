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

#include <algorithm>
#include <atomic>
#include <cmath>
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

// Pictures at most this many pixels across on the graphics card.
constexpr uint32_t MOST_PICTURE = 2048;
// Drawn twice the size for smooth edges, unless that's past this.
constexpr uint32_t MOST_DRAWN = 8192;

uint32_t next_power(uint32_t v)
{
	uint32_t p = 1;
	while (p < v && p < MOST_PICTURE)
		p <<= 1;
	return p;
}

// Half the size: each pixel the average of 2 x 2 (colours weighted by how
// solid they are, so see-through edges don't darken).
std::vector<uint8_t> halved(const std::vector<uint8_t> &px, uint32_t w, uint32_t h, uint32_t &hw, uint32_t &hh)
{
	hw = std::max(1u, w / 2);
	hh = std::max(1u, h / 2);
	std::vector<uint8_t> out((size_t)hw * hh * 4);
	for (uint32_t y = 0; y < hh; y++)
		for (uint32_t x = 0; x < hw; x++) {
			float c[3] = {0, 0, 0}, a = 0, plain[3] = {0, 0, 0};
			int n = 0;
			for (uint32_t dy = 0; dy < 2; dy++)
				for (uint32_t dx = 0; dx < 2; dx++) {
					const uint32_t sx = std::min(w - 1, x * 2 + dx),
						       sy = std::min(h - 1, y * 2 + dy);
					const uint8_t *p = &px[((size_t)sy * w + sx) * 4];
					const float pa = p[3] / 255.0f;
					for (int k = 0; k < 3; k++) {
						c[k] += p[k] * pa;
						plain[k] += p[k];
					}
					a += pa;
					n++;
				}
			uint8_t *o = &out[((size_t)y * hw + x) * 4];
			for (int k = 0; k < 3; k++)
				o[k] = (uint8_t)std::lround(a > 1e-4f ? c[k] / a : plain[k] / n);
			o[3] = (uint8_t)std::lround(a / n * 255);
		}
	return out;
}

std::vector<uint8_t> resized(const uint8_t *px, uint32_t w, uint32_t h, uint32_t to)
{
	// Halved first while it's over twice the size, then smoothly read.
	std::vector<uint8_t> cur(px, px + (size_t)w * h * 4);
	while (w >= to * 2 && h >= to * 2) {
		uint32_t nw, nh;
		cur = halved(cur, w, h, nw, nh);
		w = nw;
		h = nh;
	}
	std::vector<uint8_t> out((size_t)to * to * 4);
	for (uint32_t y = 0; y < to; y++) {
		const float fy = std::max(0.0f, (y + 0.5f) * h / to - 0.5f);
		const uint32_t y0 = std::min(h - 1, (uint32_t)fy), y1 = std::min(h - 1, y0 + 1);
		const float ty = fy - y0;
		for (uint32_t x = 0; x < to; x++) {
			const float fx = std::max(0.0f, (x + 0.5f) * w / to - 0.5f);
			const uint32_t x0 = std::min(w - 1, (uint32_t)fx), x1 = std::min(w - 1, x0 + 1);
			const float tx = fx - x0;
			for (int k = 0; k < 4; k++) {
				auto at = [&](uint32_t xx, uint32_t yy) {
					return (float)cur[((size_t)yy * w + xx) * 4 + k];
				};
				const float v = (at(x0, y0) * (1 - tx) + at(x1, y0) * tx) * (1 - ty) +
						(at(x0, y1) * (1 - tx) + at(x1, y1) * tx) * ty;
				out[((size_t)y * to + x) * 4 + k] =
					(uint8_t)std::lround(std::min(255.0f, std::max(0.0f, v)));
			}
		}
	}
	return out;
}

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

ModelPicture picture_from(const uint8_t *rgba, uint32_t w, uint32_t h)
{
	ModelPicture pic;
	if (!rgba || !w || !h)
		return pic;
	const uint32_t size = next_power(std::max(w, h));
	pic.size = size;
	pic.levels.push_back(resized(rgba, w, h, size));
	uint32_t s = size;
	while (s > 1) {
		uint32_t nw, nh;
		pic.levels.push_back(halved(pic.levels.back(), s, s, nw, nh));
		s = nw;
	}
	return pic;
}

std::vector<ModelPicture> read_pictures(const Model &m)
{
	static std::atomic<unsigned> counter{0};
	std::vector<ModelPicture> out(m.images.size());
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
		uint8_t *data = gs_create_texture_file_data(path.string().c_str(), &format, &w, &h);
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
			out[i] = picture_from(rgba.data(), w, h);
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

void ModelLoader::want(const std::string &path, bool again)
{
	{
		std::lock_guard<std::mutex> l(lock_);
		if (path == asked_ && !again)
			return;
		asked_ = path;
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
		{
			std::unique_lock<std::mutex> l(lock_);
			wake_.wait(l, [this] { return stop_ || pending_; });
			if (stop_)
				return;
			path = want_;
			pending_ = false;
			working_ = true;
		}
		auto pack = std::make_shared<ModelPack>();
		pack->path = path;
		if (!path.empty()) {
			auto model = std::make_shared<Model>();
			std::string problem;
			if (load_model(path, *model, &problem)) {
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

Avatar3D::Avatar3D()
{
	char *path = obs_module_file("effects/direbot-face-3d.effect");
	if (path) {
		char *errors = nullptr;
		effect_ = gs_effect_create_from_file(path, &errors);
		if (!effect_)
			obs_log(LOG_WARNING, "DireBot Face Avatar: the 3D effect didn't load: %s",
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
		check_.load(effect_, "3D", face_3d_needs());
	}
	const uint32_t white = 0xFFFFFFFF, black = 0xFF000000;
	const uint8_t *w = reinterpret_cast<const uint8_t *>(&white);
	const uint8_t *b = reinterpret_cast<const uint8_t *>(&black);
	white_ = gs_texture_create(1, 1, GS_RGBA, 1, &w, 0);
	black_ = gs_texture_create(1, 1, GS_RGBA, 1, &b, 0);
	make_head();
}

Avatar3D::~Avatar3D()
{
	clear();
	gs_vertexbuffer_destroy(head_.vb);
	gs_indexbuffer_destroy(head_.ib);
	gs_texture_destroy(white_);
	gs_texture_destroy(black_);
	gs_texrender_destroy(scene_);
	gs_texrender_destroy(out_);
	gs_effect_destroy(effect_);
}

void Avatar3D::clear()
{
	for (gs_texture_t *t : textures_)
		gs_texture_destroy(t);
	textures_.clear();
	for (Part &p : parts_) {
		gs_vertexbuffer_destroy(p.vb);
		gs_indexbuffer_destroy(p.ib);
	}
	parts_.clear();
	model_.reset();
}

namespace {

// A buffer of `n` points; a still one (not `dynamic`) is filled from
// `points` now, as OBS keeps no copy of it to change later.
gs_vertbuffer_t *make_vb(size_t n, bool dynamic, const V3 *points = nullptr)
{
	gs_vb_data *vb = gs_vbdata_create();
	vb->num = n;
	vb->points = static_cast<vec3 *>(bzalloc(sizeof(vec3) * n));
	vb->normals = static_cast<vec3 *>(bzalloc(sizeof(vec3) * n));
	for (size_t i = 0; points && i < n; i++) {
		vec3_set(&vb->points[i], points[i].x, points[i].y, points[i].z);
		vec3_set(&vb->normals[i], 0, 0, 1);
	}
	vb->colors = static_cast<uint32_t *>(bzalloc(sizeof(uint32_t) * n));
	vb->num_tex = 1;
	vb->tvarray = static_cast<gs_tvertarray *>(bzalloc(sizeof(gs_tvertarray)));
	vb->tvarray[0].width = 2;
	vb->tvarray[0].array = bzalloc(sizeof(float) * 2 * n);
	return gs_vertexbuffer_create(vb, dynamic ? GS_DYNAMIC : 0);
}

gs_indexbuffer_t *make_ib(const std::vector<uint32_t> &index)
{
	uint32_t *ix = static_cast<uint32_t *>(bmalloc(sizeof(uint32_t) * index.size()));
	std::memcpy(ix, index.data(), sizeof(uint32_t) * index.size());
	return gs_indexbuffer_create(GS_UNSIGNED_LONG, ix, index.size(), 0);
}

} // namespace

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
	const Model &m = *model_;
	for (const Node &node : m.nodes) {
		if (node.mesh < 0)
			continue;
		for (const Primitive &p : m.meshes[node.mesh].parts) {
			Part part;
			part.points = p.pos.size();
			if (part.points && !p.index.empty()) {
				part.vb = make_vb(part.points, true);
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
}

void Avatar3D::set_material(const Material &m, const View3D &v, bool outline)
{
	// Everything a technique reads, every time: OBS empties them all
	// when the technique before finishes (see effect-needs.hpp).
	set_camera();
	auto tex = [this](const TexRef &r, gs_texture_t *none) {
		if (r.image >= 0 && r.image < (int)textures_.size() && textures_[r.image])
			return textures_[r.image];
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
	set_vec4(p_base, m.base[0], m.base[1], m.base[2], m.base[3]);
	set_vec3(p_emissive, m.emissive);
	set_vec4(p_surface, m.metallic, m.roughness, m.alpha == AlphaMode::Mask ? std::max(0.001f, m.cutoff) : 0.0f,
		 m.alpha == AlphaMode::Blend ? 1.0f : 0.0f);
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
	gs_load_vertexbuffer(part.vb);
	gs_load_indexbuffer(part.ib);
	check_(technique);
	while (gs_effect_loop(effect_, technique))
		gs_draw(GS_TRIS, 0, 0);
	(void)p;
}

gs_texture_t *Avatar3D::draw(const Rig &rig, const View3D &v)
{
	if (!effect_ || !model_ || &rig.model() != model_.get() || !v.w || !v.h)
		return nullptr;
	const std::vector<Rig::Part> &posed = rig.parts();
	if (posed.size() != parts_.size())
		return nullptr;
	// The posed points onto the graphics card.
	for (size_t i = 0; i < parts_.size(); i++) {
		Part &part = parts_[i];
		const Rig::Part &p = posed[i];
		if (!part.vb || p.pos.size() != part.points)
			continue;
		gs_vb_data *d = gs_vertexbuffer_get_data(part.vb);
		for (size_t k = 0; k < part.points; k++) {
			vec3_set(&d->points[k], p.pos[k].x, p.pos[k].y, p.pos[k].z);
			vec3_set(&d->normals[k], p.normal[k].x, p.normal[k].y, p.normal[k].z);
		}
		gs_vertexbuffer_flush(part.vb);
	}

	const uint32_t k = v.smooth && v.w * 2 <= MOST_DRAWN && v.h * 2 <= MOST_DRAWN ? 2 : 1;
	if (!scene_)
		scene_ = gs_texrender_create(GS_RGBA, GS_Z24_S8);
	if (!scene_)
		return nullptr;
	opacity_ = v.opacity;
	outline_share_ = v.outline;
	height_ = v.height;
	world_scale_ = v.world_scale;
	fov_ = v.fov;

	// Each part's distance from the camera (its middle), for the see-through
	// parts' order and screen-sized outlines.
	std::vector<float> distance(posed.size(), 1);
	for (size_t i = 0; i < posed.size(); i++) {
		const std::vector<V3> &pos = posed[i].pos;
		V3 sum;
		size_t n = 0;
		for (size_t j = 0; j < pos.size(); j += 17, n++)
			sum = sum + pos[j];
		if (n)
			distance[i] = std::max(1e-4f, -point(v.view, sum * (1.0f / n)).z);
	}

	gs_texrender_reset(scene_);
	if (!gs_texrender_begin(scene_, v.w * k, v.h * k))
		return nullptr;
	vec4 clear_colour;
	vec4_zero(&clear_colour);
	gs_clear(GS_CLEAR_COLOR | GS_CLEAR_DEPTH, &clear_colour, 1.0f, 0);
	gs_perspective(v.fov * 180 / 3.14159265f, (float)v.w / v.h, v.near, v.far);
	// The camera's view as OBS's 3D wants it: y down, looking along -z.
	// The same on Direct3D and OpenGL: OBS's Direct3D turns z round
	// itself, so turning it here too would put the model behind the
	// camera.
	const matrix4 view = obs_matrix(obs_view(v.view));
	gs_matrix_set(&view);
	cam_ = v.view;

	const gs_cull_mode cull = gs_get_cull_mode();
	gs_set_cull_mode(GS_NEITHER);
	gs_enable_depth_test(true);
	gs_depth_function(GS_LESS);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	if (v.hide_behind_head && head_.vb && head_.ib) {
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

	static const Material plain;
	const Model &m = *model_;
	auto material_of = [&](size_t i) -> const Material & {
		const int mi = posed[i].prim->material;
		return mi >= 0 && mi < (int)m.materials.size() ? m.materials[mi] : plain;
	};
	// Outlines (only their far side, so they show round the edges), then
	// the solid parts over them, then the see-through ones far to near.
	// The outline setting draws one round every part - but a character
	// with outlines of its own keeps them off the parts it has none on (a
	// face, its eyes).
	bool own_outlines = false;
	for (const Material &mat : m.materials)
		own_outlines = own_outlines || mat.outline != Outline::None;
	std::vector<size_t> blended;
	for (size_t i = 0; i < posed.size(); i++) {
		const Material &mat = material_of(i);
		if (mat.outline == Outline::None && (v.outline <= 0 || own_outlines))
			continue;
		part_distance_ = distance[i];
		set_material(mat, v, true);
		draw_part(i, posed[i], "Outline");
	}
	for (size_t i = 0; i < posed.size(); i++) {
		const Material &mat = material_of(i);
		if (mat.alpha == AlphaMode::Blend || v.opacity < 0.999f) {
			blended.push_back(i);
			continue;
		}
		part_distance_ = distance[i];
		set_material(mat, v, false);
		draw_part(i, posed[i], "Draw");
	}
	std::stable_sort(blended.begin(), blended.end(), [&](size_t a, size_t b) {
		const int qa = material_of(a).queue, qb = material_of(b).queue;
		if (qa != qb)
			return qa < qb;
		return distance[a] > distance[b];
	});
	for (size_t i : blended) {
		part_distance_ = distance[i];
		set_material(material_of(i), v, false);
		draw_part(i, posed[i], "Draw");
	}

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
