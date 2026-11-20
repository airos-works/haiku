/*
** Copyright 2004, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
** Distributed under the terms of the MIT License.
*/
#ifndef CONSOLE_H
#define CONSOLE_H


#include <boot/platform/generic/text_console.h>


status_t console_init(void);
uint32 console_check_boot_keys(void);
void console_control_apple(bool graphics);


#endif	/* CONSOLE_H */
