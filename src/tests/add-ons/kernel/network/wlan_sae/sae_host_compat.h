/* Host build of net80211/ieee80211_sae.c for the test vectors. */
#ifndef SAE_HOST_COMPAT_H
#define SAE_HOST_COMPAT_H

#include <sys/types.h>
#include <sys/param.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <sys/systm.h>
#include <crypto/md5.h>
#include <crypto/sha1.h>
#include <crypto/sha2.h>
#include <crypto/hmac.h>

static inline int
timingsafe_bcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *p = a, *q = b;
	unsigned char d = 0;

	while (n-- > 0)
		d |= *p++ ^ *q++;
	return d != 0;
}

#endif
