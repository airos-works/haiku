/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#ifndef AMDGPU_UVD_H
#define AMDGPU_UVD_H
#include "Sdma.h"
#include "UvdH264.h"
#include "UvdHevc.h"
struct UvdSession {
	UvdSession* nextQuarantined;
	enum { kGuardReadbackBytes = 4 * 4096 };
	union {
		amdgpu_h264_config config;
		amdgpu_hevc_config hevcConfig;
	};
	amdgpu::UvdVideoLayout layout;
	bool hevc;
	uint32 hevcReferenceSlots;
	volatile uint32* cpu;
	uint64 offset, gpu;
	area_id area;
	// Kernel-only cached RAM, mapped through snooped GART PTEs for SDMA
	// readback. Never cloned into userspace; quarantined on engine failure.
	void* readback;
	area_id readbackArea;
	uint64 readbackOffset, readbackBytes, readbackGpu;
	bool readbackBound;
	uint32 handle, frames;
	bigtime_t timingTotal[5];
	bool created;
};
struct UvdEngine {
	volatile uint32* regs;
	volatile uint32* memory;
	area_id area;
	uint64 gpu;
	uint32 wptr, sequence;
	bool attempted, ready, faulted;
	status_t Initialize(volatile uint32* r, const amdgpu_info& info,
		const amdgpu::AtomVramReservation& reservation, bool clocksQualified,
		const amdgpu::FirmwareView& firmware, amdgpu_uvd_test& result);
	status_t Session(UvdSession& session, uint32 type,
		const void* picture, const void* bitstream, uint32 bytes,
		SdmaEngine& dma, amdgpu_uvd_test& result);
	status_t Test(volatile uint32* r, const amdgpu_info& info,
		const amdgpu::AtomVramReservation& reservation, bool clocksQualified,
		const amdgpu::FirmwareView& firmware, amdgpu_uvd_test& result, void* output);
	status_t Start(const amdgpu::FirmwareView& firmware);
	status_t Submit(uint32 words, amdgpu_uvd_test& result);
	void Snapshot(amdgpu_uvd_test& result);
	void Stop();
	void Uninitialize();
};
#endif
