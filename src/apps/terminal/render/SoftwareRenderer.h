/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef SOFTWARE_RENDERER_H
#define SOFTWARE_RENDERER_H


#include <OS.h>

#include <atomic>
#include <vector>

#include "TermRenderer.h"


/*!	Draws frames with the processor. A large frame (scrolling) is shared
	out row by row among a few threads.
*/
class SoftwareRenderer : public TermRenderer {
public:
								SoftwareRenderer();
	virtual						~SoftwareRenderer();

	virtual	const char*			Name() const	{ return "software"; }

	virtual	bool				Render(const RenderFrame& frame,
									const GlyphAtlasInfo& atlas,
									uint8* bits, int32 bytesPerRow,
									int32& _firstRow, int32& _lastRow);

private:
	static	const int64			kShareMinimum = 512 * 1024;	// pixels
	static	const int32			kRowsAtOnce = 2;

			void				_DrawRows();
	static	status_t			_HelperEntry(void* data);

private:
			std::vector<thread_id> fHelpers;
			sem_id				fStart;
			sem_id				fDone;
			bool				fQuit;

			// the frame being drawn
			const RenderFrame*	fFrame;
			const GlyphAtlasInfo* fAtlas;
			uint32*				fBits;
			int32				fPixelsPerRow;
			std::vector<int32>	fRows;
			std::atomic<int32>	fNextRow;
};


// shared with the OpenGL renderer, which draws decorations the same way
void render_decorations(const RenderFrame& frame, const RenderCell& cell,
	uint32* pixels, int32 pixelsPerRow, int32 clipTop, int32 clipBottom);
void render_cursor(const RenderFrame& frame, const RenderCursor& cursor,
	uint32* bits, int32 pixelsPerRow);
void render_images(const RenderFrame& frame, uint32* bits,
	int32 pixelsPerRow, int32 top, int32 bottom, int32 minimumZ,
	int32 maximumZ);


#endif	// SOFTWARE_RENDERER_H
