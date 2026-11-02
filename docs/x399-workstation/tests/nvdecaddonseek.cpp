/* Drive the NVDEC media add-on as Summit and airTime do, starting mid-stream.
 *
 * Usage: nvdecaddonseek <add-on> <stream.h265> <start AU> <new|seek> <out.yuv>
 *
 * new:  a fresh decoder, given the stream's parameter sets as codec data,
 *       is fed access units from <start AU> on (Summit's MSE seek).
 * seek: the decoder decodes 20 pictures from the start, is told SeekedTo,
 *       and is fed from <start AU> on (BMediaTrack / airTime's flush).
 * Pictures are asked for as NV12 and written as yuv420p. Each chunk's
 * start_time is 1000 * (AU index + 1), so every line printed says which
 * access unit the picture came from. W and H in the environment give the
 * picture size the container would (1280x720 by default).
 *
 * Built on the build host against the SDK sysroot:
 *   x86_64-unknown-haiku-g++ --sysroot=<haiku-sysroot-x86_64> -O2 \
 *     -o nvdecaddonseek nvdecaddonseek.cpp -I<tree>/headers/private/media \
 *     -lbe -lmedia
 * Compare its output with libavcodec's decode of the stream cut at the same
 * access unit (parameter sets + the rest): `ffmpeg -i cut.h265 -f rawvideo
 * -pix_fmt yuv420p`, frame by frame (haiku#44).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <image.h>
#include <MediaFormats.h>
#include <OS.h>

#include "DecoderPlugin.h"

#define NV12 ((color_space)0x4e563132)

struct Nal { size_t offset, length; int type; bool firstSlice; };

static uint8_t* sData;
static size_t sSize;
static Nal* sNals;
static size_t sNalCount;
static size_t* sAuStart;
static size_t* sAuEnd;
static size_t sAuCount;

static void
findNals()
{
	sNals = (Nal*)malloc((sSize / 3 + 16) * sizeof(Nal));
	size_t i = 0;
	while (i + 3 <= sSize) {
		if (!(sData[i] == 0 && sData[i + 1] == 0 && sData[i + 2] == 1)) {
			i++;
			continue;
		}
		size_t payload = i + 3, j = payload;
		while (j + 3 <= sSize && !(sData[j] == 0 && sData[j + 1] == 0 && sData[j + 2] == 1))
			j++;
		size_t end = j + 3 <= sSize ? j : sSize;
		while (end > payload && sData[end - 1] == 0)
			end--;
		if (end >= payload + 3) {
			Nal& n = sNals[sNalCount++];
			n.offset = i;
			n.length = end - i;
			n.type = (sData[payload] >> 1) & 0x3f;
			n.firstSlice = n.type < 32 && (sData[payload + 2] & 0x80) != 0;
		}
		i = j + 3 <= sSize ? j : sSize;
	}
}

static bool
beginsAccessUnit(const Nal& n)
{
	if (n.type < 32)
		return n.firstSlice;
	return (n.type >= 32 && n.type <= 35) || n.type == 39
		|| (n.type >= 41 && n.type <= 44);
}

static void
splitAccessUnits()
{
	sAuStart = (size_t*)malloc((sNalCount + 1) * sizeof(size_t));
	sAuEnd = (size_t*)malloc((sNalCount + 1) * sizeof(size_t));
	size_t start = 0;
	bool sawPicture = false;
	for (size_t i = 0; i <= sNalCount; i++) {
		bool last = i == sNalCount;
		if ((last || beginsAccessUnit(sNals[i])) && sawPicture) {
			size_t end = last ? sSize : sNals[i].offset;
			sAuStart[sAuCount] = start;
			sAuEnd[sAuCount] = end;
			sAuCount++;
			start = end;
			sawPicture = false;
		}
		if (last)
			break;
		if (sNals[i].type < 32)
			sawPicture = true;
	}
}

class Provider : public ChunkProvider {
public:
	size_t next = 0;
	status_t GetNextChunk(const void** buffer, size_t* size, media_header* header)
	{
		if (next >= sAuCount)
			return B_LAST_BUFFER_ERROR;
		size_t start = sAuStart[next], end = sAuEnd[next];
		/* Without the parameter sets the first access unit carries: the
		 * decoder has them as codec data, as from an hvcC record. */
		*buffer = sData + start;
		*size = end - start;
		memset(header, 0, sizeof(*header));
		header->type = B_MEDIA_ENCODED_VIDEO;
		header->start_time = 1000 * (bigtime_t)(next + 1);
		next++;
		return B_OK;
	}
};

int
main(int argc, char** argv)
{
	if (argc < 6) {
		printf("usage: %s <add-on> <stream.h265> <start AU> <new|seek> <out.yuv>\n", argv[0]);
		return 2;
	}
	size_t startAu = strtoul(argv[3], NULL, 10);
	bool seekMode = strcmp(argv[4], "seek") == 0;

	FILE* file = fopen(argv[2], "rb");
	if (file == NULL) { perror(argv[2]); return 1; }
	fseek(file, 0, SEEK_END);
	sSize = ftell(file);
	fseek(file, 0, SEEK_SET);
	sData = (uint8_t*)malloc(sSize);
	if (fread(sData, 1, sSize, file) != sSize) { printf("short read\n"); return 1; }
	fclose(file);
	findNals();
	splitAccessUnits();

	/* The parameter sets, in Annex B form, as codec data. */
	size_t infoSize = 0;
	uint8_t* info = (uint8_t*)malloc(sSize);
	for (size_t i = 0; i < sNalCount && sNals[i].type >= 32; i++) {
		if (sNals[i].type >= 32 && sNals[i].type <= 34) {
			memcpy(info + infoSize, sData + sNals[i].offset, sNals[i].length);
			infoSize += sNals[i].length;
		}
	}
	printf("%zu access units, %zu bytes of parameter sets, start at %zu, %s\n",
		sAuCount, infoSize, startAu, argv[4]);

	image_id image = load_add_on(argv[1]);
	if (image < 0) { printf("load_add_on: %s\n", strerror(image)); return 1; }
	MediaPlugin* (*instantiate)() = NULL;
	if (get_image_symbol(image, "instantiate_plugin", B_SYMBOL_TYPE_TEXT,
			(void**)&instantiate) != B_OK) {
		printf("no instantiate_plugin\n");
		return 1;
	}
	DecoderPlugin* plugin = dynamic_cast<DecoderPlugin*>(instantiate());
	Decoder* decoder = plugin->NewDecoder(1);

	media_format_description description;
	description.family = B_MISC_FORMAT_FAMILY;
	description.u.misc.file_format = 'ffmp';
	description.u.misc.codec = 173;	/* AV_CODEC_ID_HEVC */
	media_format format;
	BMediaFormats formats;
	if (formats.GetFormatFor(description, &format) != B_OK) {
		format.Clear();
		format.type = B_MEDIA_ENCODED_VIDEO;
		formats.MakeFormatFor(&description, 1, &format);
	}
	format.type = B_MEDIA_ENCODED_VIDEO;
	media_raw_video_format& output = format.u.encoded_video.output;
	output.field_rate = 30;
	output.interlace = 1;
	output.orientation = B_VIDEO_TOP_LEFT_RIGHT;
	output.pixel_width_aspect = 16;
	output.pixel_height_aspect = 9;
	output.display.format = B_YCbCr420;
	output.display.line_width = getenv("W") ? atoi(getenv("W")) : 1280;
	output.display.line_count = getenv("H") ? atoi(getenv("H")) : 720;
	output.last_active = output.display.line_count - 1;

	/* The decoder deletes its chunk provider. */
	Provider& provider = *new Provider;
	decoder->SetChunkProvider(&provider);
	if (decoder->Setup(&format, info, infoSize) != B_OK) {
		printf("Setup failed\n");
		return 1;
	}
	media_format decoded;
	decoded.Clear();
	decoded.type = B_MEDIA_RAW_VIDEO;
	decoded.u.raw_video = media_raw_video_format::wildcard;
	decoded.u.raw_video.display.format = NV12;
	if (decoder->NegotiateOutputFormat(&decoded) != B_OK
		|| decoded.u.raw_video.display.format != NV12) {
		printf("no NV12\n");
		return 1;
	}
	int width = decoded.u.raw_video.display.line_width;
	int height = decoded.u.raw_video.display.line_count;
	size_t rowBytes = decoded.u.raw_video.display.bytes_per_row;
	uint8_t* buffer = (uint8_t*)malloc(rowBytes * height * 3 / 2 + 4096);
	uint8_t* plane = (uint8_t*)malloc((size_t)width * height / 4);

	if (seekMode) {
		for (int i = 0; i < 20; i++) {
			int64 count = 0;
			media_header header;
			if (decoder->Decode(buffer, &count, &header, NULL) != B_OK)
				break;
		}
		decoder->SeekedTo(startAu, 1000 * (bigtime_t)(startAu + 1));
	}
	provider.next = startAu;

	setvbuf(stdout, NULL, _IOLBF, 0);
	FILE* out = fopen(argv[5], "wb");
	int frames = 0;
	for (;;) {
		int64 count = 0;
		media_header header;
		status_t status = decoder->Decode(buffer, &count, &header, NULL);
		if (status != B_OK)
			break;
		printf("frame %d: AU %lld\n", frames, (long long)(header.start_time / 1000 - 1));
		for (int y = 0; y < height; y++)
			fwrite(buffer + (size_t)y * rowBytes, 1, width, out);
		const uint8_t* chroma = buffer + rowBytes * height;
		for (int c = 0; c < 2; c++) {
			for (int y = 0; y < height / 2; y++) {
				for (int x = 0; x < width / 2; x++)
					plane[y * (width / 2) + x] = chroma[y * rowBytes + 2 * x + c];
			}
			fwrite(plane, 1, (size_t)width * height / 4, out);
		}
		frames++;
	}
	fclose(out);
	printf("%d pictures written, %dx%d\n", frames, width, height);
	delete decoder;
	return 0;
}
