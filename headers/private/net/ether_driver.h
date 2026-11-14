/*
 * Copyright 2007, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _ETHER_DRIVER_H
#define _ETHER_DRIVER_H

/*! Standard ethernet driver interface */


#include <Drivers.h>


/* ioctl() opcodes a driver should support */
enum {
	ETHER_GETADDR = B_DEVICE_OP_CODES_END,
		/* get ethernet address (required) */
	ETHER_INIT,								/* (obsolete) */
	ETHER_NONBLOCK,							/* change non blocking mode (int *) */
	ETHER_ADDMULTI,							/* add multicast address */
	ETHER_REMMULTI,							/* remove multicast address */
	ETHER_SETPROMISC,						/* set promiscuous mode (int *) */
	ETHER_GETFRAMESIZE,						/* get frame size (required) (int *) */
	ETHER_SET_LINK_STATE_SEM,
		/* pass over a semaphore to release on link state changes (sem_id *) */
	ETHER_GET_LINK_STATE,
		/* get line speed, quality, duplex mode, etc. (ether_link_state_t *) */

	ETHER_SEND_NET_BUFFER,					/* send a net_buffer */
	ETHER_RECEIVE_NET_BUFFER,				/* receive a net_buffer */
	ETHER_GET_OFFLOAD,
		/* what the device computes for the stack (uint32 *, ETHER_OFFLOAD_*) */
	ETHER_RECEIVE_NET_BUFFERS,
		/* receive several net_buffers at once (ether_receive_batch *) */
};

/* ETHER_RECEIVE_NET_BUFFERS: waits for one frame like ETHER_RECEIVE_NET_BUFFER,
   then also returns the frames that are already there, up to count */
typedef struct ether_receive_batch {
	struct net_buffer**	buffers;
	uint32				count;	/* in: room in buffers, out: returned */
} ether_receive_batch;

/* ETHER_GET_OFFLOAD */
#define ETHER_OFFLOAD_TX_CHECKSUM_IPV4	0x01
	/* TCP and UDP checksums over IPv4 when sending (net_buffers flagged
	   NET_BUFFER_L4_CHECKSUM_OFFLOAD, with the pseudo header sum in the
	   checksum field) */
#define ETHER_OFFLOAD_TCP_SEGMENTATION_IPV4	0x02
	/* TCP segments over IPv4 of up to 64000 bytes (net_buffers flagged
	   NET_BUFFER_SEGMENTATION_OFFLOAD, see there) */


/* ETHER_GETADDR - MAC address */
typedef struct ether_address {
	uint8	ebyte[6];
} ether_address_t;

/* ETHER_GETLINKSTATE */
typedef struct ether_link_state {
	uint32	media;		/* as specified in net/if_media.h */
	uint32  quality;	/* in one tenth of a percent */
	uint64	speed;		/* in bit/s */
} ether_link_state_t;

#endif	/* _ETHER_DRIVER_H */
