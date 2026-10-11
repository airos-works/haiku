/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
// Ordered-append prerequisite: fixed state readback and trusted same-value
// wave-ID reset/lease tests with a marker-only shader. No OA instruction yet.
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
#include "ShaderOa.h"

static const uint64 kCode = 0x500000000ULL, kOutput = 0x300000000ULL;
static const uint32 kGuard = 0x6b6b6b6b, kDone = 0x0a0c0ffe;
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
static std::vector<uint32> Commands(uint32 marker)
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
	reg(0x2e42, 0); reg(0x2e43, 0); reg(0x2e44, 0);
	reg(0x2e45, 0); reg(0x2e46, marker);
	w.insert(w.end(), {0xc0031502, 1, 1, 1, 5, 0xc0004600, 7 | 4 << 8});
	w.resize(256, 0x80000000);
	Require(amdgpu::ValidateGfx8Commands(w.data(), w.size()).error == amdgpu::GfxCommandValidation::OK,
		"OA prerequisite outer command policy"); return w;
}
static void Code(const Client& c)
{
	Require(memcmp((void*)(addr_t)c.code.address, kShaderOa, sizeof(kShaderOa)) == 0, "shader unchanged");
	auto p = (volatile uint32*)(addr_t)c.code.address;
	for (uint32 i = sizeof(kShaderOa) / 4; i < 1024; i++) Require(p[i] == kGuard, "shader tail guard");
}
static amdgpu_oa_info Snapshot(int fd, bool print = false)
{
	auto r = Info<amdgpu_oa_info>(fd, AMDGPU_OA_INFO);
	Require(r.resource_count == 16 && r.selector_before == r.selector_after,
		"all OA resources and restored selector");
	if (print) {
		printf("OA selectors %#x/%#x VMID0/2 %#x/%#x GDS %#x/%#x max_wave_id %#x\n",
			r.selector_before, r.selector_after, r.oa_vmid0, r.oa_vmid2,
			r.gds_base, r.gds_size, r.max_wave_id);
		for (uint32 i = 0; i < 16; i++)
			printf("OA %u counter %#x address %#x ring_size %#x\n", i,
				r.counters[i], r.addresses[i], r.ring_sizes[i]);
	}
	return r;
}
static void Run(Client& c, uint32 mode)
{
	Client& peer = sClients[&c == &sClients[0] ? 1 : 0];
	uint32 peerOutput[1024];
	memcpy(peerOutput, (void*)(addr_t)peer.output.address, sizeof(peerOutput));
	auto before = Snapshot(c.fd, sJobs == 0);
	auto gwsBefore = Info<amdgpu_gws_info>(c.fd, AMDGPU_GWS_INFO);
	auto out = (uint32*)(addr_t)c.output.address;
	for (uint32 i = 0; i < 1024; i++) out[i] = kGuard;
	auto w = Commands(c.marker);
	auto r = Request<amdgpu_gfx_submit>(); r.address = (addr_t)w.data(); r.dwords = w.size(); r.flags = mode;
	__sync_synchronize();
	Require(ioctl(c.fd, AMDGPU_GFX_GDS_PROBE_COPY, &r, sizeof(r)) == 0, "OA prerequisite copied request");
	__sync_synchronize(); sJobs++;
	printf("OA prerequisite job %llu mode %u result %#x completion %llu VM %#x/%#x ring %u/%u\n",
		(unsigned long long)sJobs, mode, (unsigned)r.status, (unsigned long long)r.completion,
		r.vm_fault_status[0], r.vm_fault_status[1], r.rptr, r.wptr);
	Require(r.status == B_OK && !r.vm_fault_status[0] && !r.vm_fault_status[1],
		"reset-prefix job retires; inspect terminal evidence before recovery");
	auto after = Snapshot(c.fd);
	auto gwsAfter = Info<amdgpu_gws_info>(c.fd, AMDGPU_GWS_INFO);
	Require(before.max_wave_id == after.max_wave_id && before.oa_vmid0 == after.oa_vmid0,
		"reset preserves inherited capacity and max wave-ID value");
	Require(memcmp(before.counters, after.counters, sizeof(before.counters)) == 0
		&& memcmp(before.addresses, after.addresses, sizeof(before.addresses)) == 0
		&& memcmp(before.ring_sizes, after.ring_sizes, sizeof(before.ring_sizes)) == 0,
		"all 16 OA resource states unchanged");
	Require(gwsBefore.selector_before == gwsBefore.selector_after
		&& gwsAfter.selector_before == gwsAfter.selector_after
		&& memcmp(gwsBefore.resources, gwsAfter.resources, sizeof(gwsBefore.resources)) == 0,
		"all 64 GWS resource states unchanged");
	for (uint32 i = 0; i < 1024; i++) {
		uint32 want = kGuard;
		if (i >= 256 && i < 768) {
			uint32 lane = (i - 256) / 8;
			switch (i % 8) {
				case 0: want = c.marker ^ lane; break;
				case 1: want = 0x0a0c0001; break;
				case 2: case 3: want = 0; break;
				case 4: want = kDone; break;
			}
		}
		if (out[i] != want) {
			fprintf(stderr, "OA word %u = %#x expected %#x\n", i, out[i], want);
			Require(false, "marker, exception fields and every output guard");
		}
	}
	Require(memcmp(peerOutput, (void*)(addr_t)peer.output.address, sizeof(peerOutput)) == 0,
		"same-VA peer output unchanged");
	Code(c); Code(peer); Revoked(c.fd); Healthy(c.fd);
}
static void ABI(Client& c)
{
	auto info = Request<amdgpu_oa_info>();
	Require(ioctl(c.fd, AMDGPU_OA_INFO, NULL, sizeof(info)) < 0
		&& errno == B_BAD_ADDRESS, "null OA snapshot rejected");
	for (uint32 variant = 0; variant < 5; variant++) {
		auto bad = info; size_t bytes = sizeof(bad);
		switch (variant) {
			case 0: bad.version++; break; case 1: bad.size--; break;
			case 2: bytes--; break; case 3: bad.flags = 1; break;
			case 4: bad.reserved = 1; break;
		}
		Require(ioctl(c.fd, AMDGPU_OA_INFO, &bad, bytes) < 0
			&& errno == B_BAD_VALUE, "invalid OA snapshot rejected");
	}
	auto w = Commands(c.marker);
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
	Require(ioctl(ro, AMDGPU_GFX_GDS_PROBE_COPY, &r, sizeof(r)) < 0 && errno == B_NOT_ALLOWED, "read-only probe denied");
	Require(ioctl(ro, AMDGPU_OA_INFO, &info, sizeof(info)) < 0
		&& errno == B_NOT_ALLOWED, "read-only OA snapshot denied"); close(ro);
	pid_t child = fork(); Require(child >= 0, "fork permission check");
	if (child == 0) {
		Require(setgid(65534) == 0 && setuid(65534) == 0, "drop privilege");
		int fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR); Require(fd >= 0, "ordinary fd");
		Require(ioctl(fd, AMDGPU_GFX_GDS_PROBE_COPY, &r, sizeof(r)) < 0 && errno == B_NOT_ALLOWED, "root-only OA diagnostic");
		Require(ioctl(fd, AMDGPU_OA_INFO, &info, sizeof(info)) < 0
			&& errno == B_NOT_ALLOWED, "root-only OA snapshot");
		close(fd); _exit(0);
	}
	int status; Require(waitpid(child, &status, 0) == child && WIFEXITED(status)
		&& WEXITSTATUS(status) == 0, "privilege child");
}
int main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0); disable_debugger(1);
	bool absent = argc == 2 && strcmp(argv[1], "--expect-no-device") == 0;
	bool reset = argc == 2 && strcmp(argv[1], "--reset") == 0;
	bool leases = argc == 2 && strcmp(argv[1], "--leases") == 0;
	bool snapshot = argc == 2 && strcmp(argv[1], "--snapshot-only") == 0;
	Require(absent || ((reset || leases || snapshot) && geteuid() == 0), "usage: amdgpu_oa --reset | --leases | --snapshot-only | --expect-no-device");
	static_assert(sizeof(amdgpu_oa_info) == 240, "OA ABI");
	Commands(0x5678);
	int monitor = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
	if (absent) { Require(monitor < 0 && errno == ENOENT, "no AMD device"); puts("PASS: OA prerequisite command policy and no-device handling"); return 0; }
	Require(monitor >= 0, "open monitor"); Info<amdgpu_render_info>(monitor, AMDGPU_RENDER_INFO);
	if (snapshot) { Snapshot(monitor, true); close(monitor); return 0; }
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
		memcpy((void*)(addr_t)c[i].code.address, kShaderOa, sizeof(kShaderOa));
	}
	sClients = c;
	ABI(c[0]); Require(Healthy(monitor).completed_fences == initial.completed_fences, "ABI rejects before GPU execution");
	for (uint32 mode = AMDGPU_GDS_PROBE_OA_RESET;
		mode <= (leases ? AMDGPU_GDS_PROBE_LAST : AMDGPU_GDS_PROBE_OA_RESET); mode++) {
		Run(c[0], mode); Run(c[1], mode);
	}
	for (auto& client : c) {
		Code(client); close(client.fd);
		Require(delete_area(client.code.area) == B_OK && delete_area(client.output.area) == B_OK, "release revoked clones");
	}
	Require(Info<amdgpu_gart_info>(monitor, AMDGPU_GART_INFO).allocated_bytes == ram.allocated_bytes
		&& Info<amdgpu_memory_info>(monitor, AMDGPU_MEMORY_INFO).allocated_bytes == vram.allocated_bytes, "complete memory reclamation");
	auto final = Healthy(monitor); Require(final.completed_fences == initial.completed_fences + sJobs, "exact OA prerequisite completion count");
	Revoked(monitor); close(monitor);
	printf("PASS: OA prerequisite %s, %llu checked jobs, 2 VMs, ABI, all resource/VM guards, revoke and reclamation\n",
		leases ? "leases" : "reset", (unsigned long long)sJobs);
}
