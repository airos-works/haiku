/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Checks the LE pairing cryptography against the Bluetooth Core
	specification's sample data (Vol 3 Part H, 2.2.3, 2.2.4 and Appendix D):
	AES-CMAC (the RFC 4493 vectors D.1 uses), both P-256 data sets (public
	keys from the private keys, the DHKey both ways), f4, f5, f6, g2, ah,
	legacy c1 and s1, plus the association model table (2.3.5.1).

	Values are written as the specification prints them (most significant
	octet first) and converted to wire order here, so a byte-order mistake in
	the code under test cannot cancel one in the test.

	Runs on the host (run.sh) and on Haiku (jam le_sc_crypto_test).
*/


#include <LELegacyPairingCrypto.h>
#include <LESecureConnectionsCrypto.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


using namespace Bluetooth;

static int sFailures = 0;
static int sChecks = 0;


static void
Check(bool condition, const char* what)
{
	sChecks++;
	printf("%s: %s\n", condition ? "PASS" : "FAIL", what);
	if (!condition)
		sFailures++;
}


// A step whose result the next check compares: reported only if it fails.
static void
Require(bool condition, const char* what)
{
	if (!condition) {
		printf("FAIL: %s could not be computed\n", what);
		sFailures++;
	}
}


// Parses hex as printed in the specification (spaces allowed) into
// most-significant-first octets.
static size_t
Parse(const char* text, uint8_t* output, size_t capacity)
{
	size_t count = 0;
	int high = -1;
	for (; *text != '\0'; text++) {
		if (isspace((unsigned char)*text))
			continue;
		int value;
		if (*text >= '0' && *text <= '9')
			value = *text - '0';
		else if (*text >= 'a' && *text <= 'f')
			value = *text - 'a' + 10;
		else if (*text >= 'A' && *text <= 'F')
			value = *text - 'A' + 10;
		else {
			fprintf(stderr, "bad hex digit '%c'\n", *text);
			exit(2);
		}
		if (high < 0)
			high = value;
		else {
			if (count == capacity) {
				fprintf(stderr, "hex value too long\n");
				exit(2);
			}
			output[count++] = (uint8_t)(high << 4 | value);
			high = -1;
		}
	}
	if (high >= 0) {
		fprintf(stderr, "odd number of hex digits\n");
		exit(2);
	}
	return count;
}


// The specification's text to wire order (least significant octet first).
static void
Wire(const char* text, uint8_t* output, size_t length)
{
	uint8_t ordered[128];
	if (Parse(text, ordered, sizeof(ordered)) != length) {
		fprintf(stderr, "wrong length for %s\n", text);
		exit(2);
	}
	for (size_t i = 0; i < length; i++)
		output[i] = ordered[length - 1 - i];
}


static void
Ordered(const char* text, uint8_t* output, size_t length)
{
	if (Parse(text, output, length) != length) {
		fprintf(stderr, "wrong length for %s\n", text);
		exit(2);
	}
}


static void
Dump(const char* label, const uint8_t* data, size_t length)
{
	printf("  %s:", label);
	for (size_t i = length; i-- > 0;)
		printf("%s%02x", (length - 1 - i) % 4 == 0 ? " " : "", data[i]);
	printf("\n");
}


static void
CheckEqual(const uint8_t* actual, const uint8_t* expected, size_t length,
	const char* what)
{
	bool equal = memcmp(actual, expected, length) == 0;
	Check(equal, what);
	if (!equal) {
		Dump("got     ", actual, length);
		Dump("expected", expected, length);
	}
}


// #pragma mark - AES-CMAC


static void
TestCmac()
{
	// RFC 4493 4 (also Core Vol 3 Part H D.1).
	uint8_t key[16];
	Ordered("2b7e1516 28aed2a6 abf71588 09cf4f3c", key, 16);
	uint8_t message[64];
	Ordered("6bc1bee2 2e409f96 e93d7e11 7393172a ae2d8a57 1e03ac9c 9eb76fac "
		"45af8e51 30c81c46 a35ce411 e5fbc119 1a0a52ef f69f2445 df4f9b17 "
		"ad2b417b e66c3710", message, 64);
	struct {
		size_t length;
		const char* mac;
		const char* name;
	} cases[] = {
		{ 0, "bb1d6929 e9593728 7fa37d12 9b756746", "AES-CMAC, empty message" },
		{ 16, "070a16b4 6b4d4144 f79bdd9d d04a287c", "AES-CMAC, 16 octets" },
		{ 40, "dfa66747 de9ae630 30ca3261 1497c827", "AES-CMAC, 40 octets" },
		{ 64, "51f0bebf 7e3b9d92 fc497417 79363cfe", "AES-CMAC, 64 octets" }
	};
	for (const auto& test : cases) {
		uint8_t expected[16], mac[16];
		Ordered(test.mac, expected, 16);
		Require(LEAesCmac(key, message, test.length, mac), test.name);
		CheckEqual(mac, expected, 16, test.name);
	}
}


// #pragma mark - P-256


struct P256DataSet {
	const char* name;
	const char* privateA;
	const char* privateB;
	const char* publicAX;
	const char* publicAY;
	const char* publicBX;
	const char* publicBY;
	const char* dhKey;
};


// Core Vol 3 Part H, Appendix D ("P-256 sample data"), data sets 1 and 2.
static const P256DataSet kP256DataSets[] = {
	{
		"P-256 data set 1",
		"3f49f6d4 a3c55f38 74c9b3e3 d2103f50 4aff607b eb40b799 5899b8a6 "
			"cd3c1abd",
		"55188b3d 32f6bb9a 900afcfb eed4e72a 59cb9ac2 f19d7cfb 6b4fdd49 "
			"f47fc5fd",
		"20b003d2 f297be2c 5e2c83a7 e9f9a5b9 eff49111 acf4fddb cc030148 "
			"0e359de6",
		"dc809c49 652aeb6d 63329abf 5a52155c 766345c2 8fed3024 741c8ed0 "
			"1589d28b",
		"1ea1f0f0 1faf1d96 09592284 f19e4c00 47b58afd 8615a69f 559077b2 "
			"2faaa190",
		"4c55f33e 429dad37 7356703a 9ab85160 472d1130 e28e3676 5f89aff9 "
			"15b1214a",
		"ec0234a3 57c8ad05 341010a6 0a397d9b 99796b13 b4f866f1 868d34f3 "
			"73bfa698"
	},
	{
		"P-256 data set 2",
		"06a51669 3c9aa31a 6084545d 0c5db641 b48572b9 7203ddff b7ac73f7 "
			"d0457663",
		"529aa067 0d72cd64 97502ed4 73502b03 7e8803b5 c60829a5 a3caa219 "
			"505530ba",
		"2c31a47b 5779809e f44cb5ea af5c3e43 d5f8faad 4a8794cb 987e9b03 "
			"745c78dd",
		"91951218 3898dfbe cd52e240 8e43871f d0211091 17bd3ed4 eaf84377 "
			"43715d4f",
		"f465e43f f23d3f1b 9dc7dfc0 4da87581 84dbc966 204796ec cf0d6cf5 "
			"e16500cc",
		"0201d048 bcbbd899 eeefc424 164e33c2 01c2b010 ca6b4d43 a8a155ca "
			"d8ecb279",
		"ab85843a 2f6d883f 62e5684b 38e30733 5fe6e194 5ecd1960 4105c6f2 "
			"3221eb69"
	}
};


static void
TestP256DataSet(const P256DataSet& set)
{
	uint8_t privateA[32], privateB[32];
	uint8_t expectedA[64], expectedB[64], expectedKey[32];
	Wire(set.privateA, privateA, 32);
	Wire(set.privateB, privateB, 32);
	Wire(set.publicAX, expectedA, 32);
	Wire(set.publicAY, expectedA + 32, 32);
	Wire(set.publicBX, expectedB, 32);
	Wire(set.publicBY, expectedB + 32, 32);
	Wire(set.dhKey, expectedKey, 32);

	char what[128];
	uint8_t publicA[64], publicB[64];
	snprintf(what, sizeof(what), "%s: public key A from private key A",
		set.name);
	Require(LEP256PublicKey(privateA, publicA), what);
	CheckEqual(publicA, expectedA, 64, what);
	snprintf(what, sizeof(what), "%s: public key B from private key B",
		set.name);
	Require(LEP256PublicKey(privateB, publicB), what);
	CheckEqual(publicB, expectedB, 64, what);

	snprintf(what, sizeof(what), "%s: public keys are on the curve",
		set.name);
	Check(LEP256ValidPublicKey(expectedA) && LEP256ValidPublicKey(expectedB),
		what);

	uint8_t key[32];
	snprintf(what, sizeof(what), "%s: DHKey = P-256(private A, public B)",
		set.name);
	Require(LEP256DHKey(expectedB, privateA, key), what);
	CheckEqual(key, expectedKey, 32, what);
	snprintf(what, sizeof(what), "%s: DHKey = P-256(private B, public A)",
		set.name);
	Require(LEP256DHKey(expectedA, privateB, key), what);
	CheckEqual(key, expectedKey, 32, what);

	// An off-curve point (Y changed) must be refused, not multiplied.
	uint8_t invalid[64];
	memcpy(invalid, expectedB, 64);
	invalid[40] ^= 0x01;
	snprintf(what, sizeof(what), "%s: off-curve public key refused",
		set.name);
	Check(!LEP256ValidPublicKey(invalid)
		&& !LEP256DHKey(invalid, privateA, key), what);
}


static void
TestP256()
{
	for (const P256DataSet& set : kP256DataSets)
		TestP256DataSet(set);

	// Data set 1's key A is the specification's debug key pair.
	uint8_t publicA[64];
	Wire(kP256DataSets[0].publicAX, publicA, 32);
	Wire(kP256DataSets[0].publicAY, publicA + 32, 32);
	Check(LEP256IsDebugKey(publicA), "the debug public key is recognised");
	uint8_t publicB[64];
	Wire(kP256DataSets[0].publicBX, publicB, 32);
	Wire(kP256DataSets[0].publicBY, publicB + 32, 32);
	Check(!LEP256IsDebugKey(publicB), "another public key is not the debug "
		"key");

	// Fresh key pairs agree on a DHKey and differ from each other.
	uint8_t public1[64], private1[32], public2[64], private2[32];
	uint8_t key1[32], key2[32];
	bool generated = LEP256GenerateKey(public1, private1)
		&& LEP256GenerateKey(public2, private2);
	Check(generated, "two key pairs generated");
	Check(generated && memcmp(public1, public2, 64) != 0
		&& LEP256ValidPublicKey(public1) && LEP256ValidPublicKey(public2),
		"generated public keys differ and are on the curve");
	uint8_t derived[64];
	Check(generated && LEP256PublicKey(private1, derived)
		&& memcmp(derived, public1, 64) == 0,
		"a generated public key matches its private key");
	Check(generated && LEP256DHKey(public2, private1, key1)
		&& LEP256DHKey(public1, private2, key2)
		&& memcmp(key1, key2, 32) == 0,
		"generated key pairs agree on the DHKey");
}


// #pragma mark - f4, f5, f6, g2


static const char* kU = "20b003d2 f297be2c 5e2c83a7 e9f9a5b9 eff49111 acf4fddb "
	"cc030148 0e359de6";
static const char* kV = "55188b3d 32f6bb9a 900afcfb eed4e72a 59cb9ac2 f19d7cfb "
	"6b4fdd49 f47fc5fd";
static const char* kX = "d5cb8454 d177733e ffffb2ec 712baeab";
static const char* kY = "a6e8e7cc 25a75f6e 216583f7 ff3dc4cf";
static const char* kW = "ec0234a3 57c8ad05 341010a6 0a397d9b 99796b13 b4f866f1 "
	"868d34f3 73bfa698";
static const char* kA1 = "561237 37bfce";
static const char* kA2 = "a71370 2dcfc1";


static void
TestF4()
{
	// D.2
	uint8_t u[32], v[32], x[16], expected[16], result[16];
	Wire(kU, u, 32);
	Wire(kV, v, 32);
	Wire(kX, x, 16);
	Wire("f2c916f1 07a9bd1c f1eda1be a974872d", expected, 16);
	Require(LESCConfirm(u, v, x, 0x00, result), "f4");
	CheckEqual(result, expected, 16, "f4(U, V, X, 0x00)");
}


static void
TestF5()
{
	// D.3
	uint8_t w[32], n1[16], n2[16], a1[6], a2[6];
	uint8_t expectedMac[16], expectedLtk[16], macKey[16], ltk[16];
	Wire(kW, w, 32);
	Wire(kX, n1, 16);
	Wire(kY, n2, 16);
	Wire(kA1, a1, 6);
	Wire(kA2, a2, 6);
	Wire("2965f176 a1084a02 fd3f6a20 ce636e20", expectedMac, 16);
	Wire("69867911 69d7cd23 980522b5 94750a38", expectedLtk, 16);
	Require(LESCKeys(w, n1, n2, 0, a1, 0, a2, macKey, ltk), "f5");
	CheckEqual(macKey, expectedMac, 16, "f5 MacKey");
	CheckEqual(ltk, expectedLtk, 16, "f5 LTK");
}


static void
TestF6()
{
	// D.4
	uint8_t w[16], n1[16], n2[16], r[16], ioCap[3], a1[6], a2[6];
	uint8_t expected[16], result[16];
	Wire("2965f176 a1084a02 fd3f6a20 ce636e20", w, 16);
	Wire(kX, n1, 16);
	Wire(kY, n2, 16);
	Wire("12a3343b b453bb54 08da42d2 0c2d0fc8", r, 16);
	Wire("010102", ioCap, 3);
	Wire(kA1, a1, 6);
	Wire(kA2, a2, 6);
	Wire("e3c47398 9cd0e8c5 d26c0b09 da958f61", expected, 16);
	Require(LESCCheck(w, n1, n2, r, ioCap, 0, a1, 0, a2, result), "f6");
	CheckEqual(result, expected, 16, "f6(W, N1, N2, R, IOcap, A1, A2)");
}


static void
TestG2()
{
	// D.5
	uint8_t u[32], v[32], x[16], y[16];
	Wire(kU, u, 32);
	Wire(kV, v, 32);
	Wire(kX, x, 16);
	Wire(kY, y, 16);
	uint32_t value = 0;
	Require(LESCNumericValue(u, v, x, y, &value), "g2");
	Check(value == 0x2f9ed5ba, "g2(U, V, X, Y) = 0x2f9ed5ba");
	if (value != 0x2f9ed5ba)
		printf("  got 0x%08x\n", value);
	Check(value % 1000000 == 938554, "g2 numeric comparison value 938554");
}


// #pragma mark - legacy and address resolution


static void
TestAh()
{
	// D.7: ah(IRK, prand) = 0x0dfbaa for prand 0x708194, so 70:81:94:0D:FB:AA
	// is a resolvable private address for this IRK.
	uint8_t irk[16], address[6];
	Wire("ec0234a3 57c8ad05 341010a6 0a397d9b", irk, 16);
	Wire("708194 0dfbaa", address, 6);
	Check(LEResolvePrivateAddress(irk, address),
		"ah: 70:81:94:0D:FB:AA resolves with the sample IRK");
	address[0] ^= 0x01;
	Check(!LEResolvePrivateAddress(irk, address),
		"ah: a different hash does not resolve");
	address[0] ^= 0x01;
	irk[0] ^= 0x01;
	Check(!LEResolvePrivateAddress(irk, address),
		"ah: a different IRK does not resolve");
}


static void
TestLegacy()
{
	// 2.2.3 c1 and 2.2.4 s1 examples.
	uint8_t tk[16] = {}, random[16], request[7], response[7];
	uint8_t initiator[6], responder[6], expected[16], result[16];
	Wire("5783d521 56ad6f0e 6388274e c6702ee0", random, 16);
	Wire("07071000000101", request, 7);
	Wire("05000800000302", response, 7);
	Wire("a1a2a3a4a5a6", initiator, 6);
	Wire("b1b2b3b4b5b6", responder, 6);
	Wire("1e1e3fef 878988ea d2a74dc5 bef13b86", expected, 16);
	Require(LELegacyConfirm(tk, random, request, response, 1, initiator, 0,
		responder, result), "c1");
	CheckEqual(result, expected, 16, "legacy c1 confirm value");

	uint8_t r1[16], r2[16];
	Wire("000f0e0d 0c0b0a09 11223344 55667788", r1, 16);
	Wire("01020304 05060708 99aabbcc ddeeff00", r2, 16);
	Wire("9a1fe1f0 e8b0f49b 5b4216ae 796da062", expected, 16);
	Require(LELegacyShortTermKey(tk, r1, r2, result), "s1");
	CheckEqual(result, expected, 16, "legacy s1 short-term key");
}


// #pragma mark - association model


static void
TestMethodTable()
{
	// Core Vol 3 Part H Table 2.8. Rows: responder; columns: initiator
	// (DisplayOnly, DisplayYesNo, KeyboardOnly, NoInputNoOutput,
	// KeyboardDisplay). J = Just Works, N = Numeric Comparison, I/R =
	// passkey shown by the initiator/responder, B = both type it.
	static const char* kLegacy[5] = {
		"JJRJR",
		"JJRJR",
		"IIBJI",
		"JJJJJ",
		"IIRJI"
	};
	static const char* kSecure[5] = {
		"JJRJR",
		"JNRJN",
		"IIBJI",
		"JJJJJ",
		"INRJN"
	};
	bool legacyOkay = true, secureOkay = true, noMitmOkay = true;
	for (int responder = 0; responder < 5; responder++) {
		for (int initiator = 0; initiator < 5; initiator++) {
			for (int sc = 0; sc < 2; sc++) {
				char expected = (sc ? kSecure : kLegacy)[responder][initiator];
				LESMPMethod method = LESMPSelectMethod(sc != 0, true,
					initiator, responder);
				char actual = "JNIRB"[method];
				if (actual != expected) {
					printf("  %s responder IO %d initiator IO %d: %c, "
						"expected %c\n", sc ? "SC" : "legacy", responder,
						initiator, actual, expected);
					(sc ? secureOkay : legacyOkay) = false;
				}
				if (LESMPSelectMethod(sc != 0, false, initiator, responder)
						!= LE_SMP_JUST_WORKS)
					noMitmOkay = false;
			}
		}
	}
	Check(legacyOkay, "association models, legacy pairing");
	Check(secureOkay, "association models, Secure Connections");
	Check(noMitmOkay, "without MITM on either side every pairing is Just "
		"Works");
}


int
main()
{
	TestCmac();
	TestP256();
	TestF4();
	TestF5();
	TestF6();
	TestG2();
	TestAh();
	TestLegacy();
	TestMethodTable();
	printf("%d checks, %d failed\n", sChecks, sFailures);
	return sFailures == 0 ? 0 : 1;
}
