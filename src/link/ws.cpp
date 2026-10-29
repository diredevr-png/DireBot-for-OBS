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

#include "ws.hpp"
#include "crypto.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

namespace direbot::ws {

namespace {

const char *GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

std::string trim(const std::string &s)
{
	size_t a = 0, b = s.size();
	while (a < b && (s[a] == ' ' || s[a] == '\t'))
		a++;
	while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t'))
		b--;
	return s.substr(a, b - a);
}

// "a, b,c" -> {"a", "b", "c"} (lower-cased).
std::vector<std::string> tokens(const std::string &value)
{
	std::vector<std::string> out;
	size_t start = 0;
	while (start <= value.size()) {
		size_t comma = value.find(',', start);
		if (comma == std::string::npos)
			comma = value.size();
		std::string t = lower(trim(value.substr(start, comma - start)));
		if (!t.empty())
			out.push_back(t);
		start = comma + 1;
	}
	return out;
}

bool has_token(const std::string &value, const std::string &want)
{
	for (auto &t : tokens(value))
		if (t == want)
			return true;
	return false;
}

Handshake refuse(int status, const std::string &why, size_t consumed)
{
	Handshake h;
	h.state = Handshake::State::Reject;
	h.why = why;
	h.consumed = consumed;
	const char *text = status == 403 ? "Forbidden" : status == 426 ? "Upgrade Required" : "Bad Request";
	h.response = "HTTP/1.1 " + std::to_string(status) + " " + text +
		     "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
	return h;
}

} // namespace

std::string accept_key(const std::string &client_key)
{
	return crypto::base64(crypto::sha1(client_key + GUID));
}

bool is_this_pc(const std::string &host_and_port)
{
	std::string host = lower(trim(host_and_port));
	if (host.empty())
		return false;
	if (host[0] == '[') {
		size_t end = host.find(']');
		if (end == std::string::npos)
			return false;
		host = host.substr(1, end - 1);
	} else {
		size_t colon = host.find(':');
		if (colon != std::string::npos)
			host = host.substr(0, colon);
	}
	if (!host.empty() && host.back() == '.')
		host.pop_back();
	return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

Handshake read_handshake(const std::string &buffer, size_t max_bytes)
{
	size_t end = buffer.find("\r\n\r\n");
	if (end == std::string::npos) {
		if (buffer.size() > max_bytes)
			return refuse(400, "the request was far too long", buffer.size());
		return Handshake();
	}
	const size_t consumed = end + 4;
	if (consumed > max_bytes)
		return refuse(400, "the request was far too long", consumed);

	std::string head = buffer.substr(0, end);
	std::vector<std::string> lines;
	size_t start = 0;
	while (start <= head.size()) {
		size_t nl = head.find("\r\n", start);
		if (nl == std::string::npos)
			nl = head.size();
		lines.push_back(head.substr(start, nl - start));
		start = nl + 2;
	}
	if (lines.empty() || lines[0].rfind("GET ", 0) != 0)
		return refuse(400, "it wasn't a WebSocket request", consumed);

	std::string host, origin, upgrade, connection, version, key, protocols;
	for (size_t i = 1; i < lines.size(); i++) {
		size_t colon = lines[i].find(':');
		if (colon == std::string::npos)
			continue;
		std::string name = lower(trim(lines[i].substr(0, colon)));
		std::string value = trim(lines[i].substr(colon + 1));
		if (name == "host")
			host = value;
		else if (name == "origin")
			origin = value;
		else if (name == "upgrade")
			upgrade = value;
		else if (name == "connection")
			connection = value;
		else if (name == "sec-websocket-version")
			version = value;
		else if (name == "sec-websocket-key")
			key = value;
		else if (name == "sec-websocket-protocol")
			protocols = protocols.empty() ? value : protocols + "," + value;
	}
	if (!has_token(upgrade, "websocket") || !has_token(connection, "upgrade"))
		return refuse(426, "it didn't ask for a WebSocket", consumed);
	if (trim(version) != "13")
		return refuse(426, "it asked for an old WebSocket version", consumed);
	if (key.size() != 24)
		return refuse(400, "its WebSocket key was missing", consumed);
	if (!is_this_pc(host))
		return refuse(403, "it wasn't from this PC", consumed);
	if (!origin.empty()) {
		// A browser always says which page it's on. Only a page on this PC's
		// own address is allowed (websocket clients like DireBot's send one
		// like that too); any website is refused.
		std::string o = lower(origin);
		size_t scheme = o.find("://");
		if (scheme == std::string::npos)
			return refuse(403, "it came from a web page", consumed);
		std::string rest = o.substr(scheme + 3);
		size_t slash = rest.find('/');
		if (slash != std::string::npos)
			rest = rest.substr(0, slash);
		std::string s = o.substr(0, scheme);
		if ((s != "http" && s != "https") || !is_this_pc(rest))
			return refuse(403, "it came from a web page", consumed);
	}

	Handshake h;
	h.state = Handshake::State::Accept;
	h.consumed = consumed;
	h.json_protocol = has_token(protocols, "obswebsocket.json");
	h.response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
		     "Sec-WebSocket-Accept: " +
		     accept_key(key) + "\r\n";
	if (h.json_protocol)
		h.response += "Sec-WebSocket-Protocol: obswebsocket.json\r\n";
	h.response += "\r\n";
	return h;
}

std::string frame(uint8_t opcode, const std::string &payload)
{
	std::string out;
	out.push_back((char)(0x80 | (opcode & 0x0f)));
	const uint64_t n = payload.size();
	if (n < 126) {
		out.push_back((char)n);
	} else if (n <= 0xffff) {
		out.push_back((char)126);
		out.push_back((char)(n >> 8));
		out.push_back((char)(n & 0xff));
	} else {
		out.push_back((char)127);
		for (int i = 7; i >= 0; i--)
			out.push_back((char)((n >> (i * 8)) & 0xff));
	}
	out += payload;
	return out;
}

std::string close_frame(uint16_t code, const std::string &reason)
{
	std::string payload;
	payload.push_back((char)(code >> 8));
	payload.push_back((char)(code & 0xff));
	// A control frame carries at most 125 bytes.
	payload += reason.substr(0, 123);
	return frame(Opcode::Close, payload);
}

bool Reader::next(std::string &buffer, Item &out)
{
	for (;;) {
		if (buffer.size() < 2)
			return false;
		const auto *p = (const unsigned char *)buffer.data();
		const bool fin = (p[0] & 0x80) != 0;
		const uint8_t opcode = p[0] & 0x0f;
		const bool masked = (p[1] & 0x80) != 0;
		uint64_t length = p[1] & 0x7f;
		size_t at = 2;

		auto fail = [&](uint16_t code, const char *why) {
			out = Item();
			out.kind = Item::Kind::Error;
			out.code = code;
			out.data = why;
			buffer.clear();
			return true;
		};

		if (p[0] & 0x70)
			return fail(1002, "a frame used an extension that wasn't agreed");
		if (!masked)
			return fail(1002, "a frame from the client wasn't masked");
		if (length == 126) {
			if (buffer.size() < 4)
				return false;
			length = ((uint64_t)p[2] << 8) | p[3];
			at = 4;
		} else if (length == 127) {
			if (buffer.size() < 10)
				return false;
			length = 0;
			for (int i = 0; i < 8; i++)
				length = (length << 8) | p[2 + i];
			at = 10;
			if (length >> 63)
				return fail(1002, "a frame said it was impossibly long");
		}
		const bool control = (opcode & 0x08) != 0;
		if (control && (!fin || length > 125))
			return fail(1002, "a control frame was split or too long");
		if (!control && length > max_message_)
			return fail(1009, "a message was too big");
		if (buffer.size() < at + 4 + length)
			return false;

		const unsigned char *mask = p + at;
		std::string payload(buffer, at + 4, (size_t)length);
		for (size_t i = 0; i < payload.size(); i++)
			payload[i] = (char)(payload[i] ^ mask[i % 4]);
		buffer.erase(0, at + 4 + (size_t)length);

		if (control) {
			out = Item();
			if (opcode == Opcode::Close) {
				out.kind = Item::Kind::Close;
				if (payload.size() >= 2)
					out.code = (uint16_t)(((unsigned char)payload[0] << 8) | (unsigned char)payload[1]);
				out.data = payload.size() > 2 ? payload.substr(2) : std::string();
			} else if (opcode == Opcode::Ping) {
				out.kind = Item::Kind::Ping;
				out.data = payload;
			} else if (opcode == Opcode::Pong) {
				out.kind = Item::Kind::Pong;
				out.data = payload;
			} else {
				return fail(1002, "an unknown control frame");
			}
			return true;
		}

		if (opcode == Opcode::Continuation) {
			if (!in_message_)
				return fail(1002, "a continuation frame with nothing to continue");
		} else if (opcode == Opcode::Text || opcode == Opcode::Binary) {
			if (in_message_)
				return fail(1002, "a new message started before the last one ended");
			in_message_ = true;
			partial_opcode_ = opcode;
			partial_.clear();
		} else {
			return fail(1002, "an unknown frame");
		}
		if (partial_.size() + payload.size() > max_message_)
			return fail(1009, "a message was too big");
		partial_ += payload;
		if (!fin)
			continue;

		out = Item();
		out.kind = partial_opcode_ == Opcode::Text ? Item::Kind::Text : Item::Kind::Binary;
		out.data.swap(partial_);
		partial_.clear();
		in_message_ = false;
		return true;
	}
}

} // namespace direbot::ws
