/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "Transcoder.h"

#include <Autolock.h>
#include <Locker.h>
#include <OS.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <deque>

#include <pthread.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include "HardwareCaps.h"
#include "LavcVideoEncoder.h"
#include "MediaProbe.h"
#include "SoftwareEncoders.h"
#include "VideoEncoder.h"


/*!	Where the time goes, when MEDIACONVERTER_PROFILE is set: summed per
	stage and printed at the end. */
struct StageClock {
	const char*	name;
	bigtime_t	total;
	int64		count;
};

static StageClock sStages[] = {
	{ "read", 0, 0 }, { "decode video", 0, 0 }, { "filter video", 0, 0 },
	{ "encode video", 0, 0 }, { "audio", 0, 0 }, { "write", 0, 0 },
};
enum { kStageRead, kStageDecode, kStageFilter, kStageEncode, kStageAudio,
	kStageWrite };
static bool sProfile = getenv("MEDIACONVERTER_PROFILE") != NULL;

class StageTimer {
public:
	StageTimer(int stage)
		: fStage(stage), fStart(sProfile ? system_time() : 0) {}
	~StageTimer()
	{
		if (sProfile) {
			atomic_add64(&sStages[fStage].total, system_time() - fStart);
			atomic_add64(&sStages[fStage].count, 1);
		}
	}
private:
	int			fStage;
	bigtime_t	fStart;
};


enum {
	kStreamIgnored = 0,
	kStreamVideo,
	kStreamAudio,
	kStreamSubtitle,
	kStreamAttachment,
	kStreamBurnIn,
};


struct Transcoder::VideoStream {
	AVStream*			in;
	AVStream*			out;
	bool				copy;
	AVCodecContext*		decoder;
	AVFilterGraph*		graph;
	AVFilterContext*	source;
	AVFilterContext*	subtitleSource;	// bitmap subtitles burned in
	AVFilterContext*	sink;
	int					graphWidth;		// what the graph was built for
	int					graphHeight;
	int					graphFormat;
	VideoEncoder*		encoder;
	AVRational			encoderTimeBase;
	int					width;			// output
	int					height;
	AVRational			sampleAspect;
	int					rotation;		// to apply, clockwise
	bool				forceRate;
	AVRational			rate;			// output frame rate
	int64				frames;
	std::vector<int64>	keyTimes;		// chapter starts, encoder time base
	size_t				nextKey;
	AVFrame*			subtitleCanvas;	// RGBA, source size
	bool				subtitleDirty;
	int64				subtitleEnd;	// AV_TIME_BASE, INT64_MAX: none

	// The encoder runs on a thread of its own, fed through a short queue,
	// so decoding and filtering go on while it works (or waits for the
	// GPU). A NULL frame in the queue drains it.
	thread_id			encoderThread;
	pthread_mutex_t		queueLock;
	pthread_cond_t		queueChanged;
	std::deque<AVFrame*> queue;
	bool				stopEncoder;
	bool				encoderDone;
	status_t			encoderStatus;

	VideoStream()
		:
		in(NULL), out(NULL), copy(false), decoder(NULL), graph(NULL),
		source(NULL), subtitleSource(NULL), sink(NULL), graphWidth(0),
		graphHeight(0), graphFormat(-1), encoder(NULL), width(0), height(0),
		rotation(0), forceRate(false), frames(0), nextKey(0),
		subtitleCanvas(NULL), subtitleDirty(false), subtitleEnd(INT64_MAX),
		encoderThread(-1), stopEncoder(false), encoderDone(false),
		encoderStatus(B_OK)
	{
		pthread_mutex_init(&queueLock, NULL);
		pthread_cond_init(&queueChanged, NULL);
		encoderTimeBase = (AVRational){1, 90000};
		sampleAspect = (AVRational){1, 1};
		rate = (AVRational){25, 1};
	}
};


struct Transcoder::AudioStream {
	AVStream*			in;
	AVStream*			out;
	bool				copy;
	AVCodecContext*		decoder;
	AVCodecContext*		encoder;
	AVFilterGraph*		graph;
	AVFilterContext*	source;
	AVFilterContext*	sink;
	int					graphRate;
	int					graphFormat;
	AVChannelLayout		graphLayout;
	int64				nextPts;		// encoder time base

	AudioStream()
		:
		in(NULL), out(NULL), copy(false), decoder(NULL), encoder(NULL),
		graph(NULL), source(NULL), sink(NULL), graphRate(0), graphFormat(-1),
		nextPts(AV_NOPTS_VALUE)
	{
		memset(&graphLayout, 0, sizeof(graphLayout));
	}
};


struct Transcoder::SubtitleStream {
	AVStream*			in;
	AVStream*			out;
	bool				copy;
	AVCodecContext*		decoder;
	AVCodecContext*		encoder;

	SubtitleStream()
		:
		in(NULL), out(NULL), copy(true), decoder(NULL), encoder(NULL)
	{
	}
};


TranscodeReport::TranscodeReport()
	:
	backend(kBackendSoftware),
	cpuFallback(false),
	width(0),
	height(0)
{
}


TranscodeListener::~TranscodeListener()
{
}


void
TranscodeListener::TranscodeStarted(const TranscodeReport& report)
{
}


static BString
av_error(int error)
{
	char message[AV_ERROR_MAX_STRING_SIZE];
	av_strerror(error, message, sizeof(message));
	return BString(message);
}


static int
stream_rotation(const AVStream* stream)
{
	const AVPacketSideData* sideData = av_packet_side_data_get(
		stream->codecpar->coded_side_data,
		stream->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
	if (sideData == NULL || sideData->size < 9 * sizeof(int32_t))
		return 0;
	double rotation = av_display_rotation_get((const int32_t*)sideData->data);
	if (isnan(rotation))
		return 0;
	int degrees = (int)lround(-rotation) % 360;
	if (degrees < 0)
		degrees += 360;
	return (degrees + 45) / 90 * 90 % 360;
}


Transcoder::Transcoder(const TranscodeJob& job)
	:
	fJob(job),
	fListener(NULL),
	fCancelled(false),
	fInput(NULL),
	fOutput(NULL),
	fHeaderWritten(false),
	fStartTime(0),
	fEndTime(0),
	fDuration(0),
	fStarted(0),
	fLastProgress(0),
	fLastPosition(0),
	fVideo(NULL),
	fBurnInStream(-1),
	fBurnInText(false)
{
	memset(fCrop, 0, sizeof(fCrop));
}


Transcoder::~Transcoder()
{
	_Cleanup();
}


void
Transcoder::Cancel()
{
	fCancelled = true;
}


void
Transcoder::_Note(const char* text)
{
	if (!fReport.notes.IsEmpty())
		fReport.notes << "\n";
	fReport.notes << text;
}


// #pragma mark - geometry


void
Transcoder::OutputSize(const SourceInfo& info, const OutputSettings& settings,
	int* width, int* height)
{
	*width = *height = 0;
	const VideoTrackInfo* video = info.MainVideo();
	if (video == NULL || video->width <= 0 || video->height <= 0)
		return;
	int w = video->width;
	int h = video->height;
	int rotation = (video->rotation + settings.rotation) % 360;
	if (rotation == 90 || rotation == 270)
		std::swap(w, h);

	if (settings.maxHeight > 0) {
		int boxShort = settings.maxHeight;
		int boxLong = (settings.maxHeight * 16 / 9 + 1) & ~1;
		bool portrait = h > w;
		int boxW = portrait ? boxShort : boxLong;
		int boxH = portrait ? boxLong : boxShort;
		double scale = std::min(1.0, std::min((double)boxW / w,
			(double)boxH / h));
		w = (int)lround(w * scale / 2) * 2;
		h = (int)lround(h * scale / 2) * 2;
	}
	*width = (w + 1) & ~1;
	*height = (h + 1) & ~1;
}


// #pragma mark - setup


status_t
Transcoder::_OpenInput(BString& error)
{
	int result = avformat_open_input(&fInput, fJob.source.String(), NULL,
		NULL);
	if (result < 0) {
		error.SetToFormat("Cannot open the source: %s",
			av_error(result).String());
		return B_ERROR;
	}
	result = avformat_find_stream_info(fInput, NULL);
	if (result < 0) {
		error = "The source's streams cannot be read.";
		return B_ERROR;
	}
	if (fJob.info.path.IsEmpty())
		MediaProbe::Probe(fJob.source.String(), fJob.info, NULL);

	fStreamKinds.assign(fInput->nb_streams, kStreamIgnored);
	fStreamSlots.assign(fInput->nb_streams, -1);

	// Negative times are a pre-roll (an MP4 edit list, Matroska's codec
	// delay): what is shown starts at 0. Positive ones (MPEG-TS) are where
	// the source's clock starts.
	int64 base = fInput->start_time != AV_NOPTS_VALUE
		? max_c(fInput->start_time, (int64)0) : 0;
	fStartTime = base + max_c(fJob.settings.start, 0);
	if (fJob.settings.end > fJob.settings.start)
		fEndTime = base + fJob.settings.end;
	fDuration = fInput->duration > 0 ? fInput->duration : 0;
	if (fEndTime > 0)
		fDuration = fEndTime - fStartTime;
	else if (fJob.settings.start > 0 && fDuration > fJob.settings.start)
		fDuration -= fJob.settings.start;

	if (fJob.settings.start > 0) {
		av_seek_frame(fInput, -1, fStartTime, AVSEEK_FLAG_BACKWARD);
	}
	return B_OK;
}


status_t
Transcoder::_OpenOutput(BString& error)
{
	const ContainerInfo& container = container_info(fJob.settings.container);
	int result = avformat_alloc_output_context2(&fOutput, NULL,
		container.muxer, fJob.destination.String());
	if (result < 0 || fOutput == NULL) {
		error.SetToFormat("Cannot make a %s file: %s", container.name,
			av_error(result).String());
		return B_ERROR;
	}
	if (fJob.settings.copyMetadata)
		av_dict_copy(&fOutput->metadata, fInput->metadata, 0);
	av_dict_set(&fOutput->metadata, "encoding_tool",
		"MediaConverter (air/OS)", 0);
	return B_OK;
}


status_t
Transcoder::_DetectCrop(int crop[4])
{
	crop[0] = crop[1] = crop[2] = crop[3] = 0;
	if (fVideo == NULL || fJob.info.duration <= 0)
		return B_OK;

	AVFormatContext* context = NULL;
	if (avformat_open_input(&context, fJob.source.String(), NULL, NULL) < 0)
		return B_ERROR;
	avformat_find_stream_info(context, NULL);
	int index = fVideo->in->index;
	AVStream* stream = context->streams[index];
	const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
	AVCodecContext* decoder = codec != NULL ? avcodec_alloc_context3(codec)
		: NULL;
	if (decoder == NULL
		|| avcodec_parameters_to_context(decoder, stream->codecpar) < 0
		|| avcodec_open2(decoder, codec, NULL) < 0) {
		avcodec_free_context(&decoder);
		avformat_close_input(&context);
		return B_ERROR;
	}

	int best[4] = { INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX };
	int samples = 0;
	AVPacket* packet = av_packet_alloc();
	AVFrame* frame = av_frame_alloc();
	const int kSamples = 10;
	for (int sample = 0; sample < kSamples && !fCancelled; sample++) {
		int64 target = fJob.info.duration * (sample + 1) / (kSamples + 1);
		if (context->start_time != AV_NOPTS_VALUE)
			target += context->start_time;
		if (av_seek_frame(context, -1, target, AVSEEK_FLAG_BACKWARD) < 0)
			continue;
		avcodec_flush_buffers(decoder);
		bool got = false;
		for (int reads = 0; reads < 600 && !got; reads++) {
			if (av_read_frame(context, packet) < 0)
				break;
			if (packet->stream_index == index)
				avcodec_send_packet(decoder, packet);
			av_packet_unref(packet);
			got = avcodec_receive_frame(decoder, frame) == 0;
		}
		if (!got)
			continue;

		const AVPixFmtDescriptor* descriptor
			= av_pix_fmt_desc_get((AVPixelFormat)frame->format);
		if (descriptor == NULL || (descriptor->flags & AV_PIX_FMT_FLAG_RGB)
			|| (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
			av_frame_unref(frame);
			continue;
		}
		int depth = descriptor->comp[0].depth;
		int step = descriptor->comp[0].step;
		bool full = frame->color_range == AVCOL_RANGE_JPEG;
		int black = (full ? 0 : 16) << (depth - 8);
		int limitAverage = black + (6 << (depth - 8));
		int limitMax = black + (48 << (depth - 8));
		const uint8* plane = frame->data[0];
		int stride = frame->linesize[0];
		int width = frame->width;
		int height = frame->height;
		auto sampleAt = [&](int x, int y) -> int {
			const uint8* p = plane + (size_t)y * stride + (size_t)x * step;
			return depth > 8 ? (p[0] | p[1] << 8) : p[0];
		};
		auto rowBlack = [&](int y) -> bool {
			int64 sum = 0;
			int count = 0, maximum = 0;
			for (int x = 0; x < width; x += 4) {
				int value = sampleAt(x, y);
				sum += value;
				maximum = max_c(maximum, value);
				count++;
			}
			return sum / max_c(count, 1) <= limitAverage
				&& maximum <= limitMax;
		};
		auto columnBlack = [&](int x) -> bool {
			int64 sum = 0;
			int count = 0, maximum = 0;
			for (int y = 0; y < height; y += 4) {
				int value = sampleAt(x, y);
				sum += value;
				maximum = max_c(maximum, value);
				count++;
			}
			return sum / max_c(count, 1) <= limitAverage
				&& maximum <= limitMax;
		};
		int top = 0, bottom = 0, left = 0, right = 0;
		while (top < height / 2 && rowBlack(top))
			top++;
		while (bottom < height / 2 && rowBlack(height - 1 - bottom))
			bottom++;
		while (left < width / 2 && columnBlack(left))
			left++;
		while (right < width / 2 && columnBlack(width - 1 - right))
			right++;
		av_frame_unref(frame);
		// an all black picture (a fade) says nothing
		if (top >= height / 2 || left >= width / 2)
			continue;
		best[0] = min_c(best[0], top);
		best[1] = min_c(best[1], bottom);
		best[2] = min_c(best[2], left);
		best[3] = min_c(best[3], right);
		samples++;
	}
	av_frame_free(&frame);
	av_packet_free(&packet);
	avcodec_free_context(&decoder);
	avformat_close_input(&context);

	if (samples < 3)
		return B_OK;
	for (int i = 0; i < 4; i++) {
		// only borders that are worth it, and in even numbers for 4:2:0
		crop[i] = best[i] >= 4 ? best[i] & ~1 : 0;
	}
	return B_OK;
}


status_t
Transcoder::_SetUpVideo(BString& error)
{
	const OutputSettings& settings = fJob.settings;
	if (settings.videoCodec == kVideoNone
		|| !container_info(settings.container).video)
		return B_OK;

	int index = av_find_best_stream(fInput, AVMEDIA_TYPE_VIDEO, -1, -1,
		NULL, 0);
	if (index < 0)
		return B_OK;
	AVStream* in = fInput->streams[index];
	if ((in->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0)
		return B_OK;

	fVideo = new VideoStream;
	fVideo->in = in;
	fStreamKinds[index] = kStreamVideo;

	video_codec codec = settings.videoCodec;
	if (codec == kVideoCopy) {
		if (container_takes_stream(settings.container, in->codecpar->codec_id,
				true)) {
			fVideo->copy = true;
		} else {
			codec = default_video_codec(settings.container);
			BString note;
			note.SetToFormat("%s cannot hold the source's video as it is: it "
				"is converted to %s.", container_info(settings.container).name,
				video_codec_info(codec).name);
			_Note(note.String());
		}
	}

	fVideo->out = avformat_new_stream(fOutput, NULL);
	if (fVideo->out == NULL)
		return B_NO_MEMORY;
	av_dict_copy(&fVideo->out->metadata, in->metadata, 0);
	av_dict_set(&fVideo->out->metadata, "encoder", NULL, 0);
	av_dict_set(&fVideo->out->metadata, "BPS", NULL, 0);
	av_dict_set(&fVideo->out->metadata, "NUMBER_OF_BYTES", NULL, 0);
	fVideo->out->disposition = in->disposition & AV_DISPOSITION_DEFAULT;

	if (fVideo->copy) {
		avcodec_parameters_copy(fVideo->out->codecpar, in->codecpar);
		fVideo->out->codecpar->codec_tag = 0;
		fVideo->out->time_base = in->time_base;
		fVideo->out->avg_frame_rate = in->avg_frame_rate;
		fVideo->out->sample_aspect_ratio = in->sample_aspect_ratio;
		fReport.videoEncoder = "copied";
		fReport.width = in->codecpar->width;
		fReport.height = in->codecpar->height;
		return B_OK;
	}

	// decoder
	const AVCodec* decoderCodec = avcodec_find_decoder(in->codecpar->codec_id);
	if (decoderCodec == NULL) {
		error = "There is no decoder for the source's video.";
		return B_NOT_SUPPORTED;
	}
	fVideo->decoder = avcodec_alloc_context3(decoderCodec);
	if (fVideo->decoder == NULL)
		return B_NO_MEMORY;
	avcodec_parameters_to_context(fVideo->decoder, in->codecpar);
	fVideo->decoder->pkt_timebase = in->time_base;
	fVideo->decoder->thread_count = 0;
	fVideo->decoder->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
	int result = avcodec_open2(fVideo->decoder, decoderCodec, NULL);
	if (result < 0) {
		error.SetToFormat("The video decoder did not start: %s",
			av_error(result).String());
		return B_ERROR;
	}

	// geometry
	if (settings.autoCrop)
		_DetectCrop(fCrop);
	SourceInfo cropped = fJob.info;
	if (!cropped.video.empty()) {
		cropped.video[0].width = in->codecpar->width - fCrop[2] - fCrop[3];
		cropped.video[0].height = in->codecpar->height - fCrop[0] - fCrop[1];
		cropped.video[0].rotation = stream_rotation(in);
	}
	fVideo->rotation = (stream_rotation(in) + settings.rotation) % 360;
	OutputSize(cropped, settings, &fVideo->width, &fVideo->height);
	AVRational aspect = in->sample_aspect_ratio.num > 0
		? in->sample_aspect_ratio : in->codecpar->sample_aspect_ratio;
	if (aspect.num <= 0 || aspect.den <= 0)
		aspect = (AVRational){1, 1};
	if (fVideo->rotation == 90 || fVideo->rotation == 270)
		aspect = (AVRational){aspect.den, aspect.num};
	fVideo->sampleAspect = aspect;
	fReport.width = fVideo->width;
	fReport.height = fVideo->height;
	if (fCrop[0] + fCrop[1] + fCrop[2] + fCrop[3] > 0) {
		BString note;
		note.SetToFormat("Cropped %d, %d, %d and %d pixels from the top, "
			"bottom, left and right.", fCrop[0], fCrop[1], fCrop[2], fCrop[3]);
		_Note(note.String());
	}

	// frame rate
	AVRational sourceRate = av_guess_frame_rate(fInput, in, NULL);
	if (sourceRate.num <= 0 || sourceRate.den <= 0)
		sourceRate = (AVRational){25, 1};
	fVideo->rate = sourceRate;
	if (settings.frameRate > 0) {
		AVRational wanted = (AVRational){settings.frameRate, 1000};
		av_reduce(&wanted.num, &wanted.den, wanted.num, wanted.den, 1 << 20);
		if (settings.frameRate % 1000 != 0 && settings.frameRate % 1001 != 0) {
			// 23976 is 24000/1001
			int rounded = (settings.frameRate + 999) / 1000 * 1000;
			if (rounded * 1000 / 1001 == settings.frameRate)
				wanted = (AVRational){rounded, 1001};
		}
		if (!settings.peakFrameRate || av_cmp_q(sourceRate, wanted) > 0) {
			fVideo->forceRate = true;
			fVideo->rate = wanted;
		}
	}
	// MPEG-2, MPEG-4 Part 2 and Theora count time in pictures
	if (codec == kVideoMPEG2 || codec == kVideoMPEG4 || codec == kVideoTheora)
		fVideo->forceRate = true;
	if (codec == kVideoMPEG2 && !fVideo->forceRate)
		fVideo->forceRate = true;

	// subtitles to burn in
	if (fBurnInStream >= 0) {
		AVStream* subtitle = fInput->streams[fBurnInStream];
		const AVCodecDescriptor* descriptor
			= avcodec_descriptor_get(subtitle->codecpar->codec_id);
		fBurnInText = descriptor != NULL
			&& (descriptor->props & AV_CODEC_PROP_TEXT_SUB) != 0;
	}

	// The graph is built for the decoder's format now, so that the encoder
	// knows its time base, and again if the pictures turn out different.
	AVFrame* model = av_frame_alloc();
	model->width = in->codecpar->width;
	model->height = in->codecpar->height;
	model->format = in->codecpar->format >= 0 ? in->codecpar->format
		: AV_PIX_FMT_YUV420P;
	model->sample_aspect_ratio = in->codecpar->sample_aspect_ratio;
	status_t status = _BuildVideoGraph(*fVideo, model, error);
	av_frame_free(&model);
	if (status != B_OK)
		return status;

	fVideo->encoderTimeBase = av_buffersink_get_time_base(fVideo->sink);
	AVRational sinkRate = av_buffersink_get_frame_rate(fVideo->sink);
	if (sinkRate.num > 0 && sinkRate.den > 0)
		fVideo->rate = sinkRate;

	// the encoder
	VideoEncoderParams params;
	params.codec = codec;
	params.width = fVideo->width;
	params.height = fVideo->height;
	params.timeBase = fVideo->encoderTimeBase;
	params.frameRate = fVideo->rate;
	params.constantFrameRate = fVideo->forceRate
		|| (!fJob.info.video.empty() && !fJob.info.video[0].variableFrameRate);
	params.sampleAspect = fVideo->sampleAspect;
	params.rateMode = settings.rateMode;
	params.quality = settings.quality;
	params.bitrate = settings.videoBitrate;
	params.speed = settings.speed;
	double fps = av_q2d(fVideo->rate);
	params.keyInterval = (int)std::min(600.0, std::max(24.0, fps * 5));
	params.globalHeader = (fOutput->oformat->flags & AVFMT_GLOBALHEADER) != 0;
	params.primaries = in->codecpar->color_primaries;
	params.transfer = in->codecpar->color_trc;
	params.matrix = in->codecpar->color_space;
	params.range = in->codecpar->color_range == AVCOL_RANGE_JPEG
		? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
	params.chromaLocation = in->codecpar->chroma_location;
	const AVPacketSideData* mastering = av_packet_side_data_get(
		in->codecpar->coded_side_data, in->codecpar->nb_coded_side_data,
		AV_PKT_DATA_MASTERING_DISPLAY_METADATA);
	if (mastering != NULL
		&& mastering->size >= sizeof(AVMasteringDisplayMetadata)) {
		params.mastering = *(const AVMasteringDisplayMetadata*)mastering->data;
		params.haveMastering = params.mastering.has_primaries
			&& params.mastering.has_luminance;
	}
	const AVPacketSideData* light = av_packet_side_data_get(
		in->codecpar->coded_side_data, in->codecpar->nb_coded_side_data,
		AV_PKT_DATA_CONTENT_LIGHT_LEVEL);
	if (light != NULL && light->size >= sizeof(AVContentLightMetadata)) {
		params.lightLevel = *(const AVContentLightMetadata*)light->data;
		params.haveLightLevel = true;
	}
	if ((params.transfer == AVCOL_TRC_SMPTE2084
			|| params.transfer == AVCOL_TRC_ARIB_STD_B67)
		&& !params.haveMastering && !params.haveLightLevel)
		_ProbeHdrMetadata(params);
	const VideoCodecInfo& codecInfo = video_codec_info(codec);
	if (codecInfo.bitDepth == 8 && fJob.info.HasVideo()
		&& fJob.info.video[0].transfer.Length() > 0
		&& (in->codecpar->color_trc == AVCOL_TRC_SMPTE2084
			|| in->codecpar->color_trc == AVCOL_TRC_ARIB_STD_B67)) {
		_Note("The source is HDR and the output is 8-bit: colours keep the "
			"HDR description; choose a 10-bit format to keep the range.");
	}

	VideoEncoder* encoder = NULL;
	BString hardwareError;
	if (settings.encoderPreference == kPreferHardware) {
		HardwareInventory& inventory = HardwareInventory::Default();
		if (!inventory.IsProbed())
			inventory.Probe();
		const GpuDevice* device = inventory.EncoderFor(codec, params.width,
			params.height, settings.gpu.String());
		if (device != NULL) {
			encoder = HardwareInventory::CreateEncoder(*device);
			if (encoder != NULL && encoder->Open(params, hardwareError)
					!= B_OK) {
				delete encoder;
				encoder = NULL;
				fReport.cpuFallback = true;
				fReport.fallbackReason = hardwareError;
			}
		} else if (inventory.HasHardwareEncoder()
			|| inventory.CountDevices() > 0) {
			fReport.cpuFallback = true;
			fReport.fallbackReason = inventory.WhyNotHardware(codec,
				params.width, params.height);
		}
	}
	if (encoder == NULL) {
		BString missing;
		if (SoftwareEncoders::IsAvailable(codec, &missing))
			encoder = SoftwareEncoders::Create(codec);
		else if (LavcVideoEncoder::IsAvailable(codec)) {
			encoder = new(std::nothrow) LavcVideoEncoder(
				(AVPixelFormat)in->codecpar->format);
		}
		if (encoder == NULL) {
			if (!missing.IsEmpty() && SoftwareEncoders::Installable()) {
				error.SetToFormat("%s needs the %s package to encode on the "
					"CPU.", codecInfo.name, missing.String());
			} else if (!missing.IsEmpty()) {
				error.SetToFormat("%s cannot be encoded on this computer: "
					"there is no %s for its processor yet.", codecInfo.name,
					missing.String());
			} else
				error.SetToFormat("Nothing here encodes %s.", codecInfo.name);
			return B_NOT_SUPPORTED;
		}
		status = encoder->Open(params, error);
		if (status != B_OK) {
			delete encoder;
			return status;
		}
	}
	fVideo->encoder = encoder;
	fReport.videoEncoder = encoder->Name();
	fReport.videoSettings = encoder->Description();
	fReport.backend = encoder->Backend();
	if (fReport.backend != kBackendSoftware)
		fReport.cpuFallback = false;

	// The encoder's input format is only known now: rebuild the graph's end
	if (av_buffersink_get_format(fVideo->sink) != encoder->InputFormat()) {
		fVideo->graphFormat = -1;
		AVFrame* again = av_frame_alloc();
		again->width = in->codecpar->width;
		again->height = in->codecpar->height;
		again->format = in->codecpar->format >= 0 ? in->codecpar->format
			: AV_PIX_FMT_YUV420P;
		again->sample_aspect_ratio = in->codecpar->sample_aspect_ratio;
		status = _BuildVideoGraph(*fVideo, again, error);
		av_frame_free(&again);
		if (status != B_OK)
			return status;
	}

	// the output stream
	AVCodecParameters* parameters = fVideo->out->codecpar;
	parameters->codec_type = AVMEDIA_TYPE_VIDEO;
	parameters->codec_id = codecInfo.avCodec;
	parameters->width = fVideo->width;
	parameters->height = fVideo->height;
	parameters->format = encoder->InputFormat();
	parameters->sample_aspect_ratio = fVideo->sampleAspect;
	parameters->color_primaries = params.primaries;
	parameters->color_trc = params.transfer;
	parameters->color_space = params.matrix;
	parameters->color_range = params.range;
	parameters->chroma_location = params.chromaLocation;
	parameters->field_order = AV_FIELD_PROGRESSIVE;
	parameters->bit_rate = settings.rateMode == kRateAverageBitrate
		? (int64)settings.videoBitrate * 1000 : 0;
	const std::vector<uint8>& extradata = encoder->Extradata();
	if (!extradata.empty()) {
		parameters->extradata = (uint8*)av_mallocz(extradata.size()
			+ AV_INPUT_BUFFER_PADDING_SIZE);
		memcpy(parameters->extradata, extradata.data(), extradata.size());
		parameters->extradata_size = (int)extradata.size();
	}
	if (params.haveMastering) {
		av_packet_side_data_add(&parameters->coded_side_data,
			&parameters->nb_coded_side_data,
			AV_PKT_DATA_MASTERING_DISPLAY_METADATA,
			av_memdup(&params.mastering, sizeof(params.mastering)),
			sizeof(params.mastering), 0);
	}
	if (params.haveLightLevel) {
		av_packet_side_data_add(&parameters->coded_side_data,
			&parameters->nb_coded_side_data, AV_PKT_DATA_CONTENT_LIGHT_LEVEL,
			av_memdup(&params.lightLevel, sizeof(params.lightLevel)),
			sizeof(params.lightLevel), 0);
	}
	fVideo->out->time_base = fVideo->encoderTimeBase;
	fVideo->out->avg_frame_rate = fVideo->rate;
	fVideo->out->sample_aspect_ratio = fVideo->sampleAspect;

	// chapters start new key frames, as HandBrake does
	if (fJob.settings.copyChapters) {
		for (unsigned i = 1; i < fInput->nb_chapters; i++) {
			const AVChapter* chapter = fInput->chapters[i];
			fVideo->keyTimes.push_back(av_rescale_q(chapter->start,
				chapter->time_base, fVideo->encoderTimeBase));
		}
	}
	return B_OK;
}


/*!	HDR10's mastering display and light level are often only in the video
	stream itself (x265 writes them as SEI messages, the container has
	none): decode the first picture to find them. */
void
Transcoder::_ProbeHdrMetadata(VideoEncoderParams& params)
{
	AVFormatContext* context = NULL;
	if (avformat_open_input(&context, fJob.source.String(), NULL, NULL) < 0)
		return;
	AVCodecContext* decoder = NULL;
	AVPacket* packet = av_packet_alloc();
	AVFrame* frame = av_frame_alloc();
	int index = fVideo->in->index;
	const AVCodec* codec = NULL;
	if (avformat_find_stream_info(context, NULL) < 0)
		goto out;
	codec = avcodec_find_decoder(context->streams[index]->codecpar->codec_id);
	decoder = codec != NULL ? avcodec_alloc_context3(codec) : NULL;
	if (decoder == NULL || avcodec_parameters_to_context(decoder,
			context->streams[index]->codecpar) < 0
		|| avcodec_open2(decoder, codec, NULL) < 0)
		goto out;
	for (int reads = 0; reads < 500; reads++) {
		if (av_read_frame(context, packet) < 0)
			break;
		if (packet->stream_index == index)
			avcodec_send_packet(decoder, packet);
		av_packet_unref(packet);
		if (avcodec_receive_frame(decoder, frame) != 0)
			continue;
		const AVFrameSideData* mastering = av_frame_get_side_data(frame,
			AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
		if (mastering != NULL
			&& mastering->size >= sizeof(AVMasteringDisplayMetadata)) {
			params.mastering
				= *(const AVMasteringDisplayMetadata*)mastering->data;
			params.haveMastering = params.mastering.has_primaries
				&& params.mastering.has_luminance;
		}
		const AVFrameSideData* light = av_frame_get_side_data(frame,
			AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
		if (light != NULL && light->size >= sizeof(AVContentLightMetadata)) {
			params.lightLevel = *(const AVContentLightMetadata*)light->data;
			params.haveLightLevel = true;
		}
		break;
	}

out:
	av_frame_free(&frame);
	av_packet_free(&packet);
	avcodec_free_context(&decoder);
	avformat_close_input(&context);
}


/*!	The chain of filters, as libavfilter descriptions; the subtitles filter
	is added by _BuildVideoGraph(), which knows the file. */
BString
Transcoder::_VideoFilters(VideoStream& stream, int sourceWidth,
	int sourceHeight)
{
	const OutputSettings& settings = fJob.settings;
	BString chain;
	auto add = [&](const BString& filter) {
		if (!chain.IsEmpty())
			chain << ",";
		chain << filter;
	};

	if (settings.deinterlace != kDeinterlaceOff) {
		add(settings.deinterlace == kDeinterlaceAlways
			? "bwdif=mode=send_frame:parity=auto:deint=all"
			: "bwdif=mode=send_frame:parity=auto:deint=interlaced");
	}
	if (fCrop[0] + fCrop[1] + fCrop[2] + fCrop[3] > 0) {
		BString crop;
		crop.SetToFormat("crop=w=%d:h=%d:x=%d:y=%d",
			sourceWidth - fCrop[2] - fCrop[3], sourceHeight - fCrop[0] - fCrop[1],
			fCrop[2], fCrop[0]);
		add(crop);
	}
	switch (stream.rotation) {
		case 90:
			add("transpose=dir=clock");
			break;
		case 180:
			add("hflip,vflip");
			break;
		case 270:
			add("transpose=dir=cclock");
			break;
		default:
			break;
	}
	if (settings.flip)
		add("hflip");

	BString scale;
	scale.SetToFormat("scale=w=%d:h=%d:flags=%s", stream.width, stream.height,
		settings.speed == kSpeedFastest ? "bicubic" : "lanczos");
	add(scale);

	// After the scaling, so a smaller picture costs less. hqdn3d is GPL and
	// not in the system's FFmpeg, and NLMeans's integral image is not
	// threaded (0.4 s a 480p picture on the X399): a temporal average, then
	// the threaded FFT denoiser (1080p: about 60 and 30 pictures a second)
	static const char* kDenoise[] = { NULL, "atadenoise", "fftdnoiz=sigma=4",
		"fftdnoiz=sigma=8:block=16,atadenoise" };
	if (settings.denoise > kFilterOff && settings.denoise <= kFilterStrong)
		add(kDenoise[settings.denoise]);

	static const char* kSharpen[] = { NULL, "unsharp=5:5:0.4:5:5:0",
		"unsharp=5:5:0.8:5:5:0", "unsharp=7:7:1.2:7:7:0" };
	if (settings.sharpen > kFilterOff && settings.sharpen <= kFilterStrong)
		add(kSharpen[settings.sharpen]);
	if (settings.grayscale)
		add("hue=s=0");

	if (stream.forceRate) {
		BString fps;
		fps.SetToFormat("fps=fps=%d/%d:eof_action=pass", stream.rate.num,
			stream.rate.den);
		add(fps);
	}
	return chain;
}


status_t
Transcoder::_BuildVideoGraph(VideoStream& stream, const AVFrame* frame,
	BString& error)
{
	avfilter_graph_free(&stream.graph);
	stream.source = stream.sink = stream.subtitleSource = NULL;
	stream.graph = avfilter_graph_alloc();
	if (stream.graph == NULL)
		return B_NO_MEMORY;
	system_info info;
	get_system_info(&info);
	stream.graph->nb_threads = (int)info.cpu_count;

	char arguments[512];
	AVRational aspect = frame->sample_aspect_ratio.num > 0
		? frame->sample_aspect_ratio : (AVRational){0, 1};
	snprintf(arguments, sizeof(arguments),
		"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
		frame->width, frame->height, frame->format,
		stream.in->time_base.num, stream.in->time_base.den,
		aspect.num, aspect.den > 0 ? aspect.den : 1);
	AVRational rate = av_guess_frame_rate(fInput, stream.in, NULL);
	if (rate.num > 0 && rate.den > 0) {
		size_t length = strlen(arguments);
		snprintf(arguments + length, sizeof(arguments) - length,
			":frame_rate=%d/%d", rate.num, rate.den);
	}

	int result = avfilter_graph_create_filter(&stream.source,
		avfilter_get_by_name("buffer"), "in", arguments, NULL, stream.graph);
	if (result < 0) {
		error.SetToFormat("The picture pipeline did not start: %s",
			av_error(result).String());
		return B_ERROR;
	}
	result = avfilter_graph_create_filter(&stream.sink,
		avfilter_get_by_name("buffersink"), "out", NULL, NULL, stream.graph);
	if (result < 0)
		return B_ERROR;
	AVPixelFormat format = stream.encoder != NULL
		? stream.encoder->InputFormat() : AV_PIX_FMT_YUV420P;

	AVFilterContext* last = stream.source;
	auto link = [&](AVFilterContext* filter) -> bool {
		if (avfilter_link(last, 0, filter, 0) < 0)
			return false;
		last = filter;
		return true;
	};

	// bitmap subtitles go over the source picture, before any scaling
	if (fBurnInStream >= 0 && !fBurnInText) {
		snprintf(arguments, sizeof(arguments),
			"video_size=%dx%d:pix_fmt=%d:time_base=1/%d", frame->width,
			frame->height, AV_PIX_FMT_RGBA, AV_TIME_BASE);
		AVFilterContext* overlay = NULL;
		if (avfilter_graph_create_filter(&stream.subtitleSource,
				avfilter_get_by_name("buffer"), "subtitles", arguments, NULL,
				stream.graph) < 0
			|| avfilter_graph_create_filter(&overlay,
				avfilter_get_by_name("overlay"), "overlay",
				"eof_action=pass:repeatlast=1:format=auto", NULL,
				stream.graph) < 0
			|| avfilter_link(stream.subtitleSource, 0, overlay, 1) < 0
			|| !link(overlay)) {
			error = "The subtitles cannot be burned in.";
			return B_ERROR;
		}
		if (stream.subtitleCanvas == NULL) {
			stream.subtitleCanvas = av_frame_alloc();
			stream.subtitleCanvas->format = AV_PIX_FMT_RGBA;
			stream.subtitleCanvas->width = frame->width;
			stream.subtitleCanvas->height = frame->height;
			av_frame_get_buffer(stream.subtitleCanvas, 0);
			for (int y = 0; y < frame->height; y++) {
				memset(stream.subtitleCanvas->data[0]
					+ (size_t)y * stream.subtitleCanvas->linesize[0], 0,
					frame->width * 4);
			}
		}
	}

	// text subtitles, rendered by libass from the file itself
	if (fBurnInStream >= 0 && fBurnInText) {
		int subtitleIndex = 0;
		for (int i = 0; i < fBurnInStream; i++) {
			if (fInput->streams[i]->codecpar->codec_type
					== AVMEDIA_TYPE_SUBTITLE)
				subtitleIndex++;
		}
		AVFilterContext* subtitles = avfilter_graph_alloc_filter(stream.graph,
			avfilter_get_by_name("subtitles"), "subtitles");
		if (subtitles == NULL
			|| av_opt_set(subtitles, "filename", fJob.source.String(),
				AV_OPT_SEARCH_CHILDREN) < 0
			|| av_opt_set_int(subtitles, "stream_index", subtitleIndex,
				AV_OPT_SEARCH_CHILDREN) < 0) {
			error = "The subtitles cannot be burned in (libass).";
			return B_ERROR;
		}
		char size[32];
		snprintf(size, sizeof(size), "%dx%d", frame->width, frame->height);
		av_opt_set(subtitles, "original_size", size, AV_OPT_SEARCH_CHILDREN);
		if (avfilter_init_str(subtitles, NULL) < 0 || !link(subtitles)) {
			error = "The subtitles cannot be burned in (libass).";
			return B_ERROR;
		}
	}

	BString chain = _VideoFilters(stream, frame->width, frame->height);
	char formatName[64];
	snprintf(formatName, sizeof(formatName), "format=pix_fmts=%s",
		av_get_pix_fmt_name(format));
	if (!chain.IsEmpty())
		chain << ",";
	chain << formatName;

	AVFilterInOut* inputs = avfilter_inout_alloc();
	AVFilterInOut* outputs = avfilter_inout_alloc();
	outputs->name = av_strdup("in");
	outputs->filter_ctx = last;
	outputs->pad_idx = 0;
	outputs->next = NULL;
	inputs->name = av_strdup("out");
	inputs->filter_ctx = stream.sink;
	inputs->pad_idx = 0;
	inputs->next = NULL;
	result = avfilter_graph_parse_ptr(stream.graph, chain.String(), &inputs,
		&outputs, NULL);
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);
	if (result < 0) {
		error.SetToFormat("The picture filters did not start (%s): %s",
			chain.String(), av_error(result).String());
		return B_ERROR;
	}
	result = avfilter_graph_config(stream.graph, NULL);
	if (result < 0) {
		error.SetToFormat("The picture filters did not fit together: %s",
			av_error(result).String());
		return B_ERROR;
	}

	stream.graphWidth = frame->width;
	stream.graphHeight = frame->height;
	stream.graphFormat = frame->format;
	return B_OK;
}


static void
choose_layout(const AVCodec* codec, const AVChannelLayout& wanted,
	AVChannelLayout* chosen)
{
	if (codec->ch_layouts == NULL) {
		av_channel_layout_copy(chosen, &wanted);
		return;
	}
	const AVChannelLayout* best = NULL;
	for (const AVChannelLayout* layout = codec->ch_layouts;
			layout->nb_channels != 0; layout++) {
		if (av_channel_layout_compare(layout, &wanted) == 0) {
			av_channel_layout_copy(chosen, layout);
			return;
		}
		if (layout->nb_channels == wanted.nb_channels && best == NULL)
			best = layout;
	}
	if (best == NULL) {
		// the most channels below what was wanted, else the fewest above
		for (const AVChannelLayout* layout = codec->ch_layouts;
				layout->nb_channels != 0; layout++) {
			if (layout->nb_channels <= wanted.nb_channels
				&& (best == NULL || layout->nb_channels > best->nb_channels))
				best = layout;
		}
	}
	if (best == NULL)
		best = codec->ch_layouts;
	av_channel_layout_copy(chosen, best);
}


static int
choose_sample_rate(const AVCodec* codec, int wanted)
{
	if (codec->supported_samplerates == NULL)
		return wanted;
	int best = 0;
	for (const int* rate = codec->supported_samplerates; *rate != 0; rate++) {
		if (*rate == wanted)
			return wanted;
		if (best == 0 || abs(*rate - wanted) < abs(best - wanted)
			|| (abs(*rate - wanted) == abs(best - wanted) && *rate > best))
			best = *rate;
	}
	return best;
}


status_t
Transcoder::_SetUpAudio(BString& error)
{
	const OutputSettings& settings = fJob.settings;
	if (settings.audioCodec == kAudioNone)
		return B_OK;

	std::vector<int> chosen;
	int defaultIndex = -1;
	for (unsigned i = 0; i < fInput->nb_streams; i++) {
		AVStream* stream = fInput->streams[i];
		if (stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
			continue;
		if ((stream->disposition & AV_DISPOSITION_DEFAULT) != 0
			&& defaultIndex < 0)
			defaultIndex = (int)i;
		chosen.push_back((int)i);
	}
	if (chosen.empty())
		return B_OK;
	bool audioOnly = !container_info(settings.container).video;
	if (settings.audioTracks == kAudioTracksFirst || audioOnly) {
		int best = av_find_best_stream(fInput, AVMEDIA_TYPE_AUDIO, -1,
			fVideo != NULL ? fVideo->in->index : -1, NULL, 0);
		chosen.assign(1, best >= 0 ? best : chosen[0]);
	} else if (settings.audioTracks == kAudioTracksDefault)
		chosen.assign(1, defaultIndex >= 0 ? defaultIndex : chosen[0]);

	for (size_t t = 0; t < chosen.size(); t++) {
		AVStream* in = fInput->streams[chosen[t]];
		AudioStream* stream = new AudioStream;
		stream->in = in;
		fStreamKinds[in->index] = kStreamAudio;
		fStreamSlots[in->index] = (int)fAudio.size();
		fAudio.push_back(stream);

		audio_codec codec = settings.audioCodec;
		AVCodecID sourceCodec = in->codecpar->codec_id;
		bool copy = codec == kAudioCopy;
		if (!copy && settings.passthroughWhenPossible
			&& audio_codec_info(codec).avCodec == sourceCodec
			&& in->codecpar->ch_layout.nb_channels
				<= audio_codec_info(codec).maxChannels)
			copy = true;
		if (copy && !container_takes_stream(settings.container, sourceCodec,
				false)) {
			copy = false;
			if (codec == kAudioCopy)
				codec = default_audio_codec(settings.container);
			BString note;
			note.SetToFormat("Audio track %d cannot be copied into %s: it is "
				"converted to %s.", (int)t + 1,
				container_info(settings.container).name,
				audio_codec_info(codec).name);
			_Note(note.String());
		}

		stream->out = avformat_new_stream(fOutput, NULL);
		if (stream->out == NULL)
			return B_NO_MEMORY;
		av_dict_copy(&stream->out->metadata, in->metadata, 0);
		av_dict_set(&stream->out->metadata, "encoder", NULL, 0);
		av_dict_set(&stream->out->metadata, "BPS", NULL, 0);
		av_dict_set(&stream->out->metadata, "NUMBER_OF_BYTES", NULL, 0);
		stream->out->disposition = t == 0 ? AV_DISPOSITION_DEFAULT : 0;
		if ((in->disposition & AV_DISPOSITION_COMMENT) != 0)
			stream->out->disposition |= AV_DISPOSITION_COMMENT;

		if (copy) {
			stream->copy = true;
			avcodec_parameters_copy(stream->out->codecpar, in->codecpar);
			stream->out->codecpar->codec_tag = 0;
			stream->out->time_base = in->time_base;
			BString line;
			line.SetToFormat("%s, copied",
				avcodec_get_name(in->codecpar->codec_id));
			fReport.audioTracks.push_back(line);
			continue;
		}

		const AudioCodecInfo& info = audio_codec_info(codec);
		const AVCodec* encoderCodec = info.lavcEncoder != NULL
			? avcodec_find_encoder_by_name(info.lavcEncoder) : NULL;
		if (encoderCodec == NULL && codec == kAudioOpus)
			encoderCodec = avcodec_find_encoder_by_name("opus");
		if (encoderCodec == NULL && codec == kAudioVorbis)
			encoderCodec = avcodec_find_encoder_by_name("vorbis");
		if (encoderCodec == NULL && codec == kAudioHEAAC) {
			_Note("HE-AAC needs FFmpeg with libfdk_aac: AAC is used.");
			encoderCodec = avcodec_find_encoder_by_name("aac");
		}
		if (encoderCodec == NULL) {
			error.SetToFormat("Nothing here encodes %s.", info.name);
			return B_NOT_SUPPORTED;
		}
		const AVCodec* decoderCodec = avcodec_find_decoder(sourceCodec);
		if (decoderCodec == NULL) {
			error.SetToFormat("There is no decoder for audio track %d (%s).",
				(int)t + 1, avcodec_get_name(sourceCodec));
			return B_NOT_SUPPORTED;
		}

		stream->decoder = avcodec_alloc_context3(decoderCodec);
		avcodec_parameters_to_context(stream->decoder, in->codecpar);
		stream->decoder->pkt_timebase = in->time_base;
		int result = avcodec_open2(stream->decoder, decoderCodec, NULL);
		if (result < 0) {
			error.SetToFormat("The audio decoder did not start: %s",
				av_error(result).String());
			return B_ERROR;
		}

		AVCodecContext* encoder = avcodec_alloc_context3(encoderCodec);
		stream->encoder = encoder;
		AVChannelLayout wanted;
		memset(&wanted, 0, sizeof(wanted));
		int sourceChannels = in->codecpar->ch_layout.nb_channels > 0
			? in->codecpar->ch_layout.nb_channels : 2;
		switch (settings.mixdown) {
			case kMixdownMono:
				wanted = (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
				break;
			case kMixdownStereo:
				wanted = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
				break;
			case kMixdown51:
				wanted = sourceChannels >= 6
					? (AVChannelLayout)AV_CHANNEL_LAYOUT_5POINT1
					: (sourceChannels == 1
						? (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO
						: (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO);
				break;
			case kMixdown71:
				wanted = sourceChannels >= 8
					? (AVChannelLayout)AV_CHANNEL_LAYOUT_7POINT1
					: sourceChannels >= 6
						? (AVChannelLayout)AV_CHANNEL_LAYOUT_5POINT1
						: (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
				break;
			default:
				if (in->codecpar->ch_layout.order == AV_CHANNEL_ORDER_NATIVE)
					av_channel_layout_copy(&wanted, &in->codecpar->ch_layout);
				else
					av_channel_layout_default(&wanted, sourceChannels);
				break;
		}
		if (wanted.nb_channels > info.maxChannels)
			av_channel_layout_default(&wanted, info.maxChannels);
		choose_layout(encoderCodec, wanted, &encoder->ch_layout);
		av_channel_layout_uninit(&wanted);

		int sourceRate = in->codecpar->sample_rate > 0
			? in->codecpar->sample_rate : 48000;
		encoder->sample_rate = choose_sample_rate(encoderCodec,
			settings.sampleRate > 0 ? settings.sampleRate : sourceRate);
		if (codec == kAudioMP2 || codec == kAudioAC3 || codec == kAudioEAC3) {
			// keep to the rates every player of these knows
			if (encoder->sample_rate > 48000)
				encoder->sample_rate = 48000;
		}

		// sample format: what the encoder prefers, deep enough for 24 bits
		AVSampleFormat sampleFormat = encoderCodec->sample_fmts != NULL
			? encoderCodec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
		if (codec == kAudioFLAC16 || codec == kAudioALAC16
			|| codec == kAudioPCM16) {
			for (const AVSampleFormat* f = encoderCodec->sample_fmts;
					f != NULL && *f != AV_SAMPLE_FMT_NONE; f++) {
				if (av_get_bytes_per_sample(*f) == 2) {
					sampleFormat = *f;
					break;
				}
			}
		} else if (codec == kAudioFLAC24 || codec == kAudioALAC24
			|| codec == kAudioPCM24) {
			for (const AVSampleFormat* f = encoderCodec->sample_fmts;
					f != NULL && *f != AV_SAMPLE_FMT_NONE; f++) {
				if (av_get_bytes_per_sample(*f) == 4) {
					sampleFormat = *f;
					break;
				}
			}
			if (codec != kAudioPCM24)
				encoder->bits_per_raw_sample = 24;
		}
		encoder->sample_fmt = sampleFormat;
		encoder->time_base = (AVRational){1, encoder->sample_rate};
		if (!info.lossless) {
			int pairs = max_c(1, encoder->ch_layout.nb_channels / 2);
			int64 bitrate = (int64)settings.audioBitrate * 1000;
			if (pairs > 1)
				bitrate = bitrate * (pairs + 1) / 2;
			if (codec == kAudioAC3)
				bitrate = min_c(bitrate, (int64)640000);
			if (codec == kAudioMP3 || codec == kAudioMP2)
				bitrate = min_c(bitrate, (int64)320000);
			encoder->bit_rate = bitrate;
		}
		if (codec == kAudioHEAAC && strcmp(encoderCodec->name, "libfdk_aac")
				== 0) {
			encoder->profile = encoder->ch_layout.nb_channels == 2
				&& settings.audioBitrate <= 48 ? AV_PROFILE_AAC_HE_V2
				: AV_PROFILE_AAC_HE;
		}
		// TrueHD's encoder is experimental, and so are FFmpeg's own Opus and
		// Vorbis encoders where libopus and libvorbis are missing (arm64)
		if ((encoderCodec->capabilities & AV_CODEC_CAP_EXPERIMENTAL) != 0)
			encoder->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
		if ((fOutput->oformat->flags & AVFMT_GLOBALHEADER) != 0)
			encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
		encoder->thread_count = 0;

		AVDictionary* options = NULL;
		if (codec == kAudioFLAC16 || codec == kAudioFLAC24)
			av_dict_set(&options, "compression_level", "8", 0);
		result = avcodec_open2(encoder, encoderCodec, &options);
		av_dict_free(&options);
		if (result < 0) {
			error.SetToFormat("The %s encoder did not start: %s", info.name,
				av_error(result).String());
			return B_ERROR;
		}
		avcodec_parameters_from_context(stream->out->codecpar, encoder);
		stream->out->time_base = encoder->time_base;

		char layout[64] = "";
		av_channel_layout_describe(&encoder->ch_layout, layout,
			sizeof(layout));
		BString line;
		if (info.lossless) {
			line.SetToFormat("%s, %s, %d Hz", info.name, layout,
				encoder->sample_rate);
		} else {
			line.SetToFormat("%s %" B_PRId64 " kbit/s, %s, %d Hz", info.name,
				encoder->bit_rate / 1000, layout, encoder->sample_rate);
		}
		fReport.audioTracks.push_back(line);
	}
	return B_OK;
}


status_t
Transcoder::_SetUpSubtitles(BString& error)
{
	const OutputSettings& settings = fJob.settings;
	if (settings.subtitles == kSubtitlesNone
		|| !container_info(settings.container).video)
		return B_OK;

	if (settings.subtitles == kSubtitlesBurnForced
		|| settings.subtitles == kSubtitlesBurnFirst) {
		// fBurnInStream was chosen before the video was set up
		return B_OK;
	}

	for (unsigned i = 0; i < fInput->nb_streams; i++) {
		AVStream* in = fInput->streams[i];
		if (in->codecpar->codec_type == AVMEDIA_TYPE_ATTACHMENT) {
			if (settings.container == kContainerMKV) {
				AVStream* out = avformat_new_stream(fOutput, NULL);
				if (out == NULL)
					return B_NO_MEMORY;
				avcodec_parameters_copy(out->codecpar, in->codecpar);
				av_dict_copy(&out->metadata, in->metadata, 0);
				fStreamKinds[i] = kStreamAttachment;
			}
			continue;
		}
		if (in->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE)
			continue;

		AVCodecID convertTo;
		if (!container_takes_subtitle(settings.container,
				in->codecpar->codec_id, &convertTo)) {
			BString note;
			note.SetToFormat("%s subtitles cannot go into %s and are left "
				"out (burn them in, or choose Matroska).",
				avcodec_get_name(in->codecpar->codec_id),
				container_info(settings.container).name);
			_Note(note.String());
			continue;
		}

		SubtitleStream* stream = new SubtitleStream;
		stream->in = in;
		fStreamKinds[i] = kStreamSubtitle;
		fStreamSlots[i] = (int)fSubtitles.size();
		fSubtitles.push_back(stream);

		stream->out = avformat_new_stream(fOutput, NULL);
		if (stream->out == NULL)
			return B_NO_MEMORY;
		av_dict_copy(&stream->out->metadata, in->metadata, 0);
		stream->out->disposition = in->disposition
			& (AV_DISPOSITION_DEFAULT | AV_DISPOSITION_FORCED
				| AV_DISPOSITION_HEARING_IMPAIRED);

		BString language = MediaProbe::LanguageName(
			av_dict_get(in->metadata, "language", NULL, 0) != NULL
				? av_dict_get(in->metadata, "language", NULL, 0)->value : "");
		if (convertTo == AV_CODEC_ID_NONE) {
			avcodec_parameters_copy(stream->out->codecpar, in->codecpar);
			stream->out->codecpar->codec_tag = 0;
			stream->out->time_base = in->time_base;
			BString line;
			line.SetToFormat("%s (%s), copied", language.String(),
				avcodec_get_name(in->codecpar->codec_id));
			fReport.subtitleTracks.push_back(line);
			continue;
		}

		// text to the container's text format
		stream->copy = false;
		const AVCodec* decoderCodec
			= avcodec_find_decoder(in->codecpar->codec_id);
		const AVCodec* encoderCodec = avcodec_find_encoder(convertTo);
		if (decoderCodec == NULL || encoderCodec == NULL) {
			error = "A subtitle track cannot be converted.";
			return B_NOT_SUPPORTED;
		}
		stream->decoder = avcodec_alloc_context3(decoderCodec);
		avcodec_parameters_to_context(stream->decoder, in->codecpar);
		stream->decoder->pkt_timebase = in->time_base;
		if (avcodec_open2(stream->decoder, decoderCodec, NULL) < 0) {
			error = "A subtitle decoder did not start.";
			return B_ERROR;
		}
		stream->encoder = avcodec_alloc_context3(encoderCodec);
		stream->encoder->time_base = AV_TIME_BASE_Q;
		if (stream->decoder->subtitle_header != NULL) {
			stream->encoder->subtitle_header = (uint8*)av_mallocz(
				stream->decoder->subtitle_header_size + 1);
			memcpy(stream->encoder->subtitle_header,
				stream->decoder->subtitle_header,
				stream->decoder->subtitle_header_size);
			stream->encoder->subtitle_header_size
				= stream->decoder->subtitle_header_size;
		}
		if (fVideo != NULL) {
			stream->encoder->width = fVideo->width;
			stream->encoder->height = fVideo->height;
		}
		if (avcodec_open2(stream->encoder, encoderCodec, NULL) < 0) {
			error = "A subtitle encoder did not start.";
			return B_ERROR;
		}
		avcodec_parameters_from_context(stream->out->codecpar,
			stream->encoder);
		stream->out->time_base = AV_TIME_BASE_Q;
		BString line;
		line.SetToFormat("%s (%s, from %s)", language.String(),
			avcodec_get_name(convertTo),
			avcodec_get_name(in->codecpar->codec_id));
		fReport.subtitleTracks.push_back(line);
	}
	return B_OK;
}


void
Transcoder::_CopyChapters()
{
	if (!fJob.settings.copyChapters || fInput->nb_chapters == 0)
		return;
	fOutput->chapters = (AVChapter**)av_calloc(fInput->nb_chapters,
		sizeof(AVChapter*));
	if (fOutput->chapters == NULL)
		return;
	unsigned count = 0;
	for (unsigned i = 0; i < fInput->nb_chapters; i++) {
		const AVChapter* source = fInput->chapters[i];
		int64 offset = av_rescale_q(fStartTime, AV_TIME_BASE_Q,
			source->time_base);
		int64 end = fEndTime > 0 ? av_rescale_q(fEndTime, AV_TIME_BASE_Q,
			source->time_base) : INT64_MAX;
		if (source->end <= offset || source->start >= end)
			continue;
		AVChapter* chapter = (AVChapter*)av_mallocz(sizeof(AVChapter));
		if (chapter == NULL)
			break;
		chapter->id = source->id;
		chapter->time_base = source->time_base;
		chapter->start = max_c(source->start - offset, (int64)0);
		chapter->end = min_c(source->end, end) - offset;
		av_dict_copy(&chapter->metadata, source->metadata, 0);
		fOutput->chapters[count++] = chapter;
	}
	fOutput->nb_chapters = count;
}


// #pragma mark - video


status_t
Transcoder::_FeedBurnIn(AVPacket* packet)
{
	if (fVideo == NULL || fVideo->copy || fBurnInText)
		return B_OK;
	AVStream* in = fInput->streams[packet->stream_index];

	// The decoder lives in a subtitle slot of its own
	if (fStreamSlots[packet->stream_index] < 0) {
		const AVCodec* codec = avcodec_find_decoder(in->codecpar->codec_id);
		if (codec == NULL)
			return B_OK;
		SubtitleStream* stream = new SubtitleStream;
		stream->in = in;
		stream->copy = false;
		stream->decoder = avcodec_alloc_context3(codec);
		avcodec_parameters_to_context(stream->decoder, in->codecpar);
		stream->decoder->pkt_timebase = in->time_base;
		if (avcodec_open2(stream->decoder, codec, NULL) < 0) {
			delete stream;
			return B_OK;
		}
		fStreamSlots[packet->stream_index] = (int)fSubtitles.size();
		fSubtitles.push_back(stream);
	}
	SubtitleStream* stream = fSubtitles[fStreamSlots[packet->stream_index]];

	AVSubtitle subtitle;
	int got = 0;
	if (avcodec_decode_subtitle2(stream->decoder, &subtitle, &got, packet) < 0
		|| !got)
		return B_OK;

	bool forcedOnly = fJob.settings.subtitles == kSubtitlesBurnForced
		&& (in->disposition & AV_DISPOSITION_FORCED) == 0;
	AVFrame* canvas = fVideo->subtitleCanvas;
	if (canvas != NULL && av_frame_make_writable(canvas) >= 0) {
		for (int y = 0; y < canvas->height; y++)
			memset(canvas->data[0] + (size_t)y * canvas->linesize[0], 0,
				canvas->width * 4);
		for (unsigned r = 0; r < subtitle.num_rects; r++) {
			const AVSubtitleRect* rect = subtitle.rects[r];
			if (rect->type != SUBTITLE_BITMAP || rect->data[0] == NULL)
				continue;
			if (forcedOnly && (rect->flags & AV_SUBTITLE_FLAG_FORCED) == 0)
				continue;
			const uint32* palette = (const uint32*)rect->data[1];
			for (int y = 0; y < rect->h; y++) {
				int ty = rect->y + y;
				if (ty < 0 || ty >= canvas->height)
					continue;
				uint8* line = canvas->data[0] + (size_t)ty
					* canvas->linesize[0];
				const uint8* source = rect->data[0] + (size_t)y
					* rect->linesize[0];
				for (int x = 0; x < rect->w; x++) {
					int tx = rect->x + x;
					if (tx < 0 || tx >= canvas->width)
						continue;
					uint32 argb = palette[source[x]];
					uint8* pixel = line + tx * 4;
					pixel[0] = argb >> 16;
					pixel[1] = argb >> 8;
					pixel[2] = argb;
					pixel[3] = argb >> 24;
				}
			}
		}
		int64 start = subtitle.pts != AV_NOPTS_VALUE ? subtitle.pts
			: av_rescale_q(packet->pts, in->time_base, AV_TIME_BASE_Q);
		fVideo->subtitleEnd = subtitle.end_display_time > 0
			? start + (int64)subtitle.end_display_time * 1000 : INT64_MAX;
		fVideo->subtitleDirty = true;
	}
	avsubtitle_free(&subtitle);
	return B_OK;
}


status_t
Transcoder::_DecodeVideo(VideoStream& stream, AVPacket* packet)
{
	int result;
	{
		StageTimer timer(kStageDecode);
		result = avcodec_send_packet(stream.decoder, packet);
	}
	if (result < 0 && result != AVERROR_EOF && result != AVERROR(EAGAIN)) {
		// a broken packet: carry on with the next one, as players do
		return B_OK;
	}
	AVFrame* frame = av_frame_alloc();
	status_t status = B_OK;
	while (status == B_OK) {
		{
			StageTimer timer(kStageDecode);
			result = avcodec_receive_frame(stream.decoder, frame);
		}
		if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
			break;
		if (result < 0)
			break;
		status = _FilterVideo(stream, frame);
		av_frame_unref(frame);
	}
	av_frame_free(&frame);
	return status;
}


status_t
Transcoder::_FilterVideo(VideoStream& stream, AVFrame* frame)
{
	if (frame != NULL) {
		frame->pts = frame->best_effort_timestamp;
		if (frame->pts == AV_NOPTS_VALUE)
			frame->pts = frame->pkt_dts;
		if (frame->width != stream.graphWidth
			|| frame->height != stream.graphHeight
			|| frame->format != stream.graphFormat) {
			// drain what the old graph holds before the new one starts
			_FilterVideo(stream, NULL);
			BString error;
			if (_BuildVideoGraph(stream, frame, error) != B_OK)
				return B_ERROR;
		}
		if (stream.subtitleSource != NULL && frame->pts != AV_NOPTS_VALUE) {
			int64 now = av_rescale_q(frame->pts, stream.in->time_base,
				AV_TIME_BASE_Q);
			if (now >= stream.subtitleEnd) {
				for (int y = 0; y < stream.subtitleCanvas->height; y++)
					memset(stream.subtitleCanvas->data[0] + (size_t)y
						* stream.subtitleCanvas->linesize[0], 0,
						stream.subtitleCanvas->width * 4);
				stream.subtitleEnd = INT64_MAX;
			}
			// a heartbeat for the overlay: the canvas, at this time
			AVFrame* canvas = av_frame_clone(stream.subtitleCanvas);
			canvas->pts = now;
			if (av_buffersrc_add_frame_flags(stream.subtitleSource, canvas,
					AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
				av_frame_free(&canvas);
				return B_ERROR;
			}
			av_frame_free(&canvas);
		}
	} else if (stream.subtitleSource != NULL
		&& av_buffersrc_add_frame(stream.subtitleSource, NULL) < 0)
		return B_ERROR;

	{
		StageTimer timer(kStageFilter);
		if (av_buffersrc_add_frame_flags(stream.source, frame,
				AV_BUFFERSRC_FLAG_KEEP_REF) < 0)
			return B_ERROR;
	}

	AVFrame* filtered = av_frame_alloc();
	status_t status = B_OK;
	while (status == B_OK) {
		int result;
		{
			StageTimer timer(kStageFilter);
			result = av_buffersink_get_frame(stream.sink, filtered);
		}
		if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
			break;
		if (result < 0) {
			status = B_ERROR;
			break;
		}
		status = _EncodeVideo(stream, filtered);
		av_frame_unref(filtered);
	}
	av_frame_free(&filtered);
	return status;
}


status_t
Transcoder::_EncodeVideo(VideoStream& stream, AVFrame* frame)
{
	AVFrame* queued = NULL;
	if (frame != NULL) {
		if (frame->pts == AV_NOPTS_VALUE)
			return B_OK;
		int64 start = av_rescale_q(fStartTime, AV_TIME_BASE_Q,
			stream.encoderTimeBase);
		if (frame->pts < start)
			return B_OK;
		if (fEndTime > 0 && frame->pts >= av_rescale_q(fEndTime,
				AV_TIME_BASE_Q, stream.encoderTimeBase))
			return B_OK;
		frame->pict_type = AV_PICTURE_TYPE_NONE;
		while (stream.nextKey < stream.keyTimes.size()
			&& frame->pts >= stream.keyTimes[stream.nextKey]) {
			frame->pict_type = AV_PICTURE_TYPE_I;
			stream.nextKey++;
		}
		frame->pts -= start;
		frame->sample_aspect_ratio = stream.sampleAspect;
		stream.frames++;
		if (stream.encoderThread < 0)
			return _EncodeQueued(stream, frame);
		queued = av_frame_clone(frame);
		if (queued == NULL)
			return B_NO_MEMORY;
	} else if (stream.encoderThread < 0)
		return _EncodeQueued(stream, NULL);

	static const size_t kQueueLength = 6;
	pthread_mutex_lock(&stream.queueLock);
	while (frame != NULL && stream.queue.size() >= kQueueLength
		&& !stream.encoderDone)
		pthread_cond_wait(&stream.queueChanged, &stream.queueLock);
	if (stream.encoderDone) {
		status_t status = stream.encoderStatus != B_OK
			? stream.encoderStatus : B_ERROR;
		pthread_mutex_unlock(&stream.queueLock);
		av_frame_free(&queued);
		return status;
	}
	stream.queue.push_back(queued);
	pthread_cond_broadcast(&stream.queueChanged);
	pthread_mutex_unlock(&stream.queueLock);

	if (frame == NULL) {
		// drained: wait for the thread, which ends after the last packet
		status_t result;
		wait_for_thread(stream.encoderThread, &result);
		stream.encoderThread = -1;
		return stream.encoderStatus;
	}
	return B_OK;
}


/*!	On the encoder thread (or the transcoder's, without one): one picture,
	or NULL to drain, and the packets that come out. */
status_t
Transcoder::_EncodeQueued(VideoStream& stream, AVFrame* frame)
{
	{
		StageTimer timer(kStageEncode);
		if (stream.encoder->SendFrame(frame) != B_OK)
			return B_ERROR;
	}

	AVPacket* packet = av_packet_alloc();
	status_t status = B_OK;
	while (true) {
		status_t result = stream.encoder->ReceivePacket(packet);
		if (result == B_WOULD_BLOCK || result == B_LAST_BUFFER_ERROR)
			break;
		if (result != B_OK) {
			status = result;
			break;
		}
		int64 position = av_rescale_q(packet->pts, stream.encoderTimeBase,
			AV_TIME_BASE_Q);
		status = _Write(packet, stream.out, stream.encoderTimeBase);
		if (status != B_OK)
			break;
		_Progress(position, false);
	}
	av_packet_free(&packet);
	return status;
}


status_t
Transcoder::_EncoderThread(void* data)
{
	Transcoder* transcoder = (Transcoder*)data;
	VideoStream& stream = *transcoder->fVideo;
	while (true) {
		pthread_mutex_lock(&stream.queueLock);
		while (stream.queue.empty() && !stream.stopEncoder)
			pthread_cond_wait(&stream.queueChanged, &stream.queueLock);
		if (stream.stopEncoder) {
			stream.encoderDone = true;
			pthread_cond_broadcast(&stream.queueChanged);
			pthread_mutex_unlock(&stream.queueLock);
			break;
		}
		AVFrame* frame = stream.queue.front();
		stream.queue.pop_front();
		pthread_cond_broadcast(&stream.queueChanged);
		pthread_mutex_unlock(&stream.queueLock);

		bool drain = frame == NULL;
		status_t status = transcoder->_EncodeQueued(stream, frame);
		av_frame_free(&frame);

		if (status != B_OK || drain) {
			pthread_mutex_lock(&stream.queueLock);
			stream.encoderStatus = status;
			stream.encoderDone = true;
			pthread_cond_broadcast(&stream.queueChanged);
			pthread_mutex_unlock(&stream.queueLock);
			break;
		}
	}
	return B_OK;
}


void
Transcoder::_StopEncoderThread()
{
	if (fVideo == NULL || fVideo->encoderThread < 0)
		return;
	pthread_mutex_lock(&fVideo->queueLock);
	fVideo->stopEncoder = true;
	pthread_cond_broadcast(&fVideo->queueChanged);
	pthread_mutex_unlock(&fVideo->queueLock);
	status_t result;
	wait_for_thread(fVideo->encoderThread, &result);
	fVideo->encoderThread = -1;
	while (!fVideo->queue.empty()) {
		av_frame_free(&fVideo->queue.front());
		fVideo->queue.pop_front();
	}
}


// #pragma mark - audio


status_t
Transcoder::_BuildAudioGraph(AudioStream& stream, const AVFrame* frame)
{
	avfilter_graph_free(&stream.graph);
	stream.graph = avfilter_graph_alloc();
	if (stream.graph == NULL)
		return B_NO_MEMORY;

	char layout[128] = "";
	if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
		AVChannelLayout fallback;
		av_channel_layout_default(&fallback, frame->ch_layout.nb_channels);
		av_channel_layout_describe(&fallback, layout, sizeof(layout));
	} else
		av_channel_layout_describe(&frame->ch_layout, layout, sizeof(layout));
	char arguments[512];
	snprintf(arguments, sizeof(arguments),
		"time_base=1/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
		frame->sample_rate, frame->sample_rate,
		av_get_sample_fmt_name((AVSampleFormat)frame->format), layout);
	if (avfilter_graph_create_filter(&stream.source,
			avfilter_get_by_name("abuffer"), "in", arguments, NULL,
			stream.graph) < 0
		|| avfilter_graph_create_filter(&stream.sink,
			avfilter_get_by_name("abuffersink"), "out", NULL, NULL,
			stream.graph) < 0)
		return B_ERROR;

	char outLayout[128] = "";
	av_channel_layout_describe(&stream.encoder->ch_layout, outLayout,
		sizeof(outLayout));
	// soxr where this FFmpeg has it (HaikuPorts' does, air/OS's arm64 build
	// does not): asking for it otherwise fails when the graph is configured.
	static const bool kHaveSoxr
		= strstr(swresample_configuration(), "libsoxr") != NULL;
	BString chain;
	chain.SetToFormat("aresample=%sosr=%d,"
		"aformat=sample_fmts=%s:sample_rates=%d:channel_layouts=%s",
		kHaveSoxr ? "resampler=soxr:" : "", stream.encoder->sample_rate,
		av_get_sample_fmt_name(stream.encoder->sample_fmt),
		stream.encoder->sample_rate, outLayout);

	AVFilterInOut* inputs = avfilter_inout_alloc();
	AVFilterInOut* outputs = avfilter_inout_alloc();
	outputs->name = av_strdup("in");
	outputs->filter_ctx = stream.source;
	inputs->name = av_strdup("out");
	inputs->filter_ctx = stream.sink;
	int result = avfilter_graph_parse_ptr(stream.graph, chain.String(),
		&inputs, &outputs, NULL);
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);
	if (result < 0) {
		// no soxr: the default resampler
		chain.ReplaceFirst("resampler=soxr:", "");
		avfilter_graph_free(&stream.graph);
		stream.graph = avfilter_graph_alloc();
		avfilter_graph_create_filter(&stream.source,
			avfilter_get_by_name("abuffer"), "in", arguments, NULL,
			stream.graph);
		avfilter_graph_create_filter(&stream.sink,
			avfilter_get_by_name("abuffersink"), "out", NULL, NULL,
			stream.graph);
		inputs = avfilter_inout_alloc();
		outputs = avfilter_inout_alloc();
		outputs->name = av_strdup("in");
		outputs->filter_ctx = stream.source;
		inputs->name = av_strdup("out");
		inputs->filter_ctx = stream.sink;
		result = avfilter_graph_parse_ptr(stream.graph, chain.String(),
			&inputs, &outputs, NULL);
		avfilter_inout_free(&inputs);
		avfilter_inout_free(&outputs);
		if (result < 0)
			return B_ERROR;
	}
	if (avfilter_graph_config(stream.graph, NULL) < 0)
		return B_ERROR;
	if (stream.encoder->frame_size > 0
		&& (stream.encoder->codec->capabilities
			& AV_CODEC_CAP_VARIABLE_FRAME_SIZE) == 0)
		av_buffersink_set_frame_size(stream.sink, stream.encoder->frame_size);

	stream.graphRate = frame->sample_rate;
	stream.graphFormat = frame->format;
	av_channel_layout_uninit(&stream.graphLayout);
	av_channel_layout_copy(&stream.graphLayout, &frame->ch_layout);
	return B_OK;
}


status_t
Transcoder::_DecodeAudio(AudioStream& stream, AVPacket* packet)
{
	int result = avcodec_send_packet(stream.decoder, packet);
	if (result < 0 && result != AVERROR_EOF && result != AVERROR(EAGAIN))
		return B_OK;
	AVFrame* frame = av_frame_alloc();
	status_t status = B_OK;
	while (status == B_OK) {
		result = avcodec_receive_frame(stream.decoder, frame);
		if (result < 0)
			break;
		status = _FilterAudio(stream, frame);
		av_frame_unref(frame);
	}
	av_frame_free(&frame);
	return status;
}


status_t
Transcoder::_FilterAudio(AudioStream& stream, AVFrame* frame)
{
	if (frame != NULL) {
		int64 pts = frame->best_effort_timestamp;
		if (pts == AV_NOPTS_VALUE)
			pts = frame->pts;
		int64 position = pts != AV_NOPTS_VALUE ? av_rescale_q(pts,
			stream.in->time_base, AV_TIME_BASE_Q) : AV_NOPTS_VALUE;
		if (position != AV_NOPTS_VALUE) {
			if (position + av_rescale(frame->nb_samples, AV_TIME_BASE,
					max_c(frame->sample_rate, 1)) <= fStartTime)
				return B_OK;
			if (fEndTime > 0 && position >= fEndTime)
				return B_OK;
		}
		frame->pts = position != AV_NOPTS_VALUE ? av_rescale_q(pts,
			stream.in->time_base, (AVRational){1, frame->sample_rate})
			: AV_NOPTS_VALUE;
		if (stream.graph == NULL || frame->sample_rate != stream.graphRate
			|| frame->format != stream.graphFormat
			|| av_channel_layout_compare(&frame->ch_layout,
				&stream.graphLayout) != 0) {
			if (stream.graph != NULL)
				_FilterAudio(stream, NULL);
			if (_BuildAudioGraph(stream, frame) != B_OK)
				return B_ERROR;
		}
	} else if (stream.graph == NULL)
		return _EncodeAudio(stream, NULL);

	if (av_buffersrc_add_frame_flags(stream.source, frame,
			AV_BUFFERSRC_FLAG_KEEP_REF) < 0)
		return B_ERROR;
	AVFrame* filtered = av_frame_alloc();
	status_t status = B_OK;
	while (status == B_OK) {
		int result = av_buffersink_get_frame(stream.sink, filtered);
		if (result < 0)
			break;
		status = _EncodeAudio(stream, filtered);
		av_frame_unref(filtered);
	}
	av_frame_free(&filtered);
	if (frame == NULL && status == B_OK)
		status = _EncodeAudio(stream, NULL);
	return status;
}


status_t
Transcoder::_EncodeAudio(AudioStream& stream, AVFrame* frame)
{
	if (frame != NULL) {
		// the encoder counts samples from the start of the output
		int64 start = av_rescale_q(fStartTime, AV_TIME_BASE_Q,
			stream.encoder->time_base);
		if (frame->pts != AV_NOPTS_VALUE)
			frame->pts -= start;
		if (frame->pts == AV_NOPTS_VALUE || (stream.nextPts != AV_NOPTS_VALUE
				&& llabs(frame->pts - stream.nextPts) < frame->nb_samples / 2))
			frame->pts = stream.nextPts != AV_NOPTS_VALUE ? stream.nextPts : 0;
		if (frame->pts < 0)
			frame->pts = 0;
		stream.nextPts = frame->pts + frame->nb_samples;
	}
	int result = avcodec_send_frame(stream.encoder, frame);
	if (result < 0 && result != AVERROR_EOF)
		return B_ERROR;

	AVPacket* packet = av_packet_alloc();
	status_t status = B_OK;
	while (status == B_OK) {
		result = avcodec_receive_packet(stream.encoder, packet);
		if (result < 0)
			break;
		int64 position = av_rescale_q(packet->pts, stream.encoder->time_base,
			AV_TIME_BASE_Q) + fStartTime;
		status = _Write(packet, stream.out, stream.encoder->time_base);
		if (fVideo == NULL)
			_Progress(position, false);
	}
	av_packet_free(&packet);
	return status;
}


// #pragma mark - subtitles


status_t
Transcoder::_ConvertSubtitle(SubtitleStream& stream, AVPacket* packet)
{
	AVSubtitle subtitle;
	int got = 0;
	if (avcodec_decode_subtitle2(stream.decoder, &subtitle, &got, packet) < 0
		|| !got)
		return B_OK;

	int64 pts = subtitle.pts != AV_NOPTS_VALUE ? subtitle.pts
		: av_rescale_q(packet->pts, stream.in->time_base, AV_TIME_BASE_Q);
	if (pts < fStartTime || (fEndTime > 0 && pts >= fEndTime)) {
		avsubtitle_free(&subtitle);
		return B_OK;
	}
	// encoders want the times relative to the packet
	subtitle.pts = pts - fStartTime;
	int64 duration = subtitle.end_display_time - subtitle.start_display_time;
	if (duration <= 0 && packet->duration > 0) {
		duration = av_rescale_q(packet->duration, stream.in->time_base,
			(AVRational){1, 1000});
	}
	subtitle.end_display_time = (uint32)(subtitle.start_display_time
		+ max_c(duration, (int64)1));

	uint8 buffer[65536];
	int size = avcodec_encode_subtitle(stream.encoder, buffer, sizeof(buffer),
		&subtitle);
	avsubtitle_free(&subtitle);
	if (size <= 0)
		return B_OK;

	AVPacket* out = av_packet_alloc();
	if (av_new_packet(out, size) < 0) {
		av_packet_free(&out);
		return B_NO_MEMORY;
	}
	memcpy(out->data, buffer, size);
	out->pts = out->dts = pts - fStartTime
		+ (int64)subtitle.start_display_time * 1000;
	out->duration = max_c(duration, (int64)1) * 1000;
	status_t status = _Write(out, stream.out, AV_TIME_BASE_Q);
	av_packet_free(&out);
	return status;
}


// #pragma mark - output


status_t
Transcoder::_Write(AVPacket* packet, AVStream* out, AVRational timeBase)
{
	StageTimer timer(kStageWrite);
	BAutolock _(fWriteLock);
	packet->stream_index = out->index;
	av_packet_rescale_ts(packet, timeBase, out->time_base);
	int result = av_interleaved_write_frame(fOutput, packet);
	if (result < 0) {
		char message[AV_ERROR_MAX_STRING_SIZE];
		av_strerror(result, message, sizeof(message));
		fprintf(stderr, "MediaConverter: writing failed: %s\n", message);
		return B_IO_ERROR;
	}
	return B_OK;
}


status_t
Transcoder::_Copy(AVPacket* packet, AVStream* in, AVStream* out)
{
	int64 offset = av_rescale_q(fStartTime, AV_TIME_BASE_Q, in->time_base);
	if (packet->pts != AV_NOPTS_VALUE) {
		if (fEndTime > 0) {
			int64 end = av_rescale_q(fEndTime, AV_TIME_BASE_Q, in->time_base);
			if (packet->pts >= end)
				return B_OK;
			// a subtitle shown at the end goes only to the end
			if (in->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE
				&& packet->duration > 0 && packet->pts + packet->duration > end)
				packet->duration = end - packet->pts;
		}
		packet->pts -= offset;
	}
	if (packet->dts != AV_NOPTS_VALUE)
		packet->dts -= offset;
	// copies start at the first key frame at or after the start, and drop
	// the pictures that lead it (they refer to the cut-off pictures)
	int slot = in->index;
	if (slot >= (int)fCopyStarted.size())
		fCopyStarted.resize(fInput->nb_streams, AV_NOPTS_VALUE);
	if (packet->pts != AV_NOPTS_VALUE) {
		bool video = in->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
		if (fCopyStarted[slot] == AV_NOPTS_VALUE) {
			if (packet->pts < 0
				|| (video && (packet->flags & AV_PKT_FLAG_KEY) == 0))
				return B_OK;
			fCopyStarted[slot] = packet->pts;
		} else if (video && packet->pts < fCopyStarted[slot])
			return B_OK;
	} else if (fCopyStarted[slot] == AV_NOPTS_VALUE)
		return B_OK;
	if ((packet->flags & AV_PKT_FLAG_DISCARD) != 0)
		return B_OK;
	int64 position = packet->pts != AV_NOPTS_VALUE
		? av_rescale_q(packet->pts, in->time_base, AV_TIME_BASE_Q) + fStartTime
		: AV_NOPTS_VALUE;
	packet->pos = -1;
	status_t status = _Write(packet, out, in->time_base);
	if (status == B_OK && position != AV_NOPTS_VALUE
		&& (fVideo == NULL ? true : fVideo->in == in))
		_Progress(position, false);
	return status;
}


void
Transcoder::_Progress(int64 position, bool force)
{
	if (fListener == NULL)
		return;
	BAutolock _(fProgressLock);
	bigtime_t now = system_time();
	if (position != AV_NOPTS_VALUE && position > fLastPosition)
		fLastPosition = position;
	if (!force && now - fLastProgress < 200000)
		return;
	fLastProgress = now;

	TranscodeProgress progress;
	memset(&progress, 0, sizeof(progress));
	int64 done = fLastPosition - fStartTime;
	progress.fraction = fDuration > 0 ? (float)std::min(1.0,
		std::max(0.0, (double)done / fDuration)) : 0;
	progress.elapsed = now - fStarted;
	progress.frames = fVideo != NULL ? fVideo->frames : 0;
	double seconds = progress.elapsed / 1e6;
	progress.framesPerSecond = seconds > 0 ? progress.frames / seconds : 0;
	progress.speed = seconds > 0 ? done / 1e6 / seconds : 0;
	progress.remaining = progress.fraction > 0.01
		? (bigtime_t)(progress.elapsed * (1 - progress.fraction)
			/ progress.fraction) : -1;
	progress.bytesWritten = fOutput != NULL && fOutput->pb != NULL
		? avio_tell(fOutput->pb) : 0;
	fListener->TranscodeProgressed(progress);
}


status_t
Transcoder::Run(TranscodeListener* listener, BString& error)
{
	fListener = listener;
	fStarted = system_time();
	av_log_set_level(AV_LOG_ERROR);

	status_t status = _OpenInput(error);
	if (status == B_OK)
		status = _OpenOutput(error);
	if (status != B_OK) {
		_Cleanup();
		return status;
	}

	// which subtitle track to burn in, before the video graph is built
	const OutputSettings& settings = fJob.settings;
	if ((settings.subtitles == kSubtitlesBurnForced
			|| settings.subtitles == kSubtitlesBurnFirst)
		&& container_info(settings.container).video
		&& settings.videoCodec != kVideoCopy
		&& settings.videoCodec != kVideoNone) {
		int first = -1, forced = -1, fallback = -1;
		for (unsigned i = 0; i < fInput->nb_streams; i++) {
			AVStream* stream = fInput->streams[i];
			if (stream->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE)
				continue;
			if (first < 0)
				first = (int)i;
			if ((stream->disposition & AV_DISPOSITION_FORCED) != 0
				&& forced < 0)
				forced = (int)i;
			if ((stream->disposition & AV_DISPOSITION_DEFAULT) != 0
				&& fallback < 0)
				fallback = (int)i;
		}
		fBurnInStream = settings.subtitles == kSubtitlesBurnFirst ? first
			: forced >= 0 ? forced : fallback;
		if (fBurnInStream >= 0) {
			fStreamKinds[fBurnInStream] = kStreamBurnIn;
			BString line;
			AVDictionaryEntry* language = av_dict_get(
				fInput->streams[fBurnInStream]->metadata, "language", NULL, 0);
			line.SetToFormat("%s, burned in", MediaProbe::LanguageName(
				language != NULL ? language->value : "").String());
			fReport.subtitleTracks.push_back(line);
		}
	}

	status = _SetUpVideo(error);
	if (status == B_OK)
		status = _SetUpAudio(error);
	if (status == B_OK)
		status = _SetUpSubtitles(error);
	if (status == B_OK && fVideo == NULL && fAudio.empty()) {
		error = "The source has nothing the output can hold.";
		status = B_BAD_VALUE;
	}

	// cover art for audio files
	if (status == B_OK && !container_info(settings.container).video) {
		for (unsigned i = 0; i < fInput->nb_streams; i++) {
			AVStream* in = fInput->streams[i];
			if ((in->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0
				|| !container_takes_stream(settings.container,
					in->codecpar->codec_id, false))
				continue;
			AVStream* out = avformat_new_stream(fOutput, NULL);
			if (out == NULL)
				break;
			avcodec_parameters_copy(out->codecpar, in->codecpar);
			out->disposition = AV_DISPOSITION_ATTACHED_PIC;
			out->time_base = in->time_base;
			fAttachments.push_back(std::make_pair((int)i, out));
			break;
		}
	}

	if (status != B_OK) {
		_Cleanup();
		return status;
	}

	_CopyChapters();
	if ((fOutput->oformat->flags & AVFMT_NOFILE) == 0) {
		int result = avio_open(&fOutput->pb, fJob.destination.String(),
			AVIO_FLAG_WRITE);
		if (result < 0) {
			error.SetToFormat("Cannot write %s: %s",
				fJob.destination.String(), av_error(result).String());
			_Cleanup();
			return B_ERROR;
		}
	}
	AVDictionary* options = NULL;
	if (settings.container == kContainerMP4 || settings.container
			== kContainerMOV || settings.container == kContainerM4A)
		av_dict_set(&options, "movflags", "+faststart", 0);
	int result = avformat_write_header(fOutput, &options);
	av_dict_free(&options);
	if (result < 0) {
		error.SetToFormat("Cannot start the output file: %s",
			av_error(result).String());
		_Cleanup();
		return B_ERROR;
	}
	fHeaderWritten = true;
	if (fListener != NULL)
		fListener->TranscodeStarted(fReport);

	if (fVideo != NULL && !fVideo->copy
		&& getenv("MEDIACONVERTER_NO_ENCODER_THREAD") == NULL) {
		fVideo->encoderThread = spawn_thread(_EncoderThread, "video encoder",
			B_NORMAL_PRIORITY, this);
		if (fVideo->encoderThread < 0
			|| resume_thread(fVideo->encoderThread) != B_OK)
			fVideo->encoderThread = -1;
	}

	// the attached picture goes first
	for (size_t i = 0; i < fAttachments.size(); i++) {
		AVStream* in = fInput->streams[fAttachments[i].first];
		if (in->attached_pic.size > 0) {
			AVPacket* picture = av_packet_clone(&in->attached_pic);
			picture->stream_index = fAttachments[i].second->index;
			picture->pts = picture->dts = 0;
			av_interleaved_write_frame(fOutput, picture);
			av_packet_free(&picture);
		}
	}

	AVPacket* packet = av_packet_alloc();
	status = B_OK;
	while (status == B_OK && !fCancelled) {
		{
			StageTimer timer(kStageRead);
			result = av_read_frame(fInput, packet);
		}
		if (result == AVERROR(EAGAIN))
			continue;
		if (result < 0)
			break;
		int index = packet->stream_index;
		if (index < 0 || index >= (int)fStreamKinds.size()) {
			av_packet_unref(packet);
			continue;
		}
		// past the end of the range: stop once every stream is there
		bool master = fVideo != NULL ? fStreamKinds[index] == kStreamVideo
			: (fStreamKinds[index] == kStreamAudio && !fAudio.empty()
				&& fAudio[0]->in->index == index);
		if (fEndTime > 0 && packet->pts != AV_NOPTS_VALUE && master
			&& av_rescale_q(packet->pts, fInput->streams[index]->time_base,
				AV_TIME_BASE_Q) > fEndTime + AV_TIME_BASE) {
			av_packet_unref(packet);
			break;
		}

		switch (fStreamKinds[index]) {
			case kStreamVideo:
				if (fVideo->copy)
					status = _Copy(packet, fVideo->in, fVideo->out);
				else
					status = _DecodeVideo(*fVideo, packet);
				break;
			case kStreamAudio:
			{
				StageTimer timer(kStageAudio);
				AudioStream* stream = fAudio[fStreamSlots[index]];
				if (stream->copy)
					status = _Copy(packet, stream->in, stream->out);
				else
					status = _DecodeAudio(*stream, packet);
				break;
			}
			case kStreamSubtitle:
			{
				SubtitleStream* stream = fSubtitles[fStreamSlots[index]];
				if (stream->copy)
					status = _Copy(packet, stream->in, stream->out);
				else
					status = _ConvertSubtitle(*stream, packet);
				break;
			}
			case kStreamBurnIn:
				status = _FeedBurnIn(packet);
				break;
			default:
				break;
		}
		av_packet_unref(packet);
	}
	av_packet_free(&packet);

	if (fCancelled) {
		_Cleanup();
		error = "Cancelled.";
		return B_CANCELED;
	}

	// drain everything
	if (status == B_OK && fVideo != NULL && !fVideo->copy) {
		_DecodeVideo(*fVideo, NULL);
		status = _FilterVideo(*fVideo, NULL);
		if (status == B_OK)
			status = _EncodeVideo(*fVideo, NULL);
	}
	for (size_t i = 0; status == B_OK && i < fAudio.size(); i++) {
		if (fAudio[i]->copy)
			continue;
		_DecodeAudio(*fAudio[i], NULL);
		status = _FilterAudio(*fAudio[i], NULL);
	}

	if (status != B_OK) {
		if (error.IsEmpty())
			error = "Encoding failed.";
		_Cleanup();
		return status;
	}

	result = av_write_trailer(fOutput);
	if (result < 0) {
		error.SetToFormat("Cannot finish the output file: %s",
			av_error(result).String());
		_Cleanup();
		return B_ERROR;
	}
	fHeaderWritten = false;
	_Progress(fEndTime > 0 ? fEndTime : fStartTime + fDuration, true);
	if (sProfile) {
		bigtime_t wall = system_time() - fStarted;
		fprintf(stderr, "profile: %.2f s wall\n", wall / 1e6);
		for (size_t i = 0; i < sizeof(sStages) / sizeof(sStages[0]); i++) {
			fprintf(stderr, "  %-13s %8.3f s %7" B_PRId64 " calls %7.3f ms\n",
				sStages[i].name, sStages[i].total / 1e6, sStages[i].count,
				sStages[i].count > 0
					? sStages[i].total / 1e3 / sStages[i].count : 0.0);
		}
	}
	_Cleanup();
	return B_OK;
}


void
Transcoder::_Cleanup()
{
	_StopEncoderThread();
	if (fVideo != NULL) {
		pthread_mutex_destroy(&fVideo->queueLock);
		pthread_cond_destroy(&fVideo->queueChanged);
		avcodec_free_context(&fVideo->decoder);
		avfilter_graph_free(&fVideo->graph);
		av_frame_free(&fVideo->subtitleCanvas);
		delete fVideo->encoder;
		delete fVideo;
		fVideo = NULL;
	}
	for (size_t i = 0; i < fAudio.size(); i++) {
		avcodec_free_context(&fAudio[i]->decoder);
		avcodec_free_context(&fAudio[i]->encoder);
		avfilter_graph_free(&fAudio[i]->graph);
		av_channel_layout_uninit(&fAudio[i]->graphLayout);
		delete fAudio[i];
	}
	fAudio.clear();
	for (size_t i = 0; i < fSubtitles.size(); i++) {
		avcodec_free_context(&fSubtitles[i]->decoder);
		avcodec_free_context(&fSubtitles[i]->encoder);
		delete fSubtitles[i];
	}
	fSubtitles.clear();

	if (fOutput != NULL) {
		if (fHeaderWritten)
			av_write_trailer(fOutput);
		if ((fOutput->oformat->flags & AVFMT_NOFILE) == 0 && fOutput->pb)
			avio_closep(&fOutput->pb);
		avformat_free_context(fOutput);
		fOutput = NULL;
		fHeaderWritten = false;
	}
	avformat_close_input(&fInput);
}
