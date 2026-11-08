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

#include "phone.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
// (Windows' old "near" and "far" words, which PhoneLook uses.)
#undef near
#undef far
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace direbot::face {

using json = nlohmann::json;

namespace {

// How long a phone that's stopped sending still counts as seeing you
// (seconds): then your face is lost, as a camera's would be.
constexpr double LOST = 0.5;
// How often it asks VTube Studio (or MeowFace) for more, and for how long
// each time (the app takes 0.5 to 10 seconds).
constexpr float ASK_EVERY = 1.0f, ASK_FOR = 3.0f;
// iFacialMocap: asked again while nothing comes for this long.
constexpr float HELLO_EVERY = 2.0f;
// A lost address (a name that isn't found, a network that isn't there) is
// looked up again this often.
constexpr float FIND_EVERY = 5.0f;
// The most a head is taken to be from the middle of the phone's picture
// across or up (cm), and how near or far (cm): past these the phone's
// numbers aren't what we think they are, so they're held in.
constexpr float MOST_ACROSS = 40, NEAREST = 12, FURTHEST = 200;

std::string lower(const std::string &s)
{
	std::string out = s;
	for (char &c : out)
		c = (char)std::tolower((unsigned char)c);
	return out;
}

bool ends_with(const std::string &s, const char *end)
{
	const size_t n = std::strlen(end);
	return s.size() >= n && s.compare(s.size() - n, n, end) == 0;
}

// "eyeBlink_L", "EyeBlinkLeft", "eyeBlinkLeft" -> "eyeblinkleft".
std::string plain_name(const std::string &name)
{
	std::string n = lower(name);
	if (ends_with(n, "_l"))
		n = n.substr(0, n.size() - 2) + "left";
	else if (ends_with(n, "_r"))
		n = n.substr(0, n.size() - 2) + "right";
	std::string out;
	for (char c : n)
		if (std::isalnum((unsigned char)c))
			out += c;
	return out;
}

// Apple's names (plain) -> ours, its left and right swapped.
const std::map<std::string, int> &shape_names()
{
	static const std::map<std::string, int> names = [] {
		std::map<std::string, int> m;
		for (int i = 1; i < EXPRESSIONS; i++) {
			std::string n = EXPRESSION_NAMES[i];
			if (ends_with(n, "Left"))
				n = n.substr(0, n.size() - 4) + "Right";
			else if (ends_with(n, "Right"))
				n = n.substr(0, n.size() - 5) + "Left";
			m[plain_name(n)] = i;
		}
		return m;
	}();
	return names;
}

float clamp01(float v)
{
	return v > 0 ? (v < 1 ? v : 1) : 0;
}

float clampf(float v, float lo, float hi)
{
	return std::min(hi, std::max(lo, v));
}

// Degrees, -180 to 180 (some apps say 350 for -10).
float around(double degrees)
{
	if (!std::isfinite(degrees))
		return 0;
	double d = std::fmod(degrees, 360.0);
	if (d > 180)
		d -= 360;
	else if (d <= -180)
		d += 360;
	return (float)d;
}

// A number in plain text, whatever language the PC is set to (a decimal
// point, never a comma).
bool number(const char *b, const char *e, double &out)
{
	while (b < e && (*b == ' ' || *b == '\t' || *b == '\r' || *b == '\n'))
		b++;
	while (e > b && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
		e--;
	if (b >= e)
		return false;
	bool minus = false;
	if (*b == '-' || *b == '+')
		minus = *b++ == '-';
	double v = 0;
	int digits = 0;
	while (b < e && *b >= '0' && *b <= '9') {
		v = v * 10 + (*b++ - '0');
		digits++;
	}
	if (b < e && *b == '.') {
		b++;
		double place = 0.1;
		while (b < e && *b >= '0' && *b <= '9') {
			v += (*b++ - '0') * place;
			place /= 10;
			digits++;
		}
	}
	if (!digits)
		return false;
	if (b < e && (*b == 'e' || *b == 'E')) {
		b++;
		bool down = false;
		if (b < e && (*b == '-' || *b == '+'))
			down = *b++ == '-';
		int power = 0, power_digits = 0;
		while (b < e && *b >= '0' && *b <= '9' && power < 400) {
			power = power * 10 + (*b++ - '0');
			power_digits++;
		}
		if (!power_digits)
			return false;
		v *= std::pow(10.0, down ? -power : power);
	}
	if (b != e || !std::isfinite(v))
		return false;
	out = minus ? -v : v;
	return true;
}

// A key of an object whatever its case ("FaceFound", "faceFound").
const json *key_of(const json &j, const char *key)
{
	if (!j.is_object())
		return nullptr;
	auto it = j.find(key);
	if (it != j.end())
		return &*it;
	const std::string want = lower(key);
	for (auto i = j.begin(); i != j.end(); ++i)
		if (lower(i.key()) == want)
			return &*i;
	return nullptr;
}

bool vector_of(const json *j, double v[3])
{
	if (!j)
		return false;
	if (j->is_array() && j->size() >= 3) {
		for (int k = 0; k < 3; k++) {
			if (!(*j)[k].is_number())
				return false;
			v[k] = (*j)[k].get<double>();
		}
		return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
	}
	const char *names[3] = {"x", "y", "z"};
	for (int k = 0; k < 3; k++) {
		const json *n = key_of(*j, names[k]);
		if (!n || !n->is_number())
			return false;
		v[k] = n->get<double>();
	}
	return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

// One of its moves (`seen`: which it sent, in our numbers; its tongue at
// EXPRESSIONS - one of the 52, though nothing of ours moves with it).
void take_shape(PhoneLook &out, const std::string &name, double value, bool seen[EXPRESSIONS + 1])
{
	if (!std::isfinite(value))
		return;
	const int i = phone_shape(name);
	const int slot = i >= 0 ? i : plain_name(name) == "tongueout" ? EXPRESSIONS : -1;
	if (slot < 0)
		return;
	if (i >= 0)
		out.expressions[i] = clamp01((float)value);
	if (!seen[slot]) {
		seen[slot] = true;
		out.shapes++;
	}
}

} // namespace

int phone_shape(const std::string &name)
{
	const auto &names = shape_names();
	const auto it = names.find(plain_name(name));
	return it == names.end() ? -1 : it->second;
}

bool read_vtube_studio(const char *text, size_t size, PhoneLook &out)
{
	const json j = json::parse(text, text + size, nullptr, false);
	if (!j.is_object())
		return false;
	const json *shapes = key_of(j, "BlendShapes");
	const json *found = key_of(j, "FaceFound");
	double rotation[3], position[3];
	const bool turned = vector_of(key_of(j, "Rotation"), rotation);
	const bool placed = vector_of(key_of(j, "Position"), position);
	if (!shapes && !found && !turned)
		return false;
	PhoneLook look;
	bool seen[EXPRESSIONS + 1] = {};
	if (shapes && shapes->is_array()) {
		for (const json &s : *shapes) {
			const json *k = key_of(s, "k"), *v = key_of(s, "v");
			if (k && k->is_string() && v && v->is_number())
				take_shape(look, k->get<std::string>(), v->get<double>(), seen);
		}
	} else if (shapes && shapes->is_object()) {
		for (auto it = shapes->begin(); it != shapes->end(); ++it)
			if (it->is_number())
				take_shape(look, it.key(), it->get<double>(), seen);
	}
	look.found = found && found->is_boolean() ? found->get<bool>() : look.shapes > 0;
	// Its turn: x round to the side (+ your own right), y up and down
	// (+ up), z the lean (+ towards your own right shoulder), degrees.
	if (turned) {
		look.turn.yaw = -around(rotation[0]);
		look.turn.pitch = around(rotation[1]);
		look.turn.roll = -around(rotation[2]);
	}
	// Where it is, across (+ your own right) and up, about a centimetre a
	// step; how near isn't said the same way by every app, so it isn't
	// used.
	if (placed) {
		look.x = clampf((float)-position[0], -MOST_ACROSS, MOST_ACROSS);
		look.y = clampf((float)position[1], -MOST_ACROSS, MOST_ACROSS);
		look.placed = true;
	}
	if (!look.found) {
		// Lost: nothing it says now is you.
		look = PhoneLook();
	}
	out = look;
	return true;
}

bool read_ifacialmocap(const char *text, size_t size, PhoneLook &out)
{
	PhoneLook look;
	bool seen[EXPRESSIONS + 1] = {}, head = false;
	const char *p = text, *end = text + size;
	int named = 0;
	while (p < end) {
		const char *bar = std::find(p, end, '|');
		std::string part(p, bar);
		p = bar < end ? bar + 1 : end;
		// Trim.
		size_t a = 0, b = part.size();
		while (a < b && std::isspace((unsigned char)part[a]))
			a++;
		while (b > a && std::isspace((unsigned char)part[b - 1]))
			b--;
		part = part.substr(a, b - a);
		if (part.empty())
			continue;
		const size_t hash = part.find('#');
		if (hash != std::string::npos) {
			std::string name = part.substr(0, hash);
			if (!name.empty() && name[0] == '=')
				name = name.substr(1);
			if (name != "head")
				continue; // its eyes: the eye moves say where they look
			double v[6];
			int n = 0;
			const char *q = part.c_str() + hash + 1, *qe = part.c_str() + part.size();
			while (q <= qe && n < 6) {
				const char *comma = std::find(q, qe, ',');
				if (!number(q, comma, v[n]))
					break;
				n++;
				q = comma + 1;
				if (comma == qe)
					break;
			}
			if (n < 3)
				continue;
			// Its turn: x the nod (+ down), y round to the side (+ your
			// own right), z the lean (+ towards your own left shoulder).
			look.turn.pitch = -around(v[0]);
			look.turn.yaw = -around(v[1]);
			look.turn.roll = around(v[2]);
			head = true;
			// Where it is (metres): across (+ your own right), up, and
			// towards the camera.
			if (n == 6) {
				look.x = clampf((float)(-v[3] * 100), -MOST_ACROSS, MOST_ACROSS);
				look.y = clampf((float)(v[4] * 100), -MOST_ACROSS, MOST_ACROSS);
				look.placed = true;
				const float z = (float)(v[5] * 100);
				if (z < -NEAREST && z > -FURTHEST) {
					look.z = z;
					look.far = true;
				}
			}
			continue;
		}
		// "name&value" (its second version) or "name-value".
		size_t split = part.find('&');
		if (split == std::string::npos)
			split = part.find('-');
		if (split == std::string::npos || split == 0)
			continue;
		double value;
		if (!number(part.c_str() + split + 1, part.c_str() + part.size(), value))
			continue;
		named++;
		take_shape(look, part.substr(0, split), value / 100, seen);
	}
	if (!head && !named)
		return false;
	look.found = true;
	out = look;
	return true;
}

std::string vtube_studio_request(int port, float seconds)
{
	return json{{"messageType", "iOSTrackingDataRequest"},
		    {"time", std::round(clampf(seconds, 0.5f, 10.0f) * 10) / 10},
		    {"sentBy", "DireBot"},
		    {"ports", json::array({port})}}
		.dump();
}

std::string ifacialmocap_hello(bool v2)
{
	std::string hello = "iFacialMocap_sahuasouryya9218sauhuiayeta91555dy3719";
	if (v2)
		hello += "|sendDataVersion=v2";
	return hello;
}

bool split_address(const std::string &text, int fallback, std::string &host, int &port)
{
	std::string t;
	for (char c : text)
		if (!std::isspace((unsigned char)c))
			t += c;
	// Typed with a web address's start: just the address.
	for (const char *start : {"udp://", "http://", "https://"})
		if (lower(t).rfind(start, 0) == 0)
			t = t.substr(std::strlen(start));
	if (!t.empty() && t.back() == '/')
		t.pop_back();
	std::string h, p;
	if (!t.empty() && t[0] == '[') {
		const size_t close = t.find(']');
		if (close == std::string::npos)
			return false;
		h = t.substr(1, close - 1);
		const std::string rest = t.substr(close + 1);
		if (!rest.empty()) {
			if (rest[0] != ':' || rest.size() == 1)
				return false;
			p = rest.substr(1);
		}
	} else if (std::count(t.begin(), t.end(), ':') == 1) {
		const size_t colon = t.find(':');
		h = t.substr(0, colon);
		p = t.substr(colon + 1);
		if (p.empty())
			return false;
	} else {
		h = t;
	}
	if (h.empty() || h.size() > 253)
		return false;
	for (char c : h)
		if (!(std::isalnum((unsigned char)c) || c == '.' || c == '-' || c == ':' || c == '%' || c == '_'))
			return false;
	int got = fallback;
	if (!p.empty()) {
		if (p.size() > 5 || !std::all_of(p.begin(), p.end(), [](char c) { return c >= '0' && c <= '9'; }))
			return false;
		got = std::stoi(p);
	}
	if (got < 1 || got > 65535)
		return false;
	host = h;
	port = got;
	return true;
}

// ---- the link -------------------------------------------------------------------

namespace {

#ifdef _WIN32
typedef SOCKET sock_t;
const sock_t NO_SOCKET = INVALID_SOCKET;
void close_socket(sock_t s)
{
	closesocket(s);
}
int poll_now(pollfd *fds, size_t count, int ms)
{
	return WSAPoll(fds, (ULONG)count, ms);
}
bool nonblocking(sock_t s)
{
	u_long on = 1;
	return ioctlsocket(s, FIONBIO, &on) == 0;
}
#else
typedef int sock_t;
const sock_t NO_SOCKET = -1;
void close_socket(sock_t s)
{
	::close(s);
}
int poll_now(pollfd *fds, size_t count, int ms)
{
	return ::poll(fds, (nfds_t)count, ms);
}
bool nonblocking(sock_t s)
{
	const int flags = fcntl(s, F_GETFL, 0);
	return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}
#endif

using clock_type = std::chrono::steady_clock;

double seconds_since(clock_type::time_point t)
{
	return std::chrono::duration<double>(clock_type::now() - t).count();
}

uint64_t now_ns()
{
	return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
		       std::chrono::steady_clock::now().time_since_epoch())
		.count();
}

std::string address_text(const sockaddr_storage &a)
{
	char text[INET6_ADDRSTRLEN] = {};
	if (a.ss_family == AF_INET)
		inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in &>(a).sin_addr, text, sizeof(text));
	else if (a.ss_family == AF_INET6)
		inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6 &>(a).sin6_addr, text, sizeof(text));
	return text;
}

// The same machine (any port)? An IPv6 socket can be sent to from an
// IPv4 address mapped into it.
bool same_host(const sockaddr_storage &a, const sockaddr_storage &b)
{
	auto v4_of = [](const sockaddr_storage &s, in_addr &out) {
		if (s.ss_family == AF_INET) {
			out = reinterpret_cast<const sockaddr_in &>(s).sin_addr;
			return true;
		}
		if (s.ss_family == AF_INET6) {
			const auto &six = reinterpret_cast<const sockaddr_in6 &>(s).sin6_addr;
			static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
			if (std::memcmp(&six, mapped, 12) == 0) {
				std::memcpy(&out, reinterpret_cast<const uint8_t *>(&six) + 12, 4);
				return true;
			}
		}
		return false;
	};
	in_addr x, y;
	if (v4_of(a, x) && v4_of(b, y))
		return std::memcmp(&x, &y, sizeof(x)) == 0;
	if (a.ss_family == AF_INET6 && b.ss_family == AF_INET6)
		return std::memcmp(&reinterpret_cast<const sockaddr_in6 &>(a).sin6_addr,
				   &reinterpret_cast<const sockaddr_in6 &>(b).sin6_addr, sizeof(in6_addr)) == 0;
	return false;
}

} // namespace

struct PhoneLink::Impl {
	Taker take;
	std::thread thread;
	mutable std::mutex lock;
	bool stop = false;
	// Asked for (under lock) and what the thread is doing.
	bool wanted = false, changed = false;
	PhoneApp app = PHONE_VTUBE_STUDIO;
	std::string address;
	std::atomic<float> ask_every{ASK_EVERY};
	State state;
#ifdef _WIN32
	bool wsa = false;
#endif

	void run();
	void set_problem(const std::string &words)
	{
		std::lock_guard<std::mutex> l(lock);
		state.problem = words;
	}
};

PhoneLink::PhoneLink(Taker take) : impl_(std::make_unique<Impl>())
{
	impl_->take = std::move(take);
#ifdef _WIN32
	WSADATA data;
	impl_->wsa = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif
	Impl *m = impl_.get();
	impl_->thread = std::thread([m] { m->run(); });
}

PhoneLink::~PhoneLink()
{
	{
		std::lock_guard<std::mutex> l(impl_->lock);
		impl_->stop = true;
	}
	if (impl_->thread.joinable())
		impl_->thread.join();
#ifdef _WIN32
	if (impl_->wsa)
		WSACleanup();
#endif
}

void PhoneLink::set(PhoneApp app, const std::string &address)
{
	std::lock_guard<std::mutex> l(impl_->lock);
	const bool want = !address.empty();
	if (want == impl_->wanted && app == impl_->app && address == impl_->address)
		return;
	impl_->wanted = want;
	impl_->app = app;
	impl_->address = address;
	impl_->changed = true;
}

void PhoneLink::stop()
{
	set(PHONE_VTUBE_STUDIO, "");
}

PhoneLink::State PhoneLink::state() const
{
	std::lock_guard<std::mutex> l(impl_->lock);
	return impl_->state;
}

void PhoneLink::set_ask_every(float seconds)
{
	impl_->ask_every = std::max(0.05f, seconds);
}

void PhoneLink::Impl::run()
{
	sock_t sock = NO_SOCKET;
	sockaddr_storage phone{};
	socklen_t phone_len = 0;
	bool have_phone = false;
	PhoneApp now_app = PHONE_VTUBE_STUDIO;
	std::string now_address, host;
	int phone_port = 0;
	clock_type::time_point last_ask{}, last_packet{}, last_find{}, window{clock_type::now()};
	bool asked = false, any_packet = false, lost_told = true, hello_v2 = true;
	uint64_t window_packets = 0;
	std::vector<char> buffer(64 * 1024);

	auto close_now = [&] {
		if (sock != NO_SOCKET)
			close_socket(sock);
		sock = NO_SOCKET;
		have_phone = false;
	};
	auto tell_lost = [&] {
		if (!lost_told && take)
			take(PhoneLook(), now_ns());
		lost_told = true;
	};
	// The phone's address found, and a socket of the same kind opened
	// (iFacialMocap's port 49983 for it; any free one for VTube Studio).
	auto open_now = [&]() -> bool {
		close_now();
		last_find = clock_type::now();
		std::string h;
		int p = 0;
		if (!split_address(now_address, now_app == PHONE_IFACIALMOCAP ? IFM_PORT : VTS_PORT, h, p)) {
			set_problem("That isn't an address - type the one your phone's app shows, like 192.168.1.23.");
			return false;
		}
		host = h;
		phone_port = p;
		addrinfo hints{}, *found = nullptr;
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_DGRAM;
		hints.ai_protocol = IPPROTO_UDP;
		const std::string port_text = std::to_string(p);
		if (getaddrinfo(h.c_str(), port_text.c_str(), &hints, &found) != 0 || !found) {
			set_problem("Can't find " + h + " - check the address your phone's app shows.");
			return false;
		}
		// An IPv4 address first: a phone's Wi-Fi nearly always has one.
		const addrinfo *pick = found;
		for (const addrinfo *a = found; a; a = a->ai_next)
			if (a->ai_family == AF_INET) {
				pick = a;
				break;
			}
		std::memcpy(&phone, pick->ai_addr, pick->ai_addrlen);
		phone_len = (socklen_t)pick->ai_addrlen;
		const int family = pick->ai_family;
		freeaddrinfo(found);
		sock = ::socket(family, SOCK_DGRAM, IPPROTO_UDP);
		if (sock == NO_SOCKET || !nonblocking(sock)) {
			close_now();
			set_problem("This PC wouldn't open a network socket for it.");
			return false;
		}
#ifdef _WIN32
		// A phone not listening yet must not stop the socket reading
		// (Windows reports it on the next read otherwise).
		BOOL no = FALSE;
		DWORD got = 0;
		WSAIoctl(sock, SIO_UDP_CONNRESET, &no, sizeof(no), nullptr, 0, &got, nullptr, nullptr);
#endif
		sockaddr_storage here{};
		socklen_t here_len = 0;
		const int our_port = now_app == PHONE_IFACIALMOCAP ? IFM_PORT : 0;
		if (family == AF_INET6) {
			auto &six = reinterpret_cast<sockaddr_in6 &>(here);
			six.sin6_family = AF_INET6; // any of ours: all 0 already
			six.sin6_port = htons((uint16_t)our_port);
			here_len = sizeof(sockaddr_in6);
		} else {
			auto &four = reinterpret_cast<sockaddr_in &>(here);
			four.sin_family = AF_INET;
			four.sin_addr.s_addr = htonl(INADDR_ANY);
			four.sin_port = htons((uint16_t)our_port);
			here_len = sizeof(sockaddr_in);
		}
		if (::bind(sock, reinterpret_cast<sockaddr *>(&here), here_len) != 0) {
			close_now();
			set_problem(now_app == PHONE_IFACIALMOCAP
					    ? "Another app on this PC is using iFacialMocap's port (49983) - close it "
					      "(VSeeFace, VMagicMirror, Warudo...) and this tries again."
					    : "This PC wouldn't let it listen for your phone.");
			return false;
		}
		sockaddr_storage bound{};
		socklen_t bound_len = sizeof(bound);
		int our = 0;
		if (getsockname(sock, reinterpret_cast<sockaddr *>(&bound), &bound_len) == 0)
			our = ntohs(bound.ss_family == AF_INET6 ? reinterpret_cast<sockaddr_in6 &>(bound).sin6_port
								: reinterpret_cast<sockaddr_in &>(bound).sin_port);
		have_phone = true;
		std::lock_guard<std::mutex> l(lock);
		state.listening = true;
		state.port = our;
		state.phone = (family == AF_INET6 ? "[" + h + "]" : h) + ":" + std::to_string(p);
		state.problem.clear();
		return true;
	};
	for (;;) {
		bool stopping, change;
		{
			std::lock_guard<std::mutex> l(lock);
			stopping = stop;
			change = changed;
			changed = false;
			if (change) {
				now_app = app;
				now_address = wanted ? address : std::string();
				state = State();
				state.on = wanted;
				state.app = app;
				state.address = address;
			}
		}
		if (stopping)
			break;
		if (change) {
			tell_lost();
			close_now();
			asked = false;
			any_packet = false;
			window_packets = 0;
			window = clock_type::now();
			if (!now_address.empty())
				open_now();
		}
		if (now_address.empty()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			continue;
		}
		if (!have_phone) {
			if (seconds_since(last_find) >= FIND_EVERY)
				open_now();
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			continue;
		}
		// Ask: VTube Studio every second; iFacialMocap only while nothing
		// comes (its first and its second way, in turn).
		const bool quiet = !any_packet || seconds_since(last_packet) > 1.0;
		const double every = now_app == PHONE_IFACIALMOCAP ? HELLO_EVERY : ask_every.load();
		if ((!asked || seconds_since(last_ask) >= every) && (now_app != PHONE_IFACIALMOCAP || quiet)) {
			int our_port;
			{
				std::lock_guard<std::mutex> l(lock);
				our_port = state.port;
			}
			const std::string words = now_app == PHONE_IFACIALMOCAP ? ifacialmocap_hello(hello_v2)
									  : vtube_studio_request(our_port, ASK_FOR);
			if (now_app == PHONE_IFACIALMOCAP)
				hello_v2 = !hello_v2;
			const auto sent = ::sendto(sock, words.data(), (int)words.size(), 0,
						   reinterpret_cast<const sockaddr *>(&phone), phone_len);
			last_ask = clock_type::now();
			asked = true;
			if (sent < 0)
				set_problem("Can't reach " + host +
					    " - is this PC on the same network (Wi-Fi) as your phone?");
			else if (!any_packet) {
				std::lock_guard<std::mutex> l(lock);
				if (state.problem.rfind("Can't reach", 0) == 0)
					state.problem.clear();
			}
		}
		pollfd fds{};
		fds.fd = sock;
		fds.events = POLLIN;
		const int ready = poll_now(&fds, 1, 50);
		if (ready > 0 && (fds.revents & POLLIN)) {
			// Everything waiting (the socket doesn't wait for more).
			for (int k = 0; k < 64; k++) {
				sockaddr_storage from{};
				socklen_t from_len = sizeof(from);
				const auto n = ::recvfrom(sock, buffer.data(), (int)buffer.size(), 0,
							  reinterpret_cast<sockaddr *>(&from), &from_len);
				if (n <= 0)
					break;
				if (!same_host(from, phone)) {
					std::lock_guard<std::mutex> l(lock);
					state.others++;
					state.other = address_text(from);
					continue;
				}
				PhoneLook look;
				const bool read = now_app == PHONE_IFACIALMOCAP
							  ? read_ifacialmocap(buffer.data(), (size_t)n, look)
							  : read_vtube_studio(buffer.data(), (size_t)n, look);
				if (!read) {
					std::lock_guard<std::mutex> l(lock);
					state.unread++;
					continue;
				}
				last_packet = clock_type::now();
				any_packet = true;
				lost_told = false;
				window_packets++;
				if (take)
					take(look, now_ns());
				std::lock_guard<std::mutex> l(lock);
				state.packets++;
				state.found = look.found;
				state.shapes = look.shapes;
				state.problem.clear();
			}
		} else if (ready < 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		// Gone quiet: your face is lost.
		if (any_packet && !lost_told && seconds_since(last_packet) > LOST)
			tell_lost();
		const double span = seconds_since(window);
		std::lock_guard<std::mutex> l(lock);
		if (span >= 1.0) {
			state.per_second = (float)((double)window_packets / span);
			window_packets = 0;
			window = clock_type::now();
		}
		state.since = any_packet ? (float)seconds_since(last_packet) : -1.0f;
		if (!any_packet)
			state.found = false;
	}
	tell_lost();
	close_now();
}

} // namespace direbot::face
