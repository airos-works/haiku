# USB CDC NCM (usb_ncm)

`usb_ncm` drives USB Ethernet functions of the CDC Network Control Model
(NCM 1.0): 2.5 Gbit/s Realtek RTL8156 adapters, many docks, phones and Linux
USB gadgets (`f_ncm`). It replaces `usb_ecm` for every device that offers NCM
(airos-works/haiku#123).

## Why

ECM carries one Ethernet frame per USB transfer. With `usb_ecm`, the Framework
13's RTL8156 ran 651-703 Mbit/s up and 522-524 down on a 2.5 Gbit/s link, the
same as a 1 Gbit/s RTL8153: every frame costs a transfer, a doorbell write, a
completion and a thread wake-up. NCM packs many frames into one Network
Transfer Block (NTB), so those costs are paid per block.

## Which driver takes a device

Every driver that says it supports a USB device gets it (the USB bus node has
`B_FIND_MULTIPLE_CHILDREN`), so two drivers must not both say yes: they would
set different configurations. A device like the RTL8156 offers three:

| index | configuration |
|---|---|
| 0 | vendor specific (Linux' `r8152`) |
| 1 | CDC NCM (communication interface 2/0x0d, data interface protocol 1) |
| 2 | CDC ECM |

- `usb_ncm` supports any device with a CDC NCM interface in any configuration
  and sets that configuration (unless it is the current one, so that the other
  functions of a composite device keep their endpoints).
- `usb_ecm` steps aside for a device that also offers NCM, as long as the
  `usb_ncm` module is installed. Without it, `usb_ecm` takes such a device as
  before.

## How it works

- **Setup.** With the data interface in alternate 0: `GET_NTB_PARAMETERS`,
  `SET_NTB_FORMAT` (16 bit, or 32 bit for a function that has nothing else),
  `SET_CRC_MODE` off, `SET_NTB_INPUT_SIZE`; then the NTB alternate (protocol 1)
  and `SET_ETHERNET_PACKET_FILTER` (directed, broadcast, all multicast;
  promiscuous on request). Link and speed come from the `NetworkConnection` and
  `ConnectionSpeedChange` notifications.
- **Receiving.** Several bulk IN transfers of the NTB input size stay queued.
  The stack's reader takes the datagrams of a completed NTB, up to 16 per call
  (`ETHER_RECEIVE_NET_BUFFERS`); the transfer goes back to the device as soon
  as its last datagram is out. Every NDP in the chain is read; entries that
  point outside the block, or are shorter than an Ethernet header or longer
  than a frame, are skipped and counted.
- **Sending.** Frames (`ETHER_SEND_NET_BUFFER`, no copy before the one into
  the NTB) are packed into the NTB being filled: NTH, the NDP with room for
  the most datagrams, then the datagrams at the function's divisor and
  remainder. The NTB goes out at once while fewer than `tx_eager` transfers
  are in flight, when it is full (size or datagram count), or else when a
  transfer completes. A frame therefore waits only while transfers are in
  flight, and the next completion sends it, so there is no flush timer; under
  load the NTBs grow to what the bus takes. A block that would end on a packet
  boundary gets one byte of padding instead of a zero length packet.
- **Realtek PHY.** As in `usb_ecm`, the RTL815x PHY is powered up at open
  (`usb_ecm` powers it down while its interface is down, and a later boot can
  find it so) and down at close.

## Tunables

`/boot/home/config/settings/kernel/drivers/usb_ncm`, read at each open (take
the interface down and up to apply):

| key | default | range | meaning |
|---|---|---|---|
| `rx_ntb_size` | 32768 | 2048-65536 | NTB input size asked for (capped by the function's `dwNtbInMaxSize`) |
| `tx_ntb_size` | 32768 | 2048-65536 | largest NTB sent (capped by `dwNtbOutMaxSize`) |
| `rx_transfers` | 8 | 1-12 | bulk IN transfers kept queued |
| `tx_transfers` | 8 | 1-12 | most bulk OUT transfers in flight |
| `tx_eager` | 2 | 1-12 | below this many in flight an NTB goes out at once |
| `tx_max_datagrams` | 32 | 1-256 | datagrams per NTB sent (capped by `wNtbOutMaxDatagrams`) |
| `disable` | false | | `disable true` keeps the module from loading (read when it loads, so at the latest at the next boot): `usb_ecm` then takes devices that also offer ECM, as before |

`ncmstat [/dev/net/usb_ncm/N]` (built from
`src/tests/add-ons/kernel/drivers/usb_ncm`) prints what was negotiated and the
counters: NTBs and datagrams each way, datagrams per NTB, why NTBs were sent
(idle, full, on completion), waits for a free buffer, bad NTBs and datagrams,
errors, and right now: reads queued with the device, completed reads not yet
taken, writes in flight. It opens the device read only, which leaves it to the
network stack.
The syslog gets the negotiated parameters at each open and a summary at each
close.

## Tests

- `ntb_test` (same directory, also builds on any host with
  `-fsanitize=address,undefined`): NTBs built by the writer parse back to the
  same datagrams in both formats and with odd alignments; broken headers,
  NDPs outside the block, NDP loops, chains, bad datagram entries; 200,000
  randomly damaged blocks never yield a datagram outside the transfer.
- **QEMU with Linux' NCM gadget as the function** (November 2026): a Linux
  guest runs `f_ncm` (configfs) on the `usbip-vudc` virtual UDC, attaches it
  to itself with `vhci-hcd` and exports it with `usbredirect`; the air/OS
  guest gets it through `qemu-xhci` + `usb-redir`. High speed, so this checks
  correctness, not 2.5 Gbit/s. Results with the defaults (16 KiB NTBs, which
  is what `f_ncm` offers):
  - TCP 228-231 Mbit/s up and 217-230 down; Linux' `cdc_ncm` on the same rig
    managed 137-139 up and 155-203 down.
  - 64-byte UDP datagrams to air/OS: 80,000 per second, none lost, up to 31 in
    one NTB; sent: up to 32 per NTB.
  - DHCP, ping, 200 pings of 1472 bytes: all answered; no bad NTB.
  - Gadget unbind and bind (a replug): the device goes and comes back as
    `/dev/net/usb_ncm/1`, DHCP and traffic resume.
  - A gadget with ECM in its first configuration and NCM in its second (like
    the RTL8156): `usb_ecm` leaves it to `usb_ncm`, which selects the NCM
    configuration; with `disable true`, `usb_ecm` takes it after a replug.
  - `ifconfig down` and `up` 15 times: all work. Known rig problem: the first
    down and up after a replug, or one done while traffic flows, sometimes
    leaves the bulk IN transfers queued with the controller without ever
    completing (the gadget sends; nothing arrives) until the next down and up.
    Linux' `cdc_ncm` loses its receive path on the same rig in 8 of 15 down
    and up cycles under traffic, so this is taken to be the rig (usbip-vudc
    and usbredir); it is a thing to watch for on real hardware.

## Test plan: RTL8156 on the Framework 13

Not run yet: the laptop's reboots can hang in shutdown (#136), so this runs
with the owner around. Adapter: 0bda:8156, configurations vendor (index 0),
NCM (1), ECM (2); laptop on the 2.5 Gbit/s port, RTL8156 at 192.168.1.41
(Wi-Fi .31 and the dock's RTL8153 .195 share the subnet, so every iperf3
binds to .41). Peer: the Linux build server, 192.168.1.200.

1. **Baseline** (usb_ecm in ECM mode, laptop build 2561, 17 Nov), with
   `iperf3 -c 192.168.1.200 -B 192.168.1.41`: up 664 Mbit/s (1 MB buffer
   703, 4 streams 711), TCP down 522-524, UDP offered at 2 Gbit/s down:
   1.0 Gbit/s delivered (about 87,000 frames/s, 50 % lost).
2. **Install.** Both add-ons are kernel modules in
   `add-ons/kernel/drivers/network/`. The kernel searches the packaged
   `/boot/system/add-ons` before `/boot/system/non-packaged/add-ons`, so a
   non-packaged `usb_ecm` never replaces the packaged one, and a
   non-packaged `usb_ncm` next to the old packaged `usb_ecm` would leave both
   drivers attached to the RTL8156, setting different configurations. Two
   ways that work:
   - **A `haiku.hpkg` built from this branch** on the laptop's base revision
     (`jam -q haiku.hpkg`), installed like the other test builds and
     activated by a reboot. Both modules come from it.
   - **Without a package rebuild:** hide the packaged `usb_ecm` and put both
     new modules in non-packaged. `/boot/system/settings/packages`:
     ```
     Package haiku {
     	BlockedEntries {
     		add-ons/kernel/drivers/network/usb_ecm
     	}
     }
     ```
     (merged into the file if it exists), copy `usb_ecm` and `usb_ncm` from
     `objects/haiku/x86_64/release/add-ons/kernel/drivers/network/ether/`
     to `/boot/system/non-packaged/add-ons/kernel/drivers/network/`, reboot.
     These are device-manager modules: nothing watches that directory, so the
     copies are only loaded at the next probe (no live-load trap). To undo:
     delete both copies and the `BlockedEntries` lines, reboot.
   Also copy `ncmstat` (`objects/.../tests/add-ons/kernel/drivers/usb_ncm/`).
3. **Driver selection:** the syslog has `usb_ecm: leaving a device that
   offers NCM to usb_ncm`, `usb_ncm: NCM in configuration 1, interfaces ...`
   and `usb_ncm: NTB16: in ... out ...`; `ifconfig` shows
   `/dev/net/usb_ncm/0` with .41 from DHCP; `ncmstat` shows what the RTL8156
   asked for (record it). The dock's RTL8153 (no NCM) still comes up as
   `/dev/net/usb_ecm/0` with .195.
4. **Throughput,** 20 s each, `ncmstat` before and after each:
   - up: `iperf3 -c 192.168.1.200 -B 192.168.1.41 -t 20`, also with `-P 4`
     and `-w 1M`, and against a receiver on .200 that reads 1 MB per
     `recv()` (iperf3's server is a slow reader that holds back Linux'
     receive autotuning);
   - down: the same with `-R`;
   - UDP: `-u -b 2G -R` (down) and `-u -b 2G` (up).
   Target: above 2 Gbit/s both ways. Look at datagrams per NTB, the flush
   reasons, `waits` (all OUT buffers busy), bad NTBs and the errors.
5. **If short of it,** the settings file
   `/boot/home/config/settings/kernel/drivers/usb_ncm` (read at each open:
   `ifconfig /dev/net/usb_ncm/0 down`, then `up`, after each change):
   `rx_ntb_size 65536`, `rx_transfers 12`, `tx_ntb_size 65536`,
   `tx_transfers 12`, `tx_eager 1` or `4`. `profile -a -k` during a download
   shows whether the xHCI driver's copy of each NTB through its bounce
   buffers (`ReadDescriptor`, in the controller's finisher thread) is the
   next limit.
6. **Robustness:** cable out and in (link down and up from the
   notifications, DHCP again); the adapter unplugged during a transfer and
   plugged back; link lights off while the interface is down (PHY power); a
   reboot with the interface left down; `ifconfig down`/`up` 10 times idle,
   10 times while iperf3 runs, and once right after a replug.
   **The receive stall seen on the QEMU rig** would look like this: after the
   `up`, DHCP and ping get no answer although the link is up and frames go
   out; `ncmstat` shows `rx: 0 ntbs` (or a count that stops growing),
   "8 reads queued, 0 completed and not taken", no receive errors, and `tx`
   NTBs going out. It ended with the next down and up. If it happens on the
   RTL8156, it is real: record `ncmstat`, the syslog and `listusb -v`, and try
   whether a down and up ends it.
7. **Way back:** `disable true` in
   `/boot/home/config/settings/kernel/drivers/usb_ncm`, then a reboot (or
   unplugging and replugging the adapter) gives the RTL8156 back to `usb_ecm`
   in its ECM configuration. The module refuses to load while that line is
   there, and `usb_ecm` only steps aside for a loaded `usb_ncm`. With the
   non-packaged install, deleting the copies and the `BlockedEntries` lines
   and rebooting restores the old drivers.
