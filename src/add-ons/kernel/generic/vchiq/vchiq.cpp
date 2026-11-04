/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * The protocol and the layout of the shared memory are those of Broadcom's
 * VCHIQ implementation (vchiq_core, Copyright 2010-2012 Broadcom, BSD
 * 3-clause); this is a new implementation of the ARM side for Haiku.
 */

/*	VCHIQ: messages to and from the services of the Raspberry Pi's
	VideoCore firmware.

	Both sides share a block of memory cut into 4 KiB slots. Slot zero holds
	each side's state; each side owns half of the remaining slots and writes
	its messages into them one after the other (a header with the ports and
	the size, then the data, padded to 8 bytes). The reader hands a slot
	back once it is through with it. "Remote events" in the shared state
	stand for semaphores; ringing the doorbell register raises the other
	side's interrupt when it sleeps on one. Bulk transfers send only an
	address: the firmware copies from or to our memory itself.

	Left out of this implementation, as nothing here needs it: services
	opened by the firmware, synchronous services, suspend and resume, and
	message quotas per service. */


#include <vchiq.h>

#include <stdlib.h>
#include <string.h>

#include <bus/FDT.h>
#include <device_manager.h>
#include <KernelExport.h>

#include <kernel.h>
#include <lock.h>
#include <team.h>
#include <util/AutoLock.h>
#include <vm/vm.h>

#include <rpi_firmware.h>


#define ERROR(x...)	dprintf("vchiq: " x)
#define INFO(x...)	dprintf("vchiq: " x)
//#define TRACE_VCHIQ
#ifdef TRACE_VCHIQ
#	define TRACE(x...)	dprintf("vchiq: " x)
#else
#	define TRACE(x...)	do {} while (false)
#endif

#define TAG_VCHIQ_INIT			0x00048010

// the doorbell registers, as 32 bit words from the device's base
#define BELL0					0
#define BELL2					2
#define  BELL_ACTIVE			0x4

// The VideoCore sees the ARM's first gigabyte at this bus address.
#define VC_BUS_OFFSET			0xc0000000

#define VCHIQ_MAGIC				VCHIQ_FOURCC('V', 'C', 'H', 'I')
#define VCHIQ_VERSION			8
#define VCHIQ_VERSION_MIN		3

#define SLOT_SIZE				4096
#define SLOT_MASK				(SLOT_SIZE - 1)
#define MAX_SLOTS				128
#define MAX_SLOTS_PER_SIDE		64
#define SLOT_QUEUE_MASK			(MAX_SLOTS_PER_SIDE - 1)
#define SLOT_ZERO_SLOTS			1
#define TOTAL_SLOTS				(SLOT_ZERO_SLOTS + 2 * 32)

#define CACHE_LINE_SIZE			64
#define FRAGMENT_SIZE			(2 * CACHE_LINE_SIZE)
#define MAX_FRAGMENTS			64

#define MAX_SERVICES			MAX_FRAGMENTS
	// a service's port is also the number of its fragment buffer

#define MSG_PADDING				0
#define MSG_CONNECT				1
#define MSG_OPEN				2
#define MSG_OPENACK				3
#define MSG_CLOSE				4
#define MSG_DATA				5
#define MSG_BULK_RX				6
#define MSG_BULK_TX				7
#define MSG_BULK_RX_DONE		8
#define MSG_BULK_TX_DONE		9
#define MSG_PAUSE				10
#define MSG_RESUME				11
#define MSG_REMOTE_USE			12
#define MSG_REMOTE_RELEASE		13
#define MSG_REMOTE_USE_ACTIVE	14

#define MAKE_MSG(type, source, destination) \
	(((type) << 24) | ((source) << 12) | (destination))
#define MSG_TYPE(id)			((uint32)(id) >> 24)
#define MSG_SOURCE(id)			(((uint32)(id) >> 12) & 0xfff)
#define MSG_DESTINATION(id)		((uint32)(id) & 0xfff)

#define PAGELIST_WRITE			0
#define PAGELIST_READ			1
#define PAGELIST_READ_WITH_FRAGMENTS	2
// An entry of a page list is the address of a run of pages with the number
// of pages - 1 in its low bits: a bus address and 12 bits, or on the BCM2711
// the physical address shifted right by 4 (it has 36 bits) and 8 bits.
#define PAGELIST_MAX_RUN		4096
#define PAGELIST_MAX_RUN_36		256

#define DEBUG_MAX				11

#define OPEN_TIMEOUT			5000000LL
#define BULK_TIMEOUT			10000000LL
#define SLOT_TIMEOUT			10000000LL


struct message_header {
	int32	id;
	uint32	size;
	uint8	data[0];
};

struct remote_event {
	int32	armed;
	int32	fired;
	uint32	_unused;
};

struct shared_state {
	int32			initialised;
	int32			slot_first;
	int32			slot_last;
	int32			slot_sync;
	remote_event	trigger;
	int32			tx_pos;
	remote_event	recycle;
	int32			slot_queue_recycle;
	remote_event	sync_trigger;
	remote_event	sync_release;
	int32			slot_queue[MAX_SLOTS_PER_SIDE];
	int32			debug[DEBUG_MAX];
};

struct slot_info {
	int16	use_count;
	int16	release_count;
};

struct slot_zero {
	int32			magic;
	int16			version;
	int16			version_min;
	int32			slot_zero_size;
	int32			slot_size;
	int32			max_slots;
	int32			max_slots_per_side;
	int32			platform_data[2];
	shared_state	master;
	shared_state	slave;
	slot_info		slots[MAX_SLOTS];
};

struct open_payload {
	uint32	fourcc;
	int32	client_id;
	int16	version;
	int16	version_min;
};

struct pagelist {
	uint32	length;
	uint16	type;
	uint16	offset;
	uint32	addrs[1];
};

enum {
	SERVICE_OPENING,
	SERVICE_OPEN,
	SERVICE_CLOSE_SENT,
	SERVICE_CLOSED
};

enum {
	BULK_TRANSMIT,
	BULK_RECEIVE
};

struct vchiq_service {
	uint32				fourcc;
	uint32				localPort;
	uint32				remotePort;
	int32				state;
	int16				version;
	int16				minVersion;
	int16				peerVersion;
	vchiq_service_hook	hook;
	void*				cookie;
	sem_id				stateSem;

	// one transfer per direction at a time, each with memory of its own
	struct {
		mutex			lock;
		sem_id			sem;
		int32			actual;
		bool			waiting;
		bool			broken;
		// the page list, there from the start
		area_id			listArea;
		pagelist*		list;
		phys_addr_t		listAddress;
		// the data, as large as the largest transfer so far
		area_id			area;
		uint8*			buffer;
		size_t			size;
		phys_addr_t*	pages;
	} bulk[2];
};


static device_manager_info* sDeviceManager;
static rpi_firmware_module_info* sFirmware;

static status_t sInitStatus = B_NO_INIT;
static area_id sRegistersArea = -1;
static volatile uint32* sRegisters;
static uint32 sInterrupt;
static area_id sSlotArea = -1;
static uint8* sSlotData;
static phys_addr_t sSlotAddress;
static uint8* sFragments;
static bool s36BitAddresses;

static volatile shared_state* sLocal;
static volatile shared_state* sRemote;

static sem_id sTriggerSem = -1;
static sem_id sRecycleSem = -1;
static sem_id sSlotAvailableSem = -1;
static sem_id sConnectSem = -1;

static mutex sInitLock = MUTEX_INITIALIZER("vchiq init");
static mutex sSlotLock = MUTEX_INITIALIZER("vchiq slots");
	// writing messages
static mutex sRecycleLock = MUTEX_INITIALIZER("vchiq recycle");
static mutex sServiceLock = MUTEX_INITIALIZER("vchiq services");
	// the table; held while a service's hook runs

static uint8* sTxData;
static int32 sLocalTxPos;
static int32 sSlotQueueAvailable;
static uint8* sRxData;
static slot_info* sRxInfo;
static int32 sRxPos;
static slot_info sSlotInfo[MAX_SLOTS];

static vchiq_service* sServices[MAX_SERVICES];
static uint32 sNextPort;


static inline void
barrier()
{
#ifdef __aarch64__
	asm volatile("dsb sy" : : : "memory");
#else
	memory_full_barrier();
#endif
}


/*!	Writes the CPU cache's content of the range to memory and drops it: the
	firmware reads and writes the memory behind the cache's back.
*/
static void
flush_cache(void* address, size_t size)
{
#ifdef __aarch64__
	addr_t end = (addr_t)address + size;
	for (addr_t line = (addr_t)address & ~(addr_t)(CACHE_LINE_SIZE - 1);
			line < end; line += CACHE_LINE_SIZE) {
		asm volatile("dc civac, %0" : : "r" (line) : "memory");
	}
	asm volatile("dsb sy" : : : "memory");
#endif
}


static inline size_t
calc_stride(size_t size)
{
	return (size + 2 * sizeof(message_header) - 1)
		& ~(sizeof(message_header) - 1);
}


static inline uint8*
slot_data(int32 index)
{
	return sSlotData + (size_t)index * SLOT_SIZE;
}


//	#pragma mark - remote events


static void
remote_event_wait(volatile remote_event* event, sem_id sem)
{
	while (event->fired == 0) {
		event->armed = 1;
		barrier();
		if (event->fired != 0)
			break;
		acquire_sem(sem);
	}

	event->armed = 0;
	barrier();
	event->fired = 0;
}


static void
remote_event_signal(volatile remote_event* event)
{
	barrier();
	event->fired = 1;
	barrier();

	if (event->armed != 0)
		sRegisters[BELL2] = 0;
}


static bool
remote_event_poll(volatile remote_event* event, sem_id sem)
{
	if (event->fired == 0 || event->armed == 0)
		return false;

	event->armed = 0;
	release_sem_etc(sem, 1, B_DO_NOT_RESCHEDULE);
	return true;
}


static int32
doorbell_interrupt(void* data)
{
	// reading clears it
	if ((sRegisters[BELL0] & BELL_ACTIVE) == 0)
		return B_UNHANDLED_INTERRUPT;

	bool wake = remote_event_poll(&sLocal->trigger, sTriggerSem);
	wake |= remote_event_poll(&sLocal->recycle, sRecycleSem);
	return wake ? B_INVOKE_SCHEDULER : B_HANDLED_INTERRUPT;
}


//	#pragma mark - sending


/*!	Room for a message of \a space bytes (a stride) in our slots. The slot
	lock must be held.
*/
static message_header*
reserve_space(size_t space)
{
	int32 txPos = sLocalTxPos;
	size_t slotSpace = SLOT_SIZE - (txPos & SLOT_MASK);

	if (space > slotSpace) {
		// fill the rest of the slot
		message_header* header
			= (message_header*)(sTxData + (txPos & SLOT_MASK));
		header->id = MAKE_MSG(MSG_PADDING, 0, 0);
		header->size = slotSpace - sizeof(message_header);
		txPos += slotSpace;
	}

	if ((txPos & SLOT_MASK) == 0) {
		// the next slot
		if (acquire_sem_etc(sSlotAvailableSem, 1, B_RELATIVE_TIMEOUT, 0)
				!= B_OK) {
			// none free: have the firmware read what there is
			sLocalTxPos = txPos;
			sLocal->tx_pos = txPos;
			remote_event_signal(&sRemote->trigger);

			if (acquire_sem_etc(sSlotAvailableSem, 1, B_RELATIVE_TIMEOUT,
					SLOT_TIMEOUT) != B_OK) {
				ERROR("no free slot, the firmware does not read\n");
				return NULL;
			}
		}

		int32 index = sLocal->slot_queue[(txPos / SLOT_SIZE)
			& SLOT_QUEUE_MASK];
		sTxData = slot_data(index);
	}

	sLocalTxPos = txPos + space;
	return (message_header*)(sTxData + (txPos & SLOT_MASK));
}


static status_t
send_message(int32 id, const void* data, size_t size, bool userData)
{
	if (size > VCHIQ_MAX_MESSAGE_SIZE)
		return B_BAD_VALUE;

	MutexLocker locker(sSlotLock);

	message_header* header = reserve_space(calc_stride(size));
	if (header == NULL)
		return B_TIMED_OUT;

	status_t status = B_OK;
	if (size > 0) {
		if (userData)
			status = user_memcpy(header->data, data, size);
		else
			memcpy(header->data, data, size);
	}

	// the space is taken either way
	header->id = status == B_OK ? id : MAKE_MSG(MSG_PADDING, 0, 0);
	header->size = size;
	barrier();
	sLocal->tx_pos = sLocalTxPos;
	barrier();

	locker.Unlock();
	remote_event_signal(&sRemote->trigger);
	return status;
}


//	#pragma mark - receiving


/*!	Hands a slot of the firmware's back to it once all of its messages are
	dealt with.
*/
static void
release_slot(slot_info* info)
{
	MutexLocker locker(sRecycleLock);

	if (++info->release_count != info->use_count)
		return;

	barrier();
	int32 recycle = sRemote->slot_queue_recycle;
	sRemote->slot_queue[recycle & SLOT_QUEUE_MASK] = info - sSlotInfo;
	sRemote->slot_queue_recycle = recycle + 1;
	remote_event_signal(&sRemote->recycle);
}


static void
finish_bulk(vchiq_service* service, int direction, int32 actual)
{
	if (!service->bulk[direction].waiting)
		return;

	service->bulk[direction].actual = actual;
	service->bulk[direction].waiting = false;
	release_sem(service->bulk[direction].sem);
}


static void
parse_message(message_header* header)
{
	int32 id = header->id;
	uint32 size = header->size;
	uint32 type = MSG_TYPE(id);
	uint32 localPort = MSG_DESTINATION(id);
	uint32 remotePort = MSG_SOURCE(id);

	TRACE("received type %" B_PRIu32 " %" B_PRIu32 "->%" B_PRIu32 " size %"
		B_PRIu32 "\n", type, remotePort, localPort, size);

	switch (type) {
		case MSG_PADDING:
		case MSG_REMOTE_RELEASE:
		case MSG_REMOTE_USE_ACTIVE:
			return;

		case MSG_CONNECT:
			release_sem(sConnectSem);
			return;

		case MSG_OPEN:
			// the firmware opens a service of ours: there is none
			send_message(MAKE_MSG(MSG_CLOSE, 0, remotePort), NULL, 0, false);
			return;

		case MSG_REMOTE_USE:
			// the firmware wants to know that we are awake
			send_message(MAKE_MSG(MSG_REMOTE_USE_ACTIVE, 0, 0), NULL, 0,
				false);
			return;

		case MSG_PAUSE:
		case MSG_RESUME:
		case MSG_BULK_RX:
		case MSG_BULK_TX:
			ERROR("unexpected message type %" B_PRIu32 "\n", type);
			return;
	}

	MutexLocker locker(sServiceLock);

	vchiq_service* service = localPort < MAX_SERVICES
		? sServices[localPort] : NULL;
	if (service == NULL) {
		ERROR("message type %" B_PRIu32 " for port %" B_PRIu32
			", which is not open\n", type, localPort);
		return;
	}

	switch (type) {
		case MSG_OPENACK:
			if (service->state != SERVICE_OPENING)
				break;
			if (size >= sizeof(int16))
				service->peerVersion = *(int16*)header->data;
			service->remotePort = remotePort;
			service->state = SERVICE_OPEN;
			release_sem(service->stateSem);
			break;

		case MSG_CLOSE:
		{
			int32 state = service->state;
			if (state == SERVICE_CLOSED)
				break;

			service->state = SERVICE_CLOSED;
			finish_bulk(service, BULK_TRANSMIT, -1);
			finish_bulk(service, BULK_RECEIVE, -1);

			if (state == SERVICE_OPEN) {
				// the firmware's doing: answer, tell the owner
				send_message(MAKE_MSG(MSG_CLOSE, service->localPort,
					service->remotePort), NULL, 0, false);
				service->hook(service->cookie, VCHIQ_EVENT_CLOSED, NULL, 0);
			} else
				release_sem(service->stateSem);
			break;
		}

		case MSG_DATA:
			if (service->state == SERVICE_OPEN
				&& service->remotePort == remotePort) {
				service->hook(service->cookie, VCHIQ_EVENT_MESSAGE,
					header->data, size);
			}
			break;

		case MSG_BULK_RX_DONE:
		case MSG_BULK_TX_DONE:
			if (size >= sizeof(int32)) {
				finish_bulk(service, type == MSG_BULK_RX_DONE
					? BULK_RECEIVE : BULK_TRANSMIT, *(int32*)header->data);
			}
			break;

		default:
			ERROR("unknown message type %" B_PRIu32 "\n", type);
			break;
	}
}


static void
parse_rx_slots()
{
	barrier();
	int32 txPos = sRemote->tx_pos;

	while (sRxPos != txPos) {
		if (sRxData == NULL) {
			int32 index = sRemote->slot_queue[(sRxPos / SLOT_SIZE)
				& SLOT_QUEUE_MASK];
			if (index < 0 || index >= MAX_SLOTS) {
				ERROR("slot %" B_PRId32 " in the firmware's queue\n", index);
				return;
			}
			sRxData = slot_data(index);
			sRxInfo = &sSlotInfo[index];
			// one use for the slot itself, released at its end
			sRxInfo->use_count = 1;
			sRxInfo->release_count = 0;
		}

		message_header* header
			= (message_header*)(sRxData + (sRxPos & SLOT_MASK));
		uint32 size = header->size;
		if ((sRxPos & SLOT_MASK) + calc_stride(size) > SLOT_SIZE) {
			ERROR("message of %" B_PRIu32 " bytes at %#" B_PRIx32
				" leaves its slot; receiving stops\n", size, sRxPos);
			sRxPos = txPos;
			return;
		}

		parse_message(header);
		sRxPos += calc_stride(size);

		if ((sRxPos & SLOT_MASK) == 0) {
			release_slot(sRxInfo);
			sRxData = NULL;
		}
	}
}


static int32
slot_handler(void* data)
{
	while (true) {
		remote_event_wait(&sLocal->trigger, sTriggerSem);
		parse_rx_slots();
	}
	return 0;
}


/*!	Slots of ours that the firmware has read become free again. */
static int32
recycle_handler(void* data)
{
	while (true) {
		remote_event_wait(&sLocal->recycle, sRecycleSem);

		barrier();
		while (sSlotQueueAvailable != sLocal->slot_queue_recycle) {
			sSlotQueueAvailable++;
			release_sem(sSlotAvailableSem);
		}
	}
	return 0;
}


//	#pragma mark - services


static status_t allocate_page_list(vchiq_service* service, int direction);


static void
delete_service(vchiq_service* service)
{
	for (int i = 0; i < 2; i++) {
		if (service->bulk[i].area >= 0)
			delete_area(service->bulk[i].area);
		if (service->bulk[i].listArea >= 0)
			delete_area(service->bulk[i].listArea);
		free(service->bulk[i].pages);
		delete_sem(service->bulk[i].sem);
		mutex_destroy(&service->bulk[i].lock);
	}
	delete_sem(service->stateSem);
	free(service);
}


static status_t init();


/*!	Starts the channel on first use. Not in B_MODULE_INIT: init() asks the
	device manager for the doorbell's node, and module initialisation runs
	under the modules lock, which the device manager takes while it probes with
	its own lock held. bcm2835_audio, loaded on every arm64 board by a scan of
	/dev/audio, then deadlocked with a concurrent probe: no network, no USB
	input (ROCK 5, 2026-11-04).
*/
static status_t
vchiq_init_check()
{
	MutexLocker _(sInitLock);

	// Once the firmware knows the slots they stay: the module is never
	// unloaded, and a failed start is not tried again.
	if (sInitStatus == B_NO_INIT) {
		sInitStatus = init();
		if (sInitStatus != B_OK)
			ERROR("not started: %s\n", strerror(sInitStatus));
	}
	return sInitStatus;
}


static status_t
vchiq_open_service(uint32 fourcc, int16 version, int16 minVersion,
	vchiq_service_hook hook, void* cookie, vchiq_service** _service)
{
	if (sInitStatus != B_OK)
		return sInitStatus;
	if (hook == NULL)
		return B_BAD_VALUE;

	vchiq_service* service = (vchiq_service*)calloc(1, sizeof(vchiq_service));
	if (service == NULL)
		return B_NO_MEMORY;

	service->fourcc = fourcc;
	service->version = version;
	service->minVersion = minVersion;
	service->hook = hook;
	service->cookie = cookie;
	service->state = SERVICE_OPENING;
	service->stateSem = create_sem(0, "vchiq service state");
	status_t status = service->stateSem < 0 ? B_NO_MORE_SEMS : B_OK;
	for (int i = 0; i < 2; i++) {
		service->bulk[i].area = -1;
		service->bulk[i].listArea = -1;
		mutex_init(&service->bulk[i].lock, "vchiq bulk");
		service->bulk[i].sem = create_sem(0, "vchiq bulk");
		if (service->bulk[i].sem < 0)
			status = B_NO_MORE_SEMS;
	}
	for (int i = 0; i < 2 && status == B_OK; i++)
		status = allocate_page_list(service, i);

	bool registered = false;
	if (status == B_OK) {
		MutexLocker locker(sServiceLock);
		status = B_NO_MORE_PORTS;
		for (uint32 i = 0; i < MAX_SERVICES; i++) {
			// not the port closed last: late messages for it may follow
			uint32 port = (sNextPort + i) % MAX_SERVICES;
			if (sServices[port] == NULL) {
				service->localPort = port;
				sServices[port] = service;
				sNextPort = port + 1;
				registered = true;
				status = B_OK;
				break;
			}
		}
	}

	if (status == B_OK) {
		// the firmware tells its clients apart by this
		open_payload payload = { fourcc, team_get_current_team_id(), version,
			minVersion };
		status = send_message(MAKE_MSG(MSG_OPEN, service->localPort, 0),
			&payload, sizeof(payload), false);
	}
	if (status == B_OK) {
		status = acquire_sem_etc(service->stateSem, 1, B_RELATIVE_TIMEOUT,
			OPEN_TIMEOUT);
	}
	if (status == B_OK && service->state != SERVICE_OPEN) {
		// the firmware has no such service, or not in this version
		status = B_ENTRY_NOT_FOUND;
	}

	if (status != B_OK) {
		if (registered) {
			MutexLocker locker(sServiceLock);
			sServices[service->localPort] = NULL;
		}
		delete_service(service);
		return status;
	}

	*_service = service;
	return B_OK;
}


static void
vchiq_close_service(vchiq_service* service)
{
	MutexLocker locker(sServiceLock);
	bool wait = false;
	if (service->state == SERVICE_OPEN) {
		service->state = SERVICE_CLOSE_SENT;
		wait = true;
	}
	locker.Unlock();

	if (wait) {
		if (send_message(MAKE_MSG(MSG_CLOSE, service->localPort,
				service->remotePort), NULL, 0, false) == B_OK) {
			if (acquire_sem_etc(service->stateSem, 1, B_RELATIVE_TIMEOUT,
					OPEN_TIMEOUT) != B_OK) {
				ERROR("the firmware did not close service %" B_PRIu32 "\n",
					service->localPort);
			}
		}
	}

	locker.Lock();
	service->state = SERVICE_CLOSED;
	sServices[service->localPort] = NULL;
	finish_bulk(service, BULK_TRANSMIT, -1);
	finish_bulk(service, BULK_RECEIVE, -1);
	locker.Unlock();

	// a transfer still under way has its lock
	for (int i = 0; i < 2; i++) {
		mutex_lock(&service->bulk[i].lock);
		mutex_unlock(&service->bulk[i].lock);
	}

	delete_service(service);
}


static int16
vchiq_peer_version(vchiq_service* service)
{
	return service->peerVersion;
}


static status_t
vchiq_queue_message(vchiq_service* service, const void* data, size_t size,
	bool userData)
{
	if (service->state != SERVICE_OPEN)
		return B_DEV_NOT_READY;

	return send_message(MAKE_MSG(MSG_DATA, service->localPort,
		service->remotePort), data, size, userData);
}


//	#pragma mark - bulk transfers


/*!	The page list of a service's transfers in one direction. The firmware
	is given its address in 32 bits, so it has to lie in the first gigabyte,
	in one piece. It is allocated with the service and large enough for the
	largest transfer: memory like that can be scarce later on, and a transfer
	that fails for the lack of it leaves the firmware waiting.
*/
static status_t
allocate_page_list(vchiq_service* service, int direction)
{
	size_t size = ROUNDUP(sizeof(pagelist)
		+ VCHIQ_MAX_BULK_SIZE / B_PAGE_SIZE * sizeof(uint32), B_PAGE_SIZE);

	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	physicalRestrictions.high_address = 1ull << 30;
	void* address;
	area_id area = create_area_etc(B_SYSTEM_TEAM, "vchiq page list", size,
		B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0,
		&virtualRestrictions, &physicalRestrictions, &address);
	if (area < 0)
		return area;

	physical_entry entry;
	status_t status = get_memory_map(address, size, &entry, 1);
	if (status != B_OK) {
		delete_area(area);
		return status;
	}

	service->bulk[direction].listArea = area;
	service->bulk[direction].list = (pagelist*)address;
	service->bulk[direction].listAddress = entry.address;
	return B_OK;
}


/*!	The memory the firmware copies from or to. Where page lists hold 36-bit
	addresses (BCM2711) any pages will do; the older form only reaches the
	first gigabyte. The buffer only ever grows and stays with the service.
*/
static status_t
ensure_bulk_buffer(vchiq_service* service, int direction, size_t size)
{
	size = ROUNDUP(size, B_PAGE_SIZE);
	if (service->bulk[direction].area >= 0
		&& service->bulk[direction].size >= size) {
		return B_OK;
	}

	size_t pageCount = size / B_PAGE_SIZE;
	phys_addr_t* pages = (phys_addr_t*)malloc(pageCount * sizeof(phys_addr_t));
	if (pages == NULL)
		return B_NO_MEMORY;

	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	if (!s36BitAddresses)
		physicalRestrictions.high_address = 1ull << 30;
	void* address;
	area_id area = create_area_etc(B_SYSTEM_TEAM, "vchiq bulk", size,
		s36BitAddresses ? B_FULL_LOCK : B_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0,
		&virtualRestrictions, &physicalRestrictions, &address);
	if (area < 0) {
		free(pages);
		return area;
	}

	for (size_t i = 0; i < pageCount; i++) {
		physical_entry entry;
		status_t status = get_memory_map((uint8*)address + i * B_PAGE_SIZE,
			B_PAGE_SIZE, &entry, 1);
		if (status != B_OK) {
			delete_area(area);
			free(pages);
			return status;
		}
		pages[i] = entry.address;
	}

	dprintf("vchiq: service %" B_PRIu32 ": %" B_PRIuSIZE " bytes to %s "
		"through, first page at %#" B_PRIxPHYSADDR "\n", service->localPort,
		size, direction == BULK_TRANSMIT ? "send" : "receive", pages[0]);

	if (service->bulk[direction].area >= 0)
		delete_area(service->bulk[direction].area);
	free(service->bulk[direction].pages);
	service->bulk[direction].area = area;
	service->bulk[direction].buffer = (uint8*)address;
	service->bulk[direction].size = size;
	service->bulk[direction].pages = pages;
	return B_OK;
}


static status_t
bulk_transfer(vchiq_service* service, void* data, size_t size, bool userData,
	int direction, size_t* _actual)
{
	if (size == 0 || size > VCHIQ_MAX_BULK_SIZE)
		return B_BAD_VALUE;

	MutexLocker locker(service->bulk[direction].lock);

	if (service->state != SERVICE_OPEN)
		return B_DEV_NOT_READY;
	if (service->bulk[direction].broken)
		return B_IO_ERROR;

	status_t status = ensure_bulk_buffer(service, direction, size);
	if (status != B_OK)
		return status;

	pagelist* list = service->bulk[direction].list;
	phys_addr_t address = service->bulk[direction].listAddress;
	uint8* buffer = service->bulk[direction].buffer;
	const phys_addr_t* pageAddresses = service->bulk[direction].pages;
	// one fragment per service will do: only receiving uses it
	uint8* fragment = sFragments + service->localPort * FRAGMENT_SIZE;

	if (direction == BULK_TRANSMIT) {
		if (userData) {
			status = user_memcpy(buffer, data, size);
			if (status != B_OK)
				return status;
		} else
			memcpy(buffer, data, size);
	}

	list->length = size;
	list->offset = 0;
	if (direction == BULK_TRANSMIT)
		list->type = PAGELIST_WRITE;
	else if ((size & (CACHE_LINE_SIZE - 1)) == 0)
		list->type = PAGELIST_READ;
	else {
		// the firmware puts the last, partial cache line into the fragment
		list->type = PAGELIST_READ_WITH_FRAGMENTS + service->localPort;
	}

	// one entry for each run of pages that follow each other in memory
	uint32 pages = ROUNDUP(size, B_PAGE_SIZE) / B_PAGE_SIZE;
	uint32 maxRun = s36BitAddresses ? PAGELIST_MAX_RUN_36 : PAGELIST_MAX_RUN;
	uint32 runs = 0;
	for (uint32 i = 0; i < pages;) {
		phys_addr_t pageAddress = pageAddresses[i];
		uint32 run = 1;
		while (i + run < pages && run < maxRun && pageAddresses[i + run]
				== pageAddress + (phys_addr_t)run * B_PAGE_SIZE) {
			run++;
		}

		if (s36BitAddresses)
			list->addrs[runs++] = (uint32)(pageAddress >> 4) | (run - 1);
		else {
			list->addrs[runs++] = (uint32)pageAddress | VC_BUS_OFFSET
				| (run - 1);
		}
		i += run;
	}
	uint16 type = list->type;

	flush_cache(list, sizeof(pagelist) + runs * sizeof(uint32));
	flush_cache(buffer, size);

	{
		MutexLocker serviceLocker(sServiceLock);
		if (service->state != SERVICE_OPEN)
			return B_DEV_NOT_READY;
		service->bulk[direction].waiting = true;
	}

	uint32 payload[2] = { (uint32)address | VC_BUS_OFFSET, (uint32)size };
	status = send_message(MAKE_MSG(direction == BULK_TRANSMIT
			? MSG_BULK_TX : MSG_BULK_RX, service->localPort,
		service->remotePort), payload, sizeof(payload), false);
	if (status == B_OK) {
		status = acquire_sem_etc(service->bulk[direction].sem, 1,
			B_RELATIVE_TIMEOUT, BULK_TIMEOUT);
	}
	if (status != B_OK) {
		// The firmware may still use the memory, and its answer would be
		// taken for the next transfer's.
		MutexLocker serviceLocker(sServiceLock);
		if (service->bulk[direction].waiting) {
			service->bulk[direction].waiting = false;
			service->bulk[direction].broken = true;
			ERROR("bulk transfer of %" B_PRIuSIZE " bytes on service %"
				B_PRIu32 ": %s\n", size, service->localPort,
				strerror(status));
			return status;
		}
		// it came in after all
		acquire_sem_etc(service->bulk[direction].sem, 1, B_RELATIVE_TIMEOUT,
			0);
	}

	int32 actual = service->bulk[direction].actual;
	if (actual < 0)
		return B_CANCELED;
	if ((size_t)actual > size)
		actual = size;

	if (direction == BULK_RECEIVE) {
		flush_cache(buffer, size);

		uint32 tail = actual & (CACHE_LINE_SIZE - 1);
		if (type >= PAGELIST_READ_WITH_FRAGMENTS && tail != 0) {
			memcpy(buffer + (actual & ~(CACHE_LINE_SIZE - 1)),
				fragment + CACHE_LINE_SIZE, tail);
		}

		if (userData) {
			status = user_memcpy(data, buffer, actual);
			if (status != B_OK)
				return status;
		} else
			memcpy(data, buffer, actual);
	}

	if (_actual != NULL)
		*_actual = actual;
	return B_OK;
}


static status_t
vchiq_bulk_transmit(vchiq_service* service, const void* data, size_t size,
	bool userData)
{
	return bulk_transfer(service, (void*)data, size, userData, BULK_TRANSMIT,
		NULL);
}


static status_t
vchiq_bulk_receive(vchiq_service* service, void* data, size_t size,
	bool userData, size_t* _actual)
{
	return bulk_transfer(service, data, size, userData, BULK_RECEIVE,
		_actual);
}


//	#pragma mark - start


/*!	The device tree's node of the doorbell: this only runs on a
	Raspberry Pi.
*/
static status_t
find_device(phys_addr_t* _registers, uint32* _interrupt)
{
	device_node* root = sDeviceManager->get_root_node();
	if (root == NULL)
		return B_DEVICE_NOT_FOUND;

	static const char* const kCompatible[] = {
		"brcm,bcm2711-vchiq", "brcm,bcm2836-vchiq", "brcm,bcm2835-vchiq"
	};

	device_node* node = NULL;
	for (size_t i = 0; node == NULL && i < B_COUNT_OF(kCompatible); i++) {
		device_attr attributes[] = {
			{ "fdt/compatible", B_STRING_TYPE,
				{ .string = kCompatible[i] } },
			{}
		};
		if (sDeviceManager->find_child_node(root, attributes, &node) != B_OK)
			node = NULL;
		else
			s36BitAddresses = i == 0;
	}
	sDeviceManager->put_node(root);
	if (node == NULL)
		return B_DEVICE_NOT_FOUND;

	fdt_device_module_info* fdt;
	fdt_device* device;
	status_t status = sDeviceManager->get_driver(node,
		(driver_module_info**)&fdt, (void**)&device);
	if (status == B_OK) {
		uint64 base, size, interrupt;
		if (fdt->get_reg(device, 0, &base, &size)
			&& fdt->get_interrupt(device, 0, NULL, &interrupt)) {
			*_registers = base;
			*_interrupt = interrupt;
		} else
			status = B_BAD_DATA;
	}

	sDeviceManager->put_node(node);
	return status;
}


static status_t
init()
{
	phys_addr_t registers;
	status_t status = find_device(&registers, &sInterrupt);
	if (status != B_OK)
		return status;

	void* address;
	sRegistersArea = map_physical_memory("vchiq doorbell",
		registers & ~(phys_addr_t)(B_PAGE_SIZE - 1), B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		&address);
	if (sRegistersArea < 0)
		return sRegistersArea;
	sRegisters = (volatile uint32*)((uint8*)address
		+ (registers & (B_PAGE_SIZE - 1)));

	// The slots and the fragments: where the VideoCore reaches them, and
	// out of the CPU cache.
	size_t slotSize = TOTAL_SLOTS * SLOT_SIZE;
	size_t fragmentSize = ROUNDUP(MAX_FRAGMENTS * FRAGMENT_SIZE, B_PAGE_SIZE);
	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	physicalRestrictions.high_address = 1ull << 30;
	sSlotArea = create_area_etc(B_SYSTEM_TEAM, "vchiq slots",
		slotSize + fragmentSize, B_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0, &virtualRestrictions,
		&physicalRestrictions, (void**)&sSlotData);
	if (sSlotArea < 0)
		return sSlotArea;

	physical_entry entry;
	status = get_memory_map(sSlotData, slotSize + fragmentSize, &entry, 1);
	if (status != B_OK)
		return status;
	sSlotAddress = entry.address;

	memset(sSlotData, 0, slotSize + fragmentSize);
	flush_cache(sSlotData, slotSize + fragmentSize);
	status = vm_set_area_memory_type(sSlotArea, sSlotAddress,
		B_WRITE_COMBINING_MEMORY);
	if (status != B_OK)
		return status;

	sFragments = sSlotData + slotSize;

	slot_zero* zero = (slot_zero*)sSlotData;
	int32 slots = TOTAL_SLOTS - SLOT_ZERO_SLOTS;
	zero->magic = VCHIQ_MAGIC;
	zero->version = VCHIQ_VERSION;
	zero->version_min = VCHIQ_VERSION_MIN;
	zero->slot_zero_size = sizeof(slot_zero);
	zero->slot_size = SLOT_SIZE;
	zero->max_slots = MAX_SLOTS;
	zero->max_slots_per_side = MAX_SLOTS_PER_SIDE;
	zero->platform_data[0] = (uint32)(sSlotAddress + slotSize)
		| VC_BUS_OFFSET;
	zero->platform_data[1] = MAX_FRAGMENTS;
	zero->master.slot_sync = SLOT_ZERO_SLOTS;
	zero->master.slot_first = SLOT_ZERO_SLOTS + 1;
	zero->master.slot_last = SLOT_ZERO_SLOTS + slots / 2 - 1;
	zero->slave.slot_sync = SLOT_ZERO_SLOTS + slots / 2;
	zero->slave.slot_first = SLOT_ZERO_SLOTS + slots / 2 + 1;
	zero->slave.slot_last = SLOT_ZERO_SLOTS + slots - 1;

	// we are the slave
	sLocal = &zero->slave;
	sRemote = &zero->master;

	int32 available = 0;
	for (int32 i = sLocal->slot_first; i <= sLocal->slot_last; i++)
		sLocal->slot_queue[available++] = i;
	sSlotQueueAvailable = available;
	sLocal->slot_queue_recycle = available;
	sLocal->tx_pos = 0;
	sLocal->debug[0] = DEBUG_MAX;
	((message_header*)slot_data(sLocal->slot_sync))->id
		= MAKE_MSG(MSG_PADDING, 0, 0);
	// the sync slot is free
	sLocal->sync_release.fired = 1;

	sTriggerSem = create_sem(0, "vchiq trigger");
	sRecycleSem = create_sem(0, "vchiq recycle");
	sSlotAvailableSem = create_sem(available, "vchiq slot available");
	sConnectSem = create_sem(0, "vchiq connect");
	if (sTriggerSem < 0 || sRecycleSem < 0 || sSlotAvailableSem < 0
		|| sConnectSem < 0) {
		return B_NO_MORE_SEMS;
	}

	thread_id slotThread = spawn_kernel_thread(slot_handler, "vchiq slots",
		B_REAL_TIME_DISPLAY_PRIORITY, NULL);
	thread_id recycleThread = spawn_kernel_thread(recycle_handler,
		"vchiq recycle", B_REAL_TIME_DISPLAY_PRIORITY, NULL);
	if (slotThread < 0 || recycleThread < 0)
		return B_NO_MORE_THREADS;

	status = install_io_interrupt_handler(sInterrupt, doorbell_interrupt,
		NULL, 0);
	if (status != B_OK)
		return status;

	resume_thread(slotThread);
	resume_thread(recycleThread);

	sLocal->initialised = 1;
	barrier();

	// tell the firmware where the slots are
	uint32 channelBase = (uint32)sSlotAddress | VC_BUS_OFFSET;
	status = sFirmware->property(TAG_VCHIQ_INIT, &channelBase,
		sizeof(channelBase));
	if (status != B_OK || channelBase != 0) {
		ERROR("the firmware refused the slots: %s, %#" B_PRIx32 "\n",
			strerror(status), channelBase);
		return status != B_OK ? status : B_ERROR;
	}

	status = send_message(MAKE_MSG(MSG_CONNECT, 0, 0), NULL, 0, false);
	if (status == B_OK) {
		status = acquire_sem_etc(sConnectSem, 1, B_RELATIVE_TIMEOUT,
			OPEN_TIMEOUT);
	}
	if (status != B_OK) {
		ERROR("no connection to the firmware: %s\n", strerror(status));
		return status;
	}

	INFO("connected, the firmware speaks version %d (interrupt %" B_PRIu32
		")\n", zero->version, sInterrupt);
	return B_OK;
}


static status_t
std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
			// started by vchiq_init_check(), see there
			return B_OK;

		case B_MODULE_UNINIT:
			return B_OK;
	}

	return B_ERROR;
}


module_dependency module_dependencies[] = {
	{B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager},
	{RPI_FIRMWARE_MODULE_NAME, (module_info**)&sFirmware},
	{}
};

static vchiq_module_info sModule = {
	{
		VCHIQ_MODULE_NAME,
		B_KEEP_LOADED,
		std_ops
	},
	vchiq_init_check,
	vchiq_open_service,
	vchiq_close_service,
	vchiq_peer_version,
	vchiq_queue_message,
	vchiq_bulk_transmit,
	vchiq_bulk_receive
};

module_info* modules[] = {
	(module_info*)&sModule,
	NULL
};
