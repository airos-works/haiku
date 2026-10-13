/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef I2C_HID_GPIO_INTERRUPT_H
#define I2C_HID_GPIO_INTERRUPT_H


#include <ACPI.h>
#include <OS.h>


struct GpioController;


/*!	The interrupt line of an I2C HID device that ACPI describes as a GpioInt
	resource, on a GPIO controller this driver knows how to program (the AMD
	FCH controller, AMDI0030). HID over I2C interrupts are level triggered:
	Wait() unmasks the pin and returns once the controller saw it asserted,
	with the pin masked again until the caller has read the report that
	deasserts it and waits again.
*/
class GpioInterrupt {
public:
	static	status_t			Create(acpi_module_info* acpi,
									acpi_handle device,
									GpioInterrupt** _interrupt);
								~GpioInterrupt();

			bool				Asserted() const;
			status_t			Wait(bigtime_t timeout);
			void				Mask();

			uint32				Pin() const { return fPin; }
			const char*			ControllerName() const;

private:
								GpioInterrupt(GpioController* controller,
									uint32 pin, bool levelTriggered,
									bool activeLow);

			friend struct GpioController;

			GpioController*		fController;
			GpioInterrupt*		fNext;
			uint32				fPin;
			bool				fLevelTriggered;
			bool				fActiveLow;
			sem_id				fSemaphore;
};


#endif	// I2C_HID_GPIO_INTERRUPT_H
