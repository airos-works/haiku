/*
 * Copyright 2007 Oliver Ruiz Dorantes, oliver.ruiz.dorantes_at_gmail.com
 * All rights reserved. Distributed under the terms of the MIT License.
 */
#ifndef _LOCALDEVICE_IMPL_H_
#define _LOCALDEVICE_IMPL_H_

#include <String.h>
#include <Locker.h>
#include <Messenger.h>

#include <vector>

#include <bluetooth/bluetooth.h>
#include <bluetooth/L2CAP/btL2CAP.h>
#include <bluetoothserver_p.h>
#include <bluetooth/RemoteDevice.h>
#include <ObjectList.h>

#include "LocalDeviceHandler.h"

#include "HCIDelegate.h"
#include "HCIControllerAccessor.h"
#include "HCITransportAccessor.h"


struct ServerRemoteDevice
{
	bdaddr_t			bdaddr;
	BString     		friendly_name;
	uint16				clock_offset;
	uint8				pscan_rep_mode;
	uint8				classOfDevice[3];
	linkkey_t			link_key;
	uint8				key_type;
		// of link_key, from Link Key Notification (Core Vol 4 Part E
		// 7.7.24)
	uint8				link_type;
	uint8				encryption_enabled;
	uint8				remote_io_capability;
		// from the IO Capability Response of the current pairing
	bool				outgoing;
		// the current link was asked for here

	uint16				handle;
	RemoteDevice::ConnectionState			conn_state;
	BMessage			services;
};

typedef BObjectList<ServerRemoteDevice> RemoteDevicesList;

class LocalDeviceImpl : public LocalDeviceHandler {

private:
	LocalDeviceImpl(HCIDelegate* hd);

public:

	// Factory methods
	static LocalDeviceImpl* CreateControllerAccessor(BPath* path);
	static LocalDeviceImpl* CreateTransportAccessor(BPath* path);
	~LocalDeviceImpl();
	void Unregister();

	void HandleEvent(struct hci_event_header* event);

	// Request handling
	status_t 	ProcessSimpleRequest(BMessage* request);
	// Low Energy (LocalDeviceLE.cpp). Scanning is shared: every listener
	// receives advertisements until it stops. One LE link at a time.
	status_t StartLEScan(const BMessenger& listener);
	status_t StopLEScan(const BMessenger* listener);
		// NULL stops the scan for every listener.
	status_t StartLEConnection(const BMessenger& listener,
		const uint8* address, uint8 addressType);
	status_t CancelLEConnection(const BMessenger* listener);
	status_t DisconnectLEConnection(const BMessenger* listener);
	status_t StartLEEncryption(const uint8* key, const uint8* randomNumber,
		uint16 encryptedDiversifier);
	void LEPulse();
	void LEInquiryState(bool active);
	void Pulse();
		// Periodic housekeeping: HCI command queue and LE watchdogs.
	status_t StartPairingSetup();

	// Connection
	void CreateConnection(BMessage* message);
	void CancelConnection(BMessage* message);
	void Disconnect(BMessage* message);
	void SetConnEncryption(uint16 handle, bool encryption_enabled);
	void ConfirmPairing(bdaddr_t address, bool accepted);
	void EnablePageScan();
		// so that paired HID devices can reconnect

	ServerRemoteDevice*	RemoteDeviceByAddr(bdaddr_t bdaddr);
	ServerRemoteDevice*	RemoteDeviceByHandle(uint16 handle);
	void				AddRemoteDevice(ServerRemoteDevice* rd);
	void				RemoveRemoteDevice(ServerRemoteDevice* rd);
	RemoteDevicesList*	GetRemoteDevicesList();

private:
	enum PairSetupState {
		PAIR_SETUP_IDLE,
		PAIR_SETUP_BUFFER_SIZE,
		PAIR_SETUP_LE_BUFFER_SIZE,
		PAIR_SETUP_EVENT_MASK,
		PAIR_SETUP_SSP,
		PAIR_SETUP_SC,
		PAIR_SETUP_LE_HOST
	};
	PairSetupState fPairSetupState;
	void HandlePairSetupCommandComplete(struct hci_event_header* event);

	// Low Energy state, guarded by fLELock (HCI events arrive on the port
	// listener thread, requests on the application thread).
	enum LEMaskState {
		LE_MASKS_NONE,
		LE_MASKS_CLASSIC,
		LE_MASKS_LE,
		LE_MASKS_READY
	};
	enum LEScanState {
		LE_SCAN_IDLE,
		LE_SCAN_WAIT_MASKS,
		LE_SCAN_PARAMETERS,
		LE_SCAN_ENABLE,
		LE_SCAN_ACTIVE,
		LE_SCAN_DISABLE
	};
	enum LEConnectionState {
		LE_CONN_IDLE,
		LE_CONN_PENDING,
			// waiting for masks or for the scan to pause
		LE_CONN_CREATE_SENT,
		LE_CONN_CONNECTING,
		LE_CONN_CANCELING,
		LE_CONN_CONNECTED,
		LE_CONN_DISCONNECTING
	};
	struct LEAdvertiserSeen {
		uint8 address[6];
		uint8 addressType;
		uint8 eventType;
		bigtime_t when;
	};

	BLocker fLELock;
	LEMaskState fLEMaskState;
	LEScanState fLEScanState;
	bigtime_t fLEScanSince;
	bigtime_t fLECommandSince;
		// when the last LE command went to the controller
	bool fClassicInquiryActive;
	bigtime_t fClassicInquirySince;
	std::vector<BMessenger> fLEScanListeners;
	std::vector<LEAdvertiserSeen> fLESeen;

	LEConnectionState fLEConnectionState;
	bigtime_t fLEConnectionSince;
	bool fLECancelRequested;
	BMessenger fLEConnectionListener;
	uint8 fLEConnectionAddress[6];
	uint8 fLEConnectionAddressType;
	uint16 fLEConnectionHandle;
	bool fLEEncryptionPending;
	bool fLEEncrypted;

	status_t _SendLECommand(uint16 opcode, const void* data, size_t length);
	void _EnsureLEMasks();
	void _UpdateLE();
	bool _LEInitiating() const;
	void _FailLEScan(uint8 status);
	status_t _IssueLECreateConnection();
	void _SendLEScanNotice(uint32 what, uint8 status = 0);
	void _SendLEConnectionNotice(uint32 what, status_t status = B_OK);
	void _ResetLEConnection();
	bool _ForwardAdvertisement(const uint8* report, size_t dataLength);

	void HandleLECommandComplete(struct hci_event_header* event);
	void HandleLECommandStatus(struct hci_event_header* event);
	void HandleLEMeta(struct hci_event_header* event);
	void HandleLEAdvertisingReport(struct hci_event_header* event);
	void HandleLEConnectionComplete(struct hci_event_header* event);
	void HandleLERemoteParameterRequest(struct hci_event_header* event);
	bool HandleLEDisconnection(uint16 handle, uint8 status, uint8 reason);
	bool HandleLEEncryptionChange(uint16 handle, uint8 status, bool enabled);

	RemoteDevicesList	fRemoteDevicesList;

	void SaveRemoteDevices();
	void LoadRemoteDevices();

	void HandleUnexpectedEvent(struct hci_event_header* event);
	void HandleExpectedRequest(struct hci_event_header* event,
		BMessage* request);

	// Events handling
	void CommandComplete(struct hci_ev_cmd_complete* event, BMessage* request,
		int32 index);
	void CommandStatus(struct hci_ev_cmd_status* event, BMessage* request,
		int32 index);

	void NumberOfCompletedPackets(struct hci_ev_num_comp_pkts* event);

	// Inquiry
	void InquiryResult(uint8* numberOfResponses, BMessage* request);
	void InquiryResultWithRSSI(uint8* numberOfResponses, BMessage* request);
	void ExtendedInquiryResult(uint8* numberOfResponses, BMessage* request);
	void ParseEIR(const uint8* eir, BMessage& reply);
	void InquiryComplete(uint8* status, BMessage* request);
	void RemoteNameRequestComplete(struct hci_ev_remote_name_request_complete_reply*
		remotename, BMessage* request);

	// Connection
	void Authenticate(uint16 handle);
	void ConnectionComplete(struct hci_ev_conn_complete* event);
	void ConnectionRequest(struct hci_ev_conn_request* event, BMessage* request);
	void DisconnectionComplete(struct hci_ev_disconnection_complete_reply* event);

	// Pairing
	void PinCodeRequest(struct hci_ev_pin_code_req* event, BMessage* request);
	void RoleChange(struct hci_ev_role_change* event, BMessage* request);
	void LinkKeyNotify(struct hci_ev_link_key_notify* event, BMessage* request);
	void ReturnLinkKeys(struct hci_ev_return_link_keys* returnedKeys);

	void LinkKeyRequested(struct hci_ev_link_key_req* keyReqyested,
		BMessage* request);

	void PageScanRepetitionModeChange(struct hci_ev_page_scan_rep_mode_change* event,
		BMessage* request);
	void MaxSlotChange(struct hci_ev_max_slot_change* event, BMessage* request);

	void HardwareError(struct hci_ev_hardware_error* event);

	// Simple Secure Pairing
	void IOCapabilityRequest(struct hci_ev_io_capability_request* event,
		BMessage* request);
	void IOCapabilityResponse(struct hci_ev_io_capability_response* event,
		BMessage* request);
	void UserConfirmationRequest(struct hci_ev_user_confirmation_request* event, BMessage* request);
	void UserPasskeyNotification(
		struct hci_ev_user_passkey_notification* event);
	void KeypressNotification(struct hci_ev_keypress_notification* event);
	void SimplePairingComplete(struct hci_ev_simple_pairing_complete* event,
		BMessage* request);
	void _ClosePasskeyAlert();
	BMessenger fPasskeyAlert;
		// the "type this code on the keyboard" alert
	void AuthComplete(struct hci_ev_auth_complete* eventData, BMessage* request);

	void EncryptChange(struct hci_ev_encrypt_change* eventData);
};

#endif
