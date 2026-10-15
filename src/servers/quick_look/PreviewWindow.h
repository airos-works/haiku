/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef QUICK_LOOK_PREVIEW_WINDOW_H
#define QUICK_LOOK_PREVIEW_WINDOW_H


#include <mutex>
#include <vector>

#include <DirectWindow.h>
#include <Entry.h>
#include <Messenger.h>
#include <String.h>


class AddOnManager;
class BButton;
class BQuickLookPreview;
class BStringView;
class FooterView;


/*!	The Quick Look panel: a floating window that never takes the focus (so
	Tracker keeps the keyboard), with the preview above a footer that names
	the kind of file and offers to open it. It is a BDirectWindow so that a
	video preview can draw into the frame buffer. */
class PreviewWindow : public BDirectWindow {
public:
								PreviewWindow(AddOnManager* addOns);
	virtual						~PreviewWindow();

	//! The refs of a kMsgQuickLookShow message.
			void				ShowEntries(const BMessage* message);
			void				HidePanel(bool notifyOwner);
			bool				IsShowing() const { return !IsHidden(); }
			bool				IsOwnedBy(const BMessenger& owner) const
									{ return fOwner == owner; }

	virtual	void				MessageReceived(BMessage* message);
	virtual	bool				QuitRequested();
	virtual	void				DirectConnected(direct_buffer_info* info);
	virtual	void				FrameResized(float width, float height);
	virtual	void				FrameMoved(BPoint origin);

private:
			void				_ShowCurrent();
			void				_SetPreview(BQuickLookPreview* preview);
			void				_HidePreview(bool hide);
			void				_StartPreview();
			void				_FitToContent();
			BRect				_TargetFrame();
			void				_StartAnimation(BRect from, BRect to,
									bool hiding);
			void				_AnimationStep();
			void				_FinishAnimation();
			void				_Layout();
			void				_UpdateFooter();
			void				_Step(int32 direction);
			void				_OpenCurrent();
			void				_NotifyClosed();
			void				_NotifyShown();
			float				_FooterHeight() const;

			AddOnManager*		fAddOns;
			std::vector<entry_ref> fRefs;
			int32				fIndex;
			BMessenger			fOwner;
			BRect				fOwnerFrame;
			BRect				fIconRect;

			// The zoom out of the entry's icon and back into it.
			BMessageRunner*		fAnimation;
			BRect				fAnimationFrom;
			BRect				fAnimationTo;
			bigtime_t			fAnimationStart;
			int32				fAnimationFrames;
			bigtime_t			fAnimationLongest;
			bool				fAnimationHides;
			bool				fNotifyAfterHide;

			BView*				fBorder;
			BView*				fContent;
			FooterView*			fFooter;
			BStringView*		fName;
			BStringView*		fSummary;
			BStringView*		fCount;
			BButton*			fPrevious;
			BButton*			fNext;
			BButton*			fOpen;

			std::mutex			fPreviewLock;
			BQuickLookPreview*	fPreview;
			bool				fPreviewStarted;
			bool				fPreviewHidden;
			BString				fMimeType;
			BString				fDefaultSummary;
			bool				fUserResized;
};


#endif	// QUICK_LOOK_PREVIEW_WINDOW_H
