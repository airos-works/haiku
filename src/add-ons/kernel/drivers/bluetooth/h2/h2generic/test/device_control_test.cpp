/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Host test of h2generic's device_control(): the function itself, cut out
	of h2generic.cpp by run.sh (DEVICE_CONTROL_SOURCE), against stand-ins
	for the kernel and the rest of the driver. What it checks: a command of
	any size must not size an allocation on the kernel stack, and no path
	may leave with the device's lock held or a buffer lost (haiku#195).
*/


#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


// #pragma mark - what the function needs of the kernel


typedef int32_t status_t;
typedef int32_t int32;
typedef uint32_t uint32;
typedef uint16_t uint16;
typedef uint8_t uint8;
typedef int32_t sem_id;
typedef int32_t hci_id;

#define B_OK					0
#define B_ERROR					(-1)
#define B_BAD_VALUE				(-2147483643)
#define B_NO_MEMORY				(-2147483648)
#define B_BAD_ADDRESS			(-2147478783)
#define B_DEV_INVALID_IOCTL		(-2147479552)
#define B_DEV_NOT_READY			(-2147479535)
#define B_PRId32				"d"
#define B_PRIuSIZE				"zu"

#define TRACE(x...)
#define ERROR(x...)
#define TOUCH(x)				((void)(x))

#define BT_DRIVER_SUPPORTS_ACL	1
#define MAX_ACL_IN_WINDOW		1
#define HCI_COMMAND_HDR_SIZE	3

enum {
	ISSUE_BT_COMMAND = 12999,
	GET_STATS,
	GET_NOTIFICATION_PORT,
	GET_HCI_ID,
	BT_UP
};

enum {
	ANCILLYANT = 1 << 0,
	RUNNING = 1 << 1
};

struct bt_hci_statistics {
	uint32 acceptedTX;
	uint32 rejectedTX;
};

struct list {
	int unused;
};

struct snet_buffer {
	uint8	data[512];
	size_t	size;
	bool	parked;
};

struct spinlock {
	int unused;
};

struct bt_usb_dev {
	sem_id				lock;
	uint32				state;
	hci_id				hdev;
	bt_hci_statistics	stat;
	struct list			snetBufferRecycleTrash;
	spinlock			snetBufferRecycleLock;
};


// user memory: one address is "not mapped"
static uint8 sUserMemory[70000];
static void* const kBadAddress = (void*)0x1000;
static void* const kKernelAddress = (void*)0xffffffff80001000ull;

#define IS_USER_ADDRESS(x)	((uintptr_t)(x) < 0x800000000000ull)


static status_t
user_memcpy(void* to, const void* from, size_t size)
{
	if (to == kBadAddress || from == kBadAddress)
		return B_BAD_ADDRESS;
	memcpy(to, from, size);
	return B_OK;
}


static int sLockCount;

static status_t
acquire_sem(sem_id)
{
	sLockCount++;
	return B_OK;
}


static status_t
release_sem(sem_id)
{
	sLockCount--;
	return B_OK;
}


// The kernel stack has 16 KiB: remember the largest request for it.
static size_t sLargestStackAllocation;
static void* sStackAllocations[16];
static int sStackAllocationCount;

static void*
test_alloca(size_t size)
{
	if (size > sLargestStackAllocation)
		sLargestStackAllocation = size;
	void* memory = malloc(size);
	if (sStackAllocationCount < 16)
		sStackAllocations[sStackAllocationCount++] = memory;
	return memory;
}

#undef alloca
#define alloca(size)	test_alloca(size)


// #pragma mark - what it needs of the driver


static bool sFetchFails;
static int sBuffersOut;
	// fetched and neither queued for sending nor given back
static bool sNullBufferUsed;
static snet_buffer sBuffer;
static status_t sSubmitStatus;
static snet_buffer* sSubmitted;


static snet_buffer*
snb_fetch(struct list*, uint16 size)
{
	if (sFetchFails)
		return NULL;
	sBuffersOut++;
	sBuffer.size = 0;
	sBuffer.parked = false;
	return &sBuffer;
}


static void
snb_put(snet_buffer* buffer, void* data, uint16 size)
{
	if (buffer == NULL) {
		sNullBufferUsed = true;
		return;
	}
	memcpy(buffer->data, data, size < sizeof(buffer->data)
		? size : sizeof(buffer->data));
	buffer->size = size;
}


static void
snb_park(struct list*, snet_buffer* buffer)
{
	buffer->parked = true;
	sBuffersOut--;
}


// since the recycle list has a lock (haiku#228)
static snet_buffer*
snb_fetch(struct list* list, spinlock*, uint16 size)
{
	return snb_fetch(list, size);
}


static void
snb_park(struct list* list, spinlock*, snet_buffer* buffer)
{
	snb_park(list, buffer);
}


static status_t
submit_tx_command(bt_usb_dev* device, snet_buffer* buffer)
{
	if (sSubmitStatus == B_OK) {
		// the completion gives it back
		sSubmitted = buffer;
		sBuffersOut--;
	}
	return sSubmitStatus;
}


static status_t
submit_rx_event(bt_usb_dev*)
{
	return B_OK;
}


static status_t
submit_rx_acl(bt_usb_dev*)
{
	return B_OK;
}


#include DEVICE_CONTROL_SOURCE


// #pragma mark - the test


static int sFailures;

static void
check(bool ok, const char* what)
{
	if (!ok)
		sFailures++;
	printf("%s: %s\n", ok ? "ok" : "FAILED", what);
}


static void
reset()
{
	sLockCount = 0;
	sLargestStackAllocation = 0;
	sFetchFails = false;
	sBuffersOut = 0;
	sNullBufferUsed = false;
	sSubmitStatus = B_OK;
	sSubmitted = NULL;
}


int
main()
{
	bt_usb_dev device = {};
	device.state = RUNNING;
	char text[128];

	// an ordinary command: HCI_Reset
	reset();
	const uint8 kReset[] = { 0x03, 0x0c, 0x00 };
	memcpy(sUserMemory, kReset, sizeof(kReset));
	status_t status = device_control(&device, ISSUE_BT_COMMAND, sUserMemory,
		sizeof(kReset));
	check(status == B_OK && sSubmitted != NULL
			&& sSubmitted->size == sizeof(kReset)
			&& memcmp(sSubmitted->data, kReset, sizeof(kReset)) == 0
			&& sLockCount == 0 && sBuffersOut == 0,
		"a command is sent as it was given, the lock released");

	// the longest one
	reset();
	memset(sUserMemory, 0x5a, 258);
	status = device_control(&device, ISSUE_BT_COMMAND, sUserMemory, 258);
	check(status == B_OK && sSubmitted != NULL && sSubmitted->size == 258
			&& sLockCount == 0,
		"a command with 255 parameter bytes is sent");

	// what is no command
	reset();
	status = device_control(&device, ISSUE_BT_COMMAND, sUserMemory, 259);
	check(status == B_BAD_VALUE && sSubmitted == NULL && sLockCount == 0,
		"259 bytes are refused");

	reset();
	status = device_control(&device, ISSUE_BT_COMMAND, sUserMemory, 2);
	check(status == B_BAD_VALUE && sSubmitted == NULL && sLockCount == 0,
		"2 bytes (less than a command header) are refused");

	reset();
	status = device_control(&device, ISSUE_BT_COMMAND, sUserMemory,
		sizeof(sUserMemory));
	snprintf(text, sizeof(text), "70000 bytes are refused (%zu bytes asked of "
		"the kernel stack)", sLargestStackAllocation);
	check(status == B_BAD_VALUE && sSubmitted == NULL && sLockCount == 0
			&& sLargestStackAllocation <= 4096, text);

	// a pointer that cannot be read
	reset();
	status = device_control(&device, ISSUE_BT_COMMAND, kBadAddress, 16);
	snprintf(text, sizeof(text), "an unreadable command: Bad address, the "
		"lock %s", sLockCount == 0 ? "released" : "LEFT HELD");
	check(status == B_BAD_ADDRESS && sLockCount == 0 && sBuffersOut == 0,
		text);

	reset();
	status = device_control(&device, ISSUE_BT_COMMAND, kKernelAddress, 16);
	check(status != B_OK && sSubmitted == NULL && sLockCount == 0,
		"a kernel address is refused");

	// no buffer
	reset();
	sFetchFails = true;
	memcpy(sUserMemory, kReset, sizeof(kReset));
	status = device_control(&device, ISSUE_BT_COMMAND, sUserMemory,
		sizeof(kReset));
	check(status == B_NO_MEMORY && !sNullBufferUsed && sLockCount == 0,
		"without a buffer: No memory, and no buffer used");

	// the device does not take it
	reset();
	sSubmitStatus = B_DEV_NOT_READY;
	status = device_control(&device, ISSUE_BT_COMMAND, sUserMemory,
		sizeof(kReset));
	check(status == B_DEV_NOT_READY && sBuffersOut == 0 && sLockCount == 0,
		"a command the device does not take: its buffer is given back");

	// the other requests still work
	reset();
	device.hdev = 7;
	hci_id id = -1;
	status = device_control(&device, GET_HCI_ID, &id, sizeof(id));
	check(status == B_OK && id == 7 && sLockCount == 0, "GET_HCI_ID");

	printf("%d failed\n", sFailures);
	for (int i = 0; i < sStackAllocationCount; i++)
		free(sStackAllocations[i]);
	return sFailures == 0 ? 0 : 1;
}
