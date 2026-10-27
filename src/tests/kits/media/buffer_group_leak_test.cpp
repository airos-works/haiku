// buffer_group_leak_test hold N | check: "hold" creates a BBufferGroup of N buffers and
// sleeps (to be killed); "check" creates one of 8 and reports. Buffers of a
// killed team stayed in the media kit's shared buffer list for ever, which
// holds 2047 (summit#119: buffer groups failed until media_server restarted).
#include <Application.h>
#include <BufferGroup.h>
#include <MediaRoster.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(int argc, char** argv)
{
	BApplication app("application/x-vnd.airos-bgleak");
	BMediaRoster::Roster();
	bool hold = argc > 1 && strcmp(argv[1], "hold") == 0;
	int32 count = hold && argc > 2 ? atoi(argv[2]) : 8;
	BBufferGroup group(4096, count);
	int32 buffers = 0;
	group.CountBuffers(&buffers);
	printf("%s: InitCheck %s, %d buffers\n", hold ? "hold" : "check",
		strerror(group.InitCheck()), (int)buffers);
	fflush(stdout);
	if (hold) {
		for (;;)
			snooze(1000000);
	}
	return group.InitCheck() == B_OK ? 0 : 1;
}
