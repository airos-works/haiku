/* See nvenc_common.h. */

#include "nvenc_common.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void
nvencBitsInit(NvencBits *bits, uint8_t *data, size_t capacity)
{
	bits->data = data;
	bits->capacity = capacity;
	bits->bits = 0;
	memset(data, 0, capacity);
}

void
nvencPut(NvencBits *bits, uint32_t value, int count)
{
	for (int i = count - 1; i >= 0; i--) {
		size_t byte = bits->bits / 8;
		if (byte >= bits->capacity)
			return;
		if ((value >> i) & 1)
			bits->data[byte] |= 0x80 >> (bits->bits % 8);
		bits->bits++;
	}
}

void
nvencPutUe(NvencBits *bits, uint32_t value)
{
	uint64_t coded = (uint64_t)value + 1;
	int length = 0;
	while ((coded >> length) > 1)
		length++;
	nvencPut(bits, 0, length);
	nvencPut(bits, (uint32_t)coded, length + 1);
}

void
nvencPutSe(NvencBits *bits, int32_t value)
{
	nvencPutUe(bits, value > 0 ? 2 * (uint32_t)value - 1 : 2 * (uint32_t)(-value));
}

void
nvencTrailingBits(NvencBits *bits)
{
	nvencPut(bits, 1, 1);
	while (bits->bits % 8 != 0)
		nvencPut(bits, 0, 1);
}

size_t
nvencAppendNal(uint8_t *out, size_t outSize, size_t at, const uint8_t *header,
	int headerSize, const NvencBits *bits)
{
	static const uint8_t kStartCode[4] = { 0, 0, 0, 1 };
	if (at + 4 + headerSize > outSize)
		return at;
	memcpy(out + at, kStartCode, 4);
	at += 4;
	memcpy(out + at, header, headerSize);
	at += headerSize;
	size_t bytes = (bits->bits + 7) / 8;
	int zeros = 0;
	for (size_t i = 0; i < bytes && at + 2 <= outSize; i++) {
		uint8_t byte = bits->data[i];
		if (zeros >= 2 && byte <= 3) {
			out[at++] = 3;
			zeros = 0;
		}
		out[at++] = byte;
		zeros = byte == 0 ? zeros + 1 : 0;
	}
	return at;
}

/* Most likely first: the arrangement NVDEC writes, which NVDEC-to-NVENC
 * transcoding hands the encoder unchanged, then the tiled 16x16 one the
 * engine's reference pictures must use, then the graphics arrangement. */
const NvencLayout kNvencLayouts[] = {
	{ false, true, 1, true, "video block linear, offsets from the base" },
	{ false, true, 1, false, "video block linear, chroma addressed" },
	{ true, false, 0, true, "tiled 16x16, offsets from the base" },
	{ true, false, 0, false, "tiled 16x16, chroma addressed" },
	{ false, true, 2, true, "video block linear (height 2), offsets from the base" },
	{ false, false, 1, true, "graphics block linear, offsets from the base" },
	{ false, false, 1, false, "graphics block linear, chroma addressed" },
};
const uint32_t kNvencLayoutCount = sizeof(kNvencLayouts) / sizeof(kNvencLayouts[0]);

void
nvencPictureLayout(NvencPictureLayout *layout, int width, int height,
	int bytesPerSample, int alignWidth, int alignHeight)
{
	memset(layout, 0, sizeof(*layout));
	layout->width = width;
	layout->height = height;
	layout->bytesPerSample = bytesPerSample;
	layout->alignedWidth = (width + alignWidth - 1) / alignWidth * alignWidth;
	/* 32 lines: chroma is half as tall and must still fill whole blocks of
	 * sixteen lines */
	if (alignHeight < 32)
		alignHeight = 32;
	layout->alignedHeight = (height + alignHeight - 1) / alignHeight * alignHeight;
	layout->pitch = (layout->alignedWidth * bytesPerSample + 255) / 256 * 256;
	layout->chromaOffset = (size_t)layout->pitch * layout->alignedHeight;
	layout->size = layout->chromaOffset + layout->chromaOffset / 2 + 4096;
}

/* Sixteen bytes of a line at `x` bytes in, repeating the last `unit` bytes
 * (a sample, or a pair of chroma samples) past the right edge and the last
 * line past the bottom. */
static void
fetch(uint8_t chunk[16], const uint8_t *plane, uint32_t stride, int widthBytes,
	int height, int x, int y, int unit)
{
	if (y >= height)
		y = height - 1;
	const uint8_t *line = plane + (size_t)y * stride;
	if (x + 16 <= widthBytes) {
		memcpy(chunk, line + x, 16);
		return;
	}
	for (int i = 0; i < 16; i += unit) {
		int at = x + i;
		if (at > widthBytes - unit)
			at = widthBytes - unit;
		memcpy(chunk + i, line + at, unit);
	}
}

static size_t
place(const NvencLayout *layout, int x, int y, int pitch, int tileLines)
{
	if (layout->tiled) {
		/* tiles of 16 bytes by `tileLines` lines, across and then down */
		return (size_t)(y / tileLines) * pitch * tileLines + (size_t)(x / 16) * 16 * tileLines
			+ (size_t)(y % tileLines) * 16 + (x % 16);
	}
	return nvencBlockLinearOffset(x, y, pitch, layout->linesInFours);
}

static void
store_plane(uint8_t *out, const NvencPictureLayout *picture, const NvencLayout *layout,
	const uint8_t *plane, uint32_t stride, int widthBytes, int height, int lines, int unit,
	int tileLines)
{
	int alignedBytes = picture->alignedWidth * picture->bytesPerSample;
	uint8_t chunk[16];
	for (int y = 0; y < lines; y++) {
		for (int x = 0; x < alignedBytes; x += 16) {
			fetch(chunk, plane, stride, widthBytes, height, x, y, unit);
			memcpy(out + place(layout, x, y, picture->pitch, tileLines), chunk, 16);
		}
	}
}

void
nvencStorePicture(uint8_t *out, const NvencPictureLayout *picture,
	const NvencLayout *layout, const nvenc_picture *source)
{
	int bps = picture->bytesPerSample;
	store_plane(out, picture, layout, source->planes[0], source->strides[0],
		picture->width * bps, picture->height, picture->alignedHeight, bps, 16);
	/* Interleaved chroma: as many bytes per line as luma, half the lines. A
	 * macroblock's chroma is 16 bytes by 8 lines. */
	store_plane(out + picture->chromaOffset, picture, layout, source->planes[1],
		source->strides[1], (picture->width + 1) / 2 * 2 * bps, (picture->height + 1) / 2,
		picture->alignedHeight / 2, 2 * bps, 8);
}

bool
nvencQueuePush(NvencQueue *queue, const uint8_t *data, size_t size, int64_t pts,
	int64_t dts, bool key)
{
	NvencPacket *packet = calloc(1, sizeof(NvencPacket));
	if (packet == NULL)
		return false;
	packet->data = malloc(size);
	if (packet->data == NULL) {
		free(packet);
		return false;
	}
	memcpy(packet->data, data, size);
	packet->size = size;
	packet->pts = pts;
	packet->dts = dts;
	packet->key = key;
	if (queue->last != NULL)
		queue->last->next = packet;
	else
		queue->first = packet;
	queue->last = packet;
	return true;
}

NvencPacket *
nvencQueuePop(NvencQueue *queue)
{
	NvencPacket *packet = queue->first;
	if (packet == NULL)
		return NULL;
	queue->first = packet->next;
	if (queue->first == NULL)
		queue->last = NULL;
	packet->next = NULL;
	return packet;
}

void
nvencQueueClear(NvencQueue *queue)
{
	NvencPacket *packet;
	while ((packet = nvencQueuePop(queue)) != NULL) {
		free(packet->data);
		free(packet);
	}
}

void
nvencRateInit(NvencRateControl *rc, const nvenc_config *config, int minQp, int maxQp,
	int startQp)
{
	memset(rc, 0, sizeof(*rc));
	double fps = config->frame_rate_den != 0
		? (double)config->frame_rate_num / config->frame_rate_den : 30.0;
	if (fps <= 0)
		fps = 30.0;
	rc->targetPerPicture = config->bitrate / fps;
	rc->minQp = minQp;
	rc->maxQp = maxQp;
	rc->constrained = config->rate_control == NVENC_RC_CBR;
	if (config->rate_control == NVENC_RC_CONSTQP || config->bitrate == 0) {
		rc->qp = config->qp;
		rc->targetPerPicture = 0;
		return;
	}
	/* About QP 26 for a tenth of a bit per pixel, six QP per doubling */
	double bitsPerPixel = config->bitrate
		/ ((double)config->width * config->height * fps);
	double qp = startQp - 6.0 * log2(bitsPerPixel / 0.1);
	rc->qp = qp < minQp + 6 ? minQp + 6 : qp > maxQp - 6 ? maxQp - 6 : qp;
}

int
nvencRateQp(const NvencRateControl *rc, bool intra)
{
	int qp = (int)lround(rc->qp) - (intra && rc->targetPerPicture > 0 ? 2 : 0);
	return qp < rc->minQp ? rc->minQp : qp > rc->maxQp ? rc->maxQp : qp;
}

void
nvencRateUpdate(NvencRateControl *rc, size_t bytes, bool intra)
{
	if (rc->targetPerPicture <= 0)
		return;
	/* A key picture costs about what four others do; it is spread over the
	 * pictures that follow it rather than answered at once. */
	double bits = bytes * 8.0;
	rc->fullness = rc->fullness * (rc->constrained ? 0.9 : 0.97)
		+ (bits - rc->targetPerPicture * (intra ? 4 : 1));
	double window = rc->targetPerPicture * (rc->constrained ? 8 : 30);
	double ratio = (window + rc->fullness) / window;
	if (ratio < 0.25)
		ratio = 0.25;
	double step = 6.0 * log2(ratio) * (rc->constrained ? 0.5 : 0.25);
	double limit = rc->constrained ? 2.0 : 1.0;
	step = step < -limit ? -limit : step > limit ? limit : step;
	rc->qp += step;
	if (rc->qp < rc->minQp)
		rc->qp = rc->minQp;
	if (rc->qp > rc->maxQp)
		rc->qp = rc->maxQp;
}

int
nvencH264Level(int width, int height, double fps, uint32_t bitrate)
{
	static const struct {
		int	level;
		long	macroblocksPerSecond;
		long	frameSize;
		long	bitrate;	/* kbit/s, Main */
	} kLevels[] = {
		{ 30, 40500, 1620, 10000 }, { 31, 108000, 3600, 14000 },
		{ 32, 216000, 5120, 20000 }, { 40, 245760, 8192, 20000 },
		{ 41, 245760, 8192, 50000 }, { 42, 522240, 8704, 50000 },
		{ 50, 589824, 22080, 135000 }, { 51, 983040, 36864, 240000 },
		{ 52, 2073600, 36864, 240000 }, { 60, 4177920, 139264, 240000 },
		{ 61, 8355840, 139264, 480000 }, { 62, 16711680, 139264, 800000 },
	};
	long frameSize = (long)((width + 15) / 16) * ((height + 15) / 16);
	long perSecond = (long)(frameSize * (fps > 0 ? fps : 30));
	for (size_t i = 0; i < sizeof(kLevels) / sizeof(kLevels[0]); i++) {
		if (frameSize <= kLevels[i].frameSize
			&& perSecond <= kLevels[i].macroblocksPerSecond
			&& bitrate / 1000 <= (uint32_t)kLevels[i].bitrate)
			return kLevels[i].level;
	}
	return 62;
}

int
nvencHevcLevel(int width, int height, double fps)
{
	static const struct {
		int	level;		/* general_level_idc */
		long	pictureSize;
		long	sampleRate;
	} kLevels[] = {
		{ 60, 122880, 3686400 }, { 63, 245760, 7372800 },
		{ 90, 552960, 16588800 }, { 93, 983040, 33177600 },
		{ 120, 2228224, 66846720 }, { 123, 2228224, 133693440 },
		{ 150, 8912896, 267386880 }, { 153, 8912896, 534773760 },
		{ 156, 8912896, 1069547520 }, { 180, 35651584, 1069547520 },
		{ 183, 35651584, 2139095040 },
	};
	long size = (long)width * height;
	long rate = (long)(size * (fps > 0 ? fps : 30));
	for (size_t i = 0; i < sizeof(kLevels) / sizeof(kLevels[0]); i++) {
		if (size <= kLevels[i].pictureSize && rate <= kLevels[i].sampleRate)
			return kLevels[i].level;
	}
	return 186;
}
