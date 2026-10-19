/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef BOX_DRAWING_H
#define BOX_DRAWING_H


#include <SupportDefs.h>


//!	Whether the character is drawn by draw_box_character() rather than
//!	taken from the font.
bool is_box_character(uint32 c);

/*!	Draws a box drawing, block element, braille or powerline character as
	coverage (0-255) into a cell of \a width x \a height pixels, so lines
	meet exactly from cell to cell. \a lineWidth is the thickness of a
	light line.
*/
bool draw_box_character(uint32 c, uint8* bits, int32 bytesPerRow,
	int32 width, int32 height, int32 lineWidth);


#endif	// BOX_DRAWING_H
