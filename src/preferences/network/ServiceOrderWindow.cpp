/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "ServiceOrderWindow.h"

#include <net/if.h>
#include <string.h>

#include <Alert.h>
#include <Button.h>
#include <Catalog.h>
#include <ControlLook.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <NetworkDevice.h>
#include <NetworkInterface.h>
#include <NetworkRoster.h>
#include <ScrollView.h>
#include <StringView.h>
#include <TextView.h>

#include <NetServer.h>

#include "../../shared/net/ServiceOrder.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "ServiceOrderWindow"


using namespace BPrivate::ServiceOrder;


static const uint32 kMsgMoveUp = 'SOup';
static const uint32 kMsgMoveDown = 'SOdn';
static const uint32 kMsgApply = 'SOok';
static const uint32 kMsgSelectionChanged = 'SOsl';
static const uint32 kMsgDragItem = 'SOdr';
static const uint32 kMsgOrderEdited = 'SOed';

static const bigtime_t kNetServerTimeout = 5000000;


static void
GetServices(std::vector<Service>& services)
{
	services.clear();

	BNetworkRoster& roster = BNetworkRoster::Default();
	BNetworkInterface interface;
	uint32 cookie = 0;
	while (roster.GetNextInterface(&cookie, interface) == B_OK) {
		if ((interface.Flags() & IFF_LOOPBACK) != 0)
			continue;

		Service service;
		service.name = interface.Name();
		service.kind = KindOf(interface.Name());
		services.push_back(service);
	}

	BStringList order;
	Read(order);
	Resolve(order, services);
}


// #pragma mark - ServiceItem


class ServiceItem : public BListItem {
public:
	ServiceItem(const Service& service)
		:
		fService(service)
	{
	}

	const char* Name() const
	{
		return fService.name.String();
	}

	virtual void Update(BView* owner, const BFont* font)
	{
		BListItem::Update(owner, font);
		font_height height;
		font->GetHeight(&height);
		fLineHeight = ceilf(height.ascent + height.descent + height.leading);
		fAscent = ceilf(height.ascent);
		SetHeight(fLineHeight * 2 + be_control_look->DefaultLabelSpacing());
	}

	virtual void DrawItem(BView* owner, BRect frame, bool complete)
	{
		rgb_color background = IsSelected()
			? ui_color(B_LIST_SELECTED_BACKGROUND_COLOR)
			: ui_color(B_LIST_BACKGROUND_COLOR);
		rgb_color text = IsSelected()
			? ui_color(B_LIST_SELECTED_ITEM_TEXT_COLOR)
			: ui_color(B_LIST_ITEM_TEXT_COLOR);
		rgb_color muted = mix_color(text, background, 160);

		owner->SetLowColor(background);
		if (IsSelected() || complete) {
			owner->SetHighColor(background);
			owner->FillRect(frame);
		}

		float spacing = be_control_look->DefaultLabelSpacing();
		BListView* list = dynamic_cast<BListView*>(owner);
		int32 index = list != NULL ? list->IndexOf(this) : 0;

		// The position in the order
		BString number;
		number << index + 1;
		owner->SetHighColor(muted);
		owner->SetFont(be_bold_font);
		float numberWidth = owner->StringWidth("00");
		float firstBaseline = frame.top + spacing / 2 + fAscent;
		owner->DrawString(number, BPoint(frame.left + spacing
			+ numberWidth - owner->StringWidth(number), firstBaseline));

		float x = frame.left + spacing * 2 + numberWidth;

		// Connection dot
		bool connected = _IsConnected();
		float dotSize = floorf(fAscent * 0.6f);
		float centerY = firstBaseline - floorf(fAscent * 0.35f);
		BRect dot(x, centerY - dotSize / 2, x + dotSize, centerY + dotSize / 2);
		owner->SetHighColor(connected ? make_color(52, 199, 89)
			: make_color(150, 150, 150));
		owner->FillEllipse(dot);
		x += dotSize + spacing;

		owner->SetHighColor(fService.present ? text : muted);
		owner->DrawString(_Title(), BPoint(x, firstBaseline));

		owner->SetFont(be_plain_font);
		owner->SetHighColor(muted);
		BString subtitle(fService.name);
		subtitle.RemoveFirst("/dev/net/");
		subtitle << "  " B_UTF8_BULLET "  ";
		if (!fService.present)
			subtitle << B_TRANSLATE("Not present");
		else if (connected)
			subtitle << B_TRANSLATE("Connected");
		else
			subtitle << B_TRANSLATE("Not connected");
		owner->DrawString(subtitle, BPoint(x, firstBaseline + fLineHeight));
	}

private:
	BString _Title() const
	{
		switch (fService.kind) {
			case SERVICE_KIND_WIRED:
				return B_TRANSLATE("Ethernet");
			case SERVICE_KIND_WIRELESS:
				return B_TRANSLATE("Wi-Fi");
			default:
			{
				BString name(fService.name);
				name.RemoveFirst("/dev/net/");
				return name;
			}
		}
	}

	bool _IsConnected() const
	{
		if (!fService.present)
			return false;
		BNetworkInterface interface(fService.name.String());
		uint32 flags = interface.Flags();
		return (flags & IFF_UP) != 0 && (flags & IFF_LINK) != 0;
	}

private:
	Service		fService;
	float		fLineHeight;
	float		fAscent;
};


// #pragma mark - ServiceOrderListView


/*!	A list whose items can be dragged into a new order. */
class ServiceOrderListView : public BListView {
public:
	ServiceOrderListView()
		:
		BListView("services", B_SINGLE_SELECTION_LIST),
		fDropIndex(-1)
	{
	}

	virtual bool InitiateDrag(BPoint point, int32 index, bool wasSelected)
	{
		BMessage drag(kMsgDragItem);
		drag.AddInt32("index", index);
		BRect frame = ItemFrame(index);
		DragMessage(&drag, frame, this);
		return true;
	}

	virtual void MouseMoved(BPoint where, uint32 transit,
		const BMessage* dragMessage)
	{
		if (dragMessage != NULL && dragMessage->what == kMsgDragItem) {
			int32 dropIndex = transit == B_EXITED_VIEW
				|| transit == B_OUTSIDE_VIEW ? -1 : _DropIndex(where);
			if (dropIndex != fDropIndex) {
				fDropIndex = dropIndex;
				Invalidate();
			}
		}
		BListView::MouseMoved(where, transit, dragMessage);
	}

	virtual void MouseUp(BPoint where)
	{
		_ClearDropIndex();
		BListView::MouseUp(where);
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what == kMsgDragItem && message->WasDropped()) {
			int32 from = message->GetInt32("index", -1);
			int32 to = _DropIndex(ConvertFromScreen(message->DropPoint()));
			_ClearDropIndex();
			if (from < 0 || from >= CountItems())
				return;
			if (to > from)
				to--;
			if (to != from) {
				MoveItem(from, to);
				Select(to);
				Window()->PostMessage(kMsgOrderEdited);
			}
			return;
		}
		BListView::MessageReceived(message);
	}

	virtual void Draw(BRect updateRect)
	{
		BListView::Draw(updateRect);
		if (fDropIndex < 0)
			return;

		float y;
		if (fDropIndex < CountItems())
			y = ItemFrame(fDropIndex).top;
		else if (CountItems() > 0)
			y = ItemFrame(CountItems() - 1).bottom;
		else
			y = 0;
		SetHighUIColor(B_CONTROL_HIGHLIGHT_COLOR);
		SetPenSize(2);
		StrokeLine(BPoint(Bounds().left + 2, y),
			BPoint(Bounds().right - 2, y));
		SetPenSize(1);
	}

private:
	int32 _DropIndex(BPoint where)
	{
		int32 index = IndexOf(where);
		if (index < 0)
			return where.y < 0 ? 0 : CountItems();
		BRect frame = ItemFrame(index);
		if (where.y > (frame.top + frame.bottom) / 2)
			index++;
		return index;
	}

	void _ClearDropIndex()
	{
		if (fDropIndex >= 0) {
			fDropIndex = -1;
			Invalidate();
		}
	}

private:
	int32		fDropIndex;
};


// #pragma mark - ServiceOrderWindow


ServiceOrderWindow::ServiceOrderWindow(BWindow* parent,
	const BMessenger& target)
	:
	BWindow(BRect(0, 0, 100, 100), B_TRANSLATE("Service order"),
		B_MODAL_WINDOW_LOOK, B_MODAL_APP_WINDOW_FEEL,
		B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE | B_ASYNCHRONOUS_CONTROLS
			| B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE),
	fTarget(target)
{
	BTextView* explanation = new BTextView("explanation");
	explanation->SetText(B_TRANSLATE("Drag the services into the order in "
		"which they should be used. When more than one is connected, the "
		"traffic goes through the one closest to the top, and the next one "
		"takes over at once when it loses its link."));
	explanation->MakeEditable(false);
	explanation->MakeSelectable(false);
	explanation->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	explanation->SetLowUIColor(B_PANEL_BACKGROUND_COLOR);
	rgb_color textColor = ui_color(B_PANEL_TEXT_COLOR);
	explanation->SetFontAndColor(be_plain_font, B_FONT_ALL, &textColor);
	explanation->SetWordWrap(true);
	float width = be_plain_font->StringWidth("M") * 34;
	explanation->SetExplicitMinSize(BSize(width, B_SIZE_UNSET));
	explanation->SetExplicitPreferredSize(BSize(width, B_SIZE_UNSET));

	fListView = new ServiceOrderListView();
	fListView->SetSelectionMessage(new BMessage(kMsgSelectionChanged));
	BScrollView* scrollView = new BScrollView("scroll", fListView, 0, false,
		true);

	font_height height;
	be_plain_font->GetHeight(&height);
	float lineHeight = ceilf(height.ascent + height.descent + height.leading);
	scrollView->SetExplicitMinSize(BSize(width, lineHeight * 9));

	fUpButton = new BButton("up", B_TRANSLATE("Move up"),
		new BMessage(kMsgMoveUp));
	fDownButton = new BButton("down", B_TRANSLATE("Move down"),
		new BMessage(kMsgMoveDown));
	BButton* cancelButton = new BButton("cancel", B_TRANSLATE("Cancel"),
		new BMessage(B_QUIT_REQUESTED));
	fOKButton = new BButton("ok", B_TRANSLATE("OK"), new BMessage(kMsgApply));

	BLayoutBuilder::Group<>(this, B_VERTICAL)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(explanation)
		.Add(scrollView)
		.AddGroup(B_HORIZONTAL)
			.Add(fUpButton)
			.Add(fDownButton)
			.AddGlue()
		.End()
		.AddGroup(B_HORIZONTAL)
			.AddGlue()
			.Add(cancelButton)
			.Add(fOKButton)
		.End();

	SetDefaultButton(fOKButton);
	AddToSubset(parent);

	_Populate();
	_UpdateButtons();

	// The explanation wraps, so let the layout find its height for the
	// chosen width before sizing the window.
	int32 lines = (int32)ceilf(be_plain_font->StringWidth(
		explanation->Text()) / width) + 1;
	explanation->SetExplicitMinSize(BSize(width, lines * lineHeight));
	ResizeToPreferred();
	CenterIn(parent->Frame());
}


ServiceOrderWindow::~ServiceOrderWindow()
{
	while (fListView->CountItems() > 0)
		delete fListView->RemoveItem((int32)0);
}


void
ServiceOrderWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgMoveUp:
		case kMsgMoveDown:
		{
			int32 index = fListView->CurrentSelection();
			int32 to = index + (message->what == kMsgMoveUp ? -1 : 1);
			if (index < 0 || to < 0 || to >= fListView->CountItems())
				break;
			fListView->MoveItem(index, to);
			fListView->Select(to);
			fListView->ScrollToSelection();
			fListView->Invalidate();
			_UpdateButtons();
			break;
		}

		case kMsgSelectionChanged:
		case kMsgOrderEdited:
			fListView->Invalidate();
			_UpdateButtons();
			break;

		case kMsgApply:
		{
			status_t status = _Apply();
			if (status != B_OK) {
				BString text(B_TRANSLATE("The service order could not be "
					"changed:\n%error%"));
				text.ReplaceFirst("%error%", strerror(status));
				BAlert* alert = new BAlert(B_TRANSLATE("Service order"),
					text, B_TRANSLATE("OK"), NULL, NULL, B_WIDTH_AS_USUAL,
					B_STOP_ALERT);
				alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
				alert->Go(NULL);
				break;
			}
			fTarget.SendMessage(kMsgServiceOrderChanged);
			Quit();
			break;
		}

		default:
			BWindow::MessageReceived(message);
	}
}


/*static*/ void
ServiceOrderWindow::GetServiceOrder(BStringList& order)
{
	std::vector<Service> services;
	GetServices(services);

	order.MakeEmpty();
	for (size_t i = 0; i < services.size(); i++)
		order.Add(services[i].name);
}


void
ServiceOrderWindow::_Populate()
{
	std::vector<Service> services;
	GetServices(services);

	for (size_t i = 0; i < services.size(); i++)
		fListView->AddItem(new ServiceItem(services[i]));

	if (fListView->CountItems() > 0)
		fListView->Select(0);
}


void
ServiceOrderWindow::_UpdateButtons()
{
	int32 index = fListView->CurrentSelection();
	fUpButton->SetEnabled(index > 0);
	fDownButton->SetEnabled(index >= 0
		&& index < fListView->CountItems() - 1);
	fOKButton->SetEnabled(fListView->CountItems() > 0);
}


status_t
ServiceOrderWindow::_Apply()
{
	BMessage request(kMsgSetServiceOrder);
	for (int32 i = 0; i < fListView->CountItems(); i++) {
		ServiceItem* item = dynamic_cast<ServiceItem*>(fListView->ItemAt(i));
		if (item != NULL)
			request.AddString("service", item->Name());
	}

	BMessenger netServer(kNetServerSignature);
	if (!netServer.IsValid())
		return B_NAME_NOT_FOUND;

	BMessage reply;
	status_t status = netServer.SendMessage(&request, &reply,
		kNetServerTimeout, kNetServerTimeout);
	if (status != B_OK)
		return status;

	return reply.GetInt32("status", B_ERROR);
}
