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

#include "settings.hpp"

#include <string>

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/config-file.h>

#include "../obs/keystore.hpp"
#include <plugin-support.h>
#include "outputs.hpp"
#include "rules.hpp"

namespace direbot::vertical {

namespace {

const char *const SITES[] = {"tiktok", "youtube", "instagram", "kick", "facebook", "x", "twitch", "other"};

std::string field(int slot, const char *name)
{
	return "p" + std::to_string(slot) + "_" + name;
}

std::string words(const char *key)
{
	return obs_module_text(key);
}

std::string trimmed(const char *text)
{
	std::string s = text ? text : "";
	const char *space = " \t\r\n";
	const size_t start = s.find_first_not_of(space);
	if (start == std::string::npos) {
		obs::keystore::wipe(s);
		return "";
	}
	const size_t end = s.find_last_not_of(space);
	std::string out = s.substr(start, end - start + 1);
	obs::keystore::wipe(s);
	return out;
}

// A sound choice: "Same as your stream", then OBS's six audio tracks, by
// the names given them in OBS (Settings → Output → Audio).
void add_sounds(obs_property_t *list)
{
	obs_property_list_add_int(list, words("Vertical.SoundSame").c_str(), 0);
	config_t *profile = obs_frontend_get_profile_config();
	for (int t = 1; t <= 6; t++) {
		std::string label = words("Vertical.SoundTrack");
		const size_t at = label.find("%1");
		if (at != std::string::npos)
			label.replace(at, 2, std::to_string(t));
		const std::string key = "Track" + std::to_string(t) + "Name";
		const char *named = profile ? config_get_string(profile, "AdvOut", key.c_str()) : nullptr;
		if (named && *named)
			label += std::string(" (") + named + ")";
		obs_property_list_add_int(list, label.c_str(), t);
	}
}

bool is_usual_server(const std::string &server)
{
	if (server.empty())
		return true;
	for (const char *site : SITES)
		if (server == rules::usual_server(site))
			return true;
	return false;
}

// Picking a site fills in its server, unless one was typed.
bool site_changed(void *, obs_properties_t *, obs_property_t *property, obs_data_t *settings)
{
	const std::string name = obs_property_name(property); // "p2_site"
	const std::string slot = name.substr(0, name.find('_'));
	const std::string server_field = slot + "_server";
	const std::string server = obs_data_get_string(settings, server_field.c_str());
	const std::string usual = rules::usual_server(obs_data_get_string(settings, name.c_str()));
	if (is_usual_server(server) && server != usual)
		obs_data_set_string(settings, server_field.c_str(), usual.c_str());
	return true; // its help changes too
}

} // namespace

SettingsPage::SettingsPage(std::function<const Options &()> get, std::function<void(const Options &)> set)
	: get_(std::move(get)),
	  set_(std::move(set))
{
}

void SettingsPage::defaults(obs_data_t *settings)
{
	const Options plain;
	for (int i = 1; i <= Options::SLOTS; i++) {
		obs_data_set_default_string(settings, field(i, "site").c_str(), SITES[(i - 1) % 3]);
		obs_data_set_default_int(settings, field(i, "track").c_str(), 0);
	}
	obs_data_set_default_int(settings, "kbps", plain.kbps);
	obs_data_set_default_string(settings, "encoder", plain.encoder.c_str());
	obs_data_set_default_int(settings, "audio_track", plain.audio_track);
}

void SettingsPage::fill(obs_data_t *settings, const Options &options)
{
	for (int i = 1; i <= Options::SLOTS; i++) {
		const Destination &d = options.places[i - 1];
		obs_data_set_bool(settings, field(i, "on").c_str(), d.enabled);
		obs_data_set_string(settings, field(i, "site").c_str(), d.platform.c_str());
		obs_data_set_string(settings, field(i, "server").c_str(), d.server.c_str());
		obs_data_set_int(settings, field(i, "track").c_str(), d.audio_track);
	}
	obs_data_set_int(settings, "kbps", options.kbps);
	obs_data_set_string(settings, "encoder", options.encoder.c_str());
	obs_data_set_int(settings, "audio_track", options.audio_track);
	obs_data_set_bool(settings, "with_stream", options.with_stream);
	obs_data_set_bool(settings, "record_with_main", options.record_with_main);
}

obs_properties_t *SettingsPage::properties(obs_data_t *settings)
{
	const Options &now = get_();
	obs_properties_t *props = obs_properties_create();
	if (stale_) {
		obs_property_t *gone = obs_properties_add_text(props, "stale", words("Vertical.Stale").c_str(),
								 OBS_TEXT_INFO);
		obs_property_text_set_info_type(gone, OBS_TEXT_INFO_WARNING);
	}
	obs_properties_add_text(props, "intro", words("Vertical.Intro").c_str(), OBS_TEXT_INFO);

	for (int i = 1; i <= Options::SLOTS; i++) {
		const Destination &d = now.places[i - 1];
		obs_properties_t *group = obs_properties_create();

		obs_property_t *site = obs_properties_add_list(group, field(i, "site").c_str(),
							       words("Vertical.Site").c_str(), OBS_COMBO_TYPE_LIST,
							       OBS_COMBO_FORMAT_STRING);
		for (const char *s : SITES)
			obs_property_list_add_string(site, rules::platform_label(s).c_str(), s);
		obs_property_set_modified_callback2(site, site_changed, this);

		const std::string chosen = obs_data_get_string(settings, field(i, "site").c_str());
		const std::string help = "Vertical.Help." + (chosen.empty() ? std::string("other") : chosen);
		obs_properties_add_text(group, field(i, "help").c_str(), words(help.c_str()).c_str(), OBS_TEXT_INFO);

		obs_properties_add_text(group, field(i, "server").c_str(), words("Vertical.Server").c_str(),
					OBS_TEXT_DEFAULT);
		obs_property_t *key = obs_properties_add_text(group, field(i, "key").c_str(),
							      words("Vertical.Key").c_str(), OBS_TEXT_PASSWORD);
		obs_property_set_long_description(key, words("Vertical.KeyTip").c_str());

		const char *state = unreadable_[i - 1] ? "Vertical.KeyUnreadable"
				    : d.key_locked.empty() ? "Vertical.KeyNone"
							   : "Vertical.KeySaved";
		obs_property_t *note =
			obs_properties_add_text(group, field(i, "key_state").c_str(), words(state).c_str(), OBS_TEXT_INFO);
		if (unreadable_[i - 1])
			obs_property_text_set_info_type(note, OBS_TEXT_INFO_WARNING);
		if (!d.key_locked.empty())
			obs_properties_add_bool(group, field(i, "clear").c_str(), words("Vertical.KeyClear").c_str());

		obs_property_t *sound = obs_properties_add_list(group, field(i, "track").c_str(),
								words("Vertical.Sound").c_str(), OBS_COMBO_TYPE_LIST,
								OBS_COMBO_FORMAT_INT);
		add_sounds(sound);

		std::string title = words("Vertical.Place");
		const size_t at = title.find("%1");
		if (at != std::string::npos)
			title.replace(at, 2, std::to_string(i));
		// The group's tick is "stream here".
		obs_properties_add_group(props, field(i, "on").c_str(), title.c_str(), OBS_GROUP_CHECKABLE, group);
	}

	obs_properties_t *quality = obs_properties_create();
	obs_property_t *kbps = obs_properties_add_int_slider(quality, "kbps", words("Vertical.Kbps").c_str(),
							     rules::MIN_KBPS, rules::MAX_KBPS, 100);
	obs_property_int_set_suffix(kbps, " kbps");
	obs_property_t *encoder = obs_properties_add_list(quality, "encoder", words("Vertical.Encoder").c_str(),
							  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(encoder, words("Vertical.EncoderAuto").c_str(), "auto");
	for (const EncoderRow &row : h264_encoders())
		obs_property_list_add_string(encoder, row.name.c_str(), row.id.c_str());
	obs_property_t *track = obs_properties_add_list(quality, "audio_track", words("Vertical.AudioTrack").c_str(),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	add_sounds(track);
	obs_properties_add_group(props, "quality", words("Vertical.Quality").c_str(), OBS_GROUP_NORMAL, quality);

	obs_properties_t *with = obs_properties_create();
	obs_properties_add_bool(with, "with_stream", words("Vertical.WithStream").c_str());
	obs_properties_add_bool(with, "record_with_main", words("Vertical.RecordWithMain").c_str());
	obs_properties_add_group(props, "with_obs", words("Vertical.WithObs").c_str(), OBS_GROUP_NORMAL, with);
	obs_properties_add_text(props, "twitch", words("Vertical.TwitchTip").c_str(), OBS_TEXT_INFO);
	return props;
}

void SettingsPage::forget_keys(obs_data_t *settings)
{
	for (int i = 1; i <= Options::SLOTS; i++) {
		obs_data_erase(settings, field(i, "key").c_str());
		obs_data_erase(settings, field(i, "clear").c_str());
	}
}

void SettingsPage::profile_changed()
{
	if (!open_)
		return;
	stale_ = true;
	for (int i = 1; i <= Options::SLOTS; i++) {
		obs::keystore::wipe(key_at_open_[i - 1]);
		key_at_open_[i - 1].clear();
	}
}

void SettingsPage::changed(obs_data_t *settings)
{
	const Options &now = get_();
	// OBS's Edit → Undo / Redo of the window's OK puts back all of its
	// settings from then, typed keys and all, with the window shut. The
	// places stay as they are, and the window's settings go back to
	// matching them, without keys - so a key can't come back into the
	// box (and its Show button) the next time it opens.
	if (obs_data_has_user_value(settings, "undo_uuid")) {
		obs_data_erase(settings, "undo_uuid");
		if (!open_) {
			obs_log(LOG_INFO, "Vertical: OBS's Undo / Redo doesn't change where it streams; the "
					  "window's settings were put back.");
			forget_keys(settings);
			fill(settings, now);
			return;
		}
	}
	// A window opened for another profile: what's in it isn't this
	// profile's, keys least of all.
	if (stale_) {
		forget_keys(settings);
		return;
	}
	Options next = now;
	for (int i = 1; i <= Options::SLOTS; i++) {
		Destination &d = next.places[i - 1];
		d.enabled = obs_data_get_bool(settings, field(i, "on").c_str());
		const std::string site = obs_data_get_string(settings, field(i, "site").c_str());
		d.platform = site.empty() ? d.platform : site;
		std::string server = trimmed(obs_data_get_string(settings, field(i, "server").c_str()));
		d.server = server;
		const int sound = (int)obs_data_get_int(settings, field(i, "track").c_str());
		d.audio_track = sound >= 0 && sound <= 6 ? sound : d.audio_track;

		// The key, only from the open window: a key typed (not empty)
		// replaces it, "remove" takes it away, and anything else - Cancel
		// too, which puts back the settings from before the window
		// opened - leaves the key it had then.
		if (!open_)
			continue;
		std::string typed = trimmed(obs_data_get_string(settings, field(i, "key").c_str()));
		if (!typed.empty()) {
			std::string before;
			const bool same = obs::keystore::unlock(d.key_locked, before) && before == typed;
			obs::keystore::wipe(before);
			if (!same) {
				std::string locked = obs::keystore::lock(typed);
				if (locked.empty())
					// Windows wouldn't lock it: not saved, and the
					// key it had stays.
					obs_log(LOG_WARNING, "Vertical: Windows couldn't lock the stream key for place "
							     "%d, so it wasn't saved. Type it in again.",
						i);
				else
					d.key_locked = locked;
			}
		} else if (obs_data_get_bool(settings, field(i, "clear").c_str())) {
			d.key_locked.clear();
		} else if (d.platform == site_at_open_[i - 1]) {
			d.key_locked = key_at_open_[i - 1];
		} else {
			// Another site than when the window opened: that site's
			// key would go to this one's servers, so there's none
			// until one's typed (picking the first site again puts
			// it back).
			d.key_locked.clear();
		}
		obs::keystore::wipe(typed);
	}
	next.kbps = (int)obs_data_get_int(settings, "kbps");
	if (next.kbps < rules::MIN_KBPS || next.kbps > rules::MAX_KBPS)
		next.kbps = now.kbps;
	const std::string encoder = obs_data_get_string(settings, "encoder");
	next.encoder = encoder.empty() ? "auto" : encoder;
	const int track = (int)obs_data_get_int(settings, "audio_track");
	next.audio_track = track >= 0 && track <= 6 ? track : now.audio_track;
	next.with_stream = obs_data_get_bool(settings, "with_stream");
	next.record_with_main = obs_data_get_bool(settings, "record_with_main");

	if (next.to_file() != now.to_file())
		set_(next);
}

bool SettingsPage::opened(obs_data_t *)
{
	open_ = true;
	const Options &now = get_();
	for (int i = 1; i <= Options::SLOTS; i++) {
		key_at_open_[i - 1] = now.places[i - 1].key_locked;
		site_at_open_[i - 1] = now.places[i - 1].platform;
		std::string key;
		unreadable_[i - 1] = !obs::keystore::unlock(key_at_open_[i - 1], key);
		obs::keystore::wipe(key);
	}
	return true; // the key notes change
}

void SettingsPage::closed(obs_data_t *settings)
{
	open_ = false;
	stale_ = false;
	for (int i = 1; i <= Options::SLOTS; i++) {
		// Out of OBS's hands as soon as it's no longer on screen.
		obs_data_erase(settings, field(i, "key").c_str());
		obs_data_erase(settings, field(i, "clear").c_str());
		obs::keystore::wipe(key_at_open_[i - 1]);
		key_at_open_[i - 1].clear();
		site_at_open_[i - 1].clear();
		unreadable_[i - 1] = false;
	}
}

} // namespace direbot::vertical
