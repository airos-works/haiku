/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*	The device-level glue of drm/imagination on Haiku, in place of
	pvr_device.c and pvr_drv.c: a struct pvr_device for the reused code,
	the GPU ID, the firmware image, the firmware start (pvr_fw_init()) and
	the checks that it runs, the interrupt work, and diagnostics.

	Everything is logged with "powervr: " for the serial log: a failed
	start is diagnosed from the log of one boot. */


#include "pvr_ccb.h"
#include "pvr_context.h"
#include "pvr_device.h"
#include "pvr_device_info.h"
#include "pvr_fw.h"
#include "pvr_fw_info.h"
#include "pvr_fw_mips.h"
#include "pvr_fw_startstop.h"
#include "pvr_fw_trace.h"
#include "pvr_gem.h"
#include "pvr_haiku_device.h"
#include "pvr_mmu.h"
#include "pvr_power.h"
#include "pvr_queue.h"
#include "pvr_rogue_cr_defs.h"
#include "pvr_rogue_fwif.h"
#include "pvr_rogue_heap_config.h"
#include "pvr_rogue_mips.h"


#define TRACE(x...)			dprintf("powervr: " x)

/* pvr_device.c's PVR_FW_VERSION_MAJOR: the firmware file name's "v1". */
#define PVR_FW_VERSION_MAJOR	1

#define OSID_COUNT				8
#define OSID_REGISTER_STRIDE	0x10000

#define HEALTH_CHECKS			3

/* Allwinner's DDK, sunxi_platform.c: sunxi_secure_config() */
#define SUNXI_SMC_BASE			0x0a000000
#define SUNXI_SMC_DRM_GPU_HW_RST	0xb0
#define SUNXI_GPU_GLB_DRM_CTRL	0x80014		/* GPU_GLB + 0x14 */


/* The firmware trace's initial log groups (pvr_fw_trace.c). */
extern const struct kernel_param lx_param_init_fw_trace_mask;


/* #pragma mark - register names and tracing */


static const struct {
	u32			offset;
	const char*	name;
} kRegisterNames[] = {
	{ ROGUE_CR_CLK_CTRL, "CLK_CTRL" },
	{ ROGUE_CR_CLK_STATUS, "CLK_STATUS" },
	{ ROGUE_CR_CORE_ID, "CORE_ID" },
	{ ROGUE_CR_CORE_ID__PBVNC, "CORE_ID__PBVNC" },
	{ ROGUE_CR_SOFT_RESET, "SOFT_RESET" },
	{ ROGUE_CR_SOFT_RESET2, "SOFT_RESET2" },
	{ ROGUE_CR_EVENT_STATUS, "EVENT_STATUS" },
	{ ROGUE_CR_EVENT_CLEAR, "EVENT_CLEAR" },
	{ ROGUE_CR_TIMER, "TIMER" },
	{ ROGUE_CR_SIDEKICK_IDLE, "SIDEKICK_IDLE" },
	{ ROGUE_CR_MIPS_WRAPPER_CONFIG, "MIPS_WRAPPER_CONFIG" },
	{ ROGUE_CR_MIPS_ADDR_REMAP1_CONFIG1, "MIPS_ADDR_REMAP1_CONFIG1" },
	{ ROGUE_CR_MIPS_ADDR_REMAP1_CONFIG2, "MIPS_ADDR_REMAP1_CONFIG2" },
	{ ROGUE_CR_MIPS_ADDR_REMAP2_CONFIG1, "MIPS_ADDR_REMAP2_CONFIG1" },
	{ ROGUE_CR_MIPS_ADDR_REMAP2_CONFIG2, "MIPS_ADDR_REMAP2_CONFIG2" },
	{ ROGUE_CR_MIPS_ADDR_REMAP3_CONFIG1, "MIPS_ADDR_REMAP3_CONFIG1" },
	{ ROGUE_CR_MIPS_ADDR_REMAP3_CONFIG2, "MIPS_ADDR_REMAP3_CONFIG2" },
	{ ROGUE_CR_MIPS_ADDR_REMAP4_CONFIG1, "MIPS_ADDR_REMAP4_CONFIG1" },
	{ ROGUE_CR_MIPS_ADDR_REMAP4_CONFIG2, "MIPS_ADDR_REMAP4_CONFIG2" },
	{ ROGUE_CR_MIPS_ADDR_REMAP5_CONFIG1, "MIPS_ADDR_REMAP5_CONFIG1" },
	{ ROGUE_CR_MIPS_ADDR_REMAP5_CONFIG2, "MIPS_ADDR_REMAP5_CONFIG2" },
	{ ROGUE_CR_MIPS_ADDR_REMAP_UNMAPPED_STATUS,
		"MIPS_ADDR_REMAP_UNMAPPED_STATUS" },
	{ ROGUE_CR_MIPS_WRAPPER_IRQ_ENABLE, "MIPS_WRAPPER_IRQ_ENABLE" },
	{ ROGUE_CR_MIPS_WRAPPER_IRQ_STATUS, "MIPS_WRAPPER_IRQ_STATUS" },
	{ ROGUE_CR_MIPS_WRAPPER_IRQ_CLEAR, "MIPS_WRAPPER_IRQ_CLEAR" },
	{ ROGUE_CR_MIPS_DEBUG_CONFIG, "MIPS_DEBUG_CONFIG" },
	{ ROGUE_CR_MIPS_EXCEPTION_STATUS, "MIPS_EXCEPTION_STATUS" },
	{ ROGUE_CR_MIPS_WRAPPER_STATUS, "MIPS_WRAPPER_STATUS" },
	{ ROGUE_CR_MARS_IDLE, "MARS_IDLE" },
	{ ROGUE_CR_MTS_SCHEDULE, "MTS_SCHEDULE" },
	{ ROGUE_CR_MTS_BGCTX_THREAD0_DM_ASSOC, "MTS_BGCTX_THREAD0_DM_ASSOC" },
	{ ROGUE_CR_MTS_INTCTX_THREAD0_DM_ASSOC, "MTS_INTCTX_THREAD0_DM_ASSOC" },
	{ ROGUE_CR_MTS_GARTEN_WRAPPER_CONFIG, "MTS_GARTEN_WRAPPER_CONFIG" },
	{ ROGUE_CR_IRQ_OS0_EVENT_STATUS, "IRQ_OS0_EVENT_STATUS" },
	{ ROGUE_CR_IRQ_OS0_EVENT_CLEAR, "IRQ_OS0_EVENT_CLEAR" },
	{ ROGUE_CR_BIF_FAULT_BANK0_MMU_STATUS, "BIF_FAULT_BANK0_MMU_STATUS" },
	{ ROGUE_CR_BIF_FAULT_BANK0_REQ_STATUS, "BIF_FAULT_BANK0_REQ_STATUS" },
	{ ROGUE_CR_BIF_READS_EXT_STATUS, "BIF_READS_EXT_STATUS" },
	{ ROGUE_CR_BIFPM_READS_EXT_STATUS, "BIFPM_READS_EXT_STATUS" },
	{ ROGUE_CR_BIFPM_STATUS_MMU, "BIFPM_STATUS_MMU" },
	{ ROGUE_CR_BIF_STATUS_MMU, "BIF_STATUS_MMU" },
	{ ROGUE_CR_OS0_SCRATCH0, "OS0_SCRATCH0" },
	{ ROGUE_CR_OS0_SCRATCH1, "OS0_SCRATCH1" },
	{ ROGUE_CR_OS0_SCRATCH2, "OS0_SCRATCH2" },
	{ ROGUE_CR_OS0_SCRATCH3, "OS0_SCRATCH3" },
	{ ROGUE_CR_SLC_CTRL_MISC, "SLC_CTRL_MISC" },
	{ ROGUE_CR_SLC_STATUS1, "SLC_STATUS1" },
	{ ROGUE_CR_SLC_IDLE, "SLC_IDLE" },
	{ ROGUE_CR_AXI_ACE_LITE_CONFIGURATION, "AXI_ACE_LITE_CONFIGURATION" },
	{ ROGUE_CR_SYS_BUS_SECURE, "SYS_BUS_SECURE" },
};


static const char*
register_name(u32 offset)
{
	for (size_t i = 0; i < ARRAY_SIZE(kRegisterNames); i++) {
		if (kRegisterNames[i].offset == offset)
			return kRegisterNames[i].name;
	}
	return "";
}


#define MMIO_TRACE_MAX_LINES	200

/*	Register tracing of the firmware start: every access the boot thread
	makes through ioread*()/iowrite*() while it runs, with repeated
	identical accesses (polls) folded into one line. Also notes when the
	MIPS is let out of reset (SOFT_RESET written 0), to time its boot. */
static struct {
	struct pvr_haiku_device*	device;
	thread_id					thread;
	bool						log;
	u32							lines;
	bool						have_last;
	u32							last_offset;
	u64							last_value;
	unsigned int				last_bits;
	bool						last_write;
	u32							repeats;
} sTrace;


static void
mmio_trace_print(u32 offset, u64 value, unsigned int bits, bool write,
	u32 repeats)
{
	if (sTrace.lines > MMIO_TRACE_MAX_LINES)
		return;
	if (sTrace.lines++ == MMIO_TRACE_MAX_LINES) {
		TRACE("mmio: (%u lines, the trace stops here)\n",
			MMIO_TRACE_MAX_LINES);
		return;
	}

	const char* name = register_name(offset % OSID_REGISTER_STRIDE);
	char repeated[24] = "";
	if (repeats > 0)
		snprintf(repeated, sizeof(repeated), " (x%u)", repeats + 1);
	if (bits == 64) {
		TRACE("mmio: %s64 %#07x %-31s %#018llx%s\n", write ? "w" : "r",
			offset, name, value, repeated);
	} else {
		TRACE("mmio: %s32 %#07x %-31s %#010llx%s\n", write ? "w" : "r",
			offset, name, value, repeated);
	}
}


static void
mmio_trace_flush(void)
{
	if (sTrace.have_last) {
		mmio_trace_print(sTrace.last_offset, sTrace.last_value,
			sTrace.last_bits, sTrace.last_write, sTrace.repeats);
	}
	sTrace.have_last = false;
	sTrace.repeats = 0;
}


void
lx_mmio_trace(const volatile void* address, u64 value, unsigned int bits,
	bool write)
{
	const volatile u8* base = (const volatile u8*)lx_mmio_trace_base;
	const volatile u8* access = (const volatile u8*)address;
	if (base == NULL || sTrace.device == NULL
		|| find_thread(NULL) != sTrace.thread || access < base
		|| access >= base + sTrace.device->register_size) {
		return;
	}
	u32 offset = (u32)(access - base);

	if (write && offset == ROGUE_CR_SOFT_RESET && value == 0
		&& !sTrace.device->reset_released) {
		sTrace.device->reset_released = true;
		sTrace.device->reset_release_time = system_time();
	}
	if (!sTrace.log)
		return;

	if (sTrace.have_last && sTrace.last_offset == offset
		&& sTrace.last_value == value && sTrace.last_bits == bits
		&& sTrace.last_write == write) {
		sTrace.repeats++;
		return;
	}
	mmio_trace_flush();
	sTrace.have_last = true;
	sTrace.last_offset = offset;
	sTrace.last_value = value;
	sTrace.last_bits = bits;
	sTrace.last_write = write;
}


static void
mmio_trace_start(struct pvr_haiku_device* device, bool log)
{
	memset(&sTrace, 0, sizeof(sTrace));
	sTrace.device = device;
	sTrace.thread = find_thread(NULL);
	sTrace.log = log;
	lx_mmio_trace_base = device->pvr.regs;
}


static void
mmio_trace_stop(void)
{
	if (lx_mmio_trace_base == NULL)
		return;
	mmio_trace_flush();
	lx_mmio_trace_base = NULL;
	sTrace.device = NULL;
}


/* #pragma mark - device */


struct pvr_device*
pvr_haiku_device_create(const pvr_haiku_platform* platform)
{
	struct pvr_haiku_device* device
		= (struct pvr_haiku_device*)kzalloc(sizeof(*device), GFP_KERNEL);
	if (device == NULL)
		return NULL;
	struct pvr_device* pvr_dev = &device->pvr;

	device->device.name = "powervr";
	device->device.dma_mask = DMA_BIT_MASK(32);
	lx_drm_dev_init(&pvr_dev->base, &device->device);

	device->registers.start = platform->register_base;
	device->registers.end = platform->register_base
		+ platform->register_size - 1;
	device->register_size = platform->register_size;
	device->core_clock.rate = platform->core_clock;
	device->firmware_status = B_NO_INIT;

	pvr_dev->regs = (void __iomem*)(uintptr_t)platform->registers;
	pvr_dev->regs_resource = &device->registers;
	pvr_dev->core_clk = &device->core_clock;
	pvr_dev->irq = -1;

	// what pvr_device_init() and pvr_device_irq_init() set up for the
	// reused code
	init_waitqueue_head(&pvr_dev->kccb.rtn_q);
	init_rwsem(&pvr_dev->reset_sem);
	atomic_set(&pvr_dev->mmu_flush_cache_flags, 0);

	// and pvr_probe() for the ioctls: contexts, job queues and their
	// scheduler thread, job and free list IDs, resets
	lx_dma_fence_init_globals();
	pvr_haiku_power_init(pvr_dev);
	pvr_context_device_init(pvr_dev);
	if (pvr_queue_device_init(pvr_dev) != 0) {
		pvr_context_device_fini(pvr_dev);
		lx_drm_dev_release(&pvr_dev->base);
		kfree(device);
		return NULL;
	}
	xa_init_flags(&pvr_dev->free_list_ids, XA_FLAGS_ALLOC1);
	xa_init_flags(&pvr_dev->job_ids, XA_FLAGS_ALLOC1);
	atomic_set(&device->irq_count, 0);
	atomic_set(&device->irq_spurious, 0);
	return pvr_dev;
}


void
pvr_haiku_device_delete(struct pvr_device* pvr_dev)
{
	if (pvr_dev == NULL)
		return;
	struct pvr_haiku_device* device = to_haiku_device(pvr_dev);

	xa_destroy(&pvr_dev->job_ids);
	xa_destroy(&pvr_dev->free_list_ids);
	pvr_queue_device_fini(pvr_dev);
	pvr_context_device_fini(pvr_dev);
	release_firmware(pvr_dev->fw_dev.firmware);
	lx_drm_dev_release(&pvr_dev->base);
	rw_lock_destroy(&pvr_dev->reset_sem.lock);
	kfree(device);
	// page-table pages waiting for reuse; none of them is in use
	lx_free_pages_flush();
}


/* #pragma mark - features (pvr_device.c, unchanged) */


bool
pvr_device_has_uapi_quirk(struct pvr_device *pvr_dev, u32 quirk)
{
	switch (quirk) {
	case 47217:
		return PVR_HAS_QUIRK(pvr_dev, 47217);
	case 48545:
		return PVR_HAS_QUIRK(pvr_dev, 48545);
	case 49927:
		return PVR_HAS_QUIRK(pvr_dev, 49927);
	case 51764:
		return PVR_HAS_QUIRK(pvr_dev, 51764);
	case 62269:
		return PVR_HAS_QUIRK(pvr_dev, 62269);
	default:
		return false;
	};
}

bool
pvr_device_has_uapi_enhancement(struct pvr_device *pvr_dev, u32 enhancement)
{
	switch (enhancement) {
	case 35421:
		return PVR_HAS_ENHANCEMENT(pvr_dev, 35421);
	case 42064:
		return PVR_HAS_ENHANCEMENT(pvr_dev, 42064);
	default:
		return false;
	};
}

/**
 * pvr_device_has_feature() - Look up device feature based on feature definition
 * @pvr_dev: Device pointer.
 * @feature: Feature to look up. Should be one of %PVR_FEATURE_*.
 *
 * Returns:
 *  * %true if feature is present on device, or
 *  * %false if feature is not present on device.
 */
bool
pvr_device_has_feature(struct pvr_device *pvr_dev, u32 feature)
{
	switch (feature) {
	case PVR_FEATURE_CLUSTER_GROUPING:
		return PVR_HAS_FEATURE(pvr_dev, cluster_grouping);

	case PVR_FEATURE_COMPUTE_MORTON_CAPABLE:
		return PVR_HAS_FEATURE(pvr_dev, compute_morton_capable);

	case PVR_FEATURE_FB_CDC_V4:
		return PVR_HAS_FEATURE(pvr_dev, fb_cdc_v4);

	case PVR_FEATURE_GPU_MULTICORE_SUPPORT:
		return PVR_HAS_FEATURE(pvr_dev, gpu_multicore_support);

	case PVR_FEATURE_ISP_ZLS_D24_S8_PACKING_OGL_MODE:
		return PVR_HAS_FEATURE(pvr_dev, isp_zls_d24_s8_packing_ogl_mode);

	case PVR_FEATURE_S7_TOP_INFRASTRUCTURE:
		return PVR_HAS_FEATURE(pvr_dev, s7_top_infrastructure);

	case PVR_FEATURE_TESSELLATION:
		return PVR_HAS_FEATURE(pvr_dev, tessellation);

	case PVR_FEATURE_TPU_DM_GLOBAL_REGISTERS:
		return PVR_HAS_FEATURE(pvr_dev, tpu_dm_global_registers);

	case PVR_FEATURE_VDM_DRAWINDIRECT:
		return PVR_HAS_FEATURE(pvr_dev, vdm_drawindirect);

	case PVR_FEATURE_VDM_OBJECT_LEVEL_LLS:
		return PVR_HAS_FEATURE(pvr_dev, vdm_object_level_lls);

	case PVR_FEATURE_ZLS_SUBTILE:
		return PVR_HAS_FEATURE(pvr_dev, zls_subtile);

	/* Derived features. */
	case PVR_FEATURE_CDM_USER_MODE_QUEUE: {
		u8 cdm_control_stream_format = 0;

		PVR_FEATURE_VALUE(pvr_dev, cdm_control_stream_format, &cdm_control_stream_format);
		return (cdm_control_stream_format >= 2 && cdm_control_stream_format <= 4);
	}

	case PVR_FEATURE_REQUIRES_FB_CDC_ZLS_SETUP:
		if (PVR_HAS_FEATURE(pvr_dev, fbcdc_algorithm)) {
			u8 fbcdc_algorithm = 0;

			PVR_FEATURE_VALUE(pvr_dev, fbcdc_algorithm, &fbcdc_algorithm);
			return (fbcdc_algorithm < 3 || PVR_HAS_FEATURE(pvr_dev, fb_cdc_v4));
		}
		return false;

	default:
		WARN(true, "Looking up undefined feature %u\n", feature);
		return false;
	}
}



/*!	pvr_device.c's pvr_gpuid_decode_reg(): the BVNC from the PBVNC register,
	or from CORE_REVISION and CORE_ID on cores that have none.
*/
uint64
pvr_haiku_identify(struct pvr_device* pvr_dev)
{
	struct pvr_gpu_id* gpu_id = &pvr_dev->gpu_id;
	u64 bvnc = pvr_cr_read64(pvr_dev, ROGUE_CR_CORE_ID__PBVNC);

	gpu_id->b = PVR_CR_FIELD_GET(bvnc, CORE_ID__PBVNC__BRANCH_ID);
	if (gpu_id->b != 0) {
		gpu_id->v = PVR_CR_FIELD_GET(bvnc, CORE_ID__PBVNC__VERSION_ID);
		gpu_id->n = PVR_CR_FIELD_GET(bvnc,
			CORE_ID__PBVNC__NUMBER_OF_SCALABLE_UNITS);
		gpu_id->c = PVR_CR_FIELD_GET(bvnc, CORE_ID__PBVNC__CONFIG_ID);
	} else {
		u32 core_rev = pvr_cr_read32(pvr_dev, ROGUE_CR_CORE_REVISION);
		u32 core_id = pvr_cr_read32(pvr_dev, ROGUE_CR_CORE_ID);
		u16 core_id_config = PVR_CR_FIELD_GET(core_id, CORE_ID_CONFIG);

		gpu_id->b = PVR_CR_FIELD_GET(core_rev, CORE_REVISION_MAJOR);
		gpu_id->v = PVR_CR_FIELD_GET(core_rev, CORE_REVISION_MINOR);
		gpu_id->n = FIELD_GET(0xFF00, core_id_config);
		gpu_id->c = FIELD_GET(0x00FF, core_id_config);
	}
	return pvr_gpu_id_to_packed_bvnc(gpu_id);
}


/* #pragma mark - firmware image */


/*!	pvr_fw_validate() checks the info header at the end of the image;
	pvr_fw_process_elf_command_stream() then takes the ELF program headers
	at their word. This checks them against the file first.
*/
static int
check_firmware_elf(const struct firmware* firmware)
{
	const struct elf32_hdr* header = (const struct elf32_hdr*)firmware->data;
	if (firmware->size < SZ_4K + sizeof(*header)
		|| memcmp(header->e_ident, "\177ELF", 4) != 0
		|| header->e_ident[4] != 1 || header->e_ident[5] != 1) {
		TRACE("firmware: not a 32-bit little-endian ELF image\n");
		return -EINVAL;
	}
	if (header->e_phentsize != sizeof(struct elf32_phdr)
		|| header->e_phoff > firmware->size
		|| header->e_phnum
			> (firmware->size - header->e_phoff) / sizeof(struct elf32_phdr)) {
		TRACE("firmware: bad ELF program header table (%u at %#x)\n",
			header->e_phnum, header->e_phoff);
		return -EINVAL;
	}

	const struct elf32_phdr* segment
		= (const struct elf32_phdr*)(firmware->data + header->e_phoff);
	u32 loadCount = 0;
	for (u32 i = 0; i < header->e_phnum; i++, segment++) {
		if (segment->p_type != PT_LOAD)
			continue;
		if (segment->p_filesz > segment->p_memsz
			|| segment->p_offset > firmware->size
			|| segment->p_filesz > firmware->size - segment->p_offset) {
			TRACE("firmware: ELF segment %u (%#x + %#x at %#x) is outside"
				" the file\n", i, segment->p_offset, segment->p_filesz,
				segment->p_vaddr);
			return -EINVAL;
		}
		loadCount++;
	}
	TRACE("firmware: ELF machine %u, entry %#x, %u loadable segments\n",
		header->e_machine, header->e_entry, loadCount);
	return 0;
}


static void
log_device_info(struct pvr_device* pvr_dev)
{
	char quirks[96] = "";
	char enhancements[96] = "";
	size_t length = 0;

#define LOG_QUIRK(number) \
	if (PVR_HAS_QUIRK(pvr_dev, number) && length < sizeof(quirks)) \
		length += snprintf(quirks + length, sizeof(quirks) - length, \
			" %u", number);
	LOG_QUIRK(44079) LOG_QUIRK(47217) LOG_QUIRK(48492) LOG_QUIRK(48545)
	LOG_QUIRK(49927) LOG_QUIRK(50767) LOG_QUIRK(51764) LOG_QUIRK(62269)
	LOG_QUIRK(63142) LOG_QUIRK(63553) LOG_QUIRK(66011) LOG_QUIRK(71242)
#undef LOG_QUIRK

	length = 0;
#define LOG_ENHANCEMENT(number) \
	if (PVR_HAS_ENHANCEMENT(pvr_dev, number) && length < sizeof(enhancements)) \
		length += snprintf(enhancements + length, \
			sizeof(enhancements) - length, " %u", number);
	LOG_ENHANCEMENT(35421) LOG_ENHANCEMENT(38020) LOG_ENHANCEMENT(38748)
	LOG_ENHANCEMENT(42064) LOG_ENHANCEMENT(42290) LOG_ENHANCEMENT(42606)
	LOG_ENHANCEMENT(47025) LOG_ENHANCEMENT(57596)
#undef LOG_ENHANCEMENT

	u64 physBits = 0, virtualBits = 0, clusters = 0, slcSize = 0;
	u64 slcLine = 0, layoutMars = 0, osids = 0;
	PVR_FEATURE_VALUE(pvr_dev, phys_bus_width, &physBits);
	PVR_FEATURE_VALUE(pvr_dev, virtual_address_space_bits, &virtualBits);
	PVR_FEATURE_VALUE(pvr_dev, num_clusters, &clusters);
	PVR_FEATURE_VALUE(pvr_dev, slc_size_in_kilobytes, &slcSize);
	PVR_FEATURE_VALUE(pvr_dev, slc_cache_line_size_bits, &slcLine);
	PVR_FEATURE_VALUE(pvr_dev, layout_mars, &layoutMars);
	PVR_FEATURE_VALUE(pvr_dev, num_osids, &osids);

	TRACE("firmware: device info: %s firmware processor, %llu-bit"
		" physical bus, %llu-bit GPU addresses, %llu cluster(s), SLC %llu"
		" KiB with %llu-bit lines, LAYOUT_MARS %llu, %llu OSIDs%s%s%s%s\n",
		PVR_HAS_FEATURE(pvr_dev, mips) ? "MIPS"
			: PVR_HAS_FEATURE(pvr_dev, meta) ? "META"
			: PVR_HAS_FEATURE(pvr_dev, riscv_fw_processor) ? "RISC-V" : "no",
		physBits, virtualBits, clusters, slcSize, slcLine, layoutMars,
		osids,
		PVR_HAS_FEATURE(pvr_dev, sys_bus_secure_reset)
			? ", SYS_BUS_SECURE_RESET" : "",
		PVR_HAS_FEATURE(pvr_dev, pbe2_in_xe) ? ", PBE2_IN_XE" : "",
		PVR_HAS_FEATURE(pvr_dev, xe_tpu2) ? ", XE_TPU2" : "",
		PVR_HAS_FEATURE(pvr_dev, axi_acelite) ? ", AXI_ACELITE" : "");
	TRACE("firmware: quirks:%s; enhancements:%s\n",
		quirks[0] != '\0' ? quirks : " none",
		enhancements[0] != '\0' ? enhancements : " none");
}


/*!	What pvr_device_gpu_init() does before pvr_fw_init(): the firmware
	file, its validation and device info, the processor type, the DMA mask.
*/
status_t
pvr_haiku_firmware_load(struct pvr_device* pvr_dev)
{
	struct pvr_haiku_device* device = to_haiku_device(pvr_dev);
	const struct pvr_gpu_id* id = &pvr_dev->gpu_id;
	if (device->firmware_loaded)
		return B_OK;

	char name[64];
	snprintf(name, sizeof(name), "powervr/rogue_%u.%u.%u.%u_v%u.fw", id->b,
		id->v, id->n, id->c, PVR_FW_VERSION_MAJOR);

	const struct firmware* firmware;
	int error = request_firmware(&firmware, name, &device->device);
	if (error != 0) {
		TRACE("firmware: %s not found in data/firmware of /boot/system or"
			" /boot/system/non-packaged (%d)\n", name, error);
		return lx_status(error);
	}
	TRACE("firmware: %s, %zu bytes\n", firmware->path, firmware->size);
	pvr_dev->fw_dev.firmware = firmware;

	error = check_firmware_elf(firmware);
	if (error == 0)
		error = pvr_fw_validate_init_device_info(pvr_dev);
	if (error != 0) {
		TRACE("firmware: image refused (%d)\n", error);
		return lx_status(error);
	}
	log_device_info(pvr_dev);

	if (PVR_HAS_FEATURE(pvr_dev, meta))
		pvr_dev->fw_dev.processor_type = PVR_FW_PROCESSOR_TYPE_META;
	else if (PVR_HAS_FEATURE(pvr_dev, mips))
		pvr_dev->fw_dev.processor_type = PVR_FW_PROCESSOR_TYPE_MIPS;
	else if (PVR_HAS_FEATURE(pvr_dev, riscv_fw_processor))
		pvr_dev->fw_dev.processor_type = PVR_FW_PROCESSOR_TYPE_RISCV;
	else
		return B_BAD_DATA;
	if (pvr_dev->fw_dev.processor_type != PVR_FW_PROCESSOR_TYPE_MIPS) {
		TRACE("firmware: only MIPS firmware processors are supported\n");
		return B_NOT_SUPPORTED;
	}

	// pvr_set_dma_info(): everything the GPU sees below 2^phys_bus_width
	u64 physBits = 0;
	if (PVR_FEATURE_VALUE(pvr_dev, phys_bus_width, &physBits) != 0
		|| physBits < 32 || physBits > 64) {
		TRACE("firmware: no usable physical bus width\n");
		return B_BAD_DATA;
	}
	dma_set_mask(&device->device, DMA_BIT_MASK(physBits));

	// pvr_device_safety_irq_init(): ECC RAMs or a watchdog would raise
	// safety events, which the interrupt handler does not take (yet)
	u64 eccRams = 0;
	PVR_FEATURE_VALUE(pvr_dev, ecc_rams, &eccRams);
	pvr_dev->has_safety_events = PVR_HAS_FEATURE(pvr_dev, roguexe)
		&& (eccRams > 0 || PVR_HAS_FEATURE(pvr_dev, watchdog_timer));
	if (pvr_dev->has_safety_events)
		TRACE("firmware: warning: this core raises safety events\n");

	device->firmware_loaded = true;
	return B_OK;
}


/* #pragma mark - interrupts */


static inline u32
raw_read32(struct pvr_device* pvr_dev, u32 offset)
{
	return *(volatile u32*)((volatile u8*)pvr_dev->regs + offset);
}


static inline void
raw_write32(struct pvr_device* pvr_dev, u32 offset, u32 value)
{
	*(volatile u32*)((volatile u8*)pvr_dev->regs + offset) = value;
}


/*!	Allwinner's cleanInterrupt() (sunxi_platform.c) before the handler is
	installed: the MIPS wrapper interrupt, the event status and each OSID's
	interrupt event.
*/
void
pvr_haiku_clear_stale_interrupts(struct pvr_device* pvr_dev)
{
	struct pvr_haiku_device* device = to_haiku_device(pvr_dev);
	u32 wrapper = pvr_cr_read32(pvr_dev, ROGUE_CR_MIPS_WRAPPER_IRQ_STATUS);
	u32 events = pvr_cr_read32(pvr_dev, ROGUE_CR_EVENT_STATUS);
	u32 osEvents[OSID_COUNT] = {};
	for (u32 os = 0; os < OSID_COUNT; os++) {
		u32 offset = os * OSID_REGISTER_STRIDE + ROGUE_CR_IRQ_OS0_EVENT_STATUS;
		if (offset + 4 <= device->register_size)
			osEvents[os] = pvr_cr_read32(pvr_dev, offset);
	}
	TRACE("interrupts before the handler: MIPS wrapper %#x, events %#x, OS"
		" events %#x %#x %#x %#x %#x %#x %#x %#x\n", wrapper, events,
		osEvents[0], osEvents[1], osEvents[2], osEvents[3], osEvents[4],
		osEvents[5], osEvents[6], osEvents[7]);

	pvr_cr_write32(pvr_dev, ROGUE_CR_MIPS_WRAPPER_IRQ_CLEAR,
		ROGUE_CR_MIPS_WRAPPER_IRQ_CLEAR_EVENT_EN);
	pvr_cr_write32(pvr_dev, ROGUE_CR_EVENT_CLEAR, 0xffffffff);
	for (u32 os = 0; os < OSID_COUNT; os++) {
		u32 offset = os * OSID_REGISTER_STRIDE + ROGUE_CR_IRQ_OS0_EVENT_CLEAR;
		if (offset + 4 <= device->register_size) {
			pvr_cr_write32(pvr_dev, offset,
				ROGUE_CR_IRQ_OS0_EVENT_CLEAR_SOURCE_EN);
		}
	}

	TRACE("interrupts cleared: MIPS wrapper %#x, events %#x, OS0 events"
		" %#x\n", pvr_cr_read32(pvr_dev, ROGUE_CR_MIPS_WRAPPER_IRQ_STATUS),
		pvr_cr_read32(pvr_dev, ROGUE_CR_EVENT_STATUS),
		pvr_cr_read32(pvr_dev, ROGUE_CR_IRQ_OS0_EVENT_STATUS));
}


/*!	The hard interrupt handler's part (pvr_device_irq_handler() and the
	irq_clear of pvr_fw_mips.c): a MIPS wrapper event is cleared right
	here, as nothing masks the level-triggered line until the thread runs.
	Raw accesses: never traced.
*/
/*!	pvr_device.c's pvr_device_process_active_queues(): signals the jobs the
	firmware finished and lets waiting ones into their CCCBs.
*/
static void
process_active_queues(struct pvr_device* pvr_dev)
{
	struct pvr_queue* queue;
	struct pvr_queue* next;
	LIST_HEAD(active_queues);

	mutex_lock(&pvr_dev->queues.lock);

	// the queues still active afterwards go back onto queues.active
	list_splice_init(&pvr_dev->queues.active, &active_queues);
	list_for_each_entry_safe(queue, next, &active_queues, node)
		pvr_queue_process(queue);

	mutex_unlock(&pvr_dev->queues.lock);
}


bool
pvr_haiku_interrupt(struct pvr_device* pvr_dev)
{
	struct pvr_haiku_device* device = to_haiku_device(pvr_dev);
	u32 status = raw_read32(pvr_dev, ROGUE_CR_MIPS_WRAPPER_IRQ_STATUS);
	if ((status & ROGUE_CR_MIPS_WRAPPER_IRQ_STATUS_EVENT_EN) == 0) {
		atomic_inc(&device->irq_spurious);
		return false;
	}

	raw_write32(pvr_dev, ROGUE_CR_MIPS_WRAPPER_IRQ_CLEAR,
		ROGUE_CR_MIPS_WRAPPER_IRQ_CLEAR_EVENT_EN);
	// make sure the line is down before the interrupt is ended
	(void)raw_read32(pvr_dev, ROGUE_CR_MIPS_WRAPPER_IRQ_STATUS);
	atomic_inc(&device->irq_count);
	return true;
}


/*!	pvr_device_irq_thread_handler()'s work, after each interrupt. */
void
pvr_haiku_interrupt_work(struct pvr_device* pvr_dev)
{
	// a reset deals with the firmware itself (Linux disables the
	// interrupt meanwhile); a lost device is left alone
	if (!READ_ONCE(pvr_dev->fw_dev.initialised)
		|| atomic_read(&to_haiku_device(pvr_dev)->resetting) != 0
		|| pvr_dev->lost) {
		return;
	}
	pvr_fwccb_process(pvr_dev);
	pvr_kccb_wake_up_waiters(pvr_dev);
	process_active_queues(pvr_dev);
}


/* #pragma mark - firmware start */


static void
vendor_secure_config(struct pvr_device* pvr_dev)
{
	u32 drmControl = pvr_cr_read32(pvr_dev, SUNXI_GPU_GLB_DRM_CTRL);
	TRACE("vendor secure config: SMC %#x = 0x2, GPU_GLB DRM control %#x"
		" (%#x) = 0\n", SUNXI_SMC_BASE + SUNXI_SMC_DRM_GPU_HW_RST,
		(u32)(to_haiku_device(pvr_dev)->registers.start
			+ SUNXI_GPU_GLB_DRM_CTRL),
		drmControl);

	void* smc;
	area_id area = map_physical_memory("powervr smc", SUNXI_SMC_BASE,
		B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &smc);
	if (area < 0) {
		TRACE("vendor secure config: SMC not mapped: %s\n", strerror(area));
		return;
	}
	iowrite32(0x2, (u8*)smc + SUNXI_SMC_DRM_GPU_HW_RST);
	delete_area(area);

	pvr_cr_write32(pvr_dev, SUNXI_GPU_GLB_DRM_CTRL, 0);
	TRACE("vendor secure config: done, DRM control now %#x\n",
		pvr_cr_read32(pvr_dev, SUNXI_GPU_GLB_DRM_CTRL));
}


/*!	Puts the whole GPU, the MIPS included, into soft reset, as the end of
	pvr_fw_stop() does, and keeps the firmware memory from being freed.
*/
void
pvr_haiku_force_reset(struct pvr_device* pvr_dev, const char* why)
{
	u64 mask = PVR_HAS_FEATURE(pvr_dev, pbe2_in_xe)
		? ROGUE_CR_SOFT_RESET__PBE2_XE__MASKFULL : ROGUE_CR_SOFT_RESET_MASKFULL;
	pvr_cr_write64(pvr_dev, ROGUE_CR_SOFT_RESET, mask);
	if (PVR_HAS_FEATURE(pvr_dev, xe_tpu2)) {
		pvr_cr_write64(pvr_dev, ROGUE_CR_SOFT_RESET2,
			ROGUE_CR_SOFT_RESET2_MASKFULL);
	}
	u64 readBack = pvr_cr_read64(pvr_dev, ROGUE_CR_SOFT_RESET);
	to_haiku_device(pvr_dev)->keep_memory = true;
	TRACE("GPU held in soft reset %s (SOFT_RESET %#llx); its firmware memory"
		" is kept until the next boot\n", why, readBack);
}


/*!	Called by pvr_fw_init() (a local change to pvr_fw.c) when the firmware
	does not report itself started: the state is logged while the
	firmware's memory still exists, then the GPU is reset.
*/
void
pvr_haiku_fw_boot_failed(struct pvr_device* pvr_dev)
{
	struct pvr_haiku_device* device = to_haiku_device(pvr_dev);
	mmio_trace_stop();
	device->keep_memory = true;

	TRACE("firmware: firmware_started not set %" B_PRIdBIGTIME " us after"
		" the MIPS left reset\n", device->reset_released
			? system_time() - device->reset_release_time : (bigtime_t)0);
	device->failed_start = true;
	pvr_haiku_dump(pvr_dev, "the firmware did not start",
		device->trace_lines);
	device->failed_start = false;
	pvr_haiku_force_reset(pvr_dev, "after the failed start");
	lx_debugfs_remove_all();
	TRACE("firmware: now pvr_fw_stop() and the clean-up\n");
}


static void
log_firmware_layout(struct pvr_device* pvr_dev)
{
	struct pvr_fw_device* fw_dev = &pvr_dev->fw_dev;
	struct pvr_fw_mips_data* mips = fw_dev->processor_data.mips_data;
	dma_addr_t code = 0, data = 0;
	pvr_fw_object_get_dma_addr(fw_dev->mem.code_obj, 0, &code);
	pvr_fw_object_get_dma_addr(fw_dev->mem.data_obj, 0, &data);

	TRACE("firmware: heap %u KiB at GPU %#llx (MIPS %#x), config at +%#x;"
		" code %u KiB at %#llx, data %u KiB at %#llx\n",
		fw_dev->fw_heap_info.raw_size / 1024, fw_dev->fw_heap_info.gpu_addr,
		pvr_dev->fw_dev.defs->get_fw_addr_with_offset(fw_dev->mem.code_obj, 0),
		fw_dev->fw_heap_info.config_offset,
		fw_dev->mem.code_alloc_size / 1024, code,
		fw_dev->mem.data_alloc_size / 1024, data);
	if (mips != NULL) {
		TRACE("firmware: MIPS boot code %#llx, boot data %#llx, exceptions"
			" %#llx, page table %#llx %#llx %#llx %#llx\n",
			mips->boot_code_dma_addr, mips->boot_data_dma_addr,
			mips->exception_code_dma_addr, mips->pt_dma_addr[0],
			mips->pt_dma_addr[1], mips->pt_dma_addr[2], mips->pt_dma_addr[3]);
	}
}


status_t
pvr_haiku_firmware_boot(struct pvr_device* pvr_dev,
	const pvr_haiku_firmware_options* options)
{
	struct pvr_haiku_device* device = to_haiku_device(pvr_dev);
	if (!device->firmware_loaded)
		return B_NO_INIT;
	device->trace_lines = options->trace_lines;
	device->verbose = options->trace_mask != 0;

	// the firmware's log groups, before pvr_fw_trace_init() reads them
	if (options->trace_mask != 0) {
		char mask[16];
		snprintf(mask, sizeof(mask), "%#x", options->trace_mask);
		int error = lx_param_init_fw_trace_mask.ops->set(mask,
			&lx_param_init_fw_trace_mask);
		TRACE("firmware: trace groups %s%s\n", mask,
			error != 0 ? " refused" : "");
	}

	if (options->vendor_secure_config)
		vendor_secure_config(pvr_dev);

	TRACE("firmware: starting, %u MHz core clock%s\n",
		(u32)(clk_get_rate(pvr_dev->core_clk) / 1000000),
		options->trace_registers ? ", register accesses follow" : "");
	device->reset_released = false;
	mmio_trace_start(device, options->trace_registers);
	bigtime_t start = system_time();
	int error = pvr_fw_init(pvr_dev);
	bigtime_t end = system_time();
	mmio_trace_stop();

	if (error != 0) {
		device->firmware_status = lx_status(error);
		TRACE("firmware: start failed: %d (%s) after %" B_PRIdBIGTIME
			" us\n", error, strerror(device->firmware_status), end - start);
		lx_debugfs_remove_all();
		return device->firmware_status;
	}

	device->boot_time = device->reset_released
		? end - device->reset_release_time : 0;
	TRACE("firmware: running: firmware_started %" B_PRIdBIGTIME " us after"
		" the MIPS left reset (%" B_PRIdBIGTIME " us with the set-up)\n",
		device->boot_time, end - start);
	log_firmware_layout(pvr_dev);
	pvr_fw_trace_debugfs_init(pvr_dev, NULL);
	device->firmware_status = B_OK;
	return B_OK;
}


/* #pragma mark - firmware checks */


/*!	Waits up to a second for the firmware's count of executed KCCB
	commands to pass \a count: commands such as HEALTH_CHECK do not answer
	in their return slot (the firmware writes it only for commands the host
	waits on; Linux's watchdog sends HEALTH_CHECK without a slot and watches
	this count, pvr_watchdog_kccb_stalled()).
*/
static int
wait_for_executed(struct pvr_device* pvr_dev, u32 count)
{
	struct rogue_fwif_osdata* osdata = pvr_dev->fw_dev.fwif_osdata;
	bigtime_t deadline = system_time() + 1000000;
	while ((s32)(READ_ONCE(osdata->kccb_cmds_executed) - count) < 0) {
		if (system_time() > deadline)
			return -ETIMEDOUT;
		snooze(20);
	}
	return 0;
}


/*!	Sends one KCCB command and waits up to a second, for its return slot
	(\a answers) or for the firmware's executed count, and logs the slot,
	the return value, the time, the executed count and the interrupts.
*/
int
pvr_haiku_kccb_execute(struct pvr_device* pvr_dev,
	struct rogue_fwif_kccb_cmd* command, const char* what, bool answers,
	u32* _return)
{
	struct pvr_haiku_device* device = to_haiku_device(pvr_dev);
	struct rogue_fwif_osdata* osdata = pvr_dev->fw_dev.fwif_osdata;
	u32 executedBefore = READ_ONCE(osdata->kccb_cmds_executed);
	int irqsBefore = atomic_read(&device->irq_count);
	u32 slot = 0;
	u32 result = 0;

	bigtime_t start = system_time();
	int error = pvr_kccb_send_cmd(pvr_dev, command, &slot);
	if (error != 0) {
		TRACE("%s: not sent: %d\n", what, error);
		return error;
	}
	if (answers)
		error = pvr_kccb_wait_for_completion(pvr_dev, slot, HZ, &result);
	else
		error = wait_for_executed(pvr_dev, executedBefore + 1);
	bigtime_t elapsed = system_time() - start;
	if (error != 0 || !answers)
		result = READ_ONCE(pvr_dev->kccb.rtn[slot]);

	device->last_kccb_return = result;
	int irqs = atomic_read(&device->irq_count) - irqsBefore;
	const char* outcome;
	if (answers) {
		outcome = (result & ROGUE_FWIF_KCCB_RTN_SLOT_CMD_EXECUTED) != 0
			? "executed" : "no answer";
	} else
		outcome = error == 0 ? "executed, no answer expected" : "not executed";
	TRACE("%s: KCCB slot %u, return %#x (%s), %" B_PRIdBIGTIME " us,"
		" kccb_cmds_executed %u -> %u, %d interrupt%s%s\n", what, slot, result,
		outcome, elapsed, executedBefore,
		READ_ONCE(osdata->kccb_cmds_executed), irqs, irqs == 1 ? "" : "s",
		answers && error == 0 && elapsed >= 900000
			? ", seen only at the timeout: no interrupt woke the wait" : "");
	if (_return != NULL)
		*_return = result;
	return error;
}


status_t
pvr_haiku_health_check(struct pvr_device* pvr_dev)
{
	struct pvr_haiku_device* device = to_haiku_device(pvr_dev);
	if (!READ_ONCE(pvr_dev->fw_dev.initialised))
		return B_NO_INIT;

	struct rogue_fwif_kccb_cmd command = {};
	command.cmd_type = ROGUE_FWIF_KCCB_CMD_HEALTH_CHECK;
	command.kccb_flags = 0;

	char what[32];
	snprintf(what, sizeof(what), "HEALTH_CHECK %u",
		device->health_checks + device->health_check_failures + 1);
	int error = pvr_haiku_kccb_execute(pvr_dev, &command, what, false, NULL);
	if (error == 0) {
		device->health_checks++;
		return B_OK;
	}
	device->health_check_failures++;
	return lx_status(error);
}


/*!	pvr_mmu.c's full flush (pvr_mmu_flush_request_all() and
	pvr_mmu_flush_exec()), sent here to have it logged and timed, and
	without the reset and retry that follow a failure there.
*/
static int
mmu_flush(struct pvr_device* pvr_dev)
{
	struct rogue_fwif_kccb_cmd command = {};
	struct rogue_fwif_mmucachedata* data = &command.cmd_data.mmu_cache_data;

	command.cmd_type = ROGUE_FWIF_KCCB_CMD_MMUCACHE;
	data->cache_flags = ROGUE_FWIF_MMUCACHEDATA_FLAGS_PT
		| ROGUE_FWIF_MMUCACHEDATA_FLAGS_INTERRUPT
		| ROGUE_FWIF_MMUCACHEDATA_FLAGS_TLB
		| ROGUE_FWIF_MMUCACHEDATA_FLAGS_PD
		| ROGUE_FWIF_MMUCACHEDATA_FLAGS_PC;
	pvr_fw_object_get_fw_addr(pvr_dev->fw_dev.mem.mmucache_sync_obj,
		&data->mmu_cache_sync_fw_addr);
	data->mmu_cache_sync_update_value = 0;
	return pvr_haiku_kccb_execute(pvr_dev, &command, "MMU cache flush", true,
		NULL);
}


/*!	The firmware stage's proof of life: HEALTH_CHECKs, an MMU cache flush
	whose sync object the firmware has to write, and interrupts.
*/
status_t
pvr_haiku_firmware_verify(struct pvr_device* pvr_dev,
	const pvr_haiku_firmware_options* options)
{
	struct pvr_haiku_device* device = to_haiku_device(pvr_dev);
	struct pvr_fw_device* fw_dev = &pvr_dev->fw_dev;
	if (!READ_ONCE(fw_dev->initialised))
		return B_NO_INIT;

	status_t status = B_OK;
	for (int i = 0; i < HEALTH_CHECKS; i++) {
		status_t checkStatus = pvr_haiku_health_check(pvr_dev);
		if (checkStatus != B_OK && status == B_OK)
			status = checkStatus;
	}

	// The firmware writes the update value (0, as pvr_mmu.c sends it) to
	// the sync object once the flush is done: starting it at something else
	// shows whether the firmware writes host memory. Linux never looks, so
	// this is only reported.
	u32* sync = (u32*)pvr_fw_object_vmap(fw_dev->mem.mmucache_sync_obj);
	WRITE_ONCE(*sync, 0xffffffff);
	int error = mmu_flush(pvr_dev);
	u32 syncValue = READ_ONCE(*sync);
	pvr_fw_object_vunmap(fw_dev->mem.mmucache_sync_obj);
	TRACE("MMU cache flush: %s, sync object 0xffffffff -> %#x (%s)\n",
		error == 0 ? "done" : "failed", syncValue,
		syncValue == 0 ? "written by the firmware" : "not written");
	if (error != 0 && status == B_OK)
		status = lx_status(error);

	int irqs = atomic_read(&device->irq_count);
	int spurious = atomic_read(&device->irq_spurious);
	TRACE("interrupts: %d from the MIPS wrapper, %d without a wrapper event,"
		" OSDATA interrupt_count %u/%u\n", irqs, spurious,
		READ_ONCE(fw_dev->fwif_osdata->interrupt_count[0]),
		READ_ONCE(fw_dev->fwif_osdata->interrupt_count[1]));
	if (irqs == 0) {
		TRACE("interrupts: none arrived (GIC SPI 63, INTID 95)\n");
		if (status == B_OK)
			status = B_ERROR;
	}

	if (status != B_OK)
		pvr_haiku_dump(pvr_dev, "a firmware check failed",
			options->trace_lines);
	else if (options->trace_mask != 0)
		pvr_haiku_dump(pvr_dev, "firmware running", options->trace_lines);

	device->firmware_status = status;
	return status;
}


/* #pragma mark - state and diagnostics */


bool
pvr_haiku_device_lost(struct pvr_device* pvr_dev)
{
	return pvr_dev->lost;
}


void
pvr_haiku_firmware_state_get(struct pvr_device* pvr_dev,
	pvr_haiku_firmware_state* state)
{
	struct pvr_haiku_device* device = to_haiku_device(pvr_dev);
	struct pvr_fw_device* fw_dev = &pvr_dev->fw_dev;
	memset(state, 0, sizeof(*state));

	state->status = device->firmware_status;
	state->loaded = device->firmware_loaded;
	if (device->firmware_loaded && fw_dev->header != NULL) {
		state->version_major = fw_dev->header->fw_version_major;
		state->version_minor = fw_dev->header->fw_version_minor;
		state->version_build = fw_dev->header->fw_version_build;
	}
	state->running = READ_ONCE(fw_dev->initialised);
	state->boot_time = device->boot_time;
	state->health_checks = device->health_checks;
	state->health_check_failures = device->health_check_failures;
	state->last_kccb_return = device->last_kccb_return;
	if (state->running) {
		state->kccb_cmds_executed
			= READ_ONCE(fw_dev->fwif_osdata->kccb_cmds_executed);
		state->fw_faults = READ_ONCE(fw_dev->fwif_sysdata->fw_faults);
	}
	state->irq_count = atomic_read(&device->irq_count);
	state->irq_spurious = atomic_read(&device->irq_spurious);
	state->resets = device->resets;
	state->lost = pvr_dev->lost;
	state->mips_exception_status
		= pvr_cr_read32(pvr_dev, ROGUE_CR_MIPS_EXCEPTION_STATUS);
}


static void
dump_registers(struct pvr_device* pvr_dev)
{
	TRACE("dump: SOFT_RESET %#llx, SOFT_RESET2 %#llx, SYS_BUS_SECURE %#x,"
		" CLK_CTRL %#llx, CLK_STATUS %#llx, TIMER %#llx\n",
		pvr_cr_read64(pvr_dev, ROGUE_CR_SOFT_RESET),
		pvr_cr_read64(pvr_dev, ROGUE_CR_SOFT_RESET2),
		pvr_cr_read32(pvr_dev, ROGUE_CR_SYS_BUS_SECURE),
		pvr_cr_read64(pvr_dev, ROGUE_CR_CLK_CTRL),
		pvr_cr_read64(pvr_dev, ROGUE_CR_CLK_STATUS),
		pvr_cr_read64(pvr_dev, ROGUE_CR_TIMER));
	TRACE("dump: MIPS wrapper config %#llx, status %#x, exception status"
		" %#x, IRQ enable %#x status %#x, debug config %#x\n",
		pvr_cr_read64(pvr_dev, ROGUE_CR_MIPS_WRAPPER_CONFIG),
		pvr_cr_read32(pvr_dev, ROGUE_CR_MIPS_WRAPPER_STATUS),
		pvr_cr_read32(pvr_dev, ROGUE_CR_MIPS_EXCEPTION_STATUS),
		pvr_cr_read32(pvr_dev, ROGUE_CR_MIPS_WRAPPER_IRQ_ENABLE),
		pvr_cr_read32(pvr_dev, ROGUE_CR_MIPS_WRAPPER_IRQ_STATUS),
		pvr_cr_read32(pvr_dev, ROGUE_CR_MIPS_DEBUG_CONFIG));
	TRACE("dump: remap 1 %#llx/%#llx, 2 %#llx/%#llx, 3 %#llx/%#llx, 5"
		" %#llx/%#llx, unmapped access %#llx\n",
		pvr_cr_read64(pvr_dev, ROGUE_CR_MIPS_ADDR_REMAP1_CONFIG1),
		pvr_cr_read64(pvr_dev, ROGUE_CR_MIPS_ADDR_REMAP1_CONFIG2),
		pvr_cr_read64(pvr_dev, ROGUE_CR_MIPS_ADDR_REMAP2_CONFIG1),
		pvr_cr_read64(pvr_dev, ROGUE_CR_MIPS_ADDR_REMAP2_CONFIG2),
		pvr_cr_read64(pvr_dev, ROGUE_CR_MIPS_ADDR_REMAP3_CONFIG1),
		pvr_cr_read64(pvr_dev, ROGUE_CR_MIPS_ADDR_REMAP3_CONFIG2),
		pvr_cr_read64(pvr_dev, ROGUE_CR_MIPS_ADDR_REMAP5_CONFIG1),
		pvr_cr_read64(pvr_dev, ROGUE_CR_MIPS_ADDR_REMAP5_CONFIG2),
		pvr_cr_read64(pvr_dev, ROGUE_CR_MIPS_ADDR_REMAP_UNMAPPED_STATUS));
	TRACE("dump: events %#x, OS0 events %#x, SIDEKICK_IDLE %#x, MARS_IDLE"
		" %#x, SLC_IDLE %#x, SLC_STATUS1 %#llx, SLC_CTRL_MISC %#llx\n",
		pvr_cr_read32(pvr_dev, ROGUE_CR_EVENT_STATUS),
		pvr_cr_read32(pvr_dev, ROGUE_CR_IRQ_OS0_EVENT_STATUS),
		pvr_cr_read32(pvr_dev, ROGUE_CR_SIDEKICK_IDLE),
		pvr_cr_read32(pvr_dev, ROGUE_CR_MARS_IDLE),
		pvr_cr_read32(pvr_dev, ROGUE_CR_SLC_IDLE),
		pvr_cr_read64(pvr_dev, ROGUE_CR_SLC_STATUS1),
		pvr_cr_read64(pvr_dev, ROGUE_CR_SLC_CTRL_MISC));
	TRACE("dump: AXI ACE-lite %#llx, MTS garten wrapper %#llx, BIF fault"
		" %#x/%#llx, BIF MMU %#x, BIFPM MMU %#x, scratch %#x %#x %#x %#x\n",
		pvr_cr_read64(pvr_dev, ROGUE_CR_AXI_ACE_LITE_CONFIGURATION),
		pvr_cr_read64(pvr_dev, ROGUE_CR_MTS_GARTEN_WRAPPER_CONFIG),
		pvr_cr_read32(pvr_dev, ROGUE_CR_BIF_FAULT_BANK0_MMU_STATUS),
		pvr_cr_read64(pvr_dev, ROGUE_CR_BIF_FAULT_BANK0_REQ_STATUS),
		pvr_cr_read32(pvr_dev, ROGUE_CR_BIF_STATUS_MMU),
		pvr_cr_read32(pvr_dev, ROGUE_CR_BIFPM_STATUS_MMU),
		pvr_cr_read32(pvr_dev, ROGUE_CR_OS0_SCRATCH0),
		pvr_cr_read32(pvr_dev, ROGUE_CR_OS0_SCRATCH1),
		pvr_cr_read32(pvr_dev, ROGUE_CR_OS0_SCRATCH2),
		pvr_cr_read32(pvr_dev, ROGUE_CR_OS0_SCRATCH3));
}


static void
dump_text(const char* what, const struct rogue_fwif_file_info_buf* info)
{
	if (info->path[0] == '\0' && info->info[0] == '\0')
		return;
	TRACE("dump: %s: %.*s at %.*s:%u\n", what, (int)sizeof(info->info),
		info->info, (int)sizeof(info->path), info->path, info->line_num);
}


/*!	The firmware's shared structures. Only while they exist: after a
	successful start, or from pvr_haiku_fw_boot_failed().
*/
static void
dump_firmware_memory(struct pvr_device* pvr_dev)
{
	struct pvr_fw_device* fw_dev = &pvr_dev->fw_dev;
	struct rogue_fwif_sysinit* sysinit = fw_dev->fwif_sysinit;
	struct rogue_fwif_sysdata* sysdata = fw_dev->fwif_sysdata;
	struct rogue_fwif_osdata* osdata = fw_dev->fwif_osdata;

	TRACE("dump: SYSINIT firmware_started %u (at %#x), marker %#x, core"
		" clock %u; SYSDATA power state %u, HWR state %#x, config %#x,"
		" faults %u\n", (u32)READ_ONCE(sysinit->firmware_started),
		READ_ONCE(sysinit->firmware_started_timestamp),
		READ_ONCE(sysinit->marker_val),
		READ_ONCE(sysinit->initial_core_clock_speed),
		(u32)READ_ONCE(sysdata->pow_state),
		READ_ONCE(sysdata->hwr_state_flags), READ_ONCE(sysdata->config_flags),
		READ_ONCE(sysdata->fw_faults));
	u32 faults = min_t(u32, READ_ONCE(sysdata->fw_faults),
		ROGUE_FWIF_FWFAULTINFO_MAX);
	for (u32 i = 0; i < faults; i++) {
		TRACE("dump: firmware fault %u: data %#x, timer %#llx\n", i,
			sysdata->fault_info[i].data,
			(u64)sysdata->fault_info[i].cr_timer);
		dump_text("  where", &sysdata->fault_info[i].fault_buf);
	}

	TRACE("dump: OSDATA kccb_cmds_executed %u, interrupt_count %u/%u,"
		" config %#x; KCCB write %u read %u, FWCCB write %u read %u\n",
		READ_ONCE(osdata->kccb_cmds_executed),
		READ_ONCE(osdata->interrupt_count[0]),
		READ_ONCE(osdata->interrupt_count[1]),
		READ_ONCE(osdata->fw_os_config_flags),
		READ_ONCE(pvr_dev->kccb.ccb.ctrl->write_offset),
		READ_ONCE(pvr_dev->kccb.ccb.ctrl->read_offset),
		READ_ONCE(pvr_dev->fwccb.ctrl->write_offset),
		READ_ONCE(pvr_dev->fwccb.ctrl->read_offset));

	struct rogue_fwif_tracebuf* tracebuf = fw_dev->fw_trace.tracebuf_ctrl;
	if (tracebuf != NULL) {
		for (u32 thread = 0; thread < MAX_THREAD_NUM; thread++) {
			TRACE("dump: trace %u: log type %#x, pointer %u\n", thread,
				READ_ONCE(tracebuf->log_type),
				READ_ONCE(tracebuf->tracebuf[thread].trace_pointer));
			dump_text("  firmware assertion",
				&tracebuf->tracebuf[thread].assert_buf);
		}
	}

	// the boot loader's page: its configuration, NMI state and boot stage
	const struct pvr_fw_layout_entry* bootData
		= pvr_fw_find_layout_entry(pvr_dev, MIPS_BOOT_DATA);
	u8* data = (u8*)pvr_fw_object_vmap(fw_dev->mem.data_obj);
	if (bootData != NULL && !IS_ERR(data)) {
		const u8* page = data + bootData->alloc_offset;
		const struct rogue_mipsfw_boot_data* config
			= (const struct rogue_mipsfw_boot_data*)(page
				+ ROGUE_MIPSFW_BOOTLDR_CONF_OFFSET);
		const u32* nmi = (const u32*)(page + ROGUE_MIPSFW_NMI_SHARED_DATA_BASE);
		TRACE("dump: MIPS boot data: stack %#llx, registers %#llx, page"
			" table %#llx %#llx %#llx %#llx (%u pages of 2^%u); NMI %#x %#x;"
			" boot stage %#x\n", config->stack_phys_addr, config->reg_base,
			config->pt_phys_addr[0], config->pt_phys_addr[1],
			config->pt_phys_addr[2], config->pt_phys_addr[3],
			config->pt_num_pages, config->pt_log2_page_size,
			READ_ONCE(nmi[ROGUE_MIPSFW_NMI_SYNC_FLAG_OFFSET]),
			READ_ONCE(nmi[ROGUE_MIPSFW_NMI_STATE_OFFSET]),
			READ_ONCE(*(const u32*)(page + ROGUE_MIPSFW_BOOT_STAGE_OFFSET)));
	}

	struct pvr_fw_mips_data* mips = fw_dev->processor_data.mips_data;
	if (mips != NULL && mips->pt != NULL) {
		u32 mask = fw_dev->fw_heap_info.offset_mask;
		u32 sysinitPage = (pvr_fw_obj_get_gpu_addr(fw_dev->mem.sysinit_obj)
			& mask) >> ROGUE_MIPSFW_LOG2_PAGE_SIZE_4K;
		u32 dataPage = (pvr_fw_obj_get_gpu_addr(fw_dev->mem.data_obj)
			& mask) >> ROGUE_MIPSFW_LOG2_PAGE_SIZE_4K;
		TRACE("dump: MIPS page table: code %#x, data [%#x] %#x, SYSINIT"
			" [%#x] %#x\n", READ_ONCE(mips->pt[0]), dataPage,
			READ_ONCE(mips->pt[dataPage]), sysinitPage,
			READ_ONCE(mips->pt[sysinitPage]));
	}
}


static void
dump_trace_line(void* cookie, const char* text)
{
	(void)cookie;
	TRACE("fw: %s\n", text);
}


static u32
xa_count(struct xarray* xa)
{
	unsigned long index;
	void* entry;
	u32 count = 0;
	xa_for_each(xa, index, entry)
		count++;
	return count;
}


/*!	What exists: buffers, page-table pages, firmware objects, contexts,
	jobs on the queues (handed to the firmware, and still queued).
*/
static void
dump_objects(struct pvr_device* pvr_dev)
{
	uint32 buffers, pages, vmaps, imports;
	uint64 bufferBytes;
	lx_memory_stats(&buffers, &bufferBytes, &pages, &vmaps, &imports);

	u32 fwObjects = 0;
	struct list_head* position;
	if (READ_ONCE(pvr_dev->fw_dev.initialised)) {
		mutex_lock(&pvr_dev->fw_dev.fw_objs.lock);
		list_for_each(position, &pvr_dev->fw_dev.fw_objs.list)
			fwObjects++;
		mutex_unlock(&pvr_dev->fw_dev.fw_objs.lock);
	}

	u32 queues = 0, pending = 0, queued = 0;
	struct pvr_queue* queue;
	struct list_head* lists[] = {
		&pvr_dev->queues.active, &pvr_dev->queues.idle
	};
	mutex_lock(&pvr_dev->queues.lock);
	for (int i = 0; i < 2; i++) {
		list_for_each_entry(queue, lists[i], node) {
			queues++;
			spin_lock(&queue->scheduler.job_list_lock);
			list_for_each(position, &queue->scheduler.pending_list)
				pending++;
			spin_unlock(&queue->scheduler.job_list_lock);
			struct drm_sched_entity* entity = queue->scheduler.entity;
			if (entity != NULL) {
				spin_lock(&entity->lock);
				list_for_each(position, &entity->job_queue)
					queued++;
				spin_unlock(&entity->lock);
			}
		}
	}
	mutex_unlock(&pvr_dev->queues.lock);

	TRACE("dump: objects: %u buffers (%llu KiB), %u imports, %u pages, %u"
		" vmaps, %u firmware objects, %u contexts, %u jobs, %u free lists;"
		" %u queues with %u jobs on the GPU and %u queued\n", buffers,
		(unsigned long long)(bufferBytes / 1024), imports, pages, vmaps,
		fwObjects,
		xa_count(&pvr_dev->ctx_ids), xa_count(&pvr_dev->job_ids),
		xa_count(&pvr_dev->free_list_ids), queues, pending, queued);
	uint64 made, madeBytes;
	lx_memory_totals(&made, &madeBytes);
	TRACE("dump: since boot: %llu buffers made (%llu KiB)\n",
		(unsigned long long)made, (unsigned long long)(madeBytes / 1024));
	pvr_haiku_ioctl_stats_dump();
	pvr_haiku_job_times_dump();
}


void
pvr_haiku_dump(struct pvr_device* pvr_dev, const char* why,
	uint32 traceLines)
{
	struct pvr_fw_device* fw_dev = &pvr_dev->fw_dev;
	TRACE("dump (%s):\n", why);
	dump_objects(pvr_dev);
	dump_registers(pvr_dev);

	// the firmware structures exist while the firmware runs, and in the
	// failure hook during the start
	if (!READ_ONCE(fw_dev->initialised)
		&& !to_haiku_device(pvr_dev)->failed_start) {
		return;
	}
	dump_firmware_memory(pvr_dev);

	if (traceLines == 0 || fw_dev->fw_trace.tracebuf_ctrl == NULL)
		return;
	pvr_fw_trace_debugfs_init(pvr_dev, NULL);
	int lines = lx_debugfs_dump("trace_0", dump_trace_line, NULL, traceLines);
	TRACE("dump: %d firmware trace line%s (log groups %#x)\n", lines,
		lines == 1 ? "" : "s", fw_dev->fw_trace.group_mask);
}


/* #pragma mark - shutdown */


void
pvr_haiku_firmware_shutdown(struct pvr_device* pvr_dev)
{
	struct pvr_haiku_device* device = to_haiku_device(pvr_dev);
	lx_debugfs_remove_all();
	if (!READ_ONCE(pvr_dev->fw_dev.initialised))
		return;

	int error = pvr_haiku_power_off(pvr_dev);
	if (error == 0)
		error = pvr_fw_stop(pvr_dev);
	TRACE("firmware: stopped (%d)\n", error);
	if (error != 0)
		pvr_haiku_force_reset(pvr_dev, "at shutdown");

	pvr_fw_fini(pvr_dev);
	device->firmware_status = B_NO_INIT;
}
