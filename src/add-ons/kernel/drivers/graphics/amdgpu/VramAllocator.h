/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#ifndef AMDGPU_VRAM_ALLOCATOR_H
#define AMDGPU_VRAM_ALLOCATOR_H
#include <stddef.h>
#include <stdint.h>

// Page allocator for VRAM offsets, independent of kernel/CPU/GPU addresses.
// The owner serializes calls and keeps allocation sizes in buffer objects.
struct VramAllocator {
	uint32_t* bits;
	uint64_t pages;
	uint64_t allocated;
	bool Init(uint64_t size);
	void Uninit();
	bool Reserve(uint64_t offset, uint64_t size);
	bool Allocate(uint64_t size, uint64_t alignment, uint64_t limit, uint64_t& offset,
		uint64_t minimum = 0);
	bool Free(uint64_t offset, uint64_t size);
};
#endif
