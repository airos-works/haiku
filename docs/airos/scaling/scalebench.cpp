/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Times what a scaled desktop costs app_server: drawing text, redrawing a
 * window of controls, moving that window and scrolling a text view by one
 * logical pixel at a time. Every step waits until app_server has done the
 * work (Sync()) and the window has redrawn what became dirty.
 *
 *   scalebench [iterations]
 */


#include <Application.h>
#include <Button.h>
#include <CheckBox.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <MessageRunner.h>
#include <ScrollView.h>
#include <String.h>
#include <TextControl.h>
#include <TextView.h>
#include <View.h>
#include <Window.h>

#include <OS.h>
#include <stdio.h>
#include <stdlib.h>


class TextView : public BView {
public:
	TextView()
		:
		BView("bench text", B_WILL_DRAW)
	{
		SetExplicitMinSize(BSize(400, 300));
	}

	virtual void Draw(BRect updateRect)
	{
		BFont font(be_plain_font);
		for (int i = 0; i < 25; i++) {
			BString text;
			text.SetToFormat("%d: The quick brown fox jumps over the lazy dog"
				" 0123456789", i);
			DrawString(text.String(), BPoint(4, 12 + i * 12));
		}
	}
};


class BenchWindow : public BWindow {
public:
	BenchWindow()
		:
		BWindow(BRect(60, 60, 500, 500), "scalebench", B_TITLED_WINDOW,
			B_AUTO_UPDATE_SIZE_LIMITS)
	{
		fText = new TextView();
		fTextView = new BTextView("scrolling text");
		BString text;
		for (int i = 0; i < 400; i++)
			text << "Line " << i << ": scrolling text to copy or redraw\n";
		fTextView->SetText(text.String());
		fTextView->MakeEditable(false);
		BScrollView* scroll = new BScrollView("scroll", fTextView, 0, false,
			true);
		scroll->SetExplicitMinSize(BSize(300, 200));

		BListView* list = new BListView("list");
		for (int i = 0; i < 20; i++) {
			BString item;
			item << "Item " << i;
			list->AddItem(new BStringItem(item.String()));
		}

		BLayoutBuilder::Group<>(this, B_VERTICAL)
			.SetInsets(B_USE_WINDOW_SPACING)
			.Add(fText)
			.AddGroup(B_HORIZONTAL)
				.Add(new BButton("One", NULL))
				.Add(new BButton("Two", NULL))
				.Add(new BCheckBox("Check"))
			.End()
			.Add(new BTextControl("Name:", "text", NULL))
			.AddGroup(B_HORIZONTAL)
				.Add(scroll)
				.Add(new BScrollView("lscroll", list, 0, false, true))
			.End();
	}

	virtual void MessageReceived(BMessage* message);

	BView* Text() const { return fText; }
	BTextView* Scrolling() const { return fTextView; }

private:
	BView*		fText;
	BTextView*	fTextView;
};


static double
elapsed_ms(bigtime_t start, int count)
{
	return (system_time() - start) / 1000.0 / count;
}


// Runs in the window's thread, where UpdateIfNeeded() works.
static void
run_bench(BenchWindow* window)
{
	int iterations = atoi(getenv("SCALEBENCH_ITERATIONS") != NULL
		? getenv("SCALEBENCH_ITERATIONS") : "100");

	// drawing text alone
	bigtime_t start = system_time();
	for (int i = 0; i < iterations; i++) {
		window->Text()->Invalidate();
		window->UpdateIfNeeded();
		window->Text()->Sync();
	}
	printf("text view redraw: %.3f ms\n", elapsed_ms(start, iterations));

	// the whole window
	start = system_time();
	for (int i = 0; i < iterations; i++) {
		window->ChildAt(0)->Invalidate();
		for (int32 c = 0; BView* child = window->ChildAt(0)->ChildAt(c); c++)
			child->Invalidate();
		window->UpdateIfNeeded();
		window->ChildAt(0)->Sync();
	}
	printf("window redraw: %.3f ms\n", elapsed_ms(start, iterations));

	// moving the window one logical pixel at a time
	start = system_time();
	for (int i = 0; i < iterations; i++) {
		window->MoveBy(i % 2 == 0 ? 1 : -1, 0);
		window->ChildAt(0)->Sync();
		window->UpdateIfNeeded();
		window->ChildAt(0)->Sync();
	}
	printf("move by 1: %.3f ms\n", elapsed_ms(start, iterations));

	start = system_time();
	for (int i = 0; i < iterations; i++) {
		window->MoveBy(i % 2 == 0 ? 2 : -2, 0);
		window->ChildAt(0)->Sync();
		window->UpdateIfNeeded();
		window->ChildAt(0)->Sync();
	}
	printf("move by 2: %.3f ms\n", elapsed_ms(start, iterations));

	// scrolling a text view one logical pixel at a time
	BTextView* text = window->Scrolling();
	start = system_time();
	for (int i = 0; i < iterations; i++) {
		text->ScrollBy(0, 1);
		text->Sync();
		window->UpdateIfNeeded();
		text->Sync();
	}
	printf("scroll by 1: %.3f ms\n", elapsed_ms(start, iterations));
	start = system_time();
	for (int i = 0; i < iterations; i++) {
		text->ScrollBy(0, 2);
		text->Sync();
		window->UpdateIfNeeded();
		text->Sync();
	}
	printf("scroll by 2: %.3f ms\n", elapsed_ms(start, iterations));

	be_app->PostMessage(B_QUIT_REQUESTED);
}


void
BenchWindow::MessageReceived(BMessage* message)
{
	if (message->what == 'bnch') {
		run_bench(this);
		return;
	}
	BWindow::MessageReceived(message);
}


class BenchApp : public BApplication {
public:
	BenchApp()
		:
		BApplication("application/x-vnd.airos-scalebench")
	{
	}

	virtual void ReadyToRun()
	{
		BenchWindow* window = new BenchWindow();
		window->Show();
		BMessage message('bnch');
		new BMessageRunner(BMessenger(window), &message, 1000000, 1);
	}
};


int
main(int argc, char** argv)
{
	if (argc > 1)
		setenv("SCALEBENCH_ITERATIONS", argv[1], 1);
	BenchApp app;
	app.Run();
	return 0;
}
