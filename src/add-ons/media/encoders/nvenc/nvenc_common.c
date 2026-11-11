/* See nvenc_common.h. */

#include "nvenc_common.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "video/nvenc_drv.h"

void
nvencDumpSetup(const void *data, size_t size, int picture)
{
	const char *directory = getenv("NVENC_DUMP");
	if (directory == NULL)
		return;
	char name[512];
	snprintf(name, sizeof(name), "%s/ours-%04d.bin", directory, picture);
	FILE *file = fopen(name, "wb");
	if (file == NULL)
		return;
	fwrite(data, 1, size, file);
	fclose(file);
}

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

/* Hints from the neighbours, the previous picture's vectors (the motion
 * predictor buffers), the co-located block and zero, each searched with
 * its own pattern of points. */
void
nvencFillMeControl(void *control, bool bPicture)
{
	nvenc_h264_me_control_s *me = control;
	memset(me, 0, sizeof(*me));
	me->refinement_mode = 1;		/* quarter pel */
	me->refine_on_search_enable = 1;
	me->fps_mvcost = 1;
	me->sps_mvcost = 1;
	me->sps_cost_func = 1;

	me->predsrc.self_spatial_enable = 1;
	me->predsrc.self_spatial_search = 1;
	me->predsrc.self_spatial_stamp_l0 = 2;
	me->predsrc.self_spatial_stamp_l1 = 3;
	me->predsrc.self_temporal_enable = 1;
	me->predsrc.self_temporal_search = 1;
	me->predsrc.self_temporal_stamp_l0 = 4;
	me->predsrc.self_temporal_stamp_l1 = 5;
	/* A B picture's direct and skipped blocks take their vectors from the
	 * co-located data of the picture after it, which the engine only reads
	 * with this set (and SET_IN_COLOC_DATA given: P pictures fail with it). */
	me->predsrc.coloc_enable = bPicture;
	me->predsrc.coloc_stamp_l0 = 1;
	me->predsrc.coloc_stamp_l1 = 1;
	me->predsrc.const_mv_enable = 1;
	me->predsrc.const_mv_search = 1;
	me->predsrc.const_mv_stamp_l1 = 1;
	me->predsrc.stamp_refidx1_stamp = 6;
	me->predsrc.stamp_refidx2_stamp = 6;
	me->predsrc.stamp_refidx3_stamp = 6;

	/* Two patterns on an 8x8 grid of points, a line of eight with a column
	 * through it and a diamond. */
	nvenc_h264_me_stamp_s *stamps[] = { &me->shape0, &me->shape1, &me->shape2,
		&me->shape3, &me->shape4, &me->shape5, &me->shape6 };
	static const bool kDiamond[] = { false, true, false, true, false, false, true };
	for (int i = 0; i < 7; i++) {
		stamps[i]->bitmask[0] = kDiamond[i] ? 0x1e1e0c00 : 0xff0c0c00;
		stamps[i]->bitmask[1] = kDiamond[i] ? 0x0000000c : 0x00000c0c;
		stamps[i]->hor_adjust = 1;
		stamps[i]->ver_adjust = kDiamond[i];
	}
	me->spatial_hint_pattern = 6;
	me->temporal_hint_pattern = 4;
	me->external_hint_order = 1;
	me->coloc_hint_order = 1;
	/* the five hint kinds in order; the header says they must all appear */
	me->hint_type0 = 0;
	me->hint_type1 = 1;
	me->hint_type2 = 2;
	me->hint_type3 = 3;
	me->hint_type4 = 4;
	me->fbm_op_winner_num_p_frame = 1;
	me->fbm_op_winner_num_b_frame_l0 = 1;
	me->fbm_op_winner_num_b_frame_l1 = 1;
	me->Cu16partDecisionMadeByFPP = 1;
	me->fbm_select_best_cu32_parttype_num = 3;
	me->sps_evaluate_merge_cand_num = 5;
	me->fps_quad_thresh_hold = 60;
}


/* The arrangement the engine reads its input in and writes its reference
 * pictures in: the video block linear one NVDEC writes, two groups of eight
 * lines to a block (found on a GTX 1070; NVIDIA's driver configures an RTX
 * 3050 the same way). With it go the parts of H.264 a card may turn out not
 * to encode right: MediaConverter's test tries the ways in turn and uses
 * the first that gives its pictures back, so a card that fails B pictures
 * or the 8x8 transform still encodes without them. */
const NvencLayout kNvencLayouts[] = {
	{ false, true, 2, false, true, true, "video block linear" },
	{ false, true, 2, false, false, true, "video block linear, no B pictures" },
	{ false, true, 2, false, false, false,
		"video block linear, no B pictures, Main profile" },
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

/* The quantiser step of a QP, as x264 counts it: doubling every six. */
static double
qp_to_step(double qp)
{
	return 0.85 * pow(2.0, (qp - 12.0) / 6.0);
}

static double
step_to_qp(double step)
{
	return 12.0 + 6.0 * log2(step / 0.85);
}

/* I pictures get a finer quantiser than the P pictures around them and B
 * pictures, which nothing is predicted from, a coarser one. */
static const int kQpOffset[NVENC_PICTURE_KINDS] = { 0, -3, 2 };

void
nvencRateInit(NvencRateControl *rc, const nvenc_config *config, int minQp, int maxQp,
	int startQp, int bPictures)
{
	memset(rc, 0, sizeof(*rc));
	double fps = config->frame_rate_den != 0
		? (double)config->frame_rate_num / config->frame_rate_den : 30.0;
	if (fps <= 0)
		fps = 30.0;
	rc->minQp = minQp;
	rc->maxQp = maxQp;
	rc->keyInterval = config->key_interval > 0 ? config->key_interval : 250;
	rc->bPictures = bPictures;
	if (config->rate_control == NVENC_RC_CONSTQP || config->bitrate == 0) {
		rc->qp = config->qp;
		return;
	}
	rc->targetPerPicture = config->bitrate / fps;
	/* An average rate may wander half a second's worth from its budget
	 * before the quantiser step doubles, a constant one a quarter of a
	 * second's; the costs are averaged over two seconds and one. (On a GTX
	 * 1070 a 12 second 1080p clip came out as good with these as with four
	 * times the room, and 1 to 3 % over its rate instead of 5 to 10.) */
	bool constant = config->rate_control == NVENC_RC_CBR;
	rc->bufferBits = config->bitrate * (constant ? 0.25 : 0.5);
	rc->window = fps * (constant ? 1.0 : 2.0);

	/* Until a picture has been encoded: about QP 26 for a tenth of a bit per
	 * pixel, six QP per doubling */
	double bitsPerPixel = config->bitrate
		/ ((double)config->width * config->height * fps);
	double qp = startQp - 6.0 * log2(bitsPerPixel / 0.1);
	rc->qp = qp < minQp + 6 ? minQp + 6 : qp > maxQp - 6 ? maxQp - 6 : qp;
}

static int
clamp_qp(const NvencRateControl *rc, double qp)
{
	int rounded = (int)lround(qp);
	return rounded < rc->minQp ? rc->minQp : rounded > rc->maxQp ? rc->maxQp : rounded;
}

/* The P picture QP at which the stream meets its budget, from what the
 * pictures so far cost. */
static double
rate_estimate(const NvencRateControl *rc)
{
	const double *cost = rc->cost;
	double p, i, b;
	if (rc->seen[NVENC_PICTURE_P] > 0)
		p = cost[NVENC_PICTURE_P];
	else if (rc->seen[NVENC_PICTURE_B] > 0)
		p = cost[NVENC_PICTURE_B] * 1.5;
	else
		p = cost[NVENC_PICTURE_I] / 6;
	i = rc->seen[NVENC_PICTURE_I] > 0 ? cost[NVENC_PICTURE_I] : p * 6;
	b = rc->seen[NVENC_PICTURE_B] > 0 ? cost[NVENC_PICTURE_B] : p / 1.5;

	/* the share of each kind: one I picture per key interval, or as many
	 * as there were when the caller asked for more */
	int total = rc->seen[0] + rc->seen[1] + rc->seen[2];
	double iShare = 1.0 / rc->keyInterval;
	if (rc->seen[NVENC_PICTURE_I] > 1 && (double)rc->seen[NVENC_PICTURE_I] / total > iShare)
		iShare = (double)rc->seen[NVENC_PICTURE_I] / total;
	double bShare = (1 - iShare) * rc->bPictures / (rc->bPictures + 1);
	double pShare = 1 - iShare - bShare;
	double perPicture = iShare * i / pow(2.0, kQpOffset[NVENC_PICTURE_I] / 6.0)
		+ pShare * p + bShare * b / pow(2.0, kQpOffset[NVENC_PICTURE_B] / 6.0);

	/* and six QP for every buffer the stream is from its budget */
	double overflow = (rc->totalBits - rc->wantedBits) / rc->bufferBits;
	overflow = overflow < -2.0 ? -2.0 : overflow > 2.0 ? 2.0 : overflow;
	return step_to_qp(perPicture / rc->targetPerPicture) + 6.0 * overflow;
}

int
nvencRateQp(NvencRateControl *rc, int kind)
{
	if (rc->targetPerPicture <= 0) {
		/* constant QP: the one asked for, B pictures two above it */
		return rc->usedQp[kind] = clamp_qp(rc, rc->qp
			+ (kind == NVENC_PICTURE_B ? kQpOffset[NVENC_PICTURE_B] : 0));
	}
	int total = rc->seen[0] + rc->seen[1] + rc->seen[2];
	if (total > 0) {
		double qp = rate_estimate(rc);
		/* two QP a picture at most */
		if (qp > rc->qp + 2)
			qp = rc->qp + 2;
		if (qp < rc->qp - 2)
			qp = rc->qp - 2;
		rc->qp = qp < rc->minQp ? rc->minQp : qp > rc->maxQp ? rc->maxQp : qp;
	}
	return rc->usedQp[kind] = clamp_qp(rc, rc->qp + kQpOffset[kind]);
}

int
nvencRateRetry(NvencRateControl *rc, size_t bytes, int kind)
{
	if (rc->targetPerPicture <= 0 || rc->retried
		|| rc->seen[0] + rc->seen[1] + rc->seen[2] > 0)
		return -1;
	rc->retried = true;
	NvencRateControl trial = *rc;
	trial.cost[kind] = bytes * 8.0 * qp_to_step(rc->usedQp[kind]);
	trial.seen[kind] = 1;
	int qp = clamp_qp(rc, rate_estimate(&trial) + kQpOffset[kind]);
	if (abs(qp - rc->usedQp[kind]) < 3)
		return -1;
	rc->qp = qp - kQpOffset[kind];
	return rc->usedQp[kind] = qp;
}

void
nvencRateUpdate(NvencRateControl *rc, size_t bytes, int kind)
{
	if (rc->targetPerPicture <= 0)
		return;
	double bits = bytes * 8.0;
	rc->totalBits += bits;
	rc->wantedBits += rc->targetPerPicture;
	if (getenv("NVENC_RC_DEBUG") != NULL) {
		fprintf(stderr, "nvenc rc: %c qp %d, %zu bytes, %+.2f pictures from the budget\n",
			"PIB"[kind], rc->usedQp[kind], bytes,
			(rc->totalBits - rc->wantedBits) / rc->targetPerPicture);
	}
	double cost = bits * qp_to_step(rc->usedQp[kind]);
	if (rc->seen[kind] == 0)
		rc->cost[kind] = cost;
	else {
		/* a change of scene costs a P picture many times the usual and says
		 * little about the pictures that follow */
		if (kind != NVENC_PICTURE_I && cost > 4 * rc->cost[kind])
			cost = 4 * rc->cost[kind];
		double window = kind == NVENC_PICTURE_I ? 2 : rc->window;
		double weight = 1.0 / (rc->seen[kind] + 1);
		if (weight < 1.0 / window)
			weight = 1.0 / window;
		rc->cost[kind] += (cost - rc->cost[kind]) * weight;
	}
	rc->seen[kind]++;
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
