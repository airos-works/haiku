/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Sending and receiving Security Manager Protocol PDUs on a connected SMP
 * fixed-channel socket. Internal to the Bluetooth kit.
 */
#ifndef _LE_SMP_CHANNEL_H_
#define _LE_SMP_CHANNEL_H_

#include <SupportDefs.h>
#include <stddef.h>


namespace Bluetooth {

class LESMPUserAgent;

enum {
	LE_SMP_PAIRING_REQUEST			= 0x01,
	LE_SMP_PAIRING_RESPONSE			= 0x02,
	LE_SMP_PAIRING_CONFIRM			= 0x03,
	LE_SMP_PAIRING_RANDOM			= 0x04,
	LE_SMP_PAIRING_FAILED			= 0x05,
	LE_SMP_ENCRYPTION_INFORMATION	= 0x06,
	LE_SMP_CENTRAL_IDENTIFICATION	= 0x07,
	LE_SMP_IDENTITY_INFORMATION		= 0x08,
	LE_SMP_IDENTITY_ADDRESS			= 0x09,
	LE_SMP_SIGNING_INFORMATION		= 0x0a,
	LE_SMP_SECURITY_REQUEST			= 0x0b,
	LE_SMP_PAIRING_PUBLIC_KEY		= 0x0c,
	LE_SMP_PAIRING_DHKEY_CHECK		= 0x0d,
	LE_SMP_KEYPRESS_NOTIFICATION	= 0x0e
};

// The SMP transaction timeout (Core Vol 3 Part H 3.4).
static const bigtime_t kSMPTimeout = 30000000;

const char*	SMPCodeName(uint8 code);
const char*	SMPReasonName(uint8 reason);

void		SMPClearSecret(void* data, size_t length);
bool		SMPEqualSecret(const uint8* first, const uint8* second,
				size_t length);
void		SMPLogFeatures(const char* direction, const uint8* features);

status_t	SMPSend(int descriptor, const uint8* packet, size_t length);
void		SMPSendFailure(int descriptor, uint8 reason);

// Waits until deadline (system_time() base) for one PDU, skipping Security
// Requests and Keypress Notifications. With an agent, gives up with
// B_CANCELED as soon as its Cancelled() says so.
status_t	SMPReceive(int descriptor, uint8* packet, size_t capacity,
				size_t& length, bigtime_t deadline,
				LESMPUserAgent* agent = NULL);

} // namespace Bluetooth

#endif
