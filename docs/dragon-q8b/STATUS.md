# Radxa Dragon Q8B (Qualcomm SC8280XP) status

What works on the board, what was measured, and what is still open. The
board boots air/OS through its UEFI firmware, which also hands over an ACPI
table set and a device tree; the kernel prefers the device tree.

| Area | State | Evidence |
|---|---|---|
| Boot (UEFI, NVMe) | air/OS installs onto the NVMe with the arm64 ISO's Installer ("whole disk") and boots to the desktop | Samsung PM981; `haiku_loader` page-aligned RX/RW sections for the firmware's W^X |
| Serial console | the GENI debug UART carries the kernel log | loader re-votes the QUP2 SE1 clocks after ExitBootServices |
| Clock | the firmware's GetTime(), passed as `haiku,firmware-clock` | |
| Ethernet (2 x 2.5GbE, TC9563 switch, QCA8081 PHYs) | both ports, 1 Gbit/s tested (the lab switch is gigabit), DHCP; 106 MB/s down, 73 MB/s up | `tc956x` driver; MACs from the board's I2C EEPROM |
| Wi-Fi (Intel AX210) | joins WPA2 networks over 802.11ax, 38.6 MB/s on the LAN | `iaxwifi200` |
| USB | the three DWC3 controllers at USB 2 | SuperSpeed ports show Rx.Detect with `dwc3_superspeed true`; no USB 3 device tested yet |
| GPU (Adreno 690) | Vulkan 1.3 (Mesa Turnip), OpenGL ES 3.2 and OpenGL through zink | `adreno` kernel driver; GLTeapot ~885 fps in its window, `pvr_glbench` 2650 frames/s with readback; WebGL in Summit |
| Display | HDMI (DisplayPort 2 through the board's CH7218A bridge): own frame buffer, vertical blank interrupt (60.00 Hz), EDID over DP AUX, mode setting on the trained link | `qcom_display`; 1080p set again through the full path; other modes are correct on the Dragon's side but the lab's NanoKVM does not lock onto them, so they need a monitor to verify |
| Multiple displays | not yet: the USB-C ports' DisplayPort alt mode goes through the ADSP's pmic-glink | |
| Video decode (Iris) | the codec's firmware boots and answers HFI's system init; a decode session opens and closes from userland | `qcom_iris`; decoding itself is in progress |
| Video encode | not started | |
| Applications | Amp, Burrow, DeckCraft, GridCraft, Kiri, Mail, PdfCraft, WordCraft, airShot, airTime open their windows on the NVMe install | LightCraft and PhotoCraft show empty windows (not looked into) |
| SD card | SDHC2 is clocked by the firmware (SDHCI v3); no card in the slot yet, and `sdhci_fdt` needs a Qualcomm glue | |

## Known problems

- A GPU fault cannot be recovered from yet: the GPU is then marked lost,
  its users get errors (Vulkan: device lost) and new processes fall back to
  software OpenGL until a restart.
- Each process gets an 8 GiB window of the one GPU address space (no
  per-process GPU page tables yet).
- NVMe: now and then, at boot, the controller completes a command the host
  did not submit (generic status 0x0b, as from a zeroed submission entry);
  it is logged and skipped instead of panicking, and the command it took
  the place of times out once.
- Summit shows only digits of page text on the CI install, with software
  OpenGL too: a font fallback problem of that build, not the GPU.

## Lab

Tools are in `/mnt/HaikuWork/dragon/tools` (not in the tree): image
iteration over the NanoKVM's USB disk, GPU, display and video codec test
loops, the lab probe (`tools/dragon-q8b/qcom_probe`).
