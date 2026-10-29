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

#include "keystore.hpp"

#include <cstring>

#include "../link/crypto.hpp"

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#endif

namespace direbot::obs::keystore {

namespace {

constexpr const char *DPAPI = "dpapi:";
constexpr const char *PLAIN = "plain:";

bool starts(const std::string &s, const char *prefix)
{
	return s.compare(0, std::strlen(prefix), prefix) == 0;
}

#ifdef _WIN32
// Standard base64 back to bytes; false on anything that isn't.
bool unbase64(const std::string &in, std::string &out)
{
	auto value = [](char c) -> int {
		if (c >= 'A' && c <= 'Z')
			return c - 'A';
		if (c >= 'a' && c <= 'z')
			return c - 'a' + 26;
		if (c >= '0' && c <= '9')
			return c - '0' + 52;
		if (c == '+')
			return 62;
		if (c == '/')
			return 63;
		return -1;
	};
	out.clear();
	unsigned buffer = 0;
	int bits = 0;
	size_t i = 0;
	for (; i < in.size() && in[i] != '='; i++) {
		int v = value(in[i]);
		if (v < 0)
			return false;
		buffer = (buffer << 6) | (unsigned)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out.push_back((char)((buffer >> bits) & 0xff));
		}
	}
	for (; i < in.size(); i++)
		if (in[i] != '=')
			return false;
	return true;
}

// Ties the lock to this add-on as well as the Windows account: another
// program of the same user can't unlock it without knowing this.
const char ENTROPY[] = "DireBot for OBS: stream keys";

bool protect(const std::string &secret, std::string &out)
{
	DATA_BLOB in{(DWORD)secret.size(), (BYTE *)secret.data()};
	DATA_BLOB extra{(DWORD)(sizeof(ENTROPY) - 1), (BYTE *)ENTROPY};
	DATA_BLOB locked{0, nullptr};
	if (!CryptProtectData(&in, L"DireBot for OBS", &extra, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &locked))
		return false;
	out.assign((const char *)locked.pbData, locked.cbData);
	LocalFree(locked.pbData);
	return true;
}

bool unprotect(const std::string &blob, std::string &out)
{
	DATA_BLOB in{(DWORD)blob.size(), (BYTE *)blob.data()};
	DATA_BLOB extra{(DWORD)(sizeof(ENTROPY) - 1), (BYTE *)ENTROPY};
	DATA_BLOB open{0, nullptr};
	if (!CryptUnprotectData(&in, nullptr, &extra, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &open))
		return false;
	out.assign((const char *)open.pbData, open.cbData);
	SecureZeroMemory(open.pbData, open.cbData);
	LocalFree(open.pbData);
	return true;
}
#endif

} // namespace

void wipe(std::string &secret)
{
	volatile char *p = secret.empty() ? nullptr : &secret[0];
	for (size_t i = 0; p && i < secret.size(); i++)
		p[i] = 0;
	secret.clear();
}

std::string lock(const std::string &secret)
{
	if (secret.empty())
		return std::string();
#ifdef _WIN32
	std::string blob;
	if (!protect(secret, blob))
		return std::string();
	return DPAPI + crypto::base64(blob);
#else
	return PLAIN + secret;
#endif
}

bool unlock(const std::string &stored, std::string &secret)
{
	wipe(secret);
	if (stored.empty())
		return true;
	if (starts(stored, PLAIN)) {
		// Only written away from Windows (the tests). One in a Windows
		// file was put there by hand: still the key.
		secret = stored.substr(std::strlen(PLAIN));
		return true;
	}
	if (starts(stored, DPAPI)) {
#ifdef _WIN32
		std::string blob;
		if (!unbase64(stored.substr(std::strlen(DPAPI)), blob))
			return false;
		return unprotect(blob, secret);
#else
		return false;
#endif
	}
	return false;
}

} // namespace direbot::obs::keystore
