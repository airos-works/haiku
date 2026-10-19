/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef COLOR_NAMES_H
#define COLOR_NAMES_H


#include <SupportDefs.h>


//! Looks up an X11 color name (case insensitive), as 0xRRGGBB.
bool lookup_color_name(const char* name, uint32& _color);


#endif	// COLOR_NAMES_H
