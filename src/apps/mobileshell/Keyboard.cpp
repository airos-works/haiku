/*
 * Copyright 2026, air/OS.
 * Copyright 2026 Renku Mobile contributors.
 * Distributed under the terms of the MIT License.
 */


#include "Keyboard.h"

#include "EmojiData.h"
#include "Glyphs.h"
#include "Theme.h"

#include <Application.h>
#include <Bitmap.h>
#include <Region.h>
#include <MessageRunner.h>
#include <String.h>
#include <InterfaceDefs.h>
#include <OS.h>
#include <Window.h>

#include <algorithm>
#include <ctype.h>
#include <math.h>
#include <string.h>


static const char* kLetterRows[] = { "qwertyuiop", "asdfghjkl", "zxcvbnm" };
static const char* kSymbolRows[] = { "1234567890", "@#$%&-+()", "*\"':;!?/" };

using Theme::S;
using Theme::F;


#define kGap Theme::KeyGap()
#define kRowHeight Theme::KeyRowHeight()
#define kExtrasRowHeight (Theme::KeyboardExtrasHeight() - kGap)

// What the globe key's menu offers; the space bar shows the name.
static const struct {
	const char*	name;
	const char*	description;
	bool		extras;		// the row of terminal keys
	bool		corners;	// symbols in the keys' corners
} kLayouts[] = {
	{ "English", "US letters", false, false },
	{ "Terminal", "US letters, Ctrl, Tab and arrows", true, false },
	{ "Corners", "Flick a key to its corner symbols", false, true }
};

// The Corners layout's symbols on each letter, a to z, in compass order: nw,
// n, ne, w, e, sw, s, se. Placed after a US keyboard's: the top
// row's digits top right and their shifted symbols bottom right, the
// punctuation keys' pairs on the middle row, Esc and Tab where they sit on a
// PC keyboard.
static const char* const kCorners[26][8] = {
	{ "Tab", 0, "=", 0, 0, 0, 0, "+" },		// a
	{ 0 },									// b
	{ 0, 0, ",", 0, 0, 0, 0, 0 },			// c
	{ 0, 0, "[", 0, 0, 0, 0, "{" },			// d
	{ 0, 0, "3", 0, 0, 0, 0, "#" },			// e
	{ 0, 0, "]", 0, 0, 0, 0, "}" },			// f
	{ 0, 0, "\\", 0, 0, 0, 0, "|" },		// g
	{ 0, 0, ";", 0, 0, 0, 0, ":" },			// h
	{ 0, 0, "8", 0, 0, 0, 0, "*" },			// i
	{ 0, 0, "'", 0, 0, 0, 0, "\"" },		// j
	{ 0, 0, "/", 0, 0, 0, 0, "?" },			// k
	{ 0, 0, "`", 0, 0, 0, 0, "~" },			// l
	{ 0 },									// m
	{ 0 },									// n
	{ 0, 0, "9", 0, 0, 0, 0, "(" },			// o
	{ 0, 0, "0", 0, 0, 0, 0, ")" },			// p
	{ "Esc", 0, "1", 0, 0, 0, 0, "!" },		// q
	{ 0, 0, "4", 0, 0, 0, 0, "$" },			// r
	{ 0, 0, "-", 0, 0, 0, 0, "_" },			// s
	{ 0, 0, "5", 0, 0, 0, 0, "%" },			// t
	{ 0, 0, "7", 0, 0, 0, 0, "&" },			// u
	{ 0, 0, ".", 0, 0, 0, 0, 0 },			// v
	{ 0, 0, "2", 0, 0, 0, 0, "@" },			// w
	{ 0, 0, ">", 0, 0, 0, 0, 0 },			// x
	{ 0, 0, "6", 0, 0, 0, 0, "^" },			// y
	{ 0, 0, "<", 0, 0, 0, 0, 0 },			// z
};
static const char* const kArrowCorners[8] = { 0, "\xe2\x86\x91", 0,
	"\xe2\x86\x90", "\xe2\x86\x92", 0, "\xe2\x86\x93", 0 };
// A press that moves this far picks a corner; the space bar moves the cursor
// one character each this far it slides.
#define kCornerSlop S(20)
#define kSlideStep S(24)

// The globe key's menu: a row per layout above the key, then Emoji, then
// Settings below a separator.
#define kMenuWidth S(320)
// small enough for the menu to fit above the globe key on the English layout
#define kMenuRowHeight S(54)
#define kMenuSettingsHeight S(50)
#define kMenuPadding S(6)
#define kMenuSeparator S(9)
static const int32 kLayoutCount = sizeof(kLayouts) / sizeof(kLayouts[0]);
// after the layouts
static const int32 kMenuEmojiRow = kLayoutCount;
static const int32 kMenuSettingsRow = kLayoutCount + 1;

// The emoji bar's key for recent emoji: a stopwatch.
static const char* kRecentIcon = "\xe2\x8f\xb1\xef\xb8\x8f";
// A finger that moves this far across the emoji scrolls them instead.
#define kDragSlop S(20)

// Swipe typing. A press on a letter that moves this far is a swipe.
#define kSwipeStart S(30)
// The trail, with the widths and timings of AOSP's LatinIME: it thins from
// head to tail and fades from 100 ms old, gone at 900 ms.
#define kTrailStartWidth S(20)
#define kTrailEndWidth S(5)
static const bigtime_t kTrailFadeDelay = 100000;
static const bigtime_t kTrailFadeDuration = 800000;
static const bigtime_t kTrailFrame = 20000;
// How often the strip follows the finger.
static const bigtime_t kDecodeInterval = 50000;


// The key codes of the US layout, so that applications that look at "key"
// (shortcuts) get one.
static int32
key_code(char c)
{
	static const struct { const char* keys; int32 first; } kRows[] = {
		{ "1234567890", 0x12 }, { "qwertyuiop", 0x27 }, { "asdfghjkl", 0x3c },
		{ "zxcvbnm", 0x4c }
	};
	c = tolower(c);
	for (const auto& row : kRows) {
		if (const char* at = strchr(row.keys, c))
			return row.first + (at - row.keys);
	}
	return c == ' ' ? 0x5e : 0;
}


KeyboardView::KeyboardView(BRect frame)
	:
	BView(frame, "keyboard", B_FOLLOW_ALL, B_WILL_DRAW | B_FRAME_EVENTS),
	fShift(false),
	fSymbols(false),
	fLayout(0),
	fControl(false),
	fAlt(false),
	fPressed(-1),
	fMenu(false),
	fMenuPressed(-1),
	fEmoji(false),
	fPressedEmoji(NULL),
	fScrollStart(0),
	fPanelPressed(false),
	fDragging(false),
	fSwiping(false),
	fTrailRunner(NULL),
	fLastDecode(0),
	fSuggestionPressed(-1),
	fSpaceBefore(false),
	fCorner(-1),
	fSlideX(0),
	fSlid(false),
	fBuffer(NULL),
	fBufferView(NULL)
{
	// Draw() paints the background with the keys: app_server filling it
	// first would show as a flash under the trail on every frame.
	SetViewColor(B_TRANSPARENT_COLOR);
	_Layout();
}


KeyboardView::~KeyboardView()
{
	delete fTrailRunner;
	delete fBuffer;
}


void
KeyboardView::Draw(BRect updateRect)
{
	// Into a buffer first, then onto the screen in one go: drawn straight
	// there, the keys under the trail showed half drawn on every frame.
	if (fBuffer == NULL || fBuffer->Bounds() != Bounds()) {
		delete fBuffer;
		fBuffer = new BBitmap(Bounds(), B_BITMAP_ACCEPTS_VIEWS, B_RGB32);
		fBufferView = new BView(Bounds(), "buffer", B_FOLLOW_NONE, 0);
		fBuffer->AddChild(fBufferView);
		updateRect = Bounds();
	}
	if (!fBuffer->Lock())
		return;
	fBufferView->ConstrainClippingRegion(NULL);
	BRegion clip(updateRect);
	fBufferView->ConstrainClippingRegion(&clip);
	_Render(fBufferView, updateRect);
	fBufferView->Sync();
	fBuffer->Unlock();
	DrawBitmap(fBuffer, updateRect, updateRect);
}


void
KeyboardView::_Render(BView* view, BRect updateRect)
{
	view->SetHighColor(Theme::kPanel);
	view->FillRect(updateRect);
	_DrawSuggestions(view);
	if (fEmoji)
		fEmojiPanel.Draw(view, fPressedEmoji);

	BFont font(be_plain_font);
	font_height height;

	for (size_t i = 0; i < fKeys.size(); i++) {
		const Key& key = fKeys[i];
		bool modifier = key.special != kCharacter && key.special != kSpace;
		rgb_color fill = modifier ? Theme::kSurface : Theme::kSurfaceRaised;
		// the category in view, tinted as the menu's current row is
		if (key.special == kCategory && key.code == fEmojiPanel.Section())
			fill = mix_color(Theme::kSurface, Theme::kAccent, 90);
		if ((int32)i == fPressed || (_IsOn(key) && key.special != kShift))
			fill = Theme::kAccent;
		view->SetHighColor(fill);
		view->FillRoundRect(key.frame, S(8), S(8));

		float glyph = S(30);
		BRect icon(0, 0, glyph, glyph);
		icon.OffsetTo(key.frame.left + floorf((key.frame.Width() - glyph) / 2),
			key.frame.top + floorf((key.frame.Height() - glyph) / 2));
		switch (key.special) {
			case kShift:
				draw_glyph(view, kGlyphShift, icon, Theme::kText, fShift ? 1 : 0);
				continue;
			case kBackspace:
				draw_glyph(view, kGlyphBackspace, icon, Theme::kText);
				continue;
			case kReturn:
				draw_glyph(view, kGlyphReturn, icon, Theme::kText);
				continue;
			case kLayout:
				draw_glyph(view, kGlyphGlobe, icon, Theme::kText);
				continue;
			default:
				break;
		}

		char label[16];
		strlcpy(label, key.label, sizeof(label));
		if (key.special == kCharacter && fShift)
			label[0] = toupper(label[0]);
		// characters, arrows and emoji are drawn large, names small
		bool large = key.special == kCharacter || key.special == kCategory
			|| (key.special == kNamed && (uint8)label[0] >= 0x80);
		font.SetSize(F(large ? 26 : 18));
		view->SetFont(&font);
		font.GetHeight(&height);
		view->SetHighColor(key.special == kSpace ? Theme::kTextDim : Theme::kText);
		view->SetLowColor(fill);
		view->DrawString(label, BPoint(key.frame.left
				+ floorf((key.frame.Width() - font.StringWidth(label)) / 2),
			key.frame.top + floorf((key.frame.Height() + height.ascent
				- height.descent) / 2)));

		if (key.corners == NULL)
			continue;
		// the corner symbols, small; the one flicked to stands out
		font.SetSize(F(13));
		view->SetFont(&font);
		for (int32 c = 0; c < 8; c++) {
			const char* symbol = key.corners[c];
			if (symbol == NULL)
				continue;
			float width = font.StringWidth(symbol);
			float x = c == 0 || c == 3 || c == 5 ? key.frame.left + S(5)
				: c == 1 || c == 6
					? key.frame.left + floorf((key.frame.Width() - width) / 2)
				: key.frame.right - S(5) - width;
			float y = c < 3 ? key.frame.top + S(15) : c < 5
				? key.frame.top + floorf(key.frame.Height() / 2) + S(5)
				: key.frame.bottom - S(6);
			bool chosen = (int32)i == fPressed && c == fCorner;
			view->SetHighColor(chosen ? Theme::kText : Theme::kTextDim);
			view->DrawString(symbol, BPoint(x, y));
		}
	}

	_DrawTrail(view);
	if (fMenu)
		_DrawMenu(view);
}


void
KeyboardView::FrameResized(float width, float height)
{
	_Layout();
}


void
KeyboardView::MouseDown(BPoint where)
{
	if (fMenu) {
		fMenuPressed = _MenuRowAt(where);
		SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
		Invalidate();
		return;
	}

	if (where.y < kGap + Theme::SuggestionHeight()) {
		fSuggestionPressed = _SuggestionAt(where);
		SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
		Invalidate();
		return;
	}

	fPressed = _KeyAt(where);
	fDownPoint = where;
	fCorner = -1;
	fSlideX = where.x;
	fSlid = false;
	if (fEmoji && fPressed < 0 && fEmojiPanel.Frame().Contains(where)) {
		fPanelPressed = true;
		fDragging = false;
		fDragStart = where;
		fScrollStart = fEmojiPanel.Scroll();
		fPressedEmoji = fEmojiPanel.EmojiAt(where);
	}
	SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
	Invalidate();
}


void
KeyboardView::MouseMoved(BPoint where, uint32 transit,
	const BMessage* dragMessage)
{
	if (_Corners() && fPressed >= 0) {
		const Key& key = fKeys[fPressed];
		if (key.special == kSpace) {
			// the space bar slides the cursor, a character a step
			while (fabsf(where.x - fSlideX) >= kSlideStep) {
				bool right = where.x > fSlideX;
				char bytes[2] = { (char)(right ? B_RIGHT_ARROW : B_LEFT_ARROW),
					'\0' };
				_Send(bytes, right ? 0x63 : 0x61, bytes[0]);
				fSlideX += right ? kSlideStep : -kSlideStep;
				fSlid = true;
			}
			return;
		}
		int32 corner = key.corners != NULL ? _CornerAt(key, where) : -1;
		if (corner != fCorner) {
			fCorner = corner;
			Invalidate(key.frame);
		}
		return;
	}

	if (!fSwiping && fPressed >= 0 && _CanSwipe(fPressed)
		&& hypotf(where.x - fDownPoint.x, where.y - fDownPoint.y)
			> kSwipeStart) {
		// The key press becomes a swipe; the trail starts where it did.
		fSwiping = true;
		Invalidate(fKeys[fPressed].frame);
		fPressed = -1;
		fTrail.clear();
		fTrail.push_back({ fDownPoint, system_time() });
		fSwipePath.clear();
		fSwipePath.push_back({ fDownPoint.x, fDownPoint.y });
		if (fTrailRunner == NULL) {
			BMessage frame(kMsgAnimate);
			fTrailRunner = new BMessageRunner(BMessenger(this), &frame,
				kTrailFrame);
		}
	}
	if (fSwiping) {
		BRect before = _TrailBounds();
		fTrail.push_back({ where, system_time() });
		fSwipePath.push_back({ where.x, where.y });
		if (system_time() - fLastDecode > kDecodeInterval)
			_Decode();
		Invalidate(before | _TrailBounds());
		return;
	}

	if (!fPanelPressed)
		return;

	float dx = where.x - fDragStart.x;
	if (!fDragging && fabsf(dx) > kDragSlop) {
		fDragging = true;
		fPressedEmoji = NULL;
	}
	if (fDragging) {
		fEmojiPanel.SetScroll(fScrollStart - dx);
		Invalidate();
	}
}


void
KeyboardView::MouseUp(BPoint where)
{
	if (fMenu) {
		// A row acts when the finger lifts on it; a press outside the menu
		// only closes it.
		int32 row = _MenuRowAt(where);
		int32 pressed = fMenuPressed;
		fMenuPressed = -1;
		if (row >= 0 && row == pressed) {
			if (row < kLayoutCount)
				_SetLayout(row);
			else if (row == kMenuEmojiRow) {
				fEmoji = true;
				_Layout();
			} else
				be_app->PostMessage(kMsgShowSettings);
		}
		if (row < 0 || row == pressed)
			CloseMenu();
		Invalidate();
		return;
	}

	if (fSuggestionPressed >= 0) {
		// Swap in the word tapped for the one just swiped.
		int32 slot = _SuggestionAt(where);
		int32 index = slot == 1 ? 0 : slot == 0 ? 1 : 2;
		if (slot == fSuggestionPressed && index < (int32)fSuggestions.size()
			&& !fSwiped.IsEmpty()) {
			BString word = fSuggestions[index];
			if (isupper(fSwiped[0]))
				word.SetByteAt(0, toupper(word[0]));
			_Backspace(fSwiped.Length());
			_SendText(word);
			fSwiped = word;
		}
		fSuggestionPressed = -1;
		Invalidate();
		return;
	}

	if (fSwiping) {
		fSwiping = false;
		fTrail.push_back({ where, system_time() });
		fSwipePath.push_back({ where.x, where.y });
		_Decode();
		if (!fSuggestions.empty())
			_CommitSwipe(fSuggestions[0]);
		Invalidate();
		return;
	}

	if (fPanelPressed) {
		// A tap types the emoji and keeps it among the recent ones; a drag
		// comes to rest on a column.
		fPanelPressed = false;
		if (fDragging)
			fEmojiPanel.Snap();
		else if (fPressedEmoji != NULL
			&& fEmojiPanel.EmojiAt(where) == fPressedEmoji) {
			BString emoji(fPressedEmoji);
			_Send(emoji.String(), 0, 0);
			fEmojiPanel.AddRecent(emoji.String());
		}
		fPressedEmoji = NULL;
		Invalidate();
		return;
	}

	int32 pressed = fPressed;
	fPressed = -1;
	Invalidate();

	// A flick types the corner it went to, wherever the finger lifts; a
	// slide of the space bar has moved the cursor already.
	int32 corner = fCorner;
	fCorner = -1;
	if (pressed >= 0 && corner >= 0) {
		_SendSymbol(fKeys[pressed].corners[corner]);
		_EndWord();
		fShift = fControl = fAlt = false;
		return;
	}
	if (pressed >= 0 && fKeys[pressed].special == kSpace && fSlid)
		return;

	if (pressed < 0 || _KeyAt(where) != pressed)
		return;

	const Key& key = fKeys[pressed];
	bool typed = true;
	switch (key.special) {
		case kCharacter:
		{
			char c = fShift ? toupper(key.label[0]) : key.label[0];
			// a letter after a swiped word starts the next one
			if (fSpaceBefore && isalpha(c))
				_SendText(" ");
			char bytes[2] = { c, '\0' };
			// Control and a letter type its control character, as a real
			// keyboard does.
			if (fControl && isalpha(c))
				bytes[0] = tolower(c) & 0x1f;
			_Send(bytes, key_code(c), c);
			break;
		}
		case kSpace:
			_Send(" ", 0x5e, ' ');
			break;
		case kBackspace:
			// right after a swipe, the whole word goes
			_Backspace(fSwiped.IsEmpty() ? 1 : fSwiped.Length());
			break;
		case kReturn:
		{
			char bytes[2] = { B_RETURN, '\0' };
			_Send(bytes, 0x47, B_RETURN);
			break;
		}
		case kNamed:
		{
			char bytes[2] = { key.byte, '\0' };
			_Send(bytes, key.code, key.byte);
			break;
		}
		case kShift:
			fShift = !fShift;
			typed = false;
			break;
		case kControl:
			fControl = !fControl;
			typed = false;
			break;
		case kAlt:
			fAlt = !fAlt;
			typed = false;
			break;
		case kSymbols:
			fSymbols = !fSymbols;
			fShift = false;
			_Layout();
			typed = false;
			break;
		case kLayout:
			fMenu = true;
			typed = false;
			break;
		case kLetters:
			fEmoji = false;
			_Layout();
			typed = false;
			break;
		case kCategory:
			fEmojiPanel.JumpTo(key.code);
			typed = false;
			break;
		case kArrows:
			// it types only when flicked
			typed = false;
			break;
	}

	if (typed)
		_EndWord();

	// Shift, Control and Alt hold for one key.
	if (typed && (fShift || fControl || fAlt)) {
		fShift = fControl = fAlt = false;
		Invalidate();
	}
}


void
KeyboardView::Reset()
{
	CloseMenu();
	fShift = fControl = fAlt = false;
	_EndWord();
	// back to the letters: emoji are asked for, a field at a time
	if (fEmoji || fSymbols) {
		fEmoji = fSymbols = false;
		_Layout();
	}
	Invalidate();
}


void
KeyboardView::CloseMenu()
{
	if (!fMenu)
		return;
	fMenu = false;
	fMenuPressed = -1;
	Invalidate();
}


void
KeyboardView::_SetLayout(int32 layout)
{
	bool extras = kLayouts[fLayout].extras;
	fLayout = layout;
	float grow = (kLayouts[fLayout].extras ? 1 : 0) - (extras ? 1 : 0);
	if (grow == 0) {
		_Layout();
		return;
	}
	// The keyboard grows upwards, above the navigation panel, and the
	// applications move up with it; FrameResized() lays the keys out again.
	grow *= Theme::KeyboardExtrasHeight();
	Window()->MoveBy(0, -grow);
	Window()->ResizeBy(0, grow);
	be_app->PostMessage(kMsgKeyboardResized);
}


void
KeyboardView::_Layout()
{
	fKeys.clear();
	const char** rows = fSymbols ? kSymbolRows : kLetterRows;
	static char characters[3][16][2];
	bool extras = kLayouts[fLayout].extras;

	BRect bounds = Bounds();
	float unit = (bounds.Width() - kGap * 11) / 10;
	float y = kGap + Theme::SuggestionHeight();
	if (fEmoji) {
		// The emoji take the letters' place, whatever the layout; the bar of
		// categories takes the bottom row's.
		float top = y;
		y = bounds.bottom - kGap - kRowHeight;
		fEmojiPanel.SetFrame(BRect(kGap, top, bounds.right - kGap,
			y - kGap - 1));
	}
	for (int32 row = fEmoji ? 3 : extras ? -1 : 0; row < 4; row++) {
		std::vector<Key> keys;
		float height = kRowHeight;
		if (row < 0) {
			height = kExtrasRowHeight;
			keys = {
				{ "Esc", kNamed, 1, BRect(), B_ESCAPE, 0x01 },
				{ "Tab", kNamed, 1, BRect(), B_TAB, 0x26 },
				{ "Ctrl", kControl, 1, BRect(), 0, 0 },
				{ "Alt", kAlt, 1, BRect(), 0, 0 },
				{ "Home", kNamed, 1, BRect(), B_HOME, 0x20 },
				{ "End", kNamed, 1, BRect(), B_END, 0x35 },
				{ "\xe2\x86\x90", kNamed, 1, BRect(), B_LEFT_ARROW, 0x61 },
				{ "\xe2\x86\x91", kNamed, 1, BRect(), B_UP_ARROW, 0x57 },
				{ "\xe2\x86\x93", kNamed, 1, BRect(), B_DOWN_ARROW, 0x62 },
				{ "\xe2\x86\x92", kNamed, 1, BRect(), B_RIGHT_ARROW, 0x63 }
			};
		} else if (row < 3) {
			if (row == 2) {
				keys.push_back({ "", fSymbols ? kCharacter : kShift, 1.5f,
					BRect(), 0, 0 });
			}
			for (int32 i = 0; rows[row][i] != '\0'; i++) {
				characters[row][i][0] = rows[row][i];
				characters[row][i][1] = '\0';
				keys.push_back({ characters[row][i], kCharacter, 1, BRect(), 0,
					0 });
				if (_Corners() && !fSymbols)
					keys.back().corners = kCorners[rows[row][i] - 'a'];
			}
			if (row == 2)
				keys.push_back({ "", kBackspace, 1.5f, BRect(), 0, 0 });
			// The symbols layout has no shift: give its first key a symbol.
			if (row == 2 && fSymbols)
				keys[0].label = "=";
		} else if (fEmoji) {
			keys.push_back({ "ABC", kLetters, 1.5f, BRect(), 0, 0 });
			keys.push_back({ kRecentIcon, kCategory, 0.8f, BRect(), 0,
				EmojiPanel::kRecent });
			for (int32 i = 0; i < kEmojiCategoryCount; i++) {
				keys.push_back({ kEmojiCategories[i].icon, kCategory, 0.8f,
					BRect(), 0, i });
			}
			keys.push_back({ "", kBackspace, 1.5f, BRect(), 0, 0 });
		} else if (_Corners()) {
			// no symbols page: the symbols are in the corners
			keys = {
				{ "Ctrl", kControl, 1.2f, BRect(), 0, 0 },
				{ "Alt", kAlt, 1, BRect(), 0, 0 },
				{ "", kLayout, 1, BRect(), 0, 0 },
				{ kLayouts[fLayout].name, kSpace, 3.6f, BRect(), 0, 0 },
				{ "", kArrows, 1, BRect(), 0, 0, kArrowCorners },
				{ "", kReturn, 1.7f, BRect(), 0, 0 }
			};
		} else {
			keys = {
				{ fSymbols ? "ABC" : "?123", kSymbols, 1.5f, BRect(), 0, 0 },
				{ "", kLayout, 1, BRect(), 0, 0 },
				{ ",", kCharacter, 1, BRect(), 0, 0 },
				{ kLayouts[fLayout].name, kSpace, 3.5f, BRect(), 0, 0 },
				{ ".", kCharacter, 1, BRect(), 0, 0 },
				{ "", kReturn, 1.5f, BRect(), 0, 0 }
			};
		}

		float total = 0;
		for (const Key& key : keys)
			total += key.width * unit + kGap;
		float x = floorf((bounds.Width() - total + kGap) / 2);
		for (Key& key : keys) {
			float width = floorf(key.width * unit);
			key.frame = BRect(x, y, x + width, y + height);
			x += width + kGap;
			fKeys.push_back(key);
		}
		y += height + kGap;
	}

	fDecoder.SetKeyWidth(unit);
	fDecoder.SetRowHeight(kRowHeight + kGap);
	for (const Key& key : fKeys) {
		if (key.special == kCharacter && isalpha(key.label[0])) {
			BPoint center((key.frame.left + key.frame.right) / 2,
				(key.frame.top + key.frame.bottom) / 2);
			fDecoder.SetKey(key.label[0], { center.x, center.y });
		}
	}
	Invalidate();
}


void
KeyboardView::_Send(const char* bytes, int32 code, char raw)
{
	port_id port = find_port(MOBILE_KEYBOARD_PORT);
	if (port < 0)
		return;

	mobile_key key = {};
	key.key = code;
	if (fShift)
		key.modifiers |= B_SHIFT_KEY | B_LEFT_SHIFT_KEY;
	if (fControl)
		key.modifiers |= B_CONTROL_KEY | B_LEFT_CONTROL_KEY;
	if (fAlt)
		key.modifiers |= B_COMMAND_KEY | B_LEFT_COMMAND_KEY;
	strlcpy(key.bytes, bytes, sizeof(key.bytes));
	key.raw = raw;
	write_port_etc(port, kMobileKey, &key, sizeof(key), B_RELATIVE_TIMEOUT,
		100000);
}


void
KeyboardView::MessageReceived(BMessage* message)
{
	if (message->what != kMsgAnimate) {
		BView::MessageReceived(message);
		return;
	}

	// The trail's next frame: what has faded away goes.
	BRect before = _TrailBounds();
	bigtime_t gone = system_time() - kTrailFadeDelay - kTrailFadeDuration;
	while (!fTrail.empty() && fTrail.front().when < gone)
		fTrail.erase(fTrail.begin());
	if (fTrail.empty() && !fSwiping) {
		delete fTrailRunner;
		fTrailRunner = NULL;
	}
	Invalidate(before);
}


void
KeyboardView::_SendText(const char* text)
{
	for (const char* c = text; *c != '\0'; c++) {
		char bytes[2] = { *c, '\0' };
		_Send(bytes, key_code(*c), *c);
	}
}


void
KeyboardView::_Backspace(int32 count)
{
	char bytes[2] = { B_BACKSPACE, '\0' };
	for (int32 i = 0; i < count; i++)
		_Send(bytes, 0x1e, B_BACKSPACE);
}


bool
KeyboardView::_CanSwipe(int32 key) const
{
	return !fSymbols && !fEmoji && !_Corners()
		&& fKeys[key].special == kCharacter && isalpha(fKeys[key].label[0]);
}


void
KeyboardView::_Decode()
{
	// The trail fades as the finger goes on, so a slow swipe's start is
	// kept apart from it.
	fSuggestions.clear();
	for (const char* word : fDecoder.Decode(fSwipePath, 3))
		fSuggestions.push_back(word);
	fLastDecode = system_time();
	Invalidate(BRect(Bounds().left, 0, Bounds().right,
		kGap + Theme::SuggestionHeight()));
}


void
KeyboardView::_CommitSwipe(const BString& swiped)
{
	// Shift, and nothing else, holds for a swiped word as for a key.
	BString word = swiped;
	if (fShift)
		word.SetByteAt(0, toupper(word[0]));
	fShift = fControl = fAlt = false;

	if (fSpaceBefore)
		_SendText(" ");
	_SendText(word);
	fSwiped = word;
	fSpaceBefore = true;
}


/*!	Anything typed but a swipe ends the swiped word: it can't be swapped or
	taken out whole any more, and it wants no space after it.
*/
void
KeyboardView::_EndWord()
{
	fSwiped = "";
	fSpaceBefore = false;
	if (!fSuggestions.empty()) {
		fSuggestions.clear();
		Invalidate();
	}
}


/*!	Three slots across the strip, the best word in the middle. */
BRect
KeyboardView::_SuggestionFrame(int32 slot) const
{
	BRect bounds = Bounds();
	float width = floorf((bounds.Width() - kGap * 2) / 3);
	float left = kGap + slot * width;
	return BRect(left, floorf(kGap / 2), left + width - 1,
		floorf(kGap / 2) + Theme::SuggestionHeight() - 1);
}


int32
KeyboardView::_SuggestionAt(BPoint where) const
{
	for (int32 slot = 0; slot < 3; slot++) {
		if (_SuggestionFrame(slot).Contains(where))
			return slot;
	}
	return -1;
}


void
KeyboardView::_DrawSuggestions(BView* view)
{
	BFont font(be_plain_font);
	font.SetSize(F(20));
	font_height height;
	font.GetHeight(&height);

	for (int32 slot = 0; slot < 3; slot++) {
		BRect frame = _SuggestionFrame(slot);
		if (slot == fSuggestionPressed) {
			view->SetHighColor(Theme::kSurface);
			view->FillRoundRect(frame, S(8), S(8));
		}
		if (slot > 0) {
			view->SetHighColor(Theme::kDisabled);
			view->StrokeLine(BPoint(frame.left, frame.top + S(12)),
				BPoint(frame.left, frame.bottom - S(12)));
		}

		int32 index = slot == 1 ? 0 : slot == 0 ? 1 : 2;
		if (index >= (int32)fSuggestions.size())
			continue;
		BFont face(index == 0 ? be_bold_font : be_plain_font);
		face.SetSize(F(20));
		view->SetFont(&face);
		view->SetHighColor(index == 0 ? Theme::kText : Theme::kTextDim);
		view->SetLowColor(slot == fSuggestionPressed ? Theme::kSurface
			: Theme::kPanel);
		const char* word = fSuggestions[index].String();
		view->DrawString(word, BPoint(
			frame.left + floorf((frame.Width() - face.StringWidth(word)) / 2),
			frame.top + floorf((frame.Height() + height.ascent
				- height.descent) / 2)));
	}
}


/*!	The trail as a strip of quads, each as wide and opaque as its start point
	is young.
*/
void
KeyboardView::_DrawTrail(BView* view)
{
	if (fTrail.size() < 2)
		return;

	bigtime_t now = system_time();
	auto width = [now](bigtime_t when) {
		float age = std::min(1.0f, (now - when)
			/ (float)(kTrailFadeDelay + kTrailFadeDuration));
		return kTrailStartWidth - (kTrailStartWidth - kTrailEndWidth) * age;
	};
	auto alpha = [now](bigtime_t when) {
		bigtime_t fading = now - when - kTrailFadeDelay;
		return (uint8)std::clamp(255 - 255 * fading / kTrailFadeDuration,
			(bigtime_t)0, (bigtime_t)255);
	};

	view->PushState();
	view->SetDrawingMode(B_OP_ALPHA);
	view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
	rgb_color color = Theme::kAccent;
	for (size_t i = 1; i < fTrail.size(); i++) {
		BPoint a = fTrail[i - 1].where, b = fTrail[i].where;
		float length = hypotf(b.x - a.x, b.y - a.y);
		if (length < 0.5f)
			continue;
		BPoint normal(-(b.y - a.y) / length, (b.x - a.x) / length);
		float widthA = width(fTrail[i - 1].when) / 2;
		float widthB = width(fTrail[i].when) / 2;
		BPoint quad[] = {
			BPoint(a.x + normal.x * widthA, a.y + normal.y * widthA),
			BPoint(b.x + normal.x * widthB, b.y + normal.y * widthB),
			BPoint(b.x - normal.x * widthB, b.y - normal.y * widthB),
			BPoint(a.x - normal.x * widthA, a.y - normal.y * widthA) };
		color.alpha = alpha(fTrail[i - 1].when);
		view->SetHighColor(color);
		view->FillPolygon(quad, 4);
		// round the joint, so a turn leaves no notch; not once it fades,
		// where the overlap would show as a bead
		if (color.alpha == 255)
			view->FillEllipse(b, widthB, widthB);
	}
	view->PopState();
}


BRect
KeyboardView::_TrailBounds() const
{
	BRect bounds;
	for (const TrailPoint& point : fTrail)
		bounds = bounds | BRect(point.where, point.where);
	return bounds.InsetByCopy(-kTrailStartWidth, -kTrailStartWidth);
}


int32
KeyboardView::_KeyAt(BPoint where) const
{
	for (size_t i = 0; i < fKeys.size(); i++) {
		if (fKeys[i].frame.InsetByCopy(-kGap / 2, -kGap / 2).Contains(where))
			return i;
	}
	return -1;
}


/*!	Above the globe key, centred on it, and kept inside the keyboard. */
BRect
KeyboardView::_MenuFrame() const
{
	BRect globe;
	for (const Key& key : fKeys) {
		if (key.special == kLayout)
			globe = key.frame;
	}

	float height = kMenuPadding * 2 + kMenuSettingsRow * kMenuRowHeight
		+ kMenuSeparator + kMenuSettingsHeight;
	BRect bounds = Bounds();
	float left = std::clamp(globe.left + (globe.Width() - kMenuWidth) / 2,
		kGap, bounds.right - kGap - kMenuWidth);
	float top = std::max(kGap, globe.top - kGap - height);
	return BRect(left, top, left + kMenuWidth, top + height);
}


/*!	The layouts' rows, then Emoji, then Settings below a separator. */
BRect
KeyboardView::_MenuRowFrame(int32 row) const
{
	BRect menu = _MenuFrame().InsetByCopy(kMenuPadding, kMenuPadding);
	float top = menu.top + row * kMenuRowHeight;
	float height = kMenuRowHeight;
	if (row == kMenuSettingsRow) {
		top += kMenuSeparator;
		height = kMenuSettingsHeight;
	}
	return BRect(menu.left, top, menu.right, top + height - 1);
}


int32
KeyboardView::_MenuRowAt(BPoint where) const
{
	for (int32 row = 0; row <= kMenuSettingsRow; row++) {
		if (_MenuRowFrame(row).Contains(where))
			return row;
	}
	return -1;
}


void
KeyboardView::_DrawMenu(BView* view)
{
	BRect menu = _MenuFrame();
	view->SetHighColor(Theme::kSurface);
	view->FillRoundRect(menu, S(10), S(10));
	view->SetHighColor(Theme::kDisabled);
	view->StrokeRoundRect(menu, S(10), S(10));

	// The layout in use is tinted with the accent and outlined; the row
	// under the finger is tinted more.
	rgb_color tint = mix_color(Theme::kSurface, Theme::kAccent, 70);
	rgb_color pressed = mix_color(Theme::kSurface, Theme::kAccent, 140);

	BFont font(be_plain_font);
	font_height height;
	for (int32 row = 0; row <= kMenuSettingsRow; row++) {
		BRect frame = _MenuRowFrame(row);
		bool current = row == fLayout;
		if (row == fMenuPressed || current) {
			view->SetHighColor(row == fMenuPressed ? pressed : tint);
			view->FillRoundRect(frame, S(6), S(6));
		}
		if (current) {
			view->SetHighColor(Theme::kAccent);
			view->StrokeRoundRect(frame, S(6), S(6));
		}
		view->SetLowColor(row == fMenuPressed ? pressed : current ? tint
			: Theme::kSurface);

		if (row == kMenuSettingsRow) {
			view->SetHighColor(Theme::kDisabled);
			float line = frame.top - kMenuSeparator / 2;
			view->StrokeLine(BPoint(frame.left + S(8), line),
				BPoint(frame.right - S(8), line));

			BRect icon(frame.left + S(14),
				frame.top + floorf((frame.Height() - S(26)) / 2),
				frame.left + S(40),
				frame.top + floorf((frame.Height() + S(26)) / 2));
			draw_glyph(view, kGlyphGear, icon, Theme::kText);
			font.SetSize(F(20));
			view->SetFont(&font);
			font.GetHeight(&height);
			view->SetHighColor(Theme::kText);
			view->DrawString("Settings", BPoint(icon.right + S(14), frame.top
				+ floorf((frame.Height() + height.ascent - height.descent)
					/ 2)));
			continue;
		}

		bool emoji = row == kMenuEmojiRow;
		font.SetSize(F(22));
		view->SetFont(&font);
		view->SetHighColor(Theme::kText);
		view->DrawString(emoji ? "Emoji" : kLayouts[row].name,
			BPoint(frame.left + S(14), frame.top + S(24)));
		font.SetSize(F(16));
		view->SetFont(&font);
		view->SetHighColor(Theme::kTextDim);
		view->DrawString(emoji ? "Smileys, animals, food and more"
			: kLayouts[row].description, BPoint(frame.left + S(14),
			frame.top + S(45)));
	}
}


bool
KeyboardView::_Corners() const
{
	return kLayouts[fLayout].corners && !fEmoji;
}


/*!	The corner of \a key the finger has flicked towards: of those it has, the
	one nearest the direction, within a quarter turn. -1 for none yet.
*/
int32
KeyboardView::_CornerAt(const Key& key, BPoint where) const
{
	float dx = where.x - fDownPoint.x, dy = where.y - fDownPoint.y;
	if (hypotf(dx, dy) < kCornerSlop)
		return -1;
	float angle = atan2f(-dy, dx) * 180 / M_PI;
	// compass order's directions: nw, n, ne, w, e, sw, s, se
	static const float kAngles[8] = { 135, 90, 45, 180, 0, 225, 270, 315 };
	int32 best = -1;
	float bestOff = 90;
	for (int32 c = 0; c < 8; c++) {
		if (key.corners[c] == NULL)
			continue;
		float off = fabsf(fmodf(angle - kAngles[c] + 540, 360) - 180);
		if (off < bestOff) {
			bestOff = off;
			best = c;
		}
	}
	return best;
}


/*!	A corner's symbol: Esc, Tab and the arrows as those keys, the rest as
	the character.
*/
void
KeyboardView::_SendSymbol(const char* symbol)
{
	static const struct { const char* name; char byte; int32 code; } kKeys[] = {
		{ "Esc", B_ESCAPE, 0x01 }, { "Tab", B_TAB, 0x26 },
		{ "\xe2\x86\x91", B_UP_ARROW, 0x57 },
		{ "\xe2\x86\x90", B_LEFT_ARROW, 0x61 },
		{ "\xe2\x86\x92", B_RIGHT_ARROW, 0x63 },
		{ "\xe2\x86\x93", B_DOWN_ARROW, 0x62 }
	};
	for (const auto& key : kKeys) {
		if (strcmp(symbol, key.name) == 0) {
			char bytes[2] = { key.byte, '\0' };
			_Send(bytes, key.code, key.byte);
			return;
		}
	}
	_Send(symbol, key_code(symbol[0]), symbol[0]);
}


bool
KeyboardView::_IsOn(const Key& key) const
{
	switch (key.special) {
		case kShift:
			return fShift;
		case kControl:
			return fControl;
		case kAlt:
			return fAlt;
		default:
			return false;
	}
}
