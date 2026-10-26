# System Updater

air/OS keeps itself current from the build server: **System Updater**
(Deskbar › Applications) finds a newer CI build of the whole system, shows
what changed, downloads it and installs it at the next restart. Every
installed version stays bootable until two newer ones have proven
themselves, from the boot loader and from System Updater. A version that
does not start is replaced by the one before it without help.

Status: work in progress on branch `system-updater`.

## What an update is

A CI image build (`jmgasper/airos-ci`, `images/build-image.sh`) is one
*release*: every package in the image's `system/packages` (Haiku, the
drivers, Summit, the Office and Media apps, Kiri, airTime, Amp, Burrow,
Clipper, airShot, Proton Mail Bridge and VPN, Mail, the libraries and
firmware) plus what the image keeps outside packages (GPU stacks, NVIDIA,
the AMD user land, firmware files). An update installs the whole release:
there are no partial updates, so a machine always runs a combination that
CI built and smoke-tested together.

Each release carries two packages of its own:

| Package | Contents |
|---|---|
| `airos_release` | `data/airos/release`: the release id (`20261026-0915-hrev60207-1930-gabcdef0123`), target (`x86_64`, `arm64`, `rpi4`, `cubie`), build time, Haiku and airos-ci commits, and the update feed's address. Its version is the build time, `20261026.0915`. |
| `airos_payload` | `data/airos/non-packaged/…`: everything CI used to copy into `system/non-packaged`. The image has symlinks there instead (`system/non-packaged/lib/libvulkan_nouveau.so` → `/boot/system/data/airos/non-packaged/lib/libvulkan_nouveau.so`), so these files belong to a package state like every other system file, and an older state brings its own GPU stack back. Paths are unchanged: "Disable user add-ons" still turns them off, and a lab copy that replaces a symlink with a real file takes over as before. |

## The feed

The build server publishes each target's releases next to its images:
`http://192.168.1.200/updates/<target>/` (Haiku cannot resolve
`airos-build.local`).

```
index.json                    the releases, newest first, with their changes
builds/<release id>/update.json
packages/<sha256>/<file>.hpkg  every package once, by content
```

`update.json` lists every package of the release with its name, version,
file name, size, SHA-256 and address, the payload's symlinks, and what
changed since the release before it in the feed (Haiku commits, package
versions, application commits). System Updater checks each download's size
and SHA-256 and reads its package info before using it.

## On the machine

System Updater keeps its state on the system volume, in
`/boot/system/packages/administrative/airos/`:

| File | Written by | Read by | Purpose |
|---|---|---|---|
| `releases/<id>.json` | System Updater | System Updater | the `update.json` of each installed release: which packages it had, for removing ones a later release drops |
| `downloads/` | System Updater | System Updater | packages being downloaded; reused when an update is tried again |
| `next-boot` | System Updater | boot loader | a state to start once ("Restart into this version") |
| `trial` | System Updater | boot loader | an installed update that has not started yet: its release and the state to fall back to |
| `trial-attempts` | kernel | boot loader | one line per start of the release on trial |
| `log` | System Updater | people | what was done |

### Installing an update

1. Download every package the machine does not have yet; check size and
   SHA-256, read the package's name, version and architecture.
2. Make one package transaction (`BDaemonClient`): activate the release's
   new packages, deactivate the packages they replace and those the
   previous release had and this one dropped. Packages the user installed
   are left alone. The transaction is *deferred* (applied at the next
   start, as Haiku always does for an update of the `haiku` package): the
   running system is not changed, and the package daemon keeps the replaced
   packages in a new old-state directory (`state_<date>`).
3. Write `trial` (fallback: that new state), the release record and the
   payload symlinks for new files, then ask to restart.

### Starting it: trial, confirmation, fallback

- The boot loader sees `trial`. While `trial-attempts` has fewer lines than
  the trial allows (2), it starts the latest state and tells the kernel it
  is a trial start; the kernel adds a line to `trial-attempts` as soon as it
  has mounted the boot volume.
- Once the desktop has run for a while (Tracker and Deskbar up),
  System Updater's boot check deletes `trial`: the release is confirmed.
  Then it updates the EFI loader on the EFI system partition if the new
  `haiku` package has a different one, and removes old versions beyond the
  two it keeps.
- If the release never gets that far (a panic, a hang, a power cut), the
  next start finds the attempts used up and the loader starts the fallback
  state instead. System Updater then says the update did not start and
  offers to try again or to remove it.
- A transaction cut short (power loss) leaves a latest state whose system
  package is missing; the loader then starts the newest old state, which is
  the system as it was before the update.

### Older versions

The boot loader's *Select boot volume/state* menu names each state by its
release ("air/OS 26 Oct 2026 09:15, hrev60207+1930"). System Updater's
*Versions* list offers the same with two actions:

- **Restart into This Version** writes `next-boot`: the loader starts that
  state once, the kernel consumes the request, the next start is the
  current version again.
- **Go Back to This Version** makes it the current version: a transaction
  that activates its packages again (copied from the state directories) and
  keeps the current ones in a new state, so going forward again is the same
  action.

System Updater keeps the current version and the two before it; older
states are deleted from the oldest end only (a state needs the newer state
directories to find its packages).

## Platforms

| Target | Loader | Older versions | Notes |
|---|---|---|---|
| x86_64 (UEFI, BIOS) | `EFI/BOOT/BOOTX64.EFI` / `haiku_loader` package | loader menu, System Updater | |
| arm64 UEFI (ROCK 5, Dragon) | `EFI/BOOT/BOOTAA64.EFI` | loader menu, System Updater | |
| Cubie A7S | `EFI/BOOT/BOOTAA64.EFI` started by U-Boot | loader menu, System Updater | U-Boot and boot chain are not updated |
| Raspberry Pi 4 | firmware starts `airos-loader.img` + `airos-boot.tgz` from FAT | System Updater | the kernel comes from the FAT partition; each version keeps its own loader and boot archive there (see below) |

### The Raspberry Pi

The Pi's firmware loads `airos-loader.img` and `airos-boot.tgz` (the kernel
and the modules that mount the system volume) from the FAT partition, and its
loader cannot read BFS. So on the Pi:

- the kernel makes the boot loader's choice (`airos_choose_boot_state()` in
  `vfs_boot.cpp`) when the loader did not, from the same files;
- System Updater keeps each installed version's loader and boot archive in
  `airos/<version>/` on the FAT partition (the image's own pair stays at the
  top), and writes `airos-boot.txt`, which `config.txt` includes, so that the
  firmware loads the kernel of the version that starts next. An older
  `config.txt` that names the files itself is changed to include
  `airos-boot.txt` (the original is kept as `config-before-system-updater.txt`);
- an update's boot files go onto the FAT partition before its packages are
  committed, and are selected after;
- after a fallback the kernel that started is the update's: the fallback
  version runs on it until the next restart, which System Updater has
  already pointed at the fallback's own kernel.

## Building releases

`jmgasper/airos-ci` makes a release of every image build:

1. `images/release-inputs.py` writes the release file, the package infos of
   `airos_release` and `airos_payload` and `files.jam` (every file of CI's
   trees), which `images/build-image.sh` passes to jam.
2. After the image, `images/release-feed.py extract` copies its packages out
   with `bfs_shell` into the image directory's `release/`.
3. After the smoke test, `images/release-feed.py publish TARGET IMAGE_DIR`
   adds it to `updates/<target>/` with what changed since the release before
   it: the Haiku commits (`git log`), the packages whose versions changed and
   the commits of the pinned applications.
