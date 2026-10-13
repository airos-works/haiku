/*
 * Copyright 2007 Oliver Ruiz Dorantes, oliver.ruiz.dorantes_at_gmail.com
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#ifndef _BTL2CAP_H_
#define _BTL2CAP_H_

#include <bluetooth/bluetooth.h>

/* Use these values in l2cap_psm when connecting to an LE fixed channel. */
#define B_L2CAP_LE_ATT_CID 0x0004
#define B_L2CAP_LE_SMP_CID 0x0006

struct sockaddr_l2cap {
	uint8		l2cap_len;		/* total length */
	uint8		l2cap_family;	/* address family */
	uint16		l2cap_psm;		/* PSM, or ATT/SMP fixed CID on an LE link */
	bdaddr_t	l2cap_bdaddr;	/* address */
};

/* getsockopt() options at level BLUETOOTH_PROTO_L2CAP, each a uint16, valid
   once the channel is connected. */
#define B_L2CAP_OUTGOING_MTU	1	/* largest packet the peer accepts */
#define B_L2CAP_INCOMING_MTU	2	/* largest packet we accept */


#endif // _BTL2CAP_H_
