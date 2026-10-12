/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include "GpuPageTable.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <map>
#include <random>
#include <vector>

struct Heap {
	std::map<uint64_t, GpuPageTable::Allocation> blocks;
	uint64_t next = 0xf410000000ULL;
	uint64_t bytes = 0;
	int fail = -1;
	unsigned retained = 0;
};

static bool
Allocate(void* cookie, size_t bytes, GpuPageTable::Allocation& a)
{
	Heap& heap = *(Heap*)cookie;
	if (heap.fail == 0) return false;
	if (heap.fail > 0) heap.fail--;
	uint64_t* memory = (uint64_t*)malloc(bytes + 16);
	assert(memory != NULL);
	memory[bytes / 8] = 0x13579bdfeca86420ULL;
	memory[bytes / 8 + 1] = bytes;
	a.cpu = memory; a.gpu = heap.next; a.cookie = bytes;
	heap.next += bytes;
	heap.blocks[a.gpu] = a;
	heap.bytes += bytes;
	return true;
}

static void
Release(void* cookie, size_t bytes, GpuPageTable::Allocation& a, bool reclaim)
{
	Heap& heap = *(Heap*)cookie;
	assert((size_t)a.cookie == bytes && heap.blocks.count(a.gpu) == 1);
	assert(a.cpu[bytes / 8] == 0x13579bdfeca86420ULL && a.cpu[bytes / 8 + 1] == bytes);
	if (!reclaim) heap.retained++;
	heap.bytes -= bytes;
	heap.blocks.erase(a.gpu);
	free((void*)a.cpu);
}

// Walk the actual callback allocations using the hardware PDE/PTE format,
// independently of GpuPageTable's CPU leaf index and Entry() implementation.
static uint64_t
Walk(const Heap& heap, const GpuPageTable& table, uint64_t address)
{
	uint64_t page = address / 4096;
	uint64_t pde = table.directory.cpu[page / 512];
	if ((pde & 1) == 0) return 0;
	auto found = heap.blocks.find(pde & ~4095ULL);
	assert(found != heap.blocks.end());
	return found->second.cpu[page % 512];
}

int
main()
{
	Heap heap;
	GpuPageTable table = {};
	heap.fail = 0;
	assert(!table.Initialize(&heap, Allocate, Release));
	assert(heap.blocks.empty());
	heap.fail = -1;
	assert(table.Initialize(&heap, Allocate, Release));
	assert(!table.Initialize(&heap, Allocate, Release));
	assert(!GpuPageTable::Range(UINT64_MAX - 4095, 2));
	assert(!GpuPageTable::Range(0, 0));
	assert(!GpuPageTable::Range(1, 1));
	assert(!GpuPageTable::Range(GpuPageTable::kSize - 4096, 2));
	assert(!GpuPageTable::ValidEntry(0x10));
	assert(!GpuPageTable::ValidEntry(0x8000000000000071ULL));
	assert(!GpuPageTable::ValidEntry(0x51));
	assert(GpuPageTable::ValidEntry(0xf400000031ULL));
	assert(GpuPageTable::ValidEntry(0x0001abcd0067ULL));
	uint64_t entries[] = {0xf420000071ULL, 0xf420001071ULL, 0x1000067};
	assert(table.Map(0x1ff000, entries, 3) == GpuPageTable::OK);
	assert(table.mappedPages == 3 && table.leafCount == 2);
	assert(table.Map(0x1fe000, entries, 3) == GpuPageTable::OVERLAP);
	assert(table.Entry(0x1fe000) == 0);
	assert(!table.Unmap(0x1fe000, 2));
	assert(Walk(heap, table, 0x1ff000) == entries[0]);
	assert(table.Unmap(0x1ff000, 3));
	assert(table.leafCount == 0 && table.mappedPages == 0);
	uint64_t baseline = heap.bytes;
	heap.fail = 1;
	assert(table.Map(0x1ff000, entries, 3) == GpuPageTable::NO_MEMORY);
	assert(heap.bytes == baseline && table.mappedPages == 0 && table.leafCount == 0);
	assert(Walk(heap, table, 0x1ff000) == 0 && Walk(heap, table, 0x200000) == 0);
	heap.fail = -1;
	assert(table.Map(0x10000, entries, 1) == GpuPageTable::OK);
	heap.fail = 0;
	assert(table.Map(0x1ff000, entries, 3) == GpuPageTable::NO_MEMORY);
	assert(Walk(heap, table, 0x10000) == entries[0] && Walk(heap, table, 0x1ff000) == 0);
	heap.fail = -1;
	assert(table.Unmap(0x10000, 1));

	std::map<uint64_t, uint64_t> oracle;
	std::mt19937 random(0x5100);
	struct Mapping { uint64_t address; uint32_t pages; };
	std::vector<Mapping> live;
	for (unsigned round = 0; round < 5000; round++) {
		if (!live.empty() && random() % 2 == 0) {
			unsigned slot = random() % live.size();
			Mapping m = live[slot];
			assert(table.Unmap(m.address, m.pages));
			for (unsigned i = 0; i < m.pages; i++) oracle.erase(m.address + i * 4096);
			live.erase(live.begin() + slot);
		} else {
			uint32_t pages = 1 + random() % 64;
			uint64_t address = ((uint64_t)random() % (GpuPageTable::kSize / 4096 - pages)) * 4096;
			std::vector<uint64_t> ptes(pages);
			bool overlaps = false;
			for (uint32_t i = 0; i < pages; i++) {
				ptes[i] = ((uint64_t)random() << 12 & GpuPageTable::kPhysicalMask) | 0x67;
				overlaps |= oracle.count(address + i * 4096) != 0;
			}
			assert(table.Map(address, ptes.data(), pages)
				== (overlaps ? GpuPageTable::OVERLAP : GpuPageTable::OK));
			if (!overlaps) {
				live.push_back({address, pages});
				for (uint32_t i = 0; i < pages; i++) oracle[address + i * 4096] = ptes[i];
			}
		}
		assert(table.mappedPages == oracle.size());
		for (auto item : oracle) {
			assert(Walk(heap, table, item.first) == item.second);
			assert(table.Entry(item.first) == item.second);
		}
		for (unsigned sample = 0; sample < 32; sample++) {
			uint64_t address = ((uint64_t)random() % (GpuPageTable::kSize / 4096)) * 4096;
			auto found = oracle.find(address);
			assert(Walk(heap, table, address) == (found == oracle.end() ? 0 : found->second));
		}
	}
	table.Uninitialize(false);
	assert(heap.blocks.empty() && heap.bytes == 0 && heap.retained != 0);
	assert(table.Initialize(&heap, Allocate, Release));
	assert(table.Map(GpuPageTable::kSize - 4096, entries, 1) == GpuPageTable::OK);
	assert(Walk(heap, table, GpuPageTable::kSize - 4096) == entries[0]);
	table.Uninitialize(true);
	assert(heap.blocks.empty() && heap.bytes == 0);
	puts("PASS: independent 64-GiB page walks, 5000 randomized sparse mappings,"
		" atomic allocation failure, permissions, bounds, reclamation and quarantine");
}
