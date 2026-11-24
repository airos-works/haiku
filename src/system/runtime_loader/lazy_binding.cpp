/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "lazy_binding.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <syscalls.h>

#include "elf_symbol_lookup.h"
#include "images.h"
#include "runtime_loader_private.h"


// Whether programs bind their jump slots lazily when the environment does not
// say (LD_BIND_LAZY, LD_BIND_NOW). Off until it has run on real machines.
static const bool kLazyBindingDefault = false;

#ifndef DT_FLAGS_1
#	define DT_FLAGS_1	0x6ffffffb
#endif
#ifndef DF_1_NOW
#	define DF_1_NOW		0x00000001
#endif


static image_t* sLazyRoot = NULL;
	// the program image, once the program binds lazily
static bool sDeferring = false;
	// while the program's images are relocated
static image_t** sScope = NULL;
static uint32 sScopeCount = 0;
	// the images a global lookup searched when they were relocated, in order
static uint32 sDeferredCount = 0;


#if defined(__x86_64__) || defined(__aarch64__)
static bool
lazy_binding_requested()
{
	const char* bindNow = getenv("LD_BIND_NOW");
	if (bindNow != NULL && bindNow[0] != '\0')
		return false;

	const char* lazy = getenv("LD_BIND_LAZY");
	if (lazy != NULL && lazy[0] != '\0')
		return strcmp(lazy, "0") != 0;

	return kLazyBindingDefault;
}
#endif


static bool
image_binds_now(image_t* image)
{
	elf_dyn* dynamic = (elf_dyn*)image->dynamic_ptr;
	for (int i = 0; dynamic[i].d_tag != DT_NULL; i++) {
		switch (dynamic[i].d_tag) {
			case DT_BIND_NOW:
				return true;
			case DT_FLAGS:
				if ((dynamic[i].d_un.d_val & DF_BIND_NOW) != 0)
					return true;
				break;
			case DT_FLAGS_1:
				if ((dynamic[i].d_un.d_val & DF_1_NOW) != 0)
					return true;
				break;
		}
	}

	return false;
}


/*!	Called by load_program() once the program's images are loaded and marked
	RTLD_GLOBAL, before they are relocated: decides whether they bind lazily
	and, if so, records the lookup scope their relocation uses.
*/
void
lazy_binding_init(image_t* programImage, bool runtimeLoaderAddOns)
{
#if defined(__x86_64__) || defined(__aarch64__)
	// Runtime loader add-ons can patch symbols, which needs the loader lock.
	if (runtimeLoaderAddOns || !lazy_binding_requested()
		|| programImage->find_undefined_symbol
			!= find_undefined_symbol_global) {
		return;
	}

	// The same images find_undefined_symbol_global() would search for any
	// image relocated with the program as root, in the same order. Images
	// loaded later are queued after them; images unloaded later were loaded
	// later.
	image_queue_t& loadedImages = get_loaded_images();
	uint32 count = 0;
	for (image_t* image = loadedImages.head; image != NULL;
			image = image->next) {
		count++;
	}

	sScope = (image_t**)malloc(sizeof(image_t*) * count);
	if (sScope == NULL)
		return;

	for (image_t* image = loadedImages.head; image != NULL;
			image = image->next) {
		if (image == programImage
			|| (image->type != B_ADD_ON_IMAGE
				&& (image->flags & (RTLD_GLOBAL | RFLAG_USE_FOR_RESOLVING))
					!= 0)) {
			sScope[sScopeCount++] = image;
		}
	}

	arch_lazy_binding_init();
	sLazyRoot = programImage;
	sDeferring = true;
#else
	(void)programImage;
	(void)runtimeLoaderAddOns;
#endif
}


/*!	The program's images are relocated: images loaded from now on bind at
	load.
*/
void
lazy_binding_done()
{
	sDeferring = false;
}


bool
lazy_binding_enabled()
{
	return sLazyRoot != NULL;
}


/*!	Returns the PLT's GOT of \a image if its jump slots shall be bound lazily,
	or NULL if they are to be bound now.
*/
addr_t*
lazy_binding_plt_got(image_t* rootImage, image_t* image)
{
	if (!sDeferring || rootImage != sLazyRoot || image->pltrel == NULL
		|| image_binds_now(image)) {
		return NULL;
	}

	elf_dyn* dynamic = (elf_dyn*)image->dynamic_ptr;
	for (int i = 0; dynamic[i].d_tag != DT_NULL; i++) {
		if (dynamic[i].d_tag == DT_PLTGOT) {
			return (addr_t*)(dynamic[i].d_un.d_ptr
				+ image->regions[0].delta);
		}
	}

	return NULL;
}


void
lazy_binding_note_deferred(uint32 count)
{
	sDeferredCount += count;
}


uint32
lazy_binding_deferred_count()
{
	return sDeferredCount;
}


/*!	Whether \a target (a run time address) lies in an executable segment of
	\a image, as the PLT entry an unbound jump slot points to must.
*/
bool
lazy_binding_slot_target_valid(image_t* image, addr_t target)
{
	for (uint32 i = 0; i < image->num_regions; i++) {
		const elf_region_t& region = image->regions[i];
		if ((region.flags & RFLAG_EXECUTABLE) != 0
			&& target >= region.vmstart
			&& target - region.vmstart < region.vmsize) {
			return true;
		}
	}

	return false;
}


/*!	find_undefined_symbol_global() over the lazy binding scope. The scope and
	the images in it never change while the program runs, so this needs no
	lock.
*/
elf_sym*
find_undefined_symbol_lazy(image_t* rootImage, image_t* image,
	const SymbolLookupInfo& lookupInfo, image_t** _foundInImage)
{
	image_t* candidateImage = NULL;
	elf_sym* candidateSymbol = NULL;

	bool symbolic = (image->flags & RFLAG_SYMBOLIC) != 0;
	if (symbolic) {
		candidateSymbol = find_symbol(image, lookupInfo);
		if (candidateSymbol != NULL) {
			if (candidateSymbol->Bind() != STB_WEAK) {
				*_foundInImage = image;
				return candidateSymbol;
			}

			candidateImage = image;
		}
	}

	for (uint32 i = 0; i < sScopeCount; i++) {
		image_t* otherImage = sScope[i];
		if (otherImage == rootImage && symbolic)
			continue;

		if (elf_sym* symbol = find_symbol(otherImage, lookupInfo)) {
			*_foundInImage = otherImage;
			return symbol;
		}
	}

	if (candidateSymbol != NULL)
		*_foundInImage = candidateImage;

	return candidateSymbol;
}


#if defined(__x86_64__) || defined(__aarch64__)


/*!	Called by the architecture's lazy binding entry the first time a jump
	slot is called through: resolves the slot's symbol, writes the slot and
	returns the function to continue in. It may run in any thread at the same
	time, and in a signal handler: it neither locks nor allocates. Two threads
	resolving the same slot write the same value.
*/
extern "C" addr_t
lazy_binding_fixup(image_t* image, size_t relocationIndex)
{
	Elf64_Rela* relocations = (Elf64_Rela*)image->pltrel;
	const char* symbolName = "?";

	if (relocationIndex < image->pltrel_len / sizeof(Elf64_Rela)) {
		Elf64_Rela& relocation = relocations[relocationIndex];
		elf_sym* symbol = SYMBOL(image, ELF64_R_SYM(relocation.r_info));
		symbolName = SYMNAME(image, symbol);

		addr_t address;
		if (resolve_symbol_lazily(sLazyRoot, image, symbol, &address)
				== B_OK) {
			address += relocation.r_addend;
			__atomic_store_n((addr_t*)(image->regions[0].delta
				+ relocation.r_offset), address, __ATOMIC_RELEASE);
			return address;
		}
	}

	// Eager binding would not have started the program. The launching
	// application is not waiting any more, so say it on standard error.
	char message[512];
	int length = snprintf(message, sizeof(message),
		"runtime_loader: %s: cannot resolve symbol '%s' (PLT relocation %lu) "
		"on its first call\n", image->path, symbolName,
		(unsigned long)relocationIndex);
	if (length > (int)sizeof(message) - 1)
		length = sizeof(message) - 1;
	_kern_write(2, -1, message, length);
	_kern_exit_team(127);
	return 0;
}


#endif	// __x86_64__ || __aarch64__
