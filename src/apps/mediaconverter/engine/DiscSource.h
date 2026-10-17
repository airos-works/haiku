/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef DISC_SOURCE_H
#define DISC_SOURCE_H


#include <String.h>
#include <SupportDefs.h>

#include <vector>


struct AVFormatContext;


/*!	DVD-Video and Blu-ray titles as sources, as HandBrake opens them: a disc
	folder (one holding VIDEO_TS or BDMV, or that folder itself) or a DVD
	image. One title of a disc is named by a source string,
	"disc:<title>:<path>" (title -1: the main one), and the engine opens
	every source with open_source(), so a title goes wherever a file goes.
	Encrypted discs (CSS, AACS) cannot be read. */


enum disc_kind {
	kDiscNone = 0,
	kDiscDVD,
	kDiscBluray
};


struct DiscTitle {
	int32		number;		// DVD title 1-99, Blu-ray playlist 0-99999
	bigtime_t	duration;
	int32		chapters;
	int32		angles;
};


// What a file or folder is: a DVD folder or image, a Blu-ray folder, or
// neither.
disc_kind		identify_disc(const char* path);
// "Movie" for Movie/VIDEO_TS, Movie/BDMV, Movie and Movie.iso
BString			disc_name(const char* path);
// The folder the disc is in, where "next to the source" puts the outputs.
BString			disc_folder(const char* path);
// The titles longer than a few seconds, in disc order; *mainTitle is the
// longest one's number.
status_t		list_disc_titles(const char* path,
					std::vector<DiscTitle>& titles, int32* mainTitle,
					BString* error);

BString			disc_source(const char* path, int32 title);
bool			parse_disc_source(const char* source, BString* path,
					int32* title);

// avformat_open_input() and avformat_find_stream_info() on a file or a
// disc title; for a title the languages, chapters, duration and subtitle
// palettes come from the disc. Returns a negative AVERROR on failure.
int				open_source(AVFormatContext** context, const char* source,
					BString* error = NULL);
void			close_source(AVFormatContext** context);
// The bytes a source reads: a file's size, a title's on its disc.
int64			source_size(AVFormatContext* context);


#endif	// DISC_SOURCE_H
