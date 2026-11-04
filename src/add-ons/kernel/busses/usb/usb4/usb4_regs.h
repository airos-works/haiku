/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * USB4 / Thunderbolt host interface and router registers, as described in
 * the USB4 specification (chapters 8, 10, 11 and 12).
 */
#ifndef USB4_REGS_H
#define USB4_REGS_H


#include <SupportDefs.h>


// #pragma mark - Host interface (NHI) registers in BAR 0


#define NHI_TX_RING_BASE			0x00000
	// 16 bytes per ring: descriptor base (64 bit), producer/consumer, size
#define NHI_RX_RING_BASE			0x08000
#define NHI_TX_OPTIONS_BASE			0x19800
	// 32 bytes per ring
#define NHI_RX_OPTIONS_BASE			0x29800
#define NHI_RING_NOTIFY_BASE		0x37800
	// TX, RX and RX overflow status bits, cleared when read
#define NHI_RING_INT_CLEAR			0x37808
#define NHI_RING_INTERRUPT_BASE		0x38200
	// interrupt enable bits: TX rings, then RX rings
#define NHI_RING_INTERRUPT_MASK_CLEAR_BASE	0x38208
#define NHI_INT_THROTTLING_RATE		0x38c00
#define NHI_INT_VEC_ALLOC_BASE		0x38c40
#define NHI_CAPS					0x39640
#define NHI_CAPS_HOP_COUNT_MASK		0x3ff
#define NHI_CAPS_VERSION(x)			(((x) >> 16) & 0xff)
#define NHI_CAPS_VERSION_2			0x40
#define NHI_DMA_MISC				0x39864
#define NHI_DMA_MISC_INT_AUTO_CLEAR	(1u << 2)
#define NHI_DMA_MISC_DISABLE_AUTO_CLEAR	(1u << 17)
#define NHI_RESET					0x39898
#define NHI_RESET_HRR				(1u << 0)
#define NHI_INMAIL_DATA				0x39900
#define NHI_INMAIL_CMD				0x39904
#define NHI_OUTMAIL_CMD				0x3990c
#define NHI_FW_STS					0x39944

#define NHI_MSIX_VECTORS			16

// ring options, dword 0
#define RING_FLAG_E2E_FLOW_CONTROL	(1u << 28)
#define RING_FLAG_NO_SNOOP			(1u << 29)
#define RING_FLAG_RAW				(1u << 30)
	// frames keep their CRC, start/end of frame masks are ignored
#define RING_FLAG_ENABLE			(1u << 31)

// descriptor flags (bits 31:20 of the descriptor's third dword)
#define RING_DESC_ISOCH				0x1
#define RING_DESC_CRC_ERROR			0x1
#define RING_DESC_COMPLETED			0x2
#define RING_DESC_POSTED			0x4
#define RING_DESC_BUFFER_OVERRUN	0x4
#define RING_DESC_INTERRUPT			0x8

struct nhi_ring_desc {
	uint64	address;
	uint32	control;
		// length 11:0, end of frame 15:12, start of frame 19:16, flags 31:20
	uint32	time;
} _PACKED;

#define RING_DESC_LENGTH(x)			((x) & 0xfff)
#define RING_DESC_EOF(x)			(((x) >> 12) & 0xf)
#define RING_DESC_SOF(x)			(((x) >> 16) & 0xf)
#define RING_DESC_FLAGS(x)			(((x) >> 20) & 0xfff)
#define RING_DESC_MAKE(length, eof, sof, flags) \
	(((length) & 0xfff) | (((eof) & 0xf) << 12) | (((sof) & 0xf) << 16) \
		| (((flags) & 0xfff) << 20))

#define CTL_FRAME_SIZE				256


// #pragma mark - Control packets


enum {
	USB4_PDF_READ			= 1,
	USB4_PDF_WRITE			= 2,
	USB4_PDF_ERROR			= 3,
	USB4_PDF_NOTIFY_ACK		= 4,
	USB4_PDF_EVENT			= 5,
	USB4_PDF_XDOMAIN_REQ	= 6,
	USB4_PDF_XDOMAIN_RESP	= 7,
	USB4_PDF_OVERRIDE		= 8,
	USB4_PDF_RESET			= 9,
	USB4_PDF_ICM_EVENT		= 10,
	USB4_PDF_ICM_CMD		= 11,
	USB4_PDF_ICM_RESP		= 12,
};

enum usb4_config_space {
	USB4_CFG_HOPS		= 0,
	USB4_CFG_ADAPTER	= 1,
	USB4_CFG_ROUTER		= 2,
	USB4_CFG_COUNTERS	= 3,
};

enum {
	USB4_ERROR_PORT_NOT_CONNECTED	= 0,
	USB4_ERROR_LINK_ERROR			= 1,
	USB4_ERROR_INVALID_CONFIG_SPACE	= 2,
	USB4_ERROR_NO_SUCH_PORT			= 4,
	USB4_ERROR_ACK_PLUG_EVENT		= 7,
	USB4_ERROR_LOOP					= 8,
	USB4_ERROR_HEC_ERROR_DETECTED	= 12,
	USB4_ERROR_FLOW_CONTROL_ERROR	= 13,
	USB4_ERROR_LOCK					= 15,
	USB4_ERROR_DP_BW				= 32,
	USB4_ERROR_ROP_CMPLT			= 33,
	USB4_ERROR_POP_CMPLT			= 34,
	USB4_ERROR_PCIE_WAKE			= 35,
	USB4_ERROR_DP_CON_CHANGE		= 36,
	USB4_ERROR_DPTX_DISCOVERY		= 37,
	USB4_ERROR_LINK_RECOVERY		= 38,
	USB4_ERROR_ASYM_LINK			= 39,
};

#define USB4_ERROR_PG_HOT_PLUG		0x2
#define USB4_ERROR_PG_HOT_UNPLUG	0x3

// first header dword: route bits 53:32, bit 31 set in responses
#define USB4_HEADER_ROUTE_HI_MASK	0x3fffff
#define USB4_HEADER_RESPONSE		(1u << 31)

// read/write address dword
#define USB4_ADDR_MAKE(offset, length, port, space, seq) \
	(((offset) & 0x1fff) | (((length) & 0x3f) << 13) | (((port) & 0x3f) << 19) \
		| (((space) & 0x3) << 25) | (((seq) & 0x3) << 27))
#define USB4_ADDR_OFFSET(x)			((x) & 0x1fff)
#define USB4_ADDR_LENGTH(x)			(((x) >> 13) & 0x3f)
#define USB4_ADDR_PORT(x)			(((x) >> 19) & 0x3f)
#define USB4_ADDR_SPACE(x)			(((x) >> 25) & 0x3)
#define USB4_ADDR_SEQ(x)			(((x) >> 27) & 0x3)

// error packet dword 2
#define USB4_ERROR_CODE(x)			((x) & 0xff)
#define USB4_ERROR_PORT(x)			(((x) >> 8) & 0x3f)
#define USB4_ERROR_PG(x)			(((x) >> 30) & 0x3)
#define USB4_ERROR_MAKE(code, port, pg) \
	(((code) & 0xff) | (((port) & 0x3f) << 8) | (((pg) & 0x3) << 30))

// hot plug event dword 2
#define USB4_EVENT_PORT(x)			((x) & 0x3f)
#define USB4_EVENT_UNPLUG			(1u << 31)

#define USB4_MAX_CONFIG_DWORDS		60


// #pragma mark - Router configuration space


#define ROUTER_CS_0					0x00
	// vendor 15:0, device 31:16
#define ROUTER_CS_1					0x01
#define ROUTER_CS_1_NEXT_CAP(x)		((x) & 0xff)
#define ROUTER_CS_1_UPSTREAM(x)		(((x) >> 8) & 0x3f)
#define ROUTER_CS_1_MAX_ADAPTER(x)	(((x) >> 14) & 0x3f)
#define ROUTER_CS_1_DEPTH(x)		(((x) >> 20) & 0x7)
#define ROUTER_CS_1_REVISION(x)		(((x) >> 24) & 0xff)
#define ROUTER_CS_2					0x02
	// route string, bits 31:0
#define ROUTER_CS_3					0x03
	// route string bits 54:32 in 30:0, valid 31
#define ROUTER_CS_3_V				(1u << 31)
#define ROUTER_CS_4					0x04
#define ROUTER_CS_4_NOTIFICATION_TIMEOUT(x)	((x) & 0xff)
#define ROUTER_CS_4_CMUV(x)			(((x) >> 8) & 0xff)
#define ROUTER_CS_4_USB4_VERSION(x)	(((x) >> 24) & 0xff)
#define ROUTER_CS_4_CMUV_V1			0x10
#define ROUTER_CS_4_CMUV_V2			0x20
#define ROUTER_CS_5					0x05
#define ROUTER_CS_5_SLP				(1u << 0)
#define ROUTER_CS_5_WOP				(1u << 1)
#define ROUTER_CS_5_WOU				(1u << 2)
#define ROUTER_CS_5_WOD				(1u << 3)
#define ROUTER_CS_5_CNS				(1u << 23)
#define ROUTER_CS_5_PTO				(1u << 24)
#define ROUTER_CS_5_UTO				(1u << 25)
#define ROUTER_CS_5_HCO				(1u << 26)
#define ROUTER_CS_5_CV				(1u << 31)
#define ROUTER_CS_6					0x06
#define ROUTER_CS_6_SLPR			(1u << 0)
#define ROUTER_CS_6_TNS				(1u << 1)
#define ROUTER_CS_6_WOPS			(1u << 2)
#define ROUTER_CS_6_WOUS			(1u << 3)
#define ROUTER_CS_6_HCI				(1u << 18)
#define ROUTER_CS_6_CR				(1u << 25)
#define ROUTER_CS_7					0x07
	// unique id, 64 bits
#define ROUTER_CS_9					0x09
	// router operation data, 16 dwords
#define ROUTER_CS_25				0x19
	// router operation metadata
#define ROUTER_CS_26				0x1a
#define ROUTER_CS_26_OPCODE_MASK	0xffff
#define ROUTER_CS_26_STATUS(x)		(((x) >> 24) & 0x3f)
#define ROUTER_CS_26_ONS			(1u << 30)
#define ROUTER_CS_26_OV				(1u << 31)

#define USB4_VERSION_MAJOR(x)		(((x) >> 5) & 0x7)

enum {
	USB4_ROUTER_OP_QUERY_DP_RESOURCE	= 0x10,
	USB4_ROUTER_OP_ALLOC_DP_RESOURCE	= 0x11,
	USB4_ROUTER_OP_DEALLOC_DP_RESOURCE	= 0x12,
	USB4_ROUTER_OP_NVM_WRITE			= 0x20,
	USB4_ROUTER_OP_NVM_AUTH				= 0x21,
	USB4_ROUTER_OP_NVM_READ				= 0x22,
	USB4_ROUTER_OP_NVM_SET_OFFSET		= 0x23,
	USB4_ROUTER_OP_DROM_READ			= 0x24,
	USB4_ROUTER_OP_NVM_SECTOR_SIZE		= 0x25,
	USB4_ROUTER_OP_BUFFER_ALLOC			= 0x33,
};

// buffer allocation parameters
#define USB4_BA_LENGTH_MASK			0xff
#define USB4_BA_INDEX(x)			((x) & 0xffff)
#define USB4_BA_VALUE(x)			(((x) >> 16) & 0xffff)
enum {
	USB4_BA_MAX_USB3	= 0x1,
	USB4_BA_MIN_DP_AUX	= 0x2,
	USB4_BA_MIN_DP_MAIN	= 0x3,
	USB4_BA_MAX_PCIE	= 0x4,
	USB4_BA_MAX_HI		= 0x5,
};

// DROM read metadata
#define USB4_DROM_ADDRESS(x)		(((x) & 0x1fff) << 2)
#define USB4_DROM_SIZE(x)			(((x) & 0x1f) << 15)

// router capabilities
#define USB4_ROUTER_CAP_TMU			0x03
#define USB4_ROUTER_CAP_VSE			0x05
#define USB4_VSE_CAP_PLUG_EVENTS	0x01
#define USB4_VSE_CAP_TIME2			0x03
#define USB4_VSE_CAP_CP_LP			0x04
#define USB4_VSE_CAP_LINK_CONTROLLER	0x06

// router TMU capability
#define TMU_RTR_CS_0				0x00
#define TMU_RTR_CS_0_FREQ_WIND_MASK	(0x7ffu << 16)
#define TMU_RTR_CS_0_TD				(1u << 27)
#define TMU_RTR_CS_0_UCAP			(1u << 30)
#define TMU_RTR_CS_1				0x01
#define TMU_RTR_CS_1_LOCAL_TIME_NS(x)	(((x) >> 16) & 0xffff)
#define TMU_RTR_CS_2				0x02
#define TMU_RTR_CS_3				0x03
#define TMU_RTR_CS_3_LOCAL_TIME_NS(x)	((x) & 0xffff)
#define TMU_RTR_CS_3_TS_PACKET_INTERVAL_SHIFT	16
#define TMU_RTR_CS_3_TS_PACKET_INTERVAL_MASK	(0xffffu << 16)
#define TMU_RTR_CS_15				0x0f
#define TMU_RTR_CS_22				0x16
#define TMU_RTR_CS_24				0x18
#define TMU_RTR_CS_25				0x19

#define TMU_RATE_OFF				0
#define TMU_RATE_LOWRES				1000
#define TMU_RATE_HIFI				16


// #pragma mark - Adapter configuration space


#define ADP_CS_0					0x00
#define ADP_CS_1					0x01
#define ADP_CS_1_NEXT_CAP(x)		((x) & 0xff)
#define ADP_CS_2					0x02
#define ADP_CS_2_TYPE(x)			((x) & 0xffffff)
#define ADP_CS_3					0x03
#define ADP_CS_3_NUMBER(x)			(((x) >> 20) & 0x3f)
#define ADP_CS_4					0x04
#define ADP_CS_4_NFC_BUFFERS_MASK	0x3ff
#define ADP_CS_4_TOTAL_BUFFERS(x)	(((x) >> 20) & 0x3ff)
#define ADP_CS_4_LCK				(1u << 31)
#define ADP_CS_5					0x05
#define ADP_CS_5_MAX_IN_HOP(x)		((x) & 0x7ff)
#define ADP_CS_5_MAX_OUT_HOP(x)		(((x) >> 11) & 0x7ff)
#define ADP_CS_5_DHP				(1u << 31)

enum usb4_adapter_type {
	ADAPTER_INACTIVE		= 0x000000,
	ADAPTER_LANE			= 0x000001,
	ADAPTER_NHI				= 0x000002,
	ADAPTER_DP_IN			= 0x0e0101,
	ADAPTER_DP_OUT			= 0x0e0102,
	ADAPTER_PCIE_DOWN		= 0x100101,
	ADAPTER_PCIE_UP			= 0x100102,
	ADAPTER_USB3_DOWN		= 0x200101,
	ADAPTER_USB3_UP			= 0x200102,
};

// adapter capabilities
#define USB4_ADP_CAP_LANE			0x01
#define USB4_ADP_CAP_POWER			0x02
#define USB4_ADP_CAP_TMU			0x03
#define USB4_ADP_CAP_ADAPTER		0x04
#define USB4_ADP_CAP_VSE			0x05
#define USB4_ADP_CAP_USB4			0x06

// lane adapter capability
#define LANE_ADP_CS_0				0x00
#define LANE_ADP_CS_0_SUPPORTED_SPEED(x)	(((x) >> 16) & 0xf)
#define LANE_ADP_CS_0_SUPPORTED_WIDTH(x)	(((x) >> 20) & 0x3f)
#define LANE_ADP_CS_0_CL0S			(1u << 26)
#define LANE_ADP_CS_0_CL1			(1u << 27)
#define LANE_ADP_CS_0_CL2			(1u << 28)
#define LANE_ADP_CS_1				0x01
#define LANE_ADP_CS_1_TARGET_WIDTH_MASK	(0x3u << 4)
#define LANE_ADP_CS_1_TARGET_WIDTH_SINGLE	(0x1u << 4)
#define LANE_ADP_CS_1_TARGET_WIDTH_DUAL	(0x3u << 4)
#define LANE_ADP_CS_1_LD			(1u << 14)
#define LANE_ADP_CS_1_LB			(1u << 15)
#define LANE_ADP_CS_1_CURRENT_SPEED(x)	(((x) >> 16) & 0xf)
#define LANE_ADP_CS_1_CURRENT_WIDTH(x)	(((x) >> 20) & 0x3f)
#define LANE_ADP_CS_1_ADAPTER_STATE(x)	(((x) >> 26) & 0xf)
#define LANE_ADP_CS_1_PMS			(1u << 30)

#define LANE_SPEED_GEN2				0x8
#define LANE_SPEED_GEN3				0x4
#define LANE_SPEED_GEN4				0x2

#define LINK_WIDTH_SINGLE			0x1
#define LINK_WIDTH_DUAL				0x2
#define LINK_WIDTH_ASYM_TX			0x4
#define LINK_WIDTH_ASYM_RX			0x8

enum {
	ADAPTER_STATE_DISABLED	= 0,
	ADAPTER_STATE_TRAINING	= 1,
	ADAPTER_STATE_CL0		= 2,
	ADAPTER_STATE_CL0S_TX	= 3,
	ADAPTER_STATE_CL0S_RX	= 4,
	ADAPTER_STATE_CL1		= 5,
	ADAPTER_STATE_CL2		= 6,
	ADAPTER_STATE_CLD		= 7,
};

// USB4 port capability
#define PORT_CS_18					0x12
#define PORT_CS_18_BE				(1u << 8)
#define PORT_CS_18_TCM				(1u << 9)
#define PORT_CS_18_CPS				(1u << 10)
#define PORT_CS_18_WOCS				(1u << 16)
#define PORT_CS_18_WODS				(1u << 17)
#define PORT_CS_18_WOU4S			(1u << 18)
#define PORT_CS_18_CSA				(1u << 22)
#define PORT_CS_18_TIP				(1u << 24)
#define PORT_CS_19					0x13
#define PORT_CS_19_DPR				(1u << 0)
#define PORT_CS_19_PC				(1u << 3)
#define PORT_CS_19_PID				(1u << 4)
#define PORT_CS_19_WOC				(1u << 16)
#define PORT_CS_19_WOD				(1u << 17)
#define PORT_CS_19_WOU4				(1u << 18)

// adapter TMU capability
#define TMU_ADP_CS_3				0x03
#define TMU_ADP_CS_3_UDM			(1u << 29)
#define TMU_ADP_CS_6				0x06
#define TMU_ADP_CS_6_DTS			(1u << 1)

// DP adapter capability
#define ADP_DP_CS_0					0x00
#define ADP_DP_CS_0_VIDEO_HOP_SHIFT	16
#define ADP_DP_CS_0_VIDEO_HOP_MASK	(0x7ffu << 16)
#define ADP_DP_CS_0_AE				(1u << 30)
#define ADP_DP_CS_0_VE				(1u << 31)
#define ADP_DP_CS_1					0x01
#define ADP_DP_CS_1_AUX_TX_HOP_MASK	0x7ffu
#define ADP_DP_CS_1_AUX_RX_HOP_SHIFT	11
#define ADP_DP_CS_1_AUX_RX_HOP_MASK	(0x7ffu << 11)
#define ADP_DP_CS_2					0x02
#define ADP_DP_CS_2_HPD				(1u << 6)
#define ADP_DP_CS_3					0x03
#define ADP_DP_CS_3_HPDC			(1u << 9)
#define DP_LOCAL_CAP				0x04
#define DP_REMOTE_CAP				0x05
#define DP_STATUS					0x06
#define DP_STATUS_CTRL				0x06
#define DP_STATUS_CTRL_CMHS			(1u << 25)
#define DP_STATUS_CTRL_UF			(1u << 26)
#define DP_COMMON_CAP				0x07

#define DP_CAP_RATE(x)				(((x) >> 8) & 0xf)
#define DP_CAP_RATE_MASK			(0xfu << 8)
#define DP_CAP_LANES(x)				(((x) >> 12) & 0x7)
#define DP_CAP_LANES_MASK			(0x7u << 12)
#define DP_CAP_LTTPR_NS				(1u << 27)
#define DP_CAP_BW_MODE				(1u << 28)
#define DP_CAP_DPRX_DONE			(1u << 31)

// PCIe adapter capability
#define ADP_PCIE_CS_0				0x00
#define ADP_PCIE_CS_0_LTSSM(x)		(((x) >> 25) & 0xf)
#define ADP_PCIE_CS_0_PE			(1u << 31)

// USB3 adapter capability
#define ADP_USB3_CS_0				0x00
#define ADP_USB3_CS_0_V				(1u << 30)
#define ADP_USB3_CS_0_PE			(1u << 31)
#define ADP_USB3_CS_1				0x01
#define ADP_USB3_CS_1_CUBW(x)		((x) & 0xfff)
#define ADP_USB3_CS_1_CDBW(x)		(((x) >> 12) & 0xfff)
#define ADP_USB3_CS_1_HCA			(1u << 31)
#define ADP_USB3_CS_2				0x02
#define ADP_USB3_CS_2_AUBW_MASK		0xfffu
#define ADP_USB3_CS_2_ADBW_SHIFT	12
#define ADP_USB3_CS_2_ADBW_MASK		(0xfffu << 12)
#define ADP_USB3_CS_2_CMR			(1u << 31)
#define ADP_USB3_CS_3				0x03
#define ADP_USB3_CS_3_SCALE_MASK	0x3f
#define ADP_USB3_CS_4				0x04
#define ADP_USB3_CS_4_MSLR(x)		(((x) >> 12) & 0x7f)
#define ADP_USB3_CS_4_MSLR_20G		0x1


// #pragma mark - Path configuration space


#define USB4_FIRST_PATH_HOP			8
	// hops below this are reserved (control and NHI)

// two dwords per hop entry
#define HOP_0_NEXT_HOP(x)			((x) & 0x7ff)
#define HOP_0_OUT_ADAPTER(x)		(((x) & 0x3f) << 11)
#define HOP_0_INITIAL_CREDITS(x)	(((x) & 0x7f) << 17)
#define HOP_0_PMPS					(1u << 24)
#define HOP_0_VALID					(1u << 31)
#define HOP_1_WEIGHT(x)				((x) & 0xf)
#define HOP_1_PRIORITY(x)			(((x) & 0x7) << 8)
#define HOP_1_DROP_PACKETS			(1u << 11)
#define HOP_1_COUNTER(x)			(((x) & 0x7ff) << 12)
#define HOP_1_COUNTER_ENABLE		(1u << 23)
#define HOP_1_INGRESS_FC			(1u << 24)
#define HOP_1_EGRESS_FC				(1u << 25)
#define HOP_1_INGRESS_SHARED		(1u << 26)
#define HOP_1_EGRESS_SHARED			(1u << 27)
#define HOP_1_PENDING				(1u << 28)


#endif	// USB4_REGS_H
