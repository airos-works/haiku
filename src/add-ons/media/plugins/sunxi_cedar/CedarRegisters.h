/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef CEDAR_REGISTERS_H
#define CEDAR_REGISTERS_H

/*	The Cedar video engine's decoder registers, offsets from its base
	(0x01c0e000 on the A733). What they mean is public: the register facts
	are those mainline Linux's Cedrus driver, Allwinner's cedar_ve shim and
	the A733 port's own measurements document (the air/OS evidence
	cubie/evidence/video/cedar-register-map.md); no code is taken from
	them. */

#include <stdint.h>

/* top */
#define CEDAR_MODE			0x000
#define   CEDAR_MODE_MPEG		0
#define   CEDAR_MODE_H264		1
#define   CEDAR_MODE_HEVC		4
#define   CEDAR_MODE_DISABLED		7
#define   CEDAR_MODE_DDR_128		(3u << 16)
#define   CEDAR_MODE_DDR_256		(2u << 16)
#define   CEDAR_MODE_REC_WR_2MB		(1u << 20)
#define   CEDAR_MODE_WIDTH_OVER_2048	(1u << 21)
#define   CEDAR_MODE_WIDTH_IS_4096	(1u << 22)
#define CEDAR_RESET			0x004
#define CEDAR_BUF_CTRL			0x050
#define   CEDAR_BUF_INTRA_MIXED		(1u << 2)
#define   CEDAR_BUF_DBLK_MIXED		(1u << 0)
#define CEDAR_DBLK_BUF			0x054
#define CEDAR_INTRA_BUF			0x058
#define CEDAR_DEC_VCU_CFG		0x05c	/* A733 */
#define CEDAR_PRI_CHROMA_LEN		0x0c4
#define CEDAR_PRI_STRIDE		0x0c8
#define CEDAR_SEC_CHROMA_LEN		0x0e8
#define   CEDAR_SEC_FMT_EXT		(1u << 30)
#define CEDAR_OUT_FMT			0x0ec
#define   CEDAR_PRI_FMT_TILED32		(0u << 4)
#define   CEDAR_PRI_FMT_YU12		(2u << 4)
#define   CEDAR_PRI_FMT_NV12		(4u << 4)
#define   CEDAR_SEC_FMT_EXT_NV12	(4u << 0)
#define CEDAR_VERSION			0x0f0
#define CEDAR_TOP_RESET			0x804	/* A733: pulse bits 0 and 4 */

/* H.264 engine */
#define H264_SPS			0x200
#define H264_PPS			0x204
#define H264_SHS			0x208
#define H264_SHS2			0x20c
#define H264_SHS_WP			0x210
#define H264_SHS_QP			0x21c
#define H264_CTRL			0x220
#define   H264_IRQ_MASK			0x7
#define H264_TRIGGER			0x224
#define   H264_TRIG_FLUSH_BITS(n)	(3u | ((uint32_t)(n) & 0x3f) << 8)
#define   H264_TRIG_INIT_SWDEC		7u
#define   H264_TRIG_DECODE_SLICE	8u
#define H264_STATUS			0x228
#define   H264_STATUS_DONE		(1u << 0)
#define   H264_STATUS_ERROR		(1u << 1)
#define   H264_STATUS_DATA_REQ		(1u << 2)
#define   H264_STATUS_VLD_BUSY		(1u << 8)
#define H264_CUR_MB			0x22c
#define H264_VLD_ADDR			0x230
#define H264_VLD_OFFSET			0x234
#define H264_VLD_LEN			0x238
#define H264_VLD_END			0x23c
#define H264_SDROT_CTRL			0x240
#define H264_OUTPUT_FRAME_IDX		0x24c
#define H264_EXTRA_BUF1			0x250	/* picture info */
#define H264_EXTRA_BUF2			0x254	/* neighbour info */
#define H264_ERROR_CASE			0x2b8
#define H264_BASIC_BITS			0x2dc
#define H264_SRAM_OFFSET		0x2e0
#define H264_SRAM_DATA			0x2e4

#define H264_SRAM_WEIGHTS		0x000
#define H264_SRAM_FRAMES		0x100
#define H264_SRAM_LIST0			0x190
#define H264_SRAM_LIST1			0x199
#define H264_SRAM_SCALING_8X8_0		0x200
#define H264_SRAM_SCALING_8X8_1		0x210
#define H264_SRAM_SCALING_4X4		0x220
#define H264_FRAME_SLOTS		18

static inline uint32_t
h264_vld_addr(uint32_t a)
{
	return (a & 0x0ffffff0u) | (a >> 28);
}

/* HEVC engine */
#define HEVC_NAL_HDR			0x500
#define HEVC_SPS			0x504
#define HEVC_PIC_SIZE			0x508
#define HEVC_PCM_CTRL			0x50c
#define HEVC_PPS0			0x510
#define HEVC_PPS1			0x514
#define HEVC_SCALING_CTRL		0x518
#define   HEVC_SCALING_ENABLED		(1u << 31)
#define   HEVC_SCALING_DEFAULT		(1u << 30)
#define HEVC_SLICE0			0x520
#define HEVC_SLICE1			0x524
#define HEVC_SLICE2			0x528
#define HEVC_CTB_ADDR			0x52c
#define HEVC_CTRL			0x530
#define   HEVC_IRQ_MASK			0x7
#define HEVC_TRIGGER			0x534
#define   HEVC_TRIG_SHOW_BITS(n)	(1u | ((uint32_t)(n) & 0x3f) << 8)
#define   HEVC_TRIG_FLUSH_BITS(n)	(3u | ((uint32_t)(n) & 0x3f) << 8)
#define   HEVC_TRIG_INIT_SWDEC		7u
#define   HEVC_TRIG_DECODE_SLICE	8u
#define HEVC_STATUS			0x538
#define   HEVC_STATUS_DONE		(1u << 0)
#define   HEVC_STATUS_ERROR		(1u << 1)
#define   HEVC_STATUS_DATA_REQ		(1u << 2)
#define   HEVC_STATUS_VLD_BUSY		(1u << 8)
#define HEVC_CTB_NUM			0x53c
#define HEVC_BITS_ADDR			0x540
#define HEVC_BITS_OFFSET		0x544
#define HEVC_BITS_LEN			0x548
#define HEVC_BITS_END			0x54c
#define HEVC_OUTPUT_FRAME_IDX		0x55c
#define HEVC_NEIGHBOR_ADDR		0x560
#define HEVC_ENTRY_POINT_ADDR		0x564
#define HEVC_TILE_START			0x568
#define HEVC_TILE_END			0x56c
#define HEVC_SCALING_DC0		0x578
#define HEVC_SCALING_DC1		0x57c
#define HEVC_OFFSET_FIRST_OUT		0x584
#define HEVC_10BIT_CONFIG		0x58c
#define HEVC_BITS_READ			0x5dc
#define HEVC_SRAM_OFFSET		0x5e0
#define HEVC_SRAM_DATA			0x5e4

#define HEVC_SRAM_WEIGHTS_L0_LUMA	0x000
#define HEVC_SRAM_WEIGHTS_L0_CHROMA	0x020
#define HEVC_SRAM_WEIGHTS_L1_LUMA	0x060
#define HEVC_SRAM_WEIGHTS_L1_CHROMA	0x080
#define HEVC_SRAM_FRAME_INFO		0x400
#define HEVC_SRAM_FRAME_INFO_SIZE	0x20
#define HEVC_SRAM_SCALING_LISTS		0x800
#define HEVC_SRAM_LIST0			0xc00
#define HEVC_SRAM_LIST1			0xc10
#define HEVC_OUTPUT_SLOT		16

#define CEDAR_VCU_DEC_INT_STA		0x628	/* A733 */

#endif	/* CEDAR_REGISTERS_H */
