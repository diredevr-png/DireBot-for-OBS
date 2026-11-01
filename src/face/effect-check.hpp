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

#pragma once

#include <string>
#include <utility>
#include <vector>

#include <graphics/graphics.h>

#include <plugin-support.h>

#include "effect-needs.hpp"

namespace direbot::face {

// Called just before each technique of an effect: says once (in OBS's
// log) if a value the technique reads isn't set, since Windows won't draw
// it then (see effect-needs.hpp). The real-OBS tests fail on that line.
class EffectCheck {
public:
	void load(gs_effect_t *effect, const char *what, const std::vector<EffectNeeds> &needs)
	{
		what_ = what;
		needs_.clear();
		for (const EffectNeeds &n : needs) {
			Needs got;
			got.technique = n.technique;
			for (const std::string &name : n.values)
				got.values.emplace_back(name, gs_effect_get_param_by_name(effect, name.c_str()));
			needs_.push_back(std::move(got));
		}
	}

	void operator()(const char *technique)
	{
		for (Needs &n : needs_) {
			if (n.technique != technique || n.said)
				continue;
			std::string missing;
			for (const auto &value : n.values)
				if (!value.second || !gs_effect_get_val_size(value.second))
					missing += (missing.empty() ? "" : ", ") + value.first;
			if (!missing.empty()) {
				n.said = true;
				obs_log(LOG_WARNING, "DireBot Face Avatar: %s %s is missing %s (Windows won't draw it)",
					what_.c_str(), technique, missing.c_str());
			}
		}
	}

private:
	struct Needs {
		std::string technique;
		std::vector<std::pair<std::string, gs_eparam_t *>> values;
		bool said = false;
	};
	std::string what_;
	std::vector<Needs> needs_;
};

} // namespace direbot::face
