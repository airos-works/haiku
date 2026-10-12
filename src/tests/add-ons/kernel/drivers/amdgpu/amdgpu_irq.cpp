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

static void Require(bool okay, const char* message)
{
	if (okay) return;
	fprintf(stderr, "FAIL: %s (%d: %s)\n", message, errno, strerror(errno));
	exit(1);
}
static amdgpu_irq_info Request()
{
	amdgpu_irq_info r = {};
	r.version = AMDGPU_HAIKU_ABI_VERSION; r.size = sizeof(r);
	return r;
}
int main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	bool absent = argc == 2 && strcmp(argv[1], "--expect-no-device") == 0;
	Require(absent || (argc == 3 && strcmp(argv[1], "--expect-events") == 0),
		"usage: amdgpu_irq --expect-events COUNT | --expect-no-device");
	// The diagnostic must remain available to an ordinary owning client.
	if (!absent && geteuid() == 0)
		Require(setgid(65534) == 0 && setuid(65534) == 0, "drop privilege");
	int fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
	if (absent) {
		Require(fd < 0 && errno == ENOENT, "no AMD device in QEMU");
		puts("PASS: no-device interrupt handling"); return 0;
	}
	Require(fd >= 0, "open interrupt diagnostic");
	char* end;
	uint64 expected = strtoull(argv[2], &end, 10);
	Require(*argv[2] != 0 && *end == 0 && expected > 0, "positive expected event count");
	amdgpu_irq_info r = Request();
	Require(ioctl(fd, AMDGPU_IRQ_INFO, NULL, sizeof(r)) == -1 && errno == B_BAD_ADDRESS,
		"null request rejected");
	Require(ioctl(fd, AMDGPU_IRQ_INFO, &r, sizeof(r) - 1) == -1 && errno == B_BAD_VALUE,
		"short request rejected");
	r.version++;
	Require(ioctl(fd, AMDGPU_IRQ_INFO, &r, sizeof(r)) == -1 && errno == B_BAD_VALUE,
		"unknown version rejected");
	r = Request(); r.size--;
	Require(ioctl(fd, AMDGPU_IRQ_INFO, &r, sizeof(r)) == -1 && errno == B_BAD_VALUE,
		"bad size rejected");
	r = Request(); r.reserved = 1;
	Require(ioctl(fd, AMDGPU_IRQ_INFO, &r, sizeof(r)) == -1 && errno == B_BAD_VALUE,
		"reserved input rejected");
	r = Request();
	Require(ioctl(fd, AMDGPU_IRQ_INFO, &r, sizeof(r)) == 0, "read interrupt counters");
	printf("IH enabled %u MSI %u vector %u bytes %u status %#x interrupts %llu vectors %llu"
		" EOP %llu completed %llu waits %llu VM faults %llu privileged %llu unknown %llu overflow %llu"
		" ring %#x/%#x last %#x/%#x/%#x/%#x\n",
		(unsigned)r.enabled, (unsigned)r.msi, (unsigned)r.vector, (unsigned)r.ring_bytes,
		(unsigned)r.status, (unsigned long long)r.interrupts, (unsigned long long)r.vectors,
		(unsigned long long)r.eop_events, (unsigned long long)r.completed_fences, (unsigned long long)r.waits,
		(unsigned long long)r.vm_faults, (unsigned long long)r.privileged_faults,
		(unsigned long long)r.unknown, (unsigned long long)r.overflows,
		(unsigned)r.rptr, (unsigned)r.wptr, (unsigned)r.last[0], (unsigned)r.last[1],
		(unsigned)r.last[2], (unsigned)r.last[3]);
	Require(r.enabled == 1 && r.msi == 1 && r.ring_bytes == 65536 && r.status == B_OK,
		"MSI ring enabled and healthy");
	for (uint32 i = 0; i < 2; i++)
		printf("first EOP %u: %#x/%#x/%#x/%#x\n", (unsigned)i,
			(unsigned)r.first_eop[i][0], (unsigned)r.first_eop[i][1],
			(unsigned)r.first_eop[i][2], (unsigned)r.first_eop[i][3]);
	Require(r.completed_fences == expected && r.waits == expected && r.eop_events >= expected
		&& r.vectors == r.eop_events && r.interrupts > 0 && r.interrupts <= r.vectors,
		"every job fence was observed by its completion interrupt");
	Require(r.vm_faults == 0 && r.privileged_faults == 0 && r.unknown == 0 && r.overflows == 0,
		"no fault or unexpected interrupt vectors");
	Require(r.rptr == r.wptr && r.rptr == ((r.vectors * 16) & 65535)
		&& (r.last[0] & 255) == 181 && (r.last[2] & 65535) == 0,
		"all completion vectors consumed from the trusted ring");
	close(fd);
	puts("PASS: ordinary-client IRQ ABI, exact completed fences, empty healthy MSI ring");
	return 0;
}
