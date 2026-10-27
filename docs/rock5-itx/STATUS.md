# Tested status

Updated 2026-09-25 (Australia/Hobart). This page distinguishes lab readiness from native Haiku
support. No hardware row in the roadmap is accepted merely because Linux or
firmware supports it.

The owner has expanded the scope to a full Rock 5 ITX installation and the
remaining CPU, network, storage, GPU, media, audio and AX210 work. The regular
ARM64 [full image](FULL-BUILD.md) is installed and boots from NVMe. CPU,
onboard Ethernet, NVMe TRIM/MSI-X and installed 3D application results are
recorded at the end of this page. Media and analog audio playback are now
qualified. The [AX210 status](AX210.md) records native Wi-Fi association,
HE capability scans, Bluetooth firmware loading, Classic inquiry and LE
advertisement discovery. The AX210 now joins the 5 GHz Gaspers network at
802.11ax, obtains DHCP, reports a two-stream HE rate, and carries gateway and
internet traffic with Ethernet down. The current HE rate configuration uses
20 MHz; wider-channel parity with the 40 MHz Linux reference and other Wi-Fi 6
modes remain open. Bluetooth LE connection initiation and cancellation pass on
the native AX210. A live MX Master 2S reached an LE connection during pairing,
but the SMP exchange stalled; encryption, bonding, and mouse input are not yet
verified.
The [Wi-Fi and Bluetooth preferences update](AX210.md#wi-fi-and-bluetooth-preferences-2026-09-24)
adds a dedicated WiFi panel and Deskbar applet, groups mesh access points by
SSID and security type, and makes Bluetooth scan results usable when a Classic
peer does not provide a name. The
Bluetooth inquiry panel now keeps a bounded LE scan running if Classic inquiry
fails. The revised app is installed and a native scan returned LE devices;
the Classic-failure branch still needs a live failure test. An MX Master 2S
appeared by name as an LE mouse; its pairing remains unresolved. The
new Wi-Fi UI is installed on the board. Its panel joined Gaspers with its
password field, obtained DHCP, and carried
gateway and internet traffic with Ethernet down. The separate applet reported
SSID and strength and survived a reboot. The later Network preferences
revision fixes selection from its grouped WiFi menu. The dedicated panel
clears an entered password when switching networks; both revisions are
installed and exercised natively. The WiFi panel now offers a default-on
remember option: a native join saved Gaspers by SSID in a mode-`0600` settings
file, and a password-free manual join reused it after reboot. A separate
short-lived `wifiautojoin` helper now joins a saved network at boot without
running a scan on Deskbar's UI thread. After an orderly Rock 5 reboot, the
WiFi panel showed Gaspers connected without a manual join, and `ifconfig`
reported DHCP address `192.168.1.156`. The earlier applet scanning experiment
was rolled back to the working applet. The applet now
routes unsaved protected networks to the dedicated panel with
the SSID selected, while a saved Gaspers entry joins directly. Its latest
installed revision returned automatically after an orderly board reboot and
showed the grouped menu. ARM64 and x86_64 targeted builds pass; x86_64 native
UI use remains open. The applet now offers a Disconnect action while
associated. On the board it left Gaspers, then a selection from the grouped
menu rejoined and obtained `192.168.1.153` by DHCP. The native **Scan again**
trial then exposed an OpenBSD WLAN adapter issue: reading scan results
implicitly began a background scan, and the explicit scan request did not
start one. A revised `iaxwifi200` override makes result reads passive and
starts a scan only on request, without selecting a new mesh BSS on scan
completion. ARM64/x86_64 targeted builds and the full ARM64 QEMU boot passed.
The installed driver survived an NVMe reboot; applet rescan and native
`ifconfig scan` completed while Gaspers remained associated with DHCP address
`192.168.1.153`. The installed WiFi panel now requests one scan when opened:
a native reopen replaced a stale single-network cache entry with grouped
Gaspers and Gaspers_ioT within 12 seconds. The latest panel hides stale
network rows while a scan is in progress and enables Connect only after fresh
results arrive. Native opening and manual rescan both returned the grouped,
secured list while Gaspers stayed connected. The panel also
now refreshes adapter discovery
while open, clears a vanished adapter's stale networks, and retained a
selected Gaspers entry through a native periodic refresh. Adapter hotplug
itself remains untested. The current WiFi panel saves a newly entered
password only after the selected network associates and reports link, since
`JoinNetwork()` initially reports only that the request was submitted. An
empty password is accepted for an already saved network; pending joins have a
60-second timeout. ARM64/x86_64 builds and the full ARM64 QEMU boot pass. On
the board, an empty-password attempt for unsaved Gaspers_ioT was rejected;
an invalid password reached the existing retry dialog and then timed out.
Gaspers_ioT was absent from the saved-network file before and after that
trial. Selecting saved Gaspers in WiFiStatus restored WPA2 association and
DHCP address `192.168.1.153`. A successful first-time save with the current
panel remains to be verified. Both WiFi surfaces now require link before
showing an association as connected. A native cache regression had replaced
the panel's grouped list with one falsely **Open** Gaspers row on a periodic
refresh; it now retains the last completed scan until the next requested
scan. The active association supplies security details for its matching
BSSID. ARM64/x86_64 builds and the full ARM64 QEMU boot passed, and the
installed panel kept Gaspers and Gaspers_ioT as secured rows through more
than seven refresh ticks. Another native trial showed a stale NetServer scan
cache could turn a secured Gaspers_ioT join into an **Open – no encryption**
dialog. The latest panel sends the selected security mode with new join
requests; ARM64/x86_64 builds and the full ARM64 QEMU boot passed. An
invalid-password retry now displayed **WPA/WPA2**. Gaspers_ioT remained
absent from saved networks, and rejoining saved Gaspers restored an encrypted
link, DHCP address `192.168.1.154`, and two successful gateway pings. A
native Disconnect/Connect cycle in the latest panel then reused saved Gaspers
without entering a password and passed two more gateway pings. A further
native trial supplied the known valid Gaspers password with Remember enabled:
the panel authenticated, the mode-`0600` saved-network file was rewritten,
its content matched a private recovery copy, and two gateway pings passed.
The temporary credential copies were removed. A later first-time save trial
removed the saved Gaspers entry through NetServer after backup: the helper
reported zero saved networks, the panel required a password, and a valid
Remember join recreated the entry with mode `0600` and byte-for-byte content
matching the backup. Gateway pings and an applet reconnect using that newly
saved entry passed. The temporary helper and copies were removed. Bluetooth
preferences scans Classic and LE
advertisements, exposes a Pair action for a
selected connectable LE peer, and reports pairing errors by stage. A native
scan of the latest installed panel kept Pair enabled for a connectable peer
and disabled it with a pairing-mode hint for a peer seen only in
nonconnectable reports. The installed stack
includes legacy Just Works SMP, controller encryption, a private reusable bond
store, encrypted reconnection, ATT notification handling, HID service discovery
and a report-map mouse decoder. An input server add-on for paired HID mice is
installed and loaded on the Rock 5. The main preferences window now has a
separate saved LE mouse list and a clearer Scan for devices action. The
installed panel showed the empty list and opened the scan window natively;
the populated list awaits a paired mouse.
The LE scan now uses advertised Mouse Appearance and HID service hints to
identify otherwise unnamed candidates. The parser and both architecture
builds pass, and the latest board app completed a native scan; no observed
peer advertised mouse or HID fields. The Classic Headphones address has a
Cambridge Silicon Radio prefix in the local IEEE assignment dataset, which
does not identify the particular product.
The installed Classic scan now recovers its controls after an inquiry error,
and Classic Connect uses a cached name or address without a blocking name
request. The no-controller scan window closed safely on the board. After a
service-only restart, two scans found the same Classic address but no LE
peers. A direct probe confirmed that this restart accepts LE scan requests
but never reports scan start, even after a 30-second wait. The updated panel
now times out the missing scan-start notice and reports an error; ARM64 and
x86_64 builds pass, and the installed app exercised both success and failure
paths natively. Running the AX210 boot helper restored LE scanning without a
reboot (46 reports in six seconds). The preferences Start menu now uses an
optional portable startup hook; the Rock 5 hook runs the AX210 helper. A menu
Stop/Start then produced 47 LE reports in six seconds, and the preferences
scan again listed Classic and LE devices. A raw server-only restart still
needs controller initialization; its specific failure cause remains open.
The installed Bluetooth preferences now offers a confirmed Forget action for
a selected saved LE mouse. ARM64/x86_64 builds and the full ARM64 QEMU check
pass; on the board the empty-list action is disabled and a new Classic/LE scan
completed. The populated-list Forget path awaits an actual paired mouse.
Bonded mouse reconnection now resolves rotating LE addresses from the saved
identity key before connecting; the published Bluetooth `ah` vector passes
on the host and natively, and the revised add-on is loaded after reboot.
The ATT and SMP socket waits now use `poll()` so the mouse path does not depend
on the process's descriptor count. High-descriptor scripted cases pass on the
host; native protocol cases pass, and both Bluetooth services load the revised
library after reboot.
The LE mouse input add-on now releases held buttons if its notification stream
ends during a disconnect. ARM64/x86_64 targeted builds and the full ARM64
EL2/xHCI QEMU boot pass; the hash-verified add-on is installed on the Rock 5
with a restore copy. A synced restart returned to the desktop, and
`listimage` showed the new nonpackaged add-on loaded by `input_server`. Gaspers
did not auto-join at that boot; the installed WiFi panel rejoined its saved
entry without a password and showed 65% signal. A later `wifiautojoin`
installation and reboot verified automatic Gaspers association. A live mouse is still needed to
exercise the button-release path.
Scripted protocol and decoder tests pass on the host and Rock 5; the full
ARM64 image build and QEMU boot also pass. With the user's mouse in pairing
mode, a native scan identified **MX Master 2S** and the pairing worker reached
the SMP exchange after the LE connection notice. It then remained waiting for
minutes, with no bond or pointer event. A diagnostic preferences app and
library showing finer SMP progress are installed for the next live attempt;
pairing, reconnection, and pointer events remain unverified.
The stalled exchange also left the Bluetooth server unable to answer a new
preferences launch. An orderly reboot restored the desktop, preferences, and
native Classic/LE scans; the mouse did not appear in those later scans.
The WiFi and Bluetooth applications, supporting libraries and LE mouse input
add-on also pass a targeted x86_64 cross-build from this fork
(`artifacts/wifi-bt-x86-build.json`). An x86_64 runtime test and full image
build remain open.
See the [AX210 status](AX210.md#le-hid-mouse-bridge-and-native-installation)
for the exact image, installed paths, backups and evidence.

The system-default OpenGL candidate ([MESA-SYSTEM.md](MESA-SYSTEM.md), Mesa
at `3139063445` on the qualified +256 image) runs GLTeapot on Mali with no
launch environment on two native boots: the Mesa backend takes the Mali
device and its firmware from installed defaults, the CPU polygon path is on
by default, and the GLVND/Mesa libraries and EGL vendor file are installed
system-wide. Every earlier Mesa fixture passes on the same boots and the
emulator gates show the software fallback without a device. It is
qualified (2026-09-19, `…automated-mali-system-opengl/20260919T124336Z-d49da9`)
with the independent Linux eMMC readbacks now run from the owner's SD-card
Debian, the lab's new recovery OS.

The +320 USB image runs [2D and 3D together on both connectors](DISPLAY.md):
one 3840x1080 app_server desktop across HDMI1 and the DisplayPort-bridged
port, a hardware cursor window on each port, and the qualified system-default
Mesa on the Mali-G610. On two native boots GLTeapot, started with no
environment, renders through the installed system OpenGL at 59 FPS on the
desktop, and the NanoKVM captures it on HDMI1's half.

The +315 USB image completes the [dual-display desktop](DISPLAY.md): one
3840x1080 app_server desktop across both HDMI ports, with a hardware cursor
window on each port. A pointer on the seam shows half on each screen. On
both native boots the NanoKVM on the DisplayPort-bridged port captures the
right half with the probe's cursor on it. The owner confirmed the left half
on a monitor attached to HDMI1.

The +313 USB image gives app_server [one desktop spanning both HDMI
ports](DISPLAY.md): a 3840x1080 frame buffer whose left half HDMI1 scans
(its port raised to 1080p by the driver's mode set) and whose right half
the DisplayPort-bridged port scans. On both native boots the NanoKVM on the
second port captures the desktop's right half, with the Deskbar and half of
the centred pointer. HDMI1's half is checked by register read-back because
no display is attached to it. The hardware cursor across both ports and
mode changes on the spanning desktop come next.

The +310 USB image runs [app_server on the second connector](DISPLAY.md)
at 1920x1080: with the dp-desktop profile the driver trains DP1, scans its
own frame buffer with a window on video port 1, moves the kernel console
there and drives app_server's retrace from that port at 60 Hz. On both
native boots the NanoKVM captures the full Haiku desktop from the
DisplayPort-bridged HDMI port. The hardware cursor, mode changes and one
desktop spanning both ports come next.

The +308 USB image shows [Haiku's desktop on the second connector](DISPLAY.md):
after training DP1 and starting video port 1, a second VOP2 window
(ESMART0) on that port scans the desktop's frame buffer. On both native
boots the NanoKVM captures Tracker, the Deskbar and the pointer on the
DisplayPort-bridged HDMI port. The desktop is still the firmware's 640x480
buffer, shown over the port's 1080p background. app_server driving that
port at its own mode, and one wide desktop spanning both ports, come next.

The +306 USB image puts the [first picture on the second connector](DISPLAY.md):
Haiku brings DisplayPort TX1 and USBDP PHY1 up itself, reads the sink's
DPCD and EDID over AUX through the RA620 bridge, and trains the link at
5.4 Gb/s over two lanes on the first attempt. It then drives video port 1
at 1920x1080@60 into DP1. On both native boots the NanoKVM captures the
port's magenta background from that connector, and HDMI1's port is left
untouched. A frame buffer window on that port and a second app_server
screen come next.

The +300 USB image adds a read-only [observation of the second connector's
DisplayPort path](DISPLAY.md) to the qualified desktop-cursor cycle: on
two native boots the DisplayPort TX1 block is powered, clocked and idle
with its PHY interface parked, the USBDP PHY1 is in low power, and the
hot-plug pin is a plain input reading low with nothing on the connector;
every word agreed between samples and boots. Bringing that path up (PHY,
link training through the RA620 bridge, a second video port) needs a
monitor or an HDMI dummy plug on the second connector, which is not
attached.

The +297 USB image qualifies [app_server's pointer on the hardware cursor](DISPLAY.md)
on two native boots: on the desktop cursor profile the accelerant exports
the cursor hooks, app_server hands its 22x22 pointer to the driver's second
VOP2 window at start and stops drawing it into the frame buffer, the window
reads back at the desktop's centre, survives the 720p/1080p mode changes
and DPMS, and the NanoKVM captures show Haiku's hand pointer there; the
probe's own placements, clipping, hide and restore of app_server's bitmap
pass on the same boots. The HDMI1 "2D" set (EDID, own frame buffer,
retrace, mode changes, DPMS, hardware cursor) is complete; drawing itself
is not accelerated, and the second (DisplayPort-bridged) HDMI port and a
spanning desktop remain open.

The +294 USB image qualifies a [hardware cursor on HDMI1](DISPLAY.md) on
two native boots: on the probe-only cursor profile the driver programs a
second VOP2 window (ESMART3, blended by the port's alpha mixer) with a
64x64 straight-alpha bitmap over the desktop window, clips it at every
edge, hides it and restores app_server's state, with every window register
read back and the NanoKVM captures showing the white, black, transparent
and half-transparent quadrants where expected and the desktop untouched.
The cause of an earlier dark frame is recorded: the firmware runs the
desktop window with the AXI read ids Linux assigns to ESMART3, so the cursor
window takes ids derived from the desktop window's. app_server still draws
its software pointer on this image; handing its pointer to the window (the
desktop cursor profile) is the next stage, and the second
(DisplayPort-bridged) HDMI port remains open.

The +285 USB image qualifies [DPMS power control on HDMI1](DISPLAY.md) on
two native boots: through the accelerant's DPMS hooks the driver stops the
video port and powers the HDPTX PHY down, so the sink loses its signal, the
frame-start interrupts stop and the NanoKVM has no frame to capture, and
brings the current mode back with the full mode set in under a millisecond,
with retrace at 60 Hz again and the desktop restored. The 720p/1080p mode
changes and the earlier read-only, EDID, scanout-swap, accelerant and
retrace checks pass on the same boots, and app_server's start-up DPMS-on
request is answered as a no-op. A cursor window and the second
(DisplayPort-bridged) HDMI port remain open.

The +282 USB image qualifies [native mode changes on HDMI1](DISPLAY.md) on
two native boots: with the opt-in mode-set profile the driver switches the
port app_server draws to from the firmware's 1920x1080@60 to 1280x720@60 and
back, reprogramming the HDPTX PHY PLL and lanes, the VOP2 port timing and
window, and the AVI infoframe, while the accelerant reports each mode and
its retrace keeps running at 60 Hz. The NanoKVM shows the 720p signal as the
desktop's top-left crop and the normal desktop after the return; normal
reboot, verified shutdown and recovery pass, and the earlier read-only,
EDID, scanout-swap, accelerant and retrace checks pass on the same boots.
A cursor window and the second (DisplayPort-bridged) HDMI port remained
open.

The +277 USB image qualifies the [board accelerant with vertical
retrace](DISPLAY.md) on two native boots: app_server runs on
`rk3588_display.accelerant`, draws the desktop into a driver-owned contiguous
frame buffer that the live HDMI1 window scans at the firmware's 1920x1080
mode, reports the sink's EDID, and its retrace semaphore is released by the
VOP2 frame-start interrupt at 60.0 Hz. Normal reboot, verified shutdown and
recovery pass; the earlier read-only, EDID and scanout-swap checks pass on
the same boots. A cursor window, power control, a real mode change and the
second (DisplayPort-bridged) HDMI port remain open.

The +269 USB image qualifies the first [VOP2 write path](DISPLAY.md): with
the opt-in scanout profile the driver points the live HDMI1 window at its own
contiguous colour-bar buffer, commits the port and waits for the
configuration-done bit, and restores the firmware framebuffer, on two native
boots. The NanoKVM capture shows the bars during the 30 s hold and the normal
desktop afterwards, the observation is unchanged across the swap, and normal
reboot, verified shutdown and recovery pass. The first candidate (+267)
verified the address before the port's next frame start and is retained.

The +264 USB image qualifies the [EDID read](DISPLAY.md) over the HDMI TX1
I2C master on two native boots: with the opt-in profile the driver reads the
NanoKVM's 256-byte EDID (VCS `0x1145`, EDID 1.3 with one CEA-861 extension,
preferred 1920x1080 at 148.5 MHz) one byte per transfer in about 52 ms per
block, and the read-only observation before and after the read is identical.
Both QEMU modes, the Mali regressions, normal reboot, verified shutdown and
automatic recovery pass; the desktop was viewed on both boots. Native mode
setting and the second (DisplayPort-bridged) HDMI port remain open.

The +263 USB image qualifies the first [native display observation](DISPLAY.md)
on two native boots: the read-only `rk3588_display` driver admits the VOP2,
HDMI TX1, HDPTX PHY1 and control-block description and reads the firmware
display state without a register write. The firmware drives the HDMI1 port
from VOP2 video port 2 at 1920x1080 (2200x1125 total) through ESMART2 at
`0xed280000`; the HDMI1 hot-plug level, PHY lock and TMDS link are recorded.
Both QEMU modes, the earlier Mali regressions, normal reboot, verified shutdown
and automatic recovery pass. A first +259 run panicked reading a write-only
HDMI register and is retained.

The +256 Mesa fix, on the retained +254 Haiku kernel, qualifies the bounded
[GLTeapot and polygon tests](MESA-APPLICATION.md) on two native boots. All 128
polygon frames pass, including complete immediate-mode boundaries, points and
quad culling. Four normal GLTeapot launches produce 32 reviewed frames and
8,732 completed GPU submissions with the correct quad wireframe. The earlier
GPU suite, both QEMU modes, normal recovery and independent integrity checks
pass. The private renderer still uses opt-in CPU geometry with Mali rasterization;
broader application compatibility, system integration and native display control
remain open.

The fix clears the CPU interpreter's cached binding before freeing shader
tokens. A deterministic address-reuse test fails four checks before the fix,
then passes all ten under ASan/UBSan and on each native boot. The +255 diagnostic
established that new shader inputs were running against old instructions.
Earlier failed candidates and their original evidence remain preserved.

The +243 USB image qualifies [reset notification for live Mesa contexts](MESA-LOSS.md)
on two native boots. Both shared contexts receive one loss notification, ignore
further rendering and close completely; fresh contexts then render without a
Haiku reboot. All 181,972 before/fresh pixels, 3,584 guards and 104,496 ignored
readback bytes pass. Both QEMU modes, earlier regressions, normal recovery and
independent integrity checks pass. The earlier failed trials are preserved.
General application compatibility, arbitrary hangs and native display control
remain open.

The +238 USB image qualifies [bounded GPU command-fault recovery](MESA-RECOVERY.md)
on two native boots. Three affected jobs receive errors per boot; reset,
address-space cleanup and platform restoration are verified. After all affected
clients close, fresh native queues and Mesa contexts work without a Haiku
reboot. All 155,976 subsequent pixels and 3,072 guards pass, together with
earlier regressions, both QEMU modes, normal recovery and independent integrity
checks. The separate +243 fixture above qualifies retained Mesa-context
notification; arbitrary hangs and native display control remain open.

The +236 USB image qualifies [termination during pending graphics work](MESA-PENDING.md)
on two native boots. Both a fence timeout and the driver's actual close-time
queue state establish unfinished work. Survivor rendering, fresh-process reuse
and allocation cleanup pass; all fourteen completed frames, 90,986 pixels and
1,792 guards match. Earlier graphics regressions, both QEMU modes, normal
recovery and independent integrity checks pass. Active work can complete while
close waits; immediate preemption and GPU fault/reset recovery remain open.

The +234 USB image qualifies [concurrent graphics applications](MESA-CONCURRENCY.md)
on two native boots. All 128 paired draw rounds overlap; all 260 frames,
1,689,740 pixels, 33,280 guards and 532 submissions pass. Normal process
retirement, survivor rendering, fresh-process reuse and allocation cleanup
pass. Both QEMU modes, earlier graphics regressions, normal recovery and
independent integrity checks pass. Each graphics fixture logs to RAM before
its checked transfer. Pending-work termination, GPU fault/reset recovery and
native display control remain open.

The +232 USB image qualifies [sustained GPU rendering](MESA-SUSTAINED.md)
on two native boots. Four retained contexts each complete at least sixty
seconds of measured rendering with stable tiler-heap use. All 91,648 frames,
595,620,352 pixels, 11,730,944 guard bytes and 183,304 submissions pass; fixed
heaps complete 147,196 incremental passes. Earlier graphics fixtures, both
QEMU modes, normal reboot/shutdown, recovery and independent integrity checks
pass. Full logs are captured in RAM and retrieved with checked, paced transfers.
Pending-work termination, GPU fault/reset recovery and native display control
remain open.

The +227 USB image qualifies [fixed tiler heaps and incremental rendering](MESA-HEAP-LIMIT.md)
on two native boots. Mesa completes 36 incremental passes after 36 requests
for more heap memory are refused. All 51,992 pixels, 1,024 guards, 24 submissions
and allocation baselines pass. Earlier graphics tests, both QEMU modes, normal
reboot/shutdown, recovery and independent integrity checks pass. The OpenGL Kit
fixture now waits for completed window updates before screen capture. GPU
fault/reset recovery and native display control remain open.

The +224 USB image qualifies [firmware tiler heap growth](MESA-HEAP-PRESSURE.md)
on two native boots. Four contexts each grow from one chunk to four, then
seven; all 24 firmware requests receive memory. All 51,992 pixels, 1,024 guards,
24 submissions and allocation baselines pass. Previous graphics regressions,
both QEMU modes, normal reboot/shutdown, recovery and independent integrity
checks pass. GPU fault/reset recovery remains open.

The +221 USB image qualifies [graphics-process cleanup](MESA-LIFETIME.md)
on two native boots. A process is terminated after completed rendering while
its resources remain open; the survivor renders correctly and a fresh context
works afterward. All 51,992 pixels, 1,024 guards and allocation baselines pass.
Earlier GPU/window fixtures, both QEMU modes, normal reboot/shutdown, recovery
and independent integrity checks pass. Termination during pending graphics
work and GPU fault/reset recovery remain open.

The +219 USB image qualifies [six GLES pipeline operations](MESA-PIPELINE.md)
on two native boots: texture upload/sampling, depth, stencil, blending, scissor
and render-to-texture. All 155,976 pixels, 3,072 guards and sixty GPU submissions
pass, with allocations restored after each context. Both QEMU modes, earlier
GPU/window fixtures, normal reboot/shutdown, recovery and independent storage
integrity pass. Conformance, termination during pending graphics work and
GPU fault recovery remain open.

The +217 USB image qualifies [normal OpenGL Kit rendering](MESA-GLVIEW.md)
on two native boots: two live BGLView instances, desktop OpenGL 3.1, alternating
draws, resizing and context retirement. All 32 frames, 168,960 GL pixels,
168,960 screen pixels and 4,096 guards pass; all 104 submissions complete and
allocation counts return to baseline. Previous GPU regressions, both QEMU modes,
normal reboot/shutdown, recovery and independent integrity checks also pass.
This is a bounded application fixture using private libraries and CPU bitmap
presentation. General compatibility and GPU fault recovery
remain open.

The +215 USB image qualifies [native GPU rendering in EGL windows](MESA-WINDOW.md)
on two boots. All 16 frames, 84,480 bitmap pixels and 84,480 independently
captured screen pixels pass, including resizing and context retirement. All
36 window submissions complete, allocation counts return to baseline, and the
previous offscreen/kernel GPU regressions pass. Both QEMU modes, normal
reboot/shutdown, recovery and independent storage/image integrity pass. The GPU
renders the content and the CPU copies it into Haiku's existing display path.
General OpenGL application compatibility and native display control remain
open. The failed first native attempt and its rectangle correction are retained.

The +210 USB image qualifies [native Mesa/Panfrost rendering](MESA.md) on two
Haiku boots: four full RGBA8 images over two contexts per boot, with all 32,768
pixels matching the Linux reference byte for byte. Mesa completes all 32 GPU
submissions and allocation counts return to baseline. Pbuffer rendering,
EGL termination/reinitialization, previous GPU regressions, both QEMU modes,
normal reboot/shutdown, recovery and independent storage/image integrity pass.
The complete Mesa/GLVND port and reconstruction recipe are saved in this fork.
The failed first attempt and its mutex correction remain recorded. Conformance,
sustained rendering, native display control and GPU fault recovery remain open;
the desktop still uses the EFI framebuffer.

The +209 image qualifies [cached GPU property queries](GPU.md#cached-gpu-properties)
on two native boots: 86 checked queries, including 64 while GPU work is pending,
with identical hardware and firmware values. Invalid requests, foreign/inherited
handles and failed copyout are rejected without changing queue state or allocation
counts. All earlier GPU regressions pass: 1,696 accepted submissions, 1,432 checked
completions and eight application compute shaders. All 147 host checks, the full
ARM64 build, both QEMU modes, both desktops, normal recovery and independent
storage/image integrity pass. These interfaces support the later +210 Mesa port.

The +207 image qualifies [native tiler heaps](GPU.md#native-tiler-heaps) on two
ROCK boots. Each checks 5,159 GPU memory samples across five 2 MiB chunks,
queued heap data across address reuse, HEAP_SET and normal/killed process cleanup.
All earlier buffer, VM, queue, compute and synchronization regressions pass:
1,696 accepted submissions, 1,432 checked completions and eight application
compute shaders across both boots. Driver and kernel allocation counts return
to baseline. All 146 host checks, the full ARM64 build, both QEMU modes, both
desktops, normal recovery and independent storage/image integrity pass. The
failed +206 command encoding and an initial +207 EL2 USB timeout remain recorded;
the corrected native trial and unchanged-image EL2 repeat pass. Mesa adaptation
and rendering are next. The desktop still uses the EFI framebuffer; native
firmware OOM/growth and automatic GPU reset remain unqualified.

The +195 USB image passes all
139 host checks, the ARM64 build, both two-boot QEMU modes and two native boots.
Haiku now passes four compute-shader submissions and four CS memory-store
regressions using its own firmware and application page tables, group/queue
setup, completion objects and interrupts.
Each submission changes eight words and preserves all 2,040 guards; the complete
result matches Linux. The command, ring, completion and queue-interface buffers
are independently checked. Group termination, MCU halt/stop, both address-space
removals and all ten checked platform-register restorations pass. Both desktops,
normal reboot/shutdown, automatic recovery and independent recovery-image/eMMC
integrity pass. The shader's entire 4 KiB command/descriptor allocation matches
the separately qualified Linux reference. Rendering and Mesa integration remain pending.

The earlier +190 firmware startup/ping cycle remains qualified. Its initial +189 attempt
misclassified normal MMU address-space completion and stopped before starting
firmware; its failed result and recovery evidence remain recorded.

The same image passes 129,024 instruction-alias checks on all eight CPUs per
native boot. It corrects a reproduced [ARM64 instruction-cache alias defect](ARM64-ICACHE.md)
found while investigating the earlier +186 startup panic. That exact failed
boot remains recorded; the cache defect was reproduced independently on Linux.
A separate RAM-only Linux 6.18.52 reference passes four checked compute shaders
across two GPU contexts, including fresh completion fences, complete
buffer guards, cleanup, normal recovery and independent storage/image integrity.
Its EL2 QEMU and EL1 Cortex-A76 boot gates pass; an earlier synthetic EL1 `max`
CPU assertion remains recorded in [GPU.md](GPU.md). The first shader attempt
omitted a compute-resource request and timed out; the corrected stream passes,
and the failed trial and its successful recovery remain recorded.
The visible desktop continues to use the EFI framebuffer.

The Linux Mesa 25.3.6/Panfrost rendering reference is now qualified: two contexts
render four full 64-by-64 RGBA8 images, and all 16,384 pixels and 512 readback
guard bytes match. All 22 observed DRM request types return without errors;
creation/destruction counts balance for VMs, groups, heaps, buffers and
synchronization objects. Both QEMU boot modes, normal native reboot/recovery
and independent eMMC/image integrity pass. The initial image's non-executable
ELF interpreter was caught and corrected in QEMU before hardware deployment.
[GPU.md](GPU.md#mesa-rendering-reference) records the results and the Haiku
userspace interface work that remains. This is Linux rendering evidence;
Haiku rendering is still pending.

The +198 image qualifies the first persistent [client/CPU-buffer interface](GPU.md#haiku-userspace-interface-work)
on two native boots. Six buffer sizes pass zeroing, two CPU aliases, three patterns,
fork and a fourth shared pattern on each boot. Mappings survive handle removal
and final descriptor close. Four simultaneous child clients leave 48 buffers and
maps open; two exit normally and two are killed. Driver counters and independently
enumerated kernel buffer-area counts return to their baseline. The existing
command/compute regression, both desktops, normal reboot/shutdown, automatic
recovery and independent eMMC/recovery-image integrity pass. All 141 host checks,
the ARM64 build and both two-boot QEMU modes pass.

The +200 image qualifies [persistent software GPU VMs](GPU.md#persistent-gpu-address-spaces)
on two native boots. Atomic mapping batches, overlap replacement, partial unmaps,
invalid-batch and copyout rollback pass. Mappings retain buffers after handle
removal. Four child teams per boot leave 12 VMs and 48 buffers mapped; two exit
normally and two are killed. Driver counts and independently enumerated kernel
buffer/table areas return to baseline. Host tests also retain immutable generations
across updates and client closure, with a page-level oracle and injected failures.
All 142 host checks, the ARM64 build, both two-boot QEMU modes, prior CPU-buffer and
compute regressions, both desktops, normal reboot/shutdown, automatic recovery
and independent eMMC/recovery-image integrity pass. The later +202 queue runtime
activates these VMs and retains them through completion; +204 adds shared
synchronization. Tiler heaps, Mesa rendering and acceleration remain pending.
The installed SSD's qualification status is unchanged.

A read-only Linux check on 15 September reconfirms both Ethernet links: the
SFP-connected `enP3p49s0` negotiates 2,500 Mbit/s full duplex and `enP4p65s0`
negotiates 1,000 Mbit/s full duplex. Both have carrier and report an operational
state of up. This is a link observation, not a new throughput test. Evidence:
`artifacts/link-observation/20260915T045258Z-b2392c/result.json`.

Latest qualified installed result: `+156` passes two boot, integrity,
network and normal-reboot cycles, with independent Linux eMMC verification.
Its first boot waited 1,577 us for SATA Link Training to clear, then initialized
all four ports. The same fix had passed on USB with a 2,587 us wait.
[SSD-INTEGRATION.md](SSD-INTEGRATION.md) and [PCIE-TRAINING.md](PCIE-TRAINING.md)
preserve the earlier failures and tested scope.

The SSD has since received the `+174` package/configuration update and passed
its before/after integrity checks. Replacing the live USB boot medium during
recovery caused a filesystem panic. Linux recovery and independent integrity
checks passed, but the transition remains failed. The new shutdown-aware
controller passed a separate clean native recovery witness and independent
image/eMMC integrity checks. The first subsequent SSD boot reached the desktop
and passed its preflight, profiler, ASID and process-teardown checks, then
panicked in `VMTranslationMap::PageUnmapped` during concurrent startup-shell
activity and storage verification. Automatic recovery succeeded with no USB
checksum errors; independent Linux integrity passed. Page-aging bookkeeping
has a reproduced host failure and a correction passing 128 host checks;
the +178 EL2 QEMU check passes. Its EL1 repeat stopped during a profiled
fault-recovery probe after reboot, without a kernel panic; full qualification
remains open, and `+156` remains the qualified baseline.
See [ARM64-PAGE-AGING.md](ARM64-PAGE-AGING.md) and
[RECOVERY-MEDIA.md](RECOVERY-MEDIA.md).

| Area | Evidence and state |
| --- | --- |
| Repository | `airos-works/haiku`; all work merged into `master` (2026-10-05); upstream base `b42b90f5ec2839f8074400c849ee025b50d0f7f1` (hrev60206, merged 2026-10-05) |
| Build tools | Haiku GCC 13.3.0 cross-compiler and binutils built successfully; buildtools `8375c2dbeaf109c520798cb234d57f0895463201` |
| ARM64 image and QEMU | Current clean 336 MiB `@minimum-mmc` image passes first login at both EL1 and EL2 with 4 virtual CPUs and 2 GiB RAM; a basic Tracker/Deskbar desktop was inspected during phase 0 |
| ARM64 address-space reuse | The +174 USB image reserves ASID zero for the empty user table and passes 280 live-child private-page checks over four migration rounds on all eight CPUs on each of two boots, deliberate-corruption cleanup and subsequent reuse. All 119 host checks, full two-boot EL1/EL2 QEMU gates, sixteen checked network streams, profiler/storage/desktop checks, normal reboot and automatic recovery with independent Linux readback pass. The host model reproduces the old retained translation; no old native stale read is claimed. See [ARM64-ASID.md](ARM64-ASID.md); the SSD remains +156. |
| ARM64 system profiling | The +165 USB image passes 16 user/kernel/invalid-frame sampling checks across two native boots, plus 192 profiled fork/exec checks, 48 recovered faults and eight newly executed probes with resolved user symbols. Both QEMU modes and 114 host checks pass; desktop, normal reboot, recovery and independent eMMC integrity also pass. The new corrections fix image-notification capacity and unaligned scalar decoding. Frame-pointer limits, per-CPU placement and sustained overhead remain; see [PROFILING.md](PROFILING.md). The SSD remains +156. |
| NanoKVM | PCIe model, application 2.4.3 and base image v1.4.0; staged downloads passed before/after native reboot, but simultaneous USB/Ethernet relay still causes outages; the latest outage did not recover through the hardware watchdog |
| Remote controls | HDMI capture, keyboard, reset, full off/on and controller availability through target power-off tested |
| Virtual storage | Raw USB image verified byte-for-byte from ROOBI; selected image survives reset and target power cycle |
| Automated controls | One hundred and forty-five host checks pass locally, including production Mali binary/timeline synchronization, shared and snapshot descriptor lifetimes, atomic descriptor publication and module release, concurrent publication waits, queue dependencies and cancellation, persistent Mali queue scheduling, independently interpreted context/generation changes and ring wrap, worker/API waits and concurrent close, failure retention, and immutable Mali VM generations, transactional map/unmap rollback, independently decoded scatter page tables, retained buffer lifetimes and quotas, persistent Mali client/buffer ownership, allocation/copyout cleanup, shared CPU mappings, concurrent client teardown and RAM-clone cache attributes, plus the complete Linux-matched compute shader/descriptor allocation, shader request routing, Linux-matched CSF commands, application page-table permissions, queue completion and fault/teardown cases, independently decoded GPU page tables, bounded MCU startup/ping/stop, firmware-interface bounds, allocation/IRQ lifetime and failure retention, mixed-CPU instruction-cache alias synchronization, bounded GPU reset/IRQ delivery and concurrent handler removal, production Mali firmware decoding, FDT resource admission, read-only platform observation and bounded GPU power/identity/restoration, verified system-off/media-switch ordering and failure-preserving session recovery, production ARM64 ASID reservation/recycling and independent cross-CPU TLB invalidation/completion checks, cached private packet DMA ownership/cache-barrier and allocator-policy checks, recovery-media protection from late guest writes, production 64-bit image add/remove notification delivery, message capacity and unaligned scalar decoding, production PCIe training transitions, profile changes and bounded failures, concurrent MMC file/flush evidence and guard-corruption rejection, serialized cache-command busy completion and cache capability/state validation, DMA lower-address request/device-end bounds, SDHCI completion-publication ordering, MMC width sequencing and corrupt EXT_CSD rejection, MMC fixture rediscovery after device renumbering, independent MMC FAT file persistence and guard-corruption rejection, RK3588 eMMC resource/clock admission, ARM64 SDMA allocation/cleanup, MMC reset after an unsupported SD probe, production MMC/SDHCI register decoding, geometry, flush, bounded I/O and partial-initialization failures, shared-IRQ ownership, MMC backing-file corruption rejection, ARM64 AHCI request/DMA and controller lifecycle failure cases, two-disk persistence-oracle rejection, IPv6 prefix ranking and streams and production NDP source/link selection, the production Realtek receive routine with malformed lengths/fragments/ring wrap, actual ARM64 copy alignment/protected-page checks, checked network streams and corruption/truncation rejection, GIC MBI firmware/register admission and MSI vector allocation/reuse, onboard PCI host resource/profile rejection and root-link rejection, native-input dependency preflight, strict BFS report parsing, SSH controller-input isolation, explicit SSD-session USB reset interlocks, bounded concurrent storage writes and corruption detection, NVMe trim interval boundaries, the firmware PCIe profile, NVMe sector guards, ARM64 cache-line decoding, RNDIS packet bounds, native interrupt decoding, EFI device-path matching, capture transport, baud transitions and staged file verification/failure paths; build, QEMU and real NanoKVM deployment/recovery have been exercised |
| Recovery OS | ROOBI / Debian 11, kernel `5.10.110-33-rockchip`; SSH works independently of virtual media. Recovery now attaches its boot image read-only to prevent late Haiku writes from corrupting the replacement LUN. Automatic recovery after four native boots retained the complete original image hash and passed independent eMMC integrity; the earlier failed writable-media recovery is retained in [RECOVERY-MEDIA.md](RECOVERY-MEDIA.md). |
| Boot firmware | Board-specific EDK2 v1.1 installed in SPI; native EFI diagnostic completed; current Haiku profile uses mainline DT only; original eMMC boot firmware backed up and cleared |
| Native Haiku on ROCK | All eight CPUs start; Tracker/Deskbar, NanoKVM input, RNDIS DHCP and authenticated USB shell work. The qualified 238 GiB SSD baseline is hrev60097+156, with two installed boot/data/network/eMMC/normal-reboot cycles. Its physical +174 update remains unqualified after the recorded page-aging failure. Its first boot handled an actual SATA training transition before AHCI attachment. NVMe has passed ITS1 MSI-X, filesystem TRIM and large-file persistence; a short locked 8 GiB memory check also passed. The earlier startup stall, intermittent USB control failures, sustained acceptance and remaining hardware stay open. |
| Onboard Ethernet | The +120 USB image passes DHCP and static IPv4/IPv6 at negotiated 2.5/1 Gbit/s, including IPv6 address replacement, discovery in both directions, route selection and concurrent traffic before/after reboot. The +156 SSD passes DHCP and four simultaneous IPv4 streams on each of two boots, with full physical-path captures and zero interface errors/drops. The +168 optional cached-buffer comparison passed four native boots and 48 checked 128 MiB + 7 byte streams, with complete physical-path captures and zero interface/capture errors or drops. Short unprofiled median receive/send rates rose from 304/187 to 489/246 Mbit/s on port 0 and from 255/165 to 493/238 on port 1. Automatic Linux recovery and independent recovery-image/eMMC integrity passed after the recovery-media correction. The option is disabled by default; see [CACHED-PACKET-DMA.md](CACHED-PACKET-DMA.md). An earlier +163 full-stack trial hit its deadline during a continuously progressing but very slow TCP send; its cause remains open. Throughput remains variable and below Linux; see [NETWORK-PROFILING.md](NETWORK-PROFILING.md). Automatic IPv6 configuration, forwarding, sustained load and fault recovery remain open. |
| SATA controller | The +156 SSD firmware-profile driver handled the captured training state with a 1,577 us wait, then attached the ASM1164 using INTx IRQ 287. All four direct ports initialized on both installed boots. ARM64 two-disk 512n/512e I/O, explicit flush, guards and independent readback pass in QEMU. No physical SATA disk is identified, so native disk I/O, interrupt delivery under I/O and sustained acceptance remain untested. See [SATA.md](SATA.md) and [SSD-INTEGRATION.md](SSD-INTEGRATION.md). |
| MMC / eMMC | The +148 USB image passes eight-bit cached FAT file overwrites, explicit device-cache flushes, fresh mounts and persistence across normal reboot and orderly shutdown/startup with CPU buffers forced above 4 GiB. Linux independently verifies files, the filesystem and reference regions. A four-writer, 128 MiB cached trial passes normal reboot and Linux readback. Common SD/eMMC and FAT persistence pass QEMU. Power-loss integrity, sustained I/O, error recovery, faster clocks and eMMC boot remain pending. The +156 SSD uses the read-only cached profile and passes unchanged file/reference checks, including a full Linux FAT readback. ROOBI is preserved; microSD uses a different host. See [MMC.md](MMC.md). |
| Board revision | Owner confirmed ROCK 5 ITX PCB v1.12; current public electrical schematic is v1.11, so exact revision electrical details remain to be checked before raw register work |
| USB recovery | Workstation USB-C connection enumerates as `2207:350b`; remote loader and MaskROM entry, RAM loader download, eMMC/SPI selection and matching read-back hashes verified |
| Serial | NanoKVM UART1 (`/dev/ttyS1`) captures readable DDR/SPL, U-Boot and Linux output at 1,500,000 baud, 8N1; longer input is corrupted and an interactive login has not passed |
| Power LED readback | Reports false while running; use boot IDs, video and actual reachability |

Phase 0 is complete. The final verified image was built from the clean source
revision `288f7e9fab76234747671f881cc4ddf5240e9fa9` (`hrev60097+3`). Subsequent
status-documentation commits do not change that artifact's recorded revision.

| Final validation artifact | Location or result |
| --- | --- |
| Immutable image | `/mnt/HaikuWork/artifacts/images/haiku-arm64-30638bdd666cd13d.img` |
| SHA-256 | `30638bdd666cd13d14a4faf05977d1e7850934949db545bbf0697975469c7cf8` |
| Manifest | Same filename with `.json`; records source, toolchain, packages and EFI loader validation |
| QEMU | `/mnt/HaikuWork/artifacts/qemu/20260911T050646Z-2b567d`; first-login marker passed, no kernel panic, desktop screenshot inspected |
| Hardware | `/mnt/HaikuWork/artifacts/hardware/20260911T050746Z-ae7015`; upload hash matched, nine HDMI frames, recovery returned a different ROOBI boot ID |
| Complete command log | `/mnt/HaikuWork/artifacts/verified-iteration-final.log`; exit status 0 |
| CI | [11 control regression tests passed](https://github.com/airos-works/haiku/actions/runs/34564745899) |

At the end of phase 0, the target ran ROOBI with `/data/storage-probe.img`
selected, and neither SPI nor onboard OS partitions had been rewritten.
Subsequent recovery and firmware work is recorded below.

After correcting TX/RX wiring on 2026-09-11, NanoKVM `/dev/ttyS1` received a
15,386-byte boot log from DDR initialization through SPL, trusted firmware,
U-Boot and the ROOBI login prompt. The log contained no UTF-8 decoding errors.
Capture ran at 1,500,000 baud, 8N1 without flow control; the other spare port
received no bytes. The ROCK returned over SSH with boot ID
`e9fe9b8a-59ba-480d-8b13-5cf56da85427`. Reset evidence is in
`/mnt/HaikuWork/artifacts/serial/20260911T060852Z-9c3f0e/result.json`.

Carriage returns elicited login prompts, but longer input was corrupted:
`ps` was received as bytes `70 f3` in a paced login attempt. Neither that
attempt nor the unpaced attempt authenticated, and stock NanoKVM picocom also
produced a corrupted input character. Input pacing alone did not solve this.
The observed UART clocks are 25 MHz on NanoKVM and 24 MHz on the ROCK; baud
rounding is a suspected cause, not a measured wire-speed diagnosis. Serial
logging is available, but reliable command entry and the full serial acceptance
gate remain open. All capture sessions were closed, their serial settings
restored, and the target's serial getty reset to a fresh login prompt.
Interactive evidence is in
`/mnt/HaikuWork/artifacts/serial/20260911T061151Z-8446f7/result.json`.

First QEMU desktop capture:
`/mnt/HaikuWork/artifacts/qemu/20260911T045107Z-071bbd/screen.png`.
The serial log records successful framebuffer initialization and first-login
processing. The minimum image lacks some optional libraries: screen-saver and
shortcut input filters and `desklink` report missing dependencies. These do
not prevent this basic desktop boot; broader ARM64 package completeness remains
part of phase 11. No GPU acceleration or networking was tested in QEMU.

First full hardware cycle:
`/mnt/HaikuWork/artifacts/hardware/20260911T045250Z-a193a5/result.json`.
The board booted ROOBI with the Haiku disk attached, and recovery reselected
the known recovery image and returned a new ROOBI boot ID. One HDMI timeout
during reboot was recorded without interrupting recovery. This validates the
deployment/observation/recovery loop, not Haiku boot on the RK3588.

An incremental-build defect left `BOOTAA64.EFI` empty after `fat_shell` tried
to copy a Haiku `BEOS:TYPE` attribute onto FAT. The fork's MMC recipe now skips
attributes for the EFI loader, and the build validates its actual ARM64 PE32+
application header and partition bounds before recording success.

A write-protected USB trial reached userspace but panicked while writing the
BFS journal (`last transaction (2) still open`). QEMU's panic gate prevented
hardware deployment. The lab therefore uses a fresh writable remote copy for
each trial; the immutable local image remains unchanged. Evidence is in
`/mnt/HaikuWork/artifacts/qemu/20260911T050414Z-910fad`.

Inventory observed from this unit: RK3588, 16 GiB RAM, approximately 7.3 GiB
onboard eMMC, 16 MiB SPI loader device, two `10ec:8125` rev 05 Ethernet devices,
ASM1164 `1b21:1164` SATA controller, ES8316 audio and HYM8563 RTC. Linux's running
device tree also identifies RK806/RK8602/RK8603 power devices, FUSB302 Type-C
control and PWM fan. Identification does not establish Haiku driver support.

The installed 256 GB Samsung 950 Pro NVMe SSD is identified in ROOBI. The owner
authorizes erasing its existing data for testing and eventual Haiku installation.
Native Haiku PCIe/NVMe discovery and a full-capacity SSD installation now work
through the explicit firmware profile described below. No SATA disk was
identified at this inventory stage; the Intel AX210 Wi-Fi/Bluetooth module
was subsequently installed and identified.
Additional storage, network peers, audio loopback/receivers, displays and camera
fixtures are needed for the corresponding acceptance tests. The owner confirmed
PCB revision v1.12 on 2026-09-11.

Local evidence and credentials are not published. Initial evaluation lives at
`/mnt/HaikuWork/nanokvm/EVALUATION.md`; raw inventory, screenshots and running
device tree are in its `evidence/` directory. New runs go to
`/mnt/HaikuWork/artifacts`. The original partial snapshots have been supplemented by a full eMMC user-area
and boot-area backup and a new SPI dump. Current backup and restore evidence
is indexed by `state/rock5-backup.json`.

Serial integration validation on 2026-09-11: 17 control/transport checks passed,
including a real PTY byte stream, terminal restoration, a busy UART, a missing
readiness handshake and a dropped capture process. A rebuilt ARM64 image
`184dec46561672f2ebd144645bf5369287716173dd9ceca2f74eea0c8b8ba78c`
passed QEMU first-login review in `artifacts/qemu/20260911T065043Z-d7f19c`.
The hardware cycle in `artifacts/hardware/20260911T065636Z-50ed62` recorded
30,556 serial bytes through deployment and recovery, with no capture errors.
The board ran ROOBI during that trial; this is not a native Haiku pass.

Independent USB reads from MaskROM were verified in
`artifacts/recovery/maskrom-20260911T065323Z-2727e4`. Rockchip tool source
`304f073752fd25c854e1bcf05d8e7f925b1f4e14` was built locally and used with the
Radxa-published `rk3588_spl_loader_v1.15.113.bin` RAM loader (SHA-256
`26baab70e6b915364f7d73d88298366db1bfc346e34683e95d3d11b52492047f`).
Both storage selections reported their expected capacity, and the eMMC first
16 MiB and complete SPI image matched the Linux snapshots. The complete restoration drill passed in
`artifacts/recovery/restore-drill-20260911T070040Z-0383e0`: all 7,818,182,656
eMMC user-area bytes and all 16,777,216 SPI bytes were written through the
MaskROM RAM loader, read back and matched against SHA-256. ROOBI then returned
with boot ID `6eb27d7c-0b28-4062-905c-08c580d9a2ea`. UART capture remained
active throughout. The cleaned full eMMC restoration image has SHA-256
`ce5abe6ec8372a198198d2868031ec5b1feddeff88be85fe5fb15174472286b5`;
the restored original SPI hash is
`fbf27964ee2694dbea2836aa996120a86bffb1a57657a18e7b28e1fbb30009e5`.
See [RECOVERY.md](RECOVERY.md) for the procedure and its physical fallback.

The standalone EFI diagnostic and recovery chainloader build with
`tools/rock5-itx/build-efi-tools.sh`. The diagnostic records GOP geometry,
firmware revision, exception level, configuration tables and the memory map,
and saves available DT/ACPI data to its own USB volume. Its EFI entry and
completion markers passed QEMU; the recovery chainloader also loaded and ran
the diagnostic as a sibling EFI application in
`artifacts/qemu/20260911T070129Z-939dd0`. The native diagnostic subsequently passed on the ROCK in
`artifacts/firmware-trials/20260911T071501Z-93f34c`: EL2, UEFI 2.70,
1920x1080 BGR framebuffer at `0xed3a0000`, 134 memory descriptors, a
184,025-byte mainline DTB, and eight ACPI tables with valid checksums.
The firmware exposed both DT and ACPI. Raw tables and the complete diagnostic
are in that run's `efi-diagnostic/` directory. The matching ROOBI kernel then
booted through the EFI stub using the saved vendor DTB and `acpi=off`;
`/sys/firmware/efi` was present and SSH returned boot ID
`cd809968-0462-4b71-a93a-04f3f9844ddd`. The combined diagnostic/recovery run
showed Linux userspace startup around 93 seconds, including the diagnostic's
30-second pause. Standalone EFI recovery is tested with a 180-second timeout.
F4 subsequently worked after NanoKVM's
BIOS-compatible HID subclass was enabled and the controller restarted.
Ordinary reset and power-button recovery remained in MaskROM; loading the
verified RAM downloader and issuing its normal `rd 0` reset returned ROOBI
through the standalone EFI recovery image. Evidence is in
`artifacts/firmware-trials/maskrom-exit-20260911T074004Z-ce46e1`, with boot ID
`0c0d415f-504d-49ef-bbb1-7fbb879e197b`. This is a working remote firmware recovery
route while EDK2 can initialize USB keyboard input; physical-button entry
remains the fallback for firmware that cannot initialize.

The reusable EFI media packager verified all files by FAT read-back, and its
generated diagnostic image passed QEMU in
`artifacts/qemu/20260911T073935Z-90d8aa`. Capture now supports acknowledged baud
changes. Six real-PTY tests and
sixteen lab-control tests passed, including recovery after a failed baud change.

## First native Haiku trials

The first native Haiku image (`184dec46561672f2...`) stopped in
`dtb_get_interrupt()` with `unsupported interruptCells`. The panic is visible
in `artifacts/hardware/20260911T074159Z-376565/frame-010.jpg`. The running
firmware DTB declares four cells for `arm,gic-v3`; the loader only accepted
three-cell GIC descriptions. The fork now accepts four-cell descriptions with
a zero affinity cell, validates the property length, and continues to reject
PPI affinity partitions that the loader cannot represent.

That first trial also exposed a console problem. EDK2's DW8250 serial library
cannot change attributes, and its Serial I/O wrapper reports that failure as
`EFI_INVALID_PARAMETER`. Haiku discarded the usable interface after requesting
115,200 baud. The fork now keeps firmware serial output for unsupported
attributes and preserves the UART setup; it also avoids programming a baud
divisor without a known input clock. Current hardware capture stays at
1,500,000 baud. This does not resolve the previously observed UART input errors.

Image SHA-256 `be6afb099a08adbfb4c6aa4e7525a21c8f89ebfcbb0da65129fba5280e9a72bd`
passed the QEMU first-login gate in `artifacts/qemu/20260911T074929Z-601c05`.
On the ROCK, `artifacts/hardware/20260911T075114Z-c7daf0` showed Haiku boot icons,
readable loader diagnostics, the selected 8250 UART/GICv3, kernel loading, and
`Calling ExitBootServices. So long, EFI!`. It then reported
`PANIC at PC : 0x000000000005f420` from trusted firmware. Recovery returned
ROOBI boot ID `38fdfe87-d47f-4950-958a-f10633a4a98c`; capture saved 186,551 bytes
without transport errors. The original interrupt-parser failure is resolved
on hardware, but kernel entry has not yet been demonstrated.

The panic address was checked against the exact BL31 segment extracted from
the installed release (segment SHA-256
`9a365c355a5eec4eebc863dc62afbc3b2ac6ca46d7e9f141463c743767acf314`). It is in
the lower-exception-level entry path after a pending SError check. The access
that caused that error remains unidentified. This is separate from Haiku's
earlier parser panic. Further handoff tracing is recorded with subsequent runs.

The final checkpoint image is
`artifacts/images/haiku-arm64-51547c73298843b6.img`, SHA-256
`51547c73298843b609573076315ecd46e483558fc23a8d00762023491c0b2af0`.
It passed QEMU first login in `artifacts/qemu/20260911T080602Z-64cd8a` and was
tested natively in `artifacts/hardware/20260911T081017Z-97bb79`. The image remains
a development build with its original source revision and patch in the manifest;
its functional loader changes are committed in `e1aeff87be`.

The firmware now exposes mainline DT only (`ConfigTableMode=2`,
`FdtCompatMode=2`). This changed Haiku's CPU count from sixteen to the correct
eight. Serial confirms that `ExitBootServices` completes,
`SetVirtualAddressMap` returns `EFI_SUCCESS`, and boot-CPU MMU setup completes.
The same trusted-firmware pending-SError panic then occurs as secondary CPU
startup begins. The loader now keeps serial active during runtime-map setup
and checks the runtime-map return status. The originating access remains open;
single-core bring-up and the CPU startup/PSCI path are the next investigations.
That checkpoint did not demonstrate native kernel entry, desktop, or peripheral acceptance.

Automatic recovery returned ROOBI boot ID
`d354f0ef-e6fa-4718-b48e-bc7ba831f1be` with the DT-only profile. The final run
saved 170,769 serial bytes without transport errors. Four native Haiku trials
have exercised the loop; phase 1's twenty-reset gate and complete Linux
functional baseline are still open. UART receive is usable, while the previous
long-input corruption remains unresolved.

## Kernel bring-up

Subsequent diagnostic builds used the existing `disable_smp true` setting to
isolate boot-CPU behavior. Native kernel entry is now demonstrated in
`artifacts/hardware/20260911T100833Z-051366`,
`artifacts/hardware/20260911T101639Z-0f6113`, and
`artifacts/hardware/20260911T103227Z-84dea9`. These runs reach the kernel's early
memory allocator, GICv3 setup and architected timer initialization. They stop
with `ESR=0xbe000011` when the first kernel thread enables interrupts. The
SError's reported FAR does not identify the originating access.

The loader's `ISR_EL1` traces show the error is absent at EFI entry and after
CPU, ACPI, device-tree, video and VFS initialization. It is already pending by
boot-volume selection, before loading the kernel, drawing the logo, generating
page tables or exiting boot services. Thus the earlier trusted-firmware panic
at the first secondary-CPU SMC did not establish a fault in PSCI itself.
Further tracing localized the error to scanning unrelated eMMC boot areas.
The original access inside that firmware path is still unidentified.

EL2 QEMU testing exposed a separate timer bug: with VHE enabled, the virtual
timer register aliases no longer signal the EL1 virtual timer interrupt 27.
The kernel now uses the EL2 physical timer and interrupt 26 when running at EL2,
retaining the existing virtual timer at EL1. The firmware DT's `timer` node
confirms these interrupt numbers on the ROCK. Before the change, EL2 QEMU
stalled during xHCI initialization in `artifacts/qemu/20260911T100201Z-24c9e2`.
After it, both EL1 and EL2 first-login gates passed for image
`224bdd5d310c4f811c934a45ed798a300cf1d7aa023a76d8da6c46c5a2b8594a` in
`artifacts/qemu/20260911T101539Z-5a6fda` and
`artifacts/qemu/20260911T101539Z-c69d07`. The iteration script now exercises EL2.
This still uses fixed standard timer PPIs; generalized DT/ACPI timer discovery
remains future work.

The ARM64 EFI runtime map also now supplies identity `VirtualStart` values
for runtime descriptors, consistent with its transition mappings, and maps
runtime MMIO as Device memory. Previously every runtime descriptor supplied
virtual address zero. This is a separate correction: the pending SError is
observed before runtime-map installation.

Each trial preserves its immutable image, source patch, raw UART capture and
automatic recovery result under `/mnt/HaikuWork/artifacts`. Matching unstripped
loader/kernel symbols for recent trials are retained under
`artifacts/symbols/<image-sha256-prefix>/`.

The EFI loader previously scanned every disk in firmware enumeration order.
In `artifacts/hardware/20260911T104615Z-d2ba76`, it scanned the eMMC user area
and boot areas before reaching the USB disk. A pending SError was first seen
on entry to an LBA 1 read of the third raw eMMC device. A separate run without
eMMC protocols had no SError, so an earlier suspected network-probe cause was
ruled out. Network probing did expose independent bugs: absent/invalid load
options could leave an uninitialized buffer, unrelated `key=value` options
could loop forever, and failed probes shut down an interface they never started.
Those cases are now guarded; netboot functionality itself remains unverified.

The loader now uses EFI's loaded-image device handle and device-path ancestry
to try its own disk first, preserving explicit network boot and other disks as
fallbacks. This both respects the selected boot disk and avoids the unrelated
eMMC accesses on normal USB boots. It does not repair firmware access to the
eMMC boot areas; a full fallback/menu scan can still encounter that problem.

Image `d5c8d93f2633007c...` passed four-CPU EL2 QEMU in
`artifacts/qemu/20260911T105407Z-b5e084` and booted natively in
`artifacts/hardware/20260911T105507Z-a86957`. The eMMC protocols were present,
the USB disk matched a 35-byte EFI path prefix, and the pending-error register
stayed clear through handoff. The kernel completed its all-CPU rendezvous,
reported eight logical CPUs, and ran `main2` on CPU 4. The normal SMP default
has been restored. This establishes initial eight-core boot, not the roadmap's
SMP stress, IRQ/IPI routing or memory-integrity acceptance tests.

The same run passed a newly added ACPI guard. DT-only EDK2 still publishes a
small ACPI set containing BGRT, with no DSDT. ACPICA previously dereferenced an
invalid DSDT index and faulted in `AcpiTbLoadNamespace`. The bus manager now
checks for a DSDT before loading the namespace and declines ACPI initialization
when it is absent. ACPICA cleanup still logs `Could not remove SCI handler`;
the kernel continues to device discovery. No native ACPI support is claimed
for this firmware profile.

Boot now reaches `vfs_mount_boot_file_system` and stops with
`did not find any boot partitions!`. The existing USB host drivers bind through
PCI, while the ROCK exposes its host controllers through the device tree.
The Linux reference captured in `artifacts/rock5-usb-baseline-20260911.json`
identifies NanoKVM `3346:1009` at 480 Mb/s on the platform EHCI controller
`fc880000.usb`, bus 2 port 1. The firmware DT declares
`rockchip,rk3588-ehci`, `generic-ehci`, a `0x40000` register window, and SPI 218
(GIC interrupt 250). The first USB target is therefore platform EHCI attachment,
including four-cell GIC decoding, DMA/cache handling and the existing PHY/clock
state. DWC3/xHCI and the other physical ports remain separate work.

Recovery after this run returned ROOBI boot ID
`4fab5591-2479-4103-82ce-29c89b69d815`; UART capture reported no transport errors.
Native userspace and peripheral acceptance remain open.

## Verified kernel checkpoint

A clean build from `c9cf0293450521ca6fa13c8a251e5d3013c2afef`
(`hrev60097+11`) repeated the eight-core native boot after temporary tracing
was removed. The loaded-image disk preference kept the pending SError clear
through handoff, the EL2 physical timer initialized on IRQ 26, and all eight
CPUs completed startup. `main2` ran on CPU 4 and reached the expected
`did not find any boot partitions!` panic. The HDMI debugger frame was inspected.
This is a kernel bring-up checkpoint; it does not establish native userspace,
SMP stress stability, or peripheral acceptance.

| Checkpoint evidence | Location or result |
| --- | --- |
| Immutable image | `artifacts/images/haiku-arm64-6ffb18dd981474d2.img` |
| SHA-256 | `6ffb18dd981474d224183ff6df5946c030f41985becfa69472d569c6f4e02063` |
| Manifest and symbols | Same image basename with `.json`; unstripped loader and kernel in `artifacts/symbols/6ffb18dd981474d2/` |
| Build log | `artifacts/build-20260911T110538Z.log` |
| EL1 QEMU | `artifacts/qemu/20260911T110644Z-ce9634`; four CPUs, first login passed, no kernel panic |
| EL2 QEMU | `artifacts/qemu/20260911T110644Z-b339fb`; four CPUs, first login passed, no kernel panic |
| Native ROCK | `artifacts/hardware/20260911T110842Z-aa232c`; reviewed `result.json`, `trial-serial.log` and `frame-016.jpg` |
| Local checks | 23 regression checks, Bash syntax and whitespace checks passed; `artifacts/control-checks-kernel-checkpoint.log` |
| CI | [23 control regression checks passed for this source revision](https://github.com/airos-works/haiku/actions/runs/34592601056) |

All evidence paths above are under `/mnt/HaikuWork`. Documentation-only commits
after this checkpoint do not change the image's recorded source revision.
Automatic recovery returned ROOBI boot ID
`db3777db-743f-4192-9b59-1c7c22d139bc`, different from the pre-trial boot ID.
The cycle saved 160,428 serial bytes with no transport errors and restored the
configured EFI recovery image. Platform EHCI support for the NanoKVM USB disk
is the next native boot dependency.

## First native desktop through platform EHCI

Image SHA-256
`5fe52597f02f912f3a731f7f749efa80a0ec74e47c74e9cc562b45468ee30dfe`
booted the native Haiku desktop in
`artifacts/hardware/20260911T113955Z-3c80da`. Both device-tree EHCI controllers
started: `fc800000` on IRQ 247 and `fc880000` on IRQ 250. The second controller
enumerated `NanoKVM USB Mass Storage 0520`; Haiku mounted BFS and packagefs,
ran the first-login script, initialized the framebuffer driver, and displayed
Tracker and Deskbar. `frame-012.jpg` was inspected. All eight CPUs completed
startup, and the trial contained no kernel panic.

The FDT attachment reads register windows and interrupt numbers from the
firmware device tree. It accepts enabled, little-endian generic EHCI devices
with level-sensitive GIC SPIs and identity address mappings. Unsupported
translations, IOMMUs, integrated transaction translators and PPI affinity
partitions are not silently accepted. Kernel FDT decoding now understands
four-cell GIC descriptions with a zero affinity cell and rejects malformed
or out-of-range entries; the separate interrupt-map and extended-interrupt
paths retain their earlier limitations.

The shared EHCI implementation now separates PCI configuration from platform
resources, uses ordering barriers when publishing queues and reading DMA
completion, and guards cleanup after partial initialization. A private ARM64
DMA pool provides Normal Non-cacheable RAM below 4 GiB, including bounce
buffers. Cached allocation contents are cleaned and invalidated before the
mapping changes, and the pool is accessed only through that mapping. This
establishes boot I/O, not sustained integrity or high-memory acceptance.

Only the standard EHCI register interface is used. The board remains on the
previously recorded EDK2 firmware and mainline-DT profile; PHY, clock, reset-line
and power-domain programming is still supplied by firmware. Native resource
management and suspend/resume remain separate work. The implementation was
checked against the [EHCI specification](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/ehci-specification-for-usb.pdf),
the [generic EHCI binding](https://github.com/torvalds/linux/blob/v6.15/Documentation/devicetree/bindings/usb/generic-ehci.yaml),
and the [GICv3 interrupt binding](https://github.com/torvalds/linux/blob/v6.15/Documentation/devicetree/bindings/interrupt-controller/arm%2Cgic-v3.yaml).

The preceding trial, `artifacts/hardware/20260911T113345Z-4c1fb4`, still stopped
at boot-volume discovery without attempting EHCI initialization. Generic FDT
nodes did not search `busses/usb`; adding that driver-search path allowed the
attachment to run. FDT nodes are registered in source order, so the attachment
does not require the GIC's device-manager node to have been registered before
the already-decodable interrupt specifier is used.

The desktop image passed the four-CPU EL2 QEMU EHCI first-login gate in
`artifacts/qemu/20260911T113829Z-c40999`. Earlier shared-driver checks passed
both EHCI and xHCI first login in `artifacts/qemu/20260911T112954Z-152d78` and
`artifacts/qemu/20260911T112954Z-667655`. Twenty-four host regression checks
passed in `artifacts/control-checks-ehci-platform.log`. These are development
builds, with their source patches in the image manifests; unstripped loader,
kernel and EHCI symbols are saved under each image hash prefix.

Automatic recovery returned ROOBI boot ID
`ec4a0f60-1e30-4b41-8e98-50155a04d299`; capture saved 172,358 bytes with no
transport errors. One USB control request stalled during HID initialization.
The minimum image also reports missing optional screen-saver, game and media
libraries, as it did in QEMU. Keyboard/mouse interaction, USB networking,
sustained storage writes, SMP/memory stress and other peripheral acceptance
remain untested by this desktop observation. NanoKVM's USB network endpoint is
configured as `10.239.6.1/24`, and the image includes `usb_rndis`; loading that
driver alone does not prove a working network connection.

## Recovery cycle audit and interactive diagnostics

The reset/recovery portion of phase 1 now has 20 reviewed cycles with serial
readiness, more than 10,000 captured bytes, no serial transport errors, a Linux
recovery marker, and different pre-trial and recovered SSH boot IDs. The audit
is `artifacts/reset-recovery-audit-20260911.json`; its runs span
`hardware/20260911T065636Z-50ed62` through
`interactive/20260911T122522Z-f213a0`. Eighteen contain the Haiku EFI loader
banner. The first two precede that handoff. Trials used different development
images and firmware profiles, so this validates the recovery mechanism, not
20 consecutive successful Haiku boots. Transfer failures and trials without
serial capture are excluded. UART transmit reliability and a measured Linux
functional baseline remain open.

The clean desktop checkpoint at source `9a1b6b0b2ebc9eaa5eb091483a75a5e36f2ff65c`
has image SHA-256
`bb19f2f074ad705e6910f57c777321734cd436733890979438173d907100c811`.
Its EL2 QEMU EHCI gate passed in `artifacts/qemu/20260911T114903Z-264eac`.
The real desktop was visible in
`artifacts/interactive/20260911T115408Z-597177/frame-007.jpg`, but keyboard and
mouse actions did not visibly change it. The unsupported keyboard control
`0x2710` is the optional `KB_GET_KEYBOARD_ID` query; input initialization proceeds
past it. In QEMU, Ctrl+Alt+Delete opens Team Monitor and its Terminal can run
commands whose output is captured through `/dev/dprintf`. See
`artifacts/qemu-interactive/20260911T121444Z-20d5f4` and
`artifacts/qemu-rndis/20260911T122650Z-7c1a8c`.

Two subsequent native trials stopped before entering the kernel, at the
loader's cache clean-by-set/way instruction (`Loop3`, offset `0x11c0`). These
are `interactive/20260911T120538Z-8264bf` and
`interactive/20260911T121718Z-fbfcbf`; the latter received no keyboard input
during the firmware countdown. Their loader bytes match the preceding clean
desktop image. A temporary exception diagnostic then booted successfully in
`interactive/20260911T122522Z-f213a0`, reporting EL2 with `DAIF=0x3c0` before
cache cleanup. That success does not establish the cause or a fix for the
earlier faults.

The diagnostic desktop trial queued input reads for all three NanoKVM HID
interfaces, but no input transfer completed after direct writes to NanoKVM's
keyboard and relative-mouse character devices. Recovery returned boot ID
`53228bd4-6825-4c57-be8e-ba21e9231e48`; 175,805 serial bytes were captured with
no transport errors. USB input and networking remain under investigation.

RNDIS control setup now matches the legacy CDC ACM `02/02/ff` interface used by
NanoKVM, allocates one notification endpoint packet, and sends a four-byte
packet-filter payload including directed traffic. The earlier fixed eight-byte
buffer rejected QEMU's 16-byte notification endpoint, and its malformed
packet-filter length caused a control stall. The corrected driver obtained
DHCP address `10.0.2.15/24` in
`artifacts/qemu-rndis/20260911T125207Z-4dfbb9`, with two packets transmitted and
received and no interface errors. This test used one EL2 CPU and xHCI for
RNDIS. Its local fixture blocks `usb_ecm` through package settings because
QEMU exposes both RNDIS and ECM configurations; otherwise ECM switches the
configuration under RNDIS. The base image is
`2ae8081dff330fa2464f25ab95c9152196adedf92e70246e4a0dedce0df4b788`, and the
fixture's parent hash and exact settings are recorded in its manifest. Native
DHCP, sustained networking, configuration arbitration and hotplug acceptance
are separate checks.

## Native USB networking and automatic diagnostics

Native RNDIS obtained `10.239.6.146/24` in
`artifacts/interactive/20260911T130941Z-72a087`, using image
`13b806fb0988e0c07341070dba3ff1ce0c1bafda132b6dc02297991488a69932`.
All four pings from NanoKVM's `10.239.6.1` returned, with no packet loss.
An automatic lab startup script reported all eight CPUs, approximately 16 GiB
of managed RAM, the network configuration and USB inventory through
`/dev/dprintf`. Reporting available RAM is not a high-memory integrity test.
The image includes a correction to high-speed interrupt polling intervals:
EHCI now converts the descriptor's microframe exponent to a frame interval and
S-mask instead of issuing bursts of eight polls.

The next trial, `interactive/20260911T131644Z-1a3764`, repeated DHCP and four
successful pings. Its image is
`bf6efe2c9b55d800d95ae80b6812b0a12b3c9b70ce8ca1ad93d6d29b1eaa2674`.
A separate queue-retirement experiment preserved removed periodic links and
waited before reusing their storage. It did not resolve HID inactivity:
direct Ctrl+Alt+Delete reports were accepted by NanoKVM, but no Haiku HID input
transfer completed. Both runs returned to ROOBI with fresh boot IDs and no
serial transport errors. Sustained network traffic and other USB ports remain
separate acceptance work.

Linux input comparison is recorded in
`artifacts/linux-input/20260911T132936Z-8274f1`. ROOBI kernel
`5.10.110-33-rockchip` received the expected Ctrl press/release, relative X/Y
movement and absolute X/Y coordinates on all three NanoKVM interfaces using
the same raw report path. A final metadata SSH call briefly failed; a follow-up
confirmed the same ROOBI boot ID. This validates the physical input path and
these basic Linux events, not the complete Linux hardware baseline.

The cache-handoff diagnostic caught another loader fault in
`interactive/20260911T125610Z-ceced9`: ESR was `0x02000000`, and the saved loader
has an `orr` instruction at the reported PC, offset `0x3100`. This is an
unknown-instruction exception, not evidence of a trapped cache-maintenance
instruction or a valid FAR memory address. Cleaning caches before disabling
the firmware MMU/cache configuration then passed the two native boots above.
Stale instruction contents are a hypothesis; these two successes do not yet
establish a reliable fix. Those development images contained temporary exception
instrumentation, removed in the later checkpoint below.

The login program stored `getopt()`'s integer return value in a `char`.
This ARM64 toolchain defines `__CHAR_UNSIGNED__`, so the `-1` end marker became
255 and login exited through its usage path. Keeping the result as an `int`
allowed an authenticated shell in `artifacts/qemu-shell/20260911T133038Z-625a42`:
an incorrect password was rejected, then the correct private credentials ran
`uname`, `ifconfig` and a service-configuration check. The local image overlay
contains a generated password hash and an explicit opt-in file; telnet binds
only to the USB address. QEMU uses localhost forwarding and blocks the
competing ECM configuration. Native shell access subsequently passed below.

## Native input, authenticated commands and high-memory check

EHCI interrupt queue heads must use a zero NAK reload count (RL), as specified
in section 4.9 of the
[Intel EHCI specification](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/ehci-specification-for-usb.pdf).
The shared initializer had set RL to three. Correcting it for interrupt queues
and periodic anchors restored native HID polling. The preceding interval and
queue-retirement changes alone had not restored input. The combined fix is
commit `494b6180f6`.

In `artifacts/interactive/20260911T133313Z-6b71f4`, Ctrl+Alt+Delete opened Team
Monitor, an absolute mouse click opened Terminal, and a typed command emitted
`ROCK_NATIVE_INPUT_OK` on UART. Relative mouse movement also visibly moved the
cursor. The private image SHA-256 is
`4e7a2eba33c78d6c241c5e355936f3895749e1af76fd92c453f66c5569e90dde`.
An authenticated shell executed `uname`, `sysinfo`, `ifconfig`, `netstat`, process
inventory and a driver checksum. The listener was bound to `10.239.6.146:23`,
with NanoKVM at `10.239.6.1` as its peer. Workstation access used an SSH tunnel
to NanoKVM, then telnet across the private USB link.

The next trial, `artifacts/interactive/20260911T134405Z-19e940`, repeated the
desktop, keyboard, absolute mouse and authenticated shell checks with all
temporary exception vectors and USB traces removed. Its private image is
`d0e576d822b76616da13c3b405ac475b6cdc3e11f92f236808828369c7764da1`.
This was the fourth successful native boot with the pre-handoff cache clean
(`186268d8cd`), and the first without diagnostic instrumentation. More boot
coverage is required before calling the intermittent loader fault resolved.

A 140,691-byte memory-test executable was uploaded directly to that running
Haiku session, read back through `sha256sum`, and executed only after its hash
matched the workstation binary:
`d5a0e997f1250d61c7bba1954c95460dc465f623c379a148066afe1c28673d0b`.
No new image or reboot was needed for this test. The 8 GiB allocation used
`B_FULL_LOCK`, eight workers and two fill/verify passes, including reads of
another worker's region. It passed with zero mismatches and recorded activity
on all eight CPUs. Since the allocation was resident and exceeded 4 GiB, this
exercises physical RAM above that boundary. This short run does not establish
sustained CPU, thermal, DMA or general memory-management stability.

The packaged checker and its injected-corruption negative control passed in
four-CPU QEMU at `artifacts/qemu-shell/20260911T135151Z-7c2fee`. The 64 MiB check
passed, and a deliberately corrupted word caused the expected failure and
exit status 1. Source is [memory_probe.cpp](../../tools/rock5-itx/memory_probe.cpp).

Normal native `shutdown -r` then stalled with the desktop showing “Asking other
processes to quit.” No new loader banner appeared. ARM64's kernel reset function
was still a stub, but this screen also permits an earlier userspace hang;
the two paths require separate tests. Out-of-band recovery returned ROOBI with
boot ID `dccca9d2-ecae-4cb4-8bfb-dad7fffc6a2c` and no serial transport errors.
The trial's `review.json`, `native-memory.txt`, `probe-upload.txt`,
`software-reboot-command.txt` and `frame-090.jpg` preserve these results.

## Firmware software reset and power-off

Commit `9a23e4317c` replaces ARM64's shutdown stub with PSCI `SYSTEM_RESET` and
`SYSTEM_OFF`. The EFI loader passes the SMC/HVC calling method discovered from
the device tree or ACPI FADT to the kernel. Device-tree discovery accepts PSCI
0.2 and 1.0 compatible lists and validates the method property. The kernel
queries the firmware version before enabling these calls. This follows the
standard interface also used by
[EDK2's PSCI reset library](https://github.com/tianocore/edk2/blob/master/ArmPkg/Library/ArmPsciResetSystemLib/ArmPsciResetSystemLib.c);
it adds no RK3588 register or clock programming.

Four-CPU QEMU completed quick reboot, a fresh authenticated login and power-off
at both EL2/SMC (`artifacts/qemu-shell/20260911T141410Z-d1a82d`) and EL1/HVC
(`20260911T141639Z-d5aa80`). Normal desktop reboot also completed in
`20260911T142204Z-cdbcf3`, followed by fresh login and quick power-off. An earlier
normal-path attempt (`20260911T141909Z-0ef1e6`) never reached a login prompt and
therefore did not test shutdown.

Native trial `artifacts/interactive/20260911T141640Z-a9ec13` used private image
`bf5752c86a8122d8d32fe9bcfad77729dc3e7364e49e4ee57dccea70d7453fe4`.
The ROCK reported PSCI 1.1 via SMC. `shutdown -rq` issued a firmware reset,
returned through the EFI loader to Haiku, restored DHCP and accepted a new
authenticated shell. A 64 MiB, eight-worker, two-pass memory check then passed.
This is an actual software reboot without a NanoKVM reset pulse between boots.

Native `shutdown -q` reached `SYSTEM_OFF` without returning; serial output
stopped and a requested HDMI capture timed out. The session consequently recorded
an error for that capture. Reset alone did not bring the powered-off board back;
the existing recovery power-button fallback returned ROOBI with boot ID
`b207da33-2a4a-4abd-b0a4-b9e691751fb1`. NanoKVM remained reachable. These are
observations of firmware power-off and recovery, not electrical power measurement
or front-panel/suspend qualification. Native normal reboot passed in the later binary-transfer trial below; normal
power-off remains untested on the ROCK.

Repeated QEMU shell testing exposed an intermittent login failure, both on an
initial boot and after software reboot. In `20260911T143736Z-6d3a56`, keyboard
diagnostics showed DHCP, an active RNDIS interface and a TCP listener on the
correct USB address, but remote login did not open. Thus a printed shell
configuration marker is not sufficient acceptance. Investigation and repeated
authenticated checks remain necessary before calling this lab path reliable.


## Service listener and binary transfer checks

The intermittent missing login prompt was reproduced natively in
`artifacts/interactive/20260911T152843Z-e77e29`. A temporary server trace showed
`select min=4 max=4 pipe=4`: `pipe()` had returned read descriptor 4 and write
descriptor 3. The initial `select()` range was calculated from the write end,
so it excluded the read end. The listener never received the update wakeup when
the USB login socket was added. The authenticated attempt timed out, and
recovery returned ROOBI boot ID `0a7a9217-800c-408f-9239-914dad9a74ec`.

The server now derives its initial range from the read descriptor. Temporary
traces have been removed. `rock5_services_probe` links the actual server listener
and forces its read descriptor to 64 while the write descriptor remains lower.
It waits for the empty listener to block, adds a loopback service, and requires
an actual child process reply. Its `--legacy-range` negative control deliberately
restores the old range and must fail to receive that reply.

All combined QEMU checks passed in `artifacts/qemu-shell/20260911T154454Z-708873`,
using private image SHA-256
`f04c260283dd5ad01af2431caba3789aae9e3c6f5c636b55eee491368e2267ab`:
incorrect-password rejection, authenticated commands, the service regression and
negative control, locked memory checking with injected-corruption detection,
CPU affinity and cross-core clock ordering, 32 fork/exec/COW checks, eight
protected-page faults, an 8 MiB binary round trip, rejection of truncated input,
and normal desktop reboot, fresh login and power-off through PSCI/SMC.
The clock/fork/fault tests are short integrity probes, not sustained qualification.

Large base64 uploads through an interactive terminal exceeded the five-minute
limit even after fixing simultaneous send/receive. A subsequent 4 KiB test in
`artifacts/interactive/20260911T151858Z-d617de` confirmed that bootstrap Bash
also lacks `/dev/tcp`. The lab image now packages `rock5_file_transfer`, a bounded
native binary stream client. Host tools relay files over NanoKVM SSH and its
private USB network, check lengths and SHA-256, and retain partial results as
unaccepted files on error. No extra NanoKVM file copy is needed. The reusable
session client supports commands, uploads and downloads while keeping UART,
HDMI and automatic recovery active. All 32 host control checks passed in
`artifacts/control-checks-20260911-binary-files.log`.

A separate QEMU run, `artifacts/qemu-shell/20260911T153111Z-027922`, had no DHCP
address and zero USB network packets. That run did not reach shell configuration;
it is a separate startup failure from the traced listener bug. RNDIS control
waits and initialization remain under investigation. QEMU packet captures and
private shell images contain authentication material and remain local.


Native validation of that image is in
`artifacts/interactive/20260911T154757Z-47e4ab`. The service regression and its
negative control passed on the ROCK. All eight pinned workers sampled the
clock without backwards readings or wrong-CPU observations; 32 fork/exec/COW
checks and eight protected-page faults passed. A locked 8 GiB allocation passed
two checks with eight workers in 2.97 seconds. This is a short integrity result.

The native 8 MiB binary upload completed in 18.97 seconds and its first download
in 16.18 seconds, including authentication and checksum verification. The
returned file matched the source byte for byte, with SHA-256
`13d3d2a1d7eee5c4ff92996af3a186741320b02dd95451eaff323772fb966d95`.
Normal `shutdown -r` then reached PSCI reset, booted Haiku again without a
NanoKVM reset pulse, and accepted fresh authenticated commands. The stored file's
hash, another platform probe, and a 64 MiB memory check passed after reboot.

During a subsequent download, the NanoKVM itself stopped answering SSH, HTTP,
ping and ARP at `192.168.1.8`; the workstation's Ethernet link and router
remained reachable. The controller outage's cause is unknown. The 425,984-byte
partial download was not accepted, and queued executable-deployment and normal
power-off tests were not run. Automatic recovery could not contact NanoKVM, so
the session is explicitly `recovery_failed`; the last verified target state is
Haiku running after normal reboot. Twelve native boots have now reached the
startup probe with the cache pre-clean change; this is not a long-run stability
acceptance. Controller access must be restored before another hardware trial.

Shell and file SSH connections now request a five-second keepalive interval
with three missed replies allowed; the workstation's inherited 300-second
interval had delayed failure detection. Interactive sessions also return a
nonzero process status when the trial or recovery fails. All 32 host checks
passed after these changes in
`artifacts/control-checks-20260911-final-shell.log`.

## Bounded RNDIS control waits

Topic branch `rock5-rndis-control`, source commit `7d51890c72`, returns failed
initialization sends immediately and limits response-notification waits to five
seconds. Failed opens cancel the notification transfer, and subsequent opens
clear old semaphore counts. A timeout or interruption during an optional query
also stops initialization, since a late response must not be consumed as the
next command's reply. This bounds the notification wait; it does not establish
the cause of the earlier intermittent startup failures.

Three temporary fault injections were exercised in QEMU:

| Injected failure | Observed result | Evidence under `artifacts/rndis-faults/` |
| --- | --- | --- |
| Drop control-response notifications | Initialization returned a timeout after approximately five seconds; the desktop startup probe completed | `20260911T162522Z-a711a9` |
| Fail the initialization send | Open returned the send error without entering the response wait | `20260911T162858Z-8abd58` |
| Return a timeout from the maximum-frame-size query | Initialization stopped before media-state and link-speed queries; no shell was configured through the failed adapter | `20260911T163744Z-acad91` |

An earlier query-fault attempt, `20260911T163304Z-7c63e9`, stalled during general
userspace startup before reaching either the RNDIS driver or startup probe. It
is retained as a failed trial, not counted as a fault-handling pass. The later
attempt used the same immutable image and reached the intended injection.

All injections were removed before the final development image was built.
The full QEMU suite passed in `artifacts/qemu-shell/20260911T163918Z-9a7a33`:
authenticated commands, memory and platform probes, service-listener regression
and negative control, an 8 MiB binary round trip with truncated-input rejection,
normal reboot, fresh login and normal power-off. All 32 host control checks
passed in `artifacts/control-checks-rndis-timeouts.log`.

A clean build from `7d51890c72d5f22986b95b53e8138ccb2d9e29af` has base-image
SHA-256 `9ab5f631c0ae3a78dc027142b90df7530554cc796f11897521d99f01c10bc238`.
Its private shell overlay also passed the full suite in
`artifacts/qemu-shell/20260911T164148Z-612466`, including normal reset and off.
The manifest and gate are indexed locally by
`state/rock5-rndis-control-checkpoint.json`; the private overlay remains local.

Native validation of this change remains pending: NanoKVM is still unreachable
at its saved address, and its saved mDNS name did not resolve. The hardware
checkpoint on `rock5-itx` remains available separately. Response-length and
request-ID validation, retry behavior and sustained USB network testing remain
open; the new timeout is not evidence of full RNDIS reliability.

## Native RNDIS retest and recurring controller outage

NanoKVM returned after a controller reboot at approximately 21:25 UTC on
2026-09-11. Its boot ID was `e74dfc19-8368-4fda-9a20-eba86b42b8b1`.
The expired web session was renewed, and recovery with active UART capture
returned ROOBI boot ID `c1d0ee95-6fb6-408e-ad26-2ce2ea6a9bc7`.
Evidence is in `artifacts/controller-recovery/20260911T212742Z-51072a`.

The pre-recovery screenshot showed the old Haiku session in the kernel debugger
with `last transaction (6) still open!`. Its stack ran from a syslog write through
BFS to `cache_start_transaction()`. The capture gap prevents determining whether
this happened during the original controller outage or the later controller
restart. It does not establish the cause of the controller's LAN failure.

The clean RNDIS candidate was then deployed as a fresh USB image in
`artifacts/interactive/20260911T212914Z-78e307`, private image SHA-256
`b4bd947372f2189b5cda5c7c5d807842e3d2b5c404a11ea639abdb15c2e8ef28`.
All eight CPUs started and the adapter obtained `10.239.6.102`. NanoKVM's gadget
MAC changed across its reboot, so each trial must use its observed DHCP address.
Authenticated commands, the service-listener regression and negative control,
the ten-second platform probe, and a locked 64 MiB/eight-worker memory check
passed. This is the thirteenth native startup observed with the cache pre-clean
change across the recorded development trials, not sustained qualification.

The 8 MiB upload passed with the expected checksum in 19.35 seconds. During the
following download, NanoKVM again stopped answering LAN SSH and ARP; UART capture
and the independent controller-health SSH stream also disconnected. The router
remained reachable. The 65,536-byte incoming file is retained but unaccepted.
Live executable upload/execution, reboot persistence and normal power-off were
not reached. The session is `recovery_failed`, not a full native gate pass.

Controller samples in `artifacts/controller-health/20260911T212845Z-534dae`
ended at 21:34:08 UTC. Across 64 samples, available memory stayed above
50,912 KiB and the highest reported temperature was 44.095 degrees Celsius.
The last sample had 60,736 KiB available. These five-second snapshots do not
exclude a sudden failure between samples or identify its cause.

Before another Haiku download trial, compare NanoKVM-to-workstation SSH traffic,
ROOBI-to-NanoKVM USB traffic, and the combined relay separately while ROOBI runs
from eMMC. A local comparison script is prepared but has only passed Python
syntax/CLI checks; its hardware cases have not run. Sipeed documents an optional
[controller watchdog](https://wiki.sipeed.com/hardware/en/kvm/NanoKVM/user_guide.html).
Its installed behavior and recovery operation still need verification. The
pending work is indexed locally by `state/controller-outage-next.json`.

Inspection of the vendor's 2.4.3 source clarifies that this documented watchdog
is a userspace service monitor: `kvm_system` checks a heartbeat file each second
and calls `system("reboot")` after more than ten missing checks. It does not arm
a hardware watchdog in that loop. Thus enabling the documented flag alone
cannot establish recovery from a whole-kernel hang; a LAN-only failure may also
leave the monitored service alive. See the pinned
[watchdog loop](https://github.com/sipeed/NanoKVM/blob/3b2ba7c0c1214f44da9d328f90bbdd025fac0413/support/sg2002/kvm_system/main/src/main.cpp#L238-L256)
and [heartbeat check](https://github.com/sipeed/NanoKVM/blob/3b2ba7c0c1214f44da9d328f90bbdd025fac0413/support/sg2002/kvm_system/main/lib/system_state/system_state.cpp#L506-L525).
The installed `/dev/watchdog*` devices, driver identity, timeout and clean
disarm behavior must be inspected on the controller before planning a hardware
watchdog trial. No watchdog setting has been changed.

## Controller watchdog and transfer isolation

Subsequent inspection found an installed `soph_wdt` hardware driver exposing
`/dev/watchdog0`, with `nowayout=N` and a device-tree reset connection. The
installed module's start, timeout and stop disassembly matches the relevant
paths in the pinned [vendor implementation](https://github.com/sipeed/LicheeRV-Nano-Build/blob/d4003f15b35d43ad4842f427050ab2bba0114fa5/osdrv/interdrv/v2/wdt/wdt.c).
Module SHA-256 is
`05d687320c7a7872281b230b0528ab42d15bd9065828ac3a1730a9584c4a4f2d`.
A requested 30-second timeout reports 21 seconds through `GETTIMEOUT`, while
`GETTIMELEFT` immediately after feeding reports 30. Tests use frequent
acknowledged heartbeats and do not assume the reported timeout is exact.

| Watchdog check | Evidence under `artifacts/controller-watchdog/` |
| --- | --- |
| Enable, feed, disable and remain alive for 35 seconds afterward | `20260911T215941Z-d9eb06` |
| Deliberately stop feeding; NanoKVM returns with a new boot ID in 50.6 seconds, ROOBI's boot ID unchanged | `20260911T220112Z-91d9cb` |
| Acknowledged host heartbeats keep it alive for 40 seconds, followed by clean disarm | `20260911T220318Z-c2d2f0` |
| Stop host heartbeats; NanoKVM returns in 68.3 seconds without a software reboot command | `20260911T220551Z-c7462a` |

The temporary guard is scoped to individual tests; no watchdog startup service
was installed. It leaves the hardware timer armed if the SSH heartbeat fails.
An alternate SSH route through ROOBI's LAN connection to NanoKVM's USB address
was also verified while ROOBI ran from eMMC.

Three 8 MiB Linux comparisons passed with matching checksums and unchanged
controller/ROOBI boot IDs: NanoKVM-to-workstation SSH in 3.30 seconds,
ROOBI-to-NanoKVM USB with local hashing in 4.50 seconds, and the combined relay
in 5.04 seconds. Evidence is in `artifacts/controller-baseline/` directories
`20260911T220423Z-67d337`, `20260911T220441Z-cc2f73`, and
`20260911T220510Z-46eed5`. SSH compression was disabled.

Native session `artifacts/interactive/20260911T220734Z-efae81` passed the
service/platform/memory checks, a 141,227-byte executable upload followed by
execution, the 8 MiB upload, and an 8 MiB USB-only download hashed on NanoKVM.
The combined USB/SSH relay then reproduced the controller outage, leaving an
unaccepted 1,507,328-byte partial file. Normal reboot and power-off were not run.
The hardware watchdog restored NanoKVM with a new boot ID. Initial automatic
ROOBI recovery failed because SSH became available before the web API; the
local wrapper now waits for API readiness. Recovery subsequently passed with
125,417 captured UART bytes in
`artifacts/controller-recovery/20260911T221542Z-73a13b`.

This narrows the failing workload, without identifying the underlying defect.
No kernel panic message reached the NanoKVM kernel-log capture before its SSH
stream ended, and the guard's failure diagnostic was absent after reboot.
Available memory stayed above 53,792 KiB in 67 five-second samples; their maximum
reported temperature was 42.347 degrees Celsius. These observations do not rule
out a sudden failure between samples.

The lab's default download transport now receives and flushes a unique NanoKVM
scratch file before copying it to the workstation. It verifies the receipt's
length and SHA-256, then the guest's before/after checksum, and removes the
scratch file on success. The original relay remains an explicit diagnostic
option. All 35 host checks pass, including real local receiver processes for
successful, truncated and corrupted staged transfers. A staged 8 MiB ROOBI
comparison passed in 13.95 seconds and removed its scratch file:
`artifacts/controller-baseline/20260911T222015Z-8a64b1`.

In native session `artifacts/interactive/20260911T222106Z-571446`, the service,
platform and memory checks passed again. The 8 MiB upload took 18.24 seconds and
the first staged download passed in 22.34 seconds, with its scratch file removed.
A short locked 8 GiB memory test passed before normal PSCI reboot. The new boot
accepted authenticated commands and verified the persisted file's checksum.
The second staged download then caused another controller outage during its
USB-receive stage, before copying to the workstation. Thus simultaneous bulk
Ethernet traffic is not required, and staging alone does not solve the problem.
Normal power-off was not reached.

This time the watchdog and API-readiness wait completed recovery automatically:
NanoKVM returned with a new boot ID, a fresh UART capture recorded 125,586 bytes,
and ROOBI became reachable. Evidence is in
`artifacts/controller-guarded-native/20260911T222106Z-5b6ccb/controller-recovery.json`.
The native test remains an error despite successful recovery. The verified
guard and wrapper are now available as `controller_watchdog.py` and
`guarded_session.py` in the lab tools.

Staged reception now also limits its TCP receive window and paces data at
256 KiB/second. Its 35 host checks pass, and an 8 MiB Linux comparison passed
in 45.41 seconds in `artifacts/controller-baseline/20260911T224323Z-05a9b3`.
An earlier paced attempt, `20260911T224132Z-15138a`, failed its ROOBI SSH
prerequisite before transferring data and is retained separately. The paced
transport has not yet passed native qualification; no controller driver fix
is claimed.

## RNDIS Ethernet frame bounds

Source revision `3b9f2720e554f601ef81239643a7f2c5dba16f9e` corrects the size
reported through `ETHER_GETFRAMESIZE`: RNDIS's
[maximum-frame-size query](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/oid-gen-maximum-frame-size)
excludes the Ethernet header, while Haiku's interface expects that header to be
included. The driver also validates each received message and destination
capacity before copying, using byte-wise little-endian decoding for unaligned
buffers. Invalid or incomplete messages discard the remaining batch without
copying data. A missing or unusable maximum frame size now fails device open.

All 36 host checks passed, including the actual packet extraction code under
AddressSanitizer and UndefinedBehaviorSanitizer: full 1514-byte frames,
insufficient destination capacity, truncated messages, length/offset limits,
unaligned buffers and padded packet batches. Evidence is
`artifacts/control-checks-rndis-framing.log`. The clean ARM64 build produced
base image SHA-256
`7434a37c28d8ec4644f44a06a9eed284ebefad4ddcf900aee457d3ce8d5e4cf5`.
Its private lab image has SHA-256
`6fcb9cbb51a2258bc029159403425490074686d3c2fa88782724e346c49ae23c`.
The full QEMU suite passed in `artifacts/qemu-shell/20260911T230644Z-a4d84e`,
including the 8 MiB round trip, normal reboot and normal shutdown.

The first native trial, `artifacts/interactive/20260911T230930Z-43f108`,
booted and reported MTU 1500 instead of the previous 1486. Its command script
then failed with status 127 because the minimal image lacks `grep`; subsequent
checks did not run. Automatic ROOBI recovery succeeded and the controller
watchdog disarmed normally. The retry uses a shell built-in for the MTU check.
The NanoKVM transfer outage remains a separate unresolved issue.

The retry in `artifacts/interactive/20260911T231505Z-d3c156` passed the MTU,
service, eight-CPU platform and 64 MiB memory checks. Normal shutdown reached
PSCI SYSTEM_OFF, the USB link went down, and the remote power button started a
fresh Haiku boot that passed authenticated checks. NanoKVM kept the same boot
ID through that cycle. Its screenshot retained the previous desktop image;
the screenshot alone does not demonstrate HDMI signal loss. Power evidence is
recorded in the run's `normal-power-off.json`.

An 8 MiB upload passed in 28.60 seconds. A staged download paced at 256 KiB/s
passed in 54.03 seconds. After a short locked 8 GiB memory check and normal
PSCI reboot, the persisted file checksum, MTU and platform/memory checks passed
again. A second paced download passed in 53.79 seconds, and an unpaced staged
download passed in 22.11 seconds. All returned files matched SHA-256
`13d3d2a1d7eee5c4ff92996af3a186741320b02dd95451eaff323772fb966d95`.
These are bounded successful tests, not sustained transfer qualification.

The original simultaneous relay then reproduced the controller outage,
leaving an unaccepted 1,638,400-byte partial file. The watchdog heartbeat ended,
and this time NanoKVM did not return with a new boot ID within the 150-second
recovery gate. ROOBI was also unreachable and no Rockchip USB recovery device
was visible. The session ended `recovery_failed`; a physical NanoKVM power cycle
was requested. Evidence is in
`artifacts/controller-guarded-native/20260911T231505Z-09a084/controller-recovery.json`.
The watchdog's earlier successes do not establish recovery from every outage,
and the RNDIS bounds correction does not fix this relay failure.

## ARM64 instruction-cache synchronization

Source revision `e10bf161c52aa09d566216f3ab1be2bbe3a1f844` corrects a shift
expression in `arch_cpu_sync_icache()`. Instruction-cache line size must use
the low four bits of `CTR_EL0`; the old expression shifted the register value
instead of masking it, producing an invalid C++ shift count. The change uses
small shared decoding helpers and adds compiler memory clobbers to the existing
cache-maintenance barriers. The architectural sequence remains data clean,
barrier, instruction invalidate, barrier and instruction synchronization, as
described in [Arm's cache-maintenance explanation](https://developer.arm.com/community/arm-community-blogs/b/architectures-and-processors-blog/posts/caches-self-modifying-code-implementing-clear-cache).

All 37 host checks passed in `artifacts/control-checks-arm64-cache.log`. The
cache test compiles the actual decoding helpers with UndefinedBehaviorSanitizer
and checks explicit register values with different instruction/data line sizes.
Substituting the old expression into a temporary copy makes that same test fail
with an excessive-shift diagnostic; evidence is in
`artifacts/arm64-cache-regression/`. The source helpers remain corrected.

The clean build has base-image SHA-256
`54ca9b7cf76eed624e6d47e753c903ac002dae3558461d082c5b1dde4b42d01a`
and private-image SHA-256
`8c32202ee225e6aafc36238406b537ee847af8f01c5295be5f60691a49751327`.
The full QEMU suite passed in `artifacts/qemu-shell/20260911T234507Z-90b6ce`,
including 8,192 instruction-replacement checks across four CPUs, the memory,
platform and service probes, the 8 MiB transfer and negative controls, normal
reboot, fresh login and normal shutdown.

The new `rock5_cache_probe` writes functions crossing 64-byte boundaries and
one page boundary, calls `clear_caches()`, and verifies their changed return
values after pinning the thread to each CPU. Each executing CPU performs its
own instruction synchronization; code is never modified concurrently with its
execution. See [Arm's discussion of cross-CPU instruction synchronization](https://developer.arm.com/community/arm-community-blogs/b/architectures-and-processors-blog/posts/caches-self-modifying-code-working-with-threads).
QEMU does not prove physical cache coherence. Native validation remains pending:
the owner reported power-cycling NanoKVM, but its saved LAN address remained
unreachable and its saved mDNS name did not resolve at the subsequent check.
No new controller boot ID or native test result has yet been observed.

## Controller recovery and installed NVMe fixture

NanoKVM subsequently returned at its saved LAN address with boot ID
`84b69ae8-4d9e-4c03-9f5c-6c9547084819`. Recovery with UART capture passed in
`artifacts/controller-recovery/20260911T235338Z-d1dc1a`, recording 130,051 bytes
without capture errors and ROOBI boot ID
`907709b6-075c-481e-9863-b42af8a54ef6`. The initial screenshot showed the previous
Haiku session in `last transaction (7) still open!` during a syslog/BFS write.
The capture gap does not establish whether that panic preceded or followed
the controller outage/restart, or explain the controller failure.

Read-only Linux inventory in `artifacts/nvme-inventory/20260911T235503Z-f4fa6c`
identifies `Samsung SSD 950 PRO 256GB`, firmware `1B0QBXX7`, at PCI address
`0000:01:00.0` (`144d:a802`). The namespace is 256,060,514,304 bytes with
512-byte logical/physical sectors. Linux reports an 8.0 GT/s, two-lane link;
the SSD advertises a four-lane maximum. This records the observed configuration,
not a diagnosis of lane sharing or a throughput measurement.

The SSD has an unmounted 1 GiB FAT partition and a 254,984,323,072-byte Btrfs
partition. Neither was mounted or modified by the inventory. ROOBI continues
to run from eMMC with kernel `5.10.110-33-rockchip`; Haiku support and SSD
functional acceptance remain open. ROOBI's clock was approximately 37 minutes
behind the workstation during inventory, so artifact names use workstation UTC.

After inventory, the owner explicitly authorized discarding all existing data
on this SSD, using it for testing, and eventually installing Haiku on it. This
drive is now an available scratch fixture; that authorization does not establish
native driver support or a completed installation.

## Native cache validation and SSD reference patterns

The cache-fix image subsequently passed a bounded native trial in
`artifacts/interactive/20260911T235552Z-f9a804`. Before and after normal reboot,
all eight CPUs passed 32-round and 256-round cache probes: 16,384 and 131,072
instruction checks per boot, or 294,912 checks with zero mismatches overall.
The service regression, ten-second platform probe and 64 MiB memory check
passed on both boots. A locked 8 GiB/eight-worker/two-pass memory check passed
in 3.52 seconds before the reboot. UART recorded the normal reset request and
two independently started authenticated Haiku sessions. The inspected initial
screenshot shows Tracker and Deskbar.

ROOBI recovery passed with boot ID `79c7c397-b254-49a8-8fcf-045c2b94fa52`.
UART capture saved 232,353 bytes without errors, and the scoped controller
watchdog disarmed normally. All 144 five-second controller samples retained
the same NanoKVM boot ID; minimum available memory was 46,756 KiB and maximum
reported temperature was 44.095 degrees Celsius. Evidence is in
`artifacts/controller-health/20260911T235545Z-e59823`. The collector was stopped
manually after recovery; its interrupted SSH exit is not an outage. This trial
did not exercise bulk downloads or establish sustained cache/memory acceptance.

With the owner's erasure authorization, the physical Samsung SSD then passed
a bounded Linux direct-I/O test in
`artifacts/nvme-linux-baseline/20260912T000649Z-0d883f`. The test checked the
model, serial, firmware, size and absence of mounted partitions/holders before
opening the exact device exclusively. It wrote and flushed two 8 MiB patterns
at offsets 2 GiB and 5 GiB, then read them through `O_DIRECT`; both SHA-256
values matched. The patterns and offsets remain recorded for future native
Haiku read comparisons. Existing filesystem data is disposable; this test
overwrote 16 MiB but did not alter the partition table. These small transfers
do not establish sustained throughput or power-loss durability.

## Initial ARM64 NVMe emulation checks

An exploratory [QEMU NVMe device](https://www.qemu.org/docs/master/system/devices/nvme.html)
with a separate 8 GiB sparse namespace exposes the existing ARM64 driver as
`/dev/disk/nvme/0/raw`. This exercises generic PCI/NVMe independently of the
RK3588 PCIe host, which remains unimplemented. The local test seeds distinct
8 MiB patterns at offsets zero and 4 GiB; all virtual disks and evidence remain
under `/mnt/HaikuWork`.

The first run, `artifacts/qemu-nvme/20260912T000322Z-e48924`, read both initial
patterns and completed a checked 8 MiB write with `fsync`, then stalled during
another readback into an existing BFS file. Host inspection after QEMU stopped
confirmed the written region's expected checksum. The complete test failed its
180-second command deadline; it did not reach reboot or shutdown validation.
Its process listing retained a running `dd` command.

A second run, `artifacts/qemu-nvme/20260912T001016Z-e5a4bf`, streamed read data
directly into the checksum program and stalled on the first bulk read, before
any test write. This shows that rewriting an existing BFS destination file is
not required for the stall. Both runs retain their unchanged source image,
command script, serial log and failed result. Device enumeration and individual
successful transfers do not establish NVMe readiness; the blocked thread's
kernel stack was the next diagnostic target.

## ARM64 NVMe interrupt starvation and polling fallback

Two further diagnostic runs localized the stall. In
`artifacts/qemu-nvme/20260912T002241Z-b3286d`, the writing thread was running on
CPU 0, stopped immediately after `ConditionVariableEntry::Wait` re-enabled
interrupts. Its caller was NVMe `await_status`. Switching KDL to the actual CPU
was necessary: ARM64's current `bt` command ignores its advertised thread-ID
argument. Matching kernel/driver symbols and all CPU stacks are preserved.

The driver used legacy INTx without acknowledging the completion queue in its
interrupt handler. The waiting thread normally consumes that queue, but a
reasserted level interrupt can prevent it from returning to the polling code.
The [QEMU 8.2.2 NVMe model](https://github.com/qemu/qemu/blob/v8.2.2/hw/nvme/ctrl.c)
asserts the pin for pending completions and deasserts it when they are consumed.
This supports interrupt starvation as the cause, rather than a locked condition
variable or a requirement to overwrite a BFS destination file.

The ARM64 candidate keeps PCI INTx disabled and uses polling when MSI/MSI-X
cannot be configured. It installs no handler for that polling path, records
the actual installed interrupt vector for teardown, and caps polling backoff
without an unbounded shift. Successful MSI/MSI-X setup retains interrupt mode.
Polling is a compatibility fallback with a performance cost; controller-stall
recovery and native MSI routing remain separate work.

The candidate passed the entire original reproduction in
`artifacts/qemu-nvme/20260912T003443Z-c825f9`: both initial reads, an 8 MiB write
with `fsync`, readback before and after normal reboot, normal power-off and
independent host checksums. Its private image SHA-256 is
`c9ef547b9da38799203c4a245ad8707663d06e8bd90c132554c4f7338d353c7f`;
the manifest records the development patch against `97ba616c21`.
The serial log reports polling on both boots. This is emulated NVMe validation,
not Samsung SSD access or native RK3588 PCIe support.

`qemu_shell.py --nvme --power --normal` now provides this regression as a regular
gate with its own disposable namespace. Thirty-nine host checks pass, including
rejection of an unwritten or misplaced high region, corrupted data and a
truncated namespace. The clean source is
`2f2efd5c51d8ce40a514b4d7cf408605a6f16e50`; its private image SHA-256 is
`adae21d51f65d81a3d8edb15e3de88f2314c070367d5373dbc03c56eeaec1ddd`.

The first combined clean-image run,
`artifacts/qemu-shell/20260912T004022Z-8a2cc1`, failed before executing guest
tests: RNDIS initialization timed out waiting for its control notification.
That failed result remains preserved. A diagnostic repeat with QEMU xHCI
startup tracing passed in `artifacts/qemu-shell/20260912T004506Z-be4600`, including
memory and its negative control, platform, cache, service regression, 8 MiB USB
round trip and truncated-input rejection, all NVMe checks, normal reboot and
power-off. The trace is in
`artifacts/qemu-usb-notify/20260912T004506Z-9aafbc`. This successful repeat does
not explain or fix the intermittent RNDIS startup failure.

A separate run booted the same clean image directly from emulated NVMe, with
no USB boot disk. `artifacts/qemu-shell/20260912T004328Z-4c3846` records
`Mounted boot partition: /dev/disk/nvme/0/1` on both boots. Memory, cache, the
USB file round trip and negative checks, normal reboot and power-off passed.
The exploratory wrapper initially rejected the log because it counted the
SSD serial number in firmware messages as well as driver messages. The saved
`nvme-boot-review.json` checks the exact driver and mounted-partition markers;
the original wrapper error is retained. This qualifies a basic emulated BFS
boot through the NVMe driver, not installation or boot from the physical SSD.

## Read-only native UEFI PCI inventory

The extended EFI diagnostic reads UEFI PCI I/O and PCI Root Bridge I/O protocols.
It records device locations, the first 256 configuration bytes and the root
resource descriptors. QEMU validation in
`artifacts/efi-pci-qemu/20260912T005250Z-cde6bb` checked the emulated NVMe identity
and saved configuration bytes and verified that its separate virtual SSD was
unchanged. The source files and development patch against `0ca44d8dc3` are
preserved there. The tested diagnostic image SHA-256 is
`9c838892b82f8fac7fdd0a57832624190398a4c2455cf60cc3da76244852e019`;
the EFI application SHA-256 is
`532215cebc61b8fa4221a1b7a9510608299618f47a52ddda231b04362e16e76d`.

The same image passed a native deployment/inventory/recovery cycle in
`artifacts/hardware/20260912T005517Z-8be452`. Firmware exposes four PCI root
bridges and fourteen PCI protocol handles. Eight handles have valid vendor IDs:
four RK3588 bridges, Samsung NVMe, ASM1164 SATA and two RTL8125 controllers.
The other six report vendor ID `ffff` and are not counted as physical functions.
The Samsung is at segment 0, bus 1, device/function 0 with ID `144d:a802`,
class `010802`, revision 1 and PCIe link status reporting 8.0 GT/s, two lanes.
Its BAR0 is assigned `0xf0000000`; firmware reports a 2 MiB memory aperture
starting there for root segment 0. This differs from the saved Linux/mainline
device-tree resource arrangement. A Haiku handoff must account for the actual
firmware setup instead of treating the Linux device tree as live PCI mappings.

All device snapshots were read back from the copied USB image and matched the
logged identities. The copy's SHA-256 matched the detached NanoKVM file.
UART captured 186,546 bytes without errors; ROOBI returned with boot ID
`ed4cce1a-9c0f-4fe3-b64c-f532a8afcc51`. No PCI configuration, SoC register,
SSD, eMMC or SPI writes were requested by this inventory. The probe's files
were written only to its USB image. This establishes firmware discovery,
not native Haiku PCIe discovery or physical NVMe I/O under Haiku.

## Native PCI configuration after the firmware handoff

The installed EDK2 v1.1 source (commit
`6a682c0ef3ed74feb8b0d98f1c2aa771ddfbae18`) uses separate RK3588 root/endpoint
configuration mappings and filters invalid device slots. For segment zero,
the root is at `0xa40000000` and bus 1/device 0/function 0 is at `0x900100000`.
These are firmware mappings, not the Linux device tree's configuration window.
The new `rock5_pci_config_probe` reads only those two known functions through
read-only, uncached Haiku mappings. Its generated ARM64 loop uses individual
32-bit volatile loads. It does not write PCI configuration or program SoC
registers, clocks, address windows or DMA.

The first QEMU run, `artifacts/qemu-shell/20260912T011650Z-86bb19`, failed before
mapping any PCI page. The `poke` driver was packaged but could not load because
it required an ISA module absent from the ARM64 image. Source
`20a934e26caf2a1445af826382fc40dd88f5a5c0` makes its ISA/PCI module dependencies
optional, rejects operations requiring an unavailable bus and retains root-only
access. The initial failed evidence remains preserved.

The clean private image has SHA-256
`ec7d1ef97100b754112881f81b7a9b8f2d7fa04a751d6f13a8c2c50e0554b374`.
QEMU passed in `artifacts/qemu-shell/20260912T012011Z-d2825f`: host/NVMe
configuration reads before and after normal reboot, the separate NVMe fixture's
read/write/reboot/host-hash checks, 64 MiB memory with its negative control,
8,192 cache checks and normal power-off.

The same image passed the native probe before and after normal reboot in
`artifacts/interactive/20260912T012245Z-e698d4`. All 256 bytes of each root and
Samsung configuration snapshot matched the earlier UEFI inventory on both
boots. The Samsung retains ID `144d:a802`, BAR0 `0xf0000000` and PCIe 3.0 x2.
Both boots also passed a locked 64 MiB/eight-worker/two-pass memory check.
The inspected screenshot shows Tracker and Deskbar. Haiku's disk directory
still contains only USB and virtual devices: this proves configuration access
after ExitBootServices, not a PCI host driver or native SSD I/O.

ROOBI recovery passed with boot ID `71c3342d-95d3-421f-afb1-68d15e737dfb`.
UART saved 251,544 bytes without transport errors. The controller retained
boot ID `84b69ae8-4d9e-4c03-9f5c-6c9547084819` and its scoped watchdog disarmed
normally. `pci-config-review.json` preserves the parsed comparisons and raw
configuration files. Firmware handoff support, noncoherent DMA and PCIe
interrupt routing remain open before native disk qualification.

## ARM64 NVMe DMA buffer isolation

Source `6b8a1be864` adds a conservative ARM64 DMA path. Queue entries, PRP lists
and transfer buffers use private Normal Non-cacheable mappings. Allocation
cleans and invalidates the initial cached zeroing before changing the mapping,
as in the previously tested platform EHCI allocator. Device payloads use
preallocated 128 KiB buffers owned by command trackers, including admin
identification data and scattered block-I/O data. Copying follows the opcode's
transfer direction and preserves split-request offsets. Metadata payloads are
rejected rather than sent through an unimplemented metadata mapping.

The change adds full-system ordering before MMIO writes and after observing a
completion phase tag, reads that tag through a volatile access and bounds-checks
completion IDs. Coherent allocation alone would not provide ordering; see the
[Linux DMA guide](https://docs.kernel.org/core-api/dma-api-howto.html) and the
[Linux NVMe completion barrier](https://github.com/torvalds/linux/blob/v6.6/drivers/nvme/host/pci.c).
Eight data buffers are reserved per I/O queue and sixteen per admin queue;
the admin count leaves room for asynchronous event requests. This limits memory
use and avoids allocating new DMA areas while submitting I/O. Copying and the
smaller transfer/queue limits have a performance cost. This does not implement
an IOMMU, general PCI DMA translations, controller-stall recovery or per-device
coherence policy; other CPU architectures retain their existing payload path.

The development candidate passed the original NVMe checks in
`artifacts/qemu-shell/20260912T013856Z-ae3b4a`. Its recorded source patch and new
header are in `artifacts/nvme-dma-source/20260912T013855Z-ce6d9f`.
All forty host checks passed in `artifacts/control-checks-nvme-dma.log`.
The clean private image SHA-256 is
`6d1096c0fe01c5449e2a0df609ee63d263f6a2725e600b564db77b125d3a71af`.

The clean image passed in `artifacts/qemu-shell/20260912T014338Z-47339f`:
the original 8 MiB reads/write, five unaligned writes of 1, 513, 4,097,
131,073 and 1,048,579 bytes above 6 GiB, and complete 2 MiB surrounding-region
hashes after every write. All three regions matched again after normal reboot
and through independent host reads after normal power-off. The same run passed
the memory negative control, 8,192 cache checks and PCI configuration probes.
Serial records the noncached buffer pools on both boots.

`artifacts/qemu-shell/20260912T014540Z-839cbc` then booted the clean image directly
from emulated NVMe, with no USB boot disk. Both boots mounted
`/dev/disk/nvme/0/1`; memory/cache, an 8 MiB USB round trip and its truncated-input
check, normal reboot and power-off passed. These are functional emulation
checks. Physical Samsung DMA, storage performance and native SSD boot remain
untested until the firmware PCIe host handoff is implemented and qualified.

## First native Samsung NVMe I/O

Source `2d9159afce62b19faadb086e01a3da23ce85a7be` adds the explicit
[EDK2 v1.1 host profile](PCIE-FIRMWARE.md). It exposes only firmware segment 0,
retains the firmware's PCIe setup and checks the root/SSD configuration and
active link before the PCI core attaches. The existing Linux DT windows are
not used as live mappings. The opt-in setting is installed by the lab build.
Other PCIe ports, port I/O and MSI/INTx routing are not part of this profile.

All 41 host checks pass, including malformed profile/configuration cases under
sanitizers and the profile executable run against both captured firmware
configuration files. The clean private image SHA-256 is
`31be3e08dcb649c3ecc1d60b5afd1cbb89033c3d343157c1a9b041ed0989bee3`.
QEMU passed the NVMe sector guards, original I/O, normal reboot/readback,
independent host hashes, memory/cache and PCI config checks in
`artifacts/qemu-shell/20260912T021806Z-3cea25`.

On the ROCK, `artifacts/interactive/20260912T022104Z-706c6a` shows Haiku's
driver attaching to the Samsung 950 Pro on both boots, publishing
`/dev/disk/nvme/0/raw`, identifying the 512-byte namespace and using polling.
The 16 admin and 8-by-8 I/O tracker buffers use the ARM64 noncoherent path.
BAR0 remains CPU/PCI address `0xf0000000`, size 16 KiB. Tracker/Deskbar and the
USB shell remained usable; the eight-worker locked 64 MiB memory check passed.

The first native reads matched both independently generated Linux 8 MiB
fixtures at 2 GiB and 5 GiB. Haiku then wrote 8 MiB at 5 GiB using `dd conv=fsync`, seeded
a 2 MiB surrounding-byte test region at 7 GiB, and performed writes of 1, 513,
4,097, 131,073 and 1,048,579 bytes at unaligned offsets in that region. The full
2 MiB hash matched the host-generated expected bytes after each operation.
All three regions matched before and after normal Haiku reboot. After recovery,
Linux `O_RDONLY|O_DIRECT|O_EXCL` reads independently matched all three hashes.
The final surrounding-region hash is
`91d744cbe2803fbd788226912c73d56102634e1dbafe071cb70ecfa4da80f582`.
Expected bytes, scripts and Linux results are in
`artifacts/native-nvme-fixture/20260912T022046Z-338b9d`.

This is bounded physical read/write and warm-reboot evidence. Raw-device
`fsync()` did not flush the drive cache; see the correction below. It does not
qualify sustained performance, concurrent queues, controller-error recovery,
TRIM, high physical DMA addresses, native SSD boot or power-loss durability.
The logged DMA buffer addresses were below 4 GiB; large disk offsets do not
establish high-memory DMA. Both boots still used the NanoKVM USB boot volume.
The SSD's partition table is unchanged and its filesystem contents are disposable.

Recovery returned ROOBI boot ID `5a2d044f-2ea1-4d8b-9df5-ae5c6730bf40`.
UART captured 249,402 bytes without transport errors; the controller retained
boot ID `84b69ae8-4d9e-4c03-9f5c-6c9547084819` and its scoped watchdog disarmed.
`nvme-review.json` in the native artifact records the reviewed transcripts.

The same clean image subsequently booted directly from emulated NVMe on both
boots in `artifacts/qemu-shell/20260912T023150Z-6008aa`. Both kernel mounts were
`/dev/disk/nvme/0/1`; memory/cache, USB transfer and truncated-input checks,
normal reboot and power-off passed. This gates the first native SSD boot image.

## Native Samsung SSD development installation

The owner authorized erasing the entire Samsung drive. Installation evidence
in `artifacts/nvme-install/20260912T024056Z-90ec70` records the exact model,
serial and capacity checks, removal of its old GPT, and writing the tested
336 MiB private image above. A full Linux `O_DIRECT` readback matched the
source SHA-256. The resulting MBR contains a 32 MiB EFI partition and a
300 MiB BFS partition; most of the 256 GB drive remains unallocated. This is
a development installation, not a full-drive installation or release image.

Both physical trials used a one-time UEFI `BootNext` option with the full
Samsung namespace device path. The ordinary `BootOrder` stayed unchanged and
NanoKVM kept the ROOBI recovery image selected. Haiku's boot volume therefore
had to come from the SSD; UART and `df` both report `/dev/disk/nvme/0/1`.
The bootstrap image and firmware loader are unchanged from the QEMU gate.

- `artifacts/interactive/20260912T024625Z-d59135`: native SSD desktop and shell;
  eight-worker locked 64 MiB memory check; 16,384 instruction-cache checks;
  an 8 MiB file written and flushed to BFS. Normal Haiku reboot returned to
  ROOBI after firmware consumed the one-time boot option.
- `artifacts/interactive/20260912T025256Z-8dd22a`: a second one-time SSD boot
  read that existing file with matching SHA-256
  `e2641c6d7df3dc39fe21e02b32825f37ba0b6f76fa908b269882d4b917580abf`.
  The memory check passed again. Normal Haiku shutdown reached
  `PSCI: requesting system off`; a NanoKVM power-button command then started
  fresh DDR/SPL/EDK2 output and returned ROOBI.

The two UART captures contain 313,122 and 311,779 bytes with no transport
errors. HDMI capture timed out while powered off, and a short burst of
undecodable UART bytes followed the power-off marker. NanoKVM SSH remained
reachable with controller boot ID `84b69ae8-4d9e-4c03-9f5c-6c9547084819`.
Both scoped watchdogs disarmed. Final recovery boot ID is
`4b3a06db-b95e-4257-ad0e-ea291e12d62f`; review assertions and the per-trial
results are saved as `native-boot-review.json` in the installation evidence.

The persisted file was checked after reboot, before the subsequent power-off.
Concurrent/sustained I/O, controller-error recovery, TRIM, DMA above 4 GiB and
power-loss durability remain unqualified. The firmware-specific PCIe and
polling limitations still apply. BFS resizing is currently unimplemented, so
using the rest of the SSD requires a new larger filesystem and a proper copy
or installation, rather than growing this 300 MiB volume in place.

## Concurrent native NVMe I/O and USB control failure

The standalone `rock5_nvme_stress` probe at source
`726258c8d56e6744cb69a7529a0c308bc6e0905e` uses eight workers pinned across the
available Haiku CPUs. Each writes offset- and round-dependent patterns in
1 MiB requests, followed by raw-device `fsync()` and cross-worker reads in
reverse block order. That `fsync()` was a no-op, as diagnosed below. Host checks verify independent expected bytes, unchanged surrounding
data, read-only verification and corruption detection. The ARM64 binary SHA-256
is `85f353259e6d8eea216fed6da66d79ef1dc6a076827d0bb2e0b8ccae4d974f33`.
QEMU `artifacts/qemu-shell/20260912T031611Z-42d3cf` passed two rounds over
128 MiB, CPU placement, reboot readback and independent backing-file comparison.
The kernel/driver image remains the earlier SHA-256 `31be3e08...0989bee3`.

On the physical SSD, `artifacts/interactive/20260912T032043Z-2037fd` ran four
rounds over the unallocated 16–18 GiB range: 8 GiB written and 8 GiB verified,
with one worker on each of the eight CPUs. Every round and both 1 MiB guards
passed. The test took 35.409 seconds; each 2 GiB write phase took about
2.35 seconds and each verification phase about 6.50 seconds. These timings
include pattern generation/comparison and describe this bounded workload,
not a sustained performance qualification. The existing BFS file also matched
after the earlier normal power-off and power-button startup.

After the storage command completed, Haiku's EHCI/RNDIS/HID paths reported
USB transaction/checksum errors. A new shell connection closed before it
could request normal reboot. This trial remains **error**, despite its passing
storage transcript. It recorded 1,547 `Device check-sum error` messages.
NanoKVM SSH and watchdog heartbeats continued, with the same controller boot
ID. External recovery returned ROOBI `2ccc97af-b233-4b82-b251-e98e7049c1d6`;
the watchdog disarmed and UART transport itself stayed intact. NanoKVM's saved
kernel log includes DWC2 endpoint-stop timeouts during recovery. The cause of
the USB failure is unresolved; this does not establish that NVMe load caused it.

ROOBI then independently read all eight 256 MiB regions and both guards through
`O_RDONLY|O_DIRECT|O_EXCL`. Every hash matched the host-generated expected data.
The plan, source, binary, reference hashes, Linux scripts and results are in
`artifacts/nvme-stress/20260912T031610Z-16369c`. The obsolete 336 MiB installation
staging file was removed from ROOBI after its hash was verified; the immutable
workstation image remains available.

A subsequent SSD boot, `artifacts/interactive/20260912T032858Z-dc3cac`, read
the same 2 GiB from Haiku in 6.625 seconds, with all eight CPU placements,
patterns, guards, the probe executable and the BFS file checked. Normal Haiku
reboot returned ROOBI `973dfc55-a390-4c3f-b1bd-b7bce69e3392`. No USB checksum
errors appeared through that reboot. These are passing bounded concurrent
storage and reset-readback observations. Explicit flush qualification was
missing; the subsequent high-address DMA failure is recorded below. USB control
stability, longer
mixed workloads, high physical DMA addresses, error recovery, TRIM and
full-size SSD installation remain open.


## High-address NVMe trial and raw-device flush correction

Source `215a43a021913435a29f4b009fdff0af415d9fbb` adds an opt-in ARM64
allocation floor above 4 GiB, described in [PCIE-FIRMWARE.md](PCIE-FIRMWARE.md).
The private diagnostic image SHA-256 is
`a7fe3ac018dcffae29a9b7e7234e7cc7057dfb5dab394d5ba810c5d9fc202eb0`.
QEMU with 6 GiB RAM passed in `artifacts/qemu-shell/20260912T033950Z-a5ad84`,
with all ten logged buffer pools above 4 GiB across two boots. With only 2 GiB
RAM, `artifacts/qemu-shell/20260912T034228Z-1a1779` confirmed allocation failure,
failed namespace reads and unchanged backing data, while USB boot and reset
remained functional. A lazily published device entry can still exist after
attachment fails; successful I/O, rather than entry absence, is the criterion.

The native USB-boot trial `artifacts/interactive/20260912T034602Z-eda8a5`
recorded all eighteen buffer pools above 4 GiB across two boots. The same
8-worker probe `85f35325...d974f33` wrote and immediately verified four rounds
over 16–18 GiB, totaling 8 GiB each way, in 35.417 seconds. The surrounding
1 MiB guards matched. However, readback after normal Haiku reboot **failed**.
Independent Linux direct reads confirmed stale disk data. A complete read-only
2 GiB snapshot, its transfer/hash checks and sector analysis are retained in
`artifacts/native-nvme-high-dma/20260912T034405Z-f5f561`.
Exactly 192 KiB matches round three instead of round four: 16 KiB in each of
the first two worker regions and 160 KiB in the third. Every remaining sector
matches round four. The snapshot SHA-256 is
`dd783842e50b7647afef3d65251944f478c02f2e032e8be1a638ec65dbcad04c`.
The controller remained reachable and recovery returned ROOBI
`ec369cf5-3a67-4f59-a47e-22abf6c04926`. USB checksum errors appeared during
automatic recovery after the failed readback, not during the storage command.

Source inspection found that `devfs_fsync()` returns success without calling
the device driver. Therefore neither earlier raw `dd conv=fsync` commands nor
the original concurrent probe proved a drive-cache flush. Their recorded data
checks remain observations, but prior descriptions of raw-device flush
qualification were incorrect. BFS file syncing follows a different path and
can issue `B_FLUSH_DRIVE_CACHE`; the earlier SSD-boot trials may have benefited
from incidental filesystem flushes. That is an explanation to test, not an
established cause of the high-address failure.

The probe now calls `B_FLUSH_DRIVE_CACHE` explicitly for the raw namespace,
keeps `fsync()` for regular files, logs every flush result and fails on errors.
The QEMU `dd` test no longer reports a flushed-write result. Forty-six host
checks pass, including an injected flush failure that must stop before the
next write round. Emulator command-trace verification and a corrected native
write/reboot/Linux-readback trial are pending; high-address DMA persistence
is not yet qualified.


## Corrected native high-address DMA flush and persistence checks

The corrected probe at source `33d832c078a3236f1ab8907421dbb9e8179fffdd`
has ARM64 binary SHA-256
`8d826730f9490da896b1055df94f5a93213de87d60d02bf560a550eb6f6856e8`.
The kernel/private high-DMA image remains `a7fe3ac0...fc202eb0`, isolating the
probe's flush change. Forty-six host checks pass in
`artifacts/control-checks-nvme-flush.log`, including injected flush failure.

QEMU `artifacts/qemu-shell/20260912T042339Z-2affe3` passed two 128 MiB rounds,
reboot readback, independent backing-file hashes, normal shutdown, memory and
cache checks. The NVMe trace records exactly two namespace flushes and two
completion callbacks during the two rounds, with no flush from the preceding
raw `dd conv=fsync` checks. The selected events follow the actual
[QEMU 8.2.2 flush implementation](https://github.com/qemu/qemu/blob/v8.2.2/hw/nvme/ctrl.c).
Two preceding harness failures remain recorded: `20260912T041756Z-739a3f`
rejected a corrupted terminal/base64 upload before storage testing; binary
transfer replaced it. `20260912T042131Z-7d506b` completed the writes but rejected
an obsolete, unused callback trace event; the corrected check uses
`pci_nvme_misc_cb` alongside `pci_nvme_flush_ns`.

Native `artifacts/interactive/20260912T042647Z-9310c1` passed the corrected
four-round 16–18 GiB workload with one worker pinned to each CPU. All four
`B_FLUSH_DRIVE_CACHE` calls succeeded. It wrote and verified 8 GiB in 35.497
seconds, then verified the complete final 2 GiB after normal reboot in 6.644
seconds. Normal power-off, an 800 ms NanoKVM power-button pulse and a third
Haiku boot followed; all 2 GiB matched again in 6.845 seconds. The two 1 MiB
guards remained unchanged. All 27 logged buffer pools across these three USB
boots were above 4 GiB. This trial does not change the installed SSD image.

After recovery, Linux `O_RDONLY|O_DIRECT|O_EXCL` reads independently matched
all eight 256 MiB region hashes and both guards. ROOBI returned with boot ID
`7d6f1d18-478e-46b1-bbd5-5ee95dd0f28a`; the controller retained boot ID
`84b69ae8-4d9e-4c03-9f5c-6c9547084819` and its watchdog disarmed. UART captured
301,387 bytes without transport errors. All 77 USB checksum messages occurred
after the final passing readback during forced recovery. HDMI capture timed
out while the board was powered off. Neither observation invalidates the
completed storage checks or establishes a fix for the earlier USB outage.

The scripts, source, binary, QEMU references, Linux results and reviewed
checkpoint are in
`artifacts/native-nvme-high-dma-flush/20260912T041905Z-f547db` and indexed by
`state/native-nvme-high-dma-flush-checkpoint.json`. The earlier failure snapshot
is retained. Explicit flushing resolves this reproduction and qualifies the
bounded workload with high DMA addresses across normal reset and power-off.
These raw-device checks did not qualify sudden power loss, longer mixed
workloads, controller-stall recovery, TRIM, MSI or full-size SSD installation.


## Full-capacity SSD installation and large-file checks

The owner-authorized Samsung SSD was repartitioned as GPT with a 512 MiB EFI
partition at sector 2048 and a 255,522,242,560-byte BFS partition at sector
1,050,624. The BFS volume uses 4096-byte blocks and reports 238.0 GiB. Exact
model, serial, firmware, capacity, mount/swap state and partition geometry were
checked before formatting. The disk GUID is
`0eee8446-86b3-4e3a-8181-f49193b4968c`; EFI and BFS partition GUIDs are
`251a9ddb-8c8b-4935-876a-fe51d3e2f5f6` and
`f7e520a0-41fd-459a-9f92-e431d817faec` respectively.

The workflow was first rehearsed in QEMU
`artifacts/qemu-shell/20260912T043159Z-853d71`: Haiku formatted the partitions,
Installer copied the system, and the EFI loader and installed files were
verified. A sparse full-capacity image then passed two NVMe-only boots, a file
extending beyond 5 GiB, reboot readback, filesystem checking and normal shutdown
in `artifacts/qemu-shell/20260912T045749Z-a98c4a`. The original wrapper rejected
an overlong emulated serial label that the driver truncated. Its error remains
recorded; `fullsize-nvme-boot-review.json` checks the actual label, NVMe-only
configuration, two boot-volume markers and all passing core results. The
256 GB logical image is for QEMU only and was never uploaded to NanoKVM.

Native installation used the ordinary private USB image `6a2e3322...031bc7a7`,
built from kernel source `215a43a021913435a29f4b009fdff0af415d9fbb`.
`artifacts/interactive/20260912T050604Z-ee7d20` records Haiku's FAT32/BFS
formatting and Installer's successful copy. All eleven installed package hashes
and three required lab settings matched the source. The ARM64 EFI loader was
copied separately to `EFI/BOOT/BOOTAA64.EFI` and matched SHA-256
`ac0bc6ace649e94c3b67190c27e91cd733d8fda8349b31c3c6821f072f57f8db`.
`checkfs -c` checked 241 nodes without allocation errors. Both SSD volumes were
cleanly unmounted before recovery.

The full GPT device path is recorded in lab option `Boot0010`. Trials request
it through one-time `BootNext`, with the verified ROOBI USB selected and
`BootOrder` unchanged. This keeps the automated recovery route available.
The first two SSD sessions, `interactive/20260912T052301Z-b7462a` and
`interactive/20260912T053049Z-c5f388`, both reached the desktop and authenticated
USB shell with `/boot` mounted from `/dev/disk/nvme/0/1`. The first also passed
the locked 64 MiB, eight-worker memory probe and 16,384 cache checks.

A regular BFS file of 7 GiB + 8 MiB holds the storage test. Eight workers, one
per CPU, wrote and read four rounds over file offsets 5–7 GiB: 8 GiB written
and immediately verified in 36.524 seconds. Regular-file `fsync()` succeeded
after each round. The final 2 GiB independently matches the host-calculated
pattern SHA-256 `ae657b8cc8195eb7aacfe432772c9dcc8102f0e14ec5a4d010d263babcafc94b`.
Two surrounding 8 MiB guards also match. Normal reboot returned to recovery;
the next SSD boot verified the complete region in 6.733 seconds and repeated
the hash and guard checks. Filesystem checking found no allocation errors.
Normal Haiku shutdown and an 800 ms NanoKVM power-button startup subsequently
returned ROOBI, followed by another one-time SSD boot. This sequence checks
persistence across shutdown/startup with recovery intervening; it does not
establish direct SSD cold boot without that recovery step.

The third SSD session, `interactive/20260912T053644Z-187855`, verified the full
region, both guards and the host-calculated hash after shutdown/startup.
However, USB control failed during the subsequent filesystem check: UART
recorded 2,535 USB checksum messages, and the shell command timed out before
reporting completion. This remains an interrupted run, with its partial
transcript and successful automatic recovery retained. There was no observed
data mismatch, but the interruption does not qualify USB reliability.

The fourth SSD session, `interactive/20260912T054356Z-162715`, sent all final
check output to UART as well as returning a shell completion marker. Its full
2 GiB probe passed in 6.746 seconds, the independent hash and both guards
matched, and all eleven installed packages still matched their original
hashes. The filesystem check processed 254 nodes with no allocation errors;
the 47 indices and indirect block runs were checked. This session had no USB
checksum messages and completed a normal Haiku reboot. Each guarded session
returned to ROOBI with the NanoKVM controller still reachable.

The partition plan, scripts and receipts are in
`artifacts/nvme-full-install/20260912T044937Z-703399`; the reviewed checkpoint is
`state/nvme-full-install-checkpoint.json`. The earlier raw test ranges
at 2, 5, 7 and 16–18 GiB now lie inside the BFS partition and are retired in
`state/nvme-raw-fixtures-retired.json`. Their historical evidence is retained;
subsequent write tests must use identified regular files on the installed
filesystem. This is an experimental minimal installation with eleven packages,
the firmware-specific PCIe profile and polling. TRIM, controller-stall recovery,
MSI, sudden power loss, sustained mixed load and full-board parity remain open.


## NVMe TRIM range candidate

Source review found that subtracting the partial leading sector from a smaller
TRIM request could underflow. For example, offset 17 and length 1 with 512-byte
sectors should trim nothing; the old arithmetic produced a range subsequently
capped at `UINT32_MAX` sectors. The candidate normalizes only complete sectors
inside the requested and namespace intervals, compacts away empty ranges,
checks the 256-range command limit before narrowing the count, and checks
namespace DSM support before submission. Empty work succeeds without a command.

Forty-seven host tests pass, including more than two million interval cases
checked against a separate 128-bit arithmetic calculation. The ARM64 driver
and a QEMU-only ioctl probe compile. That probe requires an 8 GiB namespace
with a private test marker, so it rejects this physical Samsung drive.
The raw ioctl probe passes in ordinary and forced-high-DMA QEMU runs
`qemu-shell/20260912T060739Z-436e5d` and `qemu-shell/20260912T060739Z-d3c4c3`:
ten cases, exactly three nonempty DSM commands, exact surrounding bytes and
post-reboot readback. Earlier harness failures with QEMU's default discard
policy of `ignore` remain recorded; the corrected runs use `discard=unmap`.

The BFS test in `qemu-shell/20260912T061028Z-af5780` exposed a separate driver
issue. Although `fstrim` reported 8,025,243,648 bytes and allocated-file checks
passed, the controller trace skipped a 7,335,920-sector range exceeding its
advertised DMRSL of 4,194,303 sectors. This is a failed TRIM qualification;
`state/nvme-trim-dmrsl-failure.json` preserves the trace and review. GPT, EFI
and tail guards remained intact, and all ten DMA pools were above 4 GiB.

The follow-up candidate reads NVM command-set-specific controller Identify
limits and splits intervals to honor range count, per-range blocks and total
blocks per command. It retains intervals beyond `UINT32_MAX`, validates every
input before the first command, and waits for completion before reusing the
DMA descriptor buffer. Pre-1.2 controllers use legacy format limits without
sending an unencodable CNS selector. Optional Identify rejection on pre-2.0
controllers retains those defaults; other discovery failures disable TRIM
while retaining ordinary I/O. Nonzero limits also enable the mandatory DSM
support variant. Host interval and batch-limit checks and the ARM64 driver
build pass. Refreshed ordinary/high-DMA QEMU raw ioctl checks pass in
`qemu-shell/20260912T064028Z-63836b` and
`qemu-shell/20260912T064326Z-957939`. The BFS test passes in
`qemu-shell/20260912T064325Z-3230f4`, with CNS 06h / CSI 00h Identify traces,
five DSM ranges covering every requested block, and no skipped ranges. The
high-DMA BFS run `qemu-shell/20260912T064028Z-1aa8cf` also passes independent
review of those ranges, file checks, reboot, guards and ten DMA pools above
4 GiB. Its original wrapper error is retained: it expected a `nvme_notice`
message suppressed by the driver's library log level; the corrected wrapper
uses controller Identify traces. `state/nvme-trim-qemu-checkpoint.json` records
the combined gate. The image manifest records untracked host build libraries
created by checks; its tracked source patch is empty, and those libraries were
subsequently archived outside the source tree.

The field layout and processing semantics were checked against the
[NVM Command Set 1.0d specification](https://nvmexpress.org/wp-content/uploads/NVM-Express-NVM-Command-Set-Specification-1.0d-2023.12.28-Ratified.pdf),
with the skipped-range behavior checked in QEMU 8.2.2's `hw/nvme/ctrl.c`.
The previous physical raw fixture plan is retired; the next native test uses
BFS's free-space interface.


## Native BFS TRIM qualification

Driver source `5ac55e25f837c686b7489753312eacbdb59f733b` passed native
free-space TRIM from the diagnostic USB image SHA-256
`2d1b1f04c009f23607754ad70215bdef92376816243b26881d32230b050f15e4`.
In `interactive/20260912T064648Z-456e0c`, all nine NVMe DMA pools were above
4 GiB. The Samsung's full BFS volume was mounted at `/HaikuNVMe`;
`fstrim -f -v` reported 231,037,755,392 bytes, exactly its 56,405,702 free
4 KiB blocks. The existing 2 GiB pattern region, surrounding guards, eleven
installed package hashes, EFI loader and filesystem allocation checks passed
before and immediately after trimming. No USB check-sum errors appeared in
that initial TRIM/readback segment.

Normal reboot loaded the same diagnostic image and again allocated all nine
DMA pools above 4 GiB. The next command referenced a missing local script,
triggering automatic recovery before its readback could execute. That failed
session is preserved. Its serial capture also contains 59 USB transaction-error
messages after the second boot and before recovery; it does not establish USB
reliability. The preparation script now gives reboot requests and reboot
readback distinct filenames.

A subsequent one-time SSD boot in `interactive/20260912T065903Z-d24410`
reached the installed desktop with `/boot` on `/dev/disk/nvme/0/1`. Full
2 GiB verification passed in 6.744 seconds, the independent SHA-256 and both
guards matched, all eleven packages still matched, and `checkfs -c` reported
no allocation errors. That session captured no USB check-sum errors and
completed a normal reboot to ROOBI. Both controller guards disarmed.

Linux inspection before and after these trials used read-only direct I/O on
the identified, unmounted Samsung namespace. The primary GPT prefix, the entire
512 MiB EFI partition and the complete region after BFS containing the backup
GPT match exactly. The loader SHA-256 remains
`ac0bc6ace649e94c3b67190c27e91cd733d8fda8349b31c3c6821f072f57f8db`.
Scripts, raw results and the reviewed checkpoint are in
`artifacts/native-nvme-fstrim/20260912T064429Z-1c2c08` and
`state/native-nvme-fstrim-checkpoint.json`.

This qualifies successful filesystem free-space TRIM commands and preservation
across reboot/recovery; it does not measure physical NAND reclamation or sudden
power-loss durability. That SSD readback used the prior
`215a43a021913435a29f4b009fdff0af415d9fbb` build; the subsequent installed update
is recorded below. Controller-stall handling, sustained
mixed load, MSI, the other PCI roots and overall board parity remain open.


## Installed NVMe TRIM update

The physical Samsung installation now runs `hrev60097+57`, driver source
`5ac55e25f837c686b7489753312eacbdb59f733b`. Installer copied from the ordinary
private image SHA-256
`01c26d17692268e2f6dfe70792b217e2e0465f48e0e0b2c0022efe1282020191` onto
the existing full-capacity BFS volume. The GPT layout remains unchanged.
The installed image does not contain the high-DMA diagnostic setting.

The update rehearsal exposed a FAT overwrite problem: copying directly over
the read-only `BOOTAA64.EFI` returned `Operation not allowed` after truncating
the file to zero bytes. This occurred only in a disposable QEMU copy. The new
`install-efi-loader.sh` helper verifies the old loader, a separate backup and
a staged replacement before renaming the new file into place. QEMU checks
covered rejection of a wrong old hash, successful replacement and backup,
and an already-current retry. The new loader SHA-256 is
`31d8f11cef5a998ad2ef0a29608397dc1d1ce51cb8cfe4d6aea7eadfd1d9dc75`;
the retained previous loader is
`ac0bc6ace649e94c3b67190c27e91cd733d8fda8349b31c3c6821f072f57f8db`.

The complete QEMU installation repeat in
`qemu-shell/20260912T072836Z-41d159` passed the full 5 GiB + 8 MiB file hash,
eleven package hashes, filesystem checks, staged EFI replacement and explicit
sync/unmount. Independent cold readback of all packages and both EFI files
passed. The resulting full-capacity image then booted as NVMe-only storage in
`qemu-shell/20260912T075048Z-922b2c`; the full file and packages matched on
both sides of normal reboot, and normal shutdown completed. Memory, cache
and binary-transfer checks also passed. The transfer harness now resets its
two known fixture files in the disposable overlay, allowing reuse of an
installed image that retains those fixtures.

Failed rehearsals remain recorded. One whole-file hash exceeded the original
deadline before Installer started. A clone of the interrupted EFI-overwrite
run later had a mismatching `noto` package; that run had not reached its final
sync/unmount, and the exact cause is not established. Installation was repeated
from the original qualified disk and verified after clean shutdown. The first
updated-image boot test also stopped on a preexisting transfer-test file before
the corrected test setup was applied. These failures are not counted as passes.

On the board, `interactive/20260912T071040Z-13b11c` lost USB control after
the eight-worker precheck and before Installer launched; it recorded 13,118
USB check-sum errors and recovered to ROOBI. The fresh session
`interactive/20260912T074104Z-2c2277` completed Installer, a one-worker read of
the existing 2 GiB pattern region, its independent SHA-256, both 8 MiB guards,
all eleven package hashes, filesystem checks and the EFI update. Both volumes
were synced and cleanly unmounted. No USB check-sum errors were recorded in
that repeat. Using one reader here does not establish eight-core USB stability.

Two subsequent SSD boots in `interactive/20260912T075633Z-8ce009` and
`interactive/20260912T080235Z-7edb2d` mounted `/boot` from
`/dev/disk/nvme/0/1` with the updated kernel. On the first boot, installed-system
TRIM reported 231,038,304,256 bytes, matching 56,405,836 free 4 KiB blocks.
The full pattern region, guards, packages and filesystem checks passed before
TRIM and after normal reboot/recovery and the second SSD boot. Linux also
verified both EFI file hashes between those boots. Both native sessions
completed normal reboot to ROOBI, and their controller guards disarmed.
Neither recorded USB check-sum errors before its reboot.

Scripts and retained failure reviews are under
`artifacts/nvme-installed-update/20260912T070143Z-d7ec4d`; the final reviewed
checkpoint is `state/nvme-installed-update-checkpoint.json`. This qualifies the
installed update and the stated persistence checks. Sudden power-loss
durability, controller-stall recovery and overall hardware parity remain open.


## Explicit USB reset while booted from NVMe

The session tools now support a prepared, single-use NanoKVM USB reset for an
installed NVMe boot. Preparation verifies the boot receipt and UART mount,
uses an authenticated shell to sync and reject mounted USB filesystems, and
checks the selected and persistent recovery images. A changed target boot,
controller boot or image invalidates preparation. Failed reset operations
retain their error receipt and the session's normal recovery route. Nine new
host checks cover these interlocks; the complete 56-check suite passed.

The implementation uses NanoKVM's documented Reset HID operation. The
[2.4.3 implementation](https://github.com/sipeed/NanoKVM/blob/3b2ba7c0c1214f44da9d328f90bbdd025fac0413/server/service/hid/status.go)
invokes the installed USB script's `restart_phy` action. The local script was
read and archived before testing; it unbinds/rebinds the NanoKVM DWC2 device
and restores gadget mode. The configured recovery image remained selected,
and NanoKVM's boot ID did not change.

Native session `interactive/20260912T081718Z-9e2e7f` ran the qualified
`hrev60097+57` SSD installation, with the preceding ordinary-image QEMU gate
in `qemu-shell/20260912T081407Z-671b8c`. The reset removed and re-enumerated
the USB disk, HID and RNDIS device without rebooting Haiku. Keyboard and mouse
worked afterward. A Tracker prompt to mount the recovery FAT volume was
canceled. UART recorded 67 USB check-sum errors during disconnect and no more
after the new RNDIS device was added. A notification callback also recorded a
five-second control-request timeout during removal.

Networking did not return automatically. Through the KVM Terminal, `ifconfig`
showed only loopback, although `/dev/net/usb_rndis/0` existed. Explicitly running
`ifconfig /dev/net/usb_rndis/0 auto-config` restored `10.239.6.102`. Authenticated
commands and a filesystem check then passed. An 8 MiB upload took 17.81 seconds;
its staged return at 256 KiB/second took 53.68 seconds. Both copies matched
SHA-256 `7d212b9c884f5c77896de960ae17cc341cda43b14d6a971f34ca29ebd4badf7f`.
The target completed normal reboot to ROOBI boot ID
`f6dcbdce-97a0-435c-8066-3667219d1267`, and the controller guard disarmed.

This was a controlled reset of a working link. It establishes that USB reset,
KVM input and explicit network reconfiguration can preserve a running SSD
session; it does not qualify automatic reconnect or recovery from a spontaneous
controller outage. Evidence and source snapshots are in
`artifacts/native-usb-reset/20260912T081408Z-1af315` and
`state/native-usb-reset-checkpoint.json`. Automatic interface recreation is the
next investigation.

## USB network interface replacement

QEMU reproduced the missing interface after unplug/replug. Keeping the RNDIS
USB cookie alive through the final free hook and excluding removed devices
from publication corrected driver lifetime errors. A second failure remained:
an old socket/route retained the previous network interface and its ARP receive
handler, so creating the replacement failed with a duplicate handler. Network
interfaces now retire from name/index lookup while their old references finish
closing. Handler cleanup matches the device object, and delayed removal checks
the interface's device identity before removing it.

The first complete reconnect pass is
`qemu-shell/20260912T090748Z-1b769a`. The extended run in
`qemu-shell/20260912T091124Z-00b718` also passed connected-interface down/up,
three unplug/replug cycles with automatic authenticated reconnect, and 8 MiB
checksum-verified transfers before and after each cycle. After a 130-second
wait for old TCP references, another round trip and filesystem check passed.
Normal reboot, power-off, memory/cache checks and their negative controls passed.
Reconnection took about 20 seconds in this emulator configuration.

The image remains based on the installed `hrev60097+57` kernel and packages.
It overlays RNDIS component `fbd0d76c9f` and network stack `3d928a4833`, with
combined image SHA-256
`9a4468089f4ad7d1e838c3c355c8ae55519937f2f10887b0b852e8cabef47688`.
The manifest records both component revisions and hashes separately. Guest
kernel image listings and file hashes verify that the overrides were loaded.

`install-network-overrides.sh` stages and hashes both components before placing
them in the non-packaged add-on directories. Its initial install, identical
second invocation, subsequent boot with both overrides, and shutdown passed in
`qemu-shell/20260912T091505Z-978bac`. Native installation in
`interactive/20260912T091817Z-8ab084` passed initial and identical repeat
installation, then normal reboot to ROOBI. The subsequent SSD boot in
`interactive/20260912T092446Z-7f6097` verified both loaded component paths and
hashes. Connected-interface down/up and the first USB reset passed automatic
network configuration, authenticated commands and an 8 MiB upload/return with
matching SHA-256. There were 64 USB check-sum errors during the first disconnect
and none between reattachment and the second reset preparation.

The second reset restored the interface and ICMP reachability, but the later
authenticated login failed. Between the second preparation and recovery entry,
UART recorded 145 USB check-sum errors, including 68 after the RNDIS device was
added again. HID and RNDIS both reported errors. Recovery returned ROOBI boot ID
`fbcb1834-5fdb-4c17-86dc-8744326a8ac5`; NanoKVM's boot ID stayed unchanged and its
guard disarmed. Three-cycle native qualification and the final SSD readback
remain incomplete. The overrides remain installed for investigation; they are
not a qualified replacement for the recorded base image. The plan, rollback
script and reproducers are in
`artifacts/native-network-overrides/20260912T091424Z-79df24`.

This trial also exposed a host controller-input defect: a read-only SSH child
inherited stdin and consumed a queued JSON action. Such children now use null
stdin; explicit input remains available for sudo. Two subprocess tests include
a reproducer of the old behavior and verify both corrected paths. All 58 host
checks passed. The active hardware session predated that fix and issued later
actions individually.

Failed diagnostic runs are retained. Early manual-recovery attempts had commands
queued behind `netstat`; diagnostics now use `netstat -n` to avoid name resolution.
Two later trials recreated the interface and exchanged TCP handshakes, but the
harness closed each connection after a two-second login timeout. Corrected tests
allow ten seconds per prompt and save separate results. These harness failures
are not evidence that the final driver failed to recreate the interface.

## RNDIS notification completion worker

The notification callback previously called synchronous `clear_feature` while
running on a USB completion thread. The same thread could be needed to complete
that request. The driver now uses a notification worker; callbacks only record
the result and wake it. Close/removal serialize cancellation with submission and
join the worker before releasing the USB device. Only an endpoint STALL clears
the halt/data toggle; other notification errors retry after a bounded delay.

Production component `f661a168ac` (SHA-256
`0728416cb6d1003ba1cb2d51f13ea6a36b85fd58fb4a3aa39ae6863e2812a3d8`)
and the existing replacement-aware stack passed three automatic reconnects,
connected-interface down/up, transfer checks, the 130-second reference wait,
normal reboot and shutdown in `qemu-shell/20260912T095008Z-d02d27`. The combined
image is `ba54c0f3f5526556d5d307329bc736035573ff8a4305668743adebce54fb9df0`;
base kernel/packages remain `hrev60097+57`. All 58 host checks passed.

Diagnostic-only CRC and short-notification injections passed boot, transfers,
reboot and shutdown in `qemu-shell/20260912T095521Z-1e491d`, with no control
request/response timeouts. The combined STALL injection trial did not pass its
boot gate. QEMU's [USB network model](https://github.com/qemu/qemu/blob/v8.2.2/hw/usb/dev-network.c)
and [descriptor handler](https://github.com/qemu/qemu/blob/v8.2.2/hw/usb/desc.c)
lack endpoint CLEAR_FEATURE handling. The new worker returned from that request
in 489 microseconds, but the resulting control-endpoint stall broke subsequent
initialization. The old callback did not return before its 180-second observation
ended. This timing comparison does not qualify real endpoint STALL recovery.
The retained failure review explains the original comparator's failed result.
These emulated networking trials use xHCI because the bundled USB network model
has only a full-speed descriptor. Native EHCI remains a separate gate.

The first `update-rndis-override.sh` version verified the expected current driver,
replacement and stack, saved a verified copy of the old driver, then renamed the
staged replacement into place. Initial update, identical repeat, rejection of incorrect new/old
hashes, subsequent boot with the replacement and old-backup readback passed in
`qemu-shell/20260912T095559Z-2f1a35`. Native installation and cleanup are recorded
below. Evidence and scripts are in
`artifacts/native-rndis-notify-update/20260912T095522Z-11431b`.


## Preserve the loaded driver inode during updates

The first notification-driver update exposed a filesystem allocation leak.
Although `checkfs -c` returned zero, its next-boot report listed eight
unreferenced 4 KiB blocks on the SSD. The matching QEMU installation listed
fifteen 2 KiB blocks. These counts match the replaced 28,019-byte driver and
its inode. Copying the loaded file to a backup and overwriting its original
name left the original inode open and unlinked until reboot. Pre-update
large-file, guard and package hashes had passed; the new driver loaded and
connected-interface down/up also passed.

The update helper now stages the replacement, renames the original into its
backup, and publishes the replacement. It can resume if interrupted after the
original rename. It rejects a conflicting backup. The corrected standard
update and interrupted-update rehearsal passed in
`qemu-shell/20260912T102054Z-30fd50` and
`qemu-shell/20260912T102056Z-0760df`: component/backup hashes, zero allocation
counters, one notification worker, normal reboot and shutdown all passed.

A clone of the original QEMU installation with fifteen unreferenced blocks
was repaired in `qemu-shell/20260912T102055Z-29d4fb`. `checkfs /boot` reclaimed
them; subsequent read-only checks, transfers, driver/backup hashes, reboot and
shutdown passed. Native cleanup of the eight SSD blocks passed as recorded below.

The host now parses the actual allocation counters and rejects incomplete or
ambiguous reports and node-damage diagnostics. A substring containing zero is
not accepted as a zero count. All 62 host checks pass. The SSH stdin tests also
use the configured temporary directory so they can run on CI without the lab
mount. Two intermediate QEMU reviews failed an added thread-list assertion:
Haiku `ps` treats a positional argument as a team-name filter, not a numeric
PID. The corrected trials use `ps -as`. A native diagnostic using unavailable
`grep` likewise ended its session through normal automatic recovery; no USB
reset result was recorded in that session.


Native cleanup in `interactive/20260912T102552Z-1a16d7` reclaimed the eight
unreferenced SSD blocks. A subsequent strict check reported zero missing,
duplicate and unreferenced blocks, with one notification worker. The 2 GiB
pattern, its independent hash, both guards and all eleven packages passed after
cleanup. No USB check-sum errors or control-request timeouts occurred before
the first USB reset. That reset restored an authenticated shell automatically;
a workstation import failure prevented the subsequent KVM click, so the
8 MiB round trip and remaining resets were not run.

The native session had been launched with system Python instead of the lab's
NanoKVM virtual environment. Interactive sessions now validate `websocket-client`
before deployment and report the required interpreter when it is missing. The
local virtual environment provides version 1.9.2. All 64 host checks pass, and
a CLI check confirmed the missing-dependency failure occurs before the hardware
lock/deployment. Native recovery from the interrupted session then encountered
Linux I2C/SPI timeouts and did not restore ROOBI through its reset/power sequence.
A later captured 1,500 ms reset restored ROOBI boot ID
`710d64b2-ca95-4589-8ff5-876d21676b2b`. NanoKVM GPIO readback showed assertion
and release, and UART captured the fresh firmware/Linux boot in
`recovery-observed-reset/20260912T104943Z-39d80f`. The cause of the earlier
recovery failure remains open.

## RNDIS bulk completion errors and byte counts

Bulk reads and writes previously cleared ENDPOINT_HALT for every non-canceled
error while the device was present. A failed write could then return the
clear-halt result instead of its original transfer status, or claim success if
clearing succeeded. Successful writes also included the 44-byte RNDIS header
in the caller's Ethernet byte count. The driver now clears only STALL, returns
the original failed-transfer status with zero bytes, rejects a short completed
write, and reports the Ethernet payload length on success. Failed reads remain
errors when device removal overlaps their completion.

Diagnostic-only completion injection reproduced both unnecessary clear-halt
calls and excess write counts in `qemu-shell/20260912T110429Z-f4129b`. The
original write's injected CRC status became a control-request timeout. The
same traffic eventually completed, so this is an error-path/byte-count
reproducer rather than a boot-failure claim. The interpretation of transaction
errors follows the [Linux USB error documentation](https://www.kernel.org/doc/html/v5.10/driver-api/usb/error-codes.html);
such an error does not establish an endpoint STALL or a physical wire CRC fault.

The corrected CRC trial in `qemu-shell/20260912T110829Z-651e6a` retained the
original read/write error, issued no clear-halt requests, reported exact
successful write lengths, and had no control-request or response timeouts.
A separate short-write trial in `qemu-shell/20260912T111133Z-b60a5f` returned
`B_IO_ERROR` with zero bytes on both boots. Both trials passed authenticated
commands, the 8 MiB transfer/negative checks, normal reboot and shutdown.
All 64 host checks passed. The injection changes completion metadata after
real emulated traffic; it does not qualify native EHCI transaction recovery
or real endpoint STALL recovery. Production sources contain no injection.

The completed native trial of notification-worker component `f661a168ac` and
stack `3d928a4833` in `interactive/20260912T105126Z-c9bc47` passed three automatic
authenticated reconnects, an 8 MiB upload/return after each, and a 360-second
pause in guest test traffic after the second cycle. No serial bytes appeared
during the pause, and the subsequent authenticated shell passed. Strict BFS
allocation checks and the single-worker check passed before the first cycle,
after the second, and after the third. The disconnect segments contained 147,
347 and 626 USB check-sum messages respectively, with none after RNDIS was
added again in any cycle. No control-request or response timeouts were observed.

The 2 GiB file pattern, independent SHA-256, both 8 MiB guards, eleven package
hashes and zero BFS allocation counters passed after all three cycles. Normal
reboot returned ROOBI boot ID `e491cdf7-5164-496a-a028-351cf684a45a`. A second
SSD boot in `interactive/20260912T111904Z-da79d8` repeated the component,
single-worker, large-file, guard, package and allocation checks successfully.
All three newly written reconnect files also retained their expected hashes.
Its normal reboot returned ROOBI boot ID
`c9cd10c5-22be-411a-aa45-be59bbee28c1`. Both guards disarmed, and NanoKVM kept
its original boot ID. The full qualification is indexed by
`state/native-rndis-notify-retest.json`.

This qualifies the specified NanoKVM/EHCI reconnect trial and SSD persistence;
all USB ports and sustained mixed loads remain separate work. Downloads were
staged at 256 KiB/s, and the SSD checks used ordinary buffered file reads.

Production bulk-error component `2016925fa8` has SHA-256
`653fade5f9498ff0a7a87b134dca5e59db19276c25af98c10f271e591b905a9f`.
The combined image with the existing stack is
`69b9c2d57f95fd04588107cef9293f79917e0ea573aee081ffebc10cef4837a0`.
A single-reconnect production trial passed in
`qemu-shell/20260912T111733Z-a9d917`. Its update rehearsal in
`qemu-shell/20260912T112050Z-4b7756` passed install/idempotence, wrong-hash
rejection, transfers, correct post-reboot component and backup hashes,
zero allocation counters, one worker, normal reboot and shutdown. The update
plan and rollback command are in
`artifacts/native-rndis-bulk-update/20260912T112049Z-2f94b9`; hardware deployment
also requires the repeated-reconnect gate in its session wrapper.

That repeated production gate passed in `qemu-shell/20260912T112306Z-ac3ab4`:
connected-interface down/up, three automatic reconnects with transfer checks,
a 130-second wait for retired network references, another transfer check,
normal reboot and shutdown. Native update in
`interactive/20260912T113046Z-7bc3ab` then passed initial and identical repeat
installation. The new driver and the renamed original both matched their
hashes, with zero allocation counters. Normal reboot returned ROOBI boot ID
`2abb3402-55ea-47ba-9e2e-e05488e248f4`, and its guard disarmed.

The subsequent SSD boot in `interactive/20260912T114032Z-24b674` verified the
new `2016925fa8` component and existing stack in their loaded non-packaged
paths, one notification worker, and zero missing, duplicate or unreferenced
blocks. This also verifies the inode-preserving update on native 4 KiB BFS
across reboot. The completed trial then passed three automatic authenticated
USB reconnects with an 8 MiB upload/return after each. A 360-second pause after
cycle two produced no UART bytes, and authenticated access afterward passed.
The three disconnect segments contained 188, 676 and 301 USB check-sum
messages; none appeared after the corresponding RNDIS reattachment. No USB
control-request or response timeouts were observed. The single-worker and
strict zero-allocation checks passed before and after the cycles.

After all three cycles, the 2 GiB pattern, independent SHA-256, both 8 MiB
guards, all eleven package hashes and BFS checks passed. Normal reboot returned
ROOBI boot ID `ecf9f5da-aad3-44ad-be77-813608c62934`. The subsequent SSD boot in
`interactive/20260912T121602Z-9b82a3` repeated those checks successfully and
verified the three reconnect files and saved previous-driver binary. Its
normal reboot returned ROOBI boot ID
`554c8790-241f-4452-aebc-d9e0355bae78`. Both guards disarmed, with NanoKVM's boot
ID unchanged. Qualification is indexed by `state/native-rndis-bulk-retest.json`;
current component identity is in `state/rock5-installed-network-components.json`.
The earlier notification trial remains separate evidence for its component
hash. Downloads were staged at 256 KiB/s; these are buffered file checks and
the specified NanoKVM/EHCI path, not sustained mixed-load or all-port acceptance.

A subsequent mixed-load script rehearsal passed in
`qemu-shell/20260912T120519Z-e62e75`. Eight virtual CPUs completed five 64 MiB
file-verification passes while both directions of an 8 MiB USB round trip
ran. Guest monotonic timestamps recorded 1.405 seconds of read-process overlap
during receive and 1.871 seconds during send. The independent file hash,
strict allocation check, normal reboot, repeated file verification and shutdown
passed. The plan is `state/mixed-read-plan.json`.

The corresponding native trial in `interactive/20260912T122302Z-a7c3ac`
completed 29 verification passes over the existing 2 GiB file region, with
all eight workers confirming their assigned CPU on every pass. The 8 MiB
upload and staged return matched their independent host hash. Monotonic guest
timestamps recorded 3.865 seconds of verification-process overlap during
receive and 34.831 seconds during send. No USB check-sum messages,
control-request/response timeouts or kernel debugger appeared after the initial
shell configuration through collection of the workload results.

The old file pattern, independent SHA-256, both guard regions, all eleven
package hashes and zero BFS allocation counters passed afterward, as did the
component hashes and single notification worker. Normal reboot returned ROOBI
boot ID `d77d0873-ee18-4851-b32f-8d1ed782175e`. The subsequent SSD boot in
`interactive/20260912T123510Z-aebe2c` repeated those checks and verified the new
round-trip file. Its normal reboot returned
`f76bc95e-c7da-4eb5-8d68-5245ab14f3d3`. Both guards disarmed without a NanoKVM
restart. The complete result is `state/native-mixed-read.json`.

This is a short mixed-read qualification on the specified NanoKVM USB path.
The repeated reads use ordinary file caching; 29 passes do not mean 58 GiB of
physical SSD traffic. Downloads remain staged at 256 KiB/s. Writes and sustained
platform acceptance remain separate tests.

A write-workload rehearsal passed in `qemu-shell/20260912T123403Z-b573ab`.
Eight virtual CPUs completed four write/fsync/peer-read rounds over a separate
64 MiB region between two 8 MiB guards. Both directions of an 8 MiB round trip
overlapped that write-and-verify invocation. The final independent region hash,
both guards, clean filesystem check, normal reboot, readback and shutdown
passed. The prepared native test uses a separate 2 GiB region, preserving the
existing large-file reference for independent checks. Its plan is
`state/mixed-write-plan.json`.

The native write trial in `interactive/20260912T124354Z-9d3b53` completed
three invocations, each with four write/fsync/peer-read rounds and eight
workers, over the new 2 GiB region. Both directions of an 8 MiB USB round trip
matched the host hash; guest monotonic timestamps recorded 4.099 seconds of
write-and-verify process overlap during receive and 34.870 seconds during
send. No USB check-sum messages, control-request/response timeouts or kernel
debugger appeared between initial shell configuration and workload collection.

Independent verification of the new region hash and its two 8 MiB guards
passed afterward, along with the old reference region, its guards, all eleven
package hashes and strict zero BFS allocation counters. Component hashes and
the single RNDIS notification worker also passed. Normal reboot returned ROOBI
boot ID `a389dac6-b202-4bd3-bdea-4ade7ff158b1`. The next SSD boot in
`interactive/20260912T125722Z-8aa76a` repeated both region/guard checks, package
and component checks, and verified the new round-trip file. Normal reboot
returned `28a3f4b8-c152-43ae-966b-eb51d6346670`. Both guards disarmed without a
NanoKVM restart. The full result is `state/native-mixed-write.json`.

These twelve write rounds represent 24 GiB of logical regular-file writes,
with an explicit file flush and peer read after each round. They are not a
physical throughput measurement. This short mixed-write qualification uses
the specified NanoKVM/EHCI path with staged downloads at 256 KiB/s; sustained
load, all USB ports and storage error recovery remain open.

## Onboard PCI configuration after firmware handoff

Diagnostic `2faf3d56407c4707e0376e6f80b09068745398b4d93722ebe0d4e259199dc586`
passed on SSD boots `interactive/20260912T133024Z-3b996f` and
`interactive/20260912T134213Z-228a3d`. Both read the eight known functions on
firmware segments 0, 1, 3 and 4, checking each root's active link before its
endpoint. All eight 256-byte captures are identical across the two Haiku boots.
The six SATA/Ethernet captures also match the EFI snapshots byte for byte.
The Samsung pair differs in three bytes of standard command/bridge state.
No PCI configuration, clock, regulator, address-window or DMA writes were
performed by the diagnostic. The full result is
`state/native-onboard-pci-probe.json`.

Component hashes, one notification worker and strict zero-allocation BFS
checks passed before and after each probe. The second boot also passed the
existing 2 GiB reference pattern, independent hash, guards and eleven package
hashes. Its normal reboot returned ROOBI boot ID
`86635310-fa37-4af4-9c24-cf9609519b61`. The first normal recovery instead reported
inaccessible SATA configuration and stalled in Linux's `ahci_enable_ahci`.
The established reset recovery restored ROOBI with boot ID
`6d646fa5-d809-4bd8-bf57-107e81cb5ab4`. Both guards disarmed without a NanoKVM
restart. The read-only configuration milestone passed; the recovery failure's
cause remains open.

The subsequent ROOBI resource snapshot in
`pci-linux-reference/20260912T135308Z-07733b` records 8 KiB SATA BAR0/BAR5,
and 64 KiB BAR2 plus 16 KiB BAR4 for each RTL8125. Linux's BAR addresses differ
from EDK2's retained configuration. Ethernet segment 4 is connected at 1 Gb/s;
segment 3 has no carrier. This is resource evidence, not a Linux throughput
baseline or a native Ethernet pass.

The firmware DT advertises MBI interrupt IDs 424 through 479, but the same DT
assigns several of those IDs to USB PHY, ADC, thermal and random-number devices.
The audit is `pci-onboard-probe/20260912T125505Z-9edda7/mbi-resource-audit.json`.
Rockchip's [RK3588 TRM, Table 1-3](https://www.scs.stanford.edu/~zyedidia/docs/rockchip/rk3588_part1.pdf)
confirms wired sources through ID 453 and labels 454 through 511 reserved.
The entire advertised MBI pool must not be enabled without resolving those
conflicts. Interrupt routing and noncoherent network DMA remain unimplemented.

The opt-in onboard host profile from `08ccc83a79` passed its first native
enumeration trial. Image `hrev60097+79`, SHA-256
`08dfd31c523fd53d11b6c4852bfd6e449f18addda5f0f8ec4171806631d4c998`,
first passed QEMU in `qemu-shell/20260912T141001Z-0a8c43`, including emulated
PCI/NVMe checks, USB transfer, component hashes, AHCI exclusion, filesystem
checks, normal reboot and shutdown. QEMU did not claim the RK3588 host.

On native USB boots in `interactive/20260912T141439Z-cea686`, the host attached
all four firmware roots and enumerated four bridges, Samsung NVMe, ASM1164 and
both RTL8125 controllers. Each root retained its 1 MiB firmware memory window.
The initial boot and a normal Haiku reboot both passed a 64 MiB NVMe EFI-prefix
read against Linux SHA-256
`6e5cdc10b948959d2f419beca3dd590e065d151a18b75433c36a3de520e6a265`,
all four component hashes, one RNDIS notification worker and strict zero BFS
allocation counters on the USB boot filesystem. No USB check-sum messages or
control-request timeouts occurred during either check window.

Normal reboot restored authenticated Haiku access, and session recovery returned
ROOBI `c5422540-cfee-418d-b9fe-847b7f587914`. The guard disarmed without a
NanoKVM restart. Evidence is indexed by `state/native-onboard-host.json`.
This accepts the expanded firmware host's enumeration and bounded NVMe read
milestone. AHCI was blocked, RTL8125 was absent, and peripheral operation,
interrupt routing and sustained host qualification remain open. The SSD's
installed packages were not replaced by this USB-image trial.

The CPU-generated GIC message diagnostic from `5e961c4459` passed three runs
across two USB boots, including a normal Haiku reboot, in
`interactive/20260912T211720Z-6d22f2`. The 24 messages all arrived exactly once
on CPU 0, half through the distributor and half through its DT alias. Each
run passed quiet intervals and restored the selected vector's trigger state,
with delivery disabled and inactive afterward. Both boots retained eight PCI
functions, verified five component hashes, matched the 64 MiB NVMe EFI-prefix
Linux hash, and passed strict zero BFS allocation counters and one RNDIS
notification worker. No USB checksum or control timeout errors occurred in
these workload windows. QEMU passed its rejection-before-MMIO and existing
storage/USB/reboot gates in `qemu-shell/20260912T145447Z-7b7d61`.

Recovery returned ROOBI `adf214ad-c5dc-4348-85b0-efe008f0aade`; the guard
was disarmed and NanoKVM did not restart. `state/native-mbi-probe.json` and
[MBI-PROBE.md](MBI-PROBE.md) record the scope and the earlier register-view
rejection. This proves CPU-generated message delivery on reserved ID 464.
The opt-in kernel MSI provider from `6a613b204d` leases IDs 464 through 479.
Its QEMU gate passed in `qemu-shell/20260912T213553Z-9f5c4b`, but native session
`interactive/20260912T213902Z-a71c6d` received no SSD messages. NVMe enabled
MSI-X, timed out once and recovered through its existing polling fallback.
Two 64 MiB EFI reads still matched the Linux hash.

The read-only MSI inspector from `29f14a00b0` passed QEMU rejection and the
existing regression gates in `qemu-shell/20260912T215411Z-b066fc`, then captured
identical state before and after the second native read. The SSD's unmasked
table entry held the intended address `0xfe610040` and data 464. That GIC ID
was enabled, edge-triggered and routed to CPU 0, with no pending or active
interrupt. PHP_GRF ITS address selectors retained `0xfe65`/`0xfe67`. The reads
do not establish where messages were lost; no routing registers were changed.

Final checks passed all six component hashes, strict zero BFS allocation
counters and one RNDIS notification worker. There were no USB checksum or
control timeout errors in the workload window. Recovery returned ROOBI
`4488812d-d230-491b-b6ec-2385f8110a39`, the guard disarmed and NanoKVM stayed
on the same boot. `state/native-mbi-provider.json` retains the delivery failure
and `state/native-msi-inspect.json` records the inspection/recovery evidence.

Linux reference `linux-nvme-msix/20260912T220657Z-de3cda` then captured nine
unmasked NVMe MSI-X entries targeting ITS1 at `0xfe670040`, with event data
0 through 8. A matching 64 MiB EFI read generated 513 NVMe interrupts; the
table remained unchanged. `state/linux-nvme-msix.json` retains the actual
resources, kernel and before/after counts. At that checkpoint ITS and
noncoherent network DMA were unimplemented; the later ITS trial is recorded
below. The installed SSD continues to use its previously qualified components.

The initial [ITS1/LPI provider](ITS.md) from `3ce6f6c457` passed the ARM64 build,
73 host checks and QEMU rejection/regression gates in
`qemu-shell/20260912T223910Z-1aec32`. Native USB session
`interactive/20260912T224223Z-79bc34` then delivered Samsung NVMe MSI-X
interrupts on CPU 0, LPI 8192, through ITS1 at `0xfe670040`. The initial boot
and a normal Haiku reboot both logged interrupts during the 64 MiB EFI-prefix
read, reaching a logged count of 512 and matching the Linux hash. Six component
hashes, eight PCI functions, one RNDIS notification worker and zero USB BFS
allocation counters passed. No interrupt timeout, polling fallback, quarantine,
unexpected ID or USB checksum/control timeout occurred in either read window.

A subsequent write-test setup in that session failed on the script's incorrect
unversioned package filename. Only a new file's two 8 MiB guards had been
written; the main workload never started. The failed script, partial file and
session error remain preserved. Automatic recovery returned ROOBI
`1368635e-ecc8-4384-afde-40edf7177e7b`, the guard disarmed and NanoKVM retained
its boot ID. `state/native-its-provider.json` records the completed read/reboot
milestone separately from this setup failure. Write qualification is pending.
The installed SSD components remain unchanged. Other ITS devices and CPU
targets, allocation/free/reuse and noncoherent network DMA remain open.

The corrected write trial passed in `interactive/20260912T230705Z-74d328`
using the same immutable `hrev60097+86` USB image. It verified all 11 installed
package hashes before creating a new 2064 MiB file. Eight workers pinned to
CPUs 0 through 7 wrote four rounds over its 2 GiB region, with successful
`fsync()` and peer reads after each round. The 8 GiB write/8 GiB peer-read
workload passed in 33.5 seconds; this bounded regular-file check is not a
sustained disk benchmark. Independent region SHA-256 and the two 8 MiB guards
matched. A normal Haiku reboot restored authenticated access in 94 seconds.
Eight-worker readback, independent hashes, packages and strict zero allocation
counters on SSD and USB BFS all passed afterward.

ITS interrupt counts reached logged thresholds 65536 before reboot and 16384
afterward, with arrivals in both workloads. Four preflights verified all six
components, the PCI inventory, USB root and one RNDIS notification worker.
The accepted windows contained no interrupt timeout, polling fallback, ITS
quarantine, unexpected interrupt ID or USB checksum/control timeout. Recovery
returned ROOBI `f5b86238-1904-4170-96ed-8a208f14047c`; the guard disarmed and
NanoKVM did not restart. `state/native-its-storage-stress.json` indexes the
complete evidence. The failed setup record and partial file remain preserved.
ITS tables were below 4 GiB in these boots; a separate opt-in high-table
allocation trial is now built and awaiting QEMU/native qualification.

That high-table trial also passed. Build `b8571b931a` (`hrev60097+88`) and all
73 host checks passed, followed by QEMU in `qemu-shell/20260912T232143Z-aad50b`.
Native session `interactive/20260912T232610Z-fc067f` allocated all six ITS/LPI
tables above 4 GiB on both boots; [ITS.md](ITS.md) records the actual addresses.
Every range remained aligned and entirely below the GIC's 32 GiB limit.
Another 8 GiB of eight-worker writes, four `fsync()` calls and peer reads
passed, followed by guard/region/package hashes and normal reboot readback.
The two boots logged interrupt thresholds 65536 and 16384 during the accepted
windows without polling fallback, quarantine, unexpected IDs or USB control
errors. Component hashes, PCI inventory, one RNDIS worker and strict zero
filesystem allocation counters passed throughout.

Recovery returned ROOBI `b6ea3fc1-1ada-42b2-a4f6-0ffed26a3443`; the guard
disarmed and NanoKVM did not restart. The evidence index is
`state/native-its-high-storage-stress.json`. The installed SSD still has its
earlier packages; an emulated upgrade rehearsal is in progress before changing
that installation. General ITS lifecycle, other devices and CPU targets, and
noncoherent network DMA remain open.

## Installed ITS1 update and intermittent startup stall

The `hrev60097+88` upgrade passed the full-capacity QEMU Installer rehearsal
in `qemu-shell/20260912T233237Z-1c30ad` and two NVMe-root boots in
`qemu-shell/20260912T235232Z-80a7ea`. Native Installer then completed in
`interactive/20260912T235929Z-4b5d53`. All 11 packages matched the source image;
before/after checks preserved two 2 GiB data regions, guards, network overrides
and the existing EFI loader, with strict zero allocation counters on both
filesystems. The installed settings now enable the onboard PCI profile and
ITS1, with no high-table requirement or MBI setting.

Installed session `interactive/20260913T001949Z-8efbf6` reached the desktop
with `/boot` on `/dev/disk/nvme/0/1` and verified all six components. NVMe
interrupts arrived through ITS1 on CPU 0/LPI 8192. Eight-worker verification
and independent hashes passed for both data regions, guards and all packages.
TRIM completed for 222279335936 free bytes; immediate readback and filesystem
checks passed. Normal Haiku reboot returned to ROOBI with BootOrder unchanged.

The next one-shot SSD boot, `interactive/20260913T002814Z-a838e2`, stalled
before the desktop and remote shell. It had mounted the SSD, enabled ITS1,
received interrupts and completed package-daemon volume verification. There
was no reported panic or NVMe timeout, and the keyboard debugger request
produced no response. Recovery succeeded. The failed startup is retained in
`state/native-its-installed-boot-stall.json`; its cause is still unknown.

Retry `interactive/20260913T003535Z-5cadd4` reached the installed desktop and
passed both regions, guards, all package/component hashes, one RNDIS worker
and strict zero allocation counters. The two accepted sessions logged NVMe
interrupt thresholds through 32768 during their workloads, without polling
fallback or ITS quarantine. The retry rebooted normally to ROOBI
`0d7bb955-5b56-4139-ad05-624394ab219c`. All guards disarmed; NanoKVM did not
restart. `state/native-its-installed-update.json` accepts the bounded update,
TRIM and data-persistence checks while keeping startup reliability open.

The Installer session also captured a Time preferences process crash in
`GetAvailableTimeZonesWithRegionInfo`. Read-only inspection on the successful
SSD retry confirmed that ICU's compiled data directory names the old bootstrap
package, while the actual data is under the renamed package and
`/boot/system/data/icu/74.1`. This is a separate desktop defect under investigation.
See [ITS.md](ITS.md) for the installation evidence and limits.

The [startup investigation](STARTUP.md) found and reproduced a launcher pipe
inheritance bug. QEMU baseline `qemu-shell/20260913T005314Z-9796e1` compiled the
actual environment reader and demonstrated that an unrelated executable could
keep it waiting for EOF. The `pipe2(O_CLOEXEC)` candidate passed the focused
test in `qemu-shell/20260913T005634Z-1d0ab4`; deliberately clearing the flags
reproduced the failure. This does not yet establish the cause of the native
stall. The observer and combined full-image/native qualification are recorded
below; the installed SSD still has its accepted `hrev60097+88` components.

## Startup observer and terminal command transport follow-up

The original `+88` SSD completed five early thread/descriptor snapshots in
`interactive/20260913T014533Z-453fff`. Observer file hashes survived reboot;
six platform/storage component hashes plus the original launcher, both flushed
2 GiB data regions and guards, eleven package hashes and zero BFS allocation
counters passed. The traced boot reached the desktop. The earlier intermittent
startup stall remains unexplained. See [STARTUP.md](STARTUP.md).

Investigation also found a [terminal partial-write bug](TTY.md), reproduced
in QEMU and on the physical SSD: a nonblocking write returns `EAGAIN` after
accepting 4,096 bytes. A focused source fix and probe are now present. The lab
transport waits for the shell prompt, verifies received command-file hashes
before execution and paces input.
Its QEMU fresh-login/reboot/shutdown checks, native command checks and 75 host
checks passed. Earlier failed transport/install attempts remain separately
recorded and do not count as native startup or storage passes.

The combined `hrev60097+94` image passed two full QEMU boots in
`qemu-shell/20260913T021120Z-799210` and native USB session
`interactive/20260913T022437Z-e73a47`. The terminal probe accounted for all
64 KiB in sixteen partial writes; six QEMU and three native fresh logins each
preserved an 82,944-byte literal command with pacing disabled. The ROCK also
passed the focused launcher probe and its deliberate inheritance negative
control, all eight component hashes and five startup snapshots. Old SSD
packages, both 2 GiB test regions, their guards and zero BFS allocation counters
passed before recovery. ROOBI returned after SSD unmount; NanoKVM did not restart
and its watchdog disarmed. `state/native-tty-fixed.json` accepts this bounded
USB-boot qualification. The physical SSD update and boot checks below followed
this gate.

The physical `+94` installation and two installed boots have now passed.
Installer session `interactive/20260913T025330Z-b22da2` verified all eleven
packages, preserved test data and the EFI loader, and returned clean SSD/USB
allocation counters. Installed sessions `interactive/20260913T031611Z-a505e7`
and `interactive/20260913T032252Z-6d76ed` each passed eight component hashes,
the terminal probe, five startup snapshots, both 2 GiB test regions with their
guards and all package hashes. NVMe MSI-X interrupts continued during reads;
normal reboot returned to ROOBI after each session. UART capture had no errors,
and NanoKVM remained up with its watchdog disarmed. The original startup stall
remains unresolved. `state/native-startup-installed-update.json` and
[STARTUP.md](STARTUP.md) record the scope.

A separately QEMU-qualified, 498-byte ICU settings file was installed after the
second read check. SSD boot `interactive/20260913T033150Z-f33ee2` inherited the
correct data directory, passed timezone/number-format probes and opened Time
preferences without crashing. Its date/time and expanded timezone views were
inspected; component/file hashes, five startup snapshots, filesystem checks and
normal reboot to ROOBI also passed. This is `+94` plus the settings file, not a
native qualification of the full `+96` USB image. The evidence index is
`state/native-icu-installed-environment.json`. RTC/NTP and general ARM64 package
completeness remain open.

## ARM64 BSD network DMA preparation

The next Ethernet prerequisite is implemented on `rock5-network-dma` and remains
unqualified on native NIC hardware. Private ARM64 DMA allocations use Normal
Non-cacheable RAM; descriptor rings stay direct and ordinary packets use
preallocated bounce buffers. Mapping ownership, interval checks, failure cleanup
and partial synchronization are covered by the actual-source host tests. All
76 host checks passed; RTL8125 and the Intel QEMU fixture driver cross-linked.
The complete `+98` image built from `c68654e89d`. Its first combined QEMU boot
obtained DHCP on PCI Ethernet but failed USB control readiness after a RNDIS
response timeout. The same image passed the ordinary USB/storage/platform gates
without the extra NIC. A combined retry then passed an 8 MiB PCI round trip
before and after normal reboot, USB control, NVMe persistence and shutdown;
packet capture independently confirms at least 16 MiB each way on the PCI
interface. The original USB timeout remains open. This is a QEMU DMA milestone,
not native Ethernet qualification. The physical SSD still has its accepted
`+94` installation and ICU setting, and the ROCK remains in ROOBI. See
[NETWORK-DMA.md](NETWORK-DMA.md) for evidence, the Intel fixture's limitations
and the remaining native interrupt-routing work.

## Native Ethernet bring-up

The experimental path adds a separate 32-bit PCI INTx module and an opt-in RK3588
provider for the two onboard RTL8125 endpoints, GIC IRQs 277 and 282. It validates
the retained firmware resources and enables INTA only after handler installation.
The Realtek top half now masks level interrupts before scheduling its worker;
setup failure cleanup also handles suspended workers. Review additionally found
and corrected rejection of RTL8125's unrestricted parent DMA tag, which had
silently lost the driver's 32-bit address ceiling. Review also corrected a
hard-coded root-device unit that associated both Realtek interfaces with the
first device, and checked interface allocation before enabling bus mastering.

All 77 final host checks passed, including injected failure of the required
ARM64 interrupt module. The full `hrev60097+102` image built from `fdcd191d20`.
QEMU in `qemu-shell/20260913T052005Z-47931a` passed memory/platform/cache/service
checks, USB control and transfers, NVMe persistence, normal reboot and shutdown.
The Intel PCI fixture passed an 8 MiB round trip before and after reboot, with
independent returned-file hashes and packet capture confirming at least 16 MiB
of TCP payload each way. Both boots used IRQ 35 through the new interface and
rejected the unrelated RK3588 host profile. The preceding `+101` candidate's
QEMU pass is retained separately.

The first native `+102` trial rejected the NIC roots because it incorrectly
required a bit in a reserved register. The corrected `+104` image passed all
77 host checks and the same two-boot QEMU gates. Native session
`interactive/20260913T055134Z-f7b607` then attached both Realtek interfaces,
enabled IRQs 277/282 and observed interrupts. The connected port obtained DHCP
at 1 Gbit/s. Component/settings hashes, USB control, desktop and a short memory
check passed. Its first bulk-transfer test failed on a missing guest directory,
before a payload checksum; the script is corrected and the failure retained in
`state/native-network-intx-mask-only-first.json`.

`state/network-intx-checkpoint.json` records the newer software/emulation gate.
The repeat `+104` native session `interactive/20260913T055957Z-afe24b` passed
8 MiB round trips on port 1 at a negotiated 1 Gbit/s, before and after normal
reboot and after an interface down/up cycle. Guest and independent workstation
hashes matched, truncated input was rejected, and interface counters confirm
Ethernet carried the payload. No error/drop counter grew during transfers;
one receive error appeared during interface shutdown and its source was not
isolated. Both boots passed component/settings hashes, USB control and a short
memory check. Recovery completed without a NanoKVM restart.
`state/native-network-intx-qualified.json` records this bounded milestone.

The owner then connected the second Ethernet cable through a 10 GbE copper SFP.
Linux negotiated 2.5 Gbit/s full duplex on port 0, while port 1 retained 1 Gbit/s.
A native `+104` port 0 transfer passed but exposed a reporting defect: the PHY's
extended 2500BASE-T media value was truncated to a 10 Mbit/s value, and its
graphical speed label was blank. The correction preserves the extended subtype
and adds 2.5/5 Gbit/s labels to the shared formatter.

All 78 host checks and the complete `+106` QEMU gates passed. Native session
`interactive/20260913T063242Z-e0d8da` initially stalled in firmware before the
Haiku loader; one controlled reset recovered it. That failed attempt remains
unresolved. Both subsequent Haiku boots passed twelve component hashes, settings
hashes, USB control and the short memory check. The kernel, `ifconfig` and
Network preferences reported port 0 at 2.5 Gbit/s. Port 0 passed 8 MiB round
trips before and after normal reboot, and port 1 passed on the same image after
reboot. Each phase disabled the other Ethernet interface to identify its path,
checked hashes independently on the workstation, and rejected truncated input.
No error/drop counter increased during transfers. Recovery and serial capture
completed, and NanoKVM remained up with its watchdog disarmed.
`state/native-network-media-qualified.json` records the limited driver checks
and the firmware retry separately.

At that checkpoint, static IPv4/IPv6, simultaneous traffic, sustained load,
throughput and error recovery remained open. The earlier USB timeout and native
startup stall also remained open. The SSD remained at `+94` with its ICU setting. See
[ETHERNET.md](ETHERNET.md) for implementation, evidence and remaining limits.

## Concurrent Ethernet streams and measured performance gap

Source `9dc5aaaa45ebb6fe3394082f056005bda5d9ae6a`, image `hrev60097+108`, adds a
bounded memory-stream fixture with position/seed-dependent data verification,
explicit source addresses and a receiver acknowledgement. All 83 host checks,
the full ARM64 build and the QEMU boot/storage/power gates passed. Both QEMU
boots also passed simultaneous PCI send/receive streams.

Native session `interactive/20260913T072325Z-450eb7` passed on its first boot
and after a normal reboot. Each boot verified thirteen components, three
settings, USB control, DHCP/link reporting and the short eight-worker memory
check. Both physical Ethernet ports passed simultaneous sending and receiving
on separate static IPv4 subnets: 32 MiB and 128 MiB per stream before reboot,
then 128 MiB per stream afterward, each with seven additional bytes to exercise
a partial word. Total verified payload was about 1.125 GiB. Complete captures
proved the MAC/path of every stream and overlapping use of both ports; capture
drops and Haiku interface error/drop counters stayed at zero.

Throughput is a substantial remaining gap. First-boot Haiku streams measured
about 25–37 Mbit/s; the rebooted run with a CPU sample measured about 77–144
Mbit/s. The cause of that variation is unidentified. The Realtek interrupt
workers consumed most of one CPU's time combined while overall utilization
was around 19–21% of eight CPUs. Their shared synchronization is a lead for
investigation, not yet a diagnosed root cause.

Using the same 128 MiB fixture, recovery Linux measured about 1.42 Gbit/s
receiving and 2.29 Gbit/s sending on port 0 through the owner's SFP connection,
and 614/937 Mbit/s on port 1. Those figures have complete independent packet
coverage. One earlier Linux capture dropped eight packets although its data
checks passed; it is preserved separately. Vendor Linux RX byte counters were
implausible and were not used to calculate rates or traffic amounts. Details,
setup failures and artifact paths are recorded in [ETHERNET.md](ETHERNET.md).

The bounded results are in `state/native-network-stream-qualified.json` and
`state/linux-network-stream.json`. Serial capture and ROOBI recovery passed;
NanoKVM did not restart and its watchdog disarmed. Temporary addresses and
capture processes were removed. The SSD remains at `+94`. Throughput, IPv6,
long mixed load, link/fault recovery and the previously recorded boot/control
failures remain open, as do the broader hardware rows in the roadmap.

## ARM64 memory-copy improvement on native hardware

Source `381da7d16110257e49bb9a89d12073120b1db039` (`hrev60097+110`) replaces the
generic ARM64 byte-copy fallback for mismatched alignment with bounded word
copies in the kernel and libroot. Received Ethernet packets use precisely that
mismatched alignment. Disassembly of both built entry points confirms paired
general-register loads/stores without SIMD or function calls. The drivers,
network stack, benchmark executable and firmware settings match `+108`.

All 84 host checks and the full QEMU gate passed. An actual-entry copy probe
passed 51,301 alignment, canary and protected-page cases on both QEMU boots and
both native boots. Native session `interactive/20260913T082603Z-4a96fc` also
passed fifteen component hashes, memory checks, both DHCP links, a normal
reboot, and four concurrent network runs totaling about 2.125 GiB. Every run
has complete packet coverage and zero interface/capture errors or drops.

The matching 128 MiB after-reboot comparison improved from `+108`'s
144.5/86.1 Mbit/s receive/send to 265.3/158.3 on the 2.5 Gbit/s port, and from
120.5/76.7 to 335.6/170.3 on the 1 Gbit/s port. The longer 256 MiB run measured
241.2/123.0 and 253.1/145.3 respectively. Rates remain variable and well below
Linux; this is bounded progress, not sustained Ethernet acceptance.

The SSD was mounted read-only to check both existing 2 GiB test regions using
eight workers and independent hashes, all four surrounding guards and all
eleven installed package hashes. Those checks passed and the volume was
unmounted. Its installed `+94` system was not updated. Full evidence, pins and
the recovery receipt are in `state/native-arm64-memcpy.json`; implementation
and measurement details are in [ETHERNET.md](ETHERNET.md).

## Bound Realtek receive copies to validated fragment lengths

Source `6ffbb12e732b85405f9e02a02d7911b3c5dc5d32` (`hrev60097+112`) validates
each receive fragment against its mapped buffer before exposing its length.
Valid fragments synchronize only received bytes; invalid lengths retain full
synchronization and use the existing discard path. The production receive
routine passed sanitizer tests with short/full buffers, errors, fragments,
ring wrap and hardware-owned descriptors. All 85 host checks, the full ARM64
build and the QEMU gate passed.

Native session `interactive/20260913T090225Z-82d0ed` passed first boot and one
normal reboot, fifteen component hashes and four concurrent network runs
totaling about 2.125 GiB. Each run has complete packet coverage, both physical
paths and zero interface/capture errors or drops. The longer 256 MiB streams
measured 313.5/187.5 Mbit/s receive/send on port 0 and 298.5/192.2 on port 1,
improving over `+110`'s corresponding 241.2/123.0 and 253.1/145.3. Shorter
comparisons were mixed; a uniform speedup and Linux parity are not established.

The SSD was not mounted or updated and remains at `+94`. Results and recovery
are recorded in `state/native-rge-receive.json`. [ETHERNET.md](ETHERNET.md)
contains the exact image, reference sources, bounded acceptance and remaining
limits, plus a resolved local snapshot-inspection error. IPv6, throughput
variation, sustained mixed load and link/fault recovery remain open alongside
the rest of the hardware roadmap.

## Static IPv6, address replacement and neighbor discovery

Source `87ab8f7dc8149a89a9964b7d789d2f93ccfccb3d` (`hrev60097+117`) fixes two
IPv6 failures found by the physical two-port fixture. Neighbor discovery now
sends through the interface owning its source address, and changing that
address replaces NDP's cached source even while the interface still exposes
its old address list. The checked stream probe now supports IPv6 literals.

All 88 host checks and the full ARM64 build passed. QEMU passed its existing
platform/storage/control/power suite plus simultaneous IPv4 and IPv6 streams
on both boots. Native session `interactive/20260913T103638Z-5861fd` passed
seventeen component hashes, three settings, memory/copy checks and both DHCP
links before and after normal reboot. The SFP-connected port remains at
2.5 Gbit/s and the original connection at 1 Gbit/s.

Three native IPv6 runs checked about 1.125 GiB, including replacing both
addresses before reboot and configuring fresh addresses afterward. Both larger
runs also passed workstation-initiated discovery and ICMPv6 echoes. A final
IPv4 regression adds 128 MiB. Complete captures prove every stream's physical
path and concurrent operation on both ports; interface and capture errors/drops
remain zero. The larger IPv6 runs measured 360/202 then 477/213 Mbit/s
receive/send on port 0, and 276/193 then 324/210 on port 1. The matching Linux
reference measured 1145/1698 and 803/897 Mbit/s, so throughput remains a gap.

The first `+114` IPv6 failure and `+115` address-replacement failure remain
preserved with their evidence. An earlier diagnostic timeout was traced to
`route get` dereferencing a missing gateway in userspace; that command still
needs correction. Automatic IPv6 configuration, scoped/link-local addresses,
duplicate-address detection, general multicast, sustained load and fault
recovery are outside this qualification. [ETHERNET.md](ETHERNET.md) records the
implementation, fixture corrections, pinned artifacts and limits.

The complete result is `state/native-ndp-source.json`. Serial capture and ROOBI
recovery passed; NanoKVM retained its boot ID and its watchdog disarmed. The SSD
was not mounted or changed and remains at `+94`. All other open hardware rows
in the roadmap remain active.

## Route diagnostics and IPv6 longest-prefix selection

Source `196adb96e8868cada83de07fbd7bfcc0f6fb6d8a` (`hrev60097+120`) corrects
IPv6 route ranking so more specific prefixes take precedence over a default.
The preceding route-utility fix also handles absent host masks and direct-route
gateways. Its new runtime checks exposed the routing defect in QEMU; that
failed `+119` run is retained and was never deployed to the board.

All 89 host checks, the full build and the full QEMU regression suite passed.
QEMU additionally passed ten real route-query cases on each boot. Native session
`interactive/20260913T110601Z-7c7921` passed twenty route-query cases across both
interfaces, eighteen component hashes, settings, memory/copy checks and DHCP
at 2.5/1 Gbit/s. Three IPv6 runs passed simultaneous traffic with a default
through port 0, including address replacement and normal reboot. Captures
prove that port 1 retained its more specific physical path. Discovery and echo
checks pass in both directions. A final IPv4 regression also passes.

Total native payload is about 1.25 GiB across 371,192 captured frames, with no
interface/capture errors or drops. IPv6 rates remain about 154–309 Mbit/s in
these bounded runs, below the earlier Linux reference. Throughput variability
and sustained acceptance remain open. Actual forwarding through an IPv6 router
and automatic IPv6 configuration remain unqualified.

Evidence and limits are in [ETHERNET.md](ETHERNET.md) and
`state/native-ipv6-route-mask.json`. Recovery and serial capture passed, NanoKVM
stayed up with its watchdog disarmed, and the temporary workstation fixture was
removed. The SSD remains at `+94` and was not mounted or updated.

## ARM64 AHCI and native SATA controller initialization

Source `5f8d9b0ba429a4e968b33bf02bf950e45edd3b2e` (`hrev60097+124`) adds
ARM64 private noncacheable AHCI buffers, bounded scatter/gather copies and
completion ordering, and uses the managed 32-bit PCI interrupt interface.
The RK3588 segment-one profile admits the documented legacy INTID 287.
Controller and request error paths have explicit cleanup and ownership checks.

All 92 host checks, the full build and the complete QEMU suite passed. Two
independently seeded 512n/512e SATA disks passed concurrent I/O, high-offset
and guarded partial-sector writes, explicit drive-cache flushes and normal
reboot readback. Six host hashes checked the backing files after shutdown.
An earlier unsupported 4Kn QEMU fixture failed before boot; that evidence is
retained, and 4Kn I/O remains untested.

Native session `interactive/20260913T122354Z-c3acae` passed desktop startup,
twenty component hashes, four settings and memory/copy checks on both sides
of a normal reboot. An explicit setting limits ASM1164 probing to its four
direct ports. The previous unmasked `+123` run reached the remote shell but
also probed sixteen virtual ports, delaying boot; it remains a separate failed
four-direct-port readiness attempt. The default driver still permits virtual
ports for port-multiplier configurations.

Concurrent IPv4 before reboot and IPv6 afterward checked 268,435,512 bytes
over both Ethernet ports, with 90,505 captured frames and no capture drops or
interface errors. IPv6 discovery, echo and specific-route selection with a
default route present also passed. Negotiation remains 2.5/1 Gbit/s, while
short transfer rates of 143–396 Mbit/s remain below the Linux reference.

The native scope is controller initialization and reboot: no SATA disk is
attached, so physical disk I/O, interrupt delivery under I/O, DMA coherency
and durability remain untested. [SATA.md](SATA.md) records implementation,
artifact pins, retained failures and the remaining acceptance gates.
`state/native-ahci-controller.json` and the session's `qualification.json`
record the pass. Serial capture and ROOBI recovery completed, NanoKVM stayed
up and its watchdog disarmed. The Samsung SSD was not mounted or updated
and remains at `+94`; the rest of the hardware roadmap remains active.


## Native eMMC identification and read-only data

Source `cee0be9cdfeab92e5ef8504bd7e4d7ede498791f` (`hrev60097+131`) adds
native RK3588 eMMC attachment with an explicit firmware profile, private
noncacheable DMA below 4 GiB and conservative legacy clock setup. The final
initialization correction returns the card to idle after an unsupported SD
probe, clearing the stale error that had prevented MMC address assignment.
The preceding clock and RCA failures remain preserved in [MMC.md](MMC.md).

All 101 host checks, the ARM64 build and the combined QEMU suite passed.
Native session `interactive/20260913T150450Z-7f2149` passed read-only geometry
and three 8 MiB region hashes on both sides of a normal PSCI reboot. Capacity
is 7,818,182,656 bytes, matching Linux. The regions include offset 5 GiB and
the end of the device; all six native hashes match the Linux references.
Short reads measured about 7.8–8.3 MB/s in the conservative mode.

Both boots also passed twenty-four component and five setting hashes, memory
and copy checks, and inspected Tracker/Deskbar desktops. The Ethernet links
remain at 2.5/1 Gbit/s with DHCP. The native trial did not write or flush eMMC,
force CPU caller buffers above 4 GiB, repeat network traffic or update the
`+94` SSD installation. One transfer-complete interrupt diagnostic appeared
during each initialization; its timing cause remains unisolated despite the
successful EXT_CSD and data reads.

Serial capture completed without transport errors. ROOBI recovered with boot
ID `e03fe45c-89ea-4529-94e9-95f820a9070d`, and independent Linux reads still
match all three reference hashes. NanoKVM stayed up and its guard disarmed.
`state/native-mmc-read-only.json` records this bounded pass. Native writes,
flush/power-cycle persistence, faster speed modes, sustained/error recovery
and booting Haiku from eMMC remain open, alongside the rest of the roadmap.


## Native eMMC file writes and independent Linux readback

Source `921b671c8e616ce7185b8a8050350d383a1a5dad` (`hrev60097+136`) passes
bounded file writes on the previously empty, separately backed-up 300 MiB eMMC
FAT partition. The tested changes also correct partition block-size publication
and the geometry helper's handling of exact partition extents. The QEMU fixture
now rediscovers uniquely sized cards after asynchronous device renumbering.
Its three preceding failures remain preserved in [MMC.md](MMC.md).

All 104 host checks, the ARM64 build and the combined QEMU suite pass. Native
session `interactive/20260913T155758Z-aeeb8f` completed an 8 MiB overwrite and
five writes at odd byte offsets inside an existing 16 MiB file. Each operation
passed explicit MMC flush and complete source/target hashes after a fresh
read-only mount. The final hashes pass after normal PSCI reboot as well.
Total checked file content is 192 MiB; another 48 MiB of raw reference reads
outside the test partition matches Linux across both boots.

Both boots pass twenty-five component and five setting hashes, memory/copy
checks, inspected desktops and DHCP at 2.5/1 Gbit/s. The SSD remains at `+94`
and was not mounted or updated. This run does not repeat network throughput
qualification. The eMMC card reported cache disabled on both boots; native
cached-card flush and power-cycle integrity are still open, along with faster
modes, sustained/error recovery, high-memory callers and Haiku boot from eMMC.
Transfer-complete diagnostics during initialization remain recorded.

ROOBI recovered with boot ID `d0ba2e20-9591-432c-9bd3-d493fbd682d9`. Linux
independently copied the unmounted FAT partition; host filesystem checks and
both file hashes pass. The other reference regions remain unchanged. FAT/Tracker
metadata includes empty recycling directories, and the final fixture is retained
for further tests. Serial capture completed without errors and the NanoKVM guard
disarmed. `state/native-mmc-filesystem.json` records this bounded pass. The
ordinary profile remains read-only by default; only the private trial enables
writes. The remaining hardware roadmap stays active.


## Native eMMC orderly shutdown and startup persistence

The unchanged `hrev60097+136` image passes a separate native shutdown/startup
trial in `interactive/20260913T163033Z-bde4d4`. Haiku overwrote 4 MiB inside
the retained FAT target file, flushed it and verified both complete files
after a fresh mount. Normal shutdown reached the PSCI system-off call. More
than 107 seconds later, NanoKVM remained reachable, its USB gadget reported
detached and no new Haiku boot had started. HDMI capture timed out while off.
A single 800 ms power-button pulse started the same Haiku image; the complete
file hashes and three raw reference hashes pass after startup.

Both boots pass the component/setting hashes, memory/copy checks, inspected
desktops and DHCP links at 2.5/1 Gbit/s. Native checked payload totals 72 MiB
of file content and 48 MiB of raw references. The existing build, 104 host
checks and combined QEMU evidence cover this unchanged image. The SSD remains
at `+94`; no network traffic qualification was repeated.

ROOBI recovered with boot ID `a6a13da0-52b7-4de1-b6f0-aecb9ae716df`. Linux
independently verified both files, FAT consistency and the unchanged reference
regions. Serial capture completed without transport errors and the NanoKVM
guard disarmed. `state/native-mmc-shutdown.json` records the pass and the final
fixture remains available. [MMC.md](MMC.md) has the evidence paths and scope.
Abrupt power loss and electrical removal of the eMMC supply were not tested.
The card cache remained disabled; cached-card flush, high-memory callers,
faster modes, sustained/error recovery and eMMC boot remain open.


## Native eight-bit eMMC legacy SDR

Source `fb1811b338f1f2cf1972d034d7391eff3abfdc01` (`hrev60097+139`) selects
the eMMC bus width from the host's admitted wiring before publishing the card.
It validates a fresh EXT_CSD read at that width and preserves the verified
mode when the disk opens. The ROCK profile supplies eight-bit wiring; hosts
without a wiring description retain the four-bit default. Host failures and
corrupt identification/capacity data are rejected before disk publication.

All 105 host checks, the ARM64 build and the combined QEMU suite pass. QEMU
exercises the revised path at its four-bit default. Native session
`interactive/20260913T170301Z-f34335` verifies eight-bit mode on two boots
separated by normal PSCI reboot. Read-only geometry, three raw reference
regions and both retained FAT files pass on each boot: 96 MiB checked in total.
Short raw reads measured 11.1–11.7 MB/s in the existing legacy clock setup.
Sustained throughput and faster clock modes remain unqualified.

Both boots pass twenty-five component and five setting hashes, memory/copy
checks, inspected desktops and DHCP at 2.5/1 Gbit/s. The read-only profile
issued no native file writes or flush ioctls; eight-bit write persistence is
the next storage gate. The earlier four-bit write/shutdown result remains
available. Cache stayed disabled, caller buffers above 4 GiB were not forced,
and transfer-complete diagnostics remain recorded. The SSD stays at `+94`.

ROOBI recovered with boot ID `a84eee7f-49db-407d-b875-aa8f22bbbfa5`. Linux
verified that all 300 MiB of the FAT fixture remained byte-for-byte unchanged,
passed FAT consistency and file-hash checks, and confirmed the three raw
references. Serial capture completed without transport errors and the NanoKVM
guard disarmed. `state/native-mmc-width.json` and [MMC.md](MMC.md) record the
scope and evidence. The remaining hardware roadmap stays active.


## Qualified board components installed on NVMe

The Samsung 950 Pro has been updated from `hrev60097+94` to the exact qualified
`+148` components, source `bc21c9ef581ef034a4f79ef2185b4e101ac3772b`.
The standard Installer replaced three generated Haiku packages; nineteen
configuration/helper files were staged, hashed and renamed. Eleven package
hashes, old-package/settings backups, the retained EFI loader, guarded file
regions and both filesystem checks passed before recovery.

Two accepted native SSD boots passed component/settings/package hashes, the
startup observer, memory/copy/PTY checks, both guarded 2 GiB NVMe regions, ITS1
MSI-X delivery, concurrent IPv4 traffic, four-port SATA initialization and
read-only cached eMMC checks. Both ports obtained DHCP at 2.5/1 Gbit/s. Eight
streams verified 256 MiB plus 56 bytes with complete physical-path captures,
zero interface errors/drops and no capture drops. Each normal reboot returned
to ROOBI; NanoKVM stayed up and both guards disarmed.

The preceding updated SSD boot did not pass: its initial SATA PCIe profile
was rejected and AHCI never attached, despite later PCI discovery. No file or
setting change preceded the two accepted repeats. The rejected values were
not logged, so this is an unresolved startup issue, not a demonstrated fix.
All three attempts are retained in [SSD-INTEGRATION.md](SSD-INTEGRATION.md).

The existing ARM64 build, combined source QEMU suite and QEMU Installer rehearsal
cover the unchanged candidate. Additional QEMU boots from the updated emulated
SSD passed full 5.12 GiB file hashes, reboot and shutdown; a further run with
two SATA disks passed I/O, explicit flush and independent persistence checks.
Linux finally verified the complete 300 MiB eMMC FAT partition unchanged,
both files, filesystem consistency and all three raw reference regions.

`state/native-integrated-installed-update.json` records the bounded pass and
limitations; `state/installed-current.json` points to the installed development
baseline. Final Linux boot ID is `a5890ee2-35c4-4cf7-ab5a-4bb1409635aa`.
The SATA startup issue, earlier startup/USB failures, sustained acceptance and
the remaining hardware roadmap stay open.

## Full ARM64 system installed on the replacement NVMe drive

On 2026-09-22 the full regular image was installed on the 256 GB SPCC NVMe
SSD. The segment-zero PCIe profile was updated for the replacement endpoint.
QEMU, the native Installer and BFS check, independent Debian GPT/EFI inspection,
and two native NVMe desktop boots passed. The second boot used persistent UEFI
BootOrder with NVMe first; NanoKVM USB, eMMC and SD remain boot alternatives.
The eMMC was backed up before installation. See
[NVME-FULL-INSTALL.md](NVME-FULL-INSTALL.md) for the exact image, hashes,
partitions, boot evidence and current polling limitation.

## ARM64 CPU identity and frequency in About this system

On 2026-09-22 the ARM64 topology gained per-core MIDR identities and maximum
frequencies from the firmware CPU operating-point tables. A native NVMe boot
and the About window showed four Cortex-A55 cores up to 1.80 GHz and four
Cortex-A76 cores up to 2.40 GHz. The updated package hash, QEMU build smoke,
screenshot and the observed EDK2 warm-restart stall are recorded in
[CPU-DETAILS.md](CPU-DETAILS.md).

## Both onboard Ethernet ports on the NVMe installation

On 2026-09-22 the EDK2 v1.1 PCIe setting was widened to the captured onboard
endpoints and legacy interrupts. Both RTL8125 ports attached, linked at
1 Gbit/s, obtained `192.168.1.*` DHCP leases, and passed isolated outbound
Internet pings. DNS worked through port 1; both restored ports answered LAN
pings. Build, profile checks and QEMU smoke passed. See
[NETWORK-PORTS.md](NETWORK-PORTS.md) for native evidence and limits.

## NVMe TRIM and MSI-X on the replacement SSD

On 2026-09-22 the 256 GB SPCC default-boot drive completed native BFS
free-space TRIM with the trimmed byte count matching the free-block count.
The image now includes the guarded ITS1 setting; the installed system booted
from NVMe using MSI-X with delivered interrupts. A 16 MiB file retained its
hash across a second TRIM, and BFS checked without allocation errors. The
file hash persisted through a reset-assisted second NVMe boot. A warm restart
paused at the EDK2 splash before that reset. The build, QEMU smoke, trim-range
and PCIe profile checks passed. See
[NVME-SUPPORT.md](NVME-SUPPORT.md) for evidence and limits.

The subsequent GLInfo launch exposed an oversized NVMe read and a
use-after-free in its error notification; the `+323` kernel panicked. The
`+326` driver bounds large vectors through DMA translation and completes each
request once. Its installed package passed the exact GLInfo repro on MSI-X,
then native TRIM and BFS check. The failed run and correction are documented
in [NVME-SUPPORT.md](NVME-SUPPORT.md).

## Mali 3D applications on the full NVMe installation

On 2026-09-22 the corrected NVMe system launched installed GLInfo, which
identified Mali-G610 (Panfrost) and OpenGL 3.1 Mesa 25.3.6. GLTeapot ran from
the installed demos with live rotation at 59 FPS using the normal system
OpenGL setup. The boot log showed the enabled Mali CSF firmware profile.
See [GPU-FULL-INSTALL.md](GPU-FULL-INSTALL.md) for screenshots, prior native
qualification and the remaining compatibility limit.

## Media decoding bring-up on the NVMe installation

On 2026-09-23 the NVMe installation completed hardware video qualification for
8-bit H.264 and H.265 through RKVDEC0 and AV1 through VPU981. Direct MPP jobs
produced NV12 frames identical to host FFmpeg for all three codecs. The Media
Kit add-on decoded video and AAC audio, then sought and decoded both tracks
again in all three Big Buck Bunny samples.

The installed `Rock5MediaPlayer` provides playback with audio, play, pause and
seek controls. Its native AV1 trial played continuously, paused, sought to
about 10 seconds while paused, and resumed with a new scene. Physical audio
output remains part of the separate board-audio item.

Every VPU job uses checked buffer handles, bounded completion polling and exact
PMU/CRU restoration. Host success, rejection and timeout tests pass. The final
native power snapshot reported RKVDEC0, RKVDEC1, VDPU and AV1 off after decode.
The package and driver were installed on the default NVMe system, which passed
a serial-captured reboot followed by Media Kit H.264 and AV1 decode-and-seek
checks from the installed package.

The final image SHA-256 is
`b37ec067153587c08c35ce0ddc28153ac14c828fbfd34cf91c6156603578ef77`;
its QEMU boot passed before the native installation. Qualification covers the
tested 1280x720 8-bit samples. Source pins, exact frame hashes, build steps and
native evidence paths are in [MEDIA.md](../../tools/rock5-itx/MEDIA.md).

## ROCK 5 ITX analog audio playback

On 2026-09-23 the NVMe installation gained a board-specific multi_audio driver
for RK3588 I2S0 and the ES8316 codec. Haiku validates the exact board, I2S
register range and GIC interrupt 212, configures power, clocks, pin mux, I2C7
and codec state, and publishes `/dev/audio/hmulti/rk3588/0`.

Native `media_client` playback of a five-second 1 kHz stereo WAV completed at
48 kHz S16. The driver started and stopped cleanly with zero underruns. The
final run had no kernel panic or media add-on fault. Four focused host checks,
the kernel add-on build, the full regular image build, and EL1 and EL2 QEMU
framebuffer gates pass. The immutable full image SHA-256 is
`d85128622cca9295322cc50ce68680dd53d8272f283c75e050cb3ea84a2f0314`.

The remote fixture does not measure the analog waveform outside the jack.
Capture, mixer controls, additional rates and formats, DMA, HDMI/DisplayPort
audio and S/PDIF remain open. [AUDIO.md](AUDIO.md) records the implementation,
Linux oracle, failed first attempts, final evidence and exact scope.
