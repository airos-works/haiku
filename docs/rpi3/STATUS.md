# Raspberry Pi 3: status

| Area | State |
|---|---|
| Loader, interrupts, timer, SMP | QEMU raspi3b: kernel up to mounting the boot volume |
| SD card (sdhost) | in progress (branch rpi3-sdhost) |
| USB (DWC2) | in progress (branch rpi3-dwc2) |
| Ethernet (LAN7800 behind the LAN7515) | microchip_lan78xx (FreeBSD if_muge), builds; needs USB |
| Wi-Fi (CYW43455 on the Arasan SDIO) | broadcomfmac's SDIO host knows the BCM2837; untested |
| Bluetooth (mini UART) | h4bcm knows the BCM2837; untested |
| GPIO header | rpi_gpio knows the BCM2835 pulls; untested |
| Sound (firmware AUDS over vchiq) | vchiq has the BCM2835 page lists; untested |
| Display (firmware frame buffer + planes) | rpi_display knows the HVS 4; untested |
| 3D (VideoCore IV, Mesa vc4) | in progress (branch rpi3-vc4) |

The lab board on NanoKVM .33 was a Pi 2 B (no 64-bit): waiting for the real
Pi 3 B+ and for the serial wiring (KVM RX must go to pin 8).
