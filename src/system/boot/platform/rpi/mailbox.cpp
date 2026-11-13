/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The VideoCore firmware's property interface (mailbox channel 8): the
	loader asks it for the frame buffer and for the ARM clock. */


#include "mailbox.h"

#include <string.h>

#include <boot/stage2.h>
#include <boot/stdio.h>

#include "dtb.h"
#include "mmu.h"


#define MAILBOX_READ			0x00
#define MAILBOX_STATUS			0x18
#define MAILBOX_WRITE			0x20
#define MAILBOX_STATUS_FULL		0x80000000
#define MAILBOX_STATUS_EMPTY	0x40000000
#define MAILBOX_CHANNEL_PROPERTY	8

#define PROPERTY_REQUEST		0x00000000
#define PROPERTY_SUCCESS		0x80000000

#define TAG_GET_CLOCK_RATE		0x00030002
#define TAG_GET_MAX_CLOCK_RATE	0x00030004
#define TAG_SET_CLOCK_RATE		0x00038002
#define TAG_ALLOCATE_BUFFER		0x00040001
#define TAG_GET_PHYSICAL_SIZE	0x00040003
#define TAG_GET_PITCH			0x00040008
#define TAG_SET_PHYSICAL_SIZE	0x00048003
#define TAG_SET_VIRTUAL_SIZE	0x00048004
#define TAG_SET_DEPTH			0x00048005
#define TAG_SET_PIXEL_ORDER		0x00048006
#define TAG_SET_VIRTUAL_OFFSET	0x00048009

// The BCM2711's VideoCore sees the ARM's first gigabyte at this bus address.
// The BCM2712's (Pi 5) uses the ARM's physical addresses.
#define BCM2711_VC_BUS_OFFSET	0xc0000000

#define MAILBOX_TIMEOUT			1000000


static addr_t sMailbox = 0xfe00b880;

// The firmware reads and writes the message in memory, so keep it apart
// from everything else in the cache.
static uint32 sMessage[128] __attribute__((aligned(64)));
static uint32 sMessageLength;


static inline uint32
read32(uint32 reg)
{
	return *(volatile uint32*)(sMailbox + reg);
}


static inline void
write32(uint32 reg, uint32 value)
{
	*(volatile uint32*)(sMailbox + reg) = value;
}


static void
begin_message()
{
	sMessage[1] = PROPERTY_REQUEST;
	sMessageLength = 2;
}


/*!	Adds a tag with \a valueSize bytes of value buffer, the first
	\a count words of it set from \a values. Returns the index of the value
	buffer in the message.
*/
static uint32
add_tag(uint32 tag, uint32 valueSize, const uint32* values, uint32 count)
{
	sMessage[sMessageLength++] = tag;
	sMessage[sMessageLength++] = valueSize;
	sMessage[sMessageLength++] = 0;
	uint32 index = sMessageLength;
	for (uint32 i = 0; i < valueSize / 4; i++)
		sMessage[sMessageLength++] = i < count ? values[i] : 0;
	return index;
}


static status_t
send_message()
{
	sMessage[sMessageLength++] = 0;
	sMessage[0] = sMessageLength * 4;

	uint32 address = (uint32)(addr_t)sMessage
		| (gBCM2712 ? 0 : BCM2711_VC_BUS_OFFSET);

	mmu_flush_data_cache(sMessage, sizeof(sMessage));

	bigtime_t timeout = system_time() + MAILBOX_TIMEOUT;
	while ((read32(MAILBOX_STATUS) & MAILBOX_STATUS_FULL) != 0) {
		if (system_time() > timeout)
			return B_TIMED_OUT;
	}
	write32(MAILBOX_WRITE, address | MAILBOX_CHANNEL_PROPERTY);

	while (true) {
		while ((read32(MAILBOX_STATUS) & MAILBOX_STATUS_EMPTY) != 0) {
			if (system_time() > timeout)
				return B_TIMED_OUT;
		}
		if (read32(MAILBOX_READ) == (address | MAILBOX_CHANNEL_PROPERTY))
			break;
	}

	mmu_flush_data_cache(sMessage, sizeof(sMessage));
	return sMessage[1] == PROPERTY_SUCCESS ? B_OK : B_ERROR;
}


void
mailbox_init()
{
	addr_range range;
	if (fdt_find_compatible_reg("brcm,bcm2835-mbox", range))
		sMailbox = range.start;
}


status_t
mailbox_get_clock_rate(uint32 clock, bool maximum, uint32& _rate)
{
	begin_message();
	uint32 index = add_tag(maximum
		? TAG_GET_MAX_CLOCK_RATE : TAG_GET_CLOCK_RATE, 8, &clock, 1);
	status_t status = send_message();
	if (status != B_OK)
		return status;

	_rate = sMessage[index + 1];
	return _rate != 0 ? B_OK : B_ERROR;
}


status_t
mailbox_set_clock_rate(uint32 clock, uint32 rate)
{
	uint32 values[3] = {clock, rate, 0};
		// 0: let the firmware raise the voltage as needed ("turbo")
	begin_message();
	add_tag(TAG_SET_CLOCK_RATE, 12, values, 3);
	return send_message();
}


status_t
mailbox_get_display_size(uint32& _width, uint32& _height)
{
	begin_message();
	uint32 index = add_tag(TAG_GET_PHYSICAL_SIZE, 8, NULL, 0);
	status_t status = send_message();
	if (status != B_OK)
		return status;

	_width = sMessage[index];
	_height = sMessage[index + 1];
	return B_OK;
}


status_t
mailbox_allocate_frame_buffer(uint32 width, uint32 height,
	mailbox_frame_buffer& _buffer)
{
	uint32 size[2] = {width, height};
	uint32 depth = 32;
	uint32 pixelOrder = 0;
		// blue in the lowest byte, which is B_RGB32
	uint32 alignment = 4096;

	begin_message();
	uint32 physical = add_tag(TAG_SET_PHYSICAL_SIZE, 8, size, 2);
	add_tag(TAG_SET_VIRTUAL_SIZE, 8, size, 2);
	add_tag(TAG_SET_VIRTUAL_OFFSET, 8, NULL, 0);
	uint32 depthIndex = add_tag(TAG_SET_DEPTH, 4, &depth, 1);
	add_tag(TAG_SET_PIXEL_ORDER, 4, &pixelOrder, 1);
	uint32 buffer = add_tag(TAG_ALLOCATE_BUFFER, 8, &alignment, 1);
	uint32 pitch = add_tag(TAG_GET_PITCH, 4, NULL, 0);

	status_t status = send_message();
	if (status != B_OK)
		return status;

	if (sMessage[buffer] == 0 || sMessage[buffer + 1] == 0
		|| sMessage[depthIndex] != 32) {
		return B_ERROR;
	}

	// A bus address: the Pi 4's VideoCore puts its alias bits on top. The
	// Pi 5's buffer is just below 1 GB too (the gap in the memory map).
	_buffer.base = sMessage[buffer] & 0x3fffffff;
	_buffer.size = sMessage[buffer + 1];
	_buffer.width = sMessage[physical];
	_buffer.height = sMessage[physical + 1];
	_buffer.bytesPerRow = sMessage[pitch];
	return B_OK;
}
