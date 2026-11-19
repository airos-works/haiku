/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef FIND_BAR_H
#define FIND_BAR_H


#include <String.h>
#include <View.h>


class BStringView;
class BTextControl;
class FindBarButton;


// what the bar tells its target (the terminal view)
enum {
	MSG_FIND_BAR_CHANGED	= 'fbch',	// the text or the options
	MSG_FIND_BAR_UP			= 'fbup',	// to the match above (older)
	MSG_FIND_BAR_DOWN		= 'fbdn',	// to the match below (newer)
	MSG_FIND_BAR_CLOSE		= 'fbcl'
};

// case sensitivity
enum {
	FIND_SMART_CASE = 0,	// sensitive if the text has capitals
	FIND_MATCH_CASE,
	FIND_IGNORE_CASE
};


/*!	The search bar (as iTerm2's): a field at the top right of a terminal
	that finds as you type, with the number of matches, buttons to go to
	the next and previous one, and options (case, regular expressions,
	whole words).
*/
class FindBar : public BView {
public:
								FindBar(BHandler* target);
	virtual						~FindBar();

			const char*			Text() const;
			void				SetText(const char* text);
			int32				CaseMode() const	{ return fCaseMode; }
			bool				IsRegularExpression() const
									{ return fRegularExpression; }
			bool				IsWholeWords() const	{ return fWholeWords; }

			//! "3 of 17", "No matches", or an error.
			void				SetStatus(const char* status, bool error);
			//! Puts the keyboard focus into the field, its text selected.
			void				FocusField();
			bool				HasFocus() const;

			//! Places the bar at the top right of its parent.
			void				UpdatePosition();

	virtual	void				AttachedToWindow();
	virtual	void				Draw(BRect updateRect);
	virtual	void				MessageReceived(BMessage* message);
	virtual	void				GetPreferredSize(float* _width,
									float* _height);

private:
			void				_Layout();
			void				_ShowOptions();
			void				_Notify(uint32 what);

private:
			BHandler*			fTarget;
			BTextControl*		fField;
			BStringView*		fStatus;
			FindBarButton*		fOptionsButton;
			FindBarButton*		fPreviousButton;
			FindBarButton*		fNextButton;
			FindBarButton*		fCloseButton;
			int32				fCaseMode;
			bool				fRegularExpression;
			bool				fWholeWords;
			bool				fError;
};


#endif	// FIND_BAR_H
