// A pretend OBS for the link's network tests: the real server, with a
// backend that answers a few requests the way obs-websocket does.
//
//   harness <password> <preferred port>
//
// Prints "PORT <n>" once listening. Then reads commands, one per line:
//   event <intent> <type> <json>   send an event to whoever wants it
//   meters <count>                 send that many audio meter updates
//   wanted                         print "WANTED <bits>"
//   clients                        print "CLIENTS <n>"
//   quit
// Run by tests/test_link.py.

#include "../src/link/protocol.hpp"
#include "../src/link/server.hpp"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <thread>

using namespace direbot::link;

struct PretendObs : Backend {
	RequestResult call(const std::string &type, const json &data) override
	{
		if (type == "GetVersion")
			return {status::Success, "",
				{{"obsVersion", "32.2.2"}, {"obsWebSocketVersion", "5.6.3"}, {"rpcVersion", 1},
				 {"platform", "windows"}}};
		if (type == "GetCurrentProgramScene")
			return {status::Success, "", {{"currentProgramSceneName", "Live"}, {"sceneName", "Live"}}};
		if (type == "GetSceneList")
			return {status::Success, "",
				{{"currentProgramSceneName", "Live"},
				 {"scenes", json::array({{{"sceneName", "BRB"}, {"sceneIndex", 1}},
							 {{"sceneName", "Live"}, {"sceneIndex", 0}}})}}};
		if (type == "EchoTest")
			return {status::Success, "", data.is_object() ? data : json::object()};
		if (type == "SlowTest") {
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
			return {status::Success, "", {{"slow", true}}};
		}
		return {status::UnknownRequestType, "Your request type is not valid.", nullptr};
	}
};

int main(int argc, char **argv)
{
	if (argc < 3) {
		std::fprintf(stderr, "usage: harness <password> <port>\n");
		return 2;
	}
	PretendObs obs;
	Server server(obs, [](Server::LogLevel, const std::string &line) {
		std::fprintf(stderr, "LOG %s\n", line.c_str());
	});
	server.on_wanted_changed = [](uint64_t bits) {
		std::fprintf(stderr, "LOG wanted now %llu\n", (unsigned long long)bits);
	};
	Server::Options options;
	options.password = argv[1];
	options.websocket_version = "5.6.3";
	options.preferred_port = (uint16_t)std::atoi(argv[2]);
	uint16_t port = server.start(options);
	std::printf("PORT %u\n", port);
	std::fflush(stdout);
	if (!port)
		return 1;

	std::string line;
	while (std::getline(std::cin, line)) {
		std::istringstream in(line);
		std::string cmd;
		in >> cmd;
		if (cmd == "quit")
			break;
		if (cmd == "event") {
			unsigned long long intent;
			std::string type, data;
			in >> intent >> type;
			std::getline(in, data);
			server.broadcast(intent, event_message(type, intent, data.empty() ? "null" : data.substr(1)));
		} else if (cmd == "meters") {
			int count = 0;
			in >> count;
			for (int i = 0; i < count; i++) {
				json d = {{"inputs", json::array({{{"inputName", "Mic"},
								   {"inputUuid", "u1"},
								   {"inputLevelsMul", {{0.5, 0.7, 0.8}}}}})}};
				server.broadcast(subs::InputVolumeMeters,
						 event_message("InputVolumeMeters", subs::InputVolumeMeters, d.dump()), true);
			}
		} else if (cmd == "wanted") {
			std::printf("WANTED %llu\n", (unsigned long long)server.wanted());
		} else if (cmd == "clients") {
			std::printf("CLIENTS %zu\n", server.clients());
		}
		std::fflush(stdout);
	}
	server.begin_stop();
	server.stop();
	std::printf("STOPPED\n");
	return 0;
}
