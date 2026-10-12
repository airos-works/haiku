/*
 * Copyright 2008 Oliver Ruiz Dorantes, oliver.ruiz.dorantes_at_gmail.com
 * Copyright 2008 Mika Lindqvist, monni1995_at_gmail.com
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 */

#include "BluetoothServer.h"

#include "CommandManager.h"
#include "Debug.h"
#include "LocalDeviceImpl.h"
#include "bluetooth/HCI/btHCI_command.h"

#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/bluetooth_error.h>
#include <bluetooth/LinkKeyUtils.h>
#include <bluetooth/HCI/btHCI_event.h>

#include <ConnectionIncoming.h>
#include <LELog.h>
#include <PincodeWindow.h>
#include <Alert.h>
#include <ByteOrder.h>
#include <Invoker.h>

#include <FindDirectory.h>
#include <stdio.h>
#include <string.h>
#include <new>


// Classic links, pairing and encryption go to the Bluetooth log (see
// LELog.h): connection problems are otherwise invisible.
using namespace Bluetooth;

#define CLASSIC_LOG(level, format, args...) \
	LELog(level, "classic", format, ##args)


#if 0
#pragma mark - Class methods -
#endif


// Factory methods
LocalDeviceImpl*
LocalDeviceImpl::CreateControllerAccessor(BPath* path)
{
	HCIDelegate* delegate = new(std::nothrow) HCIControllerAccessor(path);
	if (delegate == NULL)
		return NULL;

	LocalDeviceImpl* device = new(std::nothrow) LocalDeviceImpl(delegate);
	if (device == NULL) {
		delete delegate;
		return NULL;
	}

	return device;
}


LocalDeviceImpl*
LocalDeviceImpl::CreateTransportAccessor(BPath* path)
{
	HCIDelegate* delegate = new(std::nothrow) HCITransportAccessor(path);
	if (delegate == NULL)
		return NULL;

	LocalDeviceImpl* device = new(std::nothrow) LocalDeviceImpl(delegate);
	if (device == NULL) {
		delete delegate;
		return NULL;
	}

	return device;
}


LocalDeviceImpl::LocalDeviceImpl(HCIDelegate* hd)
	: LocalDeviceHandler(hd),
	  fPairSetupState(PAIR_SETUP_IDLE),
	  fLELock("LE state"),
	  fLEMaskState(LE_MASKS_NONE),
	  fLEScanState(LE_SCAN_IDLE),
	  fLEScanSince(0),
	  fLECommandSince(0),
	  fClassicInquiryActive(false),
	  fClassicInquirySince(0),
	  fLEConnectionState(LE_CONN_IDLE),
	  fLEConnectionSince(0),
	  fLECancelRequested(false),
	  fLEConnectionAddressType(0),
	  fLEConnectionHandle(0),
	  fLEEncryptionPending(false),
	  fLEEncrypted(false)
{
	memset(fLEConnectionAddress, 0, sizeof(fLEConnectionAddress));
	LoadRemoteDevices();
}


status_t
LocalDeviceImpl::StartPairingSetup()
{
	if (fPairSetupState != PAIR_SETUP_IDLE)
		return B_BUSY;
	// The kernel picks the controller's ACL packet sizes out of these
	// answers; L2CAP fragments and paces its data with them.
	fPairSetupState = PAIR_SETUP_BUFFER_SIZE;
	status_t result = _SendLECommand(
		PACK_OPCODE(OGF_INFORMATIONAL_PARAM, OCF_READ_BUFFER_SIZE), NULL, 0);
	if (result != B_OK)
		fPairSetupState = PAIR_SETUP_IDLE;
	return result;
}


void
LocalDeviceImpl::HandlePairSetupCommandComplete(struct hci_event_header* event)
{
	if (fPairSetupState == PAIR_SETUP_IDLE || event->elen < 4)
		return;
	const uint8* response = (const uint8*)(event + 1);
	// Command Complete carries ncmd, opcode, status; a controller that does
	// not know a command may refuse it in a Command Status instead: status,
	// ncmd, opcode.
	uint16 opcode = response[1] | (response[2] << 8);
	uint8 status = response[3];
	if (event->ecode == HCI_EVENT_CMD_STATUS) {
		opcode = response[2] | (response[3] << 8);
		status = response[0];
		if (status == 0)
			return;
	}
	uint16 expected = 0;
	switch (fPairSetupState) {
		case PAIR_SETUP_BUFFER_SIZE:
			expected = PACK_OPCODE(OGF_INFORMATIONAL_PARAM,
				OCF_READ_BUFFER_SIZE);
			break;
		case PAIR_SETUP_LE_BUFFER_SIZE:
			expected = PACK_OPCODE(OGF_LE_CONTROL, OCF_LE_READ_BUFFER_SIZE);
			break;
		case PAIR_SETUP_EVENT_MASK:
			expected = PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_SET_EVENT_MASK);
			break;
		case PAIR_SETUP_SSP:
			expected = PACK_OPCODE(OGF_CONTROL_BASEBAND,
				OCF_WRITE_SIMPLE_PAIRING_MODE);
			break;
		case PAIR_SETUP_SC:
			expected = PACK_OPCODE(OGF_CONTROL_BASEBAND,
				OCF_WRITE_SC_HOST_SUPPORT);
			break;
		case PAIR_SETUP_LE_HOST:
			expected = PACK_OPCODE(OGF_CONTROL_BASEBAND,
				OCF_WRITE_LE_HOST_SUPPORTED);
			break;
		case PAIR_SETUP_LINK_POLICY:
			expected = PACK_OPCODE(OGF_LINK_POLICY,
				OCF_WRITE_DEFAULT_LINK_POLICY);
			break;
		default:
			return;
	}
	if (opcode != expected)
		return;
	// A controller without LE, or an old one, may not know the buffer size
	// commands; carry on without them.
	if (status != 0 && fPairSetupState != PAIR_SETUP_BUFFER_SIZE
		&& fPairSetupState != PAIR_SETUP_LE_BUFFER_SIZE) {
		printf("Bluetooth pairing setup opcode %#x failed: %#x\n",
			opcode, status);
		fPairSetupState = PAIR_SETUP_IDLE;
		return;
	}
	const uint8 enabled = 1;
	const uint8 leHost[2] = { 1, 1 };
	const uint8 eventMask[8] = {
		0xff, 0xff, 0xfb, 0xff, 0x07, 0xf8, 0xbf, 0x3d
	};
	status_t result = B_OK;
	switch (fPairSetupState) {
		case PAIR_SETUP_BUFFER_SIZE:
			fPairSetupState = PAIR_SETUP_LE_BUFFER_SIZE;
			result = _SendLECommand(PACK_OPCODE(OGF_LE_CONTROL,
				OCF_LE_READ_BUFFER_SIZE), NULL, 0);
			break;
		case PAIR_SETUP_LE_BUFFER_SIZE:
			fPairSetupState = PAIR_SETUP_EVENT_MASK;
			result = _SendLECommand(PACK_OPCODE(OGF_CONTROL_BASEBAND,
				OCF_SET_EVENT_MASK), eventMask, sizeof(eventMask));
			break;
		case PAIR_SETUP_EVENT_MASK:
			fPairSetupState = PAIR_SETUP_SSP;
			result = _SendLECommand(PACK_OPCODE(OGF_CONTROL_BASEBAND,
				OCF_WRITE_SIMPLE_PAIRING_MODE), &enabled, 1);
			break;
		case PAIR_SETUP_SSP:
			fPairSetupState = PAIR_SETUP_SC;
			result = _SendLECommand(PACK_OPCODE(OGF_CONTROL_BASEBAND,
				OCF_WRITE_SC_HOST_SUPPORT), &enabled, 1);
			break;
		case PAIR_SETUP_SC:
			fPairSetupState = PAIR_SETUP_LE_HOST;
			result = _SendLECommand(PACK_OPCODE(OGF_CONTROL_BASEBAND,
				OCF_WRITE_LE_HOST_SUPPORTED), leHost, sizeof(leHost));
			break;
		case PAIR_SETUP_LE_HOST:
		{
			// Role switch and sniff mode for new links (both are off after
			// a reset): a keyboard that cannot enter sniff mode is polled
			// all the time, and the radio it shares with an LE mouse (and
			// Wi-Fi) has that much less time for them.
			printf("Bluetooth SSP, Secure Connections and LE host enabled\n");
			fPairSetupState = PAIR_SETUP_LINK_POLICY;
			const uint8 policy[2] = {
				HCI_LINK_POLICY_ROLE_SWITCH | HCI_LINK_POLICY_SNIFF, 0 };
			result = _SendLECommand(PACK_OPCODE(OGF_LINK_POLICY,
				OCF_WRITE_DEFAULT_LINK_POLICY), policy, sizeof(policy));
			break;
		}
		case PAIR_SETUP_LINK_POLICY:
			CLASSIC_LOG(LE_LOG_INFO, "default link policy: role switch, sniff");
			fPairSetupState = PAIR_SETUP_IDLE;
			if (((BluetoothServer*)be_app)->GetHIDHost()->HasDevices())
				EnablePageScan();
			break;
		default:
			break;
	}
	if (result != B_OK) {
		printf("Bluetooth pairing setup command failed to send: %ld\n",
			(long)result);
		fPairSetupState = PAIR_SETUP_IDLE;
	}
}


LocalDeviceImpl::~LocalDeviceImpl()
{
	SaveRemoteDevices();
}


void
LocalDeviceImpl::Unregister()
{
	BMessage* msg =	new	BMessage(BT_MSG_REMOVE_DEVICE);

	msg->AddInt32("hci_id", fHCIDelegate->Id());

	TRACE_BT("LocalDeviceImpl: Unregistering %" B_PRId32 "\n",
		fHCIDelegate->Id());

	be_app_messenger.SendMessage(msg);
}


void
LocalDeviceImpl::SaveRemoteDevices()
{
	BMessage devices;

	for (int32 i = 0; i < fRemoteDevicesList.CountItems(); i++) {
		ServerRemoteDevice* rd = fRemoteDevicesList.ItemAt(i);
		BMessage device;
		bdaddr_t bdaddr = rd->bdaddr;
		linkkey_t link_key = rd->link_key;
		device.AddData("bdaddr", B_ANY_TYPE, &bdaddr, sizeof(bdaddr_t));
		device.AddString("name", rd->friendly_name);
		device.AddUInt16("clock_offset", rd->clock_offset);
		device.AddUInt8("pscan_rep_mode", rd->pscan_rep_mode);
		device.AddData("class_of_device", B_RAW_TYPE, rd->classOfDevice,
			sizeof(rd->classOfDevice));
		device.AddData("link key", B_ANY_TYPE, &link_key, sizeof(linkkey_t));
		device.AddUInt8("key type", rd->key_type);
		device.AddUInt8("link type", rd->link_type);

		devices.AddMessage("remote", &device);
	}

	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK) {
		path.Append("Bluetooth_paired_devices");
		BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
		if (file.InitCheck()==B_OK)
			devices.Flatten(&file);
	}
}


void
LocalDeviceImpl::LoadRemoteDevices()
{
	BPath path;
	BMessage devices;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK) {
		path.Append("Bluetooth_paired_devices");
		BFile file(path.Path(), B_READ_ONLY);
		if (file.InitCheck()==B_OK)
			devices.Unflatten(&file);
	}

	BMessage device;
	for (int32 i = 0; devices.FindMessage("remote", i, &device) == B_OK; i++) {
		const void* data;
		ssize_t size;
		if (device.FindData("bdaddr", B_ANY_TYPE, &data, &size) != B_OK
			|| size != sizeof(bdaddr_t))
			continue;
		bdaddr_t bdaddr;
		memcpy(&bdaddr, data, sizeof(bdaddr));
		if (RemoteDeviceByAddr(bdaddr) != NULL)
			continue;

		ServerRemoteDevice* rd = new ServerRemoteDevice();
		rd->bdaddr = bdaddr;
		device.FindString("name", &rd->friendly_name);
		device.FindUInt16("clock_offset", &rd->clock_offset);
		device.FindUInt8("pscan_rep_mode", &rd->pscan_rep_mode);
		if (device.FindData("class_of_device", B_RAW_TYPE, &data, &size)
				== B_OK && size == sizeof(rd->classOfDevice))
			memcpy(rd->classOfDevice, data, sizeof(rd->classOfDevice));
		rd->link_key = LinkKeyUtils::NullKey();
		if (device.FindData("link key", B_ANY_TYPE, &data, &size) == B_OK
			&& size == sizeof(linkkey_t))
			memcpy(&rd->link_key, data, sizeof(linkkey_t));
		device.FindUInt8("key type", &rd->key_type);
		device.FindUInt8("link type", &rd->link_type);
		rd->conn_state = RemoteDevice::DISCONNECTED;

		fRemoteDevicesList.AddItem(rd);
	}
}


ServerRemoteDevice*
LocalDeviceImpl::RemoteDeviceByAddr(bdaddr_t bdaddr)
{
	for (int32 i = 0; i < fRemoteDevicesList.CountItems(); i++) {
		ServerRemoteDevice* rd = fRemoteDevicesList.ItemAt(i);
		if (bdaddrUtils::Compare(rd->bdaddr, bdaddr))
			return rd;
	}
	return NULL;
}


ServerRemoteDevice*
LocalDeviceImpl::RemoteDeviceByHandle(uint16 handle)
{
	for (int32 i = 0; i < fRemoteDevicesList.CountItems(); i++) {
		ServerRemoteDevice* rd = fRemoteDevicesList.ItemAt(i);
		if (rd->handle == handle)
			return rd;
	}
	return NULL;
}


void
LocalDeviceImpl::AddRemoteDevice(ServerRemoteDevice* rd)
{
	fRemoteDevicesList.AddItem(rd);
}


void
LocalDeviceImpl::RemoveRemoteDevice(ServerRemoteDevice* rd)
{
	if (rd == NULL)
		return;
	fRemoteDevicesList.RemoveItem(rd);
	SaveRemoteDevices();
		// a forgotten device must not come back with its key
}


RemoteDevicesList*
LocalDeviceImpl::GetRemoteDevicesList()
{
	return &fRemoteDevicesList;
}


#if 0
#pragma mark - Event handling methods -
#endif

template<typename T, typename Header>
inline T*
JumpEventHeader(Header* event)
{
	return (T*)(event + 1);
}


void
LocalDeviceImpl::HandleUnexpectedEvent(struct hci_event_header* event)
{
	// Events here might have not been initated by us
	// TODO: ML mark as handled pass a reply by parameter and reply in common
	switch (event->ecode) {
		case HCI_EVENT_CMD_COMPLETE:
			HandlePairSetupCommandComplete(event);
			HandleLECommandComplete(event);
			break;

		case HCI_EVENT_CMD_STATUS:
			HandlePairSetupCommandComplete(event);
			HandleLECommandStatus(event);
			break;

		case HCI_EVENT_LE_META:
			HandleLEMeta(event);
			break;

		case 0x30:
			// Encryption Key Refresh Complete: status, handle.
			if (event->elen >= 3) {
				const uint8* payload = (const uint8*)(event + 1);
				HandleLEEncryptionChange(payload[1] | (payload[2] << 8),
					payload[0], true);
			}
			break;

		case HCI_EVENT_HARDWARE_ERROR:
			HardwareError(
				JumpEventHeader<struct hci_ev_hardware_error>(event));
			break;

		case HCI_EVENT_CONN_REQUEST:
			ConnectionRequest(
				JumpEventHeader<struct hci_ev_conn_request>(event), NULL);
			break;

		case HCI_EVENT_NUM_COMP_PKTS:
			NumberOfCompletedPackets(
				JumpEventHeader<struct hci_ev_num_comp_pkts>(event));
			break;

		case HCI_EVENT_DISCONNECTION_COMPLETE:
			DisconnectionComplete(
				JumpEventHeader<struct hci_ev_disconnection_complete_reply>(event));
			break;

		case HCI_EVENT_CONN_COMPLETE:
			ConnectionComplete(JumpEventHeader<struct hci_ev_conn_complete>(event));
			break;

		case HCI_EVENT_ENCRYPT_CHANGE:
			if (event->elen >= sizeof(hci_ev_encrypt_change))
				EncryptChange(JumpEventHeader<struct hci_ev_encrypt_change>(event));
			break;

		// Pairing and authentication events come whenever the controller or
		// the peer needs them, not only after a request of ours that
		// registered for them; answer them anyway, or the controller waits
		// and the pairing (and the link key) is lost.
		case HCI_EVENT_LINK_KEY_REQ:
			LinkKeyRequested(
				JumpEventHeader<struct hci_ev_link_key_req>(event), NULL);
			break;

		case HCI_EVENT_LINK_KEY_NOTIFY:
			LinkKeyNotify(
				JumpEventHeader<struct hci_ev_link_key_notify>(event), NULL);
			break;

		case HCI_EVENT_IO_CAPABILITY_REQUEST:
			IOCapabilityRequest(JumpEventHeader
				<struct hci_ev_io_capability_request>(event), NULL);
			break;

		case HCI_EVENT_IO_CAPABILITY_RESPONSE:
			IOCapabilityResponse(JumpEventHeader
				<struct hci_ev_io_capability_response>(event), NULL);
			break;

		case HCI_EVENT_USER_CONFIRMATION_REQUEST:
			UserConfirmationRequest(JumpEventHeader
				<struct hci_ev_user_confirmation_request>(event), NULL);
			break;

		case HCI_EVENT_USER_PASSKEY_NOTIFICATION:
			if (event->elen >= sizeof(hci_ev_user_passkey_notification)) {
				UserPasskeyNotification(JumpEventHeader
					<struct hci_ev_user_passkey_notification>(event));
			}
			break;

		case HCI_EVENT_MODE_CHANGE:
			if (event->elen >= sizeof(hci_ev_mode_change))
				ModeChange(JumpEventHeader<struct hci_ev_mode_change>(event));
			break;

		case HCI_EVENT_KEYPRESS_NOTIFICATION:
			if (event->elen >= sizeof(hci_ev_keypress_notification)) {
				KeypressNotification(JumpEventHeader
					<struct hci_ev_keypress_notification>(event));
			}
			break;

		case HCI_EVENT_SIMPLE_PAIRING_COMPLETE:
			SimplePairingComplete(JumpEventHeader
				<struct hci_ev_simple_pairing_complete>(event), NULL);
			break;

		case HCI_EVENT_AUTH_COMPLETE:
			AuthComplete(
				JumpEventHeader<struct hci_ev_auth_complete>(event), NULL);
			break;

		case HCI_EVENT_PIN_CODE_REQ:
			PinCodeRequest(
				JumpEventHeader<struct hci_ev_pin_code_req>(event), NULL);
			break;

		default:
			TRACE_BT("Couldn't handle the unexpected event with code: %x", event->ecode);
			break;
	}
}


void
LocalDeviceImpl::HandleExpectedRequest(struct hci_event_header* event,
	BMessage* request)
{
	// we are waiting for a reply
	switch (event->ecode) {
		case HCI_EVENT_INQUIRY_COMPLETE:
			InquiryComplete(JumpEventHeader<uint8>(event), request);
			break;

		case HCI_EVENT_INQUIRY_RESULT:
			InquiryResult(JumpEventHeader<uint8>(event), request);
			break;

		case HCI_EVENT_AUTH_COMPLETE:
			AuthComplete(JumpEventHeader<struct hci_ev_auth_complete>(event), request);
			break;

		case HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE:
			RemoteNameRequestComplete(
				JumpEventHeader
					<struct hci_ev_remote_name_request_complete_reply>
				(event), request);
			break;

		case HCI_EVENT_CHANGE_CONN_LINK_KEY_COMPLETE:
			break;

		case HCI_EVENT_MASTER_LINK_KEY_COMPL:
			break;

		case HCI_EVENT_RMT_FEATURES:
			break;

		case HCI_EVENT_RMT_VERSION:
			break;

		case HCI_EVENT_QOS_SETUP_COMPLETE:
			break;

		case HCI_EVENT_FLUSH_OCCUR:
			break;

		case HCI_EVENT_ROLE_CHANGE:
			RoleChange(
				JumpEventHeader<struct hci_ev_role_change>(event), request);
			break;

		case HCI_EVENT_MODE_CHANGE:
			ModeChange(JumpEventHeader<struct hci_ev_mode_change>(event));
			break;

		case HCI_EVENT_RETURN_LINK_KEYS:
			ReturnLinkKeys(
				JumpEventHeader<struct hci_ev_return_link_keys>(event));
			break;

		case HCI_EVENT_LINK_KEY_REQ:
			LinkKeyRequested(
				JumpEventHeader<struct hci_ev_link_key_req>(event), request);
			break;

		case HCI_EVENT_LINK_KEY_NOTIFY:
			LinkKeyNotify(
				JumpEventHeader<struct hci_ev_link_key_notify>(event), request);
			break;

		case HCI_EVENT_LOOPBACK_COMMAND:
			break;

		case HCI_EVENT_DATA_BUFFER_OVERFLOW:
			break;

		case HCI_EVENT_MAX_SLOT_CHANGE:
			MaxSlotChange(JumpEventHeader<struct hci_ev_max_slot_change>(event),
			request);
			break;

		case HCI_EVENT_READ_CLOCK_OFFSET_COMPL:
			break;

		case HCI_EVENT_CON_PKT_TYPE_CHANGED:
			break;

		case HCI_EVENT_QOS_VIOLATION:
			break;

		case HCI_EVENT_PAGE_SCAN_REP_MODE_CHANGE:
			PageScanRepetitionModeChange(
				JumpEventHeader<struct hci_ev_page_scan_rep_mode_change>(event),
				request);
			break;

		case HCI_EVENT_FLOW_SPECIFICATION:
			break;

		case HCI_EVENT_INQUIRY_RESULT_WITH_RSSI:
			InquiryResultWithRSSI(JumpEventHeader<uint8>(event), request);
			break;
		case HCI_EVENT_EXTENDED_INQUIRY_RESULT:
			ExtendedInquiryResult(JumpEventHeader<uint8>(event), request);
			break;

		case HCI_EVENT_REMOTE_EXTENDED_FEATURES:
			break;

		case HCI_EVENT_SYNCHRONOUS_CONNECTION_COMPLETED:
			break;

		case HCI_EVENT_SYNCHRONOUS_CONNECTION_CHANGED:
			break;

		case HCI_EVENT_IO_CAPABILITY_REQUEST:
			IOCapabilityRequest(JumpEventHeader<struct hci_ev_io_capability_request>(event),
				request);
			break;

		case HCI_EVENT_IO_CAPABILITY_RESPONSE:
			IOCapabilityResponse(JumpEventHeader<struct hci_ev_io_capability_response>(event),
				request);
			break;

		case HCI_EVENT_USER_CONFIRMATION_REQUEST:
			UserConfirmationRequest(JumpEventHeader<struct hci_ev_user_confirmation_request>(event),
				request);
			break;

		case HCI_EVENT_SIMPLE_PAIRING_COMPLETE:
			SimplePairingComplete(JumpEventHeader<struct hci_ev_simple_pairing_complete>(event),
				request);
			break;

		case HCI_EVENT_PIN_CODE_REQ:
			PinCodeRequest(JumpEventHeader<struct hci_ev_pin_code_req>(event), NULL);
			break;
	}
}


void
LocalDeviceImpl::HandleEvent(struct hci_event_header* event)
{
/*
	printf("### Incoming event: len = %d\n", event->elen);
	for (int16 index = 0; index < event->elen + 2; index++) {
		printf("%x:", ((uint8*)event)[index]);
	}
	printf("### \n");
*/
	BMessage* request = NULL;
	int32 eventIndexLocation;

	// HCI command flow control: both events report how many commands the
	// controller accepts now.
	const uint8* payload = (const uint8*)(event + 1);
	if (event->ecode == HCI_EVENT_CMD_COMPLETE && event->elen >= 3) {
		fHCIDelegate->CommandCredits(payload[0]);
		if ((payload[1] | (payload[2] << 8))
				== PACK_OPCODE(OGF_LINK_CONTROL, OCF_INQUIRY_CANCEL))
			LEInquiryState(false);
	} else if (event->ecode == HCI_EVENT_CMD_STATUS && event->elen >= 4) {
		fHCIDelegate->CommandCredits(payload[1]);
		if ((payload[2] | (payload[3] << 8))
				== PACK_OPCODE(OGF_LINK_CONTROL, OCF_INQUIRY))
			LEInquiryState(payload[0] == 0);
	} else if (event->ecode == HCI_EVENT_INQUIRY_COMPLETE)
		LEInquiryState(false);

	// Check if it is a requested one
	switch (event->ecode) {
		case HCI_EVENT_CMD_COMPLETE:
		{
			struct hci_ev_cmd_complete* commandComplete
				= JumpEventHeader<struct hci_ev_cmd_complete>(event);

			TRACE_BT("LocalDeviceImpl: Incoming CommandComplete(%d) for %s\n", commandComplete->ncmd,
				BluetoothCommandOpcode(commandComplete->opcode));

			request = FindPetition(event->ecode, commandComplete->opcode,
				&eventIndexLocation);

			if (request != NULL)
				CommandComplete(commandComplete, request, eventIndexLocation);

			break;
		}
		case HCI_EVENT_CMD_STATUS:
		{
			struct hci_ev_cmd_status* commandStatus
				= JumpEventHeader<struct hci_ev_cmd_status>(event);

			// A controller may reject a command before its expected completion.
			// Finish that request so callers do not wait forever for an event
			// that the controller will never send.
			if (commandStatus->status != BT_OK) {
				CLASSIC_LOG(LE_LOG_ERROR, "the controller refused command %#x: "
					"status %#x", commandStatus->opcode, commandStatus->status);
				request = FindPetition(HCI_EVENT_CMD_COMPLETE,
					commandStatus->opcode, &eventIndexLocation);
				if (request != NULL) {
					BMessage reply;
					reply.AddUInt8("status", commandStatus->status);
					request->SendReply(&reply);
					ClearWantedEvent(request);
					break;
				}
			}

			TRACE_BT("LocalDeviceImpl: Incoming CommandStatus(%d)(%s) for %s\n", commandStatus->ncmd,
				BluetoothError(commandStatus->status),
				BluetoothCommandOpcode(commandStatus->opcode));

			request = FindPetition(event->ecode, commandStatus->opcode,
				&eventIndexLocation);
			if (request != NULL) {
				CommandStatus(commandStatus, request, eventIndexLocation);
				break;
			}

			// A controller that will not run a command at all refuses it
			// here, and then says nothing further about it - there will be
			// no Command Complete. Whoever asked for one is waiting for an
			// answer that is never coming, so hand them the refusal and let
			// the request go. Not every controller implements every command,
			// and one that does not should not be able to wedge its caller.
			if (commandStatus->status != BT_OK) {
				request = FindPetition(HCI_EVENT_CMD_COMPLETE,
					commandStatus->opcode, &eventIndexLocation);

				if (request != NULL) {
					TRACE_BT("LocalDeviceImpl: %s refused, failing the"
						" request waiting on it\n",
						BluetoothCommandOpcode(commandStatus->opcode));

					BMessage refusal;
					refusal.AddUInt8("status", commandStatus->status);
					request->SendReply(&refusal);

					ClearWantedEvent(request, HCI_EVENT_CMD_COMPLETE,
						commandStatus->opcode);
				}
			}

			break;
		}
		default:
			TRACE_BT("LocalDeviceImpl: Incoming %s event\n", BluetoothEvent(event->ecode));

			request = FindPetition(event->ecode);
			if (request != NULL)
				HandleExpectedRequest(event, request);

			break;
	}

	if (request == NULL) {
		TRACE_BT("LocalDeviceImpl: Event %s could not be understood or delivered\n",
			BluetoothEvent(event->ecode));
		HandleUnexpectedEvent(event);
	}
}


#if 0
#pragma mark -
#endif


void
LocalDeviceImpl::CommandComplete(struct hci_ev_cmd_complete* event,
	BMessage* request, int32 index)
{
	int16 opcodeExpected;
	BMessage reply;
	status_t status;

	// Handle command complete information
	request->FindInt16("opcodeExpected", index, &opcodeExpected);

	if (request->IsSourceWaiting() == false) {
		TRACE_BT("LocalDeviceImpl: Nobody waiting for the event\n");
	}

	switch ((uint16)opcodeExpected) {

		case PACK_OPCODE(OGF_INFORMATIONAL_PARAM, OCF_READ_LOCAL_VERSION):
		{
			struct hci_rp_read_loc_version* version
				= JumpEventHeader<struct hci_rp_read_loc_version,
				struct hci_ev_cmd_complete>(event);


			if (version->status == BT_OK) {

				if (!IsPropertyAvailable("hci_version"))
					fProperties->AddInt8("hci_version", version->hci_version);

				if (!IsPropertyAvailable("hci_revision")) {
					fProperties->AddInt16("hci_revision",
						version->hci_revision);
				}

				if (!IsPropertyAvailable("lmp_version"))
					fProperties->AddInt8("lmp_version", version->lmp_version);

				if (!IsPropertyAvailable("lmp_subversion")) {
					fProperties->AddInt16("lmp_subversion",
						version->lmp_subversion);
				}

				if (!IsPropertyAvailable("manufacturer")) {
					fProperties->AddInt16("manufacturer",
						version->manufacturer);
				}
			}

			TRACE_BT("LocalDeviceImpl: Reply for Local Version %x\n", version->status);

			reply.AddUInt8("status", version->status);
			status = request->SendReply(&reply);
			//printf("Sending reply... %ld\n", status);
			// debug reply.PrintToStream();

			// This request is not gonna be used anymore
			ClearWantedEvent(request);
			break;
		}

		case  PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_READ_PG_TIMEOUT):
		{
			struct hci_rp_read_page_timeout* pageTimeout
				= JumpEventHeader<struct hci_rp_read_page_timeout,
				struct hci_ev_cmd_complete>(event);

			if (pageTimeout->status == BT_OK) {
				fProperties->AddInt16("page_timeout",
					pageTimeout->page_timeout);

				TRACE_BT("LocalDeviceImpl: Page Timeout=%x\n", pageTimeout->page_timeout);
			}

			reply.AddUInt8("status", pageTimeout->status);
			reply.AddInt32("result", pageTimeout->page_timeout);
			status = request->SendReply(&reply);
			//printf("Sending reply... %ld\n", status);
			// debug reply.PrintToStream();

			// This request is not gonna be used anymore
			ClearWantedEvent(request);
			break;
		}

		case PACK_OPCODE(OGF_INFORMATIONAL_PARAM, OCF_READ_LOCAL_FEATURES):
		{
			struct hci_rp_read_loc_features* features
				= JumpEventHeader<struct hci_rp_read_loc_features,
				struct hci_ev_cmd_complete>(event);

			if (features->status == BT_OK) {

				if (!IsPropertyAvailable("features")) {
					fProperties->AddData("features", B_ANY_TYPE,
						&features->features, 8);

					uint16 packetType = HCI_DM1 | HCI_DH1 | HCI_HV1;

					bool roleSwitch
						= (features->features[0] & LMP_RSWITCH) != 0;
					bool encryptCapable
						= (features->features[0] & LMP_ENCRYPT) != 0;

					if (features->features[0] & LMP_3SLOT)
						packetType |= (HCI_DM3 | HCI_DH3);

					if (features->features[0] & LMP_5SLOT)
						packetType |= (HCI_DM5 | HCI_DH5);

					if (features->features[1] & LMP_HV2)
						packetType |= (HCI_HV2);

					if (features->features[1] & LMP_HV3)
						packetType |= (HCI_HV3);

					fProperties->AddInt16("packet_type", packetType);
					fProperties->AddBool("role_switch_capable", roleSwitch);
					fProperties->AddBool("encrypt_capable", encryptCapable);

					TRACE_BT("LocalDeviceImpl: Packet type %x role switch %d encrypt %d\n",
						packetType, roleSwitch, encryptCapable);
				}

			}

			TRACE_BT("LocalDeviceImpl: Reply for Local Features %x\n", features->status);

			reply.AddUInt8("status", features->status);
			status = request->SendReply(&reply);
			//printf("Sending reply... %ld\n", status);
			// debug reply.PrintToStream();

			// This request is not gonna be used anymore
			ClearWantedEvent(request);
			break;
		}

		case PACK_OPCODE(OGF_INFORMATIONAL_PARAM, OCF_READ_BUFFER_SIZE):
		{
			struct hci_rp_read_buffer_size* buffer
				= JumpEventHeader<struct hci_rp_read_buffer_size,
				struct hci_ev_cmd_complete>(event);

			if (buffer->status == BT_OK) {

				if (!IsPropertyAvailable("acl_mtu"))
					fProperties->AddInt16("acl_mtu", buffer->acl_mtu);

				if (!IsPropertyAvailable("sco_mtu"))
					fProperties->AddInt8("sco_mtu", buffer->sco_mtu);

				if (!IsPropertyAvailable("acl_max_pkt"))
					fProperties->AddInt16("acl_max_pkt", buffer->acl_max_pkt);

				if (!IsPropertyAvailable("sco_max_pkt"))
					fProperties->AddInt16("sco_max_pkt", buffer->sco_max_pkt);

			}

			TRACE_BT("LocalDeviceImpl: Reply for Read Buffer Size %x\n", buffer->status);


			reply.AddUInt8("status", buffer->status);
			status = request->SendReply(&reply);
			//printf("Sending reply... %ld\n", status);
			// debug reply.PrintToStream();

			// This request is not gonna be used anymore
			ClearWantedEvent(request);
			break;
		}

		case PACK_OPCODE(OGF_INFORMATIONAL_PARAM, OCF_READ_LOCAL_SUPPORTED_COMMANDS):
		{
			struct hci_rp_read_loc_supported_cmd* supported_commands
				= JumpEventHeader<struct hci_rp_read_loc_supported_cmd,
				struct hci_ev_cmd_complete>(event);

			if (supported_commands->status == BT_OK) {

				if (!IsPropertyAvailable("supported_commands"))
					fProperties->AddData("supported_commands", B_ANY_TYPE,
						&supported_commands->supported_commands, 64);

			}

			TRACE_BT("LocalDeviceImpl: Reply for Supported Commands %x\n",
				supported_commands->status);

			reply.AddUInt8("status", supported_commands->status);
			status = request->SendReply(&reply);
			// printf("Sending reply... %d\n", status);
			// debug reply.PrintToStream();

			// This request is not gonna be used anymore
			ClearWantedEvent(request);
			break;
		}

		case PACK_OPCODE(OGF_INFORMATIONAL_PARAM, OCF_READ_BD_ADDR):
		{
			struct hci_rp_read_bd_addr* readbdaddr
				= JumpEventHeader<struct hci_rp_read_bd_addr,
				struct hci_ev_cmd_complete>(event);

			if (readbdaddr->status == BT_OK) {
				reply.AddData("bdaddr", B_ANY_TYPE, &readbdaddr->bdaddr,
					sizeof(bdaddr_t));
			}

			TRACE_BT("LocalDeviceImpl: Read bdaddr status = %x\n", readbdaddr->status);

			reply.AddUInt8("status", readbdaddr->status);
			status = request->SendReply(&reply);
			//printf("Sending reply... %ld\n", status);
			// debug reply.PrintToStream();

			// This request is not gonna be used anymore
			ClearWantedEvent(request);
			break;
		}

		case PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_READ_CLASS_OF_DEV):
		{
			struct hci_read_dev_class_reply* classDev
				= JumpEventHeader<struct hci_read_dev_class_reply,
				struct hci_ev_cmd_complete>(event);

			if (classDev->status == BT_OK) {
				reply.AddData("devclass", B_ANY_TYPE, &classDev->dev_class,
					sizeof(classDev->dev_class));
			}

			TRACE_BT("LocalDeviceImpl: Read DeviceClass status = %x DeviceClass = [%x][%x][%x]\n",
				classDev->status, classDev->dev_class[0],
				classDev->dev_class[1], classDev->dev_class[2]);


			reply.AddUInt8("status", classDev->status);
			status = request->SendReply(&reply);
			//printf("Sending reply... %ld\n", status);
			// debug reply.PrintToStream();

			// This request is not gonna be used anymore
			ClearWantedEvent(request);
			break;
		}

		case PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_READ_LOCAL_NAME):
		{
			struct hci_rp_read_local_name* readLocalName
				= JumpEventHeader<struct hci_rp_read_local_name,
				struct hci_ev_cmd_complete>(event);


			if (readLocalName->status == BT_OK) {
				reply.AddString("friendlyname",
					(const char*)readLocalName->local_name);
			}

			TRACE_BT("LocalDeviceImpl: Friendly name status %x\n", readLocalName->status);

			reply.AddUInt8("status", readLocalName->status);
			status = request->SendReply(&reply);
			//printf("Sending reply... %ld\n", status);
			// debug reply.PrintToStream();

			// This request is not gonna be used anymore
			ClearWantedEvent(request);
			break;
		}

		case PACK_OPCODE(OGF_LINK_CONTROL, OCF_PIN_CODE_REPLY):
		{
			uint8* statusReply = (uint8*)(event + 1);

			// TODO: This reply has to match the BDADDR of the outgoing message
			TRACE_BT("LocalDeviceImpl: pincode accept status %x\n", *statusReply);

			reply.AddUInt8("status", *statusReply);
			status = request->SendReply(&reply);
			//printf("Sending reply... %ld\n", status);
			// debug reply.PrintToStream();

			// This request is not gonna be used anymore
			//ClearWantedEvent(request, HCI_EVENT_CMD_COMPLETE, opcodeExpected);
			ClearWantedEvent(request);
			break;
		}

		case PACK_OPCODE(OGF_LINK_CONTROL, OCF_PIN_CODE_NEG_REPLY):
		{
			uint8* statusReply = (uint8*)(event + 1);

			// TODO: This reply might match the BDADDR of the outgoing message
			// => FindPetition should be expanded....
			TRACE_BT("LocalDeviceImpl: pincode reject status %x\n", *statusReply);

			reply.AddUInt8("status", *statusReply);
			status = request->SendReply(&reply);
			//printf("Sending reply... %ld\n", status);
			// debug reply.PrintToStream();

			// This request is not gonna be used anymore
			ClearWantedEvent(request, HCI_EVENT_CMD_COMPLETE, opcodeExpected);
			break;
		}

		case PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_READ_STORED_LINK_KEY):
		{
			struct hci_read_stored_link_key_reply* linkKeyRetrieval
				= JumpEventHeader<struct hci_read_stored_link_key_reply,
				struct hci_ev_cmd_complete>(event);

			TRACE_BT("LocalDeviceImpl: Status %s MaxKeys=%d, KeysRead=%d\n",
				BluetoothError(linkKeyRetrieval->status),
				linkKeyRetrieval->max_num_keys,
				linkKeyRetrieval->num_keys_read);

			reply.AddUInt8("status", linkKeyRetrieval->status);
			status = request->SendReply(&reply);
			//printf("Sending reply... %ld\n", status);
			// debug reply.PrintToStream();

			ClearWantedEvent(request);
			break;
		}

		case PACK_OPCODE(OGF_LINK_CONTROL, OCF_LINK_KEY_NEG_REPLY):
		case PACK_OPCODE(OGF_LINK_CONTROL, OCF_LINK_KEY_REPLY):
		{
			struct hci_cp_link_key_reply_reply* linkKeyReply
				= JumpEventHeader<struct hci_cp_link_key_reply_reply,
				struct hci_ev_cmd_complete>(event);

			TRACE_BT("LocalDeviceImpl: Status %s addresss=%s\n", BluetoothError(linkKeyReply->status),
				bdaddrUtils::ToString(linkKeyReply->bdaddr).String());

			ClearWantedEvent(request, HCI_EVENT_CMD_COMPLETE, opcodeExpected);
			break;
		}

		case  PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_READ_SCAN_ENABLE):
		{
			struct hci_read_scan_enable* scanEnable
				= JumpEventHeader<struct hci_read_scan_enable,
				struct hci_ev_cmd_complete>(event);

			if (scanEnable->status == BT_OK) {
				fProperties->AddInt8("scan_enable", scanEnable->enable);

				TRACE_BT("LocalDeviceImpl: enable = %x\n", scanEnable->enable);
			}

			reply.AddUInt8("status", scanEnable->status);
			reply.AddInt8("scan_enable", scanEnable->enable);
			status = request->SendReply(&reply);
			printf("Sending reply. scan_enable = %d\n", scanEnable->enable);
			// debug reply.PrintToStream();

			// This request is not gonna be used anymore
			ClearWantedEvent(request);
			break;
		}

		// without clearing all events, no reply
		case PACK_OPCODE(OGF_LINK_CONTROL, OCF_IO_CAPABILITY_REQUEST_REPLY):
		case PACK_OPCODE(OGF_LINK_CONTROL, OCF_USER_CONFIRM_REPLY):
		case PACK_OPCODE(OGF_LINK_CONTROL, OCF_USER_CONFIRM_NEG_REPLY):
		{
			TRACE_BT("LocalDeviceImpl: %s for %s status %x\n", __FUNCTION__,
				BluetoothCommandOpcode(opcodeExpected), *(uint8*)(event + 1));
			ClearWantedEvent(request, HCI_EVENT_CMD_COMPLETE, opcodeExpected);
			break;
		}

		case PACK_OPCODE(OGF_LINK_CONTROL, OCF_INQUIRY_CANCEL):
		{
			reply.AddUInt8("status", *(uint8*)(event + 1));

			TRACE_BT("LocalDeviceImpl: %s for %s status %x\n", __FUNCTION__,
				BluetoothCommandOpcode(opcodeExpected), *(uint8*)(event + 1));

			status = request->SendReply(&reply);
			printf("%s: Sending reply write...\n", __func__);
			if (status < B_OK)
				printf("%s: Error sending reply write!\n", __func__);

			BMessage* inquiry_request = FindPetition(HCI_EVENT_INQUIRY_COMPLETE);
			if (inquiry_request != NULL)
				ClearWantedEvent(inquiry_request);

			ClearWantedEvent(request);
			break;
		}

		// place here all CC that just replies a uint8 status
		case PACK_OPCODE(OGF_LINK_CONTROL, OCF_SET_CONN_ENCRYPT):
		case PACK_OPCODE(OGF_LINK_CONTROL, OCF_CREATE_CONN_CANCEL):
		case PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_RESET):
		case PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_WRITE_SCAN_ENABLE):
		case PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_WRITE_CLASS_OF_DEV):
		case PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_WRITE_PG_TIMEOUT):
		case PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_WRITE_CA_TIMEOUT):
		case PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_WRITE_AUTH_ENABLE):
		case PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_WRITE_LOCAL_NAME):
		case PACK_OPCODE(OGF_VENDOR_CMD, OCF_WRITE_BCM2035_BDADDR):
		{
			reply.AddUInt8("status", *(uint8*)(event + 1));

			TRACE_BT("LocalDeviceImpl: %s for %s status %x\n", __FUNCTION__,
				BluetoothCommandOpcode(opcodeExpected), *(uint8*)(event + 1));

			status = request->SendReply(&reply);
			printf("%s: Sending reply write...\n", __func__);
			if (status < B_OK)
				printf("%s: Error sending reply write!\n", __func__);

			ClearWantedEvent(request);
			break;
		}

		default:
			TRACE_BT("LocalDeviceImpl: Command Complete not handled\n");
			break;
	}
}


void
LocalDeviceImpl::CommandStatus(struct hci_ev_cmd_status* event,
	BMessage* request, int32 index)
{
	int16 opcodeExpected;
	BMessage reply;

	// Handle command complete information
	request->FindInt16("opcodeExpected", index, &opcodeExpected);

	if (request->IsSourceWaiting() == false) {
		TRACE_BT("LocalDeviceImpl: Nobody waiting for the event\n");
	}

	switch (opcodeExpected) {

		case PACK_OPCODE(OGF_LINK_CONTROL, OCF_INQUIRY):
		{
			reply.what = BT_MSG_INQUIRY_STARTED;

			TRACE_BT("LocalDeviceImpl: Inquiry status %x\n", event->status);

			reply.AddUInt8("status", event->status);
			request->SendReply(&reply);
			//printf("Sending reply... %ld\n", status);
			// debug reply.PrintToStream();

			ClearWantedEvent(request, HCI_EVENT_CMD_STATUS,
				PACK_OPCODE(OGF_LINK_CONTROL, OCF_INQUIRY));
		}
		break;

		case PACK_OPCODE(OGF_LINK_CONTROL, OCF_REMOTE_NAME_REQUEST):
		{
			TRACE_BT("LocalDeviceImpl: Command Status for remote friendly name %x\n",
				event->status);

			if (event->status != BT_OK) {
				reply.AddInt8("status", event->status);
				request->SendReply(&reply);
			}
			// printf("Sending reply... %ld\n", status);
			//  debug reply.PrintToStream();

			ClearWantedEvent(request, HCI_EVENT_CMD_STATUS, opcodeExpected);

		}
		break;
		case PACK_OPCODE(OGF_LINK_CONTROL, OCF_CREATE_CONN):
		{
			TRACE_BT("LocalDeviceImpl: Command Status for create connection %x\n", event->status);
			if (event->status != BT_OK) {
				CLASSIC_LOG(LE_LOG_ERROR, "Create Connection refused: %#x",
					event->status);
			}

			switch (event->status) {
				case BT_OK:
					ClearWantedEvent(request, HCI_EVENT_CMD_STATUS, opcodeExpected);
					break;

				case BT_ACL_CONNECTION_EXISTS:
					reply.what = BT_MSG_CONN_COMPLETED;
					reply.AddUInt8("status", BT_OK);
					request->SendReply(&reply);

					ClearWantedEvent(request);
					break;

				default:
					reply.what = BT_MSG_CONN_FAILED;
					reply.AddUInt8("status", event->status);
					request->SendReply(&reply);

					ClearWantedEvent(request);
					break;
			}
		}
		break;

		case PACK_OPCODE(OGF_LINK_CONTROL, OCF_DISCONNECT):
		{
			TRACE_BT("LocalDeviceImpl: Command Status for disconnect %x\n", event->status);

			ClearWantedEvent(request, HCI_EVENT_CMD_STATUS, opcodeExpected);
			if (event->status == BT_NO_CONNECTION) {
				reply.what = BT_MSG_DISCONN_COMPLETED;

				reply.AddUInt8("status", event->status);
				((BluetoothServer*)be_app)->NotifyWatchers(&reply);

				ClearWantedEvent(request);
			}
		}
		break;


		case PACK_OPCODE(OGF_LINK_CONTROL, OCF_ACCEPT_CONN_REQ):
		{
			TRACE_BT("LocalDeviceImpl: Command Status for accept connection request %x\n",
				event->status);
			ClearWantedEvent(request, HCI_EVENT_CMD_STATUS, opcodeExpected);
		}
		break;

		case PACK_OPCODE(OGF_LINK_CONTROL, OCF_REJECT_CONN_REQ):
		{
			TRACE_BT("LocalDeviceImpl: Command Status for reject connection request %x\n",
				event->status);
			ClearWantedEvent(request, HCI_EVENT_CMD_STATUS, opcodeExpected);
		} break;

		case PACK_OPCODE(OGF_LINK_CONTROL, OCF_AUTH_REQUESTED):
		{
			TRACE_BT("LocalDeviceImpl: Command Status for auth requested %x\n", event->status);
			ClearWantedEvent(request, HCI_EVENT_CMD_STATUS, opcodeExpected);
		} break;

		default:
			TRACE_BT("LocalDeviceImpl: Command Status not handled\n");
		break;
	}
}


void
LocalDeviceImpl::InquiryResult(uint8* numberOfResponses, BMessage* request)
{
	uint8 count = *numberOfResponses;
	TRACE_BT("LocalDeviceImpl: %s #responses=%d\n", __FUNCTION__, count);
	if (count == 0 || request == NULL)
		return;

	uint8* base_ptr = numberOfResponses + 1;

	// get pointers ready for the arrays
	bdaddr_t* bdaddr_array = (bdaddr_t*)base_ptr;
	uint8* page_repetition_mode_array = (uint8*)(bdaddr_array + count);
	uint8* scan_period_mode_array = page_repetition_mode_array + count;
	uint8* scan_mode_array = scan_period_mode_array + count;
	uint8* dev_class_array = scan_mode_array + count;
	uint16* clock_offset_array = (uint16*)(dev_class_array + (count * 3));

	BMessage reply(BT_MSG_INQUIRY_DEVICE);
	reply.AddUInt8("count", count);

	for (uint8 i = 0; i < count; i++) {
		TRACE_BT("LocalDeviceImpl: page_rep=%d scan_period=%d, scan=%d clock=%d\n",
			page_repetition_mode_array[i], scan_period_mode_array[i], scan_mode_array[i],
			clock_offset_array[i]);

		reply.AddData("bdaddr", B_ANY_TYPE, &bdaddr_array[i], sizeof(bdaddr_t));
		reply.AddData("dev_class", B_ANY_TYPE, &dev_class_array[i * 3], 3);
		reply.AddUInt8("page_repetition_mode", page_repetition_mode_array[i]);
		reply.AddUInt8("scan_period_mode", scan_period_mode_array[i]);
		reply.AddUInt8("scan_mode", scan_mode_array[i]);
		reply.AddUInt16("clock_offset", clock_offset_array[i]);
	}

	printf("%s: Sending reply...\n", __func__);
	status_t status = request->SendReply(&reply);
	if (status < B_OK)
		printf("%s: Error sending reply!\n", __func__);
}


void
LocalDeviceImpl::InquiryResultWithRSSI(uint8* numberOfResponses, BMessage* request)
{
	uint8 count = *numberOfResponses;
	TRACE_BT("LocalDeviceImpl: %s #responses=%d\n", __FUNCTION__, count);
	if (count == 0 || request == NULL)
		return;

	uint8* base_ptr = numberOfResponses + 1;

	// get pointers ready for the parallel arrays
	bdaddr_t* bdaddr_array = (bdaddr_t*)base_ptr;
	uint8* page_repetition_mode_array = (uint8*)(bdaddr_array + count);
	uint8* scan_period_mode_array = page_repetition_mode_array + count;
	uint8* dev_class_array = scan_period_mode_array + count;
	uint16* clock_offset_array = (uint16*)(dev_class_array + (count * 3));
	int8* rssi_array = (int8*)(clock_offset_array + count);

	BMessage reply(BT_MSG_INQUIRY_DEVICE);
	reply.AddUInt8("count", count);

	for (uint8 i = 0; i < count; i++) {
		TRACE_BT("LocalDeviceImpl: page_rep=%d scan_period=%d clock=%d rssi=%d\n",
			page_repetition_mode_array[i], scan_period_mode_array[i], clock_offset_array[i],
			rssi_array[i]);

		reply.AddData("bdaddr", B_ANY_TYPE, &bdaddr_array[i], sizeof(bdaddr_t));
		reply.AddData("dev_class", B_ANY_TYPE, &dev_class_array[i * 3], 3);
		reply.AddUInt8("page_repetition_mode", page_repetition_mode_array[i]);
		reply.AddUInt8("scan_period_mode", scan_period_mode_array[i]);
		reply.AddUInt16("clock_offset", clock_offset_array[i]);
		reply.AddInt8("rssi", rssi_array[i]);
	}

	printf("%s: Sending reply...\n", __func__);
	status_t status = request->SendReply(&reply);
	if (status < B_OK)
		printf("%s: Error sending reply!\n", __func__);
}


void
LocalDeviceImpl::ExtendedInquiryResult(uint8* numberOfResponses, BMessage* request)
{
	uint8 count = *numberOfResponses;
	TRACE_BT("LocalDeviceImpl: %s #responses=%d\n", __FUNCTION__, count);

	// the spec says count always equals 1
	if (count != 1 || request == NULL)
		return;

	hci_ev_extended_inquiry_info* info = (hci_ev_extended_inquiry_info*)(numberOfResponses + 1);

	BMessage reply(BT_MSG_INQUIRY_DEVICE);
	reply.AddUInt8("count", count);

	TRACE_BT("LocalDeviceImpl: page_rep=%d scan_period=%d clock=%d rssi=%d\n",
		info->page_repetition_mode, info->scan_period_mode, info->clock_offset, info->rssi);

	reply.AddData("bdaddr", B_ANY_TYPE, &info->bdaddr, sizeof(bdaddr_t));
	reply.AddData("dev_class", B_ANY_TYPE, info->dev_class, 3);
	reply.AddUInt8("page_repetition_mode", info->page_repetition_mode);
	reply.AddUInt8("scan_period_mode", info->scan_period_mode);
	reply.AddUInt16("clock_offset", info->clock_offset);
	reply.AddInt8("rssi", info->rssi);

	ParseEIR(info->eir, reply);

	printf("%s: Sending reply...\n", __func__);
	status_t status = request->SendReply(&reply);
	if (status < B_OK)
		printf("%s: Error sending reply!\n", __func__);
}


void
LocalDeviceImpl::ParseEIR(const uint8* eir, BMessage& reply)
{
	if (eir == NULL)
		return;

	int offset = 0;
	BString completeName;
	BString shortName;

	while (offset < HCI_MAX_EIR_LENGTH) {
		uint8 length = eir[offset];
		// break either when finished reading buffer or when next data value is zero
		if (length == 0 || offset + length >= HCI_MAX_EIR_LENGTH)
			break;
		uint8 type = eir[offset + 1];
		const uint8* data = &eir[offset + 2];
		uint8 dataLen = length - 1;
		switch (type) {
			case EIR_FLAGS:
				if (dataLen >= 1) {
					reply.AddUInt8("eir_flags", data[0]);
					TRACE_BT("LocalDeviceImpl: Parsed EIR Flags: 0x%02X\n", data[0]);
				}
				break;
			case EIR_UUID16_INCOMPLETE:
				reply.AddBool("uuid16_complete", false);
			case EIR_UUID16_COMPLETE:
				for (uint8 i = 0; i + 1 < dataLen; i += 2) {
					uint16 uuid = B_LENDIAN_TO_HOST_INT16(*(const uint16*)(data + i));
					reply.AddUInt16("uuid16", uuid);
					TRACE_BT("LocalDeviceImpl: Parsed EIR UUID16: 0x%04X\n", uuid);
				}
				break;
			case EIR_UUID32_INCOMPLETE:
				reply.AddBool("uuid32_complete", false);
			case EIR_UUID32_COMPLETE:
				for (uint8 i = 0; i + 3 < dataLen; i += 4) {
					uint32 uuid = B_LENDIAN_TO_HOST_INT32(*(const uint32*)(data + i));
					reply.AddUInt32("uuid32", uuid);
					TRACE_BT("LocalDeviceImpl: Parsed EIR of UUID32: 0x%08" B_PRIx32 "\n", uuid);
				}
				break;
			case EIR_UUID128_INCOMPLETE:
				reply.AddBool("uuid128_complete", false);
			case EIR_UUID128_COMPLETE:
				for (uint8 i = 0; i + 15 < dataLen; i += 16) {
					// UUID128 is stored as 16 bytes in little-endian format
					reply.AddData("uuid128", B_ANY_TYPE, data + i, 16);
					TRACE_BT("LocalDeviceImpl: Parsed EIR UUID128 \n");
				}
				break;
			case EIR_NAME_SHORT:
				if (shortName.Length() == 0) {
					shortName.SetTo((const char*)data, dataLen);
					TRACE_BT("LocalDeviceImpl: Parsed EIR Short Name: '%s'\n", shortName.String());
				}
				break;
			case EIR_NAME_COMPLETE:
				if (completeName.Length() == 0) {
					completeName.SetTo((const char*)data, dataLen);
					TRACE_BT("LocalDeviceImpl: Parsed EIR Complete Name: '%s'\n",
						completeName.String());
				}
				break;
			case EIR_TX_POWER:
				if (dataLen >= 1) {
					reply.AddInt8("tx_power", (int8)data[0]);
					TRACE_BT("LocalDeviceImpl: Parsed EIR TX Power: %d dBm\n", (int8)data[0]);
				}
				break;
			case EIR_CLASS_OF_DEVICE:
				if (dataLen >= 3) {
					reply.AddData("dev_class", B_ANY_TYPE, data, 3);
					TRACE_BT("LocalDeviceImpl: Parsed EIR Class of Device: "
							 "0x%02X 0x%02X 0x%02X\n",
						data[0], data[1], data[2]);
				}
				break;
			case EIR_MANUFACTURER_DATA:
				if (dataLen >= 2) {
					reply.AddData("manufacturer_data", B_ANY_TYPE, data, dataLen);
					uint16 company = data[0] | (data[1] << 8);
					TRACE_BT("LocalDeviceImpl: Parsed EIR Manufacturer Data: "
							 "company=0x%04X len=%d\n",
						company, dataLen);
				}
				break;
			default:
				TRACE_BT("LocalDeviceImpl: Ignored EIR Type: 0x%02X (Length: %d)\n", type, dataLen);
				break;
		}

		offset += length + 1;
	}

	if (completeName.Length() > 0) {
		reply.AddString("friendly_name", completeName.String());
		reply.AddBool("friendly_name_is_complete", true);
	} else if (shortName.Length() > 0) {
		reply.AddString("friendly_name", shortName.String());
		reply.AddBool("friendly_name_is_complete", false);
	}
}


void
LocalDeviceImpl::InquiryComplete(uint8* status, BMessage* request)
{
	BMessage reply(BT_MSG_INQUIRY_COMPLETED);

	reply.AddUInt8("status", *status);

	printf("%s: Sending reply...\n", __func__);
	status_t stat = request->SendReply(&reply);
	if (stat < B_OK)
		printf("%s: Error sending reply!\n", __func__);

	ClearWantedEvent(request);
}


void
LocalDeviceImpl::RemoteNameRequestComplete(
	struct hci_ev_remote_name_request_complete_reply* remotename,
	BMessage* request)
{
	BMessage reply;

	if (remotename->status == BT_OK) {
		reply.AddString("friendlyname", (const char*)remotename->remote_name );
	}

	reply.AddUInt8("status", remotename->status);

	TRACE_BT("LocalDeviceImpl: %s for %s with status %s\n",
		BluetoothEvent(HCI_EVENT_REMOTE_NAME_REQUEST_COMPLETE),
		bdaddrUtils::ToString(remotename->bdaddr).String(),
		BluetoothError(remotename->status));

	status_t status = request->SendReply(&reply);
	if (status < B_OK)
		printf("%s: Error sending reply to BMessage request: %s!\n",
			__func__, strerror(status));

	// This request is not gonna be used anymore
	ClearWantedEvent(request);
}


void
LocalDeviceImpl::ConnectionRequest(struct hci_ev_conn_request* event,
	BMessage* request)
{
	size_t size;
	void* command;

	TRACE_BT("LocalDeviceImpl: Connection Incoming type %x from %s...\n",
		event->link_type, bdaddrUtils::ToString(event->bdaddr).String());

	// TODO: add a possible request in the queue
	if (true) { // Check Preferences if we are to accept this connection

		ServerRemoteDevice* serverRd;
		serverRd = RemoteDeviceByAddr(event->bdaddr);

		bool known = serverRd != NULL;
		if (serverRd == NULL) {
			serverRd = new ServerRemoteDevice();
			serverRd->bdaddr = event->bdaddr;
			serverRd->link_key = LinkKeyUtils::NullKey();
		}

		serverRd->link_type = event->link_type;
		serverRd->conn_state = RemoteDevice::CONNECTING;
		serverRd->outgoing = false;
		memcpy(serverRd->classOfDevice, event->dev_class, 3 * sizeof(uint8));

		BMessage notice(BT_MSG_NEW_REMOTE_DEVICE);
		notice.AddData("bdaddr", B_ANY_TYPE, &serverRd->bdaddr, sizeof(bdaddr_t));
		notice.AddData("class_of_device", B_RAW_TYPE, serverRd->classOfDevice,
			sizeof(serverRd->classOfDevice));

		((BluetoothServer*)be_app)->NotifyWatchers(&notice);
		if (!known)
			AddRemoteDevice(serverRd);

		// Keep ourselves as slave
		command = buildAcceptConnectionRequest(event->bdaddr, 0x01 , &size);

		BMessage* newrequest = new BMessage;
		newrequest->AddInt16("eventExpected", HCI_EVENT_CMD_STATUS);
		newrequest->AddInt16("opcodeExpected", PACK_OPCODE(OGF_LINK_CONTROL,
			OCF_ACCEPT_CONN_REQ));

		newrequest->AddInt16("eventExpected", HCI_EVENT_PAGE_SCAN_REP_MODE_CHANGE);

		newrequest->AddInt16("eventExpected", HCI_EVENT_LINK_KEY_REQ);
		newrequest->AddInt16("eventExpected", HCI_EVENT_ROLE_CHANGE);

#if 0
		newrequest->AddInt16("eventExpected", HCI_EVENT_MAX_SLOT_CHANGE);
		newrequest->AddInt16("eventExpected", HCI_EVENT_DISCONNECTION_COMPLETE);
#endif

		AddWantedEvent(newrequest);

		if ((fHCIDelegate)->IssueCommand(command, size) == B_ERROR) {
			TRACE_BT("LocalDeviceImpl: Command issued error for Accepting connection\n");
				// remove the request?
		} else {
			TRACE_BT("LocalDeviceImpl: Command issued for Accepting connection\n");
		}
	}
}


void
LocalDeviceImpl::CreateConnection(BMessage* message)
{
	const bdaddr_t* bdaddr;
	ssize_t addr_size;
	if (message->FindData("bdaddr", B_ANY_TYPE,
			(const void**)&bdaddr, &addr_size) != B_OK
		|| addr_size != sizeof(bdaddr_t))
		return;

	TRACE_BT("LocalDeviceImpl: Create Connection to %s...\n",
		bdaddrUtils::ToString(*bdaddr).String());

	ServerRemoteDevice* rdConn;
	rdConn = RemoteDeviceByAddr(*bdaddr);

	bool known = rdConn != NULL;
	if (rdConn == NULL) {
		rdConn = new ServerRemoteDevice();
		rdConn->bdaddr = *bdaddr;
		rdConn->link_key = LinkKeyUtils::NullKey();
	}

	message->FindString("name", &rdConn->friendly_name);
	message->FindUInt8("pscan_rep_mode", &rdConn->pscan_rep_mode);
	message->FindUInt16("clock_offset", &rdConn->clock_offset);

	uint32 record;
	message->FindUInt32("record", &record);
	rdConn->classOfDevice[0] = (record & 0x000000FF);
	rdConn->classOfDevice[1] = (record & 0x0000FF00) >> 8;
	rdConn->classOfDevice[2] = (record & 0x00FF0000) >> 16;

	rdConn->conn_state = RemoteDevice::CONNECTING;
	rdConn->link_type = HCI_ACL_CONN;
	rdConn->outgoing = true;

	if (!known)
		AddRemoteDevice(rdConn);

	BluetoothCommand<typed_command(hci_cp_create_conn)>
		command(OGF_LINK_CONTROL, OCF_CREATE_CONN);

	command->bdaddr = *bdaddr;
	message->FindUInt16("packet type", &command->pkt_type);
	command->pkt_type &= ACL_PTYPE_MASK;
	command->pscan_rep_mode = rdConn->pscan_rep_mode;
	message->FindUInt8("pscan_mode", &command->pscan_mode); // Reserved in spec 2.1
	// Bit 15 says the offset is valid; only an inquiry gives one.
	command->clock_offset = rdConn->clock_offset & 0x7fff;
	if (command->clock_offset != 0)
		command->clock_offset |= 0x8000;
	message->FindUInt8("role_switch", &command->role_switch);


	CLASSIC_LOG(LE_LOG_INFO, "connecting to %s (packet types %#x, page scan "
		"mode %u, clock offset %#x)", bdaddrUtils::ToString(*bdaddr).String(),
		command->pkt_type, command->pscan_rep_mode, command->clock_offset);
	if (fHCIDelegate->IssueCommand(command.Data(), command.Size()) == B_ERROR) {
		CLASSIC_LOG(LE_LOG_ERROR, "could not issue Create Connection");
		return;
	}

	BMessage* newRequest = new BMessage;

	newRequest->AddInt32("hci_id", fHCIDelegate->Id());

	newRequest->AddInt16("eventExpected",  HCI_EVENT_CMD_STATUS);
	newRequest->AddInt16("opcodeExpected", PACK_OPCODE(OGF_LINK_CONTROL, OCF_CREATE_CONN));

	newRequest->AddInt16("eventExpected", HCI_EVENT_LINK_KEY_REQ);
	newRequest->AddInt16("eventExpected", HCI_EVENT_ROLE_CHANGE);

	AddWantedEvent(newRequest);
}


void
LocalDeviceImpl::CancelConnection(BMessage* message)
{
	const bdaddr_t* bdaddr;
	ssize_t addr_size;
	message->FindData("bdaddr", B_ANY_TYPE, (const void**)&bdaddr, &addr_size);

	TRACE_BT("LocalDeviceImpl: Cancel Connection to %s...\n",
		bdaddrUtils::ToString(*bdaddr).String());

	BluetoothCommand<typed_command(hci_cp_create_conn_cancel)> command(OGF_LINK_CONTROL,
		OCF_CREATE_CONN_CANCEL);

	command->bdaddr = *bdaddr;

	if (fHCIDelegate->IssueCommand(command.Data(), command.Size()) == B_ERROR) {
		TRACE_BT("LocalDeviceImpl: Command issued error for %s\n", __FUNCTION__);
		return;
	} else {
		TRACE_BT("LocalDeviceImpl: Command issued for %s\n", __FUNCTION__);
	}

	BMessage* newRequest = new BMessage(BT_MSG_HANDLE_SIMPLE_REQUEST);

	newRequest->AddInt32("hci_id", fHCIDelegate->Id());
	newRequest->AddInt16("eventExpected", HCI_EVENT_CMD_COMPLETE);
	newRequest->AddInt16("opcodeExpected", PACK_OPCODE(OGF_LINK_CONTROL, OCF_CREATE_CONN_CANCEL));

	AddWantedEvent(newRequest);
}


void
LocalDeviceImpl::Disconnect(BMessage* message)
{
	TRACE_BT("LocalDeviceImpl: %s...\n", __FUNCTION__);

	BluetoothCommand<typed_command(hci_disconnect)> command(OGF_LINK_CONTROL,
		OCF_DISCONNECT);

	const bdaddr_t* bdaddr;
	ssize_t addr_size;
	message->FindData("bdaddr", B_ANY_TYPE, (const void**)&bdaddr, &addr_size);

	ServerRemoteDevice* rd = RemoteDeviceByAddr(*bdaddr);
	if (rd == NULL || rd->conn_state != RemoteDevice::CONNECTED)
		return;
	command->handle = rd->handle;
	message->FindUInt8("reason", &command->reason);

	if (fHCIDelegate->IssueCommand(command.Data(), command.Size()) == B_ERROR) {
		TRACE_BT("LocalDeviceImpl: Command issued error for %s\n", __FUNCTION__);
		return;
	} else {
		TRACE_BT("LocalDeviceImpl: Command issued for %s\n", __FUNCTION__);
	}
	BMessage* newRequest = new BMessage(BT_MSG_HANDLE_SIMPLE_REQUEST);

	newRequest->AddInt32("hci_id", fHCIDelegate->Id());

	newRequest->AddInt16("eventExpected", HCI_EVENT_CMD_STATUS);
	newRequest->AddInt16("opcodeExpected", PACK_OPCODE(OGF_LINK_CONTROL, OCF_DISCONNECT));

	AddWantedEvent(newRequest);
}


void
LocalDeviceImpl::SetConnEncryption(uint16 handle, bool encryption_enabled)
{
	TRACE_BT("LocalDeviceImpl: %s...\n", __FUNCTION__);

	BluetoothCommand<typed_command(hci_cp_set_conn_encrypt)> command(OGF_LINK_CONTROL,
		OCF_SET_CONN_ENCRYPT);

	command->handle = handle;
	command->encrypt = encryption_enabled;

	if (fHCIDelegate->IssueCommand(command.Data(), command.Size()) == B_ERROR) {
		TRACE_BT("LocalDeviceImpl: Command issued error for %s\n", __FUNCTION__);
		return;
	}
	TRACE_BT("LocalDeviceImpl: Command issued for %s\n", __FUNCTION__);

	BMessage* newRequest = new BMessage(BT_MSG_HANDLE_SIMPLE_REQUEST);

	newRequest->AddInt32("hci_id", fHCIDelegate->Id());

	newRequest->AddInt16("eventExpected", HCI_EVENT_CMD_STATUS);
	newRequest->AddInt16("opcodeExpected", PACK_OPCODE(OGF_LINK_CONTROL, OCF_SET_CONN_ENCRYPT));

	AddWantedEvent(newRequest);
}


void
LocalDeviceImpl::Authenticate(uint16 handle)
{
	TRACE_BT("LocalDeviceImpl: %s...\n", __FUNCTION__);

	BluetoothCommand<typed_command(hci_cp_auth_requested)> command(OGF_LINK_CONTROL,
		OCF_AUTH_REQUESTED);

	command->handle = handle;


	if (fHCIDelegate->IssueCommand(command.Data(), command.Size()) == B_ERROR) {
		TRACE_BT("LocalDeviceImpl: Command issued error for %s\n", __FUNCTION__);
		return;
	} else {
		TRACE_BT("LocalDeviceImpl: Command issued for %s\n", __FUNCTION__);
	}
	BMessage* newRequest = new BMessage();

	newRequest->AddInt32("hci_id", fHCIDelegate->Id());

	newRequest->AddInt16("eventExpected", HCI_EVENT_CMD_STATUS);
	newRequest->AddInt16("opcodeExpected", PACK_OPCODE(OGF_LINK_CONTROL, OCF_AUTH_REQUESTED));

	newRequest->AddInt16("eventExpected", HCI_EVENT_AUTH_COMPLETE);

	AddWantedEvent(newRequest);
}


void
LocalDeviceImpl::ConnectionComplete(struct hci_ev_conn_complete* event)
{
	BMessage reply;

	ServerRemoteDevice* rd = RemoteDeviceByAddr(event->bdaddr);
	if (rd == NULL && event->status == BT_OK) {
		rd = new ServerRemoteDevice();
		rd->bdaddr = event->bdaddr;
		rd->link_key = LinkKeyUtils::NullKey();
		memset(rd->classOfDevice, 0, sizeof(rd->classOfDevice));
		AddRemoteDevice(rd);
	}
	if (rd != NULL)
		reply.AddData("bdaddr", B_ANY_TYPE, &event->bdaddr, sizeof(bdaddr_t));

	reply.AddUInt8("status", event->status);
	CLASSIC_LOG(event->status == BT_OK ? LE_LOG_INFO : LE_LOG_ERROR,
		"link to %s: status %#x, handle %#x, type %u, encryption %u",
		bdaddrUtils::ToString(event->bdaddr).String(), event->status,
		event->handle, event->link_type, event->encrypt_mode);
	if (event->status == BT_OK) {
		if (rd != NULL) {
			rd->handle = event->handle;
			rd->link_type = event->link_type;
			rd->conn_state = RemoteDevice::CONNECTED;
			rd->encryption_enabled = event->encrypt_mode;
			((BluetoothServer*)be_app)->GetHIDHost()->LinkEncrypted(
				rd->bdaddr, event->encrypt_mode != 0);
		}

		// Tell the user about a device that connected on its own; one we
		// connected to was asked for, and a paired one (a keyboard waking
		// up) is expected.
		linkkey_t nullKey = LinkKeyUtils::NullKey();
		bool paired = !LinkKeyUtils::Compare(&rd->link_key, &nullKey);
		if (!rd->outgoing && !paired) {
			// TODO: Review, this rDevice is leaked
			ConnectionIncoming* iConnection = new ConnectionIncoming(
				new RemoteDevice(event->bdaddr, rd->classOfDevice));
			iConnection->Show();
		}

		TRACE_BT("LocalDeviceImpl: %s: Address %s handle=%#x type=%d encrypt=%d\n", __FUNCTION__,
				bdaddrUtils::ToString(event->bdaddr).String(), event->handle,
				event->link_type, event->encrypt_mode);

		BMessage* newrequest = new BMessage;
		newrequest->AddInt16("eventExpected", HCI_EVENT_IO_CAPABILITY_REQUEST);
		newrequest->AddInt16("eventExpected", HCI_EVENT_IO_CAPABILITY_RESPONSE);

		// this implies PAIRING complete
		newrequest->AddInt16("eventExpected", HCI_EVENT_LINK_KEY_NOTIFY);
		AddWantedEvent(newrequest);

		Authenticate(event->handle);

		reply.what = BT_MSG_CONN_COMPLETED;
	} else {
		if (rd != NULL) {
			linkkey_t nullLinkKey = LinkKeyUtils::NullKey();
			if (LinkKeyUtils::Compare(&rd->link_key, &nullLinkKey))
				RemoveRemoteDevice(rd);
			else
				rd->conn_state = RemoteDevice::DISCONNECTED;
		}

		TRACE_BT("LocalDeviceImpl: %s: failed with error %s\n", __FUNCTION__,
			BluetoothError(event->status));

		reply.what = BT_MSG_CONN_FAILED;
	}

	((BluetoothServer*)be_app)->NotifyWatchers(&reply);
}


void
LocalDeviceImpl::DisconnectionComplete(hci_ev_disconnection_complete_reply* event)
{
	TRACE_BT("LocalDeviceImpl: %s: Handle=%#x, reason=%s status=%x\n", __FUNCTION__, event->handle,
		BluetoothError(event->reason), event->status);
	uint16 handle = B_LENDIAN_TO_HOST_INT16(event->handle);
	HandleLEDisconnection(handle, event->status, event->reason);
	CLASSIC_LOG(LE_LOG_INFO, "link %#x down: status %#x, reason %#x", handle,
		event->status, event->reason);

	ServerRemoteDevice* rd;
	rd = RemoteDeviceByHandle(handle);
	BMessage reply(BT_MSG_DISCONN_COMPLETED);
	reply.AddUInt8("status", event->status);

	if (rd != NULL)
		reply.AddData("bdaddr", B_ANY_TYPE, &rd->bdaddr, sizeof(bdaddr_t));

	if (rd != NULL && (event->status == BT_OK
		|| event->status == BT_NO_CONNECTION)) {
		rd->conn_state = RemoteDevice::DISCONNECTED;
		rd->outgoing = false;
		rd->encryption_enabled = 0;
		((BluetoothServer*)be_app)->GetHIDHost()->LinkDown(rd->bdaddr);
	}


	((BluetoothServer*)be_app)->NotifyWatchers(&reply);
}


void
LocalDeviceImpl::PinCodeRequest(struct hci_ev_pin_code_req* event,
	BMessage* request)
{
	TRACE_BT("LocalDeviceImpl: Opening PincodeWindow...");
	PincodeWindow* iPincode = new PincodeWindow(event->bdaddr, GetID());
	iPincode->Show();
}


void
LocalDeviceImpl::RoleChange(hci_ev_role_change* event, BMessage* request)
{
	// TODO: we shall update the RemoteDevice
	TRACE_BT("LocalDeviceImpl: %s: Address %s role=%d status=%d\n", __FUNCTION__,
		bdaddrUtils::ToString(event->bdaddr).String(), event->role, event->status);
}


void
LocalDeviceImpl::PageScanRepetitionModeChange(
	struct hci_ev_page_scan_rep_mode_change* event, BMessage* request)
{
	// TODO: we shall update the RemoteDevice
	TRACE_BT("LocalDeviceImpl: %s: Address %s type=%d\n",	__FUNCTION__,
		bdaddrUtils::ToString(event->bdaddr).String(), event->page_scan_rep_mode);
}


void
LocalDeviceImpl::LinkKeyNotify(hci_ev_link_key_notify* event,
	BMessage* request)
{
	TRACE_BT("LocalDeviceImpl: %s: Address %s, key=%s, type=%d\n", __FUNCTION__,
		bdaddrUtils::ToString(event->bdaddr).String(),
		LinkKeyUtils::ToString(event->link_key).String(), event->key_type);

	if (request != NULL)
		ClearWantedEvent(request);

	ServerRemoteDevice* rd = RemoteDeviceByAddr(event->bdaddr);
	if (rd == NULL)
		return;

	CLASSIC_LOG(LE_LOG_INFO, "new link key for %s, type %u",
		bdaddrUtils::ToString(event->bdaddr).String(), event->key_type);
	rd->link_key = event->link_key;
	rd->key_type = event->key_type;
	// Keep the key now: the next time the device connects it is asked for,
	// and losing it to a crash or power cut means pairing again.
	SaveRemoteDevices();
}


void
LocalDeviceImpl::LinkKeyRequested(struct hci_ev_link_key_req* keyRequested,
	BMessage* request)
{
	TRACE_BT("LocalDeviceImpl: %s: Address %s\n", __FUNCTION__,
		bdaddrUtils::ToString(keyRequested->bdaddr).String());

	ServerRemoteDevice* rd = RemoteDeviceByAddr(keyRequested->bdaddr);
	linkkey_t nullLinkKey = LinkKeyUtils::NullKey();
	const bool haveKey = rd != NULL
		&& !LinkKeyUtils::Compare(&rd->link_key, &nullLinkKey);
	CLASSIC_LOG(LE_LOG_INFO, "link key for %s requested: %s",
		bdaddrUtils::ToString(keyRequested->bdaddr).String(),
		haveKey ? "stored" : "none, pairing");
	if (!haveKey) {
		BluetoothCommand<typed_command(hci_cp_link_key_neg_reply)>
			linkKeyNegativeReply(OGF_LINK_CONTROL, OCF_LINK_KEY_NEG_REPLY);

		BMessage* newrequest = new BMessage;
		newrequest->AddInt16("eventExpected", HCI_EVENT_CMD_COMPLETE);
		newrequest->AddInt16("opcodeExpected",
			PACK_OPCODE(OGF_LINK_CONTROL, OCF_LINK_KEY_NEG_REPLY));
		// on neg reply we expect this
		newrequest->AddInt16("eventExpected", HCI_EVENT_PIN_CODE_REQ);
		AddWantedEvent(newrequest);
		bdaddrUtils::Copy(linkKeyNegativeReply->bdaddr, keyRequested->bdaddr);

		if ((fHCIDelegate)->IssueCommand(linkKeyNegativeReply.Data(),
			linkKeyNegativeReply.Size()) == B_ERROR) {
			TRACE_BT("LocalDeviceImpl: Command issued error for neg reply %s\n", __FUNCTION__);
		} else {
			TRACE_BT("LocalDeviceImpl: Command issued in neg reply of  %s\n", __FUNCTION__);
		}
	} else {

		BluetoothCommand<typed_command(hci_cp_link_key_reply)>
			linkKeyReply(OGF_LINK_CONTROL, OCF_LINK_KEY_REPLY);

		BMessage* newrequest = new BMessage;
		newrequest->AddInt16("eventExpected", HCI_EVENT_CMD_COMPLETE);
		newrequest->AddInt16("opcodeExpected", PACK_OPCODE(OGF_LINK_CONTROL, OCF_LINK_KEY_REPLY));
		// on neg reply we expect this
		newrequest->AddInt16("eventExpected", HCI_EVENT_PIN_CODE_REQ);
		AddWantedEvent(newrequest);

		bdaddrUtils::Copy(linkKeyReply->bdaddr, keyRequested->bdaddr);
		linkKeyReply->link_key = rd->link_key;

		if ((fHCIDelegate)->IssueCommand(linkKeyReply.Data(), linkKeyReply.Size()) == B_ERROR) {
			TRACE_BT("LocalDeviceImpl: Command issued error for reply %s\n", __FUNCTION__);
		} else {
			TRACE_BT("LocalDeviceImpl: Command issued in reply of  %s\n", __FUNCTION__);
		}
	}
	if (request != NULL)
		ClearWantedEvent(request, HCI_EVENT_LINK_KEY_REQ);

}


void
LocalDeviceImpl::ReturnLinkKeys(struct hci_ev_return_link_keys* returnedKeys)
{
	TRACE_BT("LocalDeviceImpl: %s: #keys=%d\n",
		__FUNCTION__, returnedKeys->num_keys);

	uint8 numKeys = returnedKeys->num_keys;

	struct link_key_info* linkKeys = &returnedKeys->link_keys;

	while (numKeys > 0) {

		TRACE_BT("LocalDeviceImpl: Address=%s key=%s\n",
			bdaddrUtils::ToString(linkKeys->bdaddr).String(),
			LinkKeyUtils::ToString(linkKeys->link_key).String());

		linkKeys++;
		numKeys--;
	}
}


void
LocalDeviceImpl::MaxSlotChange(struct hci_ev_max_slot_change* event,
	BMessage* request)
{
	TRACE_BT("LocalDeviceImpl: %s: Handle=%#x, max slots=%d\n", __FUNCTION__,
		event->handle, event->lmp_max_slots);
}


void
LocalDeviceImpl::HardwareError(struct hci_ev_hardware_error* event)
{
	TRACE_BT("LocalDeviceImpl: %s: hardware code=%#x\n",	__FUNCTION__, event->hardware_code);
}


void
LocalDeviceImpl::NumberOfCompletedPackets(struct hci_ev_num_comp_pkts* event)
{
	TRACE_BT("LocalDeviceImpl: %s: #Handles=%d\n", __FUNCTION__, event->num_hndl);

	struct handle_and_number* numberPackets
		= JumpEventHeader<handle_and_number, hci_ev_num_comp_pkts>(event);

	for (uint8 i = 0; i < event->num_hndl; i++) {

		TRACE_BT("LocalDeviceImpl: %s: Handle=%d #packets=%d\n", __FUNCTION__, numberPackets->handle,
			numberPackets->num_completed);

			numberPackets++;
	}
}


void
LocalDeviceImpl::IOCapabilityRequest(struct hci_ev_io_capability_request* event, BMessage* request)
{
	size_t size;
	void* command;

	TRACE_BT("LocalDeviceImpl: IO Capability Request from %s...\n",
		bdaddrUtils::ToString(event->bdaddr).String());

	// TODO: this should be temporary, need to change this to HCI_IO_CAP_DISPLAY_YES_NO
	command = buildIOCapabilityRequestReply(event->bdaddr, HCI_IO_CAP_DISPLAY_YES_NO,
		HCI_OOB_DATA_NOT_PRESENT, HCI_AUTH_REQ_MITM_GENERAL_BOND, &size);

	BMessage* newrequest = new BMessage;

	newrequest->AddInt16("eventExpected", HCI_EVENT_CMD_COMPLETE);
	newrequest->AddInt16("opcodeExpected",
		PACK_OPCODE(OGF_LINK_CONTROL, OCF_IO_CAPABILITY_REQUEST_REPLY));

	// TODO:Check if there are more events we need to look for
	newrequest->AddInt16("eventExpected", HCI_EVENT_USER_CONFIRMATION_REQUEST);
	newrequest->AddInt16("eventExpected", HCI_EVENT_USER_PASSKEY_REQUEST);
	newrequest->AddInt16("eventExpected", HCI_EVENT_SIMPLE_PAIRING_COMPLETE);

	AddWantedEvent(newrequest);

	if ((fHCIDelegate)->IssueCommand(command, size) == B_ERROR)
		TRACE_BT("LocalDeviceImpl: Command issued error for reply %s\n", __FUNCTION__);
	else
		TRACE_BT("LocalDeviceImpl: Command issued in reply of  %s\n", __FUNCTION__);
}


void
LocalDeviceImpl::IOCapabilityResponse(struct hci_ev_io_capability_response* event,
	BMessage* request)
{
	TRACE_BT("LocalDeviceImpl: %s for %s - Capability: 0x%02x, OOB: 0x%02x, Auth: 0x%02x\n",
		__FUNCTION__, bdaddrUtils::ToString(event->bdaddr).String(), event->capability,
		event->oob_data, event->authentication);

	ServerRemoteDevice* rd = RemoteDeviceByAddr(event->bdaddr);
	if (rd != NULL)
		rd->remote_io_capability = event->capability;
	CLASSIC_LOG(LE_LOG_INFO, "pairing with %s: its IO capability %u, "
		"authentication %#x", bdaddrUtils::ToString(event->bdaddr).String(),
		event->capability, event->authentication);
}


void
LocalDeviceImpl::UserConfirmationRequest(struct hci_ev_user_confirmation_request* event,
	BMessage* request)
{
	TRACE_BT("LocalDeviceImpl: User Confirmation Request for %s (Passkey: %06" B_PRIu32 ")\n",
		bdaddrUtils::ToString(event->bdaddr).String(), event->passkey);

	ServerRemoteDevice* rd = RemoteDeviceByAddr(event->bdaddr);
	BString name = bdaddrUtils::ToString(event->bdaddr);
	if (rd != NULL && !rd->friendly_name.IsEmpty())
		name = rd->friendly_name;

	BString prompt;
	if (rd != NULL
		&& rd->remote_io_capability == HCI_IO_CAP_NO_INPUT_NO_OUTPUT) {
		// "Just Works" (Core Vol 3 Part C 5.2.2.6): a speaker or headset
		// can neither show nor confirm a number, so there is nothing to
		// compare. When the link was asked for here, the user already chose
		// this device; otherwise ask, but without a meaningless code.
		if (rd->outgoing) {
			printf("Bluetooth: pairing with %s, which has no display or "
				"input\n", name.String());
			ConfirmPairing(event->bdaddr, true);
			return;
		}
		prompt.SetToFormat("Pair with %s?", name.String());
	} else {
		prompt.SetToFormat("Confirm pairing with %s only if the other device "
			"shows the same code: %06" B_PRIu32, name.String(),
			event->passkey);
	}
	BMessage* decision = new BMessage(BT_MSG_PAIR_CONFIRM_RESULT);
	decision->AddInt32("hci_id", GetID());
	decision->AddData("bdaddr", B_ANY_TYPE, &event->bdaddr, sizeof(bdaddr_t));
	BAlert* alert = new BAlert("Bluetooth pairing", prompt.String(),
		"Reject", "Confirm", NULL, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
	alert->Go(new BInvoker(decision, be_app_messenger));
}


void
LocalDeviceImpl::ConfirmPairing(bdaddr_t address, bool accepted)
{
	uint16 opcode = PACK_OPCODE(OGF_LINK_CONTROL,
		accepted ? OCF_USER_CONFIRM_REPLY : OCF_USER_CONFIRM_NEG_REPLY);
	BluetoothCommand<typed_command(hci_cp_user_confirm_reply)> command(
		OGF_LINK_CONTROL,
		accepted ? OCF_USER_CONFIRM_REPLY : OCF_USER_CONFIRM_NEG_REPLY);
	command->bdaddr = address;
	if (fHCIDelegate->IssueCommand(command.Data(), command.Size()) != B_OK)
		return;
	BMessage* request = new BMessage;
	request->AddInt16("eventExpected", HCI_EVENT_CMD_COMPLETE);
	request->AddInt16("opcodeExpected", opcode);
	AddWantedEvent(request);
}


void
LocalDeviceImpl::EnablePageScan()
{
	// Page scan only: paired devices can connect, but we are not
	// discoverable unless the user asks for it in the preferences.
	uint8 scan = 0x02;
	if (fProperties != NULL) {
		int8 current;
		if (fProperties->FindInt8("scan_enable", &current) == B_OK)
			scan |= current;
	}
	CLASSIC_LOG(LE_LOG_INFO, "page scan on (scan enable %#x) for paired HID "
		"devices", scan);
	_SendLECommand(PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_WRITE_SCAN_ENABLE),
		&scan, 1);
}


void
LocalDeviceImpl::UserPasskeyNotification(
	struct hci_ev_user_passkey_notification* event)
{
	// We have a display and the device a keyboard (Core Vol 3 Part C
	// 5.2.2.6, Passkey Entry): the user types our number on it.
	ServerRemoteDevice* rd = RemoteDeviceByAddr(event->bdaddr);
	BString name = bdaddrUtils::ToString(event->bdaddr);
	if (rd != NULL && !rd->friendly_name.IsEmpty())
		name = rd->friendly_name;

	CLASSIC_LOG(LE_LOG_INFO, "pairing with %s: the passkey is to be typed on "
		"it", bdaddrUtils::ToString(event->bdaddr).String());

	_ClosePasskeyAlert();
	BString text;
	text.SetToFormat("To pair %s, type this number on it and press Enter:"
		"\n\n%06" B_PRIu32, name.String(), event->passkey);
	BAlert* alert = new BAlert("Bluetooth pairing", text.String(), "Close",
		NULL, NULL, B_WIDTH_AS_USUAL, B_INFO_ALERT);
	alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
	fPasskeyAlert = BMessenger(alert);
	alert->Go(NULL);
}


void
LocalDeviceImpl::ModeChange(struct hci_ev_mode_change* event)
{
	static const char* kModes[] = { "active", "hold", "sniff", "park" };
	uint16 interval = B_LENDIAN_TO_HOST_INT16(event->interval);
	CLASSIC_LOG(event->status == BT_OK ? LE_LOG_INFO : LE_LOG_ERROR,
		"link %#x: %s mode, interval %.2f ms (status %#x)",
		B_LENDIAN_TO_HOST_INT16(event->handle),
		event->mode < 4 ? kModes[event->mode] : "unknown",
		interval * 0.625, event->status);
}


void
LocalDeviceImpl::KeypressNotification(
	struct hci_ev_keypress_notification* event)
{
	CLASSIC_LOG(LE_LOG_DEBUG, "pairing with %s: keypress notification %u",
		bdaddrUtils::ToString(event->bdaddr).String(), event->type);
}


void
LocalDeviceImpl::_ClosePasskeyAlert()
{
	if (fPasskeyAlert.IsValid())
		fPasskeyAlert.SendMessage(B_QUIT_REQUESTED);
	fPasskeyAlert = BMessenger();
}


void
LocalDeviceImpl::SimplePairingComplete(struct hci_ev_simple_pairing_complete* event,
	BMessage* request)
{
	_ClosePasskeyAlert();
	CLASSIC_LOG(event->status == BT_OK ? LE_LOG_INFO : LE_LOG_ERROR,
		"simple pairing with %s: status %#x",
		bdaddrUtils::ToString(event->bdaddr).String(), event->status);
	CLASSIC_LOG(event->status == BT_OK ? LE_LOG_INFO : LE_LOG_ERROR,
		"pairing with %s finished: %#x",
		bdaddrUtils::ToString(event->bdaddr).String(), event->status);
	if (event->status == BT_OK) {
		TRACE_BT("LocalDeviceImpl: %s successfull for %s!\n", __FUNCTION__,
			bdaddrUtils::ToString(event->bdaddr).String());

	} else {
		TRACE_BT("LocalDeviceImpl: %s failed for %s with error %s\n", __FUNCTION__,
			bdaddrUtils::ToString(event->bdaddr).String(), BluetoothError(event->status));
	}
	if (request != NULL)
		ClearWantedEvent(request);
}


void
LocalDeviceImpl::AuthComplete(struct hci_ev_auth_complete* eventData, BMessage* request)
{
	uint16 handle = B_LENDIAN_TO_HOST_INT16(eventData->handle);
	uint8 status = eventData->status;
	CLASSIC_LOG(status == BT_OK ? LE_LOG_INFO : LE_LOG_ERROR,
		"authentication of link %#x: %#x", handle, status);

	if (status == BT_OK) {
		TRACE_BT("LocalDeviceImpl: Authentication Successful for handle %d\n", handle);
		ServerRemoteDevice* rd = RemoteDeviceByHandle(handle);
		if (rd != NULL) {
			SetConnEncryption(rd->handle, true);
			((BluetoothServer*)be_app)->DiscoverServices(rd);
		}
	} else {
		TRACE_BT("LocalDeviceImpl: Authentication Failed for handle %d with status 0x%02x\n",
			handle, status);

		ServerRemoteDevice* rd = RemoteDeviceByHandle(handle);
		if (rd != NULL) {
			rd->link_key = LinkKeyUtils::NullKey();
			SaveRemoteDevices();
			BMessage disconnReq;
			bdaddr_t bdaddr = rd->bdaddr;
			disconnReq.AddData("bdaddr", B_ANY_TYPE, &bdaddr, sizeof(bdaddr_t));
			disconnReq.AddUInt8("reason", BT_AUTHENTICATION_FAILURE);
			Disconnect(&disconnReq);
		}
	}

	if (request != NULL) {
		BMessage reply;
		reply.AddUInt8("status", status);
		reply.AddUInt16("handle", handle);

		request->SendReply(&reply);

		ClearWantedEvent(request);
	} else {
		TRACE_BT("LocalDeviceImpl: Auth Complete received but no local request was waiting.\n");
	}
}


void
LocalDeviceImpl::EncryptChange(struct hci_ev_encrypt_change* event)
{
	TRACE_BT("LocalDeviceImpl: %s: Handle=%#x, encrypt=%x, status=%x\n",
		__FUNCTION__, event->handle, event->encrypt, event->status);
	uint16 handle = B_LENDIAN_TO_HOST_INT16(event->handle);
	if (HandleLEEncryptionChange(handle, event->status, event->encrypt != 0))
		return;

	ServerRemoteDevice* rd = RemoteDeviceByHandle(handle);
	CLASSIC_LOG(event->status == BT_OK ? LE_LOG_INFO : LE_LOG_ERROR,
		"encryption of link %#x: status %#x, %s%s", handle, event->status,
		event->encrypt ? "on" : "off", rd == NULL ? " (unknown link)" : "");
	if (rd == NULL)
		return;

	rd->encryption_enabled = event->encrypt;
	if (rd->encryption_enabled != 0)
		((BluetoothServer*)be_app)->NotifyServices(rd);
}


#if 0
#pragma mark - Request Methods -
#endif

status_t
LocalDeviceImpl::ProcessSimpleRequest(BMessage* request)
{
	ssize_t size;
	void* command = NULL;

	if (request->FindData("raw command", B_ANY_TYPE, 0,
		(const void **)&command, &size) == B_OK) {

		// Give the chance of just issuing the command
		int16 eventFound;
		if (request->FindInt16("eventExpected", &eventFound) == B_OK)
			AddWantedEvent(request);
		// LEAK: is command buffer freed within the Message?
		if (((HCITransportAccessor*)fHCIDelegate)->IssueCommand(command, size)
			== B_ERROR) {
			// TODO:
			// Reply the request with error!
			// Remove the just added request
			TRACE_BT("LocalDeviceImpl: ## ERROR Command issue, REMOVING!\n");
			ClearWantedEvent(request);

		} else {
			return B_OK;
		}
	} else {
		TRACE_BT("LocalDeviceImpl: No command specified for simple request\n");
	}

	return B_ERROR;
}
