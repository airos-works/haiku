/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef TERMINAL_EMULATOR_H
#define TERMINAL_EMULATOR_H


#include <pthread.h>

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "Base64.h"
#include "TerminalCell.h"
#include "TerminalHistory.h"
#include "VTParser.h"


class TerminalGraphics;


// mouse tracking modes (DECSET 9, 1000, 1002, 1003)
enum {
	MOUSE_TRACKING_NONE = 0,
	MOUSE_TRACKING_X10 = 9,
	MOUSE_TRACKING_NORMAL = 1000,
	MOUSE_TRACKING_BUTTON = 1002,
	MOUSE_TRACKING_ANY = 1003
};

// mouse report encodings (DECSET 1005, 1006, 1015, 1016)
enum {
	MOUSE_ENCODING_DEFAULT = 0,
	MOUSE_ENCODING_UTF8 = 1005,
	MOUSE_ENCODING_SGR = 1006,
	MOUSE_ENCODING_URXVT = 1015,
	MOUSE_ENCODING_SGR_PIXELS = 1016
};

// cursor shapes (DECSCUSR)
enum {
	CURSOR_SHAPE_DEFAULT = 0,	// the user's choice
	CURSOR_SHAPE_BLOCK,
	CURSOR_SHAPE_UNDERLINE,
	CURSOR_SHAPE_BEAM
};

// kitty keyboard protocol flags
enum {
	KEYBOARD_DISAMBIGUATE = 0x01,
	KEYBOARD_REPORT_EVENTS = 0x02,
	KEYBOARD_REPORT_ALTERNATES = 0x04,
	KEYBOARD_REPORT_ALL_KEYS = 0x08,
	KEYBOARD_REPORT_TEXT = 0x10,
	KEYBOARD_ALL_FLAGS = 0x1f
};

// special colors of the dynamic palette (OSC 10-19 and friends)
enum {
	COLOR_FOREGROUND = 256,
	COLOR_BACKGROUND,
	COLOR_CURSOR,
	COLOR_CURSOR_TEXT,
	COLOR_SELECTION_FOREGROUND,
	COLOR_SELECTION_BACKGROUND,
	COLOR_POINTER_FOREGROUND,
	COLOR_POINTER_BACKGROUND,
	COLOR_COUNT
};

// a palette entry that is not set (the renderer then picks one)
static const uint32 kColorUnset = 0xffffffff;


// keys without a character, numbered as kitty numbers them
enum {
	KEY_ESCAPE = 57344,
	KEY_ENTER,
	KEY_TAB,
	KEY_BACKSPACE,
	KEY_INSERT,
	KEY_DELETE,
	KEY_LEFT,
	KEY_RIGHT,
	KEY_UP,
	KEY_DOWN,
	KEY_PAGE_UP,
	KEY_PAGE_DOWN,
	KEY_HOME,
	KEY_END,
	KEY_CAPS_LOCK,
	KEY_SCROLL_LOCK,
	KEY_NUM_LOCK,
	KEY_PRINT_SCREEN,
	KEY_PAUSE,
	KEY_MENU,
	KEY_F1,			// 57364
	KEY_F12 = KEY_F1 + 11,
	KEY_F13 = 57376,
	KEY_F35 = 57398,
	KEY_KP_0 = 57399,
	KEY_KP_9 = 57408,
	KEY_KP_DECIMAL,
	KEY_KP_DIVIDE,
	KEY_KP_MULTIPLY,
	KEY_KP_SUBTRACT,
	KEY_KP_ADD,
	KEY_KP_ENTER,
	KEY_KP_EQUAL,
	KEY_KP_SEPARATOR,
	KEY_KP_LEFT,
	KEY_KP_RIGHT,
	KEY_KP_UP,
	KEY_KP_DOWN,
	KEY_KP_PAGE_UP,
	KEY_KP_PAGE_DOWN,
	KEY_KP_HOME,
	KEY_KP_END,
	KEY_KP_INSERT,
	KEY_KP_DELETE,
	KEY_KP_BEGIN,
	KEY_MEDIA_PLAY,
	KEY_MEDIA_PAUSE,
	KEY_MEDIA_PLAY_PAUSE,
	KEY_MEDIA_REVERSE,
	KEY_MEDIA_STOP,
	KEY_MEDIA_FAST_FORWARD,
	KEY_MEDIA_REWIND,
	KEY_MEDIA_TRACK_NEXT,
	KEY_MEDIA_TRACK_PREVIOUS,
	KEY_MEDIA_RECORD,
	KEY_LOWER_VOLUME,
	KEY_RAISE_VOLUME,
	KEY_MUTE_VOLUME,
	KEY_LEFT_SHIFT,
	KEY_LEFT_CONTROL,
	KEY_LEFT_ALT,
	KEY_LEFT_SUPER,
	KEY_LEFT_HYPER,
	KEY_LEFT_META,
	KEY_RIGHT_SHIFT,
	KEY_RIGHT_CONTROL,
	KEY_RIGHT_ALT,
	KEY_RIGHT_SUPER,
	KEY_RIGHT_HYPER,
	KEY_RIGHT_META,
	KEY_ISO_LEVEL3_SHIFT,
	KEY_ISO_LEVEL5_SHIFT
};

// key modifiers as the kitty protocol numbers them
enum {
	KEY_MODIFIER_SHIFT = 0x01,
	KEY_MODIFIER_ALT = 0x02,
	KEY_MODIFIER_CONTROL = 0x04,
	KEY_MODIFIER_SUPER = 0x08,
	KEY_MODIFIER_HYPER = 0x10,
	KEY_MODIFIER_META = 0x20,
	KEY_MODIFIER_CAPS_LOCK = 0x40,
	KEY_MODIFIER_NUM_LOCK = 0x80
};

enum {
	KEY_ACTION_PRESS = 1,
	KEY_ACTION_REPEAT = 2,
	KEY_ACTION_RELEASE = 3
};


struct TermKeyEvent {
	uint32		key;			// the unshifted character, or a KEY_ code
	uint32		shiftedKey;		// the character with shift, 0 if unknown
	uint32		baseKey;		// the key on a US (PC-101) layout, or 0
	uint32		modifiers;		// KEY_MODIFIER_*
	int32		action;			// KEY_ACTION_*
	const char*	text;			// what the key types (UTF-8), or ""
};


// mouse events
enum {
	MOUSE_PRESS,
	MOUSE_RELEASE,
	MOUSE_MOTION,
	MOUSE_LEAVE
};

// mouse buttons, xterm numbering
enum {
	MOUSE_BUTTON_LEFT = 0,
	MOUSE_BUTTON_MIDDLE = 1,
	MOUSE_BUTTON_RIGHT = 2,
	MOUSE_BUTTON_NONE = 3,
	MOUSE_BUTTON_WHEEL_UP = 64,
	MOUSE_BUTTON_WHEEL_DOWN = 65,
	MOUSE_BUTTON_WHEEL_LEFT = 66,
	MOUSE_BUTTON_WHEEL_RIGHT = 67,
	MOUSE_BUTTON_BACK = 128,
	MOUSE_BUTTON_FORWARD = 129
};


struct TermPoint {
	int32	x;
	int32	y;		// negative rows are in the history

	TermPoint() : x(0), y(0) {}
	TermPoint(int32 x, int32 y) : x(x), y(y) {}

	bool operator==(const TermPoint& other) const
		{ return x == other.x && y == other.y; }
	bool operator!=(const TermPoint& other) const
		{ return !(*this == other); }
	bool operator<(const TermPoint& other) const
		{ return y < other.y || (y == other.y && x < other.x); }
	bool operator<=(const TermPoint& other) const
		{ return !(other < *this); }
	bool operator>(const TermPoint& other) const
		{ return other < *this; }
	bool operator>=(const TermPoint& other) const
		{ return !(*this < other); }
};


/*!	A desktop notification asked for with OSC 99 (kitty), OSC 9 or
	OSC 777.
*/
struct TermNotification {
	std::string	identifier;
	std::string	title;
	std::string	body;
	std::vector<std::string> iconNames;	// to try in order
	std::string	iconKey;	// names icon data for later notifications
	std::string	iconData;	// PNG, JPEG or GIF
	bool		focusOnActivation;
	bool		reportActivation;
	bool		reportClose;
	int32		urgency;	// 0 low, 1 normal, 2 critical
	int32		timeout;	// milliseconds, -1 for the default, 0 never
	bool		onlyWhenUnfocused;
	std::vector<std::string> buttons;

				TermNotification()
					:
					focusOnActivation(true),
					reportActivation(false),
					reportClose(false),
					urgency(1),
					timeout(-1),
					onlyWhenUnfocused(false)
				{
				}
};


//! Clipboard contents by MIME type, as OSC 5522 transfers them.
typedef std::vector<std::pair<std::string, std::string> > ClipboardData;


class Terminal : private VTParser::Handler {
public:
	class Listener {
	public:
		virtual					~Listener();

		// All hooks are called with the terminal locked, from whatever
		// thread fed it. They should only note or post what happened.
		virtual	void			TerminalContentsChanged();
		virtual	void			TerminalTitleChanged(const char* title);
		virtual	void			TerminalIconTitleChanged(const char* title);
		virtual	void			TerminalBell();
		// OSC 52
		virtual	void			TerminalSetClipboard(const char* selections,
									const void* data, size_t length);
		virtual	void			TerminalRequestClipboard(
									const char* selections);
		// OSC 5522
		virtual	void			TerminalClipboardWrite(bool primary,
									const ClipboardData& data,
									const char* identifier);
		virtual	void			TerminalClipboardRead(bool primary,
									const std::vector<std::string>& types,
									const char* identifier,
									const char* programName);
		virtual	void			TerminalNotify(
									const TermNotification& notification);
		virtual	void			TerminalCloseNotification(
									const char* identifier);
		virtual	void			TerminalPointerShapeChanged(
									const char* shape);
		virtual	void			TerminalColorsChanged();
		virtual	void			TerminalWorkingDirectoryChanged(
									const char* url);
		virtual	void			TerminalModesChanged();
		virtual	void			TerminalProgressChanged(int32 state,
									int32 percent);
	};

	struct CursorInfo {
		int32	x;
		int32	y;
		bool	visible;
		bool	blinking;
		int32	shape;
	};

public:
								Terminal();
	virtual						~Terminal();

			status_t			Init(int32 columns, int32 rows,
									int32 historyCapacity);

			void				SetListener(Listener* listener)
									{ fListener = listener; }

			bool				Lock();
			void				Unlock();

			//! Interprets output of the program. The terminal must be
			//! locked.
			void				Feed(const uint8* data, size_t length);

			//! Takes what the terminal has to send to the program (answers
			//! to queries), appending it to \a output.
			bool				TakeOutput(std::string& output);
			//! Queues \a data to be sent to the program.
			void				Send(const char* data, size_t length);
			void				Send(const std::string& data)
									{ Send(data.data(), data.size()); }

			status_t			Resize(int32 columns, int32 rows);
			void				SetCellSize(int32 width, int32 height);
			int32				CellWidth() const	{ return fCellWidth; }
			int32				CellHeight() const	{ return fCellHeight; }

			status_t			SetHistoryCapacity(int32 lines);

			void				FullReset();
			void				ClearHistory();
			//! Clears the screen and history, keeping the cursor line at the
			//! top (the "Clear all" menu item).
			void				ClearAll();

			// geometry
			int32				Columns() const		{ return fColumns; }
			int32				Rows() const		{ return fRows; }
			int32				HistorySize() const;
			bool				IsAlternateScreen() const
									{ return fScreen == &fAlternate; }
			uint64				LinesScrolled() const
									{ return fLinesScrolled; }
			uint64				ContentGeneration() const
									{ return fGeneration; }

			// reading the contents; rows below 0 are in the history
			const TerminalLine*	ScreenLine(int32 row) const;
			bool				GetLine(int32 row, TerminalCell* cells,
									uint16* _flags = NULL,
									uint64* _version = NULL) const;
			int32				LineLength(int32 row) const;
			uint16				LineFlags(int32 row) const;
			bool				GetCell(int32 row, int32 column,
									TerminalCell& cell) const;
			//! The code points of a cell's character (more than one for a
			//! grapheme cluster). Returns the count.
			int32				GetCharacter(uint32 character,
									uint32* codePoints,
									int32 maxCount) const;
			void				AppendCharacterUTF8(uint32 character,
									std::string& text) const;
			void				GetText(TermPoint start, TermPoint end,
									std::string& text,
									bool rectangular = false) const;
			bool				FindWord(TermPoint position,
									bool (*isWordChar)(uint32 c, void* cookie),
									void* cookie, bool findNonWords,
									TermPoint& _start, TermPoint& _end) const;
			bool				Find(const char* pattern, TermPoint start,
									bool forward, bool matchCase,
									bool matchWord, TermPoint& _matchStart,
									TermPoint& _matchEnd) const;
			//! Previous/next shell prompt (OSC 133) from \a row.
			bool				FindPrompt(int32 row, bool forward,
									int32& _row) const;

			// hyperlinks (OSC 8)
			bool				GetHyperLink(uint32 id, std::string& uri,
									std::string& identifier) const;

			CursorInfo			Cursor() const;

			// modes the user interface needs
			bool				IsReverseVideo() const
									{ return fModes.reverseVideo; }
			bool				IsApplicationCursorKeys() const
									{ return fModes.cursorKeys; }
			bool				IsApplicationKeypad() const
									{ return fModes.applicationKeypad; }
			bool				IsBracketedPaste() const
									{ return fModes.bracketedPaste; }
			bool				IsFocusReporting() const
									{ return fModes.focusReporting; }
			bool				IsAlternateScroll() const
									{ return fModes.alternateScroll; }
			bool				IsBackarrowSendsBackspace() const
									{ return fModes.backarrowSendsBS; }
			bool				IsMetaSendingEscape() const
									{ return fModes.metaSendsEscape; }
			bool				IsSynchronizedOutput() const;
			bool				IsLineFeedNewLine() const
									{ return fModes.lineFeedNewLine; }
			int32				MouseTracking() const
									{ return fModes.mouseTracking; }
			int32				MouseEncoding() const
									{ return fModes.mouseEncoding; }
			int32				ModifyOtherKeys() const
									{ return fModes.modifyOtherKeys; }
			uint32				KeyboardFlags() const;
			bool				WantsPasteEvents() const
									{ return fModes.pasteEvents; }
			bool				WantsColorSchemeReports() const
									{ return fModes.colorSchemeReports; }

			// encoding input for the program; return whether there is
			// something to send
			bool				EncodeKey(const TermKeyEvent& event,
									std::string& output) const;
			bool				EncodeMouse(int32 type, int32 button,
									uint32 modifiers, int32 column,
									int32 row, int32 pixelX, int32 pixelY,
									std::string& output) const;
			void				EncodePaste(const char* text, size_t length,
									std::string& output) const;

			// colors: 0x00RRGGBB, or kColorUnset
			void				SetDefaultColor(int32 index, uint32 color);
			uint32				DefaultColor(int32 index) const;
			uint32				Color(int32 index) const;
			uint32				ColorsGeneration() const
									{ return fColorsGeneration; }
			//! Resolves a cell color to RGB.
			uint32				ResolveColor(TermColor color,
									bool foreground) const;
			const char*			Title() const	{ return fTitle.c_str(); }
			const char*			WorkingDirectory() const
									{ return fWorkingDirectory.c_str(); }
			const char*			PointerShape() const;

			void				SetDefaultCursorShape(int32 shape,
									bool blinking);
			void				SetColorScheme(bool dark);
			void				SetFocused(bool focused);

			TerminalGraphics*	Graphics() const	{ return fGraphics; }

			// the user's answer to a notification (OSC 99)
			void				ReportNotificationActivated(
									const char* identifier, int32 button);
			void				ReportNotificationClosed(
									const char* identifier);

			// answers to clipboard requests
			void				ReplyClipboard(const char* selections,
									const void* data, size_t length);
			void				ReplyClipboardRead(const char* identifier,
									bool primary, const char* status,
									const ClipboardData* data);

			// absolute line numbers (for images): screen row 0 of the
			// main screen is line LinesScrolled()
			int64				AbsoluteLine(int32 row) const;

private:
			struct Modes {
				// ANSI modes
				bool	insert;					// IRM (4)
				bool	lineFeedNewLine;		// LNM (20)
				// DEC private modes
				bool	cursorKeys;				// DECCKM (1)
				bool	column132;				// DECCOLM (3)
				bool	reverseVideo;			// DECSCNM (5)
				bool	origin;					// DECOM (6)
				bool	autoWrap;				// DECAWM (7)
				bool	autoRepeat;				// DECARM (8)
				bool	cursorBlink;			// (12)
				bool	cursorVisible;			// DECTCEM (25)
				bool	allow132;				// (40)
				bool	reverseWrap;			// (45)
				bool	applicationKeypad;		// DECNKM (66), DECKPAM
				bool	backarrowSendsBS;		// DECBKM (67)
				bool	leftRightMargins;		// DECLRMM (69)
				int32	mouseTracking;
				int32	mouseEncoding;
				bool	focusReporting;			// (1004)
				bool	alternateScroll;		// (1007)
				bool	interpretMeta;			// (1034)
				bool	metaSendsEscape;		// (1036, 1039)
				bool	bracketedPaste;			// (2004)
				bool	synchronizedOutput;		// (2026)
				bool	colorSchemeReports;		// (2031)
				bool	inBandResize;			// (2048)
				bool	pasteEvents;			// (5522)
				int32	modifyOtherKeys;		// XTMODKEYS 4
			};

			struct Charsets {
				uint8	designations[4];		// 'B', '0', 'A', ...
				int8	gl;
				int8	gr;
				int8	singleShift;			// -1 or 2, 3
			};

			struct SavedCursor {
				int32		x;
				int32		y;
				bool		pendingWrap;
				CellStyle	style;
				uint32		hyperlink;
				Charsets	charsets;
				bool		origin;
				bool		autoWrap;
				bool		valid;
			};

			struct Screen {
				TerminalLine*	lines;			// ring of fRows lines
				TerminalCell*	storage;
				int32			firstLine;		// ring index of row 0
				SavedCursor		savedCursor;
				std::vector<uint32> keyboardStack;	// kitty flags stack
				uint32			keyboardFlags;
				std::vector<std::string> pointerShapes;
			};

			struct HyperLink {
				std::string	uri;
				std::string	identifier;
			};

private:
	// VTParser::Handler
	virtual	void				Print(const uint32* codePoints, int32 count);
	virtual	void				Execute(uint8 control);
	virtual	void				EscDispatch(const VTSequence& sequence);
	virtual	void				CsiDispatch(const VTSequence& sequence);
	virtual	void				OscDispatch(const char* data, size_t length);
	virtual	void				DcsDispatch(const VTSequence& sequence,
									const char* data, size_t length);
	virtual	void				ApcDispatch(const char* data, size_t length);

private:
	// screen storage
			status_t			_InitScreen(Screen& screen, int32 columns,
									int32 rows);
			void				_FreeScreen(Screen& screen);
	inline	TerminalLine&		_Line(int32 row) const;
	inline	TerminalCell*		_Cells(int32 row) const;
			void				_Touch(int32 row);
			void				_TouchRange(int32 firstRow, int32 lastRow);
			void				_Changed();
			void				_ClearCells(TerminalCell* cells, int32 count,
									bool keepProtected = false);
			void				_ClearLine(int32 row, int32 from, int32 to,
									bool selective = false);
			void				_ClearRows(int32 firstRow, int32 lastRow,
									bool selective = false);

	// printing
			void				_PrintCodePoint(uint32 c);
			void				_PrintASCII(const uint32* codePoints,
									int32 count);
			bool				_AppendToPrevious(uint32 c, int width);
			void				_PutCharacter(uint32 character, int width);
			uint32				_MapCharset(uint32 c);
			uint32				_InternCluster(const uint32* codePoints,
									int32 count);
			void				_ClusterCodePoints(uint32 character,
									std::vector<uint32>& codePoints) const;

	// cursor movement and scrolling
			void				_SetCursor(int32 x, int32 y);
			void				_MoveCursorTo(int32 x, int32 y);
			void				_LineFeed();
			void				_ReverseIndex();
			void				_CarriageReturn();
			void				_Backspace();
			void				_Tab(int32 count);
			void				_BackTab(int32 count);
			void				_ScrollUp(int32 count);
			void				_ScrollDown(int32 count);
			void				_ScrollRegionUp(int32 top, int32 bottom,
									int32 count, bool toHistory);
			void				_ScrollRegionDown(int32 top, int32 bottom,
									int32 count);
			void				_ScrollLeft(int32 count);
			void				_ScrollRight(int32 count);
			void				_Unscroll(int32 count);
			void				_PushLineToHistory(TerminalLine& line);
			bool				_FullWidthMargins() const;
			void				_InsertCharacters(int32 count);
			void				_DeleteCharacters(int32 count);
			void				_InsertLines(int32 count);
			void				_DeleteLines(int32 count);
			void				_InsertColumns(int32 count);
			void				_DeleteColumns(int32 count);
			void				_EraseCharacters(int32 count);
			void				_EraseInDisplay(int32 mode, bool selective);
			void				_EraseInLine(int32 mode, bool selective);
			void				_RepeatCharacter(int32 count);
			void				_FixWideCharacters(int32 row, int32 from,
									int32 to);

	// state
			void				_SaveCursor();
			void				_RestoreCursor();
			void				_SoftReset();
			void				_SwitchScreen(bool alternate, bool clear,
									bool saveCursor);
			void				_SetMode(int32 mode, bool privateMode,
									bool set);
			int32				_ModeState(int32 mode, bool privateMode)
									const;
			void				_SetTabStop();
			void				_ClearTabStops(int32 mode);
			void				_ResetTabStops();
			void				_SetScrollRegion(int32 top, int32 bottom);
			void				_SetHorizontalMargins(int32 left,
									int32 right);

	// control sequences
			void				_SelectGraphicRendition(
									const VTSequence& sequence);
			void				_ApplySGR(const VTSequence& sequence,
									int32 start, int32 end, CellStyle& style,
									bool reverseOnly);
			int32				_ParseSGRColor(const VTSequence& sequence,
									int32 index, TermColor& color);
			void				_DeviceStatusReport(
									const VTSequence& sequence);
			void				_WindowOperation(const VTSequence& sequence);
			void				_RequestMode(const VTSequence& sequence);
			void				_SetCursorStyle(int32 style);
			void				_KeyboardProtocol(const VTSequence& sequence);
			void				_RectangularOperation(
									const VTSequence& sequence);
			void				_CopyRectangle(const VTSequence& sequence);
			void				_ChangeAttributesInArea(
									const VTSequence& sequence, bool reverse);
			void				_FillRectangle(const VTSequence& sequence);
			void				_EraseRectangle(const VTSequence& sequence,
									bool selective);
			bool				_RectangleFromParameters(
									const VTSequence& sequence, int32 first,
									int32& top, int32& left, int32& bottom,
									int32& right) const;
			void				_PushSGR(const VTSequence& sequence);
			void				_PopSGR();
			void				_ReportCursorStyle(std::string& reply) const;

	// operating system commands
			void				_SetTitle(const char* title, bool window,
									bool icon);
			void				_SetColors(int32 command, const char* spec);
			void				_ResetColors(int32 command, const char* spec);
			void				_ColorReply(int32 command, int32 index,
									uint32 color, bool bell);
			bool				_ParseColor(const char* spec,
									uint32& _color) const;
			void				_HyperLink(const char* parameters,
									const char* uri);
			void				_Clipboard(const char* parameters);
			void				_KittyColors(const char* spec);
			void				_PointerShape(const char* spec);
			void				_ShellIntegration(const char* spec);
			void				_KittyNotification(const char* metadata,
									const char* payload);
			void				_PushColors(int32 index);
			void				_PopColors(int32 index);
			void				_ClipboardProtocol(const char* data);
			void				_TextSizing(const char* data);

	// device control strings
			void				_RequestStatusString(const char* request);
			void				_RequestTermcap(const char* request);

			void				_Reply(const char* format, ...)
									__attribute__((format(printf, 2, 3)));

private:
			friend class TerminalGraphics;

			pthread_mutex_t		fLock;
			Listener*			fListener;
			VTParser			fParser;

			int32				fColumns;
			int32				fRows;
			int32				fCellWidth;
			int32				fCellHeight;

			Screen				fMain;
			Screen				fAlternate;
			Screen*				fScreen;
			TerminalHistory		fHistory;
			uint64				fLinesScrolled;
			uint64				fGeneration;
			uint64				fVersionCounter;

			// cursor
			int32				fCursorX;
			int32				fCursorY;
			bool				fPendingWrap;
			CellStyle			fStyle;
			uint32				fHyperLink;
			Charsets			fCharsets;
			int32				fCursorShape;
			bool				fCursorBlinkSet;
			int32				fDefaultCursorShape;
			bool				fDefaultCursorBlinking;

			// margins
			int32				fTop;
			int32				fBottom;
			int32				fLeft;
			int32				fRight;

			Modes				fModes;
			std::map<int32, bool> fSavedModes;
			std::vector<bool>	fTabStops;

			// the last printed character, for REP and combining
			uint32				fLastCharacter;
			int32				fLastRow;
			int32				fLastColumn;
			bool				fLastValid;

			// grapheme clusters
			std::vector<std::vector<uint32> > fClusters;
			std::unordered_map<std::string, uint32> fClusterIndex;

			// hyperlinks
			std::vector<HyperLink> fHyperLinks;
			std::unordered_map<std::string, uint32> fHyperLinkIndex;

			// colors
			uint32				fDefaultColors[COLOR_COUNT];
			uint32				fColors[COLOR_COUNT];
			uint32				fColorsGeneration;
			std::vector<std::vector<uint32> > fColorStack;
			std::vector<CellStyle> fSGRStack;

			// titles
			std::string			fTitle;
			std::string			fIconTitle;
			std::vector<std::string> fTitleStack;
			std::string			fWorkingDirectory;

			bool				fFocused;
			bool				fDarkColorScheme;
			bool				fRectangularAttributes;	// DECSACE 2
			bigtime_t			fSynchronizedSince;

			// notifications in pieces (OSC 99)
			std::map<std::string, TermNotification> fPendingNotifications;
			std::map<std::string, int32> fNotificationFlags;

			// a clipboard write in progress (OSC 5522)
			struct ClipboardWrite {
				bool			active;
				bool			failed;
				bool			primary;
				std::string		identifier;
				std::string		mimeType;
				ClipboardData	data;
				std::vector<std::pair<std::string, std::string> > aliases;
				size_t			size;
			}					fClipboardWrite;
			Base64Decoder		fClipboardDecoder;

			std::string			fOutput;
			TerminalGraphics*	fGraphics;
};


TerminalLine&
Terminal::_Line(int32 row) const
{
	int32 index = fScreen->firstLine + row;
	if (index >= fRows)
		index -= fRows;
	return fScreen->lines[index];
}


TerminalCell*
Terminal::_Cells(int32 row) const
{
	return _Line(row).cells;
}


#endif	// TERMINAL_EMULATOR_H
