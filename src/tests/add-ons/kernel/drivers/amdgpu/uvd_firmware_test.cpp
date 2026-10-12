/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include "Firmware.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vector>
int main(int argc, char** argv)
{
	assert(argc == 2);
	FILE* f = fopen(argv[1], "rb");
	assert(f && fseek(f, 0, SEEK_END) == 0);
	long n = ftell(f); rewind(f);
	assert(n >= 32 && n <= 1024 * 1024);
	std::vector<uint8_t> bytes(n);
	assert(fread(bytes.data(), 1, n, f) == (size_t)n); fclose(f);
	amdgpu::FirmwareView view;
	assert(amdgpu::ParseUvdFirmware(bytes.data(), n, view));
	printf("UVD version %#x, %u bytes\n", view.version, view.codeSize);
	for (size_t size = 0; size < bytes.size(); size++)
		assert(!amdgpu::ParseUvdFirmware(bytes.data(), size, view));
	for (size_t field : {size_t(0), size_t(4), size_t(8), size_t(12), size_t(20), size_t(24)}) {
		auto bad = bytes; memset(bad.data() + field, 0xff, 4);
		assert(!amdgpu::ParseUvdFirmware(bad.data(), bad.size(), view));
	}
	std::vector<uint8_t> unaligned(n + 1);
	memcpy(unaligned.data() + 1, bytes.data(), n);
	assert(amdgpu::ParseUvdFirmware(unaligned.data() + 1, n, view));
	assert(!amdgpu::ParseUvdFirmware(NULL, n, view));
	assert(!amdgpu::ParseSdmaFirmware(bytes.data(), n, view));
	assert(!amdgpu::ParseGfxFirmware(bytes.data(), n, false, view));
	puts("PASS: UVD6.3 container bounds, truncation, alignment and wrong IP");
}
