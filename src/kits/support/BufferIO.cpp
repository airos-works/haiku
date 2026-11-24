/*
 *	Copyright 2001-2008 Haiku, Inc. All rights reserved.
 *	Distributed under the terms of the MIT license
 *
 *	Authors:
 *		Stefano Ceccherini, burton666@libero.it
 */


#include <BufferIO.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


BBufferIO::BBufferIO(BPositionIO* stream, size_t bufferSize, bool ownsStream)
	:
	fBufferStart(0),
	fStream(stream),
	fBuffer(NULL),
	fBufferUsed(0),
	fBufferIsDirty(false),
	fOwnsStream(ownsStream)

{
	fBufferSize = max_c(bufferSize, 512);
	fPosition = stream->Position();

	// What can we do if this malloc fails ?
	// I think R5 uses new, but doesn't catch the thrown exception
	// (if you specify a very big buffer, the application just
	// terminates with abort).
	fBuffer = (char*)malloc(fBufferSize);
}


BBufferIO::~BBufferIO()
{
	if (fBufferIsDirty) {
		// Write pending changes to the stream
		Flush();
	}

	free(fBuffer);

	if (fOwnsStream)
		delete fStream;
}


ssize_t
BBufferIO::ReadAt(off_t pos, void* buffer, size_t size)
{
	// We refuse to crash, even if
	// you were lazy and didn't give a valid
	// stream on construction.
	if (fStream == NULL)
		return B_NO_INIT;
	if (buffer == NULL || pos < 0)
		return B_BAD_VALUE;

	// If the amount of data we want doesn't fit in the buffer, just
	// read it directly from the disk (and don't touch the buffer).
	if (size > fBufferSize || fBuffer == NULL) {
		status_t status = Flush();
		if (status != B_OK)
			return status;
		return fStream->ReadAt(pos, buffer, size);
	}

	// If the data we are looking for is not in the buffer...
	if (pos < fBufferStart
		|| (uint64)(pos - fBufferStart) + size > fBufferUsed) {
		// If there are pending writes, do them.
		status_t status = Flush();
		if (status != B_OK)
			return status;

		// ...cache as much as we can from the stream. Whatever comes of it,
		// the old window is gone: the buffer starts at this offset, and a
		// read at or behind the end of the stream leaves it empty.
		fBufferStart = pos;
		fBufferUsed = 0;

		ssize_t sizeRead = fStream->ReadAt(pos, fBuffer, fBufferSize);
		if (sizeRead < 0)
			return sizeRead;

		fBufferUsed = min_c((size_t)sizeRead, fBufferSize);
	}

	// position in buffer: at most fBufferUsed, see above
	size_t offset = (size_t)(pos - fBufferStart);
	size = min_c(size, fBufferUsed - offset);

	// copy data from the cache to the given buffer
	memcpy(buffer, fBuffer + offset, size);

	return size;
}


ssize_t
BBufferIO::WriteAt(off_t pos, const void* buffer, size_t size)
{
	if (fStream == NULL)
		return B_NO_INIT;
	if (buffer == NULL || pos < 0)
		return B_BAD_VALUE;

	// If data doesn't fit into the buffer, write it directly to the stream:
	// after what is pending, which is older, and the cached data it replaces
	// is dropped.
	if (size > fBufferSize || fBuffer == NULL) {
		status_t status = Flush();
		if (status != B_OK)
			return status;

		fBufferUsed = 0;
		return fStream->WriteAt(pos, buffer, size);
	}

	// The data goes into the buffer if it starts within the cached data or
	// right behind it, and ends within the buffer. Otherwise the buffer
	// starts over at this offset (writes of a file from start to end never
	// need the stream's old contents).
	if (pos < fBufferStart
		|| (uint64)(pos - fBufferStart) > fBufferUsed
		|| (uint64)(pos - fBufferStart) + size > fBufferSize) {
		status_t status = Flush();
		if (status != B_OK)
			return status;

		fBufferStart = pos;
		fBufferUsed = 0;
	}

	if (size == 0)
		return 0;

	// position in buffer
	size_t offset = (size_t)(pos - fBufferStart);
	memcpy(fBuffer + offset, buffer, size);

	fBufferIsDirty = true;
	fBufferUsed = max_c(offset + size, fBufferUsed);

	return size;
}


off_t
BBufferIO::Seek(off_t position, uint32 seekMode)
{
	if (fStream == NULL)
		return B_NO_INIT;

	off_t newPosition = fPosition;

	switch (seekMode) {
		case SEEK_CUR:
			newPosition += position;
			break;
		case SEEK_SET:
			newPosition = position;
			break;
		case SEEK_END:
		{
			off_t size;
			status_t status = fStream->GetSize(&size);
			if (status != B_OK)
				return status;

			// data that is still to be written may reach behind the
			// stream's end
			if (fBufferIsDirty)
				size = max_c(size, fBufferStart + (off_t)fBufferUsed);

			newPosition = size + position;
			break;
		}
		default:
			return B_BAD_VALUE;
	}

	if (newPosition < 0)
		return B_BAD_VALUE;

	fPosition = newPosition;
	return newPosition;
}


off_t
BBufferIO::Position() const
{
	return fPosition;
}


status_t
BBufferIO::SetSize(off_t size)
{
	if (fStream == NULL)
		return B_NO_INIT;
	if (size < 0)
		return B_BAD_VALUE;

	status_t status = fStream->SetSize(size);
	if (status != B_OK)
		return status;

	// What is cached behind the new end is dropped, pending writes too:
	// they would make the stream grow again.
	if (size <= fBufferStart) {
		fBufferUsed = 0;
		fBufferIsDirty = false;
	} else if ((uint64)(size - fBufferStart) < fBufferUsed)
		fBufferUsed = (size_t)(size - fBufferStart);

	return B_OK;
}


status_t
BBufferIO::Flush()
{
	if (!fBufferIsDirty)
		return B_OK;

	// Write the cached data to the stream
	size_t written = 0;
	while (written < fBufferUsed) {
		ssize_t bytesWritten = fStream->WriteAt(fBufferStart + written,
			fBuffer + written, fBufferUsed - written);
		if (bytesWritten < 0)
			return bytesWritten;
		if (bytesWritten == 0)
			return B_IO_ERROR;

		written += bytesWritten;
	}

	fBufferIsDirty = false;
	return B_OK;
}


BPositionIO*
BBufferIO::Stream() const
{
	return fStream;
}


size_t
BBufferIO::BufferSize() const
{
	return fBufferSize;
}


bool
BBufferIO::OwnsStream() const
{
	return fOwnsStream;
}


void
BBufferIO::SetOwnsStream(bool ownsStream)
{
	fOwnsStream = ownsStream;
}


void
BBufferIO::PrintToStream() const
{
	printf("stream %p\n", fStream);
	printf("buffer %p\n", fBuffer);
	printf("start  %" B_PRId64 "\n", fBufferStart);
	printf("used   %ld\n", fBufferUsed);
	printf("phys   %ld\n", fBufferSize);
	printf("dirty  %s\n", (fBufferIsDirty) ? "true" : "false");
	printf("owns   %s\n", (fOwnsStream) ? "true" : "false");
}


//	#pragma mark - FBC padding


// These functions are here to maintain future binary
// compatibility.
status_t BBufferIO::_Reserved_BufferIO_0(void*) { return B_ERROR; }
status_t BBufferIO::_Reserved_BufferIO_1(void*) { return B_ERROR; }
status_t BBufferIO::_Reserved_BufferIO_2(void*) { return B_ERROR; }
status_t BBufferIO::_Reserved_BufferIO_3(void*) { return B_ERROR; }
status_t BBufferIO::_Reserved_BufferIO_4(void*) { return B_ERROR; }
