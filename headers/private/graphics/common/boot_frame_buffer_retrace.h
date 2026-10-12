/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BOOT_FRAME_BUFFER_RETRACE_H
#define _BOOT_FRAME_BUFFER_RETRACE_H


/*!	Between an accelerant that shows the boot frame buffer (framebuffer,
	vesa), which knows nothing of the display hardware, and a GPU driver
	that knows it without driving the display itself (yet): the semaphore
	released at every vertical blank of the display that shows the boot
	frame buffer, for B_ACCELERANT_RETRACE_SEMAPHORE.

	The GPU driver answers on its device; the argument is a sem_id, the
	calling team's (see retrace_semaphores.h). The accelerant keeps the
	device open for as long as it uses the semaphore.
*/


#include <Drivers.h>


#define B_GET_BOOT_FRAME_BUFFER_RETRACE_SEMAPHORE	(B_DEVICE_OP_CODES_END + 0x5254)


#endif	/* _BOOT_FRAME_BUFFER_RETRACE_H */
