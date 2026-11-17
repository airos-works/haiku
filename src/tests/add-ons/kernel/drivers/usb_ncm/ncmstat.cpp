/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Prints what usb_ncm negotiated and counted: ncmstat [/dev/net/usb_ncm/0].
	Opens the device read only, which leaves it to the network stack.
*/


#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "usb_ncm.h"


int
main(int argc, char** argv)
{
	const char* path = argc > 1 ? argv[1] : "/dev/net/usb_ncm/0";
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return 1;
	}

	usb_ncm_info info;
	memset(&info, 0, sizeof(info));
	if (ioctl(fd, USB_NCM_GET_INFO, &info, sizeof(info)) != 0) {
		fprintf(stderr, "%s: USB_NCM_GET_INFO: %s\n", path, strerror(errno));
		close(fd);
		return 1;
	}
	close(fd);

	printf("%s: link %s, %" B_PRIu64 "/%" B_PRIu64 " Mbit/s\n", path,
		info.link_up ? "up" : "down", info.downstream_speed / 1000000,
		info.upstream_speed / 1000000);
	printf("ntb: formats 0x%" B_PRIx32 ", %s; in %" B_PRIu32 " (device max %"
		B_PRIu32 "), out %" B_PRIu32 " (device max %" B_PRIu32 ")\n",
		info.ntb_formats, info.ntb32 ? "NTB32" : "NTB16", info.rx_ntb_size,
		info.device_rx_max, info.tx_ntb_size, info.device_tx_max);
	printf("out: divisor %" B_PRIu32 ", remainder %" B_PRIu32 ", ndp alignment %"
		B_PRIu32 ", max %" B_PRIu32 " datagrams; frame %" B_PRIu32 "\n",
		info.tx_divisor, info.tx_remainder, info.tx_ndp_alignment,
		info.tx_max_datagrams, info.max_frame);
	printf("transfers: %" B_PRIu32 " in, %" B_PRIu32 " out, eager below %"
		B_PRIu32 "\n", info.rx_transfers, info.tx_transfers, info.tx_eager);
	printf("rx: %" B_PRIu64 " ntbs, %" B_PRIu64 " datagrams (%.1f per ntb, max %"
		B_PRIu64 "), %" B_PRIu64 " bytes; bad: %" B_PRIu64 " ntbs, %" B_PRIu64
		" datagrams, %" B_PRIu64 " truncated; %" B_PRIu64 " errors, %" B_PRIu64
		" without buffer\n", info.rx_ntbs, info.rx_datagrams,
		info.rx_ntbs != 0 ? (double)info.rx_datagrams / info.rx_ntbs : 0.0,
		info.rx_max_datagrams, info.rx_bytes, info.rx_bad_ntbs,
		info.rx_bad_datagrams, info.rx_truncated, info.rx_errors,
		info.rx_no_buffers);
	printf("tx: %" B_PRIu64 " ntbs, %" B_PRIu64 " datagrams (%.1f per ntb, max %"
		B_PRIu64 "), %" B_PRIu64 " bytes; sent idle %" B_PRIu64 ", full %"
		B_PRIu64 ", on completion %" B_PRIu64 "; %" B_PRIu64 " waits, %"
		B_PRIu64 " errors, %" B_PRIu64 " dropped\n", info.tx_ntbs,
		info.tx_datagrams,
		info.tx_ntbs != 0 ? (double)info.tx_datagrams / info.tx_ntbs : 0.0,
		info.tx_max_datagrams_seen, info.tx_bytes, info.tx_flush_idle,
		info.tx_flush_full, info.tx_flush_completion, info.tx_waits,
		info.tx_errors, info.tx_dropped);
	printf("notifications: %" B_PRIu64 "; now: %" B_PRIu32 " reads queued, %"
		B_PRIu32 " completed and not taken, %" B_PRIu32 " writes in flight, %"
		B_PRId32 " transfers pending\n", info.notifications, info.rx_queued,
		info.rx_completed, info.tx_in_flight, info.pending_transfers);
	return 0;
}
