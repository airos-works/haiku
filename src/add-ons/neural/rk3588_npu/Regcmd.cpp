/*
 * Copyright 2026, air/OS contributors.
 * Copyright (c) 2024 Tomeu Vizoso <tomeu@tomeuvizoso.net>
 * Distributed under the terms of the MIT License.
 */


/*!	The "register command" list of a task: the values of the registers of
	the processor's units for one convolution (or one band of it), which
	the processor reads from memory and puts into the registers itself.
	A command is 64 bits: the unit in the top 16, the value in the middle
	32, the register in the low 16.

	The list is Mesa's (rkt_regcmd.c), in its order. What differs:

	- The scales that turn sums into outputs, and addends into sums, are
	  computed here for any tensors. Mesa has tables for the networks it
	  was tried with.
	- Zero points are those of signed tensors (see Compiler.h), so that
	  networks of int8 numbers work as well as those of uint8 ones.
	- Inputs of a single channel, which the processor takes in another
	  layout, are not supported and their registers left out.
*/


#include "Compiler.h"

#include <math.h>
#include <stdint.h>

#include <algorithm>

#include "rkt_registers.h"


namespace {


struct Commands {
	uint64*		list;
	uint32		count;

	void Raw(uint32 target, uint32 reg, uint32 value)
	{
		if (count >= kMaxTaskCommands)
			return;
		list[count++] = ((uint64)target << 48) | ((uint64)value << 16)
			| (uint64)reg;
	}

	void Emit(uint32 reg, uint32 value)
	{
		Raw(rkt_get_target(reg) + 0x1, reg, value);
	}

	void Append(uint64 value)
	{
		if (count < kMaxTaskCommands)
			list[count++] = value;
	}
};


}	// namespace


#define EMIT(reg, value) commands.Emit(reg, value)


/*!	A positive factor as the processor multiplies by it: an integer of 15
	bits and a number of bits to shift the product right by. */
static bool
fixed_point(double scale, uint32 maxShift, uint32* _scale, uint32* _shift)
{
	if (!(scale > 0) || !isfinite(scale))
		return false;

	int exponent;
	double mantissa = frexp(scale, &exponent);
		// 0.5 <= mantissa < 1
	int64 value = llround(mantissa * 32768.0);
	if (value >= 32768) {
		value = 16384;
		exponent++;
	}

	int shift = 15 - exponent;
	if (shift < 0 || shift > (int)maxShift)
		return false;

	*_scale = (uint32)value;
	*_shift = (uint32)shift;
	return true;
}


/*!	The factor that makes outputs of sums: the scale of the sums (input
	scale times weights scale) over the scale of the output. */
bool
output_conversion(double scale, uint32* _scale, uint32* _shift)
{
	return fixed_point(scale, 31, _scale, _shift);
}


/*!	What the processor makes of a sum with a factor of
	output_conversion(), before the zero point is added. */
int64
converted_output(const Operation& operation, int64 sum, uint32 scale,
	uint32 shift)
{
	int64 product = ((sum << operation.rounding_bits)
		+ operation.rounding_offset) * (int64)scale;
	shift += operation.rounding_bits;
	if (shift == 0)
		return product;
	return (product + ((int64)1 << (shift - 1))) >> shift;
}


/*!	The factor that brings the other operand of an addition to the scale
	of the sums. */
bool
addend_conversion(double scale, uint32* _scale, uint32* _shift)
{
	return fixed_point(scale, 31, _scale, _shift);
}


uint32
fill_commands(const Operation& operation, uint32 taskIndex,
	const OperationAddresses& addresses, uint64* list)
{
	const SplitTask& task = operation.tasks[taskIndex];
	Commands commands = { list, 0 };

	bool addition = operation.add_tensor >= 0;
	double sumScale = (double)operation.input_scale * operation.weights_scale
		* (double)((uint32)1 << operation.truncate_bits);

	uint32 con0 = CNA_CBUF_CON0_WEIGHT_BANK(task.weights_banks)
		| CNA_CBUF_CON0_DATA_BANK(task.input_banks);
	if (taskIndex > 0 && operation.reuse_weights_cbuf)
		con0 |= CNA_CBUF_CON0_WEIGHT_REUSE(1);

	EMIT(REG_CNA_CBUF_CON0, con0);

	EMIT(REG_CNA_DCOMP_REGNUM, 0);
	EMIT(REG_CNA_DCOMP_CTRL, 0);

	uint32 con1 = 0;
	if (operation.depthwise)
		con1 |= CNA_CONV_CON1_CONV_MODE(3);

	EMIT(REG_CNA_CONV_CON1, con1);

	EMIT(REG_DPU_S_POINTER, DPU_S_POINTER_POINTER_PP_MODE(1)
		| DPU_S_POINTER_EXECUTER_PP_EN(1) | DPU_S_POINTER_POINTER_PP_EN(1));
	EMIT(REG_DPU_RDMA_RDMA_S_POINTER,
		DPU_RDMA_RDMA_S_POINTER_POINTER_PP_MODE(1)
		| DPU_RDMA_RDMA_S_POINTER_EXECUTER_PP_EN(1)
		| DPU_RDMA_RDMA_S_POINTER_POINTER_PP_EN(1));
	EMIT(REG_CNA_CONV_CON1, con1);
	// Mesa: "Magic: Seems to pass the most tests"
	EMIT(REG_CNA_CONV_CON2,
		CNA_CONV_CON2_FEATURE_GRAINS(50 + task.stride_y + 1));
	EMIT(REG_CNA_CONV_CON3, CNA_CONV_CON3_CONV_X_STRIDE(task.stride_x)
		| CNA_CONV_CON3_CONV_Y_STRIDE(task.stride_y));
	EMIT(REG_CNA_DATA_SIZE0, CNA_DATA_SIZE0_DATAIN_WIDTH(task.input_width)
		| CNA_DATA_SIZE0_DATAIN_HEIGHT(task.input_height));

	EMIT(REG_CNA_DATA_SIZE1,
		CNA_DATA_SIZE1_DATAIN_CHANNEL_REAL(task.input_channels_real - 1)
		| CNA_DATA_SIZE1_DATAIN_CHANNEL(task.input_channels));

	EMIT(REG_CNA_DATA_SIZE2, CNA_DATA_SIZE2_DATAOUT_WIDTH(task.output_width));
	EMIT(REG_CNA_DATA_SIZE3,
		CNA_DATA_SIZE3_DATAOUT_ATOMICS(task.atomic_count));
	EMIT(REG_CNA_WEIGHT_SIZE0, task.weights_width * task.weights_height
		* task.weights_channels * task.weights_kernels);
	EMIT(REG_CNA_WEIGHT_SIZE1, task.weights_width * task.weights_height
		* task.weights_channels);
	EMIT(REG_CNA_WEIGHT_SIZE2,
		CNA_WEIGHT_SIZE2_WEIGHT_WIDTH(task.weights_width)
		| CNA_WEIGHT_SIZE2_WEIGHT_HEIGHT(task.weights_height)
		| CNA_WEIGHT_SIZE2_WEIGHT_KERNELS(task.weights_kernels));

	EMIT(REG_CNA_CBUF_CON0, con0);

	EMIT(REG_CNA_CBUF_CON1,
		CNA_CBUF_CON1_DATA_ENTRIES(task.input_data_entries));

	// The input is signed already and goes in as it is.
	EMIT(REG_CNA_CVT_CON0, CNA_CVT_CON0_DATA_SIGN(1) | CNA_CVT_CON0_CVT_TYPE(1)
		| CNA_CVT_CON0_CVT_BYPASS(1));
	EMIT(REG_CNA_CVT_CON1, CNA_CVT_CON1_CVT_SCALE0(1));
	EMIT(REG_CNA_CVT_CON2, CNA_CVT_CON2_CVT_SCALE1(1));
	EMIT(REG_CNA_CVT_CON3, CNA_CVT_CON3_CVT_SCALE2(1));
	EMIT(REG_CNA_CVT_CON4, CNA_CVT_CON4_CVT_SCALE3(1));

	EMIT(REG_CNA_FC_CON0, 0);
	EMIT(REG_CNA_FC_CON1, 0);
	EMIT(REG_CNA_PAD_CON0, CNA_PAD_CON0_PAD_LEFT(task.pad_left)
		| CNA_PAD_CON0_PAD_TOP(task.pad_top));
	EMIT(REG_CNA_FEATURE_DATA_ADDR, addresses.input + task.input_offset);
	EMIT(REG_CNA_FC_CON2, 0);
	EMIT(REG_CNA_DMA_CON0, CNA_DMA_CON0_WEIGHT_BURST_LEN(15)
		| CNA_DMA_CON0_DATA_BURST_LEN(15));
	EMIT(REG_CNA_DMA_CON1,
		CNA_DMA_CON1_LINE_STRIDE((uint32)task.input_line_stride));
	EMIT(REG_CNA_DMA_CON2,
		CNA_DMA_CON2_SURF_STRIDE((uint32)task.input_surface_stride));

	EMIT(REG_CNA_FC_DATA_SIZE0,
		CNA_FC_DATA_SIZE0_DMA_WIDTH(operation.input_width)
		| CNA_FC_DATA_SIZE0_DMA_HEIGHT(task.input_height));

	EMIT(REG_CNA_FC_DATA_SIZE1,
		CNA_FC_DATA_SIZE1_DMA_CHANNEL(task.input_channels));
	EMIT(REG_CNA_DCOMP_CTRL, 0);
	EMIT(REG_CNA_DCOMP_REGNUM, 0);
	EMIT(REG_CNA_DCOMP_ADDR0, addresses.weights);
	EMIT(REG_CNA_DCOMP_AMOUNT0, 0);
	EMIT(REG_CNA_DCOMP_AMOUNT1, 0);
	EMIT(REG_CNA_DCOMP_AMOUNT2, 0);
	EMIT(REG_CNA_DCOMP_AMOUNT3, 0);
	EMIT(REG_CNA_DCOMP_AMOUNT4, 0);
	EMIT(REG_CNA_DCOMP_AMOUNT5, 0);
	EMIT(REG_CNA_DCOMP_AMOUNT6, 0);
	EMIT(REG_CNA_DCOMP_AMOUNT7, 0);
	EMIT(REG_CNA_DCOMP_AMOUNT8, 0);
	EMIT(REG_CNA_DCOMP_AMOUNT9, 0);
	EMIT(REG_CNA_DCOMP_AMOUNT10, 0);
	EMIT(REG_CNA_DCOMP_AMOUNT11, 0);
	EMIT(REG_CNA_DCOMP_AMOUNT12, 0);
	EMIT(REG_CNA_DCOMP_AMOUNT13, 0);
	EMIT(REG_CNA_DCOMP_AMOUNT14, 0);
	EMIT(REG_CNA_DCOMP_AMOUNT15, 0);

	EMIT(REG_CNA_CVT_CON5, 0);

	// What is outside the input counts as nothing: the zero point. The
	// register holds two of them, for even and odd channels (depthwise
	// convolutions use both), as a signed number of 16 bits.
	uint32 padding = (uint32)operation.input_zero_point & 0xff;
	EMIT(REG_CNA_PAD_CON1,
		(uint32)(int32)(int16)(uint16)(padding | (padding << 8)));

	uint32 miscConfig = CORE_MISC_CFG_QD_EN(1);
	if (operation.depthwise)
		miscConfig |= CORE_MISC_CFG_DW_EN(1);

	EMIT(REG_CORE_MISC_CFG, miscConfig);
	EMIT(REG_CORE_DATAOUT_SIZE_0,
		CORE_DATAOUT_SIZE_0_DATAOUT_HEIGHT(task.output_height - 1)
		| CORE_DATAOUT_SIZE_0_DATAOUT_WIDTH(task.output_width - 1));
	EMIT(REG_CORE_DATAOUT_SIZE_1,
		CORE_DATAOUT_SIZE_1_DATAOUT_CHANNEL(task.output_channels - 1));
	EMIT(REG_CORE_CLIP_TRUNCATE,
		CORE_CLIP_TRUNCATE_CLIP_TRUNCATE(operation.truncate_bits));
	commands.Raw(CORE | 0x1, 0x3030, 0);

	uint32 featureMode = DPU_FEATURE_MODE_CFG_BURST_LEN(15)
		| DPU_FEATURE_MODE_CFG_OUTPUT_MODE(2);
	if (operation.depthwise)
		featureMode |= DPU_FEATURE_MODE_CFG_CONV_MODE(3);

	EMIT(REG_DPU_FEATURE_MODE_CFG, featureMode);
	EMIT(REG_DPU_DATA_FORMAT, 0);
	EMIT(REG_DPU_OFFSET_PEND, 0);
	EMIT(REG_DPU_DST_BASE_ADDR, addresses.output + task.output_offset);
	EMIT(REG_DPU_DST_SURF_STRIDE, DPU_DST_SURF_STRIDE_DST_SURF_STRIDE(
		(uint32)task.output_surface_stride));
	EMIT(REG_DPU_DATA_CUBE_WIDTH,
		DPU_DATA_CUBE_WIDTH_WIDTH(task.output_width - 1));
	EMIT(REG_DPU_DATA_CUBE_HEIGHT,
		DPU_DATA_CUBE_HEIGHT_HEIGHT(task.output_height - 1));
	EMIT(REG_DPU_DATA_CUBE_NOTCH_ADDR, 0);
	EMIT(REG_DPU_DATA_CUBE_CHANNEL,
		DPU_DATA_CUBE_CHANNEL_ORIG_CHANNEL(task.output_channels_real - 1)
		| DPU_DATA_CUBE_CHANNEL_CHANNEL(task.output_channels - 1));
	// The first stage of the unit after the sums: the bias is added, and
	// the result kept above zero and below a limit if there are bounds
	// (the lower bound was taken off the bias for that, see
	// fill_biases(), and is added again by the next stage).
	// It also multiplies by a power of two where the sums are to have
	// bits after the point, see plan_rounding() in Support.cpp; the limit
	// is applied to the product.
	uint32 biasConfig = DPU_BS_CFG_BS_ALU_ALGO(2) | DPU_BS_CFG_BS_ALU_SRC(1);
	if (operation.rounding_bits == 0)
		biasConfig |= DPU_BS_CFG_BS_MUL_BYPASS(1);
	if (!operation.clamp_low)
		biasConfig |= DPU_BS_CFG_BS_RELU_BYPASS(1);
	if (operation.clamp_high)
		biasConfig |= DPU_BS_CFG_BS_RELUX_EN(1);

	EMIT(REG_DPU_BS_CFG, biasConfig);
	EMIT(REG_DPU_BS_ALU_CFG, 0);
	EMIT(REG_DPU_BS_MUL_CFG, operation.rounding_bits != 0
		? DPU_BS_MUL_CFG_BS_MUL_OPERAND(1 << operation.rounding_bits) : 0);
	EMIT(REG_DPU_BS_RELUX_CMP_VALUE, operation.clamp_high
		? (uint32)(operation.clamp_maximum - operation.clamp_minimum)
			<< operation.rounding_bits
		: 0);

	if (operation.depthwise) {
		EMIT(REG_DPU_BS_OW_CFG, DPU_BS_OW_CFG_SIZE_E_2(3)
			| DPU_BS_OW_CFG_SIZE_E_1(3) | DPU_BS_OW_CFG_SIZE_E_0(3));
	} else {
		EMIT(REG_DPU_BS_OW_CFG, DPU_BS_OW_CFG_SIZE_E_2(1)
			| DPU_BS_OW_CFG_SIZE_E_1(1) | DPU_BS_OW_CFG_SIZE_E_0(1));
	}

	// The zero point of the weights: this is added to every one of them.
	EMIT(REG_DPU_BS_OW_OP,
		DPU_BS_OW_OP_OW_OP((uint32)-operation.weights_zero_point));

	EMIT(REG_DPU_WDMA_SIZE_0,
		DPU_WDMA_SIZE_0_CHANNEL_WDMA(task.output_channels - 1));
	EMIT(REG_DPU_WDMA_SIZE_1,
		DPU_WDMA_SIZE_1_HEIGHT_WDMA(task.output_height - 1)
		| DPU_WDMA_SIZE_1_WIDTH_WDMA(task.output_width - 1));
	// The second stage adds the lower bound again, and what makes the
	// rounding come out as the CPU's.
	int32 stageAddend = operation.rounding_offset;
	if (operation.clamp_low) {
		stageAddend += operation.clamp_minimum
			* ((int32)1 << operation.rounding_bits);
	}

	if (stageAddend != 0) {
		EMIT(REG_DPU_BN_CFG, DPU_BN_CFG_BN_ALU_ALGO(2)
			| DPU_BN_CFG_BN_RELU_BYPASS(1) | DPU_BN_CFG_BN_MUL_BYPASS(1));
		EMIT(REG_DPU_BN_ALU_CFG, (uint32)stageAddend);
	} else {
		EMIT(REG_DPU_BN_CFG, DPU_BN_CFG_BN_RELU_BYPASS(1)
			| DPU_BN_CFG_BN_MUL_BYPASS(1) | DPU_BN_CFG_BN_ALU_BYPASS(1)
			| DPU_BN_CFG_BN_BYPASS(1));
		EMIT(REG_DPU_BN_ALU_CFG, 0);
	}
	EMIT(REG_DPU_BN_MUL_CFG, 0);
	EMIT(REG_DPU_BN_RELUX_CMP_VALUE, 0);

	uint32 outputScale = 1;
	uint32 outputShift = 0;
	output_conversion(sumScale / operation.output_scale, &outputScale,
		&outputShift);

	if (addition) {
		// The other operand is brought to the scale of the sums, less its
		// zero point, and added to them before they become outputs. See
		// http://nvdla.org/hw/v1/ias/precision.html#element-wise
		uint32 addendScale = 1;
		uint32 addendShift = 0;
		addend_conversion(operation.addend_scale / sumScale, &addendScale,
			&addendShift);

		EMIT(REG_DPU_EW_CFG, DPU_EW_CFG_EW_CVT_TYPE(1)
			| DPU_EW_CFG_EW_DATA_MODE(1) | DPU_EW_CFG_EDATA_SIZE(1)
			| DPU_EW_CFG_EW_ALU_ALGO(2) | DPU_EW_CFG_EW_RELU_BYPASS(1)
			| DPU_EW_CFG_EW_LUT_BYPASS(1) | DPU_EW_CFG_EW_OP_SRC(1));
		EMIT(REG_DPU_EW_CVT_OFFSET_VALUE,
			(uint32)-operation.addend_zero_point);
		EMIT(REG_DPU_EW_CVT_SCALE_VALUE,
			DPU_EW_CVT_SCALE_VALUE_EW_OP_CVT_SHIFT(addendShift)
			| DPU_EW_CVT_SCALE_VALUE_EW_OP_CVT_SCALE(addendScale));
		EMIT(REG_DPU_EW_RELUX_CMP_VALUE, 0);
	} else {
		EMIT(REG_DPU_EW_CFG, DPU_EW_CFG_EW_RELU_BYPASS(1)
			| DPU_EW_CFG_EW_OP_CVT_BYPASS(1) | DPU_EW_CFG_EW_LUT_BYPASS(1)
			| DPU_EW_CFG_EW_OP_BYPASS(1) | DPU_EW_CFG_EW_BYPASS(1));
		EMIT(REG_DPU_EW_CVT_OFFSET_VALUE, 0);
		EMIT(REG_DPU_EW_CVT_SCALE_VALUE,
			DPU_EW_CVT_SCALE_VALUE_EW_OP_CVT_SCALE(1));
		EMIT(REG_DPU_EW_RELUX_CMP_VALUE, 0);
	}

	EMIT(REG_DPU_OUT_CVT_OFFSET, (uint32)operation.output_zero_point);
	EMIT(REG_DPU_OUT_CVT_SCALE, DPU_OUT_CVT_SCALE_OUT_CVT_SCALE(outputScale));
	EMIT(REG_DPU_OUT_CVT_SHIFT, DPU_OUT_CVT_SHIFT_OUT_CVT_SHIFT(outputShift
		+ operation.rounding_bits));

	EMIT(REG_DPU_EW_OP_VALUE_0, 0);
	EMIT(REG_DPU_EW_OP_VALUE_1, 0);
	EMIT(REG_DPU_EW_OP_VALUE_2, 0);
	EMIT(REG_DPU_EW_OP_VALUE_3, 0);
	EMIT(REG_DPU_EW_OP_VALUE_4, 0);
	EMIT(REG_DPU_EW_OP_VALUE_5, 0);
	EMIT(REG_DPU_EW_OP_VALUE_6, 0);
	EMIT(REG_DPU_EW_OP_VALUE_7, 0);
	EMIT(REG_DPU_SURFACE_ADD, DPU_SURFACE_ADD_SURF_ADD(task.surfaces_per_row));
	commands.Raw(DPU | 0x1, 0x40c4, 0);
	EMIT(REG_DPU_LUT_ACCESS_CFG, 0);
	EMIT(REG_DPU_LUT_ACCESS_DATA, 0);
	EMIT(REG_DPU_LUT_CFG, 0);
	EMIT(REG_DPU_LUT_INFO, 0);
	EMIT(REG_DPU_LUT_LE_START, 0);
	EMIT(REG_DPU_LUT_LE_END, 0);
	EMIT(REG_DPU_LUT_LO_START, 0);
	EMIT(REG_DPU_LUT_LO_END, 0);
	EMIT(REG_DPU_LUT_LE_SLOPE_SCALE, 0);
	EMIT(REG_DPU_LUT_LE_SLOPE_SHIFT, 0);
	EMIT(REG_DPU_LUT_LO_SLOPE_SCALE, 0);
	EMIT(REG_DPU_LUT_LO_SLOPE_SHIFT, 0);
	EMIT(REG_DPU_RDMA_RDMA_DATA_CUBE_WIDTH,
		DPU_RDMA_RDMA_DATA_CUBE_WIDTH_WIDTH(task.output_width - 1));
	EMIT(REG_DPU_RDMA_RDMA_DATA_CUBE_HEIGHT,
		DPU_RDMA_RDMA_DATA_CUBE_HEIGHT_HEIGHT(task.output_height - 1));
	EMIT(REG_DPU_RDMA_RDMA_DATA_CUBE_CHANNEL,
		DPU_RDMA_RDMA_DATA_CUBE_CHANNEL_CHANNEL(task.output_channels - 1));

	if (addition) {
		EMIT(REG_DPU_RDMA_RDMA_SRC_BASE_ADDR,
			addresses.addend + task.output_offset);
	} else
		EMIT(REG_DPU_RDMA_RDMA_SRC_BASE_ADDR, 0);

	EMIT(REG_DPU_RDMA_RDMA_BRDMA_CFG,
		DPU_RDMA_RDMA_BRDMA_CFG_BRDMA_DATA_USE(1));
	EMIT(REG_DPU_RDMA_RDMA_BS_BASE_ADDR, addresses.bias);
	EMIT(REG_DPU_RDMA_RDMA_NRDMA_CFG, 0);
	EMIT(REG_DPU_RDMA_RDMA_BN_BASE_ADDR, 0);

	// The addend is read by two of the unit's readers in turns, a slice
	// of 16 channels each: one starts at the first slice, the other at the
	// second, and both skip a slice after each one they read.
	// (Mesa has at least 12 here, which is wrong for tensors of fewer
	// pixels as this back end lays them out.)
	uint32 addendStride = operation.output_width * operation.output_height;

	if (addition) {
		EMIT(REG_DPU_RDMA_RDMA_ERDMA_CFG,
			DPU_RDMA_RDMA_ERDMA_CFG_ERDMA_DATA_MODE(1)
			| DPU_RDMA_RDMA_ERDMA_CFG_ERDMA_DATA_SIZE(1));
		uint32 addendBaseOffset = operation.output_width
			* operation.output_height * ATOMIC_K_SIZE;
		EMIT(REG_DPU_RDMA_RDMA_EW_BASE_ADDR,
			addresses.addend + task.output_offset + addendBaseOffset);
		EMIT(REG_DPU_RDMA_RDMA_EW_SURF_STRIDE,
			DPU_RDMA_RDMA_EW_SURF_STRIDE_EW_SURF_STRIDE(addendStride));
	} else {
		EMIT(REG_DPU_RDMA_RDMA_ERDMA_CFG,
			DPU_RDMA_RDMA_ERDMA_CFG_ERDMA_DISABLE(1));
		EMIT(REG_DPU_RDMA_RDMA_EW_BASE_ADDR, 0);
		EMIT(REG_DPU_RDMA_RDMA_EW_SURF_STRIDE, 0);
	}

	uint32 readerMode = DPU_RDMA_RDMA_FEATURE_MODE_CFG_BURST_LEN(15);
	if (addition)
		readerMode |= DPU_RDMA_RDMA_FEATURE_MODE_CFG_COMB_USE(5);
	else
		readerMode |= DPU_RDMA_RDMA_FEATURE_MODE_CFG_MRDMA_DISABLE(1);
	if (operation.depthwise)
		readerMode |= DPU_RDMA_RDMA_FEATURE_MODE_CFG_CONV_MODE(3);

	EMIT(REG_DPU_RDMA_RDMA_FEATURE_MODE_CFG, readerMode);
	EMIT(REG_DPU_RDMA_RDMA_SRC_DMA_CFG, 0);

	uint32 surfaceNotch = addendStride + task.output_width
		* (operation.output_height - task.output_height);

	if (addition) {
		EMIT(REG_DPU_RDMA_RDMA_SURF_NOTCH,
			DPU_RDMA_RDMA_SURF_NOTCH_SURF_NOTCH_ADDR(surfaceNotch));
	} else
		EMIT(REG_DPU_RDMA_RDMA_SURF_NOTCH, 0);

	EMIT(REG_DPU_RDMA_RDMA_PAD_CFG, 0);
	EMIT(REG_DPU_RDMA_RDMA_WEIGHT, DPU_RDMA_RDMA_WEIGHT_E_WEIGHT(1)
		| DPU_RDMA_RDMA_WEIGHT_N_WEIGHT(1) | DPU_RDMA_RDMA_WEIGHT_B_WEIGHT(1)
		| DPU_RDMA_RDMA_WEIGHT_M_WEIGHT(1));

	if (addition) {
		EMIT(REG_DPU_RDMA_RDMA_EW_SURF_NOTCH,
			DPU_RDMA_RDMA_EW_SURF_NOTCH_EW_SURF_NOTCH(surfaceNotch));
	} else
		EMIT(REG_DPU_RDMA_RDMA_EW_SURF_NOTCH, 0);

	// Where the next task's list is, for the processor to go on by
	// itself; link_commands() fills it in.
	if (operation.task_count == 1)
		commands.Append(0);
	else
		EMIT(REG_PC_BASE_ADDRESS, 0);

	EMIT(REG_PC_REGISTER_AMOUNTS, 0);

	// The manual: before op_en, 64'h0041_xxxx_xxxx_xxxx must be set.
	commands.Append(0x0041000000000000ull);

	// The manual: 64'h0081_0000_007f_0008 will set each block's op_en.
	commands.Raw(0x81, REG_PC_OPERATION_ENABLE,
		PC_OPERATION_ENABLE_RESERVED_0(14) | PC_OPERATION_ENABLE_OP_EN(1));

	return commands.count;
}


/*!	Puts the place and length of the next task's list into the tail of a
	list. */
void
link_commands(uint64* commands, uint32 count, uint32 nextAddress,
	uint32 nextCount)
{
	// from the end: operation enable, the 0x41 word, amounts, address
	commands[count - 4] |= (uint64)nextAddress << 16;

	uint32 toFetch = round_up((nextCount - 4) / 2, 2);
	commands[count - 3] |= (uint64)toFetch << 16;
}
