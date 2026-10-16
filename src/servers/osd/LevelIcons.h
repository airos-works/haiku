/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef LEVEL_ICONS_H
#define LEVEL_ICONS_H


#include <Rect.h>


class BView;


void	DrawSunIcon(BView* view, BRect frame);
void	DrawSpeakerIcon(BView* view, BRect frame, float level, bool muted);


#endif	// LEVEL_ICONS_H
