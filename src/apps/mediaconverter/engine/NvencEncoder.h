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

	// Nothing documents how each NVIDIA generation wants its input, so a
	// format only counts once a test picture encoded through the add-on
	// decodes again (trying each way the add-on knows). Blocks, for up to
	// some seconds the first time; the result is kept, also on disk, per
	// card and add-on build. It runs in a process of its own, so a crash or
	// a hang only costs the format. MEDIACONVERTER_NVENC_RETEST ignores what
	// was kept, MEDIACONVERTER_NVENC_VARIANT=n tries only that way,
	// MEDIACONVERTER_NVENC_IN_PROCESS runs it in this process.
	static	bool				SelfTest(video_codec codec, BString* why = NULL);
	// Whether the test has run, and what came of it.
	static	bool				Tested(video_codec codec, bool* works,
									BString* why = NULL);
	// Tests every format the card has.
	static	void				SelfTestAll();
	// The test's own process: main() of MediaConverter and mediaconvert
	// call this for "--nvenc-self-test <NVENC_CODEC_* bit>"; it prints
	// "works <variant>" or "fails <reason>".
	static	int					SelfTestMain(const char* codec);

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
