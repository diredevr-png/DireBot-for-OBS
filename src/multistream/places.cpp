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

#include "places.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace direbot::multistream {

namespace {

constexpr int FORMAT = 1;
constexpr size_t MAX_PLACES = 12;
constexpr size_t MAX_NAME = 40;
constexpr size_t MAX_SERVER = 1024;
constexpr size_t MAX_KEY = 1024;
constexpr int MIN_KBPS = 300;
constexpr int MAX_KBPS = 50000;

std::string lower(std::string s)
{
	for (char &c : s)
		c = (char)std::tolower((unsigned char)c);
	return s;
}

std::string trim(const std::string &s)
{
	size_t a = 0, b = s.size();
	while (a < b && std::isspace((unsigned char)s[a]))
		a++;
	while (b > a && std::isspace((unsigned char)s[b - 1]))
		b--;
	return s.substr(a, b - a);
}

// A name on one line, without control characters, cut to `limit` bytes
// at a character boundary.
std::string tidy_text(const std::string &raw, size_t limit)
{
	std::string out;
	for (unsigned char c : raw)
		out.push_back(c < 32 || c == 127 ? ' ' : (char)c);
	out = trim(out);
	if (out.size() > limit) {
		size_t cut = limit;
		while (cut > 0 && ((unsigned char)out[cut] & 0xC0) == 0x80)
			cut--;
		out = trim(out.substr(0, cut));
	}
	return out;
}

std::string text(const json &j, const char *key)
{
	auto it = j.find(key);
	return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool flag(const json &j, const char *key, bool fallback)
{
	auto it = j.find(key);
	return it != j.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

int number(const json &j, const char *key, int fallback)
{
	auto it = j.find(key);
	if (it == j.end() || !it->is_number())
		return fallback;
	double v = it->get<double>();
	if (!(v > -1e9 && v < 1e9))
		return fallback;
	return (int)v;
}

std::vector<std::string> words_of(const std::string &s)
{
	std::vector<std::string> out;
	std::string cur;
	for (char c : lower(s)) {
		if (std::isalnum((unsigned char)c)) {
			cur.push_back(c);
		} else if (!cur.empty()) {
			out.push_back(cur);
			cur.clear();
		}
	}
	if (!cur.empty())
		out.push_back(cur);
	return out;
}

struct Guess {
	const char *site;
	std::vector<std::string> words;
	std::vector<std::string> hosts; // the end of the host name
};

const std::vector<Guess> &guesses()
{
	static const std::vector<Guess> g = {
		{"youtube", {"youtube", "yt"}, {"youtube.com"}},
		{"kick", {"kick"}, {"fa723fc1b171.global-contribute.live-video.net", "kick.com"}},
		{"tiktok", {"tiktok"}, {"tiktokcdn.com", "tiktokcdn-us.com", "tiktokv.com", "tiktok.com"}},
		{"facebook", {"facebook", "fb"}, {"facebook.com"}},
		{"instagram", {"instagram", "insta", "ig"}, {"instagram.com"}},
		{"x", {"twitter"}, {"pscp.tv", "x.com"}},
		{"trovo", {"trovo"}, {"trovo.live"}},
		{"twitch", {"twitch", "ttv"}, {"twitch.tv", "ingest.global-contribute.live-video.net"}},
	};
	return g;
}

bool host_ends(const std::string &host, const std::string &end)
{
	if (host == end)
		return true;
	return host.size() > end.size() && host.compare(host.size() - end.size(), end.size(), end) == 0 &&
	       host[host.size() - end.size() - 1] == '.';
}

} // namespace

const std::vector<Site> &sites()
{
	static const std::vector<Site> list = {
		{"youtube", "YouTube", "rtmps://a.rtmps.youtube.com:443/live2",
		 "YouTube Studio → Create → Go live → Stream: copy the Stream key.", false},
		{"kick", "Kick", "rtmps://fa723fc1b171.global-contribute.live-video.net:443/app/",
		 "Kick → Creator Dashboard → Settings → Stream URL & Key: copy the Stream key.", true},
		{"tiktok", "TikTok", "",
		 "TikTok LIVE Producer (or LIVE Center): copy its Server URL and Stream key.", true},
		{"facebook", "Facebook", "rtmps://rtmp-api.facebook.com:443/rtmp/",
		 "Facebook → Live Producer → Streaming software: copy the Stream key.", true},
		{"instagram", "Instagram", "",
		 "Instagram on the web → Create → Live video: copy its Stream URL and Stream key.", true},
		{"x", "X", "", "X Media Studio → Producer → Sources: copy its RTMP URL and key.", true},
		{"trovo", "Trovo", "rtmp://livepush.trovo.live/live/",
		 "Trovo → Creator Studio → Stream settings: copy the Stream key.", true},
		{"twitch", "Twitch", "rtmp://live.twitch.tv/app",
		 "Twitch → Creator Dashboard → Settings → Stream: copy the Primary Stream key.", true},
		{"other", "Another site", "", "That site's streaming settings: a server (or URL) and a key.", false},
	};
	return list;
}

const Site *find_site(const std::string &key)
{
	for (const Site &s : sites())
		if (key == s.key)
			return &s;
	return nullptr;
}

const std::vector<int> &heights()
{
	static const std::vector<int> list = {0, 1080, 936, 900, 720, 540, 480};
	return list;
}

std::string host_of(const std::string &server)
{
	std::string s = trim(server);
	size_t scheme = s.find("://");
	if (scheme != std::string::npos)
		s = s.substr(scheme + 3);
	// Up to the path, the query, or the end.
	size_t end = s.find_first_of("/?#");
	if (end != std::string::npos)
		s = s.substr(0, end);
	// Never anything that was before an @ (a name and password).
	size_t at = s.rfind('@');
	if (at != std::string::npos)
		s = s.substr(at + 1);
	if (!s.empty() && s[0] == '[') {
		size_t close = s.find(']');
		s = close == std::string::npos ? std::string() : s.substr(1, close - 1);
	} else {
		size_t colon = s.find(':');
		if (colon != std::string::npos)
			s = s.substr(0, colon);
	}
	s = lower(s);
	for (char c : s)
		if (!(std::isalnum((unsigned char)c) || c == '.' || c == '-' || c == ':'))
			return std::string();
	return s.substr(0, 120);
}

std::string guess_site(const std::string &name, const std::string &server)
{
	const std::vector<std::string> words = words_of(name);
	for (const Guess &g : guesses())
		for (const std::string &w : g.words)
			if (std::find(words.begin(), words.end(), w) != words.end())
				return g.site;
	const std::string host = host_of(server);
	if (!host.empty())
		for (const Guess &g : guesses())
			for (const std::string &h : g.hosts)
				if (host_ends(host, h))
					return g.site;
	return "other";
}

void tidy(Place &p)
{
	p.name = tidy_text(p.name, MAX_NAME);
	if (!find_site(p.site))
		p.site = "other";
	if (p.name.empty())
		p.name = find_site(p.site)->label;
	p.server = trim(p.server).substr(0, MAX_SERVER);
	// A key is whatever was pasted, less the spaces around it.
	p.key = trim(p.key).substr(0, MAX_KEY);
	if (p.video != SAME && p.video != LIGHTER && p.video != OWN)
		p.video = SAME;
	if (p.kbps != 0)
		p.kbps = std::clamp(p.kbps, MIN_KBPS, MAX_KBPS);
	const auto &h = heights();
	if (std::find(h.begin(), h.end(), p.height) == h.end())
		p.height = 0;
	p.encoder = tidy_text(p.encoder, 80);
	p.track = std::clamp(p.track, 0, 6);
	if (p.came_from != "multi_rtmp" && p.came_from != "aitum")
		p.came_from.clear();
}

void tidy(Lighter &l)
{
	l.kbps = std::clamp(l.kbps == 0 ? 3500 : l.kbps, MIN_KBPS, MAX_KBPS);
	const auto &h = heights();
	if (std::find(h.begin(), h.end(), l.height) == h.end())
		l.height = 720;
	l.encoder = tidy_text(l.encoder, 80);
}

std::string not_ready(const Place &p)
{
	if (p.server.empty())
		return p.name + " has no server yet.";
	if (p.key_lost())
		return p.name + "'s stream key was typed in on another PC or Windows account, so it has to be typed in "
				"again here.";
	if (p.key.empty())
		return p.name + " has no stream key yet.";
	return std::string();
}

std::string twin_of(const Places &all, const Place &p)
{
	if (!p.on || p.key.empty() || p.server.empty())
		return std::string();
	const std::string host = host_of(p.server);
	for (const Place &q : all.list) {
		if (q.id == p.id)
			break; // only the ones before it: the second one is the twin
		if (q.on && q.key == p.key && host_of(q.server) == host)
			return q.id;
	}
	return std::string();
}

bool Place::operator==(const Place &o) const
{
	return id == o.id && name == o.name && site == o.site && server == o.server && key == o.key &&
	       locked_key == o.locked_key && on == o.on && with_stream == o.with_stream &&
	       stop_with_stream == o.stop_with_stream && video == o.video && kbps == o.kbps && height == o.height &&
	       encoder == o.encoder && track == o.track && came_from == o.came_from;
}

Place *Places::find(const std::string &id)
{
	for (Place &p : list)
		if (p.id == id)
			return &p;
	return nullptr;
}

const Place *Places::find(const std::string &id) const
{
	for (const Place &p : list)
		if (p.id == id)
			return &p;
	return nullptr;
}

std::string Places::new_id(const std::function<std::string(size_t)> &random) const
{
	static const char *letters = "abcdefghijkmnpqrstuvwxyz23456789";
	for (int attempt = 0; attempt < 50; attempt++) {
		std::string bytes = random(8);
		std::string id;
		for (unsigned char c : bytes)
			id.push_back(letters[c % 32]);
		if (id.size() == 8 && !find(id))
			return id;
	}
	// Random numbers never fail like this; counting still gives one.
	for (int n = (int)list.size() + 1;; n++) {
		std::string id = "place" + std::to_string(n);
		if (!find(id))
			return id;
	}
}

json Places::to_file(const std::function<std::string(const std::string &)> &lock) const
{
	json places = json::array();
	for (const Place &p : list) {
		std::string locked = p.key.empty() ? p.locked_key : lock(p.key);
		places.push_back({{"id", p.id},
				  {"name", p.name},
				  {"site", p.site},
				  {"server", p.server},
				  {"key", locked},
				  {"on", p.on},
				  {"with_stream", p.with_stream},
				  {"stop_with_stream", p.stop_with_stream},
				  {"video", p.video},
				  {"kbps", p.kbps},
				  {"height", p.height},
				  {"encoder", p.encoder},
				  {"track", p.track},
				  {"came_from", p.came_from}});
	}
	return {{"format", FORMAT},
		{"places", places},
		{"lighter", {{"kbps", lighter.kbps}, {"height", lighter.height}, {"encoder", lighter.encoder}}}};
}

Places Places::from_file(const json &file, const std::function<bool(const std::string &, std::string &)> &unlock)
{
	Places out;
	if (!file.is_object())
		return out;
	auto lighter = file.find("lighter");
	if (lighter != file.end() && lighter->is_object()) {
		out.lighter.kbps = number(*lighter, "kbps", 3500);
		out.lighter.height = number(*lighter, "height", 720);
		out.lighter.encoder = text(*lighter, "encoder");
	}
	tidy(out.lighter);
	auto places = file.find("places");
	if (places == file.end() || !places->is_array())
		return out;
	std::set<std::string> ids;
	for (const json &raw : *places) {
		if (!raw.is_object() || out.list.size() >= MAX_PLACES)
			continue;
		Place p;
		p.id = tidy_text(text(raw, "id"), 24);
		if (p.id.empty() || ids.count(p.id))
			continue;
		ids.insert(p.id);
		p.name = text(raw, "name");
		p.site = text(raw, "site");
		p.server = text(raw, "server");
		const std::string locked = text(raw, "key");
		if (!locked.empty() && !unlock(locked, p.key)) {
			p.key.clear();
			p.locked_key = locked;
		}
		p.on = flag(raw, "on", true);
		p.with_stream = flag(raw, "with_stream", true);
		p.stop_with_stream = flag(raw, "stop_with_stream", true);
		p.video = text(raw, "video");
		p.kbps = number(raw, "kbps", 0);
		p.height = number(raw, "height", 0);
		p.encoder = text(raw, "encoder");
		p.track = number(raw, "track", 0);
		p.came_from = text(raw, "came_from");
		tidy(p);
		out.list.push_back(std::move(p));
	}
	return out;
}

std::vector<Place> from_multi_rtmp(const json &file)
{
	std::vector<Place> out;
	if (!file.is_object())
		return out;
	std::set<std::string> videos;
	std::vector<std::pair<std::string, int>> audios;
	auto vc = file.find("video_configs");
	if (vc != file.end() && vc->is_array())
		for (const json &v : *vc)
			if (v.is_object() && !text(v, "id").empty())
				videos.insert(text(v, "id"));
	auto ac = file.find("audio_configs");
	if (ac != file.end() && ac->is_array())
		for (const json &a : *ac)
			if (a.is_object() && !text(a, "id").empty())
				audios.push_back({text(a, "id"), number(a, "mixerId", 0)});
	auto targets = file.find("targets");
	if (targets == file.end() || !targets->is_array())
		return out;
	for (const json &t : *targets) {
		if (!t.is_object())
			continue;
		Place p;
		p.name = text(t, "name");
		auto service = t.find("service-param");
		if (service != t.end() && service->is_object()) {
			p.server = text(*service, "server");
			p.key = text(*service, "key");
		}
		p.site = guess_site(p.name, p.server);
		p.with_stream = flag(t, "sync-start", false);
		p.stop_with_stream = flag(t, "sync-stop", p.with_stream);
		const std::string video = text(t, "video-config");
		p.video = videos.count(video) ? LIGHTER : SAME;
		const std::string audio = text(t, "audio-config");
		for (const auto &a : audios)
			if (a.first == audio && a.second >= 1 && a.second <= 5)
				p.track = a.second + 1;
		p.came_from = "multi_rtmp";
		// In both add-ons at once, every site would get it twice: it
		// comes over switched off, to switch on once the old one is.
		p.on = false;
		tidy(p);
		out.push_back(std::move(p));
	}
	return out;
}

std::vector<Place> from_aitum(const json &file, const std::string &profile)
{
	std::vector<Place> out;
	if (!file.is_object())
		return out;
	auto profiles = file.find("profiles");
	if (profiles == file.end() || !profiles->is_array())
		return out;
	const json *pick = nullptr;
	size_t count = 0;
	for (const json &p : *profiles) {
		if (!p.is_object())
			continue;
		count++;
		if (text(p, "name") == profile)
			pick = &p;
	}
	if (!pick && count == 1)
		for (const json &p : *profiles)
			if (p.is_object())
				pick = &p;
	if (!pick)
		return out;
	auto outputs = pick->find("outputs");
	if (outputs == pick->end() || !outputs->is_array())
		return out;
	for (const json &o : *outputs) {
		if (!o.is_object() || text(o, "name").empty())
			continue;
		Place p;
		p.name = text(o, "name");
		p.server = o.contains("stream_server") ? text(o, "stream_server") : text(o, "server");
		p.key = o.contains("stream_key") ? text(o, "stream_key") : text(o, "key");
		p.site = guess_site(p.name, p.server);
		// Aitum starts each one from its own button.
		p.with_stream = false;
		p.stop_with_stream = true;
		p.video = flag(o, "advanced", false) ? LIGHTER : SAME;
		p.came_from = "aitum";
		p.on = false;
		tidy(p);
		out.push_back(std::move(p));
	}
	return out;
}

std::string stop_words(int code)
{
	// OBS_OUTPUT_* in obs-defs.h.
	switch (code) {
	case 0:
		return std::string();
	case -1:
		return "The server address isn't right.";
	case -2:
		return "Couldn't reach the server. Check its address, or the site may be down.";
	case -3:
		return "The site turned the stream away. The stream key is probably wrong or out of date.";
	case -5:
		return "The connection dropped and didn't come back.";
	case -6:
		return "The site doesn't take this kind of video. Try Lighter (H.264) for it.";
	case -8:
		return "The encoder stopped working.";
	default:
		return "It stopped with an error.";
	}
}

} // namespace direbot::multistream
