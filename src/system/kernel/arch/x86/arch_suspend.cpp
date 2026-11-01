/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	ACPI S3 (suspend to RAM) support for x86_64.

	Suspending saves the state of every CPU in a struct x86_suspend_context.
	The application processors are parked in a halt loop from an ICI, the boot
	CPU then enters S3 through ACPICA. On wakeup the firmware jumps to the real
	mode trampoline in low memory, which switches to long mode on temporary
	page tables and continues in x86_resume_entry(). That restores the boot
	CPU, which afterwards restarts the application processors with INIT/SIPI
	through the same trampoline.
*/


#include <arch/x86/arch_suspend.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>

#include <ACPI.h>
#include <cpu.h>
#include <debug.h>
#include <kdevice_manager.h>
#include <generic_syscall.h>
#include <interrupts.h>
#include <kernel.h>
#include <real_time_clock.h>
#include <smp.h>
#include <thread.h>
#include <syscalls.h>
#include <timer.h>
#include <arch/timer.h>
#include <arch/x86/apic.h>
#include <arch/x86/arch_cpu.h>
#include <arch/x86/arch_smp.h>
#include <arch/x86/descriptors.h>
#include <arch/x86/ioapic.h>
#include <util/AutoLock.h>
#include <AutoDeleter.h>


//#define TRACE_SUSPEND
#ifdef TRACE_SUSPEND
#	define TRACE(x...) dprintf("suspend: " x)
#else
#	define TRACE(x...) ;
#endif
#define INFO(x...) do { dprintf("suspend: " x); \
	device_manager_suspend_trace(x); } while (false)


struct x86_suspend_context {
	// saved and restored by suspend.S
	uint64	rbx;
	uint64	rbp;
	uint64	r12;
	uint64	r13;
	uint64	r14;
	uint64	r15;
	uint64	rsp;
	uint64	rip;
	uint64	rflags;
	uint64	cr0;
	uint64	cr3;
	uint64	cr4;
	uint64	efer;
	uint64	gs_base;
	uint64	kernel_gs_base;
	uint64	fs_base;
	uint8	gdtr[10];

	// handled in C
	uint64	star;
	uint64	lstar;
	uint64	cstar;
	uint64	fmask;
	uint64	sysenter_cs;
	uint64	sysenter_esp;
	uint64	sysenter_eip;
	uint64	pat;
	uint64	tsc_aux;
	uint64	de_cfg;
	uint64	tsc;
	int16	acpi_processor_id;
};


struct x86_wakeup_args {
	uint32	trampoline;
	uint32	gdtr32;
	uint32	pml4;
	uint32	padding;
	uint64	entry;
	uint64	context;
	uint64	stack;

	// temporary GDT
	uint16	gdt_limit;
	uint32	gdt_base;
	uint16	padding2;
	uint64	gdt[4];
} _PACKED;


extern "C" void x86_wakeup_trampoline(void);
extern "C" void x86_wakeup_trampoline_args(void);
extern "C" void x86_wakeup_trampoline_end(void);
extern "C" int x86_suspend_save_context(x86_suspend_context* context);
extern "C" void x86_resume_entry(void);
extern "C" void x86_suspend_restore_cpu(x86_suspend_context* context);

extern bool gHasXsave;
extern uint64 gXsaveMask;


// Pages below 1 MB used for waking up. The first 64 KB are avoided, since
// firmware tends to use them during resume.
static const phys_addr_t kWakeupArgsPage = 0x80000;
static const phys_addr_t kWakeupCodePage = 0x81000;
static const phys_addr_t kWakeupPML4 = 0x82000;
static const phys_addr_t kWakeupPDPT = 0x83000;
static const phys_addr_t kWakeupPageDirectory = 0x84000;
static const phys_addr_t kTracePage = 0x85000;
	// the trace of the last resume is kept here, so that it can be printed
	// after rebooting when the machine could not report it otherwise
static const uint32 kTraceMagic = 0x53555350;	// 'SUSP'
	// the trampoline stores its progress at this offset of the argument page
static const size_t kProgressOffset = 0xff0;

static const uint64 kPagePresent = 1 << 0;
static const uint64 kPageWritable = 1 << 1;
static const uint64 kPageLarge = 1 << 7;
static const uint64 kPageAddressMask = 0x000ffffffffff000ULL;

static x86_suspend_context sContexts[SMP_MAX_CPUS];
static int32 sParked[SMP_MAX_CPUS];
static int32 sRestarted[SMP_MAX_CPUS];
static bool sAdjustTSC;
static int64 sTSCDelta;
static bool sHasTSCAux;
static bool sHasDECfg;
static bool sHasPAT;

static uint32 sPowerOffCheckpoint;

// AMD's IOMMU is set up by the firmware and forgets its configuration while
// the system sleeps. Without restoring it, devices can neither access memory
// nor deliver interrupts after resuming.
static const uint32 kIommuDeviceTableBase	= 0x00;
static const uint32 kIommuCommandBufferBase	= 0x08;
static const uint32 kIommuEventLogBase		= 0x10;
static const uint32 kIommuControl			= 0x18;
static const uint32 kIommuExclusionBase		= 0x20;
static const uint32 kIommuExclusionLimit	= 0x28;
static const uint32 kIommuCommandHead		= 0x2000;
static const uint32 kIommuCommandTail		= 0x2008;
static const uint32 kIommuEventHead			= 0x2010;
static const uint32 kIommuEventTail			= 0x2018;

static area_id sIommuArea = -1;
static volatile uint8* sIommuRegisters;
static uint64 sIommuDeviceTable;
static uint64 sIommuCommandBuffer;
static uint64 sIommuEventLog;
static uint64 sIommuExclusionBase;
static uint64 sIommuExclusionLimit;
static uint64 sIommuControl;

static mutex sSuspendLock = MUTEX_INITIALIZER("x86 suspend");


static acpi_module_info* sACPI;


/*!	Logs a step and waits afterwards, so that the syslog daemon can write the
	log before a step that hangs.
*/
static void
verbose_step(uint32 flags, const char* format, ...)
{
	if ((flags & X86_SUSPEND_VERBOSE) == 0)
		return;

	char buffer[128];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);

	INFO("%s\n", buffer);

	if (!are_interrupts_enabled())
		return;

	// give the syslog daemon time to write the line, then flush it to disk,
	// so that it survives if the next step hangs
	snooze(300000);
	_kern_sync();
}


static void
checkpoint(uint32 number)
{
	if (sPowerOffCheckpoint != number || sACPI == NULL)
		return;

	// Powering off requires evaluating _PTS, writing the PM1 control
	// register alone is ignored by the firmware.
	sACPI->prepare_sleep_state(ACPI_POWER_STATE_OFF, NULL, 0);
	sACPI->enter_sleep_state(ACPI_POWER_STATE_OFF);
	while (true)
		asm volatile("hlt");
}


/*!	Resume progress is recorded in a byte of CMOS RAM, which survives losing
	power: when resuming goes wrong, the machine usually cannot write its log
	any more, but the marker can be read at the next boot.
*/
static const uint8 kResumeMarkerRegister = 0x3f;

static void
write_resume_marker(uint8 value)
{
	out8(kResumeMarkerRegister, 0x70);
	out8(value, 0x71);
}


static uint8
read_resume_marker()
{
	out8(kResumeMarkerRegister, 0x70);
	return in8(0x71);
}


static inline void*
physical_page(phys_addr_t address)
{
	return (void*)(KERNEL_PMAP_BASE + address);
}


static void
save_trace_to_memory()
{
	x86_suspend_save_trace();
}


// How far the last sleep got, in the RTC's battery backed CMOS (the upper
// 128 bytes, which the firmware leaves alone here): the number of trace
// lines, of idle rounds, the phase, and the start of the last trace line.
// Unlike the trace page, it survives holding the power button.
static const uint8 kCMOSProgressText = 0xc0;
static const size_t kCMOSProgressTextSize = 48;
static const uint8 kCMOSProgress = 0xf8;
static const uint8 kCMOSProgressMagic = 0x54;
static int32 sProgressLines;
static int32 sProgressRound;
static int32 sProgressPhase;
static char sProgressText[kCMOSProgressTextSize];
static int32 sProgressWriting;
static int32 sProgressTextHeld;


static void
cmos_write(uint8 index, uint8 value)
{
	out8(index, 0x72);
	out8(value, 0x73);
}


static uint8
cmos_read(uint8 index)
{
	out8(index, 0x72);
	return in8(0x73);
}


static void
write_progress(bool withText)
{
	if (atomic_test_and_set(&sProgressWriting, 1, 0) != 0)
		return;

	uint16 lines = (uint16)atomic_get(&sProgressLines);
	uint16 round = (uint16)atomic_get(&sProgressRound);
	uint8 bytes[7] = { kCMOSProgressMagic, (uint8)lines, (uint8)(lines >> 8),
		(uint8)round, (uint8)(round >> 8),
		(uint8)atomic_get(&sProgressPhase), 0xa5 };
	for (int i = 0; i < 6; i++)
		bytes[6] ^= bytes[i];

	cpu_status state = disable_interrupts();
	if (withText) {
		for (size_t i = 0; i < kCMOSProgressTextSize; i++)
			cmos_write(kCMOSProgressText + i, (uint8)sProgressText[i]);
	}
	for (int i = 0; i < 7; i++)
		cmos_write(kCMOSProgress + i, bytes[i]);
	restore_interrupts(state);
	atomic_set(&sProgressWriting, 0);
}


/*!	Starts counting the trace lines and idle rounds of a sleep in CMOS. */
void
x86_suspend_start_progress()
{
	atomic_set(&sProgressLines, 0);
	atomic_set(&sProgressRound, 0);
	atomic_set(&sProgressPhase, X86_SUSPEND_PROGRESS_SLEEPING);
	atomic_set(&sProgressTextHeld, 0);
	memset(sProgressText, 0, sizeof(sProgressText));
	write_progress(true);
}


/*!	Keeps the last line in CMOS as it is: the lines that follow only explain
	it. */
void
x86_suspend_hold_progress_text()
{
	atomic_set(&sProgressTextHeld, 1);
}


/*!	Notes an idle round in CMOS (the trace only has the first ones). */
void
x86_suspend_note_round(uint32 round)
{
	atomic_set(&sProgressRound, (int32)round);
	write_progress(false);
}


/*!	Notes how far the sleep (or the time after it) got. Safe in a timer. */
void
x86_suspend_note_phase(uint32 phase)
{
	atomic_set(&sProgressPhase, (int32)phase);
	write_progress(false);
}


static void
print_saved_progress()
{
	uint8 bytes[7];
	char text[kCMOSProgressTextSize + 1];
	cpu_status state = disable_interrupts();
	for (int i = 0; i < 7; i++)
		bytes[i] = cmos_read(kCMOSProgress + i);
	for (size_t i = 0; i < kCMOSProgressTextSize; i++)
		text[i] = (char)cmos_read(kCMOSProgressText + i);
	restore_interrupts(state);
	text[kCMOSProgressTextSize] = '\0';

	uint8 check = 0xa5;
	for (int i = 0; i < 6; i++)
		check ^= bytes[i];
	if (bytes[0] != kCMOSProgressMagic || bytes[6] != check)
		return;

	static const char* const kPhases[] = { "?", "in the sleep",
		"back from the sleep", "a minute after the sleep" };
	uint8 phase = bytes[5] < B_COUNT_OF(kPhases) ? bytes[5] : 0;
	for (size_t i = 0; i < kCMOSProgressTextSize && text[i] != '\0'; i++) {
		if (text[i] < ' ' || text[i] > '~')
			text[i] = '.';
	}
	INFO("progress of the last sleep (CMOS): %s, %u trace lines, round %u, "
		"last line \"%s\"\n", kPhases[phase], bytes[1] | (bytes[2] << 8),
		bytes[3] | (bytes[4] << 8), text);
	state = disable_interrupts();
	cmos_write(kCMOSProgress, 0);
	restore_interrupts(state);
}


/*!	Keeps the end of the suspend trace (what happened last) in a page that a
	warm reboot leaves alone; the next boot prints it. Safe in an interrupt
	handler. */
void
x86_suspend_save_trace()
{
	atomic_add(&sProgressLines, 1);

	// (an interrupt handler's line may come in meanwhile: the next line
	// saves it)
	static int32 sSaving;
	if (atomic_test_and_set(&sSaving, 1, 0) != 0) {
		write_progress(false);
		return;
	}

	static char trace[16384];
	size_t length = device_manager_get_suspend_trace(trace, sizeof(trace));

	// the last line, for the CMOS
	size_t lineEnd = atomic_get(&sProgressTextHeld) != 0 ? 0 : length;
	while (lineEnd > 0 && trace[lineEnd - 1] == '\n')
		lineEnd--;
	size_t lineStart = lineEnd;
	while (lineStart > 0 && trace[lineStart - 1] != '\n')
		lineStart--;
	bool withText = lineEnd > lineStart;
	if (withText) {
		memset(sProgressText, 0, sizeof(sProgressText));
		memcpy(sProgressText, trace + lineStart,
			std::min(lineEnd - lineStart, sizeof(sProgressText)));
	}
	write_progress(withText);
	size_t room = B_PAGE_SIZE - 9;
	size_t start = length > room ? length - room : 0;

	uint32* page = (uint32*)physical_page(kTracePage);
	char* text = (char*)(page + 2);
	memcpy(text, trace + start, length - start);
	page[0] = kTraceMagic;
	page[1] = (uint32)(length - start);
	atomic_set(&sSaving, 0);
}


static void
print_saved_trace()
{
	uint32* page = (uint32*)physical_page(kTracePage);
	if (page[0] != kTraceMagic)
		return;

	page[0] = 0;
	uint32 length = page[1];
	if (length >= B_PAGE_SIZE - 8)
		return;

	char* text = (char*)(page + 2);
	text[length] = '\0';
	// line by line, as dprintf() only takes so much at once
	dprintf("suspend: trace of the last resume:\n");
	char* line = text;
	while (*line != '\0') {
		char* end = strchr(line, '\n');
		if (end != NULL)
			*end = '\0';
		if (*line != '\0')
			dprintf("suspend:   %s\n", line);
		if (end == NULL)
			break;
		line = end + 1;
	}
}


static inline uint64
iommu_read(uint32 offset)
{
	return *(volatile uint64*)(sIommuRegisters + offset);
}


static inline void
iommu_write(uint32 offset, uint64 value)
{
	*(volatile uint64*)(sIommuRegisters + offset) = value;
}


/*!	Maps the registers of the first IOMMU described by the IVRS table. */
static void
iommu_init(acpi_module_info* acpi)
{
	if (sIommuArea >= 0 || acpi == NULL)
		return;

	void* table;
	if (acpi->get_table("IVRS", 0, &table) != B_OK)
		return;

	uint8* ivrs = (uint8*)table;
	uint32 length = *(uint32*)(ivrs + 4);
	phys_addr_t base = 0;

	for (uint32 offset = 48; offset + 24 <= length;) {
		uint8 type = ivrs[offset];
		uint16 entryLength = *(uint16*)(ivrs + offset + 2);
		if (entryLength == 0)
			break;

		// IVHD entries describe an IOMMU and carry its base address
		if (type == 0x10 || type == 0x11 || type == 0x40) {
			base = *(uint64*)(ivrs + offset + 8);
			if (base != 0)
				break;
		}
		offset += entryLength;
	}

	if (base == 0)
		return;

	void* address;
	sIommuArea = map_physical_memory("amd iommu", base, 0x3000,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		&address);
	if (sIommuArea < 0) {
		INFO("mapping the IOMMU at %#" B_PRIxPHYSADDR " failed: %s\n", base,
			strerror(sIommuArea));
		return;
	}

	sIommuRegisters = (volatile uint8*)address;
	INFO("IOMMU at %#" B_PRIxPHYSADDR ", control %#" B_PRIx64 "\n", base,
		iommu_read(kIommuControl));
}


static void
iommu_save()
{
	if (sIommuRegisters == NULL)
		return;

	sIommuDeviceTable = iommu_read(kIommuDeviceTableBase);
	sIommuCommandBuffer = iommu_read(kIommuCommandBufferBase);
	sIommuEventLog = iommu_read(kIommuEventLogBase);
	sIommuExclusionBase = iommu_read(kIommuExclusionBase);
	sIommuExclusionLimit = iommu_read(kIommuExclusionLimit);
	sIommuControl = iommu_read(kIommuControl);
}


static void
iommu_restore()
{
	if (sIommuRegisters == NULL)
		return;

	// Disable it while its tables are set up again, then start over with
	// empty command and event rings.
	iommu_write(kIommuControl, 0);

	iommu_write(kIommuDeviceTableBase, sIommuDeviceTable);
	iommu_write(kIommuCommandBufferBase, sIommuCommandBuffer);
	iommu_write(kIommuEventLogBase, sIommuEventLog);
	iommu_write(kIommuExclusionBase, sIommuExclusionBase);
	iommu_write(kIommuExclusionLimit, sIommuExclusionLimit);

	iommu_write(kIommuCommandHead, 0);
	iommu_write(kIommuCommandTail, 0);
	iommu_write(kIommuEventHead, 0);
	iommu_write(kIommuEventTail, 0);

	iommu_write(kIommuControl, sIommuControl);
}


static void
save_msrs(x86_suspend_context* context, int32 cpu)
{
	context->star = x86_read_msr(IA32_MSR_STAR);
	context->lstar = x86_read_msr(IA32_MSR_LSTAR);
	context->cstar = x86_read_msr(IA32_MSR_CSTAR);
	context->fmask = x86_read_msr(IA32_MSR_FMASK);
	context->sysenter_cs = x86_read_msr(IA32_MSR_SYSENTER_CS);
	context->sysenter_esp = x86_read_msr(IA32_MSR_SYSENTER_ESP);
	context->sysenter_eip = x86_read_msr(IA32_MSR_SYSENTER_EIP);
	if (sHasPAT)
		context->pat = x86_read_msr(IA32_MSR_PAT);
	if (sHasTSCAux)
		context->tsc_aux = x86_read_msr(IA32_MSR_TSC_AUX);
	if (sHasDECfg)
		context->de_cfg = x86_read_msr(MSR_F10H_DE_CFG);
	context->acpi_processor_id = gCPU[cpu].arch.acpi_processor_id;
}


extern "C" void
x86_suspend_restore_cpu(x86_suspend_context* context)
{
	int32 cpu = context - sContexts;
	*(uint8*)physical_page(kWakeupArgsPage + kProgressOffset) = 4;
	if (cpu == 0 && sAdjustTSC)
		write_resume_marker(0x10);

	if (sAdjustTSC) {
		// The TSC was reset while sleeping. Continue where the boot CPU
		// stopped, so that system_time() stays monotonic.
		uint64 tsc = x86_read_msr(IA32_MSR_TSC);
		if (cpu == 0)
			sTSCDelta = context->tsc - tsc;
		x86_write_msr(IA32_MSR_TSC, tsc + sTSCDelta);
	}

	x86_write_msr(IA32_MSR_STAR, context->star);
	x86_write_msr(IA32_MSR_LSTAR, context->lstar);
	x86_write_msr(IA32_MSR_CSTAR, context->cstar);
	x86_write_msr(IA32_MSR_FMASK, context->fmask);
	x86_write_msr(IA32_MSR_SYSENTER_CS, context->sysenter_cs);
	x86_write_msr(IA32_MSR_SYSENTER_ESP, context->sysenter_esp);
	x86_write_msr(IA32_MSR_SYSENTER_EIP, context->sysenter_eip);
	if (sHasPAT)
		x86_write_msr(IA32_MSR_PAT, context->pat);
	if (sHasTSCAux)
		x86_write_msr(IA32_MSR_TSC_AUX, context->tsc_aux);
	if (sHasDECfg)
		x86_write_msr(MSR_F10H_DE_CFG, context->de_cfg);

	x86_descriptors_resume_percpu(cpu);

	if (gHasXsave)
		xsetbv(0, gXsaveMask);
	asm volatile("fninit");

	apic_per_cpu_init(NULL, cpu);
	gCPU[cpu].arch.acpi_processor_id = context->acpi_processor_id;

	if (cpu == 0 && sAdjustTSC)
		checkpoint(1);
}


static void
prepare_wakeup_trampoline(uint64 cr3, x86_suspend_context* context)
{
	x86_wakeup_args* args = (x86_wakeup_args*)physical_page(kWakeupArgsPage);
	memset(args, 0, B_PAGE_SIZE);

	// Copy the trampoline code and point it to its arguments.
	size_t codeSize = (addr_t)x86_wakeup_trampoline_end
		- (addr_t)x86_wakeup_trampoline;
	void* code = physical_page(kWakeupCodePage);
	memcpy(code, (const void*)x86_wakeup_trampoline, codeSize);
	uint32* argsPointer = (uint32*)((addr_t)code
		+ (addr_t)x86_wakeup_trampoline_args - (addr_t)x86_wakeup_trampoline);
	*argsPointer = kWakeupArgsPage;

	// The temporary page tables identity map the first 2 MB and share the
	// kernel half with the kernel page tables.
	uint64* pageDirectory = (uint64*)physical_page(kWakeupPageDirectory);
	memset(pageDirectory, 0, B_PAGE_SIZE);
	pageDirectory[0] = kPagePresent | kPageWritable | kPageLarge;

	uint64* pdpt = (uint64*)physical_page(kWakeupPDPT);
	memset(pdpt, 0, B_PAGE_SIZE);
	pdpt[0] = kWakeupPageDirectory | kPagePresent | kPageWritable;

	uint64* pml4 = (uint64*)physical_page(kWakeupPML4);
	uint64* kernelPML4 = (uint64*)physical_page(cr3 & kPageAddressMask);
	memset(pml4, 0, B_PAGE_SIZE);
	pml4[0] = kWakeupPDPT | kPagePresent | kPageWritable;
	memcpy(pml4 + 256, kernelPML4 + 256, 256 * sizeof(uint64));

	args->trampoline = kWakeupCodePage;
	args->gdtr32 = kWakeupArgsPage + offsetof(x86_wakeup_args, gdt_limit);
	args->pml4 = kWakeupPML4;
	args->entry = (addr_t)x86_resume_entry;
	args->context = (addr_t)context;
	args->stack = kWakeupArgsPage + B_PAGE_SIZE - 16;

	args->gdt_limit = sizeof(args->gdt) - 1;
	args->gdt_base = kWakeupArgsPage + offsetof(x86_wakeup_args, gdt);
	args->gdt[0] = 0;
	args->gdt[1] = 0x00cf9a000000ffffULL;	// 32 bit code
	args->gdt[2] = 0x00cf92000000ffffULL;	// data
	args->gdt[3] = 0x00af9a000000ffffULL;	// 64 bit code
}


static void
park_cpu(void* /*cookie*/, int cpu)
{
	x86_suspend_context* context = &sContexts[cpu];
	save_msrs(context, cpu);

	if (x86_suspend_save_context(context) == 0) {
		atomic_set(&sParked[cpu], 1);
		while (true)
			asm volatile("hlt");
	}

	// We were restarted. The local APIC timer is stopped, get it going again
	// so that pending timer events are handled.
	atomic_set(&sRestarted[cpu], 1);
	arch_timer_set_hardware_timer(1000);
}


static status_t
wait_for_flag(int32* flag, bigtime_t timeout, bool interruptsEnabled)
{
	bigtime_t end = system_time() + timeout;
	while (atomic_get(flag) == 0) {
		if (system_time() > end)
			return B_TIMED_OUT;
		if (interruptsEnabled)
			snooze(1000);
		else
			spin(100);
	}
	return B_OK;
}


static status_t
start_cpu(int32 cpu)
{
	x86_suspend_context* context = &sContexts[cpu];
	prepare_wakeup_trampoline(context->cr3, context);

	uint32 apicID = x86_get_cpu_apic_id(cpu);
	atomic_set(&sRestarted[cpu], 0);

	apic_set_interrupt_command(apicID, APIC_TRIGGER_MODE_LEVEL
		| APIC_INTR_COMMAND_1_ASSERT | APIC_DELIVERY_MODE_INIT);
	while (!apic_interrupt_delivered())
		cpu_pause();
	spin(200);

	apic_set_interrupt_command(apicID,
		APIC_TRIGGER_MODE_LEVEL | APIC_DELIVERY_MODE_INIT);
	while (!apic_interrupt_delivered())
		cpu_pause();
	spin(10000);

	for (int i = 0; i < 2; i++) {
		apic_set_interrupt_command(apicID,
			APIC_DELIVERY_MODE_STARTUP | (kWakeupCodePage >> 12));
		spin(200);
		while (!apic_interrupt_delivered())
			cpu_pause();
		if (atomic_get(&sRestarted[cpu]) != 0)
			break;
	}

	status_t status = wait_for_flag(&sRestarted[cpu], 1000000, false);
	if (status != B_OK) {
		INFO("CPU %" B_PRId32 " did not restart, progress %u\n", cpu,
			*(uint8*)physical_page(kWakeupArgsPage + kProgressOffset));
	}
	return status;
}


static void
check_features()
{
	cpu_ent* cpu = get_cpu_struct();
	sHasPAT = x86_check_feature(IA32_FEATURE_PAT, FEATURE_COMMON);
	sHasTSCAux = x86_check_feature(IA32_FEATURE_AMD_EXT_RDTSCP,
			FEATURE_EXT_AMD)
		|| x86_check_feature(IA32_FEATURE_RDPID, FEATURE_7_ECX);
	uint32 family = cpu->arch.family + cpu->arch.extended_family;
	sHasDECfg = cpu->arch.vendor == VENDOR_AMD && family >= 0x10
		&& family != 0x11;
}


status_t
x86_suspend_restart_cpu(int32 cpu)
{
	if (cpu <= 0 || cpu >= smp_get_num_cpus())
		return B_BAD_VALUE;

	MutexLocker locker(sSuspendLock);

	bool wasEnabled = !gCPU[cpu].disabled;
	if (wasEnabled) {
		status_t status = cpu_set_enabled(cpu, false);
		if (status != B_OK)
			return status;
	}

	sAdjustTSC = false;
	atomic_set(&sParked[cpu], 0);
	call_single_cpu(cpu, &park_cpu, NULL);

	status_t status = wait_for_flag(&sParked[cpu], 1000000, true);
	if (status != B_OK) {
		INFO("CPU %" B_PRId32 " did not park\n", cpu);
		return status;
	}

	thread_pin_to_current_cpu(thread_get_current_thread());
	cpu_status state = disable_interrupts();
	status = start_cpu(cpu);
	restore_interrupts(state);
	thread_unpin_from_current_cpu(thread_get_current_thread());

	if (status == B_OK && wasEnabled)
		cpu_set_enabled(cpu, true);

	INFO("restart of CPU %" B_PRId32 ": %s\n", cpu, strerror(status));
	return status;
}


status_t
x86_suspend_enter_s3(const x86_suspend_s3_args* args)
{
	uint32 flags = args->flags;

	MutexLocker locker(sSuspendLock);

	acpi_module_info* acpi;
	if (get_module(B_ACPI_MODULE_NAME, (module_info**)&acpi) != B_OK)
		return B_NOT_SUPPORTED;
	BPrivate::CObjectDeleter<const char, status_t, put_module>
		acpiPutter(B_ACPI_MODULE_NAME);

	int32 cpuCount = smp_get_num_cpus();
	device_manager_clear_suspend_trace();
	INFO("entering S3, flags %#" B_PRIx32 "\n", flags);

	sPowerOffCheckpoint = args->power_off_checkpoint;
	sACPI = acpi;
	iommu_init(acpi);
	device_manager_set_suspend_verbose((flags & X86_SUSPEND_VERBOSE) != 0);

	// Write out everything while all CPUs are still running.
	_kern_sync();
	verbose_step(flags, "file systems synced");

	// Move everything to the boot CPU.
	bool wasEnabled[SMP_MAX_CPUS] = {};
	for (int32 cpu = 0; cpu < cpuCount; cpu++)
		wasEnabled[cpu] = !gCPU[cpu].disabled;
	if (!wasEnabled[0])
		cpu_set_enabled(0, true);
	for (int32 cpu = 1; cpu < cpuCount; cpu++) {
		if (wasEnabled[cpu]) {
			verbose_step(flags, "disabling CPU %" B_PRId32, cpu);
			cpu_set_enabled(cpu, false);
		}
	}
	verbose_step(flags, "all application processors disabled");
	while (smp_get_current_cpu() != 0)
		thread_yield();
	thread_pin_to_current_cpu(thread_get_current_thread());
	verbose_step(flags, "running on the boot CPU");


	uint32 deviceFlags = 0;
	if ((flags & X86_SUSPEND_SKIP_POWER_HOOKS) != 0)
		deviceFlags |= DEVICE_MANAGER_SKIP_POWER_HOOKS;
	if ((flags & X86_SUSPEND_SKIP_DEVICE_TREE) != 0)
		deviceFlags |= DEVICE_MANAGER_SKIP_DEVICE_TREE;

	status_t status = B_OK;
	if ((flags & X86_SUSPEND_SKIP_DEVICES) == 0)
		status = device_manager_suspend(ACPI_POWER_STATE_SLEEP_S3, deviceFlags);

	if (status == B_OK) {
		verbose_step(flags, "devices suspended");

		// Give the devices a moment to settle: without it, hardware that
		// still has transfers in flight (the network card in particular)
		// does not come back reliably.
		snooze(500000);
		status = acpi->prepare_sleep_state(ACPI_POWER_STATE_SLEEP_S3,
			(void (*)(void))(addr_t)kWakeupCodePage, 0);
		if (status != B_OK)
			INFO("preparing the sleep state failed: %s\n", strerror(status));
	}

	if (status == B_OK)
		verbose_step(flags, "sleep state prepared");

	checkpoint(20);

	// From here on the application processors are halted, so nothing that
	// waits for them (an inter-processor call, for instance) may run: park
	// them all at once with interrupts already disabled.
	cpu_status state = disable_interrupts();

	int32 parkedCount = 1;
	if (status == B_OK) {
		CPUSet parkSet;
		for (int32 cpu = 1; cpu < cpuCount; cpu++) {
			atomic_set(&sParked[cpu], 0);
			parkSet.SetBit(cpu);
		}

		smp_multicast_ici_interrupts_disabled(0, parkSet,
			SMP_MSG_CALL_FUNCTION, 0, 0, 0, (void*)&park_cpu,
			SMP_MSG_FLAG_ASYNC);

		for (int32 cpu = 1; cpu < cpuCount; cpu++) {
			if (wait_for_flag(&sParked[cpu], 1000000, false) != B_OK) {
				INFO("CPU %" B_PRId32 " did not park\n", cpu);
				status = B_TIMED_OUT;
				break;
			}
			parkedCount++;
		}
	}

	if (status == B_OK) {
		verbose_step(flags, "application processors parked");
		checkpoint(21);
	}

	bool resumed = false;
	if (status == B_OK) {
		x86_suspend_context* context = &sContexts[0];
		save_msrs(context, 0);
		ioapic_suspend();

		iommu_save();

		sAdjustTSC = true;
		context->tsc = x86_read_msr(IA32_MSR_TSC);
		prepare_wakeup_trampoline(x86_read_cr3(), context);

		if (x86_suspend_save_context(context) == 0) {
			acpi->enter_sleep_state(ACPI_POWER_STATE_SLEEP_S3);
			// only returns on failure
			sAdjustTSC = false;
			status = B_ERROR;
		} else
			resumed = true;
	}

	if (resumed) {
		iommu_restore();
		checkpoint(2);

		// Mask the legacy PICs again, the firmware may have unmasked them.
		out8(0xff, 0x21);
		out8(0xff, 0xa1);
		ioapic_resume();
		arch_timer_set_hardware_timer(1000);
	}

	if (resumed) {
		write_resume_marker(0x11);
		checkpoint(3);
	}

	int32 restartedCount = 0;
	for (int32 cpu = 1; cpu < parkedCount; cpu++) {
		if (start_cpu(cpu) == B_OK)
			restartedCount++;
	}
	if (resumed)
		write_resume_marker(0x40 | (restartedCount & 0x3f));
	sAdjustTSC = false;

	if (resumed) {
		checkpoint(4);
		acpi->leave_sleep_state(ACPI_POWER_STATE_SLEEP_S3, true);
		checkpoint(5);
	}

	restore_interrupts(state);

	if (resumed) {
		checkpoint(6);
		acpi->leave_sleep_state(ACPI_POWER_STATE_SLEEP_S3, false);
		checkpoint(7);
		rtc_resync_from_hardware();
		INFO("resumed from S3\n");
		checkpoint(8);
		snooze(1000000);
		checkpoint(9);
	}

	thread_unpin_from_current_cpu(thread_get_current_thread());
	for (int32 cpu = 1; cpu < cpuCount; cpu++) {
		if (wasEnabled[cpu])
			cpu_set_enabled(cpu, true);
	}

	if (resumed)
		checkpoint(10);

	if (resumed)
		write_resume_marker(0x13);

	if ((flags & X86_SUSPEND_SKIP_DEVICES) == 0)
		device_manager_resume(deviceFlags);

	if (resumed)
		write_resume_marker(0x14);

	if (resumed)
		verbose_step(flags, "devices resumed");

	device_manager_set_suspend_verbose(false);

	if (resumed)
		checkpoint(11);

	if (resumed && (flags & X86_SUSPEND_POWER_OFF_ON_CPU_ERROR) != 0
		&& restartedCount != parkedCount - 1) {
		// The only way to report this on a machine whose devices did not
		// come back: power off, so that the failure is visible from outside.
		INFO("only %" B_PRId32 " of %" B_PRId32 " application processors "
			"restarted, powering off\n", restartedCount, parkedCount - 1);
		snooze(2000000);
		acpi->prepare_sleep_state(ACPI_POWER_STATE_OFF, NULL, 0);
		acpi->enter_sleep_state(ACPI_POWER_STATE_OFF);
	}

	if (resumed) {
		save_trace_to_memory();

		if ((flags & X86_SUSPEND_REBOOT_AFTER_RESUME) != 0) {
			INFO("rebooting after resume\n");
			snooze(15000000);
			save_trace_to_memory();
			arch_cpu_shutdown(true);
		}
	}

	if (resumed && (flags & X86_SUSPEND_POWER_OFF_AFTER_RESUME) != 0) {
		snooze(5000000);
		INFO("powering off after resume\n");
		_kern_sync();
		snooze(2000000);
		acpi->prepare_sleep_state(ACPI_POWER_STATE_OFF, NULL, 0);
		acpi->enter_sleep_state(ACPI_POWER_STATE_OFF);
	}

	return resumed ? B_OK : status;
}


static status_t
suspend_syscall(const char* subsystem, uint32 function, void* buffer,
	size_t bufferSize)
{
	if (geteuid() != 0)
		return B_PERMISSION_DENIED;

	switch (function) {
		case X86_SUSPEND_RESTART_CPU:
		{
			int32 cpu;
			if (bufferSize != sizeof(cpu) || !IS_USER_ADDRESS(buffer)
				|| user_memcpy(&cpu, buffer, sizeof(cpu)) != B_OK) {
				return B_BAD_ADDRESS;
			}
			return x86_suspend_restart_cpu(cpu);
		}

		case X86_SUSPEND_GET_TRACE:
		{
			const size_t size = 16384;
			char* trace = (char*)malloc(size);
			if (trace == NULL)
				return B_NO_MEMORY;
			MemoryDeleter traceDeleter(trace);
			size_t length = device_manager_get_suspend_trace(trace, size);
			if (bufferSize < length + 1 || !IS_USER_ADDRESS(buffer))
				return B_BAD_VALUE;
			return user_memcpy(buffer, trace, length + 1);
		}

		case X86_SUSPEND_ENTER_S2IDLE:
		{
			// (callers built before s0i3_blockers pass less)
			x86_suspend_s2idle_args args = {};
			if (bufferSize > sizeof(args)
				|| bufferSize < offsetof(x86_suspend_s2idle_args, s0i3_blockers)
				|| !IS_USER_ADDRESS(buffer)
				|| user_memcpy(&args, buffer, bufferSize) != B_OK) {
				return B_BAD_ADDRESS;
			}
			status_t status = x86_suspend_enter_s2idle(&args);
			if (status == B_OK
				&& user_memcpy(buffer, &args, bufferSize) != B_OK) {
				return B_BAD_ADDRESS;
			}
			return status;
		}

		case X86_SUSPEND_ENTER_S3:
		{
			x86_suspend_s3_args args = {};
			if ((bufferSize != sizeof(args.flags) && bufferSize != sizeof(args))
				|| !IS_USER_ADDRESS(buffer)
				|| user_memcpy(&args, buffer, bufferSize) != B_OK) {
				return B_BAD_ADDRESS;
			}
			return x86_suspend_enter_s3(&args);
		}
	}

	return B_BAD_VALUE;
}


status_t
x86_suspend_init(void)
{
	STATIC_ASSERT(offsetof(x86_suspend_context, gdtr) == 128);

	print_saved_trace();
	print_saved_progress();

	uint8 marker = read_resume_marker();
	if (marker != 0) {
		INFO("resume marker of the last attempt: %#x\n", marker);
		write_resume_marker(0);
	}

	check_features();

	return register_generic_syscall(X86_SUSPEND_SYSCALLS, &suspend_syscall,
		X86_SUSPEND_SYSCALLS_VERSION, 0);
}
