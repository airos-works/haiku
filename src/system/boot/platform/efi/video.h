/*
 * Copyright 2004, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
 * Copyright 2011, Rene Gollent, rene@gollent.com. All rights reserved.
 *
 * Distributed under the terms of the MIT License.
 */
#ifndef VIDEO_H
#define VIDEO_H


class Menu;
class MenuItem;

bool video_mode_hook(Menu *menu, MenuItem *item);
bool video_set_kernel_frame_buffer(void);
Menu *video_mode_menu();

// diagnostic progress marks, valid until the loader enters the kernel
void video_diag_mark(int step);
enum {
	kDiagCPUTrampoline,		// white
	kDiagCPUStarting,		// yellow
	kDiagCPUInitSent,		// orange
	kDiagCPUStartupSent,	// blue
	kDiagCPUStarted,		// green
	kDiagCPUNotDelivered,	// purple: an IPI was never sent
	kDiagCPUTimedOut,		// red: given up after a second
	kDiagCPUCopying			// grey: copying the trampoline code
};
void video_diag_cpu_mark(int cpu, int state);

#endif	/* VIDEO_H */
