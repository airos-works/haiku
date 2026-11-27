#include <string.h>
#include "RmStack.h"
#include <stdio.h>
#include <new>

#include <drivers/KernelExport.h>
#include <module.h>

#include "nv-include.h"
extern "C" {
#include "nv-kernel-rmapi-ops.h"
}

#include "nv-haiku-kernel.h"

#include "Driver.h"
#include "Device.h"


#define CHECK_RET(err) {status_t _err = (err); if (_err < B_OK) return _err;}


status_t nvidia_module_init()
{
	dprintf("nvidia_rm: module_init\n");

	new(&NvHaikuDriver::Instance()) NvHaikuDriver();
	status_t res = NvHaikuDriver::Instance().Init();
	if (res < B_OK) {
		NvHaikuDriver::Instance().~NvHaikuDriver();
	}
	return res;
}

void nvidia_module_uninit()
{
	dprintf("nvidia_rm: module_uninit\n");

	NvHaikuDriver::Instance().~NvHaikuDriver();
}

static NvU32 nvidia_module_enumerate_gpus(nv_gpu_info_t *gpu_info)
{
	dprintf("nvidia_module: enumerate_gpus()\n");
	// TODO: implement
	return 0;
}

int nvidia_module_open_gpu(NvU32 gpu_id)
{
	dprintf("nvidia_module: open_gpu(%" B_PRIu32 ")\n", gpu_id);

	uint32 numDevices = NvHaikuDriver::Instance().DeviceCount();
	for (uint32 i = 0; i < numDevices; i++) {
		auto dev = NvHaikuDriver::Instance().DeviceAt(i);
		nv_state_t *nv = dev->Nv();
		if (nv->gpu_id == gpu_id) {
			if (dev->AcquireRef() < B_OK) {
				return false;
			}
			return true;
		}
	}

	return false;
}

void nvidia_module_close_gpu(NvU32 gpu_id)
{
	dprintf("nvidia_module: close_gpu(%" B_PRIu32 ")\n", gpu_id);

	uint32 numDevices = NvHaikuDriver::Instance().DeviceCount();
	for (uint32 i = 0; i < numDevices; i++) {
		auto dev = NvHaikuDriver::Instance().DeviceAt(i);
		nv_state_t *nv = dev->Nv();
		if (nv->gpu_id == gpu_id) {
			dev->ReleaseRef();
			return;
		}
	}
}

void nvidia_module_op(void *ops_cmd)
{
	//dprintf("nvidia_module: op()\n");
	RmStack stack;
	rm_kernel_rmapi_op(stack.Get(), ops_cmd);
}

int nvidia_module_set_callbacks(const nvidia_modeset_callbacks_t *cb)
{
	dprintf("nvidia_module: set_callbacks(%p)\n", cb);
	NvHaikuDriver::Instance().SetModesetCallbacks(cb);
	return 0;
}


static status_t nvidia_module_enter_user_call(uint32 flags)
{
	return NvHaikuDriver::Instance().EnterUserCall(flags);
}

static void nvidia_module_leave_user_call()
{
	NvHaikuDriver::Instance().LeaveUserCall();
}


static status_t NvidiaModuleOps(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
			return nvidia_module_init();
		case B_MODULE_UNINIT:
			nvidia_module_uninit();
			return B_OK;
		default:
			return ENOSYS;
	}
}


nvidia_module_info gNvidiaModule = {
	.base = {
		.name = NV_HAIKU_MODULE_NAME,
		.std_ops = NvidiaModuleOps,
	},
	.system_info = {
		.allow_write_combining = true,
	},
	.enumerate_gpus = nvidia_module_enumerate_gpus,
	.open_gpu = nvidia_module_open_gpu,
	.close_gpu = nvidia_module_close_gpu,
	.op = nvidia_module_op,
	.set_callbacks = nvidia_module_set_callbacks,
	.enter_user_call = nvidia_module_enter_user_call,
	.leave_user_call = nvidia_module_leave_user_call,
};

_EXPORT module_info *modules[] = {
	(module_info *)&gNvidiaModule,
	nullptr
};
