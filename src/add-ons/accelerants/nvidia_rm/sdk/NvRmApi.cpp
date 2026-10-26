#include "NvRmApi.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <utility>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

extern "C" {
#include "nv.h"
#include "nv_escape.h"
#include "nv-unix-nvos-params-wrappers.h"
#include "nv-ioctl.h"
#include "nvstatus.h"

#include "ctrl/ctrl0000/ctrl0000unix.h"
}
#ifdef __HAIKU__
#include "nv-haiku.h"
#endif

#include "ErrorUtils.h"


int NvRmIoctl(int fd, NvU32 cmd, void *pParams, NvU32 paramsSize)
{
	int res;
	do {
#ifdef __HAIKU__
		res = ioctl(fd, cmd + NV_HAIKU_BASE, pParams, paramsSize);
#else
		res = ioctl(fd, _IOC(IOC_INOUT, NV_IOCTL_MAGIC, cmd, paramsSize), pParams);
#endif
	} while (res < 0 && (errno == EINTR || errno == EAGAIN));
	return res;
}


NvRmApi::NvRmApi()
{
	fFd = FileDesc(open("/dev/" NVIDIA_CONTROL_DEVICE_NAME, O_RDWR | O_CLOEXEC));
	assert(fFd.IsSet());
	fClient = NvRmObject(*this, Alloc(NV01_ROOT_CLIENT, 0, NULL));
}

NvHandle NvRmApi::Alloc(NvV32 hClass, NvHandle hParent, void *params, NvU32 paramsSize)
{
	NVOS21_PARAMETERS p {
		.hRoot = fClient.Get(),
		.hObjectParent = hParent,
		.hClass = hClass,
		.pAllocParms = params,
		.paramsSize = paramsSize
	};
	int ret = NvRmIoctl(fFd.Get(), NV_ESC_RM_ALLOC, &p, sizeof(p));
	CheckErrno(ret);
	CheckNvStatus(p.status);
	return p.hObjectNew;
}

// Take a reference to an object belonging to another client, which its owner
// has shared; the copy lives under hParent in this client and is freed like
// any other object of ours.
NvHandle NvRmApi::DupObject(NvHandle hParent, NvHandle hClientSrc, NvHandle hObjectSrc)
{
	NVOS55_PARAMETERS p {
		.hClient = fClient.Get(),
		.hParent = hParent,
		.hObject = 0,
		.hClientSrc = hClientSrc,
		.hObjectSrc = hObjectSrc,
		.flags = 0
	};
	int ret = NvRmIoctl(fFd.Get(), NV_ESC_RM_DUP_OBJECT, &p, sizeof(p));
	CheckErrno(ret);
	CheckNvStatus(p.status);
	return p.hObject;
}

void NvRmApi::Free(NvHandle hObject)
{
	NVOS00_PARAMETERS p {
		.hRoot = fClient.Get(),
		.hObjectOld = hObject
	};
	int ret = NvRmIoctl(fFd.Get(), NV_ESC_RM_FREE, &p, sizeof(p));
	CheckErrno(ret);
	CheckNvStatus(p.status);
}

void NvRmApi::Control(NvHandle hObject, NvV32 cmd, void *params, NvU32 paramsSize, NvU32 flags)
{
	NVOS54_PARAMETERS p {
		.hClient = fClient.Get(),
		.hObject = hObject,
		.cmd = cmd,
		.flags = flags,
		.params = params,
		.paramsSize = paramsSize
	};
	int ret = NvRmIoctl(fFd.Get(), NV_ESC_RM_CONTROL, &p, sizeof(p));
	CheckErrno(ret);
	CheckNvStatus(p.status);
}

void NvRmApi::MapMemory(NvU32 hDevice, NvU32 hMemory, NvU64 offset, NvU64 length, void *&pLinearAddress, NvU32 flags, int memFd)
{
	nv_ioctl_nvos33_parameters_with_fd p {
		.params = {
			.hClient = fClient.Get(),
			.hDevice = hDevice,
			.hMemory = hMemory,
			.offset = offset,
			.length = length,
			.pLinearAddress = pLinearAddress,
			.flags = flags
		},
		.fd = memFd
	};
	int ret = NvRmIoctl(fFd.Get(), NV_ESC_RM_MAP_MEMORY, &p, sizeof(p));
	CheckErrno(ret);
	CheckNvStatus(p.params.status);
	pLinearAddress = p.params.pLinearAddress;
}

void NvRmApi::UnmapMemory(NvU32 hDevice, NvU32 hMemory, void *pLinearAddress, NvU32 flags)
{
	NVOS34_PARAMETERS p {
		.hClient = fClient.Get(),
		.hDevice = hDevice,
		.hMemory = hMemory,
		.pLinearAddress = pLinearAddress,
		.status = 0,
		.flags = flags,
	};
	int ret = NvRmIoctl(fFd.Get(), NV_ESC_RM_UNMAP_MEMORY, &p, sizeof(p));
	CheckErrno(ret);
	CheckNvStatus(p.status);
}

NvU64 NvRmApi::MapMemoryDma(NvU32 hDevice, NvU32 hDma, NvU32 hMemory, NvU64 offset, NvU64 length, NvU32 flags)
{
	NVOS46_PARAMETERS p {
		.hClient = fClient.Get(),
		.hDevice = hDevice,
		.hDma = hDma,
		.hMemory = hMemory,
		.offset = offset,
		.length = length,
		.flags = flags,
	};
	int ret = NvRmIoctl(fFd.Get(), NV_ESC_RM_MAP_MEMORY_DMA, &p, sizeof(p));
	CheckErrno(ret);
	CheckNvStatus(p.status);
	return p.dmaOffset;
}

void NvRmApi::UnmapMemoryDma(NvU32 hDevice, NvU32 hDma, NvU32 hMemory, NvU32 flags, NvU64 dmaOffset)
{
	NVOS47_PARAMETERS p {
		.hClient = fClient.Get(),
		.hDevice = hDevice,
		.hDma = hDma,
		.hMemory = hMemory,
		.flags = flags,
		.dmaOffset = dmaOffset
	};
	int ret = NvRmIoctl(fFd.Get(), NV_ESC_RM_UNMAP_MEMORY_DMA, &p, sizeof(p));
	CheckErrno(ret);
	CheckNvStatus(p.status);
}

void NvRmApi::AllocOsEvent(int fd)
{
	nv_ioctl_alloc_os_event_t p {
		.hClient = fClient.Get(),
		.fd = (NvU32)fd
	};
	int ret = NvRmIoctl(fFd.Get(), NV_ESC_ALLOC_OS_EVENT, &p, sizeof(p));
	CheckErrno(ret);
	CheckNvStatus(p.Status);
}

void NvRmApi::FreeOsEvent(int fd)
{
	nv_ioctl_free_os_event_t p {
		.hClient = fClient.Get(),
		.fd = (NvU32)fd
	};
	int ret = NvRmIoctl(fFd.Get(), NV_ESC_FREE_OS_EVENT, &p, sizeof(p));
	CheckErrno(ret);
	CheckNvStatus(p.Status);
}

void NvRmApi::CardInfo(nv_ioctl_card_info_t *infos, NvU32 count)
{
	int ret = NvRmIoctl(fFd.Get(), NV_ESC_CARD_INFO, infos, sizeof(nv_ioctl_card_info_t)*count);
	CheckErrno(ret);
}
