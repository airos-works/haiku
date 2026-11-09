/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef PICTURE_VIEW_H
#define PICTURE_VIEW_H


#include <Message.h>
#include <String.h>
#include <View.h>

#include "Snapshot.h"


/*!	Shows the picture, as large as fits with its proportions kept, and on
	top of it what a detection model found: a box, a name and a score per
	thing. Without a picture it shows a line of text that says why. */
class PictureView : public BView {
public:
								PictureView();
	virtual						~PictureView();

			void				SetFrame(Snapshot* frame);
	//! A MSG_ENGINE_RESULT message, or NULL for nothing.
			void				SetResult(const BMessage* result);
	/*!	Text for the middle of the view: instead of a picture, or on a
		plate over the picture. NULL or empty for none. */
			void				SetNote(const char* note);

	virtual	void				Draw(BRect updateRect);
	virtual	BSize				MinSize();
	virtual	BSize				PreferredSize();
	virtual	BSize				MaxSize();

	//! The color things of a class are marked with.
	static	rgb_color			ColorFor(int32 classIndex);
	static	rgb_color			TextColorOn(rgb_color color);

private:
			BRect				_PictureRect() const;
			void				_DrawBoxes(BRect pictureRect);
			BRect				_BoxRect(BRect box, BRect pictureRect) const;
			void				_DrawNote(bool plate);

private:
			SnapshotReference	fFrame;
			BMessage			fResult;
			BString				fNote;
};


#endif	// PICTURE_VIEW_H
