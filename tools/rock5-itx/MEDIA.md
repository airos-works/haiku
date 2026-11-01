# RK3588 hardware media decoding

The ROCK 5 ITX image includes hardware decoding for 8-bit H.264, H.265 and
AV1 video. The Media Kit decoder add-on uses Rockchip MPP and the in-tree
`rk3588_vpu` driver. `Rock5MediaPlayer` is a small sample application with
video, queued audio, play, pause and seek controls.

The qualified paths are:

| Format | RK3588 block | Media Kit input | Qualified output |
| --- | --- | --- | --- |
| H.264/AVC | RKVDEC0 (VDPU381) | MP4, converted to Annex B | NV12, then B_RGB32 |
| H.265/HEVC | RKVDEC0 (VDPU381) | MP4, converted to Annex B | NV12, then B_RGB32 |
| AV1 | VPU981 | MP4 with temporal delimiter insertion | NV12, then B_RGB32 |

This qualification covers the 1280x720, 8-bit Big Buck Bunny samples below.
Other profiles, bit depths, chroma layouts and containers need separate tests.

## Reproducible build

`build-ffmpeg-arm64.sh` builds and packages:

- FFmpeg 6.1.6 from the release archive with SHA-256
  `d4fcb164028dd3beee5d92c0ac72e46aac6973c75ea12dc14de07bf8f407370a`;
- Rockchip MPP `develop` commit
  `14729dd578e570e5f00fd1dd2113f5429012d64b` with
  `mpp/mpp-14729dd-haiku.patchset` (SHA-256
  `04dc008c41f397624121102743fe357f7663f9a7213195e22f9e8b6be49ec201`);
- Haiku's regular FFmpeg Media Kit add-on;
- `00_rockchip_mpp`, the RK3588 Media Kit decoder add-on;
- `librockchip_mpp.so.1`; and
- `Rock5MediaPlayer`.

Run the build from the Haiku source directory:

```sh
source tools/rock5-itx/env.sh
tools/rock5-itx/build-ffmpeg-arm64.sh
```

The resulting `rock5_ffmpeg-6.1.6-2-arm64.hpkg` is added to
`@rock5full-mmc`. The qualified package SHA-256 is
`bbca9a19d039d9a58430d2f47e33df485ab050e9a16c0e29145d7631b78a0540`.

## Driver safety model

The driver accepts MPP buffer allocation and decode requests from applications.
Power and register diagnostic requests require a privileged opener. Every
decode job:

1. validates the exact RK3588 block identity and the selected board profile;
2. validates all buffer handles, ownership, ranges and register addresses;
3. powers the required parent and child domains and configures their clocks;
4. submits one bounded job and polls completion for at most 500 ms; and
5. restores the exact PMU and CRU state, including failure and timeout paths.

The installed default settings enable the profiles qualified with EDK2 v1.1:

```text
power_profile rock5-itx-edk2-v1.1-vdpu-power
rkvdec0_profile rock5-itx-edk2-v1.1-rkvdec0-power
av1_profile rock5-itx-edk2-v1.1-av1-power
decode_profile rock5-itx-edk2-v1.1-rkvdec0-h264-h265
av1_decode_profile rock5-itx-edk2-v1.1-vpu981-av1
```

Host tests cover successful jobs, rejected state and addresses, timeouts and
restoration. They finish with `ROCK5_VPU_POWER_TEST_PASS`.

## Native hardware qualification

The source Big Buck Bunny samples live outside Git in
`/mnt/HaikuWork/artifacts/media-samples/`:

| Sample | SHA-256 |
| --- | --- |
| `bbb-20s-h264-aac.mp4` | `ef26380aa96bc1685c484a32f43e2163b86e6a692fad4a746c5fd8fc43f4a649` |
| `bbb-20s-h265-aac.mp4` | `074af9165f6813a2f3a93440e6497e52b6270e5ba4e0e0d1969863597bec959a` |
| `bbb-20s-av1-aac.mp4` | `68d8585d8dd704b7bdb7583e001f64e35f6ca6eaa2b6516dd54a877b90b14451` |

Direct MPP jobs produced byte-for-byte identical NV12 frames to host FFmpeg:

| Format | Hardware/software frame SHA-256 | PSNR |
| --- | --- | --- |
| H.264 | `d14068d66a82caede48a6292c79e5f5a7bc1b53790c303369b646138e7c973c2` | infinite |
| H.265 | `fdfa2bf4dc557ce0d570df8d17073712157e79acb55904d47a6924d032d9928d` | infinite |
| AV1 | `758c2896a527e20367cc47ccb8e4379761451a6d79368e4e245ce9b03cfc7a99` | infinite |

The Media Kit probe decoded the first video and AAC audio buffers, sought to a
later keyframe, and decoded both tracks again for all three formats. The sample
player continuously played AV1 with its audio path active, paused, sought to
about 10 seconds while paused, and resumed with a new video scene.

Native evidence is outside Git under
`/mnt/HaikuWork/artifacts/ffmpeg-arm64/native/`. The principal records are:

- `test-media-mpp-h264-v6.txt`, `test-media-mpp-h265.txt` and
  `test-media-mpp-av1.txt` for Media Kit decode and seek;
- `player-continuous-h264.txt` and `player-continuous-h265.txt` for continuous
  hardware frames;
- `check-hardware-player-av1-v7.txt`,
  `player-hardware-av1-v7-running.jpg` and
  `player-hardware-av1-resumed.jpg` for player controls and playback;
- `item7-final-media-probes.txt` for post-install H.264 and AV1 decode and seek;
- `item7-final-power-restored.txt` for the final all-domains-off snapshot; and
- `item7-final-nvme-boot-serial.log` for the installed NVMe reboot.

The final full image is
`/mnt/HaikuWork/artifacts/images/haiku-arm64-b37ec067153587c0.img`, SHA-256
`b37ec067153587c08c35ce0ddc28153ac14c828fbfd34cf91c6156603578ef77`.
It passed QEMU in `artifacts/qemu/20260922T194046Z-e3f870`. The final driver,
SHA-256
`ce0343a9acfe5727b37fdf91d562b02fb01b8f9eb45f6e351a3a66b98969aa4d`,
and package were installed on the NVMe system. A serial-captured reboot and
post-install tests passed. The final PMU snapshot reported RKVDEC0, RKVDEC1,
VDPU and AV1 off after decoding.

The player's `BSoundPlayer` path and AAC decode are working. Physical audio
output qualification is tracked separately with the board audio work.

## Ten-bit HEVC and 4K playback

On 2026-10-03 `00_rockchip_mpp` gained HEVC Main 10 output and the driver
lighter cache maintenance; airTime 1.0.0-11 uses both. The film tested was a
3840x1600 HEVC Main 10 HDR10 UHD Blu-ray encode (23 GB, DTS-HD audio) read
from an SMB share, played in airTime on the NVMe installation at 1920x1080.

The add-on:

- turns MPP's ten-bit output (`MPP_FMT_YUV420SP_10BIT`, four samples in five
  bytes, Linux's NV15) into P010 with NEON, 2.1 ms a 4K picture; the
  unpacking matches a scalar reference bit for bit;
- offers eight-bit pictures as NV12, a copy of rows, where the B_YCbCr422
  repacking took a single thread most of a 4K picture's time; B_RGB32 and
  B_YCbCr422 remain, from either depth;
- converts straight into the caller's buffer, keeps the first picture for
  `Decode()` however often the output format is negotiated, and leaves out
  pictures MPP marks broken or discarded (those after a seek that refer to
  pictures before it), as FFmpeg's rkmpp decoder does;
- no longer asks MPP for NV12 when the stream is AV1: on the RK3588 that
  request makes VPU981 cut ten-bit pictures to eight. Ten-bit AV1 comes out
  in the same five-bytes-for-four layout as RKVDEC's and becomes P010;
- resets MPP on a seek instead of destroying it. Destroyed in the middle of
  a picture it kept that picture's buffer (14 MB of the DMA pool at 4K) for
  good, and a new context allocated and cleared all of its buffers again.

The driver no longer cleans and invalidates the buffers only the decoder
uses: reference pictures, the one errors are concealed from, motion vectors
and row caches (RKVDEC registers 131-142, 164-179, 181-196). MPP writes
none of them; a missing reference gets a new buffer, cleaned when it was
handed out. The stream, the tables and the picture being decoded are still
maintained before and after every job. VPU981 (AV1) jobs leave out their
seven references' luma, chroma, motion vector and tile table registers and
the decoder's column and synchronisation buffers in the same way; the
post-processor's output and every register not identified stay
maintained. The power-down log line now also gives the time spent in the
decoder.

The DMA pool is 640 MiB by default (it was 320). A 4K ten-bit AV1 stream
keeps more pictures of 21 MiB than 320 MiB holds; the rest came from write
combining memory, from which the add-on's unpacking took 58 ms a picture
instead of a few, and the stream played at 15 pictures a second. The
driver now logs, once, when a buffer misses the pool; `dma_pool_mb` sets a
smaller pool on boards with less memory.

| Measurement (the film above) | Before | After |
| --- | --- | --- |
| Decoder in airTime | libavcodec (Main 10 refused) | RKVDEC, P010 |
| Pictures shown per second (24 wanted) | 1-2 | 24.0, none dropped |
| airTime decode time a picture | 100 ms | 10 ms |
| VPU job, average (decoding alone) | 17.2-17.9 ms | 8.1 ms (5.3 ms) |
| VPU job, slowest | 47-50 ms | 24.8 ms |

A 4K ten-bit HDR10 AV1 clip (SVT-AV1, 24 pictures a second) went from
libavcodec to VPU981 at 23.9 pictures a second, none dropped; its jobs took
20 ms (13 ms decoding), where full maintenance took 34 ms.

Seeks across the whole film (to 5, 10, 20, 41, 66, 90 and 135 minutes)
resumed at 24 pictures a second with no buffer left behind; scanning at 8x
and -16x showed 9-10 key frames a second. 1080p H.264 (Blu-ray x264),
3840x1600 eight-bit SDR HEVC, 1080p ten-bit SDR HEVC and a 3832x1596 Dolby
Vision/HDR10 MP4 all played on RKVDEC at 24 pictures a second.

## Encoding, VP9 and interlaced H.264 (2026-11-01)

On the CI installation (hrev60207+2161) of 2026-11-01, measured on the board
with the rk3588_vpu driver, 00_rockchip_mpp add-on and MediaConverter of
haiku master after d0c9e5322f.

### RKVENC encoding

`rk3588_vpu` serves MPP's RKVENC client (16) on core 0 of the VEPU580:
VENC0 power (Linux 6.18.52 pm-domains.c bits under VCODEC), the firmware's
ungated RKVENC0 clocks checked, register words only in the blocks MPP's
VEPU580 H.264/H.265 HALs write, every address register a buffer of the
team, the block's IOMMU in bypass, jobs polled to completion with the BSP
driver's sequence (`mpp_rkvenc2.c`) and its safe reset after an error. The
gate is `encode_profile rock5-itx-edk2-v1.1-rkvenc0-h264-h265`; without the
line, the qualified decode profile admits it too (installations keep the
settings file they were made with).

| Test (MPP's own `mpi_enc_test`, airos-works/mpp 4ad48e27) | Result |
| --- | --- |
| 1280x720 H.264 High, 60 pictures | 2.0 ms a picture, luma PSNR 46.4 dB average |
| 1280x720 H.265 Main, 60 pictures | 2.0 ms, 54.9 dB |
| 3840x2160 H.264, 10 pictures | 16 ms, 52.1 dB |
| 3840x2160 H.265, 10 pictures | 17 ms, 57.8 dB |

MediaConverter's `MppVideoEncoder` (backend "RKVENC") encodes H.264 High and
H.265 Main through MPP, opened at run time. 30 s of a 1080p H.264 episode to
H.265 took 4.98 s (151 pictures a second, 44.5 dB against the source); 20 s
of a 3840x1600 HEVC film 17.5 s, against 312 s with x265. Software decoding
of the source is now the limit at 4K. Ten bits stay on the CPU (the VEPU580
encodes eight).

### VP9 and fields

RKVDEC's VP9 (MPP's vdpu34x VP9 HAL, decode mode 2) is admitted by the
driver and taken by the add-on: 60 of 60 pictures of a 1080p YouTube VP9 clip
byte-identical to libavcodec. MPP gives H.264 PAFF/MBAFF pictures as two
woven fields, byte-identical to libavcodec for every picture libavcodec
outputs; the add-on marks them `field_sequence` 2 and airTime 1.0.0-33
deinterlaces them (1080i broadcasts at 50/60 pictures a second, none
dropped). The add-on now registers its formats only where
`/dev/video/rk3588_vpu/0` exists.

### MPEG-1/2, MPEG-4 Part 2 and H.263 on VPU121

The driver also serves MPP's VDPU2 client: VPU121, the Hantro decoder in the
VDPU domain (0xfdb50400), for MPEG-1/2 (fields woven), MPEG-4 Part 2 and
H.263, with the BSP `mpp_vdpu2.c` sequence and address table. Against
libavcodec: 80 pictures of a 1080i MPEG-2 broadcast at 70.7 dB luma PSNR
average (IDCT rounding), 80 of an XviD episode at 73.0 dB. MPEG-4 GMC
(S-VOPs) is refused by MPP and stays in software.
