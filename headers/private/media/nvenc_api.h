/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _NVENC_API_H
#define _NVENC_API_H


/*!	The interface of the nvenc add-on: NVIDIA's video encoder (NVENC),
	driven directly through the nvidia_rm resource manager.

	The add-on is built outside the tree with the NVDEC add-on, because both
	need NVIDIA's resource manager headers, and installed as
	system/non-packaged/add-ons/media/encoders/nvenc. MediaConverter finds it
	with load_add_on() and get_image_symbol(NVENC_GET_API_SYMBOL); a machine
	without it, without nvidia_rm or without an NVIDIA card encodes on the
	CPU instead. */


#include <stddef.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif


#define NVENC_API_VERSION		1
#define NVENC_GET_API_SYMBOL	"nvenc_get_api"


/* codecs, also bits of the probe's mask */
enum {
	NVENC_CODEC_H264		= 0x01,
	NVENC_CODEC_HEVC		= 0x02,
	NVENC_CODEC_HEVC_10		= 0x04,
	NVENC_CODEC_AV1			= 0x08,
	NVENC_CODEC_AV1_10		= 0x10,
};


/* rate control */
enum {
	NVENC_RC_CONSTQP		= 0,
	NVENC_RC_VBR			= 1,
	NVENC_RC_CBR			= 2,
};


typedef struct nvenc_session nvenc_session;


typedef struct {
	uint32_t	codec;				/* one NVENC_CODEC_* */
	uint32_t	width;
	uint32_t	height;
	uint32_t	frame_rate_num;
	uint32_t	frame_rate_den;
	uint32_t	sar_num;			/* 0 when unknown */
	uint32_t	sar_den;
	uint32_t	rate_control;		/* NVENC_RC_* */
	uint32_t	qp;					/* for NVENC_RC_CONSTQP: H.264/H.265 QP, */
									/* AV1 qindex */
	uint32_t	bitrate;			/* bit/s, for VBR and CBR */
	uint32_t	max_bitrate;
	uint32_t	key_interval;		/* pictures */
	uint32_t	preset;				/* 1 fastest .. 7 best */
	uint8_t		colour_primaries;	/* ITU-T H.273 codes */
	uint8_t		transfer;
	uint8_t		matrix;
	uint8_t		full_range;
	uint8_t		chroma_location;	/* H.273, 0 = left */
	uint8_t		reserved[3];
	uint32_t	reserved2[8];
} nvenc_config;


/* A picture in NV12 (8 bits) or P010 (10 bits) */
typedef struct {
	const uint8_t*	planes[2];		/* luma, interleaved chroma */
	uint32_t		strides[2];		/* bytes */
	int64_t			pts;
	uint32_t		force_key;
	uint32_t		reserved;
} nvenc_picture;


typedef struct {
	const uint8_t*	data;			/* Annex B or AV1 temporal unit */
	size_t			size;
	int64_t			pts;
	int64_t			dts;
	uint32_t		key;
	uint32_t		reserved;
	void*			cookie;			/* the add-on's */
} nvenc_packet;


typedef struct {
	uint32_t	version;			/* NVENC_API_VERSION */

	/* What the card can encode, as a mask of NVENC_CODEC_*, and its name
	 * (e.g. "NVIDIA GeForce GTX 1070"). Returns 0, or -1 and a reason. */
	int			(*probe)(uint32_t* codecs, char* name, size_t nameSize,
					char* reason, size_t reasonSize);

	nvenc_session* (*open)(const nvenc_config* config, char* reason,
					size_t reasonSize);
	/* The parameter sets (or AV1 sequence header) of the stream. */
	int			(*headers)(nvenc_session* session, uint8_t* buffer,
					size_t* size);
	/* picture == NULL drains the encoder. Returns 0 or -1. */
	int			(*encode)(nvenc_session* session,
					const nvenc_picture* picture);
	/* 0: a packet, 1: none yet, 2: drained, -1: error */
	int			(*receive)(nvenc_session* session, nvenc_packet* packet);
	void		(*release)(nvenc_session* session, nvenc_packet* packet);
	void		(*close)(nvenc_session* session);
	/* "constant QP 24, P pictures" */
	const char*	(*describe)(nvenc_session* session);
} nvenc_api;


typedef const nvenc_api* (*nvenc_get_api_func)(void);


#ifdef __cplusplus
}
#endif


#endif	/* _NVENC_API_H */
