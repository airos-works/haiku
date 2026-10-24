# air/OS on Raspberry Pi 4: current status

The lab board is a Raspberry Pi 4 Model B revision 1.5, 4 GB (`c03115`),
with two HDMI displays, Ethernet, the NanoKVM's USB HID devices,
and a USB thumb drive. The fresh-install investigation on 2026-10-07 uses
a 128 GB SD card; the earlier performance run below used 8 GB. Current
reliability fixes were merged into private `master` as `6ea9a7d410`.
Earlier bring-up notes are retained in `HISTORY-20261005.md`;
this page supersedes their older status statements.

## Performance follow-up (2026-10-08 evening)

The session started on native `hrev60206+750`. A Mesa buffer-cache metadata
fix is installed and hash-verified. A bounded 1,024-buffer growth/retirement
test reduces retained process heap from 24.2 to 2.2 MiB; repeated baseline
and installed-candidate runs agree. Pixel, texture and window-resize checks
pass; that change alone leaves window throughput unchanged. A second installed
Mesa change presents eligible EGL windows from shared cached GPU buffers,
removing one full-frame CPU copy. At 1280x720 it improves completed BView
draws from about 96 to 120 per second, reduces combined client/app_server CPU
per frame by about 19%, and saves 3.51 MiB of frame storage. These are EGL
window results, not monitor refresh or Summit scrolling results. Pixel,
32-context lifetime, process-loss, OpenGL Kit and installed regression checks
pass. The Pi's BGLView library now also avoids clearing the background before
its own complete drawing. At 1280x720, the isolated same-library comparison
improves completed draws from 116 to 146 per second and cuts app_server CPU
per frame by 22%. Empty views, resize gaps, software/fallback paths and 192
context lifetimes pass; the installed default repeats 146 draws/s. This is
an additional OpenGL Kit improvement, not a Summit scrolling result.
The V3D driver also retains fixed cache-walk bounds in registers instead of
reloading BO metadata per cache line. Its native profile reduces cache-walk
CPU from 3.174 to 0.884 sampled seconds over 3,000 frames. Matched 720p
before/after runs reduce client CPU by 31% and improve completed draws by
6%; graphics ownership, pixels and buffer-lifetime checks pass after an
orderly reboot. This kernel change can also affect cached texture readbacks.
AirPins 1.0.0-2 reuses unchanged toolbar icons on window attachment and keeps
temporary rasterization bitmaps local. Native warm first-window time falls
from 301 to 263 ms (13%), with about 11% less sampled startup/quit CPU.
Icon states, font-size changes and all three layouts match the baseline pixels;
the installed package repeats a 265 ms warm median and passes health checks.
New applications now use a libbe override that calculates the vector-icon
gamma table once and deep-copies it for each renderer. Native rendering CPU
falls about 20% for 16-pixel icons and 5.8% across the mixed-size fixture;
application launch time is effectively unchanged. Concurrent rendering,
672 pixel hashes, the editor UI and a QEMU default-library boot pass.
The later app_server qualification reboots also load this library into
app_server and registrar. Tracker and Deskbar retain their adjacent packaged
libraries under Haiku's application-local search rule. App_server now batches opaque copy-mode scanline
stores and combines exact partial-coverage color arithmetic. Four native runs
per variant across alternating boots cut offscreen drawing CPU by 8–13% at
640×480 and 4–8% in the larger fixture. All 192 bitmap hashes agree; exhaustive
arithmetic, cached/uncached graphics mappings, 1,152 EGL frames and BGLView
pixels/lifetimes pass. The packaged server remains intact behind a removable
launch-service amendment, and the rollback boot and final 20 health checks pass.
This is a drawing-workload gain; tiny opaque spans can cost a few extra ns and
large uncached spans are essentially unchanged.
An ARM64 whole-page zeroing candidate passed integrity checks but did not
improve warmed native allocation workloads, so it was rolled back. The original
kernel and boot archive are restored. USB recovery, kernel rollback and a
reusable allocation benchmark are now verified; the seven installed changes
above remain the accepted performance improvements.
An eighth change adds GNU symbol hashes to the pinned GCC 13.3 C++ runtime.
Same-object, alternating native runs reduce warm startup client CPU by about
4 ms: 4.3% for StyledEdit and 2.3% for AirPins. First-window time improves by
4–5 ms. The cost is 64 KiB of additional read-only mapped space; writable
storage is unchanged. The original ABI, imports and feature configuration are
preserved. Both C++ ABIs, threads, cross-library exceptions, drawing and native
graphics pass, as do a default-runtime reboot and 20 health checks. Removing
the non-packaged override restores the original provider, verified with both
ABI probes. Tracker and Deskbar continue to use their adjacent packaged
runtime; no gain is claimed for them.
AirTop 1.2.0-2 is the ninth accepted change. It caches the DSI panel's fixed
geometry while continuing to draw values and graphs at five frames per second.
Matched live runs reduce its app_server CPU by 28.9% and client CPU by 9.6%
(21.8% combined), with exactly 225 frames and 180 samples in each 45-second run.
The cache costs 1.17 MiB of shared bitmap storage and needs no extra drawing
window or thread. Native/QEMU pixels, 288 direct/cache comparisons, repeated
panel restarts, the full window UI, exact package rollback and a normal native
boot pass, including graphics and all 20 health checks. The cache
object is freed when hidden, but its bitmap-pool space can remain reusable.
A tenth change reuses free space at the end of a client bitmap pool when
growing it. A repeated 1/4/16/64 MiB temporary-image sequence retains
64.04 MiB instead of 85.16 MiB (21.12 MiB less), while gradual resizing
retains 12.04 instead of 17.33 MiB when freeing the old bitmap first. Total
workload CPU is essentially unchanged; smaller growth adds a few microseconds
per allocation in the gradual case. Both original and candidate can still
hit expensive separate-area allocation when virtual-address layout blocks
growth. Alternating native server boots, cloned-memory/failure checks, drawing
hashes, EGL/BGLView, AirTop rendering and all 20 health checks pass. The original
scanline server remains available behind the removable launch amendment.
An eleventh improvement fixes bitmap-client reservation cleanup and the
kernel iterator that skipped the first reservation when unreserving a range.
Cloning and releasing 1,000 areas now reuses one virtual address range instead
of consuming roughly 125 GiB of virtual space; this is not physical RAM. The
matched native allocator workload uses 38.5% less CPU at 1,000 cycles and 56.3%
less at 2,000. The new default libbe and boot kernel pass isolated ownership
and VM regressions, original-kernel and library rollbacks, QEMU, and the final
native reboot with graphics, C++ and all 20 health checks. Serial identifies
the boot kernel as `hrev60206+766+dirty`; the packaged kernel file and its
metadata remain the original 750. All original boot files are preserved.
A twelfth improvement caches AirTop’s fixed panel titles and captions in the
existing background bitmap. Native live ABBA uses 3.2% less combined CPU than
the geometry-only cache, with identical cadence and mapped memory. Version
1.2.0-3 passes pixel checks, QEMU, package rollback/reinstall and a normal native
boot. The kernel’s loader/malloc callers also pass 50 loader cases, four-thread
allocation checks and fork copy-on-write.
A thirteenth improvement collects AirTop’s per-process details only while its
dashboard is open. Native live ABBA uses 26.9% less panel client CPU
and 10.2% less combined CPU than revision 3, at unchanged frame/sample
cadence and mapped memory. Revision 4 preserves complete command-line samples
and passes repeated dashboard reopen, fresh CPU intervals, QEMU, exact package
rollback/reinstall and a normal native boot with graphics and health 20/20.
The twelve-hour investigation is complete with thirteen accepted changes.
Final sustained observations and the installed-state/recovery audit pass;
all accepted source is integrated and pushed. Measured results and limits
are in [PERFORMANCE-20261008.md](PERFORMANCE-20261008.md).
The present lab boot uses Ethernet `.213`, HDMI1 at 1920x1080, and the DSI
panel. HDMI0 is currently disconnected.

## DSI display panel (2026-10-08)

The Waveshare 3.5" DSI LCD (E) on the DISPLAY connector works as an
auxiliary display: the `rpi_dsi` driver runs the DSI1 pipeline beside the
firmware's HDMI outputs (HVS channel 2, pixel valve 1, PLLD's DSI1
channel, the firmware power domain 19), programs draw on it with the
Device Kit's `BAuxDisplay`, and AirTop shows its CPU, network and
temperature panels there (`AirTop --panel`, started by the image's
UserBootscript). `rpi_thermal` adds the SoC temperature as
`/dev/power/rpi_thermal/0`. Touch is not read yet. Details: `DSI.md`.

## Owner-reported fresh-install issues: native verification (2026-10-08)

The owner reported four problems with a fresh SD image (Summit refusing
HTTPS with certificate errors, a panic when a USB keyboard was unplugged,
a second HDMI monitor not detected after plugging it in, and a card that
would not boot after a power cut following the expansion reboot). The
fixes from 2026-10-07 were merged as `9d1bdbc8b8` and checked on the lab
Pi from a generic (non-lab) image built from that tree, `hrev60206+741`,
SHA-256 `682f9d5c…`, written to the 128 GB card through the recovery OS:

- First boot showed Step 1; "Use entire SD card" → Step 2 → Hobart →
  "Restart now". The restart logged `bfs: Reserved-space growth to
  127727042560 bytes: No error`; `RPiInstaller --status` and `df` agree on
  119 GiB and no step is offered again.
- `rpi-time` status is "Synchronization successful" with `last-sync`
  saved; the Deskbar clock went from 01:02 (UTC seed) to 12:03 Hobart.
- `/boot/system/data/ssl/CARootCertificates.pem` exists; Summit loads
  `https://www.haiku-os.org/` with the padlock.
- Three NanoKVM HID resets (`POST /api/hid/reset`, the gadget detaches and
  re-enumerates its keyboard, mouse, tablet, disk and RNDIS) produced three
  `device removed` / `new device connected` pairs and no panic; all three
  HID endpoints were still polled afterwards (`kvm-hid-probe.sh` 20/20).
  The RNDIS function still times out on re-initialisation (known).
- Disabling the NanoKVM's HDMI capture drops HPD: the driver logged
  `HDMI0 disconnected`, the desktop fell back to HDMI1 alone, and
  re-enabling logged `HDMI0 connected` with the 3840x1080 layout restored
  and a live KVM picture. Windows that were on HDMI0 stayed on the display
  they survived on: afterwards Terminal and Summit sat at x ≥ 1920 (HDMI1).
- Power cut ~1 s after a running desktop, power back: the card booted
  straight to the desktop, BFS mounted without journal complaints.

Observations that are not among the four: the generic image built here
had no `curl` (the CI package list has `airos_curl`; the local staging set
did not), and Terminal prints `tput: unknown terminal "xterm-256color"`.
An earlier EDID failure on HDMI0 during this session was a loose cable at
the KVM, not the driver. Evidence: `/mnt/HaikuWork/rpi4/evidence/owner-issues-20261008`
(serial logs per step, screenshots, image checksum).

## Follow-up USB and network investigation (2026-10-07)

The owner's late-attached keyboard exposed a further xHCI cancellation race:
the captured panic has an empty descriptor queue but a leaked pipe reference.
A targeted sanitizer test reproduces it before the fix and passes afterward.
QEMU hotplug and bounded native NanoKVM resets pass; the physical keyboard
retest remains pending. See [USB-NETWORK.md](USB-NETWORK.md) for the exact scope.

The first Ethernet candidate raises three LAN downloads from 667–698 to
737–745 Mbit/s while reducing measured total CPU from 65.6% to 50.9%.
Summit profiling also identifies a costly full TCP queue scan; its separate
candidate retains cheap checks and a diagnostic full scan. The combined
candidate reaches 752–764 Mbit/s at 51.4% total CPU and passes bidirectional
SHA-256 checks and native queue tests. Summit remains slow: local browser
transfers reproduce the problem, with profiling pointing to shared-buffer
growth in the network process. Browser qualification is still in progress.

## Fresh-install reliability investigation (2026-10-07)

The reported unbootable card has an intact FAT partition and a correctly
expanded 127,727,042,560-byte BFS volume. An allocated-data backup and its
extent checksums are preserved before changes. `bfs_shell checkfs` found
496 nodes and no missing, duplicate or reclaimable blocks. Its original
system boots when the NanoKVM presents a valid nonbootable USB disk instead
of an empty mass-storage LUN. This establishes a lab boot-device problem;
the observed card does not show corruption from filesystem expansion.

The serial cable is working: a known Linux UART message was received intact,
followed by a complete native AirOS boot log. `serial-capture.sh` previously
used a `pkill -f` pattern that could kill its own SSH shell before setting
the baud rate. It now uses a bounded reader and the shared serial lock.

The HTTPS failure also has a concrete certificate-store cause: the original
image has no `CARootCertificates.pem` at either curl's canonical path or the
legacy Summit path. Installing the official `ca_root_certificates` package
and the compatibility symlink restores certificate-validated HTTPS. The old
arm64 package reader cannot load the repository's zstd-compressed bundle;
CI now stages a zlib copy. A correct clock alone did not fix this image.

The candidate time/onboarding and USB HID lifetime changes build and boot in
QEMU. Both storage choices and the Hobart timezone persist across a guest
restart. The expanded virtual card passes an offline BFS check (434 nodes,
no allocation errors). Choosing a zone preserves UTC; a failed actual
`Time --update` returns status 1. Twenty keyboard and twenty tablet
attach/remove cycles completed with working input afterward. A networked
candidate synchronizes time automatically and Summit opens HTTPS.

The native SD has been upgraded to clean `hrev60206+733` packages and
matching loader/archive from `6ea9a7d410`, with the original files retained
for rollback. The complete image booted in QEMU before native deployment.
It reaches the timezone step, NTP reports synchronization success,
and both curl and Summit load HTTPS with certificate validation enabled.
Repeated native USB gadget disconnect/reconnect cycles complete without a
panic. The final three cycles use NanoKVM's supported HID-reset endpoint and
confirm a typed serial marker after every reconnect. Manual gadget rebinding
can leave the KVM with stale Linux HID handles (`ENXIO`); use its HID reset
when recovering input, and explicitly refocus Terminal after device dialogs.
The KVM's RNDIS interface times out on reinitialization; this is not a successful USB
network hotplug qualification. Ethernet remains available.

HDMI now polls the BCM2711 HPD register, debounces edges, reads EDID using
firmware display IDs 2 and 7, restores a supported EDID timing through firmware
KMS, and notifies app_server through its existing display-change port. A locked
state snapshot prevents accelerants reading a partly updated EDID. Both physical
connectors retain stable IDs even if only one was present at boot. Three native
HDMI0 disconnect/reconnect cycles shrink the desktop to HDMI1 and restore the
3840x1080 layout with a visible KVM picture. A separate boot starts with HDMI0 absent;
with both firmware pipelines initialized in `config.txt`, a later connection
selects 1080p and produces a live picture. Without pipeline initialization it
remains black. HDMI1's physical reconnect and picture still need observation. Firmware EDID responses alone are unsuitable
for detection: they remain cached after HPD goes low.

Evidence: `/mnt/HaikuWork/rpi4/evidence/fresh-install-20261007`, including
`sd-allocated-manifest.json`, `bfs-check.log`, `no-boot-media-serial.log`,
`qemu/expanded-bfs-check.log`, `qemu-final/hotplug.log`,
`native-verify.log`, `native-summit-visible.jpg`, `native-hdmi-hotplug.log`,
`native-final-late-hdmi.log`, `native-final-late-hdmi.jpg`,
`native-usb-fixture-reset.log`, `native-usb-reset-cycles.log`,
`native-release-install.log`, `native-release-verify.log`,
`native-release-summit.jpg` and `release-manifest.json`. RPiInstaller 1.1.0 is
staged in the build server's ARM64 package pool; its old 1.0.0 package is retained in the task
backup directory. The complete CI image pipeline has not been run here.

## Current reliability image

Built on 2026-10-07 from clean `6ea9a7d410`, version `hrev60206+733`:

- Image: `/mnt/HaikuWork/rpi4/build-installer/airos-rpi4.img`.
- SHA-256: `2e1c5e1f9faf05cc0d51c7a212701e8df58cb6b1aa244dd91d1ae5fc6f15137d`.
- Includes RPiInstaller 1.1.0, the CA bundle and compatibility path, automatic
  time synchronization, USB HID lifetime fixes, and HDMI hotplug support.
- Native cold boot after a clean shutdown and smart-plug power cycle passes.
  The recovery detach command leaves a valid read-only idle disk, and all six
  installed package/boot-file hashes match the staged artifacts. Automatic
  NTP, two verified HTTPS requests, Summit, keyboard input and the dual-display
  layout pass after this boot. The temporary diagnostic driver is absent.
- The generic image excludes the private lab overlay and diagnostic mailbox
  driver. Native installation updates the existing expanded card in place;
  it does not erase the user's volume or rerun expansion.

The earlier performance results below describe the previous image and were
not all repeated for this reliability build.

## Previous performance image

Built on 2026-10-06 from clean source commit `63f5e1e916c41369d9464a062f20a23912fe8ee6`:

- Image: `/mnt/HaikuWork/rpi4/build/airos-rpi4.img`, 4,030,726,144 bytes.
- SHA-256: `dc071043870ab5e704c5b6cc76a5db8f9f307a49849a70a0f786f1c719fe2d32`.
- Emulator: the complete image reaches the branded desktop.
- Flashed to the lab SD card on 2026-10-06; a full 4,030,726,144-byte
  readback matched that checksum before boot. The installed kernel is
  `hrev60097+672`. FAT config, loader, boot archive, packaged libraries, Mesa,
  WebKit, V3D and Wi-Fi helper match the recorded build; UTC time is correct.

The checksum sidecar is `airos-rpi4.img.sha256`. The complete package/library
manifest and raw acceptance results are in
`/mnt/HaikuWork/rpi4/evidence/performance-20261006/release`.


The image contains the full branded air/OS desktop, custom Screen, Wi-Fi
and Bluetooth preferences/applets, Summit, airTime, and the other available
arm64 application packages. The lab build includes its private remote-test
overlay. That overlay is controlled by `/mnt/HaikuWork/rpi4/state/lab-image`;
it is not part of the generic `rpi4-airos` profile's application set.

Build instructions are in `BOOT.md`; the performance work, controlled
comparisons and rejected experiments are in `PERFORMANCE.md`. Firmware,
Mesa and Summit builds are pinned by the scripts in `tools/rpi4`.

## Hardware and applications

| Stage | Current result | Remaining limit |
| --- | --- | --- |
| SD and boot | Full air/OS boots from SD; four cores; high-speed SD with DMA requests up to 512 KiB; USB recovery retained | Only the 4 GB rev 1.5 board has been tested; no SD hot plug |
| Ethernet | GENET at 1000 Mbit/s full duplex, DHCP and data transfer | One queue and packet copies; no checksum offload |
| USB | VL805 storage and HID work after the descriptor-overfetch fix | 8 GB board DMA above 4 GB untested; no webcam connected |
| Graphics | Native OpenGL/GLES and Summit WebGL use V3D; pixel-exact GLES and Vulkan probes | Vulkan is headless: no arm64 loader or window-system presentation |
| Displays | Both outputs advertise 1920x1080; the desktop is 3840x1080; custom Screen preferences arrange, mirror and scale outputs | HDMI1's physical picture has not been observed; firmware scaling; HDMI0 hotplug tested; HDMI1 physical reconnect, DPMS and hardware cursor unqualified |
| Wi-Fi | CYW43455 scans and joins WPA2 through the custom preferences window, gets DHCP, and remembers the network; final independent traffic and reboot results below | Intermittent startup association timeout remains: Cold boots 3 and 4 fail to autojoin; dismissing the blocked supplicant dialog permits recovery; roaming and wider AP compatibility need more coverage |
| Bluetooth/BLE | Controller, custom preferences and nearby-device scanning work; earlier LE connection/service discovery passed | Pairing and application profiles untested; UART at 115200 |
| Media/airTime | Hardware H.264 and HEVC Main/Main10 decode; 25 HEVC fixtures and H.264 match FFmpeg; 1080p HEVC plays at 30 FPS | H.264 memory contention drops occasional frames; 4K10 remains below 30 FPS; sound has not been listened to physically |
| Summit | GPU compositing, WebGL, JIT and the WebRTC-enabled engine are included; local browser regression fixture passes | Full-window scrolling still has substantial readback and app_server copying costs |
| Other apps | airShot, Amp, Burrow, Kiri, LCDMonitor, Natter, TurboChook, Clipper and AirPins are included | Aurora needs an arm64 Haiku Node.js port; launch checks are not comprehensive app testing |

The installed image passes the correctness, launch, playback and device checks
below. Four cold boots deliver all 20 keyboard, 20 relative-mouse and 20
absolute-tablet events from the KVM. Wi-Fi autojoins on boot 2 but fails on
boots 3 and 4. On boot 3, dismissing the supplicant's blocked password-error
dialog releases the queued retries and restores the link. On boot 4,
dismissing it and running the normal helper reconnects with the saved
credentials. Ethernet remains available throughout. Startup reliability is
still a release blocker; the generic password-error dialog does not prove
that the password is wrong.

The final combined-load run passes 168 graphics/Vulkan rounds over 3,659
seconds, 38,700 checksum-verified HEVC frames, and 31,680 randomized
compression checks. Peak sampled temperature is 74.95 C, all sampled
firmware throttle flags are zero, and swap remains unused. This run holds
no extra GPU descriptor; the packaged driver retains its own hardware state.
The decoder reuses picture buffers, so the separate repeated-allocation
failure remains open.

The temporary telemetry module is removed from the driver directories,
then a clean cold boot verifies that its device node is absent. Production
library/driver hashes still match the image, UTC is correct, Ethernet and
USB are up, both logical displays retain their layout, and the GPU lifetime
probe leaves zero client buffers. The device is left running with Ethernet
and the saved Wi-Fi network connected. Evidence: `release/clean-boot-checks.txt`,
`verify-installed-final.txt`, `hid-boot-4.txt` and `wifi-boot-4-recovery.txt`.
An additional Wi-Fi-only transfer after recovery matches its checksum and
runs at 73–75 Mbit/s, with no ping loss; Ethernet is restored afterward
(`final-network-wifi.txt`, `final-idle-2.txt`).

The graphics and memory suite passes on the installed libraries: 737,600
string-length cases, 6,422,376 string comparisons, 549,027 memory moves,
497,300 memory-set guards plus 8,192 graphics-buffer cases, 50 loader cases,
four view checks, GPU lifetime/pixel checks, and TCP regressions. All 25 HEVC
streams and the 90-frame H.264 fixture match FFmpeg. The H.264 harness's
initial expected digest was stale; a fresh host decode and both native input
modes agree on `2ceb043d50df9e2292ac39efaeac639b`.

All application launch checks pass; LCDMonitor also writes a visually checked
dashboard. BLE scanning finds nearby devices, and the audio API accepts a
48 kHz tone. These are not physical audio or external-LCD observations. Both
Linux recovery and air/OS enumerate the Verbatim thumb drive and NanoKVM;
the separate physical mouse is not present in either enumeration. An 8 MiB
read of the thumb drive matches between Linux and air/OS without any write.

First observed HDMI desktop is 34.4 seconds after the EEPROM banner on the
initial flashed boot. A KVM video gap covers the HDMI reset, so this is an
observed upper bound, not the exact first desktop paint. The serial display
layout marker is at 22.49 seconds and Ethernet link at 25.98 seconds.

The first fresh-image Summit launch, with no pre-launch engine hash read and
an empty persistent shader cache, is ready in 2.56 seconds and sends its
first content tiles in 8.64 seconds. Browser cache and later cold-boot results
are distinguished in `PERFORMANCE.md`. Across three runs, warm window
medians are 103–120 ms for StyledEdit, about 108 ms for About,
277–281 ms for AirPins and 330–336 ms
for Natter. The controlled Aquarium workload measures 32 FPS with normal
maps/reflections and 36 FPS with both disabled; full-window synthetic
WebGL runs at about 59 FPS and scrolling at 30–32 FPS.


## Release limits

Exhausting both RAM and swap can leave the system unresponsive without an
OOM victim being selected. Fragmentation during repeated allocations can
also prevent the HEVC driver from finding a contiguous picture allocation
despite ample free RAM; the test process fails and the board recovers after
memory is released. The sustained bounded-memory
stress test and the memory-pressure failure are reported separately.

The port still needs physical HDMI1 and audio verification, Bluetooth pairing
and profiles, wider board/AP coverage, and Vulkan presentation. No result here
claims those gaps are closed. See `DISPLAY.md`, `MEDIA.md`, `WIFI.md`,
`BLUETOOTH.md`, `GPU.md`, and `PERFORMANCE.md` for implementation details.
`CPU-TUNING.md` describes the opt-in Cortex-A72 build profile and the QEMU
kernel add-on race that keeps it off by default.
