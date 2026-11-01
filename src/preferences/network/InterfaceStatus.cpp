/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "InterfaceStatus.h"

#include <net/if.h>

#include <Catalog.h>
#include <InterfaceDefs.h>
#include <NetworkAddress.h>
#include <NetworkInterface.h>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "InterfaceStatus"


static bool
HasUsableAddress(BNetworkInterface& interface)
{
	int32 count = interface.CountAddresses();
	for (int32 i = 0; i < count; i++) {
		BNetworkInterfaceAddress address;
		if (interface.GetAddressAt(i, address) != B_OK)
			continue;

		const BNetworkAddress& local = address.Address();
		if (local.IsEmpty() || local.IsWildcard())
			continue;
		if (local.Family() == AF_INET6 && local.IsLinkLocal())
			continue;
		return true;
	}
	return false;
}


interface_status
GetInterfaceStatus(BNetworkInterface& interface)
{
	uint32 flags = interface.Flags();

	if ((flags & IFF_UP) == 0 && (flags & IFF_CONFIGURING) == 0)
		return INTERFACE_STATUS_DISABLED;
	if ((flags & IFF_LINK) == 0)
		return INTERFACE_STATUS_NO_LINK;
	if ((flags & IFF_CONFIGURING) != 0)
		return INTERFACE_STATUS_CONFIGURING;
	if (!HasUsableAddress(interface))
		return INTERFACE_STATUS_NO_ADDRESS;
	return INTERFACE_STATUS_CONNECTED;
}


const char*
InterfaceStatusText(interface_status status)
{
	switch (status) {
		case INTERFACE_STATUS_DISABLED:
			return B_TRANSLATE("Disabled");
		case INTERFACE_STATUS_NO_LINK:
			return B_TRANSLATE("No link");
		case INTERFACE_STATUS_CONFIGURING:
			return B_TRANSLATE("Configuring" B_UTF8_ELLIPSIS);
		case INTERFACE_STATUS_NO_ADDRESS:
			return B_TRANSLATE("No address");
		case INTERFACE_STATUS_CONNECTED:
			return B_TRANSLATE("Connected");
	}
	return "";
}


rgb_color
InterfaceStatusColor(interface_status status)
{
	switch (status) {
		case INTERFACE_STATUS_NO_LINK:
			return make_color(255, 69, 58);
		case INTERFACE_STATUS_CONFIGURING:
		case INTERFACE_STATUS_NO_ADDRESS:
			return make_color(255, 204, 0);
		case INTERFACE_STATUS_CONNECTED:
			return make_color(52, 199, 89);
		case INTERFACE_STATUS_DISABLED:
		default:
			return make_color(150, 150, 150);
	}
}
