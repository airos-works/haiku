/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include "Firmware.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vector>

int main(int argc, char** argv)
{
	assert(argc > 1);
	for (int fileIndex = 1; fileIndex < argc; fileIndex++) {
		FILE* file = fopen(argv[fileIndex], "rb");
		assert(file != NULL && fseek(file, 0, SEEK_END) == 0);
		long size = ftell(file);
		assert(size >= 44 && size <= 320 * 1024);
		rewind(file);
		std::vector<uint8_t> bytes(size);
		assert(fread(bytes.data(), 1, size, file) == (size_t)size);
		fclose(file);
		amdgpu::MecFirmwareView view;
		assert(amdgpu::ParseMecFirmware(bytes.data(), bytes.size(), view));
		assert(view.program.code == bytes.data() + amdgpu::ReadLE32(bytes.data() + 24));
		assert(view.program.codeSize == amdgpu::ReadLE32(bytes.data() + 36) * 4);
		assert(view.jumpTable == view.program.code + view.program.codeSize);
		assert(view.jumpTable + view.jumpTableSize == bytes.data() + bytes.size());
		printf("%s version %u feature %u: signed program %u, signed JT %u bytes\n",
			argv[fileIndex], view.program.version, view.program.featureVersion,
			view.program.codeSize, view.jumpTableSize);
		for (size_t n = 0; n < bytes.size(); n++)
			assert(!amdgpu::ParseMecFirmware(bytes.data(), n, view));
		std::vector<uint8_t> unaligned(bytes.size() + 1);
		memcpy(unaligned.data() + 1, bytes.data(), bytes.size());
		assert(amdgpu::ParseMecFirmware(unaligned.data() + 1, size, view));
		for (size_t field : {size_t(0), size_t(4), size_t(8), size_t(12),
				size_t(20), size_t(24), size_t(36), size_t(40)}) {
			for (uint8_t fill : {uint8_t(0), uint8_t(0xff)}) {
				auto bad = bytes;
				memset(bad.data() + field, fill, 4);
				assert(!amdgpu::ParseMecFirmware(bad.data(), bad.size(), view));
			}
		}
		amdgpu::FirmwareView other;
		assert(!amdgpu::ParseGfxFirmware(bytes.data(), size, false, other));
		assert(!amdgpu::ParseGfxFirmware(bytes.data(), size, true, other));
		assert(!amdgpu::ParseSdmaFirmware(bytes.data(), size, other));
		// Mutate the header at every byte position. Any accepted container
		// must still expose two complete, nonoverlapping, bounded extents.
		auto bad = bytes;
		uint32_t random = 0x78561423;
		for (unsigned trial = 0; trial < 25000; trial++) {
			random = random * 1664525u + 1013904223u;
			unsigned position = random % 44;
			uint8_t saved = bad[position];
			bad[position] ^= uint8_t((random >> 16) | 1);
			if (amdgpu::ParseMecFirmware(bad.data(), bad.size(), view)) {
				assert(view.program.code >= bad.data() + 44);
				assert(view.program.code + view.program.codeSize == view.jumpTable);
				assert(view.jumpTable + view.jumpTableSize == bad.data() + bad.size());
			}
			bad[position] = saved;
		}
	}
	puts("PASS: MEC signed extents, truncation, alignment, malformed headers and mutations");
}
