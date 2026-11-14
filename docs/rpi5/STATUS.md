# Raspberry Pi 5: status

Board: Raspberry Pi 5 Model B rev 1.1, 4 GB (c04171), BCM2712 D0 (the firmware applies overlays/bcm2712d0.dtbo), RP1 C0.
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
| GPU | `v3d` on the V3D 7.1.10 at 1.15 GHz, Mesa 25.3.6 | 2026-11-15: GL ES 3.1 and Vulkan 1.3 probes pass (1080p with depth exact), texture and readback probes pass, GLTeapot ~1690 FPS; `rpi4_v3d_probe`'s TFU layout check fails (only a quarter of its expected tiles), see below |
| Wi-Fi | `broadcomfmac` on the second SDHCI (sd2, GPIO 30-35, WL_ON gio 28) | WPA2 join, DHCP, ping and ssh over Wi-Fi |
| Bluetooth | `h4bcm` on the BCM7271 UART (GPIO 24-27, BT_ON gio 29) | patched, LE scan finds the room's devices |
| HEVC decoding | `rpi_hevc` on `/axi/codec@800000` (910 MHz, version 0x202) | all 25 test streams byte-exact (`rpi4_hevc_decode`) |
| Temperature | `rpi_thermal` through the firmware mailbox | loads |

Not done yet:

- GPU: the TFU (texture formatting unit) runs jobs, but `rpi4_v3d_probe`
  finds only 1024 of the 4096 words it expects of a 64x64 raster to
  linear-tile conversion; Mesa's own uses (texture probe, mipmaps) pass.
  The probe's idea of the 7.1 output layout is the first suspect.
- Display driver: no HVS/HDMI driver (Pi 4: `rpi_display` with firmware
  planes); no mode setting, one screen, no second HDMI port, so no
  direct GL presentation either.
- Video: the BCM2712 has no H.264 decoder and no video encoder at all
  (only the HEVC decoder): H.264 decoding and all encoding stay in
  software (FFmpeg).
- Sound (HDMI audio, or the RP1's I2S for a HAT), the fan (RP1 PWM), the
  RTC, the power button, CPU frequency scaling.
- The PCIe x1 connector (NVMe) and USB 3 speeds: the lab board has no NVMe
  drive and no USB 3 device to test with.
