/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */

/*	The vertical blanks of the display that shows the boot frame buffer, for
	the framebuffer accelerant app_server uses while the driver does not
	drive the display itself (B_GET_BOOT_FRAME_BUFFER_RETRACE_SEMAPHORE).

	The firmware set the display engine (DCE 11.2) up and it stays the
	firmware's: nothing here writes a display register or touches the
	interrupt handler. A thread follows the frame counter of the head that
	scans the boot frame buffer out (retrace_follower.h); DCE counts frames
	at the start of the vertical sync, a few lines into the blank. */

#include "Retrace.h"

#include <new>

#include <KernelExport.h>

#include <retrace_follower.h>


namespace {

// the heads' register blocks (dword indices): graphics at +0, CRTC at +0x17c
const uint32 kHeads[] = {0x1a00, 0x1c00, 0x1e00, 0x4000, 0x4200, 0x4400};
const uint32 kGraphicsEnable = 0x0;			// GRPH_ENABLE: bit 0
const uint32 kSurfaceLow = 0x4;				// GRPH_PRIMARY_SURFACE_ADDRESS
const uint32 kSurfaceHigh = 0x7;			// ..._HIGH: address bits 39..32
const uint32 kCrtcControl = 0x19c;			// CRTC_CONTROL: bit 0 master enable
const uint32 kFrameCount = 0x1a6;			// CRTC_STATUS_FRAME_COUNT: 23..0


class DceRetraceFollower : public RetraceFollower {
public:
	DceRetraceFollower(RetraceSemaphores& semaphores,
		volatile uint32* registers, uint32 head)
		:
		RetraceFollower(semaphores, 0xffffff),
		fRegisters(registers + head)
	{
	}

protected:
	virtual int64 FrameCount()
	{
		if ((fRegisters[kCrtcControl] & 1) == 0)
			return -1;
		return fRegisters[kFrameCount] & 0xffffff;
	}

private:
	volatile uint32*	fRegisters;
};


RetraceSemaphores* sSemaphores;
DceRetraceFollower* sFollower;


/*!	The enabled head whose primary surface is the boot frame buffer: its
	place in the CPU's VRAM aperture (BAR0) is its offset in VRAM. */
int32
BootFrameBufferHead(volatile uint32* registers, const amdgpu_info& info)
{
	if (info.boot_framebuffer < info.bar_address[0]
		|| info.boot_framebuffer >= info.bar_address[0] + info.bar_size[0]) {
		return -1;
	}
	uint64 address = info.vram_gpu_base
		+ (info.boot_framebuffer - info.bar_address[0]);
	for (uint32 i = 0; i < B_COUNT_OF(kHeads); i++) {
		volatile uint32* head = registers + kHeads[i];
		if ((head[kCrtcControl] & 1) == 0 || (head[kGraphicsEnable] & 1) == 0)
			continue;
		uint64 surface = ((uint64)(head[kSurfaceHigh] & 0xff) << 32)
			| (head[kSurfaceLow] & 0xffffff00);
		if (surface == address)
			return i;
	}
	return -1;
}

}	// namespace


status_t
amdgpu_retrace_semaphore(volatile uint32* registers, const amdgpu_info& info,
	sem_id* _semaphore)
{
	if (registers == NULL)
		return B_NO_INIT;
	if (sFollower == NULL) {
		int32 head = BootFrameBufferHead(registers, info);
		if (head < 0) {
			dprintf("amdgpu: no head shows the boot frame buffer %#" B_PRIx64
				", no vertical blanks\n", info.boot_framebuffer);
			return B_NOT_SUPPORTED;
		}
		if (sSemaphores == NULL)
			sSemaphores = new(std::nothrow) RetraceSemaphores;
		if (sSemaphores == NULL)
			return B_NO_MEMORY;
		sFollower = new(std::nothrow) DceRetraceFollower(*sSemaphores,
			registers, kHeads[head]);
		if (sFollower == NULL)
			return B_NO_MEMORY;
		status_t status = sFollower->Start("amdgpu blanks");
		if (status != B_OK) {
			delete sFollower;
			sFollower = NULL;
			return status;
		}
		dprintf("amdgpu: vertical blanks of head %" B_PRId32 " (boot frame "
			"buffer)\n", head);
	}

	sem_id semaphore = sSemaphores->Get();
	if (semaphore < 0)
		return semaphore;
	*_semaphore = semaphore;
	return B_OK;
}


void
amdgpu_retrace_stop()
{
	delete sFollower;
	sFollower = NULL;
	delete sSemaphores;
	sSemaphores = NULL;
}
