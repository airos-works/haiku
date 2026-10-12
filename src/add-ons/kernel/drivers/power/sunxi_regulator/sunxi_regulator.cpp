/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Fixed regulators that the board switches with an Allwinner A733 pin and
	that must always be on ("regulator-fixed" with "regulator-always-on" or
	"regulator-boot-on" and a "gpio" on the PIO or R_PIO): Linux turns them
	on when it boots, the firmware does not. On the Cubie A7S they power the
	AIC8800 Wi-Fi/Bluetooth module (PM0 and PM1), which only then shows up
	on USB.

	The pin is made an output at the regulator's enable level. Nothing turns
	them off again.

	The add-on also carries the PMIC module (sunxi_pmic.cpp, sunxi_pmic.h):
	the converters' voltages for the drivers that scale them. */


#include <string.h>

#include <ByteOrder.h>
#include <KernelExport.h>
#include <device_manager.h>
#include <bus/FDT.h>
#include <sunxi_pmic.h>


#define SUNXI_REGULATOR_MODULE_NAME "drivers/power/sunxi_regulator/driver_v1"

#define TRACE(x...) dprintf("sunxi_regulator: " x)


static device_manager_info* sDeviceManager;


struct pin_controller {
	const char*	compatible;
	uint32		bankSize;
	uint32		firstBankOffset;
	char		firstBank;
};

// The PIO's bank registers start a bank further in than bank A; the R_PIO's
// first bank is L (pinctrl-sunxi's A733 descriptions and U-Boot).
static const pin_controller kPinControllers[] = {
	{ "allwinner,sun60i-a733-pinctrl", 0x80, 0x80, 'A' },
	{ "allwinner,sun60i-a733-r-pinctrl", 0x30, 0x0, 'L' },
};

#define PIN_DATA	0x10
#define PIN_OUTPUT	1


class FdtNode {
public:
	bool SetTo(device_node* node)
	{
		driver_module_info* module;
		void* cookie;
		if (node == NULL
			|| sDeviceManager->get_driver(node, &module, &cookie) != B_OK
			|| strcmp(module->info.name, "bus_managers/fdt/driver_v1") != 0) {
			return false;
		}
		fModule = (fdt_device_module_info*)module;
		fDevice = (fdt_device*)cookie;
		return true;
	}

	bool HasString(const char* property, const char* wanted) const
	{
		int length = 0;
		const char* data = (const char*)fModule->get_prop(fDevice, property,
			&length);
		while (data != NULL && length > 0) {
			const char* end = (const char*)memchr(data, 0, length);
			if (end == NULL)
				return false;
			if (strcmp(data, wanted) == 0)
				return true;
			length -= end - data + 1;
			data = end + 1;
		}
		return false;
	}

	bool Has(const char* property) const
	{
		return fModule->get_prop(fDevice, property, NULL) != NULL;
	}

	bool Enabled() const
	{
		const char* status = (const char*)fModule->get_prop(fDevice, "status",
			NULL);
		return status == NULL || strcmp(status, "okay") == 0
			|| strcmp(status, "ok") == 0;
	}

	const uint32* Cells(const char* property, int* _count) const
	{
		int length = 0;
		const uint32* data = (const uint32*)fModule->get_prop(fDevice,
			property, &length);
		*_count = data != NULL ? length / 4 : 0;
		return data;
	}

	const char* Name() const
	{
		return fModule->get_name(fDevice);
	}

	fdt_device_module_info* fModule = NULL;
	fdt_device* fDevice = NULL;
};


struct regulator_pin {
	phys_addr_t	bankBase;
	char		bank;
	uint32		pin;
	bool		activeHigh;
};


static bool
find_pin(device_node* node, regulator_pin& pin)
{
	FdtNode regulator;
	if (!regulator.SetTo(node) || !regulator.Enabled()
		|| !regulator.HasString("compatible", "regulator-fixed")
		|| (!regulator.Has("regulator-always-on")
			&& !regulator.Has("regulator-boot-on"))) {
		return false;
	}

	int count;
	const uint32* gpio = regulator.Cells("gpio", &count);
	if (gpio == NULL)
		gpio = regulator.Cells("gpios", &count);
	if (gpio == NULL || count != 4)
		return false;

	fdt_bus_module_info* busModule;
	fdt_bus* bus;
	if (sDeviceManager->get_driver(regulator.fModule->get_bus(
			regulator.fDevice), (driver_module_info**)&busModule,
			(void**)&bus) != B_OK) {
		return false;
	}
	FdtNode controller;
	if (!controller.SetTo(busModule->node_by_phandle(bus,
			B_BENDIAN_TO_HOST_INT32(gpio[0])))
		|| !controller.Enabled()) {
		return false;
	}

	for (size_t i = 0; i < B_COUNT_OF(kPinControllers); i++) {
		if (!controller.HasString("compatible",
				kPinControllers[i].compatible)) {
			continue;
		}
		uint64 base, size;
		if (!controller.fModule->get_reg(controller.fDevice, 0, &base, &size))
			return false;
		uint32 bank = B_BENDIAN_TO_HOST_INT32(gpio[1]);
		pin.bankBase = base + kPinControllers[i].firstBankOffset
			+ bank * kPinControllers[i].bankSize;
		pin.bank = kPinControllers[i].firstBank + bank;
		pin.pin = B_BENDIAN_TO_HOST_INT32(gpio[2]);
		// gpio flags: bit 0 is GPIO_ACTIVE_LOW; regulator-fixed takes its
		// level from "enable-active-high" alone
		pin.activeHigh = regulator.Has("enable-active-high");
		return pin.pin < 32;
	}
	return false;
}


static status_t
set_pin(const regulator_pin& pin)
{
	void* address;
	area_id area = map_physical_memory("sunxi regulator pin",
		pin.bankBase & ~(B_PAGE_SIZE - 1), B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address);
	if (area < 0)
		return area;
	volatile uint8* bank = (volatile uint8*)address
		+ (pin.bankBase & (B_PAGE_SIZE - 1));

	// the level first, so that the pin does not glitch when it turns output
	volatile uint32* data = (volatile uint32*)(bank + PIN_DATA);
	if (pin.activeHigh)
		*data |= 1u << pin.pin;
	else
		*data &= ~(1u << pin.pin);

	volatile uint32* config = (volatile uint32*)(bank + (pin.pin / 8) * 4);
	uint32 shift = (pin.pin % 8) * 4;
	*config = (*config & ~(0xfu << shift)) | (PIN_OUTPUT << shift);

	delete_area(area);
	return B_OK;
}


//	#pragma mark - driver


static float
sunxi_regulator_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "fdt") != 0) {
		return 0.0f;
	}
	regulator_pin pin;
	return find_pin(parent, pin) ? 0.8f : 0.0f;
}


static status_t
sunxi_regulator_register_device(device_node* parent)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "Allwinner pin switched regulator" } },
		{}
	};
	return sDeviceManager->register_node(parent, SUNXI_REGULATOR_MODULE_NAME,
		attrs, NULL, NULL);
}


static status_t
sunxi_regulator_init_driver(device_node* node, void** _cookie)
{
	device_node* parent = sDeviceManager->get_parent_node(node);
	regulator_pin pin;
	bool found = find_pin(parent, pin);
	FdtNode regulator;
	const char* name = regulator.SetTo(parent) ? regulator.Name() : "?";
	sDeviceManager->put_node(parent);
	if (!found)
		return B_NOT_SUPPORTED;

	status_t status = set_pin(pin);
	TRACE("%s: P%c%" B_PRIu32 " %s: %s\n", name, pin.bank, pin.pin,
		pin.activeHigh ? "high" : "low", strerror(status));
	*_cookie = NULL;
	return status;
}


static void
sunxi_regulator_uninit_driver(void* cookie)
{
}


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{}
};


static driver_module_info sSunxiRegulatorDriver = {
	{
		SUNXI_REGULATOR_MODULE_NAME,
		0,
		NULL
	},
	sunxi_regulator_supports_device,
	sunxi_regulator_register_device,
	sunxi_regulator_init_driver,
	sunxi_regulator_uninit_driver,
	NULL,
	NULL,
	NULL
};


extern sunxi_pmic_module_info gSunxiPmicModule;


module_info* modules[] = {
	(module_info*)&sSunxiRegulatorDriver,
	(module_info*)&gSunxiPmicModule,
	NULL
};
