/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef LAZY_BINDING_H
#define LAZY_BINDING_H


#include <runtime_loader.h>


struct SymbolLookupInfo;


// Lazy binding of PLT (jump slot) relocations.
//
// Only the images loaded with the program take part: they are never unloaded
// while it runs, and the images that were loaded when they were relocated
// (the "scope" below) never go away either, so a jump slot can be resolved
// later, from any thread, without the loader lock, and still bind to exactly
// what eager binding would have bound it to. Images loaded later
// (load_add_on(), dlopen()) are always bound at load.


void		lazy_binding_init(image_t* programImage, bool runtimeLoaderAddOns);
void		lazy_binding_done();
bool		lazy_binding_enabled();
addr_t*		lazy_binding_plt_got(image_t* rootImage, image_t* image);
void		lazy_binding_note_deferred(uint32 count);
uint32		lazy_binding_deferred_count();
bool		lazy_binding_slot_target_valid(image_t* image, addr_t target);

elf_sym*	find_undefined_symbol_lazy(image_t* rootImage, image_t* image,
				const SymbolLookupInfo& lookupInfo, image_t** _foundInImage);

extern "C" addr_t lazy_binding_fixup(image_t* image, size_t relocationIndex)
	__attribute__((visibility("hidden")));

// architecture specific
extern "C" void arch_lazy_binding_entry() __attribute__((visibility("hidden")));
void		arch_lazy_binding_init();


#endif	// LAZY_BINDING_H
