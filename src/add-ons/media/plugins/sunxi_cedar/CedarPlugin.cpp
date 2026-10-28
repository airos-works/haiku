/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	H.264, HEVC and VP9 on the Allwinner A733's video engine (Radxa Cubie
	A7S), as a decoder add-on of the Media Kit (CedarDecoder does the work).

	The Media Kit gives a format to one decoder and never tries another when
	that one's Setup() fails, so the formats are only registered where the
	engine is (/dev/misc/sunxi_ve exists), and a stream the engine does not
	decode (fields, more than eight bits, not 4:2:0, larger than 4096; VP9
	profiles 1 and 3 and twelve bits) goes on in software: the decoder then
	hands everything to one of the FFmpeg add-on's decoders. A stream whose parameter sets only come in band is
	found out at its first picture; the chunks read until then are given to
	the FFmpeg decoder again.

	airTime asks for decoder 1, which has no such fallback: it falls back to
	libavcodec itself, and needs to know that it does.

	VP9 comes as FFmpeg's demuxer (or a WebM reader) hands it out: a chunk
	is a frame or a superframe, and gives at most one picture (the last one
	it shows, as libvpx). Profile 0 pictures are eight bit, profile 2 ones
	ten bit (P010). A VP9 stream whose first frame the engine does not take
	(or a sunxi_ve driver from before VP9, which refuses the frames) goes to
	the FFmpeg decoder as the others do.

	Pictures are handed out as
	- 'I420': line_count rows of luma bytes_per_row apart, then a plane of Cb
	  and one of Cr, half as many rows of half the length, or
	- 'NV12': as I420, but with Cb and Cr in pairs in one plane, or
	- 'P010' (ten bit streams): as NV12 with sixteen bit samples, the value
	  in the upper ten bits, or
	- B_YCbCr422 or B_RGB32, for MediaPlayer and the other Media Kit users
	  (ten bit pictures by their eight most significant bits).

	A caller that asks for 'P010', 'I420' or B_RGB32 pictures exactly half as
	wide and high (display.line_width and line_count) gets them so, when the
	width and height are multiples of four: each sample the mean of a 2x2
	block.

	A negative time_to_decode in Decode()'s media_decode_info is, negated,
	the time before which the caller will drop the pictures anyway (it is
	seeking); those are returned with their time but not copied. */


#include <DecoderPlugin.h>
#include <FindDirectory.h>
#include <MediaFormats.h>
#include <Path.h>
#include <image.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "CedarDecoder.h"
#include "h264_parse.h"
#include "hevc_parse.h"


// AV_CODEC_ID_H264, AV_CODEC_ID_HEVC and AV_CODEC_ID_VP9 of the FFmpeg
// add-on's format descriptions
static const uint32 kCodecH264 = 27;
static const uint32 kCodecHEVC = 173;
static const uint32 kCodecVP9 = 167;

static const color_space kColorSpaceNV12 = (color_space)0x4e563132;	// 'NV12'
static const color_space kColorSpaceI420 = (color_space)0x49343230;	// 'I420'
static const color_space kColorSpaceP010 = (color_space)0x50303130;	// 'P010'

static const uint32 kMaxSize = 4096;

// NewDecoder()'s index for a decoder without the software fallback
static const uint kEngineOnly = 1;


class SunxiCedarDecoder;


/*!	The chunks for the FFmpeg decoder: first those this decoder read before
	it gave up, then the stream's. Libmedia's Decoder deletes its provider.
*/
class ChunkReplay : public ChunkProvider {
public:
								ChunkReplay(SunxiCedarDecoder* owner)
									: fOwner(owner) {}

	virtual	status_t			GetNextChunk(const void** chunk, size_t* size,
									media_header* header);

private:
			SunxiCedarDecoder*	fOwner;
};


class SunxiCedarDecoder : public Decoder {
public:
								SunxiCedarDecoder(bool fallback);
	virtual						~SunxiCedarDecoder();

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

			status_t			_NextPicture(CedarDecoder::Picture& picture);
			status_t			_Feed(const void* chunk, size_t size,
									const media_header& header);
			void				_StartAhead();
			status_t			_JoinAhead();
			void				_AheadLoop();
			status_t			_ReadChunk(const void** chunk, size_t* size,
									media_header* header);
			status_t			_PutUnits(const uint8* data, size_t size,
									int64 time);
			void				_SetParameterSets(const uint8* data,
									size_t size);
			void				_AddParameterSet(const uint8* data,
									size_t size);
			bool				_EngineDecodes(BString& reason) const;
			status_t			_StartSoftware(const char* reason);
			uint32				_Stride() const;

			const bool			fFallback;
			CedarDecoder*		fDecoder;
			uint32				fCodec;
			media_format		fInputFormat;		// as Setup() got it
			std::vector<uint8>	fInfo;
			uint32				fNalLengthSize;
				// of the stream's NAL units; 0 when they come with start
				// codes
			std::vector<uint8>	fParameterSets;	// with start codes
			bool				fParameterSetsSent;
			bool				fInputEnded;

			color_space			fOutputSpace;
			bool				fHavePicture;
			CedarDecoder::Picture fPicture;
			uint32				fWidth;
			uint32				fHeight;
			bool				fHalf;
				// half as wide and high, as the caller asked

			// chunks kept until the first picture, for the software
			// decoder, and those it has yet to get
			bool				fRecording;
			std::deque<Chunk>	fRecorded;
			Chunk				fReplaying;

			// The access unit after the picture being handed out, decoded
			// on a thread of its own meanwhile (CEDAR_NO_AHEAD=1: not).
			// The picture is released only once that is done.
			std::thread			fAheadThread;
			std::mutex			fAheadLock;
			std::condition_variable fAheadWake;
			std::condition_variable fAheadDone;
			bool				fAheadQuit;
			bool				fAheadBusy;
			bool				fAheadDrain;
			std::vector<uint8>	fAheadChunk;
				// a copy: the caller's chunk need not outlive the next
				// call (Summit swaps chunk sources between seeks)
			media_header		fAheadHeader;
			status_t			fAheadStatus;
			bool				fReleasePending;
			CedarDecoder::Picture fPendingRelease;

			// the software decoder, when the engine does not do the stream
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


SunxiCedarDecoder::SunxiCedarDecoder(bool fallback)
	:
	fFallback(fallback),
	fDecoder(NULL),
	fCodec(0),
	fNalLengthSize(0),
	fParameterSetsSent(false),
	fInputEnded(false),
	fOutputSpace(kColorSpaceI420),
	fHavePicture(false),
	fWidth(0),
	fHeight(0),
	fHalf(false),
	fRecording(false),
	fAheadQuit(false),
	fAheadBusy(false),
	fAheadDrain(false),
	fAheadStatus(B_OK),
	fReleasePending(false),
	fSoftwareImage(-1),
	fSoftwarePlugin(NULL),
	fSoftware(NULL)
{
}


SunxiCedarDecoder::~SunxiCedarDecoder()
{
	_JoinAhead();
	if (fAheadThread.joinable()) {
		{
			std::lock_guard<std::mutex> lock(fAheadLock);
			fAheadQuit = true;
		}
		fAheadWake.notify_all();
		fAheadThread.join();
	}
	delete fDecoder;
	// the FFmpeg decoder's code goes with its image: it goes first
	delete fSoftware;
	delete fSoftwarePlugin;
	if (fSoftwareImage >= 0)
		unload_add_on(fSoftwareImage);
}


void
SunxiCedarDecoder::GetCodecInfo(media_codec_info* info)
{
	if (fSoftware != NULL) {
		fSoftware->GetCodecInfo(info);
		return;
	}
	memset(info, 0, sizeof(*info));
	strlcpy(info->pretty_name, fCodec == kCodecHEVC
			? "Allwinner Cedar hardware HEVC decoder"
			: fCodec == kCodecVP9 ? "Allwinner Cedar hardware VP9 decoder"
			: "Allwinner Cedar hardware H.264 decoder",
		sizeof(info->pretty_name));
	strlcpy(info->short_name, "sunxi_cedar", sizeof(info->short_name));
}


void
SunxiCedarDecoder::_AddParameterSet(const uint8* data, size_t size)
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
SunxiCedarDecoder::_SetParameterSets(const uint8* data, size_t size)
{
	fParameterSets.clear();
	fNalLengthSize = 0;
	if (data == NULL || size < 4 || fCodec == kCodecVP9)
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
	kind the engine decodes. Without any, nothing can be said yet.
*/
bool
SunxiCedarDecoder::_EngineDecodes(BString& reason) const
{
	size_t position = 0;
	const uint8_t* nal;
	size_t size;
	char error[160];
	bool decodes = true;

	// VP9 says what it is in its frames
	if (fCodec == kCodecVP9)
		return true;

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
					reason = "the engine decodes eight bit 4:2:0 only";
					decodes = false;
				} else if (!sps.frameMbsOnly) {
					reason = "the engine decodes no fields";
					decodes = false;
				} else if (sps.profileIdc == 88
					&& (sps.constraintFlags & 0xc0) == 0) {
					// Extended profile, not kept to Baseline or Main: SP
					// and SI slices or data partitioning may come later,
					// when the software decoder could no longer take over
					reason = "the engine has no SP and SI slices or data "
						"partitioning (Extended profile)";
					decodes = false;
				} else if ((uint32)sps.widthMbs * 16 > kMaxSize
					|| (uint32)sps.heightMapUnits * 16 > kMaxSize) {
					reason = "the engine decodes up to 4096x4096";
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
			if (sps.chromaFormatIdc != 1
				|| (sps.bitDepthLuma != 8 && sps.bitDepthLuma != 10)
				|| sps.bitDepthChroma != sps.bitDepthLuma) {
				reason = "the engine decodes HEVC Main and Main 10 (4:2:0) "
					"only";
				decodes = false;
			} else if ((uint32)sps.width > kMaxSize
				|| (uint32)sps.height > kMaxSize) {
				reason = "the engine decodes up to 4096x4096";
				decodes = false;
			}
		}
	}
	delete state;
	return decodes;
}


/*!	The rest of the stream goes to one of the FFmpeg add-on's decoders. */
status_t
SunxiCedarDecoder::_StartSoftware(const char* reason)
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

	fprintf(stderr, "sunxi_cedar: in software (%s)\n", reason);
	fSoftware->SetChunkProvider(new(std::nothrow) ChunkReplay(this));
	media_format format = fInputFormat;
	status = fSoftware->Setup(&format, fInfo.data(), fInfo.size());
	if (status != B_OK)
		return status;

	// the engine's part is over
	if (fDecoder != NULL && fHavePicture)
		fDecoder->ReleasePicture(fPicture);
	fHavePicture = false;
	delete fDecoder;
	fDecoder = NULL;
	return B_OK;
}


status_t
SunxiCedarDecoder::ReplayChunk(const void** chunk, size_t* size,
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
SunxiCedarDecoder::_ReadChunk(const void** chunk, size_t* size,
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
SunxiCedarDecoder::Setup(media_format* format, const void* info,
	size_t infoSize)
{
	if (format == NULL || format->type != B_MEDIA_ENCODED_VIDEO)
		return B_NOT_SUPPORTED;

	media_format_description description;
	if (BMediaFormats().GetCodeFor(*format, B_MISC_FORMAT_FAMILY,
			&description) != B_OK
		|| description.u.misc.file_format != 'ffmp'
		|| (description.u.misc.codec != kCodecH264
			&& description.u.misc.codec != kCodecHEVC
			&& description.u.misc.codec != kCodecVP9)) {
		return B_NOT_SUPPORTED;
	}

	fCodec = description.u.misc.codec;
	fInputFormat = *format;
	fInfo.assign((const uint8*)info, (const uint8*)info + infoSize);
	_SetParameterSets((const uint8*)info, infoSize);

	const media_video_display_info& display
		= format->u.encoded_video.output.display;
	BString reason;
	if (display.line_width > kMaxSize || display.line_count > kMaxSize)
		reason = "the engine decodes up to 4096x4096";
	else if (!_EngineDecodes(reason)) {
		// (reason says why)
	} else {
		fDecoder = new(std::nothrow) CedarDecoder(fCodec == kCodecHEVC
			? CedarDecoder::HEVC : fCodec == kCodecVP9 ? CedarDecoder::VP9
			: CedarDecoder::H264);
		if (fDecoder == NULL)
			return B_NO_MEMORY;
		status_t status = fDecoder->Open(&reason);
		if (status == B_OK) {
			fRecording = fFallback;
			return B_OK;
		}
		delete fDecoder;
		fDecoder = NULL;
	}

	if (!fFallback) {
		fprintf(stderr, "sunxi_cedar: %s\n", reason.String());
		return B_NOT_SUPPORTED;
	}
	return _StartSoftware(reason.String());
}


uint32
SunxiCedarDecoder::_Stride() const
{
	if (fOutputSpace == B_YCbCr422)
		return (fWidth * 2 + 3) & ~3u;
	if (fOutputSpace == B_RGB32)
		return fWidth * 4;
	if (fOutputSpace == kColorSpaceP010)
		return ((fWidth + 15) & ~15u) * 2;
	return (fWidth + 15) & ~15u;
}


status_t
SunxiCedarDecoder::NegotiateOutputFormat(media_format* format)
{
	if (format == NULL)
		return B_BAD_VALUE;
	if (fSoftware != NULL)
		return fSoftware->NegotiateOutputFormat(format);
	if (fDecoder == NULL)
		return B_NO_INIT;

	// The first picture tells how large they are; the engine may find out
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
	// what the software decoder would need again is not needed any more
	fRecording = false;
	fRecorded.clear();

	// (MediaPlayer leaves the format's type unset and only names the colour
	// space)
	color_space space = format->u.raw_video.display.format;
	bool tenBit = fPicture.bitDepth > 8;
	if (space == kColorSpaceNV12 || space == kColorSpaceI420) {
		// ten bit pictures go out as P010 or packed, eight bit ones not as
		// P010
		if (tenBit)
			return B_MEDIA_BAD_FORMAT;
	} else if (space == kColorSpaceP010) {
		if (!tenBit)
			return B_MEDIA_BAD_FORMAT;
	} else if (space != B_YCbCr422)
		space = B_RGB32;
	fOutputSpace = space;
	// Half as wide and high when asked for exactly that, made in the pass
	// that copies the picture anyway: a 4K film on a 1080p screen needs no
	// more, and the caller then scales a quarter of the samples.
	const media_video_display_info& asked = format->u.raw_video.display;
	fHalf = (space == kColorSpaceP010 || space == kColorSpaceI420
			|| space == B_RGB32)
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
SunxiCedarDecoder::SeekedTo(int64 frame, bigtime_t time)
{
	if (fSoftware != NULL)
		return fSoftware->SeekedTo(frame, time);
	if (fDecoder == NULL)
		return B_NO_INIT;
	_JoinAhead();
	fAheadStatus = B_OK;
	if (fHavePicture) {
		fDecoder->ReleasePicture(fPicture);
		fHavePicture = false;
	}
	fDecoder->Reset();
	fInputEnded = false;
	fParameterSetsSent = false;
	fRecorded.clear();
	return B_OK;
}


/*!	The NAL units of a chunk: each with its length in front, or between
	start codes.
*/
status_t
SunxiCedarDecoder::_PutUnits(const uint8* data, size_t size, int64 time)
{
	if (fCodec == kCodecVP9)
		return fDecoder->PutNal(data, size, time);
	if (fNalLengthSize != 0) {
		size_t offset = 0;
		while (offset + fNalLengthSize <= size) {
			size_t length = 0;
			for (uint32 i = 0; i < fNalLengthSize; i++)
				length = (length << 8) | data[offset++];
			if (length > size - offset)
				length = size - offset;
			status_t status = fDecoder->PutNal(data + offset, length, time);
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
		status_t status = fDecoder->PutNal(data + begin, end - begin, time);
		if (status != B_OK)
			return status;
		position = end;
	}
	return B_OK;
}


status_t
SunxiCedarDecoder::_NextPicture(CedarDecoder::Picture& picture)
{
	while (true) {
		if (fDecoder->NextPicture(picture))
			return B_OK;
		if (fInputEnded)
			return B_LAST_BUFFER_ERROR;

		const void* chunk;
		size_t size;
		media_header header;
		status_t status = _ReadChunk(&chunk, &size, &header);
		if (status == B_LAST_BUFFER_ERROR) {
			fInputEnded = true;
			fDecoder->Drain();
			continue;
		}
		if (status != B_OK)
			return status;

		status = _Feed(chunk, size, header);
		if (status != B_OK)
			return status;
	}
}


/*!	A chunk (an access unit) to the engine. */
status_t
SunxiCedarDecoder::_Feed(const void* chunk, size_t size,
	const media_header& header)
{
	// after the start and a seek, the parameter sets again
	status_t status = B_OK;
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
		status = fDecoder->EndAccessUnit();
	if (status != B_OK) {
		fprintf(stderr, "sunxi_cedar: %s\n", fDecoder->Error());
		return status == B_NOT_SUPPORTED ? status : B_ERROR;
	}
	return B_OK;
}


/*!	Reads the next chunk here (the caller's chunk provider stays on the
	caller's thread) and has the engine decode it on the ahead thread while
	the caller copies out the picture before it: the engine's time and the
	copying's overlap rather than add up. */
void
SunxiCedarDecoder::_StartAhead()
{
	static const bool sNoAhead = getenv("CEDAR_NO_AHEAD") != NULL;
	if (sNoAhead || fInputEnded || fAheadStatus != B_OK)
		return;
	const void* chunk;
	size_t size;
	media_header header;
	status_t status = _ReadChunk(&chunk, &size, &header);
	bool drain = false;
	if (status == B_LAST_BUFFER_ERROR) {
		fInputEnded = true;
		drain = true;
	} else if (status != B_OK) {
		// a seek or the end of the caller's input: Decode() says so next
		fAheadStatus = status;
		return;
	}
	if (!fAheadThread.joinable())
		fAheadThread = std::thread(&SunxiCedarDecoder::_AheadLoop, this);
	std::lock_guard<std::mutex> lock(fAheadLock);
	if (!drain)
		fAheadChunk.assign((const uint8*)chunk, (const uint8*)chunk + size);
	fAheadHeader = header;
	fAheadDrain = drain;
	fAheadBusy = true;
	fAheadWake.notify_all();
}


/*!	Waits for the access unit given to the ahead thread, then releases the
	picture copied meanwhile. */
status_t
SunxiCedarDecoder::_JoinAhead()
{
	{
		std::unique_lock<std::mutex> lock(fAheadLock);
		fAheadDone.wait(lock, [this] { return !fAheadBusy; });
	}
	if (fReleasePending && fDecoder != NULL) {
		fDecoder->ReleasePicture(fPendingRelease);
		fReleasePending = false;
	}
	return fAheadStatus;
}


void
SunxiCedarDecoder::_AheadLoop()
{
	rename_thread(find_thread(NULL), "sunxi_cedar ahead");
	set_thread_priority(find_thread(NULL), B_DISPLAY_PRIORITY);
	std::unique_lock<std::mutex> lock(fAheadLock);
	while (true) {
		fAheadWake.wait(lock, [this] { return fAheadQuit || fAheadBusy; });
		if (fAheadQuit)
			return;
		lock.unlock();
		status_t status = B_OK;
		if (fAheadDrain)
			fDecoder->Drain();
		else
			status = _Feed(fAheadChunk.data(), fAheadChunk.size(), fAheadHeader);
		lock.lock();
		if (status != B_OK)
			fAheadStatus = status;
		fAheadBusy = false;
		fAheadDone.notify_all();
	}
}


status_t
SunxiCedarDecoder::Decode(void* buffer, int64* frameCount,
	media_header* header, media_decode_info* info)
{
	if (fSoftware != NULL)
		return fSoftware->Decode(buffer, frameCount, header, info);
	if (fDecoder == NULL)
		return B_NO_INIT;
	if (buffer == NULL || frameCount == NULL || header == NULL)
		return B_BAD_VALUE;

	status_t aheadStatus = _JoinAhead();
	if (aheadStatus != B_OK) {
		fAheadStatus = B_OK;
		return aheadStatus;
	}

	fDecoder->SetSkipBefore(info != NULL && info->time_to_decode < 0
		? -info->time_to_decode : INT64_MIN);

	if (!fHavePicture) {
		status_t status = _NextPicture(fPicture);
		if (status != B_OK)
			return status;
	}
	fHavePicture = false;

	if (fPicture.width != (fHalf ? fWidth * 2 : fWidth)
		|| fPicture.height != (fHalf ? fHeight * 2 : fHeight)) {
		// the stream changed its pictures; nothing here renegotiates
		fDecoder->ReleasePicture(fPicture);
		fprintf(stderr, "sunxi_cedar: the pictures changed to %" B_PRIu32
			"x%" B_PRIu32 "\n", fPicture.width, fPicture.height);
		return B_MEDIA_BAD_FORMAT;
	}

	uint32 stride = _Stride();
	bool skip = info != NULL && info->time_to_decode < 0
		&& fPicture.pts < -info->time_to_decode;
	_StartAhead();
	if (!skip) {
		if (fHalf && fOutputSpace == B_RGB32)
			fDecoder->CopyHalfPacked(fPicture, (uint8*)buffer, stride);
		else if (fHalf)
			fDecoder->CopyHalfPlanes(fPicture, (uint8*)buffer, stride);
		else if (fOutputSpace == kColorSpaceNV12
			|| fOutputSpace == kColorSpaceI420
			|| fOutputSpace == kColorSpaceP010) {
			fDecoder->CopyPlanes(fPicture, (uint8*)buffer, stride,
				fOutputSpace != kColorSpaceI420);
		} else
			fDecoder->CopyPacked(fPicture, (uint8*)buffer, stride, fOutputSpace);
	}

	memset(header, 0, sizeof(*header));
	header->type = B_MEDIA_RAW_VIDEO;
	header->start_time = fPicture.pts;
	header->size_used = fOutputSpace == kColorSpaceNV12
			|| fOutputSpace == kColorSpaceI420
			|| fOutputSpace == kColorSpaceP010
		? (size_t)stride * (fHeight + (fHeight + 1) / 2)
		: (size_t)stride * fHeight;
	header->u.raw_video.display_line_width = fWidth;
	header->u.raw_video.display_line_count = fHeight;
	header->u.raw_video.bytes_per_row = stride;
	header->u.raw_video.line_count = fHeight;
	*frameCount = 1;

	// once the access unit decoded meanwhile is done (_JoinAhead())
	fPendingRelease = fPicture;
	fReleasePending = true;
	return B_OK;
}


//	#pragma mark -


class SunxiCedarPlugin : public DecoderPlugin {
public:
	virtual	Decoder*			NewDecoder(uint index);
	virtual	status_t			GetSupportedFormats(media_format** formats,
									size_t* count);
};


Decoder*
SunxiCedarPlugin::NewDecoder(uint index)
{
	return new(std::nothrow) SunxiCedarDecoder(index != kEngineOnly);
}


status_t
SunxiCedarPlugin::GetSupportedFormats(media_format** _formats, size_t* _count)
{
	static media_format sFormats[3];
	*_formats = sFormats;
	*_count = 0;

	// none where the engine is not (the arm64 packages are the same for
	// every board)
	if (access(SUNXI_VE_DEVICE_PATH, F_OK) != 0)
		return B_OK;

	BMediaFormats formats;
	if (formats.InitCheck() != B_OK)
		return B_OK;
	const uint32 kCodecs[] = { kCodecH264, kCodecHEVC, kCodecVP9 };
	for (size_t i = 0; i < 3; i++) {
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
	*_count = 3;
	return B_OK;
}


MediaPlugin*
instantiate_plugin()
{
	return new(std::nothrow) SunxiCedarPlugin;
}
