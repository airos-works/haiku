#ifndef _TEST_HID_OS_H
#define _TEST_HID_OS_H
#include <SupportDefs.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
typedef int32 thread_id;
typedef int64 off_t64;
typedef status_t (*thread_func)(void*);
#define B_REAL_TIME_DISPLAY_PRIORITY 100
#define B_DEV_NAME_LENGTH 128
#define B_FILE_NAME_LENGTH 256
enum { B_INTERRUPTED = -2147483638, B_DEV_NOT_READY = -2147479535,
	B_BUFFER_OVERFLOW = -2147454955, B_BAD_ADDRESS = -2147478783,
	B_FILE_ERROR = -2147459074, B_NO_INIT = -2147483635,
	B_IO_ERROR = -2147483647 };
#ifndef ENODEV
#	define ENODEV (-2147454969)
#endif
extern "C" {
thread_id spawn_kernel_thread(thread_func function, const char* name,
	int32 priority, void* data);
status_t resume_thread(thread_id thread);
status_t wait_for_thread(thread_id thread, status_t* _result);
status_t snooze(bigtime_t time);
bigtime_t system_time(void);
int32 atomic_add(int32* value, int32 addValue);
int32 atomic_get(int32* value);
}
#endif
