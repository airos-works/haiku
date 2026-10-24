/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef QCOM_DP_TU_H
#define QCOM_DP_TU_H


#include <SupportDefs.h>


#ifdef __cplusplus
extern "C" {
#endif

/*	The transfer unit registers of a DisplayPort stream (dp_tu.c, Linux's
	calculation): RGB 4:4:4, no DSC, no FEC. \a linkRate in MHz of the
	link symbol clock (162, 270, 540, 810), \a pixelClock in kHz. */
status_t qcom_dp_calc_tu(uint32 linkRate, uint32 pixelClock, uint32 hActive,
	uint32 hBlank, uint32 lanes, uint32 bitsPerPixel, uint32* _tu,
	uint32* _validBoundary, uint32* _validBoundary2);

#ifdef __cplusplus
}
#endif


#endif	// QCOM_DP_TU_H
