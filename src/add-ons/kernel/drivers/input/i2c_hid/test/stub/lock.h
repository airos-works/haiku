#ifndef _TEST_LOCK_H
#define _TEST_LOCK_H
struct mutex { int locked; };
#define MUTEX_INITIALIZER(n) { 0 }
void mutex_lock(mutex* m);
void mutex_unlock(mutex* m);
#endif
