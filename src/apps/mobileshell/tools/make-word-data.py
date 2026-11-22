#!/usr/bin/env python3
# Copyright 2026, air/OS.
# Copyright 2026 Renku Mobile contributors.
# Distributed under the terms of the MIT License.
"""make-word-data.py 1GRAMS_ENGLISH.CSV > ../WordData.h

The words swipe typing knows, with how common each is: the 10,000 most
frequent English words of the Google Books Ngram Corpus, as cleaned by
https://github.com/orgtre/google-books-ngram-frequency (ngrams/
1grams_english.csv, CC BY 3.0). Words keep how they're written (I,
English, o'clock); the keyboard matches their letters. Anything with other
characters is left out.
"""
import csv
import math
import sys

words = {}
for row in csv.DictReader(open(sys.argv[1], encoding="utf-8")):
    word, freq = row["ngram"], int(row["freq"])
    letters = word.replace("'", "")
    if not letters.isascii() or not letters.isalpha():
        continue
    # the same word written two ways counts once, as its commoner form
    key = word.lower()
    if key not in words or words[key][1] < freq:
        words[key] = (word, freq)

out = sys.stdout
out.write("""/*
 * Copyright 2026, air/OS.
 * Copyright 2026 Renku Mobile contributors.
 * Distributed under the terms of the MIT License.
 */

/*	The words swipe typing knows, most common first, each with 100 times the
	natural logarithm of how often it was counted. Generated; don't edit.

	From the Google Books Ngram Corpus's 10,000 most frequent English words,
	as cleaned in https://github.com/orgtre/google-books-ngram-frequency,
	licensed under CC BY 3.0 (https://creativecommons.org/licenses/by/3.0/).
*/

#ifndef WORD_DATA_H
#define WORD_DATA_H

#include <stdint.h>


struct SwipeWord {
	const char*	word;
	uint16_t	weight;		// 100 times the natural log of its count
};

static const SwipeWord kSwipeWords[] = {
""")
for word, freq in sorted(words.values(), key=lambda w: -w[1]):
    out.write('\t{ "%s", %d },\n' % (word, round(math.log(freq) * 100)))
out.write("""};
static const int kSwipeWordCount = sizeof(kSwipeWords) / sizeof(kSwipeWords[0]);


#endif	// WORD_DATA_H
""")
