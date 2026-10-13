/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Decodes the video of a file with an amduvd add-on, as airTime does
	(libavformat demuxes, the add-on decodes), and prints an MD5 of every
	picture in the requested output format: compare with
	"ffmpeg -i file -f framemd5 -pix_fmt nv12|yuv420p|p010le|bgra -", which
	hashes the same layouts (bgra: RGB32).

	amduvd_decode add-on file nv12|i420|p010|rgb32 [pictures] */


#include <Application.h>
#include <DecoderPlugin.h>
#include <MediaFormats.h>
#include <image.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/md5.h>
}

using namespace BPrivate::media;


class PacketSource : public ChunkProvider {
public:
	PacketSource(AVFormatContext* context, int stream)
		:
		fContext(context),
		fStream(stream),
		fPacket(av_packet_alloc())
	{
	}

	~PacketSource()
	{
		av_packet_free(&fPacket);
	}

	status_t GetNextChunk(const void** chunk, size_t* size,
		media_header* header)
	{
		av_packet_unref(fPacket);
		while (av_read_frame(fContext, fPacket) >= 0) {
			if (fPacket->stream_index != fStream) {
				av_packet_unref(fPacket);
				continue;
			}
			AVRational base = fContext->streams[fStream]->time_base;
			memset(header, 0, sizeof(*header));
			header->type = B_MEDIA_ENCODED_VIDEO;
			int64_t pts = fPacket->pts != AV_NOPTS_VALUE ? fPacket->pts
				: fPacket->dts;
			header->start_time = av_rescale_q(pts, base,
				AVRational{1, 1000000});
			header->size_used = fPacket->size;
			*chunk = fPacket->data;
			*size = fPacket->size;
			return B_OK;
		}
		return B_LAST_BUFFER_ERROR;
	}

private:
	AVFormatContext*	fContext;
	int					fStream;
	AVPacket*			fPacket;
};


int
main(int argc, char** argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: amduvd_decode add-on file nv12|i420|p010|rgb32"
			" [pictures]\n");
		return 2;
	}
	int limit = argc > 4 ? atoi(argv[4]) : 1 << 30;
	BApplication application("application/x-vnd.airos-amduvd-decode");

	AVFormatContext* context = NULL;
	if (avformat_open_input(&context, argv[2], NULL, NULL) < 0
		|| avformat_find_stream_info(context, NULL) < 0) {
		fprintf(stderr, "cannot read %s\n", argv[2]);
		return 1;
	}
	int stream = av_find_best_stream(context, AVMEDIA_TYPE_VIDEO, -1, -1, NULL,
		0);
	if (stream < 0)
		return 1;
	AVCodecParameters* parameters = context->streams[stream]->codecpar;
	int decoderIndex = parameters->codec_id == AV_CODEC_ID_HEVC ? 1 : 0;

	image_id image = load_add_on(argv[1]);
	MediaPlugin* (*instantiate)() = NULL;
	if (image < 0 || get_image_symbol(image, "instantiate_plugin",
			B_SYMBOL_TYPE_TEXT, (void**)&instantiate) != B_OK) {
		fprintf(stderr, "cannot load %s\n", argv[1]);
		return 1;
	}
	DecoderPlugin* plugin = dynamic_cast<DecoderPlugin*>(instantiate());
	Decoder* decoder = plugin != NULL ? plugin->NewDecoder(decoderIndex) : NULL;
	if (decoder == NULL)
		return 1;
	decoder->SetChunkProvider(new PacketSource(context, stream));

	media_format_description description;
	description.family = B_MISC_FORMAT_FAMILY;
	description.u.misc.file_format = 'ffmp';
	description.u.misc.codec = parameters->codec_id;
	media_format format;
	BMediaFormats formats;
	if (formats.GetFormatFor(description, &format) != B_OK) {
		format.Clear();
		format.type = B_MEDIA_ENCODED_VIDEO;
		formats.MakeFormatFor(&description, 1, &format);
	}
	format.type = B_MEDIA_ENCODED_VIDEO;
	media_raw_video_format& encodedOutput = format.u.encoded_video.output;
	encodedOutput.interlace = 1;
	encodedOutput.last_active = parameters->height - 1;
	encodedOutput.orientation = B_VIDEO_TOP_LEFT_RIGHT;
	encodedOutput.pixel_width_aspect = 1;
	encodedOutput.pixel_height_aspect = 1;
	encodedOutput.display.format = B_YCbCr420;
	encodedOutput.display.line_width = parameters->width;
	encodedOutput.display.line_count = parameters->height;
	if (decoder->Setup(&format, parameters->extradata,
			parameters->extradata_size) != B_OK) {
		fprintf(stderr, "the decoder refused the stream\n");
		return 1;
	}

	color_space space = B_RGB32;
	if (strcmp(argv[3], "nv12") == 0)
		space = (color_space)0x4e563132;
	else if (strcmp(argv[3], "i420") == 0)
		space = (color_space)0x49343230;
	else if (strcmp(argv[3], "p010") == 0)
		space = (color_space)0x50303130;
	media_format output;
	output.Clear();
	output.type = B_MEDIA_RAW_VIDEO;
	output.u.raw_video = media_raw_video_format::wildcard;
	output.u.raw_video.display.format = space;
	if (decoder->NegotiateOutputFormat(&output) != B_OK
		|| output.u.raw_video.display.format != space) {
		fprintf(stderr, "the decoder will not output %s\n", argv[3]);
		return 1;
	}
	int width = output.u.raw_video.display.line_width;
	int height = output.u.raw_video.display.line_count;
	size_t bytes = space == B_RGB32 ? (size_t)width * height * 4
		: space == (color_space)0x50303130 ? (size_t)width * height * 3
		: (size_t)width * height * 3 / 2;
	std::vector<uint8_t> picture(bytes);

	int pictures = 0;
	while (pictures < limit) {
		int64 count = 0;
		media_header header;
		media_decode_info info;
		status_t status = decoder->Decode(picture.data(), &count, &header,
			&info);
		if (status == B_LAST_BUFFER_ERROR)
			break;
		if (status != B_OK || count != 1) {
			fprintf(stderr, "decoding failed: %s\n", strerror(status));
			return 1;
		}
		uint8_t digest[16];
		av_md5_sum(digest, picture.data(), bytes);
		printf("%d %s ", pictures++, argv[3]);
		for (int i = 0; i < 16; i++)
			printf("%02x", digest[i]);
		printf("\n");
	}
	fprintf(stderr, "%d picture(s), %dx%d\n", pictures, width, height);
	delete decoder;
	return 0;
}
