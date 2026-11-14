# How air/OS boots on the Raspberry Pi 5

The Pi 5 runs the same SD card image and the same loader as the Pi 4
(docs/rpi4/BOOT.md); this file is about what differs.

```
EEPROM bootloader (no start*.elf on the Pi 5)
  reads config.txt ([pi5] section) and loads:
    bcm2712-rpi-5-b.dtb (or the d0 / 500 / CM5 variant it picks)
    airos-loader.img        kernel=..., kernel_address=0x80000
    airos-boot.tgz          initramfs ... followkernel
    cmdline.txt             -> /chosen/bootargs
  -> TF-A (BL31, built into the firmware, at 0 - 0x80000)
  -> airos-loader.img, core 0, EL2, MMU off, x0 = device tree
     cores 1-3 stay in TF-A until PSCI CPU_ON
  -> kernel_arm64 from the boot archive
  -> the BFS partition on the SD card
```

## What the loader does differently on a BCM2712

| Area | Pi 4 (BCM2711) | Pi 5 (BCM2712) |
| --- | --- | --- |
| Other cores | spin table (`cpu-release-addr`) | PSCI `CPU_ON` (SMC); the kernel gets the conduit for restart and power off |
| Peripherals | 0xfc00_0000 - 4 GB | above 64 GB: SoC at 0x10_0000_0000, PCIe windows up to 0x20_0000_0000; the identity map has them as device memory, the low 4 GB are all RAM |
| Mailbox | message at bus address phys \| 0xc000_0000 | physical address |
| Console UART | PL011 on GPIO 14/15 | the RP1's UART0 on GPIO 14/15, behind PCIe (see below) |
| SD card | EMMC2 | `brcm,bcm2712-sdhci`, same driver (`bcm2711_emmc2`); fixed 200 MHz base clock put into the kernel's tree |

## The RP1

GPIO 14/15, USB and Ethernet are in the RP1, a chip on the BCM2712's PCIe x4
link. The kernel does not bring that link up itself:

- `config.txt` has `enable_rp1_uart=1` and `pciex4_reset=0` for the Pi 5: the
  firmware sets UART0 up at 115200 baud and leaves the link and the RP1 as
  they are. `dtparam=uart0_console` points the `console` alias at UART0.
- The firmware maps the RP1's BAR 1 (its peripherals) at CPU address
  0x1c_0000_0000; the device tree's `ranges` say 0x1f_0000_0000. The loader
  reads the controller's outbound windows and the BAR and corrects the
  RP1's `ranges` (in the firmware's tree, so the kernel's copy has it too).
  An access to the wrong window stalls the core for good.
- Interrupts: RP1 IRQ N becomes MSI-X vector N, a write of N to the MIP
  (`brcm,bcm2712-mip`), which raises GIC SPI 128 + N, edge triggered. The
  loader programs the MIP, an inbound window for the message address if
  the firmware left none, the RP1's MSI-X table and its per-IRQ enables, and
  rewrites the RP1 devices' `interrupts` in the kernel's tree as GIC SPIs.
  Level triggered sources (Ethernet) have to be acknowledged at the RP1:
  their node gets `airos,rp1-iack`, the register their driver writes
  `1 << 2` to after each interrupt.
- The kernel's FDT bus translates addresses through the PCI-style
  three-cell `ranges` on the way.
- The GICv2 code makes SPIs that the tree calls edge triggered so.

## Lab options in `cmdline.txt`

| Word | Effect |
| --- | --- |
| `airos.watchdog` | the power manager's watchdog restarts the board if the loader or the kernel stops for 15 s (the kernel reloads it from a timer) |
| `airos.rp1dump` | the PCIe controller's, the RP1's and the MIP's registers as the firmware left them |

Loader faults print their syndrome; a loader panic restarts the board
through PSCI after 15 s.

## The lab (tools/rpi5)

- NanoKVM at 192.168.1.39 (HDMI0, USB, serial: its ttyS1 is the Pi's GPIO
  14/15). Its ATX power lines are not connected to the Pi 5.
- Power: the Home Assistant plug "PiSwitch" (since 2026-11-14):
  `RPI4_STATE=/mnt/HaikuWork/rpi5/state tools/rpi4/power.py status|on|off|cycle`.
  A cold start from the SD card answers pings after about 60 s.
- The EEPROM's BOOT_ORDER is 0xf614 since 2026-11-14 (it was 0xf461, SD
  first): USB first, so the KVM's virtual disk decides what starts. An
  "idle" disk (rpi5-idle.img, FAT without boot files) lets it fall through
  to the SD card; rpi5-recovery.img starts the recovery OS; pi5-boot.img
  (`make-boot-disk.sh`) a test build.
- `lab.sh`: serial capture on the KVM, the virtual disk (`lun`; it unbinds
  the KVM's USB gadget for a moment when the Pi holds the disk), uploads,
  in-place updates, screenshots. Never change the disk while the EEPROM
  reads it (during the first seconds of a start): that wedged the EEPROM
  twice until a power cycle. `update-boot` rewrites only the boot partition,
  so a system running from the same disk's BFS keeps its volume.
- After `lab.sh lun` had to unbind the gadget, the KVM's keyboard can stay
  dead in air/OS while the mouse works: `POST /api/hid/reset` on the KVM
  brings it back.
- A running air/OS restarts from the kernel debugger over serial: send
  `+++` (one character a second), then `reboot`.
- With the lab's 3 A supply the EEPROM refuses USB boot unless that disk's
  config.txt has `usb_max_current_enable=1` (the lab disks have it).
- The recovery OS is the Pi 4 lab's Alpine image with a [pi5] section:
  DHCP .182, `ssh -F state/ssh_config rpi5-recovery`. Unmounting and
  deleting its USB disk does not free the KVM's LUN; `lab.sh lun` copes.
- Credentials live in /mnt/HaikuWork/rpi5/state (the KVM's web user is
  "admin").
