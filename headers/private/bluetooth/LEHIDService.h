/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef _LE_HID_SERVICE_H_
#define _LE_HID_SERVICE_H_

#include <LEAttributeClient.h>


namespace Bluetooth {

struct LEHIDInputReport {
	uint16 valueHandle;
	uint16 configurationHandle;
	uint8 reportID;
};


// Discovers the HID service and its report-mode input characteristics on an
// encrypted ATT connection, and the battery level if the device has one. The
// caller retains the ATT client and LE link.
//
// A bonded device keeps its attribute handles, so what Discover() finds can
// be saved and loaded on the next connection instead of being discovered
// again: discovery takes some fifty requests, seconds on a mouse that only
// listens every few connection events.
class LEHIDService {
public:
								LEHIDService();

	status_t Discover(LEAttributeClient& client);
	status_t EnableInputReports(LEAttributeClient& client) const;

	status_t Save(const char* path) const;
	status_t Load(const char* path);
	// Checks that a loaded layout still fits the device: every saved
	// notification descriptor is one (one request each). B_BAD_DATA if not.
	status_t Verify(LEAttributeClient& client) const;

	const std::vector<uint8>& ReportMap() const { return fReportMap; }
	const std::vector<LEHIDInputReport>& InputReports() const
		{ return fInputReports; }
	const LEHIDInputReport* FindInputReport(uint16 valueHandle) const;

	// Zero when there is no Battery Service (or it does not notify).
	uint16 BatteryHandle() const { return fBatteryHandle; }
	uint16 BatteryConfiguration() const { return fBatteryConfiguration; }

private:
	void _DiscoverBattery(LEAttributeClient& client,
		const LEPrimaryService& service);

	std::vector<uint8> fReportMap;
	std::vector<LEHIDInputReport> fInputReports;
	uint16 fBatteryHandle;
	uint16 fBatteryConfiguration;
};

} // namespace Bluetooth

#endif
