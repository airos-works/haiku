/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */

/*	What all the shaders share.

	A tensor is an array of vec4: its last dimension (the channels) is
	rounded up to a multiple of four and stored four to an element, all
	the dimensions before it are counted through ("positions"). Element
	c of position p is data[p * groups + c / 4][c % 4], with groups the
	number of vec4 per position. The lanes past the last channel are
	kept at zero, so that whatever is computed with them stays finite.

	One invocation makes one vec4 of the output (the softmax and the mean
	make more). They are numbered along x and y: there may be more of
	them than one dimension of a dispatch holds.
*/

#define LOCAL_SIZE 64

layout(local_size_x = LOCAL_SIZE, local_size_y = 1, local_size_z = 1) in;

#define INVOCATION(rowSize) \
	(gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * (rowSize))

//! Zero for the lanes of a group that are no channels.
vec4
mask_lanes(vec4 value, uint group, uint channels)
{
	return mix(vec4(0.0), value,
		lessThan(group * 4u + uvec4(0u, 1u, 2u, 3u), uvec4(channels)));
}
