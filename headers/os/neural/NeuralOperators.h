/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _NEURAL_OPERATORS_H
#define _NEURAL_OPERATORS_H


/*!	The operators a network is made of, as the kit and its back ends see
	them, whatever file format the network came from. Tensors that hold
	pictures are laid out batch, height, width, channels (NHWC); the
	weights of a convolution are output channels, height, width, input
	channels (depthwise: 1, height, width, output channels).

	Each operator has a fixed list of inputs and, where the table says so,
	a structure of parameters. An input index of -1 means "not given"
	(a convolution without bias).
*/


#include <neural/NeuralDefs.h>


enum neural_operator_code {
	B_NEURAL_OP_NONE = 0,

	// inputs: input, weights, bias; neural_conv_params
	B_NEURAL_OP_CONV_2D = 1,
	B_NEURAL_OP_DEPTHWISE_CONV_2D,
	// inputs: input, weights, bias; neural_fully_connected_params
	B_NEURAL_OP_FULLY_CONNECTED,
	// inputs: output shape (int32), weights, input, bias; neural_conv_params
	B_NEURAL_OP_TRANSPOSE_CONV,

	// inputs: input; neural_pool_params
	B_NEURAL_OP_AVERAGE_POOL_2D,
	B_NEURAL_OP_MAX_POOL_2D,

	// inputs: a, b (broadcast); neural_binary_params
	B_NEURAL_OP_ADD,
	B_NEURAL_OP_SUB,
	B_NEURAL_OP_MUL,
	B_NEURAL_OP_DIV,
	B_NEURAL_OP_MAXIMUM,
	B_NEURAL_OP_MINIMUM,

	// inputs: input; no parameters
	B_NEURAL_OP_RELU,
	B_NEURAL_OP_RELU6,
	B_NEURAL_OP_LOGISTIC,
	B_NEURAL_OP_TANH,
	B_NEURAL_OP_HARD_SWISH,
	// inputs: input; neural_leaky_relu_params
	B_NEURAL_OP_LEAKY_RELU,
	// inputs: input, slope
	B_NEURAL_OP_PRELU,
	// inputs: input; neural_softmax_params
	B_NEURAL_OP_SOFTMAX,

	// inputs: input, shape (int32, optional); neural_reshape_params
	B_NEURAL_OP_RESHAPE,
	// inputs: input; neural_squeeze_params
	B_NEURAL_OP_SQUEEZE,
	// inputs: any number; neural_concat_params
	B_NEURAL_OP_CONCATENATION,
	// inputs: input, paddings (int32 [rank, 2]), constant value (optional)
	B_NEURAL_OP_PAD,
	// inputs: input, axes (int32); neural_reduce_params
	B_NEURAL_OP_MEAN,
	// inputs: input, size (int32 [2]: height, width); neural_resize_params
	B_NEURAL_OP_RESIZE_BILINEAR,
	B_NEURAL_OP_RESIZE_NEAREST,
	// inputs: input, permutation (int32)
	B_NEURAL_OP_TRANSPOSE,
	// inputs: input, begin, end, strides (int32); neural_strided_slice_params
	B_NEURAL_OP_STRIDED_SLICE,

	// inputs: input; converts between types and quantizations
	B_NEURAL_OP_QUANTIZE,
	B_NEURAL_OP_DEQUANTIZE,

	/*!	Turns the raw output of a single-shot detector into a list of
		boxes. Inputs: box encodings [1, anchors, 4], class scores
		[1, anchors, classes], anchors [anchors, 4] (constant). Outputs:
		boxes [1, n, 4] (top, left, bottom, right as fractions of the
		picture), classes [1, n], scores [1, n], count [1], all float.
		neural_detection_params. */
	B_NEURAL_OP_DETECTION_POSTPROCESS,

	// What transformer networks (speech, language) need besides the
	// above.

	// inputs: a, b (broadcast); no parameters: (a - b) squared
	B_NEURAL_OP_SQUARED_DIFFERENCE,
	// inputs: input; no parameters: 1 / square root
	B_NEURAL_OP_RSQRT,
	// inputs: input; neural_gelu_params
	B_NEURAL_OP_GELU,
	/*!	Products of matrices: the last two dimensions of the inputs are
		rows and columns ([..., m, k] times [..., k, n] gives [..., m, n]),
		the dimensions before them say how many matrices there are and
		broadcast. neural_batch_matmul_params. */
	B_NEURAL_OP_BATCH_MATMUL,
	/*!	Picks slices of the input along one dimension. Inputs: input,
		indices (int32 or int64, any shape). The output has the shape of
		the input with that dimension replaced by the shape of the
		indices. An index outside of the dimension fails the run.
		neural_gather_params. */
	B_NEURAL_OP_GATHER,

	B_NEURAL_OP_COUNT
};


//! What is done to the result of an operator before it is stored.
enum neural_activation {
	B_NEURAL_ACTIVATION_NONE = 0,
	B_NEURAL_ACTIVATION_RELU,
	//! clamps to -1 .. 1
	B_NEURAL_ACTIVATION_RELU_N1_TO_1,
	B_NEURAL_ACTIVATION_RELU6,
	B_NEURAL_ACTIVATION_TANH
};


enum neural_padding {
	//! as much padding as it takes to keep size / stride outputs
	B_NEURAL_PADDING_SAME = 0,
	//! no padding: only positions where the whole window fits
	B_NEURAL_PADDING_VALID
};


struct neural_conv_params {
	neural_padding		padding;
	int32				stride_width;
	int32				stride_height;
	int32				dilation_width;
	int32				dilation_height;
	//! depthwise only: output channels per input channel
	int32				depth_multiplier;
	neural_activation	activation;
};

struct neural_fully_connected_params {
	neural_activation	activation;
	//! keep the leading dimensions of the input instead of flattening it
	bool				keep_dimensions;
};

struct neural_pool_params {
	neural_padding		padding;
	int32				stride_width;
	int32				stride_height;
	int32				filter_width;
	int32				filter_height;
	neural_activation	activation;
};

struct neural_binary_params {
	neural_activation	activation;
};

struct neural_leaky_relu_params {
	float				alpha;
};

struct neural_softmax_params {
	float				beta;
};

struct neural_reshape_params {
	//! used when the shape input is not given
	int32				rank;
	int32				shape[B_NEURAL_MAX_RANK];
};

struct neural_squeeze_params {
	//! 0: remove every dimension of size 1
	int32				count;
	int32				dimensions[B_NEURAL_MAX_RANK];
};

struct neural_concat_params {
	int32				axis;
	neural_activation	activation;
};

struct neural_reduce_params {
	bool				keep_dimensions;
};

struct neural_resize_params {
	bool				align_corners;
	bool				half_pixel_centers;
};

struct neural_strided_slice_params {
	int32				begin_mask;
	int32				end_mask;
	int32				ellipsis_mask;
	int32				new_axis_mask;
	int32				shrink_axis_mask;
};

struct neural_gelu_params {
	//! the approximation with tanh instead of the error function
	bool				approximate;
};

struct neural_batch_matmul_params {
	//! use the first (second) input's matrices transposed
	bool				adjoint_a;
	bool				adjoint_b;
};

struct neural_gather_params {
	//! the dimension of the input the indices count along
	int32				axis;
	//! leading dimensions input and indices share; only 0 is supported
	int32				batch_dimensions;
};

struct neural_detection_params {
	int32				max_detections;
	int32				max_classes_per_detection;
	int32				detections_per_class;
	int32				num_classes;
	float				score_threshold;
	float				iou_threshold;
	float				y_scale;
	float				x_scale;
	float				h_scale;
	float				w_scale;
	bool				use_regular_nms;
};


#ifdef __cplusplus
extern "C" {
#endif

//! "CONV_2D", "SOFTMAX", ...; "?" for an unknown code
const char*	neural_operator_name(neural_operator_code code);
//! Size of the parameter structure of an operator, 0 when it has none.
size_t		neural_operator_params_size(neural_operator_code code);

#ifdef __cplusplus
}
#endif


#endif	// _NEURAL_OPERATORS_H
