/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include <amdgpu_haiku.h>
#include <OS.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

static const uint64 kSource = 0x200000000ULL, kOutput = 0x300000000ULL;
static const uint32 kBytes = 65536, kSentinel = 0x6b6b6b6b;
static void Require(bool okay, const char* why)
{
	if (!okay) { fprintf(stderr, "FAIL: %s: %s\n", why, strerror(errno)); exit(1); }
}
template<typename T> static T Request()
{
	T r = {}; r.version = AMDGPU_HAIKU_ABI_VERSION; r.size = sizeof(r); return r;
}
static int Open()
{
	int fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
	Require(fd >= 0, "open client"); return fd;
}
static amdgpu_buffer Buffer(int fd, uint64 address, uint32 bytes, uint32 permissions)
{
	auto b = Request<amdgpu_buffer>(); b.bytes = bytes;
	Require(ioctl(fd, AMDGPU_CREATE_SYSTEM_BUFFER, &b, sizeof(b)) == 0
		&& ioctl(fd, AMDGPU_MAP_BUFFER, &b, sizeof(b)) == 0, "create CPU RAM");
	auto m = Request<amdgpu_vm_mapping>();
	m.handle = b.handle; m.address = address; m.bytes = bytes; m.permissions = permissions;
	Require(ioctl(fd, AMDGPU_VM_MAP, &m, sizeof(m)) == 0, "map owned GPU RAM");
	return b;
}
static amdgpu_irq_info IRQ(int fd)
{
	auto r = Request<amdgpu_irq_info>();
	Require(ioctl(fd, AMDGPU_IRQ_INFO, &r, sizeof(r)) == 0 && r.enabled && r.msi
		&& r.status == B_OK && !r.vm_faults && !r.privileged_faults && !r.unknown
		&& !r.overflows && r.rptr == r.wptr, "healthy IRQ state"); return r;
}
static std::vector<uint32> Program(uint32 seed, uint32 dwords, bool gate)
{
	std::vector<uint32> p;
	auto emit = [&](uint32 word) { p.push_back(word); };
	if (gate) {
		emit(0xc0033700); emit(5 << 8 | 1 << 20); // confirmed entry marker
		emit((uint32)kOutput); emit(kOutput >> 32); emit(seed);
		emit(0xc0004200); emit(0); // PFP_SYNC_ME
		emit(0xc0053c00); emit(5 | 1 << 4 | 1 << 8); // memory/PFP wait >= seed
		emit((uint32)(kOutput + 4)); emit(kOutput >> 32); emit(seed); emit(0xffffffff); emit(0x20);
	}
	// Read both ends of every private page after the entry gate. These are
	// data reads, independent of any command bytes already prefetched by CP.
	for (uint32 page = 0; page < 16; page++) {
		for (uint32 end = 0; end < 2; end++) {
			uint64 src = AMDGPU_COPY_IB_ADDRESS + page * 4096 + (end ? 4088 : 0);
			uint64 dst = kOutput + 4096 + page * 32 + end * 8;
			emit(0xc0044000); emit(1 | 5 << 8 | 1 << 16 | 1 << 20);
			emit(src); emit(src >> 32); emit(dst); emit(dst >> 32);
		}
	}
	uint32 padding = dwords - p.size();
	Require(padding >= 2 && padding <= 16385, "packet padding bounds");
	emit(0xc0001000 | (padding - 2) << 16);
	while (p.size() < dwords) emit(seed ^ (uint32)p.size() * 0x10204081u);
	return p;
}
struct Job { int fd; amdgpu_gfx_submit request; int result; };
static int32 Execute(void* data)
{
	Job& j = *(Job*)data;
	j.result = ioctl(j.fd, AMDGPU_GFX_SUBMIT_COPY, &j.request, sizeof(j.request));
	return B_OK;
}
static void Completion(const amdgpu_gfx_submit& r)
{
	Require(r.status == B_OK && r.completion && !r.vm_fault_status[0]
		&& !r.vm_fault_status[1] && r.rptr == r.wptr, "copied commands retire without fault");
}
static void Pixels(volatile uint32* output, const std::vector<uint32>& copied, uint32 seed)
{
	std::vector<uint32> expected(16384 / 4, kSentinel);
	if (seed) expected[0] = expected[1] = seed;
	for (uint32 page = 0; page < 16; page++) for (uint32 end = 0; end < 2; end++) {
		uint32 src = page * 1024 + (end ? 1022 : 0), dst = 1024 + page * 8 + end * 2;
		for (uint32 word = 0; word < 2; word++)
			expected[dst + word] = src + word < copied.size() ? copied[src + word] : 0;
	}
	for (uint32 i = 0; i < expected.size(); i++) Require(output[i] == expected[i],
		"private page samples, zeroed tail and untouched output guards");
}
int main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc == 2 && strcmp(argv[1], "--unprivileged") == 0) {
		Require(setgid(65534) == 0 && setuid(65534) == 0, "drop privileges");
		int fd = Open(); auto r = Request<amdgpu_gfx_submit>(); r.dwords = 256;
		Require(ioctl(fd, AMDGPU_GFX_SUBMIT_COPY, &r, sizeof(r)) == -1
			&& errno == B_NOT_ALLOWED, "raw copied commands retain root gate");
		close(fd); puts("PASS: unprivileged copied raw submission rejected"); return 0;
	}
	if (argc == 2 && strcmp(argv[1], "--expect-no-device") == 0) {
		int fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
		Require(fd < 0 && errno == ENOENT, "no GPU in QEMU");
		puts("PASS: no-device copied submission handling"); return 0;
	}
	Require(argc == 1 && geteuid() == 0, "copied raw commands remain root-only");
	uint8* partial = (uint8*)mmap(NULL, 8192, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	Require(partial != MAP_FAILED, "allocate partial-copy source");
	memset(partial, 0xac, 4096);
	Require(mprotect(partial + 4096, 4096, PROT_NONE) == 0, "protect source's second page");
	int monitor = Open(); auto render = Request<amdgpu_render_info>();
	Require(ioctl(monitor, AMDGPU_RENDER_INFO, &render, sizeof(render)) == 0
		&& (render.capabilities & AMDGPU_RENDER_COPY_SUBMIT), "copy submission capability");
	auto before = IRQ(monitor);
	for (uint32 round = 0; round < 16; round++) {
		int fd = Open(); auto vm = Request<amdgpu_vm_info>();
		Require(ioctl(fd, AMDGPU_VM_INFO, &vm, sizeof(vm)) == 0 && vm.address_start == 131072,
			"create private VM and reserve copied command addresses");
		auto source = Buffer(fd, kSource, kBytes, AMDGPU_VM_READ | AMDGPU_VM_EXECUTE);
		auto output = Buffer(fd, kOutput, 16384, AMDGPU_VM_READ | AMDGPU_VM_WRITE);
		volatile uint32* pixels = (volatile uint32*)(addr_t)output.address;
		for (uint32 test = 0; test < 6; test++) {
			auto bad = Request<amdgpu_gfx_submit>(); bad.dwords = 256;
			if (test == 0) bad.flags = 2;
			if (test == 1) bad.reserved = 1;
			if (test == 2) bad.dwords = 0;
			if (test == 3) bad.dwords = 1;
			if (test == 4) bad.dwords = kBytes / 4 + 256;
			if (test == 5) bad.version++;
			Require(ioctl(fd, AMDGPU_GFX_SUBMIT_COPY, &bad, sizeof(bad)) == -1
				&& errno == B_BAD_VALUE, "invalid copied submission rejected before CPU access");
		}
		for (uint32 pass = 0; pass < 2; pass++) {
			uint32 seed = 1 + round * 2 + pass, dwords = pass ? 256 : kBytes / 4;
			auto program = Program(seed, dwords, true);
			std::vector<uint8> unaligned(program.size() * 4 + 1);
			memcpy(unaligned.data() + 1, program.data(), program.size() * 4);
			for (uint32 i = 0; i < 4096; i++) pixels[i] = kSentinel;
			pixels[0] = pixels[1] = 0;
			Job job = {fd, Request<amdgpu_gfx_submit>(), -1};
			job.request.address = (addr_t)(unaligned.data() + 1); job.request.dwords = dwords;
			thread_id thread = spawn_thread(Execute, "copied IB", B_NORMAL_PRIORITY, &job);
			Require(thread >= 0 && resume_thread(thread) == B_OK, "start copied submission");
			bigtime_t deadline = system_time() + 200000;
			while (pixels[0] != seed && system_time() < deadline) snooze(10);
			bool entered = pixels[0] == seed;
			memset(unaligned.data(), 0xcc, unaligned.size());
			__sync_synchronize(); pixels[1] = seed; __sync_synchronize();
			status_t result;
			Require(wait_for_thread(thread, &result) == B_OK && result == B_OK
				&& job.result == 0, "join copied submission");
			Completion(job.request); Require(entered, "observe GPU entry before replacing original commands");
			Pixels(pixels, program, seed);
		}
		// A failed user copy must clear previously copied bytes without any
		// hardware submission. Inspect that storage with a separate raw IB.
		auto invalid = Request<amdgpu_gfx_submit>(); invalid.dwords = 256;
		auto prior = IRQ(monitor);
		for (uint64 address : {0ULL, 0xfffffffffffff000ULL, 0x7fffffffffffULL,
				0xfffffffffffffe00ULL}) {
			invalid.address = address;
			Require(ioctl(fd, AMDGPU_GFX_SUBMIT_COPY, &invalid, sizeof(invalid)) == -1
				&& errno == B_BAD_ADDRESS, "bad/kernel/cross-boundary/wrapping CPU source rejected");
		}
		invalid.address = (addr_t)(partial + 4096 - 512);
		Require(ioctl(fd, AMDGPU_GFX_SUBMIT_COPY, &invalid, sizeof(invalid)) == -1
			&& errno == B_BAD_ADDRESS, "partly readable CPU source rejected and cleared");
		Require(IRQ(monitor).completed_fences == prior.completed_fences, "bad copy submits no GPU work");
		auto inspect = Program(0, 256, false);
		memcpy((void*)(addr_t)source.address, inspect.data(), inspect.size() * 4);
		for (uint32 i = 0; i < 4096; i++) pixels[i] = kSentinel;
		__sync_synchronize();
		auto raw = Request<amdgpu_gfx_submit>(); raw.address = kSource; raw.dwords = 256;
		Require(ioctl(fd, AMDGPU_GFX_SUBMIT, &raw, sizeof(raw)) == 0, "inspect failed-copy storage");
		Completion(raw); Pixels(pixels, {}, 0);
		close(fd);
		Require(delete_area(source.area) == B_OK && delete_area(output.area) == B_OK,
			"release revoked CPU clones");
	}
	auto after = IRQ(monitor);
	Require(after.completed_fences - before.completed_fences == 48
		&& after.waits - before.waits == 48 && after.eop_events - before.eop_events == 96,
		"exact copied/raw completion counts");
	auto memory = Request<amdgpu_memory_info>(); auto ram = Request<amdgpu_gart_info>();
	Require(ioctl(monitor, AMDGPU_MEMORY_INFO, &memory, sizeof(memory)) == 0
		&& !memory.faulted && !memory.allocated_bytes && !memory.pending_jobs, "all VRAM reclaimed");
	Require(ioctl(monitor, AMDGPU_GART_INFO, &ram, sizeof(ram)) == 0
		&& !ram.allocated_bytes && !(ram.vm_fault_status & 0xff), "all RAM reclaimed");
	close(monitor);
	Require(munmap(partial, 8192) == 0, "release partial-copy source");
	puts("PASS: 48 jobs, 16 private/reused VMs, 64KiB and short copied IBs, unaligned input, "
		"post-entry source replacement, zeroed tails/failures, exact data/IRQs and reclamation");
}
