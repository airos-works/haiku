/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef POWERVR_PVR_DEVICE_H
#define POWERVR_PVR_DEVICE_H


#include <OS.h>

#include <lock.h>

#include <pvr_haiku.h>

#include "pvr_haiku_glue.h"


namespace powervr {


/*	One PowerVR GPU: the bring-up stages behind the driver settings file
	"powervr" (power and identity, then the firmware), the interrupt
	handler and its thread, and the STAGE query. */
class PvrDevice {
public:
								PvrDevice(uint64 registerBase,
									uint64 registerSize, int32 interrupt);
								~PvrDevice();

			status_t			Init();

			status_t			Stage(pvr_haiku_stage& stage);

			struct pvr_device*	Device() const;

private:
			void				_ReadSettings();
			void				_StartFirmware();
			status_t			_InstallInterruptHandler();
			void				_RemoveInterruptHandler();

	static	int32				_InterruptHandler(void* data);
	static	status_t			_InterruptThread(void* data);

private:
			mutex				fLock;

			uint64				fRegisterBase;
			uint64				fRegisterSize;
			int32				fInterrupt;
			area_id				fRegisterArea;
			volatile uint8*		fRegisters;

			uint32				fStage;
			uint64				fBvnc;
			uint32				fCoreId;
			uint32				fCoreClock;
			uint32				fCoreMegahertz;
			struct pvr_device*	fDevice;

			bool				fDisabled;
			bool				fFirmwareEnabled;
			pvr_haiku_firmware_options fOptions;

			bool				fInterruptInstalled;
			sem_id				fInterruptSemaphore;
			thread_id			fInterruptThread;
			volatile bool		fQuit;
			int32				fUnhandledInARow;
};


}	// namespace powervr


#endif	// POWERVR_PVR_DEVICE_H
