/* Host stand-in for the kernel's <sys/systm.h>, for the SAE host test. */
#ifndef SAE_TEST_SYS_SYSTM_H
#define SAE_TEST_SYS_SYSTM_H

#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <stdlib.h>

#define __bounded__(...)
#define swap32(x) __builtin_bswap32(x)
#define swap64(x) __builtin_bswap64(x)

#endif
