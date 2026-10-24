/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef NVME_HAIKU_DMA_H
#define NVME_HAIKU_DMA_H

#if defined(__aarch64__)
// Until PCI supplies per-device DMA mappings, keep ARM64 device-owned RAM
// noncached and copy payloads through private, preallocated buffers.
#define NVME_HAIKU_NONCOHERENT_DMA
#define NVME_DMA_MAX_TRANSFER (128U * 1024U)
#define NVME_DMA_IO_TRACKERS 8U

enum nvme_dma_copy {
	NVME_DMA_VALIDATE,
	NVME_DMA_FROM_HOST,
	NVME_DMA_TO_HOST
};

struct nvme_request;
int nvme_dma_copy_payload(const struct nvme_request* request, void* buffer,
	enum nvme_dma_copy operation);
// What the CPU wrote through the noncached mapping, for the controller to
// read: the cached linear map's lines of it cleaned and invalidated. A
// cache-coherent PCIe (the SC8280XP's) answers the controller's reads from
// a stale line the linear map left there by speculation, which then reads
// a zeroed submission entry.
void nvme_dma_clean_alias(uint64_t physical, size_t size);
#endif

#endif
