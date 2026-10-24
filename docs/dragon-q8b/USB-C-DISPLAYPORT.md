# DisplayPort over the Dragon Q8B's USB-C ports

What a second (and third) display needs, what is already proven on the board without a monitor, and what is left. The HDMI output (DP2 through the CH7218A bridge) is the firmware's trained link and does not need any of this.

## The chain, per USB-C connector

| | Connector 0 | Connector 1 |
|---|---|---|
| DisplayPort controller (MDSS 0) | DP0, 0xae90000 | DP1, 0xae98000 |
| Combo PHY (QMP USB43DP) | 0x88eb000 | 0x8903000 |
| SBU mux (PI3USB102, `gpio-sbu-mux`) | enable GPIO 101 (active low), select GPIO 164 | enable GPIO 48 (active low), select GPIO 47 |
| Port reports | pmic_glink alternate mode, port 0 | port 1 |

1. **The port report** (`qcom_adsp`, owner USBC_PAN, `USBC_NOTIFY_IND`): SVID 0xff01 means DisplayPort; the pin assignment (C, E: four DP lanes; D: two DP lanes and USB 3), the orientation, HPD and the HPD IRQ. Each report is acknowledged (`ALTMODE_PAN_ACK`). Done; no report has arrived yet, because nothing was plugged in.
2. **The SBU mux**: enable, and select = reverse orientation; off when the port is not in DisplayPort mode (Linux' `gpio-sbu-mux.c`). Proven to switch (the GPIOs).
3. **The combo PHY** (Linux' `phy-qcom-qmp-combo.c`, `sc8280xp_usb43dpphy_cfg`, offsets v5: com 0x0000, txa 0x0400, txb 0x0a00, dp_serdes 0x2000, dp_dp_phy 0x2200): USB30_PRIM_GDSC, the aux/ref/com aux clocks, L9D and L4D, its resets; the common block in USB3+DP mode with the orientation in TYPEC_CTRL; the AUX block (`qmp_v4_dp_aux_init`); the DP serdes at the link rate, both TX blocks, `qmp_v456_configure_dp_phy` (lane power from the lane count and orientation), and the swing/pre-emphasis tables during training. Proven: the PLL locks at RBR and the PHY reports ready (`qcom_probe`'s `dp_phy_test()`). Its registers read back as their byte in all four lanes.
4. **dispcc 0**: DP0's link RCG (0x21a4) from the PHY's link clock (source 1), its pixel RCG (0x21d8) from the PHY's VCO divider clock (source 2) with M/N for the mode; the AUX RCG (0x2238) from XO; branches aux 0x206c, link 0x205c, link intf 0x2060, pixel 0x2070. Proven: the link branch runs from the PHY, the AUX branch from XO.
5. **The controller's AUX** (`msm_dp_aux`): reset, timeout count and limits 0xffff, enable; then the transfers `qcom_display`'s `DisplayPort` already does. Proven: a native DPCD read with nothing plugged in ends in an AUX timeout after 55 ms, as it should.
6. **Link training** (Linux' `dp_ctrl.c`): DPCD caps, then TPS1 with the swing/pre-emphasis the sink asks for until clock recovery, TPS2/3/4 until channel equalization, at the highest rate and lane count both sides support, lowering them on failure. Not written yet.
7. **The stream**: the controller's mainlink, the MSA and TU registers (as `DisplayPort::SetStream` does for DP2), and a second DPU path (SSPP, layer mixer, CTL, INTF for DP0) on its own frame buffer. Not written yet.
8. **app_server**: the accelerant publishing a second head, so the Screens preferences place it. Not written yet.

Steps 6 to 8 need a USB-C (or USB-C to DisplayPort/HDMI) monitor to be tested, and the port reports of step 1 to say when to start.
