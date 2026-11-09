/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef SNAPSHOT_H
#define SNAPSHOT_H


#include <Bitmap.h>
#include <Locker.h>
#include <Referenceable.h>


class SnapshotPool;


/*!	A picture on its way through the program: a file that was opened, or
	what the camera saw a moment ago. The window draws it and the engine
	runs the model on it at the same time, from two threads, so nobody
	writes to the bitmap once the snapshot has left the hands of whoever
	made it, and whoever holds a reference keeps it alive.

	The source is the number the window gave to what it shows right now;
	a snapshot (or a result) with another number is from before the user
	changed the source and is dropped where it turns up.
*/
class Snapshot : public BReferenceable {
public:
								Snapshot(BBitmap* bitmap, int32 source);
	virtual						~Snapshot();

			BBitmap*			Bitmap() const
									{ return fBitmap; }
			int32				Source() const
									{ return atomic_get((int32*)&fSource); }
			void				SetSource(int32 source)
									{ atomic_set(&fSource, source); }
	//! true for a camera's frame, false for a picture that stays
			bool				IsLive() const
									{ return fPool != NULL; }

protected:
	virtual	void				LastReferenceReleased();

private:
	friend class SnapshotPool;

			BBitmap*			fBitmap;
			int32				fSource;
			SnapshotPool*	fPool;
};


typedef BReference<Snapshot> SnapshotReference;


/*!	Where the camera's frames come from and go back to, so that thirty
	bitmaps per second are not made and thrown away. */
class SnapshotPool : public BReferenceable {
public:
								SnapshotPool();
	virtual						~SnapshotPool();

	/*!	A B_RGB32 snapshot of that size with one reference, the caller's.
		NULL when there is no memory for it. */
			Snapshot*		GetFrame(int32 width, int32 height,
									int32 source);

private:
	friend class Snapshot;

			void				_Recycle(Snapshot* frame);

	static	const int32			kMaxFree = 4;

			BLocker				fLock;
			Snapshot*		fFree[kMaxFree];
			int32				fFreeCount;
};


/*!	Room for one snapshot, the newest. A producer puts them in as they
	come, a consumer takes one when it has time; what was not taken in
	between is forgotten. Put() says whether the slot was empty: then,
	and only then, the consumer has to be told. */
class SnapshotSlot {
public:
								SnapshotSlot();
								~SnapshotSlot();

			bool				Put(Snapshot* frame);
			SnapshotReference	Take();

private:
			BLocker				fLock;
			SnapshotReference	fFrame;
};


#endif	// SNAPSHOT_H
