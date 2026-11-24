/*
 * Copyright 2026 air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	The launch_daemon reads the environment of a sourced script ("env" of a
	job or target) from the shell's "export -p" through a pipe, in pieces of
	up to 4 KiB. It took the end of each piece for the end of a line: the
	variable at the boundary lost its end, and the rest was dropped
	(airos-works/haiku#188).

	This feeds Utility::ParseExportOutput() the same output in pieces of many
	sizes, and compares the variables with the ones of the output in one
	piece.
*/


#include <stdio.h>
#include <string.h>

#include <String.h>
#include <StringList.h>

#include "Utility.h"


static void
parse(const BString& output, size_t pieceSize, BStringList& environment)
{
	BString rest;
	size_t length = output.Length();
	for (size_t offset = 0; offset < length; offset += pieceSize) {
		size_t size = length - offset;
		if (size > pieceSize)
			size = pieceSize;
		Utility::ParseExportOutput(environment, rest,
			output.String() + offset, size);
	}
	Utility::ParseExportVariable(environment, rest);
}


static bool
same(const BStringList& a, const BStringList& b)
{
	if (a.CountStrings() != b.CountStrings())
		return false;
	for (int32 i = 0; i < a.CountStrings(); i++) {
		if (a.StringAt(i) != b.StringAt(i))
			return false;
	}
	return true;
}


int
main()
{
	// what "export -p" prints: 300 variables of different lengths, a line
	// that is none, and a last line without a line break
	BString output;
	BStringList expected;
	for (int32 i = 0; i < 300; i++) {
		BString name;
		name.SetToFormat("TEST_VARIABLE_%03" B_PRId32, i);
		BString value;
		for (int32 j = 0; j <= i % 97; j++)
			value << (char)('a' + (i + j) % 26);
		if (i == 150)
			output << "sh: some message of the script\n";
		output << "export " << name << "=\"" << value << "\"";
		if (i < 299)
			output << "\n";
		expected.Add(BString(name) << "=" << value);
	}

	bool ok = true;

	BStringList whole;
	parse(output, output.Length(), whole);
	if (!same(whole, expected)) {
		printf("FAIL the output in one piece: %" B_PRId32 " variables, "
			"expected %" B_PRId32 "\n", whole.CountStrings(),
			expected.CountStrings());
		ok = false;
	} else {
		printf("ok   the output in one piece (%" B_PRId32 " bytes): %" B_PRId32
			" variables\n", output.Length(), whole.CountStrings());
	}

	int32 failed = 0;
	int32 sizes = 0;
	for (size_t pieceSize = 1; pieceSize <= 5000; pieceSize++) {
		BStringList environment;
		parse(output, pieceSize, environment);
		sizes++;
		if (same(environment, expected))
			continue;
		if (failed++ < 5) {
			int32 different = 0;
			for (int32 i = 0; i < expected.CountStrings(); i++) {
				if (!environment.HasString(expected.StringAt(i)))
					different++;
			}
			printf("FAIL pieces of %" B_PRIuSIZE " bytes: %" B_PRId32
				" variables, %" B_PRId32 " of %" B_PRId32 " missing or cut\n",
				pieceSize, environment.CountStrings(), different,
				expected.CountStrings());
		}
	}
	if (failed > 0) {
		printf("FAIL %" B_PRId32 " of %" B_PRId32 " piece sizes gave other "
			"variables than the whole output\n", failed, sizes);
		ok = false;
	} else {
		printf("ok   pieces of 1 to 5000 bytes (4095 and 4096 among them) "
			"give the same variables\n");
	}

	printf("export_parse_test: %s\n", ok ? "PASSED" : "FAILED");
	return ok ? 0 : 1;
}
