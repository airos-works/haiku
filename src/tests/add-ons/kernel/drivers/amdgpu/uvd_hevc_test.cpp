/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include "UvdHevc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <initializer_list>
#ifdef AMDGPU_MESA_UVD_ORACLE
#include <ac_uvd_dec.h>
#endif

#define CHECK(value) do { if (!(value)) { \
	fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); abort(); } } while (0)

static amdgpu_hevc_picture Picture()
{
	amdgpu_hevc_picture p = {};
	p.sps_flags = 0xe7; p.pps_flags = 0xfffff; p.nal_type = 1;
	p.current_slot = 0; p.log2_max_poc_lsb_minus4 = 4; p.max_dec_pic_buffering_minus1 = 6;
	p.log2_diff_max_min_cb_size = 3; p.log2_diff_max_min_tb_size = 3;
	p.max_transform_hierarchy_depth_inter = 2; p.max_transform_hierarchy_depth_intra = 3;
	p.num_extra_slice_header_bits = 2; p.num_short_term_ref_pic_sets = 4;
	p.num_long_term_ref_pics_sps = 3; p.ref_l0_minus1 = 2; p.ref_l1_minus1 = 1;
	p.cb_qp_offset = -2; p.cr_qp_offset = 3; p.beta_offset_div2 = -3; p.tc_offset_div2 = 4;
	p.diff_cu_qp_delta_depth = 2; p.tile_columns_minus1 = 2; p.tile_rows_minus1 = 1;
	p.log2_parallel_merge_level_minus2 = 2; p.initial_qp_minus26 = -4;
	p.num_delta_pocs_ref_rps_idx = 5;
	p.column_width_minus1[0] = 3; p.column_width_minus1[1] = 5;
	p.row_height_minus1[0] = 7;
	p.current_poc = 2;
	memset(p.reference_slot, 0x7f, sizeof(p.reference_slot));
	p.reference_slot[0] = 1; p.reference_slot[1] = 4; p.reference_slot[2] = 7;
	p.reference_poc[0] = -4; p.reference_poc[1] = 4; p.reference_poc[2] = 20;
	memset(p.st_before, 0xff, sizeof(p.st_before));
	memset(p.st_after, 0xff, sizeof(p.st_after));
	memset(p.lt_current, 0xff, sizeof(p.lt_current));
	p.st_before[0] = 0; p.st_after[0] = 1; p.lt_current[0] = 2;
	unsigned value = 1;
	for (auto& a : p.scaling4x4) for (auto& v : a) v = value++ % 251 + 1;
	for (auto& a : p.scaling8x8) for (auto& v : a) v = value++ % 251 + 1;
	for (auto& a : p.scaling16x16) for (auto& v : a) v = value++ % 251 + 1;
	for (auto& a : p.scaling32x32) for (auto& v : a) v = value++ % 251 + 1;
	for (auto& v : p.dc16x16) v = value++ % 251 + 1;
	for (auto& v : p.dc32x32) v = value++ % 251 + 1;
	return p;
}

static uint32_t Get32(const uint8_t* bytes, size_t offset)
{
	return uint32_t(bytes[offset]) | (uint32_t(bytes[offset + 1]) << 8)
		| (uint32_t(bytes[offset + 2]) << 16) | (uint32_t(bytes[offset + 3]) << 24);
}

static void Guards(const uint8_t* bytes, size_t payload)
{
	for (unsigned i = 0; i < 32; i++) CHECK(bytes[i] == 0xa5 && bytes[32 + payload + i] == 0xa5);
}

static void Bounds(const amdgpu::UvdHevcLayout& l)
{
	CHECK(l.bytes <= amdgpu::kUvdHevcMaxSessionBytes && l.bytes % 4096 == 0);
	CHECK(l.message + amdgpu::kUvdMessageBytes <= l.feedback);
	CHECK(l.feedback + 4096 <= l.scaling);
	CHECK(l.scaling + amdgpu::kUvdHevcScalingBytes <= l.session);
	CHECK(l.session + 128 * 1024 <= l.bitstream);
	CHECK(l.bitstream + AMDGPU_VIDEO_MAX_BITSTREAM == l.guards[0]);
	CHECK(l.guards[0] + 4096 == l.dpb);
	CHECK(l.dpb + l.dpbBytes <= l.guards[1]);
	CHECK(l.guards[1] + 4096 == l.context);
	CHECK(l.context + l.contextBytes <= l.guards[2]);
	CHECK(l.guards[2] + 4096 == l.target);
	CHECK(l.target + l.outputBytes <= l.guards[3]);
	CHECK(l.guards[3] + 4096 == l.bytes);
}

#ifdef AMDGPU_MESA_UVD_ORACLE
static void Oracle(const uint8_t* message, const amdgpu_hevc_config& c,
	const amdgpu_hevc_picture& p)
{
	static_assert(sizeof(ruvd_msg) == amdgpu::kUvdMessageBytes, "Mesa UVD message ABI");
	ruvd_msg expected = {};
	expected.size = sizeof(expected); expected.msg_type = RUVD_MSG_DECODE;
	expected.stream_handle = 19; expected.status_report_feedback_number = 7;
	auto& d = expected.body.decode;
	d.stream_type = RUVD_CODEC_H265; d.decode_flags = 1;
	d.width_in_samples = 1920; d.height_in_samples = 1080;
	d.dpb_size = c.profile == 1 ? 53268480 : 79902720;
	d.dpb_reserved = c.profile == 1 ? 3101008 : 2287104;
	d.db_pitch = 1920; d.bsd_size = 1408;
	// si_uvd_set_dt_surfaces: surface pitch is in samples, while
	// texture_offset() supplies byte offsets for the planes.
	d.dt_pitch = c.profile == 1 ? 2048 : 1920;
	d.dt_chroma_top_offset = d.dt_chroma_bottom_offset
		= (c.profile == 1 ? 2048 : 3840) * 1088;
	d.dt_wa_chroma_top_offset = d.dt_pitch / 2; d.extension_support = 1;
	auto& h = d.codec.h265;
	h.sps_info_flags = p.sps_flags; h.pps_info_flags = p.pps_flags;
	h.chroma_format = 1; h.bit_depth_luma_minus8 = h.bit_depth_chroma_minus8 = c.bit_depth - 8;
	h.log2_max_pic_order_cnt_lsb_minus4 = p.log2_max_poc_lsb_minus4;
	h.sps_max_dec_pic_buffering_minus1 = p.max_dec_pic_buffering_minus1;
	h.log2_min_luma_coding_block_size_minus3 = p.log2_min_cb_size_minus3;
	h.log2_diff_max_min_luma_coding_block_size = p.log2_diff_max_min_cb_size;
	h.log2_min_transform_block_size_minus2 = p.log2_min_tb_size_minus2;
	h.log2_diff_max_min_transform_block_size = p.log2_diff_max_min_tb_size;
	h.max_transform_hierarchy_depth_inter = p.max_transform_hierarchy_depth_inter;
	h.max_transform_hierarchy_depth_intra = p.max_transform_hierarchy_depth_intra;
	h.pcm_sample_bit_depth_luma_minus1 = p.pcm_bit_depth_luma_minus1;
	h.pcm_sample_bit_depth_chroma_minus1 = p.pcm_bit_depth_chroma_minus1;
	h.log2_min_pcm_luma_coding_block_size_minus3 = p.log2_min_pcm_cb_size_minus3;
	h.log2_diff_max_min_pcm_luma_coding_block_size = p.log2_diff_max_min_pcm_cb_size;
	h.num_extra_slice_header_bits = p.num_extra_slice_header_bits;
	h.num_short_term_ref_pic_sets = p.num_short_term_ref_pic_sets;
	h.num_long_term_ref_pic_sps = p.num_long_term_ref_pics_sps;
	h.num_ref_idx_l0_default_active_minus1 = p.ref_l0_minus1;
	h.num_ref_idx_l1_default_active_minus1 = p.ref_l1_minus1;
	h.pps_cb_qp_offset = p.cb_qp_offset; h.pps_cr_qp_offset = p.cr_qp_offset;
	h.pps_beta_offset_div2 = p.beta_offset_div2; h.pps_tc_offset_div2 = p.tc_offset_div2;
	h.diff_cu_qp_delta_depth = p.diff_cu_qp_delta_depth;
	h.num_tile_columns_minus1 = p.tile_columns_minus1; h.num_tile_rows_minus1 = p.tile_rows_minus1;
	h.log2_parallel_merge_level_minus2 = p.log2_parallel_merge_level_minus2;
	memcpy(h.column_width_minus1, p.column_width_minus1, sizeof(p.column_width_minus1));
	memcpy(h.row_height_minus1, p.row_height_minus1, sizeof(p.row_height_minus1));
	h.init_qp_minus26 = p.initial_qp_minus26;
	h.num_delta_pocs_ref_rps_idx = p.num_delta_pocs_ref_rps_idx; h.curr_idx = p.current_slot;
	h.curr_poc = p.current_poc;
	memcpy(h.ref_pic_list, p.reference_slot, sizeof(p.reference_slot));
	memcpy(h.poc_list, p.reference_poc, sizeof(p.reference_poc));
	memcpy(h.ref_pic_set_st_curr_before, p.st_before, sizeof(p.st_before));
	memcpy(h.ref_pic_set_st_curr_after, p.st_after, sizeof(p.st_after));
	memcpy(h.ref_pic_set_lt_curr, p.lt_current, sizeof(p.lt_current));
	memcpy(h.ucScalingListDCCoefSizeID2, p.dc16x16, sizeof(p.dc16x16));
	memcpy(h.ucScalingListDCCoefSizeID3, p.dc32x32, sizeof(p.dc32x32));
	if (c.profile == 2) h.p010_mode = h.msb_mode = 1;
	CHECK(memcmp(message, &expected, sizeof(expected)) == 0);
}
#endif

int main()
{
	// Exercise the session policy used before every firmware submission.
	// Failed admission must not publish a slot; dropped references cannot return.
	auto state = Picture();
	state.nal_type = 19;
	memset(state.reference_slot, 0x7f, sizeof(state.reference_slot));
	uint32_t next = 0xdeadbeef;
	CHECK(amdgpu::UvdHevcNextReferences(state, 0, true, next) && next == 1);
	state.nal_type = 1; state.current_slot = 1; state.reference_slot[0] = 0;
	CHECK(!amdgpu::UvdHevcNextReferences(state, 0, false, next) && next == 1);
	CHECK(amdgpu::UvdHevcNextReferences(state, 1, false, next) && next == 3);
	CHECK(!amdgpu::UvdHevcNextReferences(state, 1, true, next) && next == 3);
	state.nal_type = 21;
	CHECK(!amdgpu::UvdHevcNextReferences(state, 1, true, next));
	state.current_slot = 15; state.reference_slot[0] = 1;
	CHECK(amdgpu::UvdHevcNextReferences(state, 3, false, next) && next == 0x8002);
	state.current_slot = 2; state.reference_slot[0] = 0;
	CHECK(!amdgpu::UvdHevcNextReferences(state, next, false, next) && next == 0x8002);
	for (uint8_t bad : {uint8_t(16), uint8_t(32), uint8_t(255)}) {
		state.reference_slot[0] = bad;
		CHECK(!amdgpu::UvdHevcNextReferences(state, 0xffff, false, next));
	}
	puts("PASS: session references require completed retained slots and first-picture random access");
	amdgpu_hevc_config c = {1920, 1080, 1, 8, 6, 6, {0, 0}};
	amdgpu_hevc_picture p = Picture();
	amdgpu::UvdHevcLayout l;
	CHECK(amdgpu::UvdHevcSize(c, l)); Bounds(l);
	CHECK(l.pitch == 2048 && l.outputHeight == 1088 && l.outputBytes == 3342336);
	CHECK(l.dpbBytes == 53268480 && l.contextBytes == 3101008);
	CHECK(amdgpu::UvdHevcValidate(c, p, 1321));
	uint8_t message[amdgpu::kUvdMessageBytes + 64];
	memset(message, 0xa5, sizeof(message));
	CHECK(amdgpu::UvdHevcMessage(message + 32, 1, 19, c, &p, 1321, 7));
	Guards(message, amdgpu::kUvdMessageBytes);
#ifdef AMDGPU_MESA_UVD_ORACLE
	Oracle(message + 32, c, p);
#endif
	c.profile = 2; c.bit_depth = 10;
	CHECK(amdgpu::UvdHevcSize(c, l)); Bounds(l);
	CHECK(l.pitch == 3840 && l.outputHeight == 1088 && l.outputBytes == 6266880);
	CHECK(l.dpbBytes == 79902720 && l.contextBytes == 2287104);
	CHECK(amdgpu::UvdHevcMessage(message + 32, 1, 19, c, &p, 1321, 7));
	CHECK(Get32(message + 32, 0x70) == 1920); // P010 sample stride, not 3840 bytes.
	CHECK(Get32(message + 32, 0x88) == 3840 * 1088); // UV byte offset.
	CHECK(Get32(message + 32, 0x98) == 960);
	Guards(message, amdgpu::kUvdMessageBytes);
#ifdef AMDGPU_MESA_UVD_ORACLE
	Oracle(message + 32, c, p);
	puts("PASS: complete Main and Main 10 firmware messages match Mesa's independent ABI");
#endif
	auto pcm = p; pcm.sps_flags |= 1 << 3;
	pcm.pcm_bit_depth_luma_minus1 = pcm.pcm_bit_depth_chroma_minus1 = 9;
	pcm.log2_min_pcm_cb_size_minus3 = 1; pcm.log2_diff_max_min_pcm_cb_size = 1;
	CHECK(amdgpu::UvdHevcMessage(message + 32, 1, 19, c, &pcm, 1321, 7));
#ifdef AMDGPU_MESA_UVD_ORACLE
	Oracle(message + 32, c, pcm);
#endif
	// A 4K Main 10 layout must fit the available aperture; larger reference
	// demands must fail explicitly, without overflowing into a small allocation.
	auto large = c; large.width = 3840; large.height = 2160;
	CHECK(amdgpu::UvdHevcSize(large, l)); Bounds(l);
	CHECK(l.pitch == 7680 && l.outputHeight == 2160 && l.outputBytes == 24883200);
	large.max_references = 16;
	CHECK(!amdgpu::UvdHevcSize(large, l));
	uint32_t invalid[] = {0, 1, 15, 17, 4097, 0x7fffffff, 0xffffffff};
	for (uint32_t value : invalid) {
		auto bad = c; bad.width = value; CHECK(!amdgpu::UvdHevcSize(bad, l));
		bad = c; bad.height = value; CHECK(!amdgpu::UvdHevcSize(bad, l));
	}
	for (uint32_t value : {0u, 3u, 0xffffffffu}) {
		auto bad = c; bad.profile = value; CHECK(!amdgpu::UvdHevcSize(bad, l));
	}
	auto badConfig = c; badConfig.log2_ctb_size = 32; CHECK(!amdgpu::UvdHevcSize(badConfig, l));
	badConfig = c; badConfig.max_references = 17; CHECK(!amdgpu::UvdHevcSize(badConfig, l));
	badConfig = c; badConfig.bit_depth = 12; CHECK(!amdgpu::UvdHevcSize(badConfig, l));
	badConfig = c; badConfig.reserved[1] = 1; CHECK(!amdgpu::UvdHevcSize(badConfig, l));
	CHECK(!amdgpu::UvdHevcValidate(c, p, 0));
	CHECK(!amdgpu::UvdHevcValidate(c, p, AMDGPU_VIDEO_MAX_BITSTREAM + 1));
	CHECK(amdgpu::UvdHevcValidate(c, p, AMDGPU_VIDEO_MAX_BITSTREAM));
	#define REJECT(field, value) do { auto bad = p; bad.field = value; \
		CHECK(!amdgpu::UvdHevcValidate(c, bad, 1321)); } while (0)
	REJECT(current_slot, 16); REJECT(current_slot, 4); REJECT(reference_slot[1], 1);
	REJECT(reference_slot[0], 16); REJECT(reference_slot[0], 0xff);
	REJECT(st_before[0], 15); REJECT(st_after[0], 0); REJECT(st_before[2], 2);
	REJECT(log2_min_cb_size_minus3, 255); REJECT(log2_min_tb_size_minus2, 255);
	REJECT(log2_diff_max_min_tb_size, 4); REJECT(log2_max_poc_lsb_minus4, 13);
	REJECT(max_transform_hierarchy_depth_inter, 255); REJECT(max_dec_pic_buffering_minus1, 2);
	REJECT(column_width_minus1[0], 29); REJECT(row_height_minus1[0], 16);
	REJECT(column_width_minus1[18], 1); REJECT(tile_columns_minus1, 20);
	REJECT(tile_rows_minus1, 22); REJECT(sps_flags, 0x1e7); REJECT(pps_flags, 0x1fffff);
	REJECT(sps_flags, 0xc7); REJECT(nal_type, 19); REJECT(nal_type, 31);
	REJECT(initial_qp_minus26, -39); REJECT(initial_qp_minus26, 26);
	REJECT(cb_qp_offset, -13); REJECT(tc_offset_div2, 7); REJECT(num_delta_pocs_ref_rps_idx, 17);
	REJECT(num_short_term_ref_pic_sets, 65); REJECT(reserved[2], 1);
	REJECT(pcm_bit_depth_luma_minus1, 7);
	#undef REJECT
	const uint8_t slice[] = {0, 0, 1, 2, 1, 0x80};
	CHECK(amdgpu::UvdHevcBitstream(slice, sizeof(slice), 1));
	CHECK(!amdgpu::UvdHevcBitstream(slice, sizeof(slice), 19));
	CHECK(!amdgpu::UvdHevcBitstream(NULL, sizeof(slice), 1));
	for (unsigned length = 0; length < sizeof(slice); length++)
		CHECK(!amdgpu::UvdHevcBitstream(slice, length, 1));
	uint8_t two[12]; memcpy(two, slice, 6); memcpy(two + 6, slice, 6);
	CHECK(amdgpu::UvdHevcBitstream(two, sizeof(two), 1));
	two[10] = 0; CHECK(!amdgpu::UvdHevcBitstream(two, sizeof(two), 1));
	two[10] = 9; CHECK(!amdgpu::UvdHevcBitstream(two, sizeof(two), 1));
	two[10] = 1; two[9] = 3; CHECK(!amdgpu::UvdHevcBitstream(two, sizeof(two), 1));
	uint8_t scaling[amdgpu::kUvdHevcScalingBytes + 64]; memset(scaling, 0xa5, sizeof(scaling));
	amdgpu::UvdHevcScaling(scaling + 32, p); Guards(scaling, amdgpu::kUvdHevcScalingBytes);
	CHECK(memcmp(scaling + 32, p.scaling4x4, 96) == 0);
	CHECK(memcmp(scaling + 32 + 96, p.scaling8x8, 384) == 0);
	CHECK(memcmp(scaling + 32 + 480, p.scaling16x16, 384) == 0);
	CHECK(memcmp(scaling + 32 + 864, p.scaling32x32, 128) == 0);
	// Mutate actual client metadata and bitstream bytes under ASan/UBSan.
	// Invalid requests must not partially overwrite a command message.
	uint32_t random = 0x5620354a;
	for (unsigned trial = 0; trial < 25000; trial++) {
		auto bad = p;
		random = random * 1664525 + 1013904223;
		reinterpret_cast<uint8_t*>(&bad)[random % sizeof(bad)] ^= uint8_t((random >> 17) | 1);
		memset(message, 0xa5, sizeof(message));
		bool valid = amdgpu::UvdHevcValidate(c, bad, 1321);
		CHECK(amdgpu::UvdHevcMessage(message + 32, 1, 19, c, &bad, 1321, 7) == valid);
		Guards(message, amdgpu::kUvdMessageBytes);
		if (!valid) for (uint8_t byte : message) CHECK(byte == 0xa5);
		uint8_t packet[64]; for (auto& byte : packet) { random = random * 1664525 + 1013904223; byte = random >> 24; }
		amdgpu::UvdHevcBitstream(packet, random % sizeof(packet), random % 64);
	}
	CHECK(amdgpu::UvdHevcMessage(message + 32, 0, 19, c, NULL, 0, 0));
	CHECK(amdgpu::UvdHevcMessage(message + 32, 2, 19, c, NULL, 0, 0));
	CHECK(!amdgpu::UvdHevcMessage(message + 32, 3, 19, c, NULL, 0, 0));
	CHECK(!amdgpu::UvdHevcMessage(message + 32, 1, 19, c, NULL, 1321, 7));
	CHECK(!amdgpu::UvdHevcMessage(message + 32, 1, 0, c, &p, 1321, 7));
	puts("PASS: HEVC geometry, metadata, slot/tile bounds, VCL-only packets, scaling guards and 25000 mutations");
	puts("Hardware HEVC execution, parser integration and P010 pixels remain unqualified.");
}
