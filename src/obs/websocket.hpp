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

// The link's requests and events come from obs-websocket, the websocket
// plugin that ships inside OBS, through its plugin API. Every call into
// obs-websocket is in websocket.cpp alone.
//
// Requests from the link go straight to obs-websocket's own request code
// as JSON text (no conversion on the way, so nothing is lost), except the
// plugin's own requests, which are answered here: CallVendorRequest with
// vendorName "direbot". The same requests are registered with
// obs-websocket too, so they also work over OBS's normal websocket.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <obs.h>

#include "../link/protocol.hpp"

namespace direbot::obs {

using link::json;

class WebSocket : public link::Backend {
public:
	// The plugin's own requests: (requestType, requestData) -> responseData.
	// `found` is false for a request type the plugin doesn't have.
	using VendorHandler = std::function<json(const std::string &type, const json &data, bool &found)>;
	// An obs-websocket event, with its data as JSON text.
	using EventHandler = std::function<void(uint64_t intent, const char *type, const char *data)>;

	static constexpr const char *VendorName = "direbot";

	// Finds obs-websocket. Only from obs_module_post_load or later: it may
	// load after this plugin. False when it isn't in this OBS, or is too
	// old for the link.
	bool find();
	// In this OBS at all (any version): the plugin's requests still work
	// over OBS's normal websocket.
	bool present() const { return api_version_ >= 1; }
	unsigned api_version() const { return api_version_; }

	// Starts and stops the events (for the link), and the plugin's
	// requests over OBS's normal websocket. Not from obs_module_unload:
	// obs-websocket may be gone by then.
	bool connect(VendorHandler vendor, EventHandler events, const std::vector<std::string> &vendor_requests);
	void disconnect();

	link::RequestResult call(const std::string &type, const json &data) override;

private:
	unsigned api_version_ = 0;
	void *vendor_ = nullptr;
	bool events_on_ = false;
	struct VendorRequest {
		WebSocket *self;
		std::string type;
	};
	std::vector<std::unique_ptr<VendorRequest>> registered_;
	VendorHandler vendor_handler_;
	EventHandler event_handler_;

	link::RequestResult call_vendor(const json &data);
	static void on_event(uint64_t intent, const char *type, const char *data, void *self);
	static void on_vendor_request(obs_data_t *request, obs_data_t *response, void *param);
};

} // namespace direbot::obs
