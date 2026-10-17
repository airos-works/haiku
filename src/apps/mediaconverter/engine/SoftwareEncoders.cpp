/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * The x264 and x265 interfaces in thirdparty/ are GPL-2.0-or-later; they are
 * only used through the libraries opened here at run time.
 */


#include "SoftwareEncoders.h"

#include <Autolock.h>
#include <ByteOrder.h>
#include <Locker.h>

#include <dlfcn.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <deque>

extern "C" {
#include <libavutil/pixdesc.h>
}

#include "x264/x264.h"
#include "x265/x265.h"
#include "svt-av1/EbSvtAv1Enc.h"


static BLocker sLibraryLock("encoder libraries");


// #pragma mark - libraries


struct X264Library {
	bool		tried;
	void*		image;
	void		(*param_default)(x264_param_t*);
	int			(*param_default_preset)(x264_param_t*, const char*,
					const char*);
	int			(*param_apply_profile)(x264_param_t*, const char*);
	int			(*param_parse)(x264_param_t*, const char*, const char*);
	void		(*picture_init)(x264_picture_t*);
	x264_t*		(*encoder_open)(x264_param_t*);
	int			(*encoder_headers)(x264_t*, x264_nal_t**, int*);
	int			(*encoder_encode)(x264_t*, x264_nal_t**, int*,
					x264_picture_t*, x264_picture_t*);
	int			(*encoder_delayed_frames)(x264_t*);
	void		(*encoder_close)(x264_t*);
};


struct X265Library {
	bool		tried;
	void*		image;
	const x265_api* (*api_get)(int bitDepth);
};


struct SvtLibrary {
	bool		tried;
	void*		image;
	const char*	(*get_version)(void);
	EbErrorType	(*init_handle)(EbComponentType**, EbSvtAv1EncConfiguration*);
	EbErrorType	(*set_parameter)(EbComponentType*, EbSvtAv1EncConfiguration*);
	EbErrorType	(*init)(EbComponentType*);
	EbErrorType	(*stream_header)(EbComponentType*, EbBufferHeaderType**);
	EbErrorType	(*stream_header_release)(EbBufferHeaderType*);
	EbErrorType	(*send_picture)(EbComponentType*, EbBufferHeaderType*);
	EbErrorType	(*get_packet)(EbComponentType*, EbBufferHeaderType**,
					uint8_t);
	void		(*release_out_buffer)(EbBufferHeaderType**);
	EbErrorType	(*deinit)(EbComponentType*);
	EbErrorType	(*deinit_handle)(EbComponentType*);
};


static X264Library sX264;
static X265Library sX265;
static SvtLibrary sSvt;


#define STRINGIFY2(x) #x
#define STRINGIFY(x) STRINGIFY2(x)


template<typename T>
static bool
find_symbol(void* image, const char* name, T& function)
{
	function = (T)dlsym(image, name);
	return function != NULL;
}


static const X264Library*
x264_library()
{
	BAutolock _(sLibraryLock);
	if (!sX264.tried) {
		sX264.tried = true;
		void* image = dlopen("libx264.so." STRINGIFY(X264_BUILD), RTLD_NOW);
		if (image != NULL
			&& find_symbol(image, "x264_param_default", sX264.param_default)
			&& find_symbol(image, "x264_param_default_preset",
				sX264.param_default_preset)
			&& find_symbol(image, "x264_param_apply_profile",
				sX264.param_apply_profile)
			&& find_symbol(image, "x264_param_parse", sX264.param_parse)
			&& find_symbol(image, "x264_picture_init", sX264.picture_init)
			&& find_symbol(image, "x264_encoder_open_" STRINGIFY(X264_BUILD),
				sX264.encoder_open)
			&& find_symbol(image, "x264_encoder_headers",
				sX264.encoder_headers)
			&& find_symbol(image, "x264_encoder_encode",
				sX264.encoder_encode)
			&& find_symbol(image, "x264_encoder_delayed_frames",
				sX264.encoder_delayed_frames)
			&& find_symbol(image, "x264_encoder_close",
				sX264.encoder_close)) {
			sX264.image = image;
		} else if (image != NULL)
			dlclose(image);
	}
	return sX264.image != NULL ? &sX264 : NULL;
}


static const x265_api*
x265_library(int bitDepth)
{
	{
		BAutolock _(sLibraryLock);
		if (!sX265.tried) {
			sX265.tried = true;
			void* image = dlopen("libx265.so." STRINGIFY(X265_BUILD),
				RTLD_NOW);
			if (image != NULL && find_symbol(image,
					"x265_api_get_" STRINGIFY(X265_BUILD), sX265.api_get))
				sX265.image = image;
			else if (image != NULL)
				dlclose(image);
		}
	}
	if (sX265.image == NULL)
		return NULL;
	const x265_api* api = sX265.api_get(bitDepth);
	if (api == NULL || api->bit_depth != bitDepth
		|| api->sizeof_param > (int)sizeof(x265_param) + 4096)
		return NULL;
	return api;
}


static const SvtLibrary*
svt_library()
{
	BAutolock _(sLibraryLock);
	if (!sSvt.tried) {
		sSvt.tried = true;
		void* image = dlopen("libSvtAv1Enc.so." STRINGIFY(SVT_AV1_VERSION_MAJOR),
			RTLD_NOW);
		if (image != NULL
			&& find_symbol(image, "svt_av1_get_version", sSvt.get_version)
			&& find_symbol(image, "svt_av1_enc_init_handle",
				sSvt.init_handle)
			&& find_symbol(image, "svt_av1_enc_set_parameter",
				sSvt.set_parameter)
			&& find_symbol(image, "svt_av1_enc_init", sSvt.init)
			&& find_symbol(image, "svt_av1_enc_stream_header",
				sSvt.stream_header)
			&& find_symbol(image, "svt_av1_enc_stream_header_release",
				sSvt.stream_header_release)
			&& find_symbol(image, "svt_av1_enc_send_picture",
				sSvt.send_picture)
			&& find_symbol(image, "svt_av1_enc_get_packet", sSvt.get_packet)
			&& find_symbol(image, "svt_av1_enc_release_out_buffer",
				sSvt.release_out_buffer)
			&& find_symbol(image, "svt_av1_enc_deinit", sSvt.deinit)
			&& find_symbol(image, "svt_av1_enc_deinit_handle",
				sSvt.deinit_handle)) {
			sSvt.image = image;
		} else if (image != NULL)
			dlclose(image);
	}
	return sSvt.image != NULL ? &sSvt : NULL;
}


// #pragma mark - common


/*!	Splits Annex B parameter sets from the SEI messages an encoder puts with
	them: the SEI goes in front of the first picture instead, as FFmpeg's
	own wrappers do, so that the extradata holds only what avcC/hvcC take. */
class PacketQueue {
public:
	~PacketQueue()
	{
		while (!fPackets.empty()) {
			av_packet_free(&fPackets.front());
			fPackets.pop_front();
		}
	}

	AVPacket* Add(const uint8* data, size_t size)
	{
		AVPacket* packet = av_packet_alloc();
		if (packet == NULL)
			return NULL;
		size_t prefix = fPrefix.size();
		if (av_new_packet(packet, (int)(size + prefix)) < 0) {
			av_packet_free(&packet);
			return NULL;
		}
		if (prefix > 0) {
			memcpy(packet->data, fPrefix.data(), prefix);
			fPrefix.clear();
		}
		memcpy(packet->data + prefix, data, size);
		fPackets.push_back(packet);
		return packet;
	}

	status_t Take(AVPacket* packet, bool drained)
	{
		if (fPackets.empty())
			return drained ? B_LAST_BUFFER_ERROR : B_WOULD_BLOCK;
		AVPacket* front = fPackets.front();
		fPackets.pop_front();
		av_packet_move_ref(packet, front);
		av_packet_free(&front);
		return B_OK;
	}

	void SetPrefix(const uint8* data, size_t size)
	{
		fPrefix.assign(data, data + size);
	}

private:
	std::deque<AVPacket*>	fPackets;
	std::vector<uint8>		fPrefix;
};


static const char*
x26x_preset(speed_preset speed)
{
	static const char* kPresets[kSpeedCount] = {
		"veryfast", "fast", "medium", "slow", "slower"
	};
	return kPresets[speed];
}


static BString
mastering_string(const AVMasteringDisplayMetadata& mastering)
{
	// x265's G(x,y)B(x,y)R(x,y)WP(x,y)L(max,min): chromaticity in units of
	// 0.00002, luminance in units of 0.0001 cd/m²
	const AVRational* p = &mastering.display_primaries[0][0];
	// FFmpeg's primaries are R, G, B
	BString text;
	text.SetToFormat("G(%d,%d)B(%d,%d)R(%d,%d)WP(%d,%d)L(%d,%d)",
		(int)lround(av_q2d(p[2]) * 50000), (int)lround(av_q2d(p[3]) * 50000),
		(int)lround(av_q2d(p[4]) * 50000), (int)lround(av_q2d(p[5]) * 50000),
		(int)lround(av_q2d(p[0]) * 50000), (int)lround(av_q2d(p[1]) * 50000),
		(int)lround(av_q2d(mastering.white_point[0]) * 50000),
		(int)lround(av_q2d(mastering.white_point[1]) * 50000),
		(int)lround(av_q2d(mastering.max_luminance) * 10000),
		(int)lround(av_q2d(mastering.min_luminance) * 10000));
	return text;
}


// #pragma mark - x264


class X264Encoder : public VideoEncoder {
public:
	X264Encoder()
		:
		fEncoder(NULL),
		fBitDepth(8),
		fDrained(false)
	{
	}

	virtual ~X264Encoder()
	{
		if (fEncoder != NULL)
			x264_library()->encoder_close(fEncoder);
	}

	virtual status_t Open(const VideoEncoderParams& params, BString& error)
	{
		const X264Library* library = x264_library();
		if (library == NULL) {
			error = "x264 is not installed (package x264).";
			return B_NOT_SUPPORTED;
		}
		fBitDepth = video_codec_info(params.codec).bitDepth;

		x264_param_t param;
		if (library->param_default_preset(&param, x26x_preset(params.speed),
				NULL) < 0) {
			error = "x264 does not know the speed preset.";
			return B_ERROR;
		}
		param.i_log_level = X264_LOG_ERROR;
		// x264 cannot count Haiku's processors itself: tell it, as many
		// frame threads as x264 would pick (1.5 per processor)
		int threads = encoder_thread_count();
		param.i_threads = min_c(threads * 3 / 2, 128);
		param.i_lookahead_threads = X264_THREADS_AUTO;
		const char* forced = getenv("MEDIACONVERTER_X264_THREADS");
		if (forced != NULL)
			param.i_threads = atoi(forced);
		param.i_width = params.width;
		param.i_height = params.height;
		param.i_csp = X264_CSP_I420;
		param.i_bitdepth = fBitDepth;
		param.i_fps_num = params.frameRate.num;
		param.i_fps_den = params.frameRate.den;
		param.i_timebase_num = params.timeBase.num;
		param.i_timebase_den = params.timeBase.den;
		param.b_vfr_input = 1;
		param.i_keyint_max = params.keyInterval;
		param.b_annexb = 1;
		param.b_repeat_headers = params.globalHeader ? 0 : 1;
		if (params.sampleAspect.num > 0) {
			param.vui.i_sar_width = params.sampleAspect.num;
			param.vui.i_sar_height = params.sampleAspect.den;
		}
		if (params.primaries != AVCOL_PRI_UNSPECIFIED)
			param.vui.i_colorprim = params.primaries;
		if (params.transfer != AVCOL_TRC_UNSPECIFIED)
			param.vui.i_transfer = params.transfer;
		if (params.matrix != AVCOL_SPC_UNSPECIFIED)
			param.vui.i_colmatrix = params.matrix;
		param.vui.b_fullrange = params.range == AVCOL_RANGE_JPEG;
		if (params.chromaLocation != AVCHROMA_LOC_UNSPECIFIED)
			param.vui.i_chroma_loc = params.chromaLocation - 1;

		if (params.rateMode == kRateConstantQuality) {
			int crf = quality_to_crf(params.quality, 14, 34);
			param.rc.i_rc_method = X264_RC_CRF;
			param.rc.f_rf_constant = crf;
			fDescription.SetToFormat("RF %d, preset %s", crf,
				x26x_preset(params.speed));
		} else {
			param.rc.i_rc_method = X264_RC_ABR;
			param.rc.i_bitrate = params.bitrate;
			fDescription.SetToFormat("%" B_PRId32 " kbit/s, preset %s",
				params.bitrate, x26x_preset(params.speed));
		}

		if (library->param_apply_profile(&param,
				fBitDepth > 8 ? "high10" : "high") < 0) {
			error = "x264 cannot make this profile.";
			return B_ERROR;
		}

		fEncoder = library->encoder_open(&param);
		if (fEncoder == NULL) {
			error = "x264 did not start.";
			return B_ERROR;
		}

		if (params.globalHeader) {
			x264_nal_t* nals;
			int count;
			if (library->encoder_headers(fEncoder, &nals, &count) < 0) {
				error = "x264 gave no headers.";
				return B_ERROR;
			}
			for (int i = 0; i < count; i++) {
				if (nals[i].i_type == NAL_SEI) {
					fQueue.SetPrefix(nals[i].p_payload, nals[i].i_payload);
					continue;
				}
				fExtradata.insert(fExtradata.end(), nals[i].p_payload,
					nals[i].p_payload + nals[i].i_payload);
			}
		}
		return B_OK;
	}

	virtual AVPixelFormat InputFormat() const
	{
		return fBitDepth > 8 ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
	}

	virtual status_t SendFrame(const AVFrame* frame)
	{
		const X264Library* library = x264_library();
		x264_picture_t output;
		library->picture_init(&output);
		x264_nal_t* nals;
		int count;

		if (frame == NULL) {
			while (library->encoder_delayed_frames(fEncoder) > 0) {
				int size = library->encoder_encode(fEncoder, &nals, &count,
					NULL, &output);
				if (size < 0)
					return B_ERROR;
				if (size > 0)
					_Queue(nals, count, size, output);
			}
			fDrained = true;
			return B_OK;
		}

		x264_picture_t input;
		library->picture_init(&input);
		input.img.i_csp = X264_CSP_I420
			| (fBitDepth > 8 ? X264_CSP_HIGH_DEPTH : 0);
		input.img.i_plane = 3;
		for (int i = 0; i < 3; i++) {
			input.img.plane[i] = frame->data[i];
			input.img.i_stride[i] = frame->linesize[i];
		}
		input.i_pts = frame->pts;
		input.i_type = frame->pict_type == AV_PICTURE_TYPE_I
			? X264_TYPE_KEYFRAME : X264_TYPE_AUTO;

		int size = library->encoder_encode(fEncoder, &nals, &count, &input,
			&output);
		if (size < 0)
			return B_ERROR;
		if (size > 0)
			_Queue(nals, count, size, output);
		return B_OK;
	}

	virtual status_t ReceivePacket(AVPacket* packet)
	{
		return fQueue.Take(packet, fDrained);
	}

	virtual encoder_backend Backend() const
	{
		return kBackendSoftware;
	}

	virtual BString Name() const
	{
		return BString("x264");
	}

	virtual BString Description() const
	{
		return fDescription;
	}

private:
	void _Queue(x264_nal_t* nals, int count, int size,
		const x264_picture_t& picture)
	{
		// x264 lays the payloads out one after the other
		AVPacket* packet = fQueue.Add(nals[0].p_payload, size);
		if (packet == NULL)
			return;
		packet->pts = picture.i_pts;
		packet->dts = picture.i_dts;
		if (picture.b_keyframe)
			packet->flags |= AV_PKT_FLAG_KEY;
	}

	x264_t*		fEncoder;
	int			fBitDepth;
	bool		fDrained;
	PacketQueue	fQueue;
	BString		fDescription;
};


// #pragma mark - x265


class X265Encoder : public VideoEncoder {
public:
	X265Encoder()
		:
		fApi(NULL),
		fParam(NULL),
		fEncoder(NULL),
		fPicture(NULL),
		fBitDepth(8),
		fDrained(false)
	{
	}

	virtual ~X265Encoder()
	{
		if (fApi != NULL) {
			if (fEncoder != NULL)
				fApi->encoder_close(fEncoder);
			if (fPicture != NULL)
				fApi->picture_free(fPicture);
			if (fParam != NULL)
				fApi->param_free(fParam);
		}
	}

	virtual status_t Open(const VideoEncoderParams& params, BString& error)
	{
		fBitDepth = video_codec_info(params.codec).bitDepth;
		fApi = x265_library(fBitDepth);
		if (fApi == NULL) {
			if (x265_library(8) == NULL)
				error = "x265 is not installed (package x265).";
			else {
				error.SetToFormat("This x265 cannot encode %d-bit pictures.",
					fBitDepth);
			}
			return B_NOT_SUPPORTED;
		}

		fParam = fApi->param_alloc();
		if (fParam == NULL)
			return B_NO_MEMORY;
		if (fApi->param_default_preset(fParam, x26x_preset(params.speed),
				NULL) < 0) {
			error = "x265 does not know the speed preset.";
			return B_ERROR;
		}

		char value[64];
		fParam->logLevel = X265_LOG_ERROR;
		fParam->sourceWidth = params.width;
		fParam->sourceHeight = params.height;
		fParam->internalCsp = X265_CSP_I420;
		fParam->fpsNum = params.frameRate.num;
		fParam->fpsDenom = params.frameRate.den;
		fParam->bAnnexB = 1;
		fParam->bRepeatHeaders = params.globalHeader ? 0 : 1;
		snprintf(value, sizeof(value), "%d", params.keyInterval);
		fApi->param_parse(fParam, "keyint", value);

		if (params.sampleAspect.num > 0) {
			snprintf(value, sizeof(value), "%d:%d", params.sampleAspect.num,
				params.sampleAspect.den);
			fApi->param_parse(fParam, "sar", value);
		}
		fParam->vui.bEnableVideoSignalTypePresentFlag = 1;
		fParam->vui.videoFormat = 5;
		fParam->vui.bEnableVideoFullRangeFlag
			= params.range == AVCOL_RANGE_JPEG;
		if (params.primaries != AVCOL_PRI_UNSPECIFIED
			|| params.transfer != AVCOL_TRC_UNSPECIFIED
			|| params.matrix != AVCOL_SPC_UNSPECIFIED) {
			fParam->vui.bEnableColorDescriptionPresentFlag = 1;
			fParam->vui.colorPrimaries = params.primaries;
			fParam->vui.transferCharacteristics = params.transfer;
			fParam->vui.matrixCoeffs = params.matrix;
		}
		if (params.chromaLocation > AVCHROMA_LOC_LEFT) {
			fParam->vui.bEnableChromaLocInfoPresentFlag = 1;
			fParam->vui.chromaSampleLocTypeTopField
				= params.chromaLocation - 1;
			fParam->vui.chromaSampleLocTypeBottomField
				= params.chromaLocation - 1;
		}
		if (fBitDepth > 8 && params.transfer == AVCOL_TRC_SMPTE2084) {
			fApi->param_parse(fParam, "hdr10", "1");
			fApi->param_parse(fParam, "hdr10-opt", "1");
			if (params.haveMastering) {
				fApi->param_parse(fParam, "master-display",
					mastering_string(params.mastering).String());
			}
			if (params.haveLightLevel) {
				snprintf(value, sizeof(value), "%u,%u",
					params.lightLevel.MaxCLL, params.lightLevel.MaxFALL);
				fApi->param_parse(fParam, "max-cll", value);
			}
		}

		if (params.rateMode == kRateConstantQuality) {
			int crf = quality_to_crf(params.quality, 14, 34);
			snprintf(value, sizeof(value), "%d", crf);
			fApi->param_parse(fParam, "crf", value);
			fDescription.SetToFormat("RF %d, preset %s", crf,
				x26x_preset(params.speed));
		} else {
			snprintf(value, sizeof(value), "%" B_PRId32, params.bitrate);
			fApi->param_parse(fParam, "bitrate", value);
			fDescription.SetToFormat("%" B_PRId32 " kbit/s, preset %s",
				params.bitrate, x26x_preset(params.speed));
		}

		const char* profile = fBitDepth == 12 ? "main12"
			: fBitDepth == 10 ? "main10" : "main";
		if (fApi->param_apply_profile(fParam, profile) < 0) {
			error = "x265 cannot make this profile.";
			return B_ERROR;
		}

		fEncoder = fApi->encoder_open(fParam);
		if (fEncoder == NULL) {
			error = "x265 did not start.";
			return B_ERROR;
		}
		fPicture = fApi->picture_alloc();
		if (fPicture == NULL)
			return B_NO_MEMORY;
		fApi->picture_init(fParam, fPicture);

		if (params.globalHeader) {
			x265_nal* nals;
			uint32_t count;
			if (fApi->encoder_headers(fEncoder, &nals, &count) < 0) {
				error = "x265 gave no headers.";
				return B_ERROR;
			}
			for (uint32_t i = 0; i < count; i++) {
				if (nals[i].type == NAL_UNIT_PREFIX_SEI
					|| nals[i].type == NAL_UNIT_SUFFIX_SEI) {
					fQueue.SetPrefix(nals[i].payload, nals[i].sizeBytes);
					continue;
				}
				fExtradata.insert(fExtradata.end(), nals[i].payload,
					nals[i].payload + nals[i].sizeBytes);
			}
		}
		return B_OK;
	}

	virtual AVPixelFormat InputFormat() const
	{
		if (fBitDepth == 12)
			return AV_PIX_FMT_YUV420P12LE;
		return fBitDepth == 10 ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
	}

	virtual status_t SendFrame(const AVFrame* frame)
	{
		x265_picture output;
		fApi->picture_init(fParam, &output);
		x265_nal* nals;
		uint32_t count;

		if (frame == NULL) {
			while (true) {
				int result = fApi->encoder_encode(fEncoder, &nals, &count,
					NULL, &output);
				if (result < 0)
					return B_ERROR;
				if (result == 0)
					break;
				_Queue(nals, count, output);
			}
			fDrained = true;
			return B_OK;
		}

		for (int i = 0; i < 3; i++) {
			fPicture->planes[i] = frame->data[i];
			fPicture->stride[i] = frame->linesize[i];
		}
		fPicture->bitDepth = fBitDepth;
		fPicture->colorSpace = X265_CSP_I420;
		fPicture->pts = frame->pts;
		fPicture->sliceType = frame->pict_type == AV_PICTURE_TYPE_I
			? X265_TYPE_IDR : X265_TYPE_AUTO;

		int result = fApi->encoder_encode(fEncoder, &nals, &count, fPicture,
			&output);
		if (result < 0)
			return B_ERROR;
		if (result > 0)
			_Queue(nals, count, output);
		return B_OK;
	}

	virtual status_t ReceivePacket(AVPacket* packet)
	{
		return fQueue.Take(packet, fDrained);
	}

	virtual encoder_backend Backend() const
	{
		return kBackendSoftware;
	}

	virtual BString Name() const
	{
		return BString("x265");
	}

	virtual BString Description() const
	{
		return fDescription;
	}

private:
	void _Queue(x265_nal* nals, uint32_t count, const x265_picture& picture)
	{
		if (count == 0)
			return;
		// x265 keeps the payloads in one buffer too
		size_t size = 0;
		for (uint32_t i = 0; i < count; i++)
			size += nals[i].sizeBytes;
		AVPacket* packet = fQueue.Add(nals[0].payload, size);
		if (packet == NULL)
			return;
		packet->pts = picture.pts;
		packet->dts = picture.dts;
		if (picture.sliceType == X265_TYPE_IDR
			|| picture.sliceType == X265_TYPE_I)
			packet->flags |= AV_PKT_FLAG_KEY;
	}

	const x265_api*	fApi;
	x265_param*		fParam;
	x265_encoder*	fEncoder;
	x265_picture*	fPicture;
	int				fBitDepth;
	bool			fDrained;
	PacketQueue		fQueue;
	BString			fDescription;
};


// #pragma mark - SVT-AV1


class SvtAv1Encoder : public VideoEncoder {
public:
	SvtAv1Encoder()
		:
		fHandle(NULL),
		fInitialized(false),
		fBitDepth(8),
		fSentEnd(false),
		fDrained(false)
	{
	}

	virtual ~SvtAv1Encoder()
	{
		const SvtLibrary* library = svt_library();
		if (fHandle != NULL) {
			if (fInitialized)
				library->deinit(fHandle);
			library->deinit_handle(fHandle);
		}
	}

	virtual status_t Open(const VideoEncoderParams& params, BString& error)
	{
		const SvtLibrary* library = svt_library();
		if (library == NULL) {
			error = "SVT-AV1 is not installed (package svt_av13).";
			return B_NOT_SUPPORTED;
		}
		fBitDepth = video_codec_info(params.codec).bitDepth;

		EbSvtAv1EncConfiguration config;
		memset(&config, 0, sizeof(config));
		if (library->init_handle(&fHandle, &config) != EB_ErrorNone) {
			error = "SVT-AV1 did not start.";
			return B_ERROR;
		}

		static const int kPresets[kSpeedCount] = { 12, 10, 8, 6, 4 };
		config.enc_mode = kPresets[params.speed];
		config.source_width = params.width;
		config.source_height = params.height;
		config.frame_rate_numerator = params.frameRate.num;
		config.frame_rate_denominator = params.frameRate.den;
		config.encoder_bit_depth = fBitDepth;
		config.encoder_color_format = EB_YUV420;
		config.intra_period_length = params.keyInterval - 1;
		if (params.primaries != AVCOL_PRI_UNSPECIFIED)
			config.color_primaries = (EbColorPrimaries)params.primaries;
		if (params.transfer != AVCOL_TRC_UNSPECIFIED) {
			config.transfer_characteristics
				= (EbTransferCharacteristics)params.transfer;
		}
		if (params.matrix != AVCOL_SPC_UNSPECIFIED)
			config.matrix_coefficients = (EbMatrixCoefficients)params.matrix;
		config.color_range = params.range == AVCOL_RANGE_JPEG
			? EB_CR_FULL_RANGE : EB_CR_STUDIO_RANGE;
		if (params.chromaLocation == AVCHROMA_LOC_LEFT)
			config.chroma_sample_position = EB_CSP_VERTICAL;
		else if (params.chromaLocation == AVCHROMA_LOC_TOPLEFT)
			config.chroma_sample_position = EB_CSP_COLOCATED;
		if (params.haveMastering) {
			// AV1's metadata units (chromaticity 0.16, luminance 24.8 and
			// 18.14 fixed point), stored big-endian as SVT-AV1 wants them
			const AVMasteringDisplayMetadata& m = params.mastering;
			struct EbSvtAv1ChromaPoints* points[3] = {
				&config.mastering_display.r, &config.mastering_display.g,
				&config.mastering_display.b
			};
			for (int i = 0; i < 3; i++) {
				points[i]->x = B_HOST_TO_BENDIAN_INT16((uint16)lround(
					av_q2d(m.display_primaries[i][0]) * (1 << 16)));
				points[i]->y = B_HOST_TO_BENDIAN_INT16((uint16)lround(
					av_q2d(m.display_primaries[i][1]) * (1 << 16)));
			}
			config.mastering_display.white_point.x = B_HOST_TO_BENDIAN_INT16(
				(uint16)lround(av_q2d(m.white_point[0]) * (1 << 16)));
			config.mastering_display.white_point.y = B_HOST_TO_BENDIAN_INT16(
				(uint16)lround(av_q2d(m.white_point[1]) * (1 << 16)));
			config.mastering_display.max_luma = B_HOST_TO_BENDIAN_INT32(
				(uint32)lround(av_q2d(m.max_luminance) * (1 << 8)));
			config.mastering_display.min_luma = B_HOST_TO_BENDIAN_INT32(
				(uint32)lround(av_q2d(m.min_luminance) * (1 << 14)));
		}
		if (params.haveLightLevel) {
			config.content_light_level.max_cll = B_HOST_TO_BENDIAN_INT16(
				(uint16)params.lightLevel.MaxCLL);
			config.content_light_level.max_fall = B_HOST_TO_BENDIAN_INT16(
				(uint16)params.lightLevel.MaxFALL);
		}

		if (params.rateMode == kRateConstantQuality) {
			int crf = quality_to_crf(params.quality, 18, 48);
			config.rate_control_mode = 0;
			config.qp = crf;
			fDescription.SetToFormat("CRF %d, preset %d", crf,
				config.enc_mode);
		} else {
			config.rate_control_mode = 1;
			config.target_bit_rate = params.bitrate * 1000;
			fDescription.SetToFormat("%" B_PRId32 " kbit/s, preset %d",
				params.bitrate, config.enc_mode);
		}

		if (library->set_parameter(fHandle, &config) != EB_ErrorNone) {
			error = "SVT-AV1 refused the settings.";
			return B_ERROR;
		}
		if (library->init(fHandle) != EB_ErrorNone) {
			error = "SVT-AV1 could not set up the encoder.";
			return B_ERROR;
		}
		fInitialized = true;

		EbBufferHeaderType* header = NULL;
		if (library->stream_header(fHandle, &header) == EB_ErrorNone
			&& header != NULL) {
			fExtradata.assign(header->p_buffer,
				header->p_buffer + header->n_filled_len);
			library->stream_header_release(header);
		}
		return B_OK;
	}

	virtual AVPixelFormat InputFormat() const
	{
		return fBitDepth > 8 ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
	}

	virtual status_t SendFrame(const AVFrame* frame)
	{
		const SvtLibrary* library = svt_library();
		EbBufferHeaderType buffer;
		memset(&buffer, 0, sizeof(buffer));
		buffer.size = sizeof(buffer);

		if (frame == NULL) {
			if (!fSentEnd) {
				buffer.flags = EB_BUFFERFLAG_EOS;
				buffer.pic_type = EB_AV1_INVALID_PICTURE;
				if (library->send_picture(fHandle, &buffer) != EB_ErrorNone)
					return B_ERROR;
				fSentEnd = true;
			}
			return B_OK;
		}

		EbSvtIOFormat picture;
		memset(&picture, 0, sizeof(picture));
		picture.luma = frame->data[0];
		picture.cb = frame->data[1];
		picture.cr = frame->data[2];
		// strides are in samples
		int bytes = fBitDepth > 8 ? 2 : 1;
		picture.y_stride = frame->linesize[0] / bytes;
		picture.cb_stride = frame->linesize[1] / bytes;
		picture.cr_stride = frame->linesize[2] / bytes;

		buffer.p_buffer = (uint8_t*)&picture;
		buffer.n_filled_len = (frame->linesize[0] * frame->height
			+ (frame->linesize[1] + frame->linesize[2])
				* ((frame->height + 1) / 2));
		buffer.n_alloc_len = buffer.n_filled_len;
		buffer.pts = frame->pts;
		buffer.pic_type = frame->pict_type == AV_PICTURE_TYPE_I
			? EB_AV1_KEY_PICTURE : EB_AV1_INVALID_PICTURE;
		if (library->send_picture(fHandle, &buffer) != EB_ErrorNone)
			return B_ERROR;
		return B_OK;
	}

	virtual status_t ReceivePacket(AVPacket* packet)
	{
		if (fDrained)
			return B_LAST_BUFFER_ERROR;
		const SvtLibrary* library = svt_library();
		EbBufferHeaderType* output = NULL;
		EbErrorType result = library->get_packet(fHandle, &output,
			fSentEnd ? 1 : 0);
		if (result == EB_NoErrorEmptyQueue)
			return fSentEnd ? B_LAST_BUFFER_ERROR : B_WOULD_BLOCK;
		if (result != EB_ErrorNone || output == NULL)
			return B_ERROR;

		bool last = (output->flags & EB_BUFFERFLAG_EOS) != 0;
		status_t status = B_OK;
		if (output->n_filled_len > 0) {
			if (av_new_packet(packet, output->n_filled_len) < 0)
				status = B_NO_MEMORY;
			else {
				memcpy(packet->data, output->p_buffer, output->n_filled_len);
				packet->pts = output->pts;
				packet->dts = output->dts;
				if (output->pic_type == EB_AV1_KEY_PICTURE)
					packet->flags |= AV_PKT_FLAG_KEY;
			}
		} else
			status = last ? B_LAST_BUFFER_ERROR : B_WOULD_BLOCK;
		library->release_out_buffer(&output);
		if (last)
			fDrained = true;
		return status;
	}

	virtual encoder_backend Backend() const
	{
		return kBackendSoftware;
	}

	virtual BString Name() const
	{
		return BString("SVT-AV1");
	}

	virtual BString Description() const
	{
		return fDescription;
	}

private:
	EbComponentType*	fHandle;
	bool				fInitialized;
	int					fBitDepth;
	bool				fSentEnd;
	bool				fDrained;
	BString				fDescription;
};


// #pragma mark - SoftwareEncoders


bool
SoftwareEncoders::IsAvailable(video_codec codec, BString* missing)
{
	const VideoCodecInfo& info = video_codec_info(codec);
	switch (info.software) {
		case kSoftwareX264:
			if (x264_library() != NULL)
				return true;
			if (missing != NULL)
				*missing = "x264";
			return false;
		case kSoftwareX265:
			if (x265_library(info.bitDepth) != NULL)
				return true;
			if (missing != NULL) {
				if (x265_library(8) == NULL)
					*missing = "x265";
				else
					missing->SetToFormat("x265 with %d-bit support",
						info.bitDepth);
			}
			return false;
		case kSoftwareSvtAv1:
			if (svt_library() != NULL)
				return true;
			if (missing != NULL)
				*missing = "svt_av13";
			return false;
		default:
			return false;
	}
}


VideoEncoder*
SoftwareEncoders::Create(video_codec codec)
{
	switch (video_codec_info(codec).software) {
		case kSoftwareX264:
			return new(std::nothrow) X264Encoder;
		case kSoftwareX265:
			return new(std::nothrow) X265Encoder;
		case kSoftwareSvtAv1:
			return new(std::nothrow) SvtAv1Encoder;
		default:
			return NULL;
	}
}


BString
SoftwareEncoders::Version(software_encoder encoder)
{
	BString version;
	switch (encoder) {
		case kSoftwareX264:
			if (x264_library() != NULL)
				version.SetToFormat("x264 %s", X264_POINTVER);
			break;
		case kSoftwareX265:
		{
			const x265_api* api = x265_library(8);
			if (api != NULL) {
				version.SetToFormat("x265 %s", api->version_str);
				BString depths;
				for (int depth = 8; depth <= 12; depth += 2) {
					if (x265_library(depth) != NULL) {
						if (!depths.IsEmpty())
							depths << "/";
						depths << depth;
					}
				}
				version << " (" << depths << "-bit)";
			}
			break;
		}
		case kSoftwareSvtAv1:
		{
			const SvtLibrary* library = svt_library();
			if (library != NULL)
				version.SetToFormat("SVT-AV1 %s", library->get_version());
			break;
		}
		default:
			break;
	}
	return version;
}
