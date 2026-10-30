/* Decode a whole VP9 stream on the card and say what came out.
 *
 * Reads an IVF file (ffmpeg -i in.webm -c copy out.ivf), hands the decoder
 * one chunk at a time, and for every frame shown prints its size and a hash
 * of its planes - Y, then Cb, then Cr, each cropped to the frame, eight-bit
 * pictures as yuv420p and ten-bit ones as yuv420p10le - which nvdecvp9ref.py
 * prints for libavcodec's decoder, so the two can be compared line by line.
 * With an output file the planes are written there too.
 *
 * Usage: nvdecvp9stream <stream.ivf> [out.yuv|-] [frame limit]
 * NVDEC_VP9_NOHASH=1 skips the hashing and untiling (for timing the engine).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <OS.h>

#include "nvdec_convert.h"
#include "nvdec_vp9.h"

/* zlib's CRC-32, as Python's zlib.crc32 computes it. */
static uint32_t
crc(uint32_t value, const uint8_t *data, size_t size)
{
	static uint32_t table[256];
	if (table[1] == 0) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++)
				c = (c & 1) != 0 ? 0xedb88320u ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
	}
	value = ~value;
	for (size_t i = 0; i < size; i++)
		value = table[(value ^ data[i]) & 0xff] ^ (value >> 8);
	return ~value;
}

static uint32_t sLowBits;
static int sShift = 6;

/* One line of a plane out of the card's arrangement: sixteen consecutive
 * columns are consecutive bytes. */
static void
copyLine(uint8_t *line, const uint8_t *plane, int y, size_t bytes, int pitch)
{
	size_t x = 0;
	for (; x + 16 <= bytes; x += 16)
		memcpy(line + x, plane + nvdecTileOffset((int)x, y, pitch), 16);
	for (; x < bytes; x++)
		line[x] = plane[nvdecTileOffset((int)x, y, pitch)];
}

static void
writeFrame(FILE *out, const NvdecFrame *frame, int index)
{
	int width = frame->width, height = frame->height;
	int bytes = frame->bitDepth > 8 ? 2 : 1;
	int chromaWidth = (width + 1) / 2, chromaHeight = (height + 1) / 2;
	size_t lineBytes = (size_t)width * bytes;
	uint8_t *line = malloc(lineBytes + 64);
	uint8_t *cb = malloc((size_t)chromaWidth * chromaHeight * bytes);
	uint8_t *cr = malloc((size_t)chromaWidth * chromaHeight * bytes);
	uint32_t hash = 0;

	for (int y = 0; y < height; y++) {
		copyLine(line, frame->luma, y, lineBytes, frame->pitch);
		if (bytes == 2) {
			uint16_t *samples = (uint16_t *)line;
			for (int x = 0; x < width; x++) {
				sLowBits |= samples[x] & 0x3f;
				samples[x] >>= sShift;
			}
		}
		hash = crc(hash, line, lineBytes);
		if (out != NULL)
			fwrite(line, 1, lineBytes, out);
	}
	for (int y = 0; y < chromaHeight; y++) {
		copyLine(line, frame->chroma, y, (size_t)chromaWidth * 2 * bytes,
			frame->pitch);
		for (int x = 0; x < chromaWidth; x++) {
			size_t at = (size_t)y * chromaWidth + x;
			if (bytes == 2) {
				uint16_t *samples = (uint16_t *)line;
				((uint16_t *)cb)[at] = samples[2 * x] >> sShift;
				((uint16_t *)cr)[at] = samples[2 * x + 1] >> sShift;
			} else {
				cb[at] = line[2 * x];
				cr[at] = line[2 * x + 1];
			}
		}
	}
	size_t chromaBytes = (size_t)chromaWidth * chromaHeight * bytes;
	hash = crc(hash, cb, chromaBytes);
	hash = crc(hash, cr, chromaBytes);
	if (out != NULL) {
		fwrite(cb, 1, chromaBytes, out);
		fwrite(cr, 1, chromaBytes, out);
	}
	printf("frame %d %dx%d %d %08x\n", index, width, height,
		frame->bitDepth > 8 ? 10 : 8, hash);
	free(line);
	free(cb);
	free(cr);
}

static uint32_t
le32(const uint8_t *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		printf("usage: %s <stream.ivf> [out.yuv|-] [frame limit]\n", argv[0]);
		return 2;
	}
	int limit = (argc > 3) ? atoi(argv[3]) : 0;
	bool hashing = getenv("NVDEC_VP9_NOHASH") == NULL;
	/* NVDEC_VP9_SHIFT: how far down ten-bit samples are moved (6). */
	if (getenv("NVDEC_VP9_SHIFT") != NULL)
		sShift = atoi(getenv("NVDEC_VP9_SHIFT"));

	FILE *file = fopen(argv[1], "rb");
	if (file == NULL) { perror(argv[1]); return 1; }
	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	fseek(file, 0, SEEK_SET);
	uint8_t *data = malloc(size);
	if (data == NULL || fread(data, 1, size, file) != (size_t)size) {
		printf("cannot read %s\n", argv[1]);
		return 1;
	}
	fclose(file);
	if (size < 32 || memcmp(data, "DKIF", 4) != 0) {
		printf("%s is not an IVF file\n", argv[1]);
		return 1;
	}

	FILE *out = NULL;
	if (argc > 2 && strcmp(argv[2], "-") != 0) {
		out = fopen(argv[2], "wb");
		if (out == NULL) { perror(argv[2]); return 1; }
	}

	char reason[256] = "";
	NvdecEngine *engine = nvdecOpen(reason, sizeof(reason));
	if (engine == NULL) { printf("opening the decoder: %s\n", reason); return 1; }
	NvdecVp9 *decoder = nvdecVp9Create(engine, reason, sizeof(reason));
	if (decoder == NULL) { printf("creating the decoder failed\n"); return 1; }

	int chunks = 0, written = 0, failures = 0;
	bigtime_t began = system_time();
	bigtime_t decodeTime = 0;
	size_t at = le32(data + 6) & 0xffff;
	NvdecFrame frame;
	while (at + 12 <= (size_t)size) {
		size_t length = le32(data + at);
		at += 12;
		if (at + length > (size_t)size)
			break;
		bigtime_t start = system_time();
		if (!nvdecVp9Decode(decoder, data + at, length, chunks)) {
			printf("chunk %d: %s\n", chunks, nvdecVp9LastError(decoder));
			failures++;
			if (nvdecVp9Refused(decoder))
				break;
		}
		decodeTime += system_time() - start;
		at += length;
		chunks++;
		while (nvdecVp9NextFrame(decoder, &frame)) {
			if (hashing)
				writeFrame(out, &frame, written);
			nvdecVp9ReleaseFrame(decoder, &frame);
			written++;
		}
		if (limit > 0 && written >= limit)
			break;
	}
	bigtime_t elapsed = system_time() - began;
	if (sLowBits != 0)
		printf("ten-bit samples had bits below the top ten: %#x\n", sLowBits);
	printf("%d chunks, %d frames, %d failed; %.2f ms a chunk in the decoder, "
		"%.1f s in all\n", chunks, written, failures,
		chunks > 0 ? decodeTime / 1000.0 / chunks : 0.0, elapsed / 1e6);
	if (out != NULL)
		fclose(out);
	nvdecVp9Destroy(decoder);
	nvdecClose(engine);
	return failures > 0 ? 1 : 0;
}
