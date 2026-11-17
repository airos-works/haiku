/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 */

#ifndef R10B_REG_H
#define R10B_REG_H

/*
 * RTL8710B (RTL8188GU) registers that differ from the RTL8192C/RTL8188F
 * ones rtwn knows. Addresses and bits as the vendor driver and Linux's
 * rtl8xxxu (regs.h, 8710b.c) use them; nothing else is taken from there.
 *
 * Over USB the MAC registers 0x00..0xff are not at their usual place:
 * the host reaches them at 0x8000 + address (rtl8xxxu adds the offset in
 * its rtl8xxxu_read*()/rtl8xxxu_write*()). usb/r10bu_reg.c does that for
 * all of rtwn, so the addresses below are the plain ones.
 */
#define R10B_USB_LOW_REG_BASE		0x8000
#define R10B_USB_LOW_REG_END		0x0100

/* System function enable, 32-bit view (R92C_SYS_FUNC_EN is 0x02). */
#define R10B_SYS_FUNC			0x004
#define R10B_SYS_FUNC_BIT0		0x00000001
#define R10B_SYS_FUNC_BB		0x00030000	/* bits 17:16 */
#define R10B_SYS_FUNC_RF		0x07000000	/* bits 26:24 */

/* Power state machine (rtl8710b_emu_to_active(), _active_to_emu()). */
#define R10B_PWR_FSM			0x020
#define R10B_PWR_FSM_ON			0x01
#define R10B_PWR_FSM_OFF		0x02
#define R10B_AFE_CTRL			0x050
#define R10B_PWR_056			0x056	/* written 0x0e at power on */
#define R10B_WL_RF_PSS			0x05c
#define R10B_WL_RF_PSS_LDO		0x00000107	/* bits 8, 2:0 */
#define R10B_AFE_PWR_MODE		0x05d	/* bit 0: 1 LDO, 0 power cut */

/* Indirect access to the SYSON registers and the eFuse. */
#define R10B_INDIRECT_CTRL		0x06c
#define R10B_INDIRECT_CTRL_BUSY		0x80000000
#define R10B_INDIRECT_CMD_REG_READ	0x83000000
#define R10B_INDIRECT_CMD_REG_WRITE	0x84000000
#define R10B_INDIRECT_CMD_EFUSE_READ	0x85000000
#define R10B_INDIRECT_DATA		0x09c
#define R10B_INDIRECT_ADDR		0x0f8

/* Host interrupt mask/status (where R92C_MCUFWDL is on other chips). */
#define R10B_HIMR0			0x080
#define R10B_HISR0			0x084

/*
 * 8051 firmware control: what R92C_MCUFWDL (0x80) is on the other chips,
 * with the same bits 0-7 (download enable, checksum report, ready,
 * RAM select...) and page select in bits 18:16. Bit 8 must be set before
 * a download; bit 24 releases the MCU from reset.
 */
#define R10B_8051FW_CTRL		0x090
#define R10B_8051FW_CTRL_DL_PREP	0x00000100
#define R10B_8051FW_CTRL_ROM_DLEN	0x00080000
#define R10B_8051FW_CTRL_MCU_EN		0x01000000

#define R10B_WL_STATUS			0x0f0
#define R10B_WL_STATUS_BIT1		0x02

#define R10B_EARLY_MODE_CTRL		0x2bc
#define R10B_PORT_CTRL			0x76d
#define R10B_PORT_CTRL_PORT01_EN	0x30	/* 00: MP, 11: normal driver */

#define R10B_HCI_PWR_FEF9		0xfef9
#define R10B_USB_ACCESS_TIMEOUT		0xfe4c

/*
 * SYSON registers (through the indirect access above; the address is
 * ORed with R10B_SYSON_BASE).
 */
#define R10B_SYSON_BASE			0x40000000
#define R10B_SYSON_XTAL_CTRL0		0x060
#define R10B_SYSON_XTAL_CTRL0_XTAL1	0x3f000000	/* bits 29:24 */
#define R10B_SYSON_XTAL_CTRL0_XTAL0	0x00fc0000	/* bits 23:18 */
#define R10B_SYSON_SUSPEND_CTRL		0x138
#define R10B_SYSON_SUSPEND_CM4		0x00000020	/* allow CM4 suspend */
#define R10B_SYSON_EEPROM_CTRL0		0x0e0
#define R10B_SYSON_EEPROM_BOOT		0x00000010
#define R10B_SYSON_EEPROM_EN		0x00000020
#define R10B_SYSON_SYSTEM_CFG0		0x1f0
#define R10B_SYSON_SYSTEM_CFG0_CUT	0x0000000f
#define R10B_SYSON_SYSTEM_CFG0_VENDOR	0x000000c0
#define R10B_SYSON_SYSTEM_CFG0_TEST	0x00010000
#define R10B_SYSON_SYSTEM_CFG2		0x1f8

/* SYSTEM_CFG0 vendor field (not the R92C_SYS_CFG encoding). */
#define R10B_VENDOR_SMIC		0
#define R10B_VENDOR_TSMC		1
#define R10B_VENDOR_UMC			2

/* Physical eFuse: package type byte. */
#define R10B_EFUSE_PACKAGE_TYPE		0x0f8
#define R10B_PACKAGE_QFN48M_U		0xee
#define R10B_PACKAGE_QFN48M_S		0xfe

/*
 * Baseband registers rtwn has no name for (rtl8xxxu regs.h names in
 * comments).
 */
/* R92C_FPGA0_RFIFACESW(0): TRSW, TRSWB, ANTSW, ANTSWB, PAPE; BD: ANTSW(B), PAPE. */
#define R10B_RF_SW_CTRL			0x0760
#define R10B_RF_SW_CTRL_BD		0x0700
#define R10B_FPGA0_XB_RF_INT_OE		0x864
#define R10B_FPGA0_XCD_RF_SW_CTRL	0x874
#define R10B_NHM_TH9_TH10		0x890
#define R10B_NHM_TIMER			0x894
#define R10B_NHM_TH3_TO_TH0		0x898
#define R10B_NHM_TH7_TO_TH4		0x89c
#define R10B_S0S1_PATH_SWITCH		0x948
#define R10B_RX_DFIR			0x954	/* REG_OFDM_RX_DFIR */
#define R10B_CCK0_TX_FILTER1		0xa20
#define R10B_CCK0_TX_FILTER2		0xa24
#define R10B_CCK0_DEBUG_PORT		0xa28	/* and TX filter 3 */
#define R10B_CCK0_NEW_AGC		0xa9c	/* bit 17: new CCK AGC report */
#define R10B_CCK0_NEW_AGC_BIT		0x00020000
#define R10B_CCK0_TX_FILTER3		0xaac
#define R10B_OFDM0_FA_RSTC		0xc0c
#define R10B_OFDM0_XA_RX_AFE		0xc10
#define R10B_OFDM0_TX_PSDO_NOISE_WEIGHT	0xce4
#define R10B_WMAC_TRXPTCL_CTL		0x668
#define R10B_WMAC_TRXPTCL_CTL_BW_M	0x0180
#define R10B_WMAC_TRXPTCL_CTL_BW_40	0x0080
#define R10B_DATA_SUBCHANNEL		0x483

/* IQ calibration ADDA registers (REG_FPGA0_XCD_SWITCH_CTRL...). */
#define R10B_FPGA0_XCD_SWITCH_CTRL	0x85c
#define R10B_BLUETOOTH			0xe6c
#define R10B_RX_WAIT_CCA		0xe70
#define R10B_TX_CCK_RFON		0xe74
#define R10B_TX_CCK_BBON		0xe78
#define R10B_TX_OFDM_RFON		0xe7c
#define R10B_TX_OFDM_BBON		0xe80
#define R10B_TX_TO_RX			0xe84
#define R10B_TX_TO_TX			0xe88
#define R10B_RX_CCK			0xe8c
#define R10B_RX_OFDM			0xed0
#define R10B_RX_WAIT_RIFS		0xed4
#define R10B_RX_TO_RX			0xed8
#define R10B_STANDBY			0xedc
#define R10B_SLEEP			0xee0
#define R10B_PMPD_ANAEN			0xeec

/* MAC registers used with other meanings in rtwn's headers. */
#define R10B_TX_RPT_CTRL		0x4ec
#define R10B_TX_RPT_CTRL_TIMER_EN	0x02
#define R10B_TX_RPT_TIME		0x4f0
#define R10B_PKT_VO_VI_LIFE_TIME	0x4c0
#define R10B_PKT_BE_BK_LIFE_TIME	0x4c2
#define R10B_SCH_TX_CMD			0x5f8
#define R10B_RXDMA_PRO			0x290
#define R10B_RXDMA_PRO_DMA_MODE		0x02
#define R10B_RXDMA_PRO_BURST_CNT_M	0x0c
#define R10B_RXDMA_PRO_BURST_CNT_S	2
#define R10B_RXDMA_PRO_BURST_SIZE_M	0x30
#define R10B_RXDMA_PRO_BURST_SIZE_S	4
#define R10B_FWHW_TXQ_CTRL_XMIT_MGMT_ACK 0x00001000

/* RF registers. */
#define R10B_RF_CHNLBW_CHAN_M		0x003ff
#define R10B_RF_CHNLBW_BW_M		0x00c00
#define R10B_RF_CHNLBW_BW20		0x00c00
#define R10B_RF_CHNLBW_BW40		0x00400
#define R10B_RF_T_METER			0x42
#define R10B_RF_PAD_TXG			0x56
#define R10B_RF_S0S1			0xb0
#define R10B_RF_GAIN_CCA		0xdf
#define R10B_RF_WE_LUT			0xef

#endif	/* R10B_REG_H */
