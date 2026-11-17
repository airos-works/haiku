/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Broadcom VideoCore IV 3D (V3D 2.1), the 3D core of the BCM2835/6/7
	(Raspberry Pi 1 to 3, the Pi 3 B+'s BCM2837 in particular).

	This is the render-only device that Mesa's vc4 driver talks to (see
	<graphics/vc4/vc4_haiku.h>): Linux' DRM VC4 interface, as its
	drivers/gpu/drm/vc4 implements it for the 3D core. Scan-out is
	rpi_display's business.

	Memory. The core has no MMU: it reads and writes physical memory, at
	the bus address of the BCM2837's uncached alias (physical | 0xc0000000,
	the /soc dma-ranges). Every buffer object is physically contiguous. When
	the driver starts (at boot, while memory is still in one piece) it
	reserves a pool of contiguous memory (64 MB unless the "pool_size"
	setting says otherwise) and gives the buffers ranges of it; when the
	pool is full a buffer gets a contiguous area of its own, which may fail
	on a system that has run for a while. The CPU sees all of it
	write-combining (the core does not snoop the ARM's caches).

	A buffer from the pool is mapped into a team as a clone of a kernel area
	of its own over its range; the range goes back to the pool only once
	that area's cache has no other user (a fork() of the team copies the
	mapping). The mappings are B_KERNEL_AREA: the team can neither resize
	nor delete them; the driver removes them when the handle is closed.

	Validation. The core would write wherever a command list or shader
	told it to. Every job is validated as Linux does it (vc4_validate.cpp,
	vc4_validate_shaders.cpp, vc4_render_cl.cpp: ports of Linux' MIT
	licensed sources): the binner command list packet by packet, each
	address relocated from a buffer handle and checked against the buffer;
	the shader records and their uniforms, the texture parameters in them;
	shaders once when their buffer is created (no stores to memory, every
	texture and uniform access accounted for). The render command list is
	written by the driver from the job's surfaces and tile range.

	Jobs. A job is a binner list (may be empty) and the render list. They
	run one at a time, in the order they were submitted, on one thread:
	bin, wait for the flush, render, wait for the frame. The binner gets a
	512 KB slot of a 16 MB buffer for its tile state and lists, and more
	slots when it runs out (the "out of memory" interrupt). A job that does
	not get anywhere for a second is given up: the core is reset through
	its power domain, as Linux does it. Should the interrupts not come
	through, the core's flush and frame counters still tell when a list is
	done, and the driver polls them.

	Power. The firmware owns the V3D power domain and clock (Linux:
	raspberrypi-power and clk-raspberrypi); the driver asks it through the
	mailbox (rpi_firmware).

	The device tree's node (v3d@7ec00000, "brcm,vc4-v3d" in the Pi
	firmware's trees, "brcm,bcm2835-v3d" in Linux's) is "disabled" unless
	the firmware was told to hand the display pipeline to the OS
	(vc4-kms-v3d); the core is there all the same and the driver ignores
	the status, as v3d does. */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <new>

#include <AutoDeleter.h>
#include <KernelExport.h>
#include <bus/FDT.h>
#include <device_manager.h>
#include <driver_settings.h>

#include <condition_variable.h>
#include <kernel.h>
#include <lock.h>
#include <rpi_firmware.h>
#include <team.h>
#include <util/AutoLock.h>
#include <vm/vm.h>
#include <vm_defs.h>
#include <vm/VMAddressSpace.h>
#include <vm/VMArea.h>
#include <vm/VMCache.h>

#include <graphics/vc4/vc4_drm.h>
#include <graphics/vc4/vc4_haiku.h>

#include "vc4_packet.h"
#include "vc4_private.h"
#include "vc4_regs.h"


//#define TRACE_VC4
#ifdef TRACE_VC4
#	define TRACE(x...) dprintf("vc4: " x)
#else
#	define TRACE(x...) ;
#endif
#define INFO(x...)	dprintf("vc4: " x)
#define ERROR(x...)	dprintf("vc4: " x)

#define VC4_DRIVER_MODULE_NAME	"drivers/graphics/vc4/driver_v1"
#define VC4_DEVICE_MODULE_NAME	"drivers/graphics/vc4/device_v1"

// The /soc dma-ranges of the BCM2835/6/7: the core sees the first 1008 MB
// of RAM at 0xc0000000, the alias that bypasses the VideoCore's L2 cache.
#define VC4_BUS_OFFSET			0xc0000000u
#define VC4_DMA_LIMIT			0x3f000000u

// The binner's memory: tile state, tile lists and their overflow, in 512 KB
// slots (Linux: bin_bo_alloc()). The addresses the binner keeps in the tile
// state have 28 bits, the top 4 come from the tile state's own: the buffer
// must not cross a 256 MB line.
#define VC4_BIN_SIZE			(16 * 1024 * 1024)
#define VC4_BIN_SLOT_SIZE		(512 * 1024)
#define VC4_BIN_SLOTS			(VC4_BIN_SIZE / VC4_BIN_SLOT_SIZE)
#define VC4_BIN_BOUNDARY		0x10000000u
// Slots a submission may not take: the running job's overflow memory
#define VC4_BIN_RESERVED_SLOTS	4

#define VC4_DEFAULT_POOL_MB		64
#define VC4_MIN_POOL_MB			8
#define VC4_MAX_BO_SIZE			(256 * 1024 * 1024)
#define VC4_MAX_SHADER_SIZE		(16 * 1024 * 1024)
#define VC4_MAX_JOB_BOS			16384

// A job that has not moved on in this long is given up (Linux: 100 ms).
#define VC4_STALL_TIMEOUT		1000000
// How often a running job is looked at: with interrupts, and without them.
#define VC4_CHECK_INTERVAL		250000
#define VC4_POLL_INTERVAL		1000

#define VC4_SLCACTL_ALL \
	(VC4_SET_FIELD(0xf, V3D_SLCACTL_T1CC) | VC4_SET_FIELD(0xf, V3D_SLCACTL_T0CC) \
		| VC4_SET_FIELD(0xf, V3D_SLCACTL_UCC) \
		| VC4_SET_FIELD(0xf, V3D_SLCACTL_ICC))
#define VC4_SLCACTL_TEXTURE \
	(VC4_SET_FIELD(0xf, V3D_SLCACTL_T1CC) | VC4_SET_FIELD(0xf, V3D_SLCACTL_T0CC))

#define VC4_DRIVER_INTERRUPTS \
	(V3D_INT_OUTOMEM | V3D_INT_FLDONE | V3D_INT_FRDONE)


/*!	A range of the pool. A "zombie" range belongs to no buffer any more, but
	a mapping of it may still exist (in a fork of the team that had it
	mapped): it is kept until its cache has no other user.
*/
struct vc4_block {
	vc4_block*		next;		// sorted by offset
	uint32			offset;
	uint32			size;
	VMCache*		zombie;		// referenced
};

struct vc4_info {
	device_node*	node;
	uint64			regsBase;
	uint64			regsSize;
	uint32			interrupt;

	area_id			regsArea;
	volatile uint8*	regs;
	bool			interruptInstalled;
	uint32			clockRate;
	uint32			ident[3];

	mutex			lock;
		// buffers, the pool, bin slots, the job queue

	// the pool
	area_id			poolArea;
	uint8*			pool;
	phys_addr_t		poolPhysical;
	uint32			poolSize;
	uint32			poolUsed;
	vc4_block*		blocks;
	uint64			contiguousBytes;
	vc4_bo*			buffers;

	// the binner's memory
	area_id			binArea;
	phys_addr_t		binPhysical;
	uint32			binSlotsUsed;

	// jobs
	vc4_exec_info*	firstJob;
	vc4_exec_info*	lastJob;
	uint64			submitted;
	uint64			completed;
	sem_id			jobSemaphore;
	thread_id		executor;
	bool			stopping;
	ConditionVariable completedCondition;

	int32			events;
	ConditionVariable eventCondition;
	int64			interrupts;
	bool			polling;
		// the interrupts do not arrive: the job thread looks for itself
	uint32			missedInterrupts;
	uint32			resets;

	bool			hangStateValid;
	drm_vc4_get_hang_state hangState;
};

struct vc4_file {
	vc4_info*		device;
	vc4_bo**		buffers;	// handle - 1 is the index
	uint32			bufferCount;
};


bool gVc4Debug = true;

static device_manager_info* sDeviceManager;
static rpi_firmware_module_info* sFirmware;


static inline uint32
read32(vc4_info* info, uint32 reg)
{
	return *(volatile uint32*)(info->regs + reg);
}


static inline void
write32(vc4_info* info, uint32 reg, uint32 value)
{
	*(volatile uint32*)(info->regs + reg) = value;
}


//	#pragma mark - power


/*!	The V3D power domain and clock, as Linux' raspberrypi-power and
	clk-raspberrypi drivers ask the firmware for them (vc4_v3d's runtime
	resume): the domain on, the clock on, at its maximum rate.
*/
static status_t
power_on(vc4_info* info)
{
	uint32 domain[2] = {RPI_FIRMWARE_DOMAIN_V3D, 1};
	status_t status = sFirmware->property(RPI_FIRMWARE_SET_DOMAIN_STATE,
		domain, sizeof(domain));
	if (status != B_OK) {
		// Firmware older than the domain interface: the core may be on
		// anyway; IDENT0 tells.
		ERROR("the firmware does not switch the V3D power domain on: %s\n",
			strerror(status));
	}

	uint32 maximum = 0;
	sFirmware->get_clock_rate(RPI_FIRMWARE_CLOCK_V3D, true, &maximum);
	status = sFirmware->set_clock_state(RPI_FIRMWARE_CLOCK_V3D, true);
	if (status != B_OK) {
		ERROR("the firmware does not switch the V3D clock on: %s\n",
			strerror(status));
	}
	if (maximum != 0)
		sFirmware->set_clock_rate(RPI_FIRMWARE_CLOCK_V3D, maximum);
	sFirmware->get_clock_rate(RPI_FIRMWARE_CLOCK_V3D, false, &info->clockRate);

	spin(10);
	return B_OK;
}


static void
power_off(vc4_info* info)
{
	sFirmware->set_clock_state(RPI_FIRMWARE_CLOCK_V3D, false);
	uint32 domain[2] = {RPI_FIRMWARE_DOMAIN_V3D, 0};
	sFirmware->property(RPI_FIRMWARE_SET_DOMAIN_STATE, domain,
		sizeof(domain));
}


/*!	What Linux' vc4_v3d_init_hw(), vc4_v3d_bind() and vc4_irq_enable() set
	up after the core was powered: all of the vertex pipe memory for the
	shaders, no binner overflow memory, the driver's interrupts acknowledged,
	bin and render done enabled (out of memory only while a job bins).
*/
static void
init_hardware(vc4_info* info)
{
	write32(info, V3D_VPMBASE, 0);
	write32(info, V3D_BPOA, 0);
	write32(info, V3D_BPOS, 0);
	write32(info, V3D_INTDIS, 0xffffffff);
	write32(info, V3D_INTCTL, VC4_DRIVER_INTERRUPTS);
	write32(info, V3D_INTENA, V3D_INT_FLDONE | V3D_INT_FRDONE);
}


//	#pragma mark - memory


static size_t
data_cache_line_size()
{
	uint64 ctr;
	asm volatile("mrs %0, ctr_el0" : "=r" (ctr));
	return (size_t)4 << ((ctr >> 16) & 0xf);
}


/*!	Takes what the allocation left in the CPU caches out and keeps the
	memory out of them: the core reads and writes RAM behind their back.
	Clones of the area inherit the memory type.
*/
static status_t
make_uncached(area_id area, void* address, size_t size, phys_addr_t physical)
{
	const size_t lineSize = data_cache_line_size();
	for (addr_t line = (addr_t)address; line < (addr_t)address + size;
			line += lineSize) {
		asm volatile("dc civac, %0" : : "r" (line) : "memory");
	}
	memory_full_barrier();
	return vm_set_area_memory_type(area, physical, B_WRITE_COMBINING_MEMORY);
}


/*!	A contiguous area the core reaches, below VC4_DMA_LIMIT. */
static area_id
create_contiguous_area(const char* name, size_t size, phys_size_t boundary,
	uint8** _address, phys_addr_t* _physical)
{
	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	physicalRestrictions.high_address = VC4_DMA_LIMIT;
	physicalRestrictions.boundary = boundary;

	void* address;
	area_id area = create_area_etc(B_SYSTEM_TEAM, name, size, B_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0, &virtualRestrictions,
		&physicalRestrictions, &address);
	if (area < 0)
		return area;

	physical_entry entry;
	status_t status = get_memory_map(address, B_PAGE_SIZE, &entry, 1);
	if (status == B_OK && entry.address + size > VC4_DMA_LIMIT)
		status = B_NO_MEMORY;
	if (status == B_OK)
		status = make_uncached(area, address, size, entry.address);
	if (status != B_OK) {
		delete_area(area);
		return status;
	}

	*_address = (uint8*)address;
	*_physical = entry.address;
	return area;
}


/*!	First fit of \a size bytes in the pool. Device locked. */
static vc4_block*
pool_allocate(vc4_info* info, uint32 size)
{
	if (info->pool == NULL)
		return NULL;

	uint32 offset = 0;
	vc4_block** link = &info->blocks;
	while (*link != NULL && (*link)->offset - offset < size) {
		offset = (*link)->offset + (*link)->size;
		link = &(*link)->next;
	}
	if ((uint64)offset + size > info->poolSize)
		return NULL;

	vc4_block* block = (vc4_block*)calloc(1, sizeof(vc4_block));
	if (block == NULL)
		return NULL;
	block->offset = offset;
	block->size = size;
	block->next = *link;
	*link = block;
	info->poolUsed += size;
	return block;
}


static void
pool_free(vc4_info* info, vc4_block* block)
{
	for (vc4_block** link = &info->blocks; *link != NULL;
			link = &(*link)->next) {
		if (*link == block) {
			*link = block->next;
			break;
		}
	}
	info->poolUsed -= block->size;
	free(block);
}


/*!	Gives back the ranges whose last mapping is gone. Device locked. */
static void
pool_reclaim(vc4_info* info)
{
	vc4_block* block = info->blocks;
	while (block != NULL) {
		vc4_block* next = block->next;
		if (block->zombie != NULL) {
			VMCache* cache = block->zombie;
			cache->Lock();
			if (cache->RefCount() == 1) {
				cache->ReleaseRefAndUnlock();
				pool_free(info, block);
			} else
				cache->Unlock();
		}
		block = next;
	}
}


/*!	A new buffer object of at least \a size bytes, cleared, with one
	reference. Called without the device lock.
*/
static status_t
create_bo(vc4_info* info, uint32 size, vc4_bo** _bo)
{
	if (size == 0 || size > VC4_MAX_BO_SIZE)
		return B_BAD_VALUE;
	size = ROUNDUP(size, B_PAGE_SIZE);

	vc4_bo* bo = (vc4_bo*)calloc(1, sizeof(vc4_bo));
	if (bo == NULL)
		return B_NO_MEMORY;
	bo->references = 1;
	bo->size = size;
	bo->area = -1;
	bo->mapSource = -1;
	bo->userArea = -1;
	bo->madv = VC4_MADV_WILLNEED;

	MutexLocker locker(info->lock);
	vc4_block* block = pool_allocate(info, size);
	if (block == NULL && info->pool != NULL) {
		pool_reclaim(info);
		block = pool_allocate(info, size);
	}
	locker.Unlock();

	if (block != NULL) {
		bo->block = block;
		bo->physical = info->poolPhysical + block->offset;
		bo->vaddr = info->pool + block->offset;
		// someone else's data may have been here
		memset(bo->vaddr, 0, size);
	} else {
		// The VM clears the pages.
		bo->area = create_contiguous_area("vc4 buffer", size, 0, &bo->vaddr,
			&bo->physical);
		if (bo->area < 0) {
			status_t status = bo->area;
			TRACE("no contiguous %" B_PRIu32 " bytes: %s\n", size,
				strerror(status));
			free(bo);
			return status;
		}
	}
	bo->paddr = (uint32)bo->physical | VC4_BUS_OFFSET;

	// the clearing reaches memory before the core can read it
	memory_full_barrier();

	locker.Lock();
	bo->next = info->buffers;
	info->buffers = bo;
	if (bo->area >= 0)
		info->contiguousBytes += size;

	*_bo = bo;
	return B_OK;
}


/*!	Drops a reference; the last one frees the buffer. Device locked. */
static void
put_bo(vc4_info* info, vc4_bo* bo)
{
	if (--bo->references > 0)
		return;

	for (vc4_bo** link = &info->buffers; *link != NULL;
			link = &(*link)->next) {
		if (*link == bo) {
			*link = bo->next;
			break;
		}
	}

	if (bo->userArea >= 0) {
		// normally gone with the handle already
		vm_delete_area(bo->userTeam, bo->userArea, true);
	}
	vc4_free_validated_shader(bo->validated_shader);

	if (bo->area >= 0) {
		// mappings left in a fork keep the pages
		delete_area(bo->area);
		info->contiguousBytes -= bo->size;
	} else if (bo->mapSource >= 0) {
		delete_area(bo->mapSource);
		bo->block->zombie = bo->mapCache;
		pool_reclaim(info);
	} else
		pool_free(info, bo->block);

	free(bo);
}


status_t
vc4_bo_create_kernel(vc4_info* info, uint32 size, vc4_bo** _bo)
{
	return create_bo(info, size, _bo);
}


void
vc4_bo_release(vc4_info* info, vc4_bo* bo)
{
	MutexLocker locker(info->lock);
	put_bo(info, bo);
}


/*!	Maps \a bo into the calling team (VC4_HAIKU_MMAP_BO). Device locked. */
static status_t
map_bo(vc4_info* info, vc4_bo* bo, void** _address)
{
	team_id team = team_get_current_team_id();

	if (bo->userArea >= 0) {
		area_info areaInfo;
		if (get_area_info(bo->userArea, &areaInfo) == B_OK
			&& areaInfo.team == bo->userTeam) {
			if (bo->userTeam != team)
				return B_NOT_ALLOWED;
			*_address = areaInfo.address;
			return B_OK;
		}
		// the team went through exec() since
		bo->userArea = -1;
	}

	area_id source = bo->area;
	if (source < 0) {
		// A kernel area of its own over the buffer's range of the pool,
		// whose cache tells when no mapping of the range is left.
		if (bo->mapSource < 0) {
			void* address;
			area_id area = vm_map_physical_memory(VMAddressSpace::KernelID(),
				"vc4 buffer", &address,
				B_ANY_KERNEL_ADDRESS | B_WRITE_COMBINING_MEMORY, bo->size,
				B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, bo->physical, false);
			if (area < 0)
				return area;

			VMArea* vmArea = VMAreas::Lookup(area);
			if (vmArea == NULL) {
				delete_area(area);
				return B_ERROR;
			}
			VMCache* cache = vm_area_get_locked_cache(vmArea);
			cache->AcquireRefLocked();
			vm_area_put_locked_cache(cache);

			bo->mapSource = area;
			bo->mapCache = cache;
		}
		source = bo->mapSource;
	}

	// Shaders are validated once: their code must not change.
	uint32 protection = B_READ_AREA | B_KERNEL_AREA
		| (bo->validated_shader != NULL ? 0 : B_WRITE_AREA);
	void* address = NULL;
	area_id area = vm_clone_area(team, "vc4 buffer mapping", &address,
		B_ANY_ADDRESS, protection, REGION_NO_PRIVATE_MAP, source, true);
	if (area < 0)
		return area;

	bo->userArea = area;
	bo->userTeam = team;
	*_address = address;
	return B_OK;
}


static void
unmap_bo(vc4_bo* bo)
{
	if (bo->userArea < 0)
		return;
	vm_delete_area(bo->userTeam, bo->userArea, true);
	bo->userArea = -1;
}


//	#pragma mark - binner memory


/*!	A free slot, or -1 when no more than \a reserve are left. Device
	locked.
*/
static int
take_bin_slot(vc4_info* info, uint32 reserve = 0)
{
	uint32 freeSlots = ~info->binSlotsUsed
		& (uint32)(((uint64)1 << VC4_BIN_SLOTS) - 1);
	if ((uint32)__builtin_popcount(freeSlots) <= reserve)
		return -1;
	int slot = __builtin_ffs(freeSlots) - 1;
	info->binSlotsUsed |= BIT(slot);
	return slot;
}


static status_t wait_for_job(vc4_info* info, uint64 seqno,
	bigtime_t timeout, bool interruptible);


/*!	A slot for a job's tile state and lists (Linux: vc4_v3d_get_bin_slot()):
	waits for the jobs queued so far, whose slots are given back when they
	are done, while there is none. A few slots stay for the overflow
	memory of the job that runs: without it the binner stalls and the job
	is given up.
*/
int
vc4_get_bin_slot(vc4_info* info)
{
	while (true) {
		MutexLocker locker(info->lock);
		int slot = take_bin_slot(info, VC4_BIN_RESERVED_SLOTS);
		if (slot >= 0)
			return slot;
		uint64 seqno = info->submitted;
		bool idle = info->completed >= seqno;
		locker.Unlock();

		if (idle)
			return B_NO_MEMORY;
		status_t status = wait_for_job(info, seqno, B_INFINITE_TIMEOUT, true);
		if (status != B_OK)
			return status;
	}
}


uint32
vc4_bin_slot_address(vc4_info* info, int slot)
{
	return ((uint32)info->binPhysical | VC4_BUS_OFFSET)
		+ slot * VC4_BIN_SLOT_SIZE;
}


uint32
vc4_bin_alloc_size(vc4_info* info)
{
	return VC4_BIN_SLOT_SIZE;
}


//	#pragma mark - interrupts and jobs


/*!	Linux' vc4_irq(): acknowledges what came. Out of memory stays asserted
	until the binner has memory again, so it is switched off until then.
*/
static int32
vc4_interrupt(void* data)
{
	vc4_info* info = (vc4_info*)data;

	uint32 status = read32(info, V3D_INTCTL) & read32(info, V3D_INTENA)
		& VC4_DRIVER_INTERRUPTS;
	if (status == 0)
		return B_UNHANDLED_INTERRUPT;

	if ((status & V3D_INT_OUTOMEM) != 0)
		write32(info, V3D_INTDIS, V3D_INT_OUTOMEM);
	write32(info, V3D_INTCTL, status);

	atomic_add64(&info->interrupts, 1);
	atomic_or(&info->events, status);
	info->eventCondition.NotifyAll();
	return B_HANDLED_INTERRUPT;
}


/*!	Waits for one of the events in \a mask, \a timeout at most; returns the
	ones that came and takes them off the pending set.
*/
static uint32
wait_for_event(vc4_info* info, uint32 mask, bigtime_t timeout)
{
	bigtime_t deadline = system_time() + timeout;

	while (true) {
		ConditionVariableEntry entry;
		info->eventCondition.Add(&entry);

		uint32 events = atomic_get(&info->events) & mask;
		if (events != 0) {
			atomic_and(&info->events, ~events);
			return events;
		}

		if (entry.Wait(B_ABSOLUTE_TIMEOUT, deadline) == B_TIMED_OUT) {
			events = atomic_get(&info->events) & mask;
			atomic_and(&info->events, ~events);
			return events;
		}
	}
}


/*!	Linux' vc4_flush_caches(): the core's L2 cache and the slices' caches
	(texture units, uniforms, instructions), before a job reads what the CPU
	or an earlier job wrote.
*/
static void
flush_caches(vc4_info* info)
{
	write32(info, V3D_L2CACTL, V3D_L2CACTL_L2CCLR);
	write32(info, V3D_SLCACTL, VC4_SLCACTL_ALL);
}


/*!	Linux' vc4_flush_texture_caches(), before rendering: the binning may
	have read textures the job before rendered into.
*/
static void
flush_texture_caches(vc4_info* info)
{
	write32(info, V3D_L2CACTL, V3D_L2CACTL_L2CCLR);
	write32(info, V3D_SLCACTL, VC4_SLCACTL_TEXTURE);
}


/*!	Gives the binner a slot of overflow memory (Linux:
	vc4_overflow_mem_work()); it belongs to the job until it is done. Linux
	writes the whole bin buffer's size to BPOS; the binner may only have
	the slot.
*/
static status_t
give_overflow(vc4_info* info, vc4_exec_info* job)
{
	MutexLocker locker(info->lock);
	int slot = take_bin_slot(info);
	locker.Unlock();
	if (slot < 0) {
		ERROR("job %" B_PRIu64 ": no binner overflow memory left\n",
			job->seqno);
		return B_NO_MEMORY;
	}

	job->bin_slots |= BIT(slot);
	write32(info, V3D_BPOA, vc4_bin_slot_address(info, slot));
	write32(info, V3D_BPOS, VC4_BIN_SLOT_SIZE);
	write32(info, V3D_INTCTL, V3D_INT_OUTOMEM);
	write32(info, V3D_INTENA, V3D_INT_OUTOMEM);
	return B_OK;
}


/*!	Waits until control list thread \a thread is done (\a doneEvent),
	giving the binner memory when it asks. Gives up when the thread's
	address has not moved for a while (Linux: vc4_hangcheck_elapsed()).

	The core counts the binning flushes and rendered frames (BFC, RFC), with
	or without interrupts: when the count moved (from \a startCount) but the
	interrupt did not come, the interrupts do not get through, and the
	thread polls the core from then on.
*/
static status_t
wait_for_list(vc4_info* info, vc4_exec_info* job, uint32 thread,
	uint32 doneEvent, uint32 startCount)
{
	const uint32 counter = thread == 0 ? V3D_BFC : V3D_RFC;
	// only the binner asks for memory; a request that came after its
	// flush is not the renderer's
	const uint32 outOfMemory = thread == 0 ? V3D_INT_OUTOMEM : 0;
	uint32 lastAddress = read32(info, V3D_CTNCA(thread));
	bigtime_t lastProgress = system_time();

	while (true) {
		uint32 events = wait_for_event(info, doneEvent | outOfMemory,
			info->polling ? VC4_POLL_INTERVAL : VC4_CHECK_INTERVAL);
		if ((events & doneEvent) != 0) {
			info->missedInterrupts = 0;
			return B_OK;
		}
		if ((events & V3D_INT_OUTOMEM) != 0) {
			TRACE("job %" B_PRIu64 ": binner out of memory\n", job->seqno);
			// without memory the binner stalls, and the check below ends it
			give_overflow(info, job);
			continue;
		}

		if ((read32(info, counter) & 0xff) != startCount) {
			// the interrupt may just have come
			// A late interrupt can look the same once; three lists in a
			// row cannot.
			if ((atomic_and(&info->events, ~(int32)doneEvent) & doneEvent)
					!= 0) {
				info->missedInterrupts = 0;
			} else if (!info->polling && ++info->missedInterrupts >= 3) {
				ERROR("job %" B_PRIu64 " finished without its interrupt "
					"(%" B_PRId64 " so far): polling the core from now on\n",
					job->seqno, atomic_get64(&info->interrupts));
				info->polling = true;
			}
			return B_OK;
		}
		if (info->polling && thread == 0
			&& (read32(info, V3D_INTCTL) & read32(info, V3D_INTENA)
				& V3D_INT_OUTOMEM) != 0) {
			write32(info, V3D_INTDIS, V3D_INT_OUTOMEM);
			give_overflow(info, job);
			continue;
		}

		bigtime_t now = system_time();
		uint32 address = read32(info, V3D_CTNCA(thread));
		if (address != lastAddress) {
			lastAddress = address;
			lastProgress = now;
		} else if (now - lastProgress >= VC4_STALL_TIMEOUT)
			return B_TIMED_OUT;
	}
}


static status_t
run_job(vc4_info* info, vc4_exec_info* job)
{
	// nothing left over from the job before
	write32(info, V3D_INTCTL, V3D_INT_FLDONE | V3D_INT_FRDONE);
	atomic_and(&info->events, ~(int32)VC4_DRIVER_INTERRUPTS);

	// what the CPU wrote into the buffers reaches memory first
	memory_full_barrier();

	if (job->ct0ca != job->ct0ea) {
		flush_caches(info);
		status_t status = give_overflow(info, job);
		if (status != B_OK)
			return status;

		// writing the end address starts the thread
		uint32 flushes = read32(info, V3D_BFC) & 0xff;
		write32(info, V3D_CTNCA(0), job->ct0ca);
		write32(info, V3D_CTNEA(0), job->ct0ea);

		status = wait_for_list(info, job, 0, V3D_INT_FLDONE, flushes);
		write32(info, V3D_INTDIS, V3D_INT_OUTOMEM);
		if (status != B_OK) {
			ERROR("job %" B_PRIu64 ": the binner did not finish\n",
				job->seqno);
			return status;
		}
	}

	flush_texture_caches(info);
	uint32 frames = read32(info, V3D_RFC) & 0xff;
	write32(info, V3D_CTNCA(1), job->ct1ca);
	write32(info, V3D_CTNEA(1), job->ct1ea);

	status_t status = wait_for_list(info, job, 1, V3D_INT_FRDONE, frames);
	if (status != B_OK) {
		ERROR("job %" B_PRIu64 ": the render list did not finish\n",
			job->seqno);
	}
	return status;
}


/*!	The registers GET_HANG_STATE returns (Linux: vc4_save_hang_state()),
	and the kernel log.
*/
static void
save_hang_state(vc4_info* info, vc4_exec_info* job)
{
	drm_vc4_get_hang_state& state = info->hangState;
	memset(&state, 0, sizeof(state));
	state.start_bin = job->ct0ca;
	state.start_render = job->ct1ca;
	state.ct0ca = read32(info, V3D_CT0CA);
	state.ct0ea = read32(info, V3D_CT0EA);
	state.ct1ca = read32(info, V3D_CT1CA);
	state.ct1ea = read32(info, V3D_CT1EA);
	state.ct0cs = read32(info, V3D_CT0CS);
	state.ct1cs = read32(info, V3D_CT1CS);
	state.ct0ra0 = read32(info, V3D_CT00RA0);
	state.ct1ra0 = read32(info, V3D_CT01RA0);
	state.bpca = read32(info, V3D_BPCA);
	state.bpcs = read32(info, V3D_BPCS);
	state.bpoa = read32(info, V3D_BPOA);
	state.bpos = read32(info, V3D_BPOS);
	state.vpmbase = read32(info, V3D_VPMBASE);
	state.dbge = read32(info, V3D_DBGE);
	state.fdbgo = read32(info, V3D_FDBGO);
	state.fdbgb = read32(info, V3D_FDBGB);
	state.fdbgr = read32(info, V3D_FDBGR);
	state.fdbgs = read32(info, V3D_FDBGS);
	state.errstat = read32(info, V3D_ERRSTAT);
	info->hangStateValid = true;

	ERROR("job %" B_PRIu64 " hung: bin %#" B_PRIx32 " at %#" B_PRIx32 "-%#"
		B_PRIx32 " (status %#" B_PRIx32 "), render %#" B_PRIx32 " at %#"
		B_PRIx32 "-%#" B_PRIx32 " (status %#" B_PRIx32 "), pool %#" B_PRIx32
		"+%#" B_PRIx32 ", overflow %#" B_PRIx32 "+%#" B_PRIx32 ", PCS %#"
		B_PRIx32 ", interrupts %#" B_PRIx32 "/%#" B_PRIx32 ", errors %#"
		B_PRIx32 ", FDBGO %#" B_PRIx32 " FDBGB %#" B_PRIx32 " FDBGR %#"
		B_PRIx32 " FDBGS %#" B_PRIx32 "\n", job->seqno, state.start_bin,
		state.ct0ca, state.ct0ea, state.ct0cs, state.start_render, state.ct1ca,
		state.ct1ea, state.ct1cs, state.bpca, state.bpcs, state.bpoa,
		state.bpos, read32(info, V3D_PCS), read32(info, V3D_INTCTL),
		read32(info, V3D_INTENA), state.errstat, state.fdbgo, state.fdbgb,
		state.fdbgr, state.fdbgs);
}


/*!	After a job that did not finish (Linux: vc4_reset()): the control list
	threads stopped, the core powered off and on again.
*/
static void
reset_hardware(vc4_info* info)
{
	write32(info, V3D_INTDIS, 0xffffffff);
	write32(info, V3D_CT0CS, V3D_CTRSTA);
	write32(info, V3D_CT1CS, V3D_CTRSTA);

	power_off(info);
	snooze(1000);
	power_on(info);

	uint32 ident0 = read32(info, V3D_IDENT0);
	if (ident0 != V3D_EXPECTED_IDENT0)
		ERROR("the core is not back after the reset: IDENT0 %#" B_PRIx32 "\n",
			ident0);

	init_hardware(info);
	atomic_set(&info->events, 0);
	info->resets++;
}


void
vc4_exec_cleanup(vc4_info* info, vc4_exec_info* exec)
{
	MutexLocker locker(info->lock);
	if (exec->bo != NULL) {
		for (uint32 i = 0; i < exec->bo_count; i++) {
			if (exec->bo[i] != NULL)
				put_bo(info, exec->bo[i]);
		}
	}
	while (exec->unref_list != NULL) {
		vc4_bo* bo = exec->unref_list;
		exec->unref_list = bo->unref_next;
		put_bo(info, bo);
	}
	info->binSlotsUsed &= ~exec->bin_slots;
	locker.Unlock();

	free(exec->bo);
	free(exec);
}


static status_t
executor_thread(void* data)
{
	vc4_info* info = (vc4_info*)data;

	while (true) {
		if (acquire_sem(info->jobSemaphore) != B_OK || info->stopping)
			break;

		MutexLocker locker(info->lock);
		vc4_exec_info* job = info->firstJob;
		if (job == NULL)
			continue;
		info->firstJob = job->next;
		if (info->firstJob == NULL)
			info->lastJob = NULL;
		locker.Unlock();

		TRACE("job %" B_PRIu64 ": bin %#" B_PRIx32 "-%#" B_PRIx32 ", render %#"
			B_PRIx32 "-%#" B_PRIx32 "\n", job->seqno, job->ct0ca, job->ct0ea,
			job->ct1ca, job->ct1ea);

		status_t status = run_job(info, job);
		if (status != B_OK) {
			save_hang_state(info, job);
			reset_hardware(info);
		}

		uint64 seqno = job->seqno;
		// its buffers and bin slots back first: waiters for the job may
		// want them
		vc4_exec_cleanup(info, job);

		locker.Lock();
		info->completed = seqno;
		locker.Unlock();
		info->completedCondition.NotifyAll();
	}

	return B_OK;
}


/*!	Waits until job \a seqno is done, \a timeout (relative) at most. */
static status_t
wait_for_job(vc4_info* info, uint64 seqno, bigtime_t timeout,
	bool interruptible)
{
	bigtime_t deadline = timeout == B_INFINITE_TIMEOUT
		? B_INFINITE_TIMEOUT : system_time() + timeout;

	while (true) {
		ConditionVariableEntry entry;
		info->completedCondition.Add(&entry);

		mutex_lock(&info->lock);
		bool done = info->completed >= seqno;
		mutex_unlock(&info->lock);
		if (done)
			return B_OK;
		if (timeout == 0)
			return B_TIMED_OUT;

		uint32 flags = interruptible ? B_CAN_INTERRUPT : 0;
		status_t status = deadline == B_INFINITE_TIMEOUT
			? entry.Wait(flags)
			: entry.Wait(B_ABSOLUTE_TIMEOUT | flags, deadline);
		if (status == B_TIMED_OUT || status == B_INTERRUPTED)
			return status;
	}
}


/*!	A wait request's timeout (nanoseconds, ~0 for ever) in microseconds. */
static bigtime_t
request_timeout(uint64 timeout)
{
	if (timeout == ~(uint64)0 || timeout / 1000 >= (uint64)B_INFINITE_TIMEOUT)
		return B_INFINITE_TIMEOUT;
	return (bigtime_t)((timeout + 999) / 1000);
}


/*!	Waits as a request asks; an interrupted wait leaves what is left of the
	timeout in it, as Linux' vc4_wait_for_seqno_ioctl_helper() does.
*/
static status_t
wait_for_request(vc4_info* info, uint64 seqno, uint64& timeout)
{
	bigtime_t start = system_time();
	status_t status = wait_for_job(info, seqno, request_timeout(timeout),
		true);
	if (status == B_INTERRUPTED && timeout != ~(uint64)0) {
		uint64 elapsed = (uint64)(system_time() - start) * 1000;
		timeout = timeout > elapsed ? timeout - elapsed : 0;
	}
	return status;
}


//	#pragma mark - handles


static vc4_bo*
lookup_bo(vc4_file* file, uint32 handle)
{
	if (handle == 0 || handle > file->bufferCount)
		return NULL;
	return file->buffers[handle - 1];
}


/*!	A free handle for \a bo, growing the table as needed. Device locked. */
static status_t
add_handle(vc4_file* file, vc4_bo* bo, uint32& _handle)
{
	uint32 index = 0;
	while (index < file->bufferCount && file->buffers[index] != NULL)
		index++;
	if (index == file->bufferCount) {
		uint32 count = std::max(file->bufferCount * 2, (uint32)64);
		vc4_bo** table = (vc4_bo**)realloc(file->buffers,
			count * sizeof(vc4_bo*));
		if (table == NULL)
			return B_NO_MEMORY;
		memset(table + file->bufferCount, 0,
			(count - file->bufferCount) * sizeof(vc4_bo*));
		file->buffers = table;
		file->bufferCount = count;
	}

	file->buffers[index] = bo;
	_handle = index + 1;
	return B_OK;
}


//	#pragma mark - requests


template<typename Type>
static status_t
copy_in(Type& request, void* buffer, size_t length)
{
	if (buffer == NULL || !IS_USER_ADDRESS(buffer))
		return B_BAD_ADDRESS;
	return user_memcpy(&request, buffer, sizeof(Type));
}


template<typename Type>
static status_t
copy_out(void* buffer, const Type& request)
{
	return user_memcpy(buffer, &request, sizeof(Type));
}


static status_t
get_param(vc4_info* info, drm_vc4_get_param& request)
{
	if (request.pad != 0)
		return B_BAD_VALUE;

	switch (request.param) {
		case DRM_VC4_PARAM_V3D_IDENT0:
		case DRM_VC4_PARAM_V3D_IDENT1:
		case DRM_VC4_PARAM_V3D_IDENT2:
			// read at start: the registers are not there during a reset
			request.value = info->ident[request.param
				- DRM_VC4_PARAM_V3D_IDENT0];
			return B_OK;
		case DRM_VC4_PARAM_SUPPORTS_BRANCHES:
		case DRM_VC4_PARAM_SUPPORTS_ETC1:
		case DRM_VC4_PARAM_SUPPORTS_THREADED_FS:
		case DRM_VC4_PARAM_SUPPORTS_FIXED_RCL_ORDER:
			request.value = 1;
			return B_OK;
		case DRM_VC4_PARAM_SUPPORTS_MADVISE:
		case DRM_VC4_PARAM_SUPPORTS_PERFMON:
			request.value = 0;
			return B_OK;
		default:
			return B_BAD_VALUE;
	}
}


/*!	Takes a reference to each buffer of the job (Linux: vc4_cl_lookup_bos()).
*/
static status_t
lookup_job_bos(vc4_file* file, vc4_exec_info* exec)
{
	vc4_info* info = file->device;
	drm_vc4_submit_cl* args = exec->args;

	exec->bo_count = args->bo_handle_count;
	if (exec->bo_count == 0) {
		// See comment on bo_index for why we have to check this.
		DRM_DEBUG("Rendering requires BOs to validate\n");
		return B_BAD_VALUE;
	}
	if (exec->bo_count > VC4_MAX_JOB_BOS)
		return B_BAD_VALUE;

	uint32* handles = (uint32*)malloc(exec->bo_count * sizeof(uint32));
	exec->bo = (vc4_bo**)calloc(exec->bo_count, sizeof(vc4_bo*));
	MemoryDeleter handlesDeleter(handles);
	if (handles == NULL || exec->bo == NULL)
		return B_NO_MEMORY;

	if (!IS_USER_ADDRESS((addr_t)args->bo_handles)
		|| user_memcpy(handles, (void*)(addr_t)args->bo_handles,
			exec->bo_count * sizeof(uint32)) != B_OK) {
		return B_BAD_ADDRESS;
	}

	MutexLocker locker(info->lock);
	for (uint32 i = 0; i < exec->bo_count; i++) {
		vc4_bo* bo = lookup_bo(file, handles[i]);
		if (bo == NULL) {
			DRM_DEBUG("Failed to look up GEM BO %d: %d\n", i, handles[i]);
			return B_BAD_VALUE;
		}
		bo->references++;
		exec->bo[i] = bo;
	}
	return B_OK;
}


/*!	Linux' vc4_submit_cl_ioctl(): validates the job and queues it. */
static status_t
submit_cl(vc4_file* file, drm_vc4_submit_cl& args)
{
	vc4_info* info = file->device;

	if ((args.flags & ~(VC4_SUBMIT_CL_USE_CLEAR_COLOR
			| VC4_SUBMIT_CL_FIXED_RCL_ORDER
			| VC4_SUBMIT_CL_RCL_ORDER_INCREASING_X
			| VC4_SUBMIT_CL_RCL_ORDER_INCREASING_Y)) != 0) {
		DRM_DEBUG("Unknown flags: 0x%02x\n", args.flags);
		return B_BAD_VALUE;
	}
	if (args.pad2 != 0) {
		DRM_DEBUG("Invalid pad: 0x%08x\n", args.pad2);
		return B_BAD_VALUE;
	}
	// no performance monitors, no sync objects
	if (args.perfmonid != 0 || args.in_sync != 0 || args.out_sync != 0)
		return B_NOT_SUPPORTED;

	vc4_exec_info* exec = (vc4_exec_info*)calloc(1, sizeof(vc4_exec_info));
	if (exec == NULL)
		return B_NO_MEMORY;
	exec->dev = info;
	exec->args = &args;

	status_t status = lookup_job_bos(file, exec);
	if (status == B_OK && args.bin_cl_size != 0)
		status = vc4_get_bcl(info, exec);
	if (status == B_OK)
		status = vc4_get_rcl(info, exec);
	exec->args = NULL;
	if (status != B_OK) {
		vc4_exec_cleanup(info, exec);
		return status;
	}

	// the validated lists reach memory before the job is started
	memory_full_barrier();

	MutexLocker locker(info->lock);
	exec->seqno = ++info->submitted;
	for (uint32 i = 0; i < exec->bo_count; i++)
		exec->bo[i]->seqno = exec->seqno;
	for (vc4_bo* bo = exec->unref_list; bo != NULL; bo = bo->unref_next)
		bo->seqno = exec->seqno;
	for (uint32 i = 0; i < exec->rcl_write_bo_count; i++)
		exec->rcl_write_bo[i]->write_seqno = exec->seqno;

	exec->next = NULL;
	if (info->lastJob != NULL)
		info->lastJob->next = exec;
	else
		info->firstJob = exec;
	info->lastJob = exec;
	args.seqno = exec->seqno;
	locker.Unlock();

	release_sem(info->jobSemaphore);
	return B_OK;
}


static status_t
create_shader_bo(vc4_file* file, drm_vc4_create_shader_bo& request)
{
	vc4_info* info = file->device;

	if (request.size == 0 || request.size % sizeof(uint64) != 0
		|| request.size > VC4_MAX_SHADER_SIZE || request.flags != 0
		|| request.pad != 0) {
		return B_BAD_VALUE;
	}

	// validated in the kernel's copy, which is what the core then runs
	uint64* code = (uint64*)malloc(request.size);
	if (code == NULL)
		return B_NO_MEMORY;
	MemoryDeleter codeDeleter(code);
	if (!IS_USER_ADDRESS((addr_t)request.data)
		|| user_memcpy(code, (void*)(addr_t)request.data, request.size)
			!= B_OK) {
		return B_BAD_ADDRESS;
	}

	vc4_validated_shader_info* validated = vc4_validate_shader(code,
		request.size);
	if (validated == NULL)
		return B_BAD_VALUE;

	vc4_bo* bo;
	status_t status = create_bo(info, request.size, &bo);
	if (status != B_OK) {
		vc4_free_validated_shader(validated);
		return status;
	}
	memcpy(bo->vaddr, code, request.size);
	memory_full_barrier();
	bo->validated_shader = validated;
	bo->user = true;

	MutexLocker locker(info->lock);
	status = add_handle(file, bo, request.handle);
	if (status != B_OK)
		put_bo(info, bo);
	return status;
}


static status_t
vc4_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	vc4_file* file = (vc4_file*)cookie;
	vc4_info* info = file->device;

	switch (op) {
		case VC4_HAIKU_GET_PARAM:
		{
			drm_vc4_get_param request;
			status_t status = copy_in(request, buffer, length);
			if (status == B_OK)
				status = get_param(info, request);
			if (status != B_OK)
				return status;
			return copy_out(buffer, request);
		}

		case VC4_HAIKU_CREATE_BO:
		{
			drm_vc4_create_bo request;
			status_t status = copy_in(request, buffer, length);
			if (status != B_OK)
				return status;

			vc4_bo* bo;
			status = create_bo(info, request.size, &bo);
			if (status != B_OK)
				return status;
			bo->user = true;

			MutexLocker locker(info->lock);
			status = add_handle(file, bo, request.handle);
			if (status != B_OK) {
				put_bo(info, bo);
				return status;
			}
			status = copy_out(buffer, request);
			if (status != B_OK) {
				file->buffers[request.handle - 1] = NULL;
				put_bo(info, bo);
			}
			return status;
		}

		case VC4_HAIKU_CREATE_SHADER_BO:
		{
			drm_vc4_create_shader_bo request;
			status_t status = copy_in(request, buffer, length);
			if (status == B_OK)
				status = create_shader_bo(file, request);
			if (status != B_OK)
				return status;

			status = copy_out(buffer, request);
			if (status != B_OK) {
				MutexLocker locker(info->lock);
				vc4_bo* bo = lookup_bo(file, request.handle);
				if (bo != NULL) {
					file->buffers[request.handle - 1] = NULL;
					put_bo(info, bo);
				}
			}
			return status;
		}

		case VC4_HAIKU_MMAP_BO:
		{
			drm_vc4_mmap_bo request;
			status_t status = copy_in(request, buffer, length);
			if (status != B_OK)
				return status;
			if (request.flags != 0)
				return B_BAD_VALUE;

			MutexLocker locker(info->lock);
			vc4_bo* bo = lookup_bo(file, request.handle);
			if (bo == NULL)
				return B_BAD_VALUE;
			void* address;
			status = map_bo(info, bo, &address);
			if (status != B_OK)
				return status;
			locker.Unlock();

			request.offset = (addr_t)address;
			return copy_out(buffer, request);
		}

		case VC4_HAIKU_CLOSE_BO:
		{
			vc4_haiku_handle request;
			status_t status = copy_in(request, buffer, length);
			if (status != B_OK)
				return status;

			MutexLocker locker(info->lock);
			vc4_bo* bo = lookup_bo(file, request.handle);
			if (bo == NULL)
				return B_BAD_VALUE;
			file->buffers[request.handle - 1] = NULL;
			unmap_bo(bo);
			put_bo(info, bo);
			return B_OK;
		}

		case VC4_HAIKU_SUBMIT_CL:
		{
			drm_vc4_submit_cl request;
			status_t status = copy_in(request, buffer, length);
			if (status == B_OK)
				status = submit_cl(file, request);
			if (status != B_OK)
				return status;
			// the job is queued whatever happens to the answer
			return copy_out(buffer, request);
		}

		case VC4_HAIKU_WAIT_SEQNO:
		{
			drm_vc4_wait_seqno request;
			status_t status = copy_in(request, buffer, length);
			if (status != B_OK)
				return status;

			status = wait_for_request(info, request.seqno, request.timeout_ns);
			if (status == B_INTERRUPTED)
				copy_out(buffer, request);
			return status;
		}

		case VC4_HAIKU_WAIT_BO:
		{
			drm_vc4_wait_bo request;
			status_t status = copy_in(request, buffer, length);
			if (status != B_OK)
				return status;
			if (request.pad != 0)
				return B_BAD_VALUE;

			MutexLocker locker(info->lock);
			vc4_bo* bo = lookup_bo(file, request.handle);
			if (bo == NULL)
				return B_BAD_VALUE;
			uint64 seqno = bo->seqno;
			locker.Unlock();

			status = wait_for_request(info, seqno, request.timeout_ns);
			if (status == B_INTERRUPTED)
				copy_out(buffer, request);
			return status;
		}

		case VC4_HAIKU_GET_HANG_STATE:
		{
			drm_vc4_get_hang_state request;
			status_t status = copy_in(request, buffer, length);
			if (status != B_OK)
				return status;

			MutexLocker locker(info->lock);
			if (!info->hangStateValid)
				return B_ENTRY_NOT_FOUND;
			drm_vc4_get_hang_state state = info->hangState;
			info->hangStateValid = false;
			locker.Unlock();

			// the registers only, no buffers
			state.bo = request.bo;
			state.bo_count = 0;
			return copy_out(buffer, state);
		}

		case VC4_HAIKU_SET_TILING:
		{
			drm_vc4_set_tiling request;
			status_t status = copy_in(request, buffer, length);
			if (status != B_OK)
				return status;
			if (request.flags != 0 || (request.modifier != VC4_HAIKU_MOD_LINEAR
					&& request.modifier != VC4_HAIKU_MOD_T_TILED)) {
				return B_BAD_VALUE;
			}

			MutexLocker locker(info->lock);
			vc4_bo* bo = lookup_bo(file, request.handle);
			if (bo == NULL)
				return B_ENTRY_NOT_FOUND;
			bo->t_format = request.modifier == VC4_HAIKU_MOD_T_TILED;
			return B_OK;
		}

		case VC4_HAIKU_GET_TILING:
		{
			drm_vc4_get_tiling request;
			status_t status = copy_in(request, buffer, length);
			if (status != B_OK)
				return status;
			if (request.flags != 0 || request.modifier != 0)
				return B_BAD_VALUE;

			MutexLocker locker(info->lock);
			vc4_bo* bo = lookup_bo(file, request.handle);
			if (bo == NULL)
				return B_ENTRY_NOT_FOUND;
			request.modifier = bo->t_format
				? VC4_HAIKU_MOD_T_TILED : VC4_HAIKU_MOD_LINEAR;
			locker.Unlock();
			return copy_out(buffer, request);
		}

		case VC4_HAIKU_LABEL_BO:
		{
			// names are for Linux' debugfs; there is none
			drm_vc4_label_bo request;
			status_t status = copy_in(request, buffer, length);
			if (status != B_OK)
				return status;

			MutexLocker locker(info->lock);
			return lookup_bo(file, request.handle) != NULL
				? B_OK : B_ENTRY_NOT_FOUND;
		}

		case VC4_HAIKU_GEM_MADVISE:
		{
			// Recorded, never acted upon: nothing is purged.
			drm_vc4_gem_madvise request;
			status_t status = copy_in(request, buffer, length);
			if (status != B_OK)
				return status;
			if ((request.madv != VC4_MADV_WILLNEED
					&& request.madv != VC4_MADV_DONTNEED)
				|| request.pad != 0) {
				return B_BAD_VALUE;
			}

			MutexLocker locker(info->lock);
			vc4_bo* bo = lookup_bo(file, request.handle);
			if (bo == NULL)
				return B_ENTRY_NOT_FOUND;
			bo->madv = request.madv;
			request.retained = 1;
			locker.Unlock();
			return copy_out(buffer, request);
		}

		case VC4_HAIKU_GET_INFO:
		{
			vc4_haiku_info request = {};
			MutexLocker locker(info->lock);
			memcpy(request.ident, info->ident, sizeof(request.ident));
			request.clock_rate = info->clockRate;
			request.pool_size = info->poolSize;
			request.pool_free = info->poolSize - info->poolUsed;
			request.contiguous_bytes = info->contiguousBytes;
			request.bin_size = info->binArea >= 0 ? VC4_BIN_SIZE : 0;
			request.bin_slots_used = info->binSlotsUsed;
			request.submitted = info->submitted;
			request.completed = info->completed;
			request.resets = info->resets;
			request.interrupt = info->interrupt;
			request.interrupts = atomic_get64(&info->interrupts);
			request.polling = info->polling ? 1 : 0;
			locker.Unlock();

			if (buffer == NULL || !IS_USER_ADDRESS(buffer))
				return B_BAD_ADDRESS;
			return copy_out(buffer, request);
		}
	}

	// Not a display driver: app_server asks every device under graphics/
	// for an accelerant and has to be turned away.
	return B_DEV_INVALID_IOCTL;
}


//	#pragma mark - device


static status_t
initialize_device(vc4_info* info, uint32 poolMB)
{
	info->regsArea = info->poolArea = info->binArea = -1;
	info->jobSemaphore = -1;
	info->executor = -1;
	mutex_init(&info->lock, "vc4");
	info->completedCondition.Init(info, "vc4 job done");
	info->eventCondition.Init(info, "vc4 event");

	uint64 offset = info->regsBase & (B_PAGE_SIZE - 1);
	void* mapped;
	info->regsArea = map_physical_memory("vc4 registers",
		info->regsBase - offset, ROUNDUP(info->regsSize + offset, B_PAGE_SIZE),
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		&mapped);
	if (info->regsArea < 0)
		return info->regsArea;
	info->regs = (volatile uint8*)mapped + offset;

	status_t status = power_on(info);
	if (status != B_OK)
		return status;

	for (int i = 0; i < 3; i++)
		info->ident[i] = read32(info, V3D_IDENT0 + 4 * i);
	if (info->ident[0] != V3D_EXPECTED_IDENT0) {
		ERROR("no V3D 2 behind the registers at %#" B_PRIx64 ": IDENT0 %#"
			B_PRIx32 " instead of %#" B_PRIx32 "\n", info->regsBase,
			info->ident[0], (uint32)V3D_EXPECTED_IDENT0);
		return B_DEVICE_NOT_FOUND;
	}

	// The binner's memory, then the pool; at boot, while there are still
	// long runs of free pages.
	uint8* binAddress;
	info->binArea = create_contiguous_area("vc4 binner memory", VC4_BIN_SIZE,
		VC4_BIN_BOUNDARY, &binAddress, &info->binPhysical);
	if (info->binArea < 0) {
		ERROR("no %d MB of contiguous memory for the binner: %s\n",
			VC4_BIN_SIZE >> 20, strerror(info->binArea));
		return info->binArea;
	}

	if (poolMB != 0)
		poolMB = std::max(poolMB, (uint32)VC4_MIN_POOL_MB);
	for (uint32 size = poolMB; size >= VC4_MIN_POOL_MB; size /= 2) {
		info->poolArea = create_contiguous_area("vc4 buffer pool",
			(size_t)size << 20, 0, &info->pool, &info->poolPhysical);
		if (info->poolArea >= 0) {
			info->poolSize = size << 20;
			break;
		}
	}
	if (poolMB != 0 && info->poolArea < 0) {
		ERROR("no contiguous pool: every buffer will need a contiguous area "
			"of its own\n");
		info->pool = NULL;
	}

	info->jobSemaphore = create_sem(0, "vc4 jobs");
	if (info->jobSemaphore < 0)
		return info->jobSemaphore;
	info->stopping = false;

	write32(info, V3D_INTDIS, 0xffffffff);
	write32(info, V3D_INTCTL, 0xffffffff);
	status = install_io_interrupt_handler(info->interrupt, vc4_interrupt, info,
		0);
	if (status != B_OK)
		return status;
	info->interruptInstalled = true;

	init_hardware(info);

	info->executor = spawn_kernel_thread(executor_thread, "vc4 executor",
		B_DISPLAY_PRIORITY, info);
	if (info->executor < 0)
		return info->executor;
	resume_thread(info->executor);

	uint32 ident1 = info->ident[1];
	// the VPM size is in KB, 0 for 16
	uint32 vpmSize = VC4_GET_FIELD(ident1, V3D_IDENT1_VPM_SIZE);
	INFO("V3D %" B_PRIu32 ".%" B_PRIu32 ": %" B_PRIu32 " slice(s) of %"
		B_PRIu32 " QPUs and %" B_PRIu32 " texture units, %" B_PRIu32 " KB VPM, %"
		B_PRIu32 " semaphores; clock %" B_PRIu32 " Hz, interrupt %" B_PRIu32
		"\n", info->ident[0] >> 24, VC4_GET_FIELD(ident1, V3D_IDENT1_REV),
		VC4_GET_FIELD(ident1, V3D_IDENT1_NSLC),
		VC4_GET_FIELD(ident1, V3D_IDENT1_QUPS),
		VC4_GET_FIELD(ident1, V3D_IDENT1_TUPS),
		vpmSize != 0 ? vpmSize : 16,
		VC4_GET_FIELD(ident1, V3D_IDENT1_NSEM), info->clockRate,
		info->interrupt);
	INFO("binner memory at %#" B_PRIxPHYSADDR ", buffer pool of %" B_PRIu32
		" MB at %#" B_PRIxPHYSADDR "\n", info->binPhysical,
		info->poolSize >> 20, info->poolPhysical);
	return B_OK;
}


static void
uninitialize_device(vc4_info* info)
{
	info->stopping = true;
	if (info->jobSemaphore >= 0)
		delete_sem(info->jobSemaphore);
	if (info->executor >= 0) {
		status_t result;
		wait_for_thread(info->executor, &result);
	}

	if (info->interruptInstalled) {
		write32(info, V3D_INTDIS, 0xffffffff);
		write32(info, V3D_INTCTL, 0xffffffff);
		remove_io_interrupt_handler(info->interrupt, vc4_interrupt, info);
		info->interruptInstalled = false;
	}
	if (info->regs != NULL) {
		// the next one to start the core must not find our memory
		write32(info, V3D_BPOA, 0);
		write32(info, V3D_BPOS, 0);
		power_off(info);
	}

	const area_id areas[] = {info->poolArea, info->binArea, info->regsArea};
	for (area_id area : areas) {
		if (area >= 0)
			delete_area(area);
	}
	mutex_destroy(&info->lock);
}


static status_t
vc4_init_device(void* cookie, void** _cookie)
{
	*_cookie = cookie;
	return B_OK;
}


static status_t
vc4_open(void* _info, const char* path, int openMode, void** _cookie)
{
	vc4_file* file = (vc4_file*)calloc(1, sizeof(vc4_file));
	if (file == NULL)
		return B_NO_MEMORY;

	file->device = (vc4_info*)_info;
	*_cookie = file;
	return B_OK;
}


static status_t
vc4_close(void* cookie)
{
	return B_OK;
}


static status_t
vc4_free(void* cookie)
{
	vc4_file* file = (vc4_file*)cookie;
	vc4_info* info = file->device;

	// Buffers that queued jobs still use live on until those are done.
	MutexLocker locker(info->lock);
	for (uint32 i = 0; i < file->bufferCount; i++) {
		vc4_bo* bo = file->buffers[i];
		if (bo == NULL)
			continue;
		unmap_bo(bo);
		put_bo(info, bo);
	}
	locker.Unlock();

	free(file->buffers);
	free(file);
	return B_OK;
}


//	#pragma mark - driver


static float
vc4_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "fdt") != 0) {
		return 0.0f;
	}

	const char* compatible;
	if (sDeviceManager->get_attr_string(parent, "fdt/compatible", &compatible,
			false) != B_OK
		|| (strcmp(compatible, "brcm,bcm2835-v3d") != 0
			&& strcmp(compatible, "brcm,vc4-v3d") != 0)) {
		return 0.0f;
	}

	return 1.0f;
}


static status_t
vc4_register_device(device_node* parent)
{
	device_attr attrs[] = {
		{B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{.string = "Broadcom VideoCore IV 3D"}},
		{B_DEVICE_FLAGS, B_UINT32_TYPE, {.ui32 = B_KEEP_DRIVER_LOADED}},
		{}
	};

	return sDeviceManager->register_node(parent, VC4_DRIVER_MODULE_NAME, attrs,
		NULL, NULL);
}


static status_t
vc4_init_driver(device_node* node, void** _cookie)
{
	device_node* parent = sDeviceManager->get_parent_node(node);
	fdt_device_module_info* fdt;
	fdt_device* device;
	status_t status = sDeviceManager->get_driver(parent,
		(driver_module_info**)&fdt, (void**)&device);
	sDeviceManager->put_node(parent);
	if (status != B_OK)
		return status;

	vc4_info* info = (vc4_info*)calloc(1, sizeof(vc4_info));
	if (info == NULL)
		return B_NO_MEMORY;
	info->node = node;

	uint64 interrupt;
	if (!fdt->get_reg(device, 0, &info->regsBase, &info->regsSize)
		|| !fdt->get_interrupt(device, 0, NULL, &interrupt)) {
		free(info);
		return B_BAD_DATA;
	}
	info->interrupt = interrupt;

	// Settings (kernel/drivers/vc4): "pool_size" in MB (0: none, a
	// contiguous area for each buffer), "debug" for the validators' reasons
	// to refuse a job.
	uint32 poolMB = VC4_DEFAULT_POOL_MB;
	void* settings = load_driver_settings("vc4");
	if (settings != NULL) {
		const char* value = get_driver_parameter(settings, "pool_size", NULL,
			NULL);
		if (value != NULL)
			poolMB = std::min(strtoul(value, NULL, 0), (unsigned long)512);
		gVc4Debug = get_driver_boolean_parameter(settings, "debug", true,
			true);
		unload_driver_settings(settings);
	}

	// Set up once, at boot: the contiguous memory is to be had then.
	status = initialize_device(info, poolMB);
	if (status != B_OK) {
		uninitialize_device(info);
		free(info);
		return status;
	}

	*_cookie = info;
	return B_OK;
}


static void
vc4_uninit_driver(void* cookie)
{
	vc4_info* info = (vc4_info*)cookie;
	uninitialize_device(info);
	free(info);
}


static status_t
vc4_register_child_devices(void* cookie)
{
	vc4_info* info = (vc4_info*)cookie;
	return sDeviceManager->publish_device(info->node, "graphics/vc4/0",
		VC4_DEVICE_MODULE_NAME);
}


module_dependency module_dependencies[] = {
	{B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager},
	{RPI_FIRMWARE_MODULE_NAME, (module_info**)&sFirmware},
	{}
};

static device_module_info sVc4Device = {
	{
		VC4_DEVICE_MODULE_NAME,
		0,
		NULL
	},
	vc4_init_device,
	NULL,	// hardware state lives with the retained driver node
	NULL,	// removed
	vc4_open,
	vc4_close,
	vc4_free,
	NULL,	// read
	NULL,	// write
	NULL,	// io
	vc4_control,
	NULL,	// select
	NULL,	// deselect
};

static driver_module_info sVc4Driver = {
	{
		VC4_DRIVER_MODULE_NAME,
		0,
		NULL
	},
	vc4_supports_device,
	vc4_register_device,
	vc4_init_driver,
	vc4_uninit_driver,
	vc4_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};

module_info* modules[] = {
	(module_info*)&sVc4Driver,
	(module_info*)&sVc4Device,
	NULL
};
