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

// Hair strands, the numbers: the hair detail setting and what each choice
// allows, how many of a part's strands are drawn for its size on screen,
// and the hair budget, which keeps hair's time on the graphics card under
// a limit by drawing fewer, wider hairs. Plain code over plain numbers
// (tests/unit_hair.cpp); the drawing is avatar3d.cpp's.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "model.hpp"

namespace direbot::face {

// The hair detail setting ("hair_detail": "full", "light" or "today";
// anything else: full).
//   Full   the most strands, the thinnest hairs
//   Light  fewer strands kept and drawn, each wider and more solid, and
//          half the budget
//   Today  a character's stand-in hair (its strips and pictures, as
//          before real strands) instead of its strands; one with no
//          stand-in hair keeps its strands, as Light does
enum class HairDetail { Full, Light, Today };

inline HairDetail hair_detail_of(const std::string &name)
{
	if (name == "light")
		return HairDetail::Light;
	if (name == "today")
		return HairDetail::Today;
	return HairDetail::Full;
}

inline const char *hair_detail_name(HairDetail d)
{
	return d == HairDetail::Light ? "light" : d == HairDetail::Today ? "today" : "full";
}

// What a choice allows. (As made here: nothing limited.)
struct HairPlan {
	// The most strand points a character keeps: posed on the graphics
	// card, and posed here on the CPU (where every point costs time each
	// frame).
	size_t points_card = 2'000'000, points_cpu = 150'000;
	// A hair thinner than a pixel is drawn a pixel wide and as
	// see-through as it's thinner; only as many strands are drawn as
	// leaves each at least this solid.
	float solid = 0.4f;
	// Hair's time on the graphics card a frame, in milliseconds (0: no
	// limit).
	float budget_ms = 0;
	// The most pieces of hair (two triangles each) drawn in a frame, all
	// the hair on screen together, whatever the card's clock says (0: no
	// limit).
	size_t pieces = 0;
	// Hair that can't show - behind a solid part, or deep under other hair
	// - left out before the card cuts it into pixels (posed on the card,
	// Direct3D: the effect's strand_shown): 0 never, 1 where it can, 2
	// only the hair that would be left out is drawn (a test's view of it),
	// 3 every piece is left out (a measure: the card's work on hair before
	// any of it is cut into pixels), 4 the test is made and nothing is
	// left out (a measure: what the test itself costs).
	int skip_hidden = 1;
	// Hairs well under a pixel wide sent as lines (hair_form): 0 never, 1
	// where they're that thin, 2 always (tests).
	int lines = 1;
	// How hair posed on the card lies in its memory: 0 a point's numbers
	// once, in its strand's place, whichever pieces use it (the pieces'
	// corners point into them: a run's are far apart); 1 as it's drawn -
	// each run's corners one after another in the order its pieces go to
	// the card (Avatar3D's lay_strands), so the card reads them straight
	// through. (Measured on a Radeon RX 9070 XT, 11-11: a coat of 436,000
	// hairs 2.5 ms by point, 1.3 laid out - the same picture to the pixel.
	// Each form a part is drawn in is made off the drawing thread, and by
	// point its numbers aren't kept at all.)
	int layout = 1;
	// Laid out so, a part keeps each form it's drawn in (ribbons, lines)
	// in the card's memory: one it hasn't been drawn in for this many
	// frames is freed (and made again, off the drawing thread, if it's
	// drawn so again).
	uint32_t idle = 1800;
	// Laid out as drawn, a layer's groups in one draw (StrandMerge): 0 a
	// draw a group, 1 one draw while its groups keep their order (made
	// again once they haven't for a while), 2 (tests) the same, never made
	// again for a new order - a draw for each row that still holds.
	int merge = 0;
	// For measuring only ("hair_measure"): 1 hair is drawn into a single
	// pixel of the picture's corner (a scissor) - the card poses every
	// piece and puts its triangles together as ever, and fills next to
	// none of their pixels: what hair costs but for its pixels. 2 and 3
	// (posed on the card): every piece is left out, and of a corner's 52
	// bytes the card reads only its place and bones (32), or its place
	// alone (16, not posed): what reading a strand's numbers costs. 4 the
	// hull isn't drawn (hair behind other hair isn't hidden by it): what
	// the hull costs. 5 each run's pieces go to the card in two draws, its
	// first half and its second (the same picture): what a draw costs.
	int measure = 0;
	// The hair budget's rule (HairBudget::set_rule): 0 the one it's made
	// with, 1 the one that uses the whole limit - and with it a part that
	// is lines under a budget stays lines a little past where it would
	// become them (hair_form's `stays`).
	int budget_rule = 0;
	// Under a budget (fewer hairs, each wider) a part stays lines while
	// that loses under this much of what its hairs cover (hair_form's
	// `loss`; HAIR_LINE_LOSS as ever) - and with too few hairs for that,
	// it's ribbons at this much of the budget's share for each line it had
	// (hair_form's `cost`; HAIR_LINE_COST as ever).
	float line_loss = 0.05f, line_cost = 0.5f;
	// For measuring only ("hair_share"): the hair budget's say held at
	// this share of the hairs, whatever the clock says (0: not held).
	float share = 0;
};

// How much of what its hairs cover a part's lines may lose under a budget
// where they're kept lines with fewer hairs (HairPlan::line_loss; the most
// Strands::LINE_LOSS goes to).
constexpr float HAIR_LINE_LOSS_KEPT = 0.4f;

inline HairPlan hair_plan(HairDetail d)
{
	HairPlan p;
	if (d == HairDetail::Full) {
		p.points_card = 2'000'000;
		p.points_cpu = 150'000;
		p.solid = 0.4f;
		p.budget_ms = 1.0f;
		p.pieces = 1'500'000;
		// Its budget's rule that uses the whole limit, and with it a
		// budget's thin hairs kept lines with fewer hairs: the two go
		// together. (Measured on a Radeon RX 9070 XT, a coat of 436,000
		// hairs filling the picture: the first rule stops at 159,000 of
		// them, as ribbons, with a fifth of the limit unused; this rule
		// with lines as ever goes back and forth where the lines begin,
		// right at the limit; with the lines kept it holds 253,000 at
		// 0.93 ms (257,000 with lines that may lose 0.2).)
		p.budget_rule = 1;
		p.line_loss = HAIR_LINE_LOSS_KEPT;
	} else {
		p.points_card = 800'000;
		p.points_cpu = 60'000;
		p.solid = 0.65f;
		p.budget_ms = 0.5f;
		p.pieces = 750'000;
	}
	return p;
}

// How a character's file is loaded for a choice: `on_card` whether its
// points are posed on the graphics card.
inline LoadOptions hair_load_options(HairDetail d, bool on_card)
{
	const HairPlan plan = hair_plan(d);
	LoadOptions o;
	const size_t points = on_card ? plan.points_card : plan.points_cpu;
	o.strand_points = d == HairDetail::Today ? 0 : points;
	o.strand_points_bare = points;
	o.strand_bend = d == HairDetail::Full ? 0.0003f : 0.0005f;
	o.strand_turn = d == HairDetail::Full ? 20.0f : 30.0f;
	o.strand_swing = d == HairDetail::Full ? 0.004f : 0.008f;
	return o;
}

// How many of a part's strands are drawn: the first `count` of them (in
// their draw order) - the ones whose keys are under `upto`
// (Strands::key): min(1, upto x its rate) of the hairs round each strand,
// each drawn as much wider, so the coat covers as much.
struct HairShare {
	size_t count = 0;
	float upto = 1;
};

// For a part's strands, the first `loaded` of them ready to draw, its
// bones making it `scale` times bigger, seen from `distance`, where one
// drawn pixel is `pixel` wide a unit from the camera and no hair is drawn
// thinner than `thinnest` pixels: all of them while its hairs (as wide as
// they're drawn with all of them: Strands::mean_width) are at least
// `solid` of a pixel wide; smaller on screen, fewer - its threshold as
// much lower as leaves the hairs drawn that solid. Then `budget` (0 to 1,
// the hair budget's say) of that. Never more than the `loaded`, never
// fewer than a few.
inline HairShare hair_share(const Strands &h, size_t loaded, float scale, float distance, float pixel, float thinnest,
			    float solid, float budget)
{
	HairShare out;
	loaded = std::min(loaded, h.strands());
	if (!loaded)
		return out;
	const float on_screen = h.mean_width * scale / std::max(1e-12f, distance * pixel); // a hair's width in pixels
	float share = on_screen / std::max(1e-6f, 0.95f * thinnest * std::min(1.0f, std::max(0.05f, solid)));
	if (!std::isfinite(share))
		share = 1;
	share = std::min(1.0f, share) * std::min(1.0f, std::max(0.0f, budget));
	const size_t least = std::min<size_t>(loaded, 16);
	out.count = share >= 1 ? loaded : std::min(loaded, std::max(least, h.strands_under(share * h.full)));
	out.upto = h.threshold_of(out.count);
	return out;
}

// How many pixels a width of a part's (its file's units, of its hairs as
// drawn with all of them: Strands::mean_width, line_pixel) comes to on
// screen with the share of its hairs `s` - fewer hairs, each as much
// wider.
inline float hair_px(const Strands &h, float width, const HairShare &s, float scale, float distance, float pixel)
{
	return width * scale / std::max(1e-12f, distance * pixel) * (h.full / std::max(1e-6f, s.upto));
}

// Lines. A hair thinner than a pixel is drawn a pixel wide and as
// see-through as it's thinner - which is what a line is: a pixel wide. So
// where a part's hairs are that thin anyway, its pieces go to the card as
// lines: the same picture from half the corners and half the shapes of a
// ribbon's two triangles. A line covers one pixel in each row or column
// it crosses, so it can't stand for a hair wider than 0.7 of a pixel (one
// running corner to corner) to 1 (straight across): the effect makes it
// as solid as its hair is wide, up to that, and what's wider is lost.
//   - A part is lines when, with every hair its size on screen asks for,
//     that loses its hairs under a hundredth of what they cover
//     (Strands::line_pixel is no bigger than a pixel) - and once it is,
//     until a pixel is a tenth smaller than that, so it doesn't flicker
//     between the two at the edge. Nearer, it's ribbons as ever: nothing
//     changes in a close-up.
//   - A coat of many thin hairs and a few wide ones (an animal's: the
//     wide ones standing for the deep coat) would never be lines by that,
//     for what its wide hairs lose. It's in two sorts (Strands::thin), and
//     by the same rule of its thin pieces alone (Strands::thin_pixel)
//     those go as lines while its wide ones stay ribbons.
//   - The budget draws fewer hairs, each wider - and a line can only be
//     more solid. So under a budget it stays lines while that loses under
//     a twentieth (Strands::line_pixel_most, thin_pixel_most); with too
//     few hairs for that it's ribbons again - at as much less of the
//     budget's share as the lines saved (a ribbon costs the card about
//     twice a line, HAIR_LINE_COST): the same cost either side of the
//     change, so the budget finds one answer and stays there. (But never
//     under the budget's least share, HAIR_SHARE_LEAST: a coat is never
//     thinned further than it was as ribbons alone.)
//   - How much a budget's lines may lose, and what a ribbon costs against
//     a line, are the plan's to say (HairPlan::line_loss, line_cost: a
//     coat a little thinner with its hairs still hair-thin, or as thick
//     as ever with fewer, wider hairs). With the budget's other rule
//     (HairPlan::budget_rule) a part that's lines under a budget stays
//     lines until a pixel is a tenth smaller, as above. (Full's plan: that
//     rule, and lines that may lose HAIR_LINE_LOSS_KEPT - hair_plan.)
constexpr float HAIR_LINE_STAYS = 1.1f, HAIR_LINE_COST = 0.5f, HAIR_LINE_LOSS = 0.05f, HAIR_SHARE_LEAST = 0.1f;

// How wide a pixel has to be (in the part's file's units, its hairs as
// drawn with all of them) for its hairs sent as lines - all of them
// (`whole`), or its thin ones - to lose `loss` of what they cover:
// Strands' line_pixel_most for a twentieth (and under), its line_pixel_at
// for more, and a straight line between the ones it knows.
inline float hair_line_pixel(const Strands &h, bool whole, float loss)
{
	const float *at = whole ? h.line_pixel_at : h.thin_pixel_at;
	float under = HAIR_LINE_LOSS, pixel = whole ? h.line_pixel_most : h.thin_pixel_most;
	if (!(loss > under))
		return pixel;
	for (int k = 0; k < Strands::LINE_LOSSES && at[k] > 0; k++) {
		if (loss <= Strands::LINE_LOSS[k])
			return pixel + (at[k] - pixel) * (loss - under) / (Strands::LINE_LOSS[k] - under);
		under = Strands::LINE_LOSS[k];
		pixel = at[k];
	}
	return pixel;
}

struct HairForm {
	HairShare share;
	// All its pieces as lines; or (of a part in two sorts) its thin ones.
	bool lines = false, thin = false;
};

// hair_share's answer and whether the part is sent as lines: `lines` the
// plan's say (HairPlan::lines), `was_lines` and `was_thin` what it was
// last frame; `loss`, `cost` and `stays` the plan's say for lines under a
// budget (HairPlan::line_loss, line_cost, and its budget_rule 1).
inline HairForm hair_form(const Strands &h, size_t loaded, float scale, float distance, float pixel, float thinnest,
			  float solid, float budget, int lines, bool was_lines, bool was_thin = false,
			  float loss = HAIR_LINE_LOSS, float cost = HAIR_LINE_COST, bool stays = false)
{
	HairForm f;
	f.share = hair_share(h, loaded, scale, distance, pixel, thinnest, solid, budget);
	if (!f.share.count || lines <= 0)
		return f;
	if (lines >= 2) {
		f.lines = true;
		return f;
	}
	const HairShare all = budget < 1 ? hair_share(h, loaded, scale, distance, pixel, thinnest, solid, 1.0f)
					 : f.share;
	const bool whole = h.line_pixel > 0 && hair_px(h, h.line_pixel, all, scale, distance, pixel) <=
						       (was_lines ? HAIR_LINE_STAYS : 1.0f);
	const bool thin = !whole && !h.thin.empty() && h.thin_pixel > 0 &&
			  hair_px(h, h.thin_pixel, all, scale, distance, pixel) <=
				  (was_lines || was_thin ? HAIR_LINE_STAYS : 1.0f);
	if (!whole && !thin)
		return f;
	const bool was = stays && (was_lines || (!whole && was_thin));
	if (!(budget < 1) || hair_px(h, hair_line_pixel(h, whole, loss), f.share, scale, distance, pixel) <=
				     (was ? HAIR_LINE_STAYS : 1.0f)) {
		(whole ? f.lines : f.thin) = true;
		return f;
	}
	const float line = std::min(1.0f, std::max(0.1f, cost));
	const float ribbons = whole ? line : 1 - (1 - line) * std::min(1.0f, std::max(0.0f, h.thin_share));
	f.share = hair_share(h, loaded, scale, distance, pixel, thinnest, solid,
			     std::max(budget * ribbons, std::min(budget, HAIR_SHARE_LEAST)));
	return f;
}

// The corners a strand part's pieces go to the graphics card with (its
// index buffer; a point's two sides are corners 2 x point and 2 x point +
// 1). As ribbons, 6 a piece: two triangles over its start's and its end's
// sides. As lines, 2 a piece: its start's and its end's first sides (the
// effect puts both sides at the hair's middle then). A piece runs from
// starts[k] to ends[k] - or, with no `ends`, to the point after its start.
inline void strand_corners(const uint32_t *starts, const uint32_t *ends, size_t pieces, bool lines, uint32_t *out)
{
	for (size_t k = 0; k < pieces; k++) {
		const uint32_t a0 = starts[k] * 2, b0 = (ends ? ends[k] : starts[k] + 1) * 2;
		if (lines) {
			out[k * 2] = a0;
			out[k * 2 + 1] = b0;
		} else {
			const uint32_t six[6] = {a0, b0, a0 + 1, a0 + 1, b0, b0 + 1};
			std::copy(six, six + 6, out + k * 6);
		}
	}
}

// A strand part's pieces of one sort (0: each run's wide pieces, or all of
// them with one sort; 1: its thin ones - Strands::thin) laid out as
// they're drawn (HairPlan::layout 1): the points the card is given, in the
// order its pieces are drawn in (Strands::pieces), each run's after the
// one before's. A piece that starts where the piece before it (in the
// same run and sort) ends shares that point; a point of two runs, or of
// both sorts, is there once for each.
//   points   each point laid out: which of the part's points it is
//   corners  its pieces' corners, as strand_corners has them - 6 a piece
//            as ribbons (a point laid out is two corners, its sides: 2 x
//            its place and 2 x its place + 1), 2 a piece as lines (one
//            corner a point: its place)
//   from     a run: how many pieces of this sort come before its own (one
//            more entry than runs: all of them)
// The first so many strands' pieces of a run (Strands::pieces_of) are
// still the start of its stretch: pieces from[run] on.
struct StrandLayout {
	std::vector<uint32_t> points, corners, from;
};

inline StrandLayout strand_layout(const Strands &h, int sort, bool lines)
{
	StrandLayout out;
	if (h.runs.size() < 2)
		return out;
	const size_t runs = h.runs.size() - 1;
	const bool two = h.thin.size() >= runs;
	out.from.assign(runs + 1, 0);
	size_t pieces = 0;
	for (size_t r = 0; r < runs; r++) {
		const uint32_t middle = two ? h.thin[r] : h.runs[r + 1];
		out.from[r] = (uint32_t)pieces;
		pieces += sort ? h.runs[r + 1] - middle : middle - h.runs[r];
	}
	out.from[runs] = (uint32_t)pieces;
	const size_t sides = lines ? 1 : 2;
	out.corners.reserve(pieces * (lines ? 2 : 6));
	out.points.reserve(pieces + pieces / 2);
	for (size_t r = 0; r < runs; r++) {
		const uint32_t middle = two ? h.thin[r] : h.runs[r + 1];
		const uint32_t a = sort ? middle : h.runs[r], b = sort ? h.runs[r + 1] : middle;
		uint32_t end = 0; // (the piece before's end's first corner)
		for (uint32_t q = a; q < b; q++) {
			const uint32_t i = h.pieces[q];
			uint32_t a0 = end;
			if (q == a || i != h.pieces[q - 1] + 1) {
				a0 = (uint32_t)(out.points.size() * sides);
				out.points.push_back(i);
			}
			const uint32_t b0 = (uint32_t)(out.points.size() * sides);
			out.points.push_back(i + 1);
			end = b0;
			if (lines) {
				out.corners.push_back(a0);
				out.corners.push_back(b0);
			} else {
				const uint32_t six[6] = {a0, b0, a0 + 1, a0 + 1, b0, b0 + 1};
				out.corners.insert(out.corners.end(), six, six + 6);
			}
		}
	}
	return out;
}

// A layer of a strand part's pieces of one sort, all its groups' in one
// draw (HairPlan::merge 1). Laid out as drawn (strand_layout) each group's
// each layer is a stretch of its own, and a layer's groups go to the card
// far to near - an order that changes as the character turns - so a layer
// is a draw a group (a coat on 32 bones in 5 layers and two sorts: 277
// draws; measured on a Radeon RX 9070 XT, 11-11: half a microsecond of the
// card's time each, a tenth of that coat's whole cost). Here a layer's
// stretches lie one after another in the order its groups were drawn in
// when it was made - their corners copied out of the layout's, the first
// `count` strands' pieces of each group - so while the groups keep that
// order and that many strands are drawn, the layer is one draw: the same
// corners in the same order as a draw a group, so the same picture. Once
// groups have changed places, the stretches that still follow each other
// go together - a draw for each such row (draws) - until it's made again.
struct StrandMerge {
	std::vector<int> order; // the groups, in the order it was made in
	std::vector<uint32_t>
		at; // each of those, in that order: how many pieces come before its own (and one more entry: all of them)
	std::vector<int> place; // a group: its place in `order` (-1: it isn't in it)
	size_t count = 0;       // the strands it was made for (the first so many)
	bool made = false;

	// A row of its pieces: one draw.
	struct Row {
		uint32_t from = 0, pieces = 0;
	};
	// The draws that send the groups `now`'s pieces in that order: `out`
	// (emptied first). False - and nothing in `out` - if it wasn't made
	// with every one of them.
	bool draws(const std::vector<int> &now, std::vector<Row> &out) const
	{
		out.clear();
		if (!made)
			return false;
		for (int g : now) {
			if (g < 0 || (size_t)g >= place.size() || place[(size_t)g] < 0) {
				out.clear();
				return false;
			}
			const size_t k = (size_t)place[(size_t)g];
			const uint32_t from = at[k], pieces = at[k + 1] - at[k];
			if (!pieces)
				continue;
			if (!out.empty() && out.back().from + out.back().pieces == from)
				out.back().pieces += pieces;
			else
				out.push_back({from, pieces});
		}
		return true;
	}
};

// When one is made, and made again (Avatar3D's draw_merged): once the
// strands drawn and their form have been the same for MERGE_STEADY frames;
// again once its groups' order takes more than MERGE_ROWS draws, no sooner
// than MERGE_REST frames after the last time; and in one frame no more of
// them than about MERGE_BYTES of corners (one always): making one is a
// copy of a layer's corners, on the CPU.
constexpr uint32_t MERGE_STEADY = 20, MERGE_REST = 30;
constexpr size_t MERGE_ROWS = 4, MERGE_BYTES = (size_t)2 << 20;

// How many pieces a layer's merged stretch has room for: all of that
// layer's pieces of the sort `from` is of (StrandLayout::from).
inline size_t strand_merge_room(const Strands &h, const std::vector<uint32_t> &from, size_t layer)
{
	const size_t layers = (size_t)std::max(1, h.layers);
	size_t room = 0;
	for (size_t g = 0; layer < layers && g < h.groups.size(); g++) {
		const size_t run = g * layers + layer;
		if (run + 1 < from.size())
			room += from[run + 1] - from[run];
	}
	return room;
}

// Makes one: layer `layer`'s pieces of sort `sort` (0 or 1) of the first
// `count` strands, the groups in `order` (each of the part's groups once
// at most). `from` and `corners`: that sort's layout (StrandLayout::from
// and corners, `each` corners a piece - 6 as ribbons, 2 as lines); `out`:
// where its corners go, with room for strand_merge_room pieces. Not made
// (StrandMerge::made false) if `order` names a group twice or one the
// part doesn't have.
inline StrandMerge strand_merge(const Strands &h, const std::vector<uint32_t> &from, int sort, size_t layer,
				size_t count, const std::vector<int> &order, const uint32_t *corners, size_t each,
				uint32_t *out)
{
	StrandMerge m;
	const size_t layers = (size_t)std::max(1, h.layers);
	if (layer >= layers || !corners || !out || !each)
		return m;
	m.place.assign(h.groups.size(), -1);
	m.count = count;
	uint32_t pieces = 0;
	for (int g : order) {
		if (g < 0 || (size_t)g >= m.place.size() || m.place[(size_t)g] >= 0)
			return StrandMerge();
		const size_t run = (size_t)g * layers + layer;
		Strands::Stretch some[2];
		h.pieces_of(run, count, some);
		const uint32_t n = run < from.size() ? some[sort ? 1 : 0].count : 0;
		if (n)
			std::copy(corners + (size_t)from[run] * each, corners + ((size_t)from[run] + n) * each,
				  out + (size_t)pieces * each);
		m.place[(size_t)g] = (int)m.order.size();
		m.order.push_back(g);
		m.at.push_back(pieces);
		pieces += n;
	}
	m.at.push_back(pieces);
	m.made = true;
	return m;
}

// The hull (Avatar3D's draw_hull): the inner layers - the first `core` of
// them - of a quarter of the strands drawn (the first of them; of a few
// strands, all), each as much wider, into the picture of the solid parts'
// distances.
inline size_t hull_strands(size_t count)
{
	return std::max(std::min<size_t>(count, 16), count / 4);
}

// A strand part's hull laid out as it's drawn (HairPlan::layout 1). The
// picture it's drawn into keeps the nearest distance at each pixel, so the
// order its pieces go to the card in changes nothing: they lie a group at
// a time (each group has its own reach) and inside a group by strand, a
// strand's pieces after each other, whichever layer or sort they're in.
// So the first so many strands' pieces of a group are the start of its
// stretch, the card reads them straight through, and a group is one draw.
// Only the strands the hull can ever draw: the first hull_strands of all
// of them.
//   points   each point laid out: which of the part's points it is (two
//            corners each, its sides: 2 x its place and 2 x its place + 1)
//   corners  its pieces' corners, 6 a piece, as strand_corners has them
//   from     a group: how many pieces come before its own (one more entry
//            than groups: all of them)
//   starts   each piece laid out: the point it starts at (they rise
//            inside a group)
struct StrandHull {
	std::vector<uint32_t> points, corners, from, starts;

	// How many of group `g`'s pieces - the start of its stretch - belong
	// to the first `count` strands.
	uint32_t pieces_of(const Strands &h, size_t g, size_t count) const
	{
		if (g + 1 >= from.size())
			return 0;
		const uint32_t a = from[g], b = from[g + 1];
		if (count >= h.strands())
			return b - a;
		return (uint32_t)(std::lower_bound(starts.begin() + a, starts.begin() + b, h.first[count]) -
				  (starts.begin() + a));
	}
};

inline StrandHull strand_hull(const Strands &h)
{
	StrandHull out;
	const size_t layers = (size_t)std::max(1, h.layers), core = (size_t)std::max(0, std::min(h.core, h.layers));
	const size_t groups = h.groups.size();
	if (!core || !groups || !h.strands() || h.runs.size() < groups * layers + 1)
		return out;
	const size_t most = hull_strands(h.strands());
	out.from.assign(groups + 1, 0);
	std::vector<uint32_t> some;
	for (size_t g = 0; g < groups; g++) {
		out.from[g] = (uint32_t)out.starts.size();
		some.clear();
		for (size_t l = 0; l < core; l++) {
			Strands::Stretch both[2];
			h.pieces_of(g * layers + l, most, both);
			for (const Strands::Stretch &one : both)
				some.insert(some.end(), h.pieces.begin() + one.from,
					    h.pieces.begin() + one.from + one.count);
		}
		std::sort(some.begin(), some.end());
		uint32_t end = 0; // (the piece before's end's first corner)
		for (size_t k = 0; k < some.size(); k++) {
			const uint32_t i = some[k];
			uint32_t a0 = end;
			if (k == 0 || i != some[k - 1] + 1) {
				a0 = (uint32_t)(out.points.size() * 2);
				out.points.push_back(i);
			}
			const uint32_t b0 = (uint32_t)(out.points.size() * 2);
			out.points.push_back(i + 1);
			end = b0;
			const uint32_t six[6] = {a0, b0, a0 + 1, a0 + 1, b0, b0 + 1};
			out.corners.insert(out.corners.end(), six, six + 6);
			out.starts.push_back(i);
		}
	}
	out.from[groups] = (uint32_t)out.starts.size();
	return out;
}

// The hair budget. Each frame the drawing says how long hair took on the
// graphics card (milliseconds; the card's own clock, read a few frames
// late), and the budget answers with the share of the hairs to draw next
// (0 to 1 of what its size on screen asks for). Over the limit: fewer at
// once, by as much as it's over. Well under: more, a little at a time. A
// game sharing the card makes single readings jump (the card stopped to
// draw the game in between), so it goes by the lowest few of the last
// readings, and waits for fresh ones after every change. It starts low
// and works up, so a new character never starts over the limit. It never
// goes under a tenth: fewer than that (each hair ten times as wide) and a
// coat is shards - a card that slow is over the limit, and the reading
// (measured) says so. A clock that stops answering (SILENT frames in a
// row with no reading) is no clock: all the hairs are drawn, as with no
// limit, until it answers again - never held at a share nothing measures.
//
// Its other rule (set_rule(1); it's made with the first). The first holds
// anywhere from three quarters of the limit up to it, so as much as a
// quarter of the limit goes unused; and where what hair costs jumps as
// more of it is drawn (a part's hairs turn from ribbons into lines:
// hair_form) it can go up over the jump and come back down for ever. This
// one:
//   - goes up while hair takes under CLIMB of the limit, aiming at AIM of
//     it;
//   - keeps the share it was over the limit at. Where it had just come up
//     to that from a share it was under at, it goes back to that one, and
//     halves the gap between the two until the gap is a few hundredths
//     (GAP): a jump is found once, not crossed again and again;
//   - with a share it was over at kept, holds as the first rule does
//     (more only when well under, never to within GAP of that share), and
//     tries past it after WAIT sets of readings with room to spare - four
//     times as many after each try that ends over the limit;
//   - forgets that share once hair takes a tenth less than it has where
//     it's held (EASED: a game stopped, a character left).
class HairBudget {
public:
	static constexpr float START = 0.25f, LEAST = HAIR_SHARE_LEAST;
	static constexpr size_t READINGS = 6; // how many it waits for after a change
	static constexpr size_t SILENT = 120; // frames with no reading before the clock counts as gone
	// (The other rule's.)
	static constexpr float CLIMB = 0.9f, AIM = 0.95f, GAP = 0.03f, EASED = 0.9f;
	static constexpr uint32_t WAIT = 300, WAIT_MOST = 1u << 20;

	// Milliseconds a frame (0 or less: no limit - all of them).
	void set_limit(float ms)
	{
		const float to = ms > 0 && std::isfinite(ms) ? ms : 0.0f;
		if (to == limit_)
			return;
		limit_ = to;
		if (!(limit_ > 0))
			share_ = 1;
		else if (share_ >= 1)
			share_ = START;
		readings_.clear();
		measured_ = -1;
		unread_ = 0;
		forget();
	}
	// Which rule: 0 the first, 1 the other.
	void set_rule(int rule)
	{
		const int to = rule == 1 ? 1 : 0;
		if (to == rule_)
			return;
		rule_ = to;
		readings_.clear();
		forget();
	}
	int rule() const { return rule_; }
	float limit() const { return limit_; }
	float share() const { return limit_ > 0 && !silent() ? share_ : 1.0f; }
	// A frame whose clock was asked and never answered.
	void unread()
	{
		if (unread_ < SILENT)
			unread_++;
	}
	// The clock has stopped answering (under a limit).
	bool silent() const { return limit_ > 0 && unread_ >= SILENT; }
	// The time hair took at the share it was drawn with, as the budget
	// sees it: the second lowest of its last full set of readings (-1:
	// none yet under this limit).
	float measured() const { return measured_; }
	// How many times its share has changed since its limit or its rule was
	// set; and (the other rule) the share it was over the limit at, kept
	// (0: none).
	size_t changes() const { return changes_; }
	float over() const { return over_; }

	// A reading: hair took `ms` drawn at `share` of its hairs. Readings
	// at another share than the one asked for now (drawn before the last
	// change reached the card) are left out.
	void read(float ms, float share)
	{
		if (!(limit_ > 0) || !(ms >= 0) || !std::isfinite(ms))
			return;
		unread_ = 0;
		if (std::fabs(share - share_) > 1e-6f)
			return;
		readings_.push_back(ms);
		if (readings_.size() < READINGS)
			return;
		std::vector<float> sorted = readings_;
		std::sort(sorted.begin(), sorted.end());
		const float took = sorted[1];
		measured_ = took;
		readings_.clear();
		const float was = share_;
		if (rule_ == 1) {
			whole(took);
		} else if (took > limit_) {
			// Over: fewer at once (never less than half at a step: a
			// reading can be wrong).
			share_ = std::max(LEAST, share_ * std::max(0.5f, 0.9f * limit_ / took));
		} else if (took < 0.75f * limit_ && share_ < 1) {
			// Well under: more, up to a quarter more at a step, aiming
			// a little under the limit.
			const float room = took > 1e-4f ? 0.9f * limit_ / took : 1.25f;
			share_ = std::min(1.0f, share_ * std::min(1.25f, std::max(1.02f, room)));
		}
		if (share_ != was)
			changes_++;
	}

private:
	// The other rule, for a set of readings: hair took `took`.
	void whole(float took)
	{
		const float was = share_;
		if (took > limit_) {
			// Over. (On a try past a share it had been over at: the next
			// try waits four times as long.)
			if (tried_ > 0) {
				wait_ = wait_ < WAIT_MOST / 4 ? wait_ * 4 : WAIT_MOST;
				tried_ = 0;
			}
			over_ = share_;
			held_ = 0;
			// Back to the share it was last under at, where it has just
			// come up from one (and the gap between the two is searched);
			// else fewer by as much as it's over, as ever.
			search_ = under_ > 0 && under_ < share_;
			share_ = search_ ? under_ : std::max(LEAST, share_ * std::max(0.5f, 0.9f * limit_ / took));
			under_ = 0;
		} else {
			under_ = share_;
			if (over_ > 0 && (share_ >= over_ || (least_ >= 0 && took <= EASED * least_))) {
				// Under where it was over, or it has eased here: what it
				// kept is old.
				over_ = tried_ = 0;
				held_ = 0;
				wait_ = WAIT;
				search_ = false;
			}
			const bool room = took < CLIMB * limit_ && share_ < 1;
			if (over_ > 0) {
				float want = share_;
				if (search_ && room && over_ > share_ * (1 + 2 * GAP)) {
					want = std::sqrt(share_ * over_);
				} else {
					search_ = false;
					if (took < 0.75f * limit_ && share_ < 1) {
						const float more = took > 1e-4f ? 0.9f * limit_ / took : 1.25f;
						want = std::max(
							share_,
							std::min(share_ * std::min(1.25f, std::max(1.02f, more)),
								 over_ * (1 - GAP)));
					}
					if (want == share_ && room && ++held_ >= wait_) {
						// A try past it: from the next set of readings.
						tried_ = over_;
						over_ = 0;
						held_ = 0;
					}
				}
				share_ = std::min(1.0f, want);
			} else if (room) {
				const float more = took > 1e-4f ? AIM * limit_ / took : 1.25f;
				float want = share_ * std::min(1.25f, std::max(1.02f, more));
				// (A try: first to where it was over, no further.)
				if (tried_ > 0 && share_ < tried_)
					want = std::min(want, tried_);
				share_ = std::min(1.0f, want);
			} else if (tried_ > 0) {
				// The try has come to rest under the limit.
				tried_ = 0;
				wait_ = WAIT;
			}
		}
		// (The least hair has taken at the share it's held at.)
		if (share_ != was)
			least_ = -1;
		else if (least_ < 0 || took < least_)
			least_ = took;
	}
	void forget()
	{
		over_ = under_ = tried_ = 0;
		least_ = -1;
		held_ = 0;
		wait_ = WAIT;
		search_ = false;
		changes_ = 0;
	}

	float limit_ = 0, share_ = 1, measured_ = -1;
	size_t unread_ = 0, changes_ = 0;
	std::vector<float> readings_;
	int rule_ = 0;
	// (The other rule's: the share it was over at, the one it was last
	// under at, the one a try is on its way past; the least hair took
	// where it's held; sets of readings held with room, and how many
	// before a try; searching the gap under `over_`.)
	float over_ = 0, under_ = 0, tried_ = 0, least_ = -1;
	uint32_t held_ = 0, wait_ = WAIT;
	bool search_ = false;
};

} // namespace direbot::face
