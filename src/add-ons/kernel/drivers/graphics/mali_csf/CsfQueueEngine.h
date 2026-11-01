/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#ifndef MALI_CSF_QUEUE_ENGINE_H
#define MALI_CSF_QUEUE_ENGINE_H

#include "CsfRun.h"
#include "CsfQueueMemory.h"
#include "CsfHeapGrowth.h"
#include "CsfPropertyCapture.h"

namespace MaliCSF {

enum QueueWorkType { kQueueWorkIdle, kQueueWorkSubmit, kQueueWorkDestroy, kQueueWorkStop };

// The feed owns the job and its VM lease until Complete(), and each queue's
// active lease until it is replaced or Destroyed(). The engine owns no OS
// objects. Only this worker touches GPU registers and private queue memory.
struct QueueWork {
	QueueWorkType type;
	uint32_t slot;
	uint64_t address;
	uint32_t bytes;
	uint64_t sequence;
	uint64_t generation;
	const uint64_t* root;
	QueueMemory* memory;
	void* cookie;
};

// Where the hardware worker's time goes: selecting a queue (suspending the
// group's previous one and resuming this one), running its job, idle.
struct QueueEngineStats {
	uint64_t jobs;
	uint64_t switches;
	int64_t selectMicros;
	int64_t submitMicros;
	int64_t firstMicros;
	int64_t lastMicros;
};

struct QueueProgress {
	uint64_t generation;
	uint64_t completed;
	uint64_t insert;
	uint64_t extract;
	uint32_t interrupts;
	uint32_t syncEvents;
	uint32_t suspends;
	uint32_t resumes;
};

enum QueueEngineError {
	kQueueEngineOK, kQueueEngineInterface, kQueueEngineConfig, kQueueEngineMap,
	kQueueEngineStart, kQueueEngineSuspend, kQueueEngineResume, kQueueEngineFreshness,
	kQueueEngineCompletion, kQueueEngineFault, kQueueEngineTerminate, kQueueEngineUnmap,
	kQueueEngineHalt
};

template<typename Feed>
class QueueEngine {
public:
	explicit QueueEngine(Feed& feed) : fFeed(feed) {}
	uint32_t CompletedAddressSpaces() const { return 3u << 16; }
	QueueEngineError Error() const { return fError; }
	bool Mapped() const { return fMapped; }
	uint32_t StreamFault() const { return fStreamFault; }
	uint32_t StreamFatal() const { return fStreamFatal; }
	uint64_t FaultAddress() const { return fFaultAddress; }
	uint32_t HeapEvents() const { return fHeapEvents; }
	uint32_t HeapGrowths() const { return fHeapGrowths; }
	uint32_t HeapDeclines() const { return fHeapDeclines; }
	uint32_t SuspendBytes() const { return fInterface.suspendBytes; }
	const QueueEngineStats& Stats() const { return fStats; }
	uint32_t ProtectedBytes() const { return fInterface.protectedBytes; }

	template<typename IO>
	bool Run(IO& io, FirmwareMemory& memory, FirmwareRunInfo& firmware)
	{
		fMemory = &memory;
		fError = kQueueEngineInterface;
		InterfaceInfo global;
		io.MemoryBarrier();
		if (!InspectInterface(memory.SharedData(), memory.SharedBytes(), memory.SharedAddress(),
			global, &fInterface) || memcmp(&global, &firmware.interface, sizeof(global)) != 0
			|| fInterface.suspendBytes > kRuntimeSuspendBytes
			|| fInterface.protectedBytes > kRuntimeSuspendBytes)
			return false;
		volatile uint32_t* shared = (volatile uint32_t*)memory.SharedData();
		fGlobalIn = shared + global.inputOffset / 4;
		fGlobalOut = shared + global.outputOffset / 4;
		fGroupIn = shared + fInterface.groupInputOffset / 4;
		fGroupOut = shared + fInterface.groupOutputOffset / 4;
		fStreamIn = shared + fInterface.streamInputOffset / 4;
		fStreamOut = shared + fInterface.streamOutputOffset / 4;
		fError = kQueueEngineConfig;
		uint32_t timer = io.TimerRate();
		GpuProperties properties = CaptureGpuProperties(io, global, fInterface, timer);
		fShaderMask = properties.shaderPresent;
		fTilerMask = properties.tilerPresent;
		if (timer == 0 || timer > 1000000000 || fShaderMask == 0 || fTilerMask == 0
			|| fTilerMask > 0xffffffff || (fGroupOut[0] & 7) != 0
			|| (fStreamOut[0] & 7) != 0 || !io.ArmCommandJob())
			return false;
		fGlobalIn[4] = (UINT64_C(5) * 500 * 1024 * 1024) >> 10;
		fGlobalIn[5] = (UINT64_C(10000) * timer + 1023999999) / 1024000000;
		WriteInterface64(fGlobalIn, 24, fShaderMask);
		fGlobalIn[1] = 0x10e;
		fGlobalIn[0] = (fGlobalIn[0] & ~0x40eu) | ((fGlobalOut[0] ^ 14) & 14);
		io.MemoryBarrier();
		io.RingFirmwareDoorbell();
		if (!WaitFirmware(io, 100000, [&]() {
			io.MemoryBarrier();
			return ((fGlobalOut[0] ^ fGlobalIn[0]) & 0x40e) == 0 || io.FirmwareFaulted();
		}) || io.FirmwareFaulted())
			return false;
		fError = kQueueEngineOK;
		fFeed.Ready(global, properties);
		for (;;) {
			Poll(io);
			if (Faulted(io)) { fError = kQueueEngineFault; return false; }
			QueueWork work = {};
			fFeed.Next(work);
			if (work.type == kQueueWorkIdle) continue;
			if (work.type == kQueueWorkStop) return true;
			if (work.slot >= kMaxRuntimeQueues) { fError = kQueueEngineInterface; return false; }
			if (work.type == kQueueWorkDestroy) {
				if (fActive == int(work.slot) && !Detach(io, true)) return false;
				fStates[work.slot] = {};
				fFeed.Destroyed(work.slot);
				continue;
			}
			if (work.type != kQueueWorkSubmit || work.memory == NULL
				|| work.sequence == 0 || work.sequence >= UINT64_MAX / 128) {
				fError = kQueueEngineInterface;
				return false;
			}
			bool switching = fActive != int(work.slot)
				|| fStates[work.slot].progress.generation != work.generation;
			int64_t selectStart = io.Now();
			if (!Select(io, work)) return false;
			int64_t submitStart = io.Now();
			if (!Submit(io, work)) return false;
			int64_t submitEnd = io.Now();
			if (fStats.jobs == 0) fStats.firstMicros = selectStart;
			fStats.lastMicros = submitEnd;
			fStats.jobs++;
			fStats.switches += switching ? 1 : 0;
			fStats.selectMicros += submitStart - selectStart;
			fStats.submitMicros += submitEnd - submitStart;
			fFeed.Complete(work, fStates[work.slot].progress);
		}
	}

	template<typename IO>
	bool BeforeStop(IO& io)
	{
		bool ok = true;
		if (fActive >= 0 && !Detach(io, true)) ok = false;
		if (fGlobalIn != NULL) {
			fGlobalIn[0] |= 1;
			io.MemoryBarrier();
			io.RingFirmwareDoorbell();
			if (!WaitFirmware(io, 100000, [&]() { return io.ReadGpu(0x704) == 2; })) {
				if (fError == kQueueEngineOK) fError = kQueueEngineHalt;
				ok = false;
			}
		}
		return ok;
	}

	template<typename IO>
	bool Cleanup(IO& io, bool stopped)
	{
		if (!fMapped) return true;
		return stopped && Unmap(io);
	}

private:
	struct State {
		bool started;
		QueueMemory* memory;
		QueueProgress progress;
	};
	static unsigned BitCount(uint64_t mask)
	{
		unsigned result = 0;
		for (; mask != 0; mask >>= 1) result += mask & 1;
		return result;
	}
	volatile uint32_t* QueueIn(unsigned slot)
	{
		return (volatile uint32_t*)fMemory->WorkspaceData(0) + slot * 8192 / 4;
	}
	const volatile uint32_t* QueueOut(unsigned slot) { return QueueIn(slot) + 4096 / 4; }
	template<typename IO> void Ring(IO& io, bool queue)
	{
		if (queue) fGroupIn[2] = (fGroupIn[2] & ~1u) | ((fGroupOut[2] ^ 1) & 1);
		fGlobalIn[2] = (fGlobalIn[2] & ~1u) | ((fGlobalOut[2] ^ 1) & 1);
		io.MemoryBarrier();
		io.RingFirmwareDoorbell();
	}
	template<typename IO> bool Faulted(IO& io) { return fFault || io.FirmwareFaulted(); }
	// panthor_sched.c/panthor_mmu.c (MIT option): an OOM reply publishes a new
	// chunk, or zero so firmware can wait/reclaim/run the userspace exception
	// handler. The only hardware worker holds the active VM lease throughout.
	template<typename IO> bool HeapOom(IO& io)
	{
		fHeapEvents++;
		if (fActive < 0 || !fMapped) return false;
		HeapGrowth growth = {};
		HeapGrowthResult result = fFeed.PrepareHeap(fActive, ReadInterface64(fStreamOut, 208),
			fStreamOut[48], fStreamOut[49], fStreamOut[51], growth);
		uint64_t chunk = 0;
		if (result == kHeapGrowthOK) {
			chunk = growth.encodedChunk;
			WriteGpu64(io, 0x2450, 47); // AS1: lock the whole 48-bit address space
			if (!FirmwareAsCommand(io, 2, 1)) {
				fFeed.AbortHeap(growth); return false;
			}
			bool committed = fFeed.CommitHeap(growth);
			io.MemoryBarrier();
			bool flushed = committed && FlushLockedFirmwareCaches(io);
			bool unlocked = FirmwareAsCommand(io, 3, 1);
			// Commit transferred all allocations into the leased heap. Even if
			// flush/unlock fails, abort is empty and cleanup retains those pages.
			if (!committed || !flushed || !unlocked) {
				fFeed.AbortHeap(growth); return false;
			}
			fHeapGrowths++;
		} else {
			fFeed.AbortHeap(growth);
			if (result != kHeapGrowthNoMemory) return false;
			fHeapDeclines++;
		}
		WriteInterface64(fStreamIn, 32, chunk);
		WriteInterface64(fStreamIn, 40, chunk);
		fStreamIn[0] = (fStreamIn[0] & ~(1u << 26)) | (fStreamOut[0] & (1u << 26));
		Ring(io, true);
		return true;
	}
	template<typename IO> void Poll(IO& io)
	{
		io.MemoryBarrier();
		if (fGroupIn == NULL) return;
		uint32_t ack = fGroupOut[0], events = (fGroupIn[0] ^ ack) & 0xb0000000u;
		uint32_t irqs = fGroupIn[3] ^ fGroupOut[3];
		fGroupIn[3] = fGroupOut[3];
		io.MemoryBarrier();
		uint32_t streamEvents = (fStreamIn[0] ^ fStreamOut[0]) & 0xcc000000u;
		if (fActive >= 0 && (events & (1u << 28)) != 0)
			fStates[fActive].progress.syncEvents++;
		if (fStreamOut[32] != 0 && fStreamFault == 0) {
			fStreamFault = fStreamOut[32];
			fFaultAddress = ReadInterface64(fStreamOut, 136);
		}
		if (fStreamOut[33] != 0 && fStreamFatal == 0) {
			fStreamFatal = fStreamOut[33];
			if (fStreamFault == 0) fFaultAddress = ReadInterface64(fStreamOut, 144);
		}
		if ((events & (1u << 31)) != 0 || (streamEvents & ~(1u << 26)) != 0 || (irqs & ~1u) != 0
			|| fStreamFault != 0 || fStreamFatal != 0)
			fFault = true;
		if (!fFault && (streamEvents & (1u << 26)) != 0 && !HeapOom(io))
			fFault = true;
		fGroupIn[0] = (fGroupIn[0] & ~0xb0000000u) | (ack & 0xb0000000u);
		if (events != 0 || irqs != 0) Ring(io, false);
	}
	template<typename IO> bool Unmap(IO& io)
	{
		if (!fMapped) return true;
		if (!FlushFirmware(io, 1)) { fError = kQueueEngineUnmap; return false; }
		WriteGpu64(io, 0x2470, 1);
		WriteGpu64(io, 0x2440, 0);
		WriteGpu64(io, 0x2448, 0);
		if (!FirmwareAsCommand(io, 1, 1) || ReadGpu64(io, 0x2470) != 1
			|| ReadGpu64(io, 0x2440) != 0 || ReadGpu64(io, 0x2448) != 0) {
			fError = kQueueEngineUnmap;
			return false;
		}
		fMapped = false;
		return true;
	}
	template<typename IO> bool Detach(IO& io, bool terminate)
	{
		if (fActive < 0) return !fMapped;
		unsigned slot = fActive;
		fGroupIn[0] = (fGroupIn[0] & ~7u) | (terminate ? 0u : 2u);
		Ring(io, false);
		bool done = WaitFirmware(io, 100000, [&]() {
			Poll(io);
			return (fGroupOut[0] & 7) == (terminate ? 0u : 2u) || io.FirmwareFaulted();
		});
		if (!done || Faulted(io)) {
			fError = terminate ? kQueueEngineTerminate : kQueueEngineSuspend;
			return false;
		}
		// Linux resets the CS request without ringing after CSG suspension; the
		// request is reprogrammed before the next START/RESUME of that group.
		fStreamIn[0] &= ~7u;
		io.MemoryBarrier();
		if (!Unmap(io)) return false;
		if (!terminate) fStates[slot].progress.suspends++;
		fFeed.Progress(slot, fStates[slot].progress);
		fActive = -1;
		return true;
	}
	template<typename IO> bool Select(IO& io, const QueueWork& work)
	{
		State& state = fStates[work.slot];
		if (fActive == int(work.slot) && state.progress.generation == work.generation)
			return true;
		if (fActive >= 0 && !Detach(io, false)) return false;
		fError = kQueueEngineMap;
		if (fMapped || (io.ReadGpu(0x18) & 2) == 0 || (io.ReadGpu(0x2468) & 1) != 0
			|| ReadGpu64(io, 0x2470) != 1 || !work.memory->SetUserRoot(work.root))
			return false;
		state.memory = work.memory;
		// Both old active and incoming job leases remain owned by the feed until
		// this mapping is acknowledged. Failure retains them through cleanup.
		fMapped = true;
		io.MemoryBarrier();
		WriteGpu64(io, 0x2440, work.memory->RootPhysical());
		WriteGpu64(io, 0x2448, FirmwareMemory::MemoryAttributes());
		WriteGpu64(io, 0x2470, FirmwareMemory::TranslationConfig());
		if (!FirmwareAsCommand(io, 1, 1)
			|| ReadGpu64(io, 0x2440) != work.memory->RootPhysical()
			|| ReadGpu64(io, 0x2448) != FirmwareMemory::MemoryAttributes()
			|| ReadGpu64(io, 0x2470) != FirmwareMemory::TranslationConfig())
			return false;
		fFeed.Activated(work);
		state.progress.generation = work.generation;
		if (!state.started) {
			memset((void*)QueueIn(work.slot), 0, 8192);
		} else {
			state.progress.resumes++;
		}
		WriteInterface64(QueueIn(work.slot), 8, ReadInterface64(QueueOut(work.slot), 0));
		WriteInterface64(fStreamIn, 16, kPrivateRingAddress);
		fStreamIn[6] = 65536;
		uint32_t interface = kRuntimeInterfaceAddress + work.slot * 8192;
		WriteInterface64(fStreamIn, 48, interface);
		WriteInterface64(fStreamIn, 56, interface + 4096);
		fStreamIn[1] = 1u << 8;
		fStreamIn[3] = ~0u;
		fStreamIn[0] = (fStreamIn[0] & ~0x517u) | 0x511u;
		WriteInterface64(fGroupIn, 32, fShaderMask);
		WriteInterface64(fGroupIn, 40, fShaderMask);
		fGroupIn[12] = uint32_t(fTilerMask);
		fGroupIn[13] = BitCount(fShaderMask) | (BitCount(fShaderMask) << 8)
			| (BitCount(fTilerMask) << 16);
		WriteInterface64(fGroupIn, 64, fInterface.suspendBytes
			? kRuntimeSuspendAddress + work.slot * kRuntimeSuspendBytes : 0);
		WriteInterface64(fGroupIn, 72, fInterface.protectedBytes
			? kRuntimeProtectedAddress + work.slot * kRuntimeSuspendBytes : 0);
		fGroupIn[20] = 1;
		fGroupIn[1] = ~0u;
		fGroupIn[3] = fGroupOut[3];
		fGroupIn[0] = (fGroupIn[0] & ~0xb0000017u) | (fGroupOut[0] & 0xb0000000u)
			| ((fGroupOut[0] ^ 16) & 16) | (state.started ? 3u : 1u);
		fActive = work.slot; // the request can expose all group/queue memory
		fError = state.started ? kQueueEngineResume : kQueueEngineStart;
		Ring(io, true);
		if (!WaitFirmware(io, 100000, [&]() {
			Poll(io);
			return (((fGroupIn[0] ^ fGroupOut[0]) & 0x17) == 0
				&& ((fStreamIn[0] ^ fStreamOut[0]) & 7) == 0) || Faulted(io);
		}) || Faulted(io))
			return false;
		state.started = true;
		fError = kQueueEngineOK;
		return true;
	}
	template<typename IO> bool Submit(IO& io, const QueueWork& work)
	{
		State& state = fStates[work.slot];
		QueueProgress& progress = state.progress;
		const volatile uint32_t* completion = (const volatile uint32_t*)work.memory->Completion();
		fError = kQueueEngineFreshness;
		io.MemoryBarrier();
		if (work.sequence != progress.completed + 1 || ReadInterface64(completion, 0) != progress.completed
			|| completion[2] != 0 || completion[3] != 0
			|| ReadInterface64(QueueIn(work.slot), 0) != progress.insert
			|| ReadInterface64(QueueOut(work.slot), 0) != progress.insert)
			return false;
		uint64_t* ring = (uint64_t*)((uint8_t*)work.memory->Ring() + (progress.insert & 65535));
		if (!BuildQueueWrapper(ring, work.address, work.bytes)) return false;
		uint32_t irqBefore = io.CommandJobCount(), syncBefore = progress.syncEvents;
		progress.insert += 128;
		io.MemoryBarrier();
		WriteInterface64(QueueIn(work.slot), 8, ReadInterface64(QueueOut(work.slot), 0));
		WriteInterface64(QueueIn(work.slot), 0, progress.insert);
		Ring(io, true);
		fError = kQueueEngineCompletion;
		bool done = WaitFirmware(io, 5000000, [&]() {
			Poll(io);
			return (ReadInterface64(completion, 0) == work.sequence
				&& ReadInterface64(QueueOut(work.slot), 0) == progress.insert
				&& io.CommandJobCount() != irqBefore && progress.syncEvents != syncBefore)
				|| Faulted(io);
		});
		io.MemoryBarrier();
		progress.extract = ReadInterface64(QueueOut(work.slot), 0);
		progress.interrupts += io.CommandJobCount() - irqBefore;
		if (Faulted(io)) { fError = kQueueEngineFault; return false; }
		if (!done || completion[2] != 0 || completion[3] != 0) return false;
		progress.completed = work.sequence;
		fError = kQueueEngineOK;
		return true;
	}
	Feed& fFeed;
	FirmwareMemory* fMemory = NULL;
	QueueInterfaceInfo fInterface = {};
	State fStates[kMaxRuntimeQueues] = {};
	volatile uint32_t* fGlobalIn = NULL;
	const volatile uint32_t* fGlobalOut = NULL;
	volatile uint32_t* fGroupIn = NULL;
	const volatile uint32_t* fGroupOut = NULL;
	volatile uint32_t* fStreamIn = NULL;
	const volatile uint32_t* fStreamOut = NULL;
	uint64_t fShaderMask = 0, fTilerMask = 0;
	int fActive = -1;
	bool fMapped = false, fFault = false;
	QueueEngineError fError = kQueueEngineOK;
	uint32_t fStreamFault = 0, fStreamFatal = 0;
	uint64_t fFaultAddress = 0;
	uint32_t fHeapEvents = 0, fHeapGrowths = 0, fHeapDeclines = 0;
	QueueEngineStats fStats = {};
};

} // namespace MaliCSF
#endif
