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

/* How a picture is laid out for the engine. Nothing in the class headers
 * says which arrangement each generation reads input in or how the chroma
 * address is taken, so the add-on knows a few and the caller tries them:
 * MediaConverter encodes a test picture with each until one decodes. */
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

/* A simple rate control for the bit rate modes: the engine is always run at
 * a constant QP, and the QP moves with how far the stream is from its
 * budget. */
typedef struct {
	double		targetPerPicture;	/* bits */
	double		fullness;		/* bits over budget, decaying */
	double		qp;
	int		minQp, maxQp;
	bool		constrained;		/* CBR: react harder */
} NvencRateControl;

void nvencRateInit(NvencRateControl *rc, const nvenc_config *config, int minQp,
	int maxQp, int startQp);
int nvencRateQp(const NvencRateControl *rc, bool intra);
void nvencRateUpdate(NvencRateControl *rc, size_t bytes, bool intra);

/* Level tables */
int nvencH264Level(int width, int height, double fps, uint32_t bitrate);
int nvencHevcLevel(int width, int height, double fps);

#ifdef __cplusplus
}
#endif

#endif	/* NVENC_COMMON_H */
