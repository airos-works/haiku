/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef CEDAR_VIDEO_ENCODER_H
#define CEDAR_VIDEO_ENCODER_H


#include "VideoEncoder.h"

#include <deque>


class CedarEncoder;


/*!	The Allwinner A733's video encoder (Radxa Cubie A7S) through the
	sunxi_ve driver, with the sunxi_venc add-on's CedarEncoder: H.264 and
	H.265, eight bits, I and P pictures.

	Nothing documents this encoder, so a format is only used once test
	pictures encoded on it decode again with libavcodec close to what went
	in (SelfTest(), once a process and format). */
class CedarVideoEncoder : public VideoEncoder {
public:
								CedarVideoEncoder(const char* deviceName);
	virtual						~CedarVideoEncoder();

	static	bool				Implements(video_codec codec);
	static	bool				IsAvailable();
	static	bool				SelfTest(video_codec codec,
									BString* why = NULL);
	static	void				SelfTestAll();
	// Whether the test has run, and what came of it.
	static	bool				Tested(video_codec codec, bool* works,
									BString* why = NULL);

	virtual	status_t			Open(const VideoEncoderParams& params,
									BString& error);
	virtual	AVPixelFormat		InputFormat() const;
	virtual	status_t			SendFrame(const AVFrame* frame);
	virtual	status_t			ReceivePacket(AVPacket* packet);
	virtual	encoder_backend		Backend() const;
	virtual	BString				Name() const;
	virtual	BString				Description() const;

private:
			BString				fDeviceName;
			CedarEncoder*		fEncoder;
			std::deque<AVPacket*> fPackets;
			bool				fDraining;
};


#endif	// CEDAR_VIDEO_ENCODER_H
