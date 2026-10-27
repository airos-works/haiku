/* SPDX-License-Identifier: MIT */
#include <stddef.h>

static __thread unsigned long sValue = 17;

void*
store_value(unsigned long value)
{
	sValue = value;
	return &sValue;
}

unsigned long
read_value(void** address)
{
	*address = &sValue;
	return sValue;
}
