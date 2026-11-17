/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef VC4_REGS_H
#define VC4_REGS_H


/*	The registers of the VideoCore IV 3D core (V3D 2.1, BCM2835/6/7) that the
	driver uses, as the "VideoCore IV 3D Architecture Reference Guide"
	(Broadcom, 2013) names them. One 4 KB block, v3d@7ec00000 in the device
	tree. */


#include "vc4_fields.h"


// identification: IDENT0 is "V3D" and the technology version
#define V3D_IDENT0				0x0000
#define  V3D_EXPECTED_IDENT0	((2u << 24) | ('D' << 16) | ('3' << 8) | 'V')
#define V3D_IDENT1				0x0004
#define  V3D_IDENT1_VPM_SIZE_MASK	VC4_MASK(31, 28)
#define  V3D_IDENT1_VPM_SIZE_SHIFT	28
#define  V3D_IDENT1_NSEM_MASK		VC4_MASK(23, 16)
#define  V3D_IDENT1_NSEM_SHIFT		16
#define  V3D_IDENT1_TUPS_MASK		VC4_MASK(15, 12)
#define  V3D_IDENT1_TUPS_SHIFT		12
#define  V3D_IDENT1_QUPS_MASK		VC4_MASK(11, 8)
#define  V3D_IDENT1_QUPS_SHIFT		8
#define  V3D_IDENT1_NSLC_MASK		VC4_MASK(7, 4)
#define  V3D_IDENT1_NSLC_SHIFT		4
#define  V3D_IDENT1_REV_MASK		VC4_MASK(3, 0)
#define  V3D_IDENT1_REV_SHIFT		0
#define V3D_IDENT2				0x0008
#define V3D_SCRATCH				0x0010

// caches: the L2 cache and the slices' (texture units 0/1, uniforms,
// instructions)
#define V3D_L2CACTL				0x0020
#define  V3D_L2CACTL_L2CCLR		BIT(2)
#define  V3D_L2CACTL_L2CDIS		BIT(1)
#define  V3D_L2CACTL_L2CENA		BIT(0)
#define V3D_SLCACTL				0x0024
#define  V3D_SLCACTL_T1CC_MASK	VC4_MASK(27, 24)
#define  V3D_SLCACTL_T1CC_SHIFT	24
#define  V3D_SLCACTL_T0CC_MASK	VC4_MASK(19, 16)
#define  V3D_SLCACTL_T0CC_SHIFT	16
#define  V3D_SLCACTL_UCC_MASK	VC4_MASK(11, 8)
#define  V3D_SLCACTL_UCC_SHIFT	8
#define  V3D_SLCACTL_ICC_MASK	VC4_MASK(3, 0)
#define  V3D_SLCACTL_ICC_SHIFT	0

// interrupts: status (write 1 to clear), enable and disable (write 1 to
// set; reading either gives the enabled ones)
#define V3D_INTCTL				0x0030
#define V3D_INTENA				0x0034
#define V3D_INTDIS				0x0038
#define  V3D_INT_SPILLUSE		BIT(3)
#define  V3D_INT_OUTOMEM		BIT(2)	// binner out of overflow memory
#define  V3D_INT_FLDONE			BIT(1)	// binning list flushed
#define  V3D_INT_FRDONE			BIT(0)	// render frame done

// the control list executors: thread 0 bins, thread 1 renders
#define V3D_CT0CS				0x0100
#define V3D_CT1CS				0x0104
#define V3D_CTNCS(n)			(V3D_CT0CS + 4 * (n))
#define  V3D_CTRSTA				BIT(15)	// reset the thread
#define  V3D_CTSEMA				BIT(12)
#define  V3D_CTRTSD				BIT(8)
#define  V3D_CTRUN				BIT(5)
#define  V3D_CTSUBS				BIT(4)
#define  V3D_CTERR				BIT(3)
#define  V3D_CTMODE				BIT(0)
#define V3D_CT0EA				0x0108
#define V3D_CT1EA				0x010c
#define V3D_CTNEA(n)			(V3D_CT0EA + 4 * (n))
#define V3D_CT0CA				0x0110
#define V3D_CT1CA				0x0114
#define V3D_CTNCA(n)			(V3D_CT0CA + 4 * (n))
#define V3D_CT00RA0				0x0118
#define V3D_CT01RA0				0x011c
#define V3D_CT0LC				0x0120
#define V3D_CT1LC				0x0124
#define V3D_CT0PC				0x0128
#define V3D_CT1PC				0x012c
#define V3D_PCS					0x0130	// pipeline control and status
#define V3D_BFC					0x0134	// binning mode flush count
#define V3D_RFC					0x0138	// rendering mode frame count

// the binner's memory pool: current and overflow
#define V3D_BPCA				0x0300
#define V3D_BPCS				0x0304
#define V3D_BPOA				0x0308
#define V3D_BPOS				0x030c
#define V3D_BXCF				0x0310

// QPU scheduler
#define V3D_SQRSV0				0x0410
#define V3D_SQRSV1				0x0414
#define V3D_SQCNTL				0x0418
#define V3D_SRQCS				0x043c

// vertex pipe memory
#define V3D_VPACNTL				0x0500
#define V3D_VPMBASE				0x0504

// debugging and errors
#define V3D_DBGE				0x0f00
#define V3D_FDBGO				0x0f04
#define V3D_FDBGB				0x0f08
#define V3D_FDBGR				0x0f0c
#define V3D_FDBGS				0x0f10
#define V3D_ERRSTAT				0x0f20


#endif	/* VC4_REGS_H */
