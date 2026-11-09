/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef VULKAN_PLAN_H
#define VULKAN_PLAN_H


#include <stdlib.h>

#include <add-ons/neural/NeuralBackend.h>

#include "VulkanDevice.h"


/*!	A list that grows, of things that may be moved around as bytes. No
	exceptions: Add() says when there is no memory. */
template<typename Type>
class PlanList {
public:
	PlanList()
		:
		fItems(NULL),
		fCount(0),
		fCapacity(0)
	{
	}

	~PlanList()
	{
		free(fItems);
	}

	int32 Count() const { return fCount; }
	Type& operator[](int32 index) { return fItems[index]; }
	const Type& operator[](int32 index) const { return fItems[index]; }

	//! The index of the new item, or -1.
	int32 Add(const Type& item)
	{
		if (fCount == fCapacity) {
			int32 capacity = fCapacity == 0 ? 32 : fCapacity * 2;
			Type* items = (Type*)realloc(fItems, capacity * sizeof(Type));
			if (items == NULL)
				return -1;
			fItems = items;
			fCapacity = capacity;
		}
		fItems[fCount] = item;
		return fCount++;
	}

private:
	PlanList(const PlanList&);
	PlanList& operator=(const PlanList&);

	Type*	fItems;
	int32	fCount;
	int32	fCapacity;
};


/*!	Numbers in the device's memory in the layout the shaders work on:
	the last dimension, the channels, rounded up to a multiple of four
	and kept as vec4 ("groups" of them per position), the dimensions
	before it counted through as positions. The lanes past the last
	channel are zero. A value is the memory of a tensor of the network,
	or something in between that the plan needs.
*/
struct PlanValue {
	uint32				positions;
	uint32				channels;
	//! vec4 per position
	uint32				groups;
	/*!	The value whose memory this one is in; itself for most. A
		reshape that keeps the layout only makes another view. */
	int32				root;
	//! the constant with the numbers, or -1
	int32				constant;

	// what Finish() finds out, for roots
	bool				is_input;
	bool				is_output;
	//! the steps that write it first and use it last; -1: before all
	int32				first_step;
	int32				last_step;
	//! where it is in the memory of the tensors
	uint64				offset;

	uint64				Bytes() const
							{ return (uint64)positions * groups * 16; }
};


enum {
	//! a tensor of the network as a value
	PLAN_CONSTANT_TENSOR = 0,
	//! see conv.comp
	PLAN_CONSTANT_CONV_WEIGHTS,
	//! see depthwise.comp
	PLAN_CONSTANT_DEPTHWISE_WEIGHTS,
	//! a vec4 per group of output channels; zeros without a tensor
	PLAN_CONSTANT_BIAS
};


//! Numbers of the network (weights) as the shaders want them.
struct PlanConstant {
	uint32				kind;
	//! the tensor of the graph with the numbers, or -1
	int32				tensor;
	uint32				outputs;
	uint32				kernel_height;
	uint32				kernel_width;
	uint32				inputs;
	//! bytes in the device's layout
	uint64				size;
	//! where it is in the memory of the constants
	uint64				offset;

	//! Writes the \c size bytes.
	void				Fill(const BNeuralGraph& graph,
							float* target) const;
};


enum {
	PLAN_BINDING_NONE = 0,
	PLAN_BINDING_VALUE,
	PLAN_BINDING_CONSTANT
};

struct PlanBinding {
	uint32				kind;
	int32				index;
};


union PlanWord {
	uint32				u;
	int32				i;
	float				f;
};


//! Ways a step may have to do the same thing.
#define PLAN_MAX_VARIANTS	2

/*!	A shader and how many workgroups of it do a step. Some steps can be
	done by more than one, with the same buffers and constants; which is
	the fastest is for the program to find out on the device. */
struct PlanVariant {
	uint32				shader;
	uint32				groups[3];
	//! the first two push constants: "total" and "rowSize"
	uint32				total;
	uint32				row_size;
};


//! One dispatch of a shader.
struct PlanStep {
	//! the operator of the graph it is (part of)
	int32				op;
	//! by binding number: 0 is what the shader writes
	PlanBinding			bindings[VULKAN_BINDING_COUNT];
	uint32				variant_count;
	PlanVariant			variants[PLAN_MAX_VARIANTS];
	//! the push constants: the Params of the shader
	uint32				word_count;
	PlanWord			words[VULKAN_PUSH_CONSTANT_SIZE / 4];
};


//! A tensor that is copied in before or out after the steps.
struct PlanTransfer {
	int32				tensor;
	int32				value;
	//! where it is in the memory the CPU shares with the device
	uint64				offset;
};


/*!	Operators of a network as work for the device: which shaders to run
	on which numbers, and what memory that takes. A plan does nothing with
	the device itself; it is also how the back end finds out whether an
	operator can be run (AddOperator() says no to what the shaders do not
	do), so that the answer cannot differ from what a program is made of.
*/
class VulkanPlan {
public:
								VulkanPlan(const BNeuralGraph& graph,
									const VulkanLimits& limits);
								~VulkanPlan();

			status_t			InitCheck() const { return fStatus; }

	/*!	Adds the steps for an operator. B_NEURAL_UNSUPPORTED_OPERATOR
		when the device cannot run it; the plan is good for nothing but
		more questions then. */
			status_t			AddOperator(int32 index);

	//! Lays out the memory, once all the operators are in.
			status_t			Finish(const neural_partition& partition);

			const BNeuralGraph&	Graph() const { return fGraph; }

			int32				CountSteps() const
									{ return fSteps.Count(); }
			const PlanStep&		StepAt(int32 index) const
									{ return fSteps[index]; }
			int32				CountValues() const
									{ return fValues.Count(); }
			const PlanValue&	ValueAt(int32 index) const
									{ return fValues[index]; }
			int32				CountConstants() const
									{ return fConstants.Count(); }
			const PlanConstant&	ConstantAt(int32 index) const
									{ return fConstants[index]; }
			int32				CountInputs() const
									{ return fInputs.Count(); }
			const PlanTransfer&	InputAt(int32 index) const
									{ return fInputs[index]; }
			int32				CountOutputs() const
									{ return fOutputs.Count(); }
			const PlanTransfer&	OutputAt(int32 index) const
									{ return fOutputs[index]; }

	//! Bytes of device memory for the tensors, with those that share.
			uint64				TensorMemory() const
									{ return fTensorMemory; }
			uint64				ConstantMemory() const
									{ return fConstantMemory; }
	//! Bytes of memory shared with the CPU, for what goes in and out.
			uint64				TransferMemory() const
									{ return fTransferMemory; }

private:
			class Params;

			status_t			_Convolution(
									const neural_graph_operator& op,
									bool depthwise);
			status_t			_FullyConnected(
									const neural_graph_operator& op);
			status_t			_Pool(const neural_graph_operator& op,
									bool average);
			status_t			_Binary(const neural_graph_operator& op,
									uint32 operation);
			status_t			_Unary(const neural_graph_operator& op,
									uint32 operation);
			status_t			_Softmax(const neural_graph_operator& op);
			status_t			_Reshape(const neural_graph_operator& op);
			status_t			_Concatenation(
									const neural_graph_operator& op);
			status_t			_Mean(const neural_graph_operator& op);
			status_t			_Pad(const neural_graph_operator& op);
			status_t			_Resize(const neural_graph_operator& op);

			const neural_graph_tensor* _Tensor(int32 index) const;
			const neural_graph_tensor* _FloatTensor(int32 index) const;
			const neural_graph_tensor* _Picture(int32 index) const;
			bool				_ReadIntegers(int32 index, int32 count,
									int64* values) const;

			status_t			_NewValue(uint32 positions,
									uint32 channels, int32* _value);
			status_t			_InputValue(int32 tensor, int32* _value);
			status_t			_OutputValue(int32 tensor, int32* _value);
			status_t			_View(int32 value, uint32 positions,
									uint32 channels, int32* _view);
			status_t			_AddConstant(const PlanConstant& constant,
									int32* _index);
			status_t			_AddBias(int32 tensor, uint32 outputs,
									int32* _index);
			bool				_SetVariant(PlanVariant& variant,
									uint32 shader, uint32 total) const;
			status_t			_AddVariant(uint32 shader, uint32 total);
			status_t			_AddGridVariant(uint32 shader, uint32 width,
									uint32 height, uint32 depth);
			status_t			_AddStep(uint32 shader, uint32 total,
									const Params& params, int32 output,
									int32 input, int32 second = -1,
									int32 weights = -1, int32 bias = -1);

			const BNeuralGraph&	fGraph;
			VulkanLimits		fLimits;
			status_t			fStatus;
			int32				fOperator;

			//! per tensor of the graph: its value, or -1
			int32*				fTensorValues;

			PlanList<PlanValue>	fValues;
			PlanList<PlanConstant> fConstants;
			PlanList<PlanStep>	fSteps;
			PlanList<PlanTransfer> fInputs;
			PlanList<PlanTransfer> fOutputs;

			uint64				fTensorMemory;
			uint64				fConstantMemory;
			uint64				fTransferMemory;
};


#endif	// VULKAN_PLAN_H
