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
| USB | both RP1 xHCIs (`xhci_fdt`), SuperSpeed (since 2026-11-15) | KVM keyboard, mouse, RNDIS, mass storage; an RTL9210 NVMe adapter (1 TB SSD) on a blue port reads 243-270 MB/s |
| Ethernet | `rp1_gem`, 1 Gbit/s | DHCP, ping 0.16 ms, no loss |
| Display | `rpi_display` on the HVS's display lists: both HDMI outputs, one desktop, at the modes the firmware set (1080p) | 2026-11-15: 3840x1080 desktop over the KVM (HDMI0) and a Dell 4K monitor (HDMI1, at 1080p); both pictures right (HDMI0 through the KVM, HDMI1 seen by the owner: the desktop's right half); blanks at 60.00 Hz; GLTeapot synced at 59 FPS |
| Watchdog | `airos.watchdog` (lab), restart through PSCI | resets after a hang |
| `airos.debug` | loader and kernel output on the screen | boots to the network (kernel pager: P) |
| GPU | `v3d` on the V3D 7.1.10 at 1.15 GHz, Mesa 25.3.6 | 2026-11-15: GL ES 3.1 and Vulkan 1.3 probes pass (1080p with depth exact), texture and readback probes pass, GLTeapot ~1690 FPS; `rpi4_v3d_probe` passes (TFU raster to UIF, uncached and cached) |
| Wi-Fi | `broadcomfmac` on the second SDHCI (sd2, GPIO 30-35, WL_ON gio 28) | WPA2 join, DHCP, ping and ssh over Wi-Fi |
| Bluetooth | `h4bcm` on the BCM7271 UART (GPIO 24-27, BT_ON gio 29) | patched, LE scan finds the room's devices |
| HEVC decoding | `rpi_hevc` on `/axi/codec@800000` (910 MHz, version 0x202) | all 25 test streams byte-exact (`rpi4_hevc_decode`) |
| Sound | `bcm2712_hdmi_audio`: the HDMI controllers' MAI fed by a 40 bit DMA channel, 16 bit stereo at 44.1 or 48 kHz, output HDMI 0 or 1, software volume | 2026-11-15: the Media Kit's default output; 48000 frames per second through the mixer for 30 s; the KVM's HDMI receiver reports the audio stream starting and stopping with each tone (nothing while idle); not listened to |
| Temperature | `rpi_thermal` through the firmware mailbox | loads |

Not done yet:

- Display: no mode setting. The firmware picks each output's mode at boot
  (1080p here, also for the 4K monitor; it ignores `hdmi_mode` on the
  Pi 5) and only for outputs connected then; air/OS shows its regions
  unscaled at those modes (no scaled modes, a software cursor). Mode
  setting needs the pixel valves and the HDMI PHY/PLL (Linux:
  `vc4_crtc.c`, `vc4_hdmi_phy.c`); a hardware cursor a second plane with
  its own UPM range. The firmware KMS tags are no way round it: the Pi 5
  firmware accepts `SET_PLANE` (0x00048015) but shows nothing.
- Video: the BCM2712 has no H.264 decoder and no video encoder at all
  (only the HEVC decoder): H.264 decoding and all encoding stay in
  software (FFmpeg).
- The RP1's I2S (sound HATs), the fan, the RTC, the power button, CPU
  frequency scaling. The fan is PWM1 channel 3 on GPIO 45 (41566 ns,
  inverted, tach count at PWM1 + 0x3c); the firmware leaves
  `cooling_fan` disabled and PWM1's clock off, and reading an RP1 block
  whose clock is off stalls the SoC for good (it took a power cycle). The
  lab board idles at 60 C, so it probably has no cooler. The RTC
  (firmware tag 0x00030087) counts from 0 after a power cut: no battery
  on the lab board. The firmware runs the cores at 2.4 GHz (its maximum;
  minimum 1.5 GHz), so frequency scaling would only save power.
- The PCIe x1 connector (NVMe) and USB 3 speeds: the lab board has no NVMe
  drive and no USB 3 device to test with.

## Sound

The Pi 5 has no headphone jack, and its firmware no sound service (no
VCHIQ): `bcm2712_hdmi_audio` drives the HDMI controllers' audio itself, on
top of the mode the firmware set (Linux: `vc4_hdmi.c`, `bcm2835-dma.c`).

- The MAI takes IEC 60958 subframes (sample in bits 12-27, channel status
  bit 30, parity bit 31, preamble 8 at each 192 frame block start). A
  real-time thread converts the Media Kit's 16 bit samples, applying the
  volume, into a ring of four 20 ms periods, two ahead of the DMA.
- DMA channel 7 of the 40 bit controller at 0x10_0001_0600 (the device
  tree leaves 7-11 to the OS) runs one control block per period, cyclic,
  into `MAI_DATA` (0x10_7c72_001c for HDMI 0, +0x20 for HDMI 1), paced by
  DREQ 12 or 13; the cell in the HDMI node's `dmas` carries the DREQ and
  the QoS flags. It interrupts at the end of each period (GIC SPI 87).
- Audio clock regeneration: N 6144 (48 kHz) or 6272 (44.1 kHz) and CTS
  from the pixel clock, which the firmware measures for HDMI 0 (property
  tag 0x00030047, clock 9: 148.5 MHz at 1080p60). The firmware leaves the
  vertical front porch out of the timing registers, so the fallback
  estimate for HDMI 1 is a little low.
- The MAI's sample clock comes from the DVP's 108 MHz gate: n/m = 2250/1
  (48 kHz) or 120000/49 (44.1 kHz). The D0 stepping moved the MAI
  threshold fields.
- The KVM's receiver (LT6911) logs a pair of interrupts when the audio of
  its input starts and when it stops: the check that a stream is there.

## Display

The firmware sets both HDMI outputs up at boot (`max_framebuffers=2`,
`hdmi_force_hotplug`), each HVS channel n feeding HDMI n with one plane:
its frame buffer, unscaled. `rpi_display` (also the Pi 4's driver)
attaches to `brcm,bcm2712-hvs` and, instead of the Pi 4's firmware
planes:

- reads each channel's mode from `DISPn_CTRL0` (width, lines) and the
  firmware's plane from the display list `DISPn_LPTRS` points at (HVS
  memory from 0x4000, D0 register offsets 0x100 + 0x40 n);
- shows a region of its frame buffer by writing a copy of that plane
  (address, pitch, size, centered position changed; the firmware's UPM
  fields kept) into one of two slots per channel at the end of the list
  memory (words 0xe00 on), then pointing `LPTRS` at it: the compositor
  takes it at the next frame;
- points `LPTRS` back at the firmware's list when app_server lets go, so
  the boot console shows again;
- follows the blanks with the channel's frame count (`DISPn_STATUS`
  bits 21:16) and the connection with `HDMI_HOTPLUG` (HDMI block + 0x1c8).
  An output plugged in after boot stays dark: the firmware did not start
  its channel.

The accelerant gets `RPI_DISPLAY_FIXED_MODES`: each output's own mode
only, and a region the size of the mode (app_server's own rendering
scale is fine).

Read the HVS through `/dev/misc/mem` in blocks (`dd bs=512`): four-byte
reads came back wrong.
