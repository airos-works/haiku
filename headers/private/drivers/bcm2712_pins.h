/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BCM2712_PINS_H
#define _BCM2712_PINS_H


/*!	The Raspberry Pi 5's (BCM2712) pin multiplexer and its "gio" GPIOs, for
	the drivers of the on-board Wi-Fi/Bluetooth chip, whose pins the firmware
	does not set up (Linux: pinctrl-bcm2712.c, gpio-brcmstb.c). Only the
	pins those drivers use are described: GPIO 24 to 35.

	The multiplexer has a four bit function select per pin (0: GPIO, n:
	alternate function n) and a two bit pull per pin; where they are differs
	between the C0 and the D0 stepping of the chip, whose device trees say
	"brcm,bcm2712c0-pinctrl" (or "brcm,bcm2712-pinctrl") and
	"brcm,bcm2712d0-pinctrl". */


#include <string.h>

#include <KernelExport.h>
#include <bus/FDT.h>
#include <device_manager.h>


#define BCM2712_PULL_NONE	0
#define BCM2712_PULL_DOWN	1
#define BCM2712_PULL_UP		2

// function select values, by stepping
enum bcm2712_pin_function {
	BCM2712_FUNCTION_UART0,		// the Bluetooth UART, GPIO 24 to 27
	BCM2712_FUNCTION_SD2		// the Wi-Fi SDIO, GPIO 30 to 35
};


struct bcm2712_pin_regs {
	uint8	muxRegister, muxIndex, padRegister, padIndex;
};

static const bcm2712_pin_regs kBCM2712C0Pins[12] = {
	{3, 0, 9, 1}, {3, 1, 9, 2}, {3, 2, 9, 3}, {3, 3, 9, 4}, {3, 4, 9, 5},
	{3, 5, 9, 6}, {3, 6, 9, 7}, {3, 7, 9, 8}, {4, 0, 9, 9}, {4, 1, 9, 10},
	{4, 2, 9, 11}, {4, 3, 9, 12}
};
static const bcm2712_pin_regs kBCM2712D0Pins[12] = {
	{2, 0, 5, 6}, {2, 1, 5, 7}, {2, 2, 5, 8}, {2, 3, 5, 9}, {2, 4, 5, 10},
	{2, 5, 5, 11}, {2, 6, 5, 12}, {2, 7, 5, 13}, {3, 0, 5, 14}, {3, 1, 6, 0},
	{3, 2, 6, 1}, {3, 3, 6, 2}
};


/*!	The function select value of \a function on \a pin, or -1. */
static inline int
bcm2712_function_select(bool d0, uint32 pin, bcm2712_pin_function function)
{
	if (function == BCM2712_FUNCTION_UART0 && pin >= 24 && pin <= 27) {
		if (d0)
			return 4;
		return pin == 24 ? 3 : 4;
	}
	if (function == BCM2712_FUNCTION_SD2 && pin >= 30 && pin <= 35) {
		if (d0)
			return 1;
		return pin == 33 || pin == 35 ? 3 : 4;
	}
	return -1;
}


/*!	Gives \a pin (24 to 35) \a function and \a pull. \a pinctrl: the
	multiplexer's registers, mapped. */
static inline status_t
bcm2712_set_pin(volatile uint8* pinctrl, bool d0, uint32 pin,
	bcm2712_pin_function function, uint32 pull)
{
	int select = bcm2712_function_select(d0, pin, function);
	if (select < 0)
		return B_BAD_VALUE;
	const bcm2712_pin_regs& regs = (d0 ? kBCM2712D0Pins : kBCM2712C0Pins)
		[pin - 24];

	volatile uint32* mux = (volatile uint32*)(pinctrl + regs.muxRegister * 4);
	uint32 shift = regs.muxIndex * 4;
	*mux = (*mux & ~(0xfu << shift)) | ((uint32)select << shift);

	volatile uint32* pad = (volatile uint32*)(pinctrl + regs.padRegister * 4);
	shift = regs.padIndex * 2;
	*pad = (*pad & ~(0x3u << shift)) | ((pull & 0x3) << shift);
	return B_OK;
}


/*!	Drives \a pin of a brcmstb GPIO block (32 per bank of eight registers:
	ODEN, DATA, IODIR, ...) as an output. The pin has to be a GPIO in the
	multiplexer. */
static inline void
bcm2712_gpio_output(volatile uint8* gio, uint32 pin, bool high)
{
	volatile uint32* bank = (volatile uint32*)(gio + (pin / 32) * 0x20);
	uint32 bit = 1u << (pin % 32);
	if (high)
		bank[1] |= bit;
	else
		bank[1] &= ~bit;
	bank[2] &= ~bit;	// IODIR: 0 is an output
}


/*!	Finds the device tree node whose first "compatible" is \a compatible
	(and, if \a address is not 0, whose first register range starts there)
	and returns its register ranges \a index and its first interrupt.
	Returns false if there is none. */
static inline bool
bcm2712_find_node(const char* compatible, uint64 address, uint32 regIndex,
	uint64* _registers, uint64* _size, uint64* _interrupt = NULL,
	bool (*accept)(fdt_device_module_info*, fdt_device*) = NULL)
{
	device_manager_info* manager;
	if (get_module(B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&manager)
			!= B_OK) {
		return false;
	}

	device_attr attributes[] = {
		{ "fdt/compatible", B_STRING_TYPE, { .string = compatible } },
		{}
	};
	bool found = false;
	device_node* root = manager->get_root_node();
	device_node* node = NULL;
	// recursive, continuing after the last node found (and releasing it)
	while (!found
		&& manager->find_child_node(root, attributes, &node) == B_OK) {
		fdt_device_module_info* module;
		fdt_device* device;
		uint64 base, size;
		if (manager->get_driver(node, (driver_module_info**)&module,
				(void**)&device) != B_OK
			|| !module->get_reg(device, 0, &base, &size)
			|| (address != 0 && base != address)
			|| (accept != NULL && !accept(module, device))
			|| !module->get_reg(device, regIndex, _registers, _size)) {
			continue;
		}
		if (_interrupt != NULL
			&& !module->get_interrupt(device, 0, NULL, _interrupt)) {
			*_interrupt = 0;
		}
		found = true;
	}
	if (found)
		manager->put_node(node);
	manager->put_node(root);
	put_module(B_DEVICE_MANAGER_MODULE_NAME);
	return found;
}


/*!	The pin multiplexer: \a _d0 tells the stepping. */
static inline bool
bcm2712_find_pinctrl(uint64* _registers, bool* _d0)
{
	uint64 size;
	if (bcm2712_find_node("brcm,bcm2712d0-pinctrl", 0, 0, _registers, &size)) {
		*_d0 = true;
		return true;
	}
	*_d0 = false;
	return bcm2712_find_node("brcm,bcm2712c0-pinctrl", 0, 0, _registers, &size)
		|| bcm2712_find_node("brcm,bcm2712-pinctrl", 0, 0, _registers, &size);
}


// The "gio" GPIO block with the Wi-Fi's and Bluetooth's enable lines
// (WL_ON is GPIO 28, BT_ON 29)
#define BCM2712_GIO_BASE		0x107d508500ull
#define BCM2712_GPIO_WL_ON		28
#define BCM2712_GPIO_BT_ON		29


#endif	/* _BCM2712_PINS_H */
