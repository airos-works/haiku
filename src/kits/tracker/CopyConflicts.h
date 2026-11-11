/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _TRACKER_COPY_CONFLICTS_H
#define _TRACKER_COPY_CONFLICTS_H


#include <Directory.h>
#include <Entry.h>
#include <ObjectList.h>
#include <String.h>

#include <map>
#include <string>


namespace BPrivate {


// Before a copy or move, every item that meets one of the same name in the
// destination (also inside folders that are merged) is listed, and the user
// decides for each: replace it, skip it, or keep both.

enum CopyConflictDecision {
	kConflictReplace = 0,
	kConflictSkip,
	kConflictKeepBoth
};


struct CopyConflict {
	entry_ref	source;
	BString		existingPath;
	BString		displayName;
		// relative to the destination folder, as "Photos/a.jpg"
	off_t		sourceSize;
	off_t		existingSize;
	time_t		sourceTime;
	time_t		existingTime;
	int32		decision;
};


class CopyConflicts {
public:
								CopyConflicts();

			// Looks for clashes of the items in srcList in destDir; true
			// when there are some
			bool				Find(const BObjectList<entry_ref, true>* srcList,
									const BDirectory& destDir);

			int32				CountConflicts() const
									{ return fConflicts.CountItems(); }
			CopyConflict*		ConflictAt(int32 index) const
									{ return fConflicts.ItemAt(index); }

			// Shows the list and waits for the user; false if canceled
			bool				Ask(bool moving, const char* destName);

			// The decision for an item named name in dir, or -1 when it was
			// not listed
			int32				DecisionFor(const BDirectory& dir,
									const char* name) const;

			bool				Truncated() const { return fTruncated; }

private:
			void				_FindIn(const BDirectory& sourceDir,
									const BDirectory& destDir,
									const BString& prefix, int32 depth);
			void				_Add(const BEntry& source,
									const BEntry& existing,
									const BString& displayName);

			BObjectList<CopyConflict, true> fConflicts;
			std::map<std::string, int32> fByPath;
			bool				fTruncated;
};


// A name for a kept copy that is not taken in dir: "photo 2.jpg"
void MakeKeepBothName(char* name, const BDirectory* dir);


} // namespace BPrivate

using namespace BPrivate;


#endif	// _TRACKER_COPY_CONFLICTS_H
