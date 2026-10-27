/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef POWERVR_LX_HAIKU_H
#define POWERVR_LX_HAIKU_H


/*	What linux_compat.h (C) and linux_compat_haiku.cpp (C++) share: plain
	structures and the functions the C++ side implements on Haiku's kernel
	API. Errors are negative Linux codes; the LX_E* values below are the
	Linux ones, and linux_compat.h's E* codes are defined from them. */


#include <OS.h>
#include <StorageDefs.h>
#include <SupportDefs.h>


/* Linux error numbers (asm-generic/errno-base.h, errno.h). */
#define LX_EPERM		1
#define LX_ENOENT		2
#define LX_EINTR		4
#define LX_EIO			5
#define LX_E2BIG		7
#define LX_EAGAIN		11
#define LX_ENOMEM		12
#define LX_EACCES		13
#define LX_EFAULT		14
#define LX_EBUSY		16
#define LX_EEXIST		17
#define LX_ENODEV		19
#define LX_EINVAL		22
#define LX_ENOSPC		28
#define LX_ERANGE		34
#define LX_EDEADLK		35
#define LX_ENOSYS		38
#define LX_ETIME		62
#define LX_EOVERFLOW	75
#define LX_EOPNOTSUPP	95
#define LX_ETIMEDOUT	110
#define LX_ECANCELED	125
#define LX_ENOTSUPP		524


#ifdef __cplusplus
extern "C" {
#endif


struct device;


/*	A negative Linux error code as the Haiku status that means the same:
	what userland gets as errno (Mesa checks for ETIME and friends). */
status_t	lx_status(int error);

/*	Clones a kernel area into the calling team, readable and writable,
	with the source's memory type (write-combined) or, when \a cached,
	write-back; at *_address exactly when \a exact, anywhere otherwise. */
area_id		lx_area_clone_to_user(area_id source, void** _address,
				bool exact, bool cached);


/* Wait queues: a generation count and a ConditionVariable in the storage. */
typedef struct wait_queue_head {
	int32				generation;
	unsigned long long	storage[12];
} wait_queue_head_t;

void	init_waitqueue_head(wait_queue_head_t* queue);
void	wake_up_all(wait_queue_head_t* queue);
int32	lx_wait_queue_generation(wait_queue_head_t* queue);
void	lx_wait_queue_sleep(wait_queue_head_t* queue, int32 generation,
			bigtime_t deadline);
/*	The same, but a signal ends the wait too: B_INTERRUPTED then, B_OK
	otherwise (woken or past the deadline). For waits userland asked for. */
status_t	lx_wait_queue_sleep_interruptible(wait_queue_head_t* queue,
				int32 generation, bigtime_t deadline);

/* How many GPU buffers (and their bytes), single pages, vmaps and imports
   of team memory exist. */
void	lx_memory_stats(uint32* buffers, uint64* bufferBytes, uint32* pages,
			uint32* vmaps, uint32* imports);
/* How many GPU buffers were made since boot, and their bytes. */
void	lx_memory_totals(uint64* buffers, uint64* bytes);

/* Whether all of [address, address + size) is userland memory. */
bool	lx_access_ok(const void* address, unsigned long size);


/* The highest physical address + 1 mask the GPU can reach (dma_set_mask). */
extern unsigned long long lx_dma_mask;


/* One physically contiguous piece of a buffer. */
struct lx_dma_run {
	unsigned long long	address;
	unsigned long long	size;
};

/* A locked kernel area, Normal-NC, with its physical pieces. */
struct lx_dma_buffer {
	area_id				area;
	void*				address;
	size_t				size;
	uint32				run_count;
	struct lx_dma_run*	runs;
	bool				imported;	/* a locked view of a team's memory */
};

int		lx_dma_buffer_alloc(struct lx_dma_buffer* buffer, size_t size,
			const char* name);
void	lx_dma_buffer_free(struct lx_dma_buffer* buffer);
/*	[address, address + size) of the calling team as a buffer: the area
	holding it is cloned into the kernel (so the pages outlive the team's
	mapping) and the range locked until lx_dma_buffer_free(). Page aligned;
	the GPU must reach every page. */
int		lx_dma_buffer_import(struct lx_dma_buffer* buffer, const void* address,
			size_t size);
int		lx_dma_buffer_address(const struct lx_dma_buffer* buffer,
			size_t offset, unsigned long long* _address);


/* Single pages for page tables; struct page is private to the C++ side. */
struct page;

enum dma_data_direction {
	DMA_BIDIRECTIONAL = 0,
	DMA_TO_DEVICE = 1,
	DMA_FROM_DEVICE = 2,
	DMA_NONE = 3
};

typedef struct {
	unsigned long	value;
} pgprot_t;

#define LX_PGPROT_WRITECOMBINE	0x1

struct page*		alloc_page(unsigned int flags);
void				__free_page(struct page* page);
void				lx_free_pages_flush(void);
unsigned long long	dma_map_page(struct device* device, struct page* page,
						size_t offset, size_t size,
						enum dma_data_direction direction);
void				dma_unmap_page(struct device* device,
						unsigned long long address, size_t size,
						enum dma_data_direction direction);
void*				vmap(struct page** pages, unsigned int count,
						unsigned long flags, pgprot_t protection);
void				vunmap(const void* address);


/* Firmware files. */
struct firmware {
	size_t			size;
	const uint8*	data;
	char			path[B_PATH_NAME_LENGTH];
};

int		request_firmware(const struct firmware** _firmware, const char* name,
			struct device* device);
void	release_firmware(const struct firmware* firmware);


#ifdef __cplusplus
}
#endif


#endif	/* POWERVR_LX_HAIKU_H */
