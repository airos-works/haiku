/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	Checks the rk3588_npu driver without the Neural Kit: that the core
	powers up and identifies itself, that buffers are made, shown to the
	caller and given distinct device addresses, and that a job whose
	command list is not in one of the caller's buffers is refused.

		rk3588_npu_test [--stress <rounds>]

	Exits with 0 when everything checked out.
*/


#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <OS.h>

#include "rk3588_npu.h"


static int sFailures;


static void
check(bool ok, const char* what)
{
	printf("%s: %s\n", ok ? "ok" : "FAILED", what);
	if (!ok)
		sFailures++;
}


int
main(int argc, char** argv)
{
	int rounds = 1;
	if (argc == 3 && strcmp(argv[1], "--stress") == 0)
		rounds = atoi(argv[2]);

	for (int round = 0; round < rounds; round++) {
		int fd = open(RK3588_NPU_DEVICE_PATH, O_RDWR);
		if (fd < 0) {
			printf("FAILED: cannot open %s: %s\n", RK3588_NPU_DEVICE_PATH,
				strerror(errno));
			return 1;
		}

		rk3588_npu_info info;
		memset(&info, 0, sizeof(info));
		check(ioctl(fd, RK3588_NPU_GET_INFO, &info, sizeof(info)) == 0,
			"get info");
		printf("interface %" B_PRIu32 ", hardware version %#" B_PRIx32
			", %" B_PRIu32 " core(s), clock %" B_PRIu32 " Hz\n",
			info.interface_version, info.hardware_version, info.core_count,
			info.clock_rate);
		check(info.hardware_version != 0
			&& info.hardware_version != 0xffffffff,
			"the core identifies itself");

		rk3588_npu_create_buffer buffers[4];
		static const uint64 kSizes[4] = { 4096, 100000, 4 * 1024 * 1024,
			12345 };
		for (int i = 0; i < 4; i++) {
			memset(&buffers[i], 0, sizeof(buffers[i]));
			buffers[i].size = kSizes[i];
			bool ok = ioctl(fd, RK3588_NPU_CREATE_BUFFER, &buffers[i],
				sizeof(buffers[i])) == 0;
			check(ok, "create buffer");
			if (!ok)
				return 1;
			printf("  buffer %" B_PRIu32 ": %" B_PRIu64 " bytes at %p, "
				"device address %#" B_PRIx64 ", area %" B_PRId32 "\n",
				buffers[i].handle, buffers[i].size, buffers[i].address,
				buffers[i].device_address, buffers[i].area);

			area_info areaInfo;
			check(get_area_info(buffers[i].area, &areaInfo) == B_OK
				&& areaInfo.address == buffers[i].address
				&& areaInfo.size == buffers[i].size, "the area is ours");

			// new buffers are zero, and can be written
			uint8* data = (uint8*)buffers[i].address;
			bool zero = true;
			for (uint64 j = 0; j < buffers[i].size; j++)
				zero &= data[j] == 0;
			check(zero, "the buffer starts out zeroed");
			memset(data, 0xa5 + i, buffers[i].size);

			rk3588_npu_sync_buffer sync;
			memset(&sync, 0, sizeof(sync));
			sync.handle = buffers[i].handle;
			sync.direction = RK3588_NPU_SYNC_FOR_DEVICE;
			check(ioctl(fd, RK3588_NPU_SYNC_BUFFER, &sync, sizeof(sync)) == 0,
				"sync for the device");
			sync.direction = RK3588_NPU_SYNC_FOR_CPU;
			check(ioctl(fd, RK3588_NPU_SYNC_BUFFER, &sync, sizeof(sync)) == 0
				&& data[0] == 0xa5 + i
				&& data[buffers[i].size - 1] == 0xa5 + i,
				"sync for the CPU keeps the contents");
		}

		bool distinct = true;
		for (int i = 0; i < 4; i++) {
			for (int j = 0; j < 4; j++) {
				if (i != j && buffers[i].device_address
						< buffers[j].device_address + buffers[j].size
					&& buffers[j].device_address
						< buffers[i].device_address + buffers[i].size) {
					distinct = false;
				}
			}
		}
		check(distinct, "device addresses do not overlap");

		// a command list outside every buffer is refused, not run
		rk3588_npu_task task;
		task.regcmd = 0x00100000;
		task.regcmd_count = 16;
		rk3588_npu_submit submit;
		memset(&submit, 0, sizeof(submit));
		submit.tasks = &task;
		submit.task_count = 1;
		check(ioctl(fd, RK3588_NPU_SUBMIT, &submit, sizeof(submit)) != 0
			&& errno == B_BAD_VALUE, "a stray command list is refused");

		// one that runs off the end of a buffer, too
		task.regcmd = buffers[0].device_address;
		task.regcmd_count = 1024;
		check(ioctl(fd, RK3588_NPU_SUBMIT, &submit, sizeof(submit)) != 0
			&& errno == B_BAD_VALUE, "an overlong command list is refused");

		rk3588_npu_delete_buffer deleteBuffer;
		deleteBuffer.handle = buffers[1].handle;
		check(ioctl(fd, RK3588_NPU_DELETE_BUFFER, &deleteBuffer,
				sizeof(deleteBuffer)) == 0, "delete buffer");
		check(ioctl(fd, RK3588_NPU_DELETE_BUFFER, &deleteBuffer,
				sizeof(deleteBuffer)) != 0, "delete it again fails");

		area_info areaInfo;
		check(get_area_info(buffers[1].area, &areaInfo) != B_OK,
			"the deleted buffer's area is gone");

		// the freed device address range is used again
		rk3588_npu_create_buffer again;
		memset(&again, 0, sizeof(again));
		again.size = 8192;
		check(ioctl(fd, RK3588_NPU_CREATE_BUFFER, &again, sizeof(again)) == 0
			&& again.device_address == buffers[1].device_address,
			"its device address is reused");

		// the rest is cleaned up by closing
		close(fd);
	}

	printf("%s\n", sFailures == 0 ? "all checks passed" : "CHECKS FAILED");
	return sFailures == 0 ? 0 : 1;
}
