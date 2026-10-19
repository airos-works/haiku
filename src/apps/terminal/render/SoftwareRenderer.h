/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef SOFTWARE_RENDERER_H
#define SOFTWARE_RENDERER_H


#include "TermRenderer.h"


//! Draws frames with the processor, where there is no OpenGL.
class SoftwareRenderer : public TermRenderer {
public:
	virtual	const char*			Name() const	{ return "software"; }

	virtual	bool				Render(const RenderFrame& frame,
									const GlyphAtlasInfo& atlas,
									uint8* bits, int32 bytesPerRow,
									int32& _firstRow, int32& _lastRow);
};


// shared with the OpenGL renderer, which draws decorations the same way
void render_decorations(const RenderFrame& frame, const RenderCell& cell,
	uint32* pixels, int32 pixelsPerRow, int32 clipTop, int32 clipBottom);
void render_cursor(const RenderFrame& frame, uint32* bits,
	int32 pixelsPerRow);
void render_images(const RenderFrame& frame, uint32* bits,
	int32 pixelsPerRow, int32 top, int32 bottom, int32 minimumZ,
	int32 maximumZ);


#endif	// SOFTWARE_RENDERER_H
