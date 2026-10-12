/*
 * Copyright 2011 Advanced Micro Devices, Inc.
 * Copyright 2026, air/OS.
 * Distributed under the MIT license in UvdRegisters.h.
 * Polaris H.264 PERF layout: Mesa radeonsi/radeon_uvd.c and ac_uvd_dec.h.
 */
#include "UvdH264.h"
#include <string.h>

namespace amdgpu {
static uint32_t Align(uint32_t n, uint32_t a) { return (n + a - 1) & ~(a - 1); }
static void Put(uint8_t* p, uint32_t offset, uint32_t value)
{
	for (unsigned i = 0; i < 4; i++) p[offset + i] = value >> (8 * i);
}

bool UvdH264Size(const amdgpu_h264_config& c, UvdH264Layout& l)
{
	memset(&l, 0, sizeof(l));
	if (c.width < 16 || c.height < 16 || c.width > 4096 || c.height > 4096
		|| ((c.width | c.height) & 15) != 0 || c.max_references > 16 || c.reserved != 0
		|| (c.profile != 66 && c.profile != 77 && c.profile != 100))
		return false;
	uint32_t maxDpbMbs;
	switch (c.level) {
		case 10: maxDpbMbs = 396; break;
		case 11: maxDpbMbs = 900; break;
		case 12: case 13: case 20: maxDpbMbs = 2376; break;
		case 21: maxDpbMbs = 4752; break;
		case 22: case 30: maxDpbMbs = 8100; break;
		case 31: maxDpbMbs = 18000; break;
		case 32: maxDpbMbs = 20480; break;
		case 40: case 41: maxDpbMbs = 32768; break;
		case 42: maxDpbMbs = 34816; break;
		case 50: maxDpbMbs = 110400; break;
		case 51: case 52: maxDpbMbs = 184320; break;
		default: return false;
	}
	// Mesa's firmware sizing uses the conservative default for these levels.
	if (c.level != 30 && c.level != 31 && c.level != 32 && c.level != 41
		&& c.level != 42 && c.level != 50 && c.level != 51)
		maxDpbMbs = 184320;
	uint32_t mbs = c.width / 16 * Align(c.height / 16, 2);
	// Match UVD's lean reference sizing on amdgpu: one extra picture plus
	// the greater of the level-derived DPB count and the declared references.
	uint32_t frames = maxDpbMbs / mbs + 1;
	if (frames > 17) frames = 17;
	if (frames < c.max_references + 1) frames = c.max_references + 1;
	l.dpbBytes = Align(c.width * c.height * 3 / 2, 1024) * frames;
	l.contextBytes = Align(mbs * 192, 256) * frames;
	l.pitch = Align(c.width, 256);
	l.outputHeight = c.height;
	l.outputBytes = l.pitch * c.height * 3 / 2;
	l.message = 0;
	l.feedback = 4096;
	l.scaling = 8192;
	l.session = 12288;
	l.bitstream = l.session + 128 * 1024;
	l.guards[0] = l.bitstream + AMDGPU_VIDEO_MAX_BITSTREAM;
	l.dpb = l.guards[0] + 4096;
	l.guards[1] = l.dpb + Align(l.dpbBytes, 4096);
	l.context = l.guards[1] + 4096;
	l.guards[2] = l.context + Align(l.contextBytes, 4096);
	l.target = l.guards[2] + 4096;
	l.guards[3] = l.target + Align(l.outputBytes, 4096);
	l.bytes = l.guards[3] + 4096;
	// Bound each session's kernel-only mapping inside the CPU aperture.
	return l.bytes <= 160 * 1024 * 1024;
}

bool UvdH264Validate(const amdgpu_h264_config& c, const amdgpu_h264_picture& p,
	uint32_t bytes)
{
	if (bytes == 0 || bytes > AMDGPU_VIDEO_MAX_BITSTREAM || (p.flags & ~AMDGPU_H264_IDR) != 0
		|| (p.sps_flags & ~0x2du) != 0 || (p.sps_flags & 4) == 0
		|| (p.pps_flags & ~0x1ffu) != 0 || ((p.pps_flags >> 4) & 3) > 2
		|| p.log2_frame_num_minus4 > 12 || p.log2_poc_lsb_minus4 > 12 || p.poc_type > 2
		|| p.ref_l0_minus1 > 15 || p.ref_l1_minus1 > 15
		|| p.initial_qp_minus26 < -26 || p.initial_qp_minus26 > 25
		|| p.chroma_qp_offset < -12 || p.chroma_qp_offset > 12
		|| p.second_chroma_qp_offset < -12 || p.second_chroma_qp_offset > 12
		|| p.reserved0 != 0 || p.reserved1[0] != 0 || p.reserved1[1] != 0 || p.reserved1[2] != 0)
		return false;
	if (c.profile == 66 && (p.pps_flags & (1 | 0x100)) != 0)
		return false;
	if (c.profile != 100 && (p.pps_flags & 1) != 0)
		return false;
	uint32_t maxFrame = 1u << (p.log2_frame_num_minus4 + 4);
	if (p.frame_num >= maxFrame || ((p.flags & AMDGPU_H264_IDR) && p.frame_num != 0))
		return false;
	for (unsigned i = 0; i < 16; i++) {
		if (p.reference_frame_num[i] >= maxFrame)
			return false;
	}
	for (auto& list : p.scaling4x4) for (uint8_t value : list) if (value == 0) return false;
	for (auto& list : p.scaling8x8) for (uint8_t value : list) if (value == 0) return false;
	return true;
}

bool UvdH264Bitstream(const uint8_t* data, uint32_t bytes, bool idr)
{
	if (data == NULL || bytes < 5 || bytes > AMDGPU_VIDEO_MAX_BITSTREAM) return false;
	uint32_t zeros = 0, slices = 0, payload = 0;
	for (uint32_t i = 0; i < bytes; i++) {
		if (data[i] == 1 && zeros >= 2) {
			if ((slices != 0 && payload <= zeros) || ++slices > 4096 || ++i >= bytes)
				return false;
			uint8_t nal = data[i];
			if ((nal & 0x80) != 0 || (nal & 31) != (idr ? 5 : 1)
				|| (idr && (nal & 0x60) == 0)) return false;
			zeros = payload = 0;
		} else {
			if (slices == 0 && data[i] != 0) return false;
			zeros = data[i] == 0 ? zeros + 1 : 0;
			payload++;
		}
	}
	return slices != 0 && payload > zeros;
}

void UvdH264Message(uint8_t* msg, uint32_t type, uint32_t handle,
	const amdgpu_h264_config& c, const UvdH264Layout& l,
	const amdgpu_h264_picture* p, uint32_t bytes, uint32_t frame)
{
	memset(msg, 0, kUvdMessageBytes);
	Put(msg, 0, kUvdMessageBytes); Put(msg, 4, type); Put(msg, 8, handle);
	if (type == 2) return;
	Put(msg, 0x10, 7); // RUVD_CODEC_H264_PERF
	if (type == 0) {
		Put(msg, 0x1c, c.width); Put(msg, 0x20, c.height); Put(msg, 0x28, l.dpbBytes);
		return;
	}
	Put(msg, 0x0c, frame);
	Put(msg, 0x14, 1); Put(msg, 0x18, c.width); Put(msg, 0x1c, c.height);
	Put(msg, 0x24, l.dpbBytes); Put(msg, 0x2c, l.contextBytes);
	Put(msg, 0x34, c.width); // linear DB pitch, 16-pixel aligned
	Put(msg, 0x58, Align(bytes, 128));
	Put(msg, 0x70, l.pitch); // linear progressive NV12 output
	Put(msg, 0x88, l.pitch * c.height); Put(msg, 0x8c, l.pitch * c.height);
	Put(msg, 0x98, l.pitch / 2); // dt_wa_chroma_top_offset, Stoney and newer
	uint8_t* avc = msg + 0xe0;
	Put(avc, 0, c.profile == 100 ? 2 : c.profile == 77 ? 1 : 0);
	Put(avc, 4, c.level); Put(avc, 8, p->sps_flags); Put(avc, 12, p->pps_flags);
	avc[16] = 1; avc[19] = p->log2_frame_num_minus4;
	avc[20] = p->poc_type; avc[21] = p->log2_poc_lsb_minus4; avc[22] = c.max_references;
	avc[24] = p->initial_qp_minus26; avc[26] = p->chroma_qp_offset;
	avc[27] = p->second_chroma_qp_offset;
	avc[30] = p->ref_l0_minus1; avc[31] = p->ref_l1_minus1;
	memcpy(avc + 36, p->scaling4x4, 96); memcpy(avc + 132, p->scaling8x8, 128);
	Put(avc, 260, p->frame_num);
	for (unsigned i = 0; i < 16; i++) Put(avc, 264 + 4 * i, p->reference_frame_num[i]);
	Put(avc, 328, p->field_order_count[0]); Put(avc, 332, p->field_order_count[1]);
	for (unsigned i = 0; i < 16; i++) {
		Put(avc, 336 + 8 * i, p->reference_field_order_count[i][0]);
		Put(avc, 340 + 8 * i, p->reference_field_order_count[i][1]);
	}
	Put(avc, 464, p->frame_num); // decoded_pic_idx
	msg[0xce0] = 1; // extension_support
}
}
