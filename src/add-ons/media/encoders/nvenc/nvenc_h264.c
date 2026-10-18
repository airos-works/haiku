/* See nvenc_h264.h.
 *
 * Everything the engine is told comes from NVIDIA's nvenc_drv.h (in Mesa's
 * nouveau headers); no open driver drives NVENC, so what the header does not
 * say - the arrangement of the input picture, which of the many tuning
 * fields must not stay zero - is chosen as the header suggests and checked
 * by MediaConverter's self-test, which decodes what comes out. */

#include "nvenc_h264.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "video/nvenc_drv.h"

_Static_assert(sizeof(nvenc_h264_drv_pic_setup_s) == 512, "pic setup");
_Static_assert(sizeof(nvenc_h264_slice_control_s) == 128, "slice control");
_Static_assert(sizeof(nvenc_h264_me_control_s) == 192, "me control");
_Static_assert(sizeof(nvenc_h264_md_control_s) == 128, "md control");
_Static_assert(sizeof(nvenc_h264_quant_control_s) == 192, "quant control");

/* Where the parts of the picture setup buffer go; the engine is given the
 * offsets. Each part starts on 256 bytes. */
#define SLICE_CONTROL_OFFSET	0x200
#define ME_CONTROL_OFFSET	0x300
#define MD_CONTROL_OFFSET	0x400
#define QUANT_CONTROL_OFFSET	0x500
#define PIC_SETUP_SIZE		0x1000

/* The status buffer: the picture's statistics first, then room for what
 * the engine may write about slices and macroblocks. */
#define STATUS_SLICE_OFFSET	0x100
#define STATUS_MPEC_OFFSET	0x1000
#define STATUS_AQ_OFFSET	0x4000
#define STATUS_FIFO_OFFSET	0x8000
#define STATUS_SIZE		0x10000

#define LOG2_MAX_FRAME_NUM	8
#define MIN_QP			10
#define MAX_QP			51

struct NvencH264 {
	NvencEngine	*engine;
	nvenc_config	config;
	const NvencLayout *layout;
	NvencPictureLayout input;
	NvencPictureLayout reference;
	int		widthInMbs, heightInMbs;
	int		level;
	int		count;			/* pictures so far */
	int		indexInGop;
	int		frameNum;
	int		idrPicId;
	NvencRateControl rc;

	NvencBuffer	picSetup, inputPicture, bitstream, status, history, rcProcess;
	NvencBuffer	recon[2], coloc[2], mePred[2];

	uint8_t		headers[256];
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
write_parameter_sets(NvencH264 *encoder)
{
	const nvenc_config *config = &encoder->config;
	uint8_t rbsp[160];
	NvencBits bits;

	/* sequence parameter set */
	nvencBitsInit(&bits, rbsp, sizeof(rbsp));
	nvencPut(&bits, 77, 8);			/* profile_idc: Main */
	nvencPut(&bits, 0x40, 8);		/* constraint_set1: Main */
	nvencPut(&bits, encoder->level, 8);
	nvencPutUe(&bits, 0);			/* seq_parameter_set_id */
	nvencPutUe(&bits, LOG2_MAX_FRAME_NUM - 4);
	nvencPutUe(&bits, 2);			/* pic_order_cnt_type */
	nvencPutUe(&bits, 1);			/* max_num_ref_frames */
	nvencPut(&bits, 0, 1);			/* gaps_in_frame_num_value_allowed */
	nvencPutUe(&bits, encoder->widthInMbs - 1);
	nvencPutUe(&bits, encoder->heightInMbs - 1);
	nvencPut(&bits, 1, 1);			/* frame_mbs_only */
	nvencPut(&bits, 1, 1);			/* direct_8x8_inference */
	int cropRight = (encoder->widthInMbs * 16 - (int)config->width) / 2;
	int cropBottom = (encoder->heightInMbs * 16 - (int)config->height) / 2;
	bool crop = cropRight != 0 || cropBottom != 0;
	nvencPut(&bits, crop, 1);
	if (crop) {
		nvencPutUe(&bits, 0);
		nvencPutUe(&bits, cropRight);
		nvencPutUe(&bits, 0);
		nvencPutUe(&bits, cropBottom);
	}
	nvencPut(&bits, 1, 1);			/* vui_parameters_present */
	bool aspect = config->sar_num > 0 && config->sar_den > 0
		&& config->sar_num != config->sar_den;
	nvencPut(&bits, aspect, 1);
	if (aspect) {
		nvencPut(&bits, 255, 8);	/* Extended_SAR */
		nvencPut(&bits, config->sar_num, 16);
		nvencPut(&bits, config->sar_den, 16);
	}
	nvencPut(&bits, 0, 1);			/* overscan_info_present */
	nvencPut(&bits, 1, 1);			/* video_signal_type_present */
	nvencPut(&bits, 5, 3);			/* video_format: unspecified */
	nvencPut(&bits, config->full_range != 0, 1);
	bool colour = config->colour_primaries != 2 || config->transfer != 2
		|| config->matrix != 2;
	nvencPut(&bits, colour, 1);
	if (colour) {
		nvencPut(&bits, config->colour_primaries, 8);
		nvencPut(&bits, config->transfer, 8);
		nvencPut(&bits, config->matrix, 8);
	}
	bool chromaLocation = config->chroma_location != 0;
	nvencPut(&bits, chromaLocation, 1);
	if (chromaLocation) {
		nvencPutUe(&bits, config->chroma_location);
		nvencPutUe(&bits, config->chroma_location);
	}
	bool timing = config->frame_rate_num > 0 && config->frame_rate_den > 0;
	nvencPut(&bits, timing, 1);
	if (timing) {
		nvencPut(&bits, config->frame_rate_den, 32);	/* num_units_in_tick */
		nvencPut(&bits, 2 * config->frame_rate_num, 32);	/* time_scale */
		nvencPut(&bits, 0, 1);		/* fixed_frame_rate */
	}
	nvencPut(&bits, 0, 1);			/* nal_hrd_parameters_present */
	nvencPut(&bits, 0, 1);			/* vcl_hrd_parameters_present */
	nvencPut(&bits, 0, 1);			/* pic_struct_present */
	nvencPut(&bits, 1, 1);			/* bitstream_restriction */
	nvencPut(&bits, 1, 1);			/* motion_vectors_over_pic_boundaries */
	nvencPutUe(&bits, 2);			/* max_bytes_per_pic_denom */
	nvencPutUe(&bits, 1);			/* max_bits_per_mb_denom */
	nvencPutUe(&bits, 15);			/* log2_max_mv_length_horizontal */
	nvencPutUe(&bits, 15);			/* log2_max_mv_length_vertical */
	nvencPutUe(&bits, 0);			/* max_num_reorder_frames */
	nvencPutUe(&bits, 1);			/* max_dec_frame_buffering */
	nvencTrailingBits(&bits);
	const uint8_t spsHeader = 0x67;
	size_t size = nvencAppendNal(encoder->headers, sizeof(encoder->headers), 0,
		&spsHeader, 1, &bits);

	/* picture parameter set */
	nvencBitsInit(&bits, rbsp, sizeof(rbsp));
	nvencPutUe(&bits, 0);			/* pic_parameter_set_id */
	nvencPutUe(&bits, 0);			/* seq_parameter_set_id */
	nvencPut(&bits, 1, 1);			/* entropy_coding_mode: CABAC */
	nvencPut(&bits, 0, 1);			/* bottom_field_pic_order_in_frame_present */
	nvencPutUe(&bits, 0);			/* num_slice_groups_minus1 */
	nvencPutUe(&bits, 0);			/* num_ref_idx_l0_default_active_minus1 */
	nvencPutUe(&bits, 0);			/* num_ref_idx_l1_default_active_minus1 */
	nvencPut(&bits, 0, 1);			/* weighted_pred */
	nvencPut(&bits, 0, 2);			/* weighted_bipred_idc */
	nvencPutSe(&bits, 0);			/* pic_init_qp_minus26 */
	nvencPutSe(&bits, 0);			/* pic_init_qs_minus26 */
	nvencPutSe(&bits, 0);			/* chroma_qp_index_offset */
	nvencPut(&bits, 1, 1);			/* deblocking_filter_control_present */
	nvencPut(&bits, 0, 1);			/* constrained_intra_pred */
	nvencPut(&bits, 0, 1);			/* redundant_pic_cnt_present */
	nvencTrailingBits(&bits);
	const uint8_t ppsHeader = 0x68;
	encoder->headersSize = nvencAppendNal(encoder->headers, sizeof(encoder->headers), size,
		&ppsHeader, 1, &bits);
}

static void
fill_surface(nvenc_h264_surface_cfg_s *cfg, const NvencPictureLayout *picture,
	const NvencLayout *layout, bool reference)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->frame_width_minus1 = picture->width - 1;
	cfg->frame_height_minus1 = picture->height - 1;
	cfg->sfc_pitch = picture->pitch;
	cfg->sfc_pitch_chroma = picture->pitch;
	cfg->luma_top_frm_offset = 0;
	cfg->chroma_top_frm_offset = layout->chromaFromBase ? picture->chromaOffset >> 8 : 0;
	cfg->memory_mode = 0;			/* semi-planar */
	if (reference) {
		cfg->tiled_16x16 = 1;		/* the header: must be 1 for refpics */
		return;
	}
	cfg->tiled_16x16 = layout->tiled;
	cfg->block_height = layout->blockHeight;
	cfg->input_bl_mode = 0;			/* GPU block linear */
}

static void
fill_me_control(nvenc_h264_me_control_s *me)
{
	memset(me, 0, sizeof(*me));
	me->me_predictor_mode = 0;
	me->refinement_mode = 1;		/* quarter pel */
	me->lambda_mode = 0;
	me->refine_on_search_enable = 1;
	me->mv_only_enable = 0;

	/* Search around the neighbours' vectors and around zero. Temporal
	 * hints would need the predictor buffers to be written as the engine
	 * expects, which nothing documents. */
	me->predsrc.self_spatial_enable = 1;
	me->predsrc.self_spatial_search = 1;
	me->predsrc.self_spatial_refine = 1;
	me->predsrc.self_spatial_explicit = 1;
	me->predsrc.const_mv_enable = 1;
	me->predsrc.const_mv_search = 1;
	me->predsrc.const_mv_refine = 1;
	me->predsrc.const_mv_explicit = 1;

	/* Every point of an 8 by 8 pattern for every priority */
	nvenc_h264_me_stamp_s *shapes[] = { &me->shape0, &me->shape1, &me->shape2,
		&me->shape3, &me->shape4, &me->shape5, &me->shape6, &me->shape7 };
	for (int i = 0; i < 8; i++) {
		shapes[i]->bitmask[0] = 0xffffffff;
		shapes[i]->bitmask[1] = 0xffffffff;
	}

	/* the five hint kinds in order; the header says they must all appear */
	me->hint_type0 = 0;
	me->hint_type1 = 1;
	me->hint_type2 = 2;
	me->hint_type3 = 3;
	me->hint_type4 = 4;
	me->fbm_select_best_cu16_parttype_num = 1;
	me->fbm_op_winner_num_p_frame = 1;
	me->fbm_op_winner_num_b_frame_l0 = 1;
	me->fbm_op_winner_num_b_frame_l1 = 1;
	me->fbm_select_best_cu32_parttype_num = 1;
}

static void
fill_md_control(nvenc_h264_md_control_s *md)
{
	memset(md, 0, sizeof(*md));
	md->intra_luma4x4_mode_enable = 0x1ff;
	md->intra_luma8x8_mode_enable = 0;	/* Main profile: no 8x8 transform */
	md->intra_luma16x16_mode_enable = 0xf;
	md->intra_chroma_mode_enable = 0xf;
	md->l0_part_16x16_enable = 1;
	md->l0_part_16x8_enable = 1;
	md->l0_part_8x16_enable = 1;
	md->l0_part_8x8_enable = 1;
	md->pskip_enable = 1;
	md->mv_cost_enable = 1;
	md->mv_cost_predictor_control = 1;
	md->intra_most_prob_force_on = 1;
	md->early_intra_mode_control = 3;	/* none: full intra search */
	md->ip_search_mode = 1 | 4;		/* 4x4 and 16x16 */
}

static void
fill_quant_control(nvenc_h264_quant_control_s *quant)
{
	/* No QPP, no saturation; zero dead zones round towards zero, which costs
	 * a little quality but is valid whatever their unit is. */
	memset(quant, 0, sizeof(*quant));
}

static void
fill_slice_control(NvencH264 *encoder, nvenc_h264_slice_control_s *slice, int qp,
	bool intra)
{
	memset(slice, 0, sizeof(*slice));
	slice->num_mb = encoder->widthInMbs * encoder->heightInMbs;
	slice->qp_avr = qp;
	slice->qp_slice_min = MIN_QP;
	slice->qp_slice_max = MAX_QP;
	slice->force_intra = intra;
	slice->disable_deblocking_filter_idc = 0;
	slice->cabac_init_idc = 0;
	slice->disable_slice_header = 0;	/* the engine writes it */
	slice->num_ref_idx_active_override_flag = 0;
}

NvencH264 *
nvencH264Open(NvencEngine *engine, const nvenc_config *config, char *reason,
	size_t reasonSize)
{
	if (config->width < 32 || config->height < 32 || config->width > 4096
		|| config->height > 4096) {
		set_reason(reason, reasonSize, "NVENC encodes H.264 from 32x32 to 4096x4096.");
		return NULL;
	}
	if (config->variant >= kNvencLayoutCount) {
		set_reason(reason, reasonSize, "no such picture arrangement");
		return NULL;
	}

	NvencH264 *encoder = calloc(1, sizeof(NvencH264));
	if (encoder == NULL)
		return NULL;
	encoder->engine = engine;
	encoder->config = *config;
	if (encoder->config.key_interval == 0)
		encoder->config.key_interval = 250;
	encoder->layout = &kNvencLayouts[config->variant];
	encoder->widthInMbs = (config->width + 15) / 16;
	encoder->heightInMbs = (config->height + 15) / 16;
	double fps = config->frame_rate_den != 0
		? (double)config->frame_rate_num / config->frame_rate_den : 30;
	encoder->level = nvencH264Level(config->width, config->height, fps,
		config->rate_control == NVENC_RC_CONSTQP ? 0 : config->max_bitrate);
	nvencRateInit(&encoder->rc, config, MIN_QP, MAX_QP, 26);

	nvencPictureLayout(&encoder->input, config->width, config->height, 1, 16, 32);
	nvencPictureLayout(&encoder->reference, config->width, config->height, 1, 16, 32);
	int macroblocks = encoder->widthInMbs * encoder->heightInMbs;
	size_t historySize = ((size_t)encoder->widthInMbs + 16) * 128 * 2;
	size_t bitstreamSize = (size_t)encoder->input.pitch * encoder->input.alignedHeight * 2
		+ 0x10000;

	bool ok = nvencAlloc(engine, &encoder->picSetup, PIC_SETUP_SIZE, true)
		&& nvencAlloc(engine, &encoder->inputPicture, encoder->input.size, true)
		&& nvencAlloc(engine, &encoder->bitstream, bitstreamSize, true)
		&& nvencAlloc(engine, &encoder->status, STATUS_SIZE, true)
		&& nvencAlloc(engine, &encoder->history, historySize, false)
		&& nvencAlloc(engine, &encoder->rcProcess, 0x1000, false);
	for (int i = 0; ok && i < 2; i++) {
		ok = nvencAlloc(engine, &encoder->recon[i], encoder->reference.size, false)
			&& nvencAlloc(engine, &encoder->coloc[i], (size_t)macroblocks * 64 + 0x1000,
				false)
			&& nvencAlloc(engine, &encoder->mePred[i], (size_t)macroblocks * 64 + 0x1000,
				false);
	}
	if (!ok) {
		set_reason(reason, reasonSize, "Not enough memory on the card for NVENC.");
		nvencH264Close(encoder);
		return NULL;
	}

	write_parameter_sets(encoder);
	if (config->rate_control == NVENC_RC_CONSTQP) {
		snprintf(encoder->description, sizeof(encoder->description),
			"H.264 Main, constant QP %u, I and P pictures", config->qp);
	} else {
		snprintf(encoder->description, sizeof(encoder->description),
			"H.264 Main, %s %u kbit/s, I and P pictures",
			config->rate_control == NVENC_RC_CBR ? "constant" : "average",
			config->bitrate / 1000);
	}
	return encoder;
}

void
nvencH264Close(NvencH264 *encoder)
{
	if (encoder == NULL)
		return;
	NvencEngine *engine = encoder->engine;
	for (int i = 0; i < 2; i++) {
		nvencFree(engine, &encoder->recon[i]);
		nvencFree(engine, &encoder->coloc[i]);
		nvencFree(engine, &encoder->mePred[i]);
	}
	nvencFree(engine, &encoder->rcProcess);
	nvencFree(engine, &encoder->history);
	nvencFree(engine, &encoder->status);
	nvencFree(engine, &encoder->bitstream);
	nvencFree(engine, &encoder->inputPicture);
	nvencFree(engine, &encoder->picSetup);
	free(encoder);
}

size_t
nvencH264Headers(NvencH264 *encoder, uint8_t *buffer, size_t size)
{
	if (size < encoder->headersSize)
		return 0;
	memcpy(buffer, encoder->headers, encoder->headersSize);
	return encoder->headersSize;
}

const char *
nvencH264Describe(NvencH264 *encoder)
{
	return encoder->description;
}

static void
fill_pic_setup(NvencH264 *encoder, bool idr, bool intra, int qp)
{
	const nvenc_config *config = &encoder->config;
	uint8_t *base = encoder->picSetup.data;
	memset(base, 0, PIC_SETUP_SIZE);
	nvenc_h264_drv_pic_setup_s *setup = (nvenc_h264_drv_pic_setup_s *)base;

	/* The class number and the header's version 0, revision 6; the
	 * microcode checks the upper 24 bits. */
	setup->magic = (nvencCard(encoder->engine)->encoderClass << 16) | 0x0006;
	fill_surface(&setup->refpic_cfg, &encoder->reference, encoder->layout, true);
	fill_surface(&setup->input_cfg, &encoder->input, encoder->layout, false);
	fill_surface(&setup->outputpic_cfg, &encoder->reference, encoder->layout, true);

	nvenc_h264_sps_data_s *sps = &setup->sps_data;
	sps->profile_idc = 77;
	sps->level_idc = encoder->level;
	sps->chroma_format_idc = 1;
	sps->pic_order_cnt_type = 2;
	sps->log2_max_frame_num_minus4 = LOG2_MAX_FRAME_NUM - 4;
	sps->log2_max_pic_order_cnt_lsb_minus4 = 0;
	sps->frame_mbs_only = 1;

	nvenc_h264_pps_data_s *pps = &setup->pps_data;
	pps->pic_param_set_id = 0;
	pps->entropy_coding_mode_flag = 1;
	pps->num_ref_idx_l0_active_minus1 = 0;
	pps->num_ref_idx_l1_active_minus1 = 0;
	pps->weighted_bipred_idc = 0;
	pps->pic_init_qp_minus26 = 0;
	pps->chroma_qp_index_offset = 0;
	pps->second_chroma_qp_index_offset = 0;
	pps->constrained_intra_pred_flag = 0;
	pps->deblocking_filter_control_present_flag = 1;
	pps->transform_8x8_mode_flag = 0;
	pps->pic_order_present_flag = 0;
	pps->weighted_pred_flag = 0;

	nvenc_h264_rc_s *rc = &setup->rate_control;
	rc->QP[0] = qp;
	rc->QP[1] = qp;
	rc->QP[2] = qp;
	rc->minQP[0] = rc->minQP[1] = rc->minQP[2] = MIN_QP;
	rc->maxQP[0] = rc->maxQP[1] = rc->maxQP[2] = MAX_QP;
	rc->framerate = config->frame_rate_den != 0
		? (int)(config->frame_rate_num / config->frame_rate_den) : 30;
	rc->gop_length = config->key_interval;

	nvenc_h264_pic_control_s *pic = &setup->pic_control;
	memset(pic->l0, -1, sizeof(pic->l0));
	memset(pic->l1, -1, sizeof(pic->l1));
	if (!intra) {
		pic->l0[0] = 0;			/* SET_IN_REF_PIC0 */
		pic->temp_dist_l0[0] = 2;	/* POC type 2 counts in twos */
	}
	pic->max_byte_count_before_resid_zero = (uint32_t)encoder->bitstream.size;
	pic->frame_num = encoder->frameNum;
	pic->pic_order_cnt_lsb = (2 * encoder->frameNum) & 0xf;
	pic->idr_pic_id = encoder->idrPicId;
	pic->num_forced_slices_minus1 = 0;
	pic->num_me_controls_minus1 = 0;
	pic->num_md_controls_minus1 = 0;
	pic->num_q_controls_minus1 = 0;
	pic->slice_control_offset = SLICE_CONTROL_OFFSET;
	pic->me_control_offset = ME_CONTROL_OFFSET;
	pic->md_control_offset = MD_CONTROL_OFFSET;
	pic->q_control_offset = QUANT_CONTROL_OFFSET;
	pic->hist_buf_size = (uint32_t)encoder->history.size;
	pic->bitstream_buf_size = (uint32_t)encoder->bitstream.size;
	pic->bitstream_start_pos = 0;
	pic->pic_struct = 0;			/* frame */
	pic->pic_type = idr ? 3 : intra ? 2 : 0;
	pic->ref_pic_flag = 1;
	pic->slice_mode = 1;			/* the slice control array */
	pic->base_view = 1;
	pic->codec = 3;				/* H.264, the decoder's numbers */
	pic->e4byteStartCode = 1;
	pic->slice_stat_offset = STATUS_SLICE_OFFSET;
	pic->mpec_stat_offset = STATUS_MPEC_OFFSET;
	pic->aq_stat_offset = STATUS_AQ_OFFSET;
	pic->stats_fifo_offset = STATUS_FIFO_OFFSET;
	pic->strips_in_frame = 1;
	pic->slice_encoding_row_start = 0;
	pic->slice_encoding_row_num = encoder->heightInMbs;

	fill_slice_control(encoder, (nvenc_h264_slice_control_s *)(base + SLICE_CONTROL_OFFSET),
		qp, intra);
	fill_me_control((nvenc_h264_me_control_s *)(base + ME_CONTROL_OFFSET));
	fill_md_control((nvenc_h264_md_control_s *)(base + MD_CONTROL_OFFSET));
	fill_quant_control((nvenc_h264_quant_control_s *)(base + QUANT_CONTROL_OFFSET));
}

/* The chroma address the engine is given: the picture's base when it adds
 * the configured offset itself, the chroma plane otherwise. */
static uint64_t
chroma_address(const NvencH264 *encoder, const NvencBuffer *buffer,
	const NvencPictureLayout *picture)
{
	return encoder->layout->chromaFromBase ? buffer->gpuAddress
		: buffer->gpuAddress + picture->chromaOffset;
}

int
nvencH264Encode(NvencH264 *encoder, const nvenc_picture *picture, NvencQueue *queue,
	char *reason, size_t reasonSize)
{
	bool idr = encoder->count == 0 || picture->force_key
		|| encoder->indexInGop >= (int)encoder->config.key_interval;
	if (idr) {
		encoder->indexInGop = 0;
		encoder->frameNum = 0;
		if (encoder->count > 0)
			encoder->idrPicId = (encoder->idrPicId + 1) & 0xffff;
	}
	bool intra = idr;
	int qp = nvencRateQp(&encoder->rc, intra);

	nvencStorePicture(encoder->inputPicture.data, &encoder->input, encoder->layout, picture);
	fill_pic_setup(encoder, idr, intra, qp);
	memset(encoder->status.data, 0, sizeof(nvenc_pic_stat_s));
	memset(encoder->bitstream.data, 0, 64);

	int current = encoder->count % 2;
	int previous = 1 - current;
	NvencEngine *engine = encoder->engine;
	nvencBegin(engine);
	nvencMethod(engine, NVENC_SET_APPLICATION_ID, NVENC_APPLICATION_H264);
	nvencMethod(engine, NVENC_SET_WATCHDOG_TIMER, 0);
	nvencMethod(engine, NVENC_SET_CONTROL_PARAMS,
		NVENC_CONTROL_CODEC_H264 | NVENC_CONTROL_FORCE_OUT_PIC
		| NVENC_CONTROL_RCMODE(0));
	nvencMethod(engine, NVENC_SET_PICTURE_INDEX, encoder->count);
	nvencAddress(engine, NVENC_SET_IN_DRV_PIC_SETUP, encoder->picSetup.gpuAddress);
	nvencAddress(engine, NVENC_SET_IO_RC_PROCESS, encoder->rcProcess.gpuAddress);
	nvencAddress(engine, NVENC_SET_OUT_ENC_STATUS, encoder->status.gpuAddress);
	nvencAddress(engine, NVENC_SET_OUT_BITSTREAM, encoder->bitstream.gpuAddress);
	nvencAddress(engine, NVENC_SET_IOHISTORY, encoder->history.gpuAddress);
	nvencAddress(engine, NVENC_SET_IN_CUR_PIC, encoder->inputPicture.gpuAddress);
	uint64_t inputChroma = chroma_address(encoder, &encoder->inputPicture, &encoder->input);
	nvencAddress(engine, NVENC_SET_IN_CUR_PIC_CHROMA_U, inputChroma);
	nvencAddress(engine, NVENC_SET_IN_CUR_PIC_CHROMA_V, inputChroma);
	nvencAddress(engine, NVENC_SET_OUT_REF_PIC_LUMA, encoder->recon[current].gpuAddress);
	nvencAddress(engine, NVENC_SET_OUT_REF_PIC_CHROMA,
		chroma_address(encoder, &encoder->recon[current], &encoder->reference));
	nvencAddress(engine, NVENC_SET_OUT_COLOC_DATA, encoder->coloc[current].gpuAddress);
	nvencAddress(engine, NVENC_SET_IN_COLOC_DATA, encoder->coloc[previous].gpuAddress);
	nvencAddress(engine, NVENC_SET_OUT_MEPRED_DATA, encoder->mePred[current].gpuAddress);
	nvencAddress(engine, NVENC_SET_IN_MEPRED_DATA, encoder->mePred[previous].gpuAddress);
	if (!intra) {
		nvencAddress(engine, NVENC_SET_IN_REF_PIC_LUMA(0),
			encoder->recon[previous].gpuAddress);
		nvencAddress(engine, NVENC_SET_IN_REF_PIC_CHROMA(0),
			chroma_address(encoder, &encoder->recon[previous], &encoder->reference));
	}
	nvencMethod(engine, NVENC_EXECUTE, 1u << 0);
	char why[160];
	int executed = nvencExecute(engine,
		encoder->config.timeout_ms != 0 ? (int)encoder->config.timeout_ms : 2000, why,
		sizeof(why));
	if (executed != 0) {
		if (getenv("NVENC_DEBUG") != NULL) {
			const uint32_t *words = encoder->status.data;
			fprintf(stderr, "nvenc: status");
			for (int i = 0; i < 16; i++)
				fprintf(stderr, " %08x", words[i]);
			fprintf(stderr, "\n");
			fprintf(stderr, "nvenc: status words:");
			for (size_t i = 16; i < encoder->status.size / 4; i++) {
				if (words[i] != 0)
					fprintf(stderr, " %zx:%08x", i * 4, words[i]);
			}
			fprintf(stderr, "\n");
		}
		if (reason != NULL)
			snprintf(reason, reasonSize, "NVENC: %s", why);
		return executed;
	}

	const nvenc_pic_stat_s *stat = encoder->status.data;
	if (stat->error_status != 0 || stat->ucode_error_status != 0) {
		if (reason != NULL) {
			snprintf(reason, reasonSize, "NVENC reported error %u (microcode %#x)",
				stat->error_status, stat->ucode_error_status);
		}
		return NVENC_FAILED;
	}
	size_t bytes = (stat->total_bit_count + 7) / 8;
	const uint8_t *slice = encoder->bitstream.data;
	if (bytes < 5 || bytes > encoder->bitstream.size) {
		if (reason != NULL) {
			snprintf(reason, reasonSize, "NVENC wrote %zu bytes for a picture", bytes);
		}
		return NVENC_FAILED;
	}
	/* The engine writes slice NALs from where it was told to start. */
	size_t skip = 0;
	while (skip + 3 < bytes && !(slice[skip] == 0 && slice[skip + 1] == 0
			&& slice[skip + 2] == 1))
		skip++;
	if (skip > 1 || skip + 3 >= bytes || (slice[skip + 3] & 0x1f) == 0) {
		if (reason != NULL) {
			snprintf(reason, reasonSize,
				"NVENC's output is no H.264 slice (%02x %02x %02x %02x %02x)",
				slice[0], slice[1], slice[2], slice[3], slice[4]);
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
	memcpy(packet + at, slice, bytes);
	bool queued = nvencQueuePush(queue, packet, total, picture->pts, picture->pts, idr);
	free(packet);
	if (!queued) {
		set_reason(reason, reasonSize, "out of memory");
		return NVENC_FAILED;
	}

	nvencRateUpdate(&encoder->rc, bytes, intra);
	encoder->count++;
	encoder->indexInGop++;
	encoder->frameNum = (encoder->frameNum + 1) % (1 << LOG2_MAX_FRAME_NUM);
	return 0;
}
