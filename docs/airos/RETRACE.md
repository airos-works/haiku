# Vertical blank (retrace) synchronization

Drawing that asks for `B_WAIT_FOR_RETRACE` reaches the screen at a vertical
blank, so video and animation do not tear. Since branch `retrace-sync`
(2026-10-12).

## What app_server does

app_server draws into a back buffer in memory and copies what changed to the
frame buffer the display scans out. A copy made while the scan passes through
it shows the top of one picture and the bottom of the next: a tear.

- `BView::DrawBitmap(..., B_WAIT_FOR_RETRACE)` draws into the back buffer
  first; the copy to the screen then waits for the next blank
  (`HWInterface::WaitForRetraceBeforeCopy()`, called from the drawing
  engine's `DrawTransaction`). The copy starts ahead of the scan and runs
  faster than it, so the picture stays whole.
- Drawn inside an update (`Draw()`), the wait comes before the update's copy
  in `Window::EndUpdate()` (`DrawingEngine::CopyToFront()`).
- Without a back buffer, the wait comes before the drawing.
- The waits use the accelerant's `B_ACCELERANT_RETRACE_SEMAPHORE`, the same
  semaphore `BScreen::WaitForRetrace()` gives programs. An accelerant without
  one is asked once; then nothing waits. A display that stops reporting
  blanks costs three 50 ms timeouts, then the waits stop for a second.
- The window thread waits with the window and the interface read locked:
  other windows go on drawing, but whoever needs the interface exclusively
  (a mode change) waits behind it, for at most a frame.

Only drawing that asks waits: MediaPlayer's video, airTime's copy through the
app_server, `retrace_test`. Other drawing is copied at once, as before.

Programs that write into the frame buffer themselves (`BDirectWindow`) wait
with `BScreen::WaitForRetrace()` before they write; airTime does since
`c6d8c0c` (its branch `retrace-sync`; `AIRTIME_NO_VSYNC` turns it off).

## Where the blanks come from

| Display | Driver | Source |
| --- | --- | --- |
| GTX 1070 (X399 until 2026-10-10) | `nvidia_rm` accelerant | NVKMS vblank semaphore page, a polling thread in the accelerant (since 2026-09-18) |
| Radeon Pro WX 5100 (X399) and other AMD GPUs | `amdgpu_kms` accelerant (amdgpu_drm) | DRM vertical blank waits on the primary output's CRTC, a thread in the accelerant (the native `amdgpu` driver's frame counter on the boot frame buffer, measured below, was removed with that driver on 2026-10-13) |
| Cubie A7S | `sunxi_display` | TCON-TV1's vertical blank interrupt (GIC SPI 58) |
| Raspberry Pi 4 | `rpi_display` | the HVS frame counter of the channel feeding the first output shown, read by a kernel thread |

Shared pieces in `headers/private/graphics/common/`:

- `retrace_semaphores.h`: the semaphores a driver releases at each blank.
  Userland may not acquire a semaphore the kernel owns, so each team that
  asks gets its own, owned by that team. `Release()` is interrupt safe, lets
  every waiter go and leaves nothing for later ones.
- `retrace_follower.h`: a thread that follows a hardware frame counter for
  drivers without a usable blank interrupt. It looks closely only when the
  counter is due, and releases only when it saw the advance within a
  millisecond, so a wait never ends in the middle of a frame. It sleeps when
  nobody has waited for two seconds.

Limits: one semaphore per accelerant, so with several monitors the blanks are
those of one of them (`nvidia_rm`: the primary head; `rpi_display`: the first
output shown). Windows on the others can still tear. No page flipping: the
copy is timed instead, which works while the copy outpaces the scan.

## Checking it

`retrace_test` (in `src/tests/servers/app/retrace`, in the Cubie lab image):

```
retrace_test blanks 5 [--line sunxi]
retrace_test draw 5 [--no-wait] [--update] [--size WxH] [--line ...]
retrace_test lines --line sunxi
```

`blanks` measures the semaphore's rhythm. `draw` draws a full bitmap a frame
and calls `Sync()`. With `--line`, both tell where the scan was when the wait
or the copy ended, as a fraction of the frame from the start of the blank:
copies that waited end early in the frame, ahead of the scan; without the
wait, they end anywhere. The frames alternate between two greys, so a
capture of the display shows a torn copy as both at once.

### WX 5100 on the X399, 2026-10-12

(With the native `amdgpu` driver and the `framebuffer` accelerant, both
since removed, and `--line amdgpu`, which went with them.)

The boot frame buffer is 1600x1200, shown on a 3840x2160 timing (2222
lines, 60 Hz).

| Test | Result |
| --- | --- |
| blanks | 300 in 5 s, 16.667 ms apart (60.00 Hz), deviation 0.115 ms, no timeouts; every wait ended within 3 % of a frame after the blank began |
| draw 600x400, wait | 60.0 a second; copies ended at 3.4 % (median) to 5.4 % (max) of the frame, before the scan reached the window |
| draw 600x400, no wait | 1240 a second; copies ended evenly across the whole frame |
| draw 780x540, wait / no wait | 60.1 a second, by 8.1 % / 544 a second, anywhere |
| draw --update, wait / no wait | by 4.9 % (one of 329 at 21 %) / anywhere |

The `amdgpu blanks` thread used 0.24 s of processor time in about four
minutes, mostly while the tests waited.

### Cubie A7S, 2026-10-12

DisplayPort 1920x1080 at 60 Hz (1125 lines); TCON-TV1 counts lines from the
start of the vertical sync, the blank starts at line 1121.

| Test | Result |
| --- | --- |
| blanks | 300 in 5 s, 16.667 ms apart, deviation 0.003 ms (an interrupt, not polling), no timeouts; waits end 6 to 14 lines into the blank |
| draw 640x480, wait / no wait | 60.0 a second, copies ended by 11.7 % of the frame (about 4 times the scan's speed) / 504 a second, anywhere |
| draw 1600x900, wait / no wait | 59.9 a second, by 27.8 % (3 times the scan's speed) / 150 a second, anywhere |
| draw --update, wait | 60.3 a second, by 11.3 % (one of 302 at 64 %) |

The NanoKVM's capture of the DisplayPort output (`retrace/kvm-tear.py`
in the lab: frames alternating between two greys, a capture showing both is
torn):

| What alternates | Torn captures |
| --- | --- |
| retrace_test 1600x900, B_WAIT_FOR_RETRACE | 0 of 437 |
| the same without | 413 of 430 |
| airTime full screen, 1080p30, retrace waits | 2 of 446 (one event: its render threads ran late once) |
| the same with AIRTIME_NO_VSYNC / airTime 1.0.0-18 | 337 of 445 / 312 of 445 |
| MediaPlayer, 360p30 | 0 of 495 |

Frames dropped, 10 s of 1080p30 H.264 (300 frames), runs interleaved; the
board idles near 78 C and was throttled (level 5 to 8 of 8) throughout, so
these are not full-speed numbers: MediaPlayer in a window 164 / 318 / 194
without the wait, 166 / 265 / 193 with it; airTime 0 and 0 either way. The
wait costs a frame's drawing up to one refresh: at 30 frames a second on a
60 Hz display that fits while drawing and copying take less than about
30 ms a frame, and app_server's scaled 1080p drawing on the Cubie is close
to that.
