/*
 * Copyright 2010, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Distributed under the terms of the MIT License.
 */


#include "paging/X86PagingStructures.h"


static int64 sNextPCIDID = 0;


X86PagingStructures::X86PagingStructures()
	:
	ref_count(1),
	pcid_id(atomic_add64(&sNextPCIDID, 1) + 1),
	tlb_generation(0)
{
}


X86PagingStructures::~X86PagingStructures()
{
}
