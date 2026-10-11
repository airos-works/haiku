/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include <amdgpu_haiku.h>
#include "GfxCommandValidator.h"
#include "HevcStream.h"
#include "UvdFixture.h"
#include <OS.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

static const uint64 kVA = 0x300000000ULL;
static const uint32 kGuard = 0x6b6b6b6b, kSeed = 0x5100dec0;
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
	T r = Request<T>(); Require(ioctl(fd, op, &r, sizeof(r)) == 0, "read device state"); return r;
}
static int Open()
{
	int fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR); Require(fd >= 0, "open GPU"); return fd;
}
static std::vector<uint8> Read(const char* path)
{
	FILE* f = fopen(path, "rb"); Require(f != NULL, "open reference/fixture");
	Require(fseek(f, 0, SEEK_END) == 0, "seek fixture"); long size = ftell(f);
	Require(size > 0 && size < 64 * 1024 * 1024 && fseek(f, 0, SEEK_SET) == 0, "fixture bounds");
	std::vector<uint8> data(size);
	Require(fread(data.data(), 1, size, f) == size_t(size) && fclose(f) == 0, "read complete fixture");
	return data;
}
struct Memory { uint64 vram, ram; };
static Memory Allocation(int fd)
{
	return {Info<amdgpu_memory_info>(fd, AMDGPU_MEMORY_INFO).allocated_bytes,
		Info<amdgpu_gart_info>(fd, AMDGPU_GART_INFO).allocated_bytes};
}
static void CheckAllocation(int fd, Memory expected)
{
	Memory actual = Allocation(fd);
	printf("Allocation VRAM/RAM %llu/%llu expected %llu/%llu\n", (unsigned long long)actual.vram,
		(unsigned long long)actual.ram, (unsigned long long)expected.vram, (unsigned long long)expected.ram);
	Require(actual.vram == expected.vram && actual.ram == expected.ram, "exact independent allocation accounting");
}
static amdgpu_recovery_policy Policy(int fd, int enabled = -1)
{
	auto r = Request<amdgpu_recovery_policy>();
	if (enabled >= 0) { r.flags = AMDGPU_RECOVERY_SET_POLICY; r.enabled = enabled; }
	Require(ioctl(fd, AMDGPU_RECOVERY_POLICY, &r, sizeof(r)) == 0, "automatic recovery policy"); return r;
}
static void Lost(int fd, uint64 generation)
{
	auto r = Info<amdgpu_client_status>(fd, AMDGPU_CLIENT_STATUS);
	Require(r.flags == (AMDGPU_CLIENT_LOST | AMDGPU_CLIENT_RESET_COMPLETED)
		&& r.status != B_OK && r.device_status == B_OK && r.generation == generation
		&& r.loss_generation + 1 == generation && !r.submitted && !r.completed && !r.failed_fence,
		"idle decoder permanently lost, reset completed, no invented async fences");
}
struct Decoder {
	int fd;
	bool hevc;
	amdgpu_video_create avc;
	amdgpu_hevc_create hvc;
	uint64 Handle() const { return hevc ? hvc.handle : avc.handle; }
	uint32 Output() const { return hevc ? hvc.output_bytes : avc.output_bytes; }
	Memory Storage() const { return {hevc ? hvc.allocated_bytes : avc.allocated_bytes,
		((Output() + 4095ULL) & ~4095ULL) + 24576}; }
};
static HevcStream* sHevc;
static std::vector<uint8> sAvcReference, sHevcReference;
static Decoder Create(bool hevc)
{
	Decoder d = {}; d.fd = Open(); d.hevc = hevc;
	if (hevc) {
		d.hvc = Request<amdgpu_hevc_create>(); d.hvc.config = sHevc->config;
		Require(ioctl(d.fd, AMDGPU_HEVC_CREATE, &d.hvc, sizeof(d.hvc)) == 0, "create HEVC decoder");
		Require(d.hvc.output_bytes == sHevcReference.size(), "HEVC reference geometry");
	} else {
		d.avc = Request<amdgpu_video_create>(); d.avc.config = {864, 480, 100, 30, 2, 0};
		Require(ioctl(d.fd, AMDGPU_VIDEO_CREATE, &d.avc, sizeof(d.avc)) == 0, "create H264 decoder");
		Require(d.avc.output_bytes == sAvcReference.size(), "H264 reference geometry");
	}
	printf("Created %s handle %llu VRAM/RAM %llu/%llu\n", hevc ? "HEVC" : "H264",
		(unsigned long long)d.Handle(), (unsigned long long)d.Storage().vram, (unsigned long long)d.Storage().ram);
	return d;
}
static void Decode(const Decoder& d, bool lost = false)
{
	std::vector<uint8> pixels(d.Output(), 0x7b);
	int status;
	uint32 sequence, fence, rptr, wptr, guards, vm;
	if (d.hevc) {
		auto r = Request<amdgpu_hevc_decode>(); r.handle = d.Handle(); r.picture = sHevc->picture;
		r.bitstream = (addr_t)sHevc->bitstream.data(); r.bitstream_bytes = sHevc->bitstream.size();
		r.output = (addr_t)pixels.data(); r.output_capacity = pixels.size();
		status = ioctl(d.fd, AMDGPU_HEVC_DECODE, &r, sizeof(r));
		sequence = r.sequence; fence = r.fence; rptr = r.rptr; wptr = r.wptr;
		guards = r.guard_mismatches; vm = r.vm_fault_status;
	} else {
		auto r = Request<amdgpu_video_decode>(); r.handle = d.Handle();
		r.bitstream = (addr_t)uvd_bitstream; r.bitstream_bytes = sizeof(uvd_bitstream);
		r.output = (addr_t)pixels.data(); r.output_capacity = pixels.size();
		auto& p = r.picture; p.flags = AMDGPU_H264_IDR; p.sps_flags = 5; p.pps_flags = 0x88;
		p.log2_frame_num_minus4 = 1; p.log2_poc_lsb_minus4 = 3; p.initial_qp_minus26 = 2;
		memset(p.scaling4x4, 16, sizeof(p.scaling4x4)); memset(p.scaling8x8, 16, sizeof(p.scaling8x8));
		status = ioctl(d.fd, AMDGPU_VIDEO_DECODE, &r, sizeof(r));
		sequence = r.sequence; fence = r.fence; rptr = r.rptr; wptr = r.wptr;
		guards = r.guard_mismatches; vm = r.vm_fault_status;
	}
	if (lost) {
		Require(status == -1 && errno == B_DEV_NOT_READY && !sequence && !fence,
			"lost decoder rejects a new submission");
		for (uint8 byte : pixels) Require(byte == 0x7b, "lost decoder preserves every output sentinel");
	} else {
		Require(status == 0 && sequence && sequence == fence && rptr == wptr && !guards && !(vm & 0xff),
			"video fence and all private guards");
		Require(pixels == (d.hevc ? sHevcReference : sAvcReference), "every padded pixel equals independent reference");
	}
}
static void Destroy(Decoder& d, bool lost)
{
	auto r = Request<amdgpu_video_destroy>(); r.handle = d.Handle();
	Require(ioctl(d.fd, AMDGPU_VIDEO_DESTROY, &r, sizeof(r)) == 0, "firmware destroy after reset");
	if (lost) {
		if (d.hevc) Require(ioctl(d.fd, AMDGPU_HEVC_CREATE, &d.hvc, sizeof(d.hvc)) < 0 && errno == B_DEV_NOT_READY,
			"old HEVC file cannot create a replacement session");
		else Require(ioctl(d.fd, AMDGPU_VIDEO_CREATE, &d.avc, sizeof(d.avc)) < 0 && errno == B_DEV_NOT_READY,
			"old H264 file cannot create a replacement session");
	}
	Require(close(d.fd) == 0, "close decoder"); d.fd = -1;
}
struct Graphics { int fd; amdgpu_buffer buffer; };
static Graphics GraphicsCreate(bool cp)
{
	Graphics c = {}; c.fd = Open(); Info<amdgpu_vm_info>(c.fd, AMDGPU_VM_INFO);
	c.buffer = Request<amdgpu_buffer>(); c.buffer.bytes = 8192;
	Require(ioctl(c.fd, AMDGPU_CREATE_SYSTEM_BUFFER, &c.buffer, sizeof(c.buffer)) == 0
		&& ioctl(c.fd, AMDGPU_MAP_BUFFER, &c.buffer, sizeof(c.buffer)) == 0, "owned graphics RAM");
	for (uint32 i = 0; i < 2048; i++) ((uint32*)(addr_t)c.buffer.address)[i] = i ? kGuard : 0;
	for (uint32 page = 0; page < 2; page++) {
		auto m = Request<amdgpu_vm_mapping>(); m.handle = c.buffer.handle;
		m.address = kVA + page * 4096; m.buffer_offset = page * 4096; m.bytes = 4096;
		m.permissions = AMDGPU_VM_READ | ((cp || page) ? AMDGPU_VM_WRITE : 0);
		Require(ioctl(c.fd, AMDGPU_VM_MAP, &m, sizeof(m)) == 0, "bind owned graphics pages");
	}
	return c;
}
static void GraphicsCheck(const Graphics& c)
{
	for (uint32 i = 0; i < 2048; i++) Require(((volatile uint32*)(addr_t)c.buffer.address)[i]
		== (i == 0 ? 0 : i < 1024 ? kGuard : kSeed ^ ((i - 1024) * 0x10204081u)),
		"exact graphics output and all fault victim guards");
}
static void GraphicsControl(Graphics& c)
{
	auto r = Request<amdgpu_vm_test>(); r.address = kVA + 4096; r.value = kSeed;
	Require(ioctl(c.fd, AMDGPU_VM_TEST, &r, sizeof(r)) == 0 && r.status == B_OK
		&& r.completion && !r.vm_fault_status[0] && !r.vm_fault_status[1], "fresh graphics control");
	GraphicsCheck(c);
}
static void GraphicsClose(Graphics& c)
{
	Require(close(c.fd) == 0 && delete_area(c.buffer.area) == B_OK, "close graphics and revoked clone");
}
static std::vector<uint32> Commands(bool cp)
{
	std::vector<uint32> w;
	if (cp) w = {0xc0053c00, 0x13, uint32(kVA), uint32(kVA >> 32), 1, 0xffffffff, 4};
	else w = {0xc0033700, 0x100500, uint32(kVA), uint32(kVA >> 32), 0x5100bad0};
	w.resize(256, 0x80000000);
	Require(amdgpu::ValidateGfx8Commands(w.data(), w.size()).error == amdgpu::GfxCommandValidation::OK,
		"fault fixture passes command policy"); return w;
}
int main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0); Commands(false); Commands(true);
	if (argc == 2 && strcmp(argv[1], "--expect-no-device") == 0) {
		int fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
		Require(fd < 0 && errno == B_ENTRY_NOT_FOUND, "no GPU in emulator");
		puts("PASS: video recovery command policy and no-device handling"); return 0;
	}
	Require(argc == 5 && geteuid() == 0,
		"usage: amdgpu_video_recovery --baseline|--auto-cp|--manual-fault avc.nv12 hevc.au hevc.padded");
	bool baseline = strcmp(argv[1], "--baseline") == 0, automatic = strcmp(argv[1], "--auto-cp") == 0;
	Require(baseline || automatic || strcmp(argv[1], "--manual-fault") == 0, "explicit recovery mode");
	sAvcReference = Read(argv[2]); sHevcReference = Read(argv[4]);
	auto au = Read(argv[3]); Require(au.size() >= 4, "HEVC access unit header");
	uint32 bytes = au[0] | uint32(au[1]) << 8 | uint32(au[2]) << 16 | uint32(au[3]) << 24;
	Require(bytes && bytes <= au.size() - 4, "complete first HEVC access unit");
	sHevc = new HevcStream();
	Require(sHevc->Prepare(au.data() + 4, bytes) && !sHevc->skipPicture && sHevc->poc == 0
		&& sHevc->picture.nal_type >= 16 && sHevc->picture.nal_type <= 20,
		"independently repeatable first HEVC random access picture");
	int monitor = Open(); auto start = Info<amdgpu_recovery_info>(monitor, AMDGPU_RECOVERY_INFO);
	Require(start.status == B_OK && !start.pending && !start.live_handles && !start.live_vms && !start.live_videos
		&& !start.buffers && !start.vms && !start.videos, "idle clean device baseline");
	Memory empty = Allocation(monitor); auto policy = Policy(monitor);
	Require(policy.enabled == 1 && !policy.pending, "automatic recovery enabled initially");
	if (!baseline && !automatic) Policy(monitor, 0);
	Graphics graphics = GraphicsCreate(automatic); GraphicsControl(graphics);
	// Allocate the copied-command backing before measuring retained storage.
	std::vector<uint32> noop(256, 0x80000000); auto warm = Request<amdgpu_gfx_submit>();
	warm.address = (addr_t)noop.data(); warm.dwords = noop.size(); warm.flags = AMDGPU_GFX_VALIDATE_COMMANDS;
	Require(ioctl(graphics.fd, AMDGPU_GFX_SUBMIT_COPY, &warm, sizeof(warm)) == 0
		&& warm.status == B_OK && warm.completion, "warm copied-command storage");
	Memory graphicsOnly = Allocation(monitor);
	Decoder d[4] = {Create(false), Create(true), Create(false), Create(true)};
	Memory all = graphicsOnly;
	for (auto& decoder : d) { Decode(decoder); all.vram += decoder.Storage().vram; all.ram += decoder.Storage().ram; }
	CheckAllocation(monitor, all);
	if (!baseline) {
		auto commands = Commands(automatic); auto submit = Request<amdgpu_gfx_submit>();
		submit.address = (addr_t)commands.data(); submit.dwords = commands.size(); submit.flags = AMDGPU_GFX_VALIDATE_COMMANDS;
		Require(ioctl(graphics.fd, AMDGPU_GFX_SUBMIT_COPY, &submit, sizeof(submit)) == 0
			&& submit.status == (automatic ? B_TIMED_OUT : B_BAD_DATA) && !submit.completion,
			"one terminal graphics fault with four live decoder sessions");
		printf("Fault status %#x VM %#x/%#x ring %u/%u\n", (unsigned)submit.status,
			submit.vm_fault_status[0], submit.vm_fault_status[1], submit.rptr, submit.wptr);
		Memory reclaim = {};
		if (!automatic) {
			for (uint32 i = 2; i < 4; i++) {
				reclaim.vram += d[i].Storage().vram; reclaim.ram += d[i].Storage().ram;
				Require(close(d[i].fd) == 0, "close video while faulted"); d[i].fd = -1;
			}
			auto q = Info<amdgpu_recovery_info>(monitor, AMDGPU_RECOVERY_INFO);
			Require(q.status != B_OK && q.generation == start.generation && q.live_videos == 2 && q.videos == 2
				&& !q.buffers && !q.vms && q.quarantined_vram == reclaim.vram && q.quarantined_ram == reclaim.ram,
				"closed firmware sessions retain exact storage until recovery");
			CheckAllocation(monitor, all);
			Require(Info<amdgpu_recovery_info>(monitor, AMDGPU_GFX_RECOVER).status == B_BUSY,
				"legacy reset still excludes live sessions/resources");
			Require(Info<amdgpu_recovery_info>(monitor, AMDGPU_GFX_RECOVER_LIVE).status == B_OK,
				"one explicit reset retires closed firmware sessions");
		}
		auto recovered = Request<amdgpu_recovery_info>(); bigtime_t deadline = system_time() + 10000000;
		do {
			recovered = Info<amdgpu_recovery_info>(monitor, AMDGPU_RECOVERY_INFO);
			if (recovered.generation != start.generation) break;
			Require(system_time() < deadline, "observe automatic recovery without retry/reset"); snooze(1000);
		} while (true);
		Require(recovered.status == B_OK && recovered.generation == start.generation + 1 && recovered.stage == 7
			&& recovered.live_videos == (automatic ? 4u : 2u) && recovered.live_handles == 1 && recovered.live_vms == 1
			&& !recovered.videos && !recovered.vms && !recovered.buffers && !recovered.pending
			&& recovered.reclaimed_vram == reclaim.vram && recovered.reclaimed_ram == reclaim.ram
			&& !(recovered.srbm_reset & 0x8000), "one GFX reset preserves live video, reclaims only closed ownership");
		auto afterPolicy = Policy(monitor);
		Require(afterPolicy.attempts == policy.attempts + automatic && afterPolicy.successes == policy.successes + automatic
			&& !afterPolicy.pending, "exact automatic attempt/success counters");
		all.vram -= reclaim.vram; all.ram -= reclaim.ram; CheckAllocation(monitor, all); GraphicsCheck(graphics);
		for (auto& decoder : d) if (decoder.fd >= 0) { Lost(decoder.fd, recovered.generation); Decode(decoder, true); }
		puts("PASS: live video remains allocated, closed sessions retired, every old output sentinel untouched");
	}
	for (auto& decoder : d) if (decoder.fd >= 0) {
		Memory bytes = decoder.Storage(); Destroy(decoder, !baseline);
		all.vram -= bytes.vram; all.ram -= bytes.ram; CheckAllocation(monitor, all);
	}
	CheckAllocation(monitor, graphicsOnly); GraphicsClose(graphics); CheckAllocation(monitor, empty);
	Graphics fresh = GraphicsCreate(true); GraphicsControl(fresh);
	for (unsigned round = 0; round < 8; round++) {
		Decoder a = Create(false), b = Create(true);
		Decode(a); Decode(b); Destroy(a, false); Destroy(b, false);
	}
	GraphicsControl(fresh); GraphicsClose(fresh); CheckAllocation(monitor, empty);
	if (!baseline && !automatic) Policy(monitor, policy.enabled);
	auto irq = Info<amdgpu_irq_info>(monitor, AMDGPU_IRQ_INFO);
	Require(irq.status == B_OK && irq.enabled && irq.msi && irq.completed_fences == irq.waits
		&& !irq.vm_faults && !irq.privileged_faults && !irq.overflows && !irq.unknown && irq.rptr == irq.wptr,
		"fresh IRQ epoch is healthy");
	if (!baseline) Require(irq.completed_fences == 3, "private reset proof plus two fresh graphics controls");
	auto final = Info<amdgpu_recovery_info>(monitor, AMDGPU_RECOVERY_INFO);
	Require(final.status == B_OK && !final.live_handles && !final.live_vms && !final.live_videos
		&& !final.pending && !final.buffers && !final.vms && !final.videos, "no leaked owners or quarantine");
	printf("PASS: %s, 20 exact decoded frames, 16 recreated sessions, generation %llu, healthy fences %llu\n",
		argv[1], (unsigned long long)final.generation, (unsigned long long)irq.completed_fences);
	close(monitor); delete sHevc; return 0;
}
