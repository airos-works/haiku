/*
 * Copyright 2026, air/OS.
 * Distributed under the terms of the MIT License.
 */


/*!	BFont::StringWidth() and GetStringWidths() remember widths in the
	application (#191). This checks that what they return is, bit for bit,
	what app_server answers when it is asked directly (AS_GET_STRING_WIDTHS,
	the request the kit made for every call before), for a sample of strings
	(ASCII, accented, Greek, Cyrillic, CJK, Thai, emoji and their sequences,
	symbols no font has, broken UTF-8, lengths that cut a character) in
	several fonts, sizes and spacing modes; cold, warm, from several threads
	at once, and after the hinting mode changed. Then it measures.

	string_width_cache_test          checks, then measures
	string_width_cache_test check    only checks
	string_width_cache_test measure  only measures

	Exit status 0 when every width was the server's, 1 otherwise.
*/


#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <Application.h>
#include <Font.h>
#include <ListView.h>
#include <OS.h>
#include <String.h>
#include <Window.h>

#include <AppServerLink.h>
#include <ServerProtocol.h>


void set_hinting_mode(uint8 hinting);
status_t get_hinting_mode(uint8* hinting);


static int32 sFailures = 0;
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


/*!	What app_server says, asked the way the kit asked before it remembered
	anything.
*/
static void
server_widths(const BFont& font, const char* strings[], const int32 lengths[],
	int32 count, float widths[])
{
	BPrivate::AppServerLink link;
	link.StartMessage(AS_GET_STRING_WIDTHS);
	link.Attach<uint16>(font.FamilyAndStyle() >> 16);
	link.Attach<uint16>(font.FamilyAndStyle() & 0xffff);
	link.Attach<float>(font.Size());
	link.Attach<uint8>(font.Spacing());
	link.Attach<int32>(count);
	for (int32 i = 0; i < count; i++)
		link.AttachString(strings[i], lengths[i]);

	int32 status = B_ERROR;
	if (link.FlushWithReply(status) != B_OK || status != B_OK) {
		for (int32 i = 0; i < count; i++)
			widths[i] = -1;
		return;
	}
	link.Read(widths, sizeof(float) * count);
}


//	#pragma mark - the sample


struct Sample {
	BString	text;
	int32	length;
		// can be shorter or longer than the text
};


static Sample* sSamples = NULL;
static int32 sSampleCount = 0;


static void
add_sample(const char* text, int32 length = -1)
{
	sSamples[sSampleCount].text = text;
	sSamples[sSampleCount].length = length < 0 ? (int32)strlen(text) : length;
	sSampleCount++;
}


static void
make_sample()
{
	static const char* kFixed[] = {
		"Save changes to this document?",
		"OK", "Cancel", "File", "Edit", "W", "i", " ", "  two  spaces ",
		"AVAV To Ty Wa", "fi fl ffi",
		"Größe ändern, café, naïve, Ærø, ½ ¼ © ®",
		"Ελληνικά κείμενο", "Русский текст", "日本語のテキスト", "中文字符",
		"한국어 텍스트", "ภาษาไทย", "עברית", "العربية",
		"→ ← ↑ ↓ ⌘ ✓ ✗ ♥ ★ ☃ ∑ ∞ ≠",
		"\xf0\x9f\x98\x80", "Hello \xf0\x9f\x98\x80 world",
		"\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd thumbs",
		"\xf0\x9f\x87\xa6\xf0\x9f\x87\xba flag",
		"1\xef\xb8\x8f\xe2\x83\xa3 keycap", "#\xe2\x83\xa3", "123 456",
		"\xe2\x98\xba\xef\xb8\x8e text style", "\xe2\x98\xba\xef\xb8\x8f",
		"\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f"
			"\x91\xa7 family",
		"A\xef\xb8\x8f after text", "\xe2\x80\x8d zwj alone",
		"e\xcc\x81 combining", "\xee\x80\x80 private use",
		"\xf4\x8f\xbf\xbd unassigned", "\xf0\x9f\xaf\x8a unknown",
		"\xff\xfe broken", "cut \xe6\x97", "\xc3", "\x80\x80\x80",
		"tab\there", "line\nbreak",
	};
	const int32 kFixedCount = sizeof(kFixed) / sizeof(kFixed[0]);

	// pieces for random strings: single characters of all kinds
	static const char* kPieces[] = {
		"a", "b", "Q", "W", "m", "1", "7", "#", "*", " ", ".", ",", "-",
		"\xc3\xa4", "\xc3\x9f", "\xce\xbb", "\xd0\x96", "\xe6\x97\xa5",
		"\xe8\xaa\x9e", "\xea\xb0\x80", "\xe0\xb8\x81", "\xe2\x86\x92",
		"\xe2\x9c\x93", "\xf0\x9f\x98\x80", "\xf0\x9f\x8f\xbd",
		"\xef\xb8\x8f", "\xe2\x80\x8d", "\xe2\x83\xa3", "\xee\x80\x80",
		"\xf0\x9f\x87\xa6", "\xcc\x81", "\xff",
	};
	const int32 kPieceCount = sizeof(kPieces) / sizeof(kPieces[0]);
	const int32 kRandomCount = 1500;

	sSamples = new Sample[kFixedCount + 8 + kRandomCount];
	for (int32 i = 0; i < kFixedCount; i++)
		add_sample(kFixed[i]);

	// lengths that do not match the text
	add_sample("length cuts \xe6\x97\xa5 a character", 14);
	add_sample("length cuts \xe6\x97\xa5 a character", 13);
	add_sample("emoji cut \xf0\x9f\x98\x80", 12);
	add_sample("short", 2);
	add_sample("longer than the text", 200);
	add_sample("", 0);
	add_sample("x", 0);
	add_sample("keycap 1\xef\xb8\x8f", 8);
		// the digit is the last character the length lets in

	srand(1124);
	for (int32 i = 0; i < kRandomCount; i++) {
		BString text;
		int32 count = 1 + rand() % 24;
		// most strings are of a few kinds of characters, like real text
		int32 pool = 1 + rand() % kPieceCount;
		for (int32 j = 0; j < count; j++)
			text << kPieces[rand() % pool];
		add_sample(text.String());
	}
}


//	#pragma mark - checks


struct FontCase {
	const BFont*	base;
	float			size;
	uint8			spacing;
	uint16			face;
	float			shear;
};


static void
make_font(const FontCase& fontCase, BFont& font)
{
	font = *fontCase.base;
	font.SetSize(fontCase.size);
	font.SetSpacing(fontCase.spacing);
	if (fontCase.face != 0)
		font.SetFace(fontCase.face);
	if (fontCase.shear != 90)
		font.SetShear(fontCase.shear);
}


static int32
compare(const char* what, const BFont& font, int32 index, float kit,
	float server)
{
	// bit for bit: what was computed before is what is remembered now
	if (memcmp(&kit, &server, sizeof(float)) == 0)
		return 0;
	if (atomic_add(&sFailures, 1) < 20) {
		font_family family;
		font_style style;
		font.GetFamilyAndStyle(&family, &style);
		printf("  %s: \"%s\" (length %d) in %s %s %g, spacing %d: kit %.9g,"
			" server %.9g\n", what, sSamples[index].text.String(),
			(int)sSamples[index].length, family, style, font.Size(),
			font.Spacing(), kit, server);
	}
	return 1;
}


/*!	All samples in \a font: one by one (StringWidth()), all at once
	(GetStringWidths()), and one by one again.
*/
static int32
check_font(const BFont& font)
{
	const char** strings = new const char*[sSampleCount];
	int32* lengths = new int32[sSampleCount];
	float* expected = new float[sSampleCount];
	float* widths = new float[sSampleCount];
	for (int32 i = 0; i < sSampleCount; i++) {
		strings[i] = sSamples[i].text.String();
		lengths[i] = sSamples[i].length;
	}

	// the server in batches of 64, as a big request would be by area
	for (int32 i = 0; i < sSampleCount; i += 64) {
		int32 count = sSampleCount - i < 64 ? sSampleCount - i : 64;
		server_widths(font, strings + i, lengths + i, count, expected + i);
	}

	int32 wrong = 0;
	for (int32 i = 0; i < sSampleCount; i++) {
		wrong += compare("StringWidth", font, i,
			font.StringWidth(strings[i], lengths[i]), expected[i]);
	}
	for (int32 i = 0; i < sSampleCount; i += 50) {
		int32 count = sSampleCount - i < 50 ? sSampleCount - i : 50;
		font.GetStringWidths(strings + i, lengths + i, count, widths + i);
	}
	for (int32 i = 0; i < sSampleCount; i++)
		wrong += compare("GetStringWidths", font, i, widths[i], expected[i]);
	for (int32 i = sSampleCount - 1; i >= 0; i--) {
		wrong += compare("StringWidth again", font, i,
			font.StringWidth(strings[i], lengths[i]), expected[i]);
	}

	delete[] strings;
	delete[] lengths;
	delete[] expected;
	delete[] widths;
	return wrong;
}


static FontCase* sFontCases = NULL;
static int32 sFontCaseCount = 0;


static void
make_font_cases()
{
	static const float kSizes[] = { 7, 9.5f, 10, 12, 12.3f, 18, 31, 48, 100 };
	static const uint8 kSpacings[] = { B_CHAR_SPACING, B_STRING_SPACING,
		B_BITMAP_SPACING, B_FIXED_SPACING };
	const BFont* fonts[] = { be_plain_font, be_bold_font, be_fixed_font };

	sFontCases = new FontCase[3 * 9 * 4 + 4];
	for (int32 f = 0; f < 3; f++) {
		for (int32 s = 0; s < 9; s++) {
			for (int32 p = 0; p < 4; p++) {
				FontCase& fontCase = sFontCases[sFontCaseCount++];
				fontCase.base = fonts[f];
				fontCase.size = kSizes[s];
				fontCase.spacing = kSpacings[p];
				fontCase.face = 0;
				fontCase.shear = 90;
			}
		}
	}
	// what a width request does not carry: the same widths
	FontCase extra[] = {
		{ be_plain_font, 12, B_BITMAP_SPACING, B_ITALIC_FACE, 90 },
		{ be_plain_font, 12, B_BITMAP_SPACING, 0, 60 },
		{ be_plain_font, 12, B_BITMAP_SPACING, B_UNDERSCORE_FACE, 90 },
		{ be_bold_font, 12, B_CHAR_SPACING, 0, 120 },
	};
	for (int32 i = 0; i < 4; i++)
		sFontCases[sFontCaseCount++] = extra[i];
}


static int32
check_all(const char* what)
{
	bigtime_t start = system_time();
	int32 wrong = 0;
	for (int32 i = 0; i < sFontCaseCount; i++) {
		BFont font;
		make_font(sFontCases[i], font);
		wrong += check_font(font);
	}
	printf("%s %s: %d widths of %d strings in %d fonts, %d differ from the "
		"server's (%.1f s)\n", wrong == 0 ? "ok  " : "FAIL", what,
		(int)(sSampleCount * sFontCaseCount * 3), (int)sSampleCount,
		(int)sFontCaseCount, (int)wrong,
		(system_time() - start) / 1000000.0);
	return wrong;
}


static status_t
check_thread(void* data)
{
	// each thread goes through the fonts from another one on
	int32 first = (int32)(addr_t)data;
	int32 wrong = 0;
	for (int32 i = 0; i < sFontCaseCount; i++) {
		BFont font;
		make_font(sFontCases[(first + i * 7) % sFontCaseCount], font);
		wrong += check_font(font);
	}
	return wrong;
}


static int32
check_threads()
{
	const int32 kThreads = 4;
	thread_id threads[kThreads];
	for (int32 i = 0; i < kThreads; i++) {
		threads[i] = spawn_thread(check_thread, "check", B_NORMAL_PRIORITY,
			(void*)(addr_t)(i * 13));
		resume_thread(threads[i]);
	}
	int32 wrong = 0;
	for (int32 i = 0; i < kThreads; i++) {
		status_t result = 0;
		wait_for_thread(threads[i], &result);
		wrong += result;
	}
	printf("%s 4 threads at once: %d differ from the server's\n",
		wrong == 0 ? "ok  " : "FAIL", (int)wrong);
	return wrong;
}


static int32
check()
{
	make_sample();
	make_font_cases();

	int32 wrong = check_all("cold, then warm");
	wrong += check_all("warm");
	wrong += check_threads();

	// another hinting mode: other advances; nothing remembered may be used
	uint8 hinting = 1;
	if (get_hinting_mode(&hinting) == B_OK) {
		// (the widths the kit remembers before differ from those after)
		BFont font(be_plain_font);
		font.SetSpacing(B_BITMAP_SPACING);
		font.SetSize(12.3f);
		const char* text = sSamples[0].text.String();
		float before = font.StringWidth(text);
		uint8 other = hinting == 0 ? 1 : 0;
		set_hinting_mode(other);
		uint8 now = hinting;
		get_hinting_mode(&now);
		printf("     hinting %d -> %d, \"%s\" at 12.3 in bitmap spacing: "
			"%.9g -> %.9g\n", hinting, now, text, before,
			font.StringWidth(text));
		wrong += check_all("after the hinting mode changed");
		set_hinting_mode(hinting);
		get_hinting_mode(&now);
		printf("     hinting back to %d: %.9g\n", now,
			font.StringWidth(text));
		wrong += check_all("after it changed back");
	} else {
		printf("FAIL could not read the hinting mode\n");
		wrong++;
	}
	return wrong;
}


//	#pragma mark - measurements


static void
measure()
{
	BFont font(be_plain_font);
	const int32 kCalls = 20000;
	char text[64];

	bigtime_t start = system_time();
	float sum = 0;
	for (int32 i = 0; i < kCalls; i++)
		sum += font.StringWidth("Save changes to this document?");
	bigtime_t same = system_time() - start;

	start = system_time();
	for (int32 i = 0; i < kCalls; i++) {
		snprintf(text, sizeof(text), "File name number %d.txt", (int)i);
		sum += font.StringWidth(text);
	}
	bigtime_t different = system_time() - start;

	start = system_time();
	for (int32 i = 0; i < kCalls; i++) {
		snprintf(text, sizeof(text), "Größe %d → 日本語 Ελληνικά",
			(int)i);
		sum += font.StringWidth(text);
	}
	bigtime_t mixed = system_time() - start;

	start = system_time();
	for (int32 i = 0; i < kCalls; i++) {
		snprintf(text, sizeof(text), "emoji %d \xf0\x9f\x98\x80",
			(int)(i % 500));
		sum += font.StringWidth(text);
	}
	bigtime_t emoji = system_time() - start;

	printf("BFont::StringWidth(), %d calls each, us per call: "
		"same string %.2f, different strings %.2f, different with "
		"accents/CJK/Greek %.2f, 500 strings with an emoji %.2f (%g)\n",
		(int)kCalls, (double)same / kCalls, (double)different / kCalls,
		(double)mixed / kCalls, (double)emoji / kCalls, sum);

	// a list of 10000 items in a window: BListView::AddItem() has each
	// BStringItem measure its text
	BWindow* window = new BWindow(BRect(50, 50, 349, 449), "list",
		B_TITLED_WINDOW, 0);
	BListView* list = new BListView(BRect(0, 0, 299, 399), "list");
	window->AddChild(list);
	window->Show();
	window->Lock();
	start = system_time();
	for (int32 i = 0; i < 10000; i++) {
		snprintf(text, sizeof(text), "Item number %d of the list", (int)i);
		list->AddItem(new BStringItem(text));
	}
	bigtime_t items = system_time() - start;
	window->Quit();
	printf("10000 BStringItems added to a BListView in a window: %.1f ms\n",
		items / 1000.0);
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	signal(SIGSEGV, crash_handler);
	signal(SIGBUS, crash_handler);
	signal(SIGABRT, crash_handler);

	sDeadline = system_time() + 600 * 1000000LL;
	resume_thread(spawn_thread(watchdog, "watchdog", B_NORMAL_PRIORITY, NULL));

	BApplication app("application/x-vnd.airos-string-width-cache-test");

	bool all = argc < 2;
	int32 wrong = 0;
	if (all || strcmp(argv[1], "check") == 0)
		wrong = check();
	if (all || strcmp(argv[1], "measure") == 0)
		measure();

	sDeadline = 0;
	printf("%s\n", wrong == 0 && sFailures == 0
		? "all widths were the server's" : "some widths were WRONG");
	return wrong == 0 && sFailures == 0 ? 0 : 1;
}
