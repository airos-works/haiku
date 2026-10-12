# Wi-Fi and Bluetooth on the Radxa Cubie A7S (AIC8800D80)

The board's wireless module is a Quectel FCU760K, an AICSemi AIC8800D80 on
USB (the A733's USB1 EHCI, behind the onboard CH334F hub). Once the
regulator driver powers it, it shows up as `a69c:8d80`, a ROM loader. The
`aic8800wifi` driver loads the firmware into it, after which it comes back
as `a69c:8d81`: Wi-Fi on a vendor interface, and a standard USB Bluetooth
(H2) controller that `h2generic` drives.

The protocol notes this was written from (the vendor's Linux driver is GPL;
none of its code is used) are in `cubie/evidence/wifi/` on the lab disk:
`DESIGN.md`, `fdrv-protocol.md`, `bt-notes.md`.

## State (2026-10-10)

Measured on the lab's network ("Gaspers", a WPA2-PSK/CCMP mesh; joined on
5 GHz):

- Firmware load 0.31-0.38 s (560 messages); the chip is back as 8d81 0.6-1.4
  s later. The driver attaches during boot.
- Scans of all 38 channels take 1.5 s; the Wi-Fi preferences and the
  Deskbar list the networks.
- Join through net_server and wpa_supplicant (`ifconfig ... join`, the
  Deskbar, or `wifiautojoin` at boot with a saved network): associated, both
  keys installed and the port open in under 2 s, then DHCP. Link VHT MCS 8-9
  at 40 MHz on the lab's AP (HT MCS 7 with `vht false`; see 802.11ac below).
- Ping to a LAN host over Wi-Fi: about 1.7 ms, no loss (power save is off).
- 1 GiB from the lab host in 121 s (about 71 Mbit/s, HT40) while a BLE scan
  ran; no USB errors.
- 12 minutes of `ping -i 0.2` over Wi-Fi: 3600 of 3600 answered, median
  1.7 ms (p90 10 ms, p99 31 ms).
- `ifconfig down`/`up` 20 times: rejoins by itself. Reloading the driver
  with the interface down: the chip restarts, the driver attaches again and
  `wifiautojoin` rejoins.
- A firmware restart (forced from the lab with `aicload --reboot`): the
  driver reloads the chip and rejoins 5 s after it left the bus; Bluetooth
  comes back with it.
- After a power cycle, five boots out of five: the driver attached during
  boot, `wifiautojoin` joined the saved network, and the Bluetooth
  controller was there.
- The desktop: the WiFiStatus Deskbar menu lists the networks and joins a
  known one with a click; the Wi-Fi preferences show the connection and its
  address; the "Connected" notification comes up.
- Bluetooth: `bt_dev_info` shows the controller (Bluetooth 5.4, HCI/LMP 13,
  manufacturer 0x0b3b, ACL 1021 x 9); `bt_le scan` lists advertisers; the
  BluetoothStatus menu and the Bluetooth preferences show the adapter
  ("AIC8820") and find nearby devices.

## The "hang" of 2026-10-11 06:38Z: the board overheated

With the antenna fitted, a 512 MiB download over Wi-Fi seemed to hang the
board: no answer on either interface, the serial log stopped. It was not the
driver. The board lost power: the lab plug read 8, 9, 10, then 12 W during
the download and 0 W from 06:38:16, 11 s after the last serial line, and the
board only came back after 60 s without power (two 5 s cycles were not
enough). The same boot had two Summit pages playing 1080p30 videos in a
loop on the video engine (H.264 and HEVC, GPU compositing), which the same
test session meant to close before the download but did not; the download
came on top.

air/OS has no thermal management on the Cubie: no CPU frequency scaling, no
throttling, no critical trip (no fan is fitted: in Linux its PWM changes
nothing; Linux holds both CPU clusters at 416 MHz once the SoC is over
60 C). Read from the A733's THS
(evidence/wifi-hang):

- idle: 69-71 C; the two looping videos alone (Wi-Fi not joined): 71 to
  100 C in 4.5 minutes, still rising, with 0.8 of 8 cores busy and the
  plug at 5 to 8 W;
- one video and the download: 77 to 95 C in under 3 minutes;
- the download alone: 70 to 74 C over 8 minutes, 5 W, the whole Wi-Fi path
  (EHCI finish thread, receive thread, the stack's reader) about 0.15 of a
  core, no transfer errors.

So sustained video work cooks the board to its cut-off in about 6 minutes,
Wi-Fi or not; the fix is thermal management for the A733 (THS, throttling).

## 802.11ac (2026-10-12)

With the antenna, VHT first cost three quarters of the download rate: on
the lab's AP (26:5a:4c:21:6c:a2, a Ubiquiti Wi-Fi 6 AP, 5805 MHz, 40 MHz,
-48 dBm) the lab's 1 GiB download ran at 17.2-17.8 Mbit/s with VHT and
75.7 Mbit/s with HT, so for a while the driver advertised HT only. There
were two causes (cubie/evidence/wifi-vht):

- The TCP stack: a connection that is opened and then only receives (the
  lab's `cubie_fetch` sends no request) never measured a round trip, and
  without one the receive window stays at 64 KiB. Each round trip on the
  AP took 25-33 ms with VHT, so 64 KiB per round trip was 17 Mbit/s. The
  SYN is timed now (TCPEndpoint::_SynchronizeSentReceive()); the window
  grows to 6-12 MB. Clients that send a request first (HTTP) had grown
  their window all along: 38.5 Mbit/s with VHT on the old stack.
- The AP and the firmware: the firmware's association request always
  carries an Operating Mode Notification of 80 MHz. In this 40 MHz
  network the AP then sent the board nothing for about 30 ms twice per
  beacon interval (at the beacon and about 50 ms later), after which it
  resent with CTS-to-self: over half the airtime gone, and many lost
  MPDUs at MCS 8-9. The vendor's Linux driver is hit the same way (HT
  100.4, VHT 48.1 Mbit/s). Once
  the port is open the driver now sends the AP an Operating Mode
  Notification action frame with the channel's real width (20 or 40
  MHz); the stalls stop. With Linux and that frame, a UDP flood got
  130.8 Mbit/s through with VHT against 105 with HT.

256 MiB downloads from the lab host: VHT 17.1 Mbit/s before, 38.3 with
the TCP change, 117.3 with both; HT 90.6 with the TCP change. VHT is the
default again; `vht false` in the driver settings turns it off.

The 802.11ax (HE) the AP and the chip both offer does not work here: with
the vendor's Linux driver the downlink got 8.6 Mbit/s of a UDP flood, 34.7
after the same notification. This driver does not advertise HE.

Measured on the branch cubie-wifi-vht (evidence/wifi-vht/haiku), the lab's
watch on, within the same two hours (2026-10-12 00:10-01:01Z):

| 1 GiB from the lab host       | download (Mbit/s)          | 512 MiB upload |
|-------------------------------|----------------------------|----------------|
| HT, the TCP stack before      | 70.6, 73.0                 | 55.3           |
| HT, SYN timed                 | 92.5, 93.1                 | 54.1           |
| VHT + notification, SYN timed | 121.5, 115.1, 116.2        | 55.1           |

- one more VHT download with a BLE scan running (it listed 10
  advertisers): 118.9 Mbit/s;
- ping from the lab host over Wi-Fi both ways: 180 of 180 in the first 3
  minutes after the route change, then 600 of 600 in 10 minutes, median
  1.4 ms, p99 9.6 ms (the loss after the route change that VHT showed
  before is gone); no driver or USB errors after the boot, Ethernet
  unaffected;
- at these rates the interface drops a few TCP acknowledgements (about
  4000 of 240000 per GiB) when the 64-frame USB transmit ring is full;
  TCP does not notice.

Measured earlier on the branch cubie-wifi-hang (HT, mostly MCS 7 at 40 MHz,
the TCP stack before), evidence/wifi-hang/run9-final:

- three 1 GiB downloads: 75.0, 75.5 and 75.7 Mbit/s (the plug at 5 W, the
  receive path about 0.8 of a core); one more with a BLE scan running (it
  listed 8 advertisers): 75.9 Mbit/s; a 512 MiB upload: 67.5 Mbit/s;
- ping from the lab host over Wi-Fi both ways: 180 of 180 in the first
  3 minutes after the route change, then 600 of 600 in 10 minutes, median
  1.0 ms, p99 7.7 ms;
- no driver or USB errors after the boot, Ethernet unaffected.

`watch true` in the driver settings puts the processors' load, the busiest
threads, free memory, the USB transport's counters and what the driver's
threads wait for on the serial port every 5 s; a packet that runs past the
end of its USB transfer is logged with its context.

## Pieces

`src/add-ons/kernel/drivers/network/wlan/aic8800wifi`:

- `host/aic_loader.c`: the ROM loader. It writes the Bluetooth ROM patch
  (with the host's Bluetooth configuration: HCI over USB, not the UART the
  patch table names) and the Wi-Fi firmware, patches the firmware's
  configuration block and starts it: 560 messages, about 0.35 s.
- `host/aic_usb.cpp`: the USB side, plain Haiku code. The driver registers
  for both identities with the USB stack under its own name, so the stack
  loads it again when the chip comes back as the other one. At driver load
  a ROM-mode chip is loaded and the load fails; the next load finds the
  firmware running and attaches. A named kernel area marks a firmware this
  boot loaded: firmware found running without it (a warm restart from
  another system) is sent back to the ROM first. The firmware's message and
  data pipes have one transfer queued each; confirmations are matched on the
  transport's own threads.
- `dev/aic/if_aic.c`: the driver on OpenBSD's net80211, the way
  `broadcomfmac` runs `bwfm`. The firmware scans, associates, encrypts and
  aggregates; net80211 picks the network and runs the WPA handshake;
  wpa_supplicant's `HAIKU_JOIN`, net_server, the Wi-Fi preferences and the
  Deskbar applet work as for any OpenBSD-layer driver. Scan results are the
  beacons the firmware heard, fed to net80211 as received frames. Received
  data arrives as decrypted 802.11 frames that the driver turns into
  Ethernet frames (A-MSDU, and an A-MPDU reorder window of 64 per TID).
- `glue.c`: reports one device when the firmware runs.

Firmware: the files the loader needs, unchanged, with AICSemi's licence
(`LICENSE.aic`) and a provenance note, in
`/boot/system/non-packaged/data/firmware/aic8800wifi/`.
`tools/cubie-a7s/stage-aic8800-firmware.sh` stages them from Radxa's
`aic8800-firmware` package; `tools/cubie-a7s/UserBuildConfig` puts them in
both Cubie images. Before a public release, AICSemi should confirm that
`LICENSE.aic` covers the Wi-Fi images too (it names the Bluetooth files).

Driver settings (`~/config/settings/kernel/drivers/aic8800wifi`):
`debug true` logs what the driver does; `watch true` adds the 5 s watch
(above); `vht false` advertises HT only (802.11ac above);
`attach_at_boot false` keeps it out
of the first minute after boot (touch its file to bring it in later).

## Bluetooth

Nothing in `h2generic` had to change: it matches the controller by its
interface class and takes its endpoints from interface 0 only. The
Bluetooth ROM patch is part of the Wi-Fi driver's firmware load, so
Bluetooth exists only once `aic8800wifi` has run.

`bluetooth_server` never ran its device manager's looper, so a controller
that appeared after the server had started was never seen; this one
appears during the server's start, and was found on some boots and not on
others. That is fixed in the server.

## Lab tools

- `aicload` (lab images): the loader from userland over usb_raw, and
  `--probe` / `--reboot` for a running firmware.
- `bt_dev_info` (lab images): the controller, its address and versions.
