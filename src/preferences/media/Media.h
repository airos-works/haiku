/*
 * Copyright 2003-2026, Haiku. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors in chronological order:
 *		Sikosis
 *		Jérôme Duval
 */
#ifndef MEDIA_H
#define MEDIA_H


#include <Application.h>
#include <Message.h>


class MediaWindow;


status_t load_media_settings(BMessage& settings);
status_t save_media_settings(const BMessage& settings);


class Media : public BApplication {
public:
								Media();

	virtual	void				ReadyToRun();

private:
			MediaWindow*		fWindow;
};


#endif	// MEDIA_H
