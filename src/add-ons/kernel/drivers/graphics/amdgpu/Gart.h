/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#ifndef AMDGPU_GART_H
#define AMDGPU_GART_H
#include "Sdma.h"

// VMID0 is private to kernel-built DMA packets. This is not a userspace VM.
// The first 64 MiB of VRAM is reserved by Device.cpp. The GART occupies
// 16..18 MiB; SDMA/SMU occupy 32..35 MiB. GPU GART addresses do not overlap
// the physical VRAM aperture. Client RAM PTEs are snooped, read/write, non-executable. Private command
// pages additionally allow instruction fetch and are never mapped to users.
struct Gart {
	static const uint64 kSize = 1ULL << 30;
	static const uint64 kBase = 0x10000000;
	static const uint32 kCommandBytes = 131072;
	volatile uint32* regs;
	volatile uint64* table;
	area_id tableArea, dummyArea, commandArea;
	volatile uint32* commandMemory;
	bool enabled;
	uint64 boundPages, scatterBoundaries;
	status_t Initialize(volatile uint32* r, const amdgpu_info& info,
		const amdgpu::AtomVramReservation& reservation);
	status_t Bind(uint64 offset, uint64 bytes, const void* cpu, bool executable = false);
	status_t Unbind(uint64 offset, uint64 bytes);
	status_t Flush();
	void Uninitialize(bool faulted);
};
#endif
