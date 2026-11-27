#pragma once

#include <atomic>

#include <PCI.h>

#include <condition_variable.h>
#include <lock.h>
#include <util/Vector.h>

#include "Drivers_cpp.h"
#include "DeviceNamesArray.h"

#include "IntrSafePool.h"
#include "nv-haiku.h"


class NvHaikuControlDevice;
class NvHaikuDevice;

status_t nv_haiku_work_queue_init();
void nv_haiku_work_queue_uninit();

typedef struct nvidia_stack_s nvidia_stack_t;
void nv_haiku_apply_registry_settings(nvidia_stack_t* sp);


class NvHaikuDriver {
private:
	pci_module_info *fPCI {};
	class IntrSafePool fIntrSafePool;

	bool fInitRmDone = false;
	bool fWorkQueueInitDone = false;

	bool fPowerHookAdded = false;
	const struct nvidia_modeset_callbacks_s *fModesetCallbacks {};

	std::atomic<uint32> fResumeGeneration {};
		// odd from the start of a suspend to the end of the resume
	ConditionVariable fResumeCondition;

	// System suspend and resume, see PowerHook().
	enum PowerState {
		kPowerRunning,
		kPowerSuspended,
			// from the start of a suspend to the end of the resume
		kPowerResumeStuck
			// the resume did not finish in time, and may still
	};
	mutex fPowerLock = MUTEX_INITIALIZER("nvidia_rm power");
	ConditionVariable fPowerCondition;
	PowerState fPowerState = kPowerRunning;
	int32 fUserCalls = 0;
	bool fResumeDone = false;
	status_t fResumeResult = B_OK;
	thread_id fResumeThread = -1;

	// Where the screen's frame buffer lives, as published by the accelerant.
	mutex fScanoutLock = MUTEX_INITIALIZER("nvidia_rm scanout");
	nv_haiku_scanout_info fScanout {};

	NvHaikuControlDevice *fControlDevice {};
	Vector<NvHaikuDevice*> fDevices;
	DeviceNamesArray fDeviceNamesArray;

	static NvHaikuDriver sInstance;

	static status_t PowerHook(void *cookie, bool resume, int32 state);
	status_t SuspendDevices();
	status_t ResumeDevices();
	static status_t ResumeThreadEntry(void *data);
	void ResumeDevicesNow();

public:
	~NvHaikuDriver();
	status_t Init();

	static NvHaikuDriver &Instance() {return sInstance;}
	pci_module_info &PCI() {return *fPCI;}
	class IntrSafePool &IntrSafePool() {return fIntrSafePool;}

	void SetModesetCallbacks(const struct nvidia_modeset_callbacks_s *callbacks)
		{fModesetCallbacks = callbacks;}
	const struct nvidia_modeset_callbacks_s *ModesetCallbacks()
		{return fModesetCallbacks;}

	status_t WaitForResume(uint32 &generation, bigtime_t timeout);

	// Calls from user land into resman and NVKMS: they wait while the
	// system suspends and resumes (B_CAN_INTERRUPT in \a flags lets them
	// be interrupted), and fail while a resume is stuck.
	status_t EnterUserCall(uint32 flags = B_CAN_INTERRUPT);
	void LeaveUserCall();

	void PublishScanout(const nv_haiku_scanout_info &info);
	status_t GetScanout(nv_haiku_scanout_info &info);

	uint32 DeviceCount() {return fDevices.Count();}
	NvHaikuDevice *DeviceAt(uint32 index) {return fDevices[index];}

	static status_t InitDriver();
	static void UninitDriver();
	static const char **PublishDevices();
	static DevfsNodeInstance FindDevice(const char *name);
};
