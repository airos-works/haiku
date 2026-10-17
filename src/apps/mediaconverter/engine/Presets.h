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
};


/*!	A short list in the spirit of HandBrake's general presets: a handful of
	everyday targets, a few for devices and the web, and audio only. */
namespace Presets {

int32			Count();
const Preset&	At(int32 index);
int32			IndexOf(const char* name);
int32			DefaultIndex();

}	// namespace Presets


#endif	// PRESETS_H
