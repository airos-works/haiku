/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _SUNXI_PMIC_H
#define _SUNXI_PMIC_H


/*	The power management IC of Allwinner A733 boards (X-Powers AXP318W, which
	Allwinner's BSP calls AXP8191) on the R_I2C0 bus, for the drivers that
	set its rails: the CPU clusters' voltage scaling (sunxi_thermal) and the
	GPU's supply (powervr). The module serializes them: the I2C controller
	takes one transfer at a time.

	It is only there where the device tree has the PMIC on R_I2C0; get_module()
	fails elsewhere. */


#include <module.h>


#define SUNXI_PMIC_MODULE_NAME	"drivers/power/sunxi_regulator/pmic_v1"


typedef struct sunxi_pmic_module_info {
	module_info	info;

	status_t	(*read)(uint8 reg, uint8* _value);
	status_t	(*write)(uint8 reg, uint8 value);
	// read-modify-write under the lock: the bits of mask are set to value
	status_t	(*update)(uint8 reg, uint8 mask, uint8 value);

	// A DC-DC converter's output (number as in the device tree: dcdc1 is 1).
	// Setting it refuses a voltage outside the device tree regulator's
	// regulator-min/max-microvolt or one the converter cannot make exactly,
	// and returns once the output had time to ramp there.
	status_t	(*get_dcdc_voltage)(uint32 dcdc, uint32* _microvolts);
	status_t	(*set_dcdc_voltage)(uint32 dcdc, uint32 microvolts);
} sunxi_pmic_module_info;


#endif	// _SUNXI_PMIC_H
