/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 */


#include <stdint.h>
#include <string.h>
#undef memset


/*!	The kernel's memset() (#237): 64 bytes per iteration with STP pairs,
	where the generic one stores 8 bytes per iteration. Drivers use it on
	device memory (B_UNCACHED_MEMORY is Device-nGnRnE), so every store is
	naturally aligned and there is no DC ZVA here: both fault on device
	memory. Whole pages of RAM are cleared with DC ZVA in
	PMAPPhysicalPageMapper::MemsetPhysical() instead.
*/


typedef uint64_t __attribute__((may_alias)) word_t;


void* __attribute__((optimize("no-tree-loop-distribute-patterns")))
memset(void* dest, int _value, size_t count)
{
	uint8_t* d = (uint8_t*)dest;
	const uint8_t value = (uint8_t)_value;

	if (count >= 16) {
		while (((uintptr_t)d & 7) != 0) {
			*d++ = value;
			count--;
		}

		const uint64_t pattern = value * UINT64_C(0x0101010101010101);
		word_t* w = (word_t*)d;
		while (count >= 64) {
			w[0] = pattern;
			w[1] = pattern;
			w[2] = pattern;
			w[3] = pattern;
			w[4] = pattern;
			w[5] = pattern;
			w[6] = pattern;
			w[7] = pattern;
			w += 8;
			count -= 64;
		}
		while (count >= 8) {
			*w++ = pattern;
			count -= 8;
		}
		d = (uint8_t*)w;
	}

	while (count > 0) {
		*d++ = value;
		count--;
	}

	return dest;
}
