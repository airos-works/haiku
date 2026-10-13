/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */


#include "DeviceModel.h"

#include <Catalog.h>
#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <FindDirectory.h>
#include <OS.h>
#include <Path.h>

#include <bluetooth/RemoteDevice.h>

#include <LEBondStore.h>

#include <stdio.h>
#include <string.h>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Devices"


static const char* kNameCacheFile = "Bluetooth_device_names";
static const int32 kInvalidRSSI = 127;


device_key
KeyForAddress(const uint8 address[6])
{
	device_key key = 0;
	for (int i = 5; i >= 0; i--)
		key = (key << 8) | address[i];
	return key;
}


void
AddressForKey(device_key key, uint8 address[6])
{
	for (int i = 0; i < 6; i++) {
		address[i] = key & 0xff;
		key >>= 8;
	}
}


BString
AddressString(const uint8 address[6])
{
	char text[18];
	snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
		address[5], address[4], address[3], address[2], address[1],
		address[0]);
	return BString(text);
}


bool
SanitizeName(const char* name, const uint8 address[6], BString& result)
{
	result = "";
	if (name == NULL)
		return false;

	BString candidate(name);
	for (int32 i = 0; i < candidate.Length(); i++) {
		if ((uint8)candidate[i] < 0x20)
			candidate.SetByteAt(i, ' ');
	}
	candidate.Trim();
	if (candidate.IsEmpty())
		return false;

	// Older kits return "#CommandFailed#Not Valid name" and similar strings
	// when a remote-name request fails.
	if (candidate.ByteAt(0) == '#' || candidate.IFindFirst("Not Valid name") >= 0)
		return false;

	if (address != NULL && candidate.ICompare(AddressString(address)) == 0)
		return false;

	result = candidate;
	return true;
}


device_kind
KindForClass(DeviceClass deviceClass)
{
	uint8 minor = deviceClass.MinorDeviceClass();
	switch (deviceClass.MajorDeviceClass()) {
		case 1:
			return DEVICE_KIND_COMPUTER;
		case 2:
			return DEVICE_KIND_PHONE;
		case 4:
			return DEVICE_KIND_AUDIO;
		case 5:
		{
			// Peripheral: bits 5-4 describe keyboard/pointing, bits 3-0 the
			// device subtype.
			switch ((minor >> 4) & 0x3) {
				case 1:
				case 3:
					return DEVICE_KIND_KEYBOARD;
				case 2:
					return DEVICE_KIND_MOUSE;
			}
			if ((minor & 0xf) == 1 || (minor & 0xf) == 2)
				return DEVICE_KIND_GAMEPAD;
			return DEVICE_KIND_INPUT;
		}
		default:
			return DEVICE_KIND_GENERIC;
	}
}


device_kind
KindForAppearance(uint16 appearance, bool advertisesHID)
{
	uint16 category = appearance >> 6;
	uint16 subcategory = appearance & 0x3f;
	switch (category) {
		case 0x01:
			return DEVICE_KIND_PHONE;
		case 0x02:
			return DEVICE_KIND_COMPUTER;
		case 0x0f:
			switch (subcategory) {
				case 0x01:
					return DEVICE_KIND_KEYBOARD;
				case 0x02:
				case 0x05:
					return DEVICE_KIND_MOUSE;
				case 0x03:
				case 0x04:
					return DEVICE_KIND_GAMEPAD;
				default:
					return DEVICE_KIND_INPUT;
			}
		case 0x21:
		case 0x22:
		case 0x25:
			return DEVICE_KIND_AUDIO;
	}
	return advertisesHID ? DEVICE_KIND_INPUT : DEVICE_KIND_GENERIC;
}


bool
IsInputKind(device_kind kind)
{
	return kind == DEVICE_KIND_MOUSE || kind == DEVICE_KIND_KEYBOARD
		|| kind == DEVICE_KIND_INPUT || kind == DEVICE_KIND_GAMEPAD;
}


const char*
KindLabel(device_kind kind)
{
	switch (kind) {
		case DEVICE_KIND_MOUSE:
			return B_TRANSLATE("Mouse");
		case DEVICE_KIND_KEYBOARD:
			return B_TRANSLATE("Keyboard");
		case DEVICE_KIND_INPUT:
			return B_TRANSLATE("Input device");
		case DEVICE_KIND_AUDIO:
			return B_TRANSLATE("Audio device");
		case DEVICE_KIND_PHONE:
			return B_TRANSLATE("Phone");
		case DEVICE_KIND_COMPUTER:
			return B_TRANSLATE("Computer");
		case DEVICE_KIND_GAMEPAD:
			return B_TRANSLATE("Game controller");
		default:
			return B_TRANSLATE("Bluetooth device");
	}
}


BString
FallbackName(device_kind kind, const uint8 address[6])
{
	BString name;
	if (kind == DEVICE_KIND_GENERIC)
		name = B_TRANSLATE("Unknown device (%address%)");
	else {
		name = B_TRANSLATE_COMMENT("%kind% (%address%)",
			"An unnamed device, e.g. \"Mouse (CB:8C:04:89:64:E6)\"");
		name.ReplaceFirst("%kind%", KindLabel(kind));
	}
	name.ReplaceFirst("%address%", AddressString(address));
	return name;
}


int32
SignalBars(int32 rssi)
{
	if (rssi >= -60)
		return 4;
	if (rssi >= -70)
		return 3;
	if (rssi >= -80)
		return 2;
	return 1;
}


//	#pragma mark - DeviceNameCache


DeviceNameCache::DeviceNameCache()
	:
	fDirty(false)
{
}


void
DeviceNameCache::Load()
{
	fEntries.clear();
	fDirty = false;

	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK
		|| path.Append(kNameCacheFile) != B_OK)
		return;

	BFile file(path.Path(), B_READ_ONLY);
	BMessage archive;
	if (file.InitCheck() != B_OK || archive.Unflatten(&file) != B_OK)
		return;

	BMessage device;
	for (int32 i = 0; archive.FindMessage("device", i, &device) == B_OK;
			i++) {
		const void* address;
		ssize_t size;
		const char* name;
		if (device.FindData("address", B_RAW_TYPE, &address, &size) != B_OK
			|| size != 6 || device.FindString("name", &name) != B_OK)
			continue;
		BString clean;
		if (!SanitizeName(name, (const uint8*)address, clean))
			continue;
		Entry entry;
		entry.name = clean;
		entry.kind = (device_kind)device.GetInt32("kind", DEVICE_KIND_GENERIC);
		fEntries[KeyForAddress((const uint8*)address)] = entry;
	}
}


void
DeviceNameCache::Save()
{
	if (!fDirty)
		return;

	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK
		|| path.Append(kNameCacheFile) != B_OK)
		return;

	BMessage archive;
	std::map<device_key, Entry>::const_iterator iterator;
	for (iterator = fEntries.begin(); iterator != fEntries.end(); iterator++) {
		uint8 address[6];
		AddressForKey(iterator->first, address);
		BMessage device;
		device.AddData("address", B_RAW_TYPE, address, sizeof(address));
		device.AddString("name", iterator->second.name);
		device.AddInt32("kind", iterator->second.kind);
		archive.AddMessage("device", &device);
	}

	BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	if (file.InitCheck() == B_OK && archive.Flatten(&file) == B_OK)
		fDirty = false;
}


bool
DeviceNameCache::Lookup(device_key key, BString& name, device_kind& kind) const
{
	std::map<device_key, Entry>::const_iterator found = fEntries.find(key);
	if (found == fEntries.end())
		return false;
	name = found->second.name;
	kind = found->second.kind;
	return true;
}


void
DeviceNameCache::Set(device_key key, const BString& name, device_kind kind)
{
	std::map<device_key, Entry>::iterator found = fEntries.find(key);
	if (found != fEntries.end() && found->second.name == name
		&& found->second.kind == kind)
		return;
	Entry entry;
	entry.name = name;
	entry.kind = kind;
	fEntries[key] = entry;
	fDirty = true;
}


void
DeviceNameCache::Remove(device_key key)
{
	if (fEntries.erase(key) > 0)
		fDirty = true;
}


//	#pragma mark - NearbyDevice


NearbyDevice::NearbyDevice(device_key key)
	:
	key(key),
	hasLE(false),
	leAddressType(0),
	connectable(false),
	appearance(0),
	advertisesHID(false),
	leNameComplete(false),
	hasClassic(false),
	pageRepetitionMode(0),
	clockOffset(0),
	nameLookupDone(false),
	cachedKind(DEVICE_KIND_GENERIC),
	companionOf(0),
	hasRSSI(false),
	rssi(kInvalidRSSI),
	lastSeen(system_time()),
	item(NULL)
{
	AddressForKey(key, address);
}


void
NearbyDevice::MergeAdvertisement(uint8 addressType, uint8 eventType,
	const LEAdvertisingData& advertising, bool hasRSSIValue, int8 rssiValue)
{
	hasLE = true;
	leAddressType = addressType;
	lastSeen = system_time();

	// A scan response can follow a connectable advertisement. Retain that
	// observation when later packets contain only scan-response data.
	if (eventType == 0 || eventType == 1)
		connectable = true;

	if (!advertising.name.empty()
		&& (advertising.completeName || !leNameComplete)) {
		BString name;
		if (SanitizeName(BString(advertising.name.data(),
				advertising.name.size()), address, name)) {
			leName = name;
			leNameComplete = advertising.completeName;
		}
	}
	if (advertising.appearance != 0)
		appearance = advertising.appearance;
	advertisesHID |= advertising.advertisesHID;

	if (hasRSSIValue)
		UpdateRSSI(rssiValue);
}


void
NearbyDevice::MergeClassic(uint32 record, const char* name,
	uint8 repetitionMode, uint16 offset)
{
	hasClassic = true;
	uint8 bytes[3] = { (uint8)(record & 0xff), (uint8)((record >> 8) & 0xff),
		(uint8)((record >> 16) & 0xff) };
	deviceClass.SetRecord(bytes);
	pageRepetitionMode = repetitionMode;
	clockOffset = offset;
	lastSeen = system_time();

	BString clean;
	if (SanitizeName(name, address, clean))
		classicName = clean;
}


void
NearbyDevice::SetClassicName(const BString& name)
{
	BString clean;
	if (SanitizeName(name, address, clean))
		classicName = clean;
	nameLookupDone = true;
}


void
NearbyDevice::UpdateRSSI(int8 value)
{
	// RSSI jumps around by several dB between reports; smooth it so the
	// signal bars do not flicker. The server reports each advertiser only a
	// few times a minute, so recent values count as much as the old one.
	if (!hasRSSI || rssi == kInvalidRSSI)
		rssi = value;
	else
		rssi = (rssi + value) / 2;
	hasRSSI = true;
}


bool
NearbyDevice::HasName() const
{
	return !leName.IsEmpty() || !classicName.IsEmpty() || !cachedName.IsEmpty()
		|| !companionName.IsEmpty();
}


BString
NearbyDevice::Name() const
{
	// LE complete name > Classic remote name > LE short name > remembered.
	if (!leName.IsEmpty() && leNameComplete)
		return leName;
	if (!classicName.IsEmpty())
		return classicName;
	if (!leName.IsEmpty())
		return leName;
	if (!cachedName.IsEmpty())
		return cachedName;
	if (!companionName.IsEmpty())
		return companionName;
	return FallbackName(Kind(), address);
}


device_kind
NearbyDevice::Kind() const
{
	if (hasLE && (appearance != 0 || advertisesHID)) {
		device_kind kind = KindForAppearance(appearance, advertisesHID);
		if (kind != DEVICE_KIND_GENERIC)
			return kind;
	}
	if (hasClassic) {
		device_kind kind = KindForClass(deviceClass);
		if (kind != DEVICE_KIND_GENERIC)
			return kind;
	}
	return cachedKind;
}


bool
NearbyDevice::IsHID() const
{
	if (hasLE && (advertisesHID
			|| IsInputKind(KindForAppearance(appearance, false))))
		return true;
	return false;
}


bool
NearbyDevice::UsesLEPairing() const
{
	if (!hasLE)
		return false;
	if (!hasClassic)
		return true;
	// A dual-mode device: HID over GATT is what the LE path can set up; for
	// anything else keep the established Classic path.
	return IsHID();
}


bool
NearbyDevice::CanConnect() const
{
	// Over LE only mice and keyboards (HID over GATT) can be set up; a
	// speaker's or phone's LE beacon is not the device to pair with.
	if (UsesLEPairing())
		return connectable && leAddressType <= 1 && IsHID();
	return hasClassic;
}


bool
NearbyDevice::IsSupported() const
{
	return hasClassic || IsHID();
}


bool
NearbyDevice::InPairingMode() const
{
	return hasLE && connectable && IsHID();
}


bool
NearbyDevice::NeedsNameLookup() const
{
	return hasClassic && !nameLookupDone
		&& classicName.IsEmpty() && !(leNameComplete && !leName.IsEmpty());
}


bool
NearbyDevice::IsCompanionOf(const NearbyDevice& other) const
{
	// This LE-only device and the other Classic-only device share the
	// upper four bytes of a public address (e.g. 00:02:5B:12:50:61 and
	// 00:02:5B:12:53:12 for one audio amplifier).
	return hasLE && !hasClassic && leAddressType == 0
		&& other.hasClassic && !other.hasLE
		&& memcmp(address + 2, other.address + 2, 4) == 0;
}


//	#pragma mark - PairedDevice


PairedDevice::PairedDevice(device_key key)
	:
	key(key),
	isLE(false),
	leAddressType(0),
	leMouse(false),
	leLocalAddressType(0),
	connectionState(Bluetooth::RemoteDevice::DISCONNECTED),
	pageRepetitionMode(0),
	clockOffset(0),
	knownKind(DEVICE_KIND_GENERIC),
	item(NULL)
{
	AddressForKey(key, address);
	memset(leLocalAddress, 0, sizeof(leLocalAddress));
}


BString
PairedDevice::Name() const
{
	BString name;
	if (!isLE && SanitizeName(serverName, address, name))
		return name;
	if (!knownName.IsEmpty())
		return knownName;
	return FallbackName(Kind(), address);
}


device_kind
PairedDevice::Kind() const
{
	if (isLE && leMouse)
		return DEVICE_KIND_MOUSE;
	if (!isLE) {
		device_kind kind = KindForClass(deviceClass);
		if (kind != DEVICE_KIND_GENERIC)
			return kind;
	}
	return knownKind;
}


//	#pragma mark - LE bond listing


static bool
ParseHexAddress(const char* text, uint8 address[6])
{
	// The bond store writes the address most significant byte first.
	for (int i = 0; i < 6; i++) {
		unsigned int byte;
		if (sscanf(text + i * 2, "%2x", &byte) != 1)
			return false;
		address[5 - i] = (uint8)byte;
	}
	return true;
}


status_t
ListLEBondedPeers(std::vector<LEBondedPeer>& peers)
{
	peers.clear();

	char directoryPath[B_PATH_NAME_LENGTH];
	status_t status = Bluetooth::DefaultLEBondDirectory(directoryPath,
		sizeof(directoryPath));
	if (status != B_OK)
		return status;

	std::vector<Bluetooth::LEHIDMouseDevice> mice;
	Bluetooth::ListLEHIDMice(directoryPath, mice);

	BDirectory directory(directoryPath);
	status = directory.InitCheck();
	if (status != B_OK)
		return status;

	char name[B_FILE_NAME_LENGTH];
	BEntry entry;
	while (directory.GetNextEntry(&entry) == B_OK) {
		if (entry.GetName(name) != B_OK)
			continue;
		// "<local type>-<local address>-<peer type>-<peer address>.bond"
		unsigned int localType, peerType;
		char local[13], peer[13], suffix[8];
		if (sscanf(name, "%1u-%12[0-9a-f]-%1u-%12[0-9a-f].%7s", &localType,
				local, &peerType, peer, suffix) != 5
			|| strcmp(suffix, "bond") != 0 || strlen(local) != 12
			|| strlen(peer) != 12 || localType > 1 || peerType > 1)
			continue;

		LEBondedPeer bonded;
		if (!ParseHexAddress(local, bonded.localAddress)
			|| !ParseHexAddress(peer, bonded.peerAddress))
			continue;
		bonded.localAddressType = localType;
		bonded.peerAddressType = peerType;
		bonded.mouse = false;
		for (size_t i = 0; i < mice.size(); i++) {
			if (memcmp(mice[i].peerAddress, bonded.peerAddress, 6) == 0
				&& memcmp(mice[i].localAddress, bonded.localAddress, 6) == 0
				&& mice[i].peerAddressType == bonded.peerAddressType)
				bonded.mouse = true;
		}
		peers.push_back(bonded);
	}
	return B_OK;
}
