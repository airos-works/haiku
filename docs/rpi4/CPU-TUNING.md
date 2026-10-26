# Raspberry Pi 4 CPU tuning

Work of 24 October 2026, following the ARM64 profiles proposed in
[SYSTEM-PERFORMANCE.md](../airos/SYSTEM-PERFORMANCE.md), measured on the Pi on
26 October (see [Native measurement](#native-measurement-26-october-2026)).
Every Pi image, the lab's and CI's, is built with the profile.
Evidence: `/mnt/HaikuWork/rpi4/evidence/cpu-tuning-20261024` and
`cpu-tuning-native-20261026`.

## The profile

`SetCPUProfile <architecture> : <profile>` (`build/jam/ArchitectureRules`)
adds code generation flags to userland, kernel and boot loaders of a build.
Call it from `UserBuildConfig`. `tools/rpi4/UserBuildConfig` applies profile
`rpi4` to every Pi image; `RPI4_CPU_PROFILE = none ;` before including it
builds generic code (for comparisons). Use an object directory per profile:
objects built with other flags are not rebuilt.

Profile `rpi4` keeps the instruction set floor (`-march=armv8-a+crc`; the
BCM2711 has no crypto extension) and adds:

- `-mtune=cortex-a72`: scheduling and cost decisions for the A72.
- `-mno-outline-atomics`: atomic operations as inline exclusive load/store
  loops. GCC 13 otherwise calls libgcc's `__aarch64_*` helpers, which pick
  LSE instructions at run time. Haiku's libgcc never sets
  `__aarch64_have_lse_atomics`, and the A72 has no LSE anyway: each atomic
  operation (Haiku's `atomic_*()`, `std::atomic`, reference counts) was a
  call plus a flag load for nothing.

Hand-written assembly is unchanged. `tools/rpi4/mesa/build.sh` gives Mesa the
same flags (`RPI4_MESA_CPU_FLAGS=` builds generic code).

No native measurement exists yet: the Pi was not connected. The gain is the
removed call per atomic operation plus A72 scheduling; how much that is in
applications is for the native comparison at the end of this page.

## Code generation, generic against tuned

Same source, two build directories (`/mnt/HaikuWork/rpi4/build-tune-base`,
`build-tune-a72`), `tools/rpi4/compare-cpu-profile.sh`:

| Binary | Helper calls, generic | Helper calls, tuned | `.text` change |
| --- | ---: | ---: | ---: |
| kernel_arm64 | 1194 | 4 | +1.27% |
| libroot.so | 96 | 4 | +0.54% |
| libbe.so | 60 | 0 | +1.87% |
| app_server | 13 | 0 | +0.98% |
| registrar | 157 | 0 | +1.07% |
| libmedia.so | 34 | 0 | +2.03% |
| libtracker.so | 34 | 0 | +1.15% |
| libEGL_mesa (Mesa) | 922 | 0 | +0.19% |
| libvulkan_broadcom | 57 | 0 | +0.06% |

The remaining four calls are in the prebuilt libsupc++ (`set_terminate`,
`set_new_handler`, exception cleanup). The prebuilt `gcc_syslibs` package
(libstdc++) still uses the helpers.

## Bug found: `atomic_test_and_set()` was a weak exchange

`headers/os/support/SupportDefs.h` implemented `atomic_test_and_set()` and
`atomic_test_and_set64()` with `__atomic_compare_exchange_n(..., weak = 1,
...)`. A weak exchange may fail spuriously; the function then returns
`testAgainst` unchanged, which callers read as success. The outlined helpers,
x86 `cmpxchg` and LSE `cas` are all strong, so this never showed. With
inline atomics a single `ldaxr`/`stlxr` attempt can fail, and the first
tuned QEMU boot panicked in the kernel's page-access debugging
(`first-tuned-weak-cas.log`). Both functions are strong now.

## QEMU runs stale translated code after add-on reloads

The first tuned images stopped in QEMU while probing drivers: a NULL
`gDeviceManager`, a GOT entry of 0, a jump into relocation data. The generic
image failed the same way about once in seven boots. All of it is QEMU's,
not air/OS's:

- The kernel's FNV hash of each segment right after loading matches the
  file on the build host; KDL and the QEMU monitor show the right code,
  GOT entries and `gDeviceManager` at the time of the fault.
- With a single CPU it still happens, deterministically.
- The registers at the fault hold pc-relative values (`adrp` results, the
  link register) that are off by exactly the distance to another virtual
  address: translated code of an earlier add-on that occupied the same
  addresses ran instead.
- `-accel tcg,one-insn-per-tb=on` boots every time; QEMU 9.2.4 and 10.2.0
  both fail without it. A global `tlbi vmalle1is` or a 200 us delay after
  each `load_kernel_add_on()` also hide it, page-level `tlbi` does not.

`tools/rpi4/qemu-run.sh` therefore uses one instruction per translation block
(`RPI4_QEMU_TB=multi` for the old behaviour). The full image then needs about
1200 seconds instead of 240 to reach the desktop; tuned and generic images
both get there without a panic (`qemu-resolution/long-*`). Experiments that ruled out the
kernel (page scrubber, page queue debugging, live page tracking, segment
re-reads, SD driver, boot loader block mappings, TLB flushes on unmap) are
in the evidence directory with their scripts: `qemu_kdl.py` runs KDL
commands at the panic, `qemu-probe.sh` QEMU monitor commands.

Found on the way and fixed: `load_kernel_add_on()` wrote add-on code through
a data mapping and never synchronised the instruction cache (real hardware
can run stale instructions from reused pages). Kept as a patch, not applied:
`rpi-loader-no-blocks.patch` (the loader may map its regions with block
descriptors, which the kernel cannot split or unmap).

`FreeTable()` flushed the address without the kernel's upper bits (it
computed `fullVa` and passed `nextVa`); fixed. It only runs for user maps
today, where both are the same.

## Firmware settings considered

- `arm_boost=1` is already in `data/boot/rpi/config.txt` (1.8 GHz).
- `force_turbo=1` sets the board's overclock (warranty) bit: not used.
- `initial_turbo` defaults to 60 s on current firmware; the loader sets the
  ARM clock itself anyway.
- `boot_delay` is not described by the current documentation; measure
  before changing.

## Native measurement, 26 October 2026

Three images from the same source (master 7ae85ccb57 plus this branch), the
same packages and firmware, written in turn to the lab Pi's SD card (Pi 4B
rev 1.5, 4 GB, 1.8 GHz with `arm_boost`): generic code, the `rpi4` profile,
and the profile with `-O3`. Each pass is a fresh boot, one minute of idle,
then the benchmark set; generic and tuned passes alternated (ABBA). Probe
binaries other than `rpi4_atomic_bench` were one build for all images, so
only the system under them differed. Passes: generic 3, `rpi4` 4, `-O3` 2.
Values are medians of the passes; lower is better.

| Workload | Generic | `rpi4` | `rpi4` + `-O3` |
| --- | ---: | ---: | ---: |
| `atomic_add()`, ns | 15.57 | 15.02 (-3.6%) | 15.02 (-3.6%) |
| `atomic_test_and_set()`, ns | 9.45 | 8.90 (-5.9%) | 8.90 (-5.9%) |
| `std::atomic` compare-exchange, ns | 23.35 | 22.79 (-2.4%) | 22.79 (-2.4%) |
| `atomic_add()`, 4 threads, one variable, ns | 51.5 | 47.2 (-8.4%) | 51.8 (+0.5%) |
| uncontended `pthread_mutex_lock/unlock`, ns | 56.19 | 57.82 (+2.9%) | 62.82 (+11.8%) |
| semaphore release/acquire round trip, ns | 1769 | 1755 (-0.8%) | 1757 (-0.7%) |
| 3000 small files: create, ms | 6444 | 5986 (-7.1%) | 6287 (-2.4%) |
| 3000 small files: remove, ms | 2441 | 2424 (-0.7%) | 2425 (-0.7%) |
| 64 MB area delete (resident / demand), us | 20742 / 35990 | 19755 / 34419 (-4.8 / -4.4%) | 19670 / 34569 |
| app_server drawing, 8 cases, ms | | -2.1% to +1.3% | -2.7% to +0.7% |
| StyledEdit launch to window, ms | 99.7 | 95.9 (-3.9%) | 95.2 (-4.6%) |
| AboutSystem / Terminal / Appearance launch | | -0.5 / -0.2 / -0.3% | -3.2 / -2.5 / -2.3% |
| 512 MiB TCP receive (Ethernet), ms | 5936 | 5903 (-0.6%) | 6540 (+10.2%) |
| Kernel / libroot / libbe text size | | +0.8 / +0.3 / +0.7% | +12.4 / +8.9 / +3.2% |

Run-to-run noise: drawing about 2%, TCP about 15% between single runs
(three per pass), the file test about 10%. Atomic and mutex timings repeat
to within 0.1%: the uncontended mutex really is 1.6 ns slower tuned, although
the tuned lock path is the generic one with the libgcc call replaced by an
inline exclusive load/store loop; layout is the likely cause. The contended
four-thread mutex numbers scatter by 2x between passes and are not
used. Mesa (`rpi4_mesa_cpu_bench`, generic and tuned `libEGL_mesa.so.0` from
identical source swapped on one boot, 8 alternating pairs): shader compile
-1.0% (every pair), draw calls unchanged (GPU bound), texture upload +1.0%
(within its noise; the first 4 pairs gave -1%).

Decisions:

- **`rpi4` profile at `-O2`: adopted** for the kernel, loaders, userland and
  Mesa of every Pi image. Small, consistent gains (atomics, file system,
  VM, launch), nothing outside noise slower except the 1.6 ns mutex case.
- **`-O3`: rejected.** One to three percent in drawing and launches does not
  pay for a 12% larger kernel, the slower mutex, losing the file-system
  gain, and the TCP result. It also breaks the build: GCC 13 at `-O3`
  reports 65 `maybe-uninitialized`, 2 `nonnull` and 3 `stringop-overflow`
  warnings that `-Werror` turns into errors (pci, bfs, elf, signal, ...);
  the experiment had to demote them.
- Not worth more experiments without a new idea: the remaining CPU cost of
  these workloads is in hand-written assembly (memory copies), the GPU, the
  SD card and prebuilt packages (FFmpeg, WebKit, libstdc++), not in code
  generation.

The images reached the desktop on every boot (9 boots, three images).
`tools/rpi4/UserBuildConfig` (lab) and `tools/airos/ci/UserBuildConfig`
(`@airos-rpi4`, CI) both call `SetCPUProfile arm64 : rpi4`; airos-ci builds
Mesa's V3D and V3DV with the same flags and rebuilds the Pi's objects when
the profile changes, because jam does not rebuild objects for changed flags.
Revert with `RPI4_CPU_PROFILE = none ;` (lab) or by removing the call (CI).

Lab procedure without the HA plug (it powers the Dragon since 24 October):
`evidence/cpu-tuning-native-20261026/pi-flash.sh <image>` attaches the
recovery disk on the NanoKVM (now 192.168.1.30), restarts air/OS from telnet,
writes and reads back the card from Alpine, releases the disk inside Alpine
(else the gadget answers "Resource busy"), detaches it and reboots into the
card. `pi-bench.sh`, `pi-mesa-ab.sh` and `summarize.py` there repeat the
measurement.
