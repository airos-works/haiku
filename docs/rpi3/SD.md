# The Raspberry Pi 3's SD card slot (BCM2837 "sdhost")

State 2026-11-18: the kernel finds the SD card through
`src/add-ons/kernel/busses/mmc/bcm2835_sdhost`, mounts the BFS boot volume
from it and starts the user land, in QEMU's `raspi3b`. Reads and writes are
checked end to end (below). **Not run on a real Pi 3 yet.**

## What the hardware needs

- The Pi 3 has two SD controllers: Broadcom's own "sdhost"
  (`brcm,bcm2835-sdhost`, `/soc/mmc@7e202000`, CPU address 0x3f202000) for
  the card slot, and an Arasan SDHCI (`/soc/mmcnr@7e300000`,
  `brcm,bcm2835-mmc`) for the Wi-Fi chip on GPIO 34-39. The driver binds
  only the first, and only when its node is enabled (the Pi 4's tree has the
  block too, switched off).
- Pins: GPIO 48-53 in ALT0. The firmware does that on the boards. QEMU
  connects the card to whichever controller the pins select and starts with
  the SDHCI, so the loader (`mux_bcm2837_sd_pins()` in
  `src/system/boot/platform/rpi/dtb.cpp`) sets the pins of the node's
  `pinctrl-0` group on a BCM2837: harmless where the firmware did it already.
- Clock: the sdhost runs from the VideoCore's core clock. The loader asks
  the firmware for it (mailbox clock 4) and puts it into the node's
  `clock-frequency` ("SD host clock: ... Hz" in the loader's log). 250 MHz on
  a Pi 3 with `enable_uart=1`, 350 MHz in QEMU (which does not care). The
  card clock is core / (SDCDIV + 2): 400 kHz for identification, 25 MHz,
  then 50 MHz in SD high speed mode after CMD6 (the bus manager's
  `configure_sd_high_speed()`, through the driver's `switch_sd_function`).
- No card detection on the Pi 3: the slot is probed once when the bus comes
  up.

## How the driver works

Linux' `drivers/mmc/host/bcm2835.c` is the reference. The block is no
standard SD host controller: a command register whose "new" flag clears when
the answer is in, a status register (write 1s to clear), a 16-word FIFO
behind SDDATA, and a debug register (SDEDM) with the data state machine and
the FIFO's fill level.

- Commands: wait for the previous one and for the state machine to be idle,
  clear the status, block size and count (SDHBCT, SDHBLC), argument, command
  with the new flag, then poll until the flag clears. The fail flag with
  CMD_TIME_OUT in SDHSTS is a timeout (how the bus manager probes CMD8,
  CMD1, ...). R1b commands (`SDCMD_BUSYWAIT`) wait for the busy status.
- Long answers: the block has all 128 bits, CRC included; they are shifted
  into the SDHCI layout the bus manager expects (bits 127:8).
- Data: PIO, polled, through a 128 KiB buffer (256 blocks, the block count
  register has nine bits). Reads take what the FIFO holds in bursts of up to
  eight words; writes fill it the same way and wait for the block status
  after each block, as Linux does. Open-ended multi-block transfers (the
  disk driver only uses CMD18/CMD25) end with a CMD12 of the driver's own:
  this block has no automatic CMD12. After a read, what is left in the FIFO
  is drained.
- No interrupt handler: the status bits that the interrupt line follows are
  the ones the polling code clears. Long waits (first data of a read, card
  busy after a write) poll with 10-100 µs sleeps.

### Hardware quirks from Linux

Implemented:

- FIFO read and write thresholds of 4 words in SDEDM ("silicon bug"); the
  write back of SDEDM also takes the state machine out of its power-down
  reset state.
- SDHCFG `SLOW_CARD` always set, so the 11-bit identification divider is used
  in the data mode too (the data mode's 3-bit one is too small above 250 MHz);
  `WIDE_INT_BUS` always set, `WIDE_EXT_BUS` for the 4-bit bus.
- PIO in bursts of at most 8 words with the FIFO level from SDEDM.
- The state machine left in READWAIT or WRITESTART1 (after an open-ended
  transfer or a failed command) is kicked into the data mode with
  `FORCE_DATA_MODE`; transfers without CMD12 wait for it the way
  `bcm2835_wait_transfer_complete()` does.
- CRC7 errors on R3 answers (CMD1, ACMD41) are ignored.
- Reset as `bcm2835_reset_internal()` (power off and on through SDVDD,
  20 ms each), and SDTOUT at half a second of card clocks.

Skipped:

- DMA through the BCM2835 DMA engine (DREQ 13). Linux needs a workaround
  there (the last three words of a multi-block read are taken by PIO,
  "drain_words"), and QEMU does not model the DMA requests, so it could not
  be checked here. PIO first.
- CMD23 (SET_BLOCK_COUNT) for reads, which Linux uses where the card has it:
  the bus manager does not read the SCR, so every multi-block transfer ends
  with CMD12.
- Linux' software timeout that resets the block when an interrupt never
  comes: the polling loops have their own timeouts; a controller that stays
  busy for half a second is reset.
- Changes of the core clock at run time (Linux follows them through the
  clock framework): the divider is computed once from the loader's value.
  `enable_uart=1` (in `data/boot/rpi/config.txt`) keeps the core clock fixed.

## How it was tested

`tools/rpi3/sd-test.sh` boots the minimum image (`jam -q @minimum-rpi4 build
airos-rpi-image`) from a copy of its SD image in QEMU 10.2's `raspi3b`. It
puts a file of random data and a `UserBootscript` on the BFS partition first
(`bfs_shell`); the script, run when the user session starts, checks the
file's SHA-256, reads 64 MiB of the raw card with `dd` and copies the file.
After QEMU has stopped, the results, the copy and the syslog are read back
from the image on the host:

```
RPI3_BUILD=/mnt/HaikuWork/rpi3/build-sdhost RPI3_QEMU_TB=multi \
	tools/rpi3/sd-test.sh 420 64
```

Results (QEMU, 2026-11-18): see the table below. The card answers CMD8,
ACMD41, CMD2/CMD3, switches to high speed, the disk is published with the
image's size, the kernel mounts the BFS volume, packagefs reads the packages
(compressed: corrupted data would fail there), the user land starts, the
SHA-256 of the test file matches the host's, the copy written by the guest
is identical to the original, and the syslog is on the card.

| QEMU mode | test file | raw read (`dd`, 64 MiB) | copy (read + write) | checksum | copy |
|---|---|---|---|---|---|
| multi TB | 16 MiB | 1.2 MB/s | (in the cache) | matches | identical |
| multi TB | 64 MiB | 0.58 MB/s | 0.53 MB/s | matches | identical |
| one insn per TB | 8 MiB | 0.95 MB/s | (in the cache) | matches | identical |

(The second run shared the host with a one-insn-per-tb run.) Without a card
the bus gives up after CMD8, CMD1 and ACMD41 time out, and the kernel stops
with "did not find any boot partitions", as before.

The throughput is QEMU's: every FIFO word is an emulated MMIO access. On the
board PIO is bound by the 50 MHz 4-bit bus (25 MB/s at most) and the CPU's
uncached accesses; it has not been measured there.

## On a real Pi 3

To check first: the loader's "SD host clock" line (250 MHz expected), the
"sdhost: registers ..." line, "SD card in high speed mode, 50 MHz", and a raw
`dd` read for the throughput. Risks: QEMU's sdhost model has no data state
machine (SDEDM shows only the identification and data modes), sets the busy
and block status at once, and never fills the FIFO late or reports CRC or
FIFO errors, so the waits, the CMD12 while the state machine is still
reading ahead, the FIFO drain and the error paths have only run against the
model.
