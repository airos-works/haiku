/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
// Root-only translation qualification. Every possible target is owned.
// Read-only store/atomic modes deliberately fault and quarantine the GPU.
#include <amdgpu_haiku.h>
#include <OS.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include "GfxCommandValidator.h"
#include "ShaderAtc.h"

static_assert(sizeof(amdgpu_diagnostic_ram_page) == 40, "diagnostic ABI");
static const uint64 kCode = 0x500000000ULL, kOutput = 0x300000000ULL;
static const uint32 kGuard = 0x6b6b6b6b, kDone = 0x51a7c001;
enum Operation { kRead, kStore, kAtomic };
struct Client { int fd; amdgpu_buffer code, output, input; uint32 seed, marker; };
static void Require(bool okay, const char* why)
{
	if (!okay) { fprintf(stderr, "FAIL: %s (%d: %s)\n", why, errno, strerror(errno)); exit(1); }
}
template<typename T> static T Request()
{
	T r = {}; r.version = AMDGPU_HAIKU_ABI_VERSION; r.size = sizeof(r); return r;
}
template<typename T> static T Info(int fd, uint32 op)
{
	T r = Request<T>(); Require(ioctl(fd, op, &r, sizeof(r)) == 0, "read independent state"); return r;
}
static amdgpu_irq_info IRQ(int fd, bool healthy)
{
	auto r = Info<amdgpu_irq_info>(fd, AMDGPU_IRQ_INFO);
	if (healthy) Require(r.enabled && r.msi && r.status == B_OK && !r.vm_faults
		&& !r.privileged_faults && !r.unknown && !r.overflows && r.rptr == r.wptr
		&& r.waits == r.completed_fences, "healthy independent IRQ counters");
	return r;
}
static amdgpu_buffer Buffer(int fd, uint32 bytes, bool ram = true, bool diagnostic = false)
{
	auto b = Request<amdgpu_buffer>(); b.bytes = bytes;
	uint32 op = diagnostic ? AMDGPU_CREATE_DIAGNOSTIC_RAM_BUFFER
		: ram ? AMDGPU_CREATE_SYSTEM_BUFFER : AMDGPU_CREATE_BUFFER;
	Require(ioctl(fd, op, &b, sizeof(b)) == 0,
		"allocate owned memory");
	if (ram) Require(ioctl(fd, AMDGPU_MAP_BUFFER, &b, sizeof(b)) == 0, "map owned CPU memory");
	if (diagnostic) {
		for (uint32 i = 0; i < b.bytes / 4; i++)
			Require(((uint32*)(addr_t)b.address)[i] == 0, "diagnostic RAM starts zeroed");
	}
	return b;
}
static void Map(int fd, const amdgpu_buffer& b, uint64 va, uint32 permissions)
{
	auto m = Request<amdgpu_vm_mapping>(); m.handle = b.handle; m.address = va;
	m.bytes = 4096; m.permissions = permissions;
	Require(ioctl(fd, AMDGPU_VM_MAP, &m, sizeof(m)) == 0, "map one owned GPU page");
}
static uint64 Physical(int fd, const amdgpu_buffer& b, uint64 offset = 0)
{
	auto r = Request<amdgpu_diagnostic_ram_page>(); r.handle = b.handle; r.offset = offset;
	Require(ioctl(fd, AMDGPU_DIAGNOSTIC_RAM_PAGE, &r, sizeof(r)) == 0
		&& r.physical_address != 0 && !(r.physical_address & 4095), "owned wired RAM backing");
	return r.physical_address;
}
static uint32 Input(uint32 seed, uint32 word) { return seed ^ (word * 0x01020408u); }
static uint32 Stored(uint32 seed, uint32 lane) { return seed ^ 0x33330000 ^ lane; }
static void Fill(const amdgpu_buffer& b, uint32 seed, bool pattern)
{
	auto p = (uint32*)(addr_t)b.address;
	for (uint32 i = 0; i < b.bytes / 4; i++) p[i] = pattern ? Input(seed, i) : seed;
}
static void InputUnchanged(const amdgpu_buffer& b, uint32 seed)
{
	auto p = (volatile uint32*)(addr_t)b.address;
	for (uint32 i = 0; i < b.bytes / 4; i++) Require(p[i] == Input(seed, i), "source and guards unchanged");
}
static void Target(const Client& c, Operation operation)
{
	auto p = (volatile uint32*)(addr_t)c.input.address;
	for (uint32 i = 0; i < c.input.bytes / 4; i++) {
		uint32 want = Input(c.seed, i);
		if (i >= 256 && i < 320) {
			if (operation == kStore) want = Stored(c.seed, i - 256);
			if (operation == kAtomic) want += 0x101;
		}
		Require(p[i] == want, "entire GPUVM target and guards");
	}
}
static void CodeUnchanged(const Client& c)
{
	Require(memcmp((void*)(addr_t)c.code.address, kShaderAtc, sizeof(kShaderAtc)) == 0,
		"shader unchanged");
	auto p = (volatile uint32*)(addr_t)c.code.address;
	for (uint32 i = sizeof(kShaderAtc) / 4; i < 1024; i++) Require(p[i] == kGuard, "shader tail guard");
}
static void Output(const Client& c, bool complete, bool checkValue, uint32 seed,
	Operation operation = kRead)
{
	auto p = (volatile uint32*)(addr_t)c.output.address;
	for (uint32 i = 0; i < 1024; i++) {
		uint32 want = kGuard;
		if (i >= 256 && i < 512) {
			uint32 lane = (i - 256) / 4, field = i % 4;
			if (field == 0) want = c.marker ^ lane;
			if (field == 1) {
				if (!complete || !checkValue) continue;
				want = Input(seed, 256 + lane);
				if (operation == kStore) want = Stored(seed, lane);
			}
			if (field == 2) {
				if (!complete) { Require(p[i] == kGuard || p[i] == kDone, "bounded post-fault marker"); continue; }
				want = kDone;
			}
		}
		if (p[i] != want) {
			fprintf(stderr, "output word %u: %#x expected %#x\n", i, (unsigned)p[i], want);
			Require(false, "output markers and all guards");
		}
	}
}
static std::vector<uint32> Commands(uint64 source, bool atc, uint32 marker,
	Operation operation, uint32 seed)
{
	std::vector<uint32> w;
	auto reg = [&](uint32 index, uint32 value) {
		w.push_back(0xc0017600); w.push_back(index - 0x2c00); w.push_back(value);
	};
	reg(0x2e04, 0); reg(0x2e05, 0); reg(0x2e06, 0);
	reg(0x2e07, 64); reg(0x2e08, 1); reg(0x2e09, 1);
	reg(0x2e0c, kCode >> 8); reg(0x2e0d, kCode >> 40);
	reg(0x2e12, 1 | 2 << 6 | 0xc0 << 12); // Eight VGPRs, 24 SGPRs; PRIV clear.
	reg(0x2e13, 8 << 1 | 1 << 7);
	reg(0x2e15, 0); reg(0x2e16, 0xffffffff); reg(0x2e17, 0xffffffff);
	reg(0x2e18, 0); reg(0x2e19, 0xffffffff); reg(0x2e1a, 0xffffffff);
	reg(0x2e40, uint32(kOutput + 1024)); reg(0x2e41, kOutput >> 32);
	reg(0x2e42, uint32(source + 1024)); reg(0x2e43, source >> 32);
	reg(0x2e44, atc ? 1 << 24 : 0); reg(0x2e45, marker);
	reg(0x2e46, operation); reg(0x2e47, seed ^ 0x33330000);
	w.insert(w.end(), {0xc0031502, 1, 1, 1, 5, 0xc0004600, 7 | 4 << 8});
	w.resize(256, 0x80000000);
	Require(amdgpu::ValidateGfx8Commands(w.data(), w.size()).error == amdgpu::GfxCommandValidation::OK,
		"ATC probe outer commands pass checked policy");
	return w;
}
static amdgpu_gfx_submit Submit(Client& c, uint64 source, bool atc, Operation operation)
{
	Fill(c.output, kGuard, false);
	auto w = Commands(source, atc, c.marker, operation, c.seed);
	auto r = Request<amdgpu_gfx_submit>(); r.address = (addr_t)w.data(); r.dwords = w.size();
	r.flags = AMDGPU_GFX_VALIDATE_COMMANDS; __sync_synchronize();
	Require(ioctl(c.fd, AMDGPU_GFX_SUBMIT_COPY, &r, sizeof(r)) == 0, "checked copied submission");
	__sync_synchronize();
	printf("ATC %u operation %u result %#x completion %llu VM %#x/%#x ring %u/%u elapsed %llu us\n",
		atc, operation, (unsigned)r.status, (unsigned long long)r.completion, r.vm_fault_status[0],
		r.vm_fault_status[1], r.rptr, r.wptr, (unsigned long long)r.elapsed_us);
	return r;
}
static void ABI(int fd, const amdgpu_buffer& ram, uint64 foreign)
{
	auto r = Request<amdgpu_diagnostic_ram_page>(); r.handle = ram.handle;
	Require(ioctl(fd, AMDGPU_DIAGNOSTIC_RAM_PAGE, NULL, sizeof(r)) < 0 && errno == B_BAD_ADDRESS,
		"null page diagnostic rejected");
	for (uint32 variant = 0; variant < 10; variant++) {
		r = Request<amdgpu_diagnostic_ram_page>(); r.handle = ram.handle;
		size_t bytes = sizeof(r);
		switch (variant) {
			case 0: r.version++; break;
			case 1: r.size--; break;
			case 2: bytes--; break;
			case 3: r.flags = 1; break;
			case 4: r.reserved = 1; break;
			case 5: r.offset = 1; break;
			case 6: r.offset = ram.bytes; break;
			case 7: r.offset = ~4095ULL; break;
			case 8: r.handle = 0; break;
			case 9: r.handle = foreign; break;
		}
		Require(ioctl(fd, AMDGPU_DIAGNOSTIC_RAM_PAGE, &r, bytes) < 0 && errno == B_BAD_VALUE,
			"malformed/range/foreign page query rejected");
	}
	auto vram = Buffer(fd, 4096, false);
	r = Request<amdgpu_diagnostic_ram_page>(); r.handle = vram.handle;
	Require(ioctl(fd, AMDGPU_DIAGNOSTIC_RAM_PAGE, &r, sizeof(r)) < 0 && errno == B_BAD_VALUE,
		"VRAM cannot be queried as RAM");
	Require(ioctl(fd, AMDGPU_FREE_BUFFER, &vram, sizeof(vram)) == 0, "free VRAM test buffer");
	for (uint32 bytes : {0u, 4096u, 8193u, 65536u}) {
		auto bad = Request<amdgpu_buffer>(); bad.bytes = bytes;
		Require(ioctl(fd, AMDGPU_CREATE_DIAGNOSTIC_RAM_BUFFER, &bad, sizeof(bad)) < 0
			&& errno == B_BAD_VALUE, "diagnostic allocation has fixed size");
	}
	auto bad = Request<amdgpu_buffer>(); bad.bytes = 8192; bad.reserved = 1;
	Require(ioctl(fd, AMDGPU_CREATE_DIAGNOSTIC_RAM_BUFFER, &bad, sizeof(bad)) < 0
		&& errno == B_BAD_VALUE, "diagnostic allocation rejects reserved flags");
	r.handle = ram.handle;
	int ro = open("/dev/" AMDGPU_DEVICE_NAME, O_RDONLY); Require(ro >= 0, "open read-only client");
	Require(ioctl(ro, AMDGPU_DIAGNOSTIC_RAM_PAGE, &r, sizeof(r)) < 0 && errno == B_NOT_ALLOWED,
		"read-only page query rejected");
	bad = Request<amdgpu_buffer>(); bad.bytes = 8192;
	Require(ioctl(ro, AMDGPU_CREATE_DIAGNOSTIC_RAM_BUFFER, &bad, sizeof(bad)) < 0
		&& errno == B_NOT_ALLOWED, "read-only diagnostic allocation rejected"); close(ro);
	pid_t child = fork(); Require(child >= 0, "fork privilege check");
	if (child == 0) {
		Require(setgid(65534) == 0 && setuid(65534) == 0, "drop privilege");
		int own = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR); Require(own >= 0, "ordinary client");
		Require(ioctl(own, AMDGPU_DIAGNOSTIC_RAM_PAGE, &r, sizeof(r)) < 0 && errno == B_NOT_ALLOWED,
			"physical diagnostic is root-only");
		Require(ioctl(own, AMDGPU_CREATE_DIAGNOSTIC_RAM_BUFFER, &bad, sizeof(bad)) < 0
			&& errno == B_NOT_ALLOWED, "high-RAM diagnostic allocation is root-only");
		close(own); _exit(0);
	}
	int status; Require(waitpid(child, &status, 0) == child && WIFEXITED(status)
		&& WEXITSTATUS(status) == 0, "privilege child passes");
}
int main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0); disable_debugger(1);
	for (auto op : {kRead, kStore, kAtomic}) {
		Commands(0x100000, false, 0x1234, op, 0x5678);
		Commands(0x100000, true, 0x1234, op, 0x5678);
	}
	bool absent = argc == 2 && strcmp(argv[1], "--expect-no-device") == 0;
	bool atc = argc == 2 && strcmp(argv[1], "--atc") == 0;
	bool control = argc == 2 && strcmp(argv[1], "--control") == 0;
	bool store = argc == 2 && (strcmp(argv[1], "--atc-store") == 0
		|| strcmp(argv[1], "--atc-store-readonly") == 0);
	bool atomic = argc == 2 && (strcmp(argv[1], "--atc-atomic") == 0
		|| strcmp(argv[1], "--atc-atomic-readonly") == 0);
	bool readOnly = (store || atomic) && strstr(argv[1], "-readonly") != NULL;
	Operation operation = store ? kStore : atomic ? kAtomic : kRead;
	atc |= store || atomic;
	Require(absent || ((atc || control) && geteuid() == 0),
		"usage: amdgpu_atc --control | --atc[-store|-atomic][-readonly] | --expect-no-device");
	int monitor = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
	if (absent) { Require(monitor < 0 && errno == ENOENT, "no AMD device");
		puts("PASS: ATC probe command policy and no-device handling"); return 0; }
	Require(monitor >= 0, "open monitor");
	Info<amdgpu_render_info>(monitor, AMDGPU_RENDER_INFO);
	auto device = Info<amdgpu_info>(monitor, AMDGPU_GET_INFO);
	Require(device.vram_gpu_base >= (1ULL << 36), "VRAM GPU aperture outside candidate RAM addresses");
	auto initial = IRQ(monitor, true);
	auto initialRAM = Info<amdgpu_gart_info>(monitor, AMDGPU_GART_INFO);
	auto initialVRAM = Info<amdgpu_memory_info>(monitor, AMDGPU_MEMORY_INFO);
	Client clients[2] = {};
	for (uint32 i = 0; i < 2; i++) {
		auto& c = clients[i]; c.fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
		Require(c.fd >= 0, "open separate VM"); Info<amdgpu_vm_info>(c.fd, AMDGPU_VM_INFO);
		c.code = Buffer(c.fd, 4096); c.output = Buffer(c.fd, 4096); c.input = Buffer(c.fd, 4096);
		c.seed = 0x5b000001 ^ (i * 0x30303030); c.marker = 0x51a71000 | i;
		Fill(c.code, kGuard, false); memcpy((void*)(addr_t)c.code.address, kShaderAtc, sizeof(kShaderAtc));
		Fill(c.input, c.seed, true); Fill(c.output, kGuard, false);
		Map(c.fd, c.code, kCode, AMDGPU_VM_READ | AMDGPU_VM_EXECUTE);
		Map(c.fd, c.output, kOutput, AMDGPU_VM_READ | AMDGPU_VM_WRITE);
	}
	// Hold candidates until teardown to avoid repeatedly receiving the same
	// physical page if a page above the current 64-GiB GPUVA limit is selected.
	std::vector<amdgpu_buffer> physicalBuffers;
	uint64 address = 0;
	for (uint32 i = 0; i < 128; i++) {
		auto b = Buffer(clients[0].fd, 8192, true, true); physicalBuffers.push_back(b);
		uint64 p = Physical(clients[0].fd, b);
		Require(p >= (1ULL << 32) && p <= (1ULL << 36) - 8192, "diagnostic physical range enforced");
		// Independently exclude device apertures, the VMID0 GART window and
		// our code and output GPUVAs, even with the restricted allocation. The CPU
		// mapping/GART agreement proves this is live, owned, wired system RAM.
		bool aperture = p >= 0x10000000 && p < 0x50000000;
		for (uint32 bar = 0; bar < 6; bar++)
			aperture |= p >= device.bar_address[bar]
				&& p - device.bar_address[bar] < device.bar_size[bar];
		if (i < 4) printf("owned RAM candidate %u: %#llx excluded aperture %u\n",
			i, (unsigned long long)p, aperture);
		if (p >= 0x20000 && p < (1ULL << 36) && !aperture && p != kCode && p != kOutput) {
			address = p; break;
		}
	}
	Require(address != 0, "owned physical RAM within the GPUVA aperture");
	const uint32 physicalSeed = 0xa7000001;
	for (const auto& b : physicalBuffers) Fill(b, physicalSeed, true);
	const auto& physical = physicalBuffers.back();
	Require(Physical(clients[0].fd, physical, 4096) != address, "distinct second RAM page");
	ABI(clients[0].fd, physical, clients[1].input.handle);
	for (auto& c : clients) {
		Require(Physical(c.fd, c.input) != address, "distinct GPUVM backing from physical target");
		Map(c.fd, c.input, address, AMDGPU_VM_READ | (operation == kRead ? 0 : AMDGPU_VM_WRITE));
		auto result = Submit(c, address, false, operation);
		Require(result.status == B_OK && !result.vm_fault_status[0] && !result.vm_fault_status[1], "GPUVM control completes");
		Output(c, true, true, c.seed, operation); Target(c, operation);
	}
	for (const auto& c : clients) { Target(c, operation); CodeUnchanged(c); }
	for (const auto& b : physicalBuffers) InputUnchanged(b, physicalSeed);
	IRQ(monitor, true);
	auto snapshot = Info<amdgpu_shader_memory_info>(monitor, AMDGPU_SHADER_MEMORY_INFO);
	Require(snapshot.vmid == 2 && snapshot.selector_before == snapshot.selector_after
		&& snapshot.sh_mem_config == 0x338 && snapshot.sh_mem_bases == 0x2000
		&& snapshot.sh_mem_ape1_base == 1 && snapshot.sh_mem_ape1_limit == 0
		&& snapshot.atc_ats_control == 0xa0800 && snapshot.atc_ats_debug == 0x1c20
		&& !snapshot.atc_vmid_pasid && !snapshot.atc_l1_address_offset,
		"qualified translation configuration before ATC access");
	for (const auto& aperture : snapshot.atc_aperture)
		for (uint32 field : aperture) Require(field == 0, "no ATC aperture translation configured");
	printf("PASS: diagnostic ABI and two distinct GPUVM controls at owned physical address %#llx\n",
		(unsigned long long)address);
	if (readOnly) {
		auto& c = clients[0]; Fill(c.input, c.seed, true);
		auto good = Submit(c, address, true, operation);
		Require(good.status == B_OK && !good.vm_fault_status[0] && !good.vm_fault_status[1],
			"same ATC operation succeeds on writable mapping");
		Output(c, true, true, c.seed, operation); Target(c, operation);
		for (const auto& b : physicalBuffers) InputUnchanged(b, physicalSeed);
		auto m = Request<amdgpu_vm_mapping>(); m.address = address; m.bytes = 4096;
		Require(ioctl(c.fd, AMDGPU_VM_UNMAP, &m, sizeof(m)) == 0, "unmap writable target");
		Map(c.fd, c.input, address, AMDGPU_VM_READ);
		IRQ(monitor, true);
		puts("PASS: same ATC shader on writable mapping before removing write permission");
	}
	auto heldRAM = Info<amdgpu_gart_info>(monitor, AMDGPU_GART_INFO);
	auto heldVRAM = Info<amdgpu_memory_info>(monitor, AMDGPU_MEMORY_INFO);
	bool fault = false, escaped = false;
	if (atc) {
		Fill(clients[0].input, clients[0].seed, true);
		clients[0].marker = 0x51a710f0;
		auto result = Submit(clients[0], address, true, operation);
		fault = result.status != B_OK;
		auto irq = IRQ(monitor, !fault);
		printf("ATC IRQ status %#x waits %llu completed %llu VM %llu privileged %llu unknown %llu overflow %llu last %#x/%#x/%#x/%#x\n",
			(unsigned)irq.status, (unsigned long long)irq.waits, (unsigned long long)irq.completed_fences,
			(unsigned long long)irq.vm_faults, (unsigned long long)irq.privileged_faults,
			(unsigned long long)irq.unknown, (unsigned long long)irq.overflows,
			irq.last[0], irq.last[1], irq.last[2], irq.last[3]);
		Output(clients[0], !fault, operation != kRead && !readOnly, clients[0].seed, operation);
		Output(clients[1], true, true, clients[1].seed, operation);
		uint32 physicalMatches = 0, virtualMatches = 0, zeroes = 0, untouched = 0;
		auto p = (volatile uint32*)(addr_t)clients[0].output.address;
		for (uint32 lane = 0; lane < 64; lane++) {
			uint32 v = p[256 + lane * 4 + 1];
			physicalMatches += v == Input(physicalSeed, 256 + lane);
			virtualMatches += v == Input(clients[0].seed, 256 + lane);
			zeroes += v == 0; untouched += v == kGuard;
		}
		printf("ATC returned data: physical %u GPUVM %u zero %u untouched %u of64\n",
			physicalMatches, virtualMatches, zeroes, untouched);
		escaped = physicalMatches != 0;
		if (fault) {
			Require(result.status == B_BAD_DATA && irq.status == B_BAD_DATA && irq.vm_faults
				&& !irq.unknown && !irq.privileged_faults && !irq.overflows,
				"ATC rejection has a hardware VM fault, not a timeout");
			auto denied = Request<amdgpu_buffer>(); denied.bytes = 4096;
			Require(ioctl(clients[0].fd, AMDGPU_CREATE_SYSTEM_BUFFER, &denied, sizeof(denied)) < 0
				&& errno == B_DEV_NOT_READY, "fault blocks further allocations");
		} else if (operation == kRead) Require(physicalMatches == 64 || virtualMatches == 64 || zeroes == 64,
			"uniform ATC read classification");
		Require(!readOnly || fault, "read-only mapping rejects ATC write/atomic");
		Require(irq.waits == initial.waits + (readOnly ? 4 : 3), "exact execution waits");
	}
	Target(clients[0], fault ? kRead : operation); Target(clients[1], operation);
	for (const auto& c : clients) CodeUnchanged(c);
	for (const auto& b : physicalBuffers) InputUnchanged(b, physicalSeed);
	for (auto& c : clients) {
		close(c.fd);
		Require(delete_area(c.code.area) == B_OK && delete_area(c.output.area) == B_OK
			&& delete_area(c.input.area) == B_OK, "release revoked clones");
	}
	for (const auto& b : physicalBuffers) Require(delete_area(b.area) == B_OK, "release physical-target clone");
	auto finalRAM = Info<amdgpu_gart_info>(monitor, AMDGPU_GART_INFO);
	auto finalVRAM = Info<amdgpu_memory_info>(monitor, AMDGPU_MEMORY_INFO);
	Require(finalRAM.allocated_bytes == (fault ? heldRAM : initialRAM).allocated_bytes
		&& finalVRAM.allocated_bytes == (fault ? heldVRAM : initialVRAM).allocated_bytes
		&& finalVRAM.faulted == uint32(fault), "memory reclamation or fault quarantine");
	if (!fault) Require(IRQ(monitor, true).completed_fences == initial.completed_fences + (atc ? 3 : 2),
		"exact healthy completion count");
	close(monitor);
	if (fault) puts("TERMINAL_GPU_FAULT: owned targets unchanged, all guards intact, post-close quarantine verified");
	Require(!escaped, "ATC must not escape GPUVM into the separate owned physical target");
	puts(control ? "PASS: ATC fixture positive controls, owned dual targets, ABI and reclamation"
		: fault ? "PASS: ATC access rejected by VM fault with owned targets and quarantine"
		: "OBSERVED: ATC access completes without touching the separate physical target");
}
