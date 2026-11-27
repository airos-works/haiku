#pragma once

#include <module.h>

extern "C" {
#include "nvtypes.h"
#include "nv-gpu-info.h"
}


#define NV_HAIKU_MODULE_NAME "drivers/dev/graphics/nvidia_rm/v570.86.16"


/*
 * Callback functions from the RM OS interface layer into the NVKMS OS interface
 * layer.
 *
 * These functions should be called without the RM lock held, using the kernel's
 * native calling convention.
 */
typedef struct nvidia_modeset_callbacks_s {
    /*
     * Suspend & resume callbacks.  Note that these are called once per GPU.
     */
    void (*suspend)(NvU32 gpu_id);
    void (*resume)(NvU32 gpu_id);
} nvidia_modeset_callbacks_t;


struct nvidia_module_info {
	module_info base;

    struct {
        /* Availability of write combining support for video memory */
        NvBool allow_write_combining;
    } system_info;

    /*
     * Enumerate list of gpus probed by nvidia driver.
     *
     * gpu_info is an array of NVIDIA_MAX_GPUS elements. The number of GPUs
     * in the system is returned.
     */
    NvU32 (*enumerate_gpus)(nv_gpu_info_t *gpu_info);

    /*
     * {open,close}_gpu() raise and lower the reference count of the
     * specified GPU.  This is equivalent to opening and closing a
     * /dev/nvidiaN device file from user-space.
     */
    int (*open_gpu)(NvU32 gpu_id);
    void (*close_gpu)(NvU32 gpu_id);

    void (*op)(void *ops_cmd);

    int (*set_callbacks)(const nvidia_modeset_callbacks_t *cb);

	/*
	 * Calls from user land into NVKMS: like those into resman, they wait
	 * while the system suspends and resumes (B_CAN_INTERRUPT in flags lets
	 * them be interrupted), and fail while a resume is stuck. Each
	 * successful enter_user_call() is followed by a leave_user_call().
	 */
	status_t (*enter_user_call)(uint32 flags);
	void (*leave_user_call)(void);
};
