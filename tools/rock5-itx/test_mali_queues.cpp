/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#include "CsfQueueEngine.h"
#define MALI_RUN_MODEL_ONLY
#include "test_mali_run.cpp"
#undef MALI_RUN_MODEL_ONLY
#include "test_mali_page_table.h"
#include <array>
#include <memory>

static void PrivateMemory()
{
	QueueMemory memory;
	Guarded arena(memory.RequiredBytes()), user(4096);
	memset(user.data, 0, 4096);
	for (uint64_t physical : {UINT64_C(0x1000), UINT64_C(0x1a2300000),
		(UINT64_C(1) << 40) - memory.RequiredBytes()}) {
		assert(memory.Build(arena.data, arena.size, physical));
		uint64_t pages[4];
		for (unsigned i = 0; i < 4; i++) pages[i] = physical + i * 4096;
		auto leaves = DecodeGpuTables(arena.data, pages, 4);
		assert(leaves.size() == 17);
		for (unsigned i = 0; i < 17; i++) {
			uint64_t entry = leaves.at((UINT64_C(1) << 47) + i * 4096);
			assert((entry & UINT64_C(0xfffffff000)) == physical + 16384 + i * 4096);
			assert((entry & ~UINT64_C(0xfffffff000)) == ((UINT64_C(3) << 53) | 0x643));
		}
		uint64_t upper[256]; memcpy(upper, arena.data + 2048, sizeof(upper));
		((uint64_t*)user.data)[255] = UINT64_C(0x182301003);
		assert(memory.SetUserRoot((uint64_t*)user.data));
		assert(memcmp(arena.data, user.data, 2048) == 0);
		assert(memcmp(arena.data + 2048, upper, sizeof(upper)) == 0);
		for (auto bad : {UINT64_C(1), UINT64_C(0x1003) | (UINT64_C(1) << 40), UINT64_C(0x1043)}) {
			((uint64_t*)user.data)[1] = bad;
			assert(!memory.SetUserRoot((uint64_t*)user.data));
			assert(((uint64_t*)arena.data)[1] == 0);
		}
		((uint64_t*)user.data)[1] = 0;
		((uint64_t*)user.data)[256] = 0x1003;
		assert(!memory.SetUserRoot((uint64_t*)user.data));
		((uint64_t*)user.data)[256] = 0;
		((uint64_t*)user.data)[257] = UINT64_C(0x182304003);
		assert(memory.SetUserRoot((uint64_t*)user.data));
		assert(((uint64_t*)arena.data)[257] == UINT64_C(0x182304003));
		assert(((uint64_t*)arena.data)[256] == upper[0]);
		((uint64_t*)user.data)[258] = 0x1003;
		assert(!memory.SetUserRoot((uint64_t*)user.data));
		((uint64_t*)user.data)[258] = ((uint64_t*)user.data)[257] = 0;
		assert(memory.SetUserRoot((uint64_t*)user.data));
		assert(memcmp(arena.data + 2048, upper, sizeof(upper)) == 0);
		assert(!memory.SetUserRoot(NULL) && !memory.SetUserRoot((uint64_t*)(user.data + 8)));
	}
	assert(!memory.Build(NULL, arena.size, 0x1000));
	assert(!memory.Build(arena.data + 8, arena.size, 0x1000));
	assert(!memory.Build(arena.data, arena.size - 4096, 0x1000));
	assert(!memory.Build(arena.data, arena.size, 0x1001));
	assert(!memory.Build(arena.data, arena.size, UINT64_C(1) << 40));
	assert(!memory.Build(arena.data, arena.size, (UINT64_C(1) << 40) - 4096));
	assert(memory.Root() == NULL && memory.Ring() == NULL && memory.Completion() == NULL);
	uint64_t wrapper[16];
	for (auto range : {std::pair<uint64_t, uint32_t>{0, 0}, {0, 8},
		{UINT64_C(0x123456780), 64}, {kVmUserLimit - kMaxStreamBytes, kMaxStreamBytes}}) {
		assert(BuildQueueWrapper(wrapper, range.first, range.second));
		const unsigned withCall[] = {2,36,1,2,3,32,3,2,36,3,1,1,51,47,0,0};
		const unsigned withoutCall[] = {2,36,3,3,2,36,3,1,1,51,47,0,0,0,0,0};
		for (unsigned i = 0; i < 16; i++)
			assert((wrapper[i] >> 56) == (range.second ? withCall[i] : withoutCall[i]));
		if (range.second) {
			assert((wrapper[2] & UINT64_C(0xffffffffffff)) == range.first);
			assert(uint32_t(wrapper[3]) == range.second);
		}
	}
	for (auto range : {std::pair<uint64_t, uint32_t>{8, 0}, {1, 8}, {0, 7},
		{0, kMaxStreamBytes + 8}, {kVmUserLimit, 8}, {kVmUserLimit - 8, 16}, {UINT64_MAX - 7, 8}})
		assert(!BuildQueueWrapper(wrapper, range.first, range.second));
	assert(!BuildQueueWrapper(NULL, 0, 0));
}

// Deliberately independent physical inventory and page-table walker: interpret
// actual roots installed by the engine, including the borrowed user subtree.
struct PhysicalInventory {
	std::map<uint64_t, uint8_t*> pages;
	void Add(Guarded& memory, uint64_t physical)
	{
		for (size_t i = 0; i < memory.size; i += 4096)
			assert(pages.emplace(physical + i, memory.data + i).second);
	}
	uint8_t* At(uint64_t root, uint64_t address, unsigned bytes, bool write = false)
	{
		assert(address < (UINT64_C(1) << 48) && bytes <= 4096 - (address & 4095));
		for (unsigned level = 0; level < 4; level++) {
			uint64_t entry = ((uint64_t*)pages.at(root))[(address >> (39 - level * 9)) & 511];
			assert((entry & 3) == 3);
			root = entry & UINT64_C(0xfffffff000);
			if (level == 3) {
				assert(!write || ((entry >> 6) & 3) == 1);
				return pages.at(root) + (address & 4095);
			}
		}
		assert(false); return NULL;
	}
};

struct ApplicationMemory {
	Guarded arena{24576};
	uint64_t physical;
	explicit ApplicationMemory(uint64_t value) : physical(value)
	{
		memset(arena.data, 0, arena.size);
		((uint64_t*)arena.data)[0] = (physical + 4096) | 3;
		((uint64_t*)(arena.data + 4096))[4] = (physical + 8192) | 3;
		((uint64_t*)(arena.data + 8192))[0] = (physical + 12288) | 3;
		for (unsigned i = 0; i < 2; i++)
			((uint64_t*)(arena.data + 12288))[i] = (physical + 16384 + i * 4096)
				| (UINT64_C(3) << 53) | 0x643;
	}
	uint64_t* Root() { return (uint64_t*)arena.data; }
	uint64_t* Code() { return (uint64_t*)(arena.data + 16384); }
	uint32_t* Data() { return (uint32_t*)(arena.data + 20480); }
};

enum QueueFault { QueueNone, QueueLargeSuspend, QueueNoTimer, QueueConfigTimeout,
	QueueMapTimeout, QueueStartTimeout, QueueResumeTimeout, QueueSuspendTimeout,
	QueueDirtyFence, QueueMissingFence, QueueMissingIRQ, QueueMissingSync,
	QueueBadGuard, QueueStreamFault, QueueProgressFault, QueueTerminateTimeout,
	QueueHaltTimeout, QueueStopTimeout, QueueFlushTimeout, QueueUnmapTimeout,
	QueueBadExtract, QueueFrozenClock, QueueBadRoot };

struct QueueModel : FirmwareModel {
	PhysicalInventory& inventory;
	QueueFault queueFault;
	bool commandMode = false, pending = false, userMapped = false, userLocked = false;
	bool groupRunning = false, configured = false;
	unsigned groupIRQs = 0, executed = 0, pauses = 0, starts = 0, suspends = 0, resumes = 0;
	unsigned active = 0;
	uint64_t currentRoot = 0;
	std::array<uint32_t, 96> registers{};
	std::map<unsigned, std::array<uint32_t, 96>> saved;
	std::map<unsigned, unsigned> queueJobs;
	QueueModel(FirmwareMemory& memory, PhysicalInventory& physical, QueueFault fault)
		: FirmwareModel(&memory), inventory(physical), queueFault(fault) {}
	uint32_t TimerRate() { return queueFault == QueueNoTimer ? 0 : 24000000; }
	int64_t Now()
	{
		return commandMode && queueFault == QueueFrozenClock ? 200000 : FirmwareModel::Now();
	}
	bool InstallFirmwareHandlers()
	{
		bool result = FirmwareModel::InstallFirmwareHandlers();
		regs[0x18] = 0xffff; regs[0x100] = 0x50005; regs[0x110] = 1; regs[0x2470] = 1;
		return result;
	}
	bool ArmCommandJob()
	{
		assert(handlers && !jobArmed && mcuRunning);
		commandMode = true; captures[0] = {}; regs[0x1008] = 0x80000001; return true;
	}
	uint32_t CommandJobCount() { return groupIRQs; }
	void Event(bool group)
	{
		if (queueFault == QueueMissingIRQ && group && executed != 0) return;
		auto& event = captures[0]; event.count++;
		event.status |= group ? 1 : 0x80000000; event.raw = event.status;
		event.cpu = 3; event.whenMicros = Now();
		if (group) groupIRQs++;
	}
	uint32_t* Words() { return (uint32_t*)memory->SharedData(); }
	uint32_t* In(unsigned field) { return Words() + (Words()[field] - 0x4000000) / 4; }
	uint64_t* Queue() { return (uint64_t*)((uint8_t*)memory->WorkspaceData(0) + active * 8192); }
	void RingFirmwareDoorbell()
	{
		if (!commandMode && !(In(2)[0] & 1)) FirmwareModel::RingFirmwareDoorbell();
		else pending = true;
	}
	void WriteGpu(uint32_t offset, uint32_t value)
	{
		if (firmwareMode && offset >= 0x2440 && offset < 0x2480) {
			if (offset == 0x2458) {
				assert(l2On && (!groupRunning || !mcuRunning));
				regs[0x2000] |= 1u << 17;
				if (value == 2) userLocked = true;
				else if (value == 3) { assert(userLocked); userLocked = false; }
				else {
					assert(value == 1 && !userLocked);
					if (regs[0x2470] == 1) {
						assert(ReadGpu64(*this, 0x2440) == 0 && ReadGpu64(*this, 0x2448) == 0);
						if (queueFault == QueueUnmapTimeout) regs[0x2468] = 1;
						else userMapped = false;
					} else {
						assert(configured && !userMapped && !groupRunning);
						currentRoot = ReadGpu64(*this, 0x2440);
						assert(inventory.pages.count(currentRoot));
						userMapped = true;
						if (queueFault == QueueMapTimeout) regs[0x2468] = 1;
					}
				}
			}
			regs[offset] = value; return;
		}
		if (offset == kGpuCommand && userLocked) {
			assert(value == 0x3304 && l2On && (!groupRunning || !mcuRunning));
			if (queueFault != QueueFlushTimeout) regs[kGpuRaw] |= 1u << 17;
			return;
		}
		if (offset == 0x700 && value == 0 && queueFault == QueueStopTimeout) return;
		FirmwareModel::WriteGpu(offset, value);
	}
	void Interpret(uint64_t address, unsigned bytes, bool application)
	{
		unsigned flushes = 0;
		bool flushed = false;
		for (unsigned i = 0; i < bytes; i += 8) {
			uint64_t instruction = *(uint64_t*)inventory.At(currentRoot, address + i, 8);
			unsigned op = instruction >> 56, reg = (instruction >> 48) & 255;
			unsigned ar = (instruction >> 40) & 255, vr = (instruction >> 32) & 255;
			if (op == 1 || op == 2) {
				assert(reg < 96 && (application || reg >= 92));
				registers[reg] = uint32_t(instruction);
				if (op == 1) { assert(reg < 95); registers[reg + 1] = (instruction >> 32) & 65535; }
			} else if (op == 21) {
				assert(application && reg == 10 && ar == 0 && ((instruction >> 16) & 65535) == 1);
				uint64_t dest = registers[ar] | (uint64_t(registers[ar + 1]) << 32);
				*(uint32_t*)inventory.At(currentRoot, dest + (instruction & 65535), 4, true) = registers[reg];
			} else if (op == 32) {
				assert(!application && ar == 92 && vr == 94 && flushed && flushes == 1);
				Interpret(registers[ar] | (uint64_t(registers[ar + 1]) << 32), registers[vr], true);
				flushed = false;
			} else if (op == 36) {
				assert(!application && ar == 94 && registers[ar] == 0 && (instruction & 0xffff) == 0x233);
				flushes++; flushed = false;
			} else if (op == 3) {
				unsigned mask = (instruction >> 16) & 65535;
				assert(mask == 1 || mask == 255);
				if (!application && mask == 1) flushed = true;
			} else if (op == 51) {
				assert(!application && flushed && flushes == 2 && ar == 92 && vr == 94);
				assert(registers[vr] == 1 && registers[vr + 1] == 0 && (instruction & 65535) == 1);
				uint64_t dest = registers[ar] | (uint64_t(registers[ar + 1]) << 32);
				assert(dest == (UINT64_C(1) << 47) + 65536);
				uint64_t* completion = (uint64_t*)inventory.At(currentRoot, dest, 16, true);
				if (queueFault != QueueMissingFence && queueFault != QueueFrozenClock) (*completion)++;
				if (queueFault == QueueBadGuard) ((uint32_t*)completion)[3] = 1;
			} else assert(op == 0 || op == 23 || op == 47);
		}
	}
	void PauseReset()
	{
		if (!commandMode && !pending) {
			FirmwareModel::PauseReset();
			if (!pendingBoot && mcuRunning) {
				for (unsigned i = 0; i < 8; i++) {
					Words()[(0x1000 + i * 0xa0) / 4 + 3]
						= queueFault == QueueLargeSuspend ? 1048577 : 4096;
					Words()[(0x1000 + i * 0xa0) / 4 + 4] = 4096;
				}
			}
			return;
		}
		assert(++pauses < 300000); elapsed += 100;
		if (!pending) return;
		pending = false;
		uint32_t* gi = In(2); uint32_t* go = In(3);
		uint32_t* cgi = In(1025); uint32_t* cgo = In(1026);
		uint32_t* csi = In(1041); uint32_t* cso = In(1042);
		if (gi[0] & 1) {
			if (queueFault != QueueHaltTimeout) { regs[0x704] = 2; regs[0x140] = regs[0x150] = 0; }
			return;
		}
		if (((gi[0] ^ go[0]) & 14) != 0) {
			assert(!groupRunning && !userMapped && gi[4] == 2560000 && gi[5] == 235);
			assert(ReadInterface64(gi, 24) == 0x50005 && !(gi[0] & 0x400));
			if (queueFault == QueueConfigTimeout) return;
			go[0] = gi[0]; configured = true; Event(false);
		}
		if (((gi[2] ^ go[2]) & 1) == 0) return;
		go[2] = gi[2]; cgo[2] = cgi[2];
		unsigned request = cgi[0] & 7;
		if ((request == 1 || request == 3) && !groupRunning) {
			assert(userMapped && ReadInterface64(csi, 16) == (UINT64_C(1) << 47) && csi[6] == 65536);
			active = (ReadInterface64(csi, 48) - 0x4010000) / 8192;
			assert(active < kMaxRuntimeQueues && ReadInterface64(csi, 56) == 0x4011000 + active * 8192);
			assert(cgi[20] == 1 && ReadInterface64(cgi, 64) == 0x4100000 + active * kRuntimeSuspendBytes);
			assert(ReadInterface64(cgi, 72) == kRuntimeProtectedAddress + active * kRuntimeSuspendBytes);
			if (request == 1) {
				if (queueFault == QueueStartTimeout) return;
				assert(!saved.count(active)); registers = {}; starts++;
			} else {
				if (queueFault == QueueResumeTimeout) return;
				registers = saved.at(active); resumes++;
			}
			groupRunning = true; cgo[0] = cgi[0]; cso[0] = csi[0]; Event(true);
		} else if ((request == 0 || request == 2) && groupRunning) {
			if ((request == 0 && queueFault == QueueTerminateTimeout)
				|| (request == 2 && queueFault == QueueSuspendTimeout)) return;
			if (request == 2) { saved[active] = registers; suspends++; }
			else saved.erase(active);
			groupRunning = false; cgo[0] = cgi[0]; cso[0] &= ~7u; Event(true);
		}
		if (groupRunning && Queue()[0] > Queue()[512]) {
			uint64_t extract = Queue()[512];
			assert(Queue()[0] == extract + 128);
			Interpret((UINT64_C(1) << 47) + (extract & 65535), 128, false);
			executed++; queueJobs[active]++;
			Queue()[512] = Queue()[0] + (queueFault == QueueBadExtract ? 8 : 0);
			if (queueFault != QueueMissingSync) cgo[0] ^= 1u << 28;
			if (queueFault == QueueStreamFault) {
				cso[32] = 0x45; cso[0] ^= 1u << 31; cgo[3] ^= 1;
				WriteInterface64(cso, 136, 0x12345678);
			}
			if (queueFault == QueueProgressFault) cgo[0] ^= 1u << 31;
			Event(true);
		}
	}
};

struct TestQueue {
	QueueMemory memory;
	Guarded arena{memory.RequiredBytes()};
	ApplicationMemory first, second;
	QueueProgress progress{};
	unsigned submitted = 0, completed = 0, activations = 0;
	bool destroyed = false;
	TestQueue(PhysicalInventory& inventory, unsigned slot)
		: first(UINT64_C(0x280000000) + slot * 0x100000),
		second(UINT64_C(0x280010000) + slot * 0x100000)
	{
		uint64_t physical = UINT64_C(0x380000000) + slot * 0x100000;
		assert(memory.Build(arena.data, arena.size, physical));
		inventory.Add(arena, physical);
		inventory.Add(first.arena, first.physical); inventory.Add(second.arena, second.physical);
	}
};

struct TestFeed {
	TestQueue& a;
	TestQueue& b;
	QueueModel& io;
	std::vector<std::pair<unsigned, unsigned>> jobs{{0,0},{1,0},{0,0},{0,1},{1,0}};
	unsigned step = 0, complete = 0;
	bool ready = false;
	TestFeed(TestQueue& first, TestQueue& second, QueueModel& model)
		: a(first), b(second), io(model)
	{
		for (unsigned i = 0; i < 520; i++) jobs.push_back({0, 1});
		jobs.push_back({0, 2}); // ordered completion without application commands
	}
	void Ready(const InterfaceInfo& info, const GpuProperties& properties)
	{
		assert(info.version == 0x01050000 && io.configured && !ready);
		assert(properties.firmwareVersion == info.version && properties.workRegisters == 96
			&& properties.scoreboards == 8 && properties.reservedRegisters == 4
			&& properties.shaderPresent == 0x50005 && properties.tilerPresent == 1
			&& properties.firmwareTimerHz == io.TimerRate()); ready = true;
	}
	void Next(QueueWork& work)
	{
		assert(ready);
		unsigned current = step++;
		if (current >= jobs.size()) {
			if (current == jobs.size()) work = {kQueueWorkDestroy, 1, 0, 0, 0, 0, NULL, NULL, NULL};
			else if (current == jobs.size() + 1) work = {kQueueWorkDestroy, 0, 0, 0, 0, 0, NULL, NULL, NULL};
			else work.type = kQueueWorkStop;
			return;
		}
		unsigned slot = jobs[current].first, generation = jobs[current].second;
		TestQueue& queue = slot ? b : a;
		assert(queue.submitted == queue.completed);
		ApplicationMemory& memory = generation == 0 ? queue.first : queue.second;
		uint64_t* code = memory.Code();
		unsigned n = 0;
		code[n++] = UINT64_C(0x0300000000ff0000);
		code[n++] = UINT64_C(0x1700000000000002);
		if (queue.submitted == 0) code[n++] = UINT64_C(0x020a000012345678) + slot;
		code[n++] = UINT64_C(0x0100000100001000);
		code[n++] = UINT64_C(0x150a000000010004);
		code[n++] = UINT64_C(0x0300000000010000);
		if (generation != 2) {
			memory.Data()[0] = 0xdeadbeef; memory.Data()[1] = 0; memory.Data()[2] = 0x87654321;
		}
		queue.submitted++;
		work = {kQueueWorkSubmit, slot, generation == 2 ? 0 : UINT64_C(0x100000000),
			generation == 2 ? 0 : n * 8, queue.submitted, generation == 0 ? 1u : 2u,
			memory.Root(), &queue.memory, &memory};
	}
	HeapGrowthResult PrepareHeap(unsigned, uint64_t, uint32_t, uint32_t, uint32_t, HeapGrowth&)
	{ assert(false); return kHeapGrowthInvalid; }
	bool CommitHeap(HeapGrowth&) { assert(false); return false; }
	void AbortHeap(HeapGrowth&) { assert(false); }
	void Activated(const QueueWork& work)
	{
		assert(io.userMapped && !io.groupRunning && io.currentRoot == work.memory->RootPhysical());
		assert(memcmp(work.memory->Root(), work.root, 2048) == 0);
		(work.slot ? b : a).activations++;
	}
	void Progress(unsigned slot, const QueueProgress& progress) { (slot ? b : a).progress = progress; }
	void Complete(const QueueWork& work, const QueueProgress& progress)
	{
		TestQueue& queue = work.slot ? b : a;
		queue.completed++; complete++; queue.progress = progress;
		assert(queue.completed == work.sequence && progress.completed == work.sequence);
		assert(progress.insert == work.sequence * 128 && progress.extract == progress.insert);
		assert(progress.interrupts >= work.sequence && progress.syncEvents == work.sequence);
		assert(progress.generation == work.generation);
		ApplicationMemory& memory = *(ApplicationMemory*)work.cookie;
		assert(memory.Data()[0] == 0xdeadbeef && memory.Data()[1] == 0x12345678 + work.slot
			&& memory.Data()[2] == 0x87654321);
	}
	void Destroyed(unsigned slot)
	{
		assert(!io.groupRunning || io.active != slot);
		assert(!io.userMapped || io.currentRoot != (slot ? b : a).memory.RootPhysical());
		(slot ? b : a).destroyed = true;
	}
};

#include "test_mali_heap_engine.inc"

#ifndef MALI_QUEUE_MODEL_ONLY
int main()
{
	setvbuf(stdout, NULL, _IONBF, 0);
	PrivateMemory();
	CheckHeapEngine();
	auto bytes = container({{0x800000, 4096, 13}, {0x4000000, 65536, 0xc000001b}});
	FirmwareImage image;
	assert(image.Init(bytes.data(), bytes.size()) == FIRMWARE_OK);
	FirmwareMemory memory;
	assert(PlanRuntimeFirmware(memory, image));
	Guarded arena(memory.RequiredBytes());
	for (unsigned fault = QueueNone; fault <= QueueBadRoot; fault++) {
		assert(memory.Build(arena.data, arena.size, UINT64_C(0x182300000)));
		PhysicalInventory inventory;
		TestQueue a(inventory, 0), b(inventory, 1);
		if (fault == QueueDirtyFence) *(uint64_t*)a.memory.Completion() = 1;
		if (fault == QueueBadRoot) a.first.Root()[0] |= 16;
		QueueModel model(memory, inventory, QueueFault(fault));
		TestFeed feed(a, b, model);
		QueueEngine<TestFeed> engine(feed);
		FirmwareRunInfo info{}; info.flags = kFirmwareAllocated;
		CycleFirmware(model, memory, info, engine);
		printf("queues fault=%u engine=%u firmware=%u/%u/%x jobs=%u starts=%u suspends=%u resumes=%u\n",
			fault, engine.Error(), info.result, info.cleanupResult, info.flags,
			feed.complete, model.starts, model.suspends, model.resumes);
		assert(!model.handlers && model.gpu == NULL);
		if (fault == QueueNone) {
			assert(info.flags == 255 && info.result == kFirmwareRunOK && info.cleanupResult == kFirmwareRunOK);
			assert(engine.Error() == kQueueEngineOK && !engine.Mapped());
			assert(a.completed == 524 && b.completed == 2 && feed.complete == 526 && model.executed == 526);
			assert(a.progress.insert > 65536 && a.progress.generation == 2);
			assert(a.activations == 4 && b.activations == 2 && model.starts == 2 && model.resumes == 4);
			assert(model.suspends == 5 && a.destroyed && b.destroyed);
			assert(!model.userMapped && !model.mcuRunning && !model.l2On && !model.groupRunning);
		} else {
			assert((info.flags & kFirmwareNeedsRecovery) != 0);
			assert(info.result != kFirmwareRunOK || info.cleanupResult != kFirmwareRunOK);
			if (fault != QueueStopTimeout) assert(engine.Error() != kQueueEngineOK);
		}
		if (fault == QueueStreamFault)
			assert(engine.StreamFault() == 0x45 && engine.FaultAddress() == 0x12345678);
		if (fault == QueueMissingFence || fault == QueueMissingIRQ || fault == QueueMissingSync
			|| fault == QueueBadGuard || fault == QueueBadExtract) assert(feed.complete == 0);
		if (fault == QueueFlushTimeout || fault == QueueUnmapTimeout || fault == QueueMapTimeout)
			assert(engine.Mapped());
	}
	puts("MALI_CSF_QUEUES_TEST_PASS");
}
#endif
