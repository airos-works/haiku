# amdgpu_drm: the WX 5100 on Linux's amdgpu

Since 2026-10-12 the X399's Radeon Pro WX 5100 (Polaris 10, 1002:67c7) runs
on **amdgpu_drm**: Linux 6.18's amdgpu driver, with the DRM core, TTM, the GPU
scheduler and Display Core, built as a Haiku kernel driver over a Linux
compatibility layer. Everything above it is the stock Linux user land for AMD
GPUs: libdrm, Mesa's radeonsi (OpenGL, OpenGL ES, EGL), RADV (Vulkan) and
Mesa's VA-API driver. The same code is meant to drive other AMD GPUs later
(the Framework Desktop's Strix Halo, other Radeon cards); only firmware and
testing are per device.

The native driver for this card (`amdgpu`, [WX5100.md](WX5100.md)) is still in
the image; on the X399 it is blocked (below), as two drivers must never start
the same GPU.

## What works (verified on the X399, 2026-10-12/13)

| Area | Result |
| --- | --- |
| Display | app_server on the `amdgpu_kms` accelerant: both 3840x2160@60 DisplayPort monitors in one 7680x2160 frame buffer at 200 %, hardware cursor, vertical blank sync |
| OpenGL / GLES / EGL | radeonsi (ACO) for BGLView applications (GLTeapot) and EGL (Summit's compositor); `gles2-draw-probe` passes |
| Vulkan | RADV, Vulkan 1.4.328: `vkprobe` passes, `vkbench` compute 1.77 TFLOP/s, fill 33.5 Gpixel/s, VRAM copy 123 GB/s, host upload 5.7 GB/s |
| Vulkan in windows | `VK_KHR_wayland_surface` through Haiku's in-process Wayland server: 60.0 fps with vsync (FIFO), a 1920x1080 swapchain at 167 fps (mailbox) |
| Video decoding | UVD through VA-API: MPEG-2, VC-1, H.264, HEVC Main and Main 10, JPEG; VCE encoding for H.264 and HEVC. H.264, HEVC and HEVC Main 10 decode bit-exact against FFmpeg's software decoder |
| airTime | 1.0.0-18 (installed) plays H.264 and HEVC/Main 10 on the GPU ("AMD UVD (amduvd ...) [hardware]"): 1080p at 4-5 ms a picture, 4K at 30 fps without drops |
| Summit | renders with radeonsi and plays H.264 and HEVC on the GPU (bundle-ci0334, the CI build of Summit 803d1c9; see Summit's docs/wx5100-media.md) |

Known limits:

- Monitors coming and going (hot plug) and display power management are
  implemented in the accelerant but not yet tested with a monitor unplugged.
- Boot safety: the driver keeps a marker
  (`~/config/settings/kernel/drivers/amdgpu_drm.starting`) from its start
  until the GPU has run for 30 s; a boot that finds it leaves the GPU to the
  framebuffer driver until it is deleted. No real crash has exercised it yet.

- GPU writes into system memory reach 1.55 GB/s (reads 5.7 GB/s), which
  bounds readback: Summit's compositor reads every frame back, and 4K
  pictures are copied out of video memory. Ruled out on 2026-10-13: the
  link (held at 8 GT/s x8 during the copies; AER clean; root port and GPU
  both at 256-byte payloads with relaxed ordering), the copy engine (CP DMA
  and shaders alike) and snooping (non-snooped write-combined buffers,
  `haiku_uswc`, write no faster). Cause open; comparing with Linux on this
  machine would settle whether it is the platform.
- 4K at 60 fps decodes on the GPU but misses frames: the 8-bit YUV to RGB
  conversion takes 10 ms a picture.
- Vulkan presents by copying (no dma-buf on Haiku): GPU blit to host memory,
  copy into the window's shared buffer.
- Wayland clients (vkwayland) sometimes crash as they exit: the in-process
  Wayland server's window thread (`wayland_server` 0.1.20251004, HaikuPorts)
  runs on in `BWindow::task_looper()` after the program has unloaded the
  server's add-on. A roundtrip after destroying the window makes it rarer
  (1 in 15 runs). Not the GPU stack's; debug_server is set to write a
  report rather than stop with a dialog for the GPU test programs.
- No sysfs: power management can only be set with module parameters.

## Where the pieces are

| Piece | Source | Installed |
| --- | --- | --- |
| kernel driver `amdgpu_drm` | `src/forks/linux-drm`, branch `airos-6.18.52` (local) | `~/config/non-packaged/add-ons/kernel/drivers/bin/amdgpu_drm`, links in `dev/dri` and `dev/graphics` |
| firmware | linux-firmware `amdgpu/polaris10_*` | `~/config/non-packaged/data/firmware/amdgpu` |
| accelerant `amdgpu_kms` | this tree, `src/add-ons/accelerants/amdgpu_kms` | haiku.hpkg |
| VA-API media add-on | this tree, `src/add-ons/media/plugins/amdgpu_va` | `~/config/non-packaged/add-ons/media/plugins/amduvd` |
| libdrm | `src/forks/libdrm`, `airos-2.4.134` (local) | `/boot/system/non-packaged/lib` |
| libva | `src/forks/libva`, `airos-2.24.1` (local) | `/boot/system/non-packaged/lib` |
| libglvnd | `src/forks/libglvnd`, `airos-1.7.0` | `/boot/system/non-packaged/lib` |
| Mesa (radeonsi, RADV, VA) | `src/forks/mesa`, `airos-amdgpu` | `/boot/system/non-packaged/lib`, EGL vendor and Vulkan ICD files in `/boot/system/non-packaged/add-ons/{opengl/egl_vendor.d,vulkan/icd.d}` |
| static VA-API FFmpeg (for the add-on) | `src/forks/ffmpeg`, `airos-6.1.6` | built on the workstation in `/boot/home/gpu-userland/ffmpeg-va` |

The user land libraries carry an RPATH to `/boot/system/non-packaged/lib`, as
some programs (Summit) set a LIBRARY_PATH without it.

### The native driver is blocked

The x86_64 image carries the native `amdgpu` driver (in `dev/graphics`) and
its `amduvd` decoder. On the X399 `/boot/system/settings/packages` hides them:

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
Summit load AMD decoders by that name (decoder 0 H.264, 1 HEVC).

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

`x399-install.sh` never replaces the binary of a loaded driver: devfs would
reload it underneath itself. A changed build waits in
`/boot/home/amdgpu-drm/amdgpu_drm.new` and the `x399-driver-safety` boot job
swaps it in before app_server starts. Nor may a new file be put into a
drivers directory: devfs starts it as a second driver for the GPU.

## Tests

In `src/forks/linux-drm/haiku/tests`, built on the workstation:

- `amdgpu_smoke`, `libdrm_amdgpu_test`: buffer objects, mappings, command
  submission, from the kernel interface up
- `kmstest`: modes, frame buffers and page flips on the KMS interface
- `vaprobe`, `vadecode`: VA-API profiles; per-picture MD5s to compare with
  `ffmpeg -f framemd5` (media and MD5s in `tests/media` on the workstation)
- `vabench`, `amdgpu_alloc_bench`, `amdgpu_map_bench`: where readback time goes
- `vkwayland`: a Vulkan swapchain in a window

and in `x399/tests`: `vkprobe`, `vkbench` (Vulkan), and Summit's
`gles2-draw-probe` (GLES through EGL). `AMDGPU_VA_TIMING=1` makes the media
add-on print its per-picture costs.

## Traps

- **Two drivers for one GPU bring the machine down.** Keep the native
  driver blocked, and never stage a file inside a drivers directory.
- `amdgpu` thinks a zeroed `boot_cpu_data` is an Intel CPU, decides the host
  cannot change PCIe speeds and leaves the link in training at 2.5 GT/s
  (BAR writes at 9 MB/s): the compat layer fills it from CPUID.
- The kernel debug level 2 checks page ownership (DEBUG_PAGE_ACCESS): pages
  allocated on one thread and freed on another need the access handed over.
- Mesa needs a futex for its simple mutexes; Haiku has none, the compat
  `util/futex` emulates it.
- RADV maps buffer objects with `mmap()` on the device, which Haiku cannot
  do: on Haiku it uses `drmHaikuMmap()`.
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
