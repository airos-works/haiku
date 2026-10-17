/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef PRESETS_H
#define PRESETS_H


#include "OutputSettings.h"


struct Preset {
	const char*		category;
	const char*		name;
	const char*		description;
	OutputSettings	settings;
	bool			sound;		// made for songs: sound in, sound out
};


/*!	A short list in the spirit of HandBrake's general presets: a handful of
	everyday targets, a few for devices and the web - and for songs MP3, AAC
	and FLAC at a few rates and a few others, which video sources offer as
	"Sound only". */
namespace Presets {

int32			Count();
const Preset&	At(int32 index);
int32			IndexOf(const char* name);
// The first preset of the kind this computer can make: for a song
// "MP3, 320 kbit/s" when it can.
int32			DefaultIndex(bool sound = false);

}	// namespace Presets


#endif	// PRESETS_H
