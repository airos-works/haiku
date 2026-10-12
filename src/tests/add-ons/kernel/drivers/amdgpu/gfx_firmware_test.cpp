/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include "Firmware.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vector>

int main(int argc, char** argv)
{
	assert(argc == 5);
	for (int i = 1; i <= 4; i++) {
		FILE* file = fopen(argv[i], "rb");
		assert(file != NULL && fseek(file, 0, SEEK_END) == 0);
		long size = ftell(file);
		assert(size >= 44 && size <= 65536);
		rewind(file);
		std::vector<uint8_t> bytes(size);
		assert(fread(bytes.data(), 1, size, file) == (size_t)size);
		fclose(file);
		bool rlc = i == 4;
		amdgpu::FirmwareView view;
		assert(amdgpu::ParseGfxFirmware(bytes.data(), bytes.size(), rlc, view));
		printf("%s version %u, feature %u, %u bytes\n", argv[i], view.version,
			view.featureVersion, view.codeSize);
		for (size_t n = 0; n < bytes.size(); n++)
			assert(!amdgpu::ParseGfxFirmware(bytes.data(), n, rlc, view));
		std::vector<uint8_t> unaligned(bytes.size() + 1);
		memcpy(unaligned.data() + 1, bytes.data(), bytes.size());
		assert(amdgpu::ParseGfxFirmware(unaligned.data() + 1, size, rlc, view));
		for (size_t field : {size_t(0), size_t(4), size_t(8), size_t(12),
				size_t(20), size_t(24), size_t(36), size_t(40)}) {
			auto bad = bytes;
			memset(bad.data() + field, 0xff, 4);
			assert(!amdgpu::ParseGfxFirmware(bad.data(), bad.size(), rlc, view));
		}
		if (rlc) {
			for (size_t field = 72; field < 104; field += 4) {
				auto bad = bytes;
				memset(bad.data() + field, 0xff, 4);
				assert(!amdgpu::ParseGfxFirmware(bad.data(), bad.size(), true, view));
			}
		}
		assert(!amdgpu::ParseGfxFirmware(bytes.data(), bytes.size(), !rlc, view));
		assert(!amdgpu::ParseSdmaFirmware(bytes.data(), bytes.size(), view));
	}
	puts("PASS: GFX8 CP/RLC containers, truncation, alignment, arrays and wrong IP");
}
