/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The PMIC of the Allwinner A733 boards (X-Powers AXP318W, AXP8191 in
	Allwinner's BSP) on R_I2C0, shared by the drivers that set its rails
	(sunxi_pmic.h). The module finds the PMIC in the device tree, and owns
	the bus: every access takes its lock.

	The DC-DC converters 2 to 5, 7 (the CPU clusters, the GPU, the system
	rail) take their output voltage in bits 6:0 of registers 0x13 to 0x16 and
	0x18: 500 mV + 10 mV steps up to 1200 mV (0x46), then 20 mV steps from
	1220 mV (0x47) to 1540 mV (0x57); bit 7 is kept as it is. DCDC1 (the
	3.3 V rail, 100 mV steps from 1 V) is read only. The outputs slew at
	250 uV/us (the BSP's regulator-ramp-delay); a change waits for that. */


#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <KernelExport.h>
#include <lock.h>
#include <util/AutoLock.h>

extern "C" {
#include <libfdt.h>
}

#include <sunxi_pmic.h>

// the display driver's R_TWI controller and R pin helpers
#include "registers.h"
#include "twi.h"


#define TRACE(x...)	dprintf("sunxi_pmic: " x)

extern void* gFDT;


// R_I2C0 on the A733: its gate and reset in the R clock controller, PL0 and
// PL1 in function 2
static const phys_addr_t kRTwi0Base = 0x07083000;
static const uint32 kRCcuTwiGate = 0x19c;
static const uint32 kRTwi0GateBit = 0;
// SCL = 24 MHz / (10 * (5 + 1) * 2^1) = 200 kHz (Linux runs it at 400 kHz)
static const uint32 kTwiClock = 5 << 3 | 1;

static const uint32 kMaxDcdc = 9;
static const uint32 kRampMicrovoltsPerMicrosecond = 250;
	// Linux: 250 uV/us, written as regulator-ramp-delay = <250>


static mutex sLock = MUTEX_INITIALIZER("sunxi pmic");
static sunxi::Twi* sTwi;
static uint8 sAddress;
static uint32 sMinMicrovolts[kMaxDcdc + 1];
static uint32 sMaxMicrovolts[kMaxDcdc + 1];


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


static bool
fdt_enabled(const void* fdt, int node)
{
	const char* status = (const char*)fdt_getprop(fdt, node, "status", NULL);
	return status == NULL || strcmp(status, "okay") == 0
		|| strcmp(status, "ok") == 0;
}


/*!	The PMIC's node: an AXP318W on the A733's R_I2C0, both enabled. Reads
	its address and the converters' limits.
*/
static status_t
find_pmic()
{
	const void* fdt = gFDT;
	if (fdt == NULL || fdt_check_header(fdt) != 0)
		return B_DEVICE_NOT_FOUND;

	int node = fdt_node_offset_by_compatible(fdt, -1, "x-powers,axp318w");
	if (node < 0 || !fdt_enabled(fdt, node))
		return B_DEVICE_NOT_FOUND;
	int bus = fdt_parent_offset(fdt, node);
	if (bus < 0 || !fdt_enabled(fdt, bus)
		|| fdt_node_check_compatible(fdt, bus, "allwinner,sun60i-a733-i2c")
			!= 0) {
		return B_DEVICE_NOT_FOUND;
	}
	// R_I2C0 sits in the soc node (one address and one size cell, mapped
	// one to one)
	int length;
	const fdt32_t* busReg = (const fdt32_t*)fdt_getprop(fdt, bus, "reg",
		&length);
	if (busReg == NULL || length < 4 || fdt32_to_cpu(busReg[0]) != kRTwi0Base)
		return B_DEVICE_NOT_FOUND;

	uint32 address = fdt_u32(fdt, node, "reg", 0);
	if (address == 0 || address > 0x7f)
		return B_BAD_DATA;
	sAddress = address;

	int regulators = fdt_subnode_offset(fdt, node, "regulators");
	for (uint32 i = 1; i <= kMaxDcdc; i++) {
		char name[8];
		snprintf(name, sizeof(name), "dcdc%" B_PRIu32, i);
		int regulator = regulators >= 0
			? fdt_subnode_offset(fdt, regulators, name) : -1;
		// without a node the converter is left alone
		sMinMicrovolts[i] = regulator >= 0
			? fdt_u32(fdt, regulator, "regulator-min-microvolt", 0) : 0;
		sMaxMicrovolts[i] = regulator >= 0
			? fdt_u32(fdt, regulator, "regulator-max-microvolt", 0) : 0;
	}
	return B_OK;
}


static status_t
ensure_bus()
{
	if (sTwi != NULL)
		return B_OK;

	sunxi::r_pin_set_function(0, 0, 2);	// PL0, PL1: R_I2C0
	sunxi::r_pin_set_function(0, 1, 2);
	sunxi::Twi* twi = new(std::nothrow) sunxi::Twi;
	if (twi == NULL)
		return B_NO_MEMORY;
	status_t status = twi->Init(kRTwi0Base, kRCcuTwiGate, kRTwi0GateBit,
		kTwiClock);
	if (status != B_OK) {
		delete twi;
		return status;
	}
	sTwi = twi;
	return B_OK;
}


static status_t
locked_read(uint8 reg, uint8* _value)
{
	status_t status = ensure_bus();
	if (status != B_OK)
		return status;
	// one retry: a transfer that fails resets the controller
	status = sTwi->Read8(sAddress, reg, *_value);
	if (status != B_OK)
		status = sTwi->Read8(sAddress, reg, *_value);
	return status;
}


static status_t
locked_write(uint8 reg, uint8 value)
{
	status_t status = ensure_bus();
	if (status != B_OK)
		return status;
	for (int attempt = 0; attempt < 2; attempt++) {
		status = sTwi->Write8(sAddress, reg, value);
		uint8 check;
		if (status == B_OK)
			status = sTwi->Read8(sAddress, reg, check);
		if (status == B_OK && check != value)
			status = B_IO_ERROR;
		if (status == B_OK)
			return B_OK;
	}
	TRACE("register %#x: writing %#x failed: %s\n", reg, value,
		strerror(status));
	return status;
}


static uint8
dcdc_register(uint32 dcdc)
{
	return 0x12 + (dcdc - 1);
}


static bool
dcdc_supported(uint32 dcdc)
{
	return dcdc >= 2 && dcdc <= 5;
}


static uint32
dcdc_microvolts(uint8 code)
{
	code &= 0x7f;
	if (code <= 0x46)
		return 500000 + code * 10000;
	return 1220000 + (code - 0x47) * 20000;
}


static bool
dcdc_code(uint32 microvolts, uint8* _code)
{
	if (microvolts >= 500000 && microvolts <= 1200000
		&& (microvolts - 500000) % 10000 == 0) {
		*_code = (microvolts - 500000) / 10000;
		return true;
	}
	if (microvolts >= 1220000 && microvolts <= 1540000
		&& (microvolts - 1220000) % 20000 == 0) {
		*_code = 0x47 + (microvolts - 1220000) / 20000;
		return true;
	}
	return false;
}


//	#pragma mark - module API


static status_t
pmic_read(uint8 reg, uint8* _value)
{
	MutexLocker locker(sLock);
	return locked_read(reg, _value);
}


static status_t
pmic_write(uint8 reg, uint8 value)
{
	MutexLocker locker(sLock);
	return locked_write(reg, value);
}


static status_t
pmic_update(uint8 reg, uint8 mask, uint8 value)
{
	MutexLocker locker(sLock);
	uint8 current;
	status_t status = locked_read(reg, &current);
	if (status != B_OK)
		return status;
	uint8 wanted = (current & ~mask) | (value & mask);
	if (wanted == current)
		return B_OK;
	return locked_write(reg, wanted);
}


static status_t
pmic_get_dcdc_voltage(uint32 dcdc, uint32* _microvolts)
{
	if (!dcdc_supported(dcdc))
		return B_BAD_VALUE;
	MutexLocker locker(sLock);
	uint8 code;
	status_t status = locked_read(dcdc_register(dcdc), &code);
	if (status == B_OK)
		*_microvolts = dcdc_microvolts(code);
	return status;
}


static status_t
pmic_set_dcdc_voltage(uint32 dcdc, uint32 microvolts)
{
	if (!dcdc_supported(dcdc))
		return B_BAD_VALUE;
	uint8 code;
	if (sMaxMicrovolts[dcdc] == 0 || microvolts < sMinMicrovolts[dcdc]
		|| microvolts > sMaxMicrovolts[dcdc] || !dcdc_code(microvolts, &code)) {
		TRACE("dcdc%" B_PRIu32 ": %" B_PRIu32 " uV refused (limits %" B_PRIu32
			" to %" B_PRIu32 " uV)\n", dcdc, microvolts, sMinMicrovolts[dcdc],
			sMaxMicrovolts[dcdc]);
		return B_BAD_VALUE;
	}

	MutexLocker locker(sLock);
	uint8 current;
	status_t status = locked_read(dcdc_register(dcdc), &current);
	if (status != B_OK)
		return status;
	if ((current & 0x7f) == code)
		return B_OK;
	uint32 before = dcdc_microvolts(current);
	status = locked_write(dcdc_register(dcdc), (current & 0x80) | code);
	locker.Unlock();
	if (status != B_OK)
		return status;

	// the output slews to it; with some margin
	uint32 delta = before > microvolts ? before - microvolts
		: microvolts - before;
	snooze(delta / kRampMicrovoltsPerMicrosecond + 50);
	return B_OK;
}


static status_t
pmic_std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
		{
			status_t status = find_pmic();
			if (status != B_OK)
				return status;
			MutexLocker locker(sLock);
			uint8 chip;
			status = locked_read(0x03, &chip);
			if (status != B_OK) {
				TRACE("no answer from the PMIC at %#x: %s\n", sAddress,
					strerror(status));
				delete sTwi;
				sTwi = NULL;
				return status;
			}
			TRACE("AXP318W at %#x on R_I2C0 (IC type %#x)\n", sAddress, chip);
			return B_OK;
		}

		case B_MODULE_UNINIT:
			delete sTwi;
			sTwi = NULL;
			return B_OK;
	}
	return B_ERROR;
}


sunxi_pmic_module_info gSunxiPmicModule = {
	{
		SUNXI_PMIC_MODULE_NAME,
		0,
		pmic_std_ops
	},
	pmic_read,
	pmic_write,
	pmic_update,
	pmic_get_dcdc_voltage,
	pmic_set_dcdc_voltage
};
