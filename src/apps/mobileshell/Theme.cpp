/*
 * Copyright 2026, air/OS.
 * Copyright 2026 Renku Mobile contributors.
 * Distributed under the terms of the MIT License.
 */


#include "Theme.h"

#include <Font.h>

#include <algorithm>


float
Theme::Scale()
{
	static float sScale = 0;
	if (sScale == 0)
		sScale = std::max(0.6f, be_plain_font->Size() / 12.0f * 0.8f);
	return sScale;
}


int32
Theme::GridColumns(float width)
{
	return std::clamp((int32)(width / S(165)), (int32)3, (int32)8);
}
