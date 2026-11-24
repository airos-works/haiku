/*
 * Copyright 2026, air/OS.
 * Distributed under the terms of the MIT License.
 */


/*!	What app_server does for a character the string's font lacks (#199): it
	takes the glyph from a fallback font. This measures what that costs the
	first time at a size (each size has entries of its own in the font cache),
	what GetHasGlyphs() with fallbacks costs, and whether the ordinary fonts
	stay in the cache. It also prints a fingerprint of the results (widths,
	escapements, which characters have glyphs, and the pixels of the strings
	drawn into a bitmap) that has to be the same with the server before and
	after a change of the fallback code.

	fallback_fonts_test                 measures, then the fingerprint
	fallback_fonts_test measure         only measures
	fallback_fonts_test fingerprint     only the fingerprint
*/


#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <Application.h>
#include <Bitmap.h>
#include <Font.h>
#include <String.h>
#include <OS.h>
#include <View.h>

#include <AppServerLink.h>
#include <ServerProtocol.h>


static bigtime_t sDeadline = 0;


static status_t
watchdog(void*)
{
	while (true) {
		snooze(500000);
		if (sDeadline != 0 && system_time() > sDeadline) {
			printf("FAIL: still running after the deadline\n");
			_exit(1);
		}
	}
	return B_OK;
}


static void
crash_handler(int signal)
{
	printf("FAIL: signal %d\n", signal);
	_exit(1);
}


//!	The server's width, asked directly (no width the kit remembers).
static float
server_width(const BFont& font, const char* string)
{
	BPrivate::AppServerLink link;
	link.StartMessage(AS_GET_STRING_WIDTHS);
	link.Attach<uint16>(font.FamilyAndStyle() >> 16);
	link.Attach<uint16>(font.FamilyAndStyle() & 0xffff);
	link.Attach<float>(font.Size());
	link.Attach<uint8>(font.Spacing());
	link.Attach<int32>(1);
	link.AttachString(string);
	int32 status = B_ERROR;
	float width = -1;
	if (link.FlushWithReply(status) == B_OK && status == B_OK)
		link.Read<float>(&width);
	return width;
}


/*!	The time of the first width of \a string at \a count sizes no string was
	measured at before, from \a firstSize on in steps of 0.01 (the font cache
	tells sizes apart by two decimals).
*/
static double
first_width_time(const char* string, float firstSize, int32 count)
{
	BFont font(be_plain_font);
	bigtime_t total = 0;
	for (int32 i = 0; i < count; i++) {
		font.SetSize(firstSize + i * 0.01f);
		bigtime_t start = system_time();
		server_width(font, string);
		total += system_time() - start;
	}
	return (double)total / count / 1000.0;
}


static void
measure()
{
	const int32 kSizes = 10;
	const char* kAscii = "Plain text";
	const char* kMissing = "No font has \xf4\x8f\xbf\xbd";
		// U+10FFFD, a private use character of plane 16
	const char* kCJK = "CJK \xe6\x97\xa5\xe6\x9c\xac";
	const char* kEmoji = "Emoji \xf0\x9f\x98\x80";
	const char* kSymbol = "Symbol \xe2\x8c\x98";

	printf("first width at a new size, ms per size (%d sizes each):\n",
		(int)kSizes);
	double ascii = first_width_time(kAscii, 11.01f, kSizes);
	printf("  plain text           %8.3f\n", ascii);
	printf("  a character no font has %5.3f\n",
		first_width_time(kMissing, 12.01f, kSizes));
	// the plain text sizes again: their entries are still cached, unless
	// the fallback fonts pushed them out (the cache holds 30)
	printf("  plain text again     %8.3f (the sizes of the first line)\n",
		first_width_time(kAscii, 11.01f, kSizes));
	printf("  CJK                  %8.3f\n",
		first_width_time(kCJK, 13.01f, kSizes));
	printf("  emoji                %8.3f\n",
		first_width_time(kEmoji, 14.01f, kSizes));
	printf("  symbol               %8.3f\n",
		first_width_time(kSymbol, 15.01f, kSizes));

	// a character no font has, asked again and again
	BFont font(be_plain_font);
	const int32 kCalls = 2000;
	bool has[2];
	bigtime_t start = system_time();
	for (int32 i = 0; i < kCalls; i++)
		font.GetHasGlyphs("a\xf4\x8f\xbf\xbd", 2, has, true);
	printf("GetHasGlyphs() of a character no font has, with fallbacks: "
		"%.1f us per call (%d %d)\n",
		(double)(system_time() - start) / kCalls, has[0], has[1]);
	start = system_time();
	for (int32 i = 0; i < kCalls; i++)
		font.GetHasGlyphs("a\xe6\x97\xa5", 2, has, true);
	printf("GetHasGlyphs() of a CJK character, with fallbacks: "
		"%.1f us per call (%d %d)\n",
		(double)(system_time() - start) / kCalls, has[0], has[1]);
}


static uint32
hash_bytes(const void* data, size_t size, uint32 hash = 2166136261u)
{
	const uint8* bytes = (const uint8*)data;
	for (size_t i = 0; i < size; i++)
		hash = (hash ^ bytes[i]) * 16777619u;
	return hash;
}


static void
fingerprint()
{
	static const char* kStrings[] = {
		"Plain text",
		"\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e CJK",
		"\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4 Hangul",
		"\xe0\xb8\xa0\xe0\xb8\xb2\xe0\xb8\xa9\xe0\xb8\xb2 Thai",
		"\xe1\x8e\xa0\xe1\x8e\xa1 Cherokee",
		"\xe2\x8c\x98 \xe2\x9c\x93 \xe2\x99\xa5 \xe2\x98\x83 symbols",
		"\xf0\x9f\x98\x80 \xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd emoji",
		"\xf0\x9f\x87\xa6\xf0\x9f\x87\xba flag, 1\xef\xb8\x8f\xe2\x83\xa3",
		"\xe2\x98\xba\xef\xb8\x8e \xe2\x98\xba\xef\xb8\x8f text/emoji",
		"\xf4\x8f\xbf\xbd \xee\x80\x80 none",
		"\xd7\xa2\xd7\x91\xd7\xa8\xd7\x99\xd7\xaa \xd8\xb9\xd8\xb1\xd8\xa8",
		"\xf0\x9d\x84\x9e music \xf0\x93\x80\x80",
	};
	static const float kSizes[] = { 9, 12, 12.5f, 18, 40 };
	const BFont* kFonts[] = { be_plain_font, be_bold_font, be_fixed_font };

	BBitmap bitmap(BRect(0, 0, 799, 79), B_RGB32, true);
	BView* view = new BView(bitmap.Bounds(), "view", 0, 0);
	bitmap.AddChild(view);

	uint32 all = 2166136261u;
	for (int32 f = 0; f < 3; f++) {
		for (size_t s = 0; s < sizeof(kSizes) / sizeof(kSizes[0]); s++) {
			BFont font(kFonts[f]);
			font.SetSize(kSizes[s]);
			for (size_t i = 0; i < sizeof(kStrings) / sizeof(kStrings[0]);
					i++) {
				const char* string = kStrings[i];
				int32 chars = BString(string).CountChars();
				float width = server_width(font, string);
				float escapements[64];
				font.GetEscapements(string, chars, escapements);
				bool has[64];
				font.GetHasGlyphs(string, chars, has, true);
				char hasText[65];
				for (int32 c = 0; c < chars; c++)
					hasText[c] = has[c] ? '1' : '0';
				hasText[chars] = '\0';

				bitmap.Lock();
				view->SetHighColor(255, 255, 255);
				view->FillRect(view->Bounds());
				view->SetHighColor(0, 0, 0);
				view->SetLowColor(255, 255, 255);
				view->SetFont(&font);
				view->DrawString(string, BPoint(4, 56));
				view->Sync();
				bitmap.Unlock();
				uint32 pixels = hash_bytes(bitmap.Bits(),
					bitmap.BitsLength());

				uint32 line = hash_bytes(&width, sizeof(width));
				line = hash_bytes(escapements, chars * sizeof(float), line);
				line = hash_bytes(has, chars, line);
				line = hash_bytes(&pixels, sizeof(pixels), line);
				all = hash_bytes(&line, sizeof(line), all);
				printf("fp font %d size %g string %2d: width %.6f has %s "
					"pixels %08x\n", (int)f, kSizes[s], (int)i, width,
					hasText, pixels);
			}
		}
	}
	printf("fingerprint of all: %08x\n", all);
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	signal(SIGSEGV, crash_handler);
	signal(SIGBUS, crash_handler);
	signal(SIGABRT, crash_handler);

	sDeadline = system_time() + 300 * 1000000LL;
	resume_thread(spawn_thread(watchdog, "watchdog", B_NORMAL_PRIORITY, NULL));

	BApplication app("application/x-vnd.airos-fallback-fonts-test");

	bool all = argc < 2;
	if (all || strcmp(argv[1], "measure") == 0)
		measure();
	if (all || strcmp(argv[1], "fingerprint") == 0)
		fingerprint();

	sDeadline = 0;
	return 0;
}
