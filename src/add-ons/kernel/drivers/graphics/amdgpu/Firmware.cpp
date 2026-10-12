/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */

#include "Firmware.h"
#include <string.h>

// Layouts: AMD amdgpu_ucode.h and atombios.h. Read bytes explicitly so that
// unaligned firmware, including an adversarial input, has no undefined access.
namespace amdgpu {

bool
ParseSmcFirmware(const void* data, size_t size, FirmwareView& view)
{
	view = {};
	if (data == NULL || size < 36 || size > 0x20100)
		return false;
	const uint8_t* p = (const uint8_t*)data;
	uint32_t headerSize = ReadLE32(p + 4);
	uint32_t codeSize = ReadLE32(p + 20);
	uint32_t offset = ReadLE32(p + 24);
	if (ReadLE32(p) != size || ReadLE16(p + 8) != 1
		|| ReadLE16(p + 10) != 0 || ReadLE16(p + 12) != 7
		|| ReadLE16(p + 14) != 2 || headerSize < 36 || headerSize > size
		|| offset < headerSize || offset > size || (offset & 3) != 0
		|| codeSize < 256 || codeSize > 0x20000 || (codeSize & 3) != 0
		|| codeSize > size - offset || ReadLE32(p + 32) != 0x20000)
		return false;
	view.code = p + offset;
	view.codeSize = codeSize;
	view.version = ReadLE32(p + 16);
	return true;
}


bool
ParseSdmaFirmware(const void* data, size_t size, FirmwareView& view)
{
	view = {};
	if (data == NULL || size < 52 || size > 64 * 1024)
		return false;
	const uint8_t* p = (const uint8_t*)data;
	uint32_t headerSize = ReadLE32(p + 4);
	uint32_t codeSize = ReadLE32(p + 20);
	uint32_t offset = ReadLE32(p + 24);
	// Polaris10 uses SDMA 3.1, signed header 1.1. Do not feed another IP's
	// program, or an unknown container revision, to its micro-engine.
	if (ReadLE32(p) != size || ReadLE16(p + 8) != 1
		|| ReadLE16(p + 10) != 1 || ReadLE16(p + 12) != 3
		|| ReadLE16(p + 14) != 1 || headerSize < 52 || headerSize > size
		|| offset < headerSize || offset > size || (offset & 3) != 0
		|| codeSize == 0 || (codeSize & 3) != 0 || codeSize > size - offset)
		return false;
	uint32_t jtOffset = ReadLE32(p + 40);
	uint32_t jtSize = ReadLE32(p + 44);
	if (jtOffset > codeSize / 4 || jtSize > codeSize / 4 - jtOffset)
		return false;
	view.code = p + offset;
	view.codeSize = codeSize;
	view.version = ReadLE32(p + 16);
	view.featureVersion = ReadLE32(p + 32);
	return true;
}


bool
ParseGfxFirmware(const void* data, size_t size, bool rlc, FirmwareView& view)
{
	view = {};
	uint32_t minimum = rlc ? 104 : 44;
	if (data == NULL || size < minimum || size > 65536)
		return false;
	const uint8_t* p = (const uint8_t*)data;
	uint32_t headerSize = ReadLE32(p + 4), codeSize = ReadLE32(p + 20);
	uint32_t offset = ReadLE32(p + 24);
	if (ReadLE32(p) != size || ReadLE16(p + 8) != (rlc ? 2 : 1)
		|| ReadLE16(p + 10) != 0 || ReadLE16(p + 12) != 8
		|| ReadLE16(p + 14) != 0 || headerSize < minimum || headerSize > size
		|| offset < headerSize || offset > size || (offset & 3) != 0
		|| codeSize < 24 || (codeSize & 3) != 0 || codeSize > size - offset)
		return false;
	uint32_t jtOffset = ReadLE32(p + 36), jtSize = ReadLE32(p + 40);
	if (jtOffset > codeSize / 4 || jtSize > codeSize / 4 - jtOffset)
		return false;
	// RLC carries restoration arrays outside its signed microcode. Validate
	// their file ranges even though Polaris10's initial ring path skips PG.
	if (rlc) {
		for (uint32_t field = 72; field <= 96; field += 8) {
			uint32_t bytes = ReadLE32(p + field), start = ReadLE32(p + field + 4);
			if ((bytes & 3) != 0 || (start & 3) != 0 || start > size
				|| bytes > size - start || (bytes != 0 && start < headerSize))
				return false;
		}
	}
	view.code = p + offset;
	view.codeSize = codeSize;
	view.version = ReadLE32(p + 16);
	view.featureVersion = ReadLE32(p + 32);
	return true;
}


bool
ParseMecFirmware(const void* data, size_t size, MecFirmwareView& view)
{
	view = {};
	if (data == NULL || size < 44 || size > 320 * 1024)
		return false;
	const uint8_t* p = (const uint8_t*)data;
	uint32_t headerSize = ReadLE32(p + 4), codeSize = ReadLE32(p + 20);
	uint32_t offset = ReadLE32(p + 24);
	uint32_t jtOffset = ReadLE32(p + 36), jtSize = ReadLE32(p + 40);
	if (ReadLE32(p) != size || ReadLE16(p + 8) != 1
		|| ReadLE16(p + 10) != 0 || ReadLE16(p + 12) != 8
		|| ReadLE16(p + 14) != 0 || headerSize < 44 || headerSize > size
		|| offset < headerSize || offset > size || (offset & 3) != 0
		|| (codeSize & 3) != 0 || codeSize != size - offset
		|| jtOffset <= 65536 / 4 || jtOffset > (256 * 1024 + 20) / 4
		|| jtOffset > codeSize / 4 || jtSize < 6 || jtSize > 4096 / 4
		|| jtSize != codeSize / 4 - jtOffset)
		return false;
	// Linux's SMU loader authenticates these separately. The program and
	// jump-table extents each include their own 20-byte digest on this PF.
	view.program.code = p + offset;
	view.program.codeSize = jtOffset * 4;
	view.program.version = ReadLE32(p + 16);
	view.program.featureVersion = ReadLE32(p + 32);
	view.jumpTable = p + offset + jtOffset * 4;
	view.jumpTableSize = jtSize * 4;
	return true;
}


bool
ParseUvdFirmware(const void* data, size_t size, FirmwareView& view)
{
	view = {};
	if (data == NULL || size < 32 || size > 1024 * 1024)
		return false;
	const uint8_t* p = (const uint8_t*)data;
	uint32_t header = ReadLE32(p + 4), bytes = ReadLE32(p + 20);
	uint32_t offset = ReadLE32(p + 24);
	// Polaris10's UVD 6.3 image uses the common 1.0 container. Preserve the
	// complete code, including the VCPU's embedded authentication material.
	if (ReadLE32(p) != size || ReadLE16(p + 8) != 1 || ReadLE16(p + 10) != 0
		|| ReadLE16(p + 12) != 6 || ReadLE16(p + 14) != 3
		|| header < 32 || header > size || offset < header || offset > size
		|| (offset & 3) != 0 || bytes < 256 || (bytes & 3) != 0
		|| bytes > size - offset)
		return false;
	view.code = p + offset;
	view.codeSize = bytes;
	view.version = ReadLE32(p + 16);
	return true;
}


static bool
TableFits(const uint8_t* p, size_t size, size_t offset, size_t minimum)
{
	return offset >= 0x4a && offset <= size && minimum <= size - offset
		&& ReadLE16(p + offset) >= minimum
		&& ReadLE16(p + offset) <= size - offset;
}


bool
ParseAtomVramReservation(const void* data, size_t size,
	AtomVramReservation& reservation)
{
	reservation = {};
	if (data == NULL || size < 0x4a)
		return false;
	const uint8_t* p = (const uint8_t*)data;
	if (p[0] != 0x55 || p[1] != 0xaa)
		return false;
	size_t rom = ReadLE16(p + 0x48);
	if (!TableFits(p, size, rom, 36)
		|| (memcmp(p + rom + 4, "ATOM", 4) != 0
			&& memcmp(p + rom + 4, "MOTA", 4) != 0))
		return false;
	size_t master = ReadLE16(p + rom + 32);
	// VRAM_UsageByFirmware is entry 11 in the data table.
	if (!TableFits(p, size, master, 4 + 12 * 2))
		return false;
	size_t usage = ReadLE16(p + master + 4 + 11 * 2);
	if (!TableFits(p, size, usage, 12) || p[usage + 2] != 1
		|| (p[usage + 3] != 4 && p[usage + 3] != 5))
		return false;
	uint32_t start = ReadLE32(p + usage + 4);
	// The high two bits are operation flags, not part of the KiB offset.
	reservation.start = (uint64_t)(start & 0x3fffffff) << 10;
	reservation.size = (uint64_t)ReadLE16(p + usage + 8) << 10;
	reservation.driverScratchSize = (uint32_t)ReadLE16(p + usage + 10) << 10;
	return true;
}

bool
ParseAtomRenderInfo(const void* data, size_t size, AtomRenderInfo& info)
{
	info = {};
	if (data == NULL || size < 0x4a)
		return false;
	const uint8_t* p = (const uint8_t*)data;
	if (p[0] != 0x55 || p[1] != 0xaa)
		return false;
	size_t rom = ReadLE16(p + 0x48);
	if (!TableFits(p, size, rom, 36)
		|| (memcmp(p + rom + 4, "ATOM", 4) != 0
			&& memcmp(p + rom + 4, "MOTA", 4) != 0))
		return false;
	size_t master = ReadLE16(p + rom + 32);
	if (!TableFits(p, size, master, 4 + 15 * 2))
		return false;
	// ATOM data entries 14 (GFX_Info) and 4 (FirmwareInfo). GFX 2.1 and
	// 2.3 share these first twelve bytes; only 2.2 FirmwareInfo is supported.
	size_t gfx = ReadLE16(p + master + 4 + 14 * 2);
	size_t firmware = ReadLE16(p + master + 4 + 4 * 2);
	if (!TableFits(p, size, gfx, 12) || p[gfx + 2] != 2
		|| (p[gfx + 3] != 1 && p[gfx + 3] != 3)
		|| (p[gfx + 3] == 3 && !TableFits(p, size, gfx, 24))
		|| !TableFits(p, size, firmware, 108)
		|| p[firmware + 2] != 2 || p[firmware + 3] != 2)
		return false;
	AtomRenderInfo value = {};
	value.gfxMajor = p[gfx + 5];
	value.gfxMinor = p[gfx + 4];
	value.shaderEngines = p[gfx + 6];
	value.tilePipes = p[gfx + 7];
	value.cuPerArray = p[gfx + 8];
	value.shaderArraysPerEngine = p[gfx + 9];
	value.backendsPerEngine = p[gfx + 10];
	value.tccBlocks = p[gfx + 11];
	uint32_t engine = ReadLE32(p + firmware + 8);
	uint32_t memory = ReadLE32(p + firmware + 12);
	uint32_t reference = ReadLE16(p + firmware + 82);
	// These bounds protect all later bank selection, shifts and output arrays.
	if (value.gfxMajor != 8 || value.gfxMinor > 1
		|| value.shaderEngines == 0 || value.shaderEngines > 4
		|| value.shaderArraysPerEngine == 0 || value.shaderArraysPerEngine > 2
		|| value.cuPerArray == 0 || value.cuPerArray > 16
		|| value.backendsPerEngine == 0 || value.backendsPerEngine > 8
		|| value.backendsPerEngine % value.shaderArraysPerEngine != 0
		|| value.tilePipes == 0 || value.tilePipes > 16
		|| value.tccBlocks == 0 || value.tccBlocks > 16
		|| engine == 0 || engine > 1000000 || memory == 0 || memory > 1000000
		|| reference == 0 || reference > 100000)
		return false;
	value.defaultEngineKHz = engine * 10;
	value.defaultMemoryKHz = memory * 10;
	value.referenceKHz = reference * 10;
	info = value;
	return true;
}

} // namespace amdgpu
