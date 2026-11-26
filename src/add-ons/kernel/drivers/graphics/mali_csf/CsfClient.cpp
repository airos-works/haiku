/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#include <KernelExport.h>
#include <lock.h>
#include <team.h>
#include <util/AutoLock.h>
#include <vm/vm.h>
#include <vm/VMAddressSpace.h>
#include <vm/VMArea.h>
#include <vm/VMCache.h>
#include <stdlib.h>
#if defined(__aarch64__)
#include <arch/arm64/cache_line_size.h>
#endif

#include "CsfClient.h"
#include "CsfPageTable.h"

using namespace MaliCSF;

static mutex sClientLock = MUTEX_INITIALIZER("Mali CSF clients");
static const uint32 kMaxClients = 256;
static const uint64 kMaxGlobalBufferBytes = UINT64_C(4) << 30;
static uint32 sClients, sBuffers, sNextHandle = 1;
static uint64 sBufferBytes;
static const uint32 kMaxGlobalVms = 256;
static const uint32 kMaxGlobalGenerations = 4096;
static const uint32 kMaxGlobalTablePages = 65536;
static uint32 sVms, sGenerations, sTablePages, sAccounts;
static const uint32 kMaxGlobalHeaps = 256;
static const uint64 kMaxGlobalHeapBytes = UINT64_C(512) << 20;
static uint32 sHeaps, sHeapChunks, sHeapTablePages, sHeapGenerations;
static uint64 sHeapBytes;
static const uint64 kMaxGlobalImportedBytes = UINT64_C(1) << 30;
static uint64 sImportedBytes;
// Mesa reports a refused allocation only as GL_OUT_OF_MEMORY, which does not
// tell a full quota from a full machine. Name the bound, once per kind.
enum {
	kRefusedClients, kRefusedBufferCount, kRefusedClientBytes, kRefusedGlobalBytes,
	kRefusedVms, kRefusedMappings, kRefusedMappedBytes, kRefusedGenerations,
	kRefusedTablePages, kRefusedImportedBytes
};
static uint32 sRefusalsLogged;

static void
LogRefusal(uint32 kind, const char* what, uint64 used, uint64 limit)
{
	if ((sRefusalsLogged & (1u << kind)) != 0)
		return;
	sRefusalsLogged |= 1u << kind;
	dprintf("mali_csf: refused: %s (%llu in use, limit %llu)\n", what,
		(unsigned long long)used, (unsigned long long)limit);
}

static const uint64 kHeapPageAttributes = (UINT64_C(3) << 53) | 0x747;
	// GPU cached, outer/inner shareable, read/write, NX; CPU initialization is NC.

struct ClientMemory {
	area_id area;
	void* address;
	size_t bytes;
	uint64* pages;
};

// A generation can outlive its client. Keep its resident allocations charged
// to this account until the final mapping/work reference releases them.
struct ClientAccount {
	uint32 references;
	uint32 buffers;
	uint64 bytes;
	uint64 importedBytes;
	uint32 heaps;
	uint64 heapBytes;
};

// How an imported buffer holds the team's pages (kImportBuffer).
enum {
	kNotImported = 0,
	kImportWired, // lock_memory_etc() of the range in the team
	kImportCacheReference // a reference to a contiguous area's cache
};

struct ClientBuffer : ClientMemory {
	bool cached;
	uint8 import;
	team_id team;
	addr_t userAddress;
	VMCache* cache;
	ClientBuffer* next;
	uint32 handle;
	uint32 references;
	ClientAccount* account;
};

struct ClientMapping {
	uint64 address;
	uint64 bytes;
	uint64 offset;
	ClientBuffer* buffer;
	uint32 flags;
};

struct ClientHeap;
struct ClientHeapChunk {
	ClientHeapChunk* next;
	ClientHeap* heap;
	ClientMemory data;
	ClientMemory tables;
	uint64 address;
	uint32 newLeaves;
	uint32 leafIndices[5]; // an unaligned 8 MiB range can occupy five 2 MiB leaves
};

struct ClientHeap {
	uint32 references, handle, slot;
	uint32 chunkSize, initialChunks, maxChunks, targetInFlight, chunkCount;
	uint64 firstChunkAddress;
	ClientAccount* account;
	ClientMemory base; // L2 table, context L3 table, private context page
	uint64* leaves[512];
	ClientHeapChunk* chunks;
	ClientHeapChunk* pending;
};

struct ClientGeneration {
	uint32 references;
	uint32 count;
	uint64 number;
	uint64 userLimit;
	uint64 mappedBytes;
	ClientMemory tables;
	ClientMapping* mappings;
	ClientMemory heapTable;
	ClientHeap* heaps[kMaxVmHeaps];
};

struct ClientVm {
	ClientVm* next;
	uint32 handle;
	ClientGeneration* current;
};

struct Client {
	team_id owner;
	bool writable;
	bool closed;
	ClientAccount* account;
	ClientBuffer* first;
	uint32 vms;
	ClientVm* firstVm;
};

static status_t
CleanClientRam(void* address, size_t bytes)
{
#if defined(__aarch64__)
	// The allocator clears through its cached kernel alias. Evict that data so
	// that the GPU, which does not snoop the CPU's caches, reads zeros.
	uint64 ctr;
	asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
	size_t line = arm64_data_cache_line_size(ctr);
	for (addr_t p = (addr_t)address; p < (addr_t)address + bytes; p += line)
		asm volatile("dc civac, %0" :: "r"(p) : "memory");
	memory_full_barrier();
	return B_OK;
#else
	return B_NOT_SUPPORTED;
#endif
}

static status_t
MakeClientRamNoncacheable(area_id area, void* address, size_t bytes)
{
#if defined(__aarch64__)
	// Evict the allocator's data before changing the memory type, and before
	// creating any user aliases.
	CleanClientRam(address, bytes);
	// ARM64 encodes the type in each PTE; physicalBase is unused here. The
	// backing RAM can be scattered. vm_clone_area preserves this memory type.
	status_t status = vm_set_area_memory_type(area, 0, B_WRITE_COMBINING_MEMORY);
	memory_full_barrier();
	return status;
#else
	return B_NOT_SUPPORTED;
#endif
}

static status_t
CheckAccess(const Client* client)
{
	return !client->closed && client->owner == team_get_current_team_id()
		? B_OK : B_NOT_ALLOWED;
}

static void
DeleteMemory(ClientMemory& memory)
{
	if (memory.area >= B_OK)
		delete_area(memory.area);
	free(memory.pages);
	memset(&memory, 0, sizeof(memory));
	memory.area = -1;
}

static status_t
AllocateMemory(const char* name, size_t bytes, ClientMemory& memory, bool cached = false)
{
	memory.area = -1;
	memory.bytes = bytes;
	memory.pages = (uint64*)malloc(bytes / B_PAGE_SIZE * sizeof(uint64));
	if (memory.pages == NULL)
		return B_NO_MEMORY;
	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	memory.area = create_area_etc(B_SYSTEM_TEAM, name, bytes,
		B_FULL_LOCK, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0,
		&virtualRestrictions, &physicalRestrictions, &memory.address);
	status_t status = memory.area < B_OK ? memory.area : B_OK;
	for (size_t offset = 0; status == B_OK && offset < bytes; offset += B_PAGE_SIZE) {
		physical_entry entry;
		status = get_memory_map((uint8*)memory.address + offset, B_PAGE_SIZE, &entry, 1);
		if (status == B_OK && (entry.size < B_PAGE_SIZE
			|| !GpuPageTable::ValidPhysical(entry.address))) {
			status = B_BAD_VALUE;
		}
		if (status == B_OK)
			memory.pages[offset / B_PAGE_SIZE] = entry.address;
	}
	if (status == B_OK) {
		status = cached ? CleanClientRam(memory.address, bytes)
			: MakeClientRamNoncacheable(memory.area, memory.address, bytes);
	}
	if (status != B_OK)
		DeleteMemory(memory);
	return status;
}

static void
ReleaseAccount(ClientAccount* account)
{
	if (--account->references != 0)
		return;
	sAccounts--;
	free(account);
}

static void
ReleaseImport(ClientBuffer* buffer)
{
	if (buffer->import == kImportWired) {
		// Fails harmlessly once the team's address space is gone, and with it
		// the wiring. The team closes its files (and the watcher in driver.cpp
		// closes this client) before that.
		unlock_memory_etc(buffer->team, (void*)buffer->userAddress, buffer->bytes,
			B_READ_DEVICE);
	} else if (buffer->import == kImportCacheReference)
		buffer->cache->ReleaseRef();
	buffer->import = kNotImported;
	buffer->cache = NULL;
}

static void
ReleaseBuffer(ClientBuffer* buffer)
{
	if (--buffer->references != 0)
		return;
	if (buffer->import != kNotImported) {
		buffer->account->importedBytes -= buffer->bytes;
		sImportedBytes -= buffer->bytes;
		ReleaseImport(buffer);
	} else {
		buffer->account->bytes -= buffer->bytes;
		sBufferBytes -= buffer->bytes;
	}
	buffer->account->buffers--;
	sBuffers--;
	ReleaseAccount(buffer->account);
	// User CPU clones retain the VM cache independently after this area is gone.
	DeleteMemory(*buffer);
	free(buffer);
}

static void
DeleteHeapChunk(ClientHeapChunk* chunk)
{
	ClientHeap* heap = chunk->heap;
	heap->account->heapBytes -= heap->chunkSize;
	sHeapBytes -= heap->chunkSize;
	sHeapChunks--;
	sTablePages -= chunk->newLeaves;
	sHeapTablePages -= chunk->newLeaves;
	DeleteMemory(chunk->data);
	DeleteMemory(chunk->tables);
	free(chunk);
}

static void
ReleaseHeap(ClientHeap* heap)
{
	if (--heap->references != 0) return;
	// A prepared transaction owns a reference, so pending cannot reach here.
	while (heap->chunks != NULL) {
		ClientHeapChunk* chunk = heap->chunks;
		heap->chunks = chunk->next;
		DeleteHeapChunk(chunk);
	}
	heap->account->heaps--;
	heap->account->heapBytes -= B_PAGE_SIZE;
	sHeaps--;
	sHeapBytes -= B_PAGE_SIZE;
	sTablePages -= 2;
	sHeapTablePages -= 2;
	ReleaseAccount(heap->account);
	DeleteMemory(heap->base);
	free(heap);
}

// No exposed PTE or old chunk is changed until CommitHeapChunk(). Reservations
// count immediately, including while the worker is waiting for the hardware AS
// lock. Existing generations retain each heap's stable L2/leaf-table ownership.
static status_t
PrepareHeapChunk(ClientHeap* heap, bool initial, ClientHeapChunk** result)
{
	if (heap->pending != NULL) return B_BUSY;
	if (heap->chunkCount >= heap->maxChunks
		|| heap->chunkSize > kMaxClientHeapBytes - heap->account->heapBytes
		|| heap->chunkSize > kMaxGlobalHeapBytes - sHeapBytes)
		return B_NO_MEMORY;
	uint64 offset = kHeapChunksOffset + uint64(heap->chunkCount) * heap->chunkSize;
	unsigned first = offset >> 21, last = (offset + heap->chunkSize - 1) >> 21;
	if (last >= 512 || last - first >= 5) return B_BAD_VALUE;
	ClientHeapChunk* chunk = (ClientHeapChunk*)calloc(1, sizeof(ClientHeapChunk));
	if (chunk == NULL) return B_NO_MEMORY;
	chunk->data.area = chunk->tables.area = -1;
	chunk->heap = heap;
	chunk->address = kHeapBase + heap->slot * kHeapSlotBytes + offset;
	for (unsigned i = first; i <= last; i++)
		if (heap->leaves[i] == NULL) chunk->leafIndices[chunk->newLeaves++] = i;
	status_t status = chunk->newLeaves > kMaxGlobalTablePages - sTablePages
		? B_NO_MEMORY : B_OK;
	if (status == B_OK)
		status = AllocateMemory("Mali CSF heap chunk", heap->chunkSize, chunk->data);
	if (status == B_OK && chunk->newLeaves != 0)
		status = AllocateMemory("Mali CSF heap page tables", chunk->newLeaves * B_PAGE_SIZE, chunk->tables);
	if (status != B_OK) {
		DeleteMemory(chunk->data); DeleteMemory(chunk->tables); free(chunk);
		return status;
	}
	memset(chunk->data.address, 0, chunk->data.bytes);
	if (chunk->tables.address != NULL)
		memset(chunk->tables.address, 0, chunk->tables.bytes);
	// Linux panthor_heap.c (MIT option): a 64-byte header, with only the first
	// u64 set. All 56 other bytes are MBZ. Growth starts a fresh one-chunk list.
	if (initial && heap->chunks != NULL)
		*(uint64*)chunk->data.address = heap->chunks->address | (heap->chunkSize >> 12);
	heap->account->heapBytes += heap->chunkSize;
	sHeapBytes += heap->chunkSize;
	sHeapChunks++;
	sTablePages += chunk->newLeaves;
	sHeapTablePages += chunk->newLeaves;
	heap->pending = chunk;
	*result = chunk;
	return B_OK;
}

static void
CommitHeapChunk(ClientHeapChunk* chunk)
{
	ClientHeap* heap = chunk->heap;
	for (unsigned i = 0; i < chunk->newLeaves; i++)
		heap->leaves[chunk->leafIndices[i]] = (uint64*)((uint8*)chunk->tables.address + i * B_PAGE_SIZE);
	for (size_t i = 0; i < chunk->data.bytes / B_PAGE_SIZE; i++) {
		uint64 address = chunk->address + i * B_PAGE_SIZE;
		heap->leaves[(address >> 21) & 511][(address >> 12) & 511]
			= chunk->data.pages[i] | kHeapPageAttributes;
	}
	memory_full_barrier();
	for (unsigned i = 0; i < chunk->newLeaves; i++)
		((uint64*)heap->base.address)[chunk->leafIndices[i]] = chunk->tables.pages[i] | 3;
	memory_full_barrier();
	chunk->next = heap->chunks;
	heap->chunks = chunk;
	heap->chunkCount++;
	heap->pending = NULL;
}

static status_t
NewHeap(ClientAccount* account, const HeapCreate& request, uint32 slot, ClientHeap** result)
{
	uint64 bytes = B_PAGE_SIZE + uint64(request.initialChunks) * request.chunkSize;
	if (sNextHandle == 0 || account->heaps >= kMaxVmHeaps || sHeaps >= kMaxGlobalHeaps
		|| bytes > kMaxClientHeapBytes - account->heapBytes
		|| bytes > kMaxGlobalHeapBytes - sHeapBytes || sTablePages > kMaxGlobalTablePages - 2)
		return B_NO_MEMORY;
	ClientHeap* heap = (ClientHeap*)calloc(1, sizeof(ClientHeap));
	if (heap == NULL) return B_NO_MEMORY;
	status_t status = AllocateMemory("Mali CSF heap context", 3 * B_PAGE_SIZE, heap->base);
	if (status != B_OK) { free(heap); return status; }
	memset(heap->base.address, 0, heap->base.bytes);
	heap->leaves[0] = (uint64*)((uint8*)heap->base.address + B_PAGE_SIZE);
	heap->leaves[0][0] = heap->base.pages[2] | kHeapPageAttributes;
	((uint64*)heap->base.address)[0] = heap->base.pages[1] | 3;
	heap->references = 1;
	heap->handle = sNextHandle++;
	heap->slot = slot;
	heap->chunkSize = request.chunkSize;
	heap->initialChunks = request.initialChunks;
	heap->maxChunks = request.maxChunks;
	heap->targetInFlight = request.targetInFlight;
	heap->account = account;
	account->references++; account->heaps++; account->heapBytes += B_PAGE_SIZE;
	sHeaps++; sHeapBytes += B_PAGE_SIZE; sTablePages += 2; sHeapTablePages += 2;
	for (unsigned i = 0; status == B_OK && i < request.initialChunks; i++) {
		ClientHeapChunk* chunk = NULL;
		status = PrepareHeapChunk(heap, true, &chunk);
		if (status == B_OK) CommitHeapChunk(chunk);
	}
	if (status != B_OK) { ReleaseHeap(heap); return status; }
	heap->firstChunkAddress = heap->chunks->address;
	*result = heap;
	return B_OK;
}

static void
ReleaseGeneration(ClientGeneration* generation)
{
	if (--generation->references != 0)
		return;
	for (uint32 i = 0; i < generation->count; i++)
		ReleaseBuffer(generation->mappings[i].buffer);
	for (unsigned i = 0; i < kMaxVmHeaps; i++)
		if (generation->heaps[i] != NULL) ReleaseHeap(generation->heaps[i]);
	if (generation->heapTable.area >= B_OK) {
		sHeapGenerations--; sHeapTablePages--; sTablePages--;
		DeleteMemory(generation->heapTable);
	}
	sTablePages -= generation->tables.bytes / B_PAGE_SIZE;
	sGenerations--;
	DeleteMemory(generation->tables);
	free(generation->mappings);
	free(generation);
}

static status_t
CreateGeneration(const ClientMapping* mappings, uint32 count, uint64 limit,
	uint64 number, ClientGeneration** result, ClientHeap* const* heaps = NULL)
{
	bool hasHeaps = false;
	if (heaps != NULL) {
		for (unsigned i = 0; i < kMaxVmHeaps; i++)
			hasHeaps |= heaps[i] != NULL;
	}
	PageTableRegion* regions = NULL;
	if (count != 0) {
		regions = (PageTableRegion*)malloc(count * sizeof(PageTableRegion));
		if (regions == NULL)
			return B_NO_MEMORY;
		for (uint32 i = 0; i < count; i++) {
			const ClientMapping& map = mappings[i];
			regions[i] = {map.address, map.bytes, map.offset, map.buffer->bytes,
				map.buffer->pages, map.flags};
		}
	}
	unsigned pages = GpuPageTable::CountPages(regions, count, limit);
	if (pages == 0 || sGenerations == kMaxGlobalGenerations
		|| pages + unsigned(hasHeaps) > kMaxGlobalTablePages - sTablePages) {
		if (pages != 0 && sGenerations == kMaxGlobalGenerations) {
			LogRefusal(kRefusedGenerations, "VM generations", sGenerations,
				kMaxGlobalGenerations);
		} else if (pages != 0) {
			LogRefusal(kRefusedTablePages, "GPU page-table pages", sTablePages,
				kMaxGlobalTablePages);
		}
		free(regions);
		return pages == 0 ? B_BAD_VALUE : B_NO_MEMORY;
	}
	ClientGeneration* generation = (ClientGeneration*)calloc(1, sizeof(ClientGeneration));
	if (generation == NULL) {
		free(regions);
		return B_NO_MEMORY;
	}
	generation->heapTable.area = -1;
	if (count != 0) {
		generation->mappings = (ClientMapping*)malloc(count * sizeof(ClientMapping));
		if (generation->mappings == NULL) {
			free(regions);
			free(generation);
			return B_NO_MEMORY;
		}
		memcpy(generation->mappings, mappings, count * sizeof(ClientMapping));
	}
	status_t status = AllocateMemory("Mali CSF VM page tables", pages * B_PAGE_SIZE,
		generation->tables);
	if (status == B_OK && !GpuPageTable::Build(generation->tables.address,
		generation->tables.bytes, generation->tables.pages, pages, regions, count, limit)) {
		status = B_BAD_VALUE;
	}
	free(regions);
	if (status == B_OK && hasHeaps) {
		status = AllocateMemory("Mali CSF heap membership table", B_PAGE_SIZE, generation->heapTable);
		if (status == B_OK) {
			memset(generation->heapTable.address, 0, B_PAGE_SIZE);
			for (unsigned i = 0; i < kMaxVmHeaps; i++) {
				if (heaps[i] != NULL)
					((uint64*)generation->heapTable.address)[i] = heaps[i]->base.pages[0] | 3;
			}
			((uint64*)generation->tables.address)[kHeapRootIndex] = generation->heapTable.pages[0] | 3;
		}
	}
	if (status != B_OK) {
		DeleteMemory(generation->heapTable);
		DeleteMemory(generation->tables);
		free(generation->mappings);
		free(generation);
		return status;
	}
	// Complete the NC page-table stores before any later hardware activation.
	memory_full_barrier();
	generation->references = 1;
	generation->count = count;
	generation->number = number;
	generation->userLimit = limit;
	for (uint32 i = 0; i < count; i++) {
		generation->mappings[i].buffer->references++;
		generation->mappedBytes += mappings[i].bytes;
	}
	if (hasHeaps) {
		sHeapGenerations++; sHeapTablePages++; sTablePages++;
		for (unsigned i = 0; i < kMaxVmHeaps; i++) {
			generation->heaps[i] = heaps[i];
			if (heaps[i] != NULL) heaps[i]->references++;
		}
	}
	sGenerations++;
	sTablePages += pages;
	*result = generation;
	return B_OK;
}

status_t
MaliCSF::OpenClient(bool writable, void** cookie)
{
	Client* client = (Client*)calloc(1, sizeof(Client));
	if (client == NULL)
		return B_NO_MEMORY;
	client->account = (ClientAccount*)calloc(1, sizeof(ClientAccount));
	if (client->account == NULL) {
		free(client);
		return B_NO_MEMORY;
	}
	MutexLocker locker(sClientLock);
	if (sClients == kMaxClients) {
		LogRefusal(kRefusedClients, "clients", sClients, kMaxClients);
		free(client->account);
		free(client);
		return B_BUSY;
	}
	client->owner = team_get_current_team_id();
	client->writable = writable;
	client->account->references = 1;
	sAccounts++;
	sClients++;
	*cookie = client;
	return B_OK;
}

status_t
MaliCSF::AccessClient(void* cookie)
{
	MutexLocker locker(sClientLock);
	return CheckAccess((Client*)cookie);
}

static void
DeleteBuffer(ClientBuffer** link)
{
	ClientBuffer* buffer = *link;
	*link = buffer->next;
	ReleaseBuffer(buffer);
}

static void
DeleteVm(Client* client, ClientVm** link)
{
	ClientVm* vm = *link;
	*link = vm->next;
	client->vms--;
	sVms--;
	ReleaseGeneration(vm->current);
	free(vm);
}

static void
CloseLocked(Client* client)
{
	client->closed = true;
	while (client->firstVm != NULL)
		DeleteVm(client, &client->firstVm);
	while (client->first != NULL)
		DeleteBuffer(&client->first);
}

void
MaliCSF::CloseClient(void* cookie)
{
	MutexLocker locker(sClientLock);
	CloseLocked((Client*)cookie);
}

void
MaliCSF::FreeClient(void* cookie)
{
	MutexLocker locker(sClientLock);
	CloseLocked((Client*)cookie);
	ReleaseAccount(((Client*)cookie)->account);
	sClients--;
	free(cookie);
}

template<typename Request>
static status_t
ReadRequest(void* user, size_t length, Request& request)
{
	if (length != sizeof(request))
		return B_BAD_VALUE;
	if (user == NULL || user_memcpy(&request, user, sizeof(request)) != B_OK)
		return B_BAD_ADDRESS;
	return request.version == kClientVersion ? B_OK : B_BAD_VALUE;
}

static ClientBuffer**
FindBuffer(Client* client, uint32 handle)
{
	ClientBuffer** link = &client->first;
	while (*link != NULL && (*link)->handle != handle)
		link = &(*link)->next;
	return link;
}

static status_t
CreateBuffer(Client* client, BufferCreate& request)
{
	if ((request.flags & ~kBufferCpuCached) != 0 || request.reserved != 0 || request.reserved2 != 0
		|| request.handle != 0 || request.bytes == 0 || request.bytes > kMaxBufferBytes) {
		return B_BAD_VALUE;
	}
	size_t bytes = (request.bytes + B_PAGE_SIZE - 1) & ~(size_t)(B_PAGE_SIZE - 1);
	if (client->account->buffers == kMaxClientBuffers || sNextHandle == 0
		|| bytes > kMaxClientBufferBytes - client->account->bytes
		|| bytes > kMaxGlobalBufferBytes - sBufferBytes) {
		if (client->account->buffers == kMaxClientBuffers) {
			LogRefusal(kRefusedBufferCount, "buffers of one client",
				client->account->buffers, kMaxClientBuffers);
		} else if (bytes > kMaxClientBufferBytes - client->account->bytes) {
			LogRefusal(kRefusedClientBytes, "buffer bytes of one client",
				client->account->bytes, kMaxClientBufferBytes);
		} else if (sNextHandle != 0) {
			LogRefusal(kRefusedGlobalBytes, "buffer bytes of all clients", sBufferBytes,
				kMaxGlobalBufferBytes);
		}
		return B_NO_MEMORY;
	}
	ClientBuffer* buffer = (ClientBuffer*)calloc(1, sizeof(ClientBuffer));
	if (buffer == NULL)
		return B_NO_MEMORY;
	const bool cached = (request.flags & kBufferCpuCached) != 0;
	status_t status = AllocateMemory(cached ? "Mali CSF cached client buffer" : "Mali CSF client buffer",
		bytes, *buffer, cached);
	buffer->cached = cached;
	if (status != B_OK) {
		free(buffer);
		return status;
	}
	buffer->handle = sNextHandle++;
	buffer->references = 1;
	buffer->account = client->account;
	buffer->account->references++;
	buffer->next = client->first;
	client->first = buffer;
	client->account->buffers++;
	client->account->bytes += bytes;
	sBuffers++;
	sBufferBytes += bytes;
	request.bytes = bytes;
	request.handle = buffer->handle;
	return B_OK;
}

// The calling team's area covering [address, address + bytes): writable,
// ordinary memory. The address space must be read-locked.
static VMArea*
LookupImportArea(VMAddressSpace* space, addr_t address, size_t bytes)
{
	VMArea* area = space->LookupArea(address);
	if (area == NULL || address - area->Base() > area->Size()
		|| bytes > area->Size() - (address - area->Base())
		|| (area->protection & (B_READ_AREA | B_WRITE_AREA))
			!= (B_READ_AREA | B_WRITE_AREA)
		|| area->cache_type != CACHE_TYPE_RAM) {
		return NULL;
	}
	return area;
}

static bool
CpuCachedArea(const VMArea* area)
{
	// The default memory type is normal write-back memory.
	return area->MemoryType() == 0 || area->MemoryType() == B_WRITE_BACK_MEMORY;
}

// The physical pages of [address, address + bytes) of a contiguous area (a
// frame buffer a display driver cloned into the team), with a reference to
// their cache instead of wiring: the pages stay with the cache when the area,
// its source or the source's other clones go, and the display driver may
// replace its frame buffer (which turns the clones into null areas, which a
// wired range must not be) without waiting for this client.
static status_t
ReferenceContiguousPages(VMArea* area, addr_t address, size_t bytes, uint64* pages,
	VMCache*& _cache)
{
	VMCache* cache = vm_area_get_locked_cache(area);
	status_t status = cache->source == NULL ? B_OK : B_NOT_SUPPORTED;
	off_t offset = area->cache_offset + (address - area->Base());
	for (size_t i = 0; status == B_OK && i < bytes / B_PAGE_SIZE; i++) {
		vm_page* page = cache->LookupPage(offset + i * B_PAGE_SIZE);
		if (page == NULL || page->busy) {
			status = B_BAD_ADDRESS;
			break;
		}
		pages[i] = (uint64)page->PhysicalPageNumber() * B_PAGE_SIZE;
		if (!GpuPageTable::ValidPhysical(pages[i]))
			status = B_BAD_VALUE;
	}
	if (status == B_OK) {
		cache->AcquireRefLocked();
		_cache = cache;
	}
	vm_area_put_locked_cache(cache);
	return status;
}

// Wires [address, address + bytes) in the calling team, as for a transfer
// into user memory, and returns its physical pages. Not a kernel clone of the
// team's area: such a clone maps a wired source's pages without mapping
// objects, and wiring it for writing does copy-on-write on them.
static status_t
WireTeamPages(addr_t address, size_t bytes, uint64* pages, bool& cached)
{
	const team_id team = team_get_current_team_id();
	status_t status = lock_memory_etc(team, (void*)address, bytes, B_READ_DEVICE);
	if (status != B_OK)
		return status;
	// Mapped now, as get_memory_map() needs: a fully locked clone maps its
	// pages only once they are touched.
	for (size_t offset = 0; status == B_OK && offset < bytes; offset += B_PAGE_SIZE) {
		uint8 byte;
		status = user_memcpy(&byte, (const void*)(address + offset), 1);
	}
	// While the range is wired its areas cannot go: check what it is now.
	if (status == B_OK) {
		VMAddressSpace* space = VMAddressSpace::GetCurrent();
		if (space == NULL)
			status = B_BAD_TEAM_ID;
		else {
			space->ReadLock();
			VMArea* area = LookupImportArea(space, address, bytes);
			if (area == NULL || area->wiring == B_CONTIGUOUS)
				status = B_BAD_ADDRESS;
			else
				cached = CpuCachedArea(area);
			space->ReadUnlock();
			space->Put();
		}
	}
	for (size_t i = 0; status == B_OK && i < bytes / B_PAGE_SIZE; i++) {
		physical_entry entry;
		status = get_memory_map((const void*)(address + i * B_PAGE_SIZE), B_PAGE_SIZE,
			&entry, 1);
		if (status == B_OK && (entry.size < B_PAGE_SIZE
				|| !GpuPageTable::ValidPhysical(entry.address))) {
			status = B_BAD_VALUE;
		}
		if (status == B_OK)
			pages[i] = entry.address;
	}
	if (status != B_OK)
		unlock_memory_etc(team, (void*)address, bytes, B_READ_DEVICE);
	return status;
}

// kImportBuffer. The memory work runs without sClientLock: wiring may fault
// pages in. The client is checked again before the buffer is published.
status_t
MaliCSF::ImportClientBuffer(void* cookie, void* user, size_t length)
{
	BufferImport request;
	status_t status = ReadRequest(user, length, request);
	if (status != B_OK)
		return status;
	const addr_t address = (addr_t)request.address;
	const size_t bytes = (size_t)request.bytes;
	if (request.flags != 0 || request.handle != 0 || request.reserved != 0
		|| request.address != address || bytes == 0 || bytes > kMaxBufferBytes
		|| ((address | bytes) & (B_PAGE_SIZE - 1)) != 0
		|| address + bytes < address || !IS_USER_ADDRESS(address)
		|| !IS_USER_ADDRESS(address + bytes - 1)) {
		return B_BAD_VALUE;
	}
	{
		MutexLocker locker(sClientLock);
		Client* client = (Client*)cookie;
		status = CheckAccess(client);
		if (status != B_OK)
			return status;
		if (!client->writable)
			return B_NOT_ALLOWED;
		// Checked again below; refusing early spares the wiring.
		if (bytes > kMaxClientImportedBytes - client->account->importedBytes
			|| bytes > kMaxGlobalImportedBytes - sImportedBytes)
			return B_NO_MEMORY;
	}

	ClientBuffer* buffer = (ClientBuffer*)calloc(1, sizeof(ClientBuffer));
	if (buffer == NULL)
		return B_NO_MEMORY;
	buffer->area = -1;
	buffer->bytes = bytes;
	buffer->pages = (uint64*)malloc(bytes / B_PAGE_SIZE * sizeof(uint64));
	if (buffer->pages == NULL) {
		free(buffer);
		return B_NO_MEMORY;
	}
	buffer->team = team_get_current_team_id();
	buffer->userAddress = address;

	VMAddressSpace* space = VMAddressSpace::GetCurrent();
	if (space == NULL) {
		DeleteMemory(*buffer);
		free(buffer);
		return B_BAD_TEAM_ID;
	}
	space->ReadLock();
	VMArea* area = LookupImportArea(space, address, bytes);
	bool contiguous = area != NULL && area->wiring == B_CONTIGUOUS;
	status = area == NULL ? B_BAD_ADDRESS : B_OK;
	if (contiguous) {
		buffer->cached = CpuCachedArea(area);
		status = ReferenceContiguousPages(area, address, bytes, buffer->pages, buffer->cache);
		if (status == B_OK)
			buffer->import = kImportCacheReference;
	}
	space->ReadUnlock();
	space->Put();
	if (status == B_OK && !contiguous) {
		status = WireTeamPages(address, bytes, buffer->pages, buffer->cached);
		if (status == B_OK)
			buffer->import = kImportWired;
	}
	if (status != B_OK) {
		DeleteMemory(*buffer);
		free(buffer);
		return status;
	}

	MutexLocker locker(sClientLock);
	Client* client = (Client*)cookie;
	status = CheckAccess(client);
	if (status == B_OK && !client->writable)
		status = B_NOT_ALLOWED;
	if (status == B_OK && (client->account->buffers == kMaxClientBuffers
			|| sNextHandle == 0)) {
		LogRefusal(kRefusedBufferCount, "buffers of one client", client->account->buffers,
			kMaxClientBuffers);
		status = B_NO_MEMORY;
	}
	if (status == B_OK && (bytes > kMaxClientImportedBytes - client->account->importedBytes
			|| bytes > kMaxGlobalImportedBytes - sImportedBytes)) {
		LogRefusal(kRefusedImportedBytes, "imported bytes",
			sImportedBytes, kMaxGlobalImportedBytes);
		status = B_NO_MEMORY;
	}
	if (status == B_OK) {
		request.flags = kBufferImported | (buffer->cached ? kBufferCpuCached : 0);
		request.handle = sNextHandle;
		status = user_memcpy(user, &request, sizeof(request));
	}
	if (status != B_OK) {
		ReleaseImport(buffer);
		DeleteMemory(*buffer);
		free(buffer);
		return status;
	}
	buffer->handle = sNextHandle++;
	buffer->references = 1;
	buffer->account = client->account;
	buffer->account->references++;
	buffer->next = client->first;
	client->first = buffer;
	client->account->buffers++;
	client->account->importedBytes += bytes;
	sBuffers++;
	sImportedBytes += bytes;
	return B_OK;
}

static ClientVm**
FindVm(Client* client, uint32 handle)
{
	ClientVm** link = &client->firstVm;
	while (*link != NULL && (*link)->handle != handle)
		link = &(*link)->next;
	return link;
}

static status_t
CreateVm(Client* client, VmCreate& request)
{
	uint64 limit = request.userLimit != 0 ? request.userLimit : kVmUserLimit;
	if (request.flags != 0 || request.handle != 0 || request.reserved != 0
		|| request.reserved2 != 0 || (limit & (B_PAGE_SIZE - 1)) != 0
		|| limit > kVmUserLimit) {
		return B_BAD_VALUE;
	}
	if (client->vms == kMaxClientVms || sVms == kMaxGlobalVms || sNextHandle == 0) {
		LogRefusal(kRefusedVms, "VMs", client->vms == kMaxClientVms ? client->vms : sVms,
			client->vms == kMaxClientVms ? kMaxClientVms : kMaxGlobalVms);
		return B_NO_MEMORY;
	}
	ClientVm* vm = (ClientVm*)calloc(1, sizeof(ClientVm));
	if (vm == NULL)
		return B_NO_MEMORY;
	status_t status = CreateGeneration(NULL, 0, limit, 1, &vm->current);
	if (status != B_OK) {
		free(vm);
		return status;
	}
	vm->handle = sNextHandle++;
	vm->next = client->firstVm;
	client->firstVm = vm;
	client->vms++;
	sVms++;
	request.handle = vm->handle;
	request.userLimit = limit;
	return B_OK;
}

// The scratch arrays have room for a replacement to split one existing range
// into two pieces and insert a new mapping. Coalesce before applying the quota.
static status_t
ApplyVmOperation(Client* client, uint64 limit, const VmOperation& operation,
	const ClientMapping* input, uint32 inputCount, ClientMapping* output, uint32& count)
{
	if (operation.reserved != 0
		|| !GpuPageTable::ValidRange(operation.address, operation.bytes, limit)) {
		return B_BAD_VALUE;
	}
	ClientBuffer* buffer = NULL;
	if (operation.type == kVmMapOperation) {
		if (!GpuPageTable::ValidPermissions(operation.flags) || (operation.offset & 4095) != 0)
			return B_BAD_VALUE;
		buffer = *FindBuffer(client, operation.buffer);
		if (buffer == NULL)
			return B_ENTRY_NOT_FOUND;
		if (operation.offset > buffer->bytes || operation.bytes > buffer->bytes - operation.offset)
			return B_BAD_VALUE;
	} else if (operation.type != kVmUnmapOperation || operation.flags != 0
		|| operation.buffer != 0 || operation.offset != 0) {
		return B_BAD_VALUE;
	}
	uint64 start = operation.address, end = start + operation.bytes;
	count = 0;
	for (uint32 i = 0; i < inputCount; i++) {
		ClientMapping map = input[i];
		uint64 mapEnd = map.address + map.bytes;
		if (mapEnd <= start || map.address >= end) {
			output[count++] = map;
			continue;
		}
		if (map.address < start) {
			output[count] = map;
			output[count++].bytes = start - map.address;
		}
		if (mapEnd > end) {
			map.offset += end - map.address;
			map.address = end;
			map.bytes = mapEnd - end;
			output[count++] = map;
		}
	}
	if (buffer != NULL) {
		uint32 at = 0;
		while (at < count && output[at].address < start)
			at++;
		memmove(output + at + 1, output + at, (count - at) * sizeof(ClientMapping));
		output[at] = {start, operation.bytes, operation.offset, buffer, operation.flags};
		count++;
	}
	uint32 merged = 0;
	uint64 mappedBytes = 0;
	for (uint32 i = 0; i < count; i++) {
		ClientMapping map = output[i];
		if (map.bytes > kMaxVmMappedBytes - mappedBytes) {
			LogRefusal(kRefusedMappedBytes, "bytes mapped into one VM", mappedBytes,
				kMaxVmMappedBytes);
			return B_NO_MEMORY;
		}
		mappedBytes += map.bytes;
		if (merged != 0 && output[merged - 1].buffer == map.buffer
			&& output[merged - 1].flags == map.flags
			&& output[merged - 1].address + output[merged - 1].bytes == map.address
			&& output[merged - 1].offset + output[merged - 1].bytes == map.offset) {
			output[merged - 1].bytes += map.bytes;
		} else {
			output[merged++] = map;
		}
	}
	count = merged;
	if (count > kMaxVmMappings) {
		LogRefusal(kRefusedMappings, "mappings of one VM", count, kMaxVmMappings);
		return B_NO_MEMORY;
	}
	return B_OK;
}

static status_t
BindVm(Client* client, void* user, size_t length)
{
	VmBind request;
	if (length < sizeof(request))
		return B_BAD_VALUE;
	status_t status = ReadRequest(user, sizeof(request), request);
	if (status != B_OK)
		return status;
	if (request.flags != 0 || request.newGeneration != 0 || request.operationCount == 0
		|| request.operationCount > kMaxVmOperations
		|| length != sizeof(request) + request.operationCount * sizeof(VmOperation)) {
		return B_BAD_VALUE;
	}
	ClientVm* vm = *FindVm(client, request.handle);
	if (vm == NULL)
		return B_ENTRY_NOT_FOUND;
	ClientGeneration* old = vm->current;
	if (request.expectedGeneration != 0 && request.expectedGeneration != old->number)
		return B_BUSY;
	if (old->number == UINT64_MAX)
		return B_NO_MEMORY;
	VmOperation* operations = (VmOperation*)malloc(request.operationCount * sizeof(VmOperation));
	if (operations == NULL)
		return B_NO_MEMORY;
	status = user_memcpy(operations, (const uint8*)user + sizeof(request),
		request.operationCount * sizeof(VmOperation));
	if (status != B_OK) {
		free(operations);
		return status;
	}
	const uint32 capacity = kMaxVmMappings + 2;
	ClientMapping* scratch = (ClientMapping*)malloc(2 * capacity * sizeof(ClientMapping));
	if (scratch == NULL) {
		free(operations);
		return B_NO_MEMORY;
	}
	ClientMapping* current = scratch;
	ClientMapping* next = scratch + capacity;
	uint32 count = old->count;
	if (count != 0)
		memcpy(current, old->mappings, count * sizeof(ClientMapping));
	for (uint32 i = 0; status == B_OK && i < request.operationCount; i++) {
		uint32 nextCount;
		status = ApplyVmOperation(client, old->userLimit, operations[i], current, count,
			next, nextCount);
		if (status == B_OK) {
			ClientMapping* swap = current; current = next; next = swap;
			count = nextCount;
		}
	}
	ClientGeneration* candidate = NULL;
	if (status == B_OK) {
		status = CreateGeneration(current, count, old->userLimit, old->number + 1, &candidate, old->heaps);
	}
	free(scratch);
	free(operations);
	if (status != B_OK)
		return status;
	request.newGeneration = candidate->number;
	status = user_memcpy(user, &request, sizeof(request));
	if (status != B_OK) {
		ReleaseGeneration(candidate);
		return status;
	}
	// No hardware root changes here. A future submission leases this generation;
	// already queued work keeps its previous root until it has completed.
	vm->current = candidate;
	ReleaseGeneration(old);
	return B_OK;
}

status_t
MaliCSF::AcquireClientVm(void* cookie, uint32 handle, uint64 expectedGeneration,
	ClientVmLease& lease)
{
	MutexLocker locker(sClientLock);
	Client* client = (Client*)cookie;
	status_t status = CheckAccess(client);
	if (status != B_OK)
		return status;
	if (!client->writable)
		return B_NOT_ALLOWED;
	if (lease.state != NULL)
		return B_BAD_VALUE;
	ClientVm* vm = *FindVm(client, handle);
	if (vm == NULL)
		return B_ENTRY_NOT_FOUND;
	ClientGeneration* current = vm->current;
	if (expectedGeneration != 0 && expectedGeneration != current->number)
		return B_BUSY;
	if (current->references == UINT32_MAX)
		return B_NO_MEMORY;
	current->references++;
	lease.state = current;
	lease.rootPhysical = current->tables.pages[0];
	lease.generation = current->number;
	lease.userLimit = current->userLimit;
	return B_OK;
}

void
MaliCSF::ReleaseClientVm(ClientVmLease& lease)
{
	MutexLocker locker(sClientLock);
	if (lease.state != NULL)
		ReleaseGeneration((ClientGeneration*)lease.state);
	memset(&lease, 0, sizeof(lease));
}

const uint64*
MaliCSF::ClientVmRoot(const ClientVmLease& lease)
{
	const ClientGeneration* generation = (const ClientGeneration*)lease.state;
	return generation == NULL ? NULL : (const uint64*)generation->tables.address;
}

bool
MaliCSF::ClientVmRange(const ClientVmLease& lease, uint64 address, uint64 bytes)
{
	const ClientGeneration* generation = (const ClientGeneration*)lease.state;
	if (generation == NULL || bytes == 0 || address >= generation->userLimit
		|| bytes > generation->userLimit - address)
		return false;
	for (uint32 i = 0; i < generation->count; i++) {
		const ClientMapping& map = generation->mappings[i];
		uint64 end = map.address + map.bytes;
		if (address >= end)
			continue;
		if (address < map.address)
			return false;
		if (bytes <= end - address)
			return true;
		bytes -= end - address;
		address = end;
	}
	return false;
}

status_t
MaliCSF::PrepareClientHeapGrowth(const ClientVmLease& lease, uint64 context,
	uint32 vtStart, uint32 vtEnd, uint32 fragEnd, HeapGrowth& growth)
{
	MutexLocker locker(sClientLock);
	const ClientGeneration* generation = (const ClientGeneration*)lease.state;
	if (generation == NULL || growth.state != NULL || growth.address != 0
		|| growth.bytes != 0 || growth.encodedChunk != 0 || context < kHeapBase
		|| context - kHeapBase >= kMaxVmHeaps * kHeapSlotBytes
		|| (context - kHeapBase) % kHeapSlotBytes != 0
		|| fragEnd > vtEnd || vtEnd >= vtStart)
		return B_BAD_VALUE;
	ClientHeap* heap = generation->heaps[(context - kHeapBase) / kHeapSlotBytes];
	if (heap == NULL) return B_ENTRY_NOT_FOUND;
	if (vtStart - fragEnd > heap->targetInFlight) return B_NO_MEMORY;
	ClientHeapChunk* chunk = NULL;
	status_t status = PrepareHeapChunk(heap, false, &chunk);
	if (status != B_OK) return status;
	heap->references++;
	growth = {chunk, chunk->address, heap->chunkSize, chunk->address | (heap->chunkSize >> 12)};
	return B_OK;
}

status_t
MaliCSF::CommitClientHeapGrowth(HeapGrowth& growth)
{
	MutexLocker locker(sClientLock);
	ClientHeapChunk* chunk = (ClientHeapChunk*)growth.state;
	if (chunk == NULL || chunk->heap->pending != chunk || chunk->address != growth.address
		|| chunk->heap->chunkSize != growth.bytes
		|| growth.encodedChunk != (chunk->address | (chunk->heap->chunkSize >> 12)))
		return B_BAD_VALUE;
	ClientHeap* heap = chunk->heap;
	CommitHeapChunk(chunk);
	growth = {};
	ReleaseHeap(heap);
	return B_OK;
}

void
MaliCSF::AbortClientHeapGrowth(HeapGrowth& growth)
{
	MutexLocker locker(sClientLock);
	ClientHeapChunk* chunk = (ClientHeapChunk*)growth.state;
	if (chunk != NULL) {
		ClientHeap* heap = chunk->heap;
		heap->pending = NULL;
		DeleteHeapChunk(chunk);
		ReleaseHeap(heap);
	}
	growth = {};
}

static status_t
ControlHeaps(Client* client, uint32 op, void* user, size_t length)
{
	if (op == kGetHeapInfo) {
		HeapInfo request;
		status_t status = ReadRequest(user, length, request);
		if (status != B_OK) return status;
		if (request.reserved != 0 || request.reserved2 != 0
			|| ((request.vm == 0) != (request.handle == 0))) return B_BAD_VALUE;
		HeapInfo info = {};
		info.version = kClientVersion; info.vm = request.vm; info.handle = request.handle;
		if (request.vm != 0) {
			ClientVm* vm = *FindVm(client, request.vm);
			if (vm == NULL) return B_ENTRY_NOT_FOUND;
			ClientHeap* heap = NULL;
			for (unsigned i = 0; i < kMaxVmHeaps; i++) {
				ClientHeap* entry = vm->current->heaps[i];
				if (entry != NULL && entry->handle == request.handle) { heap = entry; break; }
			}
			if (heap == NULL) return B_ENTRY_NOT_FOUND;
			info.contextAddress = kHeapBase + heap->slot * kHeapSlotBytes;
			info.firstChunkAddress = heap->firstChunkAddress;
			info.bytes = B_PAGE_SIZE + uint64(heap->chunkCount) * heap->chunkSize;
			info.chunkSize = heap->chunkSize; info.initialChunks = heap->initialChunks;
			info.maxChunks = heap->maxChunks; info.chunkCount = heap->chunkCount;
			info.targetInFlight = heap->targetInFlight; info.slot = heap->slot;
			info.generation = vm->current->number;
		}
		info.clientHeaps = client->account->heaps; info.globalHeaps = sHeaps;
		info.clientBytes = client->account->heapBytes; info.globalBytes = sHeapBytes;
		info.globalChunks = sHeapChunks; info.globalTablePages = sHeapTablePages;
		info.globalHeapGenerations = sHeapGenerations;
		return user_memcpy(user, &info, sizeof(info));
	}
	if (!client->writable) return B_NOT_ALLOWED;
	if (op == kCreateHeap) {
		HeapCreate request;
		status_t status = ReadRequest(user, length, request);
		if (status != B_OK) return status;
		if (request.flags != 0 || request.handle != 0 || request.contextAddress != 0
			|| request.firstChunkAddress != 0 || request.generation != 0 || request.reserved != 0
			|| request.initialChunks == 0 || request.initialChunks > request.maxChunks
			|| request.maxChunks > kMaxHeapChunks || (request.chunkSize & 4095) != 0
			|| request.chunkSize < 128 * 1024 || request.chunkSize > 8 * 1024 * 1024)
			return B_BAD_VALUE;
		ClientVm* vm = *FindVm(client, request.vm);
		if (vm == NULL) return B_ENTRY_NOT_FOUND;
		ClientGeneration* old = vm->current;
		if (old->number == UINT64_MAX) return B_NO_MEMORY;
		unsigned slot = 0;
		while (slot < kMaxVmHeaps && old->heaps[slot] != NULL) slot++;
		if (slot == kMaxVmHeaps) return B_NO_MEMORY;
		ClientHeap* heap = NULL;
		status = NewHeap(client->account, request, slot, &heap);
		if (status != B_OK) return status;
		ClientHeap* members[kMaxVmHeaps];
		memcpy(members, old->heaps, sizeof(members)); members[slot] = heap;
		ClientGeneration* candidate = NULL;
		status = CreateGeneration(old->mappings, old->count, old->userLimit,
			old->number + 1, &candidate, members);
		if (status == B_OK) {
			request.handle = heap->handle; request.contextAddress = kHeapBase + slot * kHeapSlotBytes;
			request.firstChunkAddress = heap->firstChunkAddress; request.generation = candidate->number;
			status = user_memcpy(user, &request, sizeof(request));
		}
		ReleaseHeap(heap);
		if (status != B_OK) {
			if (candidate != NULL) ReleaseGeneration(candidate);
			return status;
		}
		vm->current = candidate; ReleaseGeneration(old);
		return B_OK;
	}
	if (op != kDestroyHeap) return B_DEV_INVALID_IOCTL;
	HeapHandle request;
	status_t status = ReadRequest(user, length, request);
	if (status != B_OK) return status;
	if (request.reserved != 0 || request.reserved2 != 0 || request.generation != 0) return B_BAD_VALUE;
	ClientVm* vm = *FindVm(client, request.vm);
	if (vm == NULL) return B_ENTRY_NOT_FOUND;
	ClientGeneration* old = vm->current;
	unsigned slot = 0;
	while (slot < kMaxVmHeaps && (old->heaps[slot] == NULL || old->heaps[slot]->handle != request.handle)) slot++;
	if (slot == kMaxVmHeaps) return B_ENTRY_NOT_FOUND;
	if (old->number == UINT64_MAX) return B_NO_MEMORY;
	ClientHeap* members[kMaxVmHeaps];
	memcpy(members, old->heaps, sizeof(members)); members[slot] = NULL;
	ClientGeneration* candidate = NULL;
	status = CreateGeneration(old->mappings, old->count, old->userLimit,
		old->number + 1, &candidate, members);
	if (status != B_OK) return status;
	request.generation = candidate->number;
	status = user_memcpy(user, &request, sizeof(request));
	if (status != B_OK) { ReleaseGeneration(candidate); return status; }
	vm->current = candidate; ReleaseGeneration(old);
	return B_OK;
}

status_t
MaliCSF::ControlClient(void* cookie, uint32 op, void* user, size_t length)
{
	MutexLocker locker(sClientLock);
	Client* client = (Client*)cookie;
	status_t status = CheckAccess(client);
	if (status != B_OK)
		return status;
	if (op >= kCreateHeap && op <= kGetHeapInfo)
		return ControlHeaps(client, op, user, length);
	if (op == kGetClientInfo) {
		ClientInfo request;
		status = ReadRequest(user, length, request);
		if (status != B_OK)
			return status;
		if (request.reserved[0] != 0 || request.reserved[1] != 0)
			return B_BAD_VALUE;
		ClientInfo info = {};
		info.version = kClientVersion;
		info.capabilities = client->writable ? kClientCpuBuffers | kClientVmMappings | kClientQueues | kClientSynchronization | kClientHeaps | kClientProperties | kClientCachedBuffers | kClientImportedBuffers : 0;
		info.maxBuffers = kMaxClientBuffers;
		info.maxBufferBytes = kMaxBufferBytes;
		info.maxClientBytes = kMaxClientBufferBytes;
		info.bufferCount = client->account->buffers;
		info.bufferBytes = client->account->bytes;
		info.globalBufferBytes = sBufferBytes;
		info.globalBuffers = sBuffers;
		info.globalClients = sClients;
		return user_memcpy(user, &info, sizeof(info));
	}
	if (op == kGetVmInfo) {
		VmInfo request;
		status = ReadRequest(user, length, request);
		if (status != B_OK)
			return status;
		if (request.reserved != 0)
			return B_BAD_VALUE;
		VmInfo info = {};
		info.version = kClientVersion;
		info.handle = request.handle;
		if (request.handle != 0) {
			ClientVm* vm = *FindVm(client, request.handle);
			if (vm == NULL)
				return B_ENTRY_NOT_FOUND;
			ClientGeneration* current = vm->current;
			info.userLimit = current->userLimit;
			info.generation = current->number;
			info.mappedBytes = current->mappedBytes;
			info.mappings = current->count;
			info.tablePages = (current->tables.bytes + current->heapTable.bytes) / B_PAGE_SIZE;
		}
		info.clientVms = client->vms;
		info.globalVms = sVms;
		info.globalGenerations = sGenerations;
		info.globalTablePages = sTablePages;
		return user_memcpy(user, &info, sizeof(info));
	}
	if ((op < kCreateBuffer || op > kGetBufferInfo) && (op < kCreateVm || op > kBindVm))
		return B_DEV_INVALID_IOCTL;
	if (!client->writable)
		return B_NOT_ALLOWED;
	if (op == kCreateVm) {
		VmCreate request;
		status = ReadRequest(user, length, request);
		if (status == B_OK)
			status = CreateVm(client, request);
		if (status != B_OK)
			return status;
		status = user_memcpy(user, &request, sizeof(request));
		if (status != B_OK)
			DeleteVm(client, FindVm(client, request.handle));
		return status;
	}
	if (op == kDestroyVm) {
		VmHandle request;
		status = ReadRequest(user, length, request);
		if (status != B_OK)
			return status;
		if (request.reserved != 0)
			return B_BAD_VALUE;
		ClientVm** link = FindVm(client, request.handle);
		if (*link == NULL)
			return B_ENTRY_NOT_FOUND;
		DeleteVm(client, link);
		return B_OK;
	}
	if (op == kBindVm)
		return BindVm(client, user, length);
	if (op == kCreateBuffer) {
		BufferCreate request;
		status = ReadRequest(user, length, request);
		if (status == B_OK)
			status = CreateBuffer(client, request);
		if (status != B_OK)
			return status;
		status = user_memcpy(user, &request, sizeof(request));
		if (status != B_OK)
			DeleteBuffer(FindBuffer(client, request.handle));
		return status;
	}
	if (op == kDestroyBuffer) {
		BufferHandle request;
		status = ReadRequest(user, length, request);
		if (status != B_OK)
			return status;
		if (request.reserved != 0)
			return B_BAD_VALUE;
		ClientBuffer** link = FindBuffer(client, request.handle);
		if (*link == NULL)
			return B_ENTRY_NOT_FOUND;
		DeleteBuffer(link);
		return B_OK;
	}
	if (op == kGetBufferInfo) {
		BufferInfo request;
		status = ReadRequest(user, length, request);
		if (status != B_OK)
			return status;
		if (request.reserved != 0)
			return B_BAD_VALUE;
		ClientBuffer* buffer = *FindBuffer(client, request.handle);
		if (buffer == NULL)
			return B_ENTRY_NOT_FOUND;
		request.bytes = buffer->bytes;
		request.flags = buffer->cached ? kBufferCpuCached : kBufferNormalNoncacheable;
		if (buffer->import != kNotImported)
			request.flags |= kBufferImported;
		return user_memcpy(user, &request, sizeof(request));
	}
	BufferMap request;
	status = ReadRequest(user, length, request);
	if (status != B_OK)
		return status;
	if (request.address != 0 || request.area != 0 || request.flags != 0
		|| request.bytes != 0 || request.reserved != 0) {
		return B_BAD_VALUE;
	}
	ClientBuffer* buffer = *FindBuffer(client, request.handle);
	if (buffer == NULL)
		return B_ENTRY_NOT_FOUND;
	// An import is the team's own memory, mapped already.
	if (buffer->import != kNotImported)
		return B_NOT_ALLOWED;
	void* address = NULL;
	area_id area = vm_clone_area(client->owner, "Mali CSF buffer mapping", &address,
		B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, 0, buffer->area, true);
	if (area < B_OK)
		return area;
	request.address = (addr_t)address;
	request.area = area;
	request.bytes = buffer->bytes;
	request.flags = buffer->cached ? kBufferCpuCached : kBufferNormalNoncacheable;
	status = user_memcpy(user, &request, sizeof(request));
	if (status != B_OK)
		vm_delete_area(client->owner, area, true);
	return status;
}
