/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */


#include <BufferIO.h>
#include <DataIO.h>

#include <TestSuiteAddon.h>
#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>

#include <stdlib.h>
#include <string.h>


// The buffer BBufferIO gets in these tests (its minimum is 512 bytes).
static const size_t kBufferSize = 1024;


class BufferIOTest : public CppUnit::TestFixture {
	CPPUNIT_TEST_SUITE(BufferIOTest);
	CPPUNIT_TEST(ReadAt_BehindEnd_ReturnsZero);
	CPPUNIT_TEST(ReadAt_AtEnd_ReturnsZero);
	CPPUNIT_TEST(ReadAt_AcrossEnd_ReturnsWhatIsThere);
	CPPUNIT_TEST(ReadAt_AfterReadBehindEnd_ReturnsData);
	CPPUNIT_TEST(ReadAt_NegativeOffset_Fails);
	CPPUNIT_TEST(WriteAt_BehindEnd_GoesToTheStream);
	CPPUNIT_TEST(WriteAt_AcrossBufferEnd_KeepsAllData);
	CPPUNIT_TEST(WriteAt_Elsewhere_KeepsPendingData);
	CPPUNIT_TEST(WriteAt_LargerThanBuffer_ReplacesCachedData);
	CPPUNIT_TEST(Seek_End_IsRelativeToTheEnd);
	CPPUNIT_TEST(SetSize_Smaller_DropsCachedData);
	CPPUNIT_TEST(RandomOperations_MatchPlainArray);
	CPPUNIT_TEST_SUITE_END();

public:
	void setUp()
	{
		fStream.SetSize(0);
		fStream.Seek(0, SEEK_SET);
		for (size_t i = 0; i < sizeof(fData); i++)
			fData[i] = (char)(i * 7 + i / 251);
	}

	// A read far behind the end of the stream copied from outside the
	// buffer: the RAW translator crashed every program that identified a
	// file with a wild TIFF directory offset.
	void ReadAt_BehindEnd_ReturnsZero()
	{
		fStream.Write(fData, 100);
		BBufferIO buffered(&fStream, kBufferSize, false);
		char out[16];
		CPPUNIT_ASSERT_EQUAL((ssize_t)16, buffered.ReadAt(0, out, 16));
		CPPUNIT_ASSERT_EQUAL((ssize_t)0,
			buffered.ReadAt(500000000, out, 2));
		CPPUNIT_ASSERT_EQUAL((ssize_t)0,
			buffered.ReadAt((off_t)1 << 40, out, 16));
	}

	void ReadAt_AtEnd_ReturnsZero()
	{
		fStream.Write(fData, 100);
		BBufferIO buffered(&fStream, kBufferSize, false);
		char out[16];
		CPPUNIT_ASSERT_EQUAL((ssize_t)0, buffered.ReadAt(100, out, 16));
		CPPUNIT_ASSERT_EQUAL((ssize_t)0, buffered.ReadAt(101, out, 16));
	}

	void ReadAt_AcrossEnd_ReturnsWhatIsThere()
	{
		fStream.Write(fData, 100);
		BBufferIO buffered(&fStream, kBufferSize, false);
		char out[16];
		CPPUNIT_ASSERT_EQUAL((ssize_t)10, buffered.ReadAt(90, out, 16));
		CPPUNIT_ASSERT(memcmp(out, fData + 90, 10) == 0);
	}

	void ReadAt_AfterReadBehindEnd_ReturnsData()
	{
		fStream.Write(fData, 100);
		BBufferIO buffered(&fStream, kBufferSize, false);
		char out[16];
		CPPUNIT_ASSERT_EQUAL((ssize_t)0, buffered.ReadAt(5000, out, 16));
		CPPUNIT_ASSERT_EQUAL((ssize_t)16, buffered.ReadAt(20, out, 16));
		CPPUNIT_ASSERT(memcmp(out, fData + 20, 16) == 0);
	}

	void ReadAt_NegativeOffset_Fails()
	{
		fStream.Write(fData, 100);
		BBufferIO buffered(&fStream, kBufferSize, false);
		char out[16];
		CPPUNIT_ASSERT(buffered.ReadAt(-1, out, 16) < 0);
		CPPUNIT_ASSERT(buffered.WriteAt(-1, out, 16) < 0);
	}

	// A write behind the end of an empty stream went to the buffer at the
	// stream's offset: outside of it.
	void WriteAt_BehindEnd_GoesToTheStream()
	{
		{
			BBufferIO buffered(&fStream, kBufferSize, false);
			CPPUNIT_ASSERT_EQUAL((ssize_t)32,
				buffered.WriteAt(100000, fData, 32));
		}
		CPPUNIT_ASSERT_EQUAL((size_t)100032, fStream.BufferLength());
		CPPUNIT_ASSERT(memcmp((const char*)fStream.Buffer() + 100000, fData,
			32) == 0);
	}

	// A write that started within the cached data and ended behind the
	// buffer went on behind the buffer.
	void WriteAt_AcrossBufferEnd_KeepsAllData()
	{
		fStream.Write(fData, 4096);
		{
			BBufferIO buffered(&fStream, kBufferSize, false);
			char out[16];
			CPPUNIT_ASSERT_EQUAL((ssize_t)16, buffered.ReadAt(0, out, 16));
				// the buffer holds 0 to 1024 now
			CPPUNIT_ASSERT_EQUAL((ssize_t)100,
				buffered.WriteAt(1000, fData + 5000, 100));
		}
		CPPUNIT_ASSERT_EQUAL((size_t)4096, fStream.BufferLength());
		const char* stream = (const char*)fStream.Buffer();
		CPPUNIT_ASSERT(memcmp(stream, fData, 1000) == 0);
		CPPUNIT_ASSERT(memcmp(stream + 1000, fData + 5000, 100) == 0);
		CPPUNIT_ASSERT(memcmp(stream + 1100, fData + 1100, 4096 - 1100) == 0);
	}

	// A write outside the cached data replaced the buffer without writing
	// what was pending in it.
	void WriteAt_Elsewhere_KeepsPendingData()
	{
		fStream.Write(fData, 8192);
		{
			BBufferIO buffered(&fStream, kBufferSize, false);
			CPPUNIT_ASSERT_EQUAL((ssize_t)10,
				buffered.WriteAt(10, fData + 9000, 10));
			CPPUNIT_ASSERT_EQUAL((ssize_t)10,
				buffered.WriteAt(6000, fData + 9100, 10));
			char out[10];
			CPPUNIT_ASSERT_EQUAL((ssize_t)10, buffered.ReadAt(10, out, 10));
			CPPUNIT_ASSERT(memcmp(out, fData + 9000, 10) == 0);
		}
		const char* stream = (const char*)fStream.Buffer();
		CPPUNIT_ASSERT(memcmp(stream + 10, fData + 9000, 10) == 0);
		CPPUNIT_ASSERT(memcmp(stream + 6000, fData + 9100, 10) == 0);
		CPPUNIT_ASSERT(memcmp(stream + 20, fData + 20, 6000 - 20) == 0);
	}

	// A write too large for the buffer went straight to the stream: before
	// older writes that were still pending, and reads kept returning the
	// cached data it had replaced.
	void WriteAt_LargerThanBuffer_ReplacesCachedData()
	{
		fStream.Write(fData, 8192);
		BBufferIO buffered(&fStream, kBufferSize, false);
		CPPUNIT_ASSERT_EQUAL((ssize_t)10,
			buffered.WriteAt(10, fData + 9000, 10));
		CPPUNIT_ASSERT_EQUAL((ssize_t)4000,
			buffered.WriteAt(0, fData + 10000, 4000));
		char out[20];
		CPPUNIT_ASSERT_EQUAL((ssize_t)20, buffered.ReadAt(0, out, 20));
		CPPUNIT_ASSERT(memcmp(out, fData + 10000, 20) == 0);
		CPPUNIT_ASSERT_EQUAL(B_OK, buffered.Flush());
		CPPUNIT_ASSERT(memcmp(fStream.Buffer(), fData + 10000, 4000) == 0);
	}

	void Seek_End_IsRelativeToTheEnd()
	{
		fStream.Write(fData, 100);
		BBufferIO buffered(&fStream, kBufferSize, false);
		CPPUNIT_ASSERT_EQUAL((off_t)100, buffered.Seek(0, SEEK_END));
		CPPUNIT_ASSERT_EQUAL((off_t)90, buffered.Seek(-10, SEEK_END));
		char out[16];
		CPPUNIT_ASSERT_EQUAL((ssize_t)10, buffered.Read(out, 16));
		CPPUNIT_ASSERT(memcmp(out, fData + 90, 10) == 0);
		CPPUNIT_ASSERT(buffered.Seek(-101, SEEK_END) < 0);

		// data that is not in the stream yet counts
		CPPUNIT_ASSERT_EQUAL((ssize_t)20, buffered.WriteAt(100, fData, 20));
		CPPUNIT_ASSERT_EQUAL((off_t)120, buffered.Seek(0, SEEK_END));
	}

	void SetSize_Smaller_DropsCachedData()
	{
		fStream.Write(fData, 600);
		BBufferIO buffered(&fStream, kBufferSize, false);
		char out[16];
		CPPUNIT_ASSERT_EQUAL((ssize_t)16, buffered.ReadAt(0, out, 16));
		CPPUNIT_ASSERT_EQUAL((ssize_t)10,
			buffered.WriteAt(590, fData + 9000, 10));
		CPPUNIT_ASSERT_EQUAL(B_OK, buffered.SetSize(300));
		CPPUNIT_ASSERT_EQUAL((ssize_t)0, buffered.ReadAt(300, out, 16));
		CPPUNIT_ASSERT_EQUAL((ssize_t)6, buffered.ReadAt(294, out, 16));
		CPPUNIT_ASSERT_EQUAL(B_OK, buffered.Flush());
		CPPUNIT_ASSERT_EQUAL((size_t)300, fStream.BufferLength());
		CPPUNIT_ASSERT(memcmp(fStream.Buffer(), fData, 300) == 0);
	}

	// Reads, writes and size changes at random places, small and large,
	// against the same operations on a plain array.
	void RandomOperations_MatchPlainArray()
	{
		static const size_t kMaxSize = 16384;
		char* model = (char*)malloc(kMaxSize);
		char* out = (char*)malloc(kMaxSize);
		CPPUNIT_ASSERT(model != NULL && out != NULL);
		size_t modelSize = 0;
		uint32 seed = 12345;

		BBufferIO* buffered = new BBufferIO(&fStream, kBufferSize, false);
		for (int round = 0; round < 200000; round++) {
			uint32 random = _Random(seed);
			uint32 operation = random % 100;
			size_t pos = _Random(seed) % (kMaxSize - 4096);
			// most operations fit the buffer, some don't
			size_t size = _Random(seed) % (random % 16 == 0 ? 4096 : 300);

			if (operation < 45) {
				ssize_t expected = pos >= modelSize
					? 0 : (ssize_t)min_c(size, modelSize - pos);
				// a read behind the end must not touch the array
				memset(out, 0x5a, size + 1);
				ssize_t read = buffered->ReadAt(pos, out, size);
				CPPUNIT_ASSERT_EQUAL(expected, read);
				CPPUNIT_ASSERT(memcmp(out, model + pos, expected) == 0);
				CPPUNIT_ASSERT_EQUAL((char)0x5a, out[size]);
			} else if (operation < 90) {
				// no holes: BMallocIO doesn't clear them
				pos = min_c(pos, modelSize);
				const char* data = fData + _Random(seed) % 8192;
				CPPUNIT_ASSERT_EQUAL((ssize_t)size,
					buffered->WriteAt(pos, data, size));
				if (size > 0) {
					memcpy(model + pos, data, size);
					modelSize = max_c(modelSize, pos + size);
				}
			} else if (operation < 94) {
				pos = min_c(pos, modelSize);
				CPPUNIT_ASSERT_EQUAL(B_OK, buffered->SetSize(pos));
				modelSize = pos;
			} else if (operation < 97) {
				CPPUNIT_ASSERT_EQUAL((off_t)modelSize,
					buffered->Seek(0, SEEK_END));
			} else if (operation < 99) {
				CPPUNIT_ASSERT_EQUAL(B_OK, buffered->Flush());
				_AssertStreamIs(model, modelSize);
			} else {
				// a new buffer over the same stream
				delete buffered;
				_AssertStreamIs(model, modelSize);
				buffered = new BBufferIO(&fStream, kBufferSize, false);
			}
		}

		delete buffered;
		_AssertStreamIs(model, modelSize);
		free(model);
		free(out);
	}

private:
	static uint32 _Random(uint32& seed)
	{
		seed = seed * 1103515245 + 12345;
		return (seed >> 8) & 0xffffff;
	}

	void _AssertStreamIs(const char* model, size_t modelSize)
	{
		CPPUNIT_ASSERT_EQUAL(modelSize, fStream.BufferLength());
		CPPUNIT_ASSERT(memcmp(fStream.Buffer(), model, modelSize) == 0);
	}

private:
	BMallocIO fStream;
	char fData[16384 + 4096];
};


CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(BufferIOTest, getTestSuiteName());
