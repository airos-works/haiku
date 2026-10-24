/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef IRIS_VIDEO_ENCODER_H
#define IRIS_VIDEO_ENCODER_H


#include "VideoEncoder.h"

#include <deque>


class IrisEncoder;


/*!	The Qualcomm SC8280XP's video codec (Iris, Radxa Dragon Q8B) through the
	qcom_iris driver, with the qcom_video add-on's IrisEncoder: H.264 High
	and HEVC Main, eight bits, no B pictures. */
class IrisVideoEncoder : public VideoEncoder {
public:
								IrisVideoEncoder(const char* deviceName);
	virtual						~IrisVideoEncoder();

	static	bool				Implements(video_codec codec);
	static	bool				IsAvailable();

	virtual	status_t			Open(const VideoEncoderParams& params,
									BString& error);
	virtual	AVPixelFormat		InputFormat() const;
	virtual	status_t			SendFrame(const AVFrame* frame);
	virtual	status_t			ReceivePacket(AVPacket* packet);
	virtual	encoder_backend		Backend() const;
	virtual	BString				Name() const;
	virtual	BString				Description() const;

private:
			status_t			_Take(bigtime_t timeout);

			BString				fDeviceName;
			IrisEncoder*		fEncoder;
			BString				fDescription;
			std::deque<AVPacket*> fPackets;
			bool				fDraining;
			bool				fDrained;
};


#endif	// IRIS_VIDEO_ENCODER_H
