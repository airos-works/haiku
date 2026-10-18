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
| `nvenc_hevc.c` | H.265 Main and Main 10, the same way, with 32x32 coding tree blocks. |
| `nvenc_addon.c` | The `nvenc_api` table. |
| `test/fake_nvenc.cpp` | A stand-in that implements the same interface with x264, to try MediaConverter's side without a card (see below). |

## What is known and what is not

No open driver drives NVENC - Mesa's nouveau headers carry NVIDIA's
`nvenc_drv.h` and the class headers, and nothing uses them yet. Everything this
add-on tells the engine comes from those headers. They describe the picture
setup, the controls for motion search, mode decision, quantisation and slices,
and the methods; they do not say how each generation wants its input picture
laid out, how the chroma address is taken, or which of the many tuning fields
must not stay zero.

So the add-on knows several ways of handing the engine a picture (`variants`
in the interface; `kNvencLayouts` in `nvenc_common.c`): the block linear
arrangement NVDEC writes, which NVDEC-to-NVENC transcoding hands the encoder
unchanged and is therefore the most likely, the tiled 16x16 arrangement the
engine's own reference pictures must use, and the graphics block linear one,
each with the chroma plane addressed two ways. MediaConverter encodes a small
test picture with each until one decodes again (`NvencEncoder::SelfTest`), and
uses only that one; a format for which none works counts as one the card
cannot encode, and MediaConverter says so and encodes it on the CPU. Nothing is
encoded on a card before it has passed. The test runs in a process of its own
(the same executable with `--nvenc-self-test`, its debugger disabled), so an
add-on that crashes or a card that hangs costs the format, not MediaConverter:
a crash is reported at once, a hang after a minute and a half. The result is
kept in `~/config/settings/MediaConverter NVENC test` per card and add-on
build.

First run on a card (GTX 1070, GP104, class C2B7 on a Pascal channel, the
X399, 2026-10-18): the add-on finds the card and its class, makes its
channel and encoder object, and the engine takes the picture - it copies the
picture type into the status buffer - and then stops it: the channel's
error notifier says ROBUST_CHANNEL_NVENC0_ERROR (47), the status
`error_status` 1 with no microcode error code, for every input arrangement
and for H.264 and H.265 alike. The structures are not the cause: the 2021
`nvenc_drv.h` NVIDIA published for C2B7 and earlier (open-gpu-doc
97c510e793) lays them out byte for byte as the 2024 one Mesa carries. So
MediaConverter's test fails, the formats are encoded on the CPU and the
window says why. What the engine rejects is not known yet; a capture of
NVIDIA's own driver encoding on the card (its picture setup, found by the
magic 0xC2B70006) would show it. `NVENC_DEBUG=1` makes the add-on print the
notifier and status words of a failed picture. The card side - resman, the channels, the class list - is the
NVDEC add-on's, which works on the GTX 1080 Ti; the encoding is written from the
header alone. On a first run with a card, `mediaconvert --hardware` says for
each format whether it passed or what went wrong, and
`MEDIACONVERTER_NVENC_RETEST=1` runs the test again,
`MEDIACONVERTER_NVENC_VARIANT=n` tries one way only and the add-on prints why a
picture failed on standard error.

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
