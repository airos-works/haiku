/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "Snapshot.h"

#include <new>

#include <Autolock.h>


Snapshot::Snapshot(BBitmap* bitmap, int32 source)
	:
	fBitmap(bitmap),
	fSource(source),
	fPool(NULL)
{
}


Snapshot::~Snapshot()
{
	delete fBitmap;
}


void
Snapshot::LastReferenceReleased()
{
	SnapshotPool* pool = fPool;
	if (pool == NULL) {
		delete this;
		return;
	}

	// The pool keeps the frame or deletes it; either way this frame gives
	// back the reference to the pool that it held while it was out.
	pool->_Recycle(this);
	pool->ReleaseReference();
}


// #pragma mark - SnapshotPool


SnapshotPool::SnapshotPool()
	:
	fLock("frame pool"),
	fFreeCount(0)
{
}


SnapshotPool::~SnapshotPool()
{
	for (int32 i = 0; i < fFreeCount; i++)
		delete fFree[i];
}


Snapshot*
SnapshotPool::GetFrame(int32 width, int32 height, int32 source)
{
	Snapshot* frame = NULL;
	Snapshot* stale[kMaxFree];
	int32 staleCount = 0;

	BAutolock locker(fLock);
	while (fFreeCount > 0 && frame == NULL) {
		Snapshot* candidate = fFree[--fFreeCount];
		BRect bounds = candidate->Bitmap()->Bounds();
		if (bounds.IntegerWidth() + 1 == width
			&& bounds.IntegerHeight() + 1 == height) {
			frame = candidate;
		} else
			stale[staleCount++] = candidate;
	}
	locker.Unlock();

	// the camera changed its size: the old frames are of no use any more
	for (int32 i = 0; i < staleCount; i++)
		delete stale[i];

	if (frame == NULL) {
		BBitmap* bitmap = new(std::nothrow) BBitmap(
			BRect(0, 0, width - 1, height - 1), B_RGB32);
		if (bitmap == NULL || bitmap->InitCheck() != B_OK) {
			delete bitmap;
			return NULL;
		}
		frame = new(std::nothrow) Snapshot(bitmap, source);
		if (frame == NULL) {
			delete bitmap;
			return NULL;
		}
		frame->fPool = this;
	} else {
		// it came back with no reference left
		atomic_set(&frame->fReferenceCount, 1);
		frame->SetSource(source);
	}

	AcquireReference();
	return frame;
}


void
SnapshotPool::_Recycle(Snapshot* frame)
{
	BAutolock locker(fLock);
	if (fFreeCount < kMaxFree) {
		fFree[fFreeCount++] = frame;
		return;
	}
	locker.Unlock();

	delete frame;
}


// #pragma mark - SnapshotSlot


SnapshotSlot::SnapshotSlot()
	:
	fLock("frame slot")
{
}


SnapshotSlot::~SnapshotSlot()
{
}


bool
SnapshotSlot::Put(Snapshot* frame)
{
	// The frame that is replaced may be released for good here, which
	// takes the pool's lock: not under ours.
	SnapshotReference previous;

	BAutolock locker(fLock);
	previous = fFrame;
	fFrame.SetTo(frame);
	return previous.Get() == NULL;
}


SnapshotReference
SnapshotSlot::Take()
{
	BAutolock locker(fLock);
	SnapshotReference frame = fFrame;
	fFrame.Unset();
	return frame;
}
