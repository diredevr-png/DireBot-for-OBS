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

#include "server.hpp"
#include "ws.hpp"

#include <chrono>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace direbot::link {

namespace {

using clock = std::chrono::steady_clock;

#ifdef _WIN32
typedef SOCKET sock_t;
const intptr_t NO_SOCKET = (intptr_t)INVALID_SOCKET;
inline sock_t S(intptr_t s)
{
	return (sock_t)s;
}
inline void close_socket(intptr_t s)
{
	closesocket(S(s));
}
inline bool would_block()
{
	int e = WSAGetLastError();
	return e == WSAEWOULDBLOCK || e == WSAEINTR;
}
inline int poll_now(pollfd *fds, size_t count, int ms)
{
	return WSAPoll(fds, (ULONG)count, ms);
}
inline bool nonblocking(intptr_t s)
{
	u_long on = 1;
	return ioctlsocket(S(s), FIONBIO, &on) == 0;
}
const int SEND_FLAGS = 0;
#else
typedef int sock_t;
const intptr_t NO_SOCKET = -1;
inline sock_t S(intptr_t s)
{
	return (sock_t)s;
}
inline void close_socket(intptr_t s)
{
	::close(S(s));
}
inline bool would_block()
{
	return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}
inline int poll_now(pollfd *fds, size_t count, int ms)
{
	return ::poll(fds, (nfds_t)count, ms);
}
inline bool nonblocking(intptr_t s)
{
	int flags = fcntl(S(s), F_GETFL, 0);
	return flags >= 0 && fcntl(S(s), F_SETFL, flags | O_NONBLOCK) == 0;
}
// A client that went away mid-send must never kill OBS with SIGPIPE.
const int SEND_FLAGS = MSG_NOSIGNAL;
#endif

constexpr auto HANDSHAKE_TIME = std::chrono::seconds(10);
constexpr auto IDENTIFY_TIME = std::chrono::seconds(60);
// A client this far behind on its mail skips audio meter updates...
constexpr size_t DROPPABLE_BEHIND = 1 * 1024 * 1024;
// ...and one this far behind is let go: it isn't reading.
constexpr size_t TOO_FAR_BEHIND = 64 * 1024 * 1024;
constexpr size_t MAX_JOBS = 512;

} // namespace

struct Server::Client {
	intptr_t sock = NO_SOCKET;
	uint64_t id = 0;
	clock::time_point started = clock::now();
	std::atomic<bool> open{false}; // past the handshake, session made
	std::atomic<bool> gone{false};
	bool closing = false; // network thread: close once `out` is sent
	std::string in;
	ws::Reader reader;
	std::unique_ptr<Session> session;

	std::mutex out_lock;
	std::deque<std::string> out;
	size_t out_bytes = 0;
	size_t front_sent = 0;
	std::atomic<bool> overflowed{false};

	// Guarded by the server's jobs_lock_.
	std::deque<Job> jobs;
	bool queued = false;
};

Server::Server(Backend &backend, Log log) : backend_(backend), log_(std::move(log)) {}

Server::~Server()
{
	stop();
}

size_t Server::clients() const
{
	std::lock_guard<std::mutex> l(clients_lock_);
	return clients_.size();
}

uint16_t Server::start(const Options &options)
{
	if (running_)
		return port_;
	options_ = options;
	stopping_ = false;
#ifdef _WIN32
	WSADATA data;
	if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
		log_(Warning, "Windows networking didn't start, so the link is off.");
		return 0;
	}
	sockets_started_ = true;
#endif
	auto give_up = [&](const char *why) -> uint16_t {
		log_(Warning, why);
		if (listener_ != NO_SOCKET)
			close_socket(listener_);
		if (waker_ != NO_SOCKET)
			close_socket(waker_);
		listener_ = waker_ = NO_SOCKET;
		port_ = 0;
#ifdef _WIN32
		if (sockets_started_)
			WSACleanup();
		sockets_started_ = false;
#endif
		return 0;
	};

	listener_ = (intptr_t)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listener_ == NO_SOCKET)
		return give_up("Couldn't open a socket for the link.");
	{
		int on = 1;
#ifdef _WIN32
		// Nobody else can take the port over while the link has it.
		setsockopt(S(listener_), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&on, sizeof(on));
#else
		setsockopt(S(listener_), SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on));
#endif
	}
	sockaddr_in addr;
	std::memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(options_.preferred_port);
	if (bind(S(listener_), (sockaddr *)&addr, sizeof(addr)) != 0) {
		// Taken (or not allowed): any free port will do, the link file
		// says which.
		addr.sin_port = 0;
		if (bind(S(listener_), (sockaddr *)&addr, sizeof(addr)) != 0)
			return give_up("Couldn't find a free port for the link.");
	}
	if (listen(S(listener_), 16) != 0 || !nonblocking(listener_))
		return give_up("Couldn't listen for DireBot.");
	socklen_t len = sizeof(addr);
	getsockname(S(listener_), (sockaddr *)&addr, &len);
	port_ = ntohs(addr.sin_port);

	// A tiny UDP socket on this PC that other threads poke to wake the
	// network thread when there's something to send.
	waker_ = (intptr_t)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (waker_ == NO_SOCKET)
		return give_up("Couldn't open the link's wake-up socket.");
	sockaddr_in wake;
	std::memset(&wake, 0, sizeof(wake));
	wake.sin_family = AF_INET;
	wake.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	wake.sin_port = 0;
	if (bind(S(waker_), (sockaddr *)&wake, sizeof(wake)) != 0 || !nonblocking(waker_))
		return give_up("Couldn't set up the link's wake-up socket.");
	len = sizeof(wake);
	getsockname(S(waker_), (sockaddr *)&wake, &len);
	waker_address_.assign((const char *)&wake, sizeof(wake));

	running_ = true;
	network_ = std::thread(&Server::network_loop, this);
	for (size_t i = 0; i < (options_.workers ? options_.workers : 1); i++)
		workers_.emplace_back(&Server::worker_loop, this);
	return port_;
}

void Server::begin_stop()
{
	stopping_ = true;
}

void Server::stop()
{
	if (!running_ && !network_.joinable())
		return;
	stopping_ = true;
	running_ = false;
	wake();
	if (network_.joinable())
		network_.join();
	{
		std::lock_guard<std::mutex> l(jobs_lock_);
		jobs_ready_.notify_all();
	}
	for (auto &t : workers_)
		if (t.joinable())
			t.join();
	workers_.clear();
	{
		std::lock_guard<std::mutex> l(jobs_lock_);
		ready_.clear();
	}
	if (waker_ != NO_SOCKET)
		close_socket(waker_);
	waker_ = NO_SOCKET;
#ifdef _WIN32
	if (sockets_started_)
		WSACleanup();
	sockets_started_ = false;
#endif
	port_ = 0;
}

void Server::wake()
{
	if (waker_ == NO_SOCKET || waker_address_.empty())
		return;
	const char poke = 1;
	sendto(S(waker_), &poke, 1, 0, (const sockaddr *)waker_address_.data(), (socklen_t)waker_address_.size());
}

void Server::broadcast(uint64_t intent, const std::string &message, bool droppable)
{
	if (!running_ || !(wanted_ & intent))
		return;
	std::vector<std::shared_ptr<Client>> to;
	{
		std::lock_guard<std::mutex> l(clients_lock_);
		for (auto &c : clients_) {
			if (c->open && !c->gone && c->session->identified() && (c->session->subscriptions() & intent))
				to.push_back(c);
		}
	}
	if (to.empty())
		return;
	const std::string f = ws::frame(ws::Text, message);
	for (auto &c : to)
		send(c, f, droppable);
}

void Server::send(const std::shared_ptr<Client> &client, const std::string &frame, bool droppable)
{
	{
		std::lock_guard<std::mutex> l(client->out_lock);
		if (client->gone || client->overflowed)
			return;
		if (droppable && client->out_bytes > DROPPABLE_BEHIND)
			return;
		if (client->out_bytes + frame.size() > TOO_FAR_BEHIND) {
			client->overflowed = true;
		} else {
			client->out.push_back(frame);
			client->out_bytes += frame.size();
		}
	}
	wake();
}

void Server::close_with(const std::shared_ptr<Client> &client, uint16_t code, const std::string &reason)
{
	send(client, ws::close_frame(code, reason));
	client->closing = true;
}

void Server::queue_job(const std::shared_ptr<Client> &client, Job job)
{
	std::lock_guard<std::mutex> l(jobs_lock_);
	if (client->jobs.size() >= MAX_JOBS) {
		client->overflowed = true; // it's flooding: let it go
		return;
	}
	client->jobs.push_back(std::move(job));
	if (!client->queued) {
		client->queued = true;
		ready_.push_back(client);
		jobs_ready_.notify_one();
	}
}

void Server::worker_loop()
{
	std::unique_lock<std::mutex> l(jobs_lock_);
	for (;;) {
		jobs_ready_.wait(l, [this] { return !running_ || !ready_.empty(); });
		if (ready_.empty()) {
			if (!running_)
				return;
			continue;
		}
		std::shared_ptr<Client> c = ready_.front();
		ready_.pop_front();
		if (c->jobs.empty()) {
			c->queued = false;
			continue;
		}
		Job job = std::move(c->jobs.front());
		c->jobs.pop_front();
		l.unlock();

		std::string reply;
		if (!c->gone) {
			try {
				reply = c->session->run(job.op, job.payload, backend_, stopping_);
			} catch (const std::exception &) {
				reply.clear();
			}
		}
		if (!reply.empty() && !c->gone)
			send(c, ws::frame(ws::Text, reply));

		l.lock();
		// One at a time per client, so its answers come back in order.
		if (!c->jobs.empty() && !c->gone && running_)
			ready_.push_back(c);
		else
			c->queued = false;
	}
}

void Server::recount_wanted()
{
	uint64_t bits = 0;
	{
		std::lock_guard<std::mutex> l(clients_lock_);
		for (auto &c : clients_)
			if (c->open && !c->gone && c->session->identified())
				bits |= c->session->subscriptions();
	}
	if (bits != wanted_.exchange(bits) && on_wanted_changed)
		on_wanted_changed(bits);
}

void Server::drop_client(const std::shared_ptr<Client> &client)
{
	if (client->gone.exchange(true))
		return;
	if (client->sock != NO_SOCKET)
		close_socket(client->sock);
	client->sock = NO_SOCKET;
	{
		std::lock_guard<std::mutex> l(clients_lock_);
		for (auto it = clients_.begin(); it != clients_.end(); ++it) {
			if (it->get() == client.get()) {
				clients_.erase(it);
				break;
			}
		}
	}
	{
		std::lock_guard<std::mutex> l(jobs_lock_);
		client->jobs.clear();
	}
	recount_wanted();
}

void Server::accept_clients()
{
	for (int i = 0; i < 16; i++) {
		sockaddr_in from;
		socklen_t len = sizeof(from);
		intptr_t s = (intptr_t)accept(S(listener_), (sockaddr *)&from, &len);
		if (s == NO_SOCKET)
			return;
		// Bound to 127.0.0.1, so this is always this PC - but be sure.
		if (from.sin_family != AF_INET || (ntohl(from.sin_addr.s_addr) >> 24) != 127) {
			close_socket(s);
			continue;
		}
		size_t count;
		{
			std::lock_guard<std::mutex> l(clients_lock_);
			count = clients_.size();
		}
		if (count >= options_.max_clients || !nonblocking(s)) {
			close_socket(s);
			continue;
		}
		int on = 1;
		setsockopt(S(s), IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof(on));
		auto c = std::make_shared<Client>();
		c->sock = s;
		c->id = next_id_++;
		std::lock_guard<std::mutex> l(clients_lock_);
		clients_.push_back(c);
	}
}

void Server::read_from(const std::shared_ptr<Client> &client)
{
	char buffer[64 * 1024];
	for (int i = 0; i < 64; i++) {
		int n = (int)recv(S(client->sock), buffer, (int)sizeof(buffer), 0);
		if (n > 0) {
			client->in.append(buffer, (size_t)n);
			if (n < (int)sizeof(buffer))
				break;
			continue;
		}
		if (n < 0 && would_block())
			break;
		drop_client(client); // closed, or broken
		return;
	}
	process(client);
}

void Server::process(const std::shared_ptr<Client> &client)
{
	if (client->gone || client->closing)
		return;
	if (!client->open) {
		ws::Handshake h = ws::read_handshake(client->in);
		if (h.state == ws::Handshake::State::NeedMore)
			return;
		{
			std::lock_guard<std::mutex> l(client->out_lock);
			client->out.push_back(h.response);
			client->out_bytes += h.response.size();
		}
		client->in.erase(0, h.consumed);
		if (h.state == ws::Handshake::State::Reject) {
			log_(Info, "Turned a connection away: " + h.why + ".");
			client->closing = true;
			return;
		}
		client->session = std::make_unique<Session>(options_.password, options_.websocket_version);
		client->open = true;
		send(client, ws::frame(ws::Text, client->session->hello()));
	}

	ws::Reader::Item item;
	while (!client->closing && client->reader.next(client->in, item)) {
		switch (item.kind) {
		case ws::Reader::Item::Kind::Text:
		case ws::Reader::Item::Kind::Binary: {
			Session::Outcome o = item.kind == ws::Reader::Item::Kind::Text
						     ? client->session->handle(item.data)
						     : client->session->binary_message();
			if (!o.reply.empty())
				send(client, ws::frame(ws::Text, o.reply));
			if (o.subscriptions_changed)
				recount_wanted();
			if (o.work)
				queue_job(client, Job{o.op, std::move(o.payload)});
			if (o.close != close_code::DontClose) {
				if (o.close == close_code::AuthenticationFailed)
					log_(Info, "Turned a connection away: the password was wrong.");
				close_with(client, o.close, o.reason);
			}
			break;
		}
		case ws::Reader::Item::Kind::Ping:
			send(client, ws::frame(ws::Pong, item.data));
			break;
		case ws::Reader::Item::Kind::Pong:
			break;
		case ws::Reader::Item::Kind::Close:
			close_with(client, item.code ? item.code : 1000, "");
			break;
		case ws::Reader::Item::Kind::Error:
			close_with(client, item.code, item.data);
			break;
		}
	}
}

void Server::write_to(const std::shared_ptr<Client> &client)
{
	std::unique_lock<std::mutex> l(client->out_lock);
	while (!client->out.empty()) {
		std::string &front = client->out.front();
		const char *at = front.data() + client->front_sent;
		int left = (int)(front.size() - client->front_sent);
		int n = (int)::send(S(client->sock), at, left, SEND_FLAGS);
		if (n < 0) {
			if (would_block())
				return;
			l.unlock();
			drop_client(client);
			return;
		}
		client->front_sent += (size_t)n;
		if (client->front_sent < front.size())
			return;
		client->out_bytes -= front.size();
		client->front_sent = 0;
		client->out.pop_front();
	}
}

void Server::network_loop()
{
	std::vector<pollfd> fds;
	std::vector<std::shared_ptr<Client>> polled;
	while (running_) {
		fds.clear();
		polled.clear();
		fds.push_back(pollfd{S(listener_), POLLIN, 0});
		fds.push_back(pollfd{S(waker_), POLLIN, 0});
		{
			std::lock_guard<std::mutex> l(clients_lock_);
			polled = clients_;
		}
		for (auto &c : polled) {
			short events = POLLIN;
			{
				std::lock_guard<std::mutex> l(c->out_lock);
				if (!c->out.empty())
					events |= POLLOUT;
			}
			fds.push_back(pollfd{S(c->sock), events, 0});
		}
		int ready = poll_now(fds.data(), fds.size(), 250);
		if (!running_)
			break;
		if (ready > 0) {
			if (fds[1].revents & POLLIN) {
				char drain[256];
				while (recv(S(waker_), drain, (int)sizeof(drain), 0) > 0) {
				}
			}
			if (fds[0].revents & POLLIN)
				accept_clients();
			for (size_t i = 0; i < polled.size(); i++) {
				auto &c = polled[i];
				short got = fds[i + 2].revents;
				if (c->gone)
					continue;
				if (got & (POLLERR | POLLNVAL)) {
					drop_client(c);
					continue;
				}
				if (got & (POLLIN | POLLHUP))
					read_from(c);
				if (!c->gone && (got & POLLOUT))
					write_to(c);
			}
		}
		// Whatever was queued while we slept, and the tidying up.
		const auto now = clock::now();
		for (auto &c : polled) {
			if (c->gone)
				continue;
			bool overflowed, empty;
			{
				std::lock_guard<std::mutex> l(c->out_lock);
				overflowed = c->overflowed;
				empty = c->out.empty();
			}
			if (overflowed) {
				log_(Warning, "Let a connection go: it stopped reading.");
				drop_client(c);
				continue;
			}
			if (!empty)
				write_to(c);
			if (c->gone)
				continue;
			{
				std::lock_guard<std::mutex> l(c->out_lock);
				empty = c->out.empty();
			}
			if (c->closing && empty) {
				drop_client(c);
				continue;
			}
			if (!c->open && now - c->started > HANDSHAKE_TIME) {
				drop_client(c);
				continue;
			}
			if (c->open && !c->session->identified() && now - c->started > IDENTIFY_TIME)
				close_with(c, close_code::NotIdentified, "You didn't identify in time.");
		}
	}

	// Closing: everyone hears so, then goes.
	std::vector<std::shared_ptr<Client>> all;
	{
		std::lock_guard<std::mutex> l(clients_lock_);
		all = clients_;
	}
	for (auto &c : all) {
		if (c->open) {
			std::string bye = ws::close_frame(1001, "OBS is closing.");
			::send(S(c->sock), bye.data(), (int)bye.size(), SEND_FLAGS);
		}
		drop_client(c);
	}
	if (listener_ != NO_SOCKET)
		close_socket(listener_);
	listener_ = NO_SOCKET;
}

} // namespace direbot::link
