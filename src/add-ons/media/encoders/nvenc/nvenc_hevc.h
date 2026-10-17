/* H.265 on NVENC: Main and Main 10, I and P pictures, one reference, one
 * slice per picture, 32x32 coding tree blocks. The engine writes the slice
 * segments; the parameter sets are written here. */
#ifndef NVENC_HEVC_H
#define NVENC_HEVC_H

#include "nvenc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NvencHevc NvencHevc;

NvencHevc *nvencHevcOpen(NvencEngine *engine, const nvenc_config *config,
	char *reason, size_t reasonSize);
void nvencHevcClose(NvencHevc *encoder);
/* VPS, SPS and PPS in Annex B */
size_t nvencHevcHeaders(NvencHevc *encoder, uint8_t *buffer, size_t size);
/* 0, NVENC_FAILED or NVENC_TIMED_OUT */
int nvencHevcEncode(NvencHevc *encoder, const nvenc_picture *picture,
	NvencQueue *queue, char *reason, size_t reasonSize);
const char *nvencHevcDescribe(NvencHevc *encoder);

#ifdef __cplusplus
}
#endif

#endif	/* NVENC_HEVC_H */
