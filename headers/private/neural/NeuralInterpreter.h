/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _NEURAL_INTERPRETER_H
#define _NEURAL_INTERPRETER_H


/*!	The kit's CPU device: runs operators of a graph one after the other.
	Each kind of operator has a kernel (src/kits/neural/ops); quantized
	kernels compute exactly what the TensorFlow Lite reference kernels
	compute, bit for bit, so that a network gives the same answer whether
	it ran here, on an NPU, or elsewhere.
*/


#include <NeuralGraphData.h>


namespace BPrivate {


/*!	Splits loops over the CPUs. One pool per team, made on first use. */
class NeuralThreadPool {
public:
	typedef void (*Function)(void* cookie, int32 first, int32 end);

	static	NeuralThreadPool*	Default();

			int32				CountThreads() const { return fThreadCount; }

	/*!	Calls the function for stretches of 0 .. count - 1 that cover it
		exactly, on several threads at once when \a count is at least
		twice \a grain (the fewest items worth a thread of their own),
		and returns when all are done. The function must not use the pool
		itself. */
			void				Run(int32 count, int32 grain,
									Function function, void* cookie);

private:
								NeuralThreadPool();
								~NeuralThreadPool();

	static	void				_Create();
	static	void*				_WorkerEntry(void* data);
			void				_Worker();
			bool				_RunSlices();

			struct Impl;
			Impl*				fImpl;
			int32				fThreadCount;
};


struct NeuralOpContext {
	const NeuralGraphData*		graph;
	const neural_graph_operator* op;
	//! memory of every tensor of the graph by index, constants included
	void* const*				tensors;
	//! what the kernel's prepare() made
	void*						prepared;
	NeuralThreadPool*			pool;

	bool HasInput(int32 index) const
	{
		return index < op->input_count && op->inputs[index] >= 0;
	}

	const neural_graph_tensor& InputInfo(int32 index) const
	{
		return graph->Tensor(op->inputs[index]);
	}

	const neural_graph_tensor& OutputInfo(int32 index) const
	{
		return graph->Tensor(op->outputs[index]);
	}

	template<typename Type> const Type* Input(int32 index) const
	{
		return (const Type*)tensors[op->inputs[index]];
	}

	template<typename Type> Type* Output(int32 index) const
	{
		return (Type*)tensors[op->outputs[index]];
	}
};


struct NeuralKernel {
	neural_operator_code code;

	/*!	Says whether the kernel can run the operator as it is (operand
		count, types, shapes, parameters; B_NEURAL_UNSUPPORTED_OPERATOR
		if not) and computes what stays the same from run to run. What it
		returns in \a _prepared is one block from malloc() or NULL; the
		interpreter free()s it. Constant inputs have their data in the
		graph and may be looked at. */
	status_t (*prepare)(const NeuralGraphData* graph,
		const neural_graph_operator* op, void** _prepared);

	status_t (*run)(const NeuralOpContext& context);
};


//! NULL when the kit has no kernel for the operator.
const NeuralKernel* neural_find_kernel(neural_operator_code code);


class NeuralInterpreter {
public:
								NeuralInterpreter(
									const NeuralGraphData* graph);
								~NeuralInterpreter();

	//! Whether the CPU can run the operator.
	static	bool				Supports(const NeuralGraphData* graph,
									int32 op);

	/*!	Gets a list of operators ready (in the order in which they are to
		run). B_NEURAL_UNSUPPORTED_OPERATOR if one cannot be run. */
			status_t			Prepare(const int32* operators, int32 count);

	/*!	Runs the prepared operators. \a tensors: memory per tensor of the
		graph. \a durations, when given, gets the time of each operator
		in microseconds (one entry per prepared operator). */
			status_t			Run(void* const* tensors,
									bigtime_t* durations = NULL);

private:
			struct Step;

			const NeuralGraphData* fGraph;
			Step*				fSteps;
			int32				fStepCount;
};


}	// namespace BPrivate


#endif	// _NEURAL_INTERPRETER_H
