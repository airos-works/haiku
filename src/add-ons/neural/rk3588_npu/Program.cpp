/*
 * Copyright 2026, air/OS contributors.
 * Copyright (c) 2024 Tomeu Vizoso <tomeu@tomeuvizoso.net>
 * Distributed under the terms of the MIT License.
 */


/*!	Making and running a program; how operations follow one another and
	how tensors are laid out is from Mesa's rkt_ml.c.
*/


#include "Program.h"

#include <arm_neon.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include "Support.h"


// Everything the processor is given an address of starts on a page of
// its own, as it does with Mesa, where each is a buffer.
static const uint32 kAlignment = 4096;
// The processor reads in bursts and may read past the end of a tensor.
static const uint32 kTensorsTail = 4096;

static const uint32 kTaskCommandsSize = kMaxTaskCommands * sizeof(uint64);


//! Bytes of a tensor that hold its numbers (and those of its last group).
static inline uint32
used_size(const TensorSlot& slot)
{
	return slot.width * slot.height * FEATURE_ATOMIC_SIZE
		* divide_up(slot.channels, FEATURE_ATOMIC_SIZE);
}


Program::Program()
	:
	fDevice(NULL),
	fOperations(NULL),
	fOperationCount(0),
	fSlots(NULL),
	fSlotCount(0),
	fTasks(NULL),
	fTaskCount(0),
	fInputs(NULL),
	fInputCount(0),
	fOutputs(NULL),
	fOutputCount(0),
	fDeviceTime(0)
{
	pthread_mutex_init(&fLock, NULL);
}


Program::~Program()
{
	if (fDevice != NULL) {
		fDevice->DeleteBuffer(fConstants);
		fDevice->DeleteBuffer(fTensors);
		fDevice->Release();
	}

	for (int32 i = 0; i < fOperationCount; i++)
		free(fOperations[i].tasks);
	free(fOperations);
	free(fSlots);
	free(fTasks);
	free(fInputs);
	free(fOutputs);
	pthread_mutex_destroy(&fLock);
}


status_t
Program::Init(const BNeuralGraph& graph, const neural_partition& partition)
{
	if (partition.operator_count < 1)
		return B_BAD_VALUE;

	status_t status = _Lower(graph, partition);
	if (status != B_OK)
		return status;

	uint32 constantsSize;
	uint32 tensorsSize;
	status = _Layout(constantsSize, tensorsSize);
	if (status != B_OK)
		return status;

	status = _MakePorts(partition);
	if (status != B_OK)
		return status;

	status = Device::Acquire(&fDevice);
	if (status != B_OK)
		return status;

	status = fDevice->CreateBuffer(constantsSize, fConstants);
	if (status == B_OK)
		status = fDevice->CreateBuffer(tensorsSize, fTensors);
	if (status != B_OK)
		return status;

	return _Fill(graph);
}


/*!	Makes the operations of the partition's operators. */
status_t
Program::_Lower(const BNeuralGraph& graph, const neural_partition& partition)
{
	int32 graphOperators = graph.CountOperators();

	GraphUse use;
	status_t status = use.Init(graph);
	if (status != B_OK)
		return status;

	fOperations = (Operation*)calloc(partition.operator_count,
		sizeof(Operation));
	// an input, an output and an addend each, at most
	fSlots = (TensorSlot*)calloc(partition.operator_count * 3,
		sizeof(TensorSlot));
	bool* inProgram = (bool*)calloc(graphOperators, sizeof(bool));
	int32* operationOf = (int32*)malloc(graphOperators * sizeof(int32));
	if (fOperations == NULL || fSlots == NULL || inProgram == NULL
		|| operationOf == NULL) {
		free(inProgram);
		free(operationOf);
		return B_NO_MEMORY;
	}

	for (int32 i = 0; i < graphOperators; i++)
		operationOf[i] = -1;
	for (int32 i = 0; i < partition.operator_count; i++) {
		int32 index = partition.operators[i];
		if (index < 0 || index >= graphOperators)
			status = B_BAD_VALUE;
		else
			inProgram[index] = true;
	}

	for (int32 i = 0; status == B_OK && i < partition.operator_count; i++) {
		int32 index = partition.operators[i];
		const neural_graph_operator* op = graph.OperatorAt(index);

		if (op->code == B_NEURAL_OP_ADD) {
			int32 convolution;
			int32 addend;
			status = find_addition_target(graph, use, index, inProgram,
				&convolution, &addend);
			if (status == B_OK && operationOf[convolution] < 0)
				status = B_NOT_SUPPORTED;
			if (status == B_OK) {
				status = fuse_addition(graph, index, addend,
					fOperations[operationOf[convolution]]);
			}
		} else {
			status = describe_convolution(graph, index,
				fOperations[fOperationCount]);
			if (status == B_OK)
				operationOf[index] = fOperationCount++;
		}
	}

	free(inProgram);
	free(operationOf);
	if (status != B_OK)
		return status;

	for (int32 i = 0; i < fOperationCount; i++) {
		Operation& operation = fOperations[i];
		operation.input = _SlotFor(graph, operation.input_tensor);
		operation.output = _SlotFor(graph, operation.output_tensor);
		if (operation.add_tensor >= 0)
			operation.addend = _SlotFor(graph, operation.add_tensor);
	}

	return B_OK;
}


int32
Program::_SlotFor(const BNeuralGraph& graph, int32 tensorIndex)
{
	for (int32 i = 0; i < fSlotCount; i++) {
		if (fSlots[i].tensor == tensorIndex)
			return i;
	}

	const neural_graph_tensor* tensor = graph.TensorAt(tensorIndex);
	TensorSlot& slot = fSlots[fSlotCount];
	slot.tensor = tensorIndex;
	slot.height = tensor->dimensions[1];
	slot.width = tensor->dimensions[2];
	slot.channels = tensor->dimensions[3];
	slot.is_signed = tensor->type == B_NEURAL_INT8;
	slot.zero_point = tensor->zero_point - (slot.is_signed ? 0 : 0x80);
	return fSlotCount++;
}


/*!	Cuts the operations into tasks and decides where everything goes in
	the two buffers. */
status_t
Program::_Layout(uint32& constantsSize, uint32& tensorsSize)
{
	uint64 constants = 0;
	fTaskCount = 0;

	for (int32 i = 0; i < fOperationCount; i++) {
		Operation& operation = fOperations[i];

		status_t status = split_tasks(operation);
		if (status != B_OK)
			return status;
		fTaskCount += operation.task_count;

		operation.commands_offset = constants;
		operation.commands_size = round_up(
			operation.task_count * kTaskCommandsSize, kAlignment);
		constants += operation.commands_size;

		operation.weights_offset = constants;
		operation.weights_size = round_up(weights_buffer_size(operation),
			kAlignment);
		constants += operation.weights_size;

		// The processor reads a bias for every channel it writes.
		operation.bias_offset = constants;
		operation.bias_size = round_up(
			output_channels_written(operation) * sizeof(int32), kAlignment);
		constants += operation.bias_size;

		if (constants > 0x7fffffff)
			return B_NO_MEMORY;
	}

	uint64 tensors = 0;
	for (int32 i = 0; i < fSlotCount; i++) {
		TensorSlot& slot = fSlots[i];

		// Room for twice the tensor's groups of channels, as Mesa has
		// it, and for as many as the operation that makes it writes.
		uint32 groups = divide_up(slot.channels, FEATURE_ATOMIC_SIZE) * 2;
		for (int32 j = 0; j < fOperationCount; j++) {
			if (fOperations[j].output == i) {
				groups = std::max(groups,
					output_channels_written(fOperations[j])
						/ FEATURE_ATOMIC_SIZE);
			}
		}

		uint64 size = (uint64)slot.width * slot.height * FEATURE_ATOMIC_SIZE
			* groups;
		size = (size + kAlignment - 1) / kAlignment * kAlignment;

		slot.offset = tensors;
		slot.size = size;
		tensors += size;

		if (tensors > 0x7fffffff)
			return B_NO_MEMORY;
	}

	constantsSize = constants;
	tensorsSize = tensors + kTensorsTail;
	return B_OK;
}


/*!	Which slots the tensors are in that go in and come out. */
status_t
Program::_MakePorts(const neural_partition& partition)
{
	fInputs = (Port*)calloc(std::max(partition.input_count, (int32)1),
		sizeof(Port));
	fOutputs = (Port*)calloc(std::max(partition.output_count, (int32)1),
		sizeof(Port));
	if (fInputs == NULL || fOutputs == NULL)
		return B_NO_MEMORY;

	for (int32 i = 0; i < partition.input_count; i++) {
		int32 slot = -1;
		for (int32 j = 0; j < fSlotCount; j++) {
			if (fSlots[j].tensor == partition.inputs[i])
				slot = j;
		}
		if (slot < 0)
			return B_NOT_SUPPORTED;

		// what an operation of the program makes cannot come from outside
		for (int32 j = 0; j < fOperationCount; j++) {
			if (fOperations[j].output == slot)
				return B_NOT_SUPPORTED;
		}

		fInputs[fInputCount].tensor = partition.inputs[i];
		fInputs[fInputCount++].slot = slot;
	}

	for (int32 i = 0; i < partition.output_count; i++) {
		// A tensor that is wanted outside has to be made: the sum of a
		// convolution that went on to add to it is not.
		int32 slot = -1;
		for (int32 j = 0; j < fOperationCount; j++) {
			if (fOperations[j].output_tensor == partition.outputs[i])
				slot = fOperations[j].output;
		}
		if (slot < 0)
			return B_NOT_SUPPORTED;

		fOutputs[fOutputCount].tensor = partition.outputs[i];
		fOutputs[fOutputCount++].slot = slot;
	}

	// Every tensor the operations read is made by one of them or comes
	// in; otherwise the program would compute with what happens to be in
	// memory.
	for (int32 i = 0; i < fSlotCount; i++) {
		bool known = false;
		for (int32 j = 0; !known && j < fOperationCount; j++)
			known = fOperations[j].output == i;
		for (int32 j = 0; !known && j < fInputCount; j++)
			known = fInputs[j].slot == i;
		if (!known)
			return B_NOT_SUPPORTED;
	}

	return B_OK;
}


/*!	Writes weights, biases and command lists and makes the list of
	tasks. */
status_t
Program::_Fill(const BNeuralGraph& graph)
{
	fTasks = (rk3588_npu_task*)calloc(fTaskCount, sizeof(rk3588_npu_task));
	if (fTasks == NULL)
		return B_NO_MEMORY;

	uint32 taskIndex = 0;
	for (int32 i = 0; i < fOperationCount; i++) {
		Operation& operation = fOperations[i];

		const neural_graph_tensor* weights = graph.TensorAt(
			operation.weights_tensor);
		const neural_graph_tensor* bias = graph.TensorAt(
			operation.bias_tensor);

		fill_weights(operation, (const uint8*)weights->data,
			fConstants.address + operation.weights_offset);
		fill_biases(operation, (const uint8*)weights->data,
			bias != NULL ? (const int32*)bias->data : NULL,
			(int32*)(fConstants.address + operation.bias_offset));

		OperationAddresses addresses;
		addresses.input = fTensors.device_address
			+ fSlots[operation.input].offset;
		addresses.output = fTensors.device_address
			+ fSlots[operation.output].offset;
		addresses.addend = operation.addend >= 0
			? fTensors.device_address + fSlots[operation.addend].offset : 0;
		addresses.weights = fConstants.device_address
			+ operation.weights_offset;
		addresses.bias = fConstants.device_address + operation.bias_offset;

		uint64* commands = (uint64*)(fConstants.address
			+ operation.commands_offset);
		for (uint32 j = 0; j < operation.task_count; j++) {
			SplitTask& task = operation.tasks[j];
			task.command_count = fill_commands(operation, j, addresses,
				commands + j * kMaxTaskCommands);
			if (task.command_count >= kMaxTaskCommands)
				return B_ERROR;
			task.command_address = fConstants.device_address
				+ operation.commands_offset + j * kTaskCommandsSize;

			fTasks[taskIndex].regcmd = task.command_address;
			fTasks[taskIndex].regcmd_count = task.command_count;
			taskIndex++;
		}

		for (uint32 j = 0; j + 1 < operation.task_count; j++) {
			link_commands(commands + j * kMaxTaskCommands,
				operation.tasks[j].command_count,
				operation.tasks[j + 1].command_address,
				operation.tasks[j + 1].command_count);
		}
	}

	return fDevice->Sync(fConstants, RK3588_NPU_SYNC_FOR_DEVICE, 0, 0);
}


// #pragma mark - running


static const uint8 kChannelMasks[17][16] = {
#define M(n) { \
	(uint8)(n > 0 ? 0xff : 0), (uint8)(n > 1 ? 0xff : 0), \
	(uint8)(n > 2 ? 0xff : 0), (uint8)(n > 3 ? 0xff : 0), \
	(uint8)(n > 4 ? 0xff : 0), (uint8)(n > 5 ? 0xff : 0), \
	(uint8)(n > 6 ? 0xff : 0), (uint8)(n > 7 ? 0xff : 0), \
	(uint8)(n > 8 ? 0xff : 0), (uint8)(n > 9 ? 0xff : 0), \
	(uint8)(n > 10 ? 0xff : 0), (uint8)(n > 11 ? 0xff : 0), \
	(uint8)(n > 12 ? 0xff : 0), (uint8)(n > 13 ? 0xff : 0), \
	(uint8)(n > 14 ? 0xff : 0), (uint8)(n > 15 ? 0xff : 0) }
	M(0), M(1), M(2), M(3), M(4), M(5), M(6), M(7), M(8), M(9), M(10),
	M(11), M(12), M(13), M(14), M(15), M(16)
#undef M
};


/*!	For how many pixels of a tensor of the network 16 bytes can be read or
	written at its last, incomplete group of channels without leaving the
	tensor. */
static inline uint32
whole_vectors(uint32 pixels, uint32 channels, uint32 fullGroups)
{
	size_t total = (size_t)pixels * channels;
	size_t first = (size_t)fullGroups * FEATURE_ATOMIC_SIZE;
	if (total < first + FEATURE_ATOMIC_SIZE)
		return 0;
	return std::min((uint32)((total - first - FEATURE_ATOMIC_SIZE) / channels
		+ 1), pixels);
}


/*!	Puts a tensor of the network (rows of pixels of channels) into its
	slot: groups of 16 channels, each rows of pixels of 16 bytes, signed.
	Channels the last group has too many are set to the zero point. */
void
Program::_Store(const TensorSlot& slot, const uint8* source)
{
	uint8* target = fTensors.address + slot.offset;
	uint32 pixels = slot.width * slot.height;
	uint32 channels = slot.channels;
	uint32 fullGroups = channels / FEATURE_ATOMIC_SIZE;
	uint32 rest = channels % FEATURE_ATOMIC_SIZE;
	size_t surface = (size_t)pixels * FEATURE_ATOMIC_SIZE;
	uint8 flipByte = slot.is_signed ? 0 : 0x80;
	uint8x16_t flip = vdupq_n_u8(flipByte);

	for (uint32 group = 0; group < fullGroups; group++) {
		const uint8* in = source + group * FEATURE_ATOMIC_SIZE;
		uint8* out = target + group * surface;
		for (uint32 i = 0; i < pixels; i++) {
			vst1q_u8(out, veorq_u8(vld1q_u8(in), flip));
			in += channels;
			out += FEATURE_ATOMIC_SIZE;
		}
	}

	if (rest == 0)
		return;

	const uint8* in = source + fullGroups * FEATURE_ATOMIC_SIZE;
	uint8* out = target + fullGroups * surface;
	uint8x16_t mask = vld1q_u8(kChannelMasks[rest]);
	uint8x16_t padding = vdupq_n_u8((uint8)slot.zero_point);

	// Reading 16 bytes reads into the pixels that follow, which is
	// harmless as long as there are some.
	uint32 wide = whole_vectors(pixels, channels, fullGroups);
	for (uint32 i = 0; i < wide; i++) {
		vst1q_u8(out, vbslq_u8(mask, veorq_u8(vld1q_u8(in), flip), padding));
		in += channels;
		out += FEATURE_ATOMIC_SIZE;
	}
	for (uint32 i = wide; i < pixels; i++) {
		for (uint32 j = 0; j < FEATURE_ATOMIC_SIZE; j++)
			out[j] = j < rest ? in[j] ^ flipByte : (uint8)slot.zero_point;
		in += channels;
		out += FEATURE_ATOMIC_SIZE;
	}
}


/*!	The other way around. */
void
Program::_Load(const TensorSlot& slot, uint8* target)
{
	const uint8* source = fTensors.address + slot.offset;
	uint32 pixels = slot.width * slot.height;
	uint32 channels = slot.channels;
	uint32 fullGroups = channels / FEATURE_ATOMIC_SIZE;
	uint32 rest = channels % FEATURE_ATOMIC_SIZE;
	size_t surface = (size_t)pixels * FEATURE_ATOMIC_SIZE;
	uint8 flipByte = slot.is_signed ? 0 : 0x80;
	uint8x16_t flip = vdupq_n_u8(flipByte);

	// The group that is not whole goes first: writing 16 bytes of it
	// writes into the first group of the next pixel, which is then put
	// right by that group's turn (or by the next pixel, if there is one
	// group only).
	if (rest != 0) {
		const uint8* in = source + fullGroups * surface;
		uint8* out = target + fullGroups * FEATURE_ATOMIC_SIZE;
		uint32 wide = whole_vectors(pixels, channels, fullGroups);
		for (uint32 i = 0; i < wide; i++) {
			vst1q_u8(out, veorq_u8(vld1q_u8(in), flip));
			in += FEATURE_ATOMIC_SIZE;
			out += channels;
		}
		for (uint32 i = wide; i < pixels; i++) {
			for (uint32 j = 0; j < rest; j++)
				out[j] = in[j] ^ flipByte;
			in += FEATURE_ATOMIC_SIZE;
			out += channels;
		}
	}

	for (uint32 group = 0; group < fullGroups; group++) {
		const uint8* in = source + group * surface;
		uint8* out = target + group * FEATURE_ATOMIC_SIZE;
		for (uint32 i = 0; i < pixels; i++) {
			vst1q_u8(out, veorq_u8(vld1q_u8(in), flip));
			in += FEATURE_ATOMIC_SIZE;
			out += channels;
		}
	}
}


status_t
Program::Run(void* const* tensors)
{
	pthread_mutex_lock(&fLock);

	status_t status = B_OK;
	for (int32 i = 0; status == B_OK && i < fInputCount; i++) {
		const TensorSlot& slot = fSlots[fInputs[i].slot];
		const uint8* source = (const uint8*)tensors[fInputs[i].tensor];
		if (source == NULL) {
			status = B_BAD_VALUE;
			break;
		}

		_Store(slot, source);
		status = fDevice->Sync(fTensors, RK3588_NPU_SYNC_FOR_DEVICE,
			slot.offset, used_size(slot));
	}

	if (status == B_OK) {
		status = fDevice->Submit(fTasks, fTaskCount, &fDeviceTime);

		// A job that ran out of time, or found the processor in a state
		// left by somebody's failed job, gets a second chance: the driver
		// has reset the processor by now. A job that reached outside its
		// memory would do so again.
		if (status == B_TIMED_OUT || status == B_IO_ERROR)
			status = fDevice->Submit(fTasks, fTaskCount, &fDeviceTime);
	}

	for (int32 i = 0; status == B_OK && i < fOutputCount; i++) {
		const TensorSlot& slot = fSlots[fOutputs[i].slot];
		uint8* target = (uint8*)tensors[fOutputs[i].tensor];
		if (target == NULL) {
			status = B_BAD_VALUE;
			break;
		}

		status = fDevice->Sync(fTensors, RK3588_NPU_SYNC_FOR_CPU,
			slot.offset, used_size(slot));
		if (status == B_OK)
			_Load(slot, target);
	}

	pthread_mutex_unlock(&fLock);

	if (status != B_OK && Device::IsGone(status))
		return B_NEURAL_DEVICE_LOST;
	return status;
}


bigtime_t
Program::LastDeviceTime() const
{
	return fDeviceTime;
}
