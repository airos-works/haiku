# First-boot storage and timezone setup

The `rpi4-airos` SD image includes `apps/rpi-installer` from the workspace,
packaged as `rpi_installer` / `/boot/system/apps/RPiInstaller`. On the first
desktop start, `data/boot/rpi/UserBootscript` launches it with `--first-boot`.
It offers to use the entire SD card for air/OS. Keeping the current size is
a persistent choice. Either storage choice proceeds to **Step 2: Time zone**;
closing an unfinished step or choosing Later leaves it available next boot.
Applications → First-Boot Setup (named Raspberry Pi Setup before version
1.2) can revisit the choice.

Since version 1.2 the Radxa Cubie A7S images (`airos-cubie` in CI and the
non-lab `cubie-airos` profile) carry the same package, and
`data/boot/cubie-a7s/UserBootscript` starts it the same way. See
[the Cubie section](#radxa-cubie-a7s-gpt-layout) below. A system on a layout
the app does not know (for example an installation on the Cubie's eMMC)
shows the reason and offers **Skip this step**, which records the storage
step as declined and continues to the time zone.

The app identifies the partition mounted at `/boot`, validates the specific
MBR FAT32 + BFS image layout and its growable BFS superblock, and displays
the current and available capacities. It requires explicit confirmation
before changing the partition table. A backup, decision and pending state
live under `~/config/settings/rpi-installer/`. It flushes and reads back the
new partition table, offers timezone selection, then a normal restart.
Storage completion is recorded only after the enlarged mounted filesystem
is observed on the next boot. A separate timezone marker allows installations
with storage already configured to complete the new step.

Version 1.1 stores the timezone in Haiku's locale settings and uses a UTC
clock, preserving the UTC instant when a zone changes. The image launches
`rpi-time-sync`: it seeds the missing RTC from a build timestamp or saved
successful sync, retries NTP indefinitely with bounded attempts, and refreshes
it hourly after success. `Time --update` now returns failure when NTP fails.
The official CA certificate package and legacy Summit path are included, since
a correct date cannot compensate for a missing trust store.

## Why the image has reserved metadata

General BFS resizing is still experimental and cannot safely relocate open
inodes on a running boot volume. This image uses a separate, opt-in format
parameter: `block_size 4096; growable true`. It reserves 64 MiB for the bitmap
and 16 MiB for the journal, enough for the MBR limit of 2 TiB. Those blocks
are marked allocated from creation; all normal file data starts after them.

After the user extends the partition, the next mount replays the journal
and grows BFS before publishing vnodes or initializing its allocator. It
clears only bitmap bits beyond the old filesystem size, flushes the device,
then commits the new block and allocation-group counts. Existing files,
inodes, indexes and journal locations do not move. Partly prepared bitmap
space can be cleared again on retry while the original size remains valid.
The operation never shrinks, never grows an ordinary BFS volume, and never
writes during a read-only mount. The general resize ioctl is disabled for
this reserved layout so it cannot reclaim the metadata reservation.

The MMC driver implements the flush completion barrier for SD cards by
waiting for CMD13 to report READY_FOR_DATA in transfer state. MMC cards with
an enabled cache also perform their existing explicit cache flush.

## Radxa Cubie A7S (GPT layout)

The Cubie's SD image (`tools/cubie-a7s/build-sd-image.sh`) is GPT: the BSP
boot chain in the first 16 MiB (boot0 at sector 256, the boot package at
12 MiB), an EFI system partition at sector 32768, then the BFS volume, with
sgdisk's 128-entry partition arrays at both ends of the image. Its BFS uses
the same growable format (`HAIKU_BFS_GROWABLE` in both the CI and the
`cubie-airos` profiles), so only the partition table work differs.

The app accepts exactly that layout: a protective MBR, a valid primary
header and array (both CRCs), the ESP type at sector 32768, the BFS type as
the second entry at the mounted `/boot` offset, no other entries, and the
backup header directly after the backup array. On consent it saves LBA 0-33,
writes the new backup array and header to the card's last 33 sectors, then
replaces LBA 0-33 in one write: the protective MBR's size, the BFS entry's
end (the card's last usable sector, rounded down to 4 KiB), and the
primary header's alternate LBA, last usable LBA and CRCs. Each write is
flushed and read back. The boot chain and ESP are not touched. Until the
primary table is replaced it still names the image's original backup, which
stays intact, so an interruption leaves a consistent table. Haiku's GPT
module reads the primary header and follows its alternate LBA, as it does
for an image flashed to a larger card.

`make check-gpt` in the app builds that layout with sgdisk around a real
growable BFS volume and expands sparse copies as 32 GiB, 128 GB, odd-sized
128 GiB and 2 TiB cards. `sgdisk -v` finds no problems; the extracted
partition mounts grown to the new size with the original file intact and a
clean `checkfs -c`; planning again changes nothing; extra partitions, wrong
types, damaged CRCs and an ordinary BFS volume are refused.

Native validation (2026-10-29) used a copy of the CI image
`20261029-0202-hrev60207-2106-gf8eac33d0a` with the 1.2 package and this
boot script added, as a 64 GiB NVMe disk on QEMU's `virt` machine (AAVMF
boots the image's own `EFI/BOOT/BOOTAA64.EFI`). The first desktop showed
step 1 with 3.50 → 63.92 GiB; expansion left `sgdisk -v` clean with the BFS
entry ending at sector 134217687; Australia/Hobart applied at once; after
Restart now, BFS logged "Reserved-space growth to 68635570176 bytes" and
setup did not return. Opened from Applications → First-Boot Setup it
reported the card fully used. Offline afterwards: `checkfs -c` clean,
`decision` = `expanded 68635570176`, the saved 17408-byte table equal to
the image's LBA 0-33, and sectors 34-163839 (boot chain and ESP)
unchanged. A card with an extra GPT entry showed the reason and **Skip this
step**, which led to the time zone. Evidence:
`/mnt/HaikuWork/artifacts/rpi-installer/cubie-qemu/`. A physical Cubie card
has not been expanded yet.

## Building a distributable image

Build the installer package, stage the application's packages (including the
desired Summit and summit_webkit revisions), then build `rpi4-airos`:

```sh
bash tools/airos/build-arm64-app-packages.sh \
  /mnt/HaikuWork/rpi4/packages-arm64 rpi_installer
bash tools/rpi4/stage-packages.sh
jam -q -j6 @rpi4-airos build airos-rpi-image
```

In the build directory's `UserBuildConfig`, set `RPI4_LAB_IMAGE = 0 ;`
before including `tools/rpi4/UserBuildConfig`. This excludes lab credentials
and services, regardless of the local lab-image marker. An optional
`RPI4_IMAGE_PACKAGES` points at an isolated staging directory. The image
build fails if its package directory has no rpi_installer package. The
minimum bring-up profile retains its existing format and startup behavior.

## Verification

The app's `make check` validates capacities, arithmetic boundaries, unrelated
partitions, boot-device identity, malformed superblocks, already-full cards,
and repeat expansion. `tests/test_bfs_growth.py` runs the
actual BFS implementation through the host `bfs_shell`: 128 GB, 128 GiB and
2 TiB growth, partial bitmap blocks/bytes, retried preparation, original-file
hashes, a new file larger than the original filesystem, remounts, read-only
mounts, ordinary BFS, rejected versions/sizes, and `checkfs -c`.

Native validation uses a disposable copy of the complete Pi image on a
128 GiB QEMU disk. An EFI loader added only to that copy permits the `virt`
machine's USB input to exercise the UI. The production FAT contents remain
the Pi firmware boot path. Hardware validation on a physical 128 GB card
is separate from these emulator and filesystem checks.

The first native run on 2026-10-07 passed the initial prompt, decline and
reboot without a repeat prompt, manual reopening, cancel without changes,
confirmed expansion, graceful restart, growth from 3.50 GiB to 127.75 GiB,
and a further boot without a repeat prompt. After shutdown, `checkfs -c`
reported 435 nodes and zero missing, duplicate or reclaimable blocks. The
partition backup matched the original image MBR byte for byte and the
installed app package's SHA-256 was unchanged. Evidence lives under
`/mnt/HaikuWork/artifacts/rpi-installer/qemu/`; the filesystem and planner
test logs are beside it. These checks used the validation image while the
latest Summit rebuild was in progress.

The merged Pi kernel also passed growth through QEMU's `raspi4b` SD
controller: `/dev/disk/mmc/0/1` mounted at 137166323712 bytes after the SD
flush barriers succeeded. The setup window reported 127.75 GiB and a host
`checkfs -c` after stopping the VM reported 418 nodes with no missing,
duplicate or reclaimable blocks (`sd-current.log`, `sd-current.png`, and
`sd-current-checkfs.log` in the same artifact directory).

The build server's `airos-rpi4` profile uses the same growable format and
requires `rpi_installer` in `AIROS_CI_PACKAGES`. Its image pipeline selects
this Pi-only package from the ARM64 package pool.

A validation image (`505134964a`, 2026-10-07) included the build server's fresh
Summit `60f37cb` packages: `summit-0.1.0~git20261007.0055-1-arm64` and
`summit_webkit-1.10.1~git20261007.0055-1-arm64`. The package hashes extracted
from the image match the staged copies. Its installer expanded a decimal
128 GB disk to 127727370240 filesystem bytes (118.96 GiB) and restarted
successfully. Summit rendered its start page and a local JavaScript test.
The final Pi SD-controller boot also displayed first-boot setup. Evidence
is in `final-qemu/`, `final-sd.log`, `final-sd.png`, and `final-verification/`.

An incremental build crossing the Pi optimization merge can retain old
merged libc objects or a Zstd reader compiled without `ZSTD_ENABLED`.
Rebuild the stale aggregates/readers or use a clean output directory. The
final image was tested after rebuilding those objects; no application
package failed to load. The compressed deliverable and source/package
manifest are in the artifact directory's `release/` subdirectory.

The delivered image uses Haiku `b6854ab9f8` and the newer completed Summit
build `cfbe5f5` (`git20261007.0112-1` packages, including certificate details).
Its fresh boot displayed setup, loaded all packages, and rendered a local
JavaScript test. All 42 package hashes extracted from the image matched
their expected inputs. The xz stream expands to the exact raw-image SHA-256.
The final evidence is in `latest-qemu/`, `latest-verification/`, and the
release manifest. The physical Pi test remains separate.

The 2026-10-07 reliability candidate also passes both onboarding paths with
Hobart selected in QEMU, restart persistence, UTC preservation, and an offline
check of the expanded 128 GiB test disk (434 nodes, no allocation errors).
The user's physical 128 GB card is already correctly expanded and passes its
own pre-change BFS check (496 nodes, no allocation errors). Its boot failure
is reproduced with NanoKVM's empty USB LUN, and resolved by leaving valid idle
media attached. See `STATUS.md` for native clock, HTTPS and hotplug evidence.
