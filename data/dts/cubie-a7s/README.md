# Radxa Cubie A7S device tree (air/OS)

`sun60i-a733.dtsi` and `sun60i-a733-cubie-a7s.dts` come from Radxa's U-Boot
(`radxa/u-boot`, branch `u-boot-dlan17`, commit 9da4b3e8), which boots the
board; the `include/dt-bindings` headers come from the same tree. All are
dual-licensed (GPL-2.0 or MIT/BSD).

air/OS adds what its drivers need below the "air/OS additions" marker of the
board file. Build with `tools/cubie-a7s/build-dtb.sh <out.dtb>`; U-Boot hands
`dtb/allwinner/sun60i-a733-cubie-a7s.dtb` on the ESP to the EFI loader.

Changes to the U-Boot files:

- Interrupt numbers. U-Boot does not use interrupts, and its A733 tree carried
  the A523 ones for the SD/MMC controllers, the USB controllers, the watchdog,
  SPI0, the NMI controller, the R_PIO and the RTC, plus the tenth PIO bank.
  They are replaced by the numbers of Radxa's BSP device tree (Linux 5.15/6.6),
  which work; Ethernet, the UARTs in use and the I2C controllers already
  matched.
- `reserved-memory` for the BSP TF-A (BL31) at 0x48000000, 16 MiB.
- CPU top speeds (`clock-frequency`, from the BSP's cpufreq) and the
  Cortex-A76 compatible of cpu6 and cpu7.
- Always-on regulators for the AIC8800 Wi-Fi/Bluetooth module (R_PIO PM0
  and PM1), switched by the sunxi_regulator driver.
