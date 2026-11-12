#include <SupportDefs.h>
enum interrupt_trigger_mode { B_EDGE_TRIGGERED = 1, B_LEVEL_TRIGGERED = 2, B_LOW_ACTIVE_POLARITY = 4, B_HIGH_ACTIVE_POLARITY = 8 };
void arch_int_configure_io_interrupt(int32 irq, int trigger, int polarity);
