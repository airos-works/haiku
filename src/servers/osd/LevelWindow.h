/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef LEVEL_WINDOW_H
#define LEVEL_WINDOW_H


#include <Window.h>


class BMessageRunner;
class LevelView;


class LevelWindow : public BWindow {
public:
								LevelWindow();
	virtual						~LevelWindow();

	virtual	void				MessageReceived(BMessage* message);

private:
			void				_ShowLevel(const BMessage* message);
			void				_MoveIntoPlace();

private:
			LevelView*			fView;
			BMessageRunner*		fHideRunner;
};


#endif	// LEVEL_WINDOW_H
