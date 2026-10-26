/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _QCOM_DP_AUDIO_H
#define _QCOM_DP_AUDIO_H


#include <module.h>


/*	Audio over the SC8280XP's DisplayPort controllers (the qcom_display
	driver): the audio DSP's DISPLAY_PORT_RX module feeds a controller's
	audio engine, which the display driver turns on with the link up. For
	the audio driver, before it starts that module's graph. */

#define QCOM_DP_AUDIO_MODULE_NAME "drivers/graphics/qcom_display/dp_audio/v1"

// the controllers, as the DSP's DISPLAY_PORT_RX interface (dptx_idx) counts
// them
enum {
	QCOM_DP_AUDIO_USBC_0	= 0,
	QCOM_DP_AUDIO_USBC_1	= 1,
	QCOM_DP_AUDIO_HDMI		= 2,	// the board's HDMI bridge
};

struct qcom_dp_audio_module_info {
	module_info	info;
	// B_DEV_NOT_READY: no sink or no link on that controller
	status_t	(*start)(uint32 controller, uint32 channels);
	void		(*stop)(uint32 controller);
};


#endif	// _QCOM_DP_AUDIO_H
