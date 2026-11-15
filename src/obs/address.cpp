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

#include "address.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace direbot::obs {

bool local_address(const std::string &url, int &port)
{
	port = 0;
	const size_t scheme = url.find("://");
	if (scheme == std::string::npos)
		return false;
	std::string host = url.substr(scheme + 3);
	host = host.substr(0, host.find_first_of("/?#"));
	const size_t at = host.rfind('@');
	if (at != std::string::npos)
		host = host.substr(at + 1);
	std::string port_text;
	if (!host.empty() && host[0] == '[') {
		const size_t close = host.find(']');
		if (close == std::string::npos)
			return false;
		if (close + 1 < host.size() && host[close + 1] == ':')
			port_text = host.substr(close + 2);
		host = host.substr(1, close - 1);
	} else {
		const size_t colon = host.rfind(':');
		if (colon != std::string::npos) {
			port_text = host.substr(colon + 1);
			host = host.substr(0, colon);
		}
	}
	auto digits = [](const std::string &t, const char *also) {
		return !t.empty() && std::all_of(t.begin(), t.end(), [also](unsigned char c) {
			return std::isdigit(c) || (*also && c == (unsigned char)*also);
		});
	};
	if (digits(port_text, "") && port_text.size() <= 5) {
		const int p = std::stoi(port_text);
		port = p <= 65535 ? p : 0;
	}
	std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	// 127.x.x.x as numbers only: "127.example.com" is someone else's.
	const bool loopback_ip = host.rfind("127.", 0) == 0 && digits(host, ".");
	const bool here = host == "localhost" || host == "::1" || loopback_ip;
	if (!here)
		port = 0;
	return here;
}

std::string address_path(const std::string &url)
{
	const size_t scheme = url.find("://");
	if (scheme == std::string::npos)
		return "";
	const size_t start = url.find_first_of("/?#", scheme + 3);
	if (start == std::string::npos || url[start] != '/')
		return "/";
	const size_t end = url.find_first_of("?#", start);
	return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

bool page_spot(double x, double y, uint32_t width, uint32_t height, int &px, int &py)
{
	px = py = 0;
	if (!width || !height || !std::isfinite(x) || !std::isfinite(y) || x < 0 || x > 1 || y < 0 || y > 1)
		return false;
	// The last pixel at most: 1.0 is the page's far edge, still on it.
	px = std::min((int)width - 1, (int)std::floor(x * width));
	py = std::min((int)height - 1, (int)std::floor(y * height));
	return true;
}

bool player_big_enough(double drawn_w, double drawn_h, double w, double h, double min_w, double min_h)
{
	if (!(min_w > 0) && !(min_h > 0))
		return true;
	if (!std::isfinite(drawn_w) || !std::isfinite(drawn_h) || !std::isfinite(w) || !std::isfinite(h))
		return false;
	// Half a pixel's grace: a scale of 0.99999 from OBS's maths is 1.
	return drawn_w * w + 0.5 >= min_w && drawn_h * h + 0.5 >= min_h;
}

} // namespace direbot::obs
