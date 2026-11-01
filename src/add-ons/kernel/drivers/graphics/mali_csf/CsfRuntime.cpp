/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#include <KernelExport.h>
#include <AutoDeleterOS.h>
#include <condition_variable.h>
#include <lock.h>
#include <smp.h>
#include <thread.h>
#include <util/AutoLock.h>
#include <vm/vm.h>
#include <new>
#include <stdlib.h>
#if defined(__aarch64__)
#include <arch/arm64/cache_line_size.h>
#endif

#include "CsfClient.h"
#include "CsfDevice.h"
#include "CsfQueueEngine.h"
#include "CsfRuntime.h"
#include "CsfSynchronization.h"

using namespace MaliCSF;

#include "CsfDmaMemory.h"
#include "CsfFirmwareHardware.h"

static mutex sRuntimeLock = MUTEX_INITIALIZER("Mali CSF runtime");
static uint32 sNextQueueHandle = 1;

struct QueueJob {
	QueueJob* next;
	ClientVmLease lease;
	uint64 address;
	uint32 bytes;
	uint64 sequence;
	SyncSubmission synchronization;
};

struct QueueSlot {
	bool used = false;
	bool destroy = false;
	uint32 handle = 0;
	uint32 vm = 0;
	void* client = NULL;
	QueueMemory memory;
	area_id area = -1;
	ClientVmLease activeLease = {};
	QueueJob* first = NULL;
	QueueJob* last = NULL;
	QueueJob* current = NULL;
	QueueProgress progress = {};
	uint64 submitted = 0;
	uint64 completed = 0;
	uint32 pending = 0;
	status_t error = B_OK;
	uint64 failedSequence = 0;
};

static void
DeleteJob(QueueJob* job)
{
	if (job == NULL) return;
	FinishSyncSubmission(job->synchronization, B_CANCELED);
	ReleaseClientVm(job->lease);
	free(job);
}

class Runtime;
static Runtime* sRuntime;
static void PutRuntime(Runtime* runtime);

class Runtime {
public:
	explicit Runtime(const ResourceInfo& value) : resources(value), hardware(true)
	{
		changed.Init(this, "Mali CSF runtime");
	}
	~Runtime()
	{
		// Hardware cycles that could not recover retain the global owner.
		for (unsigned i = 0; i < kMaxRuntimeQueues; i++) {
			QueueSlot& queue = queues[i];
			while (queue.first != NULL) {
				QueueJob* job = queue.first; queue.first = job->next; DeleteJob(job);
			}
			DeleteJob(queue.current);
			ReleaseClientVm(queue.activeLease);
			if (queue.area >= B_OK) delete_area(queue.area);
		}
		if (area >= B_OK) delete_area(area);
		free(file);
	}
	void Ready(const InterfaceInfo& value, const GpuProperties& gpu)
	{
		MutexLocker locker(sRuntimeLock);
		interface = value;
		properties = gpu;
		ready = true;
		changed.NotifyAll();
	}
	void Next(QueueWork& work)
	{
		MutexLocker locker(sRuntimeLock);
		for (unsigned i = 0; i < kMaxRuntimeQueues; i++) {
			if (queues[i].used && queues[i].destroy) {
				work.type = kQueueWorkDestroy; work.slot = i; return;
			}
		}
		for (unsigned n = 0; n < kMaxRuntimeQueues; n++) {
			unsigned i = (nextQueue + n) % kMaxRuntimeQueues;
			QueueSlot& queue = queues[i];
			if (!queue.used || queue.first == NULL) continue;
			QueueJob* job = queue.first;
			status_t dependency = ReadySyncSubmission(job->synchronization);
			if (dependency == B_WOULD_BLOCK) continue;
			if (dependency != B_OK) {
				queue.error = dependency; queue.failedSequence = job->sequence;
				while (queue.first != NULL) {
					job = queue.first; queue.first = job->next;
					FinishSyncSubmission(job->synchronization, dependency); DeleteJob(job);
				}
				queue.last = NULL; queue.pending = 0;
				changed.NotifyAll();
				continue;
			}
			queue.first = job->next;
			if (queue.first == NULL) queue.last = NULL;
			job->next = NULL;
			queue.current = job;
			work = {kQueueWorkSubmit, i, job->address, job->bytes, job->sequence,
				job->lease.generation, ClientVmRoot(job->lease), &queue.memory, job};
			nextQueue = (i + 1) % kMaxRuntimeQueues;
			return;
		}
		if (live == 0 && creating == 0) {
			work.type = kQueueWorkStop;
			return;
		}
		changed.Wait(&sRuntimeLock, B_RELATIVE_TIMEOUT, 10000);
		work.type = kQueueWorkIdle;
	}
	HeapGrowthResult PrepareHeap(unsigned slot, uint64 context, uint32 vtStart,
		uint32 vtEnd, uint32 fragEnd, HeapGrowth& growth)
	{
		MutexLocker locker(sRuntimeLock);
		if (slot >= kMaxRuntimeQueues || !queues[slot].used) return kHeapGrowthInvalid;
		status_t status = PrepareClientHeapGrowth(queues[slot].activeLease, context,
			vtStart, vtEnd, fragEnd, growth);
		return status == B_OK ? kHeapGrowthOK : status == B_NO_MEMORY ? kHeapGrowthNoMemory : kHeapGrowthInvalid;
	}
	bool CommitHeap(HeapGrowth& growth) { return CommitClientHeapGrowth(growth) == B_OK; }
	void AbortHeap(HeapGrowth& growth) { AbortClientHeapGrowth(growth); }
	void Activated(const QueueWork& work)
	{
		MutexLocker locker(sRuntimeLock);
		QueueSlot& queue = queues[work.slot];
		QueueJob* job = (QueueJob*)work.cookie;
		ReleaseClientVm(queue.activeLease);
		queue.activeLease = job->lease;
		job->lease = {};
		queue.progress.generation = work.generation;
	}
	void Progress(unsigned slot, const QueueProgress& progress)
	{
		MutexLocker locker(sRuntimeLock);
		queues[slot].progress = progress;
	}
	void Complete(const QueueWork& work, const QueueProgress& progress)
	{
		MutexLocker locker(sRuntimeLock);
		QueueSlot& queue = queues[work.slot];
		queue.progress = progress;
		queue.completed = work.sequence;
		FinishSyncSubmission(queue.current->synchronization, B_OK);
		DeleteJob(queue.current);
		queue.current = NULL;
		queue.pending--;
		changed.NotifyAll();
		if (engine != NULL && engine->Stats().jobs % 5000 == 0)
			LogStats(engine->Stats());
	}
	static void LogStats(const QueueEngineStats& stats)
	{
		int64_t span = stats.lastMicros - stats.firstMicros;
		dprintf("mali_csf: %" B_PRIu64 " jobs, %" B_PRIu64 " queue switches;"
			" selecting %" B_PRId64 " us, running %" B_PRId64 " us of %" B_PRId64
			" us (%" B_PRId64 "%% busy)\n", stats.jobs, stats.switches,
			stats.selectMicros, stats.submitMicros, span,
			span > 0 ? (stats.selectMicros + stats.submitMicros) * 100 / span : 0);
	}
	void Destroyed(unsigned slot)
	{
		MutexLocker locker(sRuntimeLock);
		RetireQueue(slot);
	}
	// sRuntimeLock held, and the engine has detached this queue or the failed
	// runtime has finished a verified recovery. Never call for retained DMA.
	void RetireQueue(unsigned slot)
	{
		QueueSlot& queue = queues[slot];
		while (queue.first != NULL) {
			QueueJob* job = queue.first; queue.first = job->next; DeleteJob(job);
		}
		DeleteJob(queue.current);
		ReleaseClientVm(queue.activeLease);
		delete_area(queue.area);
		queue = QueueSlot();
		live--;
		changed.NotifyAll();
	}
	static void StopAdmission(void* data)
	{
		Runtime* runtime = (Runtime*)data;
		MutexLocker locker(sRuntimeLock);
		runtime->error = B_IO_ERROR;
		for (unsigned i = 0; i < kMaxRuntimeQueues; i++) {
			const QueueSlot& queue = runtime->queues[i];
			if (!queue.used) continue;
			dprintf("mali_csf: recovery queue handle=%" B_PRIu32 " submitted=%" B_PRIu64
				" completed=%" B_PRIu64 " current=%" B_PRIu64 " pending=%" B_PRIu32 "\n",
				queue.handle, queue.submitted, queue.completed,
				queue.current != NULL ? queue.current->sequence : uint64(0), queue.pending);
		}
		runtime->changed.NotifyAll();
	}
	static int32 Worker(void* data)
	{
		Runtime* runtime = (Runtime*)data;
		status_t status = runtime->hardware.Init(runtime->resources);
		QueueEngine<Runtime> engine(*runtime);
		runtime->engine = &engine;
		FirmwareRecovery recovery(StopAdmission, runtime);
		if (status == B_OK)
			CycleFirmware(runtime->hardware, runtime->memory, runtime->firmware, engine, &recovery);
		runtime->engine = NULL;
		if (engine.Stats().jobs > 0)
			LogStats(engine.Stats());
		bool retained = (runtime->firmware.flags & kFirmwareNeedsRecovery) != 0
			|| (engine.Mapped() && !recovery.recovered);
		if (status == B_OK && (retained || !runtime->ready || engine.Error() != 0
			|| runtime->firmware.result != 0 || runtime->firmware.cleanupResult != 0)) status = B_IO_ERROR;
		dprintf("mali_csf: runtime status=%" B_PRId32 " engine=%u fw=%u/%u flags=%#x"
			" fault=%#x fatal=%#x address=%#" B_PRIx64 " retained=%u\n",
			status, engine.Error(), runtime->firmware.result, runtime->firmware.cleanupResult,
			runtime->firmware.flags, engine.StreamFault(), engine.StreamFatal(),
			engine.FaultAddress(), retained);
		dprintf("mali_csf: heap events=%u grown=%u declined=%u; group suspend %u bytes,"
			" protected %u\n", engine.HeapEvents(), engine.HeapGrowths(),
			engine.HeapDeclines(), engine.SuspendBytes(), engine.ProtectedBytes());
		if (recovery.attempted) {
			dprintf("mali_csf: recovery result=%u reset=%u/%u flags=%#x irq=%u/%#x/%#x"
				" quiescent=%u restored=%u recovered=%u job=%#x mmu=%#x\n",
				recovery.result, recovery.reset.result, recovery.reset.cleanupResult,
				recovery.reset.flags, recovery.reset.capture.count, recovery.reset.capture.status,
				recovery.reset.capture.raw, recovery.quiescent,
				(runtime->firmware.flags & kFirmwarePowerRestored) != 0,
				recovery.recovered, recovery.jobRaw, recovery.mmuRaw);
			dprintf("mali_csf: recovery time start=%" B_PRId64 " irq=%" B_PRId64
				" end=%" B_PRId64 " cpu=%" B_PRId32 " power=%u/%u/%#x\n",
				recovery.reset.startedMicros, recovery.reset.capture.whenMicros,
				recovery.reset.finishedMicros, recovery.reset.capture.cpu,
				runtime->firmware.power.result, runtime->firmware.power.restoreResult,
				runtime->firmware.power.flags);
			const ResetSnapshot& idle = recovery.after;
			dprintf("mali_csf: recovery idle mask=%#x raw=%#x irq=%#x status=%#x mcu=%#x"
				" job_mask=%#x mmu_mask=%#x shader=%#x/%#x tiler=%#x/%#x l2=%#x/%#x"
				" transition=%#x/%#x/%#x high=%#x/%#x/%#x\n",
				idle.mask, idle.raw, idle.interruptStatus, idle.status, idle.mcu,
				idle.jobMask, idle.mmuMask, idle.shaderReady[0], idle.shaderReady[1],
				idle.tilerReady[0], idle.tilerReady[1], idle.l2Ready[0], idle.l2Ready[1],
				idle.shaderTransition, idle.tilerTransition, idle.l2Transition,
				recovery.transitionHigh[0], recovery.transitionHigh[1], recovery.transitionHigh[2]);
			for (unsigned as = 0; as < 2; as++) {
				const FirmwareRecovery::AddressSpace& space = recovery.spaces[as];
				dprintf("mali_csf: recovery AS%u table=%#" B_PRIx64 " attributes=%#" B_PRIx64
					" config=%#" B_PRIx64 " status=%#x\n", as, space.table,
					space.attributes, space.config, space.status);
			}
		}
		{
			MutexLocker locker(sRuntimeLock);
			runtime->error = status;
			runtime->retained = retained;
			runtime->finished = true;
			if (status != B_OK) {
				for (unsigned i = 0; i < kMaxRuntimeQueues; i++) {
					QueueSlot& queue = runtime->queues[i];
					if (queue.current != NULL) FinishSyncSubmission(queue.current->synchronization, status);
					for (QueueJob* job = queue.first; job != NULL; job = job->next)
						FinishSyncSubmission(job->synchronization, status);
				}
			}
			runtime->changed.NotifyAll();
		}
		PutRuntime(runtime);
		return B_OK;
	}

	ResourceInfo resources;
	QueueEngine<Runtime>* engine = NULL;	// while the worker runs
	FirmwareHardware hardware;
	FirmwareImage image;
	FirmwareMemory memory;
	FirmwareRunInfo firmware = {};
	InterfaceInfo interface = {};
	GpuProperties properties = {};
	void* file = NULL;
	area_id area = -1;
	thread_id thread = -1;
	ConditionVariable changed;
	QueueSlot queues[kMaxRuntimeQueues];
	uint32 references = 1;
	uint32 live = 0;
	uint32 creating = 1;
	uint32 nextQueue = 0;
	bool ready = false;
	bool finished = false;
	bool retained = false;
	status_t error = B_OK;
};

static void
PutRuntime(Runtime* runtime)
{
	bool last;
	{
		MutexLocker locker(sRuntimeLock);
		last = --runtime->references == 0;
	}
	if (last) delete runtime;
}

class RuntimeReference {
public:
	RuntimeReference()
	{
		MutexLocker locker(sRuntimeLock);
		value = sRuntime;
		if (value != NULL) value->references++;
	}
	~RuntimeReference() { if (value != NULL) PutRuntime(value); }
	Runtime* value;
};

static int
FindQueue(Runtime* runtime, void* client, uint32 handle)
{
	for (unsigned i = 0; i < kMaxRuntimeQueues; i++) {
		const QueueSlot& queue = runtime->queues[i];
		if (queue.used && queue.client == client && queue.handle == handle) return i;
	}
	return -1;
}

// Called under the external hardware lock, with an API reference held. Wait
// for the worker thread itself to exit before a final descriptor can unload us.
static void
CollectRuntime(Runtime* runtime, bool& needsRecovery)
{
	bool collect;
	{
		MutexLocker locker(sRuntimeLock);
		if (runtime->live != 0 || runtime->creating != 0) {
			needsRecovery |= runtime->retained;
			return;
		}
		while (!runtime->finished) runtime->changed.Wait(&sRuntimeLock);
		needsRecovery |= runtime->retained;
		collect = !runtime->retained;
	}
	if (runtime->thread >= B_OK) {
		status_t result;
		wait_for_thread(runtime->thread, &result);
		runtime->thread = -1;
	}
	if (collect) {
		{
			MutexLocker locker(sRuntimeLock);
			if (sRuntime != runtime) return;
			sRuntime = NULL;
		}
		PutRuntime(runtime);
	}
}

template<typename Request>
static status_t
ReadQueueRequest(void* user, size_t length, Request& request)
{
	if (length != sizeof(request)) return B_BAD_VALUE;
	if (user == NULL || user_memcpy(&request, user, length) != B_OK) return B_BAD_ADDRESS;
	return request.version == kClientVersion ? B_OK : B_BAD_VALUE;
}

static status_t
StartRuntime(const ResourceInfo& resources, void* user, uint32 bytes, Runtime*& result)
{
	if (bytes == 0) return B_ENTRY_NOT_FOUND;
	if (FirmwareMemoryRetained()) return B_BUSY;
	Runtime* runtime = new(std::nothrow) Runtime(resources);
	if (runtime == NULL) return B_NO_MEMORY;
	runtime->file = malloc(bytes);
	status_t status = runtime->file == NULL ? B_NO_MEMORY
		: user_memcpy(runtime->file, (uint8*)user + sizeof(QueueCreate), bytes);
	if (status == B_OK && (runtime->image.Init(runtime->file, bytes) != FIRMWARE_OK
		|| runtime->image.Info().versionHash != 0x01050000
		|| !PlanRuntimeFirmware(runtime->memory, runtime->image))) status = B_BAD_DATA;
	if (status == B_OK) {
		runtime->area = AllocateFirmwareMemory(runtime->memory);
		if (runtime->area < B_OK) status = runtime->area;
	}
	if (status != B_OK) { delete runtime; return status; }
	FirmwareRunInfo& info = runtime->firmware;
	info.version = kFirmwareRunVersion;
	info.firmwareBytes = bytes;
	info.flags = kFirmwareAllocated;
	info.tablePages = runtime->memory.TablePages();
	info.allocationBytes = runtime->memory.RequiredBytes();
	info.rootPhysical = runtime->memory.RootPhysical();
	info.translationConfig = FirmwareMemory::TranslationConfig();
	info.memoryAttributes = FirmwareMemory::MemoryAttributes();
	{
		MutexLocker locker(sRuntimeLock);
		runtime->thread = spawn_kernel_thread(Runtime::Worker, "Mali CSF scheduler",
			B_NORMAL_PRIORITY, runtime);
		if (runtime->thread >= B_OK) {
			runtime->references += 2; // global, worker, creator
			sRuntime = runtime;
		}
	}
	if (runtime->thread < B_OK) {
		status = runtime->thread; delete runtime; return status;
	}
	status = resume_thread(runtime->thread);
	if (status != B_OK) {
		// The created kernel thread has not run; terminate/join before reclaiming.
		kill_thread(runtime->thread);
		status_t returned; wait_for_thread(runtime->thread, &returned);
		{
			MutexLocker locker(sRuntimeLock);
			runtime->thread = -1;
			runtime->error = status;
			runtime->finished = true;
		}
		PutRuntime(runtime); // unused worker reference
	}
	result = runtime;
	return B_OK;
}

static status_t
CreateQueue(const ResourceInfo& resources, void* client, void* user, size_t length,
	bool& needsRecovery)
{
	QueueCreate request;
	if (length < sizeof(request)) return B_BAD_VALUE;
	status_t status = ReadQueueRequest(user, sizeof(request), request);
	if (status != B_OK) return status;
	if (request.flags != 0 || request.reserved != 0 || request.reserved2 != 0
		|| request.reserved3 != 0 || request.handle != 0 || request.generation != 0
		|| request.firmwareBytes > kMaxFirmwareBytes
		|| length != sizeof(request) + request.firmwareBytes) return B_BAD_VALUE;
	ClientVmLease lease = {};
	status = AcquireClientVm(client, request.vm, 0, lease);
	if (status != B_OK) return status;
	RuntimeReference reference;
	Runtime* runtime = reference.value;
	if (runtime == NULL) {
		status = StartRuntime(resources, user, request.firmwareBytes, runtime);
		if (status != B_OK) { ReleaseClientVm(lease); return status; }
		reference.value = runtime; // StartRuntime supplies the creator reference
	} else {
		MutexLocker locker(sRuntimeLock);
		if (request.firmwareBytes != 0 || runtime->finished || runtime->error != B_OK) {
			ReleaseClientVm(lease);
			return request.firmwareBytes != 0 ? B_BAD_VALUE : B_BUSY;
		}
		runtime->creating++;
	}
	int slot = -1;
	{
		MutexLocker locker(sRuntimeLock);
		while (!runtime->ready && !runtime->finished) runtime->changed.Wait(&sRuntimeLock);
		status = runtime->error;
		if (status == B_OK) {
			for (unsigned i = 0; i < kMaxRuntimeQueues; i++)
				if (!runtime->queues[i].used) { slot = i; break; }
			if (slot < 0 || sNextQueueHandle == 0) status = B_NO_MEMORY;
		}
	}
	QueueMemory memory;
	area_id area = status == B_OK ? AllocateFirmwareMemory(memory, "Mali CSF queue DMA") : -1;
	if (status == B_OK && area < B_OK) status = area;
	if (status == B_OK) {
		MutexLocker locker(sRuntimeLock);
		request.handle = sNextQueueHandle++;
		request.generation = lease.generation;
		status = runtime->finished ? B_IO_ERROR : user_memcpy(user, &request, sizeof(request));
		if (status == B_OK) {
			QueueSlot& queue = runtime->queues[slot];
			queue.used = true;
			queue.handle = request.handle;
			queue.vm = request.vm;
			queue.client = client;
			queue.memory = memory;
			queue.area = area; area = -1;
			queue.activeLease = lease; lease = {};
			runtime->live++;
		}
	}
	if (area >= B_OK) delete_area(area);
	ReleaseClientVm(lease);
	{
		MutexLocker locker(sRuntimeLock);
		runtime->creating--;
		runtime->changed.NotifyAll();
	}
	CollectRuntime(runtime, needsRecovery);
	return status;
}

static void
DestroyQueues(Runtime* runtime, void* client, uint32 handle, bool& needsRecovery)
{
	bool finished;
	{
		MutexLocker locker(sRuntimeLock);
		for (unsigned i = 0; i < kMaxRuntimeQueues; i++) {
			QueueSlot& queue = runtime->queues[i];
			if (queue.used && queue.client == client && (handle == 0 || queue.handle == handle)) {
				// Record the actual close boundary, before the worker can retire
				// this queue. A userspace fence observation alone can race completion.
				if (handle == 0 && queue.pending != 0 && !queue.destroy) {
					dprintf("mali_csf: close pending handle=%" B_PRIu32
						" pending=%" B_PRIu32 " submitted=%" B_PRIu64
						" completed=%" B_PRIu64 " current=%" B_PRIu64
						" failed=%" B_PRIu64 " error=%" B_PRId32 "\n",
						queue.handle, queue.pending, queue.submitted, queue.completed,
						queue.current != NULL ? queue.current->sequence : uint64(0),
						queue.failedSequence, queue.error);
				}
				queue.destroy = true;
			}
		}
		runtime->changed.NotifyAll();
		for (;;) {
			bool waiting = false;
			for (unsigned i = 0; i < kMaxRuntimeQueues; i++) {
				QueueSlot& queue = runtime->queues[i];
				if (!queue.used || queue.client != client || !queue.destroy) continue;
				if (runtime->finished) {
					if (runtime->retained) queue.client = NULL;
					else runtime->RetireQueue(i);
				} else waiting = true;
			}
			if (!waiting) break;
			runtime->changed.Wait(&sRuntimeLock);
		}
		needsRecovery |= runtime->retained;
		finished = runtime->finished;
	}
	// Even a retained runtime must have exited its worker before close returns.
	if (finished && runtime->thread >= B_OK) {
		status_t result; wait_for_thread(runtime->thread, &result); runtime->thread = -1;
	}
	CollectRuntime(runtime, needsRecovery);
}

void
CloseQueues(void* client, bool& needsRecovery)
{
	RuntimeReference reference;
	if (reference.value != NULL) DestroyQueues(reference.value, client, 0, needsRecovery);
}

static status_t
SubmitQueue(Runtime* runtime, void* client, void* user, size_t length, bool synchronized,
	void* syncClient)
{
	QueueSubmitSync extended = {};
	QueueSubmit& request = extended.queue;
	SyncPoint points[2 * kMaxSyncPoints];
	status_t status;
	if (synchronized) {
		if (length < sizeof(extended)) return B_BAD_VALUE;
		status = user == NULL ? B_BAD_ADDRESS : user_memcpy(&extended, user, sizeof(extended));
		if (status != B_OK) return status;
		if (request.version != kClientVersion || extended.reserved != 0
			|| extended.waitCount > kMaxSyncPoints || extended.signalCount > kMaxSyncPoints
			|| length != sizeof(extended) + (extended.waitCount + extended.signalCount) * sizeof(SyncPoint))
			return B_BAD_VALUE;
		uint32 count = extended.waitCount + extended.signalCount;
		if (count != 0) status = user_memcpy(points, (uint8*)user + sizeof(extended), count * sizeof(SyncPoint));
	} else status = ReadQueueRequest(user, length, request);
	if (status != B_OK) return status;
	if (request.flags != 0 || request.reserved != 0 || request.sequence != 0
		|| (request.streamAddress & 7) != 0 || (request.streamBytes & 7) != 0
		|| request.streamBytes > kMaxStreamBytes
		|| (request.streamBytes == 0 && request.streamAddress != 0)) return B_BAD_VALUE;
	uint32 vm;
	{
		MutexLocker locker(sRuntimeLock);
		int slot = FindQueue(runtime, client, request.handle);
		if (slot < 0) return B_ENTRY_NOT_FOUND;
		vm = runtime->queues[slot].vm;
	}
	QueueJob* job = (QueueJob*)calloc(1, sizeof(QueueJob));
	if (job == NULL) return B_NO_MEMORY;
	status = AcquireClientVm(client, vm, request.generation, job->lease);
	if (status == B_OK && request.streamBytes != 0
		&& !ClientVmRange(job->lease, request.streamAddress, request.streamBytes)) status = B_BAD_VALUE;
	if (status != B_OK) { DeleteJob(job); return status; }
	job->address = request.streamAddress;
	job->bytes = request.streamBytes;
	{
		MutexLocker locker(sRuntimeLock);
		int slot = FindQueue(runtime, client, request.handle);
		if (slot < 0) status = B_ENTRY_NOT_FOUND;
		else {
			QueueSlot& queue = runtime->queues[slot];
			if (runtime->error != B_OK || runtime->finished) status = B_IO_ERROR;
			else if (queue.error != B_OK) status = queue.error;
			else if (queue.destroy) status = B_CANCELED;
			else if (queue.pending == kMaxQueueJobs) status = B_BUSY;
			else if (queue.submitted + 1 >= UINT64_MAX / 128) status = B_NO_MEMORY;
			else {
				request.sequence = queue.submitted + 1;
				request.generation = job->lease.generation;
				if (synchronized) status = PrepareSyncSubmission(syncClient, points,
					extended.waitCount, extended.signalCount, job->synchronization);
				if (status == B_OK) {
					status = user_memcpy(user, &request, sizeof(request));
					if (synchronized) {
						if (status == B_OK) CommitSyncSubmission(job->synchronization);
						else AbortSyncSubmission(job->synchronization);
					}
				}
				if (status == B_OK) {
					job->sequence = request.sequence;
					if (queue.last != NULL) queue.last->next = job;
					else queue.first = job;
					queue.last = job;
					queue.submitted = request.sequence;
					queue.pending++;
					job = NULL;
					runtime->changed.NotifyAll();
				}
			}
		}
	}
	DeleteJob(job);
	return status;
}

static status_t
WaitQueue(Runtime* runtime, void* client, void* user, size_t length)
{
	QueueWait request;
	status_t status = ReadQueueRequest(user, length, request);
	if (status != B_OK) return status;
	if (request.flags != 0 || request.result != 0 || request.completed != 0
		|| request.sequence == 0 || request.timeoutMicros < -1) return B_BAD_VALUE;
	bigtime_t now = system_time();
	if (request.timeoutMicros > INT64_MAX - now) return B_BAD_VALUE;
	bigtime_t deadline = request.timeoutMicros >= 0 ? now + request.timeoutMicros : 0;
	MutexLocker locker(sRuntimeLock);
	for (;;) {
		int slot = FindQueue(runtime, client, request.handle);
		if (slot < 0) return B_ENTRY_NOT_FOUND;
		QueueSlot& queue = runtime->queues[slot];
		if (request.sequence > queue.submitted) return B_BAD_VALUE;
		if (queue.completed >= request.sequence || runtime->error != B_OK || queue.error != B_OK || queue.destroy) {
			request.completed = queue.completed;
			request.result = queue.completed >= request.sequence ? B_OK
				: runtime->error != B_OK ? runtime->error : queue.error != B_OK ? queue.error : B_CANCELED;
			return user_memcpy(user, &request, sizeof(request));
		}
		if (request.timeoutMicros == 0) return B_TIMED_OUT;
		status = runtime->changed.Wait(&sRuntimeLock, B_CAN_INTERRUPT
			| (request.timeoutMicros >= 0 ? B_ABSOLUTE_TIMEOUT : 0), deadline);
		if (status != B_OK) return status;
	}
}

status_t
ControlQueues(const ResourceInfo& resources, void* client, uint32 op, void* user,
	size_t length, bool& needsRecovery, void* syncClient)
{
	if (op == kCreateQueue) return CreateQueue(resources, client, user, length, needsRecovery);
	RuntimeReference reference;
	Runtime* runtime = reference.value;
	if (runtime == NULL) return B_ENTRY_NOT_FOUND;
	if (op == kSubmitQueue || op == kSubmitQueueSync)
		return SubmitQueue(runtime, client, user, length, op == kSubmitQueueSync, syncClient);
	if (op == kWaitQueue) return WaitQueue(runtime, client, user, length);
	if (op == kDestroyQueue) {
		QueueHandle request;
		status_t status = ReadQueueRequest(user, length, request);
		if (status != B_OK) return status;
		if (request.reserved != 0 || request.handle == 0) return B_BAD_VALUE;
		{
			MutexLocker locker(sRuntimeLock);
			if (FindQueue(runtime, client, request.handle) < 0) return B_ENTRY_NOT_FOUND;
		}
		DestroyQueues(runtime, client, request.handle, needsRecovery);
		return needsRecovery ? B_IO_ERROR : B_OK;
	}
	if (op == kGetQueueProperties) {
		QueueProperties request;
		status_t status = ReadQueueRequest(user, length, request);
		if (status != B_OK) return status;
		if (request.flags != 0 || request.reserved != 0
			|| request.reserved2[0] != 0 || request.reserved2[1] != 0) return B_BAD_VALUE;
		MutexLocker locker(sRuntimeLock);
		if (FindQueue(runtime, client, request.handle) < 0) return B_ENTRY_NOT_FOUND;
		QueueProperties result = {};
		result.version = kClientVersion; result.handle = request.handle;
		result.gpu = runtime->properties;
		return user_memcpy(user, &result, sizeof(result));
	}
	if (op == kGetQueueResetInfo) {
		QueueResetInfo request;
		status_t status = ReadQueueRequest(user, length, request);
		if (status != B_OK) return status;
		if (request.state != 0 || request.error != 0
			|| request.reserved[0] != 0 || request.reserved[1] != 0) return B_BAD_VALUE;
		MutexLocker locker(sRuntimeLock);
		int slot = FindQueue(runtime, client, request.handle);
		if (slot < 0) return B_ENTRY_NOT_FOUND;
		QueueResetInfo result = {};
		result.version = kClientVersion; result.handle = request.handle;
		result.error = runtime->error != B_OK ? runtime->error : runtime->queues[slot].error;
		if (runtime->error != B_OK) {
			result.state = !runtime->finished ? kQueueResetPending
				: runtime->retained ? kQueueResetUnrecoverable : kQueueResetQuiescent;
		} else if (result.error != B_OK) {
			result.state = kQueueResetLocalError;
		}
		return user_memcpy(user, &result, sizeof(result));
	}
	if (op != kGetQueueInfo) return B_DEV_INVALID_IOCTL;
	QueueInfo request;
	status_t status = ReadQueueRequest(user, length, request);
	if (status != B_OK) return status;
	if (request.reserved != 0) return B_BAD_VALUE;
	MutexLocker locker(sRuntimeLock);
	int slot = FindQueue(runtime, client, request.handle);
	if (slot < 0) return B_ENTRY_NOT_FOUND;
	const QueueSlot& queue = runtime->queues[slot];
	QueueInfo info = {};
	info.version = kClientVersion; info.handle = queue.handle; info.vm = queue.vm;
	info.state = runtime->error != B_OK || queue.error != B_OK ? kQueueFailed : queue.destroy ? kQueueStopping
		: queue.current != NULL ? kQueueRunning : kQueueReady;
	info.error = runtime->error != B_OK ? runtime->error : queue.error;
	info.pending = queue.pending;
	info.submitted = queue.submitted;
	info.completed = queue.completed;
	info.activeGeneration = queue.progress.generation;
	info.insert = queue.progress.insert; info.extract = queue.progress.extract;
	info.failedSequence = queue.failedSequence != 0 ? queue.failedSequence
		: runtime->error != B_OK && queue.completed < queue.submitted ? queue.completed + 1 : 0;
	info.interrupts = queue.progress.interrupts; info.syncEvents = queue.progress.syncEvents;
	info.suspends = queue.progress.suspends; info.resumes = queue.progress.resumes;
	return user_memcpy(user, &info, sizeof(info));
}
