#include "Device.h"
#include "RmStack.h"

#include <PCI.h>

extern "C" {
#include <os-interface.h>
}

#include "Driver.h"
#include "nv-haiku-kernel.h"


#define CHECK_RET(err) {status_t _err = (err); if (_err < B_OK) return _err;}

#define PCI_VENDOR_ID_NVIDIA 0x10DE


const device_hooks gNvHaikuDeviceClass = GetDevfsNodeClass<NvHaikuDriver, NvHaikuDevice, NvHaikuDeviceHandle>();


status_t NvHaikuDevice::Probe(ObjectDeleter<NvHaikuDevice> &outDevice, const pci_info &pciInfo)
{
	if (!(
		pciInfo.vendor_id == PCI_VENDOR_ID_NVIDIA &&
		pciInfo.class_base == PCI_display && (
			pciInfo.class_sub == PCI_vga ||
			pciInfo.class_sub == PCI_3d
		)
	))
		return ENODEV;

	// The 570 resource manager drives Maxwell and newer. Older GPUs (Kepler,
	// Fermi, Tesla: device IDs below GM108's 0x1340) are nouveau_drm's;
	// probing them here would touch a GPU another driver runs.
	if (pciInfo.device_id < 0x1340) {
		dprintf("  %02x.%02x.%02x: %04x.%04x is older than Maxwell, left to "
			"nouveau_drm\n", pciInfo.bus, pciInfo.device, pciInfo.function,
			pciInfo.vendor_id, pciInfo.device_id);
		return ENODEV;
	}

	dprintf("  %02x.%02x.%02x: %04x.%04x\n",
		pciInfo.bus,
		pciInfo.device,
		pciInfo.function,
		pciInfo.vendor_id,
		pciInfo.device_id
	);

	ObjectDeleter<NvHaikuDevice> device(new(std::nothrow) NvHaikuDevice());
	if (!device.IsSet() || !device->fIsrStack.IsValid()
		|| !device->fIsrBottomHalfStack.IsValid())
		return B_NO_MEMORY;

	CHECK_RET(device->Init(pciInfo));

	outDevice.SetTo(device.Detach());
	return B_OK;
}

NvHaikuDevice::NvHaikuDevice()
{
	dprintf("+NvHaikuDevice()\n");
}

NvHaikuDevice::~NvHaikuDevice()
{
	dprintf("-NvHaikuDevice()\n");

	nv_state_t *nv = Nv();

	if (fInitSoftware) {
		dprintf("rm_free_private_state()\n");
		RmStack stack;
		rm_free_private_state(stack.Get(), nv);
		fInitSoftware = false;
	}
}

status_t NvHaikuDevice::Init(const pci_info &pciInfo)
{
	nv_state_t *nv = Nv();

	nv->pci_info.domain    = 0;
	nv->pci_info.bus       = pciInfo.bus;
	nv->pci_info.slot      = pciInfo.device;
	nv->pci_info.function  = pciInfo.function;
	nv->pci_info.vendor_id = pciInfo.vendor_id;
	nv->pci_info.device_id = pciInfo.device_id;
	nv->subsystem_id       = pciInfo.u.h0.subsystem_id;
	nv->subsystem_vendor   = pciInfo.u.h0.subsystem_vendor_id;

	nv->handle = os_pci_init_handle(0, pciInfo.bus, pciInfo.device, pciInfo.function, nullptr, nullptr);

  nv->regs = &nv->bars[NV_GPU_BAR_INDEX_REGS];
  nv->fb   = &nv->bars[NV_GPU_BAR_INDEX_FB];

	uint32 curNvBar = 0;
	for (uint32 bar = 0; bar < 6; bar++) {
		uint32 curBar = bar;
		uint64 physAdr = pciInfo.u.h0.base_registers[bar];
		uint64 size    = pciInfo.u.h0.base_register_sizes[bar];
		if ((pciInfo.u.h0.base_register_flags[bar] & PCI_address_type) == PCI_address_type_64) {
			bar++;
			physAdr |= (uint64)pciInfo.u.h0.base_registers[bar] << 32;
			size    |= (uint64)pciInfo.u.h0.base_register_sizes[bar] << 32;
		}

		if (size > 0 && curNvBar < NV_GPU_NUM_BARS) {
			nv->bars[curNvBar].offset = NVRM_PCICFG_BAR_OFFSET(curBar);
			nv->bars[curNvBar].cpu_address = physAdr;
			nv->bars[curNvBar].size = size;
			curNvBar++;
		}

		dprintf("    BAR[%" B_PRIu32 "]: %#" B_PRIx64 ", %#" B_PRIx64 "\n",
			curBar,
			physAdr,
			size
		);
	}

	RmStack stack;
	if (!stack.IsValid())
		return B_NO_MEMORY;
	if (!rm_init_private_state(stack.Get(), nv))
		return B_ERROR;

	fInitSoftware = true;

	// GSP firmware is only requested for GPUs that require it. The
	// proprietary RM runs pre-Turing GPUs such as Pascal on the CPU.
	if (nv_is_rm_firmware_supported_os)
		rm_set_rm_firmware_requested(stack.Get(), nv);

	return B_OK;
}

status_t NvHaikuDevice::Start()
{
	dprintf("NvHaikuDevice()::Start()\n");
	nv_state_t *nv = Nv();

	NvU32 pciConfig = 0;
	os_pci_read_dword(nv->handle, PCI_command, &pciConfig);
	pciConfig |= PCI_command_io | PCI_command_memory | PCI_command_master;
	os_pci_write_dword(nv->handle, PCI_command, pciConfig);

	if (NvHaikuDriver::Instance().PCI().get_msi_count(nv->pci_info.bus, nv->pci_info.slot, nv->pci_info.function) < 1) {
		dprintf("[!] no MSI support\n");
		return B_ERROR;
	}

	NvHaikuDriver::Instance().PCI().configure_msi(nv->pci_info.bus, nv->pci_info.slot, nv->pci_info.function, 1, &fIrq);
	NvHaikuDriver::Instance().PCI().enable_msi(nv->pci_info.bus, nv->pci_info.slot, nv->pci_info.function);
	install_io_interrupt_handler(fIrq, InterruptHandler, (void *)this, 0);

	RmStack stack;
	if (!stack.IsValid())
		return B_NO_MEMORY;
	if (!rm_init_adapter(stack.Get(), nv))
		return B_IO_ERROR;

	fInitHardware = true;
	nv->flags |= NV_FLAG_OPEN;

	return B_OK;
}

status_t NvHaikuDevice::Stop()
{
	dprintf("NvHaikuDevice()::Stop()\n");
	nv_state_t *nv = Nv();

	if (fInitHardware) {
		RmStack stack;
		rm_shutdown_adapter(stack.Get(), nv);
		fInitHardware = false;
		nv->flags &= ~NV_FLAG_OPEN;
	}

	if (fIrq != 0) {
		remove_io_interrupt_handler(fIrq, InterruptHandler, (void *)this);

		NvHaikuDriver::Instance().PCI().disable_msi(nv->pci_info.bus, nv->pci_info.slot, nv->pci_info.function);
		NvHaikuDriver::Instance().PCI().unconfigure_msi(nv->pci_info.bus, nv->pci_info.slot, nv->pci_info.function);
	}

	NvU32 pciConfig = 0;
	os_pci_read_dword(nv->handle, PCI_command, &pciConfig);
	pciConfig &= ~(uint32)PCI_command_master;
	os_pci_write_dword(nv->handle, PCI_command, pciConfig);

	return B_OK;
}

status_t NvHaikuDevice::AcquireRef()
{
	if (fOpenCount.fetch_add(1, std::memory_order_seq_cst) == 0) {
		status_t res = Start();
		if (res < B_OK) {
			fOpenCount--;
			return res;
		}
	}
	return B_OK;
}

void NvHaikuDevice::ReleaseRef()
{
	if (fOpenCount.fetch_sub(1, std::memory_order_seq_cst) == 1) {
		Stop();
	}
}

void NvHaikuDevice::PreemptUserChannels()
{
	if (!fInitHardware || fUserChannelsStopped)
		return;

	nv_state_t *nv = Nv();
	RmStack stack;
	if (!stack.IsValid())
		return;

	NV_STATUS status = rm_ref_dynamic_power(stack.Get(), nv,
		NV_DYNAMIC_PM_FINE);
	if (status != NV_OK)
		dprintf("nvidia_rm: rm_ref_dynamic_power: %#" B_PRIx32 "\n", status);

	status = rm_stop_user_channels(stack.Get(), nv);
	if (status != NV_OK)
		dprintf("nvidia_rm: rm_stop_user_channels: %#" B_PRIx32 "\n", status);

	fUserChannelsStopped = true;
}


void NvHaikuDevice::RestoreUserChannels()
{
	if (!fUserChannelsStopped)
		return;

	nv_state_t *nv = Nv();
	RmStack stack;
	if (!stack.IsValid())
		return;

	NV_STATUS status = rm_restart_user_channels(stack.Get(), nv);
	if (status != NV_OK) {
		dprintf("nvidia_rm: rm_restart_user_channels: %#" B_PRIx32 "\n",
			status);
	}
	rm_unref_dynamic_power(stack.Get(), nv, NV_DYNAMIC_PM_FINE);

	fUserChannelsStopped = false;
}


status_t NvHaikuDevice::Suspend()
{
	if (!fInitHardware || fSuspended)
		return B_OK;

	nv_state_t *nv = Nv();
	dprintf("nvidia_rm: suspending GPU %#" B_PRIx32 "\n", nv->gpu_id);

	RmStack stack;
	if (!stack.IsValid())
		return B_NO_MEMORY;

	const nvidia_modeset_callbacks_t *callbacks
		= NvHaikuDriver::Instance().ModesetCallbacks();
	if (callbacks != nullptr && callbacks->suspend != nullptr)
		callbacks->suspend(nv->gpu_id);

	NV_STATUS status = rm_power_management(stack.Get(), nv,
		NV_PM_ACTION_STANDBY);
	if (status != NV_OK) {
		dprintf("nvidia_rm: rm_power_management(standby): %#" B_PRIx32 "\n",
			status);
		if (callbacks != nullptr && callbacks->resume != nullptr)
			callbacks->resume(nv->gpu_id);
		return B_ERROR;
	}

	fSuspended = true;
	return B_OK;
}


status_t NvHaikuDevice::Resume()
{
	if (!fSuspended)
		return B_OK;

	nv_state_t *nv = Nv();
	dprintf("nvidia_rm: resuming GPU %#" B_PRIx32 "\n", nv->gpu_id);

	RmStack stack;
	if (!stack.IsValid())
		return B_NO_MEMORY;

	bigtime_t start = system_time();
	NV_STATUS status = rm_power_management(stack.Get(), nv,
		NV_PM_ACTION_RESUME);
	dprintf("nvidia_rm: GPU %#" B_PRIx32 ": resman resumed in %" B_PRId64
		" ms: %#" B_PRIx32 "\n", nv->gpu_id, (system_time() - start) / 1000,
		status);

	fSuspended = false;

	if (status != NV_OK) {
		// Like Linux (nvidia_resume()): NVKMS resumes only on a GPU that
		// did; it would drive a display engine resman did not bring back.
		dprintf("[!] nvidia_rm: GPU %#" B_PRIx32 " did not resume; its "
			"displays stay off\n", nv->gpu_id);
		return B_ERROR;
	}

	const nvidia_modeset_callbacks_t *callbacks
		= NvHaikuDriver::Instance().ModesetCallbacks();
	if (callbacks != nullptr && callbacks->resume != nullptr) {
		start = system_time();
		callbacks->resume(nv->gpu_id);
		dprintf("nvidia_rm: GPU %#" B_PRIx32 ": NVKMS resumed in %" B_PRId64
			" ms\n", nv->gpu_id, (system_time() - start) / 1000);
	}

	return B_OK;
}


status_t NvHaikuDevice::Open(uint32 flags, DevfsNodeHandle &handle)
{
//	dprintf("NvHaikuDevice()::Open(%#" B_PRIx32 ")\n", flags);

	ObjectDeleter<NvHaikuDeviceHandle> handleImpl(new (std::nothrow) NvHaikuDeviceHandle(this));
	if (!handleImpl.IsSet())
		return B_NO_MEMORY;

	// (Linux' nvidia_open() waits for nv_system_pm_lock too: opening
	// starts a GPU that is not in use)
	CHECK_RET(NvHaikuDriver::Instance().EnterUserCall());
	status_t status = AcquireRef();
	NvHaikuDriver::Instance().LeaveUserCall();
	CHECK_RET(status);

	handle = DevfsNodeHandle(nullptr, handleImpl.Detach());

	return B_OK;
}

void NvHaikuDevice::Closed()
{
	ReleaseRef();
}

int32 NvHaikuDevice::InterruptHandler(void* arg)
{
	return static_cast<NvHaikuDevice*>(arg)->InterruptHandlerInt();
}

int32 NvHaikuDevice::InterruptHandlerInt()
{
	// dprintf("NvHaikuDevice::InterruptHandler()\n");

	nv_state_t *nv = Nv();

	NvU32 needBottomHalf = NV_FALSE;
	NvBool isIsrHandled = rm_isr(fIsrStack.Get(), nv, &needBottomHalf);
	if (isIsrHandled) {
		//dprintf("rm_isr(%d)\n", needBottomHalf);
		if (needBottomHalf)
			DPCQueue::DefaultQueue(B_URGENT_DISPLAY_PRIORITY)->Add(&fDeferredInterruptHandler);
	}

	return B_HANDLED_INTERRUPT;
}

void NvHaikuDevice::DeferredInterruptHandler::DoDPC(DPCQueue* queue)
{
	(void)queue;
//	dprintf("NvHaikuDevice::DeferredInterruptHandler()\n");
	nv_state_t *nv = Base().Nv();

	rm_isr_bh(Base().fIsrBottomHalfStack.Get(), nv);
}


NvHaikuDeviceHandle::NvHaikuDeviceHandle(NvHaikuDevice *device):
	NvHaikuBaseDeviceHandle(device)
{
//	dprintf("+NvHaikuDeviceHandle\n");
}

NvHaikuDeviceHandle::~NvHaikuDeviceHandle()
{
//	dprintf("-NvHaikuDeviceHandle\n");
}

status_t NvHaikuDeviceHandle::Close()
{
//	dprintf("NvHaikuDeviceHandle::Close()\n");
	// The last close stops the GPU: not while it is suspended (Linux'
	// nvidia_close() waits for nv_system_pm_lock too).
	status_t gate = NvHaikuDriver::Instance().EnterUserCall(0);
	GetDevice()->Closed();
	if (gate == B_OK)
		NvHaikuDriver::Instance().LeaveUserCall();
	return NvHaikuBaseDeviceHandle::Close();
}
