// Unit tests for the parts of DireBot for OBS that don't need OBS: hashing,
// the WebSocket handshake and frames, the obs-websocket protocol, and the
// usage report's "is this address on this PC" check.
// Build and run: tools/test-link.sh (Linux, any C++17 compiler).

#include "../src/link/crypto.hpp"
#include "../src/link/protocol.hpp"
#include "../src/link/ws.hpp"
#include "../src/obs/address.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace direbot;

static int failures = 0;
static int passes = 0;

#define CHECK(cond)                                                              \
	do {                                                                     \
		if (cond) {                                                      \
			passes++;                                                \
		} else {                                                         \
			failures++;                                              \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                \
	} while (0)

static std::string hex(const std::string &raw)
{
	static const char *d = "0123456789abcdef";
	std::string out;
	for (unsigned char c : raw) {
		out.push_back(d[c >> 4]);
		out.push_back(d[c & 15]);
	}
	return out;
}

// What a client sends: masked frames.
static std::string client_frame(uint8_t opcode, const std::string &payload, bool fin = true)
{
	std::string out;
	out.push_back((char)((fin ? 0x80 : 0) | opcode));
	const unsigned char mask[4] = {0x12, 0x34, 0x56, 0x78};
	size_t n = payload.size();
	if (n < 126) {
		out.push_back((char)(0x80 | n));
	} else if (n <= 0xffff) {
		out.push_back((char)(0x80 | 126));
		out.push_back((char)(n >> 8));
		out.push_back((char)(n & 0xff));
	} else {
		out.push_back((char)(0x80 | 127));
		for (int i = 7; i >= 0; i--)
			out.push_back((char)((uint64_t)n >> (i * 8)));
	}
	out.append((const char *)mask, 4);
	for (size_t i = 0; i < n; i++)
		out.push_back((char)(payload[i] ^ mask[i % 4]));
	return out;
}

static void test_hashes()
{
	CHECK(hex(crypto::sha1("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
	CHECK(hex(crypto::sha1("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
	CHECK(hex(crypto::sha256("")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	CHECK(hex(crypto::sha256("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	CHECK(hex(crypto::sha256(std::string(1000, 'a'))) ==
	      "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
	CHECK(hex(crypto::sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
	      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	CHECK(crypto::base64("") == "");
	CHECK(crypto::base64("f") == "Zg==");
	CHECK(crypto::base64("fo") == "Zm8=");
	CHECK(crypto::base64("foo") == "Zm9v");
	CHECK(crypto::base64("foobar") == "Zm9vYmFy");
	CHECK(crypto::base64url(std::string("\xfb\xff", 2)) == "-_8");
	CHECK(crypto::random_bytes(32).size() == 32);
	CHECK(crypto::random_bytes(32) != crypto::random_bytes(32));
	CHECK(crypto::same("abc", "abc"));
	CHECK(!crypto::same("abc", "abd"));
	CHECK(!crypto::same("abc", "ab"));
}

static std::string request(const std::string &extra)
{
	return "GET / HTTP/1.1\r\nHost: 127.0.0.1:4466\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
	       "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n" +
	       extra + "\r\n";
}

static void test_handshake()
{
	// The example from RFC 6455.
	CHECK(ws::accept_key("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

	auto h = ws::read_handshake("GET / HTTP/1.1\r\nHost: 127.0.0.1");
	CHECK(h.state == ws::Handshake::State::NeedMore);

	h = ws::read_handshake(request(""));
	CHECK(h.state == ws::Handshake::State::Accept);
	CHECK(h.response.find("101 Switching Protocols") != std::string::npos);
	CHECK(h.response.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
	CHECK(h.consumed == request("").size());

	// What websocket-client (DireBot's) sends.
	h = ws::read_handshake(request("Origin: http://127.0.0.1:4466\r\n"));
	CHECK(h.state == ws::Handshake::State::Accept);
	h = ws::read_handshake(request("Origin: http://localhost:4466\r\nSec-WebSocket-Protocol: obswebsocket.json\r\n"));
	CHECK(h.state == ws::Handshake::State::Accept);
	CHECK(h.json_protocol);
	CHECK(h.response.find("Sec-WebSocket-Protocol: obswebsocket.json") != std::string::npos);

	// A web page in a browser.
	h = ws::read_handshake(request("Origin: https://evil.example\r\n"));
	CHECK(h.state == ws::Handshake::State::Reject);
	CHECK(h.response.find("403") != std::string::npos);
	h = ws::read_handshake(request("Origin: null\r\n"));
	CHECK(h.state == ws::Handshake::State::Reject);
	h = ws::read_handshake(request("Origin: http://127.0.0.1.evil.example\r\n"));
	CHECK(h.state == ws::Handshake::State::Reject);

	// Another PC's name for it (DNS rebinding) - refused.
	std::string other = request("");
	other.replace(other.find("127.0.0.1:4466"), 14, "evil.example");
	h = ws::read_handshake(other);
	CHECK(h.state == ws::Handshake::State::Reject);

	// Not a WebSocket at all.
	h = ws::read_handshake("GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
	CHECK(h.state == ws::Handshake::State::Reject);
	h = ws::read_handshake("POST / HTTP/1.1\r\n\r\n");
	CHECK(h.state == ws::Handshake::State::Reject);
	h = ws::read_handshake(std::string(20000, 'x'));
	CHECK(h.state == ws::Handshake::State::Reject);

	CHECK(ws::is_this_pc("127.0.0.1"));
	CHECK(ws::is_this_pc("[::1]:4466"));
	CHECK(ws::is_this_pc("LOCALHOST:1"));
	CHECK(!ws::is_this_pc("192.168.1.2:4466"));
	CHECK(!ws::is_this_pc(""));
}

static void test_frames()
{
	ws::Reader r;
	ws::Reader::Item item;
	std::string buf = client_frame(ws::Text, "hello");
	std::string half = buf.substr(0, 3);
	CHECK(!r.next(half, item));
	CHECK(r.next(buf, item));
	CHECK(item.kind == ws::Reader::Item::Kind::Text);
	CHECK(item.data == "hello");
	CHECK(buf.empty());

	// Longer ones, both length forms.
	std::string big(300, 'x');
	buf = client_frame(ws::Text, big);
	CHECK(r.next(buf, item) && item.data == big);
	std::string bigger(70000, 'y');
	buf = client_frame(ws::Text, bigger);
	CHECK(r.next(buf, item) && item.data == bigger);

	// In pieces, with a ping in the middle.
	buf = client_frame(ws::Text, "hel", false) + client_frame(ws::Ping, "p") +
	      client_frame(ws::Continuation, "lo", true);
	CHECK(r.next(buf, item) && item.kind == ws::Reader::Item::Kind::Ping && item.data == "p");
	CHECK(r.next(buf, item) && item.kind == ws::Reader::Item::Kind::Text && item.data == "hello");

	// Close with a code.
	buf = client_frame(ws::Close, std::string("\x03\xe8", 2) + "bye");
	CHECK(r.next(buf, item) && item.kind == ws::Reader::Item::Kind::Close && item.code == 1000 &&
	      item.data == "bye");

	// Unmasked: not allowed from a client.
	buf = ws::frame(ws::Text, "x");
	CHECK(r.next(buf, item) && item.kind == ws::Reader::Item::Kind::Error && item.code == 1002);

	// Too big.
	ws::Reader small(10);
	buf = client_frame(ws::Text, std::string(11, 'z'));
	CHECK(small.next(buf, item) && item.kind == ws::Reader::Item::Kind::Error && item.code == 1009);

	// What the server sends.
	std::string f = ws::frame(ws::Text, "hi");
	CHECK(f.size() == 4 && (unsigned char)f[0] == 0x81 && f[1] == 2 && f.substr(2) == "hi");
	f = ws::frame(ws::Text, std::string(200, 'a'));
	CHECK((unsigned char)f[1] == 126 && f.size() == 204);
	f = ws::frame(ws::Text, std::string(70000, 'a'));
	CHECK((unsigned char)f[1] == 127 && f.size() == 70010);
	f = ws::close_frame(4009, "Authentication failed.");
	CHECK((unsigned char)f[0] == 0x88 && (unsigned char)f[2] == 0x0f && (unsigned char)f[3] == 0xa9);
}

struct FakeBackend : link::Backend {
	int calls = 0;
	link::RequestResult call(const std::string &type, const link::json &data) override
	{
		calls++;
		if (type == "GetVersion")
			return {link::status::Success, "", {{"obsVersion", "32.2.2"}}};
		if (type == "Echo")
			return {link::status::Success, "", data};
		if (type == "SetThing")
			return {link::status::Success, "", nullptr};
		return {link::status::UnknownRequestType, "Your request type is not valid.", nullptr};
	}
};

static link::json parse(const std::string &s)
{
	return link::json::parse(s);
}

static void test_protocol()
{
	using link::json;
	link::Session s("secret-word", "5.6.3");
	json hello = parse(s.hello());
	CHECK(hello["op"] == 0);
	CHECK(hello["d"]["rpcVersion"] == 1);
	CHECK(hello["d"]["obsWebSocketVersion"] == "5.6.3");
	const std::string salt = hello["d"]["authentication"]["salt"];
	const std::string challenge = hello["d"]["authentication"]["challenge"];
	CHECK(salt.size() == 44 && challenge.size() == 44);

	// Nothing before logging in.
	auto o = s.handle(R"({"op":6,"d":{"requestType":"GetVersion","requestId":"1"}})");
	CHECK(o.close == link::close_code::NotIdentified);

	// The wrong password.
	link::Session wrong("secret-word", "");
	json wh = parse(wrong.hello());
	std::string bad = link::Session::auth_string("not-it", wh["d"]["authentication"]["salt"],
						     wh["d"]["authentication"]["challenge"]);
	o = wrong.handle(json({{"op", 1}, {"d", {{"rpcVersion", 1}, {"authentication", bad}}}}).dump());
	CHECK(o.close == link::close_code::AuthenticationFailed);
	CHECK(!wrong.identified());
	o = wrong.handle(R"({"op":1,"d":{"rpcVersion":1}})");
	CHECK(o.close == link::close_code::AuthenticationFailed);

	// Broken messages.
	CHECK(s.handle("not json").close == link::close_code::MessageDecodeError);
	CHECK(s.handle("[1,2]").close == link::close_code::MessageDecodeError);
	CHECK(s.handle(R"({"d":{}})").close == link::close_code::MissingDataField);
	CHECK(s.handle(R"({"op":1})").close == link::close_code::MissingDataField);
	CHECK(s.handle(R"({"op":"1","d":{}})").close == link::close_code::InvalidDataFieldType);
	CHECK(s.binary_message().close == link::close_code::MessageDecodeError);

	// The right one (how obsws-python works it out), and only meters.
	std::string good = link::Session::auth_string("secret-word", salt, challenge);
	o = s.handle(json({{"op", 1},
			   {"d", {{"rpcVersion", 1}, {"authentication", good}, {"eventSubscriptions", 1 << 16}}}})
			     .dump());
	CHECK(o.close == link::close_code::DontClose);
	CHECK(parse(o.reply)["op"] == 2);
	CHECK(parse(o.reply)["d"]["negotiatedRpcVersion"] == 1);
	CHECK(o.subscriptions_changed);
	CHECK(s.identified());
	CHECK(s.subscriptions() == (1u << 16));
	CHECK(s.handle(json({{"op", 1}, {"d", {{"rpcVersion", 1}, {"authentication", good}}}}).dump()).close ==
	      link::close_code::AlreadyIdentified);

	// Reidentify.
	o = s.handle(R"({"op":3,"d":{"eventSubscriptions":4}})");
	CHECK(parse(o.reply)["op"] == 2 && s.subscriptions() == 4);
	CHECK(s.handle(R"({"op":3,"d":{"eventSubscriptions":-1}})").close == link::close_code::InvalidDataFieldType);

	// Requests run elsewhere, then answer with the client's own id (a
	// number is fine - obsws-python sends one).
	o = s.handle(R"({"op":6,"d":{"requestType":"GetVersion","requestId":42}})");
	CHECK(o.work && o.op == 6);
	FakeBackend backend;
	std::atomic<bool> stopping{false};
	json r = parse(s.run(o.op, o.payload, backend, stopping));
	CHECK(r["op"] == 7);
	CHECK(r["d"]["requestId"] == 42);
	CHECK(r["d"]["requestType"] == "GetVersion");
	CHECK(r["d"]["requestStatus"]["result"] == true);
	CHECK(r["d"]["requestStatus"]["code"] == 100);
	CHECK(r["d"]["responseData"]["obsVersion"] == "32.2.2");

	o = s.handle(R"({"op":6,"d":{"requestType":"Nope","requestId":"x"}})");
	r = parse(s.run(o.op, o.payload, backend, stopping));
	CHECK(r["d"]["requestStatus"]["result"] == false);
	CHECK(r["d"]["requestStatus"]["code"] == 204);
	CHECK(r["d"]["requestStatus"]["comment"] == "Your request type is not valid.");
	CHECK(!r["d"].contains("responseData"));

	// Data with arrays of plain values comes through untouched.
	o = s.handle(R"({"op":6,"d":{"requestType":"Echo","requestId":"e","requestData":{"list":[1,"two",null,[3]]}}})");
	r = parse(s.run(o.op, o.payload, backend, stopping));
	CHECK(r["d"]["responseData"]["list"] == json::parse(R"([1,"two",null,[3]])"));

	CHECK(s.handle(R"({"op":6,"d":{"requestType":"GetVersion"}})").close == link::close_code::MissingDataField);
	CHECK(s.handle(R"({"op":6,"d":{"requestId":"1"}})").close == link::close_code::MissingDataField);
	CHECK(s.handle(R"({"op":6,"d":{"requestType":5,"requestId":"1"}})").close ==
	      link::close_code::InvalidDataFieldType);
	CHECK(s.handle(R"({"op":42,"d":{}})").close == link::close_code::UnknownOpCode);

	// OBS closing: no new requests run.
	stopping = true;
	int before = backend.calls;
	o = s.handle(R"({"op":6,"d":{"requestType":"GetVersion","requestId":"z"}})");
	r = parse(s.run(o.op, o.payload, backend, stopping));
	CHECK(r["d"]["requestStatus"]["code"] == 207);
	CHECK(backend.calls == before);
	stopping = false;

	// Batches, with variables and a stop on the first failure.
	o = s.handle(R"({"op":8,"d":{"requestId":"b","haltOnFailure":true,"requests":[
		{"requestType":"Echo","requestData":{"a":1},"outputVariables":{"got":"a"}},
		{"requestType":"Echo","inputVariables":{"b":"got"}},
		{"requestType":"Sleep","requestData":{"sleepMillis":5}},
		{"requestType":"Nope"},
		{"requestType":"Echo"}]}})");
	CHECK(o.work && o.op == 8);
	r = parse(s.run(o.op, o.payload, backend, stopping));
	CHECK(r["op"] == 9);
	CHECK(r["d"]["requestId"] == "b");
	CHECK(r["d"]["results"].size() == 4);
	CHECK(r["d"]["results"][1]["responseData"]["b"] == 1);
	CHECK(r["d"]["results"][2]["requestStatus"]["code"] == 100);
	CHECK(r["d"]["results"][3]["requestStatus"]["code"] == 204);
	CHECK(s.handle(R"({"op":8,"d":{"requestId":"b","requests":{}}})").close ==
	      link::close_code::InvalidDataFieldType);
	CHECK(s.handle(R"({"op":8,"d":{"requestId":"b","executionType":7,"requests":[]}})").close ==
	      link::close_code::InvalidDataFieldValue);

	// Events, straight from obs-websocket's JSON.
	CHECK(link::event_message("CurrentProgramSceneChanged", 4, R"({"sceneName":"BRB"})") ==
	      R"({"op":5,"d":{"eventType":"CurrentProgramSceneChanged","eventIntent":4,"eventData":{"sceneName":"BRB"}}})");
	CHECK(link::event_message("ExitStarted", 1, "null") ==
	      R"({"op":5,"d":{"eventType":"ExitStarted","eventIntent":1}})");
	CHECK(parse(link::event_message("Odd \"name\"", 1, "{}"))["d"]["eventType"] == "Odd \"name\"");
}

static void test_addresses()
{
	int port = -1;
	CHECK(obs::local_address("http://127.0.0.1:8123/overlay?key=x", port) && port == 8123);
	CHECK(obs::local_address("http://localhost/alerts", port) && port == 0);
	CHECK(obs::local_address("HTTP://LocalHost:9000", port) && port == 9000);
	CHECK(obs::local_address("http://[::1]:7000/x", port) && port == 7000);
	CHECK(obs::local_address("http://127.1.2.3:80", port) && port == 80);
	CHECK(obs::local_address("http://user:pass@127.0.0.1:5000/", port) && port == 5000);
	CHECK(!obs::local_address("https://streamelements.com/overlay/abc", port) && port == 0);
	CHECK(!obs::local_address("https://localhost.example.com:8123/", port) && port == 0);
	CHECK(!obs::local_address("https://127.example.com/", port));
	CHECK(!obs::local_address("http://example.com/?u=http://127.0.0.1:1", port));
	CHECK(!obs::local_address("file:///C:/overlay.html", port));
	CHECK(!obs::local_address("127.0.0.1:8000", port)); // no scheme: not a web address OBS loads
	CHECK(!obs::local_address("", port));
	CHECK(obs::local_address("http://127.0.0.1:99999/", port) && port == 0);
	CHECK(obs::local_address("http://127.0.0.1:/x", port) && port == 0);
}

int main()
{
	test_hashes();
	test_handshake();
	test_frames();
	test_protocol();
	test_addresses();
	std::printf("%d passed, %d failed\n", passes, failures);
	return failures ? 1 : 0;
}
