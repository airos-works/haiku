/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _NEURAL_UTILS_H
#define _NEURAL_UTILS_H


#include <Rect.h>
#include <neural/NeuralDefs.h>
#include <neural/Tensor.h>

class BBitmap;
class BStringList;
struct entry_ref;


//! How a picture is fitted into a tensor of another shape.
enum neural_image_fit {
	//! scaled to fill the tensor; the proportions are not kept
	B_NEURAL_FIT_STRETCH = 0,
	//! scaled to cover the tensor; what sticks out is cut off evenly
	B_NEURAL_FIT_CROP,
	//! scaled to fit inside the tensor; the rest is filled with gray
	B_NEURAL_FIT_LETTERBOX
};


struct neural_image_options {
								neural_image_options();

	neural_image_fit	fit;
	//! store blue, green, red instead of red, green, blue
	bool				bgr;
	/*!	Float tensors get (pixel - mean) / deviation per channel, pixel
		being 0 .. 255; the default of 127.5 for both gives -1 .. 1.
		Quantized 8 bit tensors get the pixel values as they are (int8:
		less 128), as models with such inputs expect. */
	float				mean[3];
	float				deviation[3];
	uint32				_reserved[4];
};


/*!	Where the picture went, to find the way back from what a model says
	about its input (a box, a point) to the bitmap. */
struct neural_image_mapping {
	//! the part of the bitmap that was used, in bitmap coordinates
	BRect				source;
	//! where it was put, in tensor pixels
	BRect				destination;
	int32				tensor_width;
	int32				tensor_height;

	/*!	Turns a box given in fractions of the tensor (0 .. 1, as detection
		models give them) into bitmap coordinates. */
	BRect				ToBitmap(float left, float top, float right,
							float bottom) const;
};


/*!	Conveniences that tie tensors to the rest of the system. */
class BNeuralUtils {
public:
	/*!	Fills an image tensor ([1, height, width, 3 or 1] or [height,
		width, channels], uint8, int8 or float32) from a bitmap of any
		size and of the color spaces B_RGB32, B_RGBA32, B_RGB24, B_RGB16,
		B_RGB15, B_GRAY8 and B_CMAP8, scaling with bilinear filtering.
		\a source selects part of the bitmap (NULL: all of it). */
	static	status_t			SetTensorFromBitmap(BTensor* tensor,
									const BBitmap* bitmap,
									const BRect* source = NULL,
									const neural_image_options* options
										= NULL,
									neural_image_mapping* _mapping = NULL);

	/*!	Makes a B_RGB32 bitmap of an image tensor (the reverse of the
		above with the same options; one channel becomes gray). The
		caller owns the bitmap. */
	static	status_t			GetBitmapFromTensor(const BTensor& tensor,
									BBitmap** _bitmap,
									const neural_image_options* options
										= NULL);

	/*!	Reads a label file: one name per line, as shipped with
		classification and detection models. */
	static	status_t			GetLabels(const entry_ref* ref,
									BStringList* labels);
	static	status_t			GetLabels(const char* path,
									BStringList* labels);

	/*!	The \a count largest elements of a tensor, best first: their
		indices and, if wanted, their real values. Returns how many were
		found, or an error. */
	static	int32				GetTopValues(const BTensor& tensor,
									int32 count, int64* indices,
									float* values = NULL);

private:
								BNeuralUtils();
};


#endif	// _NEURAL_UTILS_H
