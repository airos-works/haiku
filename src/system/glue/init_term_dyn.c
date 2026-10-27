/*
 * Copyright 2019, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include "haiku_version_glue.c"


#if defined(__aarch64__)

#include <sys/auxv.h>


/* GCC's outline atomics, which every atomic operation calls, pick between
   LSE instructions and LL/SC loops by this image's own copy of a libgcc flag,
   and only libgcc's Linux build sets it from the CPU's features. Every image
   links this glue, so set it here. getauxval() is weak: with a libroot
   without it, the LL/SC loops stay. */
extern unsigned long getauxval(unsigned long type) __attribute__((weak));
extern _Bool __aarch64_have_lse_atomics __attribute__((visibility("hidden")));


static void __attribute__((constructor(90)))
init_lse_atomics(void)
{
	if (getauxval != 0 && (getauxval(AT_HWCAP) & HWCAP_ATOMICS) != 0)
		__aarch64_have_lse_atomics = 1;
}

#endif
