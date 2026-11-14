/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _DS18B20_H
#define _DS18B20_H


#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


static inline uint8_t
ds18b20_crc8(const uint8_t* data, size_t length)
{
	uint8_t crc = 0;
	for (size_t i = 0; i < length; i++) {
		uint8_t value = data[i];
		for (uint8_t bit = 0; bit < 8; bit++) {
			uint8_t mix = (crc ^ value) & 1;
			crc >>= 1;
			if (mix != 0)
				crc ^= 0x8c;
			value >>= 1;
		}
	}
	return crc;
}


static inline bool
ds18b20_rom_is_valid(const uint8_t rom[8])
{
	return rom[0] == 0x28 && ds18b20_crc8(rom, 7) == rom[7];
}


static inline bool
ds18b20_scratchpad_is_valid(const uint8_t data[9])
{
	return ds18b20_crc8(data, 8) == data[8];
}


static inline int16_t
ds18b20_raw_temperature(const uint8_t data[9])
{
	return (int16_t)(data[0] | ((uint16_t)data[1] << 8));
}


static inline double
ds18b20_celsius(int16_t raw)
{
	return raw / 16.0;
}


#endif	/* _DS18B20_H */
