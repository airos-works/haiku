/* nobody waits in the resume test: it runs in one thread */
#ifndef _TEST_HID_CONDITION_VARIABLE_H
#define _TEST_HID_CONDITION_VARIABLE_H
#include <OS.h>
struct ConditionVariableEntry {
	status_t Wait(uint32 flags = 0, bigtime_t timeout = 0)
		{ return B_TIMED_OUT; }
};
struct ConditionVariable {
	void Init(const void* object, const char* type) {}
	void Add(ConditionVariableEntry* entry) {}
	int32 NotifyAll(status_t result = B_OK) { return 0; }
	int32 NotifyOne(status_t result = B_OK) { return 0; }
	int32 EntriesCount() { return 0; }
};
#endif
