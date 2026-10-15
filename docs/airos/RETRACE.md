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

### Torn frames counted at the WX 5100's scanout, 2026-10-16 00:10

The display controller computes a CRC of every frame it sends to the
monitor (DC's stream CRC; `DRM_HAIKU_IOCTL_CRC` in amdgpu_drm, over a
window of the frame since the Deskbar's CPU meter changes every 250 ms;
temporal dithering is switched to truncation while it runs). A frame made
of parts of two pictures (a tear) has a CRC that is no whole picture's.
`retrace/crcwatch` on the X399 reads the CRC after each blank;
`retrace/x399-crc-ab.sh` and `x399-crc-move.sh` run the comparisons,
switching the flush off and on with the `no_vblank_flush` settings file
(10 s, 598 frames each, the CRC window covering the whole test window):

| Load | Whole pictures | Flush off: torn frames | Flush on: torn frames |
| --- | --- | --- | --- |
| 3840x1080 window (the whole screen at 200 %) redrawn as fast as it can, no wait, two greys | 2 | 530 | 0 |
| 1600x900 window redrawn as fast as it can, no wait | 2 | 503 | 0 |
| 1600x900 window redrawn in updates | 2 | 0 (it asked for the blank) | 0 |
| 3840x1080 window redrawn in updates | 2 | 0 (it asked for the blank) | 0 |
| 1600x900 window with B_WAIT_FOR_RETRACE | 2 | 0 | 0 |
| 1200x700 window of stripes jumping 64 px to and fro, 100 moves a second | up to 8 (either place, the uncovered strips redrawn or not yet) | 574 | 0 |
| 1200x700 view of stripes scrolling 64 px to and fro, 115 scrolls a second | 4 | 445 | 0 |
| idle desktop | 1 | 0 | 0 |

Every frame the flush produced was one of the whole pictures. Its own
count agreed: 5 of 3068 flushes behind the hardware's scan line in the
redraw minute (window-frame rects at the top of the screen, copied 1.5 ms
after the blank while the test windows opened), 0 of 1826 in the moving
minute. The old code tears whenever a program draws without
B_WAIT_FOR_RETRACE, which is what moving windows, scrolling and most
drawing do; it already waited for the blank with it.

What the flush needed on the way there, each found by the CRC:

- it copies from a *shadow* of the back buffer, filled when a drawing op
  queues its area, so a drawing op under way does not get into a frame
  (3 to 8 % of the frames were made of both pictures before);
- two shadows, flipped at the blank: drawing ops write the current one,
  the flush copies the other, and neither waits for the other (waiting for
  writes had put 457 of 3064 copies behind the scan; skipping rects being
  written had shown a moved window without its uncovered strip);
- the flip comes 4 ms before the blank is due, so a large write that
  picked the old shadow is over when the copy starts (copies had begun
  with the scan at row 234);
- the copy goes down the screen in bands of 64 rows across all rects,
  as the scan does (rects side by side had been copied one after the
  other, the later ones behind the scan).

### Framework 13, 2026-10-16 06:58 (the flush, two displays)

Radeon 760M (DCN 3.1) driving the 2256x1504 panel (eDP-1) and a 4K
monitor (DP-2), a 6096x2160 frame buffer, two flush targets at their own
blanks. The arm of the flush that the X399 could not show: the blank
interrupt of this GPU, and so the retrace semaphore, comes 0.3 to 0.65 ms
*after* the scan has begun (the hardware scan line was at row 46 to 128
when the semaphore woke the thread), and a core in a deep idle state took
up to a millisecond more to wake. The flush therefore wakes from the
hardware's scan-start time (the scan-line hook returns it with the frame
period), sleeps until 1.5 ms before the next scan and spins the rest, and
flips the shadows 4 ms before. Loads of `retrace_test` on both outputs:
3441 flushes a minute on DP-2 and 2028 on eDP-1 under window moves at 132
to 154 a second, redraws at 211 a second and scrolls at 135 a second;
before the wake change 67 copies began behind the scan on DP-2 (top rows),
9 on eDP-1.

### Cubie A7S, 2026-10-15 23:28 (the flush, ARM64)

DisplayPort 1920x1080 at 60 Hz, `sunxi_display`'s TCON interrupt semaphore,
one target for the screen (no per-output or scan-line hooks there), the
arm64 app_server repacked into the board's package
(`retrace/cubie-vblank-run.sh`, `cubie-vblank-test.sh`):

| Minute | Flushes | Rectangles | Behind the scan (estimated) | Longest flush |
| --- | --- | --- | --- | --- |
| 1900x1000 window redrawn as fast as it can, no wait | 3564 | 4865 | 0 | 2.6 ms |
| 1200x700 window dragged | 3566 | 10630 | 0 | 3.0 ms |
| the tests before | 1324 | 3419 | 1 | 2.9 ms |

The program redrew 147 times a second and moved the window 111 times a
second; the display showed 60 whole frames of each.

## Checking it

`retrace_test` (in `src/tests/servers/app/retrace`, in the Cubie lab image):

```
retrace_test blanks 5 [--line sunxi]
retrace_test draw 5 [--no-wait] [--update] [--size WxH] [--line ...]
retrace_test move 10 [--size WxH]
retrace_test scroll 10 [--size WxH]
retrace_test lines --line sunxi
```

`retrace_test draw --update` waits for each `Draw()` (the window's thread)
so that every frame is a real update.

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
