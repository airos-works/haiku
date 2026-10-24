# Raspberry Pi 4 CPU tuning

Work of 24 October 2026, following the ARM64 profiles proposed in
[SYSTEM-PERFORMANCE.md](../airos/SYSTEM-PERFORMANCE.md). The Pi was not
connected to the lab that day, so nothing here is a native measurement.
Evidence: `/mnt/HaikuWork/rpi4/evidence/cpu-tuning-20261024`.

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

## Native measurement, when the Pi is back

1. Flash or install `build-tune-base`, then `build-tune-a72`, alternating
   (ABBA), same packages and firmware.
2. `rpi4_atomic_bench` (lab image, `tools/rpi4/atomic_bench.cpp`) built in
   each directory: uncontended `atomic_add`, `atomic_test_and_set`,
   `std::atomic`, mutexes, a semaphore round trip and four-thread contention.
3. The existing workloads in [PERFORMANCE.md](PERFORMANCE.md): app launch
   (`rpi4_app_bench`), drawing, memory, VM, the browser DOM fixture and
   boot markers.
4. Keep the profile only if nothing regresses; `RPI4_CPU_PROFILE = none`
   and `RPI4_MESA_CPU_FLAGS=` undo it.
