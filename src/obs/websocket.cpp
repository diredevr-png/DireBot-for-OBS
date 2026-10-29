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

#include "websocket.hpp"

#include <obs-websocket/obs-websocket-api.h>

namespace direbot::obs {

namespace {

// obs-websocket 5.5 (OBS 30) and up: requests and events through the API.
constexpr unsigned NEEDED_API = 3;

json parse(const char *text)
{
	if (!text || !*text)
		return nullptr;
	json j = json::parse(text, nullptr, false);
	return j.is_discarded() ? json(nullptr) : j;
}

} // namespace

bool WebSocket::find()
{
	api_version_ = obs_websocket_get_api_version();
	return api_version_ >= NEEDED_API;
}

bool WebSocket::connect(VendorHandler vendor, EventHandler events, std::initializer_list<const char *> vendor_requests)
{
	if (api_version_ < NEEDED_API)
		return false;
	vendor_handler_ = std::move(vendor);
	event_handler_ = std::move(events);
	if (!events_on_)
		events_on_ = obs_websocket_register_event_callback(&WebSocket::on_event, this);
	// Over OBS's normal websocket too. Not needed for the link, so a
	// failure here only means those calls say "not found".
	if (!vendor_)
		vendor_ = obs_websocket_register_vendor(VendorName);
	if (vendor_ && registered_.empty()) {
		for (const char *type : vendor_requests) {
			auto r = std::make_unique<VendorRequest>(VendorRequest{this, type});
			if (obs_websocket_vendor_register_request(vendor_, type, &WebSocket::on_vendor_request, r.get()))
				registered_.push_back(std::move(r));
		}
	}
	return events_on_;
}

void WebSocket::disconnect()
{
	// obs-websocket waits for an event call already under way before
	// this returns, so nothing arrives afterwards.
	if (events_on_)
		obs_websocket_unregister_event_callback(&WebSocket::on_event, this);
	events_on_ = false;
	if (vendor_)
		for (auto &r : registered_)
			obs_websocket_vendor_unregister_request(vendor_, r->type.c_str());
	registered_.clear();
}

link::RequestResult WebSocket::call(const std::string &type, const json &data)
{
	if (type == "CallVendorRequest" && data.is_object() && data.contains("vendorName") &&
	    data["vendorName"].is_string() && data["vendorName"].get<std::string>() == VendorName)
		return call_vendor(data);

	if (!obs_websocket_ensure_ph())
		return {link::status::NotReady, "obs-websocket isn't running in OBS.", nullptr};

	// Straight to obs-websocket as JSON text: obs-websocket's own
	// obs_websocket_call_request() goes through obs_data, which can't hold
	// every kind of JSON (a list of plain numbers, for one).
	const std::string body = data.is_object() ? link::to_text(data) : std::string();
	calldata_t cd = {0, 0, 0, 0};
	calldata_set_string(&cd, "request_type", type.c_str());
	calldata_set_string(&cd, "request_data", body.empty() ? nullptr : body.c_str());
	proc_handler_call(_ph, "call_request", &cd);
	auto *response = static_cast<obs_websocket_request_response *>(calldata_ptr(&cd, "response"));
	calldata_free(&cd);
	// No answer at all: OBS is still starting, changing scene
	// collection, or closing - what obs-websocket's server says then.
	if (!response)
		return {link::status::NotReady, "OBS is not ready to perform the request.", nullptr};

	link::RequestResult r;
	r.code = (int)response->status_code;
	if (response->comment)
		r.comment = response->comment;
	r.data = parse(response->response_data);
	obs_websocket_request_response_free(response);
	return r;
}

link::RequestResult WebSocket::call_vendor(const json &data)
{
	if (!data.contains("requestType") || !data["requestType"].is_string())
		return {link::status::MissingRequestField, "Your request is missing the `requestType` field.", nullptr};
	const std::string type = data["requestType"].get<std::string>();
	json request = json::object();
	if (data.contains("requestData") && !data["requestData"].is_null()) {
		if (!data["requestData"].is_object())
			return {link::status::InvalidRequestFieldType, "The field value of `requestData` must be an object.",
				nullptr};
		request = data["requestData"];
	}
	bool found = false;
	json response;
	try {
		response = vendor_handler_ ? vendor_handler_(type, request, found) : json();
	} catch (const std::exception &) {
		return {link::status::RequestProcessingFailed, "DireBot for OBS couldn't do that.", nullptr};
	}
	if (!found)
		return {link::status::ResourceNotFound, "No request was found by that name.", nullptr};
	if (!response.is_object())
		response = json::object();
	return {link::status::Success,
		"",
		{{"vendorName", VendorName}, {"requestType", type}, {"responseData", std::move(response)}}};
}

void WebSocket::on_event(uint64_t intent, const char *type, const char *data, void *self)
{
	auto *ws = static_cast<WebSocket *>(self);
	if (ws->event_handler_ && type)
		ws->event_handler_(intent, type, data ? data : "null");
}

// The same requests over OBS's normal websocket, which hands them over as
// obs_data.
void WebSocket::on_vendor_request(obs_data_t *request, obs_data_t *response, void *param)
{
	auto *r = static_cast<VendorRequest *>(param);
	if (!r->self->vendor_handler_)
		return;
	const char *text = request ? obs_data_get_json(request) : nullptr;
	json in = parse(text);
	if (!in.is_object())
		in = json::object();
	bool found = false;
	json out;
	try {
		out = r->self->vendor_handler_(r->type, in, found);
	} catch (const std::exception &) {
		return;
	}
	if (!out.is_object())
		return;
	obs_data_t *answer = obs_data_create_from_json(link::to_text(out).c_str());
	if (answer) {
		obs_data_apply(response, answer);
		obs_data_release(answer);
	}
}

} // namespace direbot::obs
