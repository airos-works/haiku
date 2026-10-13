/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */

#include <LEBondStore.h>

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <Errors.h>
#ifdef __HAIKU__
#include <FindDirectory.h>
#endif


namespace Bluetooth {

static const uint8 kMouseMagic[8] = { 'H', 'L', 'M', 'O', 'U', 'S', 'E', '1' };
static const size_t kMouseRecordSize = 21;

enum {
	kMagic = 0,
	kLocalType = 8,
	kLocalAddress = 9,
	kPeerType = 15,
	kPeerAddress = 16,
	kIdentityType = 22,
	kIdentityAddress = 23,
	kFlags = 29,
	kKeySize = 30,
	kLongTermKey = 31,
	kRandomNumber = 47,
	kDiversifier = 55,
	kIdentityKey = 57,
	kChecksum = 73,
	kRecordSize = 77
};

static const uint8 kFileMagic[8] = { 'H', 'L', 'B', 'O', 'N', 'D', '0', '1' };


status_t
DefaultLEBondDirectory(char* path, size_t capacity)
{
	if (path == NULL || capacity == 0)
		return B_BAD_VALUE;
	path[0] = '\0';
#ifdef __HAIKU__
	char settings[PATH_MAX];
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, -1, true,
		settings, sizeof(settings));
	if (status != B_OK)
		return status;
	int length = snprintf(path, capacity, "%s/Bluetooth_LE_Bonds", settings);
	return length > 0 && (size_t)length < capacity ? B_OK : B_NAME_TOO_LONG;
#else
	return B_NOT_SUPPORTED;
#endif
}


static void
ClearSecret(void* data, size_t length)
{
	volatile uint8* bytes = (volatile uint8*)data;
	while (length-- > 0)
		*bytes++ = 0;
}


static uint32
Checksum(const uint8* bytes, size_t length)
{
	uint32 value = 0xffffffff;
	for (size_t i = 0; i < length; i++) {
		value ^= bytes[i];
		for (int bit = 0; bit < 8; bit++)
			value = (value >> 1) ^ (0xedb88320 & -(value & 1));
	}
	return ~value;
}


static void
EncodeAddress(const uint8 address[6], char output[13])
{
	for (int i = 0; i < 6; i++)
		snprintf(output + i * 2, 3, "%02x", address[5 - i]);
}


static status_t
BuildPath(const char* directory, const uint8 localAddress[6],
	uint8 localType, const uint8 peerAddress[6], uint8 peerType,
	char* path, size_t capacity)
{
	if (directory == NULL || directory[0] == '\0' || localAddress == NULL
		|| peerAddress == NULL || localType > 1 || peerType > 1)
		return B_BAD_VALUE;
	char local[13] = {}, peer[13] = {};
	EncodeAddress(localAddress, local);
	EncodeAddress(peerAddress, peer);
	int length = snprintf(path, capacity, "%s/%u-%s-%u-%s.bond",
		directory, localType, local, peerType, peer);
	return length > 0 && (size_t)length < capacity ? B_OK : B_NAME_TOO_LONG;
}


static status_t
BuildMousePath(const char* directory, const uint8 localAddress[6],
	const uint8 peerAddress[6], uint8 peerType, char* path, size_t capacity)
{
	char bondPath[PATH_MAX];
	status_t status = BuildPath(directory, localAddress, 0, peerAddress,
		peerType, bondPath, sizeof(bondPath));
	if (status != B_OK)
		return status;
	char* suffix = strrchr(bondPath, '.');
	if (suffix == NULL || strcmp(suffix, ".bond") != 0)
		return B_BAD_DATA;
	*suffix = '\0';
	int length = snprintf(path, capacity, "%s.mouse", bondPath);
	return length > 0 && (size_t)length < capacity ? B_OK : B_NAME_TOO_LONG;
}


status_t
LEHIDLayoutPath(const char* directory, const uint8 localAddress[6],
	const uint8 peerAddress[6], uint8 peerAddressType, char* path,
	size_t capacity)
{
	char mousePath[PATH_MAX];
	status_t status = BuildMousePath(directory, localAddress, peerAddress,
		peerAddressType, mousePath, sizeof(mousePath));
	if (status != B_OK)
		return status;
	int length = snprintf(path, capacity, "%s.layout", mousePath);
	return length > 0 && (size_t)length < capacity ? B_OK : B_NAME_TOO_LONG;
}


static status_t
CheckDirectory(const char* directory, bool create)
{
	if (create && mkdir(directory, 0700) != 0 && errno != EEXIST)
		return B_ERROR;
	struct stat info;
	if (lstat(directory, &info) != 0)
		return errno == ENOENT ? B_ENTRY_NOT_FOUND : B_ERROR;
	if (!S_ISDIR(info.st_mode) || info.st_uid != geteuid()
		|| (info.st_mode & 0077) != 0)
		return B_PERMISSION_DENIED;
	return B_OK;
}


status_t
SaveLEBond(const char* directory, const uint8 localAddress[6],
	uint8 localType, const uint8 peerAddress[6], uint8 peerType,
	const LELegacyBondKey& bond)
{
	char path[PATH_MAX];
	status_t status = BuildPath(directory, localAddress, localType, peerAddress,
		peerType, path, sizeof(path));
	if (status != B_OK)
		return status;
	if (!bond.hasLongTermKey || bond.keySize < 7 || bond.keySize > 16
		|| (bond.hasIdentity && bond.identityAddressType > 1))
		return B_BAD_VALUE;
	status = CheckDirectory(directory, true);
	if (status != B_OK)
		return status;

	uint8 record[kRecordSize] = {};
	memcpy(record + kMagic, kFileMagic, sizeof(kFileMagic));
	record[kLocalType] = localType;
	memcpy(record + kLocalAddress, localAddress, 6);
	record[kPeerType] = peerType;
	memcpy(record + kPeerAddress, peerAddress, 6);
	record[kFlags] = 1;
	record[kKeySize] = bond.keySize;
	memcpy(record + kLongTermKey, bond.longTermKey, 16);
	for (uint8 i = bond.keySize; i < 16; i++)
		record[kLongTermKey + i] = 0;
	memcpy(record + kRandomNumber, bond.randomNumber, 8);
	record[kDiversifier] = bond.encryptedDiversifier;
	record[kDiversifier + 1] = bond.encryptedDiversifier >> 8;
	if (bond.hasIdentity) {
		record[kFlags] |= 2;
		record[kIdentityType] = bond.identityAddressType;
		memcpy(record + kIdentityAddress, bond.identityAddress, 6);
		memcpy(record + kIdentityKey, bond.identityResolvingKey, 16);
	}
	uint32 checksum = Checksum(record, kChecksum);
	for (int i = 0; i < 4; i++)
		record[kChecksum + i] = checksum >> (i * 8);

	char temporary[PATH_MAX];
	int length = snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path);
	if (length <= 0 || (size_t)length >= sizeof(temporary)) {
		ClearSecret(record, sizeof(record));
		return B_NAME_TOO_LONG;
	}
	int descriptor = mkstemp(temporary);
	if (descriptor < 0) {
		ClearSecret(record, sizeof(record));
		return B_ERROR;
	}
	status = B_OK;
	if (fchmod(descriptor, 0600) != 0)
		status = B_ERROR;
	for (size_t offset = 0; status == B_OK && offset < sizeof(record);) {
		ssize_t written = write(descriptor, record + offset,
			sizeof(record) - offset);
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			status = B_ERROR;
		else
			offset += written;
	}
	if (status == B_OK && fsync(descriptor) != 0)
		status = B_ERROR;
	if (close(descriptor) != 0)
		status = B_ERROR;
	if (status == B_OK && rename(temporary, path) != 0)
		status = B_ERROR;
	if (status != B_OK)
		unlink(temporary);
	ClearSecret(record, sizeof(record));
	return status;
}


status_t
LoadLEBond(const char* directory, const uint8 localAddress[6],
	uint8 localType, const uint8 peerAddress[6], uint8 peerType,
	LELegacyBondKey& bond)
{
	ClearSecret(&bond, sizeof(bond));
	char path[PATH_MAX];
	status_t status = BuildPath(directory, localAddress, localType, peerAddress,
		peerType, path, sizeof(path));
	if (status != B_OK)
		return status;
	status = CheckDirectory(directory, false);
	if (status != B_OK)
		return status;
	int descriptor = open(path, O_RDONLY | O_NOFOLLOW);
	if (descriptor < 0)
		return errno == ENOENT ? B_ENTRY_NOT_FOUND : B_ERROR;
	uint8 record[kRecordSize] = {};
	struct stat info;
	status = B_OK;
	if (fstat(descriptor, &info) != 0)
		status = B_ERROR;
	else if (!S_ISREG(info.st_mode) || info.st_uid != geteuid()
		|| (info.st_mode & 0077) != 0)
		status = B_PERMISSION_DENIED;
	else if (info.st_size != kRecordSize)
		status = B_BAD_DATA;
	for (size_t offset = 0; status == B_OK && offset < sizeof(record);) {
		ssize_t count = read(descriptor, record + offset,
			sizeof(record) - offset);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			status = B_BAD_DATA;
		else
			offset += count;
	}
	close(descriptor);
	uint32 checksum = 0;
	for (int i = 0; i < 4; i++)
		checksum |= (uint32)record[kChecksum + i] << (i * 8);
	if (status == B_OK
		&& (memcmp(record + kMagic, kFileMagic, sizeof(kFileMagic)) != 0
			|| record[kLocalType] != localType
			|| memcmp(record + kLocalAddress, localAddress, 6) != 0
			|| record[kPeerType] != peerType
			|| memcmp(record + kPeerAddress, peerAddress, 6) != 0
			|| (record[kFlags] & ~3) != 0
			|| (record[kFlags] & 1) == 0
			|| record[kKeySize] < 7 || record[kKeySize] > 16
			|| checksum != Checksum(record, kChecksum)))
		status = B_BAD_DATA;
	if (status == B_OK) {
		bond.hasLongTermKey = true;
		bond.keySize = record[kKeySize];
		memcpy(bond.longTermKey, record + kLongTermKey, 16);
		for (uint8 i = bond.keySize; i < 16; i++)
			bond.longTermKey[i] = 0;
		memcpy(bond.randomNumber, record + kRandomNumber, 8);
		bond.encryptedDiversifier = record[kDiversifier]
			| ((uint16)record[kDiversifier + 1] << 8);
		bond.hasIdentity = (record[kFlags] & 2) != 0;
		if (bond.hasIdentity) {
			if (record[kIdentityType] > 1)
				status = B_BAD_DATA;
			else {
				bond.identityAddressType = record[kIdentityType];
				memcpy(bond.identityAddress, record + kIdentityAddress, 6);
				memcpy(bond.identityResolvingKey, record + kIdentityKey, 16);
			}
		}
	}
	if (status != B_OK)
		ClearSecret(&bond, sizeof(bond));
	ClearSecret(record, sizeof(record));
	return status;
}


status_t
RemoveLEBond(const char* directory, const uint8 localAddress[6],
	uint8 localType, const uint8 peerAddress[6], uint8 peerType)
{
	char path[PATH_MAX];
	status_t status = BuildPath(directory, localAddress, localType, peerAddress,
		peerType, path, sizeof(path));
	if (status != B_OK)
		return status;
	status = CheckDirectory(directory, false);
	if (status != B_OK)
		return status;
	if (unlink(path) != 0)
		return errno == ENOENT ? B_ENTRY_NOT_FOUND : B_ERROR;
	if (localType == 0) {
		char mousePath[PATH_MAX];
		if (BuildMousePath(directory, localAddress, peerAddress, peerType,
				mousePath, sizeof(mousePath)) == B_OK)
			unlink(mousePath);
		if (LEHIDLayoutPath(directory, localAddress, peerAddress, peerType,
				mousePath, sizeof(mousePath)) == B_OK)
			unlink(mousePath);
	}
	return B_OK;
}


status_t
SaveLEHIDMouse(const char* directory, const uint8 localAddress[6],
	const uint8 peerAddress[6], uint8 peerAddressType)
{
	char path[PATH_MAX];
	status_t status = BuildMousePath(directory, localAddress, peerAddress,
		peerAddressType, path, sizeof(path));
	if (status != B_OK)
		return status;
	status = CheckDirectory(directory, false);
	if (status != B_OK)
		return status;
	LELegacyBondKey bond = {};
	status = LoadLEBond(directory, localAddress, 0, peerAddress,
		peerAddressType, bond);
	ClearSecret(&bond, sizeof(bond));
	if (status != B_OK)
		return status;
	uint8 record[kMouseRecordSize];
	memcpy(record, kMouseMagic, sizeof(kMouseMagic));
	memcpy(record + 8, localAddress, 6);
	record[14] = peerAddressType;
	memcpy(record + 15, peerAddress, 6);
	char temporary[PATH_MAX];
	int length = snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path);
	if (length <= 0 || (size_t)length >= sizeof(temporary))
		return B_NAME_TOO_LONG;
	int descriptor = mkstemp(temporary);
	if (descriptor < 0)
		return B_ERROR;
	status = fchmod(descriptor, 0600) == 0
		&& write(descriptor, record, sizeof(record)) == sizeof(record)
		&& fsync(descriptor) == 0 ? B_OK : B_ERROR;
	if (close(descriptor) != 0)
		status = B_ERROR;
	if (status == B_OK && rename(temporary, path) != 0)
		status = B_ERROR;
	if (status != B_OK)
		unlink(temporary);
	// A new pairing starts from a fresh discovery.
	char layoutPath[PATH_MAX];
	if (status == B_OK && LEHIDLayoutPath(directory, localAddress, peerAddress,
			peerAddressType, layoutPath, sizeof(layoutPath)) == B_OK)
		unlink(layoutPath);
	return status;
}


status_t
ListLEHIDMice(const char* directory, std::vector<LEHIDMouseDevice>& devices)
{
	devices.clear();
	status_t status = CheckDirectory(directory, false);
	if (status != B_OK)
		return status;
	DIR* entries = opendir(directory);
	if (entries == NULL)
		return B_ERROR;
	struct dirent* entry;
	while ((entry = readdir(entries)) != NULL) {
		const char* suffix = strrchr(entry->d_name, '.');
		if (suffix == NULL || strcmp(suffix, ".mouse") != 0)
			continue;
		if (devices.size() >= 32) {
			status = B_BAD_DATA;
			break;
		}
		char path[PATH_MAX];
		int length = snprintf(path, sizeof(path), "%s/%s", directory,
			entry->d_name);
		if (length <= 0 || (size_t)length >= sizeof(path)) {
			status = B_NAME_TOO_LONG;
			break;
		}
		int descriptor = open(path, O_RDONLY | O_NOFOLLOW);
		if (descriptor < 0)
			continue;
		struct stat info;
		uint8 record[kMouseRecordSize];
		bool valid = fstat(descriptor, &info) == 0 && S_ISREG(info.st_mode)
			&& info.st_uid == geteuid() && (info.st_mode & 0077) == 0
			&& info.st_size == (off_t)sizeof(record)
			&& read(descriptor, record, sizeof(record)) == sizeof(record)
			&& memcmp(record, kMouseMagic, sizeof(kMouseMagic)) == 0
			&& record[14] <= 1;
		close(descriptor);
		if (!valid)
			continue;
		LEHIDMouseDevice device = {};
		memcpy(device.localAddress, record + 8, 6);
		device.peerAddressType = record[14];
		memcpy(device.peerAddress, record + 15, 6);
		char expected[PATH_MAX];
		if (BuildMousePath(directory, device.localAddress, device.peerAddress,
				device.peerAddressType, expected, sizeof(expected)) != B_OK
			|| strcmp(expected, path) != 0)
			continue;
		LELegacyBondKey bond = {};
		valid = LoadLEBond(directory, device.localAddress, 0,
			device.peerAddress, device.peerAddressType, bond) == B_OK;
		ClearSecret(&bond, sizeof(bond));
		if (valid)
			devices.push_back(device);
	}
	closedir(entries);
	return status;
}

static bool
DecodeAddress(const char* text, uint8 address[6])
{
	if (strlen(text) != 12)
		return false;
	for (int i = 0; i < 6; i++) {
		unsigned int byte;
		if (sscanf(text + i * 2, "%2x", &byte) != 1)
			return false;
		address[5 - i] = byte;
	}
	return true;
}


status_t
ListLEBonds(const char* directory, std::vector<LEBondedDevice>& devices)
{
	devices.clear();
	if (directory == NULL)
		return B_BAD_VALUE;
	DIR* dir = opendir(directory);
	if (dir == NULL)
		return errno == ENOENT ? B_OK : errno;
	std::vector<LEHIDMouseDevice> mice;
	ListLEHIDMice(directory, mice);
	struct dirent* entry;
	while ((entry = readdir(dir)) != NULL) {
		unsigned int localType, peerType;
		char local[13] = {}, peer[13] = {}, suffix[8] = {};
		if (sscanf(entry->d_name, "%1u-%12[0-9a-f]-%1u-%12[0-9a-f].%7s",
				&localType, local, &peerType, peer, suffix) != 5
			|| strcmp(suffix, "bond") != 0 || localType > 1 || peerType > 1)
			continue;
		LEBondedDevice device;
		if (!DecodeAddress(local, device.localAddress)
			|| !DecodeAddress(peer, device.peerAddress))
			continue;
		device.localAddressType = localType;
		device.peerAddressType = peerType;
		device.mouse = false;
		for (size_t i = 0; i < mice.size(); i++) {
			if (memcmp(mice[i].peerAddress, device.peerAddress, 6) == 0
				&& memcmp(mice[i].localAddress, device.localAddress, 6) == 0)
				device.mouse = true;
		}
		devices.push_back(device);
	}
	closedir(dir);
	return B_OK;
}

} // namespace Bluetooth
