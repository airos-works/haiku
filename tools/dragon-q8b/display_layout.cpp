/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Lab tool: app_server's display layout from the shell, as the Screens
	preferences see and set it.
		display_layout					the displays, their modes
		display_layout <id> <w> <h> [refresh]
										that display at that mode, its region
										the mode's size (scale 100) */


#include <stdio.h>
#include <stdlib.h>

#include <Application.h>
#include <Message.h>

#include <InterfacePrivate.h>


using BPrivate::get_display_layout;
using BPrivate::set_display_layout;


static void
print_layout(const BMessage& layout)
{
	BMessage display;
	for (int32 i = 0; layout.FindMessage("display", i, &display) == B_OK; i++) {
		BRect frame = display.FindRect("frame");
		printf("display %" B_PRId32 " \"%s\" (%s)%s%s: %gx%g at %g,%g, mode "
			"%" B_PRId32 "x%" B_PRId32 " %.2f Hz, native %" B_PRId32 "x%" B_PRId32
			", scale %" B_PRId32 "\n", display.GetInt32("id", -1),
			display.GetString("name", ""), display.GetString("monitor", ""),
			display.GetBool("connected", false) ? "" : " not connected",
			display.GetBool("enabled", false) ? "" : " off",
			frame.Width() + 1, frame.Height() + 1, frame.left, frame.top,
			display.GetInt32("mode width", 0), display.GetInt32("mode height", 0),
			display.GetFloat("mode refresh", 0),
			display.GetInt32("native width", 0),
			display.GetInt32("native height", 0), display.GetInt32("scale", 0));
		BMessage mode;
		for (int32 m = 0; display.FindMessage("modes", m, &mode) == B_OK; m++) {
			printf("  %" B_PRId32 "x%" B_PRId32 " %.2f Hz\n",
				mode.GetInt32("width", 0), mode.GetInt32("height", 0),
				mode.GetFloat("refresh", 0));
		}
	}
}


int
main(int argc, char** argv)
{
	BApplication application("application/x-vnd.airos-display-layout");

	BMessage layout;
	status_t status = get_display_layout(layout);
	if (status != B_OK) {
		fprintf(stderr, "get_display_layout: %s\n", strerror(status));
		return 1;
	}
	if (argc < 4) {
		print_layout(layout);
		return 0;
	}

	int32 id = atoi(argv[1]);
	int32 width = atoi(argv[2]);
	int32 height = atoi(argv[3]);
	float refresh = argc > 4 ? atof(argv[4]) : 60.0f;

	BMessage request;
	BMessage display;
	bool found = false;
	for (int32 i = 0; layout.FindMessage("display", i, &display) == B_OK; i++) {
		if (!display.GetBool("connected", false))
			continue;
		BMessage entry;
		int32 displayID = display.GetInt32("id", -1);
		BRect frame = display.FindRect("frame");
		entry.AddInt32("id", displayID);
		entry.AddBool("enabled", display.GetBool("enabled", true));
		entry.AddInt32("mirror", display.GetInt32("mirror", -1));
		if (displayID == id) {
			found = true;
			frame.right = frame.left + width - 1;
			frame.bottom = frame.top + height - 1;
			entry.AddRect("frame", frame);
			entry.AddInt32("scale", 100);
			entry.AddInt32("mode width", width);
			entry.AddInt32("mode height", height);
			entry.AddFloat("mode refresh", refresh);
		} else {
			entry.AddRect("frame", frame);
			entry.AddInt32("scale", display.GetInt32("scale", 100));
		}
		if (display.GetBool("primary", false))
			entry.AddBool("primary", true);
		request.AddMessage("display", &entry);
	}
	if (!found) {
		fprintf(stderr, "no display %" B_PRId32 "\n", id);
		return 1;
	}

	status = set_display_layout(request);
	printf("set_display_layout: %s\n", strerror(status));
	if (get_display_layout(layout) == B_OK)
		print_layout(layout);
	return status == B_OK ? 0 : 1;
}
