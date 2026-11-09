/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	DETECTION_POSTPROCESS: what a single-shot detector ends with (the
	TFLite_Detection_PostProcess operator of TensorFlow Lite). The network
	gives, for each of its fixed anchor boxes, a correction of the box and
	a score per class. This operator applies the corrections, then keeps
	the best boxes that do not overlap a better one too much ("non-maximum
	suppression"): either once over the best class of each box (fast), or
	class by class with the results merged (regular).

	The arithmetic follows the reference step by step, down to which
	steps are in single and which in double precision, so that the same
	boxes come out in the same order.
*/


#pragma GCC optimize("O2", "fp-contract=off")


#include "Kernels.h"

#include <algorithm>


namespace BPrivate {


namespace {


struct DetectionData {
	neural_detection_params	params;
	int32	boxes;
	//! numbers per box in the encodings (the first four are used)
	int32	encoding_size;
	//! scores per box, with the background class if there is one
	int32	classes;
	//! 1 when the scores start with a background class
	int32	label_offset;
	//! slots in the outputs
	int32	slots;
	//! classes reported per box (fast)
	int32	classes_per_box;
};


struct Box {
	float	top;
	float	left;
	float	bottom;
	float	right;
};


struct Candidate {
	int32	index;
	float	score;
};


//! The memory a run needs, in one block.
struct Scratch {
	Box*		boxes;
	float*		scores;
	float*		column;
	int32*		best_classes;
	int32*		kept;
	int32*		merge;
	uint8*		active;
	int32*		selected;
	Candidate*	candidates;
	Candidate*	candidates_merge;
};


static inline float
real_value(const neural_graph_tensor& info, const void* data, int32 index)
{
	if (info.type == B_NEURAL_FLOAT32)
		return ((const float*)data)[index];
	return ((float)((const uint8*)data)[index] - (float)info.zero_point)
		* info.scale;
}


/*!	Corrections applied to the anchors: the centre moves by a fraction of
	the anchor's size, the size is scaled by an exponential. */
static bool
decode_boxes(const NeuralOpContext& context, const DetectionData& data,
	Box* boxes)
{
	const neural_graph_tensor& encodingInfo = context.InputInfo(0);
	const neural_graph_tensor& anchorInfo = context.InputInfo(2);
	const void* encodings = context.Input<void>(0);
	const void* anchors = context.Input<void>(2);
	const neural_detection_params& params = data.params;

	for (int32 i = 0; i < data.boxes; i++) {
		int32 encoding = i * data.encoding_size;
		float y = real_value(encodingInfo, encodings, encoding);
		float x = real_value(encodingInfo, encodings, encoding + 1);
		float height = real_value(encodingInfo, encodings, encoding + 2);
		float width = real_value(encodingInfo, encodings, encoding + 3);
		float anchorY = real_value(anchorInfo, anchors, 4 * i);
		float anchorX = real_value(anchorInfo, anchors, 4 * i + 1);
		float anchorHeight = real_value(anchorInfo, anchors, 4 * i + 2);
		float anchorWidth = real_value(anchorInfo, anchors, 4 * i + 3);

		float centerY = (float)((double)y / (double)params.y_scale
			* (double)anchorHeight + (double)anchorY);
		float centerX = (float)((double)x / (double)params.x_scale
			* (double)anchorWidth + (double)anchorX);
		float halfHeight = (float)(0.5 * exp((double)height
			/ (double)params.h_scale) * (double)anchorHeight);
		float halfWidth = (float)(0.5 * exp((double)width
			/ (double)params.w_scale) * (double)anchorWidth);

		Box& box = boxes[i];
		box.top = centerY - halfHeight;
		box.left = centerX - halfWidth;
		box.bottom = centerY + halfHeight;
		box.right = centerX + halfWidth;

		// A box that is inside out cannot be compared with others; the
		// reference gives up on the whole picture, too.
		if (box.top > box.bottom || box.left > box.right)
			return false;
	}
	return true;
}


static float
intersection_over_union(const Box& a, const Box& b)
{
	float areaA = (a.bottom - a.top) * (a.right - a.left);
	float areaB = (b.bottom - b.top) * (b.right - b.left);
	if (areaA <= 0 || areaB <= 0)
		return 0;

	float top = std::max(a.top, b.top);
	float left = std::max(a.left, b.left);
	float bottom = std::min(a.bottom, b.bottom);
	float right = std::min(a.right, b.right);
	float area = std::max(bottom - top, 0.0f) * std::max(right - left, 0.0f);
	return area / (areaA + areaB - area);
}


/*!	Sorts indices by falling score, equal scores staying in the order
	they came in (a merge sort; \a spare is as large as \a indices). */
static void
sort_by_score(const float* scores, int32* indices, int32* spare, int32 count)
{
	for (int32 width = 1; width < count; width *= 2) {
		for (int32 start = 0; start < count; start += 2 * width) {
			int32 middle = std::min(start + width, count);
			int32 end = std::min(start + 2 * width, count);
			int32 left = start;
			int32 right = middle;
			for (int32 i = start; i < end; i++) {
				if (left < middle && (right >= end
						|| !(scores[indices[right]]
							> scores[indices[left]]))) {
					spare[i] = indices[left++];
				} else
					spare[i] = indices[right++];
			}
		}
		memcpy(indices, spare, count * sizeof(int32));
	}
}


/*!	Non-maximum suppression of one list of scores, one per box: goes
	through the boxes above the threshold from the best on, takes a box
	and drops all later ones that overlap it by more than the limit.
	Returns how many were put into \a selected, at most \a limit. */
static int32
suppress(const DetectionData& data, const Scratch& scratch,
	const float* scores, int32 limit, int32* selected)
{
	int32 kept = 0;
	for (int32 i = 0; i < data.boxes; i++) {
		if (scores[i] >= data.params.score_threshold)
			scratch.kept[kept++] = i;
	}

	// sorted by score; equal ones stay in the order of their boxes
	sort_by_score(scores, scratch.kept, scratch.merge, kept);
	const int32* sorted = scratch.kept;

	int32 outputSize = std::min(kept, limit);
	int32 active = kept;
	int32 count = 0;
	memset(scratch.active, 1, kept);

	for (int32 i = 0; i < kept; i++) {
		if (active == 0 || count >= outputSize)
			break;
		if (scratch.active[i] == 0)
			continue;

		selected[count++] = sorted[i];
		scratch.active[i] = 0;
		active--;

		const Box& box = scratch.boxes[sorted[i]];
		for (int32 j = i + 1; j < kept; j++) {
			if (scratch.active[j] != 0 && intersection_over_union(box,
					scratch.boxes[sorted[j]]) > data.params.iou_threshold) {
				scratch.active[j] = 0;
				active--;
			}
		}
	}
	return count;
}


struct Outputs {
	Box*	boxes;
	float*	classes;
	float*	scores;
	float*	count;
};


/*!	Suppression once, over the best score of each box; a box that stays
	is reported with its best classes. */
static void
fast_suppression(const DetectionData& data, const Scratch& scratch,
	const float* scores, const Outputs& outputs)
{
	int32 classes = data.params.num_classes;
	int32 perBox = data.classes_per_box;
	float* bestScores = scratch.column;

	for (int32 box = 0; box < data.boxes; box++) {
		const float* boxScores = scores + (size_t)box * data.classes
			+ data.label_offset;
		int32* best = scratch.best_classes + (size_t)box * perBox;

		// the best classes in order; of equal ones the first wins
		for (int32 rank = 0; rank < perBox; rank++) {
			int32 choice = -1;
			for (int32 i = 0; i < classes; i++) {
				bool taken = false;
				for (int32 j = 0; j < rank; j++)
					taken |= best[j] == i;
				if (!taken && (choice < 0
						|| boxScores[i] > boxScores[choice])) {
					choice = i;
				}
			}
			best[rank] = choice;
		}
		bestScores[box] = boxScores[best[0]];
	}

	int32 count = suppress(data, scratch, bestScores,
		data.params.max_detections, scratch.selected);

	for (int32 i = 0; i < count; i++) {
		int32 box = scratch.selected[i];
		const float* boxScores = scores + (size_t)box * data.classes
			+ data.label_offset;
		const int32* best = scratch.best_classes + (size_t)box * perBox;
		for (int32 j = 0; j < perBox; j++) {
			int32 slot = data.params.max_classes_per_detection * i + j;
			outputs.boxes[slot] = scratch.boxes[box];
			outputs.classes[slot] = best[j];
			outputs.scores[slot] = boxScores[best[j]];
		}
	}
	outputs.count[0] = count;
}


/*!	Suppression class by class; what stays of each class is merged into
	one list by score, of which the best are reported. */
static void
regular_suppression(const DetectionData& data, const Scratch& scratch,
	const float* scores, const Outputs& outputs)
{
	int32 limit = data.params.max_detections;
	int32 perClass = std::min(data.params.detections_per_class, limit);
	Candidate* candidates = scratch.candidates;
	int32 count = 0;

	for (int32 classIndex = 0; classIndex < data.params.num_classes;
			classIndex++) {
		int32 column = classIndex + data.label_offset;
		for (int32 box = 0; box < data.boxes; box++) {
			scratch.column[box] = scores[(size_t)box * data.classes
				+ column];
		}

		int32 selected = suppress(data, scratch, scratch.column, perClass,
			scratch.selected);
		if (selected == 0)
			continue;

		// Merge with what there is; of equal scores the earlier class
		// stays in front.
		Candidate* merged = scratch.candidates_merge;
		int32 left = 0;
		int32 right = 0;
		for (int32 i = 0; i < count + selected; i++) {
			float score = right < selected
				? scratch.column[scratch.selected[right]] : 0;
			if (left < count && (right >= selected
					|| !(score > candidates[left].score))) {
				merged[i] = candidates[left++];
			} else {
				merged[i].index = scratch.selected[right] * data.classes
					+ column;
				merged[i].score = score;
				right++;
			}
		}
		memcpy(candidates, merged, (count + selected) * sizeof(Candidate));
		count = std::min(count + selected, limit);
	}

	for (int32 i = 0; i < count; i++) {
		int32 box = candidates[i].index / data.classes;
		outputs.boxes[i] = scratch.boxes[box];
		outputs.classes[i] = candidates[i].index - box * data.classes
			- data.label_offset;
		outputs.scores[i] = candidates[i].score;
	}
	outputs.count[0] = count;
}


static status_t
prepare_detection(const NeuralGraphData* graph,
	const neural_graph_operator* op, void** _prepared)
{
	if (op->input_count != 3 || !neural_has_inputs(op, 3)
		|| op->output_count != 4 || op->params == NULL) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	const neural_detection_params& params
		= *(const neural_detection_params*)op->params;
	const neural_graph_tensor& encodings = graph->Tensor(op->inputs[0]);
	const neural_graph_tensor& scores = graph->Tensor(op->inputs[1]);
	const neural_graph_tensor& anchors = graph->Tensor(op->inputs[2]);

	// one picture: [1, boxes, 4 or more], [1, boxes, classes], [boxes, 4]
	if (encodings.rank != 3 || scores.rank != 3 || anchors.rank != 2
		|| encodings.dimensions[0] != 1 || scores.dimensions[0] != 1
		|| encodings.dimensions[2] < 4
		|| scores.dimensions[1] != encodings.dimensions[1]
		|| anchors.dimensions[0] != encodings.dimensions[1]
		|| anchors.dimensions[1] != 4
		|| neural_element_count(encodings) < 0
		|| neural_element_count(scores) < 0) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	// Real numbers or unsigned quantized ones; the anchors like the
	// encodings.
	const neural_graph_tensor* inputs[3] = { &encodings, &scores, &anchors };
	for (int32 i = 0; i < 3; i++) {
		const neural_graph_tensor& input = *inputs[i];
		if (input.type != B_NEURAL_FLOAT32 && (input.type != B_NEURAL_UINT8
				|| !neural_is_quantized_8(input))) {
			return B_NEURAL_UNSUPPORTED_OPERATOR;
		}
	}
	if (anchors.type != encodings.type)
		return B_NEURAL_UNSUPPORTED_OPERATOR;

	DetectionData header;
	memset(&header, 0, sizeof(header));
	header.params = params;
	header.boxes = encodings.dimensions[1];
	header.encoding_size = encodings.dimensions[2];
	header.classes = scores.dimensions[2];
	header.label_offset = header.classes - params.num_classes;

	if (params.max_detections < 1 || params.max_detections > 65536
		|| params.max_classes_per_detection < 1
		|| params.max_classes_per_detection > 65536
		|| params.num_classes < 1
		|| header.label_offset < 0 || header.label_offset > 1
		|| !(params.iou_threshold > 0) || !(params.iou_threshold <= 1)
		|| isnan(params.score_threshold)
		|| !isfinite(params.y_scale) || params.y_scale == 0
		|| !isfinite(params.x_scale) || params.x_scale == 0
		|| !isfinite(params.h_scale) || params.h_scale == 0
		|| !isfinite(params.w_scale) || params.w_scale == 0
		|| (params.use_regular_nms && params.detections_per_class < 1)) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	header.slots = params.max_detections * params.max_classes_per_detection;
	header.classes_per_box = std::min(params.max_classes_per_detection,
		params.num_classes);
	if ((int64)header.boxes * header.classes_per_box > kNeuralMaxElements)
		return B_NEURAL_UNSUPPORTED_OPERATOR;

	// boxes [1, slots, 4], classes and scores [1, slots], count [1]
	static const int32 kNumbersPerSlot[4] = { 4, 1, 1, 0 };
	for (int32 i = 0; i < 4; i++) {
		const neural_graph_tensor& output = graph->Tensor(op->outputs[i]);
		int32 expected = i < 3 ? header.slots * kNumbersPerSlot[i] : 1;
		if (output.type != B_NEURAL_FLOAT32
			|| neural_element_count(output) != expected) {
			return B_NEURAL_UNSUPPORTED_OPERATOR;
		}
	}

	DetectionData* data = (DetectionData*)malloc(sizeof(DetectionData));
	if (data == NULL)
		return B_NO_MEMORY;
	*data = header;
	*_prepared = data;
	return B_OK;
}


static status_t
run_detection(const NeuralOpContext& context)
{
	const DetectionData& data = *(const DetectionData*)context.prepared;
	const neural_graph_tensor& scoreInfo = context.InputInfo(1);
	bool quantizedScores = scoreInfo.type != B_NEURAL_FLOAT32;
	int32 boxes = data.boxes;
	int32 candidates = data.params.max_detections
		+ std::min(data.params.detections_per_class,
			data.params.max_detections);
	if (candidates < data.params.max_detections)
		candidates = data.params.max_detections;

	NeuralBlock block(0);
	size_t boxesOffset = block.Reserve(boxes, sizeof(Box));
	size_t scoresOffset = block.Reserve(quantizedScores
		? (size_t)boxes * data.classes : 0, sizeof(float));
	size_t columnOffset = block.Reserve(boxes, sizeof(float));
	size_t bestOffset = block.Reserve((size_t)boxes * data.classes_per_box,
		sizeof(int32));
	size_t keptOffset = block.Reserve(boxes, sizeof(int32));
	size_t mergeOffset = block.Reserve(boxes, sizeof(int32));
	size_t selectedOffset = block.Reserve(boxes, sizeof(int32));
	size_t activeOffset = block.Reserve(boxes, 1);
	size_t candidatesOffset = block.Reserve(2 * (size_t)candidates,
		sizeof(Candidate));
	if (block.Allocate() == NULL)
		return B_NO_MEMORY;

	Scratch scratch;
	scratch.boxes = block.At<Box>(boxesOffset);
	scratch.scores = block.At<float>(scoresOffset);
	scratch.column = block.At<float>(columnOffset);
	scratch.best_classes = block.At<int32>(bestOffset);
	scratch.kept = block.At<int32>(keptOffset);
	scratch.merge = block.At<int32>(mergeOffset);
	scratch.selected = block.At<int32>(selectedOffset);
	scratch.active = block.At<uint8>(activeOffset);
	scratch.candidates = block.At<Candidate>(candidatesOffset);
	scratch.candidates_merge = scratch.candidates + candidates;

	// unused slots are zero
	Outputs outputs = { context.Output<Box>(0), context.Output<float>(1),
		context.Output<float>(2), context.Output<float>(3) };
	memset(outputs.boxes, 0, data.slots * sizeof(Box));
	memset(outputs.classes, 0, data.slots * sizeof(float));
	memset(outputs.scores, 0, data.slots * sizeof(float));
	outputs.count[0] = 0;

	status_t status = B_OK;
	if (decode_boxes(context, data, scratch.boxes)) {
		const float* scores = context.Input<float>(1);
		if (quantizedScores) {
			const uint8* stored = context.Input<uint8>(1);
			double scale = scoreInfo.scale;
			size_t count = (size_t)boxes * data.classes;
			for (size_t i = 0; i < count; i++) {
				scratch.scores[i] = (float)(scale
					* (stored[i] - scoreInfo.zero_point));
			}
			scores = scratch.scores;
		}

		if (data.params.use_regular_nms)
			regular_suppression(data, scratch, scores, outputs);
		else
			fast_suppression(data, scratch, scores, outputs);
	} else
		status = B_BAD_DATA;

	free(block.At<void>(0));
	return status;
}


}	// unnamed namespace


const NeuralKernel kDetectionPostprocessKernel = {
	B_NEURAL_OP_DETECTION_POSTPROCESS, &prepare_detection, &run_detection
};


}	// namespace BPrivate
