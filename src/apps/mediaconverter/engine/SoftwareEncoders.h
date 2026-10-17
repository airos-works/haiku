/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef SOFTWARE_ENCODERS_H
#define SOFTWARE_ENCODERS_H


#include "VideoEncoder.h"


/*!	HandBrake's encoders for the formats FFmpeg cannot make here: x264 for
	H.264, x265 for H.265 and SVT-AV1 for AV1. The libraries are opened when
	first needed (packages x264, x265 and svt_av13), so MediaConverter starts
	without them and says which one is missing. */
namespace SoftwareEncoders {

bool			IsAvailable(video_codec codec, BString* missing = NULL);
VideoEncoder*	Create(video_codec codec);
// "x264 0.164", for the hardware window
BString			Version(software_encoder encoder);

}	// namespace SoftwareEncoders


#endif	// SOFTWARE_ENCODERS_H
