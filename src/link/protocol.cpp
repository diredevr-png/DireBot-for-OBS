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

#include "protocol.hpp"
#include "crypto.hpp"

#include <chrono>
#include <thread>

namespace direbot::link {

namespace {

constexpr int RPC_VERSION = 1;

enum Op : int {
	Hello = 0,
	Identify = 1,
	Identified = 2,
	Reidentify = 3,
	Event = 5,
	Request = 6,
	RequestResponse = 7,
	RequestBatch = 8,
	RequestBatchResponse = 9,
};

// obs-websocket's RequestBatchExecutionType.
enum Execution : int {
	SerialRealtime = 0,
	SerialFrame = 1,
	Parallel = 2,
};

json status_json(const RequestResult &r)
{
	json s = {{"result", r.code == status::Success}, {"code", r.code}};
	if (!r.comment.empty())
		s["comment"] = r.comment;
	return s;
}

// The secrets in OBS's stream settings: the stream key, and a custom
// server's password or token. DireBot only ever needs to know one is set,
// so the link never hands them out.
const char *const STREAM_SECRETS[] = {"key", "password", "bearer_token"};

// A key in the server address itself (rtmp://host/app/KEY, or ?key=...):
// the address is shown up to its first folder, and the rest as stars.
std::string hide_server_path(const std::string &server)
{
	const size_t scheme = server.find("://");
	const size_t host_start = scheme == std::string::npos ? 0 : scheme + 3;
	const size_t path = server.find_first_of("/?#", host_start);
	if (path == std::string::npos)
		return server;
	if (server[path] != '/')
		return server.substr(0, path) + "/" + HIDDEN_SECRET;
	const size_t second = server.find_first_of("/?#", path + 1);
	if (second == std::string::npos || second + 1 >= server.size())
		return server;
	return server.substr(0, second) + "/" + HIDDEN_SECRET;
}

void hide_stream_secrets(RequestResult &r)
{
	if (!r.data.is_object() || !r.data.contains("streamServiceSettings"))
		return;
	json &settings = r.data["streamServiceSettings"];
	if (!settings.is_object())
		return;
	for (const char *name : STREAM_SECRETS)
		if (settings.contains(name) && settings[name].is_string() && !settings[name].get<std::string>().empty())
			settings[name] = HIDDEN_SECRET;
	if (settings.contains("server") && settings["server"].is_string())
		settings["server"] = hide_server_path(settings["server"].get<std::string>());
}

// Where OBS streams can only change through the link along with a key (or
// a custom server's token) sent with it: OBS keeps the key it has when only
// the server changes, so otherwise anyone with the link file could send
// your key to a server of theirs. Every secret not sent is emptied, so none
// of the old ones go along to the new server. "" when it may go on, else
// why not.
std::string guard_stream_secrets(const json &data, json &out)
{
	out = data;
	if (!data.is_object() || !data.contains("streamServiceSettings") ||
	    !data["streamServiceSettings"].is_object())
		return "";
	json &settings = out["streamServiceSettings"];
	bool given = false;
	for (const char *name : STREAM_SECRETS) {
		if (!settings.contains(name))
			continue;
		const json &v = settings[name];
		if (v == HIDDEN_SECRET)
			return "Send the stream key itself: the stars read back from OBS aren't it.";
		if (v.is_string() && !v.get<std::string>().empty())
			given = true;
	}
	if (!given)
		return "Changing where OBS streams needs the stream key sent with it.";
	if (settings.contains("server") && settings["server"].is_string() &&
	    settings["server"].get<std::string>().find(HIDDEN_SECRET) != std::string::npos)
		return "Send the server address itself: the stars read back from OBS aren't it.";
	for (const char *name : STREAM_SECRETS)
		if (!settings.contains(name))
			settings[name] = "";
	return "";
}

RequestResult call_one(const std::string &type, const json &data, Backend &backend)
{
	if (type.empty())
		return {status::MissingRequestType, "Your request is missing a `requestType`.", nullptr};
	if (type == "SetStreamServiceSettings") {
		json guarded;
		const std::string refused = guard_stream_secrets(data, guarded);
		if (!refused.empty())
			return {status::InvalidRequestField, refused, nullptr};
		return backend.call(type, guarded);
	}
	RequestResult r = backend.call(type, data);
	if (type == "GetStreamServiceSettings")
		hide_stream_secrets(r);
	return r;
}

// All the Sleeps in one batch together: a batch of a thousand long Sleeps
// would hold one of the link's workers for days.
constexpr double BATCH_SLEEP_MS = 60000;

// "Sleep" only means something inside a batch, so the batch does it here.
// `budget` is what's left of the batch's sleeping, in ms.
RequestResult sleep_for(const json &data, int execution, const std::atomic<bool> &stopping, double &budget)
{
	double ms = 0;
	if (execution == SerialFrame) {
		if (!data.is_object() || !data.contains("sleepFrames") || !data["sleepFrames"].is_number())
			return {status::MissingRequestField, "Your request is missing `sleepFrames`.", nullptr};
		double frames = data["sleepFrames"].get<double>();
		if (frames < 0 || frames > 10000)
			return {status::RequestFieldOutOfRange, "`sleepFrames` is out of range.", nullptr};
		ms = frames * 1000.0 / 60.0;
	} else {
		if (!data.is_object() || !data.contains("sleepMillis") || !data["sleepMillis"].is_number())
			return {status::MissingRequestField, "Your request is missing `sleepMillis`.", nullptr};
		ms = data["sleepMillis"].get<double>();
		if (ms < 0 || ms > 50000)
			return {status::RequestFieldOutOfRange, "`sleepMillis` is out of range.", nullptr};
	}
	if (ms > budget)
		return {status::RequestFieldOutOfRange, "A batch can sleep for up to 60 seconds in all.", nullptr};
	budget -= ms;
	auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds((long long)ms);
	while (std::chrono::steady_clock::now() < until) {
		if (stopping)
			return {status::NotReady, "OBS is closing.", nullptr};
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return {status::Success, "", nullptr};
}

} // namespace

Session::Session(const std::string &password, const std::string &websocket_version)
	: password_(password),
	  salt_(crypto::base64(crypto::random_bytes(32))),
	  challenge_(crypto::base64(crypto::random_bytes(32))),
	  websocket_version_(websocket_version.empty() ? "5.0.0" : websocket_version)
{
}

std::string Session::auth_string(const std::string &password, const std::string &salt, const std::string &challenge)
{
	std::string secret = crypto::base64(crypto::sha256(password + salt));
	return crypto::base64(crypto::sha256(secret + challenge));
}

std::string Session::hello() const
{
	json d = {{"obsWebSocketVersion", websocket_version_}, {"rpcVersion", RPC_VERSION}};
	d["authentication"] = {{"challenge", challenge_}, {"salt", salt_}};
	return json({{"op", Hello}, {"d", d}}).dump();
}

Session::Outcome Session::fail(uint16_t code, const std::string &reason) const
{
	Outcome out;
	out.close = code;
	out.reason = reason;
	return out;
}

Session::Outcome Session::binary_message() const
{
	return fail(close_code::MessageDecodeError, "Only JSON (text) messages are supported.");
}

bool Session::set_subscriptions(const json &d, Outcome &out)
{
	if (!d.contains("eventSubscriptions"))
		return true;
	if (!d["eventSubscriptions"].is_number_unsigned()) {
		out = fail(close_code::InvalidDataFieldType, "Your `eventSubscriptions` is not an unsigned number.");
		return false;
	}
	subscriptions_ = d["eventSubscriptions"].get<uint64_t>();
	out.subscriptions_changed = true;
	return true;
}

Session::Outcome Session::handle(const std::string &text)
{
	json message;
	try {
		message = json::parse(text);
	} catch (const std::exception &) {
		return fail(close_code::MessageDecodeError, "Unable to decode the message as JSON.");
	}
	if (!message.is_object())
		return fail(close_code::MessageDecodeError, "The message isn't a JSON object.");
	if (!message.contains("op"))
		return fail(close_code::MissingDataField, "Your request is missing an `op`.");
	if (!message["op"].is_number_unsigned())
		return fail(close_code::InvalidDataFieldType, "Your `op` is not a number.");
	const int op = message["op"].get<int>();
	if (!message.contains("d") || message["d"].is_null())
		return fail(close_code::MissingDataField, "Your payload is missing data (`d`).");
	json &d = message["d"];
	if (!d.is_object())
		return fail(close_code::InvalidDataFieldType, "Your payload's data (`d`) is not an object.");
	if (!identified_ && op != Identify)
		return fail(close_code::NotIdentified, "You attempted to send a non-Identify message while not identified.");

	Outcome out;
	switch (op) {
	case Identify: {
		if (identified_)
			return fail(close_code::AlreadyIdentified, "You are already Identified with the obs-websocket server.");
		if (!d.contains("authentication") || !d["authentication"].is_string())
			return fail(close_code::AuthenticationFailed, "Authentication failed.");
		const std::string expected = auth_string(password_, salt_, challenge_);
		if (!crypto::same(d["authentication"].get<std::string>(), expected))
			return fail(close_code::AuthenticationFailed, "Authentication failed.");
		if (!d.contains("rpcVersion"))
			return fail(close_code::MissingDataField, "Your payload's data is missing an `rpcVersion`.");
		if (!d["rpcVersion"].is_number_unsigned())
			return fail(close_code::InvalidDataFieldType, "Your `rpcVersion` is not an unsigned number.");
		if (d["rpcVersion"].get<uint64_t>() != RPC_VERSION)
			return fail(close_code::UnsupportedRpcVersion,
				    "Your requested RPC version is not supported by this server.");
		// Like obs-websocket: without `eventSubscriptions`, all the usual ones.
		subscriptions_ = subs::All;
		if (!set_subscriptions(d, out))
			return out;
		out.subscriptions_changed = true;
		identified_ = true;
		out.reply = json({{"op", Identified}, {"d", {{"negotiatedRpcVersion", RPC_VERSION}}}}).dump();
		return out;
	}
	case Reidentify:
		if (!set_subscriptions(d, out))
			return out;
		out.reply = json({{"op", Identified}, {"d", {{"negotiatedRpcVersion", RPC_VERSION}}}}).dump();
		return out;
	case Request:
		if (!d.contains("requestId"))
			return fail(close_code::MissingDataField, "Your payload data is missing a `requestId`.");
		if (!d.contains("requestType"))
			return fail(close_code::MissingDataField, "Your payload's data is missing an `requestType`.");
		if (!d["requestType"].is_string())
			return fail(close_code::InvalidDataFieldType, "Your `requestType` is not a string.");
		out.work = true;
		out.op = op;
		out.payload = std::move(d);
		return out;
	case RequestBatch: {
		if (!d.contains("requestId"))
			return fail(close_code::MissingDataField, "Your payload data is missing a `requestId`.");
		if (d.contains("executionType") && !d["executionType"].is_null()) {
			if (!d["executionType"].is_number_unsigned())
				return fail(close_code::InvalidDataFieldType, "Your `executionType` is not a number.");
			uint64_t e = d["executionType"].get<uint64_t>();
			if (e > Parallel)
				return fail(close_code::InvalidDataFieldValue, "Your `executionType` has an invalid value.");
			if (e == Parallel && d.contains("variables") && !d["variables"].is_null())
				return fail(close_code::UnsupportedFeature, "Variables are not supported in Parallel mode.");
		}
		if (d.contains("variables") && !d["variables"].is_null() && !d["variables"].is_object())
			return fail(close_code::InvalidDataFieldType, "Your `variables` is not an object.");
		if (d.contains("haltOnFailure") && !d["haltOnFailure"].is_null() && !d["haltOnFailure"].is_boolean())
			return fail(close_code::InvalidDataFieldType, "Your `haltOnFailure` is not a boolean.");
		if (!d.contains("requests"))
			return fail(close_code::MissingDataField, "Your payload data is missing a `requests`.");
		if (!d["requests"].is_array())
			return fail(close_code::InvalidDataFieldType, "Your `requests` is not an array.");
		out.work = true;
		out.op = op;
		out.payload = std::move(d);
		return out;
	}
	default:
		return fail(close_code::UnknownOpCode, "Unknown OpCode: " + std::to_string(op));
	}
}

std::string Session::run(int op, const json &d, Backend &backend, const std::atomic<bool> &stopping) const
{
	if (op == Request) {
		const std::string type = d["requestType"].get<std::string>();
		const json data = d.contains("requestData") ? d["requestData"] : json();
		RequestResult r = stopping ? RequestResult{status::NotReady, "OBS is closing.", nullptr}
					   : call_one(type, data, backend);
		json out = {{"requestType", type}, {"requestId", d["requestId"]}, {"requestStatus", status_json(r)}};
		if (r.data.is_object())
			out["responseData"] = r.data;
		return to_text(json({{"op", RequestResponse}, {"d", out}}));
	}

	// A batch. Parallel and per-frame batches run one after another too:
	// the answers are the same, in the same order.
	int execution = SerialRealtime;
	if (d.contains("executionType") && d["executionType"].is_number_unsigned())
		execution = d["executionType"].get<int>();
	const bool halt = d.contains("haltOnFailure") && d["haltOnFailure"].is_boolean() && d["haltOnFailure"].get<bool>();
	json variables = d.contains("variables") && d["variables"].is_object() ? d["variables"] : json::object();
	json results = json::array();
	double sleep_left = BATCH_SLEEP_MS;
	for (const auto &item : d["requests"]) {
		const json request = item.is_object() ? item : json::object();
		std::string type = request.contains("requestType") && request["requestType"].is_string()
					   ? request["requestType"].get<std::string>()
					   : std::string();
		json data = request.contains("requestData") ? request["requestData"] : json();
		// Variables in: {"requestField": "variableName"}.
		if (execution != Parallel && request.contains("inputVariables") && request["inputVariables"].is_object()) {
			if (!data.is_object())
				data = json::object();
			for (auto &kv : request["inputVariables"].items())
				if (kv.value().is_string() && variables.contains(kv.value().get<std::string>()))
					data[kv.key()] = variables[kv.value().get<std::string>()];
		}
		RequestResult r;
		if (stopping)
			r = {status::NotReady, "OBS is closing.", nullptr};
		else if (type == "Sleep")
			r = sleep_for(data, execution, stopping, sleep_left);
		else
			r = call_one(type, data, backend);
		// Variables out: {"variableName": "responseField"}.
		if (execution != Parallel && r.data.is_object() && request.contains("outputVariables") &&
		    request["outputVariables"].is_object()) {
			for (auto &kv : request["outputVariables"].items())
				if (kv.value().is_string() && r.data.contains(kv.value().get<std::string>()))
					variables[kv.key()] = r.data[kv.value().get<std::string>()];
		}
		json row = {{"requestType", type}, {"requestStatus", status_json(r)}};
		if (request.contains("requestId") && !request["requestId"].is_null())
			row["requestId"] = request["requestId"];
		if (r.data.is_object())
			row["responseData"] = r.data;
		results.push_back(row);
		if (halt && r.code != status::Success)
			break;
	}
	return to_text(json({{"op", RequestBatchResponse}, {"d", {{"requestId", d["requestId"]}, {"results", results}}}}));
}

std::string event_message(const std::string &type, uint64_t intent, const std::string &data_json)
{
	std::string out = "{\"op\":5,\"d\":{\"eventType\":" + json(type).dump() +
			  ",\"eventIntent\":" + std::to_string(intent);
	if (!data_json.empty() && data_json != "null")
		out += ",\"eventData\":" + data_json;
	out += "}}";
	return out;
}

} // namespace direbot::link
