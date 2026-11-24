/*
 * Copyright 2021-2022, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Augustin Cavalier <waddlesplash>
 *		John Scipione <jscipione@gmail.com>
 */
#ifndef _TRACKER_THUMBNAILS_H
#define _TRACKER_THUMBNAILS_H


#include <Bitmap.h>
#include <Mime.h>
#include <Point.h>


class BView;

namespace BPrivate {


class Model;


status_t GetThumbnailFromAttr(Model* model, BBitmap* icon, BSize size);
// The same for an icon that is drawn in view at where: if a thumbnail has
// to be made, that waits for its turn, and the icon is no longer in view by
// then (scrolled away, window closed), it is not made.
status_t GetThumbnailFromAttr(Model* model, BBitmap* icon, BSize size,
	BView* view, BPoint where);
bool ShouldGenerateThumbnail(const char* type);

// Whether no thumbnail could be made of the file as it is now, and it has
// no icon of its own: its type's icon is the one to draw, without opening
// it again.
bool ThumbnailFailed(const Model* model);


} // namespace BPrivate

using namespace BPrivate;


#endif	// _TRACKER_THUMBNAILS_H
