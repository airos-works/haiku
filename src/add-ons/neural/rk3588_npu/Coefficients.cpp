/*
 * Copyright 2026, air/OS contributors.
 * Copyright (c) 2024 Tomeu Vizoso <tomeu@tomeuvizoso.net>
 * Distributed under the terms of the MIT License.
 */


/*!	Weights and biases as the processor reads them; from Mesa's
	rkt_coefs.c.

	The processor knows no zero point for the input of a convolution. What
	the input's zero point adds to every sum is a constant per output
	channel (the zero point times the sum of that channel's weights), so
	it is taken off the bias instead. The zero point of the weights the
	processor does take care of itself, see fill_commands().
*/


#include "Compiler.h"

#include <string.h>

#include <algorithm>


/*!	The order of the weights of a tensor in the network: output channel,
	row, column, input channel; for depthwise convolutions the first is 1
	and the last is the (output) channel. */
static inline uint32
weight_index(const Operation& operation, uint32 outputChannel, uint32 y,
	uint32 x, uint32 inputChannel)
{
	return ((outputChannel * operation.weights_height + y)
		* operation.weights_width + x) * operation.input_channels
		+ inputChannel;
}


/*!	For how many input channels a kernel position has weights: whole
	groups of 16. (Mesa leaves the last group incomplete for 17 to 31
	channels, which the processor does not take.) Depthwise convolutions
	take their weights 64 channels at a time, and what is left over at
	the end may be one, two or four groups, not three; neither is in
	Mesa, whose networks have no such layers. */
uint32
weights_channels(const Operation& operation)
{
	uint32 channels = round_up(operation.input_channels,
		FEATURE_ATOMIC_SIZE);
	if (operation.depthwise && (channels / FEATURE_ATOMIC_SIZE) % 4 == 3)
		channels += FEATURE_ATOMIC_SIZE;
	return channels;
}


uint32
weights_buffer_size(const Operation& operation)
{
	uint32 inputChannels = weights_channels(operation);
	uint32 outputChannels = operation.depthwise
		? 1 : round_up(operation.output_channels, 2);

	// Twice what is written, as Mesa has it: the processor reads the
	// weights in bursts and may read past their end.
	return operation.weights_width * operation.weights_height * outputChannels
		* round_up(inputChannels, WEIGHT_ATOMIC_SIZE) * 2;
}


void
fill_weights(const Operation& operation, const uint8* weights, uint8* buffer)
{
	uint32 inputChannelsReal = operation.input_channels;
	uint32 outputChannelsReal = operation.output_channels;
	uint32 inputChannels = weights_channels(operation);
	uint32 outputChannels = operation.depthwise
		? 1 : round_up(outputChannelsReal, 2);

	// what is subtracted to make a signed number of a stored one
	uint8 bias = operation.weights_signed ? 0 : 0x80;
	uint8 zeroPoint = (uint8)operation.weights_zero_point;

	uint32 inputChannelGroups = WEIGHT_ATOMIC_SIZE;
	if (operation.depthwise)
		inputChannelGroups *= 2;

	uint32 inputChannels1 = divide_up(inputChannels, inputChannelGroups);
	uint32 inputChannels2 = std::min(inputChannels, inputChannelGroups);
	uint32 outputChannels1 = divide_up(outputChannels, WEIGHT_ATOMIC_SIZE);
	uint32 outputChannels2 = std::min(outputChannels,
		(uint32)WEIGHT_ATOMIC_SIZE);

	uint32 n = 0;
	for (uint32 oc1 = 0; oc1 < outputChannels1; oc1++) {
		for (uint32 ic1 = 0; ic1 < inputChannels1; ic1++) {
			for (uint32 y = 0; y < operation.weights_height; y++) {
				for (uint32 x = 0; x < operation.weights_width; x++) {
					for (uint32 oc2 = 0; oc2 < outputChannels2; oc2++) {
						uint32 oc = oc1 * WEIGHT_ATOMIC_SIZE + oc2;
						if (outputChannelsReal > 2
							&& oc >= round_up(outputChannelsReal, 2)) {
							continue;
						}

						for (uint32 ic2 = 0; ic2 < inputChannels2; ic2++) {
							uint32 ic = ic1 * inputChannelGroups + ic2;

							if (ic >= inputChannels)
								break;

							if (oc >= outputChannelsReal) {
								buffer[n++] = 0;
							} else if (ic >= inputChannelsReal) {
								// A weight of the zero point counts as
								// nothing.
								buffer[n++] = zeroPoint;
							} else {
								buffer[n++] = weights[weight_index(operation,
									oc, y, x, ic)] - bias;
							}
						}
					}
				}
			}
		}
	}
}


/*!	The sum of the weights of an output channel, each less the weights'
	zero point. */
static int32
weights_sum(const Operation& operation, const uint8* weights, uint32 oc)
{
	// zero point as the weights are stored
	int32 zeroPoint = operation.weights_zero_point
		+ (operation.weights_signed ? 0 : 0x80);
	int32 sum = 0;

	for (uint32 y = 0; y < operation.weights_height; y++) {
		for (uint32 x = 0; x < operation.weights_width; x++) {
			if (operation.depthwise) {
				uint8 weight = weights[weight_index(operation, 0, y, x, oc)];
				sum += (operation.weights_signed
					? (int32)(int8)weight : (int32)weight) - zeroPoint;
				continue;
			}

			for (uint32 ic = 0; ic < operation.input_channels; ic++) {
				uint8 weight = weights[weight_index(operation, oc, y, x, ic)];
				sum += (operation.weights_signed
					? (int32)(int8)weight : (int32)weight) - zeroPoint;
			}
		}
	}

	return sum;
}


void
fill_biases(const Operation& operation, const uint8* weights,
	const int32* biases, int32* buffer)
{
	for (uint32 oc = 0; oc < operation.output_channels; oc++) {
		int64 correction = (int64)weights_sum(operation, weights, oc)
			* operation.input_zero_point;
		int64 bias = (biases != NULL ? biases[oc] : 0) - correction;
		buffer[oc] = (int32)(bias / ((int64)1 << operation.truncate_bits));
	}
}
