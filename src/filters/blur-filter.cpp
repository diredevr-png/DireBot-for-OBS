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

#include "blur-filter.hpp"

#include <algorithm>
#include <cstring>

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>

#include <plugin-support.h>

namespace direbot::filters {

namespace {

struct Blur {
	obs_source_t *context = nullptr;
	gs_effect_t *effect = nullptr;
	gs_eparam_t *texel = nullptr, *radius = nullptr, *block = nullptr;
	bool pixelate = false;
	float amount = 24.0f;
};

const char *get_name(void *)
{
	return obs_module_text("Blur.Name");
}

void update(void *data, obs_data_t *settings)
{
	auto *b = static_cast<Blur *>(data);
	const char *mode = obs_data_get_string(settings, "mode");
	b->pixelate = mode && std::strcmp(mode, "pixelate") == 0;
	b->amount = (float)std::clamp<double>(obs_data_get_double(settings, "amount"), 0.0, 100.0);
}

void *create(obs_data_t *settings, obs_source_t *source)
{
	auto *b = new Blur();
	b->context = source;
	char *path = obs_module_file("effects/direbot-blur.effect");
	obs_enter_graphics();
	if (path)
		b->effect = gs_effect_create_from_file(path, nullptr);
	if (b->effect) {
		b->texel = gs_effect_get_param_by_name(b->effect, "texel");
		b->radius = gs_effect_get_param_by_name(b->effect, "radius");
		b->block = gs_effect_get_param_by_name(b->effect, "block");
	}
	obs_leave_graphics();
	bfree(path);
	if (!b->effect)
		obs_log(LOG_WARNING, "DireBot Blur: its effect file is missing, so it shows the source as it is.");
	update(b, settings);
	return b;
}

void destroy(void *data)
{
	auto *b = static_cast<Blur *>(data);
	if (b->effect) {
		obs_enter_graphics();
		gs_effect_destroy(b->effect);
		obs_leave_graphics();
	}
	delete b;
}

void defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "mode", "blur");
	obs_data_set_default_double(settings, "amount", 24.0);
}

obs_properties_t *properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *mode = obs_properties_add_list(props, "mode", obs_module_text("Blur.Mode"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(mode, obs_module_text("Blur.Blur"), "blur");
	obs_property_list_add_string(mode, obs_module_text("Blur.Pixelate"), "pixelate");
	obs_properties_add_float_slider(props, "amount", obs_module_text("Blur.Amount"), 0.0, 100.0, 1.0);
	return props;
}

void render(void *data, gs_effect_t *)
{
	auto *b = static_cast<Blur *>(data);
	obs_source_t *target = obs_filter_get_target(b->context);
	const uint32_t w = target ? obs_source_get_base_width(target) : 0;
	const uint32_t h = target ? obs_source_get_base_height(target) : 0;
	// Nothing to do (or nothing to do it with): the source as it is.
	if (!b->effect || !w || !h || b->amount < 0.5f) {
		obs_source_skip_video_filter(b->context);
		return;
	}
	if (!obs_source_process_filter_begin(b->context, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING))
		return;
	// "amount" is pixels on a 1080-tall picture.
	const float scale = (float)std::min(w, h) / 1080.0f;
	vec2 texel;
	vec2_set(&texel, 1.0f / (float)w, 1.0f / (float)h);
	gs_effect_set_vec2(b->texel, &texel);
	gs_effect_set_float(b->radius, std::max(1.0f, b->amount * scale));
	gs_effect_set_float(b->block, std::max(2.0f, b->amount * scale));
	obs_source_process_filter_tech_end(b->context, b->effect, w, h, b->pixelate ? "Pixelate" : "Blur");
}

} // namespace

void register_blur()
{
	obs_source_info info = {};
	info.id = BLUR_ID;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = get_name;
	info.create = create;
	info.destroy = destroy;
	info.update = update;
	info.get_defaults = defaults;
	info.get_properties = properties;
	info.video_render = render;
	obs_register_source(&info);
}

} // namespace direbot::filters
