/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	H.264 and HEVC on the SC8280XP's video codec (Radxa Dragon Q8B), as a
	decoder add-on of the Media Kit (IrisDecoder does the work).

	As sunxi_cedar: the formats are only registered where the codec is
	(/dev/video/qcom_iris/0 exists), and a stream it does not decode (fields,
	more than eight bits, not 4:2:0, larger than 4096x2304), or one the
	firmware has no room for next to others, goes on in software: the decoder then hands
	everything to one of the FFmpeg add-on's decoders. What the firmware
	only finds out at the first picture sends the chunks read until then to
	the FFmpeg decoder again.

	airTime asks for decoder 1, which has no such fallback: it falls back to
	libavcodec itself.

	Pictures are handed out as 'I420', 'NV12', B_YCbCr422 or B_RGB32 (see
	sunxi_cedar); 'I420' half as wide and high when asked for exactly that.
	A negative time_to_decode in Decode()'s media_decode_info is, negated,
	the time before which the caller will drop the pictures anyway; those
	are returned with their time but not copied. */


#include <DecoderPlugin.h>
#include <FindDirectory.h>
#include <MediaFormats.h>
#include <image.h>

#include <deque>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vector>

#include <qcom_iris.h>

#include "IrisDecoder.h"
#include "h264_parse.h"
#include "hevc_parse.h"


// AV_CODEC_ID_H264 and AV_CODEC_ID_HEVC of the FFmpeg add-on's format
// descriptions
static const uint32 kCodecH264 = 27;
static const uint32 kCodecHEVC = 173;

static const color_space kColorSpaceNV12 = (color_space)0x4e563132;	// 'NV12'
static const color_space kColorSpaceI420 = (color_space)0x49343230;	// 'I420'

static const uint32 kMaxWidth = 4096;
static const uint32 kMaxHeight = 2304;

// NewDecoder()'s index for a decoder without the software fallback
static const uint kCodecOnly = 1;

// how long the firmware may take for a picture before the stream is given up
static const bigtime_t kPictureTimeout = 2000000;


class QcomIrisDecoder;


/*!	The chunks for the FFmpeg decoder: first those this decoder read before
	it gave up, then the stream's. Libmedia's Decoder deletes its provider.
*/
class ChunkReplay : public ChunkProvider {
public:
								ChunkReplay(QcomIrisDecoder* owner)
									: fOwner(owner) {}

	virtual	status_t			GetNextChunk(const void** chunk, size_t* size,
									media_header* header);

private:
			QcomIrisDecoder*	fOwner;
};


class QcomIrisDecoder : public Decoder {
public:
								QcomIrisDecoder(bool fallback);
	virtual						~QcomIrisDecoder();

	virtual	void				GetCodecInfo(media_codec_info* info);
	virtual	status_t			Setup(media_format* format, const void* info,
									size_t infoSize);
	virtual	status_t			NegotiateOutputFormat(media_format* format);
	virtual	status_t			SeekedTo(int64 frame, bigtime_t time);
	virtual status_t			Decode(void* buffer, int64* frameCount,
									media_header* header,
									media_decode_info* info);

			status_t			ReplayChunk(const void** chunk, size_t* size,
									media_header* header);

private:
	struct Chunk {
		std::vector<uint8>	data;
		media_header		header;
	};

			status_t			_OpenDecoder(BString& reason);
			status_t			_NextPicture(IrisDecoder::Picture& picture);
			status_t			_ReadChunk(const void** chunk, size_t* size,
									media_header* header);
			void				_MakeAccessUnit(const uint8* data,
									size_t size);
			void				_AppendUnit(const uint8* data, size_t size);
			void				_SetParameterSets(const uint8* data,
									size_t size);
			void				_AddParameterSet(const uint8* data,
									size_t size);
			bool				_CodecDecodes(BString& reason) const;
			status_t			_StartSoftware(const char* reason);
			uint32				_Stride() const;

			const bool			fFallback;
			IrisDecoder*		fDecoder;
			uint32				fCodec;
			media_format		fInputFormat;		// as Setup() got it
			std::vector<uint8>	fInfo;
			uint32				fNalLengthSize;
				// of the stream's NAL units; 0 when they come with start
				// codes
			std::vector<uint8>	fParameterSets;	// with start codes
			bool				fParameterSetsSent;
			bool				fInputEnded;

			// the access unit the codec had no room for yet
			std::vector<uint8>	fUnit;
			bool				fUnitPending;
			int64				fUnitTime;

			color_space			fOutputSpace;
			bool				fHavePicture;
			IrisDecoder::Picture fPicture;
			uint32				fWidth;
			uint32				fHeight;
			bool				fHalf;

			// chunks kept until the first picture, for the software
			// decoder, and those it has yet to get
			bool				fRecording;
			std::deque<Chunk>	fRecorded;
			Chunk				fReplaying;

			// the software decoder, when the codec does not do the stream
			image_id			fSoftwareImage;
			MediaPlugin*		fSoftwarePlugin;
			Decoder*			fSoftware;
};


status_t
ChunkReplay::GetNextChunk(const void** chunk, size_t* size,
	media_header* header)
{
	return fOwner->ReplayChunk(chunk, size, header);
}


QcomIrisDecoder::QcomIrisDecoder(bool fallback)
	:
	fFallback(fallback),
	fDecoder(NULL),
	fCodec(0),
	fNalLengthSize(0),
	fParameterSetsSent(false),
	fInputEnded(false),
	fUnitPending(false),
	fUnitTime(0),
	fOutputSpace(kColorSpaceI420),
	fHavePicture(false),
	fWidth(0),
	fHeight(0),
	fHalf(false),
	fRecording(false),
	fSoftwareImage(-1),
	fSoftwarePlugin(NULL),
	fSoftware(NULL)
{
}


QcomIrisDecoder::~QcomIrisDecoder()
{
	delete fDecoder;
	// the FFmpeg decoder's code goes with its image: it goes first
	delete fSoftware;
	delete fSoftwarePlugin;
	if (fSoftwareImage >= 0)
		unload_add_on(fSoftwareImage);
}


void
QcomIrisDecoder::GetCodecInfo(media_codec_info* info)
{
	if (fSoftware != NULL) {
		fSoftware->GetCodecInfo(info);
		return;
	}
	memset(info, 0, sizeof(*info));
	strlcpy(info->pretty_name, fCodec == kCodecHEVC
			? "Qualcomm Iris hardware HEVC decoder"
			: "Qualcomm Iris hardware H.264 decoder",
		sizeof(info->pretty_name));
	strlcpy(info->short_name, "qcom_video", sizeof(info->short_name));
}


void
QcomIrisDecoder::_AddParameterSet(const uint8* data, size_t size)
{
	static const uint8 kStartCode[] = {0, 0, 0, 1};
	fParameterSets.insert(fParameterSets.end(), kStartCode, kStartCode + 4);
	fParameterSets.insert(fParameterSets.end(), data, data + size);
}


/*!	The parameter sets of an MP4 or Matroska stream ("avcC" or "hvcC")
	with start codes; \a data that has start codes already is taken as it
	is.
*/
void
QcomIrisDecoder::_SetParameterSets(const uint8* data, size_t size)
{
	fParameterSets.clear();
	fNalLengthSize = 0;
	if (data == NULL || size < 4)
		return;

	if (data[0] == 0 && data[1] == 0 && (data[2] == 1
			|| (data[2] == 0 && data[3] == 1))) {
		fParameterSets.assign(data, data + size);
		return;
	}

	if (fCodec == kCodecH264) {
		if (size < 7 || data[0] != 1)
			return;
		fNalLengthSize = (data[4] & 3) + 1;
		size_t offset = 5;
		for (int kind = 0; kind < 2 && offset < size; kind++) {
			uint32 count = kind == 0 ? data[offset] & 0x1f : data[offset];
			offset++;
			for (uint32 i = 0; i < count && offset + 2 <= size; i++) {
				size_t length = ((size_t)data[offset] << 8)
					| data[offset + 1];
				offset += 2;
				if (offset + length > size)
					return;
				_AddParameterSet(data + offset, length);
				offset += length;
			}
		}
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
			_AddParameterSet(data + offset, length);
			offset += length;
		}
	}
}


/*!	Whether the sequence parameter sets that came with the stream are of a
	kind the codec decodes (sunxi_cedar's parsers). Without any, nothing can
	be said yet.
*/
bool
QcomIrisDecoder::_CodecDecodes(BString& reason) const
{
	size_t position = 0;
	const uint8_t* nal;
	size_t size;
	char error[160];
	bool decodes = true;

	if (fCodec == kCodecH264) {
		H264State* state = new(std::nothrow) H264State;
		if (state == NULL)
			return true;
		h264_init(state);
		while (decodes && annexb_next(fParameterSets.data(),
				fParameterSets.size(), &position, &nal, &size)) {
			if (size < 1 || (nal[0] & 0x1f) != 7)
				continue;
			Rbsp rbsp;
			if (rbsp_from_nal(&rbsp, nal, size) != 0)
				continue;
			BitReader reader;
			br_init(&reader, rbsp.data, rbsp.size);
			int result = h264_parse_sps(state, &reader, error);
			rbsp_free(&rbsp);
			if (result != 0) {
				reason.SetToFormat("its sequence parameter set: %s", error);
				decodes = result != -2;
				continue;
			}
			for (int i = 0; i < 32 && decodes; i++) {
				const H264Sps& sps = state->sps[i];
				if (!sps.valid)
					continue;
				if (sps.chromaFormatIdc != 1 || sps.bitDepthLuma != 8
					|| sps.bitDepthChroma != 8) {
					reason = "the codec decodes eight bit 4:2:0 only";
					decodes = false;
				} else if (!sps.frameMbsOnly) {
					reason = "the codec decodes no fields";
					decodes = false;
				} else if (sps.profileIdc == 88
					&& (sps.constraintFlags & 0xc0) == 0) {
					reason = "the codec has no SP and SI slices or data "
						"partitioning (Extended profile)";
					decodes = false;
				} else if ((uint32)sps.widthMbs * 16 > kMaxWidth
					|| (uint32)sps.heightMapUnits * 16 > kMaxHeight) {
					reason = "the codec decodes up to 4096x2304";
					decodes = false;
				}
			}
		}
		delete state;
		return decodes;
	}

	HevcState* state = new(std::nothrow) HevcState;
	if (state == NULL)
		return true;
	hevc_init(state);
	while (decodes && annexb_next(fParameterSets.data(), fParameterSets.size(),
			&position, &nal, &size)) {
		if (size < 2 || ((nal[0] >> 1) & 0x3f) != 33)
			continue;
		Rbsp rbsp;
		if (rbsp_from_nal(&rbsp, nal, size) != 0)
			continue;
		BitReader reader;
		br_init(&reader, rbsp.data, rbsp.size);
		int result = hevc_parse_sps(state, &reader, error);
		rbsp_free(&rbsp);
		if (result != 0) {
			reason.SetToFormat("its sequence parameter set: %s", error);
			decodes = result != -2;
			continue;
		}
		for (int i = 0; i < 16 && decodes; i++) {
			const HevcSps& sps = state->sps[i];
			if (!sps.valid)
				continue;
			if (sps.chromaFormatIdc != 1 || sps.bitDepthLuma != 8
				|| sps.bitDepthChroma != 8) {
				reason = "the codec decodes HEVC Main (eight bit 4:2:0) only";
				decodes = false;
			} else if ((uint32)sps.width > kMaxWidth
				|| (uint32)sps.height > kMaxHeight) {
				reason = "the codec decodes up to 4096x2304";
				decodes = false;
			}
		}
	}
	delete state;
	return decodes;
}


/*!	The rest of the stream goes to one of the FFmpeg add-on's decoders. */
status_t
QcomIrisDecoder::_StartSoftware(const char* reason)
{
	if (!fFallback)
		return B_NOT_SUPPORTED;

	char** paths = NULL;
	size_t count = 0;
	status_t status = find_paths(B_FIND_PATH_ADD_ONS_DIRECTORY,
		"media/plugins/ffmpeg", &paths, &count);
	if (status != B_OK)
		return status;
	for (size_t i = 0; i < count && fSoftwareImage < 0; i++) {
		if (access(paths[i], R_OK) == 0)
			fSoftwareImage = load_add_on(paths[i]);
	}
	free(paths);
	if (fSoftwareImage < 0)
		return B_NOT_SUPPORTED;

	MediaPlugin* (*instantiate)() = NULL;
	if (get_image_symbol(fSoftwareImage, "instantiate_plugin",
			B_SYMBOL_TYPE_TEXT, (void**)&instantiate) == B_OK
		&& instantiate != NULL) {
		fSoftwarePlugin = instantiate();
	}
	DecoderPlugin* plugin = dynamic_cast<DecoderPlugin*>(fSoftwarePlugin);
	if (plugin != NULL)
		fSoftware = plugin->NewDecoder(0);
	if (fSoftware == NULL)
		return B_NOT_SUPPORTED;

	fprintf(stderr, "qcom_iris: in software (%s)\n", reason);
	fSoftware->SetChunkProvider(new(std::nothrow) ChunkReplay(this));
	media_format format = fInputFormat;
	status = fSoftware->Setup(&format, fInfo.data(), fInfo.size());
	if (status != B_OK)
		return status;

	// the codec's part is over
	if (fDecoder != NULL && fHavePicture)
		fDecoder->ReleasePicture(fPicture);
	fHavePicture = false;
	delete fDecoder;
	fDecoder = NULL;
	return B_OK;
}


status_t
QcomIrisDecoder::ReplayChunk(const void** chunk, size_t* size,
	media_header* header)
{
	if (!fRecorded.empty()) {
		fReplaying = fRecorded.front();
		fRecorded.pop_front();
		*chunk = fReplaying.data.data();
		*size = fReplaying.data.size();
		*header = fReplaying.header;
		return B_OK;
	}
	return GetNextChunk(chunk, size, header);
}


status_t
QcomIrisDecoder::_ReadChunk(const void** chunk, size_t* size,
	media_header* header)
{
	status_t status = GetNextChunk(chunk, size, header);
	if (status == B_OK && fRecording) {
		Chunk recorded;
		recorded.data.assign((const uint8*)*chunk,
			(const uint8*)*chunk + *size);
		recorded.header = *header;
		fRecorded.push_back(recorded);
	}
	return status;
}


status_t
QcomIrisDecoder::_OpenDecoder(BString& reason)
{
	fDecoder = new(std::nothrow) IrisDecoder(fCodec == kCodecHEVC
		? IrisDecoder::HEVC : IrisDecoder::H264);
	if (fDecoder == NULL)
		return B_NO_MEMORY;
	const media_video_display_info& display
		= fInputFormat.u.encoded_video.output.display;
	status_t status = fDecoder->Open(display.line_width, display.line_count,
		&reason);
	if (status != B_OK) {
		delete fDecoder;
		fDecoder = NULL;
	}
	return status;
}


status_t
QcomIrisDecoder::Setup(media_format* format, const void* info,
	size_t infoSize)
{
	if (format == NULL || format->type != B_MEDIA_ENCODED_VIDEO)
		return B_NOT_SUPPORTED;

	media_format_description description;
	if (BMediaFormats().GetCodeFor(*format, B_MISC_FORMAT_FAMILY,
			&description) != B_OK
		|| description.u.misc.file_format != 'ffmp'
		|| (description.u.misc.codec != kCodecH264
			&& description.u.misc.codec != kCodecHEVC)) {
		return B_NOT_SUPPORTED;
	}

	fCodec = description.u.misc.codec;
	fInputFormat = *format;
	fInfo.assign((const uint8*)info, (const uint8*)info + infoSize);
	_SetParameterSets((const uint8*)info, infoSize);

	const media_video_display_info& display
		= format->u.encoded_video.output.display;
	BString reason;
	if (display.line_width > kMaxWidth || display.line_count > kMaxHeight)
		reason = "the codec decodes up to 4096x2304";
	else if (!_CodecDecodes(reason)) {
		// (reason says why)
	} else if (_OpenDecoder(reason) == B_OK) {
		fRecording = fFallback;
		return B_OK;
	}

	if (!fFallback) {
		fprintf(stderr, "qcom_iris: %s\n", reason.String());
		return B_NOT_SUPPORTED;
	}
	return _StartSoftware(reason.String());
}


uint32
QcomIrisDecoder::_Stride() const
{
	if (fOutputSpace == B_YCbCr422)
		return (fWidth * 2 + 3) & ~3u;
	if (fOutputSpace == B_RGB32)
		return fWidth * 4;
	return (fWidth + 15) & ~15u;
}


status_t
QcomIrisDecoder::NegotiateOutputFormat(media_format* format)
{
	if (format == NULL)
		return B_BAD_VALUE;
	if (fSoftware != NULL)
		return fSoftware->NegotiateOutputFormat(format);
	if (fDecoder == NULL)
		return B_NO_INIT;

	// The first picture tells how large they are; the firmware may find out
	// only then that it cannot decode them.
	if (!fHavePicture) {
		status_t status = _NextPicture(fPicture);
		if (status != B_OK && fFallback && fDecoder->Error()[0] != '\0') {
			BString reason = fDecoder->Error();
			status = _StartSoftware(reason.String());
			if (status == B_OK)
				return fSoftware->NegotiateOutputFormat(format);
		}
		if (status != B_OK)
			return status;
		fHavePicture = true;
	}
	fRecording = false;
	fRecorded.clear();

	// (MediaPlayer leaves the format's type unset and only names the colour
	// space)
	color_space space = format->u.raw_video.display.format;
	if (space != kColorSpaceNV12 && space != kColorSpaceI420
		&& space != B_YCbCr422) {
		space = B_RGB32;
	}
	fOutputSpace = space;
	const media_video_display_info& asked = format->u.raw_video.display;
	fHalf = space == kColorSpaceI420
		&& fPicture.width % 4 == 0 && fPicture.height % 4 == 0
		&& asked.line_width == fPicture.width / 2
		&& asked.line_count == fPicture.height / 2;
	fWidth = fHalf ? fPicture.width / 2 : fPicture.width;
	fHeight = fHalf ? fPicture.height / 2 : fPicture.height;

	media_raw_video_format raw = fInputFormat.u.encoded_video.output;
	raw.interlace = 1;
	raw.first_active = 0;
	raw.last_active = fHeight - 1;
	raw.orientation = B_VIDEO_TOP_LEFT_RIGHT;
	raw.display.format = fOutputSpace;
	raw.display.line_width = fWidth;
	raw.display.line_count = fHeight;
	raw.display.bytes_per_row = _Stride();
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
QcomIrisDecoder::SeekedTo(int64 frame, bigtime_t time)
{
	if (fSoftware != NULL)
		return fSoftware->SeekedTo(frame, time);
	if (fDecoder == NULL)
		return B_NO_INIT;
	if (fHavePicture) {
		fDecoder->ReleasePicture(fPicture);
		fHavePicture = false;
	}
	fUnitPending = false;
	fInputEnded = false;
	fParameterSetsSent = false;
	fRecorded.clear();

	// a session that saw the end of the stream does not go on: a new one
	if (fDecoder->EndOfStreamSent() || fDecoder->Flush() != B_OK) {
		delete fDecoder;
		fDecoder = NULL;
		BString reason;
		status_t status = _OpenDecoder(reason);
		if (status != B_OK) {
			fprintf(stderr, "qcom_iris: %s\n", reason.String());
			return status;
		}
	}
	return B_OK;
}


void
QcomIrisDecoder::_AppendUnit(const uint8* data, size_t size)
{
	static const uint8 kStartCode[] = {0, 0, 0, 1};
	fUnit.insert(fUnit.end(), kStartCode, kStartCode + 4);
	fUnit.insert(fUnit.end(), data, data + size);
}


/*!	A chunk as the firmware takes it: Annex B, after a start or a seek with
	the parameter sets in front.
*/
void
QcomIrisDecoder::_MakeAccessUnit(const uint8* data, size_t size)
{
	fUnit.clear();
	if (!fParameterSetsSent)
		fUnit = fParameterSets;
	fParameterSetsSent = true;

	if (fNalLengthSize == 0) {
		fUnit.insert(fUnit.end(), data, data + size);
		return;
	}
	size_t offset = 0;
	while (offset + fNalLengthSize <= size) {
		size_t length = 0;
		for (uint32 i = 0; i < fNalLengthSize; i++)
			length = (length << 8) | data[offset++];
		if (length > size - offset)
			length = size - offset;
		_AppendUnit(data + offset, length);
		offset += length;
	}
}


status_t
QcomIrisDecoder::_NextPicture(IrisDecoder::Picture& picture)
{
	while (true) {
		status_t status = fDecoder->NextPicture(picture, 0);
		if (status == B_OK || status == B_LAST_BUFFER_ERROR)
			return status;
		if (status != B_WOULD_BLOCK) {
			fprintf(stderr, "qcom_iris: %s\n", fDecoder->Error());
			return B_ERROR;
		}

		if (fInputEnded) {
			// what the firmware still holds
			status = fDecoder->NextPicture(picture, kPictureTimeout);
			return status == B_WOULD_BLOCK ? B_LAST_BUFFER_ERROR : status;
		}

		if (!fUnitPending) {
			const void* chunk;
			size_t size;
			media_header header;
			status = _ReadChunk(&chunk, &size, &header);
			if (status == B_LAST_BUFFER_ERROR) {
				fInputEnded = true;
				fDecoder->EndOfStream();
				continue;
			}
			if (status != B_OK)
				return status;
			_MakeAccessUnit((const uint8*)chunk, size);
			fUnitTime = header.start_time;
			fUnitPending = true;
		}

		// no room for it: the pictures go first
		status = fDecoder->PutAccessUnit(fUnit.data(), fUnit.size(),
			fUnitTime, kPictureTimeout);
		if (status == B_OK)
			fUnitPending = false;
		else if (status != B_WOULD_BLOCK) {
			fprintf(stderr, "qcom_iris: %s\n", fDecoder->Error());
			return status == B_NOT_SUPPORTED ? status : B_ERROR;
		} else if (fDecoder->NextPicture(picture, 0) == B_OK)
			return B_OK;
		else {
			fprintf(stderr, "qcom_iris: the firmware takes no input\n");
			return B_ERROR;
		}
	}
}


status_t
QcomIrisDecoder::Decode(void* buffer, int64* frameCount,
	media_header* header, media_decode_info* info)
{
	if (fSoftware != NULL)
		return fSoftware->Decode(buffer, frameCount, header, info);
	if (fDecoder == NULL)
		return B_NO_INIT;
	if (buffer == NULL || frameCount == NULL || header == NULL)
		return B_BAD_VALUE;

	if (!fHavePicture) {
		status_t status = _NextPicture(fPicture);
		if (status != B_OK)
			return status;
	}
	fHavePicture = false;

	if (fPicture.width != (fHalf ? fWidth * 2 : fWidth)
		|| fPicture.height != (fHalf ? fHeight * 2 : fHeight)) {
		fDecoder->ReleasePicture(fPicture);
		fprintf(stderr, "qcom_iris: the pictures changed to %" B_PRIu32
			"x%" B_PRIu32 "\n", fPicture.width, fPicture.height);
		return B_MEDIA_BAD_FORMAT;
	}

	uint32 stride = _Stride();
	bool skip = info != NULL && info->time_to_decode < 0
		&& fPicture.pts < -info->time_to_decode;
	if (!skip) {
		if (fHalf)
			fDecoder->CopyHalfPlanes(fPicture, (uint8*)buffer, stride);
		else if (fOutputSpace == kColorSpaceNV12
			|| fOutputSpace == kColorSpaceI420) {
			fDecoder->CopyPlanes(fPicture, (uint8*)buffer, stride,
				fOutputSpace == kColorSpaceNV12);
		} else
			fDecoder->CopyPacked(fPicture, (uint8*)buffer, stride, fOutputSpace);
	}

	memset(header, 0, sizeof(*header));
	header->type = B_MEDIA_RAW_VIDEO;
	header->start_time = fPicture.pts;
	header->size_used = fOutputSpace == kColorSpaceNV12
			|| fOutputSpace == kColorSpaceI420
		? (size_t)stride * (fHeight + (fHeight + 1) / 2)
		: (size_t)stride * fHeight;
	header->u.raw_video.display_line_width = fWidth;
	header->u.raw_video.display_line_count = fHeight;
	header->u.raw_video.bytes_per_row = stride;
	header->u.raw_video.line_count = fHeight;
	*frameCount = 1;

	fDecoder->ReleasePicture(fPicture);
	return B_OK;
}


//	#pragma mark -


class QcomIrisPlugin : public DecoderPlugin {
public:
	virtual	Decoder*			NewDecoder(uint index);
	virtual	status_t			GetSupportedFormats(media_format** formats,
									size_t* count);
};


Decoder*
QcomIrisPlugin::NewDecoder(uint index)
{
	return new(std::nothrow) QcomIrisDecoder(index != kCodecOnly);
}


status_t
QcomIrisPlugin::GetSupportedFormats(media_format** _formats, size_t* _count)
{
	static media_format sFormats[2];
	*_formats = sFormats;
	*_count = 0;

	// none where the codec is not (the arm64 packages are the same for
	// every board)
	if (access(QCOM_IRIS_DEVICE, F_OK) != 0)
		return B_OK;

	BMediaFormats formats;
	if (formats.InitCheck() != B_OK)
		return B_OK;
	const uint32 kCodecs[] = { kCodecH264, kCodecHEVC };
	for (size_t i = 0; i < 2; i++) {
		media_format_description description;
		description.family = B_MISC_FORMAT_FAMILY;
		description.u.misc.file_format = 'ffmp';
		description.u.misc.codec = kCodecs[i];

		media_format format;
		format.type = B_MEDIA_ENCODED_VIDEO;
		format.require_flags = 0;
		format.deny_flags = B_MEDIA_MAUI_UNDEFINED_FLAGS;
		if (formats.MakeFormatFor(&description, 1, &format) != B_OK)
			return B_OK;
		sFormats[i] = format;
	}
	*_count = 2;
	return B_OK;
}


MediaPlugin*
instantiate_plugin()
{
	return new(std::nothrow) QcomIrisPlugin;
}
