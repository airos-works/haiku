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

#include "Sdma.h"
#include "Smu.h"
#include <KernelExport.h>

status_t
SdmaEngine::Initialize(volatile uint32* r, const amdgpu_info& info,
	const amdgpu::FirmwareView& firmware,
	const amdgpu::AtomVramReservation& reservation)
{
	area = -1;
	if (!amdgpu_smc_ready(r))
		return B_DEV_NOT_READY;
	if ((r[0x3412] & 1) == 0 || r[0x3480] != 0
		|| (r[0x3500] & 1) != 0 || (r[0x3580] & 1) != 0
		|| !amdgpu_vram_range_is_safe(r, info, reservation, 32ULL << 20, 3ULL << 20))
		return B_NOT_ALLOWED;
	area = map_physical_memory("amdgpu kernel VRAM", info.bar_address[0] + (32ULL << 20),
		3 << 20, B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		(void**)&memory);
	if (area < 0)
		return area;
	regs = r;
	gpu = info.vram_gpu_base + (32ULL << 20);
	sequence = 0;
	faulted = false;
	for (uint32 i = 0; i < (1 << 20) / 4; i++)
		memory[i] = 0;
	r[0x80d] = info.vram_gpu_base >> 12;
	r[0x80e] = (info.vram_gpu_base + info.vram_size - 1) >> 12;
	r[0x80f] = (gpu + 0x2f0000) >> 12;
	r[0x819] = (r[0x819] & ~0x7bu) | 0x5b;
	r[0x3404] &= ~(0x40000u | 1u);
	status_t status = amdgpu_smc_load_sdma(r, firmware,
		memory + (1 << 20) / 4, gpu + (1 << 20));
	if (status != B_OK) {
		faulted = true;
		r[0x3480] = 20; // retain ownership/failure marker across driver reloads
		Uninitialize();
		return status;
	}
	Configure();
	return B_OK;
}

void
SdmaEngine::Configure()
{
	volatile uint32* r = regs;
	r[0x3404] &= ~(0x40000u | 1u);
	r[0x3405] = (r[0x3405] & ~0xfc910007u) | 0x00810007;
	r[0x3403] &= ~0xff000fffu;
	r[0x3406] = r[0x263e] & 0x70;
	r[0x3409] = 0;
	uint32 select = r[0x391];
	r[0x391] = 0;
	r[0x34a7] = r[0x34a8] = 0;
	r[0x3480] = 20;
	r[0x3483] = r[0x3484] = r[0x348b] = r[0x348c] = 0;
	r[0x3488] = (gpu + 0x1010) >> 32;
	r[0x3489] = (uint32)(gpu + 0x1010);
	r[0x3481] = gpu >> 8;
	r[0x3482] = gpu >> 40;
	r[0x3492] &= ~0x10000000u;
	r[0x3485] &= ~1u;
	r[0x348a] = 0x100;
	r[0x391] = select;
}

void
SdmaEngine::Stop()
{
	regs[0x3404] &= ~0x40000u;
	regs[0x3480] &= ~1u;
	regs[0x348a] &= ~1u;
	regs[0x3412] |= 1;
	(void)regs[0x3412];
}

status_t
SdmaEngine::TimeoutTest(amdgpu_sdma_timeout_test& result)
{
	if (area < 0 || faulted) return B_DEV_NOT_READY;
	// Linux sdma_v3_0_ring_emit_pipeline_sync: memory equality poll,
	// private VRAM only. First prove the packet with an already-met value.
	auto poll = [&](uint32 value) {
		memory[0x2000 / 4] = value;
		memory[0] = 8u | (3u << 28) | (1u << 31);
		memory[1] = uint32(gpu + 0x2000); memory[2] = (gpu + 0x2000) >> 32;
		memory[3] = 1; memory[4] = 0xffffffff; memory[5] = 0x0fff0004;
		return Submit(6);
	};
	status_t status = poll(1);
	bigtime_t start = system_time();
	if (status == B_OK) status = poll(0);
	result.elapsed_us = system_time() - start;
	result.sequence = sequence; result.fence = memory[0x1000 / 4];
	result.rptr = regs[0x3483]; result.wptr = regs[0x3484];
	result.engine_status = regs[0x340d];
	// Even unexpected retirement cannot make this diagnostic healthy.
	result.status = status == B_OK ? B_BAD_DATA : status;
	faulted = true; Stop();
	return result.status;
}

status_t
SdmaEngine::Recover(amdgpu_recovery_info& result)
{
	if (area < 0 || !faulted || regs == NULL || memory == NULL)
		return B_DEV_NOT_READY;
	volatile uint32* r = regs;
	// Own SDMA0 only. No RLC queue or SDMA1 execution may be active.
	// Do not reset shared SEM, MC, display, UVD, or an unowned DMA engine.
	const uint32 mask = 0x100000;
	uint32 reset = r[0x398];
	if ((reset & mask) != 0 || (r[0x3500] & 1) || (r[0x3580] & 1)
		|| !(r[0x3612] & 1) || (r[0x3680] & 1) || (r[0x3700] & 1)
		|| (r[0x3780] & 1) || (r[0x393] & 0x44)) return B_NOT_ALLOWED;
	const uint32 heads[] = {0x1a00, 0x1c00, 0x1e00, 0x4000, 0x4200, 0x4400};
	const uint32 offsets[] = {1, 4, 6, 7, 0x19c};
	uint32 display[6][5];
	for (uint32 h = 0; h < 6; h++)
		for (uint32 i = 0; i < 5; i++) display[h][i] = r[heads[h] + offsets[i]];
	result.stage = 11;
	Stop();
	dprintf("amdgpu: SDMA recovery before SRBM2 %#x status %#x ring %u/%u fence %u/%u\n",
		(unsigned)r[0x393], (unsigned)r[0x340d], (unsigned)r[0x3483],
		(unsigned)r[0x3484], (unsigned)memory[0x1000 / 4], sequence);
	// Linux 6.18.52 sdma_v3_0 pre/soft/post reset. Microcode is retained;
	// the post-reset path reprograms ring state and proves execution.
	result.srbm_reset |= mask;
	r[0x398] = reset | mask; (void)r[0x398]; snooze(50);
	r[0x398] = reset; (void)r[0x398]; snooze(50);
	Stop();
	bigtime_t deadline = system_time() + 100000;
	while ((r[0x393] & 0x21) != 0 && system_time() < deadline) snooze(10);
	if ((r[0x393] & 0x21) != 0 || r[0x398] != reset) return B_DEV_NOT_READY;
	// All execution owners have retired and initialized graphics is halted.
	r[0x50c] |= 1; r[0x50d] |= 1; (void)r[0x50d];
	if (((r[0x536] | r[0x537]) & 0xff) != 0) return B_BAD_DATA;
	for (uint32 i = 0; i < (1 << 20) / 4; i++) memory[i] = 0;
	Configure(); sequence = 0;
	result.stage = 12;
	for (uint32 i = 0x3000 / 4; i < 0xa000 / 4; i++) memory[i] = 0xabcddcba;
	for (uint32 i = 0; i < 1024; i++) memory[0x4000 / 4 + i] = 0x13579bdf ^ (i * 0x10204081u);
	__sync_synchronize();
	faulted = false; // private proof only; global fault/mutex still exclude clients
	status_t status = Execute(AMDGPU_DMA_FILL, 0, gpu + 0x8000, 4096, 0x5a96c37e);
	if (status == B_OK) status = Execute(AMDGPU_DMA_COPY, gpu + 0x4000, gpu + 0x6000, 4096, 0);
	r[0xbcc] = 1; (void)r[0xbcc]; __sync_synchronize();
	for (uint32 i = 0x3000 / 4; status == B_OK && i < 0xa000 / 4; i++) {
		uint32 offset = i * 4, expected = 0xabcddcba;
		if ((offset >= 0x4000 && offset < 0x5000) || (offset >= 0x6000 && offset < 0x7000))
			expected = 0x13579bdf ^ ((i & 1023) * 0x10204081u);
		else if (offset >= 0x8000 && offset < 0x9000) expected = 0x5a96c37e;
		if (memory[i] != expected) status = B_BAD_DATA;
	}
	if (status == B_OK && (sequence != 2 || memory[0x1000 / 4] != 2
		|| r[0x3483] != r[0x3484] || !(r[0x340d] & 1)
		|| !(r[0x3412] & 1) || (r[0x3480] & 1))) status = B_BAD_DATA;
	for (uint32 h = 0; status == B_OK && h < 6; h++)
		for (uint32 i = 0; i < 5; i++)
			if (r[heads[h] + offsets[i]] != display[h][i]) status = B_BAD_DATA;
	if (status != B_OK) { faulted = true; Stop(); }
	else dprintf("amdgpu: SDMA reset private fill/copy/guards/display proof passed\n");
	return status;
}

status_t
SdmaEngine::Execute(uint32 operation, uint64 source, uint64 destination,
	uint64 bytes, uint32 value)
{
	if (area < 0 || faulted)
		return B_DEV_NOT_READY;
	if ((operation != AMDGPU_DMA_COPY && operation != AMDGPU_DMA_FILL)
		|| bytes == 0 || bytes > (64ULL << 20) || ((source | destination | bytes) & 3) != 0)
		return B_BAD_VALUE;
	if (operation == AMDGPU_DMA_COPY) {
		SdmaCopy copy = {source, destination, bytes};
		return CopyRegions(&copy, 1);
	}
	uint32 n = 0;
	while (bytes != 0) {
		uint32 count = bytes < 0x3fffe0 ? bytes : 0x3fffe0;
		memory[n++] = 11 | (2u << 30);
		memory[n++] = (uint32)destination;
		memory[n++] = destination >> 32;
		memory[n++] = value;
		memory[n++] = count;
		destination += count;
		bytes -= count;
	}
	return Submit(n);
}

status_t
SdmaEngine::CopyRegions(const SdmaCopy* regions, uint32 count)
{
	if (area < 0 || faulted)
		return B_DEV_NOT_READY;
	// Five 64 MiB regions require at most 595 command words. Fence and
	// alignment padding still fit below the private fence at word 1024.
	if (regions == NULL || count == 0 || count > 5)
		return B_BAD_VALUE;
	for (uint32 i = 0; i < count; i++) {
		const SdmaCopy& c = regions[i];
		if (c.bytes == 0 || c.bytes > (64ULL << 20)
			|| ((c.source | c.destination | c.bytes) & 3) != 0
			|| c.bytes > UINT64_MAX - c.source || c.bytes > UINT64_MAX - c.destination)
			return B_BAD_VALUE;
	}
	uint32 n = 0;
	for (uint32 i = 0; i < count; i++) {
		SdmaCopy c = regions[i];
		while (c.bytes != 0) {
			uint32 bytes = c.bytes < 0x3fffe0 ? c.bytes : 0x3fffe0;
			memory[n++] = 1;
			memory[n++] = bytes;
			memory[n++] = 0;
			memory[n++] = (uint32)c.source;
			memory[n++] = c.source >> 32;
			memory[n++] = (uint32)c.destination;
			memory[n++] = c.destination >> 32;
			c.source += bytes;
			c.destination += bytes;
			c.bytes -= bytes;
		}
	}
	return Submit(n);
}

status_t
SdmaEngine::Submit(uint32 n)
{
	volatile uint32* r = regs;
	// These ring/control registers have fixed SDMA0 addresses. Unlike the
	// virtual-address registers set at initialization, they do not need
	// SRBM_GFX_CNTL selection (see Linux sdma_v3_0_gfx_resume/stop).
	// Changing that shared selector here races GFX's per-VM SH_MEM setup
	// when private video readback overlaps an initialized graphics client.
	if (++sequence == 0)
		sequence++;
	memory[0x1000 / 4] = 0;
	memory[n++] = 5;
	memory[n++] = (uint32)(gpu + 0x1000);
	memory[n++] = (gpu + 0x1000) >> 32;
	memory[n++] = sequence;
	while ((n & 7) != 0)
		memory[n++] = 0;
	r[0x3483] = r[0x3484] = 0;
	__sync_synchronize();
	(void)memory[n - 1];
	r[0x1520] = 1;
	(void)r[0x1520];
	r[0x3480] |= 1;
	r[0x3412] &= ~1u;
	r[0x3484] = n * 4;
	(void)r[0x3484];
	bigtime_t start = system_time();
	while (memory[0x1000 / 4] != sequence && system_time() - start < 500000)
		snooze(50);
	status_t status = memory[0x1000 / 4] == sequence ? B_OK : B_TIMED_OUT;
	start = system_time();
	while ((r[0x340d] & 1) == 0 && system_time() - start < 100000)
		snooze(50);
	r[0x3480] &= ~1u;
	r[0x3412] |= 1;
	(void)r[0x3412];
	if ((r[0x340d] & 1) == 0 || (r[0x3412] & 1) == 0)
		status = B_DEV_NOT_READY;
	if ((r[0x536] & 0xff) != 0) {
		dprintf("amdgpu: VMID0 fault %#x page %#x\n", (unsigned)r[0x536],
			(unsigned)r[0x53e]);
		status = B_BAD_ADDRESS;
	}
	__sync_synchronize(); // publish snooped system-memory writes to CPU readers
	if (status != B_OK) {
		faulted = true;
		dprintf("amdgpu: DMA fault status %#x fence %u/%u ring %u/%u engine %#x\n",
			(unsigned)status, (unsigned)memory[0x1000 / 4], (unsigned)sequence,
			(unsigned)r[0x3483], (unsigned)r[0x3484], (unsigned)r[0x340d]);
	}
	return status;
}

void
SdmaEngine::Uninitialize()
{
	if (area < 0)
		return;
	regs[0x3480] &= ~1u;
	regs[0x3412] |= 1;
	(void)regs[0x3412];
	// The published device retains ownership until shutdown. Reinitializing
	// a configured ring after unloading requires a cold boot, not guessing.
	delete_area(area);
	area = -1;
	memory = NULL;
}

