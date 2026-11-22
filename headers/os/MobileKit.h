/*
 * Copyright 2026, air/OS. Distributed under the MIT License.
 */
#ifndef _MOBILE_KIT_H
#define _MOBILE_KIT_H

#include <Button.h>
#include <CardLayout.h>
#include <CardView.h>
#include <CheckBox.h>
#include <Font.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <Message.h>
#include <Resources.h>
#include <Messenger.h>
#include <Window.h>
#include <ScrollView.h>
#include <Slider.h>
#include <StringItem.h>
#include <TextControl.h>

#include <math.h>


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

} // namespace BMobileKit

#endif // _MOBILE_KIT_H
