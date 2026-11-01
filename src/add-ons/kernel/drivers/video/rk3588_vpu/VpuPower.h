/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef RK3588_VPU_POWER_H
#define RK3588_VPU_POWER_H

#include "VpuInterface.h"

namespace RK3588Vpu {

static const uint32_t kVdpuGate = 1u << 10;
static const uint32_t kVdpuRepair = 1u << 9;
static const uint32_t kVdpuIdle = 1u << 8;
static const uint32_t kRkvdec0Gate = 1u << 8;
static const uint32_t kRkvdec0Repair = 1u << 7;
static const uint32_t kRkvdec0Idle = 1u << 6;
static const uint32_t kRkvdec0Memory = 1u << 16;
static const uint32_t kAv1Gate = 1u << 12;
static const uint32_t kAv1Repair = 1u << 11;
static const uint32_t kAv1Idle = 1u << 9;
static const uint32_t kAv1Memory = 1u << 20;
// VENC0 (RKVENC core 0), a child of VCODEC. Linux 6.18.52 pm-domains.c:
// power bit 6, memory status 14, repair status 5, idle request/ack 4.
static const uint32_t kVenc0Gate = 1u << 6;
static const uint32_t kVenc0Repair = 1u << 5;
static const uint32_t kVenc0Idle = 1u << 4;
static const uint32_t kVenc0Memory = 1u << 14;
static const uint32_t kVcodecGate = 1u << 2;

// PMU hiword write-mask format. Sources: RK3588 TRM v1.0 Part 1 and
// Linux 6.18.52 drivers/pmdomain/rockchip/pm-domains.c.
static const uint32_t kPowerGateOffset = 0x14c;
static const uint32_t kIdleRequestOffset = 0x10c;

inline bool
InitialStateMatches(const Snapshot& state)
{
	return state.version == kSnapshotVersion && state.flags == 1
		&& (state.powerGate & kVdpuGate) != 0
		&& (state.powerStatus & kVdpuGate) != 0
		&& (state.repairStatus & kVdpuRepair) == 0
		&& (state.repairStatus & ((1u << 7) | (1u << 8) | (1u << 11))) == 0
		&& (state.idleRequest & kVdpuIdle) == 0
		&& (state.idleAck & kVdpuIdle) != 0
		&& (state.idleStatus & kVdpuIdle) != 0
		&& state.clockSelect98 == 0x21
		&& state.clockGate44 == 0 && state.clockGate45 == 0;
}

inline bool
RestoredStateMatches(const Snapshot& before, const Snapshot& after)
{
	return (after.powerGate & kVdpuGate) == (before.powerGate & kVdpuGate)
		&& (after.powerStatus & kVdpuGate) == (before.powerStatus & kVdpuGate)
		&& (after.repairStatus & kVdpuRepair) == (before.repairStatus & kVdpuRepair)
		&& (after.idleRequest & kVdpuIdle) == (before.idleRequest & kVdpuIdle)
		&& (after.idleAck & kVdpuIdle) == (before.idleAck & kVdpuIdle)
		&& (after.idleStatus & kVdpuIdle) == (before.idleStatus & kVdpuIdle)
		&& after.clockSelect98 == before.clockSelect98
		&& after.clockSelect89 == before.clockSelect89
		&& after.clockSelect90 == before.clockSelect90
		&& after.clockSelect91 == before.clockSelect91
		&& after.clockSelect163 == before.clockSelect163
		&& after.clockGate40 == before.clockGate40
		&& after.clockGate41 == before.clockGate41
		&& after.clockGate44 == before.clockGate44
		&& after.clockGate45 == before.clockGate45
		&& after.clockGate68 == before.clockGate68
		&& after.softReset68 == before.softReset68;
}

// The first half of a power cycle: from the state the firmware leaves to
// powered. False if the domain did not come up; PowerOffVdpu() then puts back
// whatever was touched.
template<class Hardware>
bool
PowerOnVdpu(Hardware& hardware, PowerCycle& result)
{
	result = PowerCycle();
	result.version = kPowerCycleVersion;
	result.result = kPowerInvalidState;
	result.restoreResult = kPowerOK;
	result.before = hardware.SnapshotNow();
	if (!InitialStateMatches(result.before)) {
		result.after = result.before;
		return false;
	}

	// All VDPU parent-domain clocks are ungated in the admitted firmware state.
	// No CRU word is written. Clear bit 10 to request power on.
	hardware.WritePower(kPowerGateOffset, kVdpuGate << 16);
	result.flags |= 1;
	if (!hardware.WaitPower(kVdpuRepair, true)) {
		result.result = kPowerOnTimeout;
		return false;
	}

	// Leave NIU idle after the domain reports power and memory repair.
	hardware.WritePower(kIdleRequestOffset, kVdpuIdle << 16);
	result.flags |= 2;
	if (!hardware.WaitIdle(kVdpuIdle, false)) {
		result.result = kPowerIdleTimeout;
		return false;
	}
	result.powered = hardware.SnapshotNow();
	result.flags |= 4;
	result.result = kPowerOK;
	return true;
}


// The second half: back to the firmware's state, checked against the
// snapshot PowerOnVdpu() took. Nothing happens if PowerOnVdpu() refused the
// state it found.
template<class Hardware>
void
PowerOffVdpu(Hardware& hardware, PowerCycle& result)
{
	if ((result.flags & 1) == 0)
		return;
	// A timeout may still leave the domain powered. Request NIU idle before
	// switching it off, then restore the original idle-request bit.
	if (hardware.PowerIsOn(kVdpuRepair)) {
		hardware.WritePower(kIdleRequestOffset,
			(kVdpuIdle << 16) | kVdpuIdle);
		if (!hardware.WaitIdle(kVdpuIdle, true))
			result.restoreResult = kPowerIdleTimeout;
	}
	hardware.WritePower(kPowerGateOffset, (kVdpuGate << 16) | kVdpuGate);
	if (!hardware.WaitPower(kVdpuRepair, false))
		result.restoreResult = kPowerOffTimeout;
	hardware.WritePower(kIdleRequestOffset, kVdpuIdle << 16);
	result.after = hardware.SnapshotNow();
	if (result.restoreResult == kPowerOK
		&& !RestoredStateMatches(result.before, result.after)) {
		result.restoreResult = kPowerRestoreMismatch;
	}
}


// Hardware supplies SnapshotNow(), WritePower(offset, hiwordValue), and
// WaitPower(mask, set) / WaitIdle(mask, set). Each wait is time bounded.
template<class Hardware, class Probe>
void
CycleVdpuPower(Hardware& hardware, PowerCycle& output, Probe probe)
{
	PowerCycle result;
	if (PowerOnVdpu(hardware, result))
		probe();
	PowerOffVdpu(hardware, result);
	output = result;
}

template<class Hardware>
void
CycleVdpuPower(Hardware& hardware, PowerCycle& output)
{
	CycleVdpuPower(hardware, output, [] {});
}

inline bool
InitialRkvdec0StateMatches(const Snapshot& state)
{
	return state.version == kSnapshotVersion && state.flags == 1
		&& (state.powerGate & kRkvdec0Gate) != 0
		&& (state.powerStatus & kRkvdec0Gate) != 0
		&& (state.repairStatus & kRkvdec0Repair) == 0
		&& (state.memoryStatus & kRkvdec0Memory) != 0
		&& (state.idleRequest & kRkvdec0Idle) == 0
		&& (state.idleAck & kRkvdec0Idle) != 0
		&& (state.idleStatus & kRkvdec0Idle) != 0
		&& (state.powerGate & (1u << 2)) == 0
		&& (state.powerStatus & (1u << 2)) == 0
		// Linux models RKVDEC0 under both VCODEC and VDPU. VCODEC has
		// no repair-status bit; its power gate/status above indicate on.
		&& (state.repairStatus & kVdpuRepair) != 0
		&& (state.powerStatus & kVdpuGate) == 0
		&& state.clockGate40 == 0
		&& state.clockSelect89 == 0x8100
		&& state.clockSelect90 == 0x1801
		&& state.clockSelect91 == 0x1;
}

inline bool
RestoredRkvdec0StateMatches(const Snapshot& before, const Snapshot& after)
{
	return (after.powerGate & kRkvdec0Gate)
			== (before.powerGate & kRkvdec0Gate)
		&& (after.powerStatus & kRkvdec0Gate)
			== (before.powerStatus & kRkvdec0Gate)
		&& (after.repairStatus & kRkvdec0Repair)
			== (before.repairStatus & kRkvdec0Repair)
		&& (after.memoryStatus & kRkvdec0Memory)
			== (before.memoryStatus & kRkvdec0Memory)
		&& (after.idleRequest & kRkvdec0Idle)
			== (before.idleRequest & kRkvdec0Idle)
		&& (after.idleAck & kRkvdec0Idle)
			== (before.idleAck & kRkvdec0Idle)
		&& (after.idleStatus & kRkvdec0Idle)
			== (before.idleStatus & kRkvdec0Idle)
		&& after.clockGate40 == before.clockGate40
		&& after.clockSelect89 == before.clockSelect89
		&& after.clockSelect90 == before.clockSelect90
		&& after.clockSelect91 == before.clockSelect91
		&& after.clockSelect163 == before.clockSelect163
		&& after.softReset68 == before.softReset68;
}

template<class Hardware>
bool
PowerOnRkvdec0(Hardware& hardware, PowerCycle& result)
{
	result = PowerCycle();
	result.version = kPowerCycleVersion;
	result.result = kPowerInvalidState;
	result.restoreResult = kPowerOK;
	result.before = hardware.SnapshotNow();
	if (!InitialRkvdec0StateMatches(result.before)) {
		result.after = result.before;
		return false;
	}

	hardware.WritePower(kPowerGateOffset, kRkvdec0Gate << 16);
	result.flags |= 1;
	if (!hardware.WaitRkvdec0Power(true)) {
		result.result = kPowerOnTimeout;
		return false;
	}
	if (!hardware.WaitRkvdec0Memory(true)) {
		result.result = kPowerMemoryTimeout;
		return false;
	}
	hardware.WritePower(kIdleRequestOffset, kRkvdec0Idle << 16);
	result.flags |= 2;
	if (!hardware.WaitIdle(kRkvdec0Idle, false)) {
		result.result = kPowerIdleTimeout;
		return false;
	}
	result.powered = hardware.SnapshotNow();
	result.flags |= 4;
	result.result = kPowerOK;
	return true;
}


template<class Hardware>
void
PowerOffRkvdec0(Hardware& hardware, PowerCycle& result)
{
	if ((result.flags & 1) == 0)
		return;
	if (hardware.PowerIsOn(kRkvdec0Repair)) {
		hardware.WritePower(kIdleRequestOffset,
			(kRkvdec0Idle << 16) | kRkvdec0Idle);
		if (!hardware.WaitIdle(kRkvdec0Idle, true))
			result.restoreResult = kPowerIdleTimeout;
	}
	hardware.WritePower(kPowerGateOffset,
		(kRkvdec0Gate << 16) | kRkvdec0Gate);
	if (!hardware.WaitRkvdec0Power(false))
		result.restoreResult = kPowerOffTimeout;
	if (!hardware.WaitRkvdec0Memory(false))
		result.restoreResult = kPowerMemoryTimeout;
	hardware.WritePower(kIdleRequestOffset, kRkvdec0Idle << 16);
	result.after = hardware.SnapshotNow();
	if (result.restoreResult == kPowerOK
		&& !RestoredRkvdec0StateMatches(result.before, result.after)) {
		result.restoreResult = kPowerRestoreMismatch;
	}
}


// Core 0 is missing from this EDK2 FDT. This diagnostic is restricted to the
// validated Rock 5 ITX board, exact observed clock state and an opt-in setting.
// The register and PMU layout is from Linux mainline 75f2c0b36907.
template<class Hardware, class Probe>
void
CycleRkvdec0Power(Hardware& hardware, PowerCycle& output, Probe probe)
{
	PowerCycle result;
	if (PowerOnRkvdec0(hardware, result))
		probe();
	PowerOffRkvdec0(hardware, result);
	output = result;
}

template<class Hardware>
void
CycleRkvdec0Power(Hardware& hardware, PowerCycle& output)
{
	CycleRkvdec0Power(hardware, output, [] {});
}

inline bool
InitialAv1StateMatches(const Snapshot& state)
{
	return state.version == kSnapshotVersion && state.flags == 1
		&& (state.powerGate & kAv1Gate) != 0
		&& (state.powerStatus & kAv1Gate) != 0
		&& (state.repairStatus & kAv1Repair) == 0
		&& (state.memoryStatus & kAv1Memory) != 0
		&& (state.idleRequest & kAv1Idle) == 0
		&& (state.idleAck & kAv1Idle) != 0
		&& (state.idleStatus & kAv1Idle) != 0
		&& (state.repairStatus & kVdpuRepair) != 0
		&& (state.powerStatus & kVdpuGate) == 0
		&& state.clockSelect163 == 2
		&& state.clockGate68 == 0
		&& (state.softReset68 & 0x36) == 0;
}

inline bool
RestoredAv1StateMatches(const Snapshot& before, const Snapshot& after)
{
	return (after.powerGate & kAv1Gate) == (before.powerGate & kAv1Gate)
		&& (after.powerStatus & kAv1Gate) == (before.powerStatus & kAv1Gate)
		&& (after.repairStatus & kAv1Repair)
			== (before.repairStatus & kAv1Repair)
		&& (after.memoryStatus & kAv1Memory)
			== (before.memoryStatus & kAv1Memory)
		&& (after.idleRequest & kAv1Idle) == (before.idleRequest & kAv1Idle)
		&& (after.idleAck & kAv1Idle) == (before.idleAck & kAv1Idle)
		&& (after.idleStatus & kAv1Idle) == (before.idleStatus & kAv1Idle)
		&& after.clockSelect163 == before.clockSelect163
		&& after.clockGate68 == before.clockGate68
		&& after.softReset68 == before.softReset68;
}

template<class Hardware>
bool
PowerOnAv1(Hardware& hardware, PowerCycle& result)
{
	result = PowerCycle();
	result.version = kPowerCycleVersion;
	result.result = kPowerInvalidState;
	result.restoreResult = kPowerOK;
	result.before = hardware.SnapshotNow();
	if (!InitialAv1StateMatches(result.before)) {
		result.after = result.before;
		return false;
	}

	hardware.WritePower(kPowerGateOffset, kAv1Gate << 16);
	result.flags |= 1;
	if (!hardware.WaitAv1Power(true)) {
		result.result = kPowerOnTimeout;
		return false;
	}
	if (!hardware.WaitAv1Memory(true)) {
		result.result = kPowerMemoryTimeout;
		return false;
	}
	hardware.WritePower(kIdleRequestOffset, kAv1Idle << 16);
	result.flags |= 2;
	if (!hardware.WaitIdle(kAv1Idle, false)) {
		result.result = kPowerIdleTimeout;
		return false;
	}
	result.powered = hardware.SnapshotNow();
	result.flags |= 4;
	result.result = kPowerOK;
	return true;
}


template<class Hardware>
void
PowerOffAv1(Hardware& hardware, PowerCycle& result)
{
	if ((result.flags & 1) == 0)
		return;
	if (hardware.PowerIsOn(kAv1Repair)) {
		hardware.WritePower(kIdleRequestOffset, (kAv1Idle << 16) | kAv1Idle);
		if (!hardware.WaitIdle(kAv1Idle, true))
			result.restoreResult = kPowerIdleTimeout;
	}
	hardware.WritePower(kPowerGateOffset, (kAv1Gate << 16) | kAv1Gate);
	if (!hardware.WaitAv1Power(false))
		result.restoreResult = kPowerOffTimeout;
	if (!hardware.WaitAv1Memory(false))
		result.restoreResult = kPowerMemoryTimeout;
	hardware.WritePower(kIdleRequestOffset, kAv1Idle << 16);
	result.after = hardware.SnapshotNow();
	if (result.restoreResult == kPowerOK
		&& !RestoredAv1StateMatches(result.before, result.after)) {
		result.restoreResult = kPowerRestoreMismatch;
	}
}


template<class Hardware, class Probe>
void
CycleAv1Power(Hardware& hardware, PowerCycle& output, Probe probe)
{
	PowerCycle result;
	if (PowerOnAv1(hardware, result))
		probe();
	PowerOffAv1(hardware, result);
	output = result;
}

template<class Hardware>
void
CycleAv1Power(Hardware& hardware, PowerCycle& output)
{
	CycleAv1Power(hardware, output, [] {});
}

inline bool
InitialVenc0StateMatches(const Snapshot& state)
{
	return state.version == kSnapshotVersion && state.flags == 1
		&& (state.powerGate & kVenc0Gate) != 0
		&& (state.powerStatus & kVenc0Gate) != 0
		&& (state.repairStatus & kVenc0Repair) == 0
		&& (state.memoryStatus & kVenc0Memory) != 0
		&& (state.idleRequest & kVenc0Idle) == 0
		&& (state.idleAck & kVenc0Idle) != 0
		&& (state.idleStatus & kVenc0Idle) != 0
		// VCODEC, the parent, is left on by the firmware (no repair bit).
		&& (state.powerGate & kVcodecGate) == 0
		&& (state.powerStatus & kVcodecGate) == 0;
}

inline bool
RestoredVenc0StateMatches(const Snapshot& before, const Snapshot& after)
{
	return (after.powerGate & kVenc0Gate) == (before.powerGate & kVenc0Gate)
		&& (after.powerStatus & kVenc0Gate) == (before.powerStatus & kVenc0Gate)
		&& (after.repairStatus & kVenc0Repair)
			== (before.repairStatus & kVenc0Repair)
		&& (after.memoryStatus & kVenc0Memory)
			== (before.memoryStatus & kVenc0Memory)
		&& (after.idleRequest & kVenc0Idle) == (before.idleRequest & kVenc0Idle)
		&& (after.idleAck & kVenc0Idle) == (before.idleAck & kVenc0Idle)
		&& (after.idleStatus & kVenc0Idle) == (before.idleStatus & kVenc0Idle);
}

// The same halves as RKVDEC0's for VENC0. The firmware leaves its clocks
// (CRU gate 47) ungated and out of reset; the driver checks that first.
template<class Hardware>
bool
PowerOnVenc0(Hardware& hardware, PowerCycle& result)
{
	result = PowerCycle();
	result.version = kPowerCycleVersion;
	result.result = kPowerInvalidState;
	result.restoreResult = kPowerOK;
	result.before = hardware.SnapshotNow();
	if (!InitialVenc0StateMatches(result.before)) {
		result.after = result.before;
		return false;
	}

	hardware.WritePower(kPowerGateOffset, kVenc0Gate << 16);
	result.flags |= 1;
	if (!hardware.WaitDomainPower(kVenc0Repair, kVenc0Gate, true)) {
		result.result = kPowerOnTimeout;
		return false;
	}
	if (!hardware.WaitDomainMemory(kVenc0Memory, true)) {
		result.result = kPowerMemoryTimeout;
		return false;
	}
	hardware.WritePower(kIdleRequestOffset, kVenc0Idle << 16);
	result.flags |= 2;
	if (!hardware.WaitIdle(kVenc0Idle, false)) {
		result.result = kPowerIdleTimeout;
		return false;
	}
	result.powered = hardware.SnapshotNow();
	result.flags |= 4;
	result.result = kPowerOK;
	return true;
}


template<class Hardware>
void
PowerOffVenc0(Hardware& hardware, PowerCycle& result)
{
	if ((result.flags & 1) == 0)
		return;
	if (hardware.PowerIsOn(kVenc0Repair)) {
		hardware.WritePower(kIdleRequestOffset, (kVenc0Idle << 16) | kVenc0Idle);
		if (!hardware.WaitIdle(kVenc0Idle, true))
			result.restoreResult = kPowerIdleTimeout;
	}
	hardware.WritePower(kPowerGateOffset, (kVenc0Gate << 16) | kVenc0Gate);
	if (!hardware.WaitDomainPower(kVenc0Repair, kVenc0Gate, false))
		result.restoreResult = kPowerOffTimeout;
	if (!hardware.WaitDomainMemory(kVenc0Memory, false))
		result.restoreResult = kPowerMemoryTimeout;
	hardware.WritePower(kIdleRequestOffset, kVenc0Idle << 16);
	result.after = hardware.SnapshotNow();
	if (result.restoreResult == kPowerOK
		&& !RestoredVenc0StateMatches(result.before, result.after)) {
		result.restoreResult = kPowerRestoreMismatch;
	}
}

} // namespace RK3588Vpu

#endif
