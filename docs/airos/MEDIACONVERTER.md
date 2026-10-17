# MediaConverter

MediaConverter converts video and audio files, in the spirit of HandBrake
but with fewer knobs: a queue of files on the left, what the selected file
holds at the top, and what to make of it below. Since branch
`mediaconverter-hw` (2026-10-17) it is a new application on FFmpeg that
encodes on the graphics card's video engine when the card has one that can
make the chosen format, and says so - or warns that the CPU will have to do
it, which takes longer and uses more power.

It replaces Haiku's MediaConverter (same signature,
`application/x-vnd.Haiku-MediaConverter`, same icon) in the haiku package.

## What it makes

Containers: MP4, Matroska (MKV), WebM and QuickTime (MOV), and for sound
only M4A, MP3, FLAC, Ogg, Opus and WAV.

| Video | On the GPU | On the CPU |
| --- | --- | --- |
| H.264 | AMD VCE/VCN (VA-API), NVIDIA NVENC (Maxwell and later) | x264 |
| H.264 10-bit | – | x264 |
| H.265 | AMD Polaris and later (VA-API), NVIDIA NVENC (second Maxwell and later) | x265 |
| H.265 10-bit | AMD VCN 2 and later, NVIDIA Pascal and later | x265 |
| H.265 12-bit | – | x265 |
| AV1, AV1 10-bit | AMD VCN 4 (RDNA 3, Phoenix and later), NVIDIA Ada Lovelace and later | SVT-AV1 |
| VP9 | (Intel only, which has no VA-API driver here) | libvpx |
| VP9 10-bit | – | libvpx with high bit depth (the image's own, see below) |
| VP8, MPEG-4 Part 2, MPEG-2, Theora, FFV1 | – | FFmpeg |

Those are HandBrake's video formats. The GPU column is what the cards can
do; what a given machine can do is under *Settings ▸ Video hardware*, which
puts every format against every card it found and the CPU.

Sound: AAC, HE-AAC (FFmpeg's libfdk_aac), MP3, MP2, AC-3, E-AC-3, TrueHD,
FLAC (16 and 24 bits), ALAC (16 and 24 bits), Opus, Vorbis and PCM, or the
source's own tracks copied, with a mixdown (mono, stereo, 5.1, 7.1) and a
sample rate. Subtitles are kept (text converted to the container's own text
format: MP4 timed text, WebVTT, SubRip), left out, or burned into the
picture (text through libass, pictures such as Blu-ray PGS and DVD subtitles
over the source picture). Chapters and the title and other tags are kept;
chapter starts become key frames.

The picture: a size limit (4K down to 360p; the aspect ratio is kept, the
source's sample aspect ratio too), frame rate (same, or 23.976 to 60, as a
limit or constant), deinterlacing (bwdif, only the pictures marked
interlaced or all of them), denoise (atadenoise, fftdnoiz), sharpen (unsharp), black and
white, cutting off black borders (ten pictures from the film are measured),
turning and mirroring. The source's own rotation is applied.

## Presets

*General*: Fast 1080p (H.264 and AAC in MP4), High quality 1080p, Small 720p
(H.265), 4K H.265 10-bit (MKV, HDR kept), AV1 10-bit (MKV), Change container
(MKV or MP4, copies everything). *Web*: WebM VP9, WebM AV1, Small for
messaging. *Devices*: Apple (H.265), Android and Chromecast, Old players
(MPEG-2 and AC-3). *Production*: Lossless archive (FFV1 and FLAC).

For songs: MP3 at 128, 192, 256 and 320 kbit/s (the default), AAC (M4A) at
128, 192 and 256 kbit/s, FLAC in CD quality (16-bit, 44.1 kHz) or as the
source (24 bits only when the source has more than 16), Apple Lossless,
Opus 160 kbit/s, Ogg Vorbis 192 kbit/s and WAV. They keep the tags, the
chapters and the album artwork: the file's own picture, or else the
`cover.jpg` (`folder`, `front`, `albumart*`, `album`; JPEG or PNG) beside it,
written as an ID3 or FLAC picture or M4A's `covr` (Ogg, Opus and WAV take
none). A song sees only these; a film sees them after its own, as *Sound
only*, to take the sound out of it. Songs and films keep settings of their
own, so a queue of both converts each with its kind's.

## The window

- **The queue**: files or folders dropped on it (or added with +). Each file
  says what it is, then while converting how far it is, how fast (pictures a
  second, or times real time for sound), and how long is left; at the end
  where the result went, its size and which encoder made it. A double click
  on a finished file plays it.
- **Source**: a picture from ten percent in, the title, container,
  duration, size and bit rate, and one row per track: codec with profile and
  level, size and display aspect ratio, frame rate (and whether it varies),
  bit depth and chroma, SDR/HDR10/HLG/Dolby Vision and the primaries,
  interlacing, rotation, bit rate; channel layout, sample rate, language,
  default, forced and commentary flags; and the chapters.
- **A song** (a file with sound and no picture track) shows its album
  artwork in a square, under it airTime's controller (play, scrub, skip,
  volume; `app/airtime` is airTime's ControlBar and Look as they are), its
  sound (codec, bit depth, sample rate, channels) and its tags. The sound
  card is only taken when it plays. The output has two tabs: *Summary*
  (format, codec, bit rate, channels, sample rate, the tags and artwork,
  copying sound already in the format) and *Range*; the line under them says
  what it becomes and about how big it will be.
- **Output**: the preset, then *Summary* (format, video, size, quality,
  audio and bit rate, subtitles), *Video* (encoder: automatic, a given GPU or
  the CPU; speed; constant quality or average bit rate; frame rate),
  *Filters*, *Audio* (which tracks, channels, sample rate, copying tracks
  that are already in the format, chapters, tags) and *Range* (from, to).
  Changing anything makes the preset *Custom*.
- **The encoder line**, always under the tabs: green when the GPU encodes
  ("H.265 is encoded on the GPU: AMD Radeon Pro WX 5100"), amber when the
  format has to go to the CPU ("No GPU here can encode AV1: it will be
  encoded on the CPU, which takes longer and uses more power"; its tool tip
  says why, card by card), red when nothing can (a missing x264 package).
  *Convert* asks once more before a CPU encode a GPU could not take
  (*Settings ▸ Warn before encoding on the CPU* turns that off).
- Results go next to each source, or to a chosen folder; an existing file is
  never overwritten (`name (2).mp4`). A notification says when the queue is
  done.

## How it is built

```
src/apps/mediaconverter/engine/    the engine (libmediaconverter_engine.a)
  Formats            containers, video and audio formats, what goes in what
  MediaProbe         SourceInfo from libavformat; the preview picture
  OutputSettings     the settings, archived as a BMessage; Presets
  Transcoder         one file: demux, decode, filter, encode, mux
  VideoEncoder       the encoder interface: frames in, packets out
  VaapiEncoder       H.264 and H.265 on VA-API (AMD), BitWriter for headers
  NvencEncoder       NVIDIA through the nvenc add-on (nvenc_api.h)
  SoftwareEncoders   x264, x265, SVT-AV1, opened with dlopen()
  LavcVideoEncoder   FFmpeg's own and libvpx/libtheora
  HardwareCaps       the GPUs: device manager PCI walk, VA-API queries,
                     NVENC by chip generation
src/apps/mediaconverter/app/       the window
src/apps/mediaconverter/cli/       mediaconvert, the engine from a shell (tests)
src/apps/mediaconverter/thirdparty headers of libva, x264, x265, SVT-AV1
headers/private/media/nvenc_api.h  the nvenc add-on's interface
```

The engine links the system FFmpeg 6 (the `ffmpeg` build feature): HaikuPorts'
`ffmpeg6` on x86_64, and on arm64 air/OS CI's own FFmpeg 6.1, `rock5_ffmpeg`,
whose devel package the image build hands to the build system
(`HAIKU_ARM64_FFMPEG_PACKAGES_DIR`; that package brings its own media plugin,
so the image leaves this tree's out). x264, x265 and SVT-AV1 (HaikuPorts'
`x264`, `x265`, `svt_av13`) and libva are opened at run time: MediaConverter
starts without them and names the missing package. The air/OS x86_64 images
carry all three (airos-ci `images/build-image.sh`), and a libvpx 1.16 of their
own in `system/non-packaged/lib` (airos-ci `deps/build-libvpx.sh`): HaikuPorts'
is libvpx's generic C build without high bit depth, so VP9 10-bit was
impossible and VP9 took twice as long.

HaikuPorts has no arm64 packages, so air/OS CI builds the CPU encoders for the
arm64 images itself (airos-ci `deps/recipes/x264.sh`, `x265.sh`, `svt_av1.sh`,
`libvpx.sh`, `lame.sh`, from the releases HaikuPorts uses, with NEON) and
builds rock5_ffmpeg with libvpx and LAME. On the Cubie A7S, two seconds of
1080p: H.264 50.0 dB (8 s), H.265 49.2 (48 s; x265 3.5 has no AArch64
assembly), H.265 10-bit 47.8, AV1 48.5 (25 s), VP9 46.0 and 10-bit 45.8,
VP8 48.4, and MP3. arm64's FFmpeg has no soxr, libopus, libvorbis or
libtheora: the engine resamples with swresample's own, takes FFmpeg's own
Opus and Vorbis encoders, and Theora cannot be made there. Where an encoder
is missing the format says why, and the first preset is the first one the
computer can make.

### The transcoder

One file is read with libavformat. Video is decoded with libavcodec (frame
and slice threads), goes through a libavfilter graph built for the settings
(`bwdif`, `crop`, `transpose`, `scale`, `atadenoise` or `fftdnoiz`, `unsharp`, `hue`, `fps`,
`subtitles` or an `overlay` for picture subtitles, and `format` for what the
encoder takes - NV12 or P010 for the GPUs, planar YUV for the CPU encoders)
and is encoded by a `VideoEncoder`. Sound goes through `aresample` (soxr) and
`aformat` to the encoder's rate, layout and sample format, in the encoder's
frame size. Streams kept as they are are copied packet for packet; copies
start at the first key frame at or after the start. The muxer gets the
encoder's parameter sets as extradata (MP4 `faststart`).

Times: a negative start time (an MP4 edit list's pre-roll) is not the zero
point; frames before the range's start are dropped after decoding.

### VA-API (AMD)

Mesa's radeonsi on amdgpu_drm, through libva. The structure is that of
FFmpeg's VA-API encoders, which Mesa is tested against:

- The sequence, picture and slice parameter buffers, and the parameter sets
  and slice headers as **packed headers** written here (`BitWriter`,
  Annex B with emulation prevention). Mesa parses them; its VCE path (H.264
  on Polaris) only puts the SPS and PPS in the stream when a packed slice
  header comes too.
- Mesa writes its **own** parameter sets from what it parsed and what the
  hardware does (on Polaris's UVD encoder the H.265 SPS had height 1088 and
  no conformance window when the surfaces were 1080 lines; the decoded film
  was squashed, 22 dB). So the surfaces follow the driver's
  `VASurfaceAttribAlignmentSize` (64 x 16 for H.265 and AV1 on radeonsi) and
  the cropping is in the parameter sets, and the container's extradata is
  taken from one gray picture encoded and thrown away at open time.
- I and P pictures, P referring to the one before (VCE has one reference
  and no B pictures), constant QP from the quality slider (QP 16 to 36) or
  VBR with HRD for a bit rate, the frame rate and Mesa's quality bits
  (speed, balanced, quality presets, pre-encoding and VBAQ) as misc
  parameters. Three pictures are in the encoder at once.
- Upload: `vaDeriveImage` and a copy into the mapped surface (0.5 ms for
  1080p on the WX 5100), `vaPutImage` where deriving fails.

### NVIDIA (NVENC)

NVENC is reached the way the NVDEC add-on reaches NVDEC: through the
nvidia_rm resource manager, with NVIDIA's class headers (`clc?b7.h`) and
`nvenc_drv.h`. Those live in the NVK tree, so the encoder is an add-on built
beside NVDEC (`src/add-ons/media/encoders/nvenc`, installed as
`system/non-packaged/add-ons/media/encoders/nvenc`), and MediaConverter talks
to it through `headers/private/media/nvenc_api.h`. The add-on finds the card's
encoder class in its class list (Maxwell's C0B7 to Blackwell's CFB7), opens a
channel of the kind the card has (pre-Volta, or Volta and later with the
usermode doorbell) and encodes H.264 Main and H.265 Main and Main 10 - I and P
pictures, one reference, CABAC - writing the parameter sets itself and letting
the engine write the slices. AV1, which Ada and Blackwell have, is not in the
add-on yet: on those cards AV1 goes to the CPU, with the warning.

No open driver drives NVENC, and the header does not say how each generation
wants its input picture laid out. The add-on knows several ways
(`variants`), and before a card encodes anything MediaConverter encodes a
small test picture each way until one decodes again with libavcodec above
32 dB (`NvencEncoder::SelfTest`, run in the background when the window
opens, or before the first conversion). A format that fails counts as one
the card cannot encode: the badge, the hardware window and the CPU warning
say so, with the reason. Results are kept per card and add-on build in
`~/config/settings/MediaConverter NVENC test`. This has not met an NVIDIA
card yet - the lab has none since 2026-10-12 - and was tried end to end with a
stand-in add-on that encodes with x264 (`nvenc/test/fake_nvenc.cpp`, see the
add-on's README).

The hardware inventory knows every NVIDIA chip's encoder generation by PCI id
(no NVENC on GM108, GP108, GA100 and GH100), so the window can say what a
card would do even before its driver is there.

### The hardware inventory

`HardwareInventory` walks the device manager's PCI nodes for display
controllers, opens VA-API on every `/dev/dri/renderD*` and asks each
profile's encode entry point for its formats, maximum size and whether it
has B pictures. AMD cards without amdgpu_drm, NVIDIA cards and Intel graphics
are listed with what they would do and why they cannot now.
`EncoderFor(format, width, height)` picks the card for an encode, and
`WhyNotHardware()` says why none can.

## Measured

On the X399 (Threadripper 1950X, Radeon Pro WX 5100), the 12-second 1080p30
Big Buck Bunny clip, luma PSNR against the source with
`mediaconvert --compare`:

| Encoder | Result |
| --- | --- |
| WX 5100 H.264 (VCE 3.4), QP 24 | 43.7 dB; 9.6 ms a picture (speed), 13.6 (balanced), 26 (quality) |
| WX 5100 H.265 (UVD 6.3 encoder), QP 24 | 44.1 dB; 7.5 ms a picture |
| x264 medium, 720p | 45.2 dB; 7.4 ms a picture on 48 threads (28 ms when x264 counted one processor) |
| x265 medium, 720p, 8/10/12-bit | 45.8 / 45.6 / 44.5 dB |
| SVT-AV1 preset 8, 720p, 8/10-bit | 46.1 / 46.0 dB |
| VP9, VP8, MPEG-4, MPEG-2 | 43.8, 44.8, 46.3, 46.4 dB |

A 1:32 Matroska file with two audio tracks (AAC stereo, AC-3 5.1), three
subtitle tracks and four chapters became an MP4 with both tracks as AAC
stereo, the subtitles as timed text with their languages and forced flag,
and the chapters (42.1 dB over 2778 pictures), from the window.

## Testing

The application takes files, a preset and a request to convert on its
command line - `MediaConverter --preset "AV1 10-bit (MKV)" --convert
film.mkv` - which scripts and tests use; the CPU warning, if it comes, is
answered with the window's `'cpwa'` message (`which` 0 cancel, 1 don't warn
again, 2 encode on the CPU), e.g. `hey MediaConverter cpwa of Window 0 with
which=int32(2)`.

`mediaconvert` is the engine from a shell (built with the app, not in the
image):

```
mediaconvert --hardware                     the GPUs, their formats, who encodes what
mediaconvert --info <file>                  what a file holds
mediaconvert [options] <source> <output>    convert (--help lists the options)
mediaconvert --compare <source> <output>    luma PSNR, pictures paired by time
                                            (COMPARE_BY_ORDER=1: in order)
mediaconvert --dump <file> <n> <out>        extradata and the first n packets
MEDIACONVERTER_PROFILE=1 mediaconvert ...   time per stage, and the VA-API upload/sync split
MEDIACONVERTER_NVENC_RETEST=1 ...           run the NVENC self-test again
MEDIACONVERTER_NVENC_VARIANT=n ...          try only the add-on's input layout n
MEDIACONVERTER_FAKE_NVIDIA=1b81 ...         pretend there is a GTX 1070 (with the
                                            stand-in add-on, nvenc/test)
```

## Known limits

- NVENC has not run on an NVIDIA card yet (see above), and B pictures are
  not used on NVIDIA.
- AV1 on Ada and Blackwell is encoded on the CPU: NVENC's AV1 needs the
  driver to supply default probability (CDF) tables in a layout nothing
  documents, and to write frame headers from values the engine computes.
- Theora: now and then libtheora hands back no packet for a picture and
  FFmpeg's wrapper drops it (12 s of Big Buck Bunny: one picture, after the
  first key frame). Timestamps stay right, so playback holds the previous
  picture once; `--compare` reports it as an unpaired source picture (the
  rest: 42.3 dB).
- arm64 has no Theora, and no hardware encoding on the ROCK 5's or the
  Cubie's video engines yet.
