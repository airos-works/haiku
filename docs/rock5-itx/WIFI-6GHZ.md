# Wi-Fi 6E (6 GHz) for iaxwifi200: what is missing (2026-11-11)

Design note for issue 63. Nothing here is implemented: the lab has no 6 GHz
access point that could be confirmed (scan results carry no 6 GHz
information today, and no lab machine could capture beacons), so only the
parts that can be verified on 2.4 and 5 GHz are worth landing ahead of one.

## The two questions the issue asks

**Can the join path do SAE? No.** `ifconfig join`, the Wi-Fi preferences and
`wifiautojoin` end in `IEEE80211_IOC_HAIKU_JOIN`
(`openbsd_wlan/net80211/ieee80211_haiku.cpp`), which hands net80211 a
pre-shared key and the AKMs 802.1X, PSK and their SHA-256 variants. net80211
runs the four-way handshake itself, in the kernel. It sends only Open System
authentication frames (`ieee80211_output.c`, `IEEE80211_AUTH_ALG_OPEN`) and
drops any other algorithm on receive (`ieee80211_input.c`). SAE exists only as
a value parsed out of the AP's RSN element (`IEEE80211_AKM_SAE`), so that a
scan can show it. There is no SAE commit/confirm exchange, no hash-to-element
(mandatory on 6 GHz), no OWE, and no way for a user-land supplicant to send
and receive authentication frames or to hand the kernel a PMK that SAE
produced. Management frame protection, which 6 GHz also requires, is there
(BIP, `IEEE80211_F_MFPR`).

This is the first prerequisite, and it is worth doing for its own sake: a
WPA3-only network on 2.4 or 5 GHz cannot be joined today either, so it can be
developed and tested without any 6 GHz hardware. The smaller route is
"external authentication": net80211 passes authentication frames with
algorithm 3 to wpa_supplicant (which has SAE and H2E) and takes the PMK back,
then runs its own four-way handshake with AKM 8. The larger one is SAE in the
kernel.

**What does the MCC response say about 6 GHz channels? Not known yet.** The
driver reads the firmware's channel profile for the 59 6 GHz channels
(`iwx_nvm_channels_uhb`) and throws it away. The branch `wifi-issues` logs the
count ("the regulatory data allows N of M 6 GHz channels"); it has not run on
a card. The driver sends no `LARI_CONFIG_CHANGE` and reads no ACPI `_DSM`
(Linux's "enable 6E" function and the UATS table), so the firmware's defaults
apply on every machine, PCs included. On FDT boards (ROCK 5) there is no ACPI
to ask; a driver setting would have to stand in for it.

## Channel model

`ic_channels[IEEE80211_CHAN_MAX + 1]` (256 entries) is indexed by channel
number, and `ieee80211_chan2ieee()` returns that index both as "the channel
number to put in a frame or a firmware command" and as "the bit in
`ic_chan_avail/active/scan`". 6 GHz numbers (1 to 233) collide with 2.4 and
5 GHz numbers.

Proposed: keep one table and grow it to 512 entries. 2.4 and 5 GHz channels
stay where they are; 6 GHz channel n lives at index 256 + n and carries a new
`IEEE80211_CHAN_6GHZ` flag (frequency 5950 + 5 n MHz, channel 2 at 5935).
`ieee80211_chan2ieee()` keeps returning the index, so the bitmaps and every
per-channel array keep working; a new `ieee80211_chan2num()` returns the
number for the wire. The work is the audit: about 35 uses in net80211 and 13
in `if_iwx.c` have to be sorted into "index" and "number" (DS parameter set,
HT/HE operation primary channel, PHY context and scan commands want the
number, together with the firmware's 6 GHz band value, which
`if_iwxreg.h` does not define yet). User land already receives
frequencies (`isr_freq`), so `ifconfig`, the preferences and the Deskbar
applet need the frequency-to-channel helper only. This part can land before a
6 GHz AP exists, checked by the 2.4/5 GHz scan and join results staying the
same.

## The rest, in order

1. Capabilities: the HE 6 GHz band capabilities element in association
   requests (there is no HT or VHT on 6 GHz), and the 6 GHz operation
   information in the HE operation element for the channel width
   (`ieee80211_setup_heop()` keeps only the parameters and the basic MCS set).
2. Scanning: 6 GHz APs are found through the Reduced Neighbor Report of their
   2.4/5 GHz radios; parse it, then fill the 6 GHz part of the UMAC scan
   command (`IWX_SCAN_CHANNEL_FLAG_6G_*`, short SSIDs, PSC channels). The
   firmware can also scan the PSC channels passively
   (`IWX_UCODE_TLV_CAPA_PASSIVE_6GHZ_SCAN`).
3. PHY context: `iwx_phy_ctxt_cmd_uhb_v3_v4()` exists; it needs the band and
   the width from the HE operation element. The 160 MHz work of the
   `wifi-issues` branch is shared with this.
4. Regulatory: the log line above first; `LARI_CONFIG_CHANGE` if the defaults
   turn out to forbid 6 GHz where the owner is.

Test needs: a Wi-Fi 6E or 7 access point, antennas on the ROCK 5, and the
AX210 (Framework 13) or BE200 (ROCK 5).
