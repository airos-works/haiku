/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	mimalloc as libroot's allocator: the malloc() family and the hooks libroot
	calls at process start, thread exit and around fork().

	The whole of mimalloc is compiled into this one object (as its static.c
	does), with hidden visibility, so that only the functions below are
	exported from libroot.
*/

#include <errno.h>
#include <malloc.h>
#include <stdlib.h>

#include <OS.h>

#include <libroot_private.h>
#include <system/tls.h>


// #pragma mark - configuration of mimalloc


#define MI_HAIKU_LIBROOT			1

// The thread's default heap and its companions live in fixed TLS slots.
#define MI_HAIKU_TLS_SLOT_DEFAULT	TLS_MALLOC_SLOT
#define MI_HAIKU_TLS_SLOT_CACHED	TLS_MALLOC_CACHED_SLOT
#define MI_HAIKU_TLS_SLOT_MAIN		TLS_MALLOC_MAIN_SLOT
#define MI_HAIKU_TLS_SLOT_LOCALS	TLS_MALLOC_LOCALS_SLOT

static inline void**
mi_haiku_tls_slot_address(size_t slot)
{
	// the thread pointer is the address of the thread's TLS slots
	return (void**)__builtin_thread_pointer() + slot;
}

// Address space is reserved 64 MiB at a time (mimalloc's default is 1 GiB).
// When arenas are committed with mprotect() (the strict mode of the Haiku
// primitives), the kernel keeps a protection entry for every page of the
// area, which would be 128 KiB of kernel memory per GiB, in every team.
#ifndef MI_DEFAULT_ARENA_RESERVE
#	define MI_DEFAULT_ARENA_RESERVE	(64L * 1024L)
#endif

// Nothing is torn down at exit: the kernel frees the memory.
#define MI_SKIP_COLLECT_ON_EXIT		1

#include "static.c"

// this file is compiled with hidden visibility
#define EXPORT	__attribute__((visibility("default")))


// #pragma mark - the malloc() family


EXPORT void*
malloc(size_t size)
{
	return mi_malloc(size);
}


EXPORT void*
calloc(size_t count, size_t size)
{
	return mi_calloc(count, size);
}


EXPORT void*
realloc(void* pointer, size_t size)
{
	return mi_realloc(pointer, size);
}


EXPORT void
free(void* pointer)
{
	mi_free(pointer);
}


EXPORT int
posix_memalign(void** _pointer, size_t alignment, size_t size)
{
	return mi_posix_memalign(_pointer, alignment, size);
}


EXPORT void*
aligned_alloc(size_t alignment, size_t size)
{
	return mi_aligned_alloc(alignment, size);
}


EXPORT void*
memalign(size_t alignment, size_t size)
{
	return mi_memalign(alignment, size);
}


EXPORT void*
valloc(size_t size)
{
	return mi_valloc(size);
}


EXPORT size_t
malloc_usable_size(void* pointer)
{
	return mi_usable_size(pointer);
}


// #pragma mark - libroot hooks


EXPORT status_t
__init_heap()
{
	_mi_auto_process_init();
	return B_OK;
}


EXPORT void
__heap_thread_init()
{
	// A thread's heap is created with its first allocation. Until then its
	// slots hold the empty heap, as mimalloc's thread local variables would:
	// not all of mimalloc copes with a NULL heap (realloc() of another
	// thread's block as the first call of a thread crashed).
	mi_prim_tls_slot_set(MI_HAIKU_TLS_SLOT_DEFAULT, (void*)&_mi_theap_empty);
	mi_prim_tls_slot_set(MI_HAIKU_TLS_SLOT_CACHED, (void*)&_mi_theap_empty);
}


EXPORT void
__heap_thread_exit()
{
	// Hands the thread's pages over; other threads can take them. Blocks that
	// are still in use stay valid.
	_mi_thread_done(NULL);
}


EXPORT void
__heap_terminate_after()
{
	if (mi_option_is_enabled(mi_option_show_stats)
		|| mi_option_is_enabled(mi_option_verbose)) {
		mi_stats_print(NULL);
	}
}


/*!	The locks of mimalloc. libroot only uses the main heap of the main
	subprocess, so these are all of them. Most of mimalloc is lock free; the
	locks guard the lists of arenas, heaps and abandoned pages.
*/
static mi_lock_t*
heap_lock_at(size_t index)
{
	switch (index) {
		case 0: return &subprocs_lock;
		case 1: return &subproc_main.heaps_lock;
		case 2: return &subproc_main.arena_reserve_lock;
		case 3: return &heap_main.theaps_lock;
		case 4: return &heap_main.arena_pages_lock;
		case 5: return &heap_main.os_abandoned_pages_lock;
		case 6: return &mi_thread_locals_lock;
		case 7: return &mi_page_map_lock;
		default: return NULL;
	}
}

#define HEAP_LOCK_COUNT	8


EXPORT void
__heap_before_fork()
{
	// The child must not inherit a lock that another thread holds. Take all
	// of them; as the order in which mimalloc nests them is its own business,
	// back off and start again whenever one is busy.
	while (true) {
		size_t i;
		mi_lock_acquire(heap_lock_at(0));
		for (i = 1; i < HEAP_LOCK_COUNT; i++) {
			if (!mi_lock_try_acquire(heap_lock_at(i)))
				break;
		}
		if (i == HEAP_LOCK_COUNT)
			return;

		while (i-- > 0)
			mi_lock_release(heap_lock_at(i));
		mi_atomic_yield();
	}
}


EXPORT void
__heap_after_fork_parent()
{
	size_t i = HEAP_LOCK_COUNT;
	while (i-- > 0)
		mi_lock_release(heap_lock_at(i));
}


EXPORT void
__heap_after_fork_child()
{
	// The forking thread is the only one in the child, with a new ID: set the
	// locks up again. The heaps of the other threads stay behind as they
	// were: their memory is valid, and blocks from them can be freed.
	size_t i;
	for (i = 0; i < HEAP_LOCK_COUNT; i++)
		mi_lock_init(heap_lock_at(i));
}
