# Media on the Raspberry Pi 4

The installed 2026-10-06 build passes all 25 HEVC reference streams and
the 90-frame H.264 reference in both input modes. SAND conversion now works
in cache-sized row groups. The final airTime samples show 1080p HEVC at
30 FPS without drops, 4K8 at 30 FPS over 20.4 seconds, and H.264 at 29.6 FPS
with nine drops over 20.3 seconds. Raw 4K10 plane making stays below real
time at full size (36 ms), so the add-on now also makes half size pictures
(a 2x2 box) when the player asks for exactly that: a 4K film on a 1080p
screen plays in real time in airTime this way, 26 ms a picture.
See [PERFORMANCE.md](PERFORMANCE.md) for exact fixtures, intervals and
endurance results. Physical audio remains unverified, and repeated HEVC
allocations can fail under memory fragmentation despite free RAM.

Bring-up state, 2026-10-04 (evening): sound output, hardware H.264 decoding (the
VideoCore firmware's decoder) and hardware HEVC decoding (the SoC's own HEVC
block) work. airTime plays 1080p30 in either at 30 pictures a second with
sound. Nobody has listened to the sound: the lab has no ears (see "How the
sound was checked").

| Piece | What it is | Where |
|---|---|---|
| `vchiq` | the message channel to the firmware's services, a kernel module | `src/add-ons/kernel/generic/vchiq` |
| `/dev/misc/vchiq` | the same services for user programs | `src/add-ons/kernel/drivers/misc/vchiq.cpp` |
| `bcm2835_audio` | sound driver (multi audio): HDMI 0, HDMI 1, headphone jack | `src/add-ons/kernel/drivers/audio/bcm2835` |
| `rpi_mmal` | Media Kit decoder add-on: H.264 on the firmware's decoder | `src/add-ons/media/plugins/rpi_mmal` |
| `/dev/misc/rpi_hevc` | the SoC's HEVC decoder block for a program that parses the stream | `src/add-ons/kernel/drivers/misc/rpi_hevc.cpp` |
| `rpi_hevc` | Media Kit decoder add-on: HEVC Main and Main 10 on that block | `src/add-ons/media/plugins/rpi_hevc` |
| airTime 1.0.0-14 | knows both add-ons | `/mnt/HaikuWork/apps/airTime` |

All of it is in the `rpi4-airos` image; `config.txt` gives the firmware
`gpu_mem=192` for the H.264 decoder.

## VCHIQ

The firmware's services (sound, the multimedia components, and more) are
reached through VCHIQ: slots of shared memory that each side fills with
messages, a doorbell register, and bulk transfers where the firmware copies
from or to ARM memory itself. `vchiq.cpp` is a new implementation of the ARM
side of Broadcom's protocol (their implementation is BSD licensed and was the
reference). It leaves out what nothing here needs: services opened by the
firmware, synchronous services, suspend, message quotas.

- The module finds the `brcm,bcm2711-vchiq` node in the device tree itself;
  on another board it loads and reports `B_DEVICE_NOT_FOUND`, and the two
  drivers on top of it publish nothing.
- Kernel API in `headers/private/drivers/vchiq.h`: open a service with a
  hook that gets its messages, queue a message, bulk transmit and receive.
- `/dev/misc/vchiq` has the same as ioctls; a service is closed with the file
  descriptor that opened it, so a program that dies leaves nothing open.
- Bulk transfers go through a buffer per service and direction, with CPU
  cache maintenance around firmware access. The page-list allocation stays
  below 1 GB and is retained by the service. On BCM2711, data can use
  noncontiguous pages described by the 36-bit page-list format; it no longer
  needs a contiguous multi-megabyte run below 1 GB. Copies remain.

Two things cost time and are worth knowing:

- On the BCM2711 the page list of a bulk transfer holds 36 bit physical
  addresses shifted right by four, with the run length in eight bits. The
  form of the older boards (bus address, twelve bits of run length) is what
  mainline Linux has; the firmware cancels every transfer written that way.
- The firmware cancels a bulk transfer whose size is not what it expects.
  For the decoder's input that is the length rounded up to four bytes.

## Sound

`bcm2835_audio` hands the firmware's sound service ("AUDS") 16 bit stereo at
44.1 or 48 kHz in buffers of 960 frames. The firmware does the rest: the
audio packets of the HDMI outputs, the PWM of the headphone jack. It reports
what it has played in pieces of about 10 ms; the driver sends the next buffer
when the firmware is down to one, and that pace is the Media Kit's clock.
Mixer controls: volume (0 to -60 dB) and output (automatic, headphone jack,
HDMI 0, HDMI 1); "automatic" is the firmware's choice.

### How the sound was checked

Nobody can listen in the lab, so this is what stands in for ears:

| Check | Result |
|---|---|
| The Media Kit's default output | "Raspberry Pi sound output" |
| `rpi4_tone` (a BSoundPlayer tone through the system mixer), several runs of 3 to 12 s | the mixer asks for 48000 frames per second; the driver's own count is 48000 frames in 1.000 s, second after second, with no underrun after the start |
| `rpi4_vchiq_audio <destination> 2` (a tone straight to the service) for the jack, HDMI 0 and HDMI 1 | the firmware takes 48000 frames per second on each |
| the lab KVM's HDMI receiver on HDMI 0 (its driver logs an interrupt when the audio of the signal changes) | logs when the sound service is pointed at HDMI 0 and again when it is closed or pointed at the jack; nothing while the jack plays. Through the driver, which keeps the service open, that is at each change of the "Output" control |
| `rpi4_tone controls` | volume and output can be read and set through the Media Kit |
| airTime, a film with AAC sound | audio and video stay within about 20 ms of each other (`av=` in its Stats) |

So the samples reach the firmware at the right rate, and HDMI 0 carries an
audio stream when the output is HDMI 0 and none when it is the jack. What it
sounds like, and whether anything comes out of the jack or HDMI 1, is not
checked. (The monitor on HDMI 1 is detected since 2026-10-04, with its EDID;
whether it has speakers is not known here.)

## H.264: the firmware's decoder

The firmware's H.264 decoder is the component `ril.video_decode` of its
"mmal" service. `MmalDecoder` speaks that service's messages over
`/dev/misc/vchiq`: Annex B byte stream in, in pieces of at most 80 KiB;
pictures out in display order as I420 with the time stamps that went in.
`rpi_mmal` wraps it as a decoder add-on of the Media Kit.

- H.264 up to 1920x1088, eight bit 4:2:0 (what the firmware decodes).
- The add-on registers no format: the lookup would give it every H.264
  stream and has no fallback when it refuses one. A player loads it by name;
  airTime does, and goes to libavcodec when it refuses.
- Pictures are handed out as 'I420' (three planes) or 'NV12', both colour
  spaces Haiku has no constant for; see the top of `RpiMmalPlugin.cpp`.
- At most two decoders at a time in the whole system (a named port per
  decoder is the token). The firmware's memory is `gpu_mem`; when it runs
  out the firmware's mmal service stops answering until the next start. That
  happened with the default 76 MB and two 1080p decoders. With 192 MB four
  ran side by side.
- Seeking: a negative `time_to_decode` in `Decode()` names the time before
  which the caller drops the pictures. Access units before it are marked
  "decode only" (the firmware gives no picture for them, which doubles its
  speed) and pictures nothing refers to are not sent at all.
- The end of the stream is sent when nothing has come out for 300 ms. Sent
  right behind the last access unit it overtakes the pictures still being
  decoded and they never come out (3 of 90 when fed fast).

### Checked on the board

| Check | Result |
|---|---|
| `rpi4_mmal_decode 1920 1080 test90.h264 - \| md5sum` (90 pictures of 1080p High profile, cut from `multi.mkv`), also with `RPI4_MMAL_FRAMED=1` (whole access units, as a demuxer hands them over) | the same md5 as `ffmpeg -i test90.h264 -pix_fmt yuv420p -f rawvideo -` on the build host: every picture byte for byte |
| the same without writing the pictures | 60 to 70 pictures per second |
| the same with `RPI4_MMAL_DECODEONLY=1` (no pictures out) | about 120 per second: handing a picture over costs the firmware as much as decoding it |
| two decoders at once | both give 90 pictures |
| eight decoders killed (`kill -9`) in mid-stream, then one more | byte exact again: the firmware frees what a dead client had |
| airTime, `multi.mkv` (H.264 1080p30 High, AAC) | "Raspberry Pi VideoCore (rpi_mmal h264) [hardware]", the picture is right (`evidence/airtime-hw1.jpg`, after a seek `evidence/airtime-h264-seek.jpg`), sound in step; seeking, playing to the end and seeking back from there work |

## HEVC: the SoC's decoder block

The BCM2711 has a decoder block for HEVC that the firmware does not drive.
It does the slice data (CABAC) and makes the pictures, in two phases with an
interrupt each; everything a decoder does around that is the caller's.
Linux has a stateless V4L2 driver for it (`rpivid`), which was the only
description of the block's registers; nothing of it is in the kernel here.

- `/dev/misc/rpi_hevc` (`<rpi_hevc.h>`) hands out physically contiguous
  buffers (areas the caller clones), runs a phase for registers and buffers
  the caller names and waits for its interrupt, and keeps the CPU cache out
  of the way. It turns the block's clock on with the first open. It knows
  nothing of HEVC, and checks only that the buffers named are the caller's.
  Two programs can use the block at once (a phase is the unit).
- `HevcParser`: parameter sets and slice segment headers.
- `HevcDecoder`: picture order counts, reference picture sets and lists,
  the order pictures go out in, and the list of register writes that takes
  the block through a picture's slices, tiles and wavefront rows.
- `SandConvert`: the block's pictures are columns 128 bytes wide, each with
  its luma rows and then its chroma rows; ten bit samples are three to a
  32 bit word. This makes planes of them (NEON).
- `rpi_hevc`, the add-on: 'I420' or 'NV12' for eight bits, 'P010' for ten.
  Like `rpi_mmal` it registers no format. Pictures before a seek's target
  are not copied, and those nothing refers to are not decoded.

What it decodes: Main and Main 10 (4:2:0, eight or ten bits) up to
4096x4096. Not: range extensions, more than one layer, tiles together with
wavefronts.

### Checked on the board

`rpi4_hevc_decode film.hevc - | md5sum` against
`ffmpeg -i film.hevc -pix_fmt yuv420p -f rawvideo - | md5sum` (`p010le` for
ten bits). The streams are x265's, made from `multi.mkv`
(`/mnt/HaikuWork/rpi4/media/hevc`, with the md5 of each):

| Streams | Result |
|---|---|
| intra only; P only; B pictures; wavefront; 1080p with and without wavefront; 1080p ten bit; four slices; weighted prediction; scaling lists; coding tree blocks of 16 and 32; six references with a B pyramid; open GOP (leading pictures); 4K eight and ten bit; odd sizes (1000x562, eight and ten bit; 176x144); wavefront with slices, ten bit; no SAO, no deblocking; constrained intra prediction with transform skip; lossless; CU lossless with rectangular and asymmetric partitions; no temporal motion vectors (25 streams) | every picture byte for byte, all 25 |
| speed, pictures not written: 1080p | 115 per second with the planes made (3.4 ms a picture for that), 70 for ten bit (8.5 ms) |
| speed: 4K | 28 per second, 16 for ten bit: making the planes is most of it (16 and 38 ms) |
| speed: 4K ten bit at half size, asked for exactly that | 26 ms a picture, under the full size's 36 ms; a quarter of the pixels for the player's scaler and tone mapper |
| two decoders at once (1080p eight and ten bit) | both exact |
| six decoders killed in mid-picture, then one more | exact; no buffer left behind |
| a stream cut off, a stream with 30 KB zeroed | ends without the missing or damaged pictures; "phase 1 failed", "refers to ..., which is missing" with `RPI_HEVC_TRACE=1` |
| airTime, `hevc8.mp4` and `hevc10.mkv` (1080p30) | "Raspberry Pi HEVC decoder (rpi_hevc) [hardware]", the pictures are right (`evidence/airtime-hevc8-hw.jpg`, `evidence/airtime-hevc10-hw.jpg`), seeking and playing to the end work |
| airTime, a 4K HDR film (3840x1920 Main 10) on a 1080p screen | half size P010 pictures, right colours, 25 s of film in 25 s of wall clock |

Not tried: tiles (x265 makes none; the register writes for them follow the
Linux driver), long-term references, more than one sub-layer, a stream that
changes its picture size (the add-on stops with an error there).

## What it buys

airTime on a 1080p30 film in a window of 1486x836, with sound, measured over
20 to 30 s of steady playing (`get Stats`):

| | pictures shown per second | left out | time per picture: decoding / making and drawing it |
|---|---|---|---|
| H.264, firmware decoder | 29.7 to 29.9 | 5 to 11 in 30 s | 3 to 5 ms / 12 ms |
| H.264, libavcodec | 30.0 | none | 0.2 ms (four threads beside) / 10 ms |
| HEVC, decoder block | 30.0 | none | 7 ms / 7 to 10 ms (ten bit 14 / 15 ms) |
| HEVC, libavcodec | 19 to 25 | many | 43 to 50 ms / 13 ms |

Decoding H.264 on the firmware takes about a quarter of a core where
libavcodec takes four threads at 60 % of a core together.

### Why the firmware decoder showed fewer pictures than software

It showed 27 to 29 pictures a second where libavcodec showed 29.8. Four
things were in the way; the first three are gone:

1. The Wi-Fi driver's bus thread used 80 % of a core for nothing (its
   interrupt fired for ever, see `WIFI.md`).
2. The scheduler got the work of three cores out of four busy threads, and
   sent threads woken together to the same core
   (`kernel/scheduler: use the idle cores`). Both decoders and airTime's
   scaler run four threads.
3. airTime made each picture in a bitmap and copied that to the frame
   buffer: 23 MB of memory moved per 1080p picture, on a board that moves
   about 2 GB a second. It now makes the picture in the frame buffer when
   it can (8 MB), with a scaler of its own.
4. **The firmware's decoder slows the ARM's memory down while it works.**
   `rpi4_memprobe` (copies 1 MB over and over) next to `rpi4_mmal_decode` at 30
   pictures a second: copies that take 0.46 ms on the idle board take more
   than three times as long in bursts of 30 to 100 ms, several times a
   second, also when the decoder hands no pictures over (so it is the
   decoding, not the copying). Arithmetic alone is not slowed, and nothing
   on the ARM side reproduces it (the same copies and cache flushes without
   the firmware cost 10 to 20 %). The HEVC block, flat out, does not do it.
   Pictures that take 12 ms to make then take 30 to 70 ms, and a picture is
   late. airTime now catches up after such a hold-up instead of leaving a
   picture out; what remains is the 5 to 11 in 30 s above.

### Seeking

A seek to a time 246 pictures after the key frame before it (H.264,
`multi.mkv`, to 8.2 s), from the request to the first picture:

| | first picture after | then |
|---|---|---|
| firmware decoder, before | 4.5 s | sound and picture apart by 1 to 3 s for seven seconds and more, pictures left out all the while |
| firmware decoder, now | 1.4 to 1.7 s | together at once |
| libavcodec | 4.1 s | |

The decoder is told to make no pictures before the target (twice as fast),
is not given the pictures nothing refers to, and airTime asks a decoder
that is behind the clock for no pictures until where the clock will be.
HEVC on the decoder block: 0.2 s.

## Lab tools

In the lab image's `~/config/non-packaged/bin`:

- `rpi4_vchiq_audio <destination> <seconds> [Hz] [dB]`: a tone straight to
  the firmware's sound service.
- `rpi4_tone <seconds> [Hz]`: a tone through the Media Kit;
  `rpi4_tone controls [id value]` lists and sets the sound driver's mixer
  controls.
- `rpi4_mmal_decode <width> <height> <file.h264> [output | -]`: the
  firmware's decoder on an Annex B stream. `RPI4_MMAL_FRAMED=1` feeds whole
  access units, `RPI4_MMAL_DECODEONLY=1` asks for no pictures,
  `RPI4_MMAL_REPEAT=n` and `RPI4_MMAL_PACE=ms` are for measuring.
- `rpi4_hevc_decode <file.hevc> [output | -]`: the HEVC block on an Annex B
  stream; `RPI4_HEVC_LIST=1` lists the pictures, `RPI_HEVC_TRACE=1` says
  what the decoder does.

- `rpi4_memprobe <seconds>`: says when copying memory is slow.
  `rpi4_cores`: what one to four threads get done, left to the scheduler
  and pinned.
- On the build host, `tools/rpi4/airtime-rate.sh "<environment>" <file>`
  gives airTime's pictures shown per second over 20 s of steady playing.

A kernel driver is replaced with `tools/rpi4/install-driver.sh` (it
restarts the board). Swapping the sound driver's file under the running
media server ended in a panic.

## Open

- **Nobody has heard it.** Nothing is known about the jack and HDMI 1
  beyond the firmware taking the samples; choosing the output in the Media
  preferences by hand is untried (the controls answer to
  `rpi4_tone controls`).
- **The firmware decoder and memory.** See "Why the firmware decoder showed
  fewer pictures": the bursts are the firmware's and stay. A video plane of
  the firmware's compositor (which `rpi_display` already uses for the
  desktop) would take the decoders' pictures as they are, and the ARM would
  move no picture at all; that is the way out, and a larger piece of work
  (overlays in the app_server).
- **H.264 copies.** A picture is copied twice on its way (the firmware's
  bulk transfer into a bounce buffer, that into the add-on's buffer, that
  into the player's). Buffers shared with the firmware (its "vcsm" service
  and `MMAL_PARAMETER_ZERO_COPY`) would remove two of the three; it was
  looked into and left, because the hold-ups come from the decoding.
- **HEVC in 4K.** The block decodes 4K (28 pictures a second through the
  test tool), but making planes of a picture and then pixels on the CPU is
  too slow for 4K at full rate; the same video plane would solve it (the
  firmware takes the block's column format as it is).
- **HEVC untried:** tiles, long-term references, sub-layers, a change of
  picture size in mid-stream.
- **Other players.** MediaPlayer and the Media Kit's own lookup use neither
  hardware decoder (see above). Until 2026-10-04 (night) they got no H.264
  decoder at all: `rock5_ffmpeg`, which the image installs for FFmpeg, also
  carries the ROCK 5's `00_rockchip_mpp`. That add-on sorts first, the Media
  Kit picks it for H.264, its setup fails without the RK3588's decoder, and
  `MediaExtractor` does not try the next decoder (seen by the Summit session
  with Summit's `<video>`). The image now hides the add-on with a packagefs
  block (`data/boot/rpi/packages`), so FFmpeg decodes H.264 in software
  there. Built into the image (sha256 8f8a158e…, the file checked inside
  it) but not yet flashed or played on the board. A Media Kit that tries the
  next decoder when one fails, or an MPP add-on that offers nothing without
  its hardware, would fix it for every board.
- **airTime's changes on the other boards.** The scaler for eight-bit
  pictures, drawing straight into the frame buffer and the new rule for late
  pictures are in airTime for every board; they were only run on the Pi.
