/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _QCOM_IRIS_H
#define _QCOM_IRIS_H


#include <Drivers.h>
#include <OS.h>


/*	The qcom_iris driver's interface to userland (the SC8280XP's video codec,
	Radxa Dragon Q8B). The driver powers the codec, starts its firmware and
	runs HFI's system init; userland speaks HFI (generation 1, the packets
	of Linux' iris_hfi_gen1_defines.h) to it from there on: it sends command
	packets and receives the firmware's messages, and allocates the buffers
	it hands the firmware. One open at a time owns the codec; its buffers go
	when it closes.

	Buffers are physically contiguous, below what the firmware addresses
	(0xe0000000), cached and coherent with the codec; their device address
	is what HFI packets name. */

#define QCOM_IRIS_DEVICE		"/dev/video/qcom_iris/0"
#define QCOM_IRIS_VERSION		1

enum {
	// struct qcom_iris_info
	QCOM_IRIS_GET_INFO = B_DEVICE_OP_CODES_END + 1,
	// struct qcom_iris_packet: one HFI command packet (its first word is
	// its size) into the command queue, and the firmware told
	QCOM_IRIS_SEND,
	// struct qcom_iris_receive: the next message, waiting up to timeout
	QCOM_IRIS_RECEIVE,
	// struct qcom_iris_buffer
	QCOM_IRIS_ALLOC,
	// uint32: a buffer's device address
	QCOM_IRIS_FREE,
};

struct qcom_iris_info {
	uint32	version;
	uint32	sys_init_properties;	// what SYS_INIT's answer listed
	char	firmware[128];			// HFI_PROPERTY_SYS_IMAGE_VERSION
};

struct qcom_iris_packet {
	const void*	data;
	uint32		size;
};

struct qcom_iris_receive {
	void*		buffer;
	uint32		size;			// of buffer; 4 KiB holds any message
	uint32		received;		// out
	bigtime_t	timeout;		// relative; 0: do not wait
};

struct qcom_iris_buffer {
	uint64		size;			// in, rounded up to pages
	area_id		area;			// out: the buffer in the caller's team
	void*		address;		// out
	uint32		device_address;	// out
	uint32		reserved;
};


#endif	/* _QCOM_IRIS_H */
