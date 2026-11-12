#include "soc.h"

#include <debug.h>
#include <driver_settings.h>


InterruptController *InterruptController::sInstance = NULL;
HardwareTimer *HardwareTimer::sInstance = NULL;


void
InterruptController::LoadRoutingSetting()
{
	void* settings = load_driver_settings("kernel");
	fRoutingEnabled = get_driver_boolean_parameter(settings, "irq_routing",
		true, true);
	unload_driver_settings(settings);
	if (!fRoutingEnabled)
		dprintf("irq routing: off (kernel settings), shared interrupts stay on CPU 0\n");
}


/*static*/ int32
InterruptController::UsableCpu(int32 cpu)
{
	int32 count = smp_get_num_cpus();
	if (cpu < 0 || cpu >= count)
		cpu = 0;
	if (!gCPU[cpu].disabled)
		return cpu;
	for (int32 i = 0; i < count; i++) {
		if (!gCPU[i].disabled)
			return i;
	}
	return cpu;
}


void
InterruptController::DumpDeviceInterrupts()
{
	kprintf("device interrupts taken per CPU:");
	for (int32 i = 0; i < smp_get_num_cpus(); i++)
		kprintf(" cpu%" B_PRId32 " %" B_PRId64, i, fDeviceInterrupts[i].count);
	kprintf("\nshared interrupts routed to another CPU: %" B_PRId32 " while disabled, %"
		B_PRId32 " while enabled\n", fMoves, fEnabledMoves);
}
