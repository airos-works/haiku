/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */


#include "JoinOtherWindow.h"

#include <Button.h>
#include <Catalog.h>
#include <CheckBox.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <NetworkDevice.h>
#include <PopUpMenu.h>
#include <TextControl.h>

#include <string.h>

#include "WiFiController.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "JoinOtherWindow"


static const uint32 kMsgJoin = 'jojn';
static const uint32 kMsgChanged = 'jocg';
static const uint32 kMsgShowPassword = 'josp';


JoinOtherWindow::JoinOtherWindow(BWindow* parent,
	const BMessenger& controller)
	:
	BWindow(BRect(0, 0, 360, 200), B_TRANSLATE("Join other network"),
		B_FLOATING_WINDOW_LOOK, B_MODAL_SUBSET_WINDOW_FEEL,
		B_ASYNCHRONOUS_CONTROLS | B_NOT_ZOOMABLE | B_NOT_RESIZABLE
			| B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE),
	fController(controller)
{
	fNameControl = new BTextControl("name", B_TRANSLATE("Network name:"),
		"", NULL);
	fNameControl->SetModificationMessage(new BMessage(kMsgChanged));
	fNameControl->TextView()->SetMaxBytes(32);

	BPopUpMenu* securityMenu = new BPopUpMenu("security");
	struct {
		const char*	label;
		uint32		mode;
	} modes[] = {
		{ B_TRANSLATE("None"), B_NETWORK_AUTHENTICATION_NONE },
		{ B_TRANSLATE("WEP"), B_NETWORK_AUTHENTICATION_WEP },
		{ B_TRANSLATE("WPA Personal"), B_NETWORK_AUTHENTICATION_WPA },
		{ B_TRANSLATE("WPA2 Personal"), B_NETWORK_AUTHENTICATION_WPA2 },
		{ B_TRANSLATE("WPA3 Personal"), B_NETWORK_AUTHENTICATION_WPA3 }
	};
	for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		BMessage* message = new BMessage(kMsgChanged);
		message->AddUInt32("authentication", modes[i].mode);
		BMenuItem* item = new BMenuItem(modes[i].label, message);
		securityMenu->AddItem(item);
		if (modes[i].mode == B_NETWORK_AUTHENTICATION_WPA2)
			item->SetMarked(true);
	}
	fSecurityField = new BMenuField("security", B_TRANSLATE("Security:"),
		securityMenu);

	fPasswordControl = new BTextControl("password", B_TRANSLATE("Password:"),
		"", NULL);
	fPasswordControl->TextView()->HideTyping(true);
	fPasswordControl->SetModificationMessage(new BMessage(kMsgChanged));

	fShowPassword = new BCheckBox("show", B_TRANSLATE("Show password"),
		new BMessage(kMsgShowPassword));
	fRememberCheck = new BCheckBox("remember",
		B_TRANSLATE("Remember this network"), NULL);
	fRememberCheck->SetValue(B_CONTROL_ON);

	fJoinButton = new BButton("join", B_TRANSLATE("Join"),
		new BMessage(kMsgJoin));
	BButton* cancel = new BButton("cancel", B_TRANSLATE("Cancel"),
		new BMessage(B_QUIT_REQUESTED));

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
		.SetInsets(B_USE_WINDOW_SPACING)
		.AddGrid(B_USE_SMALL_SPACING, B_USE_SMALL_SPACING)
			.AddTextControl(fNameControl, 0, 0)
			.AddMenuField(fSecurityField, 0, 1)
			.AddTextControl(fPasswordControl, 0, 2)
			.Add(fShowPassword, 1, 3)
			.Add(fRememberCheck, 1, 4)
		.End()
		.AddGroup(B_HORIZONTAL)
			.AddGlue()
			.Add(cancel)
			.Add(fJoinButton)
		.End();

	securityMenu->SetTargetForItems(this);
	SetDefaultButton(fJoinButton);
	AddToSubset(parent);
	_UpdateControls();

	fNameControl->MakeFocus(true);
	BRect frame = parent->Frame();
	MoveTo(frame.left + (frame.Width() - Frame().Width()) / 2,
		frame.top + frame.Height() / 4);
}


void
JoinOtherWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgChanged:
			_UpdateControls();
			break;

		case kMsgShowPassword:
		{
			BString text(fPasswordControl->Text());
			fPasswordControl->TextView()->HideTyping(
				fShowPassword->Value() != B_CONTROL_ON);
			fPasswordControl->SetText(text);
			text.SetTo('\0', text.Length());
			break;
		}

		case kMsgJoin:
		{
			if (!fJoinButton->IsEnabled())
				break;

			uint32 authentication = _Authentication();
			BMessage join(kMsgWiFiJoin);
			join.AddString("name", fNameControl->Text());
			join.AddUInt32("authentication", authentication);
			if (authentication != B_NETWORK_AUTHENTICATION_NONE)
				join.AddString("password", fPasswordControl->Text());
			join.AddBool("remember", fRememberCheck->Value() == B_CONTROL_ON);
			join.AddBool("hidden", true);
			fController.SendMessage(&join);
			join.MakeEmpty();
			Quit();
			break;
		}

		default:
			BWindow::MessageReceived(message);
			break;
	}
}


bool
JoinOtherWindow::QuitRequested()
{
	fPasswordControl->SetText("");
	return true;
}


uint32
JoinOtherWindow::_Authentication() const
{
	BMenuItem* item = fSecurityField->Menu()->FindMarked();
	if (item == NULL || item->Message() == NULL)
		return B_NETWORK_AUTHENTICATION_WPA2;
	return item->Message()->GetUInt32("authentication",
		B_NETWORK_AUTHENTICATION_WPA2);
}


void
JoinOtherWindow::_UpdateControls()
{
	bool secured = _Authentication() != B_NETWORK_AUTHENTICATION_NONE;
	fPasswordControl->SetEnabled(secured);
	fShowPassword->SetEnabled(secured);

	size_t length = strlen(fPasswordControl->Text());
	bool passwordValid = !secured
		|| (_Authentication() == B_NETWORK_AUTHENTICATION_WEP
			? length > 0 : length >= 8 && length <= 64);
	fJoinButton->SetEnabled(fNameControl->Text()[0] != '\0'
		&& passwordValid);
}
