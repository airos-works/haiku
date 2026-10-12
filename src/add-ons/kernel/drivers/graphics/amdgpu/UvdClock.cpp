/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include "Firmware.h"
#include <SHA256.h>
#include <string.h>

bool
amdgpu::IsQualifiedUvdClockRom(const void* data, size_t size)
{
	// Until a general ATOM interpreter and clock/voltage policy are available,
	// admit only the ROM whose ComputeMemoryEnginePLL 1.6 table was executed
	// offline and whose startup/playback clock requests passed native tests.
	// Pin the entire ROM, including its clock and voltage data, not just its
	// version string or the divider command. Unknown ROMs retain client DMA
	// but cannot start UVD. See docs/x399-workstation/WX5100.md.
	if (data == NULL || size != 256 * 1024)
		return false;
	static const uint8_t expected[32] = {
		0xe4, 0x93, 0xc6, 0x78, 0x73, 0x3c, 0x32, 0x66,
		0xca, 0x2a, 0x7d, 0x4a, 0xdc, 0x82, 0xd3, 0x74,
		0x9e, 0xd2, 0xea, 0x59, 0xb5, 0x15, 0x3b, 0x00,
		0x73, 0x0c, 0x43, 0xe5, 0xf8, 0xf1, 0x28, 0x50
	};
	SHA256 digest;
	digest.Update(data, size);
	return memcmp(digest.Digest(), expected, sizeof(expected)) == 0;
}
