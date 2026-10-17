/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef SOURCE_VIEW_H
#define SOURCE_VIEW_H


#include <View.h>

#include "SourceInfo.h"


class BBitmap;
class BColumnListView;
class BStringView;
class BTextView;
class PreviewView;


/*!	What the selected source holds: a picture from it, a summary, and one
	line per track, much as HandBrake's summary shows them. */
class SourceView : public BView {
public:
								SourceView();
	virtual						~SourceView();

			void				SetSource(const SourceInfo* info,
									int32 itemID);
			int32				ItemID() const { return fItemID; }
			// takes the bitmap
			void				SetPreview(BBitmap* bitmap, int32 itemID);

private:
			void				_AddTracks(const SourceInfo& info);

			PreviewView*		fPreview;
			BStringView*		fTitle;
			BStringView*		fSummary;
			BStringView*		fExtra;
			BColumnListView*	fTracks;
			int32				fItemID;
};


BString	format_level(AVCodecID codec, int level);
BString	format_frame_rate(AVRational rate);


#endif	// SOURCE_VIEW_H
