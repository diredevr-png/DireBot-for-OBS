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

// The WebSocket wire format (RFC 6455), as much as the link needs: the
// opening handshake and frames. No sockets here, so all of it is tested on
// its own.

#pragma once

#include <cstdint>
#include <string>

namespace direbot::ws {

// The opening handshake: an HTTP GET that asks to upgrade.
struct Handshake {
	enum class State { NeedMore, Accept, Reject };
	State state = State::NeedMore;
	std::string response;  // what to send back (101, or the refusal)
	std::string why;       // a refusal, in words, for the log
	size_t consumed = 0;   // bytes of the buffer the request used
	bool json_protocol = false;
};

// Reads the request at the start of `buffer`. Only this PC may connect: the
// Host has to be this PC, and a web page (anything with an Origin that isn't
// this PC) is turned away, so a website open in a browser can't drive OBS.
Handshake read_handshake(const std::string &buffer, size_t max_bytes = 16 * 1024);

std::string accept_key(const std::string &client_key);
bool is_this_pc(const std::string &host_and_port);

enum Opcode : uint8_t {
	Continuation = 0x0,
	Text = 0x1,
	Binary = 0x2,
	Close = 0x8,
	Ping = 0x9,
	Pong = 0xA,
};

// One server-to-client frame (servers never mask).
std::string frame(uint8_t opcode, const std::string &payload);
std::string close_frame(uint16_t code, const std::string &reason);

// Turns the bytes a client sends into whole messages.
class Reader {
public:
	explicit Reader(size_t max_message = 32 * 1024 * 1024) : max_message_(max_message) {}

	struct Item {
		enum class Kind { Text, Binary, Ping, Pong, Close, Error };
		Kind kind = Kind::Error;
		std::string data;
		uint16_t code = 0; // Close: the client's code. Error: the code to close with.
	};

	// Takes one item from the front of `buffer` (removing what it used).
	// False when more bytes are needed first.
	bool next(std::string &buffer, Item &out);

private:
	size_t max_message_;
	std::string partial_;
	uint8_t partial_opcode_ = 0;
	bool in_message_ = false;
};

} // namespace direbot::ws
