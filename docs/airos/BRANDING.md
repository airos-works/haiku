# air/OS branding

![air/OS](airos-logo.png)

The air/OS mark is a disc: sky above a wave, a sun with a cream ring, a teal
band and a teal bottom, separated by cream gaps. The wordmark is `air/os` in
slate. The brand sheet these were taken from is
[`data/artwork/airos/brand-sheet.png`](../../data/artwork/airos/brand-sheet.png).

| Colour | Hex | Use |
| --- | --- | --- |
| Sky | `#A7D8F0` | sky, highlights |
| Breeze | `#4E8D8A` | waves, air/OS headings in *About this system* |
| Dawn | `#E3A25B` | sun |
| Cloud | `#F6F3EA` | gaps, sun ring, wordmark on dark backgrounds |
| Slate | `#2F3A40` | wordmark on light backgrounds |

## Where it appears

| Place | What changed | Source |
| --- | --- | --- |
| Deskbar menu button | mark and wordmark instead of Haiku's feather; a light-text variant on dark panels; centred instead of cut off at the bottom | `src/apps/deskbar/icons.rdef` (`R_LeafLogoBitmap`, `R_LeafLogoDarkBitmap`), `BarMenuBar.cpp`, `BarMenuTitle.cpp` |
| Desktop | the air/OS hills picture, scaled to fill, with outlined icon labels; installed for a new desktop and for one still set to Haiku's default logo (which air/OS does not ship) | `data/artwork/airos/airos-desktop.tga`, `src/kits/tracker/TrackerInitialState.cpp`, `build/jam/packages/Haiku` |
| Boot screen | the air/OS logo above the stage icons; the boot disk stage icon carries the mark | `headers/private/kernel/boot/images-airos.h` (selected in `images.h` for every non-official build), `data/artwork/boot_splash/airos_splash_*.png` |
| Boot volume on the Desktop | Haiku's hard disk with the mark where the leaf (or, in development builds, the bug) was | `src/kits/tracker/TrackerIcons.rdef` (`R_BootVolumeIcon`) |
| `/boot/system` folder | folder with the mark instead of the leaf | `TrackerIcons.rdef` (`R_BeosFolderIcon`) |
| *About this system* | air/OS logo, an air/OS section with the GitHub link above Haiku's own details, which are kept and introduced as what air/OS is forked from; the application icon is the mark | `src/apps/aboutsystem/` |
| Installer | air/OS logo | `src/apps/installer/` |
| DriveSetup | the mark flags the boot partition | `src/apps/drivesetup/icons.h` (`kLeaf`) |
| HaikuDepot | the mark is the "native package" badge | `src/apps/haikudepot/HaikuDepot.rdef` (560) |

The logos in *About this system* and the Installer are vector icons rendered
at a size that follows the font, not PNG resources. arm64 builds have no PNG
or JPEG translator (libpng and libjpeg are not available as arm64 build
packages), so a PNG logo would not show at all; for the same reason the desktop
picture is a run-length encoded TGA.

Removed: the Haiku3d demo (it renders the HAIKU letters) and the Leaves screen
saver, from the build, the regular image and the ROCK 5 profile's extra demos.

Left as they are: Haiku's own credits and trademarks in *About this system*,
and the Haiku logo artwork in `data/artwork`.

The `.VolumeIcon.icns` that Apple's boot picker (Option at power on) shows for
the USB stick and for a disk the Installer set up is the mark,
`data/artwork/airos/VolumeIcon.icns` (`generate.py --only mac-volume-icon`
rewrites just that file). The image build puts it on the EFI system partition
and the `haiku` package ships it next to the EFI loader for the Installer.

## Icon style

The icons follow Haiku's [icon guidelines](https://www.haiku-os.org/development/icon-guidelines/):
light from the top left, gradients for volume, and a dark outline drawn as a
stroke behind the shape, 4 units wide at normal sizes and 6 units below about
21 pixels. Below a set size the mark switches to a simplified drawing (sky,
one wave, the sun without its ring) through HVIF's level-of-detail shapes, so
it still reads at 16 pixels.

![icons](airos-icons.png)

The HVIF files are also in `data/artwork/airos/icons/`, where Icon-O-Matic
can open them.

## Regenerating

Everything above is generated from one description of the mark
(`tools/airos-branding/airos_art.py`, fitted to the brand sheet) and the traced
wordmark (`wordmark_paths.json`):

```sh
jam -q '<build>generate_boot_screen'          # in the build directory
python3 tools/airos-branding/generate.py \
	--boot-screen-tool <build>/objects/linux/x86_64/release/tools/generate_boot_screen
```

It needs Python 3 with pycairo, Pillow and numpy. It rewrites the HVIF data in
the `.rdef` files and `icons.h` in place, the boot screen PNGs and header, the
desktop picture, and the files in `data/artwork/airos/` and `docs/airos/`.
`hvif_render.py` renders HVIF on the host the way `IconRenderer` does, which is
how the contact sheet above is made. `trace_wordmark.py` is the one-off that
traced the wordmark from the brand sheet.

![desktop](airos-desktop-preview.png)

## Installed on the ROCK 5 ITX (2026-10-02)

The board's NVMe install ("Haiku NVMe") runs this branding:

- `haiku.hpkg` built from `airos-branding` at `hrev60097+342` under the
  `@rock5full-mmc` profile, SHA-256
  `f236f88a1c70fcc671ccc516432f4ca8c9a22ad8526c6eb1193165141b99a505`, copied
  over the installed `haiku-r1~beta6_hrev60097_335_dirty-1-arm64.hpkg` (file
  name kept). The package that was there before air/OS
  (`aa66e8ffb087e205c1d57bd638cbe5cee925cdeaaee9a44dbff383ded62f96ad`) is in
  `/boot/home/rock5-lab/airos-backup/haiku-before-airos.hpkg`, with the loose
  `Haiku3d` demo that was in `/boot/system/non-packaged/demos`.
- The ESP's `EFI/BOOT/BOOTAA64.EFI` is the loader built at `hrev60097+339`
  (`f91edce4…030d`), installed by `tools/rock5-itx/install-efi-loader.sh`; it
  differs from the package's `+342` loader only in the revision string. The
  loader from before air/OS (`4e5dfb49…1c31`) is `BOOTAA64.EFI.rock5-previous`
  next to it.

The first air/OS package (`280fa016…`) shipped `data/deskbar/menu_entries` as
an empty directory instead of the 251-byte virtual directory file, so the
Deskbar menu had no Applications, Demos, Desktop applets or Preferences. A
failed `SetType` in a parallel build had been worked around by hand with
`mkdir`; the file is now built by the normal rule, and the package was checked
entry by entry (type, mode, attributes, data file sizes) against the one it
replaces. The only other difference is `lib/libroot.so`, whose old copy had
icon and signature attributes that no rule gives it, most likely stale
entries in the shared build's emulated attribute store.

Seen over the NanoKVM after the reboots: the air/OS boot screen with the
stage icons advancing; the desktop picture, which replaced the Desktop's old
default (its attribute still named Haiku's logo, which this build never
shipped); the Deskbar logo and its full menu (Applications, Demos without
Haiku3d, Desktop applets, Preferences); the boot volume and `/boot/system`
icons; *About this system* (logo, `air/OS, Haiku R1/beta6 (arm64)`, the air/OS
and Haiku sections); DriveSetup's boot partition mark; no Leaves in
`add-ons/Screen Savers`. The same builds were first checked in QEMU from a
fresh image. Not seen on hardware: the Installer's logo window (its
information panel opens first), the dark Deskbar variant and HaikuDepot's
badge.

To go back, copy the backup package over the installed name, and
`BOOTAA64.EFI.rock5-previous` over `BOOTAA64.EFI`; or pick the previous
system state from the boot menu.
