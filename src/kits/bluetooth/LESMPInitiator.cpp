/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * The initiator's side of LE pairing (Bluetooth Core Vol 3 Part H 2.3-2.3.5):
 * Secure Connections with Just Works, Numeric Comparison or Passkey Entry,
 * and legacy pairing with Just Works or Passkey Entry. Out of band data is
 * not supported.
 */

#include <LESMPInitiator.h>

#include "LESMPChannel.h"

#include <LELegacyPairingCrypto.h>
#include <LELog.h>
#include <LEPairingSession.h>

#include <Message.h>
#include <Messenger.h>
#include <OS.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>


namespace Bluetooth {

#define LOG(level, format, args...) LELog(level, "smp", format, ##args)


const char*
LESMPMethodName(LESMPMethod method)
{
	switch (method) {
		case LE_SMP_JUST_WORKS: return "Just Works";
		case LE_SMP_NUMERIC_COMPARISON: return "Numeric Comparison";
		case LE_SMP_PASSKEY_INITIATOR_DISPLAYS:
			return "Passkey Entry (this computer shows the passkey)";
		case LE_SMP_PASSKEY_RESPONDER_DISPLAYS:
			return "Passkey Entry (the device shows the passkey)";
		case LE_SMP_PASSKEY_BOTH_INPUT:
			return "Passkey Entry (typed on both)";
	}
	return "?";
}


namespace {

class Pairing {
public:
	Pairing(int socket, const uint8 initiatorAddress[6],
		uint8 initiatorAddressType, const uint8 responderAddress[6],
		uint8 responderAddressType, const LESMPOptions& options,
		LESMPUserAgent* agent, LESMPResult& result,
		const BMessenger* progress, char* detail, size_t detailSize);
	~Pairing();

	status_t Run();

private:
	status_t _ExchangeFeatures();
	status_t _Legacy();
	status_t _SecureConnections();
	status_t _SecureJustWorksOrNumeric(uint8 na[16], uint8 nb[16]);
	status_t _SecurePasskey(uint32 passkey, uint8 na[16], uint8 nb[16]);
	status_t _GetPasskey(uint32& passkey);

	status_t _Send(const uint8* packet, size_t length, const char* what);
	// Receives the PDU with the code and length expected; anything else,
	// a Pairing Failed included, ends the pairing.
	status_t _Expect(uint8 code, size_t length, uint8* packet,
		const char* what);
	status_t _Fail(uint8 reason, status_t status, const char* format, ...)
		__attribute__((format(printf, 4, 5)));
	void _Stage(LEPairingStage stage);
	void _SetDetail(const char* format, ...)
		__attribute__((format(printf, 2, 3)));
	bool _Random(void* buffer, size_t length);

	int fSocket;
	uint8 fInitiator[6];
	uint8 fInitiatorType;
	uint8 fResponder[6];
	uint8 fResponderType;
	const LESMPOptions& fOptions;
	LESMPUserAgent* fAgent;
	LESMPResult& fResult;
	const BMessenger* fProgress;
	char* fDetail;
	size_t fDetailSize;

	bigtime_t fDeadline;
		// 30 s after this side last sent something (Core Vol 3 Part H 3.4)
	uint8 fRequest[7];
	uint8 fResponse[7];
	uint8 fPrivateKey[32];
	uint8 fPublicKey[64];
	uint8 fPeerKey[64];
	bool fPasskeyShown;
};


Pairing::Pairing(int socket, const uint8 initiatorAddress[6],
	uint8 initiatorAddressType, const uint8 responderAddress[6],
	uint8 responderAddressType, const LESMPOptions& options,
	LESMPUserAgent* agent, LESMPResult& result, const BMessenger* progress,
	char* detail, size_t detailSize)
	:
	fSocket(socket),
	fInitiatorType(initiatorAddressType),
	fResponderType(responderAddressType),
	fOptions(options),
	fAgent(agent),
	fResult(result),
	fProgress(progress),
	fDetail(detail),
	fDetailSize(detailSize),
	fDeadline(system_time() + kSMPTimeout),
	fPasskeyShown(false)
{
	memcpy(fInitiator, initiatorAddress, 6);
	memcpy(fResponder, responderAddress, 6);
	memset(fRequest, 0, sizeof(fRequest));
	memset(fResponse, 0, sizeof(fResponse));
	memset(fPrivateKey, 0, sizeof(fPrivateKey));
	memset(fPublicKey, 0, sizeof(fPublicKey));
	memset(fPeerKey, 0, sizeof(fPeerKey));
}


Pairing::~Pairing()
{
	SMPClearSecret(fPrivateKey, sizeof(fPrivateKey));
}


void
Pairing::_SetDetail(const char* format, ...)
{
	if (fDetail == NULL || fDetailSize == 0)
		return;
	va_list args;
	va_start(args, format);
	vsnprintf(fDetail, fDetailSize, format, args);
	va_end(args);
	LOG(LE_LOG_ERROR, "%s", fDetail);
}


void
Pairing::_Stage(LEPairingStage stage)
{
	if (fProgress != NULL && fProgress->IsValid()) {
		BMessage message(LE_PAIRING_PROGRESS_MESSAGE);
		message.AddInt32("stage", stage);
		fProgress->SendMessage(&message);
	}
}


bool
Pairing::_Random(void* buffer, size_t length)
{
	if (getentropy(buffer, length) == 0)
		return true;
	_SetDetail("No random numbers available.");
	return false;
}


status_t
Pairing::_Send(const uint8* packet, size_t length, const char* what)
{
	status_t status = SMPSend(fSocket, packet, length);
	if (status != B_OK)
		_SetDetail("Could not send %s (%s).", what, strerror(status));
	else
		fDeadline = system_time() + kSMPTimeout;
	return status;
}


status_t
Pairing::_Fail(uint8 reason, status_t status, const char* format, ...)
{
	if (fDetail != NULL && fDetailSize > 0) {
		va_list args;
		va_start(args, format);
		vsnprintf(fDetail, fDetailSize, format, args);
		va_end(args);
		LOG(LE_LOG_ERROR, "%s", fDetail);
	}
	SMPSendFailure(fSocket, reason);
	fResult.reason = reason;
	fResult.remoteReason = false;
	return status;
}


status_t
Pairing::_Expect(uint8 code, size_t length, uint8* packet, const char* what)
{
	uint8 buffer[80];
	size_t received = 0;
	status_t status = SMPReceive(fSocket, buffer, sizeof(buffer), received,
		fDeadline, fAgent);
	if (status == B_CANCELED) {
		return _Fail(fPasskeyShown ? LE_SMP_REASON_PASSKEY_ENTRY_FAILED
				: LE_SMP_REASON_UNSPECIFIED, B_CANCELED,
			"Pairing was cancelled.");
	}
	if (status == B_TIMED_OUT) {
		_SetDetail("The device did not send %s within %d seconds.", what,
			(int)(kSMPTimeout / 1000000));
		return status;
	}
	if (status != B_OK) {
		_SetDetail("Waiting for %s failed: %s.", what, strerror(status));
		return status;
	}
	if (received >= 2 && buffer[0] == LE_SMP_PAIRING_FAILED) {
		fResult.reason = buffer[1];
		fResult.remoteReason = true;
		_SetDetail("The device ended pairing instead of sending %s: %s "
			"(reason %#x).", what, SMPReasonName(buffer[1]), buffer[1]);
		return B_PERMISSION_DENIED;
	}
	if (buffer[0] != code || received != length) {
		return _Fail(received > 0 && buffer[0] == code
				? LE_SMP_REASON_INVALID_PARAMETERS
				: LE_SMP_REASON_UNSPECIFIED, B_BAD_DATA,
			"Unexpected %s (%zu bytes) instead of %s.",
			SMPCodeName(buffer[0]), received, what);
	}
	memcpy(packet, buffer, length);
	SMPClearSecret(buffer, sizeof(buffer));
	return B_OK;
}


status_t
Pairing::_ExchangeFeatures()
{
	uint8 authentication = 0x01;	// bonding
	if (fOptions.mitm)
		authentication |= 0x04;
	if (fOptions.secureConnections)
		authentication |= 0x08;
	fRequest[0] = LE_SMP_PAIRING_REQUEST;
	fRequest[1] = fOptions.ioCapability;
	fRequest[2] = 0;	// no out of band data
	fRequest[3] = authentication;
	fRequest[4] = 16;
	fRequest[5] = 0;	// this side distributes no keys
	fRequest[6] = fOptions.responderKeys
		& (LE_SMP_KEY_ENCRYPTION | LE_SMP_KEY_IDENTITY);

	char initiatorText[18], responderText[18];
	LOG(LE_LOG_INFO, "pairing: initiator %s (type %u), responder %s (type %u)",
		LEAddressString(fInitiator, initiatorText), fInitiatorType,
		LEAddressString(fResponder, responderText), fResponderType);
	SMPLogFeatures("->", fRequest);
	_Stage(LE_PAIRING_SMP_REQUEST);
	status_t status = _Send(fRequest, sizeof(fRequest), "the pairing request");
	if (status != B_OK)
		return status;
	_Stage(LE_PAIRING_SMP_RESPONSE);
	status = _Expect(LE_SMP_PAIRING_RESPONSE, sizeof(fResponse), fResponse,
		"a pairing response");
	if (status == B_TIMED_OUT) {
		_SetDetail("The device did not answer the pairing request within 30 "
			"seconds. It may not be in pairing mode.");
	}
	if (status == B_PERMISSION_DENIED) {
		_SetDetail("The device refused pairing: %s (reason %#x).",
			SMPReasonName(fResult.reason), fResult.reason);
	}
	if (status != B_OK)
		return status;
	SMPLogFeatures("<-", fResponse);

	if (fResponse[1] > LE_SMP_IO_KEYBOARD_DISPLAY || fResponse[4] < 7
		|| fResponse[4] > 16 || (fResponse[3] & 0x03) > 1
		|| (fResponse[5] & ~fRequest[5]) != 0
		|| (fResponse[6] & ~fRequest[6]) != 0) {
		return _Fail(LE_SMP_REASON_INVALID_PARAMETERS, B_BAD_DATA,
			"The pairing response has invalid parameters (IO %u, AuthReq "
			"%#x, key size %u, keys %#x/%#x).", fResponse[1], fResponse[3],
			fResponse[4], fResponse[5], fResponse[6]);
	}
	if (fResponse[2] != 0) {
		LOG(LE_LOG_INFO, "the device has out of band data; this side has "
			"none, so it is not used");
	}

	fResult.secureConnections = fOptions.secureConnections
		&& (fResponse[3] & 0x08) != 0;
	fResult.bonding = (fResponse[3] & 0x03) == 1;
	bool mitm = fOptions.mitm || (fResponse[3] & 0x04) != 0;
	fResult.method = LESMPSelectMethod(fResult.secureConnections, mitm,
		fRequest[1], fResponse[1]);
	fResult.authenticated = fResult.method != LE_SMP_JUST_WORKS;
	fResult.keySize = fResponse[4];
	fResult.responderKeys = fResult.secureConnections
		? fResponse[6] & ~LE_SMP_KEY_ENCRYPTION : fResponse[6];
	LOG(LE_LOG_INFO, "%s pairing, %s%s", fResult.secureConnections
			? "LE Secure Connections" : "legacy",
		LESMPMethodName(fResult.method),
		mitm && !fResult.authenticated
			? " (MITM protection asked for, but not possible with these IO "
				"capabilities)" : "");
	if (!fResult.secureConnections && (fResponse[3] & 0x08) != 0) {
		LOG(LE_LOG_INFO, "the device supports Secure Connections, but legacy "
			"pairing was asked for");
	}
	return B_OK;
}


status_t
Pairing::_GetPasskey(uint32& passkey)
{
	switch (fResult.method) {
		case LE_SMP_PASSKEY_INITIATOR_DISPLAYS:
		{
			if (fAgent == NULL) {
				return _Fail(LE_SMP_REASON_PASSKEY_ENTRY_FAILED,
					B_NOT_SUPPORTED, "The device wants a passkey typed on "
					"it, and nothing here can show one.");
			}
			uint32 random;
			if (!_Random(&random, sizeof(random))) {
				return _Fail(LE_SMP_REASON_UNSPECIFIED, B_ERROR,
					"No random numbers available.");
			}
			passkey = random % 1000000;
			LOG(LE_LOG_INFO, "showing the passkey for the device to take");
			fAgent->DisplayPasskey(passkey);
			fPasskeyShown = true;
			return B_OK;
		}
		case LE_SMP_PASSKEY_RESPONDER_DISPLAYS:
		case LE_SMP_PASSKEY_BOTH_INPUT:
			if (fAgent == NULL
				|| !fAgent->EnterPasskey(passkey, fDeadline)) {
				return _Fail(LE_SMP_REASON_PASSKEY_ENTRY_FAILED,
					fAgent != NULL && fAgent->Cancelled() ? B_CANCELED
						: B_NOT_SUPPORTED,
					"The device wants a passkey typed on this computer, "
					"which is not possible here.");
			}
			if (passkey > 999999) {
				return _Fail(LE_SMP_REASON_PASSKEY_ENTRY_FAILED, B_BAD_VALUE,
					"The passkey has more than six digits.");
			}
			return B_OK;
		default:
			return B_BAD_VALUE;
	}
}


status_t
Pairing::_Legacy()
{
	uint8 temporaryKey[16] = {};
	uint8 initiatorRandom[16] = {};
	uint8 responderRandom[16] = {};
	uint8 expected[16] = {};
	uint8 packet[17] = {};
	uint8 responderConfirm[17] = {};
	status_t status = B_OK;

	if (fResult.method == LE_SMP_JUST_WORKS) {
		if (fAgent != NULL && !fAgent->Confirm(LE_SMP_JUST_WORKS, 0,
				fDeadline - 1000000)) {
			status = _Fail(LE_SMP_REASON_UNSPECIFIED, B_CANCELED,
				"Pairing was declined.");
			goto done;
		}
	} else {
		uint32 passkey = 0;
		status = _GetPasskey(passkey);
		if (status != B_OK)
			goto done;
		temporaryKey[0] = passkey;
		temporaryKey[1] = passkey >> 8;
		temporaryKey[2] = passkey >> 16;
		temporaryKey[3] = passkey >> 24;
	}

	if (!_Random(initiatorRandom, sizeof(initiatorRandom))) {
		status = _Fail(LE_SMP_REASON_UNSPECIFIED, B_ERROR,
			"No random numbers available.");
		goto done;
	}
	packet[0] = LE_SMP_PAIRING_CONFIRM;
	if (!LELegacyConfirm(temporaryKey, initiatorRandom, fRequest, fResponse,
			fInitiatorType, fInitiator, fResponderType, fResponder,
			packet + 1)) {
		status = _Fail(LE_SMP_REASON_UNSPECIFIED, B_ERROR,
			"Could not compute the confirm value.");
		goto done;
	}
	_Stage(LE_PAIRING_SMP_CONFIRM);
	status = _Send(packet, sizeof(packet), "the pairing confirm");
	if (status != B_OK)
		goto done;
	status = _Expect(LE_SMP_PAIRING_CONFIRM, sizeof(responderConfirm),
		responderConfirm, "its confirm value");
	if (status != B_OK)
		goto done;

	packet[0] = LE_SMP_PAIRING_RANDOM;
	memcpy(packet + 1, initiatorRandom, 16);
	_Stage(LE_PAIRING_SMP_RANDOM);
	status = _Send(packet, sizeof(packet), "the pairing random");
	if (status != B_OK)
		goto done;
	status = _Expect(LE_SMP_PAIRING_RANDOM, sizeof(packet), packet,
		"its random value");
	if (status == B_PERMISSION_DENIED) {
		_SetDetail("The device rejected this computer's confirm value: %s "
			"(reason %#x).%s", SMPReasonName(fResult.reason), fResult.reason,
			fResult.method == LE_SMP_JUST_WORKS ? " The address or pairing "
				"features may not match what the device used."
				: " The passkey was probably typed wrong.");
	}
	if (status != B_OK)
		goto done;
	memcpy(responderRandom, packet + 1, 16);

	if (!LELegacyConfirm(temporaryKey, responderRandom, fRequest, fResponse,
			fInitiatorType, fInitiator, fResponderType, fResponder,
			expected)) {
		status = _Fail(LE_SMP_REASON_UNSPECIFIED, B_ERROR,
			"Could not verify the device's confirm value.");
		goto done;
	}
	if (!SMPEqualSecret(expected, responderConfirm + 1, 16)) {
		status = _Fail(LE_SMP_REASON_CONFIRM_VALUE_FAILED, B_BAD_DATA,
			"The device's confirm value does not match its random value.");
		goto done;
	}
	if (!LELegacyShortTermKey(temporaryKey, responderRandom, initiatorRandom,
			fResult.key)) {
		status = _Fail(LE_SMP_REASON_UNSPECIFIED, B_ERROR,
			"Could not derive the short-term key.");
		goto done;
	}
	LOG(LE_LOG_INFO, "confirm values verified; short-term key ready");

done:
	SMPClearSecret(temporaryKey, sizeof(temporaryKey));
	SMPClearSecret(initiatorRandom, sizeof(initiatorRandom));
	SMPClearSecret(responderRandom, sizeof(responderRandom));
	SMPClearSecret(expected, sizeof(expected));
	SMPClearSecret(packet, sizeof(packet));
	return status;
}


status_t
Pairing::_SecureJustWorksOrNumeric(uint8 na[16], uint8 nb[16])
{
	// Core Vol 3 Part H 2.3.5.6.2: the responder commits to Nb first.
	uint8 confirm[17], packet[17], expected[16];
	status_t status = _Expect(LE_SMP_PAIRING_CONFIRM, sizeof(confirm),
		confirm, "its confirm value");
	if (status != B_OK)
		return status;
	if (!_Random(na, 16)) {
		return _Fail(LE_SMP_REASON_UNSPECIFIED, B_ERROR,
			"No random numbers available.");
	}
	packet[0] = LE_SMP_PAIRING_RANDOM;
	memcpy(packet + 1, na, 16);
	_Stage(LE_PAIRING_SMP_RANDOM);
	status = _Send(packet, sizeof(packet), "the pairing random");
	if (status != B_OK)
		return status;
	status = _Expect(LE_SMP_PAIRING_RANDOM, sizeof(packet), packet,
		"its random value");
	if (status != B_OK)
		return status;
	memcpy(nb, packet + 1, 16);

	// Cb = f4(PKbx, PKax, Nb, 0)
	if (!LESCConfirm(fPeerKey, fPublicKey, nb, 0, expected)) {
		return _Fail(LE_SMP_REASON_UNSPECIFIED, B_ERROR,
			"Could not compute the confirm value.");
	}
	if (!SMPEqualSecret(expected, confirm + 1, 16)) {
		return _Fail(LE_SMP_REASON_CONFIRM_VALUE_FAILED, B_BAD_DATA,
			"The device's confirm value does not match its random value.");
	}

	if (fResult.method == LE_SMP_NUMERIC_COMPARISON) {
		uint32 value;
		if (!LESCNumericValue(fPublicKey, fPeerKey, na, nb, &value)) {
			return _Fail(LE_SMP_REASON_UNSPECIFIED, B_ERROR,
				"Could not compute the comparison value.");
		}
		value %= 1000000;
		LOG(LE_LOG_INFO, "asking the user to compare the numbers");
		if (fAgent == NULL
			|| !fAgent->Confirm(LE_SMP_NUMERIC_COMPARISON, value,
				fDeadline - 1000000)) {
			return _Fail(LE_SMP_REASON_NUMERIC_COMPARISON_FAILED, B_CANCELED,
				fAgent == NULL ? "Nothing here can show the number to compare."
					: "The numbers were not confirmed.");
		}
	} else if (fAgent != NULL
		&& !fAgent->Confirm(LE_SMP_JUST_WORKS, 0, fDeadline - 1000000)) {
		return _Fail(LE_SMP_REASON_UNSPECIFIED, B_CANCELED,
			"Pairing was declined.");
	}
	return B_OK;
}


status_t
Pairing::_SecurePasskey(uint32 passkey, uint8 na[16], uint8 nb[16])
{
	// Core Vol 3 Part H 2.3.5.6.3: twenty rounds, one per passkey bit.
	uint8 packet[17], confirm[17], expected[16];
	for (int round = 0; round < 20; round++) {
		uint8 bit = 0x80 | ((passkey >> round) & 1);
		if (!_Random(na, 16)) {
			return _Fail(LE_SMP_REASON_UNSPECIFIED, B_ERROR,
				"No random numbers available.");
		}
		// Cai = f4(PKax, PKbx, Nai, rai)
		packet[0] = LE_SMP_PAIRING_CONFIRM;
		if (!LESCConfirm(fPublicKey, fPeerKey, na, bit, packet + 1)) {
			return _Fail(LE_SMP_REASON_UNSPECIFIED, B_ERROR,
				"Could not compute the confirm value.");
		}
		status_t status = _Send(packet, sizeof(packet), "the pairing confirm");
		if (status != B_OK)
			return status;
		// The first answer waits for the user to type the passkey on the
		// device.
		status = _Expect(LE_SMP_PAIRING_CONFIRM, sizeof(confirm), confirm,
			"its confirm value");
		if (status != B_OK)
			return status;
		packet[0] = LE_SMP_PAIRING_RANDOM;
		memcpy(packet + 1, na, 16);
		status = _Send(packet, sizeof(packet), "the pairing random");
		if (status != B_OK)
			return status;
		status = _Expect(LE_SMP_PAIRING_RANDOM, sizeof(packet), packet,
			"its random value");
		if (status == B_PERMISSION_DENIED) {
			_SetDetail("The device rejected this computer's confirm value "
				"(%s, reason %#x). The passkey was probably typed wrong.",
				SMPReasonName(fResult.reason), fResult.reason);
		}
		if (status != B_OK)
			return status;
		memcpy(nb, packet + 1, 16);
		// Cbi = f4(PKbx, PKax, Nbi, rbi)
		if (!LESCConfirm(fPeerKey, fPublicKey, nb, bit, expected)) {
			return _Fail(LE_SMP_REASON_UNSPECIFIED, B_ERROR,
				"Could not compute the confirm value.");
		}
		if (!SMPEqualSecret(expected, confirm + 1, 16)) {
			return _Fail(LE_SMP_REASON_CONFIRM_VALUE_FAILED, B_BAD_DATA,
				"The device's confirm value does not match (round %d); the "
				"passkey was probably typed wrong.", round + 1);
		}
		if (round == 0)
			_Stage(LE_PAIRING_SMP_RANDOM);
	}
	return B_OK;
}


status_t
Pairing::_SecureConnections()
{
	uint8 dhKey[32] = {};
	uint8 na[16] = {}, nb[16] = {}, r[16] = {};
	uint8 macKey[16] = {}, longTermKey[16] = {};
	uint8 packet[65] = {};
	uint8 expected[16] = {};
	uint32 passkey = 0;
	status_t status = B_OK;

	// A passkey this side shows goes up before the keys are exchanged, so
	// the user can type it while the rest happens.
	if (fResult.method != LE_SMP_JUST_WORKS
		&& fResult.method != LE_SMP_NUMERIC_COMPARISON) {
		status = _GetPasskey(passkey);
		if (status != B_OK)
			goto done;
	}

	if (!LEP256GenerateKey(fPublicKey, fPrivateKey)) {
		status = _Fail(LE_SMP_REASON_UNSPECIFIED, B_ERROR,
			"Could not generate a P-256 key pair.");
		goto done;
	}
	packet[0] = LE_SMP_PAIRING_PUBLIC_KEY;
	memcpy(packet + 1, fPublicKey, 64);
	_Stage(LE_PAIRING_SMP_CONFIRM);
	status = _Send(packet, sizeof(packet), "this computer's public key");
	if (status != B_OK)
		goto done;
	status = _Expect(LE_SMP_PAIRING_PUBLIC_KEY, sizeof(packet), packet,
		"its public key");
	if (status != B_OK)
		goto done;
	memcpy(fPeerKey, packet + 1, 64);
	if (memcmp(fPeerKey, fPublicKey, 64) == 0) {
		// A reflected key would let an attacker complete the checks.
		status = _Fail(LE_SMP_REASON_DHKEY_CHECK_FAILED, B_BAD_DATA,
			"The device sent this computer's own public key back.");
		goto done;
	}
	if (LEP256IsDebugKey(fPeerKey)) {
		status = _Fail(LE_SMP_REASON_AUTHENTICATION_REQUIREMENTS, B_BAD_DATA,
			"The device uses the Bluetooth debug key, which offers no "
			"security.");
		goto done;
	}
	if (!LEP256DHKey(fPeerKey, fPrivateKey, dhKey)) {
		status = _Fail(LE_SMP_REASON_DHKEY_CHECK_FAILED, B_BAD_DATA,
			"The device's public key is not a valid P-256 point.");
		goto done;
	}
	SMPClearSecret(fPrivateKey, sizeof(fPrivateKey));

	if (fResult.method == LE_SMP_JUST_WORKS
		|| fResult.method == LE_SMP_NUMERIC_COMPARISON) {
		status = _SecureJustWorksOrNumeric(na, nb);
	} else {
		status = _SecurePasskey(passkey, na, nb);
		r[0] = passkey;
		r[1] = passkey >> 8;
		r[2] = passkey >> 16;
		r[3] = passkey >> 24;
	}
	if (status != B_OK)
		goto done;

	// MacKey || LTK = f5(DHKey, Na, Nb, A, B)
	if (!LESCKeys(dhKey, na, nb, fInitiatorType, fInitiator, fResponderType,
			fResponder, macKey, longTermKey)) {
		status = _Fail(LE_SMP_REASON_UNSPECIFIED, B_ERROR,
			"Could not derive the keys.");
		goto done;
	}
	// Ea = f6(MacKey, Na, Nb, rb, IOcapA, A, B). The IO capability, OOB
	// flag and AuthReq octets of the request and response are IOcapA/B.
	packet[0] = LE_SMP_PAIRING_DHKEY_CHECK;
	if (!LESCCheck(macKey, na, nb, r, fRequest + 1, fInitiatorType,
			fInitiator, fResponderType, fResponder, packet + 1)) {
		status = _Fail(LE_SMP_REASON_UNSPECIFIED, B_ERROR,
			"Could not compute the DHKey check.");
		goto done;
	}
	_Stage(LE_PAIRING_SMP_RANDOM);
	status = _Send(packet, 17, "the DHKey check");
	if (status != B_OK)
		goto done;
	status = _Expect(LE_SMP_PAIRING_DHKEY_CHECK, 17, packet,
		"its DHKey check");
	if (status == B_PERMISSION_DENIED
		&& fResult.reason == LE_SMP_REASON_NUMERIC_COMPARISON_FAILED) {
		_SetDetail("The numbers were not confirmed on the device.");
	} else if (status == B_PERMISSION_DENIED
		&& fResult.reason == LE_SMP_REASON_DHKEY_CHECK_FAILED) {
		_SetDetail("The device could not verify this computer's DHKey "
			"check.");
	}
	if (status != B_OK)
		goto done;
	// Eb = f6(MacKey, Nb, Na, ra, IOcapB, B, A)
	if (!LESCCheck(macKey, nb, na, r, fResponse + 1, fResponderType,
			fResponder, fInitiatorType, fInitiator, expected)) {
		status = _Fail(LE_SMP_REASON_UNSPECIFIED, B_ERROR,
			"Could not compute the DHKey check.");
		goto done;
	}
	if (!SMPEqualSecret(expected, packet + 1, 16)) {
		status = _Fail(LE_SMP_REASON_DHKEY_CHECK_FAILED, B_BAD_DATA,
			"The device's DHKey check does not match.");
		goto done;
	}
	memcpy(fResult.key, longTermKey, 16);
	LOG(LE_LOG_INFO, "DHKey checks verified; long-term key ready");

done:
	SMPClearSecret(dhKey, sizeof(dhKey));
	SMPClearSecret(na, sizeof(na));
	SMPClearSecret(nb, sizeof(nb));
	SMPClearSecret(r, sizeof(r));
	SMPClearSecret(macKey, sizeof(macKey));
	SMPClearSecret(longTermKey, sizeof(longTermKey));
	SMPClearSecret(packet, sizeof(packet));
	SMPClearSecret(expected, sizeof(expected));
	SMPClearSecret(&passkey, sizeof(passkey));
	return status;
}


status_t
Pairing::Run()
{
	status_t status = _ExchangeFeatures();
	if (status == B_OK) {
		status = fResult.secureConnections ? _SecureConnections()
			: _Legacy();
	}
	if (status == B_OK) {
		for (uint8 i = fResult.keySize; i < 16; i++)
			fResult.key[i] = 0;
	} else
		SMPClearSecret(fResult.key, sizeof(fResult.key));
	if (fAgent != NULL)
		fAgent->Finished(status);
	return status;
}

} // unnamed namespace


status_t
LESMPPairAsInitiator(int socket, const uint8 initiatorAddress[6],
	uint8 initiatorAddressType, const uint8 responderAddress[6],
	uint8 responderAddressType, const LESMPOptions& options,
	LESMPUserAgent* agent, LESMPResult& result, const BMessenger* progress,
	char* detail, size_t detailSize)
{
	memset(&result, 0, sizeof(result));
	if (detail != NULL && detailSize > 0)
		detail[0] = '\0';
	if (socket < 0 || initiatorAddress == NULL || responderAddress == NULL
		|| initiatorAddressType > 1 || responderAddressType > 1
		|| options.ioCapability > LE_SMP_IO_KEYBOARD_DISPLAY)
		return B_BAD_VALUE;

	Pairing pairing(socket, initiatorAddress, initiatorAddressType,
		responderAddress, responderAddressType, options, agent, result,
		progress, detail, detailSize);
	return pairing.Run();
}

} // namespace Bluetooth
