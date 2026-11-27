/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef BT_FAKE_HCI_H
#define BT_FAKE_HCI_H


#include <Drivers.h>


#define BT_FAKE_HCI_DEVICE	"misc/bt_fake_hci"

enum {
	BT_FAKE_REGISTER = B_DEVICE_OP_CODES_END + 0x4200,
		// out: int32, the HCI device id
	BT_FAKE_UNREGISTER,
		// in: int32, the id
	BT_FAKE_POST
		// in/out: bt_fake_post
};

struct bt_fake_post {
	int32		id;
	int32		type;
		// bt_packet_t
	uint32		size;
	uint8		data[1024];

	// out
	status_t	status;
	bigtime_t	duration;
		// how long PostTransportPacket() took
};


#endif	// BT_FAKE_HCI_H
