/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _NEURAL_KERNELS_H
#define _NEURAL_KERNELS_H


#include "KernelUtils.h"


namespace BPrivate {


// Activation.cpp
extern const NeuralKernel kReluKernel;
extern const NeuralKernel kRelu6Kernel;
extern const NeuralKernel kLogisticKernel;
extern const NeuralKernel kTanhKernel;
extern const NeuralKernel kHardSwishKernel;
extern const NeuralKernel kLeakyReluKernel;
extern const NeuralKernel kRsqrtKernel;
extern const NeuralKernel kGeluKernel;
extern const NeuralKernel kSoftmaxKernel;

// Arithmetic.cpp
extern const NeuralKernel kAddKernel;
extern const NeuralKernel kSubKernel;
extern const NeuralKernel kMulKernel;
extern const NeuralKernel kDivKernel;
extern const NeuralKernel kMaximumKernel;
extern const NeuralKernel kMinimumKernel;
extern const NeuralKernel kPreluKernel;
extern const NeuralKernel kSquaredDifferenceKernel;

// Convolution.cpp
extern const NeuralKernel kConvKernel;
extern const NeuralKernel kDepthwiseConvKernel;
extern const NeuralKernel kFullyConnectedKernel;
extern const NeuralKernel kTransposeConvKernel;

// Detection.cpp
extern const NeuralKernel kDetectionPostprocessKernel;

// Gather.cpp
extern const NeuralKernel kGatherKernel;

// MatMul.cpp
extern const NeuralKernel kBatchMatMulKernel;

// Pooling.cpp
extern const NeuralKernel kAveragePoolKernel;
extern const NeuralKernel kMaxPoolKernel;

// Quantize.cpp
extern const NeuralKernel kQuantizeKernel;
extern const NeuralKernel kDequantizeKernel;

// Reduce.cpp
extern const NeuralKernel kMeanKernel;

// Resize.cpp
extern const NeuralKernel kResizeBilinearKernel;
extern const NeuralKernel kResizeNearestKernel;

// Shape.cpp
extern const NeuralKernel kReshapeKernel;
extern const NeuralKernel kSqueezeKernel;
extern const NeuralKernel kConcatenationKernel;
extern const NeuralKernel kPadKernel;
extern const NeuralKernel kTransposeKernel;
extern const NeuralKernel kStridedSliceKernel;


}	// namespace BPrivate


#endif	// _NEURAL_KERNELS_H
