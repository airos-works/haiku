/* See nvenc_hevc.h, and nvenc_h264.c for where the values come from.
 *
 * On a GTX 1070 (class C2B7) the engine encodes I pictures, IDR or not, and
 * refuses every P picture before writing anything (README.md says what was
 * tried). MediaConverter's test encodes P pictures, so it finds that out and
 * encodes H.265 on the CPU. */

#include "nvenc_hevc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "video/nvenc_drv.h"

_Static_assert(sizeof(nvenc_h265_drv_pic_setup_s) == 1024, "pic setup");
_Static_assert(sizeof(nvenc_h265_slice_control_s) == 64, "slice control");
_Static_assert(sizeof(nvenc_h265_md_control_s) == 128, "md control");
_Static_assert(sizeof(nvenc_h265_quant_control_s) == 16, "quant control");

#define SLICE_CONTROL_OFFSET	0x400
#define ME_CONTROL_OFFSET	0x500
#define MD_CONTROL_OFFSET	0x600
#define QUANT_CONTROL_OFFSET	0x700
#define PIC_SETUP_SIZE		0x1000

#define STATUS_SIZE		0x1000

#define CTB_SIZE		32
#define MIN_CB_SIZE		8
#define LOG2_MAX_POC_LSB	8
#define MIN_QP			10
#define MAX_QP			51

/* what the status buffer's error_status says when a picture is done */
#define PICTURE_DONE		2

enum {
	NAL_TRAIL_R	= 1,
	NAL_IDR_W_RADL	= 19,
	NAL_VPS		= 32,
	NAL_SPS		= 33,
	NAL_PPS		= 34,
};

struct NvencHevc {
	NvencEngine	*engine;
	nvenc_config	config;
	NvencPictureLayout input;
	NvencPictureLayout reference;
	int		bitDepth;
	int		codedWidth, codedHeight;	/* whole coding tree blocks */
	int		widthInCtbs, heightInCtbs;
	int		level;
	int		count;
	int		indexInGop;
	int		poc;
	NvencRateControl rc;

	NvencBuffer	picSetup, inputPicture, bitstream, status, history;
	NvencBuffer	recon[2], coloc[2], mePred[2];

	uint8_t		headers[384];
	size_t		headersSize;
	char		description[160];
};

static bool
set_reason(char *reason, size_t size, const char *text)
{
	if (reason != NULL && size > 0)
		snprintf(reason, size, "%s", text);
	return false;
}

static void
put_profile_tier_level(NvencBits *bits, const NvencHevc *encoder)
{
	int profile = encoder->bitDepth > 8 ? 2 : 1;	/* Main 10 or Main */
	nvencPut(bits, 0, 2);			/* general_profile_space */
	nvencPut(bits, 0, 1);			/* general_tier_flag: Main */
	nvencPut(bits, profile, 5);
	/* compatibility: Main is also Main 10 compatible */
	uint32_t compatibility = (1u << (31 - profile)) | (profile == 1 ? 1u << 29 : 0);
	nvencPut(bits, compatibility, 32);
	nvencPut(bits, 1, 1);			/* progressive_source */
	nvencPut(bits, 0, 1);			/* interlaced_source */
	nvencPut(bits, 0, 1);			/* non_packed_constraint */
	nvencPut(bits, 1, 1);			/* frame_only_constraint */
	nvencPut(bits, 0, 32);			/* 43 reserved bits and inbld */
	nvencPut(bits, 0, 12);
	nvencPut(bits, encoder->level, 8);
}

static void
put_vui(NvencBits *bits, const nvenc_config *config)
{
	bool aspect = config->sar_num > 0 && config->sar_den > 0
		&& config->sar_num != config->sar_den;
	nvencPut(bits, aspect, 1);
	if (aspect) {
		nvencPut(bits, 255, 8);
		nvencPut(bits, config->sar_num, 16);
		nvencPut(bits, config->sar_den, 16);
	}
	nvencPut(bits, 0, 1);			/* overscan_info_present */
	nvencPut(bits, 1, 1);			/* video_signal_type_present */
	nvencPut(bits, 5, 3);
	nvencPut(bits, config->full_range != 0, 1);
	bool colour = config->colour_primaries != 2 || config->transfer != 2
		|| config->matrix != 2;
	nvencPut(bits, colour, 1);
	if (colour) {
		nvencPut(bits, config->colour_primaries, 8);
		nvencPut(bits, config->transfer, 8);
		nvencPut(bits, config->matrix, 8);
	}
	bool chromaLocation = config->chroma_location != 0;
	nvencPut(bits, chromaLocation, 1);
	if (chromaLocation) {
		nvencPutUe(bits, config->chroma_location);
		nvencPutUe(bits, config->chroma_location);
	}
	nvencPut(bits, 0, 1);			/* neutral_chroma_indication */
	nvencPut(bits, 0, 1);			/* field_seq */
	nvencPut(bits, 0, 1);			/* frame_field_info_present */
	nvencPut(bits, 0, 1);			/* default_display_window */
	bool timing = config->frame_rate_num > 0 && config->frame_rate_den > 0;
	nvencPut(bits, timing, 1);
	if (timing) {
		nvencPut(bits, config->frame_rate_den, 32);
		nvencPut(bits, config->frame_rate_num, 32);
		nvencPut(bits, 0, 1);		/* poc_proportional_to_timing */
		nvencPut(bits, 0, 1);		/* hrd_parameters_present */
	}
	nvencPut(bits, 0, 1);			/* bitstream_restriction */
}

static void
write_parameter_sets(NvencHevc *encoder)
{
	const nvenc_config *config = &encoder->config;
	uint8_t rbsp[200];
	NvencBits bits;
	size_t size = 0;

	/* video parameter set */
	nvencBitsInit(&bits, rbsp, sizeof(rbsp));
	nvencPut(&bits, 0, 4);			/* vps_video_parameter_set_id */
	nvencPut(&bits, 1, 1);			/* vps_base_layer_internal */
	nvencPut(&bits, 1, 1);			/* vps_base_layer_available */
	nvencPut(&bits, 0, 6);			/* vps_max_layers_minus1 */
	nvencPut(&bits, 0, 3);			/* vps_max_sub_layers_minus1 */
	nvencPut(&bits, 1, 1);			/* vps_temporal_id_nesting */
	nvencPut(&bits, 0xffff, 16);
	put_profile_tier_level(&bits, encoder);
	nvencPut(&bits, 1, 1);			/* vps_sub_layer_ordering_info_present */
	nvencPutUe(&bits, 1);			/* vps_max_dec_pic_buffering_minus1 */
	nvencPutUe(&bits, 0);			/* vps_max_num_reorder_pics */
	nvencPutUe(&bits, 0);			/* vps_max_latency_increase_plus1 */
	nvencPut(&bits, 0, 6);			/* vps_max_layer_id */
	nvencPutUe(&bits, 0);			/* vps_num_layer_sets_minus1 */
	nvencPut(&bits, 0, 1);			/* vps_timing_info_present */
	nvencPut(&bits, 0, 1);			/* vps_extension */
	nvencTrailingBits(&bits);
	const uint8_t vpsHeader[2] = { NAL_VPS << 1, 1 };
	size = nvencAppendNal(encoder->headers, sizeof(encoder->headers), size, vpsHeader, 2,
		&bits);

	/* sequence parameter set */
	nvencBitsInit(&bits, rbsp, sizeof(rbsp));
	nvencPut(&bits, 0, 4);			/* sps_video_parameter_set_id */
	nvencPut(&bits, 0, 3);			/* sps_max_sub_layers_minus1 */
	nvencPut(&bits, 1, 1);			/* sps_temporal_id_nesting */
	put_profile_tier_level(&bits, encoder);
	nvencPutUe(&bits, 0);			/* sps_seq_parameter_set_id */
	nvencPutUe(&bits, 1);			/* chroma_format_idc */
	nvencPutUe(&bits, encoder->codedWidth);
	nvencPutUe(&bits, encoder->codedHeight);
	int cropRight = (encoder->codedWidth - (int)config->width) / 2;
	int cropBottom = (encoder->codedHeight - (int)config->height) / 2;
	bool crop = cropRight != 0 || cropBottom != 0;
	nvencPut(&bits, crop, 1);
	if (crop) {
		nvencPutUe(&bits, 0);
		nvencPutUe(&bits, cropRight);
		nvencPutUe(&bits, 0);
		nvencPutUe(&bits, cropBottom);
	}
	nvencPutUe(&bits, encoder->bitDepth - 8);
	nvencPutUe(&bits, encoder->bitDepth - 8);
	nvencPutUe(&bits, LOG2_MAX_POC_LSB - 4);
	nvencPut(&bits, 1, 1);			/* sps_sub_layer_ordering_info_present */
	nvencPutUe(&bits, 1);			/* sps_max_dec_pic_buffering_minus1 */
	nvencPutUe(&bits, 0);			/* sps_max_num_reorder_pics */
	nvencPutUe(&bits, 0);			/* sps_max_latency_increase_plus1 */
	nvencPutUe(&bits, 0);			/* log2_min_luma_coding_block_size_minus3 */
	nvencPutUe(&bits, 2);			/* to 32x32 */
	nvencPutUe(&bits, 0);			/* log2_min_luma_transform_block_size_minus2 */
	nvencPutUe(&bits, 3);			/* to 32x32 */
	nvencPutUe(&bits, 1);			/* max_transform_hierarchy_depth_inter */
	nvencPutUe(&bits, 1);			/* max_transform_hierarchy_depth_intra */
	nvencPut(&bits, 0, 1);			/* scaling_list_enabled */
	nvencPut(&bits, 0, 1);			/* amp_enabled */
	nvencPut(&bits, 0, 1);			/* sample_adaptive_offset_enabled */
	nvencPut(&bits, 0, 1);			/* pcm_enabled */
	nvencPutUe(&bits, 1);			/* num_short_term_ref_pic_sets */
	/* st_ref_pic_set(0): the previous picture */
	nvencPutUe(&bits, 1);			/* num_negative_pics */
	nvencPutUe(&bits, 0);			/* num_positive_pics */
	nvencPutUe(&bits, 0);			/* delta_poc_s0_minus1 */
	nvencPut(&bits, 1, 1);			/* used_by_curr_pic_s0 */
	nvencPut(&bits, 0, 1);			/* long_term_ref_pics_present */
	nvencPut(&bits, 0, 1);			/* sps_temporal_mvp_enabled */
	nvencPut(&bits, 0, 1);			/* strong_intra_smoothing_enabled */
	nvencPut(&bits, 1, 1);			/* vui_parameters_present */
	put_vui(&bits, config);
	nvencPut(&bits, 0, 1);			/* sps_extension_present */
	nvencTrailingBits(&bits);
	const uint8_t spsHeader[2] = { NAL_SPS << 1, 1 };
	size = nvencAppendNal(encoder->headers, sizeof(encoder->headers), size, spsHeader, 2,
		&bits);

	/* picture parameter set */
	nvencBitsInit(&bits, rbsp, sizeof(rbsp));
	nvencPutUe(&bits, 0);			/* pps_pic_parameter_set_id */
	nvencPutUe(&bits, 0);			/* pps_seq_parameter_set_id */
	nvencPut(&bits, 0, 1);			/* dependent_slice_segments_enabled */
	nvencPut(&bits, 0, 1);			/* output_flag_present */
	nvencPut(&bits, 0, 3);			/* num_extra_slice_header_bits */
	nvencPut(&bits, 0, 1);			/* sign_data_hiding_enabled */
	nvencPut(&bits, 0, 1);			/* cabac_init_present */
	nvencPutUe(&bits, 0);			/* num_ref_idx_l0_default_active_minus1 */
	nvencPutUe(&bits, 0);			/* num_ref_idx_l1_default_active_minus1 */
	nvencPutSe(&bits, 0);			/* init_qp_minus26 */
	nvencPut(&bits, 0, 1);			/* constrained_intra_pred */
	nvencPut(&bits, 0, 1);			/* transform_skip_enabled */
	nvencPut(&bits, 0, 1);			/* cu_qp_delta_enabled */
	nvencPutSe(&bits, 0);			/* pps_cb_qp_offset */
	nvencPutSe(&bits, 0);			/* pps_cr_qp_offset */
	nvencPut(&bits, 0, 1);			/* pps_slice_chroma_qp_offsets_present */
	nvencPut(&bits, 0, 1);			/* weighted_pred */
	nvencPut(&bits, 0, 1);			/* weighted_bipred */
	nvencPut(&bits, 0, 1);			/* transquant_bypass_enabled */
	nvencPut(&bits, 0, 1);			/* tiles_enabled */
	nvencPut(&bits, 0, 1);			/* entropy_coding_sync_enabled */
	nvencPut(&bits, 0, 1);			/* pps_loop_filter_across_slices_enabled */
	nvencPut(&bits, 0, 1);			/* deblocking_filter_control_present */
	nvencPut(&bits, 0, 1);			/* pps_scaling_list_data_present */
	nvencPut(&bits, 0, 1);			/* lists_modification_present */
	nvencPutUe(&bits, 0);			/* log2_parallel_merge_level_minus2 */
	nvencPut(&bits, 0, 1);			/* slice_segment_header_extension_present */
	nvencPut(&bits, 0, 1);			/* pps_extension_present */
	nvencTrailingBits(&bits);
	const uint8_t ppsHeader[2] = { NAL_PPS << 1, 1 };
	encoder->headersSize = nvencAppendNal(encoder->headers, sizeof(encoder->headers), size,
		ppsHeader, 2, &bits);
}

/* As for H.264: the video block linear arrangement throughout, chroma at
 * its own address, the coded size. The pitch counts samples, not bytes: with
 * 16-bit samples a pitch in bytes makes the engine run off the end of the
 * picture. */
static void
fill_surface(nvenc_h265_surface_cfg_s *cfg, const NvencHevc *encoder,
	const NvencPictureLayout *picture)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->frame_width_minus1 = encoder->codedWidth - 1;
	cfg->frame_height_minus1 = encoder->codedHeight - 1;
	cfg->sfc_pitch = picture->pitch / picture->bytesPerSample;
	cfg->sfc_pitch_chroma = picture->pitch / picture->bytesPerSample;
	cfg->block_height = 2;
}

static void
fill_md_control(nvenc_h265_md_control_s *md)
{
	memset(md, 0, sizeof(*md));
	md->h265_intra_luma4x4_mode_enable = 0xffffffff;
	md->h265_intra_luma8x8_mode_enable = 0xffffffff;
	md->h265_intra_luma16x16_mode_enable = 0xffffffff;
	md->h265_intra_luma32x32_mode_enable = 0xffffffff;
	md->h265_intra_luma_mode_enable_leftbits = 0x3fffffff;
	md->cu16x16_part_2Nx2N_enable = 1;
	md->cu16x16_part_2NxN_enable = 1;
	md->cu16x16_part_Nx2N_enable = 1;
	md->cu32x32_part_2Nx2N_enable = 1;
	md->cu32x32_part_2NxN_enable = 1;
	md->cu32x32_part_Nx2N_enable = 1;
	md->intra_mode_4x4_enable = 1;
	md->intra_mode_8x8_enable = 1;
	md->intra_mode_16x16_enable = 1;
	md->intra_mode_32x32_enable = 1;
	md->h265_intra_chroma_mode_enable = 0x1f;
	md->tusize_4x4_enable = 1;
	md->tusize_8x8_enable = 1;
	md->tusize_16x16_enable = 1;
	md->tusize_32x32_enable = 1;
	md->tu_search_num_cu16 = 3;
	md->tu_search_num_cu32 = 3;
	md->pskip_enable = 1;
	md->mv_cost_enable = 1;
	md->mv_cost_predictor_control = 1;
	md->ip_search_mode = 0xf;
}

NvencHevc *
nvencHevcOpen(NvencEngine *engine, const nvenc_config *config, char *reason,
	size_t reasonSize)
{
	if (config->width < 64 || config->height < 64 || config->width > 8192
		|| config->height > 8192) {
		set_reason(reason, reasonSize, "NVENC encodes H.265 from 64x64 to 8192x8192.");
		return NULL;
	}
	if (config->variant >= kNvencLayoutCount) {
		set_reason(reason, reasonSize, "no such picture arrangement");
		return NULL;
	}

	NvencHevc *encoder = calloc(1, sizeof(NvencHevc));
	if (encoder == NULL)
		return NULL;
	encoder->engine = engine;
	encoder->config = *config;
	if (encoder->config.key_interval == 0)
		encoder->config.key_interval = 250;
	encoder->bitDepth = config->codec == NVENC_CODEC_HEVC_10 ? 10 : 8;
	/* Whole coding tree blocks, and the SPS crops: the engine refuses a
	 * picture whose last row or column of blocks is half or more empty
	 * (640x360, 1280x720). */
	encoder->codedWidth = (config->width + CTB_SIZE - 1) / CTB_SIZE * CTB_SIZE;
	encoder->codedHeight = (config->height + CTB_SIZE - 1) / CTB_SIZE * CTB_SIZE;
	encoder->widthInCtbs = (config->width + CTB_SIZE - 1) / CTB_SIZE;
	encoder->heightInCtbs = (config->height + CTB_SIZE - 1) / CTB_SIZE;
	double fps = config->frame_rate_den != 0
		? (double)config->frame_rate_num / config->frame_rate_den : 30;
	encoder->level = nvencHevcLevel(config->width, config->height, fps);
	nvencRateInit(&encoder->rc, config, MIN_QP, MAX_QP, 28);

	int bytesPerSample = encoder->bitDepth > 8 ? 2 : 1;
	nvencPictureLayout(&encoder->input, config->width, config->height, bytesPerSample,
		CTB_SIZE, CTB_SIZE);
	nvencPictureLayout(&encoder->reference, config->width, config->height, bytesPerSample,
		CTB_SIZE, CTB_SIZE);
	size_t blocks = (size_t)(encoder->input.alignedWidth / 16)
		* (encoder->input.alignedHeight / 16);
	size_t historySize = ((size_t)encoder->input.alignedWidth / 16 + 16) * 384;
	size_t bitstreamSize = (size_t)encoder->input.pitch * encoder->input.alignedHeight * 2
		+ 0x10000;

	bool ok = nvencAlloc(engine, &encoder->picSetup, PIC_SETUP_SIZE, true)
		&& nvencAlloc(engine, &encoder->inputPicture, encoder->input.size, true)
		&& nvencAlloc(engine, &encoder->bitstream, bitstreamSize, true)
		&& nvencAlloc(engine, &encoder->status, STATUS_SIZE, true)
		&& nvencAlloc(engine, &encoder->history, historySize, false);
	for (int i = 0; ok && i < 2; i++) {
		ok = nvencAlloc(engine, &encoder->recon[i], encoder->reference.size, false)
			&& nvencAlloc(engine, &encoder->coloc[i], blocks * 128 + 0x1000, false)
			&& nvencAlloc(engine, &encoder->mePred[i], blocks * 128 + 0x1000, false);
	}
	if (!ok) {
		set_reason(reason, reasonSize, "Not enough memory on the card for NVENC.");
		nvencHevcClose(encoder);
		return NULL;
	}

	write_parameter_sets(encoder);
	const char *profile = encoder->bitDepth > 8 ? "Main 10" : "Main";
	if (config->rate_control == NVENC_RC_CONSTQP) {
		snprintf(encoder->description, sizeof(encoder->description),
			"H.265 %s, constant QP %u, I and P pictures", profile, config->qp);
	} else {
		snprintf(encoder->description, sizeof(encoder->description),
			"H.265 %s, %s %u kbit/s, I and P pictures", profile,
			config->rate_control == NVENC_RC_CBR ? "constant" : "average",
			config->bitrate / 1000);
	}
	return encoder;
}

void
nvencHevcClose(NvencHevc *encoder)
{
	if (encoder == NULL)
		return;
	NvencEngine *engine = encoder->engine;
	for (int i = 0; i < 2; i++) {
		nvencFree(engine, &encoder->recon[i]);
		nvencFree(engine, &encoder->coloc[i]);
		nvencFree(engine, &encoder->mePred[i]);
	}
	nvencFree(engine, &encoder->history);
	nvencFree(engine, &encoder->status);
	nvencFree(engine, &encoder->bitstream);
	nvencFree(engine, &encoder->inputPicture);
	nvencFree(engine, &encoder->picSetup);
	free(encoder);
}

size_t
nvencHevcHeaders(NvencHevc *encoder, uint8_t *buffer, size_t size)
{
	if (size < encoder->headersSize)
		return 0;
	memcpy(buffer, encoder->headers, encoder->headersSize);
	return encoder->headersSize;
}

const char *
nvencHevcDescribe(NvencHevc *encoder)
{
	return encoder->description;
}

static int
ceil_log2(int value)
{
	int log = 0;
	while ((1 << log) < value)
		log++;
	return log;
}

static void
fill_pic_setup(NvencHevc *encoder, bool idr, int qp)
{
	const nvenc_config *config = &encoder->config;
	uint8_t *base = encoder->picSetup.data;
	memset(base, 0, PIC_SETUP_SIZE);
	nvenc_h265_drv_pic_setup_s *setup = (nvenc_h265_drv_pic_setup_s *)base;

	setup->magic = (nvencCard(encoder->engine)->encoderClass << 16) | 0x0006;
	fill_surface(&setup->refpic_cfg, encoder, &encoder->reference);
	fill_surface(&setup->input_cfg, encoder, &encoder->input);
	fill_surface(&setup->outputpic_cfg, encoder, &encoder->reference);

	nvenc_h265_sps_data_s *sps = &setup->sps_data;
	sps->chroma_format_idc = 1;
	sps->log2_max_pic_order_cnt_lsb_minus4 = LOG2_MAX_POC_LSB - 4;
	sps->min_cu_size = 0;			/* 8x8 */
	sps->max_cu_size = 2;			/* 32x32 */
	sps->min_tu_size = 0;			/* 4x4 */
	sps->max_tu_size = 3;			/* 32x32 */
	sps->max_transform_depth_inter = 1;
	sps->max_transform_depth_intra = 1;
	sps->num_short_term_ref_pic_sets = 1;
	sps->bit_depth_minus_8 = encoder->bitDepth - 8;

	nvenc_h265_rc_s *rc = &setup->rate_control;
	for (int i = 0; i < 3; i++) {
		rc->QP[i] = rc->minQP[i] = rc->maxQP[i] = qp;
		rc->rhopbi[i] = 1 << 8;
	}
	rc->framerate = config->frame_rate_den != 0
		? (int)(config->frame_rate_num / config->frame_rate_den) : 30;
	rc->buffersize = (uint32_t)encoder->bitstream.size;
	rc->gop_length = config->key_interval;

	/* Unused list entries stay 0: the engine reads l1[0] even for a P
	 * picture, and -1 sends it to SET_IN_REF_PIC15, which faults. */
	nvenc_h265_pic_control_s *pic = &setup->pic_control;
	if (!idr) {
		pic->temp_dist_l0[0] = 1;
		pic->short_term_rps.num_negative_pics = 1;
		pic->short_term_rps.delta_poc_s0_minus1[0] = 0;
		pic->short_term_rps.used_by_curr_pic_s0_flag = 1;
	}
	pic->pic_output_flag = 1;
	pic->pic_order_cnt_lsb = encoder->poc % (1 << LOG2_MAX_POC_LSB);
	pic->slice_control_offset = SLICE_CONTROL_OFFSET;
	pic->me_control_offset = ME_CONTROL_OFFSET;
	pic->md_control_offset = MD_CONTROL_OFFSET;
	pic->q_control_offset = QUANT_CONTROL_OFFSET;
	pic->hist_buf_size = (uint32_t)encoder->history.size;
	pic->bitstream_buf_size = (uint32_t)encoder->bitstream.size;
	pic->pic_type = idr ? 3 : 0;
	pic->ref_pic_flag = 1;
	/* as for H.264: one dynamic slice, and not the decoder's codec number */
	pic->slice_mode = 0;
	pic->codec = 0;
	pic->ceil_log2_pic_size_in_ctbs = ceil_log2(encoder->widthInCtbs * encoder->heightInCtbs);
	pic->short_term_ref_pic_set_sps_flag = 1;
	pic->short_term_ref_pic_set_idx = 0;
	pic->nal_unit_type = idr ? NAL_IDR_W_RADL : NAL_TRAIL_R;
	pic->nuh_temporal_id_plus1 = 1;
	/* Nothing after the pic control is set: from Turing on it ends with
	 * stats_fifo_offset, and on Pascal those four bytes are the start of
	 * half_scaled_outputpic_cfg (and gpTimer_timeout_val moves with it). */

	nvenc_h265_slice_control_s *slice
		= (nvenc_h265_slice_control_s *)(base + SLICE_CONTROL_OFFSET);
	slice->num_ctus = encoder->widthInCtbs * encoder->heightInCtbs;
	slice->qp_avr = qp;
	slice->slice_type = idr ? 2 : 1;
	slice->collocated_from_l0_flag = 1;
	slice->qp_slice_min = qp;
	slice->qp_slice_max = qp;

	nvencFillMeControl(base + ME_CONTROL_OFFSET);
	fill_md_control((nvenc_h265_md_control_s *)(base + MD_CONTROL_OFFSET));
}

static void
dump_status(const char *what, const NvencBuffer *status)
{
	const uint32_t *words = status->data;
	fprintf(stderr, "nvenc: %s, status", what);
	for (int i = 0; i < 16; i++)
		fprintf(stderr, " %08x", words[i]);
	fprintf(stderr, "\n");
}

int
nvencHevcEncode(NvencHevc *encoder, const nvenc_picture *picture, NvencQueue *queue,
	char *reason, size_t reasonSize)
{
	bool idr = encoder->count == 0 || picture->force_key
		|| encoder->indexInGop >= (int)encoder->config.key_interval;
	if (idr) {
		encoder->indexInGop = 0;
		encoder->poc = 0;
	}
	int qp = nvencRateQp(&encoder->rc, idr);

	nvencStorePicture(encoder->inputPicture.data, &encoder->input,
		&kNvencLayouts[encoder->config.variant], picture);
	fill_pic_setup(encoder, idr, qp);
	memset(encoder->status.data, 0, sizeof(nvenc_h265_pic_stat_s));

	int current = encoder->count % 2;
	int previous = 1 - current;
	size_t inputChroma = encoder->input.chromaOffset;
	size_t referenceChroma = encoder->reference.chromaOffset;
	NvencEngine *engine = encoder->engine;
	nvencBegin(engine);
	nvencMethod(engine, NVENC_SET_APPLICATION_ID, NVENC_APPLICATION_H265);
	nvencMethod(engine, NVENC_SET_CONTROL_PARAMS,
		NVENC_CONTROL_CODEC_H265 | NVENC_CONTROL_FORCE_OUT_PIC | NVENC_CONTROL_RCMODE(0));
	nvencMethod(engine, NVENC_SET_PICTURE_INDEX, encoder->count + 1);
	nvencAddress(engine, NVENC_SET_IN_DRV_PIC_SETUP, encoder->picSetup.gpuAddress);
	nvencAddress(engine, NVENC_SET_IN_CUR_PIC, encoder->inputPicture.gpuAddress);
	nvencAddress(engine, NVENC_SET_IN_CUR_PIC_CHROMA_U,
		encoder->inputPicture.gpuAddress + inputChroma);
	nvencAddress(engine, NVENC_SET_IN_CUR_PIC_CHROMA_V,
		encoder->inputPicture.gpuAddress + inputChroma);
	nvencAddress(engine, NVENC_SET_OUT_REF_PIC_LUMA, encoder->recon[current].gpuAddress);
	nvencAddress(engine, NVENC_SET_OUT_REF_PIC_CHROMA,
		encoder->recon[current].gpuAddress + referenceChroma);
	nvencAddress(engine, NVENC_SET_IOHISTORY, encoder->history.gpuAddress);
	nvencAddress(engine, NVENC_SET_OUT_ENC_STATUS, encoder->status.gpuAddress);
	nvencAddress(engine, NVENC_SET_OUT_BITSTREAM, encoder->bitstream.gpuAddress);
	nvencAddress(engine, NVENC_SET_OUT_COLOC_DATA, encoder->coloc[current].gpuAddress);
	nvencAddress(engine, NVENC_SET_OUT_MEPRED_DATA, encoder->mePred[current].gpuAddress);
	nvencAddress(engine, NVENC_SET_IN_MEPRED_DATA, encoder->mePred[previous].gpuAddress);
	if (!idr) {
		nvencAddress(engine, NVENC_SET_IN_REF_PIC_LUMA(0),
			encoder->recon[previous].gpuAddress);
		nvencAddress(engine, NVENC_SET_IN_REF_PIC_CHROMA(0),
			encoder->recon[previous].gpuAddress + referenceChroma);
		nvencAddress(engine, NVENC_SET_IN_COLOC_DATA, encoder->coloc[previous].gpuAddress);
	}
	nvencMethod(engine, NVENC_EXECUTE, 0);
	char why[160];
	int executed = nvencExecute(engine,
		encoder->config.timeout_ms != 0 ? (int)encoder->config.timeout_ms : 2000, why,
		sizeof(why));
	if (executed != 0) {
		if (getenv("NVENC_DEBUG") != NULL)
			dump_status("failed", &encoder->status);
		if (reason != NULL) {
			snprintf(reason, reasonSize, "NVENC %s: %s",
				idr ? "refused an H.265 I picture" : "refused an H.265 P picture", why);
		}
		return executed;
	}

	const nvenc_h265_pic_stat_s *stat = encoder->status.data;
	if (getenv("NVENC_DEBUG") != NULL)
		dump_status("done", &encoder->status);
	if (stat->error_status != PICTURE_DONE || stat->ucode_error_status != 0) {
		if (reason != NULL) {
			snprintf(reason, reasonSize, "NVENC reported error %u (microcode %#x)",
				stat->error_status, stat->ucode_error_status);
		}
		return NVENC_FAILED;
	}
	size_t bytes = (stat->total_bit_count + 7) / 8;
	const uint8_t *segment = encoder->bitstream.data;
	if (bytes < 6 || bytes > encoder->bitstream.size) {
		if (reason != NULL)
			snprintf(reason, reasonSize, "NVENC wrote %zu bytes for a picture", bytes);
		return NVENC_FAILED;
	}
	size_t skip = 0;
	while (skip + 3 < bytes && !(segment[skip] == 0 && segment[skip + 1] == 0
			&& segment[skip + 2] == 1))
		skip++;
	int type = skip + 3 < bytes ? (segment[skip + 3] >> 1) & 0x3f : -1;
	if (skip > 1 || type < 0 || type > 21) {
		if (reason != NULL) {
			snprintf(reason, reasonSize,
				"NVENC's output is no H.265 slice (%02x %02x %02x %02x %02x)",
				segment[0], segment[1], segment[2], segment[3], segment[4]);
		}
		return NVENC_FAILED;
	}

	size_t total = (idr ? encoder->headersSize : 0) + bytes;
	uint8_t *packet = malloc(total);
	if (packet == NULL) {
		set_reason(reason, reasonSize, "out of memory");
		return NVENC_FAILED;
	}
	size_t at = 0;
	if (idr) {
		memcpy(packet, encoder->headers, encoder->headersSize);
		at = encoder->headersSize;
	}
	memcpy(packet + at, segment, bytes);
	bool queued = nvencQueuePush(queue, packet, total, picture->pts, picture->pts, idr);
	free(packet);
	if (!queued) {
		set_reason(reason, reasonSize, "out of memory");
		return NVENC_FAILED;
	}

	nvencRateUpdate(&encoder->rc, bytes, idr);
	encoder->count++;
	encoder->indexInGop++;
	encoder->poc++;
	return 0;
}
