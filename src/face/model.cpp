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

#include "model.hpp"
#include "files.hpp"

#include <mikktspace/mikktspace.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <set>

#include <nlohmann/json.hpp>

namespace direbot::face {

namespace {

using json = nlohmann::json;

// The most a file may hold, so a hostile one can't use up the PC.
constexpr size_t MOST_FILE = 512u << 20;
constexpr size_t MOST_POINTS = 4'000'000;
constexpr size_t MOST_INDEX = 24'000'000;
constexpr size_t MOST_NODES = 100'000;
constexpr size_t MOST_MORPHS = 512;
constexpr size_t MOST_IMAGES = 256;
constexpr size_t MOST_ANIMATIONS = 256;
constexpr size_t MOST_KEY_NUMBERS = 8'000'000; // all its animations' keys' numbers

struct Fail {
	std::string why;
};
[[noreturn]] void fail(const std::string &why)
{
	throw Fail{why};
}

const json &field(const json &j, const char *key)
{
	static const json none;
	if (!j.is_object())
		return none;
	auto it = j.find(key);
	return it == j.end() ? none : *it;
}
float num(const json &j, const char *key, float def)
{
	const json &v = field(j, key);
	if (!v.is_number())
		return def;
	const double d = v.get<double>();
	return std::isfinite(d) ? (float)d : def;
}
int index_of(const json &j, const char *key, int def = -1)
{
	const json &v = field(j, key);
	if (!v.is_number_integer() && !v.is_number_unsigned())
		return def;
	const long long i = v.get<long long>();
	return i < 0 || i > std::numeric_limits<int>::max() ? def : (int)i;
}
bool flag(const json &j, const char *key, bool def)
{
	const json &v = field(j, key);
	return v.is_boolean() ? v.get<bool>() : def;
}
std::string text(const json &j, const char *key)
{
	const json &v = field(j, key);
	return v.is_string() ? v.get<std::string>() : std::string();
}
// Up to `n` numbers from a list into `out` (the rest kept).
void numbers(const json &j, const char *key, float *out, int n)
{
	const json &v = field(j, key);
	if (!v.is_array())
		return;
	for (int i = 0; i < n && i < (int)v.size(); i++)
		if (v[i].is_number()) {
			const double d = v[i].get<double>();
			if (std::isfinite(d))
				out[i] = (float)d;
		}
}
const json &list(const json &j, const char *key)
{
	static const json none = json::array();
	const json &v = field(j, key);
	return v.is_array() ? v : none;
}

// sRGB (as VRM 0.x and colour pickers have colours) to linear.
float linear(float c)
{
	c = std::clamp(c, 0.0f, 1.0f);
	return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

std::vector<uint8_t> base64(const std::string &s, size_t from)
{
	static int8_t value[256];
	static bool made = false;
	if (!made) {
		std::fill(value, value + 256, (int8_t)-1);
		const char *a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		for (int i = 0; i < 64; i++)
			value[(uint8_t)a[i]] = (int8_t)i;
		value[(uint8_t)'-'] = 62;
		value[(uint8_t)'_'] = 63;
		made = true;
	}
	std::vector<uint8_t> out;
	out.reserve((s.size() - from) * 3 / 4);
	uint32_t acc = 0;
	int bits = 0;
	for (size_t i = from; i < s.size(); i++) {
		const int8_t v = value[(uint8_t)s[i]];
		if (v < 0) {
			if (s[i] == '=')
				break;
			continue; // spaces and line breaks
		}
		acc = (acc << 6) | (uint32_t)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out.push_back((uint8_t)(acc >> bits));
		}
	}
	return out;
}

bool read_file(const std::string &path, std::vector<uint8_t> &out)
{
	std::ifstream f(utf8_path(path), std::ios::binary);
	if (!f)
		return false;
	f.seekg(0, std::ios::end);
	const std::streamoff size = f.tellg();
	if (size < 0 || (size_t)size > MOST_FILE)
		return false;
	f.seekg(0);
	out.resize((size_t)size);
	return size == 0 || (bool)f.read(reinterpret_cast<char *>(out.data()), size);
}

std::string percent_decoded(const std::string &s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) &&
		    std::isxdigit((unsigned char)s[i + 2])) {
			out.push_back((char)std::stoi(s.substr(i + 1, 2), nullptr, 16));
			i += 2;
		} else {
			out.push_back(s[i]);
		}
	}
	return out;
}

struct Reader {
	const json &doc;
	std::vector<std::vector<uint8_t>> buffers;
	std::string folder;
	bool glb = false;
	std::vector<uint8_t> bin;

	explicit Reader(const json &d) : doc(d) {}

	// A data: link or a file beside the .gltf.
	std::vector<uint8_t> load_uri(const std::string &uri)
	{
		if (uri.rfind("data:", 0) == 0) {
			const size_t comma = uri.find(',');
			if (comma == std::string::npos || uri.find(";base64") > comma)
				fail("a part of the file is stored in a way DireBot can't read");
			return base64(uri, comma + 1);
		}
		if (uri.find("://") != std::string::npos)
			fail("the file needs something from the internet (" + uri + ")");
		if (folder.empty())
			fail("the file needs another file beside it (" + uri + ")");
		std::vector<uint8_t> bytes;
		if (!read_file(folder + "/" + percent_decoded(uri), bytes))
			fail("the file needs \"" + percent_decoded(uri) + "\" beside it");
		return bytes;
	}

	struct View {
		const uint8_t *data = nullptr;
		size_t size = 0, stride = 0;
	};
	View view(int i)
	{
		const json &views = list(doc, "bufferViews");
		if (i < 0 || i >= (int)views.size())
			fail("the file is damaged (a missing buffer view)");
		const json &v = views[i];
		const int b = index_of(v, "buffer");
		if (b < 0 || b >= (int)buffers.size())
			fail("the file is damaged (a missing buffer)");
		const double off = num(v, "byteOffset", 0), len = num(v, "byteLength", -1);
		if (off < 0 || len < 0 || off + len > (double)buffers[b].size())
			fail("the file is damaged (a buffer view past its end)");
		View out;
		out.data = buffers[b].data() + (size_t)off;
		out.size = (size_t)len;
		const int stride = index_of(v, "byteStride", 0);
		out.stride = stride > 0 && stride <= 252 ? (size_t)stride : 0;
		return out;
	}

	static int comps_of(const std::string &type)
	{
		if (type == "SCALAR")
			return 1;
		if (type == "VEC2")
			return 2;
		if (type == "VEC3")
			return 3;
		if (type == "VEC4" || type == "MAT2")
			return 4;
		if (type == "MAT3")
			return 9;
		if (type == "MAT4")
			return 16;
		return 0;
	}
	static int size_of(int type)
	{
		switch (type) {
		case 5120:
		case 5121:
			return 1;
		case 5122:
		case 5123:
			return 2;
		case 5125:
		case 5126:
			return 4;
		default:
			return 0;
		}
	}
	static double value(const uint8_t *p, int type, bool normalized)
	{
		switch (type) {
		case 5120: {
			const int8_t v = (int8_t)*p;
			return normalized ? std::max(v / 127.0, -1.0) : v;
		}
		case 5121:
			return normalized ? *p / 255.0 : *p;
		case 5122: {
			int16_t v;
			std::memcpy(&v, p, 2);
			return normalized ? std::max(v / 32767.0, -1.0) : v;
		}
		case 5123: {
			uint16_t v;
			std::memcpy(&v, p, 2);
			return normalized ? v / 65535.0 : v;
		}
		case 5125: {
			uint32_t v;
			std::memcpy(&v, p, 4);
			return v;
		}
		default: {
			float v;
			std::memcpy(&v, p, 4);
			return std::isfinite(v) ? v : 0.0;
		}
		}
	}

	// An accessor's numbers, `comps` a point, as doubles (with the
	// integers kept whole). `count_out`: how many points.
	std::vector<double> read(int a, int want_comps, size_t &count_out)
	{
		const json &accessors = list(doc, "accessors");
		if (a < 0 || a >= (int)accessors.size())
			fail("the file is damaged (a missing accessor)");
		const json &acc = accessors[a];
		const int type = index_of(acc, "componentType");
		const int comps = comps_of(text(acc, "type"));
		const int csize = size_of(type);
		if (!csize || !comps || (want_comps && comps != want_comps))
			fail("the file has numbers DireBot can't read");
		const long long count = (long long)num(acc, "count", -1);
		if (count < 0 || (size_t)count > MOST_INDEX)
			fail("the file is too big or damaged");
		const bool normalized = flag(acc, "normalized", false);
		std::vector<double> out((size_t)count * comps, 0.0);
		const int view_i = index_of(acc, "bufferView");
		if (view_i >= 0 && count > 0) {
			const View v = view(view_i);
			const double off = num(acc, "byteOffset", 0);
			const size_t elem = (size_t)comps * csize;
			// (Small matrices of bytes pad their columns; nothing here reads them.)
			if ((comps == 4 && text(acc, "type") == "MAT2" && csize < 4) || (comps == 9 && csize < 4))
				fail("the file has numbers DireBot can't read");
			const size_t stride = v.stride ? v.stride : elem;
			if (off < 0 || off + (double)stride * (count - 1) + elem > (double)v.size)
				fail("the file is damaged (numbers past their end)");
			for (long long i = 0; i < count; i++) {
				const uint8_t *p = v.data + (size_t)off + (size_t)i * stride;
				for (int c = 0; c < comps; c++)
					out[(size_t)i * comps + c] = value(p + (size_t)c * csize, type, normalized);
			}
		}
		const json &sparse = field(acc, "sparse");
		if (sparse.is_object()) {
			const long long n = (long long)num(sparse, "count", 0);
			const json &ix = field(sparse, "indices"), &vals = field(sparse, "values");
			const int itype = index_of(ix, "componentType");
			const int isize = size_of(itype);
			if (n < 0 || n > count || !isize || itype == 5126)
				fail("the file is damaged (sparse numbers)");
			if (n > 0) {
				const View iv = view(index_of(ix, "bufferView"));
				const View vv = view(index_of(vals, "bufferView"));
				const double ioff = num(ix, "byteOffset", 0), voff = num(vals, "byteOffset", 0);
				if (ioff < 0 || voff < 0 || ioff + (double)n * isize > (double)iv.size ||
				    voff + (double)n * comps * csize > (double)vv.size)
					fail("the file is damaged (sparse numbers past their end)");
				for (long long k = 0; k < n; k++) {
					const double at =
						value(iv.data + (size_t)ioff + (size_t)k * isize, itype, false);
					if (at < 0 || at >= (double)count)
						fail("the file is damaged (sparse numbers)");
					for (int c = 0; c < comps; c++)
						out[(size_t)at * comps + c] =
							value(vv.data + (size_t)voff + ((size_t)k * comps + c) * csize,
							      type, normalized);
				}
			}
		}
		count_out = (size_t)count;
		return out;
	}

	std::vector<V3> vec3s(int a)
	{
		size_t n = 0;
		const std::vector<double> d = read(a, 3, n);
		std::vector<V3> out(n);
		for (size_t i = 0; i < n; i++)
			out[i] = {(float)d[i * 3], (float)d[i * 3 + 1], (float)d[i * 3 + 2]};
		return out;
	}
	std::vector<float> floats(int a, int comps)
	{
		size_t n = 0;
		const std::vector<double> d = read(a, comps, n);
		return std::vector<float>(d.begin(), d.end());
	}
};

// The triangles of a strip or fan, as a list.
std::vector<uint32_t> triangles_of(const std::vector<uint32_t> &ix, int mode)
{
	if (mode == 4)
		return ix;
	std::vector<uint32_t> out;
	for (size_t i = 2; i < ix.size(); i++) {
		if (mode == 5) {
			if (i % 2 == 0)
				out.insert(out.end(), {ix[i - 2], ix[i - 1], ix[i]});
			else
				out.insert(out.end(), {ix[i - 1], ix[i - 2], ix[i]});
		} else {
			out.insert(out.end(), {ix[0], ix[i - 1], ix[i]});
		}
	}
	return out;
}

void make_normals(Primitive &p)
{
	p.normal.assign(p.pos.size(), V3{});
	for (size_t i = 0; i + 2 < p.index.size(); i += 3) {
		const uint32_t a = p.index[i], b = p.index[i + 1], c = p.index[i + 2];
		const V3 n = cross(p.pos[b] - p.pos[a], p.pos[c] - p.pos[a]);
		p.normal[a] = p.normal[a] + n;
		p.normal[b] = p.normal[b] + n;
		p.normal[c] = p.normal[c] + n;
	}
	for (V3 &n : p.normal)
		n = normalized(n, {0, 0, 1});
}

TexRef tex_ref(const json &doc, const json &info)
{
	TexRef r;
	if (!info.is_object())
		return r;
	// Only pictures on the first UVs (the only ones read).
	if (index_of(info, "texCoord", 0) != 0)
		return r;
	const json &textures = list(doc, "textures");
	const int t = index_of(info, "index");
	if (t < 0 || t >= (int)textures.size())
		return r;
	const json &tex = textures[t];
	int image = index_of(tex, "source");
	// A picture only in a newer format (WebP, KTX2) is skipped.
	if (image < 0)
		return r;
	if (image >= (int)list(doc, "images").size())
		return r;
	r.image = image;
	const json &xf = field(field(info, "extensions"), "KHR_texture_transform");
	if (xf.is_object()) {
		numbers(xf, "offset", r.offset, 2);
		numbers(xf, "scale", r.scale, 2);
	}
	return r;
}

TexRef tex_index(const json &doc, int t)
{
	json info = json::object();
	info["index"] = t;
	return tex_ref(doc, info);
}

void read_mtoon1(const json &doc, const json &m, Material &mat)
{
	mat.shading = Shading::Toon;
	mat.shade[0] = mat.shade[1] = mat.shade[2] = 0;
	numbers(m, "shadeColorFactor", mat.shade, 3);
	mat.shade_map = tex_ref(doc, field(m, "shadeMultiplyTexture"));
	mat.shade_shift = num(m, "shadingShiftFactor", 0);
	mat.shade_toony = std::clamp(num(m, "shadingToonyFactor", 0.9f), 0.0f, 1.0f);
	numbers(m, "matcapFactor", mat.matcap, 3);
	mat.matcap_map = tex_ref(doc, field(m, "matcapTexture"));
	numbers(m, "parametricRimColorFactor", mat.rim, 3);
	mat.rim_map = tex_ref(doc, field(m, "rimMultiplyTexture"));
	mat.rim_mix = num(m, "rimLightingMixFactor", 1);
	mat.rim_power = std::max(0.0f, num(m, "parametricRimFresnelPowerFactor", 5));
	mat.rim_lift = num(m, "parametricRimLiftFactor", 0);
	const std::string mode = text(m, "outlineWidthMode");
	mat.outline = mode == "worldCoordinates"    ? Outline::World
		      : mode == "screenCoordinates" ? Outline::Screen
						    : Outline::None;
	mat.outline_width = std::max(0.0f, num(m, "outlineWidthFactor", 0));
	if (mat.outline_width <= 0)
		mat.outline = Outline::None;
	numbers(m, "outlineColorFactor", mat.outline_colour, 3);
	mat.outline_mix = num(m, "outlineLightingMixFactor", 1);
	mat.queue = index_of(m, "renderQueueOffsetNumber", 0);
}

// VRM 0.x's MToon numbers, turned into VRM 1.0's.
void read_mtoon0(const json &doc, const json &p, Material &mat)
{
	const json &f = field(p, "floatProperties"), &v = field(p, "vectorProperties"),
		   &t = field(p, "textureProperties");
	auto colour = [&](const char *key, float *out, int n, bool srgb) {
		float c[4] = {out[0], out[1], out[2], n > 3 ? out[3] : 1};
		numbers(v, key, c, 4);
		for (int i = 0; i < n; i++)
			out[i] = srgb && i < 3 ? linear(c[i]) : c[i];
	};
	auto texture = [&](const char *key) {
		const int i = index_of(t, key);
		return i >= 0 ? tex_index(doc, i) : TexRef();
	};
	mat.shading = Shading::Toon;
	colour("_Color", mat.base, 4, true);
	if (texture("_MainTex").used())
		mat.base_map = texture("_MainTex");
	colour("_ShadeColor", mat.shade, 3, true);
	mat.shade_map = texture("_ShadeTexture");
	const float shift = num(f, "_ShadeShift", 0), toony = std::clamp(num(f, "_ShadeToony", 0.9f), 0.0f, 1.0f);
	const float lo = shift, hi = 1 + (shift - 1) * toony;
	mat.shade_toony = std::clamp(1 - (hi - lo) / 2, 0.0f, 1.0f);
	mat.shade_shift = -(lo + hi) / 2;
	colour("_RimColor", mat.rim, 3, true);
	mat.rim_map = texture("_RimTexture");
	mat.rim_mix = num(f, "_RimLightingMix", 0);
	mat.rim_power = std::max(0.0f, num(f, "_RimFresnelPower", 1));
	mat.rim_lift = num(f, "_RimLift", 0);
	const TexRef sphere = texture("_SphereAdd");
	if (sphere.used()) {
		mat.matcap_map = sphere;
	} else {
		mat.matcap[0] = mat.matcap[1] = mat.matcap[2] = 0;
	}
	float emission[3] = {mat.emissive[0], mat.emissive[1], mat.emissive[2]};
	colour("_EmissionColor", emission, 3, true);
	std::copy(emission, emission + 3, mat.emissive);
	if (texture("_EmissionMap").used())
		mat.emissive_map = texture("_EmissionMap");
	const int mode = (int)num(f, "_OutlineWidthMode", 0);
	mat.outline = mode == 1 ? Outline::World : mode == 2 ? Outline::Screen : Outline::None;
	mat.outline_width = std::max(0.0f, num(f, "_OutlineWidth", 0)) * 0.01f;
	if (mat.outline == Outline::Screen)
		mat.outline_width *= 0.5f;
	if (mat.outline_width <= 0)
		mat.outline = Outline::None;
	colour("_OutlineColor", mat.outline_colour, 3, true);
	mat.outline_mix = (int)num(f, "_OutlineColorMode", 0) == 1 ? num(f, "_OutlineLightingMix", 1) : 0;
	const int blend = (int)num(f, "_BlendMode", -1);
	if (blend == 0)
		mat.alpha = AlphaMode::Opaque;
	else if (blend == 1)
		mat.alpha = AlphaMode::Mask;
	else if (blend >= 2)
		mat.alpha = AlphaMode::Blend;
	mat.cutoff = num(f, "_Cutoff", mat.cutoff);
	if (texture("_BumpMap").used()) {
		mat.normal_map = texture("_BumpMap");
		mat.normal_scale = std::clamp(num(f, "_BumpScale", 1), -4.0f, 4.0f);
	}
	// Unity's culling, as MToon has it: 0 none (both sides show), 1 its
	// front (only the back shows), 2 its back.
	const float cull = num(f, "_CullMode", -1);
	if (cull == 0 || cull == 1 || cull == 2) {
		mat.double_sided = cull == 0;
		mat.inside_only = cull == 1;
	}
	mat.queue = index_of(p, "renderQueue", 2000) - 2000;
}

Material read_material(const json &doc, const json &m)
{
	Material mat;
	mat.name = text(m, "name");
	const json &pbr = field(m, "pbrMetallicRoughness");
	numbers(pbr, "baseColorFactor", mat.base, 4);
	mat.base_map = tex_ref(doc, field(pbr, "baseColorTexture"));
	mat.metallic = std::clamp(num(pbr, "metallicFactor", 1), 0.0f, 1.0f);
	mat.roughness = std::clamp(num(pbr, "roughnessFactor", 1), 0.0f, 1.0f);
	mat.metal_map = tex_ref(doc, field(pbr, "metallicRoughnessTexture"));
	numbers(m, "emissiveFactor", mat.emissive, 3);
	mat.emissive_map = tex_ref(doc, field(m, "emissiveTexture"));
	mat.normal_map = tex_ref(doc, field(m, "normalTexture"));
	mat.normal_scale = std::clamp(num(field(m, "normalTexture"), "scale", 1), -4.0f, 4.0f);
	mat.occlusion_map = tex_ref(doc, field(m, "occlusionTexture"));
	mat.occlusion_strength = std::clamp(num(field(m, "occlusionTexture"), "strength", 1), 0.0f, 1.0f);
	const std::string alpha = text(m, "alphaMode");
	mat.alpha = alpha == "MASK" ? AlphaMode::Mask : alpha == "BLEND" ? AlphaMode::Blend : AlphaMode::Opaque;
	mat.cutoff = num(m, "alphaCutoff", 0.5f);
	mat.double_sided = flag(m, "doubleSided", false);
	const json &ext = field(m, "extensions");
	if (field(ext, "KHR_materials_unlit").is_object())
		mat.shading = Shading::Unlit;
	const json &strength = field(ext, "KHR_materials_emissive_strength");
	if (strength.is_object()) {
		const float k = std::max(0.0f, num(strength, "emissiveStrength", 1));
		for (float &e : mat.emissive)
			e *= k;
	}
	const json &mtoon = field(ext, "VRMC_materials_mtoon");
	if (mtoon.is_object())
		read_mtoon1(doc, mtoon, mat);
	return mat;
}

std::string lower(std::string s)
{
	for (char &c : s)
		c = (char)std::tolower((unsigned char)c);
	return s;
}

// VRM 0.x's expression names, as VRM 1.0 has them.
std::string preset_of_vrm0(const std::string &preset)
{
	static const std::map<std::string, std::string> names = {
		{"joy", "happy"},
		{"angry", "angry"},
		{"sorrow", "sad"},
		{"fun", "relaxed"},
		{"a", "aa"},
		{"i", "ih"},
		{"u", "ou"},
		{"e", "ee"},
		{"o", "oh"},
		{"blink", "blink"},
		{"blink_l", "blinkLeft"},
		{"blink_r", "blinkRight"},
		{"lookup", "lookUp"},
		{"lookdown", "lookDown"},
		{"lookleft", "lookLeft"},
		{"lookright", "lookRight"},
		{"neutral", "neutral"},
		{"surprised", "surprised"},
	};
	auto it = names.find(lower(preset));
	return it == names.end() ? std::string() : it->second;
}

int override_of(const json &e, const char *key)
{
	const std::string v = text(e, key);
	return v == "block" ? 1 : v == "blend" ? 2 : 0;
}

LookRange range_of(const json &r, bool vrm0)
{
	LookRange out;
	if (!r.is_object())
		return out;
	if (vrm0) {
		out.in = num(r, "xRange", 90);
		out.out = num(r, "yRange", 10);
	} else {
		out.in = num(r, "inputMaxValue", 90);
		out.out = num(r, "outputScale", 10);
	}
	out.in = std::max(1.0f, out.in);
	return out;
}

void read_vrm1(const json &doc, const json &vrm, Model &m)
{
	m.vrm = 1;
	const json &meta = field(vrm, "meta");
	m.meta.title = text(meta, "name");
	for (const json &a : list(meta, "authors"))
		if (a.is_string())
			m.meta.author += (m.meta.author.empty() ? "" : ", ") + a.get<std::string>();
	m.meta.licence = text(meta, "licenseUrl");
	m.meta.allowed = text(meta, "avatarPermission");
	m.meta.commercial = text(meta, "commercialUsage");
	const json &bones = field(field(vrm, "humanoid"), "humanBones");
	if (bones.is_object())
		for (auto it = bones.begin(); it != bones.end(); ++it) {
			const int node = index_of(it.value(), "node");
			if (node >= 0 && node < (int)m.nodes.size())
				m.bones[it.key()] = node;
		}
	const json &look = field(vrm, "lookAt");
	m.look_bones = text(look, "type") != "expression";
	m.look_in = range_of(field(look, "rangeMapHorizontalInner"), false);
	m.look_out = range_of(field(look, "rangeMapHorizontalOuter"), false);
	m.look_down = range_of(field(look, "rangeMapVerticalDown"), false);
	m.look_up = range_of(field(look, "rangeMapVerticalUp"), false);
	const json &expressions = field(vrm, "expressions");
	for (const char *group : {"preset", "custom"}) {
		const json &g = field(expressions, group);
		if (!g.is_object())
			continue;
		for (auto it = g.begin(); it != g.end(); ++it) {
			Expression e;
			e.name = it.key();
			e.preset = std::strcmp(group, "preset") == 0;
			const json &x = it.value();
			for (const json &b : list(x, "morphTargetBinds")) {
				const int node = index_of(b, "node");
				if (node < 0 || node >= (int)m.nodes.size() || m.nodes[node].mesh < 0)
					continue;
				Expression::Bind bind;
				bind.mesh = m.nodes[node].mesh;
				bind.morph = index_of(b, "index");
				bind.weight = std::clamp(num(b, "weight", 1), 0.0f, 1.0f);
				if (bind.morph >= 0 && bind.morph < (int)m.meshes[bind.mesh].morph_names.size())
					e.binds.push_back(bind);
			}
			e.binary = flag(x, "isBinary", false);
			e.override_blink = override_of(x, "overrideBlink");
			e.override_look = override_of(x, "overrideLookAt");
			e.override_mouth = override_of(x, "overrideMouth");
			m.expressions.push_back(e);
		}
	}
	const json &spring = field(field(doc, "extensions"), "VRMC_springBone");
	if (!spring.is_object())
		return;
	for (const json &c : list(spring, "colliders")) {
		Collider col;
		col.node = index_of(c, "node");
		const json &shape = field(c, "shape");
		const json &sphere = field(shape, "sphere"), &capsule = field(shape, "capsule");
		const json &s = capsule.is_object() ? capsule : sphere;
		col.capsule = capsule.is_object();
		float o[3] = {0, 0, 0}, t[3] = {0, 0, 0};
		numbers(s, "offset", o, 3);
		numbers(s, "tail", t, 3);
		col.offset = {o[0], o[1], o[2]};
		col.tail = {t[0], t[1], t[2]};
		col.radius = std::max(0.0f, num(s, "radius", 0));
		if (col.node < 0 || col.node >= (int)m.nodes.size())
			col.node = -1;
		m.colliders.push_back(col);
	}
	const json &groups = list(spring, "colliderGroups");
	for (const json &s : list(spring, "springs")) {
		Spring sp;
		sp.name = text(s, "name");
		for (const json &gi : list(s, "colliderGroups")) {
			if (!gi.is_number_integer() || gi.get<int>() < 0 || gi.get<int>() >= (int)groups.size())
				continue;
			for (const json &ci : list(groups[gi.get<int>()], "colliders"))
				if (ci.is_number_integer() && ci.get<int>() >= 0 &&
				    ci.get<int>() < (int)m.colliders.size())
					sp.colliders.push_back(ci.get<int>());
		}
		const json &joints = list(s, "joints");
		// Each joint swings towards the next; the last is only a tail.
		for (size_t i = 0; i + 1 < joints.size(); i++) {
			SpringJoint j;
			j.node = index_of(joints[i], "node");
			j.tail_node = index_of(joints[i + 1], "node");
			if (j.node < 0 || j.node >= (int)m.nodes.size() || j.tail_node < 0 ||
			    j.tail_node >= (int)m.nodes.size())
				continue;
			j.radius = std::max(0.0f, num(joints[i], "hitRadius", 0));
			j.stiffness = std::max(0.0f, num(joints[i], "stiffness", 1));
			j.gravity = std::max(0.0f, num(joints[i], "gravityPower", 0));
			float g[3] = {0, -1, 0};
			numbers(joints[i], "gravityDir", g, 3);
			j.gravity_dir = normalized(V3{g[0], g[1], g[2]}, {0, -1, 0});
			j.drag = std::clamp(num(joints[i], "dragForce", 0.5f), 0.0f, 1.0f);
			sp.joints.push_back(j);
		}
		if (!sp.joints.empty())
			m.springs.push_back(sp);
	}
}

V3 vec3_of(const json &o, bool flip_z)
{
	return {num(o, "x", 0), num(o, "y", 0), num(o, "z", 0) * (flip_z ? -1.0f : 1.0f)};
}

void read_vrm0(const json &doc, const json &vrm, Model &m)
{
	m.vrm = -1;
	m.faces_back = true;
	const json &meta = field(vrm, "meta");
	m.meta.title = text(meta, "title");
	m.meta.author = text(meta, "author");
	m.meta.licence = text(meta, "licenseName");
	if (m.meta.licence == "Other")
		m.meta.licence = text(meta, "otherLicenseUrl");
	m.meta.allowed = text(meta, "allowedUserName");
	m.meta.commercial = text(meta, "commercialUssageName");
	for (const json &b : list(field(vrm, "humanoid"), "humanBones")) {
		const std::string name = text(b, "bone");
		const int node = index_of(b, "node");
		if (!name.empty() && node >= 0 && node < (int)m.nodes.size())
			m.bones[name] = node;
	}
	const json &fp = field(vrm, "firstPerson");
	m.look_bones = text(fp, "lookAtTypeName") != "BlendShape";
	m.look_in = range_of(field(fp, "lookAtHorizontalInner"), true);
	m.look_out = range_of(field(fp, "lookAtHorizontalOuter"), true);
	m.look_down = range_of(field(fp, "lookAtVerticalDown"), true);
	m.look_up = range_of(field(fp, "lookAtVerticalUp"), true);
	for (const json &g : list(field(vrm, "blendShapeMaster"), "blendShapeGroups")) {
		Expression e;
		const std::string preset = preset_of_vrm0(text(g, "presetName"));
		e.preset = !preset.empty() && preset != "unknown";
		e.name = e.preset ? preset : text(g, "name");
		if (e.name.empty())
			continue;
		for (const json &b : list(g, "binds")) {
			Expression::Bind bind;
			bind.mesh = index_of(b, "mesh");
			bind.morph = index_of(b, "index");
			bind.weight = std::clamp(num(b, "weight", 100) / 100.0f, 0.0f, 1.0f);
			if (bind.mesh >= 0 && bind.mesh < (int)m.meshes.size() && bind.morph >= 0 &&
			    bind.morph < (int)m.meshes[bind.mesh].morph_names.size())
				e.binds.push_back(bind);
		}
		e.binary = flag(g, "isBinary", false);
		// VRM 0.x has no overrides; its emotions stop blinks as 1.0's do.
		m.expressions.push_back(e);
	}
	const json &second = field(vrm, "secondaryAnimation");
	std::vector<std::vector<int>> group_colliders;
	for (const json &g : list(second, "colliderGroups")) {
		std::vector<int> ids;
		const int node = index_of(g, "node");
		for (const json &c : list(g, "colliders")) {
			Collider col;
			col.node = node >= 0 && node < (int)m.nodes.size() ? node : -1;
			col.offset = vec3_of(field(c, "offset"), true);
			col.radius = std::max(0.0f, num(c, "radius", 0));
			ids.push_back((int)m.colliders.size());
			m.colliders.push_back(col);
		}
		group_colliders.push_back(ids);
	}
	for (const json &g : list(second, "boneGroups")) {
		Spring sp;
		sp.name = text(g, "comment");
		for (const json &gi : list(g, "colliderGroups"))
			if (gi.is_number_integer() && gi.get<int>() >= 0 && gi.get<int>() < (int)group_colliders.size())
				for (int c : group_colliders[gi.get<int>()])
					sp.colliders.push_back(c);
		SpringJoint base;
		base.stiffness = std::max(0.0f, num(g, "stiffiness", 1));
		base.gravity = std::max(0.0f, num(g, "gravityPower", 0));
		base.gravity_dir = normalized(vec3_of(field(g, "gravityDir"), true), {0, -1, 0});
		if (!field(g, "gravityDir").is_object())
			base.gravity_dir = {0, -1, 0};
		base.drag = std::clamp(num(g, "dragForce", 0.4f), 0.0f, 1.0f);
		base.radius = std::max(0.0f, num(g, "hitRadius", 0.02f));
		// Each root bone and everything under it swings, each bone towards
		// its first child (or a little past its end, for the last).
		std::vector<bool> seen(m.nodes.size(), false);
		std::function<void(int)> add = [&](int n) {
			if (n < 0 || n >= (int)m.nodes.size() || seen[n])
				return;
			seen[n] = true;
			SpringJoint j = base;
			j.node = n;
			const Node &node = m.nodes[n];
			if (!node.children.empty()) {
				j.tail_node = node.children[0];
			} else {
				// 7 cm on, the way it points from its parent.
				const V3 along = node.parent >= 0 ? node.t : V3{0, 1, 0};
				j.tail = normalized(rotate(conj(node.r), along), {0, 1, 0}) * 0.07f;
				if (node.s.x > 1e-6f)
					j.tail = j.tail * (1 / node.s.x);
			}
			sp.joints.push_back(j);
			for (int c : node.children)
				add(c);
		};
		for (const json &b : list(g, "bones"))
			if (b.is_number_integer())
				add(b.get<int>());
		if (!sp.joints.empty())
			m.springs.push_back(sp);
	}
	// VRM 0.x's materials, matched by order.
	const json &props = list(vrm, "materialProperties");
	for (size_t i = 0; i < props.size() && i < m.materials.size(); i++) {
		const std::string shader = text(props[i], "shader");
		if (shader == "VRM/MToon")
			read_mtoon0(doc, props[i], m.materials[i]);
		else if (shader.find("Unlit") != std::string::npos)
			m.materials[i].shading = Shading::Unlit;
	}
}

void read_nodes(const json &doc, Model &m)
{
	const json &nodes = list(doc, "nodes");
	if (nodes.size() > MOST_NODES)
		fail("the file has too many parts");
	m.nodes.resize(nodes.size());
	for (size_t i = 0; i < nodes.size(); i++) {
		const json &n = nodes[i];
		Node &node = m.nodes[i];
		node.name = text(n, "name");
		node.mesh = index_of(n, "mesh");
		node.skin = index_of(n, "skin");
		if (node.mesh >= (int)list(doc, "meshes").size())
			node.mesh = -1;
		if (node.skin >= (int)list(doc, "skins").size())
			node.skin = -1;
		const json &matrix = field(n, "matrix");
		if (matrix.is_array() && matrix.size() == 16) {
			M4 a;
			numbers(n, "matrix", a.m, 16);
			node.t = move_of(a);
			V3 c0{a.m[0], a.m[1], a.m[2]}, c1{a.m[4], a.m[5], a.m[6]}, c2{a.m[8], a.m[9], a.m[10]};
			node.s = {length(c0), length(c1), length(c2)};
			if (dot(cross(c0, c1), c2) < 0)
				node.s.x = -node.s.x;
			node.r = quat_of(a);
			if (node.s.x < 0) {
				// A mirrored matrix: the turn of its un-mirrored columns.
				M4 b = a;
				b.m[0] = -b.m[0];
				b.m[1] = -b.m[1];
				b.m[2] = -b.m[2];
				node.r = quat_of(b);
			}
		} else {
			float t[3] = {0, 0, 0}, r[4] = {0, 0, 0, 1}, s[3] = {1, 1, 1};
			numbers(n, "translation", t, 3);
			numbers(n, "rotation", r, 4);
			numbers(n, "scale", s, 3);
			node.t = {t[0], t[1], t[2]};
			node.r = normalized(Quat{r[0], r[1], r[2], r[3]});
			node.s = {s[0], s[1], s[2]};
		}
	}
	for (size_t i = 0; i < nodes.size(); i++)
		for (const json &c : list(nodes[i], "children")) {
			if (!c.is_number_integer())
				continue;
			const int ci = c.get<int>();
			if (ci < 0 || ci >= (int)m.nodes.size() || m.nodes[ci].parent >= 0 || ci == (int)i)
				fail("the file is damaged (its parts are joined in a loop)");
			m.nodes[ci].parent = (int)i;
			m.nodes[i].children.push_back(ci);
		}
	// No loops: every node reaches the top within as many steps as there
	// are nodes.
	for (size_t i = 0; i < m.nodes.size(); i++) {
		int n = (int)i;
		size_t steps = 0;
		while (n >= 0 && steps++ <= m.nodes.size())
			n = m.nodes[n].parent;
		if (n >= 0)
			fail("the file is damaged (its parts are joined in a loop)");
	}
	const json &scenes = list(doc, "scenes");
	const int scene = index_of(doc, "scene", 0);
	if (scene >= 0 && scene < (int)scenes.size()) {
		for (const json &r : list(scenes[scene], "nodes"))
			if (r.is_number_integer() && r.get<int>() >= 0 && r.get<int>() < (int)m.nodes.size() &&
			    m.nodes[r.get<int>()].parent < 0)
				m.roots.push_back(r.get<int>());
	} else {
		for (size_t i = 0; i < m.nodes.size(); i++)
			if (m.nodes[i].parent < 0)
				m.roots.push_back((int)i);
	}
}

void read_meshes(Reader &rd, const json &doc, Model &m)
{
	size_t points = 0, indices = 0;
	for (const json &mj : list(doc, "meshes")) {
		Mesh mesh;
		mesh.name = text(mj, "name");
		size_t morphs = 0;
		for (const json &pj : list(mj, "primitives"))
			morphs = std::max(morphs, list(pj, "targets").size());
		if (morphs > MOST_MORPHS)
			fail("the file has too many expressions");
		mesh.morph_names.assign(morphs, std::string());
		mesh.weights.assign(morphs, 0.0f);
		const json &names = field(field(mj, "extras"), "targetNames");
		for (const json &pj : list(mj, "primitives")) {
			const int mode = index_of(pj, "mode", 4);
			if (mode < 4 || mode > 6)
				continue; // points and lines
			const json &attr = field(pj, "attributes");
			const int pos_a = index_of(attr, "POSITION");
			if (pos_a < 0)
				continue;
			Primitive p;
			p.pos = rd.vec3s(pos_a);
			const size_t n = p.pos.size();
			points += n;
			if (points > MOST_POINTS)
				fail("the file is too big (over 4 million points)");
			if (index_of(attr, "NORMAL") >= 0) {
				p.normal = rd.vec3s(index_of(attr, "NORMAL"));
				if (p.normal.size() != n)
					fail("the file is damaged (its parts don't add up)");
				for (V3 &v : p.normal)
					v = normalized(v, {0, 0, 1});
			}
			if (index_of(attr, "TEXCOORD_0") >= 0) {
				p.uv = rd.floats(index_of(attr, "TEXCOORD_0"), 2);
				if (p.uv.size() != n * 2)
					fail("the file is damaged (its parts don't add up)");
			}
			if (index_of(attr, "TANGENT") >= 0) {
				// Kept only if its material has a normal map (finish_tangents).
				p.tangent = rd.floats(index_of(attr, "TANGENT"), 4);
				if (p.tangent.size() != n * 4)
					p.tangent.clear();
			}
			if (index_of(attr, "COLOR_0") >= 0) {
				size_t cn = 0;
				const std::vector<double> c = rd.read(index_of(attr, "COLOR_0"), 0, cn);
				const size_t comps = cn ? c.size() / cn : 0;
				if (cn == n && (comps == 3 || comps == 4)) {
					p.colour.resize(n * 4);
					for (size_t i = 0; i < n; i++)
						for (size_t k = 0; k < 4; k++)
							p.colour[i * 4 + k] = k < comps ? (float)c[i * comps + k]
											: 1.0f;
				}
			}
			if (index_of(attr, "JOINTS_0") >= 0 && index_of(attr, "WEIGHTS_0") >= 0) {
				size_t jn = 0;
				const std::vector<double> j = rd.read(index_of(attr, "JOINTS_0"), 4, jn);
				const std::vector<float> w = rd.floats(index_of(attr, "WEIGHTS_0"), 4);
				if (jn != n || w.size() != n * 4)
					fail("the file is damaged (its parts don't add up)");
				p.joints.resize(n * 4);
				p.weights.resize(n * 4);
				for (size_t i = 0; i < n; i++) {
					float sum = 0;
					for (int k = 0; k < 4; k++) {
						const double ji = j[i * 4 + k];
						p.joints[i * 4 + k] = ji >= 0 && ji < 65536 ? (uint16_t)ji : 0;
						const float wk = std::max(0.0f, w[i * 4 + k]);
						p.weights[i * 4 + k] = wk;
						sum += wk;
					}
					for (int k = 0; k < 4; k++)
						p.weights[i * 4 + k] = sum > 1e-6f ? p.weights[i * 4 + k] / sum
										   : (k == 0 ? 1.0f : 0.0f);
				}
			}
			std::vector<uint32_t> ix;
			if (index_of(pj, "indices") >= 0) {
				size_t in = 0;
				const std::vector<double> d = rd.read(index_of(pj, "indices"), 1, in);
				ix.resize(in);
				for (size_t i = 0; i < in; i++) {
					if (d[i] < 0 || d[i] >= (double)n)
						fail("the file is damaged (a triangle past its points)");
					ix[i] = (uint32_t)d[i];
				}
			} else {
				ix.resize(n);
				for (size_t i = 0; i < n; i++)
					ix[i] = (uint32_t)i;
			}
			p.index = triangles_of(ix, mode);
			p.index.resize(p.index.size() / 3 * 3);
			indices += p.index.size();
			if (indices > MOST_INDEX)
				fail("the file is too big (over 8 million triangles)");
			if (p.normal.empty())
				make_normals(p);
			const json &targets = list(pj, "targets");
			p.morphs.resize(morphs);
			for (size_t t = 0; t < targets.size() && t < morphs; t++) {
				MorphTarget &mt = p.morphs[t];
				if (index_of(targets[t], "POSITION") >= 0)
					mt.pos = rd.vec3s(index_of(targets[t], "POSITION"));
				if (index_of(targets[t], "NORMAL") >= 0)
					mt.normal = rd.vec3s(index_of(targets[t], "NORMAL"));
				if ((!mt.pos.empty() && mt.pos.size() != n) ||
				    (!mt.normal.empty() && mt.normal.size() != n))
					fail("the file is damaged (an expression doesn't fit its part)");
				points += (mt.pos.size() + mt.normal.size()) / 4;
			}
			if (points > MOST_POINTS * 2)
				fail("the file is too big");
			const json &pnames = field(field(pj, "extras"), "targetNames");
			const json &use = names.is_array() ? names : pnames;
			if (use.is_array())
				for (size_t t = 0; t < use.size() && t < morphs; t++)
					if (use[t].is_string() && mesh.morph_names[t].empty())
						mesh.morph_names[t] = use[t].get<std::string>();
			p.material = index_of(pj, "material");
			if (p.material >= (int)m.materials.size())
				p.material = -1;
			mesh.parts.push_back(std::move(p));
		}
		float w[MOST_MORPHS];
		std::fill(w, w + morphs, 0.0f);
		numbers(mj, "weights", w, (int)morphs);
		mesh.weights.assign(w, w + morphs);
		m.meshes.push_back(std::move(mesh));
	}
}

void read_skins(Reader &rd, const json &doc, Model &m)
{
	for (const json &sj : list(doc, "skins")) {
		Skin skin;
		for (const json &j : list(sj, "joints"))
			if (j.is_number_integer() && j.get<int>() >= 0 && j.get<int>() < (int)m.nodes.size())
				skin.joints.push_back(j.get<int>());
			else
				fail("the file is damaged (a missing bone)");
		skin.inverse_bind.assign(skin.joints.size(), M4());
		if (index_of(sj, "inverseBindMatrices") >= 0) {
			const std::vector<float> f = rd.floats(index_of(sj, "inverseBindMatrices"), 16);
			if (f.size() < skin.joints.size() * 16)
				fail("the file is damaged (its bones don't add up)");
			for (size_t i = 0; i < skin.joints.size(); i++)
				std::copy(f.begin() + (long)i * 16, f.begin() + (long)i * 16 + 16,
					  skin.inverse_bind[i].m);
		}
		m.skins.push_back(std::move(skin));
	}
}

// Its own animations. A channel DireBot can't play (an expression's
// strengths, a missing part, numbers that don't add up) is left out; the
// model loads all the same.
// An animation's channel (`cj`, of animation `aj`) on one of `nodes`
// nodes, into `c`: false if it's broken, isn't a move, turn or size
// ("weights", its expressions' strengths, aren't played), or there's too
// much of it (`numbers`: all the keys' numbers so far).
bool read_channel(Reader &rd, const json &aj, const json &cj, size_t nodes, AnimChannel &c, size_t &numbers)
{
	const json &samplers = list(aj, "samplers");
	const json &target = field(cj, "target");
	const std::string path = text(target, "path");
	const int node = index_of(target, "node");
	const int si = index_of(cj, "sampler");
	if (node < 0 || node >= (int)nodes || si < 0 || si >= (int)samplers.size())
		return false;
	c = AnimChannel();
	c.node = node;
	if (path == "translation")
		c.path = AnimChannel::Move;
	else if (path == "rotation")
		c.path = AnimChannel::Turn;
	else if (path == "scale")
		c.path = AnimChannel::Size;
	else
		return false;
	const json &sj = samplers[si];
	const std::string ease = text(sj, "interpolation");
	c.ease = ease == "STEP" ? AnimChannel::Step : ease == "CUBICSPLINE" ? AnimChannel::Curved : AnimChannel::Straight;
	try {
		c.times = rd.floats(index_of(sj, "input"), 1);
		c.values = rd.floats(index_of(sj, "output"), c.size());
	} catch (const Fail &) {
		return false;
	}
	const size_t keys = c.times.size();
	const size_t per = (size_t)c.size() * (c.ease == AnimChannel::Curved ? 3 : 1);
	if (!keys || c.values.size() != keys * per)
		return false;
	for (size_t k = 0; k < keys; k++)
		if (!std::isfinite(c.times[k]) || c.times[k] < 0 || (k && c.times[k] < c.times[k - 1]))
			return false;
	for (float v : c.values)
		if (!std::isfinite(v))
			return false;
	if (numbers + c.values.size() + keys > MOST_KEY_NUMBERS)
		return false; // too much of it: the rest left out
	numbers += c.values.size() + keys;
	return true;
}

void read_animations(Reader &rd, const json &doc, Model &m)
{
	const json &animations = list(doc, "animations");
	size_t numbers = 0;
	for (size_t ai = 0; ai < animations.size() && ai < MOST_ANIMATIONS; ai++) {
		const json &aj = animations[ai];
		Animation anim;
		anim.name = text(aj, "name");
		if (anim.name.empty())
			anim.name = "Animation " + std::to_string(ai + 1);
		for (const json &cj : list(aj, "channels")) {
			AnimChannel c;
			if (!read_channel(rd, aj, cj, m.nodes.size(), c, numbers))
				continue;
			anim.length = std::max(anim.length, c.times.back());
			anim.channels.push_back(std::move(c));
		}
		m.animations.push_back(std::move(anim));
	}
}

void read_images(Reader &rd, const json &doc, Model &m)
{
	const json &images = list(doc, "images");
	if (images.size() > MOST_IMAGES)
		fail("the file has too many pictures");
	for (const json &ij : images) {
		ModelImage img;
		img.name = text(ij, "name");
		img.mime = text(ij, "mimeType");
		try {
			if (index_of(ij, "bufferView") >= 0) {
				const Reader::View v = rd.view(index_of(ij, "bufferView"));
				img.bytes.assign(v.data, v.data + v.size);
			} else if (!text(ij, "uri").empty()) {
				img.bytes = rd.load_uri(text(ij, "uri"));
			}
		} catch (const Fail &) {
			// A missing picture: drawn in its plain colour.
			img.bytes.clear();
		}
		m.images.push_back(std::move(img));
	}
}

void measure(Model &m)
{
	const std::vector<M4> world = rest_world(m);
	V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
	m.points = m.triangles = 0;
	for (size_t ni = 0; ni < m.nodes.size(); ni++) {
		const Node &node = m.nodes[ni];
		if (node.mesh < 0)
			continue;
		const Skin *skin = node.skin >= 0 ? &m.skins[node.skin] : nullptr;
		std::vector<M4> joint;
		if (skin)
			for (size_t j = 0; j < skin->joints.size(); j++)
				joint.push_back(world[skin->joints[j]] * skin->inverse_bind[j]);
		for (const Primitive &p : m.meshes[node.mesh].parts) {
			m.points += p.pos.size();
			m.triangles += p.index.size() / 3;
			for (size_t i = 0; i < p.pos.size(); i++) {
				V3 v;
				if (skin && !p.joints.empty()) {
					for (int k = 0; k < 4; k++) {
						const float w = p.weights[i * 4 + k];
						const uint16_t j = p.joints[i * 4 + k];
						if (w > 0 && j < joint.size())
							v = v + point(joint[j], p.pos[i]) * w;
					}
				} else {
					v = point(world[ni], p.pos[i]);
				}
				lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
				hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
			}
		}
	}
	if (lo.x > hi.x) {
		lo = {-0.5f, -0.5f, -0.5f};
		hi = {0.5f, 0.5f, 0.5f};
	}
	m.low = lo;
	m.high = hi;
}

void load_buffers(Reader &rd, const json &doc)
{
	const json &buffers = list(doc, "buffers");
	for (size_t i = 0; i < buffers.size(); i++) {
		const std::string uri = text(buffers[i], "uri");
		if (uri.empty()) {
			// A .glb's own: the one after its description.
			if (i != 0 || !rd.glb)
				fail("the file is damaged (a missing buffer)");
			rd.buffers.push_back(std::move(rd.bin));
			continue;
		}
		rd.buffers.push_back(rd.load_uri(uri));
	}
}

void load_json(Reader &rd, const json &doc, Model &m)
{
	const json &asset = field(doc, "asset");
	const std::string version = text(asset, "version");
	if (!version.empty() && version[0] != '2')
		fail("it's an old glTF " + version + " file; DireBot reads glTF 2.0 (save it again from Blender)");
	for (const json &e : list(doc, "extensionsRequired")) {
		const std::string name = e.is_string() ? e.get<std::string>() : "";
		if (name == "KHR_draco_mesh_compression" || name == "EXT_meshopt_compression" ||
		    name == "KHR_mesh_quantization")
			fail("its shape is squeezed (" + name +
			     "), which DireBot can't read yet: save it again with compression off");
	}
	load_buffers(rd, doc);
	for (const json &mj : list(doc, "materials"))
		m.materials.push_back(read_material(doc, mj));
	read_nodes(doc, m);
	read_meshes(rd, doc, m);
	read_skins(rd, doc, m);
	read_animations(rd, doc, m);
	read_images(rd, doc, m);
	const json &ext = field(doc, "extensions");
	if (field(ext, "VRMC_vrm").is_object())
		read_vrm1(doc, field(ext, "VRMC_vrm"), m);
	else if (field(ext, "VRM").is_object())
		read_vrm0(doc, field(ext, "VRM"), m);
	// Tangents for the parts with a normal map (a VRM 0.x's are known only
	// now), worked out where the file has none; none for the rest.
	for (Mesh &mesh : m.meshes)
		for (Primitive &p : mesh.parts) {
			const bool bumpy = p.material >= 0 && p.material < (int)m.materials.size() &&
					   m.materials[p.material].normal_map.used() && !p.uv.empty();
			if (!bumpy) {
				std::vector<float>().swap(p.tangent);
				continue;
			}
			bool usable = !p.tangent.empty();
			for (size_t i = 0; usable && i < p.pos.size(); i++) {
				float *t = &p.tangent[i * 4];
				const V3 v{t[0], t[1], t[2]};
				usable = std::isfinite(v.x + v.y + v.z + t[3]) && length(v) > 1e-6f;
				const V3 u = normalized(v, {1, 0, 0});
				t[0] = u.x, t[1] = u.y, t[2] = u.z, t[3] = t[3] < 0 ? -1.0f : 1.0f;
			}
			if (!usable)
				p.tangent = tangents_of(p);
			p.own_tangents = usable;
		}
	if (m.points == 0 && m.meshes.empty())
		fail("there's nothing to see in it");
	measure(m);
	if (m.points == 0)
		fail("there's nothing to see in it");
}

uint32_t u32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// A .glb's description and data, or a .gltf's description.
void open_file(const uint8_t *bytes, size_t size, json &doc, std::vector<uint8_t> &bin, bool &glb)
{
	glb = false;
	if (size >= 12 && std::memcmp(bytes, "glTF", 4) == 0) {
		glb = true;
		const uint32_t version = u32(bytes + 4), total = u32(bytes + 8);
		if (version != 2)
			fail("it's an old glTF file; DireBot reads glTF 2.0 (save it again from Blender)");
		const size_t end = std::min<size_t>(size, total);
		size_t at = 12;
		bool have_json = false;
		while (at + 8 <= end) {
			const uint32_t len = u32(bytes + at), type = u32(bytes + at + 4);
			at += 8;
			if (len > end - at)
				fail("the file is cut short");
			if (type == 0x4E4F534A && !have_json) {
				doc = json::parse(bytes + at, bytes + at + len, nullptr, false);
				have_json = true;
			} else if (type == 0x004E4942 && bin.empty()) {
				bin.assign(bytes + at, bytes + at + len);
			}
			at += (len + 3) & ~3u;
		}
		if (!have_json)
			fail("the file is damaged (no description in it)");
	} else {
		doc = json::parse(bytes, bytes + size, nullptr, false);
	}
}

// The VRM humanoid's bones, each with the one it hangs from (VRM 1.0).
const char *const HUMANOID[][2] = {
	{"hips", ""},
	{"spine", "hips"},
	{"chest", "spine"},
	{"upperChest", "chest"},
	{"neck", "upperChest"},
	{"head", "neck"},
	{"leftEye", "head"},
	{"rightEye", "head"},
	{"jaw", "head"},
	{"leftUpperLeg", "hips"},
	{"leftLowerLeg", "leftUpperLeg"},
	{"leftFoot", "leftLowerLeg"},
	{"leftToes", "leftFoot"},
	{"rightUpperLeg", "hips"},
	{"rightLowerLeg", "rightUpperLeg"},
	{"rightFoot", "rightLowerLeg"},
	{"rightToes", "rightFoot"},
	{"leftShoulder", "upperChest"},
	{"leftUpperArm", "leftShoulder"},
	{"leftLowerArm", "leftUpperArm"},
	{"leftHand", "leftLowerArm"},
	{"rightShoulder", "upperChest"},
	{"rightUpperArm", "rightShoulder"},
	{"rightLowerArm", "rightUpperArm"},
	{"rightHand", "rightLowerArm"},
	{"leftThumbMetacarpal", "leftHand"},
	{"leftThumbProximal", "leftThumbMetacarpal"},
	{"leftThumbDistal", "leftThumbProximal"},
	{"leftIndexProximal", "leftHand"},
	{"leftIndexIntermediate", "leftIndexProximal"},
	{"leftIndexDistal", "leftIndexIntermediate"},
	{"leftMiddleProximal", "leftHand"},
	{"leftMiddleIntermediate", "leftMiddleProximal"},
	{"leftMiddleDistal", "leftMiddleIntermediate"},
	{"leftRingProximal", "leftHand"},
	{"leftRingIntermediate", "leftRingProximal"},
	{"leftRingDistal", "leftRingIntermediate"},
	{"leftLittleProximal", "leftHand"},
	{"leftLittleIntermediate", "leftLittleProximal"},
	{"leftLittleDistal", "leftLittleIntermediate"},
	{"rightThumbMetacarpal", "rightHand"},
	{"rightThumbProximal", "rightThumbMetacarpal"},
	{"rightThumbDistal", "rightThumbProximal"},
	{"rightIndexProximal", "rightHand"},
	{"rightIndexIntermediate", "rightIndexProximal"},
	{"rightIndexDistal", "rightIndexIntermediate"},
	{"rightMiddleProximal", "rightHand"},
	{"rightMiddleIntermediate", "rightMiddleProximal"},
	{"rightMiddleDistal", "rightMiddleIntermediate"},
	{"rightRingProximal", "rightHand"},
	{"rightRingIntermediate", "rightRingProximal"},
	{"rightRingDistal", "rightRingIntermediate"},
	{"rightLittleProximal", "rightHand"},
	{"rightLittleIntermediate", "rightLittleProximal"},
	{"rightLittleDistal", "rightLittleIntermediate"},
};

// Each key of a channel changed by `f` (its value, and a curved one's
// slopes by `slope`).
template <typename F, typename G> void each_key(AnimChannel &c, F f, G slope)
{
	const int n = c.size();
	const bool curved = c.ease == AnimChannel::Curved;
	const size_t per = curved ? 3 : 1;
	for (size_t k = 0; k < c.times.size(); k++)
		for (size_t j = 0; j < per; j++) {
			float *v = &c.values[(k * per + j) * n];
			if (curved && j != 1)
				slope(v);
			else
				f(v);
		}
}

void load_motion_json(Reader &rd, const json &doc, Motion &out)
{
	const json &ext = field(field(doc, "extensions"), "VRMC_vrm_animation");
	if (!ext.is_object())
		fail("it isn't a VRM animation (.vrma) file");
	const std::string version = text(ext, "specVersion");
	if (!version.empty() && version[0] != '1')
		fail("it's a VRM animation of a version DireBot doesn't know (" + version + ")");
	load_buffers(rd, doc);
	Model m;
	read_nodes(doc, m);
	const std::vector<M4> world = rest_world(m);
	// Its bones: which node each is.
	std::map<std::string, int> bone;
	const json &bones = field(field(ext, "humanoid"), "humanBones");
	if (bones.is_object())
		for (auto it = bones.begin(); it != bones.end(); ++it) {
			const int node = index_of(it.value(), "node");
			if (node >= 0 && node < (int)m.nodes.size())
				bone[it.key()] = node;
		}
	if (!bone.count("hips"))
		fail("it has no hips bone (a VRM animation needs one)");
	std::map<int, std::string> bone_of_node, face_of_node;
	for (const auto &b : bone)
		bone_of_node[b.second] = b.first;
	const json &faces = field(ext, "expressions");
	for (const char *kind : {"preset", "custom"}) {
		const json &list_of = field(faces, kind);
		if (list_of.is_object())
			for (auto it = list_of.begin(); it != list_of.end(); ++it) {
				const int node = index_of(it.value(), "node");
				if (node >= 0 && node < (int)m.nodes.size() && !bone_of_node.count(node))
					face_of_node[node] = it.key();
			}
	}
	// Where each bone hangs from at rest: the nearest bone above it that
	// the file has (VRM's), else where its hips hang from.
	const int hips = bone["hips"];
	const M4 hips_parent = m.nodes[hips].parent >= 0 ? world[m.nodes[hips].parent] : M4();
	auto parent_world = [&](const std::string &name) {
		std::string p = name;
		for (int steps = 0; steps < 16; steps++) {
			const char *up = humanoid_parent(p);
			if (!up || !*up)
				break;
			p = up;
			auto it = bone.find(p);
			if (it != bone.end())
				return world[it->second];
		}
		return hips_parent;
	};
	out.hips_rest = move_of(world[hips]);
	const json &animations = list(doc, "animations");
	size_t numbers = 0;
	for (size_t ai = 0; ai < animations.size() && out.tracks.empty(); ai++) {
		const json &aj = animations[ai];
		out.name = text(aj, "name");
		for (const json &cj : list(aj, "channels")) {
			AnimChannel c;
			if (!read_channel(rd, aj, cj, m.nodes.size(), c, numbers))
				continue;
			const int node = c.node;
			c.node = -1;
			auto b = bone_of_node.find(node);
			if (b != bone_of_node.end()) {
				MotionTrack t;
				t.bone = b->second;
				if (c.path == AnimChannel::Turn) {
					// The turn from rest, in its world: its parent's turn at
					// rest before it, its own taken out after.
					const Quat P = normalized(quat_of(parent_world(b->second)));
					const Quat Wi = conj(normalized(quat_of(world[node])));
					auto turn = [&](float *v) {
						const Quat q = P * Quat{v[0], v[1], v[2], v[3]} * Wi;
						v[0] = q.x, v[1] = q.y, v[2] = q.z, v[3] = q.w;
					};
					each_key(c, turn, turn);
				} else if (c.path == AnimChannel::Move && b->second == "hips") {
					// Where its hips are, in its world.
					each_key(
						c,
						[&](float *v) {
							const V3 p = point(hips_parent, {v[0], v[1], v[2]});
							v[0] = p.x, v[1] = p.y, v[2] = p.z;
						},
						[&](float *v) {
							const V3 p = direction(hips_parent, {v[0], v[1], v[2]});
							v[0] = p.x, v[1] = p.y, v[2] = p.z;
						});
				} else {
					continue; // VRM moves only its hips' place, and sizes nothing
				}
				out.length = std::max(out.length, c.times.back());
				t.channel = std::move(c);
				out.tracks.push_back(std::move(t));
				continue;
			}
			auto f = face_of_node.find(node);
			if (f != face_of_node.end() && c.path == AnimChannel::Move) {
				// An expression's strength: its node's place across.
				out.length = std::max(out.length, c.times.back());
				out.expressions.push_back({f->second, std::move(c)});
			}
		}
		if (out.tracks.empty())
			out.expressions.clear();
	}
	if (out.tracks.empty())
		fail("it has no moves in it");
}

} // namespace

const char *humanoid_parent(const std::string &bone)
{
	for (const auto &b : HUMANOID)
		if (bone == b[0])
			return b[1];
	return nullptr;
}

bool load_motion(const uint8_t *bytes, size_t size, const std::string &folder, Motion &out, std::string *problem)
{
	out = Motion();
	try {
		json doc;
		std::vector<uint8_t> bin;
		bool glb = false;
		open_file(bytes, size, doc, bin, glb);
		if (doc.is_discarded() || !doc.is_object())
			fail("it isn't a VRM animation (.vrma) file");
		Reader rd(doc);
		rd.folder = folder;
		rd.glb = glb;
		rd.bin = std::move(bin);
		load_motion_json(rd, doc, out);
		return true;
	} catch (const Fail &f) {
		if (problem)
			*problem = f.why;
	} catch (const std::bad_alloc &) {
		if (problem)
			*problem = "it's too big to load";
	} catch (const std::exception &) {
		if (problem)
			*problem = "the file is damaged";
	}
	out = Motion();
	return false;
}

bool load_motion(const std::string &path, Motion &out, std::string *problem)
{
	std::vector<uint8_t> bytes;
	if (!read_file(path, bytes)) {
		if (problem)
			*problem = "DireBot can't open the file (or it's over 512 MB)";
		return false;
	}
	const size_t slash = path.find_last_of("/\\");
	const std::string folder = slash == std::string::npos ? "." : path.substr(0, slash);
	return load_motion(bytes.data(), bytes.size(), folder, out, problem);
}

std::vector<bool> skin_materials(const Model &m, const std::string &names)
{
	std::vector<bool> out(m.materials.size(), false);
	std::vector<std::string> wanted;
	std::string line;
	for (size_t i = 0; i <= names.size(); i++) {
		if (i == names.size() || names[i] == '\n' || names[i] == '\r') {
			const size_t a = line.find_first_not_of(" \t"), b = line.find_last_not_of(" \t");
			if (a != std::string::npos)
				wanted.push_back(lower(line.substr(a, b - a + 1)));
			line.clear();
		} else {
			line += names[i];
		}
	}
	if (!wanted.empty()) {
		for (size_t i = 0; i < out.size(); i++)
			out[i] = std::find(wanted.begin(), wanted.end(), lower(m.materials[i].name)) != wanted.end();
		return out;
	}
	auto has = [](const std::string &name, std::initializer_list<const char *> words) {
		for (const char *w : words)
			if (name.find(w) != std::string::npos)
				return true;
		return false;
	};
	static const std::initializer_list<const char *> NOT = {
		"eye",  "iris",  "pupil", "hair", "brow", "lash",  "teeth", "tooth", "tongue", "mouth_in", "cloth",
		"shirt", "pant", "dress", "shoe", "boot", "glove", "hat",   "cap",   "jacket", "coat",     "skirt",
		"sock",  "armor", "armour", "helmet", "glass", "mask", "beard", "mustache", "moustache", "suit",
		"phone", "band", "wear"};
	for (size_t i = 0; i < out.size(); i++) {
		const std::string name = lower(m.materials[i].name);
		out[i] = has(name, {"skin", "face", "body", "head"}) && !has(name, NOT);
	}
	return out;
}

namespace {

// What MikkTSpace asks for, from a part: its triangles (only those whose
// corners are all its points), each corner's place, facing and UV, and
// where each corner's tangent goes.
struct Mikk {
	const Primitive *p = nullptr;
	std::vector<uint32_t> tris; // where each triangle starts in p->index
	std::vector<float> *out = nullptr;
	std::vector<uint8_t> done; // a point's tangent is set (its first corner's)

	static Mikk &of(const SMikkTSpaceContext *c) { return *static_cast<Mikk *>(c->m_pUserData); }
	uint32_t point(int face, int vert) const { return p->index[tris[(size_t)face] + (uint32_t)vert]; }
	static int faces(const SMikkTSpaceContext *c) { return (int)of(c).tris.size(); }
	static int corners(const SMikkTSpaceContext *, const int) { return 3; }
	static void place(const SMikkTSpaceContext *c, float out[], const int face, const int vert)
	{
		const V3 v = of(c).p->pos[of(c).point(face, vert)];
		out[0] = v.x, out[1] = v.y, out[2] = v.z;
	}
	static void facing(const SMikkTSpaceContext *c, float out[], const int face, const int vert)
	{
		const V3 n = normalized(of(c).p->normal[of(c).point(face, vert)], {0, 0, 1});
		out[0] = n.x, out[1] = n.y, out[2] = n.z;
	}
	// A glTF picture's v runs down it; MikkTSpace's (as Blender's, where
	// most normal maps are baked) runs up.
	static void uv(const SMikkTSpaceContext *c, float out[], const int face, const int vert)
	{
		const float *t = &of(c).p->uv[(size_t)of(c).point(face, vert) * 2];
		out[0] = t[0], out[1] = 1 - t[1];
	}
	static void tangent(const SMikkTSpaceContext *c, const float t[], const float sign, const int face,
			    const int vert)
	{
		Mikk &m = of(c);
		const uint32_t i = m.point(face, vert);
		if (m.done[i] || !std::isfinite(t[0] + t[1] + t[2]))
			return;
		m.done[i] = 1;
		float *o = &(*m.out)[(size_t)i * 4];
		o[0] = t[0], o[1] = t[1], o[2] = t[2], o[3] = sign < 0 ? -1.0f : 1.0f;
	}
};

} // namespace

std::vector<float> tangents_of(const Primitive &p)
{
	const size_t n = p.pos.size();
	if (p.uv.size() != n * 2 || p.normal.size() != n)
		return {};
	// MikkTSpace's (Morten S. Mikkelsen's, zlib licence: deps/mikktspace),
	// as glTF asks and as Blender works them out - so a normal map baked
	// there lies on the part as it was made. A point shared by triangles
	// that want it two ways (a seam its file didn't split) takes its first
	// triangle's.
	std::vector<float> out(n * 4);
	Mikk m;
	m.p = &p;
	m.out = &out;
	m.done.assign(n, 0);
	for (size_t i = 0; i + 2 < p.index.size(); i += 3)
		if (p.index[i] < n && p.index[i + 1] < n && p.index[i + 2] < n && i <= UINT32_MAX)
			m.tris.push_back((uint32_t)i);
	if (!m.tris.empty() && m.tris.size() <= (size_t)std::numeric_limits<int>::max()) {
		SMikkTSpaceInterface calls = {};
		calls.m_getNumFaces = Mikk::faces;
		calls.m_getNumVerticesOfFace = Mikk::corners;
		calls.m_getPosition = Mikk::place;
		calls.m_getNormal = Mikk::facing;
		calls.m_getTexCoord = Mikk::uv;
		calls.m_setTSpaceBasic = Mikk::tangent;
		SMikkTSpaceContext context = {};
		context.m_pInterface = &calls;
		context.m_pUserData = &m;
		genTangSpaceDefault(&context);
	}
	// A point no triangle has (or one MikkTSpace couldn't do): any way
	// across its facing.
	for (size_t i = 0; i < n; i++) {
		float *o = &out[i * 4];
		const V3 nn = normalized(p.normal[i], {0, 0, 1});
		V3 t{o[0], o[1], o[2]};
		t = t - nn * dot(nn, t);
		if (!m.done[i] || !(length(t) > 1e-6f)) {
			t = cross(std::fabs(nn.y) < 0.9f ? V3{0, 1, 0} : V3{1, 0, 0}, nn);
			if (!m.done[i])
				o[3] = 1;
		}
		t = normalized(t, {1, 0, 0});
		o[0] = t.x, o[1] = t.y, o[2] = t.z;
	}
	return out;
}

bool load_model(const uint8_t *bytes, size_t size, const std::string &folder, Model &out, std::string *problem)
{
	out = Model();
	try {
		json doc;
		std::vector<uint8_t> bin;
		bool glb = false;
		open_file(bytes, size, doc, bin, glb);
		if (doc.is_discarded() || !doc.is_object())
			fail("it isn't a 3D model DireBot can read (.glb, .gltf or .vrm)");
		Reader rd(doc);
		rd.folder = folder;
		rd.glb = glb;
		rd.bin = std::move(bin);
		load_json(rd, doc, out);
		return true;
	} catch (const Fail &f) {
		if (problem)
			*problem = f.why;
	} catch (const std::bad_alloc &) {
		if (problem)
			*problem = "it's too big to load";
	} catch (const std::exception &) {
		if (problem)
			*problem = "the file is damaged";
	}
	out = Model();
	return false;
}

bool load_model(const std::string &path, Model &out, std::string *problem)
{
	std::vector<uint8_t> bytes;
	if (!read_file(path, bytes)) {
		if (problem)
			*problem = "DireBot can't open the file (or it's over 512 MB)";
		return false;
	}
	const size_t slash = path.find_last_of("/\\");
	const std::string folder = slash == std::string::npos ? "." : path.substr(0, slash);
	return load_model(bytes.data(), bytes.size(), folder, out, problem);
}

void sample(const AnimChannel &c, float t, float *out)
{
	const int n = c.size();
	const size_t keys = c.times.size();
	const bool curved = c.ease == AnimChannel::Curved;
	const size_t per = curved ? 3 : 1;
	// Key k's value (and, curved, its slopes either side of it).
	auto value = [&](size_t k) {
		return &c.values[(k * per + (curved ? 1 : 0)) * n];
	};
	if (!keys) {
		return;
	}
	if (keys == 1 || !(t > c.times.front())) {
		std::copy(value(0), value(0) + n, out);
	} else if (t >= c.times.back()) {
		std::copy(value(keys - 1), value(keys - 1) + n, out);
	} else {
		const size_t k = (size_t)(std::upper_bound(c.times.begin(), c.times.end(), t) - c.times.begin()) - 1;
		const float span = c.times[k + 1] - c.times[k];
		const float u = span > 0 ? (t - c.times[k]) / span : 0;
		const float *a = value(k), *b = value(k + 1);
		if (c.ease == AnimChannel::Step || span <= 0) {
			std::copy(a, a + n, out);
		} else if (!curved) {
			if (c.path == AnimChannel::Turn) {
				const Quat q = slerp(Quat{a[0], a[1], a[2], a[3]}, Quat{b[0], b[1], b[2], b[3]}, u);
				out[0] = q.x, out[1] = q.y, out[2] = q.z, out[3] = q.w;
			} else {
				for (int i = 0; i < n; i++)
					out[i] = a[i] + (b[i] - a[i]) * u;
			}
		} else {
			// glTF's curve: each key's value, its slope out of k and into
			// k + 1 (a second's worth, so times the gap).
			const float *slope_out = a + n, *slope_in = b - n;
			const float u2 = u * u, u3 = u2 * u;
			const float ka = 2 * u3 - 3 * u2 + 1, kb = -2 * u3 + 3 * u2;
			const float ta = (u3 - 2 * u2 + u) * span, tb = (u3 - u2) * span;
			for (int i = 0; i < n; i++)
				out[i] = ka * a[i] + ta * slope_out[i] + kb * b[i] + tb * slope_in[i];
		}
	}
	if (c.path == AnimChannel::Turn) {
		const Quat q = normalized(Quat{out[0], out[1], out[2], out[3]});
		out[0] = q.x, out[1] = q.y, out[2] = q.z, out[3] = q.w;
	}
}

int animation_named(const Model &m, const std::string &name)
{
	if (name.empty())
		return -1;
	for (size_t i = 0; i < m.animations.size(); i++)
		if (m.animations[i].name == name)
			return (int)i;
	return -1;
}

std::vector<std::vector<int>> hanging_chains(const Model &m, const Skin &skin)
{
	std::vector<char> in(m.nodes.size(), 0);
	for (int j : skin.joints)
		if (j >= 0 && j < (int)m.nodes.size())
			in[j] = 1;
	auto kids = [&](int n) {
		std::vector<int> out;
		for (int c : m.nodes[n].children)
			if (in[c])
				out.push_back(c);
		return out;
	};
	std::vector<std::vector<int>> chains;
	std::vector<int> starts;
	for (int j : skin.joints) {
		if (j < 0 || j >= (int)m.nodes.size())
			continue;
		const int p = m.nodes[j].parent;
		if (p < 0 || !in[p] || kids(p).size() > 1)
			starts.push_back(j);
	}
	std::sort(starts.begin(), starts.end());
	starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
	for (int s : starts) {
		std::vector<int> line;
		int at = s;
		bool free_end = false;
		while (line.size() <= m.nodes.size()) {
			line.push_back(at);
			const std::vector<int> next = kids(at);
			if (next.empty()) {
				free_end = true;
				break;
			}
			if (next.size() > 1)
				break; // it branches: a trunk, not a dangly part
			at = next[0];
		}
		if (free_end && line.size() >= 2)
			chains.push_back(line);
	}
	return chains;
}

bool person_skeleton(const Model &m, const Skin &skin)
{
	static const char *const WORDS[] = {"hips",     "spine",   "pelvis", "neck", "shoulder", "clavicle",
					    "upperarm", "forearm", "elbow",  "hand", "finger",   "thumb",
					    "thigh",    "calf",    "knee",   "foot", "toe",      "upleg"};
	std::set<std::string> found;
	for (int j : skin.joints) {
		if (j < 0 || j >= (int)m.nodes.size())
			continue;
		std::string n = m.nodes[j].name;
		for (char &c : n)
			c = (char)std::tolower((unsigned char)c);
		for (const char *w : WORDS)
			if (n.find(w) != std::string::npos)
				found.insert(w);
	}
	return found.size() >= 3;
}

std::vector<M4> rest_world(const Model &m)
{
	std::vector<M4> world(m.nodes.size());
	std::vector<int> stack;
	for (size_t i = 0; i < m.nodes.size(); i++)
		if (m.nodes[i].parent < 0)
			stack.push_back((int)i);
	while (!stack.empty()) {
		const int n = stack.back();
		stack.pop_back();
		const Node &node = m.nodes[n];
		world[n] = node.parent >= 0 ? world[node.parent] * node.local() : node.local();
		for (int c : node.children)
			stack.push_back(c);
	}
	return world;
}

namespace {

void put_floats(std::vector<uint8_t> &bin, const float *f, size_t n)
{
	const size_t at = bin.size();
	bin.resize(at + n * 4);
	std::memcpy(bin.data() + at, f, n * 4);
}

} // namespace

std::vector<uint8_t> write_glb(const std::vector<GlbPart> &parts)
{
	json doc = {{"asset", {{"version", "2.0"}, {"generator", "DireBot"}}}};
	json nodes = json::array(), meshes = json::array(), materials = json::array(), accessors = json::array(),
	     views = json::array(), images = json::array(), textures = json::array(), roots = json::array();
	std::vector<uint8_t> bin;
	auto view = [&](size_t from, size_t len, int target) {
		json v = {{"buffer", 0}, {"byteOffset", from}, {"byteLength", len}};
		if (target)
			v["target"] = target;
		views.push_back(v);
		return (int)views.size() - 1;
	};
	auto add_vec = [&](const float *f, size_t count, int comps, const char *type, bool bounds) {
		const size_t from = bin.size();
		put_floats(bin, f, count * comps);
		json a = {{"bufferView", view(from, count * comps * 4, 34962)},
			  {"componentType", 5126},
			  {"count", count},
			  {"type", type}};
		if (bounds && count) {
			json lo = json::array(), hi = json::array();
			for (int c = 0; c < comps; c++) {
				float l = f[c], h = f[c];
				for (size_t i = 0; i < count; i++) {
					l = std::min(l, f[i * comps + c]);
					h = std::max(h, f[i * comps + c]);
				}
				lo.push_back(l);
				hi.push_back(h);
			}
			a["min"] = lo;
			a["max"] = hi;
		}
		accessors.push_back(a);
		return (int)accessors.size() - 1;
	};
	for (const GlbPart &p : parts) {
		json mat = {{"name", p.name},
			    {"pbrMetallicRoughness",
			     {{"baseColorFactor", {p.colour[0], p.colour[1], p.colour[2], p.colour[3]}},
			      {"metallicFactor", p.metallic},
			      {"roughnessFactor", p.roughness}}}};
		if (p.colour[3] < 1)
			mat["alphaMode"] = "BLEND";
		if (!p.png.empty()) {
			const size_t from = bin.size();
			bin.insert(bin.end(), p.png.begin(), p.png.end());
			while (bin.size() % 4)
				bin.push_back(0);
			images.push_back({{"bufferView", view(from, p.png.size(), 0)}, {"mimeType", "image/png"}});
			textures.push_back({{"source", images.size() - 1}});
			mat["pbrMetallicRoughness"]["baseColorTexture"] = {{"index", textures.size() - 1}};
		}
		materials.push_back(mat);
		json attr = {{"POSITION", add_vec(&p.pos[0].x, p.pos.size(), 3, "VEC3", true)}};
		if (p.normal.size() == p.pos.size())
			attr["NORMAL"] = add_vec(&p.normal[0].x, p.normal.size(), 3, "VEC3", false);
		if (p.uv.size() == p.pos.size() * 2)
			attr["TEXCOORD_0"] = add_vec(p.uv.data(), p.pos.size(), 2, "VEC2", false);
		const size_t from = bin.size();
		bin.resize(from + p.index.size() * 4);
		std::memcpy(bin.data() + from, p.index.data(), p.index.size() * 4);
		accessors.push_back({{"bufferView", view(from, p.index.size() * 4, 34963)},
				     {"componentType", 5125},
				     {"count", p.index.size()},
				     {"type", "SCALAR"}});
		json prim = {{"attributes", attr},
			     {"indices", accessors.size() - 1},
			     {"material", materials.size() - 1}};
		json mesh = {{"name", p.name}};
		if (!p.morphs.empty()) {
			json targets = json::array();
			for (const std::vector<V3> &mt : p.morphs)
				targets.push_back({{"POSITION", add_vec(&mt[0].x, mt.size(), 3, "VEC3", true)}});
			prim["targets"] = targets;
			mesh["extras"] = {{"targetNames", p.morph_names}};
			mesh["weights"] = std::vector<float>(p.morphs.size(), 0.0f);
		}
		mesh["primitives"] = json::array({prim});
		meshes.push_back(mesh);
		nodes.push_back({{"name", p.name},
				 {"mesh", meshes.size() - 1},
				 {"translation", {p.t.x, p.t.y, p.t.z}},
				 {"rotation", {p.r.x, p.r.y, p.r.z, p.r.w}}});
		roots.push_back(nodes.size() - 1);
	}
	doc["nodes"] = nodes;
	doc["meshes"] = meshes;
	doc["materials"] = materials;
	doc["accessors"] = accessors;
	doc["bufferViews"] = views;
	if (!images.empty()) {
		doc["images"] = images;
		doc["textures"] = textures;
	}
	doc["buffers"] = json::array({{{"byteLength", bin.size()}}});
	doc["scenes"] = json::array({{{"nodes", roots}}});
	doc["scene"] = 0;
	std::string text_json = doc.dump();
	while (text_json.size() % 4)
		text_json.push_back(' ');
	while (bin.size() % 4)
		bin.push_back(0);
	std::vector<uint8_t> out;
	auto put32 = [&out](uint32_t v) {
		for (int i = 0; i < 4; i++)
			out.push_back((uint8_t)(v >> (8 * i)));
	};
	out.insert(out.end(), {'g', 'l', 'T', 'F'});
	put32(2);
	put32((uint32_t)(12 + 8 + text_json.size() + 8 + bin.size()));
	put32((uint32_t)text_json.size());
	put32(0x4E4F534A);
	out.insert(out.end(), text_json.begin(), text_json.end());
	put32((uint32_t)bin.size());
	put32(0x004E4942);
	out.insert(out.end(), bin.begin(), bin.end());
	return out;
}

} // namespace direbot::face
