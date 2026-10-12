/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include "Firmware.h"
#include <stdio.h>
#include <vector>

int
main(int argc, char** argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: amdgpu_uvd_clocks qualified-wx5100.rom\n");
		return 2;
	}
	FILE* file = fopen(argv[1], "rb");
	if (file == NULL)
		return 2;
	std::vector<uint8_t> rom(256 * 1024);
	bool read = fread(rom.data(), 1, rom.size(), file) == rom.size() && fgetc(file) == EOF;
	fclose(file);
	if (!read || !amdgpu::IsQualifiedUvdClockRom(rom.data(), rom.size())) {
		fprintf(stderr, "fixture is not the qualified complete ROM\n");
		return 1;
	}
	if (amdgpu::IsQualifiedUvdClockRom(NULL, rom.size())
		|| amdgpu::IsQualifiedUvdClockRom(rom.data(), 0)
		|| amdgpu::IsQualifiedUvdClockRom(rom.data(), rom.size() - 1)
		|| amdgpu::IsQualifiedUvdClockRom(rom.data(), rom.size() + 1))
		return 1;
	// Reject changes in the header, command directory, ATOM divider code and
	// unrelated ROM data: admission must cover more than a version/table ID.
	const size_t offsets[] = {0, 0x48, 0x22e, 0x9760, 0xd2d4, 0xd463, 0xd473,
		128 * 1024, 256 * 1024 - 1};
	for (size_t offset : offsets) {
		for (unsigned bit = 0; bit < 8; bit++) {
			rom[offset] ^= 1 << bit;
			bool accepted = amdgpu::IsQualifiedUvdClockRom(rom.data(), rom.size());
			rom[offset] ^= 1 << bit;
			if (accepted)
				return 1;
		}
	}
	if (!amdgpu::IsQualifiedUvdClockRom(rom.data(), rom.size()))
		return 1;
	puts("PASS: qualified ROM admitted; null/length errors and 72 ROM mutations rejected");
	return 0;
}
