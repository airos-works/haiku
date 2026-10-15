# Vertical blank (retrace) synchronization

Drawing that asks for `B_WAIT_FOR_RETRACE` reaches the screen at a vertical
blank, so video and animation do not tear. Since branch `retrace-sync`
(2026-10-12).

## What app_server does

app_server draws into a back buffer in memory and copies what changed to the
frame buffer the display scans out. A copy made while the scan passes through
it shows the top of one picture and the bottom of the next: a tear.

Since branch `vblank-flush` (2026-10-15) every copy to the screen is timed:

- Drawing, window moves and scrolling only mark what changed. The copies to
  the frame buffer are made by a *flush thread* once a frame, right after
  the display's vertical blank (`HWInterface::_FlushThread()`). The copy
  starts in the blank, goes from the top down in the order a `BRegion`
  keeps its rectangles, and runs faster than the scan, so the display never
  shows a half-copied picture. The thread holds the interface read locked
  while it copies, as a drawing window would.
- A large flush (over about 4 million pixels, a 4K display's half) is cut
  into bands of rows, one for the flush thread and one per worker thread (up
  to three, fewer with few processors): with N bands each has N times the
  scan's time for its rows, and all stay ahead of it while the whole copy
  takes less than a frame. On the X399 one core copies about 4 GB/s into
  the WX 5100's frame buffer, so a flush of its two 4K displays alone would
  take the whole frame.
- Each display output of a layout gets its own flush target (`FlushTarget`:
  the part of the frame buffer it shows, its own blanks, thread, pending
  region and workers), because the monitors scan at their own phases. The
  accelerant tells them apart with `B_GET_DISPLAY_OUTPUT_RETRACE_SEMAPHORE`
  (a semaphore per output; `amdgpu_kms` has it). Without it, one target for
  the whole frame buffer uses `B_ACCELERANT_RETRACE_SEMAPHORE`. Mirrors are
  left out: they show another output's part, which is timed to that one.
- A display that stops reporting blanks costs three 50 ms timeouts, then
  the copies are made unsynchronized for a second. An accelerant with no
  retrace semaphore at all gets the old behaviour: copies as drawn.
- The threads count copies that ended up behind the scan and report once
  a minute to the syslog. Where the accelerant tells where the scan is
  (`B_GET_DISPLAY_OUTPUT_SCAN_LINE`; `amdgpu_kms` computes it from the
  DRM vblank timestamp, which amdgpu takes from the scan position, and the
  mode's line time), each copy is checked against the hardware's scan
  line at its start and end; elsewhere the scan is estimated from the time
  since the blank over the output's rows, with the blank's own lines as
  the margin:
  `app_server: vertical blank flush DP-1: N flushes, N rects, N behind the
  scan (worst 0.00 frames, scan read from the hardware), longest 9.2 ms,
  period 16.666 ms, 4 bands`. A copy behind the scan at its start or its
  end is exactly what a capture of the display shows as a tear (the Cubie
  measurements below checked that criterion against its capture).
- `B_WAIT_FOR_RETRACE` drawing needs no wait of its own any more: it is
  copied at the next blank like everything else. It no longer paces the
  drawing program at the refresh rate (`retrace_test draw` reports hundreds
  of frames a second; the display shows sixty).

Before (branch `retrace-sync`, 2026-10-12), only drawing that asked for
`B_WAIT_FOR_RETRACE` was timed, in the drawing thread
(`HWInterface::WaitForRetraceBeforeCopy()`, still there for the unflushed
case); other drawing was copied at once.

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

Limits: only `amdgpu_kms` has the per-output semaphores, so with several
monitors on the others the blanks are those of one of them (`nvidia_rm`: the
primary head; `rpi_display`: the first output shown) and windows on the rest
can still tear. No page flipping: the copy is timed instead, which works
while the copy outpaces the scan.

### WX 5100 on the X399, 2026-10-15 (the flush)

One DELL P2415Q on DP-1, 3840x2160 at 60 Hz, the desktop at 200 %. Five
targets (the screen and the four DisplayPort connectors), DP-1 at its own
blanks. From the syslog during `retrace_test draw 50 --size 3840x1080` and
`move 50 --size 1600x900`:

| Minute | Flushes | Rectangles | Behind the scan | Longest flush | Bands |
| --- | --- | --- | --- | --- | --- |
| drawing full screen | 3499 | 6950 | 0 | 6.6 ms | 2 |
| moving a 3200x1800 px window | 3569 | 11156 | 0 | 5.8 ms | 2 |
| idle desktop | 241 | 540 | 0 | 0.0 ms | 1 |

The drawing program ran at 123 to 142 full-screen frames a second (it no
longer waits), the window moved 115 to 144 times a second; the display
showed 60 of each. An earlier single-threaded build copied a 4800x2000 px
window in 9.2 ms.

With the scan line read from the hardware (22:12, the same two loads):
3503 and 3568 flushes a minute, 6866 and 10981 rectangles, 0 behind the
scan at either end, longest flush 10.0 and 5.4 ms. The DRM vblank
timestamps are 16.667 ms apart and mark the start of the active scan,
433 us after the blank's interrupt (62 blanking lines of 7.5 us): the
flush thread starts with that much head start before the scan reaches
row 0 (`retrace/vblprobe`).

Trap: a boot cut short by a power cycle leaves
`~/config/settings/kernel/drivers/amdgpu_drm.starting` behind, and the next
boot runs on the plain `framebuffer` driver with no retrace semaphore: the
flush then switches itself off (`vertical blank flush: no retrace
semaphore`) and everything is copied as drawn, as before.

## Checking it

`retrace_test` (in `src/tests/servers/app/retrace`, in the Cubie lab image):

```
retrace_test blanks 5 [--line sunxi]
retrace_test draw 5 [--no-wait] [--update] [--size WxH] [--line ...]
retrace_test move 10 [--size WxH]
retrace_test scroll 10 [--size WxH]
retrace_test lines --line sunxi
```

`move` drags a window of stripes across the screen and back as the mouse
would (a move every 4 ms); `scroll` scrolls a view of stripes up and down.
Both are for watching the display (or its capture) for tears and for the
flush statistics in the syslog.

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
