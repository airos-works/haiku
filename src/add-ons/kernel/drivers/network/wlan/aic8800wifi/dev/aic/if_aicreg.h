/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _IF_AICREG_H_
#define _IF_AICREG_H_


/*	The AIC8800D80 FullMAC firmware's messages and frames, as far as a
	station uses them. Everything is little endian except the two Ethernet
	types, which are in network order. The layouts are byte offsets (from
	cubie/evidence/wifi/fdrv-protocol.md section 2.7, which measured them on
	the vendor's headers); the driver writes and reads them byte by byte. */


/* tasks */
#define AIC_TASK_MM					0
#define AIC_TASK_DBG				1
#define AIC_TASK_SCANU				4
#define AIC_TASK_ME					5
#define AIC_TASK_SM					6

/* MM: the MAC */
#define AIC_MM_RESET_REQ			0x0000
#define AIC_MM_RESET_CFM			0x0001
#define AIC_MM_START_REQ			0x0002
#define AIC_MM_START_CFM			0x0003
#define AIC_MM_VERSION_REQ			0x0004
#define AIC_MM_VERSION_CFM			0x0005
#define AIC_MM_ADD_IF_REQ			0x0006
#define AIC_MM_ADD_IF_CFM			0x0007
#define AIC_MM_REMOVE_IF_REQ		0x0008
#define AIC_MM_REMOVE_IF_CFM		0x0009
#define AIC_MM_KEY_ADD_REQ			0x0024
#define AIC_MM_KEY_ADD_CFM			0x0025
#define AIC_MM_KEY_DEL_REQ			0x0026
#define AIC_MM_KEY_DEL_CFM			0x0027
#define AIC_MM_SET_COEX_REQ			0x0065
#define AIC_MM_SET_COEX_CFM			0x0066
#define AIC_MM_SET_RF_CALIB_REQ		0x0069
#define AIC_MM_SET_RF_CALIB_CFM		0x006a
#define AIC_MM_GET_MAC_ADDR_REQ		0x0073
#define AIC_MM_GET_MAC_ADDR_CFM		0x0074
#define AIC_MM_GET_STA_INFO_REQ		0x0075
#define AIC_MM_GET_STA_INFO_CFM		0x0076
#define AIC_MM_SET_TXPWR_LVL_REQ	0x0077
#define AIC_MM_SET_TXPWR_LVL_CFM	0x0078
#define AIC_MM_SET_STACK_START_REQ	0x007b
#define AIC_MM_SET_STACK_START_CFM	0x007c
#define AIC_MM_GET_FW_VERSION_REQ	0x0080
#define AIC_MM_GET_FW_VERSION_CFM	0x0081
#define AIC_MM_FW_PANIC_IND			0x0095
#define AIC_MM_FW_ASSERT_IND		0x0096

/* DBG */
#define AIC_DBG_MEM_READ_REQ		0x0400
#define AIC_DBG_MEM_READ_CFM		0x0401
#define AIC_DBG_ERROR_IND			0x0408

/* SCANU: scanning */
#define AIC_SCANU_START_REQ			0x1000
#define AIC_SCANU_START_CFM			0x1001	/* the scan is over */
#define AIC_SCANU_RESULT_IND		0x1004
#define AIC_SCANU_ACCEPTED_CFM		0x1009	/* the scan was accepted */
#define AIC_SCANU_CANCEL_REQ		0x100a
#define AIC_SCANU_CANCEL_CFM		0x100b

/* ME: station management */
#define AIC_ME_CONFIG_REQ			0x1400
#define AIC_ME_CONFIG_CFM			0x1401
#define AIC_ME_CHAN_CONFIG_REQ		0x1402
#define AIC_ME_CHAN_CONFIG_CFM		0x1403
#define AIC_ME_SET_CONTROL_PORT_REQ	0x1404
#define AIC_ME_SET_CONTROL_PORT_CFM	0x1405
#define AIC_ME_TKIP_MIC_FAILURE_IND	0x1406
#define AIC_ME_TX_CREDITS_UPDATE_IND 0x140b
#define AIC_ME_SET_PS_MODE_REQ		0x1413
#define AIC_ME_SET_PS_MODE_CFM		0x1414

/* SM: connections */
#define AIC_SM_CONNECT_REQ			0x1800
#define AIC_SM_CONNECT_CFM			0x1801
#define AIC_SM_CONNECT_IND			0x1802
#define AIC_SM_DISCONNECT_REQ		0x1803
#define AIC_SM_DISCONNECT_CFM		0x1804
#define AIC_SM_DISCONNECT_IND		0x1805

/* MM_SET_STACK_START_REQ: 4 bytes */
#define AIC_STACK_START_SIZE		4

/* MM_SET_RF_CALIB_REQ: five words, two bytes (24 with the padding) */
#define AIC_RF_CALIB_SIZE			24

/* MM_SET_TXPWR_LVL_REQ: the "v3" table inside a 95-byte union */
#define AIC_TXPWR_LVL_SIZE			95
#define AIC_TXPWR_ENABLE			0
#define AIC_TXPWR_11B_11AG_2G4		1	/* 12: 1, 2, 5.5, 11, 6 ... 54 Mb/s */
#define AIC_TXPWR_11N_11AC_2G4		13	/* 10: MCS 0 to 9 */
#define AIC_TXPWR_11AX_2G4			23	/* 12: MCS 0 to 11 */
#define AIC_TXPWR_11A_5G			35	/* 12: four unused, 6 ... 54 Mb/s */
#define AIC_TXPWR_11N_11AC_5G		47	/* 10 */
#define AIC_TXPWR_11AX_5G			57	/* 12 */
#define AIC_TXPWR_ENTRIES			69

/* MM_VERSION_CFM */
#define AIC_VERSION_SIZE			28
#define AIC_VERSION_LMAC			0
#define AIC_VERSION_PHY_1			12	/* the PHY's features: bits 24-25 the
										   widest channel (2: 80 MHz), 27
										   LDPC decoding, 28 beamformee */
#define AIC_VERSION_PHY_2			16
#define AIC_VERSION_FEATURES		20
#define AIC_VERSION_MAX_STA			24

/* ME_CONFIG_REQ: 112 bytes */
#define AIC_ME_CONFIG_SIZE			112
#define AIC_ME_HT_CAPA_INFO			0	/* u16 */
#define AIC_ME_HT_AMPDU_PARAM		2
#define AIC_ME_HT_MCS				3	/* 16 bytes: rx mask, highest, tx */
#define AIC_ME_VHT_CAPA_INFO		32	/* u32 */
#define AIC_ME_VHT_RX_MCS_MAP		36	/* u16 */
#define AIC_ME_VHT_RX_HIGHEST		38
#define AIC_ME_VHT_TX_MCS_MAP		40
#define AIC_ME_VHT_TX_HIGHEST		42
#define AIC_ME_TX_LIFETIME			100	/* u16 */
#define AIC_ME_PHY_BW_MAX			102
#define AIC_ME_HT_SUPPORTED			103
#define AIC_ME_VHT_SUPPORTED		104
#define AIC_ME_HE_SUPPORTED			105
#define AIC_ME_HE_UL_ON				106
#define AIC_ME_PS_ON				107
#define AIC_ME_ANT_DIV_ON			108
#define AIC_ME_DPSM					109

/* a channel {u16 frequency, u8 band, u8 flags, s8 power, pad} */
#define AIC_CHAN_SIZE				6
#define AIC_BAND_2GHZ				0
#define AIC_BAND_5GHZ				1

/* ME_CHAN_CONFIG_REQ: 254 bytes */
#define AIC_CHAN_CONFIG_SIZE		254
#define AIC_CHAN_CONFIG_2G4			0	/* 14 channels */
#define AIC_CHAN_CONFIG_5G			84	/* 28 channels */
#define AIC_CHAN_CONFIG_2G4_COUNT	252
#define AIC_CHAN_CONFIG_5G_COUNT	253
#define AIC_CHAN_MAX_2G4			14
#define AIC_CHAN_MAX_5G				28

/* MM_START_REQ: 16 PHY words, the U-APSD timeout, the clock accuracy */
#define AIC_START_SIZE				72
#define AIC_START_UAPSD_TIMEOUT		64
#define AIC_START_LP_CLK_ACCURACY	68

/* MM_SET_COEX_REQ */
#define AIC_COEX_SIZE				16

/* MM_ADD_IF_REQ: {type, pad, address[6], p2p}; CFM {status, index} */
#define AIC_ADD_IF_SIZE				10
#define AIC_IF_TYPE_STA				0

/* SCANU_START_REQ: 376 bytes */
#define AIC_SCAN_SIZE				376
#define AIC_SCAN_CHANNELS			0	/* 42 */
#define AIC_SCAN_SSIDS				252	/* 3 x {length, 32 bytes} */
#define AIC_SCAN_BSSID				352
#define AIC_SCAN_VIF				366
#define AIC_SCAN_CHANNEL_COUNT		367
#define AIC_SCAN_SSID_COUNT			368
#define AIC_SCAN_NO_CCK				369
#define AIC_SCAN_DURATION			372
#define AIC_SCAN_MAX_CHANNELS		42
#define AIC_SSID_SIZE				33

/* SCANU_RESULT_IND */
#define AIC_RESULT_LENGTH			0
#define AIC_RESULT_FREQUENCY		4
#define AIC_RESULT_BAND				6
#define AIC_RESULT_RSSI				9
#define AIC_RESULT_FRAME			12

/* SM_CONNECT_REQ: 320 bytes */
#define AIC_CONNECT_SIZE			320
#define AIC_CONNECT_SSID			0
#define AIC_CONNECT_BSSID			34
#define AIC_CONNECT_CHANNEL			40
#define AIC_CONNECT_FLAGS			48	/* u32 */
#define AIC_CONNECT_ETHERTYPE		52	/* network order */
#define AIC_CONNECT_IE_LENGTH		54
#define AIC_CONNECT_LISTEN			56
#define AIC_CONNECT_DONT_WAIT_BCMC	58
#define AIC_CONNECT_AUTH_TYPE		59
#define AIC_CONNECT_UAPSD			60
#define AIC_CONNECT_VIF				61
#define AIC_CONNECT_IE				64	/* 256 bytes */
#define AIC_CONNECT_IE_MAX			256

#define AIC_CONTROL_PORT_HOST		0x01
#define AIC_CONTROL_PORT_NO_ENC		0x02
#define AIC_DISABLE_HT				0x04
#define AIC_WPA_WPA2_IN_USE			0x08
#define AIC_MFP_IN_USE				0x10

/* SM_CONNECT_IND */
#define AIC_CONNECTED_STATUS		0	/* u16, an 802.11 status code */
#define AIC_CONNECTED_BSSID			2
#define AIC_CONNECTED_ROAMED		8
#define AIC_CONNECTED_VIF			9
#define AIC_CONNECTED_AP			10
#define AIC_CONNECTED_QOS			12
#define AIC_CONNECTED_AID			820	/* u16 */
#define AIC_CONNECTED_FREQUENCY		824	/* u16 */
#define AIC_CONNECTED_MIN			826
#define AIC_CONNECTED_WIDTH			826	/* 0 20 MHz, 1 40, 2 80, 3 160, 4 80+80 */

/* SM_DISCONNECT_REQ {u16 reason, vif}, SM_DISCONNECT_IND {u16 reason, vif} */
#define AIC_DISCONNECT_SIZE			4

/* MM_KEY_ADD_REQ: 44 bytes; CFM {status, hardware index} */
#define AIC_KEY_ADD_SIZE			44
#define AIC_KEY_INDEX				0
#define AIC_KEY_STA					1
#define AIC_KEY_LENGTH				4
#define AIC_KEY_DATA				8	/* 32 bytes */
#define AIC_KEY_CIPHER				40
#define AIC_KEY_VIF					41
#define AIC_KEY_SPP					42
#define AIC_KEY_PAIRWISE			43

#define AIC_CIPHER_WEP40			0
#define AIC_CIPHER_TKIP				1
#define AIC_CIPHER_CCMP				2
#define AIC_CIPHER_WEP104			3
#define AIC_CIPHER_BIP				5

/* MM_GET_STA_INFO_REQ {sta, "sta"}; CFM: 32 bytes */
#define AIC_STA_INFO_RATE			0	/* u32 */
#define AIC_STA_INFO_TX_FAILED		4
#define AIC_STA_INFO_RSSI			8

/* A frame for the firmware: a 4-byte bus header (the length of the whole
   transfer, type 0x01, 0), the 28-byte host descriptor, then the Ethernet
   frame without its 14-byte header. */
#define AIC_TX_HEADER				32
#define AIC_TX_LENGTH				4	/* u16 */
#define AIC_TX_STATUS_ADDRESS		8	/* u32: (1 << 31) | index */
#define AIC_TX_DESTINATION			12
#define AIC_TX_SOURCE				18
#define AIC_TX_ETHERTYPE			24	/* network order */
#define AIC_TX_AC					26
#define AIC_TX_TID					27
#define AIC_TX_VIF					28
#define AIC_TX_STA					29
#define AIC_TX_FLAGS				30	/* u16 */
#define AIC_TX_NEED_CONFIRM			(1U << 31)
#define AIC_TX_MGMT					(1 << 3)	/* flags: an 802.11 frame */
#define AIC_TX_MGMT_NO_CCK			(1 << 4)

#define AIC_AC_BK					0
#define AIC_AC_BE					1
#define AIC_AC_VI					2
#define AIC_AC_VO					3

/* A received frame: the 56-byte receive header, 4 bytes, the 802.11
   frame. */
#define AIC_RX_HEADER				60
#define AIC_RX_LENGTH				0	/* u16 */
#define AIC_RX_RSSI_LEGACY			14	/* s8 */
#define AIC_RX_RSSI_1				17	/* s8 */
#define AIC_RX_STATUS				36	/* u32 */
#define AIC_RX_DECRYPT(status)		(((status) >> 2) & 0x7)
#define AIC_RX_FREQUENCY			42	/* u16 */
#define AIC_RX_FLAGS				48	/* u32 */
#define AIC_RX_IS_AMSDU				(1 << 0)
#define AIC_RX_IS_80211_MPDU		(1 << 1)
#define AIC_RX_NEED_REORDER			(1 << 5)
#define AIC_RX_UPLOAD				(1 << 6)
#define AIC_RX_STA(flags)			(((flags) >> 16) & 0xff)

#define AIC_DECRYPT_NONE			0
#define AIC_DECRYPT_WEP				1
#define AIC_DECRYPT_TKIP			2
#define AIC_DECRYPT_CCMP128			3
#define AIC_DECRYPT_CCMP256			4
#define AIC_DECRYPT_GCMP128			5
#define AIC_DECRYPT_GCMP256			6
#define AIC_DECRYPT_WAPI			7


#endif	/* _IF_AICREG_H_ */
