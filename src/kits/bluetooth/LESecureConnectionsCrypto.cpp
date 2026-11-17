/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * LE Secure Connections cryptographic functions (Bluetooth Core Vol 3 Part H
 * 2.2.5-2.2.9) on AES-128 (OpenBSD's, as for legacy pairing) and P-256
 * (micro-ecc, see LEP256.c). Checked against the specification's sample data
 * (Appendix D) by src/tests/kits/bluetooth/le_sc_crypto_test.
 */

#include <LESecureConnectionsCrypto.h>

#include <string.h>

#include "LEP256.h"

extern "C" {
#include "../../libs/compat/openbsd_wlan/crypto/aes.h"
}


namespace Bluetooth {

static void
ClearSecret(void* data, size_t size)
{
	volatile uint8_t* bytes = (volatile uint8_t*)data;
	while (size-- > 0)
		*bytes++ = 0;
}


// Copies length octets to destination in the opposite order.
static void
Reverse(uint8_t* destination, const uint8_t* source, size_t length)
{
	for (size_t i = 0; i < length; i++)
		destination[i] = source[length - 1 - i];
}


static void
ShiftLeft(const uint8_t input[16], uint8_t output[16])
{
	for (int i = 0; i < 15; i++)
		output[i] = (uint8_t)(input[i] << 1 | input[i + 1] >> 7);
	output[15] = (uint8_t)(input[15] << 1);
}


static void
NextSubkey(uint8_t key[16])
{
	bool carry = (key[0] & 0x80) != 0;
	ShiftLeft(key, key);
	if (carry)
		key[15] ^= 0x87;
}


bool
LEAesCmac(const uint8_t key[16], const uint8_t* message, size_t length,
	uint8_t mac[16])
{
	if (key == NULL || mac == NULL || (message == NULL && length > 0))
		return false;

	AES_CTX context;
	memset(&context, 0, sizeof(context));
	if (AES_Setkey(&context, key, 16) != 0) {
		ClearSecret(&context, sizeof(context));
		return false;
	}

	// Subkeys K1 and K2 (RFC 4493 2.3).
	uint8_t subkey[16] = {};
	AES_Encrypt(&context, subkey, subkey);
	NextSubkey(subkey);

	size_t blocks = (length + 15) / 16;
	bool complete = blocks > 0 && length % 16 == 0;
	if (blocks == 0)
		blocks = 1;

	uint8_t state[16] = {};
	for (size_t block = 0; block + 1 < blocks; block++) {
		for (int i = 0; i < 16; i++)
			state[i] ^= message[block * 16 + i];
		AES_Encrypt(&context, state, state);
	}

	uint8_t last[16] = {};
	size_t offset = (blocks - 1) * 16;
	size_t remaining = length - offset;
	if (remaining > 16)
		remaining = 16;
	for (size_t i = 0; i < remaining; i++)
		last[i] = message[offset + i];
	if (!complete) {
		last[remaining] = 0x80;
		NextSubkey(subkey);
	}
	for (int i = 0; i < 16; i++)
		state[i] ^= last[i] ^ subkey[i];
	AES_Encrypt(&context, state, mac);

	ClearSecret(&context, sizeof(context));
	ClearSecret(subkey, sizeof(subkey));
	ClearSecret(state, sizeof(state));
	ClearSecret(last, sizeof(last));
	return true;
}


// CMAC with a key and a result in wire order and a message already in the
// specification's order.
static bool
CmacWire(const uint8_t key[16], const uint8_t* message, size_t length,
	uint8_t result[16])
{
	uint8_t orderedKey[16];
	uint8_t mac[16];
	Reverse(orderedKey, key, 16);
	bool okay = LEAesCmac(orderedKey, message, length, mac);
	if (okay)
		Reverse(result, mac, 16);
	ClearSecret(orderedKey, sizeof(orderedKey));
	ClearSecret(mac, sizeof(mac));
	return okay;
}


static void
AppendAddress(uint8_t* message, uint8_t type, const uint8_t address[6])
{
	message[0] = type;
	Reverse(message + 1, address, 6);
}


bool
LESCConfirm(const uint8_t u[32], const uint8_t v[32], const uint8_t x[16],
	uint8_t z, uint8_t confirm[16])
{
	if (u == NULL || v == NULL || x == NULL || confirm == NULL)
		return false;
	uint8_t message[65];
	Reverse(message, u, 32);
	Reverse(message + 32, v, 32);
	message[64] = z;
	bool okay = CmacWire(x, message, sizeof(message), confirm);
	ClearSecret(message, sizeof(message));
	return okay;
}


bool
LESCKeys(const uint8_t dhKey[32], const uint8_t n1[16], const uint8_t n2[16],
	uint8_t a1Type, const uint8_t a1[6], uint8_t a2Type, const uint8_t a2[6],
	uint8_t macKey[16], uint8_t longTermKey[16])
{
	if (dhKey == NULL || n1 == NULL || n2 == NULL || a1 == NULL || a2 == NULL
		|| macKey == NULL || longTermKey == NULL || a1Type > 1 || a2Type > 1)
		return false;

	static const uint8_t kSalt[16] = {
		0x6c, 0x88, 0x83, 0x91, 0xaa, 0xf5, 0xa5, 0x38,
		0x60, 0x37, 0x0b, 0xdb, 0x5a, 0x60, 0x83, 0xbe
	};
	uint8_t w[32];
	uint8_t t[16];
	Reverse(w, dhKey, 32);
	bool okay = LEAesCmac(kSalt, w, sizeof(w), t);

	// Counter || keyID "btle" || N1 || N2 || A1 || A2 || Length (256)
	uint8_t message[53];
	message[0] = 0;
	message[1] = 0x62;
	message[2] = 0x74;
	message[3] = 0x6c;
	message[4] = 0x65;
	Reverse(message + 5, n1, 16);
	Reverse(message + 21, n2, 16);
	AppendAddress(message + 37, a1Type, a1);
	AppendAddress(message + 44, a2Type, a2);
	message[51] = 0x01;
	message[52] = 0x00;

	uint8_t result[16];
	if (okay)
		okay = LEAesCmac(t, message, sizeof(message), result);
	if (okay) {
		Reverse(macKey, result, 16);
		message[0] = 1;
		okay = LEAesCmac(t, message, sizeof(message), result);
	}
	if (okay)
		Reverse(longTermKey, result, 16);
	else {
		ClearSecret(macKey, 16);
		ClearSecret(longTermKey, 16);
	}
	ClearSecret(w, sizeof(w));
	ClearSecret(t, sizeof(t));
	ClearSecret(message, sizeof(message));
	ClearSecret(result, sizeof(result));
	return okay;
}


bool
LESCCheck(const uint8_t macKey[16], const uint8_t n1[16], const uint8_t n2[16],
	const uint8_t r[16], const uint8_t ioCapability[3], uint8_t a1Type,
	const uint8_t a1[6], uint8_t a2Type, const uint8_t a2[6],
	uint8_t check[16])
{
	if (macKey == NULL || n1 == NULL || n2 == NULL || r == NULL
		|| ioCapability == NULL || a1 == NULL || a2 == NULL || check == NULL
		|| a1Type > 1 || a2Type > 1)
		return false;
	// N1 || N2 || R || IOcap || A1 || A2
	uint8_t message[65];
	Reverse(message, n1, 16);
	Reverse(message + 16, n2, 16);
	Reverse(message + 32, r, 16);
	Reverse(message + 48, ioCapability, 3);
	AppendAddress(message + 51, a1Type, a1);
	AppendAddress(message + 58, a2Type, a2);
	bool okay = CmacWire(macKey, message, sizeof(message), check);
	ClearSecret(message, sizeof(message));
	return okay;
}


bool
LESCNumericValue(const uint8_t u[32], const uint8_t v[32],
	const uint8_t x[16], const uint8_t y[16], uint32_t* value)
{
	if (u == NULL || v == NULL || x == NULL || y == NULL || value == NULL)
		return false;
	uint8_t message[80];
	Reverse(message, u, 32);
	Reverse(message + 32, v, 32);
	Reverse(message + 64, y, 16);
	uint8_t result[16];
	bool okay = CmacWire(x, message, sizeof(message), result);
	// mod 2^32: the four least significant octets.
	*value = okay ? (uint32_t)result[0] | (uint32_t)result[1] << 8
		| (uint32_t)result[2] << 16 | (uint32_t)result[3] << 24 : 0;
	ClearSecret(result, sizeof(result));
	return okay;
}


// #pragma mark - P-256


static void
PublicKeyToWire(const uint8_t ordered[64], uint8_t wire[64])
{
	Reverse(wire, ordered, 32);
	Reverse(wire + 32, ordered + 32, 32);
}


bool
LEP256GenerateKey(uint8_t publicKey[64], uint8_t privateKey[32])
{
	if (publicKey == NULL || privateKey == NULL)
		return false;
	uint8_t orderedPublic[64];
	uint8_t orderedPrivate[32];
	bool okay = le_p256_make_key(orderedPublic, orderedPrivate) != 0;
	if (okay) {
		PublicKeyToWire(orderedPublic, publicKey);
		Reverse(privateKey, orderedPrivate, 32);
	}
	ClearSecret(orderedPrivate, sizeof(orderedPrivate));
	return okay;
}


bool
LEP256PublicKey(const uint8_t privateKey[32], uint8_t publicKey[64])
{
	if (publicKey == NULL || privateKey == NULL)
		return false;
	uint8_t orderedPublic[64];
	uint8_t orderedPrivate[32];
	Reverse(orderedPrivate, privateKey, 32);
	bool okay = le_p256_compute_public_key(orderedPrivate, orderedPublic) != 0;
	if (okay)
		PublicKeyToWire(orderedPublic, publicKey);
	ClearSecret(orderedPrivate, sizeof(orderedPrivate));
	return okay;
}


bool
LEP256ValidPublicKey(const uint8_t publicKey[64])
{
	if (publicKey == NULL)
		return false;
	uint8_t ordered[64];
	PublicKeyToWire(publicKey, ordered);
	return le_p256_valid_public_key(ordered) != 0;
}


bool
LEP256DHKey(const uint8_t peerPublicKey[64], const uint8_t privateKey[32],
	uint8_t dhKey[32])
{
	if (peerPublicKey == NULL || privateKey == NULL || dhKey == NULL)
		return false;
	uint8_t orderedPublic[64];
	uint8_t orderedPrivate[32];
	uint8_t secret[32];
	PublicKeyToWire(peerPublicKey, orderedPublic);
	Reverse(orderedPrivate, privateKey, 32);
	bool okay = le_p256_shared_secret(orderedPublic, orderedPrivate, secret)
		!= 0;
	if (okay)
		Reverse(dhKey, secret, 32);
	else
		ClearSecret(dhKey, 32);
	ClearSecret(orderedPrivate, sizeof(orderedPrivate));
	ClearSecret(secret, sizeof(secret));
	return okay;
}


bool
LEP256IsDebugKey(const uint8_t publicKey[64])
{
	// Core Vol 3 Part H 2.3.5.6.1, X then Y in wire order.
	static const uint8_t kDebugPublicKey[64] = {
		0xe6, 0x9d, 0x35, 0x0e, 0x48, 0x01, 0x03, 0xcc,
		0xdb, 0xfd, 0xf4, 0xac, 0x11, 0x91, 0xf4, 0xef,
		0xb9, 0xa5, 0xf9, 0xe9, 0xa7, 0x83, 0x2c, 0x5e,
		0x2c, 0xbe, 0x97, 0xf2, 0xd2, 0x03, 0xb0, 0x20,
		0x8b, 0xd2, 0x89, 0x15, 0xd0, 0x8e, 0x1c, 0x74,
		0x24, 0x30, 0xed, 0x8f, 0xc2, 0x45, 0x63, 0x76,
		0x5c, 0x15, 0x52, 0x5a, 0xbf, 0x9a, 0x32, 0x63,
		0x6d, 0xeb, 0x2a, 0x65, 0x49, 0x9c, 0x80, 0xdc
	};
	return publicKey != NULL
		&& memcmp(publicKey, kDebugPublicKey, sizeof(kDebugPublicKey)) == 0;
}


// #pragma mark - association model


LESMPMethod
LESMPSelectMethod(bool secureConnections, bool mitm, uint8_t initiatorIO,
	uint8_t responderIO)
{
	if (!mitm || initiatorIO > LE_SMP_IO_KEYBOARD_DISPLAY
		|| responderIO > LE_SMP_IO_KEYBOARD_DISPLAY)
		return LE_SMP_JUST_WORKS;

	const LESMPMethod JW = LE_SMP_JUST_WORKS;
	const LESMPMethod ID = LE_SMP_PASSKEY_INITIATOR_DISPLAYS;
	const LESMPMethod RD = LE_SMP_PASSKEY_RESPONDER_DISPLAYS;
	const LESMPMethod BI = LE_SMP_PASSKEY_BOTH_INPUT;
	// Rows: responder; columns: initiator. Display only, display yes/no,
	// keyboard only, no input no output, keyboard display. The entries
	// that differ for Secure Connections are corrected below.
	static const LESMPMethod kTable[5][5] = {
		{ JW, JW, RD, JW, RD },
		{ JW, JW, RD, JW, RD },
		{ ID, ID, BI, JW, ID },
		{ JW, JW, JW, JW, JW },
		{ ID, ID, RD, JW, ID }
	};
	if (secureConnections) {
		bool initiatorConfirms = initiatorIO == LE_SMP_IO_DISPLAY_YES_NO
			|| initiatorIO == LE_SMP_IO_KEYBOARD_DISPLAY;
		bool responderConfirms = responderIO == LE_SMP_IO_DISPLAY_YES_NO
			|| responderIO == LE_SMP_IO_KEYBOARD_DISPLAY;
		if (initiatorConfirms && responderConfirms)
			return LE_SMP_NUMERIC_COMPARISON;
	}
	return kTable[responderIO][initiatorIO];
}

} // namespace Bluetooth
