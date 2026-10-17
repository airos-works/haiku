/* The card's video encoder, as much of it as is not about any one codec.
 *
 * This is the NVDEC add-on's engine (src/add-ons/media/plugins/nvdec) put on
 * the encoder: resman, an address space, buffers both sides can see, and a
 * channel on NVENC0 with the card's NVxxB7 object on it. Work is a list of
 * methods ending in EXECUTE; the engine answers by releasing a semaphore.
 *
 * Two kinds of channel: before Volta resman owns the USERD page, which is
 * reached by mapping the channel, and work is kicked by writing GPPut. From
 * Volta on USERD is our memory and a write to the usermode doorbell kicks
 * it, as NVK's resman back end does.
 */
#ifndef NVENC_ENGINE_H
#define NVENC_ENGINE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NvencEngine NvencEngine;

/* A buffer with a place in the GPU's address space and in ours. */
typedef struct {
	uint64_t	gpuAddress;
	void		*data;
	uint64_t	size;
	bool		inSystemMemory;
	void		*driverState;
} NvencBuffer;

/* What the card is. */
typedef struct {
	char		name[96];		/* "NVIDIA GeForce GTX 1070" */
	uint32_t	architecture;		/* NV2080_CTRL_MC_ARCH_INFO_ARCHITECTURE_* */
	uint32_t	implementation;
	uint32_t	encoderClass;		/* NVxxB7, 0 if the card has no NVENC */
	uint32_t	channelClass;
	uint32_t	usermodeClass;		/* 0 before Volta */
	uint32_t	hardwareCodecs;		/* NVENC_CODEC_* the engine has */
} NvencCard;

/* Find the card and what its encoder is, without taking the engine. */
bool nvencIdentify(NvencCard *card, char *reason, size_t reasonSize);

/* Open the encoder. Returns NULL and, if `reason` is not NULL, leaves a line
 * there saying what failed. */
NvencEngine *nvencOpen(char *reason, size_t reasonSize);
void nvencClose(NvencEngine *engine);
const NvencCard *nvencCard(const NvencEngine *engine);

bool nvencAlloc(NvencEngine *engine, NvencBuffer *buffer, uint64_t size,
	bool inSystemMemory);
void nvencFree(NvencEngine *engine, NvencBuffer *buffer);

/* Building a list of methods. `nvencBegin` starts one, the rest add to it, and
 * `nvencExecute` hands it over and waits. */
void nvencBegin(NvencEngine *engine);
void nvencMethod(NvencEngine *engine, uint32_t method, uint32_t value);
/* Addresses the engine is given are always in units of 256 bytes. */
void nvencAddress(NvencEngine *engine, uint32_t method, uint64_t gpuAddress);
/* Returns 0, NVENC_FAILED when the channel reports an error or
 * NVENC_TIMED_OUT; the reason then says what happened. */
int nvencExecute(NvencEngine *engine, int timeoutMs, char *reason,
	size_t reasonSize);

/* Where a pixel of a picture in the video engines' block linear arrangement
 * goes: 512 byte groups of 64 columns and 8 lines, in which lines go in
 * fours (`linesInFours`, what NVDEC writes) or in twos (graphics), two groups
 * stacked into a block sixteen lines tall. See nvdecTileOffset. */
size_t nvencBlockLinearOffset(int x, int y, int pitch, bool linesInFours);

/* The methods of NVxxB7 that every NVENC class from Maxwell to Ada shares. */
#define NVENC_SET_OBJECT			0x0000
#define NVENC_NOP				0x0100
#define NVENC_SET_APPLICATION_ID		0x0200
#define NVENC_SET_WATCHDOG_TIMER		0x0204
#define NVENC_SEMAPHORE_A			0x0240
#define NVENC_SEMAPHORE_B			0x0244
#define NVENC_SEMAPHORE_C			0x0248
#define NVENC_EXECUTE				0x0300
#define NVENC_SEMAPHORE_D			0x0304
#define NVENC_SET_IN_REF_PIC_LUMA(n)		(0x0400 + 4 * (n))
#define NVENC_SET_IN_REF_PIC_CHROMA(n)		(0x0440 + 4 * (n))
#define NVENC_SET_CONTROL_PARAMS		0x0700
#define NVENC_SET_PICTURE_INDEX			0x0704
#define NVENC_SET_IN_RCDATA			0x070c
#define NVENC_SET_IN_DRV_PIC_SETUP		0x0710
#define NVENC_SET_OUT_ENC_STATUS		0x0718
#define NVENC_SET_OUT_BITSTREAM			0x071c
#define NVENC_SET_IOHISTORY			0x0720
#define NVENC_SET_IO_RC_PROCESS			0x0724
#define NVENC_SET_IN_COLOC_DATA			0x0728
#define NVENC_SET_OUT_COLOC_DATA		0x072c
#define NVENC_SET_OUT_REF_PIC_LUMA		0x0730
#define NVENC_SET_IN_CUR_PIC			0x0734
#define NVENC_SET_IN_MEPRED_DATA		0x0738
#define NVENC_SET_OUT_MEPRED_DATA		0x073c
#define NVENC_SET_IN_CUR_PIC_CHROMA_U		0x0740
#define NVENC_SET_IN_CUR_PIC_CHROMA_V		0x0744
#define NVENC_SET_OUT_REF_PIC_CHROMA		0x074c

#define NVENC_APPLICATION_H264			1
#define NVENC_APPLICATION_H265			3
#define NVENC_APPLICATION_AV1			7

/* SET_CONTROL_PARAMS */
#define NVENC_CONTROL_CODEC_H264		3
#define NVENC_CONTROL_CODEC_H265		7	/* the decoder's numbers */
#define NVENC_CONTROL_FORCE_OUT_PIC		(1u << 8)
#define NVENC_CONTROL_FORCE_OUT_COL		(1u << 9)
#define NVENC_CONTROL_DEBUG_MODE		(1u << 16)
#define NVENC_CONTROL_RCMODE(mode)		((uint32_t)(mode) << 24)

#ifdef __cplusplus
}
#endif

#endif	/* NVENC_ENGINE_H */
