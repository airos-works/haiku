/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Lab tool for /dev/misc/rpi_gpio (<rpi_gpio.h>), and for its Radxa Cubie
	A7S twin /dev/misc/sunxi_gpio (built as cubie_gpio): the first of the
	two that exists. Pins are the driver's numbers (the Cubie's are header
	positions, J6 pin n being 30 + n).

	rpi4_gpio info
	rpi4_gpio state
	rpi4_gpio set <pin> in [up|down|none]	configure and let go
	rpi4_gpio set <pin> out <0|1>
	rpi4_gpio watch <pin> [up|down|none] [seconds]
	rpi4_gpio selftest [pin]				needs nothing wired to the pin

	The self test drives the pin through its pull resistors and as an
	output, so the pin must be free (default GPIO 26, header pin 37, on the
	Pi; J11 pin 26, PD14, on the Cubie). */


#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <OS.h>

#include <rpi_gpio.h>


static const char* kFunctionNames[] = {
	"input", "output", "alt5", "alt4", "alt0", "alt1", "alt2", "alt3"
};
// the A733's pin multiplexer values
static const char* kSunxiFunctionNames[] = {
	"input", "output", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10",
	"f11", "f12", "f13", "eint", "off"
};
static const char* kPullNames[] = { "none", "up", "down", "?" };

static int sFailures;
static const char* sDevicePath;
static uint32 sBoard;


static int
open_device()
{
	static const char* kPaths[] = {
		RPI_GPIO_DEVICE_PATH, SUNXI_GPIO_DEVICE_PATH
	};
	for (const char* path : kPaths) {
		if (sDevicePath != NULL && strcmp(path, sDevicePath) != 0)
			continue;
		int fd = open(path, O_RDWR);
		if (fd < 0)
			continue;
		if (sDevicePath == NULL) {
			sDevicePath = path;
			rpi_gpio_info info;
			if (ioctl(fd, RPI_GPIO_GET_INFO, &info, sizeof(info)) == 0)
				sBoard = info.board;
		}
		return fd;
	}
	fprintf(stderr, "%s, %s: %s\n", RPI_GPIO_DEVICE_PATH,
		SUNXI_GPIO_DEVICE_PATH, strerror(errno));
	exit(1);
}


static const char*
function_name(uint8 function)
{
	if (sBoard == RPI_GPIO_BOARD_CUBIE_A7S)
		return kSunxiFunctionNames[function & 15];
	return kFunctionNames[function & 7];
}


static status_t
control(int fd, uint32 op, void* data)
{
	return ioctl(fd, op, data, 0) == 0 ? B_OK : errno;
}


static status_t
claim(int fd, uint32 pin, uint8 function, uint8 pull, int8 level,
	uint8 flags = 0)
{
	rpi_gpio_claim request = { pin, function, pull, level, flags };
	return control(fd, RPI_GPIO_CLAIM, &request);
}


static status_t
release(int fd, uint32 pin)
{
	return control(fd, RPI_GPIO_RELEASE, &pin);
}


static status_t
write_level(int fd, uint32 pin, int level)
{
	rpi_gpio_write request = { 1ull << pin, (uint64)(level != 0) << pin };
	return control(fd, RPI_GPIO_WRITE, &request);
}


static rpi_gpio_state
state_of(int fd)
{
	rpi_gpio_state state;
	if (control(fd, RPI_GPIO_GET_STATE, &state) != B_OK) {
		fprintf(stderr, "GET_STATE: %s\n", strerror(errno));
		exit(1);
	}
	return state;
}


static uint32
wait_events(int fd, rpi_gpio_event* events, uint32 capacity,
	bigtime_t timeout, uint32* _lost = NULL)
{
	rpi_gpio_wait request = {};
	request.timeout = timeout;
	request.events = events;
	request.capacity = capacity;
	status_t status = control(fd, RPI_GPIO_WAIT_EVENTS, &request);
	if (status != B_OK) {
		fprintf(stderr, "WAIT_EVENTS: %s\n", strerror(status));
		return 0;
	}
	if (_lost != NULL)
		*_lost = request.lost;
	return request.count;
}


//! All events until none came for \a quiet microseconds.
static uint32
drain(int fd, rpi_gpio_event* events, uint32 capacity, bigtime_t quiet,
	uint32* _lost = NULL)
{
	uint32 total = 0;
	uint32 lost = 0;
	while (total < capacity) {
		uint32 lostNow = 0;
		uint32 count = wait_events(fd, events + total, capacity - total, quiet,
			&lostNow);
		lost += lostNow;
		if (count == 0)
			break;
		total += count;
	}
	if (_lost != NULL)
		*_lost = lost;
	return total;
}


static void
check(bool ok, const char* format, ...)
{
	va_list args;
	va_start(args, format);
	printf("%s ", ok ? "PASS" : "FAIL");
	vprintf(format, args);
	printf("\n");
	va_end(args);
	if (!ok)
		sFailures++;
}


static int
parse_pull(const char* text)
{
	if (text == NULL || strcmp(text, "none") == 0)
		return RPI_GPIO_PULL_NONE;
	if (strcmp(text, "up") == 0)
		return RPI_GPIO_PULL_UP;
	if (strcmp(text, "down") == 0)
		return RPI_GPIO_PULL_DOWN;
	fprintf(stderr, "pull: up, down or none\n");
	exit(1);
}


static int
do_info()
{
	int fd = open_device();
	rpi_gpio_info info;
	if (control(fd, RPI_GPIO_GET_INFO, &info) != B_OK) {
		fprintf(stderr, "GET_INFO: %s\n", strerror(errno));
		return 1;
	}
	printf("%s: api %" B_PRIu32 ", board %" B_PRIu32 ", %" B_PRIu32 " pins,"
		" claimable %#" B_PRIx64 ", board revision %#" B_PRIx32 ", serial %016"
		B_PRIx64 ", %s\n", sDevicePath, info.api_version, info.board,
		info.pin_count, info.claimable, info.board_revision, info.board_serial,
		(info.flags & RPI_GPIO_INFO_EDGE_INTERRUPTS) != 0
			? "edge interrupts" : "sampled inputs");
	close(fd);
	return 0;
}


static int
do_state()
{
	int fd = open_device();
	rpi_gpio_info info;
	if (control(fd, RPI_GPIO_GET_INFO, &info) != B_OK)
		info.pin_count = 58;
	rpi_gpio_state state = state_of(fd);
	for (uint32 pin = 0; pin < info.pin_count && pin < RPI_GPIO_MAX_PINS;
			pin++) {
		printf("GPIO%-2" B_PRIu32 " %-6s pull %-4s level %d%s\n", pin,
			function_name(state.function[pin]),
			kPullNames[state.pull[pin] & 3], (int)((state.levels >> pin) & 1),
			(state.claimed_elsewhere >> pin) & 1 ? "  (claimed)" : "");
	}
	close(fd);
	return 0;
}


static int
do_set(int argc, char** argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: set <pin> in [up|down|none] | out <0|1>\n");
		return 1;
	}
	uint32 pin = strtoul(argv[2], NULL, 0);
	int fd = open_device();
	status_t status;
	if (strcmp(argv[3], "in") == 0) {
		status = claim(fd, pin, RPI_GPIO_INPUT,
			parse_pull(argc > 4 ? argv[4] : NULL), -1, RPI_GPIO_CLAIM_DETACH);
	} else if (strcmp(argv[3], "out") == 0 && argc > 4) {
		status = claim(fd, pin, RPI_GPIO_OUTPUT, RPI_GPIO_PULL_NONE,
			atoi(argv[4]) != 0 ? 1 : 0, RPI_GPIO_CLAIM_DETACH);
	} else {
		fprintf(stderr, "usage: set <pin> in [up|down|none] | out <0|1>\n");
		return 1;
	}
	if (status != B_OK)
		fprintf(stderr, "GPIO%" B_PRIu32 ": %s\n", pin, strerror(status));
	close(fd);
	return status == B_OK ? 0 : 1;
}


static int
do_watch(int argc, char** argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: watch <pin> [up|down|none] [seconds]\n");
		return 1;
	}
	uint32 pin = strtoul(argv[2], NULL, 0);
	int pull = parse_pull(argc > 3 ? argv[3] : NULL);
	bigtime_t until = system_time()
		+ (argc > 4 ? atoll(argv[4]) : 10) * 1000000LL;

	int fd = open_device();
	status_t status = claim(fd, pin, RPI_GPIO_INPUT, pull, -1);
	if (status != B_OK) {
		fprintf(stderr, "GPIO%" B_PRIu32 ": %s\n", pin, strerror(status));
		return 1;
	}
	rpi_gpio_event events[64];
	while (system_time() < until) {
		uint32 lost = 0;
		uint32 count = wait_events(fd, events, 64, until - system_time(),
			&lost);
		if (lost > 0)
			printf("%" B_PRIu32 " lost\n", lost);
		for (uint32 i = 0; i < count; i++) {
			printf("%" B_PRIdBIGTIME " GPIO%u -> %u%s%s%s\n", events[i].time,
				events[i].pin, events[i].level,
				events[i].flags & RPI_GPIO_EVENT_CLAIMED ? " (claimed)" : "",
				events[i].flags & RPI_GPIO_EVENT_WRITTEN ? " (written)" : "",
				events[i].flags & RPI_GPIO_EVENT_SAMPLED ? " (sampled)" : "");
		}
	}
	close(fd);
	return 0;
}


static int
do_selftest(int argc, char** argv)
{
	uint32 pin = argc > 2 ? strtoul(argv[2], NULL, 0) : 26;
	uint64 bit = 1ull << pin;
	rpi_gpio_event events[4096];

	int fd = open_device();
	bool cubie = sBoard == RPI_GPIO_BOARD_CUBIE_A7S;
	rpi_gpio_info info;
	check(control(fd, RPI_GPIO_GET_INFO, &info) == B_OK
		&& info.api_version == RPI_GPIO_API_VERSION
		&& info.pin_count == (cubie ? 46 : 58),
		"info: %s, api %" B_PRIu32 ", board %" B_PRIu32 ", %" B_PRIu32
		" pins, revision %#" B_PRIx32, sDevicePath, info.api_version,
		info.board, info.pin_count, info.board_revision);
	bool interrupts = (info.flags & RPI_GPIO_INFO_EDGE_INTERRUPTS) != 0;
	printf("     inputs: %s\n", interrupts ? "edge interrupts" : "sampled");

	rpi_gpio_state before = state_of(fd);
	check((before.claimed_elsewhere & bit) == 0, "GPIO%" B_PRIu32
		" is free: %s, pull %s", pin, function_name(before.function[pin]),
		kPullNames[before.pull[pin] & 3]);

	// not a header pin, or the Cubie's serial console
	check(claim(fd, cubie ? 8 : 30, RPI_GPIO_INPUT, RPI_GPIO_PULL_UP, -1)
			== B_BAD_VALUE,
		cubie ? "J11 pin 8 (the serial console) cannot be claimed"
			: "GPIO30 (Bluetooth) cannot be claimed");
	if (cubie) {
		check(claim(fd, 1, RPI_GPIO_INPUT, RPI_GPIO_PULL_UP, -1)
				== B_BAD_VALUE, "J11 pin 1 (3.3 V) cannot be claimed");
	}

	// input, pulled up
	status_t status = claim(fd, pin, RPI_GPIO_INPUT, RPI_GPIO_PULL_UP, -1);
	check(status == B_OK, "claim as input with pull-up: %s", strerror(status));
	snooze(5000);
	uint32 count = drain(fd, events, 4096, 20000);
	rpi_gpio_state state = state_of(fd);
	check(state.function[pin] == RPI_GPIO_INPUT
		&& state.pull[pin] == RPI_GPIO_PULL_UP && (state.levels & bit) != 0
		&& (state.claimed & bit) != 0,
		"state: input, pull-up, high, claimed by us");
	check(count >= 1 && (events[0].flags & RPI_GPIO_EVENT_CLAIMED) != 0
		&& events[count - 1].level == 1,
		"claim event, last level high (%" B_PRIu32 " events)", count);

	// the pull-down makes a falling edge
	bigtime_t changed = system_time();
	claim(fd, pin, RPI_GPIO_INPUT, RPI_GPIO_PULL_DOWN, -1);
	count = drain(fd, events, 4096, 50000);
	bool falling = false;
	bigtime_t latency = 0;
	for (uint32 i = 0; i < count; i++) {
		if (events[i].level == 0) {
			falling = true;
			latency = events[i].time - changed;
		}
	}
	check(falling && (state_of(fd).levels & bit) == 0,
		"pull-down: falling edge reported %" B_PRIdBIGTIME " us after the"
		" change (%" B_PRIu32 " events, flags %#x)", latency, count,
		count > 0 ? events[count - 1].flags : 0);

	// a burst of edges through the pulls
	const int kToggles = 200;
	for (int i = 0; i < kToggles; i++) {
		claim(fd, pin, RPI_GPIO_INPUT,
			i % 2 == 0 ? RPI_GPIO_PULL_UP : RPI_GPIO_PULL_DOWN, -1);
		snooze(1000);
	}
	uint32 lost = 0;
	count = drain(fd, events, 4096, 50000, &lost);
	uint32 edges = 0, claims = 0, sampled = 0;
	int lastLevel = -1;
	bool alternate = true;
	for (uint32 i = 0; i < count; i++) {
		if (events[i].flags & RPI_GPIO_EVENT_CLAIMED) {
			claims++;
			continue;
		}
		if (events[i].flags & RPI_GPIO_EVENT_SAMPLED)
			sampled++;
		if (lastLevel == events[i].level)
			alternate = false;
		lastLevel = events[i].level;
		edges++;
	}
	check(edges == (uint32)kToggles && alternate && lost == 0,
		"%d pull changes 1 ms apart: %" B_PRIu32 " edges (alternating: %s),"
		" %" B_PRIu32 " sampled, %" B_PRIu32 " lost", kToggles, edges,
		alternate ? "yes" : "no", sampled, lost);

	// as fast as the ioctl goes: the driver may throttle, nothing may hang
	const int kFastToggles = 20000;
	bigtime_t start = system_time();
	for (int i = 0; i < kFastToggles; i++) {
		claim(fd, pin, RPI_GPIO_INPUT,
			i % 2 == 0 ? RPI_GPIO_PULL_UP : RPI_GPIO_PULL_DOWN, -1);
	}
	bigtime_t took = system_time() - start;
	uint32 total = 0;
	sampled = 0;
	lost = 0;
	while (true) {
		uint32 lostNow = 0;
		count = wait_events(fd, events, 4096, 100000, &lostNow);
		lost += lostNow;
		if (count == 0)
			break;
		for (uint32 i = 0; i < count; i++) {
			if (events[i].flags & RPI_GPIO_EVENT_SAMPLED)
				sampled++;
		}
		total += count;
	}
	check(true, "%d pull changes in %" B_PRIdBIGTIME " us: %" B_PRIu32
		" events, %" B_PRIu32 " sampled, %" B_PRIu32 " lost", kFastToggles,
		took, total, sampled, lost);

	// a second descriptor may look, not touch
	int other = open_device();
	check(claim(other, pin, RPI_GPIO_OUTPUT, RPI_GPIO_PULL_NONE, 1) == B_BUSY,
		"a second descriptor cannot claim the pin");
	check((state_of(other).claimed_elsewhere & bit) != 0,
		"the second descriptor sees it claimed elsewhere");
	check(write_level(other, pin, 1) == B_NOT_ALLOWED,
		"the second descriptor cannot write it");

	// output
	status = claim(fd, pin, RPI_GPIO_OUTPUT, RPI_GPIO_PULL_NONE, 1);
	snooze(1000);
	state = state_of(fd);
	check(status == B_OK && state.function[pin] == RPI_GPIO_OUTPUT
		&& (state.levels & bit) != 0, "claim as output, high");
	drain(fd, events, 4096, 10000);
	check(write_level(fd, pin, 0) == B_OK, "write low");
	count = drain(fd, events, 4096, 20000);
	check(count == 1 && events[0].level == 0
		&& (events[0].flags & RPI_GPIO_EVENT_WRITTEN) != 0
		&& (state_of(fd).levels & bit) == 0, "written event, pin reads low");
	write_level(fd, pin, 1);
	check((state_of(fd).levels & bit) != 0, "write high, pin reads high");

	// release
	check(release(fd, pin) == B_OK, "release");
	state = state_of(fd);
	check(state.function[pin] == before.function[pin]
		&& state.pull[pin] == before.pull[pin] && (state.claimed & bit) == 0,
		"released: back to %s, pull %s", function_name(state.function[pin]),
		kPullNames[state.pull[pin] & 3]);

	// closing gives the pin back
	check(claim(other, pin, RPI_GPIO_OUTPUT, RPI_GPIO_PULL_NONE, 1) == B_OK,
		"the second descriptor claims the pin now");
	close(other);
	state = state_of(fd);
	check(state.function[pin] == before.function[pin]
		&& state.pull[pin] == before.pull[pin]
		&& (state.claimed_elsewhere & bit) == 0, "closing it restores the pin");

	close(fd);
	printf("%s: %d failure%s\n", sFailures == 0 ? "PASSED" : "FAILED",
		sFailures, sFailures == 1 ? "" : "s");
	return sFailures == 0 ? 0 : 1;
}


int
main(int argc, char** argv)
{
	if (argc >= 2 && strcmp(argv[1], "info") == 0)
		return do_info();
	if (argc >= 2 && strcmp(argv[1], "state") == 0)
		return do_state();
	if (argc >= 2 && strcmp(argv[1], "set") == 0)
		return do_set(argc, argv);
	if (argc >= 2 && strcmp(argv[1], "watch") == 0)
		return do_watch(argc, argv);
	if (argc >= 2 && strcmp(argv[1], "selftest") == 0)
		return do_selftest(argc, argv);

	fprintf(stderr, "usage: %s info | state | set <pin> in [up|down|none] |"
		" set <pin> out <0|1> | watch <pin> [up|down|none] [seconds] |"
		" selftest [pin]\n", argv[0]);
	return 1;
}
