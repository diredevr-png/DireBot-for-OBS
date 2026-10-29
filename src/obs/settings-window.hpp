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

// A settings window inside OBS, for what only OBS should hold - stream
// keys above all. It's OBS's own Properties window (the one a source's
// Properties opens) for a hidden settings object: no scene has it, no
// scene collection saves it, and "Add source" doesn't list it. So there's
// no Qt to build, and a key box is OBS's own password box with its Show
// button.
//
// It opens from OBS's Tools menu, or when DireBot asks (a vendor request)
// - which only opens it: what's typed in it stays in OBS.
//
// OK keeps the changes; Cancel puts back what was there when it opened
// (OBS does that, and calls changed() with it). OK also puts the window's
// settings - all of them, as they were when it opened and as OK left them
// - on OBS's Edit → Undo list: Undo / Redo later call changed() with
// those, plus an "undo_uuid", while the window is shut.
//
// Threads: everything a page is called with happens on OBS's UI thread.
// open() and change() can be called from any thread, the UI thread
// included (holding a lock changed() takes is fine): they never wait, and
// never run before the caller is done.

#pragma once

#include <functional>
#include <memory>
#include <string>

#include <obs.h>

namespace direbot::obs {

// What a window shows and what it does with the answers.
class SettingsPage {
public:
	virtual ~SettingsPage() = default;
	// What the window shows, for the settings as they are now: read them
	// to show or hide parts (don't change them here).
	virtual obs_properties_t *properties(obs_data_t *settings) = 0;
	// After every change in the window (and after Cancel, with what was
	// there when it opened), and after change().
	virtual void changed(obs_data_t *settings) = 0;
	virtual void defaults(obs_data_t *) {}
	// The window opened: put in what only belongs there while it's open
	// (the keys). True when the window should be drawn again for it.
	virtual bool opened(obs_data_t *) { return false; }
	// It closed: take that out again.
	virtual void closed(obs_data_t *) {}
};

class SettingsWindow {
public:
	// `id`: the hidden settings object's type, unique in OBS
	// ("direbot_multistream_settings"). `title`: the window's name and
	// its line in the Tools menu.
	SettingsWindow(const std::string &id, const std::string &title, SettingsPage *page);
	~SettingsWindow();
	SettingsWindow(const SettingsWindow &) = delete;
	SettingsWindow &operator=(const SettingsWindow &) = delete;

	// From obs_module_post_load (OBS's UI thread): the type, the settings
	// object and the Tools menu item.
	bool start();
	// OBS is closing (OBS_FRONTEND_EVENT_EXIT, on the UI thread): the page
	// is never called again, and the settings object goes. Safe twice.
	void stop();

	// Shows the window. `before` runs first, on the UI thread, with the
	// settings - to choose what it shows.
	void open(std::function<void(obs_data_t *)> before = nullptr);
	// Changes the settings on the UI thread (`edit` gets them), tells the
	// page (changed()), and draws an open window again.
	void change(std::function<void(obs_data_t *)> edit);
	// Whether it's open on screen.
	bool is_open() const;

	struct Shared;

private:
	std::shared_ptr<Shared> shared_;
};

} // namespace direbot::obs
