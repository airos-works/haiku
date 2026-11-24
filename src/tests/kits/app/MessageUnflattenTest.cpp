/*
 * Copyright 2026, air/OS.
 * Distributed under the terms of the MIT License.
 */


/*!	BMessage::Unflatten() of damaged flattened messages: a hash table size
	that is not the table's, hash entries and field chains that leave the
	fields or loop, offsets and item sizes that leave the data, a header that
	names an area. Each has to be refused (or leave a message that can be
	searched and printed) without a crash, a hang, or an area of this team
	deleted. Prints what it checked; exit status 0 when all passed.

	message_unflatten_test            runs every case
	message_unflatten_test list       lists the cases
	message_unflatten_test "<case>"   runs the named cases (a case that
	                                  crashes ends the run)
*/


#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <DataIO.h>
#include <Messenger.h>
#include <OS.h>

#define private public
#include <Message.h>
#undef private

#include <MessagePrivate.h>


static int sFailures = 0;
static const char* sCurrent = "";
static bigtime_t sDeadline = 0;
static int sArgCount = 0;
static char** sArgs = NULL;


static void
crash_handler(int signal)
{
	// never the crash dialog: it waits for somebody
	char text[160];
	int length = snprintf(text, sizeof(text), "FAIL %s: signal %d (%s)\n",
		sCurrent, signal, strsignal(signal));
	write(STDOUT_FILENO, text, length);
	_exit(1);
}


static status_t
watchdog(void*)
{
	while (true) {
		snooze(100000);
		if (sDeadline != 0 && system_time() > sDeadline) {
			printf("FAIL %s: still running after 10 s (an endless loop)\n",
				sCurrent);
			fflush(stdout);
			_exit(1);
		}
	}
	return B_OK;
}


/*!	Whether the case \a name is to be run: all of them are, unless the
	command line names some ("list" prints the names).
*/
static bool
start(const char* name)
{
	if (sArgCount > 1 && strcmp(sArgs[1], "list") == 0) {
		printf("%s\n", name);
		return false;
	}
	bool selected = sArgCount < 2;
	for (int i = 1; i < sArgCount; i++)
		selected |= strcmp(sArgs[i], name) == 0;
	if (!selected)
		return false;

	sCurrent = name;
	sDeadline = system_time() + 10 * 1000000LL;
	return true;
}


static void
check(bool ok, const char* text)
{
	printf("%s %s: %s\n", ok ? "ok  " : "FAIL", sCurrent, text);
	if (!ok)
		sFailures++;
}


//!	Uses a message the way a program that got it would.
static void
use(BMessage& message)
{
	int32 value;
	const void* data;
	ssize_t size;
	message.FindInt32("number", &value);
	message.FindInt32("no such field", &value);
	message.FindData("blob", B_RAW_TYPE, 1, &data, &size);
	message.FindString("text", 2);
	message.HasInt32("zzz");
	char* name;
	type_code type;
	int32 count;
	for (int32 i = 0; message.GetInfo(B_ANY_TYPE, i, &name, &type, &count)
			== B_OK && i < 100; i++) {
		for (int32 j = 0; j < count && j < 100; j++) {
			if (message.FindData(name, type, j, &data, &size) == B_OK
				&& size > 0) {
				// touch the first and the last byte
				volatile uint8 byte = ((const uint8*)data)[0];
				byte = ((const uint8*)data)[size - 1];
				(void)byte;
			}
		}
	}
	BMessage copy(message);
	copy.AddInt32("more", 1);
	copy.RemoveName("number");
	BMessage inner;
	message.FindMessage("inner", &inner);
}


struct Flat {
	Flat()
	{
		BMessage message('test');
		message.AddInt32("number", 42);
		message.AddData("blob", B_RAW_TYPE, "0123456789", 10, false);
		message.AddData("blob", B_RAW_TYPE, "abc", 3, false);
		message.AddString("text", "one");
		message.AddString("text", "two");
		message.AddString("text", "three");
		BMessage inner('innr');
		inner.AddInt32("x", 1);
		message.AddMessage("inner", &inner);
		size = message.FlattenedSize();
		buffer = (char*)malloc(size);
		message.Flatten(buffer, size);
		header = (BMessage::message_header*)buffer;
		fields = (BMessage::field_header*)(buffer
			+ sizeof(BMessage::message_header));
		data = (uint8*)(fields + header->field_count);
	}

	~Flat()
	{
		free(buffer);
	}

	BMessage::field_header* Field(const char* name)
	{
		for (uint32 i = 0; i < header->field_count; i++) {
			if (strcmp((const char*)data + fields[i].offset, name) == 0)
				return &fields[i];
		}
		return NULL;
	}

	char*						buffer;
	ssize_t						size;
	BMessage::message_header*	header;
	BMessage::field_header*		fields;
	uint8*						data;
};


/*!	Unflattens \a flat both ways and uses the result. \a mustFail: the
	message has to be refused (otherwise it only has to be usable).
*/
static void
unflatten(Flat& flat, bool mustFail = true)
{
	{
		BMessage message;
		BMemoryIO stream(flat.buffer, flat.size);
		status_t status = message.Unflatten(&stream);
		use(message);
		char text[128];
		snprintf(text, sizeof(text), "Unflatten(BDataIO*) returned \"%s\"",
			strerror(status));
		check(!mustFail || status != B_OK, text);
		if (status != B_OK)
			check(message.IsEmpty(), "the refused message is empty");
	}
	// Unflatten(const char*) has no length: it reads what the header says
	// is there. A header that counts more than the buffer holds is only
	// refused when the length is known (above).
	uint64 claimed = sizeof(BMessage::message_header)
		+ (uint64)flat.header->field_count * sizeof(BMessage::field_header)
		+ flat.header->data_size;
	if (claimed <= (uint64)flat.size) {
		BMessage message;
		status_t status = message.Unflatten(flat.buffer);
		use(message);
		char text[128];
		snprintf(text, sizeof(text), "Unflatten(const char*) returned \"%s\"",
			strerror(status));
		check(!mustFail || status != B_OK, text);
	}
}


int
main(int argc, char** argv)
{
	sArgCount = argc;
	sArgs = argv;
	setvbuf(stdout, NULL, _IONBF, 0);
	signal(SIGSEGV, crash_handler);
	signal(SIGBUS, crash_handler);
	signal(SIGFPE, crash_handler);
	signal(SIGABRT, crash_handler);
	resume_thread(spawn_thread(watchdog, "watchdog", B_NORMAL_PRIORITY, NULL));

	if (start("intact message")) {
		Flat flat;
		BMessage message;
		BMemoryIO stream(flat.buffer, flat.size);
		status_t status = message.Unflatten(&stream);
		int32 number = 0;
		const char* text = NULL;
		BMessage inner;
		check(status == B_OK && message.FindInt32("number", &number) == B_OK
			&& number == 42 && message.FindString("text", 2, &text) == B_OK
			&& strcmp(text, "three") == 0
			&& message.FindMessage("inner", &inner) == B_OK
			&& inner.what == 'innr' && inner.GetInt32("x", 0) == 1,
			"unflattens with its fields, also the message in a field");
		BMessage second;
		check(second.Unflatten(flat.buffer) == B_OK
			&& second.GetInt32("number", 0) == 42,
			"unflattens from a buffer");
	}
	if (start("hash table size 0")) {
		Flat flat;
		flat.header->hash_table_size = 0;
		unflatten(flat);
	}
	if (start("hash table size 1000")) {
		Flat flat;
		flat.header->hash_table_size = 1000;
		unflatten(flat);
	}
	if (start("hash entry past the fields")) {
		Flat flat;
		for (int32 i = 0; i < MESSAGE_BODY_HASH_TABLE_SIZE; i++)
			flat.header->hash_table[i] = 0x1000000 + i;
		unflatten(flat);
	}
	if (start("hash entry equal to the field count")) {
		Flat flat;
		for (int32 i = 0; i < MESSAGE_BODY_HASH_TABLE_SIZE; i++)
			flat.header->hash_table[i] = flat.header->field_count;
		unflatten(flat);
	}
	if (start("negative hash entry")) {
		Flat flat;
		for (int32 i = 0; i < MESSAGE_BODY_HASH_TABLE_SIZE; i++)
			flat.header->hash_table[i] = -1000000;
		unflatten(flat);
	}
	if (start("next field equal to the field count")) {
		Flat flat;
		for (uint32 i = 0; i < flat.header->field_count; i++)
			flat.fields[i].next_field = flat.header->field_count;
		for (int32 i = 0; i < MESSAGE_BODY_HASH_TABLE_SIZE; i++)
			flat.header->hash_table[i] = 0;
		unflatten(flat);
	}
	if (start("field chain that loops")) {
		Flat flat;
		for (uint32 i = 0; i < flat.header->field_count; i++)
			flat.fields[i].next_field = i;
		for (int32 i = 0; i < MESSAGE_BODY_HASH_TABLE_SIZE; i++)
			flat.header->hash_table[i] = 0;
		unflatten(flat);
	}
	if (start("field offset that wraps in 32 bits")) {
		Flat flat;
		flat.fields[0].offset = 0xfffffff0;
		flat.fields[0].data_size = 0x18;
		unflatten(flat);
	}
	if (start("field data size past the data")) {
		Flat flat;
		flat.Field("blob")->data_size = flat.header->data_size;
		unflatten(flat);
	}
	if (start("item size past its field")) {
		Flat flat;
		BMessage::field_header* field = flat.Field("blob");
		uint32 huge = 0x7ffffff0;
		memcpy(flat.data + field->offset + field->name_length, &huge,
			sizeof(huge));
		unflatten(flat);
	}
	if (start("more items than the field holds")) {
		Flat flat;
		flat.Field("text")->count = 1000000;
		unflatten(flat);
	}
	if (start("field with no items")) {
		Flat flat;
		flat.Field("number")->count = 0;
		unflatten(flat);
	}
	if (start("field name without an end")) {
		Flat flat;
		BMessage::field_header* field = flat.Field("number");
		memset(flat.data + field->offset, 'x', field->name_length);
		unflatten(flat);
	}
	if (start("field count of 100 million")) {
		Flat flat;
		flat.header->field_count = 100 * 1000 * 1000;
		unflatten(flat);
	}
	if (start("data size of 4 GiB")) {
		Flat flat;
		flat.header->data_size = 0xfffffff0;
		unflatten(flat);
	}
	if (start("truncated")) {
		Flat flat;
		for (ssize_t size = 0; size < flat.size; size += 7) {
			BMessage message;
			BMemoryIO stream(flat.buffer, size);
			if (message.Unflatten(&stream) == B_OK) {
				check(false, "a truncated message was accepted");
				break;
			}
			use(message);
		}
		check(true, "every truncated copy is refused");
	}
	if (start("header that names an area of this team")) {
		// what Unflatten() does with a message that says its fields are in
		// an area: makes the area read-only and deletes it with the message
		void* address;
		area_id area = create_area("not a message's", &address,
			B_ANY_ADDRESS, 4 * B_PAGE_SIZE, B_NO_LOCK,
			B_READ_AREA | B_WRITE_AREA);
		memset(address, 0, 4 * B_PAGE_SIZE);
		Flat flat;
		flat.header->flags |= MESSAGE_FLAG_PASS_BY_AREA;
		flat.header->message_area = area;
		unflatten(flat);
		area_info info;
		bool exists = get_area_info(area, &info) == B_OK;
		check(exists, "the area still exists");
		check(exists && (info.protection & B_WRITE_AREA) != 0,
			"the area is still writable");
		if (exists)
			delete_area(area);
	}
	if (start("header that names an area, no fields")) {
		void* address;
		area_id area = create_area("not a message's", &address,
			B_ANY_ADDRESS, B_PAGE_SIZE, B_NO_LOCK,
			B_READ_AREA | B_WRITE_AREA);
		BMessage empty('test');
		char buffer[sizeof(BMessage::message_header)];
		empty.Flatten(buffer, sizeof(buffer));
		BMessage::message_header* header = (BMessage::message_header*)buffer;
		header->flags |= MESSAGE_FLAG_PASS_BY_AREA;
		header->message_area = area;
		{
			BMessage message;
			message.Unflatten(buffer);
		}
		area_info info;
		bool exists = get_area_info(area, &info) == B_OK;
		check(exists, "the area still exists");
		if (exists)
			delete_area(area);
	}
	if (start("header that names a message area that is too small")) {
		// the name of a message's area, with room for less than the header
		// counts
		void* address;
		area_id area = create_area("BMessage data", &address, B_ANY_ADDRESS,
			B_PAGE_SIZE, B_NO_LOCK, B_READ_AREA | B_WRITE_AREA);
		memset(address, 0, B_PAGE_SIZE);
		Flat flat;
		flat.header->flags |= MESSAGE_FLAG_PASS_BY_AREA;
		flat.header->message_area = area;
		flat.header->field_count = 1000;
		flat.header->data_size = 1024 * 1024;
		{
			BMessage message;
			status_t status = message.Unflatten(flat.buffer);
			use(message);
			check(status != B_OK, "refused");
		}
		area_info info;
		if (get_area_info(area, &info) == B_OK)
			delete_area(area);
	}
	if (start("message sent by area")) {
		// the real thing still works: a message large enough to go by area,
		// to a looper of this team's through its port
		BMessage large('larg');
		char* blob = (char*)malloc(256 * 1024);
		memset(blob, 'q', 256 * 1024);
		large.AddData("blob", B_RAW_TYPE, blob, 256 * 1024);
		large.AddInt32("number", 7);

		port_id port = create_port(10, "by area test");
		BMessenger noReply;
		BMessage::Private(large).SendMessage(port, getpid(), 0, 1000000,
			false, noReply);
		ssize_t size = port_buffer_size_etc(port, B_RELATIVE_TIMEOUT,
			2000000);
		bool ok = false;
		if (size > 0) {
			char* buffer = (char*)malloc(size);
			int32 code;
			read_port(port, &code, buffer, size);
			BMessage::message_header* header
				= (BMessage::message_header*)buffer;
			bool byArea = (header->flags & MESSAGE_FLAG_PASS_BY_AREA) != 0;
			BMessage received;
			status_t status = received.Unflatten(buffer);
			const void* data;
			ssize_t dataSize = 0;
			ok = status == B_OK && byArea
				&& received.GetInt32("number", 0) == 7
				&& received.FindData("blob", B_RAW_TYPE, &data, &dataSize)
					== B_OK
				&& dataSize == 256 * 1024
				&& memcmp(data, blob, dataSize) == 0;
			printf("     port message of %ld bytes, by area: %d, unflatten: "
				"%s\n", (long)size, byArea, strerror(status));
			free(buffer);
		}
		delete_port(port);
		free(blob);
		check(ok, "arrives with its data");
	}

	sDeadline = 0;
	if (argc > 1 && strcmp(argv[1], "list") == 0)
		return 0;
	printf("%s\n", sFailures == 0 ? "all checks passed" : "some checks FAILED");
	return sFailures == 0 ? 0 : 1;
}
