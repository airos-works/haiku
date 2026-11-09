/*
 * Copyright 2026, air/OS contributors.
 * Copyright (c) 2024 Tomeu Vizoso <tomeu@tomeuvizoso.net>
 * Distributed under the terms of the MIT License.
 */
#ifndef RK3588_NPU_COMPILER_H
#define RK3588_NPU_COMPILER_H


/*!	What a stretch of a network becomes on its way to the processor: the
	convolutions it is made of (an addition is not an operation of its own,
	it becomes the last step of the convolution that makes one of its
	operands), the slices ("tasks") a convolution is cut into so that its
	input fits the processor's convolution buffer, and the tensors as the
	processor stores them.

	The structures and what is done with them follow Mesa's "rocket" driver
	(src/gallium/drivers/rocket, rkt_ml.h), which found out how the
	processor wants things by watching the vendor's driver. Numbers that
	driver could not explain are marked as such where they are used.

	The processor computes with signed 8 bit numbers only. A tensor of
	unsigned numbers is stored with 128 subtracted from every element, and
	its zero point with it; all zero points in these structures are in that
	signed form, whatever the type of the tensor in the network.
*/


#include <SupportDefs.h>


// The convolution buffer, see
// http://nvdla.org/hw/v1/ias/unit_description.html#convolution-buffer
#define CBUF_BANK_SIZE			32768
#define CBUF_BANKS				12
#define CBUF_ENTRIES_PER_BANK	256
#define CBUF_ENTRY_SIZE			(CBUF_BANK_SIZE / CBUF_ENTRIES_PER_BANK)
#define FEATURE_ATOMIC_SIZE		16
#define WEIGHT_ATOMIC_SIZE		32
#define ATOMIC_K_SIZE			16


//! Commands a task's list has room for; a convolution takes about 135.
static const uint32 kMaxTaskCommands = 160;


static inline uint32
round_up(uint32 value, uint32 to)
{
	return (value + to - 1) / to * to;
}


static inline uint32
divide_up(uint32 value, uint32 by)
{
	return (value + by - 1) / by;
}


/*!	A tensor in the processor's memory: slices of 16 channels each, one
	after the other, each of them rows of pixels of 16 bytes. Channels that
	the tensor does not have hold its zero point. */
struct TensorSlot {
	int32		tensor;
		// index in the graph
	uint32		width;
	uint32		height;
	uint32		channels;
	bool		is_signed;
		// int8 in the network: stored as it is
	int32		zero_point;
	uint32		offset;
		// in the program's tensor buffer
	uint32		size;
};


//! One slice of a convolution, what Mesa calls a split_task.
struct SplitTask {
	uint32		number;

	uint32		top_slice;
	uint32		bottom_slice;
	uint32		overlap_slices;
	uint32		retain_slices;
	uint32		convolutions;

	uint32		pad_top;
	uint32		pad_bottom;
	uint32		pad_left;
	uint32		pad_right;

	uint32		stride_x;
	uint32		stride_y;

	uint32		input_width;
	uint32		input_height;
	uint32		input_channels;
	uint32		input_channels_real;
	uint32		input_data_entries;
	int32		input_line_stride;
	int32		input_surface_stride;
	uint32		input_offset;

	uint32		output_width;
	uint32		output_height;
	uint32		output_channels;
	uint32		output_channels_real;
	int32		output_surface_stride;
	uint32		output_offset;

	uint32		weights_width;
	uint32		weights_height;
	uint32		weights_channels;
	uint32		weights_kernels;

	uint32		input_banks;
	uint32		weights_banks;

	uint32		atomic_count;
	uint32		surfaces_per_row;

	// where its command list is
	uint32		command_count;
	uint32		command_address;
};


//! A convolution, with the addition that follows it if there is one.
struct Operation {
	bool		depthwise;
	bool		reuse_weights_cbuf;
	uint32		truncate_bits;
	bool		padding_same;
	uint32		stride;

	// indices of the tensors in the graph
	int32		input_tensor;
	int32		output_tensor;
	int32		weights_tensor;
	int32		bias_tensor;
		// -1: none
	int32		add_tensor;
		// -1: no addition

	// indices of TensorSlots
	int32		input;
	int32		output;
	int32		addend;

	uint32		input_width;
	uint32		input_height;
	uint32		input_channels;
	int32		input_zero_point;
	float		input_scale;

	uint32		output_width;
	uint32		output_height;
	uint32		output_channels;
	int32		output_zero_point;
	float		output_scale;
		// of the sum where there is an addition

	uint32		weights_width;
	uint32		weights_height;
	bool		weights_signed;
	int32		weights_zero_point;
	float		weights_scale;

	int32		addend_zero_point;
	float		addend_scale;

	// offsets in the program's buffer of constants
	uint32		weights_offset;
	uint32		weights_size;
	uint32		bias_offset;
	uint32		bias_size;
	uint32		commands_offset;
	uint32		commands_size;

	SplitTask*	tasks;
	uint32		task_count;
};


//! Device addresses a command list refers to.
struct OperationAddresses {
	uint32		input;
	uint32		output;
	uint32		addend;
	uint32		weights;
	uint32		bias;
};


// Tasks.cpp
status_t	split_tasks(Operation& operation);
uint32		output_channels_written(const Operation& operation);

// Coefficients.cpp
uint32		weights_channels(const Operation& operation);
uint32		weights_buffer_size(const Operation& operation);
void		fill_weights(const Operation& operation, const uint8* weights,
				uint8* buffer);
void		fill_biases(const Operation& operation, const uint8* weights,
				const int32* biases, int32* buffer);

// Regcmd.cpp
uint32		fill_commands(const Operation& operation, uint32 taskIndex,
				const OperationAddresses& addresses, uint64* commands);
void		link_commands(uint64* commands, uint32 count, uint32 nextAddress,
				uint32 nextCount);
bool		output_conversion(double scale, uint32* _scale, uint32* _shift);
bool		addend_conversion(double scale, uint32* _scale, uint32* _shift);


#endif	// RK3588_NPU_COMPILER_H
