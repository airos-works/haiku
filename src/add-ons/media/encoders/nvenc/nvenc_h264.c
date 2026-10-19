/* See nvenc_h264.h.
 *
 * The structures are NVIDIA's nvenc_drv.h (in Mesa's nouveau headers). What
 * the header does not say was found on a GTX 1070 (GP104, class C2B7) and
 * is kept to what that card was seen to need; README.md has the list. The
 * tuning values of the motion search, mode decision and quantisation
 * controls are the ones NVIDIA's own driver uses, as recorded by an encoder
 * written for an RTX 3050 (Mesa merge request 45349). */

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
 * offsets. */
#define SLICE_CONTROL_OFFSET	0x400
#define ME_CONTROL_OFFSET	0x480
#define MD_CONTROL_OFFSET	0x540
#define QUANT_CONTROL_OFFSET	0x5c0
#define PIC_SETUP_SIZE		0x1000

#define STATUS_SIZE		0x1000

#define LOG2_MAX_FRAME_NUM	8
#define MIN_QP			10
#define MAX_QP			51

/* what the status buffer's error_status says when a picture is done */
#define PICTURE_DONE		2

struct NvencH264 {
	NvencEngine	*engine;
	nvenc_config	config;
	NvencPictureLayout input;
	NvencPictureLayout reference;
	int		widthInMbs, heightInMbs;
	int		level;
	int		count;			/* pictures so far */
	int		indexInGop;
	int		frameNum;
	int		idrPicId;
	NvencRateControl rc;

	NvencBuffer	picSetup, inputPicture, bitstream, status, history;
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


/* Input, reference and reconstructed pictures are all in the video block
 * linear arrangement (nvencBlockLinearOffset); block_height counts groups of
 * eight lines. Chroma has its own address, so its offset stays 0. The size
 * is the coded one, whole macroblocks: the engine refuses a width that is
 * not a multiple of 16, and the SPS crops the picture. */
static void
fill_surface(nvenc_h264_surface_cfg_s *cfg, const NvencH264 *encoder,
	const NvencPictureLayout *picture)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->frame_width_minus1 = encoder->widthInMbs * 16 - 1;
	cfg->frame_height_minus1 = encoder->heightInMbs * 16 - 1;
	cfg->sfc_pitch = picture->pitch;
	cfg->sfc_pitch_chroma = picture->pitch;
	cfg->block_height = 2;
}

static void
fill_md_control(nvenc_h264_md_control_s *md)
{
	memset(md, 0, sizeof(*md));
	md->intra_luma4x4_mode_enable = 0x1ff;
	md->intra_luma8x8_mode_enable = 0;	/* Main profile: no 8x8 transform */
	md->intra_luma16x16_mode_enable = 0xf;
	md->intra_chroma_mode_enable = 0xf;
	md->ip_search_mode = 1 | 4;		/* 4x4 and 16x16 */
	md->early_intra_mode_control = 3;
	md->early_intra_mode_type_16x16dc = 1;
	md->early_intra_mode_type_16x16h = 1;
	md->early_intra_mode_type_16x16v = 1;
	md->early_termination_ip1 = 1;
	md->priority_ipred_type_ip1 = 1;
	md->inter_penalty_factor_for_ip1 = 16;
	md->l0_part_16x16_enable = 1;
	md->l0_part_16x8_enable = 1;
	md->l0_part_8x16_enable = 1;
	md->l0_part_8x8_enable = 1;
	md->pskip_enable = 1;
	md->skip_evaluate_enable = 1;
	md->mv_cost_enable = 1;
	md->rdo_level = 1;
	md->tu_search_num = 3;
	md->num_1div8_lambda_ssd = 1;
	md->intra_ssd_cnt_4x4 = 1;
	md->intra_ssd_cnt_8x8 = 1;
	md->intra_ssd_cnt_16x16 = 1;

	/* biases against costs, multiplied by lambda */
	md->multiply_bias_with_lambda = 1;
	md->bias_inter_16x8 = -1;
	md->bias_inter_8x16 = -1;
	md->bias_inter_8x8 = -1;
	md->bias_pskip = -1;
	md->bias_intra_over_inter = -2;
	md->bias_intra_16x16 = -4;
	md->bias_intra_8x8 = -2;
	md->bias_intra_4x4 = -1;
	md->intra_nxn_bias_multiplier = 32;
	md->intra_most_prob_bias_multiplier = 3;
	md->pskip_bias_multiplier = 2;
	md->pskip_esc_threshold = 9;
	md->tempmv_wt_spread_threshold = 0xffff;
	md->tempmv_wt_distort_threshold = 0xffff;
}

/* Rounding towards zero by a third of a step for intra and a sixth for
 * inter blocks, less for the higher frequencies of a 4x4 intra block, in
 * 1/2048ths; left at zero the pictures lose 4 dB at the same QP. */
static void
fill_quant_control(nvenc_h264_quant_control_s *quant)
{
	static const uint16_t kIntra4x4[16] = { 1023, 878, 820, 682, 878, 820, 682, 512,
		820, 682, 512, 410, 682, 512, 410, 410 };
	static const uint16_t kInter4x4[16] = { 682, 586, 546, 456, 586, 546, 456, 342,
		546, 456, 342, 292, 456, 342, 292, 274 };
	memset(quant, 0, sizeof(*quant));
	for (int i = 0; i < 16; i++) {
		quant->dz_4x4_YI[i] = kIntra4x4[i];
		quant->dz_4x4_YP[i] = kInter4x4[i];
		quant->dz_8x8_YI[i] = 682;
		quant->dz_8x8_YP[i] = 342;
	}
	quant->dz_4x4_CI = 682;
	quant->dz_4x4_CP = 342;
	/* dropping lone coefficients whose cost exceeds their worth */
	quant->qpp_run_vector_4x4 = 0x056b;
	quant->qpp_run_vector_8x8[0] = 0xaaff;
	quant->qpp_run_vector_8x8[1] = 0xffaa;
	quant->qpp_run_vector_8x8[2] = 0x000f;
	quant->qpp_luma8x8_cost = 15;
	quant->qpp_luma16x16_cost = 15;
	quant->qpp_chroma_cost = 15;
}

static void
fill_slice_control(NvencH264 *encoder, nvenc_h264_slice_control_s *slice, int qp)
{
	memset(slice, 0, sizeof(*slice));
	slice->num_mb = encoder->widthInMbs * encoder->heightInMbs;
	slice->qp_avr = qp;
	slice->qp_slice_min = qp;
	slice->qp_slice_max = qp;
	slice->disable_deblocking_filter_idc = 0;
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
	/* The history keeps 384 bytes per column of macroblocks; NVIDIA's driver
	 * says as much in hist_buf_size, and a smaller buffer faults the card's
	 * memory unit at 1080p. */
	size_t historySize = ((size_t)encoder->widthInMbs + 16) * 384;
	size_t bitstreamSize = (size_t)encoder->input.pitch * encoder->input.alignedHeight * 2
		+ 0x10000;

	bool ok = nvencAlloc(engine, &encoder->picSetup, PIC_SETUP_SIZE, true)
		&& nvencAlloc(engine, &encoder->inputPicture, encoder->input.size, true)
		&& nvencAlloc(engine, &encoder->bitstream, bitstreamSize, true)
		&& nvencAlloc(engine, &encoder->status, STATUS_SIZE, true)
		&& nvencAlloc(engine, &encoder->history, historySize, false);
	for (int i = 0; ok && i < 2; i++) {
		/* co-located data is 64 bytes a macroblock; the motion predictors
		 * are not documented and get twice that */
		ok = nvencAlloc(engine, &encoder->recon[i], encoder->reference.size, false)
			&& nvencAlloc(engine, &encoder->coloc[i], (size_t)macroblocks * 64 + 0x1000,
				false)
			&& nvencAlloc(engine, &encoder->mePred[i], (size_t)macroblocks * 128 + 0x1000,
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
fill_pic_setup(NvencH264 *encoder, bool idr, int qp)
{
	const nvenc_config *config = &encoder->config;
	uint8_t *base = encoder->picSetup.data;
	memset(base, 0, PIC_SETUP_SIZE);
	nvenc_h264_drv_pic_setup_s *setup = (nvenc_h264_drv_pic_setup_s *)base;

	/* The class number and the header's version 0, revision 6; the
	 * microcode checks the upper 24 bits. */
	setup->magic = (nvencCard(encoder->engine)->encoderClass << 16) | 0x0006;
	fill_surface(&setup->refpic_cfg, encoder, &encoder->reference);
	fill_surface(&setup->input_cfg, encoder, &encoder->input);
	fill_surface(&setup->outputpic_cfg, encoder, &encoder->reference);

	nvenc_h264_sps_data_s *sps = &setup->sps_data;
	sps->profile_idc = 77;
	sps->level_idc = encoder->level;
	sps->chroma_format_idc = 1;
	sps->pic_order_cnt_type = 2;
	sps->log2_max_frame_num_minus4 = LOG2_MAX_FRAME_NUM - 4;
	sps->frame_mbs_only = 1;

	nvenc_h264_pps_data_s *pps = &setup->pps_data;
	pps->entropy_coding_mode_flag = 1;
	pps->deblocking_filter_control_present_flag = 1;

	/* The engine runs at a constant QP chosen per picture (nvencRateQp). */
	nvenc_h264_rc_s *rc = &setup->rate_control;
	for (int i = 0; i < 3; i++) {
		rc->QP[i] = rc->minQP[i] = rc->maxQP[i] = qp;
		rc->rhopbi[i] = 1 << 8;
	}
	rc->framerate = config->frame_rate_den != 0
		? (int)(config->frame_rate_num / config->frame_rate_den) : 30;
	rc->buffersize = (uint32_t)encoder->bitstream.size;
	rc->gop_length = config->key_interval;

	nvenc_h264_pic_control_s *pic = &setup->pic_control;
	memset(pic->l0, -1, sizeof(pic->l0));
	memset(pic->l1, -1, sizeof(pic->l1));
	if (!idr) {
		pic->l0[0] = 0;			/* SET_IN_REF_PIC0 */
		pic->temp_dist_l0[0] = 2;	/* POC type 2 counts in twos */
	}
	pic->frame_num = encoder->frameNum;
	pic->idr_pic_id = encoder->idrPicId;
	pic->slice_control_offset = SLICE_CONTROL_OFFSET;
	pic->me_control_offset = ME_CONTROL_OFFSET;
	pic->md_control_offset = MD_CONTROL_OFFSET;
	pic->q_control_offset = QUANT_CONTROL_OFFSET;
	pic->hist_buf_size = (uint32_t)encoder->history.size;
	pic->bitstream_buf_size = (uint32_t)encoder->bitstream.size;
	pic->pic_type = idr ? 3 : 0;
	pic->ref_pic_flag = 1;
	/* One slice: dynamic insertion with no size limit. Static slices from
	 * the slice control array (1) make the engine raise an error. */
	pic->slice_mode = 0;
	pic->cabac_zero_word_enable = 1;
	/* Not the decoder's codec numbers: 3 makes the engine raise an error. */
	pic->codec = 0;
	/* skip the 4x4 intra search where the 8x8 blocks are flat */
	pic->intra_sel_4x4_enable = 1;
	pic->intra_sel_4x4_threshold = 1000;
	pic->chroma_skip_threshold_4x4 = 8;
	/* cycles for the application's timer, which only runs with GPTIMER_ON */
	int macroblocks = encoder->widthInMbs * encoder->heightInMbs;
	setup->gpTimer_timeout_val = 46080u * (macroblocks > 3600 ? macroblocks : 3600);

	fill_slice_control(encoder, (nvenc_h264_slice_control_s *)(base + SLICE_CONTROL_OFFSET),
		qp);
	nvencFillMeControl(base + ME_CONTROL_OFFSET);
	fill_md_control((nvenc_h264_md_control_s *)(base + MD_CONTROL_OFFSET));
	fill_quant_control((nvenc_h264_quant_control_s *)(base + QUANT_CONTROL_OFFSET));
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
	int qp = nvencRateQp(&encoder->rc, idr);

	nvencStorePicture(encoder->inputPicture.data, &encoder->input,
		&kNvencLayouts[encoder->config.variant], picture);
	fill_pic_setup(encoder, idr, qp);
	memset(encoder->status.data, 0, sizeof(nvenc_pic_stat_s));

	int current = encoder->count % 2;
	int previous = 1 - current;
	size_t inputChroma = encoder->input.chromaOffset;
	size_t referenceChroma = encoder->reference.chromaOffset;
	NvencEngine *engine = encoder->engine;
	nvencBegin(engine);
	nvencMethod(engine, NVENC_SET_APPLICATION_ID, NVENC_APPLICATION_H264);
	/* Without FORCE_OUT_PIC the reconstructed picture is not written, even
	 * with ref_pic_flag set, and P pictures predict from black. */
	nvencMethod(engine, NVENC_SET_CONTROL_PARAMS,
		NVENC_CONTROL_CODEC_H264 | NVENC_CONTROL_FORCE_OUT_PIC | NVENC_CONTROL_RCMODE(0));
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
	/* the temporal hints of the motion search read the previous picture's */
	nvencAddress(engine, NVENC_SET_OUT_MEPRED_DATA, encoder->mePred[current].gpuAddress);
	nvencAddress(engine, NVENC_SET_IN_MEPRED_DATA, encoder->mePred[previous].gpuAddress);
	if (!idr) {
		nvencAddress(engine, NVENC_SET_IN_REF_PIC_LUMA(0),
			encoder->recon[previous].gpuAddress);
		nvencAddress(engine, NVENC_SET_IN_REF_PIC_CHROMA(0),
			encoder->recon[previous].gpuAddress + referenceChroma);
	}
	nvencMethod(engine, NVENC_EXECUTE, 0);
	char why[160];
	int executed = nvencExecute(engine,
		encoder->config.timeout_ms != 0 ? (int)encoder->config.timeout_ms : 2000, why,
		sizeof(why));
	if (executed != 0) {
		if (getenv("NVENC_DEBUG") != NULL)
			dump_status("failed", &encoder->status);
		if (reason != NULL)
			snprintf(reason, reasonSize, "NVENC: %s", why);
		return executed;
	}

	const nvenc_pic_stat_s *stat = encoder->status.data;
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

	nvencRateUpdate(&encoder->rc, bytes, idr);
	encoder->count++;
	encoder->indexInGop++;
	encoder->frameNum = (encoder->frameNum + 1) % (1 << LOG2_MAX_FRAME_NUM);
	return 0;
}
