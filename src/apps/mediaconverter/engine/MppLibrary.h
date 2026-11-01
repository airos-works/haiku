/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef MPP_LIBRARY_H
#define MPP_LIBRARY_H


#include <stddef.h>
#include <stdint.h>


/*!	The part of Rockchip MPP's C interface (librockchip_mpp.so.1, which the
	rock5_ffmpeg package installs) the RK3588 encoder uses, opened at run
	time like the other encoders' libraries so MediaConverter needs neither
	MPP's headers to build nor MPP to start.

	The values are those of MPP 1.0 as airos-works/mpp (airos-14729dd)
	builds it, read from its rk_type.h, rk_mpi.h, rk_mpi_cmd.h, mpp_frame.h,
	mpp_buffer.h, mpp_meta.h and rk_venc_rc.h; the layout of MppApi is
	checked below.
*/

typedef void* MppCtx;
typedef void* MppFrame;
typedef void* MppPacket;
typedef void* MppBuffer;
typedef void* MppBufferGroup;
typedef void* MppMeta;
typedef void* MppEncCfg;
typedef int32_t MPP_RET;

enum {
	kMppOK					= 0,
	kMppErrorTimeout		= -8,

	kMppContextEncoder		= 1,			// MPP_CTX_ENC
	kMppCodingAVC			= 7,			// MPP_VIDEO_CodingAVC
	kMppCodingHEVC			= 0x1000004,	// MPP_VIDEO_CodingHEVC

	kMppFormatNV12			= 0,			// MPP_FMT_YUV420SP
	kMppBufferTypeDRM		= 3,			// MPP_BUFFER_TYPE_DRM
	kMppBufferInternal		= 0,			// MPP_BUFFER_INTERNAL

	kMppSetOutputTimeout	= 0x200007,		// MPP_SET_OUTPUT_TIMEOUT
	kMppEncoderSetConfig	= 0x320001,		// MPP_ENC_SET_CFG
	kMppEncoderSetIDR		= 0x320009,		// MPP_ENC_SET_IDR_FRAME
	kMppEncoderGetHeaders	= 0x32000d,		// MPP_ENC_GET_HDR_SYNC
	kMppEncoderHeaderMode	= 0x328001,		// MPP_ENC_SET_HEADER_MODE
	kMppHeaderEachIDR		= 1,			// MPP_ENC_HEADER_MODE_EACH_IDR

	kMppRateVBR				= 0,			// MPP_ENC_RC_MODE_VBR
	kMppRateCBR				= 1,
	kMppRateFixedQP			= 2,

	kMppMetaOutputIntra		= 0x6f696472	// KEY_OUTPUT_INTRA, 'oidr'
};

struct MppApi {
	uint32_t	size;
	uint32_t	version;
	void*		decode;
	void*		decode_put_packet;
	void*		decode_get_frame;
	void*		encode;
	MPP_RET		(*encode_put_frame)(MppCtx context, MppFrame frame);
	MPP_RET		(*encode_get_packet)(MppCtx context, MppPacket* packet);
	void*		isp;
	void*		isp_put_frame;
	void*		isp_get_frame;
	void*		poll;
	void*		dequeue;
	void*		enqueue;
	MPP_RET		(*reset)(MppCtx context);
	MPP_RET		(*control)(MppCtx context, uint32_t command, void* parameter);
	uint32_t	reserve[16];
};

static_assert(offsetof(MppApi, encode_put_frame) == 40
	&& offsetof(MppApi, encode_get_packet) == 48
	&& offsetof(MppApi, reset) == 104 && offsetof(MppApi, control) == 112
	&& sizeof(MppApi) == 184, "MPP's MppApi layout");


struct MppLibrary {
	MPP_RET		(*create)(MppCtx* context, MppApi** api);
	MPP_RET		(*init)(MppCtx context, int32_t type, int32_t coding);
	MPP_RET		(*destroy)(MppCtx context);

	MPP_RET		(*cfg_init)(MppEncCfg* config);
	MPP_RET		(*cfg_deinit)(MppEncCfg config);
	MPP_RET		(*cfg_set_s32)(MppEncCfg config, const char* name, int32_t value);

	MPP_RET		(*frame_init)(MppFrame* frame);
	MPP_RET		(*frame_deinit)(MppFrame* frame);
	void		(*frame_set_width)(MppFrame frame, uint32_t width);
	void		(*frame_set_height)(MppFrame frame, uint32_t height);
	void		(*frame_set_hor_stride)(MppFrame frame, uint32_t stride);
	void		(*frame_set_ver_stride)(MppFrame frame, uint32_t stride);
	void		(*frame_set_fmt)(MppFrame frame, int32_t format);
	void		(*frame_set_pts)(MppFrame frame, int64_t pts);
	void		(*frame_set_eos)(MppFrame frame, uint32_t eos);
	void		(*frame_set_buffer)(MppFrame frame, MppBuffer buffer);

	MPP_RET		(*packet_init_with_buffer)(MppPacket* packet, MppBuffer buffer);
	MPP_RET		(*packet_deinit)(MppPacket* packet);
	void		(*packet_set_length)(MppPacket packet, size_t length);
	void*		(*packet_get_pos)(const MppPacket packet);
	size_t		(*packet_get_length)(const MppPacket packet);
	int64_t		(*packet_get_pts)(const MppPacket packet);
	int32_t		(*packet_has_meta)(const MppPacket packet);
	MppMeta		(*packet_get_meta)(const MppPacket packet);
	MPP_RET		(*meta_get_s32)(MppMeta meta, int32_t key, int32_t* value);

	// mpp_buffer_group_get_internal(), mpp_buffer_get(), mpp_buffer_put()
	// and mpp_buffer_get_ptr() are macros over these
	MPP_RET		(*buffer_group_get)(MppBufferGroup* group, int32_t type,
					int32_t mode, const char* tag, const char* caller);
	MPP_RET		(*buffer_group_put)(MppBufferGroup group);
	MPP_RET		(*buffer_get_with_tag)(MppBufferGroup group, MppBuffer* buffer,
					size_t size, const char* tag, const char* caller);
	MPP_RET		(*buffer_put_with_caller)(MppBuffer buffer, const char* caller);
	void*		(*buffer_get_ptr_with_caller)(MppBuffer buffer,
					const char* caller);

	// NULL, with why in *error, when the library or one of these is missing
	static	const MppLibrary*	Get(const char** error = NULL);
};


#endif	// MPP_LIBRARY_H
