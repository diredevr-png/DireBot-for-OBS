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

#include "settings-window.hpp"

#include <atomic>
#include <list>
#include <mutex>

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <plugin-support.h>

namespace direbot::obs {

struct SettingsWindow::Shared {
	std::string id;
	std::string title;
	// Only touched on the UI thread; null once stopped.
	SettingsPage *page = nullptr;
	std::mutex lock;
	obs_source_t *source = nullptr; // ours, under `lock`
	std::atomic<bool> showing{false};
	obs_source_info info = {};
};

namespace {

using SharedPtr = std::shared_ptr<SettingsWindow::Shared>;

// A Tools menu item can't be taken out of OBS's menu again, so what it
// points at stays as long as the plugin is loaded.
std::list<SharedPtr> &menu_slots()
{
	static std::list<SharedPtr> slots;
	return slots;
}

obs_source_t *source_of(SettingsWindow::Shared &s)
{
	std::lock_guard<std::mutex> l(s.lock);
	return s.source ? obs_source_get_ref(s.source) : nullptr;
}

struct Task {
	SharedPtr shared;
	std::function<void(SettingsWindow::Shared &)> work;
};

void run_task(void *param)
{
	std::unique_ptr<Task> task(static_cast<Task *>(param));
	task->work(*task->shared);
}

void to_ui(void *param)
{
	obs_queue_task(OBS_TASK_UI, run_task, param, false);
}

// Later, on OBS's UI thread. Asked from the UI thread itself, OBS runs a
// UI task straight away, inside the caller (which may hold a lock the
// page's changed() takes), so every task goes by way of the graphics
// thread first: always later, and always in order.
void on_ui(const SharedPtr &shared, std::function<void(SettingsWindow::Shared &)> work)
{
	obs_queue_task(OBS_TASK_GRAPHICS, to_ui, new Task{shared, std::move(work)}, false);
}

SettingsWindow::Shared &shared_of(void *data)
{
	return **static_cast<SharedPtr *>(data);
}

const char *get_name(void *type_data)
{
	return shared_of(type_data).title.c_str();
}

void *create(obs_data_t *, obs_source_t *source)
{
	return obs_source_get_type_data(source);
}

void destroy(void *) {}

void free_type_data(void *type_data)
{
	delete static_cast<SharedPtr *>(type_data);
}

void get_defaults(void *type_data, obs_data_t *settings)
{
	auto &s = shared_of(type_data);
	if (s.page)
		s.page->defaults(settings);
}

obs_properties_t *get_properties(void *data, void *)
{
	auto &s = shared_of(data);
	obs_source_t *source = source_of(s);
	if (!source || !s.page) {
		obs_source_release(source);
		return obs_properties_create();
	}
	obs_data_t *settings = obs_source_get_settings(source);
	obs_properties_t *props = s.page->properties(settings);
	obs_data_release(settings);
	obs_source_release(source);
	return props ? props : obs_properties_create();
}

void update(void *data, obs_data_t *settings)
{
	auto &s = shared_of(data);
	if (s.page)
		s.page->changed(settings);
}

// Opening and closing OBS's Properties window shows and hides the object
// (OBS tells it on its graphics thread): the page hears on the UI thread.
void shown(void *data, bool open)
{
	SharedPtr shared = *static_cast<SharedPtr *>(data);
	shared->showing = open;
	on_ui(shared, [open](SettingsWindow::Shared &s) {
		obs_source_t *source = source_of(s);
		if (!source || !s.page) {
			obs_source_release(source);
			return;
		}
		obs_data_t *settings = obs_source_get_settings(source);
		bool redraw = false;
		if (open)
			redraw = s.page->opened(settings);
		else
			s.page->closed(settings);
		obs_data_release(settings);
		if (redraw)
			obs_source_update_properties(source);
		obs_source_release(source);
	});
}

void show(void *data)
{
	shown(data, true);
}

void hide(void *data)
{
	shown(data, false);
}

void menu_clicked(void *data)
{
	SharedPtr shared = *static_cast<SharedPtr *>(data);
	obs_source_t *source = source_of(*shared);
	if (source && shared->page)
		obs_frontend_open_source_properties(source);
	obs_source_release(source);
}

} // namespace

SettingsWindow::SettingsWindow(const std::string &id, const std::string &title, SettingsPage *page)
	: shared_(std::make_shared<Shared>())
{
	shared_->id = id;
	shared_->title = title;
	shared_->page = page;
}

SettingsWindow::~SettingsWindow()
{
	stop();
}

bool SettingsWindow::start()
{
	Shared &s = *shared_;
	if (obs_source_t *have = source_of(s)) {
		obs_source_release(have);
		return true;
	}
	if (!s.info.id) {
		s.info.id = s.id.c_str();
		s.info.type = OBS_SOURCE_TYPE_INPUT;
		// Never offered under "Add source"; nothing to see or hear.
		s.info.output_flags = OBS_SOURCE_CAP_DISABLED | OBS_SOURCE_DO_NOT_DUPLICATE;
		s.info.get_name = get_name;
		s.info.create = create;
		s.info.destroy = destroy;
		s.info.get_defaults2 = get_defaults;
		s.info.get_properties2 = get_properties;
		s.info.update = update;
		s.info.show = show;
		s.info.hide = hide;
		s.info.type_data = new SharedPtr(shared_);
		s.info.free_type_data = free_type_data;
		obs_register_source(&s.info);
	}
	obs_source_t *source = obs_source_create_private(s.id.c_str(), s.title.c_str(), nullptr);
	if (!source) {
		obs_log(LOG_WARNING, "Couldn't make the settings for \"%s\".", s.title.c_str());
		return false;
	}
	{
		std::lock_guard<std::mutex> l(s.lock);
		s.source = source;
	}
	menu_slots().push_back(shared_);
	obs_frontend_add_tools_menu_item(s.title.c_str(), menu_clicked, &menu_slots().back());
	return true;
}

void SettingsWindow::stop()
{
	Shared &s = *shared_;
	s.page = nullptr;
	obs_source_t *source = nullptr;
	{
		std::lock_guard<std::mutex> l(s.lock);
		source = s.source;
		s.source = nullptr;
	}
	obs_source_release(source);
}

void SettingsWindow::open(std::function<void(obs_data_t *)> before)
{
	on_ui(shared_, [before = std::move(before)](Shared &s) {
		obs_source_t *source = source_of(s);
		if (!source || !s.page) {
			obs_source_release(source);
			return;
		}
		if (before) {
			obs_data_t *settings = obs_source_get_settings(source);
			before(settings);
			obs_data_release(settings);
			// Already open: drawn again for what `before` chose.
			if (s.showing)
				obs_source_update_properties(source);
		}
		obs_frontend_open_source_properties(source);
		obs_source_release(source);
	});
}

void SettingsWindow::change(std::function<void(obs_data_t *)> edit)
{
	on_ui(shared_, [edit = std::move(edit)](Shared &s) {
		obs_source_t *source = source_of(s);
		if (!source || !s.page) {
			obs_source_release(source);
			return;
		}
		obs_data_t *settings = obs_source_get_settings(source);
		edit(settings);
		// The settings are the object's own, so this only tells the page.
		obs_source_update(source, settings);
		obs_data_release(settings);
		if (s.showing)
			obs_source_update_properties(source);
		obs_source_release(source);
	});
}

bool SettingsWindow::is_open() const
{
	return shared_->showing;
}

} // namespace direbot::obs
