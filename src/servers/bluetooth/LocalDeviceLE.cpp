/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Bluetooth Low Energy scanning and link management for one local device.
 */

#include "LocalDeviceImpl.h"

#include <Autolock.h>
#include <ByteOrder.h>

#include <bluetooth/HCI/btHCI_command.h>
#include <bluetooth/HCI/btHCI_event.h>
#include <bluetooth/bluetooth_error.h>

#include <LELog.h>
#include <bluetoothserver_p.h>

#include <string.h>


using Bluetooth::LELog;
using Bluetooth::LE_LOG_ERROR;
using Bluetooth::LE_LOG_INFO;
using Bluetooth::LE_LOG_DEBUG;
using Bluetooth::LE_LOG_TRACE;

#define LOG(level, format, args...) LELog(level, "server", format, ##args)

static const uint16 kOpcodeSetEventMask
	= PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_SET_EVENT_MASK);
static const uint16 kOpcodeLESetEventMask
	= PACK_OPCODE(OGF_LE_CONTROL, OCF_LE_SET_EVENT_MASK);
static const uint16 kOpcodeReadRSSI = PACK_OPCODE(OGF_STATUS_PARAM, 0x0005);
static const uint16 kOpcodeLESetScanParameters
	= PACK_OPCODE(OGF_LE_CONTROL, OCF_LE_SET_SCAN_PARAMETERS);
static const uint16 kOpcodeLESetScanEnable
	= PACK_OPCODE(OGF_LE_CONTROL, OCF_LE_SET_SCAN_ENABLE);
static const uint16 kOpcodeLECreateConnection
	= PACK_OPCODE(OGF_LE_CONTROL, OCF_LE_CREATE_CONN);
static const uint16 kOpcodeLECreateConnectionCancel
	= PACK_OPCODE(OGF_LE_CONTROL, OCF_LE_CREATE_CONN_CANCEL);
static const uint16 kOpcodeLEStartEncryption
	= PACK_OPCODE(OGF_LE_CONTROL, OCF_LE_START_ENCRYPTION);
static const uint16 kOpcodeLERemoteParameterReply
	= PACK_OPCODE(OGF_LE_CONTROL, 0x0020);
static const uint16 kOpcodeLERemoteParameterNegativeReply
	= PACK_OPCODE(OGF_LE_CONTROL, 0x0021);
static const uint16 kOpcodeDisconnect
	= PACK_OPCODE(OGF_LINK_CONTROL, OCF_DISCONNECT);

static const uint8 kSubeventConnectionComplete = 0x01;
static const uint8 kSubeventAdvertisingReport = 0x02;
static const uint8 kSubeventConnectionUpdateComplete = 0x03;
static const uint8 kSubeventRemoteFeatures = 0x04;
static const uint8 kSubeventRemoteParameterRequest = 0x06;
static const uint8 kSubeventDataLengthChange = 0x07;

static const uint8 kErrorCommandDisallowed = 0x0c;
static const uint8 kEventEncryptionKeyRefresh = 0x30;

// A client that vanishes mid-connection must not hold the controller forever.
static const bigtime_t kConnectionAttemptLimit = 45000000;
// Repeated advertisements of the same kind are forwarded at most this often.
static const bigtime_t kAdvertisementInterval = 500000;
// With the controller's duplicate filter on, restart the scan this often.
static const bigtime_t kScanRefreshInterval = 10000000;
// The longest Classic inquiry is 61.44 s; do not wait longer than this.
static const bigtime_t kInquiryLimit = 70000000;
// Notices may wait briefly for a busy client; advertisements never wait.
static const bigtime_t kNoticeTimeout = 250000;
// A controller answers a setup command within milliseconds; one that has not
// in this long never will.
static const bigtime_t kCommandTimeout = 5000000;


static const char*
AddressString(const uint8* address, char buffer[18])
{
	return Bluetooth::LEAddressString(address, buffer);
}


static const char*
ScanStateName(int state)
{
	static const char* kNames[] = { "idle", "waiting for event masks",
		"setting parameters", "enabling", "active", "disabling" };
	return state >= 0 && state < (int)B_COUNT_OF(kNames) ? kNames[state] : "?";
}


static const char*
ConnectionStateName(int state)
{
	static const char* kNames[] = { "idle", "pending", "create sent",
		"connecting", "canceling", "connected", "disconnecting" };
	return state >= 0 && state < (int)B_COUNT_OF(kNames) ? kNames[state] : "?";
}


status_t
LocalDeviceImpl::_SendLECommand(uint16 opcode, const void* data, size_t length)
{
	if (length > 255)
		return B_BAD_VALUE;
	uint8 command[HCI_COMMAND_HDR_SIZE + 255] = {
		(uint8)opcode, (uint8)(opcode >> 8), (uint8)length
	};
	if (length > 0)
		memcpy(command + HCI_COMMAND_HDR_SIZE, data, length);
	LOG(LE_LOG_TRACE, "HCI command %#06x (%zu parameter bytes)", opcode,
		length);
	fLECommandSince = system_time();
	status_t status = fHCIDelegate->IssueCommand(command,
		HCI_COMMAND_HDR_SIZE + length);
	if (status != B_OK) {
		LOG(LE_LOG_ERROR, "could not issue HCI command %#06x: %s", opcode,
			strerror(status));
	}
	return status;
}


void
LocalDeviceImpl::_SendLEScanNotice(uint32 what, uint8 status)
{
	BMessage notice(what);
	notice.AddInt32("hci_id", GetID());
	notice.AddUInt8("status", status);
	for (size_t i = 0; i < fLEScanListeners.size(); i++)
		fLEScanListeners[i].SendMessage(&notice, (BHandler*)NULL,
			kNoticeTimeout);
}


void
LocalDeviceImpl::_SendLEConnectionNotice(uint32 what, status_t status)
{
	if (!fLEConnectionListener.IsValid())
		return;
	BMessage notice(what);
	notice.AddInt32("hci_id", GetID());
	notice.AddInt32("status", status);
	notice.AddData("address", B_RAW_TYPE, fLEConnectionAddress,
		sizeof(fLEConnectionAddress));
	notice.AddUInt8("address_type", fLEConnectionAddressType);
	notice.AddUInt16("handle", fLEConnectionHandle);
	status_t result = fLEConnectionListener.SendMessage(&notice,
		(BHandler*)NULL, kNoticeTimeout);
	if (result != B_OK) {
		LOG(LE_LOG_ERROR, "LE link notice %.4s not delivered: %s",
			(const char*)&what, strerror(result));
	}
}


void
LocalDeviceImpl::_ResetLEConnection()
{
	fLEConnectionState = LE_CONN_IDLE;
	fLEConnectionSince = 0;
	fLECancelRequested = false;
	fLEConnectionListener = BMessenger();
	fLEConnectionHandle = 0;
	fLEEncryptionPending = false;
	fLEEncrypted = false;
	fLEKeepIntervalMin = fLEKeepIntervalMax = 0;
	fLEKeepRestores = 0;
	fLEKeepRestoredAt = 0;
	fLELinkInterval = fLELinkLatency = fLELinkTimeout = 0;
}


bool
LocalDeviceImpl::_LEInitiating() const
{
	return fLEConnectionState == LE_CONN_PENDING
		|| fLEConnectionState == LE_CONN_CREATE_SENT
		|| fLEConnectionState == LE_CONN_CONNECTING
		|| fLEConnectionState == LE_CONN_CANCELING;
}


void
LocalDeviceImpl::_EnsureLEMasks()
{
	if (fLEMaskState != LE_MASKS_NONE)
		return;
	// Keep Classic delivery and enable the LE Meta and encryption events.
	const uint8 eventMask[8] = {
		0xff, 0xff, 0xfb, 0xff, 0x07, 0xf8, 0xbf, 0x3d
	};
	fLEMaskState = LE_MASKS_CLASSIC;
	LOG(LE_LOG_DEBUG, "configuring HCI event masks");
	if (_SendLECommand(kOpcodeSetEventMask, eventMask, sizeof(eventMask))
			!= B_OK)
		fLEMaskState = LE_MASKS_NONE;
}


void
LocalDeviceImpl::_FailLEScan(uint8 status)
{
	LOG(LE_LOG_ERROR, "LE scan failed while %s: HCI status %#x",
		ScanStateName(fLEScanState), status);
	_SendLEScanNotice(BT_MSG_LE_SCAN_ERROR, status);
	fLEScanListeners.clear();
	fLEScanState = LE_SCAN_IDLE;
}


status_t
LocalDeviceImpl::_IssueLECreateConnection()
{
	hci_cp_le_create_conn command = {};
	command.scan_interval = B_HOST_TO_LENDIAN_INT16(0x0060);
	command.scan_window = B_HOST_TO_LENDIAN_INT16(0x0030);
	command.peer_address_type = fLEConnectionAddressType;
	memcpy(&command.peer_address, fLEConnectionAddress,
		sizeof(fLEConnectionAddress));
	// Unless the client asked for others, 30-50 ms until the peripheral
	// asks for something else; 5 s timeout.
	command.connection_interval_min
		= B_HOST_TO_LENDIAN_INT16(fLECreateIntervalMin);
	command.connection_interval_max
		= B_HOST_TO_LENDIAN_INT16(fLECreateIntervalMax);
	command.connection_latency = B_HOST_TO_LENDIAN_INT16(fLECreateLatency);
	command.supervision_timeout = B_HOST_TO_LENDIAN_INT16(fLECreateTimeout);
	char address[18];
	LOG(LE_LOG_INFO, "creating LE connection to %s (address type %u, interval "
		"%.2f-%.2f ms)", AddressString(fLEConnectionAddress, address),
		fLEConnectionAddressType, fLECreateIntervalMin * 1.25,
		fLECreateIntervalMax * 1.25);
	fLEConnectionState = LE_CONN_CREATE_SENT;
	return _SendLECommand(kOpcodeLECreateConnection, &command,
		sizeof(command));
}


void
LocalDeviceImpl::_UpdateLE()
{
	if (fLEMaskState != LE_MASKS_READY) {
		if (!fLEScanListeners.empty() || _LEInitiating())
			_EnsureLEMasks();
		return;
	}

	// Changing the LE scan (or starting a connection) while the controller
	// runs a Classic inquiry preceded every observed loss of the AX210's USB
	// link. Hold such changes until the inquiry ends; LEInquiryState()
	// calls back here.
	if (fClassicInquiryActive) {
		LOG(LE_LOG_TRACE, "LE change deferred during Classic inquiry");
		return;
	}

	const bool scanWanted = !fLEScanListeners.empty() && !_LEInitiating();
	switch (fLEScanState) {
		case LE_SCAN_IDLE:
		case LE_SCAN_WAIT_MASKS:
			if (scanWanted) {
				// Active scanning so scan responses (names) arrive; 60/30 ms.
				const uint8 parameters[7] = { 1, 0x60, 0, 0x30, 0, 0, 0 };
				fLEScanState = LE_SCAN_PARAMETERS;
				if (_SendLECommand(kOpcodeLESetScanParameters, parameters,
						sizeof(parameters)) != B_OK)
					_FailLEScan(0xff);
				return;
			}
			fLEScanState = LE_SCAN_IDLE;
			break;

		case LE_SCAN_ACTIVE:
			if (!scanWanted) {
				const uint8 disable[2] = { 0, 0 };
				LOG(LE_LOG_DEBUG, "pausing LE scan (%s)",
					fLEScanListeners.empty() ? "no listeners"
						: "connection pending");
				fLEScanState = LE_SCAN_DISABLE;
				if (_SendLECommand(kOpcodeLESetScanEnable, disable,
						sizeof(disable)) != B_OK)
					_FailLEScan(0xff);
			}
			return;

		default:
			// A command is outstanding; its completion calls back here.
			return;
	}

	if (fLEConnectionState == LE_CONN_PENDING) {
		if (fLECancelRequested) {
			_SendLEConnectionNotice(BT_MSG_LE_CONNECT_FAILED, B_CANCELED);
			_ResetLEConnection();
			_UpdateLE();
			return;
		}
		status_t result = _IssueLECreateConnection();
		if (result != B_OK) {
			_SendLEConnectionNotice(BT_MSG_LE_CONNECT_FAILED, result);
			_ResetLEConnection();
		}
	}
}


status_t
LocalDeviceImpl::StartLEScan(const BMessenger& listener)
{
	if (!listener.IsValid())
		return B_BAD_VALUE;
	BAutolock lock(fLELock);
	for (size_t i = 0; i < fLEScanListeners.size(); i++) {
		if (fLEScanListeners[i] == listener)
			return B_OK;
	}
	fLEScanListeners.push_back(listener);
	LOG(LE_LOG_DEBUG, "LE scan listener added (%zu total), scan %s",
		fLEScanListeners.size(), ScanStateName(fLEScanState));
	if (fLEScanState == LE_SCAN_ACTIVE) {
		BMessage notice(BT_MSG_LE_SCAN_STARTED);
		notice.AddInt32("hci_id", GetID());
		notice.AddUInt8("status", 0);
		listener.SendMessage(&notice, (BHandler*)NULL, kNoticeTimeout);
		return B_OK;
	}
	_UpdateLE();
	return B_OK;
}


status_t
LocalDeviceImpl::StopLEScan(const BMessenger* listener)
{
	BAutolock lock(fLELock);
	std::vector<BMessenger> removed;
	for (size_t i = 0; i < fLEScanListeners.size();) {
		if (listener == NULL || fLEScanListeners[i] == *listener) {
			removed.push_back(fLEScanListeners[i]);
			fLEScanListeners.erase(fLEScanListeners.begin() + i);
		} else
			i++;
	}
	if (removed.empty())
		return listener == NULL ? B_OK : B_BAD_VALUE;
	LOG(LE_LOG_DEBUG, "LE scan listener removed (%zu left)",
		fLEScanListeners.size());
	// The departing listener gets no more reports from here on.
	BMessage notice(BT_MSG_LE_SCAN_STOPPED);
	notice.AddInt32("hci_id", GetID());
	notice.AddUInt8("status", 0);
	for (size_t i = 0; i < removed.size(); i++)
		removed[i].SendMessage(&notice, (BHandler*)NULL, kNoticeTimeout);
	_UpdateLE();
	return B_OK;
}


status_t
LocalDeviceImpl::StartLEConnection(const BMessenger& listener,
	const uint8* address, uint8 addressType, const BMessage* parameters)
{
	if (!listener.IsValid() || address == NULL || addressType > 1)
		return B_BAD_VALUE;
	uint16 intervalMin = 0x18, intervalMax = 0x28, latency = 0;
	uint16 timeout = 0x1f4;
	if (parameters != NULL) {
		intervalMin = parameters->GetUInt16("interval_min", intervalMin);
		intervalMax = parameters->GetUInt16("interval_max", intervalMax);
		latency = parameters->GetUInt16("latency", latency);
		timeout = parameters->GetUInt16("timeout", timeout);
		// Core Vol 4 Part E 7.8.12.
		if (intervalMin < 6 || intervalMin > intervalMax
			|| intervalMax > 3200 || latency > 499 || timeout < 10
			|| timeout > 3200
			|| (uint32)timeout * 4 <= (uint32)(1 + latency) * intervalMax)
			return B_BAD_VALUE;
	}
	BAutolock lock(fLELock);
	if (fLEConnectionState != LE_CONN_IDLE) {
		LOG(LE_LOG_INFO, "LE connection request refused: link is %s",
			ConnectionStateName(fLEConnectionState));
		return B_BUSY;
	}

	fLEConnectionListener = listener;
	memcpy(fLEConnectionAddress, address, sizeof(fLEConnectionAddress));
	fLEConnectionAddressType = addressType;
	fLEConnectionHandle = 0;
	fLECancelRequested = false;
	fLEEncryptionPending = false;
	fLEEncrypted = false;
	fLEConnectionState = LE_CONN_PENDING;
	fLEConnectionSince = system_time();
	fLECreateIntervalMin = intervalMin;
	fLECreateIntervalMax = intervalMax;
	fLECreateLatency = latency;
	fLECreateTimeout = timeout;
	char text[18];
	LOG(LE_LOG_INFO, "LE connection to %s requested (scan %s)",
		AddressString(address, text), ScanStateName(fLEScanState));
	_UpdateLE();
	return B_OK;
}


status_t
LocalDeviceImpl::CancelLEConnection(const BMessenger* listener)
{
	BAutolock lock(fLELock);
	if (listener != NULL && fLEConnectionListener.IsValid()
		&& !(*listener == fLEConnectionListener))
		return B_NOT_ALLOWED;
	LOG(LE_LOG_INFO, "LE connection cancel requested while %s",
		ConnectionStateName(fLEConnectionState));
	switch (fLEConnectionState) {
		case LE_CONN_PENDING:
			_SendLEConnectionNotice(BT_MSG_LE_CONNECT_FAILED, B_CANCELED);
			_ResetLEConnection();
			_UpdateLE();
			return B_OK;
		case LE_CONN_CREATE_SENT:
			fLECancelRequested = true;
			return B_OK;
		case LE_CONN_CONNECTING:
		{
			status_t result = _SendLECommand(kOpcodeLECreateConnectionCancel,
				NULL, 0);
			if (result == B_OK) {
				fLECancelRequested = true;
				fLEConnectionState = LE_CONN_CANCELING;
			}
			return result;
		}
		case LE_CONN_CANCELING:
			return B_OK;
		default:
			return B_BAD_VALUE;
	}
}


status_t
LocalDeviceImpl::DisconnectLEConnection(const BMessenger* listener)
{
	BAutolock lock(fLELock);
	if (listener != NULL && fLEConnectionListener.IsValid()
		&& !(*listener == fLEConnectionListener))
		return B_NOT_ALLOWED;
	if (fLEConnectionState == LE_CONN_DISCONNECTING)
		return B_OK;
	if (fLEConnectionState != LE_CONN_CONNECTED)
		return B_BAD_VALUE;
	const uint8 command[3] = {
		(uint8)fLEConnectionHandle, (uint8)(fLEConnectionHandle >> 8), 0x13
	};
	LOG(LE_LOG_INFO, "disconnecting LE handle %#x", fLEConnectionHandle);
	status_t result = _SendLECommand(kOpcodeDisconnect, command,
		sizeof(command));
	if (result == B_OK)
		fLEConnectionState = LE_CONN_DISCONNECTING;
	return result;
}


status_t
LocalDeviceImpl::StartLEEncryption(const uint8* key, const uint8* randomNumber,
	uint16 encryptedDiversifier)
{
	if (key == NULL || (randomNumber == NULL && encryptedDiversifier != 0))
		return B_BAD_VALUE;
	BAutolock lock(fLELock);
	if (fLEConnectionState != LE_CONN_CONNECTED)
		return B_NO_INIT;
	if (fLEEncryptionPending || fLEEncrypted)
		return B_BUSY;

	hci_cp_le_start_encryption parameters = {};
	parameters.connection_handle = B_HOST_TO_LENDIAN_INT16(fLEConnectionHandle);
	if (randomNumber != NULL) {
		memcpy(parameters.random_number, randomNumber,
			sizeof(parameters.random_number));
		parameters.encrypted_diversifier
			= B_HOST_TO_LENDIAN_INT16(encryptedDiversifier);
	}
	memcpy(parameters.long_term_key, key, sizeof(parameters.long_term_key));
	LOG(LE_LOG_INFO, "starting LE encryption on handle %#x with %s",
		fLEConnectionHandle, randomNumber != NULL ? "a stored long-term key"
			: "the pairing short-term key");
	fLEEncryptionPending = true;
	status_t result = _SendLECommand(kOpcodeLEStartEncryption, &parameters,
		sizeof(parameters));
	if (result != B_OK)
		fLEEncryptionPending = false;
	volatile uint8* secret = (volatile uint8*)&parameters;
	for (size_t i = 0; i < sizeof(parameters); i++)
		secret[i] = 0;
	return result;
}


void
LocalDeviceImpl::LEInquiryState(bool active)
{
	BAutolock lock(fLELock);
	if (active == fClassicInquiryActive)
		return;
	fClassicInquiryActive = active;
	fClassicInquirySince = system_time();
	LOG(LE_LOG_DEBUG, "Classic inquiry %s", active ? "started" : "finished");
	if (!active)
		_UpdateLE();
}


void
LocalDeviceImpl::Pulse()
{
	fHCIDelegate->Pulse();
	LEPulse();
}


void
LocalDeviceImpl::LEPulse()
{
	BAutolock lock(fLELock);
	if (_LEInitiating() && fLEConnectionSince > 0
		&& system_time() - fLEConnectionSince > kConnectionAttemptLimit
		&& fLEConnectionState != LE_CONN_CANCELING) {
		LOG(LE_LOG_ERROR, "LE connection attempt still %s after %d s; "
			"canceling it", ConnectionStateName(fLEConnectionState),
			(int)(kConnectionAttemptLimit / 1000000));
		if (fLEConnectionState == LE_CONN_CONNECTING) {
			if (_SendLECommand(kOpcodeLECreateConnectionCancel, NULL, 0)
					== B_OK) {
				fLECancelRequested = true;
				fLEConnectionState = LE_CONN_CANCELING;
			}
		} else if (fLEConnectionState == LE_CONN_PENDING) {
			_SendLEConnectionNotice(BT_MSG_LE_CONNECT_FAILED, B_TIMED_OUT);
			_ResetLEConnection();
			_UpdateLE();
		} else
			fLECancelRequested = true;
	}

	// The event masks and the scan move on only when the controller answers
	// the command before; an answer that never comes (a controller still
	// starting up, a transfer lost on the bus) would leave LE stalled for
	// good, with bonded devices never reconnected. Start that step over.
	const bool masksWaiting = fLEMaskState == LE_MASKS_CLASSIC
		|| fLEMaskState == LE_MASKS_LE;
	const bool scanWaiting = fLEScanState == LE_SCAN_PARAMETERS
		|| fLEScanState == LE_SCAN_ENABLE || fLEScanState == LE_SCAN_DISABLE;
	if ((masksWaiting || scanWaiting)
		&& system_time() - fLECommandSince > kCommandTimeout) {
		LOG(LE_LOG_ERROR, "no answer from the controller for %d s while "
			"%s; starting that over", (int)(kCommandTimeout / 1000000),
			masksWaiting ? "setting the event masks"
				: ScanStateName(fLEScanState));
		if (masksWaiting)
			fLEMaskState = LE_MASKS_NONE;
		if (scanWaiting)
			fLEScanState = LE_SCAN_IDLE;
		_UpdateLE();
	}

	if (fClassicInquiryActive
		&& system_time() - fClassicInquirySince > kInquiryLimit) {
		LOG(LE_LOG_ERROR, "Classic inquiry never reported completion; "
			"resuming LE changes");
		fClassicInquiryActive = false;
		_UpdateLE();
	}

	// Refresh the controller's duplicate filter so listeners see updated
	// RSSI and late scan responses; a short pause, then _UpdateLE() resumes.
	if (fLEScanState == LE_SCAN_ACTIVE && !fLEScanListeners.empty()
		&& !fClassicInquiryActive
		&& system_time() - fLEScanSince > kScanRefreshInterval) {
		const uint8 disable[2] = { 0, 0 };
		LOG(LE_LOG_TRACE, "refreshing LE scan duplicate filter");
		fLEScanState = LE_SCAN_DISABLE;
		if (_SendLECommand(kOpcodeLESetScanEnable, disable, sizeof(disable))
				!= B_OK)
			_FailLEScan(0xff);
	}

	// Drop scan listeners whose application has gone away.
	bool removed = false;
	for (size_t i = 0; i < fLEScanListeners.size();) {
		if (!fLEScanListeners[i].IsValid()) {
			fLEScanListeners.erase(fLEScanListeners.begin() + i);
			removed = true;
		} else
			i++;
	}
	if (removed) {
		LOG(LE_LOG_DEBUG, "dropped departed LE scan listeners (%zu left)",
			fLEScanListeners.size());
		_UpdateLE();
	}

	// A link whose owner died would otherwise stay up with no one using it.
	if (fLEConnectionState == LE_CONN_CONNECTED
		&& !fLEConnectionListener.IsValid()) {
		LOG(LE_LOG_INFO, "LE link owner is gone; disconnecting handle %#x",
			fLEConnectionHandle);
		const uint8 command[3] = {
			(uint8)fLEConnectionHandle, (uint8)(fLEConnectionHandle >> 8), 0x13
		};
		if (_SendLECommand(kOpcodeDisconnect, command, sizeof(command)) == B_OK)
			fLEConnectionState = LE_CONN_DISCONNECTING;
	}
}


void
LocalDeviceImpl::HandleLECommandComplete(struct hci_event_header* event)
{
	if (event->elen < 4)
		return;
	const uint8* payload = (const uint8*)(event + 1);
	const uint16 opcode = payload[1] | ((uint16)payload[2] << 8);
	const uint8 status = payload[3];
	BAutolock lock(fLELock);

	if (opcode == kOpcodeReadRSSI) {
		// Status, handle, RSSI (signed dBm; for BR/EDR, relative to the
		// controller's golden receive range).
		if (status == 0 && event->elen >= 7) {
			LOG(LE_LOG_INFO, "RSSI on handle %#x: %d dBm",
				payload[4] | (payload[5] << 8), (int8)payload[6]);
		} else
			LOG(LE_LOG_INFO, "reading the RSSI failed: %#x", status);
		return;
	}

	if (opcode == kOpcodeSetEventMask && fLEMaskState == LE_MASKS_CLASSIC) {
		if (status != 0)
			LOG(LE_LOG_ERROR, "Set Event Mask failed: %#x", status);
		// Continue anyway: the LE Meta event is enabled on most controllers.
		const uint8 leEventMask[8] = {
			// Connection complete, advertising report, connection update,
			// remote features, remote connection parameter request, data
			// length change.
			0x6f, 0, 0, 0, 0, 0, 0, 0
		};
		fLEMaskState = LE_MASKS_LE;
		if (_SendLECommand(kOpcodeLESetEventMask, leEventMask,
				sizeof(leEventMask)) != B_OK)
			fLEMaskState = LE_MASKS_NONE;
		return;
	}
	if (opcode == kOpcodeLESetEventMask && fLEMaskState == LE_MASKS_LE) {
		if (status != 0) {
			LOG(LE_LOG_ERROR, "LE Set Event Mask failed: %#x", status);
			fLEMaskState = LE_MASKS_NONE;
			if (!fLEScanListeners.empty())
				_FailLEScan(status);
			if (_LEInitiating()) {
				_SendLEConnectionNotice(BT_MSG_LE_CONNECT_FAILED, status);
				_ResetLEConnection();
			}
			return;
		}
		LOG(LE_LOG_DEBUG, "LE event masks ready");
		fLEMaskState = LE_MASKS_READY;
		_UpdateLE();
		return;
	}

	if (opcode == kOpcodeLESetScanParameters
		&& fLEScanState == LE_SCAN_PARAMETERS) {
		if (status != 0 && status != kErrorCommandDisallowed) {
			_FailLEScan(status);
			_UpdateLE();
			return;
		}
		// Controller duplicate filtering on: unfiltered advertising reports
		// arrive fast enough to overrun some USB host controllers. LEPulse()
		// restarts the scan periodically so RSSI and names still refresh.
		const uint8 enable[2] = { 1, 1 };
		fLEScanState = LE_SCAN_ENABLE;
		if (_SendLECommand(kOpcodeLESetScanEnable, enable, sizeof(enable))
				!= B_OK)
			_FailLEScan(0xff);
		return;
	}

	if (opcode == kOpcodeLESetScanEnable) {
		if (fLEScanState == LE_SCAN_ENABLE) {
			// Command Disallowed means the scan was already running.
			if (status != 0 && status != kErrorCommandDisallowed) {
				_FailLEScan(status);
				_UpdateLE();
				return;
			}
			fLEScanState = LE_SCAN_ACTIVE;
			fLEScanSince = system_time();
			fLESeen.clear();
			LOG(LE_LOG_DEBUG, "LE scan active for %zu listener(s)",
				fLEScanListeners.size());
			_SendLEScanNotice(BT_MSG_LE_SCAN_STARTED);
			_UpdateLE();
		} else if (fLEScanState == LE_SCAN_DISABLE) {
			if (status != 0 && status != kErrorCommandDisallowed)
				LOG(LE_LOG_ERROR, "LE scan disable failed: %#x", status);
			fLEScanState = LE_SCAN_IDLE;
			LOG(LE_LOG_DEBUG, "LE scan stopped");
			_UpdateLE();
		}
		return;
	}

	if (opcode == kOpcodeLECreateConnectionCancel) {
		LOG(LE_LOG_DEBUG, "LE Create Connection Cancel complete: %#x", status);
		return;
	}
	if (opcode == kOpcodeLERemoteParameterReply
		|| opcode == kOpcodeLERemoteParameterNegativeReply) {
		LOG(status == 0 ? LE_LOG_DEBUG : LE_LOG_ERROR,
			"connection parameter reply complete: %#x", status);
	}
}


void
LocalDeviceImpl::HandleLECommandStatus(struct hci_event_header* event)
{
	if (event->elen < sizeof(hci_ev_cmd_status))
		return;
	const hci_ev_cmd_status* commandStatus
		= (const hci_ev_cmd_status*)(event + 1);
	const uint16 opcode = B_LENDIAN_TO_HOST_INT16(commandStatus->opcode);
	const uint8 status = commandStatus->status;
	BAutolock lock(fLELock);

	if (opcode == kOpcodeLECreateConnection
		&& fLEConnectionState == LE_CONN_CREATE_SENT) {
		if (status != 0) {
			LOG(LE_LOG_ERROR, "LE Create Connection rejected: %#x (%s)",
				status, BluetoothError(status));
			_SendLEConnectionNotice(BT_MSG_LE_CONNECT_FAILED, status);
			_ResetLEConnection();
			_UpdateLE();
			return;
		}
		fLEConnectionState = LE_CONN_CONNECTING;
		LOG(LE_LOG_INFO, "LE connection initiating; waiting for the peer to "
			"advertise");
		if (fLECancelRequested) {
			if (_SendLECommand(kOpcodeLECreateConnectionCancel, NULL, 0)
					== B_OK)
				fLEConnectionState = LE_CONN_CANCELING;
		} else
			_SendLEConnectionNotice(BT_MSG_LE_CONNECTING);
	} else if (opcode == kOpcodeDisconnect
		&& fLEConnectionState == LE_CONN_DISCONNECTING && status != 0) {
		LOG(LE_LOG_ERROR, "LE Disconnect rejected: %#x", status);
		fLEConnectionState = LE_CONN_CONNECTED;
	} else if (opcode == kOpcodeLEStartEncryption && fLEEncryptionPending) {
		if (status != 0) {
			LOG(LE_LOG_ERROR, "LE Start Encryption rejected: %#x (%s)", status,
				BluetoothError(status));
			fLEEncryptionPending = false;
			_SendLEConnectionNotice(BT_MSG_LE_ENCRYPTION_FAILED, status);
		} else
			LOG(LE_LOG_DEBUG, "LE Start Encryption accepted by controller");
	}
}


bool
LocalDeviceImpl::_ForwardAdvertisement(const uint8* report, size_t dataLength)
{
	const uint8 eventType = report[0];
	const uint8 addressType = report[1];
	const uint8* address = report + 2;
	const bigtime_t now = system_time();
	for (size_t i = 0; i < fLESeen.size(); i++) {
		LEAdvertiserSeen& seen = fLESeen[i];
		if (seen.addressType != addressType || seen.eventType != eventType
			|| memcmp(seen.address, address, 6) != 0)
			continue;
		if (now - seen.when < kAdvertisementInterval)
			return false;
		seen.when = now;
		return true;
	}
	if (fLESeen.size() >= 256)
		fLESeen.erase(fLESeen.begin());
	LEAdvertiserSeen seen;
	memcpy(seen.address, address, 6);
	seen.addressType = addressType;
	seen.eventType = eventType;
	seen.when = now;
	fLESeen.push_back(seen);
	if (Bluetooth::LELogLevel() >= LE_LOG_TRACE) {
		char text[18];
		LOG(LE_LOG_TRACE, "new advertiser %s type %u event %u, %zu data bytes",
			AddressString(address, text), addressType, eventType, dataLength);
	}
	return true;
}


void
LocalDeviceImpl::HandleLEAdvertisingReport(struct hci_event_header* event)
{
	const uint8* payload = (const uint8*)(event + 1);
	BAutolock lock(fLELock);
	if (fLEScanState != LE_SCAN_ACTIVE || fLEScanListeners.empty())
		return;
	size_t offset = 2;
	for (uint8 i = 0; i < payload[1]; i++) {
		if (offset + 10 > event->elen)
			break;
		const uint8 dataLength = payload[offset + 8];
		if (offset + 10 + dataLength > event->elen)
			break;
		if (_ForwardAdvertisement(payload + offset, dataLength)) {
			BMessage report(BT_MSG_LE_ADVERTISEMENT);
			report.AddInt32("hci_id", GetID());
			report.AddUInt8("event_type", payload[offset]);
			report.AddUInt8("address_type", payload[offset + 1]);
			report.AddData("address", B_RAW_TYPE, payload + offset + 2, 6);
			report.AddData("data", B_RAW_TYPE, payload + offset + 9,
				dataLength);
			report.AddInt8("rssi", (int8)payload[offset + 9 + dataLength]);
			for (size_t j = 0; j < fLEScanListeners.size(); j++) {
				// Never block the HCI event thread on a slow listener.
				fLEScanListeners[j].SendMessage(&report, (BHandler*)NULL, 0);
			}
		}
		offset += 10 + dataLength;
	}
}


void
LocalDeviceImpl::HandleLEConnectionComplete(struct hci_event_header* event)
{
	if (event->elen < sizeof(hci_ev_le_conn_complete))
		return;
	const hci_ev_le_conn_complete* complete
		= (const hci_ev_le_conn_complete*)(event + 1);
	const uint16 handle = B_LENDIAN_TO_HOST_INT16(complete->handle);
	char text[18];
	BAutolock lock(fLELock);

	if (fLEConnectionState != LE_CONN_CONNECTING
		&& fLEConnectionState != LE_CONN_CANCELING
		&& fLEConnectionState != LE_CONN_CREATE_SENT) {
		LOG(LE_LOG_ERROR, "unexpected LE connection complete (status %#x, "
			"handle %#x, peer %s) while %s", complete->status, handle,
			AddressString((const uint8*)&complete->peer_address, text),
			ConnectionStateName(fLEConnectionState));
		if (complete->status == 0 && fLEConnectionState == LE_CONN_IDLE) {
			// Nobody asked for this link; do not leave it dangling.
			const uint8 command[3] = { (uint8)handle, (uint8)(handle >> 8),
				0x13 };
			_SendLECommand(kOpcodeDisconnect, command, sizeof(command));
		}
		return;
	}
	if (complete->status != 0) {
		LOG(fLECancelRequested ? LE_LOG_INFO : LE_LOG_ERROR,
			"LE connection %s: %#x (%s)",
			fLECancelRequested ? "canceled" : "failed", complete->status,
			BluetoothError(complete->status));
		_SendLEConnectionNotice(BT_MSG_LE_CONNECT_FAILED,
			fLECancelRequested ? B_CANCELED : complete->status);
		_ResetLEConnection();
		_UpdateLE();
		return;
	}

	fLEConnectionHandle = handle;
	fLEConnectionState = LE_CONN_CONNECTED;
	fLELinkInterval = B_LENDIAN_TO_HOST_INT16(complete->connection_interval);
	fLELinkLatency = B_LENDIAN_TO_HOST_INT16(complete->connection_latency);
	fLELinkTimeout = B_LENDIAN_TO_HOST_INT16(complete->supervision_timeout);
	LOG(LE_LOG_INFO, "LE link up: handle %#x peer %s role %u interval %.2f ms "
		"latency %u supervision %u ms", handle,
		AddressString((const uint8*)&complete->peer_address, text),
		complete->role,
		B_LENDIAN_TO_HOST_INT16(complete->connection_interval) * 1.25,
		B_LENDIAN_TO_HOST_INT16(complete->connection_latency),
		B_LENDIAN_TO_HOST_INT16(complete->supervision_timeout) * 10);
	if (fLECancelRequested) {
		const uint8 command[3] = { (uint8)handle, (uint8)(handle >> 8), 0x13 };
		if (_SendLECommand(kOpcodeDisconnect, command, sizeof(command))
				== B_OK)
			fLEConnectionState = LE_CONN_DISCONNECTING;
		_SendLEConnectionNotice(BT_MSG_LE_CONNECT_FAILED, B_CANCELED);
	} else
		_SendLEConnectionNotice(BT_MSG_LE_CONNECTED);
	_UpdateLE();
}


status_t
LocalDeviceImpl::UpdateLEConnection(uint16 intervalMin, uint16 intervalMax,
	uint16 latency, uint16 timeout, bool keepInterval)
{
	BAutolock lock(fLELock);
	if (fLEConnectionState != LE_CONN_CONNECTED)
		return B_DEV_NOT_READY;
	if (keepInterval) {
		fLEKeepIntervalMin = intervalMin;
		fLEKeepIntervalMax = intervalMax;
		fLEKeepRestores = 0;
		fLEKeepRestoredAt = 0;
	}
	// Unspecified: what the link has (a peripheral's latency saves its
	// battery; it is not ours to take away).
	if (latency == 0xffff)
		latency = fLELinkLatency;
	if (timeout == 0)
		timeout = fLELinkTimeout;
	if (keepInterval && fLELinkInterval >= intervalMin
		&& fLELinkInterval <= intervalMax)
		return B_OK;
	// Core Vol 4 Part E 7.8.18, with the limits of Vol 6 Part B 4.5.
	if (intervalMin < 6 || intervalMin > intervalMax || intervalMax > 3200
		|| latency > 499 || timeout < 10 || timeout > 3200
		|| (uint32)timeout * 4 <= (uint32)(1 + latency) * intervalMax)
		return B_BAD_VALUE;
	const uint16 handle = fLEConnectionHandle;
	const uint8 command[14] = {
		(uint8)handle, (uint8)(handle >> 8),
		(uint8)intervalMin, (uint8)(intervalMin >> 8),
		(uint8)intervalMax, (uint8)(intervalMax >> 8),
		(uint8)latency, (uint8)(latency >> 8),
		(uint8)timeout, (uint8)(timeout >> 8),
		0, 0, 0, 0
	};
	LOG(LE_LOG_INFO, "asking for interval %.2f-%.2f ms latency %u timeout %u "
		"ms on handle %#x", intervalMin * 1.25, intervalMax * 1.25, latency,
		timeout * 10, handle);
	return _SendLECommand(PACK_OPCODE(OGF_LE_CONTROL, 0x0013), command,
		sizeof(command));
}


status_t
LocalDeviceImpl::ReadRSSI(int32 handle)
{
	BAutolock lock(fLELock);
	if (handle < 0) {
		if (fLEConnectionState != LE_CONN_CONNECTED)
			return B_DEV_NOT_READY;
		handle = fLEConnectionHandle;
	}
	const uint8 command[2] = { (uint8)handle, (uint8)(handle >> 8) };
	return _SendLECommand(kOpcodeReadRSSI, command, sizeof(command));
}


/*!	The peer changed the link's parameters (a mouse asks again every now
	and then, and the controller picks the slowest interval it allows). If
	a client asked for an interval to be kept, ask for it back, with the
	latency and timeout the peer wanted. A peer that undoes it right away,
	three times, wins.
*/
void
LocalDeviceImpl::_KeepLEInterval(uint16 handle, uint16 interval,
	uint16 latency, uint16 timeout)
{
	BAutolock lock(fLELock);
	if (handle == fLEConnectionHandle) {
		fLELinkInterval = interval;
		fLELinkLatency = latency;
		fLELinkTimeout = timeout;
	}
	if (fLEConnectionState != LE_CONN_CONNECTED || handle != fLEConnectionHandle
		|| fLEKeepIntervalMin == 0
		|| (interval >= fLEKeepIntervalMin && interval <= fLEKeepIntervalMax))
		return;
	// Undone again within seconds of asking means the peer will not have it.
	const bigtime_t now = system_time();
	if (fLEKeepRestoredAt != 0 && now - fLEKeepRestoredAt < 5000000
		&& ++fLEKeepRestores >= 3) {
		LOG(LE_LOG_INFO, "peer on handle %#x keeps choosing %.2f ms; "
			"leaving it", handle, interval * 1.25);
		fLEKeepIntervalMin = fLEKeepIntervalMax = 0;
		return;
	}
	if ((uint32)timeout * 4 <= (uint32)(1 + latency) * fLEKeepIntervalMax)
		return;
	fLEKeepRestoredAt = now;
	const uint8 command[14] = {
		(uint8)handle, (uint8)(handle >> 8),
		(uint8)fLEKeepIntervalMin, (uint8)(fLEKeepIntervalMin >> 8),
		(uint8)fLEKeepIntervalMax, (uint8)(fLEKeepIntervalMax >> 8),
		(uint8)latency, (uint8)(latency >> 8),
		(uint8)timeout, (uint8)(timeout >> 8),
		0, 0, 0, 0
	};
	LOG(LE_LOG_INFO, "asking for %.2f-%.2f ms again on handle %#x (latency "
		"%u, timeout %u ms)", fLEKeepIntervalMin * 1.25,
		fLEKeepIntervalMax * 1.25, handle, latency, timeout * 10);
	_SendLECommand(PACK_OPCODE(OGF_LE_CONTROL, 0x0013), command,
		sizeof(command));
}


void
LocalDeviceImpl::HandleLERemoteParameterRequest(struct hci_event_header* event)
{
	const uint8* payload = (const uint8*)(event + 1);
	if (event->elen < 11)
		return;
	const uint16 handle = payload[1] | (payload[2] << 8);
	const uint16 minimum = payload[3] | (payload[4] << 8);
	const uint16 maximum = payload[5] | (payload[6] << 8);
	const uint16 latency = payload[7] | (payload[8] << 8);
	const uint16 timeout = payload[9] | (payload[10] << 8);
	const bool valid = minimum >= 6 && minimum <= maximum && maximum <= 3200
		&& timeout >= 10 && timeout <= 3200 && latency <= 499
		&& (uint32)timeout * 4 > (uint32)(1 + latency) * maximum;
	LOG(LE_LOG_INFO, "peer on handle %#x requests interval %.2f-%.2f ms "
		"latency %u timeout %u ms: %s", handle, minimum * 1.25, maximum * 1.25,
		latency, timeout * 10, valid ? "accepting" : "rejecting");
	if (valid) {
		uint8 reply[14] = {
			payload[1], payload[2], payload[3], payload[4], payload[5],
			payload[6], payload[7], payload[8], payload[9], payload[10],
			0, 0, 0, 0
		};
		_SendLECommand(kOpcodeLERemoteParameterReply, reply, sizeof(reply));
	} else {
		// Unacceptable Connection Parameters.
		const uint8 reply[3] = { payload[1], payload[2], 0x3b };
		_SendLECommand(kOpcodeLERemoteParameterNegativeReply, reply,
			sizeof(reply));
	}
}


void
LocalDeviceImpl::HandleLEMeta(struct hci_event_header* event)
{
	if (event->elen < 1)
		return;
	const uint8* payload = (const uint8*)(event + 1);
	switch (payload[0]) {
		case kSubeventConnectionComplete:
			HandleLEConnectionComplete(event);
			break;
		case kSubeventAdvertisingReport:
			if (event->elen >= 2)
				HandleLEAdvertisingReport(event);
			break;
		case kSubeventConnectionUpdateComplete:
			if (event->elen >= 10) {
				LOG(LE_LOG_INFO, "LE connection update on handle %#x: status "
					"%#x interval %.2f ms latency %u timeout %u ms",
					payload[2] | (payload[3] << 8), payload[1],
					(payload[4] | (payload[5] << 8)) * 1.25,
					payload[6] | (payload[7] << 8),
					(payload[8] | (payload[9] << 8)) * 10);
				if (payload[1] == 0) {
					_KeepLEInterval(payload[2] | (payload[3] << 8),
						payload[4] | (payload[5] << 8),
						payload[6] | (payload[7] << 8),
						payload[8] | (payload[9] << 8));
				}
			}
			break;
		case kSubeventRemoteFeatures:
			if (event->elen >= 12) {
				LOG(LE_LOG_DEBUG, "LE remote features on handle %#x: status "
					"%#x features %02x%02x%02x%02x%02x%02x%02x%02x",
					payload[2] | (payload[3] << 8), payload[1], payload[11],
					payload[10], payload[9], payload[8], payload[7],
					payload[6], payload[5], payload[4]);
			}
			break;
		case kSubeventRemoteParameterRequest:
			HandleLERemoteParameterRequest(event);
			break;
		case kSubeventDataLengthChange:
			LOG(LE_LOG_DEBUG, "LE data length changed");
			break;
		default:
			LOG(LE_LOG_DEBUG, "unhandled LE meta subevent %#x", payload[0]);
			break;
	}
}


bool
LocalDeviceImpl::HandleLEDisconnection(uint16 handle, uint8 status,
	uint8 reason)
{
	BAutolock lock(fLELock);
	if (fLEConnectionState == LE_CONN_IDLE || handle != fLEConnectionHandle
		|| (fLEConnectionState != LE_CONN_CONNECTED
			&& fLEConnectionState != LE_CONN_DISCONNECTING))
		return false;
	LOG(LE_LOG_INFO, "LE link down: handle %#x status %#x reason %#x (%s)",
		handle, status, reason, BluetoothError(status == 0 ? reason : status));
	if (status != 0 && fLEConnectionState == LE_CONN_DISCONNECTING) {
		fLEConnectionState = LE_CONN_CONNECTED;
		return true;
	}
	if (fLEEncryptionPending) {
		_SendLEConnectionNotice(BT_MSG_LE_ENCRYPTION_FAILED,
			reason != 0 ? reason : B_ERROR);
	}
	_SendLEConnectionNotice(BT_MSG_LE_DISCONNECTED,
		status == 0 ? reason : status);
	_ResetLEConnection();
	_UpdateLE();
	return true;
}


bool
LocalDeviceImpl::HandleLEEncryptionChange(uint16 handle, uint8 status,
	bool enabled)
{
	BAutolock lock(fLELock);
	if ((fLEConnectionState != LE_CONN_CONNECTED
			&& fLEConnectionState != LE_CONN_DISCONNECTING)
		|| handle != fLEConnectionHandle)
		return false;
	fLEEncrypted = status == 0 && enabled;
	LOG(fLEEncrypted ? LE_LOG_INFO : LE_LOG_ERROR,
		"LE encryption on handle %#x: status %#x (%s), %s", handle, status,
		BluetoothError(status), enabled ? "enabled" : "disabled");
	if (fLEEncryptionPending) {
		fLEEncryptionPending = false;
		_SendLEConnectionNotice(fLEEncrypted ? BT_MSG_LE_ENCRYPTED
			: BT_MSG_LE_ENCRYPTION_FAILED,
			fLEEncrypted ? B_OK : (status != 0 ? status : B_ERROR));
	}
	return true;
}
