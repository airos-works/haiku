# Raspberry Pi 5: status

Board: Raspberry Pi 5 Model B rev 1.1, 4 GB (c04171), BCM2712 C1, RP1 C0.
The image is the Pi image (`airos-rpi-image`); one SD card starts both a
Pi 4 and a Pi 5. How it boots: BOOT.md.

| Area | State | Evidence (2026-11-14) |
| --- | --- | --- |
| Boot from SD | works | air/OS desktop from the SD card, minimum image |
| Boot from USB disk | works | desktop from the KVM's virtual disk |
| CPU | 4 x Cortex-A76 up to 2.4 GHz | About this system; PSCI 1.1 (SMC) for the cores, restart |
| Memory | 4 GB | About this system: 4084 MiB |
| Serial console | GPIO 14/15 (RP1 UART0) | loader and kernel output, KDL in and out |
| SD card | `bcm2711_emmc2` on the BCM2712 slot, SDMA, 50 MHz high speed | 128 GB card read and written |
| USB | both RP1 xHCIs (`xhci_fdt`), USB 2 only (as on the other DWC3 boards) | KVM keyboard, mouse, RNDIS, mass storage |
| Ethernet | `rp1_gem`, 1 Gbit/s | DHCP, ping 0.16 ms, no loss |
| Display | the firmware's frame buffer, 1920x1080, 32 bits | desktop on HDMI0 |
| Watchdog | `airos.watchdog` (lab), restart through PSCI | resets after a hang |

Not done yet:

- GPU: the V3D 7.1 (`brcm,2712-v3d`) has no driver; the Pi 4's v3d
  driver and Mesa build are V3D 4.2.
- Display driver: no HVS/HDMI driver (Pi 4: `rpi_display` with firmware
  planes); no mode setting, one screen, no second HDMI port.
- Wi-Fi and Bluetooth: the CYW43455 is on the BCM2712's second SDHCI
  (non-removable) and a UART; `broadcomfmac` and `h4bcm` only know the
  Pi 4's controllers.
- Sound (HDMI audio), the HEVC decoder, the fan (RP1 PWM), the RTC, the
  power button, the PCIe x1 connector (NVMe), USB 3 speeds, CPU frequency
  scaling.
- `airos.debug`: the kernel's screen output while the frame buffer was 16
  bits per pixel hung the boot; untested since `framebuffer_depth=32`.
