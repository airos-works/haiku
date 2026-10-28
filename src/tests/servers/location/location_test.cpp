/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	location_test [max age in seconds]: asks location_server for the
	position and prints it, with the time the answer took.
*/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Messenger.h>
#include <OS.h>

#include <location_server.h>


int
main(int argc, char** argv)
{
	BMessenger server(LOCATION_SERVER_SIGNATURE);
	BMessage request(LOCATION_GET_POSITION);
	if (argc > 1)
		request.AddInt64("max_age", atoll(argv[1]) * 1000000LL);
	BMessage reply;
	bigtime_t start = system_time();
	status_t status = server.SendMessage(&request, &reply, B_INFINITE_TIMEOUT,
		30000000);
	if (status == B_OK)
		status = reply.GetInt32("status", B_ERROR);
	double took = (system_time() - start) / 1000.0;
	if (status != B_OK) {
		printf("no position: %s (%.0f ms)\n", strerror(status), took);
		return 1;
	}
	printf("%.5f %.5f, accuracy %.0f m, source %s (%.0f ms)\n",
		reply.GetDouble("latitude", 0), reply.GetDouble("longitude", 0),
		reply.GetDouble("accuracy", 0), reply.GetString("source", "?"), took);
	return 0;
}
