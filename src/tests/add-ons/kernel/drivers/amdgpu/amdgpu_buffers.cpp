/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include <amdgpu_haiku.h>
#include <OS.h>
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

static bool sSystemBuffers;

static void
Require(bool okay, const char* message)
{
	if (!okay) {
		fprintf(stderr, "FAIL: %s (errno %d: %s)\n", message, errno, strerror(errno));
		exit(1);
	}
}

template<typename T> static T
Request()
{
	T request = {};
	request.version = AMDGPU_HAIKU_ABI_VERSION;
	request.size = sizeof(T);
	return request;
}

static int
Open()
{
	int fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
	Require(fd >= 0, "open client");
	return fd;
}

static amdgpu_memory_info
Info(int fd)
{
	auto info = Request<amdgpu_memory_info>();
	Require(ioctl(fd, AMDGPU_MEMORY_INFO, &info, sizeof(info)) == 0, "memory info");
	Require(info.faulted == 0, "device remains healthy");
	auto gart = Request<amdgpu_gart_info>();
	Require(ioctl(fd, AMDGPU_GART_INFO, &gart, sizeof(gart)) == 0, "GART info");
	Require((gart.vm_fault_status & 0xff) == 0, "no GPU VM faults");
	if (sSystemBuffers) {
		info.allocated_bytes = gart.allocated_bytes;
		info.client_bytes = gart.client_bytes;
	}
	return info;
}

static amdgpu_buffer
Create(int fd, uint64 bytes, bool map = true, uint32 operation = 0)
{
	auto bo = Request<amdgpu_buffer>();
	bo.bytes = bytes;
	if (operation == 0)
		operation = sSystemBuffers ? AMDGPU_CREATE_SYSTEM_BUFFER : AMDGPU_CREATE_BUFFER;
	Require(ioctl(fd, operation, &bo, sizeof(bo)) == 0, "create buffer");
	Require(bo.bytes >= bytes && bo.handle != 0, "buffer dimensions");
	if (map)
		Require(ioctl(fd, AMDGPU_MAP_BUFFER, &bo, sizeof(bo)) == 0, "map buffer");
	return bo;
}

static uint64
Submit(int fd, uint32 operation, uint64 src, uint64 dst, uint64 srcOffset,
	uint64 dstOffset, uint64 bytes, uint32 value = 0)
{
	auto c = Request<amdgpu_dma_submit>();
	c.operation = operation;
	c.source = src; c.destination = dst;
	c.source_offset = srcOffset; c.destination_offset = dstOffset;
	c.bytes = bytes; c.value = value;
	Require(ioctl(fd, AMDGPU_SUBMIT_DMA, &c, sizeof(c)) == 0, "submit job");
	Require(c.fence != 0, "fence assigned");
	return c.fence;
}

static void
Wait(int fd, uint64 fence)
{
	auto wait = Request<amdgpu_fence_wait>();
	wait.fence = fence;
	bigtime_t deadline = system_time() + 5000000;
	int result;
	do {
		wait.timeout_us = deadline - system_time();
		Require(wait.timeout_us > 0, "fence deadline");
		result = ioctl(fd, AMDGPU_WAIT_FENCE, &wait, sizeof(wait));
	} while (result < 0 && errno == B_INTERRUPTED);
	Require(result == 0, "wait fence");
	Require(wait.status == B_OK, "GPU job completed successfully");
}

static void
Free(int fd, amdgpu_buffer bo)
{
	Require(ioctl(fd, AMDGPU_FREE_BUFFER, &bo, sizeof(bo)) == 0, "free buffer");
}

static sigjmp_buf sFaultReturn;
static void Fault(int) { siglongjmp(sFaultReturn, 1); }

static bool
Revoked(uint64 address)
{
	struct sigaction handler = {}, previous;
	handler.sa_handler = Fault;
	sigemptyset(&handler.sa_mask);
	Require(sigaction(SIGSEGV, &handler, &previous) == 0, "install mapping fault handler");
	bool fault = false;
	if (sigsetjmp(sFaultReturn, 1) == 0) {
		volatile uint32 value = *(volatile uint32*)(addr_t)address;
		(void)value;
	} else
		fault = true;
	Require(sigaction(SIGSEGV, &previous, NULL) == 0, "restore fault handler");
	return fault;
}

static uint32 Pattern(uint64 index, uint32 seed = 0)
{
	return 0x395a17e2u ^ (uint32)(index * 0x10204081u) ^ seed;
}

static int32
ConcurrentClient(void* data)
{
	uint32 seed = (uint32)(addr_t)data;
	int fd = Open();
	auto src = Create(fd, 32768);
	auto dst = Create(fd, 32768);
	volatile uint32* a = (volatile uint32*)(addr_t)src.address;
	volatile uint32* b = (volatile uint32*)(addr_t)dst.address;
	for (uint32 round = 0; round < 16; round++) {
		for (unsigned i = 0; i < 8192; i++)
			a[i] = Pattern(i, seed + round);
		Wait(fd, Submit(fd, AMDGPU_DMA_COPY, src.handle, dst.handle, 0, 0, 32768));
		for (unsigned i = 0; i < 8192; i++)
			Require(b[i] == Pattern(i, seed + round), "concurrent client's pixels");
	}
	close(fd); // closes handles and revokes mappings
	delete_area(src.area);
	delete_area(dst.area);
	return 0;
}

struct StartupBarrier {
	sem_id ready, go;
};

static int32
FirstClient(void* argument)
{
	StartupBarrier* barrier = (StartupBarrier*)argument;
	int fd = Open();
	Require(release_sem(barrier->ready) == B_OK, "first client ready");
	Require(acquire_sem(barrier->go) == B_OK, "first client barrier");
	Require(Info(fd).allocated_bytes == 0, "first client starts a healthy empty device");
	close(fd);
	return 0;
}

int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc < 2 || argc > 4) {
		fprintf(stderr, "usage: amdgpu_buffers firmware | --reuse | --auto [--system] [--unprivileged]\n");
		return 2;
	}
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--system") == 0)
			sSystemBuffers = true;
		else {
			Require((strcmp(argv[1], "--reuse") == 0 || strcmp(argv[1], "--auto") == 0)
				&& strcmp(argv[i], "--unprivileged") == 0, "unprivileged usage");
			Require(setuid(65534) == 0 && geteuid() == 65534, "drop root privileges");
		}
	}
	printf("Testing %s buffers, effective UID %u\n",
		sSystemBuffers ? "system RAM" : "VRAM", (unsigned)geteuid());
	int fd = Open();
	if (strcmp(argv[1], "--reuse") != 0 && strcmp(argv[1], "--auto") != 0) {
		FILE* file = fopen(argv[1], "rb");
		Require(file != NULL, "open firmware");
		Require(fseek(file, 0, SEEK_END) == 0, "firmware seek");
		long size = ftell(file);
		Require(size >= 52 && size <= 65536, "firmware size");
		rewind(file);
		std::vector<uint8> firmware(size);
		Require(fread(firmware.data(), 1, size, file) == (size_t)size, "read firmware");
		fclose(file);
		auto init = Request<amdgpu_dma_init>();
		init.firmware = (addr_t)firmware.data();
		init.firmware_size = size;
		Require(ioctl(fd, AMDGPU_START_DMA, &init, sizeof(init)) == 0, "initialize persistent DMA");
	}
	if (geteuid() != 0) {
		Require(ioctl(fd, AMDGPU_SMC_BOOTSTRAP, NULL, 0) == -1 && errno == B_NOT_ALLOWED,
			"unprivileged firmware startup denied");
		Require(ioctl(fd, AMDGPU_GFX_TEST, NULL, 0) == -1 && errno == B_NOT_ALLOWED,
			"unprivileged GFX startup denied");
		Require(ioctl(fd, AMDGPU_GFX_MEC_TEST, NULL, 0) == -1 && errno == B_NOT_ALLOWED,
			"unprivileged MEC firmware startup denied");
		Require(ioctl(fd, AMDGPU_UVD_TEST, NULL, 0) == -1 && errno == B_NOT_ALLOWED,
			"unprivileged UVD startup denied");
	}
	if (strcmp(argv[1], "--auto") == 0) {
		StartupBarrier barrier = {create_sem(0, "startup ready"), create_sem(0, "startup go")};
		Require(barrier.ready >= 0 && barrier.go >= 0, "create startup barriers");
		thread_id threads[4];
		for (thread_id& thread : threads) {
			thread = spawn_thread(FirstClient, "first GPU client", B_NORMAL_PRIORITY, &barrier);
			Require(thread >= 0 && resume_thread(thread) == B_OK, "start first GPU client");
		}
		for (unsigned i = 0; i < 4; i++)
			Require(acquire_sem(barrier.ready) == B_OK, "all first clients ready");
		Require(release_sem_etc(barrier.go, 4, 0) == B_OK, "release first clients together");
		for (thread_id thread : threads) {
			status_t result;
			Require(wait_for_thread(thread, &result) == B_OK && result == B_OK, "join first client");
		}
		delete_sem(barrier.ready);
		delete_sem(barrier.go);
		puts("PASS: four simultaneous first clients use installed firmware without privileged startup");
	}
	Require(Info(fd).allocated_bytes == 0, "initial allocation accounting");
	const uint64 bytes = 8ULL << 20;
	auto src = Create(fd, bytes + 8192);
	auto dst = Create(fd, bytes + 8192);
	volatile uint32* a = (volatile uint32*)(addr_t)src.address;
	volatile uint32* b = (volatile uint32*)(addr_t)dst.address;
	for (uint64 i = 0; i < src.bytes / 4; i++) {
		Require(a[i] == 0 && b[i] == 0, "new buffer is zeroed");
		a[i] = Pattern(i);
	}
	Wait(fd, Submit(fd, AMDGPU_DMA_FILL, 0, dst.handle, 0, 0, dst.bytes, 0xcccccccc));
	Wait(fd, Submit(fd, AMDGPU_DMA_COPY, src.handle, dst.handle, 4096, 4096, bytes));
	for (uint64 i = 0; i < dst.bytes / 4; i++) {
		uint32 expected = i >= 1024 && i < (4096 + bytes) / 4 ? Pattern(i) : 0xcccccccc;
		if (b[i] != expected) {
			fprintf(stderr, "byte %#" B_PRIx64 ": %#x expected %#x\n", i * 4, b[i], expected);
			Require(false, "multi-packet copy and guards");
		}
	}
	puts("PASS: mapped zeroed buffers, 8 MiB multi-packet copy, fill and guards");
	uint64 batchFence = 0;
	for (unsigned i = 0; i < 32; i++)
		batchFence = Submit(fd, AMDGPU_DMA_FILL, 0, dst.handle, 0, 0, 4096, Pattern(i));
	Wait(fd, batchFence);
	for (unsigned i = 0; i < 1024; i++)
		Require(b[i] == Pattern(31), "queued fills execute in submission order");
	puts("PASS: 32 ordered asynchronous submissions and final fence");

	int other = Open();
	auto foreign = src;
	Require(ioctl(other, AMDGPU_MAP_BUFFER, &foreign, sizeof(foreign)) == -1
		&& errno == B_BAD_VALUE, "cross-client map rejected");
	Require(ioctl(other, AMDGPU_FREE_BUFFER, &foreign, sizeof(foreign)) == -1
		&& errno == B_BAD_VALUE, "cross-client free rejected");
	auto invalid = Request<amdgpu_dma_submit>();
	invalid.operation = AMDGPU_DMA_COPY; invalid.source = src.handle;
	invalid.destination = dst.handle; invalid.bytes = 4;
	Require(ioctl(other, AMDGPU_SUBMIT_DMA, &invalid, sizeof(invalid)) == -1
		&& errno == B_BAD_VALUE, "cross-client submit rejected");
	invalid.source_offset = UINT64_MAX - 3;
	Require(ioctl(fd, AMDGPU_SUBMIT_DMA, &invalid, sizeof(invalid)) == -1
		&& errno == B_BAD_VALUE, "overflowing range rejected");
	invalid.source_offset = 0; invalid.destination = src.handle;
	Require(ioctl(fd, AMDGPU_SUBMIT_DMA, &invalid, sizeof(invalid)) == -1
		&& errno == B_BAD_VALUE, "overlapping DMA copy rejected");
	auto badSize = Request<amdgpu_buffer>(); badSize.bytes = UINT64_MAX;
	Require(ioctl(fd, sSystemBuffers ? AMDGPU_CREATE_SYSTEM_BUFFER : AMDGPU_CREATE_BUFFER,
		&badSize, sizeof(badSize)) == -1
		&& errno == B_BAD_VALUE, "overflowing allocation rejected");
	auto wait = Request<amdgpu_fence_wait>(); wait.fence = UINT64_MAX;
	Require(ioctl(fd, AMDGPU_WAIT_FENCE, &wait, sizeof(wait)) == -1
		&& errno == B_BAD_VALUE, "unsubmitted fence rejected");
	puts("PASS: client isolation, stale/overflowing ranges and invalid fences rejected");

	void* cloneAddress = NULL;
	area_id clone = clone_area("amdgpu test clone", &cloneAddress, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, src.area);
	Require(clone >= 0, "clone own mapping");
	Free(fd, src);
	Require(Revoked(src.address) && Revoked((addr_t)cloneAddress), "free revokes all mappings");
	Require(ioctl(fd, AMDGPU_MAP_BUFFER, &src, sizeof(src)) == -1 && errno == B_BAD_VALUE,
		"stale handle rejected");
	delete_area(src.area); delete_area(clone);
	Free(fd, dst); delete_area(dst.area);
	Require(Info(fd).allocated_bytes == 0, "explicit free reclaims memory");
	puts("PASS: mapping and clone revocation before memory reuse");

	thread_id threads[4];
	for (unsigned i = 0; i < 4; i++) {
		threads[i] = spawn_thread(ConcurrentClient, "amdgpu concurrent client", B_NORMAL_PRIORITY,
			(void*)(addr_t)(i * 100));
		Require(threads[i] >= 0 && resume_thread(threads[i]) == B_OK, "start concurrent client");
	}
	for (thread_id thread : threads) {
		status_t result;
		Require(wait_for_thread(thread, &result) == B_OK && result == 0, "join concurrent client");
	}
	Require(Info(fd).allocated_bytes == 0, "concurrent clients reclaim memory");
	puts("PASS: four independent clients, 64 queued copies with verified data");

	int closing = Open();
	auto pending = Create(closing, bytes);
	for (unsigned i = 0; i < 48; i++)
		Submit(closing, AMDGPU_DMA_FILL, 0, pending.handle, 0, 0, bytes, i + 1);
	uint32 queued = Info(fd).pending_jobs;
	Require(queued != 0, "close test has in-flight jobs");
	close(closing);
	Require(Revoked(pending.address), "close revokes mapping with pending jobs");
	delete_area(pending.area);
	bigtime_t deadline = system_time() + 5000000;
	while (Info(fd).pending_jobs != 0 && system_time() < deadline)
		snooze(1000);
	Require(Info(fd).pending_jobs == 0 && Info(fd).allocated_bytes == 0,
		"jobs outlive closed file, then release buffers");
	printf("PASS: close with %u queued jobs, mapping revoked and memory reclaimed\n", (unsigned)queued);

	for (unsigned i = 0; i < 128; i++) {
		auto reuse = Create(fd, 4096);
		volatile uint32* p = (volatile uint32*)(addr_t)reuse.address;
		for (unsigned j = 0; j < 1024; j++)
			Require(p[j] == 0, "reused allocation is zeroed");
		Wait(fd, Submit(fd, AMDGPU_DMA_FILL, 0, reuse.handle, 0, 0, 4096, 0x19283746));
		Require(p[0] == 0x19283746 && p[1023] == 0x19283746, "reused GPU allocation");
		Free(fd, reuse); delete_area(reuse.area);
	}
	Require(Info(fd).allocated_bytes == 0, "final allocation accounting");
	if (sSystemBuffers) {
		// Simultaneous mappings exceed the card's 256 MiB CPU VRAM aperture.
		// The last 64 MiB fill also crosses sixteen SDMA packet boundaries.
		amdgpu_buffer large[5];
		uint64 last = 0;
		for (unsigned i = 0; i < 5; i++) {
			large[i] = Create(fd, 64ULL << 20);
			last = Submit(fd, AMDGPU_DMA_FILL, 0, large[i].handle, 0, 0,
				large[i].bytes, Pattern(i));
		}
		Require(Info(fd).allocated_bytes == (320ULL << 20), "320 MiB mapped in GART");
		Wait(fd, last);
		for (unsigned i = 0; i < 5; i++) {
			volatile uint32* p = (volatile uint32*)(addr_t)large[i].address;
			for (uint64 j = 0; j < large[i].bytes / 4; j++)
				Require(p[j] == Pattern(i), "64 MiB GPU fill in system RAM");
			Free(fd, large[i]); delete_area(large[i].area);
		}
		Require(Info(fd).allocated_bytes == 0, "large RAM buffers reclaimed");
		puts("PASS: 320 MiB simultaneous RAM mappings and five complete 64 MiB fills");
	}

	// Exercise both domains in one dependency chain. CPU writes only the
	// source RAM; every destination and guard is produced by actual SDMA.
	const uint64 mixedBytes = (8ULL << 20) + 8192;
	auto ramIn = Create(fd, mixedBytes, true, AMDGPU_CREATE_SYSTEM_BUFFER);
	auto vram = Create(fd, mixedBytes, true, AMDGPU_CREATE_BUFFER);
	auto ramOut = Create(fd, mixedBytes, true, AMDGPU_CREATE_SYSTEM_BUFFER);
	volatile uint32* in = (volatile uint32*)(addr_t)ramIn.address;
	volatile uint32* out = (volatile uint32*)(addr_t)ramOut.address;
	for (uint64 i = 0; i < mixedBytes / 4; i++)
		in[i] = Pattern(i, 0xa129c45b);
	Submit(fd, AMDGPU_DMA_FILL, 0, vram.handle, 0, 0, mixedBytes, 0xabcdef98);
	Submit(fd, AMDGPU_DMA_FILL, 0, ramOut.handle, 0, 0, mixedBytes, 0x76543210);
	Submit(fd, AMDGPU_DMA_COPY, ramIn.handle, vram.handle, 4, 4092, bytes);
	Wait(fd, Submit(fd, AMDGPU_DMA_COPY, vram.handle, ramOut.handle, 4092, 4100, bytes));
	for (uint64 i = 0; i < mixedBytes / 4; i++) {
		uint32 expected = i >= 1025 && i < 1025 + bytes / 4
			? Pattern(i - 1024, 0xa129c45b) : 0x76543210;
		Require(out[i] == expected, "RAM to VRAM to RAM across unaligned page boundaries");
	}
	// Dirty cached CPU source lines again between submissions, then read
	// the same cached destination lines. This detects missing PCIe snooping.
	for (unsigned round = 0; round < 64; round++) {
		for (unsigned i = 0; i < 4096; i++)
			in[i] = Pattern(i, round);
		Wait(fd, Submit(fd, AMDGPU_DMA_COPY, ramIn.handle, ramOut.handle, 0, 0, 16384));
		for (unsigned i = 0; i < 4096; i++)
			Require(out[i] == Pattern(i, round), "cached CPU and GPU writes remain coherent");
	}
	Free(fd, ramIn); Free(fd, vram); Free(fd, ramOut);
	delete_area(ramIn.area); delete_area(vram.area); delete_area(ramOut.area);
	auto gart = Request<amdgpu_gart_info>();
	Require(ioctl(fd, AMDGPU_GART_INFO, &gart, sizeof(gart)) == 0, "final GART stats");
	Require(gart.allocated_bytes == 0 && gart.client_bytes == 0
		&& (gart.vm_fault_status & 0xff) == 0, "GART released without VM faults");
	printf("PASS: RAM/VRAM transfers, cached CPU coherency, %" B_PRIu64
		" bound pages, %" B_PRIu64 " physical discontinuities\n",
		gart.bound_pages, gart.scatter_boundaries);
	Require(Info(fd).allocated_bytes == 0, "mixed transfers reclaim memory");
	close(other); close(fd);
	fd = Open();
	Require(Info(fd).allocated_bytes == 0, "persistent device survives all clients closing");
	close(fd);
	puts("PASS: 128 allocation/reuse cycles and persistent device reopen");
	puts("PASS: AMD client buffers and asynchronous DMA");
	return 0;
}
