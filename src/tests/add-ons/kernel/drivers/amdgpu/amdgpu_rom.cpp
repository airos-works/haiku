/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include <amdgpu_haiku.h>
#include "Firmware.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

int
main(int argc, char** argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: amdgpu_rom output.rom\n");
		return 2;
	}
	int fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDONLY);
	if (fd < 0) {
		perror("open amdgpu");
		return 1;
	}
	void* data = malloc(AMDGPU_ROM_SIZE);
	if (data == NULL)
		return 1;
	amdgpu_rom request = {AMDGPU_HAIKU_ABI_VERSION, sizeof(request),
		(uint64)(addr_t)data, AMDGPU_ROM_SIZE, 0};
	int result = ioctl(fd, AMDGPU_READ_ROM, &request, sizeof(request));
	close(fd);
	if (result != 0) {
		perror("AMDGPU_READ_ROM");
		free(data);
		return 1;
	}
	FILE* output = fopen(argv[1], "wb");
	if (output == NULL) {
		perror(argv[1]);
		free(data);
		return 1;
	}
	bool written = fwrite(data, 1, AMDGPU_ROM_SIZE, output) == AMDGPU_ROM_SIZE;
	written &= fclose(output) == 0;
	amdgpu::AtomVramReservation reservation;
	bool valid = amdgpu::ParseAtomVramReservation(data, AMDGPU_ROM_SIZE, reservation);
	free(data);
	if (!valid || !written) {
		fprintf(stderr, "ROM write or ATOM reservation parse failed\n");
		return 1;
	}
	printf("ATOM VRAM reservation: %#" B_PRIx64 " + %#" B_PRIx64
		", driver scratch %#" B_PRIx32 "\n", reservation.start,
		reservation.size, reservation.driverScratchSize);
	return 0;
}
