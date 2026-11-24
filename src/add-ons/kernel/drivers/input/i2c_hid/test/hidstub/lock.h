#ifndef _TEST_HID_LOCK_H
#define _TEST_HID_LOCK_H
struct mutex { int locked; };
#define MUTEX_INITIALIZER(n) { 0 }
void mutex_init(mutex* m, const char* name);
void mutex_destroy(mutex* m);
void mutex_lock(mutex* m);
void mutex_unlock(mutex* m);
#endif
