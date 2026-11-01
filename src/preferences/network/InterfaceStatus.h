/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef INTERFACE_STATUS_H
#define INTERFACE_STATUS_H


#include <GraphicsDefs.h>


class BNetworkInterface;


/*!	The state of an interface as the Network preferences show it, in the
	list and in the interface view alike.
*/
enum interface_status {
	INTERFACE_STATUS_DISABLED,
	INTERFACE_STATUS_NO_LINK,
	INTERFACE_STATUS_CONFIGURING,
	INTERFACE_STATUS_NO_ADDRESS,
		// Link, but configuration ended without a usable address
	INTERFACE_STATUS_CONNECTED
};


interface_status	GetInterfaceStatus(BNetworkInterface& interface);
const char*			InterfaceStatusText(interface_status status);
rgb_color			InterfaceStatusColor(interface_status status);


#endif	// INTERFACE_STATUS_H
