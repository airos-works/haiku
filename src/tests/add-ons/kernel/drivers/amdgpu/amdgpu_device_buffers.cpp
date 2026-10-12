/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
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

static void
Require(bool okay, const char* message)
{
	if (!okay) {
		fprintf(stderr, "FAIL: %s (%d: %s)\n", message, errno, strerror(errno));
		exit(1);
	}
}

template<typename T> static T
Request()
{
	T value = {};
	value.version = AMDGPU_HAIKU_ABI_VERSION;
	value.size = sizeof(value);
	return value;
}

static amdgpu_memory_info
Info(int fd)
{
	auto value = Request<amdgpu_memory_info>();
	Require(ioctl(fd, AMDGPU_MEMORY_INFO, &value, sizeof(value)) == 0,
		"memory info");
	Require(value.faulted == 0, "healthy device");
	auto gart = Request<amdgpu_gart_info>();
	Require(ioctl(fd, AMDGPU_GART_INFO, &gart, sizeof(gart)) == 0
		&& (gart.vm_fault_status & 0xff) == 0, "no VM fault");
	return value;
}

static amdgpu_buffer
Create(int fd, uint32 operation, uint64 bytes, bool map)
{
	auto value = Request<amdgpu_buffer>();
	value.bytes = bytes;
	Require(ioctl(fd, operation, &value, sizeof(value)) == 0, "create buffer");
	Require(value.handle != 0 && value.bytes == bytes && value.area == -1
		&& value.address == 0, "opaque buffer identity");
	if (map)
		Require(ioctl(fd, AMDGPU_MAP_BUFFER, &value, sizeof(value)) == 0,
			"map staging buffer");
	return value;
}

static uint64
Submit(int fd, uint32 operation, uint64 source, uint64 destination,
	uint64 bytes, uint32 value = 0)
{
	auto request = Request<amdgpu_dma_submit>();
	request.operation = operation;
	request.source = source;
	request.destination = destination;
	request.bytes = bytes;
	request.value = value;
	Require(ioctl(fd, AMDGPU_SUBMIT_DMA, &request, sizeof(request)) == 0,
		"submit DMA");
	return request.fence;
}

static void
Wait(int fd, uint64 fence)
{
	auto request = Request<amdgpu_fence_wait>();
	request.fence = fence;
	request.timeout_us = 5000000;
	Require(ioctl(fd, AMDGPU_WAIT_FENCE, &request, sizeof(request)) == 0
		&& request.status == B_OK, "DMA completed");
}

static uint32
Pattern(uint64 word, uint32 block)
{
	return (uint32)(word * 0x10204081u) ^ (block * 0x591e8307u) ^ 0xa1324bf7u;
}

int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc == 2 && strcmp(argv[1], "--unprivileged") == 0)
		Require(setgid(65534) == 0 && setuid(65534) == 0, "drop privileges");
	else
		Require(argc == 1, "usage: amdgpu_device_buffers [--unprivileged]");
	int fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
	int observer = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
	Require(fd >= 0 && observer >= 0, "open independent clients");
	auto initial = Info(fd);
	Require(initial.allocated_bytes == 0 && initial.pending_jobs == 0,
		"isolated test requires idle empty device");
	Require(initial.total_vram > initial.visible_vram, "VRAM beyond CPU aperture");
	for (uint32 bad = 0; bad < 3; bad++) {
		auto request = Request<amdgpu_buffer>();
		request.bytes = bad == 0 ? 0 : (bad == 1 ? (64ULL << 20) + 1 : 4096);
		request.reserved = bad == 2 ? 1 : 0;
		Require(ioctl(fd, AMDGPU_CREATE_DEVICE_BUFFER, &request, sizeof(request)) < 0
			&& errno == B_BAD_VALUE, "malformed device allocation rejected");
	}
	Require(Info(fd).allocated_bytes == 0, "invalid allocations consume no VRAM");
	printf("Device-only VRAM: total %llu MiB, aperture %llu MiB, UID %u\n",
		(unsigned long long)(initial.total_vram >> 20),
		(unsigned long long)(initial.visible_vram >> 20), (unsigned)geteuid());
	const uint64 bytes = 64ULL << 20;
	auto input = Create(fd, AMDGPU_CREATE_SYSTEM_BUFFER, bytes, true);
	auto output = Create(fd, AMDGPU_CREATE_SYSTEM_BUFFER, bytes, true);
	auto visible = Create(fd, AMDGPU_CREATE_BUFFER, 8192, true);
	volatile uint32* in = (volatile uint32*)(addr_t)input.address;
	volatile uint32* out = (volatile uint32*)(addr_t)output.address;
	volatile uint32* guard = (volatile uint32*)(addr_t)visible.address;
	for (uint32 i = 0; i < 2048; i++)
		guard[i] = 0x47bd3921u ^ i;
	std::vector<amdgpu_buffer> buffers;
	while (buffers.size() < 250) {
		auto bo = Request<amdgpu_buffer>();
		bo.bytes = bytes;
		if (ioctl(fd, AMDGPU_CREATE_DEVICE_BUFFER, &bo, sizeof(bo)) < 0) {
			Require(errno == B_NO_MEMORY, "exhaustion reports no memory");
			break;
		}
		Require(bo.area == -1 && bo.address == 0 && bo.bytes == bytes,
			"device buffer has no CPU mapping");
		Require(ioctl(fd, AMDGPU_MAP_BUFFER, &bo, sizeof(bo)) < 0
			&& errno == B_NOT_ALLOWED, "CPU mapping refused");
		Require(ioctl(observer, AMDGPU_FREE_BUFFER, &bo, sizeof(bo)) < 0
			&& errno == B_BAD_VALUE, "foreign handle refused");
		Wait(fd, Submit(fd, AMDGPU_DMA_COPY, bo.handle, output.handle, bytes));
		for (uint64 i = 0; i < bytes / 4; i++)
			Require(out[i] == 0, "all device allocation bytes initially zero");
		uint32 block = buffers.size();
		for (uint64 i = 0; i < bytes / 4; i++)
			in[i] = Pattern(i, block);
		Wait(fd, Submit(fd, AMDGPU_DMA_COPY, input.handle, bo.handle, bytes));
		buffers.push_back(bo);
		if (buffers.size() % 16 == 0)
			printf("Initialized and wrote %zu device buffers (%zu MiB)\n",
				buffers.size(), buffers.size() * 64);
	}
	const uint64 available = initial.total_vram - initial.visible_vram;
	Require(!buffers.empty() && buffers.size() * bytes <= available
		&& available - buffers.size() * bytes < bytes,
		"all full-sized blocks beyond aperture allocated");
	Require(Info(fd).client_bytes == buffers.size() * bytes + visible.bytes,
		"VRAM accounting includes unmapped buffers");
	auto invalid = Request<amdgpu_dma_submit>();
	invalid.operation = AMDGPU_DMA_FILL;
	invalid.destination = buffers.back().handle;
	invalid.bytes = 4096;
	invalid.destination_offset = bytes;
	Require(ioctl(fd, AMDGPU_SUBMIT_DMA, &invalid, sizeof(invalid)) < 0
		&& errno == B_BAD_VALUE, "device range overflow refused");
	invalid.destination_offset = 0;
	Require(ioctl(observer, AMDGPU_SUBMIT_DMA, &invalid, sizeof(invalid)) < 0
		&& errno == B_BAD_VALUE, "foreign device DMA refused");
	// Verify after all writes: overlapping or aliased allocations cannot pass.
	for (size_t block = 0; block < buffers.size(); block++) {
		Wait(fd, Submit(fd, AMDGPU_DMA_COPY, buffers[block].handle, output.handle, bytes));
		for (uint64 i = 0; i < bytes / 4; i++) {
			if (out[i] != Pattern(i, block)) {
				fprintf(stderr, "block %zu word %llu got %#x expected %#x\n",
					block, (unsigned long long)i, (unsigned)out[i], Pattern(i, block));
				Require(false, "complete VRAM pattern readback");
			}
		}
		if ((block + 1) % 16 == 0)
			printf("Verified %zu device buffers (%zu MiB)\n", block + 1, (block + 1) * 64);
	}
	for (uint32 i = 0; i < 2048; i++)
		Require(guard[i] == (0x47bd3921u ^ i), "visible allocation remains intact");
	printf("PASS: %zu simultaneous blocks, %llu complete bytes zeroed/written/read back\n",
		buffers.size(), (unsigned long long)(buffers.size() * bytes));
	auto retired = buffers.back();
	uint64 fence = 0;
	for (uint32 i = 0; i < 32; i++)
		fence = Submit(fd, AMDGPU_DMA_FILL, 0, retired.handle, bytes, i);
	fence = Submit(fd, AMDGPU_DMA_COPY, retired.handle, output.handle, bytes);
	Require(ioctl(fd, AMDGPU_FREE_BUFFER, &retired, sizeof(retired)) == 0,
		"free with queued references");
	Wait(fd, fence);
	for (uint64 i = 0; i < bytes / 4; i++)
		Require(out[i] == 31, "queued references retain allocation");
	Require(ioctl(fd, AMDGPU_FREE_BUFFER, &retired, sizeof(retired)) < 0
		&& errno == B_BAD_VALUE, "stale handle rejected");
	auto reused = Create(fd, AMDGPU_CREATE_DEVICE_BUFFER, bytes, false);
	Require(reused.handle != retired.handle, "reused storage has a fresh handle");
	Wait(fd, Submit(fd, AMDGPU_DMA_COPY, reused.handle, output.handle, bytes));
	for (uint64 i = 0; i < bytes / 4; i++)
		Require(out[i] == 0, "recycled allocation is cleared");
	for (uint32 i = 0; i < 32; i++)
		Submit(fd, AMDGPU_DMA_FILL, 0, reused.handle, bytes, i);
	close(fd);
	bigtime_t deadline = system_time() + 5000000;
	while (Info(observer).pending_jobs != 0 && system_time() < deadline)
		snooze(1000);
	Require(Info(observer).pending_jobs == 0 && Info(observer).allocated_bytes == 0,
		"close drains queued references and releases all VRAM");
	auto gart = Request<amdgpu_gart_info>();
	Require(ioctl(observer, AMDGPU_GART_INFO, &gart, sizeof(gart)) == 0
		&& gart.allocated_bytes == 0, "close releases staging RAM too");
	delete_area(input.area);
	delete_area(output.area);
	delete_area(visible.area);
	close(observer);
	puts("PASS: device-only buffers, isolation, denied mapping, queued free/close and zeroed reuse");
	return 0;
}
