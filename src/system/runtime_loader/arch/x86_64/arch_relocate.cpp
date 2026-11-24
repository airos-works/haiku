/*
 * Copyright 2012, Alex Smith, alex@alex-smith.me.uk.
 * Distributed under the terms of the MIT License.
 */


#include "runtime_loader_private.h"

#include <runtime_loader.h>

#include "lazy_binding.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>


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
			case R_X86_64_NONE:
				continue;
			case R_X86_64_64:
			case R_X86_64_GLOB_DAT:
			case R_X86_64_JUMP_SLOT:
				relocValue = symAddr + rel[i].r_addend;
				break;
			case R_X86_64_PC32:
				relocValue = symAddr + rel[i].r_addend - rel[i].r_offset;
				break;
			case R_X86_64_RELATIVE:
				relocValue = image->regions[0].delta + rel[i].r_addend;
				break;
			case R_X86_64_DTPMOD64:
				relocValue = symbolImage == NULL
							? image->dso_tls_id : symbolImage->dso_tls_id;
				break;
			case R_X86_64_DTPOFF32:
			case R_X86_64_DTPOFF64:
				relocValue = symAddr;
				break;
			default:
				TRACE(("unhandled relocation type %d\n", type));
				return B_BAD_DATA;
		}

		if (type == R_X86_64_PC32 || type == R_X86_64_DTPOFF32)
			*(Elf32_Addr *)relocAddr = relocValue;
		else
			*(Elf64_Addr *)relocAddr = relocValue;
	}

	return B_OK;
}


// read by arch_lazy_binding_entry() (lazy_binding.S)
extern "C" {
uint64 gLazyBindingSaveSize __attribute__((visibility("hidden"))) = 512;
	// bytes of vector register state it saves on the stack
uint32 gLazyBindingXsaveMask[2] __attribute__((visibility("hidden")));
	// XSAVE components to save; zero: FXSAVE
}


static inline void
cpuid(uint32 leaf, uint32 subLeaf, uint32* eax, uint32* ebx, uint32* ecx,
	uint32* edx)
{
	asm volatile("cpuid"
		: "=a" (*eax), "=b" (*ebx), "=c" (*ecx), "=d" (*edx)
		: "a" (leaf), "c" (subLeaf));
}


/*!	Sizes the register save area of arch_lazy_binding_entry(). It saves every
	vector register an argument can be passed in (xmm0-7 and, with AVX and
	AVX-512, their wider forms, k registers included) with XSAVE when the
	kernel enabled it, else the SSE state with FXSAVE.
*/
void
arch_lazy_binding_init()
{
	uint32 eax, ebx, ecx, edx;
	cpuid(1, 0, &eax, &ebx, &ecx, &edx);
	if ((ecx & (1 << 27)) == 0)
		return;
			// no OSXSAVE: FXSAVE

	uint32 xcr0Low, xcr0High;
	asm volatile("xgetbv" : "=a" (xcr0Low), "=d" (xcr0High) : "c" (0));

	// x87, SSE, AVX, AVX-512 opmask, ZMM_Hi256, Hi16_ZMM. Not the MPX bound
	// registers, nor AMX tiles, which are neither arguments nor touched by
	// the loader.
	uint32 mask = xcr0Low & 0xe7;
	uint32 size = 512 + 64;
		// legacy area and XSAVE header
	for (uint32 component = 2; component < 8; component++) {
		if ((mask & (1 << component)) == 0)
			continue;
		cpuid(0xd, component, &eax, &ebx, &ecx, &edx);
		if (ebx + eax > size)
			size = ebx + eax;
	}

	gLazyBindingSaveSize = (size + 63) & ~(uint64)63;
	gLazyBindingXsaveMask[0] = mask;
	gLazyBindingXsaveMask[1] = 0;
}


/*!	Prepares the PLT of \a image for lazy binding: the jump slots keep
	pointing to their PLT entries, which push the relocation index and jump to
	PLT0, which pushes GOT[1] and jumps to GOT[2]. Returns B_BAD_DATA, having
	changed nothing, if a jump slot does not point into the image's code.
*/
static status_t
setup_lazy_plt(image_t* rootImage, image_t* image, addr_t* got,
	SymbolLookupCache* cache)
{
	Elf64_Rela* relocations = (Elf64_Rela*)image->pltrel;
	size_t count = image->pltrel_len / sizeof(Elf64_Rela);
	addr_t delta = image->regions[0].delta;

	for (size_t i = 0; i < count; i++) {
		if (ELF64_R_TYPE(relocations[i].r_info) != R_X86_64_JUMP_SLOT)
			continue;
		addr_t entry = *(addr_t*)(delta + relocations[i].r_offset);
		if (!lazy_binding_slot_target_valid(image, entry + delta))
			return B_BAD_DATA;
	}

	uint32 deferred = 0;
	for (size_t i = 0; i < count; i++) {
		if (ELF64_R_TYPE(relocations[i].r_info) == R_X86_64_JUMP_SLOT) {
			*(addr_t*)(delta + relocations[i].r_offset) += delta;
			deferred++;
			continue;
		}

		status_t status = relocate_rela(rootImage, image, &relocations[i],
			sizeof(Elf64_Rela), cache);
		if (status != B_OK)
			return status;
	}

	got[1] = (addr_t)image;
	got[2] = (addr_t)&arch_lazy_binding_entry;
	lazy_binding_note_deferred(deferred);
	return B_OK;
}


status_t
arch_relocate_image(image_t* rootImage, image_t* image,
	SymbolLookupCache* cache)
{
	status_t status;

	// No REL on x86_64.

	// Perform RELA relocations.
	if (image->rela) {
		status = relocate_rela(rootImage, image, image->rela, image->rela_len,
			cache);
		if (status != B_OK)
			return status;
	}

	// PLT relocations (they are RELA on x86_64), bound on their first call
	// if the program binds lazily.
	if (image->pltrel) {
		addr_t* got = lazy_binding_plt_got(rootImage, image);
		if (got != NULL && setup_lazy_plt(rootImage, image, got, cache)
				== B_OK) {
			return B_OK;
		}

		status = relocate_rela(rootImage, image, (Elf64_Rela*)image->pltrel,
			image->pltrel_len, cache);
		if (status != B_OK)
			return status;
	}

	return B_OK;
}
