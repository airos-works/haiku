/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


//!	GpioInt interrupts of I2C HID devices on the AMD FCH GPIO controller.


#include "GpioInterrupt.h"

#include <new>
#include <stdlib.h>
#include <string.h>

#include <KernelExport.h>
#include <arch/int.h>
#include <lock.h>
#include <util/AutoLock.h>

extern "C" {
#	include "acpi.h"
}

#include "Driver.h"


// Pin registers of the AMD FCH GPIO controller (Linux: pinctrl-amd.h). One
// 32-bit register per pin; three registers in the pin range are not pins.
enum {
	kLevelTrigger		= 1 << 8,
	kActiveLevelShift	= 9,
	kActiveLevelMask	= 3 << kActiveLevelShift,
	kActiveLow			= 1 << kActiveLevelShift,
	kInterruptEnable	= 1 << 11,
	kInterruptUnmasked	= 1 << 12,
		// pinctrl-amd calls this INTERRUPT_MASK; set means delivered
	kPinState			= 1 << 16,
	kInterruptStatus	= 1 << 28,
	kWakeStatus			= 1 << 29,
	kPending			= kInterruptStatus | kWakeStatus,
};

static const uint32 kMasterRegister = 0xfc;
static const uint32 kMasterEndOfInterrupt = 1 << 29;
static const uint32 kStatusRegister0 = 0x2f8;
static const uint32 kStatusRegister1 = 0x2fc;
static const uint32 kStatusBits = 46;
	// each status bit stands for four pins
static const uint32 kPinCount = kStatusBits * 4;


struct GpioController {
	GpioController*		next;
	char				path[64];
	area_id				area;
	volatile uint32*	registers;
	size_t				size;
	int32				irq;
	spinlock			lock;
	GpioInterrupt*		interrupts;
	uint32				spurious;

			bool		IsPin(uint32 pin) const
							{ return pin < kPinCount
								&& pin * 4 != kMasterRegister
								&& (pin + 1) * 4 <= size; }
			uint32		Read(uint32 offset) const
							{ return registers[offset / 4]; }
			void		Write(uint32 offset, uint32 value)
							{ registers[offset / 4] = value; }

			void		Configure(GpioInterrupt* interrupt);
			void		SetUnmasked(uint32 pin, bool unmasked);
	static	int32		HandleInterrupt(void* data);
};


static mutex sControllerLock = MUTEX_INITIALIZER("i2c_hid gpio");
static GpioController* sControllers;


static const char* const kControllerIDs[] = {
	"AMDI0030",
	"AMDI0031",
	"AMD0030",
	NULL
};


struct gpio_int_resource {
	bool		found;
	uint32		pin;
	bool		levelTriggered;
	bool		activeLow;
	char		source[64];
};


struct controller_resources {
	uint64		base;
	uint32		length;
	int32		irq;
	bool		levelTriggered;
	bool		activeLow;
};


static acpi_status
parse_gpio_int(ACPI_RESOURCE* resource, void* context)
{
	gpio_int_resource* gpio = (gpio_int_resource*)context;
	if (gpio->found || resource->Type != ACPI_RESOURCE_TYPE_GPIO)
		return AE_OK;

	ACPI_RESOURCE_GPIO& data = resource->Data.Gpio;
	if (data.ConnectionType != ACPI_RESOURCE_GPIO_TYPE_INT
		|| data.PinTableLength == 0 || data.ResourceSource.StringPtr == NULL)
		return AE_OK;

	gpio->found = true;
	gpio->pin = data.PinTable[0];
	gpio->levelTriggered = data.Triggering == ACPI_LEVEL_SENSITIVE;
	gpio->activeLow = data.Polarity == ACPI_ACTIVE_LOW;
	strlcpy(gpio->source, data.ResourceSource.StringPtr,
		sizeof(gpio->source));
	return AE_OK;
}


static acpi_status
parse_controller(ACPI_RESOURCE* resource, void* context)
{
	controller_resources* controller = (controller_resources*)context;
	switch (resource->Type) {
		case ACPI_RESOURCE_TYPE_FIXED_MEMORY32:
			controller->base = resource->Data.FixedMemory32.Address;
			controller->length = resource->Data.FixedMemory32.AddressLength;
			break;
		case ACPI_RESOURCE_TYPE_MEMORY32:
			controller->base = resource->Data.Memory32.Minimum;
			controller->length = resource->Data.Memory32.AddressLength;
			break;
		case ACPI_RESOURCE_TYPE_EXTENDED_IRQ:
			if (resource->Data.ExtendedIrq.InterruptCount > 0) {
				controller->irq = resource->Data.ExtendedIrq.Interrupts[0];
				controller->levelTriggered
					= resource->Data.ExtendedIrq.Triggering
						== ACPI_LEVEL_SENSITIVE;
				controller->activeLow
					= resource->Data.ExtendedIrq.Polarity == ACPI_ACTIVE_LOW;
			}
			break;
		case ACPI_RESOURCE_TYPE_IRQ:
			if (resource->Data.Irq.InterruptCount > 0) {
				controller->irq = resource->Data.Irq.Interrupts[0];
				controller->levelTriggered = resource->Data.Irq.Triggering
					== ACPI_LEVEL_SENSITIVE;
				controller->activeLow
					= resource->Data.Irq.Polarity == ACPI_ACTIVE_LOW;
			}
			break;
	}
	return AE_OK;
}


static bool
is_supported_controller(acpi_module_info* acpi, const char* path)
{
	char* hid = NULL;
	char* cidList[8] = { NULL };
	if (acpi->get_device_info(path, &hid, cidList, 8, NULL, NULL) != B_OK)
		return false;

	bool supported = false;
	for (int i = 0; kControllerIDs[i] != NULL; i++) {
		if (hid != NULL && strcmp(hid, kControllerIDs[i]) == 0)
			supported = true;
		for (int j = 0; cidList[j] != NULL; j++) {
			if (strcmp(cidList[j], kControllerIDs[i]) == 0)
				supported = true;
		}
	}

	free(hid);
	for (int j = 0; cidList[j] != NULL; j++)
		free(cidList[j]);
	return supported;
}


//	#pragma mark - GpioController


void
GpioController::Configure(GpioInterrupt* interrupt)
{
	uint32 offset = interrupt->fPin * 4;
	InterruptsSpinLocker locker(lock);
	uint32 value = Read(offset);
	value &= ~(kLevelTrigger | kActiveLevelMask | kInterruptUnmasked | kPending);
	if (interrupt->fLevelTriggered)
		value |= kLevelTrigger;
	if (interrupt->fActiveLow)
		value |= kActiveLow;
	value |= kInterruptEnable;
	Write(offset, value);
}


void
GpioController::SetUnmasked(uint32 pin, bool unmasked)
{
	uint32 offset = pin * 4;
	InterruptsSpinLocker locker(lock);
	uint32 value = Read(offset) & ~kPending;
	if (unmasked)
		value |= kInterruptEnable | kInterruptUnmasked;
	else
		value &= ~kInterruptUnmasked;
	Write(offset, value);
}


/*static*/ int32
GpioController::HandleInterrupt(void* data)
{
	GpioController* controller = (GpioController*)data;

	uint64 status = controller->Read(kStatusRegister1);
	status = (status << 32) | controller->Read(kStatusRegister0);
	status &= (1ULL << kStatusBits) - 1;
	if (status == 0)
		return B_UNHANDLED_INTERRUPT;

	sem_id wake[8];
	uint32 wakeCount = 0;
	bool handled = false;

	SpinLocker locker(controller->lock);
	for (uint32 bit = 0; bit < kStatusBits; bit++) {
		if ((status & (1ULL << bit)) == 0)
			continue;

		for (uint32 pin = bit * 4; pin < bit * 4 + 4; pin++) {
			if (!controller->IsPin(pin))
				continue;

			uint32 value = controller->Read(pin * 4);
			if ((value & kPending) == 0)
				continue;

			GpioInterrupt* interrupt = controller->interrupts;
			while (interrupt != NULL && interrupt->fPin != pin)
				interrupt = interrupt->fNext;

			if (interrupt != NULL) {
				// Masked until its device has been read; the level
				// trigger would raise it again right away otherwise.
				if ((value & kInterruptUnmasked) != 0 && wakeCount < 8)
					wake[wakeCount++] = interrupt->fSemaphore;
			} else if ((value & kInterruptUnmasked) != 0) {
				// Nobody handles this pin: keep it from storming.
				controller->spurious++;
			}

			// writing the status bits back clears them
			controller->Write(pin * 4, value & ~kInterruptUnmasked);
			handled = true;
		}
	}

	controller->Write(kMasterRegister,
		controller->Read(kMasterRegister) | kMasterEndOfInterrupt);
	locker.Unlock();

	for (uint32 i = 0; i < wakeCount; i++)
		release_sem_etc(wake[i], 1, B_DO_NOT_RESCHEDULE);

	if (wakeCount > 0)
		return B_INVOKE_SCHEDULER;
	return handled ? B_HANDLED_INTERRUPT : B_UNHANDLED_INTERRUPT;
}


static status_t
get_controller(acpi_module_info* acpi, const char* path,
	GpioController** _controller)
{
	for (GpioController* controller = sControllers; controller != NULL;
			controller = controller->next) {
		if (strcmp(controller->path, path) == 0) {
			*_controller = controller;
			return B_OK;
		}
	}

	if (!is_supported_controller(acpi, path)) {
		TRACE_ALWAYS("GPIO controller %s is not supported\n", path);
		return B_NOT_SUPPORTED;
	}

	acpi_handle handle;
	if (acpi->get_handle(NULL, path, &handle) != B_OK)
		return B_ENTRY_NOT_FOUND;

	controller_resources resources = {};
	resources.irq = -1;
	status_t status = acpi->walk_resources(handle, (char*)"_CRS",
		(acpi_walk_resources_callback)parse_controller, &resources);
	if (status != B_OK || resources.base == 0 || resources.length < 0x300
		|| resources.irq < 0) {
		ERROR("GPIO controller %s: unusable resources (%#" B_PRIx64 ", %#"
			B_PRIx32 ", irq %" B_PRId32 ")\n", path, resources.base,
			resources.length, resources.irq);
		return B_BAD_DATA;
	}

	GpioController* controller = new(std::nothrow) GpioController;
	if (controller == NULL)
		return B_NO_MEMORY;
	memset(controller, 0, sizeof(GpioController));
	strlcpy(controller->path, path, sizeof(controller->path));
	B_INITIALIZE_SPINLOCK(&controller->lock);
	controller->size = resources.length;
	controller->irq = resources.irq;

	controller->area = map_physical_memory("i2c_hid gpio controller",
		resources.base, resources.length, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		(void**)&controller->registers);
	if (controller->area < 0) {
		status = controller->area;
		delete controller;
		return status;
	}

	// Firmware may have left interrupts unmasked on pins nobody here
	// handles; once the controller's interrupt is enabled they would
	// assert it forever. Linux masks them all when it takes the
	// controller over, too.
	uint32 masked = 0;
	for (uint32 pin = 0; pin < kPinCount; pin++) {
		if (!controller->IsPin(pin))
			continue;
		uint32 value = controller->Read(pin * 4);
		if ((value & kInterruptUnmasked) != 0) {
			controller->Write(pin * 4, value & ~(kInterruptUnmasked | kPending));
			masked++;
		}
	}

	if (resources.levelTriggered || resources.activeLow) {
		arch_int_configure_io_interrupt(resources.irq,
			resources.levelTriggered ? B_LEVEL_TRIGGERED : B_EDGE_TRIGGERED,
			resources.activeLow ? B_LOW_ACTIVE_POLARITY
				: B_HIGH_ACTIVE_POLARITY);
	}

	status = install_io_interrupt_handler(resources.irq,
		&GpioController::HandleInterrupt, controller, 0);
	if (status != B_OK) {
		ERROR("GPIO controller %s: cannot install interrupt %" B_PRId32 ": "
			"%s\n", path, resources.irq, strerror(status));
		delete_area(controller->area);
		delete controller;
		return status;
	}

	TRACE_ALWAYS("GPIO controller %s at %#" B_PRIx64 ", irq %" B_PRId32
		" (%s, %s), %" B_PRIu32 " firmware interrupt%s masked\n", path,
		resources.base, resources.irq,
		resources.levelTriggered ? "level" : "edge",
		resources.activeLow ? "low" : "high", masked, masked == 1 ? "" : "s");

	controller->next = sControllers;
	sControllers = controller;
	*_controller = controller;
	return B_OK;
}


static void
put_controller(GpioController* controller)
{
	if (controller->interrupts != NULL)
		return;

	GpioController** link = &sControllers;
	while (*link != NULL && *link != controller)
		link = &(*link)->next;
	if (*link != NULL)
		*link = controller->next;

	remove_io_interrupt_handler(controller->irq,
		&GpioController::HandleInterrupt, controller);
	delete_area(controller->area);
	delete controller;
}


//	#pragma mark - GpioInterrupt


GpioInterrupt::GpioInterrupt(GpioController* controller, uint32 pin,
	bool levelTriggered, bool activeLow)
	:
	fController(controller),
	fNext(NULL),
	fPin(pin),
	fLevelTriggered(levelTriggered),
	fActiveLow(activeLow),
	fSemaphore(-1)
{
}


GpioInterrupt::~GpioInterrupt()
{
	MutexLocker locker(sControllerLock);
	Mask();

	{
		InterruptsSpinLocker spinLocker(fController->lock);
		GpioInterrupt** link = &fController->interrupts;
		while (*link != NULL && *link != this)
			link = &(*link)->fNext;
		if (*link != NULL)
			*link = fNext;
	}

	put_controller(fController);
	if (fSemaphore >= 0)
		delete_sem(fSemaphore);
}


/*static*/ status_t
GpioInterrupt::Create(acpi_module_info* acpi, acpi_handle device,
	GpioInterrupt** _interrupt)
{
	gpio_int_resource gpio = {};
	status_t status = acpi->walk_resources(device, (char*)"_CRS",
		(acpi_walk_resources_callback)parse_gpio_int, &gpio);
	if (status != B_OK)
		return status;
	if (!gpio.found)
		return B_ENTRY_NOT_FOUND;

	MutexLocker locker(sControllerLock);

	GpioController* controller;
	status = get_controller(acpi, gpio.source, &controller);
	if (status != B_OK)
		return status;

	if (!controller->IsPin(gpio.pin)) {
		put_controller(controller);
		return B_BAD_VALUE;
	}

	for (GpioInterrupt* other = controller->interrupts; other != NULL;
			other = other->fNext) {
		if (other->fPin == gpio.pin)
			return B_BUSY;
	}

	GpioInterrupt* interrupt = new(std::nothrow) GpioInterrupt(controller,
		gpio.pin, gpio.levelTriggered, gpio.activeLow);
	if (interrupt == NULL) {
		put_controller(controller);
		return B_NO_MEMORY;
	}

	interrupt->fSemaphore = create_sem(0, "i2c_hid gpio interrupt");
	if (interrupt->fSemaphore < 0) {
		status = interrupt->fSemaphore;
		delete interrupt;
		put_controller(controller);
		return status;
	}

	controller->Configure(interrupt);

	{
		InterruptsSpinLocker spinLocker(controller->lock);
		interrupt->fNext = controller->interrupts;
		controller->interrupts = interrupt;
	}

	TRACE_ALWAYS("interrupt on %s pin %" B_PRIu32 " (%s, active %s)\n",
		gpio.source, gpio.pin, gpio.levelTriggered ? "level" : "edge",
		gpio.activeLow ? "low" : "high");

	*_interrupt = interrupt;
	return B_OK;
}


bool
GpioInterrupt::Asserted() const
{
	bool high = (fController->Read(fPin * 4) & kPinState) != 0;
	return fActiveLow ? !high : high;
}


status_t
GpioInterrupt::Wait(bigtime_t timeout)
{
	fController->SetUnmasked(fPin, true);
	return acquire_sem_etc(fSemaphore, 1, B_RELATIVE_TIMEOUT | B_CAN_INTERRUPT,
		timeout);
}


void
GpioInterrupt::Mask()
{
	fController->SetUnmasked(fPin, false);
}


const char*
GpioInterrupt::ControllerName() const
{
	return fController->path;
}
