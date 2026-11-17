/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Runs the SMP initiator (LESMPPairAsInitiator) against a simulated
	responder over a SOCK_SEQPACKET socket pair: LE Secure Connections with
	Just Works, Numeric Comparison and Passkey Entry, legacy Just Works and
	Passkey Entry, and the ways a pairing must fail (a wrong passkey, a
	confirm value or DHKey check that does not match, an off-curve or debug
	public key, the user declining, the peer refusing).

	The responder is written from the responder's side of Core Vol 3 Part H
	2.3.5.5/2.3.5.6 on the same toolbox functions, which le_sc_crypto_test
	checks against the specification's sample data. Both sides must end up
	with the same key.

	Host only (run.sh): the SMP code is built against small stubs.
*/


#include <LELog.h>
#include <LELegacyPairingCrypto.h>
#include <LESMPInitiator.h>
#include <LESecureConnectionsCrypto.h>

#include "LESMPChannel.h"

#include <atomic>
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>


using namespace Bluetooth;


// #pragma mark - LELog stand-ins


namespace Bluetooth {

void
LELog(int32 level, const char* component, const char* format, ...)
{
	if (getenv("SMP_TEST_VERBOSE") == NULL)
		return;
	va_list args;
	va_start(args, format);
	printf("    [%s] ", component);
	vprintf(format, args);
	printf("\n");
	va_end(args);
}


const char*
LEAddressString(const uint8 address[6], char buffer[18])
{
	snprintf(buffer, 18, "%02X:%02X:%02X:%02X:%02X:%02X", address[5],
		address[4], address[3], address[2], address[1], address[0]);
	return buffer;
}

} // namespace Bluetooth


static int sFailures = 0;
static int sChecks = 0;


static void
Check(bool condition, const char* format, ...)
{
	sChecks++;
	va_list args;
	va_start(args, format);
	printf("%s: ", condition ? "PASS" : "FAIL");
	vprintf(format, args);
	printf("\n");
	va_end(args);
	if (!condition)
		sFailures++;
}


static const uint8 kInitiator[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
static const uint8 kInitiatorType = 0;
static const uint8 kResponder[6] = { 0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xc6 };
static const uint8 kResponderType = 1;


// #pragma mark - simulated responder


enum Fault {
	NO_FAULT,
	REFUSE,				// Pairing Failed instead of a response
	BAD_CONFIRM,		// a confirm value that does not match its random
	INVALID_KEY,		// a public key off the curve
	DEBUG_KEY,			// the specification's debug key pair
	WRONG_PASSKEY,		// the user types another number on the device
	REJECT_NUMERIC,		// the user rejects the numbers on the device
	BAD_DHKEY_CHECK		// a DHKey check that does not match
};

struct Responder {
	uint8 io;
	uint8 authentication;
	uint8 keySize;
	uint8 keys;
	Fault fault;

	// shared with the test's user agent
	std::atomic<int64_t>* shownByInitiator;
	std::atomic<int64_t> shownHere;

	// outcome
	bool completed;
	uint8 key[16];
	uint32 numeric;
	uint8 failureSent;
	uint8 failureReceived;

	Responder(uint8 io, uint8 authentication, Fault fault = NO_FAULT)
		:
		io(io), authentication(authentication), keySize(16), keys(0x03),
		fault(fault), shownByInitiator(NULL), shownHere(-1),
		completed(false), numeric(0), failureSent(0), failureReceived(0)
	{
		memset(key, 0, sizeof(key));
	}
};


class ResponderRun {
public:
	ResponderRun(int socket, Responder& config)
		: fSocket(socket), fConfig(config) {}

	void Run();

private:
	bool Receive(uint8 code, size_t length, uint8* packet);
	void Send(const uint8* packet, size_t length);
	void Fail(uint8 reason);
	bool Random(uint8* buffer, size_t length);
	void Secure(uint32 passkey, LESMPMethod method);
	void Legacy(uint32 passkey);

	int fSocket;
	Responder& fConfig;
	uint8 fRequest[7];
	uint8 fResponse[7];
};


bool
ResponderRun::Receive(uint8 code, size_t length, uint8* packet)
{
	pollfd readable = { fSocket, POLLIN, 0 };
	if (poll(&readable, 1, 5000) != 1)
		return false;
	uint8 buffer[80];
	ssize_t received = recv(fSocket, buffer, sizeof(buffer), 0);
	if (received >= 2 && buffer[0] == LE_SMP_PAIRING_FAILED) {
		fConfig.failureReceived = buffer[1];
		return false;
	}
	if (received != (ssize_t)length || buffer[0] != code) {
		printf("  responder: got code %#x (%zd bytes), wanted %#x (%zu)\n",
			received > 0 ? buffer[0] : 0, received, code, length);
		return false;
	}
	memcpy(packet, buffer, length);
	return true;
}


void
ResponderRun::Send(const uint8* packet, size_t length)
{
	send(fSocket, packet, length, 0);
}


void
ResponderRun::Fail(uint8 reason)
{
	const uint8 packet[2] = { LE_SMP_PAIRING_FAILED, reason };
	Send(packet, sizeof(packet));
	fConfig.failureSent = reason;
}


bool
ResponderRun::Random(uint8* buffer, size_t length)
{
	return getentropy(buffer, length) == 0;
}


static uint32
WaitForPasskey(std::atomic<int64_t>* source)
{
	for (int i = 0; i < 500 && source->load() < 0; i++)
		usleep(10000);
	return (uint32)source->load();
}


void
ResponderRun::Run()
{
	if (!Receive(LE_SMP_PAIRING_REQUEST, 7, fRequest))
		return;
	if (fConfig.fault == REFUSE) {
		Fail(LE_SMP_REASON_PAIRING_NOT_SUPPORTED);
		return;
	}
	fResponse[0] = LE_SMP_PAIRING_RESPONSE;
	fResponse[1] = fConfig.io;
	fResponse[2] = 0;
	fResponse[3] = fConfig.authentication;
	fResponse[4] = fConfig.keySize;
	fResponse[5] = 0;
	fResponse[6] = fRequest[6] & fConfig.keys;

	bool secure = (fRequest[3] & 0x08) != 0
		&& (fConfig.authentication & 0x08) != 0;
	bool mitm = ((fRequest[3] | fConfig.authentication) & 0x04) != 0;
	LESMPMethod method = LESMPSelectMethod(secure, mitm, fRequest[1],
		fConfig.io);

	// The passkey: the initiator shows it, or this side does.
	uint32 passkey = 0;
	if (method == LE_SMP_PASSKEY_RESPONDER_DISPLAYS
		|| method == LE_SMP_PASSKEY_BOTH_INPUT) {
		passkey = 654321;
		fConfig.shownHere = passkey;
	}
	Send(fResponse, sizeof(fResponse));

	if (method == LE_SMP_PASSKEY_INITIATOR_DISPLAYS) {
		passkey = WaitForPasskey(fConfig.shownByInitiator);
		if (fConfig.fault == WRONG_PASSKEY)
			passkey ^= 0x10;
	}

	if (secure)
		Secure(passkey, method);
	else
		Legacy(passkey);
}


void
ResponderRun::Secure(uint32 passkey, LESMPMethod method)
{
	uint8 packet[65];
	uint8 peerKey[64], publicKey[64], privateKey[32], dhKey[32];
	if (!Receive(LE_SMP_PAIRING_PUBLIC_KEY, 65, packet))
		return;
	memcpy(peerKey, packet + 1, 64);
	if (!LEP256ValidPublicKey(peerKey)) {
		Fail(LE_SMP_REASON_DHKEY_CHECK_FAILED);
		return;
	}
	if (fConfig.fault == DEBUG_KEY) {
		// Core Vol 3 Part H 2.3.5.6.1
		static const uint8 kDebugPrivate[32] = {
			0xbd, 0x1a, 0x3c, 0xcd, 0xa6, 0xb8, 0x99, 0x58,
			0x99, 0xb7, 0x40, 0xeb, 0x7b, 0x60, 0xff, 0x4a,
			0x50, 0x3f, 0x10, 0xd2, 0xe3, 0xb3, 0xc9, 0x74,
			0x38, 0x5f, 0xc5, 0xa3, 0xd4, 0xf6, 0x49, 0x3f
		};
		memcpy(privateKey, kDebugPrivate, 32);
		LEP256PublicKey(privateKey, publicKey);
	} else
		LEP256GenerateKey(publicKey, privateKey);
	packet[0] = LE_SMP_PAIRING_PUBLIC_KEY;
	memcpy(packet + 1, publicKey, 64);
	if (fConfig.fault == INVALID_KEY)
		packet[40] ^= 0x01;
	Send(packet, 65);
	if (!LEP256DHKey(peerKey, privateKey, dhKey))
		return;

	uint8 na[16], nb[16], r[16] = {};
	if (method == LE_SMP_JUST_WORKS || method == LE_SMP_NUMERIC_COMPARISON) {
		// Cb = f4(PKbx, PKax, Nb, 0)
		Random(nb, 16);
		packet[0] = LE_SMP_PAIRING_CONFIRM;
		LESCConfirm(publicKey, peerKey, nb, 0, packet + 1);
		if (fConfig.fault == BAD_CONFIRM)
			packet[5] ^= 0x40;
		Send(packet, 17);
		if (!Receive(LE_SMP_PAIRING_RANDOM, 17, packet))
			return;
		memcpy(na, packet + 1, 16);
		packet[0] = LE_SMP_PAIRING_RANDOM;
		memcpy(packet + 1, nb, 16);
		Send(packet, 17);
		uint32 value;
		LESCNumericValue(peerKey, publicKey, na, nb, &value);
		fConfig.numeric = value % 1000000;
	} else {
		for (int round = 0; round < 20; round++) {
			uint8 bit = 0x80 | ((passkey >> round) & 1);
			uint8 confirm[17], expected[16];
			if (!Receive(LE_SMP_PAIRING_CONFIRM, 17, confirm))
				return;
			Random(nb, 16);
			// Cbi = f4(PKbx, PKax, Nbi, rbi)
			packet[0] = LE_SMP_PAIRING_CONFIRM;
			LESCConfirm(publicKey, peerKey, nb, bit, packet + 1);
			Send(packet, 17);
			if (!Receive(LE_SMP_PAIRING_RANDOM, 17, packet))
				return;
			memcpy(na, packet + 1, 16);
			// Cai = f4(PKax, PKbx, Nai, rai)
			LESCConfirm(peerKey, publicKey, na, bit, expected);
			if (memcmp(expected, confirm + 1, 16) != 0) {
				Fail(LE_SMP_REASON_CONFIRM_VALUE_FAILED);
				return;
			}
			packet[0] = LE_SMP_PAIRING_RANDOM;
			memcpy(packet + 1, nb, 16);
			Send(packet, 17);
		}
		r[0] = passkey;
		r[1] = passkey >> 8;
		r[2] = passkey >> 16;
		r[3] = passkey >> 24;
	}

	uint8 macKey[16], longTermKey[16], expected[16];
	LESCKeys(dhKey, na, nb, kInitiatorType, kInitiator, kResponderType,
		kResponder, macKey, longTermKey);
	if (!Receive(LE_SMP_PAIRING_DHKEY_CHECK, 17, packet))
		return;
	// Ea = f6(MacKey, Na, Nb, rb, IOcapA, A, B)
	LESCCheck(macKey, na, nb, r, fRequest + 1, kInitiatorType, kInitiator,
		kResponderType, kResponder, expected);
	if (memcmp(expected, packet + 1, 16) != 0) {
		Fail(LE_SMP_REASON_DHKEY_CHECK_FAILED);
		return;
	}
	if (fConfig.fault == REJECT_NUMERIC) {
		Fail(LE_SMP_REASON_NUMERIC_COMPARISON_FAILED);
		return;
	}
	// Eb = f6(MacKey, Nb, Na, ra, IOcapB, B, A)
	packet[0] = LE_SMP_PAIRING_DHKEY_CHECK;
	LESCCheck(macKey, nb, na, r, fResponse + 1, kResponderType, kResponder,
		kInitiatorType, kInitiator, packet + 1);
	if (fConfig.fault == BAD_DHKEY_CHECK) {
		packet[16] ^= 0x01;
		Send(packet, 17);
		// The initiator's Pairing Failed is the last word.
		Receive(LE_SMP_PAIRING_FAILED, 2, packet);
		return;
	}
	Send(packet, 17);
	memcpy(fConfig.key, longTermKey, 16);
	for (int i = fConfig.keySize; i < 16; i++)
		fConfig.key[i] = 0;
	fConfig.completed = true;
}


void
ResponderRun::Legacy(uint32 passkey)
{
	uint8 tk[16] = {};
	tk[0] = passkey;
	tk[1] = passkey >> 8;
	tk[2] = passkey >> 16;
	tk[3] = passkey >> 24;
	uint8 confirm[17], packet[17], srand[16], mrand[16], expected[16];
	if (!Receive(LE_SMP_PAIRING_CONFIRM, 17, confirm))
		return;
	Random(srand, 16);
	packet[0] = LE_SMP_PAIRING_CONFIRM;
	LELegacyConfirm(tk, srand, fRequest, fResponse, kInitiatorType, kInitiator,
		kResponderType, kResponder, packet + 1);
	if (fConfig.fault == BAD_CONFIRM)
		packet[3] ^= 0x02;
	Send(packet, 17);
	if (!Receive(LE_SMP_PAIRING_RANDOM, 17, packet))
		return;
	memcpy(mrand, packet + 1, 16);
	LELegacyConfirm(tk, mrand, fRequest, fResponse, kInitiatorType, kInitiator,
		kResponderType, kResponder, expected);
	if (memcmp(expected, confirm + 1, 16) != 0) {
		Fail(LE_SMP_REASON_CONFIRM_VALUE_FAILED);
		return;
	}
	packet[0] = LE_SMP_PAIRING_RANDOM;
	memcpy(packet + 1, srand, 16);
	Send(packet, 17);
	if (fConfig.fault == BAD_CONFIRM) {
		// The initiator's Pairing Failed is the last word.
		Receive(LE_SMP_PAIRING_FAILED, 2, packet);
		return;
	}
	// STK = s1(TK, Srand, Mrand)
	LELegacyShortTermKey(tk, srand, mrand, fConfig.key);
	for (int i = fConfig.keySize; i < 16; i++)
		fConfig.key[i] = 0;
	fConfig.completed = true;
}


// #pragma mark - user agent


class TestAgent : public LESMPUserAgent {
public:
	TestAgent(bool accept)
		:
		accept(accept), confirms(0), confirmedValue(0),
		confirmedMethod(LE_SMP_JUST_WORKS), shown(-1), enterFrom(NULL),
		finished(false)
	{
	}

	bool Confirm(LESMPMethod method, uint32 value, bigtime_t) override
	{
		confirms++;
		confirmedMethod = method;
		confirmedValue = value;
		return accept;
	}

	void DisplayPasskey(uint32 passkey) override
	{
		shown = passkey;
	}

	bool EnterPasskey(uint32& passkey, bigtime_t) override
	{
		if (enterFrom == NULL)
			return false;
		passkey = WaitForPasskey(enterFrom);
		return true;
	}

	void Finished(status_t) override
	{
		finished = true;
	}

	bool accept;
	int confirms;
	uint32 confirmedValue;
	LESMPMethod confirmedMethod;
	std::atomic<int64_t> shown;
	std::atomic<int64_t>* enterFrom;
	bool finished;
};


// #pragma mark - cases


struct Outcome {
	status_t status;
	LESMPResult result;
	char detail[256];
};


static Outcome
Pair(Responder& responder, const LESMPOptions& options, TestAgent* agent)
{
	int sockets[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets) != 0) {
		perror("socketpair");
		exit(2);
	}
	std::atomic<int64_t> none(-1);
	responder.shownByInitiator = agent != NULL ? &agent->shown : &none;
	if (agent != NULL && agent->enterFrom != NULL)
		agent->enterFrom = &responder.shownHere;
	std::thread thread([&]() {
		ResponderRun run(sockets[1], responder);
		run.Run();
	});
	Outcome outcome;
	outcome.status = LESMPPairAsInitiator(sockets[0], kInitiator,
		kInitiatorType, kResponder, kResponderType, options, agent,
		outcome.result, NULL, outcome.detail, sizeof(outcome.detail));
	// The responder may still wait for a PDU the initiator will not send.
	shutdown(sockets[0], SHUT_RDWR);
	thread.join();
	close(sockets[0]);
	close(sockets[1]);
	if (outcome.status != B_OK)
		printf("  (initiator: \"%s\")\n", outcome.detail);
	return outcome;
}


static bool
SameKey(const Outcome& outcome, const Responder& responder)
{
	return outcome.status == B_OK && responder.completed
		&& memcmp(outcome.result.key, responder.key, 16) == 0;
}


static LESMPOptions
Options(uint8 io, bool mitm, bool secure = true)
{
	LESMPOptions options;
	options.ioCapability = io;
	options.mitm = mitm;
	options.secureConnections = secure;
	return options;
}


static void
TestSecureJustWorks()
{
	// The preferences' pairing: no IO, no MITM, Secure Connections.
	Responder responder(LE_SMP_IO_NO_INPUT_NO_OUTPUT, 0x09);
	Outcome outcome = Pair(responder,
		Options(LE_SMP_IO_NO_INPUT_NO_OUTPUT, false), NULL);
	Check(outcome.status == B_OK, "SC Just Works without an agent succeeds");
	Check(SameKey(outcome, responder), "SC Just Works: both sides have the "
		"same LTK");
	Check(outcome.result.secureConnections && !outcome.result.authenticated
		&& outcome.result.method == LE_SMP_JUST_WORKS
		&& outcome.result.bonding,
		"SC Just Works: Secure Connections, unauthenticated, bonding");
	Check(outcome.result.responderKeys == LE_SMP_KEY_IDENTITY,
		"SC: the encryption key bit is dropped from the keys to receive");

	TestAgent agent(true);
	Responder responder2(LE_SMP_IO_NO_INPUT_NO_OUTPUT, 0x09);
	outcome = Pair(responder2, Options(LE_SMP_IO_DISPLAY_YES_NO, true),
		&agent);
	Check(SameKey(outcome, responder2) && agent.confirms == 1
		&& agent.confirmedMethod == LE_SMP_JUST_WORKS && agent.finished,
		"SC Just Works with an agent: the user is asked once, keys agree");

	TestAgent declining(false);
	Responder responder3(LE_SMP_IO_NO_INPUT_NO_OUTPUT, 0x09);
	outcome = Pair(responder3, Options(LE_SMP_IO_DISPLAY_YES_NO, true),
		&declining);
	Check(outcome.status == B_CANCELED && !responder3.completed
		&& responder3.failureReceived != 0,
		"SC Just Works declined: B_CANCELED, the device is told");
}


static void
TestNumericComparison()
{
	TestAgent agent(true);
	Responder responder(LE_SMP_IO_DISPLAY_YES_NO, 0x0d);
	Outcome outcome = Pair(responder, Options(LE_SMP_IO_DISPLAY_YES_NO, true),
		&agent);
	Check(SameKey(outcome, responder), "Numeric Comparison: keys agree");
	Check(outcome.result.method == LE_SMP_NUMERIC_COMPARISON
		&& outcome.result.authenticated, "Numeric Comparison: authenticated");
	Check(agent.confirms == 1 && agent.confirmedValue == responder.numeric
		&& agent.confirmedValue < 1000000,
		"Numeric Comparison: the user sees the device's number (%06u/%06u)",
		agent.confirmedValue, responder.numeric);

	TestAgent rejecting(false);
	Responder responder2(LE_SMP_IO_KEYBOARD_DISPLAY, 0x0d);
	outcome = Pair(responder2, Options(LE_SMP_IO_DISPLAY_YES_NO, true),
		&rejecting);
	Check(outcome.status == B_CANCELED
		&& responder2.failureReceived
			== LE_SMP_REASON_NUMERIC_COMPARISON_FAILED,
		"Numeric Comparison rejected here: Numeric Comparison Failed sent");

	TestAgent accepting(true);
	Responder responder3(LE_SMP_IO_DISPLAY_YES_NO, 0x0d, REJECT_NUMERIC);
	outcome = Pair(responder3, Options(LE_SMP_IO_DISPLAY_YES_NO, true),
		&accepting);
	Check(outcome.status == B_PERMISSION_DENIED && outcome.result.remoteReason
		&& outcome.result.reason == LE_SMP_REASON_NUMERIC_COMPARISON_FAILED,
		"Numeric Comparison rejected on the device: reported with its "
		"reason");

	Responder responder4(LE_SMP_IO_DISPLAY_YES_NO, 0x0d);
	outcome = Pair(responder4, Options(LE_SMP_IO_DISPLAY_YES_NO, true), NULL);
	Check(outcome.status == B_CANCELED && !responder4.completed,
		"Numeric Comparison without an agent fails");
}


static void
TestSecurePasskey()
{
	TestAgent agent(true);
	Responder responder(LE_SMP_IO_KEYBOARD_ONLY, 0x0d);
	Outcome outcome = Pair(responder, Options(LE_SMP_IO_DISPLAY_YES_NO, true),
		&agent);
	Check(SameKey(outcome, responder) && outcome.result.secureConnections
		&& outcome.result.method == LE_SMP_PASSKEY_INITIATOR_DISPLAYS
		&& outcome.result.authenticated,
		"SC Passkey Entry, shown here (%06lld): 20 rounds, keys agree",
		(long long)agent.shown.load());

	TestAgent agent2(true);
	Responder responder2(LE_SMP_IO_KEYBOARD_ONLY, 0x0d, WRONG_PASSKEY);
	outcome = Pair(responder2, Options(LE_SMP_IO_DISPLAY_YES_NO, true),
		&agent2);
	Check(outcome.status == B_PERMISSION_DENIED && outcome.result.remoteReason
		&& outcome.result.reason == LE_SMP_REASON_CONFIRM_VALUE_FAILED,
		"SC Passkey Entry, wrong passkey typed: the device's Confirm Value "
		"Failed");

	TestAgent typing(true);
	typing.enterFrom = &typing.shown;
		// any non-NULL: Pair() points it at the responder's number
	Responder responder3(LE_SMP_IO_DISPLAY_ONLY, 0x0d);
	outcome = Pair(responder3, Options(LE_SMP_IO_KEYBOARD_DISPLAY, true),
		&typing);
	Check(SameKey(outcome, responder3)
		&& outcome.result.method == LE_SMP_PASSKEY_RESPONDER_DISPLAYS,
		"SC Passkey Entry, shown on the device and typed here: keys agree");

	TestAgent cannotType(true);
	Responder responder4(LE_SMP_IO_DISPLAY_ONLY, 0x0d);
	outcome = Pair(responder4, Options(LE_SMP_IO_KEYBOARD_DISPLAY, true),
		&cannotType);
	Check(outcome.status == B_NOT_SUPPORTED
		&& outcome.result.reason == LE_SMP_REASON_PASSKEY_ENTRY_FAILED
		&& responder4.failureReceived == LE_SMP_REASON_PASSKEY_ENTRY_FAILED,
		"a passkey to type here without a way to type it: Passkey Entry "
		"Failed");
}


static void
TestSecureFaults()
{
	struct {
		Fault fault;
		status_t status;
		uint8 reason;
		const char* name;
	} cases[] = {
		{ BAD_CONFIRM, B_BAD_DATA, LE_SMP_REASON_CONFIRM_VALUE_FAILED,
			"SC confirm value that does not match: Confirm Value Failed" },
		{ INVALID_KEY, B_BAD_DATA, LE_SMP_REASON_DHKEY_CHECK_FAILED,
			"SC public key off the curve: refused" },
		{ DEBUG_KEY, B_BAD_DATA, LE_SMP_REASON_AUTHENTICATION_REQUIREMENTS,
			"SC debug key: refused" },
		{ BAD_DHKEY_CHECK, B_BAD_DATA, LE_SMP_REASON_DHKEY_CHECK_FAILED,
			"SC DHKey check that does not match: DHKey Check Failed" },
		{ REFUSE, B_PERMISSION_DENIED, LE_SMP_REASON_PAIRING_NOT_SUPPORTED,
			"device refuses pairing: its reason is reported" }
	};
	for (const auto& test : cases) {
		Responder responder(LE_SMP_IO_NO_INPUT_NO_OUTPUT, 0x09, test.fault);
		Outcome outcome = Pair(responder,
			Options(LE_SMP_IO_NO_INPUT_NO_OUTPUT, false), NULL);
		bool remote = test.fault == REFUSE;
		bool okay = outcome.status == test.status
			&& outcome.result.reason == test.reason
			&& outcome.result.remoteReason == remote
			&& (remote || responder.failureReceived == test.reason);
		uint8 zero[16] = {};
		Check(okay && memcmp(outcome.result.key, zero, 16) == 0, "%s",
			test.name);
	}
}


static void
TestLegacy()
{
	Responder responder(LE_SMP_IO_NO_INPUT_NO_OUTPUT, 0x01);
	Outcome outcome = Pair(responder,
		Options(LE_SMP_IO_NO_INPUT_NO_OUTPUT, false), NULL);
	Check(SameKey(outcome, responder) && !outcome.result.secureConnections
		&& outcome.result.method == LE_SMP_JUST_WORKS
		&& outcome.result.responderKeys == 0x03,
		"legacy Just Works: both sides have the same STK");

	Responder scResponder(LE_SMP_IO_NO_INPUT_NO_OUTPUT, 0x09);
	outcome = Pair(scResponder,
		Options(LE_SMP_IO_NO_INPUT_NO_OUTPUT, false, false), NULL);
	Check(SameKey(outcome, scResponder) && !outcome.result.secureConnections,
		"Secure Connections not offered: legacy with an SC device");

	TestAgent agent(true);
	Responder responder2(LE_SMP_IO_KEYBOARD_ONLY, 0x05);
	outcome = Pair(responder2, Options(LE_SMP_IO_DISPLAY_YES_NO, true),
		&agent);
	Check(SameKey(outcome, responder2)
		&& outcome.result.method == LE_SMP_PASSKEY_INITIATOR_DISPLAYS
		&& outcome.result.authenticated,
		"legacy Passkey Entry, shown here: keys agree");

	TestAgent agent2(true);
	Responder responder3(LE_SMP_IO_KEYBOARD_ONLY, 0x05, WRONG_PASSKEY);
	outcome = Pair(responder3, Options(LE_SMP_IO_DISPLAY_YES_NO, true),
		&agent2);
	Check(outcome.status == B_PERMISSION_DENIED
		&& outcome.result.reason == LE_SMP_REASON_CONFIRM_VALUE_FAILED,
		"legacy Passkey Entry, wrong passkey: the device's Confirm Value "
		"Failed");

	Responder responder4(LE_SMP_IO_NO_INPUT_NO_OUTPUT, 0x01, BAD_CONFIRM);
	outcome = Pair(responder4, Options(LE_SMP_IO_NO_INPUT_NO_OUTPUT, false),
		NULL);
	Check(outcome.status == B_BAD_DATA
		&& responder4.failureReceived == LE_SMP_REASON_CONFIRM_VALUE_FAILED,
		"legacy confirm value that does not match: Confirm Value Failed");

	TestAgent declining(false);
	Responder responder5(LE_SMP_IO_NO_INPUT_NO_OUTPUT, 0x01);
	outcome = Pair(responder5, Options(LE_SMP_IO_DISPLAY_YES_NO, true),
		&declining);
	Check(outcome.status == B_CANCELED && declining.confirms == 1
		&& !responder5.completed, "legacy Just Works declined: B_CANCELED");
}


static void
TestKeySize()
{
	Responder responder(LE_SMP_IO_NO_INPUT_NO_OUTPUT, 0x09);
	responder.keySize = 7;
	Outcome outcome = Pair(responder,
		Options(LE_SMP_IO_NO_INPUT_NO_OUTPUT, false), NULL);
	bool masked = true;
	for (int i = 7; i < 16; i++)
		masked &= outcome.result.key[i] == 0;
	Check(SameKey(outcome, responder) && outcome.result.keySize == 7 && masked,
		"a 7-octet key is masked the same on both sides");
}


int
main()
{
	TestSecureJustWorks();
	TestNumericComparison();
	TestSecurePasskey();
	TestSecureFaults();
	TestLegacy();
	TestKeySize();
	printf("%d checks, %d failed\n", sChecks, sFailures);
	return sFailures == 0 ? 0 : 1;
}
