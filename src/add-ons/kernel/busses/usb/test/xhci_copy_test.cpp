/*
 * Copyright 2026, the air/OS project.
 * Distributed under the terms of the MIT License.
 */

// Host test of xhci's copy_descriptor_data(): what a finished IN transfer
// copies out of a descriptor's buffers into the transfer's vectors. Only
// what was received, and for an isochronous transfer each packet's actual
// length at its place. run.sh cuts the function out of xhci.cpp into
// copy_function.h (it is static there) and builds this around it.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
typedef int32_t status_t;
typedef uint64_t generic_addr_t;
typedef uint64_t generic_size_t;
#define B_OK 0
#define min_c(a, b) ((a) > (b) ? (b) : (a))
#define ASSERT_ALWAYS(x) assert(x)
struct generic_io_vec { generic_addr_t base; generic_size_t length; };
struct usb_iso_packet_descriptor {
	uint32_t request_length;
	uint32_t actual_length;
	status_t status;
};
struct usb_isochronous_data {
	usb_iso_packet_descriptor* packet_descriptors;
	uint32_t packet_count;
};
struct xhci_td { void** buffers; size_t buffer_count; size_t buffer_size; };
static size_t sBytesRead;
static status_t generic_memcpy(generic_addr_t to, bool, generic_addr_t from,
	bool, size_t length)
{
	memcpy((void*)to, (void*)from, length);
	sBytesRead += length;
	return B_OK;
}
#include "copy_function.h"
#ifndef OLD
#define READ(td, v, c, iso, received) \
	copy_descriptor_data(td, v, c, false, iso, false, received)
#else
// (before the copy knew what was received)
#define READ(td, v, c, iso, received) \
	copy_descriptor_data(td, v, c, false, iso, false)
#endif

static int sFailures;
#define CHECK(c, ...) \
	do { \
		if (!(c)) { \
			printf("FAIL: " __VA_ARGS__); \
			printf("\n"); \
			sFailures++; \
		} \
	} while (0)

// a descriptor of count buffers of size bytes, buffer i filled with i + 1
static xhci_td* make_td(size_t count, size_t size)
{
	xhci_td* td = new xhci_td;
	td->buffers = new void*[count];
	td->buffer_count = count; td->buffer_size = size;
	for (size_t i = 0; i < count; i++) {
		td->buffers[i] = malloc(size);
		memset(td->buffers[i], i + 1, size);
	}
	return td;
}

static void bulk(size_t count, size_t size, size_t asked, size_t received,
	size_t vectors)
{
	xhci_td* td = make_td(count, size);
	uint8_t* data = (uint8_t*)malloc(asked + 1);
	memset(data, 0xee, asked + 1);
	generic_io_vec vec[4];
	size_t per = asked / vectors, offset = 0;
	for (size_t i = 0; i < vectors; i++) {
		vec[i].base = (generic_addr_t)(data + offset);
		vec[i].length = i == vectors - 1 ? asked - offset : per;
		offset += vec[i].length;
	}
	sBytesRead = 0;
	READ(td, vec, vectors, NULL, received);
	bool right = true, untouched = true;
	for (size_t i = 0; i < received; i++)
		right = right && data[i] == (uint8_t)(i / size + 1);
	for (size_t i = received; i <= asked; i++)
		untouched = untouched && data[i] == 0xee;
	CHECK(right, "bulk %zu x %zu, %zu of %zu received: data wrong", count,
		size, received, asked);
	CHECK(untouched && sBytesRead == received, "bulk %zu x %zu, %zu of %zu "
		"received, %zu vectors: %zu bytes read from the buffers%s", count, size,
		received, asked, vectors, sBytesRead,
		untouched ? "" : ", the rest of the vectors written to");
}

static void iso()
{
	// 4 packets asked of 100, 101, 100, 101 bytes; 100, 50, 0, 101 came
	const uint32_t asked[4] = { 100, 101, 100, 101 };
	const uint32_t came[4] = { 100, 50, 0, 101 };
	usb_iso_packet_descriptor packets[4];
	for (int i = 0; i < 4; i++) {
		packets[i].request_length = asked[i];
		packets[i].actual_length = came[i];
	}
	usb_isochronous_data isoData = { packets, 4 };
	xhci_td* td = make_td(4, 128);
	uint8_t data[403];
	memset(data, 0xee, sizeof(data));
	generic_io_vec vec = { (generic_addr_t)data, 402 };
	sBytesRead = 0;
	READ(td, &vec, 1, &isoData, 351);
	// packet i is at the sum of the request lengths before it
	size_t offset = 0;
	bool right = true;
	for (int i = 0; i < 4; i++) {
		for (uint32_t j = 0; j < asked[i]; j++) {
			uint8_t expected = j < came[i] ? i + 1 : 0xee;
			right = right && data[offset + j] == expected;
		}
		offset += asked[i];
	}
	CHECK(right && data[402] == 0xee && sBytesRead == 251,
		"isochronous: %zu bytes read, data %s", sBytesRead,
		right ? "right" : "wrong");
}

int main()
{
	bulk(1, 1024, 1024, 1024, 1);
	bulk(1, 65536, 65536, 100, 1);		// a network frame in a 64 KiB buffer
	bulk(4, 4096, 16384, 5000, 1);		// short packet in the second buffer
	bulk(4, 4096, 16384, 8192, 3);		// at a buffer's end, three vectors
	bulk(4, 4096, 16384, 0, 2);
	bulk(4, 4096, 16000, 16000, 4);
	iso();
	printf("%s\n", sFailures == 0 ? "PASSED" : "FAILED");
	return sFailures != 0;
}
