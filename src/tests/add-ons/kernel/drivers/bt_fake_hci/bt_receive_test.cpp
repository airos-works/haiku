/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	The HCI module's receive path (#228), through the bt_fake_hci driver: a
	transport driver with no radio behind it that hands the module what this
	program gives it, as a USB radio's completion callbacks do. This program
	stands in for the bluetooth_server: it creates the port the module
	forwards events to.

	Install the driver as non-packaged (add-ons/kernel/drivers/bin/bt_fake_hci
	and a link to it in add-ons/kernel/drivers/dev/misc), make sure no
	bluetooth_server runs, then

	bt_receive_test [deliver|unmatched|stalled|split|short|unregister|all]

	deliver		an event reaches the server
	unmatched	a packet for a radio that is not registered (one just closed)
				does not end reception: the next event still arrives
	stalled		the server does not read its port: handing over packets
				must never wait (on a USB radio the caller is the host
				controller's completion thread), and events arrive again
				once the server reads
	split		an event that comes in two pieces arrives whole
	short		pieces too short to be a packet are dropped, reception goes
				on (these used to panic)
	unregister	a radio closed while the receive thread works on it
				(waiting for the server) is freed only after that

	Exit status 0 when every check passed. A kernel that panics shows it in
	the serial log, of course.
*/


#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <OS.h>

#include <bluetooth/HCI/btHCI.h>
#include <bluetooth/HCI/btHCI_transport.h>

#include "bt_fake_hci.h"


static const char* kReceiveThread = BT_RX_PORT_NAME " thread";

static int sDevice = -1;
static port_id sServerPort = -1;
static int32 sId = -1;
static int sFailures = 0;
static uint16 sSequence = 0;


static void
check(bool ok, const char* what)
{
	if (!ok)
		sFailures++;
	printf("%s: %s\n", ok ? "ok" : "FAILED", what);
	fflush(stdout);
}


static status_t
watchdog(void*)
{
	snooze(180000000);
	printf("FAILED: the test did not finish in 3 minutes\n");
	fflush(stdout);
	_exit(3);
	return B_OK;
}


static status_t
post(int32 id, bt_packet_t type, const void* data, size_t size,
	bigtime_t* _duration = NULL)
{
	bt_fake_post request;
	memset(&request, 0, sizeof(request));
	request.id = id;
	request.type = type;
	request.size = size;
	memcpy(request.data, data, size);
	if (ioctl(sDevice, BT_FAKE_POST, &request, sizeof(request)) != 0)
		return errno;
	if (_duration != NULL)
		*_duration = request.duration;
	return request.status;
}


/*!	A vendor event (0xff) nothing in the stack looks at, with a sequence
	number to recognise it by.
*/
static size_t
make_event(uint8* event, uint16 sequence, uint8 length = 6)
{
	event[0] = 0xff;
	event[1] = length;
	for (uint8 i = 0; i < length; i++)
		event[2 + i] = 0xa0 + i;
	event[2] = sequence & 0xff;
	event[3] = sequence >> 8;
	return 2 + length;
}


/*!	Waits for the event with that sequence number at the server port,
	dropping others (old ones the module still had queued).
*/
static bool
receive_event(uint16 sequence, bigtime_t timeout, size_t expectedSize = 8)
{
	bigtime_t deadline = system_time() + timeout;
	while (true) {
		uint8 buffer[512];
		int32 code;
		ssize_t size = read_port_etc(sServerPort, &code, buffer,
			sizeof(buffer), B_ABSOLUTE_TIMEOUT, deadline);
		if (size < 0)
			return false;
		if (size >= 4 && buffer[0] == 0xff
			&& (buffer[2] | (buffer[3] << 8)) == sequence) {
			if ((size_t)size != expectedSize) {
				printf("event %u arrived with %zd bytes, not %zu\n",
					sequence, size, expectedSize);
				return false;
			}
			return true;
		}
	}
}


static void
drain_server_port()
{
	uint8 buffer[512];
	int32 code;
	while (read_port_etc(sServerPort, &code, buffer, sizeof(buffer),
			B_RELATIVE_TIMEOUT, 0) >= 0) {
	}
}


static bool
deliver_one(const char* what)
{
	uint8 event[16];
	uint16 sequence = ++sSequence;
	size_t size = make_event(event, sequence);
	status_t status = post(sId, BT_EVENT, event, size);
	bool ok = status == B_OK && receive_event(sequence, 3000000);
	check(ok, what);
	return ok;
}


static void
test_deliver()
{
	deliver_one("an event reaches the server");
}


static void
test_unmatched()
{
	uint8 event[16];
	size_t size = make_event(event, 0xeeee);
	post(sId + 50, BT_EVENT, event, size);
	snooze(200000);

	check(find_thread(kReceiveThread) >= 0,
		"after a packet for an unregistered radio, the receive thread is "
		"still there");
	deliver_one("after a packet for an unregistered radio, events still "
		"arrive");
}


static void
test_stalled()
{
	// The server's port as it used to be (24 messages), nobody reading.
	delete_port(sServerPort);
	sServerPort = create_port(24, BT_USERLAND_PORT_NAME);

	const int32 kPackets = 400;
	bigtime_t longest = 0;
	int32 posted = 0;
	int32 refused = 0;
	bigtime_t start = system_time();
	for (int32 i = 0; i < kPackets; i++) {
		uint8 event[16];
		size_t size = make_event(event, 0x8000 + i);
		bigtime_t duration = 0;
		status_t status = post(sId, BT_EVENT, event, size, &duration);
		posted++;
		if (status != B_OK)
			refused++;
		if (duration > longest)
			longest = duration;
		if (duration > 200000) {
			// It waits: that is the bug, no need to wait for all of them.
			break;
		}
	}
	char text[256];
	snprintf(text, sizeof(text), "%" B_PRId32 " events to a server that "
		"does not read: the longest hand-over took %" B_PRId64 " us (%"
		B_PRId32 " dropped, %" B_PRId64 " ms in all)", posted, longest,
		refused, (system_time() - start) / 1000);
	check(posted == kPackets && longest < 50000, text);

	// The server reads again.
	bigtime_t until = system_time() + 3000000;
	while (system_time() < until) {
		drain_server_port();
		snooze(10000);
	}
	deliver_one("once the server reads again, events arrive");

	delete_port(sServerPort);
	sServerPort = create_port(128, BT_USERLAND_PORT_NAME);
}


static void
test_split()
{
	uint8 event[16];
	uint16 sequence = ++sSequence;
	size_t size = make_event(event, sequence, 10);
	post(sId, BT_EVENT, event, 5);
	post(sId, BT_EVENT, event + 5, size - 5);
	check(receive_event(sequence, 3000000, size),
		"an event in two pieces arrives whole");
	deliver_one("and the next one too");
}


static void
test_short()
{
	uint8 junk[4] = { 0x0e, 0x00, 0x00, 0x00 };
	post(sId, BT_EVENT, junk, 1);
	post(sId, BT_ACL, junk, 2);
	snooze(200000);
	deliver_one("after a 1-byte event and a 2-byte ACL packet, events still "
		"arrive");
}


static void
test_unregister()
{
	// The receive thread waits for the server: its port is full.
	delete_port(sServerPort);
	sServerPort = create_port(1, BT_USERLAND_PORT_NAME);
	uint8 event[16];
	size_t size = make_event(event, 0x7000);
	post(sId, BT_EVENT, event, size);
	size = make_event(event, 0x7001);
	post(sId, BT_EVENT, event, size);
	snooze(10000);

	bigtime_t start = system_time();
	int32 id = sId;
	status_t status = ioctl(sDevice, BT_FAKE_UNREGISTER, &id, sizeof(id)) == 0
		? B_OK : errno;
	bigtime_t took = system_time() - start;
	char text[256];
	snprintf(text, sizeof(text), "closing a radio while the receive thread "
		"waits for the server: %s after %" B_PRId64 " ms", strerror(status),
		took / 1000);
	check(status == B_OK, text);

	delete_port(sServerPort);
	sServerPort = create_port(128, BT_USERLAND_PORT_NAME);
	check(ioctl(sDevice, BT_FAKE_REGISTER, &sId, sizeof(sId)) == 0,
		"the radio is registered again");
	deliver_one("events arrive from it");
}


int
main(int argc, char** argv)
{
	const char* which = argc > 1 ? argv[1] : "all";
	bool all = strcmp(which, "all") == 0;

	resume_thread(spawn_thread(watchdog, "watchdog", B_NORMAL_PRIORITY,
		NULL));

	if (find_port(BT_USERLAND_PORT_NAME) >= 0) {
		fprintf(stderr, "a bluetooth_server is running: stop it first\n");
		return 2;
	}
	sServerPort = create_port(128, BT_USERLAND_PORT_NAME);

	sDevice = open("/dev/" BT_FAKE_HCI_DEVICE, O_RDWR);
	if (sDevice < 0) {
		fprintf(stderr, "no /dev/%s: %s\n", BT_FAKE_HCI_DEVICE,
			strerror(errno));
		return 2;
	}
	if (ioctl(sDevice, BT_FAKE_REGISTER, &sId, sizeof(sId)) != 0) {
		fprintf(stderr, "could not register a radio: %s\n", strerror(errno));
		return 2;
	}
	printf("radio %" B_PRId32 ", receive thread %" B_PRId32 "\n", sId,
		find_thread(kReceiveThread));

	if (all || strcmp(which, "deliver") == 0)
		test_deliver();
	if (all || strcmp(which, "unmatched") == 0)
		test_unmatched();
	if (all || strcmp(which, "split") == 0)
		test_split();
	if (all || strcmp(which, "short") == 0)
		test_short();
	if (all || strcmp(which, "stalled") == 0)
		test_stalled();
	if (all || strcmp(which, "unregister") == 0)
		test_unregister();

	ioctl(sDevice, BT_FAKE_UNREGISTER, &sId, sizeof(sId));
	close(sDevice);
	delete_port(sServerPort);

	printf("%d failed\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
