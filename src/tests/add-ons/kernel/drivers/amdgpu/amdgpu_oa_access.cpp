/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
// Ordered-append controls: one wave, trusted wave-ID reset. Seed and
// independently check all 4 KiB of owned GDS around every counter target.
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
#include "ShaderOaAccess.h"

static const uint64 kCode = 0x500000000ULL, kOutput = 0x300000000ULL;
static const uint32 kGuard = 0x6b6b6b6b, kDone = 0x0a0dc001;
struct Client {
	int fd;
	amdgpu_buffer code, output;
	uint32 marker;
	uint32 shader[sizeof(kShaderOaAccess) / sizeof(uint32)];
};
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
	Require(memcmp((void*)(addr_t)c.code.address, c.shader, sizeof(c.shader)) == 0, "shader unchanged");
	auto p = (volatile uint32*)(addr_t)c.code.address;
	for (uint32 i = sizeof(kShaderOaAccess) / 4; i < 1024; i++) Require(p[i] == kGuard, "shader tail guard");
}
static void OrderedShader(Client& c, uint32 index, bool swap)
{
	Code(c);
	// LLVM gfx803 encoding at byte 0x8c: index in offset[7:2], swap in
	// offset[12]. Keep CS type, wave release/done and register operands fixed.
	Require(index < 64 && kShaderOaAccess[35] == 0xd97f0300
		&& kShaderOaAccess[36] == 0x04000004, "fixed ordered instruction encoding");
	c.shader[35] = 0xd97f0300 | index << 2 | (swap ? 0x1000 : 0);
	memcpy((void*)(addr_t)c.code.address, c.shader, sizeof(c.shader));
}
static void Run(Client& c, int mode, uint32 op, uint32 offset, uint32 m0,
	uint32 seed, const uint32 expected[64], bool violation, uint32* observed = NULL,
	uint32* observedTrap = NULL)
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
	if (mode >= AMDGPU_GDS_PROBE_OA_RESET || op == 2)
		printf("OA access mode %d op %u M0 %#x result %#x completion %llu VM %#x/%#x first data/traps %#x/%#x/%#x\n",
			mode, op, m0, (unsigned)r.status, (unsigned long long)r.completion,
			r.vm_fault_status[0], r.vm_fault_status[1], out[257], out[258], out[259]);
	if (r.status != B_OK || r.vm_fault_status[0] || r.vm_fault_status[1]) {
		printf("GDS terminal result %#x completion %llu VM %#x/%#x ring %u/%u elapsed %llu\n",
			(unsigned)r.status, (unsigned long long)r.completion, r.vm_fault_status[0], r.vm_fault_status[1],
			r.rptr, r.wptr, (unsigned long long)r.elapsed_us);
		Require(false, "GDS job completes; inspect terminal evidence before recovery");
	}
	uint32 trapBefore = out[258], trapAfter = out[259];
	if (observedTrap != NULL) *observedTrap = trapAfter;
	for (uint32 i = 0; i < 1024; i++) {
		uint32 want = kGuard;
		if (i >= 256 && i < 768) {
			uint32 lane = (i - 256) / 8;
			switch (i % 8) {
				case 0: want = c.marker ^ lane; break;
				case 1:
					if (observed != NULL) observed[lane] = out[i];
					want = expected != NULL ? expected[lane] : out[i];
					break;
				case 2: want = 0; break;
				case 3: want = observedTrap != NULL ? trapAfter : violation ? 0x100 : 0; break;
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
static void Ordered(Client& c, Client& reader, int mode, uint32 index,
	uint32 address, bool swap, uint32 value, uint32 expected[1024], bool denied,
	bool observePermission = false, uint32 gdsBase = 0)
{
	Require(address % 4 == 0 && gdsBase + address * 4 + index * 4 < 4096,
		"every possible OA target is inside owned seeded GDS");
	OrderedShader(c, index, swap);
	auto before = Info<amdgpu_oa_info>(c.fd, AMDGPU_OA_INFO);
	if (mode < 0 || mode <= AMDGPU_GDS_PROBE_NONE) {
		// Previous ordered instructions can advance the GDS wave crawler.
		// Synchronize to the fixture's
		// M0 wave ID 0 before ordinary submission. This separate trusted job
		// grants no resources and executes only the already-tested GDS read.
		// Without the reset, an old wave ID can wait before permission checks.
		uint32 zero[64] = {};
		Run(reader, AMDGPU_GDS_PROBE_OA_RESET, 0, 0, 4096, 0, zero, true);
	}
	uint32 returned[64], actual[1024], trap;
	Run(c, mode, 2, 0, address << 16, value, NULL, denied, returned, &trap);
	// Always gather the complete owned region before judging address semantics.
	for (uint32 offset = 0; offset < 4096; offset += 256)
		Run(reader, AMDGPU_GDS_PROBE_FULL, 0, offset, 4096, 0, NULL,
			false, actual + offset / 4);
	for (uint32 i = 0; i < 1024; i++) {
		if (actual[i] != expected[i])
			printf("OA %s mode %d index %u address %#x changed GDS byte %#x: %#x -> %#x\n",
				swap ? "swap" : "add", mode, index, address, i * 4, expected[i], actual[i]);
	}
	uint32 word = gdsBase / 4 + address / 4 + index;
	uint32 old = expected[word];
	if (observePermission) {
		Require(trap == 0 || trap == 0x100, "observed OA exception is success or memory violation");
		denied = trap == 0x100;
	}
	Require(trap == (denied ? 0x100 : 0), "ordered instruction exception state");
	if (!denied) expected[word] = swap ? value : old + value;
	for (uint32 lane = 0; lane < 64; lane++)
		Require(returned[lane] == (denied ? 0 : old), "ordered result in every lane");
	Require(memcmp(actual, expected, sizeof(actual)) == 0, "exact counter target and all GDS guards");
	auto after = Info<amdgpu_oa_info>(c.fd, AMDGPU_OA_INFO);
	Require(before.selector_before == before.selector_after
		&& after.selector_before == after.selector_after
		&& before.max_wave_id == after.max_wave_id
		&& memcmp(before.counters, after.counters, sizeof(before.counters)) == 0
		&& memcmp(before.addresses, after.addresses, sizeof(before.addresses)) == 0
		&& memcmp(before.ring_sizes, after.ring_sizes, sizeof(before.ring_sizes)) == 0,
		"all OA resource state unchanged after completed one-wave operation");
	printf("PASS: OA %s mode %d index %u address %#x %s, all owned GDS and lane results\n",
		swap ? "swap" : "add", mode, index, address, denied ? "denied" : "allowed");
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
	for (uint32 mode = 0; mode <= AMDGPU_GDS_PROBE_LAST; mode++) {
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
	bool addressControl = argc == 2 && strcmp(argv[1], "--address-control") == 0;
	bool extended = argc == 2 && strcmp(argv[1], "--extended") == 0;
	bool bounds = argc == 2 && strcmp(argv[1], "--bounds") == 0;
	bool maskObserve = argc == 2 && strcmp(argv[1], "--mask-observe") == 0;
	bool gdsWindow = argc == 2 && strcmp(argv[1], "--gds-window") == 0;
	Require(absent || ((controls || addressControl || extended || bounds || maskObserve || gdsWindow) && geteuid() == 0),
		"usage: amdgpu_oa_access --control | --address-control | --extended | --bounds | --mask-observe | --gds-window | --expect-no-device");
	for (uint32 op = 0; op < 3; op++) Commands(op, 256, 4096, 0x1234, 0x5678);
	int monitor = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
	if (absent) { Require(monitor < 0 && errno == ENOENT, "no AMD device"); puts("PASS: OA access command policy and no-device handling"); return 0; }
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
		memcpy(c[i].shader, kShaderOaAccess, sizeof(kShaderOaAccess));
		memcpy((void*)(addr_t)c[i].code.address, c[i].shader, sizeof(c[i].shader));
	}
	sClients = c;
	ABI(c[0]); Require(Healthy(monitor).completed_fences == initial.completed_fences, "ABI rejects before GPU execution");
	uint32 expected[1024]; const uint32 seed = 0x0a510001;
	for (uint32 i = 0; i < 1024; i++) expected[i] = Pattern(seed, i);
	for (uint32 offset = 0; offset < 4096; offset += 256)
		Run(c[0], AMDGPU_GDS_PROBE_FULL, 1, offset, 4096, seed, expected + offset / 4, false);
	CheckAll(c[1], expected);
	puts("PASS: seeded and read all 4096 owned GDS bytes across two client VMs");
	auto oaBefore = Info<amdgpu_oa_info>(monitor, AMDGPU_OA_INFO);
	auto gwsBefore = Info<amdgpu_gws_info>(monitor, AMDGPU_GWS_INFO);
	for (uint32 i = 0; i < 16; i++)
		Require(oaBefore.counters[i] == 0 && oaBefore.addresses[i] == 0
			&& oaBefore.ring_sizes[i] == 0, "OA state matches idle prerequisite snapshot");
	if (controls)
		Ordered(c[1], c[0], AMDGPU_GDS_PROBE_OA_FULL, 0, 0, false, 5, expected, false);
	if (addressControl) {
		Ordered(c[1], c[0], AMDGPU_GDS_PROBE_OA_FULL, 1, 0, false, 7, expected, false);
		Ordered(c[0], c[1], AMDGPU_GDS_PROBE_OA_FULL, 0, 4, false, 9, expected, false);
		Ordered(c[1], c[0], AMDGPU_GDS_PROBE_OA_FULL, 1, 4, true, 0x12345678, expected, false);
	}
	if (extended) {
		for (uint32 swap = 0; swap < 2; swap++) {
			for (uint32 index = 0; index < 16; index++)
				Ordered(c[index % 2], c[1 - index % 2], AMDGPU_GDS_PROBE_OA_FULL,
					index, 256, swap, 0x200 + index, expected, false);
			for (uint32 index = 4; index <= 5; index++)
				Ordered(c[index % 2], c[1 - index % 2], AMDGPU_GDS_PROBE_OA_SLICE,
					index, 256, swap, 0x400 + index, expected, false);
		}
	}
	if (bounds) {
		for (uint32 swap = 0; swap < 2; swap++) {
			for (int mode : {6, 7, 8, 9, -1}) {
				for (uint32 index : {0u, 4u, 16u, 20u, 63u})
					Ordered(c[index % 2], c[1 - index % 2], mode, index, 256,
						swap, 0x700 + index, expected, mode == 9 || mode < 0);
			}
		}
	}
	if (maskObserve) {
		// This records the hardware permission mapping; it does not presume
		// the instruction's six-bit index selects the 16-bit OA resource mask.
		for (uint32 swap = 0; swap < 2; swap++) {
			for (int mode : {6, 7, 8, 9, -1}) {
				for (uint32 index : {0u, 4u, 16u, 20u, 63u})
					Ordered(c[index % 2], c[1 - index % 2], mode, index, 256,
						swap, 0x700 + index, expected, false, true);
			}
		}
	}
	if (gdsWindow) {
		for (uint32 swap = 0; swap < 2; swap++) {
			Ordered(c[0], c[1], AMDGPU_GDS_PROBE_FULL, 63, 256, swap, 11, expected, false);
			Ordered(c[1], c[0], AMDGPU_GDS_PROBE_SLICE, 0, 0, swap, 13, expected, false, false, 1024);
			Ordered(c[0], c[1], AMDGPU_GDS_PROBE_SLICE, 63, 0, swap, 17, expected, false, false, 1024);
			Ordered(c[1], c[0], AMDGPU_GDS_PROBE_SLICE, 0, 252, swap, 19, expected, false, false, 1024);
			Ordered(c[0], c[1], AMDGPU_GDS_PROBE_SLICE, 31, 128, swap, 23, expected, false, false, 1024);
			Ordered(c[1], c[0], AMDGPU_GDS_PROBE_SLICE, 1, 252, swap, 29, expected, true, false, 1024);
			Ordered(c[0], c[1], AMDGPU_GDS_PROBE_SLICE, 0, 256, swap, 31, expected, true, false, 1024);
			Ordered(c[1], c[0], AMDGPU_GDS_PROBE_SLICE, 63, 256, swap, 37, expected, true, false, 1024);
			Ordered(c[0], c[1], AMDGPU_GDS_PROBE_NONE, 0, 0, swap, 41, expected, true);
			Ordered(c[1], c[0], AMDGPU_GDS_PROBE_NONE, 63, 256, swap, 43, expected, true);
		}
	}
	auto oaAfter = Info<amdgpu_oa_info>(monitor, AMDGPU_OA_INFO);
	auto gwsAfter = Info<amdgpu_gws_info>(monitor, AMDGPU_GWS_INFO);
	Require(oaBefore.selector_before == oaBefore.selector_after
		&& oaAfter.selector_before == oaAfter.selector_after
		&& oaBefore.max_wave_id == oaAfter.max_wave_id, "OA selector and inherited max wave ID preserved");
	for (uint32 i = 0; i < 16; i++)
		printf("OA %u counter %#x/%#x address %#x/%#x ring %#x/%#x\n", i,
			oaBefore.counters[i], oaAfter.counters[i], oaBefore.addresses[i], oaAfter.addresses[i],
			oaBefore.ring_sizes[i], oaAfter.ring_sizes[i]);
	Require(memcmp(gwsBefore.resources, gwsAfter.resources, sizeof(gwsBefore.resources)) == 0,
		"all64 GWS states unchanged");
	puts("PASS: ordered operations preserve every GDS guard and all GWS resource states");
	for (auto& client : c) {
		Code(client); close(client.fd);
		Require(delete_area(client.code.area) == B_OK && delete_area(client.output.area) == B_OK, "release revoked clones");
	}
	Require(Info<amdgpu_gart_info>(monitor, AMDGPU_GART_INFO).allocated_bytes == ram.allocated_bytes
		&& Info<amdgpu_memory_info>(monitor, AMDGPU_MEMORY_INFO).allocated_bytes == vram.allocated_bytes, "complete memory reclamation");
	auto final = Healthy(monitor); Require(final.completed_fences == initial.completed_fences + sJobs, "exact GDS completion count");
	Revoked(monitor); close(monitor);
	printf("PASS: OA ordered access, %llu checked jobs, 2 VMs, all GDS/resource/VM guards, revoke and reclamation\n",
		(unsigned long long)sJobs);
}
