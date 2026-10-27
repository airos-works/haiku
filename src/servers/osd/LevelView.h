/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef LEVEL_VIEW_H
#define LEVEL_VIEW_H


#include <String.h>
#include <View.h>

#include <OnScreenDisplay.h>


class LevelView : public BView {
public:
								LevelView();

			void				SetLevel(const BMessage* message);

	virtual	void				Draw(BRect updateRect);
	virtual	void				MouseDown(BPoint where);
	virtual	void				GetPreferredSize(float* _width,
									float* _height);

private:
			float				_Scaled(float value) const;
			void				_DrawFrame(BRect frame, rgb_color base);
			void				_DrawMeter(BRect frame, rgb_color base);

private:
			float				fScale;
			osd_level_kind		fKind;
			float				fLevel;
			bool				fMuted;
			BString				fLabel;
			BString				fValue;
};


#endif	// LEVEL_VIEW_H
