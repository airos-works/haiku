/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	A window for the agent UI tools of mcp_server (ui_tree, ui_act...):
	a menu bar with a closed submenu, a pop-up menu field, text and password
	fields, a check box, a slider, a list whose items draw their own text,
	a custom view that only draws text, tabs, and a button that asks for
	confirmation. Every message the window gets is printed as
	"got <what> <name> agent=<0|1> ..." so a test can check that an action
	arrived and that it was marked as an agent's. agent_ui_test.py drives it.
*/


#include <Application.h>
#include <Button.h>
#include <CheckBox.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <ScrollView.h>
#include <Slider.h>
#include <TabView.h>
#include <TextControl.h>
#include <Window.h>

#include <Accessible.h>

#include <stdio.h>


static const uint32 kMsgNew = 'new_';
static const uint32 kMsgExportPDF = 'xpdf';
static const uint32 kMsgExportPNG = 'xpng';
static const uint32 kMsgSize = 'size';
static const uint32 kMsgName = 'name';
static const uint32 kMsgPassword = 'pass';
static const uint32 kMsgCheck = 'chck';
static const uint32 kMsgSlider = 'sldr';
static const uint32 kMsgDelete = 'dele';
static const uint32 kMsgList = 'list';
static const uint32 kMsgCanvasClick = 'cnvc';


//!	A list item that draws its text itself (no BStringItem).
class ColorItem : public BListItem {
public:
	ColorItem(const char* name) : fName(name) {}

	virtual void DrawItem(BView* owner, BRect frame, bool complete)
	{
		owner->SetHighColor(IsSelected() ? 200 : 255, 255, 255);
		owner->FillRect(frame);
		owner->SetHighColor(0, 0, 0);
		font_height height;
		owner->GetFontHeight(&height);
		owner->DrawString(fName.String(),
			BPoint(frame.left + 4, frame.top + height.ascent + 1));
	}

	virtual void Update(BView* owner, const BFont* font)
	{
		BListItem::Update(owner, font);
		SetHeight(Height() + 2);
	}

private:
	BString fName;
};


//!	A view the Interface Kit knows nothing about: it only draws text.
class StatusCanvas : public BView {
public:
	StatusCanvas()
		:
		BView("status canvas", B_WILL_DRAW),
		fClicks(0)
	{
		SetExplicitMinSize(BSize(240, 50));
	}

	virtual void Draw(BRect)
	{
		DrawString("Temperature 21 C", BPoint(6, 16));
		DrawString("Humidity", BPoint(6, 36));
		DrawString("40 %", BPoint(90, 36));
		BString clicks;
		clicks.SetToFormat("Clicks %d", fClicks);
		DrawString(clicks.String(), BPoint(160, 16));
	}

	virtual void MouseDown(BPoint where)
	{
		fClicks++;
		BMessage message(kMsgCanvasClick);
		message.AddPoint("where", where);
		BMessage* current = Window()->CurrentMessage();
		if (current != NULL && current->GetBool("be:agent", false))
			message.AddBool("be:agent", true);
		Window()->PostMessage(&message);
		Invalidate();
	}

	virtual void KeyDown(const char* bytes, int32 numBytes)
	{
		printf("got key '%s' agent=%d\n", bytes,
			Window()->CurrentMessage()->GetBool("be:agent", false));
		fflush(stdout);
	}

private:
	int32 fClicks;
};


class AgentWindow : public BWindow {
public:
	AgentWindow()
		:
		BWindow(BRect(100, 100, 560, 520), "Agent test", B_TITLED_WINDOW,
			B_AUTO_UPDATE_SIZE_LIMITS | B_QUIT_ON_WINDOW_CLOSE)
	{
		BMenuBar* menuBar = new BMenuBar("menu bar");
		BMenu* file = new BMenu("File");
		file->AddItem(new BMenuItem("New", new BMessage(kMsgNew), 'N'));
		BMenu* exportMenu = new BMenu("Export");
		exportMenu->AddItem(new BMenuItem("PDF…", new BMessage(kMsgExportPDF)));
		exportMenu->AddItem(new BMenuItem("PNG", new BMessage(kMsgExportPNG)));
		file->AddItem(exportMenu);
		menuBar->AddItem(file);

		BPopUpMenu* sizes = new BPopUpMenu("sizes");
		const char* sizeNames[] = { "Small", "Medium", "Large" };
		for (int32 i = 0; i < 3; i++) {
			BMessage* message = new BMessage(kMsgSize);
			message->AddString("size", sizeNames[i]);
			BMenuItem* item = new BMenuItem(sizeNames[i], message);
			sizes->AddItem(item);
			if (i == 1)
				item->SetMarked(true);
		}
		BMenuField* size = new BMenuField("size", "Size:", sizes);

		BTextControl* name = new BTextControl("name", "Name:", "Ada",
			new BMessage(kMsgName));
		BTextControl* password = new BTextControl("password", "Password:", "",
			new BMessage(kMsgPassword));
		password->TextView()->HideTyping(true);
		BCheckBox* check = new BCheckBox("notify", "Notify me",
			new BMessage(kMsgCheck));
		BSlider* slider = new BSlider("volume", "Volume",
			new BMessage(kMsgSlider), 0, 100, B_HORIZONTAL);
		slider->SetValue(30);

		BListView* list = new BListView("colors");
		list->AddItem(new ColorItem("Red"));
		list->AddItem(new ColorItem("Green"));
		list->AddItem(new ColorItem("Blue"));
		list->SetSelectionMessage(new BMessage(kMsgList));
		BAccessible::SetName(list, "Colors");
		BScrollView* scroll = new BScrollView("colors scroll", list, 0, false,
			true);
		scroll->SetExplicitMinSize(BSize(150, 70));

		BTabView* tabs = new BTabView("tabs");
		BView* general = new BView("General", 0);
		BLayoutBuilder::Group<>(general, B_VERTICAL)
			.SetInsets(B_USE_SMALL_INSETS)
			.Add(new StatusCanvas);
		tabs->AddTab(general);
		BView* advanced = new BView("Advanced", 0);
		BLayoutBuilder::Group<>(advanced, B_VERTICAL)
			.SetInsets(B_USE_SMALL_INSETS)
			.Add(new BCheckBox("expert", "Expert mode", NULL));
		tabs->AddTab(advanced);

		BButton* remove = new BButton("delete", "Delete everything",
			new BMessage(kMsgDelete));
		BAccessible::SetConfirmation(remove,
			"Deletes every document in the test folder");

		BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
			.Add(menuBar)
			.AddGroup(B_VERTICAL)
				.SetInsets(B_USE_WINDOW_INSETS)
				.Add(size)
				.Add(name)
				.Add(password)
				.Add(check)
				.Add(slider)
				.Add(scroll)
				.Add(tabs)
				.Add(remove)
			.End();
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case kMsgNew:
			case kMsgExportPDF:
			case kMsgExportPNG:
			case kMsgSize:
			case kMsgName:
			case kMsgPassword:
			case kMsgCheck:
			case kMsgSlider:
			case kMsgDelete:
			case kMsgList:
			case kMsgCanvasClick:
			{
				char what[5];
				for (int i = 0; i < 4; i++)
					what[i] = (char)(message->what >> (24 - 8 * i));
				what[4] = '\0';
				BString detail;
				if (message->what == kMsgSize)
					detail << " size=" << message->GetString("size", "");
				if (message->what == kMsgName) {
					BTextControl* name = dynamic_cast<BTextControl*>(
						FindView("name"));
					detail << " text=" << name->Text();
				}
				if (message->what == kMsgPassword) {
					BTextControl* password = dynamic_cast<BTextControl*>(
						FindView("password"));
					detail << " length=" << strlen(password->Text());
				}
				if (message->what == kMsgCheck || message->what == kMsgSlider)
					detail << " value=" << message->GetInt32("be:value", -1);
				if (message->what == kMsgList)
					detail << " index=" << message->GetInt32("index", -1);
				printf("got %s%s agent=%d\n", what, detail.String(),
					message->GetBool("be:agent", false));
				fflush(stdout);
				break;
			}
			default:
				BWindow::MessageReceived(message);
		}
	}
};


int
main()
{
	BApplication app("application/x-vnd.Haiku-AgentUITestApp");
	(new AgentWindow)->Show();
	app.Run();
	return 0;
}
