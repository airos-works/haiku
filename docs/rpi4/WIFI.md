# Wi-Fi on the Raspberry Pi 4 (CYW43455)

State 2026-10-03: the onboard Wi-Fi scans, joins a WPA2 network and carries
traffic. Tested on the lab's network ("Gaspers", WPA2-PSK/CCMP, 802.11ac):
DHCP address, ping to the board 0.5/1.2/35.6 ms (min/avg/max of 100), and
with Ethernet taken down the board stayed reachable over Wi-Fi alone and
pulled 8 MB into a RAM disk in 1.0 s (about 66 Mbit/s, checksum matching).

## Pieces

`src/add-ons/kernel/drivers/network/wlan/broadcomfmac`:

- `dev/ic/bwfm.c`, `dev/sdmmc/if_bwfm_sdio.c`: OpenBSD's `bwfm` FullMAC
  driver, on Haiku's OpenBSD compatibility layer like `iaxwifi200`. Changes
  are under `#ifdef __HAIKU__`.
- `host/sdio_host.cpp`: an SDIO host for the BCM2711's first SD controller
  ("mmcnr", Arasan). Haiku's MMC bus has no SDIO (no CMD5/52/53), so the
  driver brings its own: WL_ON through the firmware's GPIO expander, GPIO
  34-39 to ALT3, identification, 4 bit / high speed, CMD52 and CMD53 polled,
  data through the data register. It is built apart from the driver because
  the compatibility headers stand in for the system's.
- `dev/sdmmc/sdmmc_shim.h`: OpenBSD's `sdmmc_io_*` calls on that host.
- `glue.c`: the chip cannot be enumerated, so the driver reports one device
  when the device tree has a BCM2711 (`_fbsd_init_hardware_fixed`, a new
  "fixed" bus type in `libs/compat/freebsd_network`).

Firmware: `brcmfmac43455-sdio.bin`, `.clm_blob` and `.txt` in
`/boot/system/non-packaged/data/firmware/broadcomfmac/`
(`tools/rpi4/fetch-wifi-firmware.sh`; the `rpi4-airos` profile installs
them). The driver is in the regular image's driver list for arm64.

The Pi 400 (and some Compute Module 4 boards) carry the CYW43456: the same
chip ID, 0x4345, at revision 9. The driver asks for `brcmfmac43456-sdio.*`
there, so the images ship those three files too. Without them the adapter
shows up but never finds a network (airos-works/haiku#68). Not tested on a
Pi 400: the lab has none. Its Bluetooth half is a BCM4345C5, for which
`h4bcm` has no patch file yet (it loads `BCM4345C0.hcd` only).

Joining goes the usual way: `ifconfig <device> join <network> <password>` or
the Wi-Fi preferences → net_server → wpa_supplicant → the driver; the
four-way handshake runs in the kernel's OpenBSD net80211.

## What had to change in the port

- The firmware's event structure has a 16 byte interface name; Haiku's
  `IFNAMSIZ` is 32. With the wrong size every scan result was rejected as
  "too small" (and looked almost right: the misplaced length field happened
  to read plausibly).
- OpenBSD's `systq` and the driver's own task queue are one thread in the
  compatibility layer. A command waited on that thread for a response the
  same thread had to fetch, so every command from the driver's task timed
  out. The SDIO bus task now runs on a thread of the driver's.
- The SDIO host raises the card interrupt (the controller's interrupt line,
  shared with the SD card slot's controller) and wakes that thread; the
  interrupt stays off until the thread has served the card. The controller
  holds on to the interrupt's status bit until the bit is masked, whatever
  the card does: at first only the signal was turned off, the interrupt came
  back the moment it was turned on again, and the thread went round 50000
  times a second (80 % of a core, with the interface down). The status bit
  is masked while the card is served. Turning it back on samples the card's
  line again, so nothing the card raises meanwhile is lost; the thread
  serves the card on the interrupt and, in case the controller misses one,
  twenty times a second unasked. (For a while it also served the card every
  millisecond for 300 ms after any frame. A home network's broadcasts,
  about a hundred frames a second here, kept that going all the time: 2.6 %
  of a core joined and idle, 1.5 % with nothing joined. See "What the bus
  thread costs".)
- No power saving: the driver asked the chip for it, and the chip slept
  between beacons (a ping took 30 to 300 ms). The interrupt storm had hidden
  that by keeping the bus busy. With the chip awake a ping is 1 to 2 ms,
  also after seconds of silence, which also shows that the interrupt works.
- Unloading the driver with the interface up took a mutex it did not hold
  (panic, seen when devfs reloaded the driver).
- A background scan hook, so that the network list can be refreshed while
  associated (net80211 refuses a scan request in RUN without one).
- A detach routine: Haiku unloads the driver when its file is replaced.

## What the bus thread costs

`top -d -n 1 -i 10` on the board ("bwfm sdio poller"; top's %CPU is of all
four cores, these are of one), 2026-10-04, joined to the same access point:

| State | Every ms after a frame (before) | Interrupt + 20 per second (now) |
|---|---|---|
| Joined, idle (~100 broadcast frames/s on the network) | 2.6-2.8 % | 0.8-0.9 % |
| Interface up, nothing joined | 1.5 % | 0.2 % |
| Interface down | 0.04 % | 0.05 % |
| 32 MB received, Wi-Fi only, 8 runs | 3.83-4.11 s, median 3.88 | 3.82-4.13 s, median 3.90 |
| Ping over Wi-Fi, after 3 s of silence | 0.8-1.0 ms | 0.8-0.9 ms |

A build with counters showed every wake-up coming from the interrupt: of
about 3000 unasked serves, two found a frame, which is how often a frame
arrives during a serve by chance. Serving the card again straight away when
its line was already raised after a serve (instead of through the
interrupt) caught 6 % of the serves during a transfer and changed nothing
measurable, so it was left out. What is left when joined is serving the
network's broadcasts and multicasts: the chip takes all multicast frames
(`allmulti`, see `bwfm_iff`), and a filter list would save some of that.

## Joining at start

net_server joins a remembered network by itself only after a scan someone
asked for (the OpenBSD layer reports only those), and nothing asks for one
at start. The image's `UserBootscript` (`data/boot/rpi/UserBootscript`)
starts `wifiautojoin`, which scans, picks the strongest remembered network
and joins it through net_server; checked on two restarts.

On 2026-10-06, repeated performance-test boots exposed intermittent startup
join timeouts. Three of four consecutive cold browser tests joined; the
fourth associated but did not finish its handshake before the supplicant's
15-second timeout. This remains under investigation.

The final flashed image (`hrev60097+672`, source `63f5e1e916`) also reproduces
this failure. Joining through the custom preferences window succeeds and
remembers the network; the next cold boot joins automatically. The following
cold boot times out, as do a manual helper retry and an explicit leave/rejoin.
Ethernet and the desktop remain available. This uses the packaged autojoin
helper, not the earlier experimental scan listener. It is a release limitation,
not a passed reboot test. The helper currently exits after a join timeout;
its outer retry loop covers unavailable scans and networks, not failed joins.
Evidence: `evidence/performance-20261006/release/cold-boot-3.txt`,
`wifi-boot-3-retry.txt` and `wifi-boot-3-leave-rejoin.txt`.

The failed boot leaves the supplicant's "Failed to join network" password
dialog visible (`release/wifi-failure-during-soak.jpg`). In the pinned
supplicant source `bdf3144ad606b226aca6f8fae103e671e4655138`,
`WPASupplicantApp::MessageReceived()` synchronously calls
`wireless_config_dialog()`, whose `WaitForDialog()` waits on a semaphore.
That blocks the application's message loop until the dialog is dismissed,
so later join/leave requests can remain queued. This is a separate recovery
obstacle; it does not establish the cause of the original WPA handshake
timeout. Simply adding retries to the startup helper would not address it.

After the final endurance run, clicking Cancel on that dialog lets the queued
requests proceed. The second five-second status sample shows the saved WPA2
network associated with DHCP, and it stays connected for the rest of the
40-second observation. No password change or new join command was sent.
This confirms a recovery path on this boot; it does not fix startup or prove
that all association failures have the same cause. Evidence:
`release/post-soak-wifi-dialog.jpg` and `wifi-after-dialog-cancel.txt`.

The clean boot after removing the telemetry module also misses autojoin.
After dismissing its error dialog, a fresh invocation of the normal packaged
helper joins successfully with the saved credentials and obtains DHCP.
The final device is left connected over both Wi-Fi and Ethernet. In the
final image's three cold boots with saved credentials, one autojoins and
two need recovery; this small sample is not an estimate of the general
failure rate. See `release/cold-boot-4.txt`, `wifi-boot-4-recovery.txt` and
`final-wifi-connected.jpg`.

Traffic is checked again after that recovery with Ethernet administratively
down: five gateway pings have no loss (1.412 ms average), a 32 MiB file to SD
matches SHA-256, and three downloads to `/dev/null` take 3.66, 3.63 and
3.58 seconds (about 73–75 Mbit/s). The server sees the Wi-Fi address for
every transfer. Ethernet is then restored and both links remain up.
Evidence: `release/final-network-wifi.txt` and `final-idle-2.txt`.

The OpenBSD compatibility layer now exports Broadcom's absolute RSSI as
FreeBSD-format half-dB units over a -100 dBm reference floor. Previously a
negative dBm byte wrapped into an unsigned RSSI, so the custom Wi-Fi tool
and autojoin helper showed every network at 0 dBm. Relative-RSSI Intel
drivers keep their existing format. Native build +663 reports the strongest
saved network at -29 dBm and distinct weaker levels, joins WPA2, obtains its
address, and completes an associated scan. Evidence:
`evidence/performance-20261006/wifi-startup/signal.txt` and
`signal-boot.{log,jsonl}`;
the complete image also reaches the branded QEMU desktop. This reporting
fix alone does not remove the second association attempt.

### Startup performance, 2026-10-06

A diagnostic trace identified a firmware roam during the first WPA handshake:
net80211 selected a 5 GHz BSSID, but firmware switched to the same SSID's
2.4 GHz BSSID while the host still derived keys for the original peer. The
driver now sets `roam_off=1`, leaving BSS selection and roaming to net80211.
Subsequent traces keep the firmware and host on the same BSSID.

The Haiku join ioctl also now installs SSID and key in net80211 before
passing the final WPA parameters to the driver. This requests one radio
restart instead of three. Open-network joins keep their existing path.
The ioctl validates the request/header, SSID and key lengths before copying
into fixed-size structures. A native probe rejects six malformed requests
and confirms that the existing WPA2 association survives.

After host scanning selects an AP, `bwfm_connect` supplies its primary
channel to the firmware's join request. The previous empty channel list
caused another full scan on every association attempt. In repeated traces,
join-to-authentication falls from about 2.8 seconds to 35–43 milliseconds.
The all-channel fallback remains when no valid channel is known, and the
legacy SET_SSID fallback remains for firmware without the extended join.

The first handshake still receives a reason-6 deauthentication immediately
after the host sends its second message. A second association to the same
AP completes. Its cause remains unresolved; the channel change makes the
retry substantially cheaper. Two observed startup runs connect about
13.1 seconds after the autojoin helper begins, versus about 18.1 seconds
with unrestricted firmware scans. These are host-received serial timings,
not a guarantee for other APs.

With Ethernet administratively down, three 32 MiB downloads to `/dev/null`
take 3.70, 3.74 and 3.77 seconds (about 71–73 Mbit/s). A separate 32 MiB
download to SD has matching SHA-256; five gateway pings have no loss and
1.29–1.38 ms latency. Ethernet is restored afterward. This verifies that
the startup changes retain normal traffic, rather than establishing an
isolated throughput speedup.

Evidence is in `evidence/performance-20261006/wifi-startup/`: diagnostics
`handshake-trace` and `tx-trace`, `join-probe-native.txt`, `throughput.txt`,
`channel-trace-1.txt` and `listener-1.txt`. The full image reaches the QEMU
desktop. After removing the diagnostic override and restoring the original
package settings, the packaged driver passes the six rejection checks and
joins on both a warm restart and a cold power cycle (`production-driver-check.txt`,
`production-warm-1.txt`, `production-cold-1.txt`). The temporary scan-notification helper experiment still took its
six-second fallback: initial automatic scans do not send that notification.
It is not included in the release changes. Diagnostic packet metadata and
firmware readbacks are also excluded from the production driver.

## Traps in the lab

- devfs loads anything that appears in a drivers directory. A staged
  `name.new` next to a running driver becomes a second instance on the same
  chip and panics. `tools/rpi4/install-driver.sh` stages elsewhere, moves the
  file and restarts.
- devfs also reloads the driver when its file is replaced or removed. With
  the interface up the network stack still has the device open and later
  closes it in code that is gone (KDL in `ethernet_down`). Take the
  interface down first (`ifconfig /dev/net/broadcomfmac/0 down`): then the
  swap happens live and the new driver comes up.
- With Ethernet and Wi-Fi on one network the board answers from either
  address.

## Open

- Commands and data are still polled, and data goes word by word through the
  data register (no DMA): 65 Mbit/s received is what that gives (32 MB to
  /dev/null with `rpi4_fetch`, Ethernet down). Sending was not measured.
- The "Network:" line of `ifconfig` and the network list come from the scan
  results, which age out a while after joining; a scan brings them back.
- A packaged driver wins over a copy under non-packaged: to try a new build
  on an installed system, block the packaged one in
  `/boot/system/settings/packages` (`BlockedEntries`).
- The Wi-Fi preferences window lists the networks; joining by clicking in it
  was not tried because KVM mouse input is broken on this image (see
  STATUS.md).
- The transmit counter of the interface does not count data frames.
- Only station mode with WPA2-PSK was tried. No 5 GHz/regulatory checks.
- Bluetooth shares the chip and is not up yet (`disable-bt` in config.txt).
