/*
 * Copyright 2001-2026, Haiku, Inc.
 * Copyright 2003-2004 Kian Duffy, myob@users.sourceforge.net
 * Parts Copyright 1998-1999 Kazuho Okui and Takashi Murai.
 * All rights reserved. Distributed under the terms of the MIT license.
 *
 * Authors:
 *		Ingo Weinhold, ingo_weinhold@gmx.de
 *		Clemens Zeidler, haiku@Clemens-Zeidler.de
 *		Siarzhuk Zharski, zharik@gmx.li
 */
#ifndef TERM_VIEW_STATES_H
#define TERM_VIEW_STATES_H


#include "HyperLink.h"
#include "TerminalCharClassifier.h"
#include "TermView.h"


class TermView::State {
public:
								State(TermView* view);
	virtual						~State();

	virtual	void				Entered();
	virtual	void				Exited();

	virtual	bool				MessageReceived(BMessage* message);
									// returns true, if handled

	virtual	void				ModifiersChanged(int32 oldModifiers,
									int32 modifiers);
	virtual void				KeyDown(const char* bytes, int32 numBytes);

	virtual void				MouseDown(BPoint where, int32 buttons,
									int32 modifiers);
	virtual void				MouseMoved(BPoint where, uint32 transit,
									const BMessage* message, int32 modifiers);
	virtual void				MouseUp(BPoint where, int32 buttons);

	virtual	void				WindowActivated(bool active);

protected:
			TermView*			fView;
};


class TermView::StandardBaseState : public TermView::State {
public:
								StandardBaseState(TermView* view);

protected:
			bool				_StandardMouseMoved(BPoint where,
									int32 modifiers);
};


class TermView::DefaultState : public TermView::StandardBaseState {
public:
								DefaultState(TermView* view);

	virtual	void				ModifiersChanged(int32 oldModifiers,
									int32 modifiers);

	virtual void				KeyDown(const char* bytes, int32 numBytes);

	virtual void				MouseDown(BPoint where, int32 buttons,
									int32 modifiers);
	virtual void				MouseMoved(BPoint where, uint32 transit,
									const BMessage* message, int32 modifiers);
	virtual void				MouseUp(BPoint where, int32 buttons);

	virtual	void				WindowActivated(bool active);

private:
			bool				_CheckEnterHyperLinkState(int32 modifiers);
			bool				_CheckLinkClick(BPoint where, int32 buttons,
									int32 modifiers);
};


class TermView::SelectState : public TermView::StandardBaseState {
public:
								SelectState(TermView* view);

			void				Prepare(BPoint where, int32 modifiers,
									int32 clicks = -1);

	virtual	bool				MessageReceived(BMessage* message);

	virtual void				MouseMoved(BPoint where, uint32 transit,
									const BMessage* message, int32 modifiers);
	virtual void				MouseUp(BPoint where, int32 buttons);

private:
			void				_AutoScrollUpdate();

private:
			int32				fSelectGranularity;
			bool				fCheckMouseTracking;
			bool				fMouseTracking;
};


class TermView::HyperLinkState : public TermView::State {
public:
								HyperLinkState(TermView* view);

	virtual	void				Entered();
	virtual	void				Exited();

	virtual	void				ModifiersChanged(int32 oldModifiers,
									int32 modifiers);

	virtual void				MouseDown(BPoint where, int32 buttons,
									int32 modifiers);
	virtual void				MouseMoved(BPoint where, uint32 transit,
									const BMessage* message, int32 modifiers);

	virtual	void				WindowActivated(bool active);

private:
			bool				_GetHyperLinkAt(BPoint where,
									bool pathPrefixOnly, HyperLink& _link,
									TermPoint& _start, TermPoint& _end,
									TermLinkRange* _range = NULL);
			bool				_EntryExists(const BString& path,
									BString& _actualPath) const;

			void				_UpdateHighlight();
			void				_UpdateHighlight(BPoint where, int32 modifiers);
			void				_ActivateHighlight(const TermPoint& start,
									const TermPoint& end);
			void				_ActivateHighlight(
									const TermLinkRange& range);
			void				_DeactivateHighlight();

private:
			DefaultCharClassifier fPathComponentCharClassifier;
			BString				fCurrentDirectory;
			bool				fHighlightActive;
};


/*!	A click on a link: it opens when the button goes up where it went
	down. Moving away first makes it what it would have been without the
	link, a selection or a press the program is told about.
*/
class TermView::LinkClickState : public TermView::State {
public:
								LinkClickState(TermView* view);

			void				Prepare(BPoint where, int32 modifiers,
									const TermLinkRange& link);

	virtual	void				Exited();

	virtual void				MouseMoved(BPoint where, uint32 transit,
									const BMessage* message, int32 modifiers);
	virtual void				MouseUp(BPoint where, int32 buttons);

	virtual	void				WindowActivated(bool active);

private:
			void				_Abandon();

private:
			BPoint				fWhere;
			bigtime_t			fWhen;
			int32				fModifiers;
			TermLinkRange		fLink;
};


class TermView::HyperLinkMenuState : public TermView::State {
public:
								HyperLinkMenuState(TermView* view);

			void				Prepare(BPoint point, const HyperLink& link);

	virtual	void				Exited();

	virtual	bool				MessageReceived(BMessage* message);

private:
			class PopUpMenu;

private:
			HyperLink			fLink;
};


#endif	// TERM_VIEW_STATES_H
