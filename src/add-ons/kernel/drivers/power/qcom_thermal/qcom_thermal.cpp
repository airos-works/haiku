/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The temperature sensors of Qualcomm SoCs with TSENS v2 (the SC8280XP of
	the Radxa Dragon Q8B), published as /dev/power/qcom_thermal/0 in the text
	form of the other thermal drivers (acpi_thermal, rpi_thermal): the
	hottest valid sensor, and the lowest critical trip of the device tree's
	thermal zones. The firmware has the sensors running; they are only
	read (Linux' tsens-v2: TM_Sn_STATUS at 0xa0, the last reading in bits
	11:0 in tenths of a degree, valid in bit 21). */


#include <stdio.h>
#include <string.h>

#include <Drivers.h>
#include <KernelExport.h>
#include <vm/vm.h>

#include <libfdt.h>


extern void* gFDT;

int32 api_version = B_CUR_DRIVER_API_VERSION;

static const uint32 kSensorStatus = 0xa0;
static const uint32 kSensorEnable = 0x4;	// SROT control, sensors in 18:3
static const uint32 kMaxInstances = 8;

struct TsensInstance {
	area_id			area;
	volatile uint32* tm;
	uint32			sensors;
	uint32			enabled;		// bitmap
};

static TsensInstance sInstances[kMaxInstances];
static uint32 sInstanceCount;
static int32 sCriticalMilliCelsius = 110000;
static const char* sDeviceNames[] = { "power/qcom_thermal/0", NULL };


static uint64
fdt_reg(const fdt32_t* reg, int index)
{
	return (uint64)fdt32_to_cpu(reg[index * 4]) << 32
		| fdt32_to_cpu(reg[index * 4 + 1]);
}


static void
find_critical_trip()
{
	int zones = fdt_path_offset(gFDT, "/thermal-zones");
	if (zones < 0)
		return;
	int32 lowest = INT32_MAX;
	int zone;
	fdt_for_each_subnode(zone, gFDT, zones) {
		int trips = fdt_subnode_offset(gFDT, zone, "trips");
		if (trips < 0)
			continue;
		int trip;
		fdt_for_each_subnode(trip, gFDT, trips) {
			const char* type = (const char*)fdt_getprop(gFDT, trip, "type",
				NULL);
			int length;
			const fdt32_t* temperature = (const fdt32_t*)fdt_getprop(gFDT,
				trip, "temperature", &length);
			if (type == NULL || temperature == NULL || length < 4
				|| strcmp(type, "critical") != 0)
				continue;
			int32 value = (int32)fdt32_to_cpu(*temperature);
			if (value < lowest)
				lowest = value;
		}
	}
	if (lowest != INT32_MAX)
		sCriticalMilliCelsius = lowest;
}


static status_t
tsens_open(const char* name, uint32 flags, void** _cookie)
{
	*_cookie = NULL;
	return B_OK;
}


static status_t
tsens_close(void* cookie)
{
	return B_OK;
}


static status_t
tsens_free(void* cookie)
{
	return B_OK;
}


static status_t
tsens_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	return B_DEV_INVALID_IOCTL;
}


//!	The hottest valid sensor in tenths of a degree; false when none is.
static bool
hottest(int32* _deciCelsius)
{
	bool found = false;
	int32 hottest = INT32_MIN;
	for (uint32 i = 0; i < sInstanceCount; i++) {
		const TsensInstance& instance = sInstances[i];
		for (uint32 s = 0; s < instance.sensors; s++) {
			if ((instance.enabled & (1u << s)) == 0)
				continue;
			uint32 status = instance.tm[kSensorStatus / 4 + s];
			if ((status & (1u << 21)) == 0)
				continue;
			// a signed 12-bit value
			int32 value = (int32)(status << 20) >> 20;
			if (value > hottest)
				hottest = value;
			found = true;
		}
	}
	*_deciCelsius = hottest;
	return found;
}


static status_t
tsens_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	if (position > 0) {
		*_length = 0;
		return B_OK;
	}

	int32 current;
	if (!hottest(&current))
		return B_DEV_NOT_READY;

	char text[256];
	int length = snprintf(text, sizeof(text),
		"  Critical Temperature: %" B_PRId32 ".%" B_PRId32 " C\n"
		"  Current Temperature: %" B_PRId32 ".%" B_PRId32 " C\n",
		sCriticalMilliCelsius / 1000, (sCriticalMilliCelsius % 1000) / 100,
		current / 10, (current < 0 ? -current : current) % 10);
	if (length < 0)
		return B_ERROR;
	if ((size_t)length > *_length)
		length = *_length;
	if (user_memcpy(buffer, text, length) != B_OK)
		return B_BAD_ADDRESS;
	*_length = length;
	return B_OK;
}


static status_t
tsens_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static device_hooks sDeviceHooks = {
	tsens_open,
	tsens_close,
	tsens_free,
	tsens_control,
	tsens_read,
	tsens_write
};


status_t
init_hardware()
{
	if (gFDT == NULL
		|| fdt_node_offset_by_compatible(gFDT, -1, "qcom,tsens-v2") < 0)
		return B_DEVICE_NOT_FOUND;
	return B_OK;
}


status_t
init_driver()
{
	if (gFDT == NULL)
		return B_DEVICE_NOT_FOUND;

	int node = -1;
	while (sInstanceCount < kMaxInstances
		&& (node = fdt_node_offset_by_compatible(gFDT, node, "qcom,tsens-v2"))
			>= 0) {
		const char* status = (const char*)fdt_getprop(gFDT, node, "status",
			NULL);
		if (status != NULL && strcmp(status, "okay") != 0
			&& strcmp(status, "ok") != 0)
			continue;
		int length;
		const fdt32_t* reg = (const fdt32_t*)fdt_getprop(gFDT, node, "reg",
			&length);
		const fdt32_t* sensors = (const fdt32_t*)fdt_getprop(gFDT, node,
			"#qcom,sensors", NULL);
		if (reg == NULL || length < 32 || sensors == NULL)
			continue;

		TsensInstance& instance = sInstances[sInstanceCount];
		instance.sensors = fdt32_to_cpu(*sensors);
		if (instance.sensors > 16)
			instance.sensors = 16;

		// which sensors the firmware enabled (SROT control, read only)
		volatile uint32* srot;
		area_id srotArea = map_physical_memory("qcom tsens srot",
			fdt_reg(reg, 1), B_PAGE_SIZE,
			B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY, B_KERNEL_READ_AREA,
			(void**)&srot);
		if (srotArea < 0)
			continue;
		instance.enabled = (srot[kSensorEnable / 4] >> 3) & 0xffff;
		delete_area(srotArea);

		instance.area = map_physical_memory("qcom tsens", fdt_reg(reg, 0),
			B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
			B_KERNEL_READ_AREA, (void**)&instance.tm);
		if (instance.area < 0)
			continue;
		sInstanceCount++;
	}
	if (sInstanceCount == 0)
		return B_DEVICE_NOT_FOUND;

	find_critical_trip();
	int32 current;
	bool valid = hottest(&current);
	dprintf("qcom_thermal: %" B_PRIu32 " sensor blocks, hottest %" B_PRId32
		".%" B_PRId32 " C%s, critical at %" B_PRId32 " C\n", sInstanceCount,
		current / 10, (current < 0 ? -current : current) % 10,
		valid ? "" : " (none valid)", sCriticalMilliCelsius / 1000);
	return B_OK;
}


void
uninit_driver()
{
	for (uint32 i = 0; i < sInstanceCount; i++)
		delete_area(sInstances[i].area);
	sInstanceCount = 0;
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
