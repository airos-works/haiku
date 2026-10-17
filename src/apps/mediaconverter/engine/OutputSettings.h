/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef OUTPUT_SETTINGS_H
#define OUTPUT_SETTINGS_H


#include <Message.h>
#include <String.h>

#include "Formats.h"


enum encoder_preference {
	kPreferHardware = 0,	// a GPU when one can make the format, else the CPU
	kPreferSoftware,		// always the CPU
};


enum rate_mode {
	kRateConstantQuality = 0,
	kRateAverageBitrate,
};


enum speed_preset {
	kSpeedFastest = 0,
	kSpeedFast,
	kSpeedBalanced,
	kSpeedSlow,
	kSpeedSlowest,

	kSpeedCount
};


enum deinterlace_mode {
	kDeinterlaceOff = 0,
	kDeinterlaceAuto,		// only pictures the source marks as interlaced
	kDeinterlaceAlways,
};


enum filter_strength {
	kFilterOff = 0,
	kFilterLight,
	kFilterMedium,
	kFilterStrong,
};


enum audio_track_selection {
	kAudioTracksFirst = 0,
	kAudioTracksAll,
	kAudioTracksDefault,	// the track the file marks as default
};


/*!	Everything the output panel sets. A preset is one of these with a name;
	so is the window's state that is kept between runs. */
struct OutputSettings {
	container_kind		container;

	video_codec			videoCodec;
	encoder_preference	encoderPreference;
	BString				gpu;			// device id of a chosen GPU, or empty
	rate_mode			rateMode;
	int32				quality;		// 0 (smallest) .. 100 (best)
	int32				videoBitrate;	// kbit/s
	speed_preset		speed;
	int32				maxHeight;		// 0: as the source
	int32				frameRate;		// fps * 1000, 0: as the source
	bool				peakFrameRate;	// limit, rather than force, the rate

	deinterlace_mode	deinterlace;
	filter_strength		denoise;
	filter_strength		sharpen;
	bool				grayscale;
	bool				autoCrop;
	int32				rotation;		// extra, clockwise: 0, 90, 180, 270
	bool				flip;

	audio_codec			audioCodec;
	int32				audioBitrate;	// kbit/s for a stereo pair
	audio_mixdown		mixdown;
	int32				sampleRate;		// 0: as the source
	audio_track_selection audioTracks;
	bool				passthroughWhenPossible;

	subtitle_mode		subtitles;
	bool				copyChapters;
	bool				copyMetadata;

	bigtime_t			start;			// range, 0..0 for all of it
	bigtime_t			end;

						OutputSettings();

			status_t	Archive(BMessage& archive) const;
			void		Unarchive(const BMessage& archive);

	// Fix up combinations that cannot be written, e.g. a codec the
	// container cannot hold. Returns true when something changed.
			bool		Validate();

			bool		operator==(const OutputSettings& other) const;
			bool		operator!=(const OutputSettings& other) const
							{ return !(*this == other); }
};


const char* speed_name(speed_preset speed);


#endif	// OUTPUT_SETTINGS_H
