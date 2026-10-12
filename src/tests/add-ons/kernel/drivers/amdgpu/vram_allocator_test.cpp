/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include "VramAllocator.h"
#include <assert.h>
#include <stdio.h>
#include <vector>
#include <random>

int
main()
{
	VramAllocator a = {};
	assert(!a.Init(0));
	assert(a.Init(4096ULL * 4096));
	assert(a.Reserve(0, 257 * 4096));
	assert(a.Reserve(4096ULL * 4087, 9 * 4096));
	std::vector<bool> used(4096);
	for (unsigned i = 0; i < 4096; i++)
		used[i] = i < 257 || i >= 4087;
	struct Allocation { uint64_t offset, bytes; };
	std::vector<Allocation> live;
	std::mt19937 random(0x5100);
	for (unsigned iteration = 0; iteration < 20000; iteration++) {
		if (!live.empty() && random() % 3 == 0) {
			unsigned index = random() % live.size();
			auto item = live[index];
			assert(a.Free(item.offset, item.bytes));
			for (uint64_t i = item.offset / 4096; i < (item.offset + item.bytes) / 4096; i++)
				used[i] = false;
			live.erase(live.begin() + index);
		} else {
			uint64_t count = random() % 64 + 1;
			uint64_t align = 1ULL << (random() % 5);
			uint64_t limit = 512 + random() % 3585;
			uint64_t minimum = iteration % 2 == 0 ? 0 : random() % (limit + 1);
			uint64_t offset;
			bool result = a.Allocate(count * 4096, align * 4096, limit * 4096,
				offset, minimum * 4096);
			bool possible = false;
			for (uint64_t start = 0; start + count <= limit; start += align) {
				if (start < minimum)
					continue;
				bool free = true;
				for (uint64_t j = start; j < start + count; j++)
					free &= !used[j];
				possible |= free;
			}
			assert(result == possible);
			if (result) {
				assert(offset % (align * 4096) == 0 && offset + count * 4096 <= limit * 4096);
				assert(offset >= minimum * 4096);
				for (uint64_t i = offset / 4096; i < offset / 4096 + count; i++) {
					assert(!used[i]);
					used[i] = true;
				}
				live.push_back({offset, count * 4096});
			}
		}
		uint64_t total = 0;
		for (auto item : live)
			total += item.bytes;
		assert(a.allocated == total);
	}
	for (auto item : live)
		assert(a.Free(item.offset, item.bytes));
	assert(a.allocated == 0);
	a.Uninit();
	assert(a.Init(8ULL << 30));
	assert(a.Reserve(0, 5ULL << 30));
	uint64_t offset;
	assert(a.Allocate(4096, 4096, 8ULL << 30, offset) && offset == (5ULL << 30));
	assert(!a.Reserve(UINT64_MAX - 4095, 8192));
	assert(!a.Allocate(UINT64_MAX - 4095, 4096, UINT64_MAX, offset));
	assert(!a.Allocate(4096, UINT64_MAX, UINT64_MAX, offset));
	assert(a.Free(5ULL << 30, 4096));
	assert(!a.Allocate(4096, 4096, 8ULL << 30, offset, UINT64_MAX - 4095));
	assert(!a.Allocate(4096, 4096, 8ULL << 30, offset, 1));
	assert(!a.Allocate(4096, 4096, 8ULL << 30, offset, 8ULL << 30));
	assert(a.Allocate(4096, 65536, 8ULL << 30, offset, (5ULL << 30) + 4096));
	assert(offset == (5ULL << 30) + 65536);
	assert(a.Free(offset, 4096));
	a.Uninit();
	assert(a.Init(8ULL << 30));
	assert(a.Reserve(0, 64ULL << 20));
	// Exhaust device-only VRAM without consuming the CPU-visible pool.
	for (uint64_t i = 0; i < 124; i++) {
		assert(a.Allocate(64ULL << 20, 4096, 8ULL << 30, offset, 256ULL << 20));
		assert(offset == (256ULL << 20) + i * (64ULL << 20));
	}
	assert(!a.Allocate(4096, 4096, 8ULL << 30, offset, 256ULL << 20));
	assert(a.Allocate(192ULL << 20, 4096, 256ULL << 20, offset));
	assert(offset == (64ULL << 20));
	a.Uninit();
	puts("PASS: allocator reservations, 20,000 range-bounded randomized operations,"
		" separate aperture exhaustion and 64-bit bounds");
}
