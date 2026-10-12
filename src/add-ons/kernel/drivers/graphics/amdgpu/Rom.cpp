/*
 * Copyright 2015 Advanced Micro Devices, Inc.
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

#include "Rom.h"
#include "Firmware.h"
#include <ACPI.h>
#include <string.h>

using namespace amdgpu;

static status_t
ReadVfct(const pci_info& pci, void* data, uint32 capacity)
{
	acpi_module_info* acpi;
	status_t status = get_module(B_ACPI_MODULE_NAME, (module_info**)&acpi);
	if (status != B_OK)
		return status;
	const uint8* table = NULL;
	status = acpi->get_table("VFCT", 0, (void**)&table);
	if (status == B_OK) {
		uint32 size = ReadLE32(table + 4);
		status = B_BAD_DATA;
		if (size >= 76) {
			uint32 offset = ReadLE32(table + 52);
			while (offset >= 76 && offset <= size && size - offset >= 28) {
				const uint8* entry = table + offset;
				uint32 length = ReadLE32(entry + 24);
				offset += 28;
				if (length == 0 || length > size - offset)
					break;
				if (ReadLE32(entry) == pci.bus
					&& ReadLE32(entry + 4) == pci.device
					&& ReadLE32(entry + 8) == pci.function
					&& ReadLE16(entry + 12) == pci.vendor_id
					&& ReadLE16(entry + 14) == pci.device_id) {
					if (length <= capacity) {
						memset(data, 0, capacity);
						memcpy(data, table + offset, length);
						status = B_OK;
					}
					break;
				}
				offset += length;
			}
		}
	}
	put_module(B_ACPI_MODULE_NAME);
	return status;
}


status_t
amdgpu_read_rom(const pci_info& pci, volatile uint32* regs, void* data, uint32 size)
{
	if (ReadVfct(pci, data, size) == B_OK) {
		dprintf("amdgpu: ROM read from ACPI VFCT\n");
		return B_OK;
	}
	// vi_read_bios_from_rom(), Linux AMDGPU: the ROM stream uses SMC index
	// pair 11. No clocks, reset, power, scanout, or ROM-decode state changes.
	// The caller serializes this with all other GPU access. Restore both
	// indices, including on an invalid ROM, so observation has no residue.
	uint32 oldSmcIndex = regs[0x1ac];
	regs[0x1ac] = 0xc0600010; // ROM_INDEX
	uint32 oldRomIndex = regs[0x1ad];
	regs[0x1ad] = 0;
	regs[0x1ac] = 0xc0600014; // ROM_DATA, auto increment
	uint32* words = (uint32*)data;
	for (uint32 i = 0; i < size / 4; i++)
		words[i] = regs[0x1ad];
	regs[0x1ac] = 0xc0600010;
	regs[0x1ad] = oldRomIndex;
	regs[0x1ac] = oldSmcIndex;
	(void)regs[0x1ac];
	dprintf("amdgpu: ROM read through SMC index 11\n");
	return B_OK;
}
