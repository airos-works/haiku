/*
 * Copyright 2008, Andrej Spielmann <andrej.spielmann@seh.ox.ac.uk>.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

//! Global settings of the app server regarding antialiasing and font hinting

#include "GlobalSubpixelSettings.h"

#include <OS.h>

// NOTE: all these are initialized in DesktopSettings.cpp
bool gSubpixelAntialiasing;
uint8 gDefaultHintingMode;
float gRenderScale = 1.0f;
uint8 gSubpixelAverageWeight;
bool gSubpixelOrderingRGB;


static int32 sFontGeneration = 1;
static int32* sSharedFontGeneration = NULL;
	// in the memory that every application maps read-only


void
font_metrics_changed()
{
	int32 generation = atomic_add(&sFontGeneration, 1) + 1;
	if (generation == 0) {
		// 0 means "not maintained"
		generation = atomic_add(&sFontGeneration, 1) + 1;
	}
	if (sSharedFontGeneration != NULL)
		atomic_set(sSharedFontGeneration, generation);
}


void
set_shared_font_generation(int32* shared)
{
	sSharedFontGeneration = shared;
	if (shared != NULL)
		atomic_set(shared, atomic_get(&sFontGeneration));
}
