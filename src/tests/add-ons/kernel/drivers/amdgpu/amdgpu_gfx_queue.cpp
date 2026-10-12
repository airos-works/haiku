/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include <amdgpu_haiku.h>
#include <OS.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>
#include "GfxCommandValidator.h"

static const uint64 kVA = 0x300000000ULL;
static const uint32 kBytes = 131072, kGuard = 0x6b6b6b6b;
static volatile uint32* sGate;
using Words = std::vector<uint32>;
struct Client { int fd; amdgpu_buffer output; uint32 id; uint64 fence; };
static void ReleaseGate()
{
	if (sGate != NULL) { *sGate = 1; __sync_synchronize(); sGate = NULL; }
}
static void Require(bool okay, const char* why)
{
	if (!okay) {
		// Never leave the GPU waiting on our own gate after a harness failure.
		ReleaseGate(); fprintf(stderr, "FAIL: %s (%d: %s)\n", why, errno, strerror(errno)); exit(1);
	}
}
template<typename T> static T Request()
{
	T r = {}; r.version = AMDGPU_HAIKU_ABI_VERSION; r.size = sizeof(r); return r;
}
static int Open()
{
	int fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR); Require(fd >= 0, "open GPU client"); return fd;
}
static amdgpu_memory_info Memory(int fd)
{
	auto r = Request<amdgpu_memory_info>();
	Require(ioctl(fd, AMDGPU_MEMORY_INFO, &r, sizeof(r)) == 0, "read queue/memory state"); return r;
}
static amdgpu_gart_info Gart(int fd)
{
	auto r = Request<amdgpu_gart_info>();
	Require(ioctl(fd, AMDGPU_GART_INFO, &r, sizeof(r)) == 0, "read RAM accounting"); return r;
}
static amdgpu_irq_info IRQ(int fd)
{
	auto r = Request<amdgpu_irq_info>();
	Require(ioctl(fd, AMDGPU_IRQ_INFO, &r, sizeof(r)) == 0 && r.enabled && r.msi
		&& r.status == B_OK && !r.vm_faults && !r.privileged_faults && !r.unknown
		&& !r.overflows && r.rptr == r.wptr && r.waits == r.completed_fences,
		"healthy independent IRQ counters"); return r;
}
static Client Create(uint32 id)
{
	Client c = {}; c.fd = Open(); c.id = id;
	auto vm = Request<amdgpu_vm_info>();
	Require(ioctl(c.fd, AMDGPU_VM_INFO, &vm, sizeof(vm)) == 0, "create VM");
	c.output = Request<amdgpu_buffer>(); c.output.bytes = kBytes;
	Require(ioctl(c.fd, AMDGPU_CREATE_SYSTEM_BUFFER, &c.output, sizeof(c.output)) == 0
		&& ioctl(c.fd, AMDGPU_MAP_BUFFER, &c.output, sizeof(c.output)) == 0, "create and map owned output");
	auto m = Request<amdgpu_vm_mapping>(); m.handle = c.output.handle;
	m.address = kVA; m.bytes = kBytes; m.permissions = AMDGPU_VM_READ | AMDGPU_VM_WRITE;
	Require(ioctl(c.fd, AMDGPU_VM_MAP, &m, sizeof(m)) == 0, "map owned GPU output");
	auto p = (volatile uint32*)(addr_t)c.output.address;
	for (uint32 i = 0; i < kBytes / 4; i++) {
		Require(p[i] == 0, "new/reused output is zero"); p[i] = i == 0 ? 0 : kGuard;
	}
	__sync_synchronize(); return c;
}
static void Close(Client& c)
{
	Require(close(c.fd) == 0 && delete_area(c.output.area) == B_OK, "close and delete revoked clone");
	c.fd = -1;
}
static uint32 Value(const Client& c, unsigned slot) { return 0x51000000 | c.id << 12 | slot; }
static Words Commands(const Client& c, unsigned slot, bool gate = false, bool large = false, bool copy = false)
{
	Words w;
	if (gate) w = {0xc0053c00, 0x13, uint32(kVA), uint32(kVA >> 32), 1, 0xffffffff, 4};
	if (copy) w.insert(w.end(), {0xc0055000, 0x80000000, uint32(kVA + 16384), uint32(kVA >> 32),
		uint32(kVA + 20480), uint32(kVA >> 32), 0x40001000});
	uint64 address = kVA + 8192 + slot * 64;
	w.insert(w.end(), {0xc0063700, 0x100500, uint32(address), uint32(address >> 32),
		Value(c, slot), ~Value(c, slot), slot, 0x51005100});
	w.resize(large ? 16384 : 256, 0x80000000);
	Require(amdgpu::ValidateGfx8Commands(w.data(), w.size()).error == amdgpu::GfxCommandValidation::OK,
		"queue probe commands pass kernel policy"); return w;
}
static void Enqueue(Client& c, Words words, int error = 0, bool checked = true)
{
	auto r = Request<amdgpu_gfx_queue>(); r.address = (addr_t)words.data(); r.dwords = words.size();
	r.flags = checked ? AMDGPU_GFX_VALIDATE_COMMANDS : 0;
	int result = ioctl(c.fd, AMDGPU_GFX_QUEUE_COPY, &r, sizeof(r));
	if (error) Require(result == -1 && errno == error && r.fence == 0, "rejected enqueue publishes no fence");
	else {
		Require(result == 0 && r.fence == c.fence + 1, "enqueue returns next ordered file fence");
		c.fence = r.fence;
	}
	// Replace and immediately destroy the source. Each queued job must have
	// already captured its own complete command stream, including its tail.
	memset(words.data(), 0xd3, words.size() * 4); __sync_synchronize();
}
static void Wait(Client& c, bool poll = false)
{
	auto r = Request<amdgpu_fence_wait>(); r.fence = c.fence; r.timeout_us = poll ? 0 : 5000000;
	int result = ioctl(c.fd, AMDGPU_WAIT_FENCE, &r, sizeof(r));
	if (poll) Require(result == -1 && errno == B_WOULD_BLOCK, "pending GPU fence cannot report completion");
	else Require(result == 0 && r.status == B_OK, "GPU fence retires successfully");
}
static void Pixels(const Client& c, unsigned slots, bool released = false, bool dma = false)
{
	auto p = (volatile uint32*)(addr_t)c.output.address;
	for (uint32 i = 0; i < kBytes / 4; i++) {
		uint32 expected = i == 0 ? uint32(released) : kGuard;
		if (i >= 2048 && i < 2048 + slots * 16 && i % 16 < 4) {
			unsigned slot = (i - 2048) / 16;
			const uint32 values[] = {Value(c, slot), ~Value(c, slot), slot, 0x51005100};
			expected = values[i % 16];
		}
		if (dma && i >= 4096 && i < 7168) expected = 0xa5c35100;
		if (p[i] != expected) {
			fprintf(stderr, "VM %u word %u: %#x expected %#x\n", c.id, i, (unsigned)p[i], expected);
			Require(false, "exact queue output and all peer/neighbor guards");
		}
	}
}
static void Frozen(Client& c)
{
	auto m = Request<amdgpu_vm_mapping>();
	Require(ioctl(c.fd, AMDGPU_VM_UNMAP, &m, sizeof(m)) == -1 && errno == B_BUSY,
		"queued VM cannot mutate its mappings");
	auto s = Request<amdgpu_gfx_submit>();
	Require(ioctl(c.fd, AMDGPU_GFX_SUBMIT_COPY, &s, sizeof(s)) == -1 && errno == B_BUSY,
		"synchronous copy cannot replace a queued command allocation");
}
static void Drain(int monitor)
{
	bigtime_t end = system_time() + 5000000;
	while (Memory(monitor).pending_jobs) {
		Require(system_time() < end, "observe queued close retirement"); snooze(100);
	}
}

static void DmaWait(int fd, uint64 fence, status_t expected)
{
	auto r = Request<amdgpu_fence_wait>(); r.fence = fence; r.timeout_us = 5000000;
	Require(ioctl(fd, AMDGPU_WAIT_FENCE, &r, sizeof(r)) == 0 && r.status == expected,
		"retained DMA fence has its original terminal result");
}
static void DmaFill(int fd, const amdgpu_buffer& b, uint64 fence, uint32 value)
{
	auto r = Request<amdgpu_dma_submit>(); r.operation = AMDGPU_DMA_FILL;
	r.destination = b.handle; r.destination_offset = 1024; r.bytes = 4; r.value = value;
	Require(ioctl(fd, AMDGPU_SUBMIT_DMA, &r, sizeof(r)) == 0 && r.fence == fence,
		"owned DMA fill receives the next file fence");
}
static amdgpu_buffer DmaBuffer(int fd)
{
	auto b = Request<amdgpu_buffer>(); b.bytes = 4096;
	Require(ioctl(fd, AMDGPU_CREATE_SYSTEM_BUFFER, &b, sizeof(b)) == 0
		&& ioctl(fd, AMDGPU_MAP_BUFFER, &b, sizeof(b)) == 0, "create CPU-visible DMA buffer");
	auto p = (volatile uint32*)(addr_t)b.address;
	for (unsigned i = 0; i < 1024; i++) {
		Require(p[i] == 0, "fresh DMA allocation is zero"); p[i] = kGuard;
	}
	__sync_synchronize(); return b;
}
static void DmaPixels(const amdgpu_buffer& b, uint32 value)
{
	for (unsigned i = 0; i < 1024; i++)
		Require(((volatile uint32*)(addr_t)b.address)[i] == (i == 256 ? value : kGuard),
			"DMA target and every guard remain exact; canceled work never executes");
}
static void DmaFree(int fd, amdgpu_buffer& b)
{
	Require(ioctl(fd, AMDGPU_FREE_BUFFER, &b, sizeof(b)) == 0
		&& delete_area(b.area) == B_OK, "free DMA handle and revoked clone, retain fence fd");
}
static amdgpu_recovery_info Recovery(int fd, uint32 op)
{
	auto r = Request<amdgpu_recovery_info>();
	Require(ioctl(fd, op, &r, sizeof(r)) == 0, "recovery query/control request"); return r;
}

static_assert(sizeof(amdgpu_client_status) == 64, "client status ABI");
static amdgpu_client_status ClientStatus(int fd)
{
	auto r = Request<amdgpu_client_status>();
	Require(ioctl(fd, AMDGPU_CLIENT_STATUS, &r, sizeof(r)) == 0, "read persistent owning-client status");
	return r;
}
static void Lost(int fd, uint64 generation, bool recovered, uint64 submitted, uint64 failed)
{
	auto r = ClientStatus(fd);
	Require(r.flags == uint32(AMDGPU_CLIENT_LOST | (recovered ? AMDGPU_CLIENT_RESET_COMPLETED : 0))
		&& r.status == B_BAD_DATA && r.device_status == (recovered ? B_OK : B_BAD_DATA)
		&& r.generation == generation + uint64(recovered) && r.loss_generation == generation
		&& r.submitted == submitted && r.completed == submitted && r.failed_fence == failed,
		"client loss, recovery completion and original fence timeline persist independently");
	auto b = Request<amdgpu_buffer>(); b.bytes = 4096;
	Require(ioctl(fd, AMDGPU_CREATE_SYSTEM_BUFFER, &b, sizeof(b)) < 0
		&& errno == B_DEV_NOT_READY && !b.handle, "every lost client rejects new allocation");
	auto vm = Request<amdgpu_vm_info>();
	Require(ioctl(fd, AMDGPU_VM_INFO, &vm, sizeof(vm)) < 0 && errno == B_DEV_NOT_READY,
		"every lost client rejects VM creation/mutation");
}
static void VramPixels(const amdgpu_buffer& b)
{
	for (unsigned i = 0; i < 1024; i++)
		Require(((volatile uint32*)(addr_t)b.address)[i] == (kGuard ^ i),
			"live VRAM allocation retains every CPU-visible word across reset and fresh work");
}
static void ClientStatusABI(int fd)
{
	for (unsigned i = 0; i < 5; i++) {
		auto r = Request<amdgpu_client_status>(); size_t bytes = sizeof(r);
		if (i == 0) r.version++;
		if (i == 1) r.size--;
		if (i == 2) bytes--;
		if (i == 3) r.flags = 1;
		if (i == 4) r.reserved = 1;
		Require(ioctl(fd, AMDGPU_CLIENT_STATUS, &r, bytes) < 0 && errno == B_BAD_VALUE,
			"client status validates ABI, flags and reserved input");
	}
	int ro = open("/dev/" AMDGPU_DEVICE_NAME, O_RDONLY);
	auto r = Request<amdgpu_client_status>();
	Require(ro >= 0 && ioctl(ro, AMDGPU_CLIENT_STATUS, &r, sizeof(r)) < 0
		&& errno == B_NOT_ALLOWED, "read-only status file denied"); close(ro);
}
static void LiveRecovery(int monitor, Client& gate, Client& bad, Client& idle,
	amdgpu_buffer& vram, int dma, amdgpu_buffer& dmaBuffer,
	const amdgpu_memory_info& baselineMemory, const amdgpu_gart_info& baselineRam,
	const amdgpu_memory_info& memory, const amdgpu_gart_info& ram)
{
	auto q = Recovery(monitor, AMDGPU_RECOVERY_INFO);
	uint64 generation = q.generation;
	int late = Open(); // Even a resource-free file opened during the fault stays lost.
	Lost(gate.fd, generation, false, 8, 2); Lost(bad.fd, generation, false, 16, 1);
	Lost(dma, generation, false, 3, 2); Lost(idle.fd, generation, false, 0, 0);
	Lost(late, generation, false, 0, 0); Lost(monitor, generation, false, 0, 0);
	ClientStatusABI(idle.fd);
	Pixels(idle, 1); VramPixels(vram);
	// Mix one closed quarantined owner with three retained resource owners.
	Close(gate);
	q = Recovery(monitor, AMDGPU_RECOVERY_INFO);
	Require(q.buffers == 1 && q.vms == 1 && q.live_handles == 4 && q.live_vms == 2
		&& !q.live_videos && !q.pending && !q.videos, "one closed VM, live faulting/idle/DMA clients");
	Require(Recovery(monitor, AMDGPU_GFX_RECOVER).status == B_BUSY,
		"original closed-resource recovery contract remains strict");
	printf("Live terminal: generation %llu, live handles/VMs %u/%u, quarantine %llu/%llu; one explicit reset\n",
		(unsigned long long)generation, q.live_handles, q.live_vms,
		(unsigned long long)q.quarantined_vram, (unsigned long long)q.quarantined_ram);
	auto reset = Recovery(monitor, AMDGPU_GFX_RECOVER_LIVE);
	printf("Live recovery status %#x stage %u generation %llu reclaimed %llu/%llu\n",
		(unsigned)reset.status, reset.stage, (unsigned long long)reset.generation,
		(unsigned long long)reset.reclaimed_vram, (unsigned long long)reset.reclaimed_ram);
	Require(reset.status == B_OK && reset.stage == 7 && reset.generation == generation + 1
		&& reset.reclaimed_vram == q.quarantined_vram && reset.reclaimed_ram == q.quarantined_ram
		&& !reset.buffers && !reset.vms && !reset.quarantined_vram && !reset.quarantined_ram
		&& reset.live_handles == 4 && reset.live_vms == 2 && IRQ(monitor).completed_fences == 1,
		"one live-client reset proves hardware, reclaims only closed quarantine, retains live ownership");
	Require(Memory(monitor).allocated_bytes == memory.allocated_bytes - q.quarantined_vram
		&& Gart(monitor).allocated_bytes == ram.allocated_bytes - q.quarantined_ram,
		"live allocations remain charged to independent allocators");
	for (unsigned round = 0; round < 2; round++) {
		Lost(bad.fd, generation, true, 16, 1); Lost(dma, generation, true, 3, 2);
		Lost(idle.fd, generation, true, 0, 0); Lost(late, generation, true, 0, 0);
		Lost(monitor, generation, true, 0, 0);
		for (uint64 fence = 1; fence <= 16; fence++) DmaWait(bad.fd, fence, B_BAD_DATA);
		DmaWait(dma, 1, B_OK); DmaWait(dma, 2, B_BAD_DATA); DmaWait(dma, 3, B_BAD_DATA);
		Enqueue(bad, Commands(bad, 0), B_DEV_NOT_READY);
		Enqueue(idle, Commands(idle, 1), B_DEV_NOT_READY);
		auto words = Commands(idle, 1); auto sync = Request<amdgpu_gfx_submit>();
		sync.address = (addr_t)words.data(); sync.dwords = words.size(); sync.flags = AMDGPU_GFX_VALIDATE_COMMANDS;
		Require(ioctl(idle.fd, AMDGPU_GFX_SUBMIT_COPY, &sync, sizeof(sync)) < 0
			&& errno == B_DEV_NOT_READY, "successful synchronous-only client stays lost after recovery");
		Pixels(bad, 0); Pixels(idle, 1); VramPixels(vram); DmaPixels(dmaBuffer, 0x51510001);
		if (round == 0) {
			Client fresh = Create(33); auto healthy = ClientStatus(fresh.fd);
			Require(!healthy.flags && healthy.status == B_OK && healthy.device_status == B_OK
				&& healthy.generation == generation + 1, "fresh client starts healthy in recovered generation");
			Enqueue(fresh, Commands(fresh, 0)); Wait(fresh); Pixels(fresh, 1); Close(fresh);
			int nextDma = Open(); auto nextBuffer = DmaBuffer(nextDma);
			DmaFill(nextDma, nextBuffer, 1, 0x5151abcd); DmaWait(nextDma, 1, B_OK);
			DmaPixels(nextBuffer, 0x5151abcd); DmaFree(nextDma, nextBuffer); close(nextDma);
		}
	}
	Require(Recovery(monitor, AMDGPU_GFX_RECOVER_LIVE).status == B_NOT_ALLOWED,
		"healthy live device does not reset again");
	Close(bad); Close(idle); Require(delete_area(vram.area) == B_OK, "delete revoked VRAM clone");
	DmaFree(dma, dmaBuffer); close(dma); close(late);
	q = Recovery(monitor, AMDGPU_RECOVERY_INFO);
	Require(!q.live_handles && !q.live_vms && !q.buffers && !q.vms && !q.quarantined_vram && !q.quarantined_ram
		&& Memory(monitor).allocated_bytes == baselineMemory.allocated_bytes
		&& Gart(monitor).allocated_bytes == baselineRam.allocated_bytes
		&& IRQ(monitor).completed_fences == 2, "old live owner cleanup fully reclaims original allocations");
	puts("PASS: retained live graphics/DMA/idle clients, persistent loss and recovery status, unchanged old fences,"
		" all RAM/VRAM guards, fresh isolated work, exact closed/live reclamation");
}

// Deliberately causes a terminal GPU VM write fault. Run only on an idle
// qualification boot with a verified recovery path, never as a smoke test.
// --fault-recovery explicitly requests ONE reset after all expected terminal
// fault/quarantine checks pass. It never retries a failure or observation timeout.
static void FaultQualification(int monitor, bool recover, bool live = false)
{
	auto before = IRQ(monitor);
	auto baselineMemory = Memory(monitor); auto baselineRam = Gart(monitor);
	int dma = recover ? Open() : -1;
	amdgpu_buffer dmaBuffer = {};
	if (recover) {
		dmaBuffer = DmaBuffer(dma);
		DmaFill(dma, dmaBuffer, 1, 0x51510001); DmaWait(dma, 1, B_OK);
		DmaPixels(dmaBuffer, 0x51510001);
	}
	Client idle = {}; amdgpu_buffer vram = {};
	if (live) {
		idle = Create(29); auto words = Commands(idle, 0);
		auto sync = Request<amdgpu_gfx_submit>(); sync.address = (addr_t)words.data();
		sync.dwords = words.size(); sync.flags = AMDGPU_GFX_VALIDATE_COMMANDS;
		Require(ioctl(idle.fd, AMDGPU_GFX_SUBMIT_COPY, &sync, sizeof(sync)) == 0
			&& sync.status == B_OK && sync.completion, "innocent synchronous-only client completed before fault");
		Pixels(idle, 1);
		vram = Request<amdgpu_buffer>(); vram.bytes = 4096;
		Require(ioctl(idle.fd, AMDGPU_CREATE_BUFFER, &vram, sizeof(vram)) == 0
			&& ioctl(idle.fd, AMDGPU_MAP_BUFFER, &vram, sizeof(vram)) == 0, "retain CPU-visible VRAM owner");
		for (unsigned i = 0; i < 1024; i++) {
			Require(((volatile uint32*)(addr_t)vram.address)[i] == 0, "fresh retained VRAM is zero");
			((volatile uint32*)(addr_t)vram.address)[i] = kGuard ^ i;
		}
		__sync_synchronize();
		before = IRQ(monitor);
	}
	Client gate = Create(30), bad = Create(31);
	const uint64 readOnly = 0x400000000ULL;
	auto mapping = Request<amdgpu_vm_mapping>(); mapping.handle = bad.output.handle;
	mapping.address = readOnly; mapping.bytes = 4096; mapping.buffer_offset = 65536;
	mapping.permissions = AMDGPU_VM_READ;
	Require(ioctl(bad.fd, AMDGPU_VM_MAP, &mapping, sizeof(mapping)) == 0, "map owned read-only fault target");
	sGate = (volatile uint32*)(addr_t)gate.output.address;
	Enqueue(gate, Commands(gate, 0, true));
	Words fault = {0xc0033700, 0x100500, uint32(readOnly), uint32(readOnly >> 32), 0xdead5100};
	fault.resize(256, 0x80000000);
	Enqueue(bad, fault);
	if (recover) {
		DmaFill(dma, dmaBuffer, 2, 0xdead5102);
		DmaFill(dma, dmaBuffer, 3, 0xdead5103);
	}
	for (unsigned n = 1; n < 16; n++) Enqueue(bad, Commands(bad, n));
	for (unsigned n = 1; n < 8; n++) Enqueue(gate, Commands(gate, n));
	Wait(gate, true); Wait(bad, true);
	Require(Memory(monitor).pending_jobs == (recover ? 26u : 24u),
		"all fault and later jobs are queued behind the gate");
	auto memory = Memory(monitor); auto ram = Gart(monitor);
	ReleaseGate();
	for (Client* c : {&bad, &gate}) {
		for (uint64 fence = c == &bad ? 1 : 2; fence <= c->fence; fence++) {
			auto wait = Request<amdgpu_fence_wait>(); wait.fence = fence; wait.timeout_us = 5000000;
			Require(ioctl(c->fd, AMDGPU_WAIT_FENCE, &wait, sizeof(wait)) == 0
				&& wait.status == B_BAD_DATA, "terminal GPU fault fails every outstanding client fence");
		}
	}
	Drain(monitor);
	if (recover) {
		DmaWait(dma, 1, B_OK); DmaWait(dma, 2, B_BAD_DATA); DmaWait(dma, 3, B_BAD_DATA);
		DmaPixels(dmaBuffer, 0x51510001);
		Require(Recovery(monitor, AMDGPU_GFX_RECOVER).status == B_BUSY,
			"live faulted graphics/DMA resources exclude recovery");
	}
	auto first = Request<amdgpu_fence_wait>(); first.fence = 1;
	Require(ioctl(gate.fd, AMDGPU_WAIT_FENCE, &first, sizeof(first)) == 0 && first.status == B_OK,
		"earlier successfully retired fence remains successful after a later fault");
	Pixels(gate, 1, true); Pixels(bad, 0);
	auto irq = Request<amdgpu_irq_info>();
	Require(ioctl(monitor, AMDGPU_IRQ_INFO, &irq, sizeof(irq)) == 0, "read independent fault counters");
	printf("queued fault IRQ status %#x, completed %llu -> %llu, waits %llu -> %llu, VM %llu\n",
		(unsigned)irq.status, (unsigned long long)before.completed_fences,
		(unsigned long long)irq.completed_fences, (unsigned long long)before.waits,
		(unsigned long long)irq.waits, (unsigned long long)irq.vm_faults);
	// The trusted postlude may retire even though a VM fault invalidates the
	// job. Completion observation is not success; WAIT_FENCE must still fail.
	Require(irq.status == B_BAD_DATA && irq.vm_faults > before.vm_faults
		&& !irq.privileged_faults && !irq.unknown && !irq.overflows
		&& irq.waits == before.waits + 2
		&& irq.completed_fences >= before.completed_fences + 1
		&& irq.completed_fences <= before.completed_fences + 2,
		"independent VM fault; only the gate and faulting job reached hardware");
	Enqueue(gate, Commands(gate, 8), B_DEV_NOT_READY);
	if (live) {
		LiveRecovery(monitor, gate, bad, idle, vram, dma, dmaBuffer,
			baselineMemory, baselineRam, memory, ram);
		return;
	}
	Close(gate); Close(bad);
	if (recover) DmaFree(dma, dmaBuffer);
	Require(Memory(monitor).faulted && !Memory(monitor).pending_jobs
		&& Memory(monitor).allocated_bytes == memory.allocated_bytes
		&& Gart(monitor).allocated_bytes == ram.allocated_bytes,
		"all pending jobs drained while GPU-visible storage stays quarantined after close");
	printf("PASS: queued VM fault, 23 failed/canceled fences, prior fence preserved,"
		" all victim/peer data unchanged and close quarantines memory; completion %llu -> %llu, VM faults %llu\n",
		(unsigned long long)before.completed_fences, (unsigned long long)irq.completed_fences,
		(unsigned long long)irq.vm_faults);
	if (!recover) return;
	auto q = Recovery(monitor, AMDGPU_RECOVERY_INFO);
	Require(q.status == B_BAD_DATA && q.buffers == 3 && q.vms == 2 && !q.videos
		&& !q.live_handles && !q.live_vms && !q.live_videos && !q.pending,
		"all graphics/DMA work terminal; three BOs/two VMs retained with no live resources");
	Require(baselineMemory.allocated_bytes + q.quarantined_vram == memory.allocated_bytes
		&& baselineRam.allocated_bytes + q.quarantined_ram == ram.allocated_bytes,
		"independent allocators account for complete mixed-queue quarantine");
	printf("Mixed queue terminal: 25 failed/canceled fences, prior graphics/DMA fences preserved;"
		" quarantine VRAM/RAM %llu/%llu; one explicit recovery requested\n",
		(unsigned long long)q.quarantined_vram, (unsigned long long)q.quarantined_ram);
	auto reset = Recovery(monitor, AMDGPU_GFX_RECOVER);
	printf("Recovery status %#x stage %u generation %llu -> %llu reclaimed %llu/%llu\n",
		(unsigned)reset.status, reset.stage, (unsigned long long)q.generation,
		(unsigned long long)reset.generation, (unsigned long long)reset.reclaimed_vram,
		(unsigned long long)reset.reclaimed_ram);
	Require(reset.status == B_OK && reset.stage == 7 && reset.generation == q.generation + 1
		&& !reset.buffers && !reset.vms && !reset.videos
		&& !reset.quarantined_vram && !reset.quarantined_ram
		&& reset.reclaimed_vram == q.quarantined_vram && reset.reclaimed_ram == q.quarantined_ram,
		"one reset succeeds and reclaims exact quarantine; no retry");
	Require(Memory(monitor).allocated_bytes == baselineMemory.allocated_bytes
		&& Gart(monitor).allocated_bytes == baselineRam.allocated_bytes
		&& IRQ(monitor).completed_fences == 1, "private reset proof and allocator baselines");
	DmaWait(dma, 1, B_OK); DmaWait(dma, 2, B_BAD_DATA); DmaWait(dma, 3, B_BAD_DATA);
	auto allocation = Request<amdgpu_buffer>(); allocation.bytes = 4096;
	Require(ioctl(dma, AMDGPU_CREATE_SYSTEM_BUFFER, &allocation, sizeof(allocation)) < 0
		&& errno == B_DEV_NOT_READY, "failed queue cannot allocate after recovery");
	auto vm = Request<amdgpu_vm_info>();
	Require(ioctl(dma, AMDGPU_VM_INFO, &vm, sizeof(vm)) < 0 && errno == B_DEV_NOT_READY,
		"failed queue cannot create a new graphics VM after recovery");
	auto video = Request<amdgpu_video_create>(); video.config = {864, 480, 100, 30, 2, 0};
	Require(ioctl(dma, AMDGPU_VIDEO_CREATE, &video, sizeof(video)) < 0 && errno == B_DEV_NOT_READY,
		"failed queue cannot create a new video session after recovery");
	int fresh = Open(); auto freshBuffer = DmaBuffer(fresh);
	auto refused = Request<amdgpu_dma_submit>(); refused.operation = AMDGPU_DMA_FILL;
	refused.destination = freshBuffer.handle; refused.bytes = 4;
	Require(ioctl(dma, AMDGPU_SUBMIT_DMA, &refused, sizeof(refused)) < 0
		&& errno == B_DEV_NOT_READY && refused.fence == 0, "failed queue cannot publish new work/fence");
	DmaFill(fresh, freshBuffer, 1, 0x5151abcd); DmaWait(fresh, 1, B_OK);
	DmaPixels(freshBuffer, 0x5151abcd); DmaFree(fresh, freshBuffer); close(fresh);
	Client next = Create(32); Enqueue(next, Commands(next, 0)); Wait(next); Pixels(next, 1); Close(next);
	DmaWait(dma, 1, B_OK); DmaWait(dma, 2, B_BAD_DATA); DmaWait(dma, 3, B_BAD_DATA);
	Require(Memory(dma).submitted == 3 && Memory(dma).completed == 3,
		"old file fence timeline is unchanged by recovery and fresh clients");
	close(dma);
	Require(Memory(monitor).allocated_bytes == baselineMemory.allocated_bytes
		&& Gart(monitor).allocated_bytes == baselineRam.allocated_bytes
		&& IRQ(monitor).completed_fences == 2, "fresh graphics/DMA work and full reclamation");
	puts("PASS: mixed queued fault recovery, retained successful/failed fences, lost file rejects new work,"
		" fresh zeroed graphics/DMA reuse and exact reclamation");
}

static void AutomaticQualification(int monitor)
{
	auto policy = Request<amdgpu_recovery_policy>();
	Require(ioctl(monitor, AMDGPU_RECOVERY_POLICY, &policy, sizeof(policy)) == 0
		&& policy.enabled && !policy.pending, "default automatic recovery enabled");
	auto prior = Recovery(monitor, AMDGPU_RECOVERY_INFO);
	auto baselineMemory = Memory(monitor); auto baselineRam = Gart(monitor);
	Client idle = Create(29), gate = Create(30), bad = Create(31);
	int dma = Open(); auto dmaBuffer = DmaBuffer(dma);
	DmaFill(dma, dmaBuffer, 1, 0x51510001); DmaWait(dma, 1, B_OK);
	const uint64 readOnly = 0x400000000ULL;
	auto m = Request<amdgpu_vm_mapping>(); m.handle = bad.output.handle;
	m.address = readOnly; m.bytes = 4096; m.buffer_offset = 65536; m.permissions = AMDGPU_VM_READ;
	Require(ioctl(bad.fd, AMDGPU_VM_MAP, &m, sizeof(m)) == 0, "owned automatic fault alias");
	sGate = (volatile uint32*)(addr_t)gate.output.address;
	Enqueue(gate, Commands(gate, 0, true));
	Words fault = {0xc0033700, 0x100500, uint32(readOnly), uint32(readOnly >> 32), 0xdead5100};
	fault.resize(256, 0x80000000); Enqueue(bad, fault);
	DmaFill(dma, dmaBuffer, 2, 0xdead5102); DmaFill(dma, dmaBuffer, 3, 0xdead5103);
	for (unsigned n = 1; n < 16; n++) Enqueue(bad, Commands(bad, n));
	for (unsigned n = 1; n < 8; n++) Enqueue(gate, Commands(gate, n));
	Require(Memory(monitor).pending_jobs == 26, "26 accepted mixed jobs behind owned gate");
	auto set = Request<amdgpu_recovery_policy>(); set.flags = AMDGPU_RECOVERY_SET_POLICY; set.enabled = 0;
	Require(ioctl(monitor, AMDGPU_RECOVERY_POLICY, &set, sizeof(set)) < 0 && errno == B_BUSY,
		"automatic policy cannot change while accepted work is pending");
	auto memory = Memory(monitor); auto ram = Gart(monitor);
	ReleaseGate();
	for (uint64 fence = 1; fence <= 16; fence++) DmaWait(bad.fd, fence, B_BAD_DATA);
	DmaWait(gate.fd, 1, B_OK);
	for (uint64 fence = 2; fence <= 8; fence++) DmaWait(gate.fd, fence, B_BAD_DATA);
	DmaWait(dma, 1, B_OK); DmaWait(dma, 2, B_BAD_DATA); DmaWait(dma, 3, B_BAD_DATA);
	// Observation only: this test never submits a recovery ioctl.
	bigtime_t deadline = system_time() + 5000000;
	auto recovered = Recovery(monitor, AMDGPU_RECOVERY_INFO);
	while (recovered.generation == prior.generation && system_time() < deadline) {
		snooze(1000); recovered = Recovery(monitor, AMDGPU_RECOVERY_INFO);
	}
	auto after = Request<amdgpu_recovery_policy>();
	Require(ioctl(monitor, AMDGPU_RECOVERY_POLICY, &after, sizeof(after)) == 0
		&& after.enabled && !after.pending && after.status == B_OK
		&& after.attempts == policy.attempts + 1 && after.successes == policy.successes + 1
		&& recovered.status == B_OK && recovered.stage == 7 && recovered.generation == prior.generation + 1,
		"worker automatically resets once after every accepted job is terminal");
	Require(recovered.live_handles == 4 && recovered.live_vms == 3 && !recovered.pending
		&& !recovered.buffers && !recovered.vms && !recovered.reclaimed_ram && !recovered.reclaimed_vram
		&& Memory(monitor).allocated_bytes == memory.allocated_bytes && Gart(monitor).allocated_bytes == ram.allocated_bytes
		&& IRQ(monitor).completed_fences == 1, "automatic proof retains all live owners and allocations");
	for (unsigned round = 0; round < 2; round++) {
		Lost(gate.fd, prior.generation, true, 8, 2); Lost(bad.fd, prior.generation, true, 16, 1);
		Lost(idle.fd, prior.generation, true, 0, 0); Lost(dma, prior.generation, true, 3, 2);
		Pixels(gate, 1, true); Pixels(bad, 0); Pixels(idle, 0); DmaPixels(dmaBuffer, 0x51510001);
		DmaWait(gate.fd, 1, B_OK); DmaWait(dma, 1, B_OK);
		if (!round) {
			Client fresh = Create(34); Enqueue(fresh, Commands(fresh, 0)); Wait(fresh); Pixels(fresh, 1); Close(fresh);
		}
	}
	Close(gate); Close(bad); Close(idle); DmaFree(dma, dmaBuffer); close(dma);
	Require(Memory(monitor).allocated_bytes == baselineMemory.allocated_bytes
		&& Gart(monitor).allocated_bytes == baselineRam.allocated_bytes && IRQ(monitor).completed_fences == 2,
		"automatic recovery permits fresh work and complete old owner reclamation");
	printf("PASS: automatic mixed queue recovery generation %llu -> %llu, attempts/successes %llu/%llu;"
		"25 canceled/failed fences, old success, innocent client loss, all guards, fresh rendering and exact cleanup\n",
		(unsigned long long)prior.generation, (unsigned long long)recovered.generation,
		(unsigned long long)after.attempts, (unsigned long long)after.successes);
}

int main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0); disable_debugger(1);
	Client placeholder = {}; Commands(placeholder, 0, true, true, true);
	if (argc == 2 && strcmp(argv[1], "--expect-no-device") == 0) {
		int fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
		Require(fd < 0 && errno == ENOENT, "no AMD device in QEMU");
		puts("PASS: graphics queue command policy and no-device handling"); return 0;
	}
	if (argc == 2 && strcmp(argv[1], "--unprivileged") == 0) {
		Require(setgid(65534) == 0 && setuid(65534) == 0, "drop credentials");
		int fd = Open(); auto r = Request<amdgpu_gfx_queue>();
		Require(ioctl(fd, AMDGPU_GFX_QUEUE_COPY, &r, sizeof(r)) == -1 && errno == B_NOT_ALLOWED,
			"queued general graphics retains root gate");
		auto memory = Memory(fd); (void)memory; // Existing ordinary metadata initializes if needed.
		auto state = ClientStatus(fd); Require(!state.flags && state.status == B_OK, "ordinary owning-client status");
		ClientStatusABI(fd);
		auto reset = Request<amdgpu_recovery_info>();
		Require(ioctl(fd, AMDGPU_GFX_RECOVER_LIVE, &reset, sizeof(reset)) < 0 && errno == B_NOT_ALLOWED,
			"live recovery retains root gate"); close(fd);
		puts("PASS: asynchronous graphics retains root restriction"); return 0;
	}
	bool automatic = argc == 2 && strcmp(argv[1], "--auto-recovery") == 0;
	bool smoke = argc == 2 && strcmp(argv[1], "--smoke") == 0;
	bool fault = argc == 2 && strcmp(argv[1], "--fault") == 0;
	bool live = argc == 2 && strcmp(argv[1], "--live-recovery") == 0;
	bool recover = live || (argc == 2 && strcmp(argv[1], "--fault-recovery") == 0);
	Require((argc == 1 || smoke || fault || recover || automatic) && geteuid() == 0, "queue qualification requires root");
	int monitor = Open(); auto render = Request<amdgpu_render_info>();
	Require(ioctl(monitor, AMDGPU_RENDER_INFO, &render, sizeof(render)) == 0
		&& (render.capabilities & AMDGPU_RENDER_QUEUE_COPY), "queued graphics capability");
	if (automatic) {
		AutomaticQualification(monitor); AutomaticQualification(monitor); close(monitor); return 0;
	}
	if (fault || recover) { FaultQualification(monitor, recover, live); close(monitor); return 0; }
	auto before = IRQ(monitor); auto memory = Memory(monitor); auto ram = Gart(monitor);
	Client c[5]; for (unsigned i = 0; i < 5; i++) c[i] = Create(i + 1);
	// Malformed requests and inaccessible sources cannot submit work.
	for (unsigned i = 0; i < 8; i++) {
		auto words = Commands(c[0], 0); auto r = Request<amdgpu_gfx_queue>();
		r.address = (addr_t)words.data(); r.dwords = words.size(); r.flags = 1;
		int error = B_BAD_VALUE;
		if (i == 0) r.version++;
		if (i == 1) r.size--;
		if (i == 2) r.reserved[0] = 1;
		if (i == 3) r.flags = 2;
		if (i == 4) r.dwords = 257;
		if (i == 5) { r.address = 0; error = B_BAD_ADDRESS; }
		if (i == 6) { words[0] = 0; error = B_NOT_ALLOWED; }
		if (i == 7) r.dwords = 16640;
		Require(ioctl(c[0].fd, AMDGPU_GFX_QUEUE_COPY, &r, sizeof(r)) == -1
			&& errno == error && r.fence == 0, "reject malformed queue request");
	}
	Require(Memory(c[0].fd).submitted == 0 && IRQ(monitor).completed_fences == before.completed_fences,
		"rejected requests never reach GPU or fence sequence");
	if (smoke) {
		sGate = (volatile uint32*)(addr_t)c[0].output.address;
		Enqueue(c[0], Commands(c[0], 0, true, true));
		Enqueue(c[0], Commands(c[0], 1)); Wait(c[0], true); Frozen(c[0]);
		ReleaseGate(); Wait(c[0]); Pixels(c[0], 2, true);
		for (unsigned i = 1; i < 5; i++) Pixels(c[i], 0);
		for (auto& client : c) Close(client);
		Require(IRQ(monitor).completed_fences == before.completed_fences + 2
			&& Memory(monitor).allocated_bytes == memory.allocated_bytes
			&& Gart(monitor).allocated_bytes == ram.allocated_bytes, "smoke completion and reclamation");
		puts("PASS: two queued snapshots, CPU release of active GPU gate, pending fences, frozen VM and all guards");
		close(monitor); return 0;
	}
	sGate = (volatile uint32*)(addr_t)c[0].output.address;
	bigtime_t started = system_time();
	for (unsigned i = 0; i < 4; i++) {
		for (unsigned n = 0; n < 64; n++)
			Enqueue(c[i], Commands(c[i], n, i == 0 && n == 0, n % 16 == 0), 0, n != 63);
		Wait(c[i], true); Frozen(c[i]);
		Require(Memory(c[i].fd).submitted == 64 && Memory(c[i].fd).completed == 0,
			"enqueues return before blocked GPU completion");
	}
	Enqueue(c[0], Commands(c[0], 0), B_WOULD_BLOCK);
	Enqueue(c[4], Commands(c[4], 0), B_WOULD_BLOCK);
	Require(Memory(monitor).pending_jobs == 256, "exact device queue limit including active job");
	bigtime_t queued = system_time() - started;
	ReleaseGate();
	for (unsigned i = 0; i < 4; i++) { Wait(c[i]); Pixels(c[i], 64, i == 0); }
	Pixels(c[4], 0);
	for (auto& client : c) Close(client);
	printf("PASS: 256 captured async IBs across four VMs queued in %lld us; per-file/global quotas,"
		" pending fences, frozen mappings and all pixels/guards\n", (long long)queued);

	Client mixed = Create(8); sGate = (volatile uint32*)(addr_t)mixed.output.address;
	Enqueue(mixed, Commands(mixed, 0, true));
	auto dma = Request<amdgpu_dma_submit>(); dma.operation = AMDGPU_DMA_FILL;
	dma.destination = mixed.output.handle; dma.destination_offset = 16384;
	dma.bytes = 4096; dma.value = 0xa5c35100;
	Require(ioctl(mixed.fd, AMDGPU_SUBMIT_DMA, &dma, sizeof(dma)) == 0
		&& dma.fence == ++mixed.fence, "DMA shares ordered graphics fence sequence");
	Enqueue(mixed, Commands(mixed, 1, false, false, true));
	dma.operation = AMDGPU_DMA_COPY; dma.source = mixed.output.handle;
	dma.source_offset = 20480; dma.destination_offset = 24576;
	Require(ioctl(mixed.fd, AMDGPU_SUBMIT_DMA, &dma, sizeof(dma)) == 0
		&& dma.fence == ++mixed.fence, "DMA after graphics has ordered fence");
	Wait(mixed, true); ReleaseGate(); Wait(mixed); Pixels(mixed, 2, true, true); Close(mixed);
	puts("PASS: GFX/DMA/GFX/DMA FIFO dependencies and shared fences, exact copied data and guards");

	for (unsigned round = 0; round < 4; round++) {
		Client gate = Create(16 + round * 2), closed = Create(17 + round * 2);
		sGate = (volatile uint32*)(addr_t)gate.output.address;
		Enqueue(gate, Commands(gate, 0, true));
		for (unsigned n = 0; n < 64; n++) Enqueue(closed, Commands(closed, n));
		Wait(closed, true); Frozen(closed);
		Close(closed);
		Require(Memory(monitor).pending_jobs == 65, "close retains all queued jobs while GPU is blocked");
		ReleaseGate(); Wait(gate); Drain(monitor); Pixels(gate, 1, true); Close(gate);
		Require(Memory(monitor).allocated_bytes == memory.allocated_bytes
			&& Gart(monitor).allocated_bytes == ram.allocated_bytes, "closed queued VMs and buffers fully reclaimed");
	}
	auto after = IRQ(monitor);
	Require(after.completed_fences == before.completed_fences + 518
		&& after.eop_events == before.eop_events + 1036, "exact graphics IRQ counts across every queued job");
	printf("PASS: 518 queued graphics jobs, two DMA jobs, four closes with 64 pending IBs,"
		" completion %llu -> %llu, no faults and complete reclamation\n",
		(unsigned long long)before.completed_fences, (unsigned long long)after.completed_fences);
	close(monitor);
}
