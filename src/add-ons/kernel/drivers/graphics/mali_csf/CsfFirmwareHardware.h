/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#ifndef MALI_CSF_FIRMWARE_HARDWARE_H
#define MALI_CSF_FIRMWARE_HARDWARE_H

#include "CsfHardware.h"

class FirmwareHardware : public ResetHardware {
public:
	explicit FirmwareHardware(bool commands = false) : fCommands(commands)
	{
		fJobCondition.Init(this, "Mali CSF job interrupt");
	}
	~FirmwareHardware() { StopFirmwareHandlers(); }
	status_t Init(const ResourceInfo& resources)
	{
		fGpuBase = resources.gpuBase;
		for (unsigned i = 0; i < 3; i++) {
			fInterrupts[i].hardware = this;
			fInterrupts[i].index = i;
			fInterrupts[i].number = resources.interrupts[i];
		}
		return ResetHardware::Init(resources);
	}
	bool MapGpu()
	{
		if (!ResetHardware::MapGpu())
			return false;
		fDoorbellArea.SetTo(map_physical_memory("Mali global doorbell",
			fGpuBase + 0x80000, B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&fDoorbell));
		if (fDoorbellArea.Get() < B_OK) {
			ResetHardware::UnmapGpu();
			return false;
		}
		return true;
	}
	void UnmapGpu()
	{
		StopFirmwareHandlers();
		fDoorbellArea.SetTo(-1);
		fDoorbell = NULL;
		ResetHardware::UnmapGpu();
	}
	void MemoryBarrier() { memory_full_barrier(); }
	uint32 TimerRate()
	{
#if defined(__aarch64__)
		uint64 rate;
		asm volatile("mrs %0, cntfrq_el0" : "=r"(rate));
		return rate <= UINT32_MAX ? uint32(rate) : 0;
#else
		return 0;
#endif
	}
	void RingFirmwareDoorbell() { WritePlatformRegister(fDoorbell, 0, 1); }
	bool InstallFirmwareHandlers()
	{
		for (unsigned i = 0; i < 3; i++) {
			InterruptSource& source = fInterrupts[i];
			if (install_io_interrupt_handler(source.number, Interrupt, &source, 0) != B_OK)
				return false;
			source.installed = true;
		}
		return true;
	}
	bool ArmFirmwareInterrupts()
	{
		InterruptsSpinLocker locker(fLock);
		// An unhandled old job/MMU event is a failed admission, never a boot.
		if (ReadGpu(0x1000) != 0 || (ReadGpu(0x2000) & ~(Mask(1) << 16)) != 0
			|| (ReadGpu(kGpuRaw) & 3) != 0)
			return false;
		for (unsigned i = 0; i < 3; i++) {
			fInterrupts[i].capture = {};
			fInterrupts[i].armed = true;
			WriteGpu(MaskOffset(i), Mask(i));
			if (ReadGpu(MaskOffset(i)) != Mask(i))
				return false;
		}
		return true;
	}
	bool ArmFirmwareJob()
	{
		return ArmJob(kGlobalInterrupt, false);
	}
	bool ArmCommandJob()
	{
		return fCommands && ArmJob(kGlobalInterrupt | 1, true);
	}
	uint32 CommandJobCount()
	{
		InterruptsSpinLocker locker(fLock);
		return fGroupJobs;
	}
	/*!	Whether the job interrupt counts itself in JobInterrupts() and wakes
		WaitJobInterrupt() (#182). Off, the handler does what it always did.
		Set before the handlers are installed. */
	void SetJobNotification(bool enabled) { fNotifyJobs = enabled; }
	uint32 JobInterrupts() { return (uint32)atomic_get(&fJobInterrupts); }
	//! True if the job interrupt came, now or since \a seen was read.
	bool WaitJobInterrupt(uint32 seen, int64_t micros)
	{
		ConditionVariableEntry entry;
		fJobCondition.Add(&entry);
		if (JobInterrupts() != seen)
			return true;
		return entry.Wait(B_RELATIVE_TIMEOUT, micros) == B_OK;
	}
	bool ArmJob(uint32 mask, bool continuous)
	{
		InterruptsSpinLocker locker(fLock);
		if (ReadGpu(0x1000) != 0 || ReadGpu(0x1008) != 0)
			return false;
		fInterrupts[0].capture = {};
		fInterrupts[0].armed = true;
		fJobMask = mask;
		fContinuousJobs = continuous;
		fGroupJobs = 0;
		WriteGpu(0x1008, mask);
		return ReadGpu(0x1008) == mask;
	}
	FirmwareCapture ReadFirmwareCapture(unsigned index)
	{
		InterruptsSpinLocker locker(fLock);
		FirmwareCapture capture = fInterrupts[index].capture;
		if (index > 0 && capture.count == 0) {
			uint32 raw = ReadGpu(index == 1 ? 0x2000 : kGpuRaw);
			if ((raw & (index == 1 ? kMmuFaultBits : 3)) != 0) {
				// Also preserve a fault that precedes IRQ arming. count=0 and
				// cpu=-1 explicitly distinguish polling from IRQ delivery.
				capture.raw = raw;
				capture.cpu = -1;
				capture.whenMicros = system_time();
				uint32 base = FaultBase(raw);
				capture.deviceStatus = ReadGpu(index == 1 ? base + 0x1c : 0x3c);
				capture.address = ReadGpu64(*this, index == 1 ? base + 0x20 : 0x40);
				if (index == 1)
					capture.extra = ReadGpu64(*this, base + 0x38);
			}
		}
		return capture;
	}
	bool FirmwareFaulted()
	{
		InterruptsSpinLocker locker(fLock);
		return fInterrupts[1].capture.count != 0 || fInterrupts[2].capture.count != 0
			|| (ReadGpu(0x2000) & kMmuFaultBits) != 0 || (ReadGpu(kGpuRaw) & 3) != 0;
	}
	void StopFirmwareHandlers()
	{
		bool installed = false;
		for (unsigned i = 0; i < 3; i++)
			installed |= fInterrupts[i].installed;
		if (!installed)
			return;
		{
			InterruptsSpinLocker locker(fLock);
			for (unsigned i = 0; i < 3; i++) {
				WriteGpu(MaskOffset(i), 0);
				fInterrupts[i].armed = false;
			}
		}
		for (unsigned i = 0; i < 3; i++) {
			InterruptSource& source = fInterrupts[i];
			if (source.installed) {
				remove_io_interrupt_handler(source.number, Interrupt, &source);
				source.installed = false;
			}
		}
	}
private:
	struct InterruptSource {
		FirmwareHardware* hardware;
		uint32 number;
		unsigned index;
		bool installed;
		bool armed;
		FirmwareCapture capture;
	};
	static uint32 MaskOffset(unsigned i) { return i == 0 ? 0x1008 : i == 1 ? 0x2008 : kGpuMask; }
	uint32 Mask(unsigned i) const { return i == 0 ? fJobMask : i == 1 ? (fCommands ? 3 : 1) : 3; }
	static uint32 FaultBase(uint32 raw)
	{
		for (unsigned as = 0; as < 16; as++) {
			if ((raw & (1u << as)) != 0)
				return 0x2400 + as * 0x40;
		}
		return 0x2400;
	}
	static int32 Interrupt(void* cookie)
	{
		InterruptSource& source = *(InterruptSource*)cookie;
		FirmwareHardware& io = *source.hardware;
		InterruptsSpinLocker locker(io.fLock);
		uint32 mask = MaskOffset(source.index);
		if (!source.armed)
			return B_UNHANDLED_INTERRUPT;
		uint32 status = io.ReadGpu(mask + 4);
		if (status == 0)
			return B_UNHANDLED_INTERRUPT;
		FirmwareCapture& capture = source.capture;
		capture.count++;
		capture.status |= status;
		capture.raw |= io.ReadGpu(mask - 8);
		capture.cpu = smp_get_current_cpu();
		capture.whenMicros = system_time();
		if (source.index == 1) {
			uint32 base = FaultBase(capture.raw);
			capture.deviceStatus = io.ReadGpu(base + 0x1c);
			capture.address = ReadGpu64(io, base + 0x20);
			capture.extra = ReadGpu64(io, base + 0x38);
		} else if (source.index == 2) {
			capture.deviceStatus = io.ReadGpu(0x3c);
			capture.address = ReadGpu64(io, 0x40);
		}
		if (source.index == 0 && io.fContinuousJobs) {
			if ((status & 1) != 0)
				io.fGroupJobs++;
		} else {
			io.WriteGpu(mask, 0);
			source.armed = false;
		}
		io.WriteGpu(mask - 4, status & io.Mask(source.index));
		if (source.index == 0 && io.fNotifyJobs) {
			// The waiter reads the count before it looks at the queue, so
			// counting before the notification cannot lose a wake-up. No
			// logging here: printing from an interrupt handler stalled the
			// ROCK 5 before (the GICv3 ITS trace).
			atomic_add(&io.fJobInterrupts, 1);
			locker.Unlock();
			io.fJobCondition.NotifyAll();
		}
		return B_HANDLED_INTERRUPT;
	}
	spinlock fLock = B_SPINLOCK_INITIALIZER;
	InterruptSource fInterrupts[3] = {};
	uint64 fGpuBase = 0;
	AreaDeleter fDoorbellArea;
	volatile uint32* fDoorbell = NULL;
	bool fCommands = false;
	bool fContinuousJobs = false;
	uint32 fJobMask = kGlobalInterrupt;
	uint32 fGroupJobs = 0;
	bool fNotifyJobs = false;
	int32 fJobInterrupts = 0;
	ConditionVariable fJobCondition;
};

#endif
