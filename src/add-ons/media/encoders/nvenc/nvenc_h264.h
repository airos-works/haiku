/* H.264 on NVENC: I, P and B pictures, one reference picture each way, one
 * slice per picture, CABAC, Main profile. The engine writes the slices; the
 * parameter sets are written here. */
#ifndef NVENC_H264_H
#define NVENC_H264_H

#include "nvenc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NvencH264 NvencH264;

NvencH264 *nvencH264Open(NvencEngine *engine, const nvenc_config *config,
	char *reason, size_t reasonSize);
void nvencH264Close(NvencH264 *encoder);
/* SPS and PPS in Annex B */
size_t nvencH264Headers(NvencH264 *encoder, uint8_t *buffer, size_t size);
/* Take one picture. With B pictures it may wait for the reference picture
 * after it; the packets of the pictures that are encoded go on `queue`.
 * Returns 0, NVENC_FAILED or NVENC_TIMED_OUT. */
int nvencH264Encode(NvencH264 *encoder, const nvenc_picture *picture,
	NvencQueue *queue, char *reason, size_t reasonSize);
/* Encode the pictures that still wait. */
int nvencH264Flush(NvencH264 *encoder, NvencQueue *queue, char *reason,
	size_t reasonSize);
const char *nvencH264Describe(NvencH264 *encoder);

#ifdef __cplusplus
}
#endif

#endif	/* NVENC_H264_H */
