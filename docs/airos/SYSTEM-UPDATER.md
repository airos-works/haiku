# System Updater

air/OS keeps itself current from the build server: **System Updater**
(Deskbar › Applications) finds a newer CI build of the whole system, shows
what changed, downloads it and installs it at the next restart. Every
installed version stays bootable until two newer ones have proven
themselves, from the boot loader and from System Updater. A version that
does not start is replaced by the one before it without help.

## Using it

- **System Updater** (Deskbar › Applications) checks when it opens. *Install
  and Restart* downloads the release, checks every file, installs it for the
  next start and restarts. Nothing changes in the running system.
- After the restart the update is *on trial*: once the desktop has stayed up
  for half a minute it is confirmed. If it does not get there twice (a hang,
  a panic, no desktop), the next start is the version before it, and System
  Updater offers to try again or to remove the update.
- *Versions* lists what is installed: *Restart into This Version* starts one
  for a single start, *Go Back to This Version* makes it the current one.
  The boot loader's *Select boot volume/state* menu (hold Shift or Space
  while the computer starts) offers the same versions by name.
- Each start the desktop runs a boot check (`SystemUpdater --boot-check`),
  which also looks for a new release and says so in a notification.
- The command line does everything the window does: `SystemUpdater --status`,
  `--check`, `--plan`, `--update`, `--restart-into <state>`, `--go-back
  <state>`, `--retry`, `--remove-failed`, `--confirm`, `--prune`.
- The update server is `http://192.168.1.200/updates/<target>/`, named by
  the installed release; *Versions* › *Update server* overrides it.

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

CI builds some packages again for every image although their sources did not
change (the Office and Media apps after each Haiku commit, Vision's Deskbar
repack): same file name, same contents, other file times, another SHA-256.
`release-feed.py` compares each such package with the release before it,
entry by entry and attribute by attribute without the times, and lists the
earlier SHA-256s as `equivalent`; System Updater keeps an installed copy whose
SHA-256 is among them. Between two CI builds of 26 October this cut an
update from 24 packages (468 MiB) to 13 (249 MiB).

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

### The clock

Package states are named after the time they were made and ordered by
name, so the clock has to be about right. Boards without a battery-backed
clock start in 1970 until they reach a time server: when the clock is
earlier than the installed release's own build, System Updater sets it from
the build server's `Date` header, and installs nothing until it is right.

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

A release image has no Haiku package repository (upstream Haiku's `haiku`
package would replace air/OS's) and no Deskbar entry for SoftwareUpdater,
which would swap the release's own builds for HaikuPorts' versions.
HaikuDepot keeps HaikuPorts for adding software; a package of the release
that it updates is put back to the release's build by the next update.

## Tested

In QEMU on the build server (2026-10-26), with releases built by this
branch's CI scripts from the last CI build's inputs:

- **x86_64** (OVMF, KVM): installed with the Installer onto a blank disk
  (payload links in place, the NVIDIA modeset driver loaded through its
  link); update A → B: 10 packages replaced, among them the optional
  `haiku_devel`, the developer tools kept; the running system kept reading
  packages the deferred commit had moved (packagefs found them in the new
  state); trial start, confirmed after one start, the EFI loader updated, the
  oldest version pruned.
- A release whose desktop never comes up: two counted starts, then the loader
  started the version before it by itself; System Updater said so and
  *Remove the Update* made that version current again.
- The boot loader menu names every version by its release; starting an older
  one from it, *Restart into This Version* (one start, with that version's own
  kernel), *Go Back to This Version*.
- A package commit cut short (simulated on the disk image): the loader
  started the newest old state, System Updater offered *Repair*, which made
  the system start normally again.
- **arm64** (UEFI, TCG): install, update, trial start and confirmation; the
  clock set from the build server on a machine that believed it was 1970.
- **Raspberry Pi 4** (QEMU raspi4b): the kernel, not the loader, counted a
  trial start and started the fallback state.
- **Raspberry Pi 4** (the board, 2026-10-26): the CI image of one build
  flashed, updated by its own System Updater to the next build: the
  firmware loaded `airos/20261026.0439/airos-loader.img` and its boot archive
  as `airos-boot.txt` named them, and the trial was confirmed. *Restart into*
  the older version started the image's own pair at the top of the FAT
  partition (kernel hrev60207+1942) for that one start, and the boot check
  pointed `airos-boot.txt` back at the current version. The first download
  attempt stopped half-way ("Interrupted system call"); such failures are
  now tried again, resuming the file.
- **Cubie A7S**: the image and its release build; not booted.

From CI to CI (2026-10-26), images and feed from the `air/OS images`
workflow: the x86_64 image of one build installed with the Installer; System
Updater (the copy in that image) found the next build, showed its changes,
downloaded it and restarted; the trial was confirmed after one start and
the EFI loader on the ESP replaced (the old one kept as
`BOOTX64-previous.EFI`). *Restart into This Version* started the older
build once with its own kernel; the new loader's menu named both builds;
*Go Back* made the older one current again (its first start met the stall
below, the second confirmed it); an update from a feed with `equivalent`
packages then kept their installed copies. The arm64 image of a CI build
installed and was updated to the next CI build the same way (trial
confirmed, `BOOTAA64.EFI` replaced). Two more CI builds later the x86_64
machine, now with a System Updater that reads `equivalent`, went to the next
build with 11 packages (153 MiB) instead of the whole set, and the
confirmation pruned it to the current version and two older releases.

Known: about one start in fifteen in QEMU (and one smoke test in forty on
CI) stops before the desktop, also without System Updater; an update that
meets it twice in a row falls back and can be tried again. When it happens,
neither net_server nor app_server has started (no network or graphics driver
loads after the package daemon's volume check), every CPU idles in
`halt_idle`, and an NMI does not reach the kernel debugger: some server
everything waits for (the registrar?) is stuck. It is older than System
Updater; only the launch file's single `requires` (first-login) goes through
the launch_daemon change of this branch, and that is in the user session.
