/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
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
#include "GfxDraw.h"

static void Require(bool okay, const char* message)
{
	if (!okay) {
		fprintf(stderr, "FAIL: %s (%d: %s)\n", message, errno, strerror(errno));
		exit(1);
	}
}
template<typename T> static T Request()
{
	T value = {}; value.version = AMDGPU_HAIKU_ABI_VERSION; value.size = sizeof(T);
	return value;
}
static const uint64 kBytes = 65536;
static const uint64 kIB = 0x200000000ULL;
static const uint64 kShader = 0x300000000ULL;
static const uint64 kData = 0xfffff000ULL;
static uint32 Packet(uint32 op, uint32 count) { return 0xc0000000 | count << 16 | op << 8; }

static amdgpu_buffer Create(int fd, uint32 kind)
{
	auto b = Request<amdgpu_buffer>(); b.bytes = kBytes;
	Require(ioctl(fd, kind, &b, sizeof(b)) == 0, "allocate buffer");
	if (kind != AMDGPU_CREATE_DEVICE_BUFFER)
		Require(ioctl(fd, AMDGPU_MAP_BUFFER, &b, sizeof(b)) == 0, "map CPU buffer");
	return b;
}
static void Map(int fd, uint64 handle, uint64 address, uint32 permissions)
{
	auto m = Request<amdgpu_vm_mapping>();
	m.handle = handle; m.address = address; m.bytes = kBytes; m.permissions = permissions;
	Require(ioctl(fd, AMDGPU_VM_MAP, &m, sizeof(m)) == 0, "map GPU buffer");
}
static void Copy(int fd, uint64 source, uint64 destination)
{
	auto r = Request<amdgpu_dma_submit>(); r.operation = AMDGPU_DMA_COPY;
	r.source = source; r.destination = destination; r.bytes = kBytes;
	Require(ioctl(fd, AMDGPU_SUBMIT_DMA, &r, sizeof(r)) == 0, "queue upload/readback");
	auto wait = Request<amdgpu_fence_wait>(); wait.fence = r.fence; wait.timeout_us = 5000000;
	Require(ioctl(fd, AMDGPU_WAIT_FENCE, &wait, sizeof(wait)) == 0 && wait.status == B_OK,
		"upload/readback fence");
}
static uint64 Allocated(int fd, bool system)
{
	if (system) {
		auto r = Request<amdgpu_gart_info>();
		Require(ioctl(fd, AMDGPU_GART_INFO, &r, sizeof(r)) == 0 && r.vm_fault_status == 0,
			"GART remains healthy");
		return r.allocated_bytes;
	}
	auto r = Request<amdgpu_memory_info>();
	Require(ioctl(fd, AMDGPU_MEMORY_INFO, &r, sizeof(r)) == 0 && r.faulted == 0,
		"VRAM remains healthy");
	return r.allocated_bytes;
}
struct Client {
	int fd;
	amdgpu_buffer ib, shader, data, staging;
	std::vector<uint32> expected;
};
static void Upload(Client& c, amdgpu_buffer& b, const std::vector<uint32>& words)
{
	Require(words.size() * 4 <= kBytes, "upload extent");
	volatile uint32* dst = (volatile uint32*)(addr_t)(b.address ? b.address : c.staging.address);
	for (uint32 i = 0; i < kBytes / 4; i++) dst[i] = i < words.size() ? words[i] : 0;
	__sync_synchronize(); (void)dst[kBytes / 4 - 1];
	if (b.address == 0) Copy(c.fd, c.staging.handle, b.handle);
}
static void Check(Client& c)
{
	if (c.data.address == 0) Copy(c.fd, c.data.handle, c.staging.handle);
	volatile uint32* words = (volatile uint32*)(addr_t)(c.data.address ? c.data.address : c.staging.address);
	for (uint32 i = 0; i < c.expected.size(); i++) {
		if (words[i] != c.expected[i]) {
			fprintf(stderr, "client %d word %u got %#x expected %#x\n", c.fd,
				(unsigned)i, (unsigned)words[i], (unsigned)c.expected[i]);
			Require(false, "complete GPU result and guards");
		}
	}
}
static void Program(Client& c, uint32 seed, uint32 multiplier, uint32 dwords)
{
	// gfx803, assembled with LLVM: each lane stores seed XOR (index * factor).
	// The factor differs from the kernel-owned VM_TEST shader and is replaced
	// between jobs, proving that the uploaded instruction BO is fetched again.
	std::vector<uint32> shader = {
		0xd1c30000, 0x04018003, 0x24040082, 0x32040400, 0x7e060201,
		0xd11c6a03, 0x01a90103, 0xbe8400ff, multiplier,
		0xd2850004, 0x00000900, 0x2a080802, 0xdc710000, 0x00000402,
		0xbf8c0f70, 0xbf810000
	};
	Upload(c, c.shader, shader);
	std::vector<uint32> ib;
	auto emit = [&](uint32 word) { ib.push_back(word); };
	auto reg = [&](uint32 index, uint32 word) {
		emit(Packet(0x76, 1) | 2); emit(index - 0x2c00); emit(word);
	};
	auto write = [&](uint64 address, uint32 value) {
		emit(Packet(0x37, 3)); emit(5 << 8 | 1 << 20);
		emit(address); emit(address >> 32); emit(value);
	};
	write(kData, seed);
	reg(0x2e04, 0); reg(0x2e05, 0); reg(0x2e06, 0);
	reg(0x2e07, 64); reg(0x2e08, 1); reg(0x2e09, 1);
	reg(0x2e0c, kShader >> 8); reg(0x2e0d, kShader >> 40);
	reg(0x2e12, 1 | 1 << 6 | 0xc0 << 12); reg(0x2e13, 3 << 1 | 1 << 7);
	// COMPUTE_VMID is supplied by the kernel's trusted ring.
	reg(0x2e15, 0); reg(0x2e16, 0xffffffff); reg(0x2e17, 0xffffffff);
	reg(0x2e18, 0); reg(0x2e19, 0xffffffff); reg(0x2e1a, 0xffffffff);
	uint64 destination = kData + 4092;
	reg(0x2e40, destination); reg(0x2e41, destination >> 32); reg(0x2e42, seed);
	emit(Packet(0x15, 3) | 2); emit(16); emit(1); emit(1); emit(5);
	emit(Packet(0x46, 0)); emit(7 | 4 << 8);
	// Make the CP fetch the end of every IB, including a complete 64-KiB IB.
	uint32 padding = dwords - ib.size() - 5;
	Require(padding >= 2 && padding < 16384, "PM4 padding bounds");
	emit(Packet(0x10, padding - 2));
	for (uint32 i = 1; i < padding; i++) emit(0);
	write(kData + kBytes - 4, seed ^ 0xabcdef01);
	Require(ib.size() == dwords, "PM4 final length");
	Upload(c, c.ib, ib);
	c.expected[0] = seed; c.expected[kBytes / 4 - 1] = seed ^ 0xabcdef01;
	for (uint32 i = 0; i < 1024; i++) c.expected[4092 / 4 + i] = seed ^ (i * multiplier);
}
static amdgpu_gfx_submit SubmitRequest()
{
	auto r = Request<amdgpu_gfx_submit>(); r.address = kIB; r.dwords = 256; return r;
}
static uint32 DrawProgram(Client& c, bool red)
{
	std::vector<uint32> shaders(8192 / 4);
	memcpy(shaders.data(), kTriangleVS, sizeof(kTriangleVS));
	memcpy(shaders.data() + 4096 / 4, kColorPS, sizeof(kColorPS));
	Upload(c, c.shader, shaders);
	c.expected.assign(kBytes / 4, 0xabcddcba);
	Upload(c, c.data, c.expected);
	std::vector<uint32> ib;
	auto emit = [&](uint32 word) { ib.push_back(word); };
	auto context = [&](uint32 index, uint32 value) {
		emit(Packet(0x69, 1)); emit(index - 0xa000); emit(value);
	};
	auto shader = [&](uint32 index, uint32 value) {
		emit(Packet(0x76, 1)); emit(index - 0x2c00); emit(value);
	};
	for (const auto& entry : kDrawContext) {
		// The CP gets its VMID from the kernel's INDIRECT_BUFFER packet.
		if (entry[0] != 0xa0da) context(entry[0], entry[1]);
	}
	const uint64 target = kData + 8192;
	context(0xa318, target >> 8); context(0xa319, 3); context(0xa31a, 15);
	context(0xa31b, 0); context(0xa31c, 10 << 2 | 1 << 7 | 1 << 15); context(0xa31d, 0);
	for (uint32 reg = 0xa31e; reg <= 0xa325; reg++) context(reg, 0);
	for (uint32 target = 1; target < 8; target++) context(0xa31c + target * 15, 0);
	for (uint32 reg = 0xa2fe; reg <= 0xa30d; reg++) context(reg, 0);
	context(0x1000a2aa, 0x2010007f);
	shader(0x2c46, 0xffff);
	shader(0x2c48, kShader >> 8); shader(0x2c49, kShader >> 40);
	shader(0x2c4a, 1 | 1 << 6 | 0xc0 << 12); shader(0x2c4b, 0);
	shader(0x2c07, 0xffff);
	shader(0x2c08, (kShader + 4096) >> 8); shader(0x2c09, (kShader + 4096) >> 40);
	shader(0x2c0a, 1 << 6 | 0xc0 << 12); shader(0x2c0b, 4 << 1);
	shader(0x2c0c, red ? 0x3f800000 : 0); shader(0x2c0d, red ? 0 : 0x3f800000);
	shader(0x2c0e, 0); shader(0x2c0f, 0x3f800000);
	emit(Packet(0x79, 1)); emit(0x10000242); emit(4);
	emit(Packet(0x2f, 0)); emit(1);
	emit(Packet(0x2d, 1)); emit(3); emit(2);
	emit(Packet(0x46, 0)); emit(0x10 | 4 << 8);
	uint32 padding = (-ib.size()) & 255;
	if (padding == 1) padding += 256;
	if (padding != 0) {
		emit(Packet(0x10, padding - 2));
		for (uint32 i = 1; i < padding; i++) emit(0);
	}
	Upload(c, c.ib, ib);
	for (uint32 y = 0; y < 32; y++) {
		for (uint32 x = 0; x < 32; x++) {
			if (x + y <= 31) c.expected[8192 / 4 + y * 32 + x] = red ? 0xff0000ff : 0xff00ff00;
		}
	}
	return ib.size();
}
static void Reject(int fd, amdgpu_gfx_submit r, status_t status)
{
	Require(ioctl(fd, AMDGPU_GFX_SUBMIT, &r, sizeof(r)) == -1 && errno == status,
		"invalid user submission rejected before hardware");
}
static std::vector<uint32> DiagnosticSnapshot(Client& c)
{
	std::vector<uint32> ib;
	for (uint32 i = 0; i < 4096; i += 8) {
		uint64 destination = kData + 32768 + i;
		ib.push_back(Packet(0x40, 4));
		ib.push_back(1 | 5 << 8 | 1 << 16 | 1 << 20); // confirmed 64-bit memory copy
		ib.push_back(0x1000 + i); ib.push_back(0);
		ib.push_back(destination); ib.push_back(destination >> 32);
	}
	Require((ib.size() & 255) == 0, "snapshot IB alignment");
	Upload(c, c.ib, ib);
	auto r = SubmitRequest(); r.dwords = ib.size();
	Require(ioctl(c.fd, AMDGPU_GFX_SUBMIT, &r, sizeof(r)) == 0 && r.status == B_OK
		&& r.completion != 0 && r.vm_fault_status[0] == 0 && r.vm_fault_status[1] == 0
		&& r.rptr == r.wptr, "read own diagnostic command page");
	if (c.data.address == 0) Copy(c.fd, c.data.handle, c.staging.handle);
	volatile uint32* words = (volatile uint32*)(addr_t)(c.data.address ? c.data.address : c.staging.address);
	std::vector<uint32> snapshot(1024);
	for (uint32 i = 0; i < 1024; i++) snapshot[i] = c.expected[32768 / 4 + i] = words[32768 / 4 + i];
	Check(c); // The rest of the data and every guard must remain intact.
	return snapshot;
}
static void DiagnosticWrite(Client& c, uint32 seed)
{
	auto r = Request<amdgpu_vm_test>(); r.address = kData + 4092; r.value = seed;
	Require(ioctl(c.fd, AMDGPU_VM_TEST, &r, sizeof(r)) == 0 && r.status == B_OK
		&& r.completion != 0 && r.vm_fault_status[0] == 0 && r.vm_fault_status[1] == 0
		&& r.rptr == r.wptr, "execute private diagnostic command page");
	for (uint32 i = 0; i < 1024; i++) c.expected[4092 / 4 + i] = seed ^ (i * 0x10204081u);
	Check(c);
}
int main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	int monitor = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
	if (argc == 2 && strcmp(argv[1], "--expect-no-device") == 0) {
		Require(monitor < 0 && errno == ENOENT, "no AMD device in QEMU");
		puts("PASS: no-device user submission handling"); return 0;
	}
	bool draw = argc == 2 && (strcmp(argv[1], "--draw") == 0 || strcmp(argv[1], "--copy-draw") == 0);
	bool copy = argc == 2 && (strcmp(argv[1], "--copy") == 0 || strcmp(argv[1], "--copy-draw") == 0);
	bool privateCommands = argc == 2 && strcmp(argv[1], "--private-commands") == 0;
	Require((argc == 1 || draw || copy || privateCommands) && geteuid() == 0 && monitor >= 0,
		"usage: amdgpu_submit [--draw|--private-commands|--copy|--copy-draw] (root)");
	uint64 vram = Allocated(monitor, false), ram = Allocated(monitor, true);
	Client clients[4];
	for (uint32 i = 0; i < 4; i++) {
		Client& c = clients[i];
		c.fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR); Require(c.fd >= 0, "open client");
		auto vm = Request<amdgpu_vm_info>();
		Require(ioctl(c.fd, AMDGPU_VM_INFO, &vm, sizeof(vm)) == 0, "create VM");
		uint32 kind = i == 3 ? AMDGPU_CREATE_DEVICE_BUFFER
			: (i & 1) ? AMDGPU_CREATE_SYSTEM_BUFFER : AMDGPU_CREATE_BUFFER;
		c.ib = Create(c.fd, kind); c.shader = Create(c.fd, kind); c.data = Create(c.fd, kind);
		c.staging = Create(c.fd, AMDGPU_CREATE_SYSTEM_BUFFER);
		Map(c.fd, c.ib.handle, kIB, AMDGPU_VM_READ | AMDGPU_VM_EXECUTE);
		Map(c.fd, c.shader.handle, kShader, AMDGPU_VM_READ | AMDGPU_VM_EXECUTE);
		Map(c.fd, c.data.handle, kData, AMDGPU_VM_READ | AMDGPU_VM_WRITE);
		c.expected.resize(kBytes / 4); Check(c);
	}
	int fd = clients[0].fd;
	auto r = SubmitRequest();
	Require(ioctl(fd, AMDGPU_GFX_SUBMIT, NULL, sizeof(r)) == -1 && errno == B_BAD_ADDRESS, "null request");
	Require(ioctl(fd, AMDGPU_GFX_SUBMIT, &r, sizeof(r) - 1) == -1 && errno == B_BAD_VALUE, "short request");
	r.version++; Reject(fd, r, B_BAD_VALUE);
	r = SubmitRequest(); r.size--; Reject(fd, r, B_BAD_VALUE);
	r = SubmitRequest(); r.flags = 1; Reject(fd, r, B_BAD_VALUE);
	r = SubmitRequest(); r.reserved = 1; Reject(fd, r, B_BAD_VALUE);
	r = SubmitRequest(); r.address++; Reject(fd, r, B_BAD_VALUE);
	r = SubmitRequest(); r.dwords = 0; Reject(fd, r, B_BAD_VALUE);
	r = SubmitRequest(); r.dwords = 255; Reject(fd, r, B_BAD_VALUE);
	r = SubmitRequest(); r.dwords = 16640; Reject(fd, r, B_BAD_VALUE);
	r = SubmitRequest(); r.address = kData; Reject(fd, r, B_NOT_ALLOWED);
	r = SubmitRequest(); r.address = kIB + kBytes - 256; Reject(fd, r, B_NOT_ALLOWED);
	r = SubmitRequest(); r.address = 0x1000; Reject(fd, r, B_NOT_ALLOWED);
	r = SubmitRequest(); r.address = 1ULL << 36; Reject(fd, r, B_NOT_ALLOWED);
	int readonly = open("/dev/" AMDGPU_DEVICE_NAME, O_RDONLY);
	Require(readonly >= 0, "read-only descriptor"); Reject(readonly, SubmitRequest(), B_NOT_ALLOWED); close(readonly);
	pid_t child = fork(); Require(child >= 0, "fork ownership check");
	if (child == 0) {
		Reject(fd, SubmitRequest(), B_NOT_ALLOWED);
		Require(setgid(65534) == 0 && setuid(65534) == 0, "drop privilege");
		int own = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR); Require(own >= 0, "ordinary client open");
		Reject(own, SubmitRequest(), B_NOT_ALLOWED); close(own); _exit(0);
	}
	int childStatus; Require(waitpid(child, &childStatus, 0) == child && childStatus == 0, "ownership/privilege rejection");
	puts("PASS: ABI, range, execute permission, team ownership and root restriction");
	uint64 last = 0;
	if (privateCommands) {
		std::vector<uint32> zero(1024);
		for (Client& c : clients)
			Require(DiagnosticSnapshot(c) == zero, "fresh private commands are zero");
		DiagnosticWrite(clients[0], 0xabcdef98);
		auto first = DiagnosticSnapshot(clients[0]);
		Require(first != zero, "first client's command page contains its job");
		DiagnosticWrite(clients[1], 0x98765432);
		auto second = DiagnosticSnapshot(clients[1]);
		Require(second != zero && second != first, "clients own distinct diagnostic commands");
		Require(DiagnosticSnapshot(clients[0]) == first, "other client's diagnostic leaves first page unchanged");
		Require(DiagnosticSnapshot(clients[2]) == zero && DiagnosticSnapshot(clients[3]) == zero,
			"unused private command pages stay zero");
		for (Client& c : clients) Check(c);
		puts("PASS: all 4096 diagnostic command bytes are private per client, initially zero and unchanged by other clients");
	}
	for (uint32 round = 0; !privateCommands && round < (draw ? 4u : 8u); round++) {
		for (uint32 i = 0; i < 4; i++) {
			Client& c = clients[i];
			uint32 seed = 0x5100aabb ^ round * 0x123 ^ i * 0x102030;
			uint32 factor = (round & 1) ? 0x01010101 : 0x11223345;
			uint32 dwords = round % 3 == 0 ? 16384 : round % 3 == 1 ? 4096 : 256;
			if (draw) dwords = DrawProgram(c, (round + i) & 1);
			else Program(c, seed, factor, dwords);
			r = SubmitRequest(); r.dwords = dwords;
			if (copy) r.address = c.ib.address ? c.ib.address : c.staging.address;
			Require(ioctl(c.fd, copy ? AMDGPU_GFX_SUBMIT_COPY : AMDGPU_GFX_SUBMIT,
				&r, sizeof(r)) == 0, "submit userspace PM4");
			printf("client %u round %u status %#x fence %llu faults %#x/%#x ring %u/%u us %llu\n",
				(unsigned)i, (unsigned)round, (unsigned)r.status, (unsigned long long)r.completion,
				(unsigned)r.vm_fault_status[0], (unsigned)r.vm_fault_status[1], (unsigned)r.rptr,
				(unsigned)r.wptr, (unsigned long long)r.elapsed_us);
			Require(r.status == B_OK && r.completion > last && r.vm_fault_status[0] == 0
				&& r.vm_fault_status[1] == 0 && r.rptr == r.wptr, "user IB retirement and fault status");
			last = r.completion;
			for (Client& other : clients) Check(other);
		}
	}
	if (!privateCommands) {
		if (copy) puts("Copied command path used for every compute/raster job");
		puts(draw ? "PASS: 16 user raster submissions, four VMs, VRAM/RAM/device-only shaders and targets, exact triangle pixels and complete guards"
			: "PASS: 32 user PM4/shader submissions, shader replacement, 64-KiB IB tails, high VAs, cross-4-GiB stores, four VMs and complete data/guards");
	}
	for (Client& c : clients) {
		close(c.fd);
		for (const amdgpu_buffer* b : {&c.ib, &c.shader, &c.data, &c.staging}) {
			if (b->address != 0) Require(delete_area(b->area) == B_OK, "release revoked CPU clone");
		}
	}
	Require(Allocated(monitor, false) == vram && Allocated(monitor, true) == ram, "all buffers/page tables reclaimed");
	close(monitor); puts("PASS: complete VRAM and RAM reclamation"); return 0;
}
