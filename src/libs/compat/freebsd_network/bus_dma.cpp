/*
 * Copyright 2019-2022, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Augustin Cavalier <waddlesplash>
 */

extern "C" {
#include <sys/malloc.h>
#include <sys/bus.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/mbuf.h>

#include <machine/bus.h>
#include <vm/vm_extern.h>

phys_addr_t vm_page_max_address();
	// declared in <vm/vm_page.h> which we can't include here.
}

#if defined(FBSD_NONCOHERENT_DMA)
#include <arch/arm64/cache_poc.h>
#include <driver_settings.h>

static bool sCachedPacketBuffers = false;
#endif


// #pragma mark - structures


struct bus_dma_tag {
	bus_dma_tag_t	parent;
	int32			ref_count;
	int32			map_count;

	int				flags;
#define BUS_DMA_COULD_BOUNCE	BUS_DMA_BUS1

	phys_size_t		alignment;
	phys_addr_t		boundary;
	phys_addr_t		lowaddr;
	phys_addr_t		highaddr;

	phys_size_t		maxsize;
	uint32			maxsegments;
	phys_size_t		maxsegsz;
};

struct bus_dmamap {
	bus_dma_tag_t		dmat;

	bus_dma_segment_t*	segments;
	int					nsegs;

	void*		bounce_buffer;
	bus_size_t	bounce_buffer_size;
	bus_addr_t	bounce_physical;
	bus_size_t	buffer_length;
	bool		loaded;
	bool		bounce_prohibited;
	bool		cacheable_bounce;

	enum {
		BUFFER_NONE = 0,
		BUFFER_DIRECT,

		BUFFER_TYPE_SIMPLE,
		BUFFER_TYPE_MBUF,
	} buffer_type;
	union {
		void*				buffer;
		struct mbuf*		mbuf;
	};
};

static int _allocate_dmamem(bus_dma_tag_t dmat, phys_size_t size,
	void** vaddr, int flags, bool cacheable = false);
static int _prepare_bounce_buffer(bus_dmamap_t map, bus_size_t reqsize,
	int flags);


// #pragma mark - functions


extern "C" void
_fbsd_init_bus_dma(const char* driverName)
{
#if defined(FBSD_NONCOHERENT_DMA)
	// Called once before driver attach and before taking Giant. Packet paths
	// must never read settings or allocate memory while holding driver locks.
	void* settings = load_driver_settings(driverName);
	sCachedPacketBuffers = settings != NULL && get_driver_boolean_parameter(
		settings, "cached_packet_buffers", false, false);
	if (settings != NULL)
		unload_driver_settings(settings);
	dprintf("%s: packet DMA buffers: %s\n", driverName,
		sCachedPacketBuffers ? "cached with explicit sync" : "non-cacheable");
#endif
}


extern "C" void
busdma_lock_mutex(void* arg, bus_dma_lock_op_t op)
{
	struct mtx* dmtx = (struct mtx*)arg;
	switch (op) {
	case BUS_DMA_LOCK:
		mtx_lock(dmtx);
	break;
	case BUS_DMA_UNLOCK:
		mtx_unlock(dmtx);
	break;
	default:
		panic("busdma_lock_mutex: unknown operation 0x%x", op);
	}
}


extern "C" int
bus_dma_tag_create(bus_dma_tag_t parent, bus_size_t alignment, bus_addr_t boundary,
	bus_addr_t lowaddr, bus_addr_t highaddr, bus_dma_filter_t* filter,
	void* filterarg, bus_size_t maxsize, int nsegments, bus_size_t maxsegsz,
	int flags, bus_dma_lock_t* lockfunc, void* lockfuncarg, bus_dma_tag_t* dmat)
{
	if (dmat == NULL)
		return EINVAL;
	*dmat = NULL;
	// Parent tags may describe an unrestricted scatter/gather count without
	// ever allocating a map themselves (RTL8125 does this for its DMA ceiling).
	if (nsegments == (int)BUS_SPACE_UNRESTRICTED)
		nsegments = INT32_MAX;
	if (alignment == 0 || (alignment & (alignment - 1)) != 0
		|| (boundary != 0 && (boundary & (boundary - 1)) != 0)
		|| maxsegsz == 0 || maxsize == 0 || nsegments <= 0
		|| lowaddr > highaddr) {
		return EINVAL;
	}
	if (filter != NULL)
		return EOPNOTSUPP;

	bus_dma_tag_t newtag = (bus_dma_tag_t)kernel_malloc(sizeof(*newtag),
		M_DEVBUF, M_ZERO | M_NOWAIT);
	if (newtag == NULL)
		return ENOMEM;

	if (boundary != 0 && boundary < maxsegsz)
		maxsegsz = boundary;

	newtag->alignment = alignment;
	newtag->boundary = boundary;
	newtag->lowaddr = lowaddr;
	newtag->highaddr = highaddr;
	newtag->maxsize = maxsize;
	newtag->maxsegments = nsegments;
	newtag->maxsegsz = maxsegsz;
	newtag->flags = flags;
	newtag->ref_count = 1;
	newtag->map_count = 0;

	// lockfunc is only needed if callbacks will be invoked asynchronously.

	if (parent != NULL) {
		newtag->parent = parent;
		atomic_add(&parent->ref_count, 1);

		newtag->lowaddr = MIN(parent->lowaddr, newtag->lowaddr);
		newtag->highaddr = MAX(parent->highaddr, newtag->highaddr);
		newtag->alignment = MAX(parent->alignment, newtag->alignment);

		if (newtag->boundary == 0) {
			newtag->boundary = parent->boundary;
		} else if (parent->boundary != 0) {
			newtag->boundary = MIN(parent->boundary, newtag->boundary);
		}
	}

	if (newtag->boundary != 0)
		newtag->maxsegsz = MIN(newtag->maxsegsz, newtag->boundary);
	if (newtag->lowaddr < vm_page_max_address())
		newtag->flags |= BUS_DMA_COULD_BOUNCE;
	if (newtag->alignment > 1)
		newtag->flags |= BUS_DMA_COULD_BOUNCE;

	*dmat = newtag;
	return 0;
}


extern "C" int
bus_dma_tag_destroy(bus_dma_tag_t dmat)
{
	if (dmat == NULL)
		return 0;
	if (dmat->map_count != 0)
		return EBUSY;

	while (dmat != NULL) {
		bus_dma_tag_t parent;

		parent = dmat->parent;
		atomic_add(&dmat->ref_count, -1);
		if (dmat->ref_count == 0) {
			kernel_free(dmat, M_DEVBUF);

			// Last reference released, so release our reference on our parent.
			dmat = parent;
		} else
			dmat = NULL;
	}
	return 0;
}


static int
_create_map(bus_dma_tag_t dmat, int flags, bus_dmamap_t* mapp, bool noBounce)
{
	if (mapp == NULL)
		return EINVAL;
	*mapp = NULL;
	if (dmat == NULL || dmat->maxsegments > SIZE_MAX / sizeof(bus_dma_segment_t))
		return EINVAL;
	*mapp = (bus_dmamap_t)kernel_malloc(sizeof(**mapp), M_DEVBUF,
		M_ZERO | M_NOWAIT);
	if (*mapp == NULL)
		return ENOMEM;

	(*mapp)->dmat = dmat;
	(*mapp)->bounce_prohibited = noBounce;
#if defined(FBSD_NONCOHERENT_DMA)
	(*mapp)->cacheable_bounce = !noBounce && sCachedPacketBuffers;
#endif
	(*mapp)->segments = (bus_dma_segment_t*)kernel_malloc(
		dmat->maxsegments * sizeof(bus_dma_segment_t), M_DEVBUF,
		M_ZERO | M_NOWAIT);
	if ((*mapp)->segments == NULL) {
		kernel_free((*mapp), M_DEVBUF);
		*mapp = NULL;
		return ENOMEM;
	}

	bool reserveBounce = ((flags | dmat->flags) & BUS_DMA_ALLOCNOW) != 0
		&& (dmat->flags & BUS_DMA_COULD_BOUNCE) != 0;
#if defined(FBSD_NONCOHERENT_DMA)
	// Packet loads run under driver locks. Reserve private DMA RAM when the
	// map is created, including for callers which omit the allocation hint.
	reserveBounce = true;
#endif
	if (!noBounce && reserveBounce) {
		int error = _prepare_bounce_buffer(*mapp, dmat->maxsize, flags);
		if (error != 0) {
			kernel_free((*mapp)->segments, M_DEVBUF);
			kernel_free(*mapp, M_DEVBUF);
			*mapp = NULL;
			return error;
		}
	}

	atomic_add(&dmat->map_count, 1);
	return 0;
}


extern "C" int
bus_dmamap_create(bus_dma_tag_t dmat, int flags, bus_dmamap_t* mapp)
{
	return _create_map(dmat, flags, mapp, false);
}


extern "C" int
bus_dmamap_destroy(bus_dma_tag_t dmat, bus_dmamap_t map)
{
	if (map == NULL)
		return 0;
	if (dmat == NULL || map->dmat != dmat)
		return EINVAL;
	if (map->loaded)
		return EBUSY;

	atomic_add(&map->dmat->map_count, -1);
	kernel_contigfree(map->bounce_buffer, map->bounce_buffer_size, M_DEVBUF);
	kernel_free(map->segments, M_DEVBUF);
	kernel_free(map, M_DEVBUF);
	return 0;
}


static int
_allocate_dmamem(bus_dma_tag_t dmat, phys_size_t size, void** vaddr, int flags,
	bool cacheable)
{
	*vaddr = NULL;
	if (size == 0 || size > SIZE_MAX - (B_PAGE_SIZE - 1))
		return EINVAL;
	// A tag boundary constrains each segment, not the whole allocation.
	// The mapper splits a larger allocation at every boundary below.
	bus_addr_t boundary = size <= dmat->boundary ? dmat->boundary : 0;
	int mflags;
	if (flags & BUS_DMA_NOWAIT)
		mflags = M_NOWAIT;
	else
		mflags = M_WAITOK;

	if (flags & BUS_DMA_ZERO)
		mflags |= M_ZERO;

	// FreeBSD uses standard malloc() for the case where size <= PAGE_SIZE,
	// but we want to keep DMA'd memory a bit more separate, so we always use
	// contigmalloc.

	// The range specified by lowaddr, highaddr is an *exclusion* range,
	// not an inclusion range. So we want to at least start with the low end,
	// if possible. (The most common exclusion range is 32-bit only, and
	// ones other than that are very rare, so typically this will succeed.)
	if (dmat->lowaddr >= B_PAGE_SIZE - 1) {
		*vaddr = kernel_contigmalloc_etc(size, M_DEVBUF, mflags,
			0, dmat->lowaddr,
			dmat->alignment, boundary, cacheable);
		if (*vaddr == NULL)
			dprintf("bus_dmamem_alloc: failed to allocate with lowaddr "
				"0x%" B_PRIxPHYSADDR "\n", dmat->lowaddr);
	}
	if (*vaddr == NULL && dmat->highaddr < BUS_SPACE_MAXADDR) {
		*vaddr = kernel_contigmalloc_etc(size, M_DEVBUF, mflags,
			dmat->highaddr + 1, BUS_SPACE_MAXADDR,
			dmat->alignment, boundary, cacheable);
	}

	if (*vaddr == NULL) {
		dprintf("bus_dmamem_alloc: failed to allocate for tag (size %d, "
			"low 0x%" B_PRIxPHYSADDR ", high 0x%" B_PRIxPHYSADDR ", "
			"boundary 0x%" B_PRIxPHYSADDR ")\n",
			(int)size, dmat->lowaddr, dmat->highaddr, dmat->boundary);
		return ENOMEM;
	} else if (vtophys(*vaddr) & (dmat->alignment - 1)) {
		dprintf("bus_dmamem_alloc: allocation violates alignment\n");
		bus_dmamem_free_tagless(*vaddr, size);
		*vaddr = NULL;
		return ENOMEM;
	}

	return 0;
}


extern "C" int
bus_dmamem_alloc(bus_dma_tag_t dmat, void** vaddr, int flags,
	bus_dmamap_t* mapp)
{
	if (vaddr == NULL)
		return EINVAL;
	*vaddr = NULL;
	if (mapp != NULL)
		*mapp = NULL;
	if (dmat == NULL)
		return EINVAL;
	// FreeBSD does not permit the "mapp" argument to be NULL, but we do
	// (primarily for the OpenBSD shims.)
	if (mapp != NULL) {
		int error = _create_map(dmat, flags, mapp, true);
		if (error != 0)
			return error;
	}

	int status = _allocate_dmamem(dmat, dmat->maxsize, vaddr, flags);
	if (status != 0 && mapp != NULL) {
		bus_dmamap_destroy(dmat, *mapp);
		*mapp = NULL;
	}
	return status;
}


extern "C" void
bus_dmamem_free_tagless(void* vaddr, size_t size)
{
	kernel_contigfree(vaddr, size, M_DEVBUF);
}


extern "C" void
bus_dmamem_free(bus_dma_tag_t dmat, void* vaddr, bus_dmamap_t map)
{
	if (map != NULL && (map->dmat != dmat || map->loaded)) {
		panic("bus_dmamem_free: wrong tag or mapping still loaded");
		return;
	}
	bus_dmamem_free_tagless(vaddr, dmat->maxsize);
	bus_dmamap_destroy(dmat, map);
}


static int
_prepare_bounce_buffer(bus_dmamap_t map, bus_size_t reqsize, int flags)
{
	if (map->bounce_prohibited)
		return EINVAL;
	if (map->loaded)
		return EBUSY;

	if (map->bounce_buffer_size >= reqsize)
		return 0;

	void* buffer;
	int error = _allocate_dmamem(map->dmat, reqsize, &buffer, flags,
		map->cacheable_bounce);
	if (error != 0)
		return error;
	bus_addr_t physical = vtophys(buffer);
	kernel_contigfree(map->bounce_buffer, map->bounce_buffer_size, M_DEVBUF);
	map->bounce_buffer = buffer;
	map->bounce_buffer_size = reqsize;
	map->bounce_physical = physical;

	return 0;
}


static bool
_validate_address(bus_dma_tag_t dmat, bus_addr_t paddr, bus_size_t length,
	bool validateAlignment = true)
{
	if (length == 0 || length - 1 > BUS_SPACE_MAXADDR - paddr)
		return false;
	bus_addr_t last = paddr + length - 1;
	if (dmat->lowaddr < dmat->highaddr && last > dmat->lowaddr
		&& paddr <= dmat->highaddr) {
		return false;
	}
	return !validateAlignment || vm_addr_align_ok(paddr, dmat->alignment);
}


static bool
_valid_buffer(const void* buffer, bus_size_t length)
{
	return buffer != NULL && length != 0
		&& length - 1 <= UINTPTR_MAX - (addr_t)buffer;
}


static int
_bus_load_buffer(bus_dma_tag_t dmat, void* buf, bus_size_t buflen,
	bus_addr_t& lastPhysAddr, bus_dma_segment_t* segs, int& seg, bool first,
	bool physicalKnown = false, bus_addr_t physical = 0)
{
	vm_offset_t virtualAddress = (vm_offset_t)buf;
	const bus_addr_t boundaryMask = ~(dmat->boundary - 1);

	while (buflen > 0) {
		const bus_addr_t phys = physicalKnown ? physical
			: pmap_kextract(virtualAddress);
		bus_size_t size = MIN(PAGESIZE - (phys & PAGE_MASK), buflen);
		size = MIN(size, dmat->maxsegsz);
		if (dmat->boundary != 0) {
			bus_size_t remaining = dmat->boundary
				- (phys & (dmat->boundary - 1));
			size = MIN(size, remaining);
		}

		bool coalesce = !first && lastPhysAddr != 0 && phys == lastPhysAddr
			&& size <= dmat->maxsegsz - segs[seg].ds_len
			&& (dmat->boundary == 0
				|| (segs[seg].ds_addr & boundaryMask) == (phys & boundaryMask));
		if (!_validate_address(dmat, phys, size, !coalesce))
			return ERANGE;
		if (coalesce)
			segs[seg].ds_len += size;
		else {
			if (first)
				first = false;
			else if (++seg >= (int)dmat->maxsegments)
				return EFBIG;
			segs[seg].ds_addr = phys;
			segs[seg].ds_len = size;
		}
		lastPhysAddr = phys + size;
		virtualAddress += size;
		physical += size;
		buflen -= size;
	}
	return 0;
}


static int
_load_bounce(bus_dma_tag_t dmat, bus_dmamap_t map, bus_size_t length,
	bus_dma_segment_t* segments, int& seg, int flags)
{
	int error = _prepare_bounce_buffer(map, length, flags);
	if (error != 0)
		return error;
	seg = 0;
	bus_addr_t last = 0;
	return _bus_load_buffer(dmat, map->bounce_buffer, length, last, segments,
		seg, true, true, map->bounce_physical);
}


extern "C" int
bus_dmamap_load(bus_dma_tag_t dmat, bus_dmamap_t map, void* buf,
	bus_size_t buflen, bus_dmamap_callback_t* callback,
	void* callbackArg, int flags)
{
	if (callback == NULL)
		return EINVAL;
	if (dmat == NULL || map == NULL || map->dmat != dmat
		|| !_valid_buffer(buf, buflen) || buflen > dmat->maxsize) {
		callback(callbackArg, NULL, 0, EINVAL);
		return EINVAL;
	}
	if (map->loaded) {
		callback(callbackArg, NULL, 0, EBUSY);
		return EBUSY;
	}

	int seg = 0;
	bus_addr_t last = 0;
	bool noBounce = map->bounce_prohibited;
	int error;
#if defined(FBSD_NONCOHERENT_DMA)
	bus_addr_t physical;
	bool coherent = _kernel_contig_dma_address(buf, buflen, &physical);
	// An OpenBSD descriptor map is created separately from its allocation.
	// Never bounce such a ring, even if the caller supplies an unsuitable tag.
	noBounce |= coherent;
	error = coherent ? _bus_load_buffer(dmat, buf, buflen, last,
		map->segments, seg, true, true, physical) : ERANGE;
#else
	error = _bus_load_buffer(dmat, buf, buflen, last, map->segments, seg, true);
#endif
	bool bounced = error != 0;
	if (bounced && !noBounce)
		error = _load_bounce(dmat, map, buflen, map->segments, seg, flags);

	if (error == 0) {
		map->buffer_type = bounced ? bus_dmamap::BUFFER_TYPE_SIMPLE
			: bus_dmamap::BUFFER_DIRECT;
		map->buffer = buf;
		map->buffer_length = buflen;
		map->nsegs = seg + 1;
		map->loaded = true;
	}
	callback(callbackArg, map->segments, error == 0 ? seg + 1 : 0, error);
	// Segment/address errors are delivered to the callback, as before.
	return error == ENOMEM || error == EINVAL ? error : 0;
}


static bool
_valid_mbuf_chain(struct mbuf* mb, bus_size_t length)
{
	bus_size_t total = 0;
	for (struct mbuf* m = mb; m != NULL; m = m->m_next) {
		if (m->m_len < 0 || (bus_size_t)m->m_len > length - total
			|| (m->m_len != 0 && !_valid_buffer(m->m_data, m->m_len))) {
			return false;
		}
		total += m->m_len;
	}
	return total == length;
}


extern "C" int
bus_dmamap_load_mbuf_sg(bus_dma_tag_t dmat, bus_dmamap_t map, struct mbuf* mb,
	bus_dma_segment_t* segs, int* nsegs, int flags)
{
	if (nsegs == NULL)
		return EINVAL;
	*nsegs = 0;
	if (dmat == NULL || map == NULL || map->dmat != dmat || mb == NULL
		|| segs == NULL || (mb->m_flags & M_PKTHDR) == 0
		|| mb->m_pkthdr.len <= 0 || (bus_size_t)mb->m_pkthdr.len > dmat->maxsize
		|| !_valid_mbuf_chain(mb, mb->m_pkthdr.len)) {
		return EINVAL;
	}
	if (map->loaded)
		return EBUSY;

	int seg = 0, error = 0;
	flags |= BUS_DMA_NOWAIT;
#if defined(FBSD_NONCOHERENT_DMA)
	// Ordinary mbuf storage may share cache lines with unrelated CPU data.
	// Copy packets through private DMA RAM instead of invalidating shared lines.
	error = ERANGE;
#else
	bus_addr_t last = 0;
	bool first = true;
	for (struct mbuf* m = mb; m != NULL && error == 0; m = m->m_next) {
		if (m->m_len == 0)
			continue;
		error = _bus_load_buffer(dmat, m->m_data, m->m_len, last,
			segs, seg, first);
		first = false;
	}
#endif
	bool bounced = error != 0;
	if (bounced)
		error = _load_bounce(dmat, map, mb->m_pkthdr.len, segs, seg, flags);
	if (error != 0)
		return error;

	map->buffer_type = bounced ? bus_dmamap::BUFFER_TYPE_MBUF
		: bus_dmamap::BUFFER_DIRECT;
	map->mbuf = mb;
	map->buffer_length = mb->m_pkthdr.len;
	map->nsegs = seg + 1;
	map->loaded = true;
	*nsegs = seg + 1;
	return 0;
}


extern "C" int
bus_dmamap_load_mbuf(bus_dma_tag_t dmat, bus_dmamap_t map, struct mbuf* mb,
	bus_dmamap_callback2_t* callback, void* callbackArg, int flags)
{
	if (callback == NULL)
		return EINVAL;
	int nsegs = 0;
	int error = bus_dmamap_load_mbuf_sg(dmat, map, mb,
		map != NULL ? map->segments : NULL, &nsegs, flags);
	callback(callbackArg, map != NULL ? map->segments : NULL,
		error == 0 ? nsegs : 0, error == 0 ? map->buffer_length : 0, error);
	return error;
}


extern "C" void
bus_dmamap_unload(bus_dma_tag_t dmat, bus_dmamap_t map)
{
	if (map == NULL)
		return;
	if (map->dmat != dmat) {
		panic("bus_dmamap_unload: wrong tag");
		return;
	}
	map->loaded = false;
	map->buffer_type = bus_dmamap::BUFFER_NONE;
	map->buffer = NULL;
	map->buffer_length = 0;
	map->nsegs = 0;
}


// Copy within the existing chain only. m_copyback() may extend an mbuf chain
// with new allocations, which must not happen under an interrupt lock.
static void
_copy_mbuf(bus_dmamap_t map, bus_size_t offset, bus_size_t length, bool toHost)
{
	if (!_valid_mbuf_chain(map->mbuf, map->buffer_length)) {
		panic("bus_dmamap_sync: mbuf layout changed while loaded");
		return;
	}
	char* buffer = (char*)map->bounce_buffer + offset;
	struct mbuf* m = map->mbuf;
	while (m != NULL && offset >= (bus_size_t)m->m_len) {
		offset -= m->m_len;
		m = m->m_next;
	}
	while (length != 0) {
		if (m->m_len == 0) {
			m = m->m_next;
			continue;
		}
		bus_size_t size = MIN(length, (bus_size_t)m->m_len - offset);
		if (toHost)
			memcpy(m->m_data + offset, buffer, size);
		else
			memcpy(buffer, m->m_data + offset, size);
		buffer += size;
		length -= size;
		offset = 0;
		m = m->m_next;
	}
}


extern "C" void
bus_dmamap_sync(bus_dma_tag_t dmat, bus_dmamap_t map, bus_dmasync_op_t op)
{
	if (map != NULL)
		bus_dmamap_sync_etc(dmat, map, 0, map->buffer_length, op);
}


#if defined(FBSD_NONCOHERENT_DMA)
static void
_sync_bounce_cache(bus_dmamap_t map, bus_addr_t offset, bus_size_t length,
	bus_dmasync_op_t op)
{
	if (length == 0) {
		memory_full_barrier();
		return;
	}

	cpu_status irqState = disable_interrupts();
	const size_t lineSize = arm64_current_data_cache_line_size();
	if (lineSize > B_PAGE_SIZE) {
		restore_interrupts(irqState);
		panic("bus_dmamap_sync: DMA cache line exceeds private page alignment");
		return;
	}
	addr_t address = (addr_t)map->bounce_buffer + offset;
	const addr_t last = (address + length - 1) & ~(lineSize - 1);
	address &= ~(lineSize - 1);
	// The allocation owns complete pages, including the partial boundary lines.
	// POSTREAD must not clean stale CPU data over a device's completed write.
	for (;;) {
		if ((op & BUS_DMASYNC_PREREAD) != 0)
			arm64_clean_invalidate_data_cache_line_poc(address);
		else if ((op & BUS_DMASYNC_PREWRITE) != 0)
			arm64_clean_data_cache_line_poc(address);
		else
			arm64_invalidate_data_cache_line_poc(address);
		if (address == last)
			break;
		address += lineSize;
	}
	// ARM64's full barrier is DSB SY. Complete maintenance on this CPU before
	// allowing migration, publishing descriptors or copying received bytes.
	memory_full_barrier();
	restore_interrupts(irqState);
}
#endif


extern "C" void
bus_dmamap_sync_etc(bus_dma_tag_t dmat, bus_dmamap_t map,
	bus_addr_t offset, bus_size_t length, bus_dmasync_op_t op)
{
	if (map == NULL)
		return;
	if (map->dmat != dmat || !map->loaded || offset > map->buffer_length
		|| length > map->buffer_length - offset) {
		panic("bus_dmamap_sync: wrong tag, unloaded map or invalid range");
		return;
	}

#if (defined(__x86_64__) || defined(__i386__)) && !defined(FBSD_NONCOHERENT_DMA)
	if (map->buffer_type == bus_dmamap::BUFFER_DIRECT) {
		// x86 DMA is cache coherent, DMA memory is write-back, and x86 does
		// not reorder stores with later stores (an uncached MMIO doorbell
		// included) or loads with later loads, so a map that does not bounce
		// needs no fence; FreeBSD's x86 bus_dmamap_sync() is a compiler
		// barrier for such maps, too. The full fence cost an RTL8126's
		// interrupt thread 30 % of its time at 4 Gbit/s
		// (airos-works/haiku#122).
		__asm__ __volatile__("" ::: "memory");
		return;
	}
#endif

	if ((op & BUS_DMASYNC_PREWRITE) != 0) {
		if (map->buffer_type == bus_dmamap::BUFFER_TYPE_SIMPLE) {
			memcpy((char*)map->bounce_buffer + offset,
				(char*)map->buffer + offset, length);
		} else if (map->buffer_type == bus_dmamap::BUFFER_TYPE_MBUF)
			_copy_mbuf(map, offset, length, false);
	}
	// Coherent rings still require ordering, including PREREAD/POSTWRITE.
	// Full barriers also order the Normal Non-cacheable payload with MMIO.
#if defined(FBSD_NONCOHERENT_DMA)
	const bool cached = map->cacheable_bounce
		&& map->buffer_type != bus_dmamap::BUFFER_DIRECT;
#endif
	if ((op & (BUS_DMASYNC_PREWRITE | BUS_DMASYNC_PREREAD)) != 0) {
#if defined(FBSD_NONCOHERENT_DMA)
		if (cached)
			_sync_bounce_cache(map, offset, length, op);
		else
#endif
			memory_full_barrier();
	}
	if ((op & (BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE)) != 0) {
		memory_full_barrier();
#if defined(FBSD_NONCOHERENT_DMA)
		if (cached && (op & BUS_DMASYNC_POSTREAD) != 0)
			_sync_bounce_cache(map, offset, length, BUS_DMASYNC_POSTREAD);
#endif
	}

	if ((op & BUS_DMASYNC_POSTREAD) != 0) {
		if (map->buffer_type == bus_dmamap::BUFFER_TYPE_SIMPLE) {
			memcpy((char*)map->buffer + offset,
				(char*)map->bounce_buffer + offset, length);
		} else if (map->buffer_type == bus_dmamap::BUFFER_TYPE_MBUF)
			_copy_mbuf(map, offset, length, true);
	}
}
