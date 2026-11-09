/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include <neural/NeuralUtils.h>

#include <math.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Bitmap.h>
#include <Entry.h>
#include <File.h>
#include <GraphicsDefs.h>
#include <Path.h>
#include <Screen.h>
#include <StringList.h>

#include <neural/NeuralSession.h>


neural_image_options::neural_image_options()
	:
	fit(B_NEURAL_FIT_STRETCH),
	bgr(false)
{
	for (int32 i = 0; i < 3; i++) {
		mean[i] = 127.5f;
		deviation[i] = 127.5f;
	}
	memset(_reserved, 0, sizeof(_reserved));
}


BRect
neural_image_mapping::ToBitmap(float left, float top, float right,
	float bottom) const
{
	float scaleX = (source.Width() + 1) / (destination.Width() + 1);
	float scaleY = (source.Height() + 1) / (destination.Height() + 1);

	BRect rect;
	rect.left = source.left + (left * tensor_width - destination.left)
		* scaleX;
	rect.top = source.top + (top * tensor_height - destination.top)
		* scaleY;
	rect.right = source.left + (right * tensor_width - destination.left)
		* scaleX;
	rect.bottom = source.top + (bottom * tensor_height - destination.top)
		* scaleY;
	return rect;
}


namespace {


/*!	Reads pixels of a bitmap as red, green, blue, whatever its color
	space.
*/
class PixelReader {
public:
	PixelReader(const BBitmap* bitmap)
		:
		fBits((const uint8*)bitmap->Bits()),
		fBytesPerRow(bitmap->BytesPerRow()),
		fColorSpace(bitmap->ColorSpace()),
		fPalette(NULL)
	{
		if (fColorSpace == B_CMAP8)
			fPalette = system_colors()->color_list;
	}

	static bool Supports(color_space space)
	{
		switch (space) {
			case B_RGB32:
			case B_RGBA32:
			case B_RGB24:
			case B_RGB16:
			case B_RGB15:
			case B_RGBA15:
			case B_GRAY8:
			case B_CMAP8:
				return true;
			default:
				return false;
		}
	}

	inline void Read(int32 x, int32 y, float* rgb) const
	{
		const uint8* row = fBits + (size_t)y * fBytesPerRow;
		switch (fColorSpace) {
			case B_RGB32:
			case B_RGBA32:
				rgb[0] = row[x * 4 + 2];
				rgb[1] = row[x * 4 + 1];
				rgb[2] = row[x * 4];
				break;
			case B_RGB24:
				rgb[0] = row[x * 3 + 2];
				rgb[1] = row[x * 3 + 1];
				rgb[2] = row[x * 3];
				break;
			case B_RGB16:
			{
				uint16 pixel = row[x * 2] | (row[x * 2 + 1] << 8);
				rgb[0] = ((pixel >> 11) & 0x1f) * 255.0f / 31;
				rgb[1] = ((pixel >> 5) & 0x3f) * 255.0f / 63;
				rgb[2] = (pixel & 0x1f) * 255.0f / 31;
				break;
			}
			case B_RGB15:
			case B_RGBA15:
			{
				uint16 pixel = row[x * 2] | (row[x * 2 + 1] << 8);
				rgb[0] = ((pixel >> 10) & 0x1f) * 255.0f / 31;
				rgb[1] = ((pixel >> 5) & 0x1f) * 255.0f / 31;
				rgb[2] = (pixel & 0x1f) * 255.0f / 31;
				break;
			}
			case B_GRAY8:
				rgb[0] = rgb[1] = rgb[2] = row[x];
				break;
			case B_CMAP8:
			{
				const rgb_color& color = fPalette[row[x]];
				rgb[0] = color.red;
				rgb[1] = color.green;
				rgb[2] = color.blue;
				break;
			}
			default:
				rgb[0] = rgb[1] = rgb[2] = 0;
				break;
		}
	}

private:
	const uint8*		fBits;
	int32				fBytesPerRow;
	color_space			fColorSpace;
	const rgb_color*	fPalette;
};


/*!	The layout of an image tensor: [1, height, width, channels] or
	[height, width, channels], 1 or 3 channels. */
static status_t
image_layout(const BTensor& tensor, int32* _width, int32* _height,
	int32* _channels)
{
	if (tensor.InitCheck() != B_OK)
		return tensor.InitCheck();

	int32 first = 0;
	if (tensor.Rank() == 4) {
		if (tensor.DimensionAt(0) != 1)
			return B_NEURAL_SHAPE_MISMATCH;
		first = 1;
	} else if (tensor.Rank() != 3)
		return B_NEURAL_SHAPE_MISMATCH;

	*_height = tensor.DimensionAt(first);
	*_width = tensor.DimensionAt(first + 1);
	*_channels = tensor.DimensionAt(first + 2);
	if (*_channels != 1 && *_channels != 3)
		return B_NEURAL_SHAPE_MISMATCH;

	switch (tensor.Type()) {
		case B_NEURAL_UINT8:
		case B_NEURAL_INT8:
		case B_NEURAL_FLOAT32:
			return B_OK;
		default:
			return B_NEURAL_TYPE_MISMATCH;
	}
}


static inline void
store_pixel(void* data, neural_data_type type, int64 index, int32 channel,
	float value, const neural_image_options& options)
{
	switch (type) {
		case B_NEURAL_UINT8:
			((uint8*)data)[index] = (uint8)(value + 0.5f);
			break;
		case B_NEURAL_INT8:
			((int8*)data)[index] = (int8)((int32)(value + 0.5f) - 128);
			break;
		default:
			((float*)data)[index] = (value - options.mean[channel])
				/ options.deviation[channel];
			break;
	}
}


}	// unnamed namespace


/*static*/ status_t
BNeuralUtils::SetTensorFromBitmap(BTensor* tensor, const BBitmap* bitmap,
	const BRect* sourceRect, const neural_image_options* _options,
	neural_image_mapping* _mapping)
{
	if (tensor == NULL || bitmap == NULL || bitmap->InitCheck() != B_OK)
		return B_BAD_VALUE;

	int32 width, height, channels;
	status_t status = image_layout(*tensor, &width, &height, &channels);
	if (status != B_OK)
		return status;

	if (!PixelReader::Supports(bitmap->ColorSpace()))
		return B_BAD_TYPE;

	neural_image_options options;
	if (_options != NULL)
		options = *_options;
	for (int32 i = 0; i < 3; i++) {
		if (options.deviation[i] == 0)
			return B_BAD_VALUE;
	}

	// the part of the bitmap to use, in its pixels
	BRect bounds = bitmap->Bounds().OffsetToCopy(B_ORIGIN);
	BRect source = bounds;
	if (sourceRect != NULL) {
		source = *sourceRect & bounds;
		source.left = floorf(source.left);
		source.top = floorf(source.top);
		source.right = floorf(source.right);
		source.bottom = floorf(source.bottom);
	}
	if (!source.IsValid())
		return B_BAD_VALUE;

	float sourceWidth = source.Width() + 1;
	float sourceHeight = source.Height() + 1;
	BRect destination(0, 0, width - 1, height - 1);

	if (options.fit == B_NEURAL_FIT_CROP) {
		// cut the source down to the tensor's proportions
		float wanted = sourceHeight * width / height;
		if (wanted < sourceWidth) {
			source.left += floorf((sourceWidth - wanted) / 2);
			source.right = source.left + floorf(wanted + 0.5f) - 1;
		} else {
			wanted = sourceWidth * height / width;
			source.top += floorf((sourceHeight - wanted) / 2);
			source.bottom = source.top + floorf(wanted + 0.5f) - 1;
		}
		sourceWidth = source.Width() + 1;
		sourceHeight = source.Height() + 1;
	} else if (options.fit == B_NEURAL_FIT_LETTERBOX) {
		// shrink the destination to the source's proportions
		float scale = fminf(width / sourceWidth, height / sourceHeight);
		float usedWidth = fmaxf(1, floorf(sourceWidth * scale + 0.5f));
		float usedHeight = fmaxf(1, floorf(sourceHeight * scale + 0.5f));
		destination.left = floorf((width - usedWidth) / 2);
		destination.top = floorf((height - usedHeight) / 2);
		destination.right = destination.left + usedWidth - 1;
		destination.bottom = destination.top + usedHeight - 1;
	}

	int32 destLeft = (int32)destination.left;
	int32 destTop = (int32)destination.top;
	int32 destWidth = (int32)destination.Width() + 1;
	int32 destHeight = (int32)destination.Height() + 1;
	int32 sourceLeft = (int32)source.left;
	int32 sourceTop = (int32)source.top;
	int32 maxX = (int32)sourceWidth - 1;
	int32 maxY = (int32)sourceHeight - 1;
	float stepX = sourceWidth / destWidth;
	float stepY = sourceHeight / destHeight;

	void* data = tensor->Data();
	neural_data_type type = tensor->Type();
	PixelReader reader(bitmap);

	// what is not covered is mid gray
	if (destWidth != width || destHeight != height) {
		for (int64 i = 0; i < (int64)width * height; i++) {
			for (int32 c = 0; c < channels; c++)
				store_pixel(data, type, i * channels + c, c, 128, options);
		}
	}

	for (int32 y = 0; y < destHeight; y++) {
		float sy = (y + 0.5f) * stepY - 0.5f;
		if (sy < 0)
			sy = 0;
		int32 y0 = (int32)sy;
		if (y0 > maxY)
			y0 = maxY;
		int32 y1 = y0 < maxY ? y0 + 1 : y0;
		float fy = sy - y0;
		if (fy > 1)
			fy = 1;

		int64 rowIndex = ((int64)(destTop + y) * width + destLeft) * channels;

		for (int32 x = 0; x < destWidth; x++) {
			float sx = (x + 0.5f) * stepX - 0.5f;
			if (sx < 0)
				sx = 0;
			int32 x0 = (int32)sx;
			if (x0 > maxX)
				x0 = maxX;
			int32 x1 = x0 < maxX ? x0 + 1 : x0;
			float fx = sx - x0;
			if (fx > 1)
				fx = 1;

			float p00[3], p01[3], p10[3], p11[3];
			reader.Read(sourceLeft + x0, sourceTop + y0, p00);
			reader.Read(sourceLeft + x1, sourceTop + y0, p01);
			reader.Read(sourceLeft + x0, sourceTop + y1, p10);
			reader.Read(sourceLeft + x1, sourceTop + y1, p11);

			float rgb[3];
			for (int32 c = 0; c < 3; c++) {
				float top = p00[c] + (p01[c] - p00[c]) * fx;
				float bottom = p10[c] + (p11[c] - p10[c]) * fx;
				rgb[c] = top + (bottom - top) * fy;
			}

			int64 index = rowIndex + (int64)x * channels;
			if (channels == 1) {
				// luma, as the Interface Kit computes it
				float gray = 0.299f * rgb[0] + 0.587f * rgb[1]
					+ 0.114f * rgb[2];
				store_pixel(data, type, index, 0, gray, options);
			} else if (options.bgr) {
				store_pixel(data, type, index, 0, rgb[2], options);
				store_pixel(data, type, index + 1, 1, rgb[1], options);
				store_pixel(data, type, index + 2, 2, rgb[0], options);
			} else {
				store_pixel(data, type, index, 0, rgb[0], options);
				store_pixel(data, type, index + 1, 1, rgb[1], options);
				store_pixel(data, type, index + 2, 2, rgb[2], options);
			}
		}
	}

	if (_mapping != NULL) {
		_mapping->source = source;
		_mapping->destination = destination;
		_mapping->tensor_width = width;
		_mapping->tensor_height = height;
	}

	return B_OK;
}


/*static*/ status_t
BNeuralUtils::GetBitmapFromTensor(const BTensor& tensor, BBitmap** _bitmap,
	const neural_image_options* _options)
{
	if (_bitmap == NULL)
		return B_BAD_VALUE;

	int32 width, height, channels;
	status_t status = image_layout(tensor, &width, &height, &channels);
	if (status != B_OK)
		return status;

	neural_image_options options;
	if (_options != NULL)
		options = *_options;

	BBitmap* bitmap = new(std::nothrow) BBitmap(
		BRect(0, 0, width - 1, height - 1), B_RGB32);
	if (bitmap == NULL || bitmap->InitCheck() != B_OK) {
		status = bitmap != NULL ? bitmap->InitCheck() : B_NO_MEMORY;
		delete bitmap;
		return status;
	}

	const void* data = tensor.Data();
	neural_data_type type = tensor.Type();
	uint8* bits = (uint8*)bitmap->Bits();
	int32 bytesPerRow = bitmap->BytesPerRow();

	for (int32 y = 0; y < height; y++) {
		uint8* row = bits + (size_t)y * bytesPerRow;
		for (int32 x = 0; x < width; x++) {
			int64 index = ((int64)y * width + x) * channels;
			uint8 values[3];
			for (int32 c = 0; c < channels; c++) {
				float value;
				switch (type) {
					case B_NEURAL_UINT8:
						value = ((const uint8*)data)[index + c];
						break;
					case B_NEURAL_INT8:
						value = ((const int8*)data)[index + c] + 128;
						break;
					default:
						value = ((const float*)data)[index + c]
							* options.deviation[c] + options.mean[c];
						break;
				}
				if (!(value > 0))
					value = 0;
				else if (value > 255)
					value = 255;
				values[c] = (uint8)(value + 0.5f);
			}
			if (channels == 1)
				values[1] = values[2] = values[0];
			else if (options.bgr) {
				uint8 swap = values[0];
				values[0] = values[2];
				values[2] = swap;
			}

			row[x * 4] = values[2];
			row[x * 4 + 1] = values[1];
			row[x * 4 + 2] = values[0];
			row[x * 4 + 3] = 255;
		}
	}

	*_bitmap = bitmap;
	return B_OK;
}


/*static*/ status_t
BNeuralUtils::GetLabels(const entry_ref* ref, BStringList* labels)
{
	if (ref == NULL)
		return B_BAD_VALUE;

	BPath path(ref);
	if (path.InitCheck() != B_OK)
		return path.InitCheck();
	return GetLabels(path.Path(), labels);
}


/*static*/ status_t
BNeuralUtils::GetLabels(const char* path, BStringList* labels)
{
	if (path == NULL || labels == NULL)
		return B_BAD_VALUE;

	BFile file(path, B_READ_ONLY);
	status_t status = file.InitCheck();
	if (status != B_OK)
		return status;

	off_t size;
	status = file.GetSize(&size);
	if (status != B_OK)
		return status;
	if (size > 16 * 1024 * 1024)
		return B_BAD_DATA;

	BString contents;
	char* buffer = contents.LockBuffer(size);
	if (buffer == NULL)
		return B_NO_MEMORY;
	ssize_t bytesRead = file.Read(buffer, size);
	contents.UnlockBuffer(bytesRead > 0 ? bytesRead : 0);
	if (bytesRead < 0)
		return bytesRead;

	labels->MakeEmpty();
	contents.ReplaceAll("\r", "");
	contents.Split("\n", false, *labels);

	// a file that ends with a new line has no last, empty label
	if (labels->CountStrings() > 0
		&& labels->StringAt(labels->CountStrings() - 1).IsEmpty()) {
		labels->Remove(labels->CountStrings() - 1);
	}
	return B_OK;
}


/*static*/ int32
BNeuralUtils::GetTopValues(const BTensor& tensor, int32 count, int64* indices,
	float* values)
{
	if (tensor.InitCheck() != B_OK)
		return tensor.InitCheck();
	if (count < 0 || indices == NULL)
		return B_BAD_VALUE;

	int64 elements = tensor.CountElements();
	if (count > elements)
		count = (int32)elements;

	float* best = (float*)malloc((count > 0 ? count : 1) * sizeof(float));
	if (best == NULL)
		return B_NO_MEMORY;

	int32 found = 0;
	for (int64 i = 0; i < elements; i++) {
		float value = tensor.ValueAt(i);
		if (found == count && (count == 0 || value <= best[found - 1]))
			continue;

		// insert, keeping the list sorted; equal values keep their order
		int32 position = found < count ? found : count - 1;
		while (position > 0 && best[position - 1] < value) {
			best[position] = best[position - 1];
			indices[position] = indices[position - 1];
			position--;
		}
		best[position] = value;
		indices[position] = i;
		if (found < count)
			found++;
	}

	if (values != NULL)
		memcpy(values, best, found * sizeof(float));
	free(best);
	return found;
}


/*static*/ bool
BNeuralUtils::IsDetector(const BNeuralSession& session)
{
	if (session.InitCheck() != B_OK || session.CountOutputs() != 4)
		return false;

	const BTensor* boxes = session.OutputAt(0);
	const BTensor* classes = session.OutputAt(1);
	const BTensor* scores = session.OutputAt(2);
	const BTensor* count = session.OutputAt(3);
	return boxes->Type() == B_NEURAL_FLOAT32 && boxes->Rank() == 3
		&& boxes->DimensionAt(2) == 4
		&& classes->Type() == B_NEURAL_FLOAT32
		&& scores->Type() == B_NEURAL_FLOAT32
		&& classes->CountElements() == boxes->DimensionAt(1)
		&& scores->CountElements() == boxes->DimensionAt(1)
		&& count->Type() == B_NEURAL_FLOAT32 && count->CountElements() == 1;
}


/*static*/ int32
BNeuralUtils::GetDetections(const BNeuralSession& session,
	neural_detection* detections, int32 maxCount)
{
	if (!IsDetector(session))
		return B_BAD_TYPE;
	if (detections == NULL || maxCount < 0)
		return B_BAD_VALUE;

	const float* boxes = (const float*)session.OutputAt(0)->Data();
	const float* classes = (const float*)session.OutputAt(1)->Data();
	const float* scores = (const float*)session.OutputAt(2)->Data();
	float found = *(const float*)session.OutputAt(3)->Data();

	int64 available = session.OutputAt(1)->CountElements();
	int32 count = found > 0 ? (int32)fminf(found, (float)available) : 0;
	if (count > maxCount)
		count = maxCount;

	// boxes are top, left, bottom, right
	for (int32 i = 0; i < count; i++) {
		detections[i].top = boxes[i * 4];
		detections[i].left = boxes[i * 4 + 1];
		detections[i].bottom = boxes[i * 4 + 2];
		detections[i].right = boxes[i * 4 + 3];
		detections[i].class_index = (int32)classes[i];
		detections[i].score = scores[i];
	}
	return count;
}
