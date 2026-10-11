/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "CpuFreq.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <KernelExport.h>

#include <cpu.h>
#include <smp.h>
#include <vm/vm.h>

extern "C" {
#include <libfdt.h>
}


#define TRACE(x...)	dprintf("sunxi_thermal: " x)


namespace sunxi_thermal {


/*	The CPU PLL block (0x08870000): a PLL per cluster and one for the DSU at
	0x1000, 0x2000 and 0x3000, each with its pattern and linear frequency
	modulation registers and, at +0x1c, the clock of the cores it feeds.
	Output = crystal * N / P (Allwinner's BSP: ccu-sun60iw2-cpupll.c). */
enum {
	PLL_CONTROL			= 0x00,
	PLL_PATTERN0		= 0x04,
	PLL_PATTERN1		= 0x08,
	PLL_LFM				= 0x18,
	PLL_CPU_CLOCK		= 0x1c,		// [26:24] source (3 = the PLL),
									// [17:16] P = 1 << value
	PLL_ENABLE			= 1u << 31,
	PLL_LDO_ENABLE		= 1u << 30,
	PLL_LOCK_ENABLE		= 1u << 29,
	PLL_LOCKED			= 1u << 28,
	PLL_OUTPUT_ENABLE	= 1u << 27,
	PLL_UPDATE			= 1u << 26,	// latches the new setting, self-clearing
	PLL_N_SHIFT			= 8,
	PLL_N_MASK			= 0xffu << PLL_N_SHIFT,

	PATTERN0_TRIANGULAR	= 3u << 29,
	PATTERN1_LFM_ENABLE	= 1u << 31,
	LFM_ENABLE			= 1u << 31,
	LFM_STEP			= 1u << 8,	// 1.125 MHz per microsecond
	LFM_BYPASS			= 1u << 0,

	CPU_SOURCE_SHIFT	= 24,
	CPU_SOURCE_MASK		= 7u << CPU_SOURCE_SHIFT,
	CPU_SOURCE_PLL		= 3,
	CPU_P_SHIFT			= 16,
	CPU_P_MASK			= 3u << CPU_P_SHIFT
};

static const uint32 kPllBlockSize = 0x4000;
// the PLLs' VCO range (the BSP's N limits: 676 MHz to 2.2 GHz)
static const uint32 kMinVcoKilohertz = 672000;
static const uint32 kMaxVcoKilohertz = 2184000;

// The speed grades Allwinner's BSP knows (the eFuse code, nibbles swapped,
// names the voltage column); others take its default column vf0100.
static const uint8 kKnownSpeedCodes[] = {
	0x00, 0x01, 0x11, 0x02, 0x12, 0x03, 0x04, 0x05, 0x06, 0x14, 0x15
};


//	#pragma mark - device tree helpers


static uint32
fdt_u32(const void* fdt, int node, const char* name, uint32 fallback)
{
	int length;
	const fdt32_t* value = (const fdt32_t*)fdt_getprop(fdt, node, name,
		&length);
	if (value == NULL || length < 4)
		return fallback;
	return fdt32_to_cpu(*value);
}


static int
fdt_phandle_node(const void* fdt, int node, const char* name, int32 cell,
	uint32* _argument = NULL)
{
	int length;
	const fdt32_t* value = (const fdt32_t*)fdt_getprop(fdt, node, name,
		&length);
	if (value == NULL || length < (int)(cell + 1) * 4)
		return -1;
	if (_argument != NULL) {
		*_argument = length >= (int)(cell + 2) * 4
			? fdt32_to_cpu(value[cell + 1]) : 0;
	}
	return fdt_node_offset_by_phandle(fdt, fdt32_to_cpu(value[cell]));
}


/*!	The node's first register address, through its parent's address cells
	(the soc node maps one to one).
*/
static bool
fdt_reg_address(const void* fdt, int node, phys_addr_t* _address)
{
	int parent = fdt_parent_offset(fdt, node);
	if (parent < 0)
		return false;
	uint32 addressCells = fdt_u32(fdt, parent, "#address-cells", 2);
	int length;
	const fdt32_t* reg = (const fdt32_t*)fdt_getprop(fdt, node, "reg",
		&length);
	if (reg == NULL || length < (int)addressCells * 4 || addressCells > 2)
		return false;
	phys_addr_t address = fdt32_to_cpu(reg[0]);
	if (addressCells == 2)
		address = address << 32 | fdt32_to_cpu(reg[1]);
	*_address = address;
	return true;
}


//	#pragma mark - Cluster


int32
Cluster::IndexAtOrBelow(uint32 kilohertz) const
{
	int32 index = 0;
	for (int32 i = 0; i < pointCount; i++) {
		if (points[i].kilohertz <= kilohertz)
			index = i;
	}
	return index;
}


int32
Cluster::IndexAtOrAbove(uint32 kilohertz) const
{
	for (int32 i = 0; i < pointCount; i++) {
		if (points[i].kilohertz >= kilohertz)
			return i;
	}
	return pointCount - 1;
}


//	#pragma mark - CpuFreq


CpuFreq::CpuFreq()
	:
	voltageSettlePercent(100),
	waitPllRamp(false),
	fPmic(NULL),
	fPllArea(-1),
	fPll(NULL),
	fParentHz(0),
	fClusterCount(0),
	fTraceNext(0)
{
	fSpeedGrade[0] = '\0';
	memset(fClusters, 0, sizeof(fClusters));
	memset(fTrace, 0, sizeof(fTrace));
}


CpuFreq::~CpuFreq()
{
	if (fPllArea >= 0)
		delete_area(fPllArea);
}


status_t
CpuFreq::Init(const void* fdt, sunxi_pmic_module_info* pmic)
{
	fPmic = pmic;
	status_t status = _ParseClusters(fdt);
	if (status != B_OK)
		return status;
	DumpRegisters("at boot");

	for (int32 i = 0; i < fClusterCount; i++) {
		Cluster& cluster = fClusters[i];
		cluster.current = -1;
		cluster.bootKilohertz = PllKilohertz(cluster.pllOffset);
		status = fPmic->get_dcdc_voltage(cluster.dcdc, &cluster.bootMicrovolts);
		if (status != B_OK) {
			TRACE("%s: rail dcdc%" B_PRIu32 " unreadable: %s\n", cluster.name,
				cluster.dcdc, strerror(status));
			return status;
		}
		cluster.microvolts = cluster.bootMicrovolts;
		if (cluster.bootKilohertz == 0) {
			TRACE("%s: the cores do not run from their PLL (%#" B_PRIx32
				"), no frequency scaling\n", cluster.name,
				_Read(cluster.pllOffset + PLL_CPU_CLOCK));
			return B_NOT_SUPPORTED;
		}
	}
	_ComputeDsuFloor(fdt);

	for (int32 i = 0; i < fClusterCount; i++) {
		Cluster& cluster = fClusters[i];
		TRACE("%s: cpus %" B_PRId32 "-%" B_PRId32 ", %" B_PRId32
			" operating points %" B_PRIu32 "-%" B_PRIu32 " MHz (%s), boot %"
			B_PRIu32 " MHz at %" B_PRIu32 " mV (dcdc%" B_PRIu32 ", floor %"
			B_PRIu32 " mV)\n", cluster.name, cluster.cpus[0],
			cluster.cpus[cluster.cpuCount - 1], cluster.pointCount,
			cluster.points[0].kilohertz / 1000,
			cluster.points[cluster.pointCount - 1].kilohertz / 1000,
			fSpeedGrade, cluster.bootKilohertz / 1000,
			cluster.bootMicrovolts / 1000, cluster.dcdc,
			cluster.floorMicrovolts / 1000);
	}

	for (int32 i = 0; i < fClusterCount; i++)
		_InitPll(fClusters[i].pllOffset);
	return B_OK;
}


status_t
CpuFreq::_ParseClusters(const void* fdt)
{
	int cpus = fdt_path_offset(fdt, "/cpus");
	if (cpus < 0)
		return B_DEVICE_NOT_FOUND;
	uint32 addressCells = fdt_u32(fdt, cpus, "#address-cells", 1);

	int node;
	fdt_for_each_subnode(node, fdt, cpus) {
		const char* type = (const char*)fdt_getprop(fdt, node, "device_type",
			NULL);
		if (type == NULL || strcmp(type, "cpu") != 0)
			continue;

		int length;
		const fdt32_t* reg = (const fdt32_t*)fdt_getprop(fdt, node, "reg",
			&length);
		if (reg == NULL || length < (int)addressCells * 4)
			continue;
		uint64 mpidr = fdt32_to_cpu(reg[addressCells - 1]);
		if (addressCells == 2)
			mpidr |= (uint64)fdt32_to_cpu(reg[0]) << 32;

		uint32 clockIndex;
		int pll = fdt_phandle_node(fdt, node, "clocks", 0, &clockIndex);
		int table = fdt_phandle_node(fdt, node, "operating-points-v2", 0);
		if (pll < 0 || table < 0 || (clockIndex != 1 && clockIndex != 2)
			|| fdt_node_check_compatible(fdt, pll,
				"allwinner,sun60i-a733-cpupll") != 0) {
			// a core without frequency scaling: leave all of them alone
			TRACE("cpu %s: no PLL or operating points\n",
				fdt_get_name(fdt, node, NULL));
			return B_NOT_SUPPORTED;
		}

		int32 haikuCpu = -1;
		for (int32 i = 0; i < smp_get_num_cpus(); i++) {
			if ((gCPU[i].arch.mpidr & 0xff00ffffffull) == mpidr) {
				haikuCpu = i;
				break;
			}
		}
		if (haikuCpu < 0) {
			// not started (smp disabled): not ours to measure, the PLL is
			// still the cluster's
			continue;
		}

		if (fPll == NULL) {
			phys_addr_t base;
			int crystal = fdt_phandle_node(fdt, pll, "clocks", 0);
			fParentHz = crystal >= 0
				? fdt_u32(fdt, crystal, "clock-frequency", 0) : 0;
			if (!fdt_reg_address(fdt, pll, &base) || fParentHz == 0)
				return B_BAD_DATA;
			fPllArea = map_physical_memory("sunxi_thermal cpu pll", base,
				kPllBlockSize, B_ANY_KERNEL_ADDRESS,
				B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&fPll);
			if (fPllArea < 0)
				return fPllArea;
		}

		uint32 pllOffset = clockIndex * 0x1000;
		Cluster* cluster = NULL;
		for (int32 i = 0; i < fClusterCount; i++) {
			if (fClusters[i].pllOffset == pllOffset)
				cluster = &fClusters[i];
		}
		if (cluster == NULL) {
			if (fClusterCount == kMaxClusters)
				return B_BAD_DATA;
			cluster = &fClusters[fClusterCount++];
			strlcpy(cluster->name, clockIndex == 1 ? "little" : "big",
				sizeof(cluster->name));
			cluster->pllOffset = pllOffset;
			cluster->dcdc = _RailOf(fdt, node, "cpu-supply");
			if (cluster->dcdc == 0)
				return B_NOT_SUPPORTED;
			status_t status = _ReadSpeedGrade(fdt, table);
			if (status == B_OK) {
				status = _ParseTable(fdt, table, cluster->points,
					&cluster->pointCount);
			}
			if (status != B_OK)
				return status;
		} else if (cluster->dcdc == 0) {
			cluster->dcdc = _RailOf(fdt, node, "cpu-supply");
		}
		if (cluster->cpuCount < Cluster::kMaxCpus)
			cluster->cpus[cluster->cpuCount++] = haikuCpu;
	}
	return fClusterCount > 0 ? B_OK : B_DEVICE_NOT_FOUND;
}


/*!	The speed grade from the eFuse cell the table names ("speed"): the code
	in bits 15:8 of the word, or a later one in bits 23:16 when it is set.
*/
status_t
CpuFreq::_ReadSpeedGrade(const void* fdt, int table)
{
	if (fSpeedGrade[0] != '\0')
		return B_OK;

	int cell = fdt_phandle_node(fdt, table, "nvmem-cells", 0);
	int block = cell >= 0 ? fdt_parent_offset(fdt, cell) : -1;
	phys_addr_t base;
	int length;
	const fdt32_t* reg = cell >= 0
		? (const fdt32_t*)fdt_getprop(fdt, cell, "reg", &length) : NULL;
	if (block < 0 || reg == NULL || length < 8
		|| !fdt_reg_address(fdt, block, &base)) {
		TRACE("no speed grade cell\n");
		return B_BAD_DATA;
	}
	phys_addr_t address = base + fdt32_to_cpu(reg[0]);

	volatile uint8* page;
	area_id area = map_physical_memory("sunxi_thermal sid",
		address & ~(phys_addr_t)(B_PAGE_SIZE - 1), B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA, (void**)&page);
	if (area < 0)
		return area;
	uint32 word = *(volatile uint32*)(page + (address & (B_PAGE_SIZE - 1)));
	delete_area(area);

	uint8 code = (word >> 16) & 0xff;
	if (code == 0)
		code = (word >> 8) & 0xff;
	uint32 column = 0x0100;
	for (size_t i = 0; i < B_COUNT_OF(kKnownSpeedCodes); i++) {
		if (kKnownSpeedCodes[i] == code)
			column = (code & 0x0f) << 8 | (code & 0xf0) >> 4;
	}
	snprintf(fSpeedGrade, sizeof(fSpeedGrade), "vf%04" B_PRIx32, column);
	TRACE("speed grade code %#x (eFuse %#" B_PRIx32 "): voltages %s\n", code,
		word, fSpeedGrade);
	return B_OK;
}


status_t
CpuFreq::_ParseTable(const void* fdt, int table, OperatingPoint* points,
	int32* _count)
{
	char property[32];
	snprintf(property, sizeof(property), "opp-microvolt-%s", fSpeedGrade);

	int32 count = 0;
	int node;
	fdt_for_each_subnode(node, fdt, table) {
		int length;
		const fdt32_t* hz = (const fdt32_t*)fdt_getprop(fdt, node, "opp-hz",
			&length);
		uint32 microvolts = fdt_u32(fdt, node, property, 0);
		if (hz == NULL || length != 8 || microvolts == 0)
			continue;
		uint64 frequency = (uint64)fdt32_to_cpu(hz[0]) << 32
			| fdt32_to_cpu(hz[1]);
		if (count == Cluster::kMaxOperatingPoints)
			break;

		// in order of frequency
		OperatingPoint point = { (uint32)(frequency / 1000), microvolts };
		int32 at = count;
		while (at > 0 && points[at - 1].kilohertz > point.kilohertz) {
			points[at] = points[at - 1];
			at--;
		}
		points[at] = point;
		count++;
	}
	if (count == 0) {
		TRACE("%s: no operating points for %s\n",
			fdt_get_name(fdt, table, NULL), fSpeedGrade);
		return B_BAD_DATA;
	}
	*_count = count;
	return B_OK;
}


/*!	The number of the PMIC converter a supply property points at (the dcdcN
	nodes of the AXP318W's regulators), 0 if it is not one.
*/
uint32
CpuFreq::_RailOf(const void* fdt, int node, const char* property)
{
	int regulator = fdt_phandle_node(fdt, node, property, 0);
	if (regulator < 0)
		return 0;
	int regulators = fdt_parent_offset(fdt, regulator);
	int pmic = regulators >= 0 ? fdt_parent_offset(fdt, regulators) : -1;
	if (pmic < 0 || fdt_node_check_compatible(fdt, pmic, "x-powers,axp318w")
			!= 0) {
		return 0;
	}
	const char* name = fdt_get_name(fdt, regulator, NULL);
	if (name == NULL || strncmp(name, "dcdc", 4) != 0 || name[4] < '1'
		|| name[4] > '9') {
		return 0;
	}
	return strtoul(name + 4, NULL, 10);
}


/*!	The DSU (the cores' shared L3 and its bus) runs from its own PLL but
	shares the little cluster's rail: that rail never goes below what the
	DSU's operating points ask for its present frequency, nor below the boot
	voltage when that cannot be told.
*/
void
CpuFreq::_ComputeDsuFloor(const void* fdt)
{
	int dsu = fdt_node_offset_by_compatible(fdt, -1,
		"allwinner,sun60i-a733-dsu");
	uint32 rail = dsu >= 0 ? _RailOf(fdt, dsu, "dsu-supply") : 0;
	OperatingPoint points[Cluster::kMaxOperatingPoints];
	int32 count = 0;
	int table = dsu >= 0
		? fdt_phandle_node(fdt, dsu, "operating-points-v2", 0) : -1;
	if (table < 0 || _ParseTable(fdt, table, points, &count) != B_OK)
		count = 0;
	uint32 dsuKilohertz = DsuKilohertz();

	uint32 floor = 0;
	for (int32 i = 0; i < count; i++) {
		if (points[i].kilohertz >= dsuKilohertz) {
			floor = points[i].microvolts;
			break;
		}
	}

	for (int32 i = 0; i < fClusterCount; i++) {
		Cluster& cluster = fClusters[i];
		if (dsu < 0 || rail == 0) {
			// no description: assume the rail is shared, keep the boot level
			if (cluster.pllOffset == 0x1000)
				cluster.floorMicrovolts = cluster.bootMicrovolts;
			continue;
		}
		if (cluster.dcdc != rail)
			continue;
		cluster.floorMicrovolts = floor != 0 && dsuKilohertz != 0
			? floor : cluster.bootMicrovolts;
	}
	TRACE("DSU: %" B_PRIu32 " MHz on dcdc%" B_PRIu32 ", needs %" B_PRIu32
		" mV\n", dsuKilohertz / 1000, rail, floor / 1000);
}


void
CpuFreq::_Write(uint32 offset, uint32 value)
{
	*(volatile uint32*)(fPll + offset) = value;
	memory_full_barrier();
}


status_t
CpuFreq::_WaitClear(uint32 offset, uint32 bit, bigtime_t timeout)
{
	bigtime_t end = system_time() + timeout;
	while ((_Read(offset) & bit) != 0) {
		if (system_time() > end)
			return B_TIMED_OUT;
		spin(2);
	}
	return B_OK;
}


uint32
CpuFreq::PllKilohertz(uint32 pllOffset)
{
	uint32 clock = _Read(pllOffset + PLL_CPU_CLOCK);
	if (((clock & CPU_SOURCE_MASK) >> CPU_SOURCE_SHIFT) != CPU_SOURCE_PLL)
		return 0;
	uint32 n = (_Read(pllOffset + PLL_CONTROL) & PLL_N_MASK) >> PLL_N_SHIFT;
	uint32 p = 1u << ((clock & CPU_P_MASK) >> CPU_P_SHIFT);
	return (uint32)((uint64)fParentHz * n / p / 1000);
}


/*!	As Allwinner's driver sets the PLLs up before it changes them: the
	pattern registers for a triangular spread and linear frequency
	modulation at 1.125 MHz/us, so that a new N ramps instead of jumping.
*/
void
CpuFreq::_InitPll(uint32 pllOffset)
{
	uint32 pattern0 = _Read(pllOffset + PLL_PATTERN0);
	uint32 pattern1 = _Read(pllOffset + PLL_PATTERN1);
	uint32 lfm = _Read(pllOffset + PLL_LFM);
	uint32 wantedLfm = (lfm & ~LFM_BYPASS) | LFM_ENABLE | LFM_STEP;
	bool changed = false;
	if ((pattern0 & PATTERN0_TRIANGULAR) != PATTERN0_TRIANGULAR) {
		_Write(pllOffset + PLL_PATTERN0, pattern0 | PATTERN0_TRIANGULAR);
		changed = true;
	}
	if ((pattern1 & PATTERN1_LFM_ENABLE) == 0) {
		_Write(pllOffset + PLL_PATTERN1, pattern1 | PATTERN1_LFM_ENABLE);
		changed = true;
	}
	if (lfm != wantedLfm) {
		_Write(pllOffset + PLL_LFM, wantedLfm);
		changed = true;
	}
	uint32 control = _Read(pllOffset + PLL_CONTROL);
	uint32 enables = PLL_ENABLE | PLL_LDO_ENABLE | PLL_LOCK_ENABLE
		| PLL_OUTPUT_ENABLE;
	if ((control & enables) != enables) {
		_Write(pllOffset + PLL_CONTROL, control | enables);
		changed = true;
	}
	if (!changed)
		return;

	_Write(pllOffset + PLL_CONTROL,
		_Read(pllOffset + PLL_CONTROL) | PLL_UPDATE);
	status_t status = _WaitClear(pllOffset + PLL_CONTROL, PLL_UPDATE, 10000);
	TRACE("PLL %#" B_PRIx32 ": modulation set up (%#" B_PRIx32 " %#" B_PRIx32
		" %#" B_PRIx32 " -> %#" B_PRIx32 " %#" B_PRIx32 " %#" B_PRIx32 "): %s\n",
		pllOffset, pattern0, pattern1, lfm,
		_Read(pllOffset + PLL_PATTERN0), _Read(pllOffset + PLL_PATTERN1),
		_Read(pllOffset + PLL_LFM), strerror(status));
}


status_t
CpuFreq::_SetPll(uint32 pllOffset, uint32 kilohertz)
{
	// N for the smallest post divider that makes it exactly
	uint32 parentKilohertz = fParentHz / 1000;
	uint32 n = 0, pShift = 0;
	for (uint32 shift = 0; shift <= 2; shift++) {
		uint32 vco = kilohertz << shift;
		if (vco % parentKilohertz != 0 || vco < kMinVcoKilohertz
			|| vco > kMaxVcoKilohertz) {
			continue;
		}
		n = vco / parentKilohertz;
		pShift = shift;
		break;
	}
	if (n == 0 || n > 0xff)
		return B_BAD_VALUE;

	uint32 clockOffset = pllOffset + PLL_CPU_CLOCK;
	uint32 clock = _Read(clockOffset);
	uint32 oldShift = (clock & CPU_P_MASK) >> CPU_P_SHIFT;
	// a larger divider first, a smaller one last: the cores never run
	// faster than the faster of the two settings
	if (pShift > oldShift)
		_Write(clockOffset, (clock & ~CPU_P_MASK) | pShift << CPU_P_SHIFT);

	uint32 oldN = (_Read(pllOffset + PLL_CONTROL) & PLL_N_MASK) >> PLL_N_SHIFT;
	uint32 control = (_Read(pllOffset + PLL_CONTROL) & ~(PLL_N_MASK
		| PLL_UPDATE)) | n << PLL_N_SHIFT;
	_Write(pllOffset + PLL_CONTROL, control);
	_Write(pllOffset + PLL_CONTROL, control | PLL_UPDATE);
	status_t status = _WaitClear(pllOffset + PLL_CONTROL, PLL_UPDATE, 10000);

	if (pShift < oldShift) {
		_Write(clockOffset,
			(_Read(clockOffset) & ~CPU_P_MASK) | pShift << CPU_P_SHIFT);
	}
	if (status != B_OK)
		return status;

	bigtime_t end = system_time() + 70000;
	while ((_Read(pllOffset + PLL_CONTROL) & PLL_LOCKED) == 0) {
		if (system_time() > end)
			return B_TIMED_OUT;
		spin(2);
	}
	if (waitPllRamp) {
		// the VCO slews at 1.125 MHz per microsecond
		uint32 delta = (n > oldN ? n - oldN : oldN - n) * parentKilohertz;
		snooze(delta / 1125 + 20);
	}
	return B_OK;
}


void
CpuFreq::_Trace(const Cluster& cluster, const char* step, uint32 kilohertz,
	uint32 microvolts)
{
	TraceEntry& entry = fTrace[fTraceNext];
	fTraceNext = (fTraceNext + 1) % kTraceSize;
	entry.time = system_time();
	entry.step = step;
	entry.cluster = cluster.name;
	entry.kilohertz = kilohertz;
	entry.microvolts = microvolts;
}


void
CpuFreq::DumpTrace()
{
	kprintf("now %" B_PRId64 " us\n", system_time());
	for (int32 i = 0; i < kTraceSize; i++) {
		const TraceEntry& entry = fTrace[(fTraceNext + i) % kTraceSize];
		if (entry.step == NULL)
			continue;
		kprintf("%12" B_PRId64 " %-6s %-14s %5" B_PRIu32 " MHz %4" B_PRIu32
			" mV\n", entry.time, entry.cluster, entry.step,
			entry.kilohertz / 1000, entry.microvolts / 1000);
	}
	for (int32 i = 0; i < fClusterCount; i++) {
		Cluster& cluster = fClusters[i];
		kprintf("%s: PLL %" B_PRIu32 " MHz (control %#" B_PRIx32 ", clock %#"
			B_PRIx32 "), %" B_PRIu32 " mV\n", cluster.name,
			PllKilohertz(cluster.pllOffset) / 1000,
			_Read(cluster.pllOffset + PLL_CONTROL),
			_Read(cluster.pllOffset + PLL_CPU_CLOCK),
			cluster.microvolts / 1000);
	}
}


status_t
CpuFreq::SetPoint(Cluster& cluster, int32 index)
{
	if (index < 0 || index >= cluster.pointCount)
		return B_BAD_INDEX;
	if (index == cluster.current)
		return B_OK;

	const OperatingPoint& point = cluster.points[index];
	uint32 microvolts = max_c(point.microvolts, cluster.floorMicrovolts);

	_Trace(cluster, "start", cluster.CurrentKilohertz(), cluster.microvolts);
	if (microvolts > cluster.microvolts) {
		status_t status = fPmic->set_dcdc_voltage(cluster.dcdc, microvolts);
		if (status != B_OK) {
			cluster.errors++;
			return status;
		}
		if (voltageSettlePercent > 100) {
			// beyond the PMIC module's wait for the nominal 250 uV/us
			snooze((microvolts - cluster.microvolts) / 250
				* (voltageSettlePercent - 100) / 100);
		}
		cluster.microvolts = microvolts;
		_Trace(cluster, "voltage up", cluster.CurrentKilohertz(), microvolts);
	}

	status_t status = _SetPll(cluster.pllOffset, point.kilohertz);
	if (status != B_OK) {
		cluster.errors++;
		TRACE("%s: PLL to %" B_PRIu32 " MHz: %s\n", cluster.name,
			point.kilohertz / 1000, strerror(status));
		// it may have moved: the voltage stays where it is (high enough
		// for both)
		cluster.current = -1;
		cluster.bootKilohertz = PllKilohertz(cluster.pllOffset);
		return status;
	}
	cluster.current = index;
	cluster.transitions++;
	_Trace(cluster, "pll", point.kilohertz, cluster.microvolts);

	if (microvolts < cluster.microvolts) {
		// a failure leaves it higher than needed, which is safe
		if (fPmic->set_dcdc_voltage(cluster.dcdc, microvolts) == B_OK)
			cluster.microvolts = microvolts;
		else
			cluster.errors++;
		_Trace(cluster, "voltage down", point.kilohertz, cluster.microvolts);
	}
	return B_OK;
}


struct measure_request {
	uint64		iterations;
	bigtime_t	elapsed;
};


static void
measure_on_cpu(void* _request, int cpu)
{
	measure_request* request = (measure_request*)_request;
	uint64 value = 0;
	uint64 count = request->iterations;
	bigtime_t start = system_time();
	// eight dependent adds an iteration: one cycle each on both core types
	asm volatile(
		"1:\n"
		"	add %0, %0, #1\n"
		"	add %0, %0, #1\n"
		"	add %0, %0, #1\n"
		"	add %0, %0, #1\n"
		"	add %0, %0, #1\n"
		"	add %0, %0, #1\n"
		"	add %0, %0, #1\n"
		"	add %0, %0, #1\n"
		"	subs %1, %1, #1\n"
		"	b.ne 1b\n"
		: "+r" (value), "+r" (count) : : "cc");
	request->elapsed = system_time() - start;
}


uint32
CpuFreq::MeasureMegahertz(const Cluster& cluster)
{
	if (cluster.cpuCount == 0)
		return 0;
	measure_request request = { 100000, 0 };
	call_single_cpu_sync(cluster.cpus[0], measure_on_cpu, &request);
	if (request.elapsed <= 0)
		return 0;
	return (uint32)(request.iterations * 8 / request.elapsed);
}


void
CpuFreq::DumpRegisters(const char* when)
{
	if (fPll == NULL)
		return;
	TRACE("CPU PLLs %s: back %#" B_PRIx32 "; little %#" B_PRIx32 " %#"
		B_PRIx32 " %#" B_PRIx32 " %#" B_PRIx32 " clock %#" B_PRIx32 "; big %#"
		B_PRIx32 " %#" B_PRIx32 " %#" B_PRIx32 " %#" B_PRIx32 " clock %#"
		B_PRIx32 "; DSU %#" B_PRIx32 " clock %#" B_PRIx32 "\n", when,
		_Read(0x0000), _Read(0x1000), _Read(0x1004), _Read(0x1008),
		_Read(0x1018), _Read(0x101c), _Read(0x2000), _Read(0x2004),
		_Read(0x2008), _Read(0x2018), _Read(0x201c), _Read(0x3000),
		_Read(0x301c));
}


}	// namespace sunxi_thermal
