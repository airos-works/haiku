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

#include <libfdt.h>

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
static register_window sRsc;
static register_window sCmdDb;
static register_window sMutex;
static uint32 sTcsOffset;
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


/*	The AOP's command DB: the RPMh resources, and the levels of the voltage
	arcs (Linux' cmd-db.c). */

struct cmd_db_rsc_header {
	uint16	slave_id;
	uint16	header_offset;
	uint16	data_offset;
	uint16	count;
	uint16	version;
	uint16	reserved[3];
};

struct cmd_db_entry {
	char	id[8];
	uint32	priority[2];
	uint32	address;
	uint16	length;
	uint16	offset;
};

struct cmd_db_header {
	uint32	version;
	uint8	magic[4];
	cmd_db_rsc_header resources[8];
	uint32	checksum;
	uint32	reserved;
	uint8	data[];
};


//!	The address of RPMh resource \a id and its number of levels, if an arc.
static uint32
cmd_db_lookup(const char* id, uint32* _levels)
{
	const cmd_db_header* header = (const cmd_db_header*)sCmdDb.base;
	static const uint8 kMagic[] = {0xdb, 0x30, 0x03, 0x0c};
	if (header == NULL || memcmp((const void*)header->magic, kMagic, 4) != 0)
		return 0;

	char query[8] = {};
	memcpy(query, id, strnlen(id, 8));
	for (int i = 0; i < 8 && header->resources[i].slave_id != 0; i++) {
		const cmd_db_rsc_header& rsc = header->resources[i];
		const cmd_db_entry* entry
			= (const cmd_db_entry*)(header->data + rsc.header_offset);
		for (int j = 0; j < rsc.count; j++, entry++) {
			if (memcmp(entry->id, query, 8) == 0) {
				if (_levels != NULL)
					*_levels = entry->length / 2;
				return entry->address;
			}
		}
	}
	return 0;
}


/*!	One active-mode request through the apps RSC's first active TCS (as
	rpmh-rsc's tcs_write()), waited for: \a count writes of \a data to
	\a addresses. */
static status_t
rpmh_write(const uint32* addresses, const uint32* data, int count)
{
	uint32 version = sRsc.Read(0);
	bool v3 = ((version >> 16) & 0xff) >= 3;
	// rpmh_rsc_reg_offset_ver_3_0 / _2_7
	const uint32 cmdSize = v3 ? 24 : 20;
	const uint32 irqStatus = 0x04, irqClear = 0x08;
	const uint32 waitForCompletion = v3 ? 0x20 : 0x10;
	const uint32 control = v3 ? 0x24 : 0x14, enable = v3 ? 0x2c : 0x1c;
	const uint32 msgId = v3 ? 0x34 : 0x30, addr = v3 ? 0x38 : 0x34;
	const uint32 dataRegister = v3 ? 0x3c : 0x38;
	// TCS 0, the first active one (qcom,tcs-config); the registers from
	// the TCS base (qcom,tcs-offset)
	const uint32 tcs = sTcsOffset;

	uint32 mask = 0;
	for (int i = 0; i < count; i++) {
		uint32 c = tcs + cmdSize * i;
		sRsc.Write(c + msgId, 8 | 1u << 16 | 1u << 8);	// write, response
		sRsc.Write(c + addr, addresses[i]);
		sRsc.Write(c + dataRegister, data[i]);
		mask |= 1u << i;
	}
	sRsc.Write(tcs + waitForCompletion, mask);
	sRsc.Write(tcs + enable, mask);
	// __tcs_set_trigger(): AMC mode, then the trigger
	uint32 value = sRsc.Read(tcs + control);
	value &= ~(1u << 24);
	sRsc.Write(tcs + control, value);
	value &= ~(1u << 16);
	sRsc.Write(tcs + control, value);
	value = 1u << 16;
	sRsc.Write(tcs + control, value);
	value |= 1u << 24;
	sRsc.Write(tcs + control, value);

	bool done = false;
	for (bigtime_t deadline = system_time() + 100000;
			system_time() < deadline;) {
		if ((sRsc.Read(sTcsOffset + irqStatus) & 1) != 0) {
			done = true;
			break;
		}
		spin(10);
	}
	sRsc.Write(sTcsOffset + irqClear, 1);
	sRsc.Write(tcs + control, 0);
	sRsc.Write(tcs + enable, 0);
	return done ? B_OK : B_TIMED_OUT;
}


//!	Votes the arc \a name to \a level, or to its top level when negative.
static status_t
arc_vote(const char* name, int32 level)
{
	uint32 levels = 0;
	uint32 address = cmd_db_lookup(name, &levels);
	if (address == 0 || levels == 0) {
		ERROR("no arc %s in the command DB\n", name);
		return B_DEVICE_NOT_FOUND;
	}
	uint32 value = level < 0 ? levels - 1 : (uint32)level;
	status_t status = rpmh_write(&address, &value, 1);
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


static void
ipcc_signal(uint32 client, uint32 signal)
{
	// IPCC_REG_SEND_ID: the client in 31:16, the signal in 15:0
	sIpcc.Write(0x0c, client << 16 | signal);
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
	adsp_start();
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
	sRsc.Unmap();
	sCmdDb.Unmap();
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

	int rsc = fdt_compatible_node("qcom,rpmh-rsc");
	const fdt32_t* drvId = rsc >= 0 ? (const fdt32_t*)fdt_getprop(gFDT, rsc,
		"qcom,drv-id", NULL) : NULL;
	const fdt32_t* tcsOffset = rsc >= 0 ? (const fdt32_t*)fdt_getprop(gFDT,
		rsc, "qcom,tcs-offset", NULL) : NULL;
	if (drvId == NULL || tcsOffset == NULL) {
		ERROR("no apps RSC in the device tree\n");
		return B_DEVICE_NOT_FOUND;
	}
	sTcsOffset = fdt32_to_cpu(*tcsOffset);

	status_t status = map_compatible(sSmem, "qcom adsp smem", "qcom,smem");
	if (status == B_OK)
		status = map_compatible(sIpcc, "qcom adsp ipcc", "qcom,ipcc");
	if (status == B_OK)
		status = map_compatible(sQmp, "qcom adsp aoss qmp", "qcom,aoss-qmp");
	if (status == B_OK) {
		status = map_compatible(sRsc, "qcom adsp apps rsc", "qcom,rpmh-rsc",
			fdt32_to_cpu(*drvId));
	}
	if (status == B_OK)
		status = map_compatible(sCmdDb, "qcom adsp cmd-db", "qcom,cmd-db");
	if (status == B_OK)
		status = map_compatible(sMutex, "qcom adsp mutex", "qcom,tcsr-mutex");
	if (status != B_OK) {
		ERROR("cannot map its blocks: %s\n", strerror(status));
		unmap_all();
		return status;
	}

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
	if (sBootThread >= 0) {
		status_t result;
		wait_for_thread(sBootThread, &result);
		sBootThread = -1;
	}
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
	NULL
};
