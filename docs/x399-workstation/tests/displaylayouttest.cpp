/*
 * displaylayouttest - app_server's DisplayLayout against a pretend
 * accelerant, through what happens to monitors between boots: arranged by
 * the user, one of them asleep or late at boot, and back again.
 *
 * The first case is the one that lost the arrangement on the workstation:
 * two monitors swapped, the left one missing for a moment. Everything an
 * automatic reconfiguration decided used to be stored, so the right monitor's
 * stand-in place at x = 0 was remembered, and once the left one came back
 * both claimed x = 0 and connector order won. The same case run the old way
 * is kept as a check that the test still sees that failure.
 *
 * The mirror cases follow: one monitor showing what another shows, kept
 * across boots and unplugging, the main display deciding which is which,
 * and monitors of other sizes enlarging, or refused when they would have
 * to shrink.
 *
 * Build and run: tools/build-displaylayout-test.sh
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include <Message.h>
#include <edid.h>

#include "DisplayLayout.h"
#include "HWInterface.h"


extern "C" void
edid_decode(edid1_info* edid, const edid1_raw* raw)
{
	// the pretend monitors have no EDID
	memset(edid, 0, sizeof(*edid));
}


struct FakeOutput {
	uint32	id;
	char	name[16];
	bool	connected;
	bool	enabled;
	bool	mirror;
	int32	x;			// frame buffer pixels
	int32	y;
	uint16	scale;
	uint16	renderScale;
	display_timing native;
	display_timing timing;
};


static display_timing
uhd_timing()
{
	display_timing timing = {};
	timing.pixel_clock = 533250;
	timing.h_display = 3840;
	timing.h_total = 4000;
	timing.v_display = 2160;
	timing.v_total = 2222;
	return timing;
}


static display_timing
fhd_timing()
{
	display_timing timing = {};
	timing.pixel_clock = 148500;
	timing.h_display = 1920;
	timing.h_total = 2200;
	timing.v_display = 1080;
	timing.v_total = 1125;
	return timing;
}


// Places outputs and works out their regions the way the nvidia_rm
// accelerant does; a mirror's region is its source's. Like the GTX 1070's
// display engine, it shrinks a region onto a monitor up to 1920 pixels
// wide and refuses to shrink one onto anything wider.
class FakeAccelerant : public HWInterface {
public:
	static const uint16 kWidestShrunk = 1920;

	void Add(uint32 id, const char* name,
		display_timing native = uhd_timing())
	{
		FakeOutput output = {};
		output.id = id;
		strlcpy(output.name, name, sizeof(output.name));
		output.connected = true;
		output.scale = 100;
		output.renderScale = 100;
		output.native = native;
		output.timing = native;
		fOutputs.push_back(output);
	}

	void SetConnected(uint32 id, bool connected)
	{
		for (FakeOutput& output : fOutputs) {
			if (output.id == id) {
				output.connected = connected;
				if (!connected)
					output.enabled = false;
			}
		}
	}

	status_t GetDisplayOutputs(display_output** _outputs, uint32* _count)
		override
	{
		display_output* outputs = (display_output*)calloc(fOutputs.size(),
			sizeof(display_output));
		for (size_t i = 0; i < fOutputs.size(); i++) {
			const FakeOutput& fake = fOutputs[i];
			display_output& output = outputs[i];
			output.id = fake.id;
			strlcpy(output.name, fake.name, sizeof(output.name));
			output.flags = (fake.connected ? B_DISPLAY_OUTPUT_CONNECTED : 0)
				| (fake.enabled ? B_DISPLAY_OUTPUT_ENABLED : 0)
				| (fake.enabled && fake.mirror ? B_DISPLAY_OUTPUT_MIRROR : 0);
			output.x = fake.x;
			output.y = fake.y;
			output.width = fake.timing.h_display * fake.renderScale
				/ fake.scale;
			output.height = fake.timing.v_display * fake.renderScale
				/ fake.scale;
			output.scale = fake.scale;
			output.render_scale = fake.renderScale;
			output.native_timing = fake.native;
			output.timing = fake.timing;
		}
		*_outputs = outputs;
		*_count = fOutputs.size();
		return B_OK;
	}

	status_t GetDisplayOutputModes(uint32, display_mode** _modes,
		uint32* _count) override
	{
		*_modes = NULL;
		*_count = 0;
		return B_OK;
	}

	status_t Apply(const std::vector<display_output_config>& configs)
	{
		for (const display_output_config& config : configs) {
			const FakeOutput* output = Output(config.id);
			if (output == NULL || (config.flags & B_DISPLAY_OUTPUT_ENABLED) == 0)
				continue;
			uint16 renderScale = config.render_scale == 0
				? 100 : config.render_scale;
			uint16 width = config.timing.h_display != 0
				? config.timing.h_display : output->native.h_display;
			if (renderScale > config.scale && width > kWidestShrunk)
				return B_BAD_VALUE;
		}
		for (FakeOutput& output : fOutputs) {
			output.enabled = false;
			output.mirror = false;
			for (const display_output_config& config : configs) {
				if (config.id != output.id || !output.connected)
					continue;
				output.enabled = (config.flags & B_DISPLAY_OUTPUT_ENABLED) != 0;
				output.mirror = (config.flags & B_DISPLAY_OUTPUT_MIRROR) != 0;
				output.renderScale = config.render_scale == 0
					? 100 : config.render_scale;
				output.scale = config.scale;
				output.x = config.x * output.renderScale / 100;
				output.y = config.y * output.renderScale / 100;
				output.timing = config.timing.h_display != 0
					? config.timing : output.native;
			}
		}
		return B_OK;
	}

	const FakeOutput* Output(uint32 id) const
	{
		for (const FakeOutput& output : fOutputs) {
			if (output.id == id)
				return &output;
		}
		return NULL;
	}

private:
	std::vector<FakeOutput> fOutputs;
};


// What Desktop::_SetDisplayLayout() does: the densest render scale the
// hardware takes.
static status_t
apply(DisplayLayout& layout, FakeAccelerant& hardware)
{
	std::vector<uint16> renderScales;
	layout.RenderScales(renderScales);
	for (size_t i = 0; i < renderScales.size(); i++) {
		layout.SetRenderScale(renderScales[i]);
		std::vector<display_output_config> configs;
		layout.GetConfigs(configs);
		if (hardware.Apply(configs) == B_OK)
			return B_OK;
	}
	return B_BAD_VALUE;
}


// What Desktop::_ConfigureDisplayLayout() does at boot, on a hot plug and
// after a resume; the old version stored the result as well.
static void
configure(DisplayLayout& layout, FakeAccelerant& hardware, BMessage& saved,
	bool storeLikeBefore)
{
	layout.ReadOutputs(&hardware);
	layout.Configure(saved, false);
	apply(layout, hardware);
	layout.ReadOutputs(&hardware);
	if (storeLikeBefore)
		layout.Store(saved);
}


// What Desktop::SetDisplayLayout() does for the Screen preferences.
static void
request(DisplayLayout& layout, FakeAccelerant& hardware, BMessage& saved,
	uint32 id, float x, float y)
{
	BMessage message;
	BMessage display;
	display.AddInt32("id", id);
	display.AddPoint("origin", BPoint(x, y));
	message.AddMessage("display", &display);
	if (layout.ApplyRequest(message) != B_OK) {
		printf("  request refused\n");
		return;
	}
	apply(layout, hardware);
	layout.ReadOutputs(&hardware);
	layout.Store(saved);
}


// A request with one display message, as Screen or screenmode send it,
// worked out on a copy the way Desktop::SetDisplayLayout() does.
static status_t
request_message(DisplayLayout& layout, FakeAccelerant& hardware,
	BMessage& saved, const BMessage& display)
{
	BMessage message;
	message.AddMessage("display", &display);
	DisplayLayout changed = layout;
	status_t status = changed.ApplyRequest(message);
	if (status != B_OK)
		return status;
	status = apply(changed, hardware);
	if (status != B_OK)
		return status;
	layout = changed;
	layout.ReadOutputs(&hardware);
	layout.Store(saved);
	return B_OK;
}


static status_t
request_mirror(DisplayLayout& layout, FakeAccelerant& hardware,
	BMessage& saved, uint32 id, int32 mirrorOf)
{
	BMessage display;
	display.AddInt32("id", id);
	display.AddInt32("mirror", mirrorOf);
	return request_message(layout, hardware, saved, display);
}


static int sFailures = 0;


static void
check(bool ok, const char* what)
{
	printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		sFailures++;
}


static void
expect_mirror(const DisplayLayout& layout, uint32 id, uint32 source,
	const char* what)
{
	const DisplayInfo* display = layout.DisplayByID(id);
	const DisplayInfo* other = layout.DisplayByID(source);
	bool ok = display != NULL && other != NULL && display->IsEnabled()
		&& display->mirrorOf == source && display->frame == other->frame;
	printf("  %s %s: %s mirrors %s\n", ok ? "ok  " : "FAIL", what,
		display != NULL ? display->name.String() : "?",
		other != NULL ? other->name.String() : "?");
	if (!ok)
		sFailures++;
}


static void
expect_own(const DisplayLayout& layout, uint32 id, const char* what)
{
	const DisplayInfo* display = layout.DisplayByID(id);
	check(display != NULL && display->IsEnabled() && !display->IsMirror(),
		what);
}


static void
expect(const DisplayLayout& layout, uint32 id, float x, float y,
	const char* what)
{
	const DisplayInfo* display = layout.DisplayByID(id);
	if (display == NULL || !display->IsEnabled()) {
		printf("  FAIL %s: display %" B_PRIu32 " is not on\n", what, id);
		sFailures++;
		return;
	}
	bool ok = display->frame.left == x && display->frame.top == y;
	printf("  %s %s: %s at %g,%g (want %g,%g)\n", ok ? "ok  " : "FAIL", what,
		display->name.String(), display->frame.left, display->frame.top, x, y);
	if (!ok)
		sFailures++;
}


static void
swapped_pair(bool storeLikeBefore)
{
	printf("%s: two monitors swapped, the left one missing for a while\n",
		storeLikeBefore ? "old behaviour" : "swap");
	FakeAccelerant hardware;
	hardware.Add(1, "DP-2");
	hardware.Add(2, "DP-4");
	DisplayLayout layout;
	BMessage saved;

	configure(layout, hardware, saved, storeLikeBefore);
	expect(layout, 1, 0, 0, "first boot, connector order");
	expect(layout, 2, 3840, 0, "first boot, connector order");

	// dragging DP-4 onto DP-2 pushes DP-2 aside
	request(layout, hardware, saved, 2, 0, 0);
	expect(layout, 2, 0, 0, "swapped");
	expect(layout, 1, 3840, 0, "swapped");

	// DP-4 asleep, or not answering yet at boot
	hardware.SetConnected(2, false);
	configure(layout, hardware, saved, storeLikeBefore);
	expect(layout, 1, 0, 0, "left monitor gone");

	hardware.SetConnected(2, true);
	configure(layout, hardware, saved, storeLikeBefore);
	if (storeLikeBefore) {
		const DisplayInfo* left = layout.DisplayByID(1);
		bool lost = left != NULL && left->frame.left == 0;
		printf("  %s old behaviour loses the swap\n", lost ? "ok  " : "FAIL");
		if (!lost)
			sFailures++;
		return;
	}
	expect(layout, 2, 0, 0, "left monitor back");
	expect(layout, 1, 3840, 0, "left monitor back");

	// and a boot where the file is read afresh
	DisplayLayout booted;
	configure(booted, hardware, saved, false);
	expect(booted, 2, 0, 0, "after a reboot");
	expect(booted, 1, 3840, 0, "after a reboot");
}


static void
row_of_three()
{
	printf("row: three monitors side by side, the middle one missing\n");
	FakeAccelerant hardware;
	hardware.Add(1, "DP-0");
	hardware.Add(2, "DP-2");
	hardware.Add(3, "DP-4");
	DisplayLayout layout;
	BMessage saved;
	configure(layout, hardware, saved, false);
	// store the arrangement the way the user would
	request(layout, hardware, saved, 1, 0, 0);
	expect(layout, 3, 7680, 0, "all three");

	hardware.SetConnected(2, false);
	configure(layout, hardware, saved, false);
	expect(layout, 1, 0, 0, "middle missing");
	expect(layout, 3, 3840, 0, "middle missing, closed up");

	hardware.SetConnected(2, true);
	configure(layout, hardware, saved, false);
	expect(layout, 2, 3840, 0, "middle back");
	expect(layout, 3, 7680, 0, "middle back");
}


static void
column_of_three()
{
	printf("column: three monitors stacked, the middle one missing\n");
	FakeAccelerant hardware;
	hardware.Add(1, "DP-0");
	hardware.Add(2, "DP-2");
	hardware.Add(3, "DP-4");
	DisplayLayout layout;
	BMessage saved;
	configure(layout, hardware, saved, false);
	request(layout, hardware, saved, 2, 0, 2160);
	request(layout, hardware, saved, 3, 0, 4320);
	expect(layout, 1, 0, 0, "stacked");
	expect(layout, 2, 0, 2160, "stacked");
	expect(layout, 3, 0, 4320, "stacked");

	hardware.SetConnected(2, false);
	configure(layout, hardware, saved, false);
	expect(layout, 1, 0, 0, "middle missing");
	expect(layout, 3, 0, 2160, "middle missing, closed up");

	hardware.SetConnected(2, true);
	configure(layout, hardware, saved, false);
	expect(layout, 2, 0, 2160, "middle back");
	expect(layout, 3, 0, 4320, "middle back");
}


static void
mirror_pair()
{
	printf("mirror: two alike monitors showing the same\n");
	FakeAccelerant hardware;
	hardware.Add(1, "DP-2");
	hardware.Add(2, "DP-4");
	DisplayLayout layout;
	BMessage saved;
	configure(layout, hardware, saved, false);

	check(request_mirror(layout, hardware, saved, 2, 1) == B_OK,
		"mirror requested");
	expect_mirror(layout, 2, 1, "requested");
	expect(layout, 1, 0, 0, "source stays");
	check(layout.Frame() == BRect(0, 0, 3839, 2159),
		"the desktop is one monitor");
	check(hardware.Output(2)->mirror && !hardware.Output(1)->mirror
		&& hardware.Output(2)->x == 0 && hardware.Output(2)->y == 0,
		"the accelerant got the mirror at the source's place");
	check(layout.PrimaryDisplay() != NULL
		&& layout.PrimaryDisplay()->id == 1, "the source is the main display");
	check(layout.DisplayFor(BRect(10, 10, 100, 100))->id == 1,
		"windows belong to the source");

	DisplayLayout booted;
	configure(booted, hardware, saved, false);
	expect_mirror(booted, 2, 1, "after a reboot");

	// the source asleep: the mirror shows the desktop by itself
	hardware.SetConnected(1, false);
	configure(booted, hardware, saved, false);
	expect_own(booted, 2, "source gone, the mirror is on its own");
	expect(booted, 2, 0, 0, "source gone");
	hardware.SetConnected(1, true);
	configure(booted, hardware, saved, false);
	expect_mirror(booted, 2, 1, "source back");

	// making the mirror the main display swaps the roles
	BMessage primary;
	primary.AddInt32("id", 2);
	primary.AddBool("primary", true);
	check(request_message(booted, hardware, saved, primary) == B_OK,
		"main display requested on the mirror");
	expect_mirror(booted, 1, 2, "roles swapped");
	check(booted.PrimaryDisplay() != NULL
		&& booted.PrimaryDisplay()->id == 2, "the new source is main");

	check(request_mirror(booted, hardware, saved, 1, -1) == B_OK,
		"mirror turned off");
	expect_own(booted, 1, "own part again");
	expect(booted, 2, 0, 0, "source keeps its place");
	expect(booted, 1, 3840, 0, "the former mirror goes to the right");

	DisplayLayout again;
	configure(again, hardware, saved, false);
	expect_own(again, 1, "still on its own after a reboot");
	expect(again, 1, 3840, 0, "still on the right after a reboot");
}


static void
mirror_sizes()
{
	printf("mirror: monitors of other sizes, shapes and scales\n");
	FakeAccelerant hardware;
	hardware.Add(1, "HDMI-1", fhd_timing());
	hardware.Add(2, "DP-1", uhd_timing());
	DisplayLayout layout;
	BMessage saved;
	configure(layout, hardware, saved, false);

	check(request_mirror(layout, hardware, saved, 2, 1) == B_OK,
		"4K mirrors 1080p");
	expect_mirror(layout, 2, 1, "4K mirrors 1080p");
	check(layout.DisplayByID(2)->scale == 200, "the 4K monitor enlarges 2x");
	check(layout.RenderScale() == 200
		&& hardware.Output(1)->renderScale == 200,
		"drawn at the 4K mirror's density, the 1080p source shrunk");

	check(request_mirror(layout, hardware, saved, 2, -1) == B_OK,
		"mirror off");
	check(layout.DisplayByID(2)->scale == 200,
		"the 4K monitor keeps showing things at the size it did");

	BMessage scale;
	scale.AddInt32("id", 2);
	scale.AddInt32("scale", 100);
	check(request_message(layout, hardware, saved, scale) == B_OK,
		"4K at 100%");
	check(request_mirror(layout, hardware, saved, 1, 2) == B_BAD_VALUE,
		"1080p cannot show a 4K desktop at 100%");
	expect_own(layout, 1, "refused, nothing changed");

	scale.ReplaceInt32("scale", 200);
	check(request_message(layout, hardware, saved, scale) == B_OK,
		"4K at 200%");
	check(request_mirror(layout, hardware, saved, 1, 2) == B_OK,
		"now 1080p fits");
	expect_mirror(layout, 1, 2, "1080p mirrors 4K at 200%");
	check(layout.PrimaryDisplay() != NULL
		&& layout.PrimaryDisplay()->id == 2,
		"the main display made a mirror hands that role to its source");
	check(layout.DisplayByID(1)->scale == 100
		&& layout.RenderScale() == 200,
		"the 4K one is drawn one to one, the 1080p mirror shrunk");

	// a mirror of another shape gets black bars
	FakeAccelerant wide;
	display_timing wuxga = fhd_timing();
	wuxga.v_display = 1200;
	wuxga.v_total = 1235;
	wide.Add(1, "DP-0", fhd_timing());
	wide.Add(2, "DP-2", wuxga);
	DisplayLayout shapes;
	BMessage shapesSaved;
	configure(shapes, wide, shapesSaved, false);
	check(request_mirror(shapes, wide, shapesSaved, 2, 1) == B_OK,
		"1920x1200 mirrors 1920x1080");
	expect_mirror(shapes, 2, 1, "16:10 mirrors 16:9");
	check(shapes.DisplayByID(2)->scale == 100
		&& shapes.Frame() == BRect(0, 0, 1919, 1079),
		"1:1 with bars, the desktop stays 16:9");
	check(request_mirror(shapes, wide, shapesSaved, 2, -1) == B_OK
		&& request_mirror(shapes, wide, shapesSaved, 1, 2) == B_BAD_VALUE,
		"1920x1080 cannot show 1920x1200");
}


static void
scale_pair()
{
	FakeAccelerant hardware;
	hardware.Add(1, "DP-2");
	hardware.Add(2, "DP-4");
	DisplayLayout layout;
	BMessage saved;
	configure(layout, hardware, saved, false);
	request(layout, hardware, saved, 2, 0, 0);
	for (int scale = 100; scale <= 250; scale += 25) {
		for (int id = 1; id <= 2; id++) {
			BMessage change;
			change.AddInt32("id", id);
			change.AddInt32("scale", scale);
			check(request_message(layout, hardware, saved, change) == B_OK,
				"scale change accepted");
		}
		const DisplayInfo* left = layout.DisplayByID(2);
		const DisplayInfo* right = layout.DisplayByID(1);
		check(right->frame.left == left->frame.right + 1,
			"scaling both displays preserves their adjacency");
		check(layout.Frame().IntegerWidth() + 1 == 2 * ((384000 + scale / 2) / scale),
			"scale changes do not grow an unused framebuffer gap");
	}
	BMessage moved;
	moved.AddInt32("id", 1);
	moved.AddInt32("scale", 200);
	moved.AddPoint("origin", BPoint(2500, 0));
	check(request_message(layout, hardware, saved, moved) == B_OK,
		"scale and position request accepted");
	expect(layout, 1, 2500, 0, "an explicitly requested position is retained");
}


// The workstation: two 4K monitors at 200% and a 1080p KVM at 100%. Drawn
// at 200%, the 4K monitors show their regions one to one and the KVM's is
// shrunk onto it; drawing at 100% would enlarge the 4K ones and blur them.
static void
mixed_scales()
{
	printf("mixed scales: two 4K monitors at 200%% and 1080p at 100%%\n");
	FakeAccelerant hardware;
	hardware.Add(1, "HDMI-0", fhd_timing());
	hardware.Add(2, "DP-2");
	hardware.Add(3, "DP-4");
	DisplayLayout layout;
	BMessage saved;
	configure(layout, hardware, saved, false);

	BMessage scale;
	scale.AddInt32("id", 2);
	scale.AddInt32("scale", 200);
	check(request_message(layout, hardware, saved, scale) == B_OK,
		"DP-2 at 200%");
	scale.ReplaceInt32("id", 3);
	check(request_message(layout, hardware, saved, scale) == B_OK,
		"DP-4 at 200%");
	std::vector<uint16> renderScales;
	layout.RenderScales(renderScales);
	check(renderScales.size() == 2 && renderScales[0] == 200
		&& renderScales[1] == 100, "200% is tried before 100%");
	check(layout.RenderScale() == 200
		&& layout.DisplayByID(2)->renderScale == 200
		&& layout.DisplayByID(3)->renderScale == 200
		&& layout.DisplayByID(1)->renderScale == 200,
		"everything is drawn at 200%");
	check(layout.DisplayByID(1)->frame.Width() + 1 == 1920
		&& layout.DisplayByID(2)->frame.Width() + 1 == 1920,
		"each monitor is 1920 logical pixels wide");

	// a hot plug arranges them again from the settings, at the same density
	configure(layout, hardware, saved, false);
	check(layout.RenderScale() == 200, "still 200% after a reconfiguration");

	// the engine cannot shrink onto a 4K monitor: with one at 150% the
	// density falls back to 150%, and only the 1080p one is shrunk
	scale.ReplaceInt32("id", 2);
	scale.ReplaceInt32("scale", 150);
	check(request_message(layout, hardware, saved, scale) == B_OK,
		"DP-2 at 150%");
	check(layout.RenderScale() == 150
		&& hardware.Output(1)->renderScale == 150,
		"drawn at 150%, DP-4 enlarged");

	// without the KVM, both 4K monitors at 200% again
	scale.ReplaceInt32("scale", 200);
	check(request_message(layout, hardware, saved, scale) == B_OK,
		"DP-2 back at 200%");
	hardware.SetConnected(1, false);
	configure(layout, hardware, saved, false);
	check(layout.RenderScale() == 200 && !layout.DisplayByID(1)->IsEnabled(),
		"the KVM unplugged, 200%");
	hardware.SetConnected(1, true);
	configure(layout, hardware, saved, false);
	check(layout.RenderScale() == 200 && layout.DisplayByID(1)->IsEnabled()
		&& layout.DisplayByID(1)->scale == 100,
		"the KVM plugged back in at 100%, still 200%");
}


static void
single_scaled_timing()
{
	DisplayLayout layout;
	display_timing timing = fhd_timing();
	timing.h_display = 1280;
	timing.v_display = 800;
	layout.SetSingle(BRect(0, 0, 568, 355), 225, NULL, &timing);
	check(layout.DisplayAt(0)->timing.h_display == 1280
		&& layout.DisplayAt(0)->timing.v_display == 800,
		"physical mode is retained when logical dimensions round");
	check(layout.Frame() == BRect(0, 0, 568, 355),
		"single display still uses the logical frame");
}


int
main()
{
	single_scaled_timing();
	scale_pair();
	swapped_pair(false);
	swapped_pair(true);
	row_of_three();
	column_of_three();
	mirror_pair();
	mirror_sizes();
	mixed_scales();
	printf(sFailures == 0 ? "all passed\n" : "%d FAILED\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
