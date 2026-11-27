# Experimental Haiku Mesa package

This owner-fork port connects Mesa 25.3.6/Panfrost to the qualified Haiku Mali
CSF kernel ABI. It has passed offscreen GLES rendering on two native ROCK boots;
[MESA.md](../../../docs/rock5-itx/MESA.md) records exact evidence and limits.
It is not an upstream Mesa/Haiku release or a system package replacement.

The current polygon-mode candidate is **unqualified on hardware**. It retains
vertex/fragment NIR and lazily uses Mesa's CPU vertex/geometry pipeline for
non-solid polygons, then uploads the resulting primitives for native GPU
rasterization. `HAIKU_PAN_SW_POLYGON=1` enables it; the polygon and application
native test launchers opt in explicitly. Other launchers retain the default.
The enabled path preserves original quad, quad-strip and polygon boundaries.
Those legacy modes use CPU geometry processing even when filled, because
advertising them otherwise would submit unsupported primitives to Valhall.
The earlier native application failure and independent diagnostic are recorded
in [MESA-APPLICATION.md](../../../docs/rock5-itx/MESA-APPLICATION.md).

This development path supports direct draws and float32 varyings. Vertex
textures/images/SSBOs and writes, transform feedback, fragment front-face or
primitive-ID inputs, integer/64-bit varyings, layered viewports, stippling,
separate near/far clipping and unscaled polygon bias remain excluded. Zero
depth-range scale with polygon offset is also excluded. Unsupported draws
report an experimental-path failure; this is not general polygon conformance.

`polygon-probe.cpp` checks full readbacks for thirty-two polygon cases over two
contexts. `polygon_validation.py` independently checks every returned pixel,
state, guard and restored/equivalent image. Its original native run diagnoses
the missing feature; successful software rendering does not qualify this new
driver path. `test-polygon-geometry.py` instead executes the actual helper with
real Mesa NIR/draw libraries and a checked memory-backed pipe adapter. The test
covers geometry, resource/state restoration and map failures under ASan/UBSan;
the adapter does not emulate Panfrost callbacks or GPU rasterization.

The application launcher captures controller stdout and renderer stderr into
separate RAM files. After the controller has waited for its application child,
it emits both original streams with byte counts and SHA-256 hashes. The separate
logging validator checks those boundaries and hashes alongside the unchanged
application pixel, menu, queue and cleanup checks. Its synthetic tests do not
establish that the next native run will pass.

For that host test, configure a separate native Mesa build from the patched
source with `-Db_sanitize=address,undefined`, `-Dgallium-drivers=softpipe`,
`-Dllvm=disabled` and the usual headless options. Build `libgallium.a`, `libnir.a`,
`libcompiler.a` and their Mesa utility archives. With the lab environment loaded,
run `python3 tools/rock5-itx/mesa/test-polygon-geometry.py --mesa-source SOURCE
--host-build HOST_BUILD --output OUTPUT`; all three paths must be under
`/mnt/HaikuWork`. Each attempt preserves its exact helper/adapter/test bytes,
commands and execution output in a fresh directory.

The current source also qualifies [EGL window rendering](../../../docs/rock5-itx/MESA-WINDOW.md):
native GPU textures are copied into Haiku window bitmaps. Its probe checks
complete bitmap/screen pixels, dimensions, resizing and retirement across two
native boots. The accepted window and offscreen images are pinned in those
documents. A [normal OpenGL Kit fixture](../../../docs/rock5-itx/MESA-GLVIEW.md)
also passes on two native boots with desktop OpenGL 3.1.

The BGLView/desktop OpenGL probe checks two live
views, alternating draws, resizes, recursive GL locking, full GL/screen pixel
readback and allocation cleanup. Its libglvnd patch checks the displayed bitmap
under the same renderer mutex used by bitmap replacement. Both QEMU modes and
the exact native image pinned in MESA-GLVIEW.md pass. This does not qualify
general application compatibility or replace the normal system OpenGL packages.

The later +226 QEMU run exposed an observation race in that fixture: its final
GL readback passed, but the screen capture contained the white view background.
The revised probe requests a full update on the window thread, waits for
UpdateIfNeeded and then Sync before taking its single screen capture. Sync
inside Draw runs before the window's AS_END_UPDATE publishes the front buffer.
Every pixel and guard check remains required. This synchronization revision
passes both QEMU modes and two native +227 boots; the failed +226 image, source,
binaries and actual readback remain preserved with its local evidence.

The [GLES pipeline probe](../../../docs/rock5-itx/MESA-PIPELINE.md) passes both
QEMU modes and two native boots on the +219 image. It
checks six operations in each of two contexts at 97 by 67 pixels: padded
texture upload and sampling, depth rejection, stencil masking, additive
blending, scissor clipping and reflected sampling of a GPU-rendered texture.
Complete RGBA readbacks, guard bytes and native allocation baselines are checked
independently. All 155,976 native pixels and sixty submissions pass. This
bounded fixture does not establish GLES conformance.

The [graphics-process lifetime probe](../../../docs/rock5-itx/MESA-LIFETIME.md)
passes both QEMU modes and two native boots on the +221 image. It keeps two
independent processes' contexts open, terminates one
after completed rendering, checks a new frame in the survivor, then checks
normal cleanup and fresh-context reuse. Every returned pixel and guard is
checked. All 51,992 native pixels, allocation baselines and recovery checks
pass. It does not terminate pending GPU work or test automatic GPU reset.

`sources.json` pins both original archives, six SDK packages, seven kernel ABI
headers, host compiler helpers and every patch/probe/validator. The complete
Mesa patch includes the pinned HaikuPorts changes; do not apply those twice.
For libglvnd, apply the pinned HaikuPorts patchset, then the sysroot and mutex
patches, then the bitmap-lock patch. Original source licenses/notices remain in place. This fork's original
bridge and test code is MIT licensed and AI-assisted at the owner's request.

## Reconstruct locally

Use the existing ARM64 compiler/package tool described in the lab README.
Host prerequisites are the pinned Mesa compiler helpers in
`/mnt/HaikuWork/toolchains/mesa-host/bin`, Meson/Mako/Python in `mesa-python`,
Ninja, and the extracted LLVM 18 host dependencies in `mesa-native-deps`.
These compile shader sources for the GPU; they are not Haiku shared libraries.
Source archives and SDK packages are local inputs. No downloads, host package
installation, deployment or board reboot occur in this script.

Place the two archives named in `sources.json` in one directory, and the six
SDK packages in another. The +207 SDK snapshot is retained locally at
`artifacts/mali-haiku-mesa/20260915T100752Z-80955e/packages`; the archives are
in that stage's `reconstruction-inputs` directory. Run from the Haiku checkout:

```sh
source tools/rock5-itx/env.sh
python3 tools/rock5-itx/mesa/build.py \
  --archives /mnt/HaikuWork/artifacts/mali-haiku-mesa/20260915T100752Z-80955e/reconstruction-inputs \
  --packages /mnt/HaikuWork/artifacts/mali-haiku-mesa/20260915T100752Z-80955e/packages \
  --output /mnt/HaikuWork/artifacts/mesa-reconstruction/NEW_BUILD
```

The output must be new. The script checks inputs, applies patches, verifies all
changed Mesa files and ABI headers, constructs a private SDK, cross-builds
GLVND/Mesa/the probe and emits `package/manifest.json`. `--prepare-only` stops
after patched-source and SDK preparation. Logs, commands, input receipts and
failures remain in the output directory. This reconstructs pinned sources and
configuration; build-path/debug information can change binary hashes.

The package manifest lists thirty-three assets for `/boot/home/mesa-trial`. The `run`
launcher uses Haiku's `LIBRARY_PATH` and a private GLVND vendor file. Its probe
accepts `--native`, `--software` and `--absent-device`. Native mode expects
`/dev/graphics/mali_csf/0` and the separately supplied, licensed firmware at
`/boot/home/mali_csffw.bin`. Software mode is the explicit QEMU fixture.
Neither a build result nor software rendering qualifies a native candidate:
run the full Haiku build, both QEMU modes, native cycles and recovery checks.
`run-window --software` and `run-window --native` run the window probe. It opens
a real Haiku window and checks all pixels in both its bitmap and a screen
capture over two contexts and six view resizes. `window_validation.py` checks
the returned data independently and writes actual bitmap/screen PNGs. CPU
bitmap presentation does not accelerate app_server or implement native display
modes. `run-glview --software` and `run-glview --native` run the OpenGL Kit
fixture, checked independently by `glview_validation.py`.
`run-pipeline --software` and `run-pipeline --native` run the GLES pipeline
fixture, checked independently by `pipeline_validation.py`.
`run-lifetime --software` and `run-lifetime --native` run the process fixture,
checked independently by `lifetime_validation.py`.

`render_validation.py` independently compares complete readbacks with integer
pixel-region oracles and can write actual RGBA/PNG artifacts.
`native_render_validation.py` also requires cached native GPU properties,
accepted/completed queues, expected runtime joins and allocation baselines.
Frozen build, controller, host-fixture and qualification scripts for the first
passing image remain with the evidence linked in MESA.md.

`run-heap-pressure --software` and `run-heap-pressure --native` exercise a
bounded geometry workload with 256 KiB tiler chunks, one initial chunk and a
32-chunk limit. The independent `heap_pressure_validation.py` requires all
RGBA pixels and guards, actual native allocation growth, completed queues and
full cleanup across two contexts. Firmware growth counters are checked in the
native UART evidence as a separate gate. The [qualified +224 image](../../../docs/rock5-itx/MESA-HEAP-PRESSURE.md) passes
both QEMU modes and two native boots: 24 actual growth requests, 51,992 pixels,
1,024 guards, 24 submissions, complete cleanup and recovery/integrity checks.
This fixture does not establish sustained load, memory-limit failure or GPU reset.

The pressure fixture uses an explicit 786,432-byte vertex buffer. Its first
GL-generated vertex-ID variant exposed a softpipe split-draw limitation in
QEMU; that candidate was not deployed. Its source and complete failed readback
are retained. This workload does not qualify gl_VertexID across large draws.

`run-heap-limit --software` and `run-heap-limit --native` use the same explicit
geometry with a fixed single 256 KiB chunk. Native mode enables Mesa perf/sync
diagnostics and requires actual incremental-rendering counters, constant heap
size, full pixels/guards, completed queues and cleanup. Firmware refusal counts
are checked separately on UART. The [qualified +227 image](../../../docs/rock5-itx/MESA-HEAP-LIMIT.md)
passes both QEMU modes and two native boots: 36 actual refused requests,
36 incremental passes, 51,992 pixels, 1,024 guards, 24 completed submissions,
full cleanup and recovery/integrity. This limits the tiler heap only;
system-memory exhaustion and automatic GPU reset remain unqualified.

`run-sustained --software` and `run-sustained --native` qualify
continuous drawing in retained contexts: one permits heap growth, the next
fixes one chunk. Native mode requires at least sixty seconds of accumulated
draw/fence/readback time and 1,024 frames per context, with bounded frame and
wall-time limits. Software mode uses 32 frames per context. Each actual RGBA
readback is encoded losslessly as runs and independently expanded and checked
by `sustained_validation.py`, together with guards, timing, native heap
observations, incremental-pass counters, completed queues and cleanup. The
[+232 image](../../../docs/rock5-itx/MESA-SUSTAINED.md) passes both QEMU modes and
two native boots: 91,648 frames, 595,620,352 pixels, 11,730,944 guards, 183,304
completed submissions and 147,196 incremental passes. This does not claim GPU
utilization, thermal limits, concurrency, conformance or reset recovery.

The first +229 candidate passed both two-boot QEMU modes, but the NanoKVM
stopped responding and restarted during the first native sustained context.
All 5,368 complete captured frames matched; required duration and cleanup were
not reached. Its source, binaries and partial output are preserved. The next
candidate line-buffers stdout, reducing the many small writes used to encode
each frame while retaining every output byte and newline flush. The exact
controller-reset cause remains unresolved; this trial remains incomplete.

The buffered +230 attempt kept the NanoKVM responsive and rendered 26,511
correct frames over 60.001278 seconds of measured work, but rejected a heap
that grew from seven chunks after two frames to eight by the end. Normal
shutdown, recovery and independent storage checks passed; the native trial
remains incomplete. Neither Mesa nor the kernel growth policy promises that
two frames establish a final heap size. Protocol version 2 therefore samples
the heap every 128 frames and requires at least 20 seconds of measured rendering
and 1,024 frames without further growth, in addition to the one-minute workload.
New growth extends observation within the original frame/wall limits. The fixed
policy still requires one chunk throughout. The host checks sample order, sizes
and timestamps against actual frame timing; all cleanup checks remain required.

The +231 streamed trial completed its growing context but lost NanoKVM during
the fixed context. `sustained_capture.py` now builds a wrapper that saves full
stdout/stderr to a dedicated Haiku RAM filesystem, then splits the log into
at most sixteen 8 MiB pieces. Existing staged downloads run at 256 KiB/s after
GPU work finishes. Guest, NanoKVM and host piece hashes and the complete log
hash must agree before the unchanged rendering validator runs. Twelve damaged
capture cases are rejected; the actual QEMU captures and eleven native pieces
pass. The frozen controller and receipts are linked in MESA-SUSTAINED.md.

That wrapper first exposed a RAM filesystem unmount assertion in QEMU. The
+232 Haiku change releases its published root vnode reference before deleting
the hierarchy. Both QEMU modes and both native captures now unmount cleanly;
normal reboot/shutdown and recovery pass. The earlier assertion and failed
native trials remain preserved.

The [concurrent graphics fixture](../../../docs/rock5-itx/MESA-CONCURRENCY.md)
passes on the +234 native image: two independent processes render over 64
synchronized rounds on each of two boots. All 128 actual rendering intervals
overlap, all 260 full RGBA8 frames and guard regions pass, and all 532 GPU
submissions complete. Normal retirement, survivor rendering, fresh-process
reuse and full allocation cleanup pass. Both QEMU modes and all earlier
bounded graphics regressions pass. The first controller outage remains
recorded. `graphics_capture.py` reuses the qualified RAM collection protocol
for all seven graphics fixtures; the transfer implementation is unchanged.
Termination during pending graphics work and GPU fault/reset recovery remain
open.

The [pending-work fixture](../../../docs/rock5-itx/MESA-PENDING.md) qualifies
two +236 native boots. `run-pending`
calibrates a bounded integer fragment shader using complete pixel readback,
repeats the last checked workload and reports a zero-time fence result. The
parent terminates that worker, verifies survivor rendering, normal cleanup and
a fresh process. Native acceptance additionally requires the worker's queue
handle in the driver's pending-at-close diagnostic captured on the same boot's
UART. QEMU checks the software pixels and process lifecycle only. This case
does not claim immediate preemption or automatic recovery from GPU faults.

## Bounded command-fault recovery

`run-recovery --native` exercises a deliberate invalid CS instruction on two
native queue clients, checks three error fences and failed queue state, then
closes both clients and verifies the full allocation baseline. It starts a
fresh native store queue and runs the existing complete GLES pipeline fixture
without rebooting Haiku. `--software` requires the native device to be absent
and tests only the launcher and subsequent software rendering.

The kernel stops new submissions, performs a bounded soft reset,
explicitly removes AS0/AS1, verifies the final GPU state and restores the
qualified platform configuration. Failed recovery retains memory. Original
rendering errors remain errors; all affected queues must close before a fresh
runtime starts. The [qualified +238 image](../../../docs/rock5-itx/MESA-RECOVERY.md)
passes both QEMU modes, two native boots, all affected-job error fences, reset
and allocation checks, and fresh rendering without a Haiku reboot. Retained
Mesa-context notification, innocent-context preservation, general hang recovery
and native display control remain open.

## Shared-context loss notification

The [qualified +243 image](../../../docs/rock5-itx/MESA-LOSS.md) adds context-loss
notification requiring the +240 queue reset query. Both native boots pass
the shared-context loss, ignored readback, complete cleanup and fresh-rendering
checks, together with both QEMU modes and recovery/integrity checks.
Mesa polls each owned queue to distinguish pending recovery, quiescent cleanup
and retained memory, reports unknown reset attribution, and leaves affected
contexts available for application teardown. It does not attempt immediate
context recreation while other failed queues remain open.

`run-loss --native` keeps two GLES contexts current on separate threads with
shared program and buffer objects. It checks full images before a finite CS
command fault, both reset notifications, unchanged readback memory after loss,
complete display teardown and fresh pipeline rendering. The software mode
checks shared rendering and cleanup without injecting a reset. The host
`test-reset-status.py --source PATCHED_MESA --baseline PREVIOUS_MESA` checks the
actual reset functions and verifies that their non-Haiku implementation is
unchanged. Robustness conformance remains open.
`test-cleanup-context.py --source MESA` runs the actual `csf_cleanup_context()`
against a lost context (its last job ended in an error, the kernel refuses or
has already done the destroys): the teardown must not abort, as its asserts
made Summit's WebProcess do (airos-works/haiku#209), and must release
everything.

The +241 native trial reached the deliberate command fault and verified GPU
reset and cleanup, then failed the first application's reset-status assertion.
It is retained as a failed trial. A host reproducer using the actual Mesa
state-tracker functions found that a synchronously polled reset was returned
and cached again, producing a duplicate notification on the next query.
`test-state-tracker-reset.py` checks the candidate correction, repeated queries
during recovery, genuine callback notifications and unchanged non-Haiku code.
The probe now prints both reset return values before asserting. The validator
also recognizes the exact expected empty-runtime firmware probe; unexpected
errors still fail. The +243 qualification includes the rebuilt +242 Mesa
correction; the failed +241 trial remains preserved.

The +242 candidate passed its first native boot but stopped at the second
instruction-cache fixture: randomly placed aliases all shared the writer's
address bit 12. +243 reserves eight consecutive pages, retaining every original
cache comparison while making opposite-index coverage deterministic. Both
native boots now complete all 129,024 comparisons. The +241 and +242 failures
remain preserved; the compiled qualified source and package reuse proof are
recorded in the +243 report above.
