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

#include <math.h>
#include <string.h>

#include <algorithm>
#include <vector>


// 1: presentation switching and native touch controls.
// 2: the skeuomorphic materials and controls (MetalBar, SegmentedControl, ...).
#define B_MOBILE_KIT_VERSION 2


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

// A transient, scrollable list of commands. Each row carries its original
// BMessage (including IDs or refs). Escape and Cancel dismiss the sheet.
class ActionSheet : public BWindow {
public:
	ActionSheet(BWindow* owner, const char* title)
		:
		BWindow(owner->Frame(), title, B_MODAL_WINDOW_LOOK,
			owner->Feel() == B_MODAL_APP_WINDOW_FEEL ? B_MODAL_APP_WINDOW_FEEL
				: B_MODAL_SUBSET_WINDOW_FEEL, B_NOT_ZOOMABLE | B_NOT_RESIZABLE
				| B_ASYNCHRONOUS_CONTROLS | B_CLOSE_ON_ESCAPE),
		fOwner(owner),
		fArea(owner->Frame())
	{
		if (Feel() == B_MODAL_SUBSET_WINDOW_FEEL)
			AddToSubset(owner);
		fChoices = new Choices;
		fChoices->SetInvocationMessage(new BMessage('mVok'));
		BLayoutBuilder::Group<>(this, B_VERTICAL)
			.SetInsets(B_USE_HALF_ITEM_SPACING)
			.Add(Scroll("choices-scroll", fChoices))
			.Add(Action("cancel", "Cancel", new BMessage(B_QUIT_REQUESTED)));
		fChoices->SetTarget(this);
	}

	virtual ~ActionSheet()
	{
		while (BListItem* item = fChoices->RemoveItem((int32)0))
			delete item;
	}

	void AddAction(const char* label, const BMessage& action)
	{
		Choice* item = new Choice(label, action);
		if (!fChoices->AddItem(item))
			delete item;
	}

	void Open()
	{
		float width = fminf(fArea.Width() - 16, TargetSize() * 12);
		float height = fminf(fArea.Height() * 0.8f,
			TargetSize() * (2 + fChoices->CountItems()));
		ResizeTo(width, height);
		MoveTo(fArea.left + floorf((fArea.Width() - width) / 2),
			fArea.bottom - height - 8);
		Show();
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what != 'mVok') {
			BWindow::MessageReceived(message);
			return;
		}
		Choice* item = (Choice*)fChoices->ItemAt(message->GetInt32("index", -1));
		if (item != NULL) {
			fOwner.SendMessage(&item->action);
			PostMessage(B_QUIT_REQUESTED);
		}
	}

private:
	struct Choice : public ListItem {
		Choice(const char* label, const BMessage& value)
			: ListItem(label), action(value) {}
		BMessage action;
	};

	class Choices : public BListView {
	public:
		Choices() : BListView("choices"), fDown(-1) {}
		virtual void MouseDown(BPoint point)
		{
			BMessage* event = Window()->CurrentMessage();
			fDown = event->GetInt32("buttons", 0) == B_PRIMARY_MOUSE_BUTTON
				&& event->GetInt32("clicks", 1) == 1 ? IndexOf(point) : -1;
			BListView::MouseDown(point);
		}
		virtual void MouseUp(BPoint point)
		{
			BListView::MouseUp(point);
			if (fDown >= 0 && IndexOf(point) == fDown && CurrentSelection() == fDown)
				Invoke();
			fDown = -1;
		}
	private:
		int32 fDown;
	};

	BMessenger fOwner;
	BRect fArea;
	Choices* fChoices;
};


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
inline const Skin&
CurrentSkin()
{
	static const Skin aluminium = Detail::AluminiumSkin();
	static const Skin graphite = Detail::GraphiteSkin();
	return IsDarkPanel() ? graphite : aluminium;
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
	B_GLYPH_NOTE
};


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
		fPosition(position)
	{
		SetFlags(Flags() | B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE);
		SetViewColor(B_TRANSPARENT_COLOR);
		float inset = ceilf(be_plain_font->Size() * 0.5f);
		GroupLayout()->SetInsets(inset, 1, inset, 1);
	}

	BarPosition Position() const { return fPosition; }

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
	}

private:
	BarPosition fPosition;
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
	B_BACK_BUTTON		// pointed on the left, for drill-down navigation
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
		bool down = fPressed && fInside;
		rgb_color top = down ? skin.pressedTop : skin.controlTop;
		rgb_color bottom = down ? skin.pressedBottom : skin.controlBottom;
		BRect content = plate;
		if (fShape == B_BACK_BUTTON) {
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
		float glyph = floorf(plate.Height() * 0.5f);
		bool hasLabel = Label() != NULL && Label()[0] != '\0';
		if (fGlyph != B_GLYPH_NONE) {
			float labelWidth = hasLabel ? font.StringWidth(Label())
				+ glyph * 0.4f : 0;
			float left = floorf((content.left + content.right
				- glyph - labelWidth) / 2);
			BRect box(left, floorf((plate.top + plate.bottom - glyph) / 2),
				left + glyph, floorf((plate.top + plate.bottom + glyph) / 2));
			DrawGlyph(this, fGlyph, box.OffsetByCopy(0, 1), skin.controlEmboss);
			DrawGlyph(this, fGlyph, box, text);
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
		// the groove, the part played and the playhead
		BRect groove(fTrack.left, floorf((fTrack.top + fTrack.bottom) / 2) - 2,
			fTrack.right, floorf((fTrack.top + fTrack.bottom) / 2) + 2);
		FillGradient(this, groove, skin.trackTop, skin.trackBottom, 2);
		float fraction = fmaxf(0.0f, fminf(1.0f, (float)shown / fDuration));
		float x = roundf(groove.left + fraction * groove.Width());
		if (x > groove.left + 1) {
			BRect played(groove.left, groove.top, x, groove.bottom);
			SetHighColor(skin.trackFill);
			FillRoundRect(played, 2, 2);
		}
		BRect head(x - 1, fTrack.top + 1, x + 1, fTrack.bottom - 1);
		SetHighColor(MixColors(skin.playhead, skin.displayBorder, 0.4f));
		FillRect(head.InsetByCopy(-1, 0));
		SetHighColor(skin.playhead);
		FillRect(head);
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
			float fraction = _Fraction(where);
			fScrub = -1;
			fPosition = (bigtime_t)(fraction * fDuration);
			Invalidate();
			BMessage extra;
			extra.AddFloat("fraction", fraction);
			extra.AddInt64("position", fPosition);
			_Send(B_DISPLAY_SEEK, &extra);
			return;
		}
		BRect area = part == B_DISPLAY_ARTWORK ? fArt
			: part == B_DISPLAY_REPEAT ? fRepeatRect : fShuffleRect;
		if (part >= 0 && area.Contains(where))
			_Send((Part)part, NULL);
	}

private:
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
		fTracking(false)
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
		int32 value = (int32)roundf(fmaxf(0.0f, fminf(1.0f, fraction)) * 1000);
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

} // namespace BMobileKit

#endif // _MOBILE_KIT_H
