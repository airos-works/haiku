/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include <amdgpu_haiku.h>
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

static volatile uint32* sGate;
static void ReleaseGate()
{
	if (sGate != NULL) { *sGate = 1; __sync_synchronize(); sGate = NULL; }
}
static void Require(bool value, const char* why)
{
	if (!value) { ReleaseGate(); fprintf(stderr, "FAIL: %s: %s\n", why, strerror(errno)); exit(1); }
}
template<typename T> static T Request()
{
	T r = {}; r.version = AMDGPU_HAIKU_ABI_VERSION; r.size = sizeof(r); return r;
}
static int Open()
{
	int fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
	Require(fd >= 0, "open client"); return fd;
}
static std::vector<uint8> sReference;
struct Decoder {
	int fd;
	amdgpu_video_create session;
	uint32 frames;
	int32 done;
};
static Decoder Create(uint32 frames)
{
	Decoder d = {}; d.fd = Open(); d.frames = frames;
	d.session = Request<amdgpu_video_create>();
	d.session.config = {864, 480, 100, 30, 2, 0};
	Require(ioctl(d.fd, AMDGPU_VIDEO_CREATE, &d.session, sizeof(d.session)) == 0,
		"create video session");
	Require(d.session.output_bytes == sReference.size(), "reference geometry");
	return d;
}
static int32 Decode(void* data)
{
	Decoder& d = *(Decoder*)data;
	std::vector<uint8> pixels(d.session.output_bytes);
	for (uint32 i = 0; i < d.frames; i++) {
		auto r = Request<amdgpu_video_decode>();
		r.handle = d.session.handle;
		r.bitstream = (addr_t)uvd_bitstream; r.bitstream_bytes = sizeof(uvd_bitstream);
		r.output = (addr_t)pixels.data(); r.output_capacity = pixels.size();
		auto& p = r.picture;
		p.flags = AMDGPU_H264_IDR; p.sps_flags = 5; p.pps_flags = 0x88;
		p.log2_frame_num_minus4 = 1; p.log2_poc_lsb_minus4 = 3; p.initial_qp_minus26 = 2;
		memset(p.scaling4x4, 16, sizeof(p.scaling4x4));
		memset(p.scaling8x8, 16, sizeof(p.scaling8x8));
		Require(ioctl(d.fd, AMDGPU_VIDEO_DECODE, &r, sizeof(r)) == 0, "concurrent decode");
		Require(r.sequence != 0 && r.sequence == r.fence && r.rptr == r.wptr
			&& r.guard_mismatches == 0 && (r.vm_fault_status & 0xff) == 0,
			"decode completion and guards");
		Require(pixels == sReference, "every decoded byte equals independent reference");
	}
	atomic_set(&d.done, 1);
	return B_OK;
}
static thread_id Start(Decoder& d)
{
	thread_id t = spawn_thread(Decode, "concurrent video decode", B_NORMAL_PRIORITY, &d);
	Require(t >= 0 && resume_thread(t) == B_OK, "start decoder"); return t;
}
static void Join(thread_id t)
{
	status_t result;
	Require(wait_for_thread(t, &result) == B_OK && result == B_OK, "join decoder");
}
static bool Busy(const Decoder& d)
{
	// Never destroys the session, even if the decode retires between calls.
	auto r = Request<amdgpu_video_destroy>(); r.handle = 0;
	Require(ioctl(d.fd, AMDGPU_VIDEO_DESTROY, &r, sizeof(r)) == -1
		&& (errno == B_BUSY || errno == B_BAD_VALUE), "same-file session guard");
	return errno == B_BUSY;
}
static void WaitBusy(Decoder& d)
{
	bigtime_t deadline = system_time() + 5000000;
	while (!Busy(d)) {
		Require(atomic_get(&d.done) == 0 && system_time() < deadline,
			"observe an in-flight decode without blocking behind it");
		snooze(50);
	}
}
static amdgpu_buffer Buffer(int fd)
{
	auto b = Request<amdgpu_buffer>(); b.bytes = 16384;
	Require(ioctl(fd, AMDGPU_CREATE_SYSTEM_BUFFER, &b, sizeof(b)) == 0
		&& ioctl(fd, AMDGPU_MAP_BUFFER, &b, sizeof(b)) == 0, "create mapped RAM buffer");
	return b;
}
static void QueueOrder(int fd, const amdgpu_buffer& buffer, uint64 address, bool serialized, bool dma = false)
{
	Decoder d = Create(1);
	auto words = (volatile uint32*)(addr_t)buffer.address;
	memset((void*)words, 0, buffer.bytes); sGate = words;
	std::vector<uint32> commands = {0xc0053c00, 0x13, uint32(address), uint32(address >> 32),
		1, 0xffffffff, 4, 0xc0033700, 0x100500, uint32(address + 4), uint32(address >> 32), 0x51005100};
	commands.resize(256, 0x80000000);
	auto q = Request<amdgpu_gfx_queue>(); q.address = (addr_t)commands.data();
	q.dwords = commands.size(); q.flags = AMDGPU_GFX_VALIDATE_COMMANDS;
	Require(ioctl(fd, AMDGPU_GFX_QUEUE_COPY, &q, sizeof(q)) == 0, "queue owned GPU gate before video");
	auto wait = Request<amdgpu_fence_wait>(); wait.fence = q.fence;
	if (dma) {
		auto fill = Request<amdgpu_dma_submit>(); fill.operation = AMDGPU_DMA_FILL;
		fill.destination = buffer.handle; fill.destination_offset = 8; fill.bytes = 4; fill.value = 0x5100da00;
		Require(ioctl(fd, AMDGPU_SUBMIT_DMA, &fill, sizeof(fill)) == 0, "queue DMA after gated graphics");
		wait.fence = fill.fence;
	}
	Require(ioctl(fd, AMDGPU_WAIT_FENCE, &wait, sizeof(wait)) == -1 && errno == B_WOULD_BLOCK,
		"graphics gate is pending before starting decode");
	thread_id thread = Start(d);
	bigtime_t deadline = system_time() + 100000;
	while (atomic_get(&d.done) == 0 && system_time() < deadline) snooze(100);
	bool completed = atomic_get(&d.done) != 0;
	Require(words[0] == 0 && words[1] == 0 && words[2] == 0, "GPU gate stayed closed throughout decode observation");
	ReleaseGate(); Join(thread);
	wait.timeout_us = 5000000;
	Require(ioctl(fd, AMDGPU_WAIT_FENCE, &wait, sizeof(wait)) == 0 && wait.status == B_OK,
		"gated graphics completes after release");
	for (uint32 i = 0; i < buffer.bytes / 4; ++i)
		Require(words[i] == (i == 0 ? 1u : i == 1 ? 0x51005100u : i == 2 && dma ? 0x5100da00u : 0u),
			"gate/DMA output and all guards");
	close(d.fd);
	Require(completed != serialized, dma ? "queued DMA remains ordered before decode"
		: serialized ? "old kernel serializes decode behind queued GFX"
		: "decode finishes while earlier queued graphics is still blocked");
	puts(dma ? "PASS: pending ordinary DMA still excludes video; graphics/DMA output and decoded pixels exact"
		: serialized ? "PASS: reproduced old queued-GFX/video serialization without GPU fault"
		: "PASS: video starts and decodes exact pixels while earlier queued graphics remains gated");
}
int main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc == 2 && strcmp(argv[1], "--expect-no-device") == 0) {
		int fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
		Require(fd < 0 && errno == B_ENTRY_NOT_FOUND, "no GPU device in emulator");
		puts("PASS: no video/graphics device; no GPU submission"); return 0;
	}
	bool queue = argc == 3 && strcmp(argv[2], "--queue-order") == 0;
	bool serialized = argc == 3 && strcmp(argv[2], "--expect-serialized") == 0;
	bool unprivileged = argc == 3 && strcmp(argv[2], "--unprivileged") == 0;
	Require(argc == 2 || (argc == 3 && (queue || serialized || unprivileged)),
		"usage: amdgpu_video_concurrent reference.nv12 [--unprivileged|--queue-order|--expect-serialized]");
	FILE* f = fopen(argv[1], "rb"); Require(f != NULL, "open independent reference");
	sReference.resize(737280);
	Require(fread(sReference.data(), 1, sReference.size(), f) == sReference.size()
		&& fgetc(f) == EOF && fclose(f) == 0, "read complete padded NV12 reference");
	if (unprivileged) Require(setgid(65534) == 0 && setuid(65534) == 0, "drop privileges");
	int graphics = Open();
	auto vm = Request<amdgpu_vm_info>();
	Require(ioctl(graphics, AMDGPU_VM_INFO, &vm, sizeof(vm)) == 0, "initialize VM");
	auto buffer = Buffer(graphics);
	auto map = Request<amdgpu_vm_mapping>();
	map.handle = buffer.handle; map.address = 0x100000000; map.bytes = buffer.bytes;
	map.permissions = AMDGPU_VM_READ | AMDGPU_VM_WRITE;
	Require(ioctl(graphics, AMDGPU_VM_MAP, &map, sizeof(map)) == 0, "bind graphics buffer");
	auto gpu = Request<amdgpu_vm_test>(); gpu.address = map.address;
	Require(ioctl(graphics, AMDGPU_VM_TEST, &gpu, sizeof(gpu)) == 0 && gpu.status == B_OK,
		"initialize graphics before concurrent video");
	if (queue || serialized) {
		QueueOrder(graphics, buffer, map.address, serialized);
		if (queue) QueueOrder(graphics, buffer, map.address, true, true);
		close(graphics); Require(delete_area(buffer.area) == B_OK, "release revoked CPU clone");
		return 0;
	}
	Decoder a = Create(64), b = Create(64);
	thread_id ta = Start(a), tb = Start(b);
	WaitBusy(a);
	uint32 busyChecks = 1;
	for (uint32 round = 0; round < 64; round++) {
		busyChecks += Busy(a) || Busy(b);
		gpu.value = 0x51000000 ^ round;
		Require(ioctl(graphics, AMDGPU_VM_TEST, &gpu, sizeof(gpu)) == 0 && gpu.status == B_OK
			&& gpu.completion != 0 && gpu.vm_fault_status[0] == 0 && gpu.vm_fault_status[1] == 0,
			"graphics completion during video load");
		volatile uint32* words = (volatile uint32*)(addr_t)buffer.address;
		for (uint32 i = 0; i < buffer.bytes / 4; i++)
			Require(words[i] == (i < 1024 ? gpu.value ^ (i * 0x10204081u) : 0),
				"graphics pixels and untouched guard bytes");
		// Queue SDMA while decoders may hold the reservation. Its fence must
		// retire correctly, and the next graphics submit must see its clear.
		auto dma = Request<amdgpu_dma_submit>();
		dma.operation = AMDGPU_DMA_FILL; dma.destination = buffer.handle; dma.bytes = buffer.bytes;
		Require(ioctl(graphics, AMDGPU_SUBMIT_DMA, &dma, sizeof(dma)) == 0, "queue DMA under video load");
		auto wait = Request<amdgpu_fence_wait>(); wait.fence = dma.fence; wait.timeout_us = 5000000;
		Require(ioctl(graphics, AMDGPU_WAIT_FENCE, &wait, sizeof(wait)) == 0 && wait.status == B_OK,
			"DMA fence during video load");
		for (uint32 i = 0; i < buffer.bytes / 4; i++) Require(words[i] == 0, "complete DMA clear");
	}
	Join(ta); Join(tb); close(a.fd); close(b.fd);
	printf("PASS: 128 reference-exact frames, 64 graphics jobs, 64 DMA jobs, %u busy observations\n",
		busyChecks);
	// Close the final descriptor after observing its active ioctl. The kernel
	// must pin that file/session until retirement, then permit memory reuse.
	for (uint32 round = 0; round < 16; round++) {
		Decoder d = Create(1); thread_id t = Start(d);
		WaitBusy(d);
		Require(close(d.fd) == 0, "close descriptor during decode");
		Join(t);
	}
	close(graphics); Require(delete_area(buffer.area) == B_OK, "release revoked CPU clone");
	int monitor = Open();
	auto info = Request<amdgpu_memory_info>(); auto ram = Request<amdgpu_gart_info>();
	Require(ioctl(monitor, AMDGPU_MEMORY_INFO, &info, sizeof(info)) == 0
		&& info.allocated_bytes == 0 && info.pending_jobs == 0 && !info.faulted,
		"all VRAM reclaimed after concurrent closes");
	Require(ioctl(monitor, AMDGPU_GART_INFO, &ram, sizeof(ram)) == 0
		&& ram.allocated_bytes == 0 && (ram.vm_fault_status & 0xff) == 0,
		"all private readback and graphics RAM reclaimed");
	close(monitor);
	puts("PASS: 16 in-flight close/reuse cycles with exact pixels and no retained allocations");
}
