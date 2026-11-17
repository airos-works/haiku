# Raspberry Pi 3: booting air/OS

The Pi 3 (BCM2837, four Cortex-A53, 1 GB) starts from the same SD image as
the Pi 4 and Pi 5 (build/jam/images/RpiBootImage). Its boot ROM loads
`bootcode.bin` from the card's FAT partition, `bootcode.bin` loads the
VideoCore firmware `start.elf`, and that reads `config.txt` (the `[pi3]`
section) and starts `airos-loader.img` in 64-bit mode with the device tree
(`bcm2710-rpi-3-b-plus.dtb` on a 3 B+) and the boot archive.

## What differs from the Pi 4

- **Peripherals** at 0x3f000000, the cores' local interrupt controller at
  0x40000000. The loader knows a BCM2837 by its Cortex-A53 cores (MIDR)
  before it reads the tree, and maps that window as device memory from the
  start.
- **No GIC.** The BCM2836/BCM2837 have the cores' local controller (timers,
  four mailboxes per core, the VideoCore's interrupts as one input) and the
  VideoCore's ARM control block (device interrupts in three banks). The
  kernel (`arch_int_bcm2836.cpp`) numbers them like a GIC's: the IPI is 0,
  the timers PPIs 26/27/29/30, VideoCore interrupt n is 32 + n, the ARM's
  basic bank 96 + n. The loader rewrites the tree's two-cell specifiers into
  GIC-style ones in the kernel's copy, so the device tree bus and drivers
  find the right numbers.
- **SD card** on the "sdhost" controller (`brcm,bcm2835-sdhost`); the other
  one (Arasan, `brcm,bcm2835-mmc`) is the Wi-Fi chip's (GPIO 34-39).
- **USB** on a Synopsys DWC2 OTG core; on the 3 B+ the LAN7515 hub and its
  LAN7800 gigabit Ethernet are behind it (`microchip_lan78xx`).
- **Console** on the PL011 (GPIO 14/15, 115200 8N1); Bluetooth on the mini
  UART (`dtoverlay=miniuart-bt`), whose baud rate follows the core clock:
  `core_freq=250`.

## Lab

`tools/rpi3/lab.sh` (NanoKVM-PCIe 192.168.1.33, power through the Home
Assistant plug shared with the Dragon Q8B). `tools/rpi3/qemu-run.sh` boots
the loader and boot archive in QEMU's `raspi3b`.

The lab board's boot ROM does not boot the KVM's virtual USB disk: it resets
the port once (full speed) and gives up, with the composite gadget and with
the disk alone. A card that holds only a current `bootcode.bin` and an empty
`timeout` file boots the KVM's disk -- with the gadget showing the disk only
(`lab.sh gadget msd`); with the composite gadget it enumerates the device and
stops. `bootcode.bin` patched with `sed -i s/BOOT_UART=0/BOOT_UART=1/` logs
on GPIO 14.

Debugging a board that shows nothing (2026-11-18): the firmware's reads of
the KVM disk can be traced on the KVM (drop its page cache, boot, then
`mincore()` over the image file shows which files the firmware read; pad
the files 8 MB apart against read-ahead). A 96-byte kernel that waits 3 s
and resets the board through the PM watchdog proves the ARM runs (the KVM's
dmesg shows a boot loop). That is how the lab's "Pi 3 B+" turned out to be
a Pi 2 B v1.1 (revision 0xa21041, Cortex-A7: 64-bit kernels never start).
