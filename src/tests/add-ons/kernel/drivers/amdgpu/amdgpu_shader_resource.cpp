/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
// Root-only hardware qualification. A pass is not a general shader validator.
#include <amdgpu_haiku.h>
#include <OS.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include "GfxCommandValidator.h"
#include "ShaderResource.h"

static_assert(sizeof(amdgpu_shader_memory_info) == 112, "bounded diagnostic ABI");

static const uint64 kCode = 0x500000000ULL, kOutput = 0x300000000ULL,
	kData = 0x700000000ULL;
static const uint32 kBytes = 131072, kGuard = 0x6b6b6b6b;
using Words = std::vector<uint32>;
struct Client {
	int fd;
	amdgpu_buffer code, output, data;
	uint32 seed, inputSeed;
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
	reg(0x2e12, 3 | 1 << 6 | 0xc0 << 12); // 16 VGPRs, 16 SGPRs; PRIV clear.
	reg(0x2e13, 5 << 1 | 1 << 7 | 1 << 15); // Five user SGPRs, group X, 512-byte LDS.
	reg(0x2e15, 0); reg(0x2e16, 0xffffffff); reg(0x2e17, 0xffffffff);
	reg(0x2e18, 0); reg(0x2e19, 0xffffffff); reg(0x2e1a, 0xffffffff);
	reg(0x2e40, uint32(kOutput + 4096)); reg(0x2e41, kOutput >> 32); reg(0x2e42, seed);
	reg(0x2e43, uint32(kData + 4096)); reg(0x2e44, kData >> 32);
	w.insert(w.end(), {0xc0031502, 16, 1, 1, 5, 0xc0004600, 7 | 4 << 8});
	w.resize(256, 0x80000000);
	Require(amdgpu::ValidateGfx8Commands(w.data(), w.size()).error == amdgpu::GfxCommandValidation::OK,
		"shader probe commands pass the checked policy");
	return w;
}
static uint32 Input(const Client& c, uint32 word)
{
	return c.inputSeed ^ (word * 0x10204081u);
}
static void ResetData(Client& c)
{
	auto p = (uint32*)(addr_t)c.data.address;
	for (uint32 i = 0; i < 16384 / 4; i++) p[i] = Input(c, i);
}
static void Check(const Client& c)
{
	auto p = (volatile uint32*)(addr_t)c.output.address;
	for (uint32 i = 0; i < kBytes / 4; i++) {
		uint32 want = kGuard;
		if (c.seed && i >= 1024 && i < 1024 + 16384) {
			uint32 lane = (i - 1024) / 16;
			uint32 word = 1024 + (lane / 64) * 128 + lane % 64;
			uint32 input = Input(c, word), changed = input ^ c.seed;
			const uint32 values[16] = {
				input, input, 0, 0, // Buffer/flat, zero-size and out-of-bound reads.
				Input(c, word - lane % 64), Input(c, word - lane % 64 + 63),
				0, changed, changed, lane, // OOB atomic, in-range store, LDS.
				lane / 64, lane % 64, c.seed, changed, c.seed, kGuard
			};
			want = values[(i - 1024) % 16];
		}
		if (p[i] != want) {
			fprintf(stderr, "output word %u, record %u field %u: %#x expected %#x\n",
				i, i >= 1024 ? (i - 1024) / 16 : 0, i % 16, (unsigned)p[i], want);
			Require(false, "all resource observations, positive controls and guards match");
		}
	}
	auto data = (volatile uint32*)(addr_t)c.data.address;
	for (uint32 i = 0; i < 16384 / 4; i++) {
		uint32 want = Input(c, i);
		if (c.seed && i >= 1024 && i < 3072 && (i - 1024) % 128 < 64)
			want = (i - 1024) % 128 == 63 ? c.seed : (want ^ c.seed) + 1;
		if (data[i] != want) {
			fprintf(stderr, "data word %u: %#x expected %#x\n", i, (unsigned)data[i], want);
			Require(false, "in-range stores and out-of-resource guards");
		}
	}
	for (uint32 i = sizeof(kShaderResource) / 4; i < 1024; i++)
		Require(((uint32*)(addr_t)c.code.address)[i] == kGuard, "shader tail guard");
	Require(memcmp((void*)(addr_t)c.code.address, kShaderResource, sizeof(kShaderResource)) == 0,
		"shader instructions remain unchanged");
}
static amdgpu_shader_memory_info Snapshot(int fd, bool initialized)
{
	auto r = Request<amdgpu_shader_memory_info>();
	Require(ioctl(fd, AMDGPU_SHADER_MEMORY_INFO, &r, sizeof(r)) == 0, "shader memory snapshot");
	Require(r.vmid == 2 && r.selector_before == r.selector_after && !r.reserved_out,
		"fixed VMID and restored bank selector");
	if (initialized) Require(r.sh_mem_config == 0x338 && r.sh_mem_bases == 0x2000
		&& r.sh_mem_ape1_base == 1 && r.sh_mem_ape1_limit == 0
		&& !r.gds_base && !r.gds_size && !r.gws && !r.oa, "leased VMID apertures and no GDS/GWS/OA");
	printf("VMID%u selector %#x/%#x SH_MEM config %#x bases %#x APE1 %#x/%#x GDS %#x/%#x GWS %#x OA %#x\n",
		r.vmid, r.selector_before, r.selector_after, r.sh_mem_config, r.sh_mem_bases,
		r.sh_mem_ape1_base, r.sh_mem_ape1_limit, r.gds_base, r.gds_size, r.gws, r.oa);
	printf("ATC control %#x debug %#x PASID %#x L1 offset %#x apertures %#x/%#x/%#x/%#x %#x/%#x/%#x/%#x\n",
		r.atc_ats_control, r.atc_ats_debug, r.atc_vmid_pasid, r.atc_l1_address_offset,
		r.atc_aperture[0][0], r.atc_aperture[0][1], r.atc_aperture[0][2], r.atc_aperture[0][3],
		r.atc_aperture[1][0], r.atc_aperture[1][1], r.atc_aperture[1][2], r.atc_aperture[1][3]);
	return r;
}
static void CheckABI(int fd)
{
	auto r = Request<amdgpu_shader_memory_info>();
	Require(ioctl(fd, AMDGPU_SHADER_MEMORY_INFO, NULL, sizeof(r)) < 0 && errno == B_BAD_ADDRESS, "bad pointer");
	for (unsigned variant = 0; variant < 5; variant++) {
		r = Request<amdgpu_shader_memory_info>();
		if (variant == 0) r.version++;
		if (variant == 1) r.size--;
		if (variant == 2) r.flags = 1;
		if (variant == 3) r.reserved = 1;
		Require(ioctl(fd, AMDGPU_SHADER_MEMORY_INFO, &r, sizeof(r) - (variant == 4)) < 0
			&& errno == B_BAD_VALUE, "malformed snapshot rejected");
	}
	int ro = open("/dev/" AMDGPU_DEVICE_NAME, O_RDONLY);
	Require(ro >= 0, "read-only client"); r = Request<amdgpu_shader_memory_info>();
	Require(ioctl(ro, AMDGPU_SHADER_MEMORY_INFO, &r, sizeof(r)) < 0 && errno == B_NOT_ALLOWED,
		"read-only snapshot rejected"); close(ro);
	pid_t child = fork(); Require(child >= 0, "fork privilege check");
	if (child == 0) {
		Require(setgid(65534) == 0 && setuid(65534) == 0, "drop privilege");
		int own = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR); Require(own >= 0, "unprivileged client");
		Require(ioctl(own, AMDGPU_SHADER_MEMORY_INFO, &r, sizeof(r)) < 0 && errno == B_NOT_ALLOWED,
			"root-only diagnostic enforced"); close(own); _exit(0);
	}
	int status; Require(waitpid(child, &status, 0) == child && WIFEXITED(status)
		&& WEXITSTATUS(status) == 0, "unprivileged child passes");
}
int main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0); disable_debugger(1);
	Commands(0x12345678);
	if (argc == 2 && strcmp(argv[1], "--expect-no-device") == 0) {
		int fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
		Require(fd < 0 && errno == ENOENT, "no AMD device in QEMU");
		puts("PASS: shader resource command policy and no-device handling"); return 0;
	}
	Require(argc == 1 && geteuid() == 0, "shader resource qualification requires root");
	int monitor = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR); Require(monitor >= 0, "open monitor");
	auto render = Request<amdgpu_render_info>();
	Require(ioctl(monitor, AMDGPU_RENDER_INFO, &render, sizeof(render)) == 0
		&& (render.capabilities & AMDGPU_RENDER_CHECKED_COPY), "checked copy capability");
	CheckABI(monitor);
	auto translation = Snapshot(monitor, false);
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
		c.data = Buffer(c.fd, kData, 16384, AMDGPU_VM_READ | AMDGPU_VM_WRITE);
		c.inputSeed = 0x13550000 | uint32(&c - clients);
		ResetData(c);
		for (uint32 i = 0; i < 1024; i++) ((uint32*)(addr_t)c.code.address)[i] = kGuard;
		c.output = Buffer(c.fd, kOutput, kBytes, AMDGPU_VM_READ | AMDGPU_VM_WRITE);
		memcpy((void*)(addr_t)c.code.address, kShaderResource, sizeof(kShaderResource));
		auto p = (uint32*)(addr_t)c.output.address;
		for (uint32 i = 0; i < kBytes / 4; i++) p[i] = kGuard;
	}
	__sync_synchronize();
	for (unsigned round = 0; round < 4; round++) {
		for (unsigned client = 0; client < 4; client++) {
			auto& c = clients[client]; c.seed = 0x51000000 | round << 8 | client;
			ResetData(c);
			__sync_synchronize();
			auto words = Commands(c.seed);
			auto r = Request<amdgpu_gfx_submit>(); r.address = (addr_t)words.data();
			r.dwords = words.size(); r.flags = AMDGPU_GFX_VALIDATE_COMMANDS;
			Require(ioctl(c.fd, AMDGPU_GFX_SUBMIT_COPY, &r, sizeof(r)) == 0
				&& r.status == B_OK && r.completion && !r.vm_fault_status[0]
				&& !r.vm_fault_status[1] && r.rptr == r.wptr, "checked shader completes");
			__sync_synchronize();
			for (const auto& peer : clients) Check(peer);
			auto snapshot = Snapshot(monitor, true);
			Require(memcmp(&translation.atc_ats_control, &snapshot.atc_ats_control,
				sizeof(snapshot) - offsetof(amdgpu_shader_memory_info, atc_ats_control)) == 0,
				"ATC configuration unchanged across clients");
			printf("PASS: round %u VM %u, 1024 records, scalar/vector/flat/LDS controls,"
				" zero-size/OOB loads/stores/atomic and all same-VA peer guards match\n", round, client);
		}
	}
	for (auto& c : clients) {
		close(c.fd);
		Require(delete_area(c.code.area) == B_OK && delete_area(c.output.area) == B_OK
			&& delete_area(c.data.area) == B_OK,
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
