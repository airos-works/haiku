/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef MEDIA_CONVERTER_APP_H
#define MEDIA_CONVERTER_APP_H


#include <Application.h>


class MediaConverterWindow;


extern const char* kAppSignature;


class MediaConverterApp : public BApplication {
public:
								MediaConverterApp();
	virtual						~MediaConverterApp();

	virtual	void				ReadyToRun();
	virtual	void				RefsReceived(BMessage* message);
	virtual	void				ArgvReceived(int32 argc, char** argv);
	virtual	bool				QuitRequested();
	virtual	void				AboutRequested();

private:
			MediaConverterWindow* fWindow;
			BMessage			fPendingRefs;
};


#endif	// MEDIA_CONVERTER_APP_H
