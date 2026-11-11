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
	static constexpr float START = 0.25f, LEAST = 0.1f;
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
