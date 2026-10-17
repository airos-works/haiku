/* H.264 on NVENC: I and P pictures, one reference, one slice per picture,
 * CABAC, Main profile. The engine writes the slices; the parameter sets are
 * written here. */
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
/* Encode one picture; the packet goes on `queue`. Returns 0, NVENC_FAILED
 * or NVENC_TIMED_OUT. */
int nvencH264Encode(NvencH264 *encoder, const nvenc_picture *picture,
	NvencQueue *queue, char *reason, size_t reasonSize);
const char *nvencH264Describe(NvencH264 *encoder);

#ifdef __cplusplus
}
#endif

#endif	/* NVENC_H264_H */
