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
	resource, on a GPIO controller this driver knows how to program: the AMD
	FCH controller (AMDI0030) or an Intel one (Apollo Lake, Gemini Lake,
	Jasper Lake, Skylake to Meteor Lake). HID over I2C interrupts are level
	triggered: Wait() unmasks the pin and returns once the controller saw it
	asserted, with the pin masked again until the caller has read the report
	that deasserts it and waits again.

	A pin whose level can be read but whose interrupt cannot be used (an Intel
	pad the firmware left in ACPI mode, or locked in another trigger mode) is
	still returned: CanInterrupt() is false, and the caller watches Asserted()
	instead.
*/
class GpioInterrupt {
public:
	static	status_t			Create(acpi_module_info* acpi,
									acpi_handle device,
									GpioInterrupt** _interrupt);
								~GpioInterrupt();

			bool				Asserted() const;
			bool				CanInterrupt() const { return fCanInterrupt; }
			status_t			Wait(bigtime_t timeout);
			void				Mask();

			uint32				Pin() const { return fPin; }
			const char*			ControllerName() const;

private:
								GpioInterrupt(GpioController* controller,
									uint32 pin, bool levelTriggered,
									bool activeLow);

			friend struct GpioController;
			friend struct AmdGpioController;
			friend struct IntelGpioController;

			GpioController*		fController;
			GpioInterrupt*		fNext;
			uint32				fPin;
			bool				fLevelTriggered;
			bool				fActiveLow;
			bool				fCanInterrupt;
			bool				fRegistered;
				// on its controller's list: until then deleting it only
				// frees it (Create() holds the controllers' lock)
			sem_id				fSemaphore;

			// where the controller keeps the pin (Intel)
			uint8				fCommunity;
			uint8				fRegister;
			uint8				fBit;
			uint32				fPadOffset;
};


#endif	// I2C_HID_GPIO_INTERRUPT_H
