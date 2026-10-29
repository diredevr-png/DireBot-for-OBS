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

// The link's own little server: on 127.0.0.1 only (never the network),
// for a handful of clients (DireBot opens three or four connections).
//
// One thread does all the network work; requests run on a couple of
// worker threads so a slow one never holds up events, and each client's
// requests are answered in the order it sent them.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "protocol.hpp"

namespace direbot::link {

class Server {
public:
	enum LogLevel { Info, Warning };
	using Log = std::function<void(LogLevel, const std::string &)>;

	struct Options {
		std::string password;
		std::string websocket_version;
		uint16_t preferred_port = 4466;
		size_t workers = 2;
		size_t max_clients = 24;
	};

	Server(Backend &backend, Log log);
	~Server();
	Server(const Server &) = delete;
	Server &operator=(const Server &) = delete;

	// Starts listening. The port it got (the preferred one if it was free),
	// or 0 when it couldn't listen at all.
	uint16_t start(const Options &options);
	// No new requests start; ones already running finish (OBS is closing).
	void begin_stop();
	// Closes every connection and waits for the threads.
	void stop();

	uint16_t port() const { return port_; }
	bool running() const { return running_; }
	size_t clients() const;

	// Thread-safe: sends `message` (a whole JSON message) to every client
	// that logged in and asked for `intent`. A `droppable` message (the
	// audio meters, many a second) is skipped for a client that's behind.
	void broadcast(uint64_t intent, const std::string &message, bool droppable = false);

	// Every event bit some logged-in client wants (for the audio meters,
	// which cost something and only run while someone listens).
	uint64_t wanted() const { return wanted_; }
	// Called from the network thread whenever wanted() changes.
	std::function<void(uint64_t)> on_wanted_changed;

private:
	struct Client;
	struct Job {
		int op;
		json payload;
	};

	Backend &backend_;
	Log log_;
	Options options_;
	std::atomic<bool> running_{false};
	std::atomic<bool> stopping_{false};
	std::atomic<uint64_t> wanted_{0};
	std::atomic<uint16_t> port_{0};

	intptr_t listener_ = -1;
	intptr_t waker_ = -1;
	std::string waker_address_;
	bool sockets_started_ = false;

	mutable std::mutex clients_lock_;
	std::vector<std::shared_ptr<Client>> clients_;
	uint64_t next_id_ = 1;

	std::thread network_;
	std::vector<std::thread> workers_;
	std::mutex jobs_lock_;
	std::condition_variable jobs_ready_;
	std::deque<std::shared_ptr<Client>> ready_; // clients with a job waiting

	void wake();
	void network_loop();
	void worker_loop();
	void accept_clients();
	void read_from(const std::shared_ptr<Client> &client);
	void write_to(const std::shared_ptr<Client> &client);
	void process(const std::shared_ptr<Client> &client);
	void queue_job(const std::shared_ptr<Client> &client, Job job);
	void send(const std::shared_ptr<Client> &client, const std::string &frame, bool droppable = false);
	void close_with(const std::shared_ptr<Client> &client, uint16_t code, const std::string &reason);
	void recount_wanted();
	void drop_client(const std::shared_ptr<Client> &client);
};

} // namespace direbot::link
