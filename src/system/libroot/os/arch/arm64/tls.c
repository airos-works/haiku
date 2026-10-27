/*
 * Copyright 2019-2022, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */

#include <runtime_loader/runtime_loader.h>

#include <support/TLS.h>
#include <tls.h>

#include <arch_elf.h>


struct tls_index {
	unsigned long ti_module;
	unsigned long ti_offset;
};


static int32 gNextSlot = TLS_FIRST_FREE_SLOT;

void* __tls_get_addr(struct tls_index* ti);


static inline void**
get_tls()
{
	void **tls;
	asm volatile("MRS %0, tpidr_el0" : "=r" (tls));
	return tls;
}


int32
tls_allocate(void)
{
	int32 next = atomic_add(&gNextSlot, 1);
	if (next >= TLS_MAX_KEYS)
		return B_NO_MEMORY;

	return next;
}


void *
tls_get(int32 index)
{
	return get_tls()[index];
}


void **
tls_address(int32 index)
{
	return get_tls() + index;
}


void
tls_set(int32 index, void *value)
{
	get_tls()[index] = value;
}


void*
__tls_get_addr(struct tls_index* ti)
{
	// The common case without a call into the runtime loader: this thread's
	// vector is of the current generation and has the image's block.
	void** vector = (void**)get_tls()[TLS_DYNAMIC_THREAD_VECTOR];
	const int32* generation = __gRuntimeLoader->tls_generation;
	if (vector != NULL && generation != NULL) {
		const unsigned* vectorGeneration = (const unsigned*)vector[0];
		if (ti->ti_module < vectorGeneration[1]
			&& vectorGeneration[0] == (unsigned)*generation) {
			void* block = vector[ti->ti_module + 1];
			if (block != NULL)
				return (uint8*)block + TLS_DTV_OFFSET + ti->ti_offset;
		}
	}

	return __gRuntimeLoader->get_tls_address(ti->ti_module, ti->ti_offset);
}
