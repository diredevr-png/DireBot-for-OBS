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

#include "companion.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace direbot::face {

namespace {

float clamp01(float v)
{
	return std::isfinite(v) ? std::max(0.0f, std::min(1.0f, v)) : 0.0f;
}
float smooth(float u)
{
	u = clamp01(u);
	return u * u * (3 - 2 * u);
}
float kept(float v, float lo, float hi, float fallback)
{
	return std::isfinite(v) ? std::max(lo, std::min(hi, v)) : fallback;
}
std::string lowered(const std::string &s)
{
	std::string out;
	out.reserve(s.size());
	for (char c : s)
		out.push_back((char)std::tolower((unsigned char)c));
	return out;
}
// A name's words, lower case: split where it isn't a letter or digit, and
// where a small letter meets a capital ("WolfCome" -> wolf, come).
std::vector<std::string> words_of(const std::string &name)
{
	std::vector<std::string> out;
	std::string word;
	char before = 0;
	for (char c : name) {
		const unsigned char u = (unsigned char)c;
		if (!std::isalnum(u)) {
			if (!word.empty())
				out.push_back(word);
			word.clear();
		} else {
			if (std::isupper(u) && before && std::islower((unsigned char)before) && !word.empty()) {
				out.push_back(word);
				word.clear();
			}
			word.push_back((char)std::tolower(u));
		}
		before = c;
	}
	if (!word.empty())
		out.push_back(word);
	return out;
}
// A clip's time over and over (a loop), from 0 to under its length.
float looped(float t, float length)
{
	if (!(length > 0) || !std::isfinite(t))
		return 0;
	const float r = std::fmod(t, length);
	return r < 0 ? r + length : r;
}

const char *const CLIP_NAMES[COMPANION_CLIPS] = {"come", "idle", "pet", "go", "scared"};
// Each kind's usual names, its own first.
// clang-format off
const std::vector<std::vector<std::string>> USUAL = {
	{"come", "arrive", "enter", "in"},
	{"idle", "sit", "loop", "breathe"},
	{"pet", "pat", "happy"},
	{"go", "leave", "exit", "out"},
	{"scared", "flee", "run", "bolt"},
};
// clang-format on

// How long a move eases in and out (as react.hpp's moves do).
float ease_in_of(float length)
{
	return move_ease_in(length);
}
float ease_out_of(float length)
{
	return move_ease_out(length);
}

} // namespace

const char *companion_clip_name(int kind)
{
	return kind >= 0 && kind < COMPANION_CLIPS ? CLIP_NAMES[kind] : "";
}

const char *companion_phase_name(CompanionPhase p)
{
	switch (p) {
	case CompanionPhase::Coming:
		return "coming";
	case CompanionPhase::Out:
		return "out";
	case CompanionPhase::Going:
		return "going";
	default:
		return "away";
	}
}

CompanionClips companion_clips(const Model &m, const std::string asked[COMPANION_CLIPS])
{
	CompanionClips out;
	const int n = (int)m.animations.size();
	std::vector<char> taken(n, 0);
	auto set = [&](int kind, int a) {
		out.clip[kind].index = a;
		out.clip[kind].length =
			std::max(0.0f, std::isfinite(m.animations[a].length) ? m.animations[a].length : 0);
		taken[a] = 1;
	};
	// As asked (any case): the one so named.
	for (int k = 0; k < COMPANION_CLIPS; k++) {
		if (!asked || asked[k].empty())
			continue;
		const std::string want = lowered(asked[k]);
		for (int a = 0; a < n; a++)
			if (lowered(m.animations[a].name) == want) {
				set(k, a);
				break;
			}
	}
	if (n == 1 && out.clip[CLIP_IDLE].index < 0 && !taken[0]) {
		bool any = false;
		for (int k = 0; k < COMPANION_CLIPS; k++)
			any = any || out.clip[k].index >= 0;
		if (!any) {
			set(CLIP_IDLE, 0);
			return out;
		}
	}
	// The rest by their usual names: first a name that's just the word,
	// then one with it among its words - each kind's own word before the
	// others, so "run_in" is a come only if nothing's called "come".
	std::vector<std::vector<std::string>> words(n);
	std::vector<std::string> whole(n);
	for (int a = 0; a < n; a++) {
		words[a] = words_of(m.animations[a].name);
		whole[a] = lowered(m.animations[a].name);
	}
	size_t most = 0;
	for (const auto &w : USUAL)
		most = std::max(most, w.size());
	for (int pass = 0; pass < 2; pass++)
		for (size_t rank = 0; rank < most; rank++)
			for (int k = 0; k < COMPANION_CLIPS; k++) {
				if (out.clip[k].index >= 0 || rank >= USUAL[k].size())
					continue;
				const std::string &word = USUAL[k][rank];
				for (int a = 0; a < n; a++) {
					if (taken[a])
						continue;
					const bool hit = pass == 0 ? whole[a] == word
								   : std::find(words[a].begin(), words[a].end(),
									       word) != words[a].end();
					if (hit) {
						set(k, a);
						break;
					}
				}
			}
	return out;
}

bool CompanionPlayer::come(bool instant)
{
	if (instant) {
		const bool was_out = phase_ == CompanionPhase::Out && !go_after_;
		phase_ = CompanionPhase::Out;
		t_ = 0;
		idle_t_ = 0;
		come_after_ = go_after_ = false;
		if (!was_out)
			petting_ = pet_again_ = false;
		return !was_out;
	}
	switch (phase_) {
	case CompanionPhase::Out:
		return false;
	case CompanionPhase::Coming:
		// Asked to go once it's here: it stays after all.
		if (go_after_) {
			go_after_ = false;
			return true;
		}
		return false;
	case CompanionPhase::Going:
		// It comes back once it's gone.
		if (come_after_)
			return false;
		come_after_ = true;
		return true;
	default:
		break;
	}
	petting_ = pet_again_ = false;
	start_coming();
	return true;
}

void CompanionPlayer::start_coming()
{
	t_ = 0;
	idle_t_ = 0;
	come_after_ = go_after_ = false;
	if (clips_[CLIP_COME].index >= 0 && clips_[CLIP_COME].length > 0) {
		phase_ = CompanionPhase::Coming;
	} else {
		phase_ = CompanionPhase::Out;
	}
}

bool CompanionPlayer::go(bool scared, bool instant)
{
	if (instant) {
		const bool was_away = phase_ == CompanionPhase::Away && !come_after_;
		reset();
		return !was_away;
	}
	switch (phase_) {
	case CompanionPhase::Away:
		return false;
	case CompanionPhase::Going:
		// Asked to come back after: it stays gone after all.
		if (come_after_) {
			come_after_ = false;
			return true;
		}
		return false;
	case CompanionPhase::Coming:
		// It goes once it's here (no jump from half way in).
		if (go_after_)
			return false;
		go_after_ = true;
		go_scared_ = scared;
		return true;
	default:
		break;
	}
	start_going(scared);
	return true;
}

void CompanionPlayer::start_going(bool scared)
{
	go_after_ = false;
	going_ = scared && clips_[CLIP_SCARED].index >= 0 && clips_[CLIP_SCARED].length > 0 ? CLIP_SCARED : CLIP_GO;
	if (clips_[going_].index < 0 || !(clips_[going_].length > 0)) {
		// Neither: straight away.
		const bool back = come_after_;
		reset();
		if (back)
			start_coming();
		return;
	}
	phase_ = CompanionPhase::Going;
	t_ = 0;
	pet_again_ = false;
}

bool CompanionPlayer::pet()
{
	if (phase_ != CompanionPhase::Out || go_after_)
		return false;
	if (clips_[CLIP_PET].index < 0 || !(clips_[CLIP_PET].length > 0))
		return true;
	if (petting_) {
		// Patted again while it's still being patted: once more after.
		pet_again_ = true;
		return true;
	}
	petting_ = true;
	pet_t_ = 0;
	return true;
}

void CompanionPlayer::reset()
{
	phase_ = CompanionPhase::Away;
	t_ = idle_t_ = pet_t_ = 0;
	going_ = CLIP_GO;
	petting_ = pet_again_ = false;
	come_after_ = go_after_ = go_scared_ = false;
}

CompanionNow CompanionPlayer::step(float dt)
{
	const float s = std::isfinite(dt) ? std::max(0.0f, std::min(dt, 1.0f)) : 0.0f;
	// A long step may finish more than one thing: each in turn.
	float left = s;
	for (int guard = 0; guard < 8; guard++) {
		if (phase_ == CompanionPhase::Coming) {
			const float L = clips_[CLIP_COME].length;
			const float idle_from = L - ease_out_of(L);
			const float before = t_;
			const float to = t_ + left;
			if (to >= idle_from && before < idle_from)
				idle_t_ = looped(clips_[CLIP_IDLE].length - ease_out_of(L) + (to - idle_from),
						 clips_[CLIP_IDLE].length);
			else if (before >= idle_from)
				idle_t_ = looped(idle_t_ + left, clips_[CLIP_IDLE].length);
			if (to < L) {
				t_ = to;
				left = 0;
				break;
			}
			// Here: sitting, its idle on from where it's got to.
			left = to - L;
			phase_ = CompanionPhase::Out;
			t_ = 0;
			idle_t_ = looped(left, clips_[CLIP_IDLE].length);
			if (go_after_) {
				start_going(go_scared_);
				continue;
			}
			break;
		}
		if (phase_ == CompanionPhase::Going) {
			const float L = clips_[going_].length;
			idle_t_ = looped(idle_t_ + left, clips_[CLIP_IDLE].length);
			if (petting_) {
				pet_t_ += left;
				if (pet_t_ >= clips_[CLIP_PET].length)
					petting_ = false;
			}
			if (t_ + left < L) {
				t_ += left;
				left = 0;
				break;
			}
			left = t_ + left - L;
			const bool back = come_after_;
			reset();
			if (back) {
				start_coming();
				continue;
			}
			break;
		}
		if (phase_ == CompanionPhase::Out) {
			idle_t_ = looped(idle_t_ + left, clips_[CLIP_IDLE].length);
			if (petting_) {
				pet_t_ += left;
				const float L = clips_[CLIP_PET].length;
				if (pet_t_ >= L) {
					if (pet_again_) {
						pet_again_ = false;
						pet_t_ = std::min(pet_t_ - L, L);
					} else {
						petting_ = false;
						pet_t_ = 0;
					}
				}
			}
		}
		left = 0;
		break;
	}
	CompanionNow out;
	out.phase = phase_;
	out.shown = phase_ != CompanionPhase::Away;
	out.busy = phase_ == CompanionPhase::Coming || phase_ == CompanionPhase::Going;
	out.petting = petting_;
	out.camera = phase_ == CompanionPhase::Coming || phase_ == CompanionPhase::Out ||
		     (phase_ == CompanionPhase::Going && (come_after_ || t_ < clips_[going_].length * 0.5f));
	moves(out);
	return out;
}

void CompanionPlayer::moves(CompanionNow &out) const
{
	const CompanionClip &idle = clips_[CLIP_IDLE];
	auto add = [&out](const CompanionClip &c, float t, float amount) {
		if (c.index < 0 || !(amount > 0))
			return;
		MoveNow m;
		m.animation = c.index;
		m.time = std::max(0.0f, std::min(t, c.length));
		m.amount = clamp01(amount);
		out.moves.push_back(m);
	};
	switch (phase_) {
	case CompanionPhase::Coming: {
		const CompanionClip &come = clips_[CLIP_COME];
		add(come, t_, 1);
		const float fo = ease_out_of(come.length);
		const float from = come.length - fo;
		if (t_ >= from)
			add(idle, idle_t_, smooth((t_ - from) / std::max(1e-4f, fo)));
		break;
	}
	case CompanionPhase::Out:
	case CompanionPhase::Going: {
		add(idle, idle_t_, 1);
		if (petting_) {
			const CompanionClip &pet = clips_[CLIP_PET];
			const float fi = ease_in_of(pet.length), fo = ease_out_of(pet.length);
			float k = 1;
			if (pet_t_ < fi)
				k = smooth(pet_t_ / fi);
			else if (pet_t_ > pet.length - fo)
				k = smooth((pet.length - pet_t_) / fo);
			add(pet, pet_t_, k);
		}
		if (phase_ == CompanionPhase::Going) {
			const CompanionClip &go = clips_[going_];
			const float fi = ease_in_of(go.length);
			add(go, t_, t_ < fi ? smooth(t_ / fi) : 1.0f);
		}
		break;
	}
	default:
		break;
	}
}

CompanionPlace sensible_place(const CompanionPlace &p)
{
	CompanionPlace out;
	out.size = kept(p.size, 0.05f, 3, 0.7f);
	out.side = std::isfinite(p.side) && p.side > 0 ? 1.0f : -1.0f;
	out.made_for = std::isfinite(p.made_for) && p.made_for > 0 ? 1.0f : -1.0f;
	out.distance = kept(p.distance, -3, 3, 0.32f);
	out.forward = kept(p.forward, -3, 3, 0);
	out.turn = kept(p.turn, -180, 180, 0);
	return out;
}

namespace {
// A box turned by `q`: the box round its turned corners.
void turned_box(const Bounds &b, Quat q, V3 &lo, V3 &hi)
{
	lo = {1e30f, 1e30f, 1e30f};
	hi = {-1e30f, -1e30f, -1e30f};
	for (int i = 0; i < 8; i++) {
		const V3 c{(i & 1) ? b.high.x : b.low.x, (i & 2) ? b.high.y : b.low.y, (i & 4) ? b.high.z : b.low.z};
		const V3 r = rotate(q, c);
		lo = {std::min(lo.x, r.x), std::min(lo.y, r.y), std::min(lo.z, r.z)};
		hi = {std::max(hi.x, r.x), std::max(hi.y, r.y), std::max(hi.z, r.z)};
	}
}
} // namespace

M4 companion_place(const Bounds &its_box, Quat its_facing, const Bounds &stands, Quat facing, const M4 &rest_place,
		   const CompanionPlace &place)
{
	const CompanionPlace p = sensible_place(place);
	// The character, facing +z: its floor, middle and height.
	V3 cl{-0.25f, 0, -0.15f}, ch{0.25f, 1.7f, 0.15f};
	if (stands.found)
		turned_box(stands, facing, cl, ch);
	const float height = std::max(1e-4f, ch.y - cl.y);
	// It, facing +z: its bottom's middle and its height.
	V3 il{-0.25f, 0, -0.25f}, ih{0.25f, 1, 0.25f};
	if (its_box.found)
		turned_box(its_box, its_facing, il, ih);
	const float its_height = std::max(1e-6f, ih.y - il.y);
	const V3 anchor{(il.x + ih.x) * 0.5f, il.y, (il.z + ih.z) * 0.5f};
	const float k = p.size * height / its_height;
	const V3 spot{(cl.x + ch.x) * 0.5f + p.side * p.distance * height, cl.y,
		      (cl.z + ch.z) * 0.5f + p.forward * height};
	const float D = 3.14159265f / 180;
	// Turned towards the character on the side it's made for; then, on the
	// other side, all of it mirrored (its turn too).
	const M4 turn = matrix_of(axis_angle({0, 1, 0}, -p.made_for * p.turn * D));
	M4 mirror = scaled(1);
	if (p.side != p.made_for)
		mirror.m[0] = -1;
	return rest_place * matrix_of(conj(facing)) * moved(spot) * mirror * turn * scaled(k) * moved(anchor * -1) *
	       matrix_of(its_facing);
}

Quat companion_facing(const Model &m)
{
	return m.faces_back ? axis_angle({0, 1, 0}, 3.14159265f) : Quat{};
}

int companion_pat_node(const Model &m)
{
	for (size_t i = 0; i < m.nodes.size(); i++)
		if (lowered(m.nodes[i].name) == "pat")
			return (int)i;
	const int head = m.bone("head");
	if (head >= 0 && head < (int)m.nodes.size())
		return head;
	for (size_t i = 0; i < m.nodes.size(); i++)
		if (lowered(m.nodes[i].name) == "head")
			return (int)i;
	// A head by its last word ("mixamorig:Head", "Wolf_Head").
	for (size_t i = 0; i < m.nodes.size(); i++) {
		const std::vector<std::string> w = words_of(m.nodes[i].name);
		if (!w.empty() && w.back() == "head")
			return (int)i;
	}
	return -1;
}

} // namespace direbot::face
