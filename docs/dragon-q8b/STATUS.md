# Radxa Dragon Q8B (Qualcomm SC8280XP) status

What works on the board, what was measured, and what is still open. The
board boots air/OS through its UEFI firmware, which also hands over an ACPI
table set and a device tree; the kernel prefers the device tree.

| Area | State | Evidence |
|---|---|---|
| Boot (UEFI, NVMe) | air/OS installs onto the NVMe with the arm64 ISO's Installer ("whole disk") and boots to the desktop; checked again with the CI image of 2026-10-24 (display, GPU, video codec and their firmware all in it); the install was then updated in place to the CI image of hrev60207+1839 (ADSP, SD slot, power key, temperature, all coming up at boot) | Samsung PM981; `haiku_loader` page-aligned RX/RW sections for the firmware's W^X |
| Serial console | the GENI debug UART carries the kernel log | loader re-votes the QUP2 SE1 clocks after ExitBootServices |
| Clock | the firmware's GetTime(), passed as `haiku,firmware-clock` | |
| Ethernet (2 x 2.5GbE, TC9563 switch, QCA8081 PHYs) | both ports, 1 Gbit/s tested (the lab switch is gigabit), DHCP; 106 MB/s down, 73 MB/s up | `tc956x` driver; MACs from the board's I2C EEPROM |
| Wi-Fi (Intel AX210) | joins WPA2 networks over 802.11ax, 38.6 MB/s on the LAN | `iaxwifi200` |
| Bluetooth (AX210) | the controller comes up with Intel's firmware (ibt-0041-0041) over USB; the radio receives: `bt_le scan 15` lists 21 LE advertisers in range (-74 to -101 dBm), `bt_le inquiry` finds classic devices with their class | `h2generic`; not yet paired: no device of the lab's own was in pairing mode, and the ones in range belong to others |
| USB | the three DWC3 controllers at USB 2 | SuperSpeed ports show Rx.Detect with `dwc3_superspeed true`; no USB 3 device tested yet |
| CPUs | the firmware leaves both EPSS frequency domains at their top level (4 x 2438 MHz, 4 x 2995 MHz), each with a table of 21 levels | a cpufreq module that followed the scheduler's hints was tried: no difference at idle (9 W at the wall either way, the plug reads in watts) and none measurable under load, so the clocks stay where the firmware puts them |
| Temperature | the hottest of the SoC's TSENS sensors, as /dev/power/qcom_thermal/0 (for AirTop and the like): 31 C idle, 45 C after 30 s of all eight cores busy; critical at 105 C (the device tree's trips) | `qcom_thermal` |
| Power | shut down (PSCI system off: 0 W at the wall within 5 s) and restart (PSCI reset, back in 47 s); the power button is published as /dev/power/button/power for power_daemon (`qcom_pwrkey`: the PMIC's PON key read over SPMI, arbiter v5, PON on channel 388) | a press has not been tried: nobody at the board, and the NanoKVM's ATX output is not wired to it |
| GPU (Adreno 690) | Vulkan 1.3 (Mesa Turnip), OpenGL ES 3.2 and OpenGL through zink | `adreno` kernel driver; the clock follows the load (270 MHz idle, 690 MHz busy); `pvr_glbench` 3600 frames/s with readback (2650 at the lowest clock), 1880/s at 1080p; WebGL in Summit |
| Display | HDMI (DisplayPort 2 through the board's CH7218A bridge): own frame buffer, vertical blank interrupt (60.00 Hz), EDID over DP AUX, mode setting on the trained link | `qcom_display`; 1080p set again through the full path; other modes are correct on the Dragon's side but the lab's NanoKVM does not lock onto them, so they need a monitor to verify |
| Audio DSP (ADSP) | started at boot by `qcom_adsp` (AOSS QMP load state, LCX/LMX RPMh votes until the DSP's handover, TrustZone PAS 1, SMP2P): ready 30 ms after the start; GLINK over SMEM up, the ADSP opens eight channels | linux-firmware's `qcom/sc8280xp/radxa/dragon-q8b/qcadsp8280.mbn` (ADSP.HT.5.6.c2-00037); our SMP2P item must be there before it starts |
| USB-C ports (pmic_glink) | the alternate mode reports are on (PMIC_RTR_ADSP_APPS, owner USBC_PAN): each port's orientation, mux, SVID, DisplayPort pin assignment and HPD are logged and acknowledged | nothing was plugged into the USB-C ports yet, so no report has come |
| Multiple displays | not yet. Done towards it: the ADSP's USB-C reports arrive; USB-C port 0's QMP combo PHY in DisplayPort mode locks its PLL at RBR and reports ready, DP0's link clock runs from it through dispcc 0, and DP0's AUX channel works through the PHY and the connector's SBU mux (a DPCD read with nothing plugged in ends in an AUX timeout, lab probe). Each connector's pixel clock runs from its PHY, and a second DPU path (VIG0 -> LM_0 -> CTL_0 -> INTF_0) scans out on DP0's clock without underruns. app_server places it as a second display (DP-1), hot plugged on the ADSP's reports (proven with a simulated one: the desktop grows to 3840x1080 and back), or forced on with the lab setting `usbc_dp0 forced` (Screen preferences and screenmode show two displays, layout changes move its scanout). Link training and the mainlink are written: without a sink the controller sends its training patterns and the stream comes up ready for video. the PHY locks at RBR to HBR3. Still to do: training against a monitor, all of which need a USB-C or DisplayPort monitor to test; the steps are in USB-C-DISPLAYPORT.md | `qcom_probe`'s `dp_phy_test()`, as Linux' phy-qcom-qmp-combo.c (sc8280xp_usb43dpphy_cfg) |
| Video decode (Iris) | H.264 and HEVC Main (eight bit 4:2:0, up to 4096x2304) on the codec, through the Media Kit (`qcom_video` add-on, FFmpeg fallback for what it does not decode) and airTime | `qcom_iris` driver, linux-firmware's `qcom/vpu/vpu20_p4.mbn`; byte-exact with libavcodec at 320x240, 720p and 1080p; 1080p H.264 at 306 pictures/s, 720p HEVC at 568/s through the Media Kit, at under one core |
| Video encode | H.264 High and HEVC Main on the codec (`qcom_video`'s IrisEncoder), in MediaConverter as the "Iris" hardware backend | 720p H.264 transcoded at 531 pictures/s (36.4 dB luma PSNR), HEVC at 596/s (35.2 dB); 1080p at ~320 pictures/s; several teams can use the codec at once |
| Applications | Amp, Burrow, DeckCraft, GridCraft, Kiri, Mail, PdfCraft, WordCraft, airShot, airTime, AirTop, Clipper (its history popup on Shift+Alt+V), Natter, Proton VPN, Proton Mail Bridge (Qt 6) and TurboChook open their windows on the NVMe install (AirTop, Natter, Proton VPN and TurboChook from their own release packages: the CI image does not carry them); LightCraft and PhotoCraft draw with Vulkan on Turnip; Snap runs its effects through zink at 60 frames/s (1 ms a frame); Summit plays video on the codec | the boot script sets the ROCK 5's Panfrost EGL vendor aside on the Dragon: with both Mesas in one team Snap crashed making its second context current; Aurora runs Electron apps on Node.js 22 (air/OS CI's arm64 `nodejs22`; Aurora's arm64 package built with CI's `build-app-packages.sh`, its runner being down), and Code - OSS 1.142 runs on it with the workbench and the integrated terminal (`build/haiku/cross-arm64.sh` in jmgasper/vscode cross-builds its native modules); Claude Code's Haiku build compiles native helpers on the machine, and arm64 air/OS has no compiler, so it is not ported; LCDMonitor needs the Trofeo USB panel, AirPins and the Pi Installer are for the Raspberry Pi |
| SD card | `sdhci` drives the slot (`sdhci_qcom.cpp`, set up as Linux' sdhci-msm: GCC reset and clocks, vendor registers, 3.0 V signalling, vmmc/vqmmc through RPMh, the power interrupt acknowledged) and follows the card detect switch (GPIO 131: the schematic, page 38, has it closed to ground without a card); the slot is empty, so it reports no card; with the detect inverted (setting `qcom_cd_active_low`) the bus scan runs and gives up cleanly (CMD8, CMD1 and CMD55 time out). A card has not been tried | DMA through SMMU context bank 2, which the firmware leaves untranslated (SCTLR.M 0); the bounce buffer's cached alias is cleaned before each transfer, as for NVMe |

## Known problems

- A GPU fault or hang loses the work in flight (its users get errors,
  Vulkan reports the device lost); the driver then resets the GPU (bus
  halt, software reset, its start again) and new work runs, without a
  restart.
- Each process gets an 8 GiB window of the one GPU address space (no
  per-process GPU page tables yet).
- NVMe: now and then (2 of 109 logged boots) the controller completed a
  zeroed submission entry while the slot held the real command: arm64's
  NVMe DMA memory is noncached, the cached linear map's stale line of it
  answered the controller's read on the Dragon's cache-coherent PCIe. The
  driver now cleans those lines before the doorbell: 30 boots in a row
  since without one (at 2% that is still possible by chance, so the entry
  stays logged and skipped).
- Summit showed only digits of page text on arm64 CI installs: it includes
  `/boot/system/settings/fonts/fonts.conf`, which only x86_64's fontconfig
  package provides, so it found only its emoji fonts. arm64 CI images now
  ship that file, including the configuration Summit's engine carries.

## Lab

Tools are in `/mnt/HaikuWork/dragon/tools` (not in the tree): image
iteration over the NanoKVM's USB disk, GPU, display and video codec test
loops, the lab probe (`tools/dragon-q8b/qcom_probe`).
