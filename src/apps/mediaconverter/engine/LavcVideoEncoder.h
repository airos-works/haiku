/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef LAVC_VIDEO_ENCODER_H
#define LAVC_VIDEO_ENCODER_H


#include "VideoEncoder.h"


/*!	The formats FFmpeg encodes itself or through a library it links: VP8 and
	VP9 (libvpx), MPEG-4 Part 2, MPEG-2, Theora (libtheora) and FFV1. */
class LavcVideoEncoder : public VideoEncoder {
public:
								LavcVideoEncoder(AVPixelFormat sourceFormat);
	virtual						~LavcVideoEncoder();

	static	bool				IsAvailable(video_codec codec);

	virtual	status_t			Open(const VideoEncoderParams& params,
									BString& error);
	virtual	AVPixelFormat		InputFormat() const;
	virtual	status_t			SendFrame(const AVFrame* frame);
	virtual	status_t			ReceivePacket(AVPacket* packet);
	virtual	encoder_backend		Backend() const;
	virtual	BString				Name() const;
	virtual	BString				Description() const;

private:
			AVPixelFormat		_ChooseFormat(const AVCodec* codec,
									int bitDepth) const;

			AVPixelFormat		fSourceFormat;
			AVCodecContext*		fContext;
			AVPixelFormat		fFormat;
			BString				fName;
			BString				fDescription;
};


#endif	// LAVC_VIDEO_ENCODER_H
