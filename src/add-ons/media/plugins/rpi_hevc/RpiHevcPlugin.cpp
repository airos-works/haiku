/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	HEVC on the Raspberry Pi 4's decoder block, as a decoder add-on of the
	Media Kit (HevcDecoder does the work).

	Like rpi_mmal the add-on registers no format: a stream the block cannot
	decode would have no other decoder in the Media Kit's lookup. A player
	that knows the add-on loads it and falls back by itself; airTime does.

	Pictures are handed out in colour spaces Haiku has no constant for,
	line_count rows of luma bytes_per_row apart and then
	- 'I420': a plane of Cb and one of Cr, half as many rows of half the
	  length, or
	- 'NV12': half as many rows of Cb and Cr in pairs, or
	- 'P010' (ten bit streams, and only that for them): as NV12 with
	  sixteen bit samples, the value in the upper ten bits.

	A negative time_to_decode in Decode()'s media_decode_info is, negated,
	the time before which the caller will drop the pictures anyway (it is
	seeking); those are returned with their time but not copied. */


#include <DecoderPlugin.h>
#include <MediaFormats.h>

#include <new>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "HevcDecoder.h"


// AV_CODEC_ID_HEVC of the FFmpeg add-on's format descriptions
static const uint32 kCodecHEVC = 173;

static const color_space kColorSpaceNV12 = (color_space)0x4e563132;	// 'NV12'
static const color_space kColorSpaceI420 = (color_space)0x49343230;	// 'I420'
static const color_space kColorSpaceP010 = (color_space)0x50303130;	// 'P010'


class RpiHevcDecoder : public Decoder {
public:
								RpiHevcDecoder();
	virtual						~RpiHevcDecoder();

	virtual	void				GetCodecInfo(media_codec_info* info);
	virtual	status_t			Setup(media_format* format, const void* info,
									size_t infoSize);
	virtual	status_t			NegotiateOutputFormat(media_format* format);
	virtual	status_t			SeekedTo(int64 frame, bigtime_t time);
	virtual status_t			Decode(void* buffer, int64* frameCount,
									media_header* header,
									media_decode_info* info);

private:
			status_t			_NextPicture(HevcDecoder::Picture& picture);
			status_t			_PutUnits(const uint8* data, size_t size,
									int64 time);
			void				_SetParameterSets(const uint8* data,
									size_t size);
			uint32				_Stride(const HevcDecoder::Picture& picture)
									const;

			HevcDecoder			fDecoder;
			media_format		fInputFormat;
			uint32				fNalLengthSize;
				// of the stream's NAL units; 0 when they come with start
				// codes
			std::vector<uint8>	fParameterSets;	// with start codes
			bool				fParameterSetsSent;
			bool				fInputEnded;

			color_space			fOutputSpace;
			bool				fHalfSize;
			bool				fHavePicture;
			HevcDecoder::Picture fPicture;
			uint32				fWidth;
			uint32				fHeight;
			uint32				fStreamWidth;
			uint32				fStreamHeight;
};


RpiHevcDecoder::RpiHevcDecoder()
	:
	fNalLengthSize(0),
	fParameterSetsSent(false),
	fInputEnded(false),
	fOutputSpace(kColorSpaceI420),
		fHalfSize(false),
	fHavePicture(false),
	fWidth(0),
	fHeight(0)
{
}


RpiHevcDecoder::~RpiHevcDecoder()
{
	fDecoder.Close();
}


void
RpiHevcDecoder::GetCodecInfo(media_codec_info* info)
{
	memset(info, 0, sizeof(*info));
	strlcpy(info->pretty_name, "Raspberry Pi hardware HEVC decoder",
		sizeof(info->pretty_name));
	strlcpy(info->short_name, "rpi_hevc", sizeof(info->short_name));
}


/*!	The parameter sets of an MP4 or Matroska stream ("hvcC") with start
	codes; \a data that has start codes already is taken as it is.
*/
void
RpiHevcDecoder::_SetParameterSets(const uint8* data, size_t size)
{
	static const uint8 kStartCode[] = {0, 0, 0, 1};

	fParameterSets.clear();
	fNalLengthSize = 0;
	if (data == NULL || size < 4)
		return;

	if (data[0] == 0 && data[1] == 0 && (data[2] == 1
			|| (data[2] == 0 && data[3] == 1))) {
		fParameterSets.assign(data, data + size);
		return;
	}
	if (size < 23)
		return;

	fNalLengthSize = (data[21] & 3) + 1;
	uint32 arrays = data[22];
	size_t offset = 23;
	for (uint32 i = 0; i < arrays && offset + 3 <= size; i++) {
		uint32 count = ((uint32)data[offset + 1] << 8) | data[offset + 2];
		offset += 3;
		for (uint32 j = 0; j < count && offset + 2 <= size; j++) {
			size_t length = ((size_t)data[offset] << 8) | data[offset + 1];
			offset += 2;
			if (offset + length > size)
				return;
			fParameterSets.insert(fParameterSets.end(), kStartCode,
				kStartCode + 4);
			fParameterSets.insert(fParameterSets.end(), data + offset,
				data + offset + length);
			offset += length;
		}
	}
}


status_t
RpiHevcDecoder::Setup(media_format* format, const void* info, size_t infoSize)
{
	if (format == NULL || format->type != B_MEDIA_ENCODED_VIDEO)
		return B_NOT_SUPPORTED;

	media_format_description description;
	if (BMediaFormats().GetCodeFor(*format, B_MISC_FORMAT_FAMILY,
			&description) != B_OK
		|| description.u.misc.file_format != 'ffmp'
		|| description.u.misc.codec != kCodecHEVC) {
		return B_NOT_SUPPORTED;
	}

	const media_video_display_info& display
		= format->u.encoded_video.output.display;
	if (display.line_width > 4096 || display.line_count > 4096)
		return B_NOT_SUPPORTED;

	fInputFormat = *format;
	_SetParameterSets((const uint8*)info, infoSize);

	BString error;
	status_t status = fDecoder.Open(&error);
	if (status != B_OK) {
		fprintf(stderr, "rpi_hevc: %s\n", error.String());
		return status;
	}
	return B_OK;
}


uint32
RpiHevcDecoder::_Stride(const HevcDecoder::Picture& picture) const
{
	uint32 samples = (picture.width + 15) & ~15u;
	return picture.bitDepth > 8 ? 2 * samples : samples;
}


status_t
RpiHevcDecoder::NegotiateOutputFormat(media_format* format)
{
	if (format == NULL || format->type != B_MEDIA_RAW_VIDEO)
		return B_MEDIA_BAD_FORMAT;
	color_space space = format->u.raw_video.display.format;
	if (space != kColorSpaceNV12 && space != kColorSpaceI420
		&& space != kColorSpaceP010) {
		return B_MEDIA_BAD_FORMAT;
	}

	// The first picture tells how large they are and how deep.
	if (!fHavePicture) {
		status_t status = _NextPicture(fPicture);
		if (status != B_OK)
			return status;
		fHavePicture = true;
	}
	if ((fPicture.bitDepth > 8) != (space == kColorSpaceP010))
		return B_MEDIA_BAD_FORMAT;

	// full size, or half for a player whose screen shows the film at no
	// more than half: a 4K film on a 1080p screen, where the copy out and
	// the scaling each cost about as much as the decoding did
	uint32 requestWidth = format->u.raw_video.display.line_width;
	uint32 requestHeight = format->u.raw_video.display.line_count;
	fHalfSize = false;
	if ((requestWidth > 0 || requestHeight > 0)
		&& (requestWidth != fPicture.width
			|| requestHeight != fPicture.height)) {
		if (requestWidth != (fPicture.width + 1) / 2
			|| requestHeight != (fPicture.height + 1) / 2) {
			return B_MEDIA_BAD_FORMAT;
		}
		fHalfSize = true;
	}
	fOutputSpace = space;
	fWidth = fHalfSize ? (fPicture.width + 1) / 2 : fPicture.width;
	fHeight = fHalfSize ? (fPicture.height + 1) / 2 : fPicture.height;
	fStreamWidth = fPicture.width;
	fStreamHeight = fPicture.height;

	media_raw_video_format raw = fInputFormat.u.encoded_video.output;
	raw.interlace = 1;
	raw.first_active = 0;
	raw.last_active = fHeight - 1;
	raw.orientation = B_VIDEO_TOP_LEFT_RIGHT;
	raw.display.format = fOutputSpace;
	raw.display.line_width = fWidth;
	raw.display.line_count = fHeight;
	uint32 samples = (fWidth + 15) & ~15u;
	raw.display.bytes_per_row = _Stride(fPicture) / (fHalfSize ? 2 : 1);
	if (fHalfSize && samples * (fPicture.bitDepth > 8 ? 2 : 1)
		> raw.display.bytes_per_row) {
		// odd widths round up past half of the full size stride
		raw.display.bytes_per_row = samples * (fPicture.bitDepth > 8 ? 2 : 1);
	}
	raw.display.pixel_offset = 0;
	raw.display.line_offset = 0;
	raw.display.flags = 0;

	format->type = B_MEDIA_RAW_VIDEO;
	format->require_flags = 0;
	format->deny_flags = B_MEDIA_MAUI_UNDEFINED_FLAGS;
	format->u.raw_video = raw;
	return B_OK;
}


status_t
RpiHevcDecoder::SeekedTo(int64 frame, bigtime_t time)
{
	if (fHavePicture) {
		fDecoder.ReleasePicture(fPicture);
		fHavePicture = false;
	}
	fDecoder.Reset();
	fInputEnded = false;
	fParameterSetsSent = false;
	return B_OK;
}


/*!	The NAL units of a chunk: each with its length in front, or between
	start codes.
*/
status_t
RpiHevcDecoder::_PutUnits(const uint8* data, size_t size, int64 time)
{
	if (fNalLengthSize != 0) {
		size_t offset = 0;
		while (offset + fNalLengthSize <= size) {
			size_t length = 0;
			for (uint32 i = 0; i < fNalLengthSize; i++)
				length = (length << 8) | data[offset++];
			if (length > size - offset)
				length = size - offset;
			status_t status = fDecoder.PutNal(data + offset, length, time);
			if (status != B_OK)
				return status;
			offset += length;
		}
		return B_OK;
	}

	size_t position = 0;
	while (position + 3 <= size) {
		size_t begin = position;
		while (begin + 3 <= size && !(data[begin] == 0 && data[begin + 1] == 0
				&& data[begin + 2] == 1)) {
			begin++;
		}
		if (begin + 3 > size)
			break;
		begin += 3;
		size_t end = begin;
		while (end + 3 <= size && !(data[end] == 0 && data[end + 1] == 0
				&& (data[end + 2] == 1 || data[end + 2] == 0))) {
			end++;
		}
		if (end + 3 > size)
			end = size;
		status_t status = fDecoder.PutNal(data + begin, end - begin, time);
		if (status != B_OK)
			return status;
		position = end;
	}
	return B_OK;
}


status_t
RpiHevcDecoder::_NextPicture(HevcDecoder::Picture& picture)
{
	while (true) {
		if (fDecoder.NextPicture(picture))
			return B_OK;
		if (fInputEnded)
			return B_LAST_BUFFER_ERROR;

		const void* chunk;
		size_t size;
		media_header header;
		status_t status = GetNextChunk(&chunk, &size, &header);
		if (status == B_LAST_BUFFER_ERROR) {
			fInputEnded = true;
			fDecoder.Drain();
			continue;
		}
		if (status != B_OK)
			return status;

		// after the start and a seek, the parameter sets again
		if (!fParameterSetsSent && !fParameterSets.empty()) {
			uint32 lengthSize = fNalLengthSize;
			fNalLengthSize = 0;
			status = _PutUnits(fParameterSets.data(), fParameterSets.size(),
				header.start_time);
			fNalLengthSize = lengthSize;
		}
		fParameterSetsSent = true;

		// a chunk is an access unit: the picture is complete with it
		if (status == B_OK)
			status = _PutUnits((const uint8*)chunk, size, header.start_time);
		if (status == B_OK)
			status = fDecoder.EndAccessUnit();
		if (status != B_OK) {
			fprintf(stderr, "rpi_hevc: %s\n", fDecoder.Error());
			return B_ERROR;
		}
	}
}


status_t
RpiHevcDecoder::Decode(void* buffer, int64* frameCount, media_header* header,
	media_decode_info* info)
{
	if (buffer == NULL || frameCount == NULL || header == NULL)
		return B_BAD_VALUE;

	fDecoder.SetSkipBefore(info != NULL && info->time_to_decode < 0
		? -info->time_to_decode : INT64_MIN);

	if (!fHavePicture) {
		status_t status = _NextPicture(fPicture);
		if (status != B_OK)
			return status;
	}
	fHavePicture = false;

	if (fPicture.width != fStreamWidth || fPicture.height != fStreamHeight
		|| (fPicture.bitDepth > 8) != (fOutputSpace == kColorSpaceP010)) {
		// the stream changed its pictures; nothing here renegotiates
		fDecoder.ReleasePicture(fPicture);
		fprintf(stderr, "rpi_hevc: the pictures changed to %" B_PRIu32 "x%"
			B_PRIu32 ", %" B_PRIu32 " bit\n", fPicture.width,
			fPicture.height, fPicture.bitDepth);
		return B_MEDIA_BAD_FORMAT;
	}

	uint32 stride = (fWidth + 15) & ~15u;
	if (fPicture.bitDepth > 8)
		stride *= 2;
	bool skip = info != NULL && info->time_to_decode < 0
		&& fPicture.pts < -info->time_to_decode;
	if (!skip) {
		fDecoder.CopyPlanes(fPicture, (uint8*)buffer, stride,
			fOutputSpace == kColorSpaceNV12, fHalfSize);
	}

	memset(header, 0, sizeof(*header));
	header->type = B_MEDIA_RAW_VIDEO;
	header->start_time = fPicture.pts;
	header->size_used = (size_t)stride * (fHeight + (fHeight + 1) / 2);
	header->u.raw_video.display_line_width = fWidth;
	header->u.raw_video.display_line_count = fHeight;
	header->u.raw_video.bytes_per_row = stride;
	header->u.raw_video.line_count = fHeight;
	*frameCount = 1;

	fDecoder.ReleasePicture(fPicture);
	return B_OK;
}


//	#pragma mark -


class RpiHevcPlugin : public DecoderPlugin {
public:
	virtual	Decoder*			NewDecoder(uint index);
	virtual	status_t			GetSupportedFormats(media_format** formats,
									size_t* count);
};


Decoder*
RpiHevcPlugin::NewDecoder(uint index)
{
	return new(std::nothrow) RpiHevcDecoder;
}


status_t
RpiHevcPlugin::GetSupportedFormats(media_format** formats, size_t* count)
{
	// none for the Media Kit's lookup, see above
	static media_format sNone;
	*formats = &sNone;
	*count = 0;
	return B_OK;
}


MediaPlugin*
instantiate_plugin()
{
	return new(std::nothrow) RpiHevcPlugin;
}
