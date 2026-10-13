/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */

#include <LEHIDService.h>

#include <Errors.h>
#include <LELog.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


namespace Bluetooth {

static const uint16 kHIDService = 0x1812;
static const uint16 kBatteryService = 0x180f;
static const uint16 kBatteryLevelCharacteristic = 0x2a19;
static const uint16 kReportMapCharacteristic = 0x2a4b;
static const uint16 kReportCharacteristic = 0x2a4d;
static const uint16 kReportReferenceDescriptor = 0x2908;
static const uint16 kClientConfigurationDescriptor = 0x2902;
static const uint8 kInputReport = 1;
static const uint8 kNotifyProperty = 0x10;
static const size_t kMaximumReports = 32;
static const size_t kMaximumReportMap = 4096;

// Saved layout: magic, report map length (uint16) and bytes, report count
// (uint8) and per report value handle, configuration handle (uint16 each)
// and ID, then the battery handle and its configuration handle.
static const uint8 kLayoutMagic[8] = { 'H', 'L', 'H', 'I', 'D', 'L', 'Y', '1' };

#define LOG(level, format, args...) LELog(level, "hid", format, ##args)


LEHIDService::LEHIDService()
	:
	fBatteryHandle(0),
	fBatteryConfiguration(0)
{
}


status_t
LEHIDService::Discover(LEAttributeClient& client)
{
	fReportMap.clear();
	fInputReports.clear();
	fBatteryHandle = fBatteryConfiguration = 0;
	std::vector<LEPrimaryService> services;
	status_t status = client.DiscoverPrimaryServices(services);
	if (status != B_OK) {
		LOG(LE_LOG_ERROR, "service discovery failed: %s", strerror(status));
		return status;
	}
	for (const LEPrimaryService& service : services) {
		LOG(LE_LOG_DEBUG, "service %#06x handles %#x-%#x", service.uuid16,
			service.startHandle, service.endHandle);
	}
	for (const LEPrimaryService& service : services) {
		if (service.uuid16 == kBatteryService && fBatteryHandle == 0)
			_DiscoverBattery(client, service);
	}
	for (const LEPrimaryService& service : services) {
		if (service.uuid16 != kHIDService)
			continue;
		std::vector<LECharacteristic> characteristics;
		status = client.DiscoverCharacteristics(service.startHandle,
			service.endHandle, characteristics);
		if (status != B_OK)
			return status;
		for (size_t i = 0; i < characteristics.size(); i++) {
			const LECharacteristic& characteristic = characteristics[i];
			if (characteristic.uuid16 == kReportMapCharacteristic) {
				if (!fReportMap.empty())
					return B_BAD_DATA;
				status = client.ReadAttribute(characteristic.valueHandle,
					fReportMap);
				if (status != B_OK) {
					LOG(LE_LOG_ERROR, "reading the report map failed: %s "
						"(ATT error %#x)", strerror(status),
						client.LastATTError());
					return status;
				}
				LELogHex(LE_LOG_DEBUG, "hid", "report map",
					fReportMap.data(), fReportMap.size());
				if (fReportMap.empty())
					return B_BAD_DATA;
			} else if (characteristic.uuid16 == kReportCharacteristic
				&& (characteristic.properties & kNotifyProperty) != 0) {
				if (fInputReports.size() >= kMaximumReports)
					return B_BAD_DATA;
				uint16 end = i + 1 < characteristics.size()
					? characteristics[i + 1].declarationHandle - 1
					: service.endHandle;
				if (characteristic.valueHandle >= end)
					return B_BAD_DATA;
				// Skip a malformed report rather than lose the whole device.
				uint16 reference = 0;
				status = client.FindDescriptor(characteristic.valueHandle + 1,
					end, kReportReferenceDescriptor, reference);
				if (status != B_OK) {
					LOG(LE_LOG_INFO, "report %#x has no reference descriptor "
						"(%s); skipped", characteristic.valueHandle,
						strerror(status));
					continue;
				}
				std::vector<uint8> value;
				status = client.ReadAttribute(reference, value);
				if (status != B_OK) {
					LOG(LE_LOG_ERROR, "reading report reference %#x failed: "
						"%s (ATT error %#x)", reference, strerror(status),
						client.LastATTError());
					if (status == B_DEV_NOT_READY)
						return status;
					continue;
				}
				if (value.size() != 2 || value[1] < 1 || value[1] > 3) {
					LOG(LE_LOG_INFO, "report %#x has a malformed reference; "
						"skipped", characteristic.valueHandle);
					continue;
				}
				LOG(LE_LOG_DEBUG, "report %#x: id %u type %u",
					characteristic.valueHandle, value[0], value[1]);
				if (value[1] != kInputReport)
					continue;
				uint16 configuration = 0;
				status = client.FindDescriptor(characteristic.valueHandle + 1,
					end, kClientConfigurationDescriptor, configuration);
				if (status != B_OK) {
					LOG(LE_LOG_INFO, "input report %u has no notification "
						"descriptor; skipped", value[0]);
					continue;
				}
				bool duplicate = false;
				for (const LEHIDInputReport& report : fInputReports) {
					if (report.reportID == value[0])
						duplicate = true;
				}
				if (duplicate) {
					LOG(LE_LOG_INFO, "duplicate input report id %u; skipped",
						value[0]);
					continue;
				}
				fInputReports.push_back({characteristic.valueHandle,
					configuration, value[0]});
			}
		}
		break;
	}
	return fReportMap.empty() || fInputReports.empty()
		? B_ENTRY_NOT_FOUND : B_OK;
}


status_t
LEHIDService::EnableInputReports(LEAttributeClient& client) const
{
	if (fInputReports.empty())
		return B_NO_INIT;
	const uint8 enabled[2] = { 1, 0 };
	for (const LEHIDInputReport& report : fInputReports) {
		status_t status = client.WriteAttribute(report.configurationHandle,
			enabled, sizeof(enabled));
		if (status != B_OK)
			return status;
	}
	return B_OK;
}


void
LEHIDService::_DiscoverBattery(LEAttributeClient& client,
	const LEPrimaryService& service)
{
	// Optional: a failure here costs the battery display, not the device.
	std::vector<LECharacteristic> characteristics;
	if (client.DiscoverCharacteristics(service.startHandle, service.endHandle,
			characteristics) != B_OK)
		return;
	for (size_t i = 0; i < characteristics.size(); i++) {
		const LECharacteristic& level = characteristics[i];
		if (level.uuid16 != kBatteryLevelCharacteristic)
			continue;
		fBatteryHandle = level.valueHandle;
		uint16 end = i + 1 < characteristics.size()
			? characteristics[i + 1].declarationHandle - 1
			: service.endHandle;
		uint16 configuration;
		if ((level.properties & kNotifyProperty) != 0 && end > level.valueHandle
			&& client.FindClientConfiguration(level.valueHandle + 1, end,
				configuration) == B_OK)
			fBatteryConfiguration = configuration;
		return;
	}
}


status_t
LEHIDService::Save(const char* path) const
{
	if (fReportMap.empty() || fReportMap.size() > kMaximumReportMap
		|| fInputReports.empty() || fInputReports.size() > kMaximumReports)
		return B_NO_INIT;
	std::vector<uint8> data(kLayoutMagic, kLayoutMagic + sizeof(kLayoutMagic));
	auto add16 = [&data](uint16 value) {
		data.push_back((uint8)value);
		data.push_back((uint8)(value >> 8));
	};
	add16(fReportMap.size());
	data.insert(data.end(), fReportMap.begin(), fReportMap.end());
	data.push_back((uint8)fInputReports.size());
	for (const LEHIDInputReport& report : fInputReports) {
		add16(report.valueHandle);
		add16(report.configurationHandle);
		data.push_back(report.reportID);
	}
	add16(fBatteryHandle);
	add16(fBatteryConfiguration);

	char temporary[PATH_MAX];
	int length = snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path);
	if (length <= 0 || (size_t)length >= sizeof(temporary))
		return B_NAME_TOO_LONG;
	int descriptor = mkstemp(temporary);
	if (descriptor < 0)
		return B_ERROR;
	status_t status = fchmod(descriptor, 0600) == 0
		&& write(descriptor, data.data(), data.size()) == (ssize_t)data.size()
		&& fsync(descriptor) == 0 ? B_OK : B_ERROR;
	if (close(descriptor) != 0)
		status = B_ERROR;
	if (status == B_OK && rename(temporary, path) != 0)
		status = B_ERROR;
	if (status != B_OK)
		unlink(temporary);
	return status;
}


status_t
LEHIDService::Load(const char* path)
{
	fReportMap.clear();
	fInputReports.clear();
	fBatteryHandle = fBatteryConfiguration = 0;

	int descriptor = open(path, O_RDONLY | O_NOFOLLOW);
	if (descriptor < 0)
		return errno == ENOENT ? B_ENTRY_NOT_FOUND : B_ERROR;
	struct stat info;
	const off_t largest = sizeof(kLayoutMagic) + 2 + kMaximumReportMap + 1
		+ kMaximumReports * 5 + 4;
	std::vector<uint8> data;
	if (fstat(descriptor, &info) == 0 && S_ISREG(info.st_mode)
		&& info.st_uid == geteuid() && (info.st_mode & 0077) == 0
		&& info.st_size <= largest) {
		data.resize(info.st_size);
		if (read(descriptor, data.data(), data.size()) != (ssize_t)data.size())
			data.clear();
	}
	close(descriptor);

	size_t offset = 0;
	auto take = [&data, &offset](size_t count) -> const uint8* {
		if (data.size() - offset < count)
			return NULL;
		offset += count;
		return data.data() + offset - count;
	};
	auto take16 = [&take](uint16& value) {
		const uint8* bytes = take(2);
		if (bytes == NULL)
			return false;
		value = bytes[0] | (bytes[1] << 8);
		return true;
	};
	const uint8* magic = take(sizeof(kLayoutMagic));
	uint16 mapLength;
	if (magic == NULL || memcmp(magic, kLayoutMagic, sizeof(kLayoutMagic)) != 0
		|| !take16(mapLength) || mapLength == 0
		|| mapLength > kMaximumReportMap)
		return B_BAD_DATA;
	const uint8* map = take(mapLength);
	const uint8* count = take(1);
	if (map == NULL || count == NULL || *count == 0 || *count > kMaximumReports)
		return B_BAD_DATA;
	std::vector<LEHIDInputReport> reports;
	for (uint8 i = 0; i < *count; i++) {
		LEHIDInputReport report;
		const uint8* id;
		if (!take16(report.valueHandle) || !take16(report.configurationHandle)
			|| (id = take(1)) == NULL || report.valueHandle == 0
			|| report.configurationHandle == 0)
			return B_BAD_DATA;
		report.reportID = *id;
		reports.push_back(report);
	}
	uint16 battery, batteryConfiguration;
	if (!take16(battery) || !take16(batteryConfiguration)
		|| offset != data.size())
		return B_BAD_DATA;

	fReportMap.assign(map, map + mapLength);
	fInputReports = reports;
	fBatteryHandle = battery;
	fBatteryConfiguration = batteryConfiguration;
	return B_OK;
}


status_t
LEHIDService::Verify(LEAttributeClient& client) const
{
	if (fInputReports.empty())
		return B_NO_INIT;
	for (const LEHIDInputReport& report : fInputReports) {
		uint16 found = 0;
		status_t status = client.FindDescriptor(report.configurationHandle,
			report.configurationHandle, kClientConfigurationDescriptor, found);
		if (status == B_ENTRY_NOT_FOUND
			|| (status == B_OK && found != report.configurationHandle)) {
			LOG(LE_LOG_INFO, "handle %#x is no longer report %u's "
				"notification descriptor", report.configurationHandle,
				report.reportID);
			return B_BAD_DATA;
		}
		if (status != B_OK)
			return client.LastATTError() != 0 ? B_BAD_DATA : status;
	}
	return B_OK;
}


const LEHIDInputReport*
LEHIDService::FindInputReport(uint16 valueHandle) const
{
	for (const LEHIDInputReport& report : fInputReports) {
		if (report.valueHandle == valueHandle)
			return &report;
	}
	return NULL;
}

} // namespace Bluetooth
