/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef RK3588_NPU_SUPPORT_H
#define RK3588_NPU_SUPPORT_H


/*!	What of a network the processor can do, decided in one place for both
	questions the kit asks: "which operators?" and "make the program". The
	code generator can do exactly what these functions let through.
*/


#include <add-ons/neural/NeuralBackend.h>

#include "Compiler.h"


/*!	Who makes and who reads the tensors of a graph. */
class GraphUse {
public:
								GraphUse();
								~GraphUse();

			status_t			Init(const BNeuralGraph& graph);

	//! The operator that makes the tensor; -1: none (an input, a constant).
			int32				Producer(int32 tensor) const
									{ return fProducers[tensor]; }
	//! How many times operators read the tensor.
			int32				CountConsumers(int32 tensor) const
									{ return fConsumers[tensor]; }
			bool				IsGraphOutput(int32 tensor) const
									{ return fOutputs[tensor]; }

private:
			int32*				fProducers;
			int32*				fConsumers;
			bool*				fOutputs;
};


/*!	Describes the convolution that is operator \a index of the graph, or
	returns B_NOT_SUPPORTED if it is not one the processor can do. The
	operation has no tasks yet. */
status_t	describe_convolution(const BNeuralGraph& graph, int32 index,
				Operation& operation);

/*!	Looks at the addition that is operator \a index. The processor adds
	only as the last step of a convolution, so there has to be one that
	makes one of the operands for nobody else. \a inProgram says which
	operators are (so far) known to go into the same program. Returns the
	index of that convolution and the tensor that is the other operand,
	or B_NOT_SUPPORTED. */
status_t	find_addition_target(const BNeuralGraph& graph,
				const GraphUse& use, int32 index, const bool* inProgram,
				int32* _convolution, int32* _addend);

/*!	Makes the convolution do the addition as well; \a operation is what
	describe_convolution() made of the target. B_NOT_SUPPORTED if the
	numbers do not allow it. */
status_t	fuse_addition(const BNeuralGraph& graph, int32 index,
				int32 addend, Operation& operation);

//! Whether the operation can be cut into tasks; tells by trying.
bool		tasks_possible(const Operation& operation);


#endif	// RK3588_NPU_SUPPORT_H
