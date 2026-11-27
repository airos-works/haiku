#include "BaseDevice.h"
#include "RmStack.h"
#include "Device.h"
#include "Driver.h"

#include <new>
#include <stdlib.h>
#include <string.h>

#include <sys/ioccom.h>

#include <KernelExport.h>
#include <device/graphic_driver.h>

#include <StackOrHeapArray.h>
#include <util/AutoLock.h>
#include <team.h>
#include <vm/vm.h>
//#include <fs/fd.h>
#include <fs/select_sync_pool.h>

extern status_t user_fd_kernel_ioctl(int fd, unsigned op, void *buffer,
	size_t length);

#include <kernel.h>

#include <AutoDeleter.h>
#include <AutoDeleterOS.h>

#define CHECK_RET(err) {status_t _err = (err); if (_err < B_OK) return _err;}


NvHaikuBaseDevice::NvHaikuBaseDevice()
{
	dprintf("+NvHaikuBaseDevice()\n");

	fNvState.os_state = this;
}

NvHaikuBaseDevice::~NvHaikuBaseDevice()
{
	dprintf("-NvHaikuBaseDevice()\n");
}


NvHaikuBaseDeviceHandle *NvHaikuBaseDeviceHandle::FromFd(int fd)
{
	// TODO: check that FD is Nvidia device

	NvHaikuBaseDeviceHandle *handle {};
	if (user_fd_kernel_ioctl(fd, NV_HAIKU_BASE + NV_HAIKU_GET_COOKIE, &handle, sizeof(handle)) < 0) {
		return nullptr;
	}
	return handle;
}

NvHaikuBaseDeviceHandle::NvHaikuBaseDeviceHandle(NvHaikuBaseDevice *device):
	fDevice(device)
{
	//dprintf("+NvHaikuBaseDeviceHandle\n");

	fNvfp.ctl_nvfp_priv = this;
}

NvHaikuBaseDeviceHandle::~NvHaikuBaseDeviceHandle()
{
	//dprintf("-NvHaikuBaseDeviceHandle\n");

	status_t gate = NvHaikuDriver::Instance().EnterUserCall(0);
	RmStack stack;
	rm_cleanup_file_private(stack.Get(), &fDevice->fNvState, &fNvfp);
	if (gate == B_OK)
		NvHaikuDriver::Instance().LeaveUserCall();

    ConsumeMmapContext();
}


// Release the mapping context RM stored with nv_add_mapping_context_to_file().
// It describes exactly one pending mapping, and RM refuses to store another one
// while it is still valid, so it has to be dropped once the mapping is done -
// this is what nvidia_mmap() does on Linux.  Keeping a stale context around
// makes every later mapping on the same file descriptor alias the first one.
void NvHaikuBaseDeviceHandle::ConsumeMmapContext()
{
    if (!fMmapContext.valid)
        return;

    free(fMmapContext.page_array);
    free(fMmapContext.memArea.pRanges);
    memset(&fMmapContext, 0, sizeof(fMmapContext));
}

status_t NvHaikuBaseDeviceHandle::Close()
{
	//dprintf("NvHaikuBaseDeviceHandle::Close()\n");

	fClosed.store(true);

	return B_OK;
}

status_t NvHaikuBaseDeviceHandle::Control(uint32 op, void *data, size_t len)
{
	if (fClosed.load())
		return EINVAL;

	switch (op) {
		case B_GET_ACCELERANT_SIGNATURE: {
			if (NV_IS_CTL_DEVICE(fDevice->Nv())) {
				return EINVAL;
			}

			CHECK_RET(user_strlcpy((char*)data, "nvidia_rm.accelerant", len));
			return B_OK;
		}
		default:
			break;
	}

	if (op < NV_HAIKU_BASE)
		return B_DEV_INVALID_IOCTL;

	uint32 cmd = op - NV_HAIKU_BASE;
	//dprintf("NvHaikuBaseDeviceHandle::Control(cmd: %" B_PRIu32 ", len: %" B_PRIu32 ")\n", cmd, len);

	// What reaches resman or the GPU waits while the system suspends and
	// resumes, as on Linux (nv_system_pm_lock): resman refused it then
	// ("GPU not in full power"), and the registers are not there. What only
	// looks at the driver's own state does not, waiting for the resume least
	// of all.
	switch (cmd) {
		case NV_HAIKU_GET_COOKIE:
		case NV_HAIKU_WAIT_FOR_RESUME:
		case NV_HAIKU_PUBLISH_SCANOUT:
		case NV_HAIKU_GET_SCANOUT:
		case NV_ESC_CARD_INFO:
			return ControlUngated(cmd, data, len);
	}

	CHECK_RET(NvHaikuDriver::Instance().EnterUserCall());
	status_t status = ControlUngated(cmd, data, len);
	NvHaikuDriver::Instance().LeaveUserCall();
	return status;
}


status_t NvHaikuBaseDeviceHandle::ControlUngated(uint32 cmd, void *data,
	size_t len)
{
	switch (cmd) {
		case NV_HAIKU_GET_COOKIE: {
			if (!IS_KERNEL_ADDRESS(data)) {
				dprintf("[!] !IS_KERNEL_ADDRESS(data)\n");
				return B_DEV_INVALID_IOCTL;
			}
			if (len != sizeof(void*)) {
				dprintf("[!] len != sizeof(void*)\n");
				return EINVAL;
			}
			*(void**)data = this;
			return B_OK;
		}
		case NV_HAIKU_READ_REGISTER: {
			nv_haiku_register_params params;
			if (len != sizeof(params) || NV_IS_CTL_DEVICE(fDevice->Nv()))
				return EINVAL;
			CHECK_RET(user_memcpy(&params, data, sizeof(params)));
			nv_state_t* nv = fDevice->Nv();
			if (nv->regs == NULL || nv->regs->map == NULL
				|| params.offset > nv->regs->size - 4 || (params.offset & 3) != 0)
				return B_BAD_VALUE;
			params.value = ((uint32*)nv->regs->map)[params.offset / 4];
			return user_memcpy(data, &params, sizeof(params));
		}
		case NV_HAIKU_READ_VRAM: {
			// read VRAM through the BAR0 PRAMIN window (NV_PBUS_BAR0_WINDOW)
			nv_haiku_vram_params params;
			if (len != sizeof(params) || NV_IS_CTL_DEVICE(fDevice->Nv()))
				return EINVAL;
			CHECK_RET(user_memcpy(&params, data, sizeof(params)));
			nv_state_t* nv = fDevice->Nv();
			uint32* regs = (uint32*)nv->regs->map;
			if (regs == NULL)
				return B_BAD_VALUE;
			static spinlock sWindowLock = B_SPINLOCK_INITIALIZER;
			{
				InterruptsSpinLocker locker(sWindowLock);
				uint32 oldWindow = regs[0x1700 / 4];
				regs[0x1700 / 4] = (uint32)(params.offset >> 16);
				for (int i = 0; i < 16; i++) {
					params.values[i] = regs[(0x700000 + (params.offset & 0xffff)
						+ i * 4) / 4];
				}
				regs[0x1700 / 4] = oldWindow;
			}
			return user_memcpy(data, &params, sizeof(params));
		}
		case NV_HAIKU_WAIT_FOR_RESUME: {
			nv_haiku_resume_params params;
			if (len != sizeof(params))
				return EINVAL;
			CHECK_RET(user_memcpy(&params, data, sizeof(params)));
			CHECK_RET(NvHaikuDriver::Instance().WaitForResume(params.generation,
				params.timeout));
			return user_memcpy(data, &params, sizeof(params));
		}
		case NV_HAIKU_PUBLISH_SCANOUT: {
			nv_haiku_scanout_info info;
			if (len != sizeof(info))
				return EINVAL;
			CHECK_RET(user_memcpy(&info, data, sizeof(info)));
			NvHaikuDriver::Instance().PublishScanout(info);
			return B_OK;
		}
		case NV_HAIKU_GET_SCANOUT: {
			nv_haiku_scanout_info info;
			if (len != sizeof(info))
				return EINVAL;
			CHECK_RET(NvHaikuDriver::Instance().GetScanout(info));
			return user_memcpy(data, &info, sizeof(info));
		}
		case NV_HAIKU_MAP: {
			nv_haiku_map_params params;
			if (len != sizeof(params)) {
				dprintf("[!] len != sizeof(params): %" B_PRIu32 ", %" B_PRIuSIZE "\n", len, sizeof(params));
				return EINVAL;
			}
			if (!IS_USER_ADDRESS(data)) {
				dprintf("[!] !IS_USER_ADDRESS(data)\n");
				return B_DEV_INVALID_IOCTL;
			}
			CHECK_RET(user_memcpy(&params, data, sizeof(params)));

			if (strnlen(params.name, B_OS_NAME_LENGTH) == B_OS_NAME_LENGTH) {
				dprintf("[!] strnlen(params.name, B_OS_NAME_LENGTH) == B_OS_NAME_LENGTH\n");
				return EINVAL;
			}

			if (!fMmapContext.valid) {
				dprintf("[!] !fMmapContext.valid\n");
				return EINVAL;
			}

			struct MmapContextConsumer {
				NvHaikuBaseDeviceHandle &handle;
				~MmapContextConsumer() {handle.ConsumeMmapContext();}
			} mmapContextConsumer {*this};

			AreaDeleter area;
			void *baseAddress {};
			if (NV_IS_CTL_DEVICE(fDevice->Nv())) {
				auto alloc = static_cast<nv_alloc_t*>(fMmapContext.alloc);
				area_id srcArea = alloc->area.Get();
				area.SetTo(vm_clone_area(
					team_get_current_team_id(),
					params.name,
					&baseAddress,
					params.addressSpec,
					params.protection & (B_USER_PROTECTION | B_CLONEABLE_AREA),
					REGION_NO_PRIVATE_MAP,
					srcArea,
					true
				));
				CHECK_RET(area.Get());
				params.address = (uint8*)baseAddress + fMmapContext.page_index * B_PAGE_SIZE;
				uint32 haikuCacheType = to_haiku_cache_type(alloc->cacheType);
				vm_set_area_memory_type(area.Get(), alloc->physBase, haikuCacheType);
			} else {
				if (fMmapContext.memArea.numRanges == 0 || fMmapContext.memArea.numRanges > (uint64)UINT32_MAX) {
					dprintf("[!] fMmapContext.memArea is invalid, numRanges: %" B_PRIu64 "\n", fMmapContext.memArea.numRanges);
					return EINVAL;
				}
				if (fMmapContext.memArea.numRanges == 1) {
					area.SetTo(vm_map_physical_memory(
						team_get_current_team_id(),
						params.name,
						&baseAddress,
						params.addressSpec,
						fMmapContext.memArea.pRanges[0].size,
						params.protection & (B_USER_PROTECTION | B_CLONEABLE_AREA),
						fMmapContext.memArea.pRanges[0].start,
						false
					));
				} else {
					area.SetTo(vm_map_physical_memory_vecs(
						team_get_current_team_id(),
						params.name,
						&baseAddress,
						params.addressSpec,
						nullptr,
						params.protection & (B_USER_PROTECTION | B_CLONEABLE_AREA),
						(generic_io_vec*)fMmapContext.memArea.pRanges,
						(uint32)fMmapContext.memArea.numRanges
					));
				}
				CHECK_RET(area.Get());
				params.address = baseAddress;
				phys_addr_t basePhysAddr = fMmapContext.memArea.pRanges[0].start;
				uint32 haikuCacheType = to_haiku_cache_type(fMmapContext.caching);
				vm_set_area_memory_type(area.Get(), basePhysAddr, haikuCacheType);
			}


			CHECK_RET(user_memcpy(data, &params, sizeof(params)));

			return area.Detach();
		}

		case NV_ESC_CARD_INFO: {
			if (!NV_IS_CTL_DEVICE(fDevice->Nv())) {
				return EINVAL;
			}

			size_t numArgDevices = len / sizeof(nv_ioctl_card_info_t);
			uint32 numDevices = NvHaikuDriver::Instance().DeviceCount();

			CHECK_RET(user_memset(data, 0, len));

			if (numArgDevices < numDevices) {
				return EINVAL;
			}

			for (uint32 i = 0; i < numDevices; i++) {
				auto dev = NvHaikuDriver::Instance().DeviceAt(i);
				nv_state_t *nv = dev->Nv();

		        if ((nv->flags & NV_FLAG_EXCLUDE) != 0)
		            continue;

				nv_ioctl_card_info_t ci {};
		        ci.valid              = NV_TRUE;
		        ci.pci_info.domain    = nv->pci_info.domain;
		        ci.pci_info.bus       = nv->pci_info.bus;
		        ci.pci_info.slot      = nv->pci_info.slot;
		        ci.pci_info.vendor_id = nv->pci_info.vendor_id;
		        ci.pci_info.device_id = nv->pci_info.device_id;
		        ci.gpu_id             = nv->gpu_id;
		        ci.interrupt_line     = nv->interrupt_line;
		        ci.reg_address        = nv->regs->cpu_address;
		        ci.reg_size           = nv->regs->size;
		        ci.minor_number       = dev->DeviceIndex();
	            ci.fb_address         = nv->fb->cpu_address;
	            ci.fb_size            = nv->fb->size;

	            CHECK_RET(user_memcpy(static_cast<nv_ioctl_card_info_t*>(data) + i, &ci, sizeof(nv_ioctl_card_info_t)));
			}

			return B_OK;
		}
	}

	BStackOrHeapArray<uint8, 32> kernelData(len);
	if (!kernelData.IsValid()) {
		return B_NO_MEMORY;
	}
	CHECK_RET(user_memcpy(&kernelData[0], data, len));

	RmStack stack;
	if (!stack.IsValid())
		return B_NO_MEMORY;
	if (rm_ioctl(stack.Get(), &fDevice->fNvState, &fNvfp, cmd, &kernelData[0], len) != NV_OK)
		return EINVAL;

	CHECK_RET(user_memcpy(data, &kernelData[0], len));

	return B_OK;
}

void NvHaikuBaseDeviceHandle::NotifySelectPool()
{
	MutexLocker _(&fSelectPoolMutex);
	notify_select_event_pool(fSelectPool, B_SELECT_READ);
}

status_t NvHaikuBaseDeviceHandle::Select(uint8 event, uint32 ref, selectsync *sync)
{
	//dprintf("NvHaikuBaseDeviceHandle::Select()\n");

	MutexLocker _(&fSelectPoolMutex);
	if (event != B_SELECT_READ)
		return B_BAD_VALUE;

	CHECK_RET(add_select_sync_pool_entry(&fSelectPool, sync, event));

	if (fDatalessEventPending.exchange(false)) {
		notify_select_event(sync, event);
	}

	return B_OK;
}

status_t NvHaikuBaseDeviceHandle::Deselect(uint8 event, selectsync *sync)
{
	//dprintf("NvHaikuBaseDeviceHandle::Deselect()\n");

	MutexLocker _(&fSelectPoolMutex);

	if (event != B_SELECT_READ)
		return B_BAD_VALUE;

	CHECK_RET(remove_select_sync_pool_entry(&fSelectPool, sync, event));

	return B_OK;
}
