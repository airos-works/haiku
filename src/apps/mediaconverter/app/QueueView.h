/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef QUEUE_VIEW_H
#define QUEUE_VIEW_H


#include <Entry.h>
#include <ListItem.h>
#include <ListView.h>
#include <String.h>

#include "SourceInfo.h"
#include "Transcoder.h"


class BBitmap;


enum job_state {
	kJobProbing = 0,
	kJobWaiting,
	kJobConverting,
	kJobDone,
	kJobFailed,
	kJobCancelled,
	kJobUnreadable,
};


/*!	One source file in the queue and what became of it. */
class QueueItem : public BListItem {
public:
								QueueItem(const entry_ref& ref);
	virtual						~QueueItem();

	virtual	void				DrawItem(BView* owner, BRect frame,
									bool complete);
	virtual	void				Update(BView* owner, const BFont* font);

			int32				ID() const { return fID; }
	const	entry_ref&			Ref() const { return fRef; }
			// the file, or for a disc the source string of its title
			BString				Path() const;
			BString				Name() const;

			// a DVD or Blu-ray: which title (-1 until the main one is
			// known)
			bool				IsDisc() const { return fDisc; }
			void				SetDisc(bool disc) { fDisc = disc; }
			int32				DiscTitle() const { return fDiscTitle; }
			void				SetDiscTitle(int32 title)
									{ fDiscTitle = title; }

			job_state			State() const { return fState; }
			void				SetState(job_state state)
									{ fState = state; }
			bool				IsPending() const
									{ return fState == kJobWaiting
										|| fState == kJobCancelled
										|| fState == kJobFailed; }

			SourceInfo&			Info() { return fInfo; }
			void				SetInfo(const SourceInfo& info);

			float				Progress() const { return fProgress; }
			void				SetProgress(const TranscodeProgress& progress);
			void				SetMessage(const char* message)
									{ fMessage = message; }
			BString				Message() const { return fMessage; }

			BString				Destination() const { return fDestination; }
			void				SetDestination(const char* path)
									{ fDestination = path; }
			TranscodeReport&	Report() { return fReport; }

private:
			BString				_StatusText() const;

	static	int32				sNextID;

			int32				fID;
			entry_ref			fRef;
			bool				fDisc;
			int32				fDiscTitle;
			SourceInfo			fInfo;
			job_state			fState;
			float				fProgress;
			TranscodeProgress	fLastProgress;
			BString				fMessage;
			BString				fDestination;
			TranscodeReport		fReport;
			BBitmap*			fIcon;
			float				fBaseline;
			float				fLineHeight;
};


/*!	The list of sources: files dropped on it are added. */
class QueueView : public BListView {
public:
								QueueView();

	virtual	void				MessageReceived(BMessage* message);
	virtual	void				KeyDown(const char* bytes, int32 count);
	virtual	void				Draw(BRect updateRect);

			QueueItem*			ItemByID(int32 id) const;
			QueueItem*			QueueItemAt(int32 index) const
									{ return (QueueItem*)ItemAt(index); }
			QueueItem*			SelectedItem() const;
			void				InvalidateItem(QueueItem* item);
};


#endif	// QUEUE_VIEW_H
