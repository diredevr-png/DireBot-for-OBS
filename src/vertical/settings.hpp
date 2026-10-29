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

// OBS → Tools → DireBot Vertical: where the tall picture streams to, with
// the stream keys, and how. The only place a vertical stream key is ever
// typed or shown. DireBot can open this window, never read it.
//
// A saved key is never shown again, not even here: the key box starts
// empty ("A key is saved"), typing in it replaces the key, and a tick
// removes it. What's typed is in the window's settings only until it
// closes; the rest of the time keys are only in direbot-vertical.json,
// locked (options.hpp). Cancel puts back the keys from when it opened.
//
// Everything here is on OBS's UI thread.

#pragma once

#include <array>
#include <functional>
#include <string>

#include "../obs/settings-window.hpp"
#include "options.hpp"

namespace direbot::vertical {

class SettingsPage : public obs::SettingsPage {
public:
	// `get` gives the options as they are; `set` keeps new ones (saves
	// them and tells DireBot).
	SettingsPage(std::function<const Options &()> get, std::function<void(const Options &)> set);

	obs_properties_t *properties(obs_data_t *settings) override;
	void changed(obs_data_t *settings) override;
	void defaults(obs_data_t *settings) override;
	bool opened(obs_data_t *settings) override;
	void closed(obs_data_t *settings) override;

	// Puts the options (not the keys) in the window's settings: when they
	// load, and when DireBot changes them.
	static void fill(obs_data_t *settings, const Options &options);
	// Another profile is in use (OBS's UI thread): a window still open
	// shows the last profile's places and its keys, so nothing in it is
	// kept until it's closed and opened again.
	void profile_changed();

private:
	std::function<const Options &()> get_;
	std::function<void(const Options &)> set_;
	bool open_ = false;
	static void forget_keys(obs_data_t *settings);
	// Opened for another profile than the one in use now.
	bool stale_ = false;
	// The keys when the window opened (locked), for Cancel.
	std::array<std::string, Options::SLOTS> key_at_open_;
	// ...and the site each was for: another site doesn't get it.
	std::array<std::string, Options::SLOTS> site_at_open_;
	// A key that couldn't be unlocked when the window opened (locked on
	// another PC or Windows account).
	std::array<bool, Options::SLOTS> unreadable_{};
};

} // namespace direbot::vertical
