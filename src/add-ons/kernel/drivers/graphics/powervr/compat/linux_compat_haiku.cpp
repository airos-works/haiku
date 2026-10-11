/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*	The part of the Linux API (lx_haiku.h) that needs Haiku's C++ kernel
	API: wait queues on a ConditionVariable, GPU-visible memory on areas,
	single pages with vmap(), and firmware files.

	The GPU is not cache coherent and has no IOMMU. Everything it sees is
	mapped Normal-NC (B_WRITE_COMBINING_MEMORY) after the pages were zeroed
	through their cacheable mapping and cleaned and invalidated to the point
	of coherency ("dc civac"), as mali_csf does; afterwards they are only
	touched through the Normal-NC mapping. */


#include "lx_haiku.h"

#include <fcntl.h>
#include <new>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

#include <FindDirectory.h>
#include <KernelExport.h>

#include <arch/arm64/cache_poc.h>
#include <condition_variable.h>
#include <kernel.h>
#include <team.h>
#include <util/iovec_support.h>
#include <vm/VMArea.h>
#include <vm/VMCache.h>
#include <vm/vm.h>
#include <vm/vm_page.h>


#define TRACE(x...)		dprintf("powervr: " x)

#define MAX_FIRMWARE_SIZE	(4 * 1024 * 1024)


static_assert(sizeof(ConditionVariable)
	<= sizeof(((wait_queue_head_t*)NULL)->storage),
	"wait_queue_head_t has no room for a ConditionVariable");
static_assert(alignof(ConditionVariable) <= alignof(unsigned long long),
	"wait_queue_head_t storage is not aligned enough");


//	#pragma mark - errors and areas


status_t
lx_status(int error)
{
	switch (-error) {
		case 0:				return B_OK;
		case LX_EPERM:		return EPERM;
		case LX_ENOENT:		return ENOENT;
		case LX_EINTR:		return EINTR;
		case LX_EIO:		return EIO;
		case LX_E2BIG:		return E2BIG;
		case LX_EAGAIN:		return EAGAIN;
		case LX_ENOMEM:		return ENOMEM;
		case LX_EACCES:		return EACCES;
		case LX_EFAULT:		return EFAULT;
		case LX_EBUSY:		return EBUSY;
		case LX_EEXIST:		return EEXIST;
		case LX_ENODEV:		return ENODEV;
		case LX_EINVAL:		return EINVAL;
		case LX_ENOSPC:		return ENOSPC;
		case LX_ERANGE:		return ERANGE;
		case LX_EDEADLK:	return EDEADLK;
		case LX_ENOSYS:		return ENOSYS;
		case LX_ETIME:		return ETIME;
		case LX_EOVERFLOW:	return EOVERFLOW;
		case LX_EOPNOTSUPP:
		case LX_ENOTSUPP:	return EOPNOTSUPP;
		case LX_ETIMEDOUT:	return ETIMEDOUT;
		case LX_ECANCELED:	return ECANCELED;
		default:			return error < 0 ? B_ERROR : B_OK;
	}
}


area_id
lx_area_clone_to_user(area_id source, void** _address, bool exact,
	bool cached)
{
	if (exact && !IS_USER_ADDRESS(*_address))
		return B_BAD_ADDRESS;
	area_id area = vm_clone_area(team_get_current_team_id(), "powervr buffer",
		_address, exact ? B_EXACT_ADDRESS : B_RANDOMIZED_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, REGION_NO_PRIVATE_MAP, source, true);
	if (area < 0 || !cached)
		return area;

	// The kernel's own mapping stays Normal-NC; it is not used for the
	// buffer's contents after its creation.
	physical_entry entry;
	status_t status = get_memory_map(*_address, B_PAGE_SIZE, &entry, 1);
	if (status == B_OK) {
		status = vm_set_area_memory_type(area, entry.address,
			B_WRITE_BACK_MEMORY);
	}
	if (status != B_OK) {
		TRACE("no cached clone of area %" B_PRId32 ": %s\n", source,
			strerror(status));
		vm_delete_area(team_get_current_team_id(), area, true);
		return status;
	}
	return area;
}


//	#pragma mark - wait queues


static inline ConditionVariable*
queue_variable(wait_queue_head_t* queue)
{
	return reinterpret_cast<ConditionVariable*>(queue->storage);
}


void
init_waitqueue_head(wait_queue_head_t* queue)
{
	queue->generation = 0;
	ConditionVariable* variable = new(queue->storage) ConditionVariable;
	variable->Init(queue, "powervr wait queue");
}


void
wake_up_all(wait_queue_head_t* queue)
{
	atomic_add(&queue->generation, 1);
	queue_variable(queue)->NotifyAll();
}


int32
lx_wait_queue_generation(wait_queue_head_t* queue)
{
	return atomic_get(&queue->generation);
}


/*!	Sleeps until wake_up_all() or the deadline, unless a wake_up_all()
	came after \a generation was read: the entry is added before the count
	is checked again, so no wake-up is lost in between.
*/
void
lx_wait_queue_sleep(wait_queue_head_t* queue, int32 generation,
	bigtime_t deadline)
{
	ConditionVariableEntry entry;
	queue_variable(queue)->Add(&entry);
	if (atomic_get(&queue->generation) != generation)
		return;
	entry.Wait(B_ABSOLUTE_TIMEOUT, deadline);
}


status_t
lx_wait_queue_sleep_interruptible(wait_queue_head_t* queue, int32 generation,
	bigtime_t deadline)
{
	ConditionVariableEntry entry;
	queue_variable(queue)->Add(&entry);
	if (atomic_get(&queue->generation) != generation)
		return B_OK;
	status_t status = entry.Wait(B_ABSOLUTE_TIMEOUT | B_CAN_INTERRUPT,
		deadline);
	return status == B_INTERRUPTED ? B_INTERRUPTED : B_OK;
}


bool
lx_access_ok(const void* address, unsigned long size)
{
	return size == 0 || is_user_address_range(address, size);
}


//	#pragma mark - memory


// what exists of each kind, for the driver's dump
static int32 sBufferCount;
static int64 sBufferBytes;
static int32 sPageCount;
static int32 sVmapCount;
static int32 sImportCount;
// since boot
static int64 sBuffersMade;
static int64 sBufferBytesMade;


void
lx_memory_totals(uint64* _buffers, uint64* _bytes)
{
	*_buffers = (uint64)atomic_get64(&sBuffersMade);
	*_bytes = (uint64)atomic_get64(&sBufferBytesMade);
}


void
lx_memory_stats(uint32* _buffers, uint64* _bufferBytes, uint32* _pages,
	uint32* _vmaps, uint32* _imports)
{
	*_imports = (uint32)atomic_get(&sImportCount);
	*_buffers = (uint32)atomic_get(&sBufferCount);
	*_bufferBytes = (uint64)atomic_get64(&sBufferBytes);
	*_pages = (uint32)atomic_get(&sPageCount);
	*_vmaps = (uint32)atomic_get(&sVmapCount);
}


static inline phys_addr_t
dma_limit()
{
	// lx_dma_mask is the highest address the GPU can reach
	return (phys_addr_t)lx_dma_mask;
}


/*!	Writes back and invalidates the CPU caches over a range of a cacheable
	mapping, so that nothing stale is left behind once the memory is used
	through a Normal-NC mapping or by the GPU.
*/
static void
clean_invalidate(const void* address, size_t size)
{
	cpu_status state = disable_interrupts();
		// stay on one CPU between reading the line size and using it
	uint64 line = arm64_current_data_cache_line_size();
	addr_t start = (addr_t)address & ~(addr_t)(line - 1);
	addr_t end = (addr_t)address + size;
	for (addr_t cursor = start; cursor < end; cursor += line)
		arm64_clean_invalidate_data_cache_line_poc(cursor);
	asm volatile("dsb sy" ::: "memory");
	restore_interrupts(state);
}


/*!	A locked area for the GPU: zeroed, cleaned out of the caches, then made
	Normal-NC, with its physical pieces recorded for the GPU's page tables.
*/
int
lx_dma_buffer_alloc(struct lx_dma_buffer* buffer, size_t size,
	const char* name)
{
	memset(buffer, 0, sizeof(*buffer));
	buffer->area = -1;
	size = (size + B_PAGE_SIZE - 1) & ~(size_t)(B_PAGE_SIZE - 1);
	if (size == 0)
		return -LX_EINVAL;

	void* address = NULL;
	area_id area = create_area(name, &address, B_ANY_KERNEL_ADDRESS, size,
		B_FULL_LOCK, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (area < 0) {
		TRACE("no memory for %s (%zu bytes): %s\n", name, size,
			strerror(area));
		return -LX_ENOMEM;
	}

	uint32 pageCount = size / B_PAGE_SIZE;
	physical_entry* entries
		= (physical_entry*)malloc(pageCount * sizeof(physical_entry));
	if (entries == NULL) {
		delete_area(area);
		return -LX_ENOMEM;
	}
	uint32 entryCount = pageCount;
	status_t status = get_memory_map_etc(B_SYSTEM_TEAM, address, size,
		entries, &entryCount);
	if (status != B_OK) {
		TRACE("no memory map for %s: %s\n", name, strerror(status));
		free(entries);
		delete_area(area);
		return -LX_EIO;
	}

	// merge physically adjacent entries into runs
	struct lx_dma_run* runs
		= (struct lx_dma_run*)malloc(entryCount * sizeof(struct lx_dma_run));
	if (runs == NULL) {
		free(entries);
		delete_area(area);
		return -LX_ENOMEM;
	}
	uint32 runCount = 0;
	for (uint32 i = 0; i < entryCount; i++) {
		if (entries[i].size == 0)
			break;
		if (runCount > 0 && runs[runCount - 1].address
				+ runs[runCount - 1].size == entries[i].address) {
			runs[runCount - 1].size += entries[i].size;
			continue;
		}
		runs[runCount].address = entries[i].address;
		runs[runCount].size = entries[i].size;
		runCount++;
	}
	free(entries);

	for (uint32 i = 0; i < runCount; i++) {
		if (runs[i].address + runs[i].size - 1 > dma_limit()) {
			TRACE("%s: page at %#llx is beyond the GPU's reach (%#llx)\n",
				name, runs[i].address, lx_dma_mask);
			free(runs);
			delete_area(area);
			return -LX_ERANGE;
		}
	}

	// B_FULL_LOCK pages come cleared (no CREATE_AREA_DONT_CLEAR), maybe
	// only into the cache: push that out before the mapping goes Normal-NC.
	clean_invalidate(address, size);
	status = vm_set_area_memory_type(area, runs[0].address,
		B_WRITE_COMBINING_MEMORY);
	if (status != B_OK) {
		TRACE("%s cannot be made write-combined: %s\n", name,
			strerror(status));
		free(runs);
		delete_area(area);
		return -LX_EIO;
	}
	asm volatile("dsb sy" ::: "memory");

	buffer->area = area;
	buffer->address = address;
	buffer->size = size;
	buffer->run_count = runCount;
	buffer->runs = runs;
	atomic_add(&sBufferCount, 1);
	atomic_add64(&sBufferBytes, (int64)size);
	atomic_add64(&sBuffersMade, 1);
	atomic_add64(&sBufferBytesMade, (int64)size);
	return 0;
}


void
lx_dma_buffer_free(struct lx_dma_buffer* buffer)
{
	if (buffer->area >= 0 && buffer->imported) {
		unlock_memory_etc(B_SYSTEM_TEAM, buffer->address, buffer->size, 0);
		delete_area(buffer->area);
		atomic_add(&sImportCount, -1);
	} else if (buffer->area >= 0) {
		delete_area(buffer->area);
		atomic_add(&sBufferCount, -1);
		atomic_add64(&sBufferBytes, -(int64)buffer->size);
	}
	free(buffer->runs);
	memset(buffer, 0, sizeof(*buffer));
	buffer->area = -1;
}


/*!	Merges a memory map's entries into runs, checks that the GPU reaches
	them; frees \a entries.
*/
static int
make_runs(struct lx_dma_buffer* buffer, physical_entry* entries,
	uint32 entryCount, const char* name)
{
	struct lx_dma_run* runs
		= (struct lx_dma_run*)malloc(entryCount * sizeof(struct lx_dma_run));
	if (runs == NULL) {
		free(entries);
		return -LX_ENOMEM;
	}
	uint32 runCount = 0;
	for (uint32 i = 0; i < entryCount; i++) {
		if (entries[i].size == 0)
			break;
		if (runCount > 0 && runs[runCount - 1].address
				+ runs[runCount - 1].size == entries[i].address) {
			runs[runCount - 1].size += entries[i].size;
			continue;
		}
		runs[runCount].address = entries[i].address;
		runs[runCount].size = entries[i].size;
		runCount++;
	}
	free(entries);

	for (uint32 i = 0; i < runCount; i++) {
		if (runs[i].address + runs[i].size - 1 > dma_limit()) {
			TRACE("%s: page at %#llx is beyond the GPU's reach (%#llx)\n",
				name, (unsigned long long)runs[i].address, lx_dma_mask);
			free(runs);
			return -LX_ERANGE;
		}
	}
	buffer->run_count = runCount;
	buffer->runs = runs;
	return 0;
}


/*!	The physical pages of [offset, offset + size) of a wired area (B_FULL_LOCK,
	B_CONTIGUOUS, B_ALREADY_WIRED), from its cache: lock_memory_etc() leaves
	those alone, and a clone of one is not mapped in until it is touched, so
	get_memory_map_etc() would find nothing there (and panic). B_BAD_TYPE for
	other areas; B_BAD_ADDRESS when a page is not in the area's own cache.
*/
static status_t
wired_area_memory_map(area_id id, size_t offset, size_t size,
	physical_entry* entries, uint32* _count)
{
	VMArea* area = VMAreas::Lookup(id);
	if (area == NULL)
		return B_BAD_VALUE;
	if (area->wiring != B_FULL_LOCK && area->wiring != B_CONTIGUOUS
		&& area->wiring != B_ALREADY_WIRED) {
		return B_BAD_TYPE;
	}

	uint32 count = 0;
	VMCache* cache = vm_area_get_locked_cache(area);
	for (size_t done = 0; done < size; done += B_PAGE_SIZE) {
		vm_page* page = cache->LookupPage(area->cache_offset + offset + done);
		if (page == NULL || count >= *_count) {
			vm_area_put_locked_cache(cache);
			return B_BAD_ADDRESS;
		}
		entries[count].address
			= (phys_addr_t)page->physical_page_number * B_PAGE_SIZE;
		entries[count].size = B_PAGE_SIZE;
		count++;
	}
	vm_area_put_locked_cache(cache);
	*_count = count;
	return B_OK;
}


int
lx_dma_buffer_import(struct lx_dma_buffer* buffer, const void* address,
	size_t size)
{
	memset(buffer, 0, sizeof(*buffer));
	buffer->area = -1;
	if (size == 0 || ((addr_t)address | size) % B_PAGE_SIZE != 0
		|| !is_user_address_range(address, size)) {
		return -LX_EINVAL;
	}

	area_id source = area_for(const_cast<void*>(address));
	area_info info;
	if (source < 0 || get_area_info(source, &info) != B_OK)
		return -LX_EFAULT;
	size_t offset = (addr_t)address - (addr_t)info.address;
	if (offset + size > info.size)
		return -LX_EINVAL;
		// one area only
	if ((info.protection & (B_READ_AREA | B_WRITE_AREA))
			!= (B_READ_AREA | B_WRITE_AREA)) {
		// the GPU may write it
		return -LX_EACCES;
	}

	void* base = NULL;
	area_id area = vm_clone_area(B_SYSTEM_TEAM, "powervr import", &base,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		REGION_NO_PRIVATE_MAP, source, true);
	if (area < 0) {
		TRACE("import: no kernel clone of area %" B_PRId32 ": %s\n", source,
			strerror(area));
		return -LX_EFAULT;
	}
	void* kernelAddress = (uint8*)base + offset;
	status_t status = lock_memory_etc(B_SYSTEM_TEAM, kernelAddress, size, 0);
	if (status != B_OK) {
		delete_area(area);
		return -LX_ENOMEM;
	}

	uint32 pageCount = size / B_PAGE_SIZE;
	physical_entry* entries
		= (physical_entry*)malloc(pageCount * sizeof(physical_entry));
	uint32 entryCount = pageCount;
	status = entries != NULL ? B_OK : B_NO_MEMORY;
	if (status == B_OK) {
		status = wired_area_memory_map(area, offset, size, entries,
			&entryCount);
	}
	if (status == B_BAD_TYPE) {
		// lock_memory_etc() mapped it in: the translation map knows
		entryCount = pageCount;
		status = get_memory_map_etc(B_SYSTEM_TEAM, kernelAddress, size,
			entries, &entryCount);
	}
	if (status != B_OK) {
		TRACE("import: no memory map of area %" B_PRId32 ": %s\n", source,
			strerror(status));
		free(entries);
		unlock_memory_etc(B_SYSTEM_TEAM, kernelAddress, size, 0);
		delete_area(area);
		return status == B_NO_MEMORY ? -LX_ENOMEM : -LX_EFAULT;
	}
	int error = make_runs(buffer, entries, entryCount, "import");
	if (error != 0) {
		unlock_memory_etc(B_SYSTEM_TEAM, kernelAddress, size, 0);
		delete_area(area);
		return error;
	}

	buffer->area = area;
	buffer->address = kernelAddress;
	buffer->size = size;
	buffer->imported = true;
	atomic_add(&sImportCount, 1);
	return 0;
}


int
lx_dma_buffer_address(const struct lx_dma_buffer* buffer, size_t offset,
	unsigned long long* _address)
{
	if (offset >= buffer->size)
		return -LX_EINVAL;
	for (uint32 i = 0; i < buffer->run_count; i++) {
		if (offset < buffer->runs[i].size) {
			*_address = buffer->runs[i].address + offset;
			return 0;
		}
		offset -= buffer->runs[i].size;
	}
	return -LX_EINVAL;
}


//	#pragma mark - pages


/*	One page: its own 4 KiB area (the cacheable kernel mapping, used only to
	zero and clean it) and its physical address. vmap() maps pages again,
	Normal-NC, for the code that writes them; a single page's Normal-NC
	mapping (page tables) is made once and kept with the page.

	Page tables come and go with every GPU mapping, so freed pages wait on
	a free list for the next alloc_page() instead of costing two areas each
	time. */
struct page {
	area_id			area;
	void*			address;
	phys_addr_t		physical;
	area_id			wc_area;	// the kept single-page vmap(), or -1
	void*			wc_address;
	struct page*	next_free;
};

static const char* const kPageWCName = "powervr page wc";
static const int32 kMaxFreePages = 1024;	// 4 MiB

static mutex sFreePagesLock = MUTEX_INITIALIZER("powervr free pages");
static struct page* sFreePages;
static int32 sFreePageCount;


static void
destroy_page(struct page* page)
{
	if (page->wc_area >= 0)
		delete_area(page->wc_area);
	delete_area(page->area);
	free(page);
}


struct page*
alloc_page(unsigned int flags)
{
	(void)flags;
		// always zeroed, as with __GFP_ZERO

	mutex_lock(&sFreePagesLock);
	struct page* page = sFreePages;
	if (page != NULL) {
		sFreePages = page->next_free;
		sFreePageCount--;
	}
	mutex_unlock(&sFreePagesLock);

	if (page != NULL) {
		page->next_free = NULL;
		memset(page->address, 0, B_PAGE_SIZE);
		clean_invalidate(page->address, B_PAGE_SIZE);
		atomic_add(&sPageCount, 1);
		return page;
	}

	page = (struct page*)calloc(1, sizeof(struct page));
	if (page == NULL)
		return NULL;
	page->wc_area = -1;

	// When the GPU reaches all of memory, a plain locked page will do: a
	// physically restricted run (vm_page_allocate_page_run()) searches the
	// page array, which grows slow as memory fragments.
	virtual_address_restrictions virtualRestrictions = {};
	virtualRestrictions.address_specification = B_ANY_KERNEL_ADDRESS;
	physical_address_restrictions physicalRestrictions = {};
	uint32 lock = B_FULL_LOCK;
	if (dma_limit() < vm_page_max_address()) {
		physicalRestrictions.high_address = dma_limit() + 1;
		lock = B_CONTIGUOUS;
	}
	page->area = create_area_etc(B_SYSTEM_TEAM, "powervr page", B_PAGE_SIZE,
		lock, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0,
		&virtualRestrictions, &physicalRestrictions, &page->address);
	if (page->area < 0) {
		TRACE("no page: %s\n", strerror(page->area));
		free(page);
		return NULL;
	}

	physical_entry entry;
	if (get_memory_map(page->address, B_PAGE_SIZE, &entry, 1) != B_OK) {
		delete_area(page->area);
		free(page);
		return NULL;
	}
	page->physical = entry.address;
	// The allocator cleared it (no CREATE_AREA_DONT_CLEAR), possibly into
	// the cache only.
	clean_invalidate(page->address, B_PAGE_SIZE);
	atomic_add(&sPageCount, 1);
	return page;
}


void
__free_page(struct page* page)
{
	if (page == NULL)
		return;
	atomic_add(&sPageCount, -1);

	mutex_lock(&sFreePagesLock);
	if (sFreePageCount < kMaxFreePages) {
		page->next_free = sFreePages;
		sFreePages = page;
		sFreePageCount++;
		page = NULL;
	}
	mutex_unlock(&sFreePagesLock);

	if (page != NULL)
		destroy_page(page);
}


/*!	Gives the free pages back to the system (driver unload). */
void
lx_free_pages_flush(void)
{
	mutex_lock(&sFreePagesLock);
	struct page* page = sFreePages;
	sFreePages = NULL;
	sFreePageCount = 0;
	mutex_unlock(&sFreePagesLock);

	while (page != NULL) {
		struct page* next = page->next_free;
		destroy_page(page);
		page = next;
	}
}


unsigned long long
dma_map_page(struct device* device, struct page* page, size_t offset,
	size_t size, enum dma_data_direction direction)
{
	(void)device;
	(void)direction;
	if (page == NULL || offset + size > B_PAGE_SIZE)
		return ~0ULL;
	clean_invalidate((uint8*)page->address + offset, size);
	return page->physical + offset;
}


void
dma_unmap_page(struct device* device, unsigned long long address,
	size_t size, enum dma_data_direction direction)
{
	// Nothing to do: the pages are only used through Normal-NC mappings.
	(void)device;
	(void)address;
	(void)size;
	(void)direction;
}


/*!	Maps \a count pages into one kernel range: write-combined when asked
	for (the only use, page tables), cacheable otherwise.
*/
void*
vmap(struct page** pages, unsigned int count, unsigned long flags,
	pgprot_t protection)
{
	(void)flags;
	if (count == 0)
		return NULL;

	bool writeCombine = (protection.value & LX_PGPROT_WRITECOMBINE) != 0;
	if (count == 1 && writeCombine) {
		// a page table: its mapping is kept with the page (vunmap() leaves
		// it), so a page from the free list costs no area at all
		struct page* page = pages[0];
		if (page->wc_area < 0) {
			clean_invalidate(page->address, B_PAGE_SIZE);
			page->wc_area = map_physical_memory(kPageWCName, page->physical,
				B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS | B_WRITE_COMBINING_MEMORY,
				B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &page->wc_address);
			if (page->wc_area < 0) {
				TRACE("vmap of a page failed: %s\n",
					strerror(page->wc_area));
				return NULL;
			}
		}
		atomic_add(&sVmapCount, 1);
		return page->wc_address;
	}

	bool contiguous = true;
	for (unsigned int i = 0; i < count; i++) {
		clean_invalidate(pages[i]->address, B_PAGE_SIZE);
		if (i > 0 && pages[i]->physical
				!= pages[i - 1]->physical + B_PAGE_SIZE) {
			contiguous = false;
		}
	}

	void* address = NULL;
	area_id area;
	if (contiguous) {
		area = map_physical_memory("powervr vmap", pages[0]->physical,
			count * B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS
				| (writeCombine ? B_WRITE_COMBINING_MEMORY : 0),
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address);
	} else {
		generic_io_vec* vecs
			= (generic_io_vec*)malloc(count * sizeof(generic_io_vec));
		if (vecs == NULL)
			return NULL;
		for (unsigned int i = 0; i < count; i++) {
			vecs[i].base = pages[i]->physical;
			vecs[i].length = B_PAGE_SIZE;
		}
		addr_t size = count * B_PAGE_SIZE;
		area = vm_map_physical_memory_vecs(B_SYSTEM_TEAM, "powervr vmap",
			&address, B_ANY_KERNEL_ADDRESS, &size,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, vecs, count);
		free(vecs);
		if (area >= 0 && writeCombine) {
			status_t status = vm_set_area_memory_type(area,
				pages[0]->physical, B_WRITE_COMBINING_MEMORY);
			if (status != B_OK) {
				delete_area(area);
				area = status;
			}
		}
	}
	if (area < 0) {
		TRACE("vmap of %u pages failed: %s\n", count, strerror(area));
		return NULL;
	}
	atomic_add(&sVmapCount, 1);
	return address;
}


void
vunmap(const void* address)
{
	if (address == NULL)
		return;
	area_id area = area_for(const_cast<void*>(address));
	if (area < 0)
		return;
	area_info info;
	if (get_area_info(area, &info) == B_OK
		&& strcmp(info.name, kPageWCName) == 0) {
		// kept with its page (vmap())
		atomic_add(&sVmapCount, -1);
		return;
	}
	if (delete_area(area) == B_OK)
		atomic_add(&sVmapCount, -1);
}


//	#pragma mark - firmware files


int
request_firmware(const struct firmware** _firmware, const char* name,
	struct device* device)
{
	(void)device;
	static const directory_which kDirectories[] = {
		B_SYSTEM_DATA_DIRECTORY,
		B_SYSTEM_NONPACKAGED_DATA_DIRECTORY
	};

	struct firmware* firmware
		= (struct firmware*)calloc(1, sizeof(struct firmware));
	if (firmware == NULL)
		return -LX_ENOMEM;

	int fd = -1;
	for (size_t i = 0; i < B_COUNT_OF(kDirectories); i++) {
		if (find_directory(kDirectories[i], -1, false, firmware->path,
				sizeof(firmware->path)) != B_OK) {
			continue;
		}
		strlcat(firmware->path, "/firmware/", sizeof(firmware->path));
		strlcat(firmware->path, name, sizeof(firmware->path));
		fd = open(firmware->path, O_RDONLY);
		if (fd >= 0)
			break;
		TRACE("no %s\n", firmware->path);
	}
	if (fd < 0) {
		free(firmware);
		return -LX_ENOENT;
	}

	off_t size = lseek(fd, 0, SEEK_END);
	if (size <= 0 || size > MAX_FIRMWARE_SIZE) {
		TRACE("%s is %" B_PRIdOFF " bytes, which cannot be right\n",
			firmware->path, size);
		close(fd);
		free(firmware);
		return -LX_E2BIG;
	}

	uint8* data = (uint8*)malloc(size);
	if (data == NULL) {
		close(fd);
		free(firmware);
		return -LX_ENOMEM;
	}
	ssize_t bytesRead = 0;
	if (lseek(fd, 0, SEEK_SET) == 0)
		bytesRead = read(fd, data, size);
	close(fd);
	if (bytesRead != size) {
		TRACE("read %zd of %" B_PRIdOFF " bytes of %s\n", bytesRead, size,
			firmware->path);
		free(data);
		free(firmware);
		return -LX_EIO;
	}

	firmware->data = data;
	firmware->size = size;
	*_firmware = firmware;
	return 0;
}


void
release_firmware(const struct firmware* firmware)
{
	if (firmware == NULL)
		return;
	free(const_cast<uint8*>(firmware->data));
	free(const_cast<struct firmware*>(firmware));
}
