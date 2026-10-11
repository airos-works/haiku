/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
// Root-only fixed-window GDS qualification. All possible targets are seeded
// driver-owned scratch; ordinary copied submissions retain zero allocation.
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
#include "ShaderGds.h"

static const uint64 kCode = 0x500000000ULL, kOutput = 0x300000000ULL;
static const uint32 kGuard = 0x6b6b6b6b, kDone = 0x06d5c001;
struct Client { int fd; amdgpu_buffer code, output; uint32 marker; };
static uint64 sJobs;
static Client* sClients;
static void Require(bool ok, const char* why)
{
	if (!ok) { fprintf(stderr, "FAIL: %s (%d: %s)\n", why, errno, strerror(errno)); exit(1); }
}
template<typename T> static T Request()
{
	T r = {}; r.version = AMDGPU_HAIKU_ABI_VERSION; r.size = sizeof(r); return r;
}
template<typename T> static T Info(int fd, uint32 op)
{
	T r = Request<T>(); Require(ioctl(fd, op, &r, sizeof(r)) == 0, "read independent state"); return r;
}
static amdgpu_irq_info Healthy(int fd)
{
	auto r = Info<amdgpu_irq_info>(fd, AMDGPU_IRQ_INFO);
	Require(r.enabled && r.msi && r.status == B_OK && r.waits == r.completed_fences
		&& !r.vm_faults && !r.privileged_faults && !r.unknown && !r.overflows
		&& r.rptr == r.wptr, "healthy independent IRQ state"); return r;
}
static void Revoked(int fd)
{
	auto s = Info<amdgpu_shader_memory_info>(fd, AMDGPU_SHADER_MEMORY_INFO);
	Require(s.vmid == 2 && s.selector_before == s.selector_after
		&& !s.gds_base && !s.gds_size && !s.gws && !s.oa, "GDS/GWS/OA revoked after retirement");
}
static amdgpu_buffer Buffer(int fd, uint64 va, uint32 permissions)
{
	auto b = Request<amdgpu_buffer>(); b.bytes = 4096;
	Require(ioctl(fd, AMDGPU_CREATE_SYSTEM_BUFFER, &b, sizeof(b)) == 0, "allocate owned RAM");
	Require(ioctl(fd, AMDGPU_MAP_BUFFER, &b, sizeof(b)) == 0, "map owned CPU RAM");
	auto m = Request<amdgpu_vm_mapping>(); m.handle = b.handle; m.address = va;
	m.bytes = b.bytes; m.permissions = permissions;
	Require(ioctl(fd, AMDGPU_VM_MAP, &m, sizeof(m)) == 0, "map owned GPU page");
	return b;
}
static uint32 Pattern(uint32 seed, uint32 word) { return seed ^ word * 0x01020408u; }
static std::vector<uint32> Commands(uint32 operation, uint32 offset, uint32 m0, uint32 seed, uint32 marker)
{
	std::vector<uint32> w;
	auto reg = [&](uint32 index, uint32 value) {
		w.push_back(0xc0017600); w.push_back(index - 0x2c00); w.push_back(value);
	};
	reg(0x2e04, 0); reg(0x2e05, 0); reg(0x2e06, 0);
	reg(0x2e07, 64); reg(0x2e08, 1); reg(0x2e09, 1);
	reg(0x2e0c, kCode >> 8); reg(0x2e0d, kCode >> 40);
	reg(0x2e12, 1 | 2 << 6 | 0xc0 << 12); // Eight VGPRs, 24 SGPRs, no PRIV.
	reg(0x2e13, 7 << 1 | 1 << 7); // Seven user SGPRs, no trap handler.
	reg(0x2e15, 0); reg(0x2e16, 0xffffffff); reg(0x2e17, 0xffffffff);
	reg(0x2e18, 0); reg(0x2e19, 0xffffffff); reg(0x2e1a, 0xffffffff);
	reg(0x2e40, uint32(kOutput + 1024)); reg(0x2e41, kOutput >> 32);
	reg(0x2e42, seed); reg(0x2e43, operation); reg(0x2e44, offset);
	reg(0x2e45, m0); reg(0x2e46, marker);
	w.insert(w.end(), {0xc0031502, 1, 1, 1, 5, 0xc0004600, 7 | 4 << 8});
	w.resize(256, 0x80000000);
	Require(amdgpu::ValidateGfx8Commands(w.data(), w.size()).error == amdgpu::GfxCommandValidation::OK,
		"GDS outer command policy"); return w;
}
static void Code(const Client& c)
{
	Require(memcmp((void*)(addr_t)c.code.address, kShaderGds, sizeof(kShaderGds)) == 0, "shader unchanged");
	auto p = (volatile uint32*)(addr_t)c.code.address;
	for (uint32 i = sizeof(kShaderGds) / 4; i < 1024; i++) Require(p[i] == kGuard, "shader tail guard");
}
static void Run(Client& c, int mode, uint32 op, uint32 offset, uint32 m0,
	uint32 seed, const uint32 expected[64], bool violation)
{
	Client& peer = sClients[&c == &sClients[0] ? 1 : 0];
	uint32 peerOutput[1024];
	memcpy(peerOutput, (void*)(addr_t)peer.output.address, sizeof(peerOutput));
	auto out = (uint32*)(addr_t)c.output.address;
	for (uint32 i = 0; i < 1024; i++) out[i] = kGuard;
	auto w = Commands(op, offset, m0, seed, c.marker);
	auto r = Request<amdgpu_gfx_submit>(); r.address = (addr_t)w.data(); r.dwords = w.size();
	r.flags = mode < 0 ? AMDGPU_GFX_VALIDATE_COMMANDS : uint32(mode);
	__sync_synchronize();
	Require(ioctl(c.fd, mode < 0 ? AMDGPU_GFX_SUBMIT_COPY : AMDGPU_GFX_GDS_PROBE_COPY,
		&r, sizeof(r)) == 0, "GDS synchronous copied request");
	__sync_synchronize(); sJobs++;
	if (r.status != B_OK || r.vm_fault_status[0] || r.vm_fault_status[1]) {
		printf("GDS terminal result %#x completion %llu VM %#x/%#x ring %u/%u elapsed %llu\n",
			(unsigned)r.status, (unsigned long long)r.completion, r.vm_fault_status[0], r.vm_fault_status[1],
			r.rptr, r.wptr, (unsigned long long)r.elapsed_us);
		Require(false, "GDS job completes; inspect terminal evidence before recovery");
	}
	uint32 trapBefore = out[258], trapAfter = out[259];
	for (uint32 i = 0; i < 1024; i++) {
		uint32 want = kGuard;
		if (i >= 256 && i < 768) {
			uint32 lane = (i - 256) / 8;
			switch (i % 8) {
				case 0: want = c.marker ^ lane; break;
				case 1: want = expected[lane]; break;
				case 2: want = 0; break;
				case 3: want = violation ? 0x100 : 0; break;
				case 4: want = kDone; break;
			}
		}
		if (out[i] != want) {
			fprintf(stderr, "GDS mode %d op %u offset %#x M0 %#x word %u = %#x expected %#x traps %#x/%#x\n",
				mode, op, offset, m0, i, out[i], want, trapBefore, trapAfter);
			Require(false, "all returned data, trap state and output guards");
		}
	}
	Require(memcmp(peerOutput, (void*)(addr_t)peer.output.address, sizeof(peerOutput)) == 0,
		"same-VA peer output unchanged");
	Code(c); Code(peer); Revoked(c.fd); Healthy(c.fd);
}
static void CheckAll(Client& c, const uint32 expected[1024])
{
	for (uint32 offset = 0; offset < 4096; offset += 256)
		Run(c, AMDGPU_GDS_PROBE_FULL, 0, offset, 4096, 0, expected + offset / 4, false);
}
static void ABI(Client& c)
{
	auto w = Commands(0, 0, 4096, 0, c.marker);
	auto r = Request<amdgpu_gfx_submit>(); r.address = (addr_t)w.data(); r.dwords = w.size();
	Require(ioctl(c.fd, AMDGPU_GFX_GDS_PROBE_COPY, NULL, sizeof(r)) < 0 && errno == B_BAD_ADDRESS, "null request rejected");
	for (uint32 variant = 0; variant < 8; variant++) {
		auto bad = r; size_t bytes = sizeof(bad);
		switch (variant) {
			case 0: bad.version++; break; case 1: bad.size--; break;
			case 2: bytes--; break; case 3: bad.flags = AMDGPU_GDS_PROBE_LAST + 1; break;
			case 4: bad.reserved = 1; break; case 5: bad.dwords = 0; break;
			case 6: bad.dwords--; break; case 7: bad.dwords = 32768; break;
		}
		Require(ioctl(c.fd, AMDGPU_GFX_GDS_PROBE_COPY, &bad, bytes) < 0 && errno == B_BAD_VALUE, "invalid diagnostic request rejected");
	}
	for (uint32 mode = 0; mode <= 2; mode++) {
		r.flags = mode; w[0] = 0xc0003f00; // Nested IB is never permitted, in any mode.
		Require(ioctl(c.fd, AMDGPU_GFX_GDS_PROBE_COPY, &r, sizeof(r)) < 0 && errno == B_NOT_ALLOWED, "diagnostic always validates copied commands");
	}
	int ro = open("/dev/" AMDGPU_DEVICE_NAME, O_RDONLY); Require(ro >= 0, "read-only fd");
	Require(ioctl(ro, AMDGPU_GFX_GDS_PROBE_COPY, &r, sizeof(r)) < 0 && errno == B_NOT_ALLOWED, "read-only probe denied"); close(ro);
	pid_t child = fork(); Require(child >= 0, "fork permission check");
	if (child == 0) {
		Require(setgid(65534) == 0 && setuid(65534) == 0, "drop privilege");
		int fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR); Require(fd >= 0, "ordinary fd");
		Require(ioctl(fd, AMDGPU_GFX_GDS_PROBE_COPY, &r, sizeof(r)) < 0 && errno == B_NOT_ALLOWED, "root-only GDS diagnostic");
		close(fd); _exit(0);
	}
	int status; Require(waitpid(child, &status, 0) == child && WIFEXITED(status)
		&& WEXITSTATUS(status) == 0, "privilege child");
}
int main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0); disable_debugger(1);
	bool absent = argc == 2 && strcmp(argv[1], "--expect-no-device") == 0;
	bool controls = argc == 2 && strcmp(argv[1], "--control") == 0;
	bool bounds = argc == 2 && strcmp(argv[1], "--bounds") == 0;
	Require(absent || ((controls || bounds) && geteuid() == 0), "usage: amdgpu_gds --control | --bounds | --expect-no-device");
	for (uint32 op = 0; op < 3; op++) Commands(op, 256, 4096, 0x1234, 0x5678);
	int monitor = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
	if (absent) { Require(monitor < 0 && errno == ENOENT, "no AMD device"); puts("PASS: GDS command policy and no-device handling"); return 0; }
	Require(monitor >= 0, "open monitor"); Info<amdgpu_render_info>(monitor, AMDGPU_RENDER_INFO);
	auto initial = Healthy(monitor);
	auto ram = Info<amdgpu_gart_info>(monitor, AMDGPU_GART_INFO);
	auto vram = Info<amdgpu_memory_info>(monitor, AMDGPU_MEMORY_INFO);
	Client c[2] = {};
	for (uint32 i = 0; i < 2; i++) {
		c[i].fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR); Require(c[i].fd >= 0, "open separate VM");
		Info<amdgpu_vm_info>(c[i].fd, AMDGPU_VM_INFO);
		c[i].code = Buffer(c[i].fd, kCode, AMDGPU_VM_READ | AMDGPU_VM_EXECUTE);
		c[i].output = Buffer(c[i].fd, kOutput, AMDGPU_VM_READ | AMDGPU_VM_WRITE);
		c[i].marker = 0x6d510000 | i;
		for (uint32 j = 0; j < 1024; j++) {
			((uint32*)(addr_t)c[i].code.address)[j] = kGuard;
			((uint32*)(addr_t)c[i].output.address)[j] = kGuard;
		}
		memcpy((void*)(addr_t)c[i].code.address, kShaderGds, sizeof(kShaderGds));
	}
	sClients = c;
	ABI(c[0]); Require(Healthy(monitor).completed_fences == initial.completed_fences, "ABI rejects before GPU execution");
	uint32 expected[1024]; const uint32 seed = 0x6d500001, changed = 0x3a790001;
	for (uint32 i = 0; i < 1024; i++) expected[i] = Pattern(seed, i);
	for (uint32 offset = 0; offset < 4096; offset += 256)
		Run(c[0], AMDGPU_GDS_PROBE_FULL, 1, offset, 4096, seed, expected + offset / 4, false);
	CheckAll(c[1], expected);
	puts("PASS: seeded and independently read all 4096 GDS bytes across separate client VMs");
	Run(c[0], AMDGPU_GDS_PROBE_SLICE, 0, 0, 256, 0, expected + 256, false);
	for (uint32 i = 0; i < 64; i++) expected[256 + i] = Pattern(changed, i);
	Run(c[0], AMDGPU_GDS_PROBE_SLICE, 1, 0, 256, changed, expected + 256, false);
	Run(c[1], AMDGPU_GDS_PROBE_SLICE, 2, 0, 256, 0, expected + 256, false);
	for (uint32 i = 0; i < 64; i++) expected[256 + i] += 0x101;
	Run(c[1], AMDGPU_GDS_PROBE_FULL, 0, 0, (1024 << 16) | 256, 0, expected + 256, false);
	CheckAll(c[0], expected);
	puts("PASS: GDS slice base, write/read/atomic positive controls and full surrounding guards");
	if (bounds) {
		struct Case { int mode; uint32 offset, m0; };
		const Case cases[] = {
			{AMDGPU_GDS_PROBE_SLICE, 256, 256},
			{AMDGPU_GDS_PROBE_SLICE, 0, 4096},
			{AMDGPU_GDS_PROBE_SLICE, 0, (256 << 16) | 256},
			{AMDGPU_GDS_PROBE_NONE, 0, 4096},
			{-1, 0, 4096}, {-1, 0, (1024 << 16) | 256}
		};
		uint32 zero[64] = {};
		for (const auto& test : cases) {
			for (uint32 op = 0; op < 3; op++) {
				Run(c[1], test.mode, op, test.offset, test.m0, 0xf17e0001, zero, true);
				CheckAll(c[0], expected); Code(c[1]);
				printf("PASS: GDS mode %d op %u offset %#x M0 %#x denied; all 4096 bytes unchanged\n",
					test.mode, op, test.offset, test.m0);
			}
		}
	}
	for (auto& client : c) {
		Code(client); close(client.fd);
		Require(delete_area(client.code.area) == B_OK && delete_area(client.output.area) == B_OK, "release revoked clones");
	}
	Require(Info<amdgpu_gart_info>(monitor, AMDGPU_GART_INFO).allocated_bytes == ram.allocated_bytes
		&& Info<amdgpu_memory_info>(monitor, AMDGPU_MEMORY_INFO).allocated_bytes == vram.allocated_bytes, "complete memory reclamation");
	auto final = Healthy(monitor); Require(final.completed_fences == initial.completed_fences + sJobs, "exact GDS completion count");
	Revoked(monitor); close(monitor);
	printf("PASS: GDS %s, %llu checked jobs, 2 VMs, ABI, revoke, all data/guards and reclamation\n",
		bounds ? "bounds" : "positive controls", (unsigned long long)sJobs);
}
