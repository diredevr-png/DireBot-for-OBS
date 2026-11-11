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
	// any of it is cut into pixels).
	int skip_hidden = 1;
	// Hairs well under a pixel wide sent as lines (hair_form): 0 never, 1
	// where they're that thin, 2 always (tests).
	int lines = 1;
	// How hair posed on the card lies in its memory: 0 a point's numbers
	// once, in its strand's place, whichever pieces use it (the pieces'
	// corners point into them: a run's are far apart); 1 as it's drawn -
	// each run's corners one after another in the order its pieces go to
	// the card (Avatar3D's lay_strands), so the card reads them straight
	// through.
	int layout = 0;
};

inline HairPlan hair_plan(HairDetail d)
{
	HairPlan p;
	if (d == HairDetail::Full) {
		p.points_card = 2'000'000;
		p.points_cpu = 150'000;
		p.solid = 0.4f;
		p.budget_ms = 1.0f;
		p.pieces = 1'500'000;
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
constexpr float HAIR_LINE_STAYS = 1.1f, HAIR_LINE_COST = 0.5f, HAIR_SHARE_LEAST = 0.1f;

struct HairForm {
	HairShare share;
	// All its pieces as lines; or (of a part in two sorts) its thin ones.
	bool lines = false, thin = false;
};

// hair_share's answer and whether the part is sent as lines: `lines` the
// plan's say (HairPlan::lines), `was_lines` and `was_thin` what it was
// last frame.
inline HairForm hair_form(const Strands &h, size_t loaded, float scale, float distance, float pixel, float thinnest,
			  float solid, float budget, int lines, bool was_lines, bool was_thin = false)
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
	if (!(budget < 1) ||
	    hair_px(h, whole ? h.line_pixel_most : h.thin_pixel_most, f.share, scale, distance, pixel) <= 1.0f) {
		(whole ? f.lines : f.thin) = true;
		return f;
	}
	const float cost = whole ? HAIR_LINE_COST
				 : 1 - (1 - HAIR_LINE_COST) * std::min(1.0f, std::max(0.0f, h.thin_share));
	f.share = hair_share(h, loaded, scale, distance, pixel, thinnest, solid,
			     std::max(budget * cost, std::min(budget, HAIR_SHARE_LEAST)));
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
class HairBudget {
public:
	static constexpr float START = 0.25f, LEAST = HAIR_SHARE_LEAST;
	static constexpr size_t READINGS = 6; // how many it waits for after a change
	static constexpr size_t SILENT = 120; // frames with no reading before the clock counts as gone

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
	}
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
		if (took > limit_) {
			// Over: fewer at once (never less than half at a step: a
			// reading can be wrong).
			share_ = std::max(LEAST, share_ * std::max(0.5f, 0.9f * limit_ / took));
		} else if (took < 0.75f * limit_ && share_ < 1) {
			// Well under: more, up to a quarter more at a step, aiming
			// a little under the limit.
			const float room = took > 1e-4f ? 0.9f * limit_ / took : 1.25f;
			share_ = std::min(1.0f, share_ * std::min(1.25f, std::max(1.02f, room)));
		}
	}

private:
	float limit_ = 0, share_ = 1, measured_ = -1;
	size_t unread_ = 0;
	std::vector<float> readings_;
};

} // namespace direbot::face
