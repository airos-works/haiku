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
#include "GfxCommandValidator.h"

static_assert(sizeof(amdgpu_recovery_info) == 136, "recovery ABI");
static const uint64 kOutput = 0x300000000ULL, kVram = 0x400000000ULL, kCode = 0x500000000ULL;
static const uint32 kGuard = 0x6b6b6b6b, kMarker = 0x72535451;
struct Client { int fd; amdgpu_buffer output, vram, code; uint32 seed; };
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
static int Open() { int fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR); Require(fd >= 0, "open GPU"); return fd; }
static void Print(const amdgpu_recovery_info& r)
{
	printf("Recovery status %#x stage %u generation %llu masks %#x/%#x live handles/VM/video/jobs %u/%u/%u/%u\n",
		(unsigned)r.status, r.stage, (unsigned long long)r.generation, r.grbm_reset, r.srbm_reset,
		r.live_handles, r.live_vms, r.live_videos, r.pending);
	printf("Quarantine buffers/VM/video %u/%u/%u VRAM/RAM %llu/%llu reclaimed %llu/%llu\n",
		r.buffers, r.vms, r.videos, (unsigned long long)r.quarantined_vram, (unsigned long long)r.quarantined_ram,
		(unsigned long long)r.reclaimed_vram, (unsigned long long)r.reclaimed_ram);
	printf("Reset GRBM/GRBM2/SRBM/VM before %#x/%#x/%#x/%#x after %#x/%#x/%#x/%#x\n",
		r.before[0], r.before[1], r.before[2], r.before[3], r.after[0], r.after[1], r.after[2], r.after[3]);
}
static amdgpu_irq_info Healthy(int fd)
{
	auto r = Info<amdgpu_irq_info>(fd, AMDGPU_IRQ_INFO);
	Require(r.enabled && r.msi && r.status == B_OK && r.completed_fences == r.waits
		&& !r.vm_faults && !r.privileged_faults && !r.unknown && !r.overflows && r.rptr == r.wptr,
		"healthy IRQ retirement"); return r;
}
static amdgpu_buffer Buffer(int fd, uint64 va, bool ram, uint32 permissions, uint32 bytes)
{
	auto b = Request<amdgpu_buffer>(); b.bytes = bytes;
	Require(ioctl(fd, ram ? AMDGPU_CREATE_SYSTEM_BUFFER : AMDGPU_CREATE_BUFFER, &b, sizeof(b)) == 0
		&& ioctl(fd, AMDGPU_MAP_BUFFER, &b, sizeof(b)) == 0, "allocate/map owned buffer");
	auto p = (uint32*)(addr_t)b.address;
	for (uint32 i = 0; i < bytes / 4; i++) { Require(p[i] == 0, "fresh/reused allocation is zero"); p[i] = kGuard; }
	auto m = Request<amdgpu_vm_mapping>(); m.handle = b.handle; m.address = va;
	m.bytes = bytes; m.permissions = permissions;
	Require(ioctl(fd, AMDGPU_VM_MAP, &m, sizeof(m)) == 0, "map owned VM range"); return b;
}
static Client Create(uint32 seed)
{
	Client c = {}; c.fd = Open(); c.seed = seed;
	Info<amdgpu_vm_info>(c.fd, AMDGPU_VM_INFO);
	c.output = Buffer(c.fd, kOutput, true, AMDGPU_VM_READ | AMDGPU_VM_WRITE, 8192);
	c.vram = Buffer(c.fd, kVram, false, AMDGPU_VM_READ | AMDGPU_VM_WRITE, 8192);
	c.code = Buffer(c.fd, kCode, true, AMDGPU_VM_READ | AMDGPU_VM_EXECUTE, 4096);
	((uint32*)(addr_t)c.code.address)[0] = 0xbf82ffff; // gfx803 s_branch -1, one intentionally non-retiring wave.
	((uint32*)(addr_t)c.output.address)[0] = 0;
	__sync_synchronize(); return c;
}
static void Check(const Client& c, bool marker)
{
	auto p = (volatile uint32*)(addr_t)c.output.address;
	for (uint32 i = 0; i < 2048; i++) {
		uint32 want = i == 0 ? 0 : i == 1 && marker ? kMarker
			: i >= 1024 ? c.seed ^ ((i - 1024) * 0x10204081u) : kGuard;
		Require(p[i] == want, "exact victim/peer output and every guard");
		Require(((volatile uint32*)(addr_t)c.vram.address)[i] == kGuard, "VRAM peer/neighbor guard");
	}
	p = (volatile uint32*)(addr_t)c.code.address;
	for (uint32 i = 0; i < 1024; i++) Require(p[i] == (i == 0 ? 0xbf82ffff : kGuard), "code guard");
}
static void Control(Client& c)
{
	auto r = Request<amdgpu_vm_test>(); r.address = kOutput + 4096; r.value = c.seed;
	Require(ioctl(c.fd, AMDGPU_VM_TEST, &r, sizeof(r)) == 0 && r.status == B_OK
		&& !r.vm_fault_status[0] && !r.vm_fault_status[1], "bounded VM positive control");
	Check(c, false);
}
static void Close(Client& c)
{
	Require(close(c.fd) == 0 && delete_area(c.output.area) == B_OK
		&& delete_area(c.vram.area) == B_OK && delete_area(c.code.area) == B_OK,
		"close owners and delete revoked clones"); c.fd = -1;
}
static std::vector<uint32> Commands(bool shader)
{
	std::vector<uint32> w = {0xc0033700, 0x100500, uint32(kOutput + 4), uint32(kOutput >> 32), kMarker};
	if (!shader)
		w.insert(w.end(), {0xc0053c00, 0x13, uint32(kOutput), uint32(kOutput >> 32), 1, 0xffffffff, 4});
	else {
		auto reg = [&](uint32 index, uint32 value) { w.insert(w.end(), {0xc0017600, index - 0x2c00, value}); };
		reg(0x2e04, 0); reg(0x2e05, 0); reg(0x2e06, 0);
		reg(0x2e07, 64); reg(0x2e08, 1); reg(0x2e09, 1);
		reg(0x2e0c, kCode >> 8); reg(0x2e0d, kCode >> 40);
		reg(0x2e12, 1 | 2 << 6 | 0xc0 << 12); reg(0x2e13, 1 << 7);
		reg(0x2e15, 0); reg(0x2e16, 0xffffffff); reg(0x2e17, 0xffffffff);
		reg(0x2e18, 0); reg(0x2e19, 0xffffffff); reg(0x2e1a, 0xffffffff);
		w.insert(w.end(), {0xc0031502, 1, 1, 1, 5, 0xc0004600, 7 | 4 << 8});
	}
	w.resize(256, 0x80000000);
	auto validation = amdgpu::ValidateGfx8Commands(w.data(), w.size());
	if (validation.error != amdgpu::GfxCommandValidation::OK)
		fprintf(stderr, "%s fixture policy error %u at DWORD %u\n",
			shader ? "shader" : "CP", unsigned(validation.error), validation.dword);
	Require(validation.error == amdgpu::GfxCommandValidation::OK,
		"recovery fixture command policy"); return w;
}
static void ABI(int fd)
{
	for (uint32 op : {uint32(AMDGPU_RECOVERY_INFO), uint32(AMDGPU_GFX_RECOVER)}) {
		auto r = Request<amdgpu_recovery_info>();
		Require(ioctl(fd, op, NULL, sizeof(r)) < 0 && errno == B_BAD_ADDRESS, "null request denied");
		for (uint32 variant = 0; variant < 5; variant++) {
			auto bad = r; size_t bytes = sizeof(bad);
			switch (variant) { case 0: bad.version++; break; case 1: bad.size--; break;
				case 2: bytes--; break; case 3: bad.flags = 1; break; case 4: bad.reserved = 1; break; }
			Require(ioctl(fd, op, &bad, bytes) < 0 && errno == B_BAD_VALUE, "malformed recovery ABI rejected before action");
		}
		int ro = open("/dev/" AMDGPU_DEVICE_NAME, O_RDONLY); Require(ro >= 0, "read-only fd");
		Require(ioctl(ro, op, &r, sizeof(r)) < 0 && errno == B_NOT_ALLOWED, "read-only recovery denied"); close(ro);
	}
	pid_t child = fork(); Require(child >= 0, "fork root gate check");
	if (child == 0) {
		Require(setgid(65534) == 0 && setuid(65534) == 0, "drop privilege");
		int ordinary = Open(); auto r = Request<amdgpu_recovery_info>();
		for (uint32 op : {uint32(AMDGPU_RECOVERY_INFO), uint32(AMDGPU_GFX_RECOVER)})
			Require(ioctl(ordinary, op, &r, sizeof(r)) < 0 && errno == B_NOT_ALLOWED, "root-only recovery gates");
		close(ordinary); _exit(0);
	}
	int status; Require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "privilege child");
}
int main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0); disable_debugger(1);
	bool absent = argc == 2 && strcmp(argv[1], "--expect-no-device") == 0;
	bool baseline = argc == 2 && strcmp(argv[1], "--baseline") == 0;
	bool snapshot = argc == 2 && strcmp(argv[1], "--snapshot-only") == 0;
	bool recover = argc == 2 && strcmp(argv[1], "--recover") == 0;
	bool cp = argc == 2 && strcmp(argv[1], "--cp-timeout") == 0;
	bool shader = argc == 2 && strcmp(argv[1], "--shader-timeout") == 0;
	Require(absent || ((baseline || snapshot || recover || cp || shader) && geteuid() == 0),
		"usage: amdgpu_recovery --baseline | --snapshot-only | --cp-timeout | --shader-timeout | --recover | --expect-no-device");
	Commands(false); Commands(true);
	int monitor = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
	if (absent) { Require(monitor < 0 && errno == ENOENT, "no AMD device"); puts("PASS: recovery command policy and no-device handling"); return 0; }
	Require(monitor >= 0, "open monitor");
	auto memory = Info<amdgpu_memory_info>(monitor, AMDGPU_MEMORY_INFO);
	auto ram = Info<amdgpu_gart_info>(monitor, AMDGPU_GART_INFO);
	auto info = Info<amdgpu_recovery_info>(monitor, AMDGPU_RECOVERY_INFO); Print(info);
	if (snapshot) return 0;
	ABI(monitor);
	if (recover) {
		Require(info.status != B_OK && !info.live_handles && !info.live_vms && !info.live_videos
			&& !info.pending && info.buffers && info.vms && !info.videos, "closed graphics-only fault quarantine");
		auto request = Info<amdgpu_recovery_info>(monitor, AMDGPU_GFX_RECOVER); Print(request);
		Require(request.status == B_OK && request.stage == 7 && request.generation == info.generation + 1,
			"one explicit reset and private shader proof succeed; no automatic retry");
		Require(!request.buffers && !request.vms && !request.videos && !request.quarantined_ram
			&& !request.quarantined_vram && request.reclaimed_ram == info.quarantined_ram
			&& request.reclaimed_vram == info.quarantined_vram, "exact quarantine reclamation");
		Require(Info<amdgpu_memory_info>(monitor, AMDGPU_MEMORY_INFO).allocated_bytes + info.quarantined_vram == memory.allocated_bytes
			&& Info<amdgpu_gart_info>(monitor, AMDGPU_GART_INFO).allocated_bytes + info.quarantined_ram == ram.allocated_bytes,
			"independent allocators reclaim exactly retained storage");
		Require(Healthy(monitor).completed_fences == 1, "private recovery shader completed in fresh IRQ epoch");
		memory = Info<amdgpu_memory_info>(monitor, AMDGPU_MEMORY_INFO);
		ram = Info<amdgpu_gart_info>(monitor, AMDGPU_GART_INFO);
	} else {
		Info<amdgpu_render_info>(monitor, AMDGPU_RENDER_INFO);
		Require(Info<amdgpu_recovery_info>(monitor, AMDGPU_GFX_RECOVER).status == B_NOT_ALLOWED,
			"healthy device cannot be reset");
	}
	auto irqBefore = Healthy(monitor);
	Client c[2] = {Create(0x5100aa01), Create(0x5100aa02)};
	Control(c[0]); Control(c[1]);
	Require(Info<amdgpu_recovery_info>(monitor, AMDGPU_GFX_RECOVER).status == B_BUSY,
		"live GPU resources exclude reset");
	if (cp || shader) {
		auto words = Commands(shader); auto request = Request<amdgpu_gfx_submit>();
		request.address = (addr_t)words.data(); request.dwords = words.size(); request.flags = AMDGPU_GFX_VALIDATE_COMMANDS;
		Require(ioctl(c[0].fd, AMDGPU_GFX_SUBMIT_COPY, &request, sizeof(request)) == 0, "copied timeout fixture");
		printf("Terminal %s status %#x completion %llu VM %#x/%#x ring %u/%u elapsed %llu\n", shader ? "shader" : "CP",
			(unsigned)request.status, (unsigned long long)request.completion, request.vm_fault_status[0], request.vm_fault_status[1],
			request.rptr, request.wptr, (unsigned long long)request.elapsed_us);
		Require(request.status == B_TIMED_OUT && !request.completion && !request.vm_fault_status[0]
			&& !request.vm_fault_status[1], "expected terminal timeout, not a VM fault");
		Check(c[0], true); Check(c[1], false);
		auto irq = Info<amdgpu_irq_info>(monitor, AMDGPU_IRQ_INFO);
		Require(irq.completed_fences == irqBefore.completed_fences + 2 && irq.waits == irqBefore.waits + 3
			&& !irq.vm_faults && !irq.privileged_faults, "failed job cannot complete a fence");
		auto denied = Request<amdgpu_buffer>(); denied.bytes = 4096;
		Require(ioctl(c[1].fd, AMDGPU_CREATE_SYSTEM_BUFFER, &denied, sizeof(denied)) < 0
			&& errno == B_DEV_NOT_READY, "fault excludes fresh allocation");
		Require(Info<amdgpu_recovery_info>(monitor, AMDGPU_GFX_RECOVER).status == B_BUSY,
			"faulted live owners still exclude reset");
		Close(c[0]); Close(c[1]);
		auto q = Info<amdgpu_recovery_info>(monitor, AMDGPU_RECOVERY_INFO); Print(q);
		Require(q.status != B_OK && q.buffers == 6 && q.vms == 2 && !q.live_handles && !q.live_vms && !q.pending,
			"all closed BO/VM ownership records retained");
		Require(Info<amdgpu_memory_info>(monitor, AMDGPU_MEMORY_INFO).allocated_bytes == memory.allocated_bytes + q.quarantined_vram
			&& Info<amdgpu_gart_info>(monitor, AMDGPU_GART_INFO).allocated_bytes == ram.allocated_bytes + q.quarantined_ram,
			"independent allocators match complete quarantine");
		puts("PASS: terminal timeout, victim/peer guards, no false fence, busy gates and complete quarantine; recovery NOT attempted");
		return 0;
	}
	Close(c[0]); Close(c[1]);
	Require(Info<amdgpu_memory_info>(monitor, AMDGPU_MEMORY_INFO).allocated_bytes == memory.allocated_bytes
		&& Info<amdgpu_gart_info>(monitor, AMDGPU_GART_INFO).allocated_bytes == ram.allocated_bytes, "full healthy allocation reclamation");
	Require(Healthy(monitor).completed_fences == irqBefore.completed_fences + 2, "exact new client completion delta");
	info = Info<amdgpu_recovery_info>(monitor, AMDGPU_RECOVERY_INFO); Print(info);
	Require(!info.status && !info.buffers && !info.vms && !info.videos && !info.live_handles && !info.live_vms,
		"clean final resource state");
	puts(recover ? "PASS: explicit graphics recovery, exact reclamation, zeroed reuse and two fresh client VMs"
		: "PASS: recovery ABI/root gates, healthy/busy rejection, two VM controls and complete reclamation");
	close(monitor);
}
