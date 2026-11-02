/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef MEDIA_ICONS_H
#define MEDIA_ICONS_H


#include <GraphicsDefs.h>
#include <Rect.h>


class BView;


enum transport_symbol {
	kSymbolPlay,
	kSymbolPause,
	kSymbolNext,
	kSymbolPrevious
};


// The tray icon: two beamed notes, in the style of the system's icons.
void DrawNoteIcon(BView* view, BRect frame, bool dimmed);

// A flat transport symbol for a button, centered in the frame.
void DrawTransportSymbol(BView* view, BRect frame, transport_symbol symbol,
	rgb_color color);


#endif	// MEDIA_ICONS_H
