/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef _LE_SECURE_CONNECTIONS_CRYPTO_H_
#define _LE_SECURE_CONNECTIONS_CRYPTO_H_

#include <stddef.h>
#include <stdint.h>


namespace Bluetooth {

// The LE Secure Connections toolbox (Bluetooth Core Vol 3 Part H 2.2.5-2.2.9)
// and the association model choice (2.3.5.1). Unless noted otherwise, values
// are in Bluetooth wire order, least significant octet first, as SMP carries
// them; the functions convert to the specification's most significant first
// order themselves. Addresses are 6 octets with a separate type (0 public, 1
// random). All return false only on a bad argument or a failed primitive.

// AES-CMAC (RFC 4493) with AES-128. Key, message and MAC here are in the
// specification's order, most significant octet first.
bool LEAesCmac(const uint8_t key[16], const uint8_t* message, size_t length,
	uint8_t mac[16]);

// f4(U, V, X, Z): confirm values.
bool LESCConfirm(const uint8_t u[32], const uint8_t v[32],
	const uint8_t x[16], uint8_t z, uint8_t confirm[16]);

// f5(W, N1, N2, A1, A2): the MacKey and the LTK from the DHKey.
bool LESCKeys(const uint8_t dhKey[32], const uint8_t n1[16],
	const uint8_t n2[16], uint8_t a1Type, const uint8_t a1[6], uint8_t a2Type,
	const uint8_t a2[6], uint8_t macKey[16], uint8_t longTermKey[16]);

// f6(W, N1, N2, R, IOcap, A1, A2): DHKey checks. ioCapability is the
// device's IO capability, OOB flag and AuthReq octets as in its pairing
// request or response (IO capability first).
bool LESCCheck(const uint8_t macKey[16], const uint8_t n1[16],
	const uint8_t n2[16], const uint8_t r[16], const uint8_t ioCapability[3],
	uint8_t a1Type, const uint8_t a1[6], uint8_t a2Type, const uint8_t a2[6],
	uint8_t check[16]);

// g2(U, V, X, Y): the 32-bit value; the number both sides show is its
// remainder by 1000000.
bool LESCNumericValue(const uint8_t u[32], const uint8_t v[32],
	const uint8_t x[16], const uint8_t y[16], uint32_t* value);

// P-256. A public key is 64 octets, X then Y, each least significant octet
// first (the Pairing Public Key payload); private keys and the DHKey are 32
// octets, least significant first.
bool LEP256GenerateKey(uint8_t publicKey[64], uint8_t privateKey[32]);
bool LEP256PublicKey(const uint8_t privateKey[32], uint8_t publicKey[64]);
bool LEP256ValidPublicKey(const uint8_t publicKey[64]);
// Fails for a point that is not on the curve.
bool LEP256DHKey(const uint8_t peerPublicKey[64],
	const uint8_t privateKey[32], uint8_t dhKey[32]);
// The specification's debug key pair (2.3.5.6.1); a device using it in
// pairing offers no secrecy.
bool LEP256IsDebugKey(const uint8_t publicKey[64]);


// SMP IO capabilities.
enum {
	LE_SMP_IO_DISPLAY_ONLY			= 0x00,
	LE_SMP_IO_DISPLAY_YES_NO		= 0x01,
	LE_SMP_IO_KEYBOARD_ONLY			= 0x02,
	LE_SMP_IO_NO_INPUT_NO_OUTPUT	= 0x03,
	LE_SMP_IO_KEYBOARD_DISPLAY		= 0x04
};

enum LESMPMethod {
	LE_SMP_JUST_WORKS = 0,
	LE_SMP_NUMERIC_COMPARISON,
		// Secure Connections only
	LE_SMP_PASSKEY_INITIATOR_DISPLAYS,
		// the responder types what the initiator shows
	LE_SMP_PASSKEY_RESPONDER_DISPLAYS,
		// the initiator types what the responder shows
	LE_SMP_PASSKEY_BOTH_INPUT
		// the user types the same number on both
};

// The association model for an initiator and a responder with these IO
// capabilities (Core Vol 3 Part H 2.3.5.1, Table 2.8), out of band data
// aside. mitm is true when either side set the MITM flag; without it the
// model is Just Works.
LESMPMethod LESMPSelectMethod(bool secureConnections, bool mitm,
	uint8_t initiatorIO, uint8_t responderIO);

} // namespace Bluetooth

#endif
