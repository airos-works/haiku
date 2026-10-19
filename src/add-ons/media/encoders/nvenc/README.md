# The graphics card's video encoder, for MediaConverter

GeForce cards since Maxwell have a video encoder - NVENC - beside the decoder
the NVDEC add-on drives. This add-on encodes H.264 and H.265 on it, for
MediaConverter (`src/apps/mediaconverter`), through the interface in
`headers/private/media/nvenc_api.h`. It is not a media kit add-on:
MediaConverter loads it from `non-packaged/add-ons/media/encoders/nvenc`.

| | |
| --- | --- |
| `nvenc_engine.c` | The engine: the NVDEC add-on's resman client, address space and buffers, with a channel on NVENC0 and the card's NVxxB7 object on it. Knows both kinds of channel - the pre-Volta one NVDEC uses (resman's USERD, kicked through GPPut) and the Volta and later one (our USERD, kicked through the usermode doorbell, as NVK's resman back end does). Finds out what the card is: its name, its architecture and, from its class list, which encoder class it has and so which formats. |
| `nvenc_common.c` | Writing headers bit by bit, putting a picture into the arrangement the engine reads, the packet queue, a rate control for the bit rate modes, level tables. |
| `nvenc_h264.c` | H.264 Main: I and P pictures, one reference, CABAC, one slice per picture. The parameter sets are written here; the engine writes the slices. |
| `nvenc_hevc.c` | H.265 Main and Main 10, the same way, with 32x32 coding tree blocks (I pictures only work so far, see below). |
| `nvenc_addon.c` | The `nvenc_api` table. |
| `test/nvenctest.c` | Encodes test pictures with the add-on on a card and writes the stream (see below). |
| `test/nvpoll.cpp` | Samples the engine's falcon registers through `/dev/misc/poke`, and dumps all of them when the engine hangs. |
| `test/fake_nvenc.cpp` | A stand-in that implements the same interface with x264, to try MediaConverter's side without a card (see below). |

## What is known and what is not

No open driver in Mesa or the kernel drives NVENC yet. Everything this add-on
tells the engine comes from NVIDIA's `nvenc_drv.h` and class headers, and from
what a GTX 1070 (GP104, class C2B7, the X399, 2026-10-19) was seen to need. The
tuning values of the motion search, mode decision and quantisation controls
are those NVIDIA's own driver uses, as recorded by an H.264 encoder written for
an RTX 3050 (Mesa merge request 45349, which says its tables were taken from
NVIDIA's library and are not for redistribution; nothing of it is copied here,
the fields are set by name).

H.264 works: I and P pictures, any size from 32x32 to 4096x4096. A 12 second
1080p clip converts at 106 pictures/s (3.6 times real time, decoding the
source included) and comes back at 44.6 dB. H.265 Main and Main 10 encode I
pictures, but the engine refuses every P picture (see below), so
MediaConverter's test fails for H.265 on this card and it encodes H.265 on the
CPU and says so.

What the header does not say, found on the card:

- Input, reference and reconstructed pictures are all in the video block
  linear arrangement NVDEC writes (512 byte groups of 64 bytes by 8 lines,
  lines in fours, two groups to a block), `block_height` 2, `tiled_16x16` 0;
  chroma at its own address with its offset 0. The header says reference
  pictures must be tiled 16x16; block linear works and is what NVIDIA's driver
  uses on the RTX 3050.
- `pic_control.codec` is 0 for H.264 and H.265. The decoder's numbers (3, 7),
  which `SET_CONTROL_PARAMS` does use, make the engine raise
  ROBUST_CHANNEL_NVENC0_ERROR (47) with `error_status` 1 and nothing written.
- `slice_mode` 0 (one dynamic slice, `max_slice_size` 0); 1, the slice
  control array, raises the same error.
- `FORCE_OUT_PIC` in `SET_CONTROL_PARAMS`: without it the reconstructed
  picture is never written, even with `ref_pic_flag` set, and P pictures are
  predicted from black.
- Temporal hints in the motion search read the previous picture's motion
  predictor buffer: with them enabled `SET_IN_MEPRED_DATA` must be sent, or P
  pictures raise error 47.
- A finished picture has `error_status` 2 in the status buffer, not 0.
- The coded width must be whole macroblocks (H.264) or coding tree blocks
  (H.265, which also refuses a last block row or column that is half or more
  empty, e.g. 1280x720); the SPS crops to the picture.
- The history buffer takes 384 bytes per macroblock column; smaller faults the
  card's memory unit (RC 31) at 1080p.
- With 16-bit samples (Main 10) `sfc_pitch` counts samples, not bytes.
- Pascal's H.265 picture control is four bytes shorter than the one in Mesa's
  header (no `stats_fifo_offset`), so `half_scaled_outputpic_cfg` and
  `gpTimer_timeout_val` sit four bytes earlier; the add-on sets nothing after
  the picture control. (NVIDIA's 2021 header in open-gpu-doc, 97c510e793,
  which covers C2B7, shows this; its H.264 structures are the same as the
  current ones.)
- The H.265 application reads `l1[0]` even for P pictures; -1 there sends it to
  `SET_IN_REF_PIC15` and faults the memory unit, so unused entries stay 0.
- Not needed: `SET_WATCHDOG_TIMER`, `SET_IO_RC_PROCESS`, `gpTimer_timeout_val`
  (only with `GPTIMER_ON`), the status offsets.

H.265 P pictures: with all of the above the engine still raises error 47
before writing anything for any P picture, 8 or 10 bit, while non-IDR I
pictures with the same reference picture set encode. Not the cause: the
motion search controls (NVIDIA's, ours, none), forced intra, mode decision
transform search, merge candidates, minimum CU and TU sizes, tiled
references, explicit or SPS reference picture sets, `NumPocTotalCurr`,
`collocated_from_l0_flag`, list 1 mirroring list 0 as a B picture, colocated
input and `FORCE_OUT_COL`, `dist_scale_factor`, a half scaled reference,
`SET_UCODE_STATE`, larger buffers. With `codec` 5 instead the engine does
encode P pictures and reports microcode error 9, E_ME_PROGRAMMING (the
ERROR_CODES list in `nvenc_drv.h`), but writes slice headers that do not
parse. Watching the engine's falcon (`test/nvpoll.cpp`, NVENC0 at BAR0 + 0x1c8000
on GP104) shows what happens: on an H.265 P picture the falcon takes
external interrupt 0x400 from the encoder hardware, the units stay busy
(idle state 0x1d9a) and after about 0.9 s the watchdog fires (interrupt
0x2) and resman resets the engine, which is the error 47. So a hardware unit
hangs on something in the inter set-up; nothing in the registers says which.
A capture of NVIDIA's driver encoding H.265 on a Pascal card would settle
it.

`NVENC_DEBUG=1` makes the add-on print the error notifier and the status
words of each picture.

## Trying the add-on on a card

`test/build-nvenctest.sh <haiku build dir> <output file>` builds `nvenctest`,
which encodes a synthetic moving picture with the add-on and writes the
stream, and with a last argument the source too, to compare:

    nvenctest <add-on> h264|hevc|hevc10 <width> <height> <pictures> <out>
        [qp] [variant] [key interval] [source.yuv]

    ffmpeg -f rawvideo -pixel_format nv12 -video_size 1920x1080 -framerate 30 \
        -color_range tv -colorspace bt709 -color_primaries bt709 -color_trc bt709 \
        -i source.yuv -i out.264 \
        -lavfi '[1:v]setpts=N/(30*TB)[a];[0:v]setpts=N/(30*TB)[b];[a][b]psnr' -f null -

(the raw input needs the stream's colour tags, or FFmpeg converts one side
before comparing; `p010le` and `hevc10` for 10 bits).

MediaConverter encodes a small test picture sequence (an IDR picture and five
P pictures) through the add-on before it uses a card for a format
(`NvencEncoder::SelfTest`), decodes it and wants 32 dB; a format that fails is
encoded on the CPU and the window says why. The add-on can offer several ways
of handing the engine a picture (`variants`, `kNvencLayouts` in
`nvenc_common.c`) for the test to choose from; it now has the one found on the
card. The test runs in a process of its own (the same executable with
`--nvenc-self-test`, its debugger disabled), so an add-on that crashes or a
card that hangs costs the format, not MediaConverter. The result is kept in
`~/config/settings/MediaConverter NVENC test` per card and add-on build;
`mediaconvert --hardware` shows it, `MEDIACONVERTER_NVENC_RETEST=1` runs the
test again and `MEDIACONVERTER_NVENC_VARIANT=n` tries one way only.

AV1 (Ada and Blackwell) is in the header too (`nvenc_av1_*`) but not written
yet; MediaConverter encodes AV1 on the CPU, and says so, on those cards.

## Building it

Like the NVDEC add-on it needs NVIDIA's resource manager headers and so is not
built with the tree: `build-cross.sh <haiku build dir> <NVK tree> <output
dir>`, with `OGKM_SRC` naming the open-gpu-kernel-modules checkout. The air/OS
CI builds it with NVDEC (`deps/build-nvidia.sh`) into the x86_64 image's
`system/non-packaged/add-ons/media/encoders/nvenc`; on a machine without an
NVIDIA card it finds nothing and MediaConverter goes on without it.

## Trying MediaConverter's side without a card

`test/build-fake.sh <haiku build dir> <output file>` builds the stand-in.
Installed as `~/config/non-packaged/add-ons/media/encoders/nvenc`, and with
`MEDIACONVERTER_FAKE_NVIDIA=1b81` (a GTX 1070's PCI device ID) in the
environment, MediaConverter and `mediaconvert` believe there is an NVIDIA card
and run the self-test, the encoding and the CPU fallback through it.
`FAKE_NVENC_GOOD=n` makes only way n give correct pictures (the others come
back shifted, as a wrong arrangement would), `FAKE_NVENC_TIMEOUT=1` makes the
engine never answer, `FAKE_NVENC_CRASH=1` and `FAKE_NVENC_HANG=1` make the
first picture crash or hang the test process. On the X399 (2026-10-17) the test found the right way
among three, kept the result, stopped at the first time out, and a 1080p H.264
conversion through the stand-in came back at 44.7 dB.
