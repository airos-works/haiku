/*
 * Copyright 2026, air/OS. All rights reserved.
 * Copyright 2014-2022 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The Raspberry Pi boot platform: the loader started directly by the
	VideoCore firmware, with no UEFI or U-Boot in between. See
	docs/rpi4/BOOT.md. */


#include <string.h>

#include <KernelExport.h>

#include <arch/cpu.h>
#include <boot/kernel_args.h>
#include <boot/platform.h>
#include <boot/stage2.h>
#include <boot/stdio.h>
#include <kernel.h>

#include "console.h"
#include "cpu.h"
#include "debug.h"
#include "dtb.h"
#include "mailbox.h"
#include "mmu.h"
#include "serial.h"
#include "smp.h"
#include "video.h"


extern void (*__ctor_list)(void);
extern void (*__ctor_end)(void);

extern "C" int main(stage2_args* args);
extern "C" void rpi_start(void* fdt);
extern status_t add_safe_mode_settings(const char* settings);
extern "C" void arch_enter_kernel(struct kernel_args* kernelArgs,
	addr_t kernelEntry, addr_t kernelStackTop, uint32 cpu);


static uint32 sBootOptions;


static void
call_ctors(void)
{
	void (**f)(void);

	for (f = &__ctor_list; f < &__ctor_end; f++)
		(**f)();
}


/*!	Whether the kernel command line (cmdline.txt) has \a option as a word of
	its own. The firmware adds arguments meant for Linux; ours all start
	with "airos.".
*/
static bool
has_boot_argument(const char* option)
{
	const char* arguments = fdt_boot_arguments();
	size_t length = strlen(option);

	while (*arguments != '\0') {
		while (*arguments == ' ')
			arguments++;

		const char* end = arguments;
		while (*end != '\0' && *end != ' ' && *end != '\n')
			end++;

		if ((size_t)(end - arguments) == length
			&& strncmp(arguments, option, length) == 0) {
			return true;
		}

		arguments = *end != '\0' ? end + 1 : end;
	}

	return false;
}


/*!	"airos.watchdog": the power manager's watchdog restarts the board if the
	loader or the kernel stops for 15 s. The kernel keeps it from firing for
	as long as its timers run. For a lab board nobody can switch off.
*/
static void
init_watchdog()
{
	if (!has_boot_argument("airos.watchdog"))
		return;

	static const char* const kCompatible[] = {"brcm,bcm2712-pm",
		"brcm,bcm2711-pm", "brcm,bcm2835-pm", "brcm,bcm2835-pm-wdt"};
	addr_range pm;
	for (size_t i = 0; i < B_COUNT_OF(kCompatible); i++) {
		if (fdt_find_compatible_reg(kCompatible[i], pm)) {
			watchdog_init(pm.start);
			fdt_set_watchdog(pm.start);
			return;
		}
	}
}


static void
init_boot_options()
{
	sBootOptions = 0;

	if (has_boot_argument("airos.menu"))
		sBootOptions |= BOOT_OPTION_MENU;
	if (has_boot_argument("airos.debug")) {
		sBootOptions |= BOOT_OPTION_DEBUG_OUTPUT;
		gDebugToScreen = true;
	}

	// a space on the serial console opens the menu
	bigtime_t timeout = system_time() + 100000;
	while (system_time() < timeout) {
		if (serial_getc(false) == ' ')
			sBootOptions |= BOOT_OPTION_MENU;
	}
}


extern "C" uint32
platform_boot_options(void)
{
	return sBootOptions;
}


template<class T> static void
convert_preloaded_image(preloaded_image* _image)
{
	T* image = static_cast<T*>(_image);
	fix_address(image->next);
	fix_address(image->name);
	fix_address(image->debug_string_table);
	fix_address(image->syms);
	fix_address(image->rel);
	fix_address(image->rela);
	fix_address(image->pltrel);
	fix_address(image->debug_symbols);
}


/*!	Convert all addresses in kernel_args to virtual addresses. */
static void
convert_kernel_args()
{
	fix_address(gKernelArgs.boot_volume);
	fix_address(gKernelArgs.vesa_modes);
	fix_address(gKernelArgs.edid_info);
	fix_address(gKernelArgs.debug_output);
	fix_address(gKernelArgs.boot_splash);
	fix_address(gKernelArgs.arch_args.fdt);

	convert_preloaded_image<preloaded_elf64_image>(gKernelArgs.kernel_image);
	fix_address(gKernelArgs.kernel_image);

	// Iterate over the preloaded images. Must save the next address before
	// converting, as the next pointer will be converted.
	preloaded_image* image = gKernelArgs.preloaded_images;
	fix_address(gKernelArgs.preloaded_images);
	while (image != NULL) {
		preloaded_image* next = image->next;
		convert_preloaded_image<preloaded_elf64_image>(image);
		image = next;
	}

	// Fix driver settings files.
	driver_settings_file* file = gKernelArgs.driver_settings;
	fix_address(gKernelArgs.driver_settings);
	while (file != NULL) {
		driver_settings_file* next = file->next;
		fix_address(file->next);
		fix_address(file->buffer);
		file = next;
	}
}


extern "C" void
platform_start_kernel(void)
{
	if (gKernelArgs.kernel_image->elf_class != ELFCLASS64)
		panic("The kernel is not a 64-bit image");

	// "airos.debug" also has the kernel write its debug output to the
	// screen: on a board without a serial cable that is all there is to see.
	if ((sBootOptions & BOOT_OPTION_DEBUG_OUTPUT) != 0)
		add_safe_mode_settings("debug_screen true\n");

	smp_init_other_cpus();
	fdt_set_kernel_args();

	preloaded_elf64_image* image = static_cast<preloaded_elf64_image*>(
		gKernelArgs.kernel_image.Pointer());
	addr_t kernelEntry = image->elf_header.e_entry;

	dprintf("kernel:\n");
	dprintf("  text: %#" B_PRIx64 ", %#" B_PRIx64 "\n",
		(uint64)image->text_region.start, (uint64)image->text_region.size);
	dprintf("  data: %#" B_PRIx64 ", %#" B_PRIx64 "\n",
		(uint64)image->data_region.start, (uint64)image->data_region.size);
	dprintf("  entry: %#lx\n", kernelEntry);

	debug_cleanup();

	convert_kernel_args();

	void* stack = NULL;
	const size_t stackSize = KERNEL_STACK_SIZE
		+ KERNEL_STACK_GUARD_PAGES * B_PAGE_SIZE;
	if (platform_allocate_region(&stack, stackSize, 0) != B_OK)
		panic("Unable to allocate a stack");
	gKernelArgs.cpu_kstack[0].start = fix_address((addr_t)stack);
	gKernelArgs.cpu_kstack[0].size = stackSize;

	mmu_init_for_kernel();

	dprintf("Entering the kernel at %#" B_PRIxADDR " with %" B_PRIu32
		" CPUs\n", kernelEntry, gKernelArgs.num_cpus);

	serial_flush();
	watchdog_pet();
	smp_boot_other_cpus(kernelEntry);

	arch_enter_kernel(&gKernelArgs, kernelEntry,
		gKernelArgs.cpu_kstack[0].start + gKernelArgs.cpu_kstack[0].size, 0);

	panic("The kernel returned");
}


extern "C" void
platform_exit(void)
{
	// There is nothing to return to; the watchdog would restart the board.
	dprintf("Halted.\n");
	while (true)
		asm("wfe");
}


extern "C" void
rpi_start(void* fdt)
{
	// Nothing else may run before this: with the MMU off, unaligned
	// accesses fault.
	mmu_init_early();

	stage2_args args;
	memset(&args, 0, sizeof(args));

	call_ctors();

	fdt_init(fdt);
	init_watchdog();
	mmu_init();

	video_init();
	console_init();
	init_boot_options();

	cpu_init();
	smp_init();

	main(&args);

	platform_exit();
}
