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

static void Require(bool okay, const char* why)
{
	if (!okay) { fprintf(stderr, "FAIL: %s: %s\n", why, strerror(errno)); exit(1); }
}
template<typename T> static T Request()
{
	T r = {}; r.version = AMDGPU_HAIKU_ABI_VERSION; r.size = sizeof(r); return r;
}
static int Open()
{
	int fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
	Require(fd >= 0, "open graphics client"); return fd;
}
struct Client {
	int fd;
	amdgpu_buffer buffer;
	amdgpu_vm_test job;
	int32 done;
};
static Client Create(uint32 seed)
{
	Client c = {}; c.fd = Open();
	auto vm = Request<amdgpu_vm_info>();
	Require(ioctl(c.fd, AMDGPU_VM_INFO, &vm, sizeof(vm)) == 0, "create VM");
	c.buffer = Request<amdgpu_buffer>(); c.buffer.bytes = 16384;
	Require(ioctl(c.fd, AMDGPU_CREATE_SYSTEM_BUFFER, &c.buffer, sizeof(c.buffer)) == 0
		&& ioctl(c.fd, AMDGPU_MAP_BUFFER, &c.buffer, sizeof(c.buffer)) == 0,
		"create and map private system buffer");
	volatile uint32* words = (volatile uint32*)(addr_t)c.buffer.address;
	for (uint32 i = 0; i < c.buffer.bytes / 4; i++) Require(words[i] == 0, "reused RAM is zeroed");
	auto map = Request<amdgpu_vm_mapping>();
	map.handle = c.buffer.handle; map.address = 0x100000000; map.bytes = c.buffer.bytes;
	map.permissions = AMDGPU_VM_READ | AMDGPU_VM_WRITE;
	Require(ioctl(c.fd, AMDGPU_VM_MAP, &map, sizeof(map)) == 0, "bind owned GPU range");
	c.job = Request<amdgpu_vm_test>(); c.job.address = map.address; c.job.value = seed;
	return c;
}
static int32 Execute(void* data)
{
	Client& c = *(Client*)data;
	Require(ioctl(c.fd, AMDGPU_VM_TEST, &c.job, sizeof(c.job)) == 0, "execute pinned VM");
	Require(c.job.status == B_OK && c.job.completion != 0 && c.job.rptr == c.job.wptr
		&& c.job.vm_fault_status[0] == 0 && c.job.vm_fault_status[1] == 0,
		"GPU job retires without fault");
	// The main thread may have closed the descriptor and revoked this
	// buffer's CPU clone. Only the copied ioctl result is used here.
	atomic_set(&c.done, 1);
	return B_OK;
}
static void Pixels(const Client& c)
{
	volatile uint32* words = (volatile uint32*)(addr_t)c.buffer.address;
	for (uint32 i = 0; i < c.buffer.bytes / 4; i++)
		Require(words[i] == (i < 1024 ? c.job.value ^ (i * 0x10204081u) : 0),
			"GPU pixels and untouched neighboring bytes");
}
static void Close(Client& c)
{
	Require(close(c.fd) == 0, "close graphics client");
	Require(delete_area(c.buffer.area) == B_OK, "delete revoked CPU clone");
}
static amdgpu_irq_info IRQ(int fd)
{
	auto r = Request<amdgpu_irq_info>();
	Require(ioctl(fd, AMDGPU_IRQ_INFO, &r, sizeof(r)) == 0 && r.enabled && r.msi
		&& r.status == B_OK && !r.vm_faults && !r.privileged_faults
		&& !r.unknown && !r.overflows && r.rptr == r.wptr, "healthy IRQ state");
	return r;
}
int main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	bool absent = argc == 2 && strcmp(argv[1], "--expect-no-device") == 0;
	if (absent) {
		int fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
		Require(fd < 0 && errno == ENOENT, "no GPU in QEMU");
		puts("PASS: no-device graphics lifetime handling"); return 0;
	}
	if (argc == 2 && strcmp(argv[1], "--unprivileged") == 0)
		Require(setgid(65534) == 0 && setuid(65534) == 0, "drop privileges");
	else Require(argc == 1, "usage: amdgpu_gfx_lifetime [--unprivileged | --expect-no-device]");
	// Establish initialized graphics, then account only this test's jobs.
	Client warm = Create(0x71000000); Execute(&warm); Pixels(warm); Close(warm);
	int monitor = Open(); auto before = IRQ(monitor);
	uint32 jobs = 0, closes = 0;
	for (uint32 attempt = 0; attempt < 128 && closes < 16; attempt++) {
		Client c = Create(0x72000000 ^ attempt);
		thread_id t = spawn_thread(Execute, "graphics lifetime", B_NORMAL_PRIORITY, &c);
		Require(t >= 0 && resume_thread(t) == B_OK, "start graphics caller");
		bool busy = false;
		bigtime_t deadline = system_time() + 5000000;
		while (!atomic_get(&c.done)) {
			// Deliberately invalid even if the job just finished, so this
			// observation can never remove a real mapping.
			auto invalid = Request<amdgpu_vm_mapping>();
			Require(ioctl(c.fd, AMDGPU_VM_UNMAP, &invalid, sizeof(invalid)) == -1
				&& (errno == B_BUSY || errno == B_BAD_VALUE), "VM mutations remain frozen");
			if (errno == B_BUSY) { busy = true; break; }
			Require(system_time() < deadline, "bounded graphics observation");
			snooze(50);
		}
		if (busy) {
			// Removing the public handle must not drop the active VM's BO
			// reference. Closing the file then tests the ioctl's lifetime pin.
			Require(ioctl(c.fd, AMDGPU_FREE_BUFFER, &c.buffer, sizeof(c.buffer)) == 0,
				"free public handle while binding retains storage");
			Require(close(c.fd) == 0, "close after observing active graphics ioctl");
			closes++;
		}
		// Allocate and bind another VM immediately, potentially before the
		// old caller returns. GART invalidation and physical reuse must wait
		// for the right engine owner, without changing this new buffer.
		Client next = Create(0x73000000 ^ attempt);
		Execute(&next); Pixels(next); Close(next); jobs++;
		status_t result;
		Require(wait_for_thread(t, &result) == B_OK && result == B_OK, "join graphics caller");
		jobs++;
		if (busy) Require(delete_area(c.buffer.area) == B_OK, "release closed client's revoked clone");
		else { Pixels(c); Close(c); }
	}
	Require(closes == 16, "observe all 16 in-flight close/reuse cycles");
	auto after = IRQ(monitor);
	Require(after.completed_fences - before.completed_fences == jobs
		&& after.waits - before.waits == jobs && after.eop_events - before.eop_events == jobs * 2,
		"exact graphics completion and IRQ accounting");
	auto memory = Request<amdgpu_memory_info>(); auto ram = Request<amdgpu_gart_info>();
	Require(ioctl(monitor, AMDGPU_MEMORY_INFO, &memory, sizeof(memory)) == 0
		&& !memory.faulted && memory.allocated_bytes == 0 && memory.pending_jobs == 0,
		"all VRAM reclaimed");
	Require(ioctl(monitor, AMDGPU_GART_INFO, &ram, sizeof(ram)) == 0
		&& ram.allocated_bytes == 0 && !(ram.vm_fault_status & 0xff), "all wired RAM reclaimed");
	close(monitor);
	printf("PASS: %u graphics jobs plus warmup, 16 observed in-flight closes, frozen VMs, "
		"GART reuse, exact pixels/IRQs and complete reclamation\n", jobs);
}
