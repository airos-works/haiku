/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
// Root-only hardware qualification. A pass is not a general shader validator.
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
#include "ShaderPrivilege.h"

static const uint64 kCode = 0x500000000ULL, kOutput = 0x300000000ULL;
static const uint32 kBytes = 131072, kGuard = 0x6b6b6b6b;
using Words = std::vector<uint32>;
struct Client {
	int fd;
	amdgpu_buffer code, output;
	uint32 seed;
};
static void Require(bool okay, const char* why)
{
	if (!okay) { fprintf(stderr, "FAIL: %s (%d: %s)\n", why, errno, strerror(errno)); exit(1); }
}
template<typename T> static T Request()
{
	T r = {}; r.version = AMDGPU_HAIKU_ABI_VERSION; r.size = sizeof(r); return r;
}
static amdgpu_irq_info IRQ(int fd)
{
	auto r = Request<amdgpu_irq_info>();
	Require(ioctl(fd, AMDGPU_IRQ_INFO, &r, sizeof(r)) == 0 && r.enabled && r.msi
		&& r.status == B_OK && !r.vm_faults && !r.privileged_faults && !r.unknown
		&& !r.overflows && r.rptr == r.wptr && r.waits == r.completed_fences,
		"healthy independent IRQ counters");
	return r;
}
static amdgpu_buffer Buffer(int fd, uint64 address, uint32 bytes, uint32 permissions)
{
	auto b = Request<amdgpu_buffer>(); b.bytes = bytes;
	Require(ioctl(fd, AMDGPU_CREATE_SYSTEM_BUFFER, &b, sizeof(b)) == 0
		&& ioctl(fd, AMDGPU_MAP_BUFFER, &b, sizeof(b)) == 0, "allocate owned CPU-visible RAM");
	auto m = Request<amdgpu_vm_mapping>(); m.handle = b.handle; m.address = address;
	m.bytes = bytes; m.permissions = permissions;
	Require(ioctl(fd, AMDGPU_VM_MAP, &m, sizeof(m)) == 0, "map owned GPU memory");
	return b;
}
static Words Commands(uint32 seed)
{
	Words w;
	auto reg = [&](uint32 index, uint32 value) {
		w.push_back(0xc0017600); w.push_back(index - 0x2c00); w.push_back(value);
	};
	reg(0x2e04, 0); reg(0x2e05, 0); reg(0x2e06, 0);
	reg(0x2e07, 64); reg(0x2e08, 1); reg(0x2e09, 1);
	reg(0x2e0c, kCode >> 8); reg(0x2e0d, kCode >> 40);
	reg(0x2e12, 4 | 1 << 6 | 0xc0 << 12); // 20 VGPRs, 16 SGPRs; PRIV clear.
	reg(0x2e13, 3 << 1 | 1 << 7); // Three user SGPRs plus workgroup ID X.
	reg(0x2e15, 0); reg(0x2e16, 0xffffffff); reg(0x2e17, 0xffffffff);
	reg(0x2e18, 0); reg(0x2e19, 0xffffffff); reg(0x2e1a, 0xffffffff);
	reg(0x2e40, uint32(kOutput + 4096)); reg(0x2e41, kOutput >> 32); reg(0x2e42, seed);
	w.insert(w.end(), {0xc0031502, 16, 1, 1, 5, 0xc0004600, 7 | 4 << 8});
	w.resize(256, 0x80000000);
	Require(amdgpu::ValidateGfx8Commands(w.data(), w.size()).error == amdgpu::GfxCommandValidation::OK,
		"shader probe commands pass the checked policy");
	return w;
}
static void Check(const Client& c)
{
	auto p = (volatile uint32*)(addr_t)c.output.address;
	for (uint32 i = 0; i < kBytes / 4; i++) {
		uint32 want = kGuard;
		if (c.seed && i >= 1024 && i < 1024 + 16384) {
			uint32 lane = (i - 1024) / 16;
			const uint32 values[16] = {
				c.seed ^ lane, 0, 0, 0, // Marker and three PRIV observations.
				0, 1, 3, 0, // MODE initial, two positive writes, restoration.
				0, 0x13579bdf, 0, // Trap difference mask, M0 control/restoration.
				lane / 64, lane % 64, c.seed, 0x51005100, kGuard
			};
			want = values[(i - 1024) % 16];
		}
		if (p[i] != want) {
			fprintf(stderr, "output word %u, record %u field %u: %#x expected %#x\n",
				i, i >= 1024 ? (i - 1024) / 16 : 0, i % 16, (unsigned)p[i], want);
			Require(false, "all privilege observations, positive controls and guards match");
		}
	}
	Require(memcmp((void*)(addr_t)c.code.address, kShaderPrivilege, sizeof(kShaderPrivilege)) == 0,
		"shader instructions remain unchanged");
}
int main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0); disable_debugger(1);
	Commands(0x12345678);
	if (argc == 2 && strcmp(argv[1], "--expect-no-device") == 0) {
		int fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
		Require(fd < 0 && errno == ENOENT, "no AMD device in QEMU");
		puts("PASS: shader privilege command policy and no-device handling"); return 0;
	}
	Require(argc == 1 && geteuid() == 0, "shader privilege qualification requires root");
	int monitor = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR); Require(monitor >= 0, "open monitor");
	auto render = Request<amdgpu_render_info>();
	Require(ioctl(monitor, AMDGPU_RENDER_INFO, &render, sizeof(render)) == 0
		&& (render.capabilities & AMDGPU_RENDER_CHECKED_COPY), "checked copy capability");
	auto initial = IRQ(monitor);
	auto memory = Request<amdgpu_gart_info>();
	Require(ioctl(monitor, AMDGPU_GART_INFO, &memory, sizeof(memory)) == 0, "initial RAM accounting");
	auto vram = Request<amdgpu_memory_info>();
	Require(ioctl(monitor, AMDGPU_MEMORY_INFO, &vram, sizeof(vram)) == 0, "initial VRAM accounting");
	Client clients[4] = {};
	for (auto& c : clients) {
		c.fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR); Require(c.fd >= 0, "open independent VM");
		auto vm = Request<amdgpu_vm_info>();
		Require(ioctl(c.fd, AMDGPU_VM_INFO, &vm, sizeof(vm)) == 0, "create client VM");
		c.code = Buffer(c.fd, kCode, 4096, AMDGPU_VM_READ | AMDGPU_VM_EXECUTE);
		c.output = Buffer(c.fd, kOutput, kBytes, AMDGPU_VM_READ | AMDGPU_VM_WRITE);
		memcpy((void*)(addr_t)c.code.address, kShaderPrivilege, sizeof(kShaderPrivilege));
		auto p = (uint32*)(addr_t)c.output.address;
		for (uint32 i = 0; i < kBytes / 4; i++) p[i] = kGuard;
	}
	__sync_synchronize();
	for (unsigned round = 0; round < 4; round++) {
		for (unsigned client = 0; client < 4; client++) {
			auto& c = clients[client]; c.seed = 0x51000000 | round << 8 | client;
			auto words = Commands(c.seed);
			auto r = Request<amdgpu_gfx_submit>(); r.address = (addr_t)words.data();
			r.dwords = words.size(); r.flags = AMDGPU_GFX_VALIDATE_COMMANDS;
			Require(ioctl(c.fd, AMDGPU_GFX_SUBMIT_COPY, &r, sizeof(r)) == 0
				&& r.status == B_OK && r.completion && !r.vm_fault_status[0]
				&& !r.vm_fault_status[1] && r.rptr == r.wptr, "checked shader completes");
			__sync_synchronize();
			for (const auto& peer : clients) Check(peer);
			printf("PASS: round %u VM %u, 1024 records, PRIV stays zero, 16 trap registers unchanged,"
				" MODE/M0 controls and all same-VA peer guards match\n", round, client);
		}
	}
	for (auto& c : clients) {
		close(c.fd);
		Require(delete_area(c.code.area) == B_OK && delete_area(c.output.area) == B_OK,
			"release revoked CPU clones");
	}
	auto afterMemory = Request<amdgpu_gart_info>();
	auto afterVRAM = Request<amdgpu_memory_info>();
	Require(ioctl(monitor, AMDGPU_GART_INFO, &afterMemory, sizeof(afterMemory)) == 0
		&& ioctl(monitor, AMDGPU_MEMORY_INFO, &afterVRAM, sizeof(afterVRAM)) == 0
		&& memory.allocated_bytes == afterMemory.allocated_bytes
		&& vram.allocated_bytes == afterVRAM.allocated_bytes, "complete RAM/VRAM reclamation");
	auto final = IRQ(monitor);
	Require(final.completed_fences == initial.completed_fences + 16
		&& final.eop_events == initial.eop_events + 32, "exact independent job IRQ counts");
	printf("PASS: 16 checked shaders, 256 waves, 16384 lane records; completion %llu -> %llu, no faults\n",
		(unsigned long long)initial.completed_fences, (unsigned long long)final.completed_fences);
	close(monitor);
}
