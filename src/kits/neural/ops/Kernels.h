/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _NEURAL_KERNELS_H
#define _NEURAL_KERNELS_H


#include "KernelUtils.h"


namespace BPrivate {


// Shape.cpp
extern const NeuralKernel kReshapeKernel;
extern const NeuralKernel kSqueezeKernel;


}	// namespace BPrivate


#endif	// _NEURAL_KERNELS_H
