/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "typec.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <driver_settings.h>

#include "registers.h"


#define INFO(x...)	dprintf("sunxi_display: typec: " x)
#define ERROR(x...)	dprintf("sunxi_display: typec: " x)
//#define TRACE_TYPEC
#ifdef TRACE_TYPEC
#	define TRACE(x...) _Trace(x)
#else
#	define TRACE(x...) ;
#endif


// the port controller
#define HUSB311_ADDRESS			0x4e
#define HUSB311_VENDOR_ID		0x2e99
#define HUSB311_PRODUCT_ID		0x0311

// TCPCI registers
#define TCPC_VENDOR_ID			0x00
#define TCPC_PRODUCT_ID			0x02
#define TCPC_ALERT				0x10
#define  ALERT_CC_STATUS		(1u << 0)
#define  ALERT_POWER_STATUS		(1u << 1)
#define  ALERT_RX_STATUS		(1u << 2)
#define  ALERT_RX_HARD_RESET	(1u << 3)
#define  ALERT_TX_FAILED		(1u << 4)
#define  ALERT_TX_DISCARDED		(1u << 5)
#define  ALERT_TX_SUCCESS		(1u << 6)
#define  ALERT_RX_OVERFLOW		(1u << 10)
#define TCPC_ALERT_MASK			0x12
#define TCPC_CONFIG_STD_OUTPUT	0x18
#define TCPC_TCPC_CTRL			0x19
#define  TCPC_CTRL_ORIENTATION	(1u << 0)
#define TCPC_ROLE_CTRL			0x1a
#define  ROLE_RP_1_5			(1u << 4)
#define  ROLE_CC_RP				1
#define  ROLE_CC_RD				2
#define  ROLE_CC_OPEN			3
#define TCPC_POWER_CTRL			0x1c
#define  POWER_VCONN_ENABLE		(1u << 0)
#define TCPC_CC_STATUS			0x1d
#define  CC_STATUS_TERM_RD		(1u << 4)
#define  CC_SRC_OPEN			0
#define  CC_SRC_RA				1
#define  CC_SRC_RD				2
#define TCPC_POWER_STATUS		0x1e
#define TCPC_COMMAND			0x23
#define TCPC_MSG_HDR_INFO		0x2e
#define TCPC_RX_DETECT			0x2f
#define  RX_DETECT_SOP			(1u << 0)
#define  RX_DETECT_HARD_RESET	(1u << 5)
#define TCPC_RX_BYTE_CNT		0x30
#define TCPC_RX_BUF_FRAME_TYPE	0x31
#define TCPC_RX_HDR				0x32
#define TCPC_RX_DATA			0x34
#define TCPC_TRANSMIT			0x50
#define  TRANSMIT_RETRY_3		(3u << 4)
#define  TRANSMIT_SOP			0
#define  TRANSMIT_HARD_RESET	5
#define TCPC_TX_BYTE_CNT		0x51
#define TCPC_TX_HDR				0x52
#define TCPC_TX_DATA			0x54

// HUSB311 vendor registers (Allwinner BSP: tcpci_husb311.c)
#define HUSB311_IDLE_CTRL		0x9b
#define HUSB311_I2CRST_CTRL		0x9e
#define HUSB311_SWRESET			0xa0
#define HUSB311_TTCPC_FILTER	0xa1
#define HUSB311_DRP_TOGGLE_CYCLE 0xa2
#define HUSB311_DRP_DUTY_CTRL	0xa3
#define HUSB311_VCONN_CLIMITEN	0x95
#define HUSB311_IDLE_SET(autoIdle) ((1u << 5) | ((autoIdle) << 3) | (1u << 4))

// USB Power Delivery
#define PD_CONTROL_GOODCRC		1
#define PD_CONTROL_ACCEPT		3
#define PD_CONTROL_REJECT		4
#define PD_CONTROL_PING			5
#define PD_CONTROL_PS_RDY		6
#define PD_CONTROL_GET_SOURCE_CAP 7
#define PD_CONTROL_GET_SINK_CAP	8
#define PD_CONTROL_DR_SWAP		9
#define PD_CONTROL_PR_SWAP		10
#define PD_CONTROL_VCONN_SWAP	11
#define PD_CONTROL_WAIT			12
#define PD_CONTROL_SOFT_RESET	13
#define PD_DATA_SOURCE_CAP		1
#define PD_DATA_REQUEST			2
#define PD_DATA_VENDOR_DEFINED	15

#define PD_HEADER_DATA_ROLE_DFP	(1u << 5)
#define PD_HEADER_REV20			(1u << 6)
#define PD_HEADER_POWER_SOURCE	(1u << 8)

// 5 V at 900 mA
#define PD_SOURCE_PDO			((100u << 10) | 90u)

// structured VDMs
#define VDM_STRUCTURED			(1u << 15)
#define VDM_SVID(header)		((header) >> 16)
#define VDM_POSITION(x)			((uint32)(x) << 8)
#define VDM_TYPE(header)		(((header) >> 6) & 3)
#define VDM_TYPE_REQUEST		0
#define VDM_TYPE_ACK			1
#define VDM_TYPE_NAK			2
#define VDM_TYPE_BUSY			3
#define VDM_COMMAND(header)		((header) & 0x1f)
#define VDM_DISCOVER_IDENTITY	1
#define VDM_DISCOVER_SVIDS		2
#define VDM_DISCOVER_MODES		3
#define VDM_ENTER_MODE			4
#define VDM_EXIT_MODE			5
#define VDM_ATTENTION			6
#define VDM_DP_STATUS			16
#define VDM_DP_CONFIGURE		17
#define SVID_PD					0xff00
#define SVID_DISPLAYPORT		0xff01

// DisplayPort alt mode
#define DP_STATUS_DFP_D_CONNECTED	1
#define DP_STATUS_HPD			(1u << 7)
#define DP_STATUS_IRQ_HPD		(1u << 8)
#define DP_CAP_UFP_D			(1u << 0)
#define DP_CAP_RECEPTACLE		(1u << 6)
#define DP_CONF_UFP_U_AS_UFP_D	2
#define DP_CONF_SIGNALING_DP	(1u << 2)
#define DP_PIN_C				(1u << 2)
#define DP_PIN_D				(1u << 3)
#define DP_PIN_E				(1u << 4)

// the R pins: PL2 switches VBUS of the port, PL12/PL13 are S_TWI1
#define PIN_BANK_L				0
#define PIN_VBUS				2
#define PIN_TWI1_SCK			12
#define PIN_TWI1_SDA			13
#define PIN_FUNCTION_S_TWI1		3

static const bigtime_t kAttachDebounce = 150000;
static const bigtime_t kCapabilitiesInterval = 150000;
static const int32 kCapabilitiesCount = 50;
static const bigtime_t kResponseTimeout = 300000;
static const int32 kVdmRetries = 3;

static bigtime_t sAttachedAt = 0;

#ifdef TRACE_TYPEC
static void
_Trace(const char* format, ...)
{
	char buffer[256];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	dprintf("sunxi_display: typec: +%" B_PRId64 " ms: %s",
		(system_time() - sAttachedAt) / 1000, buffer);
}
#endif


using namespace sunxi;


TypeCPort::TypeCPort()
	:
	fInitialized(false),
	fState(kStateUnattached),
	fStateSince(0),
	fLastSend(0),
	fRetries(0),
	fTxMessageId(0),
	fRxMessageId(-1),
	fFlipped(false),
	fVconn(false),
	fDpModeVdo(0),
	fDpModePosition(1),
	fPinAssignments(0),
	fPinAssignment(0),
	fHotPlug(false),
	fHotPlugIrq(false),
	fChanges(0),
	fVendorId(0),
	fProductId(0),
	fPinPreference(0)
{
}


TypeCPort::~TypeCPort()
{
	if (fInitialized)
		_Detach();
}


status_t
TypeCPort::Init()
{
	r_pin_set_function(PIN_BANK_L, PIN_TWI1_SCK, PIN_FUNCTION_S_TWI1);
	r_pin_set_function(PIN_BANK_L, PIN_TWI1_SDA, PIN_FUNCTION_S_TWI1);
	// VBUS stays off until something is attached
	r_pin_set(PIN_BANK_L, PIN_VBUS, false);
	r_pin_set_function(PIN_BANK_L, PIN_VBUS, PIN_FUNCTION_OUTPUT);

	// 400 kHz as the BSP runs it: a sink expects the answer to its Request
	// within tSenderResponse (24 ms), and a register read takes 48 clocks
	status_t status = fTwi.Init(A733_R_TWI1_BASE, A733_R_CCU_TWI_GATE,
		A733_R_CCU_TWI1_BIT, Twi::kFastClock);
	if (status != B_OK)
		return status;

	uint16 vendor = 0, product = 0;
	status = fTwi.Read16(HUSB311_ADDRESS, TCPC_VENDOR_ID, vendor);
	if (status == B_OK)
		status = fTwi.Read16(HUSB311_ADDRESS, TCPC_PRODUCT_ID, product);
	if (status != B_OK) {
		ERROR("no port controller answers at %#x: %s\n", HUSB311_ADDRESS,
			strerror(status));
		return status;
	}
	if (vendor != HUSB311_VENDOR_ID || product != HUSB311_PRODUCT_ID) {
		ERROR("unexpected port controller %04x:%04x\n", vendor, product);
		return B_DEVICE_NOT_FOUND;
	}

	// The BSP's settings: I2C reset timeout, CC filter, DRP timing (unused,
	// the port is a source), VCONN current limit, auto idle.
	fTwi.Write8(HUSB311_ADDRESS, HUSB311_I2CRST_CTRL, 0x8f);
	fTwi.Write8(HUSB311_ADDRESS, HUSB311_TTCPC_FILTER, 0x0a);
	fTwi.Write8(HUSB311_ADDRESS, HUSB311_DRP_TOGGLE_CYCLE, 0x04);
	fTwi.Write16(HUSB311_ADDRESS, HUSB311_DRP_DUTY_CTRL, 330);
	fTwi.Write8(HUSB311_ADDRESS, HUSB311_VCONN_CLIMITEN, 0x01);
	fTwi.Write8(HUSB311_ADDRESS, HUSB311_IDLE_CTRL, HUSB311_IDLE_SET(1));

#ifdef TRACE_TYPEC
	// how fast the bus is: a 16-bit register read is about 48 SCL periods
	bigtime_t start = system_time();
	for (int i = 0; i < 16; i++)
		fTwi.Read16(HUSB311_ADDRESS, TCPC_VENDOR_ID, vendor);
	TRACE("16 register reads took %" B_PRId64 " us\n", system_time() - start);
#endif

	// Lab setting: "typec_pin_assignment C|D|E" prefers that DisplayPort
	// pin assignment when the partner offers it, "none" leaves the
	// partner in USB mode (no DisplayPort)
	void* settings = load_driver_settings("sunxi_display");
	const char* preference = get_driver_parameter(settings,
		"typec_pin_assignment", NULL, NULL);
	if (preference != NULL) {
		if (strcmp(preference, "none") == 0)
			fPinPreference = 'N';
		else if (strlen(preference) == 1 && strchr("CDE", preference[0]))
			fPinPreference = preference[0];
		INFO("pin assignment preference from the settings: %s\n", preference);
	}
	unload_driver_settings(settings);

	fInitialized = true;
	_Detach();
	INFO("HUSB311 port controller ready\n");
	return B_OK;
}


status_t
TypeCPort::_ReadCc(uint8& cc1, uint8& cc2)
{
	uint8 status;
	status_t result = fTwi.Read8(HUSB311_ADDRESS, TCPC_CC_STATUS, status);
	if (result != B_OK)
		return result;
	cc1 = status & 3;
	cc2 = (status >> 2) & 3;
	return B_OK;
}


/*!	Back to the unattached source: Rp on both CC lines, no VBUS, no VCONN,
	no PD reception.
*/
void
TypeCPort::_Detach()
{
	r_pin_set(PIN_BANK_L, PIN_VBUS, false);
	fTwi.Write8(HUSB311_ADDRESS, TCPC_POWER_CTRL, 0);
	fTwi.Write8(HUSB311_ADDRESS, TCPC_RX_DETECT, 0);
	fTwi.Write8(HUSB311_ADDRESS, TCPC_ROLE_CTRL,
		ROLE_RP_1_5 | ROLE_CC_RP << 2 | ROLE_CC_RP);
	fTwi.Write16(HUSB311_ADDRESS, TCPC_ALERT, 0xffff);
	fTwi.Write8(HUSB311_ADDRESS, HUSB311_IDLE_CTRL, HUSB311_IDLE_SET(1));

	if (fHotPlug || fState >= kStateDisplayPort)
		atomic_add(&fChanges, 1);
	fVconn = false;
	fHotPlug = false;
	fHotPlugIrq = false;
	fPinAssignment = 0;
	fPinAssignments = 0;
	fTxMessageId = 0;
	fRxMessageId = -1;
	_SetState(kStateUnattached);
}


void
TypeCPort::_Attach(bool flipped, bool vconn)
{
	fFlipped = flipped;
	sAttachedAt = system_time();
	INFO("attached (%s CC%d%s), sourcing VBUS\n", flipped ? "flipped" : "normal",
		flipped ? 2 : 1, vconn ? ", VCONN" : "");

	// Rp stays on the partner's CC line only; VCONN, if any, goes out on
	// the other one (the orientation bit), as Linux' tcpci does it
	fTwi.Write8(HUSB311_ADDRESS, TCPC_ROLE_CTRL, ROLE_RP_1_5
		| (flipped ? (ROLE_CC_RP << 2 | ROLE_CC_OPEN)
			: (ROLE_CC_OPEN << 2 | ROLE_CC_RP)));
	fTwi.Write8(HUSB311_ADDRESS, TCPC_TCPC_CTRL,
		flipped ? TCPC_CTRL_ORIENTATION : 0);
	fTwi.Write8(HUSB311_ADDRESS, TCPC_CONFIG_STD_OUTPUT, flipped ? 1 : 0);

	r_pin_set(PIN_BANK_L, PIN_VBUS, true);
	if (vconn) {
		// no auto idle while VCONN is sourced (the BSP's set_vconn)
		fTwi.Write8(HUSB311_ADDRESS, HUSB311_IDLE_CTRL, HUSB311_IDLE_SET(0));
		fTwi.Write8(HUSB311_ADDRESS, TCPC_POWER_CTRL, POWER_VCONN_ENABLE);
	}
	fVconn = vconn;

	// source, DFP, PD 2.0
	fTwi.Write8(HUSB311_ADDRESS, TCPC_MSG_HDR_INFO, 1 | 1 << 1 | 1 << 3);
	fTwi.Write16(HUSB311_ADDRESS, TCPC_ALERT, 0xffff);
	fTwi.Write8(HUSB311_ADDRESS, TCPC_RX_DETECT,
		RX_DETECT_SOP | RX_DETECT_HARD_RESET);
	fTxMessageId = 0;
	fRxMessageId = -1;
	fRetries = 0;
	_SetState(kStateSendCapabilities);
}


void
TypeCPort::_SetState(State state)
{
	fState = state;
	fStateSince = system_time();
	fLastSend = 0;
}


status_t
TypeCPort::_Transmit(uint8 type, const uint32* objects, int count,
	bool isData)
{
	uint16 header = type | PD_HEADER_DATA_ROLE_DFP | PD_HEADER_REV20
		| PD_HEADER_POWER_SOURCE | (fTxMessageId & 7) << 9
		| (isData ? (count & 7) << 12 : 0);

	fTwi.Write16(HUSB311_ADDRESS, TCPC_ALERT,
		ALERT_TX_SUCCESS | ALERT_TX_FAILED | ALERT_TX_DISCARDED);
	fTwi.Write8(HUSB311_ADDRESS, TCPC_TX_BYTE_CNT, 2 + 4 * count);
	fTwi.Write16(HUSB311_ADDRESS, TCPC_TX_HDR, header);
	if (count > 0) {
		uint8 data[28];
		for (int i = 0; i < count; i++) {
			data[4 * i] = objects[i];
			data[4 * i + 1] = objects[i] >> 8;
			data[4 * i + 2] = objects[i] >> 16;
			data[4 * i + 3] = objects[i] >> 24;
		}
		fTwi.Write(HUSB311_ADDRESS, TCPC_TX_DATA, data, 4 * count);
	}
	status_t status = fTwi.Write8(HUSB311_ADDRESS, TCPC_TRANSMIT,
		TRANSMIT_RETRY_3 | TRANSMIT_SOP);
	if (status != B_OK)
		return status;

	bigtime_t timeout = system_time() + 50000;
	while (system_time() < timeout) {
		uint16 alert = 0;
		if (fTwi.Read16(HUSB311_ADDRESS, TCPC_ALERT, alert) != B_OK)
			continue;
		uint16 done = alert
			& (ALERT_TX_SUCCESS | ALERT_TX_FAILED | ALERT_TX_DISCARDED);
		if (done != 0) {
			fTwi.Write16(HUSB311_ADDRESS, TCPC_ALERT, done);
			if ((done & ALERT_TX_SUCCESS) != 0) {
				fTxMessageId = (fTxMessageId + 1) & 7;
				TRACE("sent %s %u (%d objects)\n", isData ? "data" : "control",
					type, count);
				return B_OK;
			}
			TRACE("sending %s %u: %s\n", isData ? "data" : "control", type,
				(done & ALERT_TX_DISCARDED) != 0 ? "discarded" : "no GoodCRC");
			return (done & ALERT_TX_DISCARDED) != 0 ? B_BUSY : B_IO_ERROR;
		}
		snooze(500);
	}
	TRACE("sending %s %u: no TX result\n", isData ? "data" : "control", type);
	return B_TIMED_OUT;
}


status_t
TypeCPort::_SendVdm(uint32 header, const uint32* vdos, int count)
{
	uint32 objects[7];
	objects[0] = header;
	for (int i = 0; i < count && i < 6; i++)
		objects[i + 1] = vdos[i];
	fLastSend = system_time();
	return _Transmit(PD_DATA_VENDOR_DEFINED, objects, count + 1, true);
}


bool
TypeCPort::_Receive(Message& message)
{
	uint8 count = 0;
	status_t countStatus = fTwi.Read8(HUSB311_ADDRESS, TCPC_RX_BYTE_CNT, count);
	if (countStatus != B_OK) {
		TRACE("rx: reading the byte count: %s\n", strerror(countStatus));
		return false;
	}
	uint8 frameType = 0;
	status_t typeStatus = fTwi.Read8(HUSB311_ADDRESS, TCPC_RX_BUF_FRAME_TYPE,
		frameType);
	uint16 header = 0;
	status_t headerStatus = fTwi.Read16(HUSB311_ADDRESS, TCPC_RX_HDR, header);

	// the count is one more than the header and data bytes
	int payload = count > 3 ? count - 3 : 0;
	if (payload > 28)
		payload = 28;
	uint8 data[28] = {};
	status_t dataStatus = B_OK;
	if (payload > 0)
		dataStatus = fTwi.Read(HUSB311_ADDRESS, TCPC_RX_DATA, data, payload);
	fTwi.Write16(HUSB311_ADDRESS, TCPC_ALERT, ALERT_RX_STATUS);
	TRACE("rx: count %u, frame type %#x, header %#06x (reads %s/%s/%s)\n",
		count, frameType, header, strerror(typeStatus), strerror(headerStatus),
		strerror(dataStatus));
	if (count < 3 || typeStatus != B_OK || headerStatus != B_OK
		|| dataStatus != B_OK) {
		// nothing (or nothing readable) in the buffer
		return false;
	}

	if ((frameType & 7) != 0) {
		TRACE("rx frame type %#x dropped (header %#06x)\n", frameType, header);
		return false;
	}

	message.header = header;
	for (int i = 0; i < 7; i++) {
		message.objects[i] = i * 4 + 3 < payload
			? (uint32)data[4 * i] | (uint32)data[4 * i + 1] << 8
				| (uint32)data[4 * i + 2] << 16 | (uint32)data[4 * i + 3] << 24
			: 0;
	}
	TRACE("rx %#06x: %#" B_PRIx32 " %#" B_PRIx32 " %#" B_PRIx32 " %#" B_PRIx32
		"\n", header, message.objects[0], message.objects[1],
		message.objects[2], message.objects[3]);
	return true;
}


void
TypeCPort::_HardReset()
{
	INFO("hard reset from the partner (state %d, %" B_PRId64 " ms after attach)\n",
		fState, (system_time() - sAttachedAt) / 1000);
	r_pin_set(PIN_BANK_L, PIN_VBUS, false);
	fTwi.Write8(HUSB311_ADDRESS, TCPC_POWER_CTRL, 0);
	if (fHotPlug || fState >= kStateDisplayPort)
		atomic_add(&fChanges, 1);
	fHotPlug = false;
	// tSrcRecover, then start over as an attached source
	snooze(700000);
	uint8 cc1 = 0, cc2 = 0;
	if (_ReadCc(cc1, cc2) == B_OK && (cc1 == CC_SRC_RD || cc2 == CC_SRC_RD))
		_Attach(cc2 == CC_SRC_RD, fVconn);
	else
		_Detach();
}


void
TypeCPort::_HandleMessage(const Message& message)
{
	int count = message.Count();
	int type = message.Type();
	int id = (message.header >> 9) & 7;

	if (count == 0 && type == PD_CONTROL_SOFT_RESET) {
		INFO("soft reset from the partner\n");
		fTxMessageId = 0;
		fRxMessageId = -1;
		_SendControl(PD_CONTROL_ACCEPT);
		_SetState(kStateSendCapabilities);
		fRetries = 0;
		return;
	}

	// repeated messages (a lost GoodCRC) are only acknowledged
	if (id == fRxMessageId)
		return;
	fRxMessageId = id;

	if (count == 0) {
		switch (type) {
			case PD_CONTROL_GOODCRC:
			case PD_CONTROL_PING:
			case PD_CONTROL_ACCEPT:
			case PD_CONTROL_REJECT:
			case PD_CONTROL_WAIT:
			case PD_CONTROL_PS_RDY:
				break;
			case PD_CONTROL_GET_SOURCE_CAP:
			{
				uint32 pdo = PD_SOURCE_PDO;
				_Transmit(PD_DATA_SOURCE_CAP, &pdo, 1, true);
				_SetState(kStateWaitRequest);
				break;
			}
			default:
				// sink capabilities, swaps and everything newer: no
				_SendControl(PD_CONTROL_REJECT);
				break;
		}
		return;
	}

	switch (type) {
		case PD_DATA_REQUEST:
		{
			uint32 position = (message.objects[0] >> 28) & 7;
			if (position != 1) {
				_SendControl(PD_CONTROL_REJECT);
				break;
			}
			bigtime_t received = system_time();
			status_t accepted = _SendControl(PD_CONTROL_ACCEPT);
			bigtime_t acceptedAt = system_time();
			// VBUS is at 5 V already: tSrcTransition, then ready
			snooze(30000);
			status_t ready = _SendControl(PD_CONTROL_PS_RDY);
			INFO("request %#" B_PRIx32 ": accept %s (%" B_PRId64 " ms after "
				"the request, %" B_PRId64 " ms after attach), PS_RDY %s\n",
				message.objects[0], accepted == B_OK ? "acknowledged"
					: strerror(accepted), (acceptedAt - received) / 1000,
				(acceptedAt - sAttachedAt) / 1000,
				ready == B_OK ? "acknowledged" : strerror(ready));
			if (fState <= kStateWaitRequest) {
				INFO("power contract: 5 V\n");
				fRetries = 0;
				_SetState(kStateDiscoverIdentity);
			}
			break;
		}

		case PD_DATA_VENDOR_DEFINED:
			_HandleVdm(message);
			break;

		default:
			_SendControl(PD_CONTROL_REJECT);
			break;
	}
}


void
TypeCPort::_HandleVdm(const Message& message)
{
	uint32 header = message.objects[0];
	if ((header & VDM_STRUCTURED) == 0)
		return;
	uint32 command = VDM_COMMAND(header);
	uint32 type = VDM_TYPE(header);
	uint32 svid = VDM_SVID(header);

	if (type == VDM_TYPE_REQUEST) {
		if (command == VDM_ATTENTION) {
			if (svid == SVID_DISPLAYPORT && message.Count() >= 2)
				_UpdateDisplayPortStatus(message.objects[1]);
			return;
		}
		// the partner asks us: we have nothing to tell
		uint32 nak = (header & ~(3u << 6)) | VDM_TYPE_NAK << 6;
		_SendVdm(nak, NULL, 0);
		return;
	}

	if (type == VDM_TYPE_BUSY) {
		// ask again on the next timeout
		fLastSend = system_time() - kResponseTimeout + 50000;
		return;
	}

	bool ack = type == VDM_TYPE_ACK;
	switch (command) {
		case VDM_DISCOVER_IDENTITY:
			if (fState != kStateDiscoverIdentity)
				break;
			if (ack && message.Count() >= 2) {
				fVendorId = message.objects[1] & 0xffff;
				if (message.Count() >= 4)
					fProductId = message.objects[3] >> 16;
				INFO("partner %04x:%04x (ID header %#" B_PRIx32 ")\n",
					fVendorId, fProductId, message.objects[1]);
			}
			// whatever the answer, go on: some adapters NAK this
			fRetries = 0;
			_SetState(kStateDiscoverSvids);
			break;

		case VDM_DISCOVER_SVIDS:
		{
			if (fState != kStateDiscoverSvids)
				break;
			bool displayPort = false;
			for (int i = 1; ack && i < message.Count(); i++) {
				uint16 high = message.objects[i] >> 16;
				uint16 low = message.objects[i] & 0xffff;
				if (high == SVID_DISPLAYPORT || low == SVID_DISPLAYPORT)
					displayPort = true;
			}
			if (!displayPort) {
				INFO("the partner has no DisplayPort mode\n");
				_SetState(kStateNoDisplayPort);
				break;
			}
			if (fPinPreference == 'N') {
				INFO("DisplayPort mode left alone (settings)\n");
				_SetState(kStateNoDisplayPort);
				break;
			}
			fRetries = 0;
			_SetState(kStateDiscoverModes);
			break;
		}

		case VDM_DISCOVER_MODES:
		{
			if (fState != kStateDiscoverModes || svid != SVID_DISPLAYPORT)
				break;
			if (!ack || message.Count() < 2) {
				_SetState(kStateNoDisplayPort);
				break;
			}
			// the first mode that can be a DisplayPort sink
			fDpModePosition = 0;
			for (int i = 1; i < message.Count(); i++) {
				uint32 mode = message.objects[i];
				if ((mode & DP_CAP_UFP_D) == 0)
					continue;
				fDpModeVdo = mode;
				fDpModePosition = i;
				// a plug names its pin assignments in the DFP_D field
				fPinAssignments = (mode & DP_CAP_RECEPTACLE) != 0
					? (mode >> 16) & 0xff : (mode >> 8) & 0xff;
				break;
			}
			if (fDpModePosition == 0 || fPinAssignments == 0) {
				INFO("no DisplayPort sink mode\n");
				_SetState(kStateNoDisplayPort);
				break;
			}
			INFO("DisplayPort mode %" B_PRId32 ": %#" B_PRIx32
				", pin assignments %#x\n", fDpModePosition, fDpModeVdo,
				fPinAssignments);
			fRetries = 0;
			_SetState(kStateEnterMode);
			break;
		}

		case VDM_ENTER_MODE:
			if (fState != kStateEnterMode)
				break;
			if (!ack) {
				INFO("the partner refused DisplayPort mode\n");
				_SetState(kStateNoDisplayPort);
				break;
			}
			fRetries = 0;
			_SetState(kStateStatusUpdate);
			break;

		case VDM_DP_STATUS:
			if (fState != kStateStatusUpdate)
				break;
			if (ack && message.Count() >= 2)
				_UpdateDisplayPortStatus(message.objects[1]);
			fRetries = 0;
			_SetState(kStateConfigure);
			break;

		case VDM_DP_CONFIGURE:
			if (fState != kStateConfigure)
				break;
			if (!ack) {
				INFO("the partner refused the configuration\n");
				_SetState(kStateNoDisplayPort);
				break;
			}
			INFO("DisplayPort alt mode, pin assignment %c, %" B_PRIu32
				" lanes, HPD %s\n", fPinAssignment, Lanes(),
				fHotPlug ? "high" : "low");
			_SetState(kStateDisplayPort);
			atomic_add(&fChanges, 1);
			break;
	}
}


void
TypeCPort::_UpdateDisplayPortStatus(uint32 status)
{
	bool hotPlug = (status & DP_STATUS_HPD) != 0;
	if ((status & DP_STATUS_IRQ_HPD) != 0)
		fHotPlugIrq = true;
	if (hotPlug != fHotPlug) {
		fHotPlug = hotPlug;
		INFO("HPD %s (status %#" B_PRIx32 ")\n", hotPlug ? "high" : "low",
			status);
		atomic_add(&fChanges, 1);
	}
}


bool
TypeCPort::HotPlugInterrupt()
{
	bool irq = fHotPlugIrq;
	fHotPlugIrq = false;
	return irq;
}


uint32
TypeCPort::Lanes() const
{
	return fPinAssignment == 'D' ? 2 : 4;
}


char
TypeCPort::PinAssignment() const
{
	return fPinAssignment;
}


/*!	Sends the next request of the discovery and mode entry sequence, again
	after a timeout.
*/
void
TypeCPort::_RequestNext()
{
	bigtime_t now = system_time();
	if (fLastSend != 0 && now - fLastSend < kResponseTimeout)
		return;
	if (fLastSend != 0 && ++fRetries > kVdmRetries) {
		if (fState == kStateDiscoverIdentity) {
			// some adapters never answer this one
			fRetries = 0;
			_SetState(kStateDiscoverSvids);
		} else {
			INFO("no answer to the DisplayPort request (state %d)\n", fState);
			_SetState(kStateNoDisplayPort);
			return;
		}
	}

	uint32 dp = (uint32)SVID_DISPLAYPORT << 16 | VDM_STRUCTURED;
	switch (fState) {
		case kStateDiscoverIdentity:
			_SendVdm((uint32)SVID_PD << 16 | VDM_STRUCTURED
				| VDM_DISCOVER_IDENTITY, NULL, 0);
			break;
		case kStateDiscoverSvids:
			_SendVdm((uint32)SVID_PD << 16 | VDM_STRUCTURED
				| VDM_DISCOVER_SVIDS, NULL, 0);
			break;
		case kStateDiscoverModes:
			_SendVdm(dp | VDM_DISCOVER_MODES, NULL, 0);
			break;
		case kStateEnterMode:
			_SendVdm(dp | VDM_POSITION(fDpModePosition) | VDM_ENTER_MODE,
				NULL, 0);
			break;
		case kStateStatusUpdate:
		{
			uint32 status = DP_STATUS_DFP_D_CONNECTED;
			_SendVdm(dp | VDM_POSITION(fDpModePosition) | VDM_DP_STATUS,
				&status, 1);
			break;
		}
		case kStateConfigure:
		{
			// four lanes if the partner can, else two next to USB; the
			// settings may prefer one
			uint8 pins = 0;
			uint8 preferred = fPinPreference == 'C' ? DP_PIN_C
				: fPinPreference == 'D' ? DP_PIN_D
				: fPinPreference == 'E' ? DP_PIN_E : 0;
			if ((fPinAssignments & preferred) != 0) {
				pins = preferred;
				fPinAssignment = fPinPreference;
			} else if ((fPinAssignments & DP_PIN_C) != 0) {
				pins = DP_PIN_C;
				fPinAssignment = 'C';
			} else if ((fPinAssignments & DP_PIN_E) != 0) {
				pins = DP_PIN_E;
				fPinAssignment = 'E';
			} else if ((fPinAssignments & DP_PIN_D) != 0) {
				pins = DP_PIN_D;
				fPinAssignment = 'D';
			} else {
				INFO("no pin assignment in common (%#x)\n", fPinAssignments);
				_SetState(kStateNoDisplayPort);
				return;
			}
			uint32 configuration = DP_CONF_UFP_U_AS_UFP_D
				| DP_CONF_SIGNALING_DP | (uint32)pins << 8;
			_SendVdm(dp | VDM_POSITION(fDpModePosition) | VDM_DP_CONFIGURE,
				&configuration, 1);
			break;
		}
		default:
			break;
	}
}


void
TypeCPort::Poll()
{
	if (!fInitialized)
		return;

	uint16 alert = 0;
	if (fTwi.Read16(HUSB311_ADDRESS, TCPC_ALERT, alert) != B_OK)
		return;
	if (alert != 0 && fState != kStateUnattached)
		TRACE("alert %#06x (state %d)\n", alert, fState);

	if ((alert & ALERT_RX_HARD_RESET) != 0) {
		fTwi.Write16(HUSB311_ADDRESS, TCPC_ALERT, ALERT_RX_HARD_RESET);
		_HardReset();
		return;
	}

	uint8 cc1 = 0, cc2 = 0;
	if (_ReadCc(cc1, cc2) != B_OK)
		return;
	if ((alert & (ALERT_CC_STATUS | ALERT_POWER_STATUS)) != 0) {
		fTwi.Write16(HUSB311_ADDRESS, TCPC_ALERT,
			alert & (ALERT_CC_STATUS | ALERT_POWER_STATUS));
	}
	bool sink = cc1 == CC_SRC_RD || cc2 == CC_SRC_RD;

	if (fState == kStateUnattached || fState == kStateAttachWait) {
		if (!sink) {
			if (fState == kStateAttachWait)
				_SetState(kStateUnattached);
			return;
		}
		if (fState == kStateUnattached) {
			_SetState(kStateAttachWait);
			return;
		}
		if (system_time() - fStateSince < kAttachDebounce)
			return;
		bool flipped = cc2 == CC_SRC_RD;
		bool vconn = (flipped ? cc1 : cc2) == CC_SRC_RA;
		_Attach(flipped, vconn);
		return;
	}

	// attached: the partner's CC line going open is a detach
	if ((fFlipped ? cc2 : cc1) != CC_SRC_RD) {
		INFO("detached\n");
		_Detach();
		return;
	}

	while ((alert & ALERT_RX_STATUS) != 0) {
		Message message;
		if (_Receive(message))
			_HandleMessage(message);
		if (fTwi.Read16(HUSB311_ADDRESS, TCPC_ALERT, alert) != B_OK)
			break;
	}
	if ((alert & ALERT_RX_OVERFLOW) != 0)
		fTwi.Write16(HUSB311_ADDRESS, TCPC_ALERT, ALERT_RX_OVERFLOW);

	bigtime_t now = system_time();
	switch (fState) {
		case kStateSendCapabilities:
			if (now - fLastSend >= kCapabilitiesInterval) {
				if (fRetries++ >= kCapabilitiesCount) {
					INFO("the partner does not speak Power Delivery: no "
						"DisplayPort\n");
					_SetState(kStateNoPowerDelivery);
					break;
				}
				uint32 pdo = PD_SOURCE_PDO;
				fLastSend = now;
				if (_Transmit(PD_DATA_SOURCE_CAP, &pdo, 1, true) == B_OK)
					_SetState(kStateWaitRequest);
			}
			break;

		case kStateWaitRequest:
			// tSenderResponse; the capabilities go out again, counted
			if (now - fStateSince > 30000)
				_SetState(kStateSendCapabilities);
			break;

		case kStateDiscoverIdentity:
		case kStateDiscoverSvids:
		case kStateDiscoverModes:
		case kStateEnterMode:
		case kStateStatusUpdate:
		case kStateConfigure:
			_RequestNext();
			break;

		default:
			break;
	}
}
