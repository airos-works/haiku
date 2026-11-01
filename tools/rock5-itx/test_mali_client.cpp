/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <map>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using uint8 = uint8_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
using addr_t = uintptr_t;
using status_t = int32_t;
using area_id = int32_t;
using team_id = int32_t;
static const status_t B_OK = 0, B_BAD_VALUE = -1, B_BAD_ADDRESS = -2,
	B_NO_MEMORY = -3, B_NOT_ALLOWED = -4, B_ENTRY_NOT_FOUND = -5,
	B_NOT_SUPPORTED = -6, B_BUSY = -7, B_DEV_INVALID_IOCTL = -8;
static const uint32 B_PAGE_SIZE = 4096, B_ANY_ADDRESS = 0, B_FULL_LOCK = 1,
	B_KERNEL_READ_AREA = 16, B_KERNEL_WRITE_AREA = 32, B_READ_AREA = 1, B_WRITE_AREA = 2;
static const team_id B_SYSTEM_TEAM = 1;

#include "CsfClient.h"
#include "CsfPageTable.h"

static void memory_full_barrier() { std::atomic_thread_fence(std::memory_order_seq_cst); }

struct mutex { std::mutex value; };
#define MUTEX_INITIALIZER(name) {}
static thread_local unsigned sLockDepth;
static thread_local team_id sTeam = 42;
static team_id team_get_current_team_id() { return sTeam; }
struct MutexLocker {
	mutex& lock;
	explicit MutexLocker(mutex& value) : lock(value)
	{
		assert(sLockDepth == 0);
		lock.value.lock();
		sLockDepth++;
	}
	~MutexLocker() { assert(--sLockDepth == 0); lock.value.unlock(); }
};
static uint64 sNextRamPhysical = UINT64_C(0x182300000);
struct Ram {
	int fd;
	size_t bytes;
	uint64 physical;
	explicit Ram(size_t size) : bytes(size)
	{
		physical = sNextRamPhysical;
		sNextRamPhysical += bytes * 2 + 4096;
		fd = memfd_create("mali-client-fixture", 0);
		assert(fd >= 0 && ftruncate(fd, bytes) == 0);
	}
	~Ram() { close(fd); }
};
struct Area {
	std::shared_ptr<Ram> ram;
	void* address;
	team_id owner;
	bool noncacheable;
};
static std::map<int, Area> sAreas;
static int sNextArea = 10;
static unsigned sFailAllocation, sCopies, sFailCopy, sAreaCalls, sFailArea;
static bool sFailClone;
struct virtual_address_restrictions { uint32 address_specification; };
struct physical_address_restrictions { uint64 low_address, high_address; };
struct physical_entry { uint64 address; size_t size; };
static int AddArea(std::shared_ptr<Ram> ram, team_id owner, bool noncacheable, void** address)
{
	void* allocation = mmap(NULL, ram->bytes + 8192, PROT_NONE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(allocation != MAP_FAILED);
	*address = (char*)allocation + 4096;
	assert(mmap(*address, ram->bytes, PROT_READ | PROT_WRITE,
		MAP_FIXED | MAP_SHARED, ram->fd, 0) == *address);
	int id = sNextArea++;
	sAreas.emplace(id, Area{ram, *address, owner, noncacheable});
	return id;
}
static area_id create_area_etc(team_id team, const char*, size_t bytes, uint32 wiring,
	uint32 protection, uint32 flags, uint32 guard,
	const virtual_address_restrictions* virt, const physical_address_restrictions* phys,
	void** address)
{
	assert(sLockDepth == 1 && team == B_SYSTEM_TEAM && wiring == B_FULL_LOCK);
	assert(protection == (B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA));
	assert(flags == 0 && guard == 0 && virt->address_specification == B_ANY_ADDRESS);
	assert(phys->low_address == 0 && phys->high_address == 0);
	assert(bytes > 0 && bytes % 4096 == 0);
	if (++sAreaCalls == sFailArea || sFailAllocation == 1) return B_NO_MEMORY;
	return AddArea(std::make_shared<Ram>(bytes), team, false, address);
}
static status_t get_memory_map(void* address, size_t bytes, physical_entry* entry, int count)
{
	assert(sLockDepth == 1 && bytes == 4096 && count == 1);
	if (sFailAllocation == 2) return B_BAD_ADDRESS;
	for (const auto& pair : sAreas) {
		const Area& area = pair.second;
		uintptr_t offset = (uintptr_t)address - (uintptr_t)area.address;
		if (offset >= area.ram->bytes) continue;
		assert(area.owner == B_SYSTEM_TEAM && !area.noncacheable && offset % 4096 == 0);
		entry->address = area.ram->physical + offset * 2; // scattered, above 4 GiB
		entry->size = sFailAllocation == 3 ? 4095 : 4096;
		if (sFailAllocation == 4) entry->address++;
		if (sFailAllocation == 5) entry->address = UINT64_C(1) << 40;
		return B_OK;
	}
	assert(false); return B_BAD_ADDRESS;
}
static status_t MakeClientRamNoncacheable(area_id id, void* address, size_t bytes)
{
	Area& area = sAreas.at(id);
	assert(sLockDepth == 1 && address == area.address && bytes == area.ram->bytes);
	assert(!area.noncacheable && area.owner == B_SYSTEM_TEAM);
	if (sFailAllocation == 6) return B_NOT_SUPPORTED;
	area.noncacheable = true;
	return B_OK;
}
static status_t delete_area(area_id id)
{
	auto it = sAreas.find(id);
	assert(it != sAreas.end());
	Area& area = it->second;
	assert(munmap((char*)area.address - 4096, area.ram->bytes + 8192) == 0);
	sAreas.erase(it);
	return B_OK;
}
static area_id vm_clone_area(team_id team, const char*, void** address, uint32 spec,
	uint32 protection, uint32 mapping, area_id source, bool kernel)
{
	assert(sLockDepth == 1 && kernel && team == sTeam && spec == B_ANY_ADDRESS);
	assert(protection == (B_READ_AREA | B_WRITE_AREA) && mapping == 0);
	const Area& area = sAreas.at(source);
	assert(area.owner == B_SYSTEM_TEAM && area.noncacheable);
	if (sFailClone) return B_NO_MEMORY;
	return AddArea(area.ram, team, area.noncacheable, address);
}
static status_t vm_delete_area(team_id team, area_id area, bool kernel)
{
	assert(sLockDepth == 1 && kernel && sAreas.at(area).owner == team);
	return delete_area(area);
}
static status_t user_memcpy(void* out, const void* in, size_t bytes)
{
	assert(sLockDepth == 1);
	if (++sCopies == sFailCopy || out == NULL || in == NULL) return B_BAD_ADDRESS;
	memcpy(out, in, bytes);
	return B_OK;
}

static std::atomic<unsigned> sHeapCalls{0}, sFailHeap{0};
static void* ClientMalloc(size_t bytes)
{
	return ++sHeapCalls == sFailHeap ? NULL : malloc(bytes);
}
static void* ClientCalloc(size_t count, size_t bytes)
{
	return ++sHeapCalls == sFailHeap ? NULL : calloc(count, bytes);
}
static void dprintf(const char*, ...) {}
#define malloc ClientMalloc
#define calloc ClientCalloc
#include "client.inc"
#undef malloc
#undef calloc

template<typename T> static status_t Call(void* client, uint32 op, T& value)
{
	sCopies = 0;
	return ControlClient(client, op, &value, sizeof(value));
}
static void Empty()
{
	assert(sClients == 0 && sBuffers == 0 && sBufferBytes == 0 && sAreas.empty());
	assert(sVms == 0 && sGenerations == 0 && sTablePages == 0 && sAccounts == 0);
	assert(sHeaps == 0 && sHeapChunks == 0 && sHeapTablePages == 0 && sHeapGenerations == 0 && sHeapBytes == 0);
}
static BufferCreate Create(void* client, uint64 bytes)
{
	BufferCreate value{}; value.version = 1; value.bytes = bytes;
	assert(Call(client, kCreateBuffer, value) == B_OK);
	assert(value.handle != 0 && value.bytes == ((bytes + 4095) & ~UINT64_C(4095)));
	return value;
}
static BufferMap Map(void* client, uint32 handle)
{
	BufferMap value{}; value.version = 1; value.handle = handle;
	assert(Call(client, kMapBuffer, value) == B_OK);
	assert(value.flags == kBufferNormalNoncacheable && value.area >= 0);
	return value;
}
static void Destroy(void* client, uint32 handle)
{
	BufferHandle value{1, handle, 0};
	assert(Call(client, kDestroyBuffer, value) == B_OK);
	assert(Call(client, kDestroyBuffer, value) == B_ENTRY_NOT_FOUND);
}

#include "test_mali_vm.inc"
#include "test_mali_heaps.inc"

int main()
{
	CheckHeaps();
	Empty();
	void *a, *b;
	assert(OpenClient(true, &a) == B_OK && OpenClient(true, &b) == B_OK && a != b);
	ClientInfo info{}; info.version = 1;
	assert(Call(a, kGetClientInfo, info) == B_OK
		&& info.capabilities == (kClientCpuBuffers | kClientVmMappings | kClientQueues | kClientSynchronization | kClientHeaps | kClientProperties));
	assert(info.globalClients == 2 && info.globalBuffers == 0);
	BufferCreate bad{}; bad.version = 1; bad.bytes = 4096;
	for (size_t size : {size_t(0), sizeof(bad) - 1, sizeof(bad) + 1})
		assert(ControlClient(a, kCreateBuffer, &bad, size) == B_BAD_VALUE);
	assert(ControlClient(a, kCreateBuffer, NULL, sizeof(bad)) == B_BAD_ADDRESS);
	for (unsigned fail = 1; fail <= 6; fail++) {
		sFailAllocation = fail;
		assert(Call(a, kCreateBuffer, bad) != B_OK && sAreas.empty() && sBuffers == 0);
	}
	sFailAllocation = 0;
	for (unsigned fail = 1; fail <= 2; fail++) {
		sFailCopy = fail;
		assert(Call(a, kCreateBuffer, bad) == B_BAD_ADDRESS && sAreas.empty() && sBuffers == 0);
	}
	sFailCopy = 0;
	for (uint64 bytes : {UINT64_C(0), kMaxBufferBytes + 1, UINT64_MAX}) {
		bad.bytes = bytes;
		assert(Call(a, kCreateBuffer, bad) == B_BAD_VALUE);
	}
	bad.bytes = 4096;
	for (unsigned field = 0; field < 5; field++) {
		BufferCreate value = bad;
		if (field == 0) value.version++;
		if (field == 1) value.flags = 1;
		if (field == 2) value.handle = 1;
		if (field == 3) value.reserved = 1;
		if (field == 4) value.reserved2 = 1;
		assert(Call(a, kCreateBuffer, value) == B_BAD_VALUE);
	}
	auto buffer = Create(a, 8193);
	Client* actual = (Client*)a;
	for (unsigned page = 0; page < 3; page++)
		assert(actual->first->pages[page]
			== sAreas.at(actual->first->area).ram->physical + page * 8192);
	BufferHandle handle{1, buffer.handle, 0};
	assert(Call(b, kDestroyBuffer, handle) == B_ENTRY_NOT_FOUND);
	BufferMap foreign{}; foreign.version = 1; foreign.handle = buffer.handle;
	assert(Call(b, kMapBuffer, foreign) == B_ENTRY_NOT_FOUND);
	sTeam++;
	assert(AccessClient(a) == B_NOT_ALLOWED);
	assert(Call(a, kDestroyBuffer, handle) == B_NOT_ALLOWED);
	sTeam--;
	for (unsigned fail = 0; fail < 2; fail++) {
		sFailClone = fail == 0; sFailCopy = fail == 1 ? 2 : 0;
		assert(Call(a, kMapBuffer, foreign) != B_OK && sAreas.size() == 1 && sBuffers == 1);
	}
	sFailClone = false; sFailCopy = 0;
	auto x = Map(a, buffer.handle), y = Map(a, buffer.handle);
	for (size_t i = 0; i < buffer.bytes; i++) {
		assert(((uint8*)(uintptr_t)x.address)[i] == 0);
		((uint8*)(uintptr_t)x.address)[i] = uint8(i * 17 + 3);
	}
	assert(memcmp((void*)(uintptr_t)y.address, actual->first->address, buffer.bytes) == 0);
	Destroy(a, buffer.handle);
	assert(sBuffers == 0 && sAreas.size() == 2);
	CloseClient(a); CloseClient(a);
	assert(AccessClient(a) == B_NOT_ALLOWED);
	FreeClient(a); FreeClient(b);
	for (size_t i = 0; i < buffer.bytes; i++)
		assert(((uint8*)(uintptr_t)y.address)[i] == uint8(i * 17 + 3));
	delete_area(x.area); delete_area(y.area); Empty();
	assert(OpenClient(false, &a) == B_OK);
	assert(Call(a, kCreateBuffer, bad) == B_NOT_ALLOWED);
	assert(Call(a, kGetClientInfo, info) == B_OK && info.capabilities == 0);
	FreeClient(a); Empty();
	assert(OpenClient(true, &a) == B_OK);
	for (unsigned i = 0; i < kMaxClientBuffers; i++) Create(a, 1);
	assert(Call(a, kCreateBuffer, bad) == B_NO_MEMORY);
	FreeClient(a); Empty();
	assert(OpenClient(true, &a) == B_OK && OpenClient(true, &b) == B_OK);
	for (unsigned i = 0; i < 4; i++) Create(a, kMaxBufferBytes);
	assert(Call(a, kCreateBuffer, bad) == B_NO_MEMORY);
	for (unsigned i = 0; i < 4; i++) Create(b, kMaxBufferBytes);
	void* c; assert(OpenClient(true, &c) == B_OK);
	assert(Call(c, kCreateBuffer, bad) == B_NO_MEMORY);
	FreeClient(a); FreeClient(b); FreeClient(c); Empty();
	std::vector<void*> clients;
	for (unsigned i = 0; i < kMaxClients; i++) {
		assert(OpenClient(false, &a) == B_OK); clients.push_back(a);
	}
	assert(OpenClient(false, &a) == B_BUSY);
	for (void* client : clients) FreeClient(client);
	Empty();
	// Concurrent opens/allocations/closes use the real production lock scope.
	std::vector<std::thread> threads;
	for (unsigned n = 0; n < 8; n++) threads.emplace_back([n] {
		sTeam = 100 + n;
		for (unsigned i = 0; i < 32; i++) {
			void* client; assert(OpenClient(true, &client) == B_OK);
			BufferCreate value{}; value.version = 1; value.bytes = 16385;
			assert(ControlClient(client, kCreateBuffer, &value, sizeof(value)) == B_OK);
			CloseClient(client); FreeClient(client);
		}
	});
	for (auto& thread : threads) thread.join();
	Empty();
	CheckVms();
	sNextHandle = UINT32_MAX;
	assert(OpenClient(true, &a) == B_OK);
	assert(Create(a, 1).handle == UINT32_MAX);
	assert(Call(a, kCreateBuffer, bad) == B_NO_MEMORY);
	FreeClient(a); Empty();
	puts("MALI_CSF_CLIENT_TEST_PASS");
}
