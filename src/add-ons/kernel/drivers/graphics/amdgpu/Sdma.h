/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#ifndef AMDGPU_SDMA_H
#define AMDGPU_SDMA_H
#include <amdgpu_haiku.h>
#include <OS.h>
#include "Firmware.h"

status_t amdgpu_sdma_test(volatile uint32* regs, const amdgpu_info& info,
	const amdgpu::FirmwareView& firmware,
	const amdgpu::AtomVramReservation& reservation, amdgpu_sdma_test_result& result);

bool amdgpu_vram_range_is_safe(volatile uint32* regs, const amdgpu_info& info,
	const amdgpu::AtomVramReservation& reservation, uint64 offset, uint64 size);

struct SdmaCopy {
	uint64 source, destination, bytes;
};

struct SdmaEngine {
	volatile uint32* regs;
	volatile uint32* memory;
	area_id area;
	uint64 gpu;
	uint32 sequence;
	bool faulted;
	status_t Initialize(volatile uint32* r, const amdgpu_info& info,
		const amdgpu::FirmwareView& firmware,
		const amdgpu::AtomVramReservation& reservation);
	status_t Execute(uint32 operation, uint64 source, uint64 destination,
		uint64 bytes, uint32 value);
	status_t CopyRegions(const SdmaCopy* regions, uint32 count);
	status_t Recover(amdgpu_recovery_info& result);
	status_t TimeoutTest(amdgpu_sdma_timeout_test& result);
	void Stop();
	void Uninitialize();

private:
	void Configure();
	status_t Submit(uint32 words);
};
#endif
