/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
/*	hubport: hub-class requests to a USB hub's downstream ports, for lab
	checks of a hub behind a new host controller.
		hubport <device> status [<port>]	port status (all ports)
		hubport <device> off|on <port>		port power
	The device is the hub's /dev/bus/usb/... path. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <OS.h>
#include <USBKit.h>

#define USB_REQTYPE_CLASS_OTHER_IN	0xa3
#define USB_REQTYPE_CLASS_OTHER_OUT	0x23
#define USB_REQUEST_GET_STATUS		0
#define USB_REQUEST_CLEAR_FEATURE	1
#define USB_REQUEST_SET_FEATURE		3
#define USB_HUB_PORT_POWER			8
#define USB_HUB_PORT_CONNECTION		(1 << 0)
#define USB_HUB_PORT_ENABLE			(1 << 1)
#define USB_HUB_PORT_POWER_STATUS	(1 << 8)
#define USB_HUB_PORT_HIGH_SPEED		(1 << 10)

static int
port_status(BUSBDevice& device, int port)
{
	uint16 status[2] = {0, 0};
	ssize_t result = device.ControlTransfer(USB_REQTYPE_CLASS_OTHER_IN,
		USB_REQUEST_GET_STATUS, 0, port, 4, status);
	if (result < 4) {
		printf("port %d: GetPortStatus failed (%zd)\n", port, result);
		return 1;
	}
	printf("port %d: status %#06x change %#06x:%s%s%s%s\n", port, status[0],
		status[1], (status[0] & USB_HUB_PORT_POWER_STATUS) ? " power" : "",
		(status[0] & USB_HUB_PORT_CONNECTION) ? " connect" : "",
		(status[0] & USB_HUB_PORT_ENABLE) ? " enable" : "",
		(status[0] & USB_HUB_PORT_HIGH_SPEED) ? " high-speed" : "");
	return 0;
}

int
main(int argc, char** argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: hubport <device> status [<port>] | off|on <port>\n");
		return 1;
	}
	BUSBDevice device(argv[1]);
	if (device.InitCheck() != B_OK) {
		fprintf(stderr, "%s: cannot open\n", argv[1]);
		return 1;
	}
	int port = argc > 3 ? atoi(argv[3]) : 0;
	if (strcmp(argv[2], "status") == 0) {
		if (port > 0)
			return port_status(device, port);
		uint8 descriptor[16] = {};
		ssize_t result = device.ControlTransfer(0xa0, 6, 0x2900, 0,
			sizeof(descriptor), descriptor);
		int ports = result >= 3 ? descriptor[2] : 4;
		printf("%s: %d ports (hub descriptor %zd bytes)\n", argv[1], ports,
			result);
		for (int i = 1; i <= ports; i++)
			port_status(device, i);
		return 0;
	}
	if (port <= 0) {
		fprintf(stderr, "which port?\n");
		return 1;
	}
	bool on = strcmp(argv[2], "on") == 0;
	uint8 dummy[4] = {};
	ssize_t result = device.ControlTransfer(USB_REQTYPE_CLASS_OTHER_OUT,
		on ? USB_REQUEST_SET_FEATURE : USB_REQUEST_CLEAR_FEATURE,
		USB_HUB_PORT_POWER, port, 0, dummy);
	printf("port %d power %s: %s\n", port, on ? "on" : "off",
		result < 0 ? strerror(result) : "ok");
	snooze(300000);
	return port_status(device, port);
}
