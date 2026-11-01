# Mali-G610 development

GPU support is the owner's current scope; network stack changes and all other
hardware development are deferred. The working EFI framebuffer supplies a
desktop, while native
display control and GPU rendering remain separate milestones. Haiku now boots
the Mali firmware and passes four compute-shader submissions plus four CS
memory-store regressions across two +195 native boots. Complete data matches
the Linux reference. A separate Linux Mesa/Panfrost reference now renders four
checked images on the board. The +210 image now runs the same workload through
[native Haiku Mesa/Panfrost](MESA.md): all 32,768 pixels across two boots match
Linux, all 32 Mesa submissions complete, and allocations return to baseline.
Pbuffer/EGL lifetime checks, previous GPU regressions and recovery pass.
The +215 image adds [EGL windows](MESA-WINDOW.md), +217 adds normal
[OpenGL Kit views](MESA-GLVIEW.md), and +219 qualifies six
[GLES pipeline operations](MESA-PIPELINE.md), including textures, depth,
stencil, blending and render-to-texture. All complete pixels, cleanup and
recovery checks pass. Conformance and native display control remain open.
The +221 image also qualifies [graphics-process cleanup](MESA-LIFETIME.md):
terminate a process after finished GPU work with its resources open, render
again in the survivor, then verify fresh-context reuse and complete cleanup.
Both native boots pass all 51,992 pixels, allocation baselines and recovery.
The +198 and +200 images qualify persistent client buffers and GPU VM mappings.
The +202 image activates those VMs for persistent application queues: two
native boots accept 1,264 submissions and check 1,136 completions, including eight
compute shaders. Queue/context state, VM replacement, process cleanup and recovery
pass. The +204 image adds shared binary/timeline fences and queue dependencies,
with 210 further submissions across two native boots: 202 checked completions
and eight expected cancellations. The +207 image qualifies native tiler heaps,
including GPU data readback, queued address reuse and process cleanup. Across both
boots all regressions accept 1,696 submissions and check 1,432 completions.
The +224 image qualifies [actual firmware-requested heap growth](MESA-HEAP-PRESSURE.md):
24 extensions across four native contexts, complete pixels and allocation
cleanup, all prior regressions and recovery/integrity checks pass. Refused
allocations and incremental rendering are now qualified by the
[+227 fixed-heap image](MESA-HEAP-LIMIT.md): 36 actual refusals, 36 incremental
passes and all 51,992 pixels across two native boots pass, with full cleanup,
recovery and integrity. Fault recovery remains open.

The +243 USB image qualifies [reset notification for live Mesa contexts](MESA-LOSS.md)
on two native boots. Both shared contexts receive one loss notification, ignore
further rendering and close completely; fresh contexts then render without a
Haiku reboot. All 181,972 before/fresh pixels, 3,584 guards and 104,496 ignored
readback bytes pass. Both QEMU modes, earlier regressions, normal recovery and
independent integrity checks pass. The earlier failed trials are preserved.
General application compatibility, arbitrary hangs and native display control
remain open.

The +238 USB image qualifies [bounded GPU command-fault recovery](MESA-RECOVERY.md)
on two native boots. Three affected jobs receive errors per boot; reset,
address-space cleanup and platform restoration are verified. After all affected
clients close, fresh native queues and Mesa contexts work without a Haiku
reboot. All 155,976 subsequent pixels and 3,072 guards pass, together with
earlier regressions, both QEMU modes, normal recovery and independent integrity
checks. The separate +243 fixture above qualifies retained Mesa-context
notification; arbitrary hangs and native display control remain open.

The +236 USB image qualifies [termination during pending graphics work](MESA-PENDING.md)
on two native boots. Both a fence timeout and the driver's actual close-time
queue state establish unfinished work. Survivor rendering, fresh-process reuse
and allocation cleanup pass; all fourteen completed frames, 90,986 pixels and
1,792 guards match. Earlier graphics regressions, both QEMU modes, normal
recovery and independent integrity checks pass. Active work can complete while
close waits; immediate preemption and GPU fault/reset recovery remain open.

The +234 USB image qualifies [concurrent graphics applications](MESA-CONCURRENCY.md)
on two native boots. All 128 paired draw rounds overlap; all 260 frames,
1,689,740 pixels, 33,280 guards and 532 submissions pass. Normal process
retirement, survivor rendering, fresh-process reuse and allocation cleanup
pass. Both QEMU modes, earlier graphics regressions, normal recovery and
independent integrity checks pass. Each graphics fixture logs to RAM before
its checked transfer. Pending-work termination, GPU fault/reset recovery and
native display control remain open.

The +232 USB image qualifies [sustained GPU rendering](MESA-SUSTAINED.md)
on two native boots. Four retained contexts each complete at least sixty
seconds of measured rendering with stable tiler-heap use. All 91,648 frames,
595,620,352 pixels, 11,730,944 guard bytes and 183,304 submissions pass; fixed
heaps complete 147,196 incremental passes. Earlier graphics fixtures, both
QEMU modes, normal reboot/shutdown, recovery and independent integrity checks
pass. Full logs are captured in RAM and retrieved with checked, paced transfers.
Pending-work termination, GPU fault/reset recovery and native display control
remain open.

The system-default OpenGL candidate ([MESA-SYSTEM.md](MESA-SYSTEM.md), Mesa
at `3139063445` on the qualified +256 image) runs GLTeapot on Mali with no
launch environment on two native boots: the Mesa backend takes the Mali
device and its firmware from installed defaults, the CPU polygon path is on
by default, and the GLVND/Mesa libraries and EGL vendor file are installed
system-wide. Every earlier Mesa fixture passes on the same boots and the
emulator gates show the software fallback without a device. It is
qualified (2026-09-19, `…automated-mali-system-opengl/20260919T124336Z-d49da9`)
with the independent Linux eMMC readbacks now run from the owner's SD-card
Debian, the lab's new recovery OS.

The +300 USB image adds a read-only [observation of the second connector's
DisplayPort path](DISPLAY.md) to the qualified desktop-cursor cycle: on
two native boots the DisplayPort TX1 block is powered, clocked and idle
with its PHY interface parked, the USBDP PHY1 is in low power, and the
hot-plug pin is a plain input reading low with nothing on the connector;
every word agreed between samples and boots. Bringing that path up (PHY,
link training through the RA620 bridge, a second video port) needs a
monitor or an HDMI dummy plug on the second connector, which is not
attached.

The +297 USB image qualifies [app_server's pointer on the hardware cursor](DISPLAY.md)
on two native boots: on the desktop cursor profile the accelerant exports
the cursor hooks, app_server hands its 22x22 pointer to the driver's second
VOP2 window at start and stops drawing it into the frame buffer, the window
reads back at the desktop's centre, survives the 720p/1080p mode changes
and DPMS, and the NanoKVM captures show Haiku's hand pointer there; the
probe's own placements, clipping, hide and restore of app_server's bitmap
pass on the same boots. The HDMI1 "2D" set (EDID, own frame buffer,
retrace, mode changes, DPMS, hardware cursor) is complete; drawing itself
is not accelerated, and the second (DisplayPort-bridged) HDMI port and a
spanning desktop remain open.

The +294 USB image qualifies a [hardware cursor on HDMI1](DISPLAY.md) on
two native boots: on the probe-only cursor profile the driver programs a
second VOP2 window (ESMART3, blended by the port's alpha mixer) with a
64x64 straight-alpha bitmap over the desktop window, clips it at every
edge, hides it and restores app_server's state, with every window register
read back and the NanoKVM captures showing the white, black, transparent
and half-transparent quadrants where expected and the desktop untouched.
The cause of an earlier dark frame is recorded: the firmware runs the
desktop window with the AXI read ids Linux assigns to ESMART3, so the cursor
window takes ids derived from the desktop window's. app_server still draws
its software pointer on this image; handing its pointer to the window (the
desktop cursor profile) is the next stage, and the second
(DisplayPort-bridged) HDMI port remains open.

The +285 USB image qualifies [DPMS power control on HDMI1](DISPLAY.md) on
two native boots: through the accelerant's DPMS hooks the driver stops the
video port and powers the HDPTX PHY down, so the sink loses its signal, the
frame-start interrupts stop and the NanoKVM has no frame to capture, and
brings the current mode back with the full mode set in under a millisecond,
with retrace at 60 Hz again and the desktop restored. The 720p/1080p mode
changes and the earlier read-only, EDID, scanout-swap, accelerant and
retrace checks pass on the same boots, and app_server's start-up DPMS-on
request is answered as a no-op. A cursor window and the second
(DisplayPort-bridged) HDMI port remain open.

The +282 USB image qualifies [native mode changes on HDMI1](DISPLAY.md) on
two native boots: with the opt-in mode-set profile the driver switches the
port app_server draws to from the firmware's 1920x1080@60 to 1280x720@60 and
back, reprogramming the HDPTX PHY PLL and lanes, the VOP2 port timing and
window, and the AVI infoframe, while the accelerant reports each mode and
its retrace keeps running at 60 Hz. The NanoKVM shows the 720p signal as the
desktop's top-left crop and the normal desktop after the return; normal
reboot, verified shutdown and recovery pass, and the earlier read-only,
EDID, scanout-swap, accelerant and retrace checks pass on the same boots.
A cursor window and the second (DisplayPort-bridged) HDMI port remained
open.

The +277 USB image qualifies the [board accelerant with vertical
retrace](DISPLAY.md) on two native boots: app_server runs on
`rk3588_display.accelerant`, draws the desktop into a driver-owned contiguous
frame buffer that the live HDMI1 window scans at the firmware's 1920x1080
mode, reports the sink's EDID, and its retrace semaphore is released by the
VOP2 frame-start interrupt at 60.0 Hz. Normal reboot, verified shutdown and
recovery pass; the earlier read-only, EDID and scanout-swap checks pass on
the same boots. A cursor window, power control, a real mode change and the
second (DisplayPort-bridged) HDMI port remain open.

The +269 USB image qualifies the first [VOP2 write path](DISPLAY.md): with
the opt-in scanout profile the driver points the live HDMI1 window at its own
contiguous colour-bar buffer, commits the port and waits for the
configuration-done bit, and restores the firmware framebuffer, on two native
boots. The NanoKVM capture shows the bars during the 30 s hold and the normal
desktop afterwards, the observation is unchanged across the swap, and normal
reboot, verified shutdown and recovery pass. The first candidate (+267)
verified the address before the port's next frame start and is retained.

The +264 USB image qualifies the [EDID read](DISPLAY.md) over the HDMI TX1
I2C master on two native boots: with the opt-in profile the driver reads the
NanoKVM's 256-byte EDID (VCS `0x1145`, EDID 1.3 with one CEA-861 extension,
preferred 1920x1080 at 148.5 MHz) one byte per transfer in about 52 ms per
block, and the read-only observation before and after the read is identical.
Both QEMU modes, the Mali regressions, normal reboot, verified shutdown and
automatic recovery pass; the desktop was viewed on both boots. Native mode
setting and the second (DisplayPort-bridged) HDMI port remain open.

The +263 USB image qualifies the first [native display observation](DISPLAY.md)
on two native boots: the read-only `rk3588_display` driver admits the VOP2,
HDMI TX1, HDPTX PHY1 and control-block description and reads the firmware
display state without a register write. The firmware drives the HDMI1 port
from VOP2 video port 2 at 1920x1080 (2200x1125 total) through ESMART2 at
`0xed280000`; the HDMI1 hot-plug level, PHY lock and TMDS link are recorded.
Both QEMU modes, the earlier Mali regressions, normal reboot, verified shutdown
and automatic recovery pass. A first +259 run panicked reading a write-only
HDMI register and is retained.

The +256 Mesa fix on the retained +254 Haiku kernel qualifies the bounded
[GLTeapot and polygon tests](MESA-APPLICATION.md) on two native boots. All 128
polygon frames, four normal GLTeapot launches, 32 reviewed application frames
and 8,732 completed GPU submissions pass. Retiring the shared CPU interpreter's
cached shader binding fixes missing immediate-mode edges/points and quad culling.
Earlier GPU regressions, both QEMU modes, normal recovery and independent
integrity checks pass. The renderer remains private, with opt-in CPU polygon
geometry and Mali rasterization; general OpenGL compatibility, system integration
and native display control remain open.

## Reference and integration route

ROOBI runs Linux `5.10.110-33-rockchip` and Mesa 20.3.5. Its vendor device tree
describes a 2 MiB GPU register window at `0xfb000000`, three interrupts,
SCMI/CRU clocks, a regulator and a power domain. The old Panfrost module fails
while parsing vendor operating points. Initial inventory finds no GPU driver,
no Mali device and an off power domain. The DRM nodes belong to the display
controller and NPU; they do not prove Mali rendering. The installed
`bifrost_kbase` CSF module panicked in the vendor power-domain driver while
enabling the GPU domain. Automatic recovery and independent storage checks
passed; that vendor trial remains failed.

A separate Linux 6.18.52 EFI image boots from a built-in RAM filesystem on
the actual board. Mainline Panthor initializes the Mali-G610 and official
arch10.8 firmware, reports CSF interface 1.5.0, and passes GPU/interface queries
and empty GPU-VM creation/destruction. The GPU ID is `a8670005`, MMU features
`2830`, shader mask `50005`, with eight address spaces and eight CSF group slots.
The test then reboots normally to ROOBI. Its boot image, recovery image and eMMC
integrity checks pass, and the NanoKVM watchdog disarms. This established the
initial firmware baseline; that image tested no GPU job submission or rendering.
Linux used temporary ignore-unused clock/regulator/power-domain settings for
this reference.

The subsequent command reference passes four native CSF memory-store submissions
across two separately created VM/group lifecycles. Mesa 25.3.6's architecture-10
packer generates two 256-byte command streams. Each writes eight distinct values
into an 8 KiB buffer, waits for stores, and cleans GPU caches. The command and
data buffers use GPU virtual addresses above 4 GiB; their physical addresses were
not sampled. CPU mappings use the Linux driver's noncoherent write-combining path.

Before every submission, a fresh unsignaled fence must time out. After submission,
the fence must complete within five seconds, group/fatal/VM status must remain
clear, and every output word must match. An independent decoder checks the actual
stream, all 2,048 initial and final data words, and the entire unchanged 4 KiB
command allocation on each round. The eight changed words include both sides of
a page boundary; all 2,040 guard words remain intact. Both cycles complete group
destruction, unmapping, object closing and VM destruction successfully. No shader
or rendering instruction is present.

The reference's compiled CPU oracle matches 16,384 independently calculated
values; 18 damaged streams and 22 altered evidence cases are rejected. Its ELF
and RAM-image build pass. EL2 QEMU with the `max` CPU and EL1 with `cortex-a76`
pass EFI/RAM-init checks; neither emulates Mali. The initial EL1 `max` attempt
hit QEMU's own `regime_is_user` assertion before init. That failed emulator
configuration remains recorded; the Cortex-A76 repeat matches the board's large
core model. The native run records no GPU/MMU fault, reboots normally to ROOBI,
disarms the watchdog, and passes independent complete boot/recovery-image and
eMMC integrity checks. This supplies the reference for Haiku's qualified queue
implementation below.

EDK2's mainline tree uses `rockchip,rk3588-mali` / `arm,mali-valhall-csf`.
The actual Haiku device tree was captured on two +179 boots and is identical:
184,025 bytes, SHA-256
`d28f6e039a228ba655bc7e804c5fddaa843ec82d4cf965d79eec27972e74e574`.
Its GPU, CRU, PMU, GIC, clocks, IRQs and supply description match the new
resource admission profile. PCB revision is
v1.12; the public v1.11 schematic alone does not validate new regulator or
wiring assumptions. Do not read a powered-off GPU speculatively.

Mesa 25.3.6 has a Haiku EGL frontend and a Panfrost kernel-backend layer.
The original Haiku frontend selects software rendering. The Panthor path uses
DRM buffer objects, virtual mappings, synchronization objects, groups, queues
and tiler heaps; some CSF operations are outside the common backend abstraction.
The ABI assessment must include those direct calls. Enabling a Mesa build
option alone cannot provide Haiku GPU support.

## Compute shader reference

The Linux 6.18.52 RAM reference now passes four compute-shader submissions
across two separately created VM/group lifecycles. A 216-byte, 27-instruction
Valhall program loads a 64-bit data address and eight values from FAU uniforms,
stores those values at the existing guarded offsets, waits after each store,
and terminates. Mesa 25.3.6's assembler and generated disassembler reproduce
the same assembly. Its architecture-10 packer produces both dispatch streams,
the shader-program descriptor and the local-storage descriptor.

The entire read-only, executable 4 KiB allocation contains two 256-byte command
streams, one shared shader, descriptors and separate uniforms for each round.
Each dispatch explicitly requests a compute resource and launches one workgroup
containing one invocation. There is no CS `STORE_MULTIPLE` instruction: the
shader performs all eight data writes. The program uses neither a resource
table nor thread-local or workgroup memory. Endpoint completion, cache cleaning
and a wait precede the kernel's completion fence.

The independent decoder checks every byte of that allocation, including its
padding, and rejects 4,102 altered code variants. Twenty-four altered evidence
cases are also rejected. The compiled CPU oracle agrees on all 16,384 initial
and expected values. These host checks are separate from native execution.

On the board, every fresh fence first times out as unsignaled, then completes
after submission. All four complete initial and final 8 KiB data buffers match:
eight changed words and 2,040 unchanged guards per submission. The complete
4 KiB code allocation is unchanged. Group/fatal/VM state stays clear, and
group destruction, unbinding, CPU unmapping, object closing and VM destruction
pass. The two cycle-zero output hashes match the earlier CS memory-store
reference. Logged submit/wait intervals are 1.00–1.39 ms; they are not a rendering
benchmark. GPU virtual addresses exceed 4 GiB; physical addresses were not measured.

Both QEMU boot modes, normal RAM-root reboot, watchdog disarming and independent
boot-image, recovery-image and eMMC integrity checks pass. The used image was
archived locally and removed from the NanoKVM after verification. No rendering
or display acceleration is claimed by this compute test.

The first shader attempt omitted the compute-resource request and its first
submission timed out. Adding the request emitted by Mesa's `csf_init_batch`
allowed all four submissions to pass with the same shader binary. The failed
image, serial capture and recovery evidence remain preserved. Its controller
incorrectly described the failure as GPU initialization; GPU/CSF queries had
passed. The corrected controller also saves one checked pre-boot ID instead of
discarding a successful read and issuing a second one.

| Shader reference evidence | Location or value |
| --- | --- |
| Qualified Linux trial | `/mnt/HaikuWork/artifacts/native-linux-shader-reference/20260915T032912Z-477f93/qualification.json` |
| Source, generator and oracle | `/mnt/HaikuWork/artifacts/mali-shader-reference/20260915T032340Z-304bcf` |
| Failed first trial and review | `/mnt/HaikuWork/artifacts/native-linux-shader-reference/20260915T031925Z-de6738/failure-review.json` |
| Shader SHA-256 | `9ef33d01429d1954b6eebadd6258ac91103c3a12edb96319872f662f579fb09f` |
| Complete 4 KiB code SHA-256 | `dcc72b3bb9448e0acda21fa9dfe6052e83856460608b57c114552d1f1aa0d669` |
| Linux image manifest | `/mnt/HaikuWork/artifacts/efi-media/20260915T032602Z-deaa36/linux-shader-ram-reference.json` |

## Mesa rendering reference

Mesa 25.3.6 and libdrm 2.4.123 now build for AArch64 Linux with only the Panfrost
Gallium driver. The native shader tools are built separately with LLVM 18.1.3;
LLVM is not a dependency of the target libraries. Source archives, package
versions and checksums, build options, generated tools and every runtime ELF
are recorded. The [Panfrost build instructions](https://docs.mesa3d.org/drivers/panfrost.html)
describe this separation of native compiler tools from the cross build.

Linux 6.18.52 boots the complete runtime from its built-in RAM filesystem.
The probe selects the EGL device whose DRM node answers as Panthor with GPU
ID `a8670005` and shader mask `50005`. It reports `Mali-G610 (Panfrost)` and
`OpenGL ES 3.1 Mesa 25.3.6`. There is no software Gallium driver in this build.

Each of two separately initialized EGL/context lifecycles creates an RGBA8
texture framebuffer. A GLSL vertex shader positions two overlapping rectangles,
each made of two triangles; a fragment shader writes uniform colors. Two frames
per context change the geometry and palette. All four full 64-by-64 RGBA images
match the independent pixel oracle: 16,384 pixels, 65,536 pixel bytes and 512
CPU readback guard bytes. The decoded PNGs were also inspected. The oracle
rejects 28 altered or incomplete host fixtures; those fixtures are explicitly
marked synthetic and are not native evidence.

The probe counts the actual DRM ioctl calls from itself and the Mesa/libdrm
libraries. All 22 observed request types return without errors. The counters
show matching creation/destruction of two VMs, two groups, two tiler heaps,
36 buffer objects and 12 synchronization objects. There are ten group-submit
calls, 72 VM-bind calls and 66 CPU-mapping-offset queries. These count requests,
not the number of elements in request arrays. Six handle-to-FD and six
FD-to-handle conversions, ten binary waits, 74 timeline waits and eight
synchronization transfers also succeed. This does not qualify cross-process
buffer sharing, process termination during work, or GPU fault recovery.

`PAN_MESA_DEBUG=sync` makes Mesa wait for each submitted job and check faults.
Consequently all four application fence waits already see a signaled fence;
their 5.5–9.1 microsecond intervals are not rendering benchmarks. GPU physical
addresses were not recorded. This test does not exercise display scanout,
native Haiku rendering, API conformance or sustained performance.

The initial EL1 and EL2 QEMU boots caught an image-recipe error: the ELF
interpreter had mode `0444`, so the dynamic probe failed with `EACCES`.
Giving that interpreter execute permission fixes both modes. The original
image and both failed logs remain preserved; no native attempt used it.
The corrected QEMU boots load the target Mesa libraries and correctly report
the absent Mali GPU.

The native run renders all four frames, destroys both contexts and reboots
normally to ROOBI. The watchdog disarms; independent complete boot/recovery
image hashes, eMMC filesystem/files and three reference regions pass. ROOBI
again logs its vendor VOP2 power-domain timeout and display IOMMU message;
matching messages occur in three preceding recovery captures. That separate
display issue is recorded in `recovery-display-review.json`. The used image
was archived locally, reverified and removed from the NanoKVM, restoring
430,768,128 bytes of free space.

| Rendering reference evidence | Location or value |
| --- | --- |
| Qualified native trial | `/mnt/HaikuWork/artifacts/native-linux-mesa-reference/20260915T043315Z-087ca7/qualification.json` |
| Source, dependencies, build recipes and oracle | `/mnt/HaikuWork/artifacts/mali-render-reference/20260915T041113Z-a45ace` |
| Complete runtime/build snapshots | `/mnt/HaikuWork/artifacts/linux-mesa-reference-build/20260915T043102Z-ab66ee/build.json` |
| Image manifest | `/mnt/HaikuWork/artifacts/efi-media/20260915T043103Z-a355b0/linux-mesa-ram-reference.json` |
| Image SHA-256 | `3c0def89e5f3fc9a04d4236cf7d4fa43f06124f90fdcd9ecd6395b125124d742` |
| EL2 / EL1 QEMU | `artifacts/qemu-linux-mesa-reference/20260915T043150Z-15aeca` / `20260915T043150Z-d0a6c1` |
| Full readbacks and PNGs | `artifacts/native-linux-mesa-reference/20260915T043315Z-087ca7/frames` |
| Initial image failure review | `artifacts/mali-render-reference/20260915T041113Z-a45ace/first-qemu-failure-review.json` |

### Haiku userspace interface work

The first persistent client/buffer implementation passes two native +198 boots. Each open
owns a client and buffer handles. The native ABI supports allocation, information,
whole-buffer CPU mappings and handle destruction. Handles are never reused during
a module lifetime, duplicate descriptors share their client, and inherited
descriptors cannot operate another team's client. Close/process teardown releases
kernel-owned buffers; mapped CPU areas independently retain the backing RAM.
Allocation or map copyout failures roll back their new objects.

Buffers use scattered, locked RAM with every physical page checked against the
GPU's 40-bit limit. Cached allocator clearing is evicted before installing
Normal-NC CPU mappings. RAM area clones and fork copies now preserve the source
memory type. The host fixture reproduces the old clone failure and passes after
the correction. A 64 MiB per-buffer, 256 MiB per-client and 512 MiB global limit
bounds kernel-owned allocations; CPU areas surviving handle destruction belong
to the VM system and are outside those handle counters. There are at most 128
buffer handles per client and 64 open clients.

The development interface requires root, `O_RDWR` and the existing explicit
shader firmware profile. At +198 its only advertised capability is CPU buffers. All 141
host checks pass, including production allocation/handle/copyout cleanup, shared
mapping lifetime, limits and concurrent clients. The ARM64 build and both two-boot
QEMU modes pass. QEMU tests ordinary cached RAM clones/fork and correctly rejects
the absent Mali device; it does not emulate GPU buffer allocation.

Each native boot checks six buffer sizes from one byte to 4 MiB, rounded to pages,
with two independent CPU aliases. All bytes start at zero and match three written
patterns. A child inherits each mapping, reads its contents and writes a fourth
pattern visible to the parent; its inherited device descriptor correctly rejects
operations on the parent's client. Mappings remain readable after handle removal.
A separate mapping survives final descriptor close and a fresh allocation/write.
Four simultaneous child clients then leave 48 handles and CPU maps open. Two exit
normally and two receive SIGKILL. Driver counters and independently enumerated
kernel buffer-area counts return to their baseline. The whole sequence passes on
both boots. This tests process death without GPU work in flight.

Both boots also pass the existing CS memory-store and compute-shader regressions,
129,024 instruction-alias checks, 34 component and ten file hashes, unchanged FDTs,
and desktop inspection. Normal reboot, verified shutdown before media replacement,
automatic ROOBI recovery, watchdog disarming and independent recovery-image/eMMC
integrity pass. The recovery again logs the previously recorded vendor VOP2 timeout
and display-IOMMU message; native display support remains a separate open item.

The source is `740accbe2171a2e235fc5827d88707e0134e4d73`, image `hrev60097+198`, SHA-256
`765d82d680119accadee9a06970daf4931e2beec6825176204a40cde16eb7fed`.
Evidence beneath `/mnt/HaikuWork`:

- Build: `artifacts/build-20260915T051027Z.log`; 141 host checks in
  `artifacts/mali-client-buffers/20260915T045909Z-a9c5a6/host-checks.log`.
- Image: `artifacts/mali-client-image/20260915T051126Z-1cc917/manifest.json`.
- QEMU: `artifacts/qemu-shell/20260915T051208Z-4a04b2` (EL2) and
  `20260915T051208Z-591928` (EL1), each with `mali-client-result.json`.
- Native qualification: `artifacts/automated-mali-client/20260915T051508Z-975443/qualification.json`;
  transcripts and UART in `artifacts/interactive/20260915T051515Z-deb8f0`.
- Independent eMMC files: `artifacts/emmc-file-readback/20260915T052320Z-278854`;
  reference regions: `artifacts/emmc-read-reference/20260915T052337Z-1013ab`.

#### Persistent GPU address spaces

The +200 development interface adds per-client VM creation, information,
destruction and atomic batches of up to 64 map/unmap operations (`CsfVm.h`).
Maps cover page-aligned buffer subranges in the lower half of the 48-bit GPU
address space. Replacements and partial unmaps split ranges; adjacent compatible
ranges coalesce. GPU permissions distinguish read-only executable, non-executable
and uncached mappings. Writable executable mappings are rejected.

Each successful update builds an immutable four-level page-table generation from
scattered locked RAM, validates every physical page against the 40-bit limit and
then replaces the current generation. Allocation, validation and output-copy
failures preserve the previous generation. An optional generation check rejects
updates made against stale state. These operations prepare tables; they do not
activate an address space on the GPU.

Mappings own references to their buffers independently of buffer handles. The
kernel-only `ClientVmLease` pins a generation and all mapped buffers for future
scheduled work, surviving VM destruction and client closure until explicitly
released. Buffer counts and byte quotas include these resident allocations.
Limits are 16 VMs per client, 64 live VMs globally, 256 mappings and 1 GiB mapped
per VM, 1,024 table pages per generation, and 512 retained generations / 4,096
table pages globally. CPU aliases can still outlive the final driver reference.

Host tests independently decode the tables, compare 400 mapping batches with a
page-level reference model, hold generations across updates and client closure,
inject allocation/copy failures and exercise resident-buffer, mapping, VM,
generation and table-page limits. The native probe checks partial unmapping,
retention after handle removal, failed-update rollback and cleanup after normal
and forced process exits, with independent kernel-area counts. All 142 host
checks, the ARM64 build and both two-boot QEMU modes pass. QEMU verifies packaging
and absent-device rejection; it does not emulate Mali VM allocation or execution.

Two native +200 boots qualify the software VM lifecycle. A mapping crossing a
512 GiB boundary creates seven table pages. Replacing its middle produces three
ranges; partial unmaps release only the buffers no longer referenced. An invalid
second operation rolls back the entire batch. A readable but non-writable request
area forces the final copyout to fail after table construction; generation and
allocation counts stay unchanged. Both buffers remain resident after handle
removal until their mappings are removed. A CPU alias remains intact after the
last driver reference is released.

On each boot four child teams create 12 VMs and 48 buffers, then remove all
buffer handles while leaving the GPU mappings alive. The 12 table areas contain
120 pages. Two children exit normally and two receive SIGKILL. Driver counters
and independently enumerated buffer/table areas return to their baseline.
This native test submits no GPU work; retention of an actual in-flight GPU root
still needs qualification when the submission runtime is implemented.

Both boots retain the previous CPU-buffer, CS memory-store and compute-shader
regressions, 129,024 instruction-alias checks, 35 component and ten file hashes,
unchanged FDTs and inspected desktops. Normal reboot, verified shutdown before
media replacement, automatic recovery, watchdog disarming and independent
recovery-image/eMMC integrity pass. The recovery's vendor display, DMA-controller
and Bluetooth warnings also occur in the prior +198 recovery; their root causes
and native support remain open.

Source `0d021d4f18c21e6947a526420fdccf5d21d173d4`, image `hrev60097+200`, SHA-256
`44888d2bf8fc02805eb8e1298eb4e52ceef5e922e254504b1e161b42d5b64dcb`.
Evidence beneath `/mnt/HaikuWork`:

- Build: `artifacts/build-20260915T060548Z.log`; host checks and source snapshots:
  `artifacts/mali-client-vm/20260915T054424Z-369fef`.
- Image: `artifacts/mali-vm-image/20260915T060632Z-dbb3ea/manifest.json`.
- QEMU: `artifacts/qemu-shell/20260915T060713Z-eeac8e` (EL2) and
  `20260915T060713Z-e5fb57` (EL1), each with `mali-vm-result.json`.
- Native: `artifacts/automated-mali-vm/20260915T061005Z-0d4afd/qualification.json`;
  transcripts and UART: `artifacts/interactive/20260915T061016Z-a25491`.
- Independent eMMC files: `artifacts/emmc-file-readback/20260915T061759Z-985402`;
  reference regions: `artifacts/emmc-read-reference/20260915T061855Z-5b3b95`.

Mesa still needs persistent groups, queues, tiler heaps and synchronization
objects spanning many calls, with leases retained until queued work finishes.
The existing GPU diagnostics still start and stop their own firmware arenas.
Application GPU submission and Haiku rendering remain unimplemented.

`panthor_kmod.c` covers buffer/VM operations, but `pan_csf.c` directly creates
groups/heaps and submits work; `pan_fence.c` also calls DRM synchronization
functions. Adapting `pan_kmod_ops` alone is insufficient. The observed FD
conversions must be covered even for this offscreen workload; external sharing
needs separate tests. Query/capability results must describe implemented
behavior, and unsupported operations must remain explicit.

Haiku's ioctl wrapper supplies a zero length when the fourth argument is
omitted, and its area-based mappings differ from Linux DRM mmap offsets.
The userspace adapter must translate both operations deliberately. The Haiku
EGL frontend currently calls `sw_screen_create`; selecting a Panfrost screen
and supporting its presentation path are additional integration work. The
pinned source inventory is `mesa-abi-inventory.json` in the reference stage.

## Firmware component

`CsfFirmware.cpp` validates the container and describes ordinary MCU memory
regions without allocating or accessing hardware. It checks table/entry bounds,
page-aligned ranges, overlap, backing-data sizes, supported section flags and
the writable shared interface. Table length differs from full file length;
distinct virtual regions may reference the same file data. Protected regions
are counted and excluded from normal mappings.

The parser holds caller-owned immutable bytes. Section copies initialize all
backing memory, including padding. The future DMA layer must perform cache
maintenance before device access. File, section-count and mapped-memory limits
are explicit loader policy rather than format limits.

`rock5_mali_firmware_probe` uses this implementation, reports the layout and
prepares CPU copies. Its marker records `gpu_started=0`. The host probe accepts
the official arch10.8 image: format 0.3, version hash `01050000`, 282,624 file
bytes, 792 table bytes, seven ordinary regions totaling 917,504 mapped bytes,
one protected region and 16 ignored metadata records. All 129 host tests pass.
The new production-code fixture uses ASan/UBSan and guarded memory to check
truncation, unsupported records, reversed/overlapping ranges, valid aliases,
limits, unaligned input, copy bounds, zeroed padding and failed reinitialization.
The ARM64 build and two-boot EL1/EL2 QEMU firmware checks pass. The first native
CPU preparation check also passes, but that two-boot controller failed to
schedule reboot because its log directory was absent. The board shut down
cleanly and recovered with unchanged recovery/eMMC hashes. Corrected scripts
open their `/tmp` log before launching the delayed command. Both CPU checks
pass in the corrected two-boot +179 repeat, together with all component/file
hashes, reviewed desktops, normal reboot/shutdown and independent recovery/eMMC
integrity. The failed first controller run remains recorded separately.

## Native resource interface

The +181 kernel component binds the RK3588 CSF device only after validating
the board, named interrupts, clock provider, power controller and supply
description. It resolves phandles instead of fixing their numerical values.
It exposes a pointer-free, read-only snapshot at `graphics/mali_csf/0`, checked
by `rock5_mali_resource_probe`. The +181 implementation performs no MMIO, clock, regulator, power,
interrupt-handler or GPU-memory operation. Its diagnostic marker records
`gpu_accessed=0`; the supply range is a firmware description, not a voltage
measurement. It supplies no display accelerant.

The production FDT traversal and ioctl run in a host fixture covering changed
phandles, missing/wrong providers, incompatible boards, malformed cell/string
arrays, incorrect IRQ routes, parent-reference cleanup and ioctl bounds. Guarded
pages check truncated and unaligned input. All 130 host tests, the ARM64 build
and both two-boot QEMU modes pass. Native attachment and the exact resource
snapshot pass on two boots, with invalid ioctl length and write-open rejection,
32 component and nine file hashes, reviewed desktops, normal reboot/shutdown,
and independent recovery/eMMC integrity. QEMU correctly exposes no GPU node.
An earlier +180 image used the legacy driver packaging directory; pre-native
review caught this and +181 uses the modern graphics module directory.

## Clock and power observation

The +182 opt-in diagnostic, `rock5_mali_resource_probe --platform`, reads CRU
clock selectors 158–160 and gates 66–67, plus PMU GPU idle request/ack/status,
software power-down request and repair status. Offsets come from RK3588 TRM
v1.0 Part 1, dated 2022-03-09. PMU offsets account for the actual FDT window
starting at `fd8d8000`. The GPU repair-complete indication is bit 1 in
`PMU_BISR_STS4`; idle and software power-down requests use bit 0.

It maps only the required CRU and PMU pages, with kernel read-only permissions,
and releases both on every path. It never maps the GPU, writes a register,
changes voltage, installs an interrupt handler or enables power. The fixed
64-byte result includes observation start/end times and raw register values;
the reads are sequential, not atomic, and do not measure voltage or frequency.
The host fixture tests actual production read offsets and cleanup, failed
first/second mappings and malformed requests using guarded read-only memory.
All 130 host tests, ARM64 build, both two-boot QEMU modes and the native trial
pass. Three samples on each of two native boots return identical register
values; each observation took 2–3 microseconds. Both desktop captures were
reviewed. Normal reboot, verified shutdown before media replacement, Linux
recovery, disarmed watchdog and independent recovery/eMMC integrity all pass.
The first launcher attempt failed its Python dependency preflight before
deployment; the corrected launcher explicitly uses the NanoKVM virtual environment.

| Register group | Native value on both boots | GPU interpretation |
| --- | --- | --- |
| CRU selectors 158/159/160 | `00001f80 / 00000000 / 00000000` | Source index 4 (SPLL), divide by 1; GPU PVTPLL selection is off |
| CRU gates 66/67 | `00000004 / 00000000` | GPU clocks are enabled at the CRU; only the test-output gate is disabled |
| PMU idle request/ack/status | `00000000 / 00000fff / 00000fff` | GPU software idle request is clear; idle and acknowledge are set |
| PMU software power-down request | `0000fff9` | GPU power-down request is set |
| PMU repair status 4 | `ffff8001` | GPU power-up/repair-complete indication is clear |

The matching device tree describes SPLL as 702 MHz. Its requested 200 MHz GPU
assignment has not been applied by a native Haiku SCMI/clock driver. These
values establish a powered-down starting state and the selected clock setting,
not a measured GPU frequency or voltage. The following identity diagnostic
uses this starting state to choose a conservative divider and restore its
changes after the GPU read.

## Bounded power and identity cycle

The +184 `rock5_mali_resource_probe --identity` diagnostic implements a bounded
power cycle. It is disabled by default; the private image must explicitly set
`firmware_profile rock5-itx-edk2-v1.1-gpu-identity` in the `mali_csf` driver
settings. Admission additionally checks the fixed SPLL description and GPU
power-domain child, its clocks and matching boot-on supply. Node names are
checked as direct children and phandles remain dynamic. The existing EDK2
regulator initialization requests 750,000 microvolts for the GPU supply;
this first diagnostic inherits that policy and does not measure or change it.

The sequence changes only the GPU divider to four (described 175.5 MHz),
requests idle, powers the domain, waits for repair completion and de-idles it.
Only after checking the resulting state does it map the GPU page read-only
and compare static identity/features with the Linux reference. It unmaps the
GPU before idling/powering down, then restores the initial idle request and
divider and compares all ten platform registers. Shared PLLs, clock gates,
regulators, GPU commands, firmware and DMA are untouched by this diagnostic.
Each poll has a 10 ms deadline and an iteration limit. Failed handshakes and
restoration are retained independently; uncertain restoration blocks further
identity cycles until recovery. Platform observations share the same lock.

The production sequence passes host models with delayed handshakes, failed
power-up/de-idle/power-down, failed clock restoration, GPU mapping failures,
wrong identity, unexpected initial state, and a stopped/backwards timer. The
production driver/FDT fixture also checks additional profile rejection,
default-disabled and recovery-required ioctls, mapping permissions and cleanup.
All 131 host checks, the ARM64 build and both two-boot QEMU modes pass. QEMU
correctly rejects the absent GPU device; it does not emulate this power cycle.

The native diagnostic passes on both +184 boots. GPU ID `a8670005`, CSF ID
`040a0412`, MMU features `2830`, address-space mask `ff`, shader mask `50005`
and revision zero match the mainline Linux reference. The cycles take 585 and
590 microseconds including mappings and restoration. While powered, selector
158 is `00001f83`, idle acknowledge/status are `00000ffe`, power request is
`0000fff8` and repair status is `ffff8003`. All ten platform register values
return exactly to their initial values. The 175.5 MHz setting and inherited
supply policy remain descriptions, not measured clock/voltage qualification.

An independent validator checks identity, every platform phase and restoration
before the controller permits the next boot. Its eleven negative controls
reject altered evidence. Both native component/firmware checks, 32 component
and ten file hashes, identical FDT captures and reviewed desktops pass. Normal
reboot, verified shutdown, recovery, disarmed watchdog and independent complete
recovery-image/eMMC checks also pass. This stage issues no GPU command, loads
no firmware into the GPU and performs no rendering.

## Reset and interrupt implementation

The new `--reset` diagnostic requires the separate
`rock5-itx-edk2-v1.1-gpu-reset` profile. It reuses the admitted power cycle and
checks identity before examining GPU, MCU, core and interrupt state. An idle
GPU receives one soft-reset command (`0x101`); only reset-completion bit 8 is
enabled on the validated GPU interrupt 126. The ordinary identity ioctl keeps
its read-only GPU mapping and unchanged ABI.

The reset handler records masked/raw status, CPU and timestamp, masks the
source and acknowledges its reset bit. A 100 ms deadline and iteration bound
limit the wait. Raw completion without a handler record is a distinct failure.
Cleanup masks and disarms under the capture lock, synchronously removes the
handler, and verifies quiescent GPU/MCU/core state before GPU unmapping and
platform power restoration. Reset, interrupt evidence, cleanup and platform
restoration retain separate results; a failed diagnostic blocks further
mutating cycles until recovery. No firmware, page table or GPU job is submitted.

Host coverage includes delayed/stale/missing completion, lost or unexpected
delivery, clock and cleanup failures, admission/mapping/handler-installation
failures, and an actual two-thread handler/removal race through the production
kernel adapter with guarded mappings. All 132 host checks, the ARM64 build
and both QEMU modes pass for +186. Its first native boot panicked in device
discovery before the shell or either GPU diagnostic ran; automatic recovery
and independent recovery-image/eMMC integrity passed. Investigation reproduced
an [ARM64 instruction-cache alias defect](ARM64-ICACHE.md) on the actual CPU.
The exact boot failure remains retained.

The +187 image, including that cache correction, passes all 133 host checks,
the ARM64 build, both two-boot QEMU modes and two native boots. Each native
boot passes 129,024 instruction-alias checks before the GPU diagnostics.
Both soft resets produce exactly one GPU completion interrupt on CPU 0, with
masked/raw status `0x100`. The handlers run 15 and 9 microseconds after the
respective diagnostic start timestamps; these are not isolated hardware-latency
measurements. The inherited raw reset bit is cleared before each new command,
and final raw/masked status is zero. MCU/core-ready/transition fields and job/MMU
masks are zero before and after. All ten checked clock/power registers return
to their initial values. Normal reboot/shutdown, reviewed desktops, all 33
component and ten file hashes, identical FDT captures, automatic recovery and
independent complete recovery-image/eMMC integrity pass. This qualifies the
bounded reset/IRQ lifecycle; no GPU firmware execution or rendering occurred.

## Firmware startup and native qualification

The opt-in `rock5-itx-edk2-v1.1-gpu-firmware` profile adds
`rock5_mali_firmware_probe --start /boot/home/mali_csffw.bin`. Its root-only
request copies the bounded firmware bytes into kernel-owned memory before
parsing. The existing CPU-only probe mode and identity/reset interfaces remain
available. The +190 implementation passes the native startup and stop cycle
on both tested boots.

`CsfMemory.h` independently constructs a four-level ARM64 stage-1 table for
48-bit input addresses, MCU allocations below 4 GiB and physical addresses below
1 TiB. It uses 4 KiB pages, explicit read/write/execute and GPU-cache attributes,
and excludes protected sections. The official image requires nine table pages
and 224 payload pages, totaling 954,368 bytes. A guarded independent decoder
checks every descriptor, all mappings, payload copies, permissions, unused slots
and physical bounds. The GPU's `AS_MEMATTR` encoding is distinct from CPU MAIR.

The kernel allocates one contiguous, private system-team arena and converts its
CPU mapping to Normal Non-cacheable RAM after evicting cached initialization.
Tables and firmware payloads occupy separate portions of that arena. The startup
sequence reuses the tested power/identity/reset path, selects the Linux 6.18.52
noncoherent GPU protocol, powers L2 and activates address space 0. It then starts
the MCU and requires an actual global-interface job interrupt within one second.
The interface validator checks all 219 control/input/output intervals, including
alignment, bounds, mutual overlap and the reference 1.5.0 / eight-group /
eight-stream / 96-register / eight-scoreboard layout. A doorbell ping requires
both a fresh interrupt and the corresponding acknowledgment change. It configures
no shader work or rendering job.

Cleanup stops the MCU, locks and flushes GPU caches, removes the address-space
mapping, powers down L2, masks and synchronously removes all three interrupt
handlers, then restores platform power/clock state. Fault addresses, raw status,
boot/ping interrupt captures, reset evidence and all platform phases are retained
in the fixed 1,128-byte result. Host checks exercise missing/lost events, malformed
interfaces, MMU/GPU faults, timeouts, stuck clocks, allocation failures, partial
IRQ installation and handler/removal concurrency.

The arena is freed only after the hardware cycle, GPU cleanup and platform
restoration all succeed. A failed hardware cycle retains one system-team arena
across process exit, device close and module removal; its name prevents a
reloaded driver from starting another cycle. This also preserves the recovery
guard if failure precedes address-space activation. Recovery reboots reclaim it. This
bounded diagnostic does not yet supply persistent contexts, runtime power
management, firmware scheduling or a display accelerant.

The first +189 native attempt stopped before issuing `MCU_CONTROL`: it treated
MMU raw bit 16 (`AS0 request complete`) as a fault. The recorded final MMU raw
value is `00010000`, AS configuration is unmapped, and platform restoration
succeeded. The arena was retained and emergency Linux recovery completed;
independent recovery-image/eMMC checks pass. This trial remains failed, with
no firmware execution. The correction distinguishes low-bit faults from AS0
completion and records faults observed before IRQ arming. The native controller
also preserves a failed diagnostic exit without ending the session, allowing
its normal shutdown command to run before recovery.

The corrected +190 image passes all 136 host checks, the ARM64 build and both
two-boot QEMU modes. QEMU checks firmware preparation, cached-code aliases,
component integrity and absent-GPU rejection; it does not execute Mali firmware.
On the actual board, both boots allocate the expected nine page-table pages
and 224 payload pages, activate AS0 and receive one startup interrupt followed
by one ping interrupt on job IRQ 124. Each capture reports CPU 0 and masked/raw
status `80000000`. Firmware interface 1.5.0 exposes eight groups, eight streams
per group, 96 registers and eight scoreboards. The ping request and acknowledgment
both reach `00000100`. No GPU or MMU fault is recorded.

The complete power/start/ping/stop/restore cycles take 1,881 and 1,879 microseconds;
these include software and platform operations, not isolated GPU latency.
Both return result/cleanup zero and flags `ff`. Final AS configuration is unmapped,
MCU/core state and interrupt masks are idle, and all ten platform registers
return to their starting values. Final MMU raw `00010000` is the normal AS0
completion event; its interrupt remains disabled. The complete 1,128-byte results
are decoded independently, with 51 altered-evidence rejection cases.

Both boots also pass reset/identity diagnostics, 129,024 instruction-alias checks,
33 component and ten file hashes, identical FDT captures and reviewed desktops.
Normal reboot, verified shutdown, automatic Linux recovery, disarmed watchdog,
the complete original recovery-image hash, eMMC FAT/filesystem/file hashes and
all three eMMC reference-region hashes pass. This accepts the bounded firmware
lifecycle. It does not qualify shader execution, rendering, persistent contexts
or the separate open installed-SSD/page-aging failures.

## Native command submission

The +193 `rock5-itx-edk2-v1.1-gpu-commands` profile adds
`rock5_mali_firmware_probe --commands /boot/home/mali_csffw.bin`.
The root-only diagnostic submits the two 256-byte streams already qualified
on Linux. It preserves the previous firmware ioctl and 1,128-byte ABI and uses
one bounded kernel-owned queue. Mesa still needs a persistent application API.

`CsfCommandMemory.h` adds a separate application address space and driver-owned
MCU workspace. Its separate allocation plan checks overlap with actual firmware
regions. The initialized
firmware reports 25,600-byte normal and protected suspension buffers, both
within the diagnostic's separate 1 MiB reservations. The queue interface uses
another 8 KiB of MCU memory. AS0 now needs ten table pages and 738 payload pages,
totaling 3,063,808 bytes. Application AS1 uses six table pages and the following
buffers, totaling 106,496 bytes including tables:

| Buffer | GPU virtual address | Bytes | GPU mapping |
| --- | --- | --- | --- |
| Two command streams | `0x100000000` | 4,096 | Read-only, cached |
| Checked data and guards | `0x100002000` | 8,192 | Read/write, cached, non-executable |
| Queue ring | `0x100400000` | 65,536 | Read/write, uncached, non-executable |
| Completion objects | `0x100800000` | 4,096 | Read/write, uncached, non-executable |

One private system-team arena backs both address spaces. The CPU mapping is
Normal Non-cacheable. All holes remain unmapped. Both native allocations were
below 4 GiB physically. GPU virtual addresses above 4 GiB pass; native DMA with
physical addresses above 4 GiB remains untested. Host tests walk every descriptor at low,
above-4-GiB and upper-40-bit physical bases, checking permissions and bounds.

After firmware boot and ping, `CsfCommands.h` configures core allocation and
the reference progress/poweroff timers. It starts an empty group and queue,
then records a fresh zero-valued completion object and every initial data word
before exposing each submission. The queue wrapper flushes caches, calls the
reference stream, waits for outstanding work, updates a 64-bit completion object
and executes an error barrier. Acceptance requires the completion value, ring
extract position, fresh group interrupt and firmware synchronization event.
Actual job IRQ 124 captures both global and group events. MMU fault reporting
now covers AS0 and AS1 while distinguishing their normal completion bits.

Both rounds pass on both native boots. Each changes exactly eight words and
preserves all 2,040 guards, including stores on either side of a page boundary.
The final 8 KiB data hashes match Linux's corresponding reference rounds:
`13cbe2b3e83ba07adb5af5001bfb89fb5aab5e3c23f8d310150caff3f4b5a4fe`
and `655ea711afc75548899ed630b95b22d56f655c79a094ea2e907e6a074403baa7`.
The independent decoder also checks the entire command, ring, completion and
queue-interface allocations. Its 101 altered/incomplete-evidence cases fail
as expected. The 116,200-byte diagnostic output is emitted in bounded chunks;
missing, duplicate and reordered chunks are rejected.

Cleanup terminates the group, requests a firmware halt, stops the MCU, flushes
and unmaps both address spaces, removes IRQ handlers and restores platform
state. Both boots report command flags `1ff`, firmware flags `ff`, zero result
and cleanup errors, and no GPU/MMU/stream fault. Any failed hardware cycle retains
the entire combined arena and disables further cycles until recovery. Host
tests cover missing fences/IRQs/sync events, stale completion, wrong extract
position, guard corruption, queue faults, timeouts, stopped clocks and failed
termination/halt/flush/unmap. All 138 host checks, the ARM64 build and both
two-boot QEMU modes pass; QEMU validates packaging and absent-GPU rejection.

Both native desktops, 129,024 instruction-alias checks per boot, all 33 component
and ten file hashes, identical FDT captures, normal reboot/shutdown and automatic
ROOBI recovery pass. The watchdog disarms, and independent recovery-image,
eMMC filesystem/files and all three reference-region hashes pass. No shader,
rendering, performance comparison or installed-SSD requalification is claimed.

## Native compute shader execution

The +195 `rock5-itx-edk2-v1.1-gpu-shader` profile enables the fixed shader
diagnostic while retaining the command-memory regression. The root-only
`--shader` probe uses ioctl `0x4d435306`; its unchanged 116,200-byte response
adds the shader-workload flag `512`, giving successful flags `3ff`. The older
`--commands` path retains flags `1ff`, its ABI and its original workload.

`CsfShader.h` constructs the same complete 4 KiB allocation as the qualified
Linux reference. The driver replaces the default code before exposing any
application GPU mapping. Page-table permissions, queue submission, fresh
completion objects, interrupt handling and teardown use the qualified command
implementation. The code allocation is GPU read-only and executable; the data
allocation is writable and non-executable. This is a bounded privileged
diagnostic, not a general userspace shader or buffer interface.

Each of two native boots first passes two command-memory submissions, then
starts a fresh firmware/queue cycle and passes two shader submissions. All
initial/final data words, all guards and the complete code, ring, completion
and queue-interface buffers pass the independent oracles. Shader output matches
Linux cycle zero for both rounds. Fresh interrupts and completion objects pass;
group termination, MCU halt/stop, both address-space removals and all ten checked
platform-register restorations pass after both workloads. The shader's AS1
roots are `0x0d741000` and `0x0e472000`: both are physically below 4 GiB.
Virtual addresses above 4 GiB are qualified; high physical DMA is not.

Shader waits take 329/222 microseconds on the first boot and 313/209 on the
second. These bounded diagnostic timings are not comparable to Linux's logged
ioctl intervals. The host encoder matches all 4,096 Linux-generated bytes;
request/profile/retention checks and all 139 host tests pass. The shader oracle
rejects 101 altered or incomplete ABI records. Both two-boot QEMU modes pass,
including absence-of-GPU rejection for both command and shader operations.

Both native desktops, 129,024 instruction-alias checks per boot, 33 component
and ten file hashes, identical FDTs, normal reboot and verified shutdown before
media replacement pass. ROOBI recovery, watchdog disarming and independent
recovery-image/eMMC integrity also pass. The desktop still uses the EFI
framebuffer. Rendering, Mesa integration and installed-SSD requalification
remain open.

## Cached GPU properties

The qualified +209 image adds `kGetQueueProperties` (`0x4d435346`), a 192-byte
versioned query on a client-owned queue. The runtime worker captures the GPU
feature registers and validated firmware interface before accepting queues;
queries copy that immutable snapshot without MMIO, allocations or submissions.
Client capability bit 32 advertises the query. Metadata remains available for
an owned failed queue; `QueueInfo` reports its current health. Retired and
foreign handles fail.

The result distinguishes hardware/firmware slot counts from the driver's eight
software queues and 64 pending jobs per queue. It reports 96 working registers
with the last four reserved by the kernel wrapper, and the lower 47-bit user VA
limit. The firmware timer frequency is not a GPU timestamp/coherency guarantee.
Raw thread, texture, cache, tiler and memory feature values come from the
registers defined by Linux 6.18.52's `panthor_regs.h` and `panthor_hw.c` under
their MIT option. No guessed timestamp or cache-flush identifier is exposed.

An independent host register inventory checks all 26 register reads, including
the upper halves of four 64-bit fields. Runtime checks cover request boundaries,
copy failures, ownership, concurrent pending-work queries, failed queues and
retired handles across runtime restart. All 147 host checks and the full ARM64
build pass. Both QEMU modes pass two boots, absent-GPU rejection and normal
reboot/shutdown; QEMU does not emulate this GPU.

Two native ROCK boots each pass 43 checked queries, including 32 while the
producer queue has 32 pending jobs and the dependent consumer has one. Every
returned field matches within and between boots. Each boot rejects 14 request
boundary/ownership/copyout cases, two inherited-client accesses and three
retired queue/runtime queries. Duplicate descriptors retain access. Query
boundaries leave queue, buffer and VM counts unchanged. Existing buffer, VM,
queue, synchronization, heap and compute checks still pass: 1,696 accepted
submissions, 1,432 checked completions and eight application compute shaders
across the two boots. Runtime teardown is clean with no retained allocations;
heap events/growth/declines remain zero. Both desktops, normal reboot/shutdown,
recovery watchdog disarming and independent recovery-image/eMMC integrity pass.

The recorded values include:

| Property | Native result on both boots |
| --- | --- |
| GPU ID / revision / CSF ID | `a8670005` / `0` / `040a0412` |
| Shader / tiler / L2 present masks; address spaces | `50005` / `1` / `1`; `ff` |
| Core / L2 / tiler / memory / MMU features | `0` / `07120306` / `809` / `301` / `2830` |
| Thread features; threads / workgroup / barrier maximum | `04010000`; 2048 / 1024 / 1024 |
| Texture feature words | `c1ffff9e`, `0`, `0`, `0` |
| Coherency / GPU features | `0` / `0` |
| Firmware version / stream features; timer | `01050000` / `0007085f`; 24,000,000 Hz |

The GPU identity, present masks, L2/memory/MMU values and CS register/slot
counts agree with the previously recorded Linux GPU-query subset. Other raw
fields are captured from the documented register offsets and checked for
repeatability; the original Linux transcript did not print them. The desktop
continues to use the EFI framebuffer. GPU timestamps, rendering, actual
firmware OOM/growth and automatic fault recovery remain unqualified.

Evidence under `/mnt/HaikuWork`:

- Source `30f5d3768a732b54d40a717a9a39b8f786518996`; image `hrev60097+209`,
  SHA-256 `5fea46dbb4562ed5f94edd0c8484672fe039b9c71316b37aa3296da9abea3849`.
- Host/build and reproduction scripts:
  `artifacts/mali-properties/20260915T104500Z-4a5ec8`.
- Image manifest: `artifacts/mali-properties-image/20260915T105340Z-4d1710/manifest.json`.
- EL1/EL2: `artifacts/qemu-shell/20260915T105437Z-b1ac4b` and
  `artifacts/qemu-shell/20260915T105657Z-65330e`.
- Native qualification: `artifacts/automated-mali-properties/20260915T105944Z-a32ed3/qualification.json`.
- Serial, transcripts and desktop frames 007/019:
  `artifacts/interactive/20260915T110007Z-6d37de`.
- Independent Linux file/region readback:
  `artifacts/emmc-file-readback/20260915T110754Z-8af9d8` and
  `artifacts/emmc-read-reference/20260915T110945Z-670880`.
  The same six previously recorded vendor recovery warning signatures recur;
  `recovery-warnings-review.json` records the comparison without a causal claim.

## Next milestones

1. Broaden application/API coverage and qualify termination during pending
   graphics work. [Graphics-process cleanup](MESA-LIFETIME.md) now passes
   termination after completed rendering, survivor redraw, fresh-context reuse
   and full allocation baselines across two native boots.
   The [GLES pipeline fixture](MESA-PIPELINE.md) now passes texture upload and
   sampling, depth, stencil, additive blending, scissor and render-to-texture
   on two native boots: 155,976 pixels and sixty submissions pass.
   [Normal OpenGL Kit rendering](MESA-GLVIEW.md) now passes two live BGLViews,
   desktop OpenGL 3.1, complete GL/screen pixels, resizing and normal retirement
   across two native boots. [EGL windows](MESA-WINDOW.md) and [offscreen rendering](MESA.md)
   are also qualified. Presentation still uses a CPU copy.
2. Qualify sustained work, conformance, actual firmware heap growth and automatic
   fault/reset recovery. Extend fence integration where needed. Regulator
   ownership, runtime power management and DVFS remain separate work.
3. Implement native VOP2/HDMI modes/hotplug and additional display routes,
   followed by other board hardware.

## Sources and evidence

- [Panthor v6.12](https://github.com/torvalds/linux/tree/adc218676eef25575469234709c2d87185ca223a/drivers/gpu/drm/panthor),
  revision `adc218676eef25575469234709c2d87185ca223a`. Relevant driver files
  offer GPL-2.0 or MIT licensing; the container layout follows `panthor_fw.c`
  under its MIT option.
- [Mali CSF binding](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/Documentation/devicetree/bindings/gpu/arm%2Cmali-valhall-csf.yaml).
- RK3588 TRM v1.0 Part 1 (2022-03-09), local
  `artifacts/reference/rk3588-trm-v1.0-part1-20220309.pdf`, SHA-256
  `52fd969a33c0cfc4b14e90fb395c7bc71ac0132a1f65ab0e827e9b63c3404bc3`.
- [Mesa Panfrost](https://docs.mesa3d.org/drivers/panfrost.html), Mesa 25.3.6
  archive SHA-256 `59217efeac3b64e7ced958324b9db7494f1e0741aeb22d780276514cc1b8f206`.
- [HaikuPorts recipe and patches](https://github.com/haikuports/haikuports/tree/50ed75230b0be025c78f99a47508d1825799677d/sys-libs/mesa),
  revision `50ed75230b0be025c78f99a47508d1825799677d`.
- [Installed EDK2 v1.1 source](https://github.com/edk2-porting/edk2-rk3588/tree/6a682c0ef3ed74feb8b0d98f1c2aa771ddfbae18).
- [Linux 6.18.52 source](https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.52.tar.xz),
  archive SHA-256 `2b69564f7d4fea0c859b1959ba33709ee6e9139bd100e30a853b57159a8221b8`.
  Native firmware/VM/reference qualification:
  `artifacts/native-linux-gpu-reference/20260914T210244Z-af8ef5/qualification.json`.
  The 96 MiB EFI/RAM image SHA-256 is
  `2f1b2e90cb5706b0238fd40e523c2c38e3ea8876b4da24058fb99641423b1b69`.
- Linux command submission reference:
  `artifacts/native-linux-csf-reference/20260915T014927Z-76584c/qualification.json`.
  Its 96 MiB image SHA-256 is
  `caf10cdf64855276dd0e410b48463e73430bb28e34328beb7bd0ce6b00901768`.
  Source/configuration/ELF snapshots:
  `artifacts/linux-csf-reference-build/20260915T014621Z-8173e8/build.json`.
  The generator, full decoder, host checks and implementation notes are in
  `artifacts/mali-command-stream/20260915T013339Z-2aad56`.
  EL2 QEMU: `artifacts/qemu-linux-csf-reference/20260915T014639Z-046ba1`;
  EL1 Cortex-A76: `artifacts/qemu-linux-csf-reference/20260915T014818Z-673822`;
  retained EL1 `max` failure: `artifacts/qemu-linux-csf-reference/20260915T014639Z-3ad517/failure-review.json`.
- [Official arch10.8 firmware](https://gitlab.com/kernel-firmware/linux-firmware/-/blob/eeccccbe83daf22e1931e3557ba05b2c02427e4e/arm/mali/arch10.8/mali_csffw.bin),
  revision `eeccccbe83daf22e1931e3557ba05b2c02427e4e`, SHA-256
  `a27847ea11f8efb3136340c3ba8aab413ae25145eeb4a7f64ff5edd829a2405b`.
  Its separate `LICENCE.mali_csffw` is retained with the local binary.

The local index is `/mnt/HaikuWork/state/gpu-bringup-plan.json`. Initial
privileged inventory, source hashes and host evidence are under
`artifacts/gpu-bringup/20260914T201454Z-f2ee41`. Firmware, large sources and
images stay beneath `/mnt/HaikuWork`; firmware binaries are not committed.

Qualified native component evidence:

- +195 four compute-shader submissions and four command-memory regressions:
  `artifacts/automated-mali-shader/20260915T034514Z-125b8d/qualification.json`.
  Source `5060bcd6a594979725049db063f4677ab27463af`, image SHA-256
  `cd74c3333be18ed1444ca9f19fe40b0426c18207073486ec36f0d642d26fc399`.
  Image manifest: `artifacts/mali-shader-image/20260915T034126Z-b74930/manifest.json`.
  EL2/EL1 QEMU: `artifacts/qemu-shell/20260915T034200Z-4aebda` and
  `artifacts/qemu-shell/20260915T034200Z-2f70bc` respectively.
  Source snapshots, host checks, both decoders and synthetic negative controls:
  `artifacts/mali-haiku-shader/20260915T033414Z-f96c69`.
- +193 application mappings, queue/group lifecycle and four memory-store submissions:
  `artifacts/automated-mali-command/20260915T024219Z-51e818/qualification.json`.
  Source `545441b08bdc7b6a7baea799f71beb9a30bdb247`, image SHA-256
  `472fd3008e046298fe5c73a5fa72c4f822b959617c67299a56034d61ad750cfa`.
  Image manifest: `artifacts/mali-command-image/20260915T023625Z-2196b3/manifest.json`.
  EL2/EL1 QEMU: `artifacts/qemu-shell/20260915T023834Z-98eca2` and
  `artifacts/qemu-shell/20260915T023834Z-b267e4` respectively.
  Source snapshots, host checks, decoder and synthetic negative controls:
  `artifacts/mali-haiku-commands/20260915T023152Z-6e8727`.
- +190 firmware memory, MCU startup, ping and cleanup:
  `artifacts/automated-mali-firmware-start/20260915T011253Z-d90588/qualification.json`.
  Source `343a34d461fb950fb686ff4d398fb5e3b14a5487`, image SHA-256
  `8471e2a041445c62a86469128cfe78968f0b854e34be126f8c78e88c68128145`.
  EL2/EL1 QEMU: `artifacts/qemu-shell/20260915T010929Z-bdc32a` and
  `artifacts/qemu-shell/20260915T010929Z-6aba82` respectively.
  The initial +189 failure is retained in
  `artifacts/automated-mali-firmware-start/20260915T005401Z-27722e/failure-review.json`.
- +187 reset/interrupt and instruction aliases:
  `artifacts/automated-mali-reset/20260914T235509Z-59b6a7/qualification.json`.
  Source `bd41527b86d458cd291fb9cd23b91dbd3a23c379`, image SHA-256
  `9d4fbc5068737d69c17412671941ca7923feff4c865e59616ef30217fc52d4da`.

- +179 firmware preparation:
  `artifacts/automated-mali-firmware/20260914T211726Z-fe9d49/qualification.json`.
  Image SHA-256 `170c83873c0a3fa3509e9728833451d80976979df972c66369a47d101264d478`.
- +181 resources and firmware preparation:
  `artifacts/automated-mali-resources/20260914T213557Z-abfb48/qualification.json`.
  Source `ef7d4b9f3553059ab1debeb6288a106626c82277`, image SHA-256
  `4065a65e41b23b1f8828d3a466041af05096c60b1475dcbb6bf00029b01346fc`.
  Both native trees retain the previously recorded FDT hash. The recovery
  controller stayed up and its watchdog disarmed; no panic or USB filesystem
  checksum error occurred. This component qualification does not resolve the
  separate EL1 profiler/fault or installed SSD page-aging qualification.
- +182 read-only clock/power observation:
  `artifacts/automated-mali-platform/20260914T220117Z-9df5b3/qualification.json`.
  Source `8b2cd1e11c804a878b01b47f65ed86228ffd9262`, image SHA-256
  `9ebd436f31ad82d7c9a3bd8006699fb83a561f44e82bf5c78d888127af1330b2`.
  EL2 and EL1 QEMU evidence: `artifacts/qemu-shell/20260914T215553Z-42d745`
  and `artifacts/qemu-shell/20260914T215553Z-a01013` respectively.
- +184 power/identity/off and restoration:
  `artifacts/automated-mali-identity/20260914T224339Z-7144b5/qualification.json`.
  Source `a1b93227d781d23903867de464bc68666ca0eb48`, image SHA-256
  `7ce1024f8642f4375bea77d460ab4ce6da58f9c480efabd913c8581621a4772d`.
  EL2 and EL1 QEMU evidence: `artifacts/qemu-shell/20260914T224018Z-24475c`
  and `artifacts/qemu-shell/20260914T224018Z-efa455` respectively.

## Persistent application GPU queues

The +202 image qualifies a persistent kernel worker and per-client queue
create, submit, wait, query and destroy operations. It schedules up to eight
queues through physical CSG0/AS1, suspending and resuming each queue's command
state. Native tests use five contexts per boot; the eight-queue limit is also
checked by the host fixture. Every accepted submission retains an immutable
VM generation. Each queue has a private upper-half page-table subtree, 64 KiB
command ring and completion page. Changing the borrowed application root
requires suspension, cache maintenance and acknowledged address-space removal.

Across two native boots, the application probe accepts 1,264 submissions and
explicitly checks 1,136 completions, including eight compute shader runs. Both
contexts preserve CS register 80 across calls, switches, VM changes and compute.
Each boot advances the main ring to 67,456 bytes, past its first wrap, with
matching insert/extract pointers, 1,058 observed interrupts and 527 synchronization
events at the recorded checkpoint. All application data words and guards match.
Thirty-two queued calls retain their original VM generation through a mapping
replacement; stale-generation submissions are rejected.

Two child processes per boot each report 32 pending submissions before normal
exit and SIGKILL. They remove public buffer and VM handles before exiting.
Queued work retains the required memory, cleanup returns driver counts and
independently enumerated kernel areas to baseline, and the parent queues execute
again. Child submissions may be canceled during close, so they are included in
the accepted count but not the explicitly checked completion count.

Submission and completion waits do not hold the hardware control lock. The last
queue close joins the worker after firmware and platform teardown. Both native
cycles report successful firmware cleanup and restoration with no GPU/stream
fault. An uncertain hardware cycle retains its DMA allocations and VM leases
until reboot; this failure path is tested with host fault injection. Automatic
GPU-hang recovery remains unqualified. The API retains the existing experimental
profile and privileged-open restrictions.

All 144 host checks, the full ARM64 image build and both two-boot QEMU modes pass.
The independent scheduling model interprets 526 submissions, walks installed
physical page tables and injects 22 failure cases. The production runtime fixture
covers allocation/copyout rollback, queue limits, interrupted/timed-out waits,
concurrent close, worker joining and failed-cycle retention. QEMU checks packaging
and absent-GPU behavior; it does not emulate Mali execution.

Both native desktops, previous CPU-buffer/VM/command/compute regressions, normal
reboot/shutdown, automatic Linux recovery, independent eMMC file/filesystem/region
checks and recovery-image integrity pass. Existing vendor Linux display/DMA/
Bluetooth/old-Panfrost recovery messages are recorded against the +200 evidence;
this trial establishes neither their cause nor support for those devices.
The visible desktop still uses the EFI framebuffer. The later +204 image adds
shared synchronization; heap management, Mesa adaptation, rendering and display
integration remain open. The installed SSD's qualification is unchanged.

Source: `7eda1093697490b12895fa1056f3ef2048cb22e2` (`hrev60097+202`). Image SHA-256:
`1d81744daa32943ffc822781e79640132ea2be7a1af0926210b2881669dfda6f`.
Evidence under `/mnt/HaikuWork`:

- `artifacts/mali-runtime/20260915T063420Z-2b85f0/host-checks.log` and source snapshots.
- `artifacts/build-20260915T072612Z.log` and
  `artifacts/mali-runtime-image/20260915T072853Z-85613a/manifest.json`.
- EL1: `artifacts/qemu-shell/20260915T073000Z-40905a/mali-runtime-result.json`;
  EL2: `artifacts/qemu-shell/20260915T073000Z-8ffdac/mali-runtime-result.json`.
- `artifacts/automated-mali-runtime/20260915T073324Z-d55bae/qualification.json`,
  desktop reviews and `recovery-warnings-review.json`.
- `artifacts/interactive/20260915T073332Z-760ef1` for UART, transcripts and frames.
- `artifacts/emmc-file-readback/20260915T074111Z-da0931` and
  `artifacts/emmc-read-reference/20260915T074121Z-9f751b` for independent Linux checks.

## Shared GPU synchronization

The +204 image qualifies per-open native binary/timeline synchronization handles,
shared-object and snapshot-fence descriptors, atomic wait/signal submission
batches and cancellation propagation. Binary replacement preserves previously
captured fences; increasing timeline points include prior work. Wait-any,
wait-all, availability, submission arrival, timeouts and interruption are explicit.
Publication captures waiting fences before another update can replace them.
Completed history is pruned; object, fence, depth and wait quotas bound allocations.

Anonymous descriptors hold their driver module until the kernel returns from the
final callback. Descriptor numbers and close-on-exec flags are published together,
after successful copyout. Shared descriptors survive device closure and support
duplication, inheritance and import by another client. Snapshot descriptors retain
the selected submission. They currently provide import and an explicit wait ioctl;
poll/select and synchronization with other GPU drivers are not implemented.

Queue dependencies are captured before output fences are replaced. Failed copyout
publishes neither work nor fences. A blocked queue permits other queues to run;
failed dependencies cancel that queue's pending work, with independently reported
errors. Hardware failure signals outstanding fences while retaining potentially
active DMA memory under the existing recovery rule. Automatic GPU reset is pending.

All 145 host checks pass, including the production synchronization implementation,
descriptor publication/module release, timeline DAGs, concurrent waits, allocation
and copyout rollback, actual runtime scheduling, cancellation and retained failures.
The full ARM64 build and both two-boot QEMU modes pass. QEMU validates packaging,
kernel regressions and absent-GPU behavior; it does not emulate Mali execution.

Each of two native boots passes 128 CPU timeline points, wait-for-submit and
wait-available publication followed immediately by reset, binary replacement and
fence transfer. A consumer queue loads an actual GPU-produced word from another
context's buffer, stores it in its own buffer and preserves all 4,093 guards.
Blocked producer work remains pending while independent work completes. Timeline
point 20's job completes before point 10, but its fence remains pending until the
older work finishes. Snapshot fences retain their captured work across reset and
CPU signaling.

Two child teams per boot leave blocked GPU work: one exits normally and one is
killed. Their parent dependencies report cancellation without executing their
commands, preserving all 2,048 guards. Shared and snapshot descriptors survive
the last GPU descriptor close, duplication, reopening and import, followed by
reset/reuse. Driver counters and independently enumerated kernel areas return to
baseline. Native evidence establishes descriptor lifetime across final device
close; actual kernel module unloading was not observed. Module release ordering
is checked by the production-descriptor host fixture.

The new synchronization cases accept 210 submissions across the two boots,
check 202 completions and verify eight cancellations. The retained queue cases
accept 1,264 submissions and check 1,136 completions, including eight compute
shaders. Combined totals are 1,474 accepted submissions and 1,338 explicitly
checked completions; child work canceled during closure is not counted as a
checked completion. The earlier firmware/command/compute, buffer, VM and
129,024 instruction-alias checks per boot also pass. Both runtime shutdowns
report successful engine/firmware cleanup, restored platform state, no GPU fault
and no retained DMA allocations.

Both desktops, 36 component and ten file hashes, identical FDTs, normal reboot,
verified shutdown before media replacement, watchdog disarming and automatic
Linux recovery pass. Linux independently verifies the eMMC files, complete FAT
fixture and three reference regions; the recovery-image hash is unchanged.
Six existing vendor Linux display/DMA/Bluetooth/old-Panfrost warning signatures
recur in the prior +202 recovery; no cause or repair is inferred. The desktop
continues to use the EFI framebuffer. Tiler heaps, Mesa rendering, poll/select
and inter-driver fence integration, automatic GPU reset and native display
support remain pending.

Source: `cb87c65fcd8bff2e6b24e9e082b469727fe7e2c9` (`hrev60097+204`). Image SHA-256:
`2fccfd6be0ce3a3719c34bcb47128c99fbc1b60e6d5cd45613f1f02c8cd3a6dc`.
Evidence under `/mnt/HaikuWork`:

- `artifacts/mali-sync/20260915T075656Z-7fc037/host-checks.log`, design and source snapshots.
- `artifacts/build-20260915T083651Z.log` and
  `artifacts/mali-sync-image/20260915T083828Z-551e2c/manifest.json`.
- EL1: `artifacts/qemu-shell/20260915T083952Z-189546/mali-sync-result.json`;
  EL2: `artifacts/qemu-shell/20260915T083952Z-342f01/mali-sync-result.json`.
- `artifacts/automated-mali-sync/20260915T084304Z-6944c1/qualification.json`,
  desktop reviews and `recovery-warnings-review.json`.
- `artifacts/interactive/20260915T084321Z-21c6f6` for UART, transcripts and frames.
- `artifacts/emmc-file-readback/20260915T085212Z-db73eb` and
  `artifacts/emmc-read-reference/20260915T085722Z-334ced` for independent Linux checks.

## Native tiler heaps

The +207 image qualifies VM-owned tiler heap create/destroy/query operations.
Each heap has a private zeroed context page and GPU RW/NX chunks with the Linux
Panthor initial list format. Chunk sizes are page aligned from 128 KiB to 8 MiB,
with up to 64 chunks per heap and 128 resident heaps per client/VM. Context and
chunk bytes are bounded at 256 MiB per client and 512 MiB globally; page tables
share the existing global table quota. Pending allocations and retired objects
remain charged until their references are released.

Heap membership belongs to immutable VM generations. Upper root entry 257 holds
private heap mappings, distinct from queue entry 256 and user mappings. Creation,
destruction and copyout rollback preserve queued roots, including when a later
generation reuses a heap virtual address. The CPU initializes each opaque context
before exposure and does not modify it afterward.

The firmware OOM path validates the exact context address and render-pass
counters. It prepares allocations without changing exposed PTEs, then takes the
hardware address-space lock, publishes new entries, flushes GPU caches and waits
for unlock completion before replying. Memory pressure returns a zero chunk so
firmware can wait, reclaim or invoke Mesa's exception handler. Post-publication
failure retains the heap under the existing recovery rule. Automatic GPU reset
is still pending.

All 145 host checks pass. The production client fixture independently walks
scattered physical page tables and covers full initial chunk/context contents,
copyout/allocation rollback, mapping generations, address reuse, 64-chunk growth,
pending-growth abort and quotas retaining memory after client closure. Eight
firmware/MMU model cases check successful growth, zero-memory replies, invalid
requests and lock/commit/flush/unlock failures; published memory survives failed
completion. The full +206 ARM64 image and both two-boot QEMU modes pass.
The first native trial fails during the ninth 512-sample heap readback batch;
the first eight batches and prior queue/synchronization cases pass. Its test
incorrectly encodes a positive 32 KiB STORE_MULTIPLE offset in a signed 16-bit
field, addressing before the readback buffer. An independent host command
interpreter reproduces the failure. Rebasing the destination per batch passes
33 interpreted programs, full readback guards and the exact original-error
rejection. All 146 host checks, the full +207 ARM64 build and both two-boot QEMU
modes pass. The initial EL2 run reaches userspace after reboot but times out in
USB control before its shell marker. An unchanged-image EL2 repeat passes;
concurrency is not established as the cause. The raw failed run and review
remain alongside the accepted evidence.

The failed +206 trial recovers after verified normal shutdown. Independent Linux
checks preserve the eMMC filesystem, both test-file hashes and all three reference
regions. Its raw evidence remains under
`artifacts/automated-mali-heaps/20260915T094137Z-aaeaeb` and
`artifacts/interactive/20260915T094144Z-58e3d0`. The readback-command reproduction
and correction are recorded separately under
`artifacts/mali-heaps/20260915T095409Z-019ef0`.

Both native boots pass 5,159 GPU-read samples across five 2 MiB chunks, including
the opaque context before HEAP_SET, all 64-byte chunk headers and both ends of
every one of 2,560 pages. All 3,033 remaining readback words stay unchanged across
11 batches. The readback helper rebases each batch so every signed offset stays
within range; the test checks earlier samples and untouched guards after each
batch, including the original 32 KiB failure boundary.

HEAP_SET executes, and the GPU writes a marker into private heap data. A queued
reader waits on an independent producer while the CPU removes the heap and
creates a new one at the same virtual address. The pending old job reads the
marker; the later job reads zero from the new allocation, preserving all 8,190
remaining guards. The old allocation remains resident until its queued reference
is released. Two child teams each leave two heaps in pending GPU work; one exits
normally and the other is killed. Driver counters and independently enumerated
kernel areas return to baseline.

Per boot the new heap scope accepts 111 jobs and checks 47 completions across
four contexts and six created heaps. Together with all prior buffer, VM, queue
and synchronization cases, both boots accept 1,696 submissions, explicitly check
1,432 completions and execute eight application compute shaders. Final device-FD
closure, reopening and synchronization import also pass. Both runtime teardowns
report engine/firmware success, restored platform state, no faults and no retained
DMA allocations. The firmware heap-event counters are zero: this workload does
not exercise actual firmware OOM or growth.

Both desktop reviews, 36 component and ten file hashes, identical FDTs, normal
reboot, verified shutdown before media replacement, watchdog disarming and
ROOBI recovery pass. Linux independently verifies the complete FAT fixture, both
test-file hashes, all three eMMC reference regions and the recovery image. The
six recorded vendor recovery warning signatures also appear in +204. Actual
firmware OOM/growth, Mesa rendering, automatic GPU reset and display acceleration
remain unqualified. The desktop uses the EFI framebuffer.

Source: `7908c0f07a8d3bbed99f6140c642cf30920b3232` (`hrev60097+207`). Image SHA-256:
`75172ccb7199de6249607f2fbc799ccff8982e068c34569d4bc375024be1b34e`.
Evidence under `/mnt/HaikuWork`:

- `artifacts/mali-heaps/20260915T090452Z-9b10dd` for the original implementation,
  host heap/MMU tests and failed +206 trial review.
- `artifacts/mali-heaps/20260915T095409Z-019ef0` for the signed-offset reproduction,
  146 passing host checks, source snapshots and initial EL2 failure review.
- `artifacts/build-20260915T095625Z.log` and
  `artifacts/mali-heaps-image/20260915T095724Z-c130b8/manifest.json`.
- EL1: `artifacts/qemu-shell/20260915T095805Z-e81f32/mali-heaps-result.json`;
  EL2: `artifacts/qemu-shell/20260915T100339Z-55c4ad/mali-heaps-result.json`.
- `artifacts/automated-mali-heaps/20260915T100638Z-7b4617/qualification.json`,
  desktop reviews and recorded recovery warnings.
- `artifacts/interactive/20260915T100655Z-f7f3f2` for UART, transcripts and frames.
- `artifacts/emmc-file-readback/20260915T101457Z-0ecfe9` and
  `artifacts/emmc-read-reference/20260915T101545Z-76e740` for independent Linux checks.

## Limits a web browser fits in (2026-11-01)

On the CI installation of 2026-11-01 every WebGL page failed in Summit: the
web process asserted in `panfrost_ptr_map` (no staging texture), then
`GL_OUT_OF_MEMORY` and "could not create a WebGL context". Three bounds
refused without saying so:

- the driver's client limits (128 buffers and 256 MiB a client, 512 MiB in
  all, 256 mappings and 1 GiB a VM): now 4096 buffers of up to 512 MiB, 2 GiB
  a client, 4 GiB in all, 4096 mappings and 8 GiB a VM;
- eight queues in the whole system, one per GL context: now 24. The runtime
  already switches queues through CSG0/AS1; the bound was the firmware window
  their 1 MiB suspend buffers must fit, so the protected suspend buffers
  moved to 0x05a00000 (a static_assert keeps the regions in the window);
- Mesa's Haiku kmod kept its own VA map of 256 mappings a process and 8
  groups a device (airos-works/mesa rock5-mappings: 4096 and 24, with
  static_asserts against the haiku-abi headers).

The driver now logs, once per kind, which bound refused a request, and the
first 64 failed requests by number. The WebGL Aquarium (webglsamples.org) in
a 1776x883 Summit view, frames per second from Summit's graphics tool (the
page's own counter agreed without the MCP inspector):

| Fish | 1000 | 5000 | 10000 | 15000 | 20000 | 25000 | 30000 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| fps | 56 | 24 | 12.4 | 8.4 | 6.4 | 5.0 | 4.4 |

The web process's main thread is one full core (about 8 us a fish, WebKit,
ANGLE and Mesa's front end); Mesa's glthread cannot help, Panfrost does not
claim `map_unsynchronized_thread_safe`.
