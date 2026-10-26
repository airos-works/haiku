/*
 * Copyright 2019-2022, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Augustin Cavalier <waddlesplash>
 */

#include <debug.h>
#include <kernel/vm/vm.h>
#include <PCI.h>

#if defined(__aarch64__)
#include <arch/arm64/arch_kernel.h>
#include <arch/arm64/cache_line_size.h>
#include <driver_settings.h>
#endif

extern "C" {
#include "nvme.h"
#include "nvme_log.h"
#include "nvme_mem.h"
#include "nvme_pci.h"
#include "nvme_internal.h"
}


static pci_module_info* sPCIModule = NULL;

#if defined(NVME_HAIKU_NONCOHERENT_DMA)
static int32 sForceHighDMA = 0;
#endif


// #pragma mark - memory


int
nvme_mem_init()
{
#if defined(NVME_HAIKU_NONCOHERENT_DMA)
	// Explicit lab opt-in: exercise full-width DMA addresses without changing
	// the normal allocation policy or silently falling back to low memory.
	void* settings = load_driver_settings("nvme_disk");
	bool forceHighDMA = settings != NULL && get_driver_boolean_parameter(
		settings, "force_high_dma", false, false);
	if (settings != NULL)
		unload_driver_settings(settings);
	atomic_set(&sForceHighDMA, forceHighDMA ? 1 : 0);
	if (forceHighDMA)
		nvme_notice("ARM64 lab DMA floor: 0x100000000 (4 GiB)\n");
#endif
	return 0;
}


void
nvme_mem_cleanup()
{
	/* nothing to do */
}


void*
nvme_mem_alloc_node(size_t size, size_t align, unsigned int node_id,
	phys_addr_t* paddr)
{
	if (size == 0 || size > SIZE_MAX - (B_PAGE_SIZE - 1))
		return NULL;
	size = ROUNDUP(size, B_PAGE_SIZE);

	virtual_address_restrictions virtualRestrictions = {};

	physical_address_restrictions physicalRestrictions = {};
	physicalRestrictions.alignment = align;
#if defined(NVME_HAIKU_NONCOHERENT_DMA)
	if (atomic_get(&sForceHighDMA) != 0)
		physicalRestrictions.low_address = UINT64_C(0x100000000);
#endif

	void* address;
	area_id area = create_area_etc(B_SYSTEM_TEAM, "nvme physical buffer",
		size, B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		0, 0, &virtualRestrictions, &physicalRestrictions, &address);
	if (area < 0) {
		if (physicalRestrictions.low_address != 0)
			nvme_crit("Cannot allocate %zu bytes above lab DMA floor\n", size);
		return NULL;
	}

	phys_addr_t physicalAddress = nvme_mem_vtophys(address);
	if (physicalAddress < physicalRestrictions.low_address) {
		nvme_crit("Allocation violates physical address restriction\n");
		delete_area(area);
		return NULL;
	}
#if defined(NVME_HAIKU_NONCOHERENT_DMA)
	// create_area_etc() zeroes through a cached mapping. Remove those lines
	// before changing the allocation's private mapping to Normal Non-cacheable.
	uint64 ctr;
	asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
	const size_t lineSize = arm64_data_cache_line_size(ctr);
	for (addr_t p = (addr_t)address; p < (addr_t)address + size; p += lineSize)
		asm volatile("dc civac, %0" :: "r"(p) : "memory");
	memory_full_barrier();
	// B_UNCACHED_MEMORY denotes Device memory on ARM64, which does not
	// permit the ordinary unaligned copies needed for RAM payloads.
	if (vm_set_area_memory_type(area, physicalAddress,
			B_WRITE_COMBINING_MEMORY) != B_OK) {
		delete_area(area);
		return NULL;
	}
	memory_full_barrier();
#endif
	if (paddr != NULL)
		*paddr = physicalAddress;
	return address;
}


void*
nvme_malloc_node(size_t size, size_t align, unsigned int node_id)
{
	return nvme_mem_alloc_node(size, align, node_id, NULL);
}


void
nvme_free(void* addr)
{
	delete_area(area_for(addr));
}


phys_addr_t
nvme_mem_vtophys(void* vaddr)
{
	physical_entry entry;
	status_t status = get_memory_map((void*)vaddr, 1, &entry, 1);
	if (status != B_OK) {
		panic("nvme: get_memory_map failed for %p: %s\n",
			(void*)vaddr, strerror(status));
		return NVME_VTOPHYS_ERROR;
	}

	return entry.address;
}


#if defined(NVME_HAIKU_NONCOHERENT_DMA)
void
nvme_dma_clean_alias(uint64_t physical, size_t size)
{
	uint64 ctr;
	asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
	const size_t lineSize = arm64_data_cache_line_size(ctr);
	addr_t start = (KERNEL_PMAP_BASE + physical) & ~(addr_t)(lineSize - 1);
	addr_t end = KERNEL_PMAP_BASE + physical + size;
	for (addr_t p = start; p < end; p += lineSize)
		asm volatile("dc civac, %0" :: "r"(p) : "memory");
	memory_full_barrier();
}
#endif


#if defined(NVME_HAIKU_NONCOHERENT_DMA)
int
nvme_dma_copy_payload(const nvme_request* request, void* buffer,
	enum nvme_dma_copy operation)
{
	if (request->payload_size == 0)
		return B_OK;
	if (request->payload_size > NVME_DMA_MAX_TRANSFER
		|| request->payload.md != NULL || buffer == NULL)
		return B_BAD_VALUE;

	if (request->payload.type == NVME_PAYLOAD_TYPE_CONTIG) {
		addr_t address = (addr_t)request->payload.u.contig;
		if (address == 0 || address > UINTPTR_MAX - request->payload_offset)
			return B_BAD_ADDRESS;
		address += request->payload_offset;
		if (address > UINTPTR_MAX - request->payload_size)
			return B_BAD_ADDRESS;
		if (operation == NVME_DMA_FROM_HOST)
			memcpy(buffer, (const void*)address, request->payload_size);
		else if (operation == NVME_DMA_TO_HOST)
			memcpy((void*)address, buffer, request->payload_size);
		return B_OK;
	}

	if (request->payload.type != NVME_PAYLOAD_TYPE_SGL
		|| request->payload.u.sgl.reset_sgl_fn == NULL
		|| request->payload.u.sgl.next_sge_fn == NULL)
		return B_BAD_VALUE;

	request->payload.u.sgl.reset_sgl_fn(request->payload.u.sgl.cb_arg,
		request->payload_offset);
	size_t copied = 0;
	while (copied < request->payload_size) {
		uint64 physicalAddress;
		uint32 length;
		if (request->payload.u.sgl.next_sge_fn(request->payload.u.sgl.cb_arg,
				&physicalAddress, &length) != 0 || length == 0)
			return B_BAD_VALUE;
		length = min_c(length, request->payload_size - copied);
		if (physicalAddress > UINT64_MAX - (length - 1))
			return B_BAD_ADDRESS;

		status_t status = B_OK;
		if (operation == NVME_DMA_FROM_HOST) {
			status = vm_memcpy_from_physical((uint8*)buffer + copied,
				physicalAddress, length, false);
		} else if (operation == NVME_DMA_TO_HOST) {
			status = vm_memcpy_to_physical(physicalAddress,
				(const uint8*)buffer + copied, length, false);
		}
		if (status != B_OK)
			return status;
		copied += length;
	}
	return B_OK;
}
#endif


// #pragma mark - PCI


int
nvme_pci_init()
{
	status_t status = get_module(B_PCI_MODULE_NAME,
		(module_info**)&sPCIModule);
	return status;
}


int
nvme_pcicfg_read32(struct pci_device* dev, uint32_t* value, uint32_t offset)
{
	*value = sPCIModule->read_pci_config(dev->bus, dev->dev, dev->func, offset,
		sizeof(*value));
	return 0;
}


int
nvme_pcicfg_write32(struct pci_device* dev, uint32_t value, uint32_t offset)
{
	sPCIModule->write_pci_config(dev->bus, dev->dev, dev->func, offset,
		sizeof(value), value);
	return 0;
}


void
nvme_pcicfg_get_bar_addr_len(void* devhandle, unsigned int bar,
	uint64_t* _addr, uint64_t* _size)
{
	struct pci_device* dev = (struct pci_device*)devhandle;
	pci_info* info = (pci_info*)dev->pci_info;

	uint64 addr = info->u.h0.base_registers[bar];
	uint64 size = info->u.h0.base_register_sizes[bar];
	if ((info->u.h0.base_register_flags[bar] & PCI_address_type) == PCI_address_type_64) {
		addr |= (uint64)info->u.h0.base_registers[bar + 1] << 32;
		size |= (uint64)info->u.h0.base_register_sizes[bar + 1] << 32;
	}

	*_addr = addr;
	*_size = size;
}


int
nvme_pcicfg_map_bar(void* devhandle, unsigned int bar, bool read_only,
	void** mapped_addr)
{
	uint64 addr, size;
	nvme_pcicfg_get_bar_addr_len(devhandle, bar, &addr, &size);

	area_id area = map_physical_memory("nvme mapped bar", (phys_addr_t)addr, (size_t)size,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | (read_only ? 0 : B_KERNEL_WRITE_AREA),
		mapped_addr);
	if (area < B_OK)
		return area;

	return 0;
}


int
nvme_pcicfg_map_bar_write_combine(void* devhandle, unsigned int bar,
	void** mapped_addr)
{
	status_t status = nvme_pcicfg_map_bar(devhandle, bar, false, mapped_addr);
	if (status != 0)
		return status;

	// Turn on write combining for the area
	status = vm_set_area_memory_type(area_for(*mapped_addr),
		nvme_mem_vtophys(*mapped_addr), B_WRITE_COMBINING_MEMORY);
	if (status != 0)
		nvme_pcicfg_unmap_bar(devhandle, bar, *mapped_addr);
	return status;
}


int
nvme_pcicfg_unmap_bar(void* devhandle, unsigned int bar, void* addr)
{
	return delete_area(area_for(addr));
}


// #pragma mark - logging


void
nvme_log(enum nvme_log_level level, const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	nvme_vlog(level, format, ap);
	va_end(ap);
}


void
nvme_vlog(enum nvme_log_level level, const char *format, va_list ap)
{
	dvprintf(format, ap);
}
