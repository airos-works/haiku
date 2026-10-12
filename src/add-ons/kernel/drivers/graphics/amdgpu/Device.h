/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#ifndef AMDGPU_DEVICE_H
#define AMDGPU_DEVICE_H
#include "Firmware.h"
#include <amdgpu_haiku.h>
#include <PCI.h>

struct AmdgpuClient;
AmdgpuClient* amdgpu_client_open(uint32 flags);
void amdgpu_client_free(AmdgpuClient* client);
bool amdgpu_device_active();
status_t amdgpu_device_start(volatile uint32* regs, const amdgpu_info& info,
	const amdgpu::FirmwareView& firmware, const amdgpu::AtomVramReservation& reservation,
	bool uvdClocksQualified, pci_module_info* pci, const amdgpu::AtomRenderInfo& renderInfo);
status_t amdgpu_client_control(AmdgpuClient* client, uint32 op, void* data, size_t length,
	status_t (*initialize)());
status_t amdgpu_device_uvd_test(const amdgpu::FirmwareView& firmware,
	amdgpu_uvd_test& result, void* output);
status_t amdgpu_device_gfx_test(const amdgpu::FirmwareView firmware[4],
	amdgpu_gfx_test& result, const amdgpu::MecFirmwareView* mec = NULL);
void amdgpu_device_stop();
#endif
