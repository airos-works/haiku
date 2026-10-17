/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "MediaProbe.h"

#include <Bitmap.h>
#include <Path.h>

#include <math.h>
#include <stdio.h>
#include <dirent.h>
#include <string.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}


VideoTrackInfo::VideoTrackInfo()
	:
	index(-1),
	codec(AV_CODEC_ID_NONE),
	level(0),
	width(0),
	height(0),
	sampleAspect((AVRational){0, 1}),
	frameRate((AVRational){0, 1}),
	variableFrameRate(false),
	bitDepth(8),
	fullRange(false),
	interlaced(false),
	rotation(0),
	bitRate(0)
{
}


AudioTrackInfo::AudioTrackInfo()
	:
	index(-1),
	codec(AV_CODEC_ID_NONE),
	channels(0),
	sampleRate(0),
	bitDepth(0),
	bitRate(0),
	isDefault(false),
	commentary(false)
{
}


SubtitleTrackInfo::SubtitleTrackInfo()
	:
	index(-1),
	codec(AV_CODEC_ID_NONE),
	text(false),
	isDefault(false),
	forced(false)
{
}


SourceInfo::SourceInfo()
	:
	fileSize(0),
	duration(0),
	bitRate(0)
{
}


BString
SourceInfo::Metadata(const char* key) const
{
	for (size_t i = 0; i < metadata.size(); i++) {
		if (metadata[i].key.ICompare(key) == 0)
			return metadata[i].value;
	}
	return BString();
}


static BString
dictionary_value(AVDictionary* dictionary, const char* key)
{
	AVDictionaryEntry* entry = av_dict_get(dictionary, key, NULL, 0);
	return BString(entry != NULL ? entry->value : "");
}


static const char*
codec_long_name(AVCodecID codec)
{
	const AVCodecDescriptor* descriptor = avcodec_descriptor_get(codec);
	if (descriptor == NULL)
		return "Unknown";
	// The descriptors' long names are wordy for the common formats.
	switch (codec) {
		case AV_CODEC_ID_H264:
			return "H.264 (AVC)";
		case AV_CODEC_ID_HEVC:
			return "H.265 (HEVC)";
		case AV_CODEC_ID_AV1:
			return "AV1";
		case AV_CODEC_ID_VP9:
			return "VP9";
		case AV_CODEC_ID_VP8:
			return "VP8";
		case AV_CODEC_ID_MPEG2VIDEO:
			return "MPEG-2 Video";
		case AV_CODEC_ID_MPEG4:
			return "MPEG-4 Part 2";
		case AV_CODEC_ID_AAC:
			return "AAC";
		case AV_CODEC_ID_AC3:
			return "AC-3 (Dolby Digital)";
		case AV_CODEC_ID_EAC3:
			return "E-AC-3 (Dolby Digital Plus)";
		case AV_CODEC_ID_DTS:
			return "DTS";
		case AV_CODEC_ID_TRUEHD:
			return "Dolby TrueHD";
		case AV_CODEC_ID_MP3:
			return "MP3";
		case AV_CODEC_ID_FLAC:
			return "FLAC";
		case AV_CODEC_ID_ALAC:
			return "Apple Lossless (ALAC)";
		case AV_CODEC_ID_OPUS:
			return "Opus";
		case AV_CODEC_ID_VORBIS:
			return "Vorbis";
		case AV_CODEC_ID_SUBRIP:
			return "SubRip (SRT)";
		case AV_CODEC_ID_ASS:
			return "ASS/SSA";
		case AV_CODEC_ID_HDMV_PGS_SUBTITLE:
			return "PGS (Blu-ray)";
		case AV_CODEC_ID_DVD_SUBTITLE:
			return "VobSub (DVD)";
		case AV_CODEC_ID_MOV_TEXT:
			return "MPEG-4 Timed Text";
		default:
			break;
	}
	return descriptor->long_name != NULL ? descriptor->long_name
		: descriptor->name;
}


static int
display_rotation(const AVStream* stream)
{
	const AVPacketSideData* sideData = av_packet_side_data_get(
		stream->codecpar->coded_side_data,
		stream->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
	if (sideData == NULL || sideData->size < 9 * sizeof(int32_t))
		return 0;
	double rotation = av_display_rotation_get((const int32_t*)sideData->data);
	if (isnan(rotation))
		return 0;
	// av_display_rotation_get() is counterclockwise
	int degrees = (int)lround(-rotation) % 360;
	if (degrees < 0)
		degrees += 360;
	return (degrees + 45) / 90 * 90 % 360;
}


static bool
has_side_data(const AVStream* stream, enum AVPacketSideDataType type)
{
	return av_packet_side_data_get(stream->codecpar->coded_side_data,
		stream->codecpar->nb_coded_side_data, type) != NULL;
}


static void
fill_video(const AVFormatContext* context, AVStream* stream,
	VideoTrackInfo& track)
{
	const AVCodecParameters* parameters = stream->codecpar;
	track.index = stream->index;
	track.codec = parameters->codec_id;
	track.codecName = codec_long_name(parameters->codec_id);
	const char* profile = avcodec_profile_name(parameters->codec_id,
		parameters->profile);
	if (profile != NULL)
		track.profile = profile;
	track.level = parameters->level > 0 ? parameters->level : 0;
	track.width = parameters->width;
	track.height = parameters->height;
	track.sampleAspect = stream->sample_aspect_ratio.num != 0
		? stream->sample_aspect_ratio : parameters->sample_aspect_ratio;
	track.frameRate = av_guess_frame_rate((AVFormatContext*)context, stream,
		NULL);
	if (stream->r_frame_rate.num != 0 && stream->avg_frame_rate.num != 0) {
		double real = av_q2d(stream->r_frame_rate);
		double average = av_q2d(stream->avg_frame_rate);
		track.variableFrameRate = fabs(real - average) > 0.01 * real;
	}

	const AVPixFmtDescriptor* pixel
		= av_pix_fmt_desc_get((AVPixelFormat)parameters->format);
	if (pixel != NULL) {
		track.pixelFormat = pixel->name;
		track.bitDepth = pixel->comp[0].depth;
		if (pixel->nb_components < 3)
			track.chroma = "4:0:0";
		else if (pixel->log2_chroma_w == 1 && pixel->log2_chroma_h == 1)
			track.chroma = "4:2:0";
		else if (pixel->log2_chroma_w == 1)
			track.chroma = "4:2:2";
		else
			track.chroma = "4:4:4";
	}
	if (parameters->bits_per_raw_sample > 0
		&& parameters->bits_per_raw_sample <= 16
		&& track.bitDepth == 0)
		track.bitDepth = parameters->bits_per_raw_sample;

	if (parameters->color_primaries != AVCOL_PRI_UNSPECIFIED)
		track.colorPrimaries = av_color_primaries_name(
			parameters->color_primaries);
	if (parameters->color_trc != AVCOL_TRC_UNSPECIFIED)
		track.transfer = av_color_transfer_name(parameters->color_trc);
	if (parameters->color_space != AVCOL_SPC_UNSPECIFIED)
		track.matrix = av_color_space_name(parameters->color_space);
	track.fullRange = parameters->color_range == AVCOL_RANGE_JPEG;

	if (has_side_data(stream, AV_PKT_DATA_DOVI_CONF))
		track.dynamicRange = "Dolby Vision";
	else if (parameters->color_trc == AVCOL_TRC_SMPTE2084) {
		track.dynamicRange = has_side_data(stream,
			AV_PKT_DATA_MASTERING_DISPLAY_METADATA) ? "HDR10" : "HDR (PQ)";
	} else if (parameters->color_trc == AVCOL_TRC_ARIB_STD_B67)
		track.dynamicRange = "HLG";
	else
		track.dynamicRange = "SDR";

	track.interlaced = parameters->field_order != AV_FIELD_PROGRESSIVE
		&& parameters->field_order != AV_FIELD_UNKNOWN;
	track.rotation = display_rotation(stream);
	track.bitRate = parameters->bit_rate;
	if (track.bitRate == 0) {
		BString bps = dictionary_value(stream->metadata, "BPS");
		track.bitRate = atoll(bps.String());
	}
	track.language = dictionary_value(stream->metadata, "language");
	track.title = dictionary_value(stream->metadata, "title");
}


static void
fill_audio(AVStream* stream, AudioTrackInfo& track)
{
	const AVCodecParameters* parameters = stream->codecpar;
	track.index = stream->index;
	track.codec = parameters->codec_id;
	track.codecName = codec_long_name(parameters->codec_id);
	const char* profile = avcodec_profile_name(parameters->codec_id,
		parameters->profile);
	if (profile != NULL)
		track.profile = profile;
	track.channels = parameters->ch_layout.nb_channels;
	char layout[64];
	if (av_channel_layout_describe(&parameters->ch_layout, layout,
			sizeof(layout)) > 0)
		track.layout = layout;
	track.sampleRate = parameters->sample_rate;
	// a bit depth only means something for lossless formats; lossy ones
	// report the decoder's sample format
	const AVCodecDescriptor* descriptor
		= avcodec_descriptor_get(parameters->codec_id);
	if (descriptor != NULL
		&& (descriptor->props & AV_CODEC_PROP_LOSSLESS) != 0) {
		track.bitDepth = parameters->bits_per_raw_sample;
		if (track.bitDepth == 0)
			track.bitDepth = parameters->bits_per_coded_sample;
	}
	track.bitRate = parameters->bit_rate;
	if (track.bitRate == 0) {
		BString bps = dictionary_value(stream->metadata, "BPS");
		track.bitRate = atoll(bps.String());
	}
	track.language = dictionary_value(stream->metadata, "language");
	track.title = dictionary_value(stream->metadata, "title");
	track.isDefault = (stream->disposition & AV_DISPOSITION_DEFAULT) != 0;
	track.commentary = (stream->disposition & AV_DISPOSITION_COMMENT) != 0;
}


static void
fill_subtitle(AVStream* stream, SubtitleTrackInfo& track)
{
	const AVCodecParameters* parameters = stream->codecpar;
	track.index = stream->index;
	track.codec = parameters->codec_id;
	track.codecName = codec_long_name(parameters->codec_id);
	const AVCodecDescriptor* descriptor
		= avcodec_descriptor_get(parameters->codec_id);
	track.text = descriptor != NULL
		&& (descriptor->props & AV_CODEC_PROP_TEXT_SUB) != 0;
	track.language = dictionary_value(stream->metadata, "language");
	track.title = dictionary_value(stream->metadata, "title");
	track.isDefault = (stream->disposition & AV_DISPOSITION_DEFAULT) != 0;
	track.forced = (stream->disposition & AV_DISPOSITION_FORCED) != 0;
}


status_t
MediaProbe::Probe(const char* path, SourceInfo& info, BString* error)
{
	info = SourceInfo();
	info.path = path;
	info.name = BPath(path).Leaf();

	AVFormatContext* context = NULL;
	int result = avformat_open_input(&context, path, NULL, NULL);
	if (result < 0) {
		if (error != NULL) {
			char message[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(result, message, sizeof(message));
			error->SetToFormat("Cannot open the file: %s", message);
		}
		return B_ERROR;
	}
	result = avformat_find_stream_info(context, NULL);
	if (result < 0) {
		if (error != NULL)
			*error = "The file's streams cannot be read.";
		avformat_close_input(&context);
		return B_ERROR;
	}

	if (context->pb != NULL)
		info.fileSize = avio_size(context->pb);
	info.containerName = context->iformat->long_name != NULL
		? context->iformat->long_name : context->iformat->name;
	info.duration = context->duration != AV_NOPTS_VALUE
		? context->duration : 0;
	info.bitRate = context->bit_rate;

	for (unsigned i = 0; i < context->nb_streams; i++) {
		AVStream* stream = context->streams[i];
		switch (stream->codecpar->codec_type) {
			case AVMEDIA_TYPE_VIDEO:
			{
				if ((stream->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0)
					break;
				VideoTrackInfo track;
				fill_video(context, stream, track);
				info.video.push_back(track);
				break;
			}
			case AVMEDIA_TYPE_AUDIO:
			{
				AudioTrackInfo track;
				fill_audio(stream, track);
				info.audio.push_back(track);
				break;
			}
			case AVMEDIA_TYPE_SUBTITLE:
			{
				SubtitleTrackInfo track;
				fill_subtitle(stream, track);
				info.subtitles.push_back(track);
				break;
			}
			default:
				break;
		}
	}

	for (unsigned i = 0; i < context->nb_chapters; i++) {
		const AVChapter* chapter = context->chapters[i];
		ChapterInfo entry;
		entry.title = dictionary_value(chapter->metadata, "title");
		if (entry.title.IsEmpty())
			entry.title.SetToFormat("Chapter %u", i + 1);
		entry.start = av_rescale_q(chapter->start, chapter->time_base,
			AV_TIME_BASE_Q);
		entry.end = av_rescale_q(chapter->end, chapter->time_base,
			AV_TIME_BASE_Q);
		info.chapters.push_back(entry);
	}

	const AVDictionaryEntry* tag = NULL;
	while ((tag = av_dict_get(context->metadata, "", tag,
			AV_DICT_IGNORE_SUFFIX)) != NULL) {
		MetadataEntry entry;
		entry.key = tag->key;
		entry.value = tag->value;
		info.metadata.push_back(entry);
	}

	avformat_close_input(&context);
	return B_OK;
}


static BBitmap*
rotate_bitmap(BBitmap* bitmap, int rotation)
{
	if (rotation == 0)
		return bitmap;
	BRect bounds = bitmap->Bounds();
	int width = bounds.IntegerWidth() + 1;
	int height = bounds.IntegerHeight() + 1;
	bool swap = rotation == 90 || rotation == 270;
	BBitmap* rotated = new(std::nothrow) BBitmap(
		BRect(0, 0, (swap ? height : width) - 1, (swap ? width : height) - 1),
		B_RGB32);
	if (rotated == NULL || !rotated->IsValid()) {
		delete rotated;
		return bitmap;
	}
	const uint32* source = (const uint32*)bitmap->Bits();
	uint32* target = (uint32*)rotated->Bits();
	int sourceStride = bitmap->BytesPerRow() / 4;
	int targetStride = rotated->BytesPerRow() / 4;
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			int tx, ty;
			switch (rotation) {
				case 90:
					tx = height - 1 - y;
					ty = x;
					break;
				case 180:
					tx = width - 1 - x;
					ty = height - 1 - y;
					break;
				default:
					tx = y;
					ty = width - 1 - x;
					break;
			}
			target[ty * targetStride + tx] = source[y * sourceStride + x];
		}
	}
	delete bitmap;
	return rotated;
}


/*!	A decoded picture scaled to fit maxWidth x maxHeight (keeping its
	shape) and turned upright, as B_RGB32. */
static BBitmap*
frame_to_bitmap(const AVFrame* frame, int maxWidth, int maxHeight,
	int rotation)
{
	int width = frame->width;
	int height = frame->height;
	if (width <= 0 || height <= 0)
		return NULL;
	double aspect = (double)width / height;
	if (frame->sample_aspect_ratio.num > 0)
		aspect *= av_q2d(frame->sample_aspect_ratio);
	bool swap = rotation == 90 || rotation == 270;
	int boxWidth = swap ? maxHeight : maxWidth;
	int boxHeight = swap ? maxWidth : maxHeight;
	int outWidth = boxWidth;
	int outHeight = (int)lround(boxWidth / aspect);
	if (outHeight > boxHeight) {
		outHeight = boxHeight;
		outWidth = (int)lround(boxHeight * aspect);
	}
	outWidth = max_c(outWidth, 2);
	outHeight = max_c(outHeight, 2);

	SwsContext* scaler = sws_getContext(width, height,
		(AVPixelFormat)frame->format, outWidth, outHeight, AV_PIX_FMT_BGRA,
		SWS_BICUBIC, NULL, NULL, NULL);
	if (scaler == NULL)
		return NULL;
	BBitmap* bitmap = new(std::nothrow) BBitmap(
		BRect(0, 0, outWidth - 1, outHeight - 1), B_RGB32);
	if (bitmap != NULL && bitmap->IsValid()) {
		uint8* planes[4] = { (uint8*)bitmap->Bits() };
		int strides[4] = { (int)bitmap->BytesPerRow() };
		sws_scale(scaler, frame->data, frame->linesize, 0, height, planes,
			strides);
		bitmap = rotate_bitmap(bitmap, rotation);
	} else {
		delete bitmap;
		bitmap = NULL;
	}
	sws_freeContext(scaler);
	return bitmap;
}


BBitmap*
MediaProbe::Preview(const char* path, float position, int maxWidth,
	int maxHeight)
{
	AVFormatContext* context = NULL;
	if (avformat_open_input(&context, path, NULL, NULL) < 0)
		return NULL;
	BBitmap* bitmap = NULL;
	AVCodecContext* decoder = NULL;
	AVPacket* packet = av_packet_alloc();
	AVFrame* frame = av_frame_alloc();
	int streamIndex = -1;
	const AVCodec* codec = NULL;
	AVStream* stream = NULL;
	int64 target = 0;
	int rotation = 0;
	bool done = false;

	if (avformat_find_stream_info(context, NULL) < 0)
		goto out;
	streamIndex = av_find_best_stream(context, AVMEDIA_TYPE_VIDEO, -1, -1,
		&codec, 0);
	if (streamIndex < 0 || codec == NULL)
		goto out;
	stream = context->streams[streamIndex];
	rotation = display_rotation(stream);

	decoder = avcodec_alloc_context3(codec);
	if (decoder == NULL
		|| avcodec_parameters_to_context(decoder, stream->codecpar) < 0)
		goto out;
	decoder->thread_count = 0;
	if (avcodec_open2(decoder, codec, NULL) < 0)
		goto out;

	if (context->duration > 0 && position > 0) {
		target = (int64)(context->duration * position);
		if (context->start_time != AV_NOPTS_VALUE)
			target += context->start_time;
		av_seek_frame(context, -1, target, AVSEEK_FLAG_BACKWARD);
	}

	while (!done) {
		int result = av_read_frame(context, packet);
		if (result < 0) {
			avcodec_send_packet(decoder, NULL);
		} else if (packet->stream_index != streamIndex) {
			av_packet_unref(packet);
			continue;
		} else {
			avcodec_send_packet(decoder, packet);
			av_packet_unref(packet);
		}

		while (avcodec_receive_frame(decoder, frame) == 0) {
			bitmap = frame_to_bitmap(frame, maxWidth, maxHeight, rotation);
			done = true;
			break;
		}
		if (result < 0)
			break;
	}

out:
	av_frame_free(&frame);
	av_packet_free(&packet);
	avcodec_free_context(&decoder);
	avformat_close_input(&context);
	return bitmap;
}


/*!	The first picture of a file - an image, here - scaled to fit. */
static BBitmap*
decode_image(const char* path, int maxWidth, int maxHeight)
{
	AVFormatContext* context = NULL;
	if (avformat_open_input(&context, path, NULL, NULL) < 0)
		return NULL;
	BBitmap* bitmap = NULL;
	const AVCodec* codec = NULL;
	int index = avformat_find_stream_info(context, NULL) >= 0
		? av_find_best_stream(context, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0)
		: -1;
	AVCodecContext* decoder = index >= 0 && codec != NULL
		? avcodec_alloc_context3(codec) : NULL;
	if (decoder != NULL && avcodec_parameters_to_context(decoder,
			context->streams[index]->codecpar) >= 0
		&& avcodec_open2(decoder, codec, NULL) >= 0) {
		AVPacket* packet = av_packet_alloc();
		AVFrame* frame = av_frame_alloc();
		while (bitmap == NULL && av_read_frame(context, packet) >= 0) {
			if (packet->stream_index == index
				&& avcodec_send_packet(decoder, packet) >= 0) {
				avcodec_send_packet(decoder, NULL);
				if (avcodec_receive_frame(decoder, frame) == 0)
					bitmap = frame_to_bitmap(frame, maxWidth, maxHeight, 0);
			}
			av_packet_unref(packet);
		}
		av_frame_free(&frame);
		av_packet_free(&packet);
	}
	avcodec_free_context(&decoder);
	avformat_close_input(&context);
	return bitmap;
}


BString
MediaProbe::FolderCover(const char* path)
{
	BString folder(path);
	int32 slash = folder.FindLast('/');
	if (slash < 0)
		folder = ".";
	else
		folder.Truncate(slash);
	DIR* directory = opendir(folder.String());
	if (directory == NULL)
		return BString();
	static const char* kNames[] = { "cover", "folder", "front", "albumart",
		"album" };
	const int kNameCount = sizeof(kNames) / sizeof(kNames[0]);
	BString best;
	int bestRank = kNameCount;
	while (dirent* entry = readdir(directory)) {
		BString name(entry->d_name);
		BString lower(name);
		lower.ToLower();
		int32 dot = lower.FindLast('.');
		if (dot <= 0)
			continue;
		BString extension(lower.String() + dot + 1);
		if (extension != "jpg" && extension != "jpeg" && extension != "png")
			continue;
		lower.Truncate(dot);
		for (int rank = 0; rank < bestRank; rank++) {
			if (lower == kNames[rank]
				|| (rank == 3 && lower.StartsWith("albumart"))) {
				best = name;
				bestRank = rank;
				break;
			}
		}
	}
	closedir(directory);
	if (best.IsEmpty())
		return BString();
	if (slash < 0)
		return best;
	BString file;
	file.SetToFormat("%s/%s", folder.String(), best.String());
	return file;
}


BBitmap*
MediaProbe::CoverArt(const char* path, int maxWidth, int maxHeight)
{
	AVFormatContext* context = NULL;
	if (avformat_open_input(&context, path, NULL, NULL) < 0)
		return NULL;
	BBitmap* bitmap = NULL;
	if (avformat_find_stream_info(context, NULL) >= 0) {
		// the front cover when the file says which picture that is
		AVStream* cover = NULL;
		for (unsigned i = 0; i < context->nb_streams; i++) {
			AVStream* stream = context->streams[i];
			if ((stream->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0
				|| stream->attached_pic.size <= 0)
				continue;
			AVDictionaryEntry* comment = av_dict_get(stream->metadata,
				"comment", NULL, 0);
			bool front = comment != NULL
				&& strcasestr(comment->value, "front") != NULL;
			if (cover == NULL || front)
				cover = stream;
			if (front)
				break;
		}
		const AVCodec* codec = cover != NULL
			? avcodec_find_decoder(cover->codecpar->codec_id) : NULL;
		AVCodecContext* decoder = codec != NULL
			? avcodec_alloc_context3(codec) : NULL;
		if (decoder != NULL
			&& avcodec_parameters_to_context(decoder, cover->codecpar) >= 0
			&& avcodec_open2(decoder, codec, NULL) >= 0
			&& avcodec_send_packet(decoder, &cover->attached_pic) >= 0) {
			avcodec_send_packet(decoder, NULL);
			AVFrame* frame = av_frame_alloc();
			if (avcodec_receive_frame(decoder, frame) == 0)
				bitmap = frame_to_bitmap(frame, maxWidth, maxHeight, 0);
			av_frame_free(&frame);
		}
		avcodec_free_context(&decoder);
	}
	avformat_close_input(&context);
	if (bitmap == NULL) {
		BString cover = FolderCover(path);
		if (!cover.IsEmpty())
			bitmap = decode_image(cover.String(), maxWidth, maxHeight);
	}
	return bitmap;
}


BString
MediaProbe::FormatDuration(bigtime_t duration)
{
	int64 seconds = duration / 1000000;
	BString text;
	if (seconds >= 3600) {
		text.SetToFormat("%" B_PRId64 ":%02" B_PRId64 ":%02" B_PRId64,
			seconds / 3600, seconds / 60 % 60, seconds % 60);
	} else {
		text.SetToFormat("%" B_PRId64 ":%02" B_PRId64, seconds / 60,
			seconds % 60);
	}
	return text;
}


BString
MediaProbe::FormatSize(off_t size)
{
	BString text;
	if (size >= 1000LL * 1000 * 1000)
		text.SetToFormat("%.2f GB", size / 1e9);
	else if (size >= 1000 * 1000)
		text.SetToFormat("%.1f MB", size / 1e6);
	else if (size >= 1000)
		text.SetToFormat("%.0f kB", size / 1e3);
	else
		text.SetToFormat("%" B_PRIdOFF " bytes", size);
	return text;
}


BString
MediaProbe::FormatBitRate(int64 bitsPerSecond)
{
	BString text;
	if (bitsPerSecond <= 0)
		return text;
	if (bitsPerSecond >= 10 * 1000 * 1000)
		text.SetToFormat("%.1f Mbit/s", bitsPerSecond / 1e6);
	else if (bitsPerSecond >= 1000 * 1000)
		text.SetToFormat("%.2f Mbit/s", bitsPerSecond / 1e6);
	else
		text.SetToFormat("%" B_PRId64 " kbit/s", bitsPerSecond / 1000);
	return text;
}


struct LanguageCode {
	const char*	code;
	const char*	alternative;
	const char*	name;
};


static const LanguageCode kLanguages[] = {
	{ "eng", "en", "English" }, { "fre", "fra", "French" },
	{ "ger", "deu", "German" }, { "spa", "es", "Spanish" },
	{ "ita", "it", "Italian" }, { "por", "pt", "Portuguese" },
	{ "dut", "nld", "Dutch" }, { "swe", "sv", "Swedish" },
	{ "nor", "no", "Norwegian" }, { "nob", "nb", "Norwegian Bokmål" },
	{ "dan", "da", "Danish" }, { "fin", "fi", "Finnish" },
	{ "ice", "isl", "Icelandic" }, { "pol", "pl", "Polish" },
	{ "cze", "ces", "Czech" }, { "slo", "slk", "Slovak" },
	{ "hun", "hu", "Hungarian" }, { "rum", "ron", "Romanian" },
	{ "bul", "bg", "Bulgarian" }, { "gre", "ell", "Greek" },
	{ "tur", "tr", "Turkish" }, { "rus", "ru", "Russian" },
	{ "ukr", "uk", "Ukrainian" }, { "srp", "sr", "Serbian" },
	{ "hrv", "hr", "Croatian" }, { "slv", "sl", "Slovenian" },
	{ "heb", "he", "Hebrew" }, { "ara", "ar", "Arabic" },
	{ "per", "fas", "Persian" }, { "hin", "hi", "Hindi" },
	{ "ben", "bn", "Bengali" }, { "tam", "ta", "Tamil" },
	{ "tel", "te", "Telugu" }, { "tha", "th", "Thai" },
	{ "vie", "vi", "Vietnamese" }, { "ind", "id", "Indonesian" },
	{ "may", "msa", "Malay" }, { "fil", "tl", "Filipino" },
	{ "chi", "zho", "Chinese" }, { "jpn", "ja", "Japanese" },
	{ "kor", "ko", "Korean" }, { "cat", "ca", "Catalan" },
	{ "baq", "eus", "Basque" }, { "glg", "gl", "Galician" },
	{ "wel", "cym", "Welsh" }, { "gle", "ga", "Irish" },
	{ "lat", "la", "Latin" }, { "est", "et", "Estonian" },
	{ "lav", "lv", "Latvian" }, { "lit", "lt", "Lithuanian" },
	{ "mao", "mri", "Māori" }, { "zxx", NULL, "No language" },
	{ "und", NULL, "Unknown" }, { "mul", NULL, "Multiple languages" },
};


BString
MediaProbe::LanguageName(const char* code)
{
	if (code == NULL || code[0] == '\0')
		return BString("Unknown");
	for (size_t i = 0; i < sizeof(kLanguages) / sizeof(kLanguages[0]); i++) {
		if (strcasecmp(code, kLanguages[i].code) == 0
			|| (kLanguages[i].alternative != NULL
				&& strcasecmp(code, kLanguages[i].alternative) == 0))
			return BString(kLanguages[i].name);
	}
	return BString(code);
}
