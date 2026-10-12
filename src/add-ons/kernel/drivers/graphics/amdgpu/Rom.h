/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#ifndef AMDGPU_ROM_H
#define AMDGPU_ROM_H

#include <KernelExport.h>
#include <PCI.h>

status_t amdgpu_read_rom(const pci_info& pci, volatile uint32* registers,
	void* data, uint32 size);

#endif
