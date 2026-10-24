# Raspberry Pi 4 CPU tuning

Work of 24 October 2026, following the ARM64 profiles proposed in
[SYSTEM-PERFORMANCE.md](../airos/SYSTEM-PERFORMANCE.md). The Pi was not
connected to the lab that day, so nothing here is a native measurement.
Evidence: `/mnt/HaikuWork/rpi4/evidence/cpu-tuning-20261024`.

## The profile

`SetCPUProfile <architecture> : <profile>` (`build/jam/ArchitectureRules`)
adds code generation flags to userland, kernel and boot loaders of a build.
Call it from `UserBuildConfig`. The Pi build switches it on with
`RPI4_CPU_PROFILE = rpi4 ;` before including `tools/rpi4/UserBuildConfig`.
Use an object directory of its own: objects built with other flags are not
rebuilt.

Profile `rpi4` keeps the instruction set floor (`-march=armv8-a+crc`; the
BCM2711 has no crypto extension) and adds:

- `-mtune=cortex-a72`: scheduling and cost decisions for the A72.
- `-mno-outline-atomics`: atomic operations as inline exclusive load/store
  loops. GCC 13 otherwise calls libgcc's `__aarch64_*` helpers, which pick
  LSE instructions at run time. Haiku's libgcc never sets
  `__aarch64_have_lse_atomics`, and the A72 has no LSE anyway: each atomic
  operation (Haiku's `atomic_*()`, `std::atomic`, reference counts) was a
  call plus a flag load for nothing.

Hand-written assembly is unchanged. Mesa takes the same flags through
`RPI4_MESA_CPU_FLAGS` in `tools/rpi4/mesa/build.sh`.

**It is not the default.** Tuned images do not boot in QEMU; the cause is a
race that the baseline also hits (see below). Enable it after that race is
fixed and the native A/B below shows a gain.

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

## Blocker: kernel add-on loading race (QEMU)

After that fix, tuned images still stop in QEMU, while driver probing or
unloading kernel add-ons loaded from the system package:

- NULL `gDeviceManager` in `virtio_input` (its dependency pointer unset),
- `bindfs` calling a GOT entry that holds 0,
- `scsi_disk` writing a page past its own image, from an instruction that
  is a `mov` in the file. The page tables (read with the QEMU monitor) show
  the right code at that address.

The baseline image is affected too: it booted 6 of 7 times; the failure
was a NULL call in `net_server`'s thread (`final-base1.log`). The add-ons
involved load at `0xffff0000021xxxxx`, just above the loader's 32 MB kernel
window, in its freed ranges.

What changes the outcome (all with the `-mtune`-only build):

| Change | Result |
| --- | --- |
| `-mtune` only, or `-mno-outline-atomics` only | both fail |
| tuned kernel and boot modules, generic system package | boots |
| generic kernel, tuned system package | fails |
| system package stored uncompressed (not Zstandard) | fails |
| TLB flush in `flush_va_if_accessed` regardless of access flag | fails |
| loader maps only the physical map with block descriptors | fails |
| `tlbi vmalle1is` after each `load_kernel_add_on()` | boots 3/3 |
| I-cache sync only, or `tlbi` of the add-on's own range only | fails |
| 200 µs `spin()` after each `load_kernel_add_on()` | boots 3/3 |

A delay works as well as a TLB flush: this is a timing race, not a stale
translation. Something changes add-on memory after it is loaded and
relocated, or the add-on runs before data it depends on is in place. The
exact writer is not found. Look at what runs concurrently with `main2` at
`device_manager_init_post_modules` / `module_init_post_boot_device`, and at
what used the freed boot loader ranges (`vm_free_unused_boot_loader_range`
frees pages before it unmaps them). `qemu_kdl.py` in the evidence directory
runs KDL commands at the panic; `qemu-probe.sh` runs QEMU monitor commands.

Two related defects were found on the way and kept as patches in the
evidence directory, not committed, because neither can be validated while
the race shifts with any timing change:

- `addon-icache-sync.patch`: `load_kernel_add_on()` writes code through a
  data mapping and makes it executable with `set_area_protection()`, but
  never cleans the D-cache or invalidates the I-cache. `Protect()` does not
  either. Real hardware can run stale instructions from reused pages.
- `rpi-loader-no-blocks.patch`: the loader maps its regions with block
  descriptors when size and alignment allow; the kernel's `GetOrMakeTable()`
  can neither split nor unmap blocks (its assertion is compiled out in
  release builds). Only the physical map should use blocks.

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

1. Fix the race above; QEMU boots of both images must pass repeatedly.
2. Flash or install `build-tune-base`, then `build-tune-a72`, alternating
   (ABBA), same packages and firmware.
3. `rpi4_atomic_bench` (lab image, `tools/rpi4/atomic_bench.cpp`) built in
   each directory: uncontended `atomic_add`, `atomic_test_and_set`,
   `std::atomic`, mutexes, a semaphore round trip and four-thread contention.
4. The existing workloads in [PERFORMANCE.md](PERFORMANCE.md): app launch
   (`rpi4_app_bench`), drawing, memory, VM, the browser DOM fixture and
   boot markers.
5. Enable `RPI4_CPU_PROFILE` (and the Mesa flags) by default only for
   measured gains.
