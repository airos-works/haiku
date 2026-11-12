/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef ARM64_GICV3_ITS_COMMANDS_H
#define ARM64_GICV3_ITS_COMMANDS_H

#include <stdint.h>

namespace Gicv3Its {

// Arm IHI 0069G, chapters 5 and 8. The supported host is little-endian.
struct Command {
	uint64_t words[4];
};
static_assert(sizeof(Command) == 32, "ITS commands are 256 bits");

inline Command
MapDevice(uint32_t device, uint64_t table, unsigned eventBits, bool valid)
{
	return {{(uint64_t(device) << 32) | 0x08, eventBits - 1,
		(table & UINT64_C(0x000fffffffffff00)) | (uint64_t(valid) << 63), 0}};
}

inline Command
MapCollection(uint16_t collection, uint64_t target, bool valid)
{
	return {{0x09, 0, (target & UINT64_C(0x000fffffffff0000))
		| (uint64_t(valid) << 63) | collection, 0}};
}

inline Command
MapInterrupt(uint32_t device, uint32_t event, uint32_t interrupt, uint16_t collection)
{
	return {{(uint64_t(device) << 32) | 0x0a,
		(uint64_t(interrupt) << 32) | event, collection, 0}};
}

inline Command
EventCommand(uint8_t opcode, uint32_t device, uint32_t event)
{
	return {{(uint64_t(device) << 32) | opcode, event, 0, 0}};
}

// MOVI: the event's LPI goes to another collection, with its pending state.
inline Command
MoveInterrupt(uint32_t device, uint32_t event, uint16_t collection)
{
	return {{(uint64_t(device) << 32) | 0x01, event, collection, 0}};
}

inline Command
InvalidateAll(uint16_t collection)
{
	return {{0x0d, 0, collection, 0}};
}

inline Command
Sync(uint64_t target)
{
	return {{0x05, 0, target & UINT64_C(0x000fffffffff0000), 0}};
}

static const uint32_t kFirstLpi = 8192;
static const uint32_t kEventCount = 32;
static const uint32_t kQueueBytes = 65536;

inline bool
ValidCount(uint32_t count)
{
	return count != 0 && count <= kEventCount;
}

inline bool
ValidTableRange(uint64_t address, uint64_t bytes, uint64_t alignment,
	uint64_t minimumAddress = 0)
{
	// RK3588 GIC/ITS table masters address 35 bits. Validate the whole buffer.
	return bytes != 0 && alignment != 0 && (alignment & (alignment - 1)) == 0
		&& address >= minimumAddress && address % alignment == 0
		&& address < (UINT64_C(1) << 35)
		&& bytes <= (UINT64_C(1) << 35) - address;
}

inline uint32_t
NextCommand(uint32_t offset)
{
	return (offset + sizeof(Command)) % kQueueBytes;
}

} // namespace Gicv3Its
#endif
