/* Copyright 2026, air/OS. Distributed under the terms of the MIT License.
 * Encodings: Mesa src/amd/common/sid.h, registers/gfx8.json and pkt3.json
 * (MIT, Mesa 25.3.6); Linux 6.18.52 gfx_v8_0.c (outer IB/VMID selection).
 */
#include "GfxCommandValidator.h"

namespace amdgpu {
namespace {
using Result = GfxCommandValidation;
static const uint64_t kAddressStart = 0x20000;
static const uint64_t kAddressEnd = 1ULL << 36;

bool Address(uint32_t lo, uint32_t hi, uint64_t bytes, uint32_t alignment = 4)
{
	uint64_t address = uint64_t(hi) << 32 | lo;
	return (lo & (alignment - 1)) == 0 && address >= kAddressStart
		&& address < kAddressEnd && bytes <= kAddressEnd - address;
}

bool UserDataVS(uint32_t offset)
{
	return offset >= 0x4c && offset <= 0x5b;
}

bool ContextRegister(uint32_t reg)
{
	// Polaris adds this register; RadeonSI programs it for CHIP_POLARIS10+.
	if (reg == 0xa20c) return true; // PA_SU_SMALL_PRIM_FILTER_CNTL.
	// Named GFX8 context register blocks. CP_PERFMON_CNTX_CNTL, CP_RINGID
	// and CP_VMID (0xa0d8..0xa0da) are intentionally absent. A context-relative
	// address alone does NOT make a write safe: CP_VMID lies in this window.
	static const uint16_t ranges[][2] = {
		{0xa000,0xa005}, {0xa008,0xa00d}, {0xa00f,0xa017}, {0xa020,0xa021},
		{0xa07a,0xa0d6}, {0xa100,0xa103}, {0xa105,0xa109}, {0xa10b,0xa10d},
		{0xa10f,0xa186}, {0xa191,0xa1b1}, {0xa1b3,0xa1b6}, {0xa1b8,0xa1b8},
		{0xa1ba,0xa1ba}, {0xa1c3,0xa1c5}, {0xa1e0,0xa1e7}, {0xa1f3,0xa1fa},
		{0xa1fc,0xa1fe}, {0xa200,0xa20b}, {0xa280,0xa2a5}, {0xa2a8,0xa2b2},
		{0xa2b4,0xa2b5}, {0xa2b7,0xa2b9}, {0xa2bb,0xa2bd}, {0xa2bf,0xa2c1},
		{0xa2c3,0xa2c3}, {0xa2ca,0xa2cc}, {0xa2ce,0xa2ce}, {0xa2d4,0xa2e6},
		{0xa2f5,0xa30f}, {0xa316,0xa325}, {0xa327,0xa334}, {0xa336,0xa343},
		{0xa345,0xa352}, {0xa354,0xa361}, {0xa363,0xa370}, {0xa372,0xa37f},
		{0xa381,0xa38e}
	};
	for (unsigned i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
		if (reg < ranges[i][0]) return false;
		if (reg <= ranges[i][1]) return true;
	}
	return false;
}

bool ShaderRegister(uint32_t reg, uint32_t value)
{
	if (reg >= 0x2c00 && reg < 0x2d80) {
		uint32_t stage = (reg - 0x2c00) / 0x40;
		uint32_t offset = reg & 0x3f;
		// PS, VS, GS, ES, HS, LS. No PRIV, DEBUG_MODE, CDBG_USER, traps,
		// exceptions or dispatch-draw controls. Trap base/address registers
		// at offsets 0..3 are never writable through this policy.
		static const uint32_t resource1[] = {
			0x0fafffff, 0x3fafffff, 0x0fafffff, 0x3fafffff, 0x07afffff, 0x1fafffff
		};
		static const uint32_t resource2[] = {
			0x0000ffbf, 0x00001fbf, 0x0000003f, 0x1ff000bf, 0x000001bf, 0x0000ffbf
		};
		if (offset >= 0x0c && offset <= 0x1b) return true; // User SGPRs.
		if (offset == 8) return (value & 0xf0000000) == 0; // PGM_LO, VA >> 8.
		if (offset == 9) return value == 0; // PGM_HI, including INST_ATC.
		if (offset == 10) return (value & ~resource1[stage]) == 0;
		if (offset == 11) return (value & ~resource2[stage]) == 0;
		if (reg == 0x2c47) return (value & ~0x3fu) == 0; // Late allocation VS.
		if (reg == 0x2c46 || (offset == 7 && stage != 1)) {
			uint32_t mask = stage <= 1 ? 0x03ffffff : stage == 4 ? 0xffff : 0xffffffff;
			return (value & ~mask) == 0; // RSRC3 CU/resource controls.
		}
		if (reg == 0x2c7c || reg == 0x2cbc) return (value & ~0x1ff000bfu) == 0;
		if (reg == 0x2c7d || reg == 0x2cfd || reg == 0x2d3d)
			return (value & ~0x0000ffbfu) == 0;
		return false;
	}
	if (reg >= 0x2e40 && reg <= 0x2e4f) return true; // Compute user SGPRs.
	if (reg >= 0x2e04 && reg <= 0x2e09) return true; // Start/thread dimensions.
	if (reg == 0x2e0a) return (value & ~1u) == 0; // Pipeline stats, not perf.
	if (reg == 0x2e0c) return (value & 0xf0000000) == 0;
	if (reg == 0x2e0d) return value == 0;
	if (reg == 0x2e12) return (value & ~0x01afffffu) == 0;
	if (reg == 0x2e13) return (value & ~0x00ff9fbfu) == 0;
	if (reg == 0x2e15) return (value & ~0x07fff3ffu) == 0; // Resource limits.
	if (reg == 0x2e18) return (value & ~0x01ffffffu) == 0; // Scratch ring size.
	if (reg == 0x2e16 || reg == 0x2e17 || reg == 0x2e19 || reg == 0x2e1a)
		return true; // Per-SE CU masks.
	// No COMPUTE_VMID, TBA/TMA, wave restore, relaunch, trace or indirect state.
	return false;
}

bool UconfigRegister(uint32_t reg, uint32_t value)
{
	switch (reg) {
		case 0xc03f: return value == 0; // CP_STRMOUT_CNTL flush status reset.
		case 0xc242: return (value & ~0x3fu) == 0; // VGT_PRIMITIVE_TYPE.
		case 0xc280: case 0xc281: return true; // Line stipple state.
		case 0xc380: return (value & 0xf0000000) == 0; // TA_CS_BC_BASE_ADDR.
		case 0xc381: return value == 0;
		default: return false;
	}
}

Result Registers(uint32_t op, const volatile uint32_t* p, uint32_t n)
{
	if (n < 2) return Result(Result::PacketLength);
	uint32_t offset = p[0] & 0xffff, index = p[0] >> 28;
	if (p[0] & 0x0fff0000) return Result(Result::Register);
	uint32_t base = op == 0x69 ? 0xa000 : op == 0x76 ? 0x2c00 : 0xc000;
	uint32_t limit = op == 0x69 ? 0x2000 : op == 0x76 ? 0x400 : 0x1000;
	if (offset >= limit || n - 1 > limit - offset) return Result(Result::Register);
	if (index && !(index == 1 && n == 2
		&& ((op == 0x69 && offset == 0x2aa) || (op == 0x79 && offset == 0x242))))
		return Result(Result::Register);
	for (uint32_t i = 1; i < n; i++) {
		uint32_t reg = base + offset + i - 1;
		bool okay = op == 0x69 ? ContextRegister(reg)
			: op == 0x76 ? ShaderRegister(reg, p[i]) : UconfigRegister(reg, p[i]);
		if (!okay) return Result(Result::Register, i + 1);
	}
	return Result();
}

bool FlushEvent(uint32_t event)
{
	switch (event) {
		case 0x407: case 0x40f: case 0x410: // CS/VS/PS partial flush.
		case 0x06: case 0x08: case 0x0a: case 0x0d:
		case 0x11: case 0x12: case 0x16: case 0x19: case 0x1a:
		case 0x1c: case 0x1d: case 0x1f: case 0x21: case 0x24:
		case 0x2a: case 0x2c: case 0x2e: case 0x31: return true;
		default: return false;
	}
}

Result Packet(uint32_t op, uint32_t flags, const volatile uint32_t* p, uint32_t n)
{
	// Predicate, reset-filter-CAM and reserved header bits cannot hide state
	// changes from this linear policy. Compute bit is only accepted where used.
	if (flags && !(flags == 2 && (op == 0x11 || op == 0x15 || op == 0x16)))
		return Result(Result::Control);
#define LENGTH(wanted) do { if (n != (wanted)) return Result(Result::PacketLength); } while (0)
#define CHECK(test) do { if (!(test)) return Result(Result::Control); } while (0)
#define MEMORY(lo, hi, bytes, align) do { if (!Address(lo, hi, bytes, align)) return Result(Result::Address); } while (0)
	switch (op) {
		case 0x10: return Result(); // NOP payload is data, not further commands.
		case 0x11: // SET_BASE: draw/dispatch argument base only.
			LENGTH(3); CHECK(p[0] == 1); MEMORY(p[1], p[2], 4, 4); break;
		case 0x12: case 0x42: LENGTH(1); CHECK(p[0] == 0); break;
		case 0x13: case 0x2f: LENGTH(1); break;
		case 0x15:
			LENGTH(4); CHECK(flags == 2 && (p[3] & ~0x00000fe7u) == 0); break;
		case 0x16:
			LENGTH(2); CHECK(flags == 2 && !(p[0] & 3)
				&& (p[1] & ~0x00000fe7u) == 0); break;
		case 0x26: LENGTH(2); MEMORY(p[0], p[1], 2, 2); break;
		case 0x27:
			LENGTH(5); MEMORY(p[1], p[2], 2, 2); CHECK(p[4] == 0); break;
		case 0x28:
			LENGTH(2); CHECK(p[0] == 0x80000000 && p[1] == 0x80000000); break;
		case 0x2a: LENGTH(1); CHECK((p[0] & ~0x0fu) == 0 && (p[0] & 3) <= 2); break;
		case 0x2d: LENGTH(2); CHECK(p[1] == 2 || p[1] == 0x42); break;
		case 0x34: { // Streamout offset load/store; no GDS or privileged destination.
			LENGTH(5); CHECK((p[0] & ~0x387u) == 0 && (p[0] & 0x80));
			if (p[0] & 1) MEMORY(p[1], p[2], 4, 4);
			else CHECK(p[1] == 0 && p[2] == 0);
			uint32_t source = (p[0] >> 1) & 3;
			if (source == 2) MEMORY(p[3], p[4], 4, 4);
			else CHECK(p[4] == 0 && (source == 0 || p[3] == 0));
			break;
		}
		case 0x24: case 0x25: // Indirect draw writes only VS user SGPRs.
			LENGTH(4); CHECK(!(p[0] & 3) && UserDataVS(p[1]) && UserDataVS(p[1] + 1)
				&& UserDataVS(p[2]) && p[3] == (op == 0x24 ? 2u : 0u)); break;
		case 0x2c: case 0x38:
			LENGTH(9); CHECK(!(p[0] & 3) && UserDataVS(p[1]) && UserDataVS(p[1] + 1)
				&& UserDataVS(p[2]) && !(p[3] & 0x3fff0000)
				&& UserDataVS(p[3] & 0xffff) && !(p[7] & 3)
				&& p[7] >= (op == 0x38 ? 20u : 16u)
				&& p[8] == (op == 0x38 ? 0u : 2u));
			if (p[3] & (1u << 30)) MEMORY(p[5], p[6], 4, 4);
			else CHECK(p[5] == 0 && p[6] == 0);
			break;
		case 0x37: // WRITE_DATA to memory only, with incrementing addresses.
			if (n < 4) return Result(Result::PacketLength);
			CHECK(!(p[0] & ~0x40100f00u) && ((p[0] >> 8 & 15) == 5
				|| (p[0] >> 8 & 15) == 2));
			MEMORY(p[1], p[2], uint64_t(n - 3) * 4, 4); break;
		case 0x3c: { // WAIT_REG_MEM: own memory or streamout completion only.
			LENGTH(6); CHECK(!(p[0] & ~0x117u) && (p[0] & 7) >= 3
				&& (p[0] & 7) <= 5 && p[5] <= 0xffff);
			if (p[0] & 0x10) MEMORY(p[1], p[2], 4, 4);
			else CHECK(p[0] == 3 && p[1] == 0xc03f && p[2] == 0
				&& p[3] == 1 && p[4] == 1);
			break;
		}
		case 0x40: {
			LENGTH(5); CHECK(!(p[0] & ~0x40110f0fu));
			uint32_t src = p[0] & 15, dst = p[0] >> 8 & 15;
			uint32_t bytes = (p[0] & (1u << 16)) ? 8 : 4;
			CHECK(dst == 2 || dst == 5);
			if (src == 1 || src == 2) MEMORY(p[1], p[2], bytes, bytes);
			else CHECK(src == 5 || (src == 9 && p[1] == 0 && p[2] == 0));
			MEMORY(p[3], p[4], bytes, bytes); break;
		}
		case 0x43: // Surface cache flush, not register/memory load.
			LENGTH(4); CHECK(!(p[0] & ~0xfeecffcfu) && p[3] <= 0xffff);
			if (p[1] != 0xffffffff || p[2] != 0)
				MEMORY(p[2] << 8, p[2] >> 24, uint64_t(p[1]) << 8, 256);
			break;
		case 0x46:
			if (n == 1) CHECK(FlushEvent(p[0]));
			else {
				LENGTH(3); CHECK(p[0] == 0x115 || p[0] == 0x21e || p[0] == 0x320
					|| p[0] == 0x301 || p[0] == 0x302 || p[0] == 0x303);
				MEMORY(p[1], p[2], 8, 8);
			}
			break;
		case 0x47: {
			LENGTH(5); CHECK(!(p[0] & ~0x0003b73fu) && (p[0] & 0x700) == 0x500);
			uint32_t event = p[0] & 0x3f;
			CHECK(event == 4 || event == 20 || event == 40 || event == 41 || event == 43 || event == 45);
			// No client interrupt generation or GDS reads. Only memory/L2 writes.
			CHECK(!(p[2] & ~0x6001000fu));
			uint32_t data = p[2] >> 29;
			if (data) MEMORY(p[1], p[2] & 15, data == 1 ? 4 : 8, data == 1 ? 4 : 8);
			else CHECK((p[1] & 3) == 0);
			break;
		}
		case 0x50: {
			LENGTH(6); CHECK(!(p[0] & ~0xe6306001u) && !(p[5] & ~0x403fffffu));
			uint32_t src = p[0] >> 29 & 3, dst = p[0] >> 20 & 3;
			CHECK(src != 1 && (dst == 0 || dst == 3)); // No GDS/nowhere target.
			uint32_t bytes = p[5] & 0x1fffff;
			if (bytes) {
				MEMORY(p[3], p[4], bytes, 1);
				if (src == 2) CHECK(p[2] == 0); // Immediate fill.
				else MEMORY(p[1], p[2], bytes, 1);
			} else CHECK(p[1] == 0 && p[2] == 0 && p[3] == 0 && p[4] == 0);
			break;
		}
		case 0x69: case 0x76: case 0x79: return Registers(op, p, n);
		default: return Result(Result::Opcode);
	}
#undef LENGTH
#undef CHECK
#undef MEMORY
	return Result();
}
}

GfxCommandValidation
ValidateGfx8Commands(const volatile uint32_t* words, uint32_t count)
{
	if (words == 0 || count == 0 || count > 16384) return Result(Result::BadSize);
	uint32_t offset = 0;
	while (offset < count) {
		uint32_t header = words[offset];
		if (header == 0x80000000 || header == 0xffff1000) { offset++; continue; }
		if (header >> 30 != 3) return Result(Result::PacketType, offset);
		uint32_t n = ((header >> 16) & 0x3fff) + 1;
		if (n > count - offset - 1) return Result(Result::PacketLength, offset);
		Result result = Packet((header >> 8) & 255, header & 255, words + offset + 1, n);
		if (result.error != Result::OK) {
			result.dword += offset;
			return result;
		}
		offset += n + 1;
	}
	return Result();
}
}
