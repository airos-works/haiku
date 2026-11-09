/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef RESULTS_VIEW_H
#define RESULTS_VIEW_H


#include <Message.h>
#include <String.h>
#include <View.h>


/*!	Lists what the model said about the picture, best first: the five
	most likely classes of a classification model, or the things a
	detection model found. Each line has a bar as long as the model is
	sure. The view is always five lines high, so that the picture does not
	jump when the model changes. */
class ResultsView : public BView {
public:
								ResultsView();
	virtual						~ResultsView();

	//! A MSG_ENGINE_RESULT message, or NULL for nothing.
			void				SetResult(const BMessage* result);

	virtual	void				Draw(BRect updateRect);
	virtual	BSize				MinSize();
	virtual	BSize				PreferredSize();
	virtual	BSize				MaxSize();

	static	const int32			kRows = 5;

private:
			float				_RowHeight() const;
			float				_Inset() const;

private:
			BMessage			fResult;
			bool				fHasResult;
};


#endif	// RESULTS_VIEW_H
