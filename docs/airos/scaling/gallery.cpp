/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 *
 * A window of standard controls and drawing primitives for looking at
 * fractional scaling: 1 px lines and frames at every pixel phase, rounded
 * rectangles, ellipses, alpha strokes, text at several sizes and a bitmap.
 *
 *   gallery [x y] [--quit-after seconds]
 */


#include <Application.h>
#include <Bitmap.h>
#include <Box.h>
#include <Button.h>
#include <CheckBox.h>
#include <ColorControl.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <MenuBar.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <OptionPopUp.h>
#include <PopUpMenu.h>
#include <RadioButton.h>
#include <Screen.h>
#include <ScrollView.h>
#include <Slider.h>
#include <StatusBar.h>
#include <String.h>
#include <StringView.h>
#include <TabView.h>
#include <TextControl.h>
#include <TextView.h>
#include <View.h>
#include <Window.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static const uint32 kMsgQuit = 'quit';


class PrimitivesView : public BView {
public:
	PrimitivesView()
		:
		BView("primitives", B_WILL_DRAW),
		fBitmap(BRect(0, 0, 15, 15), B_RGBA32)
	{
		SetExplicitMinSize(BSize(420, 300));
		SetExplicitMaxSize(BSize(420, 300));
		// a checkerboard with a one pixel frame
		uint32* bits = (uint32*)fBitmap.Bits();
		int32 stride = fBitmap.BytesPerRow() / 4;
		for (int y = 0; y < 16; y++) {
			for (int x = 0; x < 16; x++) {
				bool frame = x == 0 || y == 0 || x == 15 || y == 15;
				bool dark = ((x / 2) + (y / 2)) % 2 == 0;
				bits[y * stride + x] = frame ? 0xff000000
					: dark ? 0xff3060c0 : 0xffffffff;
			}
		}
	}

	virtual void Draw(BRect updateRect)
	{
		SetHighColor(0, 0, 0);
		// 1 px vertical and horizontal lines at successive positions
		for (int i = 0; i < 8; i++) {
			StrokeLine(BPoint(10 + i * 3, 10), BPoint(10 + i * 3, 40));
			StrokeLine(BPoint(40, 10 + i * 3), BPoint(70, 10 + i * 3));
		}
		// frames at successive positions, each with a one pixel inner ring
		for (int i = 0; i < 6; i++) {
			BRect r(80 + i * 23, 10 + i % 2, 80 + i * 23 + 19 + i % 2, 40);
			SetHighColor(40, 40, 40);
			StrokeRect(r);
			r.InsetBy(1, 1);
			SetHighColor(255, 255, 255);
			StrokeRect(r);
			r.InsetBy(1, 1);
			SetHighColor(200, 200, 200);
			FillRect(r);
		}
		// alpha strokes, as the control look uses for shadows
		SetDrawingMode(B_OP_ALPHA);
		for (int i = 0; i < 6; i++) {
			SetHighColor(0, 0, 0, 100);
			StrokeRect(BRect(225 + i * 30, 10 + i % 2, 248 + i * 30, 40));
		}
		SetDrawingMode(B_OP_COPY);

		// rounded rectangles, ellipses and a diagonal
		SetHighColor(0, 0, 0);
		for (int i = 0; i < 4; i++) {
			StrokeRoundRect(BRect(10 + i * 50, 55, 50 + i * 50 + i % 2, 85), 4, 4);
			StrokeEllipse(BRect(10 + i * 50, 95, 30 + i * 50 + i % 2, 115));
			FillEllipse(BRect(35 + i * 50, 95, 45 + i * 50, 105));
		}
		StrokeLine(BPoint(210, 55), BPoint(260, 115));
		SetPenSize(2);
		StrokeRect(BRect(270, 55, 300, 85));
		SetPenSize(1);

		// text at several sizes, plain and bold
		BFont font(be_plain_font);
		float y = 140;
		for (float size = 9; size <= 18; size += 3) {
			font.SetSize(size);
			SetFont(&font);
			BString text;
			text.SetToFormat("%g pt: The quick brown fox jumps 0123456789", size);
			DrawString(text.String(), BPoint(10, y));
			y += size + 6;
		}
		font = *be_bold_font;
		SetFont(&font);
		DrawString("Bold: Sphinx of black quartz, judge my vow", BPoint(10, y));
		y += 18;
		font = *be_fixed_font;
		SetFont(&font);
		DrawString("Fixed: if (x[i] != 0) return {1, 2};", BPoint(10, y));

		// a bitmap at 1:1 logical size and doubled
		DrawBitmap(&fBitmap, BPoint(330, 55));
		DrawBitmap(&fBitmap, fBitmap.Bounds(), BRect(355, 55, 386, 86));
		DrawBitmap(&fBitmap, fBitmap.Bounds(), BRect(330, 95, 353, 118),
			B_FILTER_BITMAP_BILINEAR);

		// a checkerboard of single frame buffer pixels, drawn one to one
		float scale = BScreen().DeviceScale();
		int32 size = (int32)ceilf(40 * scale);
		BBitmap device(BRect(0, 0, size - 1, size - 1), B_RGB32);
		uint32* bits = (uint32*)device.Bits();
		int32 stride = device.BytesPerRow() / 4;
		for (int32 y = 0; y < size; y++) {
			for (int32 x = 0; x < size; x++)
				bits[y * stride + x] = (x + y) % 2 == 0 ? 0xff000000 : 0xffffffff;
		}
		DrawBitmap(&device, device.Bounds(), BRect(365, 95, 404, 134),
			B_DEVICE_PIXEL_BITMAP);
	}

private:
	BBitmap	fBitmap;
};


class GalleryWindow : public BWindow {
public:
	GalleryWindow(BPoint where)
		:
		BWindow(BRect(where, where + BPoint(100, 100)), "Scaling gallery",
			B_TITLED_WINDOW, B_AUTO_UPDATE_SIZE_LIMITS
				| B_QUIT_ON_WINDOW_CLOSE)
	{
		BMenuBar* menuBar = new BMenuBar("menu");
		BMenu* menu = new BMenu("File");
		menu->AddItem(new BMenuItem("Open" B_UTF8_ELLIPSIS, NULL, 'O'));
		menu->AddSeparatorItem();
		menu->AddItem(new BMenuItem("Quit", new BMessage(B_QUIT_REQUESTED),
			'Q'));
		menuBar->AddItem(menu);
		menuBar->AddItem(new BMenu("Edit"));

		BPopUpMenu* popUp = new BPopUpMenu("Choice");
		popUp->AddItem(new BMenuItem("First choice", NULL));
		popUp->AddItem(new BMenuItem("Second choice", NULL));
		popUp->ItemAt(0)->SetMarked(true);
		BMenuField* menuField = new BMenuField("Menu field:", popUp);

		BListView* list = new BListView("list");
		for (int i = 0; i < 12; i++) {
			BString text;
			text.SetToFormat("List item %d", i + 1);
			list->AddItem(new BStringItem(text.String()));
		}
		list->Select(2);
		BScrollView* scroll = new BScrollView("scroll", list, 0, false, true);

		BTextView* textView = new BTextView("text");
		textView->SetText("A text view with a few lines of text.\n"
			"Characters: iIl1 O0 rn m wW.\nSecond paragraph here.");
		BScrollView* textScroll = new BScrollView("textscroll", textView, 0,
			true, true);

		BSlider* slider = new BSlider("slider", "Slider", NULL, 0, 100,
			B_HORIZONTAL);
		slider->SetHashMarks(B_HASH_MARKS_BOTTOM);
		slider->SetHashMarkCount(11);
		slider->SetValue(40);

		BStatusBar* status = new BStatusBar("status", "Progress", "40%");
		status->SetTo(40);

		BCheckBox* check = new BCheckBox("Check box");
		check->SetValue(B_CONTROL_ON);
		BRadioButton* radio1 = new BRadioButton("Radio one", NULL);
		BRadioButton* radio2 = new BRadioButton("Radio two", NULL);
		radio1->SetValue(B_CONTROL_ON);
		BButton* button = new BButton("Button", NULL);
		BButton* defaultButton = new BButton("Default", NULL);
		BButton* disabled = new BButton("Disabled", NULL);
		disabled->SetEnabled(false);

		BTabView* tabs = new BTabView("tabs");
		BView* tab1 = new BView("Controls", 0);
		BLayoutBuilder::Group<>(tab1, B_VERTICAL, B_USE_SMALL_SPACING)
			.SetInsets(B_USE_SMALL_SPACING)
			.AddGroup(B_HORIZONTAL)
				.Add(check).Add(radio1).Add(radio2)
			.End()
			.AddGroup(B_HORIZONTAL)
				.Add(button).Add(defaultButton).Add(disabled)
			.End()
			.Add(new BTextControl("Text control:", "Some text", NULL))
			.Add(menuField)
			.Add(slider)
			.Add(status);
		tabs->AddTab(tab1);
		tabs->AddTab(new BView("Second tab", 0));

		BBox* box = new BBox("box");
		box->SetLabel("Primitives");
		BGroupLayout* boxLayout = new BGroupLayout(B_VERTICAL);
		box->SetLayout(boxLayout);
		boxLayout->SetInsets(B_USE_SMALL_SPACING, B_USE_BIG_SPACING,
			B_USE_SMALL_SPACING, B_USE_SMALL_SPACING);
		boxLayout->AddView(new PrimitivesView());

		BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
			.Add(menuBar)
			.AddGroup(B_HORIZONTAL)
				.SetInsets(B_USE_WINDOW_SPACING)
				.AddGroup(B_VERTICAL)
					.Add(tabs)
					.AddGroup(B_HORIZONTAL)
						.Add(scroll)
						.Add(textScroll)
					.End()
				.End()
				.Add(box)
			.End();

		SetDefaultButton(defaultButton);
	}
};


class GalleryApp : public BApplication {
public:
	GalleryApp(BPoint where, bigtime_t quitAfter)
		:
		BApplication("application/x-vnd.airos-scaling-gallery"),
		fWhere(where),
		fQuitAfter(quitAfter)
	{
	}

	virtual void ReadyToRun()
	{
		(new GalleryWindow(fWhere))->Show();
		if (fQuitAfter > 0) {
			BMessage message(B_QUIT_REQUESTED);
			new BMessageRunner(be_app_messenger, &message, fQuitAfter, 1);
		}
	}

private:
	BPoint		fWhere;
	bigtime_t	fQuitAfter;
};


int
main(int argc, char** argv)
{
	BPoint where(40, 40);
	bigtime_t quitAfter = 0;
	int position = 0;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--quit-after") == 0 && i + 1 < argc)
			quitAfter = (bigtime_t)(atof(argv[++i]) * 1000000);
		else if (position == 0) {
			where.x = atof(argv[i]);
			position++;
		} else if (position == 1) {
			where.y = atof(argv[i]);
			position++;
		}
	}
	GalleryApp app(where, quitAfter);
	app.Run();
	return 0;
}
