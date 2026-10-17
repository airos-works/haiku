/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "VaapiEncoder.h"

#include <math.h>
#include <stdlib.h>
#include <OS.h>
#include <stdio.h>
#include <string.h>

extern "C" {
#include <libavutil/pixdesc.h>
}

#include "BitWriter.h"


static bool sProfile = getenv("MEDIACONVERTER_PROFILE") != NULL;
static bigtime_t sUploadTime, sIssueTime, sSyncTime;
static int64 sUploads;
static bool sDerived;


#define ALIGN(value, alignment) \
	(((value) + (alignment) - 1) / (alignment) * (alignment))


static const struct {
	int	num;
	int	den;
} kPixelAspects[] = {
	{ 0, 1 }, { 1, 1 }, { 12, 11 }, { 10, 11 }, { 16, 11 }, { 40, 33 },
	{ 24, 11 }, { 20, 11 }, { 32, 11 }, { 80, 33 }, { 18, 11 }, { 15, 11 },
	{ 64, 33 }, { 160, 99 }, { 4, 3 }, { 3, 2 }, { 2, 1 },
};


static int
aspect_ratio_idc(AVRational aspect, int* sarWidth, int* sarHeight)
{
	int num, den;
	av_reduce(&num, &den, aspect.num, aspect.den, 65535);
	for (size_t i = 1; i < sizeof(kPixelAspects) / sizeof(kPixelAspects[0]);
			i++) {
		if (kPixelAspects[i].num == num && kPixelAspects[i].den == den)
			return (int)i;
	}
	*sarWidth = num;
	*sarHeight = den;
	return 255;
}


VaapiEncoder::VaapiEncoder(const char* renderNode, const char* deviceName)
	:
	fRenderNode(renderNode),
	fDeviceName(deviceName),
	fVA(NULL),
	fCodec(AV_CODEC_ID_NONE),
	fBitDepth(8),
	fProfile(VAProfileNone),
	fEntrypoint(VAEntrypointEncSlice),
	fRTFormat(VA_RT_FORMAT_YUV420),
	fRateControl(VA_RC_CQP),
	fPackedHeaders(0),
	fQualityRange(0),
	fMesa(false),
	fConfig(VA_INVALID_ID),
	fContext(VA_INVALID_ID),
	fAlignedWidth(0),
	fAlignedHeight(0),
	fBlockSize(16),
	fMinBlockSize(8),
	fHevcFeatures(0),
	fHevcBlockSizes(0),
	fQP(24),
	fQPIntra(23),
	fDeriveWorks(true),
	fHaveReference(false),
	fReference(VA_INVALID_SURFACE),
	fReferenceFrameNum(0),
	fReferenceOrder(0),
	fReferenceSlot(0),
	fCount(0),
	fIndexInGop(0),
	fIdrId(0),
	fDrained(false)
{
	memset(&fImageFormat, 0, sizeof(fImageFormat));
}


VaapiEncoder::~VaapiEncoder()
{
	if (sProfile && sUploads > 0) {
		fprintf(stderr, "vaapi: %" B_PRId64 " pictures, upload %.3f ms (%s), "
			"issue %.3f ms, sync %.3f ms\n", sUploads,
			sUploadTime / 1e3 / sUploads, sDerived ? "derived" : "put image",
			sIssueTime / 1e3 / sUploads, sSyncTime / 1e3 / sUploads);
	}
	if (fVA != NULL && fDisplay.Display() != NULL) {
		VADisplay display = fDisplay.Display();
		for (size_t i = 0; i < fInFlight.size(); i++) {
			fVA->SyncSurface(display, fInFlight[i].input);
			_ReleaseBuffers(fInFlight[i]);
		}
		for (size_t i = 0; i < fAllCoded.size(); i++)
			fVA->DestroyBuffer(display, fAllCoded[i]);
		if (fContext != VA_INVALID_ID)
			fVA->DestroyContext(display, fContext);
		if (!fAllSurfaces.empty()) {
			fVA->DestroySurfaces(display, fAllSurfaces.data(),
				(int)fAllSurfaces.size());
		}
		if (fConfig != VA_INVALID_ID)
			fVA->DestroyConfig(display, fConfig);
	}
	while (!fOutput.empty()) {
		av_packet_free(&fOutput.front());
		fOutput.pop_front();
	}
}


status_t
VaapiEncoder::_ChooseProfile(BString& error)
{
	VADisplay display = fDisplay.Display();
	int count = fVA->MaxNumProfiles(display);
	std::vector<VAProfile> profiles(count);
	if (fVA->QueryConfigProfiles(display, profiles.data(), &count)
			!= VA_STATUS_SUCCESS)
		count = 0;
	profiles.resize(count);

	VAProfile wanted[3] = { VAProfileNone, VAProfileNone, VAProfileNone };
	switch (fCodec) {
		case AV_CODEC_ID_H264:
			if (fBitDepth == 8) {
				wanted[0] = VAProfileH264High;
				wanted[1] = VAProfileH264Main;
				wanted[2] = VAProfileH264ConstrainedBaseline;
			}
			break;
		case AV_CODEC_ID_HEVC:
			wanted[0] = fBitDepth == 10 ? VAProfileHEVCMain10
				: fBitDepth == 8 ? VAProfileHEVCMain : VAProfileNone;
			break;
		case AV_CODEC_ID_AV1:
			wanted[0] = VAProfileAV1Profile0;
			break;
		default:
			break;
	}

	int entryCount = fVA->MaxNumEntrypoints(display);
	std::vector<VAEntrypoint> entries(entryCount);
	for (int w = 0; w < 3; w++) {
		if (wanted[w] == VAProfileNone)
			continue;
		bool listed = false;
		for (size_t i = 0; i < profiles.size(); i++)
			listed |= profiles[i] == wanted[w];
		if (!listed)
			continue;
		int n = entryCount;
		if (fVA->QueryConfigEntrypoints(display, wanted[w], entries.data(),
				&n) != VA_STATUS_SUCCESS)
			continue;
		VAEntrypoint found = (VAEntrypoint)0;
		for (int i = 0; i < n; i++) {
			if (entries[i] == VAEntrypointEncSlice)
				found = VAEntrypointEncSlice;
			else if (entries[i] == VAEntrypointEncSliceLP && found == 0)
				found = VAEntrypointEncSliceLP;
		}
		if (found == 0)
			continue;
		fProfile = wanted[w];
		fEntrypoint = found;
		return B_OK;
	}
	error.SetToFormat("%s cannot encode %s here.", fDeviceName.String(),
		video_codec_info(fParams.codec).name);
	return B_NOT_SUPPORTED;
}


status_t
VaapiEncoder::_CreateSession(BString& error)
{
	VADisplay display = fDisplay.Display();

	VAConfigAttrib attributes[] = {
		{ VAConfigAttribRTFormat, 0 },
		{ VAConfigAttribRateControl, 0 },
		{ VAConfigAttribEncPackedHeaders, 0 },
		{ VAConfigAttribEncQualityRange, 0 },
		{ VAConfigAttribMaxPictureWidth, 0 },
		{ VAConfigAttribMaxPictureHeight, 0 },
		{ VAConfigAttribEncHEVCFeatures, 0 },
		{ VAConfigAttribEncHEVCBlockSizes, 0 },
	};
	const int attributeCount = sizeof(attributes) / sizeof(attributes[0]);
	if (fVA->GetConfigAttributes(display, fProfile, fEntrypoint, attributes,
			attributeCount) != VA_STATUS_SUCCESS) {
		error = "The GPU's encoder has no attributes.";
		return B_ERROR;
	}

	uint32 wantedFormat = fBitDepth > 8 ? VA_RT_FORMAT_YUV420_10
		: VA_RT_FORMAT_YUV420;
	if (attributes[0].value == VA_ATTRIB_NOT_SUPPORTED
		|| (attributes[0].value & wantedFormat) == 0) {
		error.SetToFormat("%s cannot encode %d-bit pictures.",
			fDeviceName.String(), fBitDepth);
		return B_NOT_SUPPORTED;
	}
	fRTFormat = wantedFormat;

	uint32 modes = attributes[1].value == VA_ATTRIB_NOT_SUPPORTED
		? VA_RC_CQP : attributes[1].value;
	if (fParams.rateMode == kRateConstantQuality) {
		if ((modes & VA_RC_CQP) != 0)
			fRateControl = VA_RC_CQP;
		else if ((modes & VA_RC_ICQ) != 0)
			fRateControl = VA_RC_ICQ;
		else if ((modes & VA_RC_QVBR) != 0)
			fRateControl = VA_RC_QVBR;
		else
			fRateControl = (modes & VA_RC_VBR) != 0 ? VA_RC_VBR : VA_RC_CBR;
	} else {
		fRateControl = (modes & VA_RC_VBR) != 0 ? VA_RC_VBR
			: (modes & VA_RC_CBR) != 0 ? VA_RC_CBR : VA_RC_CQP;
	}

	uint32 packed = attributes[2].value == VA_ATTRIB_NOT_SUPPORTED
		? 0 : attributes[2].value;
	fPackedHeaders = packed & (VA_ENC_PACKED_HEADER_SEQUENCE
		| VA_ENC_PACKED_HEADER_SLICE | VA_ENC_PACKED_HEADER_PICTURE
		| VA_ENC_PACKED_HEADER_RAW_DATA);
	if (fCodec != AV_CODEC_ID_AV1)
		fPackedHeaders &= ~VA_ENC_PACKED_HEADER_PICTURE;
	fQualityRange = attributes[3].value == VA_ATTRIB_NOT_SUPPORTED
		? 0 : attributes[3].value;

	if (attributes[4].value != VA_ATTRIB_NOT_SUPPORTED
		&& (uint32)fParams.width > attributes[4].value) {
		error.SetToFormat("%s encodes at most %u pixels across.",
			fDeviceName.String(), attributes[4].value);
		return B_NOT_SUPPORTED;
	}
	if (attributes[5].value != VA_ATTRIB_NOT_SUPPORTED
		&& (uint32)fParams.height > attributes[5].value) {
		error.SetToFormat("%s encodes at most %u lines.",
			fDeviceName.String(), attributes[5].value);
		return B_NOT_SUPPORTED;
	}

	if (fCodec == AV_CODEC_ID_HEVC) {
		fHevcFeatures = attributes[6].value == VA_ATTRIB_NOT_SUPPORTED
			? 0 : attributes[6].value;
		fHevcBlockSizes = attributes[7].value == VA_ATTRIB_NOT_SUPPORTED
			? 0 : attributes[7].value;
		if (fHevcBlockSizes != 0) {
			VAConfigAttribValEncHEVCBlockSizes sizes;
			sizes.value = fHevcBlockSizes;
			fBlockSize = 1 << (sizes.bits.log2_max_coding_tree_block_size_minus3
				+ 3);
			fMinBlockSize = 1
				<< (sizes.bits.log2_min_luma_coding_block_size_minus3 + 3);
		} else {
			fBlockSize = 32;
			fMinBlockSize = 16;
		}
	} else if (fCodec == AV_CODEC_ID_AV1) {
		fBlockSize = 64;
		fMinBlockSize = 8;
	} else {
		fBlockSize = 16;
		fMinBlockSize = 16;
	}

	VAConfigAttrib config[3];
	int configCount = 0;
	config[configCount].type = VAConfigAttribRTFormat;
	config[configCount++].value = fRTFormat;
	config[configCount].type = VAConfigAttribRateControl;
	config[configCount++].value = fRateControl;
	if (fPackedHeaders != 0) {
		config[configCount].type = VAConfigAttribEncPackedHeaders;
		config[configCount++].value = fPackedHeaders;
	}
	VAStatus status = fVA->CreateConfig(display, fProfile, fEntrypoint, config,
		configCount, &fConfig);
	if (status != VA_STATUS_SUCCESS) {
		error.SetToFormat("The GPU refused the encoder setup: %s",
			fVA->ErrorStr(status));
		return B_ERROR;
	}
	_Align();

	// surfaces: the pictures waiting to be encoded, and the reconstructed
	// pictures the encoder predicts from
	uint32 fourcc = fBitDepth > 8 ? VA_FOURCC_P010 : VA_FOURCC_NV12;
	VASurfaceAttrib surfaceAttribute;
	surfaceAttribute.type = VASurfaceAttribPixelFormat;
	surfaceAttribute.flags = VA_SURFACE_ATTRIB_SETTABLE;
	surfaceAttribute.value.type = VAGenericValueTypeInteger;
	surfaceAttribute.value.value.i = (int)fourcc;

	const int inputCount = kPipelineDepth + 1;
	const int reconCount = kPipelineDepth + 2;
	std::vector<VASurfaceID> surfaces(inputCount + reconCount);
	status = fVA->CreateSurfaces(display, fRTFormat, fAlignedWidth,
		fAlignedHeight, surfaces.data(), (unsigned)surfaces.size(),
		&surfaceAttribute, 1);
	if (status != VA_STATUS_SUCCESS) {
		error.SetToFormat("The GPU has no memory for the pictures: %s",
			fVA->ErrorStr(status));
		return B_NO_MEMORY;
	}
	fAllSurfaces = surfaces;
	fFreeInputs.assign(surfaces.begin(), surfaces.begin() + inputCount);
	fFreeRecons.assign(surfaces.begin() + inputCount, surfaces.end());

	status = fVA->CreateContext(display, fConfig, fAlignedWidth,
		fAlignedHeight, VA_PROGRESSIVE, fFreeRecons.data(),
		(int)fFreeRecons.size(), &fContext);
	if (status != VA_STATUS_SUCCESS) {
		error.SetToFormat("The GPU's encoder did not start: %s",
			fVA->ErrorStr(status));
		return B_ERROR;
	}

	size_t codedSize = (size_t)fAlignedWidth * fAlignedHeight
		* (fBitDepth > 8 ? 3 : 2) + (1 << 20);
	for (int i = 0; i < kPipelineDepth + 1; i++) {
		VABufferID buffer;
		status = fVA->CreateBuffer(display, fContext, VAEncCodedBufferType,
			(unsigned)codedSize, 1, NULL, &buffer);
		if (status != VA_STATUS_SUCCESS) {
			error = "The GPU has no memory for the encoded pictures.";
			return B_NO_MEMORY;
		}
		fAllCoded.push_back(buffer);
		fFreeCoded.push_back(buffer);
	}

	fImageFormat.fourcc = fourcc;
	fImageFormat.byte_order = VA_LSB_FIRST;
	fImageFormat.bits_per_pixel = fBitDepth > 8 ? 24 : 12;
	return B_OK;
}


status_t
VaapiEncoder::Open(const VideoEncoderParams& params, BString& error)
{
	fParams = params;
	const VideoCodecInfo& info = video_codec_info(params.codec);
	fCodec = info.avCodec;
	fBitDepth = info.bitDepth;
	if (fCodec != AV_CODEC_ID_H264 && fCodec != AV_CODEC_ID_HEVC
		&& fCodec != AV_CODEC_ID_AV1) {
		error.SetToFormat("VA-API encoding of %s is not implemented.",
			info.name);
		return B_NOT_SUPPORTED;
	}

	status_t status = fDisplay.Open(fRenderNode.String(), error);
	if (status != B_OK)
		return status;
	fVA = fDisplay.Library();
	fMesa = fDisplay.Vendor().IFindFirst("Mesa") >= 0;

	status = _ChooseProfile(error);
	if (status == B_OK)
		status = _CreateSession(error);
	if (status != B_OK)
		return status;

	// QP for the constant-quality mode, from the same scale as the
	// software encoders' rate factor
	if (fCodec == AV_CODEC_ID_AV1) {
		// AV1's quantizer index, 0..255
		fQP = quality_to_crf(params.quality, 60, 180);
		fQPIntra = max_c(fQP - 8, 1);
	} else {
		fQP = quality_to_crf(params.quality, 16, 36);
		fQPIntra = max_c(fQP - 1, 1);
	}

	const char* profileName = "";
	switch (fProfile) {
		case VAProfileH264High:
			profileName = "High";
			break;
		case VAProfileH264Main:
			profileName = "Main";
			break;
		case VAProfileH264ConstrainedBaseline:
			profileName = "Constrained Baseline";
			break;
		case VAProfileHEVCMain:
			profileName = "Main";
			break;
		case VAProfileHEVCMain10:
			profileName = "Main 10";
			break;
		case VAProfileAV1Profile0:
			profileName = "Main";
			break;
		default:
			break;
	}
	if (fRateControl == VA_RC_CQP) {
		fDescription.SetToFormat(fCodec == AV_CODEC_ID_AV1
			? "%s profile, constant quantizer index %d"
			: "%s profile, constant QP %d", profileName, fQP);
	} else {
		fDescription.SetToFormat("%s profile, %s %" B_PRId32 " kbit/s",
			profileName, fRateControl == VA_RC_CBR ? "CBR" : "VBR",
			params.bitrate);
	}

	if (fCodec == AV_CODEC_ID_H264)
		_WriteH264Headers(fExtradata);
	else if (fCodec == AV_CODEC_ID_HEVC)
		_WriteHevcHeaders(fExtradata);
	else
		_WriteAv1SequenceHeader(fExtradata);
	return _PrimeExtradata();
}


AVPixelFormat
VaapiEncoder::InputFormat() const
{
	return fBitDepth > 8 ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
}


/*!	The coded size: the codec's own block, and what the driver says its
	surfaces need (Mesa's radeonsi: 64 x 16 for H.265 and AV1). The part
	below and right of the picture is cut off again by the parameter sets'
	cropping. */
void
VaapiEncoder::_Align()
{
	int alignX = fMinBlockSize;
	int alignY = fMinBlockSize;
	unsigned count = 0;
	if (fVA->QuerySurfaceAttributes(fDisplay.Display(), fConfig, NULL, &count)
			== VA_STATUS_SUCCESS && count > 0) {
		std::vector<VASurfaceAttrib> attributes(count);
		if (fVA->QuerySurfaceAttributes(fDisplay.Display(), fConfig,
				attributes.data(), &count) == VA_STATUS_SUCCESS) {
			for (unsigned i = 0; i < count; i++) {
				if (attributes[i].type != VASurfaceAttribAlignmentSize)
					continue;
				VASurfaceAttribAlignmentStruct alignment;
				alignment.value = (uint32)attributes[i].value.value.i;
				alignX = max_c(alignX,
					1 << alignment.bits.log2_width_alignment);
				alignY = max_c(alignY,
					1 << alignment.bits.log2_height_alignment);
			}
		}
	}
	fAlignedWidth = ALIGN(fParams.width, alignX);
	fAlignedHeight = ALIGN(fParams.height, alignY);
}


/*!	Drivers may write parameter sets of their own (Mesa does, from what it
	parsed of ours and what the encoder really does): the container's copy
	must be what is in the stream. One gray picture is encoded and thrown
	away, and the stream starts over. */
status_t
VaapiEncoder::_PrimeExtradata()
{
	AVFrame* frame = av_frame_alloc();
	if (frame == NULL)
		return B_NO_MEMORY;
	frame->format = InputFormat();
	frame->width = fParams.width;
	frame->height = fParams.height;
	if (av_frame_get_buffer(frame, 0) < 0) {
		av_frame_free(&frame);
		return B_NO_MEMORY;
	}
	int bytes = fBitDepth > 8 ? 2 : 1;
	for (int y = 0; y < frame->height; y++) {
		uint8* line = frame->data[0] + (size_t)y * frame->linesize[0];
		for (int x = 0; x < frame->width * bytes; x += bytes) {
			if (bytes == 2) {
				uint16 value = 512 << 6;
				memcpy(line + x, &value, 2);
			} else
				line[x] = 128;
		}
	}
	for (int y = 0; y < (frame->height + 1) / 2; y++) {
		uint8* line = frame->data[1] + (size_t)y * frame->linesize[1];
		for (int x = 0; x < frame->width * bytes; x += bytes) {
			if (bytes == 2) {
				uint16 value = 512 << 6;
				memcpy(line + x, &value, 2);
			} else
				line[x] = 128;
		}
	}
	frame->pts = 0;

	status_t status = SendFrame(frame);
	av_frame_free(&frame);
	if (status == B_OK)
		status = SendFrame(NULL);

	AVPacket* packet = NULL;
	if (!fOutput.empty()) {
		packet = fOutput.front();
		fOutput.pop_front();
	}
	while (!fOutput.empty()) {
		av_packet_free(&fOutput.front());
		fOutput.pop_front();
	}

	if (packet != NULL && fCodec == AV_CODEC_ID_AV1) {
		// keep the sequence header OBU
		const uint8* data = packet->data;
		size_t size = packet->size;
		size_t offset = 0;
		while (offset < size) {
			uint8 header = data[offset];
			int type = (header >> 3) & 0xf;
			bool extension = (header >> 2) & 1;
			bool hasSize = (header >> 1) & 1;
			size_t at = offset + 1 + (extension ? 1 : 0);
			uint64 payload = size - at;
			if (hasSize) {
				payload = 0;
				for (int i = 0; i < 8 && at < size; i++) {
					uint8 byte = data[at++];
					payload |= (uint64)(byte & 0x7f) << (7 * i);
					if ((byte & 0x80) == 0)
						break;
				}
			}
			size_t end = at + payload;
			if (end > size)
				break;
			if (type == 1) {
				fExtradata.assign(data + offset, data + end);
				break;
			}
			offset = end;
		}
		av_packet_free(&packet);
	} else if (packet != NULL) {
		// keep the parameter sets of the picture, in Annex B
		std::vector<uint8> sets;
		const uint8* data = packet->data;
		size_t size = packet->size;
		size_t i = 0;
		std::vector<size_t> starts;
		while (i + 3 <= size) {
			if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
				starts.push_back(i + 3);
				i += 3;
			} else
				i++;
		}
		for (size_t n = 0; n < starts.size(); n++) {
			size_t begin = starts[n];
			size_t end = n + 1 < starts.size() ? starts[n + 1] - 3 : size;
			while (end > begin && data[end - 1] == 0)
				end--;
			if (begin >= end)
				continue;
			int type = fCodec == AV_CODEC_ID_HEVC ? (data[begin] >> 1) & 0x3f
				: data[begin] & 0x1f;
			bool keep = fCodec == AV_CODEC_ID_HEVC
				? type >= 32 && type <= 34 : type == 7 || type == 8;
			if (!keep)
				continue;
			static const uint8 kStart[4] = { 0, 0, 0, 1 };
			sets.insert(sets.end(), kStart, kStart + 4);
			sets.insert(sets.end(), data + begin, data + end);
		}
		if (!sets.empty())
			fExtradata = sets;
		av_packet_free(&packet);
	}

	// start over: the next picture is the first IDR again
	if (fHaveReference)
		fFreeRecons.push_back(fReference);
	fHaveReference = false;
	fReference = VA_INVALID_SURFACE;
	fCount = 0;
	fIndexInGop = 0;
	fIdrId = 0;
	fDrained = false;
	return status;
}


// #pragma mark - upload


status_t
VaapiEncoder::_Upload(const AVFrame* frame, VASurfaceID surface)
{
	VADisplay display = fDisplay.Display();
	VAImage image;
	memset(&image, 0, sizeof(image));
	image.image_id = VA_INVALID_ID;
	bool derived = false;
	if (fDeriveWorks) {
		if (fVA->DeriveImage(display, surface, &image) == VA_STATUS_SUCCESS
			&& image.format.fourcc == fImageFormat.fourcc) {
			derived = true;
			sDerived = true;
		}
		else {
			if (image.image_id != VA_INVALID_ID)
				fVA->DestroyImage(display, image.image_id);
			fDeriveWorks = false;
		}
	}
	if (!derived) {
		if (fVA->CreateImage(display, &fImageFormat, fAlignedWidth,
				fAlignedHeight, &image) != VA_STATUS_SUCCESS)
			return B_ERROR;
	}

	void* mapped = NULL;
	if (fVA->MapBuffer(display, image.buf, &mapped) != VA_STATUS_SUCCESS) {
		fVA->DestroyImage(display, image.image_id);
		return B_ERROR;
	}

	int bytes = fBitDepth > 8 ? 2 : 1;
	uint8* base = (uint8*)mapped;
	// luma, then the interleaved chroma plane, which the frame already is
	for (int plane = 0; plane < 2; plane++) {
		int lines = plane == 0 ? frame->height : (frame->height + 1) / 2;
		int alignedLines = plane == 0 ? fAlignedHeight : fAlignedHeight / 2;
		size_t rowBytes = (size_t)ALIGN(frame->width, 2) * bytes;
		uint8* target = base + image.offsets[plane];
		const uint8* source = frame->data[plane];
		for (int y = 0; y < lines; y++) {
			memcpy(target, source, rowBytes);
			// repeat the last column into the alignment
			size_t pad = (size_t)fAlignedWidth * bytes - rowBytes;
			if (pad > 0) {
				size_t unit = plane == 0 ? bytes : 2 * bytes;
				const uint8* last = target + rowBytes - unit;
				for (size_t x = 0; x < pad; x += unit)
					memcpy(target + rowBytes + x, last, unit);
			}
			target += image.pitches[plane];
			source += frame->linesize[plane];
		}
		// and the last line into the lines below the picture
		const uint8* lastLine = target - image.pitches[plane];
		for (int y = lines; y < alignedLines; y++) {
			memcpy(target, lastLine, (size_t)fAlignedWidth * bytes);
			target += image.pitches[plane];
		}
	}

	fVA->UnmapBuffer(display, image.buf);
	status_t status = B_OK;
	if (!derived) {
		if (fVA->PutImage(display, surface, image.image_id, 0, 0,
				fAlignedWidth, fAlignedHeight, 0, 0, fAlignedWidth,
				fAlignedHeight) != VA_STATUS_SUCCESS)
			status = B_ERROR;
	}
	fVA->DestroyImage(display, image.image_id);
	return status;
}


// #pragma mark - buffers


status_t
VaapiEncoder::_AddBuffer(Picture& picture, VABufferType type,
	const void* data, size_t size)
{
	VABufferID buffer;
	VAStatus status = fVA->CreateBuffer(fDisplay.Display(), fContext, type,
		(unsigned)size, 1, (void*)data, &buffer);
	if (status != VA_STATUS_SUCCESS)
		return B_ERROR;
	picture.buffers.push_back(buffer);
	return B_OK;
}


status_t
VaapiEncoder::_AddMisc(Picture& picture, VAEncMiscParameterType type,
	const void* data, size_t size)
{
	std::vector<uint8> buffer(sizeof(VAEncMiscParameterBuffer) + size);
	VAEncMiscParameterBuffer* misc = (VAEncMiscParameterBuffer*)buffer.data();
	misc->type = type;
	memcpy(misc->data, data, size);
	return _AddBuffer(picture, VAEncMiscParameterBufferType, buffer.data(),
		buffer.size());
}


status_t
VaapiEncoder::_AddPacked(Picture& picture, VAEncPackedHeaderType type,
	const std::vector<uint8>& data, size_t bits, bool emulationBytes)
{
	VAEncPackedHeaderParameterBuffer parameters;
	memset(&parameters, 0, sizeof(parameters));
	parameters.type = type;
	parameters.bit_length = (uint32)bits;
	parameters.has_emulation_bytes = emulationBytes;
	if (_AddBuffer(picture, VAEncPackedHeaderParameterBufferType, &parameters,
			sizeof(parameters)) != B_OK)
		return B_ERROR;
	return _AddBuffer(picture, VAEncPackedHeaderDataBufferType, data.data(),
		(bits + 7) / 8);
}


void
VaapiEncoder::_ReleaseBuffers(Picture& picture)
{
	for (size_t i = 0; i < picture.buffers.size(); i++)
		fVA->DestroyBuffer(fDisplay.Display(), picture.buffers[i]);
	picture.buffers.clear();
}


status_t
VaapiEncoder::_AddRateControl(Picture& picture)
{
	// frame rate, as num/den in 16 bits each when they fit
	VAEncMiscParameterFrameRate frameRate;
	memset(&frameRate, 0, sizeof(frameRate));
	AVRational rate = fParams.frameRate;
	if (rate.num > 0 && rate.den > 0) {
		if (rate.num > 65535 || rate.den > 65535)
			av_reduce(&rate.num, &rate.den, rate.num, rate.den, 65535);
		frameRate.framerate = (uint32)rate.den << 16 | (uint32)rate.num;
		_AddMisc(picture, VAEncMiscParameterTypeFrameRate, &frameRate,
			sizeof(frameRate));
	}

	if (fQualityRange > 0) {
		// Mesa takes its own bits (preset, pre-encoding, VBAQ); other
		// drivers a level, 1 being the best
		VAEncMiscParameterBufferQualityLevel quality;
		memset(&quality, 0, sizeof(quality));
		if (fMesa) {
			static const uint32 kMesaModes[kSpeedCount] = {
				0,									// speed preset
				1 | 0 << 1 | 1 << 4,				// speed + VBAQ
				1,									// balanced (default)
				1 | 2 << 1 | 1 << 3 | 1 << 4,		// quality
				1 | 3 << 1 | 1 << 3 | 1 << 4,		// high quality
			};
			quality.quality_level = kMesaModes[fParams.speed];
		} else {
			uint32 levels = fQualityRange;
			quality.quality_level = levels
				- (levels - 1) * (uint32)fParams.speed / (kSpeedCount - 1);
			if (fParams.speed == kSpeedFastest)
				quality.quality_level = levels;
		}
		_AddMisc(picture, VAEncMiscParameterTypeQualityLevel, &quality,
			sizeof(quality));
	}

	if (fRateControl == VA_RC_CQP)
		return B_OK;

	uint32 target = (uint32)fParams.bitrate * 1000;
	uint32 peak = fRateControl == VA_RC_CBR ? target : target / 2 * 3;
	VAEncMiscParameterRateControl control;
	memset(&control, 0, sizeof(control));
	control.bits_per_second = peak;
	control.target_percentage = fRateControl == VA_RC_CBR ? 100
		: (uint32)((uint64)target * 100 / peak);
	control.window_size = 1000;
	control.initial_qp = 0;
	control.min_qp = 0;
	control.max_qp = 0;
	if (fRateControl == VA_RC_QVBR || fRateControl == VA_RC_ICQ)
		control.quality_factor = (uint32)fQP;
	_AddMisc(picture, VAEncMiscParameterTypeRateControl, &control,
		sizeof(control));

	VAEncMiscParameterHRD hrd;
	memset(&hrd, 0, sizeof(hrd));
	hrd.buffer_size = peak;
	hrd.initial_buffer_fullness = peak / 4 * 3;
	return _AddMisc(picture, VAEncMiscParameterTypeHRD, &hrd, sizeof(hrd));
}


// #pragma mark - levels


struct H264Level {
	int	idc;
	int	maxFrameSize;		// macroblocks
	int	maxMacroblockRate;	// per second
};


static const H264Level kH264Levels[] = {
	{ 10, 99, 1485 }, { 11, 396, 3000 }, { 12, 396, 6000 },
	{ 13, 396, 11880 }, { 20, 396, 11880 }, { 21, 792, 19800 },
	{ 22, 1620, 20250 }, { 30, 1620, 40500 }, { 31, 3600, 108000 },
	{ 32, 5120, 216000 }, { 41, 8192, 245760 }, { 42, 8704, 522240 },
	{ 50, 22080, 589824 }, { 51, 36864, 983040 }, { 52, 36864, 2073600 },
	{ 60, 139264, 4177920 }, { 61, 139264, 8355840 },
	{ 62, 139264, 16711680 },
};


struct HevcLevel {
	int	idc;				// level * 30
	int	maxLumaPictureSize;
	int64 maxLumaSampleRate;
};


static const HevcLevel kHevcLevels[] = {
	{ 30, 36864, 552960 }, { 60, 122880, 3686400 }, { 63, 245760, 7372800 },
	{ 90, 552960, 16588800 }, { 93, 983040, 33177600 },
	{ 120, 2228224, 66846720 }, { 123, 2228224, 133693440 },
	{ 150, 8912896, 267386880 }, { 153, 8912896, 534773760 },
	{ 156, 8912896, 1069547520 }, { 180, 35651584, 1069547520 },
	{ 183, 35651584, 2139095040 }, { 186, 35651584, 4278190080LL },
};


int
VaapiEncoder::_Level() const
{
	double rate = fParams.frameRate.num > 0 && fParams.frameRate.den > 0
		? av_q2d(fParams.frameRate) : 30.0;
	if (fCodec == AV_CODEC_ID_H264) {
		int widthMbs = fAlignedWidth / 16;
		int heightMbs = fAlignedHeight / 16;
		int frameSize = widthMbs * heightMbs;
		for (size_t i = 0; i < sizeof(kH264Levels) / sizeof(kH264Levels[0]);
				i++) {
			const H264Level& level = kH264Levels[i];
			int maxDimension = (int)sqrt(level.maxFrameSize * 8.0);
			if (frameSize <= level.maxFrameSize
				&& frameSize * rate <= level.maxMacroblockRate
				&& widthMbs <= maxDimension && heightMbs <= maxDimension)
				return level.idc;
		}
		return 62;
	}

	int64 size = (int64)fAlignedWidth * fAlignedHeight;
	for (size_t i = 0; i < sizeof(kHevcLevels) / sizeof(kHevcLevels[0]);
			i++) {
		const HevcLevel& level = kHevcLevels[i];
		int maxDimension = (int)sqrt(level.maxLumaPictureSize * 8.0);
		if (size <= level.maxLumaPictureSize
			&& size * rate <= level.maxLumaSampleRate
			&& fAlignedWidth <= maxDimension && fAlignedHeight <= maxDimension)
			return level.idc;
	}
	return 186;
}


// #pragma mark - H.264


void
VaapiEncoder::_WriteH264Vui(BitWriter& b)
{
	int sarWidth = 0, sarHeight = 0;
	int aspect = fParams.sampleAspect.num > 0
		? aspect_ratio_idc(fParams.sampleAspect, &sarWidth, &sarHeight) : 0;
	b.Bit(aspect != 0);
	if (aspect != 0) {
		b.Bits(aspect, 8);
		if (aspect == 255) {
			b.Bits(sarWidth, 16);
			b.Bits(sarHeight, 16);
		}
	}
	b.Bit(false);	// overscan_info_present_flag

	bool colour = fParams.primaries != AVCOL_PRI_UNSPECIFIED
		|| fParams.transfer != AVCOL_TRC_UNSPECIFIED
		|| fParams.matrix != AVCOL_SPC_UNSPECIFIED;
	b.Bit(true);	// video_signal_type_present_flag
	b.Bits(5, 3);	// video_format: unspecified
	b.Bit(fParams.range == AVCOL_RANGE_JPEG);
	b.Bit(colour);
	if (colour) {
		b.Bits(fParams.primaries, 8);
		b.Bits(fParams.transfer, 8);
		b.Bits(fParams.matrix, 8);
	}

	bool chroma = fParams.chromaLocation != AVCHROMA_LOC_UNSPECIFIED;
	b.Bit(chroma);
	if (chroma) {
		b.UnsignedExpGolomb(fParams.chromaLocation - 1);
		b.UnsignedExpGolomb(fParams.chromaLocation - 1);
	}

	b.Bit(true);	// timing_info_present_flag
	b.Bits(fParams.frameRate.den, 32);
	b.Bits(2 * fParams.frameRate.num, 32);
	b.Bit(fParams.constantFrameRate);

	b.Bit(false);	// nal_hrd_parameters_present_flag
	b.Bit(false);	// vcl_hrd_parameters_present_flag
	b.Bit(false);	// pic_struct_present_flag

	b.Bit(true);	// bitstream_restriction_flag
	b.Bit(true);	// motion_vectors_over_pic_boundaries_flag
	b.UnsignedExpGolomb(0);		// max_bytes_per_pic_denom
	b.UnsignedExpGolomb(0);		// max_bits_per_mb_denom
	b.UnsignedExpGolomb(15);	// log2_max_mv_length_horizontal
	b.UnsignedExpGolomb(15);	// log2_max_mv_length_vertical
	b.UnsignedExpGolomb(0);		// max_num_reorder_frames
	b.UnsignedExpGolomb(1);		// max_dec_frame_buffering
}


static int
h264_profile_idc(VAProfile profile)
{
	switch (profile) {
		case VAProfileH264ConstrainedBaseline:
			return 66;
		case VAProfileH264Main:
			return 77;
		default:
			return 100;
	}
}


void
VaapiEncoder::_WriteH264Headers(std::vector<uint8>& out)
{
	int profile = h264_profile_idc(fProfile);
	int widthMbs = fAlignedWidth / 16;
	int heightMbs = fAlignedHeight / 16;

	BitWriter sps;
	sps.Bits(profile, 8);
	sps.Bit(false);					// constraint_set0
	sps.Bit(profile == 66 || profile == 77);
	sps.Bit(false);					// constraint_set2
	sps.Bit(fParams.keyInterval == 1 && profile == 100);
	sps.Bit(profile == 77 || profile == 100);	// set4: frame_mbs_only
	sps.Bit(profile == 77 || profile == 100);	// set5: no B pictures
	sps.Bits(0, 2);
	sps.Bits(_Level(), 8);
	sps.UnsignedExpGolomb(0);		// seq_parameter_set_id
	if (profile == 100) {
		sps.UnsignedExpGolomb(1);	// chroma_format_idc
		sps.UnsignedExpGolomb(0);	// bit_depth_luma_minus8
		sps.UnsignedExpGolomb(0);	// bit_depth_chroma_minus8
		sps.Bit(false);				// qpprime_y_zero_transform_bypass_flag
		sps.Bit(false);				// seq_scaling_matrix_present_flag
	}
	sps.UnsignedExpGolomb(4);		// log2_max_frame_num_minus4
	sps.UnsignedExpGolomb(2);		// pic_order_cnt_type
	sps.UnsignedExpGolomb(1);		// max_num_ref_frames
	sps.Bit(false);					// gaps_in_frame_num_value_allowed_flag
	sps.UnsignedExpGolomb(widthMbs - 1);
	sps.UnsignedExpGolomb(heightMbs - 1);
	sps.Bit(true);					// frame_mbs_only_flag
	sps.Bit(true);					// direct_8x8_inference_flag
	bool crop = fAlignedWidth != fParams.width
		|| fAlignedHeight != fParams.height;
	sps.Bit(crop);
	if (crop) {
		sps.UnsignedExpGolomb(0);
		sps.UnsignedExpGolomb((fAlignedWidth - fParams.width) / 2);
		sps.UnsignedExpGolomb(0);
		sps.UnsignedExpGolomb((fAlignedHeight - fParams.height) / 2);
	}
	sps.Bit(true);					// vui_parameters_present_flag
	_WriteH264Vui(sps);
	sps.TrailingBits();

	BitWriter pps;
	pps.UnsignedExpGolomb(0);		// pic_parameter_set_id
	pps.UnsignedExpGolomb(0);		// seq_parameter_set_id
	pps.Bit(profile != 66);			// entropy_coding_mode_flag: CABAC
	pps.Bit(false);					// bottom_field_pic_order_in_frame_present
	pps.UnsignedExpGolomb(0);		// num_slice_groups_minus1
	pps.UnsignedExpGolomb(0);		// num_ref_idx_l0_default_active_minus1
	pps.UnsignedExpGolomb(0);		// num_ref_idx_l1_default_active_minus1
	pps.Bit(false);					// weighted_pred_flag
	pps.Bits(0, 2);					// weighted_bipred_idc
	pps.SignedExpGolomb(fQPIntra - 26);	// pic_init_qp_minus26
	pps.SignedExpGolomb(0);			// pic_init_qs_minus26
	pps.SignedExpGolomb(0);			// chroma_qp_index_offset
	pps.Bit(false);					// deblocking_filter_control_present_flag
	pps.Bit(false);					// constrained_intra_pred_flag
	pps.Bit(false);					// redundant_pic_cnt_present_flag
	if (profile == 100) {
		pps.Bit(true);				// transform_8x8_mode_flag
		pps.Bit(false);				// pic_scaling_matrix_present_flag
		pps.SignedExpGolomb(0);		// second_chroma_qp_index_offset
	}
	pps.TrailingBits();

	const uint8 spsHeader = 0x67;	// nal_ref_idc 3, type 7
	const uint8 ppsHeader = 0x68;
	out.clear();
	append_annexb_nal(out, &spsHeader, 1, sps);
	append_annexb_nal(out, &ppsHeader, 1, pps);
}


status_t
VaapiEncoder::_BuildH264(Picture& picture)
{
	int profile = h264_profile_idc(fProfile);
	int widthMbs = fAlignedWidth / 16;
	int heightMbs = fAlignedHeight / 16;
	int qp = picture.idr ? fQPIntra : fQP;
	if (fRateControl != VA_RC_CQP)
		qp = fQPIntra;

	if (picture.idr) {
		VAEncSequenceParameterBufferH264 sequence;
		memset(&sequence, 0, sizeof(sequence));
		sequence.seq_parameter_set_id = 0;
		sequence.level_idc = _Level();
		sequence.intra_period = fParams.keyInterval;
		sequence.intra_idr_period = fParams.keyInterval;
		sequence.ip_period = 1;
		sequence.bits_per_second = fRateControl == VA_RC_CQP ? 0
			: (uint32)fParams.bitrate * 1000;
		sequence.max_num_ref_frames = 1;
		sequence.picture_width_in_mbs = widthMbs;
		sequence.picture_height_in_mbs = heightMbs;
		sequence.seq_fields.bits.chroma_format_idc = 1;
		sequence.seq_fields.bits.frame_mbs_only_flag = 1;
		sequence.seq_fields.bits.direct_8x8_inference_flag = 1;
		sequence.seq_fields.bits.log2_max_frame_num_minus4 = 4;
		sequence.seq_fields.bits.pic_order_cnt_type = 2;
		sequence.frame_cropping_flag = fAlignedWidth != fParams.width
			|| fAlignedHeight != fParams.height;
		sequence.frame_crop_right_offset = (fAlignedWidth - fParams.width) / 2;
		sequence.frame_crop_bottom_offset
			= (fAlignedHeight - fParams.height) / 2;
		sequence.vui_parameters_present_flag = 1;
		sequence.vui_fields.bits.timing_info_present_flag = 1;
		sequence.vui_fields.bits.bitstream_restriction_flag = 1;
		sequence.vui_fields.bits.log2_max_mv_length_horizontal = 15;
		sequence.vui_fields.bits.log2_max_mv_length_vertical = 15;
		int sarWidth = 0, sarHeight = 0;
		int aspect = fParams.sampleAspect.num > 0
			? aspect_ratio_idc(fParams.sampleAspect, &sarWidth, &sarHeight)
			: 0;
		sequence.vui_fields.bits.aspect_ratio_info_present_flag = aspect != 0;
		sequence.aspect_ratio_idc = aspect;
		sequence.sar_width = sarWidth;
		sequence.sar_height = sarHeight;
		sequence.num_units_in_tick = fParams.frameRate.den;
		sequence.time_scale = 2 * fParams.frameRate.num;
		if (_AddBuffer(picture, VAEncSequenceParameterBufferType, &sequence,
				sizeof(sequence)) != B_OK)
			return B_ERROR;
		if (_AddRateControl(picture) != B_OK)
			return B_ERROR;
	}

	VAEncPictureParameterBufferH264 parameters;
	memset(&parameters, 0, sizeof(parameters));
	parameters.CurrPic.picture_id = picture.recon;
	parameters.CurrPic.frame_idx = picture.frameNum;
	parameters.CurrPic.flags = 0;
	parameters.CurrPic.TopFieldOrderCnt = picture.order;
	parameters.CurrPic.BottomFieldOrderCnt = picture.order;
	for (int i = 0; i < 16; i++) {
		parameters.ReferenceFrames[i].picture_id = VA_INVALID_ID;
		parameters.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
	}
	if (!picture.idr) {
		parameters.ReferenceFrames[0].picture_id = fReference;
		parameters.ReferenceFrames[0].frame_idx = fReferenceFrameNum;
		parameters.ReferenceFrames[0].flags
			= VA_PICTURE_H264_SHORT_TERM_REFERENCE;
		parameters.ReferenceFrames[0].TopFieldOrderCnt = fReferenceOrder;
		parameters.ReferenceFrames[0].BottomFieldOrderCnt = fReferenceOrder;
	}
	parameters.coded_buf = picture.coded;
	parameters.pic_parameter_set_id = 0;
	parameters.seq_parameter_set_id = 0;
	parameters.frame_num = picture.frameNum;
	parameters.pic_init_qp = fQPIntra;
	parameters.num_ref_idx_l0_active_minus1 = 0;
	parameters.pic_fields.bits.idr_pic_flag = picture.idr;
	parameters.pic_fields.bits.reference_pic_flag = 1;
	parameters.pic_fields.bits.entropy_coding_mode_flag = profile != 66;
	parameters.pic_fields.bits.transform_8x8_mode_flag = profile == 100;
	if (_AddBuffer(picture, VAEncPictureParameterBufferType, &parameters,
			sizeof(parameters)) != B_OK)
		return B_ERROR;

	if (picture.idr && (fPackedHeaders & VA_ENC_PACKED_HEADER_SEQUENCE) != 0) {
		std::vector<uint8> headers;
		_WriteH264Headers(headers);
		if (_AddPacked(picture, VAEncPackedHeaderSequence, headers,
				headers.size() * 8) != B_OK)
			return B_ERROR;
	}

	if ((fPackedHeaders & VA_ENC_PACKED_HEADER_SLICE) != 0) {
		BitWriter slice;
		slice.UnsignedExpGolomb(0);					// first_mb_in_slice
		slice.UnsignedExpGolomb(picture.idr ? 7 : 5);	// slice_type
		slice.UnsignedExpGolomb(0);					// pic_parameter_set_id
		slice.Bits(picture.frameNum & 0xff, 8);		// frame_num, 8 bits
		if (picture.idr)
			slice.UnsignedExpGolomb(fIdrId);		// idr_pic_id
		if (!picture.idr) {
			slice.Bit(false);	// num_ref_idx_active_override_flag
			slice.Bit(false);	// ref_pic_list_modification_flag_l0
		}
		// dec_ref_pic_marking()
		if (picture.idr) {
			slice.Bit(false);	// no_output_of_prior_pics_flag
			slice.Bit(false);	// long_term_reference_flag
		} else
			slice.Bit(false);	// adaptive_ref_pic_marking_mode_flag
		if (profile != 66 && !picture.idr)
			slice.UnsignedExpGolomb(0);				// cabac_init_idc
		slice.SignedExpGolomb(qp - fQPIntra);		// slice_qp_delta

		std::vector<uint8> header;
		uint8 nal = picture.idr ? 0x65 : 0x41;	// IDR ref 3 / slice ref 2
		size_t bits = append_annexb_nal(header, &nal, 1, slice);
		if (_AddPacked(picture, VAEncPackedHeaderSlice, header, bits) != B_OK)
			return B_ERROR;
	}

	VAEncSliceParameterBufferH264 slice;
	memset(&slice, 0, sizeof(slice));
	slice.macroblock_address = 0;
	slice.num_macroblocks = widthMbs * heightMbs;
	slice.macroblock_info = VA_INVALID_ID;
	slice.slice_type = picture.idr ? 2 : 0;
	slice.pic_parameter_set_id = 0;
	slice.idr_pic_id = (uint16)fIdrId;
	slice.direct_spatial_mv_pred_flag = 1;
	for (int i = 0; i < 32; i++) {
		slice.RefPicList0[i].picture_id = VA_INVALID_ID;
		slice.RefPicList0[i].flags = VA_PICTURE_H264_INVALID;
		slice.RefPicList1[i].picture_id = VA_INVALID_ID;
		slice.RefPicList1[i].flags = VA_PICTURE_H264_INVALID;
	}
	if (!picture.idr)
		slice.RefPicList0[0] = parameters.ReferenceFrames[0];
	slice.slice_qp_delta = qp - fQPIntra;
	return _AddBuffer(picture, VAEncSliceParameterBufferType, &slice,
		sizeof(slice));
}


// #pragma mark - H.265


static int
hevc_profile_idc(VAProfile profile)
{
	return profile == VAProfileHEVCMain10 ? 2 : 1;
}


void
VaapiEncoder::_WriteHevcProfileTierLevel(BitWriter& b)
{
	int profile = hevc_profile_idc(fProfile);
	b.Bits(0, 2);				// general_profile_space
	b.Bit(false);				// general_tier_flag: main tier
	b.Bits(profile, 5);
	for (int i = 0; i < 32; i++)
		b.Bit(i == profile || (profile == 1 && i == 2));
	b.Bit(true);				// general_progressive_source_flag
	b.Bit(false);				// general_interlaced_source_flag
	b.Bit(true);				// general_non_packed_constraint_flag
	b.Bit(true);				// general_frame_only_constraint_flag
	// Main and Main 10 (compatible with 2): reserved_zero_7bits,
	// one_picture_only_constraint_flag, reserved_zero_35bits
	b.Bits(0, 7);
	b.Bit(false);
	b.Bits(0, 3);
	b.Bits(0, 32);
	b.Bit(false);				// general_inbld_flag
	b.Bits(_Level(), 8);
}


void
VaapiEncoder::_WriteHevcHeaders(std::vector<uint8>& out)
{
	VAConfigAttribValEncHEVCFeatures features;
	features.value = fHevcFeatures;
	VAConfigAttribValEncHEVCBlockSizes sizes;
	sizes.value = fHevcBlockSizes;

	bool timing = fParams.frameRate.num > 0 && fParams.frameRate.den > 0;

	// VPS
	BitWriter vps;
	vps.Bits(0, 4);				// vps_video_parameter_set_id
	vps.Bit(true);				// vps_base_layer_internal_flag
	vps.Bit(true);				// vps_base_layer_available_flag
	vps.Bits(0, 6);				// vps_max_layers_minus1
	vps.Bits(0, 3);				// vps_max_sub_layers_minus1
	vps.Bit(true);				// vps_temporal_id_nesting_flag
	vps.Bits(0xffff, 16);
	_WriteHevcProfileTierLevel(vps);
	vps.Bit(false);				// vps_sub_layer_ordering_info_present_flag
	vps.UnsignedExpGolomb(1);	// vps_max_dec_pic_buffering_minus1
	vps.UnsignedExpGolomb(0);	// vps_max_num_reorder_pics
	vps.UnsignedExpGolomb(0);	// vps_max_latency_increase_plus1
	vps.Bits(0, 6);				// vps_max_layer_id
	vps.UnsignedExpGolomb(0);	// vps_num_layer_sets_minus1
	vps.Bit(timing);			// vps_timing_info_present_flag
	if (timing) {
		vps.Bits(fParams.frameRate.den, 32);
		vps.Bits(fParams.frameRate.num, 32);
		vps.Bit(true);			// vps_poc_proportional_to_timing_flag
		vps.UnsignedExpGolomb(0);
		vps.UnsignedExpGolomb(0);	// vps_num_hrd_parameters
	}
	vps.Bit(false);				// vps_extension_flag
	vps.TrailingBits();

	// SPS
	int minCbLog2 = 0;
	while ((1 << minCbLog2) < fMinBlockSize)
		minCbLog2++;
	int ctbLog2 = 0;
	while ((1 << ctbLog2) < fBlockSize)
		ctbLog2++;
	int minTbLog2Minus2 = fHevcBlockSizes != 0
		? sizes.bits.log2_min_luma_transform_block_size_minus2 : 0;
	int maxTbLog2Minus2 = fHevcBlockSizes != 0
		? sizes.bits.log2_max_luma_transform_block_size_minus2 : 3;
	int depthInter = fHevcBlockSizes != 0
		? sizes.bits.max_max_transform_hierarchy_depth_inter : 3;
	int depthIntra = fHevcBlockSizes != 0
		? sizes.bits.max_max_transform_hierarchy_depth_intra : 3;
	bool amp = fHevcFeatures != 0 ? features.bits.amp != 0 : true;
	bool sao = fHevcFeatures != 0 ? features.bits.sao != 0 : false;
	bool tmvp = fHevcFeatures != 0 ? features.bits.temporal_mvp != 0 : false;

	BitWriter sps;
	sps.Bits(0, 4);				// sps_video_parameter_set_id
	sps.Bits(0, 3);				// sps_max_sub_layers_minus1
	sps.Bit(true);				// sps_temporal_id_nesting_flag
	_WriteHevcProfileTierLevel(sps);
	sps.UnsignedExpGolomb(0);	// sps_seq_parameter_set_id
	sps.UnsignedExpGolomb(1);	// chroma_format_idc
	sps.UnsignedExpGolomb(fAlignedWidth);
	sps.UnsignedExpGolomb(fAlignedHeight);
	bool window = fAlignedWidth != fParams.width
		|| fAlignedHeight != fParams.height;
	sps.Bit(window);
	if (window) {
		sps.UnsignedExpGolomb(0);
		sps.UnsignedExpGolomb((fAlignedWidth - fParams.width) / 2);
		sps.UnsignedExpGolomb(0);
		sps.UnsignedExpGolomb((fAlignedHeight - fParams.height) / 2);
	}
	sps.UnsignedExpGolomb(fBitDepth - 8);
	sps.UnsignedExpGolomb(fBitDepth - 8);
	sps.UnsignedExpGolomb(8);	// log2_max_pic_order_cnt_lsb_minus4
	sps.Bit(false);				// sps_sub_layer_ordering_info_present_flag
	sps.UnsignedExpGolomb(1);	// sps_max_dec_pic_buffering_minus1
	sps.UnsignedExpGolomb(0);	// sps_max_num_reorder_pics
	sps.UnsignedExpGolomb(0);	// sps_max_latency_increase_plus1
	sps.UnsignedExpGolomb(minCbLog2 - 3);
	sps.UnsignedExpGolomb(ctbLog2 - minCbLog2);
	sps.UnsignedExpGolomb(minTbLog2Minus2);
	sps.UnsignedExpGolomb(maxTbLog2Minus2 - minTbLog2Minus2);
	sps.UnsignedExpGolomb(depthInter);
	sps.UnsignedExpGolomb(depthIntra);
	sps.Bit(false);				// scaling_list_enabled_flag
	sps.Bit(amp);
	sps.Bit(sao);
	sps.Bit(false);				// pcm_enabled_flag
	sps.UnsignedExpGolomb(0);	// num_short_term_ref_pic_sets
	sps.Bit(false);				// long_term_ref_pics_present_flag
	sps.Bit(tmvp);				// sps_temporal_mvp_enabled_flag
	sps.Bit(false);				// strong_intra_smoothing_enabled_flag
	sps.Bit(true);				// vui_parameters_present_flag
	{
		int sarWidth = 0, sarHeight = 0;
		int aspect = fParams.sampleAspect.num > 0
			? aspect_ratio_idc(fParams.sampleAspect, &sarWidth, &sarHeight)
			: 0;
		sps.Bit(aspect != 0);
		if (aspect != 0) {
			sps.Bits(aspect, 8);
			if (aspect == 255) {
				sps.Bits(sarWidth, 16);
				sps.Bits(sarHeight, 16);
			}
		}
		sps.Bit(false);			// overscan_info_present_flag
		bool colour = fParams.primaries != AVCOL_PRI_UNSPECIFIED
			|| fParams.transfer != AVCOL_TRC_UNSPECIFIED
			|| fParams.matrix != AVCOL_SPC_UNSPECIFIED;
		sps.Bit(true);			// video_signal_type_present_flag
		sps.Bits(5, 3);
		sps.Bit(fParams.range == AVCOL_RANGE_JPEG);
		sps.Bit(colour);
		if (colour) {
			sps.Bits(fParams.primaries, 8);
			sps.Bits(fParams.transfer, 8);
			sps.Bits(fParams.matrix, 8);
		}
		bool chroma = fParams.chromaLocation != AVCHROMA_LOC_UNSPECIFIED;
		sps.Bit(chroma);
		if (chroma) {
			sps.UnsignedExpGolomb(fParams.chromaLocation - 1);
			sps.UnsignedExpGolomb(fParams.chromaLocation - 1);
		}
		sps.Bit(false);			// neutral_chroma_indication_flag
		sps.Bit(false);			// field_seq_flag
		sps.Bit(false);			// frame_field_info_present_flag
		sps.Bit(false);			// default_display_window_flag
		sps.Bit(timing);		// vui_timing_info_present_flag
		if (timing) {
			sps.Bits(fParams.frameRate.den, 32);
			sps.Bits(fParams.frameRate.num, 32);
			sps.Bit(true);		// vui_poc_proportional_to_timing_flag
			sps.UnsignedExpGolomb(0);
			sps.Bit(false);		// vui_hrd_parameters_present_flag
		}
		sps.Bit(true);			// bitstream_restriction_flag
		sps.Bit(false);			// tiles_fixed_structure_flag
		sps.Bit(true);			// motion_vectors_over_pic_boundaries_flag
		sps.Bit(true);			// restricted_ref_pic_lists_flag
		sps.UnsignedExpGolomb(0);	// min_spatial_segmentation_idc
		sps.UnsignedExpGolomb(0);	// max_bytes_per_pic_denom
		sps.UnsignedExpGolomb(0);	// max_bits_per_min_cu_denom
		sps.UnsignedExpGolomb(15);
		sps.UnsignedExpGolomb(15);
	}
	sps.Bit(false);				// sps_extension_present_flag
	sps.TrailingBits();

	// PPS
	bool cuQpDelta = fRateControl != VA_RC_CQP
		&& (fHevcFeatures == 0 || features.bits.cu_qp_delta != 0);
	bool transformSkip = fHevcFeatures != 0
		&& features.bits.transform_skip != 0;
	BitWriter pps;
	pps.UnsignedExpGolomb(0);	// pps_pic_parameter_set_id
	pps.UnsignedExpGolomb(0);	// pps_seq_parameter_set_id
	pps.Bit(false);				// dependent_slice_segments_enabled_flag
	pps.Bit(false);				// output_flag_present_flag
	pps.Bits(0, 3);				// num_extra_slice_header_bits
	pps.Bit(false);				// sign_data_hiding_enabled_flag
	pps.Bit(false);				// cabac_init_present_flag
	pps.UnsignedExpGolomb(0);	// num_ref_idx_l0_default_active_minus1
	pps.UnsignedExpGolomb(0);	// num_ref_idx_l1_default_active_minus1
	pps.SignedExpGolomb(fQPIntra - 26);	// init_qp_minus26
	pps.Bit(false);				// constrained_intra_pred_flag
	pps.Bit(transformSkip);
	pps.Bit(cuQpDelta);
	if (cuQpDelta)
		pps.UnsignedExpGolomb(ctbLog2 - minCbLog2);	// diff_cu_qp_delta_depth
	pps.SignedExpGolomb(0);		// pps_cb_qp_offset
	pps.SignedExpGolomb(0);		// pps_cr_qp_offset
	pps.Bit(false);				// pps_slice_chroma_qp_offsets_present_flag
	pps.Bit(false);				// weighted_pred_flag
	pps.Bit(false);				// weighted_bipred_flag
	pps.Bit(false);				// transquant_bypass_enabled_flag
	pps.Bit(false);				// tiles_enabled_flag
	pps.Bit(false);				// entropy_coding_sync_enabled_flag
	pps.Bit(true);				// pps_loop_filter_across_slices_enabled_flag
	pps.Bit(false);				// deblocking_filter_control_present_flag
	pps.Bit(false);				// pps_scaling_list_data_present_flag
	pps.Bit(false);				// lists_modification_present_flag
	pps.UnsignedExpGolomb(0);	// log2_parallel_merge_level_minus2
	pps.Bit(false);				// slice_segment_header_extension_present
	pps.Bit(false);				// pps_extension_present_flag
	pps.TrailingBits();

	const uint8 vpsHeader[2] = { 32 << 1, 1 };
	const uint8 spsHeader[2] = { 33 << 1, 1 };
	const uint8 ppsHeader[2] = { 34 << 1, 1 };
	out.clear();
	append_annexb_nal(out, vpsHeader, 2, vps);
	append_annexb_nal(out, spsHeader, 2, sps);
	append_annexb_nal(out, ppsHeader, 2, pps);
}


status_t
VaapiEncoder::_BuildHevc(Picture& picture)
{
	VAConfigAttribValEncHEVCFeatures features;
	features.value = fHevcFeatures;
	VAConfigAttribValEncHEVCBlockSizes sizes;
	sizes.value = fHevcBlockSizes;
	bool amp = fHevcFeatures != 0 ? features.bits.amp != 0 : true;
	bool sao = fHevcFeatures != 0 ? features.bits.sao != 0 : false;
	bool tmvp = fHevcFeatures != 0 ? features.bits.temporal_mvp != 0 : false;
	bool cuQpDelta = fRateControl != VA_RC_CQP
		&& (fHevcFeatures == 0 || features.bits.cu_qp_delta != 0);
	bool transformSkip = fHevcFeatures != 0
		&& features.bits.transform_skip != 0;
	int minCbLog2 = 0;
	while ((1 << minCbLog2) < fMinBlockSize)
		minCbLog2++;
	int ctbLog2 = 0;
	while ((1 << ctbLog2) < fBlockSize)
		ctbLog2++;

	int qp = picture.idr ? fQPIntra : fQP;
	if (fRateControl != VA_RC_CQP)
		qp = fQPIntra;
	int ctusAcross = (fAlignedWidth + fBlockSize - 1) / fBlockSize;
	int ctusDown = (fAlignedHeight + fBlockSize - 1) / fBlockSize;

	if (picture.idr) {
		VAEncSequenceParameterBufferHEVC sequence;
		memset(&sequence, 0, sizeof(sequence));
		sequence.general_profile_idc = hevc_profile_idc(fProfile);
		sequence.general_level_idc = _Level();
		sequence.general_tier_flag = 0;
		sequence.intra_period = fParams.keyInterval;
		sequence.intra_idr_period = fParams.keyInterval;
		sequence.ip_period = 1;
		sequence.bits_per_second = fRateControl == VA_RC_CQP ? 0
			: (uint32)fParams.bitrate * 1000;
		sequence.pic_width_in_luma_samples = fAlignedWidth;
		sequence.pic_height_in_luma_samples = fAlignedHeight;
		sequence.seq_fields.bits.chroma_format_idc = 1;
		sequence.seq_fields.bits.bit_depth_luma_minus8 = fBitDepth - 8;
		sequence.seq_fields.bits.bit_depth_chroma_minus8 = fBitDepth - 8;
		sequence.seq_fields.bits.amp_enabled_flag = amp;
		sequence.seq_fields.bits.sample_adaptive_offset_enabled_flag = sao;
		sequence.seq_fields.bits.sps_temporal_mvp_enabled_flag = tmvp;
		sequence.log2_min_luma_coding_block_size_minus3 = minCbLog2 - 3;
		sequence.log2_diff_max_min_luma_coding_block_size
			= ctbLog2 - minCbLog2;
		sequence.log2_min_transform_block_size_minus2 = fHevcBlockSizes != 0
			? sizes.bits.log2_min_luma_transform_block_size_minus2 : 0;
		sequence.log2_diff_max_min_transform_block_size
			= fHevcBlockSizes != 0
				? sizes.bits.log2_max_luma_transform_block_size_minus2
					- sizes.bits.log2_min_luma_transform_block_size_minus2
				: 3;
		sequence.max_transform_hierarchy_depth_inter = fHevcBlockSizes != 0
			? sizes.bits.max_max_transform_hierarchy_depth_inter : 3;
		sequence.max_transform_hierarchy_depth_intra = fHevcBlockSizes != 0
			? sizes.bits.max_max_transform_hierarchy_depth_intra : 3;
		sequence.vui_parameters_present_flag = 0;
		if (_AddBuffer(picture, VAEncSequenceParameterBufferType, &sequence,
				sizeof(sequence)) != B_OK)
			return B_ERROR;
		if (_AddRateControl(picture) != B_OK)
			return B_ERROR;
	}

	VAEncPictureParameterBufferHEVC parameters;
	memset(&parameters, 0, sizeof(parameters));
	parameters.decoded_curr_pic.picture_id = picture.recon;
	parameters.decoded_curr_pic.pic_order_cnt = picture.order;
	parameters.decoded_curr_pic.flags = 0;
	for (int i = 0; i < 15; i++) {
		parameters.reference_frames[i].picture_id = VA_INVALID_ID;
		parameters.reference_frames[i].flags = VA_PICTURE_HEVC_INVALID;
	}
	if (!picture.idr) {
		parameters.reference_frames[0].picture_id = fReference;
		parameters.reference_frames[0].pic_order_cnt = fReferenceOrder;
		parameters.reference_frames[0].flags
			= VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
	}
	parameters.coded_buf = picture.coded;
	parameters.collocated_ref_pic_index = tmvp ? 0 : 0xff;
	parameters.last_picture = 0;
	parameters.pic_init_qp = fQPIntra;
	parameters.diff_cu_qp_delta_depth = cuQpDelta ? ctbLog2 - minCbLog2 : 0;
	parameters.slice_pic_parameter_set_id = 0;
	parameters.nal_unit_type = picture.idr ? 19 : 1;	// IDR_W_RADL, TRAIL_R
	parameters.pic_fields.bits.idr_pic_flag = picture.idr;
	parameters.pic_fields.bits.coding_type = picture.idr ? 1 : 2;
	parameters.pic_fields.bits.reference_pic_flag = 1;
	parameters.pic_fields.bits.transform_skip_enabled_flag = transformSkip;
	parameters.pic_fields.bits.cu_qp_delta_enabled_flag = cuQpDelta;
	parameters.pic_fields.bits.pps_loop_filter_across_slices_enabled_flag = 1;
	if (_AddBuffer(picture, VAEncPictureParameterBufferType, &parameters,
			sizeof(parameters)) != B_OK)
		return B_ERROR;

	if (picture.idr && (fPackedHeaders & VA_ENC_PACKED_HEADER_SEQUENCE) != 0) {
		std::vector<uint8> headers;
		_WriteHevcHeaders(headers);
		if (_AddPacked(picture, VAEncPackedHeaderSequence, headers,
				headers.size() * 8) != B_OK)
			return B_ERROR;
	}
	if (picture.idr && (fPackedHeaders & VA_ENC_PACKED_HEADER_RAW_DATA) != 0
		&& _AddHdrMetadata(picture) != B_OK)
		return B_ERROR;

	int sliceType = picture.idr ? 2 : 1;	// I, P
	if ((fPackedHeaders & VA_ENC_PACKED_HEADER_SLICE) != 0) {
		BitWriter slice;
		slice.Bit(true);			// first_slice_segment_in_pic_flag
		if (picture.idr)
			slice.Bit(false);		// no_output_of_prior_pics_flag
		slice.UnsignedExpGolomb(0);	// slice_pic_parameter_set_id
		slice.UnsignedExpGolomb(sliceType);
		if (!picture.idr) {
			slice.Bits(picture.order & 0xfff, 12);	// slice_pic_order_cnt_lsb
			slice.Bit(false);		// short_term_ref_pic_set_sps_flag
			// st_ref_pic_set(0): the previous picture, used
			slice.UnsignedExpGolomb(1);		// num_negative_pics
			slice.UnsignedExpGolomb(0);		// num_positive_pics
			slice.UnsignedExpGolomb(picture.order - fReferenceOrder - 1);
			slice.Bit(true);		// used_by_curr_pic_s0_flag
			if (tmvp)
				slice.Bit(true);	// slice_temporal_mvp_enabled_flag
		}
		if (sao) {
			slice.Bit(true);		// slice_sao_luma_flag
			slice.Bit(true);		// slice_sao_chroma_flag
		}
		if (!picture.idr) {
			slice.Bit(false);		// num_ref_idx_active_override_flag
			// tmvp: collocated_from_l0 is inferred for P, and with one
			// reference no collocated_ref_idx is coded
			slice.UnsignedExpGolomb(0);	// five_minus_max_num_merge_cand
		}
		slice.SignedExpGolomb(qp - fQPIntra);	// slice_qp_delta
		// pps_loop_filter_across_slices_enabled_flag and deblocking on
		slice.Bit(false);	// slice_loop_filter_across_slices_enabled_flag
		slice.Bit(true);	// byte_alignment(): alignment_bit_equal_to_one
		slice.ByteAlign();

		std::vector<uint8> header;
		uint8 nal[2] = { (uint8)((picture.idr ? 19 : 1) << 1), 1 };
		size_t bits = append_annexb_nal(header, nal, 2, slice);
		if (_AddPacked(picture, VAEncPackedHeaderSlice, header, bits) != B_OK)
			return B_ERROR;
	}

	VAEncSliceParameterBufferHEVC slice;
	memset(&slice, 0, sizeof(slice));
	slice.slice_segment_address = 0;
	slice.num_ctu_in_slice = ctusAcross * ctusDown;
	slice.slice_type = sliceType;
	slice.slice_pic_parameter_set_id = 0;
	slice.num_ref_idx_l0_active_minus1 = 0;
	slice.num_ref_idx_l1_active_minus1 = 0;
	for (int i = 0; i < 15; i++) {
		slice.ref_pic_list0[i].picture_id = VA_INVALID_ID;
		slice.ref_pic_list0[i].flags = VA_PICTURE_HEVC_INVALID;
		slice.ref_pic_list1[i].picture_id = VA_INVALID_ID;
		slice.ref_pic_list1[i].flags = VA_PICTURE_HEVC_INVALID;
	}
	if (!picture.idr)
		slice.ref_pic_list0[0] = parameters.reference_frames[0];
	slice.max_num_merge_cand = 5;
	slice.slice_qp_delta = qp - fQPIntra;
	slice.slice_fields.bits.last_slice_of_pic_flag = 1;
	slice.slice_fields.bits.slice_temporal_mvp_enabled_flag
		= tmvp && !picture.idr;
	slice.slice_fields.bits.slice_sao_luma_flag = sao;
	slice.slice_fields.bits.slice_sao_chroma_flag = sao;
	slice.slice_fields.bits.collocated_from_l0_flag = 1;
	return _AddBuffer(picture, VAEncSliceParameterBufferType, &slice,
		sizeof(slice));
}


// #pragma mark - AV1


struct Av1Level {
	int		index;		// seq_level_idx
	int64	maxPictureSize;
	int		maxWidth;
	int		maxHeight;
	int64	maxDisplayRate;
};


static const Av1Level kAv1Levels[] = {
	{ 0, 147456, 2048, 1152, 4423680 },
	{ 1, 278784, 2816, 1584, 8363520 },
	{ 4, 665856, 4352, 2448, 19975680 },
	{ 5, 1065024, 5504, 3096, 31950720 },
	{ 8, 2359296, 6144, 3456, 70778880 },
	{ 9, 2359296, 6144, 3456, 141557760 },
	{ 12, 8912896, 8192, 4352, 267386880 },
	{ 13, 8912896, 8192, 4352, 534773760 },
	{ 14, 8912896, 8192, 4352, 1069547520 },
	{ 16, 35651584, 16384, 8704, 1069547520 },
	{ 17, 35651584, 16384, 8704, 2139095040 },
	{ 18, 35651584, 16384, 8704, 4278190080LL },
};


static int
av1_level(int width, int height, double rate)
{
	int64 size = (int64)width * height;
	for (size_t i = 0; i < sizeof(kAv1Levels) / sizeof(kAv1Levels[0]); i++) {
		const Av1Level& level = kAv1Levels[i];
		if (size <= level.maxPictureSize && width <= level.maxWidth
			&& height <= level.maxHeight
			&& size * rate <= level.maxDisplayRate)
			return level.index;
	}
	return 31;
}


static int
bits_for(uint32 value)
{
	int bits = 1;
	while ((value >> bits) != 0)
		bits++;
	return bits;
}


// tile_log2() of the AV1 specification
static int
tile_log2(int blockSize, int target)
{
	int k = 0;
	while ((blockSize << k) < target)
		k++;
	return k;
}


static size_t
append_obu(std::vector<uint8>& out, int type, const BitWriter& payload)
{
	size_t start = out.size();
	out.push_back((uint8)(type << 3 | 1 << 1));	// has_size_field
	uint32 size = (uint32)payload.Data().size();
	do {
		uint8 byte = size & 0x7f;
		size >>= 7;
		out.push_back(byte | (size != 0 ? 0x80 : 0));
	} while (size != 0);
	out.insert(out.end(), payload.Data().begin(), payload.Data().end());
	return (out.size() - start) * 8;
}


/*!	The tiles: one, unless the picture is wider than a tile may be (4096). */
static void
av1_tiles(int width, int height, int* columnsLog2, int* rowsLog2,
	int* sbColumns, int* sbRows)
{
	*sbColumns = (width + 63) / 64;
	*sbRows = (height + 63) / 64;
	int maxTileWidthSb = 4096 / 64;
	int maxTileAreaSb = 4096 * 2304 / (64 * 64);
	int minLog2TileCols = tile_log2(maxTileWidthSb, *sbColumns);
	int minLog2Tiles = max_c(minLog2TileCols,
		tile_log2(maxTileAreaSb, *sbRows * *sbColumns));
	*columnsLog2 = minLog2TileCols;
	*rowsLog2 = max_c(minLog2Tiles - *columnsLog2, 0);
}


void
VaapiEncoder::_WriteAv1SequenceHeader(std::vector<uint8>& out)
{
	double rate = fParams.frameRate.num > 0 && fParams.frameRate.den > 0
		? av_q2d(fParams.frameRate) : 30.0;
	int level = av1_level(fParams.width, fParams.height, rate);
	int widthBits = bits_for(fParams.width - 1);
	int heightBits = bits_for(fParams.height - 1);

	BitWriter b;
	b.Bits(0, 3);				// seq_profile: Main
	b.Bit(false);				// still_picture
	b.Bit(false);				// reduced_still_picture_header
	b.Bit(false);				// timing_info_present_flag
	b.Bit(false);				// initial_display_delay_present_flag
	b.Bits(0, 5);				// operating_points_cnt_minus_1
	b.Bits(0, 12);				// operating_point_idc[0]
	b.Bits(level, 5);			// seq_level_idx[0]
	if (level > 7)
		b.Bit(false);			// seq_tier[0]
	b.Bits(widthBits - 1, 4);
	b.Bits(heightBits - 1, 4);
	b.Bits(fParams.width - 1, widthBits);
	b.Bits(fParams.height - 1, heightBits);
	b.Bit(false);				// frame_id_numbers_present_flag
	b.Bit(false);				// use_128x128_superblock
	b.Bit(false);				// enable_filter_intra
	b.Bit(false);				// enable_intra_edge_filter
	b.Bit(false);				// enable_interintra_compound
	b.Bit(false);				// enable_masked_compound
	b.Bit(false);				// enable_warped_motion
	b.Bit(false);				// enable_dual_filter
	b.Bit(true);				// enable_order_hint
	b.Bit(false);				// enable_jnt_comp
	b.Bit(false);				// enable_ref_frame_mvs
	b.Bit(false);				// seq_choose_screen_content_tools
	b.Bit(false);				// seq_force_screen_content_tools
	b.Bits(7, 3);				// order_hint_bits_minus_1
	b.Bit(false);				// enable_superres
	b.Bit(false);				// enable_cdef
	b.Bit(false);				// enable_restoration
	// color_config()
	b.Bit(fBitDepth > 8);		// high_bitdepth
	b.Bit(false);				// mono_chrome
	bool colour = fParams.primaries != AVCOL_PRI_UNSPECIFIED
		|| fParams.transfer != AVCOL_TRC_UNSPECIFIED
		|| fParams.matrix != AVCOL_SPC_UNSPECIFIED;
	b.Bit(colour);
	if (colour) {
		b.Bits(fParams.primaries, 8);
		b.Bits(fParams.transfer, 8);
		b.Bits(fParams.matrix, 8);
	}
	b.Bit(fParams.range == AVCOL_RANGE_JPEG);	// color_range
	// 4:2:0: chroma_sample_position
	int position = fParams.chromaLocation == AVCHROMA_LOC_LEFT ? 1
		: fParams.chromaLocation == AVCHROMA_LOC_TOPLEFT ? 2 : 0;
	b.Bits(position, 2);
	b.Bit(false);				// separate_uv_delta_q
	b.Bit(false);				// film_grain_params_present
	b.TrailingBits();

	out.clear();
	append_obu(out, 1, b);		// OBU_SEQUENCE_HEADER
}


status_t
VaapiEncoder::_BuildAv1(Picture& picture)
{
	int qindex = picture.idr ? fQPIntra : fQP;
	int slot = picture.idr ? 0 : !fReferenceSlot;
	picture.slot = slot;
	int columnsLog2, rowsLog2, sbColumns, sbRows;
	av1_tiles(fParams.width, fParams.height, &columnsLog2, &rowsLog2,
		&sbColumns, &sbRows);
	int tileColumns = 1 << columnsLog2;
	int tileRows = 1 << rowsLog2;
	// a loop filter strength that grows with the quantizer, as encoders'
	// searches end up choosing
	int filterLevel = min_c(63, max_c(0, (qindex - 20) / 6));

	if (picture.idr) {
		VAEncSequenceParameterBufferAV1 sequence;
		memset(&sequence, 0, sizeof(sequence));
		double rate = fParams.frameRate.num > 0 && fParams.frameRate.den > 0
			? av_q2d(fParams.frameRate) : 30.0;
		sequence.seq_profile = 0;
		sequence.seq_level_idx = av1_level(fParams.width, fParams.height,
			rate);
		sequence.intra_period = fParams.keyInterval;
		sequence.ip_period = 1;
		sequence.bits_per_second = fRateControl == VA_RC_CQP ? 0
			: (uint32)fParams.bitrate * 1000;
		sequence.order_hint_bits_minus_1 = 7;
		sequence.seq_fields.bits.enable_order_hint = 1;
		sequence.seq_fields.bits.bit_depth_minus8 = fBitDepth - 8;
		sequence.seq_fields.bits.subsampling_x = 1;
		sequence.seq_fields.bits.subsampling_y = 1;
		if (_AddBuffer(picture, VAEncSequenceParameterBufferType, &sequence,
				sizeof(sequence)) != B_OK)
			return B_ERROR;
		if (_AddRateControl(picture) != B_OK)
			return B_ERROR;
	}

	VAEncPictureParameterBufferAV1 parameters;
	memset(&parameters, 0, sizeof(parameters));
	parameters.frame_width_minus_1 = fParams.width - 1;
	parameters.frame_height_minus_1 = fParams.height - 1;
	parameters.reconstructed_frame = picture.recon;
	parameters.coded_buf = picture.coded;
	for (int i = 0; i < 8; i++)
		parameters.reference_frames[i] = VA_INVALID_SURFACE;
	for (int i = 0; i < 7; i++)
		parameters.ref_frame_idx[i] = picture.idr ? 0 : fReferenceSlot;
	if (!picture.idr) {
		parameters.reference_frames[fReferenceSlot] = fReference;
		parameters.ref_frame_ctrl_l0.fields.search_idx0 = 1;	// LAST_FRAME
	}
	parameters.primary_ref_frame = picture.idr ? 7 : 0;	// 7: none
	parameters.order_hint = (uint8)picture.order;
	parameters.refresh_frame_flags = picture.idr ? 0xff : 1 << slot;
	parameters.picture_flags.bits.frame_type = picture.idr ? 0 : 1;
	parameters.picture_flags.bits.error_resilient_mode = picture.idr;
	parameters.filter_level[0] = filterLevel;
	parameters.filter_level[1] = filterLevel;
	parameters.filter_level_u = filterLevel / 2;
	parameters.filter_level_v = filterLevel / 2;
	parameters.superres_scale_denominator = 8;
	parameters.base_qindex = qindex;
	parameters.min_base_qindex = 1;
	parameters.max_base_qindex = 255;
	parameters.mode_control_flags.bits.tx_mode = 2;	// TX_MODE_SELECT
	parameters.tile_cols = tileColumns;
	parameters.tile_rows = tileRows;
	for (int i = 0; i < tileColumns; i++) {
		parameters.width_in_sbs_minus_1[i] = (i + 1) * sbColumns / tileColumns
			- i * sbColumns / tileColumns - 1;
	}
	for (int i = 0; i < tileRows; i++) {
		parameters.height_in_sbs_minus_1[i] = (i + 1) * sbRows / tileRows
			- i * sbRows / tileRows - 1;
	}
	parameters.tile_group_obu_hdr_info.bits.obu_has_size_field = 1;
	static const int8_t kRefDeltas[8] = { 1, 0, 0, 0, -1, 0, -1, -1 };
	memcpy(parameters.ref_deltas, kRefDeltas, sizeof(kRefDeltas));
	if (_AddBuffer(picture, VAEncPictureParameterBufferType, &parameters,
			sizeof(parameters)) != B_OK)
		return B_ERROR;

	if (picture.idr && (fPackedHeaders & VA_ENC_PACKED_HEADER_SEQUENCE) != 0) {
		std::vector<uint8> header;
		_WriteAv1SequenceHeader(header);
		if (_AddPacked(picture, VAEncPackedHeaderSequence, header,
				header.size() * 8, false) != B_OK)
			return B_ERROR;
	}
	if (picture.idr && (fPackedHeaders & VA_ENC_PACKED_HEADER_RAW_DATA) != 0
		&& _AddHdrMetadata(picture) != B_OK)
		return B_ERROR;

	if ((fPackedHeaders & VA_ENC_PACKED_HEADER_PICTURE) != 0) {
		// uncompressed_header() for a shown key frame or inter frame
		BitWriter b;
		b.Bit(false);				// show_existing_frame
		b.Bits(picture.idr ? 0 : 1, 2);	// frame_type
		b.Bit(true);				// show_frame
		if (!picture.idr)
			b.Bit(false);			// error_resilient_mode
		b.Bit(false);				// disable_cdf_update
		b.Bit(false);				// frame_size_override_flag
		b.Bits(picture.order & 0xff, 8);	// order_hint
		if (!picture.idr) {
			b.Bits(0, 3);			// primary_ref_frame: LAST
			b.Bits(1 << slot, 8);	// refresh_frame_flags
			b.Bit(false);			// frame_refs_short_signaling
			for (int i = 0; i < 7; i++)
				b.Bits(fReferenceSlot, 3);	// ref_frame_idx[i]
			// frame_size() takes the sequence's; render_size():
			b.Bit(true);			// render_and_frame_size_different
			b.Bits(fParams.width - 1, 16);
			b.Bits(fParams.height - 1, 16);
			b.Bit(false);			// allow_high_precision_mv
			b.Bit(false);			// is_filter_switchable
			b.Bits(0, 2);			// interpolation_filter: EIGHTTAP
			b.Bit(false);			// is_motion_mode_switchable
		} else {
			// VCN 4 codes 1080 lines as 1082 (and its own sequence header
			// says so): the picture's real size is the render size, which
			// Mesa takes from here
			b.Bit(true);			// render_and_frame_size_different
			b.Bits(fParams.width - 1, 16);
			b.Bits(fParams.height - 1, 16);
		}
		b.Bit(false);				// disable_frame_end_update_cdf
		// tile_info()
		b.Bit(true);				// uniform_tile_spacing_flag
		int maxLog2TileCols = tile_log2(1, min_c(sbColumns, 64));
		int maxLog2TileRows = tile_log2(1, min_c(sbRows, 64));
		int minLog2TileCols = tile_log2(64, sbColumns);
		for (int i = minLog2TileCols; i < maxLog2TileCols; i++) {
			bool more = i < columnsLog2;
			b.Bit(more);			// increment_tile_cols_log2
			if (!more)
				break;
		}
		int minLog2TileRows = max_c(tile_log2(2304, sbRows * sbColumns)
			- columnsLog2, 0);
		for (int i = minLog2TileRows; i < maxLog2TileRows; i++) {
			bool more = i < rowsLog2;
			b.Bit(more);			// increment_tile_rows_log2
			if (!more)
				break;
		}
		if (columnsLog2 + rowsLog2 > 0) {
			b.Bits(0, columnsLog2 + rowsLog2);	// context_update_tile_id
			b.Bits(3, 2);			// tile_size_bytes_minus_1
		}
		// quantization_params()
		b.Bits(qindex, 8);			// base_q_idx
		b.Bit(false);				// DeltaQYDc delta_coded
		b.Bit(false);				// DeltaQUDc
		b.Bit(false);				// DeltaQUAc
		b.Bit(false);				// using_qmatrix
		b.Bit(false);				// segmentation_enabled
		if (qindex > 0)
			b.Bit(false);			// delta_q_present
		// loop_filter_params()
		b.Bits(filterLevel, 6);
		b.Bits(filterLevel, 6);
		if (filterLevel != 0) {
			b.Bits(filterLevel / 2, 6);
			b.Bits(filterLevel / 2, 6);
		}
		b.Bits(0, 3);				// loop_filter_sharpness
		b.Bit(false);				// loop_filter_delta_enabled
		// read_tx_mode()
		b.Bit(true);				// tx_mode_select
		if (!picture.idr)
			b.Bit(false);			// reference_select
		b.Bit(false);				// reduced_tx_set
		if (!picture.idr) {
			for (int i = 0; i < 7; i++)
				b.Bit(false);		// is_global
		}
		b.TrailingBits();

		std::vector<uint8> header;
		size_t bits = append_obu(header, 3, b);	// OBU_FRAME_HEADER
		if (_AddPacked(picture, VAEncPackedHeaderPicture, header, bits,
				false) != B_OK)
			return B_ERROR;
	}

	VAEncTileGroupBufferAV1 tiles;
	memset(&tiles, 0, sizeof(tiles));
	tiles.tg_start = 0;
	tiles.tg_end = tileColumns * tileRows - 1;
	return _AddBuffer(picture, VAEncSliceParameterBufferType, &tiles,
		sizeof(tiles));
}


/*!	HDR10's static metadata - the mastering display and the content light
	level - for key frames: H.265 SEI messages, or AV1 metadata OBUs. Only
	for 10-bit output with the PQ transfer, where it means something. */
status_t
VaapiEncoder::_AddHdrMetadata(Picture& picture)
{
	if (fBitDepth <= 8 || fParams.transfer != AVCOL_TRC_SMPTE2084
		|| (!fParams.haveMastering && !fParams.haveLightLevel))
		return B_OK;
	const AVMasteringDisplayMetadata& m = fParams.mastering;

	if (fCodec == AV_CODEC_ID_HEVC) {
		BitWriter sei;
		if (fParams.haveMastering) {
			// mastering_display_colour_volume: G, B, R in 0.00002 units
			sei.Bits(137, 8);
			sei.Bits(24, 8);
			static const int kOrder[3] = { 1, 2, 0 };
			for (int i = 0; i < 3; i++) {
				const AVRational* primary = m.display_primaries[kOrder[i]];
				sei.Bits((uint32)lround(av_q2d(primary[0]) * 50000), 16);
				sei.Bits((uint32)lround(av_q2d(primary[1]) * 50000), 16);
			}
			sei.Bits((uint32)lround(av_q2d(m.white_point[0]) * 50000), 16);
			sei.Bits((uint32)lround(av_q2d(m.white_point[1]) * 50000), 16);
			sei.Bits((uint32)llround(av_q2d(m.max_luminance) * 10000), 32);
			sei.Bits((uint32)llround(av_q2d(m.min_luminance) * 10000), 32);
		}
		if (fParams.haveLightLevel) {
			sei.Bits(144, 8);		// content_light_level_info
			sei.Bits(4, 8);
			sei.Bits(fParams.lightLevel.MaxCLL, 16);
			sei.Bits(fParams.lightLevel.MaxFALL, 16);
		}
		sei.TrailingBits();
		std::vector<uint8> data;
		const uint8 header[2] = { 39 << 1, 1 };	// PREFIX_SEI_NUT
		size_t bits = append_annexb_nal(data, header, 2, sei);
		return _AddPacked(picture, VAEncPackedHeaderRawData, data, bits);
	}

	if (fCodec == AV_CODEC_ID_AV1) {
		std::vector<uint8> data;
		if (fParams.haveLightLevel) {
			BitWriter cll;
			cll.Bits(1, 8);			// metadata_type: HDR_CLL (leb128)
			cll.Bits(fParams.lightLevel.MaxCLL, 16);
			cll.Bits(fParams.lightLevel.MaxFALL, 16);
			cll.TrailingBits();
			append_obu(data, 5, cll);
		}
		if (fParams.haveMastering) {
			BitWriter mdcv;
			mdcv.Bits(2, 8);		// metadata_type: HDR_MDCV
			for (int i = 0; i < 3; i++) {
				mdcv.Bits((uint32)lround(av_q2d(m.display_primaries[i][0])
					* (1 << 16)), 16);
				mdcv.Bits((uint32)lround(av_q2d(m.display_primaries[i][1])
					* (1 << 16)), 16);
			}
			mdcv.Bits((uint32)lround(av_q2d(m.white_point[0]) * (1 << 16)),
				16);
			mdcv.Bits((uint32)lround(av_q2d(m.white_point[1]) * (1 << 16)),
				16);
			mdcv.Bits((uint32)llround(av_q2d(m.max_luminance) * (1 << 8)), 32);
			mdcv.Bits((uint32)llround(av_q2d(m.min_luminance) * (1 << 14)),
				32);
			mdcv.TrailingBits();
			append_obu(data, 5, mdcv);
		}
		return _AddPacked(picture, VAEncPackedHeaderRawData, data,
			data.size() * 8, false);
	}
	return B_OK;
}


// #pragma mark - pictures


status_t
VaapiEncoder::_Issue(Picture& picture)
{
	status_t status;
	switch (fCodec) {
		case AV_CODEC_ID_H264:
			status = _BuildH264(picture);
			break;
		case AV_CODEC_ID_HEVC:
			status = _BuildHevc(picture);
			break;
		case AV_CODEC_ID_AV1:
			status = _BuildAv1(picture);
			break;
		default:
			status = B_NOT_SUPPORTED;
			break;
	}
	if (status != B_OK)
		return status;

	VADisplay display = fDisplay.Display();
	if (fVA->BeginPicture(display, fContext, picture.input)
			!= VA_STATUS_SUCCESS)
		return B_ERROR;
	if (fVA->RenderPicture(display, fContext, picture.buffers.data(),
			(int)picture.buffers.size()) != VA_STATUS_SUCCESS) {
		fVA->EndPicture(display, fContext);
		return B_ERROR;
	}
	if (fVA->EndPicture(display, fContext) != VA_STATUS_SUCCESS)
		return B_ERROR;
	return B_OK;
}


status_t
VaapiEncoder::_Retrieve()
{
	if (fInFlight.empty())
		return B_OK;
	Picture picture = fInFlight.front();
	fInFlight.pop_front();

	VADisplay display = fDisplay.Display();
	status_t status = B_OK;
	bigtime_t syncStart = system_time();
	if (fVA->SyncSurface(display, picture.input) != VA_STATUS_SUCCESS)
		status = B_ERROR;
	sSyncTime += system_time() - syncStart;

	VACodedBufferSegment* segment = NULL;
	if (status == B_OK && fVA->MapBuffer(display, picture.coded,
			(void**)&segment) == VA_STATUS_SUCCESS) {
		size_t size = 0;
		for (VACodedBufferSegment* s = segment; s != NULL;
				s = (VACodedBufferSegment*)s->next)
			size += s->size;
		AVPacket* packet = av_packet_alloc();
		if (packet != NULL && av_new_packet(packet, (int)size) == 0) {
			size_t offset = 0;
			for (VACodedBufferSegment* s = segment; s != NULL;
					s = (VACodedBufferSegment*)s->next) {
				memcpy(packet->data + offset, s->buf, s->size);
				offset += s->size;
			}
			packet->pts = picture.pts;
			packet->dts = picture.pts;
			if (picture.idr)
				packet->flags |= AV_PKT_FLAG_KEY;
			fOutput.push_back(packet);
		} else {
			av_packet_free(&packet);
			status = B_NO_MEMORY;
		}
		fVA->UnmapBuffer(display, picture.coded);
	} else if (status == B_OK)
		status = B_ERROR;

	_ReleaseBuffers(picture);
	fFreeInputs.push_back(picture.input);
	fFreeCoded.push_back(picture.coded);
	if (picture.releaseRecon != VA_INVALID_SURFACE)
		fFreeRecons.push_back(picture.releaseRecon);
	return status;
}


status_t
VaapiEncoder::SendFrame(const AVFrame* frame)
{
	if (frame == NULL) {
		while (!fInFlight.empty()) {
			if (_Retrieve() != B_OK)
				return B_ERROR;
		}
		fDrained = true;
		return B_OK;
	}

	while (fFreeInputs.empty() || fFreeCoded.empty() || fFreeRecons.empty()) {
		if (fInFlight.empty())
			return B_ERROR;
		if (_Retrieve() != B_OK)
			return B_ERROR;
	}

	Picture picture;
	picture.pts = frame->pts;
	picture.idr = fCount == 0 || fIndexInGop >= fParams.keyInterval
		|| frame->pict_type == AV_PICTURE_TYPE_I;
	if (picture.idr) {
		if (fCount > 0)
			fIdrId = (fIdrId + 1) & 0xffff;
		fIndexInGop = 0;
	}
	picture.index = fIndexInGop;
	picture.input = fFreeInputs.back();
	fFreeInputs.pop_back();
	picture.recon = fFreeRecons.back();
	fFreeRecons.pop_back();
	picture.coded = fFreeCoded.back();
	fFreeCoded.pop_back();
	// the old reference is free once this picture, the last one to read it
	// (or, after an IDR, the last one after the one that wrote it), is done
	picture.releaseRecon = fHaveReference ? fReference : VA_INVALID_SURFACE;
	picture.frameNum = (uint32)(picture.index & 0xff);
	picture.order = (int32)(picture.index * (fCodec == AV_CODEC_ID_H264
		? 2 : 1));
	picture.slot = 0;

	bigtime_t uploadStart = system_time();
	status_t uploaded = _Upload(frame, picture.input);
	sUploadTime += system_time() - uploadStart;
	sUploads++;
	if (uploaded != B_OK) {
		fFreeInputs.push_back(picture.input);
		fFreeRecons.push_back(picture.recon);
		fFreeCoded.push_back(picture.coded);
		return B_ERROR;
	}
	bigtime_t issueStart = system_time();
	status_t issued = _Issue(picture);
	sIssueTime += system_time() - issueStart;
	if (issued != B_OK) {
		_ReleaseBuffers(picture);
		fFreeInputs.push_back(picture.input);
		fFreeRecons.push_back(picture.recon);
		fFreeCoded.push_back(picture.coded);
		return B_ERROR;
	}
	fInFlight.push_back(picture);

	fHaveReference = true;
	fReference = picture.recon;
	fReferenceFrameNum = picture.frameNum;
	fReferenceOrder = picture.order;
	fReferenceSlot = picture.slot;
	fCount++;
	fIndexInGop++;

	if (fInFlight.size() > kPipelineDepth)
		return _Retrieve();
	return B_OK;
}


status_t
VaapiEncoder::ReceivePacket(AVPacket* packet)
{
	if (fOutput.empty())
		return fDrained ? B_LAST_BUFFER_ERROR : B_WOULD_BLOCK;
	AVPacket* front = fOutput.front();
	fOutput.pop_front();
	av_packet_move_ref(packet, front);
	av_packet_free(&front);
	return B_OK;
}


encoder_backend
VaapiEncoder::Backend() const
{
	return kBackendVAAPI;
}


BString
VaapiEncoder::Name() const
{
	BString name;
	name.SetToFormat("%s (VA-API)", fDeviceName.String());
	return name;
}


BString
VaapiEncoder::Description() const
{
	return fDescription;
}
