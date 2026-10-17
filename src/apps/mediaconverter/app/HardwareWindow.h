/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef HARDWARE_WINDOW_H
#define HARDWARE_WINDOW_H


#include <Window.h>


class BColumnListView;
class BGroupLayout;


/*!	What each graphics card's video engines can do here, next to the CPU's
	encoders: one row per format. */
class HardwareWindow : public BWindow {
public:
								HardwareWindow(BRect frame);

	virtual	void				MessageReceived(BMessage* message);

private:
			void				_Fill();

			BColumnListView*	fMatrix;
			BGroupLayout*		fDevices;
};


#endif	// HARDWARE_WINDOW_H
