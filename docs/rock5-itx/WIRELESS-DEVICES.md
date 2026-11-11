# Wireless and Bluetooth devices (2026-09-26)

The Intel AX210 work (see `AX210.md`) established one path for a combo card:
a Wi-Fi driver on Haiku's FreeBSD/OpenBSD compatibility layer, firmware
shipped as data, a userland tool that loads the Bluetooth firmware over
`usb_raw` before `bluetooth_server` starts, a login hook, and the Wi-Fi and
Bluetooth preferences. This document records extending that path to every
other card it can serve, and lists the devices that should now work.

## What changed

**Bluetooth firmware: `bt_firmware`** (`src/bin/bt_firmware`, in the system
package as `/boot/system/bin/bt_firmware`). It replaces the AX210-only
`rock5_ax210_bt_loader` with a tool that finds every Intel and Realtek USB
Bluetooth controller and brings it up the way Linux does:

- Intel, all three generations of `btintel.c`: ROM parts (7260, 7265, 3160,
  3165, 3168; optional `.bseq` patch), RSA bootloader parts (8260, 8265,
  9260, 9560, AX200, AX201) and TLV bootloader parts (AX210, AX211, AX411
  and the Wi-Fi 7 BE2xx), with RSA or ECDSA headers and Blazar's
  intermediate loader. File names follow `btintel_get_fw_name*()` exactly.
- Realtek, from `btrtl.c`: RTL8723A/B/D, RTL8761A/BU, RTL8821A/C,
  RTL8822B/CU, RTL8851BU, RTL8852AU/BU/BTU/CU and RTL8922AU, with epatch v1
  ("Realtech") and v2 ("RTBTCore") files and their configuration files.
- Endpoints are found from the descriptors instead of assumed indices.
- Each controller is set up in a forked child. Haiku's `usb_raw` holds one
  lock per device for the whole wait of a transfer, so only one transfer can
  be outstanding and a pending read cannot be abandoned; waits are bounded
  by `alarm()` and a hung controller costs only its child.

Options: `--list`, `--present`, `--info` (read state and show the chosen
files, change nothing), `--supported`, `--firmware-dir DIR`, `--verbose`.

**MediaTek Bluetooth in `h2generic`.** The MT7921/MT7922 firmware download
from the `x399-workstation` branch (`h2mediatek.cpp`, verified there on a
TP-Link TX55E) is ported, together with that branch's rule that HCI
endpoints come from interface 0 only. The download is attempted only for
the USB vendors of MediaTek-based modules in Linux btusb (LG, Foxconn,
Lite-On, MediaTek, IMC/AzureWave, Quectel, 35f5), so no Intel or Realtek
radio is sent a MediaTek vendor request.

**`h2generic` device table** lists every Intel combo-card ID from btusb
(`BTUSB_INTEL_COMBINED`), not only the AX210's `8087:0032`.

**Boot helper** `tools/rock5-itx/start-ax210-bluetooth.sh` (name kept for
UserBootscript and the preferences' `start-services` hook) now starts
`bluetooth_server` for any adapter h2generic finds and runs `bt_firmware`
first only when an Intel or Realtek controller is present. Before, a
non-AX210 adapter never got a server.

**Full images (2026-10-03): the server loads the firmware.** In a full image
(air/OS) the launch_daemon runs `bluetooth_server` as a service and starts it
again the moment it quits, so the helper's "stop the server, run
`bt_firmware`" raced a fresh server that opened the radio in the middle of
the download ("fragment 691 rejected"); the AX210 stayed in its bootloader
and answered every command "Unknown HCI Command" (the preferences showed
"Unknown|ServerFailed" and "Low Energy scan failed"). Now the server runs
`bt_firmware` itself for each controller that appeared since its previous
run, before it opens the device (`src/servers/bluetooth/ControllerFirmware.cpp`);
the steps are in the LE log. Full images no longer install the helper or the
`start-services` hook, and the preferences and BluetoothStatus start and stop
the server through its launch_daemon job. The server's own Deskbar replicant
is gone; BluetoothStatus is the only Bluetooth tray icon.

**Wi-Fi drivers for arm64.** The regular image definition enabled the WLAN
drivers only on x86, x86_64 and riscv64. It now enables `iaxwifi200`,
`idualwifi7260`, `iprowifi4965`, `iprowifi3945`, `atheroswifi`,
`ralinkwifi`, `realtekwifi`, `realtekwifi8187` and `zydwifi1211` on arm64
as well, so they are in the arm64 Haiku package; the legacy PCI/CardBus
drivers stay x86/riscv64-only. All build for arm64 and every import
resolves against the arm64 kernel. The minimum-based lab image still adds
only `iaxwifi200`, through `UserBuildConfig`.

**Firmware as packages.** The full image now installs architecture-neutral
firmware packages instead of the two loose AX210 files:
`intel_wifi_firmwares`, `ralink_wifi_firmwares`, `realtek_wifi_firmwares`
(HaikuPorts, imported by `tools/rock5-itx/import-wifi-firmware-packages.sh`)
and `intel_bluetooth_firmwares`, `realtek_bluetooth_firmwares`,
`mediatek_bluetooth_firmwares`, built from linux-firmware 20240318 by
`tools/rock5-itx/build-bluetooth-firmware-packages.sh`. Firmware stays out
of Git; `build-full.sh` checks the packages are present.

**zstd packages do not work on arm64.** The arm64 build has no zstd build
feature, so packagefs logs `failed to init package` for zstd-compressed
packages and never mounts them. HaikuPorts' current Intel and Ralink Wi-Fi
firmware packages are zstd, which would have left a fresh image's AX210
without Wi-Fi firmware. The import script recompresses them with zlib
(contents unchanged), and `build-full.sh` refuses a zstd firmware package.

## Verification

| Level | What | Evidence |
|---|---|---|
| Hardware | AX210 Bluetooth from bootloader to operational with `bt_firmware` on four reboots: ECDSA header, 2,972 fragments accepted, boot, 2 DDC records; `bluetooth_server` LE scan finds devices; Wi-Fi rejoins | `artifacts/intel-bt/reboot{1..4}-serial.log`, `after-reboot{1..4}.txt` |
| Hardware | New `h2generic` (Intel IDs, MediaTek port) drives the AX210; MediaTek path stays silent for Intel | `reboot2-serial.log`, `after-reboot2.txt` |
| Hardware | The eight added Wi-Fi drivers load and probe on the ROCK 5 without crashing (no matching hardware present) | `artifacts/intel-bt/wlan-driver-load-test.txt` |
| Hardware | Installed image state: six firmware packages activated on the board, fourth reboot through the new boot helper, Bluetooth and Wi-Fi up, no package failures | `reboot4-serial.log`, `after-reboot4.txt` |
| Build | Full `rock5full-mmc` image with the Wi-Fi drivers in the Haiku package, `bt_firmware` and the six zlib firmware packages | `artifacts/rock5full-build-20260926T033149Z.log` |
| Host | Every Intel `.sfi` (63), `.ddc` (56) and `.bseq` (9) in linux-firmware parses; the AX210 image splits into the 2,972 fragments seen on hardware; Intel names match Linux for AX210, Blazar, 8260/8265, 9260, AX200 and ROM parts; every Realtek USB chip's firmware builds a patch and is refused for other chips | `src/bin/bt_firmware/host_test.cpp` |

Only the AX210 has been exercised on hardware. Everything else below
"should work": the driver, firmware and setup sequence exist and follow the
upstream implementations, but no such device has been tested here.

## Wi-Fi support

Card type decides what fits a machine: **M.2/mini PCIe cards** work in the
ROCK 5's M.2 E-key slot (mini PCIe through an adapter); **CNVi modules**
(Intel "integrated" parts) only work in Intel laptops and boards; **USB
adapters** work in any USB port.

| Family | Driver | Card type | Standard | Firmware package |
|---|---|---|---|---|
| Intel AX210 | `iaxwifi200` | M.2 | Wi-Fi 6E, HE association verified | intel_wifi_firmwares |
| Intel BE200 (`8086:272b`) | `iaxwifi200` (Bz family) | M.2 | Wi-Fi 7 card, used as 802.11a/b/g/n/ac; association verified | intel_wifi7_firmwares |
| Intel AX200 (incl. Killer AX1650) | `iaxwifi200` | M.2 | Wi-Fi 6 | intel_wifi_firmwares |
| Intel AX201, AX211, AX411 (incl. Killer AX1650i/1675i/1690i) | `iaxwifi200` | CNVi | Wi-Fi 6/6E | intel_wifi_firmwares |
| Intel 9260, and 9461/9462/9560 M.2 modules (Thunder Peak, `8086:2526`) | `idualwifi7260` | M.2 | 802.11ac | intel_wifi_firmwares |
| Intel 9560/9462/9461 CNVi (Cannon Lake, Cannon Point-LP, Gemini Lake) | `idualwifi7260` | CNVi | 802.11ac | intel_wifi_firmwares |
| Intel 8265, 8260, 7265, 7260, 3168, 3165, 3160 | `idualwifi7260` | M.2 / mini PCIe | 802.11ac | intel_wifi_firmwares |
| Intel 6205, 6235, 6230, 6300, 6250, 6200, 6150, 5300, 5100, 5150, 5350, 4965, 2230, 2200, 1030, 1000, 135, 130, 105, 100 | `iprowifi4965` | mini PCIe / half-size | 802.11n | intel_wifi_firmwares |
| Intel PRO/Wireless 3945ABG | `iprowifi3945` | mini PCIe | 802.11a/b/g | intel_wifi_firmwares |
| Qualcomm Atheros AR5210/5211/5212/5213, AR2413/2417/2427, AR5413/5424, AR5416/5418, AR9160, AR922x/9227, AR928x/9285/9287, AR9380/9382, AR9462/9463/9482, AR9485, AR958x, AR9565, AR1111 | `atheroswifi` | PCI / mini PCIe / M.2 | 802.11a/b/g/n | none needed |
| Ralink/MediaTek RT2560, RT2561/2561S, RT2661, RT2760/2790, RT2860/2890, RT3060/3062, RT3090/3091/3092, RT3390, RT3562/3591/3592/3593, RT5390/5392 | `ralinkwifi` | PCI / mini PCIe | 802.11b/g/n | ralink_wifi_firmwares |
| Realtek RTL8188CE, RTL8192CE, RTL8188EE | `realtekwifi` | mini PCIe | 802.11n | realtek_wifi_firmwares |
| Realtek RTL8188CU/CUS/CTV/RU, RTL8191CU, RTL8192CU, RTL8188EU/ETV, RTL8192EU, RTL8812AU, RTL8821AU | `realtekwifi` | USB | 802.11n/ac | realtek_wifi_firmwares |
| Ralink/MediaTek RT2070, RT2770, RT2870, RT3070/3071/3072, RT3370, RT3572/3573, RT5370/5372, RT5572, RT8070 | `ralinkwifi` (run) | USB | 802.11n | ralink_wifi_firmwares |
| MediaTek MT7601U | `ralinkwifi` (mtw) | USB | 802.11n | ralink_wifi_firmwares |
| Ralink RT2501/RT2573, RT2500USB | `ralinkwifi` (rum, ural) | USB | 802.11b/g | ralink_wifi_firmwares / none |
| Realtek RTL8187L, RTL8187B | `realtekwifi8187` | USB | 802.11b/g | none needed |
| ZyDAS ZD1211, ZD1211B | `zydwifi1211` | USB | 802.11b/g | none needed |

623 PCI and USB IDs in total; Appendix A lists every one.

### Intel BE200 (2026-11-02)

The BE200 (`8086:272b`, Gl MAC with the Fm radio) is the first Bz-family
card `iaxwifi200` drives. What the Bz family changed, taken from Linux
iwlwifi: the MAC access request, "init done" and software reset moved into
`CSR_GP_CNTRL` (bits 21, 6 and 31, with bus-master disable in 28/29), the
ROM is started with `CSR_GP_CNTRL` bit 7 after writing `CSR_FUNC_SCRATCH`
instead of `UREG_CPU_INIT_RUN`, the RX free-buffer write pointer goes
through `HBUS_TARG_WRPTR`, the MAC address straps sit at CSR 0x30, and the
driver leaves the LTR alone.

Firmware: `iwx-gl-c0-fm-c0-83` with its PNVM, from linux-firmware
20240312, in the `intel_wifi7_firmwares` package. API 83 is the newest
image that still takes this driver's pre-MLD commands (Linux 6.18 wants
100 or later and the MLD API). That image has 67 runtime sections (the
driver allowed 60), sections the driver did not know (types 65 and 0x444,
now skipped as Linux does), a fragmented PNVM (whose segment count the
driver never reset on stop: the second start dereferenced unallocated
segments) and version 8 of the MCC (regulatory) response.

The firmware no longer answers the legacy MAC_CONTEXT_CMD, so the Bz family
uses the driver's MLD command path (MAC_CONFIG/LINK_CONFIG/STA_CONFIG, so
far enabled only for Ma devices), and it associates with HT/VHT: this
fork's HE station set-up uses STA_HE_CTXT_CMD, which that firmware does not
answer either.

On the ROCK 5 the card sits on the M.2 E-key slot (PCIe segment 2); the
firmware profile admits it next to the AX210 (it sets the multi-function
bit of its header type). Verified there: the card attaches, loads the
firmware, reads its address, scans (without antennas, signal 2 to 8) and
joins Gaspers (WPA2-PSK/CCMP) at 802.11ac with a DHCP address.
Its Bluetooth half (`8087:0036`, `ibt-0291-0291`) loads its firmware with
`bt_firmware` as before.

### HE channel width, and HE for the BE200 (2026-11-11)

Tested on the Framework 13's AX210 (firmware 77, air/OS hrev60207+2559) against
Gaspers on 5 GHz, channel 157, an 80 MHz HE access point. TCP to a wired
machine, in Mbit/s (the same test over the laptop's wired port: 580 down, 564
up with four streams):

| Driver and setting | Association | 1 stream down / up | 4 streams down / up |
|---|---|---|---|
| before this work | HE, 80 MHz, MCS 9 to 11, two streams | 131 / 163 | 217 / 254 |
| this work, default | HE, 80 MHz | 123 to 134 / 153 to 162 | 202 to 212 / 246 to 252 |
| `he 0` | VHT, 80 MHz | 4 / 55 | 4 / 57 to 62 |
| `he 0`, `max_width 40` | VHT, 40 MHz | 4 / 57 | 4 / 57 |
| `he 0`, `max_width 20` | VHT, 20 MHz | 4 / 51 | 4 / 47 |

The width of an HE association has always followed the AP's HT and VHT
operation elements; the "20 MHz" in `STATUS.md` described one ROCK 5
association without antennas. So HE is not slower than VHT here, it is fifty
times faster:

- **VHT on this access point is broken, and not by the channel width.** With
  a VHT-only association the AP sends every data frame at 6 Mbit/s legacy
  OFDM (sampled from the receive descriptors: rate 0xc100), from the first
  frame on, at any width; the upload is then limited by the TCP ACKs coming
  back at that rate. The card's own transmit rate is fine (VHT MCS 8 or 9,
  two streams), block ack sessions are set up in both directions and nothing
  is dropped on receive. The association request is unremarkable (HT and VHT
  capabilities as OpenBSD sends them). Ruled out: the station width flags
  (old and new value), the TLC's maximum MPDU length, a VHT operating mode
  notification after the handshake, and HE being switched off at run time
  rather than at boot. With HE capabilities in the same association request
  the AP starts at HE MCS 0 and climbs to MCS 8 or 9 within a few frames.
  The cause is open; without a sniffer it cannot be told whether the AP's VHT
  frames fail at this card or the AP never tries them. This is what a BE200
  gets on this network as long as it associates without HE.
- **Why HE delivers a quarter of its air rate.** Not the CPU (the busiest
  thread uses 13 % of one core) and not the channel width. On upload the
  firmware's block ack reports show about 600 aggregates a second of 39
  frames on average (the limit is 64: the block ack window, and the driver
  sends no A-MSDUs), about 10 % of them retried, 640 Mbit/s while on air,
  and the card on air for only 45 % of the time. What takes the rest of the
  channel (the AP's own traffic, a mesh backhaul on the same channel) could
  not be seen from the card. Longer aggregates (the 256-frame HE block ack
  window, A-MSDUs on transmit) would raise the card's share; neither exists
  in this driver.

What changed in the driver:

- **160 MHz for HE.** The driver advertises HE at 160 MHz (unless the card
  variant has none) and uses it when the AP runs a 160 MHz channel, signalled
  either way the standard allows, and the regulatory data allows it: PHY
  context with the control channel position inside the 160 MHz channel, the
  station's width, and the 160 MHz rates in the TLC configuration. VHT-only
  associations stay at 80 MHz. **Tested only as far as an 80 MHz AP goes**:
  the association with the 160 MHz capability advertised works as before; no
  160 MHz access point was in range.
- **Station width flags.** At 80 MHz the driver set "40 MHz" and "80 MHz"
  together, which the firmware reads as 160 MHz. It now sends one value
  (no measurable difference on the AX210).
- **A deprecated 160 MHz announcement** (width 160 with a single centre) made
  the driver program an 80 MHz channel with the wrong control channel
  position. The 80 MHz centre is now derived from the primary channel
  (not tested: no such AP).
- **BSS colour flags.** "Partial BSS colour" and "BSS colour disabled" were
  swapped in the HE station context.
- **BE200 (Bz family) HE: written, not run.** With the MLD commands the HE
  parameters go into `LINK_CONFIG_CMD` (BSS colour, default packet extension,
  RTS threshold, after the MAC context reports HE) and `STA_CONFIG_CMD`
  (packet extension thresholds, HTC support, ACK-enabled aggregation, the HE
  A-MPDU length extension), as Linux's `iwl_mvm_link_changed()` and
  `iwl_mvm_mld_cfg_sta()` do, and HE is no longer withheld from the Bz
  family. Firmware 83 lists `TLC_MNG_CONFIG_CMD` version 4, which the driver
  has. The ROCK 5 with the BE200 had no power. Forcing the MLD commands on
  the AX210 (`mld_api 1`) says nothing about it: firmware 77 asserts
  (0x20100232) at the `LINK_CONFIG_CMD` that every MLD association sends,
  before any HE parameter, as OpenBSD notes for older cards. If the BE200's
  firmware rejects the new commands, `he 0` gives the HT/VHT association back.
- **Syslog.** One line per association says what was negotiated and what the
  AP offers ("associated on channel 157, HE (802.11ax), 80 MHz (AP: 80 MHz,
  HE)"); the line per rate change, several a second under load, is gone. The
  driver also says how many 6 GHz channels the regulatory data allows: "0 of
  59" on this laptop (see [6 GHz](WIFI-6GHZ.md)).
- **Lab switches** in `~/config/settings/kernel/drivers/iaxwifi200`, read
  when the interface is brought up: `he 0` (HT/VHT only; turning HE back on
  needs a reboot if it was off at boot), `max_width 20|40|80|160` (narrows
  the card's side only; the AP is not told, so with HE at `max_width 40` the
  upload ran at 174 Mbit/s and the download at 20), `mld_api 1` (MLD commands
  on a card that defaults to the older ones).

The test kit (throughput scripts, the transmit rate probe, the procedure) is
in `fw13/tests/wifi-he` of the lab workspace.

## Bluetooth support

| Family | Setup | Card type | Firmware package |
|---|---|---|---|
| Intel AX210 (tested) | `bt_firmware`, TLV/ECDSA | M.2 | intel_bluetooth_firmwares |
| Intel AX211, AX411, BE200/BE201/BE202 | `bt_firmware`, TLV (Blazar: intermediate loader + `-usb` image) | CNVi / M.2 | intel_bluetooth_firmwares |
| Intel AX200, AX201, 9260, 9560/9462 | `bt_firmware`, RSA bootloader | M.2 / CNVi | intel_bluetooth_firmwares |
| Intel 8260, 8265 | `bt_firmware`, RSA bootloader | M.2 | intel_bluetooth_firmwares |
| Intel 7260, 7265, 3160, 3165, 3168 | `bt_firmware`, ROM (+ optional patch) | M.2 / mini PCIe | intel_bluetooth_firmwares |
| Realtek RTL8761B/BU (most BT 5.x USB dongles: TP-Link UB500, ASUS USB-BT500, ...) | `bt_firmware` | USB | realtek_bluetooth_firmwares |
| Realtek RTL8821C, RTL8822B/C, RTL8852A/B/BT/C, RTL8851B, RTL8922A (Bluetooth half of Realtek Wi-Fi cards) | `bt_firmware` | M.2 / USB | realtek_bluetooth_firmwares |
| Realtek RTL8723A/B/D, RTL8821A, RTL8761A | `bt_firmware` | USB / M.2 | realtek_bluetooth_firmwares |
| MediaTek MT7921, MT7922 (AMD RZ608/RZ616, TP-Link TX55E, ...) | `h2generic` (h2mediatek) | M.2 | mediatek_bluetooth_firmwares |
| Any other class-compliant USB adapter that needs no firmware (CSR8510, Broadcom BCM20702 on its ROM firmware, ...) | none | USB | none |

The Wi-Fi half of the Realtek RTL8821CE/8822BE/8822CE/8852xE and MediaTek
MT7921/MT7922 cards has no driver (see below), so for those cards only
Bluetooth works. Appendix B lists the 138 explicit Bluetooth IDs (Intel
15, Realtek 87, MediaTek 36); Intel and Realtek parts not listed are still
recognised by vendor and Bluetooth class.

## Not supported, and why

- **Intel BE201/BE202 Wi-Fi (Wi-Fi 7 CNVi)**: Intel-only modules; the
  discrete BE200 is supported (see "Intel BE200" below).
- **Realtek RTL8821CE, RTL8822BE/CE, RTL8852AE/BE/CE Wi-Fi**: need rtw88 or
  rtw89, which Haiku does not have.
- **MediaTek MT7921/MT7922/MT7925 Wi-Fi**: needs mt76. The x399 branch's
  native `mt7922` driver scans but does not yet join.
- **Qualcomm QCA6174, QCA9377, WCN685x (ath10k/ath11k) Wi-Fi** and
  **Broadcom FullMAC** cards: no driver.
- **Qualcomm/Atheros Bluetooth needing firmware** (AR3012, QCA61x4,
  WCN685x): no loader yet; the same `bt_firmware` structure would take one.
- **MediaTek MT7920, MT7925, MT7663, MT7668 Bluetooth**: MT7920 needs a
  firmware flavor h2mediatek does not compute, MT7925 firmware is not in
  the linux-firmware release packaged here, MT7663/MT7668 use an older
  loader.
- **Broadcom Bluetooth patchram**: ROM firmware only; no patch download.

## Files

- `src/bin/bt_firmware/` — tool, per-vendor logic, host test
- `src/add-ons/kernel/drivers/bluetooth/h2/h2generic/` — Intel IDs,
  `h2mediatek.{h,cpp}`, interface-0 endpoints
- `build/jam/images/definitions/minimum` — `bt_firmware` in `SYSTEM_BIN`
- `build/jam/images/definitions/regular` — Wi-Fi drivers on arm64
- `tools/rock5-itx/UserBuildConfig` — firmware packages
- `tools/rock5-itx/import-wifi-firmware-packages.sh`,
  `build-bluetooth-firmware-packages.sh`, `build-full.sh` — firmware
  packages and their checks
- `tools/rock5-itx/start-ax210-bluetooth.sh` — login hook

To rebuild the Bluetooth firmware packages from a newer linux-firmware,
run the script with that tree (`build-bluetooth-firmware-packages.sh
/path/to/linux-firmware /path/to/licenses`) and update the package names in
`UserBuildConfig` and `build-full.sh` if the version changes. To re-run the
host test:

```sh
g++ -O1 -Wall -o bt_firmware_host_test host_test.cpp \
	IntelBluetoothFirmware.cpp RealtekBluetoothFirmware.cpp
./bt_firmware_host_test /path/to/decompressed/firmware   # intel/, rtl_bt/
```

## Appendix A: Wi-Fi device IDs

Generated on 2026-09-26 from the drivers' ID tables (for `atheroswifi`, from the device IDs its HAL attaches to). Names come from pci.ids, the drivers and FreeBSD usbdevs.

### Intel Wi-Fi 6/6E — `iaxwifi200` (PCIe)

| ID | Device |
|---|---|
| `8086:2723` | Wi-Fi 6 AX200 |
| `8086:02f0` | Comet Lake PCH-LP CNVi WiFi |
| `8086:a0f0` | Wi-Fi 6 AX201 |
| `8086:34f0` | Ice Lake-LP PCH CNVi WiFi |
| `8086:06f0` | Comet Lake PCH CNVi WiFi |
| `8086:43f0` | Tiger Lake PCH CNVi WiFi |
| `8086:3df0` | ? |
| `8086:4df0` | Jasper Lake PCH CNVi WiFi |
| `8086:2725` | Wi-Fi 6E(802.11ax) AX210/AX1675* 2x2 [Typhoon Peak] |
| `8086:2726` | ? |
| `8086:51f0` | Alder Lake-P PCH CNVi WiFi |
| `8086:7a70` | Raptor Lake-S PCH CNVi WiFi |
| `8086:7af0` | Alder Lake-S PCH CNVi WiFi |
| `8086:7e40` | Meteor Lake PCH CNVi WiFi |
| `8086:7f70` | ? |
| `8086:54f0` | CNVi: Wi-Fi |
| `8086:51f1` | Raptor Lake PCH CNVi WiFi |

### Intel Wireless-AC — `idualwifi7260` (PCIe)

| ID | Device |
|---|---|
| `8086:08b3` | Wireless 3160 |
| `8086:08b4` | Wireless 3160 |
| `8086:3165` | Wireless 3165 |
| `8086:3166` | Dual Band Wireless-AC 3165 Plus Bluetooth |
| `8086:24fb` | Dual Band Wireless-AC 3168NGW [Stone Peak] |
| `8086:08b1` | Wireless 7260 |
| `8086:08b2` | Wireless 7260 |
| `8086:095a` | Wireless 7265 |
| `8086:095b` | Wireless 7265 |
| `8086:24f3` | Wireless 8260 |
| `8086:24f4` | Wireless 8260 |
| `8086:24fd` | Wireless 8265 / 8275 |
| `8086:2526` | Wi-Fi 5(802.11ac) Wireless-AC 9x6x [Thunder Peak] |
| `8086:9df0` | Cannon Point-LP CNVi [Wireless-AC] |
| `8086:a370` | Cannon Lake PCH CNVi WiFi |
| `8086:31dc` | Gemini Lake PCH CNVi WiFi |

### Intel Wireless-N / Advanced-N — `iprowifi4965` (PCIe)

| ID | Device |
|---|---|
| `8086:0082` | Intel Centrino Advanced-N 6205 |
| `8086:0083` | Intel Centrino Wireless-N 1000 |
| `8086:0084` | Intel Centrino Wireless-N 1000 |
| `8086:0085` | Intel Centrino Advanced-N 6205 |
| `8086:0087` | Intel Centrino Advanced-N + WiMAX 6250 |
| `8086:0089` | Intel Centrino Advanced-N + WiMAX 6250 |
| `8086:008a` | Intel Centrino Wireless-N 1030 |
| `8086:008b` | Intel Centrino Wireless-N 1030 |
| `8086:0090` | Intel Centrino Advanced-N 6230 |
| `8086:0091` | Intel Centrino Advanced-N 6230 |
| `8086:0885` | Intel Centrino Wireless-N + WiMAX 6150 |
| `8086:0886` | Intel Centrino Wireless-N + WiMAX 6150 |
| `8086:0890` | Intel(R) Centrino(R) Wireless-N 2200 BGN |
| `8086:0891` | Intel(R) Centrino(R) Wireless-N 2200 BGN |
| `8086:0887` | Intel Centrino Wireless-N 2230 |
| `8086:0888` | Intel Centrino Wireless-N 2230 |
| `8086:0896` | Intel Centrino Wireless-N 130 |
| `8086:0897` | Intel Centrino Wireless-N 130 |
| `8086:08ae` | Intel Centrino Wireless-N 100 |
| `8086:08af` | Intel Centrino Wireless-N 100 |
| `8086:0894` | Intel Centrino Wireless-N 105 |
| `8086:0895` | Intel Centrino Wireless-N 105 |
| `8086:0892` | Intel Centrino Wireless-N 135 |
| `8086:0893` | Intel Centrino Wireless-N 135 |
| `8086:4229` | Intel Wireless WiFi Link 4965 |
| `8086:422b` | Intel Centrino Ultimate-N 6300 |
| `8086:422c` | Intel Centrino Advanced-N 6200 |
| `8086:422d` | Intel Wireless WiFi Link 4965 |
| `8086:4230` | Intel Wireless WiFi Link 4965 |
| `8086:4232` | Intel WiFi Link 5100 |
| `8086:4233` | Intel Wireless WiFi Link 4965 |
| `8086:4235` | Intel Ultimate N WiFi Link 5300 |
| `8086:4236` | Intel Ultimate N WiFi Link 5300 |
| `8086:4237` | Intel WiFi Link 5100 |
| `8086:4238` | Intel Centrino Ultimate-N 6300 |
| `8086:4239` | Intel Centrino Advanced-N 6200 |
| `8086:423a` | Intel WiMAX/WiFi Link 5350 |
| `8086:423b` | Intel WiMAX/WiFi Link 5350 |
| `8086:423c` | Intel WiMAX/WiFi Link 5150 |
| `8086:423d` | Intel WiMAX/WiFi Link 5150 |
| `8086:088e` | Intel Centrino Advanced 6235 |
| `8086:088f` | Intel Centrino Advanced 6235 |

### Intel PRO/Wireless 3945ABG — `iprowifi3945` (PCIe)

| ID | Device |
|---|---|
| `8086:4222` | PRO/Wireless 3945ABG [Golan] Network Connection |
| `8086:4227` | PRO/Wireless 3945ABG [Golan] Network Connection |

### Qualcomm Atheros — `atheroswifi` (PCI/PCIe IDs its HAL attaches to)

| ID | Device |
|---|---|
| `168c:0007` | AR5210 802.11a |
| `168c:0011` | AR5311 |
| `168c:0012` | AR5211 802.11ab |
| `168c:0013` | AR5212/AR5213 802.11abg |
| `168c:0014` | AR5212 compatible |
| `168c:0015` | AR5212 compatible |
| `168c:0016` | AR5212 compatible |
| `168c:0017` | AR5212 compatible |
| `168c:0018` | AR5212 compatible |
| `168c:0019` | AR5212 compatible |
| `168c:001a` | AR2413 802.11bg |
| `168c:001b` | AR5413 802.11abg |
| `168c:001c` | AR5424/AR2424 802.11abg (PCI Express) |
| `168c:001d` | AR2417 802.11bg |
| `168c:0023` | AR5416 802.11abgn |
| `168c:0024` | AR5418 802.11abgn (PCI Express) |
| `168c:0027` | AR9160 802.11abgn |
| `168c:0029` | AR922x 802.11abgn |
| `168c:002a` | AR928x 802.11abgn (PCI Express) |
| `168c:002b` | AR9285 802.11bgn (PCI Express) |
| `168c:002c` | AR2427 802.11bg (PCI Express) |
| `168c:002d` | AR9227 802.11bgn |
| `168c:002e` | AR9287 802.11bgn (PCI Express) |
| `168c:0030` | AR93xx (AR9380/AR9382) 802.11abgn |
| `168c:0032` | AR9485 802.11bgn |
| `168c:0033` | AR958x 802.11abgn |
| `168c:0034` | AR9462/AR9463/AR9482 802.11abgn |
| `168c:0036` | QCA9565/AR9565 802.11bgn |
| `168c:0037` | AR1111 802.11bgn |

### Ralink/MediaTek — `ralinkwifi` (PCI/PCIe)

| ID | Device |
|---|---|
| `1432:7708` | Edimax RT2860 |
| `1432:7711` | Edimax RT3591 |
| `1432:7722` | Edimax RT3591 |
| `1432:7727` | Edimax RT2860 |
| `1432:7728` | Edimax RT2860 |
| `1432:7738` | Edimax RT2860 |
| `1432:7748` | Edimax RT2860 |
| `1432:7758` | Edimax RT2860 |
| `1432:7768` | Edimax RT2860 |
| `1462:891a` | MSI RT3090 |
| `1814:0201` | Ralink Technology RT2560 |
| `1814:0301` | Ralink Technology RT2561S |
| `1814:0302` | Ralink Technology RT2561 |
| `1814:0401` | Ralink Technology RT2661 |
| `1814:0601` | Ralink Technology RT2860 |
| `1814:0681` | Ralink Technology RT2890 |
| `1814:0701` | Ralink Technology RT2760 |
| `1814:0781` | Ralink Technology RT2790 |
| `1814:3060` | Ralink Technology RT3060 |
| `1814:3062` | Ralink Technology RT3062 |
| `1814:3090` | Ralink Technology RT3090 |
| `1814:3091` | Ralink Technology RT3091 |
| `1814:3092` | Ralink Technology RT3092 |
| `1814:3390` | Ralink Technology RT3390 |
| `1814:3562` | Ralink Technology RT3562 |
| `1814:3592` | Ralink Technology RT3592 |
| `1814:3593` | Ralink Technology RT3593 |
| `1814:5360` | Ralink Technology RT5390 |
| `1814:5362` | Ralink Technology RT5392 |
| `1814:5390` | Ralink Technology RT5390 |
| `1814:5392` | Ralink Technology RT5392 |
| `1814:539a` | Ralink Technology RT5390 |
| `1814:539b` | Ralink Technology RT5390 |
| `1814:539f` | Ralink Technology RT5390 |
| `1a3b:1059` | AWT RT2890 |

### Realtek — `realtekwifi` (PCIe)

| ID | Device |
|---|---|
| `10ec:8176` | Realtek RTL8188CE |
| `10ec:8179` | Realtek RTL8188EE |
| `10ec:8178` | Realtek RTL8192CE |

### Realtek USB — `realtekwifi` (rtwn)

| ID | Device |
|---|---|
| `07b8:8188` | AboCom Systems RTL8188CU |
| `07b8:8189` | AboCom Systems RTL8188CU |
| `07b8:8178` | AboCom Systems RTL8192CU |
| `0b05:17ab` | ASUSTeK Computer RTL8192CU |
| `0b05:17ba` | ASUSTeK Computer USB-N10 Nano |
| `13d3:3358` | AsureWave RTL8188CE |
| `13d3:3359` | AsureWave RTL8188CE |
| `13d3:3357` | AsureWave RTL8188CU |
| `050d:2103` | Belkin Components F7D2102 Wireless Adapter |
| `050d:1004` | Belkin Components N300 Wireless Adapter |
| `050d:1102` | Belkin Components RTL8188CU Wireless Adapter |
| `050d:2102` | Belkin Components RTL8192CU Wireless Adapter |
| `04f2:aff7` | Chicony Electronics RTL8188CUS |
| `04f2:aff8` | Chicony Electronics RTL8188CUS |
| `04f2:aff9` | Chicony Electronics RTL8188CUS |
| `04f2:affa` | Chicony Electronics RTL8188CUS |
| `07aa:0056` | Corega RTL8192CU |
| `2001:3308` | D-Link RTL8188CU |
| `2001:3307` | D-Link RTL8192CU |
| `2001:3309` | D-Link RTL8192CU |
| `2001:330a` | D-Link RTL8192CU |
| `2001:330d` | D-Link DWA-131 rev B |
| `7392:7811` | Edimax EW-7811Un |
| `7392:7822` | Edimax RTL8192CU |
| `4855:0090` | FeiXun Communication RTL8188CU |
| `4855:0091` | FeiXun Communication RTL8192CU |
| `06f8:e033` | Guillemot HWNUP-150 |
| `0e66:0019` | Hawking RTL8192CU |
| `103c:1629` | Hewlett Packard RTL8188CU |
| `0846:9041` | BayNETGEAR WNA1000M |
| `0846:9021` | BayNETGEAR RTL8192CU |
| `9846:9041` | Netgear RTL8188CU |
| `0eb0:9071` | NovaTech RTL8188CU |
| `2019:ab2a` | Planex Communications RTL8188CU |
| `2019:ed17` | Planex Communications RTL8188CU |
| `2019:4902` | Planex Communications RTL8188CU |
| `2019:ab2e` | Planex Communications RTL8188CU |
| `2019:1201` | Planex Communications RTL8188CUS |
| `2019:ab2b` | Planex Communications RTL8192CU |
| `0bda:8170` | Realtek RTL8188CE |
| `0bda:817e` | Realtek RTL8188CE |
| `0bda:018a` | Realtek RTL8188CTV |
| `0bda:8176` | Realtek RTL8188CU |
| `0bda:817a` | Realtek RTL8188CU |
| `0bda:817b` | Realtek RTL8188CU |
| `0bda:8191` | Realtek RTL8188CU |
| `0bda:8754` | Realtek RTL8188CU |
| `0bda:818a` | Realtek RTL8188CUS |
| `0bda:817d` | Realtek RTL8188RU |
| `0bda:317f` | Realtek RTL8188RU |
| `0bda:817f` | Realtek RTL8188RU |
| `0bda:8177` | Realtek RTL8191CU |
| `0bda:817c` | Realtek RTL8192CE |
| `0bda:8178` | Realtek RTL8192CU |
| `0df6:0052` | Sitecom Europe RTL8188CU |
| `0df6:005c` | Sitecom Europe RTL8188CU |
| `0df6:0061` | Sitecom Europe RTL8192CU |
| `2357:0100` | TP-Link RTL8192CU |
| `20f4:648b` | TRENDnet RTL8188CU |
| `20f4:624d` | TRENDnet RTL8192CU |
| `0586:341f` | ZyXEL Communication RTL8192CU |
| `2001:3319` | D-Link DWA-131 rev E1 |
| `0bda:818b` | Realtek RTL8192EU |
| `2357:0107` | TP-Link TL-WN821N v5 |
| `2357:0108` | TP-Link TL-WN822N v4 |
| `2357:0109` | TP-Link TL-WN823N v2 |
| `07b8:8179` | AboCom Systems RTL8188EU |
| `0b05:18f0` | ASUSTeK Computer USB-N10 Nano rev B1 |
| `2001:3310` | D-Link DWA-123 rev D1 |
| `2001:330f` | D-Link DWA-125 rev D1 |
| `7392:b811` | Edimax EW-7811UN V2 |
| `056e:4008` | Elecom WDC-150SU2M |
| `2357:010c` | TP-Link TL-WN722N v2 |
| `2357:0111` | TP-Link TL-WN727N v5 |
| `0bda:0179` | Realtek RTL8188ETV |
| `0bda:8179` | Realtek RTL8188EU |
| `2c4e:0102` | Mercusys, Inc. Mercusys MW150US |
| `0b05:17d2` | ASUSTeK Computer USB-AC56 |
| `13b1:003f` | Cisco-Linksys WUSB6300 |
| `2001:3315` | D-Link DWA-182 rev C1 |
| `2001:3316` | D-Link DWA-180 rev A1 |
| `7392:a822` | Edimax EW-7822UAC |
| `04bb:0952` | I-O Data WN-AC867U |
| `0411:025d` | Melco WI-U3-866D |
| `0409:0408` | NEC Aterm WL900U |
| `2019:ab30` | Planex Communications GW-900D |
| `0bda:8812` | Realtek RTL8812AU Wireless Adapter |
| `0bda:881a` | Realtek RTL8812AU Wireless Adapter |
| `1740:0100` | Senao EnGenius EUB1200AC |
| `0df6:0074` | Sitecom Europe WLA-7100 |
| `2604:0012` | Tenda Tenda U12 |
| `2357:0101` | TP-Link Archer T4U |
| `2357:010d` | TP-Link Archer T4U ver 2 |
| `2357:0103` | TP-Link Archer T4UH ver 1 |
| `2357:010e` | TP-Link Archer T4UH ver 2 |
| `20f4:805b` | TRENDnet TEW-805UB |
| `0586:3426` | ZyXEL Communication ND6605 |
| `2001:3314` | D-Link DWA-171 rev A1 |
| `2001:3318` | D-Link DWA-172 rev A1 |
| `7392:a811` | Edimax EW-7811UTC |
| `7392:a812` | Edimax EW-7811UTC |
| `056e:400f` | Elecom WDB-433SU2M2 |
| `0e66:0023` | Hawking HD65U |
| `0411:0242` | Melco WI-U2-433DM |
| `0411:029b` | Melco WI-U2-433DHP |
| `0846:9052` | BayNETGEAR A6100 |
| `0bda:a811` | Realtek RTL8821AU |
| `0bda:0811` | Realtek RTL8821AU |
| `2357:011e` | TP-Link Archer T2U Nano |
| `2357:0120` | TP-Link Archer T2U Plus |
| `2357:011f` | TP-Link Archer T2U ver 3 |

### Ralink/MediaTek USB — `ralinkwifi` (run)

| ID | Device |
|---|---|
| `07b8:2770` | AboCom Systems RT2770 |
| `07b8:2870` | AboCom Systems RT2870 |
| `07b8:3070` | AboCom Systems RT3070 |
| `07b8:3071` | AboCom Systems RT3071 |
| `07b8:3072` | AboCom Systems RT3072 |
| `1482:3c09` | AboCom Systems RT2870 |
| `083a:7512` | Accton Technology RT2770 |
| `083a:b522` | Accton Technology RT2870 |
| `083a:6618` | Accton Technology RT2870 |
| `083a:7522` | Accton Technology RT2870 |
| `083a:a618` | Accton Technology RT2870 |
| `083a:8522` | Accton Technology RT2870 |
| `083a:7511` | Accton Technology RT3070 |
| `083a:a701` | Accton Technology RT3070 |
| `083a:a702` | Accton Technology RT3070 |
| `083a:c522` | Accton Technology RT3070 |
| `083a:a512` | Accton Technology RT3070 |
| `083a:d522` | Accton Technology RT3070 |
| `1eda:2310` | AirTies RT3070 |
| `8516:2070` | ALLWIN Tech RT2070 |
| `8516:2770` | ALLWIN Tech RT2770 |
| `8516:2870` | ALLWIN Tech RT2870 |
| `8516:3070` | ALLWIN Tech RT3070 |
| `8516:3071` | ALLWIN Tech RT3071 |
| `8516:3072` | ALLWIN Tech RT3072 |
| `8516:3572` | ALLWIN Tech RT3572 |
| `0e0b:9031` | Amigo Technology RT2870 |
| `0e0b:9041` | Amigo Technology RT2870 |
| `18c5:0008` | AMIT CG-WLUSB2GNR |
| `18c5:0012` | AMIT RT2870 |
| `15c5:0008` | AMIT RT2870 |
| `0b05:1731` | ASUSTeK Computer RT2870 |
| `0b05:1732` | ASUSTeK Computer RT2870 |
| `0b05:1742` | ASUSTeK Computer RT2870 |
| `0b05:1760` | ASUSTeK Computer RT2870 |
| `0b05:1761` | ASUSTeK Computer RT2870 |
| `0b05:1784` | ASUSTeK Computer USB-N13 |
| `0b05:1790` | ASUSTeK Computer RT3070 |
| `0b05:17ad` | ASUSTeK Computer USB-N66 |
| `0b05:179d` | ASUSTeK Computer ASUS Black Diamond Dual Band USB-N53 |
| `0b05:17e8` | ASUSTeK Computer USB-N14 |
| `1761:0b05` | ASUS USB-N11 |
| `13d3:3247` | AsureWave RT2870 |
| `13d3:3262` | AsureWave RT2870 |
| `13d3:3273` | AsureWave RT3070 |
| `13d3:3284` | AsureWave RT3070 |
| `13d3:3305` | AsureWave RT3070 |
| `050d:1103` | Belkin Components F9L1103 Wireless Adapter |
| `050d:815c` | Belkin Components F5D8053 v3 |
| `050d:825a` | Belkin Components F5D8055 |
| `050d:825b` | Belkin Components F5D8055 v2 |
| `050d:935a` | Belkin Components F6D4050 v1 |
| `050d:935b` | Belkin Components F6D4050 v2 |
| `050d:8053` | Belkin Components RT2870 |
| `050d:805c` | Belkin Components RT2870 |
| `13b1:002f` | Cisco-Linksys AE1000 |
| `167b:4001` | Cisco-Linksys RT3070 |
| `05a6:0101` | Cisco-Linksys RT3070 |
| `14b2:3c06` | Conceptronic RT2870 |
| `14b2:3c07` | Conceptronic RT2870 |
| `14b2:3c23` | Conceptronic RT2870 |
| `14b2:3c25` | Conceptronic RT2870 |
| `14b2:3c27` | Conceptronic RT2870 |
| `14b2:3c28` | Conceptronic RT2870 |
| `14b2:3c09` | Conceptronic RT2870 |
| `14b2:3c12` | Conceptronic RT2870 |
| `14b2:3c08` | Conceptronic RT3070 |
| `14b2:3c11` | Conceptronic RT3070 |
| `07aa:0042` | Corega CG-WLUSB300GNM |
| `07aa:002f` | Corega RT2870 |
| `07aa:003c` | Corega RT2870 |
| `07aa:003f` | Corega RT2870 |
| `07aa:0041` | Corega RT3070 |
| `129b:1828` | CyberTAN Technology RT2870 |
| `2001:3c09` | D-Link RT2870 |
| `2001:3c0a` | D-Link RT3072 |
| `2001:3c19` | D-Link DWA-125 rev A3 |
| `2001:3c1b` | D-Link DWA-127 Wireless Adapter |
| `2001:3c15` | D-Link DWA-140 rev B3 |
| `2001:3c1a` | D-Link DWA-160 rev B2 |
| `2001:3c20` | D-Link DWA-140 rev D1 |
| `2001:3c25` | D-Link DWA-130 rev F1 |
| `2001:3c1f` | D-Link DWA-162 Wireless Adapter |
| `07d1:3c13` | D-Link DWA-130 |
| `07d1:3c09` | D-Link RT2870 |
| `07d1:3c11` | D-Link RT2870 |
| `07d1:3c0d` | D-Link RT3070 |
| `07d1:3c0e` | D-Link RT3070 |
| `07d1:3c0f` | D-Link RT3070 |
| `07d1:3c15` | D-Link RT3070 |
| `07d1:3c16` | D-Link RT3070 |
| `07d1:3c0a` | D-Link RT3072 |
| `07d1:3c0b` | D-Link RT3072 |
| `7392:7717` | Edimax EW-7717 |
| `7392:7718` | Edimax EW-7718 |
| `7392:7733` | Edimax EW-7733UnD |
| `7392:7711` | Edimax RT2870 |
| `203d:1480` | Encore RT3070 |
| `203d:14a1` | Encore RT3070 |
| `203d:14a9` | Encore RT3070 |
| `1044:800c` | GIGABYTE GN-WB31N |
| `1044:800d` | GIGABYTE GN-WB32L |
| `1044:800b` | GIGABYTE RT2870 |
| `1690:0740` | Gigaset RT3070 |
| `1690:0744` | Gigaset RT3070 |
| `06f8:e030` | Guillemot HWNU-300 |
| `0e66:0009` | Hawking HWUN2 |
| `0e66:0001` | Hawking RT2870 |
| `0e66:0003` | Hawking RT2870 |
| `0e66:000b` | Hawking RT3070 |
| `04bb:0944` | I-O Data RT3072 |
| `04bb:0945` | I-O Data RT3072 |
| `04bb:0947` | I-O Data RT3072 |
| `04bb:0948` | I-O Data RT3072 |
| `1737:0078` | Linksys RT3070 |
| `1737:0070` | Linksys WUSB100 |
| `1737:0077` | Linksys WUSB54GC v3 |
| `1737:0071` | Linksys WUSB600N |
| `1737:0079` | Linksys WUSB600N v2 |
| `0789:0162` | Logitec RT2870 |
| `0789:0163` | Logitec RT2870 |
| `0789:0164` | Logitec RT2870 |
| `0789:0166` | Logitec LAN-W300N/U2 |
| `0789:0168` | Logitec LAN-W150N/U2 |
| `0789:0169` | Logitec LAN-W300N/U2S |
| `0411:0148` | Melco WLI-UC-G300HP |
| `0411:0150` | Melco RT2870 |
| `0411:012e` | Melco WLI-UC-AG300N |
| `0411:00e8` | Melco WLI-UC-G300N |
| `0411:016f` | Melco WLI-UC-G301N |
| `0411:015d` | Melco WLI-UC-GN |
| `0411:01a2` | Melco WLI-UC-GNM |
| `0411:01a8` | Melco WLI-UC-G300HP-V1 |
| `0411:01ee` | Melco WLI-UC-GNM2 |
| `100d:9031` | Motorola RT2770 |
| `100d:9032` | Motorola RT3070 |
| `0db0:3820` | Micro Star International RT3070 |
| `0db0:3821` | Micro Star International RT3070 |
| `0db0:3870` | Micro Star International RT3070 |
| `0db0:6899` | Micro Star International RT3070 |
| `0db0:821a` | Micro Star International RT3070 |
| `0db0:870a` | Micro Star International RT3070 |
| `0db0:899a` | Micro Star International RT3070 |
| `0db0:3822` | Micro Star International RT3070 |
| `0db0:3871` | Micro Star International RT3070 |
| `0db0:822a` | Micro Star International RT3070 |
| `0db0:871a` | Micro Star International RT3070 |
| `0846:9012` | BayNETGEAR WNDA4100 |
| `1b75:3072` | OvisLink RT3072 |
| `20b8:8888` | PARA Industrial RT3070 |
| `1d4d:0002` | Pegatron RT2870 |
| `1d4d:000c` | Pegatron RT3070 |
| `1d4d:000e` | Pegatron RT3070 |
| `1d4d:0010` | Pegatron RT3070 |
| `0471:200f` | Philips RT2870 |
| `2019:ab24` | Planex Communications GW-US300MiniS |
| `2019:ed14` | Planex Communications GW-USMicroN |
| `2019:ed06` | Planex Communications RT2870 |
| `2019:ab25` | Planex Communications RT3070 |
| `18e8:6259` | Qcom RT2870 |
| `0408:0304` | Quanta RT3070 |
| `148f:2070` | Ralink Technology RT2070 |
| `148f:2770` | Ralink Technology RT2770 |
| `148f:2870` | Ralink Technology RT2870 |
| `148f:3070` | Ralink Technology RT3070 |
| `148f:3071` | Ralink Technology RT3071 |
| `148f:3072` | Ralink Technology RT3072 |
| `148f:3370` | Ralink Technology RT3370 |
| `148f:3572` | Ralink Technology RT3572 |
| `148f:3573` | Ralink Technology RT3573 |
| `148f:5370` | Ralink Technology RT5370 |
| `148f:5372` | Ralink Technology RT5372 |
| `148f:5572` | Ralink Technology RT5572 |
| `148f:8070` | Ralink Technology RT8070 |
| `04e8:2018` | Samsung Electronics WIS09ABGN Wireless LAN adapter |
| `055d:2018` | Samsung Electronics RT2870 |
| `1740:9701` | Senao RT2870 |
| `1740:9702` | Senao RT2870 |
| `1740:0605` | Senao RT2870 |
| `1740:0615` | Senao RT2870 |
| `1740:9703` | Senao RT3070 |
| `1740:9705` | Senao RT3071 |
| `1740:9706` | Senao RT3072 |
| `1740:9707` | Senao RT3072 |
| `1740:9708` | Senao RT3072 |
| `1740:9709` | Senao RT3072 |
| `1740:9801` | Senao RT3072 |
| `0df6:0039` | Sitecom Europe RT2770 |
| `0df6:0017` | Sitecom Europe RT2870 |
| `0df6:002b` | Sitecom Europe RT2870 |
| `0df6:002c` | Sitecom Europe RT2870 |
| `0df6:002d` | Sitecom Europe RT2870 |
| `0df6:003e` | Sitecom Europe RT3070 |
| `0df6:0051` | Sitecom Europe RT3070 |
| `0df6:003b` | Sitecom Europe RT3070 |
| `0df6:003c` | Sitecom Europe RT3070 |
| `0df6:003d` | Sitecom Europe RT3070 |
| `0df6:0040` | Sitecom Europe RT3071 |
| `0df6:0041` | Sitecom Europe RT3072 |
| `0df6:0042` | Sitecom Europe RT3072 |
| `0df6:0047` | Sitecom Europe RT3072 |
| `0df6:0048` | Sitecom Europe RT3072 |
| `0df6:004a` | Sitecom Europe RT3072 |
| `0df6:004d` | Sitecom Europe RT3072 |
| `0df6:003f` | Sitecom Europe WL-608 |
| `15a9:0006` | SparkLAN RT2870 |
| `15a9:0010` | SparkLAN RT3070 |
| `177f:0153` | Sweex LW153 |
| `177f:0302` | Sweex LW303 |
| `177f:0313` | Sweex LW313 |
| `0930:0a07` | Toshiba RT3070 |
| `157e:300e` | U-MEDIA Communications RT2870 |
| `0cde:0022` | Z-Com RT2870 |
| `0cde:0025` | Z-Com RT2870 |
| `5a57:0280` | Zinwell RT2870 |
| `5a57:0282` | Zinwell RT2870 |
| `5a57:5257` | Zinwell RT3070 |
| `5a57:0283` | Zinwell RT3072 |
| `5a57:0284` | Zinwell RT3072 |
| `0586:3416` | ZyXEL Communication RT2870 |
| `0586:341a` | ZyXEL Communication RT2870 |
| `0586:341e` | ZyXEL Communication NWD2105 |
| `0586:3421` | ZyXEL Communication NWD2705 |
| `148f:2878` | Ralink Technology USB Storage |

### MediaTek USB — `ralinkwifi` (mtw)

| ID | Device |
|---|---|
| `7392:7710` | Edimax MT7601U |
| `148f:7601` | Ralink Technology MT7601 Mediatek Wireless Adpater |
| `2717:4106` | Xiaomi MT7601U |

### Ralink USB — `ralinkwifi` (rum)

| ID | Device |
|---|---|
| `07b8:b21b` | AboCom Systems HWU54DM |
| `07b8:b21c` | AboCom Systems RT2573 |
| `07b8:b21d` | AboCom Systems RT2573 |
| `07b8:b21e` | AboCom Systems RT2573 |
| `07b8:b21f` | AboCom Systems WUG2700 |
| `18c5:0002` | AMIT CG-WLUSB2GO |
| `0b05:1723` | ASUSTeK Computer RT2573 |
| `0b05:1724` | ASUSTeK Computer RT2573 |
| `050d:705a` | Belkin Components F5D7050A Wireless Adapter |
| `050d:905b` | Belkin Components F5D9050 ver 3 Wireless Adapter |
| `13b1:0020` | Cisco-Linksys WUSB54GC |
| `13b1:0023` | Cisco-Linksys WUSB54GR |
| `14b2:3c22` | Conceptronic C54RU |
| `07aa:002d` | Corega CG-WLUSB2GL |
| `07aa:002e` | Corega CG-WLUSB2GPX |
| `1371:9032` | Dick Smith Electronics C-Net CWD-854 rev F |
| `1371:9022` | Dick Smith Electronics RT2573 |
| `7392:7318` | Edimax USB Wireless dongle |
| `07d1:3c03` | D-Link DWL-G122 c1 |
| `07d1:3c04` | D-Link WUA-1340 |
| `07d1:3c06` | D-Link DWA-111 |
| `07d1:3c07` | D-Link DWA-110 |
| `1044:8008` | GIGABYTE GN-WB01GS |
| `1044:800a` | GIGABYTE GN-WI05GS |
| `1690:0722` | Gigaset RT2573 |
| `1631:c019` | Good Way Technology RT2573 |
| `06f8:e010` | Guillemot HWGUSB2-54-LB |
| `06f8:e020` | Guillemot HWGUSB2-54V2-AP |
| `1472:0009` | Huawei-3Com Aolynk WUB320g |
| `0411:00d9` | Melco WLI-U2-G54HP |
| `0411:00d8` | Melco WLI-U2-SG54HP |
| `0411:00f4` | Melco WLI-U2-SG54HG |
| `0411:0137` | Melco WLI-UC-G |
| `0411:0116` | Melco WLR-UC-G |
| `0411:0119` | Melco WLR-UC-G-AOSS |
| `0db0:6874` | Micro Star International RT2573 |
| `0db0:6877` | Micro Star International RT2573 |
| `0db0:a861` | Micro Star International RT2573 |
| `0db0:a874` | Micro Star International RT2573 |
| `0eb0:9021` | NovaTech RT2573 |
| `2019:ab01` | Planex Communications GW-US54HP |
| `2019:ab50` | Planex Communications GW-US54Mini2 |
| `2019:ed02` | Planex Communications GW-USMM |
| `18e8:6196` | Qcom RT2573 |
| `18e8:6229` | Qcom RT2573 |
| `18e8:6238` | Qcom RT2573 |
| `148f:2573` | Ralink Technology RT2501USB Wireless Adapter |
| `148f:9021` | Ralink Technology RT2501USB Wireless Adapter |
| `148f:2671` | Ralink Technology RT2601USB Wireless Adapter |
| `0df6:9712` | Sitecom Europe WL-113 rev 2 |
| `0df6:90ac` | Sitecom Europe WL-172 |
| `15a9:0004` | SparkLAN RT2573 |
| `0769:31f3` | Surecom Technology RT2573 |

### Ralink USB — `ralinkwifi` (ural)

| ID | Device |
|---|---|
| `0b05:1707` | ASUSTeK Computer WL-167g Wireless Adapter |
| `0b05:1706` | ASUSTeK Computer RT2500USB Wireless Adapter |
| `050d:7050` | Belkin Components F5D7050 Wireless Adapter |
| `050d:7051` | Belkin Components F5D7051 54g USB Network Adapter |
| `13b1:001a` | Cisco-Linksys HU200TS Wireless Adapter |
| `13b1:000d` | Cisco-Linksys WUSB54G Wireless Adapter |
| `13b1:0011` | Cisco-Linksys WUSB54GP Wireless Adapter |
| `14b2:3c02` | Conceptronic C54RU WLAN |
| `2001:3c00` | D-Link DWL-G122 b1 Wireless Adapter |
| `1044:8001` | GIGABYTE GN-54G |
| `1044:8007` | GIGABYTE GN-WBKG |
| `06f8:e000` | Guillemot HWGUSB2-54 WLAN |
| `0411:0066` | Melco WLI-U2-KG54 WLAN |
| `0411:0067` | Melco WLI-U2-KG54-AI WLAN |
| `0411:005e` | Melco WLI-U2-KG54-YB WLAN |
| `0411:008b` | Melco Nintendo Wi-Fi |
| `0db0:6861` | Micro Star International RT2570 |
| `0db0:6865` | Micro Star International RT2570 |
| `0db0:6869` | Micro Star International RT2570 |
| `0eb0:9020` | NovaTech NovaTech NV-902W |
| `148f:1706` | Ralink Technology RT2500USB Wireless Adapter |
| `148f:2570` | Ralink Technology RT2500USB Wireless Adapter |
| `148f:9020` | Ralink Technology RT2500USB Wireless Adapter |
| `0681:3c06` | Siemens 54g USB Network Adapter |
| `0707:ee13` | Standard Microsystems EZ Connect Wireless Adapter |
| `114b:0110` | Sphairon Access Systems GmbH UB801R |
| `0769:11f3` | Surecom Technology RT2570 |
| `0f88:3012` | VTech RT2570 |
| `5a57:0260` | Zinwell RT2570 |

### Realtek USB — `realtekwifi8187`

| ID | Device |
|---|---|
| `0846:4260` | BayNETGEAR WG111v3 |
| `0bda:8189` | Realtek RTL8187B Wireless Adapter |
| `0bda:8197` | Realtek RTL8187B Wireless Adapter |
| `0bda:8198` | Realtek RTL8187B Wireless Adapter |
| `0df6:0028` | Sitecom Europe WL-168 v4 |
| `0b05:171d` | ASUSTeK Computer P5B wireless |
| `050d:705e` | Belkin Components F5D7050E Wireless Adapter |
| `1737:0073` | Linksys WUSB54GC v2 |
| `0846:6a00` | BayNETGEAR WG111V2 |
| `0bda:8187` | Realtek RTL8187 Wireless Adapter |
| `0df6:000d` | Sitecom Europe WL-168 v1 |
| `0769:11f2` | Surecom Technology EP-9001-G rev 2A |

### ZyDAS USB — `zydwifi1211`

| ID | Device |
|---|---|
| `6891:a727` | 3Com 3CRUSB10075 |
| `07b8:6001` | AboCom Systems WL54 |
| `0b05:170c` | ASUSTeK Computer WL-159g |
| `129b:1666` | CyberTAN Technology TG54USB |
| `0675:0550` | DrayTek Vigor550 |
| `2019:ed01` | Planex Communications GW-US54GD |
| `2019:c007` | Planex Communications GW-US54GZL |
| `14ea:ab10` | Planex Communications GW-US54GZ |
| `14ea:ab13` | Planex Communications GW-US54Mini |
| `079b:004a` | Sagem XG-760A |
| `1740:2000` | Senao NUB-8301 |
| `0df6:9071` | Sitecom Europe WL-113 |
| `5173:1809` | Sweex ZD1211 |
| `0b3b:1630` | Tekram Technology QuickWLAN |
| `0b3b:5630` | Tekram Technology ZD1211 |
| `0b3b:6630` | Tekram Technology ZD1211 |
| `126f:a006` | TwinMOS G240 |
| `157e:3204` | U-MEDIA Communications ALL0298 v2 |
| `157e:300a` | U-MEDIA Communications TEW-429UB_A |
| `157e:300b` | U-MEDIA Communications TEW-429UB |
| `1435:0711` | Wistron NeWeb UR055G |
| `0cde:0011` | Z-Com ZD1211 |
| `0ace:1211` | Zydas Technology Corporation ZD1211 WLAN abg |
| `0586:3409` | ZyXEL Communication AG-225H |
| `0586:3401` | ZyXEL Communication ZyAIR G-220 |
| `0586:3407` | ZyXEL Communication G-200 v2 |
| `083a:4505` | Accton Technology SMCWUSB-G (no firmware) |
| `083a:4506` | Accton Technology SMCWUSB-G |
| `083a:e501` | Accton Technology ZD1211B |
| `0b05:171b` | ASUSTeK Computer A9T wireless |
| `050d:705c` | Belkin Components F5D7050 v4000 Wireless Adapter |
| `050d:4050` | Belkin Components ZD1211B |
| `13b1:0024` | Cisco-Linksys WUSBF54G |
| `1582:6003` | Fiberline WL-430U |
| `0411:00da` | Melco WLI-U2-KG54L |
| `0471:1236` | Philips SNU5600 |
| `2019:5303` | Planex Communications GW-US54GXS WLAN |
| `079b:0062` | Sagem XG-76NA |
| `0df6:9075` | Sitecom Europe ZD1211B |
| `157e:300d` | U-MEDIA Communications TEW-429UB C1 |
| `0baf:0121` | U.S. Robotics USR5423 WLAN |
| `0f88:3014` | VTech ZD1211B |
| `0cde:001a` | Z-Com ZD1211B |
| `0ace:1215` | Zydas Technology Corporation ZD1211B |
| `0586:340a` | ZyXEL Communication M-202 |
| `0586:3410` | ZyXEL Communication G-202 |
| `0586:340f` | ZyXEL Communication G-220 v2 |

## Appendix B: Bluetooth device IDs

### Intel — `bt_firmware`

Also any other `8087:*` device with the Bluetooth class.

| ID | Device |
|---|---|
| `8087:07dc` | Wireless 7260 |
| `8087:0a2a` | Wireless 7265 / 3160 / 3165 |
| `8087:0aa7` | Wireless-AC 3168 |
| `8087:0a2b` | Wireless 8260 / 8265 |
| `8087:0aaa` | Wireless-AC 9460 / 9560 |
| `8087:0025` | Wireless-AC 9260 |
| `8087:0026` | Wi-Fi 6 AX201 |
| `8087:0029` | Wi-Fi 6 AX200 |
| `8087:0032` | Wi-Fi 6E AX210 (tested) |
| `8087:0033` | Wi-Fi 6E AX211 / AX411 |
| `8087:0035` | Wi-Fi 7 BE2xx |
| `8087:0036` | Wi-Fi 7 BE2xx |
| `8087:0037` | Wi-Fi 7 BE2xx |
| `8087:0038` | Wi-Fi 7 BE2xx |
| `8087:0039` | Wi-Fi 7 BE2xx |

### Realtek — `bt_firmware`

Also any other `0bda:*` device with the Bluetooth class.

| ID | Device |
|---|---|
| `0489:e085` | Foxconn / Hon Hai |
| `0489:e08b` | Foxconn / Hon Hai |
| `0489:e112` | Foxconn / Hon Hai |
| `0489:e122` | Foxconn / Hon Hai |
| `0489:e123` | Foxconn / Hon Hai |
| `0489:e125` | Foxconn / Hon Hai |
| `0489:e12f` | Foxconn / Hon Hai |
| `0489:e130` | Foxconn / Hon Hai |
| `04c5:161f` | Fujitsu, Ltd |
| `04c5:165c` | Fujitsu, Ltd |
| `04c5:1675` | Fujitsu, Ltd |
| `04ca:4005` | Lite-On Technology Corp. |
| `04ca:4006` | Lite-On Technology Corp. |
| `04ca:4007` | Lite-On Technology Corp. |
| `04f2:b49f` | Chicony Electronics Co., Ltd Bluetooth (RTL8723BE) |
| `0930:021d` | Toshiba Corp. |
| `0b05:17dc` | ASUSTek Computer, Inc. |
| `0b05:185c` | ASUSTek Computer, Inc. |
| `0b05:18ef` | ASUSTek Computer, Inc. |
| `0b05:190e` | ASUSTek Computer, Inc. |
| `0b05:1bef` | ASUSTek Computer, Inc. |
| `0b05:1d70` | ASUSTek Computer, Inc. |
| `0bda:2852` | Realtek Semiconductor Corp. |
| `0bda:385a` | Realtek Semiconductor Corp. |
| `0bda:4852` | Realtek Semiconductor Corp. |
| `0bda:4853` | Realtek Semiconductor Corp. |
| `0bda:8520` | Realtek Semiconductor Corp. |
| `0bda:8771` | Realtek Semiconductor Corp. |
| `0bda:887b` | Realtek Semiconductor Corp. |
| `0bda:8922` | Realtek Semiconductor Corp. |
| `0bda:a728` | Realtek Semiconductor Corp. |
| `0bda:b009` | Realtek Semiconductor Corp. Realtek Bluetooth 4.2 Adapter |
| `0bda:b00c` | Realtek Semiconductor Corp. |
| `0bda:b850` | Realtek Semiconductor Corp. |
| `0bda:b85b` | Realtek Semiconductor Corp. |
| `0bda:c123` | Realtek Semiconductor Corp. |
| `0bda:c822` | Realtek Semiconductor Corp. |
| `0bda:c852` | Realtek Semiconductor Corp. |
| `0cb5:c547` | Realtek-based module |
| `0cb8:c549` | Opticis Co., Ltd |
| `0cb8:c558` | Opticis Co., Ltd |
| `0cb8:c559` | Opticis Co., Ltd |
| `1358:c123` | Realtek-based module |
| `13d3:3394` | IMC Networks Bluetooth |
| `13d3:3410` | IMC Networks |
| `13d3:3414` | IMC Networks |
| `13d3:3416` | IMC Networks |
| `13d3:3458` | IMC Networks |
| `13d3:3459` | IMC Networks |
| `13d3:3461` | IMC Networks |
| `13d3:3462` | IMC Networks |
| `13d3:3494` | IMC Networks |
| `13d3:3526` | IMC Networks Bluetooth Radio |
| `13d3:3529` | IMC Networks |
| `13d3:3533` | IMC Networks |
| `13d3:3548` | IMC Networks |
| `13d3:3549` | IMC Networks |
| `13d3:3553` | IMC Networks |
| `13d3:3555` | IMC Networks |
| `13d3:3570` | IMC Networks |
| `13d3:3571` | IMC Networks |
| `13d3:3572` | IMC Networks |
| `13d3:3586` | IMC Networks |
| `13d3:3587` | IMC Networks |
| `13d3:3591` | IMC Networks |
| `13d3:3592` | IMC Networks |
| `13d3:3600` | IMC Networks |
| `13d3:3601` | IMC Networks |
| `13d3:3612` | IMC Networks |
| `13d3:3616` | IMC Networks |
| `13d3:3617` | IMC Networks |
| `13d3:3618` | IMC Networks |
| `2001:332a` | D-Link Corp. |
| `2357:0604` | TP-Link |
| `2550:8761` | Realtek-based module |
| `2b89:6275` | Realtek-based module |
| `2b89:8761` | Realtek-based module |
| `2c0a:8761` | Realtek-based module |
| `2c4e:0128` | Mercucys INC |
| `2ff8:3051` | Realtek-based module |
| `2ff8:b011` | Realtek-based module |
| `3625:010b` | Realtek-based module |
| `37ad:0600` | Realtek-based module |
| `6655:8771` | Realtek-based module |
| `7392:a611` | Edimax Technology Co., Ltd EW-7611ULB 802.11b/g/n and Bluetooth 4.0 Adapter |
| `7392:c611` | Edimax Technology Co., Ltd |
| `7392:e611` | Edimax Technology Co., Ltd |

### MediaTek MT7921/MT7922 — `h2generic`

| ID | Device |
|---|---|
| `0489:e0c8` | Foxconn / Hon Hai (MT7921) |
| `0489:e0cd` | Foxconn / Hon Hai (MT7921) |
| `0489:e0e0` | Foxconn / Hon Hai (MT7921) |
| `0489:e0f2` | Foxconn / Hon Hai (MT7921) |
| `04ca:3802` | Lite-On Technology Corp. (MT7921) |
| `0e8d:0608` | MediaTek Inc. (MT7921) |
| `13d3:3563` | IMC Networks (MT7921) |
| `13d3:3564` | IMC Networks (MT7921) |
| `13d3:3567` | IMC Networks (MT7921) |
| `13d3:3576` | IMC Networks (MT7921) |
| `13d3:3578` | IMC Networks (MT7921) |
| `13d3:3583` | IMC Networks (MT7921) |
| `13d3:3606` | IMC Networks (MT7921) |
| `13d3:3585` | IMC Networks (MT7922) |
| `13d3:3610` | IMC Networks (MT7922) |
| `0489:e0d8` | Foxconn / Hon Hai (MT7922A) |
| `0489:e0d9` | Foxconn / Hon Hai (MT7922A) |
| `0489:e0e2` | Foxconn / Hon Hai (MT7922A) |
| `0489:e0e4` | Foxconn / Hon Hai (MT7922A) |
| `0489:e0f1` | Foxconn / Hon Hai (MT7922A) |
| `0489:e0f5` | Foxconn / Hon Hai (MT7922A) |
| `0489:e0f6` | Foxconn / Hon Hai (MT7922A) |
| `0489:e102` | Foxconn / Hon Hai (MT7922A) |
| `0489:e152` | Foxconn / Hon Hai (MT7922A) |
| `0489:e153` | Foxconn / Hon Hai (MT7922A) |
| `0489:e170` | Foxconn / Hon Hai (MT7922A) |
| `04ca:3804` | Lite-On Technology Corp. (MT7922A) |
| `04ca:38e4` | Lite-On Technology Corp. (MT7922A) |
| `13d3:3568` | IMC Networks (MT7922A) |
| `13d3:3584` | IMC Networks (MT7922A) |
| `13d3:3605` | IMC Networks (MT7922A) |
| `13d3:3607` | IMC Networks (MT7922A) |
| `13d3:3614` | IMC Networks (MT7922A) |
| `13d3:3615` | IMC Networks (MT7922A) |
| `13d3:3633` | IMC Networks (MT7922A) |
| `35f5:7922` | (MT7922A) |
