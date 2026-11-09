/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include <NeuralGraphData.h>

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>


namespace BPrivate {


struct NeuralGraphData::Chunk {
	Chunk*	next;
	// the memory follows, 16 byte aligned
};

static const size_t kChunkHeader = 16;

struct NeuralGraphData::Buffer {
	Buffer*	next;
	void*	memory;
	size_t	size;
	bool	mapped;
};


NeuralGraphData::NeuralGraphData()
	:
	fReferenceCount(1),
	fFinished(false),
	fName(NULL),
	fDescription(NULL),
	fFormat(NULL),
	fFormatVersion(0),
	fTensors(NULL),
	fTensorCount(0),
	fTensorCapacity(0),
	fOperators(NULL),
	fOperatorCount(0),
	fOperatorCapacity(0),
	fInputs(NULL),
	fInputCount(0),
	fOutputs(NULL),
	fOutputCount(0),
	fChunks(NULL),
	fBuffers(NULL),
	fPublic(this)
{
}


NeuralGraphData::~NeuralGraphData()
{
	free(fTensors);
	free(fOperators);
	free(fInputs);
	free(fOutputs);

	while (fChunks != NULL) {
		Chunk* next = fChunks->next;
		free(fChunks);
		fChunks = next;
	}

	while (fBuffers != NULL) {
		Buffer* next = fBuffers->next;
		if (fBuffers->mapped)
			munmap(fBuffers->memory, fBuffers->size);
		else
			free(fBuffers->memory);
		free(fBuffers);
		fBuffers = next;
	}
}


void
NeuralGraphData::AcquireReference()
{
	__sync_fetch_and_add(&fReferenceCount, 1);
}


void
NeuralGraphData::ReleaseReference()
{
	if (__sync_sub_and_fetch(&fReferenceCount, 1) == 0)
		delete this;
}


void
NeuralGraphData::SetName(const char* name)
{
	fName = CopyString(name);
}


void
NeuralGraphData::SetDescription(const char* description)
{
	fDescription = CopyString(description);
}


void
NeuralGraphData::SetFormat(const char* format, int32 version)
{
	fFormat = CopyString(format);
	fFormatVersion = version;
}


void*
NeuralGraphData::Allocate(size_t size)
{
	if (size == 0)
		size = 1;

	void* memory;
	if (posix_memalign(&memory, 16, kChunkHeader + size) != 0)
		return NULL;

	Chunk* chunk = (Chunk*)memory;
	chunk->next = fChunks;
	fChunks = chunk;

	void* result = (uint8*)memory + kChunkHeader;
	memset(result, 0, size);
	return result;
}


char*
NeuralGraphData::CopyString(const char* string)
{
	if (string == NULL)
		return NULL;

	size_t length = strlen(string) + 1;
	char* copy = (char*)Allocate(length);
	if (copy != NULL)
		memcpy(copy, string, length);
	return copy;
}


status_t
NeuralGraphData::AdoptBuffer(void* memory, size_t size, bool mapped)
{
	Buffer* buffer = (Buffer*)malloc(sizeof(Buffer));
	if (buffer == NULL)
		return B_NO_MEMORY;

	buffer->memory = memory;
	buffer->size = size;
	buffer->mapped = mapped;
	buffer->next = fBuffers;
	fBuffers = buffer;
	return B_OK;
}


int32
NeuralGraphData::AddTensor(const neural_graph_tensor& tensor)
{
	if (fFinished)
		return B_NOT_ALLOWED;
	if (tensor.rank < 0 || tensor.rank > B_NEURAL_MAX_RANK
		|| TypeSize(tensor.type) == 0) {
		return B_BAD_VALUE;
	}

	int64 elements = 1;
	for (int32 i = 0; i < tensor.rank; i++) {
		if (tensor.dimensions[i] <= 0)
			return B_BAD_VALUE;
		elements *= tensor.dimensions[i];
		if (elements > ((int64)1 << 40))
			return B_BAD_VALUE;
	}

	if (fTensorCount == fTensorCapacity) {
		int32 capacity = fTensorCapacity == 0 ? 64 : fTensorCapacity * 2;
		neural_graph_tensor* tensors = (neural_graph_tensor*)realloc(
			fTensors, capacity * sizeof(neural_graph_tensor));
		if (tensors == NULL)
			return B_NO_MEMORY;
		fTensors = tensors;
		fTensorCapacity = capacity;
	}

	neural_graph_tensor& added = fTensors[fTensorCount];
	added = tensor;
	for (int32 i = tensor.rank; i < B_NEURAL_MAX_RANK; i++)
		added.dimensions[i] = 0;
	added.size = (size_t)elements * TypeSize(tensor.type);
	if (!added.quantized) {
		added.scale = 0;
		added.zero_point = 0;
	}
	if (added.channel_scales == NULL)
		added.channel_count = 0;
	memset(added._reserved, 0, sizeof(added._reserved));

	return fTensorCount++;
}


int32
NeuralGraphData::AddOperator(neural_operator_code code, const int32* inputs,
	int32 inputCount, const int32* outputs, int32 outputCount,
	const void* params, size_t paramsSize)
{
	if (fFinished)
		return B_NOT_ALLOWED;
	if (code <= B_NEURAL_OP_NONE || code >= B_NEURAL_OP_COUNT
		|| inputCount < 0 || outputCount < 1 || inputCount > 1024
		|| outputCount > 1024) {
		return B_BAD_VALUE;
	}

	size_t expected = neural_operator_params_size(code);
	if (expected != 0 && (params == NULL || paramsSize != expected))
		return B_BAD_VALUE;

	if (fOperatorCount == fOperatorCapacity) {
		int32 capacity = fOperatorCapacity == 0 ? 64 : fOperatorCapacity * 2;
		neural_graph_operator* operators = (neural_graph_operator*)realloc(
			fOperators, capacity * sizeof(neural_graph_operator));
		if (operators == NULL)
			return B_NO_MEMORY;
		fOperators = operators;
		fOperatorCapacity = capacity;
	}

	int32* lists = (int32*)Allocate((inputCount + outputCount)
		* sizeof(int32));
	void* paramsCopy = expected != 0 ? Allocate(expected) : NULL;
	if (lists == NULL || (expected != 0 && paramsCopy == NULL))
		return B_NO_MEMORY;

	memcpy(lists, inputs, inputCount * sizeof(int32));
	memcpy(lists + inputCount, outputs, outputCount * sizeof(int32));
	if (paramsCopy != NULL)
		memcpy(paramsCopy, params, expected);

	neural_graph_operator& added = fOperators[fOperatorCount];
	memset(&added, 0, sizeof(added));
	added.code = code;
	added.inputs = lists;
	added.input_count = inputCount;
	added.outputs = lists + inputCount;
	added.output_count = outputCount;
	added.params = paramsCopy;

	return fOperatorCount++;
}


status_t
NeuralGraphData::AddInput(int32 tensor)
{
	if (fFinished)
		return B_NOT_ALLOWED;
	if (tensor < 0 || tensor >= fTensorCount)
		return B_BAD_INDEX;

	int32* inputs = (int32*)realloc(fInputs,
		(fInputCount + 1) * sizeof(int32));
	if (inputs == NULL)
		return B_NO_MEMORY;
	fInputs = inputs;
	fInputs[fInputCount++] = tensor;
	return B_OK;
}


status_t
NeuralGraphData::AddOutput(int32 tensor)
{
	if (fFinished)
		return B_NOT_ALLOWED;
	if (tensor < 0 || tensor >= fTensorCount)
		return B_BAD_INDEX;

	int32* outputs = (int32*)realloc(fOutputs,
		(fOutputCount + 1) * sizeof(int32));
	if (outputs == NULL)
		return B_NO_MEMORY;
	fOutputs = outputs;
	fOutputs[fOutputCount++] = tensor;
	return B_OK;
}


status_t
NeuralGraphData::Finish()
{
	if (fFinished)
		return B_OK;
	if (fOutputCount == 0)
		return B_NEURAL_BAD_MODEL;

	// Every tensor is made exactly once (a constant, a model input or the
	// output of one operator) and before its first use.
	uint8* made = (uint8*)calloc(fTensorCount > 0 ? fTensorCount : 1, 1);
	if (made == NULL)
		return B_NO_MEMORY;

	status_t status = B_OK;

	for (int32 i = 0; i < fTensorCount; i++) {
		if (fTensors[i].data != NULL)
			made[i] = 1;
	}

	for (int32 i = 0; i < fInputCount && status == B_OK; i++) {
		if (made[fInputs[i]] != 0)
			status = B_NEURAL_BAD_MODEL;
		made[fInputs[i]] = 1;
	}

	for (int32 i = 0; i < fOperatorCount && status == B_OK; i++) {
		const neural_graph_operator& op = fOperators[i];
		for (int32 j = 0; j < op.input_count; j++) {
			int32 index = op.inputs[j];
			if (index == -1)
				continue;
			if (index < 0 || index >= fTensorCount || made[index] == 0) {
				status = B_NEURAL_BAD_MODEL;
				break;
			}
		}
		for (int32 j = 0; j < op.output_count && status == B_OK; j++) {
			int32 index = op.outputs[j];
			if (index < 0 || index >= fTensorCount || made[index] != 0)
				status = B_NEURAL_BAD_MODEL;
			else
				made[index] = 1;
		}
	}

	for (int32 i = 0; i < fOutputCount && status == B_OK; i++) {
		if (made[fOutputs[i]] == 0)
			status = B_NEURAL_BAD_MODEL;
	}

	free(made);

	if (status == B_OK)
		fFinished = true;
	return status;
}


int64
NeuralGraphData::ConstantBytes() const
{
	int64 bytes = 0;
	for (int32 i = 0; i < fTensorCount; i++) {
		if (fTensors[i].data != NULL)
			bytes += fTensors[i].size;
	}
	return bytes;
}


/*static*/ size_t
NeuralGraphData::TypeSize(neural_data_type type)
{
	switch (type) {
		case B_NEURAL_FLOAT32:
		case B_NEURAL_INT32:
			return 4;
		case B_NEURAL_FLOAT16:
		case B_NEURAL_INT16:
			return 2;
		case B_NEURAL_INT8:
		case B_NEURAL_UINT8:
		case B_NEURAL_BOOL:
			return 1;
		case B_NEURAL_INT64:
			return 8;
		default:
			return 0;
	}
}


/*static*/ int64
NeuralGraphData::CountElements(const neural_graph_tensor& tensor)
{
	int64 elements = 1;
	for (int32 i = 0; i < tensor.rank; i++)
		elements *= tensor.dimensions[i];
	return elements;
}


}	// namespace BPrivate


// #pragma mark - BNeuralGraph


using BPrivate::NeuralGraphData;


BNeuralGraph::BNeuralGraph(NeuralGraphData* data)
	:
	fData(data)
{
}


BNeuralGraph::~BNeuralGraph()
{
}


const char*
BNeuralGraph::Name() const
{
	return fData->Name() != NULL ? fData->Name() : "";
}


int32
BNeuralGraph::CountTensors() const
{
	return fData->CountTensors();
}


const neural_graph_tensor*
BNeuralGraph::TensorAt(int32 index) const
{
	if (index < 0 || index >= fData->CountTensors())
		return NULL;
	return &fData->Tensor(index);
}


int32
BNeuralGraph::CountOperators() const
{
	return fData->CountOperators();
}


const neural_graph_operator*
BNeuralGraph::OperatorAt(int32 index) const
{
	if (index < 0 || index >= fData->CountOperators())
		return NULL;
	return &fData->Operator(index);
}


int32
BNeuralGraph::CountInputs() const
{
	return fData->CountInputs();
}


int32
BNeuralGraph::InputAt(int32 index) const
{
	if (index < 0 || index >= fData->CountInputs())
		return -1;
	return fData->InputAt(index);
}


int32
BNeuralGraph::CountOutputs() const
{
	return fData->CountOutputs();
}


int32
BNeuralGraph::OutputAt(int32 index) const
{
	if (index < 0 || index >= fData->CountOutputs())
		return -1;
	return fData->OutputAt(index);
}


// #pragma mark - names


size_t
neural_data_type_size(neural_data_type type)
{
	return NeuralGraphData::TypeSize(type);
}


const char*
neural_data_type_name(neural_data_type type)
{
	switch (type) {
		case B_NEURAL_FLOAT32:
			return "float32";
		case B_NEURAL_FLOAT16:
			return "float16";
		case B_NEURAL_INT8:
			return "int8";
		case B_NEURAL_UINT8:
			return "uint8";
		case B_NEURAL_INT16:
			return "int16";
		case B_NEURAL_INT32:
			return "int32";
		case B_NEURAL_INT64:
			return "int64";
		case B_NEURAL_BOOL:
			return "bool";
		default:
			return "?";
	}
}


const char*
neural_device_kind_name(neural_device_kind kind)
{
	switch (kind) {
		case B_NEURAL_DEVICE_CPU:
			return "CPU";
		case B_NEURAL_DEVICE_GPU:
			return "GPU";
		case B_NEURAL_DEVICE_NPU:
			return "NPU";
		default:
			return "?";
	}
}


static const struct {
	neural_operator_code	code;
	const char*				name;
	size_t					params_size;
} kOperators[] = {
	{ B_NEURAL_OP_CONV_2D, "CONV_2D", sizeof(neural_conv_params) },
	{ B_NEURAL_OP_DEPTHWISE_CONV_2D, "DEPTHWISE_CONV_2D",
		sizeof(neural_conv_params) },
	{ B_NEURAL_OP_FULLY_CONNECTED, "FULLY_CONNECTED",
		sizeof(neural_fully_connected_params) },
	{ B_NEURAL_OP_TRANSPOSE_CONV, "TRANSPOSE_CONV",
		sizeof(neural_conv_params) },
	{ B_NEURAL_OP_AVERAGE_POOL_2D, "AVERAGE_POOL_2D",
		sizeof(neural_pool_params) },
	{ B_NEURAL_OP_MAX_POOL_2D, "MAX_POOL_2D", sizeof(neural_pool_params) },
	{ B_NEURAL_OP_ADD, "ADD", sizeof(neural_binary_params) },
	{ B_NEURAL_OP_SUB, "SUB", sizeof(neural_binary_params) },
	{ B_NEURAL_OP_MUL, "MUL", sizeof(neural_binary_params) },
	{ B_NEURAL_OP_DIV, "DIV", sizeof(neural_binary_params) },
	{ B_NEURAL_OP_MAXIMUM, "MAXIMUM", sizeof(neural_binary_params) },
	{ B_NEURAL_OP_MINIMUM, "MINIMUM", sizeof(neural_binary_params) },
	{ B_NEURAL_OP_RELU, "RELU", 0 },
	{ B_NEURAL_OP_RELU6, "RELU6", 0 },
	{ B_NEURAL_OP_LOGISTIC, "LOGISTIC", 0 },
	{ B_NEURAL_OP_TANH, "TANH", 0 },
	{ B_NEURAL_OP_HARD_SWISH, "HARD_SWISH", 0 },
	{ B_NEURAL_OP_LEAKY_RELU, "LEAKY_RELU",
		sizeof(neural_leaky_relu_params) },
	{ B_NEURAL_OP_PRELU, "PRELU", 0 },
	{ B_NEURAL_OP_SOFTMAX, "SOFTMAX", sizeof(neural_softmax_params) },
	{ B_NEURAL_OP_RESHAPE, "RESHAPE", sizeof(neural_reshape_params) },
	{ B_NEURAL_OP_SQUEEZE, "SQUEEZE", sizeof(neural_squeeze_params) },
	{ B_NEURAL_OP_CONCATENATION, "CONCATENATION",
		sizeof(neural_concat_params) },
	{ B_NEURAL_OP_PAD, "PAD", 0 },
	{ B_NEURAL_OP_MEAN, "MEAN", sizeof(neural_reduce_params) },
	{ B_NEURAL_OP_RESIZE_BILINEAR, "RESIZE_BILINEAR",
		sizeof(neural_resize_params) },
	{ B_NEURAL_OP_RESIZE_NEAREST, "RESIZE_NEAREST",
		sizeof(neural_resize_params) },
	{ B_NEURAL_OP_TRANSPOSE, "TRANSPOSE", 0 },
	{ B_NEURAL_OP_STRIDED_SLICE, "STRIDED_SLICE",
		sizeof(neural_strided_slice_params) },
	{ B_NEURAL_OP_QUANTIZE, "QUANTIZE", 0 },
	{ B_NEURAL_OP_DEQUANTIZE, "DEQUANTIZE", 0 },
	{ B_NEURAL_OP_DETECTION_POSTPROCESS, "DETECTION_POSTPROCESS",
		sizeof(neural_detection_params) }
};


const char*
neural_operator_name(neural_operator_code code)
{
	for (size_t i = 0; i < sizeof(kOperators) / sizeof(kOperators[0]); i++) {
		if (kOperators[i].code == code)
			return kOperators[i].name;
	}
	return "?";
}


size_t
neural_operator_params_size(neural_operator_code code)
{
	for (size_t i = 0; i < sizeof(kOperators) / sizeof(kOperators[0]); i++) {
		if (kOperators[i].code == code)
			return kOperators[i].params_size;
	}
	return 0;
}
