/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	Design bundles: their keys, their manifests, where they are, and how
	tensors lie in a design's buffers. README has the formats; what is
	computed here and by the host's tool (make-bundle.py in the linux-drm
	repository) has to stay the same.
*/


#include "Bundle.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <FindDirectory.h>
#include <PathFinder.h>
#include <StringList.h>

#include "Sha256.h"


static const size_t kMaxFileSize = 64 << 20;


// #pragma mark - keys


/*!	The parameters of an operator as the key has them: the fields in the
	order of NeuralOperators.h, integers and enumerations as 32 bits,
	booleans as one byte, floats as their 32 bits. */
static status_t
add_parameters(Sha256& hash, const neural_graph_operator& op)
{
	switch (op.code) {
		case B_NEURAL_OP_CONV_2D:
		case B_NEURAL_OP_DEPTHWISE_CONV_2D:
		case B_NEURAL_OP_TRANSPOSE_CONV:
		{
			const neural_conv_params* params
				= (const neural_conv_params*)op.params;
			if (params == NULL)
				return B_BAD_VALUE;
			hash.AddUInt32(28);
			hash.AddInt32(params->padding);
			hash.AddInt32(params->stride_width);
			hash.AddInt32(params->stride_height);
			hash.AddInt32(params->dilation_width);
			hash.AddInt32(params->dilation_height);
			// means something for depthwise convolutions only
			hash.AddInt32(op.code == B_NEURAL_OP_DEPTHWISE_CONV_2D
				? params->depth_multiplier : 0);
			hash.AddInt32(params->activation);
			return B_OK;
		}

		case B_NEURAL_OP_FULLY_CONNECTED:
		{
			const neural_fully_connected_params* params
				= (const neural_fully_connected_params*)op.params;
			if (params == NULL)
				return B_BAD_VALUE;
			hash.AddUInt32(5);
			hash.AddInt32(params->activation);
			hash.AddUInt8(params->keep_dimensions ? 1 : 0);
			return B_OK;
		}

		case B_NEURAL_OP_AVERAGE_POOL_2D:
		case B_NEURAL_OP_MAX_POOL_2D:
		{
			const neural_pool_params* params
				= (const neural_pool_params*)op.params;
			if (params == NULL)
				return B_BAD_VALUE;
			hash.AddUInt32(24);
			hash.AddInt32(params->padding);
			hash.AddInt32(params->stride_width);
			hash.AddInt32(params->stride_height);
			hash.AddInt32(params->filter_width);
			hash.AddInt32(params->filter_height);
			hash.AddInt32(params->activation);
			return B_OK;
		}

		case B_NEURAL_OP_ADD:
		case B_NEURAL_OP_SUB:
		case B_NEURAL_OP_MUL:
		case B_NEURAL_OP_DIV:
		case B_NEURAL_OP_MAXIMUM:
		case B_NEURAL_OP_MINIMUM:
		{
			const neural_binary_params* params
				= (const neural_binary_params*)op.params;
			hash.AddUInt32(4);
			hash.AddInt32(params != NULL ? params->activation
				: B_NEURAL_ACTIVATION_NONE);
			return B_OK;
		}

		case B_NEURAL_OP_LEAKY_RELU:
			if (op.params == NULL)
				return B_BAD_VALUE;
			hash.AddUInt32(4);
			hash.AddFloat(((const neural_leaky_relu_params*)op.params)->alpha);
			return B_OK;

		case B_NEURAL_OP_SOFTMAX:
			if (op.params == NULL)
				return B_BAD_VALUE;
			hash.AddUInt32(4);
			hash.AddFloat(((const neural_softmax_params*)op.params)->beta);
			return B_OK;

		case B_NEURAL_OP_RESHAPE:
		{
			const neural_reshape_params* params
				= (const neural_reshape_params*)op.params;
			int32 rank = params != NULL ? params->rank : 0;
			if (rank < 0 || rank > B_NEURAL_MAX_RANK)
				return B_BAD_VALUE;
			hash.AddUInt32(4 + 4 * rank);
			hash.AddInt32(rank);
			for (int32 i = 0; i < rank; i++)
				hash.AddInt32(params->shape[i]);
			return B_OK;
		}

		case B_NEURAL_OP_SQUEEZE:
		{
			const neural_squeeze_params* params
				= (const neural_squeeze_params*)op.params;
			int32 count = params != NULL ? params->count : 0;
			if (count < 0 || count > B_NEURAL_MAX_RANK)
				return B_BAD_VALUE;
			hash.AddUInt32(4 + 4 * count);
			hash.AddInt32(count);
			for (int32 i = 0; i < count; i++)
				hash.AddInt32(params->dimensions[i]);
			return B_OK;
		}

		case B_NEURAL_OP_CONCATENATION:
		{
			const neural_concat_params* params
				= (const neural_concat_params*)op.params;
			if (params == NULL)
				return B_BAD_VALUE;
			hash.AddUInt32(8);
			hash.AddInt32(params->axis);
			hash.AddInt32(params->activation);
			return B_OK;
		}

		case B_NEURAL_OP_MEAN:
			hash.AddUInt32(1);
			hash.AddUInt8(op.params != NULL
				&& ((const neural_reduce_params*)op.params)->keep_dimensions
				? 1 : 0);
			return B_OK;

		case B_NEURAL_OP_RESIZE_BILINEAR:
		case B_NEURAL_OP_RESIZE_NEAREST:
		{
			const neural_resize_params* params
				= (const neural_resize_params*)op.params;
			hash.AddUInt32(2);
			hash.AddUInt8(params != NULL && params->align_corners ? 1 : 0);
			hash.AddUInt8(params != NULL && params->half_pixel_centers
				? 1 : 0);
			return B_OK;
		}

		case B_NEURAL_OP_STRIDED_SLICE:
		{
			const neural_strided_slice_params* params
				= (const neural_strided_slice_params*)op.params;
			if (params == NULL)
				return B_BAD_VALUE;
			hash.AddUInt32(20);
			hash.AddInt32(params->begin_mask);
			hash.AddInt32(params->end_mask);
			hash.AddInt32(params->ellipsis_mask);
			hash.AddInt32(params->new_axis_mask);
			hash.AddInt32(params->shrink_axis_mask);
			return B_OK;
		}

		case B_NEURAL_OP_DETECTION_POSTPROCESS:
		{
			const neural_detection_params* params
				= (const neural_detection_params*)op.params;
			if (params == NULL)
				return B_BAD_VALUE;
			hash.AddUInt32(41);
			hash.AddInt32(params->max_detections);
			hash.AddInt32(params->max_classes_per_detection);
			hash.AddInt32(params->detections_per_class);
			hash.AddInt32(params->num_classes);
			hash.AddFloat(params->score_threshold);
			hash.AddFloat(params->iou_threshold);
			hash.AddFloat(params->y_scale);
			hash.AddFloat(params->x_scale);
			hash.AddFloat(params->h_scale);
			hash.AddFloat(params->w_scale);
			hash.AddUInt8(params->use_regular_nms ? 1 : 0);
			return B_OK;
		}

		default:
			// An operator with parameters this code has not been told
			// about has no key: its structure may have padding.
			if (neural_operator_params_size(op.code) != 0)
				return B_NOT_SUPPORTED;
			hash.AddUInt32(0);
			return B_OK;
	}
}


status_t
xdna_graph_key(const BNeuralGraph& graph, char key[33])
{
	int32 tensorCount = graph.CountTensors();
	int32 opCount = graph.CountOperators();

	// The tensors get the numbers of their first appearance, operator by
	// operator, inputs before outputs: the key does not depend on the
	// order a file or a program happened to list them in.
	int32* numbers = (int32*)malloc((tensorCount + 1) * sizeof(int32));
	int32* order = (int32*)malloc((tensorCount + 1) * sizeof(int32));
	if (numbers == NULL || order == NULL) {
		free(numbers);
		free(order);
		return B_NO_MEMORY;
	}
	for (int32 i = 0; i < tensorCount; i++)
		numbers[i] = -1;
	int32 used = 0;

	Sha256 hash;
	static const char kMagic[] = "amdxdna-graph-1\n";
	hash.Update(kMagic, sizeof(kMagic) - 1);
	status_t status = B_OK;

	hash.AddUInt32(opCount);
	for (int32 i = 0; i < opCount && status == B_OK; i++) {
		const neural_graph_operator* op = graph.OperatorAt(i);
		hash.AddString(neural_operator_name(op->code));
		for (int pass = 0; pass < 2 && status == B_OK; pass++) {
			const int32* list = pass == 0 ? op->inputs : op->outputs;
			int32 count = pass == 0 ? op->input_count : op->output_count;
			hash.AddUInt32(count);
			for (int32 j = 0; j < count; j++) {
				int32 tensor = list[j];
				if (tensor < 0) {
					hash.AddInt32(-1);
					continue;
				}
				if (tensor >= tensorCount) {
					status = B_BAD_VALUE;
					break;
				}
				if (numbers[tensor] < 0) {
					order[used] = tensor;
					numbers[tensor] = used++;
				}
				hash.AddInt32(numbers[tensor]);
			}
		}
		if (status == B_OK)
			status = add_parameters(hash, *op);
	}

	hash.AddUInt32(used);
	for (int32 i = 0; i < used && status == B_OK; i++) {
		const neural_graph_tensor* tensor = graph.TensorAt(order[i]);
		hash.AddUInt32(tensor->type);
		hash.AddUInt32(tensor->rank);
		for (int32 j = 0; j < tensor->rank; j++)
			hash.AddInt32(tensor->dimensions[j]);
		hash.AddUInt8(tensor->quantized ? 1 : 0);
		if (tensor->quantized) {
			hash.AddFloat(tensor->scale);
			hash.AddInt32(tensor->zero_point);
			hash.AddUInt32(tensor->channel_count);
			if (tensor->channel_count > 0) {
				hash.AddInt32(tensor->channel_axis);
				for (int32 j = 0; j < tensor->channel_count; j++)
					hash.AddFloat(tensor->channel_scales[j]);
			}
		}
		hash.AddUInt8(tensor->data != NULL ? 1 : 0);
		if (tensor->data != NULL)
			hash.AddUInt64(tensor->size);
	}

	// the network's own inputs and outputs; -2: one no operator touches
	for (int pass = 0; pass < 2 && status == B_OK; pass++) {
		int32 count = pass == 0 ? graph.CountInputs() : graph.CountOutputs();
		hash.AddUInt32(count);
		for (int32 i = 0; i < count; i++) {
			int32 tensor = pass == 0 ? graph.InputAt(i) : graph.OutputAt(i);
			hash.AddInt32(tensor >= 0 && tensor < tensorCount
				&& numbers[tensor] >= 0 ? numbers[tensor] : -2);
		}
	}

	free(numbers);
	free(order);
	if (status != B_OK)
		return status;

	uint8 digest[32];
	hash.Finish(digest);
	Sha256::ToHex(digest, 16, key);
	return B_OK;
}


status_t
xdna_weights_digest(const BNeuralGraph& graph, int32 first, int32 count,
	char text[65])
{
	if (first < 0 || count <= 0 || first + count > graph.CountOperators())
		return B_BAD_VALUE;

	Sha256 hash;
	static const char kMagic[] = "amdxdna-weights-1\n";
	hash.Update(kMagic, sizeof(kMagic) - 1);

	for (int32 i = 0; i < count; i++) {
		const neural_graph_operator* op = graph.OperatorAt(first + i);
		for (int32 j = 0; j < op->input_count; j++) {
			if (op->inputs[j] < 0)
				continue;
			const neural_graph_tensor* tensor = graph.TensorAt(op->inputs[j]);
			if (tensor == NULL || tensor->data == NULL)
				continue;
			hash.AddUInt32(i);
			hash.AddUInt32(j);
			hash.AddUInt64(tensor->size);
			hash.Update(tensor->data, tensor->size);
		}
	}

	uint8 digest[32];
	hash.Finish(digest);
	Sha256::ToHex(digest, 32, text);
	return B_OK;
}


// #pragma mark - directories


/*!	Where bundles are looked for: the directory the environment names,
	then data/neural/amdxdna of the user's and the system's (non-packaged
	first) data directories. Only directories that exist. */
static void
get_bundle_directories(BStringList& directories)
{
	struct stat st;
	const char* override = getenv(XDNA_BUNDLE_ENVIRONMENT);
	if (override != NULL && override[0] != '\0' && stat(override, &st) == 0
		&& S_ISDIR(st.st_mode)) {
		directories.Add(override);
	}

	BStringList found;
	if (BPathFinder::FindPaths(B_FIND_PATH_DATA_DIRECTORY,
			XDNA_BUNDLE_SUBPATH, B_FIND_PATH_EXISTING_ONLY, found) == B_OK) {
		directories.Add(found);
	}
}


bool
xdna_have_bundle_directories()
{
	BStringList directories;
	get_bundle_directories(directories);
	return !directories.IsEmpty();
}


// #pragma mark - Bundle


Bundle::Bundle()
	:
	fFirst(0),
	fCount(0),
	fColumns(0),
	fOpsPerCycle(2048),
	fOpcode(3),
	fSizeInBytes(false),
	fTimeout(5000),
	fBufferCount(0)
{
}


Bundle::~Bundle()
{
}


status_t
Bundle::Find(const char* key)
{
	BStringList directories;
	get_bundle_directories(directories);

	for (int32 i = 0; i < directories.CountStrings(); i++) {
		BString path(directories.StringAt(i));
		path << "/" << key;
		BString manifest(path);
		manifest << "/" << XDNA_BUNDLE_MANIFEST;
		struct stat st;
		if (stat(manifest.String(), &st) != 0)
			continue;

		// the first one decides, as with add-ons: one being worked on
		// stands in front of the installed one
		status_t status = Load(path.String());
		if (status == B_OK && fKey != key)
			status = B_MISMATCHED_VALUES;
		return status;
	}
	return B_ENTRY_NOT_FOUND;
}


status_t
Bundle::ReadFile(const char* name, void** _data, size_t* _size) const
{
	// a name, not a path: nothing outside the bundle
	if (name[0] == '\0' || strchr(name, '/') != NULL || name[0] == '.')
		return B_BAD_VALUE;

	BString path(fPath);
	path << "/" << name;
	int fd = open(path.String(), O_RDONLY);
	if (fd < 0)
		return errno;
	struct stat st;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0
		|| (size_t)st.st_size > kMaxFileSize) {
		close(fd);
		return B_BAD_DATA;
	}
	void* data = malloc(st.st_size + 1);
	if (data == NULL) {
		close(fd);
		return B_NO_MEMORY;
	}
	ssize_t bytesRead = read(fd, data, st.st_size);
	close(fd);
	if (bytesRead != st.st_size) {
		free(data);
		return B_IO_ERROR;
	}
	((char*)data)[st.st_size] = '\0';
	*_data = data;
	*_size = st.st_size;
	return B_OK;
}


/*!	"<in|out|const>:<bytes>:op<k>.<in|out><j>:<layout>" */
status_t
Bundle::_ParseBuffer(char* value)
{
	if (fBufferCount == XDNA_MAX_BUFFERS)
		return B_BAD_DATA;
	bundle_buffer& buffer = fBuffers[fBufferCount];

	char* fields[4];
	for (int i = 0; i < 4; i++) {
		fields[i] = value;
		char* colon = value != NULL ? strchr(value, ':') : NULL;
		if (colon != NULL)
			*colon++ = '\0';
		value = colon;
	}
	if (fields[3] == NULL || value != NULL)
		return B_BAD_DATA;

	if (strcmp(fields[0], "in") == 0)
		buffer.kind = BUNDLE_BUFFER_INPUT;
	else if (strcmp(fields[0], "out") == 0)
		buffer.kind = BUNDLE_BUFFER_OUTPUT;
	else if (strcmp(fields[0], "const") == 0)
		buffer.kind = BUNDLE_BUFFER_CONSTANT;
	else
		return B_BAD_DATA;

	char* end;
	unsigned long long size = strtoull(fields[1], &end, 10);
	if (*end != '\0' || size == 0 || size > kMaxFileSize)
		return B_BAD_DATA;
	buffer.size = size;

	int op, index;
	char direction[4];
	int consumed = 0;
	if (sscanf(fields[2], "op%d.%3[a-z]%d%n", &op, direction, &index,
			&consumed) != 3 || fields[2][consumed] != '\0' || op < 0
		|| index < 0) {
		return B_BAD_DATA;
	}
	if (strcmp(direction, "in") == 0)
		buffer.output = false;
	else if (strcmp(direction, "out") == 0)
		buffer.output = true;
	else
		return B_BAD_DATA;
	buffer.op = op;
	buffer.index = index;

	if (strcmp(fields[3], "nhwc") == 0)
		buffer.layout = BUNDLE_LAYOUT_NHWC;
	else if (strcmp(fields[3], "hcwc8") == 0)
		buffer.layout = BUNDLE_LAYOUT_HCWC8;
	else if (strcmp(fields[3], "oiio8") == 0)
		buffer.layout = BUNDLE_LAYOUT_OIIO8;
	else
		return B_BAD_DATA;

	fBufferCount++;
	return B_OK;
}


status_t
Bundle::Load(const char* directory)
{
	fPath = directory;
	fBufferCount = 0;

	void* data;
	size_t size;
	status_t status = ReadFile(XDNA_BUNDLE_MANIFEST, &data, &size);
	if (status != B_OK)
		return status;

	int32 format = 0;
	status = B_OK;
	char* next;
	for (char* line = (char*)data; line != NULL && *line != '\0'
			&& status == B_OK; line = next) {
		next = strchr(line, '\n');
		if (next != NULL)
			*next++ = '\0';
		line[strcspn(line, "\r")] = '\0';
		char* value = strchr(line, '=');
		if (line[0] == '#' || value == NULL)
			continue;
		*value++ = '\0';

		if (strcmp(line, "format") == 0)
			format = atoi(value);
		else if (strcmp(line, "name") == 0)
			fName = value;
		else if (strcmp(line, "graph") == 0)
			fKey = value;
		else if (strcmp(line, "weights") == 0)
			fWeights = value;
		else if (strcmp(line, "pdi") == 0)
			fPDI = value;
		else if (strcmp(line, "insts") == 0)
			fInstructions = value;
		else if (strcmp(line, "columns") == 0)
			fColumns = strtoul(value, NULL, 10);
		else if (strcmp(line, "ops_per_cycle") == 0)
			fOpsPerCycle = strtoul(value, NULL, 10);
		else if (strcmp(line, "opcode") == 0)
			fOpcode = strtoul(value, NULL, 0);
		else if (strcmp(line, "insts_size") == 0)
			fSizeInBytes = strcmp(value, "bytes") == 0;
		else if (strcmp(line, "timeout_ms") == 0)
			fTimeout = strtoul(value, NULL, 10);
		else if (strcmp(line, "operators") == 0) {
			int first, count;
			if (sscanf(value, "%d:%d", &first, &count) != 2 || first < 0
				|| count <= 0) {
				status = B_BAD_DATA;
			} else {
				fFirst = first;
				fCount = count;
			}
		} else if (strcmp(line, "buffer") == 0)
			status = _ParseBuffer(value);
		// anything else: a later format's, or for people
	}
	free(data);

	if (status != B_OK)
		return status;
	if (format != 1 || fKey.Length() != 32 || fWeights.Length() != 64
		|| fPDI.IsEmpty() || fInstructions.IsEmpty() || fCount == 0
		|| fColumns == 0 || fColumns > 64 || fBufferCount == 0
		|| fTimeout == 0) {
		return B_BAD_DATA;
	}
	return B_OK;
}


int32
Bundle::TensorOf(const BNeuralGraph& graph, const bundle_buffer& buffer) const
{
	if (buffer.op >= fCount)
		return -1;
	const neural_graph_operator* op = graph.OperatorAt(fFirst + buffer.op);
	if (op == NULL)
		return -1;
	if (buffer.output) {
		return buffer.index < op->output_count
			? op->outputs[buffer.index] : -1;
	}
	return buffer.index < op->input_count ? op->inputs[buffer.index] : -1;
}


static size_t
tensor_elements(const neural_graph_tensor& tensor)
{
	size_t count = 1;
	for (int32 i = 0; i < tensor.rank; i++)
		count *= tensor.dimensions[i];
	return count;
}


static bool
layout_fits(bundle_layout layout, const neural_graph_tensor& tensor)
{
	size_t elementSize = neural_data_type_size(tensor.type);
	switch (layout) {
		case BUNDLE_LAYOUT_NHWC:
			return elementSize > 0;
		case BUNDLE_LAYOUT_HCWC8:
			return elementSize == 1 && tensor.rank == 4
				&& tensor.dimensions[0] == 1 && tensor.dimensions[3] % 8 == 0;
		case BUNDLE_LAYOUT_OIIO8:
			return elementSize == 1 && tensor.rank == 4
				&& tensor.dimensions[1] == 1 && tensor.dimensions[2] == 1
				&& tensor.dimensions[0] % 8 == 0
				&& tensor.dimensions[3] % 8 == 0;
	}
	return false;
}


status_t
Bundle::Check(const BNeuralGraph& graph) const
{
	int32 tensorCount = graph.CountTensors();
	int32 opCount = graph.CountOperators();
	if (fFirst + fCount > opCount)
		return B_MISMATCHED_VALUES;

	// 1: made inside the stretch, 2: needed by it from outside,
	// 4: needed outside of what it makes, 8: has a buffer
	uint8* marks = (uint8*)calloc(tensorCount + 1, 1);
	if (marks == NULL)
		return B_NO_MEMORY;

	for (int32 i = fFirst; i < fFirst + fCount; i++) {
		const neural_graph_operator* op = graph.OperatorAt(i);
		for (int32 j = 0; j < op->input_count; j++) {
			int32 index = op->inputs[j];
			if (index >= 0 && graph.TensorAt(index)->data == NULL
				&& (marks[index] & 1) == 0) {
				marks[index] |= 2;
			}
		}
		for (int32 j = 0; j < op->output_count; j++)
			marks[op->outputs[j]] |= 1;
	}
	for (int32 i = fFirst + fCount; i < opCount; i++) {
		const neural_graph_operator* op = graph.OperatorAt(i);
		for (int32 j = 0; j < op->input_count; j++) {
			int32 index = op->inputs[j];
			if (index >= 0 && (marks[index] & 1) != 0)
				marks[index] |= 4;
		}
	}
	for (int32 i = 0; i < graph.CountOutputs(); i++) {
		int32 index = graph.OutputAt(i);
		if ((marks[index] & 1) != 0)
			marks[index] |= 4;
	}

	status_t status = B_OK;
	for (int32 i = 0; i < fBufferCount && status == B_OK; i++) {
		const bundle_buffer& buffer = fBuffers[i];
		int32 index = TensorOf(graph, buffer);
		if (index < 0 || index >= tensorCount || (marks[index] & 8) != 0) {
			status = B_MISMATCHED_VALUES;
			break;
		}
		const neural_graph_tensor& tensor = *graph.TensorAt(index);
		bool right = false;
		switch (buffer.kind) {
			case BUNDLE_BUFFER_INPUT:
				right = (marks[index] & 2) != 0;
				break;
			case BUNDLE_BUFFER_OUTPUT:
				right = (marks[index] & 1) != 0;
				break;
			case BUNDLE_BUFFER_CONSTANT:
				right = tensor.data != NULL;
				break;
		}
		if (!right || !layout_fits(buffer.layout, tensor)
			|| tensor_elements(tensor) * neural_data_type_size(tensor.type)
				!= buffer.size
			|| (tensor.data != NULL && tensor.size != buffer.size)) {
			status = B_MISMATCHED_VALUES;
		}
		marks[index] |= 8;
	}

	// everything that crosses the border has to have a buffer
	for (int32 i = 0; i < tensorCount && status == B_OK; i++) {
		if (((marks[i] & 2) != 0 || (marks[i] & 4) != 0)
			&& (marks[i] & 8) == 0) {
			status = B_MISMATCHED_VALUES;
		}
	}

	free(marks);
	return status;
}


// #pragma mark - layouts


void
xdna_convert_layout(bundle_layout layout, const neural_graph_tensor& tensor,
	void* _tensorData, void* _bufferData, bool toDevice)
{
	uint8* tensorData = (uint8*)_tensorData;
	uint8* bufferData = (uint8*)_bufferData;

	switch (layout) {
		case BUNDLE_LAYOUT_NHWC:
		{
			size_t size = tensor_elements(tensor)
				* neural_data_type_size(tensor.type);
			if (toDevice)
				memcpy(bufferData, tensorData, size);
			else
				memcpy(tensorData, bufferData, size);
			break;
		}

		case BUNDLE_LAYOUT_HCWC8:
		{
			// network: [y][x][c]; design: [y][c / 8][x][c % 8]
			size_t height = tensor.dimensions[1];
			size_t width = tensor.dimensions[2];
			size_t channels = tensor.dimensions[3];
			for (size_t y = 0; y < height; y++) {
				uint8* line = tensorData + y * width * channels;
				uint8* designLine = bufferData + y * width * channels;
				for (size_t group = 0; group < channels / 8; group++) {
					uint8* in = line + group * 8;
					uint8* out = designLine + group * width * 8;
					if (toDevice) {
						for (size_t x = 0; x < width; x++)
							memcpy(out + x * 8, in + x * channels, 8);
					} else {
						for (size_t x = 0; x < width; x++)
							memcpy(in + x * channels, out + x * 8, 8);
					}
				}
			}
			break;
		}

		case BUNDLE_LAYOUT_OIIO8:
		{
			// network: [o][i]; design: [o / 8][i / 8][i % 8][o % 8]
			size_t outputs = tensor.dimensions[0];
			size_t inputs = tensor.dimensions[3];
			for (size_t o = 0; o < outputs; o++) {
				for (size_t i = 0; i < inputs; i++) {
					uint8& inTensor = tensorData[o * inputs + i];
					uint8& inDesign = bufferData[(o / 8) * inputs * 8
						+ (i / 8) * 64 + (i % 8) * 8 + o % 8];
					if (toDevice)
						inDesign = inTensor;
					else
						inTensor = inDesign;
				}
			}
			break;
		}
	}
}
