#ifndef _TEST_HID_KERNEL_H
#define _TEST_HID_KERNEL_H
#include <OS.h>
#define IS_USER_ADDRESS(x) (true)
static inline status_t
user_memcpy(void* to, const void* from, size_t size)
{
	memcpy(to, from, size);
	return B_OK;
}
#endif
