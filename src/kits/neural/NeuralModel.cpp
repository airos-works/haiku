/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include <neural/NeuralModel.h>

#include <errno.h>
#include <fcntl.h>
#include <new>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <DataIO.h>
#include <Entry.h>
#include <Message.h>
#include <Path.h>

#include <NeuralGraphData.h>
#include <NeuralInterpreter.h>


using BPrivate::NeuralGraphData;
using BPrivate::NeuralInterpreter;


static void
tensor_info(const neural_graph_tensor& tensor, BTensorInfo* info)
{
	info->SetTo(tensor.type, tensor.rank, tensor.dimensions, tensor.name);
	if (tensor.quantized)
		info->SetQuantization(tensor.scale, tensor.zero_point);
}


// #pragma mark - BNeuralModel


BNeuralModel::BNeuralModel()
	:
	fGraph(NULL),
	fStatus(B_NO_INIT)
{
	memset(_reserved, 0, sizeof(_reserved));
}


BNeuralModel::BNeuralModel(const entry_ref* ref)
	:
	fGraph(NULL),
	fStatus(B_NO_INIT)
{
	memset(_reserved, 0, sizeof(_reserved));
	SetTo(ref);
}


BNeuralModel::BNeuralModel(const char* path)
	:
	fGraph(NULL),
	fStatus(B_NO_INIT)
{
	memset(_reserved, 0, sizeof(_reserved));
	SetTo(path);
}


BNeuralModel::BNeuralModel(const BNeuralModel& other)
	:
	fGraph(NULL),
	fStatus(B_NO_INIT)
{
	memset(_reserved, 0, sizeof(_reserved));
	*this = other;
}


BNeuralModel::~BNeuralModel()
{
	Unset();
}


BNeuralModel&
BNeuralModel::operator=(const BNeuralModel& other)
{
	if (this == &other)
		return *this;

	Unset();
	fGraph = other.fGraph;
	if (fGraph != NULL)
		fGraph->AcquireReference();
	fStatus = other.fStatus;
	fError = other.fError;
	return *this;
}


status_t
BNeuralModel::SetTo(const entry_ref* ref)
{
	Unset();
	if (ref == NULL)
		return fStatus = B_BAD_VALUE;

	BPath path(ref);
	status_t status = path.InitCheck();
	if (status != B_OK)
		return fStatus = status;

	return SetTo(path.Path());
}


status_t
BNeuralModel::SetTo(const char* path)
{
	Unset();
	if (path == NULL)
		return fStatus = B_BAD_VALUE;

	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return fStatus = errno;

	struct stat stat;
	if (fstat(fd, &stat) != 0) {
		status_t status = errno;
		close(fd);
		return fStatus = status;
	}
	if (!S_ISREG(stat.st_mode) || stat.st_size < 8) {
		close(fd);
		return fStatus = B_NEURAL_UNKNOWN_FORMAT;
	}

	// Mapped, so that the weights are paged in as they are needed and
	// shared between the teams that use the model.
	void* memory = mmap(NULL, stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	status_t status = memory == MAP_FAILED ? errno : B_OK;
	close(fd);
	if (status != B_OK)
		return fStatus = status;

	const char* leaf = strrchr(path, '/');
	return _SetTo(memory, stat.st_size, true, leaf != NULL ? leaf + 1 : path);
}


status_t
BNeuralModel::SetTo(BDataIO* source)
{
	Unset();
	if (source == NULL)
		return fStatus = B_BAD_VALUE;

	size_t capacity = 1024 * 1024;
	size_t size = 0;
	uint8* buffer = (uint8*)malloc(capacity);
	if (buffer == NULL)
		return fStatus = B_NO_MEMORY;

	while (true) {
		if (size == capacity) {
			capacity *= 2;
			uint8* larger = (uint8*)realloc(buffer, capacity);
			if (larger == NULL) {
				free(buffer);
				return fStatus = B_NO_MEMORY;
			}
			buffer = larger;
		}

		ssize_t bytesRead = source->Read(buffer + size, capacity - size);
		if (bytesRead < 0) {
			free(buffer);
			return fStatus = bytesRead;
		}
		if (bytesRead == 0)
			break;
		size += bytesRead;
	}

	return _SetTo(buffer, size, false, NULL);
}


status_t
BNeuralModel::SetTo(const void* buffer, size_t size)
{
	Unset();
	if (buffer == NULL || size == 0)
		return fStatus = B_BAD_VALUE;

	void* copy = malloc(size);
	if (copy == NULL)
		return fStatus = B_NO_MEMORY;
	memcpy(copy, buffer, size);

	return _SetTo(copy, size, false, NULL);
}


/*!	Takes over the buffer and makes the model of it. */
status_t
BNeuralModel::_SetTo(void* buffer, size_t size, bool mapped,
	const char* fileName)
{
	NeuralGraphData* graph = new(std::nothrow) NeuralGraphData();
	status_t status = graph != NULL ? B_OK : B_NO_MEMORY;
	if (status == B_OK)
		status = graph->AdoptBuffer(buffer, size, mapped);
	if (status != B_OK) {
		if (mapped)
			munmap(buffer, size);
		else
			free(buffer);
		if (graph != NULL)
			graph->ReleaseReference();
		return fStatus = status;
	}

	if (BPrivate::neural_is_tflite(buffer, size))
		status = BPrivate::neural_load_tflite(buffer, size, graph);
	else
		status = B_NEURAL_UNKNOWN_FORMAT;

	if (status != B_OK) {
		fError = graph->Error();
		graph->ReleaseReference();
		return fStatus = status;
	}

	if ((graph->Name() == NULL || graph->Name()[0] == '\0')
		&& fileName != NULL) {
		graph->SetName(fileName);
	}

	fGraph = graph;
	return fStatus = B_OK;
}


void
BNeuralModel::Unset()
{
	if (fGraph != NULL)
		fGraph->ReleaseReference();
	fGraph = NULL;
	fStatus = B_NO_INIT;
	fError = "";
}


status_t
BNeuralModel::InitCheck() const
{
	return fStatus;
}


const char*
BNeuralModel::ErrorDetails() const
{
	return fError.String();
}


const char*
BNeuralModel::Name() const
{
	return fGraph != NULL && fGraph->Name() != NULL ? fGraph->Name() : "";
}


const char*
BNeuralModel::Description() const
{
	return fGraph != NULL && fGraph->Description() != NULL
		? fGraph->Description() : "";
}


const char*
BNeuralModel::Format() const
{
	return fGraph != NULL && fGraph->Format() != NULL ? fGraph->Format() : "";
}


int32
BNeuralModel::CountInputs() const
{
	return fGraph != NULL ? fGraph->CountInputs() : 0;
}


status_t
BNeuralModel::GetInputInfo(int32 index, BTensorInfo* info) const
{
	if (fGraph == NULL)
		return B_NO_INIT;
	if (info == NULL)
		return B_BAD_VALUE;
	if (index < 0 || index >= fGraph->CountInputs())
		return B_BAD_INDEX;

	tensor_info(fGraph->Tensor(fGraph->InputAt(index)), info);
	return B_OK;
}


int32
BNeuralModel::FindInput(const char* name) const
{
	if (fGraph == NULL)
		return B_NO_INIT;
	if (name == NULL)
		return B_BAD_VALUE;

	for (int32 i = 0; i < fGraph->CountInputs(); i++) {
		const char* tensorName = fGraph->Tensor(fGraph->InputAt(i)).name;
		if (tensorName != NULL && strcmp(tensorName, name) == 0)
			return i;
	}
	return B_NAME_NOT_FOUND;
}


int32
BNeuralModel::CountOutputs() const
{
	return fGraph != NULL ? fGraph->CountOutputs() : 0;
}


status_t
BNeuralModel::GetOutputInfo(int32 index, BTensorInfo* info) const
{
	if (fGraph == NULL)
		return B_NO_INIT;
	if (info == NULL)
		return B_BAD_VALUE;
	if (index < 0 || index >= fGraph->CountOutputs())
		return B_BAD_INDEX;

	tensor_info(fGraph->Tensor(fGraph->OutputAt(index)), info);
	return B_OK;
}


int32
BNeuralModel::FindOutput(const char* name) const
{
	if (fGraph == NULL)
		return B_NO_INIT;
	if (name == NULL)
		return B_BAD_VALUE;

	for (int32 i = 0; i < fGraph->CountOutputs(); i++) {
		const char* tensorName = fGraph->Tensor(fGraph->OutputAt(i)).name;
		if (tensorName != NULL && strcmp(tensorName, name) == 0)
			return i;
	}
	return B_NAME_NOT_FOUND;
}


int32
BNeuralModel::CountOperators() const
{
	return fGraph != NULL ? fGraph->CountOperators() : 0;
}


neural_operator_code
BNeuralModel::OperatorAt(int32 index) const
{
	if (fGraph == NULL || index < 0 || index >= fGraph->CountOperators())
		return B_NEURAL_OP_NONE;
	return fGraph->Operator(index).code;
}


status_t
BNeuralModel::GetInfo(BMessage* info) const
{
	if (fGraph == NULL)
		return B_NO_INIT;
	if (info == NULL)
		return B_BAD_VALUE;

	info->MakeEmpty();
	info->AddString("name", Name());
	info->AddString("description", Description());
	info->AddString("format", Format());
	info->AddInt32("version", fGraph->FormatVersion());
	info->AddInt32("operators", fGraph->CountOperators());
	info->AddInt32("tensors", fGraph->CountTensors());
	info->AddInt64("weights:bytes", fGraph->ConstantBytes());

	bool used[B_NEURAL_OP_COUNT];
	memset(used, 0, sizeof(used));
	for (int32 i = 0; i < fGraph->CountOperators(); i++)
		used[fGraph->Operator(i).code] = true;
	for (int32 i = 0; i < B_NEURAL_OP_COUNT; i++) {
		if (used[i]) {
			info->AddString("operator",
				neural_operator_name((neural_operator_code)i));
		}
	}

	return B_OK;
}


/*static*/ bool
BNeuralModel::IsModel(const entry_ref* ref)
{
	if (ref == NULL)
		return false;

	BPath path(ref);
	if (path.InitCheck() != B_OK)
		return false;

	int fd = open(path.Path(), O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;

	uint8 header[8];
	bool isModel = read(fd, header, sizeof(header)) == (ssize_t)sizeof(header)
		&& BPrivate::neural_is_tflite(header, sizeof(header));
	close(fd);
	return isModel;
}


// #pragma mark - BNeuralModelBuilder


BNeuralModelBuilder::BNeuralModelBuilder(const char* name)
	:
	fGraph(new(std::nothrow) NeuralGraphData()),
	fStatus(B_OK)
{
	memset(_reserved, 0, sizeof(_reserved));

	if (fGraph == NULL) {
		fStatus = B_NO_MEMORY;
		return;
	}
	fGraph->SetName(name != NULL ? name : "");
	fGraph->SetFormat("built", 1);
}


BNeuralModelBuilder::~BNeuralModelBuilder()
{
	if (fGraph != NULL)
		fGraph->ReleaseReference();
}


static int32
add_tensor(NeuralGraphData* graph, const BTensorInfo& info, const void* data)
{
	if (!info.IsValid())
		return B_BAD_VALUE;

	neural_graph_tensor tensor;
	memset(&tensor, 0, sizeof(tensor));
	tensor.name = graph->CopyString(info.Name());
	tensor.type = info.Type();
	tensor.rank = info.Rank();
	for (int32 i = 0; i < info.Rank(); i++)
		tensor.dimensions[i] = info.DimensionAt(i);
	tensor.quantized = info.IsQuantized();
	tensor.scale = info.Scale();
	tensor.zero_point = info.ZeroPoint();
	tensor.data = data;
	return graph->AddTensor(tensor);
}


int32
BNeuralModelBuilder::AddInput(const BTensorInfo& info)
{
	if (fStatus != B_OK)
		return fStatus;

	int32 index = add_tensor(fGraph, info, NULL);
	if (index >= 0) {
		status_t status = fGraph->AddInput(index);
		if (status != B_OK)
			index = status;
	}
	if (index < 0)
		fStatus = index;
	return index;
}


int32
BNeuralModelBuilder::AddConstant(const BTensorInfo& info, const void* data,
	size_t size)
{
	if (fStatus != B_OK)
		return fStatus;
	if (data == NULL || !info.IsValid() || size != info.ByteSize())
		return fStatus = B_BAD_VALUE;

	void* copy = fGraph->Allocate(size);
	if (copy == NULL)
		return fStatus = B_NO_MEMORY;
	memcpy(copy, data, size);

	int32 index = add_tensor(fGraph, info, copy);
	if (index < 0)
		fStatus = index;
	return index;
}


status_t
BNeuralModelBuilder::SetChannelQuantization(int32 index, const float* scales,
	int32 count, int32 axis)
{
	if (fStatus != B_OK)
		return fStatus;
	if (index < 0 || index >= fGraph->CountTensors() || scales == NULL)
		return fStatus = B_BAD_VALUE;

	neural_graph_tensor& tensor = fGraph->Tensor(index);
	if (tensor.data == NULL || axis < 0 || axis >= tensor.rank
		|| count != tensor.dimensions[axis]) {
		return fStatus = B_BAD_VALUE;
	}

	float* copy = (float*)fGraph->Allocate(count * sizeof(float));
	if (copy == NULL)
		return fStatus = B_NO_MEMORY;
	memcpy(copy, scales, count * sizeof(float));

	tensor.quantized = true;
	tensor.scale = scales[0];
	tensor.zero_point = 0;
	tensor.channel_scales = copy;
	tensor.channel_count = count;
	tensor.channel_axis = axis;
	return B_OK;
}


int32
BNeuralModelBuilder::AddOperator(neural_operator_code code,
	const int32* inputs, int32 inputCount, const BTensorInfo& output,
	const void* params, size_t paramsSize)
{
	return AddOperator(code, inputs, inputCount, &output, 1, params,
		paramsSize);
}


int32
BNeuralModelBuilder::AddOperator(neural_operator_code code,
	const int32* inputs, int32 inputCount, const BTensorInfo* outputs,
	int32 outputCount, const void* params, size_t paramsSize)
{
	if (fStatus != B_OK)
		return fStatus;
	if (outputs == NULL || outputCount < 1 || outputCount > 16
		|| inputCount < 0 || (inputCount > 0 && inputs == NULL)) {
		return fStatus = B_BAD_VALUE;
	}

	int32 outputIndices[16];
	for (int32 i = 0; i < outputCount; i++) {
		outputIndices[i] = add_tensor(fGraph, outputs[i], NULL);
		if (outputIndices[i] < 0)
			return fStatus = outputIndices[i];
	}

	int32 index = fGraph->AddOperator(code, inputs, inputCount,
		outputIndices, outputCount, params, paramsSize);
	if (index < 0)
		return fStatus = index;

	return outputIndices[0];
}


status_t
BNeuralModelBuilder::MarkOutput(int32 tensor)
{
	if (fStatus != B_OK)
		return fStatus;

	status_t status = fGraph->AddOutput(tensor);
	if (status != B_OK)
		fStatus = status;
	return status;
}


status_t
BNeuralModelBuilder::Build(BNeuralModel* model)
{
	if (model == NULL)
		return B_BAD_VALUE;
	if (fStatus != B_OK)
		return fStatus;

	status_t status = fGraph->Finish();
	if (status != B_OK)
		return status;

	// The kernels know what each operator needs of its operands.
	for (int32 i = 0; i < fGraph->CountOperators(); i++) {
		if (!NeuralInterpreter::Supports(fGraph, i)) {
			fGraph->SetError("operator %d (%s): operands or parameters do "
				"not fit", (int)i,
				neural_operator_name(fGraph->Operator(i).code));
			model->Unset();
			model->fError = fGraph->Error();
			model->fStatus = B_NEURAL_UNSUPPORTED_OPERATOR;
			return B_NEURAL_UNSUPPORTED_OPERATOR;
		}
	}

	model->Unset();
	model->fGraph = fGraph;
	model->fStatus = B_OK;

	// start over
	BString name(fGraph->Name());
	fGraph = new(std::nothrow) NeuralGraphData();
	if (fGraph == NULL) {
		fStatus = B_NO_MEMORY;
	} else {
		fGraph->SetName(name);
		fGraph->SetFormat("built", 1);
	}

	return B_OK;
}
