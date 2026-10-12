# How air/OS boots on the Raspberry Pi 4

No UEFI and no U-Boot: the VideoCore firmware starts the air/OS loader the way
it starts a Linux kernel.

```
EEPROM bootloader
  -> start4.elf, fixup4.dat          from the FAT partition
       reads config.txt, loads:
         bcm2711-rpi-4-b.dtb + overlays/miniuart-bt.dtbo
         airos-loader.img            kernel=..., kernel_address=0x80000
         airos-boot.tgz              initramfs ... followkernel
         cmdline.txt                 -> /chosen/bootargs
  -> armstub (in start4.elf)         GIC, timer; cores 1-3 wait on a spin table
  -> airos-loader.img (haiku_loader.rpi), core 0, EL2, MMU off, x0 = device tree
  -> kernel_arm64 from the boot archive
  -> the BFS partition on the SD card
```

## The loader (`src/system/boot/platform/rpi`)

| File | What it does |
| --- | --- |
| `entry.S` | Moves the image to its link address if needed, drops from EL2 to EL1, stack, BSS. Entry for cores 1-3 |
| `mmu.cpp` | Identity map with the caches on as the very first thing (with the MMU off all memory is device memory, and unaligned accesses fault). Physical allocator on the kernel_args ranges. The kernel's page tables |
| `dtb.cpp` | Memory, reservations, the boot archive (`linux,initrd-start/end`), `bootargs`, UART, GIC, CPUs. Hands the kernel a copy of the tree with the firmware's clock rates added |
| `mailbox.cpp` | Firmware property interface: frame buffer, clock rates |
| `video.cpp`, `console.cpp` | Frame buffer of the size of the display on HDMI0; text console on it, mirrored to the UART; keys from the UART |
| `cpu.cpp` | Raises the ARM clock from the firmware's 600 MHz to the maximum; `system_time()` from the generic timer |
| `devices.cpp` | The only "disk" is the boot archive in memory |
| `smp.cpp` | Starts cores 1-3 through the spin table |
| `start.cpp` | Order of all this; `cmdline.txt` options; kernel handoff |

The kernel is the same arm64 kernel as on EFI machines and is built against
the EFI platform's kernel_args.

The image file contains its BSS and stack: the firmware may place the boot
archive right behind the file.

## The boot archive

`jam airos-rpi-boot-archive` (rules in `build/jam/images/RpiBootImage`) packs
`system/kernel_arm64` and the boot modules: fdt, mmc, pci, scsi, usb bus
managers; bcm2711_emmc2, ecam, xhci; mmc_disk, usb_disk, scsi_disk; bfs,
packagefs; the partitioning systems. They are the same builds that go into
`haiku.hpkg`: rebuild the archive whenever the system package changes.

The loader registers the boot as "from an image", so the kernel takes the
first BFS partition with a system on it.

## `cmdline.txt`

The firmware prepends arguments meant for Linux. The loader looks for whole
words:

| Word | Effect |
| --- | --- |
| `airos.debug` | Loader debug output on the screen, no splash; the kernel gets `debug_screen true` |
| `airos.menu` | Boot menu (also: a space on the serial console at start) |

## The card image

`jam -q @minimum-rpi4 build airos-rpi-image`:

| Partition | Type | Contents |
| --- | --- | --- |
| 1, 256 MiB at 4 MiB | `0x0c` FAT | firmware files from `HAIKU_RPI_FIRMWARE_DIR`, `config.txt`, `cmdline.txt` (both from `data/boot/rpi`), loader, boot archive |
| 2 | `0xeb` BFS | the system |

Firmware files come from `tools/rpi4/fetch-firmware.sh` (pinned tag, sha256
recorded next to them); they are not in the tree.

For the full image, the configured arm64 bootstrap build also needs
`tools/rpi4/build-zstd.sh`. It creates pinned Zstandard runtime, development
and source packages in `/mnt/HaikuWork/rpi4/zstd/packages`, supplying the
feature missing from the bootstrap package repository. Run the full build
once to rebuild the host package tool with that feature, then run
`tools/rpi4/stage-packages.sh` and build `@rpi4-airos` again. Staging
recompresses application package copies with Zstandard without changing
their files, attributes or original release packages. Set
`RPI4_PACKAGE_COMPRESSION=zlib` when staging for an older system.

The bootstrap repository has no image libraries either, and without them the
build leaves the JPEG, PNG and WebP translators out of `haiku_datatranslators`
(and MJPEG out of the USB webcam add-on): nothing that reads pictures through
the Translation Kit opens a JPEG or a PNG. `tools/rpi4/build-image-libs.sh`
cross-builds libjpeg-turbo 3.0.4 (libjpeg 6.2 API), libpng 1.6.44 (from the
air/OS fork, `jmgasper/libpng` branch `airos-v1.6.44`) and libwebp 1.4.0 into
`/mnt/HaikuWork/rpi4/image-libs/packages`, as `libjpeg_turbo`, `libpng16` and
`libwebp` packages with their `_devel` counterparts, named and providing like
HaikuPorts'. `HAIKU_ARM64_IMAGE_LIBS_PACKAGES_DIR` points the `jpeg`, `libpng`
and `libwebp` build features at them; the Pi's and the Cubie's full images
ship the three runtime packages. `SOURCES` there records the pins, the
compiler and the package hashes.

Run `tools/rpi4/build-cxx-runtime.sh` before building the full image. It stages
the pinned GCC 13.3 C++ runtime with both SysV and GNU symbol hashes at
`/mnt/HaikuWork/rpi4/cxx-runtime/stage/libstdc++.so.6`. The script verifies the
installed bootstrap's feature configuration, exported and imported symbols,
dependencies and source revision before staging, and records a manifest.
The full image includes this library as a non-packaged override. A different
bootstrap GCC version requires a fresh compatibility check and script update.
`tools/rpi4/build-cxx-runtime-tests.sh` builds the two ABI probes and their
plugins for native/QEMU validation. Run each `probe-abiN` with the absolute
path to its matching `plugin-abiN.so`; `EXPECT_CXX_PROVIDER` checks the loaded
runtime path and `LIBRARY_PATH` can select an isolated comparison library.

The new image contains Zstandard support in the boot, kernel and user
package readers. An in-place upgrade from an older image must first install
a **zlib-compressed** copy of the new core package and the Zstandard runtime,
update its matching boot archive and loader, and reboot. Only then can it
activate Zstandard packages. A fresh SD image needs no transition.

## Things that were not obvious

- With the MMU off, GCC's unaligned accesses (packed kernel_args, inline
  memcpy) fault. Nothing but the MMU setup may run before the MMU is on.
- The other cores read memory uncached until their MMU is on: the loader
  flushes its image and their stacks from the cache before releasing them.
- The SD controller needs 32-bit register accesses with pauses between
  writes; the generic sdhci driver cannot be used.
- The Pi's `/soc` bus is not an identity mapping (0x7e000000 -> 0xfe000000),
  and `emmc2bus`/`scb` have their own `ranges` and `dma-ranges`.
- The PL011 driver's constructor leaves the baud divisor at 1; whoever uses it
  has to call `InitPort()`.
