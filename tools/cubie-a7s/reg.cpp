/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	cubie_reg: reads or writes registers of the A733's display path through
	the sunxi_display driver (lab images only: the driver's settings must
	say "debug_registers true").

		cubie_reg <address> [<address>...]	read
		cubie_reg <address>=<value> [...]	write
		cubie_reg <address>:<count>			read count words
*/


#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sunxi_display.h>


static int
reg_access(int fd, uint64 address, uint32* value, bool write)
{
	sunxi_display_register request = {};
	request.address = address;
	request.value = *value;
	request.write = write ? 1 : 0;
	if (ioctl(fd, SUNXI_DISPLAY_DEBUG_REGISTER, &request, sizeof(request))
			!= 0) {
		fprintf(stderr, "%#" B_PRIx64 ": %s\n", address, strerror(errno));
		return -1;
	}
	*value = request.value;
	return 0;
}


int
main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <addr>[=<value>|:<count>]...\n", argv[0]);
		return 1;
	}
	int fd = open("/dev/" SUNXI_DISPLAY_DEVICE, O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	int result = 0;
	for (int i = 1; i < argc; i++) {
		char* end;
		uint64 address = strtoull(argv[i], &end, 16);
		if (*end == '=') {
			uint32 value = strtoul(end + 1, NULL, 16);
			if (reg_access(fd, address, &value, true) == 0)
				printf("%#010" B_PRIx64 " <- %#010" B_PRIx32 "\n", address, value);
			else
				result = 1;
			continue;
		}
		uint32 count = *end == ':' ? strtoul(end + 1, NULL, 0) : 1;
		for (uint32 j = 0; j < count; j++) {
			uint32 value = 0;
			if (reg_access(fd, address + j * 4, &value, false) != 0) {
				result = 1;
				break;
			}
			printf("%#010" B_PRIx64 " = %#010" B_PRIx32 "\n", address + j * 4,
				value);
		}
	}
	close(fd);
	return result;
}
