#ifndef _TEST_AUTOLOCK_H
#define _TEST_AUTOLOCK_H
#include <KernelExport.h>
#include <lock.h>
struct MutexLocker {
	mutex* m; bool held;
	MutexLocker(mutex& x) : m(&x), held(true) { mutex_lock(m); }
	~MutexLocker() { if (held) mutex_unlock(m); }
	void Unlock() { if (held) { mutex_unlock(m); held = false; } }
};
void test_spin_lock(spinlock* s);
void test_spin_unlock(spinlock* s);
struct SpinLocker {
	spinlock* s; bool held;
	SpinLocker(spinlock& x) : s(&x), held(true) { test_spin_lock(s); }
	~SpinLocker() { if (held) test_spin_unlock(s); }
	void Unlock() { if (held) { test_spin_unlock(s); held = false; } }
};
typedef SpinLocker InterruptsSpinLocker;
#endif
