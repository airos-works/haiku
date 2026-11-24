/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "MutexTests.h"

#include <string.h>

#include <lock.h>

#include "TestThread.h"


static const bigtime_t kTimedLockTestTime = 10000000;


/*!	mutex_lock_with_timeout() against mutex_unlock() (airos-works/haiku#168).

	A waiter whose timeout ran out between the holder's count and the
	holder's look at the queue left the mutex marked "released" with nobody
	to take it: free and released at once. The next thread got it the fast
	way, and the one after that took the mark: two holders.

	That needs a holder and a single waiter that gives up at the moment of
	the unlock, so the threads come in pairs with a mutex each: one locks,
	keeps the mutex for some tens of microseconds and unlocks; the other
	locks with timeouts of that order. Whenever the first one has the mutex
	it tries to lock it a second time (with a timeout, as nobody is going to
	unlock it): that must fail.
*/
class MutexTest : public StandardTestDelegate {
public:
	virtual status_t Setup(TestContext& context)
	{
		for (int i = 0; i < kPairCount; i++)
			mutex_init(&fLocks[i], "test mutex");
		return B_OK;
	}

	virtual void Cleanup(TestContext& context, bool setupOK)
	{
		for (int i = 0; i < kPairCount; i++)
			mutex_destroy(&fLocks[i]);
	}

	bool TestTimedLockAgainstUnlock(TestContext& context)
	{
		const int threadCount = 2 * kPairCount;
		thread_id threads[threadCount];

		fTestOK = true;
		fTestGo = false;
		fTwoHolders = 0;
		fLocked = 0;
		fTimedOut = 0;

		for (int i = 0; i < threadCount; i++) {
			threads[i] = SpawnThread(this, &MutexTest::_Thread, "mutex test",
				B_NORMAL_PRIORITY, (void*)(addr_t)i);
			if (threads[i] < 0) {
				fTestOK = false;
				context.Error("Failed to spawn thread: %s\n",
					strerror(threads[i]));
				break;
			}
		}

		for (int i = 0; i < threadCount; i++) {
			if (threads[i] >= 0)
				resume_thread(threads[i]);
		}

		fTestGo = true;

		for (int i = 0; i < threadCount; i++) {
			if (threads[i] >= 0)
				wait_for_thread(threads[i], NULL);
		}

		dprintf("mutex test: %" B_PRId64 " times locked, %" B_PRId64
			" timeouts of the waiters, %" B_PRId32 " times locked twice\n",
			fLocked, fTimedOut, fTwoHolders);

		TEST_ASSERT_PRINT(fTwoHolders == 0, "%" B_PRId32 " times a mutex "
			"that was locked could be locked again (%" B_PRId64
			" times locked, %" B_PRId64 " timeouts)", fTwoHolders, fLocked,
			fTimedOut);
		TEST_ASSERT(fTimedOut > 0);
		return fTestOK;
	}

private:
	void _Thread(TestContext& context, void* _index)
	{
		const int index = (addr_t)_index;
		mutex* lock = &fLocks[index / 2];
		const bool waiter = (index % 2) == 1;

		while (!fTestGo) {
		}

		bigtime_t startTime = system_time();
		uint32 round = 0;
		while (fTestOK && fTwoHolders == 0) {
			if ((++round % 64) == 0
				&& system_time() - startTime > kTimedLockTestTime) {
				break;
			}

			if (waiter) {
				// around the time the other one holds it for
				status_t status = mutex_lock_with_timeout(lock,
					B_RELATIVE_TIMEOUT, (round * 7) % 50);
				if (status == B_OK)
					mutex_unlock(lock);
				else
					atomic_add64(&fTimedOut, 1);
				continue;
			}

			if (mutex_lock(lock) != B_OK) {
				fTestOK = false;
				break;
			}
			atomic_add64(&fLocked, 1);

			// We have it: nobody gets it now, not even we (this spins and
			// waits for a few tens of microseconds, which is how long we
			// keep the mutex).
			if (mutex_lock_with_timeout(lock, B_RELATIVE_TIMEOUT,
					(round * 13) % 20) == B_OK) {
				atomic_add(&fTwoHolders, 1);
				mutex_unlock(lock);
			}

			mutex_unlock(lock);

			// leave the mutex alone for a moment
			bigtime_t end = system_time() + (round * 11) % 30;
			while (system_time() < end) {
			}
		}
	}

private:
	static const int	kPairCount = 4;

			mutex		fLocks[kPairCount];
	volatile bool		fTestGo;
	volatile bool		fTestOK;
			int32		fTwoHolders;
			int64		fLocked;
			int64		fTimedOut;
};


TestSuite*
create_mutex_test_suite()
{
	TestSuite* suite = new(std::nothrow) TestSuite("mutex");

	ADD_STANDARD_TEST(suite, MutexTest, TestTimedLockAgainstUnlock);

	return suite;
}
