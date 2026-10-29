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

// DireBot Link speaks obs-websocket's own language (protocol 5, RPC
// version 1), so anything that can talk to OBS's websocket - DireBot
// included - can talk to the link unchanged. The difference is only how
// you get in: the link listens on this PC alone, and its password is
// written for DireBot in a file only you can read, so nobody has to type
// one.
//
// Requests aren't answered here. They're handed to obs-websocket inside
// OBS (its plugin API runs the very same request code), so the link can
// do everything the websocket can, in the same words.

#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

namespace direbot::link {

using json = nlohmann::json;

// JSON text that never throws: OBS names aren't always valid UTF-8 (a
// script or plugin can make any), and a plain dump() throws on those. Bad
// bytes come out as U+FFFD.
inline std::string to_text(const json &j)
{
	return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

// obs-websocket's close codes (WebSocketCloseCode).
namespace close_code {
enum : uint16_t {
	DontClose = 0,
	UnknownReason = 4000,
	MessageDecodeError = 4002,
	MissingDataField = 4003,
	InvalidDataFieldType = 4004,
	InvalidDataFieldValue = 4005,
	UnknownOpCode = 4006,
	NotIdentified = 4007,
	AlreadyIdentified = 4008,
	AuthenticationFailed = 4009,
	UnsupportedRpcVersion = 4010,
	SessionInvalidated = 4011,
	UnsupportedFeature = 4012,
};
}

// obs-websocket's request status codes (RequestStatus), the ones used here.
namespace status {
enum : int {
	Success = 100,
	MissingRequestType = 203,
	UnknownRequestType = 204,
	GenericError = 205,
	NotReady = 207,
	MissingRequestField = 300,
	InvalidRequestFieldType = 401,
	RequestFieldOutOfRange = 402,
	ResourceNotFound = 600,
	RequestProcessingFailed = 702,
};
}

// obs-websocket's event subscription bits (EventSubscription).
namespace subs {
enum : uint64_t {
	General = 1 << 0,
	Config = 1 << 1,
	Scenes = 1 << 2,
	Inputs = 1 << 3,
	Transitions = 1 << 4,
	Filters = 1 << 5,
	Outputs = 1 << 6,
	SceneItems = 1 << 7,
	MediaInputs = 1 << 8,
	Vendors = 1 << 9,
	Ui = 1 << 10,
	Canvases = 1 << 11,
	All = (1 << 12) - 1,
	InputVolumeMeters = 1 << 16,
	InputActiveStateChanged = 1 << 17,
	InputShowStateChanged = 1 << 18,
	SceneItemTransformChanged = 1 << 19,
};
}

struct RequestResult {
	int code = status::Success;
	std::string comment;
	json data; // an object, or null for none
};

// Whatever runs a request: obs-websocket inside OBS, or a pretend one in
// the tests.
class Backend {
public:
	virtual ~Backend() = default;
	virtual RequestResult call(const std::string &type, const json &data) = 0;
};

// One connected client, from its Hello to its last request.
class Session {
public:
	Session(const std::string &password, const std::string &websocket_version);

	// The first message: op 0, with the login challenge.
	std::string hello() const;

	struct Outcome {
		std::string reply; // send now (empty: nothing)
		uint16_t close = close_code::DontClose;
		std::string reason;
		// A request (op 6) or batch (op 8) to run away from the network
		// thread: pass `op` and `payload` to run().
		bool work = false;
		int op = 0;
		json payload;
		// Identify or Reidentify changed which events this client wants.
		bool subscriptions_changed = false;
	};

	Outcome handle(const std::string &text);
	Outcome binary_message() const;

	// Runs a request or batch and returns the reply to send (op 7 / op 9).
	// `stopping`: OBS is closing - what hasn't run yet says "not ready".
	std::string run(int op, const json &payload, Backend &backend, const std::atomic<bool> &stopping) const;

	bool identified() const { return identified_; }
	uint64_t subscriptions() const { return subscriptions_; }

	// Exposed for the tests: what a client must send to log in.
	static std::string auth_string(const std::string &password, const std::string &salt,
				       const std::string &challenge);

private:
	std::string password_;
	std::string salt_;
	std::string challenge_;
	std::string websocket_version_;
	std::atomic<bool> identified_{false};
	std::atomic<uint64_t> subscriptions_{0};

	Outcome fail(uint16_t code, const std::string &reason) const;
	bool set_subscriptions(const json &d, Outcome &out);
};

// The text of one event (op 5), built straight from obs-websocket's own
// event data (already JSON), so it isn't parsed again for every client.
std::string event_message(const std::string &type, uint64_t intent, const std::string &data_json);

} // namespace direbot::link
