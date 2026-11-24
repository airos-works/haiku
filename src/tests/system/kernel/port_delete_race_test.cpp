/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	port_delete_race_test [seconds]: read_port() against delete_port()
	(airos-works/haiku#168).

	The kernel looked at a port's state, then locked the port, and did not
	look again: a reader that got the lock after the port's deletion had
	woken up the waiters went to sleep on the deleted port, with nobody left
	to wake it but its timeout.

	One thread creates and deletes ports; the others read from the newest
	one, with a timeout that is far longer than a port lives. A read must
	end with a message, or with B_BAD_PORT_ID when the port goes away; one
	that takes the whole timeout has slept through the deletion. Two more
	threads write large messages to the port: the kernel copies them with
	the port locked, so that readers have to wait for the lock, which is
	when the deletion can overtake them.
*/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <OS.h>


static const bigtime_t kReadTimeout = 2000000;

static volatile bool sStop;
static int32 sPort = -1;
static int32 sReads;
static int32 sSleptThrough;
static int32 sOther;


static status_t
reader(void*)
{
	while (!sStop) {
		port_id port = atomic_get(&sPort);
		if (port < 0)
			continue;

		int32 code;
		char buffer[16];
		bigtime_t start = system_time();
		ssize_t result = read_port_etc(port, &code, buffer, sizeof(buffer),
			B_RELATIVE_TIMEOUT, kReadTimeout);
		atomic_add(&sReads, 1);
		if (result < 0 && system_time() - start >= kReadTimeout) {
			// Woken by the timeout only. (It then finds the port gone and
			// returns B_BAD_PORT_ID, as if the deletion had woken it.)
			atomic_add(&sSleptThrough, 1);
		} else if (result < 0 && result != B_BAD_PORT_ID)
			atomic_add(&sOther, 1);
	}
	return B_OK;
}


static status_t
writer(void*)
{
	const size_t kSize = 512 * 1024;
	char* buffer = (char*)malloc(kSize);
	memset(buffer, 'w', kSize);

	while (!sStop) {
		port_id port = atomic_get(&sPort);
		if (port >= 0)
			write_port_etc(port, 'test', buffer, kSize, B_RELATIVE_TIMEOUT, 0);
	}

	free(buffer);
	return B_OK;
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	const int seconds = argc > 1 ? atoi(argv[1]) : 20;

	system_info info;
	get_system_info(&info);
	// more readers than CPUs: the port's lock is contended when the port
	// goes, and some reader is preempted on its way to it
	int32 readers = info.cpu_count * 4;
	if (readers > 64)
		readers = 64;

	thread_id threads[64];
	for (int32 i = 0; i < readers; i++) {
		threads[i] = spawn_thread(i < 2 ? writer : reader,
			i < 2 ? "writer" : "reader", B_NORMAL_PRIORITY, NULL);
		resume_thread(threads[i]);
	}

	const bigtime_t end = system_time() + seconds * 1000000LL;
	int64 ports = 0;
	uint32 spin = 0;
	while (system_time() < end && atomic_get(&sSleptThrough) == 0) {
		port_id port = create_port(2, "delete race test");
		if (port < 0) {
			printf("create_port() failed: %s\n", strerror(port));
			break;
		}
		atomic_set(&sPort, port);
		// give the readers a varying, short time to get to it
		for (volatile uint32 i = 0; i < (spin % 200000); i++) {
		}
		spin += 9973;
		delete_port(port);
		ports++;
	}

	sStop = true;
	for (int32 i = 0; i < readers; i++)
		wait_for_thread(threads[i], NULL);

	printf("%" B_PRId64 " ports, %" B_PRId32 " reads by %" B_PRId32
		" readers and writers: %" B_PRId32 " slept through the deletion of "
		"their port, %" B_PRId32 " ended with an unexpected error\n", ports,
		atomic_get(&sReads), readers, atomic_get(&sSleptThrough),
		atomic_get(&sOther));

	bool ok = sSleptThrough == 0 && sOther == 0;
	printf("%s\n", ok ? "PASSED" : "FAILED");
	return ok ? 0 : 1;
}
