/*
 * Copyright 2008-2013, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Distributed under the terms of the MIT License.
 */

#include "TerminalCharClassifier.h"

#include <ctype.h>

#include <algorithm>

#include <UnicodeChar.h>


// #pragma mark - TerminalCharClassifier


TerminalCharClassifier::~TerminalCharClassifier()
{
}


// #pragma mark - DefaultCharClassifier


DefaultCharClassifier::DefaultCharClassifier(const char* additionalWordChars)
{
	const char* p = additionalWordChars;
	while (p != NULL && *p != '\0')
		fAdditionalWordChars.push_back(BUnicodeChar::FromUTF8(&p));
}


int
DefaultCharClassifier::Classify(uint32 character)
{
	if (BUnicodeChar::IsSpace(character))
		return CHAR_TYPE_SPACE;

	if (BUnicodeChar::IsAlNum(character))
		return CHAR_TYPE_WORD_CHAR;

	if (std::find(fAdditionalWordChars.begin(), fAdditionalWordChars.end(),
			character) != fAdditionalWordChars.end()) {
		return CHAR_TYPE_WORD_CHAR;
	}

	return CHAR_TYPE_WORD_DELIMITER;
}
