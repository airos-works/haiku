/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#ifndef AMDGPU_SMU_H
#define AMDGPU_SMU_H
#include <amdgpu_haiku.h>
#include "Firmware.h"

status_t amdgpu_smc_bootstrap_firmware(volatile uint32* regs,
	const amdgpu::FirmwareView& firmware, amdgpu_smc_bootstrap& result);
// workspace is a permanently reserved 2 MiB VRAM region: 1 MiB SMU scratch,
// followed by TOC and firmware staging. Retain it until a cold boot.
status_t amdgpu_smc_load_sdma(volatile uint32* regs,
	const amdgpu::FirmwareView& firmware, volatile uint32* workspace, uint64 gpu);
status_t amdgpu_smc_load_gfx(volatile uint32* regs,
	const amdgpu::FirmwareView firmware[4], volatile uint32* workspace, uint64 gpu,
	const amdgpu::MecFirmwareView* mec);
void amdgpu_smc_dump_uvd_clocks(volatile uint32* regs);
status_t amdgpu_smc_set_uvd_clocks(volatile uint32* regs);
// UVD soft reset preserves the owned playback clocks. Verify, never reapply
// the cold-start divider policy to an already initialized clock domain.
status_t amdgpu_smc_check_uvd_clocks(volatile uint32* regs);
bool amdgpu_smc_ready(volatile uint32* regs);
// Caller serializes with all other SMC operations; no clock is changed.
uint32 amdgpu_smc_timestamp_khz(volatile uint32* regs, uint32 referenceKHz);
#endif
