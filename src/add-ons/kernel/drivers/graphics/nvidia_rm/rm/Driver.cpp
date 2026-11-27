#include "Driver.h"
#include "RmStack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <AutoDeleter.h>
#include <util/AutoLock.h>
#include <kdevice_manager.h>

#include "nv-include.h"
#include "nv-haiku.h"
#include "nv-haiku-kernel.h"

#include "ControlDevice.h"
#include "Device.h"


#define CHECK_RET(err) {status_t _err = (err); if (_err < B_OK) return _err;}


NvHaikuDriver NvHaikuDriver::sInstance;


static bool ParseUint32(const char *str, char *&strEnd, uint32 &res)
{
	// do not allow leading spaces or zeroes
	if (!(strcmp(str, "0") == 0 || (str[0] >= '1' && str[0] <= '9'))) {
		return false;
	}
	long val = strtol(str, &strEnd, 10);
	if (strEnd == str) {
		return false;
	}
	if ((val == LONG_MIN || val == LONG_MAX) && errno == ERANGE) {
		return false;
	}
	if (val < 0 || val > UINT32_MAX) {
		return false;
	}
	res = val;
	return true;
}

static bool ParseDeviceName(const char *name, uint32 &index)
{
	const char *deviceNamePrefix = NVIDIA_DEVICE_NAME;
	size_t deviceNamePrefixLen = strlen(deviceNamePrefix);
	if (strncmp(deviceNamePrefix, name, deviceNamePrefixLen) != 0) {
		return false;
	}

	char *strEnd {};
	if (!ParseUint32(name + deviceNamePrefixLen, strEnd, index)) {
		return false;
	}
	if (*strEnd != '\0') {
		return false;
	}
	return true;
}


NvHaikuDriver::~NvHaikuDriver()
{
	dprintf("-NvHaikuDriver\n");

	if (fPowerHookAdded)
		device_manager_remove_power_hook(PowerHook, this);
	if (fResumeThread >= 0) {
		status_t result;
		wait_for_thread(fResumeThread, &result);
	}

	for (int32 i = 0; i < fDevices.Count(); i++) {
		delete fDevices[i];
	}

	if (fControlDevice != nullptr) {
		delete fControlDevice;
	}

	if (fInitRmDone) {
		RmStack stack;
		rm_shutdown_rm(stack.Get());
	}

	if (fWorkQueueInitDone)
		nv_haiku_work_queue_uninit();

	fIntrSafePool.ReclaimAll();

	if (fPCI != nullptr) {
		put_module(B_PCI_MODULE_NAME);
	}
}

status_t NvHaikuDriver::Init()
{
	dprintf("+NvHaikuDriver\n");

	CHECK_RET(get_module(B_PCI_MODULE_NAME, (module_info**)&fPCI));

	fResumeCondition.Init(this, "nvidia_rm resume");
	fPowerCondition.Init(this, "nvidia_rm power");

	fIntrSafePool.Maintain();

	CHECK_RET(nv_haiku_work_queue_init());
	fWorkQueueInitDone = true;

	RmStack stack;
	if (!stack.IsValid())
		return B_NO_MEMORY;
	if (!rm_init_rm(stack.Get())) {
		return B_ERROR;
	}
	fInitRmDone = true;

	nv_haiku_apply_registry_settings(stack.Get());

	fControlDevice = new(std::nothrow) NvHaikuControlDevice();
	if (fControlDevice == nullptr) {
		return B_NO_MEMORY;
	}

	dprintf("Scan PCI:\n");
	pci_info pciInfo {};
	for (uint32 i = 0; fPCI->get_nth_pci_info(i, &pciInfo) >= B_OK; i++) {
		ObjectDeleter<NvHaikuDevice> device;
		if (NvHaikuDevice::Probe(device, pciInfo) < B_OK)
			continue;

		uint32 deviceIndex = fDevices.Count();
		CHECK_RET(fDevices.Add(device.Get()));
		device->SetDeviceIndex(deviceIndex);
		device.Detach();
	}

	CHECK_RET(fDeviceNamesArray.Init(1 + fDevices.Count()));
	fDeviceNamesArray.SetName(0, NVIDIA_CONTROL_DEVICE_NAME);

	if (device_manager_add_power_hook(PowerHook, this, "nvidia_rm") == B_OK)
		fPowerHookAdded = true;
	for (int32 i = 0; i < fDevices.Count(); i++) {
		char name[128];
		sprintf(name, NVIDIA_DEVICE_NAME "%" B_PRId32, i);
		fDeviceNamesArray.SetName(1 + i, name);
	}

	return B_OK;
}

// How long the resume of the GPUs may take before the rest of the system
// goes on without them: far longer than a resume should take, but a resume
// that does not return must not keep the devices after it (and the resume of
// the whole system) from coming back (airos-works/haiku#208).
static const bigtime_t kResumeTimeout = 15000000;
// How long a suspend waits for calls from user land that are running.
static const bigtime_t kUserCallDrainTimeout = 1000000;


status_t NvHaikuDriver::PowerHook(void *cookie, bool resume, int32 state)
{
	NvHaikuDriver *driver = static_cast<NvHaikuDriver*>(cookie);
	return resume ? driver->ResumeDevices() : driver->SuspendDevices();
}


/*!	Like Linux' nv_set_system_power_state(): user land is locked out from
	here to the end of the resume (nv_system_pm_lock there), the user
	channels are stopped, then each GPU suspends (NVKMS, then resman).
	Haiku does not freeze user land as Linux does during a suspend, so
	without the lock app_server's calls went on into a GPU in standby ("GPU
	not in full power"). The generation turns odd, which tells the accelerant
	to stop using its 2D engine until it turns even again.
*/
status_t NvHaikuDriver::SuspendDevices()
{
	{
		MutexLocker locker(fPowerLock);
		if (fPowerState != kPowerRunning) {
			dprintf("[!] nvidia_rm: the GPUs have not finished resuming from "
				"the last suspend; they are not suspended\n");
			return B_BUSY;
		}
		fPowerState = kPowerSuspended;
		fResumeGeneration++;
		fResumeCondition.NotifyAll();

		bigtime_t deadline = system_time() + kUserCallDrainTimeout;
		while (fUserCalls > 0) {
			ConditionVariableEntry entry;
			fPowerCondition.Add(&entry);
			locker.Unlock();
			status_t status = entry.Wait(B_ABSOLUTE_TIMEOUT, deadline);
			locker.Lock();
			if (status == B_TIMED_OUT && fUserCalls > 0) {
				dprintf("nvidia_rm: %" B_PRId32 " calls from user land still "
					"running at suspend\n", fUserCalls);
				break;
			}
		}
	}

	status_t result = B_OK;
	for (int32 i = 0; i < fDevices.Count(); i++)
		fDevices[i]->PreemptUserChannels();
	for (int32 i = fDevices.Count() - 1; i >= 0; i--) {
		status_t status = fDevices[i]->Suspend();
		if (status != B_OK)
			result = status;
	}
	return result;
}


/*!	The resume runs in a thread of its own: if it does not return within
	kResumeTimeout, the rest of the system resumes without the GPUs, calls
	from user land fail rather than wait for them, and a line says so. When
	it does finish, late, they are given back.
*/
status_t NvHaikuDriver::ResumeDevices()
{
	MutexLocker locker(fPowerLock);
	if (fPowerState != kPowerSuspended)
		return B_OK;
	fResumeDone = false;
	locker.Unlock();

	if (fResumeThread >= 0) {
		// the last one has finished, or the state would not be suspended
		status_t result;
		wait_for_thread(fResumeThread, &result);
		fResumeThread = -1;
	}
	thread_id thread = spawn_kernel_thread(&ResumeThreadEntry,
		"nvidia_rm resume", B_URGENT_DISPLAY_PRIORITY, this);
	if (thread < 0) {
		ResumeDevicesNow();
		return fResumeResult;
	}
	fResumeThread = thread;
	resume_thread(thread);

	locker.Lock();
	bigtime_t deadline = system_time() + kResumeTimeout;
	while (!fResumeDone) {
		ConditionVariableEntry entry;
		fPowerCondition.Add(&entry);
		locker.Unlock();
		status_t status = entry.Wait(B_ABSOLUTE_TIMEOUT, deadline);
		locker.Lock();
		if (status == B_TIMED_OUT && !fResumeDone) {
			fPowerState = kPowerResumeStuck;
			fPowerCondition.NotifyAll();
			dprintf("[!] nvidia_rm: the GPUs have not resumed after %" B_PRId64
				" s; the system goes on without them\n",
				kResumeTimeout / 1000000);
			return B_TIMED_OUT;
		}
	}
	return fResumeResult;
}


status_t NvHaikuDriver::ResumeThreadEntry(void *data)
{
	static_cast<NvHaikuDriver*>(data)->ResumeDevicesNow();
	return B_OK;
}


void NvHaikuDriver::ResumeDevicesNow()
{
	bigtime_t start = system_time();
	status_t result = B_OK;
	for (int32 i = 0; i < fDevices.Count(); i++) {
		status_t status = fDevices[i]->Resume();
		if (status != B_OK)
			result = status;
	}
	for (int32 i = 0; i < fDevices.Count(); i++)
		fDevices[i]->RestoreUserChannels();

	MutexLocker locker(fPowerLock);
	bool late = fPowerState == kPowerResumeStuck;
	fPowerState = kPowerRunning;
	fResumeResult = result;
	fResumeDone = true;
	fResumeGeneration++;
	fResumeCondition.NotifyAll();
	fPowerCondition.NotifyAll();
	locker.Unlock();

	dprintf("nvidia_rm: GPUs resumed%s in %" B_PRId64 " ms: %s\n",
		late ? " late" : "", (system_time() - start) / 1000, strerror(result));
}


status_t NvHaikuDriver::EnterUserCall(uint32 flags)
{
	MutexLocker locker(fPowerLock);
	while (fPowerState == kPowerSuspended) {
		ConditionVariableEntry entry;
		fPowerCondition.Add(&entry);
		locker.Unlock();
		status_t status = entry.Wait(flags & B_CAN_INTERRUPT);
		locker.Lock();
		if (status != B_OK)
			return status;
	}
	if (fPowerState == kPowerResumeStuck)
		return B_DEV_NOT_READY;
	fUserCalls++;
	return B_OK;
}


void NvHaikuDriver::LeaveUserCall()
{
	MutexLocker locker(fPowerLock);
	if (--fUserCalls == 0 && fPowerState != kPowerRunning)
		fPowerCondition.NotifyAll();
}


// The accelerant tells us where the screen's frame buffer is; anyone drawing
// with the GPU can then ask for it and put finished frames there directly.
void NvHaikuDriver::PublishScanout(const nv_haiku_scanout_info &info)
{
	MutexLocker _(&fScanoutLock);
	fScanout = info;
	dprintf("nvidia_rm: scanout published: %" B_PRIu32 "x%" B_PRIu32 ", %" B_PRIu32
		" bytes per row, client %#" B_PRIx32 ", memory %#" B_PRIx32 "\n",
		info.width, info.height, info.bytes_per_row, info.client, info.memory);
}

status_t NvHaikuDriver::GetScanout(nv_haiku_scanout_info &info)
{
	MutexLocker _(&fScanoutLock);
	if (fScanout.memory == 0)
		return B_DEV_NOT_READY;

	info = fScanout;
	return B_OK;
}


status_t NvHaikuDriver::WaitForResume(uint32 &generation, bigtime_t timeout)
{
	if (fResumeGeneration == generation) {
		ConditionVariableEntry entry;
		fResumeCondition.Add(&entry);
		if (fResumeGeneration == generation) {
			status_t status = entry.Wait(B_RELATIVE_TIMEOUT | B_CAN_INTERRUPT,
				timeout);
			if (status != B_OK && status != B_TIMED_OUT && status != B_WOULD_BLOCK)
				return status;
		}
	}

	generation = fResumeGeneration;
	return B_OK;
}


status_t NvHaikuDriver::InitDriver()
{
	dprintf("nvidia_rm: InitDriver\n");
	module_info *moduleInfo;
	return get_module(NV_HAIKU_MODULE_NAME, &moduleInfo);
}

void NvHaikuDriver::UninitDriver()
{
	dprintf("nvidia_rm: UninitDriver\n");
	put_module(NV_HAIKU_MODULE_NAME);
}

const char **NvHaikuDriver::PublishDevices()
{
	return Instance().fDeviceNamesArray.Names();
}

DevfsNodeInstance NvHaikuDriver::FindDevice(const char *name)
{
	if (strcmp(name, NVIDIA_CONTROL_DEVICE_NAME) == 0) {
		return DevfsNodeInstance(&gNvHaikuControlDeviceClass, Instance().fControlDevice);
	}
	uint32 index;
	if (ParseDeviceName(name, index)) {
		if (index >= (uint32)Instance().fDevices.Count()) {
			return DevfsNodeInstance();
		}
		return DevfsNodeInstance(&gNvHaikuDeviceClass, Instance().fDevices[index]);
	}
	return DevfsNodeInstance();
}


DRIVER_ENTRY_POINTS(NvHaikuDriver)
