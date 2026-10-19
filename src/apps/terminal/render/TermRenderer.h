/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef TERM_RENDERER_H
#define TERM_RENDERER_H


#include "RenderFrame.h"


/*!	Draws frames into memory: a B_RGB32 buffer of frame.width x
	frame.height pixels (bytes in B, G, R, A order). Only the rows marked
	dirty need to be drawn; the rest of the buffer keeps what was drawn
	before.

	The OpenGL renderer is an add-on (Terminal works where there is no
	OpenGL); it exports create_terminal_renderer().
*/
class TermRenderer {
public:
	virtual						~TermRenderer() {}

	virtual	const char*			Name() const = 0;

	//! Whether the renderer cannot draw any more (the GPU was lost): the
	//! caller then draws with the software renderer.
	virtual	bool				IsBroken() const	{ return false; }

	//! Draws the dirty rows; returns the rows drawn (first, last
	//! inclusive), or false if nothing was drawn.
	virtual	bool				Render(const RenderFrame& frame,
									const GlyphAtlasInfo& atlas,
									uint8* bits, int32 bytesPerRow,
									int32& _firstRow, int32& _lastRow) = 0;
};


extern "C" {
	typedef TermRenderer* (*create_terminal_renderer_func)(const char** error);
}

#define TERMINAL_RENDERER_ENTRY "create_terminal_renderer"


#endif	// TERM_RENDERER_H
