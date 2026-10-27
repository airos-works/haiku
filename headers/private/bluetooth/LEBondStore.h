/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef _LE_BOND_STORE_H_
#define _LE_BOND_STORE_H_

#include <LELegacyPairingClient.h>
#include <vector>


namespace Bluetooth {

// Returns the current user's Bluetooth LE bond directory path. SaveLEBond
// creates the directory on first use.
status_t DefaultLEBondDirectory(char* path, size_t capacity);

// The directory must be private to the current user (mode 0700). Save creates
// it if needed. The file is written atomically with mode 0600. Addresses are
// the connection addresses in Bluetooth wire order. Key material is stored
// unencrypted, with the same local-user trust boundary as Haiku's Classic
// Bluetooth paired-device settings.
status_t SaveLEBond(const char* directory, const uint8 localAddress[6],
	uint8 localAddressType, const uint8 peerAddress[6], uint8 peerAddressType,
	const LELegacyBondKey& bond);
status_t LoadLEBond(const char* directory, const uint8 localAddress[6],
	uint8 localAddressType, const uint8 peerAddress[6], uint8 peerAddressType,
	LELegacyBondKey& bond);
status_t RemoveLEBond(const char* directory, const uint8 localAddress[6],
	uint8 localAddressType, const uint8 peerAddress[6], uint8 peerAddressType);

struct LEHIDMouseDevice {
	uint8 localAddress[6];
	uint8 peerAddress[6];
	uint8 peerAddressType;
};

// A marker is written only after an explicitly paired peer exposes a usable
// HID mouse report. It contains no key material; the bond remains authoritative.
status_t SaveLEHIDMouse(const char* directory, const uint8 localAddress[6],
	const uint8 peerAddress[6], uint8 peerAddressType);
status_t ListLEHIDMice(const char* directory,
	std::vector<LEHIDMouseDevice>& devices);

// Where a mouse's discovered HID layout (LEHIDService::Save()) is kept; it
// goes with the bond.
status_t LEHIDLayoutPath(const char* directory, const uint8 localAddress[6],
	const uint8 peerAddress[6], uint8 peerAddressType, char* path,
	size_t capacity);

struct LEBondedDevice {
	uint8 localAddress[6];
	uint8 localAddressType;
	uint8 peerAddress[6];
	uint8 peerAddressType;
	bool mouse;
		// A HID mouse marker exists for it
};

// Lists the bonds in the directory without reading any key material.
status_t ListLEBonds(const char* directory,
	std::vector<LEBondedDevice>& devices);

} // namespace Bluetooth

#endif
