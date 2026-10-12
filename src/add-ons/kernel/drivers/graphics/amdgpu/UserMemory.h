/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#ifndef AMDGPU_USER_MEMORY_H
#define AMDGPU_USER_MEMORY_H

#include <KernelExport.h>
#include <kernel.h>

// user_memcpy is fault-safe for either kernel or user addresses. Embedded
// ABI pointers need their own userspace/range check before using that helper.
static inline bool
AmdgpuUserRange(uint64 address, size_t bytes)
{
	return address == (addr_t)address && bytes != 0
		&& is_user_address_range((const void*)(addr_t)address, bytes);
}

static inline status_t
AmdgpuCopyFromUser(void* destination, uint64 source, size_t bytes)
{
	return AmdgpuUserRange(source, bytes)
		? user_memcpy(destination, (const void*)(addr_t)source, bytes) : B_BAD_ADDRESS;
}

static inline status_t
AmdgpuCopyToUser(uint64 destination, const void* source, size_t bytes)
{
	return AmdgpuUserRange(destination, bytes)
		? user_memcpy((void*)(addr_t)destination, source, bytes) : B_BAD_ADDRESS;
}
#endif
