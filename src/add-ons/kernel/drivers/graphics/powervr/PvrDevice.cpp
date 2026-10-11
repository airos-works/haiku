/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "PvrDevice.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <driver_settings.h>
#include <KernelExport.h>

#include <arch/int.h>
#include <util/AutoLock.h>

#include "A733Power.h"


#define TRACE(x...)	dprintf("powervr: " x)

// Rogue control registers (pvr_rogue_cr_defs.h)
#define ROGUE_CR_CORE_ID			0x0018

// A level-triggered line that stays up without a MIPS wrapper event would
// take the CPU forever: after this many in a row the line is switched off.
static const int32 kInterruptStormLimit = 100000;

static const uint32 kDefaultTraceLines = 64;


namespace powervr {


PvrDevice::PvrDevice(uint64 registerBase, uint64 registerSize,
	int32 interrupt)
	:
	fRegisterBase(registerBase),
	fRegisterSize(registerSize),
	fInterrupt(interrupt),
	fRegisterArea(-1),
	fRegisters(NULL),
	fStage(PVR_HAIKU_STAGE_OFF),
	fBvnc(0),
	fCoreId(0),
	fCoreClock(0),
	fDevice(NULL),
	fDisabled(false),
	fFirmwareEnabled(false),
	fInterruptInstalled(false),
	fInterruptSemaphore(-1),
	fInterruptThread(-1),
	fQuit(false),
	fUnhandledInARow(0)
{
	mutex_init(&fLock, "powervr device");
	memset(&fOptions, 0, sizeof(fOptions));
}


PvrDevice::~PvrDevice()
{
	if (fDevice != NULL) {
		// the interrupt thread must be gone before the firmware's queues
		_RemoveInterruptHandler();
		pvr_haiku_firmware_shutdown(fDevice);
		pvr_haiku_device_delete(fDevice);
	}
	if (fRegisterArea >= 0)
		delete_area(fRegisterArea);
	mutex_destroy(&fLock);
}


/*!	The driver settings file "powervr":
		disable true				leave the GPU off
		firmware true				boot the firmware (off by default)
		fw_trace_mask 0x2			firmware log groups (ROGUE_FWIF_LOG_TYPE_
									GROUP_*), 0 for none
		fw_trace_lines 64			firmware trace lines logged at most
		trace_registers true		log the register accesses of the start
									(off by default)
		vendor_secure_config true	Allwinner's SMC/GPU_GLB writes before the
									start (see DESIGN R7)
*/
void
PvrDevice::_ReadSettings()
{
	fOptions.trace_registers = false;
	fOptions.trace_lines = kDefaultTraceLines;

	void* handle = load_driver_settings("powervr");
	if (handle == NULL) {
		TRACE("settings: no driver settings, firmware stage off\n");
		return;
	}

	fDisabled = get_driver_boolean_parameter(handle, "disable", false, true);
	fFirmwareEnabled = get_driver_boolean_parameter(handle, "firmware", false,
		true);
	fOptions.trace_registers = get_driver_boolean_parameter(handle,
		"trace_registers", false, true);
	fOptions.vendor_secure_config = get_driver_boolean_parameter(handle,
		"vendor_secure_config", false, true);
	const char* value = get_driver_parameter(handle, "fw_trace_mask", NULL,
		NULL);
	if (value != NULL)
		fOptions.trace_mask = strtoul(value, NULL, 0);
	value = get_driver_parameter(handle, "fw_trace_lines", NULL, NULL);
	if (value != NULL)
		fOptions.trace_lines = strtoul(value, NULL, 0);
	unload_driver_settings(handle);

	TRACE("settings:%s firmware stage %s, firmware trace groups %#" B_PRIx32
		" (%" B_PRIu32 " lines), register trace %s, vendor secure config"
		" %s\n", fDisabled ? " GPU disabled," : "",
		fFirmwareEnabled ? "on" : "off", fOptions.trace_mask,
		fOptions.trace_lines, fOptions.trace_registers ? "on" : "off",
		fOptions.vendor_secure_config ? "on" : "off");
}


status_t
PvrDevice::Init()
{
	_ReadSettings();
	if (fDisabled) {
		TRACE("disabled by the driver settings\n");
		return B_OK;
	}

	status_t status = a733_gpu_power_on(&fCoreClock);
	if (status != B_OK) {
		TRACE("the GPU does not power up: %s\n", strerror(status));
		return status;
	}
	fStage = PVR_HAIKU_STAGE_POWERED;

	fRegisterArea = map_physical_memory("powervr registers", fRegisterBase,
		fRegisterSize, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&fRegisters);
	if (fRegisterArea < 0)
		return fRegisterArea;

	pvr_haiku_platform platform = {};
	platform.registers = fRegisters;
	platform.register_base = fRegisterBase;
	platform.register_size = fRegisterSize;
	platform.core_clock = fCoreClock;
	fDevice = pvr_haiku_device_create(&platform);
	if (fDevice == NULL)
		return B_NO_MEMORY;

	fBvnc = pvr_haiku_identify(fDevice);
	fCoreId = *(volatile uint32*)(fRegisters + ROGUE_CR_CORE_ID);
	TRACE("BVNC %u.%u.%u.%u (%#" B_PRIx64 "), core ID %#" B_PRIx32
		", %" B_PRIu32 " MHz\n", (unsigned)(fBvnc >> 48),
		(unsigned)((fBvnc >> 32) & 0xffff), (unsigned)((fBvnc >> 16) & 0xffff),
		(unsigned)(fBvnc & 0xffff), fBvnc, fCoreId, fCoreClock / 1000000);
	if (fBvnc == 0)
		return B_OK;
	fStage = PVR_HAIKU_STAGE_IDENTIFIED;

	if (!fFirmwareEnabled) {
		TRACE("firmware stage off (\"firmware true\" in the driver settings"
			" turns it on)\n");
		return B_OK;
	}
	_StartFirmware();
	return B_OK;
}


/*!	Bring-up stage 2: the firmware loaded, booted on the MIPS and checked.
	A failure is logged and reported by the STAGE query; the driver stays.
*/
void
PvrDevice::_StartFirmware()
{
	bigtime_t start = system_time();
	TRACE("firmware stage: start\n");

	status_t status = pvr_haiku_firmware_load(fDevice);
	if (status == B_OK)
		status = _InstallInterruptHandler();
	if (status == B_OK)
		status = pvr_haiku_firmware_boot(fDevice, &fOptions);
	if (status == B_OK)
		status = pvr_haiku_firmware_verify(fDevice, &fOptions);

	pvr_haiku_firmware_state state;
	pvr_haiku_firmware_state_get(fDevice, &state);
	if (!state.running) {
		// nothing will interrupt: the GPU is off or held in reset
		_RemoveInterruptHandler();
	}
	if (status == B_OK)
		fStage = PVR_HAIKU_STAGE_FIRMWARE;

	TRACE("firmware stage %s (%s) after %" B_PRId64 " ms: FW %" B_PRIu32
		".%" B_PRIu32 " build %" B_PRIu32 ", %s, boot %" B_PRId64 " us, %"
		B_PRIu32 "/%" B_PRIu32 " health checks, last KCCB return %#" B_PRIx32
		", kccb_cmds_executed %" B_PRIu32 ", %" B_PRIu32 " interrupts (%"
		B_PRIu32 " spurious), MIPS exception status %#" B_PRIx32 ", %" B_PRIu32
		" firmware faults\n", status == B_OK ? "passed" : "FAILED",
		strerror(status), (system_time() - start) / 1000, state.version_major,
		state.version_minor, state.version_build,
		state.running ? "running" : "not running", state.boot_time,
		state.health_checks, state.health_checks + state.health_check_failures,
		state.last_kccb_return, state.kccb_cmds_executed, state.irq_count,
		state.irq_spurious, state.mips_exception_status, state.fw_faults);
}


//	#pragma mark - interrupts


/*!	Stale interrupt state cleared first (Allwinner's cleanInterrupt()),
	then a thread for the work and the handler on the GIC line.
*/
status_t
PvrDevice::_InstallInterruptHandler()
{
	if (fInterruptInstalled)
		return B_OK;

	pvr_haiku_clear_stale_interrupts(fDevice);

	fQuit = false;
	fUnhandledInARow = 0;
	fInterruptSemaphore = create_sem(0, "powervr interrupts");
	if (fInterruptSemaphore < 0)
		return fInterruptSemaphore;
	fInterruptThread = spawn_kernel_thread(_InterruptThread,
		"powervr interrupts", B_URGENT_DISPLAY_PRIORITY, this);
	if (fInterruptThread < 0) {
		delete_sem(fInterruptSemaphore);
		fInterruptSemaphore = -1;
		return fInterruptThread;
	}
	resume_thread(fInterruptThread);

	status_t status = install_io_interrupt_handler(fInterrupt,
		_InterruptHandler, this, 0);
	TRACE("interrupt handler on vector %" B_PRId32 ": %s\n", fInterrupt,
		strerror(status));
	if (status != B_OK) {
		_RemoveInterruptHandler();
		return status;
	}
	fInterruptInstalled = true;
	return B_OK;
}


void
PvrDevice::_RemoveInterruptHandler()
{
	if (fInterruptInstalled) {
		remove_io_interrupt_handler(fInterrupt, _InterruptHandler, this);
		fInterruptInstalled = false;
	}
	if (fInterruptThread >= 0) {
		fQuit = true;
		delete_sem(fInterruptSemaphore);
		status_t result;
		wait_for_thread(fInterruptThread, &result);
		fInterruptThread = -1;
		fInterruptSemaphore = -1;
	}
}


/*!	The hard handler: the MIPS wrapper event is cleared here (the line is
	level-triggered and not masked while the thread runs), the rest is the
	thread's.
*/
int32
PvrDevice::_InterruptHandler(void* data)
{
	PvrDevice* device = (PvrDevice*)data;
	if (!pvr_haiku_interrupt(device->fDevice)) {
		if (++device->fUnhandledInARow == kInterruptStormLimit) {
			arch_int_disable_io_interrupt(device->fInterrupt);
			dprintf("powervr: %" B_PRId32 " interrupts in a row without a"
				" MIPS wrapper event: vector %" B_PRId32 " switched off\n",
				kInterruptStormLimit, device->fInterrupt);
		}
		return B_UNHANDLED_INTERRUPT;
	}

	device->fUnhandledInARow = 0;
	release_sem_etc(device->fInterruptSemaphore, 1, B_DO_NOT_RESCHEDULE);
	return B_INVOKE_SCHEDULER;
}


status_t
PvrDevice::_InterruptThread(void* data)
{
	PvrDevice* device = (PvrDevice*)data;
	while (acquire_sem(device->fInterruptSemaphore) == B_OK) {
		if (device->fQuit)
			break;
		pvr_haiku_interrupt_work(device->fDevice);
	}
	return B_OK;
}


//	#pragma mark - STAGE


/*!	The device for the DRM files of new opens, while the firmware runs and
	the GPU is not lost, otherwise NULL (only STAGE then, and Mesa finds no
	GPU).
*/
struct pvr_device*
PvrDevice::Device() const
{
	if (fStage != PVR_HAIKU_STAGE_FIRMWARE || pvr_haiku_device_lost(fDevice))
		return NULL;
	return fDevice;
}


status_t
PvrDevice::Stage(pvr_haiku_stage& stage)
{
	MutexLocker locker(fLock);

	if (stage.command != PVR_HAIKU_STAGE_QUERY) {
		if (geteuid() != 0)
			return B_NOT_ALLOWED;
		if (fDevice == NULL)
			return B_NO_INIT;
		if (stage.command == PVR_HAIKU_STAGE_HEALTH_CHECK)
			pvr_haiku_health_check(fDevice);
		else if (stage.command == PVR_HAIKU_STAGE_DUMP)
			pvr_haiku_dump(fDevice, "asked for", fOptions.trace_lines);
		else if (stage.command == PVR_HAIKU_STAGE_RESET) {
			if (fStage != PVR_HAIKU_STAGE_FIRMWARE)
				return B_NO_INIT;
			pvr_haiku_reset(fDevice);
		} else if (stage.command == PVR_HAIKU_STAGE_JOB_TIMEOUT)
			pvr_haiku_set_job_timeout(stage.job_timeout_ms);
		else
			return B_BAD_VALUE;
	}

	uint32 command = stage.command;
	memset(&stage, 0, sizeof(stage));
	stage.version = PVR_HAIKU_ABI_VERSION;
	stage.command = command;
	stage.stage = fStage;
	stage.core_id = fCoreId;
	stage.bvnc = fBvnc;
	stage.core_clock = fCoreClock;
	stage.firmware_status = B_NO_INIT;
	stage.job_timeout_ms = pvr_haiku_job_timeout();
	if (fDevice == NULL)
		return B_OK;

	pvr_haiku_firmware_state state;
	pvr_haiku_firmware_state_get(fDevice, &state);
	stage.firmware_status = state.status;
	stage.firmware_running = state.running;
	stage.fw_version_major = state.version_major;
	stage.fw_version_minor = state.version_minor;
	stage.fw_version_build = state.version_build;
	stage.fw_boot_time = state.boot_time;
	stage.health_checks = state.health_checks;
	stage.health_check_failures = state.health_check_failures;
	stage.last_kccb_return = state.last_kccb_return;
	stage.kccb_cmds_executed = state.kccb_cmds_executed;
	stage.irq_count = state.irq_count;
	stage.irq_spurious = state.irq_spurious;
	stage.mips_exception_status = state.mips_exception_status;
	stage.fw_faults = state.fw_faults;
	stage.resets = state.resets;
	stage.device_lost = state.lost ? 1 : 0;
	return B_OK;
}


}	// namespace powervr
