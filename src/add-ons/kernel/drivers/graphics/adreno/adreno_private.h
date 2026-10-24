/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef ADRENO_PRIVATE_H
#define ADRENO_PRIVATE_H


#include <KernelExport.h>
#include <OS.h>


#define INFO(x...)	dprintf("adreno: " x)
#define ERROR(x...)	dprintf("adreno: " x)


// what userland is told (msm_drm.h parameters) about the SC8280XP's A690
#define ADRENO_GPU_ID				690
#define ADRENO_CHIP_ID				0x06090000ull
#define ADRENO_GMEM_SIZE			(4 * 1024 * 1024)
#define ADRENO_GMEM_BASE			0x100000
#define ADRENO_HIGHEST_BANK_BIT		16
#define ADRENO_UBWC_SWIZZLE			0x6		// levels 2 and 3
#define ADRENO_MACROTILE_MODE		1
#define ADRENO_UCHE_TRAP_BASE		0x1fffffffff000ull
#define ADRENO_MAX_FREQUENCY		690000000

// The GPU's address space: the driver's own buffers below 4 GiB, userland's
// from there to the end of the 39 bit space.
#define ADRENO_VA_USER_START		0x100000000ull
#define ADRENO_VA_USER_SIZE			(0x8000000000ull - ADRENO_VA_USER_START)


struct register_window {
	area_id			area = -1;
	volatile uint8*	base = NULL;

	status_t Map(const char* name, phys_addr_t physical, size_t size)
	{
		area = map_physical_memory(name, physical, size, B_ANY_KERNEL_ADDRESS,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&base);
		return area < 0 ? area : B_OK;
	}

	void Unmap()
	{
		if (area >= 0)
			delete_area(area);
		area = -1;
	}

	uint32 Read(uint32 offset) const
		{ return *(volatile uint32*)(base + offset); }
	void Write(uint32 offset, uint32 value)
		{ *(volatile uint32*)(base + offset) = value; }

	bool Wait(uint32 offset, uint32 mask, uint32 value, bigtime_t timeout)
	{
		bigtime_t end = system_time() + timeout;
		while ((Read(offset) & mask) != value) {
			if (system_time() > end)
				return false;
			spin(10);
		}
		return true;
	}
};


// adreno.cpp: the GPU, brought up on first use

status_t	adreno_gpu_start();
bool		adreno_gpu_running();

status_t	adreno_gpu_map(uint64 iova, void* address, size_t size,
				bool cached);
void		adreno_gpu_unmap(uint64 iova, size_t size);

// The ring: emit under the lock, between Reserve() and Flush().
void		adreno_ring_lock();
void		adreno_ring_unlock();
status_t	adreno_ring_reserve(uint32 dwords);
void		adreno_ring_emit(uint32 value);
uint32		adreno_ring_flush_fence();
	// ends the commands with a fence and returns its number

uint32		adreno_completed_fence();
status_t	adreno_wait_fence(uint32 fence, bigtime_t deadline);
	// absolute system_time() deadline, B_INFINITE_TIMEOUT for none
uint64		adreno_timestamp();
uint32		adreno_faults();

uint32		adreno_pkt7(uint32 opcode, uint32 count);


#endif	// ADRENO_PRIVATE_H
