#ifndef _TEST_HID_AUTOLOCK_H
#define _TEST_HID_AUTOLOCK_H
#include <lock.h>
struct MutexLocker {
	mutex* m; bool held;
	MutexLocker(mutex& x) : m(&x), held(true) { mutex_lock(m); }
	~MutexLocker() { if (held) mutex_unlock(m); }
	void Unlock() { if (held) { mutex_unlock(m); held = false; } }
};
#endif
