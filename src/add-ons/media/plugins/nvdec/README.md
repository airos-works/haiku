# The graphics card's video decoder, as a media add-on

The GeForce cards this fork runs on have a video engine - NVDEC - that decodes
H.264 and H.265 without the processor doing any of the arithmetic. This add-on
offers H.264 to Haiku's media kit, so that any program that plays video gets
it: the file is opened as usual, and the pictures come back from the card.
H.265 is there for programs that ask for it (see below).

| | |
| --- | --- |
| `nvdec_engine.c` | The engine itself: a resman client, an address space, buffers both sides can see, and a channel on the decoder with an NVC2B0 object on it. Also where the card's arrangement of pixels in memory is written down. |
| `h264_parse.c` | Sequence and picture parameter sets, and as much of a slice header as says which picture a slice belongs to. Nothing here touches picture data: the engine parses every entropy-coded bit itself. |
| `nvdec_h264.c` | The bookkeeping the engine does not do - which pictures are still needed as references, what order they are shown in, which surface each one is in. |
| `hevc_parse.c` | The same for H.265: parameter sets, and slice headers as far as the reference picture sets, whose length in bits the engine is told so it can step over them. |
| `nvdec_hevc.c` | H.265's reference picture sets, picture order and output ("bumping") process, and the picture setup. |
| `nvdec_convert.c` | Out of the card's arrangement and into pixels, in bands, across as many processors as there are. |
| `NVDecPlugin.cpp` | The media kit's side of it. |

## What it does and does not decode

Eight bit 4:2:0 H.264, progressive, up to what the engine allows - 4096 wide on
this generation. Field pictures, 4:2:2, 4:4:4 and more than eight bits a sample
are refused rather than decoded wrongly.

H.265 Main and Main 10, 4:2:0, up to 8192 by 8192. Ten-bit pictures come out
as sixteen-bit samples with the value in the top bits; a caller can have them
as they are by asking for the colour space `'P010'` (`NVDEC_COLOR_SPACE_P010`
in `NVDecPlugin.h`), or as eight-bit YCbCr422 or RGB32 like H.264.

Haiku picks one decoder for a format, by add-on directory, so this add-on takes
H.264 away from libavcodec wherever it is installed and `/dev/nvidiactl`
exists (on a machine without nvidia_rm or an NVIDIA card it offers nothing, so
the air/OS x86_64 image can carry it everywhere). A stream it refuses will
not fall back: it will not play. That is the reason for refusing narrowly and
loudly rather than trying - and the reason H.265 is not offered to the media
kit at all: a program that wants it instantiates the decoder itself, sets it
up with an H.265 format, and keeps libavcodec for what it refuses. airTime
does.

## Building it

It is not built with the rest of the tree, because it needs NVIDIA's resource
manager headers, which live in the NVK tree (jmgasper/mesa-nvk, branch
`airos-nvk-r2`). `build-cross.sh` cross-builds it from a Haiku build
directory and that tree (the air/OS CI does, for the x86_64 image);
`docs/x399-workstation/tools/build-nvdec-plugin.sh` builds it on the
workstation and installs it into the user's non-packaged add-ons.

## What was measured

Thirteen H.264 streams and 120 frames of 1080p Big Buck Bunny decode byte for
byte identically to ffmpeg's own decoder. 1080p decodes at 232 pictures a
second - 4.3 ms each - and with the conversion to pixels, a whole 1080p film
plays at 148 pictures a second through the media kit. `tests/nvdecstream.c`
does the comparison; `tests/mediadecode.cpp` goes through the media kit and
says which decoder answered.

For H.265, eighteen streams made with x265 to try one feature each - B
pyramids, open GOPs, ten bits, transform skip, default and custom scaling
lists, slices, wavefronts, 16 and 32 pixel CTUs, weighted prediction, lossless,
no deblocking or SAO, six references, a cropped 1080p - and 240 pictures of a
4K HDR10 film all decode byte for byte as ffmpeg does
(`tests/nvdechevcstream.c`). A 4K ten-bit picture takes 8 ms in the engine and
6 ms to copy out as P010: the film plays at its 24 pictures a second with time
to spare.

Two things about H.265 are not as the header that describes the engine says:
the maximum transform skip size is sent as its log2, not log2 minus two, and
the scaling lists go column by column, which the Tegra driver's raster order
only gets away with for the default lists, which are symmetric.

Four things about the engine are not in the header that describes it, and each
cost a measurement to find. They are in the commit messages and in the comments
where they matter: the arrangement of pixels, the field markings that decide
whether a reference is a reference, the end of stream marker the bitstream
length has to count, and the reference table place a picture must keep.

## Parser regression checks

`jam h264_parse_test` builds a CPU-only test; it needs no NVIDIA headers or
GPU. It checks truncated parameter sets, unsigned-code limits, oversized
IDs, unterminated and excessive reference-list modifications, and MMCO
limits, then tries 25,000 reproducible malformed inputs. Optional arguments
are Annex B H.264 files; `--hevc` switches subsequent files to HEVC. The
stream checks parse every parameter set and slice header, without decoding
pixels. SPS dimensions are bounded to 8192 pixels by this parser; that
does not expand the engine's hardware capabilities.

The shared bit reader reports sticky errors, including truncation and a
code with 32 leading zero bits. H.264 parameter-set IDs and counts are
checked before indexing or arithmetic, complete parameter sets require
their RBSP stop bit, and overlong lists fail instead of silently dropping
entries. HEVC propagates the same reader failure. These checks do not
replace codec capability checks or validation at the kernel boundary.

On 2026-10-10 the test passed under host AddressSanitizer and
UndefinedBehaviorSanitizer and in x86_64 Haiku QEMU: 23 H.264 streams,
1,214 slice headers, and two generated HEVC Main/Main 10 streams with
24 slice headers each. The full NVDEC add-on cross-build also passed.
This parser-only regression run does not establish new GPU decode support.
