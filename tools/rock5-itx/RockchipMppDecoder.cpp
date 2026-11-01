/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */

#include <DecoderPlugin.h>
#include <MediaFormats.h>

#include <algorithm>
#include <new>
#include <vector>

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <rk_mpi.h>
}

#if defined(__aarch64__)
#include <arm_neon.h>
#endif


// Not among Haiku's colour spaces; the same codes as airTime's and the NVDEC
// add-on's. Both are a plane of luma followed by one of Cb and Cr in pairs,
// with bytes_per_row between the rows of each.
static const color_space kColorSpaceNV12 = (color_space)0x4e563132;	// 'NV12'
static const color_space kColorSpaceP010 = (color_space)0x50303130;	// 'P010'


/*!	Unpacks a row of the decoder's ten-bit samples (four in five bytes, the
	least significant bits first, as Linux's NV15) into P010's sixteen-bit
	ones, which hold the value in their top ten bits. */
static void
unpack_ten_bit_row(const uint8_t* in, uint16_t* out, int samples)
{
	int x = 0;
#if defined(__aarch64__)
	// Each sample is in the little-endian pair of bytes starting at byte
	// 10 * k / 8 of its group of four, 2 * (k % 4) bits up.
	static const uint8_t kGather[16] = {0, 1, 1, 2, 2, 3, 3, 4,
		5, 6, 6, 7, 7, 8, 8, 9};
	static const int16_t kShift[8] = {6, 4, 2, 0, 6, 4, 2, 0};
	const uint8x16_t gather = vld1q_u8(kGather);
	const int16x8_t shift = vld1q_s16(kShift);
	const uint16x8_t mask = vdupq_n_u16(0xffc0);
	// Sixteen bytes are read for every ten used: stop while that stays
	// within the row.
	for (; samples - x >= 16; x += 8, in += 10, out += 8) {
		uint16x8_t pairs = vreinterpretq_u16_u8(vqtbl1q_u8(vld1q_u8(in),
			gather));
		vst1q_u16(out, vandq_u16(vshlq_u16(pairs, shift), mask));
	}
#endif
	for (; x + 4 <= samples; x += 4, in += 5, out += 4) {
		out[0] = (uint16_t)((in[0] | (in[1] & 0x03) << 8) << 6);
		out[1] = (uint16_t)((in[1] >> 2 | (in[2] & 0x0f) << 6) << 6);
		out[2] = (uint16_t)((in[2] >> 4 | (in[3] & 0x3f) << 4) << 6);
		out[3] = (uint16_t)((in[3] >> 6 | in[4] << 2) << 6);
	}
	if (x < samples) {
		// A last, incomplete group.
		uint8_t group[5] = {};
		memcpy(group, in, (size_t)((samples - x) * 10 + 7) / 8);
		uint16_t values[4];
		unpack_ten_bit_row(group, values, 4);
		memcpy(out, values, (size_t)(samples - x) * sizeof(uint16_t));
	}
}


class RockchipMppDecoder : public Decoder {
public:
	RockchipMppDecoder() = default;

	~RockchipMppDecoder() override
	{
		_DropHeldFrame();
		if (fSws != NULL)
			sws_freeContext(fSws);
		if (fBsf != NULL)
			av_bsf_free(&fBsf);
		_DestroyContext();
	}

	void GetCodecInfo(media_codec_info* info) override
	{
		memset(info, 0, sizeof(*info));
		strlcpy(info->pretty_name, "Rockchip RK3588 hardware video decoder",
			sizeof(info->pretty_name));
		strlcpy(info->short_name, "rk3588_mpp", sizeof(info->short_name));
	}

	status_t Setup(media_format* format, const void* info, size_t infoSize) override
	{
		if (format == NULL || format->type != B_MEDIA_ENCODED_VIDEO)
			return B_NOT_SUPPORTED;
		media_format_description description = {};
		if (BMediaFormats().GetCodeFor(*format, B_MISC_FORMAT_FAMILY,
				&description) != B_OK
			|| description.u.misc.file_format != 'ffmp') {
			return B_NOT_SUPPORTED;
		}

		const char* filterName = NULL;
		switch (description.u.misc.codec) {
			case AV_CODEC_ID_H264:
				fCoding = MPP_VIDEO_CodingAVC;
				filterName = "h264_mp4toannexb";
				break;
			case AV_CODEC_ID_HEVC:
				fCoding = MPP_VIDEO_CodingHEVC;
				filterName = "hevc_mp4toannexb";
				break;
			case AV_CODEC_ID_AV1:
				fCoding = MPP_VIDEO_CodingAV1;
				break;
			case AV_CODEC_ID_VP9:
				// Profiles 0 and 2 on RKVDEC; MPP's parser splits superframes.
				fCoding = MPP_VIDEO_CodingVP9;
				break;
			default:
				return B_NOT_SUPPORTED;
		}
		fCodecId = (AVCodecID)description.u.misc.codec;
		fInputFormat = *format;
		fprintf(stderr, "RockchipMppDecoder: codec=%d extra=%zu\n",
			(int)fCodecId, infoSize);

		if (filterName != NULL) {
			const AVBitStreamFilter* filter = av_bsf_get_by_name(filterName);
			if (filter == NULL || av_bsf_alloc(filter, &fBsf) < 0)
				return B_ERROR;
			fBsf->par_in->codec_type = AVMEDIA_TYPE_VIDEO;
			fBsf->par_in->codec_id = fCodecId;
			fBsf->par_in->width = format->u.encoded_video.output.display.line_width;
			fBsf->par_in->height = format->u.encoded_video.output.display.line_count;
			if (info != NULL && infoSize > 0) {
				fBsf->par_in->extradata = (uint8_t*)av_mallocz(infoSize
					+ AV_INPUT_BUFFER_PADDING_SIZE);
				if (fBsf->par_in->extradata == NULL)
					return B_NO_MEMORY;
				memcpy(fBsf->par_in->extradata, info, infoSize);
				fBsf->par_in->extradata_size = infoSize;
			}
			fBsf->time_base_in = AVRational{1, 1000000};
			if (av_bsf_init(fBsf) < 0)
				return B_ERROR;
		}

		return _CreateContext();
	}

	/*!	Besides B_RGB32 and B_YCbCr422, the pictures can be had as they come
		from the decoder, a plane of luma and one of chroma: NV12 when they are
		eight bit, P010 when they are ten (NV12 then keeps the top eight). Those
		cost a copy, where the others are a conversion of every sample, done
		on the thread that decodes. */
	status_t NegotiateOutputFormat(media_format* format) override
	{
		if (format == NULL || (format->type != B_MEDIA_RAW_VIDEO
				&& format->type != B_MEDIA_UNKNOWN_TYPE
				&& format->type != B_MEDIA_NO_TYPE)) {
			return B_MEDIA_BAD_FORMAT;
		}
		color_space wanted = format->type == B_MEDIA_RAW_VIDEO
			? format->u.raw_video.display.format : B_NO_COLOR_SPACE;
		if (wanted != B_NO_COLOR_SPACE && wanted != B_RGB32
			&& wanted != B_YCbCr422 && wanted != kColorSpaceNV12
			&& wanted != kColorSpaceP010) {
			return B_MEDIA_BAD_FORMAT;
		}
		// The first picture says how large and how deep they are; it is kept
		// for the first Decode(), however often this is asked.
		if (fHeldFrame == NULL) {
			status_t status = _NextPicture(&fHeldFrame);
			if (status != B_OK)
				return status;
		}
		bool tenBit = _IsTenBit(fHeldFrame);
		if (wanted == kColorSpaceP010 && !tenBit)
			return B_MEDIA_BAD_FORMAT;
		fOutputSpace = wanted == B_NO_COLOR_SPACE ? B_RGB32 : wanted;
		fWidth = mpp_frame_get_width(fHeldFrame);
		fHeight = mpp_frame_get_height(fHeldFrame);

		media_raw_video_format raw = fInputFormat.u.encoded_video.output;
		raw.interlace = 1;
		raw.first_active = 0;
		raw.last_active = fHeight - 1;
		raw.display.format = fOutputSpace;
		raw.display.line_width = fWidth;
		raw.display.line_count = fHeight;
		raw.display.bytes_per_row = _RowBytes();
		format->type = B_MEDIA_RAW_VIDEO;
		format->require_flags = 0;
		format->deny_flags = B_MEDIA_MAUI_UNDEFINED_FLAGS;
		format->u.raw_video = raw;
		return B_OK;
	}

	/*!	MPP is reset rather than made again: that keeps its picture buffers
		(a quarter of a gigabyte for a 4K film, which the driver clears
		before handing out again), and a context destroyed while it decoded
		kept one of them for good. */
	status_t SeekedTo(int64, bigtime_t) override
	{
		_DropHeldFrame();
		if (fContext == NULL || fApi->reset(fContext) != MPP_OK) {
			_DestroyContext();
			if (_CreateContext() != B_OK)
				return B_ERROR;
		}
		if (fBsf != NULL)
			av_bsf_flush(fBsf);
		fPending.clear();
		fHasPending = false;
		fEndOfInput = false;
		fEosSent = false;
		fEosReached = false;
		fSubmittedPackets = 0;
		return B_OK;
	}

	status_t Decode(void* buffer, int64* frameCount, media_header* header,
		media_decode_info*) override
	{
		if (buffer == NULL || frameCount == NULL || header == NULL)
			return B_BAD_VALUE;
		if (fWidth <= 0 || fHeight <= 0)
			return B_NO_INIT;
		MppFrame frame = fHeldFrame;
		fHeldFrame = NULL;
		if (frame == NULL) {
			status_t status = _NextPicture(&frame);
			if (status != B_OK)
				return status;
		}
		bigtime_t t0 = system_time();
		status_t status = _ConvertFrame(frame, static_cast<uint8_t*>(buffer),
			*header);
		sSpent[2] += system_time() - t0;
		mpp_frame_deinit(&frame);
		if (status != B_OK)
			return status;
		*frameCount = 1;
		return B_OK;
	}

private:
	void _DestroyContext()
	{
		if (fContext != NULL) {
			// Stopped first, as FFmpeg's rkmpp decoder does: destroyed in
			// the middle of a picture, MPP did not give its buffer back.
			fApi->reset(fContext);
			mpp_destroy(fContext);
		}
		fContext = NULL;
		fApi = NULL;
		if (fFrameGroup != NULL)
			mpp_buffer_group_put(fFrameGroup);
		fFrameGroup = NULL;
	}

	void _DropHeldFrame()
	{
		if (fHeldFrame != NULL)
			mpp_frame_deinit(&fHeldFrame);
		fHeldFrame = NULL;
	}

	status_t _CreateContext()
	{
		if (mpp_create(&fContext, &fApi) != MPP_OK
			|| mpp_init(fContext, MPP_CTX_DEC, fCoding) != MPP_OK
			|| _ConfigureContext() != B_OK) {
			_DestroyContext();
			return B_ERROR;
		}
		return B_OK;
	}

	status_t _ConfigureContext()
	{
		RK_U32 split = 1;
		RK_S64 inputTimeout = 0;
		if (fApi->control(fContext, MPP_SET_INPUT_TIMEOUT, &inputTimeout)
				!= MPP_OK
			|| fApi->control(fContext, MPP_DEC_SET_PARSER_SPLIT_MODE, &split)
				!= MPP_OK) {
			return B_ERROR;
		}
		// NV12 is what MPP gives anyway; asked for, it also makes the RK3588's
		// AV1 decoder cut ten-bit pictures to eight, so AV1 is not asked.
		MppFrameFormat outputFormat = MPP_FMT_YUV420SP;
		if (fCoding != MPP_VIDEO_CodingAV1
			&& fApi->control(fContext, MPP_DEC_SET_OUTPUT_FORMAT, &outputFormat)
				!= MPP_OK) {
			return B_ERROR;
		}
		return B_OK;
	}

	status_t _PreparePacket(const void* chunk, size_t size, bigtime_t startTime,
		std::vector<uint8_t>& bytes)
	{
		if (fBsf == NULL) {
			if (fCoding == MPP_VIDEO_CodingAV1)
				bytes.assign({0x12, 0x00});
			const uint8_t* source = static_cast<const uint8_t*>(chunk);
			bytes.insert(bytes.end(), source, source + size);
			return B_OK;
		}
		AVPacket* input = av_packet_alloc();
		AVPacket* output = av_packet_alloc();
		if (input == NULL || output == NULL) {
			av_packet_free(&input);
			av_packet_free(&output);
			return B_NO_MEMORY;
		}
		status_t status = B_ERROR;
		if (av_new_packet(input, size) == 0) {
			memcpy(input->data, chunk, size);
			input->pts = input->dts = startTime;
			if (av_bsf_send_packet(fBsf, input) == 0
				&& av_bsf_receive_packet(fBsf, output) == 0) {
				bytes.assign(output->data, output->data + output->size);
				status = B_OK;
			}
		}
		av_packet_free(&input);
		av_packet_free(&output);
		return status;
	}

	/*!	Hands a prepared packet to MPP. B_WOULD_BLOCK means its input queue
		is full: the packet is kept, pictures are taken out to make room, and
		it is offered again. Failing there (as this once did after ten
		milliseconds) ended playback of every film with B-frames. */
	status_t _Put(const std::vector<uint8_t>& bytes, bigtime_t startTime,
		bool eos)
	{
		MppPacket packet = NULL;
		bigtime_t t0 = system_time();
		MPP_RET result = mpp_packet_init(&packet,
			bytes.empty() ? NULL : (void*)bytes.data(), bytes.size());
		if (result == MPP_OK) {
			mpp_packet_set_pts(packet, startTime);
			if (eos)
				mpp_packet_set_eos(packet);
			result = fApi->decode_put_packet(fContext, packet);
		}
		sSpent[1] += system_time() - t0;
		if (packet != NULL)
			mpp_packet_deinit(&packet);
		if (fSubmittedPackets < 4 || (result != MPP_OK
				&& result != MPP_ERR_BUFFER_FULL)) {
			fprintf(stderr, "RockchipMppDecoder: packet=%u stream=%zu"
				" pts=%lld result=%d\n", fSubmittedPackets, bytes.size(),
				(long long)startTime, result);
		}
		if (result == MPP_ERR_BUFFER_FULL)
			return B_WOULD_BLOCK;
		if (result != MPP_OK)
			return B_ERROR;
		fSubmittedPackets++;
		return B_OK;
	}

	status_t _Submit(const void* chunk, size_t size, bigtime_t startTime,
		bool eos)
	{
		std::vector<uint8_t> bytes;
		if (size > 0) {
			status_t status = _PreparePacket(chunk, size, startTime, bytes);
			if (status != B_OK)
				return status;
		}
		status_t status = _Put(bytes, startTime, eos);
		if (status == B_WOULD_BLOCK) {
			fPending.swap(bytes);
			fPendingTime = startTime;
			fPendingEos = eos;
			fHasPending = true;
			return B_OK;
		}
		return status;
	}

	static bool _IsTenBit(MppFrame frame)
	{
		return (mpp_frame_get_fmt(frame) & MPP_FRAME_FMT_MASK)
			== MPP_FMT_YUV420SP_10BIT;
	}

	size_t _RowBytes() const
	{
		// The planar layouts keep whole pairs of chroma samples in a row.
		size_t evenWidth = ((size_t)fWidth + 1) & ~(size_t)1;
		if (fOutputSpace == kColorSpaceNV12)
			return evenWidth;
		if (fOutputSpace == kColorSpaceP010)
			return evenWidth * 2;
		return (size_t)fWidth * (fOutputSpace == B_YCbCr422 ? 2 : 4);
	}

	/*!	Writes the picture into the caller's buffer in the negotiated format
		and size. A picture of another size (the stream changed) is cut or
		left short rather than overrunning the buffer. */
	status_t _ConvertFrame(MppFrame frame, uint8_t* output,
		media_header& header)
	{
		MppBuffer buffer = mpp_frame_get_buffer(frame);
		uint8_t* pixels = buffer != NULL
			? static_cast<uint8_t*>(mpp_buffer_get_ptr(buffer)) : NULL;
		int width = mpp_frame_get_width(frame);
		int height = mpp_frame_get_height(frame);
		int horizontal = mpp_frame_get_hor_stride(frame);
		int vertical = mpp_frame_get_ver_stride(frame);
		MppFrameFormat format = (MppFrameFormat)(mpp_frame_get_fmt(frame)
			& MPP_FRAME_FMT_MASK);
		bool tenBit = format == MPP_FMT_YUV420SP_10BIT;
		if (pixels == NULL || width <= 0 || height <= 0
			|| horizontal < (tenBit ? width * 10 / 8 : width)
			|| vertical < height) {
			return B_ERROR;
		}
		if (format != MPP_FMT_YUV420SP && format != MPP_FMT_YUV420SP_VU
			&& !tenBit) {
			return B_NOT_SUPPORTED;
		}
		const uint8_t* chroma = pixels + (size_t)horizontal * vertical;
		bool swap = format == MPP_FMT_YUV420SP_VU;
		if (width != fWidth || height != fHeight) {
			if (fDecodedFrames < 8) {
				fprintf(stderr, "RockchipMppDecoder: %dx%d picture for %dx%d\n",
					width, height, fWidth, fHeight);
			}
			width = std::min(width, fWidth);
			height = std::min(height, fHeight);
		}
		int chromaRows = (height + 1) / 2;
		int chromaSamples = (width + 1) & ~1;
		size_t rowBytes = _RowBytes();
		uint8_t* outChroma = output + rowBytes * fHeight;

		if (fOutputSpace == kColorSpaceP010 && tenBit) {
			for (int y = 0; y < height; y++) {
				unpack_ten_bit_row(pixels + (size_t)y * horizontal,
					reinterpret_cast<uint16_t*>(output + rowBytes * y), width);
			}
			for (int y = 0; y < chromaRows; y++) {
				unpack_ten_bit_row(chroma + (size_t)y * horizontal,
					reinterpret_cast<uint16_t*>(outChroma + rowBytes * y),
					chromaSamples);
			}
			return _FinishFrame(frame, header, horizontal, vertical);
		}

		if (fOutputSpace == kColorSpaceNV12 && !tenBit && !swap) {
			for (int y = 0; y < height; y++) {
				memcpy(output + rowBytes * y, pixels + (size_t)y * horizontal,
					width);
			}
			for (int y = 0; y < chromaRows; y++) {
				memcpy(outChroma + rowBytes * y,
					chroma + (size_t)y * horizontal, chromaSamples);
			}
			return _FinishFrame(frame, header, horizontal, vertical);
		}

		// Everything else starts from eight-bit NV12 (or NV21): ten-bit
		// samples keep their top eight bits.
		const uint8_t* luma8 = pixels;
		const uint8_t* chroma8 = chroma;
		int stride8 = horizontal;
		if (tenBit) {
			stride8 = chromaSamples;
			fNarrow.resize((size_t)stride8 * (height + chromaRows));
			std::vector<uint16_t> row(chromaSamples);
			for (int y = 0; y < height + chromaRows; y++) {
				const uint8_t* in = y < height
					? pixels + (size_t)y * horizontal
					: chroma + (size_t)(y - height) * horizontal;
				unpack_ten_bit_row(in, row.data(), y < height ? width
					: chromaSamples);
				uint8_t* out = fNarrow.data() + (size_t)y * stride8;
				for (int x = 0; x < (y < height ? width : chromaSamples); x++)
					out[x] = row[x] >> 8;
			}
			luma8 = fNarrow.data();
			chroma8 = luma8 + (size_t)stride8 * height;
		}

		if (fOutputSpace == kColorSpaceNV12) {
			for (int y = 0; y < height; y++)
				memcpy(output + rowBytes * y, luma8 + (size_t)y * stride8, width);
			for (int y = 0; y < chromaRows; y++) {
				const uint8_t* in = chroma8 + (size_t)y * stride8;
				uint8_t* out = outChroma + rowBytes * y;
				for (int x = 0; x < chromaSamples; x += 2) {
					out[x] = in[x + (swap ? 1 : 0)];
					out[x + 1] = in[x + (swap ? 0 : 1)];
				}
			}
			return _FinishFrame(frame, header, horizontal, vertical);
		}

		// Semi-planar 4:2:0 to Y0 Cb Y1 Cr is a repacking: done here it is
		// a few milliseconds a 1080p picture; swscale's general path took
		// several times that.
		if (fOutputSpace == B_YCbCr422) {
			int pairs = width / 2;
			int u = swap ? 1 : 0;
			int v = swap ? 0 : 1;
			for (int y = 0; y < height; y++) {
				const uint8_t* luma = luma8 + (size_t)y * stride8;
				const uint8_t* pair = chroma8 + (size_t)(y / 2) * stride8;
				uint8_t* out = output + rowBytes * y;
				for (int x = 0; x < pairs; x++) {
					out[4 * x + 0] = luma[2 * x];
					out[4 * x + 1] = pair[2 * x + u];
					out[4 * x + 2] = luma[2 * x + 1];
					out[4 * x + 3] = pair[2 * x + v];
				}
			}
			return _FinishFrame(frame, header, horizontal, vertical);
		}

		const uint8_t* planes[4] = {luma8, chroma8, NULL, NULL};
		int strides[4] = {stride8, stride8, 0, 0};
		fSws = sws_getCachedContext(fSws, width, height,
			swap ? AV_PIX_FMT_NV21 : AV_PIX_FMT_NV12, width, height,
			AV_PIX_FMT_RGB32, SWS_FAST_BILINEAR, NULL, NULL, NULL);
		if (fSws == NULL)
			return B_ERROR;
		uint8_t* destination[4] = {output, NULL, NULL, NULL};
		int destinationStride[4] = {(int)rowBytes, 0, 0, 0};
		if (sws_scale(fSws, planes, strides, 0, height, destination,
				destinationStride) != height) {
			return B_ERROR;
		}
		return _FinishFrame(frame, header, horizontal, vertical);
	}

	status_t _FinishFrame(MppFrame frame, media_header& header,
		int horizontal, int vertical)
	{
		if (fDecodedFrames < 4) {
			fprintf(stderr, "RockchipMppDecoder: frame=%u %ux%u stride=%dx%d"
				" format=%#x output=%#x pts=%lld\n", fDecodedFrames,
				mpp_frame_get_width(frame), mpp_frame_get_height(frame),
				horizontal, vertical, mpp_frame_get_fmt(frame),
				(unsigned)fOutputSpace, (long long)mpp_frame_get_pts(frame));
		}
		fDecodedFrames++;
		memset(&header, 0, sizeof(header));
		header.type = B_MEDIA_RAW_VIDEO;
		RK_S64 pts = mpp_frame_get_pts(frame);
		header.start_time = pts >= 0 ? pts : fLastInputTime;
		size_t rowBytes = _RowBytes();
		header.size_used = rowBytes * fHeight;
		if (fOutputSpace == kColorSpaceNV12 || fOutputSpace == kColorSpaceP010)
			header.size_used += rowBytes * ((fHeight + 1) / 2);
		header.file_pos = -1;
		header.u.raw_video.field_gamma = 1.0f;
		header.u.raw_video.display_line_width = fWidth;
		header.u.raw_video.display_line_count = fHeight;
		header.u.raw_video.bytes_per_row = rowBytes;
		return B_OK;
	}

	static bigtime_t sSpent[4];	// get_frame, put, convert, chunk

	void _CountPath(int path)
	{
		static uint32 counts[9];
		static bigtime_t last = 0;
		counts[path]++;
		bigtime_t now = system_time();
		if (getenv("MPP_TRACE_LOOP") != NULL && now - last > 2000000) {
			fprintf(stderr, "RockchipMppDecoder: loop frames=%u info=%u "
				"put-ok=%u full=%u submit=%u eos=%u idle=%u getframe-err=%u"
				" dropped=%u | get %lldms put %lldms convert %lldms"
				" chunk %lldms\n", counts[0], counts[1], counts[2], counts[3],
				counts[4], counts[5], counts[6], counts[7], counts[8],
				(long long)sSpent[0] / 1000, (long long)sSpent[1] / 1000,
				(long long)sSpent[2] / 1000, (long long)sSpent[3] / 1000);
			last = now;
		}
	}

	/*!	The next picture MPP puts out, fed with packets until it does.
		Pictures it marks as broken (those after a seek that refer to ones
		before it) or to be left out are not shown, as FFmpeg's rkmpp
		decoder does not show them. */
	status_t _NextPicture(MppFrame* _frame)
	{
		if (fEosReached)
			return B_LAST_BUFFER_ERROR;
		for (int step = 0; step < 20000; step++) {
			MppFrame frame = NULL;
			bigtime_t t0 = system_time();
			MPP_RET result = fApi->decode_get_frame(fContext, &frame);
			sSpent[0] += system_time() - t0;
			if (result != MPP_OK) {
				_CountPath(7);
				return B_ERROR;
			}
			if (frame != NULL) {
				if (mpp_frame_get_info_change(frame)) {
					fprintf(stderr, "RockchipMppDecoder: info-change %ux%u"
						" stride=%ux%u buffer=%zu format=%#x\n",
						mpp_frame_get_width(frame), mpp_frame_get_height(frame),
						mpp_frame_get_hor_stride(frame),
						mpp_frame_get_ver_stride(frame),
						mpp_frame_get_buf_size(frame), mpp_frame_get_fmt(frame));
					if (fFrameGroup == NULL
						&& mpp_buffer_group_get_internal(&fFrameGroup,
							MPP_BUFFER_TYPE_DMA_HEAP) != MPP_OK) {
						mpp_frame_deinit(&frame);
						return B_ERROR;
					}
					result = fApi->control(fContext, MPP_DEC_SET_EXT_BUF_GROUP,
						fFrameGroup);
					if (result == MPP_OK)
						result = fApi->control(fContext,
							MPP_DEC_SET_INFO_CHANGE_READY, NULL);
					mpp_frame_deinit(&frame);
					if (result != MPP_OK)
						return B_ERROR;
					_CountPath(1);
					continue;
				}
				bool eos = mpp_frame_get_eos(frame) != 0;
				if (eos)
					fEosReached = true;
				if (mpp_frame_get_buffer(frame) == NULL
					|| mpp_frame_get_errinfo(frame) != 0
					|| mpp_frame_get_discard(frame) != 0) {
					mpp_frame_deinit(&frame);
					if (eos)
						return B_LAST_BUFFER_ERROR;
					_CountPath(8);
					continue;
				}
				_CountPath(0);
				*_frame = frame;
				return B_OK;
			}
			if (fHasPending) {
				status_t status = _Put(fPending, fPendingTime, fPendingEos);
				if (status == B_OK) {
					fHasPending = false;
					fPending.clear();
					if (fPendingEos)
						fEosSent = true;
					_CountPath(2);
					continue;
				}
				if (status != B_WOULD_BLOCK)
					return status;
				// Full: a picture has to come out before more can go in.
				_CountPath(3);
				snooze(500);
				continue;
			}
			// Waiting for a picture after each packet (as this once did, for
			// up to 30 ms) starves a decoder that needs the packets after a
			// picture before it can put it out (B-frames): keep its input
			// full instead, and let BUFFER_FULL say when to wait.
			if (!fEndOfInput) {
				const void* chunk = NULL;
				size_t size = 0;
				media_header chunkHeader = {};
				bigtime_t t2 = system_time();
				status_t status = GetNextChunk(&chunk, &size, &chunkHeader);
				sSpent[3] += system_time() - t2;
				if (status == B_OK) {
					fLastInputTime = chunkHeader.start_time;
					status = _Submit(chunk, size, fLastInputTime, false);
					if (status != B_OK)
						return status;
					_CountPath(4);
					// Let the decoder threads take the packet before asking
					// for a picture again.
					if (fHasPending)
						snooze(500);
					continue;
				}
				if (status != B_LAST_BUFFER_ERROR)
					return status;
				fEndOfInput = true;
			}
			if (!fEosSent) {
				status_t status = _Submit(NULL, 0, fLastInputTime, true);
				if (status != B_OK)
					return status;
				if (!fHasPending)
					fEosSent = true;
				_CountPath(5);
				continue;
			}
			_CountPath(6);
			snooze(1000);
		}
		return fEndOfInput ? B_LAST_BUFFER_ERROR : B_TIMED_OUT;
	}

	media_format fInputFormat = {};
	AVCodecID fCodecId = AV_CODEC_ID_NONE;
	MppCodingType fCoding = MPP_VIDEO_CodingUnused;
	MppCtx fContext = NULL;
	MppApi* fApi = NULL;
	MppBufferGroup fFrameGroup = NULL;
	AVBSFContext* fBsf = NULL;
	SwsContext* fSws = NULL;
	MppFrame fHeldFrame = NULL;
	std::vector<uint8_t> fNarrow;
	bigtime_t fLastInputTime = 0;
	int fWidth = 0;
	int fHeight = 0;
	bool fEndOfInput = false;
	bool fEosSent = false;
	bool fEosReached = false;
	uint32 fSubmittedPackets = 0;
	uint32 fDecodedFrames = 0;
	color_space fOutputSpace = B_RGB32;
	std::vector<uint8_t> fPending;
	bigtime_t fPendingTime = 0;
	bool fPendingEos = false;
	bool fHasPending = false;
};


bigtime_t RockchipMppDecoder::sSpent[4];


class RockchipMppPlugin : public DecoderPlugin {
public:
	Decoder* NewDecoder(uint) override
	{
		return new(std::nothrow) RockchipMppDecoder;
	}

	status_t GetSupportedFormats(media_format** formats, size_t* count) override
	{
		static media_format supported[4];
		static size_t supportedCount = 0;
		static bool initialized = false;
		if (!initialized) {
			// Only where the RK3588's decoders are: the Media Kit gives a
			// format to the first add-on that claims it and does not try
			// another when that one fails, and the package that carries
			// this add-on is installed on other arm64 boards too.
			if (access("/dev/video/rk3588_vpu/0", F_OK) != 0) {
				initialized = true;
				*formats = supported;
				*count = 0;
				return B_OK;
			}
			const AVCodecID codecs[] = {AV_CODEC_ID_H264, AV_CODEC_ID_HEVC,
				AV_CODEC_ID_AV1, AV_CODEC_ID_VP9};
			BMediaFormats registry;
			for (size_t index = 0; index < 4; index++) {
				media_format_description description = {};
				description.family = B_MISC_FORMAT_FAMILY;
				description.u.misc.file_format = 'ffmp';
				description.u.misc.codec = codecs[index];
				media_format format = {};
				format.type = B_MEDIA_ENCODED_VIDEO;
				format.deny_flags = B_MEDIA_MAUI_UNDEFINED_FLAGS;
				if (registry.MakeFormatFor(&description, 1, &format) != B_OK)
					return B_ERROR;
				supported[index] = format;
			}
			supportedCount = 4;
			initialized = true;
		}
		*formats = supported;
		*count = supportedCount;
		return B_OK;
	}
};


extern "C" MediaPlugin*
instantiate_plugin()
{
	return new(std::nothrow) RockchipMppPlugin;
}
