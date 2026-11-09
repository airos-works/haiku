/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _NEURAL_GRAPH_DATA_H
#define _NEURAL_GRAPH_DATA_H


/*!	The kit's own representation of a network: what a model loader or
	BNeuralModelBuilder fills in, what BNeuralGraph shows to back ends and
	what the CPU interpreter runs. This file and the interpreter depend on
	nothing but the C library, so that both can be built and tested on the
	build host.
*/


#include <add-ons/neural/NeuralBackend.h>


namespace BPrivate {


class NeuralGraphData {
public:
								NeuralGraphData();

			void				AcquireReference();
			void				ReleaseReference();

	// making a graph

			void				SetName(const char* name);
			void				SetDescription(const char* description);
			void				SetFormat(const char* format, int32 version);

	/*!	Memory that lives as long as the graph: for copies of names,
		weights, index lists and parameters. Zeroed, 16 byte aligned. */
			void*				Allocate(size_t size);
			char*				CopyString(const char* string);

	/*!	Keeps a buffer alive as long as the graph, so that tensors may
		point into it: a block from malloc(), or with \a mapped one from
		mmap(). */
			status_t			AdoptBuffer(void* buffer, size_t size,
									bool mapped);

	/*!	Adds a tensor; name, channel scales and data are used as they
		are and must be in memory the graph keeps (see above), the size
		is computed. Returns its index or an error. */
			int32				AddTensor(const neural_graph_tensor& tensor);
	//! The lists and the parameters are copied.
			int32				AddOperator(neural_operator_code code,
									const int32* inputs, int32 inputCount,
									const int32* outputs, int32 outputCount,
									const void* params, size_t paramsSize);
			status_t			AddInput(int32 tensor);
			status_t			AddOutput(int32 tensor);

	/*!	Checks the whole graph: indices, that every tensor is made
		before it is used, operand counts. Must succeed before the graph
		is run or shown to anyone. */
			status_t			Finish();
			bool				IsFinished() const { return fFinished; }

	//! A note on what is wrong with the model, for developers.
			void				SetError(const char* format, ...);
			const char*			Error() const { return fError; }

	// reading it

			const char*			Name() const { return fName; }
			const char*			Description() const
									{ return fDescription; }
			const char*			Format() const { return fFormat; }
			int32				FormatVersion() const
									{ return fFormatVersion; }

			int32				CountTensors() const { return fTensorCount; }
			const neural_graph_tensor& Tensor(int32 index) const
									{ return fTensors[index]; }
			neural_graph_tensor& Tensor(int32 index)
									{ return fTensors[index]; }
			int32				CountOperators() const
									{ return fOperatorCount; }
			const neural_graph_operator& Operator(int32 index) const
									{ return fOperators[index]; }

			int32				CountInputs() const { return fInputCount; }
			int32				InputAt(int32 index) const
									{ return fInputs[index]; }
			int32				CountOutputs() const
									{ return fOutputCount; }
			int32				OutputAt(int32 index) const
									{ return fOutputs[index]; }

	//! Bytes of constant data (weights).
			int64				ConstantBytes() const;

			const BNeuralGraph&	Public() const { return fPublic; }

	static	size_t				TypeSize(neural_data_type type);
	static	int64				CountElements(
									const neural_graph_tensor& tensor);

private:
								~NeuralGraphData();
								NeuralGraphData(const NeuralGraphData&);
			NeuralGraphData&	operator=(const NeuralGraphData&);

			struct Chunk;
			struct Buffer;

			int32				fReferenceCount;
			bool				fFinished;
			char*				fName;
			char*				fDescription;
			char*				fFormat;
			int32				fFormatVersion;
			char				fError[160];

			neural_graph_tensor* fTensors;
			int32				fTensorCount;
			int32				fTensorCapacity;
			neural_graph_operator* fOperators;
			int32				fOperatorCount;
			int32				fOperatorCapacity;
			int32*				fInputs;
			int32				fInputCount;
			int32*				fOutputs;
			int32				fOutputCount;

			Chunk*				fChunks;
			Buffer*				fBuffers;

			BNeuralGraph		fPublic;
};


/*!	Reads a TensorFlow Lite flat buffer. The buffer must stay valid as
	long as the graph (give it to the graph with AdoptBuffer()): the
	weights are not copied unless they are misaligned. */
status_t neural_load_tflite(const void* buffer, size_t size,
	NeuralGraphData* graph);
bool neural_is_tflite(const void* buffer, size_t size);


}	// namespace BPrivate


#endif	// _NEURAL_GRAPH_DATA_H
