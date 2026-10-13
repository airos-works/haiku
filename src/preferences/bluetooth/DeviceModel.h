/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef DEVICE_MODEL_H
#define DEVICE_MODEL_H


#include <Message.h>
#include <String.h>

#include <bluetooth/DeviceClass.h>

#include <map>
#include <vector>

#include "LEAdvertisingData.h"


class DeviceItem;


enum device_kind {
	DEVICE_KIND_GENERIC = 0,
	DEVICE_KIND_MOUSE,
	DEVICE_KIND_KEYBOARD,
	DEVICE_KIND_INPUT,
	DEVICE_KIND_AUDIO,
	DEVICE_KIND_PHONE,
	DEVICE_KIND_COMPUTER,
	DEVICE_KIND_GAMEPAD
};


// Addresses are kept in Bluetooth wire order (least significant byte first),
// like bdaddr_t. A key packs the six bytes so Classic and LE reports of the
// same address merge into one row.
typedef uint64 device_key;

device_key		KeyForAddress(const uint8 address[6]);
void			AddressForKey(device_key key, uint8 address[6]);
BString			AddressString(const uint8 address[6]);

// Rejects empty names, kit error placeholders ("#...#Not Valid name") and
// names that merely repeat the address. The result is trimmed and has control
// characters replaced.
bool			SanitizeName(const char* name, const uint8 address[6],
					BString& result);

device_kind		KindForClass(DeviceClass deviceClass);
device_kind		KindForAppearance(uint16 appearance, bool advertisesHID);
bool			IsInputKind(device_kind kind);
const char*		KindLabel(device_kind kind);
BString			FallbackName(device_kind kind, const uint8 address[6]);
int32			SignalBars(int32 rssi);


// Names and kinds seen for devices this computer has paired with. The kit
// only stores an address for LE bonds, and a Classic remote name is often
// unavailable, so the app remembers the best name it has seen.
class DeviceNameCache {
public:
								DeviceNameCache();

			void				Load();
			void				Save();

			bool				Lookup(device_key key, BString& name,
									device_kind& kind) const;
			void				Set(device_key key, const BString& name,
									device_kind kind);
			void				Remove(device_key key);

private:
			struct Entry {
				BString			name;
				device_kind		kind;
			};

			std::map<device_key, Entry> fEntries;
			bool				fDirty;
};


// One physical device seen by discovery, merged from LE advertisements and
// Classic inquiry results that share an address.
struct NearbyDevice {
								NearbyDevice(device_key key);

			void				MergeAdvertisement(uint8 addressType,
									uint8 eventType,
									const LEAdvertisingData& advertising,
									bool hasRSSI, int8 rssi);
			void				MergeClassic(uint32 deviceClass,
									const char* name,
									uint8 pageRepetitionMode,
									uint16 clockOffset);
			void				SetClassicName(const BString& name);

			bool				HasName() const;
			BString				Name() const;
			device_kind			Kind() const;
			bool				IsHID() const;
			bool				UsesLEPairing() const;
			bool				CanConnect() const;
			// Classic devices, and LE ones that are HID; other LE devices
			// (beacons, speakers' setup channels) are only listed on request.
			bool				IsSupported() const;
			bool				InPairingMode() const;
			bool				NeedsNameLookup() const;
			bool				IsCompanionOf(const NearbyDevice& other) const;
			void				UpdateRSSI(int8 rssi);

			device_key			key;
			uint8				address[6];

			bool				hasLE;
			uint8				leAddressType;
			bool				connectable;
			uint16				appearance;
			bool				advertisesHID;
			BString				leName;
			bool				leNameComplete;

			bool				hasClassic;
			DeviceClass			deviceClass;
			BString				classicName;
			uint8				pageRepetitionMode;
			uint16				clockOffset;
			bool				nameLookupDone;

			BString				cachedName;
			device_kind			cachedKind;

			// Dual-mode devices often use a second public address for LE,
			// next to the Classic one. An unnamed Classic device borrows the
			// name of such an LE twin, which is then not listed separately.
			BString				companionName;
			device_key			companionOf;

			bool				hasRSSI;
			int32				rssi;
			bigtime_t			lastSeen;

			DeviceItem*			item;
};


// A device from the server's paired list (Classic) or the LE bond store.
struct PairedDevice {
								PairedDevice(device_key key);

			BString				Name() const;
			device_kind			Kind() const;

			device_key			key;
			uint8				address[6];

			bool				isLE;
			uint8				leAddressType;
			bool				leMouse;
			uint8				leLocalAddressType;
			uint8				leLocalAddress[6];

			DeviceClass			deviceClass;
			BString				serverName;
			int32				connectionState;
			uint8				pageRepetitionMode;
			uint16				clockOffset;

			BString				knownName;
			device_kind			knownKind;

			DeviceItem*			item;
};


struct LEBondedPeer {
	uint8				localAddressType;
	uint8				localAddress[6];
	uint8				peerAddress[6];
	uint8				peerAddressType;
	bool				mouse;
};

// Lists every LE bond in the bond store directory. The kit only exposes
// the bonds that carry a HID mouse marker (ListLEHIDMice).
status_t		ListLEBondedPeers(std::vector<LEBondedPeer>& peers);


#endif	// DEVICE_MODEL_H
