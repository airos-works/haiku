/* Checks that what app_server draws reaches the screen, by reading the frame
 * buffer itself (through a clone of the accelerant, as a BWindowScreen
 * program would): at a render scale other than 100%, BScreen::GetBitmap()
 * reads app_server's back buffer, which says nothing about the copies from
 * there to the screen (the vertical blank flush, its shadows, the 2D engine).
 *
 * A window shows a grid of cells, each one solid colour that depends on the
 * cell and on a pattern number; whole logical pixels are whole device ones
 * at an integer scale, so the screen must hold exactly those colours. The
 * check redraws the window with new patterns (one at a time, and in bursts
 * without waiting, of which the last must win), moves it, and compares the
 * frame buffer with the expected picture after each step.
 *
 * usage: nvflushcheck <accelerant> [rounds]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <vector>

#include <Accelerant.h>
#include <Application.h>
#include <OS.h>
#include <Screen.h>
#include <View.h>
#include <Window.h>
#include <image.h>


static const int32 kCell = 16;			// logical pixels
static const int32 kColumns = 40, kRows = 30;
static int sFailures = 0;


static rgb_color CellColor(uint32 pattern, int32 column, int32 row)
{
	uint32 v = (pattern * 0x9e3779b1u) ^ (column * 0x85ebca77u)
		^ (row * 0xc2b2ae3du);
	v ^= v >> 15;
	v *= 0x2c1b3c6du;
	v ^= v >> 12;
	rgb_color color = {(uint8)v, (uint8)(v >> 8), (uint8)(v >> 16), 255};
	return color;
}


class GridView : public BView {
public:
	GridView(BRect frame)
		:
		BView(frame, "grid", B_FOLLOW_NONE, B_WILL_DRAW),
		fPattern(1)
	{
		SetViewColor(B_TRANSPARENT_COLOR);
	}

	void SetPattern(uint32 pattern)
	{
		fPattern = pattern;
		Invalidate();
	}

	virtual void Draw(BRect)
	{
		for (int32 row = 0; row < kRows; row++) {
			for (int32 column = 0; column < kColumns; column++) {
				SetHighColor(CellColor(fPattern, column, row));
				FillRect(BRect(column * kCell, row * kCell,
					column * kCell + kCell - 1, row * kCell + kCell - 1));
			}
		}
	}

private:
	uint32 fPattern;
};


struct Screen {
	const uint8* bits;
	uint32 bytesPerRow;
	uint32 width, height;
	int32 scale;
};


// Compares the window's cells on the screen with the pattern; the cursor
// may cover some (counted apart: a few cells at most).
static void Check(const char* what, const Screen& screen, BPoint origin,
	uint32 pattern)
{
	int32 wrongCells = 0, firstColumn = -1, firstRow = -1;
	uint32 want = 0, got = 0;
	int32 cellPixels = kCell * screen.scale;
	for (int32 row = 0; row < kRows; row++) {
		for (int32 column = 0; column < kColumns; column++) {
			rgb_color c = CellColor(pattern, column, row);
			uint32 expected = ((uint32)c.red << 16) | ((uint32)c.green << 8)
				| c.blue;
			int32 left = ((int32)origin.x + column * kCell) * screen.scale;
			int32 top = ((int32)origin.y + row * kCell) * screen.scale;
			bool wrong = false;
			// a few rows of each cell: reading video memory is slow
			for (int32 y = top; y < top + cellPixels && !wrong;
					y += cellPixels / 4) {
				if (y < 0 || y >= (int32)screen.height)
					continue;
				const uint32* line = (const uint32*)(screen.bits
					+ (size_t)y * screen.bytesPerRow);
				for (int32 x = left; x < left + cellPixels; x++) {
					if (x < 0 || x >= (int32)screen.width)
						continue;
					if ((line[x] & 0xffffff) != expected) {
						wrong = true;
						if (firstColumn < 0) {
							firstColumn = column;
							firstRow = row;
							want = expected;
							got = line[x] & 0xffffff;
						}
						break;
					}
				}
			}
			wrongCells += wrong;
		}
	}
	// the cursor covers at most a few cells
	if (wrongCells <= 4) {
		printf("ok   %s%s\n", what, wrongCells > 0 ? " (cursor cells differ)" : "");
		return;
	}
	printf("FAIL %s: %" B_PRId32 " of %" B_PRId32 " cells, first %" B_PRId32
		",%" B_PRId32 " want %06" B_PRIx32 " got %06" B_PRIx32 "\n", what,
		wrongCells, kColumns * kRows, firstColumn, firstRow, want, got);
	sFailures++;
}


int main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: nvflushcheck <accelerant> [rounds]\n");
		return 2;
	}
	int rounds = argc > 2 ? atoi(argv[2]) : 20;

	BApplication app("application/x-vnd.airos-nvflushcheck");

	image_id image = load_add_on(argv[1]);
	if (image < 0) {
		fprintf(stderr, "loading %s: %s\n", argv[1], strerror(image));
		return 2;
	}
	GetAccelerantHook hook;
	if (get_image_symbol(image, B_ACCELERANT_ENTRY_POINT, B_SYMBOL_TYPE_ANY,
			(void**)&hook) != B_OK) {
		fprintf(stderr, "no accelerant entry point\n");
		return 2;
	}
	char cloneInfo[] = "/dev/graphics/nvidia0";
	status_t status = ((clone_accelerant)hook(B_CLONE_ACCELERANT, NULL))(
		cloneInfo);
	if (status != B_OK) {
		fprintf(stderr, "cloning the accelerant: %s\n", strerror(status));
		return 2;
	}
	frame_buffer_config config = {};
	((get_frame_buffer_config)hook(B_GET_FRAME_BUFFER_CONFIG, NULL))(&config);

	BScreen bscreen;
	BRect frame = bscreen.Frame();
	Screen screen;
	screen.bits = (const uint8*)config.frame_buffer;
	screen.bytesPerRow = config.bytes_per_row;
	screen.width = config.bytes_per_row / 4;
	screen.scale = 1;
	display_mode mode = {};
	bscreen.GetMode(&mode);
	screen.width = mode.virtual_width;
	screen.height = mode.virtual_height;
	screen.scale = std::max<int32>(1,
		(int32)(mode.virtual_width / (frame.Width() + 1) + 0.5));
	printf("     frame buffer %" B_PRIu32 "x%" B_PRIu32 ", desktop %gx%g, "
		"scale %" B_PRId32 "\n", screen.width, screen.height,
		frame.Width() + 1, frame.Height() + 1, screen.scale);
	if (screen.bits == NULL) {
		printf("FAIL no frame buffer in the clone\n");
		return 1;
	}

	BRect bounds(0, 0, kColumns * kCell - 1, kRows * kCell - 1);
	BWindow* window = new BWindow(bounds.OffsetToCopy(100, 100), "flushcheck",
		B_NO_BORDER_WINDOW_LOOK, B_FLOATING_ALL_WINDOW_FEEL,
		B_NOT_MOVABLE | B_NOT_RESIZABLE | B_AVOID_FOCUS);
	GridView* view = new GridView(bounds);
	window->AddChild(view);
	window->Show();
	snooze(500000);

	auto origin = [&]() {
		window->Lock();
		BPoint point = window->Frame().LeftTop();
		window->Unlock();
		return point;
	};
	// drawn, flushed at the next blank or two
	auto settle = [&]() {
		window->Lock();
		window->Sync();
		window->Unlock();
		snooze(60000);
	};

	uint32 pattern = 1;
	settle();
	Check("first picture", screen, origin(), pattern);

	for (int i = 0; i < rounds; i++) {
		pattern++;
		window->Lock();
		view->SetPattern(pattern);
		window->Unlock();
		settle();
		char what[64];
		snprintf(what, sizeof(what), "redraw %d", i + 1);
		Check(what, screen, origin(), pattern);
	}

	for (int burst = 0; burst < rounds / 2 + 1; burst++) {
		for (int i = 0; i < 25; i++) {
			pattern++;
			window->Lock();
			view->SetPattern(pattern);
			window->Sync();
			window->Unlock();
		}
		settle();
		char what[64];
		snprintf(what, sizeof(what), "burst %d of 25 redraws, the last shown",
			burst + 1);
		Check(what, screen, origin(), pattern);
	}

	for (int i = 0; i < rounds; i++) {
		window->Lock();
		window->MoveTo(100 + (i % 5) * 37, 100 + (i % 3) * 29);
		window->Unlock();
		settle();
		char what[64];
		snprintf(what, sizeof(what), "move %d", i + 1);
		Check(what, screen, origin(), pattern);
	}

	window->Lock();
	window->Quit();

	printf("\n%d failures\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
