/*
 * Copyright 2026, air/OS.
 * Copyright 2026 Renku Mobile contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef SWIPE_DECODER_H
#define SWIPE_DECODER_H

/*	SwipeDecoder: the words a finger's path across the letters could mean.

	Shape matching: every word's path from key centre to key centre, its
	corners cut as a finger cuts them, is compared with the finger's, both
	resampled to the same number of points, and how far apart they lie is
	weighed against how common the word is. Only words that start and end
	near where the finger did are tried.

	Keys are compared as if their rows were as far apart as on a phone held
	upright, a little more than a key's width: on a wide screen the keys are
	much wider than high, and a path's ups and downs would count for little
	beside its way along the rows.

	It knows nothing of the Interface Kit, so test/swipe-test.cpp can check
	it on the build host.
*/

#include <vector>


struct SwipePoint {
	float	x;
	float	y;
};


class SwipeDecoder {
public:
								SwipeDecoder();

			// Where each letter's key is, how wide keys are, and how far
			// apart their rows.
			void				SetKey(char letter, SwipePoint center);
			void				SetKeyWidth(float width);
			void				SetRowHeight(float height);

			// The likeliest words, best first.
			std::vector<const char*> Decode(
									const std::vector<SwipePoint>& path,
									size_t count) const;

private:
			SwipePoint			fKeys[26];
			bool				fHasKey[26];
			float				fKeyWidth;
			float				fRowHeight;		// 0: as the keys are
			// Each word's finger path, worked out when first needed.
	mutable	std::vector<std::vector<SwipePoint>> fPaths;
};


#endif	// SWIPE_DECODER_H
