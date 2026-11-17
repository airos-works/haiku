/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * P-256 from the vendored micro-ecc (LEP256.c), internal to the Bluetooth
 * kit. Keys are big-endian, the public key X || Y; all return 1 on success.
 */
#ifndef _LE_P256_H_
#define _LE_P256_H_

#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

int le_p256_make_key(uint8_t publicKey[64], uint8_t privateKey[32]);
int le_p256_compute_public_key(const uint8_t privateKey[32],
	uint8_t publicKey[64]);
int le_p256_valid_public_key(const uint8_t publicKey[64]);
// Fails for a public key that is not on the curve.
int le_p256_shared_secret(const uint8_t publicKey[64],
	const uint8_t privateKey[32], uint8_t secret[32]);

#ifdef __cplusplus
}
#endif

#endif
