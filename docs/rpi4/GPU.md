# GPU on the Raspberry Pi 4 (VideoCore VI, V3D 4.2)

The installed 2026-10-06 build adds persistent shader caching, cached Gallium
textures with explicit CPU/GPU ownership, GPU-assisted large readbacks,
native EGL bitmap reuse and retained kernel hardware state. Native pixel,
lifetime and application results are in [PERFORMANCE.md](PERFORMANCE.md).
OpenGL/GLES and Summit WebGL use V3D; Vulkan remains headless.
The 2026-10-08 follow-up adds bounded BO-cache metadata, shared GPU storage
for eligible EGL window bitmaps, BGLView background-clear removal, and
fixed bounds in the kernel's CPU cache walk. Current measurements and
qualification are in [PERFORMANCE-20261008.md](PERFORMANCE-20261008.md).

Initial bring-up, 2026-10-03: GLTeapot renders in a window at
about 315 FPS (`evidence/teapot6.jpg`); the offscreen probe is pixel-exact
from 128x128 to 1920x1080 with and without a depth buffer. Vulkan (Mesa's
v3dv, 2026-10-04) renders without a window: `rpi4_vk_probe` reports "V3D
4.2.14.0, Vulkan 1.3.328" and reads back a clear and a triangle exactly.

## Pieces

| Piece | Where | Notes |
|---|---|---|
| Firmware property module | `src/add-ons/kernel/generic/rpi_firmware` | mailbox requests: clocks (the V3D clock is firmware-owned), VL805 reload |
| Kernel driver | `src/add-ons/kernel/drivers/graphics/v3d` | publishes `/dev/graphics/v3d/0` |
| Interface | `headers/private/graphics/v3d/v3d_drm.h` (Linux's uapi structures), `v3d_haiku.h` (request codes, handle/sync stand-ins) | |
| Mesa | `tools/rpi4/mesa` (build script, patch on the ROCK 5's Mesa 25.3.6 port, probe) | `v3d` Gallium driver behind libglvnd and Haiku's EGL driver |

## Kernel driver

- Power: the firmware V3D clock, then the GRAFX power domain's reset line and
  the two ASB bridges (`power_on`/`power_off`). A reset after a hung job is a
  full power cycle of the domain; touching the core any other way after a hang
  stalls the bus.
- Memory: the core sees memory only through its MMU. One flat page table
  (4 MB, uncached) covers the 4 GB GPU address space; a buffer object is a
  locked area plus a first-fit range of GPU addresses and its page table
  entries. Ordinary buffers and Vulkan use write-combining memory; eligible
  Gallium textures use write-back memory with explicit cache maintenance.
  `MMAP_BO` clones the area into the caller's team and returns the address
  (there is no mmap on a device node).
- Lifetime: the fixed FDT driver node is retained. Its page table, scratch
  page, register mappings and executor are initialized at registration and
  survive the last application close. This avoids repeatedly finding a
  contiguous 4 MiB table after memory fragmentation. Client buffers and sync
  objects are still released per file. `rpi4_v3d_lifetime_probe` checks this
  distinction; native measurements are in `PERFORMANCE.md`.
- Jobs: bin/render lists, TFU and compute (CSD) jobs go into one queue and run
  in submission order on an executor thread; the interrupt handler reports
  frame/flush done, out-of-memory in the binner (the driver hands it another
  buffer) and MMU errors. A job that does not finish in 5 s is reported with
  the core's registers and a summary of its buffers, and the core is reset.
- Sync objects are job sequence numbers; a fence is a token for one. For
  Vulkan a sync object can also be reset or signalled by hand, take over
  another one's state, and be waited for before it has a job; the multi-sync
  extension of the submit requests names several to signal.

## Raspberry Pi 5 (V3D 7.1)

The same driver handles the BCM2712's core (`brcm,2712-v3d`, Linux's
`ver >= 71` cases in `drivers/gpu/drm/v3d`). Not yet run on the board.

- Power: the V3D domain is only `PM_V3DRSTN` in `PM_GRAFX_2712` (0x304) of
  the PM block at 0x10_7d20_0000; there are no ASB bridges. The graphics
  domain (`PM_GRAFX`) is switched on Linux's way only if the firmware left
  it off. The firmware V3D clock is set to its maximum before the reset is
  released. Then the "sms" block (third `reg`) is taken out of power off
  and reset (`v3d_idle_sms()`); a hang reset is SMS reset, then the domain
  reset bit.
- Two interrupt lines: `interrupts` 0 is the hub's, 1 the core's.
- Moved: TFU registers to 0x700 with an output configuration register
  (`v71.ioc` of the submit request) and the coefficients always written;
  CSD queued configuration to 0x930 plus a CFG7 that is kept 0; CSD done is
  core bit 6 (bit 5 is the counters'); the memory protection violation is
  hub bit 6; GMP registers at 0x600.
- The firmware property module reaches the BCM2712's mailbox at
  0x10_7c01_3880 and passes physical message addresses (no 0xc0000000
  alias), as the loader does.
- Mesa needs nothing: its build has the 7.1 code (`v3d_versions` 42 and 71)
  and reads the version from the core's identity registers.
  `rpi4_v3d_probe` encodes its TFU job for the version the device reports.

## Mesa

`tools/rpi4/mesa/build.sh` builds `libEGL_mesa.so.0` and stages it together
with the EGL vendor file in `/mnt/HaikuWork/rpi4/mesa/stage`; the
`rpi4-airos` image profile installs both (plus GLTeapot and GL Info).
`README.md` in that directory lists what the patch changes.

Eligible EGL windows now present a cached linear GPU staging buffer as a
BBitmap. This removes the extra CPU readback-to-bitmap copy; app_server
still copies the bitmap into its back buffer and then to the framebuffer.
Small or unsupported windows retain the copy path. Summit uses its separate
pbuffer/readback path. None of these paths provides direct GPU scan-out.

The probe:

    B=/mnt/HaikuWork/artifacts/mali-system-opengl-build/20260918T125722Z
    /mnt/HaikuWork/build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku-g++ \
        --sysroot=$B/sysroot -std=c++17 -O2 \
        -I$B/glvnd-install/boot/system/develop/headers/os/opengl \
        -I$B/sysroot/boot/system/develop/headers/os/opengl \
        tools/rpi4/mesa/gl_probe.cpp -L$B/glvnd-install/boot/system/lib \
        -lEGL -lGLESv2 -o rpi4_gl_probe
    PROBE_DEPTH=1 PROBE_WIDTH=1920 PROBE_HEIGHT=1080 ./rpi4_gl_probe

## Traps met on the way

- **Depth format.** Haiku's Mesa frontend asked for `Z24_UNORM_S8_UINT`
  without asking the driver. V3D only has `S8_UINT_Z24_UNORM`; the store of
  the depth buffer then goes out with output format 0 (a colour format) and
  the render thread wedges after a tile and a half with no error bit set
  anywhere (render list consumed, no FRDONE, GMP status 0x30). The symptom
  looked like a kernel problem for a long time: colour-only frames were
  perfect. What found it: `V3D_DEBUG=cl` (Mesa dumps its command lists) and
  looking at what the core had written into each buffer.
- The core's identity register reads "V3D" in the low three bytes only.
- Mesa's buffer cache mutex must be initialized (Haiku asserts on a zeroed
  mutex).

## Open

- **Vulkan beyond the probe.** There is no Vulkan loader for arm64 Haiku
  (programs link `libvulkan_broadcom.so` and start from
  `vk_icdGetInstanceProcAddr`) and no window system layer, so nothing can
  present yet. Sync files, timeline semaphores, the CPU job queue (indirect
  compute, timestamp and performance queries) and PRIME are not there.
  Render coverage remains the probe's clear and triangle workloads,
  including repeated execution under combined CPU/GPU/decoder load.
- Presentation without the read-back copy.
- 8 GB boards: buffers above 4 GB cannot be mapped by this MMU setup
  (28-bit page numbers are fine, but the driver has only been run on 4 GB).
