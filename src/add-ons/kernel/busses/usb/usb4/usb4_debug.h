/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef USB4_DEBUG_H
#define USB4_DEBUG_H


#include <KernelExport.h>


extern int32 gUSB4DebugLevel;

#define USB4_LOG(level, format, args...) \
	do { \
		if (gUSB4DebugLevel >= (level)) \
			dprintf("usb4: " format, ##args); \
	} while (false)

#define TRACE_ALWAYS(format, args...)	dprintf("usb4: " format, ##args)
#define TRACE_ERROR(format, args...)	dprintf("usb4: error: " format, ##args)
#define TRACE(format, args...)			USB4_LOG(1, format, ##args)
#define TRACE_VERBOSE(format, args...)	USB4_LOG(2, format, ##args)


#endif	// USB4_DEBUG_H
