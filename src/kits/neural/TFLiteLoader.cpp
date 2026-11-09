/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	Reads TensorFlow Lite models (schema version 3) into the kit's graph.
	The file is a FlatBuffer; it is read in place with a small checked
	reader instead of generated code, since a model file comes from
	anywhere and every offset in it has to be verified anyway.
*/


#include <NeuralGraphData.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


namespace BPrivate {


namespace {


// the parts of the TensorFlow Lite schema used here

enum {
	kTensorFloat32 = 0,
	kTensorFloat16 = 1,
	kTensorInt32 = 2,
	kTensorUInt8 = 3,
	kTensorInt64 = 4,
	kTensorBool = 6,
	kTensorInt16 = 7,
	kTensorInt8 = 9
};

enum {
	kOpAdd = 0,
	kOpAveragePool2D = 1,
	kOpConcatenation = 2,
	kOpConv2D = 3,
	kOpDepthwiseConv2D = 4,
	kOpDequantize = 6,
	kOpFullyConnected = 9,
	kOpLogistic = 14,
	kOpMaxPool2D = 17,
	kOpMul = 18,
	kOpRelu = 19,
	kOpRelu6 = 21,
	kOpReshape = 22,
	kOpResizeBilinear = 23,
	kOpSoftmax = 25,
	kOpTanh = 28,
	kOpCustom = 32,
	kOpPad = 34,
	kOpGather = 36,
	kOpTranspose = 39,
	kOpMean = 40,
	kOpSub = 41,
	kOpDiv = 42,
	kOpSqueeze = 43,
	kOpStridedSlice = 45,
	kOpPRelu = 54,
	kOpMaximum = 55,
	kOpMinimum = 57,
	kOpPadV2 = 60,
	kOpTransposeConv = 67,
	kOpRsqrt = 76,
	kOpResizeNearestNeighbor = 97,
	kOpLeakyRelu = 98,
	kOpSquaredDifference = 99,
	kOpQuantize = 114,
	kOpHardSwish = 117,
	kOpBatchMatMul = 126,
	kOpGelu = 150
};

// the largest "deprecated" (8 bit) operator code; above it the 32 bit
// field is used
static const int32 kPlaceholderForGreaterOpCodes = 127;


/*!	A view of a FlatBuffer. Every access is checked against the size of
	the buffer; a failed check sets fBad and yields zeros, so the caller
	checks once at the end of a group of reads.
*/
class FlatReader {
public:
	FlatReader(const void* buffer, size_t size)
		:
		fBuffer((const uint8*)buffer),
		fSize(size),
		fBad(false)
	{
	}

	bool IsBad() const { return fBad; }
	const uint8* Buffer() const { return fBuffer; }
	size_t Size() const { return fSize; }

	bool Check(uint64 position, uint64 length)
	{
		if (position > fSize || length > fSize - position) {
			fBad = true;
			return false;
		}
		return true;
	}

	template<typename Type> Type Read(uint64 position)
	{
		if (!Check(position, sizeof(Type)))
			return 0;
		Type value;
		memcpy(&value, fBuffer + position, sizeof(Type));
		return value;
	}

	//! Position of the root table, 0 for a bad buffer.
	uint32 Root()
	{
		return Indirect(0);
	}

	//! Follows the offset stored at position.
	uint32 Indirect(uint64 position)
	{
		uint32 offset = Read<uint32>(position);
		if (fBad || offset == 0 || !Check(position + offset, 4))
			return 0;
		return position + offset;
	}

	//! Position of a field of a table, 0 when the field is absent.
	uint32 Field(uint32 table, int32 index)
	{
		if (table == 0)
			return 0;
		int32 vtableOffset = Read<int32>(table);
		int64 vtable = (int64)table - vtableOffset;
		if (fBad || vtable < 0 || !Check(vtable, 4)) {
			fBad = true;
			return 0;
		}
		uint16 vtableSize = Read<uint16>(vtable);
		if (vtableSize < 4 || !Check(vtable, vtableSize))
			return 0;
		uint32 slot = 4 + 2 * index;
		if (slot + 2 > vtableSize)
			return 0;
		uint16 fieldOffset = Read<uint16>(vtable + slot);
		if (fieldOffset == 0)
			return 0;
		return table + fieldOffset;
	}

	template<typename Type> Type Scalar(uint32 table, int32 index,
		Type defaultValue)
	{
		uint32 field = Field(table, index);
		if (field == 0)
			return defaultValue;
		return Read<Type>(field);
	}

	//! Position of a table, string or vector a field points to.
	uint32 Offset(uint32 table, int32 index)
	{
		uint32 field = Field(table, index);
		if (field == 0)
			return 0;
		return Indirect(field);
	}

	/*!	Number of elements of a vector (0 for position 0), checked to lie
		within the buffer for the given element size. */
	uint32 VectorLength(uint32 vector, size_t elementSize)
	{
		if (vector == 0)
			return 0;
		uint32 length = Read<uint32>(vector);
		if (!Check((uint64)vector + 4, (uint64)length * elementSize))
			return 0;
		return length;
	}

	//! Position of the first element of a vector.
	uint32 VectorData(uint32 vector)
	{
		return vector + 4;
	}

	//! Position of the table that is element \a index of a vector.
	uint32 VectorTable(uint32 vector, uint32 index)
	{
		return Indirect((uint64)vector + 4 + 4 * (uint64)index);
	}

	//! A string, checked to be terminated; NULL for position 0.
	const char* String(uint32 string)
	{
		if (string == 0)
			return NULL;
		uint32 length = Read<uint32>(string);
		if (!Check((uint64)string + 4, (uint64)length + 1)
			|| fBuffer[string + 4 + length] != '\0') {
			fBad = true;
			return NULL;
		}
		return (const char*)fBuffer + string + 4;
	}

private:
	const uint8*	fBuffer;
	size_t			fSize;
	bool			fBad;
};


/*!	Reads the one kind of FlexBuffer the custom operators use: a map of
	names to numbers at the root.
*/
class FlexMap {
public:
	FlexMap(const uint8* buffer, size_t size)
		:
		fBuffer(buffer),
		fSize(size),
		fMap(0),
		fWidth(0),
		fCount(0),
		fKeys(0),
		fKeyWidth(0)
	{
		if (size < 3)
			return;

		uint8 rootWidth = buffer[size - 1];
		uint8 packed = buffer[size - 2];
		if ((packed >> 2) != kTypeMap || !_ValidWidth(rootWidth)
			|| size < (size_t)rootWidth + 2) {
			return;
		}

		uint64 rootPosition = size - 2 - rootWidth;
		uint64 offset = _ReadUInt(rootPosition, rootWidth);
		if (offset > rootPosition)
			return;

		uint64 map = rootPosition - offset;
		uint8 width = 1 << (packed & 3);
		if (map < 3 * (uint64)width)
			return;

		uint64 count = _ReadUInt(map - width, width);
		uint64 keysField = map - 3 * width;
		uint64 keysOffset = _ReadUInt(keysField, width);
		uint64 keyWidth = _ReadUInt(map - 2 * width, width);
		if (keysOffset > keysField || !_ValidWidth(keyWidth)
			|| count > size || map + count * width + count > size) {
			return;
		}

		uint64 keys = keysField - keysOffset;
		if (keys + count * keyWidth > size)
			return;

		fMap = map;
		fWidth = width;
		fCount = count;
		fKeys = keys;
		fKeyWidth = keyWidth;
	}

	bool IsValid() const { return fWidth != 0; }

	bool Find(const char* name, double* _value) const
	{
		for (uint64 i = 0; i < fCount; i++) {
			uint64 keyField = fKeys + i * fKeyWidth;
			uint64 keyOffset = _ReadUInt(keyField, fKeyWidth);
			if (keyOffset > keyField)
				return false;
			const char* key = (const char*)fBuffer + keyField - keyOffset;
			size_t available = fSize - (keyField - keyOffset);
			size_t length = strnlen(key, available);
			if (length == available || strcmp(key, name) != 0)
				continue;

			return _Value(fMap + i * fWidth, fBuffer[fMap + fCount * fWidth
				+ i], fWidth, _value);
		}
		return false;
	}

	double Get(const char* name, double defaultValue) const
	{
		double value;
		return Find(name, &value) ? value : defaultValue;
	}

private:
	enum {
		kTypeInt = 1,
		kTypeUInt = 2,
		kTypeFloat = 3,
		kTypeIndirectInt = 6,
		kTypeIndirectUInt = 7,
		kTypeIndirectFloat = 8,
		kTypeMap = 9,
		kTypeBool = 26
	};

	static bool _ValidWidth(uint64 width)
	{
		return width == 1 || width == 2 || width == 4 || width == 8;
	}

	uint64 _ReadUInt(uint64 position, uint8 width) const
	{
		if (position + width > fSize)
			return 0;
		uint64 value = 0;
		memcpy(&value, fBuffer + position, width);
			// little endian hosts only, as the rest of the kit
		return value;
	}

	bool _Value(uint64 position, uint8 packed, uint8 parentWidth,
		double* _value) const
	{
		uint8 type = packed >> 2;
		uint8 width = parentWidth;

		if (type == kTypeIndirectInt || type == kTypeIndirectUInt
			|| type == kTypeIndirectFloat) {
			uint64 offset = _ReadUInt(position, parentWidth);
			if (offset > position)
				return false;
			position -= offset;
			width = 1 << (packed & 3);
			type -= kTypeIndirectInt - kTypeInt;
		}

		if (position + width > fSize)
			return false;

		switch (type) {
			case kTypeInt:
			{
				uint64 raw = _ReadUInt(position, width);
				int shift = 64 - 8 * width;
				*_value = (double)((int64)(raw << shift) >> shift);
				return true;
			}
			case kTypeUInt:
			case kTypeBool:
				*_value = (double)_ReadUInt(position, width);
				return true;
			case kTypeFloat:
				if (width == 4) {
					float value;
					memcpy(&value, fBuffer + position, 4);
					*_value = value;
					return true;
				}
				if (width == 8) {
					memcpy(_value, fBuffer + position, 8);
					return true;
				}
				return false;
			default:
				return false;
		}
	}

	const uint8*	fBuffer;
	size_t			fSize;
	uint64			fMap;
	uint8			fWidth;
	uint64			fCount;
	uint64			fKeys;
	uint64			fKeyWidth;
};


class Loader {
public:
	Loader(const void* buffer, size_t size, NeuralGraphData* graph)
		:
		fReader(buffer, size),
		fGraph(graph),
		fModel(0),
		fOperatorCodes(0),
		fOperatorCodeCount(0),
		fBuffers(0),
		fBufferCount(0)
	{
	}

	status_t Load();

private:
	status_t _LoadTensors(uint32 subgraph);
	status_t _LoadOperators(uint32 subgraph);
	status_t _LoadOperator(uint32 op);
	status_t _TensorData(uint32 tensor, neural_data_type type,
		const void** _data, size_t* _size);
	status_t _Indices(uint32 vector, int32** _indices, int32* _count);
	neural_activation _Activation(uint8 value, status_t* _status);
	status_t _FixDetectionOutputs(const int32* outputs, int32 count,
		int32 detections);

	FlatReader			fReader;
	NeuralGraphData*	fGraph;
	uint32				fModel;
	uint32				fOperatorCodes;
	uint32				fOperatorCodeCount;
	uint32				fBuffers;
	uint32				fBufferCount;
};


status_t
Loader::Load()
{
	if (!neural_is_tflite(fReader.Buffer(), fReader.Size()))
		return B_NEURAL_UNKNOWN_FORMAT;

	fModel = fReader.Root();
	if (fModel == 0)
		return B_NEURAL_BAD_MODEL;

	uint32 version = fReader.Scalar<uint32>(fModel, 0, 0);
	if (version != 3)
		return B_NEURAL_UNKNOWN_FORMAT;

	fOperatorCodes = fReader.Offset(fModel, 1);
	fOperatorCodeCount = fReader.VectorLength(fOperatorCodes, 4);
	uint32 subgraphs = fReader.Offset(fModel, 2);
	uint32 subgraphCount = fReader.VectorLength(subgraphs, 4);
	fBuffers = fReader.Offset(fModel, 4);
	fBufferCount = fReader.VectorLength(fBuffers, 4);
	if (fReader.IsBad() || subgraphCount == 0)
		return B_NEURAL_BAD_MODEL;

	fGraph->SetFormat("tflite", version);
	fGraph->SetDescription(fReader.String(fReader.Offset(fModel, 3)));

	// Models with control flow have more than one subgraph; their main
	// graph then has operators (IF, WHILE) that are refused below.
	uint32 subgraph = fReader.VectorTable(subgraphs, 0);
	if (subgraph == 0)
		return B_NEURAL_BAD_MODEL;

	fGraph->SetName(fReader.String(fReader.Offset(subgraph, 4)));

	status_t status = _LoadTensors(subgraph);
	if (status != B_OK)
		return status;

	int32* indices;
	int32 count;
	status = _Indices(fReader.Offset(subgraph, 1), &indices, &count);
	for (int32 i = 0; status == B_OK && i < count; i++)
		status = fGraph->AddInput(indices[i]);
	if (status != B_OK)
		return status;

	status = _LoadOperators(subgraph);
	if (status != B_OK)
		return status;

	status = _Indices(fReader.Offset(subgraph, 2), &indices, &count);
	for (int32 i = 0; status == B_OK && i < count; i++)
		status = fGraph->AddOutput(indices[i]);
	if (status != B_OK)
		return status;

	if (fReader.IsBad())
		return B_NEURAL_BAD_MODEL;

	return fGraph->Finish();
}


/*!	Copies an index vector to memory of the graph (the file's may be
	unaligned), checking nothing but that it is there. */
status_t
Loader::_Indices(uint32 vector, int32** _indices, int32* _count)
{
	uint32 count = fReader.VectorLength(vector, 4);
	if (fReader.IsBad() || count > 65536)
		return B_NEURAL_BAD_MODEL;

	int32* indices = (int32*)fGraph->Allocate(count * sizeof(int32));
	if (indices == NULL)
		return B_NO_MEMORY;
	if (count > 0) {
		memcpy(indices, fReader.Buffer() + fReader.VectorData(vector),
			count * sizeof(int32));
	}

	*_indices = indices;
	*_count = count;
	return B_OK;
}


status_t
Loader::_TensorData(uint32 tensor, neural_data_type type,
	const void** _data, size_t* _size)
{
	*_data = NULL;
	*_size = 0;

	uint32 index = fReader.Scalar<uint32>(tensor, 2, 0);
	if (index == 0)
		return B_OK;
	if (index >= fBufferCount)
		return B_NEURAL_BAD_MODEL;

	uint32 buffer = fReader.VectorTable(fBuffers, index);
	if (buffer == 0)
		return B_NEURAL_BAD_MODEL;

	uint64 position = 0;
	uint64 size = 0;

	uint32 data = fReader.Offset(buffer, 0);
	if (data != 0) {
		size = fReader.VectorLength(data, 1);
		position = fReader.VectorData(data);
	} else {
		// large models keep the numbers after the flat buffer proper
		position = fReader.Scalar<uint64>(buffer, 1, 0);
		size = fReader.Scalar<uint64>(buffer, 2, 0);
		if (position <= 1 || size == 0)
			return B_OK;
		if (!fReader.Check(position, size))
			return B_NEURAL_BAD_MODEL;
	}

	if (fReader.IsBad())
		return B_NEURAL_BAD_MODEL;
	if (size == 0)
		return B_OK;

	const void* memory = fReader.Buffer() + position;
	size_t alignment = NeuralGraphData::TypeSize(type);
	if (((addr_t)memory & (alignment - 1)) != 0) {
		void* copy = fGraph->Allocate(size);
		if (copy == NULL)
			return B_NO_MEMORY;
		memcpy(copy, memory, size);
		memory = copy;
	}

	*_data = memory;
	*_size = size;
	return B_OK;
}


status_t
Loader::_LoadTensors(uint32 subgraph)
{
	uint32 tensors = fReader.Offset(subgraph, 0);
	uint32 count = fReader.VectorLength(tensors, 4);
	if (fReader.IsBad() || count > 1000000)
		return B_NEURAL_BAD_MODEL;

	for (uint32 i = 0; i < count; i++) {
		uint32 tensor = fReader.VectorTable(tensors, i);
		if (tensor == 0)
			return B_NEURAL_BAD_MODEL;

		neural_graph_tensor info;
		memset(&info, 0, sizeof(info));

		switch (fReader.Scalar<int8>(tensor, 1, 0)) {
			case kTensorFloat32:
				info.type = B_NEURAL_FLOAT32;
				break;
			case kTensorFloat16:
				info.type = B_NEURAL_FLOAT16;
				break;
			case kTensorInt32:
				info.type = B_NEURAL_INT32;
				break;
			case kTensorUInt8:
				info.type = B_NEURAL_UINT8;
				break;
			case kTensorInt64:
				info.type = B_NEURAL_INT64;
				break;
			case kTensorBool:
				info.type = B_NEURAL_BOOL;
				break;
			case kTensorInt16:
				info.type = B_NEURAL_INT16;
				break;
			case kTensorInt8:
				info.type = B_NEURAL_INT8;
				break;
			default:
				return B_NEURAL_UNSUPPORTED_OPERATOR;
		}

		// A stateful tensor would have to keep its numbers between runs.
		if (fReader.Scalar<uint8>(tensor, 5, 0) != 0)
			return B_NEURAL_UNSUPPORTED_OPERATOR;

		uint32 shape = fReader.Offset(tensor, 0);
		uint32 rank = fReader.VectorLength(shape, 4);
		if (fReader.IsBad() || rank > B_NEURAL_MAX_RANK)
			return B_NEURAL_BAD_MODEL;

		info.rank = rank;
		bool unknownShape = false;
		for (uint32 j = 0; j < rank; j++) {
			info.dimensions[j] = fReader.Read<int32>(
				fReader.VectorData(shape) + 4 * j);
			if (info.dimensions[j] <= 0)
				unknownShape = true;
		}

		// Shapes that are only known when the model runs are not
		// supported; the one case that is (the outputs of the detection
		// operator) gets its shape when the operator is read. Until then
		// the tensor is a scalar.
		if (unknownShape)
			info.rank = 0;

		info.name = fGraph->CopyString(
			fReader.String(fReader.Offset(tensor, 3)));

		uint32 quantization = fReader.Offset(tensor, 4);
		uint32 scales = fReader.Offset(quantization, 2);
		uint32 scaleCount = fReader.VectorLength(scales, 4);
		uint32 zeroPoints = fReader.Offset(quantization, 3);
		uint32 zeroPointCount = fReader.VectorLength(zeroPoints, 8);
		if (fReader.IsBad())
			return B_NEURAL_BAD_MODEL;

		if (scaleCount > 0) {
			info.quantized = true;
			info.scale = fReader.Read<float>(fReader.VectorData(scales));
			if (zeroPointCount > 0) {
				info.zero_point = (int32)fReader.Read<int64>(
					fReader.VectorData(zeroPoints));
			}

			if (scaleCount > 1) {
				int32 axis = fReader.Scalar<int32>(quantization, 6, 0);
				if (axis < 0 || axis >= info.rank
					|| (int32)scaleCount != info.dimensions[axis]) {
					return B_NEURAL_BAD_MODEL;
				}

				float* copy = (float*)fGraph->Allocate(
					scaleCount * sizeof(float));
				if (copy == NULL)
					return B_NO_MEMORY;
				memcpy(copy, fReader.Buffer() + fReader.VectorData(scales),
					scaleCount * sizeof(float));
				info.channel_scales = copy;
				info.channel_count = scaleCount;
				info.channel_axis = axis;

				// Per-channel weights are symmetric: no zero points.
				for (uint32 j = 0; j < zeroPointCount; j++) {
					if (fReader.Read<int64>(fReader.VectorData(zeroPoints)
							+ 8 * j) != 0) {
						return B_NEURAL_UNSUPPORTED_OPERATOR;
					}
				}
			}
		}

		size_t dataSize;
		status_t status = _TensorData(tensor, info.type, &info.data,
			&dataSize);
		if (status != B_OK)
			return status;

		int32 index = fGraph->AddTensor(info);
		if (index < 0)
			return index == B_NO_MEMORY ? (status_t)B_NO_MEMORY
				: (status_t)B_NEURAL_BAD_MODEL;

		if (info.data != NULL && dataSize != fGraph->Tensor(index).size)
			return B_NEURAL_BAD_MODEL;
	}

	return B_OK;
}


neural_activation
Loader::_Activation(uint8 value, status_t* _status)
{
	switch (value) {
		case 0:
			return B_NEURAL_ACTIVATION_NONE;
		case 1:
			return B_NEURAL_ACTIVATION_RELU;
		case 2:
			return B_NEURAL_ACTIVATION_RELU_N1_TO_1;
		case 3:
			return B_NEURAL_ACTIVATION_RELU6;
		case 4:
			return B_NEURAL_ACTIVATION_TANH;
		default:
			*_status = B_NEURAL_UNSUPPORTED_OPERATOR;
			return B_NEURAL_ACTIVATION_NONE;
	}
}


status_t
Loader::_LoadOperators(uint32 subgraph)
{
	uint32 operators = fReader.Offset(subgraph, 3);
	uint32 count = fReader.VectorLength(operators, 4);
	if (fReader.IsBad() || count > 1000000)
		return B_NEURAL_BAD_MODEL;

	for (uint32 i = 0; i < count; i++) {
		uint32 op = fReader.VectorTable(operators, i);
		if (op == 0)
			return B_NEURAL_BAD_MODEL;

		status_t status = _LoadOperator(op);
		if (status != B_OK)
			return status;
	}

	return B_OK;
}


/*!	The detection operator's outputs have no shape in the file. */
status_t
Loader::_FixDetectionOutputs(const int32* outputs, int32 count,
	int32 detections)
{
	if (count != 4 || detections <= 0 || detections > 65536)
		return B_NEURAL_BAD_MODEL;

	static const int32 kRanks[4] = { 3, 2, 2, 1 };
	for (int32 i = 0; i < 4; i++) {
		if (outputs[i] < 0 || outputs[i] >= fGraph->CountTensors())
			return B_NEURAL_BAD_MODEL;

		neural_graph_tensor& tensor = fGraph->Tensor(outputs[i]);
		if (tensor.type != B_NEURAL_FLOAT32 || tensor.data != NULL)
			return B_NEURAL_BAD_MODEL;

		tensor.rank = kRanks[i];
		tensor.dimensions[0] = 1;
		tensor.dimensions[1] = detections;
		tensor.dimensions[2] = 4;
		for (int32 j = tensor.rank; j < B_NEURAL_MAX_RANK; j++)
			tensor.dimensions[j] = 0;
		tensor.size = NeuralGraphData::CountElements(tensor) * sizeof(float);
	}

	return B_OK;
}


status_t
Loader::_LoadOperator(uint32 op)
{
	uint32 codeIndex = fReader.Scalar<uint32>(op, 0, 0);
	if (codeIndex >= fOperatorCodeCount)
		return B_NEURAL_BAD_MODEL;

	uint32 code = fReader.VectorTable(fOperatorCodes, codeIndex);
	if (code == 0)
		return B_NEURAL_BAD_MODEL;

	int32 builtin = fReader.Scalar<int8>(code, 0, 0);
	if (builtin >= kPlaceholderForGreaterOpCodes || builtin == 0) {
		int32 extended = fReader.Scalar<int32>(code, 3, 0);
		if (extended != 0)
			builtin = extended;
	}

	int32* inputs;
	int32 inputCount;
	status_t status = _Indices(fReader.Offset(op, 1), &inputs, &inputCount);
	if (status != B_OK)
		return status;

	int32* outputs;
	int32 outputCount;
	status = _Indices(fReader.Offset(op, 2), &outputs, &outputCount);
	if (status != B_OK)
		return status;

	uint32 options = fReader.Offset(op, 4);
		// a table of the type the operator code implies; its type field
		// is not needed

	neural_operator_code kitCode = B_NEURAL_OP_NONE;
	const void* params = NULL;
	size_t paramsSize = 0;

	neural_conv_params conv;
	neural_fully_connected_params fullyConnected;
	neural_pool_params pool;
	neural_binary_params binary;
	neural_leaky_relu_params leakyRelu;
	neural_softmax_params softmax;
	neural_reshape_params reshape;
	neural_squeeze_params squeeze;
	neural_concat_params concat;
	neural_reduce_params reduce;
	neural_resize_params resize;
	neural_strided_slice_params stridedSlice;
	neural_detection_params detection;
	neural_gelu_params gelu;
	neural_batch_matmul_params matmul;
	neural_gather_params gather;

	switch (builtin) {
		case kOpConv2D:
			memset(&conv, 0, sizeof(conv));
			conv.padding = fReader.Scalar<int8>(options, 0, 0) == 0
				? B_NEURAL_PADDING_SAME : B_NEURAL_PADDING_VALID;
			conv.stride_width = fReader.Scalar<int32>(options, 1, 0);
			conv.stride_height = fReader.Scalar<int32>(options, 2, 0);
			conv.activation = _Activation(
				fReader.Scalar<uint8>(options, 3, 0), &status);
			conv.dilation_width = fReader.Scalar<int32>(options, 4, 1);
			conv.dilation_height = fReader.Scalar<int32>(options, 5, 1);
			conv.depth_multiplier = 1;
			kitCode = B_NEURAL_OP_CONV_2D;
			params = &conv;
			paramsSize = sizeof(conv);
			break;

		case kOpDepthwiseConv2D:
			memset(&conv, 0, sizeof(conv));
			conv.padding = fReader.Scalar<int8>(options, 0, 0) == 0
				? B_NEURAL_PADDING_SAME : B_NEURAL_PADDING_VALID;
			conv.stride_width = fReader.Scalar<int32>(options, 1, 0);
			conv.stride_height = fReader.Scalar<int32>(options, 2, 0);
			conv.depth_multiplier = fReader.Scalar<int32>(options, 3, 0);
			conv.activation = _Activation(
				fReader.Scalar<uint8>(options, 4, 0), &status);
			conv.dilation_width = fReader.Scalar<int32>(options, 5, 1);
			conv.dilation_height = fReader.Scalar<int32>(options, 6, 1);
			kitCode = B_NEURAL_OP_DEPTHWISE_CONV_2D;
			params = &conv;
			paramsSize = sizeof(conv);
			break;

		case kOpTransposeConv:
			memset(&conv, 0, sizeof(conv));
			conv.padding = fReader.Scalar<int8>(options, 0, 0) == 0
				? B_NEURAL_PADDING_SAME : B_NEURAL_PADDING_VALID;
			conv.stride_width = fReader.Scalar<int32>(options, 1, 0);
			conv.stride_height = fReader.Scalar<int32>(options, 2, 0);
			conv.activation = _Activation(
				fReader.Scalar<uint8>(options, 3, 0), &status);
			conv.dilation_width = 1;
			conv.dilation_height = 1;
			conv.depth_multiplier = 1;
			kitCode = B_NEURAL_OP_TRANSPOSE_CONV;
			params = &conv;
			paramsSize = sizeof(conv);
			break;

		case kOpFullyConnected:
			memset(&fullyConnected, 0, sizeof(fullyConnected));
			fullyConnected.activation = _Activation(
				fReader.Scalar<uint8>(options, 0, 0), &status);
			if (fReader.Scalar<int8>(options, 1, 0) != 0) {
				// shuffled 4x16 weights
				status = B_NEURAL_UNSUPPORTED_OPERATOR;
			}
			fullyConnected.keep_dimensions
				= fReader.Scalar<uint8>(options, 2, 0) != 0;
			kitCode = B_NEURAL_OP_FULLY_CONNECTED;
			params = &fullyConnected;
			paramsSize = sizeof(fullyConnected);
			break;

		case kOpAveragePool2D:
		case kOpMaxPool2D:
			memset(&pool, 0, sizeof(pool));
			pool.padding = fReader.Scalar<int8>(options, 0, 0) == 0
				? B_NEURAL_PADDING_SAME : B_NEURAL_PADDING_VALID;
			pool.stride_width = fReader.Scalar<int32>(options, 1, 0);
			pool.stride_height = fReader.Scalar<int32>(options, 2, 0);
			pool.filter_width = fReader.Scalar<int32>(options, 3, 0);
			pool.filter_height = fReader.Scalar<int32>(options, 4, 0);
			pool.activation = _Activation(
				fReader.Scalar<uint8>(options, 5, 0), &status);
			kitCode = builtin == kOpAveragePool2D
				? B_NEURAL_OP_AVERAGE_POOL_2D : B_NEURAL_OP_MAX_POOL_2D;
			params = &pool;
			paramsSize = sizeof(pool);
			break;

		case kOpAdd:
		case kOpSub:
		case kOpMul:
		case kOpDiv:
		case kOpMaximum:
		case kOpMinimum:
			memset(&binary, 0, sizeof(binary));
			if (builtin != kOpMaximum && builtin != kOpMinimum) {
				binary.activation = _Activation(
					fReader.Scalar<uint8>(options, 0, 0), &status);
			}
			switch (builtin) {
				case kOpAdd:
					kitCode = B_NEURAL_OP_ADD;
					break;
				case kOpSub:
					kitCode = B_NEURAL_OP_SUB;
					break;
				case kOpMul:
					kitCode = B_NEURAL_OP_MUL;
					break;
				case kOpDiv:
					kitCode = B_NEURAL_OP_DIV;
					break;
				case kOpMaximum:
					kitCode = B_NEURAL_OP_MAXIMUM;
					break;
				default:
					kitCode = B_NEURAL_OP_MINIMUM;
					break;
			}
			params = &binary;
			paramsSize = sizeof(binary);
			break;

		case kOpRelu:
			kitCode = B_NEURAL_OP_RELU;
			break;
		case kOpRelu6:
			kitCode = B_NEURAL_OP_RELU6;
			break;
		case kOpLogistic:
			kitCode = B_NEURAL_OP_LOGISTIC;
			break;
		case kOpTanh:
			kitCode = B_NEURAL_OP_TANH;
			break;
		case kOpHardSwish:
			kitCode = B_NEURAL_OP_HARD_SWISH;
			break;
		case kOpPRelu:
			kitCode = B_NEURAL_OP_PRELU;
			break;
		case kOpPad:
		case kOpPadV2:
			kitCode = B_NEURAL_OP_PAD;
			break;
		case kOpTranspose:
			kitCode = B_NEURAL_OP_TRANSPOSE;
			break;
		case kOpQuantize:
			kitCode = B_NEURAL_OP_QUANTIZE;
			break;
		case kOpDequantize:
			kitCode = B_NEURAL_OP_DEQUANTIZE;
			break;
		case kOpSquaredDifference:
			kitCode = B_NEURAL_OP_SQUARED_DIFFERENCE;
			break;
		case kOpRsqrt:
			kitCode = B_NEURAL_OP_RSQRT;
			break;

		case kOpGelu:
			memset(&gelu, 0, sizeof(gelu));
			gelu.approximate = fReader.Scalar<uint8>(options, 0, 0) != 0;
			kitCode = B_NEURAL_OP_GELU;
			params = &gelu;
			paramsSize = sizeof(gelu);
			break;

		case kOpBatchMatMul:
			memset(&matmul, 0, sizeof(matmul));
			matmul.adjoint_a = fReader.Scalar<uint8>(options, 0, 0) != 0;
			matmul.adjoint_b = fReader.Scalar<uint8>(options, 1, 0) != 0;
			kitCode = B_NEURAL_OP_BATCH_MATMUL;
			params = &matmul;
			paramsSize = sizeof(matmul);
			break;

		case kOpGather:
			memset(&gather, 0, sizeof(gather));
			gather.axis = fReader.Scalar<int32>(options, 0, 0);
			gather.batch_dimensions = fReader.Scalar<int32>(options, 1, 0);
			kitCode = B_NEURAL_OP_GATHER;
			params = &gather;
			paramsSize = sizeof(gather);
			break;

		case kOpLeakyRelu:
			leakyRelu.alpha = fReader.Scalar<float>(options, 0, 0);
			kitCode = B_NEURAL_OP_LEAKY_RELU;
			params = &leakyRelu;
			paramsSize = sizeof(leakyRelu);
			break;

		case kOpSoftmax:
			softmax.beta = fReader.Scalar<float>(options, 0, 0);
			kitCode = B_NEURAL_OP_SOFTMAX;
			params = &softmax;
			paramsSize = sizeof(softmax);
			break;

		case kOpReshape:
		{
			memset(&reshape, 0, sizeof(reshape));
			uint32 shape = fReader.Offset(options, 0);
			uint32 rank = fReader.VectorLength(shape, 4);
			if (rank <= B_NEURAL_MAX_RANK) {
				reshape.rank = rank;
				for (uint32 i = 0; i < rank; i++) {
					reshape.shape[i] = fReader.Read<int32>(
						fReader.VectorData(shape) + 4 * i);
				}
			}
			kitCode = B_NEURAL_OP_RESHAPE;
			params = &reshape;
			paramsSize = sizeof(reshape);
			break;
		}

		case kOpSqueeze:
		{
			memset(&squeeze, 0, sizeof(squeeze));
			uint32 dimensions = fReader.Offset(options, 0);
			uint32 count = fReader.VectorLength(dimensions, 4);
			if (count > B_NEURAL_MAX_RANK)
				return B_NEURAL_BAD_MODEL;
			squeeze.count = count;
			for (uint32 i = 0; i < count; i++) {
				squeeze.dimensions[i] = fReader.Read<int32>(
					fReader.VectorData(dimensions) + 4 * i);
			}
			kitCode = B_NEURAL_OP_SQUEEZE;
			params = &squeeze;
			paramsSize = sizeof(squeeze);
			break;
		}

		case kOpConcatenation:
			memset(&concat, 0, sizeof(concat));
			concat.axis = fReader.Scalar<int32>(options, 0, 0);
			concat.activation = _Activation(
				fReader.Scalar<uint8>(options, 1, 0), &status);
			kitCode = B_NEURAL_OP_CONCATENATION;
			params = &concat;
			paramsSize = sizeof(concat);
			break;

		case kOpMean:
			memset(&reduce, 0, sizeof(reduce));
			reduce.keep_dimensions
				= fReader.Scalar<uint8>(options, 0, 0) != 0;
			kitCode = B_NEURAL_OP_MEAN;
			params = &reduce;
			paramsSize = sizeof(reduce);
			break;

		case kOpResizeBilinear:
			memset(&resize, 0, sizeof(resize));
			resize.align_corners = fReader.Scalar<uint8>(options, 2, 0) != 0;
			resize.half_pixel_centers
				= fReader.Scalar<uint8>(options, 3, 0) != 0;
			kitCode = B_NEURAL_OP_RESIZE_BILINEAR;
			params = &resize;
			paramsSize = sizeof(resize);
			break;

		case kOpResizeNearestNeighbor:
			memset(&resize, 0, sizeof(resize));
			resize.align_corners = fReader.Scalar<uint8>(options, 0, 0) != 0;
			resize.half_pixel_centers
				= fReader.Scalar<uint8>(options, 1, 0) != 0;
			kitCode = B_NEURAL_OP_RESIZE_NEAREST;
			params = &resize;
			paramsSize = sizeof(resize);
			break;

		case kOpStridedSlice:
			memset(&stridedSlice, 0, sizeof(stridedSlice));
			stridedSlice.begin_mask = fReader.Scalar<int32>(options, 0, 0);
			stridedSlice.end_mask = fReader.Scalar<int32>(options, 1, 0);
			stridedSlice.ellipsis_mask
				= fReader.Scalar<int32>(options, 2, 0);
			stridedSlice.new_axis_mask
				= fReader.Scalar<int32>(options, 3, 0);
			stridedSlice.shrink_axis_mask
				= fReader.Scalar<int32>(options, 4, 0);
			if (fReader.Scalar<uint8>(options, 5, 0) != 0)
				status = B_NEURAL_UNSUPPORTED_OPERATOR;
			kitCode = B_NEURAL_OP_STRIDED_SLICE;
			params = &stridedSlice;
			paramsSize = sizeof(stridedSlice);
			break;

		case kOpCustom:
		{
			const char* name = fReader.String(fReader.Offset(code, 1));
			if (name == NULL
				|| strcmp(name, "TFLite_Detection_PostProcess") != 0) {
				fGraph->SetError("custom operator \"%s\" is not supported",
					name != NULL ? name : "");
				return B_NEURAL_UNSUPPORTED_OPERATOR;
			}

			uint32 custom = fReader.Offset(op, 5);
			uint32 customSize = fReader.VectorLength(custom, 1);
			if (fReader.IsBad() || customSize == 0)
				return B_NEURAL_BAD_MODEL;

			FlexMap map(fReader.Buffer() + fReader.VectorData(custom),
				customSize);
			if (!map.IsValid())
				return B_NEURAL_BAD_MODEL;

			memset(&detection, 0, sizeof(detection));
			detection.max_detections = (int32)map.Get("max_detections", 0);
			detection.max_classes_per_detection
				= (int32)map.Get("max_classes_per_detection", 1);
			detection.detections_per_class
				= (int32)map.Get("detections_per_class", 100);
			detection.num_classes = (int32)map.Get("num_classes", 0);
			detection.score_threshold
				= (float)map.Get("nms_score_threshold", 0);
			detection.iou_threshold
				= (float)map.Get("nms_iou_threshold", 0);
			detection.y_scale = (float)map.Get("y_scale", 1);
			detection.x_scale = (float)map.Get("x_scale", 1);
			detection.h_scale = (float)map.Get("h_scale", 1);
			detection.w_scale = (float)map.Get("w_scale", 1);
			detection.use_regular_nms
				= map.Get("use_regular_nms", 0) != 0;

			status = _FixDetectionOutputs(outputs, outputCount,
				detection.max_detections
					* detection.max_classes_per_detection);
			if (status != B_OK)
				return status;

			kitCode = B_NEURAL_OP_DETECTION_POSTPROCESS;
			params = &detection;
			paramsSize = sizeof(detection);
			break;
		}

		default:
			fGraph->SetError("TensorFlow Lite operator %d is not supported",
				(int)builtin);
			return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	if (status != B_OK) {
		fGraph->SetError("an option of TensorFlow Lite operator %d is not "
			"supported", (int)builtin);
		return status;
	}
	if (fReader.IsBad())
		return B_NEURAL_BAD_MODEL;

	int32 index = fGraph->AddOperator(kitCode, inputs, inputCount, outputs,
		outputCount, params, paramsSize);
	if (index < 0)
		return index == B_NO_MEMORY ? (status_t)B_NO_MEMORY
				: (status_t)B_NEURAL_BAD_MODEL;

	return B_OK;
}


}	// unnamed namespace


bool
neural_is_tflite(const void* buffer, size_t size)
{
	return buffer != NULL && size >= 8
		&& memcmp((const uint8*)buffer + 4, "TFL3", 4) == 0;
}


status_t
neural_load_tflite(const void* buffer, size_t size, NeuralGraphData* graph)
{
	Loader loader(buffer, size, graph);
	return loader.Load();
}


}	// namespace BPrivate
