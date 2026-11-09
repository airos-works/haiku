/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "Program.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <syslog.h>

#include <Autolock.h>
#include <OS.h>


Program::Program(Device* device)
	:
	fLock("amdxdna program"),
	fDevice(device),
	fAcquired(false),
	fBundle(NULL),
	fGraph(NULL),
	fContext(0),
	fHaveContext(false),
	fInstructionSize(0),
	fLost(false),
	fLastDeviceTime(0)
{
	for (int32 i = 0; i < XDNA_MAX_BUFFERS; i++)
		fTensors[i] = -1;
}


Program::~Program()
{
	if (fAcquired) {
		// The context first: the driver waits for its commands, and
		// nothing of the NPU's refers to the buffers after it.
		if (fHaveContext) {
			xdna_destroy_hwctx destroy;
			destroy.handle = fContext;
			destroy.pad = 0;
			ioctl(fDevice->FD(), XDNA_IOCTL_DESTROY_HWCTX, &destroy);
		}
		for (int32 i = 0; i < XDNA_MAX_BUFFERS; i++)
			fDevice->DeleteBuffer(fBuffers[i]);
		fDevice->DeleteBuffer(fCommand);
		fDevice->DeleteBuffer(fInstructions);
		fDevice->DeleteBuffer(fConfiguration);
		// the last program's release closes the node: the NPU goes off
		fDevice->Release();
	}
	delete fBundle;
}


/*!	The kit hands over what GetSupportedOperators() said yes to, cut
	where it likes: a program is made only for the bundle's stretch as a
	whole, with the border the bundle has buffers for. */
status_t
Program::_CheckPartition(const neural_partition& partition) const
{
	if (partition.operator_count != fBundle->CountOperators())
		return B_MISMATCHED_VALUES;
	for (int32 i = 0; i < partition.operator_count; i++) {
		if (partition.operators[i] != fBundle->FirstOperator() + i)
			return B_MISMATCHED_VALUES;
	}

	for (int pass = 0; pass < 2; pass++) {
		const int32* list = pass == 0 ? partition.inputs : partition.outputs;
		int32 count = pass == 0 ? partition.input_count
			: partition.output_count;
		for (int32 i = 0; i < count; i++) {
			bool found = false;
			for (int32 j = 0; j < fBundle->CountBuffers(); j++) {
				const bundle_buffer& buffer = fBundle->BufferAt(j);
				if (buffer.kind == (pass == 0 ? BUNDLE_BUFFER_INPUT
						: BUNDLE_BUFFER_OUTPUT)
					&& fTensors[j] == list[i]) {
					found = true;
				}
			}
			if (!found)
				return B_MISMATCHED_VALUES;
		}
	}
	return B_OK;
}


status_t
Program::Init(const BNeuralGraph& graph, const neural_partition& partition,
	Bundle* bundle)
{
	fBundle = bundle;
	fGraph = &graph;

	// The bundle and the network, once more: GetSupportedOperators()
	// answered from what it remembered, and this is where it counts.
	status_t status = fBundle->Check(graph);
	if (status != B_OK)
		return status;
	for (int32 i = 0; i < fBundle->CountBuffers(); i++)
		fTensors[i] = fBundle->TensorOf(graph, fBundle->BufferAt(i));
	status = _CheckPartition(partition);
	if (status != B_OK)
		return status;

	char digest[65];
	status = xdna_weights_digest(graph, fBundle->FirstOperator(),
		fBundle->CountOperators(), digest);
	if (status != B_OK)
		return status;
	if (strcmp(digest, fBundle->WeightsDigest()) != 0)
		return B_MISMATCHED_VALUES;

	void* configuration = NULL;
	void* instructions = NULL;
	size_t configurationSize = 0;
	status = fBundle->ReadFile(fBundle->PDI(), &configuration,
		&configurationSize);
	if (status == B_OK) {
		status = fBundle->ReadFile(fBundle->Instructions(), &instructions,
			&fInstructionSize);
	}
	if (status == B_OK && (fInstructionSize % 4 != 0
			|| configurationSize > XDNA_HEAP_SIZE / 4
			|| fInstructionSize > XDNA_HEAP_SIZE / 4)) {
		status = B_BAD_DATA;
	}

	// From here on the NPU is needed: this opens the node (root only),
	// which powers it up, unless another program has it open.
	if (status == B_OK)
		status = fDevice->Acquire();
	if (status != B_OK) {
		free(configuration);
		free(instructions);
		return status;
	}
	fAcquired = true;

	if (fBundle->Columns() > fDevice->Columns())
		status = B_NOT_SUPPORTED;

	// the design's configuration and instructions, in the device heap
	if (status == B_OK) {
		status = fDevice->CreateBuffer(XDNA_BO_DEV, configurationSize,
			fConfiguration);
	}
	if (status == B_OK) {
		status = fDevice->CreateBuffer(XDNA_BO_DEV, fInstructionSize,
			fInstructions);
	}
	if (status == B_OK) {
		memcpy(fConfiguration.data, configuration, configurationSize);
		memcpy(fInstructions.data, instructions, fInstructionSize);
		fDevice->Flush(fConfiguration);
		fDevice->Flush(fInstructions);
	}
	free(configuration);
	free(instructions);

	// the context: the design's share of the NPU's columns
	if (status == B_OK) {
		xdna_qos_info qos;
		memset(&qos, 0, sizeof(qos));
		qos.priority = XDNA_QOS_NORMAL_PRIORITY;
		xdna_create_hwctx create;
		memset(&create, 0, sizeof(create));
		create.qos_p = (addr_t)&qos;
		create.max_opc = fBundle->OpsPerCycle();
		create.num_tiles = fBundle->Columns() * fDevice->CoreRows();
		if (ioctl(fDevice->FD(), XDNA_IOCTL_CREATE_HWCTX, &create) != 0) {
			status = errno;
		} else {
			fContext = create.handle;
			fHaveContext = true;
		}
	}

	// the configuration becomes the context's one compute unit
	if (status == B_OK) {
		xdna_config_cu units;
		memset(&units, 0, sizeof(units));
		units.num_cus = 1;
		units.cu.cu_bo = fConfiguration.handle;
		xdna_config_hwctx config;
		memset(&config, 0, sizeof(config));
		config.handle = fContext;
		config.param_type = XDNA_HWCTX_CONFIG_CU;
		config.param_val = (addr_t)&units;
		config.param_val_size = sizeof(units);
		if (ioctl(fDevice->FD(), XDNA_IOCTL_CONFIG_HWCTX, &config) != 0)
			status = errno;
	}

	// the design's buffers; the weights are put in now
	for (int32 i = 0; status == B_OK && i < fBundle->CountBuffers(); i++) {
		const bundle_buffer& buffer = fBundle->BufferAt(i);
		status = fDevice->CreateBuffer(XDNA_BO_SHARE, buffer.size,
			fBuffers[i]);
		if (status == B_OK && buffer.kind == BUNDLE_BUFFER_CONSTANT) {
			const neural_graph_tensor& tensor = *graph.TensorAt(fTensors[i]);
			xdna_convert_layout(buffer.layout, tensor, (void*)tensor.data,
				fBuffers[i].data, true);
			fDevice->Flush(fBuffers[i]);
		}
	}
	if (status == B_OK)
		status = fDevice->CreateBuffer(XDNA_BO_CMD, B_PAGE_SIZE, fCommand);

	if (status != B_OK) {
		syslog(LOG_WARNING, "amdxdna.neural: design %s not loaded: %s",
			fBundle->Name(), strerror(status));
	}
	return status;
}


/*!	The run failed: the context may be in any state (the driver resets
	one whose command timed out), and the kit has one way to be told that
	the CPU has to compute: the stretch is the CPU's from now on. */
status_t
Program::_Lost(const char* what, status_t error)
{
	syslog(LOG_WARNING, "amdxdna.neural: design %s: %s: %s; left to the CPU",
		fBundle->Name(), what, strerror(error));
	fLost = true;
	return B_NEURAL_DEVICE_LOST;
}


status_t
Program::Run(void* const* tensors)
{
	BAutolock locker(fLock);
	if (fLost)
		return B_NEURAL_DEVICE_LOST;

	int32 count = fBundle->CountBuffers();
	for (int32 i = 0; i < count; i++) {
		const bundle_buffer& buffer = fBundle->BufferAt(i);
		if (buffer.kind != BUNDLE_BUFFER_INPUT)
			continue;
		xdna_convert_layout(buffer.layout, *fGraph->TensorAt(fTensors[i]),
			tensors[fTensors[i]], fBuffers[i].data, true);
		fDevice->Flush(fBuffers[i]);
	}

	// The command, as XRT writes it for a kernel of mlir-aie: the mask of
	// compute units, then the kernel's arguments as they lie in its
	// register map - an opcode (64 bits), the address of the instructions
	// (64) and their size (32), and five buffer addresses (64 each),
	// unused ones zero. The instructions name the buffers by their place
	// among the arguments; the firmware puts the addresses into the
	// NPU's data movers.
	uint32* command = (uint32*)fCommand.data;
	int32 words = 0;
	memset(command, 0, B_PAGE_SIZE);
	command[1 + words++] = 0x1;
	command[1 + words++] = fBundle->Opcode();
	command[1 + words++] = 0;
	command[1 + words++] = (uint32)fInstructions.address;
	command[1 + words++] = (uint32)(fInstructions.address >> 32);
	command[1 + words++] = fBundle->SizeInBytes()
		? fInstructionSize : fInstructionSize / 4;
	for (int32 i = 0; i < XDNA_MAX_BUFFERS; i++) {
		uint64 address = i < count ? fBuffers[i].address : 0;
		command[1 + words++] = (uint32)address;
		command[1 + words++] = (uint32)(address >> 32);
	}
	command[0] = XDNA_CMD_HEADER(XDNA_CMD_STATE_NEW, words,
		XDNA_CMD_START_CU);

	// the buffers the command refers to
	uint32 handles[XDNA_MAX_BUFFERS + 1];
	handles[0] = fInstructions.handle;
	for (int32 i = 0; i < count; i++)
		handles[1 + i] = fBuffers[i].handle;

	xdna_exec_cmd exec;
	memset(&exec, 0, sizeof(exec));
	exec.hwctx = fContext;
	exec.cmd_handles = fCommand.handle;
	exec.args = (addr_t)handles;
	exec.cmd_count = 1;
	exec.arg_count = 1 + count;

	bigtime_t start = system_time();
	if (ioctl(fDevice->FD(), XDNA_IOCTL_EXEC_CMD, &exec) != 0)
		return _Lost("command not taken", errno);

	xdna_wait_cmd wait;
	wait.hwctx = fContext;
	wait.timeout = fBundle->Timeout();
	wait.seq = exec.seq;
	if (ioctl(fDevice->FD(), XDNA_IOCTL_WAIT_CMD, &wait) != 0)
		return _Lost("no answer to the command", errno);
	fLastDeviceTime = system_time() - start;

	if (XDNA_CMD_STATE(command[0]) != XDNA_CMD_STATE_COMPLETED)
		return _Lost("command failed", B_IO_ERROR);

	for (int32 i = 0; i < count; i++) {
		const bundle_buffer& buffer = fBundle->BufferAt(i);
		// (an output nobody outside the stretch reads has no memory)
		if (buffer.kind != BUNDLE_BUFFER_OUTPUT
			|| tensors[fTensors[i]] == NULL) {
			continue;
		}
		fDevice->Flush(fBuffers[i]);
		xdna_convert_layout(buffer.layout, *fGraph->TensorAt(fTensors[i]),
			tensors[fTensors[i]], fBuffers[i].data, false);
	}
	return B_OK;
}


bigtime_t
Program::LastDeviceTime() const
{
	return fLastDeviceTime;
}
