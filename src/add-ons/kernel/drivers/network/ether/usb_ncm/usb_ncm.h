/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _USB_NCM_H_
#define _USB_NCM_H_


/*!	What usb_ncm tells about itself: open /dev/net/usb_ncm/<n> read only (that
	open does not take the device from the network stack) and ask with
	USB_NCM_GET_INFO.
*/


#include <SupportDefs.h>


#define USB_NCM_GET_INFO	0x6e636d01


typedef struct usb_ncm_info {
	uint32	version;		// USB_NCM_INFO_VERSION
	uint32	size;			// of this structure

	// what was negotiated with the function
	uint32	ntb_formats;	// bmNtbFormatsSupported
	uint32	ntb32;			// blocks of the 32 bit format are sent
	uint32	rx_ntb_size;	// the NTB input size set
	uint32	tx_ntb_size;	// the largest NTB sent
	uint32	device_rx_max;	// dwNtbInMaxSize
	uint32	device_tx_max;	// dwNtbOutMaxSize
	uint32	tx_divisor;
	uint32	tx_remainder;
	uint32	tx_ndp_alignment;
	uint32	tx_max_datagrams;
	uint32	rx_transfers;	// bulk IN transfers kept queued
	uint32	tx_transfers;	// most bulk OUT transfers in flight
	uint32	tx_eager;		// below that many in flight an NTB goes at once
	uint32	max_frame;
	uint32	link_up;
	uint32	reserved;
	uint64	downstream_speed;
	uint64	upstream_speed;

	// received
	uint64	rx_ntbs;
	uint64	rx_datagrams;
	uint64	rx_bytes;
	uint64	rx_max_datagrams;	// most datagrams in one NTB
	uint64	rx_bad_ntbs;		// not parsed at all, or broken part way
	uint64	rx_bad_datagrams;	// entries skipped
	uint64	rx_truncated;		// datagrams beyond what one NTB may hold
	uint64	rx_errors;			// failed transfers
	uint64	rx_no_buffers;		// datagrams without a net_buffer

	// sent
	uint64	tx_ntbs;
	uint64	tx_datagrams;
	uint64	tx_bytes;
	uint64	tx_max_datagrams_seen;	// most datagrams in one NTB
	uint64	tx_flush_idle;		// sent at once, few transfers in flight
	uint64	tx_flush_full;		// sent when full
	uint64	tx_flush_completion;	// sent when a transfer completed
	uint64	tx_waits;			// senders that waited for a free buffer
	uint64	tx_errors;			// failed transfers
	uint64	tx_dropped;			// frames not sent

	uint64	notifications;

	// right now
	uint32	rx_queued;			// bulk IN transfers with the device
	uint32	rx_completed;		// completed ones the reader has not taken yet
	uint32	tx_in_flight;
	int32	pending_transfers;	// transfers whose hook has not run yet
} usb_ncm_info;

#define USB_NCM_INFO_VERSION	1


#endif	// _USB_NCM_H_
