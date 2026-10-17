/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef MEDIA_PROBE_H
#define MEDIA_PROBE_H


#include "SourceInfo.h"


class BBitmap;


/*!	Reads a file's streams, chapters and tags with libavformat, and decodes
	single pictures for the preview. */
namespace MediaProbe {

status_t	Probe(const char* path, SourceInfo& info, BString* error);

// A picture from `position` (0..1 of the duration) scaled to fit
// maxWidth x maxHeight, as B_RGB32; NULL when the file has no video.
BBitmap*	Preview(const char* path, float position, int maxWidth,
				int maxHeight);
// The album artwork of a song (the front cover when it says which picture
// that is), or else the album's cover.jpg, folder.jpg ... beside it, scaled
// to fit; NULL when there is none.
BBitmap*	CoverArt(const char* path, int maxWidth, int maxHeight);
// The album's picture beside a song, as music libraries keep it: cover.jpg,
// folder.jpg, front.jpg and the like (JPEG or PNG); empty when there is none.
BString		FolderCover(const char* path);

BString		FormatDuration(bigtime_t duration);
BString		FormatSize(off_t size);
BString		FormatBitRate(int64 bitsPerSecond);
BString		LanguageName(const char* code);

}	// namespace MediaProbe


#endif	// MEDIA_PROBE_H
