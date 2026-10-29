# The Allwinner A733's video encoder (Radxa Cubie A7S)

H.264 and H.265 encoding on the A733's video engine: the `sunxi_ve` driver
(`src/add-ons/kernel/drivers/misc/sunxi_ve.cpp`, `SUNXI_VE_ENCODE`), the
encoder (`src/add-ons/media/encoders/sunxi_venc/CedarEncoder.cpp`) and
MediaConverter's `CedarVideoEncoder`, which lists the A733 as a hardware
encoder for H.264 and H.265 once its self-test pictures decode again.

Nothing documents this engine. The program below is what Allwinner's own
encoder library (the Radxa BSP's `libvenc_*`) writes, captured by running it
under qemu-user with a stand-in for its kernel driver, then confirmed on the
board picture by picture against libavcodec.

## The engine

| What | Value |
|---|---|
| registers | 0x01c10000, 4 KiB (the BSP's `ve2`, not in our device tree) |
| interrupt | GIC SPI 48 (INTID 80) |
| power | PCK-600 domain 3 (on at boot); CCU 0xa8c b16 reset, b0 bus gate; 0xa80 = 0x83000000 (600 MHz from pll-peri0-600m); AHB 0x5c0 b1, MBUS 0x5e0 b12 and 0x5e4 b1 (keyed, also set by the decoder's writes) |
| IOMMU | IOMMU0 master 3; IOMMU0 does not translate on air/OS (0x03900014 = 0) |
| addresses | bus addresses (= physical on the A733): the encoder's buffers >> 8, the input planes in bytes. Allwinner's library subtracts 0x40000000 from its IOMMU addresses; physical addresses must not be. |

Only these parts of the window are touched: the top (+0x800), the input stage
(+0xa00), the encoder (+0xb00) and its extension (+0xc00). Reading the whole
window hangs the SoC. The input stage and encoder answer 0x5555aaaa until the
top's mode (+0x800) enables them; Allwinner's library goes 0x00030000,
0x00130000, 0x00130040 (input stage), 0x001300c0 (and encoder) before each
picture and back to 0x00130080, 0x00130000 after it. +0x804 b0|b4 are pulsed
after the engine's reset. +0x8e4 is the encoder's IP version (0xe21320).

## A picture

The driver turns the blocks on, sets the codec (0xb18 = 0x30000 for H.265, 0
for H.264: the bit writer is not ready right after the codec changes), runs
the program's writes, puts the slice header through the bit writer, starts the
picture and waits.

- Bit writer: 0xb04 b31 set (no emulation prevention bytes), then a byte at a
  time: 0xb20 = byte, 0xb18 = mode | 8 << 8 | 1, wait for 0xb1c b9. The header
  is whole bytes with its emulation prevention bytes: H.264 ends with the
  CABAC alignment ones, H.265 with byte_alignment(). 0xb04 back without b31.
- Start: 0xb14 = 7 (interrupts), 0xb18 = mode | 8.
- End: 0xb1c [3:0] (W1C): b0 done, b1 bit stream buffer full, b2 overtime.
  0xb90 = the slice's size in bits from the bit stream buffer's start, header
  included. 0xb88 is the start offset (0).

### Registers (all set for every picture)

Input stage (NV12): 0xa00 = (width / 8) << 16 | height / 8 (the picture's
size rounded to 8, not to 16: the H.265 CTUs follow it), 0xa04 = luma
stride / 16 << 16, 0xa14 = chroma stride / 16, 0xa2c = height / 8 << 16 |
width / 8, 0xa08 = 0x20021 (H.264 IDR), 0x21 (H.264 P), 0 (H.265), 0xa78 /
0xa7c luma / chroma, 0xa80 chroma + stride * height / 4, 0xa70, 0xa74, 0xa84,
0xa88 = 0. H.264 also: 0xa90 = 0x03ff03ff, 0xa94 = 0, 0xa98 = min(width * 3 /
40, 128) << 16, 0xa9c = a statistics buffer.

Encoder, both codecs:

| reg | value |
|---|---|
| 0xb30 0xb34 0xb38 0xb74 | texture thresholds: I 0a080604 12100e0c 1a181614 22201e1c, P 1d1c1b1a 21201f1e 25242322 29282726 |
| 0xc10..0xc20 | 01020304 05060708 08070605 04030201 00003300 (also the reset values) |
| 0xb28 | 0 |
| 0xb88, 0xb80, 0xb84, 0xb8c | 0, bit stream start >> 8, (end - 1) >> 8, size in bits |
| 0xba0 0xba4 / 0xbb0 0xbb4 | reference / reconstructed luma and chroma (>> 8); IDR: reference = reconstructed; then the two alternate |
| 0xbb8 / 0xbbc | the same pictures' subsampled copies |
| 0xb9c | a row table (512 + 12 per row; zeros are fine without the line rate control) |
| 0xbc0 0xbc4 | two line buffers (width * 8) |

H.264: 0xb04 = 0x100 (I) / 0x510 (P: b4 P, b8 CABAC, b10), 0xb08 = QP |
ceil(mb width / 40) << 10 | b8 (dynamic motion search, P pictures from 13
macroblocks wide only: narrower P pictures never end), 0xb10 = 0x08001200,
0xb2c = 0 (b31 = per macroblock QP from the texture; Allwinner sets it, it
costs 1.5 dB at the same size), 0xb40 = 0x74000014, 0xb44 = 0x0c1e0000,
0xb48 = (mbw * 3 / 5) << 16 | mbw / 10, 0xb4c = (mbw * 16 / 5) << 16 |
mbw * 13 / 5, 0xb70 = 0 (I) / 0xff000000 (P), 0xb0c = 0xb24 = 0, 0xb60 = a
macroblock info buffer.

H.265: 0xb04 = 0x100 / 0x110, 0xb08 = QP | 0x1c << 16 (chroma QP offset -4,
as the PPS) | width class << 10 | b8 (P), 0xb10 = 0x08c00000 (IDR),
0x08400000 (the P picture after it), 0x08000000 (later P pictures; b22: the
collocated picture is intra, so no temporal motion vector candidates: a wrong
b22 drifts from the second P picture on), 0xc00 = 0x300 (b8/b9: SAO luma and
chroma, as the slice header's flags), 0xb2c = 0x10000000 (b28: the engine
writes cu_qp_delta, which the PPS allows; without it the stream does not
parse), 0xb40 = 0x18000014, 0xb44 = 0x0c1e0084, 0xb48 / 0xb4c as H.264's in
CTB columns (0 for IDR), the rate-distortion lambdas 0xb68 = 0xc84 (735 *
2^((QP - 28) / 3) for I, 3230 * 2^((QP - 30) / 3) for P), 0xb70 = 0xff000000 |
0.397 * that, 0xb6c = 0x3cc80000 | r, 0xb94 = r (r = 0x20 for I, 2 * sqrt(lambda)
for P), the extension's fixed tables (0xc04..0xc80, 0xce4..0xcf4, see
`CedarEncoder::_Program`), 0xbcc..0xbfc, 0xcc4, 0xcf8, 0xcfc = 0, and the
buffers 0xb60 / 0xb64 (motion vectors in / out, alternating like the
pictures), 0xba8 (block info), 0xb3c (collocated data, P only, else 0).

Buffer sizes: `sunxi_ve_get_encoder_sizes()` in `headers/private/drivers/
sunxi_ve.h` (Allwinner's sizes with room to spare); the driver refuses smaller
buffers. The reference pictures are the engine's own compressed format.

### The bit stream

The engine fixes the coding tools; the parameter sets must say the same:

- H.264: High profile, CABAC, transform_8x8_mode_flag 1, chroma QP offset 0;
  P slices use cabac_init_idc 1. pic_order_cnt_type 2, one reference picture.
- H.265: Main; CTB 32, CB 8, TB 4 to 32 with depth 1 (intra and inter), SAO,
  temporal MVP, strong intra smoothing, no AMP, no PCM, no scaling lists;
  PPS: cu_qp_delta_enabled with depth 1, cb/cr QP offsets -4,
  cabac_init_present (P slices: cabac_init_flag 0), loop filter across
  slices, deblocking control present without override. One short-term
  reference picture set {-1}; P slices set slice_temporal_mvp_enabled_flag
  and five_minus_max_num_merge_cand 0.

## Traps

- The buffers `sunxi_ve` hands out are cached. A CPU memset that is not
  cleaned with SUNXI_VE_SYNC_FOR_DEVICE is written back later, over the
  engine's reference pictures: random drift that looked like "the first GOP
  is bad".
- P pictures smaller than 176x64 (H.265 from 176 wide and 2 CTB rows, H.264
  from 160 wide) never end; the driver resets the engine after a second.
  CedarEncoder refuses them (MediaConverter then encodes on the CPU).
- A non-packaged `sunxi_ve` in `system/non-packaged/add-ons/kernel/drivers/
  misc` takes over at the next boot (`listimage 1` shows the path).

## Numbers (Cubie A7S, engine time a picture)

| | 720p | 1080p | 2160p |
|---|---|---|---|
| H.264 | 3.1 ms | 7.0 ms | 29 ms |
| H.265 | 2.5 ms | 5.4 ms | 22 ms |

MediaConverter converts a 60 s 1080p H.264 clip to H.265 at 97 fps (3.2 times
real time, 42.5 dB) where x265 on the CPU manages 5 fps.

## Lab tools

Not in the tree (they poke registers directly): `vproto` (the whole program
in user space through /dev/misc/poke), `vcheck` (decode with libavcodec and
compare with the source), `vsrc` (a file's pictures to NV12) and the qemu
tracing harness (stand-in kernel driver, MMIO logger, trace analyzer) live in
the lab's evidence directory for the encoder. `src/add-ons/media/encoders/
sunxi_venc/test/cedar_venc.cpp` encodes test pictures or NV12 files with
CedarEncoder.
