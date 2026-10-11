/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#ifndef AMDGPU_GFX_H
#define AMDGPU_GFX_H
#include "Gart.h"
#include "Interrupts.h"
struct GfxEngine {
	static const uint64 kScratchOffset = 40ULL << 20;
	static const uint64 kClientIbVA = 0x1000;
	static const uint64 kClientShaderVA = 0x2000;
	volatile uint32* regs;
	volatile uint32* memory;
	volatile uint32* ring;
	area_id area, vmArea;
	uint64 gpu;
	uint32 wptr, sequence, vmSequence;
	bool attempted, ready, faulted, vmEnabled, mecStarted;
	pci_module_info* pci;
	GpuInterrupts interrupts;
	uint32 firmwareVersion[5], firmwareFeature[5];
	status_t InitializeVM(const amdgpu_info& info,
		const amdgpu::AtomVramReservation& reservation, const Gart& gart);
	status_t Initialize(volatile uint32* r, const amdgpu_info& info,
		const amdgpu::AtomVramReservation& reservation,
		const amdgpu::FirmwareView firmware[4], SdmaEngine& sdma, Gart& gart,
		amdgpu_gfx_test& result, const amdgpu::MecFirmwareView* mec);
	status_t Test(volatile uint32* r, const amdgpu_info& info,
		const amdgpu::AtomVramReservation& reservation,
		const amdgpu::FirmwareView firmware[4], SdmaEngine& sdma, Gart& gart,
		amdgpu_gfx_test& result, const amdgpu::MecFirmwareView* mec);
	status_t ExecuteVM(uint64 directory, uint64 destination, uint32 value,
		volatile uint32* commands, Gart& gart, amdgpu_vm_test& result,
		uint64 ibAddress = kClientIbVA, uint64 shaderAddress = kClientShaderVA);
	status_t ExecuteIB(uint64 directory, uint64 address, uint32 dwords,
		Gart& gart, amdgpu_vm_test& result, uint32 diagnosticGds = 0);
	void Snapshot(amdgpu_gfx_test& result);
	status_t RenderInfo(const amdgpu::AtomRenderInfo& rom, amdgpu_render_info& result);
	status_t ShaderMemoryInfo(amdgpu_shader_memory_info& result);
	status_t GwsInfo(amdgpu_gws_info& result);
	status_t OaInfo(amdgpu_oa_info& result);
	status_t Recover(const amdgpu_info& info,
		const amdgpu::AtomVramReservation& reservation,
		const amdgpu::AtomRenderInfo& topology,
		const amdgpu::FirmwareView firmware[4], const amdgpu::MecFirmwareView& mec,
		SdmaEngine& sdma, Gart& gart, amdgpu_recovery_info& result);
	void DumpExecutionState(const char* point);
	void Halt();
	void Uninitialize();
};
#endif
