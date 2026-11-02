/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef CAPTION_WINDOW_H
#define CAPTION_WINDOW_H


#include <Window.h>


class CaptionView;


/*!	Shows what the screen reader says at the bottom of the screen. It never
	takes the focus, so it does not disturb what it describes.
*/
class CaptionWindow : public BWindow {
public:
								CaptionWindow();

	virtual	void				MessageReceived(BMessage* message);

			void				AddCaption(const char* text);
									// any thread

private:
			void				_Place();

			CaptionView*		fView;
};


#endif	// CAPTION_WINDOW_H
