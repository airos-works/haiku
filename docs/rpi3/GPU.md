# GPU on the Raspberry Pi 3 (VideoCore IV, V3D 2.1)

Status, 2026-11-18: the kernel driver (`vc4`) and Mesa's `vc4` Gallium driver
for air/OS build; the validators pass a host test. **Nothing has run on a
Pi 3 yet** (no board in the lab; QEMU's raspi3b has no V3D). The hardware test
below is written to be done in one go.

The BCM2837's 3D core is the VideoCore IV "V3D 2.1", as in the BCM2835/6:
twelve QPUs in three slices, two texture units per slice. Linux drives it
with `drivers/gpu/drm/vc4` (the 3D part: `vc4_gem.c`, `vc4_validate*.c`,
`vc4_render_cl.c`, `vc4_irq.c`, `vc4_v3d.c`) and Mesa with
`src/gallium/drivers/vc4`. air/OS does the same as for the Pi 4/5's V3D
([../rpi4/GPU.md](../rpi4/GPU.md)): a kernel driver that takes Linux's DRM
structures in Haiku device requests, and Mesa with Haiku stand-ins for libdrm.

## Pieces

| Piece | Where | Notes |
|---|---|---|
| Kernel driver | `src/add-ons/kernel/drivers/graphics/vc4` | `brcm,vc4-v3d` (the Pi firmware's trees) / `brcm,bcm2835-v3d` (Linux's); publishes `/dev/graphics/vc4/0`; in the arm64 `haiku` package |
| Interface | `headers/private/graphics/vc4/vc4_drm.h` (Linux's uapi), `vc4_haiku.h` (request codes, handle close, `GET_INFO`) | request `VC4_HAIKU_FIRST_OP + n` is `DRM_VC4_*` n |
| Firmware | `src/add-ons/kernel/generic/rpi_firmware` | V3D power domain (`SET_DOMAIN_STATE`, domain 11) and clock 5 |
| Mesa | airos-works/mesa, branch `airos-25.3.6-rpi3` | `vc4_haiku.h`, `vc4_haiku_screen_create()`, EGL picks vc4 when there is no V3D |
| Board test | `tools/rpi3/vc4_probe.cpp` | the device without Mesa |
| Host test | `tools/rpi3/vc4_host_test/run.sh` | the validators under ASan/UBSan |

## Kernel driver

### Memory

The core has **no MMU**: it reads and writes physical memory, at the bus
address of the BCM2837's uncached alias, `physical | 0xc0000000` (the `/soc`
`dma-ranges`: bus 0xc0000000 is physical 0, for 0x3f000000 bytes). So every
buffer object is physically contiguous and below 1008 MB, and every address
the core is given has been checked by the driver (see Validation).

- At boot, while there are still long runs of free pages, the driver reserves
  - 16 MB for the **binner** (tile state, tile lists and their overflow), in
    32 slots of 512 KB, not crossing a 256 MB line: the tile state keeps 28
    bit addresses and the top 4 bits come from its own (Linux:
    `bin_bo_alloc()`);
  - a **pool** for the buffers, 64 MB unless the setting `pool_size` (MB, in
    `/boot/home/config/settings/kernel/drivers/vc4`) says otherwise; 0 means
    none. If the pool cannot be had, half of it is tried, down to 8 MB.
- A buffer is a first-fit range of the pool; when the pool is full (after
  giving back ranges whose last mapping went), a contiguous area of its own,
  which may fail on a system that has run for a while: Mesa then empties its
  buffer cache and tries again, as on Linux. Linux has a 256 MB CMA area
  (64 MB in the Pi 3 B+'s tree); the pool is the same idea without page
  migration.
- New buffers are cleared. The CPU sees all of it write-combining (the core
  does not snoop the ARM's caches; the allocation's lines are cleaned and
  invalidated first).
- `MMAP_BO` maps the buffer into the calling team as a clone of a kernel area
  and returns the address. A pooled buffer gets a kernel area of its own over
  its range for that (a device mapping), whose cache the driver keeps a
  reference to. The team's mapping is `B_KERNEL_AREA`: it cannot delete,
  resize or reprotect it (shrinking a clone would free pages the core still
  uses); the driver deletes it when the handle is closed or the device file
  goes. A `fork()` copies the mapping, sharing its cache: a pooled range is
  only given back when that cache has no user but the driver (the range stays
  a "zombie" until then), so a child can never see the range reused for
  another buffer or a validated command list. Shader buffers map read-only.

### Jobs

A job is an optional binner command list (CL) and a render CL. Submission
(`SUBMIT_CL`):

1. the buffer handles are looked up and referenced;
2. `vc4_get_bcl()`: the binner CL, the shader records and the uniforms are
   copied into the kernel and validated into a kernel buffer (from the pool),
   which is what the core reads (the "exec BO");
3. `vc4_get_rcl()`: the driver writes the render CL from the job's surfaces
   (colour/ZS read and write, MSAA resolves), clear values and tile range;
4. the job gets the next sequence number, its buffers that number (and the
   render targets their "write" number), and it is queued.

One thread runs the jobs, one at a time, in the order they were submitted:
caches flushed (L2, slice caches), a 512 KB overflow slot given to the binner
(`BPOA`/`BPOS`), the binner CL started, then on the flush done interrupt
(`FLDONE`) the texture caches flushed and the render CL started, until frame
done (`FRDONE`). When the binner runs out of memory (`OUTOMEM`, which stays
asserted until it has more) it gets another slot. A job's slots are free once
it is done. Linux overlaps binning one job with rendering the one before;
here binning waits, which keeps the ordering trivial (Linux's wait for the
jobs that wrote what the binner reads, `bin_dep_seqno`, is implied).

- A list whose current address (`CTnCA`) does not move for a second is given
  up (Linux: 100 ms): its registers are logged and kept for `GET_HANG_STATE`,
  the control list threads reset, and the V3D power domain and clock cycled
  through the firmware (Linux's reset goes through runtime PM the same way).
  `GET_INFO` counts resets.
- Should the interrupt not come (routing on the BCM2837's interrupt
  controllers is new and untested), the core's flush and frame counters
  (`BFC`, `RFC`) still say when a list is done; the driver then logs it once
  and polls every millisecond.
- `WAIT_SEQNO`/`WAIT_BO` wait for a sequence number (`ETIME` →
  `B_TIMED_OUT`; an interrupted wait leaves the rest of the timeout in the
  request, as Linux does). There are no sync objects: Mesa sees no
  `DRM_CAP_SYNCOBJ` and fences by sequence number.

### Validation

Ported from Linux (MIT-licensed files, notices kept; Linux's GPL files —
`vc4_v3d.c`, `vc4_bo.c`, `vc4_drv.h`, `vc4_regs.h` — are not used; the
register offsets come from the VideoCore IV 3D reference guide):

- `vc4_validate.cpp`: the binner CL packet by packet against a whitelist
  (state, primitives, shader state, the tile binning config the kernel fills
  in with its bin slot, `GEM_HANDLES` relocation packets); it must start
  binning after the config and end with `INCREMENT_SEMAPHORE` and `FLUSH`.
  Index buffers, vertex attributes (with the highest index the primitives
  use) and textures (format, size, tiling, mip levels, cube map stride) are
  checked against their buffers and relocated; shader records point at shader
  buffers only; uniforms are copied after the handles of the textures they
  sample.
- `vc4_validate_shaders.cpp`, when a shader buffer is created: no general
  VPM DMA, no host interrupt or mutex writes, texture lookups set up from
  uniforms (direct lookups only through a clamped offset), relative branches
  inside the shader with uniform address resets after branch targets,
  threaded shaders without the upper register halves. The kernel validates
  its copy of the code and runs that copy.
- `vc4_render_cl.cpp`: surfaces (format, tiling, 16 byte alignment, size
  within the buffer) for the render config, loads and stores, full
  resolution (MSAA) surfaces.

Stricter than Linux (Mesa never does what these refuse):

- a shader record's handles must be below the handle count (Linux accepts
  the count itself, one past the array);
- a buffer is checked to be a shader before its validation state is used;
- the uniforms the shader records consume are counted against the uniform
  data submitted (Linux never decrements it);
- the render tiles must lie within the frame (32x32 tiles with 4x MSAA, 64x64
  otherwise; with a binner CL also within its tiles: Linux lets the maximum
  be one past them), frames are at most 4096 square;
- a full resolution surface must hold its last tile whole;
- the overflow memory given to the binner is its 512 KB slot (Linux writes
  the whole bin buffer's size to `BPOS`).

### Requests

`GET_PARAM` (identity registers read at start; branches, ETC1, threaded FS
and fixed RCL order are supported; madvise and performance monitors are
not), `CREATE_BO`, `MMAP_BO`, `CREATE_SHADER_BO`, `SUBMIT_CL` (no
`perfmonid`, `in_sync`, `out_sync`), `WAIT_SEQNO`, `WAIT_BO`, `CLOSE_BO`
(`GEM_CLOSE`), `SET_TILING`/`GET_TILING`, `LABEL_BO` (accepted, ignored),
`GEM_MADVISE` (recorded, nothing is purged), `GET_HANG_STATE` (the registers
of the last hang, no buffers), and air/OS's `GET_INFO` (identity, clock, pool
use, bin slots, job and interrupt counts, resets, polling).

Setting `debug false` in the settings file silences the validators' reasons
for refusing a job (on by default for the bring-up).

## Mesa

airos-works/mesa, branch `airos-25.3.6-rpi3` (from `airos-25.3.6-rpi4`):

- `src/gallium/drivers/vc4/vc4_haiku.h`: `DRM_IOCTL_VC4_*` and
  `DRM_IOCTL_GEM_CLOSE` to the device's requests; interrupted requests are
  repeated, timeouts reported as `ETIME`. `vc4_ioctl()` uses it on Haiku.
- `vc4_bufmgr.c`: the mapping comes from `MMAP_BO`, no `mmap()`/`munmap()`.
- `include/xf86drm.h` (the libdrm stand-in): `drmGetCap()` reports nothing,
  so vc4 has no sync objects; `vc4_context.c` exports no sync file.
- `vc4_haiku_screen_create()` in the winsys (the renderonly stand-ins are
  weak there: v3d's winsys has the same).
- EGL (`egl_haiku.cpp`): without a V3D, the vc4 device when its driver
  published it; `HAIKU_VC4_DEVICE` names another device,
  `LIBGL_ALWAYS_SOFTWARE=1` skips it. A frame is rendered into a GPU buffer
  and copied into the window's bitmap by the CPU
  (`haiku_present_texture()`, the texture is detiled by Mesa's transfer). The
  V3D's direct present into the frame buffer (haiku#109) and shared bitmaps
  stay V3D-only (`hgl_dpy->v3d`).

One `libEGL_mesa.so.0` carries v3d and vc4 (their request numbers differ:
v3d's start at `B_DEVICE_OP_CODES_END + 0x7600`, vc4's at `+ 0x7700`), so a
single Pi image can serve the Pi 3, 4 and 5.

### Building Mesa

The CI builds the Pi's GL stack in `airos-ci/deps/build-gl.sh` (mesa-v3d:
`-Dgallium-drivers=v3d,softpipe`). The local build of this branch uses the
CI's GL sysroot, cross file and host compilers read-only and adds vc4;
`/mnt/HaikuWork/rpi3/mesa-vc4-build/build.sh` runs exactly this:

    GL=/data2/airos/work/gl
    GLROOT=$GL/sysroot
    system=$GLROOT/boot/system
    export PATH=/data1/airos/toolchain/mesa-host-65bc4606d42a/bin:$PATH
    cross() { env -u CC -u CXX -u CPPFLAGS -u CFLAGS -u CXXFLAGS -u LDFLAGS \
        PKG_CONFIG_LIBDIR="$system/develop/lib/pkgconfig:$system/lib/pkgconfig" \
        PKG_CONFIG_SYSROOT_DIR="$GLROOT" PKG_CONFIG_PATH= "$@"; }
    cd /mnt/HaikuWork/rpi3/mesa-vc4-build
    cross meson setup build /mnt/HaikuWork/rpi3/mesa-vc4 \
        --cross-file=$GL/cross.ini --cross-file=rpi3-cpu.ini \
        --prefix=/boot/system --libdir=lib --buildtype=debugoptimized \
        --wrap-mode=nofallback \
        -Dplatforms=haiku -Dexpat=disabled -Dgallium-va=disabled \
        -Dgles1=disabled -Dgbm=disabled -Dglx=disabled -Dllvm=disabled \
        -Dvalgrind=disabled -Dbuild-tests=false '-Dtools=[]' -Dxmlconfig=disabled \
        -Dmesa-clc=system -Dprecomp-compiler=system -Dspirv-tools=disabled \
        -Dgallium-drivers=v3d,vc4,softpipe '-Dvulkan-drivers=[]' \
        -Dgles2=enabled -Dopengl=true -Degl=enabled -Dglvnd=enabled \
        -Dshader-cache=enabled -Dshader-cache-max-size=64M -Dzlib=enabled \
        -Dzstd=disabled
    cross ninja -C build

`rpi3-cpu.ini` tunes for the Pi 3's Cortex-A53 (`-mtune=cortex-a53
-mno-outline-atomics`, with the sysroot repeated: a later cross file's
options replace the earlier one's). `mesa-host-65bc4606d42a` is the CI's host
compiler build (`mesa_clc`, `vtn_bindgen2`) for this Mesa. The result is
`build/src/egl/libEGL_mesa.so.0.0.0`.

For the images, `build-gl.sh`'s mesa-v3d build needs `vc4` added to its
`-Dgallium-drivers` and its `mesa-rpi4` fork pointed at this branch (or the
branch merged into `airos-25.3.6-rpi4`); not done.

## Testing

### On the build host

    tools/rpi3/vc4_host_test/run.sh

builds the validators with stand-ins for the kernel's headers and runs them
under AddressSanitizer and UBSan: the render CL of a clear (packet by packet),
a binner CL's relocation into a bin slot, a draw with a shader record (shader,
uniform and attribute relocations), and what must be refused (out of bounds
surfaces and vertex buffers, handles past the end, shaders as data and data
as shaders, too few uniforms, VPM DMA and endless shaders).

### On a Pi 3 B+ (the one-shot test)

What it needs first: an image that boots on the Pi 3 up to the desktop
(the `rpi3` branch boots to mounting the boot volume in QEMU; the Pi 3's
SD host and USB host drivers are on `rpi3-sdhost` and `rpi3-dwc2`), plus
this branch.

1. **Image.** Build it from this branch (merged with whatever boots the Pi
   3) — the `vc4` driver is in the `haiku` package:

        cd /mnt/HaikuWork/rpi3/build-vc4
        source ../haiku-vc4/tools/rock5-itx/env.sh
        jam -q -j32 @minimum-rpi4 build airos-rpi-image

   Or, on a system that boots already, copy the driver to
   `/boot/system/non-packaged/add-ons/kernel/drivers/graphics/vc4`
   (`objects/haiku/arm64/release/add-ons/kernel/drivers/graphics/vc4/vc4`)
   and reboot (not tried: the device tree's nodes are matched against the
   modules under `drivers`, which the kernel also looks for in
   non-packaged; the driver reserves its memory when it binds, so a boot is
   the right moment anyway).
2. **Boot log** (`grep vc4 /var/log/syslog`, or the serial console).
   Expected, roughly (the counts come from the core's IDENT1):

        vc4: V3D 2.1: 3 slice(s) of 4 QPUs and 2 texture units, 12 KB VPM, 16 semaphores; clock 300000000 Hz, interrupt 42
        vc4: binner memory at 0x..., buffer pool of 64 MB at 0x...

   If instead "no V3D 2 behind the registers ... IDENT0 0": the power domain
   did not come on (firmware too old for `SET_DOMAIN_STATE`, logged just
   before). Try `dtoverlay=vc4-fkms-v3d` in config.txt's `[pi3]` section,
   which makes the firmware hand the 3D core to the OS.
3. **The probe** (no Mesa involved). Build it on the host:

        cd /mnt/HaikuWork/rpi3/haiku-vc4
        /data1/airos/build/haiku-arm64/cross-tools-arm64/bin/aarch64-unknown-haiku-g++ \
            --sysroot=/data1/airos/sdk/arm64/sysroot -O2 -Wall \
            -I headers/private -I src/add-ons/kernel/drivers/graphics/vc4 \
            tools/rpi3/vc4_probe.cpp -o vc4_probe

   copy it to the board and run `./vc4_probe`. Every line should be `PASS`;
   the exit status counts the failures. It clears a 128x96 surface three
   times (render CL only, with an empty binner CL, render CL only again) and
   checks every pixel and the page behind the surface; then the refusals and
   a shader buffer; then the counters ("no job hung", "interrupts arrive").
   Keep its whole output and the `vc4:` lines of the syslog.
4. **Mesa.** Install the vc4 build:

        /boot/system/non-packaged/lib/libEGL_mesa.so.0      <- build/src/egl/libEGL_mesa.so.0.0.0
        /boot/system/non-packaged/add-ons/opengl/egl_vendor.d/10_mesa.json

   (the vendor file as the Pi images have it: `"library_path":
   "/boot/system/non-packaged/lib/libEGL_mesa.so.0"`). Then
   - `GLInfo`: the renderer should read "VC4 V3D 2.1", OpenGL ES 2.0 /
     OpenGL 2.1;
   - `GLTeapot` (Demos): a lit, spinning teapot; note the frame rate;
   - `HGL_TRACE=1 GLTeapot` from Terminal prints which present path is used;
     `VC4_DEBUG=cl,shaders GLTeapot` dumps Mesa's command lists and shaders
     (for a job the kernel refuses: its reason is in the syslog);
     `LIBGL_ALWAYS_SOFTWARE=1` for the comparison with softpipe.
5. After a hang: `vc4_probe` prints the hang registers (`GET_HANG_STATE`);
   the syslog has "job N hung: bin ... render ..." with the control list
   addresses and status, the binner pool and overflow, `PCS`, `ERRSTAT` and
   the FDBG registers.

## Untested, and what to look at first

Everything on the hardware. In the order the risks seem likely:

1. **Booting the Pi 3 at all**, which the test depends on (storage drivers,
   the interrupt controller port: kernel IRQ 42 = 32 + VideoCore IRQ 10 is
   the loader's conversion). If the V3D interrupt does not arrive, the driver
   says so once and polls; GL then works, with more CPU wake-ups.
2. **The power domain and clock from the firmware** (domain 11 through
   `SET_DOMAIN_STATE`, clock 5 at its maximum): whether the firmware, without
   the fkms overlay, lets the ARM have the core; IDENT0 tells.
3. **Binner memory handling**: the overflow slot given at the start of each
   bin job instead of Linux's carried-over one; `BPOS` set to the slot size;
   the 256 MB rule. A frame with many triangles exercises it (GLTeapot
   should).
4. **Memory types**: buffers are Normal non-cacheable for the CPU, and
   pooled buffers are mapped a second time (a device mapping) for the teams;
   both attributes are the same, but the kernel's cacheable linear map of
   all RAM still aliases them (as for v3d and on Linux).
5. **The pool's size and the contiguous fallback** under real use (Summit,
   several GL programs): 64 MB + 16 MB are taken at boot from the Pi 3's
   ~900 MB.
6. **Mesa on Haiku**: the vc4 driver's transfers (tiling to and from
   write-combining memory) are slow for the CPU; presenting copies every
   frame (no direct present yet).

## Later

- Direct present as the V3D does it (haiku#109): render into, or blit into,
  rpi_display's frame buffer; on the VC4 that needs the frame buffer's bus
  address and a render config pointing at it (the core can write any
  physical memory the driver allows, with no import step).
- Purging buffers marked `DONTNEED` when the pool runs out (Linux's
  `vc4_bo_purge`); `SUPPORTS_MADVISE` then 1.
- Binning a job while the one before renders (Linux's two queues).
- Sync objects and sync files, if something needs them (Vulkan has no VC4
  driver).
