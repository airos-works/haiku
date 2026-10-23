/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The property interface of the Raspberry Pi's VideoCore firmware
	(mailbox channel 8), for kernel drivers. The mailbox address is the
	BCM2711's: on any other SoC every request fails without touching it. */


#include <rpi_firmware.h>

#include <string.h>

#include <KernelExport.h>

#include <libfdt.h>

#include <lock.h>
#include <util/AutoLock.h>
#include <vm/vm.h>


#define MAILBOX_BASE				0xfe00b000
#define MAILBOX_READ				0x880
#define MAILBOX_STATUS				0x898
#define MAILBOX_WRITE				0x8a0
#define  MAILBOX_FULL				0x80000000
#define  MAILBOX_EMPTY				0x40000000
#define MAILBOX_CHANNEL_PROPERTY	8
#define PROPERTY_SUCCESS			0x80000000

// The VideoCore sees the ARM's first gigabyte at this bus address.
#define VC_BUS_OFFSET				0xc0000000

#define MAX_VALUE_SIZE				1024


static mutex sLock = MUTEX_INITIALIZER("rpi firmware");
static area_id sMailboxArea = -1;
static volatile uint8* sMailbox;
static area_id sMessageArea = -1;
static uint32* sMessage;
static phys_addr_t sMessageAddress;
static status_t sInitStatus = B_NO_INIT;

extern void* gFDT;


// Set up on first use, not at module load: the drivers that depend on this
// module are loaded on every board to probe for their devices, and on boards
// without RAM below 1 GB the message buffer cannot be allocated at all.
static status_t
init()
{
	if (gFDT == NULL || fdt_node_check_compatible(gFDT, 0, "brcm,bcm2711") != 0)
		return B_DEVICE_NOT_FOUND;

	sMailboxArea = map_physical_memory("rpi firmware mailbox", MAILBOX_BASE,
		B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&sMailbox);
	if (sMailboxArea < 0)
		return sMailboxArea;

	// The message has to be where the VideoCore reaches it, and out of the
	// CPU cache: the firmware answers in place.
	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	physicalRestrictions.high_address = 1ull << 30;
	sMessageArea = create_area_etc(B_SYSTEM_TEAM, "rpi firmware message",
		B_PAGE_SIZE * 2, B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		0, 0, &virtualRestrictions, &physicalRestrictions, (void**)&sMessage);
	if (sMessageArea < 0) {
		delete_area(sMailboxArea);
		return sMessageArea;
	}

	physical_entry entry;
	status_t status = get_memory_map(sMessage, B_PAGE_SIZE * 2, &entry, 1);
	if (status != B_OK) {
		delete_area(sMessageArea);
		delete_area(sMailboxArea);
		return status;
	}
	sMessageAddress = entry.address;

#ifdef __aarch64__
	for (addr_t line = (addr_t)sMessage;
			line < (addr_t)sMessage + B_PAGE_SIZE * 2; line += 64) {
		asm volatile("dc civac, %0" : : "r" (line) : "memory");
	}
#endif
	vm_set_area_memory_type(sMessageArea, sMessageAddress,
		B_WRITE_COMBINING_MEMORY);

	return B_OK;
}


static status_t
firmware_property(uint32 tag, void* data, size_t size)
{
	if (size > MAX_VALUE_SIZE || (size & 3) != 0)
		return B_BAD_VALUE;

	MutexLocker locker(sLock);

	if (sInitStatus == B_NO_INIT)
		sInitStatus = init();
	if (sInitStatus != B_OK)
		return sInitStatus;

	uint32 words = size / 4;
	sMessage[0] = (6 + words) * 4;
	sMessage[1] = 0;
	sMessage[2] = tag;
	sMessage[3] = size;
	sMessage[4] = 0;
	memcpy(&sMessage[5], data, size);
	sMessage[5 + words] = 0;
	memory_full_barrier();

	uint32 address = ((uint32)sMessageAddress | VC_BUS_OFFSET)
		| MAILBOX_CHANNEL_PROPERTY;

	bigtime_t timeout = system_time() + 1000000;
	while ((*(volatile uint32*)(sMailbox + MAILBOX_STATUS) & MAILBOX_FULL)
			!= 0) {
		if (system_time() > timeout)
			return B_TIMED_OUT;
		snooze(50);
	}
	*(volatile uint32*)(sMailbox + MAILBOX_WRITE) = address;

	while (true) {
		while ((*(volatile uint32*)(sMailbox + MAILBOX_STATUS)
				& MAILBOX_EMPTY) != 0) {
			if (system_time() > timeout)
				return B_TIMED_OUT;
			snooze(50);
		}
		if (*(volatile uint32*)(sMailbox + MAILBOX_READ) == address)
			break;
	}

	memory_full_barrier();
	if (sMessage[1] != PROPERTY_SUCCESS)
		return B_ERROR;

	memcpy(data, &sMessage[5], size);
	return B_OK;
}


static status_t
firmware_get_clock_rate(uint32 clock, bool maximum, uint32* _rate)
{
	uint32 values[2] = {clock, 0};
	status_t status = firmware_property(maximum
		? RPI_FIRMWARE_GET_MAX_CLOCK_RATE : RPI_FIRMWARE_GET_CLOCK_RATE,
		values, sizeof(values));
	if (status != B_OK)
		return status;

	*_rate = values[1];
	return B_OK;
}


static status_t
firmware_set_clock_rate(uint32 clock, uint32 rate)
{
	uint32 values[3] = {clock, rate, 0};
	return firmware_property(RPI_FIRMWARE_SET_CLOCK_RATE, values,
		sizeof(values));
}


static status_t
firmware_set_clock_state(uint32 clock, bool on)
{
	uint32 values[2] = {clock, on ? 1u : 0u};
	return firmware_property(RPI_FIRMWARE_SET_CLOCK_STATE, values,
		sizeof(values));
}


static status_t
std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
			return B_OK;

		case B_MODULE_UNINIT:
			if (sInitStatus == B_OK) {
				delete_area(sMessageArea);
				delete_area(sMailboxArea);
			}
			sInitStatus = B_NO_INIT;
			return B_OK;
	}

	return B_ERROR;
}


static rpi_firmware_module_info sModule = {
	{
		RPI_FIRMWARE_MODULE_NAME,
		0,
		std_ops
	},
	firmware_property,
	firmware_get_clock_rate,
	firmware_set_clock_rate,
	firmware_set_clock_state
};

module_info* modules[] = {
	(module_info*)&sModule,
	NULL
};
