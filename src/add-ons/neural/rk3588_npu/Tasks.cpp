/*
 * Copyright 2026, air/OS contributors.
 * Copyright (c) 2024 Tomeu Vizoso <tomeu@tomeuvizoso.net>
 * Distributed under the terms of the MIT License.
 */


/*!	Cuts a convolution into tasks. The processor reads the input of a
	convolution and its weights into its convolution buffer first, twelve
	"banks" of 32 KiB. What does not fit there at once is done in several
	tasks, each on a band of rows of the input; bands overlap by what the
	kernel needs.

	From Mesa's rkt_task.c, which says that it has this mostly from NVDLA.
	Mesa calls the first spatial dimension of a tensor its width; here
	width and height are what they are in the network, and rows ("slices")
	run along the width.
*/


#include "Compiler.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>


/*!	How many entries of the convolution buffer a row of the input takes,
	as the processor counts them: an entry holds four groups of 16
	channels of a pixel, or two pixels of two groups, or four pixels of
	one. Three groups do not share an entry with the next pixel.

	Mesa has "width * 2 * groups / 8" and an exception for the one case of
	three groups it met (40 channels, 40 wide); the rule above is NVDLA's
	and gives the same except for 3, 7, 11 ... groups, where Mesa's value
	makes the processor compute garbage. */
static uint32
data_entries(const Operation& operation)
{
	uint32 groups = divide_up(operation.input_channels, FEATURE_ATOMIC_SIZE);
	uint32 lastGroups = groups % 4;

	return (groups / 4) * operation.input_width + (lastGroups == 3
		? operation.input_width
		: divide_up(lastGroups * operation.input_width, 4));
}


/*!	The same in the units the banks are counted in for cutting the
	input: half as many, as in Mesa. */
static uint32
entries_per_slice(const Operation& operation)
{
	return divide_up(data_entries(operation), 2);
}


static uint32
input_banks(const Operation& operation)
{
	return divide_up(entries_per_slice(operation) * operation.input_height,
		CBUF_ENTRIES_PER_BANK);
}


//! Bytes of weights a kernel has, as they are given to the processor.
static uint32
bytes_per_kernel(const Operation& operation)
{
	return operation.weights_width * operation.weights_height
		* weights_channels(operation);
}


/*!	Banks all of the weights take. Mesa counts the channels the tensor
	has; the processor is given whole groups of 16, which is up to twice
	as much, and computes garbage when it is told it has room for all the
	weights and has not. */
static uint32
weights_banks(const Operation& operation)
{
	uint64 bytes = bytes_per_kernel(operation);
	if (!operation.depthwise)
		bytes *= round_up(operation.output_channels, 2);

	uint32 banks = (bytes + CBUF_BANK_SIZE - 1) / CBUF_BANK_SIZE;

	// Mesa: "Why do we need an extra bank? The calc above might be wrong
	// on this HW"
	return banks + 1;
}


/*!	Banks the weights need at the least: when they do not fit all at
	once, the processor takes them 32 kernels at a time, and a job whose
	32 kernels do not fit never ends. */
static uint32
kernel_group_banks(const Operation& operation)
{
	uint32 kernels = operation.depthwise
		? 1 : std::min(round_up(operation.output_channels, 2),
			(uint32)WEIGHT_ATOMIC_SIZE);
	return divide_up(kernels * bytes_per_kernel(operation), CBUF_BANK_SIZE);
}


static uint32
line_stride(uint32 width)
{
	return width * ATOMIC_K_SIZE;
}


/*!	The padding TensorFlow means by "same": as much as it takes for every
	input position to be the centre of a window, the odd one after. */
static void
explicit_padding(const Operation& operation, uint32& top, uint32& bottom,
	uint32& left, uint32& right)
{
	top = bottom = left = right = 0;
	if (!operation.padding_same)
		return;

	int32 alongWidth = (int32)((operation.output_width - 1) * operation.stride
		+ operation.weights_width) - (int32)operation.input_width;
	int32 alongHeight = (int32)((operation.output_height - 1)
		* operation.stride + operation.weights_height)
		- (int32)operation.input_height;

	if (alongWidth > 0) {
		left = alongWidth / 2;
		right = alongWidth - left;
	}
	if (alongHeight > 0) {
		top = alongHeight / 2;
		bottom = alongHeight - top;
	}
}


/*!	How many channels the processor writes for an operation: more than the
	tensor has, in whole groups. The memory of the tensor has to be there
	for them. */
uint32
output_channels_written(const Operation& operation)
{
	uint32 channels = round_up(std::max(operation.output_channels, (uint32)32),
		32);
	if (operation.depthwise) {
		if (operation.output_channels <= 32)
			channels *= 2;
		channels = round_up(channels, 64);
	}
	return channels;
}


static void
fill_task(const Operation& operation, SplitTask& task)
{
	task.stride_x = operation.stride;
	task.stride_y = operation.stride;

	task.input_width = operation.input_width;
	task.input_height = operation.input_height;
	task.input_channels = round_up(std::max(operation.input_channels,
		(uint32)FEATURE_ATOMIC_SIZE), FEATURE_ATOMIC_SIZE);
	task.input_channels_real = operation.input_channels;

	task.output_width = operation.output_width;
	task.output_height = operation.output_height;
	task.output_channels_real = operation.output_channels;
	task.output_channels = output_channels_written(operation);

	// Mesa computes the surface stride as a float and lets it go negative
	// for inputs of fewer than four rows; the register takes that.
	task.input_line_stride = line_stride(operation.input_width) / 4;
	task.input_surface_stride = (int32)operation.input_width
		* ((int32)operation.input_height - 4);

	task.output_surface_stride = line_stride(operation.output_width)
		* operation.output_height / FEATURE_ATOMIC_SIZE;

	task.input_data_entries = data_entries(operation);

	task.weights_width = operation.weights_width;
	task.weights_height = operation.weights_height;
	task.weights_channels = weights_channels(operation);
	task.weights_kernels = operation.depthwise
		? 1 : round_up(operation.output_channels, 2);

	task.surfaces_per_row = operation.output_width * operation.output_height
		* 2;
	if (operation.depthwise)
		task.surfaces_per_row *= 2;
}


status_t
split_tasks(Operation& operation)
{
	uint32 entriesPerSlice = entries_per_slice(operation);
	uint32 inputBanksRequired = input_banks(operation);
	uint32 weightsBanksRequired = weights_banks(operation);
	uint32 availableWeightsBanks = weightsBanksRequired;
	uint32 availableInputBanks = 0;

	uint32 padTop;
	uint32 padBottom;
	uint32 padLeft;
	uint32 padRight;
	explicit_padding(operation, padTop, padBottom, padLeft, padRight);

	if (weightsBanksRequired + 1 < CBUF_BANKS) {
		// all of the weights, part of the input
		operation.reuse_weights_cbuf = true;
		availableInputBanks = CBUF_BANKS - weightsBanksRequired;
	} else {
		// Part of the weights, part of the input: Mesa's five banks for
		// the weights, or as many as one group of kernels takes.
		operation.reuse_weights_cbuf = false;
		availableWeightsBanks = std::max((uint32)5,
			kernel_group_banks(operation));
		if (availableWeightsBanks >= CBUF_BANKS)
			return B_NOT_SUPPORTED;
		availableInputBanks = CBUF_BANKS - availableWeightsBanks;
	}

	free(operation.tasks);
	operation.tasks = NULL;
	operation.task_count = 0;

	if (inputBanksRequired <= availableInputBanks) {
		// all of the input at once
		SplitTask* task = (SplitTask*)calloc(1, sizeof(SplitTask));
		if (task == NULL)
			return B_NO_MEMORY;

		fill_task(operation, *task);
		task->input_banks = inputBanksRequired;
		task->weights_banks = CBUF_BANKS - task->input_banks;

		task->pad_top = padTop;
		task->pad_bottom = padBottom;
		task->pad_left = padLeft;
		task->pad_right = padRight;

		task->atomic_count = task->output_width * task->output_height;

		operation.tasks = task;
		operation.task_count = 1;
		return B_OK;
	}

	uint32 availableSlices = (CBUF_ENTRIES_PER_BANK * availableInputBanks)
		/ entriesPerSlice;
	// A band has to hold a window of the kernel and move on by a stride.
	if (availableSlices < operation.weights_height + operation.stride)
		return B_NOT_SUPPORTED;

	// No more tasks than rows; the loop below ends long before.
	uint32 capacity = operation.input_height + 2;
	SplitTask* tasks = (SplitTask*)calloc(capacity, sizeof(SplitTask));
	if (tasks == NULL)
		return B_NO_MEMORY;
	uint32 count = 0;

	SplitTask task;
	memset(&task, 0, sizeof(task));
	fill_task(operation, task);
	task.input_banks = availableInputBanks;
	task.weights_banks = availableWeightsBanks;
	task.top_slice = 0;
	task.bottom_slice = availableSlices - 1;
	task.pad_top = padTop;
	task.pad_left = padLeft;
	task.pad_right = padRight;
	tasks[count++] = task;

	for (uint32 slice = operation.weights_height - padTop - 1;
			slice < operation.input_height;) {
		memset(&task, 0, sizeof(task));

		SplitTask& previous = tasks[count - 1];

		while (slice <= previous.bottom_slice)
			slice += operation.stride;
		if (slice > previous.bottom_slice)
			slice -= operation.stride;

		task.number = count;
		fill_task(operation, task);
		task.top_slice = std::min(slice, previous.bottom_slice)
			- (operation.weights_height - 1) + operation.stride;
		task.bottom_slice = task.top_slice + availableSlices - 1;
		task.pad_left = padLeft;
		task.pad_right = padRight;

		if (count == capacity) {
			free(tasks);
			return B_NOT_SUPPORTED;
		}

		// the last one?
		if (task.bottom_slice >= operation.input_height - 1) {
			task.bottom_slice = operation.input_height - 1;
			task.pad_bottom = padBottom;
			tasks[count++] = task;
			break;
		}

		slice = task.top_slice + operation.weights_height - 1;
		tasks[count++] = task;
	}

	SplitTask& last = tasks[count - 1];
	if (last.top_slice >= operation.input_height
		|| last.bottom_slice >= operation.input_height + padBottom) {
		count--;
	}

	// how the bands overlap
	for (uint32 i = 1; i < count; i++) {
		SplitTask& previous = tasks[i - 1];
		SplitTask& current = tasks[i];

		if (previous.bottom_slice >= current.top_slice) {
			current.overlap_slices = previous.bottom_slice - current.top_slice
				+ 1;
			previous.retain_slices = current.overlap_slices;
		} else {
			current.overlap_slices = 0;
			previous.retain_slices = 0;
		}
	}

	uint32 outputHeightProcessed = 0;
	for (uint32 i = 0; i < count; i++) {
		SplitTask& current = tasks[i];

		uint32 slice = current.top_slice + (operation.weights_height - 1)
			- current.pad_top;
		while (slice <= current.bottom_slice + current.pad_bottom) {
			slice += operation.stride;
			current.convolutions++;
		}

		current.bottom_slice = std::min(current.bottom_slice,
			operation.input_height - 1);
		current.input_height = current.bottom_slice - current.top_slice + 1;

		current.output_width = (current.input_width + current.pad_left
			+ current.pad_right - operation.weights_width) / operation.stride
			+ 1;
		current.output_height = (current.input_height + current.pad_top
			+ current.pad_bottom - operation.weights_height)
			/ operation.stride + 1;
		current.atomic_count = current.output_width * current.output_height;

		current.input_offset = line_stride(operation.input_width)
			* current.top_slice;
		current.output_offset = line_stride(operation.output_width)
			* outputHeightProcessed;

		current.input_banks = availableInputBanks;
		current.weights_banks = availableWeightsBanks;

		outputHeightProcessed += current.output_height;
	}

	// The bands have to make exactly the rows the output has: a tensor
	// with rows missing or written twice is not something to find out
	// from wrong answers.
	if (outputHeightProcessed != operation.output_height) {
		free(tasks);
		return B_NOT_SUPPORTED;
	}

	operation.tasks = tasks;
	operation.task_count = count;
	return B_OK;
}
