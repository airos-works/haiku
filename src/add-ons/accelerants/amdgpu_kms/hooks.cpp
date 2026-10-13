/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "accelerant.h"


extern "C" void*
get_accelerant_hook(uint32 feature, void* data)
{
	switch (feature) {
		case B_INIT_ACCELERANT:
			return (void*)amdgpu_init_accelerant;
		case B_UNINIT_ACCELERANT:
			return (void*)amdgpu_uninit_accelerant;
		case B_CLONE_ACCELERANT:
			return (void*)amdgpu_clone_accelerant;
		case B_ACCELERANT_CLONE_INFO_SIZE:
			return (void*)amdgpu_accelerant_clone_info_size;
		case B_GET_ACCELERANT_CLONE_INFO:
			return (void*)amdgpu_get_accelerant_clone_info;
		case B_GET_ACCELERANT_DEVICE_INFO:
			return (void*)amdgpu_get_accelerant_device_info;
		case B_ACCELERANT_RETRACE_SEMAPHORE:
			return (void*)amdgpu_accelerant_retrace_semaphore;

		case B_ACCELERANT_MODE_COUNT:
			return (void*)amdgpu_accelerant_mode_count;
		case B_GET_MODE_LIST:
			return (void*)amdgpu_get_mode_list;
		case B_PROPOSE_DISPLAY_MODE:
			return (void*)amdgpu_propose_display_mode;
		case B_SET_DISPLAY_MODE:
			return (void*)amdgpu_set_display_mode;
		case B_GET_DISPLAY_MODE:
			return (void*)amdgpu_get_display_mode;
		case B_GET_PREFERRED_DISPLAY_MODE:
			return (void*)amdgpu_get_preferred_display_mode;
		case B_GET_EDID_INFO:
			return (void*)amdgpu_get_edid_info;
		case B_GET_FRAME_BUFFER_CONFIG:
			return (void*)amdgpu_get_frame_buffer_config;
		case B_GET_PIXEL_CLOCK_LIMITS:
			return (void*)amdgpu_get_pixel_clock_limits;

		case B_DPMS_CAPABILITIES:
			return (void*)amdgpu_dpms_capabilities;
		case B_DPMS_MODE:
			return (void*)amdgpu_dpms_mode;
		case B_SET_DPMS_MODE:
			return (void*)amdgpu_set_dpms_mode;
		case B_SET_BRIGHTNESS:
			return (void*)amdgpu_set_brightness;
		case B_GET_BRIGHTNESS:
			return (void*)amdgpu_get_brightness;

		case B_GET_DISPLAY_OUTPUT_COUNT:
			return (void*)amdgpu_display_output_count;
		case B_GET_DISPLAY_OUTPUTS:
			return (void*)amdgpu_get_display_outputs;
		case B_GET_DISPLAY_OUTPUT_MODES:
			return (void*)amdgpu_get_display_output_modes;
		case B_SET_DISPLAY_LAYOUT:
			return (void*)amdgpu_set_display_layout;
		case B_SET_DISPLAY_CHANGE_PORT:
			return (void*)amdgpu_set_display_change_port;
		case B_WAIT_FOR_DISPLAY_RESTORE:
			return (void*)amdgpu_wait_for_display_restore;

		case B_SET_CURSOR_BITMAP:
			return (void*)amdgpu_set_cursor_bitmap;
		case B_MOVE_CURSOR:
			return (void*)amdgpu_move_cursor;
		case B_SHOW_CURSOR:
			return (void*)amdgpu_show_cursor;
	}
	return NULL;
}
