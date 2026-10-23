/*
 * Copyright 2019 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#include <arch/real_time_clock.h>

#include <real_time_clock.h>
#include <real_time_data.h>
#include <smp.h>

#include <libfdt.h>


extern void* gFDT;


// No real-time clock driver: the boot loader passes the firmware's clock,
// which then runs on from the system time.
static uint64 sClockSeconds;
static bigtime_t sClockSystemTime;


status_t
arch_rtc_init(kernel_args *args, struct real_time_data *data)
{
	if (gFDT == NULL || sClockSeconds != 0)
		return B_OK;

	int chosen = fdt_path_offset(gFDT, "/chosen");
	int length;
	const void* clock = chosen >= 0
		? fdt_getprop(gFDT, chosen, "haiku,firmware-clock", &length) : NULL;
	if (clock != NULL && length == sizeof(uint64)) {
		sClockSeconds = fdt64_to_cpu(*(const fdt64_t*)clock);
		sClockSystemTime = system_time();
	}
	return B_OK;
}


uint64
arch_rtc_get_hw_time(void)
{
	if (sClockSeconds == 0)
		return 0;
	return sClockSeconds + (system_time() - sClockSystemTime) / 1000000;
}


void
arch_rtc_set_hw_time(uint64 seconds)
{
	sClockSeconds = seconds;
	sClockSystemTime = system_time();
}


void
arch_rtc_set_system_time_offset(struct real_time_data *data, bigtime_t offset)
{
	atomic_set64(&data->arch_data.system_time_offset, offset);
}


bigtime_t
arch_rtc_get_system_time_offset(struct real_time_data *data)
{
	return atomic_get64(&data->arch_data.system_time_offset);
}
