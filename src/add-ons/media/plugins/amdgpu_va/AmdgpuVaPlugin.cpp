/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	H.264 and HEVC decoding on AMD GPUs: libavcodec parses the streams and
	orders the pictures, VA-API (Mesa's radeonsi driver on amdgpu_drm, the
	GPU's UVD/VCN) decodes them. Decoded pictures are copied to system memory
	as NV12 or P010 and converted to what the caller negotiated with the
	conversions of the amduvd add-on.

	It is installed as "amduvd": airTime and Summit load decoders by name,
	NewDecoder(0) for H.264 and NewDecoder(1) for HEVC, and keep their
	software decoders for what this one refuses. libavcodec is linked in
	statically, so that it does not clash with the system's (without VA-API)
	that the applications load themselves. */


#include <DecoderPlugin.h>
#include <MediaFormats.h>

#include <dirent.h>
#include <new>
#include <stdio.h>
#include <string.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#include "../amduvd/H264Output.h"
#include "../amduvd/HevcOutput.h"


static const color_space kNV12 = (color_space)0x4e563132;
static const color_space kI420 = (color_space)0x49343230;
static const color_space kP010 = (color_space)0x50303130;

static const int kCodecH264 = 27;
static const int kCodecHevc = 173;


/*!	The render node of the first AMD GPU, "" if there is none. */
static const char*
amd_render_node()
{
	static char sPath[64];
	static bool sDone;
	if (sDone)
		return sPath;
	sDone = true;
	const char* env = getenv("AMDGPU_VA_DEVICE");
	if (env != NULL && env[0] != '\0') {
		strlcpy(sPath, env, sizeof(sPath));
		return sPath;
	}
	DIR* dir = opendir("/dev/dri");
	if (dir == NULL)
		return sPath;
	while (dirent* entry = readdir(dir)) {
		if (strncmp(entry->d_name, "renderD", 7) == 0) {
			snprintf(sPath, sizeof(sPath), "/dev/dri/%s", entry->d_name);
			break;
		}
	}
	closedir(dir);
	return sPath;
}


static enum AVPixelFormat
choose_vaapi(AVCodecContext*, const enum AVPixelFormat* formats)
{
	for (const enum AVPixelFormat* format = formats; *format != AV_PIX_FMT_NONE;
			format++) {
		if (*format == AV_PIX_FMT_VAAPI)
			return *format;
	}
	// no hardware path for this stream: fail rather than decode in software,
	// the caller has its own software decoder
	return AV_PIX_FMT_NONE;
}


class VaDecoder : public Decoder {
public:
								VaDecoder(bool hevc);
	virtual						~VaDecoder();

	virtual	void				GetCodecInfo(media_codec_info* info);
	virtual	status_t			Setup(media_format* input, const void* info,
									size_t size);
	virtual	status_t			NegotiateOutputFormat(media_format* output);
	virtual	status_t			SeekedTo(int64 frame, bigtime_t time);
	virtual	status_t			Decode(void* buffer, int64* count,
									media_header* header,
									media_decode_info* info);

private:
			status_t			_Fail(status_t status, const char* reason);
			void				_Close();
			status_t			_Receive();
			bool				_Convert(const AVFrame* frame, uint8* output);

			bool				fHevc;
			status_t			fStatus;
			media_format		fInput;
			int					fWidth;
			int					fHeight;
			color_space			fSpace;
			HevcOutput::Format	fFormat;
			size_t				fRowBytes;
			bool				fEnded;
			bool				fDrained;
			AVBufferRef*		fDevice;
			AVCodecContext*		fContext;
			AVPacket*			fPacket;
			AVFrame*			fFrame;		// from the decoder (a VA surface)
			AVFrame*			fSoftware;	// copied to system memory
			bool				fHaveFrame;
			HevcFrame			fPixels;
};


VaDecoder::VaDecoder(bool hevc)
	:
	fHevc(hevc),
	fStatus(B_NO_INIT),
	fWidth(0),
	fHeight(0),
	fSpace(B_RGB32),
	fFormat(HevcOutput::RGB32),
	fRowBytes(0),
	fEnded(false),
	fDrained(false),
	fDevice(NULL),
	fContext(NULL),
	fPacket(NULL),
	fFrame(NULL),
	fSoftware(NULL),
	fHaveFrame(false)
{
}


VaDecoder::~VaDecoder()
{
	_Close();
}


void
VaDecoder::_Close()
{
	avcodec_free_context(&fContext);
	av_buffer_unref(&fDevice);
	av_packet_free(&fPacket);
	av_frame_free(&fFrame);
	av_frame_free(&fSoftware);
	fHaveFrame = false;
}


status_t
VaDecoder::_Fail(status_t status, const char* reason)
{
	fprintf(stderr, "amduvd (VA-API): %s\n", reason);
	_Close();
	fStatus = status;
	return status;
}


void
VaDecoder::GetCodecInfo(media_codec_info* info)
{
	memset(info, 0, sizeof(*info));
	strlcpy(info->short_name, fHevc ? "amduvd hevc" : "amduvd h264",
		sizeof(info->short_name));
	strlcpy(info->pretty_name, fHevc
		? "AMD HEVC hardware decoder (VA-API)"
		: "AMD H.264 hardware decoder (VA-API)", sizeof(info->pretty_name));
}


status_t
VaDecoder::Setup(media_format* input, const void* info, size_t size)
{
	_Close();
	fStatus = B_NO_INIT;
	if (input == NULL || input->type != B_MEDIA_ENCODED_VIDEO)
		return B_NOT_SUPPORTED;
	media_format_description description;
	if (BMediaFormats().GetCodeFor(*input, B_MISC_FORMAT_FAMILY, &description)
			!= B_OK
		|| description.u.misc.file_format != 'ffmp'
		|| description.u.misc.codec
			!= (uint32)(fHevc ? kCodecHevc : kCodecH264))
		return B_NOT_SUPPORTED;
	const media_video_display_info& display
		= input->u.encoded_video.output.display;
	if (display.line_width == 0 || display.line_count == 0)
		return B_NOT_SUPPORTED;

	const char* node = amd_render_node();
	if (node[0] == '\0')
		return B_DEVICE_NOT_FOUND;
	if (av_hwdevice_ctx_create(&fDevice, AV_HWDEVICE_TYPE_VAAPI, node, NULL,
			0) < 0)
		return _Fail(B_DEVICE_NOT_FOUND, "no VA-API device");

	const AVCodec* codec = avcodec_find_decoder(fHevc ? AV_CODEC_ID_HEVC
		: AV_CODEC_ID_H264);
	fContext = codec != NULL ? avcodec_alloc_context3(codec) : NULL;
	fPacket = av_packet_alloc();
	fFrame = av_frame_alloc();
	fSoftware = av_frame_alloc();
	if (fContext == NULL || fPacket == NULL || fFrame == NULL
		|| fSoftware == NULL)
		return _Fail(B_NO_MEMORY, "no decoder");
	if (info != NULL && size > 0) {
		fContext->extradata = (uint8_t*)av_mallocz(size
			+ AV_INPUT_BUFFER_PADDING_SIZE);
		if (fContext->extradata == NULL)
			return _Fail(B_NO_MEMORY, "no memory for the stream header");
		memcpy(fContext->extradata, info, size);
		fContext->extradata_size = size;
	}
	fContext->width = display.line_width;
	fContext->height = display.line_count;
	fContext->pkt_timebase = (AVRational){ 1, 1000000 };
	fContext->hw_device_ctx = av_buffer_ref(fDevice);
	fContext->get_format = choose_vaapi;
	fContext->thread_count = 1;
	if (avcodec_open2(fContext, codec, NULL) < 0)
		return _Fail(B_ERROR, "cannot open the decoder");

	fInput = *input;
	fWidth = display.line_width;
	fHeight = display.line_count;
	fRowBytes = 0;
	fEnded = fDrained = false;
	fStatus = B_OK;
	return B_OK;
}


status_t
VaDecoder::NegotiateOutputFormat(media_format* output)
{
	if (fStatus != B_OK)
		return fStatus;
	if (output == NULL)
		return B_BAD_VALUE;
	color_space space = output->u.raw_video.display.format;
	HevcOutput::Format format;
	size_t bytesPerPixel;
	if (space == kNV12) {
		format = HevcOutput::NV12;
		bytesPerPixel = 1;
	} else if (space == kI420) {
		format = HevcOutput::I420;
		bytesPerPixel = 1;
	} else if (space == kP010 && fHevc) {
		format = HevcOutput::P010;
		bytesPerPixel = 2;
	} else if (space == B_YCbCr422) {
		format = HevcOutput::YCbCr422;
		bytesPerPixel = 2;
	} else if (space == B_RGB32 || space == B_NO_COLOR_SPACE) {
		space = B_RGB32;
		format = HevcOutput::RGB32;
		bytesPerPixel = 4;
	} else
		return B_NOT_SUPPORTED;
	fSpace = space;
	fFormat = format;
	fRowBytes = fWidth * bytesPerPixel;

	output->Clear();
	output->type = B_MEDIA_RAW_VIDEO;
	output->u.raw_video = fInput.u.encoded_video.output;
	media_raw_video_format& raw = output->u.raw_video;
	raw.interlace = 1;
	raw.first_active = 0;
	raw.last_active = fHeight - 1;
	raw.orientation = B_VIDEO_TOP_LEFT_RIGHT;
	raw.display.format = space;
	raw.display.line_width = fWidth;
	raw.display.line_count = fHeight;
	raw.display.bytes_per_row = fRowBytes;
	raw.display.pixel_offset = 0;
	raw.display.line_offset = 0;
	raw.display.flags = 0;
	return B_OK;
}


status_t
VaDecoder::SeekedTo(int64 frame, bigtime_t time)
{
	if (fStatus != B_OK)
		return fStatus;
	avcodec_flush_buffers(fContext);
	fHaveFrame = false;
	fEnded = fDrained = false;
	return B_OK;
}


/*!	Takes the next decoded picture, if there is one. */
status_t
VaDecoder::_Receive()
{
	int result = avcodec_receive_frame(fContext, fFrame);
	if (result == AVERROR(EAGAIN))
		return B_OK;
	if (result == AVERROR_EOF) {
		fDrained = true;
		return B_OK;
	}
	if (result < 0)
		return _Fail(B_ERROR, "decoding failed");
	if (fFrame->format != AV_PIX_FMT_VAAPI) {
		av_frame_unref(fFrame);
		return _Fail(B_NOT_SUPPORTED, "not decoded by the GPU");
	}
	fHaveFrame = true;
	return B_OK;
}


/*!	Copies the VA surface to system memory, then converts it into the
	caller's buffer. */
bool
VaDecoder::_Convert(const AVFrame* frame, uint8* output)
{
	av_frame_unref(fSoftware);
	if (av_hwframe_transfer_data(fSoftware, frame, 0) < 0)
		return false;
	bool p010 = fSoftware->format == AV_PIX_FMT_P010;
	if (!p010 && fSoftware->format != AV_PIX_FMT_NV12)
		return false;
	if (fSoftware->width < fWidth || fSoftware->height < fHeight)
		return false;

	// the layout the amduvd conversions take: luma rows, then the
	// interleaved chroma rows, one pitch
	uint32 sampleBytes = p010 ? 2 : 1;
	uint32 pitch = fSoftware->width * sampleBytes;
	uint32 height = fSoftware->height;
	try {
		fPixels.pixels.resize((size_t)pitch * height * 3 / 2);
	} catch (const std::bad_alloc&) {
		return false;
	}
	for (uint32 y = 0; y < height; y++) {
		memcpy(fPixels.pixels.data() + (size_t)y * pitch,
			fSoftware->data[0] + (size_t)y * fSoftware->linesize[0], pitch);
	}
	uint8* chroma = fPixels.pixels.data() + (size_t)pitch * height;
	for (uint32 y = 0; y < height / 2; y++) {
		memcpy(chroma + (size_t)y * pitch,
			fSoftware->data[1] + (size_t)y * fSoftware->linesize[1], pitch);
	}
	fPixels.pitch = pitch;
	fPixels.codedHeight = height;
	fPixels.width = fWidth;
	fPixels.height = fHeight;
	fPixels.cropLeft = fPixels.cropTop = 0;
	fPixels.fullRange = frame->color_range == AVCOL_RANGE_JPEG;
	// both use the matrix coefficients of ITU-T H.273
	fPixels.matrix = frame->colorspace != AVCOL_SPC_UNSPECIFIED
		? frame->colorspace : (fHeight >= 720 ? AVCOL_SPC_BT709
			: AVCOL_SPC_SMPTE170M);
	fPixels.p010 = p010;
	fPixels.bitDepth = p010 ? 10 : 8;
	return HevcOutput::Copy(fPixels, fFormat, output,
		HevcOutput::Bytes(fWidth, fHeight, fFormat));
}


status_t
VaDecoder::Decode(void* buffer, int64* count, media_header* header,
	media_decode_info* info)
{
	if (count == NULL || header == NULL || buffer == NULL)
		return B_BAD_VALUE;
	*count = 0;
	if (fStatus != B_OK)
		return fStatus;
	if (fRowBytes == 0)
		return B_NO_INIT;

	for (;;) {
		if (fHaveFrame) {
			bigtime_t time = fFrame->best_effort_timestamp != AV_NOPTS_VALUE
				? fFrame->best_effort_timestamp : fFrame->pts;
			bool skip = info != NULL && info->time_to_decode < 0
				&& info->time_to_decode != INT64_MIN
				&& time < -info->time_to_decode;
			if (!skip && !_Convert(fFrame, (uint8*)buffer))
				return _Fail(B_NOT_SUPPORTED, "cannot convert the picture");
			memset(header, 0, sizeof(*header));
			header->type = B_MEDIA_RAW_VIDEO;
			header->start_time = time;
			header->size_used = HevcOutput::Bytes(fWidth, fHeight, fFormat);
			header->u.raw_video.display_line_width = fWidth;
			header->u.raw_video.display_line_count = fHeight;
			header->u.raw_video.bytes_per_row = fRowBytes;
			header->u.raw_video.line_count = fHeight;
			av_frame_unref(fFrame);
			fHaveFrame = false;
			*count = 1;
			return B_OK;
		}
		if (fDrained)
			return B_LAST_BUFFER_ERROR;

		status_t status = _Receive();
		if (status != B_OK)
			return status;
		if (fHaveFrame || fDrained)
			continue;
		if (fEnded) {
			// the decoder holds pictures back for reordering: they come now
			status = _Receive();
			if (status != B_OK)
				return status;
			if (!fHaveFrame && !fDrained)
				fDrained = true;
			continue;
		}

		const void* chunk = NULL;
		size_t size = 0;
		media_header input = {};
		status = GetNextChunk(&chunk, &size, &input);
		if (status == B_LAST_BUFFER_ERROR) {
			fEnded = true;
			avcodec_send_packet(fContext, NULL);
			continue;
		}
		// interrupted (a seek): keep what is queued until SeekedTo()
		if (status != B_OK)
			return status;
		av_packet_unref(fPacket);
		if (av_new_packet(fPacket, size) < 0)
			return _Fail(B_NO_MEMORY, "no packet");
		memcpy(fPacket->data, chunk, size);
		fPacket->pts = input.start_time;
		fPacket->dts = AV_NOPTS_VALUE;
		int result = avcodec_send_packet(fContext, fPacket);
		if (result < 0 && result != AVERROR(EAGAIN))
			return _Fail(B_BAD_DATA, "the decoder refused a packet");
	}
}


class AmdgpuVaPlugin : public DecoderPlugin {
public:
	virtual Decoder* NewDecoder(uint index)
	{
		if (index == 0)
			return new(std::nothrow) VaDecoder(false);
		if (index == 1)
			return new(std::nothrow) VaDecoder(true);
		return NULL;
	}

	virtual status_t GetSupportedFormats(media_format** formats, size_t* count)
	{
		// applications select this add-on by name and keep their own
		// software decoders for what it refuses (as with amduvd)
		if (formats != NULL)
			*formats = NULL;
		if (count != NULL)
			*count = 0;
		return B_NOT_SUPPORTED;
	}
};


MediaPlugin*
instantiate_plugin()
{
	return new(std::nothrow) AmdgpuVaPlugin();
}
