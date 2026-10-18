# Deskbar menu

The Deskbar menu of an air/OS install has six folders, in this order:
Applications, Office, Utilities, Demos, Desktop applets and Preferences. Since
branches `airos-deskbar-menu` and `deskbar-utilities-root` (2026-10-17).

![The Deskbar menu before Office was added, and its Utilities folder](deskbar-menu.png)

| Folder | Entries |
| --- | --- |
| Applications | airShot, airTime, Amp, Burrow, DeskCalc, HaikuDepot, Icon-O-Matic, Installer, Kiri, Mail, MediaConverter, MidiPlayer, Pe, People, SoftwareUpdater, SoundRecorder, StyledEdit, Summit, Terminal, TurboChook |
| Office | Clipper, WordCraft, GridCraft, DeckCraft, PdfCraft |
| Utilities | CharacterMap, CodyCam, Debugger, Devices, DiskProbe, DiskUsage, DriveSetup, Expander, Magnify, PoorMan, SerialConnect, Vision |
| Demos | Chart, Clock, Cortex, FontDemo, GLTeapot, Mandelbrot, OverlayImage, Pairs, PatchBay, Playground, Pulse, Sudoku |
| Desktop applets | AutoRaise, BluetoothStatus, LaunchBox, NetworkStatus, PowerStatus, ProcessController, WiFiStatus, Workspaces |
| Preferences | Appearance, Backgrounds, Bluetooth, DataTranslations, Deskbar, E-mail, FileTypes, Input, Keymap, Locale, MCP, Media, Network, Notifications, Power, Printers, Repositories, Screen, ScreenSaver, Shortcuts, Sounds, Time, Tracker, VirtualMemory, WiFi (and Qt6 Configurator from the qt6_base package) |

WebPositive is gone: Summit is the browser (the MIME database already made it
the preferred application for web pages and http/https URLs). The fork no
longer builds the `webpositive` package or puts it in an image or in the
Haiku repository; the sources stay for Mail, which compiles WebPositive's
autocompletion classes. ActivityMonitor and MediaPlayer are still installed,
without a menu entry (airTime is the media player).

## Where each entry comes from

The menu is the merge of the `data/deskbar/menu` directories listed in
`data/deskbar/menu_entries`; packagefs merges the packages' own entries in
`/boot/system/data/deskbar/menu`.

- The haiku package: `DESKBAR_APPLICATIONS`, `DESKBAR_UTILITIES`,
  `DESKBAR_DESKTOP_APPLETS`, `SYSTEM_PREFERENCES` and `SYSTEM_DEMOS` in
  `build/jam/images/definitions/minimum` and `regular`, turned into links by
  `build/jam/packages/Haiku`. Debugger's entry used to be
  in haiku_devel; it is in the haiku package now, under Utilities.
- The applications air/OS CI builds (airShot, airTime, Amp, Burrow, Kiri,
  Summit, Turbo Chook): each package's own
  `data/deskbar/menu/Applications/<name>` link. Clipper and the four Craft
  applications supply `data/deskbar/menu/Office/<name>` links. The new Terminal and
  MediaConverter replace the in-tree apps under the same names, so their
  entries stay as they are.
- Pe: HaikuPorts' package (the nightly profile adds it), as it is.
- Vision: HaikuPorts' build, repacked by air/OS CI with its link in
  `Utilities` and the revision raised by 101 (0.10.6-103; airos-ci
  `deps/deskbar-repack.sh`, called by `images/build-image.sh`). The x86_64
  profile in `tools/airos/ci/UserBuildConfig` keeps the repository's own
  Vision out when CI staged one (`SuppressHaikuImagePackages`, a rule this
  branch added to `build/jam/ImageRules`).

Office has its localized name in `src/data/directory_attrs/deskbar-office.rdef`
and shares the same catalog and ordering mechanism. Utilities' localized name is `src/data/directory_attrs/deskbar-utilities.rdef`
with its catalog entry in `src/apps/deskbar/LocalizedFolders.h`. The menu
itself would sort its folders by name, which puts Utilities last;
`TDeskbarMenu::_OrderStandardFolders()` (`src/apps/deskbar/DeskbarMenu.cpp`)
puts the six standard folders first in their own order, by entry name, and
anything else (a folder a user adds to `~/config/settings/deskbar/menu`)
follows them, sorted as before.

Pe and Vision are x86_64 only: HaikuPorts has no arm64 builds, so the ROCK 5,
Pi and Cubie images have the same menu without those two entries.

The five-folder layout before Office was checked on x86_64 CI images with
the packages of 2026-10-17 and the repacked Vision, booted in QEMU: Vision
and Debugger start from Utilities. The screenshot above records that layout.
Office was added on 2026-10-18; both architecture packages include the
folder. Validation of the complete Office application set is still in progress.
