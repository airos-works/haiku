// SPDX-License-Identifier: MIT
#ifndef TLS_SIZE
#define TLS_SIZE 16
#endif

static __thread volatile unsigned char sData[TLS_SIZE] = {
	[0] = 37, [TLS_SIZE - 1] = 91
};


void*
tls_address(void)
{
	return (void*)sData;
}


unsigned
tls_size(void)
{
	return TLS_SIZE;
}
