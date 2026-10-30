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

#include "move-transition.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#include <graphics/vec4.h>

#include <plugin-support.h>

#include "plan.hpp"

namespace direbot::moves {

namespace {

struct Move {
	obs_source_t *context = nullptr;
	gs_effect_t *effect = nullptr;
	gs_eparam_t *image = nullptr, *opacity = nullptr;
	std::string easing = "smooth";
	std::string come = "fade";

	// Made when a transition starts (OBS's UI thread), drawn on the
	// graphics thread.
	std::mutex lock;
	std::vector<Step> steps;
	// One reference to each source in `steps`, by key.
	std::vector<obs_source_t *> held;
	bool planned = false;

	// Graphics thread only.
	std::vector<gs_texrender_t *> renders;
};

const char *get_name(void *)
{
	return obs_module_text("Move.Name");
}

void update(void *data, obs_data_t *settings)
{
	auto *m = static_cast<Move *>(data);
	std::lock_guard<std::mutex> l(m->lock);
	const char *e = obs_data_get_string(settings, "easing");
	const char *c = obs_data_get_string(settings, "come");
	m->easing = e && *e ? e : "smooth";
	m->come = c && *c ? c : "fade";
}

void *create(obs_data_t *settings, obs_source_t *source)
{
	auto *m = new Move();
	m->context = source;
	char *path = obs_module_file("effects/direbot-move.effect");
	obs_enter_graphics();
	if (path)
		m->effect = gs_effect_create_from_file(path, nullptr);
	if (m->effect) {
		m->image = gs_effect_get_param_by_name(m->effect, "image");
		m->opacity = gs_effect_get_param_by_name(m->effect, "opacity");
	}
	obs_leave_graphics();
	bfree(path);
	if (!m->effect)
		obs_log(LOG_WARNING, "DireBot Move: its effect file is missing, so it cuts instead.");
	update(m, settings);
	return m;
}

void let_go(Move *m)
{
	std::vector<obs_source_t *> held;
	{
		std::lock_guard<std::mutex> l(m->lock);
		held.swap(m->held);
		m->steps.clear();
		m->planned = false;
	}
	for (obs_source_t *s : held)
		obs_source_release(s);
}

void destroy(void *data)
{
	auto *m = static_cast<Move *>(data);
	let_go(m);
	obs_enter_graphics();
	for (gs_texrender_t *r : m->renders)
		gs_texrender_destroy(r);
	if (m->effect)
		gs_effect_destroy(m->effect);
	obs_leave_graphics();
	delete m;
}

void defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "easing", "smooth");
	obs_data_set_default_string(settings, "come", "fade");
}

obs_properties_t *properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *e = obs_properties_add_list(props, "easing", obs_module_text("Move.Easing"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	for (const char *id : {"smooth", "ease_out", "ease_in", "linear", "bounce"}) {
		const std::string key = std::string("Move.Ease.") + id;
		obs_property_list_add_string(e, obs_module_text(key.c_str()), id);
	}
	obs_property_t *c = obs_properties_add_list(props, "come", obs_module_text("Move.Come"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	for (const char *id : {"fade", "slide", "grow"}) {
		const std::string key = std::string("Move.Come.") + id;
		obs_property_list_add_string(c, obs_module_text(key.c_str()), id);
	}
	return props;
}

struct Gather {
	std::vector<Item> items;
	std::vector<obs_source_t *> *held;
};

// A scene's shown items, bottom first, with where each is drawn.
std::vector<Item> items_of(obs_source_t *scene_source, std::vector<obs_source_t *> &held)
{
	Gather g{{}, &held};
	obs_scene_t *scene = scene_source ? obs_scene_from_source(scene_source) : nullptr;
	if (!scene)
		return {};
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *g = static_cast<Gather *>(param);
			if (!obs_sceneitem_visible(item))
				return true;
			obs_source_t *src = obs_sceneitem_get_source(item);
			const uint32_t w = obs_source_get_width(src), h = obs_source_get_height(src);
			if (!src || !w || !h)
				return true;
			matrix4 m;
			obs_sceneitem_get_draw_transform(item, &m);
			obs_sceneitem_crop crop;
			obs_sceneitem_get_crop(item, &crop);
			Drawn d{m.x.x,
				m.x.y,
				m.y.x,
				m.y.y,
				m.t.x,
				m.t.y,
				(float)w,
				(float)h,
				(float)crop.left,
				(float)crop.top,
				(float)crop.right,
				(float)crop.bottom};
			g->items.push_back({(uintptr_t)src, pose_of(d), (float)w, (float)h});
			if (std::find(g->held->begin(), g->held->end(), src) == g->held->end())
				g->held->push_back(obs_source_get_ref(src));
			return true;
		},
		&g);
	return g.items;
}

void transition_start(void *data)
{
	auto *m = static_cast<Move *>(data);
	let_go(m);
	obs_source_t *a = obs_transition_get_source(m->context, OBS_TRANSITION_SOURCE_A);
	obs_source_t *b = obs_transition_get_source(m->context, OBS_TRANSITION_SOURCE_B);
	std::vector<obs_source_t *> held;
	const bool scenes = a && b && obs_scene_from_source(a) && obs_scene_from_source(b);
	std::vector<Step> steps;
	if (scenes) {
		const std::vector<Item> before = items_of(a, held);
		const std::vector<Item> after = items_of(b, held);
		std::string come;
		{
			std::lock_guard<std::mutex> l(m->lock);
			come = m->come;
		}
		steps = plan(before, after, come, (float)obs_source_get_width(m->context),
			     (float)obs_source_get_height(m->context));
	}
	obs_source_release(a);
	obs_source_release(b);
	std::lock_guard<std::mutex> l(m->lock);
	m->steps = std::move(steps);
	m->held = std::move(held);
	m->planned = scenes;
}

void transition_stop(void *data)
{
	let_go(static_cast<Move *>(data));
}

obs_source_t *held_source(const Move *m, uintptr_t key)
{
	for (obs_source_t *s : m->held)
		if ((uintptr_t)s == key)
			return s;
	return nullptr;
}

// One source, cropped, at a pose.
void draw(Move *m, size_t slot, obs_source_t *src, const Step &step, const Pose &p)
{
	if (p.alpha <= 0.002f || std::fabs(p.w) < 0.5f || std::fabs(p.h) < 0.5f)
		return;
	const float w = step.src_w, h = step.src_h;
	const float cw = std::max(1.0f, w - p.crop_l - p.crop_r);
	const float ch = std::max(1.0f, h - p.crop_t - p.crop_b);
	while (m->renders.size() <= slot)
		m->renders.push_back(gs_texrender_create(GS_RGBA, GS_ZS_NONE));
	gs_texrender_t *r = m->renders[slot];
	gs_texrender_reset(r);
	if (!gs_texrender_begin(r, (uint32_t)cw, (uint32_t)ch))
		return;
	vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	// As OBS draws a cropped item: the whole source, moved so the part
	// that's kept fills the picture.
	gs_ortho(0.0f, w, 0.0f, h, -100.0f, 100.0f);
	gs_matrix_scale3f(w / cw, h / ch, 1.0f);
	gs_matrix_translate3f(-p.crop_l, -p.crop_t, 0.0f);
	gs_blend_state_push();
	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	obs_source_video_render(src);
	gs_blend_state_pop();
	gs_texrender_end(r);

	gs_texture_t *tex = gs_texrender_get_texture(r);
	if (!tex)
		return;
	float ax, ay, bx, by, tx, ty;
	matrix_of(p, w, h, ax, ay, bx, by, tx, ty);
	matrix4 mat;
	matrix4_identity(&mat);
	vec4_set(&mat.x, ax, ay, 0, 0);
	vec4_set(&mat.y, bx, by, 0, 0);
	vec4_set(&mat.t, tx, ty, 0, 1);
	gs_matrix_push();
	gs_matrix_mul(&mat);
	gs_effect_set_texture(m->image, tex);
	gs_effect_set_float(m->opacity, p.alpha);
	gs_blend_state_push();
	gs_blend_function_separate(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(m->effect, "Draw"))
		gs_draw_sprite(tex, 0, (uint32_t)cw, (uint32_t)ch);
	gs_blend_state_pop();
	gs_matrix_pop();
}

// Not two scenes: a plain fade.
void fade(void *data, gs_texture_t *a, gs_texture_t *b, float t, uint32_t cx, uint32_t cy)
{
	auto *m = static_cast<Move *>(data);
	gs_blend_state_push();
	gs_blend_function_separate(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	for (int i = 0; i < 2; i++) {
		gs_texture_t *tex = i ? b : a;
		if (!tex)
			continue;
		gs_effect_set_texture(m->image, tex);
		gs_effect_set_float(m->opacity, i ? t : 1.0f - t);
		while (gs_effect_loop(m->effect, "Draw"))
			gs_draw_sprite(tex, 0, cx, cy);
	}
	gs_blend_state_pop();
}

void video_render(void *data, gs_effect_t *)
{
	auto *m = static_cast<Move *>(data);
	if (!m->effect) {
		obs_transition_video_render_direct(m->context, OBS_TRANSITION_SOURCE_B);
		return;
	}
	std::unique_lock<std::mutex> l(m->lock);
	if (!m->planned) {
		l.unlock();
		obs_transition_video_render(m->context, fade);
		return;
	}
	const float t = ease(m->easing, obs_transition_get_time(m->context));
	size_t slot = 0;
	for (const Step &step : m->steps) {
		obs_source_t *src = held_source(m, step.key);
		if (src)
			draw(m, slot++, src, step, between(step.from, step.to, t));
	}
}

float mix(void *, float t)
{
	return t;
}

bool audio_render(void *data, uint64_t *ts_out, obs_source_audio_mix *audio, uint32_t mixers, size_t channels,
		  size_t sample_rate)
{
	auto *m = static_cast<Move *>(data);
	return obs_transition_audio_render(m->context, ts_out, audio, mixers, channels, sample_rate,
					   [](void *, float t) { return 1.0f - t; }, mix);
}

} // namespace

void register_transition()
{
	obs_source_info info = {};
	info.id = TRANSITION_ID;
	info.type = OBS_SOURCE_TYPE_TRANSITION;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_COMPOSITE;
	info.get_name = get_name;
	info.create = create;
	info.destroy = destroy;
	info.update = update;
	info.get_defaults = defaults;
	info.get_properties = properties;
	info.video_render = video_render;
	info.audio_render = audio_render;
	info.transition_start = transition_start;
	info.transition_stop = transition_stop;
	obs_register_source(&info);
}

} // namespace direbot::moves
