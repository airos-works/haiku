/*
 * Copyright 2026, air/OS.
 * Copyright 2026 Renku Mobile contributors.
 * Distributed under the terms of the MIT License.
 */


#include "SwipeDecoder.h"

#include "WordData.h"

#include <algorithm>
#include <ctype.h>
#include <math.h>


static const int kSamples = 32;
// A word must start and end within this many key widths of the finger.
static const float kEndSlop = 1.2f;
// How far a finger cuts corners between keys, in key widths.
static const float kCornerCut = 0.8f;
// How much the key the finger passed farthest from counts.
static const float kMissWeight = 0.5f;
// How fast the match falls off with distance, in key widths.
static const float kSigma = 0.25f;
// How much a word's commonness counts against its distance.
static const float kWeightScale = 0.006f;
// How far apart the rows are taken to be, in key widths: as on the phone
// keyboard all of the above was tuned on.
static const float kRowPitch = 1.27f;


static float
distance(SwipePoint a, SwipePoint b)
{
	return hypotf(a.x - b.x, a.y - b.y);
}


/*!	\a count points spread evenly along the polyline. */
static std::vector<SwipePoint>
resample(const std::vector<SwipePoint>& points, int count)
{
	float length = 0;
	for (size_t i = 1; i < points.size(); i++)
		length += distance(points[i - 1], points[i]);

	std::vector<SwipePoint> out;
	if (length == 0) {
		out.assign(count, points[0]);
		return out;
	}

	float step = length / (count - 1);
	float travelled = 0;
	size_t segment = 1;
	for (int i = 0; i < count; i++) {
		float target = std::min(i * step, length);
		while (segment < points.size() - 1
			&& travelled + distance(points[segment - 1], points[segment])
				< target) {
			travelled += distance(points[segment - 1], points[segment]);
			segment++;
		}
		SwipePoint a = points[segment - 1], b = points[segment];
		float span = distance(a, b);
		float t = span > 0 ? std::min(1.0f, (target - travelled) / span) : 0;
		out.push_back({ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t });
	}
	return out;
}


/*!	The path a finger takes through \a keys: the straight one with its
	corners rounded off by a gaussian of \a sigma along it, resampled to
	\a count points. The ends stay put.
*/
static std::vector<SwipePoint>
finger_path(const std::vector<SwipePoint>& keys, float sigma, int count)
{
	const int kDense = 160;
	std::vector<SwipePoint> dense = resample(keys, kDense);
	float length = 0;
	for (size_t i = 1; i < keys.size(); i++)
		length += distance(keys[i - 1], keys[i]);
	float step = length / (kDense - 1);
	if (step == 0)
		return resample(keys, count);

	int radius = (int)(sigma * 2.5f / step);
	std::vector<SwipePoint> rounded(kDense);
	for (int i = 0; i < kDense; i++) {
		float x = 0, y = 0, total = 0;
		for (int j = -radius; j <= radius; j++) {
			SwipePoint point = dense[std::clamp(i + j, 0, kDense - 1)];
			float weight = expf(-(j * step) * (j * step) / (2 * sigma * sigma));
			x += point.x * weight;
			y += point.y * weight;
			total += weight;
		}
		rounded[i] = { x / total, y / total };
	}
	return resample(rounded, count);
}


SwipeDecoder::SwipeDecoder()
	:
	fKeyWidth(1),
	fRowHeight(0)
{
	std::fill(fHasKey, fHasKey + 26, false);
}


void
SwipeDecoder::SetKey(char letter, SwipePoint center)
{
	int index = tolower(letter) - 'a';
	if (index < 0 || index >= 26)
		return;
	fKeys[index] = center;
	fHasKey[index] = true;
	fPaths.clear();
}


void
SwipeDecoder::SetKeyWidth(float width)
{
	fKeyWidth = width;
	fPaths.clear();
}


void
SwipeDecoder::SetRowHeight(float height)
{
	fRowHeight = height;
	fPaths.clear();
}


std::vector<const char*>
SwipeDecoder::Decode(const std::vector<SwipePoint>& finger, size_t count) const
{
	std::vector<std::pair<float, const char*>> scored;
	if (finger.size() < 2)
		return {};

	// the rows spread out (or pulled together) to the pitch it knows
	float stretch = fRowHeight > 0 ? kRowPitch * fKeyWidth / fRowHeight : 1;
	std::vector<SwipePoint> path = finger;
	for (SwipePoint& point : path)
		point.y *= stretch;
	SwipePoint keyCenters[26];
	for (int i = 0; i < 26; i++)
		keyCenters[i] = { fKeys[i].x, fKeys[i].y * stretch };

	std::vector<SwipePoint> swipe = resample(path, kSamples);
	float slop = kEndSlop * fKeyWidth;
	if (fPaths.empty())
		fPaths.resize(kSwipeWordCount);

	std::vector<SwipePoint> keys;
	for (int w = 0; w < kSwipeWordCount; w++) {
		// The word's keys, a letter typed twice being one stop.
		keys.clear();
		bool known = true;
		char last = 0;
		for (const char* c = kSwipeWords[w].word; *c != '\0'; c++) {
			char letter = tolower(*c);
			if (letter == '\'' || letter == last)
				continue;
			int index = letter - 'a';
			if (index < 0 || index >= 26 || !fHasKey[index]) {
				known = false;
				break;
			}
			keys.push_back(keyCenters[index]);
			last = letter;
		}
		// A one-key word is a tap, not a swipe.
		if (!known || keys.size() < 2
			|| distance(keys.front(), swipe.front()) > slop
			|| distance(keys.back(), swipe.back()) > slop) {
			continue;
		}

		// How far the word's path, as a finger would swipe it, lies from
		// the finger's, on average, and how near the finger came to the key
		// it passed farthest from: a key never reached counts more than its
		// share of the rest.
		std::vector<SwipePoint>& ideal = fPaths[w];
		if (ideal.empty())
			ideal = finger_path(keys, kCornerCut * fKeyWidth, kSamples);
		float apart = 0;
		for (int i = 0; i < kSamples; i++)
			apart += distance(ideal[i], swipe[i]);
		apart /= kSamples * fKeyWidth;

		float missed = 0;
		for (SwipePoint key : keys) {
			float nearest = INFINITY;
			for (SwipePoint point : swipe)
				nearest = std::min(nearest, distance(key, point));
			missed = std::max(missed, nearest);
		}
		missed /= fKeyWidth;

		float cost = apart + kMissWeight * missed;
		scored.push_back({ -cost * cost / (2 * kSigma * kSigma)
			+ kWeightScale * kSwipeWords[w].weight, kSwipeWords[w].word });
	}

	count = std::min(count, scored.size());
	std::partial_sort(scored.begin(), scored.begin() + count, scored.end(),
		[](const auto& a, const auto& b) { return a.first > b.first; });
	std::vector<const char*> words;
	for (size_t i = 0; i < count; i++)
		words.push_back(scored[i].second);
	return words;
}
