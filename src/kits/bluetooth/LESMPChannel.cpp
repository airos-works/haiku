/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */

#include "LESMPChannel.h"

#include <LELog.h>
#include <LESMPInitiator.h>

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include <OS.h>


namespace Bluetooth {

#define LOG(level, format, args...) LELog(level, "smp", format, ##args)

// How often a wait for the peer looks at the user's Cancel button.
static const bigtime_t kCancelPollInterval = 200000;


const char*
SMPCodeName(uint8 code)
{
	static const char* kNames[] = { "?", "Pairing Request", "Pairing Response",
		"Pairing Confirm", "Pairing Random", "Pairing Failed",
		"Encryption Information", "Central Identification",
		"Identity Information", "Identity Address Information",
		"Signing Information", "Security Request", "Pairing Public Key",
		"Pairing DHKey Check", "Keypress Notification" };
	return code < B_COUNT_OF(kNames) ? kNames[code] : "unknown";
}


const char*
SMPReasonName(uint8 reason)
{
	static const char* kNames[] = { "?", "Passkey Entry Failed",
		"OOB Not Available", "Authentication Requirements",
		"Confirm Value Failed", "Pairing Not Supported",
		"Encryption Key Size", "Command Not Supported", "Unspecified Reason",
		"Repeated Attempts", "Invalid Parameters", "DHKey Check Failed",
		"Numeric Comparison Failed", "BR/EDR pairing in progress",
		"Cross-transport Key Derivation not allowed", "Key Rejected" };
	return reason < B_COUNT_OF(kNames) ? kNames[reason] : "unknown";
}


void
SMPClearSecret(void* data, size_t length)
{
	volatile uint8* bytes = (volatile uint8*)data;
	while (length-- > 0)
		*bytes++ = 0;
}


bool
SMPEqualSecret(const uint8* first, const uint8* second, size_t length)
{
	uint8 difference = 0;
	for (size_t i = 0; i < length; i++)
		difference |= first[i] ^ second[i];
	return difference == 0;
}


void
SMPLogFeatures(const char* direction, const uint8* features)
{
	LOG(LE_LOG_INFO, "%s %s: IO capability %u, OOB %u, AuthReq %#04x "
		"(bonding %u, MITM %u, SC %u, keypress %u, CT2 %u), max key %u, "
		"initiator keys %#x, responder keys %#x", direction,
		SMPCodeName(features[0]), features[1], features[2], features[3],
		features[3] & 0x03, (features[3] >> 2) & 1, (features[3] >> 3) & 1,
		(features[3] >> 4) & 1, (features[3] >> 5) & 1, features[4],
		features[5], features[6]);
}


status_t
SMPSend(int descriptor, const uint8* packet, size_t length)
{
	ssize_t sent;
	do {
		sent = send(descriptor, packet, length, 0);
	} while (sent < 0 && errno == EINTR);
	if (sent == (ssize_t)length) {
		LOG(LE_LOG_DEBUG, "-> %s (%zu bytes)", SMPCodeName(packet[0]), length);
		return B_OK;
	}
	status_t error = sent < 0 ? errno : B_IO_ERROR;
	LOG(LE_LOG_ERROR, "sending %s failed: %s", SMPCodeName(packet[0]),
		strerror(error));
	return error;
}


void
SMPSendFailure(int descriptor, uint8 reason)
{
	LOG(LE_LOG_ERROR, "-> Pairing Failed: reason %#x (%s)", reason,
		SMPReasonName(reason));
	const uint8 packet[2] = { LE_SMP_PAIRING_FAILED, reason };
	SMPSend(descriptor, packet, sizeof(packet));
}


status_t
SMPReceive(int descriptor, uint8* packet, size_t capacity, size_t& length,
	bigtime_t deadline, LESMPUserAgent* agent)
{
	for (;;) {
		if (agent != NULL && agent->Cancelled())
			return B_CANCELED;
		bigtime_t now = system_time();
		if (now >= deadline)
			return B_TIMED_OUT;
		bigtime_t wait = deadline - now;
		if (agent != NULL && wait > kCancelPollInterval)
			wait = kCancelPollInterval;
		pollfd readable = { descriptor, POLLIN, 0 };
		int ready = poll(&readable, 1, (int)((wait + 999) / 1000));
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready < 0)
			return errno;
		if (ready == 0)
			continue;
		ssize_t received = recv(descriptor, packet, capacity, 0);
		if (received < 0 && errno == EINTR)
			continue;
		if (received < 0) {
			status_t error = errno;
			LOG(LE_LOG_ERROR, "receive failed: %s (link lost?)",
				strerror(error));
			return error == ENOTCONN ? B_DEV_NOT_READY : error;
		}
		if (received == 0)
			return B_DEV_NOT_READY;
		length = (size_t)received;
		// A Security Request is the responder asking us to do what we are
		// already doing; it can cross our Pairing Request on the air.
		if (packet[0] == LE_SMP_SECURITY_REQUEST) {
			LOG(LE_LOG_INFO, "<- Security Request (AuthReq %#04x); ignored "
				"during pairing", length > 1 ? packet[1] : 0);
			continue;
		}
		if (packet[0] == LE_SMP_KEYPRESS_NOTIFICATION) {
			LOG(LE_LOG_DEBUG, "<- Keypress Notification %u",
				length > 1 ? packet[1] : 0);
			continue;
		}
		if (packet[0] == LE_SMP_PAIRING_FAILED && length >= 2) {
			LOG(LE_LOG_ERROR, "<- Pairing Failed: reason %#x (%s)", packet[1],
				SMPReasonName(packet[1]));
		} else {
			LOG(LE_LOG_DEBUG, "<- %s (%zu bytes)", SMPCodeName(packet[0]),
				length);
		}
		return B_OK;
	}
}

} // namespace Bluetooth
