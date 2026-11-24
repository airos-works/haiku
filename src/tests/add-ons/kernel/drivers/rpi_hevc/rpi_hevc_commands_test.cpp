/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Tests the rpi_hevc driver's check of phase one's command list
	(check_phase1_commands(), get_phase1_needs(), #202).

	1. The rpi_hevc media add-on itself (HevcDecoder, built in here) decodes
	   the HEVC streams named on the command line against a fake device
	   that checks each request as the driver does: every command list it
	   makes must be let through, every phase two request too. (No pictures
	   come out of a fake device; the add-on does not look at them.)
	2. Lists made bad from a good one (the first real list, or one built
	   here as the add-on builds a picture of one slice) must be refused:
	   writes to the registers the driver sets (PU and coefficient bases
	   and strides, the list's own address and length), to phase two's
	   registers, a bit reader outside the bitstream, more rows than the
	   buffers hold, values of unknown meaning.

	Builds for Haiku (jam rpi_hevc_commands_test) and on any host:
	  W=<haiku>; P=$W/src/add-ons/media/plugins/rpi_hevc
	  g++ -std=c++17 -g -fsanitize=address,undefined \
	      -I$W/headers/os/kernel -I$W/headers/os/drivers \
	      -I$W/headers/os/support -I$W/headers/private/drivers \
	      -I$W/src/add-ons/kernel/drivers/misc -I$P \
	      rpi_hevc_commands_test.cpp $P/HevcParser.cpp $P/SandConvert.cpp \
	      -o rpi_hevc_commands_test
	  ./rpi_hevc_commands_test <stream.hevc>...
*/


#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <deque>
#include <map>
#include <string>
#include <vector>

#ifndef __HAIKU__
// what the headers under test and the add-on use of OS.h, String.h and
// Drivers.h (found, and left out by their guards)
#	define _OS_H
#	define _DRIVERS_DRIVERS_H
#	define _B_STRING_H
typedef unsigned char uint8;
typedef unsigned short uint16;
typedef unsigned int uint32;
typedef unsigned long long uint64;
typedef int int32;
typedef long long int64;
typedef int status_t;
typedef int32 area_id;
enum {
	B_OK = 0,
	B_ERROR = -1,
	B_NO_MEMORY = -2147483648 + 0,
	B_BAD_VALUE = -2147483648 + 5,
	B_TIMED_OUT = -2147483648 + 9,
	B_NO_INIT = -2147483648 + 0x6000 + 13
};
#	define B_DEVICE_OP_CODES_END 9999
#	define B_ANY_ADDRESS 1
#	define B_READ_AREA 1
#	define B_WRITE_AREA 2
#	define B_PRIu32 PRIu32
#	define B_PRId32 PRId32
#	define B_PRIx32 PRIx32
#	define B_PRId64 "lld"
#	define B_PRIuSIZE "zu"

class BString {
public:
	BString& operator=(const char* text)
	{
		fText = text != NULL ? text : "";
		return *this;
	}
	BString& SetToFormat(const char* format, ...)
	{
		char text[512];
		va_list args;
		va_start(args, format);
		vsnprintf(text, sizeof(text), format, args);
		va_end(args);
		fText = text;
		return *this;
	}
	const char* String() const { return fText.c_str(); }

private:
	std::string fText;
};
#else
#	include <OS.h>
#	include <String.h>
#endif

#include <rpi_hevc.h>
#include "rpi_hevc_commands.h"
#include "rpi_hevc_geometry.h"


static const uint64 kPageSize = 4096;

static int sFailures = 0;
static int sChecks = 0;

#define CHECK(condition, what...) \
	do { \
		sChecks++; \
		if (!(condition)) { \
			sFailures++; \
			fprintf(stderr, "%s:%d: %s failed: ", __FILE__, __LINE__, \
				#condition); \
			fprintf(stderr, what); \
			fputc('\n', stderr); \
		} \
	} while (0)


//	#pragma mark - the fake device


/*!	What the driver has of a buffer. Physical addresses are made up, with a
	page between buffers, so that an address outside one is in no other. */
struct FakeBuffer {
	uint8*	memory;
	uint64	size;
	uint64	physical;
};

/*!	A phase one request as the driver saw it, with what the check needs
	besides the list: kept to make bad lists from. */
struct Phase1Case {
	std::vector<rpi_hevc_command>	commands;
	rpi_hevc_phase1					request;
	uint64							bitstream;	// physical
	uint64							puSize;
	uint64							coeffSize;
	std::string						name;
};

static std::map<uint32, FakeBuffer> sBuffers;
static uint32 sNextBuffer = 0;
static uint64 sNextPhysical = 0x20000000;
static int sDevice = -1;

static const char* sStream = "";
static uint32 sPhase1Lists;
static uint64 sPhase1Commands;
static uint32 sPhase1MaxCount;
static uint32 sPhase1MaxRows;
static uint32 sPhase2Requests;
static uint32 sWavefrontLists;
static uint32 sMultiSliceLists;
static std::vector<Phase1Case> sCases;


static uint64
pages(uint64 size)
{
	return (size + kPageSize - 1) / kPageSize * kPageSize;
}


static FakeBuffer*
fake_buffer(uint32 id)
{
	std::map<uint32, FakeBuffer>::iterator found = sBuffers.find(id);
	return found != sBuffers.end() ? &found->second : NULL;
}


/*!	The driver's phase1() up to starting the block: the buffers exist,
	the list fits its buffer, and the copy of the list passes. */
static status_t
check_phase1_request(const rpi_hevc_command* list, const Phase1Case& context,
	hevc_phase1_check& check)
{
	const rpi_hevc_phase1& request = context.request;
	status_t status = check_phase1_commands(list, request.command_count,
		context.bitstream, request.bitstream_size, check);
	if (status != B_OK)
		return status;

	hevc_phase1_needs needs;
	if (get_phase1_needs(request, check.rows, needs) != B_OK) {
		check.reason = "stride";
		return B_BAD_VALUE;
	}
	if (context.puSize < needs.pu || context.coeffSize < needs.coeff) {
		check.reason = "rows do not fit the buffers";
		return B_BAD_VALUE;
	}
	return B_OK;
}


static bool
is_wavefront(const rpi_hevc_command* list, uint32 count)
{
	for (uint32 i = 0; i < count; i++) {
		if (list[i].address == HEVC_P1_MODE && (list[i].data & 0xffff) == 1)
			return true;
	}
	return false;
}


static status_t
fake_phase1(rpi_hevc_phase1& request)
{
	FakeBuffer* commands = fake_buffer(request.commands);
	FakeBuffer* bitstream = fake_buffer(request.bitstream);
	FakeBuffer* pu = fake_buffer(request.pu);
	FakeBuffer* coeff = fake_buffer(request.coeff);
	if (commands == NULL || bitstream == NULL || pu == NULL || coeff == NULL
		|| request.command_count == 0
		|| request.command_count > RPI_HEVC_MAX_COMMANDS
		|| (uint64)request.command_count * sizeof(rpi_hevc_command)
			> commands->size
		|| request.bitstream_size > bitstream->size) {
		CHECK(false, "%s: phase 1 request refused before its list", sStream);
		return B_BAD_VALUE;
	}

	// what the driver does: a copy, checked
	Phase1Case context;
	const rpi_hevc_command* list = (const rpi_hevc_command*)commands->memory;
	context.commands.assign(list, list + request.command_count);
	context.request = request;
	context.bitstream = bitstream->physical;
	context.puSize = pu->size;
	context.coeffSize = coeff->size;

	hevc_phase1_check check;
	status_t status = check_phase1_request(context.commands.data(), context,
		check);
	CHECK(status == B_OK, "%s: list %" B_PRIu32 " refused at command %"
		B_PRIu32 " of %" B_PRIu32 " (%#" B_PRIx32 " = %#" B_PRIx32 "): %s",
		sStream, sPhase1Lists, check.entry, request.command_count,
		context.commands[check.entry].address,
		context.commands[check.entry].data, check.reason);
	if (status != B_OK)
		return status;

	sPhase1Lists++;
	sPhase1Commands += request.command_count;
	sPhase1MaxCount = std::max(sPhase1MaxCount, request.command_count);
	sPhase1MaxRows = std::max(sPhase1MaxRows, check.rows);

	uint32 slices = 0;
	for (const rpi_hevc_command& command : context.commands) {
		if (command.address == HEVC_P1_BIT_CONTROL
			&& (command.data & HEVC_P1_BIT_STOP) == 0) {
			slices++;
		}
	}
	bool wavefront = is_wavefront(list, request.command_count);
	if (wavefront)
		sWavefrontLists++;
	if (slices > 1)
		sMultiSliceLists++;

	// the first list of each stream, of its first wavefront and multiple
	// slice picture, to make bad ones of
	bool keep = sCases.empty() || sCases.back().name.find(sStream) != 0;
	if (!keep && wavefront) {
		keep = true;
		for (const Phase1Case& other : sCases) {
			if (other.name.find(sStream) == 0
				&& is_wavefront(other.commands.data(),
					other.commands.size())) {
				keep = false;
			}
		}
	}
	if (keep) {
		context.name = std::string(sStream) + (wavefront ? " (wavefront)" : "");
		sCases.push_back(context);
	}

	request.result = RPI_HEVC_PHASE1_OK;
	request.status = 0;
	return B_OK;
}


/*!	The driver's phase2() checks. */
static status_t
fake_phase2(const rpi_hevc_phase2& request)
{
	FakeBuffer* pu = fake_buffer(request.pu);
	FakeBuffer* coeff = fake_buffer(request.coeff);
	FakeBuffer* frame = fake_buffer(request.frame);
	hevc_phase2_needs needs;
	bool ok = pu != NULL && coeff != NULL && frame != NULL
		&& get_phase2_needs(request, needs) == B_OK
		&& pu->size >= needs.pu && coeff->size >= needs.coeff
		&& frame->size >= needs.picture;
	for (int32 i = 0; ok && i < 16; i++) {
		if (request.references[i] == RPI_HEVC_NO_BUFFER)
			continue;
		FakeBuffer* reference = fake_buffer(request.references[i]);
		ok = reference != NULL && reference->size >= needs.picture;
	}
	if (ok && request.mv != RPI_HEVC_NO_BUFFER) {
		FakeBuffer* mv = fake_buffer(request.mv);
		ok = mv != NULL && mv->size >= needs.mv;
	}
	if (ok && request.collocated != RPI_HEVC_NO_BUFFER) {
		FakeBuffer* collocated = fake_buffer(request.collocated);
		ok = collocated != NULL && collocated->size >= needs.mv;
	}
	CHECK(ok, "%s: phase 2 request %" B_PRIu32 " refused (%" B_PRIu32 "x%"
		B_PRIu32 ")", sStream, sPhase2Requests,
		request.frame_size & 0xffff, request.frame_size >> 16);
	if (!ok)
		return B_BAD_VALUE;
	sPhase2Requests++;
	return B_OK;
}


static int
fake_open(const char* path, int flags)
{
	if (strcmp(path, RPI_HEVC_DEVICE_PATH) != 0)
		return open(path, flags);
	sDevice = open("/dev/null", O_RDWR);
	return sDevice;
}


/*!	The driver frees what was allocated with the file. */
static int
fake_close(int fd)
{
	if (fd == sDevice) {
		for (std::map<uint32, FakeBuffer>::iterator iterator
				= sBuffers.begin(); iterator != sBuffers.end(); iterator++) {
			free(iterator->second.memory);
		}
		sBuffers.clear();
		sDevice = -1;
	}
	return close(fd);
}


static int
fake_ioctl(int fd, unsigned long op, void* argument, size_t size)
{
	status_t status = B_OK;
	if (fd != sDevice)
		status = B_BAD_VALUE;
	else if (op == RPI_HEVC_ALLOCATE && size == sizeof(rpi_hevc_allocate)) {
		rpi_hevc_allocate& request = *(rpi_hevc_allocate*)argument;
		FakeBuffer buffer;
		buffer.size = pages(request.size);
		buffer.memory = (uint8*)calloc(1, buffer.size);
		buffer.physical = sNextPhysical;
		sNextPhysical += buffer.size + kPageSize;
		uint32 id = sNextBuffer++;
		sBuffers[id] = buffer;
		request.size = buffer.size;
		request.buffer = id;
		request.area = (area_id)id + 1;
		request.physical = buffer.physical;
	} else if (op == RPI_HEVC_FREE && size == sizeof(uint32)) {
		uint32 id = *(uint32*)argument;
		FakeBuffer* buffer = fake_buffer(id);
		if (buffer == NULL)
			status = B_BAD_VALUE;
		else {
			free(buffer->memory);
			sBuffers.erase(id);
		}
	} else if (op == RPI_HEVC_PHASE1 && size == sizeof(rpi_hevc_phase1))
		status = fake_phase1(*(rpi_hevc_phase1*)argument);
	else if (op == RPI_HEVC_PHASE2 && size == sizeof(rpi_hevc_phase2))
		status = fake_phase2(*(const rpi_hevc_phase2*)argument);
	else
		status = B_BAD_VALUE;

	if (status != B_OK) {
		errno = status;
		return -1;
	}
	return 0;
}


static area_id
fake_clone_area(const char* name, void** _address, uint32 addressSpec,
	uint32 protection, area_id source)
{
	FakeBuffer* buffer = fake_buffer((uint32)source - 1);
	if (buffer == NULL)
		return B_BAD_VALUE;
	*_address = buffer->memory;
	return source;
}


static status_t
fake_delete_area(area_id area)
{
	// the memory goes with RPI_HEVC_FREE
	return B_OK;
}


// The add-on, with the device and its areas replaced by the fake.
#define open(path, flags)	fake_open(path, flags)
#define close(fd)			fake_close(fd)
#define ioctl(fd, op, argument, size) \
	fake_ioctl(fd, op, argument, size)
#define clone_area			fake_clone_area
#define delete_area			fake_delete_area
#include "HevcDecoder.cpp"
#undef open
#undef close
#undef ioctl
#undef clone_area
#undef delete_area


//	#pragma mark - streams


static bool
decode_stream(const char* path)
{
	FILE* input = fopen(path, "rb");
	if (input == NULL) {
		perror(path);
		return false;
	}
	std::vector<uint8> stream;
	uint8 block[65536];
	size_t got;
	while ((got = fread(block, 1, sizeof(block), input)) > 0)
		stream.insert(stream.end(), block, block + got);
	fclose(input);

	const char* name = strrchr(path, '/');
	sStream = name != NULL ? name + 1 : path;
	uint32 listsBefore = sPhase1Lists;
	uint32 picturesBefore = sPhase2Requests;

	HevcDecoder decoder;
	BString error;
	if (decoder.Open(&error) != B_OK) {
		CHECK(false, "%s: %s", sStream, error.String());
		return false;
	}

	// NAL units between start codes, as tools/rpi4/hevc_decode.cpp
	size_t size = stream.size();
	size_t position = 0;
	int64 count = 0;
	HevcDecoder::Picture picture;
	while (position + 3 <= size) {
		size_t begin = position;
		while (begin + 3 <= size && !(stream[begin] == 0
				&& stream[begin + 1] == 0 && stream[begin + 2] == 1)) {
			begin++;
		}
		if (begin + 3 > size)
			break;
		begin += 3;
		size_t end = begin;
		while (end + 3 <= size && !(stream[end] == 0 && stream[end + 1] == 0
				&& (stream[end + 2] == 1 || stream[end + 2] == 0))) {
			end++;
		}
		if (end + 3 > size)
			end = size;

		status_t status = decoder.PutNal(&stream[begin], end - begin,
			count++);
		CHECK(status == B_OK, "%s: NAL unit %" B_PRId64 ": %s", sStream,
			count - 1, decoder.Error());
		if (status != B_OK)
			return false;
		while (decoder.NextPicture(picture))
			decoder.ReleasePicture(picture);
		position = end;
	}
	decoder.Drain();
	while (decoder.NextPicture(picture))
		decoder.ReleasePicture(picture);

	uint32 lists = sPhase1Lists - listsBefore;
	printf("%-24s %4" B_PRIu32 " lists, %4" B_PRIu32 " pictures through "
		"phase 2\n", sStream, lists, sPhase2Requests - picturesBefore);
	CHECK(lists > 0, "%s: no command list", sStream);
	return true;
}


//	#pragma mark - bad lists


/*!	A list for a picture of one slice, as the add-on writes it (tile mode,
	no tiles): \a rows rows of coding tree blocks, \a columns wide. */
static Phase1Case
built_case(uint32 columns, uint32 rows)
{
	Phase1Case context;
	context.name = "built";
	context.bitstream = 0x30000000;
	context.request = rpi_hevc_phase1();
	context.request.bitstream_size = 5000;
	context.request.pu_stride = 4096;
	context.request.coeff_stride = 16384;
	context.puSize = pages(rows * 4096);
	context.coeffSize = pages(rows * 16384);

	std::vector<rpi_hevc_command>& list = context.commands;
	auto write = [&list](uint32 address, uint32 data) {
		rpi_hevc_command command = { address, data };
		list.push_back(command);
	};

	// the slice data starts 37 bytes in
	uint64 data = context.bitstream + 37;
	write(HEVC_P1_BIT_BASE, (uint32)(data >> 6));
	write(HEVC_P1_BIT_COUNT, 4900);
	write(HEVC_P1_BIT_CONTROL, (data & 63) | HEVC_P1_BIT_STOP);
	write(HEVC_P1_BIT_CONTROL, (data & 63) | (1 << 6));
	for (uint32 i = 0; i < HEVC_P1_PROBABILITY_BYTES; i += 4)
		write(HEVC_P1_PROBABILITIES + i, 0x7c7c7c7c);
	write(HEVC_P1_TRANSFER, HEVC_P1_TRANSFER_SAVE);
	write(HEVC_P1_SLICE_COMMANDS, 3);
	for (uint32 i = 0; i < 3; i++)
		write(HEVC_P1_SLICE_MESSAGES + 4 * i, 0x4001 + i);
	write(HEVC_P1_SPS0, 0x00885453);
	write(HEVC_P1_SPS1, 0x00010000);
	write(HEVC_P1_PPS, 0x6);
	write(HEVC_P1_SLICE_START, 0);
	write(HEVC_P1_TILE_START, 0);
	write(HEVC_P1_TILE_END, (columns - 1) | ((rows - 1) << 16));
	write(HEVC_P1_BEGIN_TILE_END, (columns - 1) | ((rows - 1) << 16));
	write(HEVC_P1_SLICE, 0x40802000);
	write(HEVC_P1_QP, 32);
	write(HEVC_P1_MODE, 0xffff | (1 << 17) | (1 << 18));
	write(HEVC_P1_CONTROL, 0);
	write(HEVC_P1_STATUS, 1 | ((columns - 1) << 5) | ((rows - 1) << 18));

	context.request.command_count = list.size();
	return context;
}


static status_t
check_case(const Phase1Case& context, hevc_phase1_check& check)
{
	return check_phase1_request(context.commands.data(), context, check);
}


static int32
find_command(const Phase1Case& context, uint32 address, uint32 skip = 0)
{
	for (size_t i = 0; i < context.commands.size(); i++) {
		if (context.commands[i].address == address && skip-- == 0)
			return (int32)i;
	}
	return -1;
}


static int32
find_start(const Phase1Case& context)
{
	for (size_t i = 0; i < context.commands.size(); i++) {
		const rpi_hevc_command& command = context.commands[i];
		if (command.address == HEVC_P1_BIT_CONTROL
			&& (command.data & HEVC_P1_BIT_STOP) == 0) {
			return (int32)i;
		}
	}
	return -1;
}


static void
expect_refused(const Phase1Case& context, const char* what)
{
	hevc_phase1_check check;
	status_t status = check_case(context, check);
	CHECK(status == B_BAD_VALUE, "%s: %s was let through",
		context.name.c_str(), what);
}


static void
expect_accepted(const Phase1Case& context, const char* what)
{
	hevc_phase1_check check;
	status_t status = check_case(context, check);
	CHECK(status == B_OK, "%s: %s refused at %" B_PRIu32 ": %s",
		context.name.c_str(), what, check.entry,
		check.reason != NULL ? check.reason : "");
}


/*!	\a context with a write added before its first start of decoding. */
static Phase1Case
with_inserted(const Phase1Case& context, uint32 address, uint32 data)
{
	Phase1Case bad = context;
	int32 at = find_command(context, HEVC_P1_CONTROL);
	if (at < 0)
		at = 0;
	rpi_hevc_command command = { address, data };
	bad.commands.insert(bad.commands.begin() + at, command);
	bad.request.command_count = bad.commands.size();
	return bad;
}


static void
test_bad_lists(const Phase1Case& good)
{
	hevc_phase1_check check;
	status_t status = check_case(good, check);
	CHECK(status == B_OK, "%s: the good list was refused at %" B_PRIu32
		": %s", good.name.c_str(), check.entry,
		check.reason != NULL ? check.reason : "");
	if (status != B_OK)
		return;
	uint32 rows = check.rows;

	// the registers the driver sets, and others the list must not name
	static const struct {
		uint32		address;
		const char*	name;
	} kForbidden[] = {
		{ 32, "LEFT0" }, { 44, "LEFT3" }, { 60, "VERSION" },
		{ 76, "BIT_STATUS" }, { 80, "PU_WRITE_BASE" },
		{ 84, "PU_WRITE_STRIDE" }, { 88, "COEFF_WRITE_BASE" },
		{ 92, "COEFF_WRITE_STRIDE" }, { 108, "COMMAND_BASE" },
		{ 112, "COMMAND_COUNT" }, { 116, "COMMAND_STATUS" },
		{ 120, "past the registers" }, { 0x800, "unknown" },
		{ 0x8000, "PU_READ_BASE" }, { 0x8010, "ROWS" },
		{ 0x8018, "OUT_Y_BASE" }, { 0x8030, "MV_BASE" },
		{ 0x9000, "REFERENCE(0)" },
		{ 0x10000 | HEVC_P1_BIT_BASE, "BIT_BASE with bit 16" },
		{ 0x80000000u | HEVC_P1_SPS0, "SPS0 with bit 31" },
		{ HEVC_P1_PROBABILITIES + HEVC_P1_PROBABILITY_BYTES,
			"a probability past the table" },
		{ HEVC_P1_SCALING_FACTORS + HEVC_P1_SCALING_FACTOR_BYTES,
			"a scaling factor past the table" },
		{ HEVC_P1_SLICE_MESSAGES + 4 * HEVC_P1_SLICE_MESSAGE_COUNT,
			"a slice message past the table" },
		{ HEVC_P1_PROBABILITIES + 2, "a probability not on a word" },
		{ HEVC_P1_SPS1 + 1, "a register not on a word" },
	};
	for (size_t i = 0; i < sizeof(kForbidden) / sizeof(kForbidden[0]); i++) {
		char what[96];
		snprintf(what, sizeof(what), "a write to %s (%#" B_PRIx32 ")",
			kForbidden[i].name, kForbidden[i].address);
		expect_refused(with_inserted(good, kForbidden[i].address, 0), what);
	}
	// the last entries of the tables are fine
	expect_accepted(with_inserted(good, HEVC_P1_PROBABILITIES
		+ HEVC_P1_PROBABILITY_BYTES - 4, 0), "the last probability");
	expect_accepted(with_inserted(good, HEVC_P1_SCALING_FACTORS
		+ HEVC_P1_SCALING_FACTOR_BYTES - 4, 0), "the last scaling factor");
	expect_accepted(with_inserted(good, HEVC_P1_SLICE_MESSAGES
		+ 4 * (HEVC_P1_SLICE_MESSAGE_COUNT - 1), 0xffff),
		"the last slice message");
	expect_refused(with_inserted(good, HEVC_P1_SLICE_MESSAGES, 0x10000),
		"a slice message of 17 bits");

	// values whose meaning is not known
	expect_refused(with_inserted(good, HEVC_P1_TRANSFER, 0),
		"another transfer");
	expect_refused(with_inserted(good, HEVC_P1_TRANSFER,
		HEVC_P1_TRANSFER_SAVE | 1), "another transfer");
	expect_refused(with_inserted(good, HEVC_P1_MODE, 2), "pause mode 2");
	expect_refused(with_inserted(good, HEVC_P1_MODE, 1 << 19),
		"mode bit 19");
	expect_accepted(with_inserted(good, HEVC_P1_MODE, 0x70000),
		"the wavefront pause mode");
	expect_refused(with_inserted(good, HEVC_P1_STATUS, 3), "status code 3");
	expect_refused(with_inserted(good, HEVC_P1_STATUS, 0), "status code 0");
	expect_refused(with_inserted(good, HEVC_P1_BIT_CONTROL, 0x100),
		"bit control bit 8");

	// the bit reader
	int32 base = find_command(good, HEVC_P1_BIT_BASE);
	int32 bitCount = find_command(good, HEVC_P1_BIT_COUNT);
	int32 start = find_start(good);
	CHECK(base >= 0 && bitCount >= 0 && start > base && start > bitCount,
		"%s: no bit reader", good.name.c_str());
	if (base >= 0 && bitCount >= 0 && start > base && start > bitCount) {
		uint64 end = good.bitstream + good.request.bitstream_size;
		uint64 first = ((uint64)good.commands[base].data << 6)
			+ (good.commands[start].data & 63);
		uint32 count = good.commands[bitCount].data;

		Phase1Case bad = good;
		bad.commands[base].data = (uint32)((good.bitstream - 64) >> 6);
		expect_refused(bad, "a bitstream address before the bitstream");
		bad.commands[base].data = (uint32)(end >> 6) + 1;
		expect_refused(bad, "a bitstream address after the bitstream");
		bad.commands[base].data = 0x20000000 >> 6;
		expect_refused(bad, "a bitstream address in another buffer");
		bad.commands[base].data = 0;
		expect_refused(bad, "bitstream address 0");

		bad = good;
		bad.commands[bitCount].data = (uint32)(end - first) + 1;
		expect_refused(bad, "a bit count one byte past the bitstream");
		bad.commands[bitCount].data = (uint32)(end - first);
		expect_accepted(bad, "a bit count to the bitstream's end");
		bad.commands[bitCount].data = 0xffffffff;
		expect_refused(bad, "a bit count of 4 GB");

		// the start's offset moves the bytes read
		bad = good;
		bad.commands[bitCount].data = (uint32)(end - first);
		bad.commands[start].data = (bad.commands[start].data & ~63u)
			| ((first & 63) + 1);
		if ((first & 63) < 63)
			expect_refused(bad, "a start one byte later at the end");

		// a start without its own address or count: the registers hold
		// another program's
		bad = good;
		bad.commands[base].address = HEVC_P1_QP;
		expect_refused(bad, "a start without a bitstream address");
		bad = good;
		bad.commands[bitCount].address = HEVC_P1_QP;
		expect_refused(bad, "a start without a bit count");

		// a stop does not read
		bad = good;
		bad.commands[base].address = HEVC_P1_QP;
		bad.commands[start].data |= HEVC_P1_BIT_STOP;
		bool otherStart = false;
		for (size_t i = start + 1; i < bad.commands.size(); i++) {
			if (bad.commands[i].address == HEVC_P1_BIT_CONTROL
				&& (bad.commands[i].data & HEVC_P1_BIT_STOP) == 0) {
				otherStart = true;
			}
		}
		if (!otherStart)
			expect_accepted(bad, "a stop without a bitstream address");

		// a bitstream buffer of nothing
		bad = good;
		bad.request.bitstream_size = 0;
		expect_refused(bad, "a bit reader with no bitstream in use");
		(void)count;
	}

	// rows: as many as the buffers hold, not one more
	uint32 puRows = good.puSize / hevc_round_up(good.request.pu_stride, 64);
	uint32 coeffRows = good.coeffSize
		/ hevc_round_up(good.request.coeff_stride, 64);
	uint32 capacity = std::min(puRows, coeffRows);
	CHECK(rows <= capacity, "%s: %" B_PRIu32 " rows, room for %" B_PRIu32,
		good.name.c_str(), rows, capacity);
	int32 control = find_command(good, HEVC_P1_CONTROL);
	int32 tileEnd = find_command(good, HEVC_P1_TILE_END);
	int32 status1 = find_command(good, HEVC_P1_STATUS);
	CHECK(control >= 0 && tileEnd >= 0, "%s: no tile", good.name.c_str());
	if (control >= 0 && tileEnd >= 0) {
		Phase1Case bad = good;
		bad.commands[control].data = (bad.commands[control].data & 0xffff)
			| (capacity << 16);
		expect_refused(bad, "decoding started below the buffers' rows");
		bad.commands[control].data = (bad.commands[control].data & 0xffff)
			| ((capacity - 1) << 16);
		expect_accepted(bad, "decoding started in the buffers' last row");

		bad = good;
		bad.commands[tileEnd].data = (bad.commands[tileEnd].data & 0xffff)
			| (capacity << 16);
		expect_refused(bad, "a tile ending below the buffers' rows");
		bad.commands[tileEnd].data = (bad.commands[tileEnd].data & 0xffff)
			| 0xffff0000;
		expect_refused(bad, "a tile ending in row 65535");

		if (status1 >= 0) {
			bad = good;
			bad.commands[status1].data = (bad.commands[status1].data
				& ((1 << 18) - 1)) | (capacity << 18);
			expect_refused(bad, "a status below the buffers' rows");
		}

		// a buffer one page short of the rows named
		bad = good;
		bad.puSize = (uint64)rows * hevc_round_up(good.request.pu_stride, 64)
			- kPageSize;
		expect_refused(bad, "a PU buffer a page short");
		bad = good;
		bad.coeffSize = (uint64)rows
			* hevc_round_up(good.request.coeff_stride, 64) - kPageSize;
		expect_refused(bad, "a coefficient buffer a page short");
		bad = good;
		bad.request.pu_stride = 0;
		expect_refused(bad, "a PU stride of nothing");
		bad = good;
		bad.request.coeff_stride = 0;
		expect_refused(bad, "a coefficient stride of nothing");
		// strides go in units of 64 bytes, up
		bad = good;
		bad.request.pu_stride = (uint32)(good.puSize / rows) + 1;
		if (rows > 0)
			expect_refused(bad, "a PU stride past the buffer");

		// decoding started before the tile is set: the tile registers
		// hold another program's
		bad = good;
		bad.commands[tileEnd].address = HEVC_P1_QP;
		if (find_command(bad, HEVC_P1_TILE_END) < 0
			|| find_command(bad, HEVC_P1_TILE_END) > control) {
			expect_refused(bad, "decoding without a tile end");
		}
	}

	// the number of commands
	Phase1Case bad = good;
	bad.request.command_count = 0;
	expect_refused(bad, "no commands");
	CHECK(check_phase1_commands(good.commands.data(),
			RPI_HEVC_MAX_COMMANDS + 1, good.bitstream,
			good.request.bitstream_size, check) == B_BAD_VALUE,
		"more than RPI_HEVC_MAX_COMMANDS commands");
}


int
main(int argc, char** argv)
{
	for (int i = 1; i < argc; i++)
		decode_stream(argv[i]);
	if (argc > 1) {
		printf("%" B_PRIu32 " command lists (%llu commands, at most %"
			B_PRIu32 " in one, %" B_PRIu32 " wavefront, %" B_PRIu32
			" with several slices, up to %" B_PRIu32 " rows) let through, %"
			B_PRIu32 " phase 2 requests\n", sPhase1Lists,
			(unsigned long long)sPhase1Commands, sPhase1MaxCount, sWavefrontLists,
			sMultiSliceLists, sPhase1MaxRows, sPhase2Requests);
	}

	test_bad_lists(built_case(30, 17));
	test_bad_lists(built_case(1, 1));
	for (const Phase1Case& context : sCases)
		test_bad_lists(context);
	printf("bad lists made from %zu good ones\n", sCases.size() + 2);

	printf("%d of %d checks passed\n", sChecks - sFailures, sChecks);
	return sFailures == 0 ? 0 : 1;
}
