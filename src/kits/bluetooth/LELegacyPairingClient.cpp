/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */

#include <LELegacyPairingClient.h>

#include "LESMPChannel.h"

#include <LELog.h>
#include <LESMPInitiator.h>

#include <OS.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <Errors.h>


namespace Bluetooth {

#define LOG(level, format, args...) LELog(level, "smp", format, ##args)


static void
SetDetail(char* detail, size_t size, const char* format, ...)
{
	if (detail == NULL || size == 0)
		return;
	va_list args;
	va_start(args, format);
	vsnprintf(detail, size, format, args);
	va_end(args);
	LOG(LE_LOG_ERROR, "%s", detail);
}


status_t
LELegacyPairJustWorks(int descriptor, const uint8 initiatorAddress[6],
	uint8 initiatorAddressType, const uint8 responderAddress[6],
	uint8 responderAddressType, uint8 shortTermKey[16],
	uint8* responderKeyDistribution, uint8* negotiatedKeySize,
	const BMessenger* progress, char* detail, size_t detailSize)
{
	if (shortTermKey == NULL)
		return B_BAD_VALUE;
	LESMPOptions options;
	options.ioCapability = LE_SMP_IO_NO_INPUT_NO_OUTPUT;
	options.mitm = false;
	options.secureConnections = false;
	LESMPResult result;
	status_t status = LESMPPairAsInitiator(descriptor, initiatorAddress,
		initiatorAddressType, responderAddress, responderAddressType, options,
		NULL, result, progress, detail, detailSize);
	memcpy(shortTermKey, result.key, 16);
	if (responderKeyDistribution != NULL)
		*responderKeyDistribution = status == B_OK ? result.responderKeys : 0;
	if (negotiatedKeySize != NULL)
		*negotiatedKeySize = status == B_OK ? result.keySize : 0;
	SMPClearSecret(&result, sizeof(result));
	return status;
}


status_t
LELegacyReceiveResponderKeys(int descriptor, uint8 expectedDistribution,
	uint8 negotiatedKeySize, LELegacyBondKey& output, char* detail,
	size_t detailSize)
{
	if (detail != NULL && detailSize > 0)
		detail[0] = '\0';
	SMPClearSecret(&output, sizeof(output));
	if (descriptor < 0
		|| (expectedDistribution & ~0x03) != 0
		|| negotiatedKeySize < 7 || negotiatedKeySize > 16)
		return B_BAD_VALUE;
	output.keySize = negotiatedKeySize;
	if (expectedDistribution == 0) {
		LOG(LE_LOG_INFO, "device distributes no keys; nothing to store");
		return B_OK;
	}

	bigtime_t deadline = system_time() + kSMPTimeout;
	uint8 packet[32];
	size_t length = 0;
	status_t status = B_OK;
	bool haveEncryption = false, haveCentral = false;
	bool haveIdentity = false, haveAddress = false;
	const bool wantEncryption = (expectedDistribution & 0x01) != 0;
	const bool wantIdentity = (expectedDistribution & 0x02) != 0;
	LOG(LE_LOG_INFO, "waiting for the device's keys (%s%s)",
		wantEncryption ? "LTK " : "", wantIdentity ? "IRK" : "");

	// Core Vol 3 Part H 3.6.1 fixes the order, but accept any order and
	// ignore keys that were not requested rather than fail the bond.
	while ((wantEncryption && !(haveEncryption && haveCentral))
		|| (wantIdentity && !(haveIdentity && haveAddress))) {
		status = SMPReceive(descriptor, packet, sizeof(packet), length,
			deadline);
		if (status != B_OK) {
			SetDetail(detail, detailSize, status == B_TIMED_OUT
				? "The device did not send its keys within 30 seconds."
				: "Receiving the device's keys failed: %s.", strerror(status));
			goto done;
		}
		switch (packet[0]) {
			case LE_SMP_PAIRING_FAILED:
				SetDetail(detail, detailSize, "The device aborted key "
					"distribution: %s (reason %#x).",
					length >= 2 ? SMPReasonName(packet[1]) : "?",
					length >= 2 ? packet[1] : 0);
				status = B_PERMISSION_DENIED;
				goto done;
			case LE_SMP_ENCRYPTION_INFORMATION:
				if (length != 17)
					goto invalid;
				memcpy(output.longTermKey, packet + 1, 16);
				haveEncryption = true;
				break;
			case LE_SMP_CENTRAL_IDENTIFICATION:
				if (length != 11)
					goto invalid;
				output.encryptedDiversifier = packet[1]
					| ((uint16)packet[2] << 8);
				memcpy(output.randomNumber, packet + 3, 8);
				haveCentral = true;
				break;
			case LE_SMP_IDENTITY_INFORMATION:
				if (length != 17)
					goto invalid;
				memcpy(output.identityResolvingKey, packet + 1, 16);
				haveIdentity = true;
				break;
			case LE_SMP_IDENTITY_ADDRESS:
			{
				if (length != 8 || packet[1] > 1)
					goto invalid;
				output.identityAddressType = packet[1];
				memcpy(output.identityAddress, packet + 2, 6);
				haveAddress = true;
				char text[18];
				LOG(LE_LOG_INFO, "device identity address %s (type %u)",
					LEAddressString(output.identityAddress, text), packet[1]);
				break;
			}
			case LE_SMP_SIGNING_INFORMATION:
				LOG(LE_LOG_INFO, "ignoring unrequested signing key");
				break;
			default:
				LOG(LE_LOG_ERROR, "unexpected %s during key distribution",
					SMPCodeName(packet[0]));
				goto invalid;
		}
	}
	if (haveEncryption && haveCentral) {
		for (uint8 i = negotiatedKeySize; i < 16; i++)
			output.longTermKey[i] = 0;
		output.hasLongTermKey = true;
	}
	output.hasIdentity = haveIdentity && haveAddress;
	LOG(LE_LOG_INFO, "keys received: LTK %s, identity %s",
		output.hasLongTermKey ? "yes" : "no", output.hasIdentity ? "yes" : "no");
	status = B_OK;
	goto done;

invalid:
	SetDetail(detail, detailSize, "The device sent a malformed %s (%zu "
		"bytes).", SMPCodeName(packet[0]), length);
	SMPSendFailure(descriptor, LE_SMP_REASON_INVALID_PARAMETERS);
	status = B_BAD_DATA;
done:
	SMPClearSecret(packet, sizeof(packet));
	if (status != B_OK)
		SMPClearSecret(&output, sizeof(output));
	return status;
}

} // namespace Bluetooth
