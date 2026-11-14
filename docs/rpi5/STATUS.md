# Raspberry Pi 5: status

Board: Raspberry Pi 5 Model B rev 1.1, 4 GB (c04171), BCM2712 C1, RP1 C0.
The image is the Pi image (`airos-rpi-image`); one SD card starts both a
Pi 4 and a Pi 5. How it boots: BOOT.md.

| Area | State | Evidence (2026-11-14) |
| --- | --- | --- |
| Boot from SD | works | the released CI image image-rpi4-20261114054454-32f74065ac (hrev60207+2661): First-Boot Setup, SD expanded to 127.7 GB, restart, desktop |
| Boot from USB disk | works | desktop from the KVM's virtual disk |
| CPU | 4 x Cortex-A76 up to 2.4 GHz | About this system; PSCI 1.1 (SMC) for the cores, restart |
| Memory | 4 GB | About this system: 4084 MiB |
| Serial console | GPIO 14/15 (RP1 UART0) | loader and kernel output, KDL in and out |
| SD card | `bcm2711_emmc2` on the BCM2712 slot, SDMA, 50 MHz high speed | 128 GB card read and written |
| USB | both RP1 xHCIs (`xhci_fdt`), USB 2 only (as on the other DWC3 boards) | KVM keyboard, mouse, RNDIS, mass storage |
| Ethernet | `rp1_gem`, 1 Gbit/s | DHCP, ping 0.16 ms, no loss |
| Display | the firmware's frame buffer, 1920x1080, 32 bits | desktop on HDMI0 |
| Watchdog | `airos.watchdog` (lab), restart through PSCI | resets after a hang |
| `airos.debug` | loader and kernel output on the screen | boots to the network (kernel pager: P) |

Not done yet:

- GPU: the V3D 7.1 (`brcm,2712-v3d`) has no driver; the Pi 4's v3d
  driver and Mesa build are V3D 4.2.
- Display driver: no HVS/HDMI driver (Pi 4: `rpi_display` with firmware
  planes); no mode setting, one screen, no second HDMI port.
- Wi-Fi and Bluetooth: the CYW43455 is on the BCM2712's second SDHCI
  (non-removable) and a UART; `broadcomfmac` and `h4bcm` only know the
  Pi 4's controllers.
- HEVC decoder: written, not yet run on the board (branch `rpi5-hevc`).
  The BCM2712's block (`/axi/codec@800000`) is the Pi 4's: the same
  registers, interrupt control, firmware clock (11) and picture format
  (Linux's `hevc_dec` programs it as `rpivid` does; its two-buffer
  `NV12MT_COL128` is only other strides). `rpi_hevc` finds it by
  `brcm,bcm2712-hevc-dec`, maps it above 64 GB, and turns its IOMMU
  (`/axi/iommu@5100`, Linux's only) off if the firmware left it on;
  `rpi_firmware` now knows the BCM2712's mailbox (`0x107c013880`,
  physical addresses, no `0xc0000000` alias), so `rpi_thermal` and
  `/dev/misc/rpi_property` answer on the Pi 5 too. The add-on and
  `rpi4_hevc_decode` need no change. Test: docs/rpi4/MEDIA.md, "HEVC".
- Sound (HDMI audio), the fan (RP1 PWM), the RTC, the
  power button, the PCIe x1 connector (NVMe), USB 3 speeds, CPU frequency
  scaling.
