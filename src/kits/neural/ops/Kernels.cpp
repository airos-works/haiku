/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "Kernels.h"


namespace BPrivate {


static const NeuralKernel* const kKernels[] = {
	&kReshapeKernel,
	&kSqueezeKernel
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
