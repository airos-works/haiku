/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "CedarEncoder.h"

#include <errno.h>
#include <new>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <OS.h>

#include <sunxi_ve.h>


/*	The encoder's registers (offsets in its own window) that the program
	sets with a meaning known; the rest are Allwinner's values, written as
	their encoder writes them. */
enum {
	INPUT_SIZE			= 0xa00,	// [26:16] width / 8, [10:0] height / 8
	INPUT_STRIDE		= 0xa04,	// [26:16] luma stride / 16
	INPUT_CONTROL		= 0xa08,	// b0, b5 interrupts, b17 first picture
	INPUT_CHROMA_STRIDE	= 0xa14,	// [11:0] chroma stride / 16
	OUTPUT_SIZE			= 0xa2c,	// [26:16] height / 8, [10:0] width / 8
	INPUT_LUMA			= 0xa78,
	INPUT_CHROMA		= 0xa7c,
	INPUT_CHROMA2		= 0xa80,
	INPUT_STATISTICS	= 0xa9c,

	PICTURE				= 0xb04,	// b4 P, b8 CABAC, [11:10] mode count
	QP					= 0xb08,	// [5:0] QP, b8 motion search on,
									// [13:10] width / 640, [20:16] chroma QP
									// offset
	TOOLS				= 0xb10,	// b22: the collocated picture is intra
	INTERRUPTS			= 0xb14,
	RATE_CONTROL		= 0xb2c,	// b31 per block QP from the texture,
									// b28 H.265 cu_qp_delta written
	COLOCATED			= 0xb3c,
	MB_INFO				= 0xb60,
	MOTION_OUT			= 0xb64,
	BITSTREAM			= 0xb80,	// >> 8
	BITSTREAM_END		= 0xb84,
	BITSTREAM_OFFSET	= 0xb88,	// bits
	BITSTREAM_LIMIT		= 0xb8c,
	ROW_TABLE			= 0xb9c,
	REF_LUMA			= 0xba0,
	REF_CHROMA			= 0xba4,
	BLOCK_INFO			= 0xba8,
	REC_LUMA			= 0xbb0,
	REC_CHROMA			= 0xbb4,
	REF_SUBSAMPLED		= 0xbb8,
	REC_SUBSAMPLED		= 0xbbc,
	LINES0				= 0xbc0,
	LINES1				= 0xbc4,
	SAO					= 0xc00		// b8 luma, b9 chroma: the slice
									// header's flags
};

static const size_t kMinBitstream = 2 << 20;
static const uint32 kMinWidth = 176;
static const uint32 kMinHeight = 64;
// H.264's dynamic motion search hangs P pictures narrower than this
static const uint32 kMinDynamicMeBlocks = 13;
static const uint32 kMinQp = 10;
static const uint32 kMaxQp = 47;
static const int32 kHevcChromaQpOffset = -4;

#define ALIGN(x, a) (((x) + (a) - 1) & ~((size_t)(a) - 1))


struct CedarEncoder::Buffer {
	uint32		index;
	uint32		address;	// the engine's
	area_id		area;		// our clone
	uint8*		data;
	size_t		size;
};


/*	A picture's register writes for SUNXI_VE_ENCODE. */
struct CedarEncoder::Ops {
	sunxi_ve_op		ops[256];
	uint32			count;
	bool			overflow;

	Ops() : count(0), overflow(false) {}

	void Write(uint16 reg, uint32 value)
	{
		if (count == sizeof(ops) / sizeof(ops[0])) {
			overflow = true;
			return;
		}
		ops[count].type = SUNXI_VE_OP_WRITE;
		ops[count].reg = reg;
		ops[count].value = value;
		count++;
	}
};


// #pragma mark - software bit writer


namespace {

struct BitBuffer {
	uint8	data[512];
	size_t	bits;

	BitBuffer() : bits(0) { memset(data, 0, sizeof(data)); }

	void Put(uint32 value, int count)
	{
		for (int i = count - 1; i >= 0; i--) {
			if (bits / 8 >= sizeof(data))
				return;
			if ((value >> i) & 1)
				data[bits / 8] |= 0x80 >> (bits % 8);
			bits++;
		}
	}

	void Ue(uint32 value)
	{
		value++;
		int length = 32 - __builtin_clz(value);
		Put(0, length - 1);
		Put(value, length);
	}

	void Se(int32 value)
	{
		Ue(value > 0 ? 2 * value - 1 : -2 * value);
	}

	void Trailing()
	{
		Put(1, 1);
		while ((bits & 7) != 0)
			Put(0, 1);
	}
};


/*!	Start code, then the NAL (its header included in \a rbsp) with
	emulation prevention. \a rbsp ends on a byte.
*/
size_t
append_nal(uint8* out, const BitBuffer& rbsp)
{
	size_t size = 0;
	out[size++] = 0;
	out[size++] = 0;
	out[size++] = 0;
	out[size++] = 1;
	int zeros = 0;
	for (size_t i = 0; i < rbsp.bits / 8; i++) {
		uint8 byte = rbsp.data[i];
		if (zeros == 2 && byte <= 3) {
			out[size++] = 3;
			zeros = 0;
		}
		out[size++] = byte;
		zeros = byte == 0 ? zeros + 1 : 0;
	}
	return size;
}


struct H264Level {
	uint8	idc;
	uint32	maxFrameMbs;
	uint32	maxMbsPerSecond;
};


const H264Level kH264Levels[] = {
	{ 30, 1620, 40500 }, { 31, 3600, 108000 }, { 32, 5120, 216000 },
	{ 40, 8192, 245760 }, { 41, 8192, 245760 }, { 42, 8704, 522240 },
	{ 50, 22080, 589824 }, { 51, 36864, 983040 }, { 52, 36864, 2073600 }
};


struct HevcLevel {
	uint8	idc;			// 30 x the level
	uint32	maxLumaPictureSize;
	uint64	maxLumaSampleRate;
};


const HevcLevel kHevcLevels[] = {
	{ 90, 552960, 16588800 }, { 93, 983040, 33177600 },
	{ 120, 2228224, 66846720 }, { 123, 2228224, 133693440 },
	{ 150, 8912896, 267386880 }, { 153, 8912896, 534773760 },
	{ 156, 8912896, 1069547520 }
};

}	// namespace


// #pragma mark -


CedarEncoder::Params::Params()
	:
	codec(kH264),
	width(0),
	height(0),
	qp(26),
	bitrate(0),
	frameRateNum(30),
	frameRateDen(1),
	keyInterval(60),
	sarNum(0),
	sarDen(0),
	colourPrimaries(2),
	transfer(2),
	matrix(2),
	fullRange(false)
{
}


CedarEncoder::CedarEncoder()
	:
	fDevice(-1),
	fWidth16(0),
	fHeight16(0),
	fStride(0),
	fBitstream(NULL),
	fChromaOffset(0),
	fBlockInfo(NULL),
	fColocated(NULL),
	fStatistics(NULL),
	fRowTable(NULL),
	fHeaders(NULL),
	fHeadersSize(0),
	fOutput(NULL),
	fOutputCapacity(0),
	fPictures(0),
	fSinceKey(0),
	fIdrId(0),
	fReconstructed(0),
	fQp(26),
	fRateBalance(0),
	fEngineTime(0)
{
	for (int i = 0; i < 2; i++) {
		fInput[i] = NULL;
		fLuma[i] = NULL;
		fChroma[i] = NULL;
		fSubsampled[i] = NULL;
		fMotion[i] = NULL;
		fLines[i] = NULL;
	}
	fError[0] = '\0';
	fDescription[0] = '\0';
}


CedarEncoder::~CedarEncoder()
{
	Close();
}


bool
CedarEncoder::IsAvailable(char* reason, size_t reasonSize)
{
	int device = open(SUNXI_VE_DEVICE_PATH, O_RDWR);
	if (device < 0) {
		if (reason != NULL)
			snprintf(reason, reasonSize, "There is no %s.",
				SUNXI_VE_DEVICE_PATH);
		return false;
	}
	// an empty picture asks whether there is an encoder; a driver from
	// before the encoder says "invalid ioctl"
	sunxi_ve_encode request;
	memset(&request, 0, sizeof(request));
	bool available = ioctl(device, SUNXI_VE_ENCODE, &request,
		sizeof(request)) == 0;
	if (!available && reason != NULL) {
		snprintf(reason, reasonSize, "The video engine driver has no "
			"encoder (%s).", strerror(errno));
	}
	close(device);
	return available;
}


void
CedarEncoder::_SetError(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	vsnprintf(fError, sizeof(fError), format, args);
	va_end(args);
}


status_t
CedarEncoder::_Allocate(Buffer& buffer, size_t size)
{
	sunxi_ve_allocate request = { size, 0, -1, 0 };
	if (ioctl(fDevice, SUNXI_VE_ALLOCATE, &request, sizeof(request)) != 0) {
		_SetError("No memory for the video engine (%s).", strerror(errno));
		return B_NO_MEMORY;
	}
	void* address;
	area_id clone = clone_area("cedar encoder buffer", &address,
		B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, request.area);
	if (clone < 0) {
		ioctl(fDevice, SUNXI_VE_FREE, &request.buffer, sizeof(uint32));
		_SetError("No memory for the video engine (%s).", strerror(clone));
		return clone;
	}
	buffer.index = request.buffer;
	buffer.address = request.address;
	buffer.area = clone;
	buffer.data = (uint8*)address;
	buffer.size = request.size;
	// The engine starts from zeros in its own buffers. The mapping is
	// cached: the zeros must reach memory now, or the cache writes them
	// back later, over what the engine wrote.
	memset(buffer.data, 0, buffer.size);
	_Sync(buffer, SUNXI_VE_SYNC_FOR_DEVICE);
	return B_OK;
}


void
CedarEncoder::_Free(Buffer& buffer)
{
	if (buffer.area >= 0)
		delete_area(buffer.area);
	if (fDevice >= 0)
		ioctl(fDevice, SUNXI_VE_FREE, &buffer.index, sizeof(uint32));
	buffer.area = -1;
}


void
CedarEncoder::_Sync(Buffer& buffer, uint32 direction, size_t size)
{
	sunxi_ve_sync request = { buffer.index, 0,
		(uint32)(size != 0 && size < buffer.size ? size : buffer.size),
		direction };
	ioctl(fDevice, SUNXI_VE_SYNC, &request, sizeof(request));
}


status_t
CedarEncoder::Open(const Params& params)
{
	Close();
	fParams = params;
	fError[0] = '\0';

	// P pictures smaller than this never end (H.264 from 160x32, H.265
	// from 176x64 were seen to work)
	if (params.width < kMinWidth || params.height < kMinHeight
		|| params.width > 4096 || params.height > 4096
		|| (params.width & 1) != 0 || (params.height & 1) != 0) {
		_SetError("The encoder takes even picture sizes from %" B_PRIu32 "x%"
			B_PRIu32 " to 4096x4096, not %" B_PRIu32 "x%" B_PRIu32 ".",
			kMinWidth, kMinHeight, params.width, params.height);
		return B_BAD_VALUE;
	}
	if (fParams.frameRateNum == 0 || fParams.frameRateDen == 0) {
		fParams.frameRateNum = 30;
		fParams.frameRateDen = 1;
	}
	if (fParams.keyInterval == 0 || fParams.keyInterval > 65535)
		fParams.keyInterval = 65535;
	fQp = fParams.qp < kMinQp ? kMinQp : fParams.qp > kMaxQp ? kMaxQp
		: fParams.qp;

	fDevice = open(SUNXI_VE_DEVICE_PATH, O_RDWR | O_CLOEXEC);
	if (fDevice < 0) {
		_SetError("There is no video engine (%s).", strerror(errno));
		return errno;
	}

	fWidth16 = ALIGN(params.width, 16);
	fHeight16 = ALIGN(params.height, 16);
	fStride = fWidth16;
	uint32 codec = _IsHevc() ? SUNXI_VE_ENCODE_HEVC : SUNXI_VE_ENCODE_H264;
	sunxi_ve_encoder_sizes sizes;
	sunxi_ve_get_encoder_sizes(codec, params.width, params.height, &sizes);

	size_t pixels = (size_t)fWidth16 * fHeight16;
	size_t bitstream = ALIGN(pixels * 3 / 2, 1 << 20);
	if (bitstream < kMinBitstream)
		bitstream = kMinBitstream;

	struct {
		Buffer**	buffer;
		size_t		size;
	} wanted[] = {
		{ &fInput[0], (size_t)fStride * fHeight16 },
		{ &fInput[1], (size_t)fStride * fHeight16 / 2 },
		{ &fBitstream, bitstream },
		{ &fLuma[0], _IsHevc() ? sizes.luma : sizes.luma + sizes.chroma },
		{ &fLuma[1], _IsHevc() ? sizes.luma : sizes.luma + sizes.chroma },
		{ &fChroma[0], _IsHevc() ? sizes.chroma : 0 },
		{ &fChroma[1], _IsHevc() ? sizes.chroma : 0 },
		{ &fSubsampled[0], sizes.subsampled },
		{ &fSubsampled[1], sizes.subsampled },
		{ &fMotion[0], sizes.mb_info },
		{ &fMotion[1], _IsHevc() ? sizes.mb_info : 0 },
		{ &fBlockInfo, sizes.block_info },
		{ &fColocated, sizes.colocated },
		{ &fStatistics, sizes.statistics },
		{ &fRowTable, sizes.row_table },
		{ &fLines[0], sizes.lines },
		{ &fLines[1], sizes.lines }
	};
	for (size_t i = 0; i < sizeof(wanted) / sizeof(wanted[0]); i++) {
		if (wanted[i].size == 0)
			continue;
		Buffer* buffer = new(std::nothrow) Buffer;
		if (buffer == NULL) {
			Close();
			return B_NO_MEMORY;
		}
		buffer->area = -1;
		*wanted[i].buffer = buffer;
		status_t status = _Allocate(*buffer, ALIGN(wanted[i].size, 4096));
		if (status != B_OK) {
			char error[sizeof(fError)];
			strlcpy(error, fError, sizeof(error));
			Close();
			strlcpy(fError, error, sizeof(fError));
			return status;
		}
	}
	if (!_IsHevc()) {
		// H.264 has the chroma after the luma, in the same buffer
		fChroma[0] = fLuma[0];
		fChroma[1] = fLuma[1];
		fChromaOffset = sizes.luma;
	} else
		fChromaOffset = 0;

	fHeaders = (uint8*)malloc(1024);
	fOutputCapacity = 1024 + fBitstream->size;
	fOutput = (uint8*)malloc(fOutputCapacity);
	if (fHeaders == NULL || fOutput == NULL) {
		Close();
		return B_NO_MEMORY;
	}
	_WriteHeaders();

	fPictures = 0;
	fSinceKey = 0;
	fIdrId = 0;
	fReconstructed = 0;
	fRateBalance = 0;
	const char* codecName = _IsHevc() ? "H.265" : "H.264";
	if (fParams.bitrate != 0) {
		snprintf(fDescription, sizeof(fDescription), "%s, %" B_PRIu32
			" kbit/s, IDR every %" B_PRIu32, codecName,
			fParams.bitrate / 1000, fParams.keyInterval);
	} else {
		snprintf(fDescription, sizeof(fDescription), "%s, constant QP %"
			B_PRIu32 ", IDR every %" B_PRIu32, codecName, fQp,
			fParams.keyInterval);
	}
	return B_OK;
}


void
CedarEncoder::Close()
{
	Buffer** buffers[] = { &fInput[0], &fInput[1], &fBitstream, &fLuma[0],
		&fLuma[1], &fSubsampled[0], &fSubsampled[1], &fMotion[0],
		&fMotion[1], &fBlockInfo, &fColocated, &fStatistics, &fRowTable,
		&fLines[0], &fLines[1] };
	if (_IsHevc()) {
		for (int i = 0; i < 2; i++) {
			if (fChroma[i] != NULL) {
				_Free(*fChroma[i]);
				delete fChroma[i];
			}
		}
	}
	fChroma[0] = fChroma[1] = NULL;
	for (size_t i = 0; i < sizeof(buffers) / sizeof(buffers[0]); i++) {
		if (*buffers[i] == NULL)
			continue;
		_Free(**buffers[i]);
		delete *buffers[i];
		*buffers[i] = NULL;
	}
	if (fDevice >= 0)
		close(fDevice);
	fDevice = -1;
	free(fHeaders);
	fHeaders = NULL;
	fHeadersSize = 0;
	free(fOutput);
	fOutput = NULL;
	fOutputCapacity = 0;
}


static void
put_hevc_profile_tier_level(BitBuffer& b, uint8 level)
{
	b.Put(0, 2);			// general_profile_space
	b.Put(0, 1);			// general_tier_flag
	b.Put(1, 5);			// general_profile_idc: Main
	b.Put(0x60000000, 32);	// compatible with Main and Main 10
	b.Put(1, 1);			// progressive_source
	b.Put(0, 1);			// interlaced_source
	b.Put(0, 1);			// non_packed_constraint
	b.Put(1, 1);			// frame_only_constraint
	b.Put(0, 32);			// 43 reserved bits and general_inbld_flag
	b.Put(0, 12);
	b.Put(level, 8);
}


void
CedarEncoder::_WriteHeaders()
{
	double rate = (double)fParams.frameRateNum / fParams.frameRateDen;
	bool sar = fParams.sarNum != 0 && fParams.sarDen != 0;
	bool colour = fParams.colourPrimaries != 2 || fParams.transfer != 2
		|| fParams.matrix != 2;
	bool signal = colour || fParams.fullRange;

	if (!_IsHevc()) {
		uint32 mbWidth = fWidth16 / 16, mbHeight = fHeight16 / 16;
		uint32 mbs = mbWidth * mbHeight;
		uint8 level = 52;
		for (size_t i = 0; i < sizeof(kH264Levels) / sizeof(kH264Levels[0]);
				i++) {
			if (mbs <= kH264Levels[i].maxFrameMbs
				&& mbs * rate <= kH264Levels[i].maxMbsPerSecond) {
				level = kH264Levels[i].idc;
				break;
			}
		}

		// High: the engine uses 8x8 transforms (Allwinner's encoder says
		// High too)
		BitBuffer sps;
		sps.Put(0x67, 8);
		sps.Put(100, 8);
		sps.Put(0, 8);			// constraint flags
		sps.Put(level, 8);
		sps.Ue(0);				// seq_parameter_set_id
		sps.Ue(1);				// chroma_format_idc: 4:2:0
		sps.Ue(0);				// bit_depth_luma_minus8
		sps.Ue(0);				// bit_depth_chroma_minus8
		sps.Put(0, 1);			// qpprime_y_zero_transform_bypass_flag
		sps.Put(0, 1);			// seq_scaling_matrix_present_flag
		sps.Ue(4);				// log2_max_frame_num_minus4: 8 bits
		sps.Ue(2);				// pic_order_cnt_type: output = decode order
		sps.Ue(1);				// max_num_ref_frames
		sps.Put(0, 1);			// gaps_in_frame_num_value_allowed_flag
		sps.Ue(mbWidth - 1);
		sps.Ue(mbHeight - 1);
		sps.Put(1, 1);			// frame_mbs_only_flag
		sps.Put(1, 1);			// direct_8x8_inference_flag
		uint32 cropRight = (fWidth16 - fParams.width) / 2;
		uint32 cropBottom = (fHeight16 - fParams.height) / 2;
		bool crop = cropRight != 0 || cropBottom != 0;
		sps.Put(crop, 1);
		if (crop) {
			sps.Ue(0);
			sps.Ue(cropRight);
			sps.Ue(0);
			sps.Ue(cropBottom);
		}
		sps.Put(1, 1);			// vui_parameters_present_flag
		sps.Put(sar, 1);
		if (sar) {
			sps.Put(255, 8);	// Extended_SAR
			sps.Put(fParams.sarNum, 16);
			sps.Put(fParams.sarDen, 16);
		}
		sps.Put(0, 1);			// overscan_info_present_flag
		sps.Put(signal, 1);
		if (signal) {
			sps.Put(5, 3);		// video_format: unspecified
			sps.Put(fParams.fullRange, 1);
			sps.Put(colour, 1);
			if (colour) {
				sps.Put(fParams.colourPrimaries, 8);
				sps.Put(fParams.transfer, 8);
				sps.Put(fParams.matrix, 8);
			}
		}
		sps.Put(0, 1);			// chroma_loc_info_present_flag
		sps.Put(1, 1);			// timing_info_present_flag
		sps.Put(fParams.frameRateDen, 32);		// num_units_in_tick
		sps.Put(fParams.frameRateNum * 2, 32);	// time_scale
		sps.Put(0, 1);			// fixed_frame_rate_flag
		sps.Put(0, 1);			// nal_hrd_parameters_present_flag
		sps.Put(0, 1);			// vcl_hrd_parameters_present_flag
		sps.Put(0, 1);			// pic_struct_present_flag
		sps.Put(1, 1);			// bitstream_restriction_flag
		sps.Put(1, 1);			// motion_vectors_over_pic_boundaries_flag
		sps.Ue(0);				// max_bytes_per_pic_denom
		sps.Ue(0);				// max_bits_per_mb_denom
		sps.Ue(16);				// log2_max_mv_length_horizontal
		sps.Ue(16);				// log2_max_mv_length_vertical
		sps.Ue(0);				// max_num_reorder_frames
		sps.Ue(1);				// max_dec_frame_buffering
		sps.Trailing();

		BitBuffer pps;
		pps.Put(0x68, 8);
		pps.Ue(0);				// pic_parameter_set_id
		pps.Ue(0);				// seq_parameter_set_id
		pps.Put(1, 1);			// entropy_coding_mode_flag: CABAC
		pps.Put(0, 1);			// bottom_field_pic_order_in_frame_present
		pps.Ue(0);				// num_slice_groups_minus1
		pps.Ue(0);				// num_ref_idx_l0_default_active_minus1
		pps.Ue(0);				// num_ref_idx_l1_default_active_minus1
		pps.Put(0, 1);			// weighted_pred_flag
		pps.Put(0, 2);			// weighted_bipred_idc
		pps.Se(0);				// pic_init_qp_minus26 (slices say their QP)
		pps.Se(0);				// pic_init_qs_minus26
		pps.Se(0);				// chroma_qp_index_offset
		pps.Put(1, 1);			// deblocking_filter_control_present_flag
		pps.Put(0, 1);			// constrained_intra_pred_flag
		pps.Put(0, 1);			// redundant_pic_cnt_present_flag
		pps.Put(1, 1);			// transform_8x8_mode_flag
		pps.Put(0, 1);			// pic_scaling_matrix_present_flag
		pps.Se(0);				// second_chroma_qp_index_offset
		pps.Trailing();

		fHeadersSize = append_nal(fHeaders, sps);
		fHeadersSize += append_nal(fHeaders + fHeadersSize, pps);
		return;
	}

	uint64 samples = (uint64)fParams.width * fParams.height;
	uint8 level = 186;
	for (size_t i = 0; i < sizeof(kHevcLevels) / sizeof(kHevcLevels[0]);
			i++) {
		if (samples <= kHevcLevels[i].maxLumaPictureSize
			&& samples * rate <= kHevcLevels[i].maxLumaSampleRate) {
			level = kHevcLevels[i].idc;
			break;
		}
	}

	BitBuffer vps;
	vps.Put(0x4001, 16);
	vps.Put(0, 4);				// vps_video_parameter_set_id
	vps.Put(1, 1);				// vps_base_layer_internal_flag
	vps.Put(1, 1);				// vps_base_layer_available_flag
	vps.Put(0, 6);				// vps_max_layers_minus1
	vps.Put(0, 3);				// vps_max_sub_layers_minus1
	vps.Put(1, 1);				// vps_temporal_id_nesting_flag
	vps.Put(0xffff, 16);
	put_hevc_profile_tier_level(vps, level);
	vps.Put(1, 1);				// vps_sub_layer_ordering_info_present_flag
	vps.Ue(1);					// vps_max_dec_pic_buffering_minus1
	vps.Ue(0);					// vps_max_num_reorder_pics
	vps.Ue(0);					// vps_max_latency_increase_plus1
	vps.Put(0, 6);				// vps_max_layer_id
	vps.Ue(0);					// vps_num_layer_sets_minus1
	vps.Put(0, 1);				// vps_timing_info_present_flag
	vps.Put(0, 1);				// vps_extension_flag
	vps.Trailing();

	// the engine's coding tools: CTB 32, CB 8, TB 4 to 32 of depth 1, SAO,
	// temporal motion vectors, strong intra smoothing, no AMP
	BitBuffer sps;
	sps.Put(0x4201, 16);
	sps.Put(0, 4);				// sps_video_parameter_set_id
	sps.Put(0, 3);				// sps_max_sub_layers_minus1
	sps.Put(1, 1);				// sps_temporal_id_nesting_flag
	put_hevc_profile_tier_level(sps, level);
	sps.Ue(0);					// sps_seq_parameter_set_id
	sps.Ue(1);					// chroma_format_idc: 4:2:0
	uint32 width8 = ALIGN(fParams.width, 8), height8 = ALIGN(fParams.height, 8);
	sps.Ue(width8);
	sps.Ue(height8);
	bool crop = width8 != fParams.width || height8 != fParams.height;
	sps.Put(crop, 1);
	if (crop) {
		sps.Ue(0);
		sps.Ue((width8 - fParams.width) / 2);
		sps.Ue(0);
		sps.Ue((height8 - fParams.height) / 2);
	}
	sps.Ue(0);					// bit_depth_luma_minus8
	sps.Ue(0);					// bit_depth_chroma_minus8
	sps.Ue(4);					// log2_max_pic_order_cnt_lsb_minus4: 8
	sps.Put(1, 1);				// sps_sub_layer_ordering_info_present_flag
	sps.Ue(1);					// sps_max_dec_pic_buffering_minus1
	sps.Ue(0);					// sps_max_num_reorder_pics
	sps.Ue(0);					// sps_max_latency_increase_plus1
	sps.Ue(0);					// log2_min_luma_coding_block_size_minus3
	sps.Ue(2);					// log2_diff_max_min_luma_coding_block_size
	sps.Ue(0);					// log2_min_luma_transform_block_size_minus2
	sps.Ue(3);					// log2_diff_max_min_luma_transform_block_size
	sps.Ue(1);					// max_transform_hierarchy_depth_inter
	sps.Ue(1);					// max_transform_hierarchy_depth_intra
	sps.Put(0, 1);				// scaling_list_enabled_flag
	sps.Put(0, 1);				// amp_enabled_flag
	sps.Put(1, 1);				// sample_adaptive_offset_enabled_flag
	sps.Put(0, 1);				// pcm_enabled_flag
	sps.Ue(1);					// num_short_term_ref_pic_sets
	sps.Ue(1);					// num_negative_pics: the picture before
	sps.Ue(0);					// num_positive_pics
	sps.Ue(0);					// delta_poc_s0_minus1
	sps.Put(1, 1);				// used_by_curr_pic_s0_flag
	sps.Put(0, 1);				// long_term_ref_pics_present_flag
	sps.Put(1, 1);				// sps_temporal_mvp_enabled_flag
	sps.Put(1, 1);				// strong_intra_smoothing_enabled_flag
	sps.Put(1, 1);				// vui_parameters_present_flag
	sps.Put(sar, 1);
	if (sar) {
		sps.Put(255, 8);		// EXTENDED_SAR
		sps.Put(fParams.sarNum, 16);
		sps.Put(fParams.sarDen, 16);
	}
	sps.Put(0, 1);				// overscan_info_present_flag
	sps.Put(signal, 1);
	if (signal) {
		sps.Put(5, 3);			// video_format: unspecified
		sps.Put(fParams.fullRange, 1);
		sps.Put(colour, 1);
		if (colour) {
			sps.Put(fParams.colourPrimaries, 8);
			sps.Put(fParams.transfer, 8);
			sps.Put(fParams.matrix, 8);
		}
	}
	sps.Put(0, 1);				// chroma_loc_info_present_flag
	sps.Put(0, 1);				// neutral_chroma_indication_flag
	sps.Put(0, 1);				// field_seq_flag
	sps.Put(0, 1);				// frame_field_info_present_flag
	sps.Put(0, 1);				// default_display_window_flag
	sps.Put(1, 1);				// vui_timing_info_present_flag
	sps.Put(fParams.frameRateDen, 32);	// vui_num_units_in_tick
	sps.Put(fParams.frameRateNum, 32);	// vui_time_scale
	sps.Put(0, 1);				// vui_poc_proportional_to_timing_flag
	sps.Put(0, 1);				// vui_hrd_parameters_present_flag
	sps.Put(0, 1);				// bitstream_restriction_flag
	sps.Put(0, 1);				// sps_extension_present_flag
	sps.Trailing();

	BitBuffer pps;
	pps.Put(0x4401, 16);
	pps.Ue(0);					// pps_pic_parameter_set_id
	pps.Ue(0);					// pps_seq_parameter_set_id
	pps.Put(0, 1);				// dependent_slice_segments_enabled_flag
	pps.Put(0, 1);				// output_flag_present_flag
	pps.Put(0, 3);				// num_extra_slice_header_bits
	pps.Put(0, 1);				// sign_data_hiding_enabled_flag
	pps.Put(1, 1);				// cabac_init_present_flag
	pps.Ue(0);					// num_ref_idx_l0_default_active_minus1
	pps.Ue(0);					// num_ref_idx_l1_default_active_minus1
	pps.Se(0);					// init_qp_minus26 (slices say their QP)
	pps.Put(0, 1);				// constrained_intra_pred_flag
	pps.Put(0, 1);				// transform_skip_enabled_flag
	pps.Put(1, 1);				// cu_qp_delta_enabled_flag: the engine
	pps.Ue(1);					// varies the QP, diff_cu_qp_delta_depth
	pps.Se(kHevcChromaQpOffset);	// pps_cb_qp_offset
	pps.Se(kHevcChromaQpOffset);	// pps_cr_qp_offset
	pps.Put(0, 1);				// pps_slice_chroma_qp_offsets_present_flag
	pps.Put(0, 1);				// weighted_pred_flag
	pps.Put(0, 1);				// weighted_bipred_flag
	pps.Put(0, 1);				// transquant_bypass_enabled_flag
	pps.Put(0, 1);				// tiles_enabled_flag
	pps.Put(0, 1);				// entropy_coding_sync_enabled_flag
	pps.Put(1, 1);				// loop_filter_across_slices_enabled_flag
	pps.Put(1, 1);				// deblocking_filter_control_present_flag
	pps.Put(0, 1);				// deblocking_filter_override_enabled_flag
	pps.Put(0, 1);				// pps_deblocking_filter_disabled_flag
	pps.Se(0);					// pps_beta_offset_div2
	pps.Se(0);					// pps_tc_offset_div2
	pps.Put(0, 1);				// pps_scaling_list_data_present_flag
	pps.Put(0, 1);				// lists_modification_present_flag
	pps.Ue(0);					// log2_parallel_merge_level_minus2
	pps.Put(0, 1);				// slice_segment_header_extension_present
	pps.Put(0, 1);				// pps_extension_present_flag
	pps.Trailing();

	fHeadersSize = append_nal(fHeaders, vps);
	fHeadersSize += append_nal(fHeaders + fHeadersSize, sps);
	fHeadersSize += append_nal(fHeaders + fHeadersSize, pps);
}


const uint8*
CedarEncoder::Headers(size_t* size) const
{
	*size = fHeadersSize;
	return fHeaders;
}


/*!	The slice's NAL: start code, NAL header and slice header, ending on a
	byte (H.264: with CABAC's alignment ones; H.265: byte_alignment()), as
	the engine's bit writer takes it.
*/
size_t
CedarEncoder::_SliceHeader(uint8* out, bool idr, uint32 qp)
{
	BitBuffer b;
	if (!_IsHevc()) {
		b.Put(idr ? 0x65 : 0x41, 8);
		b.Ue(0);					// first_mb_in_slice
		b.Ue(idr ? 7 : 5);			// slice_type: I or P, all of them
		b.Ue(0);					// pic_parameter_set_id
		b.Put(fSinceKey & 0xff, 8);	// frame_num
		if (idr) {
			b.Ue(fIdrId & 0xffff);	// idr_pic_id
			b.Put(0, 1);			// no_output_of_prior_pics_flag
			b.Put(0, 1);			// long_term_reference_flag
		} else {
			b.Put(0, 1);			// num_ref_idx_active_override_flag
			b.Put(0, 1);			// ref_pic_list_modification_flag_l0
			b.Put(0, 1);			// adaptive_ref_pic_marking_mode_flag
			b.Ue(1);				// cabac_init_idc: the engine's
		}
		b.Se((int32)qp - 26);		// slice_qp_delta
		b.Ue(0);					// disable_deblocking_filter_idc
		b.Se(0);					// slice_alpha_c0_offset_div2
		b.Se(0);					// slice_beta_offset_div2
		while ((b.bits & 7) != 0)
			b.Put(1, 1);			// cabac_alignment_one_bit
	} else {
		b.Put(idr ? 0x2601 : 0x0201, 16);	// IDR_W_RADL or TRAIL_R
		b.Put(1, 1);				// first_slice_segment_in_pic_flag
		if (idr)
			b.Put(0, 1);			// no_output_of_prior_pics_flag
		b.Ue(0);					// slice_pic_parameter_set_id
		b.Ue(idr ? 2 : 1);			// slice_type: I or P
		if (!idr) {
			b.Put(fSinceKey & 0xff, 8);	// slice_pic_order_cnt_lsb
			b.Put(1, 1);			// short_term_ref_pic_set_sps_flag
			b.Put(1, 1);			// slice_temporal_mvp_enabled_flag
		}
		b.Put(1, 1);				// slice_sao_luma_flag
		b.Put(1, 1);				// slice_sao_chroma_flag
		if (!idr) {
			b.Put(0, 1);			// num_ref_idx_active_override_flag
			b.Put(0, 1);			// cabac_init_flag
			b.Ue(0);				// five_minus_max_num_merge_cand
		}
		b.Se((int32)qp - 26);		// slice_qp_delta
		b.Put(1, 1);				// slice_loop_filter_across_slices_...
		b.Trailing();				// byte_alignment()
	}
	return append_nal(out, b);
}


/*!	A picture's registers, as Allwinner's encoder sets them for it. */
void
CedarEncoder::_Program(Ops& ops, bool idr, uint32 qp)
{
	uint32 rec = fReconstructed;
	uint32 ref = idr ? rec : rec ^ 1;
	uint32 width8 = ALIGN(fParams.width, 8) / 8;
	uint32 height8 = ALIGN(fParams.height, 8) / 8;
	uint32 widthClass = (fWidth16 / 16 + 39) / 40;
	#define A8(buffer, offset) (((buffer)->address + (offset)) >> 8)

	// the input stage: NV12
	ops.Write(INPUT_SIZE, width8 << 16 | height8);
	ops.Write(INPUT_STRIDE, (fStride / 16) << 16);
	ops.Write(INPUT_CHROMA_STRIDE, fStride / 16);
	ops.Write(OUTPUT_SIZE, height8 << 16 | width8);
	ops.Write(INPUT_CONTROL, _IsHevc() ? 0 : idr ? 0x00020021 : 0x21);
	ops.Write(INPUT_LUMA, fInput[0]->address);
	ops.Write(INPUT_CHROMA, fInput[1]->address);
	ops.Write(INPUT_CHROMA2, fInput[1]->address + fStride * fHeight16 / 4);
	ops.Write(0xa70, 0);
	ops.Write(0xa74, 0);
	ops.Write(0xa84, 0);
	ops.Write(0xa88, 0);
	if (!_IsHevc()) {
		ops.Write(0xa90, 0x03ff03ff);
		ops.Write(0xa94, 0);
		uint32 statistics = fParams.width * 3 / 40;
		ops.Write(0xa98, (statistics > 128 ? 128 : statistics) << 16);
		ops.Write(INPUT_STATISTICS, fStatistics->address);
	}

	// motion search thresholds, per block column
	uint32 blocks = _IsHevc() ? (fParams.width + 31) / 32 : fWidth16 / 16;
	uint32 motion48 = (blocks * 3 / 5) << 16 | (blocks / 10);
	uint32 motion4c = (blocks * 16 / 5) << 16 | (blocks * 13 / 5);
	// texture thresholds
	static const uint32 kIntraMad[] = { 0x0a080604, 0x12100e0c, 0x1a181614,
		0x22201e1c };
	static const uint32 kInterMad[] = { 0x1d1c1b1a, 0x21201f1e, 0x25242322,
		0x29282726 };
	const uint32* mad = idr ? kIntraMad : kInterMad;
	ops.Write(0xb30, mad[0]);
	ops.Write(0xb34, mad[1]);
	ops.Write(0xb38, mad[2]);
	ops.Write(0xb74, mad[3]);
	ops.Write(0xc10, 0x01020304);
	ops.Write(0xc14, 0x05060708);
	ops.Write(0xc18, 0x08070605);
	ops.Write(0xc1c, 0x04030201);
	ops.Write(0xc20, 0x00003300);
	ops.Write(0xb28, 0);

	if (!_IsHevc()) {
		ops.Write(0xb70, idr ? 0 : 0xff000000);
		ops.Write(0xb44, 0x0c1e0000);
		bool dynamicMe = !idr && fWidth16 / 16 >= kMinDynamicMeBlocks;
		ops.Write(QP, qp | widthClass << 10 | (dynamicMe ? 0x100 : 0));
		ops.Write(0xb48, motion48);
		ops.Write(0xb4c, motion4c);
		ops.Write(0xb40, 0x74000014);
		// no per block QP: Allwinner's (b31) costs 1.5 dB at the same size
		ops.Write(RATE_CONTROL, 0);
		ops.Write(0xb0c, 0);
		ops.Write(0xb24, 0);
		ops.Write(TOOLS, 0x08001200);
		ops.Write(PICTURE, idr ? 0x100 : 0x510);
		ops.Write(MB_INFO, A8(fMotion[0], 0));
	} else {
		// the rate-distortion lambdas, as Allwinner's encoder sets them
		// for its QP
		double lambda = idr ? 735.0 * pow(2.0, ((int)qp - 28) / 3.0)
			: 3230.0 * pow(2.0, ((int)qp - 30) / 3.0);
		uint32 lambda68 = (uint32)(lambda + 0.5);
		uint32 lambdaRoot = idr ? 0x20 : (uint32)(2 * sqrt(lambda));
		if (lambdaRoot > 0xff)
			lambdaRoot = 0xff;

		ops.Write(PICTURE, idr ? 0x100 : 0x110);
		ops.Write(QP, (uint32)(kHevcChromaQpOffset & 0x1f) << 16 | qp
			| widthClass << 10 | (idr ? 0 : 0x100));
		ops.Write(SAO, 0x300);
		ops.Write(TOOLS, idr ? 0x08c00000
			: fSinceKey == 1 ? 0x08400000 : 0x08000000);
		// b28: the engine writes cu_qp_delta (the PPS says it may); no
		// per block QP (b31), which costs 4 dB at the same size
		ops.Write(RATE_CONTROL, 0x10000000);
		ops.Write(0xb40, 0x18000014);
		ops.Write(0xb48, idr ? 0 : motion48);
		ops.Write(0xb4c, idr ? 0 : motion4c);
		ops.Write(0xb68, lambda68);
		ops.Write(0xb6c, 0x3cc80000 | lambdaRoot);
		ops.Write(0xb70, 0xff000000 | (uint32)(lambda68 * 0.397 + 0.5));
		ops.Write(0xb94, lambdaRoot);
		ops.Write(0xc84, lambda68);
		static const uint16 kZero[] = { 0xbcc, 0xb98, 0xbd0, 0xbd4, 0xbd8,
			0xbdc, 0xbe8, 0xbec, 0xbf0, 0xbf4, 0xbf8, 0xbfc, 0xcf8, 0xcfc,
			0xcc4 };
		for (size_t i = 0; i < sizeof(kZero) / sizeof(kZero[0]); i++)
			ops.Write(kZero[i], 0);
		static const uint32 kTables[][2] = {
			{ 0xc04, 0x00030405 }, { 0xc08, 0x000a0a0a },
			{ 0xce4, 0x42104210 }, { 0xce8, 0x08880888 },
			{ 0xcec, 0x00101010 }, { 0xcf0, 0x00101010 },
			{ 0xcf4, 0x00101010 }, { 0xc24, 0x00010a1e },
			{ 0xc28, 0x00040810 }, { 0xc2c, 0x00030303 },
			{ 0xc30, 0x00030303 }, { 0xc34, 0x001d1e1f },
			{ 0xc38, 0x00030303 }, { 0xc3c, 0x00200020 },
			{ 0xc40, 0x81110108 }, { 0xc44, 0x01060103 },
			{ 0xc48, 0x04032020 }, { 0xb44, 0x0c1e0084 },
			{ 0xc4c, 0x0303031f }, { 0xc50, 0x801e1e1e },
			{ 0xc54, 0x001e1e1e }, { 0xc58, 0x00060504 },
			{ 0xc5c, 0x0001000d }, { 0xc60, 0x000b0009 },
			{ 0xc64, 0x00020408 }, { 0xc68, 0x03050b11 },
			{ 0xc6c, 0x0204070a }, { 0xc70, 0x00000503 },
			{ 0xc74, 0x10008621 }, { 0xc78, 0x24281427 },
			{ 0xc7c, 0x1c1a101a }, { 0xc80, 0x14180c0c }
		};
		for (size_t i = 0; i < sizeof(kTables) / sizeof(kTables[0]); i++)
			ops.Write(kTables[i][0], kTables[i][1]);
		ops.Write(COLOCATED, idr ? 0 : A8(fColocated, 0));
		ops.Write(MB_INFO, A8(fMotion[ref], 0));
		ops.Write(MOTION_OUT, A8(fMotion[rec], 0));
		ops.Write(BLOCK_INFO, A8(fBlockInfo, 0));
	}

	// the bit stream, the pictures, the engine's own buffers
	ops.Write(BITSTREAM_OFFSET, 0);
	ops.Write(BITSTREAM, A8(fBitstream, 0));
	ops.Write(BITSTREAM_END, A8(fBitstream, fBitstream->size - 1));
	ops.Write(BITSTREAM_LIMIT, (uint32)(fBitstream->size * 8));
	ops.Write(REF_LUMA, A8(fLuma[ref], 0));
	ops.Write(REF_CHROMA, A8(fChroma[ref], fChromaOffset));
	ops.Write(REC_LUMA, A8(fLuma[rec], 0));
	ops.Write(REC_CHROMA, A8(fChroma[rec], fChromaOffset));
	ops.Write(REF_SUBSAMPLED, A8(fSubsampled[ref], 0));
	ops.Write(REC_SUBSAMPLED, A8(fSubsampled[rec], 0));
	ops.Write(ROW_TABLE, A8(fRowTable, 0));
	ops.Write(LINES0, A8(fLines[0], 0));
	ops.Write(LINES1, A8(fLines[1], 0));
	#undef A8
}


uint32
CedarEncoder::_NextQp(uint32 bits)
{
	if (fParams.bitrate == 0)
		return fQp;
	// a picture's share of the bit rate; what was over or under it is
	// spread over the next second
	double rate = (double)fParams.frameRateNum / fParams.frameRateDen;
	double target = fParams.bitrate / rate;
	fRateBalance += bits - target;
	double second = fParams.bitrate;
	if (fRateBalance > second)
		fRateBalance = second;
	if (fRateBalance < -second)
		fRateBalance = -second;
	double want = target - fRateBalance / rate;
	if (want < target / 4)
		want = target / 4;
	// six QP steps halve the size
	double change = 6 * log2(bits / want) / 3;
	int32 qp = (int32)fQp + (change > 2 ? 2 : change < -2 ? -2
		: (int32)lround(change));
	return qp < (int32)kMinQp ? kMinQp : qp > (int32)kMaxQp ? kMaxQp : qp;
}


status_t
CedarEncoder::Encode(const uint8* luma, uint32 lumaStride,
	const uint8* chroma, uint32 chromaStride, bool forceKey,
	const uint8** _data, size_t* _size, bool* _key)
{
	if (fDevice < 0) {
		_SetError("The encoder is not open.");
		return B_NO_INIT;
	}

	// into the engine's layout: whole macroblocks, the last line and
	// column repeated into the padding
	uint32 width = fParams.width, height = fParams.height;
	uint8* y = fInput[0]->data;
	uint8* uv = fInput[1]->data;
	for (uint32 line = 0; line < fHeight16; line++) {
		uint32 from = line < height ? line : height - 1;
		uint8* to = y + (size_t)line * fStride;
		memcpy(to, luma + (size_t)from * lumaStride, width);
		memset(to + width, to[width - 1], fStride - width);
	}
	for (uint32 line = 0; line < fHeight16 / 2; line++) {
		uint32 from = line < height / 2 ? line : height / 2 - 1;
		uint8* to = uv + (size_t)line * fStride;
		memcpy(to, chroma + (size_t)from * chromaStride, width);
		for (uint32 x = width; x < fStride; x += 2) {
			to[x] = to[width - 2];
			to[x + 1] = to[width - 1];
		}
	}
	_Sync(*fInput[0], SUNXI_VE_SYNC_FOR_DEVICE);
	_Sync(*fInput[1], SUNXI_VE_SYNC_FOR_DEVICE);

	bool idr = forceKey || fPictures == 0
		|| fSinceKey >= fParams.keyInterval;
	if (idr) {
		if (fPictures != 0)
			fIdrId++;
		fSinceKey = 0;
	}

	Ops* ops = new(std::nothrow) Ops;
	if (ops == NULL)
		return B_NO_MEMORY;
	uint32 qp = fQp;
	_Program(*ops, idr, qp);
	uint8 header[SUNXI_VE_MAX_HEADER];
	size_t headerSize = _SliceHeader(header, idr, qp);

	sunxi_ve_encode request;
	memset(&request, 0, sizeof(request));
	request.ops = ops->ops;
	request.count = ops->count;
	request.codec = _IsHevc() ? SUNXI_VE_ENCODE_HEVC : SUNXI_VE_ENCODE_H264;
	request.header = header;
	request.header_size = headerSize;
	status_t status = B_OK;
	if (ops->overflow) {
		_SetError("Too many register writes.");
		status = B_ERROR;
	} else if (ioctl(fDevice, SUNXI_VE_ENCODE, &request, sizeof(request))
			!= 0) {
		status = errno;
		_SetError("The video engine did not encode the picture (%s).",
			strerror(errno));
	} else if (request.status != SUNXI_VE_ENCODE_DONE) {
		status = B_ERROR;
		_SetError("The video engine could not encode the picture (status %#"
			B_PRIx32 "%s).", request.status,
			(request.status & SUNXI_VE_ENCODE_FULL) != 0
				? ", its buffer full" : "");
	}
	delete ops;
	fEngineTime = request.engine_time;
	if (status != B_OK)
		return status;

	size_t bytes = (request.bits + 7) / 8;
	if (bytes > fBitstream->size || bytes < headerSize) {
		_SetError("The picture did not fit its buffer.");
		return B_ERROR;
	}
	_Sync(*fBitstream, SUNXI_VE_SYNC_FOR_CPU, bytes);
	size_t size = 0;
	if (idr) {
		memcpy(fOutput, fHeaders, fHeadersSize);
		size = fHeadersSize;
	}
	memcpy(fOutput + size, fBitstream->data, bytes);
	size += bytes;

	fQp = _NextQp(request.bits);
	fPictures++;
	fSinceKey++;
	fReconstructed ^= 1;
	*_data = fOutput;
	*_size = size;
	*_key = idr;
	return B_OK;
}
