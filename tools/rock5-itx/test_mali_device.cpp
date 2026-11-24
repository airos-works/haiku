/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#include "CsfRun.h"
#include "CsfCommands.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define main MaliMemoryFixtureMain
#include "test_mali_memory.cpp"
#undef main

using int32 = int32_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
using uint8 = uint8_t;
using status_t = int32_t;
using area_id = int32_t;
using phys_addr_t = uint64_t;
static const int B_OK = 0, B_NOT_SUPPORTED = -1, B_BAD_VALUE = -2,
	B_BAD_DATA = -3, B_BAD_ADDRESS = -4, B_NO_MEMORY = -5, B_NOT_ALLOWED = -6;
static const unsigned B_SYSTEM_TEAM = 2, B_CONTIGUOUS = 3, B_PAGE_SIZE = 4096,
	B_KERNEL_READ_AREA = 4, B_KERNEL_WRITE_AREA = 8,
	B_ANY_KERNEL_ADDRESS = 16, B_UNCACHED_MEMORY = 32;
static const int B_HANDLED_INTERRUPT = 1, B_UNHANDLED_INTERRUPT = 0;
#define B_PRIx64 PRIx64

struct Area { Guarded* mapping; std::string name; bool dma; };
static std::map<int, Area> sAreas;
static int sNextArea = 1, sFailMap = 0, sMaps = 0, sUid = 0, sCopies = 0;
static int sAllocationFailure = 0, sPrepared = 0;
static uint64_t sPhysical = UINT64_C(0x182300000);
static volatile uint32_t* sGpu;
static int find_area(const char* name)
{
	for (auto& entry : sAreas) if (entry.second.name == name) return entry.first;
	return B_BAD_VALUE;
}
static int delete_area(int id)
{
	assert(sAreas.count(id)); delete sAreas.at(id).mapping; sAreas.erase(id); return 0;
}
class AreaDeleter {
public:
	explicit AreaDeleter(int area = -1) : fArea(area) {}
	~AreaDeleter() { SetTo(-1); }
	void SetTo(int area) { if (fArea >= 0) delete_area(fArea); fArea = area; }
	int Get() const { return fArea; }
private:
	int fArea;
};
static area_id map_physical_memory(const char* name, uint64_t physical, size_t bytes,
	uint32 spec, uint32 protection, void** address)
{
	assert(spec == (B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY));
	assert(protection == (B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA));
	assert(physical == 0xfd7c0000 || physical == 0xfd8d8000
		|| physical == 0xfb000000 || physical == 0xfb080000);
	assert(bytes == (physical == 0xfb000000 ? 12288 : 4096));
	if (++sMaps == sFailMap) return B_NO_MEMORY;
	Guarded* guarded = new Guarded(bytes); memset(guarded->data, 0, bytes);
	*address = guarded->data;
	if (physical == 0xfb000000) sGpu = (volatile uint32_t*)*address;
	int id = sNextArea++; sAreas.emplace(id, Area{guarded, name, false}); return id;
}
struct virtual_address_restrictions { uint64_t unused; };
struct physical_address_restrictions { uint64_t low_address, high_address; };
struct physical_entry { uint64_t address; size_t size; };
static area_id create_area_etc(int team, const char* name, size_t bytes, unsigned lock,
	uint32 protection, int flags, int addressSpec, virtual_address_restrictions* virt,
	physical_address_restrictions* phys, void** address)
{
	assert(team == B_SYSTEM_TEAM && lock == B_CONTIGUOUS && bytes > 0 && !(bytes & 4095));
	assert(protection == (B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA) && flags == 0 && addressSpec == 0);
	assert(virt->unused == 0 && phys->low_address == 0 && phys->high_address == (UINT64_C(1) << 40));
	if (sAllocationFailure == 1) return B_NO_MEMORY;
	Guarded* guarded = new Guarded(bytes); *address = guarded->data;
	int id = sNextArea++; sAreas.emplace(id, Area{guarded, name, true}); return id;
}
static status_t get_memory_map(void* address, size_t bytes, physical_entry* entry, unsigned count)
{
	assert(count == 1 && address && bytes);
	entry->address = sPhysical;
	entry->size = sAllocationFailure == 3 ? bytes - 1 : bytes;
	return sAllocationFailure == 2 ? B_BAD_VALUE : B_OK;
}
static status_t MakeFirmwareRamNoncacheable(int area, void* address, uint64 physical, size_t bytes)
{
	assert(sAreas.at(area).dma && sAreas.at(area).mapping->data == address);
	assert(physical == sPhysical && sAreas.at(area).mapping->size == bytes);
	sPrepared++;
	return sAllocationFailure == 4 ? B_BAD_VALUE : B_OK;
}
static void memory_read_barrier() {}
static void memory_write_barrier() {}
static void memory_full_barrier() {}
static int64_t system_time() { return 12345; }
static void spin(int) { assert(false); }
static void snooze(int) { assert(false); }
static void kernel_dprintf(const char*, ...) {}
#define dprintf kernel_dprintf
static int TestGeteuid() { return sUid; }
#define geteuid TestGeteuid
static status_t user_memcpy(void* output, const void* input, size_t bytes)
{
	sCopies++;
	if (output == NULL || input == NULL) return B_BAD_ADDRESS;
	memcpy(output, input, bytes); return B_OK;
}
struct spinlock { std::mutex lock; };
#define B_SPINLOCK_INITIALIZER {}
static thread_local unsigned sSpinDepth;
static std::atomic<unsigned> sSpinAttempts(0);
class InterruptsSpinLocker {
public:
	explicit InterruptsSpinLocker(spinlock& lock) : fLock(lock)
	{
		sSpinAttempts++; fLock.lock.lock(); assert(sSpinDepth++ == 0);
	}
	~InterruptsSpinLocker() { if (fHeld) Unlock(); }
	void Unlock() { assert(fHeld && --sSpinDepth == 0); fHeld = false; fLock.lock.unlock(); }
private:
	spinlock& fLock;
	bool fHeld = true;
};
// The kernel condition variable as the job wait (#182) uses it: Add() before
// the last look, Wait() is B_OK when notified since Add(), else B_TIMED_OUT.
static const uint32 B_RELATIVE_TIMEOUT = 8;
static const int B_TIMED_OUT = -12;
static int32 atomic_get(int32* value) { return __atomic_load_n(value, __ATOMIC_SEQ_CST); }
static int32 atomic_add(int32* value, int32 add)
{
	assert(sSpinDepth == 1); // counted under the handler's lock
	return __atomic_fetch_add(value, add, __ATOMIC_SEQ_CST);
}
struct ConditionVariableEntry;
struct ConditionVariable {
	std::mutex lock;
	std::condition_variable changed;
	uint64_t notifications = 0;
	void Init(const void*, const char*) {}
	void Add(ConditionVariableEntry* entry);
	int32 NotifyAll()
	{
		assert(sSpinDepth == 0); // never under the handler's spinlock
		{ std::lock_guard<std::mutex> guard(lock); notifications++; }
		changed.notify_all(); return 0;
	}
};
struct ConditionVariableEntry {
	ConditionVariable* variable = NULL;
	uint64_t seen = 0;
	status_t Wait(uint32 flags, int64_t micros)
	{
		assert(flags == B_RELATIVE_TIMEOUT && variable != NULL);
		std::unique_lock<std::mutex> guard(variable->lock);
		return variable->changed.wait_for(guard, std::chrono::microseconds(micros),
			[&] { return variable->notifications != seen; }) ? B_OK : B_TIMED_OUT;
	}
};
void ConditionVariable::Add(ConditionVariableEntry* entry)
{
	std::lock_guard<std::mutex> guard(lock); entry->variable = this; entry->seen = notifications;
}
static std::atomic<bool> sPauseCpu(false), sEntered(false), sRelease(false);
static int smp_get_current_cpu()
{
	assert(sSpinDepth == 1);
	if (sPauseCpu) { sEntered = true; while (!sRelease) std::this_thread::yield(); }
	return 4;
}
using Handler = int32 (*)(void*);
static std::mutex sVector;
static std::map<int, std::pair<Handler, void*>> sHandlers;
static int sInstallFailure = 0, sInstalls = 0;
static int install_io_interrupt_handler(int irq, Handler handler, void* cookie, unsigned flags)
{
	assert(sAreas.size() >= 4 && !sHandlers.count(irq) && irq >= 124 && irq <= 126 && !flags);
	if (++sInstalls == sInstallFailure) return B_NO_MEMORY;
	std::lock_guard<std::mutex> lock(sVector); sHandlers[irq] = {handler, cookie}; return B_OK;
}
static int remove_io_interrupt_handler(int irq, Handler handler, void* cookie)
{
	assert(sSpinDepth == 0 && sAreas.size() >= 4);
	std::lock_guard<std::mutex> lock(sVector);
	assert(sHandlers.at(irq) == std::make_pair(handler, cookie)); sHandlers.erase(irq); return 0;
}
static int Invoke(int irq)
{
	std::lock_guard<std::mutex> lock(sVector);
	return sHandlers.at(irq).first(sHandlers.at(irq).second);
}

#include "CsfDevice.h"
#include "device.inc"

static ResourceInfo resources()
{
	ResourceInfo r = {};
	r.version = r.flags = 1; r.gpuBase = 0xfb000000; r.gpuSize = 0x200000;
	r.clockBase = 0xfd7c0000; r.clockSize = 0x5c000; r.powerBase = 0xfd8d8000;
	r.powerSize = 0x400; r.interruptBase = 0xfe600000;
	for (unsigned i = 0; i < 3; i++) { r.interrupts[i] = 124 + i; r.clockIds[i] = 262 + i; }
	r.powerDomain = 12; r.supplyPhandle = 33; r.supplyMinMicrovolt = 550000; r.supplyMaxMicrovolt = 950000;
	strcpy(r.supplyName, "vdd_gpu_s0"); strcpy(r.boardCompatible, "radxa,rock-5-itx");
	assert(ResourcesMatch(r)); return r;
}

int main()
{
	auto bytes = container({{0x800000, 4096, 13}, {0x4000000, 65536, 0xc000001b}});
	FirmwareImage image; assert(image.Init(bytes.data(), bytes.size()) == FIRMWARE_OK);
	FirmwareMemory memory; assert(memory.Plan(image));
	for (sAllocationFailure = 0; sAllocationFailure <= 4; sAllocationFailure++) {
		int area = AllocateFirmwareMemory(memory);
		if (sAllocationFailure == 0) {
			assert(area >= 0 && FirmwareMemoryRetained() && memory.RootPhysical() == sPhysical);
			delete_area(area);
		} else assert(area < 0);
		assert(sAreas.empty() && !FirmwareMemoryRetained());
	}
	sAllocationFailure = 0;
	for (uint64_t physical : {UINT64_C(1), UINT64_C(0x10000000000), UINT64_C(0xfffffff000)}) {
		sPhysical = physical; int prepared = sPrepared;
		assert(AllocateFirmwareMemory(memory) < 0 && sAreas.empty() && sPrepared == prepared);
	}
	sPhysical = UINT64_C(0x182300000);
	bool recovery = false;
	sUid = 5;
	assert(RunFirmwareRequest(resources(), NULL, 0, recovery) == B_NOT_ALLOWED && sCopies == 0);
	sUid = 0;
	assert(RunFirmwareRequest(resources(), NULL, sizeof(FirmwareRunInfo), recovery) == B_BAD_ADDRESS);
	vector<uint8_t> packet(sizeof(FirmwareRunInfo) + bytes.size(), 0);
	put(packet, 0, 1); put(packet, 4, bytes.size());
	memcpy(packet.data() + sizeof(FirmwareRunInfo), bytes.data(), bytes.size());
	assert(RunFirmwareRequest(resources(), packet.data(), sizeof(FirmwareRunInfo) - 1, recovery) == B_BAD_VALUE);
	assert(RunFirmwareRequest(resources(), packet.data(), packet.size() - 1, recovery) == B_BAD_VALUE);
	assert(RunFirmwareRequest({}, packet.data(), packet.size(), recovery) == B_NOT_SUPPORTED);
	assert(sAreas.empty() && !recovery);
	CommandMemory commands;
	assert(commands.Plan(image));
	int combined = AllocateFirmwareMemory(commands);
	assert(combined >= 0 && FirmwareMemoryRetained());
	assert(commands.Firmware().RootPhysical() == sPhysical);
	assert(commands.RootPhysical() == sPhysical + commands.Firmware().RequiredBytes());
	assert(sAreas.at(combined).mapping->size == commands.RequiredBytes());
	delete_area(combined);
	sUid = 5;
	assert(RunCommandRequest(resources(), NULL, 0, recovery) == B_NOT_ALLOWED);
	assert(RunCommandRequest(resources(), NULL, 0, recovery, true) == B_NOT_ALLOWED);
	sUid = 0;
	assert(RunCommandRequest(resources(), NULL, sizeof(CommandRunInfo), recovery) == B_BAD_ADDRESS);
	assert(RunCommandRequest(resources(), NULL, sizeof(CommandRunInfo), recovery, true) == B_BAD_ADDRESS);
	vector<uint8_t> commandPacket(sizeof(CommandRunInfo) + bytes.size(), 0);
	put(commandPacket, 0, 1); put(commandPacket, 4, bytes.size());
	memcpy(commandPacket.data() + sizeof(CommandRunInfo), bytes.data(), bytes.size());
	assert(RunCommandRequest(resources(), commandPacket.data(), sizeof(CommandRunInfo) - 1, recovery) == B_BAD_VALUE);
	assert(RunCommandRequest(resources(), commandPacket.data(), commandPacket.size() - 1, recovery) == B_BAD_VALUE);
	assert(RunCommandRequest(resources(), commandPacket.data(), commandPacket.size() - 1, recovery, true) == B_BAD_VALUE);
	assert(RunCommandRequest({}, commandPacket.data(), commandPacket.size(), recovery) == B_NOT_SUPPORTED);
	assert(RunCommandRequest({}, commandPacket.data(), commandPacket.size(), recovery, true) == B_NOT_SUPPORTED);
	assert(sAreas.empty() && !recovery);
	put(commandPacket, 0, 2);
	assert(RunCommandRequest({}, commandPacket.data(), commandPacket.size(), recovery) == B_BAD_VALUE);
	put(commandPacket, 0, 1); commandPacket[sizeof(CommandRunInfo)] ^= 1;
	assert(RunCommandRequest({}, commandPacket.data(), commandPacket.size(), recovery) == B_BAD_DATA);
	assert(RunCommandRequest({}, commandPacket.data(), commandPacket.size(), recovery, true) == B_BAD_DATA);
	assert(sAreas.empty() && !recovery);
	for (unsigned failure = 1; failure <= 4; failure++) {
		sFailMap = failure; sMaps = 0;
		{ FirmwareHardware io;
			status_t status = io.Init(resources());
			if (failure <= 2) assert(status == B_NO_MEMORY);
			else { assert(status == B_OK); assert(!io.MapGpu()); }
		}
		assert(sAreas.empty());
	}
	sFailMap = 0;
	for (int failure = 0; failure <= 3; failure++) {
		sInstalls = 0; sInstallFailure = failure;
		{ FirmwareHardware io;
			assert(io.Init(resources()) == B_OK && io.MapGpu());
			assert(io.InstallFirmwareHandlers() == (failure == 0));
			if (!failure) {
				for (int irq = 124; irq <= 126; irq++) assert(Invoke(irq) == B_UNHANDLED_INTERRUPT);
				sGpu[0x2000 / 4] = 65536;
				assert(!io.FirmwareFaulted());
				assert(io.ReadFirmwareCapture(1).raw == 0);
				sGpu[0x2000 / 4] |= 1;
				sGpu[0x241c / 4] = 0x345;
				sGpu[0x2420 / 4] = 0x12340000;
				assert(io.FirmwareFaulted() && !io.ArmFirmwareInterrupts());
				auto polled = io.ReadFirmwareCapture(1);
				assert(polled.count == 0 && polled.cpu == -1 && polled.raw == 65537);
				assert(polled.deviceStatus == 0x345 && polled.address == 0x12340000);
				sGpu[0x2000 / 4] = 65536;
				assert(io.ArmFirmwareInterrupts());
				for (unsigned i = 0; i < 3; i++) {
					unsigned raw = i == 0 ? 0x1000 : i == 1 ? 0x2000 : 0x20;
					uint32_t value = i == 0 ? 0x80000000 : 1;
					sGpu[raw / 4] = sGpu[(raw + 12) / 4] = value;
					sGpu[0x241c / 4] = 0x123; sGpu[0x2420 / 4] = 0xabcdef00;
					sGpu[0x2424 / 4] = 3; sGpu[0x2438 / 4] = 0x456;
					sGpu[0x3c / 4] = 0xabc; sGpu[0x40 / 4] = 0x76543210;
					assert(Invoke(124 + i) == B_HANDLED_INTERRUPT);
					auto capture = io.ReadFirmwareCapture(i);
					assert(capture.count == 1 && capture.status == value && capture.raw == value);
					assert(capture.cpu == 4 && capture.whenMicros == 12345);
					assert(sGpu[(raw + 8) / 4] == 0 && sGpu[(raw + 4) / 4] == value);
					if (i == 1) assert(capture.deviceStatus == 0x123 && capture.address == UINT64_C(0x3abcdef00) && capture.extra == 0x456);
					if (i == 2) assert(capture.deviceStatus == 0xabc && capture.address == 0x76543210);
					assert(Invoke(124 + i) == B_UNHANDLED_INTERRUPT);
					sGpu[raw / 4] = sGpu[(raw + 12) / 4] = 0;
				}
				assert(io.ArmFirmwareJob());
				sGpu[0x1000 / 4] = sGpu[0x100c / 4] = 0x80000000;
				sPauseCpu = true;
				std::thread irq([] { assert(Invoke(124) == B_HANDLED_INTERRUPT); });
				while (!sEntered) std::this_thread::yield();
				unsigned attempts = sSpinAttempts;
				std::atomic<bool> stopped(false);
				std::thread removal([&] { io.StopFirmwareHandlers(); stopped = true; });
				while (sSpinAttempts == attempts) std::this_thread::yield();
				assert(!stopped && sAreas.size() == 4);
				sRelease = true; irq.join(); removal.join(); assert(stopped);
			} else io.StopFirmwareHandlers();
			assert(sHandlers.empty()); io.UnmapGpu(); assert(sAreas.size() == 2);
		}
		assert(sAreas.empty());
	}
	sInstalls = sInstallFailure = 0;
	sPauseCpu = false;
	{ FirmwareHardware io(true);
		assert(io.Init(resources()) == B_OK && io.MapGpu() && io.InstallFirmwareHandlers());
		sGpu[0x2000 / 4] = 3u << 16;
		assert(io.ArmFirmwareInterrupts() && sGpu[0x2008 / 4] == 3);
		sGpu[0x1000 / 4] = sGpu[0x100c / 4] = 0x80000000;
		assert(Invoke(124) == B_HANDLED_INTERRUPT);
		sGpu[0x1000 / 4] = sGpu[0x100c / 4] = 0;
		assert(io.ArmCommandJob());
		for (unsigned i = 0; i < 100; i++) {
			uint32_t status = i & 1 ? 1 : 0x80000000;
			sGpu[0x1000 / 4] = sGpu[0x100c / 4] = status;
			assert(Invoke(124) == B_HANDLED_INTERRUPT);
			assert(sGpu[0x1008 / 4] == 0x80000001 && sGpu[0x1004 / 4] == status);
			sGpu[0x1000 / 4] = sGpu[0x100c / 4] = 0;
			assert(Invoke(124) == B_UNHANDLED_INTERRUPT);
		}
		assert(io.CommandJobCount() == 50 && io.ReadFirmwareCapture(0).count == 100);
		// #182: off, the job interrupt neither counts itself nor wakes anyone.
		assert(io.JobInterrupts() == 0);
		io.SetJobNotification(true);
		uint32 seen = io.JobInterrupts();
		auto started = std::chrono::steady_clock::now();
		assert(!io.WaitJobInterrupt(seen, 2000)); // nothing came: the bound ends it
		sGpu[0x1000 / 4] = sGpu[0x100c / 4] = 1;
		assert(Invoke(124) == B_HANDLED_INTERRUPT && io.JobInterrupts() == seen + 1);
		sGpu[0x1000 / 4] = sGpu[0x100c / 4] = 0;
		assert(Invoke(124) == B_UNHANDLED_INTERRUPT && io.JobInterrupts() == seen + 1);
		// It came after the count was read and before the sleep: no sleep.
		assert(io.WaitJobInterrupt(seen, 10000000));
		seen = io.JobInterrupts();
		std::atomic<bool> woken(false);
		started = std::chrono::steady_clock::now();
		std::thread sleeper([&] { woken = io.WaitJobInterrupt(seen, 10000000); });
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		sGpu[0x1000 / 4] = sGpu[0x100c / 4] = 0x80000001;
		assert(Invoke(124) == B_HANDLED_INTERRUPT);
		sGpu[0x1000 / 4] = sGpu[0x100c / 4] = 0;
		sleeper.join();
		auto slept = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - started).count();
		assert(woken && slept < 5000 && io.JobInterrupts() == seen + 1);
		printf("job interrupt woke a sleeper after %lld ms\n", (long long)slept);
		assert(io.CommandJobCount() == 52);
		io.SetJobNotification(false);
		assert(io.ReadFirmwareCapture(0).raw == 0x80000001);
		sGpu[0x2000 / 4] |= 2; sGpu[0x200c / 4] = 2;
		sGpu[0x245c / 4] = 0x123; sGpu[0x2460 / 4] = 0x12345678;
		sGpu[0x2464 / 4] = 5; sGpu[0x2478 / 4] = 0x98765432;
		auto polled = io.ReadFirmwareCapture(1);
		assert(polled.count == 0 && polled.deviceStatus == 0x123 && polled.address == UINT64_C(0x512345678));
		assert(Invoke(125) == B_HANDLED_INTERRUPT);
		auto fault = io.ReadFirmwareCapture(1);
		assert(fault.count == 1 && fault.status == 2 && fault.deviceStatus == 0x123);
		assert(fault.address == UINT64_C(0x512345678) && fault.extra == 0x98765432);
		assert(io.FirmwareFaulted());
		io.StopFirmwareHandlers();
		assert(sHandlers.empty() && sGpu[0x1008 / 4] == 0);
		io.UnmapGpu();
	}
	assert(sAreas.empty());
	puts("MALI_CSF_DEVICE_TEST_PASS");
}
