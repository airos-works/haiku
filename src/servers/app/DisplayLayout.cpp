/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "DisplayLayout.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <functional>

#include <edid.h>

#include "HWInterface.h"


static const uint16 kScales[] = { 100, 125, 150, 175, 200, 225, 250 };


static float
refresh_rate(const display_timing& timing)
{
	if (timing.h_total == 0 || timing.v_total == 0)
		return 0;
	return roundf(timing.pixel_clock * 1000.0f
		/ (timing.h_total * timing.v_total) * 10) / 10;
}


static void
add_timing(BMessage& into, const char* prefix, const display_timing& timing)
{
	BString name;
	into.AddInt32(name.SetToFormat("%s width", prefix), timing.h_display);
	into.AddInt32(name.SetToFormat("%s height", prefix), timing.v_display);
	into.AddFloat(name.SetToFormat("%s refresh", prefix),
		refresh_rate(timing));
}


static void
strip(BString& string)
{
	string.Trim();
}


// #pragma mark - DisplayInfo


float
DisplayInfo::RefreshRate() const
{
	return refresh_rate(timing);
}


float
DisplayInfo::NativeRefreshRate() const
{
	return refresh_rate(native);
}


float
DisplayInfo::DPI() const
{
	if (widthCM <= 0 || native.h_display == 0)
		return 0;
	return native.h_display / (widthCM / 2.54f);
}


// #pragma mark - DisplayLayout


DisplayLayout::DisplayLayout()
	:
	fRenderScaleOverride(0)
{
}


DisplayLayout::~DisplayLayout()
{
}


/*!	Reads what the hardware currently has: every connector with a monitor,
	its EDID, and the region and scale the accelerant drives it with, and
	which outputs mirror another. Anything the user decided about a display
	(whether it is primary) is carried over from the previous reading.
*/
status_t
DisplayLayout::ReadOutputs(HWInterface* interface)
{
	display_output* outputs = NULL;
	uint32 count = 0;
	status_t status = interface->GetDisplayOutputs(&outputs, &count);
	if (status != B_OK)
		return status;

	std::vector<DisplayInfo> previous;
	previous.swap(fDisplays);

	for (uint32 i = 0; i < count; i++) {
		const display_output& output = outputs[i];
		DisplayInfo display;
		display.id = output.id;
		display.name = output.name;
		display.flags = output.flags;
		display.renderScale = output.render_scale == 0
			? 100 : output.render_scale;
		display.scale = output.scale;
		// the region is in frame buffer pixels; frames are logical, and the
		// logical size follows from the timing and the scale exactly as the
		// accelerant computes it
		int32 logicalWidth = (output.timing.h_display * 100
			+ display.scale / 2) / display.scale;
		int32 logicalHeight = (output.timing.v_display * 100
			+ display.scale / 2) / display.scale;
		int32 left = (output.x * 100 + display.renderScale / 2)
			/ display.renderScale;
		int32 top = (output.y * 100 + display.renderScale / 2)
			/ display.renderScale;
		display.frame = BRect(left, top, left + logicalWidth - 1,
			top + logicalHeight - 1);
		display.native = output.native_timing;
		display.timing = output.timing;
		display.productID = 0;
		display.week = 0;
		display.year = 0;
		display.widthCM = 0;
		display.heightCM = 0;
		display.primary = false;
		display.hasEDID = false;
		display.pinned = false;
		display.mirrorOf = kNotMirrored;

		if (output.edid_length >= sizeof(edid1_raw)) {
			edid1_info edid;
			edid_decode(&edid, (const edid1_raw*)output.edid);
			display.hasEDID = true;
			display.vendor = edid.vendor.manufacturer;
			display.productID = edid.vendor.prod_id;
			display.week = edid.vendor.week;
			display.year = edid.vendor.year;
			display.widthCM = edid.display.h_size;
			display.heightCM = edid.display.v_size;
			if (edid.vendor.serial != 0)
				display.serial.SetToFormat("%" B_PRIu32, edid.vendor.serial);
			for (int32 j = 0; j < EDID1_NUM_DETAILED_MONITOR_DESC; j++) {
				const edid1_detailed_monitor& detailed
					= edid.detailed_monitor[j];
				switch (detailed.monitor_desc_type) {
					case EDID1_MONITOR_NAME:
						display.monitorName.SetTo(detailed.data.monitor_name,
							EDID1_EXTRA_STRING_LEN);
						strip(display.monitorName);
						break;
					case EDID1_SERIAL_NUMBER:
						display.serial.SetTo(detailed.data.serial_number,
							EDID1_EXTRA_STRING_LEN);
						strip(display.serial);
						break;
					case EDID1_IS_DETAILED_TIMING:
						if (detailed.data.detailed_timing.h_size > 0) {
							display.widthCM
								= detailed.data.detailed_timing.h_size / 10.0f;
							display.heightCM
								= detailed.data.detailed_timing.v_size / 10.0f;
						}
						break;
				}
			}
		}
		display.key = MakeKey(display);

		for (size_t j = 0; j < previous.size(); j++) {
			if (previous[j].id == display.id) {
				display.primary = previous[j].primary;
				break;
			}
		}
		fDisplays.push_back(display);
	}

	// A mirror shows the region of the output that starts where its own
	// region does.
	for (uint32 i = 0; i < count; i++) {
		if ((outputs[i].flags & B_DISPLAY_OUTPUT_MIRROR) == 0
			|| !fDisplays[i].IsEnabled())
			continue;
		for (uint32 j = 0; j < count; j++) {
			if (j == i || (outputs[j].flags & B_DISPLAY_OUTPUT_MIRROR) != 0
				|| !fDisplays[j].IsEnabled()
				|| outputs[j].x != outputs[i].x
				|| outputs[j].y != outputs[i].y)
				continue;
			fDisplays[i].mirrorOf = fDisplays[j].id;
			fDisplays[i].frame = fDisplays[j].frame;
			if (fDisplays[i].primary) {
				fDisplays[i].primary = false;
				fDisplays[j].primary = true;
			}
			break;
		}
	}
	free(outputs);

	if (PrimaryDisplay() == NULL) {
		for (size_t i = 0; i < fDisplays.size(); i++) {
			if (fDisplays[i].IsEnabled() && !fDisplays[i].IsMirror()) {
				fDisplays[i].primary = true;
				break;
			}
		}
	}
	return B_OK;
}


/*!	For hardware without layout support: the one monitor, covering the
	whole frame buffer.
*/
void
DisplayLayout::SetSingle(BRect frame, uint16 scale, const monitor_info* info,
	const display_timing* timing)
{
	DisplayInfo display;
	display.id = 0;
	display.name = "Display";
	display.flags = B_DISPLAY_OUTPUT_CONNECTED | B_DISPLAY_OUTPUT_ENABLED;
	display.frame = frame;
	display.scale = scale;
	display.renderScale = scale;
	display.primary = true;
	display.productID = 0;
	display.week = 0;
	display.year = 0;
	display.widthCM = 0;
	display.heightCM = 0;
	display.hasEDID = info != NULL;
	display.pinned = false;
	display.mirrorOf = kNotMirrored;
	memset(&display.native, 0, sizeof(display.native));
	memset(&display.timing, 0, sizeof(display.timing));
	display.native.h_display = display.timing.h_display
		= (uint16)(frame.IntegerWidth() + 1) * scale / 100;
	display.native.v_display = display.timing.v_display
		= (uint16)(frame.IntegerHeight() + 1) * scale / 100;
	if (timing != NULL)
		display.native = display.timing = *timing;
	if (info != NULL) {
		display.vendor = info->vendor;
		display.monitorName = info->name;
		display.serial = info->serial_number;
		display.productID = info->product_id;
		display.week = info->produced.week;
		display.year = info->produced.year;
		display.widthCM = info->width;
		display.heightCM = info->height;
	}
	display.key = MakeKey(display);

	fDisplays.clear();
	fDisplays.push_back(display);
}


/*!	Decides where every connected display goes, from what the settings
	remember about it, or from defaults for one never seen before. With
	\a keepCurrent, displays that are already enabled keep their present
	place and scale and only newcomers are placed, which is what a monitor
	being plugged in wants: nothing that is on screen moves.
	A display remembered as the mirror of another mirrors it again when that
	monitor is connected too.
*/
void
DisplayLayout::Configure(const BMessage& saved, bool keepCurrent)
{
	std::vector<BString> mirrorKeys(fDisplays.size());
	std::vector<BString> mirrorConnectors(fDisplays.size());

	bool anyPrimary = false;
	for (size_t i = 0; i < fDisplays.size(); i++) {
		DisplayInfo& display = fDisplays[i];
		display.pinned = false;
		if (!display.IsConnected()) {
			display.flags &= ~B_DISPLAY_OUTPUT_ENABLED;
			display.primary = false;
			display.mirrorOf = kNotMirrored;
			continue;
		}
		if (keepCurrent && display.IsEnabled()) {
			anyPrimary |= display.primary;
			continue;
		}
		display.mirrorOf = kNotMirrored;

		BMessage found;
		if (_FindSaved(saved, display, found) != NULL) {
			bool enabled;
			if (found.FindBool("enabled", &enabled) != B_OK)
				enabled = true;
			int32 x, y, scale;
			if (found.FindInt32("x", &x) != B_OK)
				x = 0;
			if (found.FindInt32("y", &y) != B_OK)
				y = 0;
			if (found.FindInt32("scale", &scale) != B_OK
				|| !IsValidScale(scale))
				scale = DefaultScale(display);
			int32 width, height;
			float refresh;
			display_timing timing = display.native;
			if (found.FindInt32("width", &width) == B_OK
				&& found.FindInt32("height", &height) == B_OK
				&& found.FindFloat("refresh", &refresh) == B_OK
				&& width > 0 && height > 0) {
				// The timing is looked up by the accelerant; only the size
				// and rate have to be right.
				memset(&timing, 0, sizeof(timing));
				timing.h_display = width;
				timing.v_display = height;
				timing.h_total = width;
				timing.v_total = height;
				timing.pixel_clock = (uint32)(refresh * width * height / 1000);
			}
			bool primary;
			if (found.FindBool("primary", &primary) != B_OK)
				primary = false;

			display.flags = (display.flags & ~B_DISPLAY_OUTPUT_ENABLED)
				| (enabled ? B_DISPLAY_OUTPUT_ENABLED : 0);
			display.scale = scale;
			display.timing = timing;
			display.primary = primary && enabled;
			const char* mirrorKey;
			const char* mirrorConnector;
			if (found.FindString("mirror key", &mirrorKey) == B_OK) {
				mirrorKeys[i] = mirrorKey;
				if (found.FindString("mirror connector", &mirrorConnector)
						== B_OK)
					mirrorConnectors[i] = mirrorConnector;
			}
			int32 width2 = (timing.h_display * 100 + scale / 2) / scale;
			int32 height2 = (timing.v_display * 100 + scale / 2) / scale;
			display.frame = BRect(x, y, x + width2 - 1, y + height2 - 1);
		} else {
			// Never seen: native resolution, a scale that suits its density;
			// it is placed below, once every remembered display has its
			// place.
			display.flags |= B_DISPLAY_OUTPUT_ENABLED;
			display.scale = DefaultScale(display);
			display.timing = display.native;
			display.primary = false;
			display.frame = BRect();
		}
		anyPrimary |= display.primary;
	}

	// The monitors the remembered mirrors show: the same monitor on the same
	// connector first, then the same monitor anywhere.
	for (size_t i = 0; i < fDisplays.size(); i++) {
		if (mirrorKeys[i].IsEmpty())
			continue;
		const DisplayInfo* source = NULL;
		for (size_t j = 0; j < fDisplays.size(); j++) {
			const DisplayInfo& other = fDisplays[j];
			if (j == i || !other.IsConnected() || other.key != mirrorKeys[i])
				continue;
			if (other.name == mirrorConnectors[i]) {
				source = &other;
				break;
			}
			if (source == NULL)
				source = &other;
		}
		if (source != NULL)
			fDisplays[i].mirrorOf = source->id;
	}

	// New displays go to the right of everything else, in connector order.
	float right = 0;
	for (size_t i = 0; i < fDisplays.size(); i++) {
		const DisplayInfo& display = fDisplays[i];
		if (display.IsConnected() && display.IsEnabled()
			&& !display.IsMirror() && display.frame.IsValid())
			right = std::max(right, display.frame.right + 1);
	}
	for (size_t i = 0; i < fDisplays.size(); i++) {
		DisplayInfo& display = fDisplays[i];
		if (!display.IsConnected() || display.frame.IsValid())
			continue;
		int32 width = (display.timing.h_display * 100 + display.scale / 2)
			/ display.scale;
		int32 height = (display.timing.v_display * 100 + display.scale / 2)
			/ display.scale;
		display.frame = BRect(right, 0, right + width - 1, height - 1);
		right += width;
	}

	// Something has to be on.
	bool anyEnabled = false;
	for (size_t i = 0; i < fDisplays.size(); i++)
		anyEnabled |= fDisplays[i].IsConnected() && fDisplays[i].IsEnabled();
	if (!anyEnabled) {
		for (size_t i = 0; i < fDisplays.size(); i++) {
			if (fDisplays[i].IsConnected()) {
				fDisplays[i].flags |= B_DISPLAY_OUTPUT_ENABLED;
				break;
			}
		}
	}
	if (!anyPrimary) {
		for (size_t i = 0; i < fDisplays.size(); i++) {
			if (fDisplays[i].IsConnected() && fDisplays[i].IsEnabled()) {
				fDisplays[i].primary = true;
				break;
			}
		}
	}

	_ResolveMirrors();
	_Separate();
	_CloseGaps();
	_PlaceMirrors();
	_Normalize();
}


/*!	Takes a layout a client asks for. The request holds one "display"
	message per display it wants changed, by "id": "enabled", "frame" (whose
	left/top is used), "scale", "mode width"/"mode height"/"mode refresh",
	"primary", and "mirror", the id of the display it is to show the same
	part of the desktop as, or -1 for its own. Displays not mentioned keep
	what they have.
	A mirror takes the place of its source, and its scale is what fits the
	source's part of the desktop on it. Making a mirror the main display
	makes it the source of the others instead.
*/
status_t
DisplayLayout::ApplyRequest(const BMessage& request)
{
	for (size_t i = 0; i < fDisplays.size(); i++)
		fDisplays[i].pinned = false;

	bool scaleChanged = false;
	bool positionChanged = false;

	// what was asked of mirrors, to tell whether it could be done
	std::vector<std::pair<uint32, uint32> > mirrorRequests;
	uint32 requestedPrimary = kNotMirrored;

	BMessage displayMessage;
	for (int32 i = 0; request.FindMessage("display", i, &displayMessage)
			== B_OK; i++) {
		int32 id;
		if (displayMessage.FindInt32("id", &id) != B_OK)
			return B_BAD_VALUE;
		DisplayInfo* display = _DisplayByID(id);
		if (display == NULL)
			return B_NAME_NOT_FOUND;
		if (!display->IsConnected())
			return B_DEV_NOT_READY;
		display->pinned = true;

		bool enabled;
		if (displayMessage.FindBool("enabled", &enabled) == B_OK) {
			display->flags = (display->flags & ~B_DISPLAY_OUTPUT_ENABLED)
				| (enabled ? B_DISPLAY_OUTPUT_ENABLED : 0);
		}
		int32 scale;
		if (displayMessage.FindInt32("scale", &scale) == B_OK) {
			if (!IsValidScale(scale))
				return B_BAD_VALUE;
			scaleChanged |= display->scale != scale;
			display->scale = scale;
		}
		int32 width, height;
		float refresh;
		if (displayMessage.FindInt32("mode width", &width) == B_OK
			&& displayMessage.FindInt32("mode height", &height) == B_OK) {
			if (displayMessage.FindFloat("mode refresh", &refresh) != B_OK)
				refresh = display->NativeRefreshRate();
			if (width <= 0 || height <= 0)
				return B_BAD_VALUE;
			display_timing timing;
			memset(&timing, 0, sizeof(timing));
			timing.h_display = width;
			timing.v_display = height;
			timing.h_total = width;
			timing.v_total = height;
			timing.pixel_clock = (uint32)(refresh * width * height / 1000);
			display->timing = timing;
		}
		bool wasMirror = display->IsMirror();
		int32 mirrorOf;
		if (displayMessage.FindInt32("mirror", &mirrorOf) == B_OK) {
			if (mirrorOf < 0) {
				// It keeps the size things had on it, in a scale it can
				// be set to.
				display->mirrorOf = kNotMirrored;
				display->scale = _ScaleStep(display->scale);
			} else {
				if ((uint32)mirrorOf == display->id
					|| _DisplayByID(mirrorOf) == NULL)
					return B_BAD_VALUE;
				display->mirrorOf = mirrorOf;
			}
			mirrorRequests.push_back(
				std::make_pair(display->id, display->mirrorOf));
		}

		BRect frame;
		BPoint origin;
		if (displayMessage.FindRect("frame", &frame) == B_OK)
			origin = frame.LeftTop();
		else if (displayMessage.FindPoint("origin", &origin) != B_OK) {
			origin = display->frame.LeftTop();
			if (wasMirror && !display->IsMirror()) {
				// Out of its source's place, to the right of the others;
				// the source is not to give way to it.
				origin.x = 0;
				origin.y = 0;
				for (size_t j = 0; j < fDisplays.size(); j++) {
					const DisplayInfo& other = fDisplays[j];
					if (&other != display && other.IsEnabled()
						&& !other.IsMirror())
						origin.x = std::max(origin.x, other.frame.right + 1);
				}
				display->pinned = false;
			}
		}

		positionChanged |= origin != display->frame.LeftTop();

		int32 logicalWidth = (display->timing.h_display * 100
			+ display->scale / 2) / display->scale;
		int32 logicalHeight = (display->timing.v_display * 100
			+ display->scale / 2) / display->scale;
		display->frame = BRect(origin.x, origin.y,
			origin.x + logicalWidth - 1, origin.y + logicalHeight - 1);

		bool primary;
		if (displayMessage.FindBool("primary", &primary) == B_OK
			&& primary) {
			for (size_t j = 0; j < fDisplays.size(); j++)
				fDisplays[j].primary = false;
			display->primary = true;
			requestedPrimary = display->id;
		}
	}

	// A mirror made the main display becomes the source of its group. (The
	// main display made a mirror hands that role to its source instead.)
	for (size_t i = 0; i < fDisplays.size(); i++) {
		DisplayInfo& display = fDisplays[i];
		if (display.id != requestedPrimary || !display.IsMirror())
			continue;
		DisplayInfo* source = _DisplayByID(display.mirrorOf);
		display.mirrorOf = kNotMirrored;
		if (source == NULL)
			break;
		for (size_t j = 0; j < fDisplays.size(); j++) {
			if (fDisplays[j].mirrorOf == source->id)
				fDisplays[j].mirrorOf = display.id;
		}
		source->mirrorOf = display.id;
		display.frame.OffsetTo(source->frame.LeftTop());
		display.pinned = true;
		display.scale = _ScaleStep(display.scale);
		int32 logicalWidth = (display.timing.h_display * 100
			+ display.scale / 2) / display.scale;
		int32 logicalHeight = (display.timing.v_display * 100
			+ display.scale / 2) / display.scale;
		display.frame.right = display.frame.left + logicalWidth - 1;
		display.frame.bottom = display.frame.top + logicalHeight - 1;
		break;
	}

	bool anyEnabled = false;
	for (size_t i = 0; i < fDisplays.size(); i++)
		anyEnabled |= fDisplays[i].IsConnected() && fDisplays[i].IsEnabled();
	if (!anyEnabled)
		return B_BAD_VALUE;

	const DisplayInfo* primary = PrimaryDisplay();
	if (primary == NULL || !primary->IsEnabled()) {
		for (size_t i = 0; i < fDisplays.size(); i++) {
			fDisplays[i].primary = fDisplays[i].IsConnected()
				&& fDisplays[i].IsEnabled() && !fDisplays[i].IsMirror()
				&& primary == NULL;
			if (fDisplays[i].primary)
				primary = &fDisplays[i];
		}
	}

	_ResolveMirrors();
	for (size_t i = 0; i < mirrorRequests.size(); i++) {
		const DisplayInfo* display = DisplayByID(mirrorRequests[i].first);
		if (display != NULL && display->IsEnabled()
			&& display->mirrorOf != mirrorRequests[i].second) {
			// The source is off or a mirror itself, or more of it would
			// have to be shown than the monitor has pixels.
			return B_BAD_VALUE;
		}
	}
	_Separate();
	// Scaling shrinks logical frames. Keep neighbouring displays together
	// unless the request also deliberately changes their positions.
	if (scaleChanged && !positionChanged)
		_CloseGaps();
	_PlaceMirrors();
	_Normalize();
	return B_OK;
}


void
DisplayLayout::GetConfigs(std::vector<display_output_config>& configs) const
{
	configs.clear();
	uint16 renderScale = RenderScale();
	for (size_t i = 0; i < fDisplays.size(); i++) {
		const DisplayInfo& display = fDisplays[i];
		if (!display.IsConnected())
			continue;
		display_output_config config;
		memset(&config, 0, sizeof(config));
		config.id = display.id;
		config.flags = display.IsEnabled() ? B_DISPLAY_OUTPUT_ENABLED : 0;
		if (display.IsEnabled() && display.IsMirror())
			config.flags |= B_DISPLAY_OUTPUT_MIRROR;
		config.x = (int32)display.frame.left;
		config.y = (int32)display.frame.top;
		config.scale = display.scale;
		config.render_scale = renderScale;
		config.timing = display.timing;
		configs.push_back(config);
	}
}


/*!	The settings: one "display" message per monitor ever configured, keyed
	by the monitor's identity. Displays the layout no longer has are kept
	from the previous settings, so that a monitor unplugged today comes back
	where it was.
*/
status_t
DisplayLayout::Store(BMessage& saved) const
{
	BMessage previous = saved;
	saved.MakeEmpty();
	saved.what = 'asdp';

	for (size_t i = 0; i < fDisplays.size(); i++) {
		const DisplayInfo& display = fDisplays[i];
		if (!display.IsConnected())
			continue;
		BMessage entry;
		entry.AddString("key", display.key);
		entry.AddString("connector", display.name);
		entry.AddBool("enabled", display.IsEnabled());
		entry.AddBool("primary", display.primary);
		entry.AddInt32("x", (int32)display.frame.left);
		entry.AddInt32("y", (int32)display.frame.top);
		entry.AddInt32("scale", display.scale);
		entry.AddInt32("width", display.timing.h_display);
		entry.AddInt32("height", display.timing.v_display);
		entry.AddFloat("refresh", display.RefreshRate());
		const DisplayInfo* source = display.IsMirror()
			? DisplayByID(display.mirrorOf) : NULL;
		if (source != NULL) {
			entry.AddString("mirror key", source->key);
			entry.AddString("mirror connector", source->name);
		}
		saved.AddMessage("display", &entry);
	}

	BMessage entry;
	for (int32 i = 0; previous.FindMessage("display", i, &entry) == B_OK;
			i++) {
		const char* key;
		const char* connector;
		if (entry.FindString("key", &key) != B_OK)
			continue;
		if (entry.FindString("connector", &connector) != B_OK)
			connector = "";
		bool present = false;
		for (size_t j = 0; j < fDisplays.size(); j++) {
			if (fDisplays[j].IsConnected() && fDisplays[j].key == key
				&& fDisplays[j].name == connector)
				present = true;
		}
		if (!present)
			saved.AddMessage("display", &entry);
	}
	return B_OK;
}


/*!	What clients see: every display with all that is known about it, plus
	the timings its monitor accepts.
*/
status_t
DisplayLayout::Archive(BMessage& into, HWInterface* interface) const
{
	for (size_t i = 0; i < fDisplays.size(); i++) {
		const DisplayInfo& display = fDisplays[i];
		BMessage entry;
		entry.AddInt32("id", display.id);
		entry.AddString("name", display.name);
		entry.AddString("key", display.key);
		entry.AddString("monitor", display.monitorName);
		entry.AddString("vendor", display.vendor);
		entry.AddString("serial", display.serial);
		entry.AddInt32("product id", display.productID);
		entry.AddInt32("week", display.week);
		entry.AddInt32("year", display.year);
		entry.AddFloat("width cm", display.widthCM);
		entry.AddFloat("height cm", display.heightCM);
		entry.AddBool("has edid", display.hasEDID);
		entry.AddInt32("flags", display.flags);
		entry.AddBool("connected", display.IsConnected());
		entry.AddBool("enabled", display.IsEnabled());
		entry.AddBool("primary", display.primary);
		entry.AddInt32("mirror", display.IsMirror()
			? (int32)display.mirrorOf : -1);
		entry.AddRect("frame", display.frame);
		entry.AddInt32("scale", display.scale);
		entry.AddInt32("render scale", display.renderScale);
		add_timing(entry, "native", display.native);
		add_timing(entry, "mode", display.timing);

		if (interface != NULL) {
			display_mode* modes = NULL;
			uint32 count = 0;
			if (interface->GetDisplayOutputModes(display.id, &modes, &count)
					== B_OK) {
				for (uint32 j = 0; j < count; j++) {
					BMessage mode;
					mode.AddInt32("width", modes[j].timing.h_display);
					mode.AddInt32("height", modes[j].timing.v_display);
					mode.AddFloat("refresh", refresh_rate(modes[j].timing));
					entry.AddMessage("modes", &mode);
				}
				free(modes);
			}
		}
		into.AddMessage("display", &entry);
	}
	into.AddRect("frame", Frame());
	for (size_t i = 0; i < sizeof(kScales) / sizeof(kScales[0]); i++)
		into.AddInt32("scales", kScales[i]);
	return B_OK;
}


const DisplayInfo*
DisplayLayout::DisplayAt(int32 index) const
{
	if (index < 0 || index >= (int32)fDisplays.size())
		return NULL;
	return &fDisplays[index];
}


const DisplayInfo*
DisplayLayout::DisplayByID(uint32 id) const
{
	for (size_t i = 0; i < fDisplays.size(); i++) {
		if (fDisplays[i].id == id)
			return &fDisplays[i];
	}
	return NULL;
}


const DisplayInfo*
DisplayLayout::PrimaryDisplay() const
{
	for (size_t i = 0; i < fDisplays.size(); i++) {
		if (fDisplays[i].primary && fDisplays[i].IsEnabled()
			&& !fDisplays[i].IsMirror())
			return &fDisplays[i];
	}
	return NULL;
}


/*!	The enabled display holding most of \a frame; when none holds any of
	it, the one nearest to its center. A mirror is never the answer: its
	source has the same frame.
*/
const DisplayInfo*
DisplayLayout::DisplayFor(BRect frame) const
{
	const DisplayInfo* best = NULL;
	float bestArea = 0;
	for (size_t i = 0; i < fDisplays.size(); i++) {
		const DisplayInfo& display = fDisplays[i];
		if (!display.IsEnabled() || display.IsMirror())
			continue;
		BRect common = display.frame & frame;
		if (!common.IsValid())
			continue;
		float area = (common.Width() + 1) * (common.Height() + 1);
		if (best == NULL || area > bestArea) {
			best = &display;
			bestArea = area;
		}
	}
	if (best != NULL)
		return best;
	return DisplayNearest(BPoint((frame.left + frame.right) / 2,
		(frame.top + frame.bottom) / 2));
}


const DisplayInfo*
DisplayLayout::DisplayNearest(BPoint point) const
{
	const DisplayInfo* best = NULL;
	float bestDistance = 0;
	for (size_t i = 0; i < fDisplays.size(); i++) {
		const DisplayInfo& display = fDisplays[i];
		if (!display.IsEnabled() || display.IsMirror())
			continue;
		float dx = 0, dy = 0;
		if (point.x < display.frame.left)
			dx = display.frame.left - point.x;
		else if (point.x > display.frame.right)
			dx = point.x - display.frame.right;
		if (point.y < display.frame.top)
			dy = display.frame.top - point.y;
		else if (point.y > display.frame.bottom)
			dy = point.y - display.frame.bottom;
		float distance = dx * dx + dy * dy;
		if (best == NULL || distance < bestDistance) {
			best = &display;
			bestDistance = distance;
		}
	}
	return best;
}


BRect
DisplayLayout::Frame() const
{
	BRect frame;
	for (size_t i = 0; i < fDisplays.size(); i++) {
		if (!fDisplays[i].IsEnabled())
			continue;
		if (!frame.IsValid())
			frame = fDisplays[i].frame;
		else
			frame = frame | fDisplays[i].frame;
	}
	return frame;
}


BRegion
DisplayLayout::Region() const
{
	BRegion region;
	for (size_t i = 0; i < fDisplays.size(); i++) {
		if (fDisplays[i].IsEnabled())
			region.Include(fDisplays[i].frame);
	}
	return region;
}


/*!	The density everything is drawn at, unless one was set: the largest
	display scale. Every display then gets at least as many pixels as it
	shows - the most scaled ones one to one, the others shrunk by the display
	engine - so that 4K monitors at 200 percent stay crisp beside a 1080p one
	at 100. Not every engine can shrink every region; the Desktop tries
	RenderScales() in turn and keeps the first one the hardware takes.
*/
uint16
DisplayLayout::RenderScale() const
{
	if (fRenderScaleOverride != 0)
		return fRenderScaleOverride;
	std::vector<uint16> scales;
	RenderScales(scales);
	return scales[0];
}


/*!	The densities to draw at, best first: the scale of every enabled
	display, from the largest down. At the smallest, displays are only ever
	enlarged, which every display engine with a scaler can do.
*/
void
DisplayLayout::RenderScales(std::vector<uint16>& scales) const
{
	scales.clear();
	for (size_t i = 0; i < fDisplays.size(); i++) {
		if (!fDisplays[i].IsEnabled())
			continue;
		if (std::find(scales.begin(), scales.end(), fDisplays[i].scale)
				== scales.end())
			scales.push_back(fDisplays[i].scale);
	}
	std::sort(scales.begin(), scales.end(), std::greater<uint16>());
	if (scales.empty())
		scales.push_back(100);
}


void
DisplayLayout::SetRenderScale(uint16 renderScale)
{
	fRenderScaleOverride = renderScale;
}


bool
DisplayLayout::HasScaledDisplay() const
{
	for (size_t i = 0; i < fDisplays.size(); i++) {
		if (fDisplays[i].IsEnabled() && fDisplays[i].scale != 100)
			return true;
	}
	return false;
}


bool
DisplayLayout::HasMirror() const
{
	for (size_t i = 0; i < fDisplays.size(); i++) {
		if (fDisplays[i].IsEnabled() && fDisplays[i].IsMirror())
			return true;
	}
	return false;
}


/*!	A scale to start a monitor at, from its pixel density: a 4K monitor of
	24 inches is nearly 190 dots per inch, twice what most programs and
	fonts were sized for.
*/
/*static*/ uint16
DisplayLayout::DefaultScale(const DisplayInfo& display)
{
	float dpi = display.DPI();
	if (dpi >= 175)
		return 200;
	if (dpi >= 130)
		return 150;
	return 100;
}


uint16
DisplayLayout::SavedScale(const BMessage& saved) const
{
	if (fDisplays.empty())
		return 100;
	const DisplayInfo& display = fDisplays[0];
	BMessage found;
	int32 scale;
	if (_FindSaved(saved, display, found) != NULL
		&& found.FindInt32("scale", &scale) == B_OK && IsValidScale(scale))
		return scale;
	return DefaultScale(display);
}


/*static*/ BString
DisplayLayout::MakeKey(const DisplayInfo& display)
{
	BString key;
	if (display.hasEDID) {
		key.SetToFormat("%s:%04" B_PRIx32 ":%s:%s", display.vendor.String(),
			display.productID, display.serial.String(),
			display.monitorName.String());
	} else
		key.SetToFormat("connector:%s", display.name.String());
	return key;
}


/*static*/ bool
DisplayLayout::IsValidScale(uint16 scale)
{
	for (size_t i = 0; i < sizeof(kScales) / sizeof(kScales[0]); i++) {
		if (kScales[i] == scale)
			return true;
	}
	return false;
}


// #pragma mark - private


DisplayInfo*
DisplayLayout::_DisplayByID(uint32 id)
{
	for (size_t i = 0; i < fDisplays.size(); i++) {
		if (fDisplays[i].id == id)
			return &fDisplays[i];
	}
	return NULL;
}


/*!	A mirror needs a source that is enabled and not a mirror itself (a
	mirror of a mirror shows what that one shows), and a monitor with at
	least as many pixels as the source's part of the desktop: the display
	engines enlarge but never shrink. A display without one shows its own
	part of the desktop again. The main display is never a mirror; its
	source is the main display instead.
*/
void
DisplayLayout::_ResolveMirrors()
{
	for (size_t i = 0; i < fDisplays.size(); i++) {
		DisplayInfo& display = fDisplays[i];
		if (!display.IsMirror())
			continue;

		const DisplayInfo* source = DisplayByID(display.mirrorOf);
		for (size_t hops = 0; source != NULL && source->IsMirror()
				&& source != &display && hops < fDisplays.size(); hops++)
			source = DisplayByID(source->mirrorOf);

		if (source == NULL || source == &display || source->IsMirror()
			|| !source->IsConnected() || !source->IsEnabled()
			|| !display.IsConnected() || !display.IsEnabled()
			|| _MirrorScale(display, *source) < 100) {
			display.mirrorOf = kNotMirrored;
			continue;
		}
		display.mirrorOf = source->id;
	}

	for (size_t i = 0; i < fDisplays.size(); i++) {
		DisplayInfo& display = fDisplays[i];
		if (!display.IsMirror() || !display.primary)
			continue;
		display.primary = false;
		DisplayInfo* source = _DisplayByID(display.mirrorOf);
		if (source != NULL)
			source->primary = true;
	}

	_PlaceMirrors();
}


/*!	The largest scale a user can choose that is not above \a scale. */
/*static*/ uint16
DisplayLayout::_ScaleStep(uint16 scale)
{
	uint16 step = kScales[0];
	for (size_t i = 0; i < sizeof(kScales) / sizeof(kScales[0]); i++) {
		if (kScales[i] <= scale)
			step = kScales[i];
	}
	return step;
}


/*!	Every mirror takes its source's frame, and the scale that fits it. */
void
DisplayLayout::_PlaceMirrors()
{
	for (size_t i = 0; i < fDisplays.size(); i++) {
		DisplayInfo& display = fDisplays[i];
		if (!display.IsMirror())
			continue;
		const DisplayInfo* source = DisplayByID(display.mirrorOf);
		if (source == NULL)
			continue;
		display.frame = source->frame;
		display.scale = _MirrorScale(display, *source);
	}
}


/*!	How much \a mirror enlarges its source's part of the desktop: as much as
	fits its mode in both directions. A monitor like the source's gets the
	source's own scale; one of another shape shows black bars.
*/
/*static*/ uint16
DisplayLayout::_MirrorScale(const DisplayInfo& mirror,
	const DisplayInfo& source)
{
	if (mirror.timing.h_display == source.timing.h_display
		&& mirror.timing.v_display == source.timing.v_display)
		return source.scale;

	float width = source.frame.Width() + 1;
	float height = source.frame.Height() + 1;
	if (width < 1 || height < 1 || mirror.timing.h_display == 0
		|| mirror.timing.v_display == 0)
		return source.scale;

	float fit = std::min(mirror.timing.h_display / width,
		mirror.timing.v_display / height);
	return (uint16)std::min(400.0f, floorf(fit * 100 + 0.5f));
}


/*!	Finds what the settings remember about \a display: the same monitor on
	the same connector first, then the same monitor anywhere.
*/
const BMessage*
DisplayLayout::_FindSaved(const BMessage& saved, const DisplayInfo& display,
	BMessage& found) const
{
	BMessage entry;
	bool haveAny = false;
	for (int32 i = 0; saved.FindMessage("display", i, &entry) == B_OK; i++) {
		const char* key;
		if (entry.FindString("key", &key) != B_OK || display.key != key)
			continue;
		const char* connector;
		if (entry.FindString("connector", &connector) == B_OK
			&& display.name == connector) {
			found = entry;
			return &found;
		}
		if (!haveAny) {
			found = entry;
			haveAny = true;
		}
	}
	return haveAny ? &found : NULL;
}


/*!	Enabled displays must not overlap, mirrors aside: one that does is moved
	right until it does not. Displays the current request placed stay where they were
	put; the others give way, in their order from left to right. Moving one
	display onto another therefore pushes the other aside, which is how a
	swap is asked for with a single move.
*/
void
DisplayLayout::_Separate()
{
	std::vector<DisplayInfo*> enabled;
	for (size_t i = 0; i < fDisplays.size(); i++) {
		if (fDisplays[i].IsConnected() && fDisplays[i].IsEnabled()
			&& !fDisplays[i].IsMirror())
			enabled.push_back(&fDisplays[i]);
	}
	std::stable_sort(enabled.begin(), enabled.end(),
		[](const DisplayInfo* a, const DisplayInfo* b) {
			if (a->pinned != b->pinned)
				return a->pinned;
			if (a->frame.left != b->frame.left)
				return a->frame.left < b->frame.left;
			return a->frame.top < b->frame.top;
		});

	for (size_t i = 1; i < enabled.size(); i++) {
		for (size_t j = 0; j < i; j++) {
			BRect common = enabled[i]->frame & enabled[j]->frame;
			if (common.IsValid() && common.Width() >= 0
				&& common.Height() >= 0) {
				float shift = enabled[j]->frame.right + 1
					- enabled[i]->frame.left;
				enabled[i]->frame.OffsetBy(shift, 0);
			}
		}
	}
}


/*!	Whether two frames share part of an edge, or overlap: the mouse can go
	from one to the other. Meeting at a corner is not enough.
*/
static bool
touches(const BRect& a, const BRect& b)
{
	bool overlapX = a.left <= b.right && b.left <= a.right;
	bool overlapY = a.top <= b.bottom && b.top <= a.bottom;
	bool adjacentX = a.left <= b.right + 1 && b.left <= a.right + 1;
	bool adjacentY = a.top <= b.bottom + 1 && b.top <= a.bottom + 1;
	return (overlapX && adjacentY) || (overlapY && adjacentX);
}


/*!	Enabled displays that remember places beside a monitor that is not there
	now would leave a hole in the desktop the mouse cannot cross. Going from
	the top left, each display that touches none before it moves left or up
	to the nearest one, or, when none is beside or below it, next to the
	rightmost one.
*/
void
DisplayLayout::_CloseGaps()
{
	std::vector<DisplayInfo*> enabled;
	for (size_t i = 0; i < fDisplays.size(); i++) {
		if (fDisplays[i].IsConnected() && fDisplays[i].IsEnabled()
			&& !fDisplays[i].IsMirror())
			enabled.push_back(&fDisplays[i]);
	}
	std::stable_sort(enabled.begin(), enabled.end(),
		[](const DisplayInfo* a, const DisplayInfo* b) {
			if (a->frame.left != b->frame.left)
				return a->frame.left < b->frame.left;
			return a->frame.top < b->frame.top;
		});

	bool moved = false;
	for (size_t i = 1; i < enabled.size(); i++) {
		BRect& frame = enabled[i]->frame;
		bool connected = false;
		float shiftX = -1;
		float shiftY = -1;
		const DisplayInfo* rightmost = NULL;
		for (size_t j = 0; j < i && !connected; j++) {
			const BRect& other = enabled[j]->frame;
			if (touches(frame, other)) {
				connected = true;
				break;
			}
			if (rightmost == NULL || other.right > rightmost->frame.right)
				rightmost = enabled[j];
			if (frame.top <= other.bottom && other.top <= frame.bottom
				&& other.right < frame.left) {
				float gap = frame.left - other.right - 1;
				if (shiftX < 0 || gap < shiftX)
					shiftX = gap;
			}
			if (frame.left <= other.right && other.left <= frame.right
				&& other.bottom < frame.top) {
				float gap = frame.top - other.bottom - 1;
				if (shiftY < 0 || gap < shiftY)
					shiftY = gap;
			}
		}
		if (connected)
			continue;

		if (shiftX >= 0 && (shiftY < 0 || shiftX <= shiftY))
			frame.OffsetBy(-shiftX, 0);
		else if (shiftY >= 0)
			frame.OffsetBy(0, -shiftY);
		else if (rightmost != NULL) {
			frame.OffsetTo(rightmost->frame.right + 1,
				rightmost->frame.top);
		}
		moved = true;
	}

	// Closing up cannot put two displays on top of each other in a row or a
	// column; anything more tangled is still kept apart.
	if (moved)
		_Separate();
}


/*!	The frame buffer starts at the top left of the enabled displays.
*/
void
DisplayLayout::_Normalize()
{
	BRect frame = Frame();
	if (!frame.IsValid())
		return;
	for (size_t i = 0; i < fDisplays.size(); i++)
		fDisplays[i].frame.OffsetBy(-frame.left, -frame.top);
}
