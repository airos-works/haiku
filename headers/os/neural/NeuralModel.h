/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _NEURAL_MODEL_H
#define _NEURAL_MODEL_H


#include <neural/NeuralDefs.h>
#include <neural/NeuralOperators.h>
#include <neural/Tensor.h>

class BDataIO;
class BMessage;
struct entry_ref;

class BNeuralDevice;
class BNeuralRoster;

namespace BPrivate {
	class NeuralGraphData;
	class NeuralSessionData;
}


/*!	A neural network: its operators, its weights and the description of
	what goes in and comes out. A model is read from a file (TensorFlow
	Lite ".tflite" files are understood by the kit itself) or put together
	with a BNeuralModelBuilder.

	A model cannot be changed once it is made, and copying one is cheap:
	the copies share the network. Files are mapped, not read, so the
	weights of a model used by several teams are in memory once.

	A model does nothing by itself; a BNeuralSession runs it.
*/
class BNeuralModel {
public:
								BNeuralModel();
								BNeuralModel(const entry_ref* ref);
								BNeuralModel(const char* path);
								BNeuralModel(const BNeuralModel& other);
								~BNeuralModel();

			BNeuralModel&		operator=(const BNeuralModel& other);

			status_t			SetTo(const entry_ref* ref);
			status_t			SetTo(const char* path);
	//! Reads the whole model from the stream.
			status_t			SetTo(BDataIO* source);
	//! The buffer is copied.
			status_t			SetTo(const void* buffer, size_t size);
			void				Unset();
			status_t			InitCheck() const;
	/*!	More about why the model could not be read, for developers:
		"operator 126 is not supported". Empty when there is nothing to
		say. */
			const char*			ErrorDetails() const;

	//! The name the model gives itself, or the name of its file.
			const char*			Name() const;
			const char*			Description() const;
	//! "tflite", or "built" for a model from a BNeuralModelBuilder
			const char*			Format() const;

			int32				CountInputs() const;
			status_t			GetInputInfo(int32 index,
									BTensorInfo* info) const;
	//! Index of the input of that name, or a negative error.
			int32				FindInput(const char* name) const;

			int32				CountOutputs() const;
			status_t			GetOutputInfo(int32 index,
									BTensorInfo* info) const;
			int32				FindOutput(const char* name) const;

			int32				CountOperators() const;
			neural_operator_code OperatorAt(int32 index) const;

	/*!	What else is known about the model: "name", "description",
		"format", "version" (int32), "operators" (int32), "weights:bytes"
		(int64) and "operator" (one string per kind of operator used). */
			status_t			GetInfo(BMessage* info) const;

	/*!	Whether a file looks like a model the kit can read, without
		reading it all. */
	static	bool				IsModel(const entry_ref* ref);

private:
	friend class BNeuralDevice;
	friend class BNeuralModelBuilder;
	friend class BNeuralRoster;
	friend class BPrivate::NeuralSessionData;

			status_t			_SetTo(void* buffer, size_t size,
									bool mapped, const char* fileName);

			BPrivate::NeuralGraphData* fGraph;
			status_t			fStatus;
			BString				fError;
			uint32				_reserved[6];
};


/*!	Puts a model together operator by operator, for programs that compute
	their network instead of reading it, and for tests. Tensors are named
	by the numbers the Add...() methods return (negative: an error, which
	is also remembered and returned by Build()).

		BNeuralModelBuilder builder;
		int32 dims[] = { 1, 8 };
		int32 in = builder.AddInput(
			BTensorInfo(B_NEURAL_FLOAT32, 2, dims, "x"));
		int32 out = builder.AddOperator(B_NEURAL_OP_RELU, &in, 1,
			BTensorInfo(B_NEURAL_FLOAT32, 2, dims, "y"));
		builder.MarkOutput(out);
		BNeuralModel model;
		status_t status = builder.Build(&model);
*/
class BNeuralModelBuilder {
public:
								BNeuralModelBuilder(
									const char* name = NULL);
								~BNeuralModelBuilder();

	//! A tensor the caller fills in before each run.
			int32				AddInput(const BTensorInfo& info);
	//! A tensor of fixed numbers (weights); the data is copied.
			int32				AddConstant(const BTensorInfo& info,
									const void* data, size_t size);
	/*!	Per-channel scales for the weights of a quantized convolution
		(the last constant added). */
			status_t			SetChannelQuantization(int32 tensor,
									const float* scales, int32 count,
									int32 axis);

	/*!	Adds an operator and returns its (first) output. The parameter
		structure must be the one NeuralOperators.h names for the code. */
			int32				AddOperator(neural_operator_code code,
									const int32* inputs, int32 inputCount,
									const BTensorInfo& output,
									const void* params = NULL,
									size_t paramsSize = 0);
	//! For operators with several outputs; returns the first one.
			int32				AddOperator(neural_operator_code code,
									const int32* inputs, int32 inputCount,
									const BTensorInfo* outputs,
									int32 outputCount,
									const void* params = NULL,
									size_t paramsSize = 0);

			status_t			MarkOutput(int32 tensor);

	/*!	Checks the network (types, shapes, order) and makes the model;
		the builder is empty afterwards. */
			status_t			Build(BNeuralModel* model);

private:
								BNeuralModelBuilder(
									const BNeuralModelBuilder&);
			BNeuralModelBuilder& operator=(const BNeuralModelBuilder&);

			BPrivate::NeuralGraphData* fGraph;
			status_t			fStatus;
			uint32				_reserved[6];
};


#endif	// _NEURAL_MODEL_H
