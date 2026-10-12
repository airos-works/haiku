/*
 * Copyright 2014 Advanced Micro Devices, Inc.
 * Copyright 2026, air/OS.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

// Bounded Polaris SDMA3 test, adapted from Linux sdma_v3_0.c (ring setup,
// packets, golden settings) and the protected SMU microcode loader. Register
// indices are dwords (oss_3_0_d.h). No interrupts, user command buffers, GART
// or power/clock policy are enabled by this first execution test.
#include "Sdma.h"
#include "Smu.h"
#include <KernelExport.h>
#include <stdlib.h>

static bool sQuarantined;
static const uint64 kScratchOffset = 32 * 1024 * 1024;
static const uint32 kScratchSize = 1024 * 1024;
// The following 2 MiB belongs to the SMU until a cold boot. Future general
// allocations must preserve this reservation, including after closing a fd.
static const uint32 kReservedSize = 3 * 1024 * 1024;
static const uint32 kPatternSize = 64 * 1024;
static const uint32 kSource = 0x10000;
static const uint32 kCopy = 0x30000;
static const uint32 kFill = 0x50000;
static const uint32 kFence = 0x1000;

static void
LogDiagnostics(volatile uint32* r)
{
	const uint32 direct[] = {0x819, 0x80d, 0x80e, 0x80f, 0x500, 0x501,
		0x502, 0x504, 0x536, 0x538, 0x53e, 0x3400, 0x340d, 0x340e, 0x3423};
	for (uint32 index : direct)
		dprintf("amdgpu: execution reg[%04x] = %08x\n", (unsigned)index,
			(unsigned)r[index]);
	uint32 oldIndex = r[0x1ac];
	uint32 oldAccess = r[0x92];
	r[0x92] = oldAccess & ~0x800u; // disable index 11 auto-increment
	const uint32 indirect[] = {0x80000004, 0x80000370, 0xe00030a4,
		0xe0003088, 0x3f000, 0x20030};
	for (uint32 index : indirect) {
		r[0x1ac] = index;
		dprintf("amdgpu: SMC[%08x] = %08x\n", (unsigned)index,
			(unsigned)r[0x1ad]);
	}
	r[0x1ac] = oldIndex;
	r[0x92] = oldAccess;
	(void)r[0x92];
}


static bool
Overlaps(uint64 start, uint64 size, uint64 other, uint64 otherSize)
{
	return size != 0 && otherSize != 0
		&& (start < other ? other - start < size : start - other < otherSize);
}


bool
amdgpu_vram_range_is_safe(volatile uint32* r, const amdgpu_info& info,
	const amdgpu::AtomVramReservation& reservation, uint64 offset, uint64 size)
{
	if (offset > info.bar_size[0] || size > info.bar_size[0] - offset
		|| info.vram_size < info.bar_size[0]
		|| info.boot_framebuffer < info.bar_address[0]
		|| info.boot_framebuffer - info.bar_address[0] >= info.bar_size[0]
		|| info.boot_framebuffer_size == 0 || reservation.start > info.vram_size
		|| reservation.size > info.vram_size - reservation.start)
		return false;
	if (Overlaps(offset, size, reservation.start, reservation.size)
		|| Overlaps(offset, size,
			info.boot_framebuffer - info.bar_address[0], info.boot_framebuffer_size))
		return false;
	// Firmware may put driver scratch immediately before its own reservation,
	// or at the end of the CPU aperture when no firmware area is recorded.
	uint64 scratchEnd = reservation.start != 0
		? reservation.start : info.bar_size[0];
	if (reservation.driverScratchSize > scratchEnd
		|| Overlaps(offset, size,
			scratchEnd - reservation.driverScratchSize, reservation.driverScratchSize))
		return false;
	const uint32 heads[] = {0x1a00, 0x1c00, 0x1e00, 0x4000, 0x4200, 0x4400};
	uint64 bootGPU = info.vram_gpu_base + info.boot_framebuffer - info.bar_address[0];
	for (uint32 base : heads) {
		if ((r[base + 0x19c] & 1) == 0)
			continue;
		uint64 surface = (uint64)r[base + 7] << 32 | (r[base + 4] & ~0xffu);
		dprintf("amdgpu: head %#x surface %#" B_PRIx64 " pitch %u control %#x\n",
			(unsigned)base, surface, (unsigned)r[base + 6],
			(unsigned)r[base + 1]);
		// Until DCE owns the displays, admit only the firmware's linear 32-bit
		// framebuffer. Unknown/tiled surfaces need a real allocator reservation.
		if (surface != bootGPU || r[base + 6] * 4 != info.boot_stride
			|| (r[base + 1] & 0xe00003) != 2) // linear general or aligned
			return false;
		if ((r[base + 0x66] & 1) != 0) {
			uint64 cursor = (uint64)r[base + 0x69] << 32 | (r[base + 0x67] & ~0xffu);
			if (Overlaps(info.vram_gpu_base + offset, size,
					cursor, 128 * 128 * 4))
				return false;
		}
	}
	return true;
}


status_t
amdgpu_sdma_test(volatile uint32* r, const amdgpu_info& info,
	const amdgpu::FirmwareView& firmware,
	const amdgpu::AtomVramReservation& reservation, amdgpu_sdma_test_result& result)
{
	result.stage = 1;
	if (sQuarantined || !amdgpu_smc_ready(r))
		return B_DEV_NOT_READY;
	dprintf("amdgpu: SDMA preflight halt %#x rings %#x/%#x/%#x\n",
		(unsigned)r[0x3412], (unsigned)r[0x3480], (unsigned)r[0x3500],
		(unsigned)r[0x3580]);
	// Never take a ring away from another driver or a running firmware job.
	// A failed test leaves RB_CNTL configured even after disabling its ring.
	// Reject that state even if the driver has since been unloaded/reloaded.
	if ((r[0x3412] & 1) == 0 || r[0x3480] != 0
		|| (r[0x3500] & 1) != 0 || (r[0x3580] & 1) != 0
		|| !amdgpu_vram_range_is_safe(r, info, reservation, kScratchOffset, kReservedSize))
		return B_NOT_ALLOWED;
	LogDiagnostics(r);
	uint32* backup = (uint32*)malloc(kScratchSize);
	if (backup == NULL)
		return B_NO_MEMORY;
	volatile uint32* vram = NULL;
	area_id area = map_physical_memory("amdgpu SDMA scratch",
		info.bar_address[0] + kScratchOffset, kReservedSize, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&vram);
	if (area < 0) {
		free(backup);
		return area;
	}
	for (uint32 i = 0; i < kScratchSize / 4; i++)
		backup[i] = vram[i];
	result.stage = 2;
	uint64 gpu = info.vram_gpu_base + kScratchOffset;
	result.scratch_gpu = gpu;
	result.firmware_version = firmware.version;
	// Keep all unused memory as a guard, and back up/restore the complete
	// region. The destination and fence never receive expected values by CPU.
	for (uint32 i = 0; i < kScratchSize / 4; i++)
		vram[i] = 0xcccccccc;
	for (uint32 i = 0; i < kPatternSize / 4; i++)
		vram[kSource / 4 + i] = 0x3ca50000u ^ (i * 0x10204081u);
	vram[kFence / 4] = 0;
	for (uint32 i = 0; i < 1024; i++)
		vram[i] = 0; // NOP ring

	const uint32 savedRegs[] = {0x391, 0x3403, 0x3404, 0x3405, 0x3406,
		0x3409, 0x3412, 0x3480, 0x3481, 0x3482, 0x3483, 0x3484, 0x3485,
		0x3488, 0x3489, 0x348a, 0x348b, 0x348c, 0x3492, 0x34a7, 0x34a8};
	uint32 saved[B_COUNT_OF(savedRegs)];
	uint32 oldSelect = r[0x391];
	r[0x391] = 0; // VMID 0, pipe/queue/ME 0
	for (uint32 i = 0; i < B_COUNT_OF(savedRegs); i++)
		saved[i] = r[savedRegs[i]];
	saved[0] = oldSelect;
	// The UEFI framebuffer does not establish the system aperture used by
	// SDMA VMID 0. Match gmc_v8_0_mc_program()/gart_enable(): physical VRAM
	// inside this aperture bypasses translation; no system RAM or page tables
	// are exposed by this test. Leave VM_CONTEXT0 disabled. Retain the
	// aperture afterwards because the SMU owns its workspace until a cold boot.
	r[0x80d] = info.vram_gpu_base >> 12;
	r[0x80e] = (info.vram_gpu_base + info.vram_size - 1) >> 12;
	r[0x80f] = (gpu + 0x2f0000) >> 12; // reserved default page, never system RAM
	r[0x819] = (r[0x819] & ~0x7bu) | 0x5b;
	(void)r[0x819];
	// The SMU authenticates and loads SDMA0; direct UCODE_DATA writes cannot
	// start the protected Polaris micro-engine. No other engine is loaded.
	r[0x3404] &= ~(0x40000u | 1u); // no auto context switch or trap interrupt
	status_t load = amdgpu_smc_load_sdma(r, firmware,
		vram + kScratchSize / 4, gpu + kScratchSize);
	if (load != B_OK) {
		// A timed-out controller may still reference its VRAM. Do not restore
		// the aperture or reuse this reservation. Mark it across driver reloads.
		sQuarantined = true;
		r[0x3480] = 10 << 1;
		result.halted = r[0x3412] & 1;
		delete_area(area);
		free(backup);
		return load;
	}
	result.stage = 3;
	r[0x3405] = (r[0x3405] & ~0xfc910007u) | 0x00810007;
	r[0x3403] &= ~0xff000fffu; // Polaris golden clock control, ungated
	r[0x3406] = r[0x263e] & 0x70;
	r[0x3409] = 0;
	r[0x34a7] = 0;
	r[0x34a8] = 0;
	r[0x3480] = 10 << 1; // 1024 dwords, no rptr writeback, no byte swap
	r[0x3483] = 0;
	r[0x3484] = 0;
	r[0x348b] = 0;
	r[0x348c] = 0;
	r[0x3488] = (gpu + 0x1010) >> 32;
	r[0x3489] = (uint32)(gpu + 0x1010);
	r[0x3481] = gpu >> 8;
	r[0x3482] = gpu >> 40;
	r[0x3492] &= ~0x10000000u; // MMIO wptr, no doorbell
	r[0x3485] &= ~1u; // no memory wptr polling
	r[0x348a] = 0x100; // no indirect buffers

	uint32 n = 0;
	// WRITE_LINEAR, COPY_LINEAR, CONST_FILL, then FENCE. Counts on SDMA3
	// are actual counts, unlike SDMA4's count-minus-one encoding.
	vram[n++] = 2;
	vram[n++] = (uint32)(gpu + kCopy);
	vram[n++] = (gpu + kCopy) >> 32;
	vram[n++] = 1;
	vram[n++] = 0x12345678;
	vram[n++] = 1;
	vram[n++] = kPatternSize;
	vram[n++] = 0;
	vram[n++] = (uint32)(gpu + kSource);
	vram[n++] = (gpu + kSource) >> 32;
	vram[n++] = (uint32)(gpu + kCopy);
	vram[n++] = (gpu + kCopy) >> 32;
	vram[n++] = 11 | (2u << 30); // FILL_SIZE=2: repeat a 32-bit value
	vram[n++] = (uint32)(gpu + kFill);
	vram[n++] = (gpu + kFill) >> 32;
	vram[n++] = 0x71a4c93e;
	vram[n++] = kPatternSize;
	vram[n++] = 5;
	vram[n++] = (uint32)(gpu + kFence);
	vram[n++] = (gpu + kFence) >> 32;
	vram[n++] = 0xdeadbeef;
	while ((n & 7) != 0)
		vram[n++] = 0;
	__sync_synchronize();
	(void)vram[n - 1]; // drain PCI writes before publishing the command ring
	r[0x1520] = 1; // vi_flush_hdp: publish CPU writes to the GPU
	(void)r[0x1520];
	r[0x3480] |= 1;
	r[0x3412] &= ~1u;
	bigtime_t start = system_time();
	r[0x3484] = n * 4;
	(void)r[0x3484];
	result.stage = 4;
	while (vram[kFence / 4] != 0xdeadbeef && system_time() - start < 500000)
		snooze(50);
	result.elapsed_us = system_time() - start;
	result.fence = vram[kFence / 4];
	result.rptr = r[0x3483];
	result.wptr = r[0x3484];
	result.engine_status = r[0x340d];
	LogDiagnostics(r);
	status_t status = result.fence == 0xdeadbeef ? B_OK : B_TIMED_OUT;
	// Wait for trailing NOPs, disable the ring, and halt before releasing any
	// scratch. If it will not become idle, quarantine until a real reboot.
	start = system_time();
	while ((r[0x340d] & 1) == 0 && system_time() - start < 100000)
		snooze(50);
	r[0x3480] &= ~1u;
	r[0x3412] |= 1;
	(void)r[0x3412];
	result.halted = r[0x3412] & 1;
	if ((r[0x340d] & 1) == 0 || result.halted == 0) {
		sQuarantined = true;
		status = B_DEV_NOT_READY;
	} else {
		if (status == B_OK) {
			for (uint32 i = 1024; i < kScratchSize / 4; i++) {
				uint32 expected = 0xcccccccc;
				uint32 byte = i * 4;
				if (byte == kFence)
					expected = 0xdeadbeef;
				else if (byte >= kSource && byte < kSource + kPatternSize)
					expected = 0x3ca50000u ^ ((i - kSource / 4) * 0x10204081u);
				else if (byte >= kCopy && byte < kCopy + kPatternSize)
					expected = 0x3ca50000u ^ ((i - kCopy / 4) * 0x10204081u);
				else if (byte >= kFill && byte < kFill + kPatternSize)
					expected = 0x71a4c93e;
				if (vram[i] != expected) {
					if (result.mismatches < 8)
						dprintf("amdgpu: mismatch +%#x got %#x expected %#x\n",
							(unsigned)byte, (unsigned)vram[i], (unsigned)expected);
					result.mismatches++;
				}
				result.checked_bytes += 4;
			}
			if (result.mismatches != 0)
				status = B_BAD_DATA;
			else
				result.stage = 5;
		}
		for (uint32 i = 0; i < kScratchSize / 4; i++)
			vram[i] = backup[i];
		__sync_synchronize();
		(void)vram[kScratchSize / 4 - 1];
		for (uint32 i = B_COUNT_OF(savedRegs); i-- > 0;)
			r[savedRegs[i]] = saved[i];
		(void)r[0x391];
	}
	delete_area(area);
	free(backup);
	dprintf("amdgpu: SDMA stage %u status %#x fence %#x rptr %u/%u errors %u\n",
		(unsigned)result.stage, (unsigned)status, (unsigned)result.fence,
		(unsigned)result.rptr, (unsigned)result.wptr, (unsigned)result.mismatches);
	return status;
}
