/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef MPP_VIDEO_ENCODER_H
#define MPP_VIDEO_ENCODER_H


#include "VideoEncoder.h"

#include <deque>

#include "MppLibrary.h"


/*!	The RK3588's video encoder (RKVENC, VEPU580; Radxa ROCK 5 ITX) through
	Rockchip MPP and the rk3588_vpu driver: H.264 High and H.265 Main, eight
	bits, I and P pictures, up to 8192x8192.

	One picture at a time: it goes to MPP and its packet comes back before
	the next one is taken (the encoder spends 2 ms on a 720p picture and
	16 ms on a 4K one). */
class MppVideoEncoder : public VideoEncoder {
public:
								MppVideoEncoder(const char* deviceName);
	virtual						~MppVideoEncoder();

	static	bool				Implements(video_codec codec);
	// The driver's device is there, MPP loads and accepts an encoder.
	static	bool				IsAvailable(BString* why = NULL);

	virtual	status_t			Open(const VideoEncoderParams& params,
									BString& error);
	virtual	AVPixelFormat		InputFormat() const;
	virtual	status_t			SendFrame(const AVFrame* frame);
	virtual	status_t			ReceivePacket(AVPacket* packet);
	virtual	encoder_backend		Backend() const;
	virtual	BString				Name() const;
	virtual	BString				Description() const;

private:
			status_t			_Configure(const VideoEncoderParams& params,
									BString& error);
			status_t			_TakePacket();

			const MppLibrary*	fLibrary;
			BString				fDeviceName;
			BString				fDescription;
			MppCtx				fContext;
			MppApi*				fApi;
			MppBufferGroup		fGroup;
			MppBuffer			fPicture;
			int					fWidth;
			int					fHeight;
			int					fStride;
			int					fRows;
			int64				fPictures;
			std::deque<AVPacket*> fPackets;
			bool				fDrained;
};


#endif	// MPP_VIDEO_ENCODER_H
