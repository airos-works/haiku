/*
 * Copyright 2011 Advanced Micro Devices, Inc.
 * Copyright 2026, air/OS.
 * Distributed under the MIT license in UvdRegisters.h.
 * Polaris HEVC protocol and sizing: Mesa radeonsi/radeon_uvd.c, ac_uvd_dec.h.
 */
#include "UvdHevc.h"
#include <string.h>

namespace amdgpu {
namespace {
uint32_t Align(uint32_t value, uint32_t alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}
void Put(uint8_t* data, uint32_t offset, uint32_t value)
{
	for (unsigned i = 0; i < 4; i++) data[offset + i] = value >> (8 * i);
}
void Put16(uint8_t* data, uint32_t offset, uint16_t value)
{
	data[offset] = value; data[offset + 1] = value >> 8;
}
bool IsVcl(uint8_t type)
{
	return type <= 9 || (type >= 16 && type <= 21);
}
}

bool UvdHevcSize(const amdgpu_hevc_config& c, UvdHevcLayout& l)
{
	memset(&l, 0, sizeof(l));
	if (c.width < 16 || c.height < 16 || c.width > 4096 || c.height > 4096
		|| ((c.width | c.height) & 7) != 0 || c.max_references > 16
		|| c.log2_ctb_size < 4 || c.log2_ctb_size > 6 || c.reserved[0] || c.reserved[1]
		|| (c.profile != 1 && c.profile != 2) || (c.bit_depth != 8 && c.bit_depth != 10)
		|| (c.profile == 1 && c.bit_depth != 8)) return false;
	// Bounds above precede every shift/product. Use the same conservative DPB
	// count and Polaris alignment as Mesa; Main 10's DPB is firmware-compressed.
	uint32_t width = Align(c.width, 16), height = Align(c.height, 16);
	uint32_t references = c.max_references + 1;
	uint32_t minimum = c.width * c.height >= 4096 * 2000 ? 8 : 17;
	if (references < minimum) references = minimum;
	if (c.profile == 1) {
		l.dpbBytes = Align(width * height * 3 / 2, 256) * references;
		l.contextBytes = ((width + 255) / 16) * ((height + 255) / 16)
			* 16 * references + 52 * 1024;
	} else {
		l.dpbBytes = Align(width * height * 9 / 4, 256) * references;
		uint32_t ctb = 1u << c.log2_ctb_size;
		uint32_t widthInCTB = (width + ctb - 1) / ctb;
		uint32_t heightInCTB = (height + ctb - 1) / ctb;
		uint32_t blocksPerCTB = (ctb / 16) * (ctb / 16);
		uint32_t contextRow = Align(widthInCTB * blocksPerCTB * 16, 256);
		uint32_t maxMBAddress = (height * 8 + 2047) / 2048;
		uint32_t pixelLeft = (c.bit_depth == 10 ? 2 : 1) * (maxMBAddress * 4096 + 1024);
		l.contextBytes = references * contextRow * heightInCTB + 24576 + pixelLeft;
	}
	// Main uses NV12; Main 10 uses full-precision P010 (MSBs, no 10-to-8 conversion).
	l.pitch = Align(c.width * (c.profile == 2 ? 2 : 1), 256);
	l.outputHeight = height;
	l.outputBytes = l.pitch * height * 3 / 2;
	l.message = 0; l.feedback = 4096; l.scaling = 8192; l.session = 12288;
	l.bitstream = l.session + 128 * 1024;
	l.guards[0] = l.bitstream + AMDGPU_VIDEO_MAX_BITSTREAM;
	l.dpb = l.guards[0] + 4096;
	l.guards[1] = l.dpb + Align(l.dpbBytes, 4096);
	l.context = l.guards[1] + 4096;
	l.guards[2] = l.context + Align(l.contextBytes, 4096);
	l.target = l.guards[2] + 4096;
	l.guards[3] = l.target + Align(l.outputBytes, 4096);
	l.bytes = l.guards[3] + 4096;
	return l.bytes <= kUvdHevcMaxSessionBytes;
}

bool UvdHevcValidate(const amdgpu_hevc_config& c, const amdgpu_hevc_picture& p,
	uint32_t bytes)
{
	UvdHevcLayout layout;
	if (!UvdHevcSize(c, layout) || bytes == 0 || bytes > AMDGPU_VIDEO_MAX_BITSTREAM
		|| !IsVcl(p.nal_type) || p.current_slot >= 16
		|| (p.sps_flags & ~0xffu) || (p.pps_flags & ~0xfffffu)
		|| p.log2_max_poc_lsb_minus4 > 12 || p.max_dec_pic_buffering_minus1 > 15
		|| p.max_dec_pic_buffering_minus1 > c.max_references
		|| p.log2_min_cb_size_minus3 > 3 || p.log2_diff_max_min_cb_size > 3
		|| p.log2_min_cb_size_minus3 + 3u + p.log2_diff_max_min_cb_size != c.log2_ctb_size
		|| p.log2_min_tb_size_minus2 > 3 || p.log2_diff_max_min_tb_size > 3
		|| p.log2_min_tb_size_minus2 + 2 > p.log2_min_cb_size_minus3 + 3
		|| p.log2_min_tb_size_minus2 + 2 + p.log2_diff_max_min_tb_size > 5
		|| p.log2_min_tb_size_minus2 + 2u + p.log2_diff_max_min_tb_size > c.log2_ctb_size
		|| p.max_transform_hierarchy_depth_inter > c.log2_ctb_size - (p.log2_min_tb_size_minus2 + 2)
		|| p.max_transform_hierarchy_depth_intra > c.log2_ctb_size - (p.log2_min_tb_size_minus2 + 2)
		|| p.num_extra_slice_header_bits > 7 || p.num_short_term_ref_pic_sets > 64
		|| p.num_long_term_ref_pics_sps > 32 || p.ref_l0_minus1 > 14 || p.ref_l1_minus1 > 14
		|| p.cb_qp_offset < -12 || p.cb_qp_offset > 12 || p.cr_qp_offset < -12 || p.cr_qp_offset > 12
		|| p.beta_offset_div2 < -6 || p.beta_offset_div2 > 6 || p.tc_offset_div2 < -6 || p.tc_offset_div2 > 6
		|| p.initial_qp_minus26 < -(26 + 6 * (int(c.bit_depth) - 8)) || p.initial_qp_minus26 > 25
		|| p.diff_cu_qp_delta_depth > p.log2_diff_max_min_cb_size
		|| p.log2_parallel_merge_level_minus2 + 2u > c.log2_ctb_size
		|| p.num_delta_pocs_ref_rps_idx > 16 || p.reserved[0] || p.reserved[1] || p.reserved[2])
		return false;
	if (((c.width | c.height) & ((1u << (p.log2_min_cb_size_minus3 + 3)) - 1)) != 0)
		return false;
	if ((p.sps_flags & (1 << 3)) != 0) {
		if (p.pcm_bit_depth_luma_minus1 + 1u > c.bit_depth
			|| p.pcm_bit_depth_chroma_minus1 + 1u > c.bit_depth
			|| p.log2_min_pcm_cb_size_minus3 > 2 || p.log2_diff_max_min_pcm_cb_size > 2
			|| p.log2_min_pcm_cb_size_minus3 + 3 + p.log2_diff_max_min_pcm_cb_size > 5
			|| p.log2_min_pcm_cb_size_minus3 + 3u + p.log2_diff_max_min_pcm_cb_size > c.log2_ctb_size)
			return false;
	} else if (p.pcm_bit_depth_luma_minus1 || p.pcm_bit_depth_chroma_minus1
		|| p.log2_min_pcm_cb_size_minus3 || p.log2_diff_max_min_pcm_cb_size || (p.sps_flags & (1 << 4)))
		return false;
	if (!(p.sps_flags & (1 << 5)) && p.num_long_term_ref_pics_sps != 0) return false;
	uint32_t widthInCTB = (c.width + (1u << c.log2_ctb_size) - 1) >> c.log2_ctb_size;
	uint32_t heightInCTB = (c.height + (1u << c.log2_ctb_size) - 1) >> c.log2_ctb_size;
	if (p.tile_columns_minus1 > 19 || p.tile_rows_minus1 > 21
		|| p.tile_columns_minus1 >= widthInCTB || p.tile_rows_minus1 >= heightInCTB
		|| (!(p.pps_flags & (1 << 11)) && (p.tile_columns_minus1 || p.tile_rows_minus1))) return false;
	uint32_t columns = 0, rows = 0;
	for (unsigned i = 0; i < 19; i++) {
		if (i < p.tile_columns_minus1) columns += uint32_t(p.column_width_minus1[i]) + 1;
		else if (p.column_width_minus1[i]) return false;
	}
	for (unsigned i = 0; i < 21; i++) {
		if (i < p.tile_rows_minus1) rows += uint32_t(p.row_height_minus1[i]) + 1;
		else if (p.row_height_minus1[i]) return false;
	}
	if (columns >= widthInCTB || rows >= heightInCTB) return false;
	uint32_t slots = 0, references = 0;
	for (uint8_t slot : p.reference_slot) {
		if (slot == 0x7f) continue;
		if (slot >= 16 || slot == p.current_slot || (slots & (1u << slot))) return false;
		slots |= 1u << slot; references++;
	}
	if (references > p.max_dec_pic_buffering_minus1) return false;
	uint32_t used = 0;
	const uint8_t* lists[] = {p.st_before, p.st_after, p.lt_current};
	for (const uint8_t* list : lists) {
		bool ended = false;
		for (unsigned i = 0; i < 8; i++) {
			uint8_t index = list[i];
			if (index == 0xff) { ended = true; continue; }
			if (ended || index >= 16 || p.reference_slot[index] == 0x7f || (used & (1u << index))) return false;
			used |= 1u << index;
		}
	}
	if (!(p.sps_flags & (1 << 5)) && p.lt_current[0] != 0xff) return false;
	if (p.nal_type >= 16 && p.nal_type <= 20 && references != 0) return false;
	return true;
}

bool UvdHevcNextReferences(const amdgpu_hevc_picture& p,
	uint32_t completedSlots, bool firstPicture, uint32_t& nextSlots)
{
	if (!IsVcl(p.nal_type) || p.current_slot >= 16) return false;
	uint32_t references = 0;
	for (uint8_t slot : p.reference_slot) {
		if (slot == 0x7f) continue;
		if (slot >= 16 || slot == p.current_slot || (references & (1u << slot))) return false;
		references |= 1u << slot;
	}
	if ((references & ~completedSlots) != 0
		|| (firstPicture && (p.nal_type < 16 || p.nal_type > 21 || references))) return false;
	nextSlots = references | (1u << p.current_slot);
	return true;
}

bool UvdHevcBitstream(const uint8_t* data, uint32_t bytes, uint8_t nalType)
{
	if (data == NULL || bytes < 6 || bytes > AMDGPU_VIDEO_MAX_BITSTREAM || !IsVcl(nalType)) return false;
	uint32_t zeros = 0, slices = 0, payload = 0;
	for (uint32_t i = 0; i < bytes; i++) {
		if (data[i] == 1 && zeros >= 2) {
			if ((slices && payload <= zeros) || ++slices > 4096 || i + 3 >= bytes) return false;
			uint8_t first = data[++i], second = data[++i];
			if ((first & 0x80) || ((first >> 1) & 63) != nalType || (first & 1)
				|| (second & 0xf8) || !(second & 7)) return false;
			zeros = payload = 0;
		} else {
			if (!slices && data[i]) return false;
			zeros = data[i] == 0 ? zeros + 1 : 0;
			payload++;
		}
	}
	return slices && payload > zeros;
}

bool UvdHevcMessage(uint8_t* msg, uint32_t type, uint32_t handle,
	const amdgpu_hevc_config& c, const amdgpu_hevc_picture* p, uint32_t bytes, uint32_t frame)
{
	UvdHevcLayout l;
	if (msg == NULL || !handle || type > 2 || (type != 2 && !UvdHevcSize(c, l))
		|| (type == 1 && (p == NULL || !frame || !UvdHevcValidate(c, *p, bytes)))) return false;
	memset(msg, 0, kUvdMessageBytes);
	Put(msg, 0, kUvdMessageBytes); Put(msg, 4, type); Put(msg, 8, handle);
	if (type == 2) return true;
	Put(msg, 0x10, 16); // RUVD_CODEC_H265
	if (type == 0) {
		Put(msg, 0x1c, c.width); Put(msg, 0x20, c.height); Put(msg, 0x28, l.dpbBytes);
		return true;
	}
	Put(msg, 0x0c, frame); Put(msg, 0x14, 1);
	Put(msg, 0x18, c.width); Put(msg, 0x1c, c.height);
	Put(msg, 0x24, l.dpbBytes); Put(msg, 0x2c, l.contextBytes);
	Put(msg, 0x34, Align(c.width, 16)); Put(msg, 0x58, Align(bytes, 128));
	// Mesa si_uvd_set_dt_surfaces uses nblk_x * blk_w: samples, not
	// bytes. P010 stores each sample in two bytes; plane offsets remain
	// byte addresses. Supplying the byte stride doubles firmware row steps.
	uint32_t targetPitch = l.pitch / (c.profile == 2 ? 2 : 1);
	Put(msg, 0x70, targetPitch);
	Put(msg, 0x88, l.pitch * l.outputHeight); Put(msg, 0x8c, l.pitch * l.outputHeight);
	Put(msg, 0x98, targetPitch / 2);
	uint8_t* h = msg + 0xe0;
	Put(h, 0, p->sps_flags); Put(h, 4, p->pps_flags);
	h[8] = 1; h[9] = h[10] = c.bit_depth - 8; h[11] = p->log2_max_poc_lsb_minus4;
	h[12] = p->max_dec_pic_buffering_minus1;
	h[13] = p->log2_min_cb_size_minus3; h[14] = p->log2_diff_max_min_cb_size;
	h[15] = p->log2_min_tb_size_minus2; h[16] = p->log2_diff_max_min_tb_size;
	h[17] = p->max_transform_hierarchy_depth_inter; h[18] = p->max_transform_hierarchy_depth_intra;
	h[19] = p->pcm_bit_depth_luma_minus1; h[20] = p->pcm_bit_depth_chroma_minus1;
	h[21] = p->log2_min_pcm_cb_size_minus3; h[22] = p->log2_diff_max_min_pcm_cb_size;
	h[23] = p->num_extra_slice_header_bits; h[24] = p->num_short_term_ref_pic_sets;
	h[25] = p->num_long_term_ref_pics_sps; h[26] = p->ref_l0_minus1; h[27] = p->ref_l1_minus1;
	h[28] = p->cb_qp_offset; h[29] = p->cr_qp_offset; h[30] = p->beta_offset_div2; h[31] = p->tc_offset_div2;
	h[32] = p->diff_cu_qp_delta_depth; h[33] = p->tile_columns_minus1; h[34] = p->tile_rows_minus1;
	h[35] = p->log2_parallel_merge_level_minus2;
	for (unsigned i = 0; i < 19; i++) Put16(h, 36 + i * 2, p->column_width_minus1[i]);
	for (unsigned i = 0; i < 21; i++) Put16(h, 74 + i * 2, p->row_height_minus1[i]);
	h[116] = p->initial_qp_minus26; h[117] = p->num_delta_pocs_ref_rps_idx; h[118] = p->current_slot;
	Put(h, 120, p->current_poc);
	memcpy(h + 124, p->reference_slot, 16);
	for (unsigned i = 0; i < 16; i++) Put(h, 140 + i * 4, p->reference_poc[i]);
	memcpy(h + 204, p->st_before, 8); memcpy(h + 212, p->st_after, 8); memcpy(h + 220, p->lt_current, 8);
	memcpy(h + 228, p->dc16x16, 6); memcpy(h + 234, p->dc32x32, 2);
	if (c.profile == 2) h[238] = h[239] = 1; // P010, MSB-aligned
	msg[0xce0] = 1;
	return true;
}

void UvdHevcScaling(uint8_t* data, const amdgpu_hevc_picture& p)
{
	memcpy(data, p.scaling4x4, 96); memcpy(data + 96, p.scaling8x8, 384);
	memcpy(data + 480, p.scaling16x16, 384); memcpy(data + 864, p.scaling32x32, 128);
}
}
