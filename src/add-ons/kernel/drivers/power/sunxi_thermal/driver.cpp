/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Thermal management of the Allwinner A733 (Radxa Cubie A7S, no fan):

	- the thermal sensor controller's five zones (big cluster, DRAM, NPU,
	  little cluster, GPU), published as /dev/power/sunxi_thermal/<n> in the
	  text form of the other thermal drivers (AirTop reads them), with
	  /dev/power/sunxi_thermal/status for the whole state;
	- frequency and voltage scaling of both CPU clusters with the BSP's
	  operating points (CpuFreq), on the cores' load (an ondemand governor:
	  the clusters' PLL and PMIC changes sleep, so they cannot be made in the
	  scheduler's cpufreq hooks, which run with interrupts off);
	- throttling: above the passive trip the clusters' top speed, the GPU's
	  and the video engine's clocks come down a step at a time (the big
	  cores first) and go back up below the release temperature;
	- at the hot trip, everything to the lowest speed and an event for the
	  power daemon (a notification); at the critical trip an event that has
	  the power daemon shut the system down through the registrar, and, if
	  the system is still up some time later (or nobody listens), the
	  kernel's own shutdown (sync, then off).

	The trips come from the device tree's thermal zones; the driver settings
	file "sunxi_thermal" overrides them for tests, and switches parts off
	(the same "key value" lines written to /dev/power/sunxi_thermal/status
	change them at run time, as does "throttle_level <n>"):

		dvfs false				no frequency changes at all
		governor performance	(or powersave, ondemand)
		throttle false
		passive_temperature 80	release_temperature 76
		hot_temperature 90		critical_temperature 96
		critical_grace 30		seconds before the kernel shuts down itself
		critical_margin 4		or degrees above critical before it does
		max_big_mhz 1794		max_little_mhz 1404
		throttle_devices false	the GPU and the video engine stay at full
								speed (GPU 800 MHz, or what powervr set)
		log_interval 5			a "watch" line every 5 s
		voltage_settle_percent 200	a raised voltage's settling time (of
								the PMIC's nominal slew)
		wait_pll_ramp true		a PLL change waits for its ramp's end
		dvfs_stress 60			(run time) a test: both clusters between
								their lowest and top point every tick

	The kernel debugger's "sunxi_thermal" command shows the last frequency
	and voltage steps.
*/


#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <bus/FDT.h>
#include <condition_variable.h>
#include <device_manager.h>
#include <driver_settings.h>
#include <Drivers.h>
#include <KernelExport.h>
#include <fs/select_sync_pool.h>
#include <kernel.h>
#include <lock.h>
#include <cpu.h>
#include <util/AutoLock.h>
#include <vm/vm.h>

extern "C" {
#include <libfdt.h>
}

#include <sunxi_pmic.h>

#include "CpuFreq.h"
#include "Ths.h"


#define DRIVER_MODULE_NAME	"drivers/power/sunxi_thermal/driver_v1"
#define DEVICE_MODULE_NAME	"drivers/power/sunxi_thermal/device_v1"

#define TRACE(x...)	dprintf("sunxi_thermal: " x)

extern void* gFDT;


using namespace sunxi_thermal;


static const int32 kMaxZones = Ths::kSensorCount;
static const int32 kNoTemperature = -1000000;
static const bigtime_t kTick = 20000;
static const int32 kThermalTicks = 12;			// a reading every 240 ms
static const int32 kUpLoad = 80;				// percent: straight to the top
static const int32 kTargetLoad = 60;			// percent below that
static const bigtime_t kDownHold = 100000;		// after a raise
// Above the passive trip a step at most every 2 s; 4 C above it two every
// 480 ms, 8 C above three on every reading: with all eight cores at their
// top speed the SoC warms by 6 C a second, from 63 C to 95 C in 5 s.
static const bigtime_t kThrottleStep = 2000000;
static const bigtime_t kThrottleFastStep = 480000;
static const bigtime_t kReleaseStep = 4000000;
static const int32 kFastMargin = 4000;
static const int32 kRunawayMargin = 8000;

// The throttle levels: each cluster's top speed (MHz), the big cores first,
// and the steps of the GPU's and the video engine's clocks (below). With
// two films playing the GPU, the video engine and the memory make most of
// the heat: the cores alone at 416 MHz do not hold it.
static const struct { uint32 big, little; uint8 gpu, ve; } kLevels[] = {
	{ 0xffffffff, 0xffffffff, 0, 0 },
	{ 1794, 1794, 1, 1 },
	{ 1612, 1716, 2, 1 },
	{ 1404, 1612, 2, 2 },
	{ 1196, 1404, 3, 2 },
	{ 1014, 1196, 3, 2 },
	{ 780, 1014, 4, 2 },
	{ 416, 780, 4, 3 },
	{ 416, 416, 4, 3 }
};

// The GPU's clock (CCU 0xb20): the powervr driver runs it from pll-peri0-800m
// (source 1), or -600m (source 2) when its settings say so; slower from the
// 400, 300 and 200 MHz taps (sources 3-5), as Allwinner's GPU frequency
// scaling does. Bit 27 latches a new source. A level's step is a cap: the
// GPU never runs faster than its driver set it.
static const uint32 kGpuSources[] = { 1, 2, 3, 4, 5 };
static const uint32 kGpuMegahertz[] = { 800, 600, 400, 300, 200 };
// The video engine's (CCU 0xa88): sunxi_ve runs it from pll-peri0-600m
// (source 3) undivided; divided by 1-4 (bits 4:0 hold the divider - 1).
static const uint32 kVeMegahertz[] = { 600, 300, 200, 150 };

static const phys_addr_t kCcu = 0x02002000;
enum {
	CCU_VE_DECODER_CLOCK	= 0x0a88,
	CCU_GPU_CLOCK			= 0x0b20,
	CCU_CLOCK_GATE			= 1u << 31,
	CCU_GPU_UPDATE			= 1u << 27,
	CCU_SOURCE_SHIFT		= 24,
	CCU_SOURCE_MASK			= 7u << 24
};
static const int32 kLevelCount = B_COUNT_OF(kLevels);

enum {
	TRIP_EVENT_NONE		= 0,
	TRIP_EVENT_HOT		= 1,
	TRIP_EVENT_CRITICAL	= 2
};

enum governor_type {
	GOVERNOR_ONDEMAND,
	GOVERNOR_PERFORMANCE,
	GOVERNOR_POWERSAVE
};


struct Zone {
	char		name[16];
	int32		sensor;
	int32		passive;		// millidegrees, 0 when the zone has none
	int32		hysteresis;		// of the passive trip
	int32		hot;
	int32		critical;
	int32		temperature;
	bool		valid;
};


struct ClusterState {
	int32		thermalCap;		// index of the top operating point
	int32		userCap;
	int32		load;			// percent, the busiest core
	bigtime_t	holdUntil;
	bigtime_t	lastActive[Cluster::kMaxCpus];
};


static device_manager_info* sDeviceManager;
static sunxi_pmic_module_info* sPmic;

static Ths sThs;
static CpuFreq* sCpuFreq;
static ClusterState sClusterState[CpuFreq::kMaxClusters];
static bool sDvfs;
static Zone sZones[kMaxZones];
static int32 sZoneCount;

// settings and trips (millidegrees)
static governor_type sGovernor = GOVERNOR_ONDEMAND;
static bool sThrottle = true;
static int32 sPassive = 80000;
static int32 sRelease = 76000;
static int32 sHot = 90000;
static int32 sCritical = 96000;
static bigtime_t sCriticalGrace = 30000000;
static int32 sCriticalMargin = 4000;
static uint32 sMaxBigMhz;
static uint32 sMaxLittleMhz;
static bigtime_t sLogInterval;
static bigtime_t sStressUntil;
static area_id sCcuArea = -1;
static volatile uint8* sCcu;
static bool sThrottleDevices = true;
static int32 sGpuStep = -1;			// as last set; -1: not touched
static int32 sGpuBase = 0;			// the step the powervr driver set
static uint32 sGpuWritten = 0;		// the source last set here, 0: none
static int32 sVeStep = -1;
static uint32 sGpuChanges;
static uint32 sVeChanges;
static uint32 sVoltageSettlePercent = 200;
static bool sWaitPllRamp = true;

// the monitor's state
static thread_id sThread = -1;
static volatile bool sStop;
static int32 sTemperature;			// the hottest zone
static int32 sMaxTemperature;
static int32 sLevel;
static int32 sHotCount;
static int32 sCriticalCount;
static bool sHotFired;
static bool sCriticalFired;
static bigtime_t sCriticalTime;
static bigtime_t sLastUp;
static bigtime_t sLastDown;
static bigtime_t sLastLog;
static uint32 sReadErrors;

// the trip event device
static mutex sTripLock = MUTEX_INITIALIZER("sunxi_thermal trip");
static ConditionVariable sTripCondition;
static select_sync_pool* sTripSelectPool;
static uint8 sTripEvent;
static int32 sTripOpenCount;
static uint32 sHotEvents;
static uint32 sCriticalEvents;


static void
format_temperature(char* buffer, size_t size, int32 milliCelsius)
{
	const char* sign = milliCelsius < 0 ? "-" : "";
	if (milliCelsius < 0)
		milliCelsius = -milliCelsius;
	snprintf(buffer, size, "%s%" B_PRId32 ".%" B_PRId32, sign,
		milliCelsius / 1000, (milliCelsius % 1000) / 100);
}


//	#pragma mark - device tree


static void
read_zones(const void* fdt, int thsNode)
{
	uint32 thsPhandle = fdt_get_phandle(fdt, thsNode);
	int zones = fdt_path_offset(fdt, "/thermal-zones");
	int node;
	if (zones >= 0) {
		fdt_for_each_subnode(node, fdt, zones) {
			int length;
			const fdt32_t* sensor = (const fdt32_t*)fdt_getprop(fdt, node,
				"thermal-sensors", &length);
			if (sensor == NULL || length < 8
				|| fdt32_to_cpu(sensor[0]) != thsPhandle
				|| fdt32_to_cpu(sensor[1]) >= (uint32)Ths::kSensorCount
				|| sZoneCount == kMaxZones) {
				continue;
			}
			Zone& zone = sZones[sZoneCount++];
			memset(&zone, 0, sizeof(zone));
			zone.sensor = fdt32_to_cpu(sensor[1]);
			const char* name = fdt_get_name(fdt, node, NULL);
			strlcpy(zone.name, name, sizeof(zone.name));
			char* suffix = strstr(zone.name, "-thermal");
			if (suffix != NULL)
				*suffix = '\0';

			int trips = fdt_subnode_offset(fdt, node, "trips");
			int trip;
			if (trips < 0)
				continue;
			fdt_for_each_subnode(trip, fdt, trips) {
				const char* type = (const char*)fdt_getprop(fdt, trip, "type",
					NULL);
				const fdt32_t* value = (const fdt32_t*)fdt_getprop(fdt, trip,
					"temperature", &length);
				if (type == NULL || value == NULL || length < 4)
					continue;
				int32 temperature = (int32)fdt32_to_cpu(*value);
				if (strcmp(type, "passive") == 0) {
					zone.passive = temperature;
					const fdt32_t* hysteresis = (const fdt32_t*)fdt_getprop(
						fdt, trip, "hysteresis", &length);
					if (hysteresis != NULL && length >= 4)
						zone.hysteresis = fdt32_to_cpu(*hysteresis);
				} else if (strcmp(type, "hot") == 0)
					zone.hot = temperature;
				else if (strcmp(type, "critical") == 0)
					zone.critical = temperature;
			}
		}
	}

	if (sZoneCount == 0) {
		// no zones described: one per sensor with the default trips
		for (int32 i = 0; i < Ths::kSensorCount; i++) {
			Zone& zone = sZones[sZoneCount++];
			memset(&zone, 0, sizeof(zone));
			snprintf(zone.name, sizeof(zone.name), "sensor%" B_PRId32, i);
			zone.sensor = i;
		}
		return;
	}

	// the strictest of each kind applies to all
	int32 passive = 0, hysteresis = 0, hot = 0, critical = 0;
	for (int32 i = 0; i < sZoneCount; i++) {
		if (sZones[i].passive != 0
			&& (passive == 0 || sZones[i].passive < passive)) {
			passive = sZones[i].passive;
			hysteresis = sZones[i].hysteresis;
		}
		if (sZones[i].hot != 0 && (hot == 0 || sZones[i].hot < hot))
			hot = sZones[i].hot;
		if (sZones[i].critical != 0
			&& (critical == 0 || sZones[i].critical < critical)) {
			critical = sZones[i].critical;
		}
	}
	if (passive != 0) {
		sPassive = passive;
		sRelease = passive - (hysteresis > 0 ? hysteresis : 4000);
	}
	if (hot != 0)
		sHot = hot;
	if (critical != 0)
		sCritical = critical;
}


static status_t
read_calibration(const void* fdt, int thsNode, uint32* words)
{
	int length;
	const fdt32_t* cells = (const fdt32_t*)fdt_getprop(fdt, thsNode,
		"nvmem-cells", &length);
	if (cells == NULL || length < 4)
		return B_ENTRY_NOT_FOUND;
	int cell = fdt_node_offset_by_phandle(fdt, fdt32_to_cpu(cells[0]));
	int block = cell >= 0 ? fdt_parent_offset(fdt, cell) : -1;
	const fdt32_t* reg = cell >= 0
		? (const fdt32_t*)fdt_getprop(fdt, cell, "reg", &length) : NULL;
	const fdt32_t* blockReg = block >= 0
		? (const fdt32_t*)fdt_getprop(fdt, block, "reg", NULL) : NULL;
	if (reg == NULL || length < 8 || blockReg == NULL
		|| fdt32_to_cpu(reg[1]) < 12) {
		return B_BAD_DATA;
	}
	// the SID block sits in the soc node (one address cell)
	phys_addr_t address = fdt32_to_cpu(blockReg[0]) + fdt32_to_cpu(reg[0]);

	volatile uint8* page;
	area_id area = map_physical_memory("sunxi_thermal sid",
		address & ~(phys_addr_t)(B_PAGE_SIZE - 1), B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA, (void**)&page);
	if (area < 0)
		return area;
	if ((address & (B_PAGE_SIZE - 1)) + 12 > B_PAGE_SIZE) {
		delete_area(area);
		return B_BAD_DATA;
	}
	for (int32 i = 0; i < 3; i++) {
		words[i] = *(volatile uint32*)(page + (address & (B_PAGE_SIZE - 1))
			+ i * 4);
	}
	delete_area(area);
	return B_OK;
}


static bool
parse_boolean(const char* value)
{
	return strcmp(value, "true") == 0 || strcmp(value, "on") == 0
		|| strcmp(value, "yes") == 0 || strcmp(value, "1") == 0
		|| strcmp(value, "enabled") == 0;
}


static void set_level(int32 level, const char* why);
static void update_user_caps();


/*!	One setting, from the settings file or written to the status device.
*/
static status_t
apply_setting(const char* key, const char* value, bool runtime)
{
	if (strcmp(key, "dvfs") == 0) {
		if (runtime)
			return B_NOT_ALLOWED;
		sDvfs = parse_boolean(value);
	} else if (strcmp(key, "governor") == 0) {
		if (strcmp(value, "performance") == 0)
			sGovernor = GOVERNOR_PERFORMANCE;
		else if (strcmp(value, "powersave") == 0)
			sGovernor = GOVERNOR_POWERSAVE;
		else if (strcmp(value, "ondemand") == 0)
			sGovernor = GOVERNOR_ONDEMAND;
		else
			return B_BAD_VALUE;
	} else if (strcmp(key, "throttle") == 0) {
		sThrottle = parse_boolean(value);
	} else if (strcmp(key, "throttle_devices") == 0) {
		sThrottleDevices = parse_boolean(value);
	} else if (strcmp(key, "throttle_level") == 0) {
		if (!runtime)
			return B_NOT_ALLOWED;
		set_level(strtol(value, NULL, 10), "set");
	} else if (strcmp(key, "passive_temperature") == 0
		|| strcmp(key, "release_temperature") == 0
		|| strcmp(key, "hot_temperature") == 0
		|| strcmp(key, "critical_temperature") == 0) {
		int32 degrees = strtol(value, NULL, 10);
		if (degrees <= 20 || degrees >= 120)
			return B_BAD_VALUE;
		int32* target = key[0] == 'p' ? &sPassive : key[0] == 'r' ? &sRelease
			: key[0] == 'h' ? &sHot : &sCritical;
		*target = degrees * 1000;
	} else if (strcmp(key, "critical_grace") == 0) {
		sCriticalGrace = strtol(value, NULL, 10) * 1000000LL;
	} else if (strcmp(key, "critical_margin") == 0) {
		int32 degrees = strtol(value, NULL, 10);
		if (degrees < 1 || degrees > 30)
			return B_BAD_VALUE;
		sCriticalMargin = degrees * 1000;
	} else if (strcmp(key, "max_big_mhz") == 0) {
		sMaxBigMhz = strtoul(value, NULL, 10);
		update_user_caps();
	} else if (strcmp(key, "max_little_mhz") == 0) {
		sMaxLittleMhz = strtoul(value, NULL, 10);
		update_user_caps();
	} else if (strcmp(key, "log_interval") == 0) {
		sLogInterval = strtol(value, NULL, 10) * 1000000LL;
	} else if (strcmp(key, "dvfs_stress") == 0) {
		// a test: every tick, each cluster between its lowest and its top
		// point, for that many seconds
		if (!runtime)
			return B_NOT_ALLOWED;
		sStressUntil = system_time() + strtol(value, NULL, 10) * 1000000LL;
	} else if (strcmp(key, "voltage_settle_percent") == 0) {
		uint32 percent = strtoul(value, NULL, 10);
		if (percent < 100 || percent > 1000)
			return B_BAD_VALUE;
		sVoltageSettlePercent = percent;
		if (sCpuFreq != NULL)
			sCpuFreq->voltageSettlePercent = percent;
	} else if (strcmp(key, "wait_pll_ramp") == 0) {
		sWaitPllRamp = parse_boolean(value);
		if (sCpuFreq != NULL)
			sCpuFreq->waitPllRamp = sWaitPllRamp;
	} else
		return B_NAME_NOT_FOUND;

	TRACE("%s %s (%s)\n", key, value, runtime ? "set" : "settings");
	return B_OK;
}


static void
read_settings()
{
	void* handle = load_driver_settings("sunxi_thermal");
	if (handle == NULL)
		return;
	const driver_settings* settings = get_driver_settings(handle);
	for (int32 i = 0; settings != NULL && i < settings->parameter_count;
			i++) {
		const driver_parameter& parameter = settings->parameters[i];
		if (parameter.value_count < 1)
			continue;
		if (apply_setting(parameter.name, parameter.values[0], false) != B_OK)
			TRACE("setting %s ignored\n", parameter.name);
	}
	unload_driver_settings(handle);
}


//	#pragma mark - trip events


static void
post_trip_event(uint8 event)
{
	MutexLocker locker(sTripLock);
	if (event > sTripEvent)
		sTripEvent = event;
	if (event == TRIP_EVENT_HOT)
		sHotEvents++;
	else
		sCriticalEvents++;
	sTripCondition.NotifyAll();
	if (sTripSelectPool != NULL)
		notify_select_event_pool(sTripSelectPool, B_SELECT_READ);
}


static void
emergency_shutdown(const char* why)
{
	TRACE("%s: shutting the system down now\n", why);
	system_shutdown(false);
	TRACE("the system did not turn off\n");
}


//	#pragma mark - policy


/*!	The GPU's and the video engine's clocks for the level, when their
	drivers run them as expected (gate on, the 600 MHz source or one of
	ours): called on every reading, since sunxi_ve sets its clock again
	each time the engine powers up.
*/
static void
apply_device_clocks()
{
	if (sCcu == NULL)
		return;
	int32 gpuStep = sThrottleDevices ? kLevels[sLevel].gpu : 0;
	int32 veStep = sThrottleDevices ? kLevels[sLevel].ve : 0;

	volatile uint32* gpu = (volatile uint32*)(sCcu + CCU_GPU_CLOCK);
	uint32 value = *gpu;
	uint32 source = (value & CCU_SOURCE_MASK) >> CCU_SOURCE_SHIFT;
	if ((value & CCU_CLOCK_GATE) != 0 && (value & 0xf) == 0
		&& source >= kGpuSources[0]
		&& source <= kGpuSources[B_COUNT_OF(kGpuSources) - 1]) {
		// a source not set here is the powervr driver's (at power-up)
		if (source != sGpuWritten)
			sGpuBase = source - kGpuSources[0];
		gpuStep = max_c(gpuStep, sGpuBase);
		if (source != kGpuSources[gpuStep]) {
			*gpu = (value & ~CCU_SOURCE_MASK) | CCU_GPU_UPDATE
				| kGpuSources[gpuStep] << CCU_SOURCE_SHIFT;
			memory_full_barrier();
			sGpuWritten = kGpuSources[gpuStep];
			sGpuChanges++;
		}
	}
	if (gpuStep != sGpuStep) {
		sGpuStep = gpuStep;
		if (sLogInterval > 0)
			TRACE("GPU at %" B_PRIu32 " MHz\n", kGpuMegahertz[gpuStep]);
	}

	volatile uint32* ve = (volatile uint32*)(sCcu + CCU_VE_DECODER_CLOCK);
	value = *ve;
	source = (value & CCU_SOURCE_MASK) >> CCU_SOURCE_SHIFT;
	if ((value & CCU_CLOCK_GATE) != 0 && source == 3
		&& (value & 0x1f) != (uint32)veStep) {
		*ve = (value & ~0x1fu) | veStep;
		memory_full_barrier();
		sVeChanges++;
	}
	if (veStep != sVeStep) {
		sVeStep = veStep;
		if (sLogInterval > 0)
			TRACE("video engine at %" B_PRIu32 " MHz\n", kVeMegahertz[veStep]);
	}
}


static int32
cluster_cap(const Cluster& cluster, int32 level)
{
	bool big = cluster.pllOffset == 0x2000;
	uint32 mhz = big ? kLevels[level].big : kLevels[level].little;
	if (mhz == 0xffffffff)
		return cluster.pointCount - 1;
	return cluster.IndexAtOrBelow(mhz * 1000);
}


static void
set_level(int32 level, const char* why)
{
	level = max_c(0, min_c(level, kLevelCount - 1));
	if (level == sLevel)
		return;
	int32 previous = sLevel;
	sLevel = level;
	if (sCpuFreq != NULL) {
		for (int32 i = 0; i < sCpuFreq->ClusterCount(); i++) {
			sClusterState[i].thermalCap = cluster_cap(sCpuFreq->ClusterAt(i),
				level);
		}
	}
	apply_device_clocks();
	if (sLogInterval > 0 || previous == 0 || level == 0
		|| level == kLevelCount - 1) {
		char line[160];
		format_temperature(line, sizeof(line), sTemperature);
		int used = strlen(line);
		used += snprintf(line + used, sizeof(line) - used, " C: throttle "
			"level %" B_PRId32 " -> %" B_PRId32 " (%s)", previous, level, why);
		for (int32 i = 0; sCpuFreq != NULL && i < sCpuFreq->ClusterCount();
				i++) {
			Cluster& cluster = sCpuFreq->ClusterAt(i);
			used += snprintf(line + used, sizeof(line) - used, ", %s %" B_PRIu32
				" MHz", cluster.name,
				cluster.points[sClusterState[i].thermalCap].kilohertz / 1000);
		}
		TRACE("%s\n", line);
	}
}


static void
log_watch()
{
	char line[320];
	char temperature[16];
	format_temperature(temperature, sizeof(temperature), sTemperature);
	int used = snprintf(line, sizeof(line), "watch t=%s zones=", temperature);
	for (int32 i = 0; i < sZoneCount; i++) {
		format_temperature(temperature, sizeof(temperature),
			sZones[i].temperature);
		used += snprintf(line + used, sizeof(line) - used, "%s%s",
			i > 0 ? "," : "", sZones[i].valid ? temperature : "-");
	}
	used += snprintf(line + used, sizeof(line) - used, " level=%" B_PRId32,
		sLevel);
	if (sGpuStep >= 0) {
		used += snprintf(line + used, sizeof(line) - used, " gpu=%" B_PRIu32
			"MHz ve=%" B_PRIu32 "MHz", kGpuMegahertz[sGpuStep],
			kVeMegahertz[sVeStep]);
	}
	if (sCpuFreq != NULL) {
		for (int32 i = 0; i < sCpuFreq->ClusterCount(); i++) {
			Cluster& cluster = sCpuFreq->ClusterAt(i);
			used += snprintf(line + used, sizeof(line) - used,
				" %s=%" B_PRIu32 "MHz/%" B_PRIu32 "mV/%" B_PRId32 "%%",
				cluster.name, cluster.CurrentKilohertz() / 1000,
				cluster.microvolts / 1000, sClusterState[i].load);
		}
	}
	TRACE("%s\n", line);
}


static void
thermal_step(bigtime_t now)
{
	int32 hottest = kNoTemperature;
	for (int32 i = 0; i < sZoneCount; i++) {
		Zone& zone = sZones[i];
		int32 temperature;
		zone.valid = sThs.Read(zone.sensor, &temperature) == B_OK
			&& temperature > -40000 && temperature < 150000;
		if (!zone.valid)
			continue;
		zone.temperature = temperature;
		hottest = max_c(hottest, temperature);
	}
	if (hottest == kNoTemperature) {
		if (sReadErrors++ == 0)
			TRACE("no valid temperature\n");
		return;
	}
	sTemperature = hottest;
	sMaxTemperature = max_c(sMaxTemperature, hottest);

	char temperature[16];
	format_temperature(temperature, sizeof(temperature), hottest);

	// critical: shut down
	if (hottest >= sCritical) {
		if (++sCriticalCount >= 2 && !sCriticalFired) {
			sCriticalFired = true;
			sCriticalTime = now;
			char limit[16];
			format_temperature(limit, sizeof(limit), sCritical);
			TRACE("CRITICAL: %s C at or above the critical %s C: shutting "
				"down\n", temperature, limit);
			set_level(kLevelCount - 1, "critical");
			sLastUp = sLastDown = now;
			post_trip_event(TRIP_EVENT_CRITICAL);
			if (atomic_get(&sTripOpenCount) == 0) {
				// nobody to shut down the user way
				emergency_shutdown("critical temperature, no power daemon");
			}
		}
	} else
		sCriticalCount = 0;
	if (sCriticalFired) {
		if (now - sCriticalTime > sCriticalGrace)
			emergency_shutdown("still running after the critical temperature");
		else if (hottest >= sCritical + sCriticalMargin)
			emergency_shutdown("still heating after the critical temperature");
	}

	// hot: everything to the lowest speed
	if (hottest >= sHot) {
		if (++sHotCount >= 2) {
			if (sLevel != kLevelCount - 1)
				set_level(kLevelCount - 1, "hot");
			sLastUp = sLastDown = now;
			if (!sHotFired) {
				sHotFired = true;
				char limit[16];
				format_temperature(limit, sizeof(limit), sHot);
				TRACE("HOT: %s C at or above %s C: all cores to the lowest "
					"speed\n", temperature, limit);
				post_trip_event(TRIP_EVENT_HOT);
			}
		}
	} else {
		sHotCount = 0;
		if (hottest < sHot - 5000)
			sHotFired = false;
	}

	// passive: step the top speeds down and back up
	if (sThrottle && sCpuFreq != NULL) {
		if (hottest >= sPassive + kRunawayMargin) {
			set_level(sLevel + 3, "heating fast");
			sLastUp = sLastDown = now;
		} else if (hottest >= sPassive + kFastMargin
			&& now - sLastUp >= kThrottleFastStep) {
			set_level(sLevel + 2, "hot zone");
			sLastUp = sLastDown = now;
		} else if (hottest >= sPassive && now - sLastUp >= kThrottleStep) {
			set_level(sLevel + 1, "above passive");
			sLastUp = sLastDown = now;
		} else if (hottest < sRelease && sLevel > 0 && !sCriticalFired
			&& now - sLastDown >= kReleaseStep) {
			set_level(sLevel - 1, "cooled");
			sLastDown = now;
		}
	}

	apply_device_clocks();

	if (sLogInterval > 0 && now - sLastLog >= sLogInterval) {
		sLastLog = now;
		log_watch();
	}
}


static void
dvfs_step(bigtime_t now, bigtime_t elapsed)
{
	for (int32 c = 0; c < sCpuFreq->ClusterCount(); c++) {
		Cluster& cluster = sCpuFreq->ClusterAt(c);
		ClusterState& state = sClusterState[c];

		int32 load = 0;
		for (int32 i = 0; i < cluster.cpuCount; i++) {
			bigtime_t active = cpu_get_active_time(cluster.cpus[i]);
			bigtime_t delta = active - state.lastActive[i];
			state.lastActive[i] = active;
			if (elapsed > 0)
				load = max_c(load, (int32)min_c(delta * 100 / elapsed, 100));
		}
		state.load = load;

		int32 cap = min_c(state.thermalCap, state.userCap);
		int32 want;
		governor_type governor = sGovernor;
		if (now < sStressUntil) {
			governor = cluster.current == 0 ? GOVERNOR_PERFORMANCE
				: GOVERNOR_POWERSAVE;
		}
		switch (governor) {
			case GOVERNOR_PERFORMANCE:
				want = cap;
				break;
			case GOVERNOR_POWERSAVE:
				want = 0;
				break;
			default:
				if (load >= kUpLoad)
					want = cap;
				else {
					uint32 kilohertz = (uint32)((uint64)cluster.CurrentKilohertz()
						* load / kTargetLoad);
					want = cluster.IndexAtOrAbove(kilohertz);
				}
				break;
		}
		want = min_c(want, cap);

		int32 current = cluster.current;
		if (want == current)
			continue;
		if (current >= 0 && want < current && now < state.holdUntil
			&& now >= sStressUntil
			&& current <= cap) {
			// stay up a little after a raise
			continue;
		}
		status_t status = sCpuFreq->SetPoint(cluster, want);
		if (status == B_OK) {
			if (want > current)
				state.holdUntil = now + kDownHold;
		} else {
			TRACE("%s: to %" B_PRIu32 " MHz: %s\n", cluster.name,
				cluster.points[want].kilohertz / 1000, strerror(status));
			if (cluster.errors >= 10) {
				TRACE("frequency scaling off after %" B_PRIu32 " errors\n",
					cluster.errors);
				sDvfs = false;
				return;
			}
		}
	}
}


static bigtime_t sLastTick;


static int
dump_thermal(int argc, char** argv)
{
	kprintf("sunxi_thermal: last tick %" B_PRId64 " us, level %" B_PRId32
		", %" B_PRId32 " mC\n", sLastTick, sLevel, sTemperature);
	if (sCpuFreq != NULL)
		sCpuFreq->DumpTrace();
	return 0;
}


static status_t
monitor_thread(void*)
{
	bigtime_t next = system_time();
	bigtime_t last = next;
	int32 tick = 0;
	while (!sStop) {
		next += kTick;
		bigtime_t now = system_time();
		if (next < now)
			next = now + kTick;
		snooze_until(next, B_SYSTEM_TIMEBASE);
		now = system_time();
		sLastTick = now;

		if (tick++ % kThermalTicks == 0)
			thermal_step(now);
		if (sDvfs && sCpuFreq != NULL)
			dvfs_step(now, now - last);
		last = now;
	}
	return B_OK;
}


//	#pragma mark - devices


enum cookie_type {
	COOKIE_ZONE,
	COOKIE_STATUS,
	COOKIE_TRIP
};

struct thermal_cookie {
	cookie_type	type;
	int32		zone;
	int			openMode;
	bool		closed;
};


static int
status_text(char* buffer, size_t size)
{
	char a[16], b[16], c[16], d[16];
	format_temperature(a, sizeof(a), sTemperature);
	format_temperature(b, sizeof(b), sMaxTemperature);
	int used = snprintf(buffer, size, "temperature %s C (highest %s C):", a,
		b);
	for (int32 i = 0; i < sZoneCount; i++) {
		format_temperature(a, sizeof(a), sZones[i].temperature);
		used += snprintf(buffer + used, size - used, " %s %s", sZones[i].name,
			sZones[i].valid ? a : "-");
	}
	format_temperature(a, sizeof(a), sPassive);
	format_temperature(b, sizeof(b), sRelease);
	format_temperature(c, sizeof(c), sHot);
	format_temperature(d, sizeof(d), sCritical);
	used += snprintf(buffer + used, size - used, "\ntrips: passive %s, "
		"release %s, hot %s, critical %s C; calibrated %s\n"
		"throttle %s, level %" B_PRId32 " of %" B_PRId32 "; hot events %"
		B_PRIu32 ", critical events %" B_PRIu32 ", power daemon %s\n",
		a, b, c, d, sThs.Calibrated() ? "yes" : "no",
		sThrottle ? "on" : "off", sLevel, kLevelCount - 1, sHotEvents,
		sCriticalEvents, atomic_get(&sTripOpenCount) > 0 ? "listening" : "absent");
	if (sCcu != NULL) {
		used += snprintf(buffer + used, size - used, "GPU %" B_PRIu32 " MHz "
			"(clock %#" B_PRIx32 ", %" B_PRIu32 " changes), video engine %"
			B_PRIu32 " MHz (clock %#" B_PRIx32 ", %" B_PRIu32 " changes)%s\n",
			kGpuMegahertz[max_c(sGpuStep, 0)],
			*(volatile uint32*)(sCcu + CCU_GPU_CLOCK), sGpuChanges,
			kVeMegahertz[max_c(sVeStep, 0)],
			*(volatile uint32*)(sCcu + CCU_VE_DECODER_CLOCK), sVeChanges,
			sThrottleDevices ? "" : ", not throttled");
	}
	if (sCpuFreq == NULL) {
		used += snprintf(buffer + used, size - used,
			"no CPU frequency scaling\n");
		return used;
	}
	used += snprintf(buffer + used, size - used, "frequency scaling %s, "
		"governor %s, speed grade %s, DSU %" B_PRIu32 " MHz\n",
		sDvfs ? "on" : "off", sGovernor == GOVERNOR_PERFORMANCE ? "performance"
			: sGovernor == GOVERNOR_POWERSAVE ? "powersave" : "ondemand",
		sCpuFreq->SpeedGrade(), sCpuFreq->DsuKilohertz() / 1000);
	for (int32 i = 0; i < sCpuFreq->ClusterCount(); i++) {
		Cluster& cluster = sCpuFreq->ClusterAt(i);
		ClusterState& state = sClusterState[i];
		uint32 measured = sCpuFreq->MeasureMegahertz(cluster);
		used += snprintf(buffer + used, size - used, "%s: cpus %" B_PRId32
			"-%" B_PRId32 ", %" B_PRIu32 " MHz (PLL %" B_PRIu32 ", measured %"
			B_PRIu32 "), %" B_PRIu32 " mV on dcdc%" B_PRIu32 " (floor %" B_PRIu32
			"), load %" B_PRId32 "%%, top %" B_PRIu32 " MHz (thermal %" B_PRIu32
			", user %" B_PRIu32 "), %" B_PRIu32 " changes, %" B_PRIu32
			" errors\n", cluster.name, cluster.cpus[0],
			cluster.cpus[cluster.cpuCount - 1],
			cluster.CurrentKilohertz() / 1000,
			sCpuFreq->PllKilohertz(cluster.pllOffset) / 1000, measured,
			cluster.microvolts / 1000, cluster.dcdc,
			cluster.floorMicrovolts / 1000, state.load,
			cluster.points[min_c(state.thermalCap, state.userCap)].kilohertz
				/ 1000,
			cluster.points[state.thermalCap].kilohertz / 1000,
			cluster.points[state.userCap].kilohertz / 1000,
			cluster.transitions, cluster.errors);
		used += snprintf(buffer + used, size - used, "  points:");
		for (int32 p = 0; p < cluster.pointCount; p++) {
			used += snprintf(buffer + used, size - used, " %" B_PRIu32 "/%"
				B_PRIu32, cluster.points[p].kilohertz / 1000,
				cluster.points[p].microvolts / 1000);
		}
		used += snprintf(buffer + used, size - used, "\n");
	}
	return min_c(used, (int)size - 1);
}


static int
zone_text(int32 index, char* buffer, size_t size)
{
	const Zone& zone = sZones[index];
	char critical[16], hot[16], passive[16], current[16];
	format_temperature(critical, sizeof(critical), sCritical);
	format_temperature(hot, sizeof(hot), sHot);
	format_temperature(passive, sizeof(passive), sPassive);
	format_temperature(current, sizeof(current), zone.temperature);
	int used = snprintf(buffer, size, "  Zone: %s\n"
		"  Critical Temperature: %s C\n"
		"  Hot Temperature: %s C\n"
		"  Passive Temperature: %s C\n", zone.name, critical, hot, passive);
	if (zone.valid) {
		used += snprintf(buffer + used, size - used,
			"  Current Temperature: %s C\n", current);
	}
	return min_c(used, (int)size - 1);
}


static status_t
thermal_init_device(void* driverCookie, void** _deviceCookie)
{
	*_deviceCookie = driverCookie;
	return B_OK;
}


static status_t
thermal_open(void* deviceCookie, const char* path, int openMode,
	void** _cookie)
{
	thermal_cookie* cookie = new(std::nothrow) thermal_cookie;
	if (cookie == NULL)
		return B_NO_MEMORY;
	cookie->openMode = openMode;
	cookie->zone = -1;
	cookie->closed = false;
	const char* name = strrchr(path, '/');
	name = name != NULL ? name + 1 : path;
	if (strncmp(path, "power/trip/", 11) == 0) {
		cookie->type = COOKIE_TRIP;
		atomic_add(&sTripOpenCount, 1);
	} else if (strcmp(name, "status") == 0)
		cookie->type = COOKIE_STATUS;
	else {
		cookie->type = COOKIE_ZONE;
		cookie->zone = strtol(name, NULL, 10);
		if (cookie->zone < 0 || cookie->zone >= sZoneCount) {
			delete cookie;
			return B_ENTRY_NOT_FOUND;
		}
	}
	*_cookie = cookie;
	return B_OK;
}


static status_t
thermal_close(void* _cookie)
{
	thermal_cookie* cookie = (thermal_cookie*)_cookie;
	if (cookie->type == COOKIE_TRIP) {
		MutexLocker locker(sTripLock);
		cookie->closed = true;
		sTripCondition.NotifyAll();
	}
	return B_OK;
}


static status_t
thermal_free(void* _cookie)
{
	thermal_cookie* cookie = (thermal_cookie*)_cookie;
	if (cookie->type == COOKIE_TRIP)
		atomic_add(&sTripOpenCount, -1);
	delete cookie;
	return B_OK;
}


static status_t
thermal_read(void* _cookie, off_t position, void* buffer, size_t* _length)
{
	thermal_cookie* cookie = (thermal_cookie*)_cookie;

	if (cookie->type == COOKIE_TRIP) {
		// one byte, the strongest event since the last read; blocks until
		// there is one
		if (*_length < 1)
			return B_BAD_VALUE;
		MutexLocker locker(sTripLock);
		while (sTripEvent == TRIP_EVENT_NONE) {
			if (cookie->closed) {
				*_length = 0;
				return B_FILE_ERROR;
			}
			if ((cookie->openMode & O_NONBLOCK) != 0) {
				*_length = 0;
				return B_WOULD_BLOCK;
			}
			status_t status = sTripCondition.Wait(&sTripLock, B_CAN_INTERRUPT);
			if (status != B_OK) {
				*_length = 0;
				return status;
			}
		}
		uint8 event = sTripEvent;
		sTripEvent = TRIP_EVENT_NONE;
		locker.Unlock();
		if (user_memcpy(buffer, &event, 1) != B_OK)
			return B_BAD_ADDRESS;
		*_length = 1;
		return B_OK;
	}

	size_t size = cookie->type == COOKIE_STATUS ? 4096 : 256;
	char* text = (char*)malloc(size);
	if (text == NULL)
		return B_NO_MEMORY;
	int length = cookie->type == COOKIE_STATUS ? status_text(text, size)
		: zone_text(cookie->zone, text, size);
	if (position >= length) {
		free(text);
		*_length = 0;
		return B_OK;
	}
	size_t count = min_c(*_length, (size_t)(length - position));
	status_t status = user_memcpy(buffer, text + position, count);
	free(text);
	if (status != B_OK)
		return B_BAD_ADDRESS;
	*_length = count;
	return B_OK;
}


/*!	"key value" lines to the status device change the settings.
*/
static status_t
thermal_write(void* _cookie, off_t position, const void* buffer,
	size_t* _length)
{
	thermal_cookie* cookie = (thermal_cookie*)_cookie;
	if (cookie->type != COOKIE_STATUS) {
		*_length = 0;
		return B_NOT_ALLOWED;
	}
	char text[256];
	size_t length = min_c(*_length, sizeof(text) - 1);
	if (user_memcpy(text, buffer, length) != B_OK)
		return B_BAD_ADDRESS;
	text[length] = '\0';

	status_t result = B_OK;
	char* line = text;
	while (line != NULL && *line != '\0') {
		char* next = strchr(line, '\n');
		if (next != NULL)
			*next++ = '\0';
		char* key = line;
		while (*key == ' ' || *key == '\t')
			key++;
		char* value = key;
		while (*value != '\0' && *value != ' ' && *value != '\t')
			value++;
		if (*value != '\0')
			*value++ = '\0';
		while (*value == ' ' || *value == '\t')
			value++;
		if (*key != '\0') {
			status_t status = apply_setting(key, value, true);
			if (status != B_OK) {
				TRACE("cannot set %s: %s\n", key, strerror(status));
				result = status;
			}
		}
		line = next;
	}
	*_length = length;
	return result;
}


static status_t
thermal_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	return B_DEV_INVALID_IOCTL;
}


static status_t
thermal_select(void* _cookie, uint8 event, selectsync* sync)
{
	thermal_cookie* cookie = (thermal_cookie*)_cookie;
	if (event != B_SELECT_READ)
		return B_BAD_VALUE;
	if (cookie->type != COOKIE_TRIP)
		return notify_select_event(sync, event);

	MutexLocker locker(sTripLock);
	status_t status = add_select_sync_pool_entry(&sTripSelectPool, sync,
		event);
	if (status != B_OK)
		return status;
	if (sTripEvent != TRIP_EVENT_NONE)
		notify_select_event(sync, event);
	return B_OK;
}


static status_t
thermal_deselect(void* _cookie, uint8 event, selectsync* sync)
{
	thermal_cookie* cookie = (thermal_cookie*)_cookie;
	if (cookie->type != COOKIE_TRIP)
		return B_OK;
	MutexLocker locker(sTripLock);
	return remove_select_sync_pool_entry(&sTripSelectPool, sync, event);
}


//	#pragma mark - driver


static float
thermal_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "fdt") != 0) {
		return 0.0f;
	}
	const char* compatible;
	if (sDeviceManager->get_attr_string(parent, "fdt/compatible", &compatible,
			false) != B_OK
		|| strcmp(compatible, "allwinner,sun60i-a733-ths") != 0) {
		return 0.0f;
	}
	return 1.0f;
}


static status_t
thermal_register_device(device_node* parent)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "Allwinner A733 thermal sensor" } },
		{ B_DEVICE_FLAGS, B_UINT32_TYPE, { .ui32 = B_KEEP_DRIVER_LOADED } },
		{}
	};
	return sDeviceManager->register_node(parent, DRIVER_MODULE_NAME, attrs,
		NULL, NULL);
}


static void
update_user_caps()
{
	if (sCpuFreq == NULL)
		return;
	for (int32 i = 0; i < sCpuFreq->ClusterCount(); i++) {
		Cluster& cluster = sCpuFreq->ClusterAt(i);
		uint32 mhz = cluster.pllOffset == 0x2000 ? sMaxBigMhz : sMaxLittleMhz;
		sClusterState[i].userCap = mhz != 0
			? cluster.IndexAtOrBelow(mhz * 1000) : cluster.pointCount - 1;
	}
}


static void
init_frequency_scaling(const void* fdt)
{
	if (get_module(SUNXI_PMIC_MODULE_NAME, (module_info**)&sPmic) != B_OK) {
		TRACE("no PMIC: no frequency scaling\n");
		sPmic = NULL;
		return;
	}
	CpuFreq* cpuFreq = new(std::nothrow) CpuFreq;
	if (cpuFreq == NULL)
		return;
	status_t status = cpuFreq->Init(fdt, sPmic);
	if (status != B_OK) {
		TRACE("no frequency scaling: %s\n", strerror(status));
		delete cpuFreq;
		put_module(SUNXI_PMIC_MODULE_NAME);
		sPmic = NULL;
		return;
	}
	for (int32 i = 0; i < cpuFreq->ClusterCount(); i++) {
		Cluster& cluster = cpuFreq->ClusterAt(i);
		ClusterState& state = sClusterState[i];
		memset(&state, 0, sizeof(state));
		state.thermalCap = cluster.pointCount - 1;
		state.userCap = cluster.pointCount - 1;
		for (int32 c = 0; c < cluster.cpuCount; c++)
			state.lastActive[c] = cpu_get_active_time(cluster.cpus[c]);
		TRACE("%s: measured %" B_PRIu32 " MHz at boot\n", cluster.name,
			cpuFreq->MeasureMegahertz(cluster));
	}
	cpuFreq->voltageSettlePercent = sVoltageSettlePercent;
	cpuFreq->waitPllRamp = sWaitPllRamp;
	sCpuFreq = cpuFreq;
	update_user_caps();

	if (!sDvfs)
		return;
	// from the boot speed to an operating point at or below it
	for (int32 i = 0; i < cpuFreq->ClusterCount(); i++) {
		Cluster& cluster = cpuFreq->ClusterAt(i);
		int32 index = cluster.IndexAtOrBelow(cluster.bootKilohertz);
		status = cpuFreq->SetPoint(cluster, index);
		TRACE("%s: to %" B_PRIu32 " MHz at %" B_PRIu32 " mV: %s (measured %"
			B_PRIu32 " MHz)\n", cluster.name,
			cluster.points[index].kilohertz / 1000, cluster.microvolts / 1000,
			strerror(status), cpuFreq->MeasureMegahertz(cluster));
		if (status != B_OK) {
			TRACE("frequency scaling off after the first change failed\n");
			sDvfs = false;
		}
	}
	cpuFreq->DumpRegisters("after the first change");
}


static status_t
thermal_init_driver(device_node* node, void** _cookie)
{
	const void* fdt = gFDT;
	if (fdt == NULL || fdt_check_header(fdt) != 0)
		return B_DEVICE_NOT_FOUND;

	device_node* parent = sDeviceManager->get_parent_node(node);
	uint32 thsNode = 0;
	fdt_device_module_info* fdtModule;
	fdt_device* fdtDevice;
	status_t status = sDeviceManager->get_attr_uint32(parent, "fdt/node",
		&thsNode, false);
	if (status == B_OK) {
		status = sDeviceManager->get_driver(parent,
			(driver_module_info**)&fdtModule, (void**)&fdtDevice);
	}
	uint64 base = 0, size = 0;
	if (status == B_OK && !fdtModule->get_reg(fdtDevice, 0, &base, &size))
		status = B_BAD_DATA;
	sDeviceManager->put_node(parent);
	if (status != B_OK)
		return status;

	sDvfs = true;
	read_zones(fdt, thsNode);
	read_settings();
	if (sRelease >= sPassive)
		sRelease = sPassive - 4000;

	uint32 calibration[3];
	status = read_calibration(fdt, thsNode, calibration);
	if (status != B_OK)
		TRACE("no calibration words: %s\n", strerror(status));
	status = sThs.Init(base, status == B_OK ? calibration : NULL);
	if (status != B_OK)
		return status;
	TRACE("THS at %#" B_PRIx64 ", %" B_PRId32 " zones, calibration %#"
		B_PRIx32 " %#" B_PRIx32 " %#" B_PRIx32 "\n", base, sZoneCount,
		sThs.CalibrationRegister(0), sThs.CalibrationRegister(1),
		sThs.CalibrationRegister(2));

	sTripCondition.Init(&sTripCondition, "sunxi_thermal trip");
	sCcuArea = map_physical_memory("sunxi_thermal ccu", kCcu, B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		(void**)&sCcu);
	if (sCcuArea < 0)
		sCcu = NULL;

	// the first conversions, of every sensor
	for (int32 i = 0; i < 50; i++) {
		int32 ready = 0;
		for (int32 zone = 0; zone < sZoneCount; zone++) {
			uint32 raw;
			if (sThs.ReadRaw(sZones[zone].sensor, &raw) == B_OK)
				ready++;
		}
		if (ready == sZoneCount)
			break;
		snooze(5000);
	}
	char passive[16], hot[16], critical[16];
	format_temperature(passive, sizeof(passive), sPassive);
	format_temperature(hot, sizeof(hot), sHot);
	format_temperature(critical, sizeof(critical), sCritical);
	sTemperature = kNoTemperature;
	thermal_step(system_time());
	for (int32 i = 0; i < sZoneCount; i++) {
		uint32 raw = 0;
		sThs.ReadRaw(sZones[i].sensor, &raw);
		char current[16];
		format_temperature(current, sizeof(current), sZones[i].temperature);
		TRACE("zone %" B_PRId32 " %s (sensor %" B_PRId32 "): %s C (code %"
			B_PRIu32 ")\n", i, sZones[i].name, sZones[i].sensor,
			sZones[i].valid ? current : "-", raw);
	}
	TRACE("trips: passive %s C, hot %s C, critical %s C\n", passive, hot,
		critical);

	init_frequency_scaling(fdt);

	add_debugger_command("sunxi_thermal", &dump_thermal,
		"the thermal driver's state and last frequency changes");

	sStop = false;
	sThread = spawn_kernel_thread(monitor_thread, "sunxi_thermal monitor",
		B_REAL_TIME_DISPLAY_PRIORITY, NULL);
	if (sThread < 0)
		return sThread;
	resume_thread(sThread);

	*_cookie = node;
	return B_OK;
}


static void
thermal_uninit_driver(void* cookie)
{
	remove_debugger_command("sunxi_thermal", &dump_thermal);
	if (sCcuArea >= 0)
		delete_area(sCcuArea);
	sCcuArea = -1;
	sCcu = NULL;
	sStop = true;
	if (sThread >= 0) {
		status_t result;
		wait_for_thread(sThread, &result);
	}
	sThread = -1;
	delete sCpuFreq;
	sCpuFreq = NULL;
	if (sPmic != NULL)
		put_module(SUNXI_PMIC_MODULE_NAME);
	sPmic = NULL;
}


static status_t
thermal_register_child_devices(void* cookie)
{
	device_node* node = (device_node*)cookie;
	char path[64];
	for (int32 i = 0; i < sZoneCount; i++) {
		snprintf(path, sizeof(path), "power/sunxi_thermal/%" B_PRId32, i);
		sDeviceManager->publish_device(node, path, DEVICE_MODULE_NAME);
	}
	sDeviceManager->publish_device(node, "power/sunxi_thermal/status",
		DEVICE_MODULE_NAME);
	return sDeviceManager->publish_device(node, "power/trip/sunxi_ths",
		DEVICE_MODULE_NAME);
}


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{}
};


static device_module_info sThermalDevice = {
	{
		DEVICE_MODULE_NAME,
		0,
		NULL
	},
	thermal_init_device,
	NULL,	// uninit
	NULL,	// removed
	thermal_open,
	thermal_close,
	thermal_free,
	thermal_read,
	thermal_write,
	NULL,	// io
	thermal_control,
	thermal_select,
	thermal_deselect
};


static driver_module_info sThermalDriver = {
	{
		DRIVER_MODULE_NAME,
		0,
		NULL
	},
	thermal_supports_device,
	thermal_register_device,
	thermal_init_driver,
	thermal_uninit_driver,
	thermal_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};


module_info* modules[] = {
	(module_info*)&sThermalDriver,
	(module_info*)&sThermalDevice,
	NULL
};
