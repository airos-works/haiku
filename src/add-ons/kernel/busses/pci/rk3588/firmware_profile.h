/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef RK3588_PCIE_FIRMWARE_PROFILE_H
#define RK3588_PCIE_FIRMWARE_PROFILE_H

#include <stdint.h>
#include <string.h>

namespace RK3588Firmware {

// Explicit lab profile: edk2-rk3588 v1.1, ROCK 5 ITX, mainline DT only.
// Firmware commit 6a682c0ef3ed74feb8b0d98f1c2aa771ddfbae18.
// Rk3588Pcie.h and PciSegmentLib.c define these live mappings. The Linux
// DT config/ranges describe a different setup and must not replace them.
static const uint64_t kMemorySize = UINT64_C(0x01000000);
static const unsigned kConfigSize = 4096;

struct PortProfile {
	unsigned segment;
	uint64_t rootConfig;
	uint64_t endpointConfig;
	uint64_t memoryBase;
	uint32_t endpointId;
	uint32_t classCode;
	uint8_t revision;
	const char* endpointName;
};

// Only root/endpoint pairs independently captured under EDK2 and Haiku.
static const PortProfile kPorts[] = {
	{0, UINT64_C(0xa40000000), UINT64_C(0x900100000), 0xf0000000,
		0x61001f99, 0x010802, 1, "NVMe SSD"},
	{1, UINT64_C(0xa40400000), UINT64_C(0x940100000), 0xf1000000,
		0x11641b21, 0x010601, 2, "ASM1164"},
	{2, UINT64_C(0xa40800000), UINT64_C(0x980100000), 0xf2000000,
		0x27258086, 0x028000, 0x1a, "Intel Wi-Fi"},
	{3, UINT64_C(0xa40c00000), UINT64_C(0x9c0100000), 0xf3000000,
		0x812510ec, 0x020000, 5, "RTL8125"},
	{4, UINT64_C(0xa41000000), UINT64_C(0xa00100000), 0xf4000000,
		0x812510ec, 0x020000, 5, "RTL8125"}
};

inline const PortProfile*
FindPort(uint64_t rootConfig)
{
	for (unsigned i = 0; i < sizeof(kPorts) / sizeof(kPorts[0]); i++) {
		if (kPorts[i].rootConfig == rootConfig)
			return &kPorts[i];
	}
	return nullptr;
}

inline bool
ProfileAllowsPort(const char* profile, const PortProfile& port)
{
	return ((port.segment == 0 || port.segment == 1 || port.segment == 2 || port.segment == 3
			|| port.segment == 4)
			&& strcmp(profile, "rock5-itx-edk2-v1.1-dt-onboard") == 0)
		|| (port.segment == 0
			&& (strcmp(profile, "rock5-itx-edk2-v1.1-dt-nvme") == 0
				|| strcmp(profile, "rock5-itx-edk2-v1.1-dt-samsung950") == 0));
}

// Intel M.2 Wi-Fi cards seen in the E-key slot (segment 2): AX210 and the
// Wi-Fi 7 BE200. Both have one 64-bit 16 KiB BAR, which EDK2 assigns.
static const uint32_t kIntelWifiIds[] = {
	0x27258086,	// AX210
	0x272b8086	// BE200
};

inline bool
EndpointIdMatches(uint32_t id, const PortProfile& port)
{
	// Both controllers have been captured on this exact EDK2 port. The
	// firmware's BAR and root window are validated separately below.
	if (port.segment == 2) {
		for (unsigned i = 0; i < sizeof(kIntelWifiIds) / sizeof(kIntelWifiIds[0]); i++) {
			if (id == kIntelWifiIds[i])
				return true;
		}
		return false;
	}
	return id == port.endpointId
		|| (port.segment == 0 && id == 0xa802144d);
}

inline bool
ValidAccess(unsigned bus, unsigned device, unsigned function, unsigned offset,
	unsigned size)
{
	// CFG0 has mirrored slots. Accessing 01:01.0 can hang this controller.
	// Every supported root has one captured single-function endpoint.
	return bus <= 1 && device == 0 && function == 0
		&& (size == 1 || size == 2 || size == 4)
		&& offset < kConfigSize && size <= kConfigSize - offset
		&& (offset & (size - 1)) == 0;
}

inline bool
RootConfigurationMatches(const uint32_t* config, uint64_t& memoryBase,
	uint64_t& memorySize,
	const PortProfile& port = kPorts[0])
{
	if (config[0] != 0x35881d87 || (config[2] >> 8) != 0x060400
		|| ((config[3] >> 16) & 0xff) != 1
		|| (config[6] & 0x00ffffff) != 0x00010100
		|| (config[1] & 2) == 0) {
		return false;
	}
	// v1.1 keeps the root's PCIe capability at 0x70.
	if ((config[0x70 / 4] & 0xff) != 0x10)
		return false;
	// Export only the memory window currently forwarded by this root bridge.
	// The PCI core can then reserve the firmware BAR without moving it.
	uint64_t base = uint64_t(config[8] & 0xfff0) << 16;
	uint64_t limit = uint64_t(config[8] & 0xfff00000) | 0xfffff;
	if (base < port.memoryBase || limit < base
		|| limit >= port.memoryBase + kMemorySize) {
		return false;
	}
	memoryBase = base;
	memorySize = limit + 1 - base;
	return true;
}

inline bool
RootLinkActive(const uint32_t* config)
{
	uint32_t link = config[0x80 / 4] >> 16;
	return (link & 0x2000) != 0 && (link & 0x000f) != 0 && (link & 0x03f0) != 0;
}

inline bool
RootMatches(const uint32_t* config, uint64_t& memoryBase, uint64_t& memorySize,
	const PortProfile& port = kPorts[0])
{
	// Downstream config access still requires an active link with training clear.
	return RootLinkActive(config) && (config[0x80 / 4] & 0x08000000) == 0
		&& RootConfigurationMatches(config, memoryBase, memorySize, port);
}

inline bool
MemoryBarMatches(const uint32_t* config, unsigned index, unsigned flags,
	uint64_t bytes, uint64_t memoryBase, uint64_t memorySize)
{
	if (index >= 6 || (flags != 0 && flags != 4) || (flags == 4 && index >= 5)
		|| bytes == 0 || (bytes & (bytes - 1)) != 0) {
		return false;
	}
	uint64_t bar = config[4 + index] & ~UINT64_C(0xf);
	if (flags == 4)
		bar |= uint64_t(config[5 + index]) << 32;
	return (config[4 + index] & 0xf) == flags && memorySize >= bytes
		&& (bar & (bytes - 1)) == 0 && bar >= memoryBase
		&& bar - memoryBase <= memorySize - bytes;
}

inline bool
EndpointMatches(const uint32_t* config, uint64_t memoryBase, uint64_t memorySize,
	const PortProfile& port = kPorts[0])
{
	// The BE200 sets the multi-function bit of its header type (Bluetooth is
	// on USB, so function 0 is the only one; ValidAccess() never reaches
	// another). The type itself must be a plain endpoint.
	uint8_t headerType = (config[3] >> 16) & 0xff;
	if (port.segment == 2)
		headerType &= 0x7f;
	if (!EndpointIdMatches(config[0], port) || config[2] >> 8 != port.classCode
		|| headerType != 0
		|| (port.segment != 2 && (config[1] & 2) == 0)) {
		return false;
	}
	if (port.segment == 0)
		return MemoryBarMatches(config, 0, 4, 0x4000, memoryBase, memorySize);
	// EDK2 assigns the Wi-Fi card's BAR but leaves memory decoding disabled.
	// The PCI core enables the command register after importing this fixed
	// resource. Any stepping of the listed cards is accepted.
	if (port.segment == 2)
		return MemoryBarMatches(config, 0, 4, 0x4000, memoryBase, memorySize);
	if ((config[2] & 0xff) != port.revision)
		return false;
	// BAR extents independently recorded from this unit's Linux resources.
	if (port.segment == 1) {
		return MemoryBarMatches(config, 0, 0, 0x2000, memoryBase, memorySize)
			&& MemoryBarMatches(config, 5, 0, 0x2000, memoryBase, memorySize)
			&& config[4] != config[9];
	}
	if (port.segment == 3 || port.segment == 4) {
		if (!MemoryBarMatches(config, 2, 4, 0x10000, memoryBase, memorySize)
			|| !MemoryBarMatches(config, 4, 4, 0x4000, memoryBase, memorySize)) {
			return false;
		}
		uint64_t first = (uint64_t(config[7]) << 32) | (config[6] & ~UINT64_C(0xf));
		uint64_t second = (uint64_t(config[9]) << 32) | (config[8] & ~UINT64_C(0xf));
		return first <= second ? second - first >= 0x10000 : first - second >= 0x4000;
	}
	return false;
}

} // namespace RK3588Firmware

#endif
