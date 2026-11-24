/*
 * Copyright 2026, air/OS.
 * Distributed under the terms of the MIT License.
 */


/*!	Sends app_server requests whose sizes do not describe what they carry
	(a bitmap whose length overflows 32 bits, a shape whose ops ask for more
	points than it has, a payload "by area" that is larger than the area, a
	flattened message with a damaged header, ...) and captures a window that
	is partly off screen. app_server has to refuse or clip each of them and
	keep running: it is one process for every application.

	as_bad_requests_test            runs every case
	as_bad_requests_test list       lists the cases
	as_bad_requests_test <case>...  runs the named cases

	Exit status 0 when app_server answered after every case and is still the
	same team, 1 otherwise. A case that takes app_server down ends the run
	(there is nobody left to talk to); run the cases one by one, with a
	restart in between, to see which of them an unfixed server survives.
*/


#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <new>

#include <OS.h>

// the test needs the window's link to app_server and a bitmap's server area
#define private public
#define protected public
#include <Application.h>
#include <Bitmap.h>
#include <Message.h>
#include <View.h>
#include <Window.h>
#undef private
#undef protected

#include <Font.h>
#include <Screen.h>
#include <String.h>

#include <AppServerLink.h>
#include <MessagePrivate.h>
#include <PortLink.h>
#include <ServerProtocol.h>
#include <ServerProtocolStructs.h>
#include <ShapePrivate.h>
#include <WindowInfo.h>


extern "C" area_id _kern_transfer_area(area_id area, void** _address,
	uint32 addressSpec, team_id target);


static const bigtime_t kAnswerTimeout = 20 * 1000000LL;
static const rgb_color kFillColor = { 30, 200, 90, 255 };

static bigtime_t sDeadline = 0;
static const char* sCurrentCase = "";


//	#pragma mark - helpers


static team_id
app_server_team()
{
	team_info info;
	int32 cookie = 0;
	while (get_next_team_info(&cookie, &info) == B_OK) {
		if (strstr(info.args, "app_server") != NULL
			&& strstr(info.args, "as_bad_requests") == NULL)
			return info.team;
	}
	return -1;
}


static status_t
watchdog(void*)
{
	// every wait for app_server has a deadline: a server that crashed sits
	// in the debugger and never answers
	while (true) {
		snooze(200000);
		if (sDeadline != 0 && system_time() > sDeadline) {
			printf("FAIL %s: no answer from app_server within %d s (it "
				"crashed or hangs)\n", sCurrentCase,
				(int)(kAnswerTimeout / 1000000));
			fflush(stdout);
			_exit(1);
		}
	}
	return B_OK;
}


static void
crash_handler(int signal)
{
	// never the crash dialog: it waits for somebody to click
	const char* text = "FAIL: the test itself crashed\n";
	write(STDOUT_FILENO, text, strlen(text));
	_exit(1);
}


class FillView : public BView {
public:
	FillView(BRect frame)
		:
		BView(frame, "fill", B_FOLLOW_ALL, B_WILL_DRAW),
		fDrawCount(0)
	{
		SetViewColor(B_TRANSPARENT_COLOR);
	}

	virtual void Draw(BRect updateRect)
	{
		SetHighColor(kFillColor);
		FillRect(Bounds());
		atomic_add(&fDrawCount, 1);
	}

	int32	fDrawCount;
};


class TestWindow : public BWindow {
public:
	TestWindow(BRect frame, const char* title)
		:
		BWindow(frame, title, B_TITLED_WINDOW,
			B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_AVOID_FOCUS)
	{
		fView = new FillView(Bounds());
		AddChild(fView);
	}

	FillView*	fView;
};


static TestWindow*
open_window(BRect frame, const char* title = "as_bad_requests")
{
	// (server_token() finds a window by its title)
	TestWindow* window = new TestWindow(frame, title);
	window->Show();
	// wait until it drew once
	bigtime_t end = system_time() + 5000000;
	while (atomic_get(&window->fView->fDrawCount) == 0 && system_time() < end)
		snooze(20000);
	return window;
}


static int32
server_token(BWindow* window)
{
	int32 result = -1;
	int32 count = 0;
	int32* tokens = get_token_list(getpid(), &count);
	for (int32 i = 0; tokens != NULL && i < count; i++) {
		client_window_info* info = get_window_info(tokens[i]);
		if (info == NULL)
			continue;
		if (strcmp(info->name, window->Title()) == 0)
			result = info->server_token;
		free(info);
	}
	free(tokens);
	return result;
}


//!	The window's link, with \a view as the server's current view.
static BPrivate::PortLink*
window_link(TestWindow* window)
{
	window->fView->_CheckLockAndSwitchCurrent();
	return window->fLink;
}


//!	Whether app_server still answers on the window's and the application's link.
static void
sync_window(TestWindow* window)
{
	window->Sync();
}


static void
draw_bitmap(TestWindow* window, int32 token, BRect bitmapRect)
{
	ViewDrawBitmapInfo info;
	info.bitmapToken = token;
	info.options = 0;
	info.viewRect = BRect(0, 0, 99, 99);
	info.bitmapRect = bitmapRect;

	window->Lock();
	BPrivate::PortLink* link = window_link(window);
	link->StartMessage(AS_VIEW_DRAW_BITMAP);
	link->Attach<ViewDrawBitmapInfo>(info);
	sync_window(window);
	window->Unlock();
}


//	#pragma mark - the cases


typedef bool (*case_function)(TestWindow* window);


static bool
bitmap_length_overflow(TestWindow* window)
{
	// 16384 x 65537 pixels of 4 bytes: 4 GiB + 64 KiB, which is 64 KiB in
	// 32 bits
	BPrivate::AppServerLink link;
	link.StartMessage(AS_CREATE_BITMAP);
	link.Attach<BRect>(BRect(0, 0, 16383, 65536));
	link.Attach<color_space>(B_RGB32);
	link.Attach<uint32>(0);
	link.Attach<int32>(B_ANY_BYTES_PER_ROW);
	link.Attach<int32>(B_MAIN_SCREEN_ID.id);

	int32 code = B_ERROR;
	if (link.FlushWithReply(code) != B_OK)
		return false;
	if (code != B_OK) {
		printf("  app_server refused the bitmap: %s\n", strerror(code));
		return true;
	}

	int32 token;
	uint8 allocationFlags;
	area_id area;
	ssize_t offset;
	link.Read<int32>(&token);
	link.Read<uint8>(&allocationFlags);
	link.Read<area_id>(&area);
	link.Read<ssize_t>(&offset);
	area_info info;
	info.size = 0;
	get_area_info(area, &info);
	printf("  app_server made the bitmap (token %d, in an area of %lu "
		"bytes); drawing its last rows\n", (int)token, (unsigned long)info.size);

	draw_bitmap(window, token, BRect(0, 65400, 99, 65499));
	return false;
}


static bool
bitmap_bounds_not_finite(TestWindow* window)
{
	const float kInfinity = __builtin_inff();
	const float kNaN = __builtin_nanf("");
	BRect frames[] = {
		BRect(0, 0, kInfinity, 10), BRect(0, 0, 10, kNaN),
		BRect(0, 0, 3e9, 3e9), BRect(10, 10, 0, 0), BRect(0, 0, 1e30, 0)
	};
	bool refused = true;
	for (size_t i = 0; i < sizeof(frames) / sizeof(frames[0]); i++) {
		BPrivate::AppServerLink link;
		link.StartMessage(AS_CREATE_BITMAP);
		link.Attach<BRect>(frames[i]);
		link.Attach<color_space>(B_RGB32);
		link.Attach<uint32>(0);
		link.Attach<int32>(B_ANY_BYTES_PER_ROW);
		link.Attach<int32>(B_MAIN_SCREEN_ID.id);
		int32 code = B_ERROR;
		if (link.FlushWithReply(code) != B_OK)
			return false;
		if (code == B_OK) {
			int32 token = -1;
			link.Read<int32>(&token);
			printf("  bounds %zu accepted (token %d)\n", i, (int)token);
			refused = false;
			draw_bitmap(window, token, BRect(0, 0, 99, 99));
		}
	}
	// a huge row length with a height that makes the product wrap
	{
		BPrivate::AppServerLink link;
		link.StartMessage(AS_CREATE_BITMAP);
		link.Attach<BRect>(BRect(0, 0, 15, 65536));
		link.Attach<color_space>(B_RGB32);
		link.Attach<uint32>(0);
		link.Attach<int32>(0x10000);
		link.Attach<int32>(B_MAIN_SCREEN_ID.id);
		int32 code = B_ERROR;
		if (link.FlushWithReply(code) != B_OK)
			return false;
		if (code == B_OK) {
			int32 token = -1;
			link.Read<int32>(&token);
			printf("  16 x 65537 with 64 KiB rows accepted (token %d)\n",
				(int)token);
			refused = false;
			draw_bitmap(window, token, BRect(0, 65400, 15, 65499));
		}
	}
	if (refused)
		printf("  app_server refused all of them\n");
	return refused;
}


static bool
reconnect_bitmap(TestWindow* window, size_t offset)
{
	// a bitmap of 4 MiB "in" a client area of one page
	void* address;
	area_id area = create_area("small bitmap area", &address, B_ANY_ADDRESS,
		B_PAGE_SIZE, B_NO_LOCK, B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA);
	if (area < 0)
		return false;

	BPrivate::AppServerLink link;
	link.StartMessage(AS_RECONNECT_BITMAP);
	link.Attach<BRect>(BRect(0, 0, 1023, 1023));
	link.Attach<color_space>(B_RGB32);
	link.Attach<uint32>(0);
	link.Attach<int32>(4096);
	link.Attach<int32>(0);
	link.Attach<int32>(area);
	link.Attach<size_t>(offset);

	int32 code = B_ERROR;
	if (link.FlushWithReply(code) != B_OK)
		return false;
	if (code != B_OK) {
		printf("  app_server refused the bitmap: %s\n", strerror(code));
		delete_area(area);
		return true;
	}
	int32 token;
	link.Read<int32>(&token);
	printf("  app_server took a 1024 x 1024 bitmap at offset %lu of a %d "
		"byte area (token %d); drawing its last rows\n",
		(unsigned long)offset, (int)B_PAGE_SIZE, (int)token);
	draw_bitmap(window, token, BRect(0, 900, 99, 999));
	return false;
}


static bool
reconnect_bitmap_too_large(TestWindow* window)
{
	return reconnect_bitmap(window, 0);
}


static bool
reconnect_bitmap_offset(TestWindow* window)
{
	return reconnect_bitmap(window, (size_t)1 << 40);
}


static void
send_shape(TestWindow* window, int32 code, int32 opCount, const uint32* ops,
	int32 pointCount, const BPoint* points)
{
	window->Lock();
	BPrivate::PortLink* link = window_link(window);
	link->StartMessage(code);
	if (code == AS_VIEW_CLIP_TO_SHAPE)
		link->Attach<bool>(false);
	else
		link->Attach<BRect>(BRect(0, 0, 100, 100));
	link->Attach<int32>(opCount);
	link->Attach<int32>(pointCount);
	link->Attach(ops, opCount * sizeof(uint32));
	link->Attach(points, pointCount * sizeof(BPoint));
	sync_window(window);
	if (code == AS_VIEW_CLIP_TO_SHAPE) {
		// the mask is made when something is drawn through it
		window->fView->FillRect(BRect(0, 0, 50, 50));
		sync_window(window);
		window->fView->ConstrainClippingRegion(NULL);
	}
	window->Unlock();
}


static bool
shape_more_points_than_sent(TestWindow* window)
{
	// one op that takes 16 million points, with two points
	uint32 ops[] = { OP_MOVETO | OP_LINETO | 0x00ffffff };
	BPoint points[] = { BPoint(10, 10), BPoint(50, 50) };
	send_shape(window, AS_FILL_SHAPE, 1, ops, 2, points);
	send_shape(window, AS_STROKE_SHAPE, 1, ops, 2, points);
	return true;
}


static bool
shape_curve_count(TestWindow* window)
{
	// a curve takes three points; with four the count never reaches zero
	uint32 ops[] = { OP_MOVETO | OP_BEZIERTO | 4 };
	BPoint points[] = { BPoint(10, 10), BPoint(50, 50), BPoint(60, 10),
		BPoint(70, 70), BPoint(80, 10) };
	send_shape(window, AS_FILL_SHAPE, 1, ops, 5, points);
	return true;
}


static bool
shape_clip(TestWindow* window)
{
	uint32 ops[] = { OP_MOVETO | OP_LINETO | 0x00ffffff };
	BPoint points[] = { BPoint(10, 10), BPoint(50, 50) };
	send_shape(window, AS_VIEW_CLIP_TO_SHAPE, 1, ops, 2, points);
	return true;
}


static bool
shape_counts(TestWindow* window)
{
	// counts whose sizes in bytes do not fit 32 bits, and negative ones;
	// no data follows
	static const int32 kCounts[][2] = { { 0x40000001, 0x20000001 },
		{ -1, -1 }, { 0x7fffffff, 0x7fffffff }, { 1, -5 } };
	static const int32 kCodes[] = { AS_VIEW_CLIP_TO_SHAPE, AS_FILL_SHAPE };
	for (size_t i = 0; i < sizeof(kCounts) / sizeof(kCounts[0]); i++) {
		for (size_t j = 0; j < 2; j++) {
			window->Lock();
			BPrivate::PortLink* link = window_link(window);
			link->StartMessage(kCodes[j]);
			if (kCodes[j] == AS_VIEW_CLIP_TO_SHAPE)
				link->Attach<bool>(false);
			else
				link->Attach<BRect>(BRect(0, 0, 100, 100));
			link->Attach<int32>(kCounts[i][0]);
			link->Attach<int32>(kCounts[i][1]);
			// what would be the lists: an op, and two words that are not
			// an area's id
			link->Attach<uint32>(OP_MOVETO | OP_LINETO | 1);
			link->Attach<int32>(-1);
			link->Attach<int32>(-1);
			sync_window(window);
			window->Unlock();
		}
	}
	return true;
}


/*!	A polygon of 16 million points (128 MiB, so "by area"), with an area of
	one page. \a transfer: the area is given to app_server, as the kit does;
	otherwise it stays this team's.
*/
static bool
payload_area(TestWindow* window, bool transfer)
{
	void* address;
	area_id area = create_area("small payload", &address, B_ANY_ADDRESS,
		B_PAGE_SIZE, B_NO_LOCK, B_READ_AREA | B_WRITE_AREA);
	if (area < 0)
		return false;
	if (transfer) {
		area_id transferred = _kern_transfer_area(area, &address,
			B_ANY_ADDRESS, app_server_team());
		if (transferred < 0) {
			printf("  could not transfer the area: %s\n",
				strerror(transferred));
			delete_area(area);
			return false;
		}
		area = transferred;
	}

	window->Lock();
	BPrivate::PortLink* link = window_link(window);
	link->StartMessage(AS_FILL_POLYGON);
	link->Attach<BRect>(BRect(0, 0, 100, 100));
	link->Attach<int32>(16 * 1024 * 1024);
	link->Attach<area_id>(area);
	sync_window(window);
	window->Unlock();

	if (!transfer) {
		// it was not app_server's to delete
		area_info info;
		if (get_area_info(area, &info) != B_OK) {
			printf("  the area is gone\n");
			return false;
		}
		delete_area(area);
	}
	return true;
}


static bool
payload_area_too_small(TestWindow* window)
{
	return payload_area(window, true);
}


static bool
payload_area_foreign(TestWindow* window)
{
	return payload_area(window, false);
}


static bool
decorator_settings_size(TestWindow* window)
{
	// the size of a buffer on the server's stack
	static const int32 kSizes[] = { 0x7ffffff0, -1, 0, 64 * 1024 * 1024 };
	for (size_t i = 0; i < sizeof(kSizes) / sizeof(kSizes[0]); i++) {
		window->Lock();
		BPrivate::PortLink* link = window->fLink;
		link->StartMessage(AS_SET_DECORATOR_SETTINGS);
		link->Attach<int32>(kSizes[i]);
		link->Attach<int32>(-1);
		sync_window(window);
		window->Unlock();
	}

	// and a message that ends before its header does
	window->Lock();
	BPrivate::PortLink* link = window->fLink;
	link->StartMessage(AS_SET_DECORATOR_SETTINGS);
	link->Attach<int32>(8);
	link->Attach<uint32>('1FMH');
		// MESSAGE_FORMAT_HAIKU
	link->Attach<uint32>('abcd');
	sync_window(window);
	window->Unlock();
	return true;
}


static bool
draw_string_length(TestWindow* window)
{
	// lengths that wrap when the terminating null and the rounding are added
	static const int32 kLengths[] = { 0x7fffffff, 0x7fffffc1, -1 };
	for (size_t i = 0; i < sizeof(kLengths) / sizeof(kLengths[0]); i++) {
		ViewDrawStringInfo info;
		info.stringLength = kLengths[i];
		info.location = BPoint(10, 10);
		info.delta.nonspace = 0;
		info.delta.space = 0;
		window->Lock();
		BPrivate::PortLink* link = window_link(window);
		link->StartMessage(AS_DRAW_STRING);
		link->Attach<ViewDrawStringInfo>(info);
		link->Attach<int32>(-1);
		sync_window(window);
		window->Unlock();
	}
	return true;
}


/*!	Hands \a buffer to app_server as the description of a bitmap's vector
	icon: a flattened message that the server unflattens and looks fields
	up in.
*/
static status_t
send_flattened(BBitmap& bitmap, const void* buffer, int32 size)
{
	BPrivate::AppServerLink link;
	link.StartMessage(AS_SET_BITMAP_VECTOR_ICON);
	link.Attach<int32>(bitmap._ServerToken());
	link.Attach<int32>(size);
	link.Attach(buffer, size);
	int32 code = B_ERROR;
	status_t status = link.FlushWithReply(code);
	return status != B_OK ? status : code;
}


static bool
flattened_message(TestWindow* window, int variant)
{
	BBitmap bitmap(BRect(0, 0, 15, 15), B_RGBA32);
	if (bitmap.InitCheck() != B_OK)
		return false;

	BMessage message('test');
	message.AddData("data", B_RAW_TYPE, "0123456789abcdef", 16);
	message.AddRect("bounds", BRect(0, 0, 15, 15));
	message.AddRect("crop", BRect(0, 0, 15, 15));
	ssize_t size = message.FlattenedSize();
	char* buffer = new char[size];
	message.Flatten(buffer, size);

	BMessage::message_header* header = (BMessage::message_header*)buffer;
	BMessage::field_header* fields
		= (BMessage::field_header*)(buffer + sizeof(BMessage::message_header));
	const char* what = "";
	switch (variant) {
		case 0:
			what = "hash table size 0";
			header->hash_table_size = 0;
			break;
		case 1:
			what = "hash table size 100000";
			header->hash_table_size = 100000;
			break;
		case 2:
			what = "hash entries past the fields";
			for (int32 i = 0; i < MESSAGE_BODY_HASH_TABLE_SIZE; i++)
				header->hash_table[i] = 0x100000 + i;
			break;
		case 3:
			what = "a field chain that loops";
			for (uint32 i = 0; i < header->field_count; i++)
				fields[i].next_field = i;
			for (int32 i = 0; i < MESSAGE_BODY_HASH_TABLE_SIZE; i++)
				header->hash_table[i] = 0;
			break;
		case 4:
			what = "a field offset that wraps in 32 bits";
			for (uint32 i = 0; i < header->field_count; i++) {
				fields[i].offset = 0xfffffff0;
				fields[i].data_size = 0x20;
			}
			break;
		case 5:
			what = "a field count of 100 million";
			header->field_count = 100 * 1000 * 1000;
			break;
	}
	status_t status = send_flattened(bitmap, buffer, size);
	printf("  %s: %s\n", what, strerror(status));
	delete[] buffer;
	return status != B_OK;
}


static bool
message_hash_table(TestWindow* window)
{
	bool refused = flattened_message(window, 0);
	refused &= flattened_message(window, 1);
	refused &= flattened_message(window, 2);
	return refused;
}


static bool
message_field_chain(TestWindow* window)
{
	return flattened_message(window, 3);
}


static bool
message_field_offsets(TestWindow* window)
{
	bool refused = flattened_message(window, 4);
	refused &= flattened_message(window, 5);
	return refused;
}


static bool
message_by_area(TestWindow* window)
{
	// A flattened message that says its fields are in an area, and names
	// one of app_server's own: the memory of a bitmap. Unflattening makes
	// that area read-only and deletes it with the message.
	BBitmap victim(BRect(0, 0, 255, 255), B_RGBA32);
	BBitmap carrier(BRect(0, 0, 15, 15), B_RGBA32);
	if (victim.InitCheck() != B_OK || carrier.InitCheck() != B_OK)
		return false;
	memset(victim.Bits(), 0, victim.BitsLength());

	BMessage message('test');
	message.AddInt32("x", 1);
	ssize_t size = message.FlattenedSize();
	char* buffer = new char[size];
	message.Flatten(buffer, size);
	BMessage::message_header* header = (BMessage::message_header*)buffer;
	header->flags |= MESSAGE_FLAG_PASS_BY_AREA;
	header->message_area = victim.fServerArea;
	for (int32 i = 0; i < MESSAGE_BODY_HASH_TABLE_SIZE; i++)
		header->hash_table[i] = -1;

	status_t status = send_flattened(carrier, buffer, size);
	printf("  a message \"in\" area %d (a bitmap's, app_server's own): %s\n",
		(int)victim.fServerArea, strerror(status));
	delete[] buffer;

	// the bitmap's memory must still be there for the server
	window->Lock();
	window->fView->DrawBitmap(&victim, BPoint(0, 0));
	sync_window(window);
	window->Unlock();
	area_info info;
	if (get_area_info(victim.fServerArea, &info) != B_OK) {
		printf("  the bitmap's area is gone\n");
		return false;
	}
	return status != B_OK;
}


static bool
string_widths_64(TestWindow* window)
{
	// not a bad request at all: 64 strings in one BFont::GetStringWidths()
	const int32 kCount = 64;
	const char* strings[kCount];
	int32 lengths[kCount];
	float widths[kCount];
	for (int32 i = 0; i < kCount; i++) {
		strings[i] = "width";
		lengths[i] = 5;
		widths[i] = -1;
	}
	be_plain_font->GetStringWidths(strings, lengths, kCount, widths);
	float single = be_plain_font->StringWidth("width");
	printf("  width of the last of 64 strings: %g (alone: %g)\n",
		widths[kCount - 1], single);
	return widths[kCount - 1] == single && widths[0] == single;
}


static bool
string_widths_missing_strings(TestWindow* window)
{
	// four strings announced, none sent
	BPrivate::AppServerLink link;
	link.StartMessage(AS_GET_STRING_WIDTHS);
	link.Attach<uint16>(be_plain_font->FamilyAndStyle() >> 16);
	link.Attach<uint16>(be_plain_font->FamilyAndStyle() & 0xffff);
	link.Attach<float>(12.0f);
	link.Attach<uint8>(B_BITMAP_SPACING);
	link.Attach<int32>(4);
	int32 code = B_ERROR;
	if (link.FlushWithReply(code) != B_OK)
		return false;
	printf("  four strings announced, none sent: %s\n", strerror(code));
	return true;
}


static bool
glyph_string_not_terminated(TestWindow* window)
{
	// more characters announced than the bytes hold
	char bytes[256];
	memset(bytes, 'x', sizeof(bytes));
	BPrivate::AppServerLink link;
	link.StartMessage(AS_GET_HAS_GLYPHS);
	link.Attach<uint16>(be_plain_font->FamilyAndStyle() >> 16);
	link.Attach<uint16>(be_plain_font->FamilyAndStyle() & 0xffff);
	link.Attach<int32>(4 * 1024 * 1024);
	link.Attach<int32>(sizeof(bytes));
	link.Attach(bytes, sizeof(bytes));
	link.Attach<bool>(false);
	int32 code = B_ERROR;
	if (link.FlushWithReply(code) != B_OK)
		return false;
	printf("  4 million characters in 256 bytes: %s\n", strerror(code));
	return true;
}


/*!	Captures a window at \a frame (screen coordinates, partly off screen)
	into a bitmap of \a targetWidth pixels (0: the window's width) and
	checks what arrived for the part that is on screen.
*/
static bool
capture(BRect frame, int32 targetWidth, const char* what)
{
	TestWindow* window = open_window(frame, "as_bad_requests capture");
	int32 token = server_token(window);
	window->Lock();
	BRect actual = window->Frame();
	window->Unlock();
	if (actual != frame) {
		printf("  the window is at (%g, %g, %g, %g), not where it was put\n",
			actual.left, actual.top, actual.right, actual.bottom);
	}
	BRect bounds = frame.OffsetToCopy(B_ORIGIN);
	if (targetWidth > 0)
		bounds.right = targetWidth - 1;
	BBitmap bitmap(bounds, B_RGB32);
	memset(bitmap.Bits(), 0, bitmap.BitsLength());

	bool drawn = false;
	status_t status = BPrivate::read_window_bitmap(token, &bitmap, 2000000,
		&drawn);
	printf("  %s: %s, drawn %d\n", what, strerror(status), drawn);

	bool ok = true;
	if (status == B_OK && drawn) {
		// every pixel of the part of the window that is on screen has the
		// view's colour, every one above or left of the screen is untouched
		BRect screen = BScreen().Frame();
		int32 wrong = 0;
		BRect filledArea(1e9, 1e9, -1e9, -1e9);
		for (int32 y = 0; y <= bounds.IntegerHeight(); y++) {
			const uint8* row = (const uint8*)bitmap.Bits()
				+ y * bitmap.BytesPerRow();
			for (int32 x = 0; x <= bounds.IntegerWidth(); x++) {
				bool onScreen = screen.Contains(BPoint(frame.left + x,
					frame.top + y));
				bool filled = row[x * 4] == kFillColor.blue
					&& row[x * 4 + 1] == kFillColor.green
					&& row[x * 4 + 2] == kFillColor.red;
				bool untouched = row[x * 4] == 0 && row[x * 4 + 1] == 0
					&& row[x * 4 + 2] == 0;
				if (filled) {
					filledArea.left = std::min(filledArea.left, (float)x);
					filledArea.top = std::min(filledArea.top, (float)y);
					filledArea.right = std::max(filledArea.right, (float)x);
					filledArea.bottom = std::max(filledArea.bottom, (float)y);
				}
				// above and left of the screen nothing is captured; below
				// and right of it the capture may or may not reach
				bool beforeScreen = frame.left + x < screen.left
					|| frame.top + y < screen.top;
				if (onScreen ? !filled
						: beforeScreen ? !untouched : !filled && !untouched) {
					wrong++;
				}
			}
		}
		printf("  %d wrong pixels in the captured bitmap (filled: %g, %g, "
			"%g, %g)\n", (int)wrong, filledArea.left, filledArea.top,
			filledArea.right, filledArea.bottom);
		ok = wrong == 0;
	}

	window->Lock();
	window->Sync();
	window->Quit();
	return ok;
}


static bool
capture_above_screen(TestWindow*)
{
	return capture(BRect(100, -1500, 499, 199), 0,
		"window with 1500 rows above the screen");
}


static bool
capture_left_of_screen(TestWindow*)
{
	return capture(BRect(-300, 100, 199, 399), 0,
		"window with 300 columns left of the screen");
}


static bool
capture_narrow_target(TestWindow*)
{
	// the target holds less than what is off screen: a negative width
	return capture(BRect(-300, 100, 199, 399), 100,
		"the same, into a bitmap 100 pixels wide");
}


static bool
capture_far_away(TestWindow*)
{
	// a bitmap up to this window's right bottom corner would be 19 GiB
	BRect screen = BScreen().Frame();
	bool ok = capture(BRect(screen.right - 100, screen.bottom - 100,
		screen.right + 70000, screen.bottom + 70000), 400,
		"window that ends 70000 pixels beyond the screen");
	return ok;
}


static const struct {
	const char*		name;
	case_function	function;
	bool			needsWindow;
} kCases[] = {
	{ "bitmap-length-overflow", bitmap_length_overflow, true },
	{ "bitmap-bounds-not-finite", bitmap_bounds_not_finite, true },
	{ "reconnect-bitmap-too-large", reconnect_bitmap_too_large, true },
	{ "reconnect-bitmap-offset", reconnect_bitmap_offset, true },
	{ "shape-more-points-than-sent", shape_more_points_than_sent, true },
	{ "shape-curve-count", shape_curve_count, true },
	{ "shape-clip", shape_clip, true },
	{ "shape-counts", shape_counts, true },
	{ "payload-area-too-small", payload_area_too_small, true },
	{ "payload-area-foreign", payload_area_foreign, true },
	{ "decorator-settings-size", decorator_settings_size, true },
	{ "draw-string-length", draw_string_length, true },
	{ "message-hash-table", message_hash_table, true },
	{ "message-field-chain", message_field_chain, true },
	{ "message-field-offsets", message_field_offsets, true },
	{ "message-by-area", message_by_area, true },
	{ "string-widths-64", string_widths_64, false },
	{ "string-widths-missing-strings", string_widths_missing_strings, false },
	{ "glyph-string-not-terminated", glyph_string_not_terminated, false },
	{ "capture-above-screen", capture_above_screen, false },
	{ "capture-left-of-screen", capture_left_of_screen, false },
	{ "capture-narrow-target", capture_narrow_target, false },
	{ "capture-far-away", capture_far_away, false },
};
static const int32 kCaseCount = sizeof(kCases) / sizeof(kCases[0]);


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	if (argc > 1 && strcmp(argv[1], "list") == 0) {
		for (int32 i = 0; i < kCaseCount; i++)
			printf("%s\n", kCases[i].name);
		return 0;
	}

	signal(SIGSEGV, crash_handler);
	signal(SIGBUS, crash_handler);
	signal(SIGFPE, crash_handler);
	signal(SIGABRT, crash_handler);

	sDeadline = system_time() + kAnswerTimeout;
	sCurrentCase = "start";
	resume_thread(spawn_thread(watchdog, "watchdog", B_NORMAL_PRIORITY, NULL));

	BApplication app("application/x-vnd.airos-as-bad-requests-test");
	team_id serverTeam = app_server_team();
	printf("app_server is team %d\n", (int)serverTeam);

	TestWindow* window = open_window(BRect(100, 100, 399, 299));

	int failed = 0;
	for (int32 i = 0; i < kCaseCount; i++) {
		bool selected = argc < 2;
		for (int arg = 1; arg < argc; arg++)
			selected |= strcmp(argv[arg], kCases[i].name) == 0;
		if (!selected)
			continue;

		sCurrentCase = kCases[i].name;
		sDeadline = system_time() + kAnswerTimeout;
		printf("%s\n", kCases[i].name);
		bool ok = kCases[i].function(window);

		// app_server has to answer on both links, and be the team it was
		window->Lock();
		window->Sync();
		window->Unlock();
		BScreen().Frame();
		be_plain_font->StringWidth("still there?");
		snooze(300000);
		team_id team = app_server_team();
		if (team != serverTeam) {
			printf("FAIL %s: app_server was team %d and is team %d now\n",
				kCases[i].name, (int)serverTeam, (int)team);
			return 1;
		}
		if (ok)
			printf("ok   %s\n", kCases[i].name);
		else {
			printf("FAIL %s: app_server runs, but did not refuse the "
				"request or returned a wrong result\n", kCases[i].name);
			failed++;
		}
	}
	sDeadline = 0;

	window->Lock();
	window->Quit();

	printf("%s\n", failed == 0 ? "all cases passed" : "some cases FAILED");
	return failed == 0 ? 0 : 1;
}
