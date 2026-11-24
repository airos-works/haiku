/*
 * Copyright 2026, the air/OS project.
 * Distributed under the terms of the MIT License.
 *
 * What a HID device can do to the shared report descriptor parser with its
 * descriptor and its reports: see run.sh. Each case is run in a process of
 * its own ("hid_parser_test <case>"), under AddressSanitizer; a case fails
 * by printing FAIL, by a sanitizer report or by not ending.
 */


#include "UserlandHID.h"

#include "HIDCollection.h"
#include "HIDParser.h"
#include "HIDReport.h"
#include "HIDReportItem.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <vector>


int gTraceCount;

static int sFailures;

#define CHECK(condition, message...) \
	do { \
		if (!(condition)) { \
			printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			printf(message); \
			printf("\n"); \
			sFailures++; \
		} \
	} while (0)


// #pragma mark - building descriptors


enum { MAIN = 0, GLOBAL = 1, LOCAL = 2 };
enum { INPUT = 0x8, OUTPUT = 0x9, COLLECTION = 0xa, FEATURE = 0xb,
	END_COLLECTION = 0xc };
enum { USAGE_PAGE = 0x0, LOGICAL_MINIMUM = 0x1, LOGICAL_MAXIMUM = 0x2,
	REPORT_SIZE = 0x7, REPORT_ID = 0x8, REPORT_COUNT = 0x9, PUSH = 0xa,
	POP = 0xb };
enum { USAGE = 0x0, USAGE_MINIMUM = 0x1, USAGE_MAXIMUM = 0x2 };

typedef std::vector<uint8> Descriptor;


static void
add_item(Descriptor &descriptor, int type, int tag, uint32 data,
	int bytes = -1)
{
	if (bytes < 0)
		bytes = data > 0xffff ? 4 : data > 0xff ? 2 : 1;

	descriptor.push_back((tag << 4) | (type << 2) | (bytes == 4 ? 3 : bytes));
	for (int i = 0; i < bytes; i++)
		descriptor.push_back((uint8)(data >> (8 * i)));
}


static void
add_mouse_start(Descriptor &descriptor)
{
	add_item(descriptor, GLOBAL, USAGE_PAGE, 0x01);		// generic desktop
	add_item(descriptor, LOCAL, USAGE, 0x02);			// mouse
	add_item(descriptor, MAIN, COLLECTION, 0x01);		// application
	add_item(descriptor, GLOBAL, USAGE_PAGE, 0x09);		// buttons
	add_item(descriptor, LOCAL, USAGE_MINIMUM, 1);
	add_item(descriptor, LOCAL, USAGE_MAXIMUM, 3);
	add_item(descriptor, GLOBAL, LOGICAL_MINIMUM, 0);
	add_item(descriptor, GLOBAL, LOGICAL_MAXIMUM, 1);
	add_item(descriptor, GLOBAL, REPORT_SIZE, 1);
	add_item(descriptor, GLOBAL, REPORT_COUNT, 3);
	add_item(descriptor, MAIN, INPUT, 0x02);			// data, variable
	add_item(descriptor, GLOBAL, REPORT_SIZE, 5);
	add_item(descriptor, GLOBAL, REPORT_COUNT, 1);
	add_item(descriptor, MAIN, INPUT, 0x01);			// constant
	add_item(descriptor, GLOBAL, USAGE_PAGE, 0x01);
	add_item(descriptor, GLOBAL, LOGICAL_MINIMUM, 0x81);	// -127
	add_item(descriptor, GLOBAL, LOGICAL_MAXIMUM, 0x7f);
}


static void
add_axis(Descriptor &descriptor, uint32 usage, uint32 bits)
{
	add_item(descriptor, LOCAL, USAGE, usage);
	add_item(descriptor, GLOBAL, REPORT_SIZE, bits);
	add_item(descriptor, GLOBAL, REPORT_COUNT, 1);
	add_item(descriptor, MAIN, INPUT, 0x06);		// data, variable, relative
}


/*!	The descriptor in a block of exactly its size, so that reading past its
	end is seen.
*/
static status_t
parse(HIDParser &parser, const Descriptor &descriptor)
{
	uint8 *copy = (uint8 *)malloc(descriptor.size());
	memcpy(copy, descriptor.data(), descriptor.size());
	status_t status = parser.ParseReportDescriptor(copy, descriptor.size());
	free(copy);
	return status;
}


static double
now()
{
	timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return time.tv_sec + time.tv_nsec / 1e9;
}


// #pragma mark - using what was parsed


/*!	What the protocol handlers do with a parsed descriptor, with reports of
	exactly the size the parser says they have: read every field of an input
	or feature report, write every field of an output report (as
	HIDReport::SendReport() does), and walk the collections.
*/
static void
exercise(HIDParser &parser, uint32 seed)
{
	uint32 reportCount = parser.CountReports(HID_REPORT_TYPE_ANY);
	for (uint32 i = 0; i < reportCount; i++) {
		HIDReport *report = parser.ReportAt(HID_REPORT_TYPE_ANY, i);
		if (report == NULL)
			continue;

		size_t size = report->ReportSize();
		uint8 *buffer = (uint8 *)malloc(size);
		for (size_t j = 0; j < size; j++)
			buffer[j] = (uint8)(seed = seed * 1103515245 + 12345) >> 16;

		report->SetReport(B_OK, buffer, size);
		uint32 itemCount = report->CountItems();
		for (uint32 j = 0; j < itemCount; j++) {
			HIDReportItem *item = report->ItemAt(j);
			if (item == NULL) {
				CHECK(false, "report %u has no item %u of %u", (unsigned)i,
					(unsigned)j, (unsigned)itemCount);
				continue;
			}

			if (report->Type() == HID_REPORT_TYPE_OUTPUT) {
				item->SetData(seed = seed * 1103515245 + 12345);
				item->Insert();
			}

			item->Extract();
			item->ScaledData(8, false);
			item->ScaledData(16, true);
			item->ScaledData(32, true);
			item->ScaledRangeData(1, 8);
			item->ScaledFloatData();
		}

		// (looks at every item of the report)
		report->FindItem(0xfffe, 0xfffe);
		report->SetReport(B_NO_INIT, NULL, 0);
		free(buffer);
	}

	// A report as the device sends it, of the size the transfer buffer has.
	size_t maxSize = parser.MaxReportSize();
	uint8 *buffer = (uint8 *)calloc(1, maxSize + 1);
	parser.SetReport(B_OK, buffer, maxSize);
	parser.SetReport(B_NO_INIT, NULL, 0);
	free(buffer);

	HIDCollection *root = parser.RootCollection();
	if (root == NULL)
		return;

	uint32 collections = root->CountChildrenFlat(COLLECTION_ALL);
	for (uint32 i = 0; i < collections && i < 64; i++) {
		HIDCollection *collection = root->ChildAtFlat(COLLECTION_ALL, i);
		if (collection == NULL)
			continue;

		uint32 items = collection->CountItemsFlat();
		for (uint32 j = 0; j < items && j < 64; j++) {
			if (collection->ItemAtFlat(j) == NULL)
				CHECK(false, "collection %u has no item %u", (unsigned)i,
					(unsigned)j);
		}
	}
}


// #pragma mark - the cases


static void
test_mouse()
{
	// an ordinary mouse still reads as before
	Descriptor descriptor;
	add_mouse_start(descriptor);
	add_axis(descriptor, 0x30, 8);
	add_axis(descriptor, 0x31, 8);
	add_item(descriptor, MAIN, END_COLLECTION, 0, 0);

	HIDParser parser(NULL);
	CHECK(parse(parser, descriptor) == B_OK, "the mouse does not parse");

	HIDReport *report = parser.ReportAt(HID_REPORT_TYPE_INPUT, 0);
	CHECK(report != NULL, "no input report");
	if (report == NULL)
		return;

	CHECK(report->ReportSize() == 3, "report size %zu", report->ReportSize());
	CHECK(report->CountItems() == 6, "%u items",
		(unsigned)report->CountItems());

	uint8 data[3] = { 0x05, 0x10, 0xf0 };
	report->SetReport(B_OK, data, sizeof(data));
	HIDReportItem *x = report->FindItem(0x01, 0x30);
	HIDReportItem *y = report->FindItem(0x01, 0x31);
	HIDReportItem *left = report->FindItem(0x09, 1);
	HIDReportItem *right = report->FindItem(0x09, 2);
	HIDReportItem *middle = report->FindItem(0x09, 3);
	CHECK(x != NULL && y != NULL && left != NULL && right != NULL
		&& middle != NULL, "missing items");
	if (x == NULL || y == NULL || left == NULL || right == NULL
		|| middle == NULL) {
		return;
	}

	CHECK(x->Extract() == B_OK && x->Valid() && (int32)x->Data() == 16,
		"x = %d", (int)x->Data());
	CHECK(y->Extract() == B_OK && y->Valid() && (int32)y->Data() == -16,
		"y = %d", (int)y->Data());
	CHECK(left->Extract() == B_OK && left->Data() == 1, "left");
	CHECK(right->Extract() == B_OK && right->Data() == 0, "right");
	CHECK(middle->Extract() == B_OK && middle->Data() == 1, "middle");
	CHECK(x->ScaledData(16, true) == 0x1010, "x scaled 0x%x",
		(unsigned)x->ScaledData(16, true));
}


static void
test_32_bits()
{
	// 32 bit fields: one at a byte, one 4 bits further (it spans 5 bytes)
	Descriptor descriptor;
	add_item(descriptor, GLOBAL, USAGE_PAGE, 0xff00, 2);
	add_item(descriptor, LOCAL, USAGE, 0x01);
	add_item(descriptor, MAIN, COLLECTION, 0x01);
	add_item(descriptor, GLOBAL, LOGICAL_MINIMUM, 0);
	add_item(descriptor, GLOBAL, LOGICAL_MAXIMUM, 0xffffffff, 4);
	add_item(descriptor, GLOBAL, REPORT_COUNT, 1);
	for (int type = INPUT; type <= OUTPUT; type++) {
		add_item(descriptor, LOCAL, USAGE, 0x10);
		add_item(descriptor, GLOBAL, REPORT_SIZE, 32);
		add_item(descriptor, MAIN, type, 0x02);
		add_item(descriptor, GLOBAL, REPORT_SIZE, 4);
		add_item(descriptor, MAIN, type, 0x01);
		add_item(descriptor, LOCAL, USAGE, 0x11);
		add_item(descriptor, GLOBAL, REPORT_SIZE, 32);
		add_item(descriptor, MAIN, type, 0x02);
		add_item(descriptor, LOCAL, USAGE, 0x12);
		add_item(descriptor, GLOBAL, REPORT_SIZE, 4);
		add_item(descriptor, MAIN, type, 0x02);
	}
	add_item(descriptor, MAIN, END_COLLECTION, 0, 0);

	HIDParser parser(NULL);
	CHECK(parse(parser, descriptor) == B_OK, "does not parse");

	HIDReport *input = parser.ReportAt(HID_REPORT_TYPE_INPUT, 0);
	HIDReport *output = parser.ReportAt(HID_REPORT_TYPE_OUTPUT, 0);
	CHECK(input != NULL && output != NULL, "missing reports");
	if (input == NULL || output == NULL)
		return;

	CHECK(input->ReportSize() == 9, "input size %zu", input->ReportSize());

	// 0xdeadbeef, 4 bits of padding, 0x12345678, 0xa
	uint8 *data = (uint8 *)malloc(9);
	const uint8 kReport[9] = { 0xef, 0xbe, 0xad, 0xde, 0x80, 0x67, 0x45, 0x23,
		0xa1 };
	memcpy(data, kReport, 9);
	input->SetReport(B_OK, data, 9);

	HIDReportItem *aligned = input->FindItem(0xff00, 0x10);
	HIDReportItem *shifted = input->FindItem(0xff00, 0x11);
	HIDReportItem *nibble = input->FindItem(0xff00, 0x12);
	CHECK(aligned != NULL && shifted != NULL && nibble != NULL,
		"missing input items");
	if (aligned != NULL && shifted != NULL && nibble != NULL) {
		CHECK(aligned->Extract() == B_OK && aligned->Data() == 0xdeadbeef,
			"32 bits at a byte read as 0x%08x", (unsigned)aligned->Data());
		CHECK(shifted->Extract() == B_OK && shifted->Data() == 0x12345678,
			"32 bits at bit 4 read as 0x%08x", (unsigned)shifted->Data());
		CHECK(nibble->Extract() == B_OK && nibble->Data() == 0xa,
			"the 4 bits after them read as 0x%x", (unsigned)nibble->Data());
	}
	free(data);

	// the same written
	data = (uint8 *)malloc(9);
	memset(data, 0, 9);
	output->SetReport(B_OK, data, 9);
	aligned = output->FindItem(0xff00, 0x10);
	shifted = output->FindItem(0xff00, 0x11);
	nibble = output->FindItem(0xff00, 0x12);
	CHECK(aligned != NULL && shifted != NULL && nibble != NULL,
		"missing output items");
	if (aligned != NULL && shifted != NULL && nibble != NULL) {
		aligned->SetData(0xdeadbeef);
		shifted->SetData(0x12345678);
		nibble->SetData(0xa);
		for (uint32 i = 0; i < output->CountItems(); i++)
			output->ItemAt(i)->Insert();

		const uint8 kWritten[9] = { 0xef, 0xbe, 0xad, 0xde, 0x80, 0x67, 0x45,
			0x23, 0xa1 };
		CHECK(memcmp(data, kWritten, 9) == 0, "written: %02x %02x %02x %02x "
			"%02x %02x %02x %02x %02x", data[0], data[1], data[2], data[3],
			data[4], data[5], data[6], data[7], data[8]);
	}
	free(data);
}


static void
test_wide_input()
{
	// the mouse of the ticket: an X axis of 200 bits
	Descriptor descriptor;
	add_mouse_start(descriptor);
	add_axis(descriptor, 0x30, 200);
	add_axis(descriptor, 0x31, 8);
	add_item(descriptor, MAIN, END_COLLECTION, 0, 0);

	HIDParser parser(NULL);
	CHECK(parse(parser, descriptor) == B_OK, "does not parse");

	HIDReport *report = parser.ReportAt(HID_REPORT_TYPE_INPUT, 0);
	CHECK(report != NULL, "no input report");
	if (report == NULL)
		return;

	CHECK(report->ReportSize() == 27, "report size %zu", report->ReportSize());

	uint8 *data = (uint8 *)malloc(27);
	memset(data, 0x41, 27);
	data[0] = 0x02;
	data[26] = 0x7f;
	report->SetReport(B_OK, data, 27);
	for (uint32 i = 0; i < report->CountItems(); i++)
		report->ItemAt(i)->Extract();

	HIDReportItem *y = report->FindItem(0x01, 0x31);
	HIDReportItem *right = report->FindItem(0x09, 2);
	CHECK(y != NULL && y->Valid() && y->Data() == 0x7f,
		"the field after the wide one is not read");
	CHECK(right != NULL && right->Data() == 1,
		"the field before the wide one is not read");
	free(data);
}


static void
test_wide_output()
{
	// an output report with a 64 bit field between two bytes
	Descriptor descriptor;
	add_item(descriptor, GLOBAL, USAGE_PAGE, 0x08);		// LEDs
	add_item(descriptor, LOCAL, USAGE, 0x01);
	add_item(descriptor, MAIN, COLLECTION, 0x01);
	add_item(descriptor, GLOBAL, LOGICAL_MINIMUM, 0);
	add_item(descriptor, GLOBAL, LOGICAL_MAXIMUM, 0xff);
	add_item(descriptor, GLOBAL, REPORT_COUNT, 1);
	add_item(descriptor, LOCAL, USAGE, 0x01);
	add_item(descriptor, GLOBAL, REPORT_SIZE, 8);
	add_item(descriptor, MAIN, OUTPUT, 0x02);
	add_item(descriptor, LOCAL, USAGE, 0x02);
	add_item(descriptor, GLOBAL, REPORT_SIZE, 64);
	add_item(descriptor, MAIN, OUTPUT, 0x02);
	add_item(descriptor, LOCAL, USAGE, 0x03);
	add_item(descriptor, GLOBAL, REPORT_SIZE, 8);
	add_item(descriptor, MAIN, OUTPUT, 0x02);
	add_item(descriptor, MAIN, END_COLLECTION, 0, 0);

	HIDParser parser(NULL);
	CHECK(parse(parser, descriptor) == B_OK, "does not parse");

	HIDReport *report = parser.ReportAt(HID_REPORT_TYPE_OUTPUT, 0);
	CHECK(report != NULL, "no output report");
	if (report == NULL)
		return;

	CHECK(report->ReportSize() == 10, "report size %zu", report->ReportSize());

	uint8 *data = (uint8 *)calloc(1, 10);
	report->SetReport(B_OK, data, 10);
	HIDReportItem *first = report->FindItem(0x08, 0x01);
	HIDReportItem *last = report->FindItem(0x08, 0x03);
	CHECK(first != NULL && last != NULL, "missing items");
	if (first != NULL && last != NULL) {
		first->SetData(0x11);
		last->SetData(0x33);
		for (uint32 i = 0; i < report->CountItems(); i++)
			report->ItemAt(i)->Insert();

		CHECK(data[0] == 0x11 && data[9] == 0x33, "written: %02x ... %02x",
			data[0], data[9]);
		for (int i = 1; i < 9; i++)
			CHECK(data[i] == 0, "byte %d of the wide field is 0x%02x", i,
				data[i]);
	}
	free(data);
}


static void
test_huge_count()
{
	// 4 billion one bit fields
	Descriptor descriptor;
	add_mouse_start(descriptor);
	add_item(descriptor, LOCAL, USAGE, 0x30);
	add_item(descriptor, GLOBAL, REPORT_SIZE, 1);
	add_item(descriptor, GLOBAL, REPORT_COUNT, 0xffffffff, 4);
	add_item(descriptor, MAIN, INPUT, 0x02);
	add_item(descriptor, MAIN, END_COLLECTION, 0, 0);

	HIDParser parser(NULL);
	double start = now();
	status_t status = parse(parser, descriptor);
	printf("INFO 4 billion fields: status 0x%x after %.3f s\n",
		(unsigned)status, now() - start);
	CHECK(status == B_BAD_DATA, "status 0x%x", (unsigned)status);
	CHECK(parser.CountReports(HID_REPORT_TYPE_ANY) == 0,
		"reports of a refused descriptor are left");
	exercise(parser, 1);

	// 4 billion empty ones: nothing to refuse, and nothing to allocate
	descriptor.clear();
	add_mouse_start(descriptor);
	add_item(descriptor, LOCAL, USAGE, 0x30);
	add_item(descriptor, GLOBAL, REPORT_SIZE, 0);
	add_item(descriptor, GLOBAL, REPORT_COUNT, 0xffffffff, 4);
	add_item(descriptor, MAIN, INPUT, 0x02);
	add_axis(descriptor, 0x31, 8);
	add_item(descriptor, MAIN, END_COLLECTION, 0, 0);

	start = now();
	status = parse(parser, descriptor);
	printf("INFO 4 billion empty fields: status 0x%x after %.3f s\n",
		(unsigned)status, now() - start);
	CHECK(status == B_OK, "status 0x%x", (unsigned)status);
	HIDReport *report = parser.ReportAt(HID_REPORT_TYPE_INPUT, 0);
	CHECK(report != NULL && report->ReportSize() == 2
		&& report->CountItems() == 5, "the report around the empty fields");
	exercise(parser, 2);

	// more fields than a descriptor may have, 4000 at a time
	descriptor.clear();
	add_mouse_start(descriptor);
	add_item(descriptor, GLOBAL, REPORT_SIZE, 1);
	add_item(descriptor, GLOBAL, REPORT_COUNT, 4000);
	for (int id = 1; id <= 100; id++) {
		add_item(descriptor, GLOBAL, REPORT_ID, id);
		add_item(descriptor, MAIN, INPUT, 0x02);
	}
	add_item(descriptor, MAIN, END_COLLECTION, 0, 0);
	status = parse(parser, descriptor);
	CHECK(status == B_BAD_DATA, "400000 fields: status 0x%x",
		(unsigned)status);
}


static void
test_size_overflow()
{
	// 8 fields of 2^29 + 8 bits: 64 bits in 32 bit arithmetic
	Descriptor descriptor;
	add_mouse_start(descriptor);
	add_item(descriptor, LOCAL, USAGE, 0x30);
	add_item(descriptor, GLOBAL, REPORT_SIZE, 0x20000008, 4);
	add_item(descriptor, GLOBAL, REPORT_COUNT, 8);
	add_item(descriptor, MAIN, INPUT, 0x02);
	add_item(descriptor, MAIN, END_COLLECTION, 0, 0);

	HIDParser parser(NULL);
	status_t status = parse(parser, descriptor);
	CHECK(status == B_BAD_DATA, "status 0x%x", (unsigned)status);
	exercise(parser, 3);

	// a report of exactly the limit is taken, one bit more is not
	for (int extra = 0; extra <= 1; extra++) {
		descriptor.clear();
		add_item(descriptor, GLOBAL, USAGE_PAGE, 0xff00, 2);
		add_item(descriptor, LOCAL, USAGE, 0x01);
		add_item(descriptor, MAIN, COLLECTION, 0x01);
		add_item(descriptor, GLOBAL, REPORT_SIZE, 8);
		add_item(descriptor, GLOBAL, REPORT_COUNT, 16384);
		add_item(descriptor, MAIN, FEATURE, 0x02);
		if (extra != 0) {
			add_item(descriptor, GLOBAL, REPORT_SIZE, 1);
			add_item(descriptor, GLOBAL, REPORT_COUNT, 1);
			add_item(descriptor, MAIN, FEATURE, 0x02);
		}
		add_item(descriptor, MAIN, END_COLLECTION, 0, 0);

		status = parse(parser, descriptor);
		CHECK(status == (extra != 0 ? B_BAD_DATA : B_OK),
			"16 KiB report + %d bits: status 0x%x", extra, (unsigned)status);
		exercise(parser, 4);
	}
}


static void
test_usage_range()
{
	// a range of 65536 usages, as an array of keys may have
	Descriptor descriptor;
	add_item(descriptor, GLOBAL, USAGE_PAGE, 0x0c);
	add_item(descriptor, LOCAL, USAGE, 0x01);
	add_item(descriptor, MAIN, COLLECTION, 0x01);
	add_item(descriptor, LOCAL, USAGE_MINIMUM, 0);
	add_item(descriptor, LOCAL, USAGE_MAXIMUM, 0xffff, 2);
	add_item(descriptor, GLOBAL, LOGICAL_MINIMUM, 0);
	add_item(descriptor, GLOBAL, LOGICAL_MAXIMUM, 0xffff, 2);
	add_item(descriptor, GLOBAL, REPORT_SIZE, 16);
	add_item(descriptor, GLOBAL, REPORT_COUNT, 2);
	add_item(descriptor, MAIN, INPUT, 0x00);		// data, array
	add_item(descriptor, MAIN, END_COLLECTION, 0, 0);

	HIDParser parser(NULL);
	double start = now();
	status_t status = parse(parser, descriptor);
	printf("INFO usages 0 to 0xffff: status 0x%x after %.3f s\n",
		(unsigned)status, now() - start);
	CHECK(status == B_OK, "status 0x%x", (unsigned)status);
	HIDReport *report = parser.ReportAt(HID_REPORT_TYPE_INPUT, 0);
	CHECK(report != NULL && report->CountItems() == 2
		&& report->CountUsages() > 0 && report->Usages()[0] == 0x000c0000,
		"the array's report");
	exercise(parser, 5);

	// and one of (nearly) all of them
	descriptor[9] = 0x2b;		// the usage maximum with 4 bytes
	descriptor[10] = 0xfe;
	descriptor.insert(descriptor.begin() + 12, 2, 0xff);
	start = now();
	status = parse(parser, descriptor);
	printf("INFO usages 0 to 0xfffffffe: status 0x%x after %.3f s\n",
		(unsigned)status, now() - start);
	CHECK(status == B_OK, "status 0x%x", (unsigned)status);
	report = parser.ReportAt(HID_REPORT_TYPE_INPUT, 0);
	CHECK(report != NULL && report->CountItems() == 2, "the array's report");
	exercise(parser, 6);

	// many main items with a range each: what is kept of them is limited
	descriptor.clear();
	add_item(descriptor, GLOBAL, USAGE_PAGE, 0x0c);
	add_item(descriptor, LOCAL, USAGE, 0x01);
	add_item(descriptor, MAIN, COLLECTION, 0x01);
	add_item(descriptor, GLOBAL, REPORT_SIZE, 8);
	add_item(descriptor, GLOBAL, REPORT_COUNT, 1);
	for (int i = 0; i < 2000; i++) {
		add_item(descriptor, LOCAL, USAGE_MINIMUM, 0);
		add_item(descriptor, LOCAL, USAGE_MAXIMUM, 0xffff, 2);
		add_item(descriptor, MAIN, INPUT, 0x00);
	}
	add_item(descriptor, MAIN, END_COLLECTION, 0, 0);
	start = now();
	status = parse(parser, descriptor);
	report = parser.ReportAt(HID_REPORT_TYPE_INPUT, 0);
	printf("INFO 2000 ranges of 65536 usages: status 0x%x after %.3f s, %u "
		"usages kept\n", (unsigned)status, now() - start,
		report != NULL ? (unsigned)report->CountUsages() : 0);
	CHECK(status == B_OK && report != NULL && report->CountItems() == 2000
		&& report->CountUsages() <= 65536, "the report of 2000 arrays");
}


static void
test_truncated()
{
	// a descriptor that ends within its last item
	for (int bytes = 0; bytes < 5; bytes++) {
		for (int size = 1; size <= 4; size++) {
			if (bytes >= (size == 3 ? 4 : size == 4 ? 2 : size))
				continue;

			Descriptor descriptor;
			add_mouse_start(descriptor);
			add_axis(descriptor, 0x30, 8);
			add_item(descriptor, MAIN, END_COLLECTION, 0, 0);
			if (size == 4) {
				// a long item: its size and tag follow
				descriptor.push_back(0xfe);
			} else {
				// a usage with data of 1, 2 or 4 bytes
				descriptor.push_back((USAGE << 4) | (LOCAL << 2) | size);
			}

			for (int i = 0; i < bytes; i++)
				descriptor.push_back(0x7f);

			HIDParser parser(NULL);
			status_t status = parse(parser, descriptor);
			HIDReport *report = parser.ReportAt(HID_REPORT_TYPE_INPUT, 0);
			CHECK(status == B_OK && report != NULL && report->ReportSize() == 2,
				"size code %d with %d bytes: status 0x%x", size, bytes,
				(unsigned)status);
		}
	}

	// a long item that says it is longer than the descriptor
	Descriptor descriptor;
	add_mouse_start(descriptor);
	descriptor.push_back(0xfe);
	descriptor.push_back(200);
	descriptor.push_back(0x01);
	descriptor.push_back(0x00);
	HIDParser parser(NULL);
	CHECK(parse(parser, descriptor) == B_OK, "long item");

	// and one that fits is skipped
	descriptor.resize(descriptor.size() - 3);
	descriptor.push_back(3);
	descriptor.push_back(0x01);
	descriptor.insert(descriptor.end(), 3, 0xa1);
	add_axis(descriptor, 0x30, 8);
	add_item(descriptor, MAIN, END_COLLECTION, 0, 0);
	CHECK(parse(parser, descriptor) == B_OK, "long item");
	HIDReport *report = parser.ReportAt(HID_REPORT_TYPE_INPUT, 0);
	CHECK(report != NULL && report->ReportSize() == 2,
		"the items after a long item");
}


static void
test_deep_collections()
{
	// as many nested collections as a USB report descriptor has room for:
	// the tree is walked and freed recursively, in the kernel on its stack
	Descriptor descriptor;
	for (int i = 0; i < 32767; i++)
		add_item(descriptor, MAIN, COLLECTION, 0x00);

	HIDParser parser(NULL);
	status_t status = parse(parser, descriptor);
	CHECK(status == B_BAD_DATA, "32767 nested collections: status 0x%x",
		(unsigned)status);
	exercise(parser, 7);

	// what a device may have
	descriptor.clear();
	for (int i = 0; i < 16; i++)
		add_item(descriptor, MAIN, COLLECTION, 0x00);
	add_mouse_start(descriptor);
	add_axis(descriptor, 0x30, 8);
	for (int i = 0; i < 17; i++)
		add_item(descriptor, MAIN, END_COLLECTION, 0, 0);
	status = parse(parser, descriptor);
	CHECK(status == B_OK, "16 nested collections: status 0x%x",
		(unsigned)status);
	exercise(parser, 8);
}


static void
test_leak()
{
	// nothing of a parsed descriptor is left behind (LeakSanitizer looks)
	Descriptor descriptor;
	add_mouse_start(descriptor);
	add_axis(descriptor, 0x30, 8);
	add_axis(descriptor, 0x31, 8);
	add_item(descriptor, GLOBAL, PUSH, 0, 0);
	add_item(descriptor, MAIN, END_COLLECTION, 0, 0);

	HIDParser *parser = new HIDParser(NULL);
	CHECK(parse(*parser, descriptor) == B_OK, "does not parse");
	CHECK(parse(*parser, descriptor) == B_OK, "does not parse again");
	delete parser;
}


static uint32 sRandom = 0x1234567;

static uint32
random_value()
{
	// xorshift32
	sRandom ^= sRandom << 13;
	sRandom ^= sRandom >> 17;
	sRandom ^= sRandom << 5;
	return sRandom;
}


static uint32
random_data()
{
	static const uint32 kValues[] = { 0, 1, 2, 3, 4, 7, 8, 9, 12, 15, 16, 17,
		24, 31, 32, 33, 40, 63, 64, 65, 127, 128, 200, 255, 256, 257, 1023,
		4096, 0x7fff, 0x8000, 0xffff, 0x10000, 0x20000, 0x7fffffff,
		0x80000000, 0xfffffffe, 0xffffffff };
	uint32 choice = random_value() % 48;
	if (choice < sizeof(kValues) / sizeof(kValues[0]))
		return kValues[choice];
	if (choice < 44)
		return random_value() % 40;
	return random_value();
}


static void
test_fuzz()
{
	// Random descriptors, of the items the parser knows and of plain noise,
	// and random reports for what it makes of them.
	static const uint8 kMain[] = { INPUT, INPUT, INPUT, OUTPUT, FEATURE,
		COLLECTION, END_COLLECTION, 0x3 };
	static const uint8 kGlobal[] = { USAGE_PAGE, LOGICAL_MINIMUM,
		LOGICAL_MAXIMUM, 0x3, 0x4, 0x5, 0x6, REPORT_SIZE, REPORT_SIZE,
		REPORT_SIZE, REPORT_ID, REPORT_COUNT, REPORT_COUNT, PUSH, POP, 0xd };
	static const uint8 kLocal[] = { USAGE, USAGE, USAGE_MINIMUM,
		USAGE_MAXIMUM, 0x3, 0x4, 0x5, 0x7, 0x8, 0x9, 0xa };

	double start = now();
	uint32 accepted = 0;
	uint32 refused = 0;
	uint32 items = 0;
	const uint32 kRounds = 60000;
	for (uint32 round = 0; round < kRounds; round++) {
		Descriptor descriptor;
		uint32 length = 1 + random_value() % 48;
		bool noise = round % 8 == 7;
		for (uint32 i = 0; i < length; i++) {
			if (noise) {
				descriptor.push_back((uint8)random_value());
				continue;
			}

			uint32 data = random_data();
			int bytes = random_value() % 8 == 0 ? (int)(random_value() % 3)
				: -1;
			if (bytes == 3)
				bytes = 4;

			switch (random_value() % 3) {
				case 0:
					add_item(descriptor, MAIN,
						kMain[random_value() % sizeof(kMain)],
						random_value() % 4 == 0 ? data : data & 0xff, bytes);
					break;
				case 1:
					add_item(descriptor, GLOBAL,
						kGlobal[random_value() % sizeof(kGlobal)], data,
						bytes);
					break;
				case 2:
					add_item(descriptor, LOCAL,
						kLocal[random_value() % sizeof(kLocal)], data, bytes);
					break;
			}
		}

		if (random_value() % 16 == 0 && !descriptor.empty())
			descriptor.resize(1 + random_value() % descriptor.size());

		HIDParser parser(NULL);
		status_t status = parse(parser, descriptor);
		if (status == B_OK)
			accepted++;
		else
			refused++;

		for (uint32 i = 0; i < parser.CountReports(HID_REPORT_TYPE_ANY); i++)
			items += parser.ReportAt(HID_REPORT_TYPE_ANY, i)->CountItems();

		exercise(parser, round);

		// (under the sanitizers a descriptor at the limits takes a while)
		if (now() - start > 20)
			break;
	}

	printf("INFO %u random descriptors: %u accepted, %u refused, %u report "
		"items read and written, %.1f s\n", (unsigned)(accepted + refused),
		(unsigned)accepted, (unsigned)refused, (unsigned)items,
		now() - start);
	CHECK(accepted + refused >= 500, "too slow");
}


static const struct {
	const char *name;
	void (*function)();
} kCases[] = {
	{ "mouse", test_mouse },
	{ "32_bits", test_32_bits },
	{ "wide_input", test_wide_input },
	{ "wide_output", test_wide_output },
	{ "huge_count", test_huge_count },
	{ "size_overflow", test_size_overflow },
	{ "usage_range", test_usage_range },
	{ "truncated", test_truncated },
	{ "deep_collections", test_deep_collections },
	{ "leak", test_leak },
	{ "fuzz", test_fuzz }
};


int
main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);

	if (argc != 2) {
		fprintf(stderr, "usage: %s list|<case>\n", argv[0]);
		return 2;
	}

	for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
		if (strcmp(argv[1], "list") == 0)
			printf("%s\n", kCases[i].name);
		else if (strcmp(argv[1], kCases[i].name) == 0) {
			kCases[i].function();
			return sFailures == 0 ? 0 : 1;
		}
	}

	return strcmp(argv[1], "list") == 0 ? 0 : 2;
}
