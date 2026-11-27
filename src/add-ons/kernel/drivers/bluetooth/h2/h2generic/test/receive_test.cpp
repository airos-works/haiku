/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Host test of h2generic's receive completions, event_complete() and
	acl_rx_complete(): the part of h2transactions.cpp from "RX Complete" to
	sco_rx_complete(), cut out by run.sh (RECEIVE_SOURCE), against
	stand-ins for the USB stack and the rest of the driver. The driver
	keeps exactly one transfer queued per endpoint and queues it again from
	its completion; what it checks is that a failed transfer does not end
	reception for good (haiku#228): a CRC error is retried, a stall is
	cleared in the device first, a run of failures ends in giving up rather
	than in a loop, and a cancelled one stays cancelled.
*/


#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


// #pragma mark - what the code needs of the kernel and USB


typedef int32_t status_t;
typedef int32_t int32;
typedef uint32_t uint32;
typedef uint16_t uint16;
typedef uint8_t uint8;
typedef int32_t hci_id;
typedef uint32_t usb_id;
typedef usb_id usb_device;
typedef usb_id usb_pipe;

#define B_OK					0
#define B_ERROR					(-1)
#define B_NO_MEMORY				(-2147483647 - 1)
#define B_CANCELED				(-2147483647 + 6)
#define B_DEV_CRC_ERROR			(-2147479552 + 20)
#define B_DEV_STALLED			(-2147479552 + 21)
#define B_DEV_DATA_OVERRUN		(-2147479552 + 24)
#define B_PRId32				"d"
#define B_PRIuSIZE				"zu"

#define TRACE(x...)
#define ERROR(x...)

#define max_c(a, b)				((a) > (b) ? (a) : (b))

#define HCI_MAX_EVENT_SIZE		260
#define HCI_MAX_FRAME_SIZE		(1024 + 4)

#define USB_REQTYPE_STANDARD			0x00
#define USB_REQTYPE_ENDPOINT_OUT		0x02
#define USB_REQUEST_CLEAR_FEATURE		1
#define USB_FEATURE_ENDPOINT_HALT		0

typedef enum { BT_COMMAND = 0, BT_EVENT, BT_ACL, BT_SCO } bt_packet_t;

enum {
	RUNNING = 1 << 1
};

typedef void (*usb_callback_func)(void* cookie, status_t status, void* data,
	size_t actualLength);

struct usb_endpoint_descriptor {
	uint8	endpoint_address;
};

struct usb_endpoint_info {
	usb_endpoint_descriptor*	descr;
	usb_pipe					handle;
};

struct bt_hci_statistics {
	uint32 acceptedRX;
	uint32 rejectedRX;
	uint32 successfulTX;
	uint32 successfulRX;
	uint32 errorRX;
	uint32 bytesRX;
};

struct list {
	int		items;
};

struct bt_usb_dev {
	usb_device					dev;
	hci_id						hdev;
	uint32						state;
	bt_hci_statistics			stat;
	const usb_endpoint_info*	bulk_in_ep;
	uint16						max_packet_size_bulk_in;
	const usb_endpoint_info*	intr_in_ep;
	uint16						max_packet_size_intr_in;
	struct list					eventRoom;
	struct list					aclRoom;
	int32						eventErrors;
	int32						aclErrors;
};


// What was queued with the USB stack.
struct Queued {
	enum { NONE, INTERRUPT, BULK, REQUEST } kind;
	usb_pipe			pipe;
	void*				data;
	usb_callback_func	callback;
	void*				cookie;
	uint16				value;
	uint16				index;
	uint8				requestType;
	uint8				request;
};

static Queued sQueued[64];
static int sQueuedCount;
static status_t sQueueStatus = B_OK;


static status_t
fake_queue_interrupt(usb_pipe pipe, void* data, size_t size,
	usb_callback_func callback, void* cookie)
{
	if (sQueueStatus != B_OK)
		return sQueueStatus;
	Queued& queued = sQueued[sQueuedCount++ % 64];
	queued = Queued();
	queued.kind = Queued::INTERRUPT;
	queued.pipe = pipe;
	queued.data = data;
	queued.callback = callback;
	queued.cookie = cookie;
	return B_OK;
}


static status_t
fake_queue_bulk(usb_pipe pipe, void* data, size_t size,
	usb_callback_func callback, void* cookie)
{
	if (sQueueStatus != B_OK)
		return sQueueStatus;
	Queued& queued = sQueued[sQueuedCount++ % 64];
	queued = Queued();
	queued.kind = Queued::BULK;
	queued.pipe = pipe;
	queued.data = data;
	queued.callback = callback;
	queued.cookie = cookie;
	return B_OK;
}


static status_t
fake_queue_request(usb_device device, uint8 requestType, uint8 request,
	uint16 value, uint16 index, uint16 length, void* data,
	usb_callback_func callback, void* cookie)
{
	Queued& queued = sQueued[sQueuedCount++ % 64];
	queued = Queued();
	queued.kind = Queued::REQUEST;
	queued.data = data;
	queued.callback = callback;
	queued.cookie = cookie;
	queued.requestType = requestType;
	queued.request = request;
	queued.value = value;
	queued.index = index;
	return B_OK;
}


struct usb_module_info {
	status_t (*queue_interrupt)(usb_pipe, void*, size_t, usb_callback_func,
		void*);
	status_t (*queue_bulk)(usb_pipe, void*, size_t, usb_callback_func,
		void*);
	status_t (*queue_request)(usb_device, uint8, uint8, uint16, uint16,
		uint16, void*, usb_callback_func, void*);
};

static usb_module_info sUSB = {
	fake_queue_interrupt,
	fake_queue_bulk,
	fake_queue_request
};
static usb_module_info* usb = &sUSB;


static int sPosted;

static status_t
assembly_rx(bt_usb_dev* bdev, bt_packet_t type, void* data, int count)
{
	sPosted++;
	return B_OK;
}


static int sRoomed;

static void
reuse_room(struct list* room, void* buffer)
{
	room->items++;
	sRoomed++;
}


void event_complete(void* cookie, status_t status, void* data,
	size_t actual_len);
void acl_rx_complete(void* cookie, status_t status, void* data,
	size_t actual_len);


#include RECEIVE_SOURCE


// #pragma mark - the test


static int sFailures;

static void
check(bool ok, const char* what)
{
	if (!ok)
		sFailures++;
	printf("%s: %s\n", ok ? "ok" : "FAILED", what);
}


static usb_endpoint_descriptor sEventDescriptor = { 0x81 };
static usb_endpoint_descriptor sAclDescriptor = { 0x82 };
static usb_endpoint_info sEventEndpoint = { &sEventDescriptor, 11 };
static usb_endpoint_info sAclEndpoint = { &sAclDescriptor, 12 };


static void
reset(bt_usb_dev& device)
{
	memset(&device, 0, sizeof(device));
	device.state = RUNNING;
	device.intr_in_ep = &sEventEndpoint;
	device.bulk_in_ep = &sAclEndpoint;
	device.max_packet_size_intr_in = 16;
	device.max_packet_size_bulk_in = 64;
	sQueuedCount = 0;
	sPosted = 0;
	sRoomed = 0;
	sQueueStatus = B_OK;
}


static bool
queued_again(int index, int kind, void* buffer)
{
	return sQueuedCount > index && sQueued[index].kind == kind
		&& sQueued[index].data == buffer;
}


static void
test_endpoint(bool events)
{
	const char* name = events ? "event" : "ACL";
	usb_callback_func complete = events ? event_complete : acl_rx_complete;
	const int kind = events ? Queued::INTERRUPT : Queued::BULK;
	const uint8 address = events ? 0x81 : 0x82;
	bt_usb_dev device;
	char text[200];
	void* buffer = malloc(2048);

	reset(device);
	complete(&device, B_OK, buffer, 6);
	snprintf(text, sizeof(text), "%s: data is passed on and the transfer "
		"queued again", name);
	check(sPosted == 1 && queued_again(0, kind, buffer), text);

	reset(device);
	complete(&device, B_DEV_CRC_ERROR, buffer, 0);
	snprintf(text, sizeof(text), "%s: after a CRC error the transfer is "
		"queued again", name);
	check(queued_again(0, kind, buffer), text);

	reset(device);
	complete(&device, B_DEV_DATA_OVERRUN, buffer, 0);
	snprintf(text, sizeof(text), "%s: after another error the transfer is "
		"queued again", name);
	check(queued_again(0, kind, buffer), text);

	reset(device);
	complete(&device, B_DEV_STALLED, buffer, 0);
	bool cleared = sQueuedCount == 1 && sQueued[0].kind == Queued::REQUEST
		&& sQueued[0].request == USB_REQUEST_CLEAR_FEATURE
		&& sQueued[0].value == USB_FEATURE_ENDPOINT_HALT
		&& sQueued[0].index == address
		&& sQueued[0].requestType
			== (USB_REQTYPE_STANDARD | USB_REQTYPE_ENDPOINT_OUT);
	snprintf(text, sizeof(text), "%s: a stall is cleared in the device "
		"(CLEAR_FEATURE ENDPOINT_HALT to 0x%02x), not resubmitted into at "
		"once", name, address);
	check(cleared, text);
	if (cleared) {
		sQueued[0].callback(sQueued[0].cookie, B_OK, NULL, 0);
		snprintf(text, sizeof(text), "%s: once that is done, the transfer "
			"is queued again", name);
		check(queued_again(1, kind, buffer), text);
	}

	// The device goes away while its halt is being cleared: the request is
	// cancelled, and the device structure is gone (AddressSanitizer would
	// catch a use of it).
	bt_usb_dev* gone = (bt_usb_dev*)malloc(sizeof(bt_usb_dev));
	reset(*gone);
	complete(gone, B_DEV_STALLED, buffer, 0);
	if (sQueuedCount == 1 && sQueued[0].kind == Queued::REQUEST) {
		free(gone);
		sQueued[0].callback(sQueued[0].cookie, B_CANCELED, NULL, 0);
		snprintf(text, sizeof(text), "%s: a cancelled clear leaves the "
			"device alone and queues nothing", name);
		check(sQueuedCount == 1, text);
		// the buffer went with it
		buffer = malloc(2048);
	} else
		free(gone);

	reset(device);
	int failures = 0;
	while (failures < 100) {
		int before = sQueuedCount;
		complete(&device, B_DEV_CRC_ERROR, buffer, 0);
		failures++;
		if (sQueuedCount == before)
			break;
	}
	snprintf(text, sizeof(text), "%s: an endpoint that fails every time is "
		"given up after %d failures, its buffer kept", name, failures);
	check(failures > 4 && failures < 100 && sRoomed == 1, text);

	reset(device);
	bool going = true;
	for (int round = 0; round < 10 && going; round++) {
		for (int i = 0; i < 10 && going; i++) {
			int before = sQueuedCount;
			complete(&device, B_DEV_CRC_ERROR, buffer, 0);
			going = sQueuedCount > before;
		}
		int before = sQueuedCount;
		complete(&device, B_OK, buffer, 4);
		going = going && sQueuedCount > before;
	}
	snprintf(text, sizeof(text), "%s: errors now and then, with data in "
		"between, never end reception", name);
	check(going, text);

	reset(device);
	complete(&device, B_CANCELED, buffer, 0);
	snprintf(text, sizeof(text), "%s: a cancelled transfer is not queued "
		"again", name);
	check(sQueuedCount == 0, text);

	reset(device);
	device.state = 0;
	complete(&device, B_DEV_CRC_ERROR, buffer, 0);
	snprintf(text, sizeof(text), "%s: nor one of a closed device", name);
	check(sQueuedCount == 0, text);

	free(buffer);
}


int
main()
{
	test_endpoint(true);
	test_endpoint(false);

	printf("%d failed\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
