/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	Checks the rk3588_npu driver without the Neural Kit: that the core
	powers up and identifies itself, that buffers are made, shown to the
	caller and given distinct device addresses, and that a job whose
	command list is not in one of the caller's buffers is refused.

		rk3588_npu_test [--stress <rounds>]
		rk3588_npu_test --jobs [<rounds>]

	With --jobs it runs jobs, good ones and bad ones: a small convolution
	whose command list is made by the code of the rk3588_npu Neural Kit
	back end and whose result is known, and the same with addresses the
	client has no memory at, with a command list of nothing, from two
	clients at once, from a client that dies or closes the device while
	its job runs. A bad job has to end with an error and must not keep
	the next job, of this or another client, from running.

	Exits with 0 when everything checked out.
*/


#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <signal.h>
#include <sys/wait.h>

#include <OS.h>

#include "Compiler.h"
#include "rk3588_npu.h"


static int sFailures;


static void
check(bool ok, const char* what)
{
	printf("%s: %s\n", ok ? "ok" : "FAILED", what);
	if (!ok)
		sFailures++;
}


// #pragma mark - jobs


static const uint32 kSize = 8;
static const uint32 kChannels = 16;
static const uint32 kPixels = kSize * kSize;

// where things are in the two buffers of a client
static const uint32 kCommandsOffset = 0;
static const uint32 kWeightsOffset = 4096;
static const uint32 kBiasOffset = 8192;
static const uint32 kConstantsSize = 12288;
static const uint32 kInputOffset = 0;
static const uint32 kOutputOffset = 4096;
static const uint32 kTensorsSize = 12288;

// a device address no client of this test has memory at
static const uint32 kNowhere = 0xe0000000;


/*!	A client of the driver with a convolution to run: 8 by 8 pixels of 16
	channels, a kernel of one pixel, 16 output channels, all numbers
	signed with zero points of 0, the sums divided by 64. */
struct Client {
	int							fd;
	rk3588_npu_create_buffer	constants;
	rk3588_npu_create_buffer	tensors;
	Operation					operation;
	int8						weights[kChannels * kChannels];
	int32						bias[kChannels];
	rk3588_npu_task				task;
	uint32						seed;
};


static uint32
next_random(uint32& seed)
{
	seed = seed * 1664525 + 1013904223;
	return seed >> 8;
}


static bool
sync_buffer(Client& client, rk3588_npu_create_buffer& buffer,
	uint32 direction)
{
	rk3588_npu_sync_buffer sync;
	memset(&sync, 0, sizeof(sync));
	sync.handle = buffer.handle;
	sync.direction = direction;
	return ioctl(client.fd, RK3588_NPU_SYNC_BUFFER, &sync, sizeof(sync)) == 0;
}


/*!	Writes the client's command list, with the tensors and weights at the
	given device addresses. */
static bool
make_commands(Client& client, uint32 input, uint32 output, uint32 weights)
{
	OperationAddresses addresses;
	memset(&addresses, 0, sizeof(addresses));
	addresses.input = input;
	addresses.output = output;
	addresses.weights = weights;
	addresses.bias = client.constants.device_address + kBiasOffset;

	uint64* commands = (uint64*)((uint8*)client.constants.address
		+ kCommandsOffset);
	memset(commands, 0, kMaxTaskCommands * sizeof(uint64));
	uint32 count = fill_commands(client.operation, 0, addresses, commands);

	client.task.regcmd = client.constants.device_address + kCommandsOffset;
	client.task.regcmd_count = count;

	return count < kMaxTaskCommands
		&& sync_buffer(client, client.constants, RK3588_NPU_SYNC_FOR_DEVICE);
}


static bool
make_good_commands(Client& client)
{
	return make_commands(client,
		client.tensors.device_address + kInputOffset,
		client.tensors.device_address + kOutputOffset,
		client.constants.device_address + kWeightsOffset);
}


static bool
init_client(Client& client, uint32 seed)
{
	memset(&client, 0, sizeof(client));
	client.seed = seed;

	client.fd = open(RK3588_NPU_DEVICE_PATH, O_RDWR);
	if (client.fd < 0)
		return false;

	client.constants.size = kConstantsSize;
	client.tensors.size = kTensorsSize;
	if (ioctl(client.fd, RK3588_NPU_CREATE_BUFFER, &client.constants,
			sizeof(client.constants)) != 0
		|| ioctl(client.fd, RK3588_NPU_CREATE_BUFFER, &client.tensors,
			sizeof(client.tensors)) != 0) {
		return false;
	}

	Operation& operation = client.operation;
	operation.add_tensor = -1;
	operation.addend = -1;
	operation.padding_same = true;
	operation.stride = 1;
	operation.input_width = operation.output_width = kSize;
	operation.input_height = operation.output_height = kSize;
	operation.input_channels = operation.output_channels = kChannels;
	operation.weights_width = operation.weights_height = 1;
	operation.weights_signed = true;
	operation.input_scale = 1.0f;
	operation.weights_scale = 1.0f / 64;
	operation.output_scale = 1.0f;
	operation.sum_multiplier = 1;

	if (!output_conversion(1.0 / 64, &operation.output_multiplier,
			&operation.output_shift)
		|| split_tasks(operation) != B_OK || operation.task_count != 1
		|| weights_buffer_size(operation) > kBiasOffset - kWeightsOffset) {
		return false;
	}

	for (uint32 i = 0; i < kChannels * kChannels; i++)
		client.weights[i] = (int8)(next_random(client.seed) % 17) - 8;
	for (uint32 i = 0; i < kChannels; i++)
		client.bias[i] = (int32)(next_random(client.seed) % 2001) - 1000;

	uint8* constants = (uint8*)client.constants.address;
	fill_weights(operation, (const uint8*)client.weights,
		constants + kWeightsOffset);
	fill_biases(operation, (const uint8*)client.weights, client.bias,
		(int32*)(constants + kBiasOffset));

	return make_good_commands(client);
}


static void
uninit_client(Client& client)
{
	if (client.fd >= 0)
		close(client.fd);
	free(client.operation.tasks);
	client.fd = -1;
}


static status_t
submit_job(Client& client, bigtime_t* _duration = NULL)
{
	rk3588_npu_submit submit;
	memset(&submit, 0, sizeof(submit));
	submit.tasks = &client.task;
	submit.task_count = 1;

	bigtime_t start = system_time();
	status_t status = B_OK;
	if (ioctl(client.fd, RK3588_NPU_SUBMIT, &submit, sizeof(submit)) != 0)
		status = errno;
	if (_duration != NULL)
		*_duration = system_time() - start;
	return status;
}


/*!	Runs the client's convolution on new random numbers and says whether
	the processor computed what it should have. */
static bool
run_good_job(Client& client)
{
	int8* input = (int8*)client.tensors.address + kInputOffset;
	int8* output = (int8*)client.tensors.address + kOutputOffset;

	for (uint32 i = 0; i < kPixels * kChannels; i++)
		input[i] = (int8)next_random(client.seed);
	memset(output, 0x55, kPixels * kChannels);

	if (!sync_buffer(client, client.tensors, RK3588_NPU_SYNC_FOR_DEVICE)
		|| submit_job(client) != B_OK
		|| !sync_buffer(client, client.tensors, RK3588_NPU_SYNC_FOR_CPU)) {
		return false;
	}

	for (uint32 pixel = 0; pixel < kPixels; pixel++) {
		for (uint32 oc = 0; oc < kChannels; oc++) {
			int32 sum = client.bias[oc];
			for (uint32 ic = 0; ic < kChannels; ic++) {
				sum += (int32)client.weights[oc * kChannels + ic]
					* input[pixel * kChannels + ic];
			}

			int32 expected = (sum + 32) >> 6;
			if (expected < -128)
				expected = -128;
			if (expected > 127)
				expected = 127;

			// the processor may round a tie the other way
			if (abs(output[pixel * kChannels + oc] - expected) > 1)
				return false;
		}
	}
	return true;
}


static int32
job_thread(void* data)
{
	Client* client = (Client*)data;
	for (int i = 0; i < 200; i++) {
		if (!run_good_job(*client))
			return i + 1;
	}
	return 0;
}


static int32
hanging_job_thread(void* data)
{
	return submit_job(*(Client*)data);
}


static int
test_jobs(int rounds)
{
	for (int round = 0; round < rounds; round++) {
		Client a;
		Client b;
		if (!init_client(a, 1 + round) || !init_client(b, 1000 + round)) {
			printf("FAILED: cannot set up clients: %s\n", strerror(errno));
			return 1;
		}

		check(run_good_job(a), "a convolution computes what it should");
		check(run_good_job(b), "another client's does, too");

		// A job that writes where the client has no memory: the IOMMU
		// stops it, the driver reports it, and the next job is fine.
		bigtime_t duration;
		make_commands(a, a.tensors.device_address + kInputOffset, kNowhere,
			a.constants.device_address + kWeightsOffset);
		status_t status = submit_job(a, &duration);
		check(status == B_BAD_ADDRESS && duration < 1000000,
			"a job that writes outside the client's memory fails at once");
		check(run_good_job(b), "the other client's job runs after it");
		make_good_commands(a);
		check(run_good_job(a), "and the client's own next job");

		// the same for reading: the input, the weights
		make_commands(a, kNowhere, a.tensors.device_address + kOutputOffset,
			a.constants.device_address + kWeightsOffset);
		status = submit_job(a, &duration);
		check(status == B_BAD_ADDRESS && duration < 1000000,
			"a job that reads its input from outside fails at once");
		check(run_good_job(b), "the other client's job runs after it");

		make_commands(a, a.tensors.device_address + kInputOffset,
			a.tensors.device_address + kOutputOffset, kNowhere);
		status = submit_job(a, &duration);
		check(status == B_BAD_ADDRESS && duration < 1000000,
			"a job that reads its weights from outside fails at once");
		check(run_good_job(b), "the other client's job runs after it");
		make_good_commands(a);
		check(run_good_job(a), "and the client's own next job");

		// One client cannot reach the other's memory: what is at an
		// address in one address space is not there in the other.
		rk3588_npu_create_buffer secret;
		memset(&secret, 0, sizeof(secret));
		secret.size = 65536;
		check(ioctl(b.fd, RK3588_NPU_CREATE_BUFFER, &secret, sizeof(secret))
			== 0, "a buffer only one client has");
		memset(secret.address, 0x77, secret.size);
		sync_buffer(b, secret, RK3588_NPU_SYNC_FOR_DEVICE);

		make_commands(a, a.tensors.device_address + kInputOffset,
			secret.device_address, a.constants.device_address + kWeightsOffset);
		status = submit_job(a);
		sync_buffer(b, secret, RK3588_NPU_SYNC_FOR_CPU);
		bool untouched = true;
		for (uint64 i = 0; i < secret.size; i++)
			untouched &= ((uint8*)secret.address)[i] == 0x77;
		check(status == B_BAD_ADDRESS && untouched,
			"a job cannot write to another client's buffer");
		make_good_commands(a);
		check(run_good_job(a) && run_good_job(b), "both clients go on");

		// A command list of nothing: the processor never says it is done,
		// the driver gives up after its timeout and resets the core.
		uint64* commands = (uint64*)((uint8*)a.constants.address
			+ kCommandsOffset);
		memset(commands, 0, kMaxTaskCommands * sizeof(uint64));
		sync_buffer(a, a.constants, RK3588_NPU_SYNC_FOR_DEVICE);
		status = submit_job(a, &duration);
		check(status == B_TIMED_OUT, "a job that never ends times out");
		check(run_good_job(b), "the other client's job runs after it");
		make_good_commands(a);
		check(run_good_job(a), "and the client's own next job");

		// two clients at once
		thread_id threads[2];
		threads[0] = spawn_thread(&job_thread, "jobs a", B_NORMAL_PRIORITY,
			&a);
		threads[1] = spawn_thread(&job_thread, "jobs b", B_NORMAL_PRIORITY,
			&b);
		resume_thread(threads[0]);
		resume_thread(threads[1]);
		status_t results[2] = { -1, -1 };
		wait_for_thread(threads[0], &results[0]);
		wait_for_thread(threads[1], &results[1]);
		check(results[0] == 0 && results[1] == 0,
			"200 jobs each of two clients at the same time");

		// A client that is killed while its job hangs.
		pid_t child = fork();
		if (child == 0) {
			Client c;
			if (!init_client(c, 77))
				_exit(1);
			uint64* list = (uint64*)((uint8*)c.constants.address
				+ kCommandsOffset);
			memset(list, 0, kMaxTaskCommands * sizeof(uint64));
			sync_buffer(c, c.constants, RK3588_NPU_SYNC_FOR_DEVICE);
			submit_job(c);
			_exit(0);
		}
		snooze(500000);
		kill(child, SIGKILL);
		int childStatus;
		waitpid(child, &childStatus, 0);
		check(run_good_job(a) && run_good_job(b),
			"jobs run after a client was killed in the middle of one");

		// A client that closes the device while its job hangs.
		Client c;
		check(init_client(c, 99), "a third client");
		uint64* list = (uint64*)((uint8*)c.constants.address
			+ kCommandsOffset);
		memset(list, 0, kMaxTaskCommands * sizeof(uint64));
		sync_buffer(c, c.constants, RK3588_NPU_SYNC_FOR_DEVICE);
		thread_id hanging = spawn_thread(&hanging_job_thread, "hanging job",
			B_NORMAL_PRIORITY, &c);
		resume_thread(hanging);
		snooze(500000);
		close(c.fd);
		c.fd = -1;
		status_t hangingResult;
		wait_for_thread(hanging, &hangingResult);
		check(hangingResult != B_OK,
			"a job fails whose client closed the device");
		check(run_good_job(a) && run_good_job(b),
			"jobs run after a client closed the device in the middle of "
			"one");
		uninit_client(c);

		uninit_client(a);
		uninit_client(b);
	}

	printf("%s\n", sFailures == 0 ? "all checks passed" : "CHECKS FAILED");
	return sFailures == 0 ? 0 : 1;
}


int
main(int argc, char** argv)
{
	if (argc >= 2 && strcmp(argv[1], "--jobs") == 0)
		return test_jobs(argc >= 3 ? atoi(argv[2]) : 1);

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
