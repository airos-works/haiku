#ifndef _TEST_HID_SUPPORT_DEFS_H
#define _TEST_HID_SUPPORT_DEFS_H
#include "../stub/SupportDefs.h"
#include <sys/types.h>
#define _PACKED __attribute__((packed))
#define B_PRIdBIGTIME PRId64
#define B_COUNT_OF(a) (sizeof(a) / sizeof(a[0]))
#endif
