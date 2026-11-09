/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "Kernels.h"


namespace BPrivate {


static const NeuralKernel* const kKernels[] = {
	&kConvKernel,
	&kDepthwiseConvKernel,
	&kFullyConnectedKernel,
	&kTransposeConvKernel,
	&kAveragePoolKernel,
	&kMaxPoolKernel,
	&kAddKernel,
	&kSubKernel,
	&kMulKernel,
	&kDivKernel,
	&kMaximumKernel,
	&kMinimumKernel,
	&kReluKernel,
	&kRelu6Kernel,
	&kLogisticKernel,
	&kTanhKernel,
	&kHardSwishKernel,
	&kLeakyReluKernel,
	&kPreluKernel,
	&kSoftmaxKernel,
	&kReshapeKernel,
	&kSqueezeKernel,
	&kConcatenationKernel,
	&kPadKernel,
	&kMeanKernel,
	&kResizeBilinearKernel,
	&kResizeNearestKernel,
	&kTransposeKernel,
	&kStridedSliceKernel,
	&kQuantizeKernel,
	&kDequantizeKernel,
	&kDetectionPostprocessKernel,
	&kSquaredDifferenceKernel,
	&kRsqrtKernel,
	&kGeluKernel,
	&kBatchMatMulKernel,
	&kGatherKernel
};


const NeuralKernel*
neural_find_kernel(neural_operator_code code)
{
	for (size_t i = 0; i < sizeof(kKernels) / sizeof(kKernels[0]); i++) {
		if (kKernels[i]->code == code)
			return kKernels[i];
	}
	return NULL;
}


}	// namespace BPrivate
