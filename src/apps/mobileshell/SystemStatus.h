/*
 * Copyright 2026, air/OS.
 * Copyright 2026 Renku Mobile contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef SYSTEM_STATUS_H
#define SYSTEM_STATUS_H

/*	SystemStatus: what the status bar and the quick settings show about the
	device.
*/

#include <String.h>


enum NetworkState {
	kNetworkOffline,
	kNetworkWired,
	kNetworkWireless
};


struct SystemStatus {
	NetworkState	network;
	BString			networkName;	// interface, or the Wi-Fi network
	bool			hasBattery;
	float			batteryLevel;	// 0..1, when hasBattery

	void			Refresh();
};


// the time and the day as the user's locale writes them
BString clock_text();
BString date_text();


#endif	// SYSTEM_STATUS_H
