/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef NVME_HAIKU_DMA_H
#define NVME_HAIKU_DMA_H

#if defined(__aarch64__)
// Until PCI supplies per-device DMA mappings, keep ARM64 device-owned RAM
// noncached and copy payloads through private, preallocated buffers.
// The "direct_dma" setting of the nvme_disk driver settings instead hands
// the controller the caller's pages, with cache maintenance around each
// transfer (airos-works/haiku#181); the 128 KiB and 8-command limits are
// then those of x86.
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

// Direct DMA (the "direct_dma" setting): whether it is on, and the alignment
// the caller's memory needs then (the CPU's cache writeback granule: a line
// the controller writes must not hold anything else the CPU may write).
int nvme_dma_direct(void);
size_t nvme_dma_alignment(void);
// Memory at or above this cannot be handed to the controller ("dma_limit";
// ~0 when there is no such limit): it is bounced.
uint64_t nvme_dma_limit(void);
// Before a transfer: the payload's lines cleaned and invalidated. A payload
// the controller writes must be aligned to nvme_dma_alignment(): returns an
// error if it is not.
int nvme_dma_map_payload(const struct nvme_request* request);
// After the controller wrote the payload: its lines invalidated again, for
// what the CPU may have speculatively loaded during the transfer.
void nvme_dma_unmap_payload(const struct nvme_request* request);
#endif

#endif
