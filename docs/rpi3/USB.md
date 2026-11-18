# USB on the Raspberry Pi 3 (DWC2 host controller)

Everything USB on the Pi 3 B/B+ hangs off one controller: the Synopsys
DesignWare USB 2.0 OTG core ("DWC2") of the BCM2837. Its single root port
carries the board's hub (the B+'s Microchip LAN7515: a USB 2.0 hub and gigabit
Ethernet; the B's LAN9514). Keyboards, mice, flash drives and the NanoKVM's
gadget sit behind that hub.

`src/add-ons/kernel/busses/usb/dwc2*.{h,cpp}` is a Haiku USB bus driver for
the core in host mode, like `ehci` and `xhci` next to it.

| Item | Value |
| --- | --- |
| Device tree node | `/soc/usb@7e980000`, `brcm,bcm2708-usb` (`brcm,bcm2835-usb` upstream) |
| Registers | CPU 0x3f980000 (bus 0x7e980000), first 4 KiB used |
| Interrupt | VideoCore IRQ 9, kernel IRQ 41 (the loader's GIC-style numbering) |
| DMA | Bus address = physical + 0xc0000000, below 0x3f000000 (`/soc` `dma-ranges`); not cache-coherent |
| Core | 2.80a on the BCM283x, 8 host channels, internal (buffer) DMA, 4080-word FIFO RAM |
| Power | VideoCore power domain 6 ("USB HCD", firmware device 3) |

## Bring-up sequence

1. Ask the firmware to power the USB domain: property tag `0x00028001` (set
   power state), device 3, state "on, wait", through the mailbox at peripheral
   base + 0xb880, with the message in uncached memory at its bus address.
   Linux' raspberrypi-power, U-Boot and FreeBSD do the same.
2. Check `GSNPSID` (0x4f54xxxx) and `GHWCFG2` (internal DMA; channel count).
   Run lengths come from `GHWCFG3`, capped at 65535 bytes/511 packets as Linux
   does for the BCM2835.
3. Core soft reset; UTMI+ PHY of the core's width (8 bit here), another reset if
   that changed; force host mode and wait up to 110 ms for it.
4. `GAHBCFG`: buffer DMA, Linux' BCM2835 burst setting (0x10). PHY clock 30/60
   MHz. FIFOs as Linux on the BCM2835: 774 words Rx, 256 non-periodic Tx, 512
   periodic Tx. Flush the FIFOs, halt all channels, port power on.
5. Interrupts: port, channel and disconnect; start-of-frame only on demand (see
   below).
6. Root port reset: `PrtRst` for 60 ms, wait for `PrtEna`, read the speed, set
   `HFIR` for it (Linux' `dwc2_calc_frame_interval()`).

## Design

The core has no schedule in memory. Software programs a host channel with one
run (a transfer chunk, a control stage or a single split transaction) and the
channel halts with an interrupt when it is done or needs attention. The driver
uses buffer DMA mode, like Linux' dwc2, with its own scheduler built to keep
interrupt load low:

- **Interrupt handler**: acknowledges and records channel halts (`HCINT` of each
  channel in `HAINT`), port changes (`HPRT0`'s write-one-to-clear bits, keeping
  `PrtEna` zero) and start-of-frame, and wakes the worker. Nothing else runs in
  interrupt context. Only `ChHltd` and `AHBErr` are unmasked per channel.
- **Worker thread** (real-time display priority): one pass under the schedule
  mutex reads what each halted channel did, starts the next run, assigns free
  channels to waiting endpoints and collects finished transfers. Callbacks run
  after the mutex is dropped, so they may submit and cancel.
- **Bulk and control** endpoints take a channel while they have transfers. In
  buffer DMA mode the core retries their NAKs itself, without interrupts. Two
  channels stay reserved for interrupt endpoints. If an endpoint waits for a
  channel, an IN that has held one for over 1 ms with a NAK latched in `HCINT`
  is halted, keeps what it received and goes to the back of the queue: a
  NAKing Ethernet receive cannot lock out mass storage.
- **Interrupt** endpoints take a channel for each poll. The worker sleeps until
  the next poll is due (a thread timeout, not start-of-frame interrupts) and
  enables the channel with `OddFrm` set for the next (micro)frame. A NAK halts
  the channel at the end of that frame; the endpoint waits for its next poll.
  High-speed endpoints asking for less than 1 ms are polled every millisecond.
- **Start-of-frame interrupt**: enabled only while a periodic split
  transaction waits for a particular microframe (a few microframes per poll of
  a full/low-speed interrupt endpoint behind the hub), never otherwise. The
  BCM283x's 8000 SOF interrupts a second (the reason for Raspberry Pi Linux'
  FIQ) do not happen.
- **Split transactions** (full/low-speed devices behind the high-speed hub,
  decided by the root port's speed; `HCSPLT` with the hub address and port from
  the pipe, the transaction translator's):
  - one packet per split, `XactPos` "all"; `LSpdDev` for low speed;
  - start split; on ACK the complete split; NYET repeats the complete split;
    NAK restarts with a start split; data, STALL or errors finish it;
  - bulk/control: complete splits right away; after three NAKs in a row the
    endpoint gives up its channel and retries a millisecond later (Linux'
    `DWC2_NAKS_BEFORE_DELAY`), except control IN stages;
  - interrupt: start splits only in microframes 0-3, the first complete split
    two microframes after the start split, NYETs retried in the following
    microframes up to six after the start split (EHCI's usual S-mask 0x01,
    C-mask 0x1c); a missed window means the poll is repeated. Error count 3
    (`MC`) as Linux does for periodic splits.
- **Data** goes through a 64 KiB bounce buffer per channel, below the DMA limit,
  at bus address physical + `dma-ranges` offset. The buffers are cached: an OUT
  run cleans its data to the point of coherency (`dc cvac`), an IN run
  invalidates the buffer before (`dc civac`) and after (`dc ivac`) the DMA.
  `DWC2_UNCACHED_BUFFERS` in `dwc2.cpp` maps them Normal Non-cacheable
  instead, as a fallback for debugging.
- **Runs**: bulk and control data up to 64 KiB less a partial packet, IN runs
  whole packets; interrupt runs one (micro)frame's worth (`MC` packets for
  high-bandwidth endpoints); splits one packet. Data toggles live in the pipe:
  a run starts with it and the next PID is read back from `HCTSIZ`.
- **Errors**: STALL, babble and AHB errors finish the transfer
  (`B_DEV_STALLED`, `B_DEV_DATA_OVERRUN`, `B_DEV_FIFO_*`); transaction and
  toggle errors are retried, three in a row finish with `B_DEV_CRC_ERROR`.
  Received whole packets of an interrupted run are kept. A device sending more
  than asked finishes with `B_DEV_DATA_OVERRUN`.
- **Cancel**: queued transfers are removed at once; a running non-periodic
  channel is halted (`ChDis`), a periodic one halts at the end of its frame by
  itself (it cannot be halted in buffer DMA mode). The channel is reused only
  after its halt; a halt that never comes is taken after 100 ms.
- **Root hub**: emulated (one port), like EHCI's.

## Files

| File | Contents |
| --- | --- |
| `dwc2_hardware.h` | Registers |
| `dwc2.h`, `dwc2.cpp` | Controller, scheduler, device tree attachment (`busses/usb/dwc2/fdt/...`) |
| `dwc2_rh.cpp` | Root hub emulation |
| `build/jam/images/RpiBootImage` | The driver is a boot module of the Pi boot archive (USB boot) |
| `build/jam/packages/Haiku` | In the arm64 system package and its boot module links |

The driver attaches to enabled `brcm,bcm2835-usb`/`brcm,bcm2708-usb` nodes
whose `dr_mode` is not `peripheral`. The Pi 4's node of the same core is
disabled in the firmware's tree (its USB-C port), so the Pi 4 and 5 images are
unaffected.

## Testing

QEMU 10.2 `raspi3b` (`hw/usb/hcd-dwc2.c`) with the loader and boot archive
(`tools/rpi3/qemu-run.sh`, `RPI3_QEMU_ARGS` for the USB devices). The model's
limits: no FIFOs (DMA mode only, which is what the driver uses), no split
transactions (`HCSPLT` is stored and ignored), `OddFrm` ignored (periodic
channels run at once), interrupt NAKs halt the channel, bulk/control NAKs are
retried without halting. Its single port gets QEMU's full-speed `usb-hub`
inserted automatically unless a device is given `port=1`.

The full system image (`jam -q @minimum-rpi4 build airos-rpi-image`, its
`airos-rpi4-minimum.img` as a `usb-storage` drive with `snapshot=on`) boots from
the USB stick, as there is no SD card driver for the Pi 3 yet: the loader
registers the boot "from an image" and the kernel takes the first BFS partition
with a system on it.

| Setup | Result |
| --- | --- |
| `-device usb-kbd` (QEMU adds the full-speed hub) | Port reset, full speed; hub enumerated, its status endpoint polled (NAK halts); keyboard behind it enumerated: device and 34-byte configuration descriptors in 8-byte packets, `SET_CONFIGURATION` |
| `-device usb-storage,drive=d0` (behind the hub, full speed), a 64 MB disk with an MBR | `usb_disk` gets INQUIRY ("QEMU QEMU HARDDISK 2.5+"), publishes `disk/usb/0/0/raw`; 31-byte CBWs, 512/1024-byte reads |
| `-device usb-storage,drive=d0,port=1` (root port, high speed), the system image | `dwc2: port enabled, high speed`; the kernel boots from the stick: `bfs: mounted "airOS" (... device = /dev/disk/usb/0/0/1)`, packagefs loads the packages, drivers come from disk, app_server and input_server start, Deskbar's first-login script runs; 10114 channel runs, no USB errors |
| The system image, `usb-kbd` and `usb-mouse`, all behind the hub; `sendkey h`, `sendkey i`, `sendkey shift-a`, `mouse_move 20 10`, `mouse_move -5 3`, `mouse_button 1`/`0` from the QEMU monitor | Boots from the stick at full speed; with `TRACE_DWC2_DATA`: keyboard boot reports `00 00 0b ..` (h), `00 00 0c ..` (i), `02 00 00 ..`, `02 00 04 ..` (shift, a) and the releases; mouse reports `00 14 0a 00` (20, 10), `00 fb 03 00` (-5, 3), `01 00 00 00` (button); hub status changes `0e 00`, `0c 00`, `08 00` |

Tracing: `TRACE_DWC2` in `dwc2.cpp` logs every run, halt and completion;
`TRACE_DWC2_DATA` logs what interrupt endpoints deliver (first 8 bytes).

In 2 of about 20 boots with QEMU's multi-instruction translation blocks
(`RPI3_QEMU_TB=multi`), both with `TRACE_DWC2`, the kernel panicked while the
device manager reprobed all nodes after mounting the boot volume
(`device_manager_init_post_modules()`): a NULL page fault in `strcmp()` called
by `ohci`'s `supports_device()`, which had just been loaded from the stick, with
a NULL `B_DEVICE_BUS` string. No node registers such an attribute, and the
driver's device tree walks keep their node references balanced. The reprobe
loads and unloads modules at reused addresses, which multi-instruction TCG
is known to run stale code for here. With one instruction per block
(`qemu-run.sh`'s default) both setups above booted to the desktop without it:
the high-speed stick with the production build, and the hub with stick,
keyboard and mouse with `TRACE_DWC2_DATA`, where 13 bursts of keys and mouse
moves over 30 minutes all arrived.

Not testable in QEMU: split transactions, start-of-frame scheduling, `OddFrm`
timing, high-bandwidth interrupt endpoints, OUT NAK/NYET/PING handling, cache
maintenance (QEMU has no cache model), the firmware power request's effect.

## Risks on the hardware

- **Split transactions** are written from Linux' `hcd_intr.c`/`hcd_queue.c`,
  U-Boot and the USB 2.0 rules (11.18) without a test. Keyboard and mouse
  dongles behind the LAN7515 depend on them. A complete split needs the worker
  thread to run within about a microframe of the start split's halt; a busy
  system missing that window repeats the poll (an interrupt IN's data is lost
  if the TT already took it from the device).
- **Cache maintenance** of the bounce buffers is invisible in QEMU. If data
  looks stale or corrupted, build with `DWC2_UNCACHED_BUFFERS` 1.
- **Firmware power request**: polls the mailbox directly. Should the USB domain
  be off and the request fail, the core reads no `GSNPSID` and the driver says
  so (`no DWC2 core at ...`).
- **PHY and AHB settings** follow Linux on the BCM2835 (UTMI+ 8 bit, burst
  setting 0x10, FIFO split 774/256/512). `GUSBCFG` and `HPRT0` are logged at
  start.
- **Interrupt latency** under Ethernet load: every bulk run's halt goes through
  the worker thread (real-time display priority). Linux needed the FIQ for the
  SOF load, which this design avoids, but halts still cost a thread wake-up.
  Runs are up to 64 KiB; at 300 Mbit/s that is several hundred halts a second.
- **Preemption** of NAKing INs (halting a bulk IN channel to give it to
  another endpoint) is routine here but only an error path in Linux; a core
  quirk there would show as lost or duplicated data on a busy bus.
- **Ethernet**: there is no driver for the LAN7515's (`lan78xx`) or the Pi 3 B's
  LAN9514's (`smsc95xx`) Ethernet function yet.

## Known gaps

- Isochronous transfers (USB audio, webcams): `SubmitTransfer()` returns
  `B_NOT_SUPPORTED`. Needs per-(micro)frame scheduling of packets (and
  `XactPos` begin/mid/end for full-speed OUT splits).
- No kernel debugger keyboard (`StartDebugTransfer()`).
- No suspend/resume or OTG/device mode.
- Interrupt polling is timed by the worker's sleep (thread timeouts): polls can
  come later than their interval under load. No bandwidth accounting: periodic
  polls are not budgeted per microframe or per transaction translator.
- The firmware power request polls the mailbox directly; another kernel user
  of the mailbox at the same moment would get confused.
