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

#include "meters.hpp"

#include <algorithm>
#include <cmath>

#include <util/platform.h>

#include "../link/protocol.hpp"

namespace direbot::obs {

using link::json;

namespace {

// No sound for this long (the input stopped sending any): read 0, as
// obs-websocket does.
constexpr uint64_t SILENT_AFTER_NS = 300'000'000;

float mul(float db)
{
	return std::isfinite(db) ? std::pow(10.0f, db / 20.0f) : 0.0f;
}

} // namespace

struct Meters::Meter {
	obs_weak_source_t *weak = nullptr;
	obs_volmeter_t *volmeter = nullptr;

	std::mutex lock;
	float magnitude[MAX_AUDIO_CHANNELS] = {};
	float peak[MAX_AUDIO_CHANNELS] = {};
	float input_peak[MAX_AUDIO_CHANNELS] = {};
	uint64_t updated = 0;

	explicit Meter(obs_source_t *input) : weak(obs_source_get_weak_source(input))
	{
		volmeter = obs_volmeter_create(OBS_FADER_LOG);
		obs_volmeter_add_callback(volmeter, &Meter::on_levels, this);
		obs_volmeter_attach_source(volmeter, input);
	}

	~Meter()
	{
		// Holding the input while the meter comes off, so it can't go
		// away half way through.
		obs_source_t *input = obs_weak_source_get_source(weak);
		obs_volmeter_remove_callback(volmeter, &Meter::on_levels, this);
		obs_volmeter_destroy(volmeter);
		obs_source_release(input);
		obs_weak_source_release(weak);
	}

	// OBS's audio thread, every few milliseconds: just a copy.
	static void on_levels(void *param, const float m[MAX_AUDIO_CHANNELS], const float p[MAX_AUDIO_CHANNELS],
			      const float ip[MAX_AUDIO_CHANNELS])
	{
		auto *self = static_cast<Meter *>(param);
		std::lock_guard<std::mutex> l(self->lock);
		std::copy(m, m + MAX_AUDIO_CHANNELS, self->magnitude);
		std::copy(p, p + MAX_AUDIO_CHANNELS, self->peak);
		std::copy(ip, ip + MAX_AUDIO_CHANNELS, self->input_peak);
		self->updated = os_gettime_ns();
	}
};

Meters::Meters() = default;

Meters::~Meters()
{
	detach();
}

bool Meters::wanted(obs_source_t *source)
{
	return source && obs_source_get_type(source) == OBS_SOURCE_TYPE_INPUT &&
	       (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) != 0;
}

// Holding lock_.
void Meters::add(obs_source_t *input)
{
	for (auto &m : meters_)
		if (obs_weak_source_references_source(m->weak, input))
			return;
	meters_.push_back(std::make_unique<Meter>(input));
}

void Meters::attach()
{
	if (attached_)
		return;
	attached_ = true;
	// Listening first, then looking: an input that comes on in between is
	// found twice, and add() skips the second.
	signal_handler_t *sh = obs_get_signal_handler();
	signal_handler_connect(sh, "source_activate", &Meters::on_activate, this);
	signal_handler_connect(sh, "source_deactivate", &Meters::on_deactivate, this);
	// Collected first and added after, so lock_ is never held inside
	// OBS's own source lock.
	std::vector<obs_source_t *> found;
	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			if (wanted(source) && obs_source_active(source)) {
				obs_source_t *ref = obs_source_get_ref(source);
				if (ref)
					static_cast<std::vector<obs_source_t *> *>(param)->push_back(ref);
			}
			return true;
		},
		&found);
	{
		std::lock_guard<std::mutex> l(lock_);
		for (obs_source_t *source : found)
			add(source);
	}
	for (obs_source_t *source : found)
		obs_source_release(source);
}

void Meters::detach()
{
	if (!attached_)
		return;
	attached_ = false;
	// Not holding lock_: a signal under way holds OBS's signal lock while
	// it waits for ours.
	signal_handler_t *sh = obs_get_signal_handler();
	signal_handler_disconnect(sh, "source_activate", &Meters::on_activate, this);
	signal_handler_disconnect(sh, "source_deactivate", &Meters::on_deactivate, this);
	std::vector<std::unique_ptr<Meter>> old;
	{
		std::lock_guard<std::mutex> l(lock_);
		old.swap(meters_);
	}
	old.clear();
}

void Meters::on_activate(void *self, calldata_t *cd)
{
	auto *source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	if (!wanted(source))
		return;
	auto *meters = static_cast<Meters *>(self);
	std::lock_guard<std::mutex> l(meters->lock_);
	meters->add(source);
}

void Meters::on_deactivate(void *self, calldata_t *cd)
{
	auto *source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	if (!wanted(source))
		return;
	auto *meters = static_cast<Meters *>(self);
	std::unique_ptr<Meter> gone;
	{
		std::lock_guard<std::mutex> l(meters->lock_);
		auto &list = meters->meters_;
		for (auto it = list.begin(); it != list.end(); ++it) {
			if (obs_weak_source_references_source((*it)->weak, source)) {
				gone = std::move(*it);
				list.erase(it);
				break;
			}
		}
	}
	// gone's meter comes off here, outside the lock.
}

std::string Meters::read()
{
	json inputs = json::array();
	const uint64_t now = os_gettime_ns();
	// Let go of after the lock: letting go of the last hold on an input
	// closes it, and that can signal.
	std::vector<obs_source_t *> held;
	{
		std::lock_guard<std::mutex> l(lock_);
		held.reserve(meters_.size());
		for (auto &m : meters_) {
			obs_source_t *input = obs_weak_source_get_source(m->weak);
			if (!input)
				continue;
			held.push_back(input);
			const int channels =
				std::clamp(obs_volmeter_get_nr_channels(m->volmeter), 0, MAX_AUDIO_CHANNELS);
			const bool muted = obs_source_muted(input);
			json levels = json::array();
			{
				std::lock_guard<std::mutex> ml(m->lock);
				const bool silent = !m->updated || now - m->updated > SILENT_AFTER_NS;
				for (int c = 0; c < channels; c++) {
					if (silent || muted)
						levels.push_back({0.0f, 0.0f, silent ? 0.0f : mul(m->input_peak[c])});
					else
						levels.push_back(
							{mul(m->magnitude[c]), mul(m->peak[c]), mul(m->input_peak[c])});
				}
			}
			const char *name = obs_source_get_name(input);
			const char *uuid = obs_source_get_uuid(input);
			inputs.push_back({{"inputName", name ? name : ""},
					  {"inputUuid", uuid ? uuid : ""},
					  {"inputLevelsMul", std::move(levels)}});
		}
	}
	for (obs_source_t *input : held)
		obs_source_release(input);
	return link::to_text(json({{"inputs", std::move(inputs)}}));
}

} // namespace direbot::obs
