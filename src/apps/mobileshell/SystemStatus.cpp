/*
 * Copyright 2026, air/OS.
 * Copyright 2026 Renku Mobile contributors.
 * Distributed under the terms of the MIT License.
 */


#include "SystemStatus.h"

#include <DateFormat.h>
#include <Directory.h>
#include <Entry.h>
#include <NetworkDevice.h>
#include <NetworkInterface.h>
#include <NetworkRoster.h>
#include <Path.h>
#include <TimeFormat.h>

#include <device/power_managment.h>

#include <fcntl.h>
#include <net/if.h>
#include <time.h>
#include <unistd.h>


static void
read_battery(SystemStatus& status)
{
	status.hasBattery = false;
	status.batteryLevel = 0;

	// the driver PowerStatus reads
	BDirectory directory("/dev/power/acpi_battery");
	BEntry entry;
	float remaining = 0, full = 0;
	while (directory.GetNextEntry(&entry) == B_OK) {
		BPath path(&entry);
		int fd = open(path.Path(), O_RDONLY);
		if (fd < 0)
			continue;

		acpi_battery_info info;
		acpi_extended_battery_info extended;
		if (ioctl(fd, GET_BATTERY_INFO, &info, sizeof(info)) == 0
			&& ioctl(fd, GET_EXTENDED_BATTERY_INFO, &extended,
				sizeof(extended)) == 0
			&& extended.last_full_charge > 0
			&& extended.last_full_charge != (uint32)-1) {
			status.hasBattery = true;
			remaining += info.capacity;
			full += extended.last_full_charge;
		}
		close(fd);
	}
	if (status.hasBattery && full > 0)
		status.batteryLevel = remaining / full;
}


static void
read_network(SystemStatus& status)
{
	status.network = kNetworkOffline;
	status.networkName = "";

	BNetworkRoster& roster = BNetworkRoster::Default();
	BNetworkInterface interface;
	uint32 cookie = 0;
	while (roster.GetNextInterface(&cookie, interface) == B_OK) {
		uint32 flags = interface.Flags();
		if ((flags & IFF_LOOPBACK) != 0 || (flags & IFF_UP) == 0
			|| (flags & IFF_LINK) == 0) {
			continue;
		}

		bool hasAddress = false;
		for (int32 i = 0; i < interface.CountAddresses(); i++) {
			BNetworkInterfaceAddress address;
			if (interface.GetAddressAt(i, address) == B_OK
				&& !address.Address().IsEmpty()) {
				hasAddress = true;
			}
		}
		if (!hasAddress)
			continue;

		BNetworkDevice device(interface.Name());
		if (device.IsWireless()) {
			wireless_network network;
			uint32 networkCookie = 0;
			status.network = kNetworkWireless;
			if (device.GetNextAssociatedNetwork(networkCookie, network) == B_OK)
				status.networkName = network.name;
			else
				status.networkName = "Wi-Fi";
			return;
		}
		status.network = kNetworkWired;
		status.networkName = "Wired";
	}
}


void
SystemStatus::Refresh()
{
	read_network(*this);
	read_battery(*this);
}


BString
clock_text()
{
	time_t now = time(NULL);
	BString text;
	if (BTimeFormat().Format(text, now, B_SHORT_TIME_FORMAT) != B_OK) {
		char buffer[16];
		struct tm local;
		localtime_r(&now, &local);
		strftime(buffer, sizeof(buffer), "%H:%M", &local);
		text = buffer;
	}
	return text;
}


BString
date_text()
{
	time_t now = time(NULL);
	BString text;
	if (BDateFormat().Format(text, now, B_FULL_DATE_FORMAT) != B_OK) {
		char buffer[64];
		struct tm local;
		localtime_r(&now, &local);
		strftime(buffer, sizeof(buffer), "%A, %e %B", &local);
		text = buffer;
	}
	return text;
}
