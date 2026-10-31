# Displays on the Raspberry Pi 4

State 2026-10-07: both HDMI outputs are displays of one desktop, arranged
with the Screen preferences (or `screenmode`). The current lab configuration
detects both displays at 1920x1080 and uses a 3840x1080 desktop. HDMI0 is
visible through the KVM; HDMI1's physical picture has not been observed.
The earlier 640x480 forced-output tests below are historical.

## How it works

The firmware owns the compositor (HVS), output timing and PHY programming.
air/OS reads the connection signal and sends firmware KMS requests:

- `rpi_display` (`src/add-ons/kernel/drivers/graphics/rpi_display`) owns one
  frame buffer (contiguous, below 1 GB, write-combining) and asks the firmware
  for one plane per output with the "firmware KMS" property tag `SET_PLANE`
  (0x00048015): a source rectangle of the buffer, scaled to a destination
  rectangle on the output. No overlay in `config.txt` is needed for that.
- `rpi_display.accelerant` implements the fork's display layout hooks
  (`B_GET_DISPLAY_OUTPUTS`, `B_SET_DISPLAY_LAYOUT`, ...). app_server arranges
  the outputs; a layout becomes one frame buffer the size of the regions'
  bounding box, and the driver points each output's plane at its region.

Because the firmware scales, an output's *resolution* and its *scale* are both
just the size of its region: 1280x720 on a 1080p monitor, or 1920x1080 at
150 %, is a 1280x720 region shown full screen. A mirror shows its source's
region, letterboxed if the shapes differ. The kernel console follows the
frame buffer (`frame_buffer_update`); when app_server lets go, the planes are
removed and the firmware's own frame buffer (the boot console) shows again.

`config.txt` has `max_framebuffers=2` and `hdmi_force_hotplug:0=1` /
`hdmi_force_hotplug:1=1` to initialize both firmware pipelines even when a
monitor is absent at boot. Without that initialization, later detection and
layout changes succeed but the newly connected output does not produce a
picture. Real HPD, read by the driver, still controls the exposed desktop.

## Things to know about the firmware interface

- A plane names its display by the firmware's display **id** (2 = HDMI0,
  7 = HDMI1; `FRAMEBUFFER_GET_DISPLAY_ID`), not by the display number. With
  the number, the request is accepted and nothing shows.
- Plane ids count across the displays (three per display, as Linux's
  firmware-KMS driver has them). Two displays using plane id 0 ends with the
  second plane on the first display.
- The buffer address is a VideoCore bus address (physical | 0xc0000000).
- `GET_EDID_BLOCK_DISPLAY` also takes a firmware display **id**, not a
  framebuffer index. The 2026-10-07 candidate fixes this: index 1 had returned
  HDMI0's EDID, and the old duplicate-dropping workaround hid HDMI1's identity.
- The number of displays is what the firmware saw at boot; EDID responses can
  also remain cached after unplugging. Neither is a live connection signal.

## Hotplug

The driver retains two stable connectors (HDMI0/ID 2 and HDMI1/ID 7), independent
of boot-time enumeration. It reads BCM2711's `HDMI_HOTPLUG` bit every 250 ms,
requires two matching samples, and retries an unavailable EDID after two seconds.
The registers are read-only maps at `0xfef008a8` and `0xfef058a8`: the HDMI bases
from Linux's `bcm2711.dtsi`, plus the VC5 register offset `0x1a8`, bit 0.

At start-up and on connection, checksum-validated EDID picks the mode
(`edid_timing.h`, tests in `tools/rpi4/test-edid-timing.py`): the preferred
detailed timing when the Pi can send it, otherwise the fastest timing of the
same size (detailed timings or the CTA codes 4, 16, 31, 93 to 97), otherwise
the largest; an explicitly advertised VGA mode is the last resort. Limits:
the firmware's per-output pixel clock (`GET_DISPLAY_CFG`, `0x00040018`, Hz;
300 MHz when it does not answer), even horizontal timings, at most 85 Hz, as
in Linux's firmware KMS. The boot mode is never trusted: the size the firmware
reports (`GET_PHYSICAL_SIZE`) is its frame buffer's, which it scales to the
mode. A Dell P2415Q (4K, preferring 594 MHz 60 Hz) gave 1920x1280 there, and
the desktop filled only the top-left 1920x1280 of the 3840x2160 picture until
2026-10-31; now it gets 3840x2160 at 60 Hz. The image's `config.txt` sets
`hdmi_enable_4kp60=1`: the firmware then reports 600 MHz for HDMI0 but only
162 MHz for HDMI1 (1080p60 still fits, 1440p60 not); without it both report
300 MHz and the P2415Q runs at 30 Hz. The driver
uses `SET_TIMING` (`0x00048017`) and `SET_DISPLAY_POWER` (`0x00048019`) with the
firmware display ID. The ABI follows Raspberry Pi Linux's
[`vc4_firmware_kms.c`](https://github.com/raspberrypi/linux/blob/rpi-5.15.y/drivers/gpu/drm/vc4/vc4_firmware_kms.c).
On disconnection it removes that plane. It sends the existing
`B_SET_DISPLAY_CHANGE_PORT` notification so app_server rearranges displays,
resizes the desktop, and moves windows away from the removed screen. Queries
use a snapshot copied under the driver lock instead of racing EDID updates.

Native evidence on 2026-10-07: three HDMI0 HPD disconnect/reconnect cycles,
triggered by the NanoKVM capture control, shrink 3840x1080 to the remaining
1920x1080 output and restore both outputs with a visible KVM picture. A separate
read-only probe verified HPD falls even though EDID remains available.
A boot with HDMI0 absent was also tested: the firmware initializes it at
640x480, the driver removes it from the desktop while HPD is low, and a later
connection selects 1920x1080 from EDID. Fresh video frames must be captured:
the NanoKVM stream's first frame can be stale, including a cached black frame.
The physical HDMI1 reconnect and picture still need observation.

## Earlier checks on the board (2026-10-03)

| What | How | Result |
|---|---|---|
| Two outputs in app_server | `screenmode -d` | HDMI-1 1920x1080 (EDID name, size), HDMI-2 640x480; desktop 2560x1080 |
| Lower resolution | `screenmode --display 1 --display-mode 1280x720` | region 1280x720 shown 1920x1080 |
| Scale | `--display 1 --scale 150` | region 1280x720, UI enlarged (`evidence/layout-swap.jpg`) |
| Arrangement | `--position` on both | HDMI0 shows the right-hand part of the desktop |
| Mirror | `--display 1 --mirror 2` | HDMI0 shows the 640x480 desktop letterboxed (`evidence/layout-mirror.jpg`) |
| Layout kept over a restart | restart | the saved layout comes back |
| Screen preferences | `evidence/screen-prefs.jpg` | both displays, arrangement, mirror, per-display resolution and scale |
| OpenGL | GLTeapot | renders as before (`evidence/teapot-rpidisplay.jpg`) |

## The lab's HDMI1

During the 3 October tests, the firmware reported one display unless
`hdmi_force_hotplug:1=1` was added to
`config.txt`, and read no EDID from HDMI1. The Linux recovery system saw
the same ("Registered
framebuffer for display 0" only). The cause was not established. By the
6 October performance run both outputs supplied 1080p modes without that
forced-output setting. This establishes detection, not HDMI1 picture quality;
the Dell monitor still needs a physical check.

## Open

- Mode selection supports progressive EDID detailed timings and a few CTA
  codes; interlaced modes, standard timings and other CTA codes are not
  supported. Natively validated: the lab's 1080p monitors; the P2415Q at
  3840x2160, 30 and 60 Hz (2026-10-31, owner: sharp, whole screen; app_server
  chose 200 %).
- The accelerant's display timings are made up (always 60 Hz): `screenmode`
  and Screen do not show the real refresh rate; the driver's syslog line does.
- No user-selectable physical timing changes or DPMS.
- Hardware cursor (2026-10-27): the cursor bitmap goes into a small
  contiguous buffer of its own and the firmware composites it as a plane
  above the frame buffer (`CURSOR_PLANE_OFFSET`/`CURSOR_PLANES_PER_DISPLAY`
  in the driver, a layer above the desktop's). The app_server's software
  cursor — backup, draw and restore into the frame buffer — then never
  runs, which is what made the pointer flicker and leave stale patches
  over direct windows such as airTime's video. The cursor's plane, layer
  and image type can be re-pointed for experiments
  (`RPI_DISPLAY_CURSOR_CONFIG`).
- The retrace semaphore (2026-10-12, `docs/airos/RETRACE.md`) follows the
  HVS frame counter of the channel feeding the first output shown. To
  check it (`jam retrace_test`),
  `retrace_test blanks 5` should report the output's refresh rate with no
  timeouts, and the driver's `rpi display blanks` thread should exist once
  something has waited. The HVS counts a frame when it has composed it, a
  few lines ahead of the pixel valve, so the blank may be signalled
  slightly early.
- An output cannot be given more pixels than its mode (no shrinking).
