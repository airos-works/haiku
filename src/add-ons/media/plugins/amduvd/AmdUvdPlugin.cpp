/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include <DecoderPlugin.h>
#include <MediaFormats.h>
#include <amdgpu_haiku.h>
#include <errno.h>
#include <fcntl.h>
#include <new>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <utility>

#include "H264Output.h"
#include "H264Packet.h"
#include "H264Stream.h"

Decoder* NewAmdHevcDecoder();

static const color_space kNV12 = (color_space)0x4e563132;
static const color_space kI420 = (color_space)0x49343230;

class AmdUvdDecoder : public Decoder {
public:
	AmdUvdDecoder() : fFd(-1), fSession(), fStatus(B_NO_INIT), fInput(),
		fWidth(0), fHeight(0), fFormat(H264Output::RGB32), fSpace(B_RGB32),
		fRowBytes(0), fSendSets(true), fEnded(false) {}
	virtual ~AmdUvdDecoder() { Close(); }
	virtual void GetCodecInfo(media_codec_info* info);
	virtual status_t Setup(media_format* input, const void* info, size_t size);
	virtual status_t NegotiateOutputFormat(media_format* output);
	virtual status_t SeekedTo(int64 frame, bigtime_t time);
	virtual status_t Decode(void* buffer, int64* count, media_header* header,
		media_decode_info* info);
private:
	status_t Feed(const uint8* data, size_t size, bigtime_t time);
	status_t Fail(status_t status, const char* reason);
	status_t DestroySession();
	void Close();
	int fFd;
	amdgpu_video_create fSession;
	status_t fStatus;
	media_format fInput;
	int fWidth, fHeight;
	H264Output::Format fFormat;
	color_space fSpace;
	size_t fRowBytes;
	bool fSendSets, fEnded;
	H264Stream fStream;
	H264Packet fPacket;
	H264Output fOutput;
	std::vector<uint8> fUnit;
};

status_t AmdUvdDecoder::DestroySession()
{
	if (fSession.handle == 0) return B_OK;
	amdgpu_video_destroy d = {AMDGPU_HAIKU_ABI_VERSION, sizeof(d), fSession.handle};
	status_t status = ioctl(fFd, AMDGPU_VIDEO_DESTROY, &d, sizeof(d)) == 0 ? B_OK : errno;
	if (status == B_OK) fSession = {};
	return status;
}

void AmdUvdDecoder::Close()
{
	// The driver's per-file cleanup also retires sessions after an error.
	if (fFd >= 0) { close(fFd); fFd = -1; }
	fSession = {};
	fOutput.Reset(); fStream.Reset(); fUnit.clear();
}

status_t AmdUvdDecoder::Fail(status_t status, const char* reason)
{
	fprintf(stderr, "amduvd: %s (%s)\n", reason, strerror(status));
	Close(); fStatus = status;
	return status;
}

void AmdUvdDecoder::GetCodecInfo(media_codec_info* info)
{
	memset(info, 0, sizeof(*info));
	strlcpy(info->short_name, "amduvd h264", sizeof(info->short_name));
	strlcpy(info->pretty_name, "AMD UVD H.264 hardware decoder", sizeof(info->pretty_name));
}

status_t AmdUvdDecoder::Setup(media_format* input, const void* info, size_t size)
{
	Close(); fStatus = B_NO_INIT;
	if (input == NULL || input->type != B_MEDIA_ENCODED_VIDEO) return B_NOT_SUPPORTED;
	media_format_description description;
	if (BMediaFormats().GetCodeFor(*input, B_MISC_FORMAT_FAMILY, &description) != B_OK
		|| description.u.misc.file_format != 'ffmp' || description.u.misc.codec != 27)
		return B_NOT_SUPPORTED;
	const media_video_display_info& display = input->u.encoded_video.output.display;
	if (H264Output::Bytes(display.line_width, display.line_count, H264Output::NV12) == 0)
		return B_NOT_SUPPORTED;
	try {
		fStream = H264Stream(); // Setup starts a new stream, including parameter sets.
		if (!fPacket.Configure((const uint8*)info, size)) return B_BAD_DATA;
	} catch (const std::bad_alloc&) { return B_NO_MEMORY; }
	fFd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
	if (fFd < 0) return errno;
	fInput = *input; fWidth = display.line_width; fHeight = display.line_count;
	fRowBytes = 0; fSendSets = true; fEnded = false;
	fStatus = B_OK;
	return B_OK;
}

status_t AmdUvdDecoder::NegotiateOutputFormat(media_format* output)
{
	if (fStatus != B_OK) return fStatus;
	if (output == NULL) return B_BAD_VALUE;
	color_space space = output->u.raw_video.display.format;
	H264Output::Format format;
	if (space == kNV12) format = H264Output::NV12;
	else if (space == kI420) format = H264Output::I420;
	else if (space == B_YCbCr422) format = H264Output::YCbCr422;
	else if (space == B_RGB32 || space == B_NO_COLOR_SPACE) {
		space = B_RGB32; format = H264Output::RGB32;
	} else return B_NOT_SUPPORTED;
	fSpace = space; fFormat = format;
	fRowBytes = fWidth * (format == H264Output::RGB32 ? 4 : format == H264Output::YCbCr422 ? 2 : 1);
	output->Clear(); output->type = B_MEDIA_RAW_VIDEO;
	output->u.raw_video = fInput.u.encoded_video.output;
	media_raw_video_format& raw = output->u.raw_video;
	raw.interlace = 1; raw.first_active = 0; raw.last_active = fHeight - 1;
	raw.orientation = B_VIDEO_TOP_LEFT_RIGHT;
	raw.display.format = space; raw.display.line_width = fWidth; raw.display.line_count = fHeight;
	raw.display.bytes_per_row = fRowBytes; raw.display.pixel_offset = 0;
	raw.display.line_offset = 0; raw.display.flags = 0;
	return B_OK;
}

status_t AmdUvdDecoder::SeekedTo(int64, bigtime_t)
{
	if (fStatus != B_OK) return fStatus;
	status_t status = DestroySession();
	if (status != B_OK) return Fail(status, "destroy session for seek");
	fStream.Reset(); fOutput.Reset(); fUnit.clear();
	fSendSets = true; fEnded = false;
	return B_OK;
}

status_t AmdUvdDecoder::Feed(const uint8* data, size_t size, bigtime_t time)
{
	if (!fPacket.Convert(data, size, fSendSets, fUnit)) return Fail(B_BAD_DATA, "invalid packet extent");
	if (!fStream.Prepare(fUnit.data(), fUnit.size())) return Fail(B_BAD_DATA, fStream.Error());
	fSendSets = false;
	// Media Kit supplied the destination's extent at negotiation. Refuse a
	// display-size change so the caller can renegotiate or restart in software.
	if (fStream.width != fWidth || fStream.height != fHeight)
		return Fail(B_MEDIA_BAD_FORMAT, "display size changed");
	if (fSession.handle == 0 || memcmp(&fSession.config, &fStream.config, sizeof(fStream.config)) != 0) {
		if ((fStream.picture.flags & AMDGPU_H264_IDR) == 0)
			return Fail(B_BAD_DATA, "format change requires IDR");
		status_t status = DestroySession();
		if (status != B_OK) return Fail(status, "destroy old format");
		fSession = {}; fSession.version = AMDGPU_HAIKU_ABI_VERSION; fSession.size = sizeof(fSession);
		fSession.config = fStream.config;
		if (ioctl(fFd, AMDGPU_VIDEO_CREATE, &fSession, sizeof(fSession)) != 0)
			return Fail(errno, "create hardware session");
	}
	H264Frame frame = {};
	frame.pixels.resize(fSession.output_bytes);
	amdgpu_video_decode d = {};
	d.version = AMDGPU_HAIKU_ABI_VERSION; d.size = sizeof(d); d.handle = fSession.handle;
	d.bitstream = (addr_t)fStream.bitstream.data(); d.bitstream_bytes = fStream.bitstream.size();
	d.output = (addr_t)frame.pixels.data(); d.output_capacity = frame.pixels.size(); d.picture = fStream.picture;
	if (ioctl(fFd, AMDGPU_VIDEO_DECODE, &d, sizeof(d)) != 0) return Fail(errno, "hardware decode");
	if (d.sequence != d.fence || d.rptr != d.wptr || d.guard_mismatches || (d.vm_fault_status & 0xff))
		return Fail(B_BAD_DATA, "hardware completion");
	if (!fStream.Commit()) return Fail(B_BAD_DATA, fStream.Error());
	frame.sequence = fStream.sequence; frame.poc = fStream.poc; frame.time = time;
	frame.width = fWidth; frame.height = fHeight;
	frame.cropLeft = fStream.cropLeft; frame.cropTop = fStream.cropTop;
	frame.pitch = fSession.pitch; frame.codedHeight = fSession.config.height;
	frame.fullRange = fStream.fullRange; frame.matrix = fStream.matrixCoefficients;
	if (!fOutput.Push(std::move(frame), fStream.reorderLimit, fStream.discardPrior))
		return Fail(B_BAD_DATA, "invalid picture output order");
	return B_OK;
}

status_t AmdUvdDecoder::Decode(void* buffer, int64* count, media_header* header,
	media_decode_info* info)
{
	if (count == NULL || header == NULL || buffer == NULL) return B_BAD_VALUE;
	*count = 0;
	if (fStatus != B_OK) return fStatus;
	if (fRowBytes == 0) return B_NO_INIT;
	try {
		for (;;) {
			if (fOutput.Ready(fEnded)) {
				const H264Frame& frame = fOutput.Front();
				bool skip = info != NULL && info->time_to_decode < 0
					&& info->time_to_decode != INT64_MIN && frame.time < -info->time_to_decode;
				if (!skip && !H264Output::Copy(frame, fFormat, (uint8*)buffer))
					return Fail(B_NOT_SUPPORTED, "output colour conversion");
				memset(header, 0, sizeof(*header));
				header->type = B_MEDIA_RAW_VIDEO; header->start_time = frame.time;
				header->size_used = H264Output::Bytes(fWidth, fHeight, fFormat);
				header->u.raw_video.display_line_width = fWidth;
				header->u.raw_video.display_line_count = fHeight;
				header->u.raw_video.bytes_per_row = fRowBytes;
				header->u.raw_video.line_count = fHeight;
				fOutput.Pop(); *count = 1; return B_OK;
			}
			if (fEnded) return B_LAST_BUFFER_ERROR;
			const void* chunk = NULL; size_t size = 0; media_header input = {};
			status_t status = GetNextChunk(&chunk, &size, &input);
			if (status == B_LAST_BUFFER_ERROR) { fEnded = true; continue; }
			// Interrupt/cancel is not EOF. Preserve the queue until SeekedTo
			// discards it; never emit stale pictures while a seek is pending.
			if (status != B_OK) return status;
			status = Feed((const uint8*)chunk, size, input.start_time);
			if (status != B_OK) return status;
		}
	} catch (const std::bad_alloc&) { return Fail(B_NO_MEMORY, "output allocation"); }
}

class AmdUvdPlugin : public DecoderPlugin {
public:
	virtual Decoder* NewDecoder(uint index) {
		if (index == 0) return new(std::nothrow) AmdUvdDecoder();
		if (index == 1) return NewAmdHevcDecoder();
		return NULL;
	}
	virtual status_t GetSupportedFormats(media_format** formats, size_t* count) {
		// Media Kit has no decoder fallback. Applications select this add-on
		// explicitly and keep their software decoder for unsupported streams.
		if (formats != NULL) *formats = NULL;
		if (count != NULL) *count = 0;
		return B_NOT_SUPPORTED;
	}
};

MediaPlugin* instantiate_plugin() { return new(std::nothrow) AmdUvdPlugin(); }
