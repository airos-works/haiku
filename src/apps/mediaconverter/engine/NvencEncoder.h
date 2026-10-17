/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef NVENC_ENCODER_H
#define NVENC_ENCODER_H


#include "VideoEncoder.h"

#include <nvenc_api.h>


/*!	NVIDIA cards' NVENC, through the nvenc add-on (see nvenc_api.h). */
class NvencEncoder : public VideoEncoder {
public:
								NvencEncoder(const char* deviceName);
	virtual						~NvencEncoder();

	static	bool				Implements(video_codec codec);
	// The add-on is installed and found a card it can drive.
	static	bool				IsAvailable(BString* why = NULL);
	// What the add-on says the card encodes, as NVENC_CODEC_* bits.
	static	uint32				Codecs();

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
			const nvenc_api*	fApi;
			nvenc_session*		fSession;
			int					fBitDepth;
			bool				fDrained;
};


#endif	// NVENC_ENCODER_H
