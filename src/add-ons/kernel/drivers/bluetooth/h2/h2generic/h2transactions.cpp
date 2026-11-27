/*
 * Copyright 2007 Oliver Ruiz Dorantes, oliver.ruiz.dorantes_at_gmail.com
 * Copyright 2008 Mika Lindqvist, monni1995_at_gmail.com
 * All rights reserved. Distributed under the terms of the MIT License.
 */


#include "h2transactions.h"

#include <bluetooth/HCI/btHCI.h>
#include <bluetooth/HCI/btHCI_event.h>
#include <bluetooth/HCI/btHCI_acl.h>

#include <ByteOrder.h>
#include <kernel.h>
#include <string.h>

#include "Errors.h"
#include "h2debug.h"
#include "h2generic.h"
#include "h2upper.h"
#include "h2util.h"


//#define DUMP_BUFFERS

/* Forward declaration */

void sco_tx_complete(void* cookie, status_t status, void* data, size_t actual_len);
void sco_rx_complete(void* cookie, status_t status, void* data, size_t actual_len);
void acl_tx_complete(void* cookie, status_t status, void* data, size_t actual_len);
void acl_rx_complete(void* cookie, status_t status, void* data, size_t actual_len);
void command_complete(void* cookie, status_t status, void* data, size_t actual_len);
void event_complete(void* cookie, status_t status, void* data, size_t actual_len);


static status_t
assembly_rx(bt_usb_dev* bdev, bt_packet_t type, void* data, int count)
{
	bdev->stat.bytesRX += count;

	return btDevices->PostTransportPacket(bdev->hdev, type, data, count);

}


#if 0
#pragma mark --- RX Complete ---
#endif

// Receive transfers that may fail in a row before the endpoint is given up.
// Errors come singly (a CRC error on a noisy bus, a stall the device clears
// at once); a device that fails every transfer has gone wrong for good, and
// retrying it for ever would keep the host controller busy for nothing.
static const int32 kMaxReceiveErrors = 16;


struct halt_request {
	bt_usb_dev*	bdev;
	void*		buffer;
	bool		events;
};


static status_t
queue_receive(bt_usb_dev* bdev, bool events, void* buffer)
{
	if (events) {
		return usb->queue_interrupt(bdev->intr_in_ep->handle, buffer,
			max_c(HCI_MAX_EVENT_SIZE, bdev->max_packet_size_intr_in),
			event_complete, bdev);
	}

	return usb->queue_bulk(bdev->bulk_in_ep->handle, buffer,
		max_c(HCI_MAX_FRAME_SIZE, bdev->max_packet_size_bulk_in),
		acl_rx_complete, bdev);
}


static void
resubmit_receive(bt_usb_dev* bdev, bool events, void* buffer)
{
	status_t error = queue_receive(bdev, events, buffer);
	if (error != B_OK) {
		reuse_room(events ? &bdev->eventRoom : &bdev->aclRoom, buffer);
		bdev->stat.rejectedRX++;
		ERROR("%s: RX %s resubmittion failed %s\n", __func__,
			events ? "event" : "acl", strerror(error));
	} else {
		bdev->stat.acceptedRX++;
	}
}


static void
halt_cleared(void* cookie, status_t status, void* data, size_t actualLength)
{
	halt_request* request = (halt_request*)cookie;
	bt_usb_dev* bdev = request->bdev;
	void* buffer = request->buffer;
	bool events = request->events;
	free(request);

	if (status == B_CANCELED) {
		// The device is going away: it may be gone already. Nothing will
		// use the buffer again.
		free(buffer);
		return;
	}

	if (status != B_OK) {
		ERROR("%s: the %s endpoint could not be cleared: %s\n", __func__,
			events ? "event" : "ACL", strerror(status));
	}

	if ((bdev->state & RUNNING) == 0) {
		reuse_room(events ? &bdev->eventRoom : &bdev->aclRoom, buffer);
		return;
	}

	// Even when clearing it failed: the next failure counts too, and the
	// count gives up on the endpoint in the end.
	resubmit_receive(bdev, events, buffer);
}


/*!	A receive transfer failed. Returns whether to queue it again now; when
	not, the buffer is taken care of: it waits for the endpoint's halt to
	be cleared, or the endpoint is given up.
*/
static bool
receive_failed(bt_usb_dev* bdev, bool events, void* buffer, status_t status)
{
	int32& errors = events ? bdev->eventErrors : bdev->aclErrors;
	const usb_endpoint_info* endpoint
		= events ? bdev->intr_in_ep : bdev->bulk_in_ep;

	bdev->stat.errorRX++;

	if (++errors > kMaxReceiveErrors) {
		// TODO: tell the bluetooth_server, so that it can reset the radio
		// (it does nothing about a Hardware Error event yet).
		ERROR("%s: %" B_PRId32 " %s transfers failed in a row (the last: "
			"%s), no longer receiving\n", __func__, errors - 1,
			events ? "event" : "ACL", strerror(status));
		reuse_room(events ? &bdev->eventRoom : &bdev->aclRoom, buffer);
		return false;
	}

	if (status != B_DEV_STALLED)
		return true;

	// The device halted the endpoint, and it stays halted in the device
	// until the host clears that: receiving again at once would only stall
	// again (the host controller resets its own side of the endpoint on
	// every stall). Clear it, and receive again once that is done. This
	// runs on the host controller's completion thread, which must not wait.
	halt_request* request = (halt_request*)malloc(sizeof(halt_request));
	if (request == NULL)
		return true;
	request->bdev = bdev;
	request->buffer = buffer;
	request->events = events;

	status_t error = usb->queue_request(bdev->dev,
		USB_REQTYPE_STANDARD | USB_REQTYPE_ENDPOINT_OUT,
		USB_REQUEST_CLEAR_FEATURE, USB_FEATURE_ENDPOINT_HALT,
		endpoint->descr->endpoint_address, 0, NULL, halt_cleared, request);
	if (error != B_OK) {
		ERROR("%s: clearing the %s endpoint's halt failed: %s\n", __func__,
			events ? "event" : "ACL", strerror(error));
		free(request);
		return true;
	}

	return false;
}


void
event_complete(void* cookie, status_t status, void* data, size_t actual_len)
{
	bt_usb_dev* bdev = (bt_usb_dev*)cookie;
	// bt_usb_dev* bdev = fetch_device(cookie, 0); -> safer / slower option

	TRACE("%s: cookie@%p status=%s len=%" B_PRIuSIZE "\n", __func__, cookie,
		strerror(status), actual_len);

	if (bdev == NULL)
		return;

	if (status == B_CANCELED || (bdev->state & RUNNING) == 0)
		return; // or not running anymore...

	// A failed transfer is queued again too: the driver keeps exactly one
	// queued, so one that is not means no events (Command Complete,
	// connections) until the radio is opened again.
	if (status != B_OK) {
		if (!receive_failed(bdev, true, data, status))
			return;
	} else {
		bdev->eventErrors = 0;

		if (actual_len > 0) {
			if (assembly_rx(bdev, BT_EVENT, data, actual_len) == B_OK) {
				bdev->stat.successfulTX++;
			} else {
				bdev->stat.errorRX++;
			}
		}
	}

	resubmit_receive(bdev, true, data);
}


void
acl_rx_complete(void* cookie, status_t status, void* data, size_t actual_len)
{
	bt_usb_dev* bdev = (bt_usb_dev*)cookie;
	// bt_usb_dev* bdev = fetch_device(cookie, 0); -> safer / slower option

	if (bdev == NULL)
		return;

	if (status == B_CANCELED || (bdev->state & RUNNING) == 0)
		return; // or not running anymore...

	if (status != B_OK) {
		if (!receive_failed(bdev, false, data, status))
			return;
	} else {
		bdev->aclErrors = 0;

		if (actual_len > 0) {
			if (assembly_rx(bdev, BT_ACL, data, actual_len) == B_OK) {
				bdev->stat.successfulRX++;
			} else {
				bdev->stat.errorRX++;
			}
		}
	}

	resubmit_receive(bdev, false, data);
}


void
sco_rx_complete(void* cookie, status_t status, void* data, size_t actual_len)
{
	sco_rx_transfer_t* ctx = (sco_rx_transfer_t*)cookie;
	if (status == B_CANCELED)
		return;

	if (ctx == NULL)
		return;

	bt_usb_dev* bdev = ctx->bdev;
	status_t error;

	if ((bdev->state & RUNNING) == 0)
		return;

	size_t packetSize = bdev->max_packet_size_iso_in;

	if (status != B_OK || actual_len == 0)
		goto resubmit;
	{
		bool frame_errors = false;
		uint8* frame_ptr = (uint8*)data;

		for (uint32 i = 0; i < SCO_PACKETS_PER_BUFFER; i++) {
			if (ctx->descriptors[i].status == B_OK && ctx->descriptors[i].actual_length > 0) {
				if (assembly_rx(bdev, BT_SCO, frame_ptr, ctx->descriptors[i].actual_length) != B_OK)
					frame_errors = true;
			}
			frame_ptr += packetSize;
		}

		if (!frame_errors)
			bdev->stat.successfulRX++;
		else
			bdev->stat.errorRX++;
	}

resubmit:

	for (uint32 i = 0; i < SCO_PACKETS_PER_BUFFER; i++) {
		ctx->descriptors[i].request_length = packetSize;
		ctx->descriptors[i].actual_length = 0;
		ctx->descriptors[i].status = B_OK;
	}

	uint32 startingFrame = 0;
	error = usb->queue_isochronous(bdev->iso_in_ep->handle, data,
		SCO_PACKETS_PER_BUFFER * packetSize, ctx->descriptors, SCO_PACKETS_PER_BUFFER,
		&startingFrame, USB_ISO_ASAP, sco_rx_complete, (void*)ctx);

	if (error != B_OK) {
		reuse_room(&bdev->scoRoom, ctx);
		bdev->stat.rejectedRX++;
		ERROR("%s: RX sco resubmittion failed %s\n", __func__, strerror(error));
	} else {
		bdev->stat.acceptedRX++;
	}
}


#if 0
#pragma mark --- RX ---
#endif

status_t
submit_rx_event(bt_usb_dev* bdev)
{
	size_t size = max_c(HCI_MAX_EVENT_SIZE, bdev->max_packet_size_intr_in);
	void* buf = alloc_room(&bdev->eventRoom, size);
	status_t status;

	if (buf == NULL)
		return ENOMEM;

	status = usb->queue_interrupt(bdev->intr_in_ep->handle,	buf, size,
		event_complete, (void*)bdev);

	if (status != B_OK) {
		reuse_room(&bdev->eventRoom, buf); // reuse allocated one
		bdev->stat.rejectedRX++;
	} else {
		bdev->stat.acceptedRX++;
		TRACE("%s: Accepted RX Event %d\n", __func__, bdev->stat.acceptedRX);
	}

	return status;
}


status_t
submit_rx_acl(bt_usb_dev* bdev)
{
	size_t size = max_c(HCI_MAX_FRAME_SIZE, bdev->max_packet_size_bulk_in);
	void* buf = alloc_room(&bdev->aclRoom, size);
	status_t status;

	if (buf == NULL)
		return ENOMEM;

	status = usb->queue_bulk(bdev->bulk_in_ep->handle, buf, size,
		acl_rx_complete, bdev);

	if (status != B_OK) {
		reuse_room(&bdev->aclRoom, buf); // reuse allocated
		bdev->stat.rejectedRX++;
	} else {
		bdev->stat.acceptedRX++;
	}

	return status;
}


status_t
submit_rx_sco(bt_usb_dev* bdev)
{

	size_t packetSize = bdev->max_packet_size_iso_in;
	if (packetSize == 0)
		return B_ERROR;

	size_t dataSize = SCO_PACKETS_PER_BUFFER * packetSize;
	size_t roomSize = sizeof(sco_rx_transfer_t) + dataSize;

	sco_rx_transfer_t* ctx = (sco_rx_transfer_t*)alloc_room(&bdev->scoRoom, roomSize);
	if (ctx == NULL)
		return B_NO_MEMORY;

	ctx->bdev = bdev;

	for (uint32 i = 0; i < SCO_PACKETS_PER_BUFFER; i++) {
		ctx->descriptors[i].request_length = packetSize;
		ctx->descriptors[i].actual_length = 0;
		ctx->descriptors[i].status = B_OK;
	}

	void* dataBuffer = (uint8*)ctx + sizeof(sco_rx_transfer_t);

	uint32 startingFrame = 0;
	status_t status
		= usb->queue_isochronous(bdev->iso_in_ep->handle, dataBuffer, dataSize, ctx->descriptors,
			SCO_PACKETS_PER_BUFFER, &startingFrame, USB_ISO_ASAP, sco_rx_complete, ctx);

	if (status != B_OK) {
		reuse_room(&bdev->scoRoom, ctx);
		bdev->stat.rejectedRX++;
	} else {
		bdev->stat.acceptedRX++;
	}

	return status;
}


#if 0
#pragma mark --- TX Complete ---
#endif

void
command_complete(void* cookie, status_t status, void* data, size_t actual_len)
{
	snet_buffer* snbuf = (snet_buffer*)cookie;
	bt_usb_dev* bdev = (bt_usb_dev*)snb_cookie(snbuf);

	TRACE("%s: len = %" B_PRIuSIZE " @%p\n", __func__, actual_len, data);

	if (status == B_OK) {
		bdev->stat.successfulTX++;
		bdev->stat.bytesTX += actual_len;
	} else {
		bdev->stat.errorTX++;
		// the packet has been lost, too late to requeue it
	}

	snb_park(&bdev->snetBufferRecycleTrash,
		&bdev->snetBufferRecycleLock, snbuf);

#ifdef BT_RESCHEDULING_AFTER_COMPLETITIONS
	// TODO: check just the empty queues
	schedTxProcessing(bdev);
#endif
}


void
acl_tx_complete(void* cookie, status_t status, void* data, size_t actual_len)
{
	net_buffer* nbuf = (net_buffer*)cookie;
	bt_usb_dev* bdev = GET_DEVICE(nbuf);

	//debugf("fetched=%p type %lx %p\n", bdev, nbuf->type, data);

	if (status == B_OK) {
		bdev->stat.successfulTX++;
		bdev->stat.bytesTX += actual_len;
	} else {
		bdev->stat.errorTX++;
		// the packet has been lost, too late to requeue it
	}

	nb_destroy(nbuf);

#ifdef BT_RESCHEDULING_AFTER_COMPLETITIONS
	schedTxProcessing(bdev);
#endif
}


void
sco_tx_complete(void* cookie, status_t status, void* data, size_t actual_len)
{
	sco_tx_transfer_t* transfer = (sco_tx_transfer_t*)cookie;
	bt_usb_dev* bdev = transfer->bdev;

	if (status == B_OK) {
		bdev->stat.successfulTX++;
		bdev->stat.bytesTX += actual_len;
	} else {
		bdev->stat.errorTX++;
	}

	nb_destroy(transfer->nbuf);

	free(transfer->packet_descriptors);
	free(transfer);

#ifdef BT_RESCHEDULING_AFTER_COMPLETITIONS
	schedTxProcessing(bdev);
#endif
}

#if 0
#pragma mark --- TX ---
#endif

status_t
submit_tx_command(bt_usb_dev* bdev, snet_buffer* snbuf)
{
	uint8 bRequestType = bdev->ctrl_req;
	uint8 bRequest = 0;
	uint16 wIndex = 0;
	uint16 value = 0;
	uint16 wLength = B_HOST_TO_LENDIAN_INT16(snb_size(snbuf));
	status_t error;

	if ((bdev->state & RUNNING) == 0) {
		return B_DEV_NOT_READY;
	}

	// set cookie
	snb_set_cookie(snbuf, bdev);

	TRACE("%s: @%p\n", __func__, snb_get(snbuf));

	error = usb->queue_request(bdev->dev, bRequestType, bRequest,
		value, wIndex, wLength,	snb_get(snbuf),
		command_complete, (void*) snbuf);

	if (error != B_OK) {
		bdev->stat.rejectedTX++;
	} else {
		bdev->stat.acceptedTX++;
	}

	return error;
}


status_t
submit_tx_acl(bt_usb_dev* bdev, net_buffer* nbuf)
{
	status_t error;

	// set cookie
	SET_DEVICE(nbuf, bdev->hdev);

	if ((bdev->state & RUNNING) == 0) {
		return B_DEV_NOT_READY;
	}
	/*
	debugf("### Outgoing ACL: len = %ld\n", nbuf->size);
	for (uint32 index = 0 ; index < nbuf->size; index++ ) {
		dprintf("%x:",((uint8*)nb_get_whole_buffer(nbuf))[index]);
	}
	*/

	error = usb->queue_bulk(bdev->bulk_out_ep->handle, nb_get_whole_buffer(nbuf),
		nbuf->size, acl_tx_complete, (void*)nbuf);

	if (error != B_OK) {
		bdev->stat.rejectedTX++;
	} else {
		bdev->stat.acceptedTX++;
	}

	return error;
}


status_t
submit_tx_sco(bt_usb_dev* bdev, net_buffer* nbuf)
{
	status_t error;

	if ((bdev->state & RUNNING) == 0)
		return B_DEV_NOT_READY;

	SET_DEVICE(nbuf, bdev->hdev);

	size_t maxPacketSize = bdev->max_packet_size_iso_out;
	if (maxPacketSize == 0)
		return B_ERROR;

	uint32 packetCount = (nbuf->size + maxPacketSize - 1) / maxPacketSize;

	sco_tx_transfer_t* transfer = (sco_tx_transfer_t*)malloc(sizeof(sco_tx_transfer_t));
	usb_iso_packet_descriptor* descriptors
		= (usb_iso_packet_descriptor*)malloc(sizeof(usb_iso_packet_descriptor) * packetCount);

	if (transfer == NULL || descriptors == NULL) {
		free(transfer);
		free(descriptors);
		return B_NO_MEMORY;
	}

	size_t remaining = nbuf->size;
	for (uint32 i = 0; i < packetCount; i++) {
		descriptors[i].request_length = (remaining > maxPacketSize) ? maxPacketSize : remaining;
		descriptors[i].actual_length = 0;
		descriptors[i].status = B_OK;

		remaining -= descriptors[i].request_length;
	}


	transfer->bdev = bdev;
	transfer->nbuf = nbuf;
	transfer->packet_descriptors = descriptors;

	uint32 startingFrameNumber = 0;
	error = usb->queue_isochronous(bdev->iso_out_ep->handle, nb_get_whole_buffer(nbuf), nbuf->size,
		descriptors, packetCount, &startingFrameNumber, USB_ISO_ASAP, sco_tx_complete,
		(void*)transfer);

	if (error != B_OK) {
		free(descriptors);
		free(transfer);
		bdev->stat.rejectedTX++;
	} else {
		bdev->stat.acceptedTX++;
	}

	return error;
}
