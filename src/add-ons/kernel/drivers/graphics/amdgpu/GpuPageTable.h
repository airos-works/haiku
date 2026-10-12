/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#ifndef AMDGPU_GPU_PAGE_TABLE_H
#define AMDGPU_GPU_PAGE_TABLE_H

#include <stddef.h>
#include <stdint.h>

// GFX8 two-level, 4 KiB pages, 512 PTEs per leaf. The owner serializes
// updates and invalidates the hardware VMID before using a changed table.
// Allocation callbacks supply CPU-accessible, pinned GPU memory. No user
// address or handle is interpreted here.
struct GpuPageTable {
	static const uint64_t kSize = 1ULL << 36;
	static const uint32_t kDirectoryEntries = kSize >> 21;
	static const uint32_t kDirectoryBytes = kDirectoryEntries * 8;
	static const uint64_t kPhysicalMask = 0x000000fffffff000ULL;
	struct Allocation {
		volatile uint64_t* cpu;
		uint64_t gpu;
		intptr_t cookie;
	};
	enum Result { OK, INVALID, OVERLAP, NO_MEMORY };
	typedef bool (*Allocate)(void*, size_t, Allocation&);
	typedef void (*Release)(void*, size_t, Allocation&, bool);

	Allocation directory;
	Allocation* leaves;
	void* owner;
	Allocate allocate;
	Release release;
	uint64_t mappedPages;
	uint32_t leafCount;

	bool Initialize(void* owner, Allocate allocate, Release release);
	Result Map(uint64_t address, const uint64_t* entries, uint32_t pages);
	bool Unmap(uint64_t address, uint32_t pages);
	uint64_t Entry(uint64_t address) const;
	void Uninitialize(bool reclaim);
	static bool Range(uint64_t address, uint32_t pages);
	static bool ValidEntry(uint64_t entry);
};
#endif
