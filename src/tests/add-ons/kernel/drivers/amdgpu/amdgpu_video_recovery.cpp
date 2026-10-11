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
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

static_assert(sizeof(amdgpu_uvd_timeout_test) == 48, "UVD timeout ABI");
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
static bool Decode(const Decoder& d, bool lost = false, bool racing = false)
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
	if (lost || (racing && status < 0)) {
		Require(status == -1 && errno == B_DEV_NOT_READY && (racing || (!sequence && !fence)),
			"lost decoder rejects a new submission");
		for (uint8 byte : pixels) Require(byte == 0x7b, "lost decoder preserves every output sentinel");
		return false;
	} else {
		Require(status == 0 && sequence && sequence == fence && rptr == wptr && !guards && !(vm & 0xff),
			"video fence and all private guards");
		Require(pixels == (d.hevc ? sHevcReference : sAvcReference), "every padded pixel equals independent reference");
	}
	return true;
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
struct VideoThread {
	const Decoder* decoder;
	int createFD;
	bool active;
	int32 frames, done;
};
static int32 VideoWorker(void* data)
{
	auto& t = *(VideoThread*)data;
	if (t.createFD >= 0) {
		auto r = Request<amdgpu_hevc_create>(); r.config = sHevc->config;
		Require(ioctl(t.createFD, AMDGPU_HEVC_CREATE, &r, sizeof(r)) < 0
			&& errno == B_DEV_NOT_READY && !r.handle && !r.output_bytes && !r.allocated_bytes,
			"waiting create cannot publish a session after its file is lost");
	} else if (t.active) {
		while (Decode(*t.decoder, false, true)) atomic_add(&t.frames, 1);
	} else Decode(*t.decoder, true);
	atomic_set(&t.done, 1); return B_OK;
}
static thread_id Start(VideoThread& t)
{
	thread_id id = spawn_thread(VideoWorker, "video reset owner", B_NORMAL_PRIORITY, &t);
	Require(id >= 0 && resume_thread(id) == B_OK, "start video owner"); return id;
}
static void Join(thread_id id)
{
	status_t result; Require(wait_for_thread(id, &result) == B_OK && result == B_OK, "join video owner");
}
static void WaitBusy(int fd, VideoThread& t)
{
	bigtime_t deadline = system_time() + 500000;
	for (;;) {
		auto r = Request<amdgpu_video_destroy>(); // Invalid handle never destroys a session.
		Require(ioctl(fd, AMDGPU_VIDEO_DESTROY, &r, sizeof(r)) < 0
			&& (errno == B_BUSY || errno == B_BAD_VALUE), "probe video ownership");
		if (errno == B_BUSY) return;
		Require(!atomic_get(&t.done) && system_time() < deadline, "observe waiting/executing video owner");
		snooze(50);
	}
}
static void FaultWithWaiters(Graphics& c, Decoder& decoder)
{
	auto words = Commands(true); auto q = Request<amdgpu_gfx_queue>();
	q.address = (addr_t)words.data(); q.dwords = words.size(); q.flags = AMDGPU_GFX_VALIDATE_COMMANDS;
	Require(ioctl(c.fd, AMDGPU_GFX_QUEUE_COPY, &q, sizeof(q)) == 0, "queue graphics timeout before video waiters");
	auto dma = Request<amdgpu_dma_submit>(); dma.operation = AMDGPU_DMA_FILL;
	dma.destination = c.buffer.handle; dma.destination_offset = 4096; dma.bytes = 4; dma.value = 0xbad05100;
	Require(ioctl(c.fd, AMDGPU_SUBMIT_DMA, &dma, sizeof(dma)) == 0, "queue cancellable DMA before video waiters");
	VideoThread decode = {&decoder, -1, false, 0, 0}, create = {NULL, Open(), false, 0, 0};
	thread_id a = Start(decode), b = Start(create);
	WaitBusy(decoder.fd, decode); WaitBusy(create.createFD, create);
	auto pending = Info<amdgpu_recovery_info>(c.fd, AMDGPU_RECOVERY_INFO);
	Require(pending.status == B_OK && pending.pending == 2 && pending.live_videos == 5,
		"both video waiters entered before the fault, including partial create ownership");
	Join(a); Join(b);
	for (uint64 fence : {q.fence, dma.fence}) {
		auto wait = Request<amdgpu_fence_wait>(); wait.fence = fence; wait.timeout_us = 5000000;
		Require(ioctl(c.fd, AMDGPU_WAIT_FENCE, &wait, sizeof(wait)) == 0
			&& wait.status == B_TIMED_OUT, "terminal graphics and canceled DMA fences");
	}
	auto state = Info<amdgpu_client_status>(create.createFD, AMDGPU_CLIENT_STATUS);
	Require(state.flags == (AMDGPU_CLIENT_LOST | AMDGPU_CLIENT_RESET_COMPLETED)
		&& state.device_status == B_OK, "waiting create file remains lost after worker recovery");
	Require(close(create.createFD) == 0, "close failed create file");
	puts("PASS: decode/create waiters rejected after reset; partial session cleaned; queued DMA canceled");
}
static void UvdTimeoutABI(int monitor)
{
	for (uint32 variant = 0; variant < 5; variant++) {
		auto r = Request<amdgpu_uvd_timeout_test>(); size_t length = sizeof(r);
		if (variant == 0) r.version++;
		if (variant == 1) r.size--;
		if (variant == 2) length--;
		if (variant == 3) r.flags = 1;
		if (variant == 4) r.reserved = 1;
		Require(ioctl(monitor, AMDGPU_UVD_TIMEOUT_TEST, &r, length) < 0 && errno == B_BAD_VALUE,
			"malformed UVD timeout ABI rejected before hardware");
	}
	auto r = Request<amdgpu_uvd_timeout_test>();
	int ro = open("/dev/" AMDGPU_DEVICE_NAME, O_RDONLY);
	Require(ro >= 0 && ioctl(ro, AMDGPU_UVD_TIMEOUT_TEST, &r, sizeof(r)) < 0
		&& errno == B_NOT_ALLOWED, "read-only timeout test denied"); close(ro);
	void* address = NULL;
	area_id area = create_area("UVD timeout result protection", &address, B_ANY_ADDRESS,
		B_PAGE_SIZE, B_NO_LOCK, B_READ_AREA | B_WRITE_AREA);
	Require(area >= 0, "create protected timeout request");
	*(amdgpu_uvd_timeout_test*)address = r;
	Require(set_area_protection(area, B_READ_AREA) == B_OK, "protect timeout result");
	Require(ioctl(monitor, AMDGPU_UVD_TIMEOUT_TEST, address, sizeof(r)) < 0
		&& errno == B_BAD_ADDRESS, "unwritable timeout result rejected before hardware");
	Require(delete_area(area) == B_OK, "delete protected request");
	pid_t child = fork(); Require(child >= 0, "fork timeout privilege test");
	if (child == 0) {
		Require(setgid(65534) == 0 && setuid(65534) == 0, "drop timeout test privilege");
		int ordinary = Open();
		Require(ioctl(ordinary, AMDGPU_UVD_TIMEOUT_TEST, &r, sizeof(r)) < 0
			&& errno == B_NOT_ALLOWED, "ordinary client cannot inject UVD timeout");
		close(ordinary); _exit(0);
	}
	int status;
	Require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
		"timeout privilege child");
	Require(Info<amdgpu_recovery_info>(monitor, AMDGPU_RECOVERY_INFO).status == B_OK,
		"all rejected timeout requests leave device healthy");
}
static void UvdTimeout(int monitor)
{
	UvdTimeoutABI(monitor);
	auto r = Request<amdgpu_uvd_timeout_test>();
	int result = ioctl(monitor, AMDGPU_UVD_TIMEOUT_TEST, &r, sizeof(r));
	printf("UVD timeout ioctl %d status %#x fence %u/%u ring %u/%u UVD %#x elapsed %llu us\n",
		result, (unsigned)r.status, r.fence, r.sequence, r.rptr, r.wptr, r.uvd_status,
		(unsigned long long)r.elapsed_us);
	Require(result == 0 && r.status == B_TIMED_OUT && r.sequence && !r.fence
		&& r.rptr != r.wptr && r.elapsed_us >= 900000, "actual non-retiring UVD ring reaches bounded timeout");
}
int main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0); Commands(false); Commands(true);
	if (argc == 2 && strcmp(argv[1], "--expect-no-device") == 0) {
		int fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
		Require(fd < 0 && errno == B_ENTRY_NOT_FOUND, "no GPU in emulator");
		puts("PASS: video recovery command policy and no-device handling"); return 0;
	}
	if (argc == 2 && strcmp(argv[1], "--failed-recovery") == 0) {
		Require(geteuid() == 0, "root failed-recovery observation");
		int monitor = Open(); Memory before = Allocation(monitor);
		auto failed = Info<amdgpu_recovery_info>(monitor, AMDGPU_RECOVERY_INFO);
		auto policy = Policy(monitor);
		Require(failed.status != B_OK && failed.videos && !failed.live_handles && !failed.live_vms
			&& !failed.live_videos && !failed.pending && !policy.pending
			&& policy.status != B_OK && policy.attempts > policy.successes,
			"terminal failed automatic reset with closed retained video");
		for (uint32 op : {uint32(AMDGPU_GFX_RECOVER), uint32(AMDGPU_GFX_RECOVER_LIVE)})
			Require(Info<amdgpu_recovery_info>(monitor, op).status == B_NOT_ALLOWED,
				"failed reset cannot be retried through either manual API");
		auto after = Info<amdgpu_recovery_info>(monitor, AMDGPU_RECOVERY_INFO);
		auto afterPolicy = Policy(monitor);
		Require(after.generation == failed.generation && after.stage == failed.stage
			&& after.videos == failed.videos && after.quarantined_vram == failed.quarantined_vram
			&& after.quarantined_ram == failed.quarantined_ram
			&& afterPolicy.attempts == policy.attempts && afterPolicy.successes == policy.successes
			&& !afterPolicy.pending, "refusal preserves terminal state and quarantine");
		CheckAllocation(monitor, before); close(monitor);
		puts("PASS: failed reset remains terminal, both manual APIs refuse retry, all quarantine retained"); return 0;
	}
	Require(argc == 5 && geteuid() == 0,
		"usage: amdgpu_video_recovery --baseline|--auto-cp|--auto-waiters|--auto-decode|--auto-hevc-decode|--manual-fault|--recover-closed|--uvd-baseline|--uvd-only|--uvd-auto|--uvd-manual avc.nv12 hevc.au hevc.padded");
	bool noGfx = strcmp(argv[1], "--uvd-only") == 0 || strcmp(argv[1], "--uvd-baseline") == 0;
	bool uvd = noGfx || strcmp(argv[1], "--uvd-auto") == 0 || strcmp(argv[1], "--uvd-manual") == 0;
	bool baseline = strcmp(argv[1], "--baseline") == 0 || strcmp(argv[1], "--uvd-baseline") == 0;
	bool recoverClosed = strcmp(argv[1], "--recover-closed") == 0;
	bool waiters = strcmp(argv[1], "--auto-waiters") == 0;
	bool activeHevc = strcmp(argv[1], "--auto-hevc-decode") == 0;
	bool active = activeHevc || strcmp(argv[1], "--auto-decode") == 0;
	bool automatic = waiters || active || strcmp(argv[1], "--auto-cp") == 0
		|| strcmp(argv[1], "--uvd-only") == 0 || strcmp(argv[1], "--uvd-auto") == 0;
	Require(baseline || automatic || recoverClosed || uvd || strcmp(argv[1], "--manual-fault") == 0, "explicit recovery mode");
	sAvcReference = Read(argv[2]); sHevcReference = Read(argv[4]);
	auto au = Read(argv[3]); Require(au.size() >= 4, "HEVC access unit header");
	uint32 bytes = au[0] | uint32(au[1]) << 8 | uint32(au[2]) << 16 | uint32(au[3]) << 24;
	Require(bytes && bytes <= au.size() - 4, "complete first HEVC access unit");
	sHevc = new HevcStream();
	Require(sHevc->Prepare(au.data() + 4, bytes) && !sHevc->skipPicture && sHevc->poc == 0
		&& sHevc->picture.nal_type >= 16 && sHevc->picture.nal_type <= 20,
		"independently repeatable first HEVC random access picture");
	int monitor = Open();
	// MEMORY_INFO initializes SDMA/GART without initializing graphics.
	Memory initialMemory = Allocation(monitor);
	auto start = Info<amdgpu_recovery_info>(monitor, AMDGPU_RECOVERY_INFO);
	if (recoverClosed) {
		Require(start.status != B_OK && !start.live_handles && !start.live_vms && !start.live_videos
			&& !start.pending && start.videos && !Policy(monitor).enabled,
			"explicit recovery requires only closed quarantined owners and disabled automatic policy");
		Memory retained = Allocation(monitor);
		auto recovered = Info<amdgpu_recovery_info>(monitor, AMDGPU_GFX_RECOVER);
		Require(recovered.status == B_OK && recovered.stage == 7 && recovered.generation == start.generation + 1
			&& !recovered.videos && !recovered.buffers && !recovered.vms
			&& recovered.reclaimed_vram == start.quarantined_vram && recovered.reclaimed_ram == start.quarantined_ram,
			"one closed-resource reset retires firmware sessions and exact quarantine");
		Memory empty = {retained.vram - start.quarantined_vram, retained.ram - start.quarantined_ram};
		CheckAllocation(monitor, empty);
		Graphics fresh = GraphicsCreate(true); GraphicsControl(fresh); GraphicsClose(fresh);
		Decoder a = Create(false), b = Create(true); Decode(a); Decode(b); Destroy(a, false); Destroy(b, false);
		CheckAllocation(monitor, empty); Policy(monitor, 1);
		printf("PASS: explicit closed-video recovery generation %llu reclaimed %llu/%llu, fresh exact video/graphics\n",
			(unsigned long long)recovered.generation, (unsigned long long)recovered.reclaimed_vram,
			(unsigned long long)recovered.reclaimed_ram);
		close(monitor); delete sHevc; return 0;
	}
	Require(start.status == B_OK && !start.pending && !start.live_handles && !start.live_vms && !start.live_videos
		&& !start.buffers && !start.vms && !start.videos, "idle clean device baseline");
	Memory empty = initialMemory; auto policy = Policy(monitor);
	Require(policy.enabled == 1 && !policy.pending, "automatic recovery enabled initially");
	if (!baseline && !automatic) Policy(monitor, 0);
	Graphics graphics = {-1, {}};
	if (!noGfx) {
		graphics = GraphicsCreate(automatic); GraphicsControl(graphics);
		// Allocate the copied-command backing before measuring retained storage.
		std::vector<uint32> noop(256, 0x80000000); auto warm = Request<amdgpu_gfx_submit>();
		warm.address = (addr_t)noop.data(); warm.dwords = noop.size(); warm.flags = AMDGPU_GFX_VALIDATE_COMMANDS;
		Require(ioctl(graphics.fd, AMDGPU_GFX_SUBMIT_COPY, &warm, sizeof(warm)) == 0
			&& warm.status == B_OK && warm.completion, "warm copied-command storage");
	}
	Memory graphicsOnly = Allocation(monitor);
	Decoder d[4] = {Create(false), Create(true), Create(false), Create(true)};
	Memory all = graphicsOnly;
	for (auto& decoder : d) { Decode(decoder); all.vram += decoder.Storage().vram; all.ram += decoder.Storage().ram; }
	CheckAllocation(monitor, all);
	uint32 racingFrames = 0;
	if (!baseline) {
		if (uvd) UvdTimeout(monitor);
		else if (waiters) FaultWithWaiters(graphics, d[0]);
		else {
			VideoThread running = {&d[activeHevc ? 1 : 0], -1, true, 0, 0}; thread_id worker = -1;
			if (active) { worker = Start(running); WaitBusy(running.decoder->fd, running); }
			auto commands = Commands(automatic); auto submit = Request<amdgpu_gfx_submit>();
			submit.address = (addr_t)commands.data(); submit.dwords = commands.size(); submit.flags = AMDGPU_GFX_VALIDATE_COMMANDS;
			Require(ioctl(graphics.fd, AMDGPU_GFX_SUBMIT_COPY, &submit, sizeof(submit)) == 0
				&& submit.status == (automatic ? B_TIMED_OUT : B_BAD_DATA)
				&& (automatic ? !submit.completion && !submit.vm_fault_status[0] && !submit.vm_fault_status[1]
					: ((submit.vm_fault_status[0] | submit.vm_fault_status[1]) & 0xff) == 0x10),
				"one terminal graphics fault with four live decoder sessions");
			printf("Fault status %#x VM %#x/%#x ring %u/%u\n", (unsigned)submit.status,
				submit.vm_fault_status[0], submit.vm_fault_status[1], submit.rptr, submit.wptr);
			if (active) {
				Join(worker); racingFrames = atomic_get(&running.frames);
				Require(racingFrames > 0, "exact frames decoded concurrently with non-retiring graphics");
				printf("PASS: %u exact concurrent frames before permanent decoder loss\n", racingFrames);
			}
		}
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
			&& recovered.live_videos == (automatic ? 4u : 2u)
			&& recovered.live_handles == (noGfx ? 0u : 1u) && recovered.live_vms == (noGfx ? 0u : 1u)
			&& !recovered.videos && !recovered.vms && !recovered.buffers && !recovered.pending
			&& recovered.reclaimed_vram == reclaim.vram && recovered.reclaimed_ram == reclaim.ram
			&& !(recovered.srbm_reset & 0x8000), "one reset preserves live video storage, reclaims only closed ownership");
		Require(((recovered.srbm_reset & 0x40000) != 0) == uvd
			&& recovered.grbm_reset == (noGfx ? 0u : 0xf0005u),
			"expected UVD/GFX reset blocks, including video before graphics startup");
		auto afterPolicy = Policy(monitor);
		Require(afterPolicy.attempts == policy.attempts + automatic && afterPolicy.successes == policy.successes + automatic
			&& !afterPolicy.pending, "exact automatic attempt/success counters");
		all.vram -= reclaim.vram; all.ram -= reclaim.ram; CheckAllocation(monitor, all); if (!noGfx) GraphicsCheck(graphics);
		for (auto& decoder : d) if (decoder.fd >= 0) { Lost(decoder.fd, recovered.generation); Decode(decoder, true); }
		puts("PASS: live video remains allocated, closed sessions retired, every old output sentinel untouched");
	}
	for (auto& decoder : d) if (decoder.fd >= 0) {
		Memory bytes = decoder.Storage(); Destroy(decoder, !baseline);
		all.vram -= bytes.vram; all.ram -= bytes.ram; CheckAllocation(monitor, all);
	}
	CheckAllocation(monitor, graphicsOnly); if (!noGfx) GraphicsClose(graphics); CheckAllocation(monitor, empty);
	const bool videoOnlyBaseline = baseline && noGfx;
	Graphics fresh = {-1, {}};
	if (!videoOnlyBaseline) { fresh = GraphicsCreate(true); GraphicsControl(fresh); }
	for (unsigned round = 0; round < 8; round++) {
		Decoder a = Create(false), b = Create(true);
		Decode(a); Decode(b); Destroy(a, false); Destroy(b, false);
	}
	if (!videoOnlyBaseline) { GraphicsControl(fresh); GraphicsClose(fresh); }
	CheckAllocation(monitor, empty);
	if (!baseline && !automatic) Policy(monitor, policy.enabled);
	auto irq = Info<amdgpu_irq_info>(monitor, AMDGPU_IRQ_INFO);
	Require(irq.status == B_OK && (videoOnlyBaseline ? !irq.enabled : irq.enabled && irq.msi)
		&& irq.completed_fences == irq.waits
		&& !irq.vm_faults && !irq.privileged_faults && !irq.overflows && !irq.unknown && irq.rptr == irq.wptr,
		"fresh IRQ epoch is healthy");
	if (!baseline) Require(irq.completed_fences == (noGfx ? 2u : 3u), "expected private reset proof and fresh graphics controls");
	auto final = Info<amdgpu_recovery_info>(monitor, AMDGPU_RECOVERY_INFO);
	Require(final.status == B_OK && !final.live_handles && !final.live_vms && !final.live_videos
		&& !final.pending && !final.buffers && !final.vms && !final.videos, "no leaked owners or quarantine");
	printf("PASS: %s, %u exact decoded frames, 16 recreated sessions, generation %llu, healthy fences %llu\n",
		argv[1], 20 + racingFrames, (unsigned long long)final.generation, (unsigned long long)irq.completed_fences);
	close(monitor); delete sHevc; return 0;
}
