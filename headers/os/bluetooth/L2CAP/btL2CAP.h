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
   once the channel is connected. On an LE ATT socket both are the link's
   ATT_MTU. */
#define B_L2CAP_OUTGOING_MTU	1	/* largest packet the peer accepts */
#define B_L2CAP_INCOMING_MTU	2	/* largest packet we accept */

/* LE ATT sockets only, a uint16. setsockopt() before connect() sets the
   largest ATT PDU this side takes (B_L2CAP_LE_ATT_MINIMUM_MTU up to
   B_L2CAP_LE_ATT_MAXIMUM_MTU; the minimum when not set). With more than the
   minimum, connect() sends the peer an Exchange MTU Request and returns once
   it is answered (or, non-blocking, completes like any connect()). The peer's
   own Exchange MTU Request is answered with the same value. getsockopt()
   gives the link's ATT_MTU now: the smaller of both sides' values after an
   exchange, the minimum before one. */
#define B_L2CAP_LE_ATT_MTU		3
#define B_L2CAP_LE_ATT_MINIMUM_MTU	23
#define B_L2CAP_LE_ATT_MAXIMUM_MTU	517


#endif // _BTL2CAP_H_
