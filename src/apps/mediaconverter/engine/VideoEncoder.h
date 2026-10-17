/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef VIDEO_ENCODER_H
#define VIDEO_ENCODER_H


#include <String.h>
#include <SupportDefs.h>

#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

#include "Formats.h"
#include "OutputSettings.h"


/*!	What an encoder is asked to make. The transcoder fills it from the
	output settings and the filtered source. */
struct VideoEncoderParams {
	video_codec				codec;
	int						width;
	int						height;
	AVRational				timeBase;		// of the frames' pts
	AVRational				frameRate;
	bool					constantFrameRate;
	AVRational				sampleAspect;
	rate_mode				rateMode;
	int32					quality;		// 0..100
	int32					bitrate;		// kbit/s
	speed_preset			speed;
	int						keyInterval;	// pictures between key frames
	bool					globalHeader;	// the container wants extradata

	AVColorPrimaries		primaries;
	AVColorTransferCharacteristic transfer;
	AVColorSpace			matrix;
	AVColorRange			range;
	AVChromaLocation		chromaLocation;
	bool					haveMastering;
	AVMasteringDisplayMetadata mastering;
	bool					haveLightLevel;
	AVContentLightMetadata	lightLevel;

							VideoEncoderParams();
};


/*!	An encoder in the libavcodec manner: frames in, packets out. The frames
	must be in InputFormat(). Packets carry pts and dts in the params' time
	base, with AV_PKT_FLAG_KEY on random access points. */
class VideoEncoder {
public:
	virtual						~VideoEncoder();

	virtual	status_t			Open(const VideoEncoderParams& params,
									BString& error) = 0;
	virtual	AVPixelFormat		InputFormat() const = 0;

	// frame == NULL drains the encoder
	virtual	status_t			SendFrame(const AVFrame* frame) = 0;
	// B_WOULD_BLOCK: send more; B_LAST_BUFFER_ERROR: drained
	virtual	status_t			ReceivePacket(AVPacket* packet) = 0;

	// Parameter sets in Annex B (H.264, H.265) or the sequence header OBU
	// (AV1), or the codec's own extradata; empty if there is none.
	virtual	const std::vector<uint8>& Extradata() const;

	virtual	encoder_backend		Backend() const = 0;
	// "x264", "AMD Radeon Pro WX 5100 (VA-API)"
	virtual	BString				Name() const = 0;
	// Settings the encoder chose, for the log: "CQP 24, IPPP, 1 reference"
	virtual	BString				Description() const;

protected:
			std::vector<uint8>	fExtradata;
};


// Quality 0..100 as a codec's own scale, 0 being the best quality.
int		quality_to_crf(int32 quality, int best, int worst);
// Keep the encoders' threads off the user interface's toes.
int		encoder_thread_count();


#endif	// VIDEO_ENCODER_H
