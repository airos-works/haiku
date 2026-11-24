/*
 * Copyright 2009, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Distributed under the terms of the MIT License.
 */


#include "RWLockTests.h"

#include <string.h>

#include <lock.h>

#include "TestThread.h"


static const int kConcurrentTestTime = 2000000;


class RWLockTest : public StandardTestDelegate {
public:
	RWLockTest()
	{
	}

	virtual status_t Setup(TestContext& context)
	{
		rw_lock_init(&fLock, "test r/w lock");
		return B_OK;
	}

	virtual void Cleanup(TestContext& context, bool setupOK)
	{
		rw_lock_destroy(&fLock);
	}


	bool TestSimple(TestContext& context)
	{
		for (int32 i = 0; i < 3; i++) {
			TEST_ASSERT(rw_lock_read_lock(&fLock) == B_OK);
			rw_lock_read_unlock(&fLock);

			TEST_ASSERT(rw_lock_write_lock(&fLock) == B_OK);
			rw_lock_write_unlock(&fLock);
		}

		return true;
	}

	bool TestNestedWrite(TestContext& context)
	{
		for (int32 i = 0; i < 10; i++)
			TEST_ASSERT(rw_lock_write_lock(&fLock) == B_OK);

		for (int32 i = 0; i < 10; i++)
			rw_lock_write_unlock(&fLock);

		return true;
	}

	bool TestNestedWriteRead(TestContext& context)
	{
		TEST_ASSERT(rw_lock_write_lock(&fLock) == B_OK);

		for (int32 i = 0; i < 10; i++)
			TEST_ASSERT(rw_lock_read_lock(&fLock) == B_OK);

		for (int32 i = 0; i < 10; i++)
			rw_lock_read_unlock(&fLock);

		rw_lock_write_unlock(&fLock);

		return true;
	}

	bool TestDegrade(TestContext& context)
	{
		TEST_ASSERT(rw_lock_write_lock(&fLock) == B_OK);
		TEST_ASSERT(rw_lock_read_lock(&fLock) == B_OK);
		rw_lock_write_unlock(&fLock);
		rw_lock_read_unlock(&fLock);

		return true;
	}

	bool TestConcurrentWriteRead(TestContext& context)
	{
		return _RunConcurrentTest(context,
			&RWLockTest::TestConcurrentWriteReadThread);
	}

	bool TestConcurrentWriteNestedRead(TestContext& context)
	{
		return _RunConcurrentTest(context,
			&RWLockTest::TestConcurrentWriteNestedReadThread);
	}

	bool TestConcurrentDegrade(TestContext& context)
	{
		return _RunConcurrentTest(context,
			&RWLockTest::TestConcurrentDegradeThread);
	}


	/*!	Waiters of a lock that is destroyed must come back with an error
		(airos-works/haiku#168: they blocked again, for good). The lock is a
		separate one: the waiters that do not come back stay with it.
	*/
	bool TestDestroyWithWaiters(TestContext& context)
	{
		rw_lock* lock = new(std::nothrow) rw_lock;
		TEST_ASSERT(lock != NULL);
		rw_lock_init(lock, "test r/w lock to destroy");
		TEST_ASSERT(rw_lock_write_lock(lock) == B_OK);

		fDestroyLock = lock;
		fDestroyResults[0] = fDestroyResults[1] = 1;

		thread_id threads[2];
		for (int i = 0; i < 2; i++) {
			threads[i] = SpawnThread(this, &RWLockTest::DestroyWaiterThread,
				"rw lock destroy test", B_NORMAL_PRIORITY, (void*)(addr_t)i);
			TEST_ASSERT(threads[i] >= 0);
			resume_thread(threads[i]);
		}

		// let them block
		snooze(200000);

		rw_lock_destroy(lock);

		bool returned = true;
		for (int i = 0; i < 2; i++) {
			status_t status = wait_for_thread_etc(threads[i],
				B_RELATIVE_TIMEOUT, 3000000, NULL);
			if (status == B_BAD_THREAD_ID) {
				// gone already: a kernel thread leaves no entry to wait for
				status = B_OK;
			}
			if (status != B_OK) {
				context.Error("the %s of the destroyed lock did not return "
					"(%s)\n", i == 0 ? "reader" : "writer", strerror(status));
				returned = false;
			}
		}
		if (!returned) {
			// they sit on the lock: it cannot be freed
			return false;
		}
		delete lock;

		TEST_ASSERT_PRINT(fDestroyResults[0] == B_ERROR,
			"reader: %#" B_PRIx32, fDestroyResults[0]);
		TEST_ASSERT_PRINT(fDestroyResults[1] == B_ERROR,
			"writer: %#" B_PRIx32, fDestroyResults[1]);
		return true;
	}

	void DestroyWaiterThread(TestContext& context, void* _index)
	{
		int index = (addr_t)_index;
		fDestroyResults[index] = index == 0
			? rw_lock_read_lock(fDestroyLock)
			: rw_lock_write_lock(fDestroyLock);
	}


	// thread function wrappers

	void TestConcurrentWriteReadThread(TestContext& context, void* _index)
	{
		if (!_TestConcurrentWriteReadThread(context, (addr_t)_index))
			fTestOK = false;
	}

	void TestConcurrentWriteNestedReadThread(TestContext& context, void* _index)
	{
		if (!_TestConcurrentWriteNestedReadThread(context, (addr_t)_index))
			fTestOK = false;
	}

	void TestConcurrentDegradeThread(TestContext& context, void* _index)
	{
		if (!_TestConcurrentDegradeThread(context, (addr_t)_index))
			fTestOK = false;
	}

private:
	bool _RunConcurrentTest(TestContext& context,
		void (RWLockTest::*method)(TestContext&, void*))
	{
		const int threadCount = 8;
		thread_id threads[threadCount];
		for (int i = 0; i < threadCount; i++)
			threads[i] = -1;

		fTestOK = true;
		fTestGo = false;
		fLockCount = 0;

		for (int i = 0; i < threadCount; i++) {
			threads[i] = SpawnThread(this, method, "rw lock test",
				B_NORMAL_PRIORITY, (void*)(addr_t)i);
			if (threads[i] < 0) {
				fTestOK = false;
				context.Error("Failed to spawn thread: %s\n",
					strerror(threads[i]));
				break;
			}
		}

		for (int i = 0; i < threadCount; i++)
			resume_thread(threads[i]);

		fTestGo = true;

		for (int i = 0; i < threadCount; i++) {
			if (threads[i] >= 0)
				wait_for_thread(threads[i], NULL);
		}

		return fTestOK;
	}

	bool _TestConcurrentWriteReadThread(TestContext& context, int32 threadIndex)
	{
		if (!fTestOK)
			return false;

		int bitShift = 8 * threadIndex;

		while (!fTestGo) {
		}

		bigtime_t startTime = system_time();
		uint64 iteration = 0;
		do {
			for (int k = 0; fTestOK && k < 255; k++) {
				TEST_ASSERT(rw_lock_read_lock(&fLock) == B_OK);
				uint64 count = fLockCount;
				rw_lock_read_unlock(&fLock);

				TEST_ASSERT(rw_lock_write_lock(&fLock) == B_OK);
				fLockCount += (uint64)1 << bitShift;
				rw_lock_write_unlock(&fLock);

				int value = (count >> bitShift) & 0xff;
				TEST_ASSERT_PRINT(value == k,
					"thread index: %" B_PRId32 ", iteration: %" B_PRId32
					", value: %d vs %d, count: %#" B_PRIx64, threadIndex,
					iteration, value, k, count);
			}

			TEST_ASSERT(rw_lock_write_lock(&fLock) == B_OK);
			fLockCount -= (uint64)255 << bitShift;
			rw_lock_write_unlock(&fLock);

			iteration++;
		} while (fTestOK && system_time() - startTime < kConcurrentTestTime);

		return true;
	}

	bool _TestConcurrentWriteNestedReadThread(TestContext& context,
		int32 threadIndex)
	{
		if (!fTestOK)
			return false;

		int bitShift = 8 * threadIndex;

		while (!fTestGo) {
		}

		bigtime_t startTime = system_time();
		uint64 iteration = 0;
		do {
			for (int k = 0; fTestOK && k < 255; k++) {
				TEST_ASSERT(rw_lock_write_lock(&fLock) == B_OK);

				TEST_ASSERT(rw_lock_read_lock(&fLock) == B_OK);
				uint64 count = fLockCount;
				rw_lock_read_unlock(&fLock);

				fLockCount += (uint64)1 << bitShift;

				rw_lock_write_unlock(&fLock);

				int value = (count >> bitShift) & 0xff;
				TEST_ASSERT_PRINT(value == k,
					"thread index: %" B_PRId32 ", iteration: %" B_PRId32
					", value: %d vs %d, count: %#" B_PRIx64, threadIndex,
					iteration, value, k, count);
			}

			TEST_ASSERT(rw_lock_write_lock(&fLock) == B_OK);
			fLockCount -= (uint64)255 << bitShift;
			rw_lock_write_unlock(&fLock);

			iteration++;
		} while (fTestOK && system_time() - startTime < kConcurrentTestTime);

		return true;
	}

	bool _TestConcurrentDegradeThread(TestContext& context, int32 threadIndex)
	{
		if (!fTestOK)
			return false;

		int bitShift = 8 * threadIndex;

		while (!fTestGo) {
		}

		bigtime_t startTime = system_time();
		uint64 iteration = 0;
		do {
			for (int k = 0; fTestOK && k < 255; k++) {
				TEST_ASSERT(rw_lock_read_lock(&fLock) == B_OK);
				uint64 count = fLockCount;
				rw_lock_read_unlock(&fLock);

				TEST_ASSERT(rw_lock_write_lock(&fLock) == B_OK);
				fLockCount += (uint64)1 << bitShift;
				uint64 newCount = fLockCount;

				// degrade
				TEST_ASSERT(rw_lock_read_lock(&fLock) == B_OK);
				rw_lock_write_unlock(&fLock);

				uint64 unchangedCount = fLockCount;

				rw_lock_read_unlock(&fLock);

				int value = (count >> bitShift) & 0xff;
				TEST_ASSERT_PRINT(value == k,
					"thread index: %" B_PRId32 ", iteration: %" B_PRId32
					", value: %d vs %d, count: %#" B_PRIx64, threadIndex,
					iteration, value, k, count);
				TEST_ASSERT(newCount == unchangedCount);
			}

			TEST_ASSERT(rw_lock_write_lock(&fLock) == B_OK);
			fLockCount -= (uint64)255 << bitShift;
			rw_lock_write_unlock(&fLock);

			iteration++;
		} while (fTestOK && system_time() - startTime < kConcurrentTestTime);

		return true;
	}

private:
			rw_lock		fLock;
	volatile bool		fTestGo;
	volatile uint64		fLockCount;
	volatile bool		fTestOK;
			rw_lock*	fDestroyLock;
	volatile status_t	fDestroyResults[2];
};


TestSuite*
create_rw_lock_test_suite()
{
	TestSuite* suite = new(std::nothrow) TestSuite("rw_lock");

	ADD_STANDARD_TEST(suite, RWLockTest, TestSimple);
	ADD_STANDARD_TEST(suite, RWLockTest, TestNestedWrite);
	ADD_STANDARD_TEST(suite, RWLockTest, TestNestedWriteRead);
	ADD_STANDARD_TEST(suite, RWLockTest, TestDegrade);
	ADD_STANDARD_TEST(suite, RWLockTest, TestConcurrentWriteRead);
	ADD_STANDARD_TEST(suite, RWLockTest, TestConcurrentWriteNestedRead);
	ADD_STANDARD_TEST(suite, RWLockTest, TestConcurrentDegrade);
	ADD_STANDARD_TEST(suite, RWLockTest, TestDestroyWithWaiters);

	return suite;
}
