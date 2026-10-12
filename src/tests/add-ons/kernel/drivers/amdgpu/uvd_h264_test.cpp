/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include "UvdH264.h"
#include "Firmware.h"
#include "UvdFixture.h"
#include <assert.h>
#include <initializer_list>
#include <stdio.h>
#include <string.h>
#ifdef AMDGPU_MESA_UVD_ORACLE
#include <ac_uvd_dec.h>
#endif

int main()
{
	amdgpu_h264_config config = {864, 480, 100, 30, 2, 0};
	amdgpu::UvdH264Layout layout;
	assert(amdgpu::UvdH264Size(config, layout));
	assert(layout.pitch == 1024 && layout.outputBytes == 737280);
	assert(layout.dpbBytes == 3735552 && layout.contextBytes == 1866240);
	assert(layout.bytes % 4096 == 0 && layout.guards[3] + 4096 == layout.bytes);
	assert(layout.bitstream + AMDGPU_VIDEO_MAX_BITSTREAM == layout.guards[0]);
	assert(layout.dpb + layout.dpbBytes <= layout.guards[1]);
	assert(layout.context + layout.contextBytes <= layout.guards[2]);
	assert(layout.target + layout.outputBytes <= layout.guards[3]);
	for (uint32_t width : {0u, 15u, 17u, 4097u, 0xffffffffu}) {
		auto bad = config; bad.width = width;
		amdgpu::UvdH264Layout rejected;
		assert(!amdgpu::UvdH264Size(bad, rejected));
	}
	for (uint32_t profile : {0u, 110u, 244u, 0xffffffffu}) {
		auto bad = config; bad.profile = profile;
		amdgpu::UvdH264Layout rejected;
		assert(!amdgpu::UvdH264Size(bad, rejected));
	}
	auto badConfig = config; badConfig.max_references = 17;
	amdgpu::UvdH264Layout rejected;
	assert(!amdgpu::UvdH264Size(badConfig, rejected));
	amdgpu_h264_picture picture = {};
	picture.flags = AMDGPU_H264_IDR; picture.sps_flags = 5; picture.pps_flags = 0x88;
	picture.log2_frame_num_minus4 = 1; picture.log2_poc_lsb_minus4 = 3;
	picture.initial_qp_minus26 = 2;
	memset(picture.scaling4x4, 16, sizeof(picture.scaling4x4));
	memset(picture.scaling8x8, 16, sizeof(picture.scaling8x8));
	assert(amdgpu::UvdH264Validate(config, picture, sizeof(uvd_bitstream)));
	assert(!amdgpu::UvdH264Validate(config, picture, 0));
	assert(!amdgpu::UvdH264Validate(config, picture, AMDGPU_VIDEO_MAX_BITSTREAM + 1));
	auto bad = picture; bad.frame_num = 32;
	assert(!amdgpu::UvdH264Validate(config, bad, 64));
	bad = picture; bad.reference_frame_num[15] = 0xffffffff;
	assert(!amdgpu::UvdH264Validate(config, bad, 64));
	bad = picture; bad.log2_frame_num_minus4 = 255;
	assert(!amdgpu::UvdH264Validate(config, bad, 64));
	bad = picture; bad.sps_flags = 0x85;
	assert(!amdgpu::UvdH264Validate(config, bad, 64));
	bad = picture; bad.scaling8x8[1][63] = 0;
	assert(!amdgpu::UvdH264Validate(config, bad, 64));
	assert(amdgpu::UvdH264Bitstream(uvd_bitstream, sizeof(uvd_bitstream), true));
	assert(!amdgpu::UvdH264Bitstream(uvd_bitstream, sizeof(uvd_bitstream), false));
	const uint8_t sps[] = {0, 0, 1, 0x67, 0x80};
	const uint8_t empty[] = {0, 0, 1, 0x65, 0, 0, 1, 0x65, 0x80};
	assert(!amdgpu::UvdH264Bitstream(sps, sizeof(sps), true));
	assert(!amdgpu::UvdH264Bitstream(empty, sizeof(empty), true));
	assert(!amdgpu::UvdH264Bitstream(NULL, 16, true));
	uint8_t guarded[amdgpu::kUvdMessageBytes + 32]; memset(guarded, 0xa5, sizeof(guarded));
	uint8_t* message = guarded + 16;
	amdgpu::UvdH264Message(message, 1, 19, config, layout, &picture, sizeof(uvd_bitstream), 7);
	assert(amdgpu::ReadLE32(message) == 3556 && amdgpu::ReadLE32(message + 8) == 19);
	assert(amdgpu::ReadLE32(message + 0x18) == 864);
	assert(amdgpu::ReadLE32(message + 0x1c) == 480);
	assert(amdgpu::ReadLE32(message + 0x58) == 1408);
	assert(amdgpu::ReadLE32(message + 0x70) == 1024);
	assert(amdgpu::ReadLE32(message + 0x88) == 491520);
	assert(amdgpu::ReadLE32(message + 0x8c) == 491520);
	assert(amdgpu::ReadLE32(message + 0x98) == 512);
	assert(message[0xce0] == 1);
#ifdef AMDGPU_MESA_UVD_ORACLE
	// Optional external protocol oracle from the local Mesa reference checkout.
	// Compare the complete wire message using its independently maintained ABI.
	static_assert(sizeof(ruvd_msg) == amdgpu::kUvdMessageBytes, "Mesa UVD message size");
	ruvd_msg expected = {};
	expected.size = sizeof(expected); expected.msg_type = RUVD_MSG_DECODE;
	expected.stream_handle = 19; expected.status_report_feedback_number = 7;
	auto& decode = expected.body.decode;
	decode.stream_type = RUVD_CODEC_H264_PERF; decode.decode_flags = 1;
	decode.width_in_samples = 864; decode.height_in_samples = 480;
	decode.dpb_size = 3735552; decode.dpb_reserved = 1866240;
	decode.db_pitch = 864; decode.bsd_size = 1408;
	decode.dt_pitch = 1024;
	decode.dt_chroma_top_offset = decode.dt_chroma_bottom_offset = 491520;
	decode.dt_wa_chroma_top_offset = 512; decode.extension_support = 1;
	auto& h264 = decode.codec.h264;
	h264.profile = RUVD_H264_PROFILE_HIGH; h264.level = 30;
	h264.sps_info_flags = 5; h264.pps_info_flags = 0x88; h264.chroma_format = 1;
	h264.log2_max_frame_num_minus4 = 1; h264.log2_max_pic_order_cnt_lsb_minus4 = 3;
	h264.num_ref_frames = 2; h264.pic_init_qp_minus26 = 2;
	memset(h264.scaling_list_4x4, 16, sizeof(h264.scaling_list_4x4));
	memset(h264.scaling_list_8x8, 16, sizeof(h264.scaling_list_8x8));
	assert(memcmp(message, &expected, sizeof(expected)) == 0);
	puts("PASS: complete firmware message matches Mesa's independent wire ABI");
#endif
	for (unsigned i = 0; i < 16; i++) {
		assert(guarded[i] == 0xa5 && guarded[16 + amdgpu::kUvdMessageBytes + i] == 0xa5);
	}
	puts("PASS: H264 layout arithmetic, metadata bounds, slice-only input and firmware message extents");
}
