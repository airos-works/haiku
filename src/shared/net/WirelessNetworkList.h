/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef WIRELESS_NETWORK_LIST_H
#define WIRELESS_NETWORK_LIST_H


//!	Shared presentation rules for wireless scan results.
//
//	Every surface that lists wireless networks (the WiFi preferences, the
//	WiFiStatus Deskbar applet, the Network preferences, NetworkStatus and the
//	wifiautojoin helper) includes this header, so that they group, rank and
//	rate networks identically.
//
//	A scan returns one entry per access point (BSSID). A mesh or an extended
//	service set therefore shows up as many entries sharing one name (SSID),
//	often on several channels and bands, and sometimes with slightly different
//	security details. Users think in terms of networks, not access points, so
//	the entries are grouped strictly by SSID: one row per name. Hidden access
//	points (empty SSID) are dropped; they can only be joined by name.


#include <FindDirectory.h>
#include <NetworkDevice.h>
#include <OS.h>
#include <Path.h>
#include <String.h>

#include <algorithm>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <vector>


static const int32 kWirelessSignalMaxBars = 4;


struct WirelessNetworkGroup {
	//! The strongest access point of the group. Its security fields are
	//! merged: when any access point is secured, the group is secured and
	//! uses the strongest secured access point's authentication and ciphers.
	wireless_network	network;
	int32				accessPoints;
	bool				secured;
	bool				connected;
};


// #pragma mark - debug logging


/*!	Returns whether verbose WiFi diagnostics are enabled. They are enabled by
	a non-empty WIFI_DEBUG environment variable other than "0", or while the
	file ~/config/settings/WiFi_debug exists. The file is rechecked every few
	seconds so it can be toggled for a running Deskbar applet.
*/
inline bool
WiFiDebugEnabled()
{
	static bigtime_t sLastCheck = -1;
	static bool sEnabled = false;

	bigtime_t now = system_time();
	if (sLastCheck >= 0 && now - sLastCheck < 3000000)
		return sEnabled;
	sLastCheck = now;

	const char* variable = getenv("WIFI_DEBUG");
	if (variable != NULL && variable[0] != '\0' && strcmp(variable, "0") != 0) {
		sEnabled = true;
		return true;
	}

	BPath path;
	struct stat info;
	sEnabled = find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK
		&& path.Append("WiFi_debug") == B_OK
		&& stat(path.Path(), &info) == 0;
	return sEnabled;
}


/*!	Logs a diagnostic line to the system log (/var/log/syslog) when WiFi
	debugging is enabled. Never pass credentials to this function.
*/
inline void
WiFiDebugLog(const char* component, const char* format, ...)
	__attribute__((format(printf, 2, 3)));

inline void
WiFiDebugLog(const char* component, const char* format, ...)
{
	if (!WiFiDebugEnabled())
		return;

	char buffer[512];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);

	syslog(LOG_INFO, "WiFi[%s]: %s", component, buffer);
}


// #pragma mark - signal strength


/*!	Estimates the received signal level in dBm.

	Relative-RSSI drivers derived from OpenBSD (iwx, iwm, ...) leave the noise
	level at zero and report "dBm + 100", clipped to their maximum. The OpenBSD
	compatibility layer converts absolute-dBm drivers to the FreeBSD format.
	FreeBSD-derived drivers report the noise floor in dBm (as a signed byte)
	and the signal in 0.5 dB steps above it.
*/
inline int32
WirelessSignalDBm(const wireless_network& network)
{
	int32 noise = (int8)network.noise_level;
	int32 dBm;
	if (noise < 0)
		dBm = noise + network.signal_strength / 2;
	else
		dBm = (int32)network.signal_strength - 100;

	return std::max((int32)-100, std::min((int32)0, dBm));
}


//!	Maps the signal to a 0-100 quality: -100 dBm is 0%, -50 dBm and up 100%.
inline int32
WirelessSignalPercent(const wireless_network& network)
{
	int32 percent = 2 * (WirelessSignalDBm(network) + 100);
	return std::max((int32)0, std::min((int32)100, percent));
}


//!	Number of bars (1 to kWirelessSignalMaxBars) for a visible network.
inline int32
WirelessSignalBars(const wireless_network& network)
{
	int32 dBm = WirelessSignalDBm(network);
	if (dBm >= -55)
		return 4;
	if (dBm >= -72)
		return 3;
	if (dBm >= -82)
		return 2;
	return 1;
}


// #pragma mark - security


inline bool
WirelessNetworkIsSecured(const wireless_network& network)
{
	return network.authentication_mode != B_NETWORK_AUTHENTICATION_NONE
		|| (network.flags & B_NETWORK_IS_ENCRYPTED) != 0;
}


inline const char*
WirelessAuthenticationKeyword(uint32 mode)
{
	switch (mode) {
		case B_NETWORK_AUTHENTICATION_NONE:
			return "none";
		case B_NETWORK_AUTHENTICATION_WEP:
			return "wep";
		case B_NETWORK_AUTHENTICATION_WPA:
			return "wpa";
		case B_NETWORK_AUTHENTICATION_WPA2:
			return "wpa2";
		case B_NETWORK_AUTHENTICATION_WPA3:
			return "wpa3";
		default:
			return NULL;
	}
}


inline const char*
WirelessAuthenticationLabel(uint32 mode, bool secured)
{
	switch (mode) {
		case B_NETWORK_AUTHENTICATION_WEP:
			return "WEP";
		case B_NETWORK_AUTHENTICATION_WPA:
			return "WPA";
		case B_NETWORK_AUTHENTICATION_WPA2:
			return "WPA2";
		case B_NETWORK_AUTHENTICATION_WPA3:
			return "WPA3";
		case B_NETWORK_AUTHENTICATION_EAP:
			return "EAP";
		default:
			return secured ? "encrypted" : "open";
	}
}


// #pragma mark - grouping


inline bool
WirelessNetworkNameEquals(const char* a, const char* b)
{
	return strncmp(a, b, sizeof(((wireless_network*)NULL)->name)) == 0;
}


/*!	Groups the access points of a scan by SSID.

	One group is returned per non-empty SSID, regardless of BSSID, channel,
	band or security differences between its access points. The group shows
	the strongest access point's signal. When \a connectedName is given, the
	group of that name is marked connected, whichever access point the adapter
	is associated with. The result is sorted by signal, strongest first.
*/
inline std::vector<WirelessNetworkGroup>
GroupWirelessNetworks(const wireless_network* networks, uint32 count,
	const char* connectedName = NULL)
{
	std::vector<WirelessNetworkGroup> groups;
	// Signal of the access point whose security details a group uses
	std::vector<int32> securitySignal;

	for (uint32 i = 0; networks != NULL && i < count; i++) {
		const wireless_network& candidate = networks[i];
		if (candidate.name[0] == '\0')
			continue;

		size_t index = 0;
		for (; index < groups.size(); index++) {
			if (WirelessNetworkNameEquals(groups[index].network.name,
					candidate.name))
				break;
		}

		if (index == groups.size()) {
			WirelessNetworkGroup group;
			group.network = candidate;
			group.accessPoints = 1;
			group.secured = WirelessNetworkIsSecured(candidate);
			group.connected = false;
			groups.push_back(group);
			securitySignal.push_back(candidate.authentication_mode
				!= B_NETWORK_AUTHENTICATION_NONE
					? candidate.signal_strength : -1);
			continue;
		}

		WirelessNetworkGroup& group = groups[index];
		group.accessPoints++;
		group.secured |= WirelessNetworkIsSecured(candidate);

		wireless_network& best = group.network;
		if (candidate.signal_strength > best.signal_strength) {
			// Keep the merged security of the group
			uint32 authentication = best.authentication_mode;
			uint32 cipher = best.cipher;
			uint32 groupCipher = best.group_cipher;
			uint32 keyMode = best.key_mode;
			uint32 flags = best.flags;
			best = candidate;
			best.authentication_mode = authentication;
			best.cipher = cipher;
			best.group_cipher = groupCipher;
			best.key_mode = keyMode;
			best.flags |= flags;
		} else
			best.flags |= candidate.flags & B_NETWORK_IS_ENCRYPTED;

		if (candidate.authentication_mode != B_NETWORK_AUTHENTICATION_NONE
			&& (int32)candidate.signal_strength > securitySignal[index]) {
			best.authentication_mode = candidate.authentication_mode;
			best.cipher = candidate.cipher;
			best.group_cipher = candidate.group_cipher;
			best.key_mode = candidate.key_mode;
			securitySignal[index] = candidate.signal_strength;
		}
	}

	for (size_t i = 0; i < groups.size(); i++) {
		WirelessNetworkGroup& group = groups[i];
		if (group.secured)
			group.network.flags |= B_NETWORK_IS_ENCRYPTED;
		group.connected = connectedName != NULL && connectedName[0] != '\0'
			&& WirelessNetworkNameEquals(group.network.name, connectedName);
	}

	std::sort(groups.begin(), groups.end(),
		[](const WirelessNetworkGroup& a, const WirelessNetworkGroup& b) {
			if (a.network.signal_strength != b.network.signal_strength)
				return a.network.signal_strength > b.network.signal_strength;
			return strncasecmp(a.network.name, b.network.name,
				sizeof(a.network.name)) < 0;
		});

	return groups;
}


/*!	Returns the SSID the device is associated with, or an empty string.
	Only the name matters: a mesh may associate with any of its access points.
	The access point is looked up in the given scan results first, as not all
	drivers can report station details.
*/
inline BString
AssociatedWirelessNetworkName(BNetworkDevice& device,
	const wireless_network* networks, uint32 count)
{
	BNetworkAddress address;
	uint32 cookie = 0;
	if (device.GetNextAssociatedNetwork(cookie, address) != B_OK)
		return BString();

	for (uint32 i = 0; networks != NULL && i < count; i++) {
		if (networks[i].address == address && networks[i].name[0] != '\0')
			return BString(networks[i].name, sizeof(networks[i].name));
	}

	wireless_network network;
	if (device.GetNetwork(address, network) != B_OK)
		return BString();
	return BString(network.name, sizeof(network.name));
}


//!	Logs the raw scan and the resulting groups when debugging is enabled.
inline void
LogWirelessScan(const char* component, const char* device,
	const wireless_network* networks, uint32 count,
	const std::vector<WirelessNetworkGroup>& groups)
{
	if (!WiFiDebugEnabled())
		return;

	WiFiDebugLog(component, "%s: scan cache has %" B_PRIu32 " access points",
		device, count);
	for (uint32 i = 0; networks != NULL && i < count; i++) {
		const wireless_network& network = networks[i];
		BString name(network.name, sizeof(network.name));
		WiFiDebugLog(component, "  ap %s \"%s\" raw %u noise %d -> %" B_PRId32
			" dBm, auth %s%s", network.address.ToString().String(),
			name.String(), network.signal_strength, (int8)network.noise_level,
			WirelessSignalDBm(network),
			WirelessAuthenticationLabel(network.authentication_mode,
				WirelessNetworkIsSecured(network)),
			name.IsEmpty() ? " (hidden, dropped)" : "");
	}
	for (size_t i = 0; i < groups.size(); i++) {
		const WirelessNetworkGroup& group = groups[i];
		BString name(group.network.name, sizeof(group.network.name));
		WiFiDebugLog(component, "  group \"%s\": %" B_PRId32 " access points,"
			" best %s %" B_PRId32 " dBm (%" B_PRId32 " bars), %s%s",
			name.String(), group.accessPoints,
			group.network.address.ToString().String(),
			WirelessSignalDBm(group.network), WirelessSignalBars(group.network),
			WirelessAuthenticationLabel(group.network.authentication_mode,
				group.secured),
			group.connected ? ", connected" : "");
	}
}


#endif	// WIRELESS_NETWORK_LIST_H
