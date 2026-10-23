/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Suspend to idle ("S0ix", Modern Standby) for x86 machines whose firmware
	has no S3, like the laptops with AMD's Phoenix (Ryzen 7040). The machine
	stays in S0: nothing loses power, so no state has to be saved. It sleeps
	because nothing runs: the application processors are disabled and idle
	in their deepest C-state, the boot CPU idles in a loop of its own, and
	the firmware is told through the LPS0 device's _DSM (and, on AMD, the
	PMC's "OS hint") that the system is idle and may enter its deepest state.
	The SCI ends the sleep: the lid or the power button (through the
	embedded controller), or the end of a test timeout.

	The firmware raises the SCI for its own business, too (the embedded
	controller reports the battery about once a minute). The sleep only ends
	when a driver reported a wakeup event meanwhile (a button pressed, the
	lid opened) or the lid is open; otherwise the ACPI threads get to handle
	the event and the system sleeps on, as with Linux' acpi_s2idle_wake().

	This is the platform part only: what the devices do is up to their
	drivers' suspend hooks; a device that keeps running keeps the SoC out of
	its deepest state, but not the machine from sleeping.
*/


#include <arch/x86/arch_suspend.h>

#include <stdio.h>
#include <string.h>

#include <ACPI.h>
#include <PCI.h>

#include <cpu.h>
#include <debug.h>
#include <interrupts.h>
#include <kdevice_manager.h>
#include <kernel.h>
#include <kscheduler.h>
#include <smp.h>
#include <syscalls.h>
#include <thread.h>
#include <timer.h>
#include <arch/timer.h>
#include <util/AutoLock.h>
#include <vm/vm.h>

#include <AutoDeleter.h>


#define INFO(x...) do { dprintf("s2idle: " x); \
	device_manager_suspend_trace(x); } while (false)


// The LPS0 device's _DSM: function 0 returns the mask of the others.
static const uint8 kLPS0AMD[16] = {
	// e3f32452-febc-43ce-9039-932122d37721
	0x52, 0x24, 0xf3, 0xe3, 0xbc, 0xfe, 0xce, 0x43,
	0x90, 0x39, 0x93, 0x21, 0x22, 0xd3, 0x77, 0x21
};
static const uint8 kLPS0Microsoft[16] = {
	// 11e00d56-ce64-47ce-837b-1f898f9aa461
	0x56, 0x0d, 0xe0, 0x11, 0x64, 0xce, 0xce, 0x47,
	0x83, 0x7b, 0x1f, 0x89, 0x8f, 0x9a, 0xa4, 0x61
};

enum {
	LPS0_AMD_ENTRY			= 2,
	LPS0_AMD_EXIT			= 3,
	LPS0_AMD_SCREEN_OFF		= 4,
	LPS0_AMD_SCREEN_ON		= 5,

	LPS0_MS_SCREEN_OFF		= 3,
	LPS0_MS_SCREEN_ON		= 4,
	LPS0_MS_ENTRY			= 5,
	LPS0_MS_EXIT			= 6,
	LPS0_MS_STANDBY_ENTRY	= 7,
	LPS0_MS_STANDBY_EXIT	= 8,
	LPS0_MS_TURN_ON_DISPLAY	= 9,
};

// AMD's PMC (Phoenix and its relatives: the root complex is 1022:14e8), and
// its mailbox to the power management firmware
#define AMD_ROOT_DEVICE_PHOENIX		0x14e8
#define AMD_SMN_INDEX				0x60
#define AMD_SMN_DATA				0x64
#define AMD_PMC_BASE_LO				0x13b102e8
#define AMD_PMC_BASE_HI				0x13b102ec
#define AMD_PMC_BASE_OFFSET			0x10000
#define AMD_PMC_MESSAGE				0x538
#define AMD_PMC_ARGUMENT			0x9bc
#define AMD_PMC_RESPONSE			0x980
#define AMD_PMC_IDLE_MASK			0xd14
#define AMD_PMC_MSG_OS_HINT			0x03
#define AMD_PMC_MSG_LOG_ADDRESS_HI	0x04
#define AMD_PMC_MSG_LOG_ADDRESS_LO	0x05
#define AMD_PMC_MSG_LOG_START		0x06
#define AMD_PMC_MSG_LOG_RESET		0x07
#define AMD_PMC_MSG_LOG_DUMP		0x08
#define AMD_PMC_MSG_ACTIVE_IPS		0x09
#define AMD_PMC_RESPONSE_OK			0x01

// The power management firmware's log of a sleep (Linux' struct smu_metrics):
// above all, for how long each IP block kept the SoC out of S0i3.
struct amd_pmc_metrics {
	uint32	table_version;
	uint32	hint_count;
	uint32	s0i3_last_entry_status;
	uint32	time_in_s0i2;
	uint64	time_entering_s0i3_last;
	uint64	time_entering_s0i3_total;
	uint64	time_to_resume_last;
	uint64	time_to_resume_total;
	uint64	time_in_s0i3_last;
	uint64	time_in_s0i3_total;
	uint64	time_in_sw_drips_last;
	uint64	time_in_sw_drips_total;
	uint64	time_condition_not_met_last[32];
	uint64	time_condition_not_met_total[32];
} _PACKED;

// the IP blocks the log knows on Phoenix (Linux' soc15_ip_blk)
static const char* const kAMDIPBlocks[] = {
	"DISPLAY", "CPU", "GFX", "VDD", "ACP", "VCN", "ISP", "NBIO", "DF",
	"USB3_0", "USB3_1", "LAPIC", "USB3_2", "USB3_3", "USB3_4", "USB4_0",
	"USB4_1", "MPM", "JPEG", "IPU", "UMSCH"
};

// the FCH's S0i3 entry and exit time stamps (48 MHz)
#define AMD_FCH_PM_BASE				0xfed81000
#define AMD_FCH_S0I3_ENTRY			0x130
#define AMD_FCH_S0I3_EXIT			0x138


static mutex sLock = MUTEX_INITIALIZER("s2idle");

static acpi_module_info* sACPI;
static char sLPS0Path[128];
static uint32 sLPS0MaskAMD;
static uint32 sLPS0MaskMicrosoft;
static bool sLPS0Probed;

static volatile uint8* sPMC;
static volatile uint8* sFCH;
static volatile amd_pmc_metrics* sPMCMetrics;
static uint32 sPMCActiveIPs;
static bool sPMCProbed;

static char sLidPath[128];
static bool sLidProbed;

static int32 sSCI = -1;
static int32 sActive;
static int32 sWakeReason;
static int32 sAlarmFired;


//	#pragma mark - LPS0


static status_t
lps0_call(const uint8* uuid, uint32 function, uint32* _mask)
{
	char method[sizeof(sLPS0Path) + 8];
	snprintf(method, sizeof(method), "%s._DSM", sLPS0Path);

	acpi_object_type arguments[4];
	arguments[0].buffer.object_type = ACPI_TYPE_BUFFER;
	arguments[0].buffer.length = 16;
	arguments[0].buffer.buffer = (void*)uuid;
	arguments[1].integer.object_type = ACPI_TYPE_INTEGER;
	arguments[1].integer.integer = 0;
		// (revision 0 for both, as Linux uses on AMD)
	arguments[2].integer.object_type = ACPI_TYPE_INTEGER;
	arguments[2].integer.integer = function;
	arguments[3].package.object_type = ACPI_TYPE_PACKAGE;
	arguments[3].package.count = 0;
	arguments[3].package.objects = NULL;
	acpi_objects list = { 4, arguments };

	acpi_data result = { ACPI_ALLOCATE_BUFFER, NULL };
	status_t status = sACPI->evaluate_method(NULL, method, &list, &result);
	if (status == B_OK && _mask != NULL) {
		acpi_object_type* object = (acpi_object_type*)result.pointer;
		*_mask = 0;
		if (object == NULL || object->object_type != ACPI_TYPE_BUFFER
			|| object->buffer.length < 1) {
			status = B_BAD_DATA;
		} else {
			const uint8* bytes = (const uint8*)object->buffer.buffer;
			for (uint32 i = 0; i < object->buffer.length && i < 4; i++)
				*_mask |= (uint32)bytes[i] << (i * 8);
		}
	}
	free(result.pointer);
	return status;
}


static void
lps0_probe()
{
	if (sLPS0Probed)
		return;
	sLPS0Probed = true;

	if (sACPI->get_device("PNP0D80", 0, sLPS0Path, sizeof(sLPS0Path))
			!= B_OK) {
		INFO("no LPS0 device\n");
		sLPS0Path[0] = '\0';
		return;
	}

	if (lps0_call(kLPS0AMD, 0, &sLPS0MaskAMD) != B_OK || (sLPS0MaskAMD & 1) == 0)
		sLPS0MaskAMD = 0;
	if (lps0_call(kLPS0Microsoft, 0, &sLPS0MaskMicrosoft) != B_OK
		|| (sLPS0MaskMicrosoft & 1) == 0) {
		sLPS0MaskMicrosoft = 0;
	}
	INFO("LPS0 device %s, AMD functions %#" B_PRIx32 ", Microsoft %#"
		B_PRIx32 "\n", sLPS0Path, sLPS0MaskAMD, sLPS0MaskMicrosoft);
}


static void
lps0_amd(uint32 function)
{
	if ((sLPS0MaskAMD & (1u << function)) != 0)
		lps0_call(kLPS0AMD, function, NULL);
}


static void
lps0_microsoft(uint32 function)
{
	if ((sLPS0MaskMicrosoft & (1u << function)) != 0)
		lps0_call(kLPS0Microsoft, function, NULL);
}


//	#pragma mark - AMD PMC


static uint32
pmc_read(uint32 offset)
{
	return *(volatile uint32*)(sPMC + offset);
}


static void
pmc_write(uint32 offset, uint32 value)
{
	*(volatile uint32*)(sPMC + offset) = value;
}


static status_t
pmc_wait_response(uint32* _response)
{
	bigtime_t end = system_time() + 1000000;
	while (true) {
		uint32 response = pmc_read(AMD_PMC_RESPONSE);
		if (response != 0) {
			*_response = response;
			return B_OK;
		}
		if (system_time() > end)
			return B_TIMED_OUT;
		spin(50);
	}
}


static status_t
pmc_send(uint32 message, uint32 argument, uint32* _data = NULL)
{
	if (sPMC == NULL)
		return B_NO_INIT;

	uint32 response;
	if (pmc_wait_response(&response) != B_OK)
		return B_BUSY;

	pmc_write(AMD_PMC_RESPONSE, 0);
	pmc_write(AMD_PMC_ARGUMENT, argument);
	pmc_write(AMD_PMC_MESSAGE, message);

	status_t status = pmc_wait_response(&response);
	if (status != B_OK)
		return status;
	if (response != AMD_PMC_RESPONSE_OK) {
		INFO("PMC message %#" B_PRIx32 ": response %#" B_PRIx32 "\n", message,
			response);
		return B_ERROR;
	}
	if (_data != NULL) {
		// the firmware may take a while to put its answer there
		spin(10000);
		*_data = pmc_read(AMD_PMC_ARGUMENT);
	}
	return B_OK;
}


static void
pmc_probe()
{
	if (sPMCProbed)
		return;
	sPMCProbed = true;

	if (gCPU[0].arch.vendor != VENDOR_AMD)
		return;

	pci_module_info* pci;
	if (get_module(B_PCI_MODULE_NAME, (module_info**)&pci) != B_OK)
		return;

	uint32 id = pci->read_pci_config(0, 0, 0, PCI_device_id, 2);
	phys_addr_t base = 0;
	if (id == AMD_ROOT_DEVICE_PHOENIX) {
		InterruptsLocker _;
		pci->write_pci_config(0, 0, 0, AMD_SMN_INDEX, 4, AMD_PMC_BASE_LO);
		uint32 low = pci->read_pci_config(0, 0, 0, AMD_SMN_DATA, 4);
		pci->write_pci_config(0, 0, 0, AMD_SMN_INDEX, 4, AMD_PMC_BASE_HI);
		uint32 high = pci->read_pci_config(0, 0, 0, AMD_SMN_DATA, 4);
		base = ((phys_addr_t)(high & 0xffff) << 32) + (low & 0xfff00000)
			+ AMD_PMC_BASE_OFFSET;
	}
	put_module(B_PCI_MODULE_NAME);

	if (base == 0) {
		INFO("no AMD PMC known for root complex %#" B_PRIx32 "\n", id);
		return;
	}

	void* address;
	if (map_physical_memory("amd pmc", base, B_PAGE_SIZE,
			B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
			&address) < 0) {
		return;
	}
	sPMC = (volatile uint8*)address;

	// The mailbox holds the last command's response (OK, normally): with
	// nothing sensible there, this is not the PMC, and must not be written.
	uint32 response = pmc_read(AMD_PMC_RESPONSE);
	if (response == 0 || response == 0xffffffff) {
		INFO("no AMD PMC at %#" B_PRIxPHYSADDR " (response %#" B_PRIx32 ")\n",
			base, response);
		delete_area(area_for((void*)sPMC));
		sPMC = NULL;
		return;
	}

	if (map_physical_memory("amd fch pm", AMD_FCH_PM_BASE, B_PAGE_SIZE,
			B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA, &address) >= 0) {
		sFCH = (volatile uint8*)address;
	}

	INFO("AMD PMC at %#" B_PRIxPHYSADDR ", idle mask %#" B_PRIx32 "\n", base,
		pmc_read(AMD_PMC_IDLE_MASK));

	// the firmware's log, as Linux' amd_pmc_setup_smu_logging()
	uint32 low = 0;
	uint32 high = 0;
	if (pmc_send(AMD_PMC_MSG_ACTIVE_IPS, 0, &sPMCActiveIPs) == B_OK
		&& pmc_send(AMD_PMC_MSG_LOG_ADDRESS_LO, 0, &low) == B_OK
		&& pmc_send(AMD_PMC_MSG_LOG_ADDRESS_HI, 0, &high) == B_OK
		&& (low | high) != 0) {
		phys_addr_t table = ((phys_addr_t)high << 32) | low;
		phys_addr_t page = table & ~(phys_addr_t)(B_PAGE_SIZE - 1);
		size_t size = ROUNDUP(table - page + sizeof(amd_pmc_metrics),
			B_PAGE_SIZE);
		if (map_physical_memory("amd pmc metrics", page, size,
				B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
				&address) >= 0) {
			sPMCMetrics = (volatile amd_pmc_metrics*)((uint8*)address
				+ (table - page));
		}
	}
}


static void
pmc_start_log()
{
	if (sPMCMetrics == NULL)
		return;

	memset((void*)sPMCMetrics, 0, sizeof(amd_pmc_metrics));
	pmc_send(AMD_PMC_MSG_LOG_RESET, 0);
	pmc_send(AMD_PMC_MSG_LOG_START, 0);
}


/*!	Logs which IP blocks kept the SoC out of S0i3, and returns their mask. */
static uint32
pmc_report_log(bigtime_t slept)
{
	if (sPMCMetrics == NULL || pmc_send(AMD_PMC_MSG_LOG_DUMP, 0) != B_OK)
		return 0;

	amd_pmc_metrics metrics;
	memcpy(&metrics, (const void*)sPMCMetrics, sizeof(metrics));

	char blockers[256];
	size_t length = 0;
	blockers[0] = '\0';
	uint32 mask = 0;
	for (uint32 i = 0; i < B_COUNT_OF(kAMDIPBlocks); i++) {
		uint64 notMet = metrics.time_condition_not_met_last[i];
		if ((sPMCActiveIPs & (1u << i)) == 0 || notMet == 0
			|| notMet > (uint64)slept * 2) {
			// (more than the sleep is junk)
			continue;
		}
		mask |= 1u << i;
		if (length < sizeof(blockers)) {
			length += snprintf(blockers + length, sizeof(blockers) - length,
				" %s %" B_PRIu64 "%%", kAMDIPBlocks[i],
				slept > 0 ? notMet * 100 / slept : 0);
		}
	}

	INFO("PMC log: S0i3 %s, %" B_PRIu64 " ms in S0i3; kept out by%s\n",
		metrics.s0i3_last_entry_status != 0 ? "reached" : "not reached",
		metrics.time_in_s0i3_last / 1000, mask != 0 ? blockers : " nothing");
	return mask;
}


static uint64
fch_time(uint32 offset)
{
	if (sFCH == NULL)
		return 0;
	uint32 low = *(volatile uint32*)(sFCH + offset);
	uint32 high = *(volatile uint32*)(sFCH + offset + 4);
	return ((uint64)high << 32) | low;
}


//	#pragma mark - RTC alarm


/*!	In S0i3 the local APIC timer stops with the cores: a timeout needs the
	CMOS clock's alarm, which raises the ACPI RTC fixed event (an SCI), as
	Linux's rtc-cmos wake alarm does.
*/
static uint8
rtc_read(uint8 index)
{
	out8(index, 0x70);
	return in8(0x71);
}


static void
rtc_write(uint8 index, uint8 value)
{
	out8(index, 0x70);
	out8(value, 0x71);
}


static uint32
rtc_alarm_event(void* /*data*/)
{
	atomic_set(&sAlarmFired, 1);
	atomic_test_and_set(&sWakeReason, X86_S2IDLE_WAKE_TIMEOUT, 0);
	return 1;
		// ACPI_INTERRUPT_HANDLED
}


/*!	Sets the alarm \a seconds (less than a day) from now; the SCI handler
	must be installed. */
static bool
rtc_arm_alarm(uint32 seconds)
{
	if (seconds >= 24 * 60 * 60)
		return false;

	// FADT DAY_ALRM, the CMOS index of the day of month alarm (0 none)
	uint8 dayAlarm = 0;
	void* table;
	if (sACPI->get_table("FACP", 0, &table) == B_OK)
		dayAlarm = *((uint8*)table + 106);

	// wait out an update in progress
	for (int32 i = 0; i < 10000 && (rtc_read(0x0a) & 0x80) != 0; i++)
		spin(1);

	uint8 statusB = rtc_read(0x0b);
	bool binary = (statusB & 0x04) != 0;
	bool twelveHour = (statusB & 0x02) == 0;
	auto fromRTC = [binary](uint8 value) {
		return binary ? value : (value >> 4) * 10 + (value & 0x0f);
	};
	auto toRTC = [binary](uint32 value) {
		return (uint8)(binary ? value : ((value / 10) << 4) | (value % 10));
	};

	uint8 hourValue = rtc_read(0x04);
	uint32 hour = fromRTC(hourValue & 0x7f);
	if (twelveHour)
		hour = hour % 12 + ((hourValue & 0x80) != 0 ? 12 : 0);
	uint32 time = (hour * 60 + fromRTC(rtc_read(0x02))) * 60
		+ fromRTC(rtc_read(0x00)) + seconds;
	uint32 day = fromRTC(rtc_read(0x07));
	uint32 month = fromRTC(rtc_read(0x08));
	uint32 year = fromRTC(rtc_read(0x09));

	if (time >= 24 * 60 * 60) {
		time -= 24 * 60 * 60;
		static const uint8 kDays[] = {
			31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
		uint32 days = month >= 1 && month <= 12 ? kDays[month - 1] : 31;
		if (month == 2 && year % 4 == 0)
			days = 29;
		day = day >= days ? 1 : day + 1;
	}

	uint32 alarmHour = time / 3600;
	uint8 alarmHourValue;
	if (twelveHour) {
		alarmHourValue = toRTC(alarmHour % 12 == 0 ? 12 : alarmHour % 12)
			| (alarmHour >= 12 ? 0x80 : 0);
	} else
		alarmHourValue = toRTC(alarmHour);

	rtc_write(0x0b, statusB & ~0x20);
	rtc_write(0x01, toRTC(time % 60));
	rtc_write(0x03, toRTC(time / 60 % 60));
	rtc_write(0x05, alarmHourValue);
	if (dayAlarm != 0)
		rtc_write(dayAlarm, toRTC(day));
	rtc_read(0x0c);
		// clears a pending alarm flag
	sACPI->reset_fixed_event(ACPI_EVENT_RTC);
	rtc_write(0x0b, statusB | 0x20);
	sACPI->enable_fixed_event(ACPI_EVENT_RTC);
	return true;
}


static void
rtc_disarm_alarm()
{
	sACPI->disable_fixed_event(ACPI_EVENT_RTC);
	rtc_write(0x0b, rtc_read(0x0b) & ~0x20);
	rtc_read(0x0c);
	sACPI->reset_fixed_event(ACPI_EVENT_RTC);
}


//	#pragma mark - wakeup events


static void
lid_probe()
{
	if (sLidProbed)
		return;
	sLidProbed = true;

	if (sACPI->get_device("PNP0C0D", 0, sLidPath, sizeof(sLidPath)) != B_OK)
		sLidPath[0] = '\0';
}


static bool
lid_is_open()
{
	if (sLidPath[0] == '\0')
		return false;

	char method[sizeof(sLidPath) + 8];
	snprintf(method, sizeof(method), "%s._LID", sLidPath);

	acpi_data result = { ACPI_ALLOCATE_BUFFER, NULL };
	bool open = false;
	if (sACPI->evaluate_method(NULL, method, NULL, &result) == B_OK) {
		acpi_object_type* object = (acpi_object_type*)result.pointer;
		open = object != NULL && object->object_type == ACPI_TYPE_INTEGER
			&& object->integer.integer != 0;
	}
	free(result.pointer);
	return open;
}


/*!	Lets the ACPI threads handle the event that raised the SCI (the embedded
	controller's query, the notifications it sends) and tells whether it is
	one to wake up for. Without timer interrupts in the sleep, the timer runs
	again for that time.
*/
static bool
wakeup_is_real(int32 wakeupEvents)
{
	{
		InterruptsLocker _;
		arch_timer_set_hardware_timer(1000);
	}

	bool real = false;
	for (int32 i = 0; i < 20 && !real; i++) {
		snooze(10000);
		real = device_manager_wakeup_events() != wakeupEvents;
	}
	if (!real)
		real = lid_is_open();

	if (!real) {
		InterruptsLocker _;
		arch_timer_clear_hardware_timer();
	}
	return real;
}


/*!	As Linux' amd_pmc_s2idle_check(): after the SoC came out of S0i3 (for
	an SCI that is not one to wake up for), it is given 2.5 s before it may
	go back (against tripping its over-voltage protection).
*/
static void
s0i3_settle(uint64* _lastEntry)
{
	uint64 entry = fch_time(AMD_FCH_S0I3_ENTRY);
	if (entry == *_lastEntry)
		return;
	*_lastEntry = entry;

	{
		InterruptsLocker _;
		arch_timer_set_hardware_timer(1000);
	}
	snooze(2500000);
	{
		InterruptsLocker _;
		arch_timer_clear_hardware_timer();
	}
}


//	#pragma mark - the sleep


static int32
sci_wake(void* /*data*/)
{
	if (atomic_get(&sActive) != 0)
		atomic_test_and_set(&sWakeReason, X86_S2IDLE_WAKE_SCI, 0);
	return B_UNHANDLED_INTERRUPT;
}


static int32
find_sci()
{
	void* table;
	if (sACPI->get_table("FACP", 0, &table) != B_OK)
		return -1;
	// SCI_INT, a 16 bit field at offset 46 of the FADT
	return *(uint16*)((uint8*)table + 46);
}


status_t
x86_suspend_enter_s2idle(x86_suspend_s2idle_args* args)
{
	MutexLocker locker(sLock);

	if (get_module(B_ACPI_MODULE_NAME, (module_info**)&sACPI) != B_OK)
		return B_NOT_SUPPORTED;
	BPrivate::CObjectDeleter<const char, status_t, put_module>
		acpiPutter(B_ACPI_MODULE_NAME);

	lps0_probe();
	pmc_probe();
	lid_probe();
	if (sSCI < 0)
		sSCI = find_sci();
	if (sSCI < 0)
		return B_NOT_SUPPORTED;

	uint32 flags = args->flags;
	device_manager_clear_suspend_trace();
	INFO("entering s2idle, flags %#" B_PRIx32 ", timeout %" B_PRIu32 " ms\n",
		flags, args->timeout_ms);

	_kern_sync();

	// Everything moves to the boot CPU: the others idle (their interrupts
	// go to the boot CPU), the boot CPU runs the loop below.
	int32 cpuCount = smp_get_num_cpus();
	bool wasEnabled[SMP_MAX_CPUS] = {};
	for (int32 cpu = 0; cpu < cpuCount; cpu++)
		wasEnabled[cpu] = !gCPU[cpu].disabled;
	if (!wasEnabled[0])
		cpu_set_enabled(0, true);
	for (int32 cpu = 1; cpu < cpuCount; cpu++) {
		if (wasEnabled[cpu])
			cpu_set_enabled(cpu, false);
	}
	while (smp_get_current_cpu() != 0)
		thread_yield();
	Thread* thread = thread_get_current_thread();
	thread_pin_to_current_cpu(thread);

	// the deepest C-states for the idle CPUs
	int32 oldMode = _user_get_scheduler_mode();
	scheduler_set_operation_mode(SCHEDULER_MODE_POWER_SAVING);

	uint32 deviceFlags = 0;
	if ((flags & X86_SUSPEND_SKIP_POWER_HOOKS) != 0)
		deviceFlags |= DEVICE_MANAGER_SKIP_POWER_HOOKS;
	if ((flags & X86_SUSPEND_SKIP_DEVICE_TREE) != 0)
		deviceFlags |= DEVICE_MANAGER_SKIP_DEVICE_TREE;
	if ((flags & X86_SUSPEND_SKIP_DEVICES) == 0)
		device_manager_suspend(ACPI_POWER_STATE_ON, deviceFlags);

	// as Linux's acpi_s2idle_prepare_late() and amd_pmc_s2idle_prepare()
	if (sLPS0Path[0] != '\0') {
		lps0_amd(LPS0_AMD_SCREEN_OFF);
		lps0_microsoft(LPS0_MS_SCREEN_OFF);
		lps0_amd(LPS0_AMD_ENTRY);
		lps0_microsoft(LPS0_MS_STANDBY_ENTRY);
		lps0_microsoft(LPS0_MS_ENTRY);
	}
	pmc_start_log();
	status_t hintStatus = pmc_send(AMD_PMC_MSG_OS_HINT, 1);
	uint64 entryBefore = fch_time(AMD_FCH_S0I3_ENTRY);

	install_io_interrupt_handler(sSCI, &sci_wake, NULL, 0);
	atomic_set(&sWakeReason, 0);

	// (rounded up to the next second, after the APIC deadline)
	atomic_set(&sAlarmFired, 0);
	bool rtcAlarm = false;
	if (args->timeout_ms != 0
		&& sACPI->install_fixed_event_handler(ACPI_EVENT_RTC,
			&rtc_alarm_event, NULL) == B_OK) {
		rtcAlarm = rtc_arm_alarm((args->timeout_ms + 1999) / 1000);
		if (!rtcAlarm)
			sACPI->remove_fixed_event_handler(ACPI_EVENT_RTC, &rtc_alarm_event);
	}

	// Nothing else runs on the boot CPU while it sleeps.
	int32 oldPriority = thread->priority;
	set_thread_priority(thread->id, B_REAL_TIME_PRIORITY);

	// No timer interrupts: only the SCI (and devices) end the idling, or the
	// one timer interrupt at the end of the timeout. (The kernel's own timers
	// would wake the CPU hundreds of times a second.)
	bigtime_t start = system_time();
	bigtime_t deadline = args->timeout_ms != 0
		? start + (bigtime_t)args->timeout_ms * 1000 : 0;
	{
		InterruptsLocker _;
		arch_timer_clear_hardware_timer();
	}

	int32 wakeupEvents = device_manager_wakeup_events();
	atomic_set(&sActive, 1);
	uint32 rounds = 0;
	uint32 ignoredWakeups = 0;
	uint64 lastEntry = entryBefore;
	while (true) {
		while (atomic_get(&sWakeReason) == 0
			&& atomic_get(&sAlarmFired) == 0) {
			if (deadline != 0) {
				// (any interrupt lets the kernel program the timer for its
				// own next timer; the hardware timer only goes so far, too)
				InterruptsLocker _;
				bigtime_t now = system_time();
				if (now >= deadline) {
					atomic_test_and_set(&sWakeReason, X86_S2IDLE_WAKE_TIMEOUT,
						0);
					break;
				}
				arch_timer_set_hardware_timer(deadline - now);
			}
			cpu_idle();
			rounds++;
		}
		// (the alarm's SCI may have been taken for another one)
		if (atomic_get(&sAlarmFired) != 0) {
			atomic_set(&sWakeReason, X86_S2IDLE_WAKE_TIMEOUT);
			break;
		}
		if (atomic_get(&sWakeReason) != X86_S2IDLE_WAKE_SCI
			|| wakeup_is_real(wakeupEvents)) {
			break;
		}

		ignoredWakeups++;
		s0i3_settle(&lastEntry);
		atomic_set(&sWakeReason, 0);
	}
	atomic_set(&sActive, 0);
	bigtime_t slept = system_time() - start;

	{
		// the timers that came due get handled now
		InterruptsLocker _;
		arch_timer_set_hardware_timer(1000);
	}

	set_thread_priority(thread->id, oldPriority);
	if (rtcAlarm) {
		rtc_disarm_alarm();
		sACPI->remove_fixed_event_handler(ACPI_EVENT_RTC, &rtc_alarm_event);
	}
	remove_io_interrupt_handler(sSCI, &sci_wake, NULL);

	status_t exitHintStatus = pmc_send(AMD_PMC_MSG_OS_HINT, 0);
	uint64 entry = fch_time(AMD_FCH_S0I3_ENTRY);
	uint64 exit = fch_time(AMD_FCH_S0I3_EXIT);
	uint32 idleMask = sPMC != NULL ? pmc_read(AMD_PMC_IDLE_MASK) : 0;
	uint32 blockers = pmc_report_log(slept);

	// as Linux's amd_pmc_s2idle_restore() and acpi_s2idle_restore_early()
	if (sLPS0Path[0] != '\0') {
		lps0_amd(LPS0_AMD_EXIT);
		lps0_microsoft(LPS0_MS_EXIT);
		lps0_microsoft(LPS0_MS_TURN_ON_DISPLAY);
		lps0_microsoft(LPS0_MS_STANDBY_EXIT);
		lps0_microsoft(LPS0_MS_SCREEN_ON);
		lps0_amd(LPS0_AMD_SCREEN_ON);
	}

	if ((flags & X86_SUSPEND_SKIP_DEVICES) == 0)
		device_manager_resume(deviceFlags);

	scheduler_set_operation_mode((scheduler_mode)oldMode);

	thread_unpin_from_current_cpu(thread);
	for (int32 cpu = 1; cpu < cpuCount; cpu++) {
		if (wasEnabled[cpu])
			cpu_set_enabled(cpu, true);
	}

	args->wake_reason = atomic_get(&sWakeReason);
	args->slept_us = slept;
	args->idle_rounds = rounds;
	args->pmc_idle_mask = idleMask;
	args->s0i3_entered = entry != entryBefore && exit > entry;
	args->s0i3_us = args->s0i3_entered ? (exit - entry) / 48 : 0;
	args->s0i3_blockers = blockers;

	INFO("woke up (%s%s) after %" B_PRIdBIGTIME " ms, %" B_PRIu32 " rounds, %"
		B_PRIu32 " SCIs slept through; OS hint %s/%s; S0i3 %s (%" B_PRIu64
		" ms of the last); PMC idle mask %#" B_PRIx32 "\n",
		args->wake_reason == X86_S2IDLE_WAKE_SCI ? "SCI" : "timeout",
		atomic_get(&sAlarmFired) != 0 ? ", RTC alarm"
			: rtcAlarm ? "" : ", no RTC alarm",
		slept / 1000, rounds, ignoredWakeups, strerror(hintStatus),
		strerror(exitHintStatus),
		args->s0i3_entered ? "entered" : "not entered", args->s0i3_us / 1000,
		idleMask);
	return B_OK;
}
