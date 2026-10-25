/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The audio DSP ("ADSP", LPASS) of the Qualcomm SC8280XP (Radxa Dragon
	Q8B). Besides audio it runs the PMIC's USB-C side: port roles, cable
	orientation and the DisplayPort alternate mode of the two USB-C ports,
	which the applications processor gets as "pmic_glink" messages over
	GLINK. The firmware leaves it off.

	Its start, as Linux' qcom_q6v5_pas (sc8280xp_adsp_resource):
	 - the AOSS is told the image is being loaded (a QMP message);
	 - the LCX and LMX power domains are voted to their top level through
	   the apps RSC (RPMh), until the DSP says it has voted for itself
	   ("handover");
	 - TrustZone's PAS (peripheral 1) authenticates the signed image that is
	   loaded into its reserved region, and starts it;
	 - the DSP reports "ready" (or "fatal") in its SMP2P entry in SMEM.

	All of it is polled: the interrupts (IPCC, SMP2P) are not needed for a
	start. */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include <KernelExport.h>
#include <arch/atomic.h>
#include <device_manager.h>
#include <driver_settings.h>
#include <lock.h>
#include <util/AutoLock.h>
#include <qcom_gpr.h>
#include <qcom_usbc_ports.h>

#include <libfdt.h>

#include "qcom_rpmh.h"
#include "qcom_scm.h"


#define QCOM_ADSP_MODULE_NAME "drivers/power/qcom_adsp/driver_v1"

#define TRACE(x...)	dprintf("qcom_adsp: " x)
#define ERROR(x...)	dprintf("qcom_adsp: " x)


extern void* gFDT;

static device_manager_info* sDeviceManager;

static const uint32 kAdspPasId = 1;


struct register_window {
	area_id			area;
	volatile uint8*	base;
	size_t			size;

	register_window() : area(-1), base(NULL), size(0) {}

	status_t Map(const char* name, phys_addr_t address, size_t length)
	{
		size = length;
		area = map_physical_memory(name, address, length,
			B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&base);
		return area < 0 ? area : B_OK;
	}

	void Unmap()
	{
		if (area >= 0)
			delete_area(area);
		area = -1;
		base = NULL;
	}

	uint32 Read(uint32 offset) const
		{ return *(volatile uint32*)(base + offset); }
	uint16 Read16(uint32 offset) const
		{ return *(volatile uint16*)(base + offset); }
	void Write(uint32 offset, uint32 value)
	{
		*(volatile uint32*)(base + offset) = value;
		memory_full_barrier();
	}
};


static register_window sSmem;
static register_window sIpcc;
static register_window sQmp;
static register_window sMutex;
static phys_addr_t sRegionBase;
static size_t sRegionSize;
static char sFirmwareName[128];
static thread_id sBootThread = -1;


// #pragma mark - device tree


static bool
fdt_reg(int node, int index, phys_addr_t* _base, size_t* _size)
{
	int length;
	const fdt32_t* reg = (const fdt32_t*)fdt_getprop(gFDT, node, "reg",
		&length);
	if (reg == NULL || length < (index + 1) * 16)
		return false;
	reg += index * 4;
	*_base = (uint64)fdt32_to_cpu(reg[0]) << 32 | fdt32_to_cpu(reg[1]);
	*_size = (uint64)fdt32_to_cpu(reg[2]) << 32 | fdt32_to_cpu(reg[3]);
	return true;
}


static int
fdt_compatible_node(const char* compatible)
{
	return fdt_node_offset_by_compatible(gFDT, -1, compatible);
}


static status_t
map_compatible(register_window& window, const char* name,
	const char* compatible, int index = 0)
{
	int node = fdt_compatible_node(compatible);
	phys_addr_t base;
	size_t size;
	if (node < 0 || !fdt_reg(node, index, &base, &size))
		return B_DEVICE_NOT_FOUND;
	return window.Map(name, base, size);
}


// #pragma mark - RPMh


// the first of the apps RSC's active TCSes (sdhci votes through the second)
static const uint32 kAdspTcs = 0;
static qcom_rpmh sRpmh;


//!	Votes the arc \a name to \a level, or to its top level when negative.
static status_t
arc_vote(const char* name, int32 level)
{
	uint32 levels = 0;
	uint32 address = qcom_cmd_db_address(sRpmh, name, &levels);
	if (address == 0 || levels == 0) {
		ERROR("no arc %s in the command DB\n", name);
		return B_DEVICE_NOT_FOUND;
	}
	uint32 value = level < 0 ? levels - 1 : (uint32)level;
	status_t status = qcom_rpmh_write(sRpmh, kAdspTcs, &address, &value, 1);
	TRACE("%s (%#" B_PRIx32 ", %" B_PRIu32 " levels) to %" B_PRIu32 ": %s\n",
		name, address, levels, value, strerror(status));
	return status;
}


// #pragma mark - AOSS QMP


/*	The AOSS's message RAM (Linux' qcom_aoss.c): a descriptor of both sides'
	link and channel states, and the mailbox of ours ("mcore"). A doorbell
	on the IPCC (client 0, the AOP; signal 0) tells it to look. */

enum {
	kQmpMagic = 0x00, kQmpVersion = 0x04,
	kQmpUcoreLinkState = 0x0c, kQmpUcoreLinkStateAck = 0x10,
	kQmpUcoreChannelState = 0x14, kQmpUcoreChannelStateAck = 0x18,
	kQmpMcoreLinkState = 0x24, kQmpMcoreLinkStateAck = 0x28,
	kQmpMcoreChannelState = 0x2c, kQmpMcoreChannelStateAck = 0x30,
	kQmpMcoreMailboxSize = 0x34, kQmpMcoreMailboxOffset = 0x38,
};

static const uint32 kQmpStateUp = 0x0000ffff;
static const uint32 kQmpMessageLength = 64;
static bool sQmpOpen;


// IPCC (qcom-ipcc.c): the receive side; the signal of the ADSP's GLINK
// doorbells (client 3, signal 0)
enum {
	kIpccSend = 0x0c, kIpccReceive = 0x10, kIpccEnable = 0x14,
	kIpccDisable = 0x18, kIpccClear = 0x1c,
};
static const uint32 kIpccGlinkAdsp = 3u << 16 | 0;
static uint32 sIpccVector;
static bool sIpccHandler;
static sem_id sGlinkSem = -1;


static int32
ipcc_interrupt(void*)
{
	bool handled = false, wake = false;
	for (int i = 0; i < 32; i++) {
		uint32 id = sIpcc.Read(kIpccReceive);
		if (id == 0xffffffff)
			break;
		sIpcc.Write(kIpccClear, id);
		handled = true;
		wake |= id == kIpccGlinkAdsp;
	}
	if (wake && sGlinkSem >= 0) {
		release_sem_etc(sGlinkSem, 1, B_DO_NOT_RESCHEDULE);
		return B_INVOKE_SCHEDULER;
	}
	return handled ? B_HANDLED_INTERRUPT : B_UNHANDLED_INTERRUPT;
}


/*!	The ADSP's GLINK doorbells as an interrupt, so the GLINK thread sleeps
	until there is something; without it, it polls. */
static void
ipcc_interrupt_start()
{
	int ipcc = fdt_compatible_node("qcom,ipcc");
	int length;
	const fdt32_t* interrupts = ipcc >= 0 ? (const fdt32_t*)fdt_getprop(gFDT,
		ipcc, "interrupts", &length) : NULL;
	if (interrupts == NULL || length < 12 || fdt32_to_cpu(interrupts[0]) != 0)
		return;
	// a GIC SPI
	sIpccVector = fdt32_to_cpu(interrupts[1]) + 32;
	if (install_io_interrupt_handler(sIpccVector, ipcc_interrupt, NULL, 0)
			!= B_OK)
		return;
	sIpccHandler = true;
	sIpcc.Write(kIpccEnable, kIpccGlinkAdsp);
}


static void
ipcc_interrupt_stop()
{
	if (!sIpccHandler)
		return;
	sIpcc.Write(kIpccDisable, kIpccGlinkAdsp);
	remove_io_interrupt_handler(sIpccVector, ipcc_interrupt, NULL);
	sIpccHandler = false;
}


static void
ipcc_signal(uint32 client, uint32 signal)
{
	// IPCC_REG_SEND_ID: the client in 31:16, the signal in 15:0
	sIpcc.Write(kIpccSend, client << 16 | signal);
}


static bool
qmp_wait(uint32 offset, uint32 value)
{
	for (bigtime_t deadline = system_time() + 1000000;
			system_time() < deadline; snooze(1000)) {
		if (sQmp.Read(offset) == value)
			return true;
	}
	return false;
}


static status_t
qmp_open()
{
	if (sQmpOpen)
		return B_OK;
	if (sQmp.Read(kQmpMagic) != 0x4d41494c || sQmp.Read(kQmpVersion) != 1
		|| sQmp.Read(kQmpMcoreMailboxSize) < kQmpMessageLength + 4) {
		ERROR("aoss qmp: no version 1 message RAM\n");
		return B_DEVICE_NOT_FOUND;
	}
	sQmp.Write(kQmpUcoreLinkStateAck, sQmp.Read(kQmpUcoreLinkState));
	sQmp.Write(kQmpMcoreLinkState, kQmpStateUp);
	ipcc_signal(0, 0);
	if (!qmp_wait(kQmpMcoreLinkStateAck, kQmpStateUp)) {
		ERROR("aoss qmp: link not acknowledged\n");
		return B_TIMED_OUT;
	}
	sQmp.Write(kQmpMcoreChannelState, kQmpStateUp);
	ipcc_signal(0, 0);
	if (!qmp_wait(kQmpUcoreChannelState, kQmpStateUp)) {
		ERROR("aoss qmp: its channel does not come up\n");
		return B_TIMED_OUT;
	}
	sQmp.Write(kQmpUcoreChannelStateAck, kQmpStateUp);
	ipcc_signal(0, 0);
	if (!qmp_wait(kQmpMcoreChannelStateAck, kQmpStateUp)) {
		ERROR("aoss qmp: our channel not acknowledged\n");
		return B_TIMED_OUT;
	}
	sQmpOpen = true;
	return B_OK;
}


static status_t
qmp_send(const char* message)
{
	uint32 words[kQmpMessageLength / 4] = {};
	size_t length = strlen(message);
	if (length >= kQmpMessageLength)
		return B_BAD_VALUE;
	memcpy(words, message, length);

	uint32 mailbox = sQmp.Read(kQmpMcoreMailboxOffset);
	// the message RAM takes 32-bit accesses only
	for (uint32 i = 0; i < kQmpMessageLength / 4; i++)
		sQmp.Write(mailbox + 4 + i * 4, words[i]);
	sQmp.Write(mailbox, kQmpMessageLength);
	ipcc_signal(0, 0);
	// the AOSS clears the length when it has the message
	status_t status = qmp_wait(mailbox, 0) ? B_OK : B_TIMED_OUT;
	if (status != B_OK)
		sQmp.Write(mailbox, 0);
	TRACE("aoss: \"%s\": %s\n", message, strerror(status));
	return status;
}


// #pragma mark - SMEM, SMP2P


/*	SMEM (Linux' smem.c): a table of partitions at the end of the region,
	one per pair of hosts (0 the applications processor, 2 the ADSP, 0xfffe
	the global one), each a list of items. */

static const uint16 kHostApps = 0;
static const uint16 kHostAdsp = 2;
static const uint16 kHostGlobal = 0xfffe;


//!	An item of the partition of \a host0 and \a host1 (uncached ones only).
static volatile uint8*
smem_item(uint16 host0, uint16 host1, uint16 item, uint32* _size = NULL)
{
	uint32 table = sSmem.size - 4096;
	if (sSmem.Read(table) != 0x434f5424)	// "$TOC"
		return NULL;
	uint32 count = sSmem.Read(table + 8);
	for (uint32 i = 0; i < count && i < 64; i++) {
		uint32 entry = table + 32 + 48 * i;
		uint32 offset = sSmem.Read(entry), size = sSmem.Read(entry + 4);
		uint16 a = sSmem.Read16(entry + 12), b = sSmem.Read16(entry + 14);
		if (!((a == host0 && b == host1) || (a == host1 && b == host0))
			|| offset == 0 || offset + size > sSmem.size)
			continue;
		if (sSmem.Read(offset) != 0x54525024)	// "$PRT"
			return NULL;
		uint32 freeUncached = sSmem.Read(offset + 12);
		uint32 position = 32;
		while (position + 16 <= freeUncached && freeUncached <= size) {
			uint32 header = offset + position;
			if (sSmem.Read16(header) != 0xa5a5)
				break;
			uint32 itemSize = sSmem.Read(header + 4);
			uint16 paddingData = sSmem.Read16(header + 8);
			uint16 paddingHeader = sSmem.Read16(header + 10);
			if (sSmem.Read16(header + 2) == item) {
				if (_size != NULL)
					*_size = itemSize - paddingData;
				return sSmem.base + header + 16 + paddingHeader;
			}
			position += 16 + paddingHeader + itemSize;
		}
		return NULL;
	}
	return NULL;
}


/*!	Takes the hardware mutex SMEM's allocations are made under (the TCSR
	mutex of its "hwlocks"; locks 4 KiB apart, held by writing one's own
	ID, 1 for the applications processor). */
static bool
smem_lock()
{
	for (bigtime_t deadline = system_time() + 1000000;
			system_time() < deadline; spin(10)) {
		sMutex.Write(0x3000, 1);
		if (sMutex.Read(0x3000) == 1)
			return true;
	}
	return false;
}


static void
smem_unlock()
{
	sMutex.Write(0x3000, 0);
}


/*!	Allocates the uncached item \a item of \a size bytes in the partition
	of \a host0 and \a host1, or returns it if it is there (as
	qcom_smem_alloc_private()). */
static volatile uint8*
smem_allocate(uint16 host0, uint16 host1, uint16 item, uint32 size)
{
	volatile uint8* existing = smem_item(host0, host1, item);
	if (existing != NULL)
		return existing;

	uint32 table = sSmem.size - 4096;
	uint32 count = sSmem.Read(table + 8);
	for (uint32 i = 0; i < count && i < 64; i++) {
		uint32 entry = table + 32 + 48 * i;
		uint32 offset = sSmem.Read(entry), partitionSize = sSmem.Read(entry + 4);
		uint16 a = sSmem.Read16(entry + 12), b = sSmem.Read16(entry + 14);
		if (!((a == host0 && b == host1) || (a == host1 && b == host0))
			|| offset == 0 || offset + partitionSize > sSmem.size
			|| sSmem.Read(offset) != 0x54525024)
			continue;
		if (!smem_lock())
			return NULL;
		volatile uint8* data = smem_item(host0, host1, item);
		uint32 freeUncached = sSmem.Read(offset + 12);
		uint32 freeCached = sSmem.Read(offset + 16);
		uint32 alignedSize = (size + 7) & ~7u;
		if (data == NULL && freeUncached + 16 + alignedSize <= freeCached) {
			uint32 header = offset + freeUncached;
			sSmem.Write(header, item << 16 | 0xa5a5);
			sSmem.Write(header + 4, alignedSize);
			sSmem.Write(header + 8, alignedSize - size);	// padding: data, header
			sSmem.Write(header + 12, 0);
			for (uint32 w = 0; w < alignedSize; w += 4)
				sSmem.Write(header + 16 + w, 0);
			sSmem.Write(offset + 12, freeUncached + 16 + alignedSize);
			data = sSmem.base + header + 16;
		}
		smem_unlock();
		if (data == NULL)
			ERROR("smem: no room for item %u\n", item);
		return data;
	}
	return NULL;
}


/*!	Our SMP2P item for the ADSP (item 429, as Linux' smp2p.c makes it when
	it starts, before any remote processor): its one entry, "master-kernel",
	is where a stop would be asked for. Then the ADSP is told. */
static status_t
smp2p_create()
{
	const uint32 entries = 16;
	volatile uint8* out = smem_allocate(kHostApps, kHostAdsp, 429,
		20 + 20 * entries);
	if (out == NULL)
		return B_NO_MEMORY;
	volatile uint32* words = (volatile uint32*)out;
	if (words[0] != 0x504d5324) {
		// version 1, the SSR acknowledgement feature
		words[1] = 1 | 1u << 8;
		words[2] = kHostApps | (uint32)kHostAdsp << 16;
		words[3] = entries | 1u << 16;	// total, valid
		words[4] = 0;
		static const char kName[16] = "master-kernel";
		for (int w = 0; w < 4; w++)
			words[5 + w] = ((const uint32*)kName)[w];
		words[9] = 0;
		memory_full_barrier();
		words[0] = 0x504d5324;
		memory_full_barrier();
	}
	// IPCC: client 3 (LPASS), signal 2 (SMP2P)
	ipcc_signal(3, 2);
	return B_OK;
}


static void
smem_log_items(uint16 host0, uint16 host1)
{
	uint32 table = sSmem.size - 4096;
	uint32 count = sSmem.Read(table + 8);
	for (uint32 i = 0; i < count && i < 64; i++) {
		uint32 entry = table + 32 + 48 * i;
		uint32 offset = sSmem.Read(entry), size = sSmem.Read(entry + 4);
		uint16 a = sSmem.Read16(entry + 12), b = sSmem.Read16(entry + 14);
		if (!((a == host0 && b == host1) || (a == host1 && b == host0))
			|| offset == 0 || offset + size > sSmem.size)
			continue;
		char list[192] = {};
		size_t length = 0;
		uint32 freeUncached = sSmem.Read(offset + 12);
		uint32 position = 32;
		while (position + 16 <= freeUncached && freeUncached <= size) {
			uint32 header = offset + position;
			if (sSmem.Read16(header) != 0xa5a5)
				break;
			if (length + 16 < sizeof(list)) {
				length += snprintf(list + length, sizeof(list) - length,
					" %u", sSmem.Read16(header + 2));
			}
			position += 16 + sSmem.Read16(header + 10)
				+ sSmem.Read(header + 4);
		}
		TRACE("smem %#x-%#x: items%s; free %#" B_PRIx32 "-%#" B_PRIx32 "\n",
			host0, host1, list, freeUncached, sSmem.Read(offset + 16));
	}
}


/*!	The value of the entry \a name in the SMP2P item \a item that the ADSP
	writes, or false when it has none (yet). Its layout (smp2p.c): magic
	"$SMP", version and features, the two hosts, the entry counts, flags;
	then entries of a 16 character name and a 32-bit value. */
static bool
smp2p_entry(uint16 item, const char* name, uint32* _value)
{
	volatile uint8* smp2p = smem_item(kHostApps, kHostAdsp, item);
	if (smp2p == NULL || *(volatile uint32*)smp2p != 0x504d5324)
		return false;
	uint32 valid = *(volatile uint32*)(smp2p + 12) >> 16;
	for (uint32 i = 0; i < valid && i < 16; i++) {
		volatile uint8* entry = smp2p + 20 + 20 * i;
		char entryName[17] = {};
		for (int c = 0; c < 16; c++)
			entryName[c] = entry[c];
		if (strcmp(entryName, name) == 0) {
			*_value = *(volatile uint32*)(entry + 16);
			return true;
		}
	}
	return false;
}


// #pragma mark - GLINK


/*	GLINK over SMEM (Linux' qcom_glink_smem.c and qcom_glink_native.c): a
	ring FIFO each way, ours (item 479) and the ADSP's (480), their tails
	and heads in the descriptor (478: of ours, then of the ADSP's). On them
	8 byte aligned messages of an 8 byte header {command, a 16 and a 32-bit
	parameter}. A doorbell on the IPCC (client 3, signal 0) tells the ADSP
	to look; its doorbells are not needed, the FIFO is polled.

	Channels are opened by name. Before a message can be sent on one, the
	other side must have announced a buffer ("intent") big enough for it;
	ours are announced when a channel opens. */

enum {
	kGlinkVersion = 0, kGlinkVersionAck = 1, kGlinkOpen = 2, kGlinkClose = 3,
	kGlinkOpenAck = 4, kGlinkIntent = 5, kGlinkRxDone = 6,
	kGlinkRxIntentRequest = 7, kGlinkRxIntentRequestAck = 8,
	kGlinkTxData = 9, kGlinkCloseAck = 11, kGlinkTxDataContinued = 12,
	kGlinkReadNotify = 13, kGlinkRxDoneReuse = 14, kGlinkSignals = 15,
};

static const uint32 kGlinkVersion1 = 1;
static const uint32 kGlinkFeatureIntentReuse = 1;
static const uint32 kGlinkFifoReserve = 8 + 16;
	// FIFO_FULL_RESERVE, TX_BLOCKED_SIGNAL_RESERVE
static const uint32 kGlinkIntentSize = 1024;
static const uint32 kGlinkIntentCount = 5;
static const uint32 kGlinkMaxMessage = 4096;
static const int kGlinkMaxChannels = 16;
static const int kGlinkMaxRemoteIntents = 64;


struct glink_message {
	uint16	command;
	uint16	param1;
	uint32	param2;
};

struct glink_pipe {
	volatile uint32*	tail;
	volatile uint32*	head;
	volatile uint8*		fifo;
	uint32				length;
};

struct glink_remote_intent {
	uint32	id;
	uint32	size;
	bool	inUse;
};

struct glink_channel;
typedef void (*glink_receive_hook)(glink_channel* channel, const uint8* data,
	uint32 length);

struct glink_channel {
	char				name[32];
	uint16				localId;
	uint16				remoteId;		// 0: the ADSP has not opened it
	bool				wanted;			// a hook of ours takes it
	bool				openSent;
	bool				openAcked;
	glink_receive_hook	hook;
	glink_remote_intent	remoteIntents[kGlinkMaxRemoteIntents];
	uint32				remoteIntentCount;
	bigtime_t			intentRequested;	// one asked for (0: none)
	uint32				nextIntentId;
	uint8*				rxBuffer;		// a message in chunks
	uint32				rxLength;
};


static glink_pipe sTx, sRx;
static uint32 sGlinkFeatures = kGlinkFeatureIntentReuse;
static bool sGlinkUp;
static glink_channel sChannels[kGlinkMaxChannels];
static int sChannelCount;
static volatile bool sQuit;


static void
glink_kick()
{
	ipcc_signal(3, 0);
}


static bool
glink_rx_fifo()
{
	if (sRx.fifo != NULL)
		return true;
	uint32 size = 0;
	sRx.fifo = smem_item(kHostApps, kHostAdsp, 480, &size);
	sRx.length = size;
	return sRx.fifo != NULL && size > 0;
}


static uint32
rx_avail()
{
	if (!glink_rx_fifo())
		return 0;
	uint32 head = *sRx.head, tail = *sRx.tail;
	if (head >= sRx.length || tail >= sRx.length)
		return 0;
	return head >= tail ? head - tail : sRx.length - tail + head;
}


static void
rx_peek(uint32 offset, void* buffer, uint32 count)
{
	uint32 tail = *sRx.tail + offset;
	if (tail >= sRx.length)
		tail -= sRx.length;
	uint8* out = (uint8*)buffer;
	for (uint32 i = 0; i < count; i++) {
		out[i] = sRx.fifo[tail];
		if (++tail == sRx.length)
			tail = 0;
	}
}


static void
rx_advance(uint32 count)
{
	uint32 tail = *sRx.tail + count;
	if (tail >= sRx.length)
		tail -= sRx.length;
	memory_full_barrier();
	*sRx.tail = tail;
	memory_full_barrier();
}


static uint32
tx_avail()
{
	uint32 head = *sTx.head, tail = *sTx.tail;
	uint32 avail = tail <= head ? sTx.length - head + tail : tail - head;
	return avail < kGlinkFifoReserve ? 0 : avail - kGlinkFifoReserve;
}


static uint32
tx_write_one(uint32 head, const void* data, uint32 count)
{
	const uint8* in = (const uint8*)data;
	for (uint32 i = 0; i < count; i++) {
		sTx.fifo[head] = in[i];
		if (++head == sTx.length)
			head = 0;
	}
	return head;
}


//!	Puts a message (its header, then \a data) into our FIFO, and rings.
static status_t
glink_send(const void* header, uint32 headerLength, const void* data = NULL,
	uint32 dataLength = 0)
{
	uint32 total = (headerLength + dataLength + 7) & ~7u;
	for (bigtime_t deadline = system_time() + 1000000; tx_avail() < total;
			snooze(1000)) {
		if (system_time() > deadline) {
			ERROR("glink: no room for %" B_PRIu32 " bytes\n", total);
			return B_TIMED_OUT;
		}
	}
	uint32 head = *sTx.head;
	head = tx_write_one(head, header, headerLength);
	if (dataLength > 0)
		head = tx_write_one(head, data, dataLength);
	head = (head + 7) & ~7u;
	if (head >= sTx.length)
		head -= sTx.length;
	memory_full_barrier();
	*sTx.head = head;
	memory_full_barrier();
	glink_kick();
	return B_OK;
}


static status_t
glink_send_command(uint16 command, uint16 param1, uint32 param2)
{
	glink_message message = { command, param1, param2 };
	return glink_send(&message, sizeof(message));
}


static glink_channel*
glink_channel_by_name(const char* name)
{
	for (int i = 0; i < sChannelCount; i++) {
		if (strcmp(sChannels[i].name, name) == 0)
			return &sChannels[i];
	}
	return NULL;
}


static glink_channel*
glink_channel_by_remote_id(uint16 id)
{
	for (int i = 0; i < sChannelCount; i++) {
		if (sChannels[i].remoteId == id)
			return &sChannels[i];
	}
	return NULL;
}


static glink_channel*
glink_channel_by_local_id(uint16 id)
{
	for (int i = 0; i < sChannelCount; i++) {
		if (sChannels[i].localId == id)
			return &sChannels[i];
	}
	return NULL;
}


static glink_channel*
glink_add_channel(const char* name)
{
	if (sChannelCount == kGlinkMaxChannels)
		return NULL;
	glink_channel* channel = &sChannels[sChannelCount];
	memset(channel, 0, sizeof(*channel));
	strlcpy(channel->name, name, sizeof(channel->name));
	channel->localId = ++sChannelCount;
	channel->nextIntentId = 1;
	return channel;
}


/*!	Our open of a channel the ADSP opened (qcom_glink_create_remote()): our
	acknowledgement of its open, then our own open. */
static void
glink_open(glink_channel* channel)
{
	glink_send_command(kGlinkOpenAck, channel->remoteId, 0);
	struct {
		glink_message	message;
		char			name[32];
	} request = {};
	uint32 nameLength = strlen(channel->name) + 1;
	request.message = { kGlinkOpen, channel->localId, nameLength };
	memcpy(request.name, channel->name, nameLength);
	glink_send(&request, sizeof(glink_message) + nameLength);
	channel->openSent = true;
}


static void
glink_announce_intents(glink_channel* channel, uint32 size, uint32 count)
{
	for (uint32 i = 0; i < count; i++) {
		struct {
			glink_message	message;
			uint32			size;
			uint32			id;
		} intent = { { kGlinkIntent, channel->localId, 1 }, size,
			channel->nextIntentId++ };
		glink_send(&intent, sizeof(intent));
	}
}


/*!	Sends \a data on \a channel, in an intent of the ADSP's that is big
	enough. Returns B_WOULD_BLOCK when it has none (yet), after asking it
	for one. */
static status_t
glink_write(glink_channel* channel, const void* data, uint32 length)
{
	if (channel->remoteId == 0 || !channel->openAcked)
		return B_NOT_ALLOWED;
	glink_remote_intent* intent = NULL;
	for (uint32 i = 0; i < channel->remoteIntentCount; i++) {
		glink_remote_intent& candidate = channel->remoteIntents[i];
		if (!candidate.inUse && candidate.size >= length
			&& (intent == NULL || candidate.size < intent->size))
			intent = &candidate;
	}
	if (intent == NULL) {
		// one request until it is answered (or seems lost)
		if (system_time() - channel->intentRequested > 500000) {
			channel->intentRequested = system_time();
			glink_send_command(kGlinkRxIntentRequest, channel->localId, length);
		}
		return B_WOULD_BLOCK;
	}
	intent->inUse = true;
	struct {
		glink_message	message;
		uint32			chunkSize;
		uint32			leftSize;
	} header = { { kGlinkTxData, channel->localId, intent->id }, length, 0 };
	return glink_send(&header, sizeof(header), data, length);
}


static void
glink_receive_open(uint16 remoteId, const char* name)
{
	glink_channel* channel = glink_channel_by_name(name);
	if (channel == NULL)
		channel = glink_add_channel(name);
	if (channel == NULL)
		return;
	channel->remoteId = remoteId;
	TRACE("glink: the ADSP opened \"%s\" (%u)%s\n", name, remoteId,
		channel->wanted ? "" : ", not used");
	if (channel->wanted)
		glink_open(channel);
}


static void
glink_receive_data(const glink_message& message, uint32 chunkSize,
	uint32 leftSize, uint32 offset)
{
	glink_channel* channel = glink_channel_by_remote_id(message.param1);
	if (channel == NULL || channel->hook == NULL)
		return;
	if (channel->rxBuffer == NULL)
		channel->rxBuffer = (uint8*)malloc(kGlinkMaxMessage);
	if (channel->rxBuffer == NULL
		|| channel->rxLength + chunkSize > kGlinkMaxMessage) {
		ERROR("glink: a message on \"%s\" too big\n", channel->name);
		channel->rxLength = 0;
		return;
	}
	if (message.command == kGlinkTxData)
		channel->rxLength = 0;
	rx_peek(offset, channel->rxBuffer + channel->rxLength, chunkSize);
	channel->rxLength += chunkSize;
	if (leftSize != 0)
		return;
	uint32 length = channel->rxLength;
	channel->rxLength = 0;
	// our intents are reused (ours are only the ones announced at the open)
	glink_send_command(kGlinkRxDoneReuse, channel->localId, message.param2);
	channel->hook(channel, channel->rxBuffer, length);
}


//!	Takes what the ADSP sent (qcom_glink_native_rx()).
static void
glink_poll()
{
	while (true) {
		uint32 avail = rx_avail();
		if (avail < sizeof(glink_message))
			return;
		glink_message message;
		rx_peek(0, &message, sizeof(message));
		uint32 consumed = sizeof(message);
		switch (message.command) {
			case kGlinkVersion:
				if (message.param1 == 0)
					break;
				if (message.param1 == kGlinkVersion1)
					sGlinkFeatures &= message.param2;
				glink_send_command(kGlinkVersionAck, kGlinkVersion1,
					sGlinkFeatures);
				break;
			case kGlinkVersionAck:
				if (message.param1 == 0)
					break;
				if (message.param1 == kGlinkVersion1
					&& message.param2 == sGlinkFeatures) {
					if (!sGlinkUp)
						TRACE("glink: up, features %#" B_PRIx32 "\n",
							sGlinkFeatures);
					sGlinkUp = true;
					break;
				}
				sGlinkFeatures &= message.param2;
				glink_send_command(kGlinkVersion, kGlinkVersion1,
					sGlinkFeatures);
				break;
			case kGlinkOpen:
			{
				consumed += message.param2;
				if (avail < consumed)
					return;
				char name[33] = {};
				rx_peek(sizeof(message), name, min_c(message.param2, 32u));
				glink_receive_open(message.param1, name);
				break;
			}
			case kGlinkClose:
			{
				glink_channel* channel
					= glink_channel_by_remote_id(message.param1);
				TRACE("glink: the ADSP closed \"%s\"\n",
					channel != NULL ? channel->name : "?");
				if (channel != NULL) {
					channel->remoteId = 0;
					channel->openSent = channel->openAcked = false;
					channel->remoteIntentCount = 0;
				}
				glink_send_command(kGlinkCloseAck, message.param1, 0);
				break;
			}
			case kGlinkOpenAck:
			{
				glink_channel* channel
					= glink_channel_by_local_id(message.param1);
				if (channel != NULL && !channel->openAcked) {
					channel->openAcked = true;
					TRACE("glink: \"%s\" open\n", channel->name);
					glink_announce_intents(channel, kGlinkIntentSize,
						kGlinkIntentCount);
				}
				break;
			}
			case kGlinkIntent:
			{
				consumed += 8 * message.param2;
				if (avail < consumed)
					return;
				glink_channel* channel
					= glink_channel_by_remote_id(message.param1);
				for (uint32 i = 0; i < message.param2; i++) {
					uint32 pair[2];
					rx_peek(sizeof(message) + 8 * i, pair, sizeof(pair));
					if (channel == NULL)
						continue;
					glink_remote_intent* slot = NULL;
					if (channel->remoteIntentCount
							< (uint32)kGlinkMaxRemoteIntents) {
						slot = &channel->remoteIntents[
							channel->remoteIntentCount++];
					} else {
						// the ADSP hands out many small ones: a bigger one
						// takes the place of the smallest free one (which
						// stays the ADSP's, unused)
						for (uint32 j = 0; j < channel->remoteIntentCount;
								j++) {
							glink_remote_intent& candidate
								= channel->remoteIntents[j];
							if (!candidate.inUse && candidate.size < pair[0]
								&& (slot == NULL
									|| candidate.size < slot->size))
								slot = &candidate;
						}
					}
					if (slot == NULL)
						continue;
					*slot = { pair[1], pair[0], false };
					if (channel->intentRequested != 0) {
						TRACE("glink: %s: an intent of %" B_PRIu32 " bytes\n",
							channel->name, pair[0]);
						channel->intentRequested = 0;
					}
				}
				break;
			}
			case kGlinkRxDone:
			case kGlinkRxDoneReuse:
			{
				glink_channel* channel
					= glink_channel_by_remote_id(message.param1);
				if (channel == NULL)
					break;
				for (uint32 i = 0; i < channel->remoteIntentCount; i++) {
					glink_remote_intent& intent = channel->remoteIntents[i];
					if (intent.id != message.param2)
						continue;
					if (message.command == kGlinkRxDoneReuse)
						intent.inUse = false;
					else {
						intent = channel->remoteIntents[
							--channel->remoteIntentCount];
					}
					break;
				}
				break;
			}
			case kGlinkRxIntentRequest:
			{
				glink_channel* channel
					= glink_channel_by_remote_id(message.param1);
				if (channel == NULL)
					break;
				bool granted = message.param2 <= kGlinkMaxMessage;
				if (granted)
					glink_announce_intents(channel, message.param2, 1);
				glink_send_command(kGlinkRxIntentRequestAck, channel->localId,
					granted ? 1 : 0);
				break;
			}
			case kGlinkTxData:
			case kGlinkTxDataContinued:
			{
				if (avail < 16)
					return;
				uint32 sizes[2];
				rx_peek(sizeof(message), sizes, sizeof(sizes));
				consumed = 16 + sizes[0];
				if (avail < consumed)
					return;
				glink_receive_data(message, sizes[0], sizes[1], 16);
				break;
			}
			case kGlinkReadNotify:
				glink_kick();
				break;
			case kGlinkRxIntentRequestAck:
			{
				glink_channel* channel
					= glink_channel_by_remote_id(message.param1);
				if (message.param2 == 0) {
					ERROR("glink: %s: the ADSP refused an intent\n",
						channel != NULL ? channel->name : "?");
				}
				break;
			}
			case kGlinkCloseAck:
			case kGlinkSignals:
				break;
			default:
				ERROR("glink: unknown command %u, the rest dropped\n",
					message.command);
				consumed = avail;
				break;
		}
		rx_advance((consumed + 7) & ~7u);
	}
}


static status_t
glink_start()
{
	uint32 size = 0;
	volatile uint32* descriptor = (volatile uint32*)smem_allocate(kHostApps,
		kHostAdsp, 478, 32);
	volatile uint8* fifo = smem_allocate(kHostApps, kHostAdsp, 479, 16384);
	if (descriptor == NULL || fifo == NULL
		|| smem_item(kHostApps, kHostAdsp, 479, &size) == NULL)
		return B_NO_MEMORY;
	sTx = { &descriptor[0], &descriptor[1], fifo, size };
	sRx = { &descriptor[2], &descriptor[3], NULL, 0 };
	*sRx.tail = 0;
	*sTx.head = 0;
	memory_full_barrier();
	return glink_send_command(kGlinkVersion, kGlinkVersion1, sGlinkFeatures);
}


// #pragma mark - pmic_glink


/*	The PMIC's services on the ADSP, on the GLINK channel
	"PMIC_RTR_ADSP_APPS" (Linux' pmic_glink.c, pmic_glink_altmode.c):
	messages of a header {owner, type, opcode}. The USB-C ports' alternate
	modes are owner 32780: once enabled with a request (ALTMODE_PAN_EN),
	the ADSP reports each port's state (cable orientation, mux, the
	DisplayPort pin assignment and HPD), each report acknowledged
	(ALTMODE_PAN_ACK). */

static const uint32 kPmicGlinkOwnerAltmode = 32780;	// USBC_PAN
static const uint32 kPmicGlinkRequest = 1;
static const uint32 kUsbcWriteRequest = 0x15;
static const uint32 kUsbcNotify = 0x16;
static const uint32 kAltmodeEnable = 0x10;
static const uint32 kAltmodeAck = 0x11;

struct pmic_glink_header {
	uint32	owner;
	uint32	type;
	uint32	opcode;
};

struct usbc_write_request {
	pmic_glink_header header;
	uint32	command;
	uint32	argument;
	uint32	reserved;
};

// USBC_NOTIFY_IND (pmic_glink_altmode_sc8280xp_notify()): the SVID is in
// the opcode's top half
struct usbc_notify {
	pmic_glink_header header;
	uint8	port;
	uint8	orientation;
	uint8	mux;
	uint8	reserved1[5];
	uint8	pinAssignmentHpd;	// DPAM (A = 1) 5:0, HPD 6, HPD IRQ 7
	uint8	reserved2[7];
	uint32	reserved3;
} _PACKED;

static glink_channel* sPmicGlink;
static bool sAltmodeEnabled;

// the ports' last reports, for the display driver (qcom_usbc_ports.h)
static qcom_usbc_port sPorts[QCOM_USBC_PORT_COUNT];
static spinlock sPortsLock = B_SPINLOCK_INITIALIZER;
// lab: a DisplayPort sink reported on this port, then gone ("simulate_dp_report")
static int32 sSimulatedPort = -1;
static bool sSimulating;
static bigtime_t sAltmodeRequested;


static status_t
altmode_request(uint32 command, uint32 argument)
{
	usbc_write_request request = {
		{ kPmicGlinkOwnerAltmode, kPmicGlinkRequest, kUsbcWriteRequest },
		command, argument, 0 };
	return glink_write(sPmicGlink, &request, sizeof(request));
}


static void
pmic_glink_receive(glink_channel* channel, const uint8* data, uint32 length)
{
	pmic_glink_header header;
	if (length < sizeof(header))
		return;
	memcpy(&header, data, sizeof(header));
	uint32 opcode = header.opcode & 0xff;
	if (header.owner != kPmicGlinkOwnerAltmode) {
		TRACE("pmic_glink: owner %" B_PRIu32 " type %" B_PRIu32 " opcode %#"
			B_PRIx32 ", %" B_PRIu32 " bytes\n", header.owner, header.type,
			header.opcode, length);
		return;
	}
	if (opcode == kUsbcWriteRequest) {
		if (!sAltmodeEnabled)
			TRACE("pmic_glink: alternate mode reports on\n");
		sAltmodeEnabled = true;
		return;
	}
	if (opcode != kUsbcNotify || length != sizeof(usbc_notify)) {
		TRACE("pmic_glink: alternate mode opcode %#" B_PRIx32 ", %" B_PRIu32
			" bytes\n", header.opcode, length);
		return;
	}
	usbc_notify notify;
	memcpy(&notify, data, sizeof(notify));
	uint16 svid = header.opcode >> 16;
	const char* mode = svid == 0xff01 ? "DisplayPort"
		: notify.mux == 4 ? "tunnelling" : notify.mux == 0 ? "nothing"
		: "USB";
	TRACE("usb-c port %u: %s, orientation %u, mux %u, svid %#x, pin "
		"assignment %u, hpd %u%s%s\n", notify.port, mode, notify.orientation,
		notify.mux, svid, notify.pinAssignmentHpd & 0x3f,
		(notify.pinAssignmentHpd >> 6) & 1,
		(notify.pinAssignmentHpd & 0x80) != 0 ? " (irq)" : "",
		sSimulating ? " (simulated)" : "");
	if (notify.port < QCOM_USBC_PORT_COUNT) {
		cpu_status state = disable_interrupts();
		acquire_spinlock(&sPortsLock);
		qcom_usbc_port& port = sPorts[notify.port];
		port.generation++;
		port.display_port = svid == 0xff01;
		port.hpd = (notify.pinAssignmentHpd >> 6) & 1;
		port.hpd_irq = (notify.pinAssignmentHpd >> 7) & 1;
		port.pin_assignment = notify.pinAssignmentHpd & 0x3f;
		port.orientation = notify.orientation;
		port.mux = notify.mux;
		release_spinlock(&sPortsLock);
		restore_interrupts(state);
	}
	if (!sSimulating)
		altmode_request(kAltmodeAck, notify.port);
}


/*!	Lab: a DisplayPort sink on port sSimulatedPort 20 s after the reports
	are on, gone 30 s later, through the same path as the ADSP's reports
	(without acknowledging them to it). */
static void
simulate_report(bool connected)
{
	usbc_notify notify = {};
	notify.header = { kPmicGlinkOwnerAltmode, 2,
		(connected ? 0xff01u << 16 : 0) | kUsbcNotify };
	notify.pinAssignmentHpd = connected ? (1u << 6 | 3) : 0;
	notify.port = sSimulatedPort;
	notify.mux = connected ? 2 : 0;
	sSimulating = true;
	pmic_glink_receive(sPmicGlink, (const uint8*)&notify, sizeof(notify));
	sSimulating = false;
}


static void
simulation_tick()
{
	static bigtime_t sStart;
	static int sStep;
	if (sSimulatedPort < 0 || !sAltmodeEnabled || sStep == 2)
		return;
	if (sStart == 0)
		sStart = system_time();
	bigtime_t elapsed = system_time() - sStart;
	if (sStep == 0 && elapsed > 20000000) {
		simulate_report(true);
		sStep = 1;
	} else if (sStep == 1 && elapsed > 50000000) {
		simulate_report(false);
		sStep = 2;
	}
}


static status_t
usbc_get_port(uint32 index, qcom_usbc_port* port)
{
	if (index >= QCOM_USBC_PORT_COUNT || port == NULL)
		return B_BAD_VALUE;
	cpu_status state = disable_interrupts();
	acquire_spinlock(&sPortsLock);
	*port = sPorts[index];
	release_spinlock(&sPortsLock);
	restore_interrupts(state);
	return B_OK;
}


static status_t
usbc_std_ops(int32 op, ...)
{
	return op == B_MODULE_INIT || op == B_MODULE_UNINIT ? B_OK : B_ERROR;
}


static qcom_usbc_ports_module_info sUsbcPortsModule = {
	{ QCOM_USBC_PORTS_MODULE_NAME, 0, usbc_std_ops },
	usbc_get_port
};


//!	Turns the reports on, again and again until the ADSP answers.
static void
pmic_glink_tick()
{
	if (sPmicGlink == NULL || !sPmicGlink->openAcked || sAltmodeEnabled)
		return;
	if (system_time() - sAltmodeRequested < 2000000)
		return;
	if (altmode_request(kAltmodeEnable, 0) != B_WOULD_BLOCK)
		sAltmodeRequested = system_time();
}


// #pragma mark - GPR (the audio DSP's AudioReach)


/*	The ADSP's audio framework (AudioReach: the Audio Processing Manager,
	"APM", and its modules) is reached through GPR packets on the GLINK
	channel "adsp_apps" (Linux' drivers/soc/qcom/apr.c, gpr): a 24 byte
	header (version, header size in words, packet size; destination and
	source domain; ports, token, opcode), then the payload. qcom_adsp asks
	whether the framework is up (APM_CMD_GET_SPF_STATE, as
	q6apm_is_adsp_ready()); the audio driver does the rest through the
	qcom_gpr module (qcom_gpr.h), each of its ports with its hook. */

static const uint32 kGprDomainAdsp = 2, kGprDomainApps = 3;
static const uint32 kGprApmPort = 1;			// APM_MODULE_INSTANCE_ID
static const uint32 kGprClientPort = 1;			// q6apm's service
static const uint32 kApmGetSpfState = 0x01001021;
static const uint32 kApmGetSpfStateResponse = 0x02001007;
static const uint32 kGprMaxPacket = 4096;

static glink_channel* sGpr;
static bool sAudioReady;
static bigtime_t sAudioRequested;

// GLINK is run by the start thread; the audio driver's sends come from its
// own threads (and from its hooks, on the start thread)
static recursive_lock sGlinkLock = RECURSIVE_LOCK_INITIALIZER("qcom adsp glink");

static const int kGprMaxPorts = 8;
static struct {
	uint32			port;
	qcom_gpr_hook	hook;
	void*			cookie;
} sGprPorts[kGprMaxPorts];


static status_t
gpr_send(uint32 sourcePort, uint32 destinationPort, uint32 opcode,
	uint32 token, const void* payload, uint32 length)
{
	if (sGpr == NULL)
		return B_NO_INIT;
	if (length > kGprMaxPacket - 24)
		return B_BAD_VALUE;
	uint32 small[32];
	uint32* packet = small;
	if (24 + length > sizeof(small)) {
		packet = (uint32*)malloc(24 + length);
		if (packet == NULL)
			return B_NO_MEMORY;
	}
	packet[0] = 0 | 6u << 4 | (24 + length) << 8;	// version, header, size
	packet[1] = kGprDomainAdsp | kGprDomainApps << 8;
	packet[2] = sourcePort;
	packet[3] = destinationPort;
	packet[4] = token;
	packet[5] = opcode;
	if (length > 0)
		memcpy(packet + 6, payload, length);
	status_t status = glink_write(sGpr, packet, 24 + length);
	if (packet != small)
		free(packet);
	return status;
}


static void
gpr_receive(glink_channel* channel, const uint8* data, uint32 length)
{
	if (length < 24)
		return;
	uint32 header[6];
	memcpy(header, data, sizeof(header));
	uint32 headerSize = (header[0] >> 4 & 0xf) * 4;
	uint32 packetSize = header[0] >> 8;
	if (headerSize < 24 || packetSize > length || packetSize < headerSize)
		return;
	uint32 opcode = header[5];
	for (int i = 0; i < kGprMaxPorts; i++) {
		if (sGprPorts[i].hook == NULL || sGprPorts[i].port != header[3])
			continue;
		qcom_gpr_packet packet = { header[2], header[3], header[4], opcode,
			data + headerSize, packetSize - headerSize };
		sGprPorts[i].hook(sGprPorts[i].cookie, &packet);
		return;
	}
	uint32 result = 0;
	if (packetSize >= 28)
		memcpy(&result, data + 24, 4);
	if (opcode == kApmGetSpfStateResponse) {
		if (!sAudioReady || result != 1) {
			TRACE("audio: the ADSP's AudioReach framework is %s (%" B_PRIu32
				")\n", result == 1 ? "ready" : "not ready", result);
		}
		sAudioReady = result == 1;
		return;
	}
	// a basic result: the command's opcode, then its status
	uint32 status = 0;
	if (packetSize >= 32)
		memcpy(&status, data + 28, 4);
	TRACE("audio: GPR opcode %#" B_PRIx32 " from port %#" B_PRIx32 " to %#"
		B_PRIx32 ", %" B_PRIu32 " bytes: %#" B_PRIx32 ", status %#" B_PRIx32
		"\n", opcode, header[2], header[3], packetSize, result, status);
}


//!	Asks whether the audio framework is up until it says it is.
static void
gpr_tick()
{
	if (sGpr == NULL || !sGpr->openAcked || sAudioReady)
		return;
	if (system_time() - sAudioRequested < 1000000)
		return;
	if (gpr_send(kGprClientPort, kGprApmPort, kApmGetSpfState, 1, NULL, 0)
			!= B_WOULD_BLOCK)
		sAudioRequested = system_time();
}


static bool
gpr_module_ready()
{
	return sAudioReady;
}


static status_t
gpr_module_register_port(uint32 port, qcom_gpr_hook hook, void* cookie)
{
	if (port < 0x100 || hook == NULL)
		return B_BAD_VALUE;
	RecursiveLocker locker(sGlinkLock);
	int free = -1;
	for (int i = 0; i < kGprMaxPorts; i++) {
		if (sGprPorts[i].hook != NULL && sGprPorts[i].port == port)
			return B_NAME_IN_USE;
		if (sGprPorts[i].hook == NULL && free < 0)
			free = i;
	}
	if (free < 0)
		return B_NO_MEMORY;
	sGprPorts[free].port = port;
	sGprPorts[free].cookie = cookie;
	sGprPorts[free].hook = hook;
	return B_OK;
}


static void
gpr_module_unregister_port(uint32 port)
{
	RecursiveLocker locker(sGlinkLock);
	for (int i = 0; i < kGprMaxPorts; i++) {
		if (sGprPorts[i].hook != NULL && sGprPorts[i].port == port)
			sGprPorts[i].hook = NULL;
	}
}


static status_t
gpr_module_send(uint32 sourcePort, uint32 destinationPort, uint32 opcode,
	uint32 token, const void* payload, uint32 payloadSize)
{
	if (!sAudioReady)
		return B_NOT_INITIALIZED;
	status_t status;
	{
		RecursiveLocker locker(sGlinkLock);
		status = gpr_send(sourcePort, destinationPort, opcode, token, payload,
			payloadSize);
	}
	if (status == B_WOULD_BLOCK) {
		// an intent was asked for: the start thread takes it in
		if (sGlinkSem >= 0)
			release_sem_etc(sGlinkSem, 1, B_DO_NOT_RESCHEDULE);
		return B_BUSY;
	}
	return status;
}


static status_t
gpr_std_ops(int32 op, ...)
{
	return op == B_MODULE_INIT || op == B_MODULE_UNINIT ? B_OK : B_ERROR;
}


static qcom_gpr_module_info sGprModule = {
	{ QCOM_GPR_MODULE_NAME, 0, gpr_std_ops },
	gpr_module_ready,
	gpr_module_register_port,
	gpr_module_unregister_port,
	gpr_module_send
};


// #pragma mark - the start


enum {
	kStateFatal = 1 << 0,
	kStateReady = 1 << 1,
	kStateHandover = 1 << 2,
};


static uint8*
load_firmware(const char* name, size_t* _size)
{
	static const char* kDirectories[] = {
		"/boot/system/non-packaged/data/firmware",
		"/boot/system/data/firmware",
	};
	for (const char* directory : kDirectories) {
		char path[B_PATH_NAME_LENGTH];
		snprintf(path, sizeof(path), "%s/%s", directory, name);
		int fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		struct stat st;
		uint8* data = NULL;
		if (fstat(fd, &st) == 0 && st.st_size > 0)
			data = (uint8*)malloc(st.st_size);
		if (data != NULL && read(fd, data, st.st_size) != st.st_size) {
			free(data);
			data = NULL;
		}
		close(fd);
		if (data != NULL) {
			TRACE("firmware %s, %" B_PRIdOFF " bytes\n", path, st.st_size);
			*_size = st.st_size;
			return data;
		}
	}
	ERROR("firmware %s not found\n", name);
	return NULL;
}


static void
log_crash_reason()
{
	// crash_reason_smem 423, in the global partition
	uint32 size = 0;
	volatile uint8* reason = smem_item(kHostGlobal, kHostGlobal, 423, &size);
	if (reason == NULL || size == 0)
		return;
	char text[128] = {};
	for (uint32 i = 0; i < size && i < sizeof(text) - 1; i++)
		text[i] = reason[i];
	ERROR("crash reason: %s\n", text);
}


static status_t
adsp_start()
{
	size_t size;
	uint8* image = load_firmware(sFirmwareName, &size);
	if (image == NULL)
		return B_ENTRY_NOT_FOUND;

	status_t status = qmp_open();
	if (status == B_OK)
		status = qmp_send("{class: image, res: load_state, name: adsp, val: on}");
	if (status != B_OK) {
		free(image);
		return status;
	}
	status = smp2p_create();
	if (status != B_OK) {
		free(image);
		return status;
	}
	// the proxy votes, until the DSP has made its own
	arc_vote("lcx.lvl", -1);
	arc_vote("lmx.lvl", -1);

	bigtime_t start = system_time();
	status = qcom_pas_load("adsp", kAdspPasId, image, size, sRegionBase,
		sRegionSize);
	free(image);
	if (status == B_OK)
		status = qcom_pas_auth_and_reset("adsp", kAdspPasId);
	if (status != B_OK) {
		arc_vote("lcx.lvl", 0);
		arc_vote("lmx.lvl", 0);
		qmp_send("{class: image, res: load_state, name: adsp, val: off}");
		return status;
	}
	TRACE("authenticated and started in %" B_PRId64 " ms\n",
		(system_time() - start) / 1000);

	// its state, in the SMP2P entry it writes (item 443, "slave-kernel";
	// the device tree's "qcom,smem" names the inbound item first)
	uint32 state = 0;
	bool seen = false;
	int polls = 0;
	for (bigtime_t deadline = system_time() + 10000000;
			system_time() < deadline; snooze(10000)) {
		// (lab: does it wait for a doorbell after its start?)
		if (!seen && ++polls % 20 == 0)
			ipcc_signal(3, 2);
		uint32 value;
		if (smp2p_entry(443, "slave-kernel", &value)) {
			if (!seen || value != state) {
				TRACE("state %#" B_PRIx32 " after %" B_PRId64 " ms\n", value,
					(system_time() - start) / 1000);
			}
			seen = true;
			state = value;
			if ((state & (kStateFatal | kStateHandover)) != 0)
				break;
		}
	}
	if ((state & kStateFatal) != 0) {
		log_crash_reason();
		return B_ERROR;
	}
	if ((state & kStateHandover) != 0) {
		arc_vote("lcx.lvl", 0);
		arc_vote("lmx.lvl", 0);
	}
	if ((state & kStateReady) == 0) {
		smem_log_items(kHostApps, kHostAdsp);
		log_crash_reason();
		ERROR("not ready after 10 s (%s)\n", seen ? "it has an SMP2P entry"
			: "no SMP2P entry");
		return B_TIMED_OUT;
	}
	TRACE("ready\n");
	return B_OK;
}


static status_t
boot_thread(void*)
{
	if (adsp_start() != B_OK)
		return B_OK;

	void* settings = load_driver_settings("qcom_adsp");
	if (settings != NULL) {
		const char* port = get_driver_parameter(settings, "simulate_dp_report",
			NULL, NULL);
		if (port != NULL)
			sSimulatedPort = atoi(port);
		unload_driver_settings(settings);
	}

	sPmicGlink = glink_add_channel("PMIC_RTR_ADSP_APPS");
	sPmicGlink->wanted = true;
	sPmicGlink->hook = pmic_glink_receive;
	sGpr = glink_add_channel("adsp_apps");
	sGpr->wanted = true;
	sGpr->hook = gpr_receive;
	sGlinkSem = create_sem(0, "qcom adsp glink");
	if (sGlinkSem >= 0)
		ipcc_interrupt_start();
	TRACE("glink: %s\n", sIpccHandler ? "doorbell interrupt" : "polled");
	if (glink_start() != B_OK) {
		ERROR("glink: cannot start\n");
		return B_OK;
	}
	while (!sQuit) {
		recursive_lock_lock(&sGlinkLock);
		glink_poll();
		pmic_glink_tick();
		gpr_tick();
		simulation_tick();
		recursive_lock_unlock(&sGlinkLock);
		// woken by the doorbell; now and then anyway, for the requests
		// pmic_glink_tick() repeats
		if (sIpccHandler)
			acquire_sem_etc(sGlinkSem, 1, B_RELATIVE_TIMEOUT, 500000);
		else
			snooze(10000);
	}
	return B_OK;
}


// #pragma mark - driver


static float
qcom_adsp_supports_device(device_node* parent)
{
	const char* bus;
	const char* compatible;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "fdt") != 0
		|| sDeviceManager->get_attr_string(parent, "fdt/compatible",
			&compatible, false) != B_OK
		|| strcmp(compatible, "qcom,sc8280xp-adsp-pas") != 0) {
		return 0.0f;
	}
	return 0.8f;
}


static status_t
qcom_adsp_register_device(device_node* parent)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "Qualcomm audio DSP" } },
		{}
	};
	return sDeviceManager->register_node(parent, QCOM_ADSP_MODULE_NAME,
		attrs, NULL, NULL);
}


static void
unmap_all()
{
	sSmem.Unmap();
	sIpcc.Unmap();
	sQmp.Unmap();
	qcom_rpmh_uninit(sRpmh);
	sMutex.Unmap();
}


static status_t
qcom_adsp_init_driver(device_node* node, void** _cookie)
{
	if (gFDT == NULL)
		return B_DEVICE_NOT_FOUND;
	int adsp = fdt_compatible_node("qcom,sc8280xp-adsp-pas");
	if (adsp < 0)
		return B_DEVICE_NOT_FOUND;
	const char* firmware = (const char*)fdt_getprop(gFDT, adsp,
		"firmware-name", NULL);
	const fdt32_t* region = (const fdt32_t*)fdt_getprop(gFDT, adsp,
		"memory-region", NULL);
	int regionNode = region != NULL
		? fdt_node_offset_by_phandle(gFDT, fdt32_to_cpu(*region)) : -1;
	if (firmware == NULL || regionNode < 0
		|| !fdt_reg(regionNode, 0, &sRegionBase, &sRegionSize)) {
		ERROR("no firmware name or memory region in the device tree\n");
		return B_DEVICE_NOT_FOUND;
	}
	strlcpy(sFirmwareName, firmware, sizeof(sFirmwareName));

	status_t status = map_compatible(sSmem, "qcom adsp smem", "qcom,smem");
	if (status == B_OK)
		status = map_compatible(sIpcc, "qcom adsp ipcc", "qcom,ipcc");
	if (status == B_OK)
		status = map_compatible(sQmp, "qcom adsp aoss qmp", "qcom,aoss-qmp");
	if (status == B_OK)
		status = qcom_rpmh_init(sRpmh);
	if (status == B_OK)
		status = map_compatible(sMutex, "qcom adsp mutex", "qcom,tcsr-mutex");
	if (status != B_OK) {
		ERROR("cannot map its blocks: %s\n", strerror(status));
		unmap_all();
		return status;
	}

	// No crash dump mode after a crash: the board's TCSR download mode
	// cookie (the device tree's qcom,dload-mode = <&tcsr 0x13000>) cleared
	// as Linux' qcom_scm does at probe
	if (qcom_scm_clear_download_mode(0x1fc0000 + 0x13000) != B_OK)
		ERROR("cannot clear the download mode\n");

	sBootThread = spawn_kernel_thread(boot_thread, "qcom adsp start",
		B_NORMAL_PRIORITY, NULL);
	if (sBootThread >= 0)
		resume_thread(sBootThread);
	*_cookie = NULL;
	return B_OK;
}


static void
qcom_adsp_uninit_driver(void* cookie)
{
	sQuit = true;
	if (sGlinkSem >= 0)
		release_sem(sGlinkSem);
	if (sBootThread >= 0) {
		status_t result;
		wait_for_thread(sBootThread, &result);
		sBootThread = -1;
	}
	ipcc_interrupt_stop();
	if (sGlinkSem >= 0)
		delete_sem(sGlinkSem);
	sGlinkSem = -1;
	unmap_all();
}


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{}
};


static driver_module_info sQcomAdspDriver = {
	{
		QCOM_ADSP_MODULE_NAME,
		0,
		NULL
	},
	qcom_adsp_supports_device,
	qcom_adsp_register_device,
	qcom_adsp_init_driver,
	qcom_adsp_uninit_driver,
	NULL,
	NULL,
	NULL
};


module_info* modules[] = {
	(module_info*)&sQcomAdspDriver,
	(module_info*)&sUsbcPortsModule,
	(module_info*)&sGprModule,
	NULL
};
