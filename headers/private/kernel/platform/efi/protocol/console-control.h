// Copyright 2020 Haiku, Inc. All rights reserved
// Released under the terms of the MIT License

#pragma once

#include <efi/types.h>

#define EFI_CONSOLE_CONTROL_PROTOCOL_GUID \
	{0xf42f7782, 0x012e, 0x4c12, \
		{0x99, 0x56, 0x49, 0xf9, 0x43, 0x04, 0xf7, 0x21}}

extern efi_guid ConsoleControlProtocol;

typedef enum {
	EfiConsoleControlScreenText,
	EfiConsoleControlScreenGraphics,
	EfiConsoleControlScreenMax
} efi_console_control_screen_mode;

// The protocol has no revision field (EDK's EFI_CONSOLE_CONTROL_PROTOCOL,
// rEFInd, GRUB): with one, every call landed on the next function, and
// switching to graphics mode called LockStdIn with a bogus password, which
// locked the keyboard (Haiku #16753).
typedef struct efi_console_control_protocol {
	efi_status (*GetMode) (struct efi_console_control_protocol* self,
		efi_console_control_screen_mode* mode,
		bool* gopUgaExists,
		bool* stdInLocked) EFIAPI;

	efi_status (*SetMode) (struct efi_console_control_protocol* self,
		efi_console_control_screen_mode mode) EFIAPI;

	efi_status (*LockStdIn) (struct efi_console_control_protocol* self,
		uint16_t* password) EFIAPI;

} efi_console_control_protocol;
