/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef TRANSCODER_H
#define TRANSCODER_H


#include <Locker.h>
#include <String.h>

#include <vector>

#include "OutputSettings.h"
#include "SourceInfo.h"


struct AVCodecContext;
struct AVFilterContext;
struct AVFilterGraph;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct AVStream;
struct AVSubtitle;
class VideoEncoder;


struct TranscodeJob {
	BString				source;
	BString				destination;
	OutputSettings		settings;
	SourceInfo			info;		// as probed; filled in if empty
};


struct TranscodeProgress {
	float				fraction;	// 0..1
	double				framesPerSecond;
	double				speed;		// times real time
	bigtime_t			elapsed;
	bigtime_t			remaining;	// -1 if not known yet
	int64				frames;
	off_t				bytesWritten;
};


/*!	What the transcoder decided, for the queue and the log. */
struct TranscodeReport {
	BString				videoEncoder;	// "x264", "AMD ... (VA-API)", "copy"
	BString				videoSettings;	// "RF 22, preset medium"
	encoder_backend		backend;
	bool				cpuFallback;	// a GPU was wanted but could not
	BString				fallbackReason;
	int					width;
	int					height;
	std::vector<BString> audioTracks;	// "AAC 160 kbit/s stereo"
	std::vector<BString> subtitleTracks;
	BString				notes;			// what was changed or left out

						TranscodeReport();
};


class TranscodeListener {
public:
	virtual						~TranscodeListener();
	virtual	void				TranscodeStarted(const TranscodeReport& report);
	virtual	void				TranscodeProgressed(
									const TranscodeProgress& progress) = 0;
};


/*!	One file from source to destination: libavformat reads it, libavcodec
	decodes it, libavfilter does what the output settings ask of the
	pictures and the sound, the encoders of VideoEncoder and libavcodec
	encode them, and libavformat writes the result. Streams the settings
	keep as they are are copied without decoding.

	Run() blocks; call it from a worker thread. Cancel() may be called from
	any thread. */
class Transcoder {
public:
								Transcoder(const TranscodeJob& job);
								~Transcoder();

			status_t			Run(TranscodeListener* listener,
									BString& error);
			void				Cancel();
			bool				IsCancelled() const { return fCancelled; }

	const	TranscodeReport&	Report() const { return fReport; }

	// The size the video will be made at, for the user interface.
	static	void				OutputSize(const SourceInfo& info,
									const OutputSettings& settings,
									int* width, int* height);

private:
	struct VideoStream;
	struct AudioStream;
	struct SubtitleStream;

			status_t			_OpenInput(BString& error);
			status_t			_OpenOutput(BString& error);
			status_t			_SetUpVideo(BString& error);
			status_t			_SetUpAudio(BString& error);
			status_t			_SetUpSubtitles(BString& error);
			void				_CopyChapters();
			status_t			_DetectCrop(int crop[4]);

			status_t			_BuildVideoGraph(VideoStream& stream,
									const AVFrame* frame, BString& error);
			BString				_VideoFilters(VideoStream& stream,
									int sourceWidth, int sourceHeight);
			status_t			_OpenVideoEncoder(VideoStream& stream,
									BString& error);
			status_t			_DecodeVideo(VideoStream& stream,
									AVPacket* packet);
			status_t			_FilterVideo(VideoStream& stream,
									AVFrame* frame);
			status_t			_EncodeVideo(VideoStream& stream,
									AVFrame* frame);
			status_t			_EncodeQueued(VideoStream& stream,
									AVFrame* frame);
	static	status_t			_EncoderThread(void* data);
			void				_StopEncoderThread();

			status_t			_BuildAudioGraph(AudioStream& stream,
									const AVFrame* frame);
			status_t			_DecodeAudio(AudioStream& stream,
									AVPacket* packet);
			status_t			_FilterAudio(AudioStream& stream,
									AVFrame* frame);
			status_t			_EncodeAudio(AudioStream& stream,
									AVFrame* frame);

			status_t			_ConvertSubtitle(SubtitleStream& stream,
									AVPacket* packet);
			status_t			_FeedBurnIn(AVPacket* packet);

			status_t			_Write(AVPacket* packet, AVStream* out,
									AVRational timeBase);
			status_t			_Copy(AVPacket* packet, AVStream* in,
									AVStream* out);
			void				_Progress(int64 position, bool force);
			void				_Note(const char* text);
			void				_Cleanup();

			TranscodeJob		fJob;
			TranscodeReport		fReport;
			TranscodeListener*	fListener;
	volatile bool				fCancelled;
			BLocker				fWriteLock;
			BLocker				fProgressLock;

			AVFormatContext*	fInput;
			AVFormatContext*	fOutput;
			bool				fHeaderWritten;
			int64				fStartTime;		// AV_TIME_BASE units
			int64				fEndTime;		// 0: to the end
			int64				fDuration;
			bigtime_t			fStarted;
			bigtime_t			fLastProgress;
			int64				fLastPosition;

			VideoStream*		fVideo;
			std::vector<AudioStream*> fAudio;
			std::vector<SubtitleStream*> fSubtitles;
			std::vector<int>	fStreamKinds;	// per input stream
			std::vector<int>	fStreamSlots;
			std::vector<int64>	fCopyStarted;	// per input stream
			std::vector<std::pair<int, AVStream*> > fAttachments;

			int					fBurnInStream;	// input index, -1: none
			bool				fBurnInText;
			int					fCrop[4];		// top, bottom, left, right
};


#endif	// TRANSCODER_H
