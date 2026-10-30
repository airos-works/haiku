# amdgpu_drm: the WX 5100 on Linux's amdgpu

Since 2026-10-12 the X399's Radeon Pro WX 5100 (Polaris 10, 1002:67c7) runs
on **amdgpu_drm**: Linux 6.18's amdgpu driver, with the DRM core, TTM, the GPU
scheduler and Display Core, built as a Haiku kernel driver over a Linux
compatibility layer. Everything above it is the stock Linux user land for AMD
GPUs: libdrm, Mesa's radeonsi (OpenGL, OpenGL ES, EGL), RADV (Vulkan) and
Mesa's VA-API driver. The same code is meant to drive other AMD GPUs later
(the Framework Desktop's Strix Halo, other Radeon cards); only firmware and
testing are per device.

The native driver for this card that came first (`amdgpu` and its `amduvd`
decoder, [WX5100.md](WX5100.md)) was removed from the tree on 2026-10-13.
The x86_64 air/OS images built since carry amdgpu_drm and the amdgpu
firmware instead (below).

## What works (verified on the X399, 2026-10-12/13)

| Area | Result |
| --- | --- |
| Display | app_server on the `amdgpu_kms` accelerant: both 3840x2160@60 DisplayPort monitors in one 7680x2160 frame buffer at 200 %, hardware cursor, vertical blank sync |
| OpenGL / GLES / EGL | radeonsi (ACO) for BGLView applications (GLTeapot) and EGL (Summit's compositor); `gles2-draw-probe` passes |
| Vulkan | RADV, Vulkan 1.4.328: `vkprobe` passes, `vkbench` compute 1.77 TFLOP/s, fill 33.5 Gpixel/s, VRAM copy 123 GB/s, host upload 5.7 GB/s |
| Vulkan in windows | `VK_KHR_wayland_surface` through Haiku's in-process Wayland server: 60.0 fps with vsync (FIFO), a 1920x1080 swapchain at 167 fps (mailbox) |
| Video decoding | UVD through VA-API: MPEG-2, VC-1, H.264, HEVC Main and Main 10, JPEG; VCE encoding for H.264 and HEVC. H.264, HEVC and HEVC Main 10 decode bit-exact against FFmpeg's software decoder |
| airTime | 1.0.0-18 (installed) plays H.264 and HEVC/Main 10 on the GPU ("AMD UVD (amduvd ...) [hardware]"): 1080p at 2-5 ms a picture, 4K H.264 at 60 fps and 4K HEVC Main 10 at 30 fps without drops |
| Summit | renders with radeonsi, plays H.264 and HEVC on the GPU, looping videos start over, WebGL 1 and 2 through ANGLE on radeonsi (WebGL Aquarium 60 fps) (bundle-ci1631, the CI build of Summit ce257e0; see Summit's docs/wx5100-media.md and docs/webgl.md) |

Known limits:

- Display power management: an 8-minute DPMS sleep and wake keeps the
  layout; the hot plug events that waking raises are recognised as the same
  monitors and change nothing (events while asleep are checked on wake).
- Monitors coming and going: forcing a connector off and back
  (`kmsforce DP-1 off`, `kmsforce DP-1 detect`), the other monitor keeps its
  CRTC and gets the desktop, and both come back where they were; a
  DisplayPort cable pulled and plugged back in at the machine (13 October)
  recovered the same way.
- The display engine (DCE 11.2) has no overlay cursor, so amdgpu_dm takes
  no hardware cursor over a region that is scaled (DP-4 at 100 % beside
  DP-1 at 200 % is drawn at 100 % and DP-1's region enlarged): app_server
  draws the cursor itself then ("amdgpu_kms: no hardware cursor on these
  monitors" in the syslog). Drawing such a layout at 200 % would need an
  11520x4320 frame buffer, more than the 256 MiB of video memory the CPU
  can reach.
- Video under load: UVD's pictures came out wrong now and then while
  Vulkan programs ran (a damaged reference, wrong to the next key frame:
  1 to 4 runs of the 300-picture H.264 test stream in about 150), because
  the memory clock idles at 300 MHz while UVD decodes and other GPU work
  starves it there (held at 300 MHz, every run was wrong). amdgpu_drm
  4400077 keeps the memory clock at its highest level while UVD or VCE is
  powered (smu7); idle stays at 300 MHz and 10 W. Since then: 0 of 188
  runs under three Vulkan programs, and `gpu-soak.sh` (OpenGL windows,
  H.264 and HEVC Main 10, Vulkan, windows and decoders killed mid-frame,
  Wayland windows) clean with every picture exact.
- Hangs under one load: decoding into video memory (VL_VIDEO_BUFFER_GTT=0)
  beside three Vulkan programs stopped the machine nine times, after 40 to
  50 minutes each - no network, no disk, nothing in the syslog. The cause
  was the compat layer's thread -> task table. Every Haiku thread that
  calls into Linux code gets a task ("current"); a THREAD_REMOVED listener
  frees it. Haiku's ThreadNotificationService::Notify(uint32, Thread*) has
  passed the thread and team IDs the wrong way round since 2010, so the
  listener freed the team's main thread's task (while it ran on) and never
  any other thread's. The table was open addressing whose deleted slots
  never became free again, looked up on every "current" under a global
  spinlock with interrupts disabled: once 8192 threads had registered,
  every lookup of a new thread walked all of it and registering failed, so
  such threads got a new task at every "current" (~180000 allocations a
  second), CPUs spun with interrupts off, and the machine stopped. Fixed in
  linux-drm 608a7cc (the listener takes the thread from the event's Thread,
  and the table has chains); the kernel's notification is fixed in the
  Haiku fork's fffb079c8f. Since then the same load ran 2 h 7 min clean
  (kernel allocations flat, 1 picture of 1700 runs not exact).
  Found on the way, and fixed too: amdgpu never freed a closed file's
  contexts (5d2bcaa: the compat IDR's idr_get_next() underflowed for IDRs
  with base 1, so idr_for_each_entry() found nothing - their scheduler
  entities piled up in the run queues with 32 fences each), and buffers
  mapped into teams kept the page frames they had when they were mapped,
  so after TTM moved a buffer the team read and wrote memory that was no
  longer the buffer's (eefd858; see the trap below).
  Lab tools that found them: `drmparam haiku_watchdog_ms 1000` (with
  log_file) logs every second the interrupts and rings, the kmalloc() call
  sites with the most objects alive, the kmem caches, and Haiku's threads,
  ports and semaphores, and what the interrupt thread waits for when it is
  slow; `drmparam haiku_track_objects 1` then `drmparam haiku_fence_report
  1` lists the scheduler fences alive by scheduler and client, the files
  and contexts open, TTM's buffers and the schedulers' jobs.
  For hangs that leave no trace (the X399 has no serial line, and its
  kernel debugger shows on a monitor nobody sees): the kernel debugger
  extension kdl_capture (`src/add-ons/kernel/debugger/kdl_capture`,
  installed in `/boot/system/non-packaged/add-ons/kernel/debugger/`)
  records a session, types `sc` and `co`, and a thread appends it to
  `/boot/home/kdl_capture.log` - a panic reads like a crash log after the
  next boot (lab use: it also leaves every panic). `log_file` now appends
  after the text already in the file, with times since boot, and the driver
  writes a heartbeat around the file system into its last MiB after the
  start, as a ring (every second for two minutes, then every 10 s; the
  newest line dates a whole-machine freeze); `trace_calls 2` traces the
  calls of the stop (TRACE_CALLS also takes compat/ and glue/ files).
- The first boot after `x399-install.sh` died most of the time (2026-10-13/14),
  right after "devfs: reload driver amdgpu_drm": devfs stopped the GPU it
  had just started and started it again. devfs reloads a driver none of
  whose devices is open as soon as it takes the binary for changed, and
  `add_driver()` (legacy_drivers.cpp) does that whenever it meets a known
  driver again with equal priority - also the same binary through a second
  link. amdgpu_drm was linked from dev/graphics and dev/dri, so the first
  scan of the second directory marked the running driver changed; the boot
  job's swap of a staged build did the same, its events handled after the
  new build had loaded. Stopping a started GPU then panicked ("object_cache:
  object ... is already freed" in `kthread_stop()` of a DRM vblank worker,
  from `drm_vblank_init_release()`; caught by kdl_capture, see the tools
  below) or froze the machine, depending on timing; the stop's
  `_kern_sync()` (start marker) in devfs' driver lock was one place it
  stopped. Fixed in linux-drm 651e81b: the driver keeps
  `/dev/dri/renderD128` open while the GPU runs (devfs never reloads it; a
  changed binary starts at the next boot), is linked from dev/graphics only,
  syncs in a thread of its own waited for 3 s at most, does no file system
  work when stopped, and a start by devfs' kernel daemon while app_server
  runs (an installation onto a desktop on the framebuffer driver) waits for
  the next boot. Four installation boots since, each one start, no reload.
  To check an installation: no "devfs: reload driver amdgpu_drm" in the
  syslog, and the kernel team holding `/dev/dri/renderD128` open (the
  driver's "keeping ... open" line can be lost from the syslog when the
  kernel's buffer overflows early in a boot, as on the Framework 13).
  A panic within 30 s of the start - anyone's - leaves the start marker
  (the Framework 13's cpuidle swap did): delete it after such a crash.
- Open: stopping the GPU soon after its start can free a kthread's task
  twice (the panic above); with the device kept open nothing stops a
  running GPU any more, but a failed start's cleanup still takes that path.
- Boot safety: the driver keeps a marker
  (`~/config/settings/kernel/drivers/amdgpu_drm.starting`) from its start
  until the GPU has run for 30 s; a boot that finds a marker of an earlier
  boot leaves the GPU to the framebuffer driver until it is deleted (a
  marker of the same boot is a start devfs repeated). The reload hangs
  exercised it: the boot after each one came up on the framebuffer driver.

- GPU writes into system memory from the graphics and compute engines
  (shaders, CP DMA) reach only 1.55 GB/s; SDMA writes 4.7 GB/s (reads 5.7
  and 7.1). Not the link (8 GT/s x8 held, AER clean) nor snooping (the
  experimental `haiku_uswc` buffers write no faster). Readbacks therefore
  go through SDMA (radeonsi `si_sdma_readback`: whole textures into linear
  system-memory ones, which transfers and vaGetImage do): GL readback of
  1024x768 2.6 -> 0.9 ms. Vulkan copies still use the graphics/compute
  queues (RADV offers no SDMA queue on Polaris).
- Vulkan presents by copying (no dma-buf on Haiku): GPU blit to host memory,
  copy into the window's shared buffer.
- Host memory imports (`DRM_AMDGPU_GEM_USERPTR`, RADV's
  `VK_EXT_external_memory_host`, airos-works/haiku#38) wire the pages for the
  buffer's whole life instead of following them with HMM: the memory has to
  stay mapped until the buffer is freed. `delete_area()` or `munmap()` of it
  before then waits, uninterruptibly, until the buffer is gone, so a
  program that frees the memory first hangs and cannot be killed. A team
  that `exec()`s while it holds such a buffer is not covered either (its
  areas go before its files; a KDEBUG kernel panics). A team that dies is:
  the driver watches it, waits for the GPU and lets the pages go before its
  address space goes. Verified on the Framework 13 (2026-10-19): Summit's
  copy into app_server's back buffer, kill -9 of the importing program while
  the GPU writes, a display layout change while the old back buffer is
  imported.
- Wayland clients (vkwayland) sometimes crash as they exit: the in-process
  Wayland server's window thread (`wayland_server` 0.1.20251004, HaikuPorts)
  runs on in `BWindow::task_looper()` after the program has unloaded the
  server's add-on. A roundtrip after destroying the window makes it rarer
  (1 in 15 runs). Not the GPU stack's; debug_server is set to write a
  report rather than stop with a dialog for the GPU test programs.
- No sysfs: power management can only be set with module parameters (at
  boot, in the settings file; `drmparam` sets one in the running driver,
  but most are read only when the GPU starts).

## Where the pieces are

| Piece | Source | Installed |
| --- | --- | --- |
| kernel driver `amdgpu_drm` | `src/forks/linux-drm`, branch `airos-6.18.52` (https://github.com/airos-works/linux-drm) | `~/config/non-packaged/add-ons/kernel/drivers/bin/amdgpu_drm`, links in `dev/dri` and `dev/graphics` |
| firmware | linux-firmware `amdgpu/polaris10_*` | `~/config/non-packaged/data/firmware/amdgpu` |
| accelerant `amdgpu_kms` | this tree, `src/add-ons/accelerants/amdgpu_kms` | haiku.hpkg |
| VA-API media add-on | this tree, `src/add-ons/media/plugins/amdgpu_va` | `~/config/non-packaged/add-ons/media/plugins/amduvd` |
| libdrm | `src/forks/libdrm`, `airos-2.4.134` (https://github.com/airos-works/libdrm) | `/boot/system/non-packaged/lib` |
| libva | `src/forks/libva`, `airos-2.24.1` (local) | `/boot/system/non-packaged/lib` |
| libglvnd | `src/forks/libglvnd`, `airos-1.7.0` | `/boot/system/non-packaged/lib` |
| Mesa (radeonsi, RADV, VA) | `src/forks/mesa`, `airos-amdgpu` | `/boot/system/non-packaged/lib`, EGL vendor and Vulkan ICD files in `/boot/system/non-packaged/add-ons/{opengl/egl_vendor.d,vulkan/icd.d}` |
| static VA-API FFmpeg (for the add-on) | `src/forks/ffmpeg`, `airos-6.1.6` | built on the workstation in `/boot/home/gpu-userland/ffmpeg-va` |

The user land libraries carry an RPATH to `/boot/system/non-packaged/lib`, as
some programs (Summit) set a LIBRARY_PATH without it.

### The native driver (removed)

Images built before 2026-10-13 carry the native `amdgpu` driver (in
`dev/graphics`) and its `amduvd` decoder, which must never start beside
amdgpu_drm. There `/boot/system/settings/packages` hides them (the X399 has
it, and the amdgpu_drm package's post-install script writes it when it finds
the native driver):

```
Package haiku {
	BlockedEntries {
		add-ons/kernel/drivers/dev/graphics/amdgpu
		add-ons/kernel/drivers/bin/amdgpu
	}
}
Package haiku_datatranslators {
	BlockedEntries {
		add-ons/media/plugins/amduvd
	}
}
```

The VA-API add-on is installed under the same name, `amduvd`, as airTime and
Summit load AMD decoders by that name (decoder 0 H.264, 1 HEVC, 2 VP9,
3 AV1). VP9 needs VCN 1 or later and AV1 VCN 3 or later: on the WX 5100
(UVD 6) those two fail in Setup() or at the first picture, and the
programs decode them in software.

## Building and installing

From the host, in `src/forks/linux-drm`:

```
make -f haiku/build/Makefile -j32 all OUT=$PWD/out-release   # release driver
haiku/tools/x399-install.sh                                  # driver, settings, blocklist
haiku/tools/x399-gpu-userland.sh sync                        # sources to the workstation
```

The user land builds on the workstation (`/boot/home/gpu-userland/build.sh`,
uploaded by `x399-gpu-userland.sh`); for the system install:

```
cd /boot/home/gpu-userland && cp build.sh build-run.sh
GPU_PREFIX=/boot/system/non-packaged MESA_PLATFORMS=haiku,wayland \
	MESA_VULKAN_DRIVERS=amd MESA_GALLIUM_VA=enabled \
	./build-run.sh libdrm libva glvnd mesa-configure mesa
```

Without `GPU_PREFIX` it builds into `/boot/home/gpu-userland/prefix` for
trials (`tests/env.sh` runs programs against it; libva then also needs
`LIBVA_DRIVERS_PATH`, Vulkan `VK_ICD_FILENAMES`). Never install over libraries
a running program has loaded: check with `listimage` first.

The media add-on: `docs/x399-workstation/tools/build-amdgpu-va-plugin.sh` in
this tree (builds on the workstation, installs as `amduvd`). The system
packages of a build: `tools/install-system-packages.sh [build dir]`. Power
cycle afterwards, never a warm reboot.

A running build keeps one of its devices open, so devfs does not reload it
when its binary changes: `x399-install.sh` moves a new build over it in
place, and it starts at the next boot. Only a running build from before
that (no "keeping /dev/dri/renderD128 open" in it) still has the new one
wait in `/boot/home/amdgpu-drm/amdgpu_drm.new` for the `x399-driver-safety`
boot job - whose swap devfs may handle after it has loaded the new build,
reloading the started GPU (see above). The upload goes outside the drivers'
directories first: devfs starts any new file there as a second driver for
the GPU.

## Another machine (other AMD GPUs)

The driver is Linux's whole amdgpu (GFX 6 to 12, Display Core up to DCN 4.01,
SMU 14, VCN 5): any AMD GPU or APU Linux 6.18 drives should start, given its
firmware. Only the WX 5100 has been tried.

The air/OS x86_64 images the build server makes (airos-works/airos-ci) carry
all of it since 2026-10-13: the `amdgpu_drm` and `amdgpu_firmwares` packages
in `system/packages`, and the user land and the amduvd add-on, cross-built
from the same forks, in `system/non-packaged/amdgpu`. The image's boot script
(`tools/airos/ci/boot/UserBootscript`) links that user land into
`system/non-packaged` where amdgpu_drm drives a GPU and removes the links
elsewhere, as its libGL and libEGL would take OpenGL from every other
machine (Zink on NVIDIA cards). See airos-ci's `docs/x86_64-gpu.md`.

For a machine that is already installed, the pieces, built by
`src/forks/linux-drm` (copies in `/mnt/HaikuWork/artifacts/amdgpu`):

- `amdgpu_firmwares-<date>-1-any.hpkg` (`haiku/tools/package.sh`): every
  amdgpu firmware file of linux-firmware, 674 of them;
- `amdgpu_drm-<version>-1-x86_64.hpkg` (same script): the driver, requiring
  the firmware and at least the haiku it was linked against; on an
  image that still has the native amdgpu and amduvd, its post-install
  script blocks them;
- `amdgpu-userland-<date>.tar.gz` (`gpu-userland-build.sh bundle` on a
  machine that has built the user land): libdrm, libva, libglvnd, Mesa
  (radeonsi, RADV, VA driver) and the amduvd add-on for
  `/boot/system/non-packaged`.

On the machine: `pkgman install ./amdgpu_firmwares-*.hpkg ./amdgpu_drm-*.hpkg`,
`tar -xzf amdgpu-userland-*.tar.gz -C /`, and a cold boot (twice the first
time: the blocklist the post-install script writes takes effect at the boot
after). An unknown chip that brings the machine down at its first start is
left to the framebuffer driver on the next boot (the start marker).

## Tests

In `src/forks/linux-drm/haiku/tests`, built on the workstation:

- `amdgpu_smoke`, `libdrm_amdgpu_test`: buffer objects, mappings, command
  submission, from the kernel interface up
- `kmstest`: modes, frame buffers and page flips on the KMS interface
- `kmsforce [CONNECTOR off|on|detect]`: a monitor unplugged or plugged in
  without anybody at the machine (`DRM_HAIKU_IOCTL_FORCE_CONNECTOR`)
- `amdgpu_churn [seconds] [pool]`: buffer objects made, moved and freed
  with no GPU work of their own (VRAM moves, evictions, clears)
- `amdgpu_mapmove [rounds] [--read-only]`: mapped VRAM buffers pushed out
  of the CPU-visible window; checks what the old mappings read and that
  what they write lands in the buffer (`--read-only` is safe on a driver
  whose mappings go stale)
- `fakemap [seconds]`, in a virtual machine (`haiku/tools/qemu-vm.sh`,
  "haiku_selftest 2" in the driver settings): the same for the fake
  device's buffers, whose "move" poisons the old pages - clones, fork(),
  areas deleted by the team, moves racing with writes and new mappings
- `amdgpu_userptr [rounds]`: host memory (a clone of a `B_NO_LOCK` area)
  imported and written by the GPU, refusals, and children killed while the
  GPU writes into their memory; `fakeuserptr` does the same in the virtual
  machine with the fake device
- `drmparam NAME VALUE`: a module parameter of the running driver
  (`DRM_HAIKU_IOCTL_SET_PARAM`); `drmparam debug 0x16` around a refused
  mode set logs amdgpu_dm's reason (and fills the syslog quickly),
  `drmparam debug 0` stops it
- `vaprobe`, `vadecode`: VA-API profiles; per-picture MD5s to compare with
  `ffmpeg -f framemd5` (media and MD5s in `tests/media` on the workstation)
- `vabench`, `amdgpu_alloc_bench`, `amdgpu_map_bench`: where readback time goes
- `vkwayland`: a Vulkan swapchain in a window

`docs/x399-workstation/tools/gpu-soak.sh [seconds]` runs all of it at
once and in the ways programs end, then checks the syslog, kernel memory
and that pictures still decode exactly.

and in `x399/tests`: `vkprobe`, `vkbench` (Vulkan), and Summit's
`gles2-draw-probe` (GLES through EGL). `AMDGPU_VA_TIMING=1` makes the media
add-on print its per-picture costs.

## Traps

- **Two drivers for one GPU bring the machine down.** Keep the native
  driver blocked, and never stage a file inside a drivers directory.
- **devfs reloads a driver it finds through a second link.** A driver that
  none of its devices holds open is stopped and started again (in devfs'
  driver lock) once devfs takes its binary for changed, which a scan of
  any other directory linking the same binary does. Link a driver from one
  devfs directory; publish other paths from it.
- **No sync in devfs' driver lock.** `init_driver()` and `uninit_driver()`
  run in it; `_kern_sync()` waits for every volume, network ones included,
  whose mounting may wait for devfs.
- `amdgpu` thinks a zeroed `boot_cpu_data` is an Intel CPU, decides the host
  cannot change PCIe speeds and leaves the link in training at 2.5 GT/s
  (BAR writes at 9 MB/s): the compat layer fills it from CPUID.
- The kernel debug level 2 checks page ownership (DEBUG_PAGE_ACCESS): pages
  allocated on one thread and freed on another need the access handed over.
- Mesa needs a futex for its simple mutexes; Haiku has none, the compat
  `util/futex` emulates it.
- RADV maps buffer objects with `mmap()` on the device, which Haiku cannot
  do: on Haiku it uses `drmHaikuMmap()`.
- **A mapping has to follow its buffer.** TTM moves buffers whenever it
  likes (evictions, the CPU-visible window, back into VRAM for a command
  submission) and calls `unmap_mapping_range()` first; a mapping that keeps
  the old page frames reads and writes VRAM that belongs to another buffer
  by then, or RAM TTM has given back. Each `DRM_HAIKU_IOCTL_MAP` mapping is
  therefore an area on a cache of the driver's own (`BufferCache` in
  driver.cpp) whose `Fault()` runs the buffer's Linux fault handler, and
  the hook unmaps the moved pages from every area of it. The handler waits
  for the buffer's reservation, which TTM holds while it unmaps, so it runs
  with the cache unlocked and a generation number says whether the buffer
  moved meanwhile.
- Mesa made a new staging buffer for every `vaGetImage()`, and FFmpeg a new
  image for every picture: on this kernel the allocations cost more than the
  copies (1080p HEVC Main 10 33 -> 9.6 ms a picture once both were kept).
  Pictures up to 1080p are now decoded straight into system memory (Mesa's
  `vl_video_buffer_usage`, `VL_VIDEO_BUFFER_GTT=0/1` to force), which takes
  the GPU's slow writes to system memory out of the copy: 5.2 ms. 4K stays
  in video memory, where UVD reads its references faster.
- A Summit launched with its private zink Mesa finds no Vulkan driver for
  zink and every web process stops ("no EGL display"); Summit's
  `install-on-workstation.sh` now uses the system Mesa.
