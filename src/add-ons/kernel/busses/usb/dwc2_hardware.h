/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef DWC2_HARDWARE_H
#define DWC2_HARDWARE_H


/*	The Synopsys DesignWare USB 2.0 Hi-Speed On-The-Go core ("DWC2", "DWC
	OTG") in host mode, as in the Raspberry Pi's BCM2835/6/7. Register names
	follow the databook and Linux's drivers/usb/dwc2/hw.h. */


// core global registers
#define DWC2_GOTGCTL				0x000
#define DWC2_GOTGINT				0x004
#define DWC2_GAHBCFG				0x008
#define		DWC2_GAHBCFG_GLBLINTRMSK	(1u << 0)
#define		DWC2_GAHBCFG_HBSTLEN_MASK	(0xfu << 1)
#define		DWC2_GAHBCFG_DMA_EN			(1u << 5)
#define DWC2_GUSBCFG				0x00c
#define		DWC2_GUSBCFG_PHYIF16		(1u << 3)
#define		DWC2_GUSBCFG_ULPI_UTMI_SEL	(1u << 4)
#define		DWC2_GUSBCFG_PHYSEL			(1u << 6)
#define		DWC2_GUSBCFG_SRPCAP			(1u << 8)
#define		DWC2_GUSBCFG_HNPCAP			(1u << 9)
#define		DWC2_GUSBCFG_PHY_LP_CLK_SEL	(1u << 15)
#define		DWC2_GUSBCFG_ULPI_FS_LS		(1u << 17)
#define		DWC2_GUSBCFG_ULPI_CLK_SUSP_M (1u << 19)
#define		DWC2_GUSBCFG_FORCEHOSTMODE	(1u << 29)
#define		DWC2_GUSBCFG_FORCEDEVMODE	(1u << 30)
#define DWC2_GRSTCTL				0x010
#define		DWC2_GRSTCTL_CSFTRST		(1u << 0)
#define		DWC2_GRSTCTL_RXFFLSH		(1u << 4)
#define		DWC2_GRSTCTL_TXFFLSH		(1u << 5)
#define		DWC2_GRSTCTL_TXFNUM_ALL		(0x10u << 6)
#define		DWC2_GRSTCTL_AHBIDLE		(1u << 31)
#define DWC2_GINTSTS				0x014
#define DWC2_GINTMSK				0x018
#define		DWC2_GINT_CURMODE_HOST		(1u << 0)
#define		DWC2_GINT_MODEMIS			(1u << 1)
#define		DWC2_GINT_OTGINT			(1u << 2)
#define		DWC2_GINT_SOF				(1u << 3)
#define		DWC2_GINT_RXFLVL			(1u << 4)
#define		DWC2_GINT_INCOMPLP			(1u << 21)
#define		DWC2_GINT_PRTINT			(1u << 24)
#define		DWC2_GINT_HCHINT			(1u << 25)
#define		DWC2_GINT_PTXFEMP			(1u << 26)
#define		DWC2_GINT_CONIDSTSCHNG		(1u << 28)
#define		DWC2_GINT_DISCONNINT		(1u << 29)
#define		DWC2_GINT_SESSREQINT		(1u << 30)
#define		DWC2_GINT_WKUPINT			(1u << 31)
#define DWC2_GRXFSIZ				0x024
#define DWC2_GNPTXFSIZ				0x028
#define DWC2_GNPTXSTS				0x02c
#define DWC2_GSNPSID				0x040
#define		DWC2_GSNPSID_ID_MASK		0xfffff000
#define		DWC2_GSNPSID_OTG_2			0x4f542000
#define		DWC2_GSNPSID_OTG_3			0x4f543000
#define		DWC2_GSNPSID_OTG_4			0x4f544000
#define DWC2_GHWCFG1				0x044
#define DWC2_GHWCFG2				0x048
#define		DWC2_GHWCFG2_ARCH(x)		(((x) >> 3) & 3)
#define		DWC2_GHWCFG2_ARCH_INT_DMA	2
#define		DWC2_GHWCFG2_HS_PHY(x)		(((x) >> 6) & 3)
#define		DWC2_GHWCFG2_HS_PHY_UTMI	1
#define		DWC2_GHWCFG2_HS_PHY_ULPI	2
#define		DWC2_GHWCFG2_HS_PHY_BOTH	3
#define		DWC2_GHWCFG2_CHANNELS(x)	((((x) >> 14) & 0xf) + 1)
#define DWC2_GHWCFG3				0x04c
#define		DWC2_GHWCFG3_XFER_WIDTH(x)	(((x) & 0xf) + 11)
#define		DWC2_GHWCFG3_PKT_WIDTH(x)	((((x) >> 4) & 7) + 4)
#define		DWC2_GHWCFG3_FIFO_DEPTH(x)	((x) >> 16)
#define DWC2_GHWCFG4				0x050
#define		DWC2_GHWCFG4_UTMI_WIDTH(x)	(((x) >> 14) & 3)
#define		DWC2_GHWCFG4_UTMI_WIDTH_16	1
#define DWC2_HPTXFSIZ				0x100

// host mode registers
#define DWC2_HCFG					0x400
#define		DWC2_HCFG_FSLSPCLKSEL_MASK	3u
#define		DWC2_HCFG_FSLSSUPP			(1u << 2)
#define DWC2_HFIR					0x404
#define		DWC2_HFIR_FRINT_MASK		0xffffu
#define DWC2_HFNUM					0x408
#define		DWC2_HFNUM_FRNUM_MASK		0x3fffu
#define DWC2_HPTXSTS				0x410
#define DWC2_HAINT					0x414
#define DWC2_HAINTMSK				0x418
#define DWC2_HPRT0					0x440
#define		DWC2_HPRT0_CONNSTS			(1u << 0)
#define		DWC2_HPRT0_CONNDET			(1u << 1)
#define		DWC2_HPRT0_ENA				(1u << 2)
#define		DWC2_HPRT0_ENACHG			(1u << 3)
#define		DWC2_HPRT0_OVRCURRACT		(1u << 4)
#define		DWC2_HPRT0_OVRCURRCHG		(1u << 5)
#define		DWC2_HPRT0_RES				(1u << 6)
#define		DWC2_HPRT0_SUSP				(1u << 7)
#define		DWC2_HPRT0_RST				(1u << 8)
#define		DWC2_HPRT0_PWR				(1u << 12)
#define		DWC2_HPRT0_SPD(x)			(((x) >> 17) & 3)
#define		DWC2_HPRT0_SPD_HIGH			0
#define		DWC2_HPRT0_SPD_FULL			1
#define		DWC2_HPRT0_SPD_LOW			2
	// writing 1 clears these; writing PrtEna disables the port
#define		DWC2_HPRT0_W1C_MASK			(DWC2_HPRT0_CONNDET \
	| DWC2_HPRT0_ENA | DWC2_HPRT0_ENACHG | DWC2_HPRT0_OVRCURRCHG)

// host channel registers
#define DWC2_HCCHAR(n)				(0x500 + 0x20 * (n))
#define		DWC2_HCCHAR_MPS_MASK		0x7ffu
#define		DWC2_HCCHAR_EPNUM_SHIFT		11
#define		DWC2_HCCHAR_EPDIR_IN		(1u << 15)
#define		DWC2_HCCHAR_LSPDDEV			(1u << 17)
#define		DWC2_HCCHAR_EPTYPE_SHIFT	18
#define		DWC2_HCCHAR_MC_SHIFT		20
#define		DWC2_HCCHAR_DEVADDR_SHIFT	22
#define		DWC2_HCCHAR_ODDFRM			(1u << 29)
#define		DWC2_HCCHAR_CHDIS			(1u << 30)
#define		DWC2_HCCHAR_CHENA			(1u << 31)
#define DWC2_HCSPLT(n)				(0x504 + 0x20 * (n))
#define		DWC2_HCSPLT_PRTADDR_MASK	0x7fu
#define		DWC2_HCSPLT_HUBADDR_SHIFT	7
#define		DWC2_HCSPLT_XACTPOS_ALL		(3u << 14)
#define		DWC2_HCSPLT_COMPSPLT		(1u << 16)
#define		DWC2_HCSPLT_SPLTENA			(1u << 31)
#define DWC2_HCINT(n)				(0x508 + 0x20 * (n))
#define DWC2_HCINTMSK(n)			(0x50c + 0x20 * (n))
#define		DWC2_HCINT_XFERCOMPL		(1u << 0)
#define		DWC2_HCINT_CHHLTD			(1u << 1)
#define		DWC2_HCINT_AHBERR			(1u << 2)
#define		DWC2_HCINT_STALL			(1u << 3)
#define		DWC2_HCINT_NAK				(1u << 4)
#define		DWC2_HCINT_ACK				(1u << 5)
#define		DWC2_HCINT_NYET				(1u << 6)
#define		DWC2_HCINT_XACTERR			(1u << 7)
#define		DWC2_HCINT_BBLERR			(1u << 8)
#define		DWC2_HCINT_FRMOVRUN			(1u << 9)
#define		DWC2_HCINT_DATATGLERR		(1u << 10)
#define		DWC2_HCINT_ALL				0x7ffu
#define DWC2_HCTSIZ(n)				(0x510 + 0x20 * (n))
#define		DWC2_HCTSIZ_XFERSIZE_MASK	0x7ffffu
#define		DWC2_HCTSIZ_PKTCNT_SHIFT	19
#define		DWC2_HCTSIZ_PKTCNT_MASK		0x3ffu
#define		DWC2_HCTSIZ_PID_SHIFT		29
#define		DWC2_HCTSIZ_PID_MASK		3u
#define		DWC2_HCTSIZ_DOPNG			(1u << 31)
#define DWC2_HCDMA(n)				(0x514 + 0x20 * (n))

#define DWC2_PCGCTL					0xe00

// HCCHAR endpoint types
#define DWC2_EPTYPE_CONTROL			0
#define DWC2_EPTYPE_ISOCHRONOUS		1
#define DWC2_EPTYPE_BULK			2
#define DWC2_EPTYPE_INTERRUPT		3

// HCTSIZ packet IDs
#define DWC2_PID_DATA0				0
#define DWC2_PID_DATA2				1
#define DWC2_PID_DATA1				2
#define DWC2_PID_SETUP				3

#define DWC2_MAX_CHANNELS			16


#endif	// DWC2_HARDWARE_H
