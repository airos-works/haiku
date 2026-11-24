/*
 * Copyright 2004-2007, Marcus Overhagen. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef __UTIL_H
#define __UTIL_H

#include <KernelExport.h>

#ifdef __cplusplus
extern "C" {
#endif

area_id alloc_mem(void **virt, phys_addr_t *phy, size_t size, uint32 protection,
			const char *name, bool supports64Bit);
area_id map_mem(void **virt, phys_addr_t phy, size_t size, uint32 protection,
			const char *name);

status_t sg_memcpy(const physical_entry *sgTable, int sgCount, const void *data, size_t dataSize);

void swap_words(void *data, size_t size);

/* ARM64 direct DMA (the "direct_dma" setting of the ahci driver settings,
 * off by default): the controller gets the request's own pages, with cache
 * maintenance around the transfer, instead of a copy through the port's
 * noncached 128 KiB buffer (airos-works/haiku#181). Then the request's
 * memory must start and end on the cache writeback granule
 * (ahci_dma_alignment()); elsewhere it is 2 bytes. */
bool ahci_direct_dma(void);
uint32 ahci_dma_alignment(void);
void ahci_dma_sync(const physical_entry *sgTable, int sgCount,
			size_t dataSize);

#ifdef __cplusplus
}
#endif

#endif
