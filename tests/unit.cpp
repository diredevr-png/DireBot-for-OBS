// Unit tests for the parts of DireBot for OBS that don't need OBS: hashing,
// the WebSocket handshake and frames, the obs-websocket protocol, the
// usage report's "is this address on this PC" check, and multistream's
// places (its settings file, and bringing places over from other
// add-ons).
// Build and run: tools/test-link.sh (Linux, any C++17 compiler).

#include "../src/link/crypto.hpp"
#include "../src/link/protocol.hpp"
#include "../src/link/ws.hpp"
#include "../src/multistream/places.hpp"
#include "../src/obs/address.hpp"
#include "../src/obs/keystore.hpp"

#include <chrono>
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
	// Many small frames at once (4 MB of pings): read from an offset, so
	// each one doesn't shift the rest down - that took seconds, holding up
	// every client.
	{
		std::string in;
		while (in.size() < 4 * 1024 * 1024)
			in += client_frame(ws::Ping, std::string(125, 'p'));
		ws::Reader many;
		ws::Reader::Item item;
		size_t at = 0, count = 0;
		const auto started = std::chrono::steady_clock::now();
		while (many.next(in, at, item) && item.kind == ws::Reader::Item::Kind::Ping)
			count++;
		const auto took = std::chrono::steady_clock::now() - started;
		CHECK(at == in.size() && count == in.size() / 131);
		CHECK(took < std::chrono::milliseconds(500));
	}
	// Its limit can change (the server keeps it small until a client logs in).
	{
		ws::Reader small(64);
		std::string in = client_frame(ws::Text, std::string(65, 'x'));
		ws::Reader::Item item;
		CHECK(small.next(in, item) && item.kind == ws::Reader::Item::Kind::Error && item.code == 1009);
		small.limit(1024);
		in = client_frame(ws::Text, std::string(65, 'x'));
		CHECK(small.next(in, item) && item.kind == ws::Reader::Item::Kind::Text && item.data.size() == 65);
	}
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
	link::json last_set;
	std::string server = "auto";
	link::RequestResult call(const std::string &type, const link::json &data) override
	{
		calls++;
		if (type == "GetVersion")
			return {link::status::Success, "", {{"obsVersion", "32.2.2"}}};
		if (type == "Echo")
			return {link::status::Success, "", data};
		if (type == "SetThing")
			return {link::status::Success, "", nullptr};
		if (type == "GetStreamServiceSettings")
			return {link::status::Success,
				"",
				{{"streamServiceType", "rtmp_common"},
				 {"streamServiceSettings",
				  {{"service", "Twitch"}, {"server", server}, {"key", "live_123_secret"}, {"password", ""}}}}};
		if (type == "SetStreamServiceSettings") {
			last_set = data;
			return {link::status::Success, "", nullptr};
		}
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

	// An OBS name that isn't valid UTF-8 (a script can make one): answered,
	// with the bad bytes as U+FFFD, never an exception.
	CHECK(link::to_text(json{{"n", "Mic \xff\xfe"}}) == "{\"n\":\"Mic \xef\xbf\xbd\xef\xbf\xbd\"}");
	struct BadName : link::Backend {
		link::RequestResult call(const std::string &, const json &) override
		{
			return {link::status::Success, "", {{"inputName", "Mic \xff"}}};
		}
	} bad_name;
	std::atomic<bool> going{false};
	std::string reply;
	bool threw = false;
	try {
		reply = s.run(6, json{{"requestType", "GetInputList"}, {"requestId", "u"}}, bad_name, going);
		reply += s.run(8, json{{"requestId", "v"}, {"requests", {{{"requestType", "GetInputList"}}}}}, bad_name,
			       going);
	} catch (const std::exception &) {
		threw = true;
	}
	CHECK(!threw);
	CHECK(reply.find("Mic \xef\xbf\xbd") != std::string::npos);

	// The stream key never comes back through the link, alone or in a
	// batch (where a variable could pass it on); an empty one stays empty,
	// so "is a key set?" still has an answer.
	FakeBackend keys;
	json got = parse(s.run(6, json{{"requestType", "GetStreamServiceSettings"}, {"requestId", "k"}}, keys, going));
	json settings = got["d"]["responseData"]["streamServiceSettings"];
	CHECK(settings["key"] == link::HIDDEN_SECRET && settings["password"] == "" && settings["service"] == "Twitch");
	reply = s.run(8,
		      json{{"requestId", "kb"},
			   {"requests",
			    {{{"requestType", "GetStreamServiceSettings"},
			      {"outputVariables", {{"k", "streamServiceSettings"}}}},
			     {{"requestType", "Echo"}, {"inputVariables", {{"x", "k"}}}}}}},
		      keys, going);
	CHECK(reply.find("live_123_secret") == std::string::npos && reply.find("Twitch") != std::string::npos);
	// Only the server changed, or sent back as read (the stars): refused -
	// OBS would keep the real key and send it to that server.
	keys.last_set = nullptr;
	settings["server"] = "rtmp://somewhere.else/app";
	json no = parse(s.run(6,
			      json{{"requestType", "SetStreamServiceSettings"},
				   {"requestId", "s"},
				   {"requestData",
				    {{"streamServiceType", "rtmp_common"}, {"streamServiceSettings", settings}}}},
			      keys, going));
	CHECK(no["d"]["requestStatus"]["result"] == false && keys.last_set.is_null());
	settings.erase("key");
	no = parse(s.run(6,
			 json{{"requestType", "SetStreamServiceSettings"},
			      {"requestId", "s2"},
			      {"requestData", {{"streamServiceType", "rtmp_custom"}, {"streamServiceSettings", settings}}}},
			 keys, going));
	CHECK(no["d"]["requestStatus"]["result"] == false && keys.last_set.is_null());
	// A key inside a custom server's address is hidden too.
	FakeBackend in_url;
	in_url.server = "rtmp://evil.example/app/live_abc_secret?x=1";
	got = parse(s.run(6, json{{"requestType", "GetStreamServiceSettings"}, {"requestId", "u"}}, in_url, going));
	CHECK(got["d"]["responseData"]["streamServiceSettings"]["server"] ==
	      std::string("rtmp://evil.example/app/") + link::HIDDEN_SECRET);
	in_url.server = "rtmps://a.rtmp.youtube.com/live2";
	got = parse(s.run(6, json{{"requestType", "GetStreamServiceSettings"}, {"requestId", "u2"}}, in_url, going));
	CHECK(got["d"]["responseData"]["streamServiceSettings"]["server"] == "rtmps://a.rtmp.youtube.com/live2");
	// A new key goes in as it is, and no old password or token goes along.
	s.run(6,
	      json{{"requestType", "SetStreamServiceSettings"},
		   {"requestId", "n"},
		   {"requestData",
		    {{"streamServiceType", "rtmp_common"}, {"streamServiceSettings", {{"key", "live_new"}}}}}},
	      keys, going);
	CHECK(keys.last_set["streamServiceSettings"]["password"] == "" &&
	      keys.last_set["streamServiceSettings"]["bearer_token"] == "");
	CHECK(keys.last_set["streamServiceSettings"]["key"] == "live_new");
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

static void test_places_words()
{
	using namespace multistream;
	// Only ever the host: never a path (where a key can be) or a login.
	CHECK(host_of("rtmps://a.rtmps.youtube.com:443/live2") == "a.rtmps.youtube.com");
	CHECK(host_of("rtmp://me:secret@Live.Example.com:1935/app/KEY123") == "live.example.com");
	CHECK(host_of("  rtmp://[::1]:1935/live ") == "::1");
	CHECK(host_of("live.example.com/app?key=1") == "live.example.com");
	CHECK(host_of("rtmp://bad host/x").empty());
	CHECK(host_of("").empty());
	CHECK(guess_site("YT main", "") == "youtube");
	CHECK(guess_site("Backup", "rtmps://fa723fc1b171.global-contribute.live-video.net:443/app/") == "kick");
	CHECK(guess_site("", "rtmp://live.twitch.tv/app") == "twitch");
	CHECK(guess_site("Kick!", "rtmp://example.com/") == "kick");
	CHECK(guess_site("Kickstarter", "rtmp://notyoutube.com/") == "other");
	CHECK(guess_site("Restream", "rtmp://live.restream.io/live") == "other");
	CHECK(find_site("youtube") && find_site("other") && !find_site("myspace"));
	CHECK(find_site("kick")->needs_h264 && !find_site("youtube")->needs_h264);
	CHECK(stop_words(0).empty());
	CHECK(stop_words(-3).find("stream key") != std::string::npos);
	CHECK(!stop_words(-99).empty());
}

static void test_places_tidy()
{
	using namespace multistream;
	Place p;
	p.name = "  My\tYouTube\n ";
	p.site = "myspace";
	p.server = "  rtmp://example.com/live  ";
	p.key = " abc ";
	p.video = "huge";
	p.kbps = 999999;
	p.height = 123;
	p.track = 9;
	p.came_from = "somewhere";
	tidy(p);
	CHECK(p.name == "My YouTube");
	CHECK(p.site == "other");
	CHECK(p.server == "rtmp://example.com/live");
	CHECK(p.key == "abc");
	CHECK(p.video == SAME);
	CHECK(p.kbps == 50000);
	CHECK(p.height == 0);
	CHECK(p.track == 6);
	CHECK(p.came_from.empty());
	// Long names are cut between characters, never inside one.
	Place q;
	q.name = std::string(39, 'a') + "\xc3\xa9\xc3\xa9";
	tidy(q);
	CHECK(q.name == std::string(39, 'a'));
	q.name = "";
	q.site = "kick";
	tidy(q);
	CHECK(q.name == "Kick");
	Lighter l;
	l.kbps = 0;
	l.height = 7;
	tidy(l);
	CHECK(l.kbps == 3500 && l.height == 720);

	Place r;
	r.name = "YouTube";
	CHECK(not_ready(r).find("no server") != std::string::npos);
	r.server = "rtmp://example.com/live";
	CHECK(not_ready(r).find("no stream key") != std::string::npos);
	r.locked_key = "dpapi:xyz";
	CHECK(r.key_lost() && not_ready(r).find("typed in again") != std::string::npos);
	r.key = "abc";
	CHECK(!r.key_lost() && r.key_set() && not_ready(r).empty());

	// Two places with one server and one key: the second is the first's twin.
	Places all;
	Place a, b, c;
	a.id = "a";
	a.server = "rtmp://a.rtmp.youtube.com/live2";
	a.key = "k1";
	b = a;
	b.id = "b";
	b.server = "rtmp://A.rtmp.youtube.com/live2/";
	c = a;
	c.id = "c";
	c.key = "k2";
	all.list = {a, b, c};
	CHECK(twin_of(all, a).empty());
	CHECK(twin_of(all, b) == "a");
	CHECK(twin_of(all, c).empty());
	all.list[0].on = false;
	CHECK(twin_of(all, all.list[1]).empty());
}

static void test_places_file()
{
	using namespace multistream;
	auto lock = [](const std::string &key) { return "L:" + key; };
	auto unlock = [](const std::string &locked, std::string &key) {
		if (locked.compare(0, 2, "L:") != 0)
			return false;
		key = locked.substr(2);
		return true;
	};
	Places a;
	Place p;
	p.id = "abcd2345";
	p.name = "YouTube";
	p.site = "youtube";
	p.server = "rtmps://a.rtmps.youtube.com:443/live2";
	p.key = "SECRET-1";
	p.video = LIGHTER;
	p.track = 2;
	a.list.push_back(p);
	Place q;
	q.id = "efgh6789";
	q.name = "Kick";
	q.site = "kick";
	q.server = "rtmps://x/app/";
	q.locked_key = "dpapi:from-another-pc"; // can't be unlocked here
	q.on = false;
	q.video = OWN;
	q.kbps = 6000;
	q.height = 1080;
	a.list.push_back(q);
	a.lighter.kbps = 2500;
	a.lighter.height = 540;
	json file = a.to_file(lock);
	const std::string text = file.dump();
	CHECK(text.find("SECRET-1") == text.find("L:SECRET-1") + 2); // only ever locked
	CHECK(text.find("dpapi:from-another-pc") != std::string::npos); // kept as it was
	CHECK(file["format"] == 1);

	Places b = Places::from_file(json::parse(text), unlock);
	CHECK(b.list.size() == 2);
	CHECK(b.list[0].key == "SECRET-1" && b.list[0].video == LIGHTER && b.list[0].track == 2);
	CHECK(b.list[1].key.empty() && b.list[1].key_lost() && !b.list[1].on);
	CHECK(b.list[1].kbps == 6000 && b.list[1].height == 1080 && b.list[1].video == OWN);
	CHECK(b.lighter.kbps == 2500 && b.lighter.height == 540);
	CHECK(b.find("efgh6789") == &b.list[1] && !b.find("nope"));
	// Saved again, a key that can't be unlocked here is never lost.
	CHECK(b.to_file(lock).dump().find("dpapi:from-another-pc") != std::string::npos);

	// Anything odd is left out or set right, never thrown.
	json odd = {{"places",
		     {1, "x", {{"name", "No id"}}, {{"id", "aaaa"}, {"name", 5}, {"kbps", "lots"}, {"on", "yes"}},
		      {{"id", "aaaa"}, {"name", "Twin"}}}},
		    {"lighter", "big"}};
	Places c = Places::from_file(odd, unlock);
	CHECK(c.list.size() == 1 && c.list[0].id == "aaaa" && c.list[0].name == "Another site");
	CHECK(c.list[0].kbps == 0 && c.list[0].on);
	CHECK(c.lighter.kbps == 3500 && c.lighter.height == 720);
	CHECK(Places::from_file(json(nullptr), unlock).list.empty());
	CHECK(Places::from_file(json::array(), unlock).list.empty());
	json many = {{"places", json::array()}};
	for (int i = 0; i < 20; i++)
		many["places"].push_back({{"id", "p" + std::to_string(i)}});
	CHECK(Places::from_file(many, unlock).list.size() == 12);

	// New ids: 8 letters and digits nobody has.
	int calls = 0;
	std::string id = b.new_id([&](size_t n) {
		calls++;
		return std::string(n, calls == 1 ? '\0' : '\1');
	});
	CHECK(id == "aaaaaaaa" && calls == 1);
	b.list[0].id = "aaaaaaaa";
	calls = 0;
	id = b.new_id([&](size_t n) {
		calls++;
		return std::string(n, calls == 1 ? '\0' : '\1');
	});
	CHECK(id == "bbbbbbbb" && calls == 2);
	// No random numbers at all: still a free id.
	CHECK(b.new_id([](size_t) { return std::string(); }) == "place3");
}

static void test_places_from_other_addons()
{
	using namespace multistream;
	// Multiple RTMP outputs (obs-multi-rtmp.json in the OBS profile).
	json multi = json::parse(R"({
		"targets": [
			{"id": "t1", "name": "YouTube", "sync-start": true, "sync-stop": true,
			 "service-param": {"server": "rtmp://a.rtmp.youtube.com/live2", "key": "YT-KEY"},
			 "video-config": "", "audio-config": "a1"},
			{"id": "t2", "name": "Backup", "sync-start": false,
			 "service-param": {"server": "rtmps://fa723fc1b171.global-contribute.live-video.net:443/app/", "key": "KICK-KEY"},
			 "video-config": "v1"},
			"junk", {"name": "Empty"}
		],
		"video_configs": [{"id": "v1", "encoder": "obs_x264", "height": 720}],
		"audio_configs": [{"id": "a1", "encoder": "ffmpeg_aac", "mixerId": 1}]
	})");
	std::vector<Place> got = from_multi_rtmp(multi);
	CHECK(got.size() == 3);
	CHECK(got[0].name == "YouTube" && got[0].site == "youtube" && got[0].key == "YT-KEY");
	CHECK(got[0].with_stream && got[0].stop_with_stream && got[0].video == SAME && got[0].track == 2);
	CHECK(!got[0].on && got[0].came_from == "multi_rtmp"); // never twice at once
	CHECK(got[1].site == "kick" && got[1].video == LIGHTER && !got[1].with_stream && got[1].track == 0);
	CHECK(got[2].name == "Empty" && got[2].key.empty());
	CHECK(from_multi_rtmp(json::parse("{}")).empty());
	CHECK(from_multi_rtmp(json(nullptr)).empty());

	// Aitum Multistream (its config.json, every profile).
	json aitum = json::parse(R"({
		"profiles": [
			{"name": "Gaming", "outputs": [
				{"name": "TikTok", "stream_server": "rtmp://push.tiktokcdn.com/live/", "stream_key": "TT-KEY", "advanced": true},
				{"name": "", "stream_server": "x"}
			]},
			{"name": "IRL", "outputs": [{"name": "YouTube", "stream_server": "rtmp://y/", "stream_key": "K"}]}
		]
	})");
	got = from_aitum(aitum, "Gaming");
	CHECK(got.size() == 1 && got[0].site == "tiktok" && got[0].key == "TT-KEY" && got[0].video == LIGHTER);
	CHECK(!got[0].on && !got[0].with_stream && got[0].came_from == "aitum");
	CHECK(from_aitum(aitum, "Untitled").empty()); // which profile isn't known: none
	json one = json::parse(R"({"profiles": [{"name": "Old", "outputs": [{"name": "Kick", "stream_server": "s", "stream_key": "k"}]}]})");
	CHECK(from_aitum(one, "Untitled").size() == 1); // only one: that one
}

static void test_keystore()
{
	std::string key;
	CHECK(obs::keystore::lock("").empty());
	CHECK(obs::keystore::unlock("", key) && key.empty());
#ifndef _WIN32
	const std::string locked = obs::keystore::lock("live_123");
	CHECK(locked == "plain:live_123");
	CHECK(obs::keystore::unlock(locked, key) && key == "live_123");
	// Locked on Windows: can't be opened here.
	CHECK(!obs::keystore::unlock("dpapi:AAAA", key) && key.empty());
#endif
	CHECK(!obs::keystore::unlock("something else", key));
	std::string secret = "wipe me";
	obs::keystore::wipe(secret);
	CHECK(secret.empty());
}

int main()
{
	test_hashes();
	test_handshake();
	test_frames();
	test_protocol();
	test_addresses();
	test_places_words();
	test_places_tidy();
	test_places_file();
	test_places_from_other_addons();
	test_keystore();
	std::printf("%d passed, %d failed\n", passes, failures);
	return failures ? 1 : 0;
}
