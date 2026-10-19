/*
 * Copyright 2001-2026, Haiku, Inc. All rights reserved.
 * Copyright (c) 2003-4 Kian Duffy <myob@users.sourceforge.net>
 * Parts Copyright (C) 1998,99 Kazuho Okui and Takashi Murai.
 *
 * Distributed under the terms of the MIT license.
 * Authors:
 *		Stefano Ceccherini, stefano.ceccherini@gmail.com
 *		Kian Duffy, myob@users.sourceforge.net
 *		Simon South, simon@simonsouth.net
 *		Ingo Weinhold, ingo_weinhold@gmx.de
 *		Siarzhuk Zharski, zharik@gmx.li
 */
#ifndef TERMVIEW_H
#define TERMVIEW_H


#include <HashMap.h>
#include <InterfaceDefs.h>
#include <Locker.h>
#include <Messenger.h>
#include <String.h>
#include <View.h>

#include <atomic>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "FrameBuilder.h"
#include "GlyphCache.h"
#include "RenderFrame.h"
#include "Terminal.h"


class ActiveProcessInfo;
class BBitmap;
class BClipboard;
class BMessageRunner;
class BScrollBar;
class BScrollView;
class BString;
class BStringView;
class DefaultCharClassifier;
class InlineInput;
class ShellInfo;
class ShellParameters;
class Shell;
class TermRenderer;


class TermView : public BView, private Terminal::Listener {
public:
			class Listener;

public:
								TermView(BRect frame,
									const ShellParameters& shellParameters,
									int32 historySize);
								TermView(int rows, int columns,
									const ShellParameters& shellParameters,
									int32 historySize);
								TermView(BMessage* archive);
								~TermView();

	static	BArchivable*		Instantiate(BMessage* data);
	virtual status_t			Archive(BMessage* data, bool deep = true) const;

	virtual void				GetPreferredSize(float* _width, float* _height);

			bool				IsShellBusy() const;
			bool				GetActiveProcessInfo(
									ActiveProcessInfo& _info) const;
			bool				GetShellInfo(ShellInfo& _info) const;

			const char*			TerminalName() const;

			void				GetTermFont(BFont* font) const;
			void				SetTermFont(const BFont* font);

			void				GetFontSize(float* width, float* height) const;
			int					Rows() const;
			int					Columns() const;
			BRect				SetTermSize(int rows, int columns,
									bool notifyShell);
			void				SetTermSize(BRect rect,
									bool notifyShell = false);
			void				GetTermSizeFromRect(const BRect &rect,
									int *rows, int *columns) const;

			void				SetTextColor(rgb_color fore, rgb_color back);
			void				SetCursorColor(rgb_color fore, rgb_color back);
			void				SetSelectColor(rgb_color fore, rgb_color back);
			void				SetTermColor(uint index, rgb_color color,
									bool dynamic = false);
			status_t			GetTermColor(uint index, rgb_color* color) const;

			int					Encoding() const;
			void				SetEncoding(int encoding);

			void				SetScrollBar(BScrollBar* scrollBar);
			BScrollBar*			ScrollBar() const { return fScrollBar; };

			void				SetKeymap(const key_map* keymap,
									const char* chars);
			void				SetUseOptionAsMetaKey(bool enable);

			void				SetMouseClipboard(BClipboard *);

			void				MakeDebugSnapshots();
			void				StartStopDebugCapture();

			void				SwitchCursorBlinking();
			void				SwitchCursorBlinking(bool blinkingOn);

			// edit functions
			void				Copy(BClipboard* clipboard);
			void				Paste(BClipboard* clipboard);
			void				SyncClipboard();
			void				SelectAll();
			void				Clear();

			// Other
			void				GetFrameSize(float* width, float* height) const;
			bool				Find(const BString& str, bool forwardSearch,
									bool matchCase, bool matchWord);
			void				GetSelection(BString& string) const;

			bool				CheckShellGone() const;

			void				InitiateDrag();

			void				DisableResizeView(int32 disableCount = 1);

			void				SetListener(Listener* listener)
									{ fListener = listener; }

			//! How the terminal is drawn: "OpenGL: <renderer>" or
			//! "software".
			const char*			RendererName() const;

protected:
	virtual void				AttachedToWindow();
	virtual void				DetachedFromWindow();
	virtual void				Draw(BRect updateRect);
	virtual void				WindowActivated(bool active);
	virtual void				MakeFocus(bool focusState = true);
	virtual void				KeyDown(const char* bytes, int32 numBytes);
	virtual void				KeyUp(const char* bytes, int32 numBytes);

	virtual void				MouseDown(BPoint where);
	virtual void				MouseMoved(BPoint where, uint32 transit,
									const BMessage* message);
	virtual void				MouseUp(BPoint where);

	virtual void				FrameResized(float width, float height);
	virtual void				MessageReceived(BMessage* message);

	virtual void				ScrollTo(BPoint where);
	virtual void				TargetedByScrollView(BScrollView *scrollView);

	virtual status_t			GetSupportedSuites(BMessage* message);
	virtual BHandler*			ResolveSpecifier(BMessage* message, int32 index,
									BMessage* specifier, int32 form,
									const char* property);

private:
			class State;
			class StandardBaseState;
			class DefaultState;
			class SelectState;
			class HyperLinkState;
			class HyperLinkMenuState;

			friend class State;
			friend class StandardBaseState;
			friend class DefaultState;
			friend class SelectState;
			friend class HyperLinkState;
			friend class HyperLinkMenuState;

			struct Highlight {
				TermPoint	start;
				TermPoint	end;		// exclusive
				bool		IsEmpty() const	{ return start == end; }
			};

			struct Event;

private:
	// Terminal::Listener, called on the reader thread with the terminal
	// locked
	virtual	void				TerminalContentsChanged();
	virtual	void				TerminalTitleChanged(const char* title);
	virtual	void				TerminalIconTitleChanged(const char* title);
	virtual	void				TerminalBell();
	virtual	void				TerminalSetClipboard(const char* selections,
									const void* data, size_t length);
	virtual	void				TerminalRequestClipboard(
									const char* selections);
	virtual	void				TerminalClipboardWrite(bool primary,
									const ClipboardData& data,
									const char* identifier);
	virtual	void				TerminalClipboardRead(bool primary,
									const std::vector<std::string>& types,
									const char* identifier,
									const char* programName);
	virtual	void				TerminalNotify(
									const TermNotification& notification);
	virtual	void				TerminalCloseNotification(
									const char* identifier);
	virtual	void				TerminalPointerShapeChanged(
									const char* shape);
	virtual	void				TerminalColorsChanged();
	virtual	void				TerminalWorkingDirectoryChanged(
									const char* url);
	virtual	void				TerminalModesChanged();
	virtual	void				TerminalProgressChanged(int32 state,
									int32 percent);

private:
			status_t			_InitObject(
									const ShellParameters& shellParameters);
			void				_GetArgumentsFromMessage(const BMessage* message,
									const char**& argv, int32& argc);

			status_t			_AttachShell(Shell* shell,
									const ShellParameters& shellParameters);
			Shell*				_DetachShell();
			status_t			_StartReader();
			void				_StopReader();
	static	status_t			_ReaderThread(void* data);
			void				_ReadLoop();
	static	status_t			_WriterThread(void* data);
			void				_WriteLoop();

			void				_Activate();
			void				_Deactivate();

			// geometry
			float				_DeviceScale() const;
			void				_UpdateMetrics();
			void				_ResizeBitmap();
			int32				_FirstRow() const;
			TermPoint			_ConvertToTerminal(const BPoint& point) const;
			BPoint				_ConvertFromTerminal(const TermPoint& pos)
									const;
			void				_PixelPosition(const BPoint& point,
									int32& x, int32& y) const;

			// drawing
			void				_ScheduleFrame();
			void				_RenderFrame();
			void				_InvalidateAll();
			void				_LoadRenderer();
			void				_PostEvent(Event* event);
			void				_HandleEvents();
			void				_UpdateScrollBarRange();

			// cursor
			bool				_IsCursorVisible() const;
			void				_BlinkCursor();
			void				_ActivateCursor(bool invalidate);

			// writing to the shell
			void				_WritePTY(const char* text, int32 numBytes);
			void				_Send(const std::string& data);
			void				_SendKey(const TermKeyEvent& event);
			bool				_TranslateKey(const BMessage* message,
									bool keyUp, TermKeyEvent& event,
									BString& text);
			void				_SendMouseEvent(int32 type, int32 button,
									BPoint where, int32 modifiers);
			bool				_ReportsMouse() const;
			void				_UpdateSIGWINCH();

			// scrolling
			void				_ScrollTo(float y, bool scrollGfx);
			void				_ScrollToRows(int32 rows);
			void				_ScrollToRange(TermPoint start, TermPoint end);
			void				_ScrollToBottom();
			void				_ScrollToPrompt(bool forward);

			// selection
			float				_MouseDistanceSinceLastClick(BPoint where);
			void				_Select(TermPoint start, TermPoint end,
									bool inclusive, bool setInitialSelection);
			void				_ExtendSelection(TermPoint, bool inclusive,
									bool useInitialSelection);
			void				_Deselect();
			bool				_HasSelection() const;
			void				_SelectWord(BPoint where, bool extend,
									bool useInitialSelection);
			void				_SelectLine(BPoint where, bool extend,
									bool useInitialSelection);
			bool				_FindWord(TermPoint position,
									DefaultCharClassifier* classifier,
									bool findNonWords, TermPoint& start,
									TermPoint& end) const;
			void				_GetText(TermPoint start, TermPoint end,
									BString& text) const;
			void				_SetLinkHighlight(TermPoint start,
									TermPoint end);
			void				_ClearLinkHighlight();

			// drag and drop, clipboard
			void				_SecondaryMouseButtonDropped(BMessage* message);
			void				_DoSecondaryMouseDropAction(BMessage* message);
			void				_DoFileDrop(entry_ref &ref);
			void				_PasteText(const char* text, size_t length);
			void				_PutOnClipboard(BClipboard* clipboard,
									const char* text, size_t length);

			// input methods
			void				_DrawInlineMethodString();
			void				_HandleInputMethodChanged(BMessage* message);
			void				_HandleInputMethodLocationRequest();
			void				_CancelInputMethod();

			void				_UpdateModifiers();
			void				_NextState(State* state);
			void				_SetPointerShape(const char* shape);

private:
			Listener*			fListener;
			Shell*				fShell;
			Terminal*			fTerminal;

			thread_id			fReaderThread;
			std::atomic<bool>	fReaderQuit;
			thread_id			fWriterThread;
			std::atomic<bool>	fWriterQuit;
			sem_id				fWriteSemaphore;
			BLocker				fWriteLock;
			std::deque<std::string> fWriteQueue;
			std::atomic<bool>	fFramePending;
			int32				fConverterState;	// non-UTF-8 encodings

			std::vector<Event*>	fEvents;		// guarded by the terminal
			bool				fEventsPosted;

			BMessageRunner*		fWinchRunner;
			BMessageRunner*		fCursorBlinkRunner;
			BMessageRunner*		fAutoScrollRunner;
			BMessageRunner*		fResizeRunner;
			BMessageRunner*		fFrameRunner;
			BMessageRunner*		fAnimationRunner;
			BStringView*		fResizeView;
			DefaultCharClassifier* fCharClassifier;

			// the font and the cells
			BFont				fFont;
			bool				fEmulateBold;
			bool				fAllowBold;
			float				fFontWidth;		// view coordinates
			float				fFontHeight;
			float				fScale;

			// drawing
			GlyphCache			fGlyphs;
			FrameBuilder		fBuilder;
			RenderFrame			fFrame;
			TermRenderer*		fRenderer;
			TermRenderer*		fSoftwareRenderer;
			BBitmap*			fBitmap;
			bigtime_t			fLastFrameTime;
			bool				fFrameNeeded;

			// frame resized flag.
			bool				fFrameResized;
			int32				fResizeViewDisableCount;

			// cursor blinking
			bigtime_t			fLastActivityTime;
			int32				fCursorState;
			bool				fCursorBlinking;

			// terminal rows and columns
			int					fColumns;
			int					fRows;

			int					fEncoding;
			bool				fActive;

			BScrollBar*			fScrollBar;
			InlineInput*		fInline;

			// colors
			rgb_color			fTextForeColor;
			rgb_color			fTextBackColor;
			rgb_color			fCursorForeColor;
			rgb_color			fCursorBackColor;
			rgb_color			fSelectForeColor;
			rgb_color			fSelectBackColor;

			// scrolling: rows scrolled back into the history
			int32				fScrollRows;
			uint64				fLinesScrolled;
			int32				fHistorySize;
			int32				fScrBufSize;
			float				fAutoScrollSpeed;

			// selection
			Highlight			fSelection;
			TermPoint			fInitialSelectionStart;
			TermPoint			fInitialSelectionEnd;
			BPoint				fLastClickPoint;
			Highlight			fLinkHighlight;

			// keyboard
			const key_map*		fKeymap;
			const char*			fKeymapChars;
			HashMap<HashKey32<int32>, const int32(*)[128]>
								fKeymapTableForModifiers;
			bool				fUseOptionAsMetaKey;
			int32				fPressedModifierKeys;

			// mouse
			int32				fMouseButtons;
			int32				fModifiers;
			TermPoint			fPrevPos;
			BClipboard*			fMouseClipboard;
			BString				fPointerShape;

			// notifications the program sent, by our id
			std::map<std::string, std::string> fNotifications;
			int32				fNotificationCounter;

			// states
			DefaultState*		fDefaultState;
			SelectState*		fSelectState;
			HyperLinkState*		fHyperLinkState;
			HyperLinkMenuState*	fHyperLinkMenuState;
			State*				fActiveState;
};


class TermView::Listener {
public:
	virtual						~Listener();

	// all hooks called in the window thread
	virtual	void				NotifyTermViewQuit(TermView* view,
									int32 reason);
	virtual	void				SetTermViewTitle(TermView* view,
									const char* title);
	virtual	void				PreviousTermView(TermView* view);
	virtual	void				NextTermView(TermView* view);
};


#endif // TERMVIEW_H
