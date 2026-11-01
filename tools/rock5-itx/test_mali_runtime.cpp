/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <inttypes.h>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include "CsfQueueEngine.h"
#define main MaliMemoryFixtureMain
#include "test_mali_memory.cpp"
#undef main

#include "test_mali_sync_os.h"
#include "CsfClient.h"
#include "CsfDevice.h"
#include "CsfRuntime.h"

struct Area { std::unique_ptr<Guarded> memory; uint64 physical; std::string name; };
static std::mutex sAreaLock;
static std::map<area_id, Area> sAreas;
static area_id sNextArea = 1;
static uint64 sNextPhysical = UINT64_C(0x280000000);
static std::atomic<unsigned> sAllocationCalls{0}, sFailAllocation{0};
struct virtual_address_restrictions { uint64 unused; };
struct physical_address_restrictions { uint64 low_address, high_address; };
struct physical_entry { uint64 address; size_t size; };
static area_id create_area_etc(int team, const char* name, size_t bytes, unsigned lock,
	uint32 protection, int flags, int spec, virtual_address_restrictions* virt,
	physical_address_restrictions* phys, void** address)
{
	assert(team == B_SYSTEM_TEAM && lock == B_CONTIGUOUS && bytes > 0 && !(bytes & 4095));
	assert(protection == (B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA) && flags == 0 && spec == 0);
	assert(virt->unused == 0 && phys->low_address == 0 && phys->high_address == (UINT64_C(1) << 40));
	if (++sAllocationCalls == sFailAllocation) return B_NO_MEMORY;
	std::lock_guard<std::mutex> guard(sAreaLock);
	auto memory = std::make_unique<Guarded>(bytes);
	*address = memory->data;
	area_id id = sNextArea++;
	sAreas.emplace(id, Area{std::move(memory), sNextPhysical, name});
	sNextPhysical += bytes; return id;
}
static status_t get_memory_map(void* address, size_t bytes, physical_entry* entry, unsigned count)
{
	assert(count == 1);
	std::lock_guard<std::mutex> guard(sAreaLock);
	for (auto& pair : sAreas) if (pair.second.memory->data == address) {
		assert(pair.second.memory->size == bytes);
		entry->address = pair.second.physical; entry->size = bytes; return B_OK;
	}
	assert(false); return B_BAD_ADDRESS;
}
static status_t MakeFirmwareRamNoncacheable(area_id id, void* address, phys_addr_t physical, size_t bytes)
{
	std::lock_guard<std::mutex> guard(sAreaLock);
	Area& area = sAreas.at(id);
	assert(area.memory->data == address && area.memory->size == bytes && area.physical == physical);
	return B_OK;
}
static status_t delete_area(area_id id)
{
	std::lock_guard<std::mutex> guard(sAreaLock);
	assert(sAreas.erase(id) == 1); return B_OK;
}
bool FirmwareMemoryRetained()
{
	std::lock_guard<std::mutex> guard(sAreaLock);
	for (auto& pair : sAreas) if (pair.second.name == "Mali CSF firmware DMA") return true;
	return false;
}
#include "test_mali_sync_kernel.inc"
#include "test_mali_sync_calls.inc"

struct Thread {
	int32 (*entry)(void*);
	void* data;
	std::thread thread;
	status_t result = B_OK;
};
static std::mutex sThreadLock;
static std::map<thread_id, std::shared_ptr<Thread>> sThreads;
static thread_id sNextThread = 1;
static std::atomic<bool> sFailSpawn{false}, sFailResume{false};
static thread_id spawn_kernel_thread(int32 (*entry)(void*), const char*, int, void* data)
{
	if (sFailSpawn) return B_NO_MEMORY;
	std::lock_guard<std::mutex> guard(sThreadLock);
	thread_id id = sNextThread++;
	auto thread = std::make_shared<Thread>(); thread->entry = entry; thread->data = data;
	sThreads.emplace(id, thread); return id;
}
static status_t resume_thread(thread_id id)
{
	if (sFailResume) return B_NO_MEMORY;
	std::lock_guard<std::mutex> guard(sThreadLock);
	auto thread = sThreads.at(id);
	thread->thread = std::thread([thread]() { thread->result = thread->entry(thread->data); });
	return B_OK;
}
static status_t kill_thread(thread_id id)
{
	std::lock_guard<std::mutex> guard(sThreadLock);
	assert(!sThreads.at(id)->thread.joinable()); return B_OK;
}
static status_t wait_for_thread(thread_id id, status_t* result)
{
	std::shared_ptr<Thread> thread;
	{
		std::lock_guard<std::mutex> guard(sThreadLock);
		thread = sThreads.at(id); sThreads.erase(id);
	}
	if (thread->thread.joinable()) thread->thread.join();
	*result = thread->result; return B_OK;
}

// A tiny independent client/VM oracle. User ownership can disappear while the
// runtime's leases must keep this root alive. Full mapping/BO semantics have
// their own production CsfClient fixture.
struct Generation {
	Guarded root{4096};
	unsigned references = 1;
	uint64 number;
	explicit Generation(uint64 value) : number(value) { memset(root.data, 0, root.size); }
};
struct Client { Generation* generation; bool closed = false; };
static std::mutex sLeaseLock;
static std::set<Generation*> sGenerations;
static Generation* NewGeneration(uint64 number)
{
	std::lock_guard<std::mutex> guard(sLeaseLock);
	Generation* value = new Generation(number); sGenerations.insert(value); return value;
}
static void ReleaseGeneration(Generation* value)
{
	if (value != NULL && --value->references == 0) {
		assert(sGenerations.erase(value) == 1); delete value;
	}
}
static void CloseVm(Client& client)
{
	std::lock_guard<std::mutex> guard(sLeaseLock);
	ReleaseGeneration(client.generation); client.generation = NULL;
}
static void AdvanceVm(Client& client)
{
	Generation* next = NewGeneration(client.generation->number + 1);
	std::lock_guard<std::mutex> guard(sLeaseLock);
	ReleaseGeneration(client.generation); client.generation = next;
}
status_t MaliCSF::AcquireClientVm(void* cookie, uint32 handle, uint64 generation, ClientVmLease& lease)
{
	std::lock_guard<std::mutex> guard(sLeaseLock);
	Client& client = *(Client*)cookie;
	if (client.closed) return B_NOT_ALLOWED;
	if (handle != 7 || client.generation == NULL) return B_ENTRY_NOT_FOUND;
	if (generation != 0 && generation != client.generation->number) return B_BUSY;
	client.generation->references++;
	lease.state = client.generation; lease.generation = client.generation->number;
	return B_OK;
}
void MaliCSF::ReleaseClientVm(ClientVmLease& lease)
{
	std::lock_guard<std::mutex> guard(sLeaseLock);
	ReleaseGeneration((Generation*)lease.state); lease = {};
}
const uint64_t* MaliCSF::ClientVmRoot(const ClientVmLease& lease)
{
	assert(lease.state != NULL); return (uint64_t*)((Generation*)lease.state)->root.data;
}
bool MaliCSF::ClientVmRange(const ClientVmLease& lease, uint64 address, uint64 bytes)
{
	assert(lease.state != NULL);
	return address >= 0x100000000 && address < 0x100001000 && bytes <= 0x100001000 - address;
}
static std::atomic<bool> sFailHardware{false};
struct FirmwareHardware {
	explicit FirmwareHardware(bool commands) { assert(commands); }
	// FullClockOperation raises the GPU divider only from SPLL/4: not here.
	uint32 ReadClock(uint32) { return 0; }
	void WriteClock(uint32, uint32) { assert(false); }
	status_t Init(const ResourceInfo&) { return sFailHardware ? B_IO_ERROR : B_OK; }
};
class Runtime;
static void DriveRuntime(Runtime*, FirmwareRunInfo&, FirmwareRecovery&);
#include "runtime.inc"

static std::atomic<bool> sHoldJob{false}, sFailJob{false}, sFailBoot{false};
static std::atomic<bool> sRecoverJob{false}, sHoldRecovery{false};
static std::atomic<unsigned> sEnteredRecovery{0};
static std::atomic<unsigned> sEnteredJob{0}, sHardwareCycles{0};
static GpuProperties FixtureProperties(unsigned cycle)
{
	GpuProperties result{};
	// Deliberately distinct runtime epochs, so stale or zero-filled snapshots
	// cannot pass. The separate register oracle tests actual capture.
	result.gpuId = 0xa8670005; result.csfId = 0x040a0412;
	result.gpuRevision = cycle; result.shaderPresent = UINT64_C(0x102345678);
	result.textureFeatures[3] = 0x983cb62f;
	result.workRegisters = 96; result.reservedRegisters = 4;
	result.firmwareVersion = 0x01050000; result.firmwareTimerHz = 24000000;
	result.userVaLimit = UINT64_C(1) << 47;
	return result;
}
static void DriveRuntime(Runtime* runtime, FirmwareRunInfo& info, FirmwareRecovery& recovery)
{
	sHardwareCycles++;
	if (sFailBoot) { info.flags |= kFirmwareNeedsRecovery; return; }
	runtime->Ready({}, FixtureProperties(sHardwareCycles));
	QueueProgress progress[kMaxRuntimeQueues]{};
	for (;;) {
		QueueWork work{}; runtime->Next(work);
		if (work.type == kQueueWorkStop) break;
		if (work.type == kQueueWorkIdle) continue;
		if (work.type == kQueueWorkDestroy) { runtime->Destroyed(work.slot); continue; }
		assert(work.type == kQueueWorkSubmit && work.root != NULL);
		sEnteredJob++;
		while (sHoldJob) std::this_thread::sleep_for(std::chrono::milliseconds(1));
		if (sFailJob) { info.flags |= kFirmwareNeedsRecovery; return; }
		assert(work.memory->SetUserRoot(work.root));
		if (work.generation != progress[work.slot].generation) runtime->Activated(work);
		if (sRecoverJob) {
			recovery.StopAdmission();
			sEnteredRecovery++;
			while (sHoldRecovery) std::this_thread::sleep_for(std::chrono::milliseconds(1));
			// Separate reset/AS/power fixtures prove this hardware outcome. The
			// production runtime must still fail work and retain all leases until
			// this model explicitly reports quiescence and full restoration.
			info.result = kFirmwareOperationFailed;
			info.cleanupResult = kFirmwareOperationCleanupFailed;
			info.flags |= kFirmwareRecovered | kFirmwarePowerRestored;
			recovery.attempted = recovery.quiescent = recovery.recovered = true;
			return;
		}
		auto& value = progress[work.slot];
		value.generation = work.generation; value.completed = work.sequence;
		value.insert += 128; value.extract = value.insert; value.interrupts++; value.syncEvents++;
		runtime->Complete(work, value);
	}
	info.result = info.cleanupResult = kFirmwareRunOK;
	info.flags |= kFirmwarePowerRestored | kFirmwareCleaned;
}

template<typename T> static status_t Call(Client& client, uint32 op, T& request, bool& recovery)
{
	return ControlQueues({}, &client, op, &request, sizeof(request), recovery);
}
static status_t Create(Client& client, uint32& handle, bool firmware, bool& recovery)
{
	auto file = container({{0x800000, 4096, 13}, {0x4000000, 65536, 0xc000001b}});
	std::vector<uint8_t> request(sizeof(QueueCreate) + (firmware ? file.size() : 0), 0);
	QueueCreate* create = (QueueCreate*)request.data();
	create->version = 1; create->vm = 7; create->firmwareBytes = firmware ? file.size() : 0;
	if (firmware) memcpy(request.data() + sizeof(*create), file.data(), file.size());
	status_t status = ControlQueues({}, &client, kCreateQueue, request.data(), request.size(), recovery);
	if (status == B_OK) {
		handle = create->handle;
		assert(handle != 0 && create->generation == client.generation->number);
	}
	return status;
}
static QueueInfo Info(Client& client, uint32 handle)
{
	bool recovery = false; QueueInfo info{}; info.version = 1; info.handle = handle;
	assert(Call(client, kGetQueueInfo, info, recovery) == B_OK && !recovery); return info;
}
static QueueSubmit Submit(Client& client, uint32 handle)
{
	QueueSubmit request{}; request.version = 1; request.handle = handle;
	request.streamAddress = 0x100000000; request.streamBytes = 48;
	bool recovery = false;
	assert(Call(client, kSubmitQueue, request, recovery) == B_OK && !recovery); return request;
}
static QueueWait Wait(Client& client, uint32 handle, uint64 sequence)
{
	QueueWait request{}; request.version = 1; request.handle = handle;
	request.sequence = sequence; request.timeoutMicros = 1000000;
	bool recovery = false;
	assert(Call(client, kWaitQueue, request, recovery) == B_OK && !recovery); return request;
}
static void Destroy(Client& client, uint32 handle)
{
	QueueHandle request{1, handle, 0}; bool recovery = false;
	assert(Call(client, kDestroyQueue, request, recovery) == B_OK && !recovery);
}
static void AwaitJob(unsigned before)
{
	bigtime_t deadline = system_time() + 1000000;
	while (sEnteredJob == before && system_time() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	assert(sEnteredJob > before);
}
static void Clean(unsigned generations)
{
	assert(sRuntime == NULL && sThreads.empty() && sAreas.empty());
	assert(sGenerations.size() == generations);
}
static void DropSimulatedRetainedRuntime()
{
	// Fixture teardown only: the simulated GPU owns no RAM. Production has no
	// such recovery shortcut; uncertain hardware allocations survive to reboot.
	Runtime* runtime = sRuntime;
	assert(runtime != NULL && runtime->retained && runtime->finished && runtime->thread == -1);
	assert(runtime->references == 1); sRuntime = NULL; PutRuntime(runtime);
}

static void CheckProperties(Client& client, uint32 handle)
{
	bool recovery = false;
	QueueProperties request{}; request.version = 1; request.handle = handle;
	memset(&request.gpu, 0x95, sizeof(request.gpu)); // output must be replaced
	assert(Call(client, kGetQueueProperties, request, recovery) == B_OK && !recovery);
	GpuProperties expected = FixtureProperties(sHardwareCycles);
	assert(request.version == 1 && request.handle == handle && request.flags == 0
		&& request.reserved == 0 && request.reserved2[0] == 0 && request.reserved2[1] == 0);
	assert(memcmp(&request.gpu, &expected, sizeof(expected)) == 0);
}

static void PropertyBoundaries(Client& client, Client& other, uint32 handle)
{
	unsigned allocations = sAllocationCalls, cycles = sHardwareCycles;
	auto before = Info(client, handle);
	bool recovery = false;
	QueueProperties clean{}; clean.version = 1; clean.handle = handle;
	for (unsigned field = 0; field < 5; field++) {
		auto bad = clean;
		if (field == 0) bad.version = 2;
		if (field == 1) bad.flags = 1;
		if (field == 2) bad.reserved = 1;
		if (field == 3) bad.reserved2[0] = 1;
		if (field == 4) bad.reserved2[1] = UINT64_MAX;
		assert(Call(client, kGetQueueProperties, bad, recovery) == B_BAD_VALUE);
	}
	for (size_t size : {size_t(0), sizeof(clean) - 1, sizeof(clean) + 1})
		assert(ControlQueues({}, &client, kGetQueueProperties, &clean, size, recovery) == B_BAD_VALUE);
	assert(ControlQueues({}, &client, kGetQueueProperties, NULL, sizeof(clean), recovery) == B_BAD_ADDRESS);
	assert(Call(other, kGetQueueProperties, clean, recovery) == B_ENTRY_NOT_FOUND);
	for (unsigned fail = 1; fail <= 2; fail++) {
		sFailCopy = sCopies + fail;
		assert(Call(client, kGetQueueProperties, clean, recovery) == B_BAD_ADDRESS);
		sFailCopy = 0;
	}
	CheckProperties(client, handle);
	auto after = Info(client, handle);
	assert(memcmp(&before, &after, sizeof(before)) == 0);
	assert(sAllocationCalls == allocations && sHardwareCycles == cycles && !recovery);
}

static void QueryRetired(Client& client, uint32 handle, bool& recovery)
{
	QueueProperties request{}; request.version = 1; request.handle = handle;
	assert(Call(client, kGetQueueProperties, request, recovery) == B_ENTRY_NOT_FOUND);
	QueueResetInfo reset{}; reset.version = 1; reset.handle = handle;
	assert(Call(client, kGetQueueResetInfo, reset, recovery) == B_ENTRY_NOT_FOUND);
}

static void
CheckResetInfo(Client& client, uint32 handle, uint32 state, status_t error)
{
	bool recovery = false;
	QueueResetInfo info{}; info.version = 1; info.handle = handle;
	assert(Call(client, kGetQueueResetInfo, info, recovery) == B_OK && !recovery);
	assert(info.version == 1 && info.handle == handle && info.state == state
		&& info.error == error && info.reserved[0] == 0 && info.reserved[1] == 0);
}

static void
ResetInfoBoundaries(Client& client, Client& other, uint32 handle)
{
	unsigned allocations = sAllocationCalls, cycles = sHardwareCycles;
	auto before = Info(client, handle);
	bool recovery = false;
	QueueResetInfo clean{}; clean.version = 1; clean.handle = handle;
	for (unsigned field = 0; field < 5; field++) {
		auto bad = clean;
		if (field == 0) bad.version = 2;
		if (field == 1) bad.state = 1;
		if (field == 2) bad.error = B_IO_ERROR;
		if (field == 3) bad.reserved[0] = 1;
		if (field == 4) bad.reserved[1] = UINT64_MAX;
		assert(Call(client, kGetQueueResetInfo, bad, recovery) == B_BAD_VALUE);
	}
	for (size_t size : {size_t(0), sizeof(clean) - 1, sizeof(clean) + 1})
		assert(ControlQueues({}, &client, kGetQueueResetInfo, &clean, size, recovery) == B_BAD_VALUE);
	assert(ControlQueues({}, &client, kGetQueueResetInfo, NULL, sizeof(clean), recovery) == B_BAD_ADDRESS);
	assert(Call(other, kGetQueueResetInfo, clean, recovery) == B_ENTRY_NOT_FOUND);
	for (unsigned fail = 1; fail <= 2; fail++) {
		sFailCopy = sCopies + fail;
		assert(Call(client, kGetQueueResetInfo, clean, recovery) == B_BAD_ADDRESS);
		sFailCopy = 0;
	}
	CheckResetInfo(client, handle, kQueueResetNone, B_OK);
	auto after = Info(client, handle);
	assert(memcmp(&before, &after, sizeof(before)) == 0);
	assert(sAllocationCalls == allocations && sHardwareCycles == cycles && !recovery);
}

static status_t
SubmitSynchronized(Client& client, void* sync, uint32 queue,
	std::initializer_list<SyncPoint> waits, std::initializer_list<SyncPoint> signals,
	uint64& sequence)
{
	std::vector<uint8> bytes(sizeof(QueueSubmitSync) + (waits.size() + signals.size()) * sizeof(SyncPoint));
	auto* request = (QueueSubmitSync*)bytes.data();
	request->queue = {1, queue, 0, 0x100000000, 48, 0, 0, 0};
	request->waitCount = waits.size(); request->signalCount = signals.size();
	auto* points = (SyncPoint*)(request + 1);
	for (const auto& point : waits) *points++ = point;
	for (const auto& point : signals) *points++ = point;
	bool recovery = false;
	status_t status = ControlQueues({}, &client, kSubmitQueueSync, request, bytes.size(), recovery, sync);
	assert(!recovery); sequence = request->queue.sequence; return status;
}

static void
SynchronizedRuntime()
{
	Client client{NewGeneration(1)};
	void* sync = SyncOpen(); bool recovery = false;
	uint32 a = 0, b = 0;
	assert(Create(client, a, true, recovery) == B_OK && Create(client, b, false, recovery) == B_OK);
	uint32 gate = SyncMake(sync), output = SyncMake(sync), independent = SyncMake(sync);
	SyncSubmission gateJob = SyncEnqueue(sync, {}, {{gate, 0, 0}});
	uint64 sequence;
	unsigned before = sEnteredJob;
	assert(SubmitSynchronized(client, sync, a, {{gate, 0, 0}}, {{output, 0, 10}}, sequence) == B_OK
		&& sequence == 1);
	assert(SubmitSynchronized(client, sync, a, {{output, 0, 10}}, {{output, 0, 20}}, sequence) == B_OK
		&& sequence == 2);
	int snapshot = SyncExport(sync, output, true, 10);
	assert(SubmitSynchronized(client, sync, b, {}, {{independent, 0, 1}}, sequence) == B_OK);
	assert(Wait(client, b, sequence).result == B_OK);
	assert(sEnteredJob == before + 1 && Info(client, a).pending == 2 && Info(client, a).completed == 0);
	assert(SyncFdWait(snapshot) == B_TIMED_OUT);
	// Replacing the handle cannot redirect already queued dependencies.
	assert(SyncUpdate(sync, kResetSync, {{output, 0, 0}}) == B_OK);
	assert(SyncUpdate(sync, kSignalSync, {{output, 0, 0}}) == B_OK);
	FinishSyncSubmission(gateJob, B_OK);
	assert(Wait(client, a, 2).result == B_OK && SyncFdWait(snapshot) == B_OK);
	SyncCloseFd(snapshot);
	uint32 rollback = SyncMake(sync);
	sFailCopy = sCopies + 3;
	assert(SubmitSynchronized(client, sync, a, {}, {{rollback, 0, 1}}, sequence) == B_BAD_ADDRESS);
	sFailCopy = 0;
	assert(Info(client, a).submitted == 2 && SyncQuery(sync, rollback).flags == 0);
	assert(SubmitSynchronized(client, sync, a, {{rollback, 0, 0}}, {{output, 0, 1}}, sequence) == B_BAD_VALUE);
	assert(Info(client, a).submitted == 2);
	Destroy(client, a); Destroy(client, b); Clean(1);
	// Cancellation of a producer propagates through dependent queues without
	// executing their commands or stopping an unrelated queue.
	assert(Create(client, a, true, recovery) == B_OK && Create(client, b, false, recovery) == B_OK);
	uint32 c = 0; assert(Create(client, c, false, recovery) == B_OK);
	gateJob = SyncEnqueue(sync, {}, {{gate, 0, 0}});
	before = sEnteredJob;
	assert(SubmitSynchronized(client, sync, a, {{gate, 0, 0}}, {{output, 0, 0}}, sequence) == B_OK);
	assert(SubmitSynchronized(client, sync, b, {{output, 0, 0}}, {{independent, 0, 2}}, sequence) == B_OK);
	snapshot = SyncExport(sync, independent, true);
	Destroy(client, a);
	assert(Wait(client, b, sequence).result == B_CANCELED);
	assert(Info(client, b).state == kQueueFailed && Info(client, b).failedSequence == 1);
	assert(Info(client, b).pending == 0 && sEnteredJob == before);
	CheckResetInfo(client, b, kQueueResetLocalError, B_CANCELED);
	CheckResetInfo(client, c, kQueueResetNone, B_OK);
	status_t result; assert(SyncFdWait(snapshot, &result) == B_OK && result == B_CANCELED);
	QueueSubmit rejected{1, b, 0, 0, 0, 0, 0, 0};
	assert(Call(client, kSubmitQueue, rejected, recovery) == B_CANCELED);
	auto good = Submit(client, c); assert(Wait(client, c, good.sequence).result == B_OK);
	FinishSyncSubmission(gateJob, B_OK);
	Destroy(client, b); Destroy(client, c); SyncCloseFd(snapshot); Clean(1);
	// Hardware failure signals queued output fences even when DMA leases must
	// be retained until reboot. Snapshot FDs can still report that error.
	assert(Create(client, a, true, recovery) == B_OK);
	sHoldJob = true; before = sEnteredJob;
	assert(SubmitSynchronized(client, sync, a, {}, {{output, 0, 0}}, sequence) == B_OK); AwaitJob(before);
	assert(SubmitSynchronized(client, sync, a, {}, {{independent, 0, 3}}, sequence) == B_OK);
	snapshot = SyncExport(sync, independent, true); sFailJob = true; sHoldJob = false;
	assert(Wait(client, a, sequence).result == B_IO_ERROR);
	assert(SyncFdWait(snapshot, &result) == B_OK && result == B_IO_ERROR);
	CheckResetInfo(client, a, kQueueResetUnrecoverable, B_IO_ERROR);
	CloseQueues(&client, recovery); assert(recovery);
	FreeSyncClient(sync); SyncCloseFd(snapshot); SyncBalanced();
	DropSimulatedRetainedRuntime(); sFailJob = false;
	CloseVm(client); Clean(0);
}

static void
RecoveredRuntime()
{
	Client first{NewGeneration(1)}, second{NewGeneration(1)}, fresh{NewGeneration(1)};
	void* sync = SyncOpen(); bool recovery = false;
	uint32 a = 0, b = 0, c = 0;
	assert(Create(first, a, true, recovery) == B_OK);
	assert(Create(second, b, false, recovery) == B_OK);
	for (auto entry : {std::make_pair(&first, a), std::make_pair(&second, b)}) {
		auto job = Submit(*entry.first, entry.second);
		assert(Wait(*entry.first, entry.second, job.sequence).result == B_OK);
	}
	uint32 current = SyncMake(sync), queued = SyncMake(sync), affected = SyncMake(sync);
	uint64 sequence;
	sHoldJob = sHoldRecovery = sRecoverJob = true;
	unsigned before = sEnteredJob, recoveries = sEnteredRecovery;
	assert(SubmitSynchronized(first, sync, a, {}, {{current, 0, 1}}, sequence) == B_OK);
	AwaitJob(before);
	assert(SubmitSynchronized(first, sync, a, {}, {{queued, 0, 1}}, sequence) == B_OK);
	assert(SubmitSynchronized(second, sync, b, {}, {{affected, 0, 1}}, sequence) == B_OK);
	int snapshots[] = {SyncExport(sync, current, true), SyncExport(sync, queued, true),
		SyncExport(sync, affected, true)};
	sHoldJob = false;
	bigtime_t deadline = system_time() + 1000000;
	while (sEnteredRecovery == recoveries && system_time() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	assert(sEnteredRecovery == recoveries + 1);
	assert(Info(first, a).state == kQueueFailed && Info(second, b).state == kQueueFailed);
	unsigned queryAllocations = sAllocationCalls, queryCycles = sHardwareCycles;
	for (unsigned i = 0; i < 64; i++) {
		CheckResetInfo(first, a, kQueueResetPending, B_IO_ERROR);
		CheckResetInfo(second, b, kQueueResetPending, B_IO_ERROR);
	}
	assert(sAllocationCalls == queryAllocations && sHardwareCycles == queryCycles);
	QueueSubmit rejected{1, b, 0, 0, 0, 0, 0, 0};
	assert(Call(second, kSubmitQueue, rejected, recovery) == B_IO_ERROR);
	assert(Create(fresh, c, false, recovery) == B_BUSY && !recovery);
	for (int fd : snapshots) assert(SyncFdWait(fd) == B_TIMED_OUT);
	unsigned areas = sAreas.size();
	CloseVm(first);
	std::atomic<bool> closed{false};
	std::thread closer([&]() { CloseQueues(&first, recovery); closed = true; });
	std::this_thread::sleep_for(std::chrono::milliseconds(5));
	assert(!closed && sAreas.size() == areas && sGenerations.size() == 3);
	sHoldRecovery = false;
	closer.join();
	assert(closed && !recovery && sRuntime != NULL && sRuntime->live == 1);
	assert(sRuntime->finished && !sRuntime->retained && sRuntime->thread == -1);
	assert(sGenerations.size() == 2 && sAreas.size() == areas - 1);
	assert(Wait(second, b, sequence).result == B_IO_ERROR);
	assert(Info(second, b).failedSequence == 2 && Info(second, b).completed == 1);
	CheckResetInfo(second, b, kQueueResetQuiescent, B_IO_ERROR);
	for (int fd : snapshots) {
		status_t status;
		assert(SyncFdWait(fd, &status) == B_OK && status == B_IO_ERROR);
		SyncCloseFd(fd);
	}
	assert(Create(fresh, c, false, recovery) == B_BUSY);
	CloseVm(second); CloseQueues(&second, recovery); Clean(1);
	sRecoverJob = false;
	assert(Create(fresh, c, true, recovery) == B_OK && c > a && c > b && !recovery);
	CheckResetInfo(fresh, c, kQueueResetNone, B_OK);
	QueryRetired(fresh, a, recovery); QueryRetired(fresh, b, recovery);
	auto job = Submit(fresh, c);
	assert(Wait(fresh, c, job.sequence).result == B_OK);
	Destroy(fresh, c); CloseVm(fresh); Clean(0);
	FreeSyncClient(sync); SyncBalanced();
}

int main()
{
	setvbuf(stdout, NULL, _IONBF, 0);
	Client client{NewGeneration(1)}, other{NewGeneration(1)};
	bool recovery = false; uint32 handle = 0;
	assert(Create(client, handle, false, recovery) == B_ENTRY_NOT_FOUND); Clean(2);
	for (unsigned fail = 1; fail <= 2; fail++) {
		sAllocationCalls = 0; sFailAllocation = fail;
		assert(Create(client, handle, true, recovery) == B_NO_MEMORY && !recovery);
		Clean(2); assert(client.generation->references == 1);
	}
	sFailAllocation = 0;
	sFailSpawn = true;
	assert(Create(client, handle, true, recovery) == B_NO_MEMORY); Clean(2); sFailSpawn = false;
	sFailResume = true;
	assert(Create(client, handle, true, recovery) == B_NO_MEMORY); Clean(2); sFailResume = false;
	sFailHardware = true;
	assert(Create(client, handle, true, recovery) == B_IO_ERROR && !recovery); Clean(2); sFailHardware = false;
	for (unsigned fail = 1; fail <= 3; fail++) {
		sCopies = 0; sFailCopy = fail;
		assert(Create(client, handle, true, recovery) == B_BAD_ADDRESS && !recovery);
		Clean(2); assert(client.generation->references == 1);
	}
	sFailCopy = 0;
	assert(Create(client, handle, true, recovery) == B_OK);
	unsigned cycles = sHardwareCycles;
	uint32 handles[kMaxRuntimeQueues]; handles[0] = handle;
	for (unsigned i = 1; i < kMaxRuntimeQueues; i++) assert(Create(client, handles[i], false, recovery) == B_OK);
	uint32 excess = 0;
	assert(Create(client, excess, false, recovery) == B_NO_MEMORY && excess == 0);
	assert(Create(client, excess, true, recovery) == B_BAD_VALUE && sHardwareCycles == cycles);
	QueueInfo invalid{}; invalid.version = 1; invalid.handle = handle;
	assert(Call(other, kGetQueueInfo, invalid, recovery) == B_ENTRY_NOT_FOUND);
	assert(Info(client, handle).state == kQueueReady);
	PropertyBoundaries(client, other, handle);
	ResetInfoBoundaries(client, other, handle);
	QueueSubmit bad{}; bad.version = 1; bad.handle = handle;
	bad.streamAddress = 0x100000000; bad.streamBytes = 8;
	sCopies = 0; sFailCopy = 2;
	assert(Call(client, kSubmitQueue, bad, recovery) == B_BAD_ADDRESS);
	sFailCopy = 0;
	assert(Info(client, handle).submitted == 0 && client.generation->references == kMaxRuntimeQueues + 1);
	bad.streamAddress += 4096;
	assert(Call(client, kSubmitQueue, bad, recovery) == B_BAD_VALUE);
	for (unsigned i = 1; i < kMaxRuntimeQueues; i++) Destroy(client, handles[i]);
	assert(client.generation->references == 2);

	sHoldJob = true; unsigned before = sEnteredJob;
	auto job = Submit(client, handle); AwaitJob(before);
	assert(job.sequence == 1 && job.generation == 1 && client.generation->references == 3);
	// Four concurrent readers while the worker is held inside an actual queued
	// job. No extra allocation, firmware cycle, submission or completion.
	unsigned queryAllocations = sAllocationCalls;
	std::thread readers[4];
	for (auto& reader : readers) reader = std::thread([&]() {
		for (unsigned i = 0; i < 64; i++) {
			CheckProperties(client, handle);
			CheckResetInfo(client, handle, kQueueResetNone, B_OK);
		}
	});
	for (auto& reader : readers) reader.join();
	assert(sAllocationCalls == queryAllocations && sHardwareCycles == cycles);
	assert(Info(client, handle).submitted == 1 && Info(client, handle).completed == 0);
	QueueWait wait{}; wait.version = 1; wait.handle = handle; wait.sequence = 1;
	assert(Call(client, kWaitQueue, wait, recovery) == B_TIMED_OUT);
	wait.timeoutMicros = 1000;
	assert(Call(client, kWaitQueue, wait, recovery) == B_TIMED_OUT);
	wait.timeoutMicros = -1; sInterruptWait = true;
	assert(Call(client, kWaitQueue, wait, recovery) == B_INTERRUPTED);
	Generation* old = client.generation; AdvanceVm(client);
	assert(old->references == 2 && sGenerations.size() == 3);
	auto newJob = Submit(client, handle);
	assert(newJob.generation == 2 && newJob.sequence == 2);
	bad.streamAddress = 0x100000000; bad.generation = 1;
	assert(Call(client, kSubmitQueue, bad, recovery) == B_BUSY);
	sHoldJob = false;
	assert(Wait(client, handle, 2).result == B_OK);
	assert(sGenerations.size() == 2 && client.generation->references == 2);
	assert(Info(client, handle).activeGeneration == 2);
	CheckProperties(client, handle);

	// Queued jobs retain the original snapshot, and timed-out waits leave it
	// untouched. Closing the VM prevents new submissions, but not old work.
	sHoldJob = true; before = sEnteredJob; Submit(client, handle); AwaitJob(before);
	for (unsigned i = 1; i < 64; i++) Submit(client, handle);
	bad.generation = 0;
	assert(Call(client, kSubmitQueue, bad, recovery) == B_BUSY);
	assert(Info(client, handle).pending == 64);
	CloseVm(client);
	assert(Call(client, kSubmitQueue, bad, recovery) == B_ENTRY_NOT_FOUND);
	std::atomic<bool> closed{false};
	std::thread closer([&]() { CloseQueues(&client, recovery); closed = true; });
	std::this_thread::sleep_for(std::chrono::milliseconds(5));
	assert(!closed);
	sHoldJob = false; closer.join();
	assert(closed && !recovery); Clean(1);
	QueryRetired(client, handle, recovery);

	// Two API users may wait and close concurrently; no worker outlives the
	// final close, and a stale handle never becomes a subsequently reused slot.
	client.generation = NewGeneration(1);
	assert(Create(client, handle, true, recovery) == B_OK);
	uint32 stale = handle;
	sHoldJob = true; before = sEnteredJob; job = Submit(client, handle); AwaitJob(before);
	status_t waitStatus = B_IO_ERROR;
	std::thread waiter([&]() {
		QueueWait pending{}; pending.version = 1; pending.handle = handle;
		pending.sequence = job.sequence; pending.timeoutMicros = -1; bool ignored = false;
		waitStatus = Call(client, kWaitQueue, pending, ignored);
		if (waitStatus == B_OK) assert(pending.result == B_OK || pending.result == B_CANCELED);
	});
	std::thread closeAgain([&]() { CloseQueues(&client, recovery); });
	std::this_thread::sleep_for(std::chrono::milliseconds(5));
	sHoldJob = false; waiter.join(); closeAgain.join();
	assert(waitStatus == B_OK || waitStatus == B_ENTRY_NOT_FOUND); Clean(2);
	assert(Create(client, handle, true, recovery) == B_OK && handle != stale);
	QueryRetired(client, stale, recovery);
	CheckProperties(client, handle);
	invalid.handle = stale;
	assert(Call(client, kGetQueueInfo, invalid, recovery) == B_ENTRY_NOT_FOUND);
	Destroy(client, handle); Clean(2);

	// A failed running job leaves both its queued lease and the previously
	// active root alive after user closure, until the fixture simulates reboot.
	assert(Create(client, handle, true, recovery) == B_OK);
	sFailJob = true; job = Submit(client, handle);
	assert(Wait(client, handle, job.sequence).result == B_IO_ERROR);
	assert(Info(client, handle).state == kQueueFailed);
	CheckProperties(client, handle); // metadata is not a health indicator
	CloseVm(client); CloseQueues(&client, recovery);
	QueryRetired(client, handle, recovery);
	assert(recovery && !sAreas.empty() && sThreads.empty() && sGenerations.size() == 2);
	assert(Create(other, excess, true, recovery) == B_BAD_VALUE);
	DropSimulatedRetainedRuntime(); sFailJob = false; recovery = false; Clean(1);

	sFailBoot = true;
	assert(Create(other, handle, true, recovery) == B_IO_ERROR && recovery);
	assert(!sAreas.empty() && sThreads.empty() && other.generation->references == 1);
	DropSimulatedRetainedRuntime(); sFailBoot = false; recovery = false; Clean(1);
	CloseVm(other); Clean(0);
	SynchronizedRuntime();
	RecoveredRuntime();
	puts("MALI_CSF_RUNTIME_TEST_PASS");
}
