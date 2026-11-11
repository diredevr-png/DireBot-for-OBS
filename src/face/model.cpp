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
// Hair strands: the most points a file's strands may have, all its strand
// parts together (how many of them are kept is LoadOptions' to say), and
// the most groups a part's strands are drawn in.
constexpr size_t MOST_STRAND_POINTS = 20'000'000;
constexpr size_t MOST_STRAND_GROUPS = 32;
constexpr size_t MOST_NODES = 100'000;
constexpr size_t MOST_MORPHS = 512;
// An expression's move of a point too small to see (squared lengths:
// 1e-7 of a move, 1e-5 of a turn of its facing): not kept.
constexpr float MOVES_NOTHING = 1e-14f, TURNS_NOTHING = 1e-10f;
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
// A size or place in a file, in bytes (or a count): exactly, as a float
// can't hold one past 16 million (a part of a big file read from the
// wrong place: "a buffer view past its end").
double whole(const json &j, const char *key, double def)
{
	const json &v = field(j, key);
	if (!v.is_number())
		return def;
	const double d = v.get<double>();
	return std::isfinite(d) ? d : def;
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
		const double off = whole(v, "byteOffset", 0), len = whole(v, "byteLength", -1);
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
		const long long count = (long long)whole(acc, "count", -1);
		if (count < 0 || (size_t)count > MOST_INDEX)
			fail("the file is too big or damaged");
		const bool normalized = flag(acc, "normalized", false);
		std::vector<double> out((size_t)count * comps, 0.0);
		const int view_i = index_of(acc, "bufferView");
		if (view_i >= 0 && count > 0) {
			const View v = view(view_i);
			const double off = whole(acc, "byteOffset", 0);
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
			const long long n = (long long)whole(sparse, "count", 0);
			const json &ix = field(sparse, "indices"), &vals = field(sparse, "values");
			const int itype = index_of(ix, "componentType");
			const int isize = size_of(itype);
			if (n < 0 || n > count || !isize || itype == 5126)
				fail("the file is damaged (sparse numbers)");
			if (n > 0) {
				const View iv = view(index_of(ix, "bufferView"));
				const View vv = view(index_of(vals, "bufferView"));
				const double ioff = whole(ix, "byteOffset", 0), voff = whole(vals, "byteOffset", 0);
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

	// An accessor's numbers read a point at a time (a big groom's are
	// read only for the strands kept): `get` reads the file's own, and
	// its sparse entries (the points they replace, their numbers after)
	// are listed beside them. data null: all zeros.
	struct Column {
		const uint8_t *data = nullptr;
		size_t stride = 0, count = 0;
		int comps = 0, type = 0, csize = 0;
		bool normalized = false;
		std::vector<uint32_t> sparse_at;
		std::vector<double> sparse_values;
		void get(size_t i, double *out) const
		{
			if (!data) {
				for (int c = 0; c < comps; c++)
					out[c] = 0;
				return;
			}
			const uint8_t *p = data + i * stride;
			for (int c = 0; c < comps; c++)
				out[c] = value(p + (size_t)c * csize, type, normalized);
		}
	};
	Column column(int a, int want_comps, size_t most)
	{
		const json &accessors = list(doc, "accessors");
		if (a < 0 || a >= (int)accessors.size())
			fail("the file is damaged (a missing accessor)");
		const json &acc = accessors[a];
		Column col;
		col.type = index_of(acc, "componentType");
		col.comps = comps_of(text(acc, "type"));
		col.csize = size_of(col.type);
		if (!col.csize || !col.comps || col.comps > 4 || (want_comps && col.comps != want_comps))
			fail("the file has numbers DireBot can't read");
		const long long count = (long long)whole(acc, "count", -1);
		if (count < 0 || (size_t)count > most)
			fail("the file is too big or damaged");
		col.count = (size_t)count;
		col.normalized = flag(acc, "normalized", false);
		const int view_i = index_of(acc, "bufferView");
		const size_t elem = (size_t)col.comps * col.csize;
		if (view_i >= 0 && count > 0) {
			const View v = view(view_i);
			const double off = whole(acc, "byteOffset", 0);
			col.stride = v.stride ? v.stride : elem;
			if (off < 0 || off + (double)col.stride * (count - 1) + elem > (double)v.size)
				fail("the file is damaged (numbers past their end)");
			col.data = v.data + (size_t)off;
		}
		const json &sparse = field(acc, "sparse");
		if (sparse.is_object()) {
			const long long n = (long long)whole(sparse, "count", 0);
			const json &ix = field(sparse, "indices"), &vals = field(sparse, "values");
			const int itype = index_of(ix, "componentType");
			const int isize = size_of(itype);
			if (n < 0 || n > count || !isize || itype == 5126)
				fail("the file is damaged (sparse numbers)");
			if (n > 0) {
				const View iv = view(index_of(ix, "bufferView"));
				const View vv = view(index_of(vals, "bufferView"));
				const double ioff = whole(ix, "byteOffset", 0), voff = whole(vals, "byteOffset", 0);
				if (ioff < 0 || voff < 0 || ioff + (double)n * isize > (double)iv.size ||
				    voff + (double)n * elem > (double)vv.size)
					fail("the file is damaged (sparse numbers past their end)");
				col.sparse_at.resize((size_t)n);
				col.sparse_values.resize((size_t)n * col.comps);
				for (long long k = 0; k < n; k++) {
					const double at =
						value(iv.data + (size_t)ioff + (size_t)k * isize, itype, false);
					if (at < 0 || at >= (double)count)
						fail("the file is damaged (sparse numbers)");
					col.sparse_at[(size_t)k] = (uint32_t)at;
					for (int c = 0; c < col.comps; c++)
						col.sparse_values[(size_t)k * col.comps + c] =
							value(vv.data + (size_t)voff +
								      ((size_t)k * col.comps + c) * col.csize,
							      col.type, col.normalized);
				}
			}
		}
		return col;
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
		std::vector<float> out(d.size());
		for (size_t i = 0; i < d.size(); i++)
			out[i] = (float)d[i];
		return out;
	}
};

// ---- Hair strands (model.hpp's Strands) ----

constexpr uint32_t NO_POINT = 0xFFFFFFFFu;

// A file's strand part (glTF LINES, mode 1, or one LINE_STRIP, mode 3)
// while it's read: its points as the file has them, each strand's found
// by following its pieces from its root (the point no piece comes into),
// and its strands in the order they're kept and drawn in. The rest of
// what the file says of each point is read only for the points kept.
struct RawStrands {
	std::vector<V3> pos;
	std::vector<uint32_t> next; // the point after each on its strand (NO_POINT: a tip)
	std::vector<uint32_t> root; // each strand's root, in draw order
	// Each of those strands' place among them spread evenly (spread_order),
	// and its rate (Strands::rate) - empty: its place in `root`, and 1.
	std::vector<uint32_t> rank;
	std::vector<float> rate;
	// Its highest rate before they were made 1 at most: its keys / this
	// are measured as every other part's are.
	float top = 1;
	// Every so-manyth strand: its place spread evenly, the points it
	// would keep, and its key.
	struct Sample {
		uint32_t rank = 0, kept = 0;
		double key = 0;
	};
	std::vector<Sample> sample;
	bool has_width = false, has_colour = false, has_bones = false;
	Reader::Column width, colour, joints, weights;
	std::vector<int> targets; // its expressions' moves: each one's accessor (-1: none)
	// Its expressions' moves strand by strand (this add-on's own extra in
	// the file, extras.direbot.strand_morphs: a groom whose every point's
	// move would make the file too big). Strand k of them is points
	// strand_start[k] up to strand_start[k + 1]; each expression lists
	// the strands it moves (rising) and how far each - all of a strand's
	// points alike. On top of any moves the usual way.
	std::vector<uint32_t> strand_start;
	struct StrandMoves {
		std::vector<uint32_t> strands;
		std::vector<V3> moves;
	};
	std::vector<StrandMoves> strand_moves; // an expression's (none: empty)
	// Its expressions' moves the usual way, read a point at a time
	// (read_strand_moves: one for each of `targets`; not read: empty), and
	// whether any of them moves each point (none does: empty). A point an
	// expression moves unlike the points kept beside it is kept
	// (shape_strand).
	std::vector<Reader::Column> moves;
	std::vector<uint8_t> moved;
	float bend = 0;           // points that bend their strand less are left out (its units)
	float turn = -1;          // ...unless it turns more than this between kept points (the angle's cosine)
	float swing = 0;          // ...or its bones could bend it more than this there
	double predicted = 0;     // the points all its strands would keep
	size_t allowed = 0;       // the points it may keep
	int material = -1;
};

// The order a part's strands are kept and drawn in: however many of the
// first are taken, they're spread evenly over the coat. Their roots are
// put in order along a curve that fills their box (near on it: near in
// space), then picked by halving it: the middle one, the quarters, the
// eighths...
std::vector<uint32_t> spread_order(const std::vector<V3> &pos, const std::vector<uint32_t> &roots)
{
	const size_t n = roots.size();
	V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
	for (uint32_t r : roots) {
		const V3 v = pos[r];
		lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
		hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
	}
	const float size = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z, 1e-20f});
	auto apart = [](float f) {
		uint32_t v = (uint32_t)std::min(1023.0f, std::max(0.0f, f));
		v = (v | v << 16) & 0x030000FFu;
		v = (v | v << 8) & 0x0300F00Fu;
		v = (v | v << 4) & 0x030C30C3u;
		v = (v | v << 2) & 0x09249249u;
		return v;
	};
	std::vector<std::pair<uint32_t, uint32_t>> coded(n);
	for (size_t i = 0; i < n; i++) {
		const V3 q = (pos[roots[i]] - lo) * (1023.99f / size);
		coded[i] = {apart(q.x) | apart(q.y) << 1 | apart(q.z) << 2, (uint32_t)i};
	}
	std::sort(coded.begin(), coded.end());
	int bits = 0;
	while (((size_t)1 << bits) < n)
		bits++;
	std::vector<uint32_t> order;
	order.reserve(n);
	for (size_t k = 0; k < ((size_t)1 << bits); k++) {
		size_t m = 0;
		for (int b = 0; b < bits; b++)
			if (k >> b & 1)
				m |= (size_t)1 << (bits - 1 - b);
		if (m < n)
			order.push_back(roots[coded[m].second]);
	}
	return order;
}

RawStrands open_strands(Reader &rd, const json &pj, const json &attr, int mode, size_t morphs, size_t &file_points)
{
	RawStrands raw;
	const Reader::Column pc = rd.column(index_of(attr, "POSITION"), 3, MOST_STRAND_POINTS);
	const size_t n = pc.count;
	file_points += n;
	if (file_points > MOST_STRAND_POINTS)
		fail("the file is too big (over 20 million hair points)");
	auto v3 = [](const double *v) {
		return V3{(float)v[0], (float)v[1], (float)v[2]};
	};
	raw.pos.resize(n);
	double v[4];
	for (size_t i = 0; i < n; i++) {
		pc.get(i, v);
		raw.pos[i] = v3(v);
	}
	for (size_t k = 0; k < pc.sparse_at.size(); k++)
		raw.pos[pc.sparse_at[k]] = v3(&pc.sparse_values[k * 3]);
	// Its pieces: pairs of points (lines), or each point to the next (a
	// line strip). A point keeps the first piece out of it and into it.
	raw.next.assign(n, NO_POINT);
	std::vector<char> entered(n, 0);
	auto link = [&](uint32_t a, uint32_t b) {
		if (a != b && raw.next[a] == NO_POINT && !entered[b]) {
			raw.next[a] = b;
			entered[b] = 1;
		}
	};
	const size_t step = mode == 1 ? 2 : 1;
	if (index_of(pj, "indices") >= 0) {
		const Reader::Column ic = rd.column(index_of(pj, "indices"), 1, MOST_STRAND_POINTS * 2);
		if (!ic.sparse_at.empty() || ic.type == 5126)
			fail("the file has numbers DireBot can't read");
		auto at = [&](size_t i) {
			double d = -1;
			ic.get(i, &d);
			if (d < 0 || d >= (double)n)
				fail("the file is damaged (a triangle past its points)");
			return (uint32_t)d;
		};
		for (size_t i = 0; i + 1 < ic.count; i += step)
			link(at(i), at(i + 1));
	} else {
		for (size_t i = 0; i + 1 < n; i += step)
			link((uint32_t)i, (uint32_t)i + 1);
	}
	std::vector<uint32_t> roots;
	for (uint32_t i = 0; i < n; i++)
		if (!entered[i] && raw.next[i] != NO_POINT)
			roots.push_back(i);
	raw.root = spread_order(raw.pos, roots);

	auto fits = [n](const Reader::Column &c) {
		return c.count == n;
	};
	if (index_of(attr, "_WIDTH") >= 0) {
		raw.width = rd.column(index_of(attr, "_WIDTH"), 1, MOST_STRAND_POINTS);
		raw.has_width = fits(raw.width);
	}
	if (index_of(attr, "COLOR_0") >= 0) {
		raw.colour = rd.column(index_of(attr, "COLOR_0"), 0, MOST_STRAND_POINTS);
		raw.has_colour = fits(raw.colour) && (raw.colour.comps == 3 || raw.colour.comps == 4);
	}
	if (index_of(attr, "JOINTS_0") >= 0 && index_of(attr, "WEIGHTS_0") >= 0) {
		raw.joints = rd.column(index_of(attr, "JOINTS_0"), 4, MOST_STRAND_POINTS);
		raw.weights = rd.column(index_of(attr, "WEIGHTS_0"), 4, MOST_STRAND_POINTS);
		if (!fits(raw.joints) || !fits(raw.weights))
			fail("the file is damaged (its parts don't add up)");
		raw.has_bones = true;
	}
	const json &targets = list(pj, "targets");
	raw.targets.assign(morphs, -1);
	for (size_t t = 0; t < targets.size() && t < morphs; t++)
		raw.targets[t] = index_of(targets[t], "POSITION");
	// Its moves strand by strand.
	const json &by_strand = field(field(field(pj, "extras"), "direbot"), "strand_morphs");
	if (by_strand.is_object() && index_of(by_strand, "strand_start") >= 0) {
		// A list of whole numbers that never fall, none over `most`.
		auto numbers = [&](int accessor, double most, bool rising) {
			const Reader::Column c = rd.column(accessor, 1, MOST_STRAND_POINTS + 1);
			if (!c.sparse_at.empty() || c.type == 5126)
				fail("the file has numbers DireBot can't read");
			std::vector<uint32_t> out(c.count);
			double d = 0, last = -1;
			for (size_t i = 0; i < c.count; i++) {
				c.get(i, &d);
				if (!(d >= 0) || d > most || d < last || (rising && d == last))
					fail("the file is damaged (its hair's moves don't fit its strands)");
				out[i] = (uint32_t)d;
				last = d;
			}
			return out;
		};
		raw.strand_start = numbers(index_of(by_strand, "strand_start"), (double)n, false);
		if (raw.strand_start.size() < 2)
			fail("the file is damaged (its hair's moves don't fit its strands)");
		const size_t in_all = raw.strand_start.size() - 1;
		const json &each = list(by_strand, "targets");
		raw.strand_moves.assign(morphs, {});
		for (size_t t = 0; t < each.size() && t < morphs; t++) {
			if (!each[t].is_object() || index_of(each[t], "strands") < 0)
				continue;
			RawStrands::StrandMoves &sm = raw.strand_moves[t];
			sm.strands = numbers(index_of(each[t], "strands"), (double)in_all - 1, true);
			sm.moves = rd.vec3s(index_of(each[t], "POSITION"));
			if (sm.moves.size() != sm.strands.size())
				fail("the file is damaged (its hair's moves don't fit its strands)");
		}
	}
	raw.material = index_of(pj, "material");
	return raw;
}

// A point's numbers, the file's own or its sparse entry's.
void column_point(const Reader::Column &c, size_t i, double *out)
{
	c.get(i, out);
	if (!c.sparse_at.empty()) {
		auto it = std::lower_bound(c.sparse_at.begin(), c.sparse_at.end(), (uint32_t)i);
		if (it != c.sparse_at.end() && *it == i) {
			const size_t k = (size_t)(it - c.sparse_at.begin());
			for (int j = 0; j < c.comps; j++)
				out[j] = c.sparse_values[k * c.comps + j];
		}
	}
}

void srgb_of(const double *linear, int comps, uint8_t *out)
{
	for (int k = 0; k < 3; k++) {
		const float v = std::min(1.0f, std::max(0.0f, (float)linear[k]));
		out[k] = (uint8_t)std::lround(std::pow(v, 1 / 2.2f) * 255);
	}
	out[3] = comps > 3 ? (uint8_t)std::lround(std::min(1.0, std::max(0.0, linear[3])) * 255) : 255;
}

// One strand: its points root to tip, how far along it each is, its
// widths, colours and bones - and which of them are kept: its root and
// tip, and every point that bends it (or changes its width by a tenth of
// its widest, or its colour by a shade you'd see) more than a straight
// run between the kept points beside it would. Where it curls, enough
// points that it never turns far from one to the next (a small curl is
// never far off a straight run, and would be left a row of corners). And
// where its bones change along it, enough points for them to bend it: a
// straight hair hung on a chain of bones still swings in a curve. And
// where an expression moves a point unlike the points beside it (a hair
// pressed aside in its middle, or curled), that point: no point left out
// is moved further than that same limit from where the kept points
// beside it carry it.
struct StrandShape {
	std::vector<uint32_t> at;
	std::vector<float> arc, width;
	std::vector<uint8_t> colour;
	std::vector<uint16_t> joints; // 4 a point, and how much each holds it (adding up to 1)
	std::vector<float> weights;
	// Each expression that moves any of its points (the usual way): every
	// point's move, one expression's after another's.
	std::vector<V3> moves;
	std::vector<char> keep;
	size_t kept = 0;
	std::vector<std::pair<uint32_t, uint32_t>> todo;
};

void shape_strand(const RawStrands &raw, uint32_t root, StrandShape &s)
{
	s.at.clear();
	s.arc.clear();
	s.width.clear();
	s.colour.clear();
	s.joints.clear();
	s.weights.clear();
	float run = 0;
	for (uint32_t a = root; a != NO_POINT && s.at.size() <= raw.pos.size(); a = raw.next[a]) {
		if (!s.at.empty())
			run += length(raw.pos[a] - raw.pos[s.at.back()]);
		s.at.push_back(a);
		s.arc.push_back(run);
	}
	const size_t n = s.at.size();
	float widest = 0;
	double v[4];
	for (size_t i = 0; i < n; i++) {
		float w = 0.0001f;
		if (raw.has_width) {
			column_point(raw.width, s.at[i], v);
			w = std::max(0.0f, (float)v[0]);
		}
		s.width.push_back(w);
		widest = std::max(widest, w);
		if (raw.has_colour) {
			column_point(raw.colour, s.at[i], v);
			uint8_t c[4];
			srgb_of(v, raw.colour.comps, c);
			s.colour.insert(s.colour.end(), c, c + 4);
		}
		if (raw.has_bones) {
			double j[4], w[4];
			column_point(raw.joints, s.at[i], j);
			column_point(raw.weights, s.at[i], w);
			float sum = 0, wk[4];
			for (int k = 0; k < 4; k++) {
				s.joints.push_back(j[k] >= 0 && j[k] < 65536 ? (uint16_t)j[k] : 0);
				wk[k] = std::max(0.0f, (float)w[k]);
				sum += wk[k];
			}
			for (int k = 0; k < 4; k++)
				s.weights.push_back(sum > 1e-6f ? wk[k] / sum : (k == 0 ? 1.0f : 0.0f));
		}
	}
	// Its expressions' moves, of a strand any of them moves.
	s.moves.clear();
	bool moved = false;
	if (!raw.moved.empty() && raw.bend > 0 && n >= 3) {
		uint32_t low = s.at[0], high = s.at[0];
		for (size_t i = 0; i < n; i++) {
			moved = moved || raw.moved[s.at[i]];
			low = std::min(low, s.at[i]);
			high = std::max(high, s.at[i]);
		}
		for (size_t t = 0; moved && t < raw.moves.size(); t++) {
			const Reader::Column &c = raw.moves[t];
			if (c.comps != 3 || c.count != raw.pos.size())
				continue;
			// (One whose listed points are none of this strand's: not read.)
			if (!c.data) {
				const auto first = std::lower_bound(c.sparse_at.begin(), c.sparse_at.end(), low);
				if (first == c.sparse_at.end() || *first > high)
					continue;
			}
			const size_t from = s.moves.size();
			s.moves.resize(from + n);
			bool any = false;
			for (size_t i = 0; i < n; i++) {
				column_point(c, s.at[i], v);
				const V3 m{(float)v[0], (float)v[1], (float)v[2]};
				if (dot(m, m) > MOVES_NOTHING) {
					s.moves[from + i] = m;
					any = true;
				}
			}
			if (!any)
				s.moves.resize(from);
		}
	}
	// How unlike point i's bones are to those `u` of the way from point
	// a's to point b's: 0 the same, 1 none shared.
	auto unlike = [&s](uint32_t i, uint32_t a, uint32_t b, float u) {
		auto held = [&s](uint32_t p, uint16_t joint) {
			float w = 0;
			for (int k = 0; k < 4; k++)
				if (s.joints[p * 4 + k] == joint)
					w += s.weights[p * 4 + k];
			return w;
		};
		float sum = 0;
		const uint32_t from[3] = {i, a, b};
		for (int f = 0; f < 3; f++)
			for (int k = 0; k < 4; k++) {
				const uint16_t joint = s.joints[from[f] * 4 + k];
				// (Each bone once: where it first comes up.)
				bool seen = false;
				for (int g = 0; g <= f && !seen; g++)
					for (int q = 0; q < (g < f ? 4 : k) && !seen; q++)
						seen = s.joints[from[g] * 4 + q] == joint;
				if (!seen)
					sum += std::fabs(held(i, joint) -
							 (held(a, joint) * (1 - u) + held(b, joint) * u));
			}
		return sum * 0.5f;
	};
	s.keep.assign(n, raw.bend > 0 && n >= 3 ? 0 : 1);
	s.kept = n;
	if (!(raw.bend > 0) || n < 3)
		return;
	s.keep[0] = s.keep[n - 1] = 1;
	s.kept = 2;
	s.todo.clear();
	s.todo.push_back({0u, (uint32_t)n - 1});
	while (!s.todo.empty()) {
		const uint32_t a = s.todo.back().first, b = s.todo.back().second;
		s.todo.pop_back();
		if (b <= a + 1)
			continue;
		const V3 pa = raw.pos[s.at[a]], ab = raw.pos[s.at[b]] - pa;
		const float ab2 = dot(ab, ab), span = s.arc[b] - s.arc[a];
		float worst = 1, furthest = -1;
		uint32_t at = 0, far = a + 1;
		for (uint32_t i = a + 1; i < b; i++) {
			const V3 ap = raw.pos[s.at[i]] - pa;
			const float t = ab2 > 0 ? std::min(1.0f, std::max(0.0f, dot(ap, ab) / ab2)) : 0.0f;
			const float off = length(ap - ab * t);
			if (off > furthest) {
				furthest = off;
				far = i;
			}
			float err = off / raw.bend;
			const float u = span > 0 ? (s.arc[i] - s.arc[a]) / span : 0.0f;
			if (widest > 0)
				err = std::max(err,
					       std::fabs(s.width[i] - (s.width[a] + (s.width[b] - s.width[a]) * u)) /
						       (0.1f * widest));
			if (raw.has_colour)
				for (int k = 0; k < 4; k++) {
					const float ca = s.colour[a * 4 + k], cb = s.colour[b * 4 + k];
					err = std::max(err,
						       std::fabs(s.colour[i * 4 + k] - (ca + (cb - ca) * u)) / 6.0f);
				}
			// (Its bones not a steady change from one end's to the other's.)
			if (raw.has_bones)
				err = std::max(err, unlike(i, a, b, u) / 0.05f);
			// (An expression's move of it, not one between the two ends'.)
			for (size_t m = 0; m < s.moves.size(); m += n) {
				const V3 ma = s.moves[m + a], mb = s.moves[m + b];
				err = std::max(err, length(s.moves[m + i] - (ma + (mb - ma) * u)) / raw.bend);
			}
			if (err > worst) {
				worst = err;
				at = i;
			}
		}
		// A run that turns too far from its start to its end: split where
		// it's furthest off.
		if (!at && raw.turn > -1) {
			const V3 da = raw.pos[s.at[a + 1]] - pa, db = raw.pos[s.at[b]] - raw.pos[s.at[b - 1]];
			const float la = length(da), lb = length(db);
			if (la > 0 && lb > 0 && dot(da, db) < raw.turn * la * lb)
				at = far;
		}
		// A run whose ends hang from different bones: they bend it by up to
		// its length x how unlike they are - too much, and it's halved.
		if (!at && raw.has_bones && raw.swing > 0 && unlike(a, a, b, 1.0f) * span > raw.swing) {
			const float half = s.arc[a] + span * 0.5f;
			at = a + 1;
			for (uint32_t i = a + 1; i < b; i++)
				if (std::fabs(s.arc[i] - half) < std::fabs(s.arc[at] - half))
					at = i;
		}
		if (at) {
			s.keep[at] = 1;
			s.kept++;
			s.todo.push_back({a, at});
			s.todo.push_back({at, b});
		}
	}
}

// A strand part's expressions' moves the usual way, read for shape_strand
// (RawStrands::moves, moved).
void read_strand_moves(Reader &rd, RawStrands &raw)
{
	const size_t n = raw.pos.size();
	raw.moves.assign(raw.targets.size(), Reader::Column());
	raw.moved.clear();
	auto mark = [&raw, n](size_t i) {
		if (raw.moved.empty())
			raw.moved.assign(n, 0);
		raw.moved[i] = 1;
	};
	double v[4];
	for (size_t t = 0; t < raw.targets.size(); t++) {
		if (raw.targets[t] < 0)
			continue;
		Reader::Column &c = raw.moves[t];
		c = rd.column(raw.targets[t], 3, MOST_STRAND_POINTS);
		if (c.count != n)
			fail("the file is damaged (an expression doesn't fit its part)");
		if (c.data)
			for (size_t i = 0; i < n; i++) {
				c.get(i, v);
				if (v[0] * v[0] + v[1] * v[1] + v[2] * v[2] > (double)MOVES_NOTHING)
					mark(i);
			}
		for (size_t k = 0; k < c.sparse_at.size(); k++) {
			const double *m = &c.sparse_values[k * 3];
			if (c.sparse_at[k] < n && m[0] * m[0] + m[1] * m[1] + m[2] * m[2] > (double)MOVES_NOTHING)
				mark(c.sparse_at[k]);
		}
	}
}

// How many points all a part's strands would keep, from one of every so
// many of them: one picked from each run of that many in a row (by a
// rule that's the same every time), not the first of each - every
// so-manyth strand of an order made by halving is no fair sample.
void predict_strands(RawStrands &raw)
{
	const size_t n = raw.root.size(), step = std::max<size_t>(1, n / 2000);
	StrandShape s;
	double sum = 0;
	size_t tried = 0;
	uint32_t pick = 12345;
	raw.sample.clear();
	for (size_t from = 0; from < n; from += step, tried++) {
		pick = pick * 1664525u + 1013904223u;
		const size_t i = from + (size_t)(pick >> 8) % std::min(step, n - from);
		shape_strand(raw, raw.root[i], s);
		sum += (double)s.kept;
		raw.sample.push_back({(uint32_t)i, (uint32_t)s.kept, (double)i / (double)n});
	}
	raw.predicted = tried ? sum / (double)tried * (double)n : 0;
}

// How thickly hair covers the skin at each of `at`, its strands' roots:
// the areas of the hairs (`area`: each one's width x length) rooted near
// it, over the skin's area there - 3 where the hair would cover its skin
// three times over. Near: within a distance such that a few dozen roots
// are, the nearer ones counting for more (so it changes smoothly from
// root to root). The roots are put in boxes, and each box's hairs counted
// as one lot at their middle.
std::vector<float> cover_at(const std::vector<V3> &at, const std::vector<float> &area)
{
	const size_t n = at.size();
	std::vector<float> cover(n, 0.0f);
	if (!n)
		return cover;
	V3 lo = at[0], hi = at[0];
	for (const V3 &v : at) {
		lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
		hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
	}
	const double size = std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z, 1e-20f});
	const double least = size / 1048576.0;
	constexpr int64_t LAST = 2097151;
	// The box a root is in, of boxes `g` on a side.
	auto box_of = [&](const V3 &v, double g) {
		auto one = [g](float d) {
			return (uint64_t)std::min((double)LAST, std::max(0.0, std::floor((double)d / g)));
		};
		return one(v.x - lo.x) << 42 | one(v.y - lo.y) << 21 | one(v.z - lo.z);
	};
	// The size of box that holds 14 roots or so (on a skin: as many times
	// more as its side is longer, squared), from every so-manyth root.
	const size_t step = std::max<size_t>(1, n / 50000), some = (n + step - 1) / step;
	double g = size / 32;
	std::vector<uint64_t> boxes(some);
	for (int pass = 0; pass < 8; pass++) {
		for (size_t i = 0, k = 0; i < n; i += step, k++)
			boxes[k] = box_of(at[i], g);
		std::sort(boxes.begin(), boxes.end());
		const double each = (double)some / (double)(std::unique(boxes.begin(), boxes.end()) - boxes.begin());
		if ((each > 10 && each < 20) || (each <= 10 && g >= size) || (each >= 20 && g <= least))
			break;
		g = std::min(size, std::max(least, g * std::sqrt(14.0 / each)));
	}
	const double h = std::max(least, g / std::sqrt((double)step));
	// The roots box by box; each box: where its roots start, their hairs'
	// areas added up, and their middle.
	std::vector<std::pair<uint64_t, uint32_t>> coded(n);
	for (size_t i = 0; i < n; i++)
		coded[i] = {box_of(at[i], h), (uint32_t)i};
	std::sort(coded.begin(), coded.end());
	std::vector<uint64_t> box;
	std::vector<uint32_t> from;
	std::vector<V3> middle;
	std::vector<float> lot;
	for (size_t k = 0; k < n; k++) {
		if (!k || coded[k].first != coded[k - 1].first) {
			box.push_back(coded[k].first);
			from.push_back((uint32_t)k);
			middle.push_back(V3{});
			lot.push_back(0.0f);
		}
		middle.back() = middle.back() + at[coded[k].second];
		lot.back() += area[coded[k].second];
	}
	from.push_back((uint32_t)n);
	for (size_t b = 0; b < box.size(); b++)
		middle[b] = middle[b] * (1.0f / (float)(from[b + 1] - from[b]));
	// A lot `d` away counts (1 - d^2 / r^2)^2, r two boxes: over a flat
	// skin that adds up to a third of the circle's area.
	const double r = 2 * h;
	const float r2 = (float)(r * r);
	const double per = 3.0 / (3.14159265358979 * r * r);
	std::vector<uint32_t> near;
	for (size_t b = 0; b < box.size(); b++) {
		const int64_t x = (int64_t)(box[b] >> 42), y = (int64_t)(box[b] >> 21 & 0x1FFFFF),
			      z = (int64_t)(box[b] & 0x1FFFFF);
		// The boxes within two of it that have roots (a row of them along
		// z lies together in the list).
		near.clear();
		for (int64_t X = std::max<int64_t>(0, x - 2); X <= std::min(LAST, x + 2); X++)
			for (int64_t Y = std::max<int64_t>(0, y - 2); Y <= std::min(LAST, y + 2); Y++) {
				const uint64_t row = (uint64_t)X << 42 | (uint64_t)Y << 21;
				const uint64_t first = row | (uint64_t)std::max<int64_t>(0, z - 2),
					       last = row | (uint64_t)std::min(LAST, z + 2);
				for (auto it = std::lower_bound(box.begin(), box.end(), first);
				     it != box.end() && *it <= last; ++it)
					near.push_back((uint32_t)(it - box.begin()));
			}
		for (uint32_t i = from[b]; i < from[b + 1]; i++) {
			const V3 p = at[coded[i].second];
			double sum = 0;
			for (uint32_t c : near) {
				const V3 d = middle[c] - p;
				const float t = 1 - dot(d, d) / r2;
				if (t > 0)
					sum += (double)lot[c] * (double)(t * t);
			}
			cover[coded[i].second] = (float)(sum * per);
		}
	}
	return cover;
}

// Which of a part's strands are kept and drawn first when not all can be
// (Strands::rate, key): hair that covers its skin up to `cover` times
// over at the full rate, deeper hair at as much less as it's deeper, down
// to 1 / `uneven`; the strands put in the order of their keys.
void rate_strands(RawStrands &raw, float cover, float uneven)
{
	const size_t n = raw.root.size();
	if (n < 2 || !(uneven > 1) || !(cover > 0))
		return;
	// Each strand: where its root is, and its own area - its length x its
	// width (its root's, middle's and tip's: 1, 4 and 1 parts).
	std::vector<V3> at(n);
	std::vector<float> area(n);
	std::vector<uint32_t> chain;
	double v[4];
	auto width_at = [&](uint32_t point) {
		column_point(raw.width, point, v);
		return std::max(0.0f, (float)v[0]);
	};
	for (size_t i = 0; i < n; i++) {
		chain.clear();
		float run = 0;
		for (uint32_t a = raw.root[i]; a != NO_POINT && chain.size() <= raw.pos.size(); a = raw.next[a]) {
			if (!chain.empty())
				run += length(raw.pos[a] - raw.pos[chain.back()]);
			chain.push_back(a);
		}
		float w = 0.0001f;
		if (raw.has_width)
			w = (width_at(chain.front()) + 4 * width_at(chain[chain.size() / 2]) + width_at(chain.back())) /
			    6;
		at[i] = raw.pos[raw.root[i]];
		area[i] = run * w;
	}
	const std::vector<float> covered = cover_at(at, area);
	const float least = 1 / std::min(4.0f, uneven);
	std::vector<float> rate(n);
	float top = least;
	for (size_t i = 0; i < n; i++) {
		rate[i] = covered[i] > cover ? std::max(least, cover / covered[i]) : 1.0f;
		top = std::max(top, rate[i]);
	}
	// (The highest: 1 - a coat deep all over is one the same all over.)
	raw.top = top;
	std::vector<double> key(n);
	for (size_t i = 0; i < n; i++) {
		rate[i] = Strands::rate_of(Strands::rate_step(rate[i] / top));
		key[i] = (double)i / (double)n / (double)rate[i];
	}
	for (RawStrands::Sample &sm : raw.sample)
		sm.key = key[sm.rank];
	std::vector<uint32_t> order(n);
	for (size_t i = 0; i < n; i++)
		order[i] = (uint32_t)i;
	std::stable_sort(order.begin(), order.end(), [&key](uint32_t a, uint32_t b) { return key[a] < key[b]; });
	std::vector<uint32_t> root(n);
	raw.rate.resize(n);
	for (size_t i = 0; i < n; i++) {
		root[i] = raw.root[order[i]];
		raw.rate[i] = rate[order[i]];
	}
	raw.root.swap(root);
	raw.rank.swap(order);
}

// The order a part's pieces are drawn in (Strands::groups, layers,
// pieces, runs): its strands grouped by the bone that holds each one's
// root most (the smallest groups joined to the one nearest, past
// MOST_STRAND_GROUPS), each group's pieces in `layers` layers of as many
// pieces each by how far from their roots they start, and a layer's by
// strand - in two sorts where it has a `thin_width`, its wide pieces
// first (Strands::thin).
void arrange_strands(Primitive &p, int layers)
{
	Strands &h = p.hair;
	const size_t strands = h.strands();
	h.groups.clear();
	h.pieces.clear();
	h.runs.clear();
	h.thin.clear();
	h.thin_share = 0;
	h.layers = std::max(1, std::min(16, layers));
	h.core = 0;
	if (!strands)
		return;
	const bool boned = p.joints.size() == p.pos.size() * 4 && p.weights.size() == p.pos.size() * 4;
	std::vector<uint32_t> group_of(strands, 0);
	struct Sum {
		int joint = 0;
		size_t count = 0;
		V3 middle;
	};
	std::vector<Sum> sums;
	std::map<int, uint32_t> by_joint;
	for (size_t s = 0; s < strands; s++) {
		int joint = 0;
		const size_t r = h.first[s];
		if (boned) {
			int best = 0;
			for (int k = 1; k < 4; k++)
				if (p.weights[r * 4 + k] > p.weights[r * 4 + best])
					best = k;
			joint = p.joints[r * 4 + best];
		}
		auto it = by_joint.find(joint);
		if (it == by_joint.end()) {
			it = by_joint.emplace(joint, (uint32_t)sums.size()).first;
			sums.push_back({joint, 0, V3{}});
		}
		group_of[s] = it->second;
		Sum &g = sums[it->second];
		g.count++;
		g.middle = g.middle + p.pos[(h.first[s] + h.first[s + 1] - 1) / 2];
	}
	// Too many: the smallest joined to the one whose middle is nearest.
	std::vector<uint32_t> into(sums.size());
	for (size_t i = 0; i < sums.size(); i++)
		into[i] = (uint32_t)i;
	size_t alive = sums.size();
	while (alive > MOST_STRAND_GROUPS) {
		size_t small = sums.size();
		for (size_t i = 0; i < sums.size(); i++)
			if (sums[i].count && (small == sums.size() || sums[i].count < sums[small].count))
				small = i;
		const V3 at = sums[small].middle * (1.0f / (float)sums[small].count);
		size_t near = sums.size();
		float best = 0;
		for (size_t i = 0; i < sums.size(); i++) {
			if (i == small || !sums[i].count)
				continue;
			const float d = length(sums[i].middle * (1.0f / (float)sums[i].count) - at);
			if (near == sums.size() || d < best) {
				near = i;
				best = d;
			}
		}
		// (The bigger one's bone stays the group's.)
		sums[near].count += sums[small].count;
		sums[near].middle = sums[near].middle + sums[small].middle;
		sums[small].count = 0;
		for (uint32_t &t : into)
			if (t == small)
				t = (uint32_t)near;
		alive--;
	}
	std::vector<uint32_t> number(sums.size(), 0);
	for (size_t i = 0; i < sums.size(); i++) {
		if (!sums[i].count)
			continue;
		number[i] = (uint32_t)h.groups.size();
		Strands::Group g;
		g.joint = sums[i].joint;
		g.middle = sums[i].middle * (1.0f / (float)sums[i].count);
		h.groups.push_back(g);
	}
	for (uint32_t &g : group_of)
		g = number[into[g]];
	// A point near each group's middle (posed on the CPU: where it is) -
	// of its first strands where it has any there, as only the first of a
	// part's strands may be posed.
	std::vector<float> nearest(h.groups.size(), 1e30f);
	const size_t early = std::max<size_t>(64, strands / 16);
	for (size_t s = 0; s < strands; s++) {
		const uint32_t mid = (h.first[s] + h.first[s + 1] - 1) / 2;
		Strands::Group &g = h.groups[group_of[s]];
		if (s >= early && nearest[group_of[s]] < 1e30f)
			continue;
		const float d = length(p.pos[mid] - g.middle);
		if (d < nearest[group_of[s]]) {
			nearest[group_of[s]] = d;
			g.sample = mid;
		}
	}
	// Each piece: how far along its strand it starts (the strand's own
	// length, in its units).
	const size_t groups = h.groups.size(), L = (size_t)h.layers;
	std::vector<std::vector<float>> starts(groups);
	std::vector<float> from(p.pos.size(), 0);
	for (size_t s = 0; s < strands; s++) {
		float run = 0;
		for (uint32_t i = h.first[s]; i + 1 < h.first[s + 1]; i++) {
			from[i] = run;
			starts[group_of[s]].push_back(run);
			run += length(p.pos[i + 1] - p.pos[i]);
		}
	}
	// A group's layers: as many pieces in each.
	std::vector<std::vector<float>> cut(groups);
	for (size_t g = 0; g < groups; g++) {
		std::vector<float> &v = starts[g];
		std::sort(v.begin(), v.end());
		for (size_t l = 1; l < L; l++)
			cut[g].push_back(v.empty() ? 0.0f : v[std::min(v.size() - 1, v.size() * l / L)]);
		std::vector<float>().swap(v);
	}
	// The inner layers (the hull): 3 of 5.
	h.core = L >= 2 ? (int)std::min(L - 1, (L * 3 + 2) / 5) : 0;
	auto layer_of = [&](uint32_t g, float start) {
		size_t l = 0;
		while (l + 1 < L && start > cut[g][l])
			l++;
		// (Pieces that start at the same distance go in the same layer,
		// the one under the first cut they don't pass: every hair's root
		// piece is in the first.)
		return l;
	};
	// How far the inner layers reach: where the outer ones start - or, of
	// hairs of a piece or two (whose pieces start at their roots or near),
	// where the inner layers' pieces end, on average.
	std::vector<double> ends(groups, 0.0);
	std::vector<size_t> inner(groups, 0);
	std::vector<uint32_t> count(groups * L + 1, 0);
	// (In two sorts: how many of each run's pieces are wide.)
	const bool sorts = h.thin_width > 0;
	std::vector<uint32_t> wide(sorts ? groups * L : 0, 0);
	size_t wide_in_all = 0;
	for (size_t s = 0; s < strands; s++)
		for (uint32_t i = h.first[s]; i + 1 < h.first[s + 1]; i++) {
			const uint32_t g = group_of[s];
			const size_t l = layer_of(g, from[i]);
			count[g * L + l + 1]++;
			if (sorts && h.piece_width(s, i) > (double)h.thin_width) {
				wide[g * L + l]++;
				wide_in_all++;
			}
			if (l < (size_t)h.core) {
				ends[g] += (double)from[i] + (double)length(p.pos[i + 1] - p.pos[i]);
				inner[g]++;
			}
		}
	for (size_t g = 0; g < groups; g++)
		h.groups[g].reach = h.core ? std::max(cut[g][(size_t)h.core - 1],
						      inner[g] ? (float)(ends[g] / (double)inner[g]) : 0.0f)
					   : 0.0f;
	for (size_t r = 0; r < groups * L; r++)
		count[r + 1] += count[r];
	h.runs = count;
	h.pieces.resize(count[groups * L]);
	std::vector<uint32_t> fill(count.begin(), count.end() - 1);
	// Two sorts only with enough thin pieces, and some wide.
	const size_t in_all = h.pieces.size();
	if (!sorts || !wide_in_all || (double)(in_all - wide_in_all) < (double)Strands::THIN_MOST * (double)in_all) {
		h.thin_width = h.thin_pixel = h.thin_pixel_most = 0;
		std::fill(h.thin_pixel_at, h.thin_pixel_at + Strands::LINE_LOSSES, 0.0f);
		for (size_t s = 0; s < strands; s++)
			for (uint32_t i = h.first[s]; i + 1 < h.first[s + 1]; i++)
				h.pieces[fill[group_of[s] * L + layer_of(group_of[s], from[i])]++] = i;
		return;
	}
	h.thin_share = (float)((double)(in_all - wide_in_all) / (double)in_all);
	h.thin.resize(groups * L);
	for (size_t r = 0; r < groups * L; r++)
		h.thin[r] = count[r] + wide[r];
	std::vector<uint32_t> fill_thin = h.thin;
	for (size_t s = 0; s < strands; s++)
		for (uint32_t i = h.first[s]; i + 1 < h.first[s + 1]; i++) {
			const size_t r = group_of[s] * L + layer_of(group_of[s], from[i]);
			h.pieces[(h.piece_width(s, i) > (double)h.thin_width ? fill : fill_thin)[r]++] = i;
		}
}

// A strand part's kept strands made ready (Primitive::strands): the
// first of them in draw order, as far as its allowed points go.
Primitive finish_strands(const RawStrands &raw, int layers, std::vector<uint32_t> &which)
{
	Primitive p;
	p.strands = true;
	p.material = raw.material;
	Strands &h = p.hair;
	const size_t in_file = raw.root.size();
	h.in_file = (uint32_t)in_file;
	h.first.push_back(0);
	which.clear();
	StrandShape s;
	auto rate_of = [&raw](size_t i) {
		return raw.rate.empty() ? 1.0f : raw.rate[i];
	};
	auto key_of = [&](size_t i) {
		const size_t rank = raw.rank.empty() ? i : raw.rank[i];
		return (float)((double)rank / (double)in_file / (double)rate_of(i));
	};
	float lowest = 1;
	for (size_t at = 0; at < in_file; at++) {
		const uint32_t root = raw.root[at];
		shape_strand(raw, root, s);
		if (s.kept < 2 || p.pos.size() + s.kept > raw.allowed)
			break;
		h.key.push_back(key_of(at));
		h.rate.push_back(rate_of(at));
		lowest = std::min(lowest, h.rate.back());
		const float total = s.arc.back();
		const size_t from = p.pos.size();
		for (size_t i = 0; i < s.at.size(); i++) {
			if (!s.keep[i])
				continue;
			which.push_back(s.at[i]);
			p.pos.push_back(raw.pos[s.at[i]]);
			if (raw.has_bones) {
				p.joints.insert(p.joints.end(), &s.joints[i * 4], &s.joints[i * 4] + 4);
				p.weights.insert(p.weights.end(), &s.weights[i * 4], &s.weights[i * 4] + 4);
			}
			h.width.push_back(s.width[i]);
			h.along.push_back(total > 0 ? s.arc[i] / total : 0.0f);
			if (raw.has_colour)
				h.colour.insert(h.colour.end(), &s.colour[i * 4], &s.colour[i * 4] + 4);
		}
		// The way it runs at each kept point: from the one before to the
		// one after.
		const size_t to = p.pos.size();
		for (size_t i = from; i < to; i++) {
			const V3 a = p.pos[i > from ? i - 1 : i], b = p.pos[i + 1 < to ? i + 1 : i];
			p.normal.push_back(normalized(b - a, {0, 1, 0}));
		}
		h.first.push_back((uint32_t)to);
	}
	const size_t kept = h.strands();
	h.full = kept < in_file ? key_of(kept) : 1 / lowest;
	// Its hairs' mean width along their lengths (whichever of their points
	// are kept), as drawn at `full`.
	// (And as lines: how wide a pixel has to be for its hairs to lose a
	// hundredth, and a twentieth, of what they cover - of every so-manyth
	// piece, so a big coat's aren't all gone through forty times.)
	double width_sum = 0, length_sum = 0;
	const size_t pieces = p.pos.size() > kept ? p.pos.size() - kept : 0,
		     every = std::max<size_t>(1, pieces / 200000);
	std::vector<std::pair<float, float>> some; // (how wide, how long)
	some.reserve(pieces / every + 1);
	size_t at = 0;
	double widest = 0;
	for (size_t k = 0; k < kept; k++) {
		for (uint32_t i = h.first[k]; i + 1 < h.first[k + 1]; i++, at++) {
			const double piece = length(p.pos[i + 1] - p.pos[i]);
			const double wide = h.piece_width(k, i);
			width_sum += wide * piece;
			length_sum += piece;
			if (at % every == 0) {
				some.push_back({(float)wide, (float)piece});
				widest = std::max(widest, wide);
			}
		}
	}
	h.mean_width = length_sum > 0 ? (float)(width_sum / length_sum) : 0.0f;
	// (Of the first `count` of them.)
	auto lost_at = [&some](double pixel, size_t count) {
		double lost = 0, all = 0;
		for (size_t k = 0; k < count; k++) {
			const std::pair<float, float> &one = some[k];
			all += (double)one.first * (double)one.second;
			lost += std::max(0.0, (double)one.first - (double)Strands::LINE_COVERS * pixel) *
				(double)one.second;
		}
		return all > 0 ? lost / all : 0.0;
	};
	auto pixel_losing = [&](double share, size_t count, double widest_of) {
		double under = 0, over = widest_of / (double)Strands::LINE_COVERS; // (at `over`, nothing's lost)
		for (int step = 0; step < 40; step++) {
			const double middle = 0.5 * (under + over);
			(lost_at(middle, count) > share ? under : over) = middle;
		}
		return (float)over;
	};
	h.line_pixel = pixel_losing(0.01, some.size(), widest);
	h.line_pixel_most = pixel_losing(0.05, some.size(), widest);
	for (int k = 0; k < Strands::LINE_LOSSES; k++)
		h.line_pixel_at[k] = pixel_losing(Strands::LINE_LOSS[k], some.size(), widest);
	// In two sorts (Strands::thin): the thin pieces, no wider than
	// THIN_WIDTH times the width half its hairs' length is under - where
	// that's most of its pieces and not all (arrange_strands, which counts
	// every one, has the last word).
	h.thin_width = h.thin_pixel = h.thin_pixel_most = 0;
	std::fill(h.thin_pixel_at, h.thin_pixel_at + Strands::LINE_LOSSES, 0.0f);
	if (!some.empty() && length_sum > 0) {
		std::sort(some.begin(), some.end());
		double sampled = 0, half = 0;
		for (const std::pair<float, float> &one : some)
			sampled += one.second;
		size_t middle = 0;
		while (middle + 1 < some.size() && half + (double)some[middle].second < 0.5 * sampled)
			half += some[middle++].second;
		const float limit = Strands::THIN_WIDTH * some[middle].first;
		const size_t thin = (size_t)(std::upper_bound(some.begin(), some.end(), limit,
							       [](float w, const std::pair<float, float> &one) {
								       return w < one.first;
							       }) -
					     some.begin());
		if (thin < some.size() && (double)thin >= (double)Strands::THIN_MOST * (double)some.size() && limit > 0) {
			h.thin_width = limit;
			h.thin_pixel = pixel_losing(0.01, thin, some[thin - 1].first);
			h.thin_pixel_most = pixel_losing(0.05, thin, some[thin - 1].first);
			for (int k = 0; k < Strands::LINE_LOSSES; k++)
				h.thin_pixel_at[k] = pixel_losing(Strands::LINE_LOSS[k], thin, some[thin - 1].first);
		}
	}
	p.joints.shrink_to_fit();
	p.weights.shrink_to_fit();
	p.pos.shrink_to_fit();
	p.normal.shrink_to_fit();
	h.first.shrink_to_fit();
	h.key.shrink_to_fit();
	h.rate.shrink_to_fit();
	h.width.shrink_to_fit();
	h.along.shrink_to_fit();
	h.colour.shrink_to_fit();
	arrange_strands(p, layers);
	return p;
}

// A strand part's expressions (Strands::anchor, moves): every point an
// expression moves follows one of up to MOST_ANCHORS spots. A strand
// that moves all of a piece (hair riding the skin at its root) keeps to
// one spot - found by where its root is - and any other point by where
// it is; the spots are made by halving the box round the moved points,
// the half that moves most and reaches furthest first, so hair that
// moves a lot gets the most spots. Each spot's move is the mean of its
// points'. Returns how far the worst point is left from where the file
// would move it.
float anchor_strands(Reader &rd, const RawStrands &raw, const std::vector<uint32_t> &which, Primitive &p)
{
	Strands &h = p.hair;
	const size_t n = p.pos.size(), strands = h.strands(), morphs = raw.targets.size();
	// (Read already for its strands' shapes - read_strand_moves: those.)
	std::vector<Reader::Column> read(raw.moves.empty() ? morphs : 0);
	const std::vector<Reader::Column> &cols = raw.moves.empty() ? read : raw.moves;
	bool any = false;
	for (size_t t = 0; t < morphs; t++) {
		if (raw.targets[t] < 0)
			continue;
		if (raw.moves.empty())
			read[t] = rd.column(raw.targets[t], 3, MOST_STRAND_POINTS);
		if (cols[t].count != raw.pos.size())
			fail("the file is damaged (an expression doesn't fit its part)");
		any = any || cols[t].data || !cols[t].sparse_at.empty();
	}
	for (const RawStrands::StrandMoves &sm : raw.strand_moves)
		any = any || !sm.strands.empty();
	if (!any || !n)
		return 0;
	std::vector<uint32_t> new_of;
	// Moved strand by strand: which of the file's strands each kept point
	// is of (none: NO_POINT), and where in an expression's list each of
	// those strands is.
	std::vector<uint32_t> strand_of, listed;
	if (!raw.strand_start.empty()) {
		const std::vector<uint32_t> &start = raw.strand_start;
		strand_of.assign(n, NO_POINT);
		for (size_t i = 0; i < n; i++) {
			const auto past = std::upper_bound(start.begin(), start.end(), which[i]);
			if (past != start.begin() && past != start.end())
				strand_of[i] = (uint32_t)(past - start.begin()) - 1;
		}
		listed.assign(start.size() - 1, NO_POINT);
	}
	// One expression's moves of the kept points (those it doesn't move: none).
	std::vector<V3> d(n);
	auto moves_of = [&](size_t t) {
		const Reader::Column &c = cols[t];
		std::fill(d.begin(), d.end(), V3{});
		bool moved = false;
		auto put = [&](size_t i, const double *v) {
			const V3 m{(float)v[0], (float)v[1], (float)v[2]};
			if (dot(m, m) > MOVES_NOTHING) {
				d[i] = m;
				moved = true;
			}
		};
		double v[4];
		if (c.comps != 3) {
			// (none the usual way)
		} else if (c.data) {
			for (size_t i = 0; i < n; i++) {
				column_point(c, which[i], v);
				put(i, v);
			}
		} else if (!c.sparse_at.empty()) {
			if (new_of.empty()) {
				new_of.assign(raw.pos.size(), NO_POINT);
				for (size_t i = 0; i < n; i++)
					new_of[which[i]] = (uint32_t)i;
			}
			for (size_t k = 0; k < c.sparse_at.size(); k++)
				if (new_of[c.sparse_at[k]] != NO_POINT)
					put(new_of[c.sparse_at[k]], &c.sparse_values[k * 3]);
		}
		if (t < raw.strand_moves.size() && !raw.strand_moves[t].strands.empty() && !strand_of.empty()) {
			const RawStrands::StrandMoves &sm = raw.strand_moves[t];
			for (size_t k = 0; k < sm.strands.size(); k++)
				listed[sm.strands[k]] = (uint32_t)k;
			for (size_t i = 0; i < n; i++) {
				if (strand_of[i] == NO_POINT || listed[strand_of[i]] == NO_POINT)
					continue;
				const V3 m = d[i] + sm.moves[listed[strand_of[i]]];
				d[i] = dot(m, m) > MOVES_NOTHING ? m : V3{};
				moved = moved || dot(m, m) > MOVES_NOTHING;
			}
			for (uint32_t k : sm.strands)
				listed[k] = NO_POINT;
		}
		return moved;
	};
	// How far each point is ever moved, and how far from its root's move.
	std::vector<float> reach(n, 0), bent(strands, 0);
	std::vector<char> used(morphs, 0);
	for (size_t t = 0; t < morphs; t++) {
		if (!moves_of(t))
			continue;
		used[t] = 1;
		for (size_t s = 0; s < strands; s++) {
			const V3 root = d[h.first[s]];
			for (uint32_t i = h.first[s]; i < h.first[s + 1]; i++) {
				reach[i] = std::max(reach[i], length(d[i]));
				bent[s] = std::max(bent[s], length(d[i] - root));
			}
		}
	}
	// The moved points, each with the place its spot is found by.
	std::vector<uint32_t> items;
	std::vector<V3> key(n);
	for (size_t s = 0; s < strands; s++) {
		float most = 0;
		for (uint32_t i = h.first[s]; i < h.first[s + 1]; i++)
			most = std::max(most, reach[i]);
		if (!(most > 0))
			continue;
		const bool whole = bent[s] <= 0.05f * most + 1e-7f;
		for (uint32_t i = h.first[s]; i < h.first[s + 1]; i++) {
			if (whole)
				reach[i] = most;
			if (!(reach[i] > 0))
				continue;
			key[i] = whole ? p.pos[h.first[s]] : p.pos[i];
			items.push_back(i);
		}
	}
	h.moves.assign(morphs, {});
	if (items.empty())
		return 0;
	struct Cell {
		uint32_t from = 0, to = 0;
		V3 lo, hi;
		float reach = 0;
	};
	auto measure_cell = [&](Cell &c) {
		c.lo = {1e30f, 1e30f, 1e30f};
		c.hi = {-1e30f, -1e30f, -1e30f};
		c.reach = 0;
		for (uint32_t k = c.from; k < c.to; k++) {
			const V3 v = key[items[k]];
			c.lo = {std::min(c.lo.x, v.x), std::min(c.lo.y, v.y), std::min(c.lo.z, v.z)};
			c.hi = {std::max(c.hi.x, v.x), std::max(c.hi.y, v.y), std::max(c.hi.z, v.z)};
			c.reach = std::max(c.reach, reach[items[k]]);
		}
	};
	auto cost_of = [](const Cell &c) {
		return length(c.hi - c.lo) * c.reach;
	};
	std::vector<Cell> cells(1);
	cells[0].from = 0;
	cells[0].to = (uint32_t)items.size();
	measure_cell(cells[0]);
	std::vector<std::pair<float, uint32_t>> heap;
	heap.push_back({cost_of(cells[0]), 0u});
	while (cells.size() < Strands::MOST_ANCHORS && !heap.empty()) {
		std::pop_heap(heap.begin(), heap.end());
		const uint32_t ci = heap.back().second;
		const float cost = heap.back().first;
		heap.pop_back();
		if (!(cost > 0))
			break;
		Cell c = cells[ci];
		const V3 size = c.hi - c.lo;
		const int axis = size.x >= size.y && size.x >= size.z ? 0 : size.y >= size.z ? 1 : 2;
		auto on = [axis](V3 v) {
			return axis == 0 ? v.x : axis == 1 ? v.y : v.z;
		};
		const float mid = (on(c.lo) + on(c.hi)) * 0.5f;
		const auto split = std::partition(items.begin() + c.from, items.begin() + c.to,
						  [&](uint32_t i) { return on(key[i]) < mid; });
		const uint32_t at = (uint32_t)(split - items.begin());
		if (at == c.from || at == c.to)
			continue; // (too small for a float to halve: left whole)
		Cell a = c, b = c;
		a.to = b.from = at;
		measure_cell(a);
		measure_cell(b);
		cells[ci] = a;
		cells.push_back(b);
		heap.push_back({cost_of(a), ci});
		std::push_heap(heap.begin(), heap.end());
		heap.push_back({cost_of(b), (uint32_t)cells.size() - 1});
		std::push_heap(heap.begin(), heap.end());
	}
	h.anchor.assign(n, 0);
	for (size_t ci = 0; ci < cells.size(); ci++)
		for (uint32_t k = cells[ci].from; k < cells[ci].to; k++)
			h.anchor[items[k]] = (uint16_t)(ci + 1);
	std::vector<uint32_t> count(cells.size() + 1, 0);
	for (uint32_t i : items)
		count[h.anchor[i]]++;
	float worst = 0;
	for (size_t t = 0; t < morphs; t++) {
		h.moves[t].assign(cells.size() + 1, V3{});
		if (!used[t] || !moves_of(t))
			continue;
		std::vector<V3> &mv = h.moves[t];
		for (uint32_t i : items)
			mv[h.anchor[i]] = mv[h.anchor[i]] + d[i];
		for (size_t a = 1; a < mv.size(); a++)
			if (count[a])
				mv[a] = mv[a] * (1.0f / (float)count[a]);
		for (uint32_t i : items)
			worst = std::max(worst, length(d[i] - mv[h.anchor[i]]));
	}
	return worst;
}

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
	// 0.9.0: glowing gas (fire, sparks, smoke), as DireBot's companions have
	// it - only for a see-through part.
	const json &glow = field(field(field(m, "extras"), "direbot"), "glow");
	if (glow.is_object() && mat.alpha == AlphaMode::Blend) {
		mat.glow = true;
		mat.glow_soft = std::clamp(num(glow, "soft", 1.5f), 0.0f, 16.0f);
		mat.glow_ragged = std::clamp(num(glow, "ragged", 0.0f), 0.0f, 1.0f);
	}
	// Stand-in hair, worn while its real strands aren't drawn.
	mat.hair_standin = flag(field(field(m, "extras"), "direbot"), "hair_standin", false);
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
		// (A switch: true, or a number that isn't 0 - Blender writes a
		// ticked box either way.)
		const json &sw = field(field(field(n, "extras"), "direbot"), "switch");
		node.size_switch = sw.is_boolean() ? sw.get<bool>() : sw.is_number() && sw.get<double>() != 0;
		const json &sh = field(field(field(n, "extras"), "direbot"), "shapes");
		node.plays_shapes = sh.is_boolean() ? sh.get<bool>() : sh.is_number() && sh.get<double>() != 0;
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

// The most bones following others it reads (a character has a few dozen
// at most), so a hostile file can't slow every frame.
constexpr size_t MOST_CONSTRAINTS = 1024;

// Its bones that follow others (VRMC_node_constraint 1.0, or its beta),
// in the order they're worked out: each after those it waits on. A twist
// bone or a copied turn waits on its source's own turn; one aiming at its
// source waits on everything above its source and above itself (where
// they are). One following itself, caught in a loop or waiting on one, of
// a version or kind not known, or pointing at no bone, is left out.
void read_constraints(const json &doc, Model &m)
{
	const size_t count = m.nodes.size();
	const json &nodes = list(doc, "nodes");
	std::vector<Constraint> found;
	std::vector<int> on(count, -1); // a node's constraint, if it has one
	int left_out = 0;
	for (size_t i = 0; i < nodes.size() && i < count; i++) {
		const json &ext = field(field(nodes[i], "extensions"), "VRMC_node_constraint");
		if (!ext.is_object())
			continue;
		const std::string version = text(ext, "specVersion");
		const json &c = field(ext, "constraint");
		const json &roll = field(c, "roll"), &aim = field(c, "aim"), &rotation = field(c, "rotation");
		Constraint k;
		k.node = (int)i;
		bool ok = version == "1.0" || version == "1.0-beta";
		const json *def = &rotation;
		if (roll.is_object()) {
			// (Its axis is needed; without one, X - as three-vrm has it.)
			k.kind = Constraint::Roll;
			def = &roll;
			const std::string a = text(roll, "rollAxis");
			if (a == "Y")
				k.axis = {0, 1, 0};
			else if (a == "Z")
				k.axis = {0, 0, 1};
			else
				ok = ok && (a == "X" || a.empty());
		} else if (aim.is_object()) {
			k.kind = Constraint::Aim;
			def = &aim;
			const std::string a = text(aim, "aimAxis");
			const char *names[6] = {"PositiveX", "NegativeX", "PositiveY", "NegativeY", "PositiveZ", "NegativeZ"};
			const V3 axes[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
			bool known = a.empty();
			for (int j = 0; j < 6; j++)
				if (a == names[j]) {
					k.axis = axes[j];
					known = true;
				}
			ok = ok && known;
		} else if (rotation.is_object()) {
			k.kind = Constraint::Rotation;
		} else {
			ok = false;
		}
		k.source = index_of(*def, "source");
		k.weight = std::clamp(num(*def, "weight", 1), 0.0f, 1.0f);
		if (!ok || k.source < 0 || k.source >= (int)count || k.source == (int)i ||
		    found.size() >= MOST_CONSTRAINTS) {
			left_out++;
			continue;
		}
		on[i] = (int)found.size();
		found.push_back(k);
	}
	if (found.empty()) {
		m.constraints_left_out = left_out;
		return;
	}
	// The nearest node above each that has one (-1: none), parents first.
	std::vector<int> above(count, -1), queue;
	for (size_t i = 0; i < count; i++)
		if (m.nodes[i].parent < 0)
			queue.push_back((int)i);
	for (size_t q = 0; q < queue.size(); q++) {
		const int p = queue[q];
		for (int c : m.nodes[p].children) {
			above[c] = on[p] >= 0 ? p : above[p];
			queue.push_back(c);
		}
	}
	// What each waits on.
	std::vector<std::vector<int>> waits(found.size());
	for (size_t k = 0; k < found.size(); k++) {
		const Constraint &c = found[k];
		auto with = [&](int node) {
			for (int n = node; n >= 0; n = above[n])
				if (on[n] >= 0)
					waits[k].push_back(on[n]);
		};
		if (c.kind == Constraint::Aim) {
			with(c.source);
			with(m.nodes[c.node].parent);
		} else if (on[c.source] >= 0) {
			waits[k].push_back(on[c.source]);
		}
	}
	// Worked out after what they wait on (a walk without recursion, so a
	// long line of them can't run out of stack). 0 not seen, 1 being
	// looked at, 2 fine, 3 left out.
	std::vector<char> state(found.size(), 0), looped(found.size(), 0);
	std::vector<int> order;
	for (size_t start = 0; start < found.size(); start++) {
		if (state[start])
			continue;
		std::vector<std::pair<int, size_t>> stack{{(int)start, 0}};
		state[start] = 1;
		while (!stack.empty()) {
			const int k = stack.back().first;
			if (stack.back().second < waits[k].size()) {
				const int w = waits[k][stack.back().second++];
				if (state[w] == 0) {
					state[w] = 1;
					stack.push_back({w, 0});
				} else if (state[w] == 1) {
					looped[k] = 1;
				}
				continue;
			}
			bool fine = !looped[k];
			for (int w : waits[k])
				fine = fine && state[w] == 2;
			state[k] = fine ? 2 : 3;
			if (fine)
				order.push_back(k);
			else
				left_out++;
			stack.pop_back();
		}
	}
	for (int k : order)
		m.constraints.push_back(found[k]);
	m.constraints_left_out = left_out;
}

// An expression's moves kept only where it moves a point (by more than
// 1e-7, or turns its facing by more than 1e-5: less is nothing to see),
// from the file's moves of all n points (dp or dn empty: none).
MorphTarget only_moved(const std::vector<V3> &dp, const std::vector<V3> &dn, size_t n)
{
	MorphTarget mt;
	for (size_t v = 0; v < n; v++) {
		const V3 p = dp.empty() ? V3{} : dp[v];
		const V3 f = dn.empty() ? V3{} : dn[v];
		if (dot(p, p) <= MOVES_NOTHING && dot(f, f) <= TURNS_NOTHING)
			continue;
		mt.at.push_back((uint32_t)v);
		mt.pos.push_back(p);
		if (!dn.empty())
			mt.normal.push_back(f);
	}
	mt.at.shrink_to_fit();
	mt.pos.shrink_to_fit();
	mt.normal.shrink_to_fit();
	return mt;
}

void read_meshes(Reader &rd, const json &doc, Model &m, const LoadOptions &options)
{
	size_t points = 0, indices = 0, strand_points = 0;
	// Its strand parts, finished once every part is read (how many of
	// each one's strands are kept goes by them all).
	struct Pending {
		size_t mesh = 0, part = 0;
		RawStrands raw;
	};
	std::vector<Pending> pending;
	V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
	auto reach = [&lo, &hi](const std::vector<V3> &pos) {
		for (const V3 &v : pos) {
			lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
			hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
		}
	};
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
			// Lines and line strips: hair strands (Primitive::strands).
			const bool strands = mode == 1 || mode == 3;
			if (!strands && (mode < 4 || mode > 6))
				continue; // points, line loops
			const json &attr = field(pj, "attributes");
			const int pos_a = index_of(attr, "POSITION");
			if (pos_a < 0)
				continue;
			const json &pnames = field(field(pj, "extras"), "targetNames");
			const json &use = names.is_array() ? names : pnames;
			if (use.is_array())
				for (size_t t = 0; t < use.size() && t < morphs; t++)
					if (use[t].is_string() && mesh.morph_names[t].empty())
						mesh.morph_names[t] = use[t].get<std::string>();
			Primitive p;
			p.material = index_of(pj, "material");
			if (p.material >= (int)m.materials.size())
				p.material = -1;
			if (strands) {
				Pending pd;
				pd.mesh = m.meshes.size();
				pd.part = mesh.parts.size();
				pd.raw = open_strands(rd, pj, attr, mode, morphs, strand_points);
				pd.raw.material = p.material;
				reach(pd.raw.pos);
				pending.push_back(std::move(pd));
				p.strands = true;
				mesh.parts.push_back(std::move(p));
				continue;
			}
			p.pos = rd.vec3s(pos_a);
			const size_t n = p.pos.size();
			points += n;
			if (points > MOST_POINTS)
				fail("the file is too big (over 4 million points)");
			reach(p.pos);
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
				std::vector<V3> dp, dn;
				if (index_of(targets[t], "POSITION") >= 0)
					dp = rd.vec3s(index_of(targets[t], "POSITION"));
				if (index_of(targets[t], "NORMAL") >= 0)
					dn = rd.vec3s(index_of(targets[t], "NORMAL"));
				if ((!dp.empty() && dp.size() != n) || (!dn.empty() && dn.size() != n))
					fail("the file is damaged (an expression doesn't fit its part)");
				p.morphs[t] = only_moved(dp, dn, n);
				// Counted by what's kept: the points it moves, not every
				// point.
				points += (p.morphs[t].pos.size() + p.morphs[t].normal.size()) / 4;
			}
			if (points > MOST_POINTS * 2)
				fail("the file is too big");
			mesh.parts.push_back(std::move(p));
		}
		float w[MOST_MORPHS];
		std::fill(w, w + morphs, 0.0f);
		numbers(mj, "weights", w, (int)morphs);
		mesh.weights.assign(w, w + morphs);
		m.meshes.push_back(std::move(mesh));
	}
	if (pending.empty())
		return;
	// Its hair strands: as many of each part's as the points allowed go
	// round (as near as a sample of its strands can tell) - the strands
	// under one threshold in every part (Strands::key: where its hair is
	// thin first), or with every strand worth the same
	// (LoadOptions::strand_uneven 1) each part the same share of its own.
	// None wanted: a character with stand-in hair to wear instead keeps
	// none, one with none keeps a few.
	size_t allowed = options.strand_points;
	if (!allowed && !m.hair_standins)
		allowed = options.strand_points_bare;
	const float size = lo.x <= hi.x ? length(hi - lo) : 0.0f;
	double predicted = 0;
	for (Pending &pd : pending) {
		pd.raw.bend = std::max(0.0f, options.strand_bend) * size;
		pd.raw.swing = std::max(0.0f, options.strand_swing) * size;
		pd.raw.turn = options.strand_turn > 0 ? std::cos(std::min(180.0f, options.strand_turn) * 0.01745329252f)
						      : -1.0f;
		// (Its expressions' moves, for the shapes of the strands kept.)
		if (allowed && pd.raw.bend > 0)
			read_strand_moves(rd, pd.raw);
		predict_strands(pd.raw);
		predicted += pd.raw.predicted;
	}
	const bool short_of = predicted > (double)allowed;
	if (short_of && allowed && options.strand_uneven > 1 && options.strand_cover > 0) {
		struct One {
			double key;
			size_t part;
			double points;
		};
		std::vector<One> all;
		for (size_t k = 0; k < pending.size(); k++) {
			RawStrands &raw = pending[k].raw;
			rate_strands(raw, options.strand_cover, options.strand_uneven);
			const double each = raw.sample.empty() ? 0
							       : (double)raw.root.size() / (double)raw.sample.size();
			for (const RawStrands::Sample &sm : raw.sample)
				all.push_back({sm.key / (double)raw.top, k, (double)sm.kept * each});
		}
		std::stable_sort(all.begin(), all.end(), [](const One &a, const One &b) { return a.key < b.key; });
		std::vector<double> points(pending.size(), 0.0);
		double sum = 0;
		for (size_t i = 0; i < all.size() && sum < (double)allowed; i++) {
			points[all[i].part] += all[i].points;
			sum += all[i].points;
		}
		for (size_t k = 0; k < pending.size(); k++)
			pending[k].raw.allowed = sum > 0 ? (size_t)((double)allowed * (points[k] / sum)) : 0;
	} else {
		for (Pending &pd : pending)
			pd.raw.allowed = short_of ? (size_t)((double)allowed * (pd.raw.predicted / predicted))
						  : allowed;
	}
	for (Pending &pd : pending) {
		std::vector<uint32_t> which;
		Primitive p = finish_strands(pd.raw, options.strand_layers, which);
		p.hair.spots_off = anchor_strands(rd, pd.raw, which, p);
		m.strands_in_file += p.hair.in_file;
		m.strands += p.hair.strands();
		m.strand_points += p.pos.size();
		m.meshes[pd.mesh].parts[pd.part] = std::move(p);
		pd.raw = RawStrands();
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

// Its own animations. A channel DireBot can't play (the strengths of a
// face's shapes, a missing part, numbers that don't add up) is left out;
// the model loads all the same.
// An animation's channel (`cj`, of animation `aj`) on one of the model's
// nodes, into `c`: false if it's broken, isn't a move, turn or size - or
// the strengths of its mesh's shapes ("weights"), on a node whose file
// asks for them to be played (Node::plays_shapes; any other's are its
// face's, moved by yours, and aren't read) - or there's too much of it
// (`numbers`: all the keys' numbers so far).
bool read_channel(Reader &rd, const json &aj, const json &cj, const Model &m, AnimChannel &c, size_t &numbers)
{
	const json &samplers = list(aj, "samplers");
	const json &target = field(cj, "target");
	const std::string path = text(target, "path");
	const int node = index_of(target, "node");
	const int si = index_of(cj, "sampler");
	if (node < 0 || node >= (int)m.nodes.size() || si < 0 || si >= (int)samplers.size())
		return false;
	c = AnimChannel();
	c.node = node;
	if (path == "translation") {
		c.path = AnimChannel::Move;
	} else if (path == "rotation") {
		c.path = AnimChannel::Turn;
	} else if (path == "scale") {
		c.path = AnimChannel::Size;
	} else if (path == "weights") {
		const Node &n = m.nodes[node];
		if (!n.plays_shapes || n.mesh < 0 || n.mesh >= (int)m.meshes.size() || m.meshes[n.mesh].weights.empty())
			return false;
		c.path = AnimChannel::Shapes;
		c.shapes = (int)m.meshes[n.mesh].weights.size();
	} else {
		return false;
	}
	const json &sj = samplers[si];
	const std::string ease = text(sj, "interpolation");
	c.ease = ease == "STEP" ? AnimChannel::Step : ease == "CUBICSPLINE" ? AnimChannel::Curved : AnimChannel::Straight;
	try {
		c.times = rd.floats(index_of(sj, "input"), 1);
		// (Shapes' strengths: one list of plain numbers, a key's after another's.)
		c.values = rd.floats(index_of(sj, "output"), c.path == AnimChannel::Shapes ? 1 : c.size());
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
			if (!read_channel(rd, aj, cj, m, c, numbers))
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

void load_json(Reader &rd, const json &doc, Model &m, const LoadOptions &options)
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
	read_constraints(doc, m);
	// (Stand-in hair: only what some part wears.)
	for (const json &mj : list(doc, "meshes"))
		for (const json &pj : list(mj, "primitives")) {
			const int mat = index_of(pj, "material");
			if (mat >= 0 && mat < (int)m.materials.size() && m.materials[mat].hair_standin)
				m.hair_standins = true;
		}
	read_meshes(rd, doc, m, options);
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
					   m.materials[p.material].normal_map.used() && !p.uv.empty() && !p.strands;
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
			if (!read_channel(rd, aj, cj, m, c, numbers))
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
	// Skin by these whole words too (a name split where it isn't a letter,
	// and where a capital starts a word: "Ear_L", "earRight", "MAT ears"):
	// inside another word they're something else ("ear" in "earring",
	// "arm" in "armour", "hand" in "handle").
	static const std::initializer_list<const char *> WORDS = {"ear",  "ears",  "neck", "nose", "lip",
								  "lips", "hand",  "hands", "arm",  "arms",
								  "leg",  "legs",  "foot", "feet", "torso"};
	auto has_word = [](const std::string &raw) {
		std::vector<std::string> words;
		std::string w;
		for (const char ch : raw) {
			const unsigned char c = (unsigned char)ch;
			if (!std::isalpha(c)) {
				if (!w.empty())
					words.push_back(lower(w));
				w.clear();
				continue;
			}
			if (std::isupper(c) && !w.empty() && std::islower((unsigned char)w.back())) {
				words.push_back(lower(w));
				w.clear();
			}
			w += ch;
		}
		if (!w.empty())
			words.push_back(lower(w));
		for (const std::string &word : words)
			for (const char *s : WORDS)
				if (word == s)
					return true;
		return false;
	};
	for (size_t i = 0; i < out.size(); i++) {
		const std::string name = lower(m.materials[i].name);
		out[i] = (has(name, {"skin", "face", "body", "head"}) || has_word(m.materials[i].name)) &&
			 !has(name, NOT);
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

V3 MorphTarget::pos_of(uint32_t v) const
{
	const auto it = std::lower_bound(at.begin(), at.end(), v);
	return it != at.end() && *it == v ? pos[(size_t)(it - at.begin())] : V3{};
}

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

size_t Strands::strands_in(size_t points) const
{
	if (first.empty())
		return 0;
	const auto past = std::upper_bound(first.begin(), first.end(), (uint32_t)std::min<size_t>(points, UINT32_MAX));
	return past == first.begin() ? 0 : (size_t)(past - first.begin()) - 1;
}

size_t Strands::strands_under(float threshold) const
{
	const size_t n = std::min(strands(), key.size());
	return (size_t)(std::lower_bound(key.begin(), key.begin() + (std::ptrdiff_t)n, threshold) - key.begin());
}

float Strands::threshold_of(size_t count) const
{
	return count < strands() && count < key.size() ? std::min(full, key[count]) : full;
}

int Strands::rate_step(float rate)
{
	const float r = rate >= 0.25f ? std::min(1.0f, rate) : 0.25f;
	return (int)std::lround((r - 0.25f) / 0.75f * (float)(RATES - 1));
}

void Strands::pieces_of(size_t run, size_t count, Stretch out[2]) const
{
	out[0] = out[1] = Stretch();
	if (run + 1 >= runs.size())
		return;
	const uint32_t a = runs[run], b = runs[run + 1], middle = run < thin.size() ? thin[run] : b;
	// (A stretch's pieces are in strand order: those before the first
	// point of strand `count`.)
	auto under = [&](uint32_t from, uint32_t to) {
		if (count >= strands())
			return to - from;
		return (uint32_t)(std::lower_bound(pieces.begin() + from, pieces.begin() + to, first[count]) -
				  (pieces.begin() + from));
	};
	out[0] = {a, under(a, middle)};
	out[1] = {middle, under(middle, b)};
}

uint32_t Strands::pieces_of(size_t run, size_t count) const
{
	Stretch both[2];
	pieces_of(run, count, both);
	return both[0].count + both[1].count;
}

bool load_model(const uint8_t *bytes, size_t size, const std::string &folder, Model &out, std::string *problem,
		const LoadOptions &options)
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
		load_json(rd, doc, out, options);
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

bool load_model(const std::string &path, Model &out, std::string *problem, const LoadOptions &options)
{
	std::vector<uint8_t> bytes;
	if (!read_file(path, bytes)) {
		if (problem)
			*problem = "DireBot can't open the file (or it's over 512 MB)";
		return false;
	}
	const size_t slash = path.find_last_of("/\\");
	const std::string folder = slash == std::string::npos ? "." : path.substr(0, slash);
	return load_model(bytes.data(), bytes.size(), folder, out, problem, options);
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
