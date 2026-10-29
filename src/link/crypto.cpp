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

#include "crypto.hpp"

#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <cstdio>
#include <sys/random.h>
#endif

namespace direbot::crypto {

namespace {

inline uint32_t rotl(uint32_t x, int n)
{
	return (x << n) | (x >> (32 - n));
}

inline uint32_t rotr(uint32_t x, int n)
{
	return (x >> n) | (x << (32 - n));
}

// Message padded the way both hashes want it: a 1 bit, zeros, then the
// length in bits as a big-endian 64-bit number, to a multiple of 64 bytes.
std::string padded(const std::string &data)
{
	std::string out = data;
	const uint64_t bits = (uint64_t)data.size() * 8;
	out.push_back((char)0x80);
	while (out.size() % 64 != 56)
		out.push_back('\0');
	for (int i = 7; i >= 0; i--)
		out.push_back((char)((bits >> (i * 8)) & 0xff));
	return out;
}

inline uint32_t be32(const unsigned char *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

std::string words_out(const uint32_t *h, int count)
{
	std::string out;
	for (int i = 0; i < count; i++) {
		out.push_back((char)(h[i] >> 24));
		out.push_back((char)(h[i] >> 16));
		out.push_back((char)(h[i] >> 8));
		out.push_back((char)h[i]);
	}
	return out;
}

const uint32_t K256[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
const char B64URL[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string encode(const std::string &data, const char *alphabet, bool pad)
{
	std::string out;
	size_t i = 0;
	const auto *p = (const unsigned char *)data.data();
	for (; i + 2 < data.size(); i += 3) {
		uint32_t v = (p[i] << 16) | (p[i + 1] << 8) | p[i + 2];
		out.push_back(alphabet[(v >> 18) & 63]);
		out.push_back(alphabet[(v >> 12) & 63]);
		out.push_back(alphabet[(v >> 6) & 63]);
		out.push_back(alphabet[v & 63]);
	}
	const size_t rest = data.size() - i;
	if (rest == 1) {
		uint32_t v = p[i] << 16;
		out.push_back(alphabet[(v >> 18) & 63]);
		out.push_back(alphabet[(v >> 12) & 63]);
		if (pad)
			out += "==";
	} else if (rest == 2) {
		uint32_t v = (p[i] << 16) | (p[i + 1] << 8);
		out.push_back(alphabet[(v >> 18) & 63]);
		out.push_back(alphabet[(v >> 12) & 63]);
		out.push_back(alphabet[(v >> 6) & 63]);
		if (pad)
			out += "=";
	}
	return out;
}

} // namespace

std::string sha1(const std::string &data)
{
	uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
	const std::string m = padded(data);
	const auto *bytes = (const unsigned char *)m.data();
	for (size_t block = 0; block < m.size(); block += 64) {
		uint32_t w[80];
		for (int i = 0; i < 16; i++)
			w[i] = be32(bytes + block + i * 4);
		for (int i = 16; i < 80; i++)
			w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
		for (int i = 0; i < 80; i++) {
			uint32_t f, k;
			if (i < 20) {
				f = (b & c) | (~b & d);
				k = 0x5A827999;
			} else if (i < 40) {
				f = b ^ c ^ d;
				k = 0x6ED9EBA1;
			} else if (i < 60) {
				f = (b & c) | (b & d) | (c & d);
				k = 0x8F1BBCDC;
			} else {
				f = b ^ c ^ d;
				k = 0xCA62C1D6;
			}
			uint32_t t = rotl(a, 5) + f + e + k + w[i];
			e = d;
			d = c;
			c = rotl(b, 30);
			b = a;
			a = t;
		}
		h[0] += a;
		h[1] += b;
		h[2] += c;
		h[3] += d;
		h[4] += e;
	}
	return words_out(h, 5);
}

std::string sha256(const std::string &data)
{
	uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
			 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	const std::string m = padded(data);
	const auto *bytes = (const unsigned char *)m.data();
	for (size_t block = 0; block < m.size(); block += 64) {
		uint32_t w[64];
		for (int i = 0; i < 16; i++)
			w[i] = be32(bytes + block + i * 4);
		for (int i = 16; i < 64; i++) {
			uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
			uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
		for (int i = 0; i < 64; i++) {
			uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
			uint32_t ch = (e & f) ^ (~e & g);
			uint32_t t1 = hh + S1 + ch + K256[i] + w[i];
			uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
			uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
			uint32_t t2 = S0 + maj;
			hh = g;
			g = f;
			f = e;
			e = d + t1;
			d = c;
			c = b;
			b = a;
			a = t1 + t2;
		}
		h[0] += a;
		h[1] += b;
		h[2] += c;
		h[3] += d;
		h[4] += e;
		h[5] += f;
		h[6] += g;
		h[7] += hh;
	}
	return words_out(h, 8);
}

std::string base64(const std::string &data)
{
	return encode(data, B64, true);
}

std::string base64url(const std::string &data)
{
	return encode(data, B64URL, false);
}

std::string random_bytes(size_t count)
{
	std::string out(count, '\0');
	if (!count)
		return out;
#ifdef _WIN32
	if (BCryptGenRandom(nullptr, (PUCHAR)&out[0], (ULONG)count, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
		return std::string();
	return out;
#else
	size_t got = 0;
	while (got < count) {
		ssize_t n = getrandom(&out[got], count - got, 0);
		if (n <= 0)
			return std::string();
		got += (size_t)n;
	}
	return out;
#endif
}

bool same(const std::string &a, const std::string &b)
{
	if (a.size() != b.size())
		return false;
	unsigned char diff = 0;
	for (size_t i = 0; i < a.size(); i++)
		diff |= (unsigned char)(a[i] ^ b[i]);
	return diff == 0;
}

} // namespace direbot::crypto
