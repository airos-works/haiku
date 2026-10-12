/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <amdgpu_haiku.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>


static bool
expect_rejection(int fd, uint32 op, void* data, size_t size, status_t expected)
{
	errno = 0;
	int result = ioctl(fd, op, data, size);
	if (result == -1 && errno == expected)
		return true;
	fprintf(stderr, "ioctl %" B_PRIu32 ": result %d errno %d, expected %" B_PRId32
		"\n", op, result, errno, expected);
	return false;
}


int
main(int argc, char** argv)
{
	bool noDevice = argc == 2 && strcmp(argv[1], "--expect-no-device") == 0;
	if (argc > 1 && !noDevice) {
		fprintf(stderr, "usage: amdgpu_probe [--expect-no-device]\n");
		return 2;
	}
	int fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDONLY);
	if (fd < 0) {
		if (noDevice && (errno == ENOENT || errno == ENODEV)) {
			puts("PASS: no supported AMD GPU exposed");
			return 0;
		}
		perror("open amdgpu");
		return 1;
	}
	if (noDevice) {
		fprintf(stderr, "unexpected AMD GPU present\n");
		close(fd);
		return 1;
	}
	amdgpu_info info = {};
	info.version = AMDGPU_HAIKU_ABI_VERSION;
	info.size = sizeof(info);
	if (ioctl(fd, AMDGPU_GET_INFO, &info, sizeof(info)) != 0) {
		perror("AMDGPU_GET_INFO");
		close(fd);
		return 1;
	}
	printf("AMD %04x:%04x revision %02x subsystem %04x:%04x at %02x:%02x.%u\n",
		info.vendor, info.device, info.revision, info.subsystem_vendor,
		info.subsystem_device, info.bus, info.slot, info.function);
	printf("PCI command %#" B_PRIx32 ", IRQ %" B_PRIu32 "\n",
		info.pci_command, info.interrupt);
	for (uint32 i = 0; i < 6; i++) {
		if (info.bar_size[i] != 0)
			printf("BAR%u: %#" B_PRIx64 " + %#" B_PRIx64 "\n", (unsigned)i,
				info.bar_address[i], info.bar_size[i]);
	}
	printf("VRAM: %" B_PRIu64 " MiB, GPU base %#" B_PRIx64 "\n",
		info.vram_size >> 20, info.vram_gpu_base);
	printf("Boot framebuffer: %#" B_PRIx64 " + %#" B_PRIx64
		", %" B_PRIu32 "x%" B_PRIu32 ", stride %" B_PRIu32 "\n",
		info.boot_framebuffer, info.boot_framebuffer_size,
		info.boot_width, info.boot_height, info.boot_stride);
	if (info.register_count > AMDGPU_INFO_REGISTER_COUNT) {
		fprintf(stderr, "invalid register count\n");
		close(fd);
		return 1;
	}
	for (uint32 i = 0; i < info.register_count; i++) {
		printf("reg[%04" B_PRIx32 "] = %08" B_PRIx32 "\n",
			info.registers[i].index, info.registers[i].value);
	}

	bool pass = info.version == AMDGPU_HAIKU_ABI_VERSION
		&& info.size == sizeof(info) && info.vendor == 0x1002
		&& info.device == 0x67c7 && info.vram_size > 0
		&& info.vram_size != (uint64)0xffffffff << 20;
	pass &= expect_rejection(fd, AMDGPU_GET_INFO, &info, sizeof(info) - 1,
		B_BAD_VALUE);
	info.version++;
	pass &= expect_rejection(fd, AMDGPU_GET_INFO, &info, sizeof(info), B_BAD_VALUE);
	info.version--;
	info.size--;
	pass &= expect_rejection(fd, AMDGPU_GET_INFO, &info, sizeof(info), B_BAD_VALUE);
	info.size++;
	pass &= expect_rejection(fd, AMDGPU_GET_INFO, NULL, sizeof(info), B_BAD_ADDRESS);
	pass &= expect_rejection(fd, AMDGPU_GET_INFO + 100, &info, sizeof(info),
		ENOTTY);

	// Closing one client must not invalidate another client's MMIO mapping.
	int second = open("/dev/" AMDGPU_DEVICE_NAME, O_RDONLY);
	close(fd);
	if (second < 0) {
		perror("second open");
		return 1;
	}
	pass &= ioctl(second, AMDGPU_GET_INFO, &info, sizeof(info)) == 0;
	close(second);
	for (unsigned i = 0; i < 32; i++) {
		fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDONLY);
		if (fd < 0) {
			perror("reopen");
			return 1;
		}
		pass &= ioctl(fd, AMDGPU_GET_INFO, &info, sizeof(info)) == 0;
		close(fd);
	}
	puts(pass ? "PASS: identity, query validation, shared open and reopen"
		: "FAIL: AMD GPU probe");
	return pass ? 0 : 1;
}
