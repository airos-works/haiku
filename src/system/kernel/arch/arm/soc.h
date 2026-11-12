#ifndef ARCH_ARM_SOC_H
#define ARCH_ARM_SOC_H

class InterruptController;

#include <cpu.h>
#include <smp.h>

// ------------------------------------------------------ InterruptController

class InterruptController {
public:
	virtual ~InterruptController() = default;

	virtual void EnableInterrupt(int32 irq) = 0;
	virtual void DisableInterrupt(int32 irq) = 0;

	virtual void HandleInterrupt() = 0;

	virtual void SendMulticastIci(CPUSet& cpuSet) {
		panic("SendMulticastIci unimplemented");
	}

	virtual void SendBroadcastIci() {
		panic("SendBroadcastIci unimplemented");
	}

	virtual void SendUnicastIci(int32 target_cpu) {
		CPUSet cpuSet;
		cpuSet.SetBit(target_cpu);
		SendMulticastIci(cpuSet);
	}

	virtual status_t PerCpuInit() {
		return B_OK;
	}

	// Routes the shared interrupt \a irq to \a cpu and returns the CPU it
	// goes to from now on: \a cpu, or the CPU it stays on where it cannot be
	// moved. Called with interrupts disabled, from install_io_interrupt_handler()
	// and the scheduler's interrupt balancing.
	virtual int32 AssignToCpu(int32 irq, int32 cpu) {
		return 0;
	}

	// Prints the hardware routing of the enabled shared interrupts and the
	// device interrupts each CPU took (kernel debugger command "irq_routes").
	virtual void DumpRouting() {}

	static InterruptController* Get() {
		return sInstance;
	}

protected:
	InterruptController()
	{
		if (sInstance) {
			panic("Multiple InterruptController objects created; that is currently unsupported!");
		}
		sInstance = this;
	}

	// "irq_routing false" in the kernel settings keeps every shared
	// interrupt on CPU 0, as before routing was implemented.
	void LoadRoutingSetting();
	bool RoutingEnabled() const { return fRoutingEnabled; }

	// \a cpu if it exists and is enabled, otherwise an enabled CPU: a
	// disabled CPU hands its interrupts back and gets no new ones.
	static int32 UsableCpu(int32 cpu);

	void CountDeviceInterrupt()
		{ fDeviceInterrupts[smp_get_current_cpu()].count++; }
	void CountMove(bool enabled)
		{ atomic_add(enabled ? &fEnabledMoves : &fMoves, 1); }
	void DumpDeviceInterrupts();

	static InterruptController *sInstance;

private:
	struct PerCpuCount {
		int64	count;
		int64	padding[7];	// a cache line per CPU
	};

	bool fRoutingEnabled = true;
	int32 fMoves = 0;
	int32 fEnabledMoves = 0;
	PerCpuCount fDeviceInterrupts[SMP_MAX_CPUS] = {};
};


// ------------------------------------------------------ HardwareTimer

class HardwareTimer {
public:
	virtual void SetTimeout(bigtime_t timeout) = 0;
	virtual bigtime_t Time() = 0;
	virtual void Clear() = 0;

	static HardwareTimer* Get() {
		return sInstance;
	}

protected:
	HardwareTimer()
	{
		if (sInstance) {
			panic("Multiple HardwareTimer objects created; that is currently unsupported!");
		}
		sInstance = this;
	}

	static HardwareTimer *sInstance;
};

#endif // ARCH_ARM_SOC_H
