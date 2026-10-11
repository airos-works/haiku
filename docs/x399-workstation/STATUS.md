# X399 workstation status

Status values are backed by observation on the workstation. Anything not
listed as verified is untested.

**2026-10-11 WX 5100 status:** the X399 now has a Radeon Pro WX 5100.
The NVIDIA results below describe the previous card. AMD desktop output still
uses the firmware framebuffer. SDMA and the full 61-submission bounded GFX diagnostic pass, including
indirect buffers, compute, rasterization and VM switches with zero VM faults
and data mismatches when the matching MEC firmware is loaded. Separate
client GPU address spaces pass same-address isolation, concurrent clients,
private diagnostic command pages and complete memory reclamation.
Root-only userspace compute and raster submission also pass on cold boots,
including device-only VRAM, changed shader instructions and exact triangle
pixels. GFX client completion now requires IH3 MSI observation of its private
fence: two 4,382-job cold-boot trials pass with ring wraps, exact data and
media/DMA coexistence. Separate read-only-page and privileged-register
faults fail the jobs and preserve quarantined allocations through close;
recovery still requires a cold boot. A native Mesa metadata candidate now
reports measured CU/RB masks, tiling, firmware, timestamp clocks and explicit
interface limits; its graphics/DMA/media regression trial passes. Private
RadeonSI now passes compositor and EGL-window pixels, resizing and shared
contexts; Summit's short native GPU rendering trial also passes. The windows
still use CPU bitmap presentation. Concurrent UVD/GFX scheduling and cached
guard readback pass decode/graphics/DMA, in-flight close, concurrent fault
quarantine and cold recovery tests. Separate graphics execution ownership
also passes 16 in-flight graphics close/reuse cycles. The combined 900-frame
Summit HEVC Main10/AAC GPU trial now passes with zero drops, compared with
560 before this work. Sustained H.264/AAC and HEVC Main10/AAC GPU playback
each passes 5,400 frames over three minutes with zero drops. Mesa now submits
private kernel copies of outer command buffers. Source replacement, failed
CPU copies, zeroed tails, GPU write protection, terminal quarantine and cold
recovery pass. Embedded firmware/media pointers now require complete userspace
ranges; invalid video ranges fail before advancing decode state. The copied
path passes the same two 5,400-frame GPU playback trials with zero drops.
Final DMA and graphics checks pass at 43,657 completions with no GPU errors.
Separate shader write/execute permission faults pass with unchanged data and
quarantine; cold recovery plus complete command capture leaves the current
boot healthy at 916 completions. Broader GLES 3.1 checks now cover indexed,
instanced and indirect draws, queries, transform feedback, and direct/indirect
compute, with every output checked. Their 25 command captures independently
decode with 23 packet opcodes. A system-header BGLView application also passes
fixed-function drawing, resize, desktop pixels and shared texture lifetime;
the private renderer now chooses AMD automatically without a device override.
Explicit software and unprivileged fallback also pass. The complete native
renderer regression passed at 1,399 completions. An initial opt-in kernel
command policy now validates the immutable copy and rejects unsupported
packet/control and privileged-register operations before GPU execution.
All 337 real command captures and 233,168 policy checks pass host sanitizers
and QEMU; native tests reject 96 streams, verify each entire private copy was
cleared, and complete 192 recovery/control jobs. The checked renderer suite,
concurrent media and both three-minute Summit playback trials pass, with
5,400 frames and zero drops for each codec. Final DMA/VM checks leave this
boot healthy at 43,457 completions with zero errors; the desktop is intact.
A subsequent compute-shader probe checks 16,384 lane records across four VMs:
neither SETREG form raises privilege, trap-register writes are ignored, and
ordinary MODE/M0 positive controls succeed. All output/peer guards and memory
reclamation pass. DMA plus checked GLES 3.1 leaves 43,498 healthy completions
without requiring a reboot.
A kernel graphics queue now passes immutable snapshots, pending fences,
64/client and 256/device limits, mixed DMA ordering, and pending-close memory
lifetime checks. A deliberate VM fault fails all 23 affected/canceled fences
and preserves quarantined memory; cold recovery repeats 1,213 graphics jobs,
both DMA paths and concurrent video with no errors. Private Mesa now has an opt-in asynchronous path with verified pending fences,
deadlines, sync generations, teardown and deliberate-fault propagation. Its
queued rendering suite passes. A kernel correction lets decode start while
graphics is queued and still excludes it behind ordinary DMA; both directions
have gated regression tests. Queued Summit HEVC 900/0 now passes after an
initial 900/80 trial exposed the scheduling issue. Both three-minute queued
playback trials also pass 5,400 frames with zero drops. Final VM/DMA checks
leave 44,699 healthy completions with no GPU errors. Synchronous Mesa remains
the default. A subsequent diagnostic kernel and corrected resource fixture
verify 16,384 scalar/vector/flat/LDS records across four VMs, descriptor bounds,
guards, memory reclamation and fixed translation readback. Two fixture errors
are documented; all GPU jobs remain healthy. Follow-up queue, VM, DMA, GLES
and concurrent media tests leave 248 completions on that boot. A later owned-RAM
ATC fixture verifies vector buffer reads, stores and atomic adds stay within
GPUVM under the current translation configuration. Separate store/atomic
read-only tests produce hardware VM faults, unchanged targets/guards and
post-close quarantine. Each reviewed fault has a planned cold recovery;
final graphics/queues/VM/DMA/concurrent-media checks pass at 255 completions,
510 EOPs and zero errors. GDS access containment and remaining shader resource
paths still need qualification, and general submissions remain root-only.
Driver `f546422d…` and private Mesa `82098de9…` are installed with same-hash
backups; prior `ffe50ea8…`, `cb8672d6…`, `cefb183c…`/`afd206fa…`, prior `c579f1cd…`,
`7d1cf6e5…`/`ee6210e5…` and older checkpoints are
preserved. The original global media addon is restored after qualification.
System Mesa defaults remain
unchanged. Unprivileged general
graphics submission, system Mesa defaults and native display remain unfinished. AMD UVD H.264 plays three minutes of 1080p30/AAC with zero drops
in private airTime and Summit builds. HEVC Main/Main 10 has independent native
pixel/format/lifecycle verification; both private applications also pass
three-minute 1080p30 Main-10/AAC playback without drops. Summit also passes
injected HEVC failure, paused-seek recovery and old-addon software fallback.
Root and unprivileged clients also pass complete readback of 7.75 GiB of
VRAM beyond the CPU aperture. General graphics, native display and the final
complete release remain open.
[WX5100.md](WX5100.md) records identities, evidence, limitations and next work.

`tools/check-workstation.sh` re-checks the machine against all of this in one
pass, with nothing set in the environment of the programs it runs, because
several of these have looked fine while being quietly broken. It last came back
25 working, 0 not (2026-10-06, running air/OS hrev60206+686).

What still needs someone at the machine: a look at the scaled picture on the
two 4K monitors, a monitor pulled out and plugged back in (the syslog says
`nvidia_rm: DP-4 disconnected` and the desktop should follow), devices in
each USB port, the serial console, and a Bluetooth device to pair with.

| Area | Goal | Status |
| --- | --- | --- |
| Boot from NVMe (UEFI) | Haiku boots directly from the Samsung 950 PRO | verified: firmware boots `EFI/BOOT/BOOTX64.EFI` from the NVMe ESP; `/boot` is `/dev/disk/nvme/0/1` |
| SMP | all 32 hardware threads (16 cores) online | verified: `sysinfo` lists 32 CPUs |
| NVMe | disk available and bootable | verified after multi-root PCI fix: 2 GiB raw read at 1.6 GiB/s, boot volume |
| Ethernet | I211 up with DHCP | verified: ipro1000 link 1000BASE-T, DHCP lease, HTTP upload and SSH |
| USB | every port at its full speed | verified on every USB 3 port of the four controllers in use, with real devices: a USB 3 SSD reads 281-308 MB/s and writes 312-349 MB/s at SuperSpeed on a CPU rear port, a chipset front port and the ASM2142's USB-A port (43 MB/s before), data verified each time; a RTL8153 gigabit adapter links at SuperSpeed on the ASM2142's USB-C port and carries 815 Mbit/s (81 before); USB 2 flash drives read their 30-36 MB/s on every controller. Disks and network adapters plugged in while the system runs now attach. The Thunderbolt card is out of scope for now (its controller halts on its first DMA, under the BIOS too; its wiring is in doubt). The chipset's controller takes 3-4 ms per control transfer to a USB 2 device (one stage per frame, the controller's own pacing as far as can be told), which slows enumeration and the Bluetooth firmware load but not data |
| Audio | ALC1220 analog output, HDMI audio | verified both: two outputs, each clocking its stream at the hardware's own rate (48322 and 48321 frames a second against the 48000 asked for). The graphics card's codec needed a change to Haiku's hda driver, which discarded any codec whose converters are all digital. The monitor reports it takes stereo. What nobody here can check is whether a speaker makes a sound |
| Graphics | GTX 1070/1080 Ti accelerated 2D/3D, 3-4 monitors | 3D verified: Vulkan on the GPU (1.4 TFLOP/s compute, 57 Gpixel/s fill) and OpenGL 4.5 through it, with frames copied straight into the screen's own frame buffer in video memory rather than sent through the host - a lit sphere at 1600x900 goes from 209 to 970 frames a second. Vertical sync works (locks to 60.0) now that the accelerant hands out a retrace semaphore. Any program gets the GPU, with nothing set in its environment. Three heads driving one spanning desktop is verified, but with the third and second forced rather than plugged in. 2D is not accelerated at all: it runs four to eight times slower than drawing in memory, which is still far more than a desktop needs at one monitor |
| Displays | two 4K monitors usable at arm's length: per-monitor scaling, arrangement, mirroring, per-monitor maximize, hot plug | verified on the two Dell P2415Q (DisplayPort): each monitor is a region of one frame buffer that the display engine scales up to the panel, at 100 to 250 percent in steps of 25, chosen per monitor. app_server arranges the monitors (side by side, stacked, swapped, one off, or one mirroring another), remembers the arrangement per monitor identity, keeps the mouse off the parts of the desktop no monitor shows, moves windows along with their monitor, and maximizes a window to the monitor most of it is on (the classic whole-desktop maximize is a setting). Two 24-inch 4K monitors come up at 200 percent, a 3840x1080 desktop drawn at full density with no settings at all; text is sharp in the frame buffer itself. Monitors at different scales share the largest density the engine can show: with the 1080p KVM at 100 percent beside them the 4K monitors stay one to one and the 1080p one is shrunk. The Screen preferences show the monitors as they stand and let them be dragged into place, identified by number on each screen, and read out from their EDID. Monitors coming and going are noticed two ways, but nobody was at the machine to plug one, so that path is untested. Frame buffer and VESA hardware gets the same scaling done in software, untested here |
| Bluetooth | TP-Link Archer TX55E, working adapter and discovery | verified: the adapter answers as `90:74:ae:33:d7:cb` "MTK MT7922 #1" and an inquiry finds devices nearby, from a cold boot with nothing done by hand. The radio is a MediaTek MT7922 on USB, which runs a bootloader rather than a Bluetooth controller until it is given firmware - it takes the HCI Reset every stack opens with and never answers. The driver now hands it that firmware at open. Three further faults were in the way: `h2generic` took its event endpoint from the last interface that had one, which on this radio is MediaTek's audio interface, so it listened where no reply is ever sent; it stood isochronous transfers on the SCO endpoints at open, which nothing wants until there is a call; and the server never answered a request for a command the controller refuses outright, which hung the first program to ask for an adapter. Remote name lookup still fails, so discovered devices show an address and no name. Pairing and audio profiles are untried |
| Wi-Fi | TP-Link Archer TX55E | verified: `mt7922wifi` joins WPA2-PSK/CCMP networks and carries traffic. Joining works from the Wi-Fi preferences (the password prompt, Remember this network, Known networks, Disconnect) and the WiFiStatus Deskbar applet lists networks with signal and lock, and shows the one joined. A saved network is joined again by net_server by itself once the card is up (it starts wpa_supplicant). DHCP configures the interface; with the wired card down the machine resolves names, fetches https pages and moves 50 MB each way (1.9 MB/s up, 2.5 MB/s down). Legacy 802.11a/g rates only for now (no HT/VHT/HE), chosen by the firmware's rate control. The driver is the Linux mt7921 layout in FreeBSD net80211 shape: firmware-offloaded scanning, the firmware's own channel management (remain-on-channel), software CCMP through net80211. It attaches at boot like any other driver and is part of the regular x86_64 image; the driver settings (`mt7922wifi`) can keep it out of the boot (`attach_at_boot false`, or `attach_at_boot_until <time>` for trying boot attachment where only the power switch reaches the machine) and turn on its diagnostics (`debug true`) |
| Video decoding | H.264 on the card's video engine, and something that plays a film | verified: the GTX 1080 Ti has an NVDEC engine and an NVC2B0 decoder class, and thirteen H.264 streams together with 120 frames of 1080p Big Buck Bunny decode byte for byte identically to ffmpeg's own decoder - multiple references, B pictures, spatial and temporal direct prediction, B pictures used as references, weighted prediction and two coded sequences among them. 1080p decodes at 232 pictures a second, 4.3 ms each, about eight times what playing it needs. It is offered to the whole system as a media add-on, so any program that opens a film gets it, and `NVPlay` plays one with sound, stopping, starting and seeking. What it will not do is field pictures, 4:2:2 or more than eight bits a sample, and because Haiku picks one decoder for a format those refusals mean the film will not play rather than falling back |
| Network shares | a share of a Windows, Samba or NAS server mounted whenever the system starts, set up in Tracker's preferences | verified with the Music share of the NAS (`//192.168.1.102/Music`, 45 TiB, SMB 3.1.1): added under Tracker preferences > Network shares, it is at `/Music` and on the Desktop seconds after a cold boot, with nothing asked and nothing done by hand. The first try comes before there is a network and fails; the mount server tries again, sooner after the network changes, for fifteen minutes. With the wired network pulled the share went on over the wireless one after the twenty seconds a request is given, failed at once with both gone, and worked again when they were back. `tests/sharetest` reads and writes it as a disk (20 of 20, 45 MiB/s reading, 50 writing). The pieces: `smbfs`, a FUSE file system on libsmb2 6.2 (in `src/libs/libsmb2`), in a package of its own next to `userland_fs`; the mount server, which keeps the list in `~/config/settings/network_shares`, the passwords in the key store and does the mounting; Tracker, which has the page and lists the shares in its Mount menu. Not there: Kerberos (NTLM only), SMB1, looking for servers on the network, and attributes - what Tracker wants to remember about a folder in a share it cannot. The key store asks once for each of three things the mount server does with passwords; on this machine that has been answered |
| Sleep | S3 suspend and resume | sleeps and wakes; the display comes back, but the NVMe and the network card usually do not. Paused until a serial console arrives, which is also what the one untested path in the vertical sync work needs |

## Log

- 2026-10-10: a 1080p monitor no longer blurs the 4K ones (branch
  `x399-mixed-scale`). With both P2415Q at 200 percent and the KVM's 1080p
  input at 100, everything was drawn at 100 percent and the 4K monitors were
  enlarged from 1080p by the display engine - blurry until the KVM was
  unplugged. The density was the smallest display scale because the engine
  was thought unable to shrink a picture (a 4K monitor at 175 percent had
  been refused). It can: `nvlayoutcheck`, which asks NVKMS whether a layout
  is possible without applying it (a mode set with commit = false, allowed
  while app_server owns the display), says a 3840x2160 region onto the
  1080p monitor is fine and only a region shrunk onto a 4K monitor is not
  (disp status 2, FAILED_EXTENDED_GPU_CAPABILITIES_CHECK - the bandwidth
  check).
  * app_server now draws at the largest display scale and lets the engine
    shrink the less scaled monitors' regions; `Desktop::_SetDisplayLayout()`
    tries `DisplayLayout::RenderScales()` from the largest down to the
    smallest, which only enlarges. `nvidia_rm` checks every layout with
    NVKMS before agreeing to it, so a refused one fails cleanly and the next
    density is tried; rpi_display and rk3588_display refuse what they cannot
    do as before.
  * The card maps only 256 MiB of video memory for the CPU (BAR1), and the
    11520x2160 frame buffer is 99.5 MiB. The accelerant kept the frame
    buffer before the current one mapped and mapped the new one beside both,
    so the second layout change in a row ran out (`NV_ERR_NO_MEMORY`) and
    fell back to 100 percent. Now a layout's frame buffer is allocated
    unmapped and mapped in place of the current one while the mode is set.
  * On the workstation (repacked app_server, accelerant, two power cycles):
    `nvidia_rm: layout: HDMI-0 ... scale 100% (3840x2160, drawn at 200%)`,
    both P2415Q 3840x2160 one to one; the frame buffer (`nvscanout --dump`)
    has text at full density in the 4K monitors' regions, and the NanoKVM's
    capture shows a window on the 1080p monitor at its logical size, the
    text shrunk cleanly. Five layout changes in a row stay at 200 percent;
    DP-2 at 150 percent is refused at 200 and drawn at 150 (only the 1080p
    region shrunk, DP-4 enlarged); the KVM disabled and enabled again stays
    at 200. `displaylayouttest` has the three monitors as a case and a fake
    engine that shrinks only onto monitors up to 1920 pixels wide.
    `check-workstation.sh` 25 working, 0 not.
  * Costs: the 1080p monitor's part of the desktop is drawn with four times
    the pixels, and a desktop arranged with a lot of empty frame buffer
    (monitors stacked in an L) can be too large to map at 200 percent; it
    then falls back to a smaller density rather than failing.
  * The workstation's wired address is now 192.168.1.247 (DHCP);
    `ssh/config` and the airos_mcp registration were updated.

- 2026-10-09: USB ports at full speed (branch `x399-usb3`, from master).
  Two USB stack bugs and two throughput limits were found with the drives
  and the RTL8153 adapter the owner plugged in:
  * No USB disk or network adapter plugged in after boot was ever attached.
    The device manager lists `drivers` for such a device, and the kernel's
    module iterator ended the whole listing at the first entry it could not
    stat - a stale `mt7922.aside` link in
    `~/config/non-packaged/add-ons/kernel/drivers`. At boot the listing is
    narrowed to `drivers/disk`, which skips that entry by name, so the same
    drives worked when present from the start. Unreadable entries are now
    skipped (`bc0d507f5e`); the link was left in place, and a drive replugged
    at the front was attached at once.
  * Every SuperSpeed endpoint was configured with bursts of one packet:
    `Pipe::InitCommon()` announced the pipe to the xhci driver before the
    burst size from the companion descriptor was set (`232a62851a`). The
    RTL8153's bulk endpoints now get Max Burst 3, as it declares.
  * `usb_ecm` read one frame at a time: 81 Mbit/s whatever the port. It
    now keeps twelve reads queued and sends without waiting (`7771ffa726`),
    and the xhci driver takes 15 transfers per endpoint instead of 7
    (`f6ad8e2554`): 297 Mbit/s over USB 2, 815 Mbit/s of UDP and about 500
    of TCP received and 650-780 sent over USB 3 on the ASM2142. The
    interrupt moderation interval was tried at 40 us instead of 125: no
    difference in throughput or latency, so it stays.
  * The chipset controller (1022:43ba, an ASMedia ASM1042A core) serves a
    control endpoint once per 1 ms frame, at microframe 4: the Setup stage
    itself waits for it, so it is not NAK handling. Transfer layout (Event
    Data, ENT, status-only events), periodic load and ASMedia's flow control
    register were tried at run time without effect; so was an Evaluate
    Context giving EP0 another interval, average TRB length or ESIT
    payload, which the controller ignores (as the specification lets it;
    at address time Haiku already uses the values Linux does). Bulk on the
    same controller runs at full speed. The ASM2142 is not affected
    (0.4 ms).
  * The Thunderbolt card (Gigabyte GC-MAPLE RIDGE, xHCI 8086:1138) halts
    with a Host System Error 22-50 ms after it is started, before any port
    activity, and the BIOS leaves it the same way: its first DMA read never
    completes. PCIe paths, ACS, AER, the IOMMU (off) and request attributes
    were checked; its connection-manager firmware runs (FW_STS 0x800001a1).
    The card most likely needs the Thunderbolt header this board does not
    have; set aside at the owner's request.
  * The NanoKVM's USB fails on every controller with two cables (the BIOS
    cannot use it either), so it is a cable or device fault.
  * USB 3 disks: a 180 GB SSD (Seagate enclosure, Bulk-Only) read 43 MB/s.
    usb_disk keeps DMA below 4 GiB (the xhci driver requires it), so on
    this machine nearly every transfer was bounced through a 16 KiB
    buffer, one SCSI command per 16 KiB; with 16 segments a buffer in low
    memory made at most 64 KiB; and commands were at most 128 KiB. A DMA
    resource with one bounce buffer now gets it as large as its largest
    transfer (up to 1 MiB), usb_disk allows enough segments, and
    SuperSpeed disks get 1 MiB commands, as Linux gives USB 3 disks
    (`1c3364e981`). Larger transfers exposed an xhci bug: a physical
    transfer over 384 KiB was laid out whole and its remainder sent again
    after each fragment, leaving the disk waiting for data and every later
    command timing out; TRBs could also cross 64 KiB now that bursts are
    16 KiB (`db80e86bf8`). The SSD now does 291/334 MB/s (front, chipset)
    and 281/312 MB/s (rear, CPU), 308/349 MB/s (ASM2142 USB-A), read/write,
    with 256 MiB written and read back identically in 1 MiB, 64 KiB and
    4 KiB blocks
    (`tests/usbdiskcheck.sh` in `/boot/home/x399-tests`).
  New read-only tools look at the controllers while the driver runs
  (`evtwatch`, `evtrate`, `portwatch`, `xhcirings`, `pcitree`, ...), and
  `usbmsbench` and `tcpbench` measure storage and network throughput
  (`aad1786ed6`). Installed on the X399 as a repacked haiku package
  (kernel, usb, xhci, usb_disk, usb_ecm from `x399/build/usb3-x86_64`). The
  diagnostics build used on the way is in
  `x399/state/usb3-lab-instrumentation.patch`. Lesson: the X399 runs a
  master build; `x399/haiku` is still on hrev60097, and a kernel built there
  stopped the boot.

- 2026-10-09: WiFi now has a native vector application icon: three blue
  arcs and a dot, with dark outlines, highlights and a small shadow. The
  preferences panel displays the icon beside its Wi-Fi control. WiFiStatus
  uses matching paths for its Deskbar icon, lighting the dot and arcs from
  inside out with the existing green/amber connection states.
  * Targeted x86_64 and ARM64 builds pass. The ARM64 build required refreshing
    a stale merged musl string object from before the architecture-specific
    `memmove`/`strlen` exclusion; no source change was needed there.
  * QEMU smoke checks cover application launch, the panel icon and the
    no-adapter Deskbar icon. On the X399, VNC confirms the panel and green
    Deskbar icon at 200% scaling, with Gaspers still connected at
    `192.168.1.197`. No network settings were changed.
  * Installed as user non-packaged WiFi/WiFiStatus overrides, with a user
    Preferences menu link. Deskbar's loaded image path and both binary hashes
    were checked. The panel is left open for review. Previous packaged files
    and the replicant settings are retained in
    `/boot/home/wifi-icon-review-20261009`; evidence and hashes are under
    `/mnt/HaikuWork/artifacts/wifi-icon-20261009` on the build host.

- 2026-10-06: fractional drawing and vector icon scaling were reviewed and
  corrected (`6a0056c5d4`, `3528aa6eb0`). The X399 now runs the clean
  **hrev60206+686** build from `3528aa6eb0`, including the concurrent fork
  master/CI changes merged in `cb915b1f0d`.
  * One-logical-pixel moves and scrolls at fractional density now redraw
    instead of accumulating rounded pixel-copy offsets. Rectangular gradient
    fills use the same edges as borders. Device clipping prevents a rounded
    logical desktop from extending beyond its physical framebuffer (the
    1280x800 QEMU display previously crashed when switched to 225%).
  * Vector sources survive bitmap copies, full imports, archives, button
    states and Tracker selection. app_server caches a raster at the drawing
    density, and discards it after raw pixel edits. RGB alpha behaviour and
    palette icons were checked separately.
  * The native sweep uncovered another issue: successive scale increases
    left gaps between the monitors until NVIDIA rejected the oversized
    framebuffer allocation. Scale changes now keep displays adjacent unless
    positions are also changed explicitly. The repeated native sweep passes
    with the expected desktop dimensions at every scale.
  * `fractionalscale`: all 64 border samples and all eight icon comparisons
    pass at 100, 125, 150, 175, 200, 225 and 250%, including eight one-pixel
    moves, eight scrolls and direct pixel mutation. Every icon comparison has
    zero differing pixels against an independently rasterized reference.
    RGB and palette variants also pass at 175%. The same checks passed in
    QEMU, along with live density changes using the same cached icons.
    `displaylayouttest` passes, including the new physical timing, adjacency
    and explicit-position cases. ARM64 app_server, libbe and libtracker build;
    no ARM hardware rendering claim is made.
  * All seven system packages and the EFI loader were installed, followed by
    a cold restart. Package, app_server, libbe and libtracker hashes match the
    build manifest. Existing non-packaged drivers remain in place. Original
    packages and loader are retained in
    `/boot/home/x399-backup/fractional-20261006`; the previous staged build is
    also retained. The final layout is DP-4 at (0,0), DP-2 primary at (1920,0),
    both 3840x2160 at 200%, desktop 3840x1080.
  * `check-workstation.sh`: **25 working, 0 not** after the final cold boot
    and scale sweep, including GPU OpenGL/Vulkan, displays, audio, USB and
    network shares.
  * Reproduction instructions and the shared-renderer/bitmap-only limits are
    in [FRACTIONAL-SCALING.md](FRACTIONAL-SCALING.md). Raw logs, captures and
    the pinned source/compiler/artifact manifest are outside the repository
    under `/mnt/HaikuWork/artifacts/fractional-scaling`.

- 2026-10-08: synchronize the runtime loader's TLS template registry. A
  Summit startup crash showed a new IPC thread copying a corrupted TLS
  template while another thread loaded Mesa. `Register()` could reallocate
  the vector without synchronizing with `CreateBlock()` on other threads.
  The new recursive lock covers template reads, mutation, and initialization
  copies. Existing per-thread blocks keep their lock-free fast path; the
  generation uses an atomic acquire read, and the lock is reset after fork.
  * `test_tls_templates.sh` loads 256 TLS libraries while 12 workers create
    576 fresh threads accessing a 1 MiB TLS template. Libraries stay loaded
    until all workers finish. The original native loader passes 26 rounds
    then reports two bad initializations; the candidate passes 100 rounds,
    **57,600 checks**, with no crash or debugger event.
  * Local x86_64 build, 100 QEMU rounds before and after a normal reboot,
    22 standard loader tests, 2,000 TLS generation checks, and C++ TLS
    construction/destruction pass. ARM64 loader deployment is untested.
  * The automatic rollback guard was verified in QEMU: prior fix -> candidate
    -> prior fix, followed by restoration of the original VM loader. Merely
    copying two versions into the packages directory prompts for an upgrade;
    the qualified procedure removes one, verifies the resulting loader hash,
    then installs the next. The guard preloads the old package and restores
    it with Python file operations, without launching another program.
  * X399 now uses `summit_runtime_loader_tls_fix-1.1-1-x86_64.hpkg`, SHA-256
    `25a64a61e4d68879ff1d821b96c0d61fcba4b7cecd1bebef0614cbfcff91adc4`.
    Active loader SHA-256 is
    `c9c57eae848f9b860ffffdd0b600631da181825c87a24d5fc8b40a6eaffd9d4c`.
    Native activation and all tests above pass; the guard reports `verified`.
    No native reboot occurred. The previous package is preserved in
    `/boot/home/summit/bench/perf12h-tls-concurrency/baseline-disabled.hpkg`.
    All 100 subsequent Summit startup/local-page/normal-quit cycles pass,
    with no crash events or leftover helpers. The engine rebuild was active
    during these correctness tests; this is not a performance claim.
  * Evidence: `/mnt/HaikuWork/apps/summit/.vm/performance-12h-20261008/tls-concurrency/`.

- 2026-10-08: the runtime loader now initializes a new dynamic TLS vector at
  the current image generation. Previously, after an image unload, a new
  thread's second access could discard the block created by its first access,
  losing writes. A Summit WebProcess crash reached Mesa's once trampoline
  with a null TLS callback after the callback had been stored; the isolated
  TLS reproducer exposes the corresponding loader defect.
  * `src/tests/system/runtime_loader/test_tls_generation.sh` builds two TLS
    libraries and checks 100 unload/reload rounds with eight new threads each,
    first-write retention, per-thread isolation, retained-image state and
    reused-slot initialization. X399 fails 1,224 of 2,000 checks before the
    fix and passes all 2,000 after it.
  * The local x86_64 loader build passes. An isolated QEMU overlay reproduces
    the failure, passes with the fix, fails again after package removal, and
    passes after reinstall and normal reboot. All 22 existing loader tests
    pass with `/boot/system/lib` in `LIBRARY_PATH`; the C++ TLS constructor /
    destructor fixture also passes.
  * Installed only `runtime_loader` through the removable local package
    `summit_runtime_loader_tls_fix-1.0-1-x86_64.hpkg`, without rebooting X399.
    Loader SHA-256 is
    `d634caaba16cb92f5879f54370212d372d081de48a99d7cb5cbfc4114b8425ce`.
    Moving that package out of `/boot/system/packages` restores the original
    packaged loader after package activation completes; rollback was tested
    in QEMU. Native activation was verified directly. The planned automatic
    guard failed to start because of a generated-script quoting error; it
    did not cover activation. This does not diagnose the separate earlier
    condition-variable invalid-opcode crash.
  * Evidence: `/mnt/HaikuWork/apps/summit/.vm/optimization-2026-10-08/tls-generation/`.
    Browser stress validation continues in the Summit repository. ARM64
    hardware was not changed or tested.

- 2026-10-05: scaled screenshots no longer copy the screen first. At 200%
  `DrawingEngine::ReadBitmap` still copied the drawing buffer's rectangle
  (7680x2160 for the whole desktop) into a BBitmap, averaged it with
  `floorf()` per pixel and copied the result again, all with the drawing
  engine locked: 250 ms a capture, with every window waiting. Summit's
  screen sharing reads the screen many times a second, so the desktop
  stuttered and the whole desktop was shared at about 2 pictures a second.
  The pixels are now averaged straight from the buffer into the caller's
  bitmap (`drawing/ScaledReadback.cpp`, 348a5d8cc2); the cursor is blended
  into a copy of only the pixels under it.
  * `tests/scaledreadback.cpp` runs the new code and the old one on a
    7680x2160 buffer: the same bytes for 64 rectangles (whole screen, bands,
    windows, clipped at both edges, 2x and 1.5x, cursor in, across and out),
    12 ms instead of 125 ms for the whole screen.
  * Deployed (repack of servers/app_server, power cycle at 15:55). On the
    machine a whole 3840x1080 read takes 10-11 ms instead of 250, a band of
    135 rows 1-2 ms instead of 30; a screenshot with the pointer is right;
    Summit shares the whole desktop at 16 pictures a second.
  * The previous contents of /boot/home/x399-stage (Oct 3-4: Terminal
    builds, airos scripts, a haiku.hpkg) are in
    /boot/home/x399-stage.saved-20261005.

- 2026-10-04: the workstation runs **air/OS**. `x399-workstation` took in
  `rock5-itx` (the branding, the whole-disk Installer, the USB, Bluetooth and
  video decoding work of the ROCK 5), and the seven system packages built from
  it - haiku, haiku_loader, haiku_datatranslators, haiku_devel, webpositive,
  smbfs and userland_fs, hrev60097+573 - replaced the installed ones under
  their old file names, with the EFI loader on the NVMe's EFI partition.
  `uname` says hrev60097+573; the Deskbar, the About window and the desktop
  picture are air/OS (seen over VNC; the boot screen cannot be seen from here,
  the same image shows the air/OS one in QEMU).
  * The installed Haiku system was not reinstalled: the whole-disk Installer
    erases the disk. The packages from before are kept as the boot menu's
    previous system state `state_2026-10-04_13:31:29` (safe mode options,
    "Select system state") and in `/boot/home/x399-backup/airos-20261004`
    with the old loader, which is also on the EFI partition as
    `BOOTX64.EFI.pre-airos`. What is in the non-packaged folders (nvidia_rm
    and its accelerant, the user's hda and mt7922 drivers, the NVDEC plug-in,
    Zink) was kept; the kernel changes since the last full install
    (hrev60097+178) only add to the interfaces they use.
  * `BOOTX64.EFI` had FAT's read-only attribute. Haiku's FAT driver let `cp`
    truncate it to nothing and then refused the write ("Operation not
    allowed"): the machine had no boot loader until `chmod u+w` and a second
    copy. Check the attribute before writing to the EFI partition.
  * The desktop stayed plain blue: its background named Haiku's logo, which
    air/OS does not ship, but in two entries (one for workspace 1, one for
    the rest), and Tracker only recognised a single one. Fixed in Tracker
    (405a25e059); the air/OS picture now shows on both monitors.
  * `check-workstation.sh`: 24 working, 0 not, against 20 and 4 before (the
    four were sound outputs and the layout line of a syslog that had rotated;
    they came back with the cold boot).
  * One boot in the first three (the second, of hrev60097+573) never reached
    the network: everything up to Tracker and sshd started, the DHCP lease
    came, then the first `ifconfig /dev/net/ipro1000/0` blocked, every later
    one queued behind it, ssh timed out before authentication and the machine
    stopped answering ARP, while the kernel's slab areas kept growing. A
    forced power cycle brought it back, and the six power cycles after that
    all came up, in 61 to 66 seconds. The merge brought network stack and
    compatibility layer changes from the ROCK 5 work that had not run on this
    machine before (deferred device interface removal, a bounded receive
    queue, the ARM64 DMA rework, mostly behind `FBSD_NONCOHERENT_DMA`); the
    hang looks like the unexplained slab growth of 2026-09-28, but which, if
    any, is the cause is open. `x399-bootlog.sh` now also saves `ps -as` (the
    threads and what they wait on), so a repeat leaves more than this.
  * Terminal opens links the way other terminals do, and lets programs that
    sign in through a browser open one: Claude Code's sign-in works from it.
    See `docs/airos/TERMINAL.md`.

- 2026-10-02: direct windows stay connected at a higher density
  (Summit issue #16). At 200 percent app_server disconnected every direct
  window, because the frame buffer is not in the coordinates a window draws
  in, so a program that composites at the screen's density with the GPU -
  Summit's pages, airTime's films - had to read each frame back from video
  memory and have app_server copy it twice more (about 10 ms of each WebGL
  frame in the browser).
  * A window with the new flag `B_DIRECT_DEVICE_PIXELS` (`Window.h`, 0x400;
    in the constructor or through `SetFlags()`) stays connected. Its bounds
    and clipping come in frame buffer pixels, rounded as the Painter rounds.
    `direct_buffer_info` gains `device_scale` (the density in percent, 200
    at 200 percent), `drawing_bits_area` and `drawing_bytes_per_row` in the
    first three words of the old reserved space, so programs built against
    the old header read them as `_reserved1[0..2]`.
  * `drawing_bits_area` is app_server's own copy of the screen, which it
    draws into and copies to the frame buffer: a direct window writes its
    pixels there too, or the next copy erases them; screenshots and the VNC
    server read it, so they show what the window drew. The back buffer is an
    area of its own now (it was a heap block), so handing it out hands out
    nothing else.
  * `tests/directscale.cpp` paints its visible rectangles into both buffers
    on the 200 percent P2415Q: its one pixel border lands exactly inside
    app_server's window frame in `nvscanout --dump` and in a screenshot,
    and the clipping follows the window when it moves. airTime draws films
    at full density through it, a 4K HDR film at 24 frames/s windowed and
    full screen, and a screenshot shows the picture.
  * Deployed as `servers/app_server` in a repacked haiku package and a power
    cycle (`install-staged-haiku-pkg.sh`).
  * Summit uses it (its pages are copied into the frame buffer by the GPU;
    Summit's docs/hidpi.md). Getting there hung the machine eight times -
    no ping on either interface, nothing reached the syslog - in two ways
    that are kernel faults, not app_server's, and are still open:
    1. **fork() of a team that maps the frame buffer.** A connected direct
       window's application clones the frame buffer area (BDirectWindow's
       daemon, `ServerMemoryAllocator::AddArea`). WebKit's launcher then
       forked the browser to start a web process, and the fork copies that
       device memory mapping copy on write: hung within seconds, every time
       (directscale and the GL probe never fork, so never hit it). Summit
       now starts processes with `load_image()`. Fork should share, or not
       inherit, areas backed by device memory.
    2. **A team that dies while nvidia_rm holds pages it locked.** A web
       process imported a clone of app_server's back buffer as host memory
       for the GPU (`os_lock_user_pages`, 8100 pages); when it quit without
       freeing the import first, the machine hung before any
       `os_unlock_user_pages` was logged. `team_delete_team()` removes the
       address space and only later, in `~Team()`, puts the io_context that
       closes the driver, so the unlock comes after the mapping is gone. A
       program that frees its imports before it exits (eglTerminate) is
       fine. Closing a dying team's descriptors before its address space
       goes, or having the driver watch the team, would fix it; until then
       Summit does not import that memory by default.

- 2026-10-02: displays can mirror each other. A display is now either a part
  of the desktop of its own or the mirror of another: it takes its source's
  place and the scale that fits the source's part on its own mode, and the
  accelerant hears of it through `B_DISPLAY_OUTPUT_MIRROR` on the output's
  config. `nvidia_rm` points the mirror's head at exactly its source's
  region (letterboxed when the monitors differ in shape; a mirror that would
  have to shrink the picture is refused, the engine only enlarges). The
  Screen preferences have a "Mirror displays" check box, shown with two
  monitors or more, that mirrors every display onto the main one; the group
  is drawn as one display numbered "1 | 2", a second click on it selects
  the other member, and "Main display" on a mirror swaps the roles.
  `screenmode --display 2 --mirror 1` (and `--mirror off`) does the same
  from a shell.
  * Both P2415Q at 200%: `nvidia_rm: layout: DP-4 head 1 3840x2160@59 at
    0,0 ... mirror`, both DP links trained and SHOWING, the desktop
    1920x1080; un-mirrored from the preferences, the countdown's Undo put
    the mirror back, the swapped roles applied, and after a power cycle the
    mirror came back by itself. `check-workstation.sh` 23 working, 0 not,
    while mirrored. The picture on the panels was not seen (no camera).
  * `tests/displaylayouttest` adds two mirror cases (alike and differently
    sized monitors, reboot, unplugged source, role swap, too small a
    mirror); all pass on the workstation.
  * Tracker stretched a scaled-to-fit desktop picture whenever the screen
    changed shape (it followed the view's resize); it is fitted again now.
  * Left as the user had it: DP-4 on the left, DP-2 on the right and main.

- 2026-10-01: scaled screenshots no longer read the desktop back from GPU
  memory. airShot issue #1 was a roughly four-second wait in
  `BScreen::GetBitmap`, before its region selector could appear. At 200%
  density, `DrawingEngine::ReadBitmap` copied the 7680x2160 front buffer
  before averaging it into the logical 3840x1080 screenshot. Direct windows
  are disconnected at that density, so the drawing buffer in RAM already
  contains the complete desktop. Read it instead; keep reading the front
  buffer at native density so direct-window pixels are still captured.
  * On the GTX 1070 with both Dell P2415Q displays at 200%, six captures
    alternating cursor inclusion fell from 3561–3719 ms to 231–476 ms
    (cursor exclusion includes airShot's 150 ms hide delay). The region
    overlay appeared 665 ms after the request, including its 250 ms settle
    delay. `tests/CaptureTiming.cpp` in the airShot repository reproduces
    the old failure and passes a 1000 ms limit with this server.
  * Cropped readbacks at a nonzero origin retained every expected RGB pixel,
    with and without cursor inclusion. The patched server also booted in an
    isolated QEMU snapshot at 100% density; cropped pixels, six timing checks
    and an airShot region-to-editor capture passed there.
  * Deployed by replacing only `servers/app_server` in the installed system
    package. The previous package is retained under
    `/boot/home/x399-backup/haiku-before-airshot-issue1-20261001-151457.hpkg`.
    A warm restart did not return; the documented NanoKVM cold power cycle
    brought the workstation back with the expected server SHA-256
    `25b954f22ba732d618ab8781dfe2240035462fdbf0e1e93732a5b8adc7921c24`.

- 2026-09-29: network shares. The Music share of the NAS is mounted when the
  machine starts, and set up where one would look for it, in Tracker's
  preferences.
  * Haiku had nothing to mount an SMB share with but what HaikuPorts has,
    fusesmb on Samba's client library, which shows the whole network as one
    volume, and libsmb2 4.0.0. `smbfs` is new, on libsmb2 6.2, which is in
    the tree now. It keeps up to three sessions with the server so that a
    file being played does not hold up a folder being opened, makes a new
    session when one is lost and opens the files again that were open, and
    remembers for five seconds what reading a folder told it about the files
    in it, which is what Tracker asks next, one file at a time.
  * userlandfs could mount one volume per FUSE file system. The name of a
    file system can now be followed by the name of an instance
    (`smbfs:music`), each of which gets a server of its own. And a FUSE file
    system that fails to mount can say why: what its `main()` returns, if
    negative, is what `mount` fails with, so that a wrong password is
    "Permission denied" rather than "General system error".
  * Found by `tests/sharetest`, and not by looking at folders: writing with
    O_APPEND went to the start of the file (the FUSE layer leaves it to the
    file system to find the end); renaming onto a file that exists failed;
    and removing a folder that is not empty was reported to have worked,
    because libsmb2 removes by opening with delete-on-close, which the server
    grants and then does not do. Nothing was deleted that should not have
    been. smbfs asks for the deletion itself now and gets the answer.
  * Packages that replace one of their own name and version stay as they
    were unless the old one is gone first, and `userland_fs` cannot go while
    `smbfs` needs it - the package daemon asks on the screen, and waits.
    `tools/deploy-smbfs.sh` does it in the order that works.
  * `tools/check-workstation.sh` looked for the display layout in the syslog
    only, and the syslog is put aside when it is full; it reads the one
    before as well.

- 2026-09-28: the monitor arrangement survives sleep and reboots, sleeping
  monitors wake, and a Bluetooth mouse moved while it connects stays
  connected.
  * The swapped arrangement (DP-4 left of DP-2) came back in connector order
    after reboots. It was lost before them: DP-4 lets go of its hot plug line
    about five minutes into DPMS off, and app_server stored every layout it
    worked out by itself, including the one without DP-4, where DP-2 had
    moved to x = 0. Seen happen on its own while the machine sat idle under
    the screen saver. app_server now stores only what the user chose, and
    writes its settings files so a reset cannot empty them; the accelerant
    ignores hot plug changes while the displays sleep, checks every
    DisplayPort link from the monitor's DPCD on waking and programs the
    layout again when one is down or something changed. Verified: 8 minutes
    of DPMS off, DP-4 dropped and returned, nothing moved, both monitors
    awake with every lane locked at once; then a power cycle came up in the
    swapped arrangement. `tests/displaylayouttest` covers the layout logic.
  * Bluetooth on a cold boot: bluetooth_server starts 29.6 s after the
    kernel, opens the radio at 33.5 s, and the MT7922's firmware download
    takes 21 s (every USB control transfer to this radio costs 3 ms, against
    0.2 ms for the keyboard), so the mouse can reconnect from about 55 s. The
    add-on scans at once. One boot that morning took ten minutes instead,
    while Wi-Fi spent that long authenticating; the server now logs each
    step with the time since boot, and `tools/boottrace.sh` runs as a system
    launch job, for the next time. A mouse moved while the add-on read its
    battery overflowed a 64-entry notification queue and dropped the link
    ("disconnected after 64 reports: Bad data"), repeatedly while it kept
    moving; fixed, not yet seen with the mouse in hand.

- 2026-09-27: Wi-Fi works. The MT7922 joins WPA2 networks from the Wi-Fi
  preferences and the WiFiStatus applet, gets its address by DHCP, and with
  the wired card unplugged in software the machine uses the internet over the
  air (1.9 MB/s up, 2.5 MB/s down; legacy rates). It attaches at boot and
  rejoins a saved network by itself. Five faults stood between scanning and
  that, each looking like something else:
  * the join "froze the machine": the driver declared WME and never set the
    hook net80211 calls, unchecked, the moment it settles on a network, so
    every join dropped into the kernel debugger with no screen to show it;
  * the firmware "ignored" two of the join's commands: a command waiting for
    its answer read only two of the three receive rings, and the grant of the
    channel hold (this firmware manages channels itself, and the grant is
    what puts the radio on the network's channel) was on none of them anyone
    read while waiting;
  * after moving between two access points of the network, every join was
    refused (0x10003): the old access point's station entry was never
    removed, because the node that named it was already gone;
  * wpa_supplicant "could not scan": Haiku reports the media in use, with its
    link bit in it, where FreeBSD reports the media set, and the supplicant
    wrote the word back (fixed in the compat layer, for every FreeBSD driver);
  * the link came up and nothing got through: net80211 leaves encrypting a
    protected frame to the driver (`ieee80211_crypto_encap`), and this one
    sent them marked protected but in the clear. Found with net80211's own
    per-station counters: unicast in stuck at the handshake's two frames.
  A sixth showed under load: the transmit ring is cut-through, so a frame is
  read from its buffer when it is sent, and buffers were reused as soon as
  the ring moved on. A long upload stopped transmission for good. Buffers are
  now kept until the card reports the frame sent.

- 2026-09-26, later: the scaled desktop is drawn sharp. The first version
  drew the desktop at its logical size and had the card's display engine
  enlarge each monitor's region, which is what X11 and Windows call bitmap
  scaling and looks like it. Now app_server draws at the monitor's density
  (`HWInterface::RenderScale`, in percent; the Painter's device scale):
  every logical coordinate is scaled on its way into the buffer, one pixel
  lines become bars as wide as the scale, filled rectangles snap to the
  finer grid, glyphs are rendered at the larger size straight from the
  glyph cache and hinting is turned off so that what a program measures at
  the logical size is what gets drawn, the cursor and drag bitmaps are
  rendered or enlarged to the density, patterns keep their logical period,
  and screenshots are averaged back down to the logical size. Windows,
  clipping, input and every program's view of the world stay logical.

  The plan was Apple's: draw at twice the density and let the display
  engine shrink the picture onto monitors that need less. The engine here
  will not shrink anything - a single monitor at 175 percent (a 4388 pixel
  picture onto 3840) is refused by NVKMS as firmly as one at 150, while
  enlarging works at any factor; the reply carries no reason across the
  ioctl. So the density is the smallest scale among the enabled monitors:
  at that scale a monitor's region is its own size and nothing is scaled
  at all, and any monitor with a larger scale is enlarged by the engine.
  Two monitors at the same scale are both sharp at every step from 100 to
  250 percent, fractional ones included, because the Painter's scale is
  fractional too (a 150 percent monitor is drawn at exactly 1.5 pixels per
  logical pixel). The cost of a fractional scale is that a logical offset
  is not a whole number of pixels: scrolling rounds and a window moved by
  one logical pixel moves by one or two, which is what every other system
  that scales fractionally does as well.

  Measured on the workstation at 200 percent on both monitors (a
  7680x2160 frame buffer, drawn one to one): text and the Deskbar are
  crisp in the frame buffer itself (`nvscanout --dump` writes it out, the
  screenshot code no longer being the thing under test); `drawtest`
  fills its window 303 times a second, draws 200 small rectangles 169
  times, scrolls it 293 times and 40 lines of text 87 times a second,
  each pushing four times the pixels of before. Direct windows are not
  connected while the desktop is scaled, since the frame buffer they would
  be handed is not in the coordinates they draw in; the GPU present path
  for OpenGL therefore falls back to presenting through app_server until
  BGLView learns the density, and the VNC server has to be told to read
  the screen through BScreen (`-ScreenReaderBDirect=0`, now in its launch
  script). Bitmaps a program supplies at the logical size are enlarged
  pixel for pixel - icons look as they do on any system without high
  density artwork; Tracker's vector icons could be rasterised larger once
  a program can ask for the density.

- 2026-09-26: two 4K monitors are a desktop rather than an expanse. The
  display engine of the card has a scaler on each head, and NVKMS accepts a
  viewport into the frame buffer that is smaller than the raster it drives -
  there is no limit on upscaling, only on downscaling - so a monitor at 150
  percent is a 2560x1440 region of the frame buffer stretched over 3840x2160
  pixels, by the hardware, for every program at once. The accelerant now has
  a layout: every connected output with a region, a scale and a timing, and
  five new hooks (`B_GET_DISPLAY_OUTPUTS`, `B_SET_DISPLAY_LAYOUT` and
  friends in `Accelerant.h`) by which app_server reads and sets it. The frame
  buffer is the smallest rectangle around the regions; its mode is
  synthesized as before.

  app_server keeps the arrangement (`DisplayLayout`), applies it before the
  first mode is set so the screen comes up once, remembers it per monitor
  (EDID vendor, product, serial and name, plus the connector) in
  `~/config/settings/system/app_server/displays`, and puts a monitor it has
  never seen at a scale that suits its density - 200 percent from 175 dots per
  inch, 150 from 130 - to the right of the others. Windows travel with their
  monitor when the layout changes, in proportion when the monitor's size in
  desktop pixels changes, and windows left where nothing is shown are brought
  onto the nearest monitor. input_server is told which parts of the desktop
  are monitors and keeps the cursor on them, sliding along an edge rather
  than disappearing into the dead corner below the smaller monitor.
  `BWindow::Zoom()` asks for the monitor holding most of the window
  (`AS_GET_DISPLAY_FRAME`), unless the user turned that off; `CenterOnScreen()`
  and alerts use the monitor the window or the mouse is on; the Deskbar
  lives on the main display. `screenmode -d` lists all of it and
  `screenmode --display 2 --scale 150 --position 0 0 --primary` changes it
  from a shell.

  Measured on the workstation: both monitors at 200 percent give a 3840x1080
  desktop and at 150 a 4480x1440 one; changing one monitor's scale, swapping
  them by moving one onto the other, and making the other one main each take
  effect at once with no error from NVKMS; a window at 2700,200 on the right
  monitor was at 140,200 after the monitors swapped, and one on the left was
  at 2145,225 after the swap and 2220,300 after that monitor went from 200
  to 150 percent; `Zoom()` on a window at 2700,200 gave 2565,29-4474,1074,
  that monitor less the tab, and the classic setting gave the whole desktop
  less the Deskbar. The Screen preferences were driven over VNC: the two
  monitors show as numbered rectangles with the selected one's EDID beside
  them (Dell P2415Q, 23.8", 185 dpi, serial, week 48 of 2016), "Identify
  displays" put a large 1 and 2 in the middle of each monitor, dragging 2
  onto 1 swapped them on Apply - the preferences window itself moved to the
  other monitor, and the Deskbar to the main display - and the countdown
  put them back when it ran out. Two things could not be checked from here: whether the
  scaled picture looks right on the panels (only a person can see them), and
  hot plugging, which the accelerant watches both through NVKMS's display
  events and by reading the hot plug lines from resman every two seconds.
  The NanoKVM has no video on this machine, and a monitor cannot be pulled
  out over the network.

  Hardware without a layout of its own (the frame buffer and VESA drivers)
  gets the scale done in software: app_server draws at the logical size and
  enlarges it into the frame buffer as it copies, whole scales by
  duplication and fractional ones by interpolation; direct windows are left
  disconnected then, since the frame buffer they would get is not what they
  draw in. This is untested, as the machine has no such display.

  2026-09-26, later: with the desktop drawn at density the software path
  is simpler than that - the frame buffer stays the panel's, the logical
  size is the panel's divided by the scale, and app_server draws into it at
  the scale; nothing is enlarged on the copy. It was verified on the ROCK 5
  ITX (rk3588_display, one head, no layout hooks): 200 percent live from
  `screenmode`, the scale kept across a reboot, and the Screen preferences'
  scale menu, countdown and undo. That menu had been disabled whenever the
  driver had no layout of its own; `DisplayLayoutState::CanScale()` now
  separates "the driver cannot arrange monitors" from "nothing can be
  scaled", and Apply, Undo and Revert send the scale through
  `set_display_layout()` while the mode keeps the classic path. On this
  machine the change is behaviour-neutral (the accelerant has the layout
  hooks), so the workstation was not power cycled for it.

- 2026-09-20: H.264 decodes on the card. The engine exists - resman lists
  an `nvdec0` engine and an `NVC2B0` class - and a channel on it, bound like
  any other but to the decoder rather than to graphics, runs methods we write.
  Four things about it are not in the header that describes the picture setup,
  and each cost a measurement:
  * The arrangement of pixels in memory is not the one Mesa and nouveau use for
    graphics surfaces on this chip. Rather than guess, the card was asked: a
    picture whose every line is a different value, then one whose every column
    is, decoded and read back, say where each pixel went.
  * A reference whose field markings are zero is not a reference. The engine
    does not complain - it predicts from picture zero instead, which for a
    slowly changing picture is nearly right and looks like a subtle motion bug.
  * The length of bitstream the engine is given must count an end of stream
    marker, which it appends itself. Ending it at the last byte of the last
    slice stops the decode partway down the picture while still reporting every
    macroblock decoded and none in error.
  * A picture keeps its place in the engine's reference table for as long as it
    is a reference, because the motion data temporal prediction reads is filed
    under the place rather than under the picture.
  The last two came from upstream Mesa's own NVDEC code, once guessing here had
  stopped being the quicker way. The decoder is a media add-on, so every
  program that plays video gets it, and `NVPlay` is a film player: seventy
  seconds of 1080p play in seventy seconds with picture and sound never more
  than a tenth of a second apart.
- 2026-09-17: hardware inventory captured with SystemRescue. The NVMe
  contained an ARM64 Haiku test install from the ROCK 5 lab; its files,
  EFI partition and GPT were backed up before reuse.
- 2026-09-17: stock nightly (hrev60097) booted from NanoKVM virtual USB.
  The second Threadripper host bridge (root bus 0x40: NVMe, GPU, one CPU
  xHCI, one AHCI) was not enumerated. After reading every ACPI host bridge,
  its resource windows and `_PRT`, the NVMe, the fifth xHCI controller and
  the GPU's HDMI audio function appear.
- 2026-09-17: installed the system to the NVMe (GPT: FAT ESP + BFS "X399")
  from the live image and booted it without removable media.
- 2026-09-17: `nvidia_rm` (X547's OS layer + NVIDIA 570.86.16 proprietary RM
  core) completes `RmInitAdapter` on the GTX 1080 Ti; app_server uses the
  NVKMS accelerant at 1920x1080.
- 2026-09-17: NVK (X547's RM backend + pre-Volta patch, built natively on the
  workstation with LLVM 20, libclc and SPIRV-LLVM-Translator) enumerates
  "NVIDIA GeForce GTX 1080 Ti (NVK GP102-A)" with Vulkan 1.3 and creates a
  device. The GPU fetches the GPFIFO and loads the channel's context (with the
  RM watchdog disabled), but submitted work does not complete yet. The RM runs
  its own watchdog channel without errors. `nvidia_rm` gained an RC timer,
  registry settings (`~/config/settings/kernel/drivers/nvidia_rm`,
  `RegistryDwords "Key=Value;..."`) and development register/VRAM readers.
- 2026-09-17: the accelerant drives every connected display on its own head
  and presents one spanning desktop (left to right, each display at its
  preferred mode). Verified with the NanoKVM on HDMI-0 and DP-0 forced on
  through `force_connected DP-0` in `~/config/settings/nvidia_rm_accelerant`.
- 2026-09-17: fixed an early-boot panic with on-screen debug output: 32 CPUs
  printing during application processor start-up made `dprintf` trip the
  spinlock deadlock detector.
- 2026-09-17: ACPI S3 on x86_64. A real mode trampoline at 0x81000 resumes
  the boot CPU, which restarts the other CPUs through INIT/SIPI and keeps the
  TSC continuous. The PCI root driver restores configuration space and MSI-X
  tables, `nvme_disk` resets its controller, and `nvidia_rm` suspends NVKMS and
  the RM; the accelerant sets the mode again after resume. Verified by waking
  with the power button: the syslog of the resumed session reaches the disk and
  the display comes back, but its contents are not redrawn yet. Testing uses
  `x86suspend s3 [flags]` (generic syscall `x86_suspend`). Later on the same
  day the workstation stopped reacting to the NanoKVM power button and needs a
  power-cycle at the wall.
- 2026-09-18: suspending with 32 CPUs deadlocked, because parking the
  application processors one at a time while the scheduler was still running
  let a halted processor block anything that waits for it. They are parked at
  once now, with interrupts already disabled, and the file systems are synced
  before the system is quiesced. Message signaled interrupts stopped working
  after resuming until the HyperTransport MSI mapping was restored along with
  configuration space; PME is cleared while sleeping, since a network card
  left in wake on LAN mode woke the machine seconds after it slept.
  Suspending can be traced step by step, the trace is also kept in memory
  (`x86suspend trace`, or `x86suspend watch <host> <port>` to have it reported
  over the network), and single parts can be skipped for bisecting.
  Sleeping and waking themselves work: all 32 CPUs come back, and the
  accelerant restores the display. The NVMe and the network card, however,
  usually stay dead afterwards, which also blocks anything that needs the
  disk (a clean shutdown, for instance). One run had everything back,
  including the network 8 seconds after waking, so the failure is not
  systematic. Skipping the USB, NVMe or driver power hooks does not change
  it, all CPUs demonstrably restart, and the IOMMU is disabled on this board,
  so the cause is still open. Debugging it further needs a channel that
  survives the failure: the machine cannot write its log to disk, and the
  HDMI capture of the NanoKVM returns stale frames.
- 2026-09-18: Vulkan works. Submitted work never completed because a mapping
  smaller than a page came back pointing at the start of its page: RM rounds
  the ranges of a mapping context down to a page boundary and hands the offset
  inside the page to the caller in `pLinearAddress`, which neither the Haiku
  RM clients nor the driver added back. A channel's USERD is 512 bytes and
  eight of them share one page, so every channel after the first wrote its
  GPPut into the first channel's USERD: the wrong channel was kicked and the
  submitting one never ran. It showed up as GPFIFO entries being consumed with
  nothing executing, no MMU fault and no semaphore release; submitting an entry
  pointing at unmapped memory, which faulted on one channel but was silently
  swallowed on the other, is what pinned it down. The driver now also releases
  the mapping context after use, as `nvidia_mmap()` does on Linux, instead of
  leaving it to be reused by the next mapping on the same file descriptor.
  With that, `vkprobe` passes reliably: a compute-free buffer copy and a
  rendered triangle read back correctly, waiting on a real GF100 semaphore
  release rather than polling.
- 2026-09-18: DisplayPort detection investigated with the new `nvdpyinfo`
  tool. NVKMS only learns about a DisplayPort connection through a hotplug
  event from RM, and RM itself reports no connection on any of the three
  physical DisplayPort connectors: the hot plug detect line reads low (a
  detection with DDC and load detection disabled returns only HDMI-0), and a
  DPCD read over the AUX channel of DP-0, DP-2 and DP-4 gets no reply, while
  the same query on the HDMI connector reports it connected. So the GPU sees
  nothing on those connectors at all, which points at the cable, the port or
  the monitor rather than at the driver. `nvdpyinfo --watch <seconds>` polls
  both RM and NVKMS and reports every change, to catch a plug event when one
  happens.
- 2026-09-18: a GTX 1070 (GP104, 0x10de:0x1b81) works with no driver change -
  the driver binds to any NVIDIA display device rather than a list of
  identifiers - and its DisplayPort output is detected: a Dell U2414H on DP-4
  comes up beside the HDMI display, both driven by their own head, for one
  3840x1080 desktop. So the DisplayPort connectors of the 1080 Ti that never
  reported a connection were a hardware matter, not a driver one.
- 2026-09-18: `vkbench` (tests/vkbench.c, built and run by
  tools/build-vkbench.sh) measures compute throughput, fill rate and copy
  bandwidth with real SPIR-V shaders. Its first numbers exposed that every
  submission took exactly one second: NVK waited for the channel's semaphore
  by polling an RM event with a one second timeout, and resman never delivers
  that event on this GPU, although GPU interrupts themselves do arrive.
  Spinning on the semaphore first, with a one millisecond poll behind it,
  turned 17 GFLOP/s into 1.4 TFLOP/s and 0.41 into 55 Gpixel/s. On the
  GTX 1070: 1.4 TFLOP/s of fused multiply-adds, 55 Gpixel/s fill,
  58 GB/s copy within video memory and 6.1 GB/s upload over PCIe.
  Delivering the completion interrupt to the waiting thread instead of
  polling is still open.
- 2026-09-18: OpenGL reaches the GPU. Haiku's OpenGL kit (libGL.so and the
  renderer add-ons in add-ons/opengl) belongs to the mesa package, 22.0.5 here,
  so the Zink renderer is built from that same release - the add-on and libGL
  then share one glapi - while Zink itself talks to NVK through the Vulkan
  loader, which is a stable interface between the two Mesa versions. The new
  add-on (src/gallium/targets/haiku-zink in src/mesa-build/mesa-hgl, built by
  tools/build-mesa-hgl.sh) is the software renderer's target with the screen
  created by `zink_create_screen`, and HGL_NO_ZINK falls back to software.
  `gltest` (tests/gltest.cpp) reports
  `renderer: zink (NVIDIA GeForce GTX 1070 (NVK GP104-A))`, OpenGL 4.5, and
  clears and presents 800x600 frames at 186 fps, against 43 fps for the same
  window drawn by softpipe.
  Three things had to be fixed on the way: the page kind table in NVK's resman
  backend was Turing's, so every depth buffer was refused on Pascal; the front
  buffer has to be flushed with the pipe context, which a hardware driver needs
  to read the image back; and the Haiku winsys asked BBitmap for a default row
  size while advertising an aligned one, so Zink's copy ran off the end of the
  bitmap.
  What is left: a draw call stops the graphics engine with a class error
  (Xid 69), after a few frames have already been presented, while the same
  draws issued directly through Vulkan by `vkbench` (including depth buffers
  and vertex buffers) run fine. NVK's flush now gives up after
  NVK_NVRM_TIMEOUT_MS instead of hanging, which makes NVK dump the command
  buffer that failed; it ends with the ordinary draw macro, so the offending
  state is somewhere earlier in the stream.
- 2026-09-18: the draw that stopped the graphics engine was an unaligned
  constant buffer. Diffing the command stream NVK builds for a Zink draw
  against one for the same draw issued straight through Vulkan left exactly
  three methods: SET_CONSTANT_BUFFER_SELECTOR_A/B/C. Its address,
  0x205700140, is 64 byte aligned, and a pre-Turing GPU wants 256 there;
  binding it that way raises a class error (Xid 69) and the channel is dead
  from then on. The address is a dynamic uniform buffer, whose offset comes
  from the caller, and the caller is Mesa's OpenGL state tracker: it uploads
  the default uniform block at a hard coded alignment of 64. On Turing, where
  NVK asks for 64, that happens to be right. It now uses the alignment the
  driver reports, and OpenGL draws correctly.
  What is left in this area: presenting a frame still copies the image back
  through the CPU into a BBitmap, which is what limits a small window to a few
  hundred frames a second.
- 2026-09-18: the GPU now writes the frame into app_server's own bitmap.
  Presenting used to read the image back with the CPU and copy it into the
  window's bitmap; the bitmap's pages are now pinned and mapped into the GPU's
  address space, so the copy that presents a frame is a GPU write into the
  memory app_server already reads, and the CPU never touches the pixels. That
  needed three pieces: `os_lock_user_pages()` in the driver, which was a stub
  that panicked and now locks the pages with lock_memory_etc(); support for
  VK_EXT_external_memory_host in NVK's resman backend, built on resman's OS
  descriptors; and a present path in Zink that imports the window system's
  buffers once and copies into them on the GPU. Redrawing a window no longer
  touches the GPU at all - the frame is already in the bitmap.
  Measuring it also exposed that every submission was costing a millisecond:
  the flush waited with poll(), whose shortest sleep is a whole tick, however
  short a timeout is asked for. Spinning first and then sleeping in short steps
  took an empty submission from 1.08 ms to 13 us, which is worth far more than
  the present path itself: the lit sphere at 800x600 went from 167 to 500 fps,
  and GLTeapot from 363 to over 1000.
  The frame rate depends heavily on the GPU's clocks, which only ramp under
  sustained load, so measurements are only comparable after a warm up.
  What is left in this area: presenting still crosses the PCIe bus, and the GPU
  writes system memory at only 1.6 GB/s (it reads at 6.2). Presenting into the
  screen's own frame buffer in video memory, through Haiku's direct window
  mode, would avoid the bus entirely.
- 2026-09-18: measured, with the GPU warmed up first, since its clocks only
  ramp under sustained load. A lit sphere at 1600x900: 230 fps and 1.0 s of
  processor time per 5 s with the GPU presenting, against 186 fps and 1.6 s
  when the frame is copied by the CPU - a fifth more frames for two thirds of
  the processor time, and the gap grows with the window, since the copy the CPU
  no longer makes is proportional to its area. At 800x600 the frame rates are
  the same and only the processor time differs (2.0 s against 2.6 s).
  GLTeapot reaches 2390 fps, against 238 on the software renderer.
- 2026-09-18: the machine froze twice - no network, no keyboard, a still
  desktop whose clock had stopped - and the second freeze left the file system
  damaged. The cause was in the page pinning added for presenting:
  os_lock_user_pages() recorded B_CURRENT_TEAM, which is not a team but a
  sentinel meaning whichever team is running. The pages are unlocked when
  resman tears the memory down, which happens while the application is being
  cleaned up and can run in another team's context, so the unlock went to the
  wrong address space. Recording the team that locked the range fixes it:
  tools/gl-stress.sh now runs the test repeatedly, killing it while the GPU is
  writing into its window, and the machine stays up with every pinned range
  released (64 locked, 64 released).
  The kernel debugger could not be reached over the KVM's USB keyboard, so the
  diagnosis came from the code rather than from the frozen machine; a serial
  console would have made it quicker.
- 2026-09-18: the GPU had been talking to the machine at PCIe 2.5 GT/s, a
  quarter of what the link can carry. Both ends advertise 8 GT/s, but the link
  drops to its slowest speed whenever the GPU is idle and nothing here raised
  it again - resman does that from the power management that this driver does
  not run. NVK now asks resman to train the link when it opens the device.
  Copies into memory the application owns went from 1.6 GB/s to 4.6, and
  reading a frame back from 1.6 to 2.8; `nvdpyinfo --rm` reports the link, and
  `--pcie-speed <gen>` retrains it by hand.
- 2026-09-18: a program other than the accelerant can now draw straight into
  the screen. The accelerant's frame buffer is an ordinary resman memory object
  in video memory, so after each mode set it shares that object
  (`NV0000_CTRL_CMD_CLIENT_SHARE_OBJECT`, giving away the right to duplicate it)
  and tells the driver where it is; anyone can then ask the driver for it
  (`NV_HAIKU_GET_SCANOUT`) and duplicate the handle into its own client
  (`NV_ESC_RM_DUP_OBJECT`). `nvscanout` proves the mechanism from a plain
  program: it duplicates the handle, maps it and paints a band that appears on
  the monitor.
  NVK imports the same memory through a private agreement on the pNext chain of
  `vkAllocateMemory` (`VkImportScanoutMemoryHAIKU`, in `vk_haiku_scanout.h`),
  which needs nothing of the Vulkan loader. `vkbench scanout` then has the GPU
  copy a frame into it at 7.8 GB/s - a whole 1920x1080 frame in 1.07 ms,
  against 2.93 ms to send the same frame to system memory. The screen turned
  the colour the GPU filled it with, which is the proof and also the warning:
  writing the whole frame buffer tramples what app_server drew, so the renderer
  must copy only the window's visible rectangles.
  What is left in this area: Zink has to use it - present into the scanout at
  the window's position, clipped to what Haiku's direct window mode reports is
  visible, and fall back to the existing host-memory path when the window is
  not direct-connected.
- 2026-09-18: OpenGL now presents straight into the screen. A window in Haiku's
  direct mode is told where it sits and which rectangles of it are visible, so
  Zink copies the frame into those rectangles with the GPU rather than into the
  window system's bitmap, which app_server would then have to write to the
  screen a second time. A lit sphere at 1600x900 went from 210 to 969 frames
  per second - four and a half times - and a calculator opened over the window
  stays untouched while the sphere keeps turning around it, so the clipping is
  honoured. `ZINK_NO_SCANOUT_PRESENT` and `GLTEST_NO_DIRECT` both fall back.
  The accelerant had to advertise `B_PARALLEL_ACCESS` in its modes: without it
  `BDirectWindow::SupportsWindowMode()` says no and no window is ever direct
  connected. The frame buffer really can be written while the GPU draws, so the
  flag is honest.
  tools/gl-stress.sh still passes - the test killed three times while the GPU
  was writing the screen, the machine up and every pinned range released.
  What is left in this area: presenting is not synchronised with the display,
  so a frame can tear.
- 2026-09-18: a window that had stopped drawing is repainted again. Nothing is
  in the window system's bitmap on this path, so there was a hole wherever
  something had covered the window; the front buffer still holds the last
  frame, so the renderer puts that back. Haiku says a direct window has been
  uncovered through DirectConnected rather than a redraw request, so that is
  where it happens - on the window's thread, with a command buffer of its own
  and no part of the application's context, and only once the application has
  been quiet for a tenth of a second, since while it draws its own next frame
  repairs the window and its context owns the image.
  Two things cost an hour between them and are worth remembering:
  * The workstation's clock runs a few minutes ahead of this machine, so tar
    preserving timestamps made ninja decide that freshly copied sources were
    older than the objects built from the last ones. Two builds silently kept
    the previous binary. The sync scripts now extract with `-m`.
  * The screen saver blanks the display after a few idle minutes, and a blanked
    desktop makes app_server report that no part of any window is visible - no
    clipping rectangles, so no window is presented into the screen and the
    frame rate quietly returns to the old path. It also stops the KVM
    capturing, which is the quickest way to notice. Kill `screen_blanker`
    before measuring.
- 2026-09-18: changing the screen mode under a running OpenGL program works.
  The accelerant publishes the new frame buffer each time (the resman handle
  changes), and the renderer notices because the window system reports a
  different frame buffer address - it used to compare only the row pitch, which
  two modes of the same width share, and would have gone on painting into a
  surface nobody was scanning out.
- 2026-09-18: sound plays. `tests/soundtest.cpp` runs a tone through the media
  kit and counts the frames the driver takes: 144000 frames in 300 buffers over
  3.0 s, which is the 48 kHz the stream asked for, so the hardware is clocking
  the stream rather than a software timer free-running. That is the analog
  output on the motherboard (AMD Family 17h HD Audio). The GPU's own HDMI audio
  (`GP104 High Definition Audio Controller`) is found but not usable: the hda
  driver reports `hda_audio_group_get_widgets failed` and `no active codec` for
  it. Nobody here can listen to the speakers, so what is proven is that the
  stream runs at the hardware's clock, not that sound reaches the jack.
- 2026-09-18: soaked the present path (tools/gl-soak.sh): three windows drawing
  into the screen at once, twenty windows opening and closing, eight mode
  changes underneath a running program, five kills while the GPU was writing
  the screen, and a quiet window covered and uncovered four times. The machine
  stayed up through all of it, every pinned range came back (10 locked, 10
  released - those are the host path, which still runs for a window's first
  frames before Haiku connects it directly, so both paths were exercised), the
  driver logged nothing new, and the desktop was clean afterwards. The program
  that had the mode changed underneath it kept drawing at 1252 fps.
  Three windows all report the same frame rate whatever their size, because
  each is held up pushing the sphere's eight thousand triangles through
  immediate mode rather than by the pixels; on sixteen cores they do not slow
  each other down.
  Reading the code during the soak turned up two things it would only have
  caught by luck: the application's thread replaces the frame buffer on a mode
  set while the window's thread may be reading it to put the last frame back,
  which is a use after free waiting for a badly timed resolution change; and
  the frame is copied into the screen with no conversion but nothing checked
  that the two agree on what a pixel is. Both are fixed and the soak was run
  again with them in.
- 2026-09-19: the graphics card's HDMI audio works. Haiku's hda driver skipped
  any widget whose capabilities said digital when looking for something to play
  through, so a codec on a graphics card - where every converter is digital,
  because its outputs are the HDMI and DisplayPort connectors - found nothing
  and was thrown away with "no active codec". Its converters enumerate fine and
  report 16, 20 and 24 bits at 32 to 192 kHz; they were simply never
  considered. The driver now looks for analog first, so a codec with both
  kinds keeps behaving exactly as it did, and falls back to digital only when
  there is no analog converter, switching it on (DIGEN) as it goes.
  Two outputs now, and both clock their stream at the hardware's own rate:
  48160 frames a second through the graphics card, 48241 through the
  motherboard, against the 48000 asked for. tests/audioout.cpp lists them and
  chooses which one the system uses, since they are both called "HD Audio".
  The driver now reads what a display says about its own audio and logs it,
  because "no sound" and "this screen has no speakers" are otherwise the same
  thing seen from a machine with nobody in the room. This monitor answers
  `"MOREJOY" over HDMI, speakers 0x1, 1 audio format`: speaker allocation 0x1
  is front left and right, so it takes stereo - which is exactly what can be
  sent without infoframes or channel mapping. Neither of those is implemented,
  so anything beyond stereo will need that work. And as with
  the motherboard's output, what is proven is that the hardware clocks the
  stream, not that a speaker makes a sound - that needs someone in the room.
  Replacing a driver that ships with Haiku needs care: the kernel looks in the
  user's non-packaged directory, the user's, the system's non-packaged one and
  then the system's, and hda is already in the last. Putting a replacement in
  the system non-packaged directory is not enough - the packaged one still
  wins, silently, and the old binary keeps running while you wonder why nothing
  changed. It has to go in the user's, with the dev/ symlink beside it, because
  a legacy driver is found through that tree rather than by its binary.
  tools/deploy-hda.sh does it.
- 2026-09-18: looked into what it would take to stop the tearing. Resman can
  say when the display is between frames - a GF100_DISP_SW object takes
  NV9072_CTRL_CMD_NOTIFY_ON_VBLANK and delivers the answer through an operating
  system event, which this driver already wakes select() on. But resman only
  allocates that object underneath a channel, and the accelerant has none: it
  drives the display through NVKMS and never touches a channel
  (`nvvblank` demonstrates the refusal). The two ways on are to give the
  accelerant a channel purely to hang the object off, or to use NVKMS's vblank
  semaphore control, which writes a counter into a surface at each blank and
  needs no channel - and which would also let the GPU wait for the blank itself
  instead of the processor waiting and then submitting.
- 2026-09-18: the tearing is gone. NVKMS can say when the display is between
  frames without needing a channel - a client registers a page and NVKMS writes
  the frame number into it at every blank - and this port reported it
  unsupported only because `nvkms_vblank_sem_control()` was a stub returning
  false, where the Linux driver has it on by default.
  Turning it on took the machine down at the first blank, and the first guess -
  a display interrupt nothing here acknowledges - was wrong. NVKMS asks for a
  timer from inside its vblank callback, resman calls that callback from the
  interrupt, and Haiku's allocator cannot be used there because the slab
  allocator's depot lock is an rw_lock. The driver already knew this for
  resman's own allocations and keeps memory set aside for them; the NVKMS timer
  queue did not, and called new(). It now takes from a handful of timers set
  aside in advance whenever interrupts are off, and running out costs one
  missed notification rather than the machine. That was a latent hang
  regardless of tearing: anything that made NVKMS allocate a timer at interrupt
  time would have done it.
  With that fixed, `nvvblank` counts 601 notifications in ten seconds - 60.0 a
  second, 16.25 to 17.07 ms apart. The accelerant turns those into the
  semaphore Haiku asks for, so `BScreen::WaitForRetrace()` works for every
  program on the machine: `retracetest` measures 60.0 a second with nothing
  timing out, three runs alike. And Zink waits for the blank before it starts
  overwriting what the display is showing, which takes a lit sphere at 1600x900
  from 957 frames a second to 60.0. Three windows at once hold 60.1 each, and a
  program keeps its lock through a mode change (59.6 across twenty seconds
  containing two of them).
  Three things had to be got right, each found by measuring:
  * The request counter needs a fence. NVKMS expects a channel's semaphore
    release to write it, so nothing orders a write made by the processor and it
    sits in a write buffer unseen - a clean 60 a second that stopped dead after
    290. The accelerant sidesteps this by only reading the frame number, which
    NVKMS updates at every blank whether or not anything asked.
  * Waiting after the swap, which the renderer already did, does not stop
    tearing: the copy has already happened. The wait has to come first, so the
    copy starts at the blank - a millisecond of copying against sixteen of
    scanning - and stays ahead of the beam.
  * The semaphore must release every waiter on the same blank, not one per
    blank. Two windows were getting every other frame each, 30 a second.
    B_RELEASE_ALL also leaves the count at zero, so a semaphore nobody waits on
    does not build up stale blanks.
  Vertical sync is off unless a program asks for it, which is Haiku's
  convention - `SwapBuffers(true)`, or `HGL_VSYNC` to force it.
- 2026-09-19: OpenGL now reaches the GPU without anything being set in the
  environment. Everything measured until now was run with VK_ICD_FILENAMES and
  LD_LIBRARY_PATH pointed at the build by hand; a program started from the
  Deskbar got the software rasterizer and nobody would have known why. The
  library path turns out never to have been needed - the manifest names the
  driver by absolute path and its dependencies are all in system directories -
  and the manifest only had to be somewhere the loader looks. It looks in the
  add-ons directories, not the data ones: asking it with `VK_LOADER_DEBUG=all`
  prints the four it searches, which is quicker than guessing (two wrong
  guesses here). `tools/haiku-build-nvk.sh` now installs it into
  /boot/system/non-packaged/add-ons/vulkan/icd.d.
  GLTeapot, started as the Deskbar starts it, now draws on the GPU - and at 60
  frames a second rather than the 2390 it managed before, because it asks for
  vertical sync and until today the accelerant had no retrace semaphore to give
  it, so the request quietly did nothing.
  Two things this turned up that are worth knowing:
  * Zink's software fallback, which runs when no Vulkan device can be used,
    dies at teardown on a locked mutex and leaves the program stopped in the
    debugger. It is upstream-equivalent code and Haiku's own Software Pipe
    add-on does not do it, the difference being softpipe against llvmpipe -
    this build has `-Dllvm=disabled`. It only matters if the GPU driver cannot
    be loaded at all, which is no longer the case here.
  * The fallback exists because a renderer add-on cannot decline.
    `GLRendererRoster::GetRenderer` returns the first add-on's answer without
    looking at it, so an add-on that returns NULL leaves the program with no
    renderer instead of passing it to the next one. Fixing that in libGL would
    let this add-on simply stand aside when there is no GPU.
- 2026-09-19: measured what 2D drawing costs, since the accelerant offers no 2D
  hooks at all and every pixel app_server draws crosses the bus. The same
  drawing into an off-screen bitmap, against a window on the screen
  (tests/drawtest.cpp, on a quiet machine):
      fill a 1000x700 window    2.71 GB/s against 23.59   0.12x
      200 small rectangles      1.44 GB/s against  7.97   0.18x
      scroll it up 20 rows      5.26 GB/s against 18.95   0.28x
      40 lines of text                                    0.73x
  So drawing on the screen is four to eight times slower than drawing in
  memory, and it is the bus, not the processor. In absolute terms it is still
  969 whole-window fills a second, which is far more than a desktop needs, so
  this is not what anyone would notice - it would begin to matter at several
  times the pixels, which three or four monitors would be.
  The cheap way out does not exist on this card. Putting the frame buffer in
  ordinary memory and letting the display read it back would make the
  processor's drawing twenty times faster, and NVKMS refuses: every mode set
  comes back NV_ERR_INVALID_ARGUMENT and the machine stops at the boot splash
  with no display (it stays reachable over the network). A discrete card's
  display engine scans out of video memory. The switch that turned this on has
  been taken out again rather than left as a trap; NvKmsBitmap keeps the
  ability to allocate elsewhere, with a comment saying not to use it for
  anything scanned out.
  What is left, if 2D ever needs to be faster, is a real 2D engine: the
  accelerant would answer B_SCREEN_TO_SCREEN_BLIT and B_FILL_RECTANGLE by
  putting work on a channel, which means giving the accelerant a channel - the
  same thing the vblank work wanted before NVKMS's own route turned out to
  need none.
- 2026-09-19: found by reading rather than testing - NVKMS stops reporting
  blanks for a head it has shut down, so every path that sets a mode has to ask
  again, and coming back from suspend goes through ApplyMode directly rather
  than through SetDisplayMode. Two of the three places did; the resume one did
  not, which would have left WaitForRetrace timing out for ever and vertical
  sync quietly doing nothing once the machine had been asleep. All three now
  share one call. The mode change paths are measured - 60.0 a second across a
  change either way - but the resume path cannot be until the serial console
  makes suspend testable, so that part is reasoned, not proven.
- 2026-09-19: dragging a window - the commonest thing anyone does with one, and
  the one case never tested - left a staircase of frames across the desktop,
  one at every position the window had been. It reproduces only on the path
  that draws into the screen: `ZINK_NO_SCANOUT_PRESENT=1` leaves the desktop
  spotless, which is how it was pinned on this work rather than on Haiku.
  Two fixes were wrong before tracing settled it, and both were worth the
  detour:
  * It is not frames sent with stale coordinates. The trace shows presents stop
    when the window system says stop and resume at the new position, with none
    in between, so the ordering was already right.
  * Waiting on the queue from the window's thread does nothing, because zink
    submits batches from a worker thread and a batch that has only been flushed
    is not on the queue yet.
  What actually happens is that a window that moves leaves a strip behind for
  the window system to repaint, and a program drawing into the screen at eight
  hundred frames a second outruns that repaint, so the strips linger until it
  stops. A window that has just moved therefore presents through the window
  system's bitmap until it has been still for a quarter of a second - slower,
  and free of this - and a window is still almost all of the time. A still
  window is back to 911 frames a second at 1600x900, vertical sync to 60.3, and
  the soak passes with every pinned range released.
  The present now also waits for its copy to have happened rather than only to
  have been sent, for the same worker thread reason. That costs about six per
  cent (911 against 970) and is what stops a copy landing after a window moves.
  `GLTEST_MOVE=1` is the reproducer.
  Worth remembering: one screenshot in this hunt came back entirely black and
  looked like a catastrophe. It was the screen saver, on a machine rebooted
  several times since it was last killed - the trap recorded above, walked into
  again a few hours later.
- 2026-09-19: resizing a window, tried next for the same reason dragging was -
  it is ordinary, and it had never been tried. It changes the frame buffer, the
  texture behind it and the clipping all at once. Two things came out of it,
  neither a defect in this work:
  * A program has to say what part of its new frame buffer to draw into. gltest
    did not, so the picture stayed the size the window started at and the rest
    was black - on both paths, which is how it was placed on the test rather
    than the renderer. It handles `FrameResized` now, as a program must.
  * With that fixed, a window resized three times a second leaves the area it
    shrank off it showing its old contents for a moment. The trace says the
    scanout path is not running at all while this happens - the settling rule
    holds, no presents at all between the notifications - so nothing here is
    painting stale pixels. It is the window system's repaint of the vacated
    area not keeping up, and it clears completely the moment the resizing
    stops. It shows on this path and not through the bitmap because this path
    is three times faster and leaves the repaint less room.
  `GLTEST_RESIZE=1` is the reproducer, beside `GLTEST_MOVE=1`.
- 2026-09-19: switching workspaces, the last ordinary thing left untried, and
  the one that nearly cost a working feature.
  A program that switches its own workspace from inside its drawing loop hangs,
  and killing the hung program takes the machine down with it - four times
  here, twice leaving a test binary full of syslog text, which is what a hard
  power cycle does to a file written a minute earlier. That looked like a
  serious defect in drawing into the screen, and the scanout present was
  switched off by default because of it.
  It was the test. A program asking the window system to take its window off
  the screen from inside its drawing loop deadlocks against the window system
  waiting for that same program to say it has stopped drawing, and no real
  program does that. Switching workspaces the way a person does - from another
  program, `tests/switchws.cpp` - works perfectly with the feature on: 1124
  frames a second across five switches, clean exit, machine untouched. The
  default is back to drawing into the screen, and gltest no longer offers a way
  to deadlock itself.
  One real fix came out of it: coming back from another workspace hands the
  window a buffer description with no address and a row of zero bytes, and
  believing it meant going back to the driver for a frame buffer of that shape
  on every frame. Nothing describes a frame buffer of no width, so that is
  refused now.
  The lesson is the same one dragging taught, pointing the other way: check
  whether the test is doing something no program would before believing what it
  says about the code.
- 2026-09-19: checked the file system after a day of hard power cycles, because
  two test binaries had come back full of syslog text and that is what a cut of
  power does to a file written a minute earlier. `checkfs /boot` found the
  structure sound - 87018 nodes, no block unallocated, none allocated twice -
  but 2548174 blocks, about 4.7 GiB, leaked by the crashes. It reclaimed them:
  the volume went from 65.5 GiB used to 60.8, and a second pass with
  `checkfs -c` reports nothing left to free. Worth running after a run of power
  cycles; it is not only space, it is the check that nothing worse happened.
- 2026-09-19: `tools/check-workstation.sh` asks the machine whether it is doing
  what it is supposed to, one line per thing, and comes back 13 working, 0 not.
  Every check in it exists because something looked fine today and was not: a
  build that never installed, a driver the kernel never loaded, a screen saver
  that quietly turned the GPU path off, a Vulkan driver visible only to a shell
  with the right variables set. So the programs it runs have nothing set in
  their environment, which is how anything started from the Deskbar sees the
  machine, and it asks the device tree rather than the syslog, which is trimmed
  as it grows.
  Writing it turned up four wrong assumptions of my own rather than any fault
  of the machine: `df` prints a block of lines here rather than a table,
  `ifconfig` puts a space after "inet addr:", `sysinfo` counts threads so
  sixteen cores read as 32, and the driver's greeting had already been trimmed
  out of the syslog while the driver was plainly running.
- 2026-09-19: exercised three displays without having three, using the
  accelerant's own `force_connected` setting - it copies the real monitor's
  EDID onto a connector nothing is plugged into, which is what it is there for.
  Forcing DP-0 gave two heads and a 2560x1080 desktop; forcing DP-0 and DP-2
  gave three heads and 3200x1080, and app_server chose it. Everything built
  this week works across a spanning frame buffer: the accelerant publishes it
  at the wider pitch (12800 bytes a row), OpenGL presents into it at 1269
  frames a second, vertical sync holds 60.3, the retrace semaphore stays at
  60.0 with nothing timing out, and 2D is unchanged. The monitor shows the
  leftmost 1920 of the desktop with the Deskbar off its right edge, which is
  what a spanning desktop wider than one screen should look like.
  What this does not prove: a forced connector takes its mode from the copied
  EDID and settled on 640 wide rather than 1920, so the desktop was 3200 rather
  than 5760. Three heads really are driving one frame buffer, but three real
  1920x1080 monitors will be three times the pixels of one, and at that size
  the 2D figures above start to matter.
  The setting has been cleared again; the machine is back on one display.
- 2026-09-18: all five XHCI controllers - two AMD, one ASMedia, and the
  Thunderbolt 4 host with its USB4 interface - start and publish a root hub.
  Only the KVM is plugged in, so what is left in this area needs someone at the
  machine: devices in each physical port, on both the chipset and the
  Thunderbolt controller.
- 2026-09-19: Bluetooth works on the TP-Link Archer TX55E, from a cold boot
  with nothing done by hand: the adapter reports `90:74:ae:33:d7:cb` and an
  inquiry finds a device nearby. Four separate faults stood between the card
  and a working adapter, and each hid the next.

  The radio is a MediaTek MT7922 (USB `13d3:3610`), which comes out of reset
  running a bootloader. It answers reads of its own registers and its own
  download protocol and nothing else - send it the HCI Reset every Bluetooth
  stack opens with and it takes the packet and stays silent. That was measured
  rather than guessed: `btraw` sends a command straight to the radio over the
  USB raw interface, and the radio accepted it and never replied, while
  `btchip` read chip id `0x7922` and revision `0x8a10` from the bootloader's
  registers on the same device. `h2generic` now reads MediaTek's firmware
  image from `data/firmware/h2generic` and feeds it over in 250 byte pieces
  wrapped in the vendor's WMT protocol, at device open rather than at probe
  because the disk is not mounted that early. A radio that already holds a
  section says so, so a second open costs 120 ms against the first 21 seconds.

  With the radio awake, the stack still saw nothing. `h2generic` scanned every
  interface for its endpoints and kept the last of each kind it found: this
  radio carries a third interface for isochronous audio whose endpoints are
  interrupt in and out, so the driver was listening on MediaTek's audio
  interface. Commands went out correctly, the radio answered correctly, and
  the reply sat in an endpoint nobody read - proved by sending a command
  through the driver and then finding its answer still waiting in the endpoint
  with `btraw`. Events and ACL data are spoken on interface zero only.

  The driver also stood isochronous transfers on the SCO endpoints at every
  open, on an alternate setting it selects at probe that reserves bandwidth
  every frame. Nothing asks for voice until a connection wants it, and the
  driver had carried a note asking for exactly this for years. Removing it
  took the idle receive traffic from 113 completions in five seconds to none.

  Last, `bluetooth_server` never answered a request for a command the
  controller refuses. A controller that will not run a command says so with a
  Command Status carrying an error and then says nothing more; the server only
  looked for a request waiting on the same event it had just received, so a
  request waiting on Command Complete stayed in the queue for ever. This
  controller refuses Read Stored Link Key, which the kit asks for while
  opening an adapter, so the first program to ask for one hung. Any controller
  may refuse any command, so the refusal is now matched against whoever is
  waiting and handed to them as an error.

  Nothing started the server either: the kit reaches it by signature, and a
  BMessenger finds a running program rather than starting one. It is launched
  once the volumes are mounted. Not on demand - that has the launch daemon
  hold the port and wait for the server to adopt it, which this server does
  not do, so every message to it sat unread.

  Remote name lookup still fails, so discovered devices show an address and no
  name. Pairing and the audio profiles are untried.

  The Wi-Fi half of the same card is a MediaTek MT7922 (`14c3:7922`). Nothing
  claims it and Haiku has no MediaTek wireless driver. A driver does exist
  elsewhere, which an earlier note here denied: FreeBSD carries `mt76` in
  CURRENT, ported from Linux, and MT7922 passes traffic there. It is not a
  driver Haiku can host, though - it is built on LinuxKPI and `linuxkpi_wlan`,
  which emulate Linux's mac80211, where Haiku's wireless support is FreeBSD
  12.0's own net80211 (`__FreeBSD_version 1200086`) with no LinuxKPI at all.
  The missing layer is the hard part rather than the driver: page and
  page-pool work in LinuxKPI is what still blocks FreeBSD's own mt76, which
  is not production ready there as of early 2026. An Intel AX200 or AX210
  card is driven by the `iaxwifi200` that Haiku already ships.
- 2026-09-19: the Wi-Fi half of the Archer TX55E can be reached and driven,
  which an earlier note here said was not the case. Two claims in that note
  were wrong and are corrected above: FreeBSD does carry an `mt76` driver, in
  CURRENT, and the MT7922 passes traffic there; and nothing about this card
  makes it unreachable.

  What is true is narrower. That driver is built on LinuxKPI and
  `linuxkpi_wlan`, which emulate Linux's mac80211, where Haiku's wireless
  support is FreeBSD 12.0's own net80211 (`__FreeBSD_version 1200086`) with no
  LinuxKPI at all, so it is not a driver Haiku can host. Supplying that layer
  is harder than the driver, and it is what still blocks FreeBSD's own mt76.

  Asked directly, the hardware is cooperative. `wifiprobe` reaches it through
  the poke driver, which hands userland PCI configuration and a mapping of any
  physical address, so this needed no kernel code and a wrong guess cost a
  rerun. The card sits at 15:0:0 with memory space and bus mastering switched
  off, which is how firmware leaves a device nobody wants. Its register window
  can reach above four gigabytes, so it is written as two registers and
  remembered as two slots with the high half in the one after the low: take
  only the first and the address looks plausible, points into memory instead
  of at the card, and reads back as zeroes. A sleeping card reads as zeroes
  too, which made those two easy to confuse - and reading the window before
  waking the card does not return at all, and takes the machine with it.

  Woken properly - the register domain is the firmware's until the host asks
  for it, through a register reachable without the remapping window - the part
  names itself: chip id `0x7922`, revision `0x8a10`. That is the same revision
  the Bluetooth function reports, which is the two halves of one die agreeing.
  Its subsystem can then be driven through its own reset and comes back saying
  it has started, and Bluetooth still works afterwards, so the two functions do
  not share a power domain in any way that matters.

  That is four of the six steps to a running firmware: PCI setup, the
  ownership handshake, the chip id, and the subsystem reset. The two left are
  six WFDMA rings and an MCU command layer, and then firmware download over
  them - `WIFI_MT7922_patch_mcu_1_1_hdr.bin` and `WIFI_RAM_CODE_MT7922_1.bin`,
  the patch in big-endian and the RAM image little-endian with its table at the
  end, in 4096 byte pieces, with the encrypted-section handling this
  generation requires. There is no simpler register path: every MCU command,
  including the first, goes over the rings.

  A running firmware is still not a working network card. After it comes the
  802.11 driver itself on net80211 - scanning, association, keys, and a data
  path - which is the larger half of the work and the part with no reference
  to check against on this system.
- 2026-09-19: `mt7922` is now a driver rather than a bench tool. It does in
  the kernel, at boot, what `wifiprobe` established by hand: finds the card,
  switches on what firmware left off, maps the window at its real address,
  takes the registers from the card's firmware, reads back MT7922 revision
  `0x10`, and resets the Wi-Fi subsystem. Bluetooth is unaffected by it.

  It publishes as `/dev/mt7922/0` rather than under `net/`. Published under
  `net/`, Haiku's stack took it for a working interface immediately and the
  DHCP client began broadcasting on a card with no data path - which is a
  useful reminder that a device node is a claim about what works.

  Still to come before the firmware runs: six WFDMA rings, an MCU command
  layer, and the download over them. Then the 802.11 driver itself.
- 2026-09-19: the workstation stopped responding and needs someone at it. The
  NanoKVM is healthy - it answers, and reports HDMI enabled and the power LED
  lit - but the machine has no video, no network, and does not respond to an
  eight second hold of the power button, which is what normally forces any
  board off. That combination means the power has to be cut at the supply.

  It went down between a power cycle and the boot after it, before the ring
  code below had run once, so nothing points at that code. What it does
  follow is a long run of hard power cycles, two of which have already
  corrupted files on this machine - `poke.h` came back as binary rubbish
  earlier today, and the syslog came back empty from another. A filesystem
  check is worth doing before trusting what is on the disk.

  The ring setup is committed unrun. It is complete enough to resume from and
  has never met the hardware; the commit says so.
- 2026-09-19: scanning works through `net80211`. `mt7922wifi` wraps the part in
  Haiku's FreeBSD wireless stack, and `ifconfig <dev> scan` lists what is in
  earshot by name, channel and signal. Getting there meant the part will not
  hand over a single beacon until it is asked to sweep the band itself, which
  looks exactly like a receive filter problem and is not; and that
  `ieee80211_input_all` wants the signal counted upwards from the noise floor
  in half decibels, not as a negative dBm, or every network fails the stack's
  minimum and it rescans for ever without saying why.

  Joining one stops the machine. Not the network - the machine: the Deskbar
  clock stays on the same minute across seventy-five seconds, so what is on
  screen is a stale frame. The keyboard interrupt is not serviced either,
  since the debugger's own key combination does nothing, which puts it at a
  CPU spinning with interrupts off.

  Every way of seeing it has been tried and none survives: the network is
  gone; the syslog is never flushed and a power cycle loses it; a script on
  the desktop that copies the log somewhere safe, started from the keyboard,
  works while the machine is well and does nothing when it is not; a watchdog
  in the driver that panics into the debugger never runs, because the thread
  behind it is stopped too. The serial console is the remaining channel.

  The one line that starts a join is commented in `ic_scan_start` with this
  written beside it: every scan inherits `IEEE80211_SCAN_NOJOIN` from whoever
  asked last, and everything else on the system asks for scans that must not
  join, so the stack is told never to join and quietly scans for ever.

- 2026-09-19: the MT7922's Wi-Fi firmware runs. The patch goes in, all five
  downloadable regions of the RAM image follow, the sixth is skipped as it
  asks to be, and the part reports that what it was given is running - from a
  cold boot, with Bluetooth unaffected and all sixteen checks still passing.

  Two of the three things in the way were found by reading what had been
  written against the protocol while the machine was unreachable, and both
  were needed before a single command could be sent: a second claim of
  ownership, made through the moveable window once the rings exist and quite
  distinct from the one that wakes the registers, and saying which mode the
  firmware should come up in, which has to be said before it is sent.

  The third was found on the hardware and is the one worth remembering.
  Commands were being taken and nothing ever came back: the part's processor
  restarted on command and said so, so sending was demonstrably working,
  while every answer went nowhere. What was missing was the prefetch
  configuration - each ring gets a slice of the engine's own buffer to read
  ahead into, and a ring without one has nowhere to stage what it fetches. On
  this engine that stops receiving and leaves sending looking healthy, which
  is as misleading a symptom as this part has produced so far.
- 2026-09-19: the Wi-Fi card scans and lists networks. Twenty-two in earshot,
  by name, address and channel, from a cold boot with nothing done by hand.
  The radio transmits as well as listens: asked after a network by name, it
  was answered twenty-five times, and an answer addressed to us can only
  follow a question we sent.

  Three things were in the way after the firmware came up, and the last was
  the longest-lived. The receiver's maximum frame length is kept in two
  registers, both left at zero by the firmware, so every frame was
  over-length and discarded before anyone saw it; neither register is
  reachable through the moveable window, so both needed entries in the table
  of fixed mappings, and the writes are read back because a wrong entry there
  writes elsewhere in silence.

  Then ordinary traffic arrived and no announcements at all, from radios that
  plainly must send both. The cause was here rather than in the part:
  announcements do not come up the ring ordinary traffic uses, they come by
  way of the part's own processor, and the code waiting there for answers to
  commands read everything, kept what carried the sequence number it wanted
  and discarded the rest - which was exactly the frames being looked for.
  Ruled out first, each by measurement: the receive filter, cleared outright;
  the routing register, which says the host; the transmit and receive gate,
  already open; and a passive sweep being too brief.

  The counting that found it is worth keeping, and one part of it had to be
  fixed first: frames were bucketed by the top half of their control byte,
  which makes a QoS data frame look like an announcement.

  What remains is joining a network, and the plan is not to do most of it
  here. Haiku's own wireless drivers present themselves through `net80211`,
  which is what `SIOCS80211` reaches and what `wpa_supplicant` drives; the
  handshake and key management already exist there. The work is to wrap this
  driver in that shape, not to reimplement them.
