/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Saves the screen in frame buffer pixels. BScreen::GetBitmap() averages a
 * scaled desktop down to its logical size; this reads app_server's drawing
 * buffer through a one pixel direct window that asks for device pixels.
 *
 *   devshot <file.png|file.ppm> [x y width height]   (device pixels)
 */


#include <Application.h>
#include <Autolock.h>
#include <Bitmap.h>
#include <BitmapStream.h>
#include <DirectWindow.h>
#include <File.h>
#include <Locker.h>
#include <Screen.h>
#include <TranslatorRoster.h>
#include <TranslatorFormats.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


class ShotWindow : public BDirectWindow {
public:
	ShotWindow(BRect frame)
		:
		BDirectWindow(frame, "devshot", B_NO_BORDER_WINDOW_LOOK,
			B_FLOATING_ALL_WINDOW_FEEL, B_AVOID_FOCUS | B_DIRECT_DEVICE_PIXELS
				| B_NOT_MOVABLE | B_NOT_RESIZABLE),
		fConnected(false),
		fArea(-1),
		fBits(NULL)
	{
		fReady = create_sem(0, "devshot ready");
	}

	~ShotWindow()
	{
		if (fArea >= 0)
			delete_area(fArea);
		delete_sem(fReady);
	}

	virtual void DirectConnected(direct_buffer_info* info)
	{
		BAutolock lock(fLock);
		if ((info->buffer_state & B_DIRECT_MODE_MASK) == B_DIRECT_STOP) {
			fConnected = false;
			return;
		}
		if (fArea >= 0)
			delete_area(fArea);
		area_id source = info->drawing_bits_area;
		fStride = info->drawing_bytes_per_row;
		if (source < 0) {
			source = info->bits_area;
			fStride = info->bytes_per_row;
		}
		fScale = info->device_scale;
		fArea = clone_area("devshot", (void**)&fBits, B_ANY_ADDRESS,
			B_READ_AREA, source);
		fConnected = fArea >= 0;
		fFormat = info->pixel_format;
		release_sem(fReady);
	}

	bool WaitConnected()
	{
		return acquire_sem_etc(fReady, 1, B_RELATIVE_TIMEOUT, 5000000) == B_OK
			&& fConnected;
	}

	BBitmap* Grab(BRect device)
	{
		BAutolock lock(fLock);
		BBitmap* bitmap = new BBitmap(BRect(0, 0, device.Width(),
			device.Height()), B_RGB32);
		int32 width = device.IntegerWidth() + 1;
		int32 height = device.IntegerHeight() + 1;
		for (int32 y = 0; y < height; y++) {
			memcpy((uint8*)bitmap->Bits() + y * bitmap->BytesPerRow(),
				fBits + (int32(device.top) + y) * fStride
					+ int32(device.left) * 4, width * 4);
		}
		return bitmap;
	}

	uint32 Scale() const { return fScale; }

private:
	BLocker		fLock;
	sem_id		fReady;
	bool		fConnected;
	area_id		fArea;
	uint8*		fBits;
	int32		fStride;
	uint32		fScale;
	color_space	fFormat;
};


int
main(int argc, char** argv)
{
	if (argc != 2 && argc != 6) {
		fprintf(stderr, "usage: %s <file.png|file.ppm> [x y width height]\n",
			argv[0]);
		return 1;
	}
	BApplication app("application/x-vnd.airos-devshot");

	BScreen screen;
	BRect logical = screen.Frame();
	// one logical pixel in the bottom right corner
	ShotWindow* window = new ShotWindow(BRect(logical.right, logical.bottom,
		logical.right, logical.bottom));
	window->Show();
	if (!window->WaitConnected()) {
		fprintf(stderr, "no direct connection\n");
		return 1;
	}
	// let pending drawing reach the buffer
	snooze(200000);

	float scale = window->Scale() > 0 ? window->Scale() / 100.0f : 1.0f;
	BRect device(0, 0, (int32)((logical.right + 1) * scale) - 1,
		(int32)((logical.bottom + 1) * scale) - 1);
	if (argc == 6) {
		BRect asked(atoi(argv[2]), atoi(argv[3]),
			atoi(argv[2]) + atoi(argv[4]) - 1, atoi(argv[3]) + atoi(argv[5]) - 1);
		device = device & asked;
	}

	BBitmap* bitmap = window->Grab(device);
	window->Lock();
	window->Quit();

	const char* path = argv[1];
	size_t length = strlen(path);
	if (length > 4 && strcmp(path + length - 4, ".ppm") == 0) {
		FILE* file = fopen(path, "wb");
		if (file == NULL)
			return 1;
		int32 width = bitmap->Bounds().IntegerWidth() + 1;
		int32 height = bitmap->Bounds().IntegerHeight() + 1;
		fprintf(file, "P6\n%d %d\n255\n", (int)width, (int)height);
		for (int32 y = 0; y < height; y++) {
			const uint8* row = (const uint8*)bitmap->Bits()
				+ y * bitmap->BytesPerRow();
			for (int32 x = 0; x < width; x++) {
				uint8 rgb[3] = { row[x * 4 + 2], row[x * 4 + 1], row[x * 4] };
				fwrite(rgb, 1, 3, file);
			}
		}
		fclose(file);
	} else {
		BBitmapStream stream(bitmap);
		BFile file(path, B_CREATE_FILE | B_ERASE_FILE | B_WRITE_ONLY);
		status_t status = BTranslatorRoster::Default()->Translate(&stream,
			NULL, NULL, &file, B_PNG_FORMAT);
		if (status != B_OK) {
			fprintf(stderr, "could not write %s: %s\n", path, strerror(status));
			return 1;
		}
		stream.DetachBitmap(&bitmap);
	}
	delete bitmap;
	printf("%s: %gx%g device pixels at %g%%\n", path, device.Width() + 1,
		device.Height() + 1, scale * 100);
	return 0;
}
