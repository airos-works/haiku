/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */

#include <cmath>
#include <cstdio>
#include <cstring>

#include "ds18b20.h"

static int sChecks;
static int sFailures;

#define CHECK(condition) \
	do { \
		sChecks++; \
		if (!(condition)) { \
			sFailures++; \
			std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, \
				__LINE__, #condition); \
		} \
	} while (false)

#define CHECK_NEAR(actual, expected) \
	do { \
		double value = (actual); \
		sChecks++; \
		if (std::fabs(value - (expected)) > 0.0000001) { \
			sFailures++; \
			std::fprintf(stderr, "%s:%d: %.10f != %.10f\n", __FILE__, __LINE__, \
				value, (double)(expected)); \
		} \
	} while (false)


int
main()
{
	const uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
	CHECK(ds18b20_crc8(check, sizeof(check)) == 0xa1);
	CHECK(ds18b20_crc8(check, 0) == 0);

	uint8_t rom[8] = {0x28, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0};
	rom[7] = ds18b20_crc8(rom, 7);
	CHECK(ds18b20_rom_is_valid(rom));
	CHECK(ds18b20_crc8(rom, 8) == 0);
	rom[2] ^= 0x80;
	CHECK(!ds18b20_rom_is_valid(rom));
	rom[2] ^= 0x80;
	rom[0] = 0;
	CHECK(!ds18b20_rom_is_valid(rom));
	rom[0] = 0x28;
	rom[7]++;
	CHECK(!ds18b20_rom_is_valid(rom));

	uint8_t scratchpad[9] = {0x90, 0x01, 0x50, 0x7f, 0xff, 0x0c, 0x10, 0x00, 0};
	scratchpad[8] = ds18b20_crc8(scratchpad, 8);
	CHECK(ds18b20_scratchpad_is_valid(scratchpad));
	CHECK(ds18b20_crc8(scratchpad, 9) == 0);
	CHECK(ds18b20_raw_temperature(scratchpad) == 400);
	CHECK_NEAR(ds18b20_celsius(ds18b20_raw_temperature(scratchpad)), 25);
	scratchpad[0] ^= 1;
	CHECK(!ds18b20_scratchpad_is_valid(scratchpad));
	scratchpad[0] ^= 1;
	scratchpad[8]++;
	CHECK(!ds18b20_scratchpad_is_valid(scratchpad));

	CHECK_NEAR(ds18b20_celsius(0x0190), 25);
	CHECK_NEAR(ds18b20_celsius(0x0550), 85);
	CHECK_NEAR(ds18b20_celsius(0xfc90), -55);
	CHECK_NEAR(ds18b20_celsius(0xff5e), -10.125);
	CHECK_NEAR(ds18b20_celsius(0x0191), 25.0625);
	CHECK_NEAR(ds18b20_celsius(0), 0);

	std::printf("%d checks, %d failed\n", sChecks, sFailures);
	return sFailures == 0 ? 0 : 1;
}
