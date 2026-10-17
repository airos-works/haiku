/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef SOURCE_INFO_H
#define SOURCE_INFO_H


#include <String.h>
#include <SupportDefs.h>

#include <vector>

#include "DiscSource.h"

extern "C" {
#include <libavcodec/codec_id.h>
#include <libavutil/rational.h>
}


class BBitmap;


/*!	What a source file holds, as the Source panel shows it and as the
	transcoder decides from it: one entry per stream, in file order. */


struct VideoTrackInfo {
	int				index;			// the stream's index in the file
	AVCodecID		codec;
	BString			codecName;
	BString			profile;
	int				level;
	int				width;
	int				height;
	AVRational		sampleAspect;	// 0/1 when unknown
	AVRational		frameRate;
	bool			variableFrameRate;
	BString			pixelFormat;
	int				bitDepth;
	BString			chroma;			// "4:2:0"
	BString			colorPrimaries;
	BString			transfer;
	BString			matrix;
	bool			fullRange;
	BString			dynamicRange;	// "SDR", "HDR10", "HLG", "Dolby Vision"
	bool			interlaced;
	int				rotation;		// degrees, clockwise
	int64			bitRate;
	BString			language;
	BString			title;

					VideoTrackInfo();
};


struct AudioTrackInfo {
	int				index;
	AVCodecID		codec;
	BString			codecName;
	BString			profile;
	int				channels;
	BString			layout;			// "5.1(side)"
	int				sampleRate;
	int				bitDepth;		// 0 if the format has none
	int64			bitRate;
	BString			language;
	BString			title;
	bool			isDefault;
	bool			commentary;

					AudioTrackInfo();
};


struct SubtitleTrackInfo {
	int				index;
	AVCodecID		codec;
	BString			codecName;
	bool			text;			// text or pictures
	BString			language;
	BString			title;
	bool			isDefault;
	bool			forced;
	BString			file;			// a subtitle file beside the source

					SubtitleTrackInfo();
};


struct ChapterInfo {
	BString			title;
	bigtime_t		start;
	bigtime_t		end;
};


struct MetadataEntry {
	BString			key;
	BString			value;
};


struct SourceInfo {
	BString			path;
	BString			name;
	off_t			fileSize;
	BString			containerName;
	bigtime_t		duration;
	int64			bitRate;

	std::vector<VideoTrackInfo>		video;
	std::vector<AudioTrackInfo>		audio;
	std::vector<SubtitleTrackInfo>	subtitles;
	std::vector<ChapterInfo>		chapters;
	std::vector<MetadataEntry>		metadata;

	// a DVD or Blu-ray title: the disc's titles and which one this is
	// (path is then a disc source string, see DiscSource.h)
	std::vector<DiscTitle>			discTitles;
	int32			discTitle;		// -1: not a disc
	int32			discMainTitle;

					SourceInfo();

	bool			HasVideo() const { return !video.empty(); }
	// a song: sound and no pictures (album artwork is not video)
	bool			IsSoundOnly() const
						{ return video.empty() && !audio.empty(); }
	const VideoTrackInfo* MainVideo() const
						{ return video.empty() ? NULL : &video[0]; }
	BString			Metadata(const char* key) const;
	bool			IsDisc() const { return discTitle >= 0; }
};


#endif	// SOURCE_INFO_H
