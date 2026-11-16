/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Low Energy fixed-channel helpers: diagnostics switch, the LE signaling
 * channel, the ATT MTU exchange and a minimal ATT server so a peripheral's
 * GATT client requests are answered instead of timing out the ATT bearer.
 */

#include "l2cap_le.h"

#include <KernelExport.h>
#include <driver_settings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <NetBufferUtilities.h>
#include <bluetooth/HCI/btHCI_command.h>
#include <bluetooth/L2CAP/btL2CAP.h>
#include <lock.h>
#include <util/AutoLock.h>

#include <l2cap.h>
#include "l2cap_internal.h"


int32 gL2capLELogLevel = 0;

static const char* kSettingsPath
	= "/boot/home/config/settings/bluetooth/le_logging";

// LE signaling channel (Core Vol 3 Part A 4).
static const uint8 kCommandReject = 0x01;
static const uint8 kConnectionParameterUpdateRequest = 0x12;
static const uint8 kConnectionParameterUpdateResponse = 0x13;

// ATT opcodes (Core Vol 3 Part F 3.4).
static const uint8 kATTErrorResponse = 0x01;
static const uint8 kATTExchangeMTURequest = 0x02;
static const uint8 kATTExchangeMTUResponse = 0x03;
static const uint8 kATTFindInformationRequest = 0x04;
static const uint8 kATTFindByTypeValueRequest = 0x06;
static const uint8 kATTReadByTypeRequest = 0x08;
static const uint8 kATTReadRequest = 0x0a;
static const uint8 kATTReadBlobRequest = 0x0c;
static const uint8 kATTReadMultipleRequest = 0x0e;
static const uint8 kATTReadByGroupTypeRequest = 0x10;
static const uint8 kATTWriteRequest = 0x12;
static const uint8 kATTPrepareWriteRequest = 0x16;
static const uint8 kATTExecuteWriteRequest = 0x18;
static const uint8 kATTReadMultipleVariableRequest = 0x20;
static const uint8 kATTHandleValueIndication = 0x1d;
static const uint8 kATTHandleValueConfirmation = 0x1e;
static const uint8 kATTInvalidHandle = 0x01;
static const uint8 kATTRequestNotSupported = 0x06;
static const uint8 kATTAttributeNotFound = 0x0a;

static const uint16 kATTDefaultMTU = B_L2CAP_LE_ATT_MINIMUM_MTU;


// Exchange MTU state of the LE links (Core Vol 3 Part F 3.4.2). One entry per
// link that has seen an exchange; a link without one uses the default.
struct att_bearer {
	HciConnection*	connection;
	uint16			mtu;
	uint16			offered;
		// the receive MTU this side gave the peer, 0 for none yet
	bool			requested;
		// this side's Exchange MTU Request went out (a client asks once)
	bool			pending;
		// and waits for its answer
	bool			socketRequest;
		// a socket sent it, and gets the answer
};

static mutex sBearerLock = MUTEX_INITIALIZER("l2cap le att bearers");
static att_bearer sBearers[16];


static att_bearer*
find_bearer(HciConnection* connection, bool create)
{
	ASSERT_LOCKED_MUTEX(&sBearerLock);
	att_bearer* unused = NULL;
	for (size_t i = 0; i < B_COUNT_OF(sBearers); i++) {
		if (sBearers[i].connection == connection)
			return &sBearers[i];
		if (sBearers[i].connection == NULL && unused == NULL)
			unused = &sBearers[i];
	}
	if (!create || unused == NULL)
		return NULL;
	memset(unused, 0, sizeof(*unused));
	unused->connection = connection;
	unused->mtu = kATTDefaultMTU;
	return unused;
}


static uint16
negotiated_mtu(uint16 clientMTU, uint16 serverMTU)
{
	// A value below the default leaves the default (3.4.2.2).
	if (clientMTU < kATTDefaultMTU || serverMTU < kATTDefaultMTU)
		return kATTDefaultMTU;
	return min_c(clientMTU, serverMTU);
}


void
l2cap_le_reload_settings()
{
	int32 level = 0;
	void* handle = load_driver_settings(kSettingsPath);
	if (handle != NULL) {
		const char* value = get_driver_parameter(handle, "level", NULL, NULL);
		if (value != NULL)
			level = strtol(value, NULL, 10);
		unload_driver_settings(handle);
	}
	if (level != gL2capLELogLevel)
		dprintf("l2cap-le: diagnostics level %" B_PRId32 "\n", level);
	gL2capLELogLevel = level;
}


void
l2cap_le_dump(const char* label, HciConnection* connection, uint16 cid,
	net_buffer* buffer)
{
	if (gL2capLELogLevel < 3 || buffer == NULL)
		return;
	uint8 bytes[32];
	size_t length = min_c(buffer->size, sizeof(bytes));
	if (gBufferModule->read(buffer, 0, bytes, length) != B_OK)
		return;
	char hex[3 * sizeof(bytes) + 4];
	size_t offset = 0;
	for (size_t i = 0; i < length; i++)
		offset += snprintf(hex + offset, sizeof(hex) - offset, "%02x ", bytes[i]);
	if (length < buffer->size)
		strlcat(hex, "...", sizeof(hex));
	// SMP key distribution and random values are secrets; show the opcode only.
	if (cid == L2CAP_SMP_CID && length > 0 && (bytes[0] == 0x03
			|| bytes[0] == 0x04 || bytes[0] == 0x06 || bytes[0] == 0x07
			|| bytes[0] == 0x08 || bytes[0] == 0x0a))
		snprintf(hex, sizeof(hex), "%02x <redacted>", bytes[0]);
	dprintf("l2cap-le: %s handle %#x cid %#x len %" B_PRIu32 ": %s\n", label,
		connection != NULL ? connection->handle : 0, cid, buffer->size, hex);
}


static status_t
send_fixed(HciConnection* connection, uint16 cid, const void* data,
	size_t length)
{
	net_buffer* buffer = gBufferModule->create(64);
	if (buffer == NULL)
		return B_NO_MEMORY;
	status_t status = gBufferModule->append(buffer, data, length);
	if (status == B_OK) {
		NetBufferPrepend<l2cap_basic_header> header(buffer);
		status = header.Status();
		if (status == B_OK) {
			header->length = B_HOST_TO_LENDIAN_INT16(length);
			header->dcid = B_HOST_TO_LENDIAN_INT16(cid);
		}
	}
	if (status != B_OK) {
		gBufferModule->free(buffer);
		return status;
	}
	l2cap_le_dump("tx", connection, cid, buffer);
	buffer->type = connection->handle;
	status = btDevices->PostACL(connection->Hid, buffer);
	if (status != B_OK)
		gBufferModule->free(buffer);
	return status;
}


static status_t
send_connection_update(HciConnection* connection, uint16 minimum,
	uint16 maximum, uint16 latency, uint16 timeout)
{
	struct {
		hci_command_header header;
		uint16 handle;
		uint16 intervalMinimum;
		uint16 intervalMaximum;
		uint16 latency;
		uint16 supervisionTimeout;
		uint16 minimumCELength;
		uint16 maximumCELength;
	} _PACKED command;
	command.header.opcode = B_HOST_TO_LENDIAN_INT16(PACK_OPCODE(0x08, 0x0013));
	command.header.clen = sizeof(command) - sizeof(command.header);
	command.handle = B_HOST_TO_LENDIAN_INT16(connection->handle);
	command.intervalMinimum = B_HOST_TO_LENDIAN_INT16(minimum);
	command.intervalMaximum = B_HOST_TO_LENDIAN_INT16(maximum);
	command.latency = B_HOST_TO_LENDIAN_INT16(latency);
	command.supervisionTimeout = B_HOST_TO_LENDIAN_INT16(timeout);
	command.minimumCELength = 0;
	command.maximumCELength = 0;

	net_buffer* buffer = gBufferModule->create(64);
	if (buffer == NULL)
		return B_NO_MEMORY;
	status_t status = gBufferModule->append(buffer, &command, sizeof(command));
	if (status == B_OK)
		status = btDevices->PostCommand(connection->Hid, buffer);
	// The transport copies commands; the buffer stays ours.
	gBufferModule->free(buffer);
	return status;
}


status_t
l2cap_le_handle_signaling(HciConnection* connection, net_buffer* buffer)
{
	l2cap_le_dump("rx", connection, L2CAP_LE_SIGNALING_CID, buffer);
	uint8 packet[16];
	if (buffer->size < 4) {
		gBufferModule->free(buffer);
		return B_BAD_DATA;
	}
	size_t length = min_c(buffer->size, sizeof(packet));
	gBufferModule->read(buffer, 0, packet, length);
	gBufferModule->free(buffer);

	const uint8 code = packet[0];
	const uint8 ident = packet[1];
	const uint16 dataLength = packet[2] | (packet[3] << 8);

	if (code == kConnectionParameterUpdateRequest && dataLength == 8
		&& length >= 12) {
		uint16 minimum = packet[4] | (packet[5] << 8);
		uint16 maximum = packet[6] | (packet[7] << 8);
		uint16 latency = packet[8] | (packet[9] << 8);
		uint16 timeout = packet[10] | (packet[11] << 8);
		// Core Vol 6 Part B 4.5.1/4.5.2 parameter limits.
		bool valid = minimum >= 6 && minimum <= maximum && maximum <= 3200
			&& timeout >= 10 && timeout <= 3200 && latency <= 499
			&& (uint32)timeout * 4 > (uint32)(1 + latency) * maximum;
		dprintf("l2cap-le: handle %#x requests interval %u-%u latency %u "
			"timeout %u: %s\n", connection->handle, minimum, maximum, latency,
			timeout, valid ? "accepted" : "rejected");
		uint8 response[6] = { kConnectionParameterUpdateResponse, ident, 2, 0,
			(uint8)(valid ? 0 : 1), 0 };
		status_t status = send_fixed(connection, L2CAP_LE_SIGNALING_CID,
			response, sizeof(response));
		if (valid && status == B_OK) {
			status = send_connection_update(connection, minimum, maximum,
				latency, timeout);
			if (status != B_OK) {
				dprintf("l2cap-le: LE connection update failed: %s\n",
					strerror(status));
			}
		}
		return status;
	}

	// Responses and rejects need no answer; anything else is not understood.
	if ((code & 1) != 0 || code == kCommandReject)
		return B_OK;
	L2CAP_LE_TRACE("unsupported LE signaling code %#x from handle %#x\n", code,
		connection->handle);
	uint8 reject[6] = { kCommandReject, ident, 2, 0, 0, 0 };
	return send_fixed(connection, L2CAP_LE_SIGNALING_CID, reject,
		sizeof(reject));
}


uint16
l2cap_le_att_mtu(HciConnection* connection)
{
	MutexLocker _(sBearerLock);
	att_bearer* bearer = find_bearer(connection, false);
	return bearer != NULL ? bearer->mtu : kATTDefaultMTU;
}


status_t
l2cap_le_att_exchange_mtu(HciConnection* connection, uint16 receiveMTU)
{
	MutexLocker locker(sBearerLock);
	att_bearer* bearer = find_bearer(connection, true);
	if (bearer == NULL)
		return B_NO_MEMORY;
	if (bearer->requested)
		return B_NOT_ALLOWED;
	// The request offers what an earlier answer to the peer gave (3.4.2.2).
	if (bearer->offered == 0)
		bearer->offered = receiveMTU;
	bearer->requested = true;
	bearer->pending = true;
	bearer->socketRequest = false;
	const uint16 offered = bearer->offered;
	locker.Unlock();

	uint8 request[3] = { kATTExchangeMTURequest, (uint8)offered,
		(uint8)(offered >> 8) };
	L2CAP_LE_TRACE("asking handle %#x for an ATT MTU of %u\n",
		connection->handle, offered);
	status_t status = send_fixed(connection, L2CAP_ATT_CID, request,
		sizeof(request));
	if (status != B_OK) {
		locker.Lock();
		bearer = find_bearer(connection, false);
		if (bearer != NULL)
			bearer->pending = false;
	}
	return status;
}


void
l2cap_le_att_note_request(HciConnection* connection, uint16 receiveMTU)
{
	MutexLocker _(sBearerLock);
	att_bearer* bearer = find_bearer(connection, true);
	if (bearer == NULL)
		return;
	bearer->offered = receiveMTU;
	bearer->requested = true;
	bearer->pending = true;
	bearer->socketRequest = true;
}


void
l2cap_le_att_forget(HciConnection* connection)
{
	MutexLocker _(sBearerLock);
	att_bearer* bearer = find_bearer(connection, false);
	if (bearer != NULL)
		bearer->connection = NULL;
}


bool
l2cap_le_answer_att(HciConnection* connection, net_buffer* buffer,
	uint16 receiveMTU, l2cap_le_att_update& update)
{
	update.mtu = 0;
	update.exchangeDone = false;

	uint8 packet[8];
	if (buffer->size < 1)
		return false;
	size_t length = min_c(buffer->size, sizeof(packet));
	if (gBufferModule->read(buffer, 0, packet, length) != B_OK)
		return false;

	const uint8 opcode = packet[0];
	uint16 handle = length >= 3 ? packet[1] | (packet[2] << 8) : 0;
	uint8 error = kATTRequestNotSupported;

	if (opcode == kATTExchangeMTUResponse
		|| (opcode == kATTErrorResponse && length >= 5
			&& packet[1] == kATTExchangeMTURequest)) {
		// The answer to an Exchange MTU Request: this side's is taken here,
		// a socket's own goes on to it.
		MutexLocker _(sBearerLock);
		att_bearer* bearer = find_bearer(connection, false);
		if (bearer == NULL || !bearer->pending)
			return false;
		bearer->pending = false;
		if (opcode == kATTExchangeMTUResponse && length >= 3)
			bearer->mtu = negotiated_mtu(bearer->offered, handle);
		update.mtu = bearer->mtu;
		update.exchangeDone = !bearer->socketRequest;
		dprintf("l2cap-le: handle %#x ATT MTU %u (offered %u, the peer %s "
			"%u)\n", connection->handle, bearer->mtu, bearer->offered,
			opcode == kATTErrorResponse ? "does not exchange, error"
				: "takes", opcode == kATTErrorResponse ? packet[4] : handle);
		return !bearer->socketRequest;
	}

	switch (opcode) {
		case kATTExchangeMTURequest:
		{
			// Answer with what this side asks for, or would: the socket's
			// receive MTU. A link no socket has opened yet keeps the
			// default, so a client that takes only 23-byte PDUs never gets
			// more.
			MutexLocker locker(sBearerLock);
			att_bearer* bearer = find_bearer(connection, true);
			uint16 offered = kATTDefaultMTU;
			if (bearer != NULL && bearer->offered != 0)
				offered = bearer->offered;
			else if (receiveMTU != 0) {
				offered = receiveMTU;
				if (bearer != NULL)
					bearer->offered = offered;
			}
			uint16 mtu = negotiated_mtu(length >= 3 ? handle : 0, offered);
			if (bearer != NULL)
				bearer->mtu = mtu;
			locker.Unlock();
			update.mtu = mtu;

			uint8 response[3] = { kATTExchangeMTUResponse, (uint8)offered,
				(uint8)(offered >> 8) };
			dprintf("l2cap-le: handle %#x offers an ATT MTU of %u; answering "
				"%u, ATT MTU %u\n", connection->handle, handle, offered, mtu);
			send_fixed(connection, L2CAP_ATT_CID, response, sizeof(response));
			return true;
		}

		case kATTHandleValueIndication:
		{
			// Confirm, then let a bound client see it like a notification.
			uint8 confirmation = kATTHandleValueConfirmation;
			send_fixed(connection, L2CAP_ATT_CID, &confirmation, 1);
			return false;
		}

		case kATTFindInformationRequest:
		case kATTFindByTypeValueRequest:
		case kATTReadByTypeRequest:
		case kATTReadByGroupTypeRequest:
			// This host exposes no attributes.
			error = kATTAttributeNotFound;
			break;

		case kATTReadRequest:
		case kATTReadBlobRequest:
		case kATTWriteRequest:
		case kATTPrepareWriteRequest:
			error = kATTInvalidHandle;
			break;

		case kATTReadMultipleRequest:
		case kATTExecuteWriteRequest:
		case kATTReadMultipleVariableRequest:
			error = kATTRequestNotSupported;
			break;

		default:
			// Responses, notifications, commands and confirmations go to the
			// bound client (or are dropped); unknown requests get an error.
			if ((opcode & 0x40) != 0 || (opcode & 1) != 0 || opcode == 0x1b
				|| opcode == kATTHandleValueConfirmation)
				return false;
			break;
	}

	uint8 response[5] = { kATTErrorResponse, opcode, (uint8)handle,
		(uint8)(handle >> 8), error };
	L2CAP_LE_TRACE("answering ATT request %#x (handle %#x) from %#x with "
		"error %#x\n", opcode, handle, connection->handle, error);
	send_fixed(connection, L2CAP_ATT_CID, response, sizeof(response));
	return true;
}
