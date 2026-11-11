/* What the codecs of the NVENC add-on share: writing headers bit by bit,
 * putting pictures into the arrangement the engine reads, and the queue of
 * finished packets. */
#ifndef NVENC_COMMON_H
#define NVENC_COMMON_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <nvenc_api.h>

#include "nvenc_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Writing a header. Bits go in most significant first. */
typedef struct {
	uint8_t		*data;
	size_t		capacity;
	size_t		bits;
} NvencBits;

void nvencBitsInit(NvencBits *bits, uint8_t *data, size_t capacity);
void nvencPut(NvencBits *bits, uint32_t value, int count);
void nvencPutUe(NvencBits *bits, uint32_t value);
void nvencPutSe(NvencBits *bits, int32_t value);
void nvencTrailingBits(NvencBits *bits);
/* Append the RBSP in `bits` to `out` as an Annex B NAL with a four byte
 * start code, `header` bytes of NAL header first, inserting emulation
 * prevention bytes. Returns the new length of `out`. */
size_t nvencAppendNal(uint8_t *out, size_t outSize, size_t at,
	const uint8_t *header, int headerSize, const NvencBits *bits);

/* How a picture is laid out for the engine (kNvencLayouts in
 * nvenc_common.c). MediaConverter encodes a test picture with each way the
 * add-on knows and uses the first that decodes. */
typedef struct {
	bool		tiled;		/* tiled 16x16 rather than block linear */
	bool		linesInFours;	/* the block linear arrangement */
	uint8_t		blockHeight;	/* the surface configuration's field */
	bool		chromaFromBase;	/* chroma address = base + configured offset */
	const char	*name;
} NvencLayout;

extern const NvencLayout kNvencLayouts[];
extern const uint32_t kNvencLayoutCount;

/* A picture's place in a buffer: luma at 0, chroma after it, both 256 byte
 * aligned. */
typedef struct {
	int		width, height;		/* the picture */
	int		alignedWidth, alignedHeight;
	int		pitch;			/* bytes, both planes */
	int		bytesPerSample;
	size_t		chromaOffset;
	size_t		size;
} NvencPictureLayout;

void nvencPictureLayout(NvencPictureLayout *layout, int width, int height,
	int bytesPerSample, int alignWidth, int alignHeight);
/* Put an NV12 or P010 picture into `out` the way `layout` says, repeating
 * the last column and line into the alignment. */
void nvencStorePicture(uint8_t *out, const NvencPictureLayout *picture,
	const NvencLayout *layout, const nvenc_picture *source);

/* The motion search control the codecs share (an nvenc_h264_me_control_s,
 * which H.265 uses too). */
void nvencFillMeControl(void *control);

/* Finished packets, oldest first. */
typedef struct NvencPacket {
	struct NvencPacket *next;
	uint8_t		*data;
	size_t		size;
	int64_t		pts;
	int64_t		dts;
	bool		key;
} NvencPacket;

typedef struct {
	NvencPacket	*first, *last;
} NvencQueue;

bool nvencQueuePush(NvencQueue *queue, const uint8_t *data, size_t size,
	int64_t pts, int64_t dts, bool key);
NvencPacket *nvencQueuePop(NvencQueue *queue);
void nvencQueueClear(NvencQueue *queue);

/* The rate control of the bit rate modes. The engine is always run at a
 * constant QP per picture; the QP comes from what the pictures so far cost
 * (bits times quantiser step, per kind of picture), the share each kind has
 * in the stream and how far the stream is from its budget. */
enum {
	NVENC_PICTURE_P = 0,
	NVENC_PICTURE_I,
	NVENC_PICTURE_B,
	NVENC_PICTURE_KINDS
};

typedef struct {
	double		targetPerPicture;	/* bits; 0: constant QP */
	double		bufferBits;		/* how far from the budget doubles the step */
	double		window;			/* pictures the costs are averaged over */
	double		qp;			/* of P pictures: the last one chosen */
	int		minQp, maxQp;
	int		keyInterval;
	int		bPictures;		/* between two reference pictures */
	double		cost[NVENC_PICTURE_KINDS];	/* bits x step, averaged */
	int		seen[NVENC_PICTURE_KINDS];
	int		usedQp[NVENC_PICTURE_KINDS];
	double		totalBits, wantedBits;
	bool		retried;
} NvencRateControl;

void nvencRateInit(NvencRateControl *rc, const nvenc_config *config, int minQp,
	int maxQp, int startQp, int bPictures);
/* The QP for the next picture of a kind. */
int nvencRateQp(NvencRateControl *rc, int kind);
/* After the stream's first picture: the QP to encode it again with when the
 * guess it was encoded with turned out far off, else -1. */
int nvencRateRetry(NvencRateControl *rc, size_t bytes, int kind);
void nvencRateUpdate(NvencRateControl *rc, size_t bytes, int kind);

/* Level tables */
int nvencH264Level(int width, int height, double fps, uint32_t bitrate);
int nvencHevcLevel(int width, int height, double fps);

#ifdef __cplusplus
}
#endif

#endif	/* NVENC_COMMON_H */
