// Unit tests for how a 3D character's pictures shrink (src/face/pictures.cpp):
// colours averaged as light, data as numbers, see-through edges never
// darker, cut-outs (lashes, stubble, hair cards) as solid small as full
// size, and each picture's use read from its materials.
// Build and run: tools/test-link.sh (Linux, any C++17 compiler).

#include "../src/face/pictures.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

using namespace direbot::face;

static int failures = 0;
static int passes = 0;

#define CHECK(cond)                                                              \
	do {                                                                     \
		if (cond) {                                                      \
			passes++;                                                \
		} else {                                                         \
			failures++;                                              \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                \
	} while (0)

using Pixels = std::vector<uint8_t>;

static Pixels filled(uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	Pixels p((size_t)w * h * 4);
	for (size_t i = 0; i < (size_t)w * h; i++) {
		p[i * 4] = r;
		p[i * 4 + 1] = g;
		p[i * 4 + 2] = b;
		p[i * 4 + 3] = a;
	}
	return p;
}

static const uint8_t *at(const Pixels &p, uint32_t w, uint32_t x, uint32_t y)
{
	return &p[((size_t)y * w + x) * 4];
}

static void set(Pixels &p, uint32_t w, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
	uint8_t *o = &p[((size_t)y * w + x) * 4];
	o[0] = r;
	o[1] = g;
	o[2] = b;
	o[3] = a;
}

// A lash or hair card: thin dark strands, soft-edged and fading to their
// tips, on nothing (see-through), the way they're painted.
static Pixels strands(uint32_t size, int count, unsigned seed)
{
	Pixels p = filled(size, size, 40, 28, 20, 0);
	auto next = [&seed]() {
		seed = seed * 1664525u + 1013904223u;
		return (seed >> 8) / 16777216.0f;
	};
	for (int s = 0; s < count; s++) {
		const float x0 = next() * size, y0 = next() * size, angle = next() * 6.2832f,
			    length = (0.1f + next() * 0.3f) * size, width = 0.8f + next() * 0.8f;
		const float dx = std::cos(angle), dy = std::sin(angle);
		const int lx = (int)std::floor(std::min(x0, x0 + dx * length)) - 3,
			  hx = (int)std::ceil(std::max(x0, x0 + dx * length)) + 3,
			  ly = (int)std::floor(std::min(y0, y0 + dy * length)) - 3,
			  hy = (int)std::ceil(std::max(y0, y0 + dy * length)) + 3;
		for (int y = std::max(0, ly); y < std::min((int)size, hy); y++)
			for (int x = std::max(0, lx); x < std::min((int)size, hx); x++) {
				const float px = x + 0.5f - x0, py = y + 0.5f - y0;
				const float along = std::min(length, std::max(0.0f, px * dx + py * dy));
				const float d = std::hypot(px - dx * along, py - dy * along);
				float a = std::min(1.0f, std::max(0.0f, width / 2 + 0.5f - d));
				a *= 1 - std::pow(along / length, 3.0f); // fading to its tip
				uint8_t *o = &p[((size_t)y * size + x) * 4];
				o[3] = std::max(o[3], (uint8_t)std::lround(a * 255));
			}
	}
	return p;
}

static void colours_as_light()
{
	// Black and white, finely mixed: half the light, so 186 as a colour
	// (the effect's 2.2 undone and done again), 128 as numbers.
	Pixels checker = filled(4, 4, 0, 0, 0, 255);
	for (uint32_t y = 0; y < 4; y++)
		for (uint32_t x = 0; x < 4; x++)
			if ((x + y) % 2)
				set(checker, 4, x, y, 255, 255, 255, 255);
	const ModelPicture colour = picture_from(checker.data(), 4, 4);
	CHECK(colour.size == 4);
	CHECK(colour.levels.size() == 3);
	CHECK(colour.levels[0] == checker);
	const uint8_t want = (uint8_t)std::lround(std::pow(0.5f, 1 / 2.2f) * 255);
	CHECK(want == 186);
	for (const Pixels *l : {&colour.levels[1], &colour.levels[2]})
		for (size_t i = 0; i < l->size(); i += 4) {
			CHECK((*l)[i] == want && (*l)[i + 1] == want && (*l)[i + 2] == want);
			CHECK((*l)[i + 3] == 255);
		}
	PictureUse data;
	data.colour = false;
	const ModelPicture numbers = picture_from(checker.data(), 4, 4, data);
	for (size_t i = 0; i < numbers.levels[2].size(); i += 4)
		CHECK(numbers.levels[2][i] == 128 && numbers.levels[2][i + 3] == 255);

	// Every grey goes back to itself (no drift from level to level).
	Pixels square = filled(512, 512, 0, 0, 0, 255);
	for (uint32_t y = 0; y < 512; y++)
		for (uint32_t x = 0; x < 512; x++) {
			const uint8_t g = (uint8_t)(x / 2);
			set(square, 512, x, y, g, g, g, 255);
		}
	const ModelPicture flat = picture_from(square.data(), 512, 512);
	bool same = true;
	for (uint32_t x = 0; x < 256; x++)
		same = same && at(flat.levels[1], 256, x, 7)[0] == x;
	CHECK(same);

	// Fine dark detail on light (stubble on skin): lighter small than
	// averaged as numbers, as it looks from afar.
	Pixels stubble = filled(64, 64, 230, 190, 170, 255);
	for (uint32_t y = 0; y < 64; y += 2)
		for (uint32_t x = (y / 2) % 2; x < 64; x += 2)
			set(stubble, 64, x, y, 30, 22, 18, 255);
	const ModelPicture as_light = picture_from(stubble.data(), 64, 64);
	const ModelPicture as_numbers = picture_from(stubble.data(), 64, 64, data);
	const uint8_t *small = at(as_light.levels[3], 8, 3, 3), *plain = at(as_numbers.levels[3], 8, 3, 3);
	CHECK(small[0] > plain[0] + 12);
	CHECK(small[1] > plain[1] + 12);
	CHECK(small[2] > plain[2] + 12);
	// ...and a quarter's light dark, three quarters skin: its light the mix.
	auto light = [](uint8_t v) { return std::pow(v / 255.0f, 2.2f); };
	for (int k = 0; k < 3; k++) {
		const uint8_t skin = at(stubble, 64, 1, 0)[k], hair = at(stubble, 64, 0, 0)[k];
		const float mixed = (light(skin) * 3 + light(hair)) / 4;
		CHECK(std::fabs(light(small[k]) - mixed) < 0.01f);
	}
}

static void see_through_edges()
{
	// Red beside nothing (see-through, black): stays red, half solid.
	Pixels edge = filled(4, 4, 0, 0, 0, 0);
	for (uint32_t y = 0; y < 4; y++)
		for (uint32_t x = 0; x < 2; x++)
			set(edge, 4, x, y, 255, 0, 0, 255);
	const ModelPicture pic = picture_from(edge.data(), 4, 4);
	const uint8_t *one = pic.levels[2].data();
	CHECK(one[0] == 255 && one[1] == 0 && one[2] == 0);
	CHECK(one[3] == 128);
	// Wholly see-through: its colours just averaged.
	const Pixels clear = filled(2, 2, 10, 200, 30, 0);
	const ModelPicture none = picture_from(clear.data(), 2, 2);
	CHECK(none.levels[1][0] == 10 && none.levels[1][1] == 200 && none.levels[1][2] == 30 && none.levels[1][3] == 0);
	// Made bigger (3 across to 4): no dark fringe either.
	Pixels three = filled(3, 3, 0, 0, 0, 0);
	set(three, 3, 1, 1, 250, 250, 250, 255);
	const ModelPicture grown = picture_from(three.data(), 3, 3);
	CHECK(grown.size == 4);
	bool white = true;
	for (size_t i = 0; i < grown.levels[0].size(); i += 4)
		if (grown.levels[0][i + 3] > 0)
			white = white && grown.levels[0][i] == 250 && grown.levels[0][i + 1] == 250;
	CHECK(white);
	CHECK(at(grown.levels[0], 4, 1, 1)[3] > 0);

	// A data picture's fourth number isn't how solid it is: a normal
	// map's directions average plainly, whatever it holds there.
	Pixels normals = filled(2, 2, 128, 128, 255, 0);
	set(normals, 2, 0, 0, 255, 128, 128, 255);
	PictureUse data;
	data.colour = false;
	const ModelPicture bumps = picture_from(normals.data(), 2, 2, data);
	const uint8_t *n = bumps.levels[1].data();
	CHECK(n[0] == 160 && n[1] == 128 && n[2] == 223);
}

static void sizes()
{
	CHECK(picture_from(nullptr, 4, 4).size == 0);
	const Pixels one = filled(1, 1, 9, 8, 7, 6);
	CHECK(picture_from(one.data(), 0, 1).size == 0);
	const ModelPicture dot = picture_from(one.data(), 1, 1);
	CHECK(dot.size == 1 && dot.levels.size() == 1 && dot.levels[0] == one);
	// Not square, not a power of two: square, the next power up.
	const Pixels wide = filled(300, 20, 50, 60, 70, 255);
	const ModelPicture up = picture_from(wide.data(), 300, 20);
	CHECK(up.size == 512);
	CHECK(up.levels.size() == 10);
	for (size_t i = 0; i < up.levels.size(); i++)
		CHECK(up.levels[i].size() == (size_t)(512 >> i) * (512 >> i) * 4);
	CHECK(at(up.levels[9], 1, 0, 0)[0] == 50 && at(up.levels[9], 1, 0, 0)[3] == 255);
	// Past the most: the most.
	const Pixels long_one = filled(2100, 3, 1, 2, 3, 255);
	const ModelPicture most = picture_from(long_one.data(), 2100, 3);
	CHECK(most.size == MOST_PICTURE);
	CHECK(most.levels.size() == 12);
	CHECK(at(most.levels[0], MOST_PICTURE, 2047, 2047)[2] == 3);
	// Twice the most: halved to it.
	const Pixels big = filled(4096, 4096, 90, 91, 92, 200);
	const ModelPicture halved = picture_from(big.data(), 4096, 4096);
	CHECK(halved.size == 2048 && at(halved.levels[0], 2048, 5, 5)[1] == 91 && at(halved.levels[0], 2048, 5, 5)[3] == 200);
}

static void cut_outs()
{
	const uint32_t size = 512;
	const Pixels lashes = strands(size, 70, 12345u);
	const float cutoff = 0.5f;
	const float full = solid_share(lashes, cutoff);
	std::printf("lash card: %.4f solid full size\n", full);
	CHECK(full > 0.02f && full < 0.2f);

	PictureUse cut;
	cut.cutoff = cutoff;
	const ModelPicture kept = picture_from(lashes.data(), size, size, cut);
	const ModelPicture plain = picture_from(lashes.data(), size, size);
	CHECK(kept.levels.size() == plain.levels.size());
	CHECK(kept.levels[0] == lashes);
	// Without: thins out, then all but vanishes. With: the same share
	// solid at every size down to 32 across (about a lash card on a face
	// a quarter of a 1080p stream high), and near it at 16 and 8.
	for (size_t i = 1; i <= 6; i++) {
		const float k = solid_share(kept.levels[i], cutoff), p = solid_share(plain.levels[i], cutoff);
		std::printf("  %4u across: kept %.4f, plain %.4f\n", size >> i, k, p);
		const float near = i <= 4 ? 0.1f : 0.25f;
		CHECK(std::fabs(k - full) <= full * near + 1.0f / ((size >> i) * (size >> i)));
		if (i >= 3)
			CHECK(p < full * 0.6f);
	}
	// Only how solid changes: the colours are the plain shrink's.
	bool colours = true;
	for (size_t i = 0; i < kept.levels.size(); i++)
		for (size_t j = 0; j < kept.levels[i].size(); j += 4)
			for (int c = 0; c < 3; c++)
				colours = colours && kept.levels[i][j + c] == plain.levels[i][j + c];
	CHECK(colours);
	// Never less solid than it was: scaled up, never down, for thin
	// strands.
	bool up = true;
	for (size_t i = 1; i < kept.levels.size(); i++)
		for (size_t j = 3; j < kept.levels[i].size(); j += 4)
			up = up && kept.levels[i][j] >= plain.levels[i][j];
	CHECK(up);

	// A low cut-off (0.1): kept as well.
	PictureUse low;
	low.cutoff = 0.1f;
	const float low_full = solid_share(lashes, 0.1f);
	const ModelPicture kept_low = picture_from(lashes.data(), size, size, low);
	for (size_t i = 1; i <= 4; i++)
		CHECK(std::fabs(solid_share(kept_low.levels[i], 0.1f) - low_full) <= low_full * 0.1f);

	// Wholly solid, wholly see-through, or a solid block: nothing to keep.
	const Pixels solid = filled(16, 16, 1, 2, 3, 255);
	CHECK(picture_from(solid.data(), 16, 16, cut).levels == picture_from(solid.data(), 16, 16).levels);
	const Pixels clear = filled(16, 16, 1, 2, 3, 0);
	CHECK(picture_from(clear.data(), 16, 16, cut).levels == picture_from(clear.data(), 16, 16).levels);
	Pixels half = filled(16, 16, 1, 2, 3, 0);
	for (uint32_t y = 0; y < 16; y++)
		for (uint32_t x = 0; x < 8; x++)
			set(half, 16, x, y, 1, 2, 3, 255);
	const ModelPicture halves = picture_from(half.data(), 16, 16, cut);
	for (size_t i = 0; i < 4; i++)
		CHECK(std::fabs(solid_share(halves.levels[i], cutoff) - 0.5f) < 1e-6f);

	// A cut-off from a see-through material (base 0.5, cut-off 0.25: the
	// picture's 0.5) reads the same.
	CHECK(solid_share(kept.levels[3], 0.5f) == solid_share(kept.levels[3], 0.25f, 0.5f));
	CHECK(solid_share(kept.levels[3], 0.5f) > 0);
	// Odd cut-offs leave it plain.
	for (float odd : {0.0f, -1.0f, 1.5f, std::numeric_limits<float>::quiet_NaN()}) {
		PictureUse u;
		u.cutoff = odd;
		CHECK(picture_from(lashes.data(), 64, 64, u).levels == picture_from(lashes.data(), 64, 64).levels);
	}
}

static void uses()
{
	Model m;
	m.images.resize(8);
	auto material = [&m](AlphaMode mode, int base, float cutoff, float alpha) {
		Material mat;
		mat.alpha = mode;
		mat.base_map.image = base;
		mat.cutoff = cutoff;
		mat.base[3] = alpha;
		m.materials.push_back(mat);
		return &m.materials.back();
	};
	material(AlphaMode::Mask, 0, 0.5f, 1);   // lashes
	material(AlphaMode::Mask, 1, 0.25f, 0.5f); // half see-through: the picture's 0.5
	material(AlphaMode::Mask, 2, 0.6f, 0.5f); // never shows: nothing to keep
	Material *skin = material(AlphaMode::Opaque, 3, 0.5f, 1);
	skin->normal_map.image = 4;
	skin->metal_map.image = 5;
	skin->occlusion_map.image = 5;
	skin->emissive_map.image = 6;
	skin->shade_map.image = 7;
	material(AlphaMode::Blend, 7, 0.5f, 1); // see-through, not cut out
	material(AlphaMode::Mask, 0, 0.3f, 1);  // shares the lashes' picture: the first's kept
	Material *odd = material(AlphaMode::Mask, 99, 0.5f, 1);
	odd->normal_map.image = -5;
	odd->metal_map.image = 8;
	material(AlphaMode::Mask, 3, 0.0f, 1);                                    // all shows
	material(AlphaMode::Mask, 3, std::numeric_limits<float>::quiet_NaN(), 1); // odd
	material(AlphaMode::Mask, 3, 0.5f, 0);                                    // nothing shows
	const std::vector<PictureUse> u = picture_uses(m);
	CHECK(u.size() == 8);
	CHECK(u[0].colour && std::fabs(u[0].cutoff - 0.5f) < 1e-6f);
	CHECK(u[1].colour && std::fabs(u[1].cutoff - 0.5f) < 1e-6f);
	CHECK(u[2].colour && u[2].cutoff < 0);
	CHECK(u[3].colour && u[3].cutoff < 0);
	CHECK(!u[4].colour && u[4].cutoff < 0);
	CHECK(!u[5].colour);
	CHECK(u[6].colour && u[7].colour && u[7].cutoff < 0);
	// A picture read as data anywhere is shrunk as data.
	material(AlphaMode::Opaque, 4, 0.5f, 1);
	CHECK(!picture_uses(m)[4].colour);
	CHECK(picture_uses(Model()).empty());
}

int main()
{
	colours_as_light();
	see_through_edges();
	sizes();
	cut_outs();
	uses();
	std::printf("%d passed, %d failed\n", passes, failures);
	return failures ? 1 : 0;
}
