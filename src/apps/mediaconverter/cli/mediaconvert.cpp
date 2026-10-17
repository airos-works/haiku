/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	MediaConverter's engine from a shell: for tests, and for scripts. */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Formats.h"
#include "HardwareCaps.h"
#include "MediaProbe.h"
#include "OutputSettings.h"
#include "Presets.h"
#include "SoftwareEncoders.h"
#include "Transcoder.h"


static const struct {
	const char*		name;
	video_codec		codec;
} kVideoNames[] = {
	{ "h264", kVideoH264 }, { "h264-10", kVideoH264_10 },
	{ "h265", kVideoH265 }, { "hevc", kVideoH265 },
	{ "h265-10", kVideoH265_10 }, { "h265-12", kVideoH265_12 },
	{ "av1", kVideoAV1 }, { "av1-10", kVideoAV1_10 },
	{ "vp9", kVideoVP9 }, { "vp9-10", kVideoVP9_10 }, { "vp8", kVideoVP8 },
	{ "mpeg4", kVideoMPEG4 }, { "mpeg2", kVideoMPEG2 },
	{ "theora", kVideoTheora }, { "ffv1", kVideoFFV1 },
	{ "copy", kVideoCopy }, { "none", kVideoNone },
};


static const struct {
	const char*		name;
	audio_codec		codec;
} kAudioNames[] = {
	{ "aac", kAudioAAC }, { "he-aac", kAudioHEAAC }, { "mp3", kAudioMP3 },
	{ "mp2", kAudioMP2 }, { "ac3", kAudioAC3 }, { "eac3", kAudioEAC3 },
	{ "truehd", kAudioTrueHD }, { "flac", kAudioFLAC16 },
	{ "flac24", kAudioFLAC24 }, { "alac", kAudioALAC16 },
	{ "alac24", kAudioALAC24 }, { "opus", kAudioOpus },
	{ "vorbis", kAudioVorbis }, { "pcm", kAudioPCM16 },
	{ "pcm24", kAudioPCM24 }, { "copy", kAudioCopy }, { "none", kAudioNone },
};


static const struct {
	const char*		name;
	container_kind	kind;
} kContainerNames[] = {
	{ "mp4", kContainerMP4 }, { "mkv", kContainerMKV },
	{ "webm", kContainerWebM }, { "mov", kContainerMOV },
	{ "m4a", kContainerM4A }, { "mp3", kContainerMP3 },
	{ "flac", kContainerFLAC }, { "ogg", kContainerOgg },
	{ "opus", kContainerOpus }, { "wav", kContainerWAV },
};


class Printer : public TranscodeListener {
public:
	Printer(bool quiet) : fQuiet(quiet) {}

	virtual void TranscodeStarted(const TranscodeReport& report)
	{
		printf("video: %s %s %dx%d%s\n", report.videoEncoder.String(),
			report.videoSettings.String(), report.width, report.height,
			report.cpuFallback ? " (CPU fallback)" : "");
		if (report.cpuFallback)
			printf("  why not a GPU: %s\n", report.fallbackReason.String());
		for (size_t i = 0; i < report.audioTracks.size(); i++)
			printf("audio %d: %s\n", (int)i + 1, report.audioTracks[i].String());
		for (size_t i = 0; i < report.subtitleTracks.size(); i++) {
			printf("subtitles %d: %s\n", (int)i + 1,
				report.subtitleTracks[i].String());
		}
		if (!report.notes.IsEmpty())
			printf("notes: %s\n", report.notes.String());
		fflush(stdout);
	}

	virtual void TranscodeProgressed(const TranscodeProgress& progress)
	{
		if (fQuiet)
			return;
		printf("progress %5.1f%% %6.1f fps %5.2fx %" B_PRId64 " frames %"
			B_PRIdOFF " bytes\n", progress.fraction * 100,
			progress.framesPerSecond, progress.speed, progress.frames,
			progress.bytesWritten);
		fflush(stdout);
	}

private:
	bool	fQuiet;
};


static void
print_hardware()
{
	HardwareInventory& inventory = HardwareInventory::Default();
	inventory.Probe();
	printf("GPUs: %" B_PRId32 "\n", inventory.CountDevices());
	for (int32 i = 0; i < inventory.CountDevices(); i++) {
		const GpuDevice& device = inventory.DeviceAt(i);
		printf("- %s [%s] %04x:%04x %s %s: %s\n", device.name.String(),
			device.id.String(), device.vendorID, device.deviceID,
			device.generation.String(), device.engine.String(),
			device.status.String());
		for (size_t f = 0; f < device.encode.size(); f++) {
			const GpuEncodeFormat& format = device.encode[f];
			printf("    encode %-14s up to %dx%d%s%s\n",
				video_codec_info(format.codec).name, format.maxWidth,
				format.maxHeight, format.bFrames ? ", B pictures" : "",
				device.CanEncode(format.codec, 16, 16) ? "" : " (not used)");
		}
		BString decode;
		for (size_t d = 0; d < device.decode.size(); d++) {
			if (d > 0)
				decode << ", ";
			decode << device.decode[d];
		}
		if (!decode.IsEmpty())
			printf("    decode %s\n", decode.String());
	}
	printf("CPU encoders: %s; %s; %s\n",
		SoftwareEncoders::Version(kSoftwareX264).String(),
		SoftwareEncoders::Version(kSoftwareX265).String(),
		SoftwareEncoders::Version(kSoftwareSvtAv1).String());
	for (int c = 0; c < kVideoCopy; c++) {
		video_codec codec = (video_codec)c;
		const GpuDevice* device = inventory.EncoderFor(codec, 1920, 1080);
		printf("  %-16s -> %s\n", video_codec_info(codec).name,
			device != NULL ? device->name.String() : "CPU");
	}
}


static void
print_info(const char* path)
{
	SourceInfo info;
	BString error;
	if (MediaProbe::Probe(path, info, &error) != B_OK) {
		fprintf(stderr, "%s\n", error.String());
		return;
	}
	printf("%s: %s, %s, %s, %s\n", info.name.String(),
		info.containerName.String(),
		MediaProbe::FormatDuration(info.duration).String(),
		MediaProbe::FormatSize(info.fileSize).String(),
		MediaProbe::FormatBitRate(info.bitRate).String());
	for (size_t i = 0; i < info.video.size(); i++) {
		const VideoTrackInfo& v = info.video[i];
		printf("video #%d: %s %s level %d, %dx%d, %.3f fps%s, %s, %d-bit %s, "
			"%s, %s/%s/%s%s%s, rotation %d\n", v.index, v.codecName.String(),
			v.profile.String(), v.level, v.width, v.height,
			av_q2d(v.frameRate), v.variableFrameRate ? " (variable)" : "",
			v.pixelFormat.String(), v.bitDepth, v.chroma.String(),
			v.dynamicRange.String(), v.colorPrimaries.String(),
			v.transfer.String(), v.matrix.String(),
			v.interlaced ? ", interlaced" : "",
			v.fullRange ? ", full range" : "", v.rotation);
	}
	for (size_t i = 0; i < info.audio.size(); i++) {
		const AudioTrackInfo& a = info.audio[i];
		printf("audio #%d: %s %s, %s, %d Hz, %s, %s%s\n", a.index,
			a.codecName.String(), a.profile.String(), a.layout.String(),
			a.sampleRate, MediaProbe::FormatBitRate(a.bitRate).String(),
			MediaProbe::LanguageName(a.language).String(),
			a.isDefault ? ", default" : "");
	}
	for (size_t i = 0; i < info.subtitles.size(); i++) {
		const SubtitleTrackInfo& s = info.subtitles[i];
		printf("subtitles #%d: %s, %s%s%s\n", s.index, s.codecName.String(),
			MediaProbe::LanguageName(s.language).String(),
			s.forced ? ", forced" : "", s.isDefault ? ", default" : "");
	}
	printf("chapters: %d\n", (int)info.chapters.size());
}




extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

#include <math.h>
#include <vector>


/*!	Decodes the main video stream of a file, picture by picture, as 8-bit
	luma at a given size. */
class LumaReader {
public:
	LumaReader()
		: fContext(NULL), fDecoder(NULL), fScaler(NULL), fIndex(-1),
		  fPacket(av_packet_alloc()), fFrame(av_frame_alloc()), fEnded(false)
	{
	}

	~LumaReader()
	{
		sws_freeContext(fScaler);
		av_frame_free(&fFrame);
		av_packet_free(&fPacket);
		avcodec_free_context(&fDecoder);
		avformat_close_input(&fContext);
	}

	bool Open(const char* path)
	{
		if (avformat_open_input(&fContext, path, NULL, NULL) < 0
			|| avformat_find_stream_info(fContext, NULL) < 0)
			return false;
		const AVCodec* codec = NULL;
		fIndex = av_find_best_stream(fContext, AVMEDIA_TYPE_VIDEO, -1, -1,
			&codec, 0);
		if (fIndex < 0)
			return false;
		fDecoder = avcodec_alloc_context3(codec);
		avcodec_parameters_to_context(fDecoder,
			fContext->streams[fIndex]->codecpar);
		fDecoder->thread_count = 0;
		return avcodec_open2(fDecoder, codec, NULL) >= 0;
	}

	int Width() const { return fDecoder->width; }
	int Height() const { return fDecoder->height; }

	// false at the end
	bool Next(int width, int height, std::vector<uint8>& luma)
	{
		while (true) {
			int result = avcodec_receive_frame(fDecoder, fFrame);
			if (result == 0) {
				// a few lines or columns more (coding alignment that the
				// render size takes away again) are cut off, not scaled
				int sourceWidth = fFrame->width;
				int sourceHeight = fFrame->height;
				if (sourceWidth >= width && sourceWidth - width <= 16
					&& sourceHeight >= height && sourceHeight - height <= 16) {
					sourceWidth = width;
					sourceHeight = height;
				}
				fScaler = sws_getCachedContext(fScaler, sourceWidth,
					sourceHeight, (AVPixelFormat)fFrame->format, width,
					height, AV_PIX_FMT_GRAY8, SWS_BICUBIC, NULL, NULL, NULL);
				luma.resize((size_t)width * height);
				uint8* planes[4] = { luma.data() };
				int strides[4] = { width };
				sws_scale(fScaler, fFrame->data, fFrame->linesize, 0,
					sourceHeight, planes, strides);
				av_frame_unref(fFrame);
				return true;
			}
			if (result == AVERROR_EOF)
				return false;
			if (fEnded)
				return false;
			if (av_read_frame(fContext, fPacket) < 0) {
				avcodec_send_packet(fDecoder, NULL);
				fEnded = true;
				continue;
			}
			if (fPacket->stream_index == fIndex)
				avcodec_send_packet(fDecoder, fPacket);
			av_packet_unref(fPacket);
		}
	}

private:
	AVFormatContext*	fContext;
	AVCodecContext*		fDecoder;
	SwsContext*			fScaler;
	int					fIndex;
	AVPacket*			fPacket;
	AVFrame*			fFrame;
	bool				fEnded;
};


static int
compare(const char* source, const char* output)
{
	LumaReader a, b;
	if (!a.Open(source) || !b.Open(output)) {
		fprintf(stderr, "cannot open the files\n");
		return 1;
	}
	int width = b.Width();
	int height = b.Height();
	if (getenv("COMPARE_AT_SOURCE") != NULL) {
		width = a.Width();
		height = a.Height();
	}
	std::vector<uint8> la, lb;
	int frames = 0;
	double sum = 0, minimum = 1000;
	int minimumAt = -1;
	while (true) {
		bool gotA = a.Next(width, height, la);
		bool gotB = b.Next(width, height, lb);
		if (!gotA || !gotB) {
			if (gotA != gotB) {
				printf("frame counts differ: %s ends first after %d\n",
					gotA ? "output" : "source", frames);
			}
			break;
		}
		if (frames == 0 && getenv("COMPARE_SHIFT") != NULL) {
			// which offset of the output matches the source best
			double best = 1e30;
			int bestX = 0, bestY = 0;
			for (int dy = -24; dy <= 24; dy++) {
				for (int dx = -24; dx <= 24; dx += 2) {
					double e = 0;
					int n = 0;
					for (int y = 32; y < height - 32; y += 2) {
						for (int x = 32; x < width - 32; x += 2) {
							int d = (int)la[(size_t)y * width + x]
								- (int)lb[(size_t)(y + dy) * width + x + dx];
							e += d * d;
							n++;
						}
					}
					e /= n;
					if (e < best) {
						best = e;
						bestX = dx;
						bestY = dy;
					}
				}
			}
			double meanA = 0, meanB = 0;
			for (size_t i = 0; i < la.size(); i++) {
				meanA += la[i];
				meanB += lb[i];
			}
			printf("best offset %d,%d: %.2f dB; means %.1f %.1f\n", bestX,
				bestY, 10 * log10(255.0 * 255.0 / max_c(best, 1e-9)),
				meanA / la.size(), meanB / lb.size());
		}
		double error = 0;
		for (size_t i = 0; i < la.size(); i++) {
			int d = (int)la[i] - (int)lb[i];
			error += d * d;
		}
		error /= la.size();
		double psnr = error > 0 ? 10 * log10(255.0 * 255.0 / error) : 99;
		sum += psnr;
		if (getenv("COMPARE_VERBOSE") != NULL)
			printf("%d %.2f\n", frames, psnr);
		if (psnr < minimum) {
			minimum = psnr;
			minimumAt = frames;
		}
		frames++;
	}
	printf("frames %d, luma PSNR average %.2f dB, lowest %.2f dB at %d\n",
		frames, frames > 0 ? sum / frames : 0, minimum, minimumAt);
	return frames > 0 && sum / frames > 30 ? 0 : 2;
}


static int
dump_packets(const char* path, int count, const char* outPath)
{
	AVFormatContext* context = NULL;
	if (avformat_open_input(&context, path, NULL, NULL) < 0
		|| avformat_find_stream_info(context, NULL) < 0)
		return 1;
	int index = av_find_best_stream(context, AVMEDIA_TYPE_VIDEO, -1, -1, NULL,
		0);
	FILE* out = fopen(outPath, "wb");
	AVCodecParameters* parameters = context->streams[index]->codecpar;
	printf("extradata %d bytes\n", parameters->extradata_size);
	fwrite(parameters->extradata, 1, parameters->extradata_size, out);
	AVPacket* packet = av_packet_alloc();
	int written = 0;
	while (written < count && av_read_frame(context, packet) >= 0) {
		if (packet->stream_index == index) {
			printf("packet %d: %d bytes pts %" B_PRId64 " dts %" B_PRId64
				"%s\n", written, packet->size, packet->pts, packet->dts,
				(packet->flags & AV_PKT_FLAG_KEY) ? " key" : "");
			fwrite(packet->data, 1, packet->size, out);
			written++;
		}
		av_packet_unref(packet);
	}
	fclose(out);
	av_packet_free(&packet);
	avformat_close_input(&context);
	return 0;
}


static void
usage()
{
	fprintf(stderr, "usage: mediaconvert --hardware\n"
		"       mediaconvert --info <file>\n"
		"       mediaconvert --compare <source> <output>\n"
		"       mediaconvert [options] <source> <destination>\n"
		"  --preset <name>  --container mp4|mkv|webm|mov|m4a|mp3|flac|ogg|"
		"opus|wav\n"
		"  --video <codec>  --cpu  --gpu <id>  --quality 0-100  --bitrate "
		"<kbit/s>\n"
		"  --speed 0-4  --height <lines>  --fps <fps*1000>  --cfr\n"
		"  --audio <codec>  --abitrate <kbit/s>  --mixdown source|mono|stereo|"
		"5.1|7.1\n"
		"  --tracks first|all|default  --passthrough\n"
		"  --subs none|copy|burn|burn-forced  --crop  --deinterlace "
		"off|auto|always\n"
		"  --denoise 0-3  --sharpen 0-3  --gray  --rotate <degrees>  --flip\n"
		"  --start <seconds>  --end <seconds>  --quiet\n");
	exit(1);
}


int
main(int argc, char** argv)
{
	av_log_set_level(AV_LOG_ERROR);
	OutputSettings settings;
	const char* source = NULL;
	const char* destination = NULL;
	bool quiet = false;

	for (int i = 1; i < argc; i++) {
		const char* arg = argv[i];
		const char* next = i + 1 < argc ? argv[i + 1] : NULL;
		bool consumed = true;
		if (strcmp(arg, "--dump") == 0 && i + 3 < argc)
			return dump_packets(argv[i + 1], atoi(argv[i + 2]), argv[i + 3]);
		if (strcmp(arg, "--compare") == 0 && i + 2 < argc)
			return compare(argv[i + 1], argv[i + 2]);
		if (strcmp(arg, "--hardware") == 0) {
			print_hardware();
			return 0;
		} else if (strcmp(arg, "--info") == 0 && next != NULL) {
			print_info(next);
			return 0;
		} else if (strcmp(arg, "--preset") == 0 && next != NULL) {
			int32 index = Presets::IndexOf(next);
			if (index < 0) {
				fprintf(stderr, "no preset \"%s\"; there are:\n", next);
				for (int32 p = 0; p < Presets::Count(); p++)
					fprintf(stderr, "  %s\n", Presets::At(p).name);
				return 1;
			}
			settings = Presets::At(index).settings;
		} else if (strcmp(arg, "--container") == 0 && next != NULL) {
			bool found = false;
			for (size_t c = 0; c < sizeof(kContainerNames)
					/ sizeof(kContainerNames[0]); c++) {
				if (strcmp(next, kContainerNames[c].name) == 0) {
					settings.container = kContainerNames[c].kind;
					found = true;
				}
			}
			if (!found)
				usage();
		} else if (strcmp(arg, "--video") == 0 && next != NULL) {
			bool found = false;
			for (size_t c = 0; c < sizeof(kVideoNames) / sizeof(kVideoNames[0]);
					c++) {
				if (strcmp(next, kVideoNames[c].name) == 0) {
					settings.videoCodec = kVideoNames[c].codec;
					found = true;
				}
			}
			if (!found)
				usage();
		} else if (strcmp(arg, "--audio") == 0 && next != NULL) {
			bool found = false;
			for (size_t c = 0; c < sizeof(kAudioNames) / sizeof(kAudioNames[0]);
					c++) {
				if (strcmp(next, kAudioNames[c].name) == 0) {
					settings.audioCodec = kAudioNames[c].codec;
					found = true;
				}
			}
			if (!found)
				usage();
		} else if (strcmp(arg, "--quality") == 0 && next != NULL) {
			settings.quality = atoi(next);
			settings.rateMode = kRateConstantQuality;
		} else if (strcmp(arg, "--bitrate") == 0 && next != NULL) {
			settings.videoBitrate = atoi(next);
			settings.rateMode = kRateAverageBitrate;
		} else if (strcmp(arg, "--speed") == 0 && next != NULL)
			settings.speed = (speed_preset)max_c(0, min_c(4, atoi(next)));
		else if (strcmp(arg, "--height") == 0 && next != NULL)
			settings.maxHeight = atoi(next);
		else if (strcmp(arg, "--fps") == 0 && next != NULL)
			settings.frameRate = atoi(next);
		else if (strcmp(arg, "--gpu") == 0 && next != NULL)
			settings.gpu = next;
		else if (strcmp(arg, "--abitrate") == 0 && next != NULL)
			settings.audioBitrate = atoi(next);
		else if (strcmp(arg, "--mixdown") == 0 && next != NULL) {
			settings.mixdown = strcmp(next, "mono") == 0 ? kMixdownMono
				: strcmp(next, "stereo") == 0 ? kMixdownStereo
				: strcmp(next, "5.1") == 0 ? kMixdown51
				: strcmp(next, "7.1") == 0 ? kMixdown71 : kMixdownSource;
		} else if (strcmp(arg, "--tracks") == 0 && next != NULL) {
			settings.audioTracks = strcmp(next, "first") == 0
				? kAudioTracksFirst : strcmp(next, "default") == 0
					? kAudioTracksDefault : kAudioTracksAll;
		} else if (strcmp(arg, "--subs") == 0 && next != NULL) {
			settings.subtitles = strcmp(next, "copy") == 0 ? kSubtitlesCopy
				: strcmp(next, "burn") == 0 ? kSubtitlesBurnFirst
				: strcmp(next, "burn-forced") == 0 ? kSubtitlesBurnForced
				: kSubtitlesNone;
		} else if (strcmp(arg, "--deinterlace") == 0 && next != NULL) {
			settings.deinterlace = strcmp(next, "off") == 0 ? kDeinterlaceOff
				: strcmp(next, "always") == 0 ? kDeinterlaceAlways
				: kDeinterlaceAuto;
		} else if (strcmp(arg, "--denoise") == 0 && next != NULL)
			settings.denoise = (filter_strength)max_c(0, min_c(3, atoi(next)));
		else if (strcmp(arg, "--sharpen") == 0 && next != NULL)
			settings.sharpen = (filter_strength)max_c(0, min_c(3, atoi(next)));
		else if (strcmp(arg, "--rotate") == 0 && next != NULL)
			settings.rotation = atoi(next);
		else if (strcmp(arg, "--start") == 0 && next != NULL)
			settings.start = (bigtime_t)(atof(next) * 1000000);
		else if (strcmp(arg, "--end") == 0 && next != NULL)
			settings.end = (bigtime_t)(atof(next) * 1000000);
		else {
			consumed = false;
			if (strcmp(arg, "--cpu") == 0)
				settings.encoderPreference = kPreferSoftware;
			else if (strcmp(arg, "--cfr") == 0)
				settings.peakFrameRate = false;
			else if (strcmp(arg, "--passthrough") == 0)
				settings.passthroughWhenPossible = true;
			else if (strcmp(arg, "--crop") == 0)
				settings.autoCrop = true;
			else if (strcmp(arg, "--gray") == 0)
				settings.grayscale = true;
			else if (strcmp(arg, "--flip") == 0)
				settings.flip = true;
			else if (strcmp(arg, "--quiet") == 0)
				quiet = true;
			else if (arg[0] == '-')
				usage();
			else if (source == NULL)
				source = arg;
			else if (destination == NULL)
				destination = arg;
			else
				usage();
		}
		if (consumed)
			i++;
	}
	if (source == NULL || destination == NULL)
		usage();
	if (settings.Validate())
		fprintf(stderr, "note: settings adjusted to fit the container\n");

	TranscodeJob job;
	job.source = source;
	job.destination = destination;
	job.settings = settings;

	Transcoder transcoder(job);
	Printer printer(quiet);
	BString error;
	bigtime_t start = system_time();
	status_t status = transcoder.Run(&printer, error);
	if (status != B_OK) {
		fprintf(stderr, "failed: %s\n", error.String());
		return 1;
	}
	printf("done in %.2f s\n", (system_time() - start) / 1e6);
	return 0;
}
