/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * P-256 (secp256r1) for LE Secure Connections pairing, from the vendored
 * micro-ecc (src/libs/microecc, BSD 2-clause). Only this curve is built and
 * none of micro-ecc's symbols leave libbluetooth.
 *
 * Keys here are micro-ecc's: big-endian, the public key X || Y. The caller
 * (LESecureConnectionsCrypto.cpp) converts to and from SMP's wire order.
 */

#define uECC_SUPPORTS_secp160r1 0
#define uECC_SUPPORTS_secp192r1 0
#define uECC_SUPPORTS_secp224r1 0
#define uECC_SUPPORTS_secp256r1 1
#define uECC_SUPPORTS_secp256k1 0
#define uECC_SUPPORT_COMPRESSED_POINT 0

#include <unistd.h>
#ifndef __HAIKU__
#include <sys/random.h>
#endif

#include "LEP256.h"

#pragma GCC visibility push(hidden)

// micro-ecc as it comes; its ECDSA helpers have no prototypes.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#include "uECC.c"
#pragma GCC diagnostic pop


static int
le_p256_random(uint8_t* destination, unsigned size)
{
	while (size > 0) {
		unsigned chunk = size > 256 ? 256 : size;
		if (getentropy(destination, chunk) != 0)
			return 0;
		destination += chunk;
		size -= chunk;
	}
	return 1;
}


static void
le_p256_use_entropy(void)
{
	if (uECC_get_rng() != &le_p256_random)
		uECC_set_rng(&le_p256_random);
}


int
le_p256_make_key(uint8_t publicKey[64], uint8_t privateKey[32])
{
	le_p256_use_entropy();
	return uECC_make_key(publicKey, privateKey, uECC_secp256r1());
}


int
le_p256_compute_public_key(const uint8_t privateKey[32],
	uint8_t publicKey[64])
{
	le_p256_use_entropy();
	return uECC_compute_public_key(privateKey, publicKey, uECC_secp256r1());
}


int
le_p256_valid_public_key(const uint8_t publicKey[64])
{
	return uECC_valid_public_key(publicKey, uECC_secp256r1());
}


int
le_p256_shared_secret(const uint8_t publicKey[64],
	const uint8_t privateKey[32], uint8_t secret[32])
{
	le_p256_use_entropy();
	// An off-curve point would leak bits of the private key (the "invalid
	// curve" attack on Bluetooth pairing, CVE-2018-5383).
	if (!uECC_valid_public_key(publicKey, uECC_secp256r1()))
		return 0;
	return uECC_shared_secret(publicKey, privateKey, secret,
		uECC_secp256r1());
}

#pragma GCC visibility pop
