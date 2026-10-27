/*
 * Copyright 2014, Paweł Dziepak, pdziepak@quarnos.org.
 * Copyright 2012, Alex Smith, alex@alex-smith.me.uk.
 * Distributed under the terms of the MIT License.
 */


#include <atomic>

#include <runtime_loader/runtime_loader.h>

#include <support/TLS.h>
#include <tls.h>

#include <assert.h>

#include <arch_elf.h>


struct tls_index {
	unsigned long int	module;
	unsigned long int	offset;
};


static std::atomic<int> gNextSlot(TLS_FIRST_FREE_SLOT);


static inline void**
get_tls()
{
	void** tls;
	__asm__ __volatile__ ("movq	%%fs:0, %0" : "=r" (tls));
	return tls;
}


int32
tls_allocate()
{
	if (gNextSlot < TLS_MAX_KEYS) {
		auto next = gNextSlot++;
		if (next < TLS_MAX_KEYS)
			return next;
	}

	return B_NO_MEMORY;
}


void*
tls_get(int32 index)
{
	return get_tls()[index];
}


void**
tls_address(int32 index)
{
	return get_tls() + index;
}


void
tls_set(int32 index, void* value)
{
	get_tls()[index] = value;
}


extern "C" void*
__tls_get_addr(tls_index* ti)
{
	// The common case without a call into the runtime loader: this thread's
	// vector is of the current generation and has the image's block.
	void** vector = (void**)get_tls()[TLS_DYNAMIC_THREAD_VECTOR];
	const int32* generation = __gRuntimeLoader->tls_generation;
	if (vector != NULL && generation != NULL) {
		const unsigned* vectorGeneration = (const unsigned*)vector[0];
		if (ti->module < vectorGeneration[1]
			&& vectorGeneration[0] == (unsigned)*generation) {
			void* block = vector[ti->module + 1];
			if (block != NULL)
				return (uint8*)block + TLS_DTV_OFFSET + ti->offset;
		}
	}

	return __gRuntimeLoader->get_tls_address(ti->module, ti->offset);
}

