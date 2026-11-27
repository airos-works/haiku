/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <emmintrin.h>


/*!	memmove() with 16 byte SSE2 moves. Buffers that do not overlap are left to
	memcpy(). Overlapping ones are moved in the direction that reads every
	source byte before the destination overwrites it: forward when the
	destination is below the source, backward when it is above. The first and
	the last 16 bytes are loaded before anything is stored and stored last,
	which leaves the loop with aligned stores and no byte-wise head or tail.
*/


namespace {


typedef uint64_t __attribute__((may_alias, aligned(1))) unaligned_uint64;
typedef uint32_t __attribute__((may_alias, aligned(1))) unaligned_uint32;
typedef uint16_t __attribute__((may_alias, aligned(1))) unaligned_uint16;


static inline __m128i
load(const uint8_t* source)
{
	return _mm_loadu_si128((const __m128i*)source);
}


static inline void
store(uint8_t* destination, __m128i value)
{
	_mm_storeu_si128((__m128i*)destination, value);
}


static inline void
store_aligned(uint8_t* destination, __m128i value)
{
	_mm_store_si128((__m128i*)destination, value);
}


/*!	Up to 64 bytes: everything is loaded before anything is stored, which is
	right for any overlap.
*/
static inline void
move_small(uint8_t* destination, const uint8_t* source, size_t length)
{
	if (length >= 32) {
		__m128i a = load(source);
		__m128i b = load(source + 16);
		__m128i c = load(source + length - 32);
		__m128i d = load(source + length - 16);
		store(destination, a);
		store(destination + 16, b);
		store(destination + length - 32, c);
		store(destination + length - 16, d);
	} else if (length >= 16) {
		__m128i a = load(source);
		__m128i b = load(source + length - 16);
		store(destination, a);
		store(destination + length - 16, b);
	} else if (length >= 8) {
		uint64_t a = *(const unaligned_uint64*)source;
		uint64_t b = *(const unaligned_uint64*)(source + length - 8);
		*(unaligned_uint64*)destination = a;
		*(unaligned_uint64*)(destination + length - 8) = b;
	} else if (length >= 4) {
		uint32_t a = *(const unaligned_uint32*)source;
		uint32_t b = *(const unaligned_uint32*)(source + length - 4);
		*(unaligned_uint32*)destination = a;
		*(unaligned_uint32*)(destination + length - 4) = b;
	} else if (length >= 2) {
		uint16_t a = *(const unaligned_uint16*)source;
		uint16_t b = *(const unaligned_uint16*)(source + length - 2);
		*(unaligned_uint16*)destination = a;
		*(unaligned_uint16*)(destination + length - 2) = b;
	} else if (length == 1)
		*destination = *source;
}


/*!	destination < source, length > 64. Every 64 byte block is loaded before
	it is stored, and the stores stay below the source bytes still to load.
*/
static void
move_forward(uint8_t* destination, const uint8_t* source, size_t length)
{
	const __m128i head = load(source);
	const __m128i tail = load(source + length - 16);

	size_t offset = 16 - ((uintptr_t)destination & 15);
	for (; offset + 64 <= length; offset += 64) {
		__m128i a = load(source + offset);
		__m128i b = load(source + offset + 16);
		__m128i c = load(source + offset + 32);
		__m128i d = load(source + offset + 48);
		store_aligned(destination + offset, a);
		store_aligned(destination + offset + 16, b);
		store_aligned(destination + offset + 32, c);
		store_aligned(destination + offset + 48, d);
	}
	for (; offset + 16 <= length; offset += 16)
		store_aligned(destination + offset, load(source + offset));

	store(destination + length - 16, tail);
	store(destination, head);
}


/*!	destination > source, length > 64: the mirror image of move_forward(),
	from the end down.
*/
static void
move_backward(uint8_t* destination, const uint8_t* source, size_t length)
{
	const __m128i head = load(source);
	const __m128i tail = load(source + length - 16);

	size_t end = length - ((uintptr_t)(destination + length) & 15);
	for (; end >= 64 + 16; end -= 64) {
		__m128i a = load(source + end - 16);
		__m128i b = load(source + end - 32);
		__m128i c = load(source + end - 48);
		__m128i d = load(source + end - 64);
		store_aligned(destination + end - 16, a);
		store_aligned(destination + end - 32, b);
		store_aligned(destination + end - 48, c);
		store_aligned(destination + end - 64, d);
	}
	for (; end >= 16 + 16; end -= 16)
		store_aligned(destination + end - 16, load(source + end - 16));

	// [16, end) may still be left (end < 32); nothing below the source bytes
	// it needs has been stored yet.
	if (end > 16)
		store(destination + end - 16, load(source + end - 16));

	store(destination + length - 16, tail);
	store(destination, head);
}


}	// namespace


extern "C" void*
memmove(void* _destination, const void* _source, size_t length)
{
	uint8_t* destination = (uint8_t*)_destination;
	const uint8_t* source = (const uint8_t*)_source;

	const uintptr_t distance = (uintptr_t)destination - (uintptr_t)source;
	if (distance >= length && -distance >= length)
		return memcpy(destination, source, length);

	if (length <= 64)
		move_small(destination, source, length);
	else if (destination < source)
		move_forward(destination, source, length);
	else if (destination > source)
		move_backward(destination, source, length);

	return _destination;
}
