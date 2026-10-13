/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef _LE_PAIRING_SESSION_H_
#define _LE_PAIRING_SESSION_H_

#include <SupportDefs.h>

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
	char detail[256];
		// Human-readable reason for a failure (or a HID warning), empty on
		// plain success. More detail is in the LE log (see LELog.h).
};

// Runs on a worker thread. The peer address is in Bluetooth wire order.
// Connects and pairs an explicitly selected LE peer, then disconnects. An
// explicit pairing always runs a fresh exchange and replaces a stored bond:
// a peripheral in pairing mode has normally discarded its old keys.
// Legacy Just Works does not authenticate the peer against an active attacker.
LEPairingResult PairLEDevice(int32 hciID, const uint8 localAddress[6],
	const uint8 peerAddress[6], uint8 peerAddressType,
	const BMessenger* progress = NULL);

// Holds an encrypted LE connection to a previously bonded peer. The ATT
// client can be used while this object remains connected. Only one LE link
// per controller is currently supported by the Bluetooth server.
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
