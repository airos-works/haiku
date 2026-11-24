/*
 * Copyright 2026, air/OS. Distributed under the MIT License.
 */
#ifndef _MOBILE_KIT_H
#define _MOBILE_KIT_H

#include <Bitmap.h>
#include <Button.h>
#include <CardLayout.h>
#include <CardView.h>
#include <CheckBox.h>
#include <Control.h>
#include <Font.h>
#include <GradientLinear.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <LayoutUtils.h>
#include <ListView.h>
#include <Message.h>
#include <Resources.h>
#include <Messenger.h>
#include <Window.h>
#include <ScrollView.h>
#include <Region.h>
#include <Shape.h>
#include <Slider.h>
#include <String.h>
#include <StringItem.h>
#include <TextControl.h>
#include <TextView.h>

#include <MessageFilter.h>
#include <MessageRunner.h>
#include <ScrollBar.h>
#include <math.h>
#include <string.h>

#include <algorithm>
#include <vector>


// 1: presentation switching and native touch controls.
// 2: the skeuomorphic materials and controls (MetalBar, SegmentedControl, ...).
// 3: per-window skins, browser and list glyphs, plain bar buttons with a count,
//    bar progress, TableView, CardGrid, TabBar, TitleView, Toast and a
//    share-sheet look for ActionSheet (activities, destructive actions).
#define B_MOBILE_KIT_VERSION 3


namespace BMobileKit {

// Sent to each adopted window before the shell fits or restores its frame.
// A floating primary window replies with "primary" = true. Palettes and
// auxiliary windows keep their regular window-manager semantics.
const uint32 B_MOBILE_WINDOW_QUERY = 'mVwq';
const uint32 B_MOBILE_VIEW_CHANGED = 'mVch'; // "mobile" (bool)

// In a primary window constructor, after building the desktop layout and
// before Show(). The shell records that frame before the first mobile draw.
// The system file panel supplies its own tablet UI regardless of its host.
template<class ChangeView>
inline void
PrepareWindow(BWindow* window, ChangeView change, bool filePanel = false)
{
	BMessenger shell("application/x-vnd.airOS-MobileShell");
	if (!shell.IsValid())
		return;
	BMessage request('mVpr'), reply;
	if (filePanel)
		request.AddBool("file panel", true);
	request.AddMessenger("window", BMessenger(window));
	request.AddRect("frame", window->Frame());
	request.AddInt32("look", window->Look());
	BRect area;
	if (shell.SendMessage(&request, &reply, 250000, 500000) != B_OK
		|| reply.FindRect("area", &area) != B_OK || !area.IsValid())
		return;
	change(true);
	window->SetLook(B_NO_BORDER_WINDOW_LOOK);
	window->MoveTo(area.LeftTop());
	window->ResizeTo(area.Width(), area.Height());
}


// Resource compilers can emit the same declaration from an .rdef file.
inline status_t
DeclareSupport(BResources& resources)
{
	const uint8 version = 1;
	return resources.AddResource('MOBV', 1, &version, 1, "Mobile Kit");
}


inline bool
SupportsMobileView(const entry_ref& ref)
{
	BFile file(&ref, B_READ_ONLY);
	BResources resources(&file);
	size_t size = 0;
	const uint8* version = (const uint8*)resources.LoadResource('MOBV',
		(int32)1, &size);
	return version != NULL && size == 1 && *version == 1;
}


// Layout coordinates already reflect the screen's interface scale.
inline float
TargetSize()
{
	return ceilf(be_plain_font->Size() * 3.75f);
}


template<class Control>
inline Control*
TouchTarget(Control* control)
{
	BSize size = control->MinSize();
	float target = TargetSize();
	float height = fmaxf(size.height, target);
	control->SetExplicitMinSize(BSize(fmaxf(size.width, target), height));
	BSize maximum = control->MaxSize();
	control->SetExplicitMaxSize(BSize(fmaxf(maximum.width, target),
		fmaxf(maximum.height, height)));
	return control;
}


inline BButton*
Action(const char* name, const char* label, BMessage* message)
{
	return TouchTarget(new BButton(name, label, message));
}


inline BTextControl*
TextField(const char* name, const char* label, const char* text,
	BMessage* submit)
{
	return TouchTarget(new BTextControl(name, label, text, submit));
}


inline BCheckBox*
Toggle(const char* name, const char* label, BMessage* changed)
{
	return TouchTarget(new BCheckBox(name, label, changed));
}


inline BSlider*
Slider(const char* name, const char* label, BMessage* changed,
	int32 minimum, int32 maximum)
{
	return TouchTarget(new BSlider(name, label, changed, minimum, maximum,
		B_HORIZONTAL));
}


// App bars, bottom navigation and grouped actions accept ordinary BViews.
class Bar : public BGroupView {
public:
	explicit Bar(const char* name)
		:
		BGroupView(name, B_HORIZONTAL, B_USE_HALF_ITEM_SPACING)
	{
		SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
		GroupLayout()->SetInsets(B_USE_HALF_ITEM_SPACING);
		SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
	}

	BButton* AddAction(const char* name, const char* label, BMessage* message)
	{
		BButton* button = Action(name, label, message);
		GroupLayout()->AddView(button);
		return button;
	}
};


// A stack of pages for drill-down navigation. Push transfers ownership on
// success. Back returns to the previous live view and destroys the popped page.
class NavigationView : public BCardView {
public:
	explicit NavigationView(const char* name) : BCardView(name) {}

	bool Push(BView* page)
	{
		BCardLayout* layout = CardLayout();
		BLayoutItem* item = layout->AddView(page);
		if (item == NULL)
			return false;
		layout->SetVisibleItem(item);
		return true;
	}

	bool Back()
	{
		BCardLayout* layout = CardLayout();
		int32 index = layout->CountItems() - 1;
		if (index < 1)
			return false;
		BView* page = layout->ItemAt(index)->View();
		layout->SetVisibleItem(index - 1);
		layout->RemoveView(page);
		delete page;
		return true;
	}
};


class ListItem : public BStringItem {
public:
	explicit ListItem(const char* text) : BStringItem(text) {}

	virtual void Update(BView* owner, const BFont* font)
	{
		BStringItem::Update(owner, font);
		SetHeight(fmaxf(Height(), TargetSize()));
	}
};


inline BScrollView*
Scroll(const char* name, BView* content)
{
	return new BScrollView(name, content, 0, false, true, B_NO_BORDER);
}


// #pragma mark - Skeuomorphic materials (version 2)

// An optional look for touch layouts that imitates objects: brushed metal bars,
// an inset LCD display, glossy segmented buttons, a pill search field, wooden
// rails along the sides of the content and a paper page under grids. The
// materials follow the panel colour: aluminium on a light panel, graphite on a
// dark one. Every control below draws the metal of the MetalBar it sits in,
// so a bar and its controls read as one piece.

struct Skin {
	rgb_color barTop, barBottom, barHighlight, barEdge;
	rgb_color controlTop, controlBottom, controlBorder, controlText;
	rgb_color controlEmboss;		// the highlight under engraved text (alpha)
	rgb_color pressedTop, pressedBottom;
	rgb_color selectedTop, selectedBottom, selectedText, selectedEmboss;
	rgb_color displayTop, displayBottom, displayBorder, displayText;
	rgb_color displaySecondary, displayActive;
	rgb_color trackTop, trackBottom, trackFill, playhead;
	rgb_color fillTop, fillBottom;	// the filled part of a level slider
	rgb_color knobTop, knobBottom, knobBorder;
	rgb_color fieldBackground, fieldBorder, fieldText, fieldPlaceholder;
	rgb_color pageTop, pageBottom, pageText, pageSecondary;
	rgb_color woodLight, woodDark;
	rgb_color stage;				// behind a large cover or picture
};


namespace Detail {

// "be:wheel_phase" (input_wheel_phase in Input.h, which older SDKs lack).
enum {
	kWheelPhaseNone = 0,
	kWheelPhaseBegan = 1,
	kWheelPhaseChanged = 2,
	kWheelPhaseEnded = 3
};


inline rgb_color
MakeColor(uint8 red, uint8 green, uint8 blue, uint8 alpha = 255)
{
	rgb_color color = { red, green, blue, alpha };
	return color;
}


inline Skin
AluminiumSkin()
{
	Skin skin;
	skin.barTop = MakeColor(230, 231, 233);
	skin.barBottom = MakeColor(168, 170, 175);
	skin.barHighlight = MakeColor(255, 255, 255, 210);
	skin.barEdge = MakeColor(82, 83, 87);
	skin.controlTop = MakeColor(251, 251, 252);
	skin.controlBottom = MakeColor(207, 208, 212);
	skin.controlBorder = MakeColor(112, 113, 118);
	skin.controlText = MakeColor(62, 64, 69);
	skin.controlEmboss = MakeColor(255, 255, 255, 190);
	skin.pressedTop = MakeColor(160, 162, 168);
	skin.pressedBottom = MakeColor(198, 200, 205);
	skin.selectedTop = MakeColor(126, 128, 134);
	skin.selectedBottom = MakeColor(166, 168, 174);
	skin.selectedText = MakeColor(255, 255, 255);
	skin.selectedEmboss = MakeColor(0, 0, 0, 120);
	skin.displayTop = MakeColor(251, 252, 249);
	skin.displayBottom = MakeColor(220, 223, 217);
	skin.displayBorder = MakeColor(118, 119, 123);
	skin.displayText = MakeColor(24, 24, 26);
	skin.displaySecondary = MakeColor(92, 94, 98);
	skin.displayActive = MakeColor(36, 112, 226);
	skin.trackTop = MakeColor(142, 143, 147);
	skin.trackBottom = MakeColor(204, 205, 208);
	skin.trackFill = MakeColor(88, 90, 95);
	skin.playhead = MakeColor(244, 132, 24);
	skin.fillTop = MakeColor(70, 136, 232);
	skin.fillBottom = MakeColor(30, 92, 204);
	skin.knobTop = MakeColor(255, 255, 255);
	skin.knobBottom = MakeColor(196, 198, 203);
	skin.knobBorder = MakeColor(104, 105, 110);
	skin.fieldBackground = MakeColor(255, 255, 255);
	skin.fieldBorder = MakeColor(118, 119, 124);
	skin.fieldText = MakeColor(20, 20, 22);
	skin.fieldPlaceholder = MakeColor(150, 151, 155);
	skin.pageTop = MakeColor(248, 248, 249);
	skin.pageBottom = MakeColor(224, 225, 228);
	skin.pageText = MakeColor(22, 22, 24);
	skin.pageSecondary = MakeColor(108, 110, 115);
	skin.woodLight = MakeColor(170, 88, 50);
	skin.woodDark = MakeColor(104, 42, 20);
	skin.stage = MakeColor(0, 0, 0);
	return skin;
}


inline Skin
GraphiteSkin()
{
	Skin skin;
	skin.barTop = MakeColor(82, 84, 89);
	skin.barBottom = MakeColor(38, 39, 42);
	skin.barHighlight = MakeColor(255, 255, 255, 56);
	skin.barEdge = MakeColor(8, 8, 10);
	skin.controlTop = MakeColor(100, 102, 108);
	skin.controlBottom = MakeColor(58, 60, 64);
	skin.controlBorder = MakeColor(16, 16, 18);
	skin.controlText = MakeColor(228, 229, 232);
	skin.controlEmboss = MakeColor(0, 0, 0, 170);
	skin.pressedTop = MakeColor(38, 39, 42);
	skin.pressedBottom = MakeColor(60, 62, 66);
	skin.selectedTop = MakeColor(26, 27, 30);
	skin.selectedBottom = MakeColor(48, 50, 54);
	skin.selectedText = MakeColor(255, 255, 255);
	skin.selectedEmboss = MakeColor(0, 0, 0, 190);
	skin.displayTop = MakeColor(60, 64, 68);
	skin.displayBottom = MakeColor(34, 36, 40);
	skin.displayBorder = MakeColor(6, 6, 8);
	skin.displayText = MakeColor(238, 239, 240);
	skin.displaySecondary = MakeColor(162, 165, 170);
	skin.displayActive = MakeColor(96, 164, 255);
	skin.trackTop = MakeColor(20, 20, 23);
	skin.trackBottom = MakeColor(52, 53, 57);
	skin.trackFill = MakeColor(150, 153, 160);
	skin.playhead = MakeColor(250, 146, 40);
	skin.fillTop = MakeColor(84, 148, 240);
	skin.fillBottom = MakeColor(40, 100, 210);
	skin.knobTop = MakeColor(170, 172, 178);
	skin.knobBottom = MakeColor(98, 100, 105);
	skin.knobBorder = MakeColor(12, 12, 14);
	skin.fieldBackground = MakeColor(36, 37, 40);
	skin.fieldBorder = MakeColor(8, 8, 10);
	skin.fieldText = MakeColor(232, 232, 234);
	skin.fieldPlaceholder = MakeColor(128, 130, 135);
	skin.pageTop = MakeColor(46, 47, 50);
	skin.pageBottom = MakeColor(28, 29, 31);
	skin.pageText = MakeColor(236, 236, 238);
	skin.pageSecondary = MakeColor(158, 160, 165);
	skin.woodLight = MakeColor(104, 64, 40);
	skin.woodDark = MakeColor(50, 28, 16);
	skin.stage = MakeColor(0, 0, 0);
	return skin;
}

} // namespace Detail


inline bool
IsDarkPanel()
{
	rgb_color panel = ui_color(B_PANEL_BACKGROUND_COLOR);
	return (panel.red * 299 + panel.green * 587 + panel.blue * 114) / 1000 < 128;
}


// The material set that goes with the current panel colour. Views that draw
// with it repaint on B_COLORS_UPDATED to follow an Appearance change.
// The material set a window draws with: the panel's (the default), or always
// aluminium or graphite (a private browsing window, for example).
enum SkinStyle {
	B_PANEL_SKIN,
	B_ALUMINIUM_SKIN,
	B_GRAPHITE_SKIN
};


namespace Detail {

// Each window draws on its own thread, so the choice is kept per thread.
inline SkinStyle&
ThreadSkin()
{
	static thread_local SkinStyle style = B_PANEL_SKIN;
	return style;
}


inline void
InvalidateTree(BView* view)
{
	for (; view != NULL; view = view->NextSibling()) {
		view->Invalidate();
		InvalidateTree(view->ChildAt(0));
	}
}


class SkinFilter : public BMessageFilter {
public:
	SkinFilter() : BMessageFilter('mVsk') {}

	virtual filter_result Filter(BMessage* message, BHandler** target)
	{
		ThreadSkin() = (SkinStyle)message->GetInt32("style", B_PANEL_SKIN);
		if (BWindow* window = dynamic_cast<BWindow*>(Looper()))
			InvalidateTree(window->ChildAt(0));
		return B_SKIP_MESSAGE;
	}
};

} // namespace Detail


// Gives the window's kit views another material set. Call it with the window
// locked (or before it is shown); the change applies on the window's thread,
// before its next update.
inline void
SetWindowSkin(BWindow* window, SkinStyle style)
{
	bool installed = false;
	BList* filters = window->CommonFilterList();
	for (int32 i = 0; filters != NULL && i < filters->CountItems(); i++) {
		if (dynamic_cast<Detail::SkinFilter*>(
				(BMessageFilter*)filters->ItemAt(i)) != NULL)
			installed = true;
	}
	if (!installed)
		window->AddCommonFilter(new Detail::SkinFilter);
	BMessage message('mVsk');
	message.AddInt32("style", style);
	window->PostMessage(&message);
}


inline const Skin&
CurrentSkin()
{
	static const Skin aluminium = Detail::AluminiumSkin();
	static const Skin graphite = Detail::GraphiteSkin();
	switch (Detail::ThreadSkin()) {
		case B_ALUMINIUM_SKIN:
			return aluminium;
		case B_GRAPHITE_SKIN:
			return graphite;
		default:
			return IsDarkPanel() ? graphite : aluminium;
	}
}


// Whether the kit's views in this window draw graphite.
inline bool
IsDarkSkin()
{
	return Detail::ThreadSkin() == B_GRAPHITE_SKIN
		|| (Detail::ThreadSkin() == B_PANEL_SKIN && IsDarkPanel());
}


inline rgb_color
MixColors(rgb_color from, rgb_color to, float amount)
{
	amount = fmaxf(0.0f, fminf(1.0f, amount));
	rgb_color result;
	result.red = (uint8)(from.red + (to.red - from.red) * amount + 0.5f);
	result.green = (uint8)(from.green + (to.green - from.green) * amount + 0.5f);
	result.blue = (uint8)(from.blue + (to.blue - from.blue) * amount + 0.5f);
	result.alpha = (uint8)(from.alpha + (to.alpha - from.alpha) * amount + 0.5f);
	return result;
}


inline void
FillGradient(BView* view, BRect rect, rgb_color top, rgb_color bottom,
	float radius = 0)
{
	BGradientLinear gradient(rect.LeftTop(), rect.LeftBottom());
	gradient.AddColor(top, 0);
	gradient.AddColor(bottom, 255);
	if (radius > 0)
		view->FillRoundRect(rect, radius, radius, gradient);
	else
		view->FillRect(rect, gradient);
}


// Height of the visible part of a bar control. The control itself keeps the
// full TargetSize() for touch; the artwork sits centred inside it.
inline float
ControlHeight()
{
	return ceilf(be_plain_font->Size() * 2.6f);
}


// Text cut into the material: a highlight (or shadow) one pixel below it.
inline void
DrawEngraved(BView* view, const char* text, BPoint where, rgb_color color,
	rgb_color emboss)
{
	view->PushState();
	view->SetDrawingMode(B_OP_ALPHA);
	view->SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
	view->SetHighColor(emboss);
	view->DrawString(text, where + BPoint(0, 1));
	view->SetHighColor(color);
	view->DrawString(text, where);
	view->PopState();
}


// Draws a string centred in a rectangle, truncated to fit.
inline void
DrawCentered(BView* view, const char* text, BRect rect, rgb_color color,
	rgb_color emboss)
{
	BFont font;
	view->GetFont(&font);
	BString shown(text);
	font.TruncateString(&shown, B_TRUNCATE_END, rect.Width());
	font_height height;
	font.GetHeight(&height);
	float width = font.StringWidth(shown.String());
	BPoint where(floorf(rect.left + (rect.Width() - width) / 2),
		floorf((rect.top + rect.bottom - height.ascent - height.descent) / 2
			+ height.ascent));
	DrawEngraved(view, shown.String(), where, color, emboss);
}


// #pragma mark - Glyphs

// Vector symbols for transport, navigation and fields. They scale with the
// rectangle they are drawn in and need no font or resource.
enum Glyph {
	B_GLYPH_NONE = 0,
	B_GLYPH_PLAY,
	B_GLYPH_PAUSE,
	B_GLYPH_PREVIOUS,
	B_GLYPH_NEXT,
	B_GLYPH_REPEAT,
	B_GLYPH_REPEAT_ONE,
	B_GLYPH_SHUFFLE,
	B_GLYPH_BACK,
	B_GLYPH_SEARCH,
	B_GLYPH_CLEAR,
	B_GLYPH_SPEAKER,
	B_GLYPH_SPEAKER_LOUD,
	B_GLYPH_NOTE,
	// version 3
	B_GLYPH_ARROW_BACK,		// filled arrows for a browser's Back and Forward
	B_GLYPH_ARROW_FORWARD,
	B_GLYPH_RELOAD,
	B_GLYPH_STOP,
	B_GLYPH_ADD,
	B_GLYPH_PAGES,			// two pages, one over the other (open pages, tabs)
	B_GLYPH_BOOK,			// an open book (bookmarks)
	B_GLYPH_ACTION,			// an arrow out of a tray (share and actions)
	B_GLYPH_CLOCK,			// history
	B_GLYPH_FOLDER,
	B_GLYPH_STAR,
	B_GLYPH_GLASSES,		// reading list
	B_GLYPH_PRIVATE,		// a hat over glasses (private browsing)
	B_GLYPH_CHEVRON,		// a row's disclosure indicator
	B_GLYPH_CHECK,
	B_GLYPH_GLOBE,
	B_GLYPH_MORE,			// three dots
	B_GLYPH_LOCK,
	B_GLYPH_DOWNLOAD
};


// The front page of B_GLYPH_PAGES in a glyph drawn in rect, where a count
// of pages can be written.
inline BRect
PagesGlyphFace(BRect rect)
{
	float size = fminf(rect.Width(), rect.Height());
	float h = size / 2;
	BPoint c((rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2);
	return BRect(c.x - h * 0.86f, c.y - h * 0.46f, c.x + h * 0.46f,
		c.y + h * 0.86f);
}


inline void
DrawGlyph(BView* view, Glyph glyph, BRect rect, rgb_color color)
{
	float size = fminf(rect.Width(), rect.Height());
	float h = size / 2;
	BPoint c((rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2);
	float pen = fmaxf(1.0f, roundf(size * 0.11f));
	view->PushState();
	view->SetDrawingMode(B_OP_ALPHA);
	view->SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
	view->SetHighColor(color);
	view->SetLineMode(B_ROUND_CAP, B_ROUND_JOIN);
	view->SetPenSize(pen);
	switch (glyph) {
		case B_GLYPH_PLAY:
			view->FillTriangle(BPoint(c.x - h * 0.6f, c.y - h * 0.78f),
				BPoint(c.x - h * 0.6f, c.y + h * 0.78f),
				BPoint(c.x + h * 0.78f, c.y));
			break;
		case B_GLYPH_PAUSE:
		{
			float width = h * 0.46f, gap = h * 0.36f;
			view->FillRect(BRect(c.x - gap / 2 - width, c.y - h * 0.76f,
				c.x - gap / 2, c.y + h * 0.76f));
			view->FillRect(BRect(c.x + gap / 2, c.y - h * 0.76f,
				c.x + gap / 2 + width, c.y + h * 0.76f));
			break;
		}
		case B_GLYPH_PREVIOUS:
		case B_GLYPH_NEXT:
		{
			float direction = glyph == B_GLYPH_NEXT ? 1 : -1;
			float width = h * 0.84f, tall = h * 0.6f;
			for (int i = 0; i < 2; i++) {
				float base = c.x + direction * (i - 1) * width;
				view->FillTriangle(BPoint(base, c.y - tall),
					BPoint(base, c.y + tall),
					BPoint(base + direction * width, c.y));
			}
			break;
		}
		case B_GLYPH_REPEAT:
		case B_GLYPH_REPEAT_ONE:
		{
			BRect loop(c.x - h * 0.74f, c.y - h * 0.46f, c.x + h * 0.74f,
				c.y + h * 0.46f);
			view->StrokeRoundRect(loop, h * 0.36f, h * 0.36f);
			float arrow = fmaxf(pen * 1.3f, h * 0.3f);
			BPoint top(c.x + h * 0.08f, loop.top);
			view->FillTriangle(BPoint(top.x - arrow * 0.3f, top.y - arrow),
				BPoint(top.x - arrow * 0.3f, top.y + arrow),
				BPoint(top.x + arrow, top.y));
			BPoint bottom(c.x - h * 0.08f, loop.bottom);
			view->FillTriangle(BPoint(bottom.x + arrow * 0.3f, bottom.y - arrow),
				BPoint(bottom.x + arrow * 0.3f, bottom.y + arrow),
				BPoint(bottom.x - arrow, bottom.y));
			if (glyph == B_GLYPH_REPEAT_ONE) {
				view->SetPenSize(fmaxf(1.0f, pen * 0.8f));
				view->StrokeLine(BPoint(c.x, c.y - h * 0.22f),
					BPoint(c.x, c.y + h * 0.22f));
				view->StrokeLine(BPoint(c.x, c.y - h * 0.22f),
					BPoint(c.x - h * 0.12f, c.y - h * 0.1f));
			}
			break;
		}
		case B_GLYPH_SHUFFLE:
		{
			float left = c.x - h * 0.84f, right = c.x + h * 0.46f;
			float top = c.y - h * 0.46f, bottom = c.y + h * 0.46f;
			BShape down, up;
			BPoint downCurve[3] = { BPoint(c.x - h * 0.2f, top),
				BPoint(c.x - h * 0.1f, bottom), BPoint(right, bottom) };
			BPoint upCurve[3] = { BPoint(c.x - h * 0.2f, bottom),
				BPoint(c.x - h * 0.1f, top), BPoint(right, top) };
			down.MoveTo(BPoint(left, top));
			down.BezierTo(downCurve);
			up.MoveTo(BPoint(left, bottom));
			up.BezierTo(upCurve);
			view->MovePenTo(B_ORIGIN);
			// shapes are drawn relative to the pen
			view->StrokeShape(&down);
			view->StrokeShape(&up);
			float arrow = fmaxf(pen * 1.3f, h * 0.3f);
			for (int i = 0; i < 2; i++) {
				float y = i == 0 ? top : bottom;
				view->FillTriangle(BPoint(right - arrow * 0.3f, y - arrow),
					BPoint(right - arrow * 0.3f, y + arrow),
					BPoint(right + arrow, y));
			}
			break;
		}
		case B_GLYPH_BACK:
		{
			BPoint points[3] = { BPoint(c.x + h * 0.3f, c.y - h * 0.62f),
				BPoint(c.x - h * 0.3f, c.y), BPoint(c.x + h * 0.3f, c.y + h * 0.62f) };
			view->SetPenSize(fmaxf(1.0f, roundf(size * 0.14f)));
			view->StrokeLine(points[0], points[1]);
			view->StrokeLine(points[1], points[2]);
			break;
		}
		case B_GLYPH_SEARCH:
		{
			BPoint lens(c.x - h * 0.16f, c.y - h * 0.16f);
			float radius = h * 0.5f;
			view->StrokeEllipse(lens, radius, radius);
			view->SetPenSize(pen * 1.4f);
			view->StrokeLine(lens + BPoint(radius * 0.72f, radius * 0.72f),
				BPoint(c.x + h * 0.74f, c.y + h * 0.74f));
			break;
		}
		case B_GLYPH_CLEAR:
			view->StrokeLine(BPoint(c.x - h * 0.42f, c.y - h * 0.42f),
				BPoint(c.x + h * 0.42f, c.y + h * 0.42f));
			view->StrokeLine(BPoint(c.x + h * 0.42f, c.y - h * 0.42f),
				BPoint(c.x - h * 0.42f, c.y + h * 0.42f));
			break;
		case B_GLYPH_SPEAKER:
		case B_GLYPH_SPEAKER_LOUD:
		{
			float shift = glyph == B_GLYPH_SPEAKER ? h * 0.25f : 0;
			BPoint cone[6] = {
				BPoint(c.x - h * 0.8f + shift, c.y - h * 0.26f),
				BPoint(c.x - h * 0.42f + shift, c.y - h * 0.26f),
				BPoint(c.x + h * 0.02f + shift, c.y - h * 0.66f),
				BPoint(c.x + h * 0.02f + shift, c.y + h * 0.66f),
				BPoint(c.x - h * 0.42f + shift, c.y + h * 0.26f),
				BPoint(c.x - h * 0.8f + shift, c.y + h * 0.26f) };
			view->FillPolygon(cone, 6);
			if (glyph == B_GLYPH_SPEAKER_LOUD) {
				view->SetPenSize(fmaxf(1.0f, pen * 0.8f));
				view->StrokeArc(BPoint(c.x + h * 0.02f, c.y), h * 0.38f,
					h * 0.38f, -50, 100);
				view->StrokeArc(BPoint(c.x + h * 0.02f, c.y), h * 0.72f,
					h * 0.72f, -50, 100);
			}
			break;
		}
		case B_GLYPH_NOTE:
		{
			BPoint head(c.x - h * 0.26f, c.y + h * 0.5f);
			view->FillEllipse(head, h * 0.34f, h * 0.26f);
			float stem = head.x + h * 0.3f;
			view->SetPenSize(fmaxf(1.0f, roundf(size * 0.08f)));
			view->StrokeLine(BPoint(stem, head.y - h * 0.05f),
				BPoint(stem, c.y - h * 0.82f));
			BShape flag;
			BPoint curve[3] = { BPoint(stem + h * 0.1f, c.y - h * 0.5f),
				BPoint(stem + h * 0.6f, c.y - h * 0.4f),
				BPoint(stem + h * 0.42f, c.y + h * 0.06f) };
			flag.MoveTo(BPoint(stem, c.y - h * 0.82f));
			flag.BezierTo(curve);
			view->MovePenTo(B_ORIGIN);
			view->StrokeShape(&flag);
			break;
		}
		case B_GLYPH_ARROW_BACK:
		case B_GLYPH_ARROW_FORWARD:
		{
			float d = glyph == B_GLYPH_ARROW_FORWARD ? 1 : -1;
			view->FillTriangle(BPoint(c.x + d * h * 0.62f, c.y),
				BPoint(c.x - d * h * 0.42f, c.y - h * 0.7f),
				BPoint(c.x - d * h * 0.42f, c.y + h * 0.7f));
			break;
		}
		case B_GLYPH_RELOAD:
		{
			float radius = h * 0.62f;
			view->SetPenSize(fmaxf(1.0f, roundf(size * 0.12f)));
			view->StrokeArc(c, radius, radius, 70, 290);
			// the arrowhead at the end of the arc, pointing on round
			float angle = 70 * M_PI / 180;
			BPoint tip(c.x + radius * cosf(angle), c.y - radius * sinf(angle));
			float arrow = h * 0.36f;
			view->FillTriangle(BPoint(tip.x - arrow * 0.9f, tip.y - arrow),
				BPoint(tip.x - arrow * 0.9f, tip.y + arrow),
				BPoint(tip.x + arrow * 0.8f, tip.y));
			break;
		}
		case B_GLYPH_STOP:
			view->SetPenSize(fmaxf(1.0f, roundf(size * 0.13f)));
			view->StrokeLine(BPoint(c.x - h * 0.55f, c.y - h * 0.55f),
				BPoint(c.x + h * 0.55f, c.y + h * 0.55f));
			view->StrokeLine(BPoint(c.x + h * 0.55f, c.y - h * 0.55f),
				BPoint(c.x - h * 0.55f, c.y + h * 0.55f));
			break;
		case B_GLYPH_ADD:
			view->SetPenSize(fmaxf(1.0f, roundf(size * 0.14f)));
			view->StrokeLine(BPoint(c.x, c.y - h * 0.68f),
				BPoint(c.x, c.y + h * 0.68f));
			view->StrokeLine(BPoint(c.x - h * 0.68f, c.y),
				BPoint(c.x + h * 0.68f, c.y));
			break;
		case B_GLYPH_PAGES:
		{
			view->SetPenSize(fmaxf(1.0f, roundf(size * 0.09f)));
			float radius = h * 0.14f;
			BRect back(c.x - h * 0.5f, c.y - h * 0.82f, c.x + h * 0.82f,
				c.y + h * 0.5f);
			BRect front = PagesGlyphFace(rect);
			// the page behind shows only above and to the right of the front
			view->StrokeLine(BPoint(back.left, front.top - pen * 0.5f),
				BPoint(back.left, back.top + radius));
			view->StrokeLine(BPoint(back.left + radius, back.top),
				BPoint(back.right - radius, back.top));
			view->StrokeArc(BPoint(back.left + radius, back.top + radius),
				radius, radius, 90, 90);
			view->StrokeArc(BPoint(back.right - radius, back.top + radius),
				radius, radius, 0, 90);
			view->StrokeLine(BPoint(back.right, back.top + radius),
				BPoint(back.right, back.bottom));
			view->StrokeLine(BPoint(back.right, back.bottom),
				BPoint(front.right + pen * 0.5f, back.bottom));
			view->StrokeRoundRect(front, radius, radius);
			break;
		}
		case B_GLYPH_BOOK:
		{
			BPoint left[4] = { BPoint(c.x - h * 0.08f, c.y - h * 0.5f),
				BPoint(c.x - h * 0.86f, c.y - h * 0.68f),
				BPoint(c.x - h * 0.86f, c.y + h * 0.56f),
				BPoint(c.x - h * 0.08f, c.y + h * 0.74f) };
			BPoint right[4] = { BPoint(c.x + h * 0.08f, c.y - h * 0.5f),
				BPoint(c.x + h * 0.86f, c.y - h * 0.68f),
				BPoint(c.x + h * 0.86f, c.y + h * 0.56f),
				BPoint(c.x + h * 0.08f, c.y + h * 0.74f) };
			view->FillPolygon(left, 4);
			view->FillPolygon(right, 4);
			break;
		}
		case B_GLYPH_ACTION:
		{
			view->SetPenSize(fmaxf(1.0f, roundf(size * 0.1f)));
			float top = c.y - h * 0.18f, bottom = c.y + h * 0.82f;
			float left = c.x - h * 0.62f, right = c.x + h * 0.62f;
			view->StrokeLine(BPoint(left + h * 0.32f, top), BPoint(left, top));
			view->StrokeLine(BPoint(left, top), BPoint(left, bottom));
			view->StrokeLine(BPoint(left, bottom), BPoint(right, bottom));
			view->StrokeLine(BPoint(right, bottom), BPoint(right, top));
			view->StrokeLine(BPoint(right, top), BPoint(right - h * 0.32f, top));
			view->StrokeLine(BPoint(c.x, c.y + h * 0.34f),
				BPoint(c.x, c.y - h * 0.6f));
			view->FillTriangle(BPoint(c.x, c.y - h * 0.92f),
				BPoint(c.x - h * 0.34f, c.y - h * 0.5f),
				BPoint(c.x + h * 0.34f, c.y - h * 0.5f));
			break;
		}
		case B_GLYPH_CLOCK:
			view->SetPenSize(fmaxf(1.0f, roundf(size * 0.09f)));
			view->StrokeEllipse(c, h * 0.78f, h * 0.78f);
			view->StrokeLine(c, BPoint(c.x, c.y - h * 0.5f));
			view->StrokeLine(c, BPoint(c.x + h * 0.36f, c.y + h * 0.2f));
			break;
		case B_GLYPH_FOLDER:
		{
			BPoint outline[6] = { BPoint(c.x - h * 0.86f, c.y - h * 0.58f),
				BPoint(c.x - h * 0.3f, c.y - h * 0.58f),
				BPoint(c.x - h * 0.16f, c.y - h * 0.4f),
				BPoint(c.x + h * 0.86f, c.y - h * 0.4f),
				BPoint(c.x + h * 0.86f, c.y + h * 0.66f),
				BPoint(c.x - h * 0.86f, c.y + h * 0.66f) };
			view->FillPolygon(outline, 6);
			break;
		}
		case B_GLYPH_STAR:
		{
			BPoint points[10];
			for (int i = 0; i < 10; i++) {
				float radius = (i % 2) == 0 ? h * 0.92f : h * 0.38f;
				float angle = (-90 + i * 36) * M_PI / 180;
				points[i] = BPoint(c.x + radius * cosf(angle),
					c.y + h * 0.08f + radius * sinf(angle));
			}
			view->FillPolygon(points, 10);
			break;
		}
		case B_GLYPH_GLASSES:
		case B_GLYPH_PRIVATE:
		{
			bool hat = glyph == B_GLYPH_PRIVATE;
			float y = hat ? c.y + h * 0.42f : c.y + h * 0.1f;
			float radius = h * 0.3f;
			view->SetPenSize(fmaxf(1.0f, roundf(size * 0.08f)));
			view->StrokeEllipse(BPoint(c.x - h * 0.46f, y), radius, radius);
			view->StrokeEllipse(BPoint(c.x + h * 0.46f, y), radius, radius);
			view->StrokeArc(BPoint(c.x, y), h * 0.16f, h * 0.12f, 20, 140);
			if (hat) {
				BPoint crown[4] = { BPoint(c.x - h * 0.5f, c.y - h * 0.08f),
					BPoint(c.x - h * 0.36f, c.y - h * 0.8f),
					BPoint(c.x + h * 0.36f, c.y - h * 0.8f),
					BPoint(c.x + h * 0.5f, c.y - h * 0.08f) };
				view->FillPolygon(crown, 4);
				view->FillRect(BRect(c.x - h * 0.92f, c.y - h * 0.16f,
					c.x + h * 0.92f, c.y - h * 0.02f));
			} else {
				view->StrokeLine(BPoint(c.x - h * 0.76f, y),
					BPoint(c.x - h * 0.94f, y - h * 0.3f));
				view->StrokeLine(BPoint(c.x + h * 0.76f, y),
					BPoint(c.x + h * 0.94f, y - h * 0.3f));
			}
			break;
		}
		case B_GLYPH_CHEVRON:
			view->SetPenSize(fmaxf(1.0f, roundf(size * 0.14f)));
			view->StrokeLine(BPoint(c.x - h * 0.24f, c.y - h * 0.56f),
				BPoint(c.x + h * 0.3f, c.y));
			view->StrokeLine(BPoint(c.x + h * 0.3f, c.y),
				BPoint(c.x - h * 0.24f, c.y + h * 0.56f));
			break;
		case B_GLYPH_CHECK:
			view->SetPenSize(fmaxf(1.0f, roundf(size * 0.15f)));
			view->StrokeLine(BPoint(c.x - h * 0.66f, c.y + h * 0.02f),
				BPoint(c.x - h * 0.2f, c.y + h * 0.5f));
			view->StrokeLine(BPoint(c.x - h * 0.2f, c.y + h * 0.5f),
				BPoint(c.x + h * 0.7f, c.y - h * 0.56f));
			break;
		case B_GLYPH_GLOBE:
			view->SetPenSize(fmaxf(1.0f, roundf(size * 0.07f)));
			view->StrokeEllipse(c, h * 0.8f, h * 0.8f);
			view->StrokeEllipse(c, h * 0.34f, h * 0.8f);
			view->StrokeLine(BPoint(c.x - h * 0.8f, c.y),
				BPoint(c.x + h * 0.8f, c.y));
			view->StrokeLine(BPoint(c.x, c.y - h * 0.8f),
				BPoint(c.x, c.y + h * 0.8f));
			break;
		case B_GLYPH_MORE:
			for (int i = -1; i <= 1; i++) {
				view->FillEllipse(BPoint(c.x + i * h * 0.62f, c.y), h * 0.17f,
					h * 0.17f);
			}
			break;
		case B_GLYPH_LOCK:
		{
			view->FillRoundRect(BRect(c.x - h * 0.6f, c.y - h * 0.1f,
				c.x + h * 0.6f, c.y + h * 0.78f), h * 0.12f, h * 0.12f);
			view->SetPenSize(fmaxf(1.0f, roundf(size * 0.11f)));
			view->StrokeArc(BPoint(c.x, c.y - h * 0.36f), h * 0.38f,
				h * 0.4f, 0, 180);
			view->StrokeLine(BPoint(c.x - h * 0.38f, c.y - h * 0.36f),
				BPoint(c.x - h * 0.38f, c.y - h * 0.1f));
			view->StrokeLine(BPoint(c.x + h * 0.38f, c.y - h * 0.36f),
				BPoint(c.x + h * 0.38f, c.y - h * 0.1f));
			break;
		}
		case B_GLYPH_DOWNLOAD:
			view->SetPenSize(fmaxf(1.0f, roundf(size * 0.12f)));
			view->StrokeLine(BPoint(c.x, c.y - h * 0.8f),
				BPoint(c.x, c.y + h * 0.2f));
			view->FillTriangle(BPoint(c.x, c.y + h * 0.5f),
				BPoint(c.x - h * 0.4f, c.y + h * 0.05f),
				BPoint(c.x + h * 0.4f, c.y + h * 0.05f));
			view->StrokeLine(BPoint(c.x - h * 0.7f, c.y + h * 0.8f),
				BPoint(c.x + h * 0.7f, c.y + h * 0.8f));
			break;
		case B_GLYPH_NONE:
			break;
	}
	view->PopState();
}


// #pragma mark - MetalBar

enum BarPosition {
	B_MOBILE_TOP_BAR,		// along the top of the window: dark edge below
	B_MOBILE_BOTTOM_BAR		// along the bottom: dark edge above
};


// A Bar in brushed aluminium (graphite on a dark panel). The skinned controls
// below paint its metal behind them, so it looks like one plate.
class MetalBar : public Bar {
public:
	explicit MetalBar(const char* name, BarPosition position = B_MOBILE_TOP_BAR)
		:
		Bar(name),
		fPosition(position),
		fProgress(-1)
	{
		SetFlags(Flags() | B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE
			| B_DRAW_ON_CHILDREN);
		SetViewColor(B_TRANSPARENT_COLOR);
		float inset = ceilf(be_plain_font->Size() * 0.5f);
		GroupLayout()->SetInsets(inset, 1, inset, 1);
	}

	BarPosition Position() const { return fPosition; }

	// A loading progress line along the edge towards the content: 0 to 1,
	// or below 0 to hide it.
	void SetProgress(float progress)
	{
		if (progress >= 1)
			progress = -1;
		if (progress == fProgress)
			return;
		fProgress = progress;
		BRect bounds = Bounds();
		float thickness = ProgressThickness();
		Invalidate(fPosition == B_MOBILE_TOP_BAR
			? BRect(bounds.left, bounds.bottom - thickness, bounds.right,
				bounds.bottom)
			: BRect(bounds.left, bounds.top, bounds.right,
				bounds.top + thickness));
	}

	float Progress() const { return fProgress; }

	static float ProgressThickness()
	{
		return fmaxf(2.0f, ceilf(be_plain_font->Size() / 4));
	}

	virtual void Draw(BRect update)
	{
		const Skin& skin = CurrentSkin();
		BRect bounds = Bounds();
		FillGradient(this, bounds, skin.barTop, skin.barBottom);
		PushState();
		SetDrawingMode(B_OP_ALPHA);
		if (fPosition == B_MOBILE_TOP_BAR) {
			SetHighColor(skin.barHighlight);
			StrokeLine(bounds.LeftTop(), bounds.RightTop());
			SetHighColor(skin.barEdge);
			StrokeLine(bounds.LeftBottom(), bounds.RightBottom());
		} else {
			SetHighColor(skin.barEdge);
			StrokeLine(bounds.LeftTop(), bounds.RightTop());
			SetHighColor(skin.barHighlight);
			StrokeLine(BPoint(bounds.left, bounds.top + 1),
				BPoint(bounds.right, bounds.top + 1));
		}
		PopState();
		_DrawProgress();
	}

	virtual void DrawAfterChildren(BRect update)
	{
		_DrawProgress();
	}

private:
	void _DrawProgress()
	{
		if (fProgress < 0)
			return;
		const Skin& skin = CurrentSkin();
		BRect bounds = Bounds();
		float thickness = ProgressThickness();
		BRect line = fPosition == B_MOBILE_TOP_BAR
			? BRect(bounds.left, bounds.bottom - thickness + 1, bounds.right,
				bounds.bottom)
			: BRect(bounds.left, bounds.top, bounds.right,
				bounds.top + thickness - 1);
		line.right = line.left + floorf(line.Width()
			* fmaxf(0.04f, fProgress));
		FillGradient(this, line, skin.fillTop, skin.fillBottom);
	}

	BarPosition fPosition;
	float fProgress;
};


// Fills `update` with the metal of the MetalBar that contains `view`, lined
// up with the bar's own gradient. Outside a bar it fills with the panel colour.
inline void
FillBarBehind(BView* view, BRect update)
{
	for (BView* parent = view->Parent(); parent != NULL;
			parent = parent->Parent()) {
		MetalBar* bar = dynamic_cast<MetalBar*>(parent);
		if (bar == NULL)
			continue;
		const Skin& skin = CurrentSkin();
		BRect plate = view->ConvertFromScreen(
			bar->ConvertToScreen(bar->Bounds()));
		BGradientLinear gradient(plate.LeftTop(), plate.LeftBottom());
		gradient.AddColor(skin.barTop, 0);
		gradient.AddColor(skin.barBottom, 255);
		view->FillRect(update, gradient);
		return;
	}
	view->PushState();
	view->SetHighColor(ui_color(B_PANEL_BACKGROUND_COLOR));
	view->FillRect(update);
	view->PopState();
}


// A plate shape pressed into the metal: a highlight below it, then a dark
// border, then the face. Returns the face rectangle.
inline BRect
DrawPlate(BView* view, BRect rect, float radius, rgb_color top,
	rgb_color bottom)
{
	const Skin& skin = CurrentSkin();
	view->PushState();
	view->SetDrawingMode(B_OP_ALPHA);
	view->SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
	view->SetHighColor(skin.controlEmboss);
	view->FillRoundRect(rect.OffsetByCopy(0, 1), radius, radius);
	view->SetHighColor(skin.controlBorder);
	view->FillRoundRect(rect, radius, radius);
	view->PopState();
	BRect face = rect.InsetByCopy(1, 1);
	FillGradient(view, face, top, bottom, fmaxf(0.0f, radius - 1));
	return face;
}


// The rectangle of visible artwork, centred vertically in a touch control.
inline BRect
VisibleRect(BRect bounds, float height = ControlHeight())
{
	height = fminf(height, bounds.Height() - 2);
	float top = floorf(bounds.top + (bounds.Height() - height) / 2);
	return BRect(bounds.left + 1, top, bounds.right - 1, top + height);
}


// #pragma mark - SegmentedControl

// A row of joined buttons. As a selector (the default) one segment is
// selected, Value() is its index, and choosing another invokes the control's
// message with "index". A momentary control (transport buttons, for example)
// keeps no selection and sends each segment's own message, or the control's
// message with "index".
class SegmentedControl : public BControl {
public:
	SegmentedControl(const char* name, BMessage* message,
		bool momentary = false)
		:
		BControl(name, NULL, message, B_WILL_DRAW | B_NAVIGABLE
			| B_FULL_UPDATE_ON_RESIZE),
		fMomentary(momentary),
		fPressed(-1),
		fInside(false),
		fEqualWidths(true)
	{
		SetViewColor(B_TRANSPARENT_COLOR);
		SetValueNoUpdate(momentary ? -1 : 0);
	}

	virtual ~SegmentedControl()
	{
		RemoveSegments();
	}

	int32 AddSegment(const char* label, Glyph glyph = B_GLYPH_NONE,
		BMessage* message = NULL)
	{
		Segment segment;
		segment.label = label != NULL ? label : "";
		segment.glyph = glyph;
		segment.message = message;
		segment.enabled = true;
		fSegments.push_back(segment);
		_Changed();
		return (int32)fSegments.size() - 1;
	}

	void RemoveSegments()
	{
		for (size_t i = 0; i < fSegments.size(); i++)
			delete fSegments[i].message;
		fSegments.clear();
		fPressed = -1;
		_Changed();
	}

	int32 CountSegments() const { return (int32)fSegments.size(); }

	void SetSegmentLabel(int32 index, const char* label)
	{
		if (_Valid(index) && fSegments[index].label != label) {
			fSegments[index].label = label;
			_Changed();
		}
	}

	void SetSegmentGlyph(int32 index, Glyph glyph)
	{
		if (_Valid(index) && fSegments[index].glyph != glyph) {
			fSegments[index].glyph = glyph;
			Invalidate();
		}
	}

	void SetSegmentEnabled(int32 index, bool enabled)
	{
		if (_Valid(index) && fSegments[index].enabled != enabled) {
			fSegments[index].enabled = enabled;
			Invalidate();
		}
	}

	// Equal widths (the default) suit tabs; off, each segment fits its label.
	void SetEqualWidths(bool equal) { fEqualWidths = equal; _Changed(); }

	int32 Selected() const { return Value(); }
	// Selects without invoking; -1 selects nothing.
	void Select(int32 index) { SetValue(index); }

	virtual BSize MinSize()
	{
		float width = 0;
		for (size_t i = 0; i < fSegments.size(); i++)
			width += _SegmentWidth(i);
		return BLayoutUtils::ComposeSize(ExplicitMinSize(),
			BSize(width, TargetSize()));
	}

	virtual BSize PreferredSize()
	{
		return BLayoutUtils::ComposeSize(ExplicitPreferredSize(), MinSize());
	}

	virtual BSize MaxSize()
	{
		return BLayoutUtils::ComposeSize(ExplicitMaxSize(), MinSize());
	}

	virtual void Draw(BRect update)
	{
		const Skin& skin = CurrentSkin();
		FillBarBehind(this, update);
		if (fSegments.empty())
			return;
		BRect plate = VisibleRect(Bounds());
		float radius = ceilf(plate.Height() / 6);
		BRect face = DrawPlate(this, plate, radius, skin.controlTop,
			skin.controlBottom);
		BFont font(be_bold_font);
		font.SetSize(be_plain_font->Size());
		SetFont(&font);
		for (int32 i = 0; i < (int32)fSegments.size(); i++) {
			BRect segment = _SegmentRect(i, face);
			bool selected = !fMomentary && i == Value();
			bool pressed = i == fPressed && fInside;
			if (selected || pressed) {
				PushState();
				BRegion clip(segment);
				ConstrainClippingRegion(&clip);
				FillGradient(this, face, selected ? skin.selectedTop
					: skin.pressedTop, selected ? skin.selectedBottom
					: skin.pressedBottom, fmaxf(0.0f, radius - 1));
				SetDrawingMode(B_OP_ALPHA);
				SetHighColor(0, 0, 0, 60);
				StrokeLine(segment.LeftTop(), segment.RightTop());
				PopState();
			}
			if (i > 0) {
				SetHighColor(skin.controlBorder);
				StrokeLine(BPoint(segment.left - 1, plate.top),
					BPoint(segment.left - 1, plate.bottom));
			}
			const Segment& item = fSegments[i];
			rgb_color text = selected ? skin.selectedText : skin.controlText;
			rgb_color emboss = selected ? skin.selectedEmboss
				: skin.controlEmboss;
			if (!item.enabled || !IsEnabled())
				text.alpha = 90;
			if (item.glyph != B_GLYPH_NONE) {
				float glyph = floorf(face.Height() * 0.5f);
				BRect box(0, 0, glyph, glyph);
				box.OffsetTo(floorf((segment.left + segment.right - glyph) / 2),
					floorf((face.top + face.bottom - glyph) / 2));
				DrawGlyph(this, item.glyph, box.OffsetByCopy(0, 1), emboss);
				DrawGlyph(this, item.glyph, box, text);
			} else {
				DrawCentered(this, item.label.String(),
					segment.InsetByCopy(4, 0), text, emboss);
			}
		}
		if (IsFocus() && Window() != NULL && Window()->IsActive()) {
			SetHighColor(ui_color(B_KEYBOARD_NAVIGATION_COLOR));
			StrokeRoundRect(plate.InsetByCopy(-1, -1), radius + 1, radius + 1);
		}
	}

	virtual void MouseDown(BPoint where)
	{
		if (!IsEnabled())
			return;
		int32 index = SegmentAt(where);
		if (index < 0 || !fSegments[index].enabled)
			return;
		fPressed = index;
		fInside = true;
		SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS
			| B_NO_POINTER_HISTORY);
		Invalidate();
	}

	virtual void MouseMoved(BPoint where, uint32 transit, const BMessage* drag)
	{
		if (fPressed < 0)
			return;
		bool inside = SegmentAt(where) == fPressed;
		if (inside != fInside) {
			fInside = inside;
			Invalidate();
		}
	}

	virtual void MouseUp(BPoint where)
	{
		int32 index = fPressed;
		fPressed = -1;
		Invalidate();
		if (index >= 0 && SegmentAt(where) == index)
			Choose(index);
	}

	virtual void KeyDown(const char* bytes, int32 numBytes)
	{
		int32 count = (int32)fSegments.size();
		if (count == 0 || numBytes != 1) {
			BControl::KeyDown(bytes, numBytes);
			return;
		}
		int32 current = Value() < 0 ? 0 : Value();
		switch (bytes[0]) {
			case B_LEFT_ARROW:
				if (!fMomentary && current > 0)
					Choose(current - 1);
				break;
			case B_RIGHT_ARROW:
				if (!fMomentary && current < count - 1)
					Choose(current + 1);
				break;
			case B_SPACE:
			case B_ENTER:
				Choose(current);
				break;
			default:
				BControl::KeyDown(bytes, numBytes);
		}
	}

	int32 SegmentAt(BPoint where) const
	{
		BRect face = VisibleRect(Bounds()).InsetByCopy(1, 1);
		if (!Bounds().Contains(where))
			return -1;
		for (int32 i = 0; i < (int32)fSegments.size(); i++) {
			BRect segment = _SegmentRect(i, face);
			if (where.x >= segment.left - 1 && where.x <= segment.right + 1)
				return i;
		}
		return -1;
	}

	// Selects (a selector) and sends the segment's message, as a tap does.
	void Choose(int32 index)
	{
		if (!_Valid(index) || !fSegments[index].enabled)
			return;
		if (!fMomentary)
			SetValue(index);
		BMessage* own = fSegments[index].message;
		if (own == NULL && Message() == NULL)
			return;
		BMessage message(own != NULL ? *own : *Message());
		message.AddInt32("index", index);
		Invoke(&message);
	}

private:
	struct Segment {
		BString label;
		Glyph glyph;
		BMessage* message;
		bool enabled;
	};

	bool _Valid(int32 index) const
	{
		return index >= 0 && index < (int32)fSegments.size();
	}

	void _Changed()
	{
		InvalidateLayout();
		Invalidate();
	}

	float _NaturalWidth(size_t index) const
	{
		const Segment& segment = fSegments[index];
		if (segment.glyph != B_GLYPH_NONE)
			return ceilf(TargetSize() * 1.25f);
		BFont font(be_bold_font);
		font.SetSize(be_plain_font->Size());
		return fmaxf(ceilf(TargetSize() * 1.4f),
			ceilf(font.StringWidth(segment.label.String())
				+ be_plain_font->Size() * 2.4f));
	}

	float _SegmentWidth(size_t index) const
	{
		if (!fEqualWidths)
			return _NaturalWidth(index);
		float width = 0;
		for (size_t i = 0; i < fSegments.size(); i++)
			width = fmaxf(width, _NaturalWidth(i));
		return width;
	}

	BRect _SegmentRect(int32 index, BRect face) const
	{
		float natural = 0;
		for (size_t i = 0; i < fSegments.size(); i++)
			natural += _SegmentWidth(i);
		float scale = natural > 0 ? (face.Width() + 1) / natural : 1;
		float left = face.left;
		for (int32 i = 0; i < index; i++)
			left += _SegmentWidth(i) * scale;
		float right = face.left - 1;
		for (int32 i = 0; i <= index; i++)
			right += _SegmentWidth(i) * scale;
		return BRect(roundf(left) + (index > 0 ? 1 : 0), face.top,
			roundf(right), face.bottom);
	}

	std::vector<Segment> fSegments;
	bool fMomentary;
	int32 fPressed;
	bool fInside;
	bool fEqualWidths;
};


// #pragma mark - MetalButton

enum ButtonShape {
	B_ROUNDED_BUTTON,	// a plain bar button
	B_PILL_BUTTON,		// fully round ends
	B_BACK_BUTTON,		// pointed on the left, for drill-down navigation
	B_PLAIN_BUTTON		// no plate: a glyph cut into the bar (toolbars)
};


class MetalButton : public BControl {
public:
	MetalButton(const char* name, const char* label, BMessage* message,
		ButtonShape shape = B_ROUNDED_BUTTON, Glyph glyph = B_GLYPH_NONE)
		:
		BControl(name, label, message, B_WILL_DRAW | B_NAVIGABLE
			| B_FULL_UPDATE_ON_RESIZE),
		fShape(shape),
		fGlyph(glyph),
		fPressed(false),
		fInside(false)
	{
		SetViewColor(B_TRANSPARENT_COLOR);
	}

	void SetGlyph(Glyph glyph) { fGlyph = glyph; InvalidateLayout(); Invalidate(); }
	Glyph GetGlyph() const { return fGlyph; }

	// Short text written inside the glyph, such as the count of open pages
	// on B_GLYPH_PAGES.
	void SetGlyphText(const char* text)
	{
		if (fGlyphText == text)
			return;
		fGlyphText = text;
		Invalidate();
	}

	// A highlighted button (a plain one glows, others look pressed in), for
	// a mode that is on.
	void SetHighlighted(bool highlighted)
	{
		if (fHighlighted == highlighted)
			return;
		fHighlighted = highlighted;
		Invalidate();
	}

	virtual void SetLabel(const char* label)
	{
		BControl::SetLabel(label);
		InvalidateLayout();
	}

	virtual BSize MinSize()
	{
		BFont font(be_bold_font);
		font.SetSize(be_plain_font->Size());
		float width = be_plain_font->Size() * 2;
		if (Label() != NULL && Label()[0] != '\0')
			width += font.StringWidth(Label());
		if (fGlyph != B_GLYPH_NONE)
			width += ControlHeight() * 0.6f;
		if (fShape == B_BACK_BUTTON)
			width += ControlHeight() * 0.3f;
		if (fShape == B_PLAIN_BUTTON && (Label() == NULL || Label()[0] == '\0'))
			width = TargetSize() * 1.1f;
		return BLayoutUtils::ComposeSize(ExplicitMinSize(),
			BSize(ceilf(fmaxf(width, TargetSize())), TargetSize()));
	}

	virtual BSize PreferredSize()
	{
		return BLayoutUtils::ComposeSize(ExplicitPreferredSize(), MinSize());
	}

	virtual BSize MaxSize()
	{
		return BLayoutUtils::ComposeSize(ExplicitMaxSize(), MinSize());
	}

	virtual void Draw(BRect update)
	{
		const Skin& skin = CurrentSkin();
		FillBarBehind(this, update);
		BRect plate = VisibleRect(Bounds());
		bool down = (fPressed && fInside) || fHighlighted;
		rgb_color top = down ? skin.pressedTop : skin.controlTop;
		rgb_color bottom = down ? skin.pressedBottom : skin.controlBottom;
		BRect content = plate;
		if (fShape == B_PLAIN_BUTTON) {
			if (down) {
				// the glow of a pressed toolbar button
				PushState();
				SetDrawingMode(B_OP_ALPHA);
				SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
				BPoint middle((plate.left + plate.right) / 2,
					(plate.top + plate.bottom) / 2);
				float radius = plate.Height() * 0.62f;
				for (int i = 4; i >= 1; i--) {
					SetHighColor(255, 255, 255, IsDarkSkin() ? 22 : 40);
					FillEllipse(middle, radius * i / 4, radius * i / 4);
				}
				PopState();
			}
		} else if (fShape == B_BACK_BUTTON) {
			float point = floorf(plate.Height() * 0.3f);
			PushState();
			SetDrawingMode(B_OP_ALPHA);
			SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
			BShape outline;
			_BackShape(&outline, plate.OffsetByCopy(0, 1), point);
			// shapes are drawn relative to the pen
			MovePenTo(B_ORIGIN);
			SetHighColor(skin.controlEmboss);
			FillShape(&outline);
			outline.Clear();
			_BackShape(&outline, plate, point);
			SetHighColor(skin.controlBorder);
			FillShape(&outline);
			PopState();
			BRect face = plate.InsetByCopy(1, 1);
			BShape inner;
			_BackShape(&inner, face, point - 0.5f);
			BGradientLinear gradient(face.LeftTop(), face.LeftBottom());
			gradient.AddColor(top, 0);
			gradient.AddColor(bottom, 255);
			MovePenTo(B_ORIGIN);
			FillShape(&inner, gradient);
			content.left += point;
		} else {
			float radius = fShape == B_PILL_BUTTON ? plate.Height() / 2
				: ceilf(plate.Height() / 6);
			DrawPlate(this, plate, radius, top, bottom);
		}
		rgb_color text = skin.controlText;
		if (!IsEnabled())
			text.alpha = 90;
		BFont font(be_bold_font);
		font.SetSize(be_plain_font->Size());
		SetFont(&font);
		float glyph = floorf(plate.Height()
			* (fShape == B_PLAIN_BUTTON ? 0.66f : 0.5f));
		bool hasLabel = Label() != NULL && Label()[0] != '\0';
		if (fGlyph != B_GLYPH_NONE) {
			float labelWidth = hasLabel ? font.StringWidth(Label())
				+ glyph * 0.4f : 0;
			float left = floorf((content.left + content.right
				- glyph - labelWidth) / 2);
			BRect box(left, floorf((plate.top + plate.bottom - glyph) / 2),
				left + glyph, floorf((plate.top + plate.bottom + glyph) / 2));
			rgb_color glyphColor = text;
			if (fShape == B_PLAIN_BUTTON && fHighlighted)
				glyphColor = skin.displayActive;
			DrawGlyph(this, fGlyph, box.OffsetByCopy(0, 1), skin.controlEmboss);
			DrawGlyph(this, fGlyph, box, glyphColor);
			if (!fGlyphText.IsEmpty()) {
				BRect face = fGlyph == B_GLYPH_PAGES ? PagesGlyphFace(box) : box;
				BFont small(be_bold_font);
				small.SetSize(fmaxf(7.0f, face.Height() * 0.62f));
				SetFont(&small);
				DrawCentered(this, fGlyphText.String(), face.OffsetByCopy(0, 0.5f),
					glyphColor, skin.controlEmboss);
				SetFont(&font);
			}
			content.left = box.right + glyph * 0.4f;
			if (hasLabel) {
				content.right = content.left + labelWidth;
			}
		}
		if (hasLabel)
			DrawCentered(this, Label(), content.InsetByCopy(2, 0), text,
				skin.controlEmboss);
		if (IsFocus() && Window() != NULL && Window()->IsActive()) {
			SetHighColor(ui_color(B_KEYBOARD_NAVIGATION_COLOR));
			StrokeLine(BPoint(content.left + 4, plate.bottom - 3),
				BPoint(content.right - 4, plate.bottom - 3));
		}
	}

	virtual void MouseDown(BPoint where)
	{
		if (!IsEnabled())
			return;
		fPressed = fInside = true;
		SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS
			| B_NO_POINTER_HISTORY);
		Invalidate();
	}

	virtual void MouseMoved(BPoint where, uint32 transit, const BMessage* drag)
	{
		if (!fPressed)
			return;
		bool inside = Bounds().Contains(where);
		if (inside != fInside) {
			fInside = inside;
			Invalidate();
		}
	}

	virtual void MouseUp(BPoint where)
	{
		if (!fPressed)
			return;
		fPressed = false;
		Invalidate();
		if (Bounds().Contains(where))
			Invoke();
	}

	virtual void KeyDown(const char* bytes, int32 numBytes)
	{
		if (numBytes == 1 && (bytes[0] == B_SPACE || bytes[0] == B_ENTER))
			Invoke();
		else
			BControl::KeyDown(bytes, numBytes);
	}

private:
	static void _BackShape(BShape* shape, BRect rect, float point)
	{
		float radius = ceilf(rect.Height() / 6);
		float middle = (rect.top + rect.bottom) / 2;
		shape->MoveTo(BPoint(rect.left, middle));
		shape->LineTo(BPoint(rect.left + point, rect.top));
		shape->LineTo(BPoint(rect.right - radius, rect.top));
		BPoint topRight[3] = { BPoint(rect.right - radius * 0.45f, rect.top),
			BPoint(rect.right, rect.top + radius * 0.45f),
			BPoint(rect.right, rect.top + radius) };
		shape->BezierTo(topRight);
		shape->LineTo(BPoint(rect.right, rect.bottom - radius));
		BPoint bottomRight[3] = { BPoint(rect.right,
				rect.bottom - radius * 0.45f),
			BPoint(rect.right - radius * 0.45f, rect.bottom),
			BPoint(rect.right - radius, rect.bottom) };
		shape->BezierTo(bottomRight);
		shape->LineTo(BPoint(rect.left + point, rect.bottom));
		shape->Close();
	}

	ButtonShape fShape;
	Glyph fGlyph;
	bool fPressed;
	bool fInside;
	bool fHighlighted = false;
	BString fGlyphText;
};


// #pragma mark - NowPlayingDisplay

// The LCD of a media player: the cover on the left, the title between artist
// and album on the first line, then repeat, the elapsed time, a scrubber with
// a playhead, the remaining time and shuffle. Taps are reported with the
// messages set for each part (to the target, or else the window):
// B_DISPLAY_SEEK gets "fraction" (float, 0 to 1) and "position" (bigtime_t)
// when a drag along the scrubber ends.
class NowPlayingDisplay : public BView {
public:
	enum Part {
		B_DISPLAY_SEEK = 0,
		B_DISPLAY_ARTWORK,
		B_DISPLAY_REPEAT,
		B_DISPLAY_SHUFFLE,
		B_DISPLAY_PART_COUNT
	};

	explicit NowPlayingDisplay(const char* name)
		:
		BView(name, B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE),
		fArtwork(NULL),
		fPosition(0),
		fDuration(0),
		fRepeat(0),
		fShuffle(false),
		fControls(true),
		fPressed(-1),
		fScrub(-1)
	{
		SetViewColor(B_TRANSPARENT_COLOR);
		for (int32 i = 0; i < B_DISPLAY_PART_COUNT; i++)
			fMessages[i] = NULL;
	}

	virtual ~NowPlayingDisplay()
	{
		delete fArtwork;
		for (int32 i = 0; i < B_DISPLAY_PART_COUNT; i++)
			delete fMessages[i];
	}

	// Takes ownership of the message.
	void SetMessage(Part part, BMessage* message)
	{
		delete fMessages[part];
		fMessages[part] = message;
	}

	void SetTarget(const BMessenger& target) { fTarget = target; }

	// The bitmap is copied; NULL shows a note.
	void SetArtwork(const BBitmap* bitmap)
	{
		delete fArtwork;
		fArtwork = bitmap != NULL && bitmap->IsValid()
			? new BBitmap(bitmap) : NULL;
		Invalidate();
	}

	void SetText(const char* title, const char* artist, const char* album)
	{
		fTitle = title;
		fArtist = artist;
		fAlbum = album;
		Invalidate();
	}

	// Shown alone when there is no title, the app's name for example.
	void SetIdleText(const char* text) { fIdle = text; Invalidate(); }

	void SetProgress(bigtime_t position, bigtime_t duration)
	{
		if (position == fPosition && duration == fDuration)
			return;
		fPosition = position;
		fDuration = duration;
		Invalidate();
	}

	// 0 off, 1 repeat all, 2 repeat one.
	void SetRepeat(int32 mode) { fRepeat = mode; Invalidate(); }
	void SetShuffle(bool shuffle) { fShuffle = shuffle; Invalidate(); }
	// Without the controls the second line is only the scrubber.
	void SetShowsModes(bool shown) { fControls = shown; Invalidate(); }
	bool IsScrubbing() const { return fScrub >= 0; }

	virtual BSize MinSize()
	{
		return BLayoutUtils::ComposeSize(ExplicitMinSize(),
			BSize(TargetSize() * 6, TargetSize()));
	}

	virtual BSize MaxSize()
	{
		return BLayoutUtils::ComposeSize(ExplicitMaxSize(),
			BSize(B_SIZE_UNLIMITED, TargetSize()));
	}

	virtual void Draw(BRect update)
	{
		const Skin& skin = CurrentSkin();
		FillBarBehind(this, update);
		_Layout();
		float radius = 5;
		BRect face = DrawPlate(this, fPanel, radius, skin.displayTop,
			skin.displayBottom);
		// a gloss over the upper half
		PushState();
		SetDrawingMode(B_OP_ALPHA);
		BRect gloss(face.left, face.top, face.right,
			floorf((face.top + face.bottom) / 2));
		BGradientLinear shine(gloss.LeftTop(), gloss.LeftBottom());
		rgb_color white = { 255, 255, 255, 70 };
		rgb_color clear = { 255, 255, 255, 0 };
		shine.AddColor(white, 0);
		shine.AddColor(clear, 255);
		FillRoundRect(gloss, radius - 1, radius - 1, shine);
		PopState();

		// the cover
		if (fArtwork != NULL) {
			DrawBitmap(fArtwork, fArtwork->Bounds(), fArt,
				B_FILTER_BITMAP_BILINEAR);
		} else {
			FillGradient(this, fArt, MixColors(skin.displayTop,
				skin.displaySecondary, 0.15f), MixColors(skin.displayBottom,
				skin.displaySecondary, 0.3f));
			DrawGlyph(this, B_GLYPH_NOTE, fArt.InsetByCopy(fArt.Width() * 0.22f,
				fArt.Height() * 0.22f), MixColors(skin.displayBottom,
				skin.displaySecondary, 0.7f));
		}
		SetHighColor(skin.displayBorder);
		StrokeLine(BPoint(fArt.right + 1, fArt.top),
			BPoint(fArt.right + 1, fArt.bottom));

		if (fTitle.IsEmpty()) {
			BFont bold(be_bold_font);
			bold.SetSize(be_plain_font->Size() * 1.15f);
			SetFont(&bold);
			rgb_color glow = { 255, 255, 255, 0 };
			BRect body(fArt.right + 2, fText.top, fText.right, fPanel.bottom - 1);
			DrawCentered(this, fIdle.String(), body, skin.displaySecondary,
				glow);
			return;
		}
		_DrawTitle(skin);
		if (fDuration <= 0)
			return;
		BFont small(be_plain_font);
		small.SetSize(be_plain_font->Size() * 0.9f);
		SetFont(&small);
		bigtime_t shown = fScrub >= 0 ? (bigtime_t)(fScrub * fDuration)
			: fPosition;
		BString elapsed = _Time(shown), remaining("-");
		remaining << _Time(fDuration - shown);
		rgb_color none = { 0, 0, 0, 0 };
		DrawCentered(this, elapsed.String(), fElapsed, skin.displaySecondary,
			none);
		DrawCentered(this, remaining.String(), fRemaining,
			skin.displaySecondary, none);
		PushState();
		DrawScrubber(fTrack, fmaxf(0.0f, fminf(1.0f,
			(float)shown / fDuration)));
		PopState();
		if (fControls) {
			rgb_color off = skin.displaySecondary;
			off.alpha = 150;
			DrawGlyph(this, fRepeat == 2 ? B_GLYPH_REPEAT_ONE : B_GLYPH_REPEAT,
				fRepeatRect.InsetByCopy(fRepeatRect.Width() * 0.16f,
					fRepeatRect.Height() * 0.16f),
				fRepeat != 0 ? skin.displayActive : off);
			DrawGlyph(this, B_GLYPH_SHUFFLE,
				fShuffleRect.InsetByCopy(fShuffleRect.Width() * 0.16f,
					fShuffleRect.Height() * 0.16f),
				fShuffle ? skin.displayActive : off);
		}
	}

	virtual void MouseDown(BPoint where)
	{
		_Layout();
		fPressed = -1;
		if (fDuration > 0 && !fTitle.IsEmpty()
				&& fTrack.InsetByCopy(-4, -6).Contains(where)) {
			fPressed = B_DISPLAY_SEEK;
			fScrub = _Fraction(where);
			Invalidate();
		} else if (fArt.Contains(where))
			fPressed = B_DISPLAY_ARTWORK;
		else if (fControls && fDuration > 0 && fRepeatRect.Contains(where))
			fPressed = B_DISPLAY_REPEAT;
		else if (fControls && fDuration > 0 && fShuffleRect.Contains(where))
			fPressed = B_DISPLAY_SHUFFLE;
		if (fPressed >= 0)
			SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS
				| B_NO_POINTER_HISTORY);
	}

	virtual void MouseMoved(BPoint where, uint32 transit, const BMessage* drag)
	{
		if (fPressed == B_DISPLAY_SEEK) {
			fScrub = _Fraction(where);
			Invalidate();
		}
	}

	virtual void MouseUp(BPoint where)
	{
		int32 part = fPressed;
		fPressed = -1;
		if (part == B_DISPLAY_SEEK) {
			_Seek(_Fraction(where));
			return;
		}
		BRect area = part == B_DISPLAY_ARTWORK ? fArt
			: part == B_DISPLAY_REPEAT ? fRepeatRect : fShuffleRect;
		if (part >= 0 && area.Contains(where))
			_Send((Part)part, NULL);
	}

	// A finger dragged along the scrubber. On a touch screen in tablet mode
	// a drag arrives as scrolling, in pixels ("be:wheel_pixel_delta_x"), and
	// never as a press: the playhead follows the finger from where it was,
	// and the seek goes when the finger lifts.
	virtual void MessageReceived(BMessage* message)
	{
		float pixelsX, pixelsY;
		if (message->what != B_MOUSE_WHEEL_CHANGED
			|| message->FindFloat("be:wheel_pixel_delta_x", &pixelsX) != B_OK
			|| message->FindFloat("be:wheel_pixel_delta_y", &pixelsY) != B_OK) {
			BView::MessageReceived(message);
			return;
		}
		int32 phase = message->GetInt32("be:wheel_phase",
			Detail::kWheelPhaseNone);
		if (fScrub < 0) {
			// only a drag that starts on the scrubber
			BPoint where;
			uint32 buttons;
			GetMouse(&where, &buttons, false);
			_Layout();
			if (phase == Detail::kWheelPhaseEnded || fDuration <= 0
				|| fTitle.IsEmpty()
				|| !fTrack.InsetByCopy(-4, -6).Contains(where)) {
				return;
			}
			fScrub = fmaxf(0.0f, fminf(1.0f, (float)fPosition / fDuration));
		}
		if (fTrack.Width() > 0) {
			fScrub = fmaxf(0.0f, fminf(1.0f,
				fScrub - pixelsX / fTrack.Width()));
		}
		Invalidate();
		if (phase == Detail::kWheelPhaseEnded
			|| phase == Detail::kWheelPhaseNone) {
			_Seek(fScrub);
		}
	}

protected:
	// Draws the scrubber in track, played to fraction (0 to 1): a groove with
	// the part played filled and a playhead. A subclass can give it its own
	// look; the drawing state is restored after the call.
	virtual void DrawScrubber(BRect track, float fraction)
	{
		const Skin& skin = CurrentSkin();
		BRect groove(track.left, floorf((track.top + track.bottom) / 2) - 2,
			track.right, floorf((track.top + track.bottom) / 2) + 2);
		FillGradient(this, groove, skin.trackTop, skin.trackBottom, 2);
		float x = roundf(groove.left + fraction * groove.Width());
		if (x > groove.left + 1) {
			BRect played(groove.left, groove.top, x, groove.bottom);
			SetHighColor(skin.trackFill);
			FillRoundRect(played, 2, 2);
		}
		BRect head(x - 1, track.top + 1, x + 1, track.bottom - 1);
		SetHighColor(MixColors(skin.playhead, skin.displayBorder, 0.4f));
		FillRect(head.InsetByCopy(-1, 0));
		SetHighColor(skin.playhead);
		FillRect(head);
	}

private:
	void _Seek(float fraction)
	{
		fScrub = -1;
		fPosition = (bigtime_t)(fraction * fDuration);
		Invalidate();
		BMessage extra;
		extra.AddFloat("fraction", fraction);
		extra.AddInt64("position", fPosition);
		_Send(B_DISPLAY_SEEK, &extra);
	}

	void _Layout()
	{
		BRect bounds = Bounds();
		fPanel = bounds.InsetByCopy(1, 1);
		fPanel.bottom -= 1;
		BRect face = fPanel.InsetByCopy(1, 1);
		fArt = BRect(face.left, face.top, face.left + face.Height(),
			face.bottom);
		BRect body(fArt.right + 2, face.top, face.right, face.bottom);
		float line = floorf(body.Height() / 2);
		fText = BRect(body.left + 4, body.top + 1, body.right - 4,
			body.top + line);
		BRect row(body.left, body.top + line, body.right, body.bottom - 1);
		float glyph = fminf(row.Height(), ceilf(be_plain_font->Size() * 1.6f));
		float pad = ceilf(be_plain_font->Size() * 0.5f);
		float timeWidth = ceilf(be_plain_font->StringWidth("-88:88") * 0.95f);
		float left = row.left + pad, right = row.right - pad;
		if (fControls) {
			fRepeatRect = BRect(left, row.top, left + glyph,
				row.top + row.Height());
			fShuffleRect = BRect(right - glyph, row.top, right, row.bottom);
			left += glyph + pad;
			right -= glyph + pad;
		} else {
			fRepeatRect = fShuffleRect = BRect();
		}
		fElapsed = BRect(left, row.top, left + timeWidth, row.bottom);
		fRemaining = BRect(right - timeWidth, row.top, right, row.bottom);
		fTrack = BRect(fElapsed.right + pad, row.top + 2,
			fRemaining.left - pad, row.bottom - 2);
	}

	void _DrawTitle(const Skin& skin)
	{
		BFont bold(be_bold_font);
		bold.SetSize(be_plain_font->Size());
		BFont plain(be_plain_font);
		float gap = plain.StringWidth("   ");
		float width = fText.Width();
		BString title(fTitle), artist(fArtist), album(fAlbum);
		bold.TruncateString(&title, B_TRUNCATE_END, width);
		float titleWidth = bold.StringWidth(title.String());
		float side = fmaxf(0.0f, (width - titleWidth) / 2 - gap);
		plain.TruncateString(&artist, B_TRUNCATE_END, side);
		plain.TruncateString(&album, B_TRUNCATE_END, side);
		float artistWidth = artist.IsEmpty() ? 0
			: plain.StringWidth(artist.String());
		float albumWidth = album.IsEmpty() ? 0
			: plain.StringWidth(album.String());
		float center = (fText.left + fText.right) / 2;
		float titleLeft = floorf(center - titleWidth / 2);
		font_height height;
		bold.GetHeight(&height);
		float baseline = floorf((fText.top + fText.bottom - height.ascent
			- height.descent) / 2 + height.ascent);
		rgb_color glow = { 255, 255, 255, 0 };
		SetFont(&plain);
		if (artistWidth > 0)
			DrawEngraved(this, artist.String(), BPoint(titleLeft - gap
				- artistWidth, baseline), skin.displaySecondary, glow);
		if (albumWidth > 0)
			DrawEngraved(this, album.String(), BPoint(titleLeft + titleWidth
				+ gap, baseline), skin.displaySecondary, glow);
		SetFont(&bold);
		DrawEngraved(this, title.String(), BPoint(titleLeft, baseline),
			skin.displayText, glow);
	}

	float _Fraction(BPoint where) const
	{
		if (fTrack.Width() <= 0)
			return 0;
		return fmaxf(0.0f, fminf(1.0f, (where.x - fTrack.left)
			/ fTrack.Width()));
	}

	static BString _Time(bigtime_t time)
	{
		int64 seconds = time > 0 ? time / 1000000 : 0;
		BString text;
		if (seconds >= 3600) {
			text.SetToFormat("%d:%02d:%02d", (int)(seconds / 3600),
				(int)(seconds / 60 % 60), (int)(seconds % 60));
		} else
			text.SetToFormat("%d:%02d", (int)(seconds / 60), (int)(seconds % 60));
		return text;
	}

	void _Send(Part part, const BMessage* extra)
	{
		if (fMessages[part] == NULL)
			return;
		BMessage message(*fMessages[part]);
		if (extra != NULL) {
			char* name;
			type_code type;
			int32 count;
			for (int32 i = 0; extra->GetInfo(B_ANY_TYPE, i, &name, &type,
					&count) == B_OK; i++) {
				const void* data;
				ssize_t size;
				if (extra->FindData(name, type, &data, &size) == B_OK)
					message.AddData(name, type, data, size);
			}
		}
		message.AddPointer("source", this);
		BMessenger target = fTarget.IsValid() ? fTarget
			: BMessenger(Window());
		target.SendMessage(&message);
	}

	BMessage* fMessages[B_DISPLAY_PART_COUNT];
	BMessenger fTarget;
	BBitmap* fArtwork;
	BString fTitle, fArtist, fAlbum, fIdle;
	bigtime_t fPosition, fDuration;
	int32 fRepeat;
	bool fShuffle;
	bool fControls;
	int32 fPressed;
	float fScrub;
	BRect fPanel, fArt, fText, fElapsed, fRemaining, fTrack;
	BRect fRepeatRect, fShuffleRect;
};


// #pragma mark - MetalSlider

// A level slider with a glossy knob, for volume. Value() runs from 0 to 1000.
// The modification message is sent while the knob moves, the control's own
// message when it is let go.
class MetalSlider : public BControl {
public:
	MetalSlider(const char* name, BMessage* message, bool speakers = true)
		:
		BControl(name, NULL, message, B_WILL_DRAW | B_NAVIGABLE
			| B_FULL_UPDATE_ON_RESIZE),
		fModification(NULL),
		fSpeakers(speakers),
		fTracking(false),
		fDragValue(0)
	{
		SetViewColor(B_TRANSPARENT_COLOR);
	}

	virtual ~MetalSlider()
	{
		delete fModification;
	}

	void SetModificationMessage(BMessage* message)
	{
		delete fModification;
		fModification = message;
	}

	bool IsTracking() const { return fTracking; }

	virtual BSize MinSize()
	{
		return BLayoutUtils::ComposeSize(ExplicitMinSize(),
			BSize(TargetSize() * 3, TargetSize()));
	}

	virtual BSize MaxSize()
	{
		return BLayoutUtils::ComposeSize(ExplicitMaxSize(),
			BSize(TargetSize() * 6, TargetSize()));
	}

	virtual void Draw(BRect update)
	{
		const Skin& skin = CurrentSkin();
		FillBarBehind(this, update);
		BRect track = _Track();
		float middle = floorf((Bounds().top + Bounds().bottom) / 2);
		if (fSpeakers) {
			float glyph = floorf(ControlHeight() * 0.42f);
			BRect low(Bounds().left + 2, middle - glyph / 2,
				Bounds().left + 2 + glyph, middle + glyph / 2);
			BRect high(Bounds().right - 2 - glyph, middle - glyph / 2,
				Bounds().right - 2, middle + glyph / 2);
			DrawGlyph(this, B_GLYPH_SPEAKER, low.OffsetByCopy(0, 1),
				skin.controlEmboss);
			DrawGlyph(this, B_GLYPH_SPEAKER, low, skin.controlText);
			DrawGlyph(this, B_GLYPH_SPEAKER_LOUD, high.OffsetByCopy(0, 1),
				skin.controlEmboss);
			DrawGlyph(this, B_GLYPH_SPEAKER_LOUD, high, skin.controlText);
		}
		float thickness = fmaxf(5.0f, roundf(be_plain_font->Size() * 0.5f));
		BRect groove(track.left, middle - thickness / 2, track.right,
			middle + thickness / 2);
		float x = roundf(track.left + Value() / 1000.0f * track.Width());
		BRect face = DrawPlate(this, groove, thickness / 2, skin.trackTop,
			skin.trackBottom);
		if (x > face.left) {
			PushState();
			BRegion clip(BRect(face.left, face.top, x, face.bottom));
			ConstrainClippingRegion(&clip);
			FillGradient(this, face, skin.fillTop, skin.fillBottom,
				face.Height() / 2);
			PopState();
		}
		float knob = floorf(ControlHeight() * 0.36f);
		BRect circle(x - knob, middle - knob, x + knob, middle + knob);
		PushState();
		SetDrawingMode(B_OP_ALPHA);
		SetHighColor(0, 0, 0, 70);
		FillEllipse(circle.OffsetByCopy(0, 1.5f));
		PopState();
		SetHighColor(skin.knobBorder);
		FillEllipse(circle);
		BGradientLinear gradient(circle.LeftTop(), circle.LeftBottom());
		gradient.AddColor(skin.knobTop, 0);
		gradient.AddColor(skin.knobBottom, 255);
		FillEllipse(circle.InsetByCopy(1, 1), gradient);
		if (IsFocus() && Window() != NULL && Window()->IsActive()) {
			SetHighColor(ui_color(B_KEYBOARD_NAVIGATION_COLOR));
			StrokeEllipse(circle.InsetByCopy(-1, -1));
		}
	}

	virtual void MouseDown(BPoint where)
	{
		if (!IsEnabled())
			return;
		fTracking = true;
		SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS
			| B_NO_POINTER_HISTORY);
		_Track(where);
	}

	virtual void MouseMoved(BPoint where, uint32 transit, const BMessage* drag)
	{
		if (fTracking)
			_Track(where);
	}

	virtual void MouseUp(BPoint where)
	{
		if (!fTracking)
			return;
		_Track(where);
		fTracking = false;
		Invoke();
	}

	// A finger dragged along the slider. On a touch screen in tablet mode a
	// drag arrives as scrolling, in pixels ("be:wheel_pixel_delta_x"), and
	// never as a press: the knob follows the finger from where it was, the
	// modification message goes as it moves and the control's message when
	// the finger lifts. A mouse wheel moves the knob a step a notch.
	virtual void MessageReceived(BMessage* message)
	{
		if (message->what != B_MOUSE_WHEEL_CHANGED || !IsEnabled()) {
			BControl::MessageReceived(message);
			return;
		}
		float pixelsX, pixelsY;
		if (message->FindFloat("be:wheel_pixel_delta_x", &pixelsX) == B_OK
			&& message->FindFloat("be:wheel_pixel_delta_y", &pixelsY) == B_OK) {
			int32 phase = message->GetInt32("be:wheel_phase",
				Detail::kWheelPhaseNone);
			if (!fTracking) {
				fTracking = true;
				fDragValue = Value();
			}
			BRect track = _Track();
			if (track.Width() > 0)
				fDragValue -= pixelsX / track.Width() * 1000;
			fDragValue = fmaxf(0.0f, fminf(1000.0f, fDragValue));
			_SetTracked((int32)roundf(fDragValue));
			if (phase == Detail::kWheelPhaseEnded
				|| phase == Detail::kWheelPhaseNone) {
				fTracking = false;
				Invoke();
			}
			return;
		}
		float delta = message->GetFloat("be:wheel_delta_y", 0);
		if (delta == 0)
			delta = message->GetFloat("be:wheel_delta_x", 0);
		if (delta != 0) {
			SetValue(std::max((int32)0, std::min((int32)1000,
				Value() - (int32)roundf(delta * 50))));
			Invoke();
		}
	}

	virtual void KeyDown(const char* bytes, int32 numBytes)
	{
		if (numBytes == 1 && (bytes[0] == B_LEFT_ARROW
				|| bytes[0] == B_RIGHT_ARROW)) {
			int32 step = bytes[0] == B_LEFT_ARROW ? -50 : 50;
			SetValue(std::max((int32)0, std::min((int32)1000, Value() + step)));
			Invoke();
			return;
		}
		BControl::KeyDown(bytes, numBytes);
	}

private:
	BRect _Track() const
	{
		BRect bounds = Bounds();
		float knob = floorf(ControlHeight() * 0.36f) + 1;
		float speaker = fSpeakers ? floorf(ControlHeight() * 0.42f) + 8 : 0;
		return BRect(bounds.left + speaker + knob, bounds.top,
			bounds.right - speaker - knob, bounds.bottom);
	}

	void _Track(BPoint where)
	{
		BRect track = _Track();
		float fraction = track.Width() > 0
			? (where.x - track.left) / track.Width() : 0;
		_SetTracked((int32)roundf(fmaxf(0.0f, fminf(1.0f, fraction)) * 1000));
	}

	void _SetTracked(int32 value)
	{
		if (value == Value())
			return;
		SetValue(value);
		if (fModification != NULL) {
			BMessage message(*fModification);
			message.AddInt32("be:value", value);
			Invoke(&message);
		}
	}

	BMessage* fModification;
	bool fSpeakers;
	bool fTracking;
	float fDragValue;
};


// #pragma mark - SearchField

// A rounded search field with a magnifier, a placeholder and a clear button.
// The modification message is sent whenever the text changes, with "text";
// Enter sends it again. The inner BTextView brings up the shell keyboard.
class SearchField : public BView {
public:
	SearchField(const char* name, const char* placeholder,
		BMessage* modification)
		:
		BView(name, B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE | B_FRAME_EVENTS),
		fPlaceholder(placeholder),
		fModification(modification)
	{
		SetViewColor(B_TRANSPARENT_COLOR);
		fEditor = new Editor(this);
		AddChild(fEditor);
	}

	virtual ~SearchField()
	{
		delete fModification;
	}

	const char* Text() const { return fEditor->Text(); }

	// Changes the text without sending the modification message.
	void SetText(const char* text)
	{
		if (strcmp(text, fEditor->Text()) == 0)
			return;
		fEditor->SetQuiet(true);
		fEditor->SetText(text);
		fEditor->SetQuiet(false);
		Invalidate();
	}

	BTextView* TextView() const { return fEditor; }
	void SetTarget(const BMessenger& target) { fTarget = target; }

	virtual BSize MinSize()
	{
		return BLayoutUtils::ComposeSize(ExplicitMinSize(),
			BSize(TargetSize() * 4, TargetSize()));
	}

	virtual BSize MaxSize()
	{
		return BLayoutUtils::ComposeSize(ExplicitMaxSize(),
			BSize(B_SIZE_UNLIMITED, TargetSize()));
	}

	virtual void AttachedToWindow()
	{
		BView::AttachedToWindow();
		_LayoutEditor();
	}

	virtual void FrameResized(float width, float height)
	{
		_LayoutEditor();
		Invalidate();
	}

	virtual void Draw(BRect update)
	{
		const Skin& skin = CurrentSkin();
		_ApplyColors();
		FillBarBehind(this, update);
		BRect plate = _Plate();
		float radius = plate.Height() / 2;
		BRect face = DrawPlate(this, plate, radius, skin.fieldBackground,
			skin.fieldBackground);
		// the field is sunk into the metal: a shadow along its top
		PushState();
		BRegion clip(BRect(face.left, face.top, face.right,
			face.top + radius));
		ConstrainClippingRegion(&clip);
		SetDrawingMode(B_OP_ALPHA);
		SetPenSize(1.5f);
		SetHighColor(0, 0, 0, 50);
		StrokeRoundRect(face.OffsetByCopy(0, 0.5f), radius - 1, radius - 1);
		PopState();
		DrawGlyph(this, B_GLYPH_SEARCH, _Magnifier(), skin.fieldPlaceholder);
		if (fEditor->TextLength() > 0) {
			BRect clear = _Clear();
			SetHighColor(skin.fieldPlaceholder);
			FillEllipse(clear);
			DrawGlyph(this, B_GLYPH_CLEAR, clear.InsetByCopy(
				clear.Width() * 0.22f, clear.Height() * 0.22f),
				skin.fieldBackground);
		}
	}

	virtual void MouseDown(BPoint where)
	{
		if (fEditor->TextLength() > 0 && _Clear().InsetByCopy(-4, -4)
				.Contains(where)) {
			fEditor->SetText("");
			fEditor->MakeFocus(true);
			return;
		}
		fEditor->MakeFocus(true);
	}

	void Modified()
	{
		Invalidate();
		if (fModification == NULL)
			return;
		BMessage message(*fModification);
		message.AddString("text", fEditor->Text());
		message.AddPointer("source", this);
		BMessenger target = fTarget.IsValid() ? fTarget
			: BMessenger(Window());
		target.SendMessage(&message);
	}

private:
	class Editor : public BTextView {
	public:
		explicit Editor(SearchField* owner)
			:
			BTextView(BRect(0, 0, 10, 10), "search-text",
				BRect(0, 0, 10, 10), B_FOLLOW_NONE, B_WILL_DRAW | B_NAVIGABLE),
			fOwner(owner),
			fQuiet(false)
		{
			SetWordWrap(false);
			DisallowChar(B_ENTER);
			DisallowChar(B_TAB);
		}

		void SetQuiet(bool quiet) { fQuiet = quiet; }

		virtual void Draw(BRect update)
		{
			BTextView::Draw(update);
			if (TextLength() == 0 && !IsFocus()) {
				const Skin& skin = CurrentSkin();
				font_height height;
				be_plain_font->GetHeight(&height);
				SetHighColor(skin.fieldPlaceholder);
				SetLowColor(ViewColor());
				DrawString(fOwner->fPlaceholder.String(), BPoint(TextRect().left,
					TextRect().top + ceilf(height.ascent)));
			}
		}

		virtual void MakeFocus(bool focus)
		{
			BTextView::MakeFocus(focus);
			Invalidate();
		}

		virtual void KeyDown(const char* bytes, int32 numBytes)
		{
			if (numBytes == 1 && bytes[0] == B_ENTER) {
				fOwner->Modified();
				return;
			}
			if (numBytes == 1 && bytes[0] == B_ESCAPE && TextLength() > 0) {
				SetText("");
				return;
			}
			BTextView::KeyDown(bytes, numBytes);
		}

	protected:
		virtual void InsertText(const char* text, int32 length, int32 offset,
			const text_run_array* runs)
		{
			BTextView::InsertText(text, length, offset, runs);
			if (!fQuiet)
				fOwner->Modified();
		}

		virtual void DeleteText(int32 from, int32 to)
		{
			BTextView::DeleteText(from, to);
			if (!fQuiet)
				fOwner->Modified();
		}

	private:
		SearchField* fOwner;
		bool fQuiet;
	};

	BRect _Plate() const { return VisibleRect(Bounds()); }

	BRect _Magnifier() const
	{
		BRect plate = _Plate();
		float glyph = floorf(plate.Height() * 0.46f);
		float left = plate.left + floorf(plate.Height() * 0.36f);
		float top = floorf((plate.top + plate.bottom - glyph) / 2);
		return BRect(left, top, left + glyph, top + glyph);
	}

	BRect _Clear() const
	{
		BRect plate = _Plate();
		float glyph = floorf(plate.Height() * 0.5f);
		float right = plate.right - floorf(plate.Height() * 0.3f);
		float top = floorf((plate.top + plate.bottom - glyph) / 2);
		return BRect(right - glyph, top, right, top + glyph);
	}

	void _LayoutEditor()
	{
		BRect plate = _Plate();
		font_height height;
		be_plain_font->GetHeight(&height);
		float line = ceilf(height.ascent + height.descent + height.leading) + 2;
		BRect frame(_Magnifier().right + 6,
			floorf((plate.top + plate.bottom - line) / 2),
			_Clear().left - 6, floorf((plate.top + plate.bottom + line) / 2));
		if (!frame.IsValid())
			return;
		fEditor->MoveTo(frame.LeftTop());
		fEditor->ResizeTo(frame.Width(), frame.Height());
		fEditor->SetTextRect(BRect(0, 1, frame.Width(), frame.Height()));
		_ApplyColors();
	}

	// The text area takes the field's colours, again after an Appearance change.
	void _ApplyColors()
	{
		const Skin& skin = CurrentSkin();
		if (fEditor->ViewColor() == skin.fieldBackground
			&& fEditor->LowColor() == skin.fieldBackground)
			return;
		fEditor->SetViewColor(skin.fieldBackground);
		fEditor->SetLowColor(skin.fieldBackground);
		BFont font(be_plain_font);
		// the text there is already, and the text still to be typed
		fEditor->SetFontAndColor(0, fEditor->TextLength(), &font, B_FONT_ALL,
			&skin.fieldText);
		fEditor->SetFontAndColor(&font, B_FONT_ALL, &skin.fieldText);
		fEditor->Invalidate();
	}

	BString fPlaceholder;
	BMessage* fModification;
	BMessenger fTarget;
	Editor* fEditor;
};


// #pragma mark - Wood, pages and reflections

// Fills a B_RGB32 bitmap with wood whose grain runs down its length.
// `seed` varies the figure so two rails do not match.
inline BBitmap*
CreateWoodGrain(int32 width, int32 height, uint32 seed = 0)
{
	if (width < 1 || height < 1)
		return NULL;
	BBitmap* bitmap = new BBitmap(BRect(0, 0, width - 1, height - 1),
		B_RGB32);
	if (!bitmap->IsValid()) {
		delete bitmap;
		return NULL;
	}
	const Skin& skin = CurrentSkin();
	uint8* bits = (uint8*)bitmap->Bits();
	int32 stride = bitmap->BytesPerRow();
	float phase = (seed % 97) * 0.37f;
	for (int32 y = 0; y < height; y++) {
		uint8* row = bits + y * stride;
		for (int32 x = 0; x < width; x++) {
			float u = x + 2.4f * sinf(y * 0.011f + x * 0.23f + phase)
				+ 1.1f * sinf(y * 0.043f + phase * 2.1f);
			float grain = 0.5f + 0.5f * sinf(u * 1.9f + phase);
			float figure = 0.5f + 0.5f * sinf(u * 0.55f + 1.3f
				+ 0.8f * sinf(y * 0.004f + phase));
			uint32 hash = (uint32)(x * 374761393u + (y / 2) * 668265263u
				+ seed * 2246822519u);
			hash = (hash ^ (hash >> 13)) * 1274126177u;
			float noise = ((hash >> 24) & 0xff) / 255.0f;
			float amount = 0.42f * grain * grain + 0.3f * figure
				+ 0.08f * noise;
			rgb_color color = MixColors(skin.woodLight, skin.woodDark, amount);
			row[x * 4 + 0] = color.blue;
			row[x * 4 + 1] = color.green;
			row[x * 4 + 2] = color.red;
			row[x * 4 + 3] = 255;
		}
	}
	return bitmap;
}


// Holds one content view between two wooden rails. With the rails hidden it
// is a plain container, so a window can keep it in its desktop layout.
class WoodFrame : public BGroupView {
public:
	WoodFrame(const char* name, BView* content)
		:
		BGroupView(name, B_VERTICAL, 0),
		fGrain(NULL),
		fRails(false)
	{
		SetFlags(Flags() | B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE);
		SetViewColor(B_TRANSPARENT_COLOR);
		GroupLayout()->AddView(content);
	}

	virtual ~WoodFrame()
	{
		delete fGrain;
	}

	static float RailWidth() { return ceilf(TargetSize() * 0.36f); }

	void SetRails(bool shown)
	{
		if (shown == fRails)
			return;
		fRails = shown;
		float width = shown ? RailWidth() : 0;
		GroupLayout()->SetInsets(width, 0, width, 0);
		Invalidate();
	}

	bool HasRails() const { return fRails; }

	// Builds the grain again in the current skin's wood.
	void ColorsChanged()
	{
		delete fGrain;
		fGrain = NULL;
		Invalidate();
	}

	virtual void Draw(BRect update)
	{
		BRect bounds = Bounds();
		if (!fRails) {
			SetHighColor(ui_color(B_PANEL_BACKGROUND_COLOR));
			FillRect(update);
			return;
		}
		float width = RailWidth();
		int32 height = (int32)bounds.Height() + 1;
		if (fGrain == NULL || fGrain->Bounds().Height() + 1 < height) {
			delete fGrain;
			fGrain = CreateWoodGrain((int32)width * 2, height + 64, 7);
		}
		BRect left(bounds.left, bounds.top, bounds.left + width - 1,
			bounds.bottom);
		BRect right(bounds.right - width + 1, bounds.top, bounds.right,
			bounds.bottom);
		if (fGrain != NULL) {
			BRect source(0, 0, width - 1, bounds.Height());
			DrawBitmap(fGrain, source, left);
			DrawBitmap(fGrain, source.OffsetByCopy(width, 0), right);
		}
		// lit from above: a soft highlight on the outer edges, the content's
		// shadow on the inner ones
		PushState();
		SetDrawingMode(B_OP_ALPHA);
		SetHighColor(255, 255, 255, 40);
		StrokeLine(left.LeftTop(), left.LeftBottom());
		StrokeLine(right.RightTop(), right.RightBottom());
		for (int32 i = 0; i < 3; i++) {
			SetHighColor(0, 0, 0, 90 - i * 28);
			StrokeLine(BPoint(left.right - i, left.top),
				BPoint(left.right - i, left.bottom));
			StrokeLine(BPoint(right.left + i, right.top),
				BPoint(right.left + i, right.bottom));
		}
		PopState();
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what == B_COLORS_UPDATED)
			ColorsChanged();
		BGroupView::MessageReceived(message);
	}

private:
	BBitmap* fGrain;
	bool fRails;
};


// The paper behind grids of covers: a light sheet, lit from above. Pass the
// view's Bounds() so the light stays put while the content scrolls.
inline void
DrawPage(BView* view, BRect visible, BRect update)
{
	const Skin& skin = CurrentSkin();
	BGradientLinear gradient(visible.LeftTop(), visible.LeftBottom());
	gradient.AddColor(skin.pageTop, 0);
	gradient.AddColor(skin.pageBottom, 255);
	view->FillRect(update, gradient);
}


// A mirror image of the lower part of `bitmap` (drawn at `rect`) on the floor
// below it, fading into `floor` over `depth` pixels. Takes B_RGB32 and
// B_RGBA32 bitmaps; others are left without a reflection.
inline void
DrawReflection(BView* view, const BBitmap* bitmap, BRect rect, float depth,
	rgb_color floor)
{
	if (bitmap == NULL || depth < 2 || rect.Height() < 2)
		return;
	color_space space = bitmap->ColorSpace();
	if (space != B_RGB32 && space != B_RGBA32)
		return;
	BRect source = bitmap->Bounds();
	int32 sourceWidth = source.IntegerWidth() + 1;
	int32 sourceHeight = source.IntegerHeight() + 1;
	float scale = (rect.Height() + 1) / sourceHeight;
	int32 rows = std::min(sourceHeight, (int32)ceilf(depth / scale));
	if (rows < 1)
		return;
	BBitmap mirror(BRect(0, 0, sourceWidth - 1, rows - 1), B_RGBA32);
	if (!mirror.IsValid())
		return;
	const uint8* from = (const uint8*)bitmap->Bits();
	uint8* to = (uint8*)mirror.Bits();
	for (int32 y = 0; y < rows; y++) {
		const uint8* in = from + (sourceHeight - 1 - y) * bitmap->BytesPerRow();
		uint8* out = to + y * mirror.BytesPerRow();
		uint8 alpha = (uint8)(110 * (1.0f - (float)y / rows));
		for (int32 x = 0; x < sourceWidth; x++) {
			out[x * 4 + 0] = in[x * 4 + 0];
			out[x * 4 + 1] = in[x * 4 + 1];
			out[x * 4 + 2] = in[x * 4 + 2];
			out[x * 4 + 3] = alpha;
		}
	}
	view->PushState();
	view->SetHighColor(floor);
	BRect target(rect.left, rect.bottom + 1, rect.right,
		rect.bottom + rows * scale);
	view->FillRect(target);
	view->SetDrawingMode(B_OP_ALPHA);
	view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
	view->DrawBitmap(&mirror, mirror.Bounds(), target,
		B_FILTER_BITMAP_BILINEAR);
	view->PopState();
}

// #pragma mark - TitleView (version 3)

// The title of a page in a MetalBar, engraved into the metal and centred.
// It takes the space between the bar's buttons; the text is truncated.
class TitleView : public BView {
public:
	explicit TitleView(const char* name, const char* title = NULL)
		:
		BView(name, B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE),
		fTitle(title)
	{
		SetViewColor(B_TRANSPARENT_COLOR);
	}

	void SetTitle(const char* title)
	{
		if (fTitle == title)
			return;
		fTitle = title;
		Invalidate();
	}

	const char* Title() const { return fTitle.String(); }

	virtual BSize MinSize()
	{
		return BLayoutUtils::ComposeSize(ExplicitMinSize(),
			BSize(TargetSize(), TargetSize()));
	}

	virtual BSize MaxSize()
	{
		return BLayoutUtils::ComposeSize(ExplicitMaxSize(),
			BSize(B_SIZE_UNLIMITED, TargetSize()));
	}

	virtual void Draw(BRect update)
	{
		const Skin& skin = CurrentSkin();
		FillBarBehind(this, update);
		BFont font(be_bold_font);
		font.SetSize(ceilf(be_plain_font->Size() * 1.25f));
		SetFont(&font);
		DrawCentered(this, fTitle.String(), Bounds().InsetByCopy(4, 0),
			skin.controlText, skin.controlEmboss);
	}

private:
	BString fTitle;
};


// #pragma mark - TableView (version 3)

enum RowAccessory {
	B_ROW_NO_ACCESSORY,
	B_ROW_CHEVRON,		// the row opens a page with more
	B_ROW_CHECK,		// the row is the chosen one
	B_ROW_SWITCH		// an ON/OFF switch (SetSwitch); a tap on the row
						// sends its message, the owner flips the setting
};


// A row of a TableView: a picture (a bitmap, which is copied, or a glyph),
// a title, an optional detail line under it and an accessory on the right.
// A tap sends the row's message with "index" to the table's target.
class TableRow : public BListItem {
public:
	TableRow(const char* title, const char* detail = NULL,
		BMessage* message = NULL)
		:
		fTitle(title),
		fDetail(detail),
		fMessage(message),
		fDeleteMessage(NULL),
		fIcon(NULL),
		fGlyph(B_GLYPH_NONE),
		fAccessory(B_ROW_NO_ACCESSORY),
		fHeader(false)
	{
		fTint = MakeTint();
	}

	virtual ~TableRow()
	{
		delete fMessage;
		delete fDeleteMessage;
		delete fIcon;
	}

	static rgb_color MakeTint()
	{
		rgb_color tint = { 108, 112, 120, 255 };
		return tint;
	}

	const char* Title() const { return fTitle.String(); }
	const char* Detail() const { return fDetail.String(); }
	void SetTitle(const char* title) { fTitle = title; }
	void SetDetail(const char* detail) { fDetail = detail; }
	BMessage* Message() const { return fMessage; }
	void SetMessage(BMessage* message) { delete fMessage; fMessage = message; }

	// The message sent when the row is deleted in editing mode; rows
	// without one cannot be deleted.
	void SetDeleteMessage(BMessage* message)
	{
		delete fDeleteMessage;
		fDeleteMessage = message;
	}
	BMessage* DeleteMessage() const { return fDeleteMessage; }

	void SetIcon(const BBitmap* icon)
	{
		delete fIcon;
		fIcon = icon != NULL ? new BBitmap(icon) : NULL;
	}

	void SetGlyph(Glyph glyph, rgb_color tint = MakeTint())
	{
		fGlyph = glyph;
		fTint = tint;
	}

	void SetAccessory(RowAccessory accessory) { fAccessory = accessory; }
	RowAccessory Accessory() const { return fAccessory; }

	// The state a B_ROW_SWITCH shows.
	void SetSwitch(bool on) { fSwitch = on; }
	bool Switch() const { return fSwitch; }

	// A row whose action removes something: its title is red and centred.
	void SetDestructive(bool destructive) { fDestructive = destructive; }

	// A section header: a strip with small capitals, not a tappable row.
	void SetHeader(bool header) { fHeader = header; }
	bool IsHeader() const { return fHeader; }

	virtual void Update(BView* owner, const BFont* font)
	{
		BListItem::Update(owner, font);
		float size = be_plain_font->Size();
		if (fHeader)
			SetHeight(ceilf(size * 2.0f));
		else if (fDetail.IsEmpty())
			SetHeight(fmaxf(TargetSize(), ceilf(size * 3.6f)));
		else
			SetHeight(fmaxf(TargetSize(), ceilf(size * 4.4f)));
	}

	virtual void DrawItem(BView* owner, BRect frame, bool complete = false);

private:
	BString fTitle;
	BString fDetail;
	BMessage* fMessage;
	BMessage* fDeleteMessage;
	BBitmap* fIcon;
	Glyph fGlyph;
	rgb_color fTint;
	RowAccessory fAccessory;
	bool fHeader;
	bool fSwitch = false;
	bool fDestructive = false;
};


// A touch list after the tables of early iPhones: tall rows with pictures,
// a blue highlight under the finger, disclosure chevrons and an editing mode
// in which a red button before a row offers to delete it. Put it in Scroll()
// for flick scrolling. Rows are TableRows; the table owns them.
class TableView : public BListView {
public:
	explicit TableView(const char* name)
		:
		BListView(name, B_SINGLE_SELECTION_LIST, B_WILL_DRAW | B_NAVIGABLE
			| B_FULL_UPDATE_ON_RESIZE | B_FRAME_EVENTS),
		fEditing(false),
		fConfirm(-1),
		fDown(-1)
	{
	}

	virtual ~TableView()
	{
		RemoveRows();
	}

	// Rows send their messages to the table's target (BInvoker::SetTarget),
	// or else to the window.

	void RemoveRows()
	{
		while (BListItem* item = RemoveItem((int32)0))
			delete item;
		fConfirm = fDown = -1;
	}

	TableRow* RowAt(int32 index) const
	{
		return dynamic_cast<TableRow*>(ItemAt(index));
	}

	// Editing: each row with a delete message gets a red button before it.
	void SetEditing(bool editing)
	{
		if (fEditing == editing)
			return;
		fEditing = editing;
		fConfirm = -1;
		Invalidate();
	}
	bool IsEditing() const { return fEditing; }

	// Text shown in the middle while the table has no rows.
	void SetEmptyText(const char* text) { fEmpty = text; Invalidate(); }

	int32 ConfirmingRow() const { return fConfirm; }

	BRect DeleteButtonRect(BRect frame) const
	{
		BFont font(be_bold_font);
		float width = font.StringWidth("Delete") + be_plain_font->Size() * 2;
		float height = fminf(frame.Height() - 10, ControlHeight());
		float top = floorf((frame.top + frame.bottom - height) / 2);
		return BRect(frame.right - width - 8, top, frame.right - 8,
			top + height);
	}

	BRect MinusRect(BRect frame) const
	{
		float size = ceilf(be_plain_font->Size() * 1.6f);
		float top = floorf((frame.top + frame.bottom - size) / 2);
		float left = frame.left + ceilf(be_plain_font->Size() * 0.8f);
		return BRect(left, top, left + size, top + size);
	}

	virtual void Draw(BRect update)
	{
		BListView::Draw(update);
		if (CountItems() == 0 && !fEmpty.IsEmpty()) {
			SetHighColor(ui_color(B_LIST_BACKGROUND_COLOR));
			FillRect(update);
			rgb_color text = ui_color(B_LIST_ITEM_TEXT_COLOR);
			text.alpha = 140;
			BFont font(be_bold_font);
			font.SetSize(ceilf(be_plain_font->Size() * 1.2f));
			SetFont(&font);
			BRect area = Bounds();
			area.bottom = area.top + TargetSize() * 3;
			PushState();
			SetDrawingMode(B_OP_ALPHA);
			SetHighColor(text);
			BString shown(fEmpty);
			font.TruncateString(&shown, B_TRUNCATE_END, area.Width() - 16);
			DrawString(shown.String(), BPoint(floorf((area.left + area.right
				- font.StringWidth(shown.String())) / 2), area.bottom));
			PopState();
		}
	}

	virtual void MouseDown(BPoint where)
	{
		MakeFocus(true);
		BMessage* event = Window()->CurrentMessage();
		if (event != NULL && (event->GetInt32("buttons", B_PRIMARY_MOUSE_BUTTON)
				& B_PRIMARY_MOUSE_BUTTON) == 0) {
			BListView::MouseDown(where);
			return;
		}
		int32 index = IndexOf(where);
		TableRow* row = RowAt(index);
		fDown = -1;
		if (row == NULL || row->IsHeader()) {
			if (fConfirm >= 0)
				_SetConfirm(-1);
			return;
		}
		BRect frame = ItemFrame(index);
		if (fEditing && row->DeleteMessage() != NULL) {
			if (fConfirm == index
				&& DeleteButtonRect(frame).InsetByCopy(-4, -4).Contains(where)) {
				fDown = index;
				fDownOnDelete = true;
				return;
			}
			if (MinusRect(frame).InsetByCopy(-8, -8).Contains(where)) {
				_SetConfirm(fConfirm == index ? -1 : index);
				return;
			}
		}
		if (fConfirm >= 0) {
			_SetConfirm(-1);
			return;
		}
		fDown = index;
		fDownOnDelete = false;
		Select(index);
		SetMouseEventMask(B_POINTER_EVENTS, B_NO_POINTER_HISTORY);
	}

	virtual void MouseMoved(BPoint where, uint32 transit, const BMessage* drag)
	{
		if (fDown >= 0 && !fDownOnDelete && IndexOf(where) != fDown) {
			DeselectAll();
			fDown = -1;
		}
		BListView::MouseMoved(where, transit, drag);
	}

	virtual void MouseUp(BPoint where)
	{
		int32 index = fDown;
		fDown = -1;
		if (index < 0 || IndexOf(where) != index)
			return;
		TableRow* row = RowAt(index);
		if (row == NULL)
			return;
		if (fDownOnDelete) {
			fConfirm = -1;
			_Send(row->DeleteMessage(), index);
			return;
		}
		_Send(row->Message(), index);
		// The highlight stays a moment, then goes, as on the phone.
		DeselectAll();
	}

	virtual bool InitiateDrag(BPoint, int32, bool) { return false; }

	virtual status_t Invoke(BMessage* message = NULL)
	{
		int32 index = CurrentSelection();
		TableRow* row = RowAt(index);
		if (message == NULL && row != NULL && row->Message() != NULL) {
			_Send(row->Message(), index);
			return B_OK;
		}
		return BListView::Invoke(message);
	}

	virtual void KeyDown(const char* bytes, int32 numBytes)
	{
		if (numBytes == 1 && bytes[0] == B_DELETE && fEditing) {
			TableRow* row = RowAt(CurrentSelection());
			if (row != NULL && row->DeleteMessage() != NULL) {
				_Send(row->DeleteMessage(), CurrentSelection());
				return;
			}
		}
		BListView::KeyDown(bytes, numBytes);
	}

private:
	void _SetConfirm(int32 index)
	{
		if (fConfirm >= 0)
			InvalidateItem(fConfirm);
		fConfirm = index;
		if (fConfirm >= 0)
			InvalidateItem(fConfirm);
	}

	void _Send(const BMessage* source, int32 index)
	{
		if (source == NULL)
			return;
		BMessage message(*source);
		message.AddInt32("index", index);
		message.AddPointer("source", this);
		BMessenger target = Messenger().IsValid() ? Messenger()
			: BMessenger(Window());
		target.SendMessage(&message);
	}

	bool fEditing;
	int32 fConfirm;
	int32 fDown;
	bool fDownOnDelete = false;
	BString fEmpty;
};


inline void
TableRow::DrawItem(BView* owner, BRect frame, bool /*complete*/)
{
	const Skin& skin = CurrentSkin();
	TableView* table = dynamic_cast<TableView*>(owner);
	float size = be_plain_font->Size();
	rgb_color background = ui_color(B_LIST_BACKGROUND_COLOR);
	rgb_color text = ui_color(B_LIST_ITEM_TEXT_COLOR);
	owner->PushState();
	if (fHeader) {
		// a strip of brushed metal with the section's name
		FillGradient(owner, frame, MixColors(skin.barTop, background, 0.3f),
			MixColors(skin.barBottom, background, 0.45f));
		owner->SetHighColor(MixColors(skin.barEdge, background, 0.6f));
		owner->StrokeLine(frame.LeftBottom(), frame.RightBottom());
		BFont font(be_bold_font);
		owner->SetFont(&font);
		font_height height;
		font.GetHeight(&height);
		DrawEngraved(owner, fTitle.String(), BPoint(frame.left + size,
			floorf((frame.top + frame.bottom + height.ascent
				- height.descent) / 2)), skin.controlText, skin.controlEmboss);
		owner->PopState();
		return;
	}
	bool selected = IsSelected();
	if (selected) {
		FillGradient(owner, frame, skin.fillTop, skin.fillBottom);
		text = make_color(255, 255, 255);
	} else {
		owner->SetHighColor(background);
		owner->FillRect(frame);
	}
	// the separator under the row
	owner->SetHighColor(MixColors(background, text, 0.16f));
	owner->StrokeLine(BPoint(frame.left, frame.bottom),
		BPoint(frame.right, frame.bottom));
	float left = frame.left + size;
	bool editing = table != NULL && table->IsEditing() && fDeleteMessage != NULL;
	int32 index = table != NULL ? table->IndexOf(this) : -1;
	bool confirming = editing && table->ConfirmingRow() == index;
	if (editing) {
		BRect minus = table->MinusRect(frame);
		owner->SetDrawingMode(B_OP_ALPHA);
		owner->SetHighColor(make_color(150, 20, 20));
		owner->FillEllipse(minus);
		FillGradient(owner, minus.InsetByCopy(1, 1), make_color(240, 92, 86),
			make_color(204, 30, 26), minus.Width() / 2);
		owner->SetHighColor(255, 255, 255);
		owner->SetPenSize(fmaxf(2.0f, roundf(minus.Height() / 7)));
		if (confirming) {
			// the minus turns upright while the Delete button shows
			owner->StrokeLine(BPoint((minus.left + minus.right) / 2,
				minus.top + minus.Height() * 0.26f),
				BPoint((minus.left + minus.right) / 2,
					minus.bottom - minus.Height() * 0.26f));
		} else {
			owner->StrokeLine(BPoint(minus.left + minus.Width() * 0.26f,
				(minus.top + minus.bottom) / 2),
				BPoint(minus.right - minus.Width() * 0.26f,
					(minus.top + minus.bottom) / 2));
		}
		owner->SetPenSize(1);
		left = minus.right + size * 0.8f;
	}
	float picture = ceilf(fminf(frame.Height() - size, size * 2.2f));
	BRect box(left, floorf((frame.top + frame.bottom - picture) / 2),
		left + picture, floorf((frame.top + frame.bottom + picture) / 2));
	if (fIcon != NULL) {
		owner->SetDrawingMode(B_OP_ALPHA);
		owner->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
		BRect source = fIcon->Bounds();
		float scale = fminf(box.Width() / (source.Width() + 1),
			box.Height() / (source.Height() + 1));
		float width = (source.Width() + 1) * scale;
		float height = (source.Height() + 1) * scale;
		BRect target(0, 0, width - 1, height - 1);
		target.OffsetTo(floorf((box.left + box.right - width) / 2),
			floorf((box.top + box.bottom - height) / 2));
		owner->DrawBitmap(fIcon, source, target, B_FILTER_BITMAP_BILINEAR);
		left = box.right + size * 0.8f;
	} else if (fGlyph != B_GLYPH_NONE) {
		DrawGlyph(owner, fGlyph, box.InsetByCopy(picture * 0.12f,
			picture * 0.12f), selected ? text : fTint);
		left = box.right + size * 0.8f;
	}
	float right = frame.right - size;
	if (confirming) {
		BRect button = table->DeleteButtonRect(frame);
		DrawPlate(owner, button, ceilf(button.Height() / 6),
			make_color(236, 84, 78), make_color(190, 24, 20));
		BFont bold(be_bold_font);
		owner->SetFont(&bold);
		DrawCentered(owner, "Delete", button, make_color(255, 255, 255),
			make_color(0, 0, 0, 110));
		right = button.left - size * 0.5f;
	} else if (fAccessory == B_ROW_SWITCH) {
		// the switch of iOS 5: a blue ON or a grey OFF with a round knob
		float height = ceilf(size * 2.2f);
		float width = ceilf(height * 2.4f);
		BRect track(right - width, floorf((frame.top + frame.bottom - height)
			/ 2), right, floorf((frame.top + frame.bottom - height) / 2)
				+ height);
		float radius = height / 2;
		owner->SetDrawingMode(B_OP_ALPHA);
		owner->SetHighColor(make_color(0, 0, 0, 70));
		owner->FillRoundRect(track.OffsetByCopy(0, 1), radius, radius);
		owner->SetHighColor(make_color(120, 122, 128));
		owner->FillRoundRect(track, radius, radius);
		BRect inside = track.InsetByCopy(1, 1);
		if (fSwitch) {
			FillGradient(owner, inside, make_color(40, 104, 214),
				make_color(92, 156, 246), radius - 1);
		} else {
			FillGradient(owner, inside, make_color(214, 215, 218),
				make_color(250, 250, 251), radius - 1);
		}
		BFont bold(be_bold_font);
		bold.SetSize(ceilf(size * 0.95f));
		owner->SetFont(&bold);
		BRect knob(0, 0, height - 1, height - 1);
		knob.OffsetTo(fSwitch ? track.right - height + 1 : track.left,
			track.top);
		BRect label = fSwitch ? BRect(track.left, track.top, knob.left,
			track.bottom) : BRect(knob.right, track.top, track.right,
				track.bottom);
		DrawCentered(owner, fSwitch ? "ON" : "OFF", label,
			fSwitch ? make_color(255, 255, 255) : make_color(110, 112, 118),
			fSwitch ? make_color(0, 0, 0, 80) : make_color(255, 255, 255, 160));
		owner->SetHighColor(make_color(110, 112, 118));
		owner->FillEllipse(knob);
		FillGradient(owner, knob.InsetByCopy(1, 1), make_color(255, 255, 255),
			make_color(206, 207, 212), radius - 1);
		right = track.left - size * 0.6f;
	} else if (fAccessory == B_ROW_CHEVRON || fAccessory == B_ROW_CHECK) {
		float glyph = ceilf(size * (fAccessory == B_ROW_CHEVRON ? 1.1f : 1.4f));
		BRect accessory(right - glyph, floorf((frame.top + frame.bottom
			- glyph) / 2), right, floorf((frame.top + frame.bottom
				+ glyph) / 2));
		rgb_color color = fAccessory == B_ROW_CHECK ? skin.displayActive
			: MixColors(background, text, 0.45f);
		if (selected)
			color = text;
		DrawGlyph(owner, fAccessory == B_ROW_CHEVRON ? B_GLYPH_CHEVRON
			: B_GLYPH_CHECK, accessory, color);
		right = accessory.left - size * 0.6f;
	}
	BFont title(be_bold_font);
	title.SetSize(ceilf(size * 1.15f));
	BFont detail(be_plain_font);
	font_height titleHeight, detailHeight;
	title.GetHeight(&titleHeight);
	detail.GetHeight(&detailHeight);
	float width = fmaxf(0.0f, right - left);
	BString shownTitle(fTitle), shownDetail(fDetail);
	title.TruncateString(&shownTitle, B_TRUNCATE_END, width);
	if (fDestructive) {
		owner->SetDrawingMode(B_OP_ALPHA);
		owner->SetFont(&title);
		owner->SetHighColor(selected ? text : make_color(204, 34, 30));
		font_height height;
		title.GetHeight(&height);
		float textWidth = title.StringWidth(shownTitle.String());
		owner->DrawString(shownTitle.String(), BPoint(floorf((frame.left
			+ frame.right - textWidth) / 2), floorf((frame.top + frame.bottom
				+ height.ascent - height.descent) / 2)));
		owner->PopState();
		return;
	}
	detail.TruncateString(&shownDetail, B_TRUNCATE_MIDDLE, width);
	float block = titleHeight.ascent + titleHeight.descent;
	if (!fDetail.IsEmpty())
		block += detailHeight.ascent + detailHeight.descent + size * 0.2f;
	float baseline = floorf((frame.top + frame.bottom - block) / 2
		+ titleHeight.ascent);
	owner->SetDrawingMode(B_OP_ALPHA);
	owner->SetLowColor(selected ? skin.fillTop : background);
	owner->SetFont(&title);
	owner->SetHighColor(text);
	owner->DrawString(shownTitle.String(), BPoint(left, baseline));
	if (!fDetail.IsEmpty()) {
		owner->SetFont(&detail);
		owner->SetHighColor(selected ? text : MixColors(background, text, 0.6f));
		owner->DrawString(shownDetail.String(), BPoint(left, baseline
			+ titleHeight.descent + size * 0.2f + detailHeight.ascent));
	}
	owner->PopState();
}


// #pragma mark - CardGrid (version 3)

// A grid of cards, each a picture of a page under a strip with its icon, its
// title and a close button, as a browser shows its open tabs. Tap a card to
// open it; tap ✕ or swipe the card sideways off the grid to close it. The
// grid scrolls vertically in Scroll(); give it the aspect of the pictures.
// The open and close messages go with "id" (int64) and "index".
class CardGrid : public BView {
public:
	CardGrid(const char* name, BMessage* open, BMessage* close)
		:
		BView(name, B_WILL_DRAW | B_FRAME_EVENTS | B_FULL_UPDATE_ON_RESIZE
			| B_NAVIGABLE),
		fOpen(open),
		fClose(close),
		fSelected(0),
		fAspect(1.4f),
		fColumns(2),
		fDown(-1),
		fDownOnClose(false),
		fSwiping(-1),
		fSwipe(0)
	{
		SetViewColor(B_TRANSPARENT_COLOR);
	}

	virtual ~CardGrid()
	{
		RemoveCards();
		delete fOpen;
		delete fClose;
	}

	void SetTarget(const BMessenger& target) { fTarget = target; }

	// The pictures are copied; either may be NULL.
	int32 AddCard(int64 id, const char* title, const BBitmap* picture,
		const BBitmap* icon)
	{
		Card card;
		card.id = id;
		card.title = title;
		card.picture = picture != NULL ? new BBitmap(picture) : NULL;
		card.icon = icon != NULL ? new BBitmap(icon) : NULL;
		fCards.push_back(card);
		_Relayout();
		return (int32)fCards.size() - 1;
	}

	void RemoveCards()
	{
		for (size_t i = 0; i < fCards.size(); i++) {
			delete fCards[i].picture;
			delete fCards[i].icon;
		}
		fCards.clear();
		fDown = fSwiping = -1;
		fSwipe = 0;
		_Relayout();
	}

	int32 CountCards() const { return (int32)fCards.size(); }

	// The card drawn with a ring around it (the page on show).
	void SetSelected(int64 id) { fSelected = id; Invalidate(); }

	// Height of a picture over its width.
	void SetAspect(float aspect)
	{
		if (aspect > 0.2f && aspect < 5 && aspect != fAspect) {
			fAspect = aspect;
			_Relayout();
		}
	}

	// What the grid says while it has no cards: a glyph over a heading and
	// lines of explanation (separated by newlines).
	void SetEmptyText(const char* text, Glyph glyph = B_GLYPH_NONE)
	{
		fEmpty = text;
		fEmptyGlyph = glyph;
		Invalidate();
	}

	void ScrollToSelected()
	{
		for (size_t i = 0; i < fCards.size(); i++) {
			if (fCards[i].id != fSelected)
				continue;
			BRect card = CardRect(i);
			BRect bounds = Bounds();
			if (card.top < bounds.top || card.bottom > bounds.bottom) {
				float top = fmaxf(0.0f, card.top - _Gap());
				ScrollTo(0, fminf(top, fmaxf(0.0f, _ContentHeight()
					- bounds.Height())));
			}
			return;
		}
	}

	BRect CardRect(int32 index) const
	{
		float gap = _Gap();
		float width = Bounds().Width() + 1;
		float cardWidth = floorf((width - gap * (fColumns + 1)) / fColumns);
		float cardHeight = _CardHeight(cardWidth);
		int32 row = index / fColumns, column = index % fColumns;
		BRect rect(0, 0, cardWidth - 1, cardHeight - 1);
		rect.OffsetTo(gap + column * (cardWidth + gap),
			gap + row * (cardHeight + gap));
		if (index == fSwiping)
			rect.OffsetBy(fSwipe, 0);
		return rect;
	}

	virtual void FrameResized(float /*width*/, float /*height*/)
	{
		_Relayout();
	}

	virtual void AttachedToWindow()
	{
		BView::AttachedToWindow();
		_Relayout();
	}

	virtual void Draw(BRect update)
	{
		const Skin& skin = CurrentSkin();
		BRect bounds = Bounds();
		// a cloth-coloured table under the cards
		BRect table = bounds;
		FillGradient(this, table, MixColors(skin.barBottom, skin.stage, 0.25f),
			MixColors(skin.barBottom, skin.stage, 0.5f));
		PushState();
		SetDrawingMode(B_OP_ALPHA);
		SetHighColor(255, 255, 255, 10);
		for (float y = floorf(update.top / 3) * 3; y <= update.bottom; y += 3)
			StrokeLine(BPoint(update.left, y), BPoint(update.right, y));
		PopState();
		if (fCards.empty() && !fEmpty.IsEmpty())
			_DrawEmpty(bounds);
		for (size_t i = 0; i < fCards.size(); i++) {
			BRect card = CardRect(i);
			if (card.InsetByCopy(-8, -8).Intersects(update))
				_DrawCard(i, card);
		}
	}

	virtual void MouseDown(BPoint where)
	{
		MakeFocus(true);
		fDown = _CardAt(where);
		fDownOnClose = fDown >= 0
			&& _CloseRect(CardRect(fDown)).InsetByCopy(-6, -6).Contains(where);
		if (fDown >= 0) {
			SetMouseEventMask(B_POINTER_EVENTS, B_NO_POINTER_HISTORY);
			Invalidate(CardRect(fDown).InsetByCopy(-8, -8));
		}
	}

	virtual void MouseMoved(BPoint where, uint32 transit, const BMessage* drag)
	{
		if (fDown >= 0 && _CardAt(where) != fDown) {
			Invalidate(CardRect(fDown).InsetByCopy(-8, -8));
			fDown = -1;
		}
	}

	virtual void MouseUp(BPoint where)
	{
		int32 index = fDown;
		fDown = -1;
		if (index < 0)
			return;
		Invalidate(CardRect(index).InsetByCopy(-8, -8));
		if (_CardAt(where) != index)
			return;
		bool onClose = _CloseRect(CardRect(index)).InsetByCopy(-6, -6)
			.Contains(where);
		_Send(onClose && fDownOnClose ? fClose : fOpen, index);
	}

	// A finger moved sideways along a card (touch scrolling) slides it; let
	// go past a third of its width to close it.
	virtual void MessageReceived(BMessage* message)
	{
		float pixelsX, pixelsY;
		if (message->what == B_MOUSE_WHEEL_CHANGED
			&& message->FindFloat("be:wheel_pixel_delta_x", &pixelsX) == B_OK
			&& message->FindFloat("be:wheel_pixel_delta_y", &pixelsY) == B_OK) {
			int32 phase = message->GetInt32("be:wheel_phase",
				Detail::kWheelPhaseNone);
			if (fSwiping < 0 && fabsf(pixelsX) > fabsf(pixelsY)
				&& phase != Detail::kWheelPhaseEnded) {
				BPoint where;
				uint32 buttons;
				GetMouse(&where, &buttons, false);
				fSwiping = _CardAt(where);
				fSwipe = 0;
			}
			if (fSwiping < 0) {
				_ScrollBy(pixelsY);
				return;
			}
			if (fSwiping >= 0) {
				BRect before = CardRect(fSwiping);
				fSwipe -= pixelsX;
				BRect after = CardRect(fSwiping);
				Invalidate((before | after).InsetByCopy(-10, -10));
				if (phase == Detail::kWheelPhaseEnded
					|| phase == Detail::kWheelPhaseNone) {
					int32 index = fSwiping;
					bool close = fabsf(fSwipe) > before.Width() / 3;
					fSwiping = -1;
					fSwipe = 0;
					Invalidate();
					if (close)
						_Send(fClose, index);
				}
				return;
			}
		}
		if (message->what == B_MOUSE_WHEEL_CHANGED
			&& ScrollBar(B_VERTICAL) == NULL) {
			_ScrollBy(message->GetFloat("be:wheel_delta_y", 0) * TargetSize());
			return;
		}
		BView::MessageReceived(message);
	}

	virtual void KeyDown(const char* bytes, int32 numBytes)
	{
		int32 index = -1;
		for (size_t i = 0; i < fCards.size(); i++) {
			if (fCards[i].id == fSelected)
				index = i;
		}
		int32 next = index;
		if (numBytes == 1) {
			switch (bytes[0]) {
				case B_LEFT_ARROW: next = index - 1; break;
				case B_RIGHT_ARROW: next = index + 1; break;
				case B_UP_ARROW: next = index - fColumns; break;
				case B_DOWN_ARROW: next = index + fColumns; break;
				case B_ENTER: case B_SPACE:
					if (index >= 0)
						_Send(fOpen, index);
					return;
				case B_DELETE: case B_BACKSPACE:
					if (index >= 0)
						_Send(fClose, index);
					return;
				default:
					BView::KeyDown(bytes, numBytes);
					return;
			}
		}
		if (next >= 0 && next < (int32)fCards.size() && next != index) {
			SetSelected(fCards[next].id);
			ScrollToSelected();
		}
	}

private:
	struct Card {
		int64 id;
		BString title;
		BBitmap* picture;
		BBitmap* icon;
	};

	float _Gap() const { return ceilf(be_plain_font->Size() * 1.2f); }

	// Without a scroll bar (in tablet mode a finger scrolls in pixels) the
	// grid scrolls itself.
	void _ScrollBy(float delta)
	{
		if (ScrollBar(B_VERTICAL) != NULL) {
			ScrollBar(B_VERTICAL)->SetValue(ScrollBar(B_VERTICAL)->Value()
				+ delta);
			return;
		}
		float limit = fmaxf(0.0f, _ContentHeight() - Bounds().Height() - 1);
		float top = fmaxf(0.0f, fminf(limit, Bounds().top + delta));
		if (top != Bounds().top)
			ScrollTo(0, top);
	}
	float _Strip() const { return ceilf(TargetSize() * 0.72f); }

	float _CardHeight(float width) const
	{
		// the pictures show the top of the page, cropped shorter than it
		return _Strip() + floorf(width * fminf(fAspect, 1.5f));
	}

	float _ContentHeight() const
	{
		if (fCards.empty())
			return 0;
		BRect last = CardRect(fCards.size() - 1);
		return last.bottom + _Gap();
	}

	void _Relayout()
	{
		// two columns on a phone, three on a tablet, up to six on a wide
		// screen
		float width = Bounds().Width() + 1;
		float column = TargetSize() * 5.2f;
		fColumns = std::max((int32)2, std::min((int32)6,
			(int32)roundf(width / column)));
		if (width < TargetSize() * 5)
			fColumns = 1;
		BScrollBar* bar = ScrollBar(B_VERTICAL);
		if (bar != NULL) {
			float height = Bounds().Height() + 1;
			float range = fmaxf(0.0f, _ContentHeight() - height);
			bar->SetRange(0, range);
			bar->SetProportion(range > 0 ? height / (range + height) : 1);
			bar->SetSteps(TargetSize(), height - TargetSize());
		} else if (Window() != NULL) {
			float limit = fmaxf(0.0f, _ContentHeight() - Bounds().Height() - 1);
			if (Bounds().top > limit)
				ScrollTo(0, limit);
		}
		Invalidate();
	}

	int32 _CardAt(BPoint where) const
	{
		for (size_t i = 0; i < fCards.size(); i++) {
			if (CardRect(i).Contains(where))
				return i;
		}
		return -1;
	}

	BRect _CloseRect(BRect card) const
	{
		float strip = _Strip();
		return BRect(card.right - strip, card.top, card.right,
			card.top + strip - 1);
	}

	void _DrawEmpty(BRect bounds)
	{
		const Skin& skin = CurrentSkin();
		float size = be_plain_font->Size();
		float y = bounds.top + TargetSize() * 2;
		if (fEmptyGlyph != B_GLYPH_NONE) {
			float glyph = ceilf(TargetSize() * 2.2f);
			BRect box(0, 0, glyph, glyph);
			box.OffsetTo(floorf((bounds.left + bounds.right - glyph) / 2), y);
			DrawGlyph(this, fEmptyGlyph, box.OffsetByCopy(0, 1),
				skin.selectedEmboss);
			DrawGlyph(this, fEmptyGlyph, box, skin.selectedText);
			y = box.bottom + size;
		}
		BString text(fEmpty);
		int32 line = 0;
		while (true) {
			int32 end = text.FindFirst('\n');
			BString part;
			text.CopyInto(part, 0, end < 0 ? text.Length() : end);
			BFont font(line == 0 ? be_bold_font : be_plain_font);
			font.SetSize(ceilf(size * (line == 0 ? 1.4f : 1.05f)));
			SetFont(&font);
			font_height height;
			font.GetHeight(&height);
			float lineHeight = ceilf(height.ascent + height.descent
				+ height.leading) + (line == 0 ? size * 0.6f : 2);
			// wrap long lines at spaces
			BString rest(part);
			float width = bounds.Width() - size * 4;
			while (!rest.IsEmpty()) {
				int32 cut = rest.Length();
				while (cut > 0 && font.StringWidth(rest.String(), cut) > width) {
					int32 space = rest.FindLast(' ', cut - 1);
					if (space <= 0) break;
					cut = space;
				}
				BString piece;
				rest.CopyInto(piece, 0, cut);
				DrawCentered(this, piece.String(), BRect(bounds.left, y,
					bounds.right, y + lineHeight), line == 0
						? skin.selectedText : MixColors(skin.selectedText,
							skin.barBottom, 0.25f), skin.selectedEmboss);
				y += lineHeight;
				rest.Remove(0, cut);
				while (rest.ByteAt(0) == ' ')
					rest.Remove(0, 1);
			}
			line++;
			if (end < 0)
				break;
			text.Remove(0, end + 1);
		}
	}

	void _DrawCard(int32 index, BRect card)
	{
		const Skin& skin = CurrentSkin();
		const Card& item = fCards[index];
		float radius = ceilf(be_plain_font->Size() * 0.5f);
		bool fading = index == fSwiping;
		float fade = fading ? fmaxf(0.25f, 1 - fabsf(fSwipe)
			/ fmaxf(1.0f, card.Width())) : 1;
		PushState();
		SetDrawingMode(B_OP_ALPHA);
		SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
		// a soft shadow under the card
		for (int i = 3; i >= 1; i--) {
			SetHighColor(0, 0, 0, (uint8)(28 * fade));
			FillRoundRect(card.OffsetByCopy(0, i).InsetByCopy(-i, -i * 0.5f),
				radius + i, radius + i);
		}
		bool selected = item.id == fSelected;
		if (selected) {
			rgb_color ring = skin.displayActive;
			ring.alpha = (uint8)(255 * fade);
			SetHighColor(ring);
			float width = ceilf(be_plain_font->Size() / 4);
			FillRoundRect(card.InsetByCopy(-width - 1, -width - 1),
				radius + width, radius + width);
		}
		rgb_color paper = skin.pageTop;
		paper.alpha = (uint8)(255 * fade);
		SetHighColor(paper);
		FillRoundRect(card, radius, radius);
		// the strip with the icon, the title and ✕
		float strip = _Strip();
		BRect header(card.left, card.top, card.right, card.top + strip - 1);
		BRegion headerClip(header);
		PushState();
		ConstrainClippingRegion(&headerClip);
		rgb_color top = skin.controlTop, bottom = skin.controlBottom;
		if (index == fDown && !fDownOnClose) {
			top = skin.pressedTop;
			bottom = skin.pressedBottom;
		}
		top.alpha = bottom.alpha = (uint8)(255 * fade);
		FillGradient(this, card, top, bottom, radius);
		PopState();
		SetHighColor(skin.controlBorder);
		StrokeLine(BPoint(header.left, header.bottom),
			BPoint(header.right, header.bottom));
		float iconSize = floorf(strip * 0.5f);
		BRect icon(0, 0, iconSize - 1, iconSize - 1);
		icon.OffsetTo(header.left + floorf(strip * 0.25f),
			floorf((header.top + header.bottom - iconSize) / 2));
		if (item.icon != NULL) {
			SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
			DrawBitmap(item.icon, item.icon->Bounds(), icon,
				B_FILTER_BITMAP_BILINEAR);
			SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
		} else {
			DrawGlyph(this, B_GLYPH_GLOBE, icon, skin.controlText);
		}
		BRect close = _CloseRect(card);
		float cross = floorf(strip * 0.34f);
		BRect crossRect(0, 0, cross, cross);
		crossRect.OffsetTo(floorf((close.left + close.right - cross) / 2),
			floorf((close.top + close.bottom - cross) / 2));
		DrawGlyph(this, B_GLYPH_STOP, crossRect,
			index == fDown && fDownOnClose ? skin.displayActive
				: skin.controlText);
		BFont font(be_bold_font);
		font.SetSize(be_plain_font->Size());
		SetFont(&font);
		BRect title(icon.right + floorf(strip * 0.2f), header.top,
			close.left, header.bottom);
		BString shown(item.title);
		font.TruncateString(&shown, B_TRUNCATE_END, title.Width());
		font_height height;
		font.GetHeight(&height);
		SetHighColor(skin.controlText);
		DrawString(shown.String(), BPoint(title.left,
			floorf((title.top + title.bottom + height.ascent
				- height.descent) / 2)));
		// the picture of the page, its top filling the card's width
		BRect picture(card.left, header.bottom + 1, card.right, card.bottom);
		if (item.picture != NULL) {
			BRect source = item.picture->Bounds();
			float scale = (picture.Width() + 1) / (source.Width() + 1);
			float visible = (picture.Height() + 1) / scale;
			if (visible < source.Height() + 1)
				source.bottom = source.top + floorf(visible) - 1;
			else
				picture.bottom = picture.top + floorf((source.Height() + 1)
					* scale) - 1;
			BRegion clip(picture);
			// round the bottom corners off
			PushState();
			ConstrainClippingRegion(&clip);
			SetDrawingMode(B_OP_COPY);
			DrawBitmap(item.picture, source, picture, B_FILTER_BITMAP_BILINEAR);
			PopState();
		} else {
			float glyph = floorf(fminf(picture.Width(), picture.Height()) / 3);
			BRect box(0, 0, glyph, glyph);
			box.OffsetTo(floorf((picture.left + picture.right - glyph) / 2),
				floorf((picture.top + picture.bottom - glyph) / 2));
			DrawGlyph(this, B_GLYPH_GLOBE, box, skin.pageSecondary);
		}
		SetHighColor(MixColors(skin.controlBorder, paper, 0.3f));
		StrokeRoundRect(card, radius, radius);
		PopState();
	}

	void _Send(const BMessage* source, int32 index)
	{
		if (source == NULL || index < 0 || index >= (int32)fCards.size())
			return;
		BMessage message(*source);
		message.AddInt64("id", fCards[index].id);
		message.AddInt32("index", index);
		BMessenger target = fTarget.IsValid() ? fTarget : BMessenger(Window());
		target.SendMessage(&message);
	}

	BMessage* fOpen;
	BMessage* fClose;
	BMessenger fTarget;
	Glyph fEmptyGlyph = B_GLYPH_NONE;
	std::vector<Card> fCards;
	int64 fSelected;
	float fAspect;
	int32 fColumns;
	int32 fDown;
	bool fDownOnClose;
	int32 fSwiping;
	float fSwipe;
	BString fEmpty;
};


// #pragma mark - TabBar (version 3)

// A row of tabs under a MetalBar, as the iPad's Safari had: the tab on show
// is lit and joins the page, the others sit back in the metal. A tab has an
// icon (copied) and a title; the tab on show has ✕. Tabs share the width down
// to a touch-sized minimum, then the row scrolls sideways. The messages go
// with "id" (int64): select when a tab is tapped, close for its ✕.
class TabBar : public BView {
public:
	TabBar(const char* name, BMessage* select, BMessage* close)
		:
		BView(name, B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE | B_FRAME_EVENTS),
		fSelect(select),
		fClose(close),
		fSelected(0),
		fScroll(0),
		fDown(-1),
		fDownOnClose(false)
	{
		SetViewColor(B_TRANSPARENT_COLOR);
	}

	virtual ~TabBar()
	{
		RemoveTabs();
		delete fSelect;
		delete fClose;
	}

	void SetTarget(const BMessenger& target) { fTarget = target; }

	void RemoveTabs()
	{
		for (size_t i = 0; i < fTabs.size(); i++)
			delete fTabs[i].icon;
		fTabs.clear();
		fDown = -1;
	}

	void AddTab(int64 id, const char* title, const BBitmap* icon)
	{
		Tab tab;
		tab.id = id;
		tab.title = title;
		tab.icon = icon != NULL ? new BBitmap(icon) : NULL;
		fTabs.push_back(tab);
	}

	// Call after adding the tabs: shows the selected one.
	void SetSelected(int64 id)
	{
		fSelected = id;
		_Reveal();
		Invalidate();
	}

	virtual BSize MinSize()
	{
		return BLayoutUtils::ComposeSize(ExplicitMinSize(),
			BSize(TargetSize() * 2, _Height()));
	}

	virtual BSize MaxSize()
	{
		return BLayoutUtils::ComposeSize(ExplicitMaxSize(),
			BSize(B_SIZE_UNLIMITED, _Height()));
	}

	virtual BSize PreferredSize()
	{
		return BLayoutUtils::ComposeSize(ExplicitPreferredSize(), MinSize());
	}

	virtual void FrameResized(float, float)
	{
		_Reveal();
		Invalidate();
	}

	virtual void Draw(BRect update)
	{
		const Skin& skin = CurrentSkin();
		BRect bounds = Bounds();
		// the slot the tabs sit in: darker metal with a shadow at the top
		rgb_color slotTop = MixColors(skin.barBottom, skin.barEdge, 0.35f);
		rgb_color slotBottom = MixColors(skin.barBottom, skin.barEdge, 0.2f);
		FillGradient(this, bounds, slotTop, slotBottom);
		PushState();
		SetDrawingMode(B_OP_ALPHA);
		SetHighColor(0, 0, 0, 60);
		StrokeLine(bounds.LeftTop(), bounds.RightTop());
		PopState();
		int32 selected = -1;
		for (size_t i = 0; i < fTabs.size(); i++) {
			if (fTabs[i].id == fSelected) {
				selected = i;
				continue;
			}
			BRect tab = TabRect(i);
			if (tab.Intersects(update))
				_DrawTab(i, tab, false);
		}
		// the edge the content starts at, under every tab but the lit one
		SetHighColor(skin.barEdge);
		StrokeLine(bounds.LeftBottom(), bounds.RightBottom());
		if (selected >= 0)
			_DrawTab(selected, TabRect(selected), true);
	}

	BRect TabRect(int32 index) const
	{
		float width = _TabWidth();
		BRect bounds = Bounds();
		float left = bounds.left + index * width - fScroll;
		return BRect(left, bounds.top + 3, left + width - 1, bounds.bottom);
	}

	virtual void MouseDown(BPoint where)
	{
		fDown = _TabAt(where);
		fDownOnClose = fDown >= 0 && fTabs[fDown].id == fSelected
			&& _CloseRect(TabRect(fDown)).InsetByCopy(-6, -6).Contains(where);
		if (fDown >= 0)
			SetMouseEventMask(B_POINTER_EVENTS, B_NO_POINTER_HISTORY);
	}

	virtual void MouseUp(BPoint where)
	{
		int32 index = fDown;
		fDown = -1;
		if (index < 0 || _TabAt(where) != index)
			return;
		bool onClose = fDownOnClose && _CloseRect(TabRect(index))
			.InsetByCopy(-6, -6).Contains(where);
		_Send(onClose ? fClose : fSelect, index);
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what == B_MOUSE_WHEEL_CHANGED) {
			float pixels;
			float delta = 0;
			if (message->FindFloat("be:wheel_pixel_delta_x", &pixels) == B_OK)
				delta = pixels;
			if (delta == 0 && message->FindFloat("be:wheel_pixel_delta_y",
					&pixels) == B_OK)
				delta = pixels;
			if (delta == 0) {
				delta = (message->GetFloat("be:wheel_delta_x", 0)
					+ message->GetFloat("be:wheel_delta_y", 0)) * _TabWidth()
					/ 2;
			}
			_ScrollTo(fScroll + delta);
			return;
		}
		BView::MessageReceived(message);
	}

private:
	struct Tab {
		int64 id;
		BString title;
		BBitmap* icon;
	};

	static float _Height()
	{
		return ceilf(TargetSize() * 0.82f);
	}

	float _TabWidth() const
	{
		float width = Bounds().Width() + 1;
		if (fTabs.empty())
			return width;
		float each = floorf(width / fTabs.size());
		return fmaxf(TargetSize() * 3.4f, fminf(each, TargetSize() * 6.5f));
	}

	int32 _TabAt(BPoint where) const
	{
		for (size_t i = 0; i < fTabs.size(); i++) {
			if (TabRect(i).Contains(where))
				return i;
		}
		return -1;
	}

	BRect _CloseRect(BRect tab) const
	{
		float size = tab.Height();
		return BRect(tab.left + 2, tab.top, tab.left + 2 + size, tab.bottom);
	}

	void _ScrollTo(float scroll)
	{
		float limit = fmaxf(0.0f, fTabs.size() * _TabWidth()
			- Bounds().Width() - 1);
		scroll = fmaxf(0.0f, fminf(limit, scroll));
		if (scroll != fScroll) {
			fScroll = scroll;
			Invalidate();
		}
	}

	void _Reveal()
	{
		for (size_t i = 0; i < fTabs.size(); i++) {
			if (fTabs[i].id != fSelected)
				continue;
			float width = _TabWidth();
			float left = i * width;
			if (left < fScroll)
				_ScrollTo(left);
			else if (left + width > fScroll + Bounds().Width() + 1)
				_ScrollTo(left + width - Bounds().Width() - 1);
			else
				_ScrollTo(fScroll);
			return;
		}
		_ScrollTo(fScroll);
	}

	void _DrawTab(int32 index, BRect tab, bool lit)
	{
		const Skin& skin = CurrentSkin();
		const Tab& item = fTabs[index];
		float slope = floorf(tab.Height() * 0.3f);
		BPoint outline[4] = {
			BPoint(tab.left, tab.bottom),
			BPoint(tab.left + slope, tab.top),
			BPoint(tab.right - slope, tab.top),
			BPoint(tab.right, tab.bottom) };
		PushState();
		SetDrawingMode(B_OP_ALPHA);
		SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
		BGradientLinear gradient(tab.LeftTop(), tab.LeftBottom());
		if (lit) {
			gradient.AddColor(skin.barTop, 0);
			gradient.AddColor(MixColors(skin.barTop, skin.barBottom, 0.45f), 255);
		} else {
			gradient.AddColor(MixColors(skin.barBottom, skin.barEdge, 0.12f), 0);
			gradient.AddColor(MixColors(skin.barBottom, skin.barEdge, 0.28f), 255);
		}
		FillPolygon(outline, 4, gradient);
		SetHighColor(lit ? skin.barHighlight : MixColors(skin.barTop,
			skin.barBottom, 0.6f));
		StrokeLine(outline[1] + BPoint(1, 1), outline[2] + BPoint(-1, 1));
		SetHighColor(skin.barEdge);
		StrokeLine(outline[0], outline[1]);
		StrokeLine(outline[1], outline[2]);
		StrokeLine(outline[2], outline[3]);
		PopState();
		float size = be_plain_font->Size();
		BRect content(tab.left + slope, tab.top, tab.right - slope, tab.bottom);
		if (lit) {
			BRect close = _CloseRect(tab).OffsetByCopy(slope * 0.6f, 0);
			float cross = floorf(close.Height() * 0.32f);
			BRect box(0, 0, cross, cross);
			box.OffsetTo(floorf((close.left + close.right - cross) / 2),
				floorf((close.top + close.bottom - cross) / 2));
			DrawGlyph(this, B_GLYPH_STOP, box, skin.controlText);
		}
		BFont font(lit ? be_bold_font : be_plain_font);
		font.SetSize(size);
		SetFont(&font);
		float icon = floorf(tab.Height() * 0.46f);
		BString title(item.title);
		float textWidth = fminf(font.StringWidth(title.String()),
			content.Width() - icon * 2.6f);
		font.TruncateString(&title, B_TRUNCATE_END, textWidth);
		textWidth = font.StringWidth(title.String());
		float total = icon + size * 0.4f + textWidth;
		float left = floorf((content.left + content.right - total) / 2);
		if (lit)
			left = fmaxf(left, content.left + icon * 1.4f);
		BRect iconRect(left, floorf((tab.top + tab.bottom - icon) / 2),
			left + icon - 1, floorf((tab.top + tab.bottom - icon) / 2) + icon - 1);
		if (item.icon != NULL) {
			PushState();
			SetDrawingMode(B_OP_ALPHA);
			SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
			DrawBitmap(item.icon, item.icon->Bounds(), iconRect,
				B_FILTER_BITMAP_BILINEAR);
			PopState();
		} else
			DrawGlyph(this, B_GLYPH_GLOBE, iconRect, skin.controlText);
		font_height height;
		font.GetHeight(&height);
		// The tabs behind sit in darker metal: their titles are darker still
		// on aluminium (and stay light on graphite) so they read as well as
		// the lit tab's.
		rgb_color text = skin.controlText;
		if (!lit && !IsDarkSkin())
			text = MixColors(text, make_color(0, 0, 0), 0.5f);
		DrawEngraved(this, title.String(), BPoint(iconRect.right + 1
			+ size * 0.4f, floorf((tab.top + tab.bottom + height.ascent
				- height.descent) / 2)), text, skin.controlEmboss);
	}

	void _Send(const BMessage* source, int32 index)
	{
		if (source == NULL || index < 0 || index >= (int32)fTabs.size())
			return;
		BMessage message(*source);
		message.AddInt64("id", fTabs[index].id);
		BMessenger target = fTarget.IsValid() ? fTarget : BMessenger(Window());
		target.SendMessage(&message);
	}

	BMessage* fSelect;
	BMessage* fClose;
	BMessenger fTarget;
	std::vector<Tab> fTabs;
	int64 fSelected;
	float fScroll;
	int32 fDown;
	bool fDownOnClose;
};


// #pragma mark - ActionSheet

enum ActionStyle {
	B_ACTION_DEFAULT,
	B_ACTION_DESTRUCTIVE	// red, for an action that removes something
};


// A transient sheet of commands that rises from the bottom of its window,
// after the share sheets of early iPhones: an optional row of activities
// (large glyph buttons with a caption), then a column of glossy buttons, then
// Cancel. Each choice carries its original BMessage (including IDs or refs),
// sent to the owner. Escape, Cancel or a tap above the sheet dismisses it.
// A long sheet scrolls.
class ActionSheet : public BWindow {
public:
	ActionSheet(BWindow* owner, const char* title)
		:
		BWindow(owner->Frame(), title, B_NO_BORDER_WINDOW_LOOK,
			owner->Feel() == B_MODAL_APP_WINDOW_FEEL ? B_MODAL_APP_WINDOW_FEEL
				: B_MODAL_SUBSET_WINDOW_FEEL, B_NOT_ZOOMABLE | B_NOT_RESIZABLE
				| B_NOT_MOVABLE | B_ASYNCHRONOUS_CONTROLS | B_CLOSE_ON_ESCAPE),
		fOwner(owner),
		fArea(owner->Frame())
	{
		if (Feel() == B_MODAL_SUBSET_WINDOW_FEEL)
			AddToSubset(owner);
		fSheet = new Sheet(this, title);
		AddChild(fSheet);
	}

	// A button in the column.
	void AddAction(const char* label, const BMessage& action,
		ActionStyle style = B_ACTION_DEFAULT)
	{
		fSheet->Add(label, action, B_GLYPH_NONE, style, false);
	}

	// A button with a glyph in the row of activities above the column.
	void AddActivity(const char* label, Glyph glyph, const BMessage& action)
	{
		fSheet->Add(label, action, glyph, B_ACTION_DEFAULT, true);
	}

	void Open()
	{
		float width = fArea.Width() + 1;
		fSheet->ResizeTo(width - 1, 10);
		float content = fSheet->ContentHeight(width);
		float height = fminf(fArea.Height() * 0.85f, content);
		ResizeTo(width - 1, height - 1);
		fSheet->ResizeTo(width - 1, height - 1);
		MoveTo(fArea.left, fArea.bottom - height + 1);
		fSheet->MakeFocus(true);
		Show();
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what != 'mVok') {
			BWindow::MessageReceived(message);
			return;
		}
		const BMessage* action = fSheet->ActionAt(message->GetInt32("index", -1));
		if (action != NULL) {
			fOwner.SendMessage(const_cast<BMessage*>(action));
			PostMessage(B_QUIT_REQUESTED);
		}
	}

	virtual void WindowActivated(bool active)
	{
		// A tap outside the sheet (on its window) puts it away.
		if (!active && IsHidden() == false && fShown)
			PostMessage(B_QUIT_REQUESTED);
		if (active)
			fShown = true;
		BWindow::WindowActivated(active);
	}

private:
	struct Choice {
		BString label;
		BMessage action;
		Glyph glyph;
		ActionStyle style;
		bool activity;
	};

	class Sheet : public BView {
	public:
		Sheet(ActionSheet* owner, const char* title)
			:
			BView(BRect(0, 0, 10, 10), "action-sheet", B_FOLLOW_ALL,
				B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE | B_NAVIGABLE),
			fTitle(title),
			fPressed(-1),
			fFocus(-1),
			fScroll(0)
		{
			SetViewColor(B_TRANSPARENT_COLOR);
			(void)owner;
		}

		void Add(const char* label, const BMessage& action, Glyph glyph,
			ActionStyle style, bool activity)
		{
			Choice choice;
			choice.label = label;
			choice.action = action;
			choice.glyph = glyph;
			choice.style = style;
			choice.activity = activity;
			fChoices.push_back(choice);
		}

		const BMessage* ActionAt(int32 index) const
		{
			if (index < 0 || index >= (int32)fChoices.size())
				return NULL;
			return &fChoices[index].action;
		}

		float ContentHeight(float width)
		{
			_Layout(width);
			return fContent;
		}

		virtual void FrameResized(float width, float height)
		{
			_Layout(width + 1);
			fScroll = fmaxf(0.0f, fminf(fScroll, fContent - height - 1));
		}

		virtual void Draw(BRect update)
		{
			BRect bounds = Bounds();
			float size = be_plain_font->Size();
			// a slab of dark glass
			FillGradient(this, bounds, Detail::MakeColor(64, 68, 78),
				Detail::MakeColor(22, 24, 30));
			SetHighColor(Detail::MakeColor(120, 126, 140));
			StrokeLine(bounds.LeftTop(), bounds.RightTop());
			SetHighColor(Detail::MakeColor(0, 0, 0));
			StrokeLine(BPoint(bounds.left, bounds.top + 1),
				BPoint(bounds.right, bounds.top + 1));
			PushState();
			SetOrigin(0, -fScroll);
			if (!fTitle.IsEmpty()) {
				BFont font(be_plain_font);
				SetFont(&font);
				DrawCentered(this, fTitle.String(), fTitleRect,
					Detail::MakeColor(210, 214, 222), Detail::MakeColor(0, 0, 0, 160));
			}
			for (size_t i = 0; i <= fChoices.size(); i++)
				_DrawChoice(i);
			PopState();
			(void)update;
			(void)size;
		}

		virtual void MouseDown(BPoint where)
		{
			MakeFocus(true);
			fPressed = _ChoiceAt(where);
			if (fPressed < 0 && where.y + fScroll < fTop)
				Window()->PostMessage(B_QUIT_REQUESTED);
			SetMouseEventMask(B_POINTER_EVENTS, B_NO_POINTER_HISTORY);
			Invalidate();
		}

		virtual void MouseMoved(BPoint where, uint32, const BMessage*)
		{
			if (fPressed >= 0 && _ChoiceAt(where) != fPressed) {
				fPressed = -1;
				Invalidate();
			}
		}

		virtual void MouseUp(BPoint where)
		{
			int32 pressed = fPressed;
			fPressed = -1;
			Invalidate();
			if (pressed >= 0 && _ChoiceAt(where) == pressed)
				_Choose(pressed);
		}

		virtual void MessageReceived(BMessage* message)
		{
			if (message->what == B_MOUSE_WHEEL_CHANGED) {
				float pixels;
				float delta = message->FindFloat("be:wheel_pixel_delta_y",
					&pixels) == B_OK ? pixels
					: message->GetFloat("be:wheel_delta_y", 0) * TargetSize();
				_ScrollTo(fScroll + delta);
				return;
			}
			BView::MessageReceived(message);
		}

		virtual void KeyDown(const char* bytes, int32 numBytes)
		{
			int32 count = (int32)fChoices.size() + 1;
			if (numBytes == 1) {
				switch (bytes[0]) {
					case B_DOWN_ARROW: case B_RIGHT_ARROW: case B_TAB:
						fFocus = (fFocus + 1) % count;
						_Reveal(fFocus);
						Invalidate();
						return;
					case B_UP_ARROW: case B_LEFT_ARROW:
						fFocus = (fFocus + count - 1) % count;
						_Reveal(fFocus);
						Invalidate();
						return;
					case B_ENTER: case B_SPACE:
						if (fFocus >= 0)
							_Choose(fFocus);
						return;
				}
			}
			BView::KeyDown(bytes, numBytes);
		}

	private:
		void _Layout(float width)
		{
			float size = be_plain_font->Size();
			float margin = ceilf(size * 1.4f);
			float y = ceilf(size * 0.8f);
			fTitleRect = BRect();
			if (!fTitle.IsEmpty()) {
				fTitleRect = BRect(margin, y, width - margin, y + ceilf(size * 2));
				y = fTitleRect.bottom + ceilf(size * 0.5f);
			}
			fTop = 0;
			fRects.assign(fChoices.size() + 1, BRect());
			// activities: square buttons in rows
			float tile = ceilf(TargetSize() * 1.35f);
			float caption = ceilf(size * 2.6f);
			int32 perRow = std::max((int32)1, (int32)((width - margin * 2)
				/ (tile + size * 1.6f)));
			perRow = std::min(perRow, (int32)6);
			float gap = perRow > 1 ? floorf((width - margin * 2 - tile * perRow)
				/ (perRow - 1)) : 0;
			int32 column = 0;
			bool any = false;
			for (size_t i = 0; i < fChoices.size(); i++) {
				if (!fChoices[i].activity)
					continue;
				any = true;
				float left = margin + column * (tile + gap);
				fRects[i] = BRect(left, y, left + tile - 1, y + tile - 1);
				if (++column == perRow) {
					column = 0;
					y += tile + caption;
				}
			}
			if (column > 0)
				y += tile + caption;
			if (any)
				y += ceilf(size * 0.4f);
			float button = ceilf(ControlHeight() * 1.2f);
			float spacing = ceilf(size * 0.7f);
			for (size_t i = 0; i < fChoices.size(); i++) {
				if (fChoices[i].activity)
					continue;
				fRects[i] = BRect(margin, y, width - margin - 1, y + button - 1);
				y += button + spacing;
			}
			y += spacing;
			// Cancel, set apart
			fRects[fChoices.size()] = BRect(margin, y, width - margin - 1,
				y + button - 1);
			y += button + ceilf(size * 1.6f);
			fContent = y;
		}

		int32 _ChoiceAt(BPoint where) const
		{
			where.y += fScroll;
			for (size_t i = 0; i < fRects.size(); i++) {
				if (fRects[i].Contains(where))
					return i;
			}
			return -1;
		}

		void _Choose(int32 index)
		{
			if (index == (int32)fChoices.size()) {
				Window()->PostMessage(B_QUIT_REQUESTED);
				return;
			}
			BMessage choose('mVok');
			choose.AddInt32("index", index);
			Window()->PostMessage(&choose);
		}

		void _ScrollTo(float scroll)
		{
			float limit = fmaxf(0.0f, fContent - Bounds().Height() - 1);
			scroll = fmaxf(0.0f, fminf(scroll, limit));
			if (scroll != fScroll) {
				fScroll = scroll;
				Invalidate();
			}
		}

		void _Reveal(int32 index)
		{
			if (index < 0 || index >= (int32)fRects.size())
				return;
			BRect rect = fRects[index];
			float height = Bounds().Height();
			if (rect.top < fScroll)
				_ScrollTo(rect.top - 8);
			else if (rect.bottom > fScroll + height)
				_ScrollTo(rect.bottom - height + 8);
		}

		void _DrawChoice(size_t index)
		{
			bool cancel = index == fChoices.size();
			BRect rect = fRects[index];
			bool pressed = (int32)index == fPressed;
			bool focused = (int32)index == fFocus && IsFocus();
			float size = be_plain_font->Size();
			rgb_color top, bottom, text, emboss;
			const Choice* choice = cancel ? NULL : &fChoices[index];
			if (pressed) {
				top = Detail::MakeColor(92, 150, 240);
				bottom = Detail::MakeColor(30, 92, 210);
				text = Detail::MakeColor(255, 255, 255);
				emboss = Detail::MakeColor(0, 0, 0, 120);
			} else if (cancel) {
				top = Detail::MakeColor(86, 90, 100);
				bottom = Detail::MakeColor(34, 36, 42);
				text = Detail::MakeColor(255, 255, 255);
				emboss = Detail::MakeColor(0, 0, 0, 160);
			} else if (choice->style == B_ACTION_DESTRUCTIVE) {
				top = Detail::MakeColor(236, 96, 90);
				bottom = Detail::MakeColor(186, 22, 20);
				text = Detail::MakeColor(255, 255, 255);
				emboss = Detail::MakeColor(0, 0, 0, 120);
			} else {
				top = Detail::MakeColor(255, 255, 255);
				bottom = Detail::MakeColor(206, 208, 214);
				text = Detail::MakeColor(30, 32, 36);
				emboss = Detail::MakeColor(255, 255, 255, 200);
			}
			float radius = ceilf(size * 0.7f);
			PushState();
			SetDrawingMode(B_OP_ALPHA);
			SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
			SetHighColor(0, 0, 0, 200);
			FillRoundRect(rect.InsetByCopy(-1, -1), radius + 1, radius + 1);
			PopState();
			FillGradient(this, rect, top, bottom, radius);
			// the gloss over the upper half
			PushState();
			SetDrawingMode(B_OP_ALPHA);
			SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
			BRect gloss = rect.InsetByCopy(1, 1);
			gloss.bottom = floorf((rect.top + rect.bottom) / 2);
			SetHighColor(255, 255, 255, cancel || pressed ? 30 : 70);
			FillRoundRect(gloss, radius - 1, radius - 1);
			if (focused) {
				SetHighColor(ui_color(B_KEYBOARD_NAVIGATION_COLOR));
				SetPenSize(2);
				StrokeRoundRect(rect.InsetByCopy(-2, -2), radius + 2, radius + 2);
			}
			PopState();
			BFont font(be_bold_font);
			font.SetSize(ceilf(size * 1.15f));
			if (choice != NULL && choice->activity) {
				float glyph = floorf(rect.Height() * 0.5f);
				BRect box(0, 0, glyph, glyph);
				box.OffsetTo(floorf((rect.left + rect.right - glyph) / 2),
					floorf((rect.top + rect.bottom - glyph) / 2));
				DrawGlyph(this, choice->glyph, box.OffsetByCopy(0, 1), emboss);
				DrawGlyph(this, choice->glyph, box, text);
				BFont small(be_plain_font);
				small.SetSize(ceilf(size * 0.92f));
				SetFont(&small);
				BRect label(rect.left - size, rect.bottom + 2, rect.right + size,
					rect.bottom + ceilf(size * 2.2f));
				DrawCentered(this, choice->label.String(), label,
					Detail::MakeColor(236, 238, 242), Detail::MakeColor(0, 0, 0, 170));
				return;
			}
			SetFont(&font);
			DrawCentered(this, cancel ? "Cancel" : choice->label.String(),
				rect.InsetByCopy(size, 0), text, emboss);
		}

		BString fTitle;
		std::vector<Choice> fChoices;
		std::vector<BRect> fRects;
		BRect fTitleRect;
		float fTop;
		float fContent = 0;
		int32 fPressed;
		int32 fFocus;
		float fScroll;
	};

	BMessenger fOwner;
	BRect fArea;
	Sheet* fSheet;
	bool fShown = false;
};


// #pragma mark - Toast (version 3)

// A dark rounded square in the middle of a window with a glyph and a line of
// text that goes by itself after a moment, to confirm that something was
// done ("Bookmark saved"). It takes no input and no focus.
class Toast : public BWindow {
public:
	Toast(BWindow* owner, const char* text, Glyph glyph = B_GLYPH_CHECK,
		bigtime_t duration = 1400000)
		:
		BWindow(BRect(0, 0, 10, 10), "toast", B_NO_BORDER_WINDOW_LOOK,
			B_FLOATING_SUBSET_WINDOW_FEEL, B_AVOID_FOCUS | B_NOT_MOVABLE
				| B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE
				| B_NOT_CLOSABLE),
		fDuration(duration)
	{
		AddToSubset(owner);
		float size = be_plain_font->Size();
		BFont font(be_bold_font);
		font.SetSize(ceilf(size * 1.1f));
		float width = fmaxf(size * 10, fminf(owner->Frame().Width() * 0.8f,
			font.StringWidth(text) + size * 3));
		float height = ceilf(size * (glyph != B_GLYPH_NONE ? 9 : 4));
		ResizeTo(width, height);
		BRect area = owner->Frame();
		MoveTo(floorf(area.left + (area.Width() - width) / 2),
			floorf(area.top + (area.Height() - height) / 2));
		AddChild(new Face(Bounds(), text, glyph));
	}

	// Shows it; the window deletes itself.
	void Go()
	{
		Show();
		BMessage quit(B_QUIT_REQUESTED);
		BMessageRunner::StartSending(BMessenger(this), &quit, fDuration, 1);
	}

private:
	class Face : public BView {
	public:
		Face(BRect frame, const char* text, Glyph glyph)
			:
			BView(frame, "toast", B_FOLLOW_ALL, B_WILL_DRAW),
			fText(text),
			fGlyph(glyph)
		{
			SetViewColor(B_TRANSPARENT_COLOR);
		}

		virtual void Draw(BRect update)
		{
			BRect bounds = Bounds();
			float size = be_plain_font->Size();
			FillGradient(this, bounds, MakeColor(58, 60, 64),
				MakeColor(24, 25, 27));
			SetHighColor(MakeColor(8, 8, 10));
			StrokeRect(bounds);
			rgb_color white = MakeColor(255, 255, 255);
			BRect text = bounds;
			if (fGlyph != B_GLYPH_NONE) {
				float glyph = ceilf(size * 3.6f);
				BRect box(0, 0, glyph, glyph);
				box.OffsetTo(floorf((bounds.left + bounds.right - glyph) / 2),
					bounds.top + ceilf(size * 1.4f));
				DrawGlyph(this, fGlyph, box, white);
				text.top = box.bottom;
			}
			BFont font(be_bold_font);
			font.SetSize(ceilf(size * 1.1f));
			SetFont(&font);
			DrawCentered(this, fText.String(), text.InsetByCopy(size, 0), white,
				MakeColor(0, 0, 0, 160));
			(void)update;
		}

	private:
		static rgb_color MakeColor(uint8 r, uint8 g, uint8 b, uint8 a = 255)
		{
			return Detail::MakeColor(r, g, b, a);
		}

		BString fText;
		Glyph fGlyph;
	};

	bigtime_t fDuration;
};


inline void
ShowToast(BWindow* owner, const char* text, Glyph glyph = B_GLYPH_CHECK)
{
	if (owner != NULL && text != NULL && text[0] != '\0')
		(new Toast(owner, text, glyph))->Go();
}


} // namespace BMobileKit

#endif // _MOBILE_KIT_H
