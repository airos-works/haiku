/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The power key of Qualcomm SoCs' PMICs ("qcom,pmk8350-pwrkey": the PON
	block of the SC8280XP's PMK8280; the Radxa Dragon Q8B's power button),
	published as /dev/power/button/power in acpi_button's form: a read gives
	1 once after a press, and select() reports one; power_daemon then shuts
	the system down.

	The PMIC is on SPMI. Its registers are read through the SPMI PMIC
	arbiter, v5 on the SC8280XP, or v7 (Linux' spmi-pmic-arb.c): the
	arbiter's table maps the
	peripheral (SID 0, PON at 0x1300) to a channel ("APID"), whose observer
	channel of our execution environment reads it. The key's state is bit 7
	of PON's real time interrupt status (PON_GEN3_KPDPWR_N_SET), polled; the
	PMIC's interrupts are not needed for that. */


#include <stdio.h>
#include <string.h>

#include <Drivers.h>
#include <KernelExport.h>
#include <arch/atomic.h>
#include <fs/select_sync_pool.h>
#include <lock.h>
#include <util/AutoLock.h>

#include <libfdt.h>


extern void* gFDT;

int32 api_version = B_CUR_DRIVER_API_VERSION;

#define TRACE(x...) dprintf("qcom_pwrkey: " x)

// spmi-pmic-arb.c, v5 and v7
static const uint32 kArbVersion = 0x0000;
static const uint32 kArbFeatures = 0x0004;
static const uint32 kChannelCommand = 0x00;
static const uint32 kChannelStatus = 0x08;
static const uint32 kChannelReadData0 = 0x18;
static const uint32 kOpExtendedReadLong = 1;
static const uint32 kStatusDone = 1u << 0;
static const uint32 kStatusErrors = 7u << 1;	// failure, denied, dropped

// the PON peripheral (qcom,pmk8350-pon "hlos" registers)
static const uint8 kPonSid = 0;
static const uint16 kPon = 0x1300;
static const uint16 kPonType = 0x04;
static const uint16 kPonSubtype = 0x05;
static const uint16 kPonRealTimeStatus = 0x10;
static const uint8 kPowerKeyPressed = 1u << 7;

static const char* sDeviceNames[] = { "power/button/power", NULL };

static area_id sCoreArea = -1, sObserverArea = -1, sConfigArea = -1;
static volatile uint8* sCore;
static volatile uint8* sObserver;
static volatile uint8* sConfig;
static uint32 sEe;
// the version's layout: the APID table (core), the owners (cnfg), the
// observer channels
static uint32 sApidMap, sApidOwner, sObserverEeSize, sObserverChannelSize;
static uint32 sMaxApids;
static int32 sPonApid = -1;
static mutex sLock = MUTEX_INITIALIZER("qcom pwrkey");
static thread_id sThread = -1;
static volatile bool sQuit;
static uint8 sPressed;
static select_sync_pool* sSelectPool;


static inline uint32
rd(volatile uint8* base, uint32 offset)
{
	return *(volatile uint32*)(base + offset);
}


static inline void
wr(volatile uint8* base, uint32 offset, uint32 value)
{
	*(volatile uint32*)(base + offset) = value;
	memory_full_barrier();
}


static bool
fdt_reg_named(int node, const char* name, phys_addr_t* _base, size_t* _size)
{
	int namesLength, regLength;
	const char* names = (const char*)fdt_getprop(gFDT, node, "reg-names",
		&namesLength);
	const fdt32_t* reg = (const fdt32_t*)fdt_getprop(gFDT, node, "reg",
		&regLength);
	if (names == NULL || reg == NULL)
		return false;
	int index = 0;
	for (const char* p = names; p < names + namesLength;
			p += strlen(p) + 1, index++) {
		if (strcmp(p, name) != 0)
			continue;
		if (regLength < (index + 1) * 16)
			return false;
		const fdt32_t* entry = reg + index * 4;
		*_base = (uint64)fdt32_to_cpu(entry[0]) << 32 | fdt32_to_cpu(entry[1]);
		*_size = (uint64)fdt32_to_cpu(entry[2]) << 32 | fdt32_to_cpu(entry[3]);
		return true;
	}
	return false;
}


/*!	The channel of peripheral \a ppid (SID << 8 | peripheral) for our
	execution environment (pmic_arb_read_apid_map_v5()): a peripheral may
	have several, one per owner; any can be read through. */
static int32
spmi_apid(uint16 ppid)
{
	uint32 count = rd(sCore, kArbFeatures) & 0x7ff;
	int32 found = -1;
	for (uint32 apid = 0; apid < count && apid < sMaxApids; apid++) {
		uint32 map = rd(sCore, sApidMap + 4 * apid);
		if (map == 0 || ((map >> 8) & 0xfff) != ppid)
			continue;
		uint32 owner = rd(sConfig, sApidOwner + 4 * apid) & 7;
		if (found < 0 || owner == sEe)
			found = apid;
	}
	return found;
}


//!	Reads \a count (1..4) registers of the peripheral on channel \a apid.
static status_t
spmi_read(int32 apid, uint8 sid, uint16 address, uint8* data, uint32 count)
{
	volatile uint8* channel = sObserver + sObserverChannelSize * apid;
	wr(channel, kChannelCommand, kOpExtendedReadLong << 27
		| (address & 0xff) << 4 | ((count - 1) & 7));
	for (int i = 0; i < 1000; i++) {
		uint32 status = rd(channel, kChannelStatus);
		if ((status & kStatusDone) != 0) {
			if ((status & kStatusErrors) != 0)
				return B_IO_ERROR;
			uint32 value = rd(channel, kChannelReadData0);
			for (uint32 b = 0; b < count; b++)
				data[b] = value >> (8 * b);
			return B_OK;
		}
		spin(1);
	}
	return B_TIMED_OUT;
}


static status_t
poll_thread(void*)
{
	bool down = false;
	while (!sQuit) {
		uint8 status;
		if (spmi_read(sPonApid, kPonSid, kPon + kPonRealTimeStatus, &status, 1)
				== B_OK) {
			bool pressed = (status & kPowerKeyPressed) != 0;
			if (pressed && !down) {
				TRACE("pressed\n");
				MutexLocker locker(sLock);
				sPressed = 1;
				if (sSelectPool != NULL)
					notify_select_event_pool(sSelectPool, B_SELECT_READ);
			}
			down = pressed;
		}
		snooze(100000);
	}
	return B_OK;
}


// #pragma mark - device


static status_t
pwrkey_open(const char* name, uint32 flags, void** _cookie)
{
	*_cookie = NULL;
	return B_OK;
}


static status_t
pwrkey_close(void* cookie)
{
	return B_OK;
}


static status_t
pwrkey_free(void* cookie)
{
	return B_OK;
}


static status_t
pwrkey_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	return B_DEV_INVALID_IOCTL;
}


static status_t
pwrkey_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	if (*_length < 1)
		return B_IO_ERROR;
	MutexLocker locker(sLock);
	if (user_memcpy(buffer, &sPressed, 1) != B_OK)
		return B_BAD_ADDRESS;
	sPressed = 0;
	*_length = 1;
	return B_OK;
}


static status_t
pwrkey_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	return B_NOT_ALLOWED;
}


static status_t
pwrkey_select(void* cookie, uint8 event, uint32 ref, selectsync* sync)
{
	if (event != B_SELECT_READ)
		return B_BAD_VALUE;
	MutexLocker locker(sLock);
	status_t status = add_select_sync_pool_entry(&sSelectPool, sync, event);
	if (status == B_OK && sPressed != 0)
		notify_select_event(sync, event);
	return status;
}


static status_t
pwrkey_deselect(void* cookie, uint8 event, selectsync* sync)
{
	if (event != B_SELECT_READ)
		return B_BAD_VALUE;
	MutexLocker locker(sLock);
	return remove_select_sync_pool_entry(&sSelectPool, sync, event);
}


static device_hooks sDeviceHooks = {
	pwrkey_open,
	pwrkey_close,
	pwrkey_free,
	pwrkey_control,
	pwrkey_read,
	pwrkey_write,
	pwrkey_select,
	pwrkey_deselect
};


// #pragma mark - driver


status_t
init_hardware()
{
	if (gFDT == NULL
		|| fdt_node_offset_by_compatible(gFDT, -1, "qcom,pmk8350-pwrkey") < 0
		|| fdt_node_offset_by_compatible(gFDT, -1, "qcom,spmi-pmic-arb") < 0)
		return B_DEVICE_NOT_FOUND;
	return B_OK;
}


static void
unmap_all()
{
	if (sCoreArea >= 0)
		delete_area(sCoreArea);
	if (sObserverArea >= 0)
		delete_area(sObserverArea);
	if (sConfigArea >= 0)
		delete_area(sConfigArea);
	sCoreArea = sObserverArea = sConfigArea = -1;
}


status_t
init_driver()
{
	if (init_hardware() != B_OK)
		return B_DEVICE_NOT_FOUND;
	int key = fdt_node_offset_by_compatible(gFDT, -1, "qcom,pmk8350-pwrkey");
	const char* status = (const char*)fdt_getprop(gFDT, key, "status", NULL);
	if (status != NULL && strcmp(status, "okay") != 0)
		return B_DEVICE_NOT_FOUND;
	int arb = fdt_node_offset_by_compatible(gFDT, -1, "qcom,spmi-pmic-arb");
	const fdt32_t* ee = (const fdt32_t*)fdt_getprop(gFDT, arb, "qcom,ee", NULL);
	sEe = ee != NULL ? fdt32_to_cpu(*ee) : 0;
	phys_addr_t base;
	size_t size;
	if (fdt_reg_named(arb, "core", &base, &size)) {
		sCoreArea = map_physical_memory("qcom spmi core", base, size,
			B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY, B_KERNEL_READ_AREA,
			(void**)&sCore);
	}
	uint32 version = sCoreArea >= 0 ? rd(sCore, kArbVersion) : 0;
	if (version >= 0x70000000) {
		sApidMap = 0x2000;
		sApidOwner = 0;
		sObserverEeSize = 0x8000;
		sObserverChannelSize = 0x20;
		sMaxApids = 1024;
	} else if (version >= 0x50000000) {
		sApidMap = 0x900;
		sApidOwner = 0x700;
		sObserverEeSize = 0x10000;
		sObserverChannelSize = 0x80;
		sMaxApids = 512;
	} else {
		TRACE("SPMI arbiter version %#" B_PRIx32 ", not v5 or v7\n", version);
		unmap_all();
		return B_DEVICE_NOT_FOUND;
	}
	// our execution environment's observer channels only
	if (fdt_reg_named(arb, "obsrvr", &base, &size)
		&& sObserverEeSize * (sEe + 1) <= size) {
		sObserverArea = map_physical_memory("qcom spmi observer",
			base + sObserverEeSize * sEe, sObserverEeSize,
			B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&sObserver);
	}
	if (fdt_reg_named(arb, "cnfg", &base, &size)) {
		sConfigArea = map_physical_memory("qcom spmi config", base, size,
			B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY, B_KERNEL_READ_AREA,
			(void**)&sConfig);
	}
	if (sCoreArea < 0 || sObserverArea < 0 || sConfigArea < 0) {
		unmap_all();
		return B_DEVICE_NOT_FOUND;
	}
	sPonApid = spmi_apid(kPonSid << 8 | kPon >> 8);
	uint8 type[2] = {};
	if (sPonApid < 0
		|| spmi_read(sPonApid, kPonSid, kPon + kPonType, type, 2) != B_OK) {
		TRACE("no PON channel (SPMI arbiter %#" B_PRIx32 ")\n", version);
		unmap_all();
		return B_DEVICE_NOT_FOUND;
	}
	uint8 state = 0;
	spmi_read(sPonApid, kPonSid, kPon + kPonRealTimeStatus, &state, 1);
	TRACE("SPMI arbiter %#" B_PRIx32 ", PON on channel %" B_PRId32 ": type %#x"
		" subtype %#x, status %#x\n", version, sPonApid, type[0], type[1],
		state);

	sQuit = false;
	sThread = spawn_kernel_thread(poll_thread, "qcom power key",
		B_LOW_PRIORITY, NULL);
	if (sThread < 0) {
		unmap_all();
		return sThread;
	}
	resume_thread(sThread);
	return B_OK;
}


void
uninit_driver()
{
	sQuit = true;
	if (sThread >= 0) {
		status_t result;
		wait_for_thread(sThread, &result);
		sThread = -1;
	}
	unmap_all();
}


const char**
publish_devices()
{
	return sDeviceNames;
}


device_hooks*
find_device(const char* name)
{
	return &sDeviceHooks;
}
