/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef NET_SECRET_H
#define NET_SECRET_H


/*!	A secret key made when a network module starts, and SipHash-2-4 keyed with
	it. The protocols use it for values a host that cannot see a connection's
	packets must not be able to guess: TCP initial sequence numbers (RFC 6528),
	ephemeral ports (RFC 6056), timestamp offsets (RFC 7323) and IP IDs
	(airos-works/haiku#127).
*/


#include <string.h>

#include <OS.h>
#include <SupportDefs.h>

#ifdef _KERNEL_MODE
#	include <util/Random.h>
#endif


struct net_secret {
	uint64	key[2];
};


static inline uint64
net_secret_rotate(uint64 value, int bits)
{
	return (value << bits) | (value >> (64 - bits));
}


#define NET_SIP_ROUND(v0, v1, v2, v3) \
	do { \
		v0 += v1; v1 = net_secret_rotate(v1, 13); v1 ^= v0; \
		v0 = net_secret_rotate(v0, 32); \
		v2 += v3; v3 = net_secret_rotate(v3, 16); v3 ^= v2; \
		v0 += v3; v3 = net_secret_rotate(v3, 21); v3 ^= v0; \
		v2 += v1; v1 = net_secret_rotate(v1, 17); v1 ^= v2; \
		v2 = net_secret_rotate(v2, 32); \
	} while (false)


/*!	SipHash-2-4 of \a length bytes at \a data, keyed with \a secret. */
static inline uint64
net_secret_hash(const net_secret& secret, const void* data, size_t length)
{
	uint64 v0 = 0x736f6d6570736575ULL ^ secret.key[0];
	uint64 v1 = 0x646f72616e646f6dULL ^ secret.key[1];
	uint64 v2 = 0x6c7967656e657261ULL ^ secret.key[0];
	uint64 v3 = 0x7465646279746573ULL ^ secret.key[1];

	const uint8* bytes = (const uint8*)data;
	size_t left = length;
	while (left >= 8) {
		uint64 m;
		memcpy(&m, bytes, 8);
			// the result only has to be stable on this machine, so the
			// byte order does not matter
		v3 ^= m;
		NET_SIP_ROUND(v0, v1, v2, v3);
		NET_SIP_ROUND(v0, v1, v2, v3);
		v0 ^= m;
		bytes += 8;
		left -= 8;
	}

	uint64 m = (uint64)(length & 0xff) << 56;
	for (size_t i = 0; i < left; i++)
		m |= (uint64)bytes[i] << (8 * i);

	v3 ^= m;
	NET_SIP_ROUND(v0, v1, v2, v3);
	NET_SIP_ROUND(v0, v1, v2, v3);
	v0 ^= m;

	v2 ^= 0xff;
	for (int i = 0; i < 4; i++)
		NET_SIP_ROUND(v0, v1, v2, v3);

	return v0 ^ v1 ^ v2 ^ v3;
}

#undef NET_SIP_ROUND


#if defined(_KERNEL_MODE) && (defined(__x86_64__) || defined(__i386__))
/*!	A value from the CPU's random number generator (RDRAND), or 0. */
static inline uint64
net_secret_hardware_random()
{
	uint32 eax, ebx, ecx, edx;
	__asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
		: "a"(1), "c"(0));
	if ((ecx & (1u << 30)) == 0)
		return 0;

	for (int i = 0; i < 10; i++) {
		unsigned long value;
		unsigned char ok;
		__asm__ volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(ok));
		if (ok)
			return value;
	}
	return 0;
}
#else
static inline uint64
net_secret_hardware_random()
{
	return 0;
}
#endif


/*!	Fills \a secret with random bytes: the CPU's random number generator
	where there is one, mixed with the kernel's random values and the time.
	This runs in module initialization, with the module lock held, so it must
	not open /dev/urandom: the random device may need the device manager,
	whose thread may be waiting for the module lock.
*/
static inline void
net_secret_init(net_secret& secret)
{
	memset(&secret, 0, sizeof(secret));

	uint64 seed[6] = {
		net_secret_hardware_random(),
		net_secret_hardware_random(),
		(uint64)system_time(),
		(uint64)find_thread(NULL),
		0, 0
	};
#ifdef _KERNEL_MODE
	seed[4] = secure_get_random<uint64>();
	seed[5] = secure_get_random<uint64>();
#endif

	secret.key[0] = net_secret_hash(secret, seed, sizeof(seed));
	seed[0] ^= 0x5bd1e995;
	secret.key[1] = net_secret_hash(secret, seed, sizeof(seed));
}


#endif	// NET_SECRET_H
