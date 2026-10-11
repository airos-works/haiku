/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#ifndef AMDGPU_GFX_COMMAND_VALIDATOR_H
#define AMDGPU_GFX_COMMAND_VALIDATOR_H

#include <stdint.h>

namespace amdgpu {

// Initial GFX8 policy for a kernel-owned, immutable outer IB. This is still a
// root-only qualification path, not proof of arbitrary shader safety, resource
// isolation or recoverable execution. Nested IBs and memory-loaded state are
// deliberately unsupported, so they cannot bypass the direct register checks.
struct GfxCommandValidation {
	enum Error {
		OK, BadSize, PacketType, PacketLength, Opcode, Control, Register, Address
	};
	Error error;
	uint32_t dword;
	GfxCommandValidation(Error e = OK, uint32_t at = 0) : error(e), dword(at) {}
};

GfxCommandValidation ValidateGfx8Commands(const volatile uint32_t* words,
	uint32_t count);

}
#endif
