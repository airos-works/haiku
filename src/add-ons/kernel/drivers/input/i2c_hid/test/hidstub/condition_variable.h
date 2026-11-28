/* one thread: a wait returns at once, with what a notification since the
   waiter was added left it */
#ifndef _TEST_HID_CONDITION_VARIABLE_H
#define _TEST_HID_CONDITION_VARIABLE_H
#include <OS.h>
struct ConditionVariable;
struct ConditionVariableEntry {
	ConditionVariable*	variable = NULL;
	bool				notified = false;
	inline status_t Wait(uint32 flags = 0, bigtime_t timeout = 0);
};
struct ConditionVariable {
	ConditionVariableEntry* entry = NULL;
	void Init(const void* object, const char* type) {}
	void Add(ConditionVariableEntry* waiter)
		{ entry = waiter; waiter->variable = this; waiter->notified = false; }
	int32 NotifyAll(status_t result = B_OK)
	{
		if (entry == NULL)
			return 0;
		entry->notified = true;
		entry = NULL;
		return 1;
	}
	int32 NotifyOne(status_t result = B_OK) { return NotifyAll(result); }
	int32 EntriesCount() { return entry != NULL ? 1 : 0; }
};
inline status_t
ConditionVariableEntry::Wait(uint32 flags, bigtime_t timeout)
{
	if (variable != NULL && variable->entry == this)
		variable->entry = NULL;
	variable = NULL;
	return notified ? B_OK : B_TIMED_OUT;
}
#endif
