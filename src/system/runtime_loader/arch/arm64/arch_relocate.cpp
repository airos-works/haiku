/*
 * Copyright 2019 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "arch_elf.h"
#include "lazy_binding.h"
#include "runtime_loader_private.h"

#include <runtime_loader.h>


//#define TRACE_RLD
#ifdef TRACE_RLD
#	define TRACE(x) dprintf x
#else
#	define TRACE(x) ;
#endif


static status_t
relocate_rela(image_t* rootImage, image_t* image, Elf64_Rela* rel,
	size_t relLength, SymbolLookupCache* cache)
{
	for (size_t i = 0; i < relLength / sizeof(Elf64_Rela); i++) {
		int type = ELF64_R_TYPE(rel[i].r_info);
		int symIndex = ELF64_R_SYM(rel[i].r_info);
		Elf64_Addr symAddr = 0;
		image_t* symbolImage = NULL;

		// Resolve the symbol, if any.
		if (symIndex != 0) {
			Elf64_Sym* sym = SYMBOL(image, symIndex);

			status_t status = resolve_symbol(rootImage, image, sym, cache,
				&symAddr, &symbolImage);
			if (status != B_OK) {
				TRACE(("resolve symbol \"%s\" returned: %" B_PRId32 "\n",
					SYMNAME(image, sym), status));
				printf("resolve symbol \"%s\" returned: %" B_PRId32 "\n",
					SYMNAME(image, sym), status);
				return status;
			}
		}

		// Address of the relocation.
		Elf64_Addr relocAddr = image->regions[0].delta + rel[i].r_offset;

		// Calculate the relocation value.
		Elf64_Addr relocValue;
		switch (type) {
			case R_AARCH64_NONE:
				continue;
			case R_AARCH64_ABS64:
			case R_AARCH64_GLOB_DAT:
			case R_AARCH64_JUMP_SLOT:
				relocValue = symAddr + rel[i].r_addend;
				break;
			case R_AARCH64_RELATIVE:
				relocValue = image->regions[0].delta + rel[i].r_addend;
				break;
			case R_AARCH64_TLS_DTPMOD64:
				relocValue = symbolImage == NULL
							? image->dso_tls_id : symbolImage->dso_tls_id;
				break;
			case R_AARCH64_TLS_DTPREL64:
				relocValue = symAddr;
				break;
			default:
				TRACE(("unhandled relocation type %d\n", type));
				return B_BAD_DATA;
		}

		*(Elf64_Addr *)relocAddr = relocValue;
	}

	return B_OK;
}


void
arch_lazy_binding_init()
{
}


/*!	Prepares the PLT of \a image for lazy binding: the jump slots keep
	pointing to PLT0, which pushes the slot's address and jumps to GOT[2].
	arch_lazy_binding_entry() derives the relocation from the slot's position,
	so every PLT relocation must be a jump slot, the n-th one for GOT[3 + n].
	Returns an error and changes nothing if the PLT does not look like that.
*/
static status_t
setup_lazy_plt(image_t* image, addr_t* got)
{
	Elf64_Rela* relocations = (Elf64_Rela*)image->pltrel;
	size_t count = image->pltrel_len / sizeof(Elf64_Rela);
	addr_t delta = image->regions[0].delta;

	for (size_t i = 0; i < count; i++) {
		if (ELF64_R_TYPE(relocations[i].r_info) != R_AARCH64_JUMP_SLOT
			|| delta + relocations[i].r_offset != (addr_t)&got[3 + i]) {
			return B_BAD_DATA;
		}
		if (!lazy_binding_slot_target_valid(image, got[3 + i] + delta))
			return B_BAD_DATA;
	}

	for (size_t i = 0; i < count; i++)
		got[3 + i] += delta;

	got[1] = (addr_t)image;
	got[2] = (addr_t)&arch_lazy_binding_entry;
	lazy_binding_note_deferred(count);
	return B_OK;
}


status_t
arch_relocate_image(image_t *rootImage, image_t *image,
	SymbolLookupCache* cache)
{
	status_t status;

	// No REL on arm64.

	// Perform RELA relocations.
	if (image->rela) {
		status = relocate_rela(rootImage, image, image->rela, image->rela_len,
			cache);
		if (status != B_OK)
			return status;
	}

	// PLT relocations (they are RELA on arm64), bound on their first call
	// if the program binds lazily.
	if (image->pltrel) {
		addr_t* got = lazy_binding_plt_got(rootImage, image);
		if (got != NULL && setup_lazy_plt(image, got) == B_OK)
			return B_OK;

		status = relocate_rela(rootImage, image, (Elf64_Rela*)image->pltrel,
			image->pltrel_len, cache);
		if (status != B_OK)
			return status;
	}

	return B_OK;
}
