/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef _LE_PAIRING_SESSION_H_
#define _LE_PAIRING_SESSION_H_

#include <SupportDefs.h>

class BMessage;
class BMessenger;


namespace Bluetooth {

// Progress notices carry "stage" (int32, LEPairingStage) and, when useful,
// "detail" (string) describing what the step is waiting for.
static const uint32 LE_PAIRING_PROGRESS_MESSAGE = 'lePG';

enum LEPairingStage {
	LE_PAIRING_CONNECT,
	LE_PAIRING_BOND_LOOKUP,
	LE_PAIRING_SMP,
	LE_PAIRING_SMP_SOCKET,
	LE_PAIRING_SMP_REQUEST,
	LE_PAIRING_SMP_RESPONSE,
	LE_PAIRING_SMP_CONFIRM,
	LE_PAIRING_SMP_RANDOM,
	LE_PAIRING_ENCRYPT,
	LE_PAIRING_KEYS,
	LE_PAIRING_SAVE,
	LE_PAIRING_HID,
	LE_PAIRING_COMPLETE,
	LE_PAIRING_CLEANUP
};

struct LEPairingResult {
	status_t status;
	LEPairingStage stage;
	bool encrypted;
	bool bonded;
	bool reusedBond;
	bool hidMouseReady;
	status_t hidStatus;
	bool secureConnections;
		// paired with LE Secure Connections (else legacy pairing)
	bool authenticated;
		// MITM protected
	char detail[256];
		// Human-readable reason for a failure (or a HID warning), empty on
		// plain success. More detail is in the LE log (see LELog.h).
};

enum {
	LE_PAIRING_LEGACY_ONLY = 0x01
		// do not offer LE Secure Connections
};

// Runs on a worker thread. The peer address is in Bluetooth wire order.
// Connects and pairs an explicitly selected LE peer, then disconnects. An
// explicit pairing always runs a fresh exchange and replaces a stored bond:
// a peripheral in pairing mode has normally discarded its old keys.
// The pairing is Just Works, with LE Secure Connections when the device
// supports it (else legacy pairing); Just Works does not authenticate the
// peer against an active attacker.
LEPairingResult PairLEDevice(int32 hciID, const uint8 localAddress[6],
	const uint8 peerAddress[6], uint8 peerAddressType,
	const BMessenger* progress = NULL, uint32 flags = 0);

// Pairing for applications (BT_MSG_LE_PAIR in bluetoothserver_p.h).
enum {
	LE_SECURE_FRESH			= 0x01,
		// pair again even when a bond is stored
	LE_SECURE_BONDED_ONLY	= 0x02
		// only encrypt with a stored bond; never prompt or pair
};

struct LELinkSecurity {
	status_t status;
		// B_OK, B_CANCELED (the user declined), B_PERMISSION_DENIED (the
		// device refused), B_BAD_DATA, B_ENTRY_NOT_FOUND (no bond with
		// LE_SECURE_BONDED_ONLY), B_BUSY, B_NOT_ALLOWED, B_TIMED_OUT, ...
	uint8 smpReason;
		// SMP Pairing Failed reason, 0 for none
	bool smpReasonRemote;
		// the device sent it
	uint8 hciStatus;
		// of a failed encryption, 0 for none
	bool encrypted;
	bool bonded;
		// a bond is stored for the next connection
	bool reusedBond;
		// encrypted with the stored bond, without pairing
	bool secureConnections;
	bool authenticated;
	int32 method;
		// LESMPMethod of a new pairing, -1 for none
	uint8 keySize;
	char detail[256];
		// a sentence for the user, empty on plain success
};

// Fills a LELinkSecurity from a BT_MSG_LE_PAIR_RESULT, for callers that
// send BT_MSG_LE_PAIR themselves with a reply handler.
void LELinkSecurityFromReply(const BMessage& reply, LELinkSecurity& result);

// Asks the Bluetooth server to secure the LE link the caller holds (the
// listener it connected with) to the peer: encrypted with the stored bond,
// or paired after the user agrees in the system's prompt, then bonded.
// Blocks until done, up to timeout (the prompt waits for the user);
// deviceName, when known, is shown in the prompt. Returns result.status.
status_t SecureLELink(int32 hciID, const BMessenger& listener,
	const uint8 peerAddress[6], LELinkSecurity& result, uint32 flags = 0,
	const char* deviceName = NULL, bigtime_t timeout = 180000000);

// Holds an encrypted LE connection to a previously bonded peer. The ATT
// client can be used while this object remains connected. Other clients may
// hold links to other peers on the same controller meanwhile.
class LEEncryptedLink {
public:
	LEEncryptedLink();
	~LEEncryptedLink();

	// Waits up to scanTimeout for the peer to advertise (the Bluetooth
	// server shares one scan among its clients), then connects and encrypts
	// with the stored key.
	status_t ConnectBonded(int32 hciID, const uint8 localAddress[6],
		const uint8 peerAddress[6], uint8 peerAddressType,
		bigtime_t scanTimeout = 8000000);
	// For the connection ConnectBonded() creates (1.25 ms units, events,
	// 10 ms units); the server's defaults otherwise.
	void SetConnectionParameters(uint16 intervalMin, uint16 intervalMax,
		uint16 latency, uint16 timeout);
	// Asks for this interval range on the link now and whenever the peer
	// moves it elsewhere, keeping the peer's latency and timeout.
	status_t KeepInterval(uint16 intervalMin, uint16 intervalMax);
	void Disconnect();
	bool IsConnected() const;
	void ConnectionAddress(uint8 address[6]) const;

private:
	struct Impl;
	Impl* fImpl;
	LEEncryptedLink(const LEEncryptedLink&);
	LEEncryptedLink& operator=(const LEEncryptedLink&);
};

} // namespace Bluetooth

#endif
