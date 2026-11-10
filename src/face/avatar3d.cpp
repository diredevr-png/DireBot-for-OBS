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
#include "files.hpp"

#include <algorithm>
#include <atomic>
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

void ModelLoader::want(const std::string &path, bool again)
{
	// The same file changed since (made again in its place): loaded anew.
	const std::string stamp = file_stamp(path);
	{
		std::lock_guard<std::mutex> l(lock_);
		if (path == asked_ && stamp == stamp_ && !again)
			return;
		asked_ = path;
		stamp_ = stamp;
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
		if (load_model(path, *model, &problem)) {
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
}

Avatar3D::~Avatar3D()
{
	clear();
	gs_vertexbuffer_destroy(head_.vb);
	gs_indexbuffer_destroy(head_.ib);
	gs_texture_destroy(white_);
	gs_texture_destroy(black_);
	gs_texture_destroy(flat_);
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
	bone_cols_.clear();
	model_.reset();
}

namespace {

// A buffer of `n` points (with tangents: `tangents`); a still one (not
// `dynamic`) is filled from `points` now, as OBS keeps no copy of it to
// change later.
gs_vertbuffer_t *make_vb(size_t n, bool dynamic, const V3 *points = nullptr, bool tangents = false,
			 bool strands = false, bool skin = false)
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
	// Hair strands: their widths and own numbers too (a second set).
	// Posed on the graphics card: each point's 4 bones and how much each
	// moves it (after those).
	vb->num_tex = (strands ? 2 : 1) + (skin ? 2 : 0);
	vb->tvarray = static_cast<gs_tvertarray *>(bzalloc(sizeof(gs_tvertarray) * vb->num_tex));
	for (size_t t = 0; t < vb->num_tex; t++) {
		vb->tvarray[t].width = t < (strands ? 2u : 1u) ? 2 : 4;
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
	make_parts();
}

void Avatar3D::set_gpu_skin(bool on)
{
	const bool now = can_gpu_skin_ && on;
	if (now == gpu_skin_)
		return;
	gpu_skin_ = now;
	// Its parts made again for the new way (its pictures kept).
	for (Part &p : parts_) {
		gs_vertexbuffer_destroy(p.vb);
		gs_indexbuffer_destroy(p.ib);
	}
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
			part.strands = p.strands && p.strand.size() == part.points * 2;
			if (part.points && !p.index.empty()) {
				part.skin = gpu_skin_ && (node.skin < 0 || m.skins[node.skin].joints.size() <=
										   Rig::GPU_MOST_BONES);
				part.vb = make_vb(part.points, true, nullptr, part.bumped, part.strands, part.skin);
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
				if (part.strands)
					std::memcpy(d->tvarray[1].array, p.strand.data(), sizeof(float) * 2 * part.points);
				if (part.skin) {
					// Its points as made, and its bones (none: its node's
					// world as bone 0, all of it).
					const size_t first = part.strands ? 2 : 1;
					float *jt = static_cast<float *>(d->tvarray[first].array);
					float *wt = static_cast<float *>(d->tvarray[first + 1].array);
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

void Avatar3D::set_strand(const View3D &v, uint32_t drawn_w, uint32_t drawn_h)
{
	const M4 to_world = inverse(cam_);
	const V3 eye = move_of(to_world);
	set_vec3(p_eye, &eye.x);
	// One pixel's width a unit from the camera; hairs at least 1 pixel of
	// the picture as drawn (twice the size: half a pixel of the one shown).
	const float pixel = 2 * std::tan(v.fov / 2) / std::max(1u, drawn_h);
	set_vec4(p_strand, pixel, 1.0f, (float)drawn_w, (float)drawn_h);
}

void Avatar3D::draw_prop(Prop3D &prop, const View3D &v, bool see_through)
{
	if (!(prop.opacity_ > 0.001f))
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
				for (size_t k = 0; k < part.points && k < t.pos.size(); k++)
					dp[k] = direction(at(k), t.pos[k]);
				if (t.normal.size() == part.points) {
					dn.resize(part.points);
					for (size_t k = 0; k < part.points; k++)
						dn[k] = direction(at(k), t.normal[k]);
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
			// (and once more as made when they stop).
			if (p.morphed || part.was_morphed || part.posed_here) {
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

gs_texture_t *Avatar3D::draw(const Rig *rig, const View3D &v, const std::vector<PropAt> &props,
			     const std::vector<Other> &others)
{
	const bool character = upload(rig);
	// 0.9.0: the others beside it, each onto the graphics card too.
	struct Drawn {
		Avatar3D *a;
		const std::vector<Rig::Part> *posed;
		std::vector<float> distance;
		std::vector<V3> middle; // each part's middle, in the world (a glowing part's depth)
		bool own_outlines = false;
		bool main = false;
	};
	std::vector<Drawn> all;
	static const std::vector<Rig::Part> nothing;
	all.push_back({this, character ? &rig->parts() : &nothing, {}, {}, false, true});
	for (const Other &o : others)
		if (o.avatar && o.avatar != this && o.avatar->effect_ && o.avatar->upload(o.rig))
			all.push_back({o.avatar, &o.rig->parts(), {}, {}, false, false});
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

	const gs_cull_mode cull = gs_get_cull_mode();
	gs_set_cull_mode(GS_NEITHER);
	gs_enable_depth_test(true);
	gs_depth_function(GS_LESS);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	if (character && v.hide_behind_head && head_.vb && head_.ib) {
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
	auto material_of = [&](const Drawn &dr, size_t i) -> const Material & {
		const int mi = (*dr.posed)[i].prim->material;
		const Model *m = dr.a->model_.get();
		return m && mi >= 0 && mi < (int)m->materials.size() ? m->materials[mi] : plain;
	};
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
		if (dr.a->parts_[i].strands)
			return sk ? "StrandSk" : "Strand";
		if (dr.a->parts_[i].bumped && v.detail && !flat_look && mat.normal_map.used())
			return sk ? "DrawBumpedSk" : "DrawBumped";
		return sk ? "DrawSk" : "Draw";
	};
	auto shows = [&](const Drawn &dr, size_t i) {
		return !(*dr.posed)[i].hidden && !(*dr.posed)[i].collapsed;
	};
	// Outlines (only their far side, so they show round the edges), then
	// the solid parts over them, then the see-through ones far to near.
	// The outline setting draws one round every part - but a character
	// with outlines of its own keeps them off the parts it has none on (a
	// face, its eyes).
	std::vector<std::pair<size_t, size_t>> blended; // (which of `all`, its part)
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
			// Hair strands are drawn with the solid parts, however
			// see-through: their see-through is a share of their pixels.
			const bool strands = dr.a->parts_[i].strands;
			if (!strands && (mat.alpha == AlphaMode::Blend || v.opacity < 0.999f)) {
				blended.push_back({w, i});
				continue;
			}
			dr.a->part_distance_ = dr.distance[i];
			dr.a->set_material(mat, v, false, is_skin(dr, i), sides_shown(mat, p.handed) == Sides::Front);
			if (strands)
				dr.a->set_strand(v, v.w * k, v.h * k);
			gs_set_cull_mode(strands ? GS_NEITHER : cull_of(mat, p.handed));
			dr.a->draw_part(i, p, technique_of(dr, i));
			gs_set_cull_mode(GS_NEITHER);
		}
	}
	// Anything with it, each put where it goes (the same one may be in
	// several places): its solid parts, then its see-through ones.
	for (const PropAt &at : props) {
		if (!at.prop)
			continue;
		for (size_t n = 0; n < at.shapes.size(); n++)
			at.prop->set_shape((int)n, at.shapes[n]);
		at.prop->place(at.place, at.opacity);
		draw_prop(*at.prop, v, false);
		draw_prop(*at.prop, v, true);
	}
	std::stable_sort(blended.begin(), blended.end(),
			 [&](const std::pair<size_t, size_t> &a, const std::pair<size_t, size_t> &b) {
				 const int qa = material_of(all[a.first], a.second).queue,
					   qb = material_of(all[b.first], b.second).queue;
				 if (qa != qb)
					 return qa < qb;
				 return all[a.first].distance[a.second] > all[b.first].distance[b.second];
			 });
	for (const auto &bi : blended) {
		Drawn &dr = all[bi.first];
		const size_t i = bi.second;
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
