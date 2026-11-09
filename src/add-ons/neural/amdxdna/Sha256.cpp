/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "Sha256.h"

#include <string.h>


static const uint32 kRoundConstants[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
	0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
	0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
	0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
	0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
	0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};


static inline uint32
rotate_right(uint32 value, int bits)
{
	return (value >> bits) | (value << (32 - bits));
}


Sha256::Sha256()
	:
	fLength(0),
	fFill(0)
{
	static const uint32 kInitial[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f,
		0x9b05688c, 0x1f83d9ab, 0x5be0cd19
	};
	memcpy(fState, kInitial, sizeof(fState));
}


void
Sha256::_Block(const uint8* block)
{
	uint32 w[64];
	for (int i = 0; i < 16; i++) {
		w[i] = (uint32)block[i * 4] << 24 | (uint32)block[i * 4 + 1] << 16
			| (uint32)block[i * 4 + 2] << 8 | block[i * 4 + 3];
	}
	for (int i = 16; i < 64; i++) {
		uint32 s0 = rotate_right(w[i - 15], 7) ^ rotate_right(w[i - 15], 18)
			^ (w[i - 15] >> 3);
		uint32 s1 = rotate_right(w[i - 2], 17) ^ rotate_right(w[i - 2], 19)
			^ (w[i - 2] >> 10);
		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	uint32 a = fState[0], b = fState[1], c = fState[2], d = fState[3];
	uint32 e = fState[4], f = fState[5], g = fState[6], h = fState[7];
	for (int i = 0; i < 64; i++) {
		uint32 s1 = rotate_right(e, 6) ^ rotate_right(e, 11)
			^ rotate_right(e, 25);
		uint32 choice = (e & f) ^ (~e & g);
		uint32 t1 = h + s1 + choice + kRoundConstants[i] + w[i];
		uint32 s0 = rotate_right(a, 2) ^ rotate_right(a, 13)
			^ rotate_right(a, 22);
		uint32 majority = (a & b) ^ (a & c) ^ (b & c);
		uint32 t2 = s0 + majority;
		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	fState[0] += a;
	fState[1] += b;
	fState[2] += c;
	fState[3] += d;
	fState[4] += e;
	fState[5] += f;
	fState[6] += g;
	fState[7] += h;
}


void
Sha256::Update(const void* _data, size_t size)
{
	const uint8* data = (const uint8*)_data;
	fLength += size;

	if (fFill > 0) {
		size_t part = 64 - fFill;
		if (part > size)
			part = size;
		memcpy(fBuffer + fFill, data, part);
		fFill += part;
		data += part;
		size -= part;
		if (fFill < 64)
			return;
		_Block(fBuffer);
		fFill = 0;
	}
	for (; size >= 64; data += 64, size -= 64)
		_Block(data);
	if (size > 0) {
		memcpy(fBuffer, data, size);
		fFill = size;
	}
}


void
Sha256::Finish(uint8 digest[32])
{
	uint64 bits = fLength * 8;
	uint8 padding[72];
	size_t count = fFill < 56 ? 56 - fFill : 120 - fFill;
	memset(padding, 0, sizeof(padding));
	padding[0] = 0x80;
	for (int i = 0; i < 8; i++)
		padding[count + i] = (uint8)(bits >> (56 - i * 8));
	Update(padding, count + 8);

	for (int i = 0; i < 8; i++) {
		digest[i * 4] = (uint8)(fState[i] >> 24);
		digest[i * 4 + 1] = (uint8)(fState[i] >> 16);
		digest[i * 4 + 2] = (uint8)(fState[i] >> 8);
		digest[i * 4 + 3] = (uint8)fState[i];
	}
}


void
Sha256::AddUInt8(uint8 value)
{
	Update(&value, 1);
}


void
Sha256::AddUInt32(uint32 value)
{
	uint8 bytes[4] = { (uint8)value, (uint8)(value >> 8),
		(uint8)(value >> 16), (uint8)(value >> 24) };
	Update(bytes, 4);
}


void
Sha256::AddInt32(int32 value)
{
	AddUInt32((uint32)value);
}


void
Sha256::AddUInt64(uint64 value)
{
	AddUInt32((uint32)value);
	AddUInt32((uint32)(value >> 32));
}


void
Sha256::AddFloat(float value)
{
	uint32 bits;
	memcpy(&bits, &value, sizeof(bits));
	AddUInt32(bits);
}


void
Sha256::AddString(const char* string)
{
	size_t length = strlen(string);
	AddUInt32((uint32)length);
	Update(string, length);
}


void
Sha256::ToHex(const uint8* digest, size_t count, char* text)
{
	static const char kDigits[] = "0123456789abcdef";
	for (size_t i = 0; i < count; i++) {
		text[i * 2] = kDigits[digest[i] >> 4];
		text[i * 2 + 1] = kDigits[digest[i] & 15];
	}
	text[count * 2] = '\0';
}
