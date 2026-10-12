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

static void Require(bool okay, const char* message)
{
	if (!okay) {
		fprintf(stderr, "FAIL: %s (%d: %s)\n", message, errno, strerror(errno));
		exit(1);
	}
}
template<typename T> static T Request()
{
	T request = {};
	request.version = AMDGPU_HAIKU_ABI_VERSION;
	request.size = sizeof(T);
	return request;
}
static int Open()
{
	int fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
	Require(fd >= 0, "open client");
	return fd;
}
static amdgpu_vm_info VM(int fd)
{
	auto request = Request<amdgpu_vm_info>();
	Require(ioctl(fd, AMDGPU_VM_INFO, &request, sizeof(request)) == 0, "create/query GPU VM");
	Require(request.address_start == 131072 && request.address_end == (1ULL << 36)
		&& request.page_size == 4096, "GPU VA bounds");
	return request;
}
static uint64 Allocated(int fd)
{
	auto info = Request<amdgpu_memory_info>();
	Require(ioctl(fd, AMDGPU_MEMORY_INFO, &info, sizeof(info)) == 0
		&& info.faulted == 0, "healthy memory query");
	return info.allocated_bytes;
}
static uint64 SystemAllocated(int fd)
{
	auto info = Request<amdgpu_gart_info>();
	Require(ioctl(fd, AMDGPU_GART_INFO, &info, sizeof(info)) == 0
		&& info.vm_fault_status == 0, "healthy GART query");
	return info.allocated_bytes;
}
static amdgpu_buffer Create(int fd, uint32 kind)
{
	auto buffer = Request<amdgpu_buffer>(); buffer.bytes = 16384;
	Require(ioctl(fd, kind, &buffer, sizeof(buffer)) == 0, "allocate owned buffer");
	if (kind != AMDGPU_CREATE_DEVICE_BUFFER)
		Require(ioctl(fd, AMDGPU_MAP_BUFFER, &buffer, sizeof(buffer)) == 0, "CPU map buffer");
	return buffer;
}
static void Free(int fd, amdgpu_buffer buffer)
{
	Require(ioctl(fd, AMDGPU_FREE_BUFFER, &buffer, sizeof(buffer)) == 0, "free buffer handle");
}
static amdgpu_vm_mapping Mapping(uint64 handle, uint64 address, uint64 bytes,
	uint64 offset = 0, uint32 permissions = AMDGPU_VM_READ | AMDGPU_VM_WRITE)
{
	auto request = Request<amdgpu_vm_mapping>();
	request.handle = handle; request.address = address; request.bytes = bytes;
	request.buffer_offset = offset; request.permissions = permissions;
	return request;
}
static void Map(int fd, amdgpu_vm_mapping request)
{
	Require(ioctl(fd, AMDGPU_VM_MAP, &request, sizeof(request)) == 0, "map owned GPU VA");
}
static void Unmap(int fd, uint64 address, uint64 bytes)
{
	auto request = Mapping(0, address, bytes, 0, 0);
	Require(ioctl(fd, AMDGPU_VM_UNMAP, &request, sizeof(request)) == 0, "unmap exact GPU range");
}
static void Execute(int fd, uint64 address, uint32 value)
{
	auto request = Request<amdgpu_vm_test>(); request.address = address; request.value = value;
	Require(ioctl(fd, AMDGPU_VM_TEST, &request, sizeof(request)) == 0, "execute client GPU VM");
	if (request.status != B_OK) {
		fprintf(stderr, "VM status %#x faults %#x/%#x ring %u/%u completion %llu\n",
			(unsigned)request.status, (unsigned)request.vm_fault_status[0],
			(unsigned)request.vm_fault_status[1], (unsigned)request.rptr,
			(unsigned)request.wptr, (unsigned long long)request.completion);
		errno = request.status;
	}
	Require(request.status == B_OK && request.completion != 0
		&& request.vm_fault_status[0] == 0 && request.vm_fault_status[1] == 0
		&& request.rptr == request.wptr, "GPU fence, faults and retirement");
}
static void RejectExecute(int fd, uint64 address)
{
	auto request = Request<amdgpu_vm_test>(); request.address = address;
	Require(ioctl(fd, AMDGPU_VM_TEST, &request, sizeof(request)) == -1
		&& errno == B_NOT_ALLOWED, "unmapped/read-only GPU destination rejected");
}
static void Check(const amdgpu_buffer& buffer, const std::vector<uint32>& expected)
{
	volatile uint32* words = (volatile uint32*)(addr_t)buffer.address;
	for (uint32 i = 0; i < expected.size(); i++) {
		if (words[i] != expected[i]) {
			fprintf(stderr, "word %u got %#x expected %#x\n", (unsigned)i,
				(unsigned)words[i], (unsigned)expected[i]);
			Require(false, "complete buffer and guard readback");
		}
	}
}
static void Expected(std::vector<uint32>& expected, uint32 offset, uint32 seed)
{
	for (uint32 i = 0; i < 1024; i++) expected[offset / 4 + i] = seed ^ (i * 0x10204081u);
}
static void Copy(int fd, uint64 source, uint64 destination)
{
	auto request = Request<amdgpu_dma_submit>(); request.operation = AMDGPU_DMA_COPY;
	request.source = source; request.destination = destination; request.bytes = 16384;
	Require(ioctl(fd, AMDGPU_SUBMIT_DMA, &request, sizeof(request)) == 0, "queue device readback");
	auto wait = Request<amdgpu_fence_wait>(); wait.fence = request.fence; wait.timeout_us = 5000000;
	Require(ioctl(fd, AMDGPU_WAIT_FENCE, &wait, sizeof(wait)) == 0 && wait.status == B_OK,
		"device readback fence");
}

static int32
Concurrent(void* data)
{
	uint32 index = (addr_t)data;
	int fd = Open(); VM(fd);
	auto buffer = Create(fd, (index & 1) ? AMDGPU_CREATE_SYSTEM_BUFFER : AMDGPU_CREATE_BUFFER);
	Map(fd, Mapping(buffer.handle, 0x100000000, 16384));
	std::vector<uint32> expected(4096);
	for (uint32 round = 0; round < 16; round++) {
		uint32 offset = (round & 1) ? 4092 : 8192;
		uint32 seed = 0x51000000 ^ index * 0x12345 ^ round;
		Execute(fd, 0x100000000 + offset, seed);
		Expected(expected, offset, seed); Check(buffer, expected);
	}
	close(fd);
	Require(delete_area(buffer.area) == B_OK, "release concurrent revoked CPU area");
	return 0;
}

int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc == 2 && strcmp(argv[1], "--unprivileged") == 0)
		Require(setgid(65534) == 0 && setuid(65534) == 0, "drop privileges");
	else Require(argc == 1, "usage: amdgpu_vm [--unprivileged]");
	printf("Client VM test UID %u\n", (unsigned)geteuid());
	int monitor = Open(), a = Open(), b = Open(), c = Open();
	auto info = Request<amdgpu_vm_info>();
	Require(ioctl(a, AMDGPU_VM_INFO, NULL, sizeof(info)) == -1 && errno == B_BAD_ADDRESS,
		"null VM request");
	Require(ioctl(a, AMDGPU_VM_INFO, &info, sizeof(info) - 1) == -1 && errno == B_BAD_VALUE,
		"short VM request");
	info.reserved[1] = 1;
	Require(ioctl(a, AMDGPU_VM_INFO, &info, sizeof(info)) == -1 && errno == B_BAD_VALUE,
		"reserved VM request");
	info = Request<amdgpu_vm_info>(); info.version++;
	Require(ioctl(a, AMDGPU_VM_INFO, &info, sizeof(info)) == -1 && errno == B_BAD_VALUE,
		"VM ABI version");
	int readOnly = open("/dev/" AMDGPU_DEVICE_NAME, O_RDONLY);
	info = Request<amdgpu_vm_info>();
	Require(readOnly >= 0 && ioctl(readOnly, AMDGPU_VM_INFO, &info, sizeof(info)) == -1
		&& errno == B_NOT_ALLOWED, "read-only client rejected"); close(readOnly);
	uint64 initial = Allocated(monitor), initialSystem = SystemAllocated(monitor);
	Require(VM(a).mapping_count == 0 && VM(b).mapping_count == 0 && VM(c).mapping_count == 0,
		"empty distinct client VMs");
	auto va = Create(a, AMDGPU_CREATE_BUFFER);
	auto vb = Create(b, AMDGPU_CREATE_SYSTEM_BUFFER);
	auto vc = Create(c, AMDGPU_CREATE_DEVICE_BUFFER);
	auto staging = Create(c, AMDGPU_CREATE_SYSTEM_BUFFER);
	const uint64 address = 0xfffff000ULL;
	Map(a, Mapping(va.handle, address, 16384));
	Map(b, Mapping(vb.handle, address, 16384));
	Map(c, Mapping(vc.handle, address, 16384, 0, AMDGPU_VM_WRITE));
	Require(VM(a).mapped_bytes == 16384 && VM(a).mapping_count == 1, "mapping accounting");
	auto bad = Mapping(vb.handle, address + 32768, 4096);
	Require(ioctl(a, AMDGPU_VM_MAP, &bad, sizeof(bad)) == -1 && errno == B_BAD_VALUE,
		"foreign BO handle rejected");
	for (unsigned test = 0; test < 9; test++) {
		bad = Mapping(va.handle, address + 32768, 4096);
		switch (test) {
			case 0: bad.address = 0x1000; break;
			case 1: bad.address++; break;
			case 2: bad.address = (1ULL << 36) - 4096; bad.bytes = 8192; break;
			case 3: bad.buffer_offset = 16384; break;
			case 4: bad.address = address; break;
			case 5: bad.permissions = AMDGPU_VM_EXECUTE; break;
			case 6: bad.bytes = UINT64_MAX; break;
			case 7: bad.reserved = 1; break;
			case 8: bad.address = AMDGPU_COPY_IB_ADDRESS; break;
		}
		Require(ioctl(a, AMDGPU_VM_MAP, &bad, sizeof(bad)) == -1 && errno == B_BAD_VALUE,
			"invalid VM mapping rejected atomically");
	}
	Map(a, Mapping(va.handle, (1ULL << 36) - 4096, 4096, 4096, AMDGPU_VM_READ));
	RejectExecute(a, (1ULL << 36) - 4096); RejectExecute(a, 0x1000); RejectExecute(a, 0x345000);
	Unmap(a, (1ULL << 36) - 4096, 4096);
	bad = Mapping(0, address, 4096, 0, 0);
	Require(ioctl(a, AMDGPU_VM_UNMAP, &bad, sizeof(bad)) == -1 && errno == B_BAD_VALUE,
		"partial unmap rejected without changing binding");
	pid_t child = fork(); Require(child >= 0, "fork foreign team");
	if (child == 0) {
		auto borrowed = Request<amdgpu_vm_info>();
		_exit(ioctl(a, AMDGPU_VM_INFO, &borrowed, sizeof(borrowed)) == -1
			&& errno == B_NOT_ALLOWED ? 0 : 1);
	}
	int childStatus;
	Require(waitpid(child, &childStatus, 0) == child && WIFEXITED(childStatus)
		&& WEXITSTATUS(childStatus) == 0, "borrowed fd cannot cross team ownership");
	puts("PASS: ABI, ownership, bounds, overlap, permissions and exact unmap validation");
	std::vector<uint32> expectedA(4096), expectedB(4096), expectedC(4096);
	for (uint32 round = 0; round < 32; round++) {
		uint32 seed = 0x71324589 ^ round;
		uint32 offset = (round & 1) ? 8192 : 4092;
		Execute(a, address + offset, seed); Expected(expectedA, offset, seed);
		Check(va, expectedA); Check(vb, expectedB);
		Execute(b, address + offset, ~seed); Expected(expectedB, offset, ~seed);
		Check(va, expectedA); Check(vb, expectedB);
	}
	Execute(c, address + 4092, 0xabcdef01); Expected(expectedC, 4092, 0xabcdef01);
	Copy(c, vc.handle, staging.handle); Check(staging, expectedC);
	puts("PASS: 65 GPU writes through separate VMs at identical VAs, cross-page/4-GiB addressing, RAM and device VRAM");
	// Alias the same BO at another VA and retain it after freeing its handle.
	const uint64 retainedVA = 0x23450000;
	auto retained = Create(a, AMDGPU_CREATE_BUFFER);
	Map(a, Mapping(retained.handle, retainedVA, 16384));
	Map(a, Mapping(retained.handle, retainedVA + 32768, 16384));
	std::vector<uint32> retainedExpected(4096);
	Execute(a, retainedVA + 32768 + 4096, 0x1234);
	Expected(retainedExpected, 4096, 0x1234); Check(retained, retainedExpected);
	Free(a, retained); Require(delete_area(retained.area) == B_OK, "delete revoked CPU mapping");
	auto fresh = Create(a, AMDGPU_CREATE_BUFFER);
	std::vector<uint32> zero(4096);
	Execute(a, retainedVA + 4096, 0x7654); Check(fresh, zero);
	Unmap(a, retainedVA, 16384); Unmap(a, retainedVA + 32768, 16384);
	RejectExecute(a, retainedVA + 4096);
	puts("PASS: aliased BO mappings, freed-handle retention, unmap and zeroed independent reuse");
	close(a); close(b); close(c);
	for (area_id area : {va.area, vb.area, staging.area, fresh.area})
		Require(delete_area(area) == B_OK, "delete revoked client area");
	Require(Allocated(monitor) == initial && SystemAllocated(monitor) == initialSystem,
		"all VM/BO allocations reclaimed on close");
	std::vector<int> clients;
	for (unsigned i = 0; i < 32; i++) { int fd = Open(); VM(fd); clients.push_back(fd); }
	int extra = Open(); info = Request<amdgpu_vm_info>();
	Require(ioctl(extra, AMDGPU_VM_INFO, &info, sizeof(info)) == -1 && errno == B_NO_MEMORY,
		"bounded VM population");
	close(clients.back()); clients.pop_back(); VM(extra); close(extra);
	for (int fd : clients) close(fd);
	Require(Allocated(monitor) == initial, "VM quota and directory reuse");
	thread_id threads[4];
	for (unsigned i = 0; i < 4; i++) {
		threads[i] = spawn_thread(Concurrent, "amdgpu VM client", B_NORMAL_PRIORITY, (void*)(addr_t)i);
		Require(threads[i] >= 0 && resume_thread(threads[i]) == B_OK, "start concurrent VM client");
	}
	for (thread_id thread : threads) {
		status_t status;
		Require(wait_for_thread(thread, &status) == B_OK && status == B_OK, "join concurrent VM client");
	}
	Require(Allocated(monitor) == initial && SystemAllocated(monitor) == initialSystem,
		"concurrent VM allocations reclaimed");
	close(monitor);
	puts("PASS: 32-context quota/reuse, four concurrent clients, GPU data/guards and complete reclamation");
}
