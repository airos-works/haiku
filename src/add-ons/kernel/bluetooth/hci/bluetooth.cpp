/*
 * Copyright 2008, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Oliver Ruiz Dorantes, oliver.ruiz.dorantes_at_gmail.com
 */

#include <new>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <KernelExport.h>
#include <lock.h>
#include <SupportDefs.h>
#include <util/AutoLock.h>
#include <util/DoublyLinkedList.h>

#include <net_buffer.h>
#include <net_device.h>
#include <net_stack.h>
#include <NetBufferUtilities.h>

#include <btDebug.h>
#include <btCoreData.h>
#include <btModules.h>
#include <CodeHandler.h>
#define KERNEL_LAND
#include <PortListener.h>
#undef KERNEL_LAND

#include <bluetooth/HCI/btHCI.h>
#include <bluetooth/HCI/btHCI_acl.h>
#include <bluetooth/HCI/btHCI_command.h>
#include <bluetooth/HCI/btHCI_event.h>
#include <bluetooth/HCI/btHCI_transport.h>
#include <bluetooth/HCI/btHCI_sco.h>
#include <bluetooth/bdaddrUtils.h>

#include "acl.h"


/*!	Host to controller ACL flow control (Core Vol 4 Part E 4.1.1): the
	controller has a fixed number of ACL data buffers, and gives them back
	with Number Of Completed Packets events. Sending more than it has room
	for loses data. Fragments wait here in order until there is a buffer.
	While the buffer counts are not known (no Read Buffer Size seen yet),
	everything goes straight through, as before.
*/
struct acl_flow_control {
	mutex		lock;
	int32		aclMax;
	int32		aclFree;
	int32		leMax;
		// 0: LE links use the ACL buffers
	int32		leFree;
	bigtime_t	aclExhausted;
	bigtime_t	leExhausted;
	struct list	queue;
	int32		queued;
	struct {
		uint16	handle;
		bool	le;
		int32	count;
	}			outstanding[16];
};

static const int32 kMaxQueuedFragments = 1024;
static const bigtime_t kCreditStallTimeout = 10000000;


int32 api_version = B_CUR_DRIVER_API_VERSION;


typedef PortListener<void,
	HCI_MAX_FRAME_SIZE, // Event Body can hold max 255 + 2 header
	24					// Some devices have sent chunks of 24 events(inquiry result)
	> BluetoothRawDataPort;


// Modules references
net_buffer_module_info* gBufferModule = NULL;
struct bluetooth_core_data_module_info* btCoreData = NULL;

static mutex sListLock;
static sem_id sLinkChangeSemaphore;
static DoublyLinkedList<bluetooth_device> sDeviceList;

BluetoothRawDataPort* BluetoothRXPort;

// forward declarations
status_t HciPacketHandler(void* data, int32 code, size_t size);


bluetooth_device*
FindDeviceByID(hci_id hid)
{
	bluetooth_device* device;

	DoublyLinkedList<bluetooth_device>::Iterator iterator
		= sDeviceList.GetIterator();

	while (iterator.HasNext()) {
		device = iterator.Next();
		if (device->index == hid)
			return device;
	}

	return NULL;
}


status_t
PostTransportPacket(hci_id hid, bt_packet_t type, void* data, size_t count)
{
	uint32 code = 0;

	Bluetooth::CodeHandler::SetDevice(&code, hid);
	Bluetooth::CodeHandler::SetProtocol(&code, type);

	return BluetoothRXPort->Trigger(code, data, count);
}


static int32
OutstandingSlot(acl_flow_control* flow, uint16 handle, bool create, bool le)
{
	int32 empty = -1;
	for (int32 i = 0; i < (int32)B_COUNT_OF(flow->outstanding); i++) {
		if (flow->outstanding[i].count > 0
			&& flow->outstanding[i].handle == handle)
			return i;
		if (flow->outstanding[i].count == 0 && empty < 0)
			empty = i;
	}
	if (create && empty >= 0) {
		flow->outstanding[empty].handle = handle;
		flow->outstanding[empty].le = le;
	}
	return create ? empty : -1;
}


static void
ReturnCredits(acl_flow_control* flow, bool le, int32 count)
{
	if (le && flow->leMax > 0) {
		flow->leFree = min_c(flow->leFree + count, flow->leMax);
		flow->leExhausted = 0;
	} else {
		flow->aclFree = min_c(flow->aclFree + count, flow->aclMax);
		flow->aclExhausted = 0;
	}
}


/*!	Sends queued fragments while the controller has buffers for them. A
	fragment whose pool is empty holds back the rest of that pool, keeping
	each link's fragments in order; the other pool may go on.
*/
static void
DrainACL(bluetooth_device* device)
{
	acl_flow_control* flow = device->flow;
	ASSERT_LOCKED_MUTEX(&flow->lock);

	const bigtime_t now = system_time();
	bool aclBlocked = false;
	bool leBlocked = false;
	net_buffer* frame = (net_buffer*)list_get_first_item(&flow->queue);
	while (frame != NULL) {
		net_buffer* next = (net_buffer*)list_get_next_item(&flow->queue,
			frame);
		const uint16 handle = frame->type & 0x0fff;
		const bool le = (frame->type & 0x10000) != 0;
		const bool lePool = le && flow->leMax > 0;
		bool& blocked = lePool ? leBlocked : aclBlocked;
		int32& available = lePool ? flow->leFree : flow->aclFree;
		bigtime_t& exhausted = lePool ? flow->leExhausted : flow->aclExhausted;

		if (!blocked && available <= 0 && exhausted != 0
			&& now - exhausted > kCreditStallTimeout) {
			// No buffer came back for a long time while data waits. Most
			// likely a Number Of Completed Packets event was lost; carry on
			// rather than stall the links for good.
			dprintf("bluetooth: hci %" B_PRId32 " gave no %s buffers back "
				"for %" B_PRId64 " ms, assuming they are free\n",
				device->index, lePool ? "LE" : "ACL",
				(now - exhausted) / 1000);
			for (int32 i = 0; i < (int32)B_COUNT_OF(flow->outstanding);
					i++) {
				if (flow->outstanding[i].count > 0
					&& (flow->outstanding[i].le && flow->leMax > 0)
						== lePool)
					flow->outstanding[i].count = 0;
			}
			available = lePool ? flow->leMax : flow->aclMax;
			exhausted = 0;
		}

		if (blocked || available <= 0) {
			if (available <= 0 && exhausted == 0)
				exhausted = now;
			blocked = true;
			frame = next;
			continue;
		}

		const int32 slot = OutstandingSlot(flow, handle, true, le);
		if (slot < 0) {
			// More links than we track; they are rare enough to just wait.
			blocked = true;
			frame = next;
			continue;
		}

		list_remove_item(&flow->queue, frame);
		flow->queued--;
		available--;
		flow->outstanding[slot].count++;
		frame->type = 0;
		if (device->hooks->SendACL(device->index, frame) != B_OK) {
			gBufferModule->free(frame);
			flow->outstanding[slot].count--;
			available++;
		}
		frame = next;
	}
}


static status_t
QueueACL(bluetooth_device* device, struct list* fragments, bool le)
{
	acl_flow_control* flow = device->flow;
	MutexLocker locker(flow->lock);

	if (flow->aclMax <= 0) {
		// Buffer counts unknown: no flow control.
		while (net_buffer* frame
				= (net_buffer*)list_remove_head_item(fragments)) {
			frame->type = 0;
			if (device->hooks->SendACL(device->index, frame) != B_OK)
				gBufferModule->free(frame);
		}
		return B_OK;
	}

	if (flow->queued >= kMaxQueuedFragments) {
		DrainACL(device);
		if (flow->queued >= kMaxQueuedFragments) {
			// The controller is not keeping up at all. Drop the PDU, as a
			// radio out of range would; the caller's buffer is consumed.
			while (net_buffer* frame
					= (net_buffer*)list_remove_head_item(fragments)) {
				gBufferModule->free(frame);
			}
			return B_OK;
		}
	}

	while (net_buffer* frame = (net_buffer*)list_remove_head_item(fragments)) {
		if (le)
			frame->type |= 0x10000;
		list_add_item(&flow->queue, frame);
		flow->queued++;
	}

	DrainACL(device);
	return B_OK;
}


static void
DropQueuedACL(bluetooth_device* device, uint16 handle, bool all)
{
	acl_flow_control* flow = device->flow;
	net_buffer* frame = (net_buffer*)list_get_first_item(&flow->queue);
	while (frame != NULL) {
		net_buffer* next = (net_buffer*)list_get_next_item(&flow->queue,
			frame);
		if (all || (frame->type & 0x0fff) == handle) {
			list_remove_item(&flow->queue, frame);
			flow->queued--;
			gBufferModule->free(frame);
		}
		frame = next;
	}
}


/*!	Picks up what the stack below L2CAP needs to know from events on their
	way to the bluetooth_server: the controller's ACL packet sizes and
	buffer counts, and which buffers it has given back.
*/
static void
SnoopEvent(bluetooth_device* device, const void* data, size_t size)
{
	const uint8* event = (const uint8*)data;
	if (size < HCI_EVENT_HDR_SIZE || size < (size_t)HCI_EVENT_HDR_SIZE + event[1])
		return;

	acl_flow_control* flow = device->flow;
	if (event[0] == HCI_EVENT_NUM_COMP_PKTS && event[1] >= 1) {
		// Number Of Completed Packets (7.7.19): handle and count pairs.
		const uint8 count = event[2];
		if (event[1] < 1 + 4 * count)
			return;
		MutexLocker locker(flow->lock);
		for (uint8 i = 0; i < count; i++) {
			const uint8* pair = event + 3 + 4 * i;
			const uint16 handle = (pair[0] | (pair[1] << 8)) & 0x0fff;
			int32 completed = pair[2] | (pair[3] << 8);
			const int32 slot = OutstandingSlot(flow, handle, false, false);
			bool le = false;
			if (slot >= 0) {
				completed = min_c(completed, flow->outstanding[slot].count);
				flow->outstanding[slot].count -= completed;
				le = flow->outstanding[slot].le;
			}
			ReturnCredits(flow, le, completed);
		}
		DrainACL(device);
		return;
	}

	if (event[0] == HCI_EVENT_DISCONNECTION_COMPLETE && event[1] >= 4) {
		// The controller drops what it still had for the link and does not
		// report it as completed (Core Vol 4 Part E 4.3).
		if (event[2] != 0)
			return;
		const uint16 handle = (event[3] | (event[4] << 8)) & 0x0fff;
		MutexLocker locker(flow->lock);
		const int32 slot = OutstandingSlot(flow, handle, false, false);
		if (slot >= 0) {
			ReturnCredits(flow, flow->outstanding[slot].le,
				flow->outstanding[slot].count);
			flow->outstanding[slot].count = 0;
		}
		DropQueuedACL(device, handle, false);
		DrainACL(device);
		return;
	}

	if (event[0] != HCI_EVENT_CMD_COMPLETE || event[1] < 4)
		return;

	// Command Complete: ncmd, opcode, then the command's return parameters
	// (Core Vol 4 Part E 7.7.14), which start with a status.
	const uint16 opcode = event[3] | (event[4] << 8);
	const uint8* result = event + 5;
	const size_t resultSize = event[1] - 3;
	if (result[0] != 0)
		return;

	MutexLocker locker(flow->lock);
	if (opcode == PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_RESET)) {
		// The controller forgot every link and buffer.
		for (int32 i = 0; i < (int32)B_COUNT_OF(flow->outstanding); i++)
			flow->outstanding[i].count = 0;
		DropQueuedACL(device, 0, true);
		flow->aclFree = flow->aclMax;
		flow->leFree = flow->leMax;
		flow->aclExhausted = flow->leExhausted = 0;
	} else if (opcode
			== PACK_OPCODE(OGF_INFORMATIONAL_PARAM, OCF_READ_BUFFER_SIZE)
		&& resultSize >= 8) {
		// Read Buffer Size (7.4.5): ACL data packet length, SCO length,
		// ACL and SCO packet counts. Applications ask again whenever they
		// open the device; only a change matters.
		const uint16 aclMtu = result[1] | (result[2] << 8);
		const int32 aclPackets = result[4] | (result[5] << 8);
		if (aclMtu >= 27 && aclMtu != device->mtu) {
			dprintf("bluetooth: hci %" B_PRId32 " ACL packets up to %u "
				"bytes, %" B_PRId32 " buffers\n", device->index, aclMtu,
				aclPackets);
			device->mtu = aclMtu;
		}
		if (aclPackets > 0 && aclPackets != flow->aclMax) {
			if (flow->aclMax <= 0)
				flow->aclFree = aclPackets;
			else
				flow->aclFree += aclPackets - flow->aclMax;
			flow->aclMax = aclPackets;
			DrainACL(device);
		}
	} else if (opcode == PACK_OPCODE(OGF_LE_CONTROL, OCF_LE_READ_BUFFER_SIZE)
		&& resultSize >= 4) {
		// LE Read Buffer Size (7.8.2): a length of 0 means LE links use the
		// ACL buffers.
		const uint16 leMtu = result[1] | (result[2] << 8);
		const int32 lePackets = leMtu != 0 ? result[3] : 0;
		if (leMtu != device->leMtu) {
			dprintf("bluetooth: hci %" B_PRId32 " LE packets up to %u "
				"bytes, %" B_PRId32 " buffers\n", device->index, leMtu,
				lePackets);
			device->leMtu = leMtu;
		}
		if (lePackets != flow->leMax) {
			flow->leFree += lePackets - flow->leMax;
			flow->leMax = lePackets;
			DrainACL(device);
		}
	}
}


status_t
Assemble(bluetooth_device* bluetoothDevice, bt_packet_t type, void* data,
	size_t count)
{
	net_buffer* nbuf = bluetoothDevice->fBuffersRx[type];

	size_t currentPacketLen = 0;

	while (count) {

		if (nbuf == NULL) {
			// new buffer incoming
			switch (type) {
				case BT_EVENT:
					if (count >= HCI_EVENT_HDR_SIZE) {
						struct hci_event_header* headerPacket
							= (struct hci_event_header*)data;
						bluetoothDevice->fExpectedPacketSize[type]
							= HCI_EVENT_HDR_SIZE + headerPacket->elen;

						if (count >= bluetoothDevice->fExpectedPacketSize[type]) {
							// the whole packet is here so it can be already posted.
							TRACE("%s: EVENT posted in HCI\n", __func__);
							SnoopEvent(bluetoothDevice, data,
								bluetoothDevice->fExpectedPacketSize[type]);
							btCoreData->PostEvent(bluetoothDevice, data,
								bluetoothDevice->fExpectedPacketSize[type]);

						} else {
							nbuf = gBufferModule->create(
								bluetoothDevice->fExpectedPacketSize[type]);
							bluetoothDevice->fBuffersRx[type] = nbuf;

							nbuf->protocol = type;
						}

					} else {
						panic("EVENT frame corrupted\n");
						return EILSEQ;
					}
					break;

				case BT_ACL:
					if (count >= HCI_ACL_HDR_SIZE) {
						struct hci_acl_header* headerPkt = (struct hci_acl_header*)data;

						bluetoothDevice->fExpectedPacketSize[type] = HCI_ACL_HDR_SIZE
							+ B_LENDIAN_TO_HOST_INT16(headerPkt->alen);

						// Create the buffer -> TODO: this allocation can fail
						nbuf = gBufferModule->create(
							bluetoothDevice->fExpectedPacketSize[type]);
						bluetoothDevice->fBuffersRx[type] = nbuf;

						nbuf->protocol = type;
					} else {
						panic("ACL frame corrupted\n");
						return EILSEQ;
					}
					break;

				case BT_SCO:

					break;

				default:
					panic("unknown packet type in assembly");
					break;
			}

			currentPacketLen = bluetoothDevice->fExpectedPacketSize[type];

		} else {
			// Continuation of a packet
			currentPacketLen = bluetoothDevice->fExpectedPacketSize[type] - nbuf->size;
		}
		if (nbuf != NULL) {
			currentPacketLen = min_c(currentPacketLen, count);

			gBufferModule->append(nbuf, data, currentPacketLen);

			if ((bluetoothDevice->fExpectedPacketSize[type] - nbuf->size) == 0) {

				switch (nbuf->protocol) {
					case BT_EVENT:
						panic("need to send full buffer to btdatacore!\n");
						btCoreData->PostEvent(bluetoothDevice, data,
							bluetoothDevice->fExpectedPacketSize[type]);

						break;
					case BT_ACL:
						// TODO: device descriptor has been fetched better not
						// pass id again
						TRACE("%s: ACL parsed in ACL!\n", __func__);
						AclAssembly(nbuf, bluetoothDevice->index);
						break;
					default:

						break;
				}

				bluetoothDevice->fBuffersRx[type] = nbuf = NULL;
				bluetoothDevice->fExpectedPacketSize[type] = 0;
			} else {
				if (type == BT_ACL) {
					TRACE("%s: ACL Packet not filled size %" B_PRIu32
						" expected=%" B_PRIuSIZE "\n", __func__, nbuf->size,
						bluetoothDevice->fExpectedPacketSize[type]);
				}
			}

		}
		// in case in the pipe there is info about the next buffer
		count -= currentPacketLen;
		data = (void*)((uint8*)data + currentPacketLen);
	}

	return B_OK;
}


status_t
HciPacketHandler(void* data, int32 code, size_t size)
{
	hci_id deviceId = Bluetooth::CodeHandler::Device(code);

	bluetooth_device* bluetoothDevice = FindDeviceByID(deviceId);

	TRACE("%s: to assemble %" B_PRIuSIZE " bytes of 0x%" B_PRIx32 "\n",
		__func__, size, deviceId);

	if (bluetoothDevice != NULL) {
		return Assemble(bluetoothDevice, Bluetooth::CodeHandler::Protocol(code),
			data, size);
	} else {
		ERROR("%s: Device 0x%" B_PRIx32 " could not be matched\n", __func__,
			deviceId);
	}

	return B_ERROR;
}


//	#pragma mark -


status_t
RegisterDriver(bt_hci_transport_hooks* hooks, bluetooth_device** _device)
{

	bluetooth_device* device = new (std::nothrow) bluetooth_device;
	if (device == NULL)
		return B_NO_MEMORY;

	for (int index = 0; index < HCI_NUM_PACKET_TYPES; index++) {
		device->fBuffersRx[index] = NULL;
		device->fExpectedPacketSize[index] = 0;
	}

	device->info = NULL; // not yet used
	device->hooks = hooks;
	device->supportedPacketTypes = (HCI_DM1 | HCI_DH1 | HCI_HV1);
	device->linkMode = (HCI_LM_ACCEPT);
	device->mtu = L2CAP_MTU_MINIMUM;
		// until the bluetooth_server's Read Buffer Size tells the real one
	device->leMtu = 0;

	device->flow = new (std::nothrow) acl_flow_control;
	if (device->flow == NULL) {
		delete device;
		return B_NO_MEMORY;
	}
	mutex_init(&device->flow->lock, "bluetooth acl flow");
	device->flow->aclMax = device->flow->aclFree = -1;
	device->flow->leMax = device->flow->leFree = 0;
	device->flow->aclExhausted = device->flow->leExhausted = 0;
	list_init_etc(&device->flow->queue, offsetof(net_buffer, link));
	device->flow->queued = 0;
	memset(device->flow->outstanding, 0, sizeof(device->flow->outstanding));

	MutexLocker _(&sListLock);

	if (sDeviceList.IsEmpty())
		device->index = HCI_DEVICE_INDEX_OFFSET; // REVIEW: dev index
	else {
		device->index = (sDeviceList.Tail())->index + 1; // REVIEW!
		TRACE("%s: List not empty\n", __func__);
	}

	sDeviceList.Add(device);

	TRACE("%s: Device %" B_PRIx32 "\n", __func__, device->index);

	*_device = device;

	return B_OK;
}


status_t
UnregisterDriver(hci_id id)
{
	bluetooth_device* device = FindDeviceByID(id);

	if (device == NULL)
		return B_ERROR;

	if (device->GetDoublyLinkedListLink()->next != NULL
		|| device->GetDoublyLinkedListLink()->previous != NULL
		|| device == sDeviceList.Head())
		sDeviceList.Remove(device);

	if (device->flow != NULL) {
		mutex_lock(&device->flow->lock);
		DropQueuedACL(device, 0, true);
		mutex_destroy(&device->flow->lock);
		delete device->flow;
	}
	delete device;

	return B_OK;
}


status_t
PostCommand(hci_id hciId, net_buffer* buffer)
{
	if (buffer == NULL)
		panic("passing null buffer");

	bluetooth_device* device = FindDeviceByID(hciId);
	if (device == NULL) {
		ERROR("%s: No device 0x%" B_PRIx32 "\n", __func__, hciId);
		return B_ERROR;
	}

	buffer->protocol = BT_COMMAND;

	return device->hooks->SendCommand(hciId, buffer);
}


// PostACL
/*!	Transport drivers hand ACL packets to the hardware as one block. Split
	and prepended buffers need not be one; copy those into a fresh buffer.
*/
static net_buffer*
ContiguousFrame(net_buffer* frame)
{
	void* data;
	if (gBufferModule->direct_access(frame, 0, frame->size, &data) == B_OK)
		return frame;

	net_buffer* copy = gBufferModule->create(0);
	if (copy == NULL)
		return NULL;
	if (gBufferModule->append_size(copy, frame->size, &data) != B_OK
		|| data == NULL
		|| gBufferModule->read(frame, 0, data, frame->size) != B_OK) {
		gBufferModule->free(copy);
		return NULL;
	}
	copy->protocol = frame->protocol;
	copy->type = frame->type;
	return copy;
}


status_t
PostACL(hci_id hciId, net_buffer* buffer)
{
	uint8 flag = HCI_ACL_PACKET_START;

	if (buffer == NULL)
		panic("passing null buffer");

	uint16 handle = buffer->type; // TODO: CodeHandler

	bluetooth_device* device = FindDeviceByID(hciId);

	if (device == NULL) {
		ERROR("%s: No device 0x%" B_PRIx32 "\n", __func__, hciId);
		return B_ERROR;
	}

	TRACE("%s: index 0x%" B_PRIx32 " try to send bt packet of %" B_PRIu32
		" bytes (flags 0x%" B_PRIx32 "):\n", __func__, device->index,
		buffer->size, buffer->flags);

	// Automatically flushable packets are not allowed on an LE-U link (Core
	// Vol 4 Part E 5.4.2); controllers may drop them. Start LE PDUs with the
	// non-flushable boundary flag instead.
	HciConnection* connection = btCoreData->ConnectionByHandle(handle, hciId);
	uint16 mtu = device->mtu;
	if (connection != NULL && connection->isLE) {
		flag = HCI_ACL_PACKET_START_NON_FLUSHABLE;
		if (device->leMtu != 0)
			mtu = device->leMtu;
	}

	// Cut the PDU into fragments first, so that a failure leaves nothing
	// half sent. The last fragment is the caller's buffer itself.
	struct list fragments;
	list_init_etc(&fragments, offsetof(net_buffer, link));
	status_t status = B_OK;
	bool lastCopied = false;
	while (true) {
		const bool last = buffer->size <= mtu;
		net_buffer* frame = buffer;
		if (!last) {
			frame = gBufferModule->split(buffer, mtu);
			if (frame == NULL) {
				status = B_NO_MEMORY;
				break;
			}
		}

		{
			NetBufferPrepend<struct hci_acl_header> header(frame);
			status = header.Status();
			if (status == B_OK) {
				header->handle = B_HOST_TO_LENDIAN_INT16(
					pack_acl_handle_flags(handle, flag, 0));
				header->alen = B_HOST_TO_LENDIAN_INT16(
					frame->size - sizeof(struct hci_acl_header));
			}
		}
		net_buffer* contiguous = NULL;
		if (status == B_OK) {
			frame->protocol = BT_ACL;
			frame->type = handle;
			contiguous = ContiguousFrame(frame);
			if (contiguous == NULL)
				status = B_NO_MEMORY;
		}
		if (status != B_OK) {
			if (!last)
				gBufferModule->free(frame);
			break;
		}
		if (contiguous != frame) {
			if (last)
				lastCopied = true;
			else
				gBufferModule->free(frame);
		}

		list_add_item(&fragments, contiguous);
		flag = HCI_ACL_PACKET_FRAGMENT;
		if (last)
			break;
	}

	if (status != B_OK) {
		// None of these is the caller's buffer, which the caller frees.
		while (net_buffer* frame
				= (net_buffer*)list_remove_head_item(&fragments)) {
			gBufferModule->free(frame);
		}
		return status;
	}
	if (lastCopied)
		gBufferModule->free(buffer);

	return QueueACL(device, &fragments,
		connection != NULL && connection->isLE);
}


status_t
PostSCO(hci_id hciId, net_buffer* buffer)
{
	bluetooth_device* device = FindDeviceByID(hciId);

	if (device == NULL) {
		ERROR("%s: No device 0x%" B_PRIx32 "\n", __func__, hciId);
		return B_ERROR;
	}

	buffer->protocol = BT_SCO;

	return device->hooks->SendSCO(hciId, buffer);
}


status_t
PostESCO(hci_id hciId, net_buffer* buffer)
{
	return B_ERROR;
}


static int
dump_bluetooth_devices(int argc, char** argv)
{
	bluetooth_device*	device;

	DoublyLinkedList<bluetooth_device>::Iterator iterator
		= sDeviceList.GetIterator();

	while (iterator.HasNext()) {
		device = iterator.Next();
		kprintf("\tindex=%" B_PRIx32 " @%p hooks=%p\n", device->index,
			device, device->hooks);
	}

	return 0;
}


static status_t
bluetooth_std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
		{
			status_t status;

			status = get_module(NET_BUFFER_MODULE_NAME,
				(module_info**)&gBufferModule);

			if (status < B_OK) {
				panic("no way Dude we need that!");
				return status;
			}

			status = get_module(BT_CORE_DATA_MODULE_NAME,
				(module_info**)&btCoreData);
			if (status < B_OK) {
				ERROR("%s: problem getting bt core data module\n", __func__);
				return status;
			}

			new (&sDeviceList) DoublyLinkedList<bluetooth_device>;
				// static C++ objects are not initialized in the module startup

			BluetoothRXPort = new BluetoothRawDataPort(BT_RX_PORT_NAME,
				(BluetoothRawDataPort::port_listener_func)&HciPacketHandler);

			if (BluetoothRXPort->Launch() != B_OK) {
				ERROR("%s: RX thread creation failed!\n", __func__);
				// we Cannot do much here ... avoid registering
			} else {
				TRACE("%s: RX thread launched!\n", __func__);
			}

			sLinkChangeSemaphore = create_sem(0, "bt sem");
			if (sLinkChangeSemaphore < B_OK) {
				put_module(NET_STACK_MODULE_NAME);
				ERROR("%s: Link change semaphore failed\n", __func__);
				return sLinkChangeSemaphore;
			}

			mutex_init(&sListLock, "bluetooth devices");

			// status = InitializeAclConnectionThread();
			ERROR("%s: Connection Thread error\n", __func__);

			add_debugger_command("btLocalDevices", &dump_bluetooth_devices,
				"Lists Bluetooth LocalDevices registered in the Stack");

			return B_OK;
		}

		case B_MODULE_UNINIT:
		{
			delete_sem(sLinkChangeSemaphore);

			mutex_destroy(&sListLock);
			put_module(NET_BUFFER_MODULE_NAME);
			put_module(NET_STACK_MODULE_NAME);
			put_module(BT_CORE_DATA_MODULE_NAME);
			remove_debugger_command("btLocalDevices", &dump_bluetooth_devices);
			// status_t status = QuitAclConnectionThread();

			return B_OK;
		}

		default:
			return B_ERROR;
	}
}


bt_hci_module_info sBluetoothModule = {
	{
		BT_HCI_MODULE_NAME, 
		B_KEEP_LOADED, 
		bluetooth_std_ops
	}, 
	RegisterDriver, 
	UnregisterDriver,
	FindDeviceByID, 
	PostTransportPacket, 
	PostCommand, 
	PostACL, 
	PostSCO, 
	PostESCO
};


module_info* modules[] = {
	(module_info*)&sBluetoothModule,
	NULL
};

