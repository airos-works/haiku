/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include <amdgpu_haiku.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void Require(bool okay, const char* what)
{
	if (!okay) {
		fprintf(stderr, "FAIL: %s (%s)\n", what, strerror(errno));
		exit(1);
	}
}

template<typename T> static T Request()
{
	T value = {};
	value.version = AMDGPU_HAIKU_ABI_VERSION;
	value.size = sizeof(T);
	return value;
}

static uint32 Register(const amdgpu_info& info, uint32 index)
{
	for (unsigned i = 0; i < info.register_count; i++) {
		if (info.registers[i].index == index)
			return info.registers[i].value;
	}
	Require(false, "snapshot register present");
	return 0;
}

int main(int argc, char** argv)
{
	if (argc != 2 || (strcmp(argv[1], "--expect-missing") != 0
		&& strcmp(argv[1], "--expect-bad-data") != 0)) {
		fprintf(stderr, "usage: amdgpu_startup --expect-missing | --expect-bad-data\n");
		return 2;
	}
	Require(setuid(65534) == 0 && geteuid() == 65534, "drop root privileges");
	int ro = open("/dev/" AMDGPU_DEVICE_NAME, O_RDONLY);
	int fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
	Require(ro >= 0 && fd >= 0, "open test clients");
	auto before = Request<amdgpu_info>();
	Require(ioctl(fd, AMDGPU_GET_INFO, &before, sizeof(before)) == 0, "initial snapshot");
	Require(Register(before, 0x3480) == 0, "requires a cold uninitialized DMA engine");
	auto info = Request<amdgpu_memory_info>();
	Require(ioctl(ro, AMDGPU_MEMORY_INFO, &info, sizeof(info)) < 0 && errno == B_NOT_ALLOWED,
		"read-only clients cannot start engines");
	Require(ioctl(fd, AMDGPU_MEMORY_INFO, NULL, sizeof(info)) < 0 && errno == B_BAD_ADDRESS,
		"invalid pointer rejected before startup");
	info.version++;
	Require(ioctl(fd, AMDGPU_MEMORY_INFO, &info, sizeof(info)) < 0 && errno == B_BAD_VALUE,
		"invalid ABI rejected before startup");
	auto bo = Request<amdgpu_buffer>();
	Require(ioctl(fd, AMDGPU_CREATE_BUFFER, &bo, sizeof(bo)) < 0 && errno == B_BAD_VALUE,
		"empty allocation rejected before startup");
	bo.bytes = 4096;
	bo.reserved = 1;
	Require(ioctl(fd, AMDGPU_CREATE_SYSTEM_BUFFER, &bo, sizeof(bo)) < 0 && errno == B_BAD_VALUE,
		"reserved allocation field rejected before startup");
	info = Request<amdgpu_memory_info>();
	status_t expected = strcmp(argv[1], "--expect-missing") == 0 ? B_ENTRY_NOT_FOUND : B_BAD_DATA;
	for (unsigned i = 0; i < 2; i++)
		Require(ioctl(fd, AMDGPU_MEMORY_INFO, &info, sizeof(info)) < 0 && errno == expected,
			"installed firmware failure reported consistently");
	auto after = Request<amdgpu_info>();
	Require(ioctl(fd, AMDGPU_GET_INFO, &after, sizeof(after)) == 0, "final snapshot");
	const uint32 indexes[] = {0x3480, 0x3412, 0x3612, 0x0504, 0x080d, 0x080e};
	for (uint32 index : indexes)
		Require(Register(before, index) == Register(after, index), "engine/VM state unchanged");
	close(fd);
	close(ro);
	puts("PASS: malformed/read-only requests and unavailable firmware leave engines uninitialized");
	return 0;
}
