#include "KmsDevice.h"

#include <new>

#include <team.h>

#include <AutoDeleter.h>
#include <util/AutoLock.h>

#include <fs/select_sync_pool.h>

#include "KmsDriver.h"
#include "rm/nv-haiku-kernel.h"


#define CHECK_RET(err) {status_t _err = (err); if (_err < B_OK) return _err;}


enum {
	NV_HAIKU_KMS_BASE = B_DEVICE_OP_CODES_END + 1,
};


const device_hooks gNvHaikuKmsDeviceClass = GetDevfsNodeClass<NvHaikuKmsDriver, NvHaikuKmsDevice, NvHaikuKmsDeviceHandle>();


status_t NvHaikuKmsDevice::Open(uint32 flags, DevfsNodeHandle &handle)
{
	ObjectDeleter<NvHaikuKmsDeviceHandle> handleImpl(new (std::nothrow) NvHaikuKmsDeviceHandle(this));
	if (!handleImpl.IsSet()) {
		return B_NO_MEMORY;
	}

	nvidia_module_info &rm = NvHaikuKmsDriver::Instance().NvidiaModule();
	CHECK_RET(rm.enter_user_call(B_CAN_INTERRUPT));
	{
		MutexLocker lock(&NvHaikuKmsDriver::Instance().Locker());
		handleImpl->fCookie = nvKmsOpen(team_get_current_team_id(), NVKMS_CLIENT_USER_SPACE, (nvkms_per_open_handle_t*)handleImpl.Get());
	}
	rm.leave_user_call();
	if (handleImpl->fCookie == NULL) {
		return B_NO_MEMORY;
	}

	handle = DevfsNodeHandle(nullptr, handleImpl.Detach());

	return B_OK;
}


NvHaikuKmsDeviceHandle::~NvHaikuKmsDeviceHandle()
{
	if (fCookie != NULL) {
		nvidia_module_info &rm = NvHaikuKmsDriver::Instance().NvidiaModule();
		status_t gate = rm.enter_user_call(0);
		{
			MutexLocker lock(&NvHaikuKmsDriver::Instance().Locker());
			nvKmsClose(fCookie);
		}
		if (gate == B_OK)
			rm.leave_user_call();
	}
}

NvHaikuKmsDeviceHandle::NvHaikuKmsDeviceHandle(NvHaikuKmsDevice *device):
	fDevice(device)
{
}

void NvHaikuKmsDeviceHandle::EventQueueChanged(NvBool eventsAvailable)
{
	MutexLocker _(&fSelectPoolMutex);
	fEventsAvailable = eventsAvailable;
	if (fEventsAvailable) {
		notify_select_event_pool(fSelectPool, B_SELECT_READ);
	}
}

status_t NvHaikuKmsDeviceHandle::Close()
{
	return B_OK;
}

status_t NvHaikuKmsDeviceHandle::Control(uint32 op, void *data, size_t len)
{
	if (op < NV_HAIKU_KMS_BASE) {
		return B_DEV_INVALID_IOCTL;
	}

	uint32 cmd = op - NV_HAIKU_KMS_BASE;

	// Not while the system suspends and resumes: NVKMS has shut its heads
	// down then, and resman is in standby (Linux: nvkms_ioctl() waits for
	// the pm lock that nvkms_suspend() takes).
	nvidia_module_info &rm = NvHaikuKmsDriver::Instance().NvidiaModule();
	CHECK_RET(rm.enter_user_call(B_CAN_INTERRUPT));
	NvBool res;
	{
		MutexLocker lock(&NvHaikuKmsDriver::Instance().Locker());
		res = nvKmsIoctl(fCookie, cmd, (addr_t)data, len);
	}
	rm.leave_user_call();
	return res ? B_OK : EINVAL;
}

status_t NvHaikuKmsDeviceHandle::Select(uint8 event, uint32 ref, selectsync *sync)
{
	MutexLocker _(&fSelectPoolMutex);
	if (event != B_SELECT_READ)
		return B_BAD_VALUE;

	CHECK_RET(add_select_sync_pool_entry(&fSelectPool, sync, event));

	if (fEventsAvailable) {
		notify_select_event(sync, event);
	}

	return B_OK;
}

status_t NvHaikuKmsDeviceHandle::Deselect(uint8 event, selectsync *sync)
{
	MutexLocker _(&fSelectPoolMutex);

	if (event != B_SELECT_READ)
		return B_BAD_VALUE;

	CHECK_RET(remove_select_sync_pool_entry(&fSelectPool, sync, event));

	return B_OK;
}
