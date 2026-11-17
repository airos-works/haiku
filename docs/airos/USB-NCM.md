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
(Wi-Fi .31 and the dock's RTL8153 .195 share the subnet: bind to .41).

1. **Baseline (usb_ecm, laptop build 2561, 17 Nov):** `iperf3 -c
   192.168.1.200 -B 192.168.1.41`: up 664 Mbit/s (1 MB buffer 703, 4 streams
   711), TCP down 522-524, UDP down offered at 2 Gbit/s: 1.0 Gbit/s delivered
   (about 87,000 frames/s, 50 % lost).
2. **Install** an image or `haiku.hpkg` with this driver: `usb_ecm` changed
   too, and both are packaged, so a non-packaged copy does not do (the
   packaged module wins). Reboot.
3. **Driver selection:** syslog has `usb_ecm: leaving a device that offers NCM
   to usb_ncm`, `usb_ncm: NCM in configuration 1, interfaces ...` and the
   `NTB16: in ... out ...` line; `ifconfig` shows `/dev/net/usb_ncm/0` with
   .41 from DHCP; `ncmstat` shows what the RTL8156 asked for (record it). The
   dock's RTL8153 (no NCM) still comes up as `/dev/net/usb_ecm/0` (.195).
4. **Throughput** against the build server (.200), 20 s each, `ncmstat`
   before and after each: up `iperf3 -c 192.168.1.200 -B 192.168.1.41 -t 20`,
   also `-P 4` and `-w 1M`; for uploads also a receiver that reads 1 MB at a
   time on .200 (iperf3's server is a slow reader that holds back receive
   autotuning); down with `-R`; UDP down `-u -b 2G -R` and up `-u -b 2G`.
   Target: above 2 Gbit/s both ways. Look at datagrams per NTB, the flush
   reasons, `waits` (all 8 OUT buffers busy) and the receive errors.
5. **If short of it,** the settings file (take the interface down and up after
   each change): `rx_ntb_size 65536`, `rx_transfers 12`, `tx_ntb_size 65536`,
   `tx_transfers 12`, `tx_eager 1` / `4`. `profile -a -k` during a download
   shows whether the xHCI driver's copy of each NTB through its bounce buffers
   (`ReadDescriptor`, in the controller's finisher thread) is the next limit.
6. **Robustness:** cable out and in (link down and up from the
   notifications, DHCP again); adapter unplugged during a transfer and plugged
   back; `ifconfig down`/`up` 10 times, idle and under traffic (watch for the
   receive stall seen on the QEMU rig: `ncmstat` "reads queued" with nothing
   received); link lights off while the interface is down (PHY power); a
   reboot with the interface left down.
7. **Way back:** `disable true` in
   `/boot/home/config/settings/kernel/drivers/usb_ncm` and a reboot (or a
   replug) gives the adapter back to `usb_ecm` in its ECM configuration.
