/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#include "CsfPropertyCapture.h"
#include <assert.h>
#include <map>
#include <stdio.h>
#include <string.h>

using namespace MaliCSF;

// Independent register inventory from Linux 6.18.52 panthor_regs.h. Values
// are synthetic distinct signatures, including high halves and zero features.
// There is deliberately no write or allocation API on this register fixture.
struct Registers {
	std::map<uint32_t, uint32_t> values{
		{0x000, 0xa8670005}, {0x004, 0x07120306}, {0x008, 0x739b2610},
		{0x00c, 0x34581972}, {0x010, 0x0301}, {0x014, 0x2830},
		{0x018, 0xff}, {0x01c, 0x040a0412},
		{0x060, 0x92d510cf}, {0x064, 0xf18b53da},
		{0x0a0, 0x5a6102c4}, {0x0a4, 0x49380cb2}, {0x0a8, 0xf8321da0},
		{0x0ac, 0x83c0d159}, {0x0b0, 0}, {0x0b4, 0x81243fac},
		{0x0b8, 0xb65409cf}, {0x0bc, 0xfffffffe},
		{0x100, 0x50005}, {0x104, 0x831927dc}, {0x110, 1}, {0x114, 0x732853fe},
		{0x120, 0x416732fa}, {0x124, 0xca61420f},
		{0x280, 0x93274ace}, {0x300, 0x80000003}
	};
	std::map<uint32_t, unsigned> reads;
	uint32_t ReadGpu(uint32_t offset) { reads[offset]++; return values.at(offset); }
};

int main()
{
	Registers io;
	InterfaceInfo interface{0x01050000, 0x8372561e, 0xdeadbeef, 8, 0x400, 8, 12,
		0x1832085f, 96, 8, 0x54320000, 0x61430000};
	QueueInterfaceInfo queue{0x3874516e, 0x38000, 0x10000, 1, 2, 3, 4, 0};
	const GpuProperties expected{
		0xa8670005, 0x93274ace, 0x040a0412, 0x739b2610,
		0x07120306, 0x34581972, 0x0301, 0x2830,
		0x83c0d159, 0x5a6102c4, 0x49380cb2, 0xf8321da0,
		0x80000003, {0, 0x81243fac, 0xb65409cf, 0xfffffffe}, 0xff,
		UINT64_C(0x831927dc00050005), UINT64_C(0x732853fe00000001),
		UINT64_C(0xca61420f416732fa), UINT64_C(0xf18b53da92d510cf),
		0x01050000, 0x8372561e, 0x3874516e, 0x1832085f, 8, 8, 96, 8,
		4, 24000000, kMaxRuntimeQueues, 64, UINT64_C(0x800000000000)
	};
	auto result = CaptureGpuProperties(io, interface, queue, 24000000);
	assert(memcmp(&result, &expected, sizeof(expected)) == 0);
	assert(io.reads.size() == 26);
	for (auto& row : io.reads) assert(row.second == 1);
	// Zero is a real hardware feature value; do not substitute guessed caps.
	for (auto& row : io.values) row.second = 0;
	result = CaptureGpuProperties(io, interface, queue, 0);
	GpuProperties zero{};
	assert(memcmp(&result, &zero, 104) == 0 && result.firmwareTimerHz == 0);
	assert(result.firmwareVersion == expected.firmwareVersion && result.userVaLimit == expected.userVaLimit);
	puts("MALI_CSF_PROPERTIES_TEST_PASS registers=26 high_halves=4 reserved_registers=4");
}
